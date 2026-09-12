#!/bin/sh
# Log the process list AND the power-IRP watchdog state on the same poll,
# so their ORDER is recorded rather than reconstructed afterwards.
#
# **Why this supersedes `rig-watch-order.sh`.** That script logs only the
# process list, which can say when WerFault appears but not what the
# machine was doing before it did. The question `26f23d8` raised needs
# both halves:
#
#   watchdog armed BEFORE WerFault appears -> WerFault is a CONSEQUENCE
#                                             of a power timeout
#   WerFault appears first, watchdog later -> it is not, and the
#                                             prediction in 7f7139a is
#                                             wrong
#
# **The two cannot be separate pollers.** The QEMU monitor takes ONE
# connection and every reader here does `pkill -x nc`, so a second poller
# does not merely contend - it kills the first one's connection mid-read
# and both report failures that look like guest state. One poller, both
# reads, same window.
#
# usage: rig-watch-order2.sh <kernel_base_hex> <cr3_hex> [poll_seconds]
set -e
BASE="$1"; CR3="$2"; POLL="${3:-120}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$BASE" ] && [ -n "$CR3" ] || { echo "usage: rig-watch-order2.sh <base> <cr3> [poll]" >&2; exit 2; }
OUT=/tmp/boot-order.txt
echo "== rig-watch-order2 starting $(date +%H:%M:%S) ==" >> "$OUT"
while :; do
    pkill -x nc 2>/dev/null || true
    ST=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
         | grep -ai "VM status" | tail -1 || true)
    pkill -x nc 2>/dev/null || true
    LIST=$(timeout 400 python3 "$HERE/guest-processes.py" "$BASE" "$CR3" \
           2>/dev/null | sed -n 's/^  \([A-Za-z0-9_.-]*\) *pid.*/\1/p' \
           | sort | tr '\n' ' ' || true)
    N=$(echo "$LIST" | wc -w | tr -d ' ')
    # The watchdog half. `guest-power-irps.py` already refuses to print an
    # age when WatchdogStart is 0 (85ac318), so "NEVER ARMED" here is the
    # script's own verdict and not an inference from a missing line.
    pkill -x nc 2>/dev/null || true
    WD=$(timeout 400 python3 "$HERE/guest-power-irps.py" "$BASE" "$CR3" \
         2>/dev/null | grep -E "NEVER ARMED|WatchdogStart [1-9]|age " \
         | tr '\n' '|' | cut -c1-200 || true)
    echo "$(date +%H:%M:%S) n=$N $ST :: $LIST" | tee -a "$OUT"
    echo "    watchdog: ${WD:-<no power IRP entries read>}" | tee -a "$OUT"
    case "$ST" in *paused*) echo "guest stopped - order log complete"; exit 0;; esac
    sleep "$POLL"
done
