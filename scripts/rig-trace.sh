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
# A hard ceiling on one capture. The interesting event sets run at tens
# of MB/s - the reference boot alone was 3.5 GB in 90 s - so an
# unattended 900 s stream would be tens of GB.
#
# 2 GB by default and 10 GB as an absolute ceiling, because these are
# written to
# **flash**, and a habit of leaving a verbose stream running costs write
# endurance for data nobody reads. Stop the stream as soon as the
# question is answered; do not leave one running "in case".
# Two ceilings, not one. The default is what a capture should cost; the
# hard limit is what it may never exceed however it is invoked, because
# ZPP_TRACE_MAX_BYTES is an override and an override with no bound is a
# footgun with a longer fuse.
MAX_BYTES=${ZPP_TRACE_MAX_BYTES:-2000000000}
HARD_MAX_BYTES=10000000000

if [ "$MAX_BYTES" -gt "$HARD_MAX_BYTES" ] 2>/dev/null; then
    echo "capping at the hard limit ${HARD_MAX_BYTES} bytes" \
         "(asked for ${MAX_BYTES})" >&2
    MAX_BYTES=$HARD_MAX_BYTES
fi
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
#
# Match on the **port** and on fd 0, not on the command line containing
# "trace_pipe". It does not: trace_pipe is a redirect, so the cmdline is
# just `nc -l -p 5555` and a grep for the path matches nothing. That left
# a previous run's listener alive holding trace_pipe, and the next
# attempt died with "Device or resource busy" while the guest was already
# booting - the capture came back empty and the run was wasted.
stop_listener()
{
    $SSH "for p in /proc/[0-9]*; do
            c=\$(cat \$p/comm 2>/dev/null) || continue
            case \"\$c\" in
                nc|timeout) ;;
                *) continue ;;
            esac
            if tr '\\0' ' ' < \$p/cmdline 2>/dev/null | grep -q -- '-p $PORT' ||
               [ \"\$(readlink \$p/fd/0 2>/dev/null)\" = $T/trace_pipe ]; then
                sudo kill \${p#/proc/} 2>/dev/null
            fi
          done
          sleep 1
          exit 0"
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

# Refuses a capture whose boot is not the one you think it is.
#
# Windows reasserts its own boot option at the front of BootOrder every
# time it completes a boot, so the firmware silently stops reaching our
# loader - and a capture taken then is a *control* run wearing the label
# of a test run. That has already invalidated one measurement and the
# retraction of it: both readings were taken from boots with no
# hypervisor, and both looked like evidence.
#
# Serial is the only thing that knows. Checked here rather than left to
# the caller, because the caller is the one who is already convinced.
require_hypervisor_booted()
{
    booted=$($SSH "grep -ac ZPP_TRACE /home/tc/zpp/serial.out" 2>/dev/null)
    option=$($SSH "grep -a 'starting Boot' /home/tc/zpp/serial.out | tail -1" 2>/dev/null)

    if [ "${booted:-0}" -eq 0 ] 2>/dev/null; then
        echo "REFUSING: no ZPP_TRACE on serial - the hypervisor did not run." >&2
        echo "  firmware started: $option" >&2
        echo "" >&2
        echo "A capture from this boot is a control run, not a test run." >&2
        echo "Windows reasserts Boot0004 at the front of BootOrder on" >&2
        echo "every completed boot; rewrite the varstore with one option" >&2
        echo "before measuring anything." >&2
        echo "" >&2
        echo "To capture a deliberate control run:" >&2
        echo "  ZPP_ALLOW_NO_HYPERVISOR=1 $0 stream ..." >&2
        exit 1
    fi
    echo "confirmed: hypervisor booted ($booted trace lines)"
}


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
    [ "${ZPP_ALLOW_NO_HYPERVISOR:-0}" = "1" ] || require_hypervisor_booted
    seconds=${1:-600}
    stop_listener
    start_listener "$seconds"
    echo "draining to $OUT (live; tail -f it), capped at $MAX_BYTES bytes"
    # Capped, because a verbose set is far louder than it looks: the
    # reference boot wrote 3.5 GB in 90 s, so a 900 s run at that rate
    # would be ~35 GB and a busier event set more. `head` exiting closes
    # the socket, nc sees EPIPE and the rig-side listener exits with it,
    # so the cap tears the whole path down rather than just truncating
    # here. The pipe is local - the corruption was the rig's kernel.
    nc "$RIG_HOST" "$PORT" | head -c "$MAX_BYTES" > "$OUT"
    echo "stream ended, $(wc -l < "$OUT") lines, $(du -h "$OUT" | cut -f1) in $OUT"
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
clean)
    # Captures are large and nobody reads an old one. Deleting is the
    # default state of this directory, not a chore - a stale multi
    # gigabyte /tmp/kvm.log on a laptop is the same waste as leaving the
    # stream running, one just costs disk instead of flash endurance.
    for f in "$OUT" "$OUT".*; do
        [ -f "$f" ] || continue
        echo "removing $f ($(du -h "$f" 2>/dev/null | cut -f1))"
        rm -f "$f"
    done
    ;;

stop)
    # Disarm as well as pause. tracing_on=0 stops the recording, and
    # leaving every event enabled behind it is how the next `status`
    # reads "armed" for a session nobody armed - which is exactly the
    # confusion the arm path checks against. Costs nothing to be
    # unambiguous.
    stop_listener
    $SSH "sudo sh -c 'echo 0 > $T/tracing_on'"
    $SSH "sudo sh -c 'echo > $T/set_event'"
    echo "stopped and disarmed"
    ;;
*)
    echo "usage: $0 {verify|arm [events...]|stream [seconds]|status|stop|clean}" >&2
    exit 1
    ;;
esac
