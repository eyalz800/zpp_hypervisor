#!/bin/sh
# Poll the guest's power IRPs and, the FIRST time a watchdog is armed,
# immediately read `PopIrpThreadList` - the instrument that names a
# culprit rather than a victim.
#
# **Why the timing matters.** The 0x9F fires 300 s after a watchdog arms,
# and `guest-power-irps.py` read post-mortem on boot 263 showed TWO
# entries aging together 20.6 s apart, which by that script's own
# pre-declared rule means the wedge is upstream and the named driver is
# arbitrary. Naming the upstream thing needs a read taken WHILE the IRPs
# age, not after - and `guest-power-workers.py`'s docstring records that
# nothing in this tree has ever read it.
#
# Its hypothesis: `PopInitializeIrpWorkers` creates exactly TWO
# `PopIrpWorker` threads at base priority 13, and the actual
# `IRP_MJ_POWER` dispatch happens on them. Two PASSIVE_LEVEL threads is
# the shape that starves first on a slow machine.
#
# This script only reads. It never kills the guest.
#
# Usage:  scripts/rig-watch-power-wedge.sh <kernel base> <cr3> [poll secs]
set -eu

KB="$1"; CR3="$2"; POLL="${3:-45}"
HERE=$(cd "$(dirname "$0")" && pwd)
RIG=tc@192.168.1.199
OUT=/tmp/power-wedge
mkdir -p "$OUT"

clear_nc() { ssh -o ConnectTimeout=8 "$RIG" 'pkill -x nc' 2>/dev/null || true; sleep 2; }

echo "watching power IRPs; kernel base $KB cr3 $CR3, every ${POLL}s"
N=0
while :; do
    N=$((N + 1))
    clear_nc
    STATUS=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
             | grep -ai "VM status" || echo "VM status: UNREADABLE")
    clear_nc
    IRPS=$(timeout 300 python3 "$HERE/guest-power-irps.py" "$KB" "$CR3" 2>&1 || true)
    # `grep -c` PRINTS 0 and EXITS NON-ZERO when it matches
    # nothing, so `|| echo 0` appends a second line and every
    # later integer test dies with "integer expression expected"
    # - which in rig-healthy-control.sh fell through to the
    # WRONG branch and reported a boot with zero armed watchdogs
    # as spent. `|| true` keeps grep's own "0".
    ARMED=$(printf '%s' "$IRPS" | grep -c "ENABLED (armed" || true)
    echo "[$N $(date +%H:%M:%S)] $STATUS   armed watchdogs: $ARMED"

    case "$STATUS" in *paused*)
        echo "guest stopped - capturing post-mortem and exiting"
        printf '%s\n' "$IRPS" > "$OUT/irps-final.txt"
        clear_nc
        timeout 300 python3 "$HERE/guest-bugcheck.py" "$KB" "$CR3" \
            > "$OUT/bugcheck.txt" 2>&1 || true
        tail -12 "$OUT/bugcheck.txt"
        exit 0 ;;
    esac

    if [ "$ARMED" -gt 0 ]; then
        # **The decisive read, and boot 265 did not take it.** If the
        # guest is running NORMALLY - trust-level traffic flowing, exits
        # at healthy rates - for the whole 300 s while its power IRPs
        # age out, then the 0x9F is not a throughput failure at all and
        # "the machine is too slow to finish in 300 s" is refuted. If
        # instead the guest has dropped to a stalled profile by now,
        # the two failures are the same failure and the throughput
        # account survives. Nothing else distinguishes those, and the
        # window to ask is only open while the watchdogs run.
        echo "=== ARMED. Is the GUEST still healthy right now? ==="
        clear_nc
        timeout 300 python3 "$HERE/rig-dump-state.py" \
            --elf .rig-deployed-hypervisor.elf --cpus 2 --delta 60 \
            > "$OUT/triage-at-arm.txt" 2>&1 || true
        grep -E "^  vmcall |vtl_fresh_calls|stimer_asked_arms" \
            "$OUT/triage-at-arm.txt" | head -6
        echo "=== Reading PopIrpThreadList NOW (never read before) ==="
        printf '%s\n' "$IRPS" | grep -E "CurrentDevice|age |WatchdogState" | head -20
        printf '%s\n' "$IRPS" > "$OUT/irps-at-arm.txt"
        clear_nc
        timeout 400 python3 "$HERE/guest-power-workers.py" "$KB" "$CR3" \
            2>&1 | tee "$OUT/workers-at-arm.txt" | tail -30
        # **Are DEVICE interrupts still arriving while the IRPs age?**
        # The three drivers seen stuck - IntcOED, USBHUB3, HidUsb - are
        # all idle devices being powered DOWN, and a D-state transition
        # completes when the device interrupts back. Those interrupts
        # reach the guest through us, so "the device stopped
        # interrupting" and "we stopped delivering" are the same
        # observation from two sides, and either is ours to answer for.
        #
        # A per-vector DELTA is the read: a vector that was arriving and
        # stops is a device that went quiet. Cumulative totals cannot
        # show that - a vector with a large total may have stopped
        # minutes ago, which is the failure mode this tree keeps
        # recording.
        MODBASE=$(ssh -o ConnectTimeout=8 "$RIG" \
            'grep -ao "allocate_rwx done at 0x[0-9a-f]*" ~/zpp/serial.out | tail -1' \
            2>/dev/null | grep -o '0x[0-9a-f]*')
        clear_nc
        timeout 600 python3 "$HERE/guest-l2-vectors.py" \
            --elf .rig-deployed-hypervisor.elf --cpus 2 --top 0 \
            --base "$MODBASE" > "$OUT/vectors-a.txt" 2>&1 || true
        sleep 60
        clear_nc
        timeout 600 python3 "$HERE/guest-l2-vectors.py" \
            --elf .rig-deployed-hypervisor.elf --cpus 2 --top 0 \
            --base "$MODBASE" > "$OUT/vectors-b.txt" 2>&1 || true
        python3 - "$OUT/vectors-a.txt" "$OUT/vectors-b.txt" <<'PY'
import re, sys
def load(path):
    out, cpu = {}, None
    for line in open(path):
        m = re.match(r"l2_injected_vector (cpu\d+):", line)
        if m:
            cpu = m.group(1); out[cpu] = {}; continue
        m = re.match(r"\s+vector (0x[0-9a-f]+)\s+([\d,]+)", line)
        if m and cpu:
            out[cpu][m.group(1)] = int(m.group(2).replace(",", ""))
    return out
a, b = load(sys.argv[1]), load(sys.argv[2])
print("\n=== per-vector DELTA over ~60 s while the power IRPs age ===")
for cpu in sorted(set(a) | set(b)):
    print(f"  {cpu}:")
    keys = sorted(set(a.get(cpu, {})) | set(b.get(cpu, {})),
                  key=lambda v: -(b.get(cpu, {}).get(v, 0)
                                  - a.get(cpu, {}).get(v, 0)))
    for v in keys:
        d = b.get(cpu, {}).get(v, 0) - a.get(cpu, {}).get(v, 0)
        tag = "  <- STOPPED (nonzero total, zero delta)" \
              if d == 0 and a.get(cpu, {}).get(v, 0) else ""
        print(f"    {v}  delta {d:>9,}{tag}")
PY
        echo "=== captured under $OUT - continuing to watch ==="
        # Keep watching: a second read while the SAME IRPs age says
        # whether the workers are stuck in one driver or cycling.
        sleep "$POLL"
        clear_nc
        timeout 400 python3 "$HERE/guest-power-workers.py" "$KB" "$CR3" \
            2>&1 | tee "$OUT/workers-second.txt" | tail -20
        echo "=== second worker read taken; compare against the first ==="
        exit 0
    fi
    sleep "$POLL"
done
