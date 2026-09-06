#!/bin/sh
# Watch a live 2-vCPU boot and, the MOMENT it reaches the 14-process wall,
# take the measurement that decides what the wall is.
#
# **Why this exists.** The wall is reached and then the guest stops
# minutes later - boot 195 hit 14 processes at 21.9 min and was in
# `paused (shutdown)` when next polled. Every attempt to take a live
# reading there has missed the window by hand-polling. Boots 195 and 196
# both denied the same measurement for that reason.
#
# The measurement: `services.exe`'s `ContextSwitches`, differenced.
# `5871b1e` found the SCM runnable-and-preempted with ZERO `svchost.exe`
# after 45 minutes, and a `WaitReason` cannot tell a just-preempted thread
# from a starved one. The counter can, and the two answers point opposite
# ways:
#
#   RISING  -> the SCM is being scheduled and is blocked on something
#              specific. A different investigation entirely.
#   FROZEN  -> it is starved, and the question is what holds the
#              processor against a thread that wants it.
#
# usage: rig-catch-14.sh <kernel_base_hex> <cr3_hex> [poll_seconds]
set -e
BASE="$1"
CR3="$2"
POLL="${3:-90}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$BASE" ] || [ -z "$CR3" ]; then
    echo "usage: rig-catch-14.sh <kernel_base_hex> <cr3_hex> [poll_seconds]" >&2
    exit 2
fi

echo "watching for the 14-process wall, polling every ${POLL}s"
echo "  base $BASE  cr3 $CR3"

while :; do
    pkill -x nc 2>/dev/null || true
    STATUS=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
             | grep -ai "VM status" | tail -1 || true)
    case "$STATUS" in
        *paused*)
            echo "GUEST STOPPED before the catch: $STATUS"
            echo "  the wall was reached and passed, or the guest died earlier."
            echo "  read the bugcheck; the live measurement is not available."
            exit 1
            ;;
    esac

    pkill -x nc 2>/dev/null || true
    N=$(timeout 400 python3 "$HERE/guest-processes.py" "$BASE" "$CR3" 2>/dev/null \
        | sed -n 's/^\([0-9]*\) processes;.*/\1/p' | head -1 || true)
    [ -n "$N" ] || N=0
    echo "$(date +%H:%M:%S)  processes: $N   ($STATUS)"

    # 13 as well as 14: the count has been 13 and 14 on different boots
    # and waiting for exactly 14 would miss one of them.
    if [ "$N" -ge 13 ] 2>/dev/null; then
        echo "=== WALL REACHED - taking the live measurement NOW ==="
        pkill -x nc 2>/dev/null || true
        timeout 500 python3 "$HERE/guest-threads.py" "$BASE" "$CR3" services \
            2>&1 | tee /tmp/svc-a.txt
        sleep 60
        pkill -x nc 2>/dev/null || true
        timeout 500 python3 "$HERE/guest-threads.py" "$BASE" "$CR3" services \
            2>&1 | tee /tmp/svc-b.txt
        echo "=== DIFFERENCED ==="
        python3 - <<'PY'
import re
def load(p):
    d = {}
    for l in open(p):
        m = re.search(r'thread (0x[0-9a-f]+)\s+WaitReason (\d+) = (\S+)'
                      r'\s+ContextSwitches (\d+)', l)
        if m:
            d[m.group(1)] = (m.group(3), int(m.group(4)))
    return d
A, B = load('/tmp/svc-a.txt'), load('/tmp/svc-b.txt')
if not A:
    print('services.exe not found or no threads read - nothing to difference')
else:
    moved = 0
    for t in A:
        if t not in B:
            continue
        (ra, ca), (rb, cb) = A[t], B[t]
        d = cb - ca
        moved += 1 if d else 0
        print(f'  {t}  {ra:>16} -> {rb:<16} ctxsw {ca:>7} -> {cb:<7} '
              f'delta {d:+}')
    print(f'\n{moved} of {len(A)} services.exe threads scheduled in ~60 s')
    print('  RISING -> the SCM runs and is blocked on something specific')
    print('  FROZEN -> the SCM is starved; find what holds the processor')
PY
        exit 0
    fi
    sleep "$POLL"
done
