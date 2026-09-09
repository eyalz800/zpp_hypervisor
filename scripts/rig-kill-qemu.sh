#!/bin/sh
# Kill the guest on the rig, by PID, without killing anything else - and
# leave the launcher alive so its VFIO teardown runs.
#
# Three rules, each of which cost a run when it was broken:
#
#   - **Find QEMU by process NAME, never by command line.** `pkill -f
#     qemu-system` and `ps -o args | grep "[q]emu-system"` both match any
#     process whose arguments merely mention it - which includes the ssh
#     command running the check, and the launcher started with
#     ZPP_QEMU_EXTRA=... in its environment. A `pkill -f boot-zpp.sh`
#     killed its own ssh session mid-command, and the ps form made
#     ensure-traced-kvm.sh refuse to load KVM because it believed a guest
#     was still running. /proc/<pid>/comm is the process name and cannot
#     match a mention.
#
#   - **Do not kill the launcher.** boot-zpp.sh rebinds the NVMe, the GPU
#     and the rest back from vfio-pci after QEMU exits. Kill it and the
#     devices stay bound to vfio-pci, so the next deploy finds no
#     /dev/nvme0n1p2 and the next run cannot claim the devices.
#
#   - **Never SIGKILL blindly and walk away.** A QEMU killed mid-VFIO
#     teardown becomes a zombie whose last thread sits in D state inside
#     the kernel, holding every pinned guest page - twice measured at
#     ~12-13 GB - and nothing reaps it. TERM first gives it the chance to
#     unwind cleanly; KILL is the fallback.

# **Kill any watcher before the guest goes.** A watcher left running
# from the previous boot polls a stale kernel base against the new
# guest and fights the real one for the monitor's single connection.
# Boot 372 ran two and produced alternating n=0 / n=5 lines.
pkill -f rig-watch-logon.sh 2>/dev/null || true
pkill -f rig-watch-power-wedge.sh 2>/dev/null || true

set -u

RIG=${ZPP_TARGET:-tc@192.168.1.199}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 $RIG"

# Everything below runs on the rig in one shell, because each step needs
# the previous step's result and a round trip per step is how a teardown
# gets interleaved with a new run.
$SSH 'set -u

qemu_pids() {
    for p in /proc/[0-9]*; do
        c=$(cat "$p/comm" 2>/dev/null) || continue
        case "$c" in
            qemu*) ;;
            *) continue ;;
        esac
        # A zombie is already dead; it cannot be killed again and
        # reporting it as running blocks the next run for no reason.
        s=$(awk "{print \$3}" "$p/stat" 2>/dev/null)
        [ "$s" = "Z" ] && continue
        echo "${p#/proc/}"
    done
}

pids=$(qemu_pids)
if [ -z "$pids" ]; then
    echo "no qemu running"
else
    echo "killing qemu: $pids"
    for pid in $pids; do sudo kill -TERM "$pid" 2>/dev/null || true; done

    i=0
    while [ $i -lt 10 ] && [ -n "$(qemu_pids)" ]; do
        sleep 1
        i=$((i + 1))
    done

    if [ -n "$(qemu_pids)" ]; then
        echo "still up after ${i}s, sending KILL"
        for pid in $(qemu_pids); do sudo kill -9 "$pid" 2>/dev/null || true; done
        i=0
        while [ $i -lt 15 ] && [ -n "$(qemu_pids)" ]; do
            sleep 1
            i=$((i + 1))
        done
    fi

    echo "qemu gone: $([ -z "$(qemu_pids)" ] && echo yes || echo no)"
fi

# The launcher was deliberately left alone; give its restore a moment and
# then report what it actually put back. The NVMe is the one that matters:
# without it there is no /dev/nvme0n1p2 and no deploy.
i=0
while [ $i -lt 20 ] && [ ! -b /dev/nvme0n1p2 ]; do
    sleep 1
    i=$((i + 1))
done

echo "nvme: $([ -b /dev/nvme0n1p2 ] && echo present || echo ABSENT)"
echo "gpu driver: $(basename "$(readlink /sys/bus/pci/devices/0000:00:02.0/driver 2>/dev/null)" 2>/dev/null || echo none)"
echo "free: $(free -m | awk "/^Mem:/ {print \$4\" MB\"}")"

# A zombie here means the guest pages are still pinned and the next run
# cannot get them back. Say so plainly - it is the one outcome that needs
# a human, and the only fix found so far is a reboot.
zombies=""
for p in /proc/[0-9]*; do
    c=$(cat "$p/comm" 2>/dev/null) || continue
    case "$c" in qemu*) ;; *) continue ;; esac
    s=$(awk "{print \$3}" "$p/stat" 2>/dev/null)
    [ "$s" = "Z" ] && zombies="$zombies ${p#/proc/}"
done
[ -n "$zombies" ] && echo "WARNING: zombie qemu$zombies still holds pinned guest memory"

exit 0'
