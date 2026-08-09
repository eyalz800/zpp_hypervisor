---
name: trace-kvm
description: Use when you need to see what a guest hypervisor asked KVM for on the TinyCore rig - IPIs, nested entry failures, MSR access - or when a KVM trace comes back empty, or when the rig pings but refuses ssh after a tracing attempt.
---

# Tracing KVM on the rig

The rig runs `6.12.11-zpptrace` with `kvm-trace.ko` / `kvm-intel-trace.ko`.
It answers what nothing else can: **what the guest hypervisor asked the
layer underneath for, and what that layer did.** That is how a broadcast
INIT-SIPI-SIPI was proven delivered to all seven application processors
while zpp was refusing it.

Two constraints hold at once and pull opposite ways:

- **Nothing may be stored on the rig.** Its `/tmp` is a RAM disk shared
  with a guest that wants nearly all of memory. `sched_switch` alone
  wrote 554 MB in fifteen seconds and left the host with 410 MB free.
- **No tracefs reader may be a child of an ssh session.** Dropbear forks
  a child per session; a reader blocked uninterruptibly on tracefs means
  that session never closes. Leak enough and dropbear refuses new
  connections — the machine pings, the QEMU monitor answers, and ssh
  times out during banner exchange. It looks exactly like sshd dying
  under load.

**A FIFO reconciles them.** It stores nothing. The process touching
tracefs is `setsid`-detached, so a blocked read takes no session with it.
The ssh session reads only the FIFO — an ordinary interruptible read.

## Recipe

```sh
# 1. Arm AS ROOT, tracing_on last. kvm.ko must already be loaded.
#    Each write is its own `sudo sh -c` with the path spelled out: a
#    shell variable does not survive into a sudo subshell, and a loop
#    that lost $T silently wrote to /events/... while the check below
#    still reported 1 from a previous arming.
ssh $RIG 'T=/sys/kernel/tracing
  [ -d $T/events ] || sudo mount -t tracefs nodev $T
  sudo sh -c "echo 0 > /sys/kernel/tracing/buffer_percent"
  for e in kvm_apic_ipi kvm_apic_accept_irq kvm_nested_vmenter_failed; do
    sudo sh -c "echo 1 > /sys/kernel/tracing/events/kvm/$e/enable"
  done
  sudo sh -c "echo 1 > /sys/kernel/tracing/tracing_on"
  echo "armed: $(sudo cat $T/events/kvm/kvm_apic_ipi/enable) \
    accept: $(sudo cat $T/events/kvm/kvm_apic_accept_irq/enable) \
    on: $(sudo cat $T/tracing_on)"'

# 2. All three must read 1. An empty answer means you read it
#    unprivileged. Check all of them - one reading 1 proves nothing about
#    the others, and may be left over from an earlier run.

# 3. Feed the FIFO, then stream it here.
ssh $RIG 'sudo vm/trace-stream.sh 120'
ssh $RIG 'timeout 130 cat /tmp/zpp-trace.fifo' > /tmp/kvm.log
```

## Traps

| Symptom | Cause |
|---|---|
| "armed: " with nothing after it | tracefs answers an **unprivileged read with an empty string**, not an error. Every control must be read with `sudo`. Five captures came back empty before this was noticed — the events were never enabled. |
| Trace empty, everything reports enabled | `tracing_on` reads 0. **Writing `buffer_size_kb` sets it back to 0** — set `tracing_on` last, after any resize and after the enables. |
| Trace empty, live reader, sparse events | `buffer_percent` defaults to 50: `trace_pipe` will not wake its reader until the buffer is half full. Set it to 0. |
| Reader gets `Device or resource busy` | `trace_pipe` admits one reader. A leftover one silently drains everything the next is waiting for. |
| Event enabled but drops everything | A filter that fails to parse leaves the event in an error state, still reporting enabled. ftrace has no bitwise predicate — `icr_low & 0x700 != 0` did this. Clear filters; streaming off-box removes the reason to filter. |
| Host OOMs, QEMU killed | `buffer_size_kb` is **per CPU**. 65536 on eight processors is 512 MB against a host left ~800 MB. |
| `events/kvm` missing | The tracepoints exist only while `kvm.ko` is loaded, and the launcher rmmods/insmods it — arm *after* its insmod. |
| ssh dies mid-capture | A reader was an ssh child. Never `ssh $RIG 'cat trace_pipe'`. |

## Verify before trusting a run

Enable `sched_switch`, stream five seconds, confirm bytes arrive, then
**turn it off** — far too noisy for anything else. A 15 s capture
produced 3,281,493 lines.

Afterwards check `ps -o stat | grep -c '^D'` is 0. Non-zero means a
reader wedged and its session is leaking.

## Never read `trace`

Only `trace_pipe`. Reading the static `trace` file has hung
uninterruptibly on this kernel, and `timeout` cannot kill a D-state
process.

## When it still comes back empty

Check in this order, all as root: `tracing_on`, the event `enable`, then
whether `kvm.ko` is loaded. Do **not** investigate with reads of `trace`,
a resize, or a second reader — that is what turns a dead trace into a
dead machine.

Falsified, so do not re-propose: resizing wedging the buffer;
`buffer_percent` alone; `timeout` signalling sudo rather than cat; and
"this kernel's ftrace does not record" — it records 3.2 M lines in 15 s.

The hypervisor's own log ring (see `read-resident-state`) is memory in
our module, read over gdb, and cannot wedge anything. Prefer it when it
can answer the question.
