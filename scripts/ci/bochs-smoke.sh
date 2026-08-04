#!/bin/sh
# Boots the UEFI loader under Bochs' software emulated VT-x and asserts the
# hypervisor is genuinely live on every processor.
#
# The assertion is the hypervisor's own CPUID answer, not the presence of some
# log line: the loader queries leaf 0x40000000 after launching and prints
# ZPP_HYPERVISOR_ACTIVE only when the signature ZppZppZppZpp comes back and the
# hypervisor present bit is set in leaf 1. That cannot happen unless vmxon, the
# VMCS setup, vmlaunch, the exit handler and vmresume all worked.
#
# The other processors answer the same question for themselves. The loader is
# only ever launched on the boot processor now, so it goes on to do what an
# operating system does - enable x2APIC, then send each other processor a real
# INIT-SIPI-SIPI through the interrupt command register - and the code they
# start executes that same CPUID leaf. A processor the hypervisor adopted on
# the way through answers with the signature; one that came up beside it on
# bare metal does not.
#
# Needs no hardware virtualization, so it runs on a stock CI runner.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-gdb/bin/bochs}"
timeout_seconds="${TIMEOUT:-300}"
# Four by default rather than one: with a single processor there is nothing to
# send a start-up IPI to, so the half of the check that matters never runs.
cpus="${CPUS:-4}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || { echo "run scripts/bochs/setup.sh first" >&2; exit 1; }

# Unattended, so no gdb stub - Bochs would otherwise wait for a connection.
# Deleted rather than set to enabled=0: a Bochs built without the stub panics
# on the option being mentioned at all.
#
# This used to say that an SMP build cannot have the stub, because bochs.h
# refuses to compile the combination. The #error is real, but reading it as a
# limitation was wrong: nothing behind it is broken, and a Bochs 3.0 built
# with both was measured booting all four processors to
# ZPP_HYPERVISOR_ACTIVE with gdb attached. Removing the #error leaves three
# mechanical problems, all in gdbstub.cc: its free functions use
# BX_CPU_THIS_PTR, which is "this->" under SMP; it calls the
# uniprocessor-only global bx_cpu; and it advances the guest with cpu_loop(),
# which under SMP neither ticks the clock nor yields, so the first processor
# to spin would hold the machine forever. Pinning the shortcuts to processor
# 0 and running the same round-robin scheduler main.cc uses for SMP answers
# all three. What survives is a functional limit rather than a compile-time
# one, and it is what the #error's own comment was really about: gdb is only
# ever shown processor 0. So the line is deleted here because this script
# runs unattended, not because the two cannot coexist.
# Also report info messages rather than ignoring them: the interactive config
# suppresses everything after startup, which leaves no way to tell a slow boot
# from a wedged one. show_ips was compiled in, so this also gives a heartbeat.
#
# Triple faults panic rather than reboot. A silent reset looks exactly like a
# slow boot from out here, and it costs the whole timeout to find out
# otherwise; the panic puts a register dump in bochs.log, which is printed
# below either way.
sed -e '/^gdbstub:/d' \
    -e 's/^info:.*/info: action=report/' \
    -e 's/reset_on_triple_fault=1/reset_on_triple_fault=0/' \
    -e "s/count=1/count=$cpus/" \
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
