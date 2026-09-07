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

# **The reading owed since boot 265, and it can ONLY be taken here.**
# `pending_event_lost` counts events owed to the second-level guest that
# `reflect_l2_exit` was holding. It read 1,244 on healthy boot 265, every
# one vector 0x2e - Windows' `int 2Eh` system-call gate - and **0 on
# every stalled boot**, because a stalled guest makes no system calls to
# interrupt. It is an activity counter, so only a healthy boot has
# anything to say.
#
# What decides it is `pending_event_lost_while_valid`: a loss where the
# HARDWARE idt-vectoring field was valid is a hand-over correctly
# refused, with the architecture's own report reaching vmcs12 by the
# normal copy. A loss where it was not is an event genuinely destroyed -
# a system call that never returns, reported by nobody, which is the
# shape that would leave a driver's power IRP outstanding and is a live
# candidate for the 0x9F.
echo "=== 2b. events owed to L2 (the reading owed since boot 265) ==="
sed -n '/held events seen by reflect_l2_exit/,/^$/p' "$OUT/full.txt" | head -8

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

    # **The vector reads must BRACKET the tick reads, not straddle them.**
    # The first version read vectors before t0 and after t1, giving a
    # ~100 s injection window against a 62 s tick window, so boot 263's
    # injections-per-ISR-entry ratio was not computable at all. The
    # stalled boot's 0.9988 stands only because those two reads did
    # bracket. Order here is: vectors, t0, wait, t1, vectors - so both
    # spans share their endpoints as closely as the monitor allows.
    echo "=== 5. HalpClockTickLogIndex + injections, tightly bracketed ==="
    VA=$(python3 -c "print(hex(int('$KB',16)+0xe0a838))")
    clear_nc
    timeout 900 python3 "$HERE/guest-l2-vectors.py" --elf "$ELF" --cpus 2 --top 2 \
        --base "$MODBASE" > "$OUT/vectors-2a.txt" 2>&1 || true
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
        --base "$MODBASE" > "$OUT/vectors-2b.txt" 2>&1 || true
    grep -E "l2_injected_vector cpu|vector 0xd1" "$OUT/vectors-2b.txt" | head -8

    # **`|| true` piped into `tail` hides a failed read as an empty one.**
    # On boot 265 sections 6 and 7 printed nothing at all and had to be
    # re-run by hand; an empty section read exactly like "no drivers
    # loading", which is the instrument fault this tree keeps recording.
    # Capture to a file, then say whether it is empty and why.
    echo "=== 6. module list tail, and is its load complete ==="
    clear_nc
    timeout 600 python3 "$HERE/guest-loading-driver.py" "$KB" "$CR3" \
        > "$OUT/driver.txt" 2>&1 || echo "  (reader exited non-zero)"
    if [ -s "$OUT/driver.txt" ]; then tail -12 "$OUT/driver.txt"; else
        echo "  !! EMPTY - the read FAILED. This is not 'no driver loading'."
    fi

    echo "=== 7. process list (LogonUI is NOT the login screen - ASK THE USER) ==="
    clear_nc
    timeout 600 python3 "$HERE/guest-processes.py" "$KB" "$CR3" \
        > "$OUT/processes.txt" 2>&1 || echo "  (reader exited non-zero)"
    if [ -s "$OUT/processes.txt" ]; then
        grep -E "^  |processes;" "$OUT/processes.txt" | head -30
    else
        echo "  !! EMPTY - the read FAILED. This is not 'no processes'."
    fi
else
    echo "!! kernel base or cr3 not found in the dump - sections 4-7 skipped."
    echo "   That is a reader gap, NOT a fact about the guest."
fi

    # **The endgame test, and it is not the process count.**
    # `multicore-login-screen-reached-recipe`: boots 209, 212 and 218 all
    # reached n=13-14 with a power watchdog ALREADY ARMED and died to
    # `0x9F` exactly 300.0002 s later; boot 231 reached the same process
    # state with **0 of 7 armed** and went straight through to LogonUI.
    # So the 14-process plateau is where every healthy boot arrives and
    # the power-IRP failure is a separate event that follows it three
    # times in four.
    #
    # Zero armed  -> this boot is going through. NURSE IT, do not cycle.
    # Non-zero    -> the 300 s are already running and the boot is spent.
    if [ -s "$OUT/processes.txt" ]; then
        N=$(grep -cE "^  [A-Za-z]" "$OUT/processes.txt" || true)
        echo "=== 8. ENDGAME TEST at n=$N: armed power watchdogs ==="
        clear_nc
        timeout 400 python3 "$HERE/guest-power-irps.py" "$KB" "$CR3" \
            > "$OUT/power-irps.txt" 2>&1 || echo "  (reader exited non-zero)"
        if [ -s "$OUT/power-irps.txt" ]; then
            ARMED=$(grep -c "ENABLED (armed" "$OUT/power-irps.txt" || true)
            TOTAL=$(grep -oE "^[0-9]+ power IRP" "$OUT/power-irps.txt" \
                    | grep -oE "^[0-9]+" || true)
            echo "  armed: $ARMED of $TOTAL power IRPs in flight"
            if [ "$ARMED" -eq 0 ]; then
                echo "  -> ZERO ARMED. This is the shape that reached LogonUI."
                echo "     NURSE THIS BOOT. Do not cycle it."
            else
                echo "  -> $ARMED ARMED: 0x9F in <=300 s from when each armed."
                echo "     Capture what is wanted now; the boot is spent."
                grep -E "CurrentDevice|age " "$OUT/power-irps.txt" | head -8
            fi
        else
            echo "  !! EMPTY - the read FAILED, which is not 'none armed'."
        fi
    fi

echo
echo "=== captured under $OUT ==="
echo "Nothing was killed. The guest is still running - work it before cycling."
