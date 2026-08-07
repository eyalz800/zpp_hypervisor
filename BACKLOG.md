# Known defects

Everything here was found by reading the tree or by measuring a running
system, and each entry says which. That distinction is the point of the
document: a defect that has been *seen* justifies a fix on its own, and a
defect that has only been *read* needs its trigger established before the
fix can be trusted.

Recorded at `7174405`, the build deployed for the bare-metal Windows test.
The four fixes already in that build — the hypervisor-present bit cleared,
the MTRR array bounded, `INVD` no longer executed, the first MWAIT logged —
are not repeated here.

The convention for closing an entry: state what was observed afterwards, not
that the code changed. "Guest now reads `cr4=0x0668`" closes one of these;
"masked CR4" does not.

## A hang after the chainload line: what it is, and what it is not

Read this before investigating one, because it cost a day.

**The variable is whether Windows enters its recovery flow.** After a clean
Windows shutdown the loader chainloads and Windows boots - from the boot
order and from the setup menu's boot override alike. Once Windows has
recorded a failed boot, the next attempt takes the recovery path and stops
after the `chainloading` trace line with nothing further drawn: no Windows
output, no bugcheck, no crash dump. Enter sometimes gets through it,
because the `0xc0000001` recovery prompt is *there and waiting for a
keypress*; whether it is drawn is a separate matter from whether it is
running.

Two wrong conclusions were reached and abandoned along the way, both of
them from too few samples of a state that alternates on Windows' own
boot-failure record:

- That it was intermittent. It is not; it is state-dependent.
- That it was deterministic in the launch method - boot override broken,
  boot order fine. It is not. Both work from a clean shutdown and both fail
  from a recovery state. The launch method is now recorded in the trace
  (`BootCurrent`, and its absence) so no future log can be read without
  knowing it, which is the mistake that made this take so long.

Because Windows records a failed boot when it bugchecks, a live bugcheck
keeps re-arming the recovery flow, and the recovery flow then looks like
the defect. Check for a fresh crash dump before believing anything else.

Three things that produce this exact symptom, all worth eliminating before
reaching for a new theory:

- **A `ZPP_VERIFY_HYPERVISOR` build.** It halts every application processor
  in real mode and leaves the boot processor's APIC in x2APIC mode where the
  firmware's xAPIC accesses cannot see it. Silent stop after `chainloading`,
  every time. It must never be deployed to a machine meant to keep booting,
  and note the trap: `-DZPP_VERIFY_HYPERVISOR=ON` persists in the CMake
  cache, so a later plain `cmake --build` inherits it silently. Check the
  built binary for the self-check's strings rather than trusting the build
  command.
- **The recovery flow above.**
- **Item 13 below**, which is on the one path the recovery flow takes and
  the fast path does not.

**Hibernation is disabled on the development target**, measured from the
volume itself: no `hiberfil.sys` at the root, while `pagefile.sys` and
`swapfile.sys` are both present. So fast startup never writes an image
there, a clean shutdown is a real shutdown rather than a resume, and no
boot of that machine has ever been a hibernation resume.

That retires a theory rather than supporting one. The S4 mechanism - a
resume restoring a memory image over frames this VMM had taken and hidden
in EPT - cannot have been happening on this machine, so it explains none of
what was observed here. Item 14 remains a real defect against the
requirement it cites, and it is worth keeping for machines where fast
startup is on, but the improvement seen after fixing it was most likely the
recovery state being cleared by the clean shutdown that preceded it, not
the fix. Two variables, one boot, again.

**The bugcheck has not recurred.** `\WINDOWS\Minidump` holds three dumps
from 20:41, 20:49 and 20:51, and `MEMORY.DMP` from 20:51 - all of them
before the build that clears the hypervisor-present bit was deployed at
22:51. Several Windows boots between then and 00:25 produced no new dump.
Tentative rather than settled: how long Windows was left idle on those
boots is not recorded, and the idle path is what provoked it.

With bugchecks gone and hibernation absent, the loop sustains itself
without either: a boot that hangs is a failed boot, a failed boot arms the
recovery flow, and the recovery flow hangs. A clean shutdown is what breaks
it.

Ruled out by measurement, so that nobody spends a boot on them again: the
module's load address (`0x8916x000` in six logs across four builds, moving
only by the page the loader itself grew, and `start up memory` at
`0x9b000` without exception); the firmware's own view of the processors
(all eight `enabled healthy`, identical before this loader runs and
immediately before the hand-over); and the zpp boot option itself
(`LOAD_OPTION_ACTIVE`, same attribute word as Windows Boot Manager's, same
GPT partition signature, no `LOAD_OPTION_CATEGORY_APP`).

## Measured

These were observed in real state. They are not inferences.

### 1. CR4.VMXE is visible to the guest — FIXED, NOT YET RUN

The guest reads `cr4 = 0x2668`, VMXE set, while CPUID leaf 1 ECX[5] reports
no VMX. That combination exists nowhere in hardware, and it is exactly the
kind of half-answered interface described under *What the guest is told* in
`CLAUDE.md`: a guest that trusts CR4 over CPUID concludes VMX is available
and takes a `#GP` on its own `vmxon`.

The cause was narrower than the entry suggested. `cr4_read_shadow` was
already being written with the guest's own CR4 - but `cr4_guest_host_mask`
was never written at all, and a shadow only answers for the bits the mask
selects. With the mask at its default of zero every bit came from the real
register and the shadow was dead code.

The mask now selects VMXE, so the guest reads that bit from the shadow
with it clear, and a write to it exits instead of reaching the register.
Exit reason 28 is handled: the guest's value goes to the shadow, and the
real register keeps VMXE, without which the next entry fails - VMXE is
required to be set by `IA32_VMX_CR4_FIXED0`.

**Not yet run.** It cannot be exercised locally, since the hypervisor
needs VT-x and the local emulator has none. Closing this means observing
the guest read `0x0668` on the rig.

### 2. A start-up IPI is swallowed, bare metal only

The sending and receiving sides disagree about which hand-off mechanism is
in use, so the sender consumes a SIPI that the receiver is waiting to
receive from hardware. Reproducing it needs the guest to re-start a
processor this VMM has already adopted, which is why it does not appear
under emulation.

## Structural

Read in the code. Each is a real divergence from what the architecture
requires, but none has been provoked yet — so the first job on each is to
establish the trigger, and only then to fix it.

### 3. `guest_fs_base` is taken from the GDT — FIXED

`hypervisor/src/hypervisor/hypervisor.cpp:1768` writes
`descriptor.context_dependent_base()`. In long mode the FS base does not
live in the descriptor; it lives in `IA32_FS_BASE`, which is already read
into a member at `:46` and then never used for this. A guest whose FS base
does not fit in a descriptor's 32 bits gets the truncated value.

### 4. `invept` is never called — CLOSED

Declared with zero call sites. Any change to an EPT entry after launch
leaves the old translation cached in the combined mappings.

"Today nothing modifies EPT after launch" stopped being true when page
watches arrived: arming one clears the write bit on a live entry, and
holding, stepping and disarming set and clear it again. Single-context
invalidation now follows every one of them.

One limitation remains and is deliberate: INVEPT is not a broadcast, so
this covers the calling processor only. Fixing that needs the rendezvous
item 10 describes. It is safe in the direction that matters - a stale
permissive entry costs a missed observation, never a wrong one.

### 5. `ia_32e_mode_guest` is set once and never re-derived — NOT A DEFECT

Written from the state at launch, cleared for the real-mode start-up
path, and never revisited - which looks stale, and is not, because
hardware maintains it.

SDM, `IA32_VMX_MISC` bit 5: "every VM exit stores the value of
IA32_EFER.LMA into the 'IA-32e mode guest' VM-entry control", and "This
bit is read as 1 on any logical processor that supports the 1-setting of
the 'unrestricted guest' VM-execution control".

This VMM enables unrestricted guest - it has to, since it starts
application processors in real mode - so the bit is set and every exit
re-derives the control from the guest's own EFER. Writing it once at
launch is therefore correct, and a guest switching in and out of long
mode is handled without this VMM doing anything.

Worth keeping rather than deleting: the entry looked obviously true, and
checking it against the specification is the only reason it was not
"fixed" into something that does the same thing by hand.

### 6. xAPIC MMIO ICR accesses are not intercepted — FIXED, NOT YET RUN

Only the x2APIC MSR path reached the exit handler. A guest in xAPIC mode
writes the command through the APIC page, which is a store rather than an
instruction the MSR bitmap can be told about, so those IPIs were invisible.

Now caught with the page watch: the page is write protected, the guest's
own store is stepped over, and the command is read back out of the page
afterwards - so no instruction decoder is involved. The two dwords of the
xAPIC form are composed into the shape the x2APIC path already produces,
and both go through one decision.

Armed only while the guest is in xAPIC mode, because the page is hot -
the end of interrupt register is in it and is written on every interrupt.
A write leaving no command pending is ignored, which is almost all of
them.

**Not yet run.** Needs VT-x, which the local emulator does not have, and
it needs a guest that actually uses xAPIC to exercise the interesting
path at all.

### 7. S3/S4 leaves the machine unvirtualized

There is no resume path, so a suspend takes the hypervisor away and the
guest keeps running on bare hardware without being told.

### 8. Debug and microcode state is unmanaged — HALF FIXED

**Debug state: fixed.** `guest_dr7` and `guest_ia32_debugctl` were being
written into the VMCS while neither the "load debug controls" VM-entry
control nor the "save debug controls" VM-exit control was requested -
`adjust_msr` only forces bits the capability MSR requires, and with the
true controls those are allowed to be zero. So both fields were written
and never read, in the same way the CR4 read shadow was inert without its
mask.

The consequence is not that the guest's debug state was stale; it is that
it was destroyed. SDM 28.5.1 sets DR7 to 400H on every VM exit
regardless, so a guest using hardware breakpoints had them cleared out
from under it on the next exit. Both controls are requested now, which
makes the two fields live.

**Microcode: examined, deliberately unchanged.** MSR `0x79` is inside
`0`-`0x1fff`, so the all-zero bitmap governs it and it does not exit at
all - it is a genuine pass-through to hardware rather than a
fall-through to a default path, which is what the entry supposed.

Passing it through is also the right answer. A microcode load cannot be
emulated, refusing it would break an update the guest legitimately
applies at boot, and letting it reach hardware is exactly what would
happen without this VMM present. The hazard worth stating is that an
update applied while resident can change VMX behaviour underneath us,
and nothing here would notice.

### 9. `vmxoff` without `vmclear` on one failure path — NOT REPRODUCIBLE

Re-checked against the current tree, and neither guard can fire with a
VMCS current. Both were read rather than assumed:

- The guard inside `enter_root_mode` is released the instant `vmptrld`
  succeeds. It therefore only ever fires because `vmclear` or `vmptrld`
  failed, and in both of those cases no VMCS is current.
- The guard in `main` has no `return` between it and `vm_launch`, and
  `vm_launch` does not return. With `-fno-exceptions` there is no unwind
  path at all, so it is unreachable.

Left open as a **trap rather than a defect**: the second guard is
correct only because nothing returns past it. Adding a `return` between
it and the launch would silently make it wrong, and the symptom -
a VMCS left current across `vmxoff`, in an implementation specific
state - would not point back here. Anyone adding an early return there
should give the guard a `vmclear` first.

## Concurrency

Found by audit, not by a crash. The comment at
`hypervisor/include/zpp/hypervisor/hypervisor.h:71` is what makes these
worth taking seriously — see item 12.

### 10. Five unsynchronised or unbounded paths

- `start_up_lock` does not cover `next_virtual_processor` — **the
  overrun is closed, the race is latent**. It indexes `vmx`,
  `vmx_vmcs`, `intermediate_gdt` and `guest_tss`, all of `max_cpus`, and
  nothing bounded it. It is bounded now, but indirectly and worth
  writing down: `main` is reachable only through `launch_on_cpu`, which
  refuses once `available_stack_index` reaches `max_cpus`, and the two
  counters advance together at one per launched processor. So the fix
  for the stack index closed this as well.

  What remains is the unsynchronised read: each processor takes its
  index by reading the shared counter and incrementing it at the end of
  `main`, so two launching concurrently would take the same one. That
  cannot happen on any current loader - the Windows and Linux ones block
  per processor, and under UEFI only the boot processor is launched -
  which is the same coincidence item 12 documents, and it is a
  coincidence rather than a design.
- The trampoline timeout races the processor it is timing out.
- The slot index and the VPID can diverge, so per-CPU state can be read
  through the wrong index.
- The host-exception recovery slot is shared between processors, so a
  second fault overwrites the first one's record.
- ~~`available_stack_index++` is unbounded~~ — **CLOSED**. It refused
  nothing and wrote a 512 KB stack past the end of the array on any
  machine with more processors than `max_cpus`. Bounded now, and the
  ceiling raised from 16 to 32, which current laptops exceed.

### 15. Per-processor storage is sized for the ceiling, not the machine

`stack[max_cpus][512 KB]` is 16 MB, `intermediate_gdt[max_cpus][0x2000]`
is 2 MB, and both are dimensioned on `max_cpus` rather than on the
processors a machine actually has. Raising `max_cpus` from 16 to 32
therefore cost 8 MB on every machine, including the ones with four
processors.

The loader knows the real count - `number_of_cpus()` - and there is now
a launch structure to pass it through, so these could be sized on it.

**Not obviously worth doing.** It is a change to a working boot path for
a gain nobody has felt: no allocation has failed, and the whole image is
`.bss`, so the cost is address space rather than anything scarce. It
also carries an ordering constraint, since the stacks must exist before
any processor launches. Recorded so the trade is visible, not because it
is queued.

## Dead and misleading

These cost nothing at runtime and cost time during every future
investigation, which is the argument for fixing them first rather than
last.

### 11. `launch_error[]` is never written — FIXED

Read where its low nibble is folded into the diagnostic CPUID leaf, and
assigned nowhere - so the nibble was always zero. A diagnostic that
reports a constant is worse than one that does not exist, because it is
trusted.

Written now in `launch_on_cpu_private_stack`, which is where a failure
and the processor it belongs to are both in scope, and which every
processor passes through whether it was launched by the loader or
started by this VMM. The index is the one `main` was given, so it
matches what the leaf reads back with.

### 12. Two comments assert a serialisation that no longer holds — CLOSED

Both said the loader launches CPUs strictly one at a time. That stopped
being true, and it was not merely stale prose: the first was the stated
justification for `-fno-threadsafe-statics` being safe on the
function-local static in `hypervisor::instance()`.

Both now carry the real reason instead.

For `instance()` it is structural and platform independent: the function
has exactly three callers, and two of them are only *reachable* once the
boot processor has armed them from inside `main` - the host IDT it loads,
and the trampoline page that carries `zpp_ap_start_up_main`'s address.
Unlike the claim it replaces, that one is checkable by grep.

For the host exception recovery slot the conclusion held but by
coincidence of two unrelated facts: the Windows and Linux loaders really
do block per processor, while under UEFI `number_of_cpus()` returns 1 so
only the boot processor is ever launched. A loader that launched
concurrently would break it, which is item 10.

## Closed

### 13. EPT gave device memory a write-back type — CLOSED

**What was observed afterwards:** the `0xc0000001` recovery screen *draws*.
Three or four boots into the case that had reliably stopped after the
`chainloading` line produced no reproduction; the recovery prompt rendered,
Enter went through, and Windows booted. Before, that prompt was running and
waiting for a keypress the whole time and simply never appeared - which is
what made a live boot manager look like a hang for a day.

The defect: under EPT, hardware ignores the guest's MTRRs -
`.references/sdm.txt:206176`, "The MTRRs have no effect on the memory type
used for an access to a guest-physical address" - so a VMM has to fold them
into the EPT itself. `initialize_ept` folded in the **variable** MTRRs only
and gave every uncovered range write-back. Firmware marks MMIO uncacheable
*through* MTRRs while its page tables sit on the default PAT entry, which is
also write-back, and combining the two per Table 14-7 yields write-back
MMIO: device register writes landing in cache, reads returning stale data.

Now the default type comes from `IA32_MTRR_DEF_TYPE`, the fixed-range MTRRs
govern the first megabyte, overlaps follow the precedence rules in SDM
14.11.4.1, and a region whose type is not uniform is split to 4 KB. MMIO
above the top of DRAM and the legacy VGA aperture both went from WB to UC.

Why it presented as it did: the recovery flow reprograms the display
controller and the ordinary fast boot path does not, which is why a machine
that booted Windows perfectly well could not draw a recovery prompt.

Not proof - three or four boots without a reproduction is not the same as a
demonstration, and the failure was state-dependent. But the code defect, its
mechanism and the behaviour change all agree, which none of the earlier
theories managed.

### 14. Module memory was described as runtime services code

`allocate_rwx` used `EfiRuntimeServicesCode`, which carries
`EFI_MEMORY_RUNTIME`. Microsoft's UEFI firmware requirements say of the S4
transition that "firmware runtime memory must be consistent across S4 sleep
state transitions, in both size and location", and exclude
`AddressRangeReserved` from that rule and from the matching one for
operating system physical memory. A hibernation image captured without this
loader and resumed with it would therefore see runtime memory differing in
both size and location. Now `EfiReservedMemoryType`, which is also the
honest description: nothing calls into this module through the runtime
services at an address the operating system assigned.

**Fixed but not confirmed here, and that is worth stating plainly.**
Hibernation is disabled on the development target, so no boot of it has ever
been a resume and this could not have been contributing to anything observed.
It was briefly credited with an improvement that a preceding clean shutdown
had almost certainly caused.

## Not a defect, but unlanded

The hypervisor's own log does not survive a reboot, so a bare-metal failure
that kills the machine takes its log with it. Work exists on
`worktree-agent-a6c772e3a57c8bba2` (`hypervisor/include/zpp/crash_log.h`).

Largely superseded: `feat/screen-halt-output` draws the log to the
framebuffer from the halt paths, so it no longer has to survive a power
cycle - it is read off the screen before the machine is powered off.
Verified under emulation by reading the glyphs back out of framebuffer
memory, including a line drawn by `on_unhandled_exit` itself.

## The disk channel works, for the window it covers

Confirmed on the real machine, real controller, real disk: the resident
hypervisor completed a log write while Windows was booting, and the block is
on the medium and readable from another machine with `dd` and
`scripts/read-disk-log.py`.

    8 blocks read
        7 stamped by the reservation, never written
        1 written by the hypervisor
      seq 0, block 0, epoch 1, 1 record, nothing lost or dropped

The block carries the log header *and* the destination signature, so it
remains a legal destination for the next write rather than becoming a hole in
the region.

What that window is, precisely: from the hand-over until Windows' storage
driver initialises the controller. The reset detection below fires at that
point - `lost_to_reset` read 1 on the same boot, which is the write command
still outstanding when the reset landed - and the channel goes quiet
deliberately from then on. Extending it past that is the next piece of work.

## Re-creating the queue after the guest's reset: the design, measured

The channel stops when the guest's driver resets the controller. Getting it
back means one borrow of the guest's admin queue, and the shape of that
borrow was decided by measurement rather than argument.

**What a borrow costs.** Any whole number of laps is a legal borrow, so the
cost was swept against the real controller at three lengths: 4 commands in
67 us, 32 in 526 us, 256 in 4,264 us. Linear, ~30,000 ticks (16.6 us) per
command, no measurable fixed cost. A guest's admin queue is 256 deep, so its
lap is `lcm(256, 512)` = 512 commands:

    512 x 30,043 ticks = 15.4M ticks = 8.5 ms

**So the guest cannot be stopped for it.** `admin_borrow::run` says "the
guest must be stopped", and that is a *sufficient* condition stated for
simplicity, not a necessary one. What is necessary is only that nothing else
touches the admin queue: no submission to its SQ, no consumption from its CQ.

**Do it inside the guest's own controller-enable wait.** When a driver writes
`CC.EN=1` it must then poll `CSTS.RDY`, and the architecture allows the
controller up to `CAP.TO x 500 ms` to answer - a wait every driver already
tolerates. We trap that write today for the reset detection. At that instant
the admin queue has just been reset, so head and tail are zero and nothing of
the guest's is outstanding, and the driver's initialisation path is single
threaded. Holding that one write for 8.5 ms is indistinguishable from a
slightly slow controller, costs no other processor anything, and needs no
rendezvous, no doorbell trapping and no interrupt masking.

**The blocker is not the borrow, it is addressing the guest's queue.** The
admin queue lives at whatever physical address `ASQ`/`ACQ` name, and the host
page table cannot currently map an arbitrary one. It has two page directories
for 512 pdpt slots, so the directory is chosen by address bit 38 alone, and
`detail/page_table.h` says what follows: two addresses agreeing in bit 38 and
bits 29:12 land on the same leaf however far apart they are, the second
mapping silently replaces the first, and nothing detects it. Mapping the
guest's queue could therefore unmap the module out from under the VMM.

That also means the mappings added for the channel - the controller's BAR and
the queue storage - are collision free by luck rather than by construction:

    module         0x78ddd000   bit38=0  bits29:12=0x38ddd
    queue storage  0x7f72b000   bit38=0  bits29:12=0x3f72b
    nvme BAR0    0x7011108000   bit38=1  bits29:12=0x11108

**What to build first**, and it is small: a temporary mapping window. One
reserved virtual address, pointed at whatever physical page is needed,
`invlpg`d, used, and released - the classic technique, needing no extra
tables and no growth of a structure whose size is fixed at compile time. With
that, the borrow is a caller away.

## Making the rebuild race free, which polling does not

The rebuild is written and switched off, and the reason is a race rather
than a missing piece. Worth stating exactly, because two plausible fixes do
not fix it.

The borrow needs the guest's admin queue to itself. The driver cannot submit
to it until `CSTS.RDY` is set - but neither can we, because our own commands
need a ready controller. **Both become eligible at the same instant**, and a
borrow takes 8.5 ms, so it is guaranteed to overlap the driver's first
`Identify` unless the driver is actually prevented from submitting.

What does not fix it:

- **Catching the `CC.EN` write.** A watch lets the write land by making the
  page writable and stepping one instruction, and for that window the page is
  writable for every processor. Measured: one trapped write of `0x00460000`,
  the controller afterwards reading `0x00460001`, the transition never seen.
- **Polling the register more often.** The VMX-preemption timer can
  manufacture an exit every few microseconds whatever the guest is doing,
  which does solve "nothing makes us look" - the driver's `CSTS.RDY` polling
  is memory mapped reads to a passed through device and causes no exits at
  all. But noticing promptly is not exclusion. Measured without it: a borrow
  that spent its whole 285 ms budget and returned `timed_out`, leaving the
  guest's admin queue desynchronised.

What would fix it, and neither is free:

- **Hold the guest's admin doorbell.** Write-protect the doorbell page and
  park any processor that rings it until the borrow finishes, then let it
  through - no decoder needed, and no processor stopped that was not trying
  to use the queue. For it to be *strictly* race free every processor has to
  have picked the protection up before the borrow starts, and "each catches
  up on its next exit" is not that. It needs an interprocessor interrupt and
  an acknowledgement, which is what KVM's `kvm_flush_remote_tlbs` does and
  this VMM has no way to send.
- **Hide `CSTS.RDY` until the borrow is done.** Then the driver cannot
  legally proceed, because it is required to wait. This needs the guest's
  memory mapped *reads* emulated, which is where an instruction decoder
  becomes unavoidable.

The doorbell hold is the smaller of the two and needs no decoder, and it
turns out to need no interprocessor interrupt either.

**Why none can be sent, and why that does not matter.** This VMM leaves
"external-interrupt exiting" clear, so an interrupt it sent to another
processor would be delivered into the guest's own handler and cause no VM
exit at all - SDM 28-7, "otherwise, the processor handles the interrupt
normally". Setting that control means intercepting and re-injecting every
interrupt the guest receives, which is a great deal of machinery for a
hypervisor that deliberately leaves interrupts alone.

But the interrupt was never the requirement. The requirement is *knowing*
that every processor has picked the protection up, and that is observable
without sending anything: each stamps `ept_generation_seen` on its own next
exit, and a guest that is running takes exits constantly.
`wait_for_ept_acknowledgement` waits for exactly that, skips processors that
never launched, and gives up rather than guessing. A caller that does not get
the acknowledgement does nothing - not borrowing is always safe, borrowing
without exclusion is not.

The doorbell hold is now written, and the acknowledgement is where it stops.
Measured on the machine, with the wait instrumented after three wrong
guesses:

    ack_target            0x31a   the generation to reach
    ack_launched_mask     0xff    all eight processors launched
    ack_outstanding_cpu   1
    ack_outstanding_seen  0x317   three generations behind, and staying there

**Processor 1 never takes an exit.** The guest has halted it, this VMM does
not set "HLT exiting", and a halted processor executes nothing - so it never
reaches the exit path where the high water mark is stamped. Waiting for it
waits forever.

That is what the interprocessor interrupt is actually for, and the earlier
claim here that it was unnecessary was wrong. Observing the acknowledgement
is sufficient for a processor that is *running*; it says nothing about one
that is idle, and idle is the common case. The interrupt is not how KVM
informs the others, it is how KVM **forces them out**.

Two lighter ways to get that than taking over the guest's whole interrupt
path:

- **NMI exiting** (pin-based control bit 3) plus an NMI sent through the
  local APIC's command register, which this VMM can already reach. A
  processor taking an NMI exits whatever state it is in, including halted.
  The cost is having to recognise and re-inject a genuine guest NMI, which
  is rare but not optional.
- **HLT exiting**, which would at least let a processor stamp on its way
  into the halt. Cheaper, and not sufficient on its own: a processor that
  halted *before* the change still wakes holding a stale translation.

**The wake works, and the borrow still fails.** With the interrupt sent as a
probe the exclusion is satisfied and the borrow runs, and it returns
`timed_out` after 1.22 seconds:

    wake_nmis_sent           7
    unresponsive_processors  7
    channel_rebuild_result   4      timed_out
    channel_rebuild_ticks    0x82e6a48b

Seven of seven not answering is correct rather than suspicious: this happens
during the guest's storage driver initialisation, when Windows is still
single threaded and every application processor is parked in wait-for-SIPI,
where the SDM says an NMI is neither delivered nor causes an exit.

So the remaining fault is in the borrow itself, and it is a new one rather
than a variant of the exclusion problem. `AQA` reads `0x00010001` - two
entries each, zero based - which gives a lap of four commands, the same
geometry the loader borrows against successfully. A four command borrow that
spends 1.22 seconds is not reaching the controller at all. The likely
suspects, in order: the doorbell stride derived from `CAP`, the window
mapping of the guest's queues, and whether `CSTS.RDY` was genuinely observed
rather than read from an unmapped address. None of them is guessed at here
because the last three guesses were all wrong and all cost a boot.

Three wrong guesses preceded the measurement, and they are worth listing so
they are not made again: the initiator waiting for its own stale stamp
(real, fixed), holding before acknowledging and deadlocking a held processor
that can no longer stamp (real, fixed), and demanding equality with a
generation the hot local APIC watch moves constantly (real, fixed with a
monotone "at least" compare). None of them was the cause. The cause was a
processor that was not running at all.

## The guest resets the controller and takes our queue with it

The disk channel's queue pair is created by the loader, before Windows'
storage driver has touched the controller. That driver then initialises the
device the way any driver does - which includes taking `CC.EN` down and back
up - and a controller reset destroys every I/O queue on it, ours included.
After that the doorbell we hold is not backed by anything.

Two things follow, and only the first is handled:

- **Writing after the reset is refused rather than attempted.** Every write
  does a guard read first and compares an epoch, and `queue_pair` counts what
  it threw away in `lost_to_reset` so the loss is visible in the next block's
  header rather than silent. That much is written and works.
- **Nothing notices the reset, so the epoch never changes.** `esp_blocks.h`
  has said so all along - "bumped by the reset detection, which is the second
  wiring point and also absent". The guard reads `CSTS.RDY` and `CC.EN` and a
  *completed* reset leaves both exactly as they were before it, so the guard
  cannot tell. It catches a controller that is down; it cannot catch one that
  went down and came back.

So the channel is live from the hand-over until Windows' driver initialises
the device, and after that it writes into a queue the controller has
forgotten, with the guard waving it through. The failure is silent, which is
the worst property available.

What it needs is a watch on the configuration register page, which the page
watch facility can already do - the same mechanism that watches the local
APIC page - so a guest write clearing `CC.EN` bumps the epoch. Recreating the
queue afterwards is a separate and larger job: it means borrowing the admin
queue from a live guest driver rather than from idle firmware, which is what
`admin_borrow` was written for and has never been asked to do.

Until then the honest description of the channel is that it covers early
boot, which is most of what it was wanted for, and stops without saying so
once Windows owns the disk.

## The log's clock: measured, not assumed

Continuous logging cannot be driven by guest exits, because a healthy guest
barely takes any. Measured on the overlay rig with a steadily running
Windows 11, `heartbeat_exits_seen` per processor:

    1564, 161, 165, 163

Fifteen hundred exits on the busiest processor and about a hundred and sixty
on the rest - not millions. That is the VMM working as intended: it
intercepts CPUID, a handful of MSRs, the local APIC page, the controller
register page and EPT violations, and steady-state Windows does almost none
of those. A write path reached only from an exit handler is therefore a write
path that stops the moment the guest settles, which is exactly what was
observed: one block written, then five samples over sixty seconds with every
counter frozen.

The VMX-preemption timer supplies the exits instead, at an interval this side
picks. Two things had to be true and only one was:

  - `arm_controller_poll` already wrote `vmx_preemption_timer_value` and set
    pin-based control bit 6. That half existed.
  - Nothing handled exit reason 52. There was no case for it, so the first
    tick recorded `unhandled_exit` with `reason=0x34` and halted the
    processor in `cli; hlt`. It had never been caught because the only caller
    of `arm_controller_poll` sat on a disabled path - the arming half was
    written and the receiving half was not.

Worth keeping as a shape: a facility built in two halves, with only one half
ever reachable, tests as if it works. The giveaway was a frozen RIP inside
our own module rather than in the guest.

Entry reloads the counter from the VMCS field every time while "save
VMX-preemption timer value" is clear (SDM 26.6.4), so the field does not need
rewriting per tick.

Result, verified on the medium rather than from counters alone: 2048
consecutive blocks off the raw device, sequence 16384-18431, `lost=0
refused=0 dropped=0 reset=0`, while `RIP=fffff806...` showed Windows running
normally.

Tuning left open: at a 1 ms tick each block carries **one** record where it
has room for about thirty, so the region wraps roughly thirty times faster
than it needs to. The fix is to flush on a deadline that is long relative to
the tick instead of on every tick, which trades log latency for region
lifetime; neither number has been chosen against a real need yet.
