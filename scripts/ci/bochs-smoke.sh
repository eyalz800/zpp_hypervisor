#!/bin/sh
# Boots the UEFI loader under Bochs' software emulated VT-x and asserts the
# hypervisor is genuinely live.
#
# The assertion is the hypervisor's own CPUID answer, not the presence of some
# log line: the loader queries leaf 0x40000000 after launching and prints
# ZPP_HYPERVISOR_ACTIVE only when the signature ZppZppZppZpp comes back and the
# hypervisor present bit is set in leaf 1. That cannot happen unless vmxon, the
# VMCS setup, vmlaunch, the exit handler and vmresume all worked.
#
# Needs no hardware virtualization, so it runs on a stock CI runner.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-gdb/bin/bochs}"
timeout_seconds="${TIMEOUT:-600}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || { echo "run scripts/bochs/setup.sh first" >&2; exit 1; }

# Unattended, so no gdb stub - Bochs would otherwise wait for a connection.
sed 's/^gdbstub:.*/gdbstub: enabled=0/' \
    "$root/scripts/bochs/bochsrc.txt" > "$work/bochsrc-ci.txt"

cd "$work"
: > serial.out
# stdin from /dev/null: the text config interface reads the console, and CI has
# no tty, so any prompt Bochs decides to raise would block until the timeout
# rather than failing. SIGKILL after the grace period in case it ignores TERM.
timeout --kill-after=30s "$timeout_seconds" \
    "$bochs" -q -f bochsrc-ci.txt < /dev/null || true

echo "=== serial output ($(wc -c < serial.out) bytes) ==="
cat serial.out || true

if grep -q "ZPP_HYPERVISOR_ACTIVE on every cpu" serial.out 2>/dev/null; then
    echo "ok: every cpu answered CPUID 0x40000000 with the zpp signature"
    exit 0
fi

if grep -q 'ZPP_HYPERVISOR_FAILED' serial.out 2>/dev/null; then
    echo "FAIL: loader ran but the hypervisor did not identify itself" >&2
    grep 'ZPP_HYPERVISOR_FAILED' serial.out >&2
    exit 1
fi

echo "FAIL: loader never reported - it did not reach the check" >&2
echo "=== bochs log tail ===" >&2
tail -40 bochs.log >&2 || true
exit 1
