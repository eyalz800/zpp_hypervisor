#!/bin/sh
# Keep ssh reachable on the rig while a VFIO guest is running.
#
# THIS SCRIPT NEVER REBOOTS ANYTHING. It restarts dropbear and nothing
# else. An earlier guard on this machine rebooted on a condition and was
# a standing risk of taking the machine down unattended; do not add a
# reboot, a power action, or a QEMU kill here. If ssh cannot be restored,
# the right outcome is that it stays down and a human decides.
#
# What actually goes wrong, measured rather than guessed: the failure is
# "Connection timed out during banner exchange", which means the TCP
# connect SUCCEEDED and dropbear's forked child never wrote its banner.
# The listener is alive and healthy - it is the child that cannot get
# memory or CPU. So restarting the daemon is the LAST resort here, not
# the first, and the two things that actually prevent the failure are:
#
#   - the daemon and its children must be exempt from the OOM killer and
#     must outrank eight spinning vCPU threads for CPU, and
#   - the check must be a real handshake. A listening socket proves
#     nothing: `nc -z` succeeded throughout the outage that prompted this.
#
# Run detached, as root:
#     setsid nohup /home/tc/vm/ssh-guard.sh >/dev/null 2>&1 </dev/null &
set -u

INTERVAL=${ZPP_GUARD_INTERVAL:-10}
FAILURES_BEFORE_RESTART=${ZPP_GUARD_FAILURES:-3}
LOG=${ZPP_GUARD_LOG:-/tmp/ssh-guard.log}
PORT=${ZPP_GUARD_PORT:-22}

log() { echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }

# Keep the log bounded: this runs for the length of a session on a RAM
# disk shared with a guest that wants nearly all of memory.
trim_log() {
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 65536 ]; then
        tail -c 32768 "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"
    fi
}

# The exact command line dropbear was started with, captured once while
# it is still healthy. Guessing a path and flags is how a guard restarts
# the daemon into a configuration that does not listen where anyone is
# looking.
dropbear_pids() {
    for p in /proc/[0-9]*; do
        c=$(cat "$p/comm" 2>/dev/null) || continue
        [ "$c" = "dropbear" ] && echo "${p#/proc/}"
    done
}

DROPBEAR_CMD=""
for pid in $(dropbear_pids); do
    DROPBEAR_CMD=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
    [ -n "$DROPBEAR_CMD" ] && break
done
[ -n "$DROPBEAR_CMD" ] || DROPBEAR_CMD="/usr/local/sbin/dropbear -R"

# Protect the guard itself first. If it is killed or starved there is
# nothing left to notice the outage.
echo -1000 > /proc/self/oom_score_adj 2>/dev/null || true
renice -n -15 -p $$ >/dev/null 2>&1 || true

# Give dropbear and every child it forks the same protection. Re-applied
# each cycle because each ssh session is a NEW process that inherits
# nothing from a one-time fix, and it is the children that were starving.
protect_dropbear() {
    for pid in $(dropbear_pids); do
        echo -1000 > "/proc/$pid/oom_score_adj" 2>/dev/null || true
        renice -n -15 -p "$pid" >/dev/null 2>&1 || true
    done
}

# A real handshake: connect and require the SSH identification string.
# Anything less passes while the machine is unusable.
handshake_ok() {
    banner=$(timeout 5 nc 127.0.0.1 "$PORT" < /dev/null 2>/dev/null | head -c 4)
    [ "$banner" = "SSH-" ]
}

restart_dropbear() {
    log "restarting dropbear: $DROPBEAR_CMD"
    for pid in $(dropbear_pids); do kill -9 "$pid" 2>/dev/null || true; done
    # No sleep-and-hope: start it, then confirm it answers before saying so.
    $DROPBEAR_CMD >/dev/null 2>&1 &
    i=0
    while [ $i -lt 10 ]; do
        sleep 1
        if handshake_ok; then
            log "dropbear answering again"
            protect_dropbear
            return 0
        fi
        i=$((i + 1))
    done
    log "dropbear still not answering after restart - leaving it to a human"
    return 1
}

log "guard started, checking :$PORT every ${INTERVAL}s (never reboots)"
protect_dropbear

consecutive=0
while true; do
    if handshake_ok; then
        [ "$consecutive" -gt 0 ] && log "recovered after $consecutive failed checks"
        consecutive=0
        protect_dropbear
    else
        consecutive=$((consecutive + 1))
        log "handshake failed ($consecutive/$FAILURES_BEFORE_RESTART)"
        if [ "$consecutive" -ge "$FAILURES_BEFORE_RESTART" ]; then
            restart_dropbear || true
            consecutive=0
        fi
    fi
    trim_log
    sleep "$INTERVAL"
done
