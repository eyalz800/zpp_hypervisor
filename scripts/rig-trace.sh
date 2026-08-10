#!/bin/sh
# Stream KVM tracepoints off the rig, live, with no pipe anywhere.
#
# ## Why the old pipeline had to go
#
# The previous design was trace_pipe -> FIFO -> a detached reader, and it
# **corrupts kernel memory** on the rig's 6.12.11-zpptrace. Measured
# 2026-08-10, from dmesg:
#
#   Oops: Bad pagetable: 0009   PMD 2e2e2e2e2e205d31        -> "1] ....."
#   Oops: general protection fault, probably for non-canonical
#         address 0x656363615f636970                        -> "pic_acce"
#   Fixing recursive fault but reboot is needed!
#
# Both constants are ASCII rather than addresses - the second is the
# middle of "kvm_apic_accept_irq" - so trace text was being written over
# kernel page tables. **Decode a suspicious address before believing it is
# one.**
#
# The attribution matters, because it says what to change. Both faulting
# tasks were touching the **FIFO**: pid 23443 was the feeder writing it,
# pid 23768 was the reader doing read() on it, and it warned inside a
# page/folio function. Neither fault was in the trace_pipe read. So the
# pipe machinery is implicated and trace_pipe is not, and the fix is to
# keep streaming but remove every pipe from the path:
#
#   rig:  nc -l -p PORT < trace_pipe     stdin IS the file - no pipe
#   here: nc rig PORT > /tmp/kvm.log     appended live, continuously
#
# `nc` reads fd 0 with read() and writes the socket with write(). There is
# no FIFO, no shell `|`, and nothing spliced.
#
# This is a hypothesis backed by which tasks faulted, not a root cause -
# the kernel has no symbols, so the call traces are bare addresses. It is
# therefore **stress tested before use**: `verify` streams sched_switch,
# which produced 3.2 M lines in 15 s, and then reads dmesg. Run it after
# any kernel change, and never take the first guest run of a session
# without it.
#
# ## Why the guest never dies for this any more
#
# The zombie qemu that pins ~12 GB and forces a reboot is downstream of
# that oops, not of killing qemu - both leaks had a capture running, and
# the one clean teardown measured had none. So a safe transport is worth
# more than it looks: it is what makes a run survivable.
#
# Two constraints still hold and still pull opposite ways:
#
#   - **Nothing is stored on the rig.** Its /tmp is a RAM disk shared with
#     a guest that wants nearly all of memory. The stream goes straight
#     out over the socket and never lands.
#   - **No tracefs reader may be a child of an ssh session.** Dropbear
#     forks a child per session and a reader blocked on tracefs takes that
#     session with it; leak enough and the rig pings but refuses ssh. The
#     listener is `setsid`-detached, so a blocked read takes nothing.
#
#   ./scripts/rig-trace.sh verify              # stress the transport, 15s
#   ./scripts/rig-trace.sh arm [events...]     # before booting
#   ./scripts/rig-trace.sh stream [seconds]    # live -> /tmp/kvm.log
#   ./scripts/rig-trace.sh status
#   ./scripts/rig-trace.sh stop
set -e

RIG=${ZPP_TARGET:-tc@192.168.1.199}
RIG_HOST=${RIG#*@}
PORT=${ZPP_TRACE_PORT:-5555}
OUT=${ZPP_TRACE_OUT:-/tmp/kvm.log}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 $RIG"
T=/sys/kernel/tracing

# Sparse by default, deliberately. These five answer "what did the guest
# hypervisor ask for, and did it arrive": IPIs sent, IPIs accepted, a
# halted processor waking, message-signalled interrupts from the
# passed-through devices - which is where a disk completion that never
# arrives would show - and nested entry refusals.
#
# Do not add kvm_exit/kvm_entry to a boot-length capture without checking
# what it costs; those are per-VM-exit and this link is 1 Gb at best.
DEFAULT_EVENTS="kvm_apic_ipi kvm_apic_accept_irq kvm_vcpu_wakeup kvm_msi_set_irq kvm_nested_vmenter_failed"

# Kill any previous listener before starting another. trace_pipe admits
# **one** reader, and a leftover one silently drains everything the next
# is waiting for - which reads as "the capture came back empty".
stop_listener()
{
    $SSH 'for p in /proc/[0-9]*; do
            c=$(cat $p/comm 2>/dev/null) || continue
            case "$c" in
                nc|timeout)
                    tr "\0" " " < $p/cmdline 2>/dev/null | grep -q trace_pipe &&
                        sudo kill ${p#/proc/} 2>/dev/null ;;
            esac
          done; exit 0'
}

# Start the detached listener. stdin is the file itself: `exec` replaces
# the shell so nc inherits fd 0 directly, and setsid detaches it so a
# blocking read can never hold an ssh session open.
start_listener()
{
    seconds=$1
    $SSH "sudo sh -c '
        [ -d $T/events ] || mount -t tracefs nodev $T
        setsid nohup sh -c \"exec timeout $seconds nc -l -p $PORT\" \
            < $T/trace_pipe > /dev/null 2>&1 &
        sleep 1
        echo \"listener on :$PORT for ${seconds}s\"
    '"
}

action=${1:-status}
shift 2>/dev/null || true

case "$action" in
verify)
    # Stress the transport at a rate far above anything KVM produces,
    # then read dmesg. sched_switch is the right stressor and the wrong
    # thing to leave on - 3.2 M lines in 15 s.
    echo "stressing the transport with sched_switch for 15s..."
    stop_listener
    $SSH "sudo sh -c '
        [ -d $T/events ] || mount -t tracefs nodev $T
        echo 0 > $T/tracing_on
        for e in $T/events/*/*/enable; do echo 0 > \$e 2>/dev/null; done
        echo 1 > $T/events/sched/sched_switch/enable
        echo 0 > $T/buffer_percent
        echo 1 > $T/tracing_on'"
    start_listener 15
    nc "$RIG_HOST" "$PORT" > /tmp/zpp-trace-verify.log || true
    lines=$(wc -l < /tmp/zpp-trace-verify.log)
    $SSH "sudo sh -c 'echo 0 > $T/tracing_on; echo 0 > $T/events/sched/sched_switch/enable'"
    echo "streamed $lines lines"
    echo "--- dmesg ---"
    $SSH 'sudo dmesg | grep -iE "oops|bad pagetable|reboot is needed|general protection" | tail -5 || true'
    if [ "$lines" -lt 1000 ]; then
        echo "FAIL: transport moved almost nothing - do not trust it" >&2
        exit 1
    fi
    $SSH 'sudo dmesg | grep -qiE "oops|bad pagetable|reboot is needed" ' &&
        { echo "FAIL: the kernel faulted during the stress - reboot and do not capture" >&2; exit 1; }
    echo "PASS: transport moved $lines lines and the kernel stayed clean"
    ;;
arm)
    events=${*:-$DEFAULT_EVENTS}
    # Every control is read *and* written as root: tracefs answers an
    # unprivileged read with an empty string rather than an error, which
    # is how five captures came back empty before anyone noticed the
    # events were never enabled.
    #
    # tracing_on goes last - writing buffer_size_kb resets it to 0 - and
    # triggers are cleared too, since a leftover traceoff trigger would
    # disarm everything while each enable still read 1.
    $SSH "sudo sh -c '
        [ -d $T/events ] || mount -t tracefs nodev $T
        [ -d $T/events/kvm ] || { echo \"no kvm events - kvm.ko not loaded\"; exit 1; }
        echo 0 > $T/tracing_on
        for e in $T/events/*/*/enable;  do echo 0 > \$e 2>/dev/null; done
        for e in $T/events/*/*/filter;  do echo 0 > \$e 2>/dev/null; done
        for e in $T/events/*/*/trigger; do echo 0 > \$e 2>/dev/null; done
        for e in $events; do
            echo 1 > $T/events/kvm/\$e/enable 2>/dev/null || echo \"no such event: \$e\"
        done
        echo 0 > $T/buffer_percent
        echo 1 > $T/tracing_on
        echo \"armed: on=\$(cat $T/tracing_on) events=\$(cat $T/set_event | tr \"\\n\" \" \")\"'"
    ;;
stream)
    seconds=${1:-600}
    stop_listener
    start_listener "$seconds"
    echo "draining to $OUT (live; tail -f it)"
    nc "$RIG_HOST" "$PORT" > "$OUT"
    echo "stream ended, $(wc -l < "$OUT") lines in $OUT"
    ;;
status)
    $SSH "sudo sh -c '
        [ -d $T/events/kvm ] || { echo \"kvm events absent - kvm.ko not loaded\"; exit 0; }
        echo \"tracing_on=\$(cat $T/tracing_on) buffer_kb=\$(cat $T/buffer_size_kb)\"
        echo \"enabled: \$(cat $T/set_event | tr \"\\n\" \" \")\"'"
    # tracing_on=0 with events still enabled is the signature of the oops
    # path having disarmed it, not of a mis-armed capture. Check dmesg
    # before re-arming, and before blaming a hypervisor bug for a hang.
    $SSH 'n=$(sudo dmesg | grep -icE "oops|bad pagetable|reboot is needed");
          [ "$n" = 0 ] && echo "dmesg: clean" ||
          echo "dmesg: $n fault lines - THE KERNEL IS DAMAGED, reboot before trusting anything"'
    ;;
stop)
    stop_listener
    $SSH "sudo sh -c 'echo 0 > $T/tracing_on'"
    echo "stopped"
    ;;
*)
    echo "usage: $0 {verify|arm [events...]|stream [seconds]|status|stop}" >&2
    exit 1
    ;;
esac
