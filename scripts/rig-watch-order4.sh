#!/bin/sh
# Date WerFault's appearance tightly, and recover the watchdog's arming
# time by arithmetic instead of trying to catch the instant it happens.
#
# **Supersedes rig-watch-order2.sh, which was wrong.** `a5c4969` records
# the failure in full: that script piped `guest-power-irps.py` through
# `cut -c1-200`, which reached only the first two of six entries. The
# five it could reach are `IRP_MN_WAIT_WAKE`, which the reader itself
# prints as never armed BY DESIGN, so the poller re-read a constant six
# times and reported it as a measurement - and it produced a confident
# refutation by being blind to the one row that could disagree.
#
# TWO CHANGES, AND THE SECOND IS THE INTERESTING ONE
# --------------------------------------------------
# 1. **No truncation, ever.** The whole watchdog block is logged, and the
#    ARMED entries are pulled out by name rather than by position.
#
# 2. **Stop chasing the arming instant.** An armed entry carries
#    `WatchdogStart` and an `age`, so ANY later read back-dates the
#    arming by subtraction - one read at the end is as good as catching
#    it live. What cannot be recovered afterwards is when `WerFault`
#    APPEARED, because a process that exits leaves nothing to date.
#
#    So the expensive precision goes where it cannot be bought later: the
#    process list every cycle, the watchdog every third. Boot 209 failed
#    to order the two events because its poll gap was 6.5 minutes and
#    both fell inside it - and the gap was spent re-reading a constant.
set -e
BASE="$1"; CR3="$2"; POLL="${3:-20}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$BASE" ] && [ -n "$CR3" ] || { echo "usage: rig-watch-order3.sh <base> <cr3> [poll]" >&2; exit 2; }
OUT=/tmp/boot-order.txt
echo "== rig-watch-order4 starting $(date +%H:%M:%S) ==" >> "$OUT"
I=0
while :; do
    I=$((I + 1))
    pkill -x nc 2>/dev/null || true
    ST=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
         | grep -ai "VM status" | tail -1 || true)
    pkill -x nc 2>/dev/null || true
    # **Names with a SPACE are real, and the old pattern dropped them.**
    # `[A-Za-z0-9_.-]*` cannot match "Secure System", so every sample of
    # boots 209 and 210 undercounted by one and was blind to precisely
    # the VBS process - the one whose presence means VTL1 is up. Match
    # everything up to the `pid` column instead, and squeeze the space so
    # the name stays one word.
    LIST=$(timeout 400 python3 "$HERE/guest-processes.py" "$BASE" "$CR3" \
           2>/dev/null | sed -n 's/^  \(.*[^ ]\) *pid .*/\1/p' \
           | tr ' ' '_' | sort | tr '\n' ' ' || true)
    N=$(echo "$LIST" | wc -w | tr -d ' ')
    echo "$(date +%H:%M:%S) n=$N $ST :: $LIST" | tee -a "$OUT"
    case "$ST" in *paused*) echo "guest stopped - order log complete"; exit 0;; esac

    # Every third cycle, and in full. An ARMED entry is identified by
    # `WatchdogState 1`, never by where it sits in the list.
    if [ $((I % 3)) -eq 1 ]; then
        pkill -x nc 2>/dev/null || true
        timeout 400 python3 "$HERE/guest-power-irps.py" "$BASE" "$CR3" \
            > /tmp/wd-latest.txt 2>&1 || true
        ARMED=$(grep -c "WatchdogState 1" /tmp/wd-latest.txt 2>/dev/null || echo 0)
        echo "    watchdog: $ARMED ARMED entr(y/ies)" | tee -a "$OUT"
        if [ "$ARMED" -gt 0 ] 2>/dev/null; then
            grep -E "WatchdogStart [0-9,]+$|age .*of|WatchdogState 1|MinorFunction" \
                /tmp/wd-latest.txt | tee -a "$OUT"
        fi
    fi
    sleep "$POLL"
done
