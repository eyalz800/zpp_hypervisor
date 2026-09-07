#!/bin/sh
# Capture the MATCHED HEALTHY CONTROL, in one pass, on a live guest.
#
# **Why this exists as a script rather than as a sequence of commands.**
# Boot 262 read healthy at fourteen minutes and was killed one minute
# later, because the kill sat in the same command block as the triage and
# ran unconditionally. A healthy draw is roughly one boot in five and
# about fifteen minutes of wall clock, and that one was destroyed before
# a single control reading was taken off it. Everything below was wanted
# at that moment and none of it was ready to run.
#
# Three BACKLOG entries are explicitly blocked on the readings here:
#
#   - `HalpClockTickLogIndex` rate. The stalled boot reads 1,666 ISR
#     entries/s against 1,059/s quoted from ANOTHER SESSION'S boot, whose
#     processor count is not even stated. Until that comparison is made
#     against a healthy boot on this rig, "1.57x" compares against a
#     number from elsewhere.
#   - the cpu0/cpu1 injection asymmetry (1,021/s against 574.6/s). A busy
#     boot processor beside an idle application processor may look exactly
#     like this when everything is fine, and nothing so far distinguishes
#     "wedged" from "busy".
#   - `ExpUpdateTimerConfigurationWorker` at 34.6% of cpu0's census and
#     absent from cpu1's. Same question: is that the stall or is that
#     Tuesday?
#
# It takes NO decisions and kills NOTHING. It reads and writes files.
#
# Usage:  scripts/rig-healthy-control.sh <tag>
set -eu

TAG="${1:-control}"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="/tmp/healthy-$TAG"
RIG=tc@192.168.1.199
ELF=.rig-deployed-hypervisor.elf
mkdir -p "$OUT"

clear_nc() { ssh -o ConnectTimeout=8 "$RIG" 'pkill -x nc' 2>/dev/null || true; sleep 2; }

echo "=== 0. residency and status (a boot without the marker is bare Windows) ==="
ssh -o ConnectTimeout=8 "$RIG" 'grep -c "allocate_rwx done" ~/zpp/serial.out' \
    2>/dev/null | tail -1 | sed 's/^/allocate_rwx markers: /'
clear_nc
printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null | grep -ai "VM status" || true

MODBASE=$(ssh -o ConnectTimeout=8 "$RIG" \
    'grep -ao "allocate_rwx done at 0x[0-9a-f]*" ~/zpp/serial.out | tail -1' \
    2>/dev/null | grep -o '0x[0-9a-f]*')
echo "module base $MODBASE"

echo "=== 1. windowed triage (the verdict itself, at the calibrated age) ==="
clear_nc
timeout 900 python3 "$HERE/rig-dump-state.py" --elf "$ELF" --cpus 2 --delta 60 \
    > "$OUT/delta.txt" 2>&1 || true
grep -E "^  vmcall |vtl_fresh_calls" "$OUT/delta.txt" | head -4

echo "=== 2. cumulative dump: censuses, stacks, cycle split ==="
clear_nc
timeout 1500 python3 "$HERE/rig-dump-state.py" --elf "$ELF" --cpus 2 \
    > "$OUT/full.txt" 2>&1 || true
echo "  -> $OUT/full.txt ($(wc -l < "$OUT/full.txt") lines)"
sed -n '/^cpu 0 second-level call stack - where it is now/,/^$/p' "$OUT/full.txt" | head -12

echo "=== 3. injection census, both processors (the asymmetry control) ==="
clear_nc
timeout 900 python3 "$HERE/guest-l2-vectors.py" --elf "$ELF" --cpus 2 --top 6 \
    --base "$MODBASE" > "$OUT/vectors-1.txt" 2>&1 || true
grep -E "l2_injected_vector cpu|vector 0x" "$OUT/vectors-1.txt" | head -16

# The guest roots. `guest_kernel_base` is ungated; `l2_exit_cr3` is NOT -
# it sits behind census_exits and reads zero on a throughput build. Any
# process CR3 maps the kernel half, and the user-mode census prints one.
KB=$(grep -oE "kernel base 0x[0-9a-f]+" "$OUT/full.txt" | head -1 | grep -o '0x[0-9a-f]*' || true)
CR3=$(grep -oE "cr3 0x[0-9a-f]+" "$OUT/full.txt" | head -1 | grep -o '0x[0-9a-f]*' || true)
echo "kernel base $KB   cr3 $CR3"

if [ -n "$KB" ] && [ -n "$CR3" ]; then
    echo "=== 4. validate the root BEFORE reading through it (expect MZ) ==="
    clear_nc
    timeout 300 python3 "$HERE/guest-walk.py" "$CR3" "$KB" 2>&1 | tail -2

    echo "=== 5. HalpClockTickLogIndex, paired 60 s apart, with injections ==="
    VA=$(python3 -c "print(hex(int('$KB',16)+0xe0a838))")
    clear_nc
    echo "--- t0 ---"; date +%H:%M:%S
    timeout 300 python3 "$HERE/guest-walk.py" "$CR3" "$VA" 2>&1 | tail -2 \
        | tee "$OUT/tick-t0.txt"
    python3 -c "import time; time.sleep(60)"
    clear_nc
    echo "--- t1 ---"; date +%H:%M:%S
    timeout 300 python3 "$HERE/guest-walk.py" "$CR3" "$VA" 2>&1 | tail -2 \
        | tee "$OUT/tick-t1.txt"
    clear_nc
    timeout 900 python3 "$HERE/guest-l2-vectors.py" --elf "$ELF" --cpus 2 --top 2 \
        --base "$MODBASE" > "$OUT/vectors-2.txt" 2>&1 || true
    grep -E "l2_injected_vector cpu|vector 0xd1" "$OUT/vectors-2.txt" | head -8

    echo "=== 6. is VBoxSup.sys still the tail, and is its load complete ==="
    clear_nc
    timeout 600 python3 "$HERE/guest-loading-driver.py" "$KB" "$CR3" 2>&1 | tail -12

    echo "=== 7. process list (LogonUI is NOT the login screen - ASK THE USER) ==="
    clear_nc
    timeout 600 python3 "$HERE/guest-processes.py" "$KB" "$CR3" 2>&1 | tail -40
else
    echo "!! kernel base or cr3 not found in the dump - sections 4-7 skipped."
    echo "   That is a reader gap, NOT a fact about the guest."
fi

echo
echo "=== captured under $OUT ==="
echo "Nothing was killed. The guest is still running - work it before cycling."
