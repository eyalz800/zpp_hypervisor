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

# The guest-side coverage suite. Not destructive the way verify::present
# is - it restores the interrupt descriptor table, the control registers
# and the local APIC register it wrote, and it touches no other processor
# - but a build carrying it deliberately faults thirteen instructions and
# then stops instead of chainloading, so it never reaches an operating
# system. That is the same failure shape as above, and it is refused for
# the same reason: the switch persists in a CMake cache, and a boot that
# ends in a verdict looks from outside like a boot that hung.
#
# The marker is a string only the suite emits. It is checked before the
# chainload-only test below because it is unconditional - there is no
# reason to deploy one deliberately.
if LC_ALL=C grep -qa 'ZPPTEST BEGIN' "$loader"; then
    echo "REFUSING: $loader carries the guest coverage suite." >&2
    echo "" >&2
    echo "It runs the VM-exit, EPT and emulation cases and then stops," >&2
    echo "without chainloading, so this machine will not boot." >&2
    echo "" >&2
    echo "Rebuild with the switch off before deploying:" >&2
    echo "  cmake --preset debug -DZPP_GUEST_TESTS=OFF" >&2
    echo "  cmake --build --preset debug" >&2
    echo "" >&2
    echo "The switch persists in the CMake cache, so passing it once" >&2
    echo "is not enough - check it, do not assume it." >&2
    exit 1
fi

# The module protection probe, which is the same shape again: not
# destructive - it reads a byte of this module's text and writes the same
# byte back - but the launch ends in a verdict rather than a guest, so the
# machine never reaches an operating system. The marker is a log line only
# the probe emits, and the hypervisor ELF carrying it is embedded in the
# loader.
if LC_ALL=C grep -qa 'module protection probe: storing to text' \
    "$loader"; then
    echo "REFUSING: $loader carries the module protection probe." >&2
    echo "" >&2
    echo "It stores into the hypervisor's own text to prove the store" >&2
    echo "faults, and reports the verdict as an error code instead of" >&2
    echo "launching a guest, so this machine will not boot." >&2
    echo "" >&2
    echo "Rebuild with the switch off before deploying:" >&2
    echo "  cmake --preset debug -DZPP_TEST_MODULE_PROTECTION=OFF" >&2
    echo "  cmake --build --preset debug" >&2
    echo "" >&2
    echo "The switch persists in the CMake cache, so passing it once" >&2
    echo "is not enough - check it, do not assume it." >&2
    exit 1
fi

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
    if LC_ALL=C grep -qa 'ZPP_LOADER chainload only' "$loader"; then
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

# The switch that wastes time rather than machines, and the reason this
# section exists at all.
#
# ZPP_NESTED_VMX off hides VMX from the guest. Windows then boots, Hyper-V
# finds no VMX and stands down cleanly, and **nothing about the boot looks
# wrong** - which is worse than a hang, because a hang is investigated. An
# afternoon went into a run read as a milestone that was a build with
# nested off: a clean reconfigure had dropped the cached ON, and the
# screen showed Windows coming up exactly as it would have with it on.
#
# The default is ON now, so this cannot be arrived at by losing a cache
# entry. This is the second line of defence, at the only point that
# matters - before the bytes reach the machine - because the first line
# is a default and defaults are what clean reconfigures reset.
if [ "${ZPP_ALLOW_NO_NESTED:-0}" != "1" ]; then
    if ! LC_ALL=C grep -qa 'nested vmx: reporting vmx to the guest' \
        "$loader"; then
        echo "REFUSING: $loader was built without ZPP_NESTED_VMX." >&2
        echo "" >&2
        echo "VMX is hidden from the guest, so Hyper-V will find none and" >&2
        echo "stand down. Windows boots and the screen looks right - which" >&2
        echo "is why this is refused rather than warned about." >&2
        echo "" >&2
        echo "Rebuild with it on before deploying:" >&2
        echo "  cmake --preset debug -DZPP_NESTED_VMX=ON" >&2
        echo "  cmake --build --preset debug" >&2
        echo "" >&2
        echo "It is the default. If it is off, something turned it off -" >&2
        echo "most likely a clean reconfigure that dropped the cache." >&2
        echo "" >&2
        echo "To deploy one deliberately - asking 'is this failure nested" >&2
        echo "at all' is a fair question, and one boot answers it:" >&2
        echo "  ZPP_ALLOW_NO_NESTED=1 $0 $loader" >&2
        exit 1
    fi
fi

# The same class again: without the loader trace there is no module base
# on serial, and without a module base nothing in scripts/rig-dump-state.py
# or rig-dump-log-physical.sh can read a single member. The boot still
# happens and still tells you nothing, which costs a boot to discover.
#
# ZPP_TRACE defaults to ZPP_VERIFY_HYPERVISOR, which is off - so unlike
# nested VMX this one is off unless asked for, and a clean reconfigure
# silently produces a blind loader.
if [ "${ZPP_ALLOW_NO_TRACE:-0}" != "1" ]; then
    if ! LC_ALL=C grep -qa 'ZPP_TRACE' "$loader"; then
        echo "REFUSING: $loader was built without ZPP_TRACE." >&2
        echo "" >&2
        echo "The loader prints its module base on serial and nothing" >&2
        echo "else does. Without it every diagnostic that reads a member" >&2
        echo "by offset is blind, and the boot has to be repeated." >&2
        echo "" >&2
        echo "Rebuild with it on before deploying:" >&2
        echo "  cmake --preset debug -DZPP_TRACE=ON" >&2
        echo "  cmake --build --preset debug" >&2
        echo "" >&2
        echo "To deploy one deliberately:" >&2
        echo "  ZPP_ALLOW_NO_TRACE=1 $0 $loader" >&2
        exit 1
    fi
fi

# The same question asked the other way round, because the check above
# fails open and this one fails closed.
#
# It grepped for a string nothing emitted - every marker identifying a
# chainload-only build carried the `ZPP_TRACE` prefix and disappeared
# with the trace compiled out - so the guard had never fired once. An
# absence test cannot notice that: no match reads as "not a control
# build" whether the marker is absent or merely misspelled.
#
# So require the resident marker to be *present*. Both are emitted with
# trace::raw and survive ZPP_TRACE being off. A loader that is neither
# is a loader nobody has classified, and that is the state this whole
# section exists to refuse.
if [ "${ZPP_ALLOW_CHAINLOAD_ONLY:-0}" != "1" ]; then
    if ! LC_ALL=C grep -qa 'ZPP_LOADER resident' "$loader"; then
        echo "REFUSING: $loader carries no resident-loader marker." >&2
        echo "" >&2
        echo "A loader that launches the hypervisor says so in its own" >&2
        echo "bytes, unconditionally. This one says neither that nor" >&2
        echo "that it is chainload-only, so what it does is unknown -" >&2
        echo "and the failure this guards against is a control build" >&2
        echo "reaching the disk and every later run silently becoming" >&2
        echo "a control run." >&2
        echo "" >&2
        echo "If the marker was renamed, update this check with it." >&2
        exit 1
    fi
fi

# Says what was actually established rather than what the checks are
# named after: an escape hatch that was used must not be reported as a
# property that holds.
# Read from the loader's own marker rather than from which escape
# hatch was set, so a deliberate control build cannot be summarised as
# one that launches the hypervisor - which is the exact sentence that
# would let it be deployed and believed.
if LC_ALL=C grep -qa 'ZPP_LOADER chainload only' "$loader"; then
    summary="ok: $loader is a CONTROL loader - it chainloads the boot"
    summary="$summary manager and does NOT launch the hypervisor"
else
    summary="ok: $loader launches the hypervisor and carries no"
    summary="$summary destructive self check"
fi
[ "${ZPP_ALLOW_NO_NESTED:-0}" = "1" ] &&
    summary="$summary; nested VMX NOT checked"
[ "${ZPP_ALLOW_NO_TRACE:-0}" = "1" ] &&
    summary="$summary; serial trace NOT checked"
[ "${ZPP_ALLOW_CHAINLOAD_ONLY:-0}" = "1" ] &&
    summary="$summary; chainload-only NOT checked"
echo "$summary"

# What the compiler actually saw, printed on every deploy.
#
# **A CMake cache reading ON is not evidence.** ZPP_PUBLISH_REFERENCE_TSC
# was ON in both caches and in compile_commands.json while the object
# file consuming it was stale, so the function it controls linked in as
# a bare `ret` - and two sessions of measurements were taken against a
# configuration nobody had built, of the switch aimed at the largest exit
# reason on the machine. `zpp_build_switches` is assembled from the same
# constexpr bools the code branches on, so this line is the binary's own
# account of itself. Printed rather than checked, because which switches
# *should* be on is the caller's question and differs per experiment -
# the failure this prevents is not noticing that they are not.
switches=$(LC_ALL=C strings "$loader" 2>/dev/null |
    LC_ALL=C grep -a 'zpp switches:' | head -1)
if [ -n "$switches" ]; then
    echo "    $switches"
else
    echo "    WARNING: no switch manifest in $loader - it predates" >&2
    echo "             build_switches.cpp, so what is compiled into it" >&2
    echo "             cannot be read back. Rebuild before trusting a" >&2
    echo "             measurement taken with it." >&2
fi
