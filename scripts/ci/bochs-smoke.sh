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
timeout_seconds="${TIMEOUT:-300}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || { echo "run scripts/bochs/setup.sh first" >&2; exit 1; }

# Unattended, so no gdb stub - Bochs would otherwise wait for a connection.
# Also report info messages rather than ignoring them: the interactive config
# suppresses everything after startup, which leaves no way to tell a slow boot
# from a wedged one. show_ips was compiled in, so this also gives a heartbeat.
sed -e 's/^gdbstub:.*/gdbstub: enabled=0/' \
    -e 's/^info:.*/info: action=report/' \
    "$root/scripts/bochs/bochsrc.txt" > "$work/bochsrc-ci.txt"

cd "$work"
: > serial.out
rm -f esp.img.lock

# Bochs never exits on its own: the firmware drops into its shell once the
# loader returns and sits there. Waiting for the timeout would spend the whole
# budget on every run, including successful ones, so poll the serial log and
# stop as soon as there is a verdict either way. The timeout stays as the
# backstop for a guest that never reports at all.
#
# stdin from /dev/null: the text config interface reads the console, and CI has
# no tty, so any prompt Bochs decides to raise would block rather than failing.
"$bochs" -q -f bochsrc-ci.txt < /dev/null > bochs.stdout 2>&1 &
bochs_pid=$!

waited=0
while kill -0 "$bochs_pid" 2>/dev/null; do
    if grep -q 'ZPP_HYPERVISOR_ACTIVE on every cpu\|ZPP_HYPERVISOR_FAILED' \
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

# TERM then KILL, in case it ignores the first.
kill "$bochs_pid" 2>/dev/null || true
sleep 1
kill -9 "$bochs_pid" 2>/dev/null || true
wait "$bochs_pid" 2>/dev/null || true
echo "bochs ran for ${waited}s"

echo "=== serial output ($(wc -c < serial.out) bytes) ==="
cat serial.out || true

# Always shown, not only on failure: if the guest made no progress this is the
# only place that says why.
echo "=== bochs log (last 60 lines of $(wc -l < bochs.log 2>/dev/null || echo 0)) ==="
tail -60 bochs.log 2>/dev/null || echo "  no bochs.log"

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
exit 1
