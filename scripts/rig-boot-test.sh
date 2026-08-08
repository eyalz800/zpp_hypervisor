#!/bin/bash
# Builds the current tree, deploys it to the rig's real ESP, boots it, and
# says GOOD when the guest reaches the Windows kernel or BAD when it does
# not. The exit status follows, so `git bisect run` can drive it.
#
# **Every wait in here is bounded, and that is the whole point.** The first
# version of this script had no timeout on any ssh call, so the run where
# the machine wedged did not fail - it sat in TCP retries for thirty five
# minutes, which is indistinguishable from a slow test and wasted the time
# twice over. A test that can hang is worse than no test, because it also
# stops you finding out that the target is gone.
#
# Unreachable is reported as 125, which `git bisect run` treats as "skip"
# rather than as evidence about the commit - the target being down says
# nothing about the code.
set -u

RIG_HOST=${RIG_HOST:-192.168.1.199}
RIG=${RIG:-tc@$RIG_HOST}
MONITOR_PORT=${MONITOR_PORT:-4446}

# Fail fast and never hang: give up on the connection, notice a dead
# session, and refuse to sit waiting for a password prompt that will not
# come.
#
# One line, and it has to stay one line. This is interpolated into a
# `bash -c` string further down, where a newline ends the command rather
# than separating two options - which failed as "could not copy", the one
# message that reads like the target being unreachable.
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 -o BatchMode=yes"

# shellcheck disable=SC2086
rig() { timeout "$1" ssh $SSH_OPTS "$RIG" "$2"; }

say() { printf '%s\n' "$*" >&2; }

# --- the target has to be there before anything is built or deployed ----
if ! rig 25 'echo up' 2>/dev/null | grep -q up; then
    say "SKIP: $RIG is not reachable - nothing was built or deployed."
    say "      It probably needs a power cycle; that needs someone there."
    exit 125
fi

# --- build ------------------------------------------------------------
if ! timeout 900 cmake --build --preset debug > /dev/null 2>&1; then
    say "SKIP: the build failed, so this commit cannot be judged."
    exit 125
fi

# Never deploy a build that destroys the machine's processor state. The
# switch persists in the CMake cache and a day was lost to it being
# inherited silently.
if ! "$(dirname "$0")/check-bootable.sh" out/debug/x86_64/zpp_loader.efi; then
    say "SKIP: not a bootable build."
    exit 125
fi

LOCAL=$(md5 -q out/debug/x86_64/zpp_loader.efi)

# --- deploy, and prove the bytes arrived -------------------------------
if ! timeout 240 bash -c "cat out/debug/x86_64/zpp_loader.efi \
        | ssh $SSH_OPTS $RIG 'cat > /tmp/newloader.efi'" 2>/dev/null; then
    say "SKIP: could not copy the loader to the rig."
    exit 125
fi

REMOTE=$(rig 180 '
    sudo pkill -9 -x qemu-system-x86_64 2>/dev/null; sleep 4
    sudo umount /tmp/resp 2>/dev/null || true
    sudo mkdir -p /tmp/resp
    sudo mount /dev/nvme0n1p2 /tmp/resp || exit 1
    sudo cp /tmp/newloader.efi /tmp/resp/EFI/zpp/zpp_loader.efi
    sync
    md5sum /tmp/resp/EFI/zpp/zpp_loader.efi | cut -d" " -f1
    sudo umount /tmp/resp' 2>/dev/null | tr -d '\r')

if [ "$LOCAL" != "$REMOTE" ]; then
    say "SKIP: deployed hash does not match (local $LOCAL, rig '$REMOTE')."
    say "      A stale binary under test looks exactly like a real bug."
    exit 125
fi

# --- boot -------------------------------------------------------------
rig 120 '
    cd /home/tc/vm
    cp RELEASEX64_OVMF_VARS.fd.orig RELEASEX64_OVMF_VARS.fd
    export ZPP_QEMU_EXTRA="-monitor telnet:0.0.0.0:'"$MONITOR_PORT"',server,nowait"
    setsid nohup sudo -E ./boot-zpp.sh > /home/tc/zpp/boot.log 2>&1 < /dev/null &
    sleep 2' > /dev/null 2>&1 || true

# Windows kernel addresses are fffff8..; sample until one shows up rather
# than sleeping blind for the worst case.
kernel=0
deadline=$((SECONDS + 200))

while [ "$SECONDS" -lt "$deadline" ]; do
    sleep 15

    rip=$({ printf "info registers\n"; sleep 1; } \
          | timeout 12 nc -w 8 "$RIG_HOST" "$MONITOR_PORT" 2>/dev/null \
          | LC_ALL=C grep -aoE 'RIP=[0-9a-f]+' | head -1)

    case "$rip" in
    RIP=fffff8*)
        kernel=1
        break
        ;;
    esac
done

# Leave nothing running, and do not let the teardown hang either.
rig 60 'sudo pkill -9 -x qemu-system-x86_64 2>/dev/null; true' \
    > /dev/null 2>&1 || true

if ! rig 25 'echo up' 2>/dev/null | grep -q up; then
    say "LOST: the rig stopped answering during this run ($LOCAL)."
    say "      It needs a power cycle. Reported as skip, not as a verdict."
    exit 125
fi

if [ "$kernel" = 1 ]; then
    echo "GOOD ($LOCAL)"
    exit 0
fi

echo "BAD ($LOCAL)"
exit 1
