#!/bin/bash
# Boots the rig's guest with every diagnostic channel open, and refuses to
# report success until all three of them answer.
#
# **This is the only way a boot should be started.** Booting by hand -
# `ssh rig 'cd vm && sudo ./boot-zpp.sh'` - opens no monitor and no gdb
# stub, and the cost of noticing that is a whole boot: the failure being
# chased is over by the time anyone wants to look at it, and the machine
# then has to be killed and re-run to get a channel that should have been
# there from the start. Every boot here costs minutes and risks the real
# disk, so a run that cannot be inspected is a run wasted.
#
# The three channels, and why each is not optional:
#
# - **Serial** is the loader's only voice. `zpp:` on serial is the one
#   proof that the firmware booted *us* rather than Windows directly, and
#   a run without that proof produces real numbers describing a machine
#   this hypervisor was never on.
# - **The monitor** reads guest state without perturbing it - `info
#   registers -a`, `xp`, `info status`. It is the only way to read the
#   resident module's counters while the guest runs, since gdb reads
#   through the current CPU's page tables and our module is not mapped in
#   the guest's CR3.
# - **The gdb stub** is what makes symbols usable. QEMU can open one on an
#   already running guest from the monitor, but only if the monitor is
#   there - and a guest that has wedged hard enough to stop answering the
#   monitor cannot be given a stub afterwards at all.
#
# None of the three costs anything while unused: a listening socket is a
# listening socket, and `-gdb` without `-S` does not pause the machine.
# There is therefore no configuration in which leaving one out is the
# better trade, which is why this script does not offer the choice.
set -u

RIG_HOST=${RIG_HOST:-192.168.1.199}
RIG=${RIG:-tc@$RIG_HOST}
MONITOR_PORT=${MONITOR_PORT:-4446}
GDB_PORT=${GDB_PORT:-1234}

# Extra QEMU arguments a caller wants *on top of* the channels, rather
# than instead of them. The launcher appends $ZPP_QEMU_EXTRA verbatim, so
# this is composed here and the caller's own value is never the whole of
# it.
EXTRA=${ZPP_QEMU_EXTRA:-}

# How many processors the guest gets, passed through to the launcher.
#
# **Empty means eight, not one.** The launcher's own default is the host's
# count, and nothing here used to forward this at all - so a run started
# to investigate a single-processor boot got eight, and every per-processor
# counter read afterwards was processor zero of eight. That cost an entire
# investigation: seven processors sat in the firmware's wait loop and the
# state that said so was never read, because nobody knew there were seven.
CPUS=${ZPP_CPUS:-}

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 -o BatchMode=yes"

# shellcheck disable=SC2086
rig() { timeout "$1" ssh $SSH_OPTS "$RIG" "$2"; }

say() { printf '%s\n' "$*" >&2; }

if ! rig 25 'echo up' 2>/dev/null | grep -q up; then
    say "FAIL: $RIG is not reachable."
    exit 125
fi

# Nothing is booted over a guest that is already running. The devices are
# passed through, so a second QEMU cannot claim them and the failure it
# produces names the devices rather than the cause.
#
# Matched as `qemu*` and **not** as the full name. `/proc/<pid>/comm` is
# truncated to fifteen characters, so `qemu-system-x86_64` reads back as
# `qemu-system-x86` and an exact match never fires - which is worse than
# no check at all, since it reports the machine idle while a guest holds
# every passed-through device. `pkill -x` compares against the same
# truncated name and misses for the same reason. rig-kill-qemu.sh has
# always matched by prefix; this is why.
if rig 25 'ls /proc/*/comm 2>/dev/null | while read -r c; do
        grep -q "^qemu" "$c" 2>/dev/null && echo running
    done' 2>/dev/null | grep -q running; then
    say "FAIL: a guest is already running. Kill it with rig-kill-qemu.sh"
    say "      first - by PID and by process name, never by command line."
    exit 1
fi

CHANNELS="-monitor telnet:0.0.0.0:$MONITOR_PORT,server,nowait"
CHANNELS="$CHANNELS -gdb tcp:0.0.0.0:$GDB_PORT"

say "booting with monitor :$MONITOR_PORT, gdb :$GDB_PORT, serial to"
say "/home/tc/zpp/serial.out, cpus ${CPUS:-<launcher default>}"

# `setsid nohup` because QEMU must outlive the ssh session, and `sudo -E`
# because the launcher reads ZPP_QEMU_EXTRA from the environment. Without
# the sudo the launcher walks past every device step printing permission
# denials and then tears itself down, which looks exactly like a guest
# that booted and died.
rig 120 "
    cd /home/tc/vm
    export ZPP_QEMU_EXTRA='$CHANNELS $EXTRA'
    ${CPUS:+export ZPP_CPUS=$CPUS}
    setsid nohup sudo -E ./boot-zpp.sh > /home/tc/zpp/boot.log 2>&1 < /dev/null &
    sleep 3" > /dev/null 2>&1 || true

# --- and now prove all three are actually there ------------------------
#
# Asked rather than assumed. A launcher that died on its own teardown
# leaves no QEMU and no error naming the cause, and every check below then
# fails in a way that says which piece is missing.
ok=1

if ! rig 30 'ls /proc/*/comm 2>/dev/null | while read -r c; do
        grep -q "^qemu" "$c" 2>/dev/null && echo running
    done' 2>/dev/null | grep -q running; then
    say "FAIL: no qemu-system-x86_64 process. Read /home/tc/zpp/boot.log -"
    say "      a missing sudo prints permission denials and exits quietly."
    exit 1
fi

if ! rig 30 "printf 'info status\n' | nc -w 5 127.0.0.1 $MONITOR_PORT" \
        2>/dev/null | grep -q 'VM status'; then
    say "FAIL: the monitor on :$MONITOR_PORT does not answer."
    ok=0
fi

# Checked by looking at the listening socket, **never by connecting to
# it**. A TCP connection to the gdb stub *is* a debugger attaching, and
# QEMU stops the virtual machine the moment one arrives - so `nc -z`,
# which exists precisely to open and immediately close a connection,
# silently paused the guest mid-firmware and left it there. The symptom
# was a boot whose serial stopped growing at the graphics option ROM,
# which reads exactly like a hang in that option ROM; `info status`
# answering `paused` rather than `running` is what distinguishes them,
# and it is worth asking before believing any hang on this machine.
if ! rig 30 "netstat -ln 2>/dev/null | grep -c ':$GDB_PORT '" 2>/dev/null \
        | grep -qE '^[1-9]'; then
    say "FAIL: the gdb stub on :$GDB_PORT is not listening."
    ok=0
fi

# Serial last, since it is the one that takes time to say anything: the
# firmware runs before the loader does.
#
# Four minutes, not ninety seconds. The firmware runs the graphics option
# ROM before it reaches any boot option, and on this machine that alone
# takes over two minutes - a shorter deadline reports a perfectly healthy
# boot as a failure, which then invites killing it and starting again.
serial=0
deadline=$((SECONDS + 240))

while [ "$SECONDS" -lt "$deadline" ]; do
    sleep 5
    if rig 20 'grep -ac "zpp:" /home/tc/zpp/serial.out' 2>/dev/null \
            | grep -qE '^[1-9]'; then
        serial=1
        break
    fi
done

if [ "$serial" -eq 0 ]; then
    say "FAIL: no 'zpp:' line on serial within 240s. The firmware booted"
    say "      something other than the loader, so this run has no"
    say "      hypervisor in it. Check the guest's boot option with"
    say "      rig-one-boot-option.sh before debugging anything else."
    ok=0
fi

if [ "$ok" -eq 0 ]; then
    exit 1
fi

# Reported rather than assumed, and always, because assuming it is what
# cost the investigation described above `CPUS`. Every per-processor
# reading afterwards has to be taken over this many processors.
seen=$(rig 30 "printf 'info cpus\n' | nc -w 5 127.0.0.1 $MONITOR_PORT" \
    2>/dev/null | LC_ALL=C tr -d '\r' | grep -ac 'CPU #')

say "ok: monitor, gdb stub and serial all answering. guest has $seen cpus."
