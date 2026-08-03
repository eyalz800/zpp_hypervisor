#!/bin/sh
# Boots the UEFI loader under Bochs' software emulated VT-x and asserts the
# hypervisor gets far enough to matter. Needs no hardware virtualization, so
# it runs on a stock CI runner.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-gdb/bin/bochs}"
timeout_seconds="${TIMEOUT:-600}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || { echo "run setup first" >&2; exit 1; }

# Unattended: no gdb stub, or Bochs would sit waiting for a connection.
sed 's/^gdbstub:.*/gdbstub: enabled=0/' \
    "$root/scripts/bochs/bochsrc.txt" > "$work/bochsrc-ci.txt"

cd "$work"
: > serial.out
timeout "$timeout_seconds" "$bochs" -q -f bochsrc-ci.txt || true

echo "=== serial output ==="
cat serial.out || true
echo "=== bochs log tail ==="
tail -40 bochs.log || true

# A VMX capable guest is the whole point of using Bochs, so a guest that
# never saw VMX means the emulator or the CPU model is wrong.
if grep -qiE 'vmxon|VMX|root mode' serial.out bochs.log 2>/dev/null; then
    echo "ok: VMX activity observed"
else
    echo "FAIL: no VMX activity in serial output or bochs log" >&2
    exit 1
fi
