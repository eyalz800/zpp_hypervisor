#!/bin/sh
# Poll the guest until it stops running, then say so.
#
# The guest is always started with `-no-reboot -no-shutdown` (rig-boot.sh
# has no opt-out), so a bugcheck leaves QEMU in `paused (shutdown)` with
# every byte of memory intact rather than resetting and destroying the
# state that explains it. This is how that stop is noticed without
# waiting out a whole run: a crash three minutes in is otherwise
# indistinguishable from a healthy boot until the very end.
#
# There is no screendump on this rig - the display is a passed-through
# GPU and QEMU answers "There is no console to take a screendump from" -
# so the VM's own status is the picture.
#
# The monitor takes ONE connection, so every probe opens and closes it.
# Decoding is done in python rather than tr/sed: the monitor echoes
# terminal control bytes that are not valid UTF-8, and `tr` fails on them
# with "Illegal byte sequence" on macOS, which reads exactly like the
# monitor not answering.
RIG=${ZPP_RIG:-192.168.1.199}
PORT=${ZPP_MONITOR_PORT:-4446}
EVERY=${1:-30}
LIMIT=${2:-40}

probe() {
    (printf 'info status\n'; sleep 2) | nc -w 6 "$RIG" "$PORT" 2>/dev/null |
        python3 -c "
import re,sys
d = sys.stdin.buffer.read().decode('utf-8', 'replace')
d = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', d).replace('\x1b', '')
m = re.findall(r'VM status: ([a-z ()]+)', d)
print(m[-1].strip() if m else '')"
}

i=0
while [ "$i" -lt "$LIMIT" ]; do
    S=$(probe)
    case "$S" in
        running) ;;
        '') echo "probe $i: monitor silent" ;;
        *)  echo "probe $i: STOPPED - VM status: $S"; exit 0 ;;
    esac
    i=$((i + 1))
    sleep "$EVERY"
done
echo "still running after $((LIMIT * EVERY))s"
exit 1
