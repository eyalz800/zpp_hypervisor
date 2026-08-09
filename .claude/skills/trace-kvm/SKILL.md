---
name: trace-kvm
description: Capture KVM tracepoints from the rig live to the development machine, without wedging the host. Use when you need to see what the guest hypervisor asked KVM for - IPIs, nested entry failures, MSR access - and only after reading the traps here, because getting this wrong takes ssh down and costs a reboot.
---

# Tracing KVM on the rig

The rig runs a kernel built with tracepoints on (`6.12.11-zpptrace`) and
the traced modules `kvm-trace.ko` / `kvm-intel-trace.ko`. It answers one
question nothing else can: **what the guest hypervisor asked the layer
underneath for, and what that layer did about it.** That is how the
broadcast INIT-SIPI-SIPI was proven delivered to all seven application
processors while zpp was refusing it.

**Read all of "What goes wrong" before running anything.** Every item is
a measured failure from one session, and two of them cost a reboot each.
The rig cannot always be rebooted - nobody may be there to do it - so a
capture that wedges the host costs the rest of the session, not just the
trace.

## It works. What was actually wrong

**The events were never enabled.** Every "armed: ipi=1" that was trusted
came from reading the control files as the unprivileged user `tc`, and
tracefs returns an **empty string** rather than an error there - so the
arming report was reporting on nothing, and `kvm_apic_ipi` and even
`sched_switch` sat at `0` while five capture attempts recorded exactly
what was enabled: nothing. `boot-zpp.sh` also never arms at all, unlike
`boot-ipi.sh`.

Read every tracefs control with `sudo`. An empty answer is not "off", it
is "you could not read it".

Once armed as root, a fifteen second capture produced **3,281,493 lines**
with ssh untouched and no D-state process. Use
`vm/trace-capture.sh <seconds> <output>`, then collect the file with an
ordinary read - a regular file cannot block the way a pipe can.

Four hypotheses were tested and are **wrong**; do not re-propose them:

- *"Resizing `buffer_size_kb` wedges it"* - captures with the buffer
  never resized produced nothing either.
- *"`buffer_percent` at 50 holds the reader"* - setting it to 0 changed
  nothing on its own, though it is still worth setting.
- *"`timeout` kills sudo rather than cat, losing buffered data"* -
  bounding inside sudo, and by bytes with `head -c`, both produced
  nothing.
- *"This kernel's ftrace does not record"* - it records 3.2 million lines
  in fifteen seconds.

One real trap was found along the way and still applies: **writing
`buffer_size_kb` sets `tracing_on` back to 0**, so `tracing_on` must be
set last, after the resize and after the event enables.

## What goes wrong

- **`buffer_percent` defaults to 50.** `trace_pipe` does not wake its
  reader until the buffer is that fraction full. With a 4 MB per-CPU
  buffer and a trickle of events, a live reader sees *nothing at all*
  while everything reports armed and enabled. Set it to `0`.

- **Reading `trace` can block uninterruptibly.** Observed on a fresh boot
  with no other reader: `timeout 5 sudo head -4 .../trace` never
  returned, `timeout` could not kill it, and the process stayed in D
  state. Every later tracing read piles up behind it, and so does every
  ssh session that runs one. **Never read `trace`. Use `trace_pipe`
  only.**

- **A blocked reader leaks an ssh session, and that is what kills the
  rig.** This is the single most expensive thing in this file. Dropbear
  forks a child per session, so `ssh $RIG 'cat trace_pipe'` makes the
  reader a child of that session; if the read blocks uninterruptibly the
  session never closes. Each attempt leaks one, and dropbear caps
  concurrent sessions - past the cap, new connections are first slow
  ("timed out during banner exchange") and then **refused outright**.
  The host still pings and the QEMU monitor still answers, which is what
  makes it look like sshd dying under load. It is not CPU starvation and
  not the OOM killer: an OOM kill leaves dead processes, not unkillable
  ones.

  So **every remote reader must be bounded on the remote side**, not by
  the local `timeout`, which only kills the local ssh and leaves the
  child:

      ssh -n $RIG 'timeout 120 sudo cat /sys/kernel/tracing/trace_pipe' > /tmp/kvm.log

  and re-issued in a loop if a longer capture is needed. A bounded reader
  that returns nothing costs one session; an unbounded one costs the
  machine.

- **Do not write `buffer_size_kb`.** It is the one action that preceded
  both hangs. It is also per CPU: `65536` on an eight-processor machine
  is 512 MB, against a host left ~800 MB after the guest takes
  `MemTotal - 800`, and that one *did* invoke the OOM killer and took
  QEMU with it.

- **`trace_pipe` admits one reader.** A second gets `Device or resource
  busy`. Worse, a *leftover* reader silently drains everything the next
  one is waiting for - a forgotten `cat` on the target made the ring read
  0/0 and looked exactly like "the guest never did it".

- **tracefs is not mounted on every boot.** Unmounted,
  `/sys/kernel/tracing` is an ordinary empty directory: every arming
  write vanishes into `2>/dev/null` and the trace is empty for a reason
  nothing reports. Check `[ -d /sys/kernel/tracing/events ]`.

- **A filter that fails to parse drops everything.** It does not fall
  back to unfiltered - the event is left in an error state, still
  reporting enabled. `icr_low & 0x700 != 0` did this; ftrace has no
  bitwise predicate. Read the filter back after writing it, or do not
  filter at all. Streaming to the development machine removes the reason
  to filter.

- **The kvm events only exist while `kvm.ko` is loaded.** The launcher
  `rmmod`s and `insmod`s them, which destroys `events/kvm` and clears
  every enable, so arming must happen *after* its insmod - which is why
  it lives inside the launcher rather than beside it.

- **`ps | grep trace_pipe` matches your own command line.** Misread as a
  live reader twice in one session. Same trap as `pkill -f qemu-system`,
  which killed the ssh session issuing it.

- **The output is a RAM disk and the events are not rate limited.**
  `sched_switch` alone wrote 554 MB in fifteen seconds and left the host
  with 410 MB free - which is how a trace becomes an OOM kill that takes
  QEMU with it. Enable only the KVM events wanted, and keep the size
  bound in `trace-capture.sh`. Never enable `sched_switch` for anything
  but a one-shot proof that recording works.

## The recipe

Do the risky part **last**, after the session's real work, because a
wedge costs the machine until somebody can reboot it.

```sh
# 1. Once, with nothing reading. Never again while a reader is attached.
ssh $RIG 'T=/sys/kernel/tracing
  [ -d $T/events ] || sudo mount -t tracefs nodev $T
  sudo sh -c "echo 0 > $T/buffer_percent"'

# 2. The launcher arms after its insmod - see vm/arm-ipi-trace.sh, which
#    clears filters rather than setting them and never resizes.

# 3. Prove the mechanism records at all, with a producer of our own and a
#    reader bounded ON THE REMOTE. Five seconds, one session at risk.
ssh -n $RIG 'sudo sh -c "echo 1 > /sys/kernel/tracing/events/sched/sched_switch/enable"'
ssh -n $RIG 'timeout 5 sudo cat /sys/kernel/tracing/trace_pipe' > /tmp/probe.txt
#    Empty? STOP. Nothing below will help, and each further attempt
#    leaks another ssh session.

# 4. Only then, the capture - still bounded, re-issued in a loop for a
#    long run. Never to the rig's disk: /home there is a RAM disk.
while ssh -n $RIG 'timeout 120 sudo cat /sys/kernel/tracing/trace_pipe' >> /tmp/kvm.log
do :; done &

# 5. Boot, and leave it alone.
```

## When it fails

Stop. Do not read `trace`, do not resize, do not start a second reader.
The whole cost of this skill has been investigating a dead trace with the
one operation that kills the host.

The alternative is usually better anyway: the hypervisor's own log ring
(see `read-resident-state`) is memory in our module, read over gdb, and
cannot wedge anything. Every finding of the session that produced this
skill came from it - the page-granular offset, the shadow EPT staleness,
the CR0 exit, the start-up IPI sequence - and the KVM trace contributed
one fact in a whole day.
