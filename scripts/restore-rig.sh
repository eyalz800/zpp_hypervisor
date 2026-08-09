#!/bin/sh
# Put the rig back the way a test run needs it, after a reboot has undone
# it.
#
# The rig restores /home from mydata.tgz on every boot and cannot write a
# new backup while its TinyCore boot medium is absent - which it is, since
# the release upgrade - so everything a session generates is gone after a
# reset. Worse, what comes back is not missing but *older*: the boot
# scripts revert to a form that uses a QEMU which no longer works, and the
# variable store reverts to one whose boot order picks Windows directly,
# so the hypervisor is never even loaded. Neither looks like a broken rig.
# They look like a broken hypervisor.
#
# So this states the whole configuration rather than patching whatever is
# there, and prints what it verified.
#
#   ./scripts/restore-rig.sh
#
# Then deploy and boot as usual:
#
#   ./scripts/deploy-to-rig.sh
#   ssh <rig> 'cd vm && sudo ./boot-ipi.sh'
set -e

TARGET=${ZPP_TARGET:-tc@192.168.1.199}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $TARGET"
HERE=$(dirname "$0")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The firmware's code half, which lives in the QEMU installation on the
# rig and no longer exists there. Taken from this machine's QEMU instead,
# where it pairs with the 540672 byte variable store the rig keeps.
FIRMWARE=${ZPP_OVMF_CODE:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}

# The passed through disk, as the guest's firmware sees it. The boot
# option has to name the partition exactly, because agreeing with what the
# firmware would have chosen by itself is not evidence of choosing right -
# a recovery partition carrying its own boot manager answers the same
# test.
PARTITION=${ZPP_PARTITION:-2}
FIRST_LBA=${ZPP_FIRST_LBA:-32768}
SECTORS=${ZPP_SECTORS:-33554432}
PARTITION_GUID=${ZPP_PARTITION_GUID:-2CC5F47B-1672-4BEB-9C70-FF8869A75262}

# Backslashes, because this is a UEFI device path and not a mount relative
# one. A forward slash here is not merely unconventional: the firmware
# prints its own separator between device path nodes, so a leading '/'
# produced `HD(...)//EFI/zpp/zpp_loader.efi` and the option failed to load
# with "Not Found" - after which the firmware fell through to booting the
# disk directly and the hypervisor never ran at all.
LOADER_PATH=${ZPP_LOADER_PATH:-'\EFI\zpp\zpp_loader.efi'}

echo "== variable store: one boot option, naming our loader =="

# From the pristine copy the rig keeps rather than from whatever is in
# place, which is usually the thing being undone.
$SSH 'cat /home/tc/vm/RELEASEX64_OVMF_VARS.fd.pristine' > "$WORK/pristine.fd"

python3 "$HERE/uefi-varstore.py" "$WORK/pristine.fd" \
    --only-boot-option "$LOADER_PATH" \
    --description 'zpp hypervisor' \
    --partition "$PARTITION" \
    --first-lba "$FIRST_LBA" \
    --sectors "$SECTORS" \
    --partition-guid "$PARTITION_GUID" \
    --timeout 0 \
    --output "$WORK/vars.fd" | sed -n '/Boot0000/p;/BootOrder/p'

$SSH 'cat > /home/tc/vm/RELEASEX64_OVMF_VARS.fd' < "$WORK/vars.fd"

echo "== firmware: supplied from here, since the rig has none =="

if [ ! -f "$FIRMWARE" ]; then
    echo "FAIL: no firmware at $FIRMWARE"
    echo "      set ZPP_OVMF_CODE to an OVMF X64 code image"
    exit 1
fi

$SSH 'cat > /home/tc/vm/edk2-x86_64-code.fd' < "$FIRMWARE"

WANT=$(md5 -q "$FIRMWARE" 2>/dev/null || md5sum "$FIRMWARE" | cut -d' ' -f1)
GOT=$($SSH 'md5sum /home/tc/vm/edk2-x86_64-code.fd' | cut -d' ' -f1)
if [ "$WANT" != "$GOT" ]; then
    echo "FAIL: firmware did not copy intact ($WANT vs $GOT)"
    exit 1
fi
echo "  ok $GOT"

echo "== trace arming =="

# Unfiltered. The trace is drained live to the development machine rather
# than kept here, so there is no ring to size and nothing to wrap - and a
# filter that fails to parse leaves the event in an error state that
# silently drops *everything*, which reads exactly like "the guest never
# did it".
$SSH 'cat > /home/tc/vm/arm-ipi-trace.sh && chmod +x /home/tc/vm/arm-ipi-trace.sh' <<'ARM'
#!/bin/sh
T=/sys/kernel/tracing

# tracefs is not mounted on every boot of this machine, and when it is
# not, $T exists as an empty directory - so arming writes to files that
# are not there, reports nothing, and the trace comes back empty. Which
# reads exactly like "the guest never did it", and did once.
[ -d $T/events ] || mount -t tracefs nodev $T 2>/dev/null

for e in $T/events/kvm/*/enable; do echo 0 > $e 2>/dev/null; done
for e in $T/events/kvm/*/filter; do echo 0 > $e 2>/dev/null; done

# Per CPU, so this is eight times what it says. Sizing it to hold a whole
# Windows boot starved the guest badly enough that the OOM killer took
# QEMU; the live drain is what makes a small ring sufficient.
echo 4096 > $T/buffer_size_kb 2>/dev/null

for e in kvm_apic_ipi kvm_apic_accept_irq kvm_apic kvm_nested_vmenter_failed \
         kvm_inj_exception kvm_mmio; do
  echo 1 > $T/events/kvm/$e/enable 2>/dev/null
done

echo > $T/trace 2>/dev/null
echo 1 > $T/tracing_on 2>/dev/null
echo "armed: ipi=$(cat $T/events/kvm/kvm_apic_ipi/enable) accept=$(cat $T/events/kvm/kvm_apic_accept_irq/enable) events=$(ls $T/events/kvm 2>/dev/null | wc -l)"
ARM
echo "  ok"

echo "== boot scripts: standalone qemu, traced kvm, no hv-passthrough =="

# Every one of these is a thing the restored copy gets wrong:
#
#   - the QEMU in the TinyCore extensions is 9.0.0, does not start after a
#     reboot, and has no tracepoints,
#   - its firmware directory does not exist, so -L and the pflash path
#     both have to point somewhere that does,
#   - the stock kvm.ko has no tracing and no Hyper-V support,
#   - hv-passthrough hands the guest the host's own enlightenments from
#     underneath us, which is the opposite of what a test of *our* answers
#     wants,
#   - QEMU 11 wants aw-bits stated on the emulated IOMMU.
$SSH 'cd /home/tc/vm
sed -i \
  -e "s|^qemu-system-x86_64 \\\\|/home/tc/vm/qemu-system-x86_64-new -L /home/tc/vm \\\\|" \
  -e "s|file=/usr/local/share/qemu/edk2-x86_64-code.fd|file=/home/tc/vm/edk2-x86_64-code.fd|" \
  -e "s|-L /usr/local/share/qemu|-L /home/tc/vm|" \
  -e "s|intel-iommu,intremap=off,caching-mode=on,device-iotlb=on|intel-iommu,intremap=off,caching-mode=on,device-iotlb=on,aw-bits=39|" \
  -e "s|insmod ./kvm.ko|insmod ./kvm-trace.ko|" \
  -e "s|insmod ./kvm-intel.ko|insmod ./kvm-intel-trace.ko|" \
  -e "s|-cpu host,kvm=on,hv-passthrough,topoext|-cpu host,kvm=on,topoext|" \
  boot-zpp.sh

# The traced variant arms the tracepoints after the KVM modules reload,
# which is the only moment that works: the launcher rmmods and insmods
# them, and that destroys events/kvm and clears every enable.
awk "{print} /insmod .\/kvm-intel-trace.ko/ {print \"/home/tc/vm/arm-ipi-trace.sh || true\"}" \
    boot-zpp.sh > boot-ipi.sh
chmod +x boot-ipi.sh'

echo "== verifying what is actually in the boot script =="

$SSH 'cd /home/tc/vm && grep -n "qemu-system-x86_64-new\|edk2-x86_64-code\|insmod\|-cpu host\|aw-bits\|intremap" boot-ipi.sh'

MISSING=$($SSH 'cd /home/tc/vm
for want in "qemu-system-x86_64-new" "file=/home/tc/vm/edk2-x86_64-code.fd" \
            "kvm-trace.ko" "kvm-intel-trace.ko" "aw-bits=39" "intremap=off"; do
    grep -q -- "$want" boot-ipi.sh || echo "$want"
done
grep -q -- "hv-passthrough" boot-ipi.sh && echo "hv-passthrough still present"
true')

if [ -n "$MISSING" ]; then
    echo "FAIL: boot-ipi.sh is not what it should be:"
    echo "$MISSING" | sed 's/^/      /'
    exit 1
fi

echo
echo "OK: rig restored. Deploy and boot:"
echo "    ./scripts/deploy-to-rig.sh"
echo "    ssh $TARGET 'cd vm && sudo ./boot-ipi.sh'"
