#!/bin/sh
# Refuses a loader that must not be booted on a machine you want to keep.
#
# ZPP_VERIFY_HYPERVISOR compiles in verify::present, which is deliberately
# destructive: it takes every application processor the firmware had parked
# and leaves it halted in real mode, and leaves the boot processor's local
# APIC in x2APIC mode. A firmware that then waits for those processors -
# EDK2's WaitApWakeup does exactly that - spins for ever, and the machine
# never reaches the operating system.
#
# That is documented in verify.h, warned about in CMakeLists.txt, and it
# still cost a full day: the switch persisted in a CMake cache, was
# inherited silently by every later build, and the resulting hang was
# indistinguishable from a hypervisor bug. Seven candidate causes were
# bisected and eliminated before the build option was suspected.
#
# So it is checked mechanically, at the only point that matters: before the
# bytes reach a real machine.
set -e

loader=${1:-out/debug/x86_64/zpp_loader.efi}

if [ ! -f "$loader" ]; then
    echo "check-bootable: no such loader: $loader" >&2
    exit 2
fi

# Strings only verify::present emits. Matching any of them means the
# destructive self check is compiled in.
for marker in 'hypervisor_bit=' 'leaf 0x40000000 ebx=' 'vmm exits '; do
    if LC_ALL=C grep -qa "$marker" "$loader"; then
        echo "REFUSING: $loader carries verify::present (found '$marker')." >&2
        echo "" >&2
        echo "It is destructive by design and will hang a real boot:" >&2
        echo "the firmware's application processors are left halted in" >&2
        echo "real mode and its own wait for them never completes." >&2
        echo "" >&2
        echo "Rebuild with the switch off before deploying:" >&2
        echo "  cmake --preset debug -DZPP_VERIFY_HYPERVISOR=OFF" >&2
        echo "  cmake --build --preset debug" >&2
        echo "" >&2
        echo "The switch persists in the CMake cache, so passing it once" >&2
        echo "is not enough - check it, do not assume it." >&2
        exit 1
    fi
done

# The other switch that persists in the cache, and the one that is worse
# to be wrong about, because nothing about the boot looks wrong.
#
# ZPP_CHAINLOAD_ONLY starts the boot manager without launching the
# hypervisor. It exists to answer "does this machine boot at all without
# us", which is a question worth being able to ask - but a build carrying
# it boots perfectly, reaches the kernel, and produces no diagnostics
# whatsoever, because there is no resident side to produce them.
#
# Measured cost of not checking: a run reported a Windows kernel twice
# over and an empty log channel, and the empty channel was read as the
# channel being broken. It was not. The hypervisor had never started.
#
# Refused by default rather than warned about, since every deployment
# this script guards wants the hypervisor. Set ZPP_ALLOW_CHAINLOAD_ONLY=1
# to deploy one deliberately.
if [ "${ZPP_ALLOW_CHAINLOAD_ONLY:-0}" != "1" ]; then
    if LC_ALL=C grep -qa 'chainload only, hypervisor not launched' \
        "$loader"; then
        echo "REFUSING: $loader is a chainload-only build." >&2
        echo "" >&2
        echo "It boots the guest without launching the hypervisor, so it" >&2
        echo "reaches the operating system and logs nothing - which reads" >&2
        echo "as a working boot and a broken log channel." >&2
        echo "" >&2
        echo "Rebuild with the switch off before deploying:" >&2
        echo "  cmake --preset debug -DZPP_CHAINLOAD_ONLY=OFF" >&2
        echo "  cmake --build --preset debug" >&2
        echo "" >&2
        echo "The switch persists in the CMake cache, so passing it once" >&2
        echo "is not enough - check it, do not assume it." >&2
        echo "" >&2
        echo "To deploy one deliberately:" >&2
        echo "  ZPP_ALLOW_CHAINLOAD_ONLY=1 $0 $loader" >&2
        exit 1
    fi
fi

echo "ok: $loader launches the hypervisor and carries no destructive"\
     "self check"
