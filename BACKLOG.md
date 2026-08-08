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

### 7. S3/S4 leaves the machine unvirtualized — PARTLY FIXED

There is still no resume path, so a suspend takes the hypervisor away and
the guest keeps running on bare hardware without being told. What has
changed is that the entry side is no longer blind and no longer leaves a
mess behind, and that the one fact a resume would rest on is now findable.
The rest of this entry is the design, written down so the next attempt does
not re-derive it.

Read `hypervisor/include/zpp/hypervisor/power.h` first: it states what each
of S3, S4 and S5 costs, with the citations.

**Three switches, all off.** `power::quiesce_on_sleep`,
`power::observe_waking_vector`, `power::resume_from_waking_vector`. Each
comment says what would turn it on.

#### What each transition does now

- **S3.** The write is recognised (SLP_EN tested, SLP_TYPx recorded), the
  channel is drained *and* flushed, and the guest re-executes its own OUT.
  This VMM is still in VMX operation with a VMCS current when the power
  goes, which SDM 27.11.1 says may corrupt it. Harmless only because
  nothing reads those regions again. With `quiesce_on_sleep` on, the
  quiescing processor VMCLEARs, VMXOFFs, WBINVDs and performs the write
  itself; the machine still resumes unvirtualized, but from a clean state.
- **S4.** Identical entry, and no resume path is needed: a hibernation
  resume is a full firmware boot plus the boot manager restoring the image,
  so the loader runs again from the beginning. What S4 needs is that the
  image neither contains nor expects our memory, which is item 14.
- **S5 and warm reset.** Identical entry, which is what "stop cleanly"
  means here. Note the flush is best-effort and usually too late: by the
  time the PM1 write happens the guest's NVMe driver has already shut the
  controller down, so `flush_pending` has nothing to write through. The
  flush that actually lands is `on_controller_register_before_write`, on
  the guest's own CC write. Worth keeping the sleep-time one for the case
  where no driver shut anything down - a firmware-initiated S5 from the
  power button - but do not credit it with more than that.
- **The guest's own view** is unchanged in every case: it gets the sleep it
  asked for. On the quiesce path the write is performed rather than handed
  back, which the guest cannot distinguish, and RIP advances past it.

#### What a resume path has to do, in order

Three things make this smaller than it looks.

1. **The trampoline already exists.** `arch/x86_64/ap_start_up.S` climbs
   real mode to long mode on the host page table, parameterised by an
   `ap_start_up_area`. The ACPI real-mode waking protocol and a SIPI
   produce the *same* entry state: EDK2's `AsmTransferControl`
   (`MdeModulePkg/Universal/Acpi/BootScriptExecutorDxe/X64/S3Asm.nasm`)
   clears CR0.PE, CR4.PAE and EFER.LME and then far-jumps to
   `(vector >> 4):(vector & 0xf)`, while a SIPI enters at `CS = page << 8,
   IP = 0` - and for a page-aligned vector those are the same address.
   So the blob is reusable verbatim, with `entry` rewritten at sleep time
   and put back to `zpp_ap_start_up_main` on the way out. No second copy
   and no new assembly.
2. **Memory is already right.** S3 preserves it, and everything this VMM
   keeps describes physical memory that has not moved: host page table,
   host GDT and IDT, EPT, the module. So `initialize_os_page_table`,
   `initialize_host_page_table`, `initialize_host_gdt`,
   `initialize_host_idt`, `initialize_ept` and `protect_module` must **not**
   run again.
3. **The application processors need nothing.** The guest sends
   INIT-SIPI-SIPI again during its own resume, and the existing adoption
   path handles that. What does need doing is rewinding the per-processor
   bookkeeping - `processor_virtualized`, `started_by_trampoline`,
   `start_up_launched`, `next_virtual_processor`, the stack index - or the
   slots leak and `max_cpus` is reached after a few suspends.

The order, then:

- **On the way down**, after the quiesce and before the OUT: read
  `FirmwareWakingVector`, save it in `guest_waking_vector`, **zero
  `XFirmwareWakingVector`**, and write the trampoline page's physical
  address into `FirmwareWakingVector`. Zeroing the extended field is what
  forces the real-mode protocol: EDK2's `S3Resume.c` uses the 16-bit vector
  only when `XFirmwareWakingVector == 0`, and takes a protected- or
  long-mode path otherwise. Then WBINVD, then the OUT.
- **On the way up**, in the resume entry the trampoline reaches: rewind the
  bookkeeping, `enable_vmx_in_feature_control()` (IA32_FEATURE_CONTROL is
  zero again - SDM 26.7), VMXON, **zero the VMCS region and re-stamp the
  revision identifier** before VMCLEAR and VMPTRLD, re-run `setup_vmcs`,
  then `apply_start_up` at `guest_waking_vector` to build a real-mode guest
  there, restore the trampoline's `entry`, and VMLAUNCH.

The VMCS is rebuilt rather than reused **on purpose**. Only the quiescing
processor's region was VMCLEARed before the power went; every other one was
active on a processor that left VMX operation, which is the case SDM 27.11.1
says may corrupt it. Rebuilding is no harder than reusing and is correct
whatever state the guest left its processors in.

#### What was rejected, so it is not re-proposed

- **A rendezvous to VMCLEAR every processor's VMCS before the sleep.**
  Impossible for a processor the guest parked in wait-for-SIPI: SDM 28.2 -
  "if a logical processor is in the wait-for-SIPI state, NMIs are blocked.
  The NMI is not delivered and no VM exit occurs" - and SDM 29.7.2 repeats
  it for external interrupts, INIT and SMI. From inside an exit handler
  there is no way to tell which state each processor is in, and an NMI to
  one that is in root mode reaches the host IDT with no recovery point
  armed and halts it for good. Rebuilding on resume removes the need
  entirely.
- **Telling S3 from S4 from S5 by the value written.** SLP_TYPx comes from
  evaluating the `\_Sx` objects in the DSDT. There is no AML interpreter
  here. It does not matter: the entry side is the same for all three.
- **Refusing or vetoing the sleep** to keep the hypervisor resident. That
  is lying to the guest, which is the mistake *What the guest is told* in
  `CLAUDE.md` is about.
- **Serving the extended waking vector.** It is entered in long mode
  through a different protocol. Zeroing it and serving the real-mode one
  reuses machinery that already works.

#### The experiments, cheapest first

Each needs `-DZPP_DIAG=ON` so the channel carries the result off the
machine, and each is one variable.

1. **Is the FACS there, and does the guest leave a vector in it?** Turn on
   `observe_waking_vector` only. Suspend the guest, resume it (the machine
   comes back unvirtualized, as today), read the channel. Wanted: a
   non-zero "guest waking vector" and a zero extended one. A zero
   `FirmwareWakingVector` with a non-zero extended one stops the whole
   approach and the answer has to be found somewhere else.
2. **Does the quiesce complete?** Add `quiesce_on_sleep`. Suspend and
   resume. Wanted: `sleep_request.stage` reaching `write_issued`, and the
   machine actually sleeping rather than hanging. `write_returned` or
   `re_established` means the write did not sleep, which is also a useful
   result.
3. **Does the guest tolerate a suspend at all?** Independent of the two
   above and worth doing first, because it is free: `system_powerdown` on
   the QEMU monitor sends an ACPI power-button event, which is a real S5
   request and needs nothing inside the guest. It exercises the recognition
   and the flush without any resume. A `lid`- or timer-driven S3 needs the
   guest to cooperate, so on Windows that is `powercfg /h off` plus a
   deliberate sleep from the menu; on the TinyCore rig,
   `echo mem > /sys/power/state`.
4. **Does the resume path work?** Only after 1 and 2, and only with
   someone at the machine, because the failure mode is a machine that looks
   dead. Wanted: the channel showing the resume entry reached, then the
   guest continuing.

Note that none of this can be exercised under emulation: Bochs has no
VT-x, and QEMU's monitor can request S5 but the interesting path is S3 on
real firmware.

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

## Real-NVMe rig: the loader works, the resident side does not, and the
## instrumentation disagrees with itself

Established on the VFIO rig, real NVMe passed through, backup taken first:

  - The loader half works end to end on the real controller. Serial says
    `borrow returned ok`, `admin sq/cq entries differing 0x0`, `VERDICT
    borrow restored the admin queue byte for byte`, `private queue pair
    created`, `proof written and verified at lba 0x1fe8000`, and
    `VERDICT the private queue moves data to the disk`. The reservation is
    idempotent as designed - `already reserved, gap sectors 0x20000`.
  - `zpp_load_elf` returned success: `ZPP_TRACE loading` then `loaded`,
    with no `ZPP_HYPERVISOR_FAILED`.
  - The resident side never configured the channel. `configure_reject`
    reads `untried`, so `configure()` was not entered at all, and
    `sequence`, `boot_id`, `target` and `physical_of` are all zero.

Unresolved, and the next thing to settle before trusting any conclusion
here: `heartbeat_exits_seen` reads zero on every processor, while the word
at module base + 0x1434070 changes about nineteen hundred times a second.
Both cannot be true of the same memory. The addressing checks out - one
contiguous PT_LOAD from the base with MemSiz 0x31387ba, so 0x1434070 is
inside .bss; ELF magic reads at base + 0; and the constant
`staged_deadline_ticks` reads 0x989680 at base + 0x1048. So either that
page is not covered by whatever protects the module, or the module is not
solely where the base says it is.

Do not theorise past this. The measurement that settles it is the extent
actually protected: compare the allocation and the EPT protection against
the ELF's full 51.6 MB MemSiz, rather than against its file size, and
check whether the guest can write the tail of .bss. A guest sharing the
module's .bss tail would explain the churn, the zeroed counters and the
un-entered configure() at once.

Three wrong turns on the way here, recorded so they are not repeated:

  - `submitted`, `completed` and `lost_to_reset` are not statics of the
    sink. A `grep ... | head -1` that matched nothing produced an empty
    offset, so the read landed on the module base and returned the ELF
    magic `0x00010102464c457f` as a plausible-looking number. Any reading
    equal to that value is a failed symbol lookup, not data.
  - `_ZGV...` is the guard variable, not the object. Matching `instance`
    loosely picked it up and put every singleton read 0x1ced000 out.
  - OVMF NVRAM persists across runs, so once Windows has booted it puts
    itself first and the rig silently stops running our loader - serial
    carries firmware output only. Restore RELEASEX64_OVMF_VARS.fd from
    .orig before every run, which is what makes bootindex=0 win.

### Ruled out statically: the allocation and the protection

Both halves of the obvious explanation are correct in the source, so the
churn is not either of them:

  - `elf_file::memory_size()` is `p_vaddr + p_memsz` of the last LOAD,
    rounded up - the full 51.6 MB, not the file size. Its comment already
    says why, and the comment is right.
  - `hypervisor::module_size` is assigned from that `memory_size()`, and
    `protect_module()` clears all four EPT permission bits over
    `module_size / page_size` pages from `module_base`.
    `max_module_size` is 100 MB, so nothing is truncated.

So a correctly launched hypervisor does protect the whole image including
the .bss tail, and the guest should not be able to write base + 0x1434070.
The next measurement is therefore the self-validating one, not another
guess: `module_base` is a member of the singleton, so reading it back
proves whether the singleton address used for every other member read is
right at all. If it reads the value the loader traced, the zero counters
are real; if it does not, every singleton read taken so far - including
`heartbeat_exits_seen` - measured nothing.

One trap found while setting that up, worth the note because it silently
produces plausible numbers: `llvm-dwarfdump --name=<member>` returns the
first DIE anywhere in the file with that name, including members of
nested and unrelated types. `heartbeat_exits_seen` resolved to offset
0x38 while `module_base` resolved to 0x1406008; only one of those can be
an offset into this class. Check the parent DIE, do not take the first
`data_member_location` that matches.

### Stop condition on the real rig

Windows on the real NVMe has now been hard-killed many times across this
work and has already produced one "Inaccessible boot device". The last
attempt chainloaded and then QEMU exited on its own with an empty log,
which is a new failure and not one worth diagnosing by repeatedly power
cycling a real Windows install. The disk was verified intact afterwards -
GPT signature, ESP still shrunk to 33423360 sectors, Windows boot region
readable, reserved region still stamped - and the full binary backup on
the external SSD remains the fallback.

Continue this leg on the passed-through disk, carefully - see the decision
recorded at the end of this file, which retires the copy-on-write rig.

## Resolved: the real-NVMe channel stops at the guest's controller reset

Measured on the real disk with the rebuild switched off, and every number
below validated first by reading `module_base` back out of the singleton -
it returned exactly the base the loader traced, which is what makes the
rest of the snapshot trustworthy:

    epoch             1                 configure() succeeded
    physical_of       0x78df1590        translator stored
    boot_id           0x8a598e999       stamped
    sequence          1                 one block written
    next_block_index  1
    lost_to_reset     1                 one block lost to a reset
    bound             all zero          the binding was forgotten

So the resident side does work on real hardware: it configures, writes a
block, and then Windows' NVMe driver clears CC.EN, `note_controller_write`
calls `queues::forget()`, and with `rebuild_channel_after_reset` off the
channel never returns. That is the documented limitation, now confirmed by
direct evidence instead of inferred. The overlay logs indefinitely only
because the emulated controller is never reset - `lost_to_reset` stays 0
there.

The remaining work is therefore exactly the piece already identified: to
survive the reset the VMM must emulate the guest's CC write rather than
step over it, so the register page is never briefly writable and no second
write slips past. That needs the instruction decoder.

### Two ways this investigation lied to itself

Both cost a lot of time and both are the kind that produce confident,
plausible numbers rather than errors.

**The flag was on.** `rebuild_channel_after_reset` was left `enabled`
after being measured, contradicting its own comment - which says in as
many words that a timed out borrow desynchronises the guest's admin queue
and "that is not a thing to leave switched on". With it on, the guest
resets the machine; under `-no-reboot` QEMU exits. Crucially the reset
takes the hypervisor with it, so every counter read afterwards is
*recycled RAM*: that is where `configure_reject = untried`,
`heartbeat_exits_seen = 0` and a word in .bss apparently changing 1900
times a second all came from. None of it was our memory. Read
`module_base` back before believing any other member.

**The instrumentation was optimised away.** `configure_reject` is written
in several places and read by nobody in the program, so the stores were
provably dead and were removed, leaving the symbol reading its zero
initializer for ever. It reported `untried` on a run where `epoch`,
`boot_id` and `physical_of` all proved `configure()` had succeeded. Any
field whose only reader is a debugger must be `volatile`, or it measures
nothing and says so confidently.

## elf_image_base() can find the wrong base, and did

`zpp::elf_image_base()` locates the running module by scanning *backwards*
from a magic constant inside itself for the first page beginning with
`\x7fELF`. That is a guess dressed as a computation: it is correct only
while no page between the module's true base and that constant happens to
start with those four bytes.

Measured on the real-NVMe rig. The loader traced its allocation and the
image is demonstrably there - `xp` reads the ELF header at 0x78ddc000 and
the constant `staged_deadline_ticks` reads 0x989680 at base + 0x1048 - yet
the singleton's own `module_base` member holds **0x78ed3000**, 0xF7000
higher and *inside* our own image. The scan stopped early on a page about
a megabyte into the module. The same build on the overlay rig computes the
base correctly, so this is machine dependent, which is the worst property
a bug like this can have.

What it breaks is everything derived from the base:

  - `module_size` is measured from it, so it describes the wrong range.
  - `protect_module()` clears EPT permissions over that wrong range, which
    leaves the low megabyte of the module reachable by the guest and
    needlessly hides a megabyte past its end.
  - `module_physical_to_virtual` maps the wrong pages, so the decoy
    redirect for guest accesses is keyed on the wrong addresses.

It is very likely the explanation for the earlier "a word inside our .bss
changes nineteen hundred times a second" reading, which was recorded as
unexplained: with the protected range displaced, part of what we think is
ours is not protected at all.

**The fix is to stop searching.** The loader knows exactly where it put
the image - it is the return of `allocate_rwx`, and `zpp_load_elf` places
the image at it - so the base should be handed over in
`zpp_launch_parameters` like everything else the platform knows, and
`elf_image_base()` kept only as the fallback for a build with no loader to
ask. A scan that can be wrong is not a good way to learn something the
caller already knows exactly.

Until that lands, treat any real-rig reading of hypervisor state as
suspect unless `module_base` read back out of the singleton equals the
base the loader traced. On the overlay it does; on the real rig it does
not.

### Correction: the "self-validating read" was not validating

Reading `module_base` back out of the singleton was recorded above as the
check that proves an address is right. It is a good idea and it is not
what the recorded runs did.

`hypervisor::module_base` sits at offset 0x1406008 into the class - past
sixteen megabytes of per-processor stacks - and the singleton address used
was the `.bss` symbol plus the module base. On the real rig that lands in
`module_physical_to_virtual`, not on the scalar: `module_size` read at the
neighbouring offset returned 0x78ed4000, an address rather than a size,
and the values at successive offsets were consecutive page numbers. That
is a map of module pages, and because the map is identity, an entry in it
*equals a module address* - which is why the same read on the overlay
returned something equal to the base and looked like a pass.

So the overlay "validation" was a coincidence of reading an identity map,
and the real rig's 0x78ed3000 is an entry from the same map rather than a
misdetected base. The elf_image_base finding recorded above is therefore
**not established** - it rests on this read. The scan's fragility is real
as an argument, and handing the base over is still the right design, but
the claim that it was measured going wrong is withdrawn.

What a real self-validating read needs: the offsets are right (they come
from the class DIE), so what is wrong is the singleton's address. Resolve
that first - the guard variable trap earlier moved it by 0x1ced000, and
`.bss` symbol plus base has now been shown to land somewhere unintended -
before trusting any member read on that machine.

## The real rig is not running the hypervisor, and the loader says it is

Two earlier entries here are **withdrawn**, and the reason is the same
both times: readings taken from a machine where the hypervisor was never
resident.

  - "elf_image_base can find the wrong base, and does on the rig" - the
    0x78ed3000 it reported was uninitialised memory, not a misdetected
    base.
  - The correction to it, which claimed the self-validating read was
    matching an identity map entry. That was wrong too. The addressing is
    provably right: `sizeof(hypervisor)` is 0x1ced000, the singleton's
    .bss symbol is 0x1435000, and 0x1435000 + 0x1ced000 = 0x3122000,
    exactly where the guard variable sits. The overlay confirms it
    empirically - `module_size` read through that address returns
    0x3163000, which is the value computed independently from the
    program headers and which no map entry could be.

What is actually true, measured with three `volatile` globals at small
.bss offsets written unconditionally at the end of
`initialize_module_region()`:

                       overlay        real rig
    handed over        0x78dfb000     0
    module_base used   0x78dfb000     0
    module_size used   0x3163000      0
    sequence           advancing      0

The probe is sound - the overlay sets all three through the identical
symbol addresses - so on the real rig `initialize_module_region()` never
runs. The hypervisor is not going resident there at all, which explains
every zero and every garbage reading taken from that machine, including
the ones above.

**And the loader reports success anyway.** `ZPP_TRACE loading` is followed
by `ZPP_TRACE loaded`, `zpp_load_elf` returns zero and no
`ZPP_HYPERVISOR_FAILED` is printed. So the loader's success is not
evidence of residency, and every conclusion drawn from "it loaded, so it
is running" on this rig has to be re-examined. That is what
`verify::enabled` exists for and it is switched off in these builds.

An earlier build did go resident on this machine - `epoch` 1, `sequence`
1, `lost_to_reset` 1 - so this is a regression, and the changes in
between are small enough to bisect: the decoder header, the 8- and
16-bit MMIO accessors, the watch handler signature, the emulation path
itself, the two counters, the module base handed over through
`zpp_launch_parameters`, and the probes.

Eliminated by measurement already: enlarging the mapping window from
eight pages to seventy-two. Shrinking it back to ten, shared under
`mapping_window_lock`, changes nothing - all three probes still read
zero. The window has been left at ten regardless, because the aliasing
argument in its comment was made about eight and growing it eightfold
was never reasoned through.

Next suspect, on the grounds that it is the only ABI change: the field
added to `zpp_launch_parameters`. Both sides are rebuilt together so it
should be harmless, which is exactly why it is worth checking rather
than assuming.

## The real rig intercepts CPUID and loses the next instruction

The sharpest signal yet, and a behavioural one rather than a memory read,
which is why it is worth more than everything above it.

The loader now asks CPUID leaf 0x40000000 immediately after
`zpp_load_elf` returns and traces ebx. One binary, both rigs:

    overlay    zpp: [main.cpp:1512] ZPP_TRACE hypervisor leaf ebx 0x5a70705a
    real rig   (nothing at 1512)

0x5a70705a is "ZppZ", so the overlay is demonstrably resident. On the
real rig that line does not appear at all - while `ZPP_TRACE loaded` at
1475 immediately before it does, and `chainloading` well after it does
too. Execution therefore passes through the region and skips the trace.

A `cpuid` followed by a call that never happens, on a machine where the
same code works, is not the loader going wrong. Something intercepts the
CPUID and returns with RIP past more than the instruction - which is the
recurring failure this tree already warns about in the exit handler, and
which the handler's own comments call out as the reason unhandled exits
must halt rather than resume. So the hypervisor is very likely resident
on the real rig after all, with a broken exit path, rather than absent.

That in turn casts doubt on the entry above it - the probes in .bss
reading zero. Both cannot be straightforwardly true, and the behavioural
evidence is the stronger of the two. Do not resolve this by reasoning; the
next step is to break in the VMM's own CPUID case on that machine, or to
have that case record its exit count and the instruction length it read
from the VMCS, and see what it does with the first CPUID after launch.

The loader keeps the probe. One CPUID after launch is nearly free, and
"the loader returned zero" has now twice been mistaken for "the
hypervisor is running".

## The channel works on the real NVMe

Read off `/dev/nvme0n1` with `dd`, after a boot with the guest running:

    2048 blocks read
       924 stamped by the reservation, never written
      1124 written by the hypervisor
      sequence 0 to 1123 contiguous, epoch 1
      seven to fifteen records per block
      lost 0, refused 0, dropped 0, lost to reset 0

So the destination, the borrow, the private queue, the hand-over and the
resident writer all work on real hardware, and blocks fill properly rather
than carrying one record each.

What is *not* yet met: the guest does not survive. After those 1124 blocks
the guest froze with RIP in the firmware range and the counters stopped -
`sequence`, `lost_to_reset` and the exit count all static across seventy
seconds. So the log covers the boot up to that hang and stops, and "while
Windows runs" is only true of the part of the boot that ran. That hang is
the next thing to chase and is very likely the same one reported as
Windows sitting on its spinning circle.

Note `lost_to_reset` stayed 0, so the guest had not yet reset the
controller when it froze - the reset path is still unexercised on real
hardware, and the instruction decoder built to survive it is therefore
still unproven for its actual purpose.

Two things this run also settled:

  - The preemption timer is **not permitted** on this machine, so the log
    has no clock there and the 1124 blocks were driven by the guest's own
    exits during boot, which are plentiful while firmware and the boot
    manager run and dry up once an OS settles. Continuous logging on this
    machine therefore still needs a clock, and the timer cannot be it.
    What can: the preemption timer is one of several ways to manufacture
    an exit, and a periodic one that is always available is worth finding
    before assuming this is fixed.
  - Blocks carry seven to fifteen records, not one, which is what the
    throttled heartbeat was meant to achieve.

## The hang: the APs never execute after their SIPI, so the BSP waits for ever

Found by dumping the guest's stack and disassembling, not by inspection.
Every number here was read off the running machine.

**What it is spinning against.** The boot processor sits at `0x7ed7a41f`
in this loop:

    call InterlockedCompareExchange32
    add  rsp, 0x20
    test eax, eax
    je   done                  ; leave only when the original value was 0
    pause
    jmp  loop

with `RCX = 0x7ed68004`, and `RDX = R8 = 0x50415453`. Compare equals
exchange, which identifies the caller exactly - EDK2 MpInitLib:

    VOID WaitApWakeup (volatile UINT32 *ApSehaphore) {
      while (InterlockedCompareExchange32 (ApSehaphore,
               WAKEUP_AP_SIGNAL, WAKEUP_AP_SIGNAL) != 0) { CpuPause (); }
    }

`WAKEUP_AP_SIGNAL` is `SIGNATURE_32('S','T','A','P')` = 0x50415453. The
contended memory confirms it - one dword per processor:

    0x7ed68000: 00000000                    slot 0, the boot processor
    0x7ed68004: 50415453 50415453 ...       slots 1..7, all still set
    0x7ed68020: 00000000                    end of the array

The caller frame is the walk over them: `imul $216, %r12` - 216 is
`sizeof(CPU_AP_DATA)` - indexing `CpuMpData->CpuData` at offset 1104,
skipping `BspNumber` at offset 12, bounded by `CpuCount` at offset 8.
Frame chain from RBP: `0x7ed7b79f` then `0x7ed7ba8b` then `0x7fe03efa`.

So the boot processor set each application processor's wake signal and is
waiting for the processor to clear it. **None of them ever does.**

**Why.** Their state, read per processor rather than inferred:

    CPU#0   RIP=0x7ed7a41f  CS=0x0038  CR3=0x7fa01000   64-bit, spinning
    CPU#1-7 EIP=0x00000000  CS=0x8700  CR3=0           real mode, PE clear

Every general purpose register on an application processor is zero and
EDX holds 0x000806eb, the CPUID value a processor has at reset. That is
the architectural state immediately after a start-up IPI, *before one
instruction has executed*. Their `heartbeat_exits_seen` is frozen at 4,
and the fourth exit is the SIPI itself:

    CPUID     cs=0x9b00 rip=0x0d
    CPUID     cs=0x9b00 rip=0x2c
    INIT (3)  cs=0x9b00 rip=0x46   activity_state -> 3, wait-for-SIPI
    SIPI (4)  qual=0x87 cs=0x8700 rip=0

Everything downstream of that is right: `start_up_launched` is 1 for all
eight, `processor_virtualized` is 1 for all eight, the target segment
0x8700 matches the vector the guest asked for, and 0x87000 really does
hold the firmware's relocated trampoline (`mov ebp,eax; mov ax,cs; mov
ds,ax; ...`). So the guest state is configured correctly and simply never
runs.

**Therefore the application processors are stuck in root mode**, in our
own code, after handling their SIPI and before resuming the guest. The
boot processor's spin is a consequence, not the fault.

That narrows it to the exit path's tail on a processor whose fourth exit
was a SIPI, and it makes two findings from the concurrency review prime
suspects, because both halt a processor in root mode with no recovery
point and neither touches `unhandled_exit` or `vm_entry_failure`:

- `queues::forget()` assigning the binding non-atomically while another
  processor is inside `submit()`, giving a store through a null MMIO
  pointer.
- the single shared `host_exception_recovery` slot, whose documented
  safety argument - that only the boot processor is ever in `main` - is
  false, because an adopted processor reaches `main` through
  `start_up_on_this_processor`.

Note the hang survives `emulate_watched_page_writes = false` and
`ZPP_DIAG=OFF`, so it is in the core rather than the channel or the
decoder.

**A correction worth keeping.** An earlier reading of this said all eight
processors were at the same RIP. That was a parsing artefact: the awk
pairing carried CPU#0's RIP forward into blocks that did not print one,
because `info registers -a` had been truncated by too short a read. Read
each processor's block whole, and check for a distinguishing register -
CR3 was 0 on every application processor, which is what exposed it.

### The bisect so far, and two conclusions withdrawn

The known-good loader is `4e167315`, still on the rig as
`/EFI/zpp/zpp_loader.efi.bak-4e167315`. It boots Windows **reliably - two
of two runs**, RIP in the kernel range and advancing - so the good/bad
split is real and deterministic, not a race. It cannot be identified by
commit: building `19d4fdc` gives `fb7398a4`, so it came from a dirty tree.
It can be identified by its embedded strings, which is how the range was
pinned: it contains `selftest: laps`, `already reserved` and `mapping
window reads`, and lacks `cpu {} alive` - the heartbeat added in
`737b754` - so it predates that commit.

Eliminated on HEAD, each by its own boot rather than by argument:

  - the instruction decoder and its emulation path
  - the whole diagnostic channel (`ZPP_DIAG=OFF`)
  - NMI exiting - tested twice, on HEAD and on `a340308` itself
  - handing the module base over instead of scanning for it
  - the mapping window at ten pages rather than eight
  - connecting every controller before the self test
  - the two log lines `arm_controller_poll` emitted on *every* exit

The last of those is fixed and kept regardless: it was thousands of
heap-allocating calls into shared state per boot, and it now speaks once
per processor.

**Two conclusions withdrawn.**

The first: that all eight processors sat at the same RIP. That was a
parsing artefact of a truncated `info registers -a`, where the pairing
carried CPU#0's RIP into blocks that printed none. CR3 reading zero on
every application processor is what exposed it.

The second, and the more expensive: that the application processors were
halted in `vmresume`'s failure loop. `vmresume`'s wrapper does fall into
`cli; hlt; jmp` on failure, and such a failure produces no VM exit, so it
was a good fit for a frozen exit count with nothing recorded. It is not
what happens. The wrapper now reports before halting - it hands RFLAGS to
`zpp_vmx_entry_failed`, which reads `vm_instruction_error` while the VMCS
is still current - and `entry_failures_seen` is **zero**. `host_exception`
is zero too. So no entry failed and no host exception was taken.

What that leaves, and it is the thing to test next: the application
processors are most likely *running*. Their guest state looked frozen only
because a processor that takes no VM exits never syncs its registers out,
so QEMU's view of them is stale - it dates from the SIPI. Their real-mode
stub at 0x87000 is eight instructions ending in `mov cr0, eax`, and with
unrestricted guest PE can be guest owned, so even the switch to protected
mode need not exit. A processor can therefore run all the way into the
firmware's park loop and produce nothing to observe.

So the question is no longer "where are they stuck" but "why does a
running processor not see the semaphore the boot processor writes". The
next experiment is to make them observable rather than to reason: arm the
monitor trap flag on one application processor, or intercept CR0 loads so
the protected-mode switch produces a fifth trace entry, and see how far
they actually get.

## Nothing is lost at the guest's controller reset any more

The channel now flushes through the guest's own working controller at the
last moment it is still there. Measured on the real rig with Windows
running - RIP in the kernel range and advancing across four samples:

    before   sequence 12, lost_to_reset 1
    after    sequence 12, lost_to_reset 0, dropped 0

The mechanism is a `before_write` hook on a page watch, called *before* the
guest's write is allowed to take effect. The existing `on_write` runs after
it has retired, which for the controller's register page is too late: if
the write cleared CC.EN the queues are already gone, and anything staged
is only countable, not writable.

It deliberately does not decode the instruction to find out whether this
particular write is the reset. It does not need to - the pages it is armed
on are touched while a driver sets itself up and almost never afterwards,
so flushing on any write to one is cheap and always correct. That also
means it works with the instruction decoder switched off, which matters
because the decoder has three unfixed defects.

**What this is not.** It is not continuous logging. The log is now
complete up to the reset rather than truncated by it, and it still stops
there: Windows takes the controller, our private queue is destroyed, and
`rebuild_channel_after_reset` is false. Continuity past that point needs
the queue re-established, and the design for it is the bracketed excursion
recorded earlier - at the CC.EN 0 to 1 edge, program our own ASQ/ACQ while
CC.EN is clear, create a private queue, then restore the guest's registers
- which avoids both the borrow from a live driver and the queue identifier
collision that would make Windows' own Create fail.

## Decision: the passed-through disk is the only rig

The copy-on-write image and emulated-controller rig is retired. Everything
is tested against the real controller, passed through.

Why, since it looked like the safe option: an emulated NVMe removes the
thing being tested. The guest and the channel talk to a *model* of a
controller, so the admin queue borrowed is the model's, the doorbell stride
and MQES are the model's, and no real-device behaviour is exercised - the
one property the channel depends on. The log blocks land in a file that is
then discarded, when the whole point is that an agent elsewhere reads them
off the medium. And it cannot be combined with passthrough at all, because
VFIO means the guest drives the hardware directly and there is no layer to
interpose an image into, so anything proved that way has to be proved again
on the real thing regardless.

It also actively misled this work. Several conclusions were drawn from
agreement or disagreement between the two rigs - a module base that "the
same build computed correctly elsewhere", a channel that "logs
indefinitely" - and the emulated controller simply never resets, so it
never exercised the one event the design has to survive. Comparing two rigs
where only one is real produced confident wrong answers more than once.

What replaces the isolation is discipline, and it is not optional now:

  - a verified backup before anything, which exists on the external SSD
  - one variable per boot
  - anything new defaulted **off**, so a plain build cannot run it
  - scripts/check-bootable.sh between the build and the disk
  - the known-good loader restored after every experiment
  - and an accepted cost: a wedge needs someone at the machine to power
    cycle it, so that has to be budgeted before enabling something untried

The historical measurements above that name the other rig are left as they
were written, because they are what was actually observed at the time -
including the ones that were later withdrawn, and why.

## Nested VMX: what exists, what is missing, and how it fails

Landed off, behind `-DZPP_NESTED_VMX=ON` which defaults to `OFF`. Read this
before turning it on, and read it before deciding what to build next.

### What exists

- **A guest instruction can no longer halt a processor.** All thirteen VMX
  instructions had one exit reason each and no case, so every one of them
  fell to `default:` in the exit handler, which halts. They now fault with
  `#UD`, which is the architecturally correct answer given that the guest
  is told CR4.VMXE is clear: VMXON's operation section raises `#UD` for
  `CR4.VMXE = 0`, and SDM 28.1.1 puts invalid-opcode exceptions above the
  VM exit. This is true in the default build and is not gated on the
  switch.
- **Guest memory access from a VM exit.** `read_guest_physical`,
  `write_guest_physical` and `guest_linear_to_physical`, the last walking
  `vmcs.guest_cr3()` rather than the launch-time `os_page_table`. This is
  the piece item 3's fix and `emulate_watched_page_writes` were both
  waiting for — the walker is now here, so re-enabling the write emulator
  is a smaller job than its comment describes.
- **A shadow VMCS**, stored in the guest's own VMCS region and indexed by
  the encoding's width, type and index rather than by a named-field
  layout. Cached per processor so VMREAD and VMWRITE do not pay a guest
  memory access each.
- **VMXON, VMXOFF, VMCLEAR, VMPTRLD, VMPTRST, VMREAD, VMWRITE** emulated,
  with the `VMsucceed`/`VMfailInvalid`/`VMfailValid` conventions of SDM
  33.2 and the error numbers of Table 33-1.
- **The capability MSRs and `IA32_FEATURE_CONTROL`** answered, as a subset
  of the hardware's rather than from a table of constants.

### What is missing, in the order it has to be built

1. **Nested EPT.** This is the one that decides everything else.
   `IA32_VMX_EPT_VPID_CAP` reads as zero and the secondary controls offer
   neither EPT nor VPIDs, because supporting them means shadowing the
   guest's extended page tables against ours: combining two sets of
   permissions per page, faulting into the shadow on a second-level EPT
   violation, and rebuilding on the guest's INVEPT. Hyper-V and every
   other current hypervisor require EPT, so **nothing real runs until this
   exists.**
   Rejected as a shortcut: using the guest's EPT pointer directly as the
   second-level EPTP. Our own extended page tables are an identity map, so
   it would *function* — and it would also hand the second-level guest our
   module, our page tables and every watched page, because the module
   protection and the channel's page watches live in entries the guest's
   tables know nothing about.
2. **A real VMCS built from the shadow** — vmcs02 — merging the shadow's
   guest state and controls with this VMM's host state and the intercepts
   it cannot give up.
3. **Exit reflection.** For every exit the second-level guest takes, a
   decision: reflected into the shadow's exit-information fields and
   delivered to the guest hypervisor, or handled here. The interesting
   cases are the ones this VMM already owns for its own reasons — NMI
   exiting, the interrupt command register, the watched pages.

### How it fails today, exactly

With the switch on, a guest hypervisor executes VMXON, VMPTRLD, a full run
of VMWRITEs, and then VMLAUNCH — which returns `VMfailValid` with
VM-instruction error 7, "VM entry with invalid control field(s)", and the
log line `guest vmlaunch refused: no second level entry`. RIP lands on the
instruction after the VMLAUNCH, which is where SDM 33.3 puts a failure on
the controls.

Error 7 is the closest honest answer and it is not a precise one. There is
no error number for "this VMM does not implement VM entry". It is defensible
because the controls this VMM can honour genuinely exclude the ones any real
guest hypervisor needs, and a hypervisor that consulted the capability MSRs
first will already have found EPT missing.

### Two known divergences, both deliberate

- **A structurally valid encoding naming a field that does not exist is
  accepted**, where hardware answers `VMfailValid` with error 12. Closing
  it needs Appendix B as data; `vmcs_fields.h` is an enum of the fields
  *this* VMM uses, so checking against it would refuse fields that do
  exist — the worse error. The gap only ever accepts more than hardware
  would, never less.
- **Field indices at or above 28 are refused.** Derived from Appendix B and
  from the region being 4096 bytes; what it excludes is 64-bit control
  indices 28 to 41, encodings `00002038H` upwards, which belong to the
  tertiary controls and the structures that go with them — none of which
  are reported as supported. `IA32_VMX_VMCS_ENUM` reports the limit, which
  SDM A.9 makes exactly its purpose, so a guest is told rather than
  surprised.

### Not run anywhere

Nothing here has executed on hardware or under an emulator. It is checked
only against the compiler, in all four configurations. What would establish
it, in order of how much it proves per boot:

1. A first-level probe of our own, under Bochs: VMXON on a region with our
   revision identifier, VMPTRLD, VMWRITE then VMREAD of one field of each
   width, VMPTRST, VMCLEAR, VMXOFF, with each outcome checked against the
   flags the SDM specifies. That exercises every path above without needing
   a guest hypervisor at all, and it is the only experiment that can be run
   locally, since the VMX instructions never execute in the default build.
2. The capability MSRs as the guest reads them, compared against the
   hardware's — the narrowing is the part most likely to be wrong, and the
   test machine's own values are already a filtered subset because QEMU
   runs with `hv-passthrough`.
3. Windows with VBS on and the switch on, expecting a Hyper-V launch
   failure rather than a hang. That is the one that needs the rig, and it
   is worth nothing until nested EPT exists.

## Nested VMX coverage checklist

What a complete nested VMX implementation has to do, where each requirement
comes from, and whether this tree does it. **This list is the
done-condition**: nested VMX is finished when every row is `yes`, and the
rows that are not `yes` are the work.

The citation column says where the requirement is *established* - an SDM
section this project has actually looked up, or the KVM function that
implements it. Rows citing KVM name the function so the shape can be
compared rather than re-derived from scratch.

Status values: `yes` implemented; `partial` implemented for some cases;
`no` not implemented; `n/a` deliberately not offered to L1, with the
capability MSR narrowed to say so.

### A. VMX instruction emulation

| # | Requirement | Established by | Status |
|---|---|---|---|
| A1 | VMXON: region checks, revision id, feature-control gate, `VMfail` when already in VMX operation | SDM 33.3 VMXON operation section; KVM `handle_vmxon` | yes |
| A2 | VMXOFF: leave VMX operation, write back the current shadow | SDM 33.3 VMXOFF; KVM `handle_vmxoff` | yes |
| A3 | VMCLEAR: address checks, VMXON-pointer check, clear the launch state of a *non-current* VMCS | SDM 33.3 VMCLEAR; KVM `handle_vmclear` | yes |
| A4 | VMPTRLD: address checks, revision-id check, write back the outgoing shadow | SDM 33.3 VMPTRLD; KVM `handle_vmptrld` | yes |
| A5 | VMPTRST: store the current pointer, all-ones when there is none | SDM 33.3 VMPTRST; KVM `handle_vmptrst` | yes |
| A6 | VMREAD/VMWRITE: encoding decode, width handling, read-only refusal | SDM 27.11.2, Table 27-22, A.6 bit 29; KVM `handle_vmread`, `handle_vmwrite` | yes |
| A7 | `VMsucceed`/`VMfailInvalid`/`VMfailValid` flag conventions and the error-number table | SDM 33.2, Table 33-1 | yes |
| A8 | Memory-operand address computation from the instruction-information field plus the displacement | SDM 30.2.1, Table 30-14, Table 30-15; KVM `get_vmx_mem_address` | yes |
| A9 | `#UD` for every VMX instruction when the guest is not in VMX operation, or CR4.VMXE is clear in its own view | SDM 33.3 operation sections, SDM 28.1.1; KVM `nested_vmx_check_permission`, `handle_vmxon` | yes |
| A10 | `#GP(0)` when CPL > 0 | SDM 33.3; KVM `nested_vmx_check_permission` | yes |
| A11 | INVEPT: descriptor decode, type checked against the reported capability, invalidate the shadow | SDM 33.3 INVEPT; KVM `handle_invept` | no |
| A12 | INVVPID: descriptor decode, type check, invalidate | SDM 33.3 INVVPID; KVM `handle_invvpid` | no |
| A13 | A VMX instruction executed by L2 is reflected to L1 unconditionally, so three-level nesting works | KVM `nested_vmx_l1_wants_exit`, the `EXIT_REASON_VMON` group | no |

### B. VM entry: building the VMCS that runs L2 (vmcs02)

| # | Requirement | Established by | Status |
|---|---|---|---|
| B1 | A second real VMCS per processor, switched to by VMLAUNCH/VMRESUME and away from on an exit to L1 | KVM `vmx_switch_vmcs`, `vmx->nested.vmcs02` | no |
| B2 | Launch-state tracking, so a freshly cleared vmcs02 gets `vmlaunch` and a launched one `vmresume` | SDM 33.3 VMLAUNCH/VMRESUME | no |
| B3 | Pin-based controls: union of L1's request and ours, with the preemption timer ours alone | KVM `prepare_vmcs02_early`, PIN CONTROLS block | no |
| B4 | Primary controls: union of L1's and ours; interrupt-window and NMI-window exiting taken from L1 only | KVM `prepare_vmcs02_early`, EXEC CONTROLS block | no |
| B5 | Secondary controls: some taken *only* from vmcs12, the rest unioned | KVM `prepare_vmcs02_early`, SECONDARY EXEC block | no |
| B6 | Entry controls from L1, except the ones that follow from EFER, which are recomputed | KVM `prepare_vmcs02_early`, ENTRY CONTROLS block | no |
| B7 | Exit controls are **ours**, not L1's - the hardware exit comes to us and L1's exit is emulated | KVM `prepare_vmcs02_early`, EXIT CONTROLS block and its comment | no |
| B8 | Exception bitmap: bitwise or of what L1 wants to trap and what we must trap | KVM `prepare_vmcs02` comment on `vmx_update_exception_bitmap` | no |
| B9 | CR0/CR4 guest-host masks merged, and the read shadows set from vmcs12 rather than from the effective register | KVM `prepare_vmcs02`, `nested_read_cr0`, `nested_read_cr4` | no |
| B10 | Guest state copied from vmcs12: segments, descriptor tables, RSP/RIP/RFLAGS, activity state, interruptibility, pending debug exceptions | SDM 27.4; KVM `prepare_vmcs02_rare` | no |
| B11 | MSR bitmap **merged**, not taken from either side: an MSR either of us wants must exit | KVM `nested_vmx_prepare_msr_bitmap` | no |
| B12 | I/O: unconditional I/O exiting forced rather than merging bitmaps, because every I/O access needs an exit here | KVM `prepare_vmcs02_early`, `CPU_BASED_UNCOND_IO_EXITING` | no |
| B13 | TSC offset composed across levels, and the multiplier too if scaling is offered | KVM `kvm_calc_nested_tsc_offset` | no |
| B14 | Entry event injection: interruption-information field, error code and instruction length taken from vmcs12 on a launch | SDM 27.8.3; KVM `prepare_vmcs02_early`, interrupt/exception block | no |
| B15 | VM-entry consistency checks on vmcs12's controls, host state and guest state, failing with error 7, error 8, or an entry-failure exit | SDM 29.2, 29.3; KVM `nested_vmx_check_controls`, `nested_vmx_check_host_state`, `nested_vmx_check_guest_state` | no |
| B16 | VM-entry MSR-load list processed, with an MSR-load-failure exit | SDM 27.8.2; KVM `nested_vmx_load_msr` | no |
| B17 | A VPID for L2 distinct from L1's, or a TLB flush on every transition instead | KVM `nested_vmx_transition_tlb_flush` | no |
| B18 | The preemption timer armed for L2 from vmcs12's value and cancelled on exit | KVM `vmx_start_preemption_timer` | n/a |

### C. VM exit: from L2 back to L1

| # | Requirement | Established by | Status |
|---|---|---|---|
| C1 | Guest state saved back into vmcs12: CR0/CR3/CR4, RSP/RIP/RFLAGS, segments, activity state, interruptibility | SDM 30.3; KVM `sync_vmcs02_to_vmcs12`, `sync_vmcs02_to_vmcs12_rare` | no |
| C2 | Exit information written into vmcs12: reason, qualification, guest-linear and guest-physical address, instruction length and information | SDM 30.2; KVM `prepare_vmcs12` | no |
| C3 | The entry interruption-information field's valid bit cleared on exit, emulating what hardware does | SDM 30.2; KVM `prepare_vmcs12` and its comment | no |
| C4 | IDT-vectoring information and error code written for an event that was mid-delivery when the exit happened | SDM 30.2.4; KVM `vmcs12_save_pending_event` | no |
| C5 | Double fault and triple fault never reported as occurring during event delivery | SDM 30.2.4; KVM `vmcs12_save_pending_event`, first branch | no |
| C6 | Launch state set to launched on a successful exit, and *not* on an entry-failure exit | SDM 33.3; KVM `prepare_vmcs12` | no |
| C7 | L1's host state loaded into the VMCS that runs L1: CR0/CR3/CR4, RIP/RSP, segments, descriptor tables, EFER, PAT, SYSENTER | SDM 30.5; KVM `load_vmcs12_host_state` | no |
| C8 | VM-exit MSR-store and MSR-load lists processed, with a VMX abort on failure | SDM 30.4, 30.6; KVM `nested_vmx_store_msr` | no |
| C9 | Events queued for injection into L2 dropped on the way out | KVM `nested_vmx_vmexit`, the `kvm_clear_exception_queue` block | no |
| C10 | An entry failure hardware detects surfaces to L1 as `VMfailValid`, not as an exit | SDM 33.3; KVM `nested_vmx_vmexit` failure path | no |

### D. The reflect-or-handle decision

| # | Requirement | Established by | Status |
|---|---|---|---|
| D1 | Two questions, in order: does *this VMM* want the exit, and only then does L1 want it | KVM `nested_vmx_reflect_vmexit`, `nested_vmx_l0_wants_exit`, `nested_vmx_l1_wants_exit` | no |
| D2 | We always take: NMI, external interrupt, EPT violation, EPT misconfiguration, preemption timer | KVM `nested_vmx_l0_wants_exit` | no |
| D3 | Always reflected: triple fault, task switch, CPUID, INVD, XSETBV, the VMX instructions, invalid guest state | KVM `nested_vmx_l1_wants_exit` | no |
| D4 | Conditional on L1's controls: HLT, INVLPG, RDPMC, RDTSC, MOV DR, MWAIT, MONITOR, PAUSE, RDRAND, RDSEED, WBINVD, descriptor-table access, INVPCID, XSAVES/XRSTORS | KVM `nested_vmx_l1_wants_exit` | no |
| D5 | Exception exits filtered through vmcs12's exception bitmap, with the page-fault error-code mask and match applied | SDM 27.6.3; KVM `nested_vmx_is_page_fault_vmexit` | no |
| D6 | CR-access exits filtered through vmcs12's CR0/CR4 guest-host masks and the CR3-target list | KVM `nested_vmx_exit_handled_cr` | no |
| D7 | I/O exits filtered through vmcs12's I/O bitmaps | KVM `nested_vmx_exit_handled_io` | no |
| D8 | MSR exits filtered through vmcs12's MSR bitmap | KVM `nested_vmx_exit_handled_msr` | no |
| D9 | The exits this VMM already owns for its own reasons - the interrupt command register, watched pages, the sleep port, the preemption timer that drives the log - keep working while L2 runs | this tree: `on_interrupt_command`, `on_ept_violation`, `arm_controller_poll` | no |

### E. Nested EPT

| # | Requirement | Established by | Status |
|---|---|---|---|
| E1 | A shadow EPT per EPTP12, composing L2-GPA→L1-GPA (L1's tables) with L1-GPA→HPA (ours) | KVM `nested_ept_init_mmu_context`, `nested_ept_get_eptp` | no |
| E2 | Two permission sets combined per page - read, write, supervisor execute and user execute all intersected | KVM `kvm_init_shadow_ept_mmu` and the shadow-page permissions it installs | no |
| E3 | Populated lazily from EPT violations taken while L2 runs, since eager construction cannot know what L2 will touch | KVM: L0 always takes the EPT violation - `nested_vmx_l0_wants_exit` | no |
| E4 | An EPT violation caused by a gap in *L1's* tables reflected to L1, with the qualification and guest-physical address it would have seen | KVM `nested_ept_inject_page_fault` | no |
| E5 | An EPT violation caused by a gap in *our* tables, or by a page we watch, handled here and never shown to L1 | KVM `nested_vmx_l0_wants_exit`, EPT-violation case | no |
| E6 | EPT misconfiguration always ours, never L1's, because L2 never walks L1's tables directly | KVM `nested_vmx_l0_wants_exit`, EPT-misconfig case and its comment | no |
| E7 | L1's INVEPT invalidates the shadow for the named EPTP, and an INVEPT type we do not report is refused | SDM 33.3 INVEPT; KVM `handle_invept` | no |
| E8 | A change to *our* EPT - arming a page watch, protecting a region - invalidates every shadow built over it | this tree: `invalidate_ept`, `ept_generation` | no |
| E9 | The memory type of a shadow leaf derived from the MTRRs as our own tables are, not taken from L1 | SDM Table 31-6 reserved-bit rule as already applied in `initialize_ept`; this tree: `mtrr_state::type_of` | no |
| E10 | Large-page shadow leaves where both levels permit, to bound the size of the shadow | SDM 31.3.2 | no |
| E11 | A bounded pool for shadow paging structures, with flush-and-rebuild on exhaustion rather than failure | this tree: the `ept` pool and the `out_of_ept_entries` precedent | no |
| E12 | The module and every watched page remain unreachable from L2 | this tree: `protect_module`, `watch_guest_page_writes` | no |

### F. Capability reporting

| # | Requirement | Established by | Status |
|---|---|---|---|
| F1 | Control MSRs report a subset of the hardware's allowed-1 settings, with allowed-0 preserved | SDM A.3.1 through A.5 | yes |
| F2 | `IA32_VMX_BASIC` reports our own revision id, 4096-byte regions, write-back, and the TRUE MSRs | SDM A.1 | yes |
| F3 | `IA32_VMX_MISC` narrowed: no VMWRITE to exit-information fields, CR3-target count zero | SDM A.6 | yes |
| F4 | `IA32_VMX_VMCS_ENUM` reports the highest index the shadow honours | SDM A.9 | yes |
| F5 | `IA32_VMX_EPT_VPID_CAP` reports exactly the EPT and VPID features we honour | SDM A.10 | no - currently zero |
| F6 | `IA32_VMX_VMFUNC` reports what we honour | SDM Table 27-7 bit 13; SDM 33.3 VMFUNC | n/a - zero, and VMFUNC takes `#UD` |
| F7 | `IA32_FEATURE_CONTROL` virtualized with write-once semantics | SDM Table 7-1, bit 0 | yes |
| F8 | The CR0/CR4 fixed-bit MSRs passed through, since they describe the real processor | SDM A.7, A.8 | yes |

### G. What Hyper-V specifically needs

Hyper-V is the target, so these are called out separately. Each is a row
above, named here so that "can Hyper-V launch" has an answer rather than an
essay.

| Requirement | Row | Status |
|---|---|---|
| EPT, without which it does not start at all | E1-E12, F5 | no |
| Secondary controls, unrestricted guest, VPID | B5, F5 | no |
| A VMLAUNCH that actually runs L2 | B1-B17 | no |
| Exit reflection for the exits its own guest takes | C1-C10, D1-D9 | no |
| MSR bitmap merging, since it traps a great many MSRs | B11, D8 | no |
| Event injection into L2 and back out again | B14, C4 | no |

**So the answer today is no, and the first blocking row is E1.** Nothing in
sections B, C, D or E is implemented. Sections A and F are, apart from the
two invalidation instructions and the EPT capability MSR, which only mean
anything once E exists.

## Nested EPT: the design, and the SDM facts that decided it

Settled by reading, before any of it was written, so that the next person
does not re-derive it. Every claim here has a section number that was
actually looked up.

### The composition is a walk of L1's tables and a walk of ours

L2-GPA → L1-GPA comes from L1's EPT; L1-GPA → HPA comes from ours. Ours is
an identity map of the first 512 GB (`initialize_ept`), so the second walk
contributes no *address* translation - only permissions and a memory type.
That is what makes this tractable here and it is worth stating plainly,
because it is a property of this VMM rather than of the architecture: a VMM
whose own EPT relocated guest memory would have real work to do in the
second walk.

The shadow is a single set of tables mapping L2-GPA directly to HPA.

### Permissions are intersected, then *normalised*, and the normalisation is not optional

Intersecting read, write, supervisor-execute and user-execute across both
walks is the obvious half. The half that is easy to get wrong is that a
naive intersection can produce an entry the processor rejects outright.

SDM 31.3.3.1 lists the EPT misconfiguration conditions, and two of them
bite here:

- "Bit 0 of the entry is clear (indicating that data reads are not allowed)
  and any of the following hold: Bit 1 is set (indicating that data writes
  are allowed)."
- "... The processor does not support execute-only translations and either
  of the following hold: Bit 2 is set ... the 'mode-based execute control
  for EPT' VM-execution control is 1 and bit 10 is set."

So an intersection that leaves write or execute set with read clear is a
**misconfiguration**, not a restrictive mapping - and a misconfiguration is
an exit this VMM would then take on its own tables, for ever. After
intersecting: if read is clear, write must be cleared too, and execute and
user-execute must be cleared unless execute-only translations are reported
in IA32_VMX_EPT_VPID_CAP bit 0 (SDM A.10).

An all-zero result is *not* a problem and needs no special case: SDM 31.3.2
says "An EPT paging-structure entry is present if any of bits 2:0 is 1;
otherwise, the entry is not present", with the note that bit 10 counts too
when mode-based execute control is on. No permissions means not present
means an EPT violation, which is exactly what is wanted.

### The exit qualification for a reflected violation has to be synthesised

SDM Table 30-7 defines bits 3, 4, 5 and 6 of the EPT-violation exit
qualification as "the logical-AND of bit 0 / bit 1 / bit 2 / bit 10 in the
EPT paging-structure entries used to translate the guest-physical address".
Hardware computed those over the *shadow*, so they describe the
intersection - not what L1's own tables say. Reflecting them unchanged
would tell L1 its own tables denied an access they permit.

They must therefore be recomputed from the walk of L1's tables alone, and
the walk has to accumulate the AND across every level it traverses, not
just read the leaf. Note 2 to the same table adds a case to get right:
bits 5:3 are "cleared to 0" if any entry used was not present, or if
4-level EPT is in use and the address sets bits in 51:48.

Bits 0, 1 and 2 - the access type - and bits 7, 8 and 12 come from
hardware's qualification unchanged, since they describe the access and the
NMI-unblocking state rather than the tables.

### Lazy fill needs no invalidation, which is the single most useful fact found

SDM 31.4.3.4: "Because a logical processor does not cache any information
derived from EPT paging-structure entries that are not present ... or
misconfigured ..., it is not necessary to execute INVEPT following
modification of an EPT paging-structure entry that had been not present or
misconfigured."

And SDM 31.4.3.1: "An EPT violation invalidates any guest-physical
mappings (associated with the current EPTRTA) that would be used to
translate the guest-physical address that caused the EPT violation."

Together: filling in a shadow entry that was absent, on the fault that
found it absent, requires no INVEPT at all. The fill path is therefore
cheap and has no invalidation ordering to get wrong. Only *removing* or
*narrowing* a shadow entry needs one, and 31.4.3.4 lists exactly which
changes those are.

### L1 may widen its own permissions without any INVEPT, so a fill-only shadow is wrong

The most important consequence of the paragraph above, and the one easiest to
get wrong, because it makes the *obvious* design incorrect rather than slow.

SDM 31.4.3.4, in full: "Software **may** use the INVEPT instruction after
modifying a present EPT paging-structure entry ... to change any of the
privilege bits 2:0 from 0 to 1. Failure to do so may cause an EPT violation
that would not otherwise occur. Because an EPT violation invalidates any
mappings that would be used by the access that caused the EPT violation ...,
an EPT violation will not recur if the original access is performed again,
even if the INVEPT instruction is not executed."

"May", not "should". A guest hypervisor is entitled to relax a permission in
its own tables and never invalidate anything, because on real hardware the
resulting violation is self-clearing: the fault invalidates the stale
mapping and the retried access succeeds.

A shadow that is only ever *filled* - populate on a violation when the entry
is absent, otherwise reflect - breaks exactly there. The shadow entry is
present but narrow, L1 has since widened its own, no INVEPT arrives, and the
violation recurs for ever with L1 seeing nothing wrong with its tables.

So the fill path must **re-derive the permissions of an entry that is already
present**, on every violation, not only install absent ones. That is one
extra intersect-and-write on a path that has already paid for a walk, so it
costs nothing measurable - but it has to be deliberate, and the reason has to
be written down, because the code looks redundant without it.

### Over-invalidation is always architecturally safe

SDM 31.4.3.2: "A logical processor may invalidate any cached mappings at
any time. For this reason, the operations identified above may invalidate
the indicated mappings despite the fact that doing so is not required."

Every INVEPT type's description in Chapter 33 says the same locally - "It
may invalidate other mappings as well". This is what licenses the cheap
answer everywhere invalidation is needed: throw the whole shadow away and
let it be rebuilt. It costs faults, never correctness, and it removes any
need for per-address invalidation bookkeeping.

The same fact settles what to do when the shadow's table pool runs out:
discard the whole shadow and start again, rather than failing the entry.
The pool is a cache of derived state, and that is the property that makes
the bound safe.

### Accessed and dirty flags are not offered, deliberately

Bit 6 of the EPTP enables them (SDM Table 27-9), and SDM 31.3.5 then makes
"processor accesses to guest paging-structure entries ... treated as
writes", so every page holding one of L2's own page tables would have to be
writable in the shadow or take violations. It also makes the flags sticky
and adds an INVEPT requirement whenever software clears one (31.4.3.4).

IA32_VMX_EPT_VPID_CAP bit 21 is therefore reported clear, which SDM
29.2.1.1 turns into a hard VM-entry check on L1's behalf: "Bit 6 (enable
bit for accessed and dirty flags for EPT) must be 0 if bit 21 of the
IA32_VMX_EPT_VPID_CAP MSR ... is read as 0". So L1 cannot ask for what is
not implemented, and does not have to be refused later.

Page-modification logging goes with them: SDM 31.3.6 makes it depend on the
same flags.

### The shadow is keyed by L1's EPTP, and 4-level only

SDM 31.4.2 defines EPTRTA as bits 51:12 of the EPTP, and says mappings are
associated with it rather than with the whole pointer - so two EPTP values
differing only in memory type or page-walk length share cached mappings.
The shadow is therefore keyed on bits 51:12 of EPTP12 and rebuilt when
those change.

Only a page-walk length of 4 is offered: IA32_VMX_EPT_VPID_CAP bit 6
reports 4-level, bit 7 reports 5-level (SDM A.10), and 29.2.1.1 checks
L1's EPTP bits 5:3 against what is reported. Refusing 5-level keeps one
walk shape rather than two. It also brings a check with it - SDM 31.3.2:
"With 4-level EPT, bits 51:48 of the guest-physical address must all be
zero; otherwise, an EPT violation occurs."

### Memory type comes from our MTRR derivation, not from L1

The memory type of a leaf lives in bits 5:3 of the last entry only (SDM
Tables 31-3, 31-5, 31-7), and SDM 31.3.7.2 gives the legal encodings - 0,
1, 4, 5, 6 - with "Other values are reserved and cause EPT
misconfigurations".

Whose choice wins is a real question with a defensible answer: the type
describes a physical page, and which type a physical page needs is settled
by the MTRRs, which `initialize_ept` already derives from. L1's choice for
its own guest is a policy about memory it does not own the physical layout
of. So the shadow takes ours, by the same `mtrr_state::type_of` call the
identity map uses.

This is a **known divergence**, recorded rather than hidden: an L1 that
maps a page uncacheable while our derivation says write-back gets
write-back. It is the safe direction for correctness of the *machine* - a
cacheable mapping over a device range is the defect that derivation exists
to fix - and the unsafe direction for fidelity to L1. Worth revisiting if
an L1 is ever seen to depend on it.

Note also that non-leaf entries have no memory-type field at all: SDM
Table 31-6 reserves bits 6:3 of a PDE that references a page table, "must
be 0", which `initialize_ept` already comments on for its own splits.

### Large-page leaves where both walks permit

A shadow leaf may be 2 MB or 1 GB only if L1's walk ended at that level
*and* our own entry covering the result is a leaf at that level or larger
*and* the memory type is uniform across it. Our own tables are 2 MB leaves
almost everywhere, so this is the common case rather than an optimisation:
without it a guest with gigabytes of memory needs a shadow page table per
2 MB.

The reserved-bit rules differ per level and have to be respected: SDM Table
31-3 reserves bits 29:12 of a 1 GB PDPTE and Table 31-5 bits 20:12 of a
2 MB PDE, and a set reserved bit is a misconfiguration rather than a
wrong address.

Capability reporting has to agree: IA32_VMX_EPT_VPID_CAP bits 16 and 17
report 2 MB and 1 GB support (SDM A.10). They are reported only if the
hardware reports them, since the shadow's leaves are real hardware entries.

### EPT misconfiguration is always ours

L2 never walks L1's tables - it walks the shadow - so a misconfigured entry
encountered while L2 runs is one this VMM wrote. It is never reflected.
That is also what KVM concludes, in `nested_vmx_l0_wants_exit`.

The corollary is that a misconfiguration exit while running L2 is a bug
here, and the misconfiguration conditions above are the list of ways to
cause one. Since SDM 30.2.1 does not list EPT misconfiguration among the
exits that save an exit qualification, there is no qualification to read:
the guest-physical address field is the only evidence, and it *is* valid
for both violation and misconfiguration.

### What still has to be decided by measurement

- Whether one shadow per processor or one per EPTP12 shared between
  processors. Per processor is simpler and cannot race; shared halves the
  fault cost when L1 runs the same L2 on many processors, which is the
  normal case. Start per processor, measure the fault count, and only then
  share.
- The pool size. It is a straight trade of memory against how often the
  flush-and-rebuild path runs, and neither side can be guessed - it needs
  the fault count from a real L1.
