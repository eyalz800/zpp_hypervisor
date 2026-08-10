---
name: trace-kvm
description: Use when you need to see what a guest hypervisor asked KVM for on the TinyCore rig - IPIs, nested entry failures, MSR access - or when a KVM trace comes back empty, or when the rig pings but refuses ssh after a tracing attempt.
---

# Tracing KVM on the rig

> **Read "The FIFO capture crashes this kernel" before running any of
> this.** The recipe below corrupts kernel memory on `6.12.11-zpptrace`
> and ends with the rig needing a reboot. It is kept because the events
> it names are still the right ones and nothing else answers what it
> answers - but taking a capture currently costs the machine, so decide
> that deliberately rather than by habit.

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

# 3. Feed the FIFO, then stream it here. START THIS BEFORE THE GUEST
#    BOOTS, and run it long enough to span what you are looking for.
ssh $RIG 'sudo vm/trace-stream.sh 300'
ssh $RIG 'timeout 310 cat /tmp/zpp-trace.fifo' > /tmp/kvm.log
```

**Start the capture before the boot, not after.** The events worth having
arrive minutes in - a guest hypervisor starts its application processors
long after the firmware has finished - and a capture armed at a fixed
sleep after boot samples an arbitrary window. Three captures came back
with nothing but end-of-interrupt traffic for exactly this reason, and
each cost a full boot to discover.

The ordering constraint that makes this awkward is real: `events/kvm`
only exists once `kvm.ko` is loaded, and the launcher reloads it. So arm
within the launcher (as `boot-ipi.sh` does), or arm immediately after the
insmod and start the stream before the firmware hands over.

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

## The FIFO capture crashes this kernel

**Measured, twice, with the kernel saying so itself.** The
`trace_pipe` → FIFO pipeline this skill recommends writes trace text over
kernel page tables on `6.12.11-zpptrace`. It is not a flaky capture; it is
memory corruption, and it ends the session.

The tell is **ASCII where a pointer belongs**. Decode the address before
believing it is an address:

```
Oops: Bad pagetable: 0009        PMD 2e2e2e2e2e205d31   ← "1] ....."
Oops: general protection fault, probably for non-canonical
      address 0x656363615f636970                        ← "pic_acce"
Fixing recursive fault but reboot is needed!
```

`2e2e2e2e2e205d31` is `1] .....` and `656363615f636970` is `pic_acce`,
the middle of `kvm_apic_accept_irq`. Both faulting tasks were `Comm: cat`
- the FIFO feeder and its reader - not QEMU and not `rmmod`.

What it looks like from outside, in the order it appears:

| What you see | What it actually is |
|---|---|
| `cat: read error: Bad address` | EFAULT from the corrupted read. The capture is over. |
| A capture holding only the *previous* run's events | The reader died in its first seconds; you drained the stale ring. |
| `tracing_on` reads 0 with every event still `enable=1` | `tracing_off()` from the oops path. Nothing disarmed it on purpose. |
| Zombie QEMU, ~12 GB pinned, `kvm` refcount stuck | The teardown afterwards, on a kernel that already said "reboot is needed". |

**So the zombie leak is caused by the capture, not by killing QEMU.** The
`boot-windows-rig` skill blamed the kill for two sessions and that was
wrong - the kill is just the last event before the symptom is noticed.
Both leaks had a capture running; the one clean teardown measured had
none.

Check `dmesg` before drawing any conclusion from a failed capture, and
before blaming a hypervisor bug for a hang:

```sh
ssh $RIG 'sudo dmesg | grep -iE "oops|bad pagetable|reboot is needed"'
```

**Until this is fixed, treat a KVM trace as costing the machine.** Take
the run without a capture unless the trace is the whole point of the run,
and expect to end the session when you do take one. A capture is not
worth spending a reboot on a question the hypervisor's own log ring can
answer - see `dump-log-physical`, which reads memory and cannot wedge
anything.

## Verify before trusting a run

Enable `sched_switch`, stream five seconds, confirm bytes arrive, then
**turn it off** — far too noisy for anything else. A 15 s capture
produced 3,281,493 lines.

Afterwards check `ps -o stat | grep -c '^D'` is 0. Non-zero means a
reader wedged and its session is leaking.

## Killing the guest: by PID, by process NAME

Use `scripts/rig-kill-qemu.sh`. Three rules it encodes, each of which cost
a run:

- **Never match processes by command line.** `pkill -f qemu-system` and
  `ps -o args | grep "[q]emu-system"` match anything whose *arguments*
  mention it. That includes the ssh command running the check and the
  launcher, which carries `ZPP_QEMU_EXTRA=...` in its environment. A
  `pkill -f boot-zpp.sh` killed its own ssh session mid-command, and the
  `ps` form made `ensure-traced-kvm.sh` refuse to load KVM - "REFUSING: 1
  qemu still running" - and the boot died with `Could not access KVM
  kernel module`. Match `/proc/<pid>/comm`, which is the process name and
  cannot match a mention.
- **Kill QEMU only, never the launcher.** `boot-zpp.sh` rebinds the NVMe
  and the GPU back from vfio-pci *after* QEMU exits. Kill the launcher and
  the devices stay with vfio-pci: no `/dev/nvme0n1p2`, so the next deploy
  fails and the next run cannot claim the devices.
- **TERM first, KILL second.** The zombie that holds 12-13 GB of pinned
  guest pages does not clear on its own and needs a reboot, and the script
  reports it explicitly because the next symptom is a launch that
  mysteriously cannot allocate guest memory. **It is not caused by the
  kill** - see "The FIFO capture crashes this kernel" above, and read
  `dmesg` before concluding otherwise. TERM first remains right on its own
  merits; it just does not buy immunity from this.

## The rig has no display of its own

There is **no `i915` module on this kernel** - `modprobe i915` answers
`not found in modules.dep` - so the host can never drive the passed
through GPU. The console is `(S) dummy device`. The screen shows
something only while a guest owns the GPU through VFIO; the moment the
guest resets, shuts down, or QEMU exits, it goes dark and stays dark.

So a black screen is not evidence about the guest. Ask the QEMU monitor
(`info status`) or read the hypervisor's own state - never infer from
the display.

## Reading resident state when no CPU is in our code

`add-symbol-file` plus `$h->member` only works while a CPU is *inside the
module*, and most of the time none is: the module hides itself, clearing
every EPT permission on its own pages, so from guest context those
addresses read as `Cannot access memory`. Attaching repeatedly and hoping
to land in root operation failed eight times out of eight.

**Use the QEMU monitor's `xp` instead.** It reads *physical* memory,
which bypasses EPT and guest paging entirely, and the module base printed
on serial *is* a physical address. Compute member offsets offline against
the ELF and read them live:

```sh
# offsets, from the ELF - expand a CU first or the class type is unknown
x86_64-elf-gdb -q -batch out/debug/x86_64/zpp_hypervisor \
  -ex "ptype zpp::hypervisor::hypervisor::on_l2_exit" \
  -ex "print/x (long)&((zpp::hypervisor::hypervisor *)0)->l2_entries"

# live, through the monitor: instance = base + symbol offset
printf 'xp/1gx 0x6a85ae20\n' | nc -w 6 <rig> 4446
```

`info status` distinguishes the two failures that look identical from
outside: `paused (shutdown)` means the guest stopped itself and every
counter is frozen at its final value - do not read a frozen counter as a
livelock, which is a mistake made once here.

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
