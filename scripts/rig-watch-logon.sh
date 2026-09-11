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
# Set ZPP_LOGON_CAPTURE_DIR to retain each process/power reply with a UTC
# timestamp. ZPP_LOGON_OUT overrides the one-line status log.
set -eu

KB="$1"; CR3="$2"; POLL="${3:-60}"
HERE=$(cd "$(dirname "$0")" && pwd)
RIG=tc@192.168.1.199
OUT=${ZPP_LOGON_OUT:-/tmp/logon-watch.txt}
CAPTURE_DIR=${ZPP_LOGON_CAPTURE_DIR:-}
[ -z "$CAPTURE_DIR" ] || mkdir -p "$CAPTURE_DIR"
: > "$OUT"

clear_nc() { ssh -o ConnectTimeout=8 "$RIG" 'pkill -x nc' 2>/dev/null || true; sleep 2; }

echo "watching for LogonUI+dwm; base $KB cr3 $CR3, every ${POLL}s" | tee -a "$OUT"
SHOUTED=0
while :; do
    STAMP=$(date -u +%Y%m%dT%H%M%SZ)
    clear_nc
    STATUS=$(printf 'info status\n' | nc -w 5 192.168.1.199 4446 2>/dev/null \
             | grep -ai "VM status" || echo "VM status: UNREADABLE")
    case "$STATUS" in *paused*)
        echo "[$(date +%H:%M:%S)] $STATUS - guest stopped, exiting" | tee -a "$OUT"
        exit 0 ;;
    esac

    clear_nc
    if ! PS=$(timeout 300 python3 "$HERE/guest-processes.py" "$KB" "$CR3" 2>&1); then
        echo "[$(date +%H:%M:%S)] **READ FAILED** incomplete process walk; $STATUS" | tee -a "$OUT"
        printf '%s\n' "$PS" > /tmp/logon-process-read-failed.txt
        if [ -n "$CAPTURE_DIR" ]; then
            printf '%s\n' "$PS" > "$CAPTURE_DIR/processes-read-failed-$STAMP.txt"
        fi
        sleep "$POLL"
        continue
    fi
    if [ -n "$CAPTURE_DIR" ]; then
        printf '%s\n' "$PS" > "$CAPTURE_DIR/processes-$STAMP.txt"
    fi
    # `grep -c` prints 0 and EXITS NON-ZERO on no match, so `|| echo 0`
    # would append a second line and break every integer test after it -
    # the bug that inverted the endgame verdict on boot 275.
    N=$(printf '%s' "$PS" | grep -cE "^  [A-Za-z]" || true)
    HAS_LOGON=$(printf '%s' "$PS" | grep -c "LogonUI.exe" || true)
    HAS_DWM=$(printf '%s' "$PS" | grep -c "dwm.exe" || true)

    # One reader owns the monitor for both lists. A second reader can
    # block the logon observation during its short window of visibility.
    # The old per-word sleeps made a power walk take minutes, requiring
    # one sample every four polls. Prompt-framed reads remove that cost.
    # Read every poll: a process-count threshold is not evidence that
    # power requests cannot already be armed. Only a complete successful
    # power-list walk can produce an armed count of zero.
    clear_nc
    if POWER=$(timeout 200 python3 "$HERE/guest-power-irps.py" "$KB" "$CR3" 2>&1); then
        ARMED=$(printf '%s\n' "$POWER" | grep -c "ENABLED (armed" || true)
        if [ -n "$CAPTURE_DIR" ]; then
            printf '%s\n' "$POWER" > "$CAPTURE_DIR/power-$STAMP.txt"
        fi
    else
        ARMED="UNREADABLE"
        printf '%s\n' "$POWER" > /tmp/logon-power-read-failed.txt
        if [ -n "$CAPTURE_DIR" ]; then
            printf '%s\n' "$POWER" > "$CAPTURE_DIR/power-read-failed-$STAMP.txt"
        fi
    fi
    # **`n=0` is a FAILED READ, not an empty guest.** The walker always
    # finds at least `System`, so zero means the read did not answer -
    # and the commonest cause is a second watcher left running from the
    # previous boot, against a stale kernel base, fighting this one for
    # the monitor's single connection. That happened on boot 372 and
    # produced alternating n=0 / n=5 lines that look like a guest
    # flickering rather than two readers colliding.
    if [ "$N" -eq 0 ]; then
        echo "[$(date +%H:%M:%S)] **READ FAILED** (n=0 is impossible - the" \
             "walker always finds System). Stale watcher or monitor" \
             "contention; not a fact about the guest.  $STATUS" \
             | tee -a "$OUT"
    else
        echo "[$(date +%H:%M:%S)] n=$N logonui=$HAS_LOGON dwm=$HAS_DWM " \
             "armed=$ARMED  $STATUS" | tee -a "$OUT"
    fi

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
