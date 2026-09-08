#!/bin/sh
# Poll the guest's process list and shout the instant `LogonUI.exe` and
# `dwm.exe` appear.
#
# **Why this exists.** Boot 278 reached n=31 with `LogonUI.exe` and
# `dwm.exe` at 23:00 and was dead by 23:03. The screen - the ONE piece of
# evidence that cannot be recovered afterwards, because the display is a
# passed-through GPU and `screendump` is impossible - was never looked at,
# and the `0x9F` repainted it with a blue screen. Everything else in that
# boot (process list, bugcheck, power IRPs) survived the stop, because
# `-no-reboot -no-shutdown` keeps memory. The frame did not.
#
# So the perishable evidence needs a trigger of its own. This writes a
# one-line status every poll and an unmissable banner on breakthrough,
# to a file that can be checked in seconds without re-reading the guest.
#
# **It does not decide anything and never kills the guest.** On
# breakthrough it keeps polling, so the record shows how long the state
# lasted - which is itself unknown: boot 231 is recorded as durable for
# 41 minutes, boot 278 lasted three.
#
# Usage:  scripts/rig-watch-logon.sh <kernel base> <cr3> [poll secs]
set -eu

KB="$1"; CR3="$2"; POLL="${3:-60}"
HERE=$(cd "$(dirname "$0")" && pwd)
RIG=tc@192.168.1.199
OUT=/tmp/logon-watch.txt
: > "$OUT"

clear_nc() { ssh -o ConnectTimeout=8 "$RIG" 'pkill -x nc' 2>/dev/null || true; sleep 2; }

echo "watching for LogonUI+dwm; base $KB cr3 $CR3, every ${POLL}s" | tee -a "$OUT"
SHOUTED=0
while :; do
    clear_nc
    STATUS=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
             | grep -ai "VM status" || echo "VM status: UNREADABLE")
    case "$STATUS" in *paused*)
        echo "[$(date +%H:%M:%S)] $STATUS - guest stopped, exiting" | tee -a "$OUT"
        exit 0 ;;
    esac

    clear_nc
    PS=$(timeout 300 python3 "$HERE/guest-processes.py" "$KB" "$CR3" 2>&1 || true)
    # `grep -c` prints 0 and EXITS NON-ZERO on no match, so `|| echo 0`
    # would append a second line and break every integer test after it -
    # the bug that inverted the endgame verdict on boot 275.
    N=$(printf '%s' "$PS" | grep -cE "^  [A-Za-z]" || true)
    HAS_LOGON=$(printf '%s' "$PS" | grep -c "LogonUI.exe" || true)
    HAS_DWM=$(printf '%s' "$PS" | grep -c "dwm.exe" || true)

    # **The armed count is read HERE, by this poller, on purpose.**
    # Boot 335 broke through to n=29 with LogonUI and dwm up and its
    # screen was never looked at, because the endgame test was run as a
    # separate reader and this watcher had to be stopped for it twice -
    # the monitor takes one connection. A missed power-IRP sample costs
    # one point in a series of twenty-three; a missed screen costs the
    # only evidence that answers the question, and it does not survive
    # the stop.
    #
    # So one poller does both. `armed` is the endgame test: zero at
    # n=13-14 is the escape class (boots 231, 278, 335), non-zero means
    # 300 s from each arming.
    clear_nc
    ARMED=$(timeout 200 python3 "$HERE/guest-power-irps.py" "$KB" "$CR3" \
            2>/dev/null | grep -c "ENABLED (armed" || true)
    echo "[$(date +%H:%M:%S)] n=$N logonui=$HAS_LOGON dwm=$HAS_DWM " \
         "armed=$ARMED  $STATUS" | tee -a "$OUT"

    if [ "$HAS_LOGON" -gt 0 ] && [ "$HAS_DWM" -gt 0 ] && [ "$SHOUTED" -eq 0 ]; then
        SHOUTED=1
        {
            echo "=============================================="
            echo "BREAKTHROUGH at $(date +%H:%M:%S): n=$N, LogonUI+dwm up."
            echo "ASK THE USER WHAT IS ON SCREEN, NOW, BEFORE ANY OTHER READ."
            echo "The frame is the only evidence that does not survive the"
            echo "stop. Boot 278 had a three-minute window and lost it."
            echo "=============================================="
        } | tee -a "$OUT"
    fi
    sleep "$POLL"
done
