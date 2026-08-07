#!/bin/bash
# Builds the current tree, deploys it to the rig's real ESP and boots it.
# Prints GOOD when the guest reaches the Windows kernel, BAD otherwise.
# Exit status follows, so `git bisect run` can drive it.
set -e
RIG=${RIG:-tc@192.168.1.199}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

cmake --build --preset debug > /dev/null 2>&1 || { echo "BUILD FAILED"; exit 125; }

# Never deploy a build that destroys the machine's processor state. This
# is not advice - the switch persists in the CMake cache and a day was
# lost to it being inherited silently.
"$(dirname "$0")/check-bootable.sh" out/debug/x86_64/zpp_loader.efi \
    || { echo "SKIPPING: not a bootable build"; exit 125; }

LOCAL=$(md5 -q out/debug/x86_64/zpp_loader.efi)

cat out/debug/x86_64/zpp_loader.efi | $SSH $RIG 'cat > /tmp/newloader.efi' 2>/dev/null
REMOTE=$($SSH $RIG '
sudo pkill -9 -x qemu-system-x86_64 2>/dev/null; sleep 4
sudo umount /tmp/resp 2>/dev/null || true
sudo mount /dev/nvme0n1p2 /tmp/resp
sudo cp /tmp/newloader.efi /tmp/resp/EFI/zpp/zpp_loader.efi
sync
md5sum /tmp/resp/EFI/zpp/zpp_loader.efi | cut -d" " -f1
sudo umount /tmp/resp' 2>/dev/null | tr -d '\r')

[ "$LOCAL" = "$REMOTE" ] || { echo "DEPLOY MISMATCH local=$LOCAL remote=$REMOTE"; exit 125; }

$SSH $RIG '
cd /home/tc/vm
cp RELEASEX64_OVMF_VARS.fd.orig RELEASEX64_OVMF_VARS.fd
export ZPP_QEMU_EXTRA="-monitor telnet:0.0.0.0:4446,server,nowait"
setsid nohup sudo -E ./boot-zpp.sh > /home/tc/zpp/boot.log 2>&1 < /dev/null &
sleep 145' > /dev/null 2>&1 || true

# Windows kernel addresses are fffff8..; anything else means it never got there.
kernel=0
for i in 1 2 3; do
  rip=$({ printf "info registers\n"; sleep 1; } | nc -w 6 192.168.1.199 4446 2>/dev/null \
        | LC_ALL=C grep -aoE 'RIP=[0-9a-f]+' | head -1)
  case "$rip" in RIP=fffff8*) kernel=1 ;; esac
  sleep 8
done

$SSH $RIG 'sudo pkill -9 -x qemu-system-x86_64 2>/dev/null; true' > /dev/null 2>&1 || true

if [ "$kernel" = 1 ]; then echo "GOOD ($LOCAL)"; exit 0; fi
echo "BAD ($LOCAL)"; exit 1
