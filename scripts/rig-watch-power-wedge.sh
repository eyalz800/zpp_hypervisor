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
            --elf .rig-deployed-hypervisor.elf --cpus 2 --delta 30 \
            > "$OUT/triage-at-arm.txt" 2>&1 || true
        grep -E "^  vmcall |vtl_fresh_calls|stimer_asked_arms" \
            "$OUT/triage-at-arm.txt" | head -6
        printf '%s\n' "$IRPS" | grep -E "CurrentDevice|age |WatchdogState" | head -20
        printf '%s\n' "$IRPS" > "$OUT/irps-at-arm.txt"
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
        sleep 30
        clear_nc
        # **A dead guest and a silent device produce the same zeroes.**
        # On boot 287 the guest stopped during this pair and every vector
        # on both processors printed "STOPPED (nonzero total, zero
        # delta)" - which reads as the finding this instrument exists to
        # make, and was only the guest having gone. Check status between
        # the reads and refuse to tag anything if it did.
        MIDSTATUS=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 \
                    2>/dev/null | grep -ai "VM status" || echo "UNREADABLE")
        echo "  status between the two vector reads: $MIDSTATUS"
        clear_nc
        timeout 600 python3 "$HERE/guest-l2-vectors.py" \
            --elf .rig-deployed-hypervisor.elf --cpus 2 --top 0 \
            --base "$MODBASE" > "$OUT/vectors-b.txt" 2>&1 || true
        case "$MIDSTATUS" in *paused*|*UNREADABLE*)
            echo "!! THE GUEST STOPPED INSIDE THE VECTOR WINDOW."
            echo "   Every delta below is zero because the machine is"
            echo "   gone, NOT because a device went quiet. VOID."
            ;;
        esac
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
print("\n=== per-vector DELTA over ~30 s while the power IRPs age ===")
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
        # **The xHCI's own MSI-X table, read from QEMU's side.**
        # USBHUB3 is armed in every 0x9F seen, and it sits on the
        # EMULATED qemu-xhci. A device power transition completes when
        # the controller interrupts back, so whether the controller can
        # interrupt AT ALL is the question - and it is answerable without
        # touching the guest, because the MSI-X table lives in the
        # device's own BAR and QEMU will read it.
        #
        # `info pci` puts qemu-xhci's BAR0 at 0x7011100000 and reports
        # "IRQ 10, pin A" for legacy INTx. `info irq` shows only IRQ 0
        # (1,009) and IRQ 1 (18) ever firing, so nothing uses INTx and
        # every device is on MSI - which `info irq` cannot count.
        #
        # Each MSI-X entry is four dwords: address low, address high,
        # data, vector control. **Vector control bit 0 is the MASK bit**,
        # and the low byte of `data` is the vector. Read early on boot
        # 295 (~7 min, before the USB driver initialises) both entries
        # read all zero with vector control **1 - masked**. If they still
        # read masked while USBHUB3's watchdog is running, the controller
        # cannot raise an interrupt and the transition can never complete.
        # If they are programmed and unmasked, it can, and the silence is
        # elsewhere.
        #
        # Narrow reads, per the device-register rule this tree records:
        # a wide `xp` over a BAR once reported a live NVMe as disabled.
        XHCI=0x7011100000
        echo "=== xHCI MSI-X table (mask bit = vector control bit 0) ==="
        for E in 0 1 2 3; do
            A=$(python3 -c "print(hex($XHCI + 0x3000 + $E * 16))")
            clear_nc
            ssh -o ConnectTimeout=8 "$RIG" \
                "printf 'xp /4xw $A\n' | nc -w 8 127.0.0.1 4446 | strings" \
                2>/dev/null | grep -E "^7011" | sed "s/^/  entry $E  /"
        done | tee "$OUT/xhci-msix.txt"

        # **The worker walk goes LAST now.** It has replicated three
        # times (boots 265, 287, 290) with the same answer - the pool is
        # not the bottleneck - while the vector delta has never once
        # produced a valid reading, so the vectors get the budget.
        echo "=== PopIrpThreadList (replicated 3x; lowest priority) ==="
        clear_nc
        timeout 400 python3 "$HERE/guest-power-workers.py" "$KB" "$CR3" \
            2>&1 | tee "$OUT/workers-at-arm.txt" | tail -20
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
