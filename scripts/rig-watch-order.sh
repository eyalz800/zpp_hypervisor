#!/bin/sh
# Log the guest's process list with a timestamp on every poll, so the
# ORDER in which processes appear is recorded rather than reconstructed.
#
# **Why this exists.** `26f23d8` proved WerFault is running `dbgeng.dll`,
# the Windows debug engine, at the 14-process wall. That raises a question
# no dump taken AT the wall can answer: is WerFault a *cause* of the wall
# or a *consequence* of it?
#
#   appears BEFORE the count stops rising -> it is part of what stops it
#   appears AFTER  the count stops rising -> it is a response to a
#                                            failure that already happened
#
# Those point at completely different investigations, and every reading
# this tree has of WerFault is a single sample at the wall - which cannot
# distinguish them, because at the wall both hypotheses look identical.
#
# This is the same defect the file already records twice: an instrument
# that samples one instant cannot report an order. So sample throughout.
#
# usage: rig-watch-order.sh <kernel_base_hex> <cr3_hex> [poll_seconds]
set -e
BASE="$1"; CR3="$2"; POLL="${3:-60}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$BASE" ] && [ -n "$CR3" ] || { echo "usage: rig-watch-order.sh <base> <cr3> [poll]" >&2; exit 2; }
OUT=/tmp/boot-order.txt
: > "$OUT"
echo "logging process-appearance order to $OUT, every ${POLL}s"
while :; do
    pkill -x nc 2>/dev/null || true
    ST=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
         | grep -ai "VM status" | tail -1 || true)
    pkill -x nc 2>/dev/null || true
    LIST=$(timeout 400 python3 "$HERE/guest-processes.py" "$BASE" "$CR3" \
           2>/dev/null | sed -n 's/^  \([A-Za-z0-9_.-]*\) *pid.*/\1/p' \
           | sort | tr '\n' ' ' || true)
    N=$(echo "$LIST" | wc -w | tr -d ' ')
    echo "$(date +%H:%M:%S) n=$N $ST :: $LIST" | tee -a "$OUT"
    case "$ST" in *paused*) echo "guest stopped - order log complete"; exit 0;; esac
    sleep "$POLL"
done
