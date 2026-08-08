#!/bin/sh
# Boots the UEFI loader under Bochs' software emulated VT-x and waits for the
# *nested VMX* probe's verdict rather than for the hypervisor's own.
#
# Separate from bochs-smoke.sh beside it, and the reason is a race that cost
# two runs to find. That script stops the moment it sees
# "ZPP_HYPERVISOR_ACTIVE on every cpu", which `verify::present` prints as its
# last act - and the nested probe runs *after* that, so the emulator is killed
# while the probe is still going. Two experiments with two verdicts want two
# stop conditions.
#
# Needs a build with both ZPP_VERIFY_HYPERVISOR=ON and ZPP_NESTED_VMX=ON, and
# ZPP_DIAG=OFF. The first because the probe only exists there, the second
# because with nesting off it correctly finds no VMX and passes trivially, and
# the third because with the diagnostic channel on the loader reserves space on
# the ESP and then restarts the machine, which ends the run.
#
# One more trap, which cost two runs of its own: a switch-on build in a second
# build directory still writes out/<config>, so the loader on the boot medium
# is whichever configuration was linked last. Delete the artifacts and rebuild
# the one you mean before running scripts/bochs/setup.sh.
#
# Needs no hardware virtualization, so it runs on a stock CI runner.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-gdb/bin/bochs}"
timeout_seconds="${TIMEOUT:-600}"
cpus="${CPUS:-2}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || { echo "run scripts/bochs/setup.sh first" >&2; exit 1; }

sed -e '/^gdbstub:/d' \
    -e 's/^info:.*/info: action=report/' \
    -e 's/reset_on_triple_fault=1/reset_on_triple_fault=0/' \
    -e "s/count=1/count=$cpus/" \
    "$root/scripts/bochs/bochsrc.txt" > "$work/bochsrc-ci.txt"

cd "$work"
: > serial.out
rm -f esp.img.lock

"$bochs" -q -f bochsrc-ci.txt < /dev/null > bochs.stdout 2>&1 &
bochs_pid=$!

waited=0
while kill -0 "$bochs_pid" 2>/dev/null; do
    if grep -q 'ZPP_NESTED_VMX_OK\|ZPP_NESTED_VMX_FAILED\|nested FAIL' \
        serial.out 2>/dev/null; then
        break
    fi
    if [ "$waited" -ge "$timeout_seconds" ]; then
        echo "timed out after ${timeout_seconds}s with no verdict" >&2
        break
    fi
    sleep 2
    waited=$((waited + 2))
done

# A moment for the verdict line to finish arriving before the kill. The break
# above fires on the first matching byte sequence, and the line it is part of
# may still be being written a character at a time.
sleep 2

kill "$bochs_pid" 2>/dev/null || true
sleep 1
kill -9 "$bochs_pid" 2>/dev/null || true
wait "$bochs_pid" 2>/dev/null || true
echo "bochs ran for ${waited}s"

# Deduplicated, because OVMF mirrors its console to the same serial port and
# every line the probe writes arrives twice.
echo "=== the probe's own lines ==="
grep -a 'zpp: nested\|ZPP_NESTED_VMX' serial.out | awk '!seen[$0]++' || true

if grep -q 'ZPP_NESTED_VMX_OK' serial.out 2>/dev/null; then
    echo "ok: every nested VMX step answered as the SDM says"
    exit 0
fi

echo "=== bochs log (last 40 lines) ===" >&2
tail -40 bochs.log >&2 2>/dev/null || true

echo "FAIL: the nested VMX probe did not pass" >&2
exit 1
