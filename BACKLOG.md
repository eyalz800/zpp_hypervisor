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

**Reopened and closed again: it was called and it always failed.** The
call site passed `&type` where the instruction wants the type itself.
SDM 33.3, INVEPT, opens with `INVEPT_TYPE := value of register operand`
and its Op/En row makes that operand ModRM:reg, which is the first
argument - so the register held a stack address, which is neither 1 nor
2, and the next line of the same pseudocode applies: "IF
IA32_VMX_EPT_VPID_CAP MSR indicates that processor does not support
INVEPT_TYPE THEN VMfail(Invalid operand to INVEPT/INVVPID)". Every
invalidation this VMM ever performed failed.

It was silent because a failed invalidation has no symptom of its own -
only a stale translation, whose symptom is a watch that does not fire or
a protection that does not bite, attributed elsewhere. The `log("invept
failed after an ept change")` line beside it was firing all along, in a
log nobody had reason to read while chasing something else.

Two things it is worth noticing about the shape of the bug. The parameter
was `void *`, so `&type` type-checked and the type itself would not have;
the signature now takes `std::uint64_t` and the wrong spelling no longer
compiles. And the INVVPID call site three thousand lines away had it
right - `reinterpret_cast<void *>(invvpid_single_context)` - so the tree
disagreed with itself, which is the tell that was there to be found.

Verified in the built release binary rather than by reading: the call site
now assembles to `movl $0x1, %edi` before `callq` into the stub, where it
previously computed a `leaq` of a stack slot.

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

### 7. S3/S4 leaves the machine unvirtualized — WRITTEN, NEVER RUN

The resume path exists now. It has never executed a single instruction, on
any machine or emulator, so the honest status is "written" and the switch
that turns it on is off. What is real is that all three stages are code
rather than design: the entry side quiesces, the vector is found and read,
and the platform's resume can be pointed at this VMM's own trampoline.

Read `hypervisor/include/zpp/hypervisor/power.h` first: it states what each
of S3, S4 and S5 costs, with the citations.

**Three switches, all off, and now ordered.** `power::quiesce_on_sleep`,
`power::observe_waking_vector`, `power::resume_from_waking_vector`. Each
comment says what would turn it on, and two `static_assert`s in that header
enforce the order — the resume requires the other two, because the resume
re-arms a port that only the quiesce path keeps, and because a resume
attempt with no log line saying what the guest's own vector was is one whose
failure cannot be read afterwards.

**The whole feature is debug-only, by accident of where the finder lives.**
`uefi_loader/src/sleep_control.cpp` is wrapped in `#if ZPP_DIAG`, and
`cmake/uefi-loader/CMakeLists.txt` hardcodes `ZPP_DIAG=0` for release. So in
a release build the loader finds no PM1 control register, hands over port
zero, `on_io_instruction` returns `false` on its first test, and every one of
these switches is inert whatever it is set to. That is not wrong — the
switches are unproven and the channel is the only way to read a result — but
it is not stated anywhere else and it means "turn the switch on and ship it"
is not a thing that can happen. Making it release-capable is a separate
decision: it needs the finder out from behind `ZPP_DIAG`, and it needs a
reason to trust the path without a channel to watch it on.

The Windows and Linux loaders pass zeroes for all four sleep fields
(`windows_loader/src/main.cpp:104`, `linux_loader/src/main.c:139`), so this
is UEFI-only as well. That one is deliberate and cheap to change: both would
need an ACPI table walk of their own.

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
  back, and it is now performed *at the width the guest's own instruction
  used* rather than at the register's width from the fixed table. That was
  the wrong way round: `OUT`'s source is architecturally AL, AX or EAX and
  the exit qualification says which (SDM Table 30-5, bits 2:0), so the guest's
  size is the one that reproduces the instruction. The table's width is now
  only the fallback for the encodings Table 30-5 leaves undefined. The
  rejected version was wrong in the direction that *writes bits the guest did
  not*: a two-byte guest access issued as four puts zeroes into bits 31:16 of
  whatever the platform decoded there.

#### What the resume path does, in order

Three things made this smaller than it looked, and all three were verified
against EDK2's own sources in `build/*/_deps/edk2-src` rather than recalled.

1. **The trampoline already exists.** `arch/x86_64/ap_start_up.S` climbs
   real mode to long mode on the host page table, parameterised by an
   `ap_start_up_area`. The ACPI real-mode waking protocol and a SIPI
   produce the *same* entry state: EDK2's `AsmTransferControl`
   (`MdeModulePkg/Universal/Acpi/BootScriptExecutorDxe/X64/S3Asm.nasm`)
   clears CR0.PE, CR0.PG, CR4.PAE and EFER.LME and then far-jumps to
   `(vector >> 4):(vector & 0xf)`, while a SIPI enters at `CS = page << 8,
   IP = 0` - and for a page-aligned vector those are the same address.
   Confirmed instruction by instruction: `shrd ebx, ecx, 20` followed by
   `and ecx, 0xf` and `mov bx, cx` leaves bits 31:16 of `EBX` holding bits
   19:4 of the vector and bits 15:0 holding its low nibble, and those four
   bytes are stored over the operands of a `jmp far ptr16:16` - offset first,
   segment second. So the blob is reused with only `entry` rewritten. No
   second copy and no new assembly, as predicted.

   Two things the prediction missed, both now handled:

   - **The vector need not be page aligned.** `apply_start_up` takes a
     *start-up IPI vector*, which is a page number, so it can only ever begin
     a guest at a page boundary - and the ACPI protocol puts the low four bits
     in IP. `apply_waking_vector` applies the firmware's own decoding
     afterwards as a correction, which for an aligned vector writes exactly
     what `apply_start_up` already wrote and is therefore self-checking. The
     vector is also refused above one megabyte, because a sixteen-bit segment
     cannot name anything higher and the firmware would truncate rather than
     refuse.
   - **The blob is not idempotent.** It relocates its GDT pointer and three
     far pointers by adding the page's base to them *in place*, which is
     correct exactly once. `start_application_processor` already restores
     `assembly_owned` before every start for this reason; the arm does the
     same, or a resume running on a blob some processor had already climbed
     would fault on its first far jump, before it has an IDT, and reset the
     machine - indistinguishable from firmware that never reached the vector.
2. **Memory is already right.** S3 preserves it, and everything this VMM
   keeps describes physical memory that has not moved: host page table,
   host GDT and IDT, EPT, the module. So `initialize_os_page_table`,
   `initialize_host_page_table`, `initialize_host_gdt`,
   `initialize_host_idt`, `initialize_ept` and `protect_module` must **not**
   run again.

   That is done by one local in `main`: `first_launch = (0 == cpuid) &&
   !resuming_from_sleep`, replacing the three `if (0 == cpuid)` guards.
   `initialize_start_up_memory` is inside it too, and for a sharper reason
   than the rest - it rewrites the very page the resuming processor is coming
   back through.

   What *does* re-run is everything per-processor, and that is exactly what
   the power transition destroyed: `initialize_vmx` (which re-stamps both
   regions' revision identifiers), `enter_root_mode` (which calls
   `enable_vmx_in_feature_control`, since IA32_FEATURE_CONTROL is zero again
   after a reset - SDM 26.7 - then VMXON, VMCLEAR, VMPTRLD), and `setup_vmcs`.
   No new copy of any of that: the resume goes through `launch_on_cpu` and
   `main` like any other processor, which it has to, because the exit handler
   is a lambda inside `main`'s `vm_launch` call and cannot be reached from
   anywhere else.
3. **The application processors need nothing.** The guest sends
   INIT-SIPI-SIPI again during its own resume, and the existing adoption
   path handles that. What does need doing is rewinding the per-processor
   bookkeeping, and the list is longer than the four originally named -
   `rewind_for_resume` has it. The two that would have been silent bugs:

   - `started_by_start_up_ipi[]`. `apply_start_up` returns early for a
     processor whose entry is already set, and every application processor's
     is, from before the suspend. Left set, the guest's re-sent SIPI is
     refused and the early return is indistinguishable from a processor that
     never started.
   - `start_up_launched[]`. `wait_for_ept_acknowledgement` waits on every
     processor it finds marked launched and probes silent ones with an NMI.
     Left set, the first EPT change after a resume spends the full budget
     per processor waiting on processors the platform has reset.

The order, then:

- **On the way down** (`arm_resume_from_sleep`), after the channel flush and
  before the quiesce: re-read and re-check the FACS, save
  `FirmwareWakingVector` in `guest_waking_vector` and
  `XFirmwareWakingVector` beside it, restore the trampoline's
  `assembly_owned`, point `entry` at `zpp_resume_from_sleep_main`, and write
  the trampoline page's address into `FirmwareWakingVector` with
  `XFirmwareWakingVector` **zeroed**. Zeroing the extended field is what
  forces the real-mode protocol: EDK2's `S3Resume.c:519` takes the sixteen-bit
  vector only when `XFirmwareWakingVector == 0` and a protected- or long-mode
  path otherwise. Both fields go out in one `write_guest_physical`, with the
  global lock and flags between them read back and written unchanged - those
  belong to the guest and the firmware. Then the quiesce's WBINVD, then the
  OUT. A refusal at any point leaves the table untouched and the machine
  resumes unvirtualized, which is what it did before.
- **On the way up** (`zpp_resume_from_sleep_main`): force every surviving lock
  open, disarm (trampoline `entry` back to `zpp_ap_start_up_main`, FACS back
  to the guest's own two vectors), rewind the bookkeeping, then capture a
  context and `launch_on_cpu` into `main` exactly as an application processor
  does.

The VMCS is rebuilt rather than reused **on purpose**. Only the quiescing
processor's region was VMCLEARed before the power went; every other one was
active on a processor that left VMX operation, which is the case SDM 27.11.1
says may corrupt it. `enter_root_mode`'s VMCLEAR is the architectural repair
for that - SDM 27.11.1: "the VMCLEAR instruction initializes any
implementation-specific information in the VMCS region referenced by its
operand" - so no explicit zeroing of the region is needed, and the earlier
plan's "zero the VMCS region" step was dropped as redundant rather than
implemented.

#### Locks survive S3, and forcing them open is the only way out

Not in the original design and the largest thing missed by it. A spin lock is
a byte in memory; S3 preserves memory; and every one of these is taken inside
a VM exit, so the write that slept the machine can land while *another*
processor holds one. That processor no longer exists, nothing will ever
release what it held, and the first use after the resume waits for ever - a
resume that never arrives, which from outside is a dead machine.

Four of them, found by grepping for the type rather than by reasoning about
which mattered:

- `hypervisor::mapping_window_lock` - held across every guest memory access
  and across the whole of `rebuild_channel_queue`.
- `hypervisor::start_up_lock` - held across a processor's entire launch.
- `log_storage::m_lock` - held across a `push_back`, which allocates, so a
  transition caught there leaves this **and** the heap's held.
- `zpp::heap::m_lock` - any allocation at all.

Each now has an `abandon_lock` (or `pump::abandon_gates`) with the reason on
it, called as the first thing the resume does. The heap's carries a residual
risk stated rather than glossed: the power can land *inside* `split_block` or
`coalesce`, leaving the free list half updated, and there is no better answer
- re-running `init` would orphan every live allocation including the log's
nodes. A window of a few instructions is accepted against a hang that is
certain.

The diagnostic channel's `diag::gate` is the same shape with a different
failure: it never spins, so a held gate does not hang - it makes the channel
permanently silent, on the one path whose entire job is to report whether this
worked. A resume that worked and a resume that hung would look identical.
`pump::abandon_gates` releases them all.

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
- **Zeroing the VMCS region before VMCLEAR on the way up.** Planned, then
  dropped as redundant: SDM 27.11.1 says VMCLEAR "initializes any
  implementation-specific information in the VMCS region referenced by its
  operand", which is the whole of what a possibly-corrupted region needs.
  `initialize_vmx` re-stamps the revision identifier on both regions anyway.
- **A second resume entry point that does not go through `main`.** Considered
  first and abandoned: the exit handler is a lambda passed to `vm_launch` from
  inside `main`, so anything that ends up running the guest has to come
  through `main` or duplicate four hundred lines of exit handling. The resume
  therefore uses `launch_on_cpu` like every other processor, and the
  once-per-boot work is skipped by one local rather than by a parallel path.
- **Rewinding `resuming_from_sleep` after the resuming processor launches.**
  There is nowhere to do it - `main` does not return - so the flag stays set
  for the rest of the boot. That is harmless for `first_launch`, which also
  tests `0 == cpuid`, but it is *not* harmless for `apply_waking_vector`:
  every application processor the guest starts after a resume comes through
  the same branch, and without the slot test each would be entered at the boot
  processor's waking vector instead of its own. The test is the fix; the flag
  is deliberately not cleared.
- **Re-initialising the heap on resume** to recover from a lock left held
  mid-update. It would reset the free list and orphan every live allocation,
  the log's nodes among them. See the note on `abandon_lock`.

#### The experiments, in order, none of them run yet

Each needs `-DZPP_DIAG=ON` so the channel carries the result off the
machine - and note that without it the loader finds no register at all, so
this is a requirement rather than a convenience. Each is one variable, and
the `static_assert`s in `power.h` enforce the order for the last one.

0. **Does the guest tolerate a suspend at all?** Free, and first because it
   is: `system_powerdown` on the QEMU monitor is a genuine S5 ACPI
   power-button request and needs nothing inside the guest. All three
   switches off. Wanted in the channel: `sleep type N entered through port P`
   with a plausible port, and the machine powering off. This exercises the
   recognition, the decode and the flush and risks nothing, because the
   pass-through path is what already boots.
1. **Is the FACS there, and does the guest leave a vector in it?**
   `observe_waking_vector` only. Suspend and resume; the machine comes back
   unvirtualized, as today. Wanted: `guest waking vector V extended 0` with V
   non-zero and below one megabyte. **A zero `FirmwareWakingVector` with a
   non-zero extended one stops the whole approach** - the real-mode protocol
   is the only one this serves - and the answer would have to be found
   somewhere else entirely. A "facs declares N bytes, too short" line means
   the loader's weaker check passed a table this refuses to write to.
2. **Does the quiesce complete?** Add `quiesce_on_sleep`. Suspend and resume.
   Wanted: `sleep_request.stage` reaching `write_issued` (5) and the machine
   actually sleeping. `write_returned` (6) or `re_established` (7) means the
   write did not sleep, which is also a useful result and not a failure.
   A stage stuck at `channel_flushed` (2) means the flush did not return -
   look at the NVMe drain, not at VMX.
3. **Does the arm work without breaking the resume?** Add
   `resume_from_waking_vector`. **Somebody has to be at the machine**: the
   failure mode is a machine that looks like a dead motherboard, and the way
   out is the power button. Wanted, in order, from `resume_request.stage`:
   `armed` (1) in the channel on the way down, then on the way up `entered`
   (2), `disarmed` (3), `rewound` (4), `launched` (5), then ordinary guest
   exits resuming.

   How to read a failure between `armed` and `entered` - the gap where the
   machine is dead and nothing of ours has run:

   - `start_up_trampoline_stage()` on the next boot says whether the
     trampoline ran at all and how far it got, because that byte is in the
     trampoline page and survives. Zero means the firmware never reached our
     vector, and the question is then the firmware's own S3 resume, not this
     code.
   - Anything non-zero and below 6 means it faulted on the climb, at the step
     the value names, and the machine reset - which takes the evidence with
     it, hence the stage byte.
   - `resume_request.stage` still reading `armed` after a successful
     unvirtualized resume means the firmware ignored the field. Check whether
     it re-wrote `XFirmwareWakingVector` itself, which some do.

Nothing here can be exercised under emulation. Bochs has no VT-x, and QEMU's
monitor can request S5 but the interesting path is S3 on real firmware.
Experiment 0 is the exception and is worth taking, precisely because it is
the only one that is.

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

### 16. The local APIC page has no access-width or alignment gate

Found by `tests/watched_page/`, which drives the real
`on_ept_violation` and `filter_local_apic_write` against a real page of
memory. Nothing on the path — not the exit handler, not the filter —
looks at how wide a watched-page access is or where it is aligned.

The SDM is explicit: "All 32-bit registers should be accessed using
128-bit aligned 32-bit loads or stores... Any FP/MMX/SSE access to an
APIC register, or any access that touches bytes 4 through 15 of an APIC
register may cause undefined behavior" (`sdm.txt:170419`, the paragraph
under Table 13-1). KVM enforces exactly that and silently drops
everything else — `apic_mmio_write`, `lapic.c:2440`,
`if (len != 4 || (offset & 0xf)) return 0;`.

What this VMM does instead, each reproduced by a check in the harness:

- A **1-byte** write to offset `0x300` reaches `on_interrupt_command`
  with a command composed from the byte alone: the vector is the byte
  and the delivery mode, level and shorthand are all zero. An interrupt
  is sent that the guest never wrote.
- **2-byte** and **8-byte** writes to `0x300` likewise compose and send.
- A **4-byte write at `0x2fe`** puts two of its bytes into the interrupt
  command register while the filter is only ever asked about offset
  `0x2fe`, which it passes straight through. That is the sharpest of
  them: the register can be changed with the hook that exists to
  intercept it never being consulted.
- **4-byte writes at `0x302` and `0x304`** are applied to the page for
  the same reason.

Smallest fix, and it is small: return `write->value` unchanged from
`filter_local_apic_write` whenever `write->size != 4` or
`(offset & 0xf) != 0`. That makes the hook agree with KVM's dispatcher
and with the SDM, and costs nothing on the traffic that matters, which
is all aligned dwords.

Rejected alternative: gating in `on_ept_violation` instead. It is the
wrong place — the width rule belongs to the device, not to the
emulation, and the controller pages watched beside the APIC have no such
rule.

Not yet measured on a running guest. No driver issues an unaligned APIC
access deliberately, so the trigger is a guest doing something unusual
rather than ordinary traffic — which is an argument for fixing it
cheaply rather than for chasing a reproduction.

Related, and folded in here because the fix touches the same three
lines: the composed command keeps the delivery-status bit. KVM clears it
before composing (`lapic.c:2326`, `val &= ~APIC_ICR_BUSY`) and then
asserts it is never set (`lapic.c:1520`); the SDM makes it read only
("Delivery Status (Read Only)", `sdm.txt:170910`). Harmless today
because `on_interrupt_command` reads only the vector, the delivery mode
and the shorthand.

### 17. A filter that rewrites a non-store stops the processor

Also from `tests/watched_page/`. `on_ept_violation` lets a
`page_watch::filter` return a different value from the one the
instruction would have written. For a plain store it replaces the
operand, which is the whole of the rewrite. For every other form —
`or`, `and`, `add`, a locked read-modify-write — there is nowhere to put
a value the operation would not have produced, so the code refuses.

It refuses by `return false` (`hypervisor.cpp:3564-3567`), and
`hypervisor.cpp:9821` answers a false from `on_ept_violation` with
`on_unhandled_exit`, which stops the CPU. The comment three lines above
says the access "takes the stepping path instead", which is what it
should do: refusing emulation is always safe, stopping never is.

Smallest fix: leave the emulation block rather than returning, so
control reaches the stepping code at `hypervisor.cpp:3669`.

Unreachable today — the only filter armed is the local APIC's, and
`on_interrupt_command` returns the command it was given on every path,
so no rewrite ever fires. That is exactly why it is worth writing down:
the next filter is what finds it, and it will find it as a halted
processor rather than as a refused emulation.

Two smaller things in the same guard, both asserted by the harness:

- The guard tests `memory_operation::store` alone and so refuses
  **exchange**, for no reason: `apply()` returns the operand for an
  exchange exactly as it does for a store, so replacing `store->operand`
  would honour a rewrite of one correctly.
- `filter_local_apic_write` returns `write->value` whatever
  `on_interrupt_command` answered, so a handler that *modifies* a
  command is silently ignored on the emulated path. The stepped path
  does honour it (`hypervisor.cpp:5942-5953`), so the two paths disagree
  about what a handler's return value means. Smallest fix: return the
  handler's value at `hypervisor.cpp:5826`.

### 18. The stepped read-back is four bytes with no bound

`on_monitor_trap_flag` reads four bytes from the offset the violation
resolved (`hypervisor.cpp:3786-3792`) to report what the guest's own
instruction wrote. Nothing bounds it to the page, so a step armed at
page offset `0xffe` reads two bytes off the end of the watched page —
the very straddle `on_ept_violation` refused to emulate eighty lines
earlier.

Harmless for the pages watched today, whose registers are dword aligned,
and a wrong value handed to a handler if that ever stops being true.
Smallest fix: clamp the read to the page, or decline to notify when
`offset + 4 > page_size`. Reproduced by a check in
`tests/watched_page/`.

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

### 19. `decode_watched_page_fully` is dead, and its comment is inverted

`hypervisor/include/zpp/hypervisor/hypervisor.h:3754` declares
`static constexpr bool decode_watched_page_fully = false` with fifty
lines of comment explaining why the exit path uses the narrow
store-only decoder rather than the full one, ending "the decoder itself
is kept, tested, and unused by the exit path".

Nothing anywhere reads that constant — `grep` over `hypervisor/`,
`scripts/` and `tests/` finds only its own declaration.
`on_ept_violation` calls `decode_guest_instruction` unconditionally,
which calls the full `arch::x86_64::decode`. The narrow
`decode_memory_store` in `decoder.h` is the one that is unused: its only
caller is `scripts/ci/decoder-test.cpp`.

So the comment describes the opposite of what the code does, and it
describes it at length, which is worse than saying nothing — the triple
fault it records was real and whatever fixed it is not recorded
anywhere. `tests/watched_page/` proves the current state rather than
asserting it: BTS is a form only the full decoder answers, and the
harness emulates one end to end through `on_ept_violation`.

Smallest fix: delete the constant, and move whatever of that comment is
still true onto `decode_guest_instruction`.

A smaller adjacent finding, recorded so it is not mistaken for a defect:
`CMP r/m32, r32` (opcodes `0x38`/`0x39`) is not in the decoder's
read-modify-write group, even though the immediate form `0x83 /7` and
`TEST` against a register both are. Refusing is always safe — the caller
steps — so this is missing coverage rather than a bug, and it costs one
stepped write per occurrence. **Closed** — see the eighth review at the
end of this file, which adds it along with BT/BTS/BTR/BTC against a
register, XADD, INC, DEC and CMPXCHG, and records the defect found while
adding them.

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

### Run: the instruction emulation and the capability MSRs, under Bochs

Experiments 1 and 2 below are done. `verify_nested::present` in the UEFI
loader executes a first-level hypervisor's worth of VMX as the guest of
this VMM, under Bochs' own emulated VT-x, and reports each outcome against
what the SDM says it should be. Every step answers correctly:

- CPUID leaf 1 ECX[5] reports VMX, and the five capability MSRs read back
  as `IA32_VMX_BASIC 0x009810007a707001`, `IA32_VMX_MISC 0x400001e0`,
  `IA32_VMX_VMCS_ENUM 0x36`, `IA32_VMX_PROCBASED_CTLS2 0x00111cee00000000`
  and `IA32_VMX_EPT_VPID_CAP 0x00000f0106134140` - each exactly the
  narrowed set, checked bit for bit against `supported_secondary_controls`
  and `supported_ept_vpid_capabilities`.
- CR4.VMXE reads back set after the guest sets it, and clear after VMXOFF.
- VMXON, then VMXON again, VMPTRST with and without a current VMCS, VMREAD
  with none, VMCLEAR, VMPTRLD, VMCLEAR and VMXOFF - each with the flags
  SDM 33.2 specifies.
- VMWRITE then VMREAD of one field of each of the four widths, each
  reading back exactly what the width says it should: `0xabcd` from a
  16-bit field written `0x1234abcd`, `0x42` from a 32-bit field written
  with a value whose high half was set, and both 64-bit values whole.
- VMWRITE to a VM-exit information field and VMREAD of an encoding with a
  reserved bit set, both VMfailValid.
- INVEPT and INVVPID all-context, both VMsucceed, and INVEPT with an
  unsupported type VMfailValid.

**The probe found one disagreement and the SDM settled it in the VMM's
favour.** A second VMXON while already in VMX root operation: the probe
expected VMfailValid with error 15, and the VMM answered VMfailInvalid.
SDM 33.3 says `VMfail("VMXON executed in VMX root operation")`, and SDM
33.2 defines VMfail as "IF VMCS pointer is valid THEN
VMfailValid(ErrorNumber); ELSE VMfailInvalid" - and VMXON has just set the
current-VMCS pointer to all ones, so there is none. The VMM was right.

That is the whole reason the probe writes its expectations out by hand
instead of taking them from the VMM's own headers: a probe that shares its
constants with the thing it is probing cannot disagree with it, and this
one could.

Two things about running it, both of which cost a run to find:

- The smoke harness needs a `ZPP_VERIFY_HYPERVISOR=ON` build, because the
  marker it greps for only exists there. A plain debug build boots fine
  and reports nothing, which looks exactly like a hang.
- It also needs `ZPP_DIAG=OFF`. With the channel on, the loader reserves
  space on the ESP and then *restarts the machine* so the channel is live
  on the next boot, and under the harness that is the end of the run.

### Run: a second-level guest, entered and exited, under Bochs

Run with `scripts/ci/bochs-nested.sh`, which is separate from the smoke
script beside it because the two have different stop conditions - that one
kills the emulator the moment the hypervisor's own verdict appears, and the
probe runs after it.

Experiment 3 as well. The same probe writes a whole vmcs12 out of the state
the processor is running with and launches a second-level guest that
differs from its hypervisor in exactly one thing - RIP, pointing at two
instructions, `cpuid` then a halt. Measured, on the reflected exit:

- `exit reason 0x0000000000000a` - CPUID, with bit 31 clear, so this was a
  VM exit and not an entry failure.
- `guest rip at exit 0x000000003dd33f50`, which is where the second-level
  guest was entered: RIP is saved at the faulting instruction and the CPUID
  is the first one there.
- `exit instruction length 0x02`, which is the length of CPUID.

That is sections B, C and D executing end to end: the vmcs02 built out of
the shadow, the entry into it, the second-level guest running real
instructions, the exit decided by `l1_wants_l2_exit`, the guest state saved
back into vmcs12, the exit-information fields written, and the guest
hypervisor's own host state loaded so that it resumed at its host RIP with
its own stack.

CPUID is the instruction to launch into, and not arbitrarily: SDM 28.1.2
makes it exit unconditionally in VMX non-root operation, so the
second-level guest cannot fail to produce an exit, and the reflect decision
gives it to the guest hypervisor unconditionally too. A run that produces
no exit is therefore a real failure rather than a guest that happened not
to trap, and the halt after it turns that case into a hang the harness
times out on rather than a wrong answer it reports.

What the launch deliberately does *not* enable is extended page tables in
vmcs12. Without them the second-level guest's physical addresses are the
first level's, which the VMM's own identity map already translates - so
this exercises the entry and the reflection without also depending on the
shadow page-table builder, and a failure has one place to be rather than
two.

The probe launches twice, and the second time its guest hypervisor uses
extended page tables of its own - one page-map level-4 entry over four
one-gigabyte identity leaves, four gigabytes in all. That run produces the
same three correct answers, which means the shadow was built: the VMM
walked the guest hypervisor's tables, composed each gigabyte with its own
2 MB entries, installed the result, and the second-level guest executed
through it.

So `build_shadow_ept`, `compose_ept` against a real guest hypervisor's
tables, and the EPT pointer written into vmcs02 have all executed.

What is still not run:

4. Windows with VBS on and the switch on, on the rig. Everything below the
   guest hypervisor has now answered correctly under an emulator, and
   nothing about that says Hyper-V will boot - it exercises far more of
   this than a two-instruction guest does, and the exits it takes are the
   conditional ones in `l1_wants_l2_exit` that the probe never reaches.

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
| A11 | INVEPT: descriptor decode, type checked against the reported capability, invalidate the shadow | SDM 33.3 INVEPT; KVM `handle_invept` | yes - `on_guest_invept`, both types, and both discard the shadow |
| A12 | INVVPID: descriptor decode, type check, invalidate | SDM 33.3 INVVPID; KVM `handle_invvpid` | yes - `on_guest_invvpid`, all four types, each answered by invalidating our own VPID |
| A13 | A VMX instruction executed by L2 is reflected to L1 unconditionally, so three-level nesting works | KVM `nested_vmx_l1_wants_exit`, the `EXIT_REASON_VMON` group | yes - by `l1_wants_l2_exit`'s default, which reflects |

### B. VM entry: building the VMCS that runs L2 (vmcs02)

| # | Requirement | Established by | Status |
|---|---|---|---|
| B1 | A second real VMCS per processor, switched to by VMLAUNCH/VMRESUME and away from on an exit to L1 | KVM `vmx_switch_vmcs`, `vmx->nested.vmcs02` | yes - `vmcs02`, one page per processor, switched to by `build_vmcs02` and back by `reflect_l2_exit` |
| B2 | Launch-state tracking, so a freshly cleared vmcs02 gets `vmlaunch` and a launched one `vmresume` | SDM 33.3 VMLAUNCH/VMRESUME | yes - `vmcs02_launched`, set only on an exit that is not an entry failure, per SDM 29 step 5 |
| B3 | Pin-based controls: union of L1's request and ours, with the preemption timer ours alone | KVM `prepare_vmcs02_early`, PIN CONTROLS block | yes - union less the preemption timer and posted interrupts |
| B4 | Primary controls: union of L1's and ours; interrupt-window and NMI-window exiting taken from L1 only | KVM `prepare_vmcs02_early`, EXEC CONTROLS block | yes - union, with the two window controls from vmcs12 alone |
| B4a | TPR shadow honoured: the virtual-APIC address validated and written, the TPR threshold copied, and CR8 load and store exiting forced wherever the control is dropped instead | SDM 29.2.1.1, 27.6.8; KVM `nested_vmx_check_tpr_shadow_controls`, `nested_get_vmcs12_pages`, `prepare_vmcs02_early` | yes - and it is the one control a real guest hypervisor was measured setting and not getting |
| B5 | Secondary controls: some taken *only* from vmcs12, the rest unioned | KVM `prepare_vmcs02_early`, SECONDARY EXEC block | yes - unrestricted guest from vmcs12 alone, mode-based execute cleared, EPT and VPID forced on |
| B6 | Entry controls from L1, except the ones that follow from EFER, which are recomputed | KVM `prepare_vmcs02_early`, ENTRY CONTROLS block | yes - vmcs12's, unchanged |
| B7 | Exit controls are **ours**, not L1's - the hardware exit comes to us and L1's exit is emulated | KVM `prepare_vmcs02_early`, EXIT CONTROLS block and its comment | yes - ours, unchanged |
| B8 | Exception bitmap: bitwise or of what L1 wants to trap and what we must trap | KVM `prepare_vmcs02` comment on `vmx_update_exception_bitmap` | yes - bitwise or, with the page-fault mask and match from vmcs12 since we trap no page faults |
| B9 | CR0/CR4 guest-host masks merged, and the read shadows set from vmcs12 rather than from the effective register | KVM `prepare_vmcs02`, `nested_read_cr0`, `nested_read_cr4` | yes - masks unioned, read shadows carrying the whole effective value |
| B10 | Guest state copied from vmcs12: segments, descriptor tables, RSP/RIP/RFLAGS, activity state, interruptibility, pending debug exceptions | SDM 27.4; KVM `prepare_vmcs02_rare` | yes - `guest_state_fields`, one list used in both directions |
| B11 | MSR bitmap **merged**, not taken from either side: an MSR either of us wants must exit | KVM `nested_vmx_prepare_msr_bitmap` | yes - `merge_nested_bitmaps`, cached on the bitmap pointers |
| B12 | I/O: unconditional I/O exiting forced rather than merging bitmaps, because every I/O access needs an exit here | KVM `prepare_vmcs02_early`, `CPU_BASED_UNCOND_IO_EXITING` | n/a - the bitmaps are merged instead; forcing every I/O access to exit would produce exits neither side asked for, and nothing here emulates I/O |
| B13 | TSC offset composed across levels, and the multiplier too if scaling is offered | KVM `kvm_calc_nested_tsc_offset` | yes - the two offsets summed; no multiplier, since TSC scaling is not offered |
| B14 | Entry event injection: interruption-information field, error code and instruction length taken from vmcs12 on a launch | SDM 27.8.3; KVM `prepare_vmcs02_early`, interrupt/exception block | yes - the three fields taken from vmcs12 when its valid bit is set |
| B15 | VM-entry consistency checks on vmcs12's controls, host state and guest state, failing with error 7, error 8, or an entry-failure exit | SDM 29.2, 29.3; KVM `nested_vmx_check_controls`, `nested_vmx_check_host_state`, `nested_vmx_check_guest_state` | partial - the controls are checked against the *narrowed* capability MSRs, the NMI rules of SDM 29.2.1.1 are checked, and the EPT pointer is validated; the guest-state checks are left to the processor and surface as a reflected entry-failure exit |
| B16 | VM-entry MSR-load list processed, with an MSR-load-failure exit | SDM 27.8.2; KVM `nested_vmx_load_msr` | yes - `load_nested_msrs`, with the exit reason 34 and entry-number qualification SDM 29.8 defines; the indices it will write are a list, see below |
| B17 | A VPID for L2 distinct from L1's, or a TLB flush on every transition instead | KVM `nested_vmx_transition_tlb_flush` | yes - a flush on every transition, `nested_transition_flush` |
| B18 | The preemption timer armed for L2 from vmcs12's value and cancelled on exit | KVM `vmx_start_preemption_timer` | n/a |

### C. VM exit: from L2 back to L1

| # | Requirement | Established by | Status |
|---|---|---|---|
| C1 | Guest state saved back into vmcs12: CR0/CR3/CR4, RSP/RIP/RFLAGS, segments, activity state, interruptibility | SDM 30.3; KVM `sync_vmcs02_to_vmcs12`, `sync_vmcs02_to_vmcs12_rare` | yes - `save_l2_state`, the same field list the entry loaded, plus the masked control registers |
| C2 | Exit information written into vmcs12: reason, qualification, guest-linear and guest-physical address, instruction length and information | SDM 30.2; KVM `prepare_vmcs12` | yes - `reflect_l2_exit` |
| C3 | The entry interruption-information field's valid bit cleared on exit, emulating what hardware does | SDM 30.2; KVM `prepare_vmcs12` and its comment | yes - emulated in vmcs12 rather than read back from vmcs02 |
| C4 | IDT-vectoring information and error code written for an event that was mid-delivery when the exit happened | SDM 30.2.4; KVM `vmcs12_save_pending_event` | yes - copied from the processor's own report, which is the guest hypervisor's account since every injected event came from vmcs12 |
| C5 | Double fault and triple fault never reported as occurring during event delivery | SDM 30.2.4; KVM `vmcs12_save_pending_event`, first branch | yes - by the same route: the rule is the processor's and it applied it |
| C6 | Launch state set to launched on a successful exit, and *not* on an entry-failure exit | SDM 33.3; KVM `prepare_vmcs12` | yes |
| C7 | L1's host state loaded into the VMCS that runs L1: CR0/CR3/CR4, RIP/RSP, segments, descriptor tables, EFER, PAT, SYSENTER | SDM 30.5; KVM `load_vmcs12_host_state` | yes - `load_l1_host_state`, with the segment shapes SDM 30.5.3 fixes |
| C8 | VM-exit MSR-store and MSR-load lists processed, with a VMX abort on failure | SDM 30.4, 30.6; KVM `nested_vmx_store_msr` | yes - and the abort is KVM's `nested_vmx_abort`: a triple fault given to the guest hypervisor, since a real VMX abort is a shutdown of the whole machine |
| C9 | Events queued for injection into L2 dropped on the way out | KVM `nested_vmx_vmexit`, the `kvm_clear_exception_queue` block | yes - vmcs02's entry-interruption field is rewritten from vmcs12 on every entry, and vmcs12's valid bit is cleared on the way out |
| C10 | An entry failure hardware detects surfaces to L1 as `VMfailValid`, not as an exit | SDM 33.3; KVM `nested_vmx_vmexit` failure path | yes - a refused entry unwinds through `on_nested_entry_failure` to a `VMfailValid` carrying the processor's own error number |

### D. The reflect-or-handle decision

| # | Requirement | Established by | Status |
|---|---|---|---|
| D1 | Two questions, in order: does *this VMM* want the exit, and only then does L1 want it | KVM `nested_vmx_reflect_vmexit`, `nested_vmx_l0_wants_exit`, `nested_vmx_l1_wants_exit` | yes - `on_l2_exit` asks `l0_wants_l2_exit` first |
| D2 | We always take: NMI, external interrupt, EPT violation, EPT misconfiguration, preemption timer | KVM `nested_vmx_l0_wants_exit` | partial - NMI, both extended page-table exits and the preemption timer; external interrupts are *not* ours, because this VMM never sets external-interrupt exiting and the control can only be there because L1 asked |
| D3 | Always reflected: triple fault, task switch, CPUID, INVD, XSETBV, the VMX instructions, invalid guest state | KVM `nested_vmx_l1_wants_exit` | yes - by `l1_wants_l2_exit`'s default |
| D4 | Conditional on L1's controls: HLT, INVLPG, RDPMC, RDTSC, MOV DR, MWAIT, MONITOR, PAUSE, RDRAND, RDSEED, WBINVD, descriptor-table access, INVPCID, XSAVES/XRSTORS | KVM `nested_vmx_l1_wants_exit` | yes |
| D5 | Exception exits filtered through vmcs12's exception bitmap, with the page-fault error-code mask and match applied | SDM 27.6.3; KVM `nested_vmx_is_page_fault_vmexit` | yes |
| D6 | CR-access exits filtered through vmcs12's CR0/CR4 guest-host masks and the CR3-target list | KVM `nested_vmx_exit_handled_cr` | partial - filtered through vmcs12's masks; the CR3-target list is not consulted because IA32_VMX_MISC reports a count of zero |
| D7 | I/O exits filtered through vmcs12's I/O bitmaps | KVM `nested_vmx_exit_handled_io` | yes - every byte of the access checked against vmcs12's bitmaps |
| D8 | MSR exits filtered through vmcs12's MSR bitmap | KVM `nested_vmx_exit_handled_msr` | yes |
| D9 | The exits this VMM already owns for its own reasons - the interrupt command register, watched pages, the sleep port, the preemption timer that drives the log - keep working while L2 runs | this tree: `on_interrupt_command`, `on_ept_violation`, `arm_controller_poll` | partial - the MSR and I/O intercepts and the preemption timer keep working through the merged bitmaps and `l0_wants_l2_exit`; a watched page works but costs a shadow rebuild per stepped write |

### E. Nested EPT

| # | Requirement | Established by | Status |
|---|---|---|---|
| E1 | A shadow EPT per EPTP12, composing L2-GPA→L1-GPA (L1's tables) with L1-GPA→HPA (ours) | KVM `nested_ept_init_mmu_context`, `nested_ept_get_eptp` | yes - `build_shadow_ept`, eager, keyed on bits 51:12 of EPTP12 |
| E2 | Two permission sets combined per page - read, write, supervisor execute and user execute all intersected | KVM `kvm_init_shadow_ept_mmu` and the shadow-page permissions it installs | yes - `ept_permissions`, with the misconfiguration normalisation SDM 31.3.3.1 requires |
| E3 | Populated eagerly instead of lazily, which removes fault-time composition and the widen-without-invalidate case | SDM 31.4.3.4, 31.4.3.2; lazy is KVM's shape and stays recorded as the optimisation | yes - deliberately not lazy |
| E4 | An EPT violation caused by a gap in *L1's* tables reflected to L1, with the qualification and guest-physical address it would have seen | KVM `nested_ept_inject_page_fault` | yes - `on_l2_ept_fault` |
| E5 | An EPT violation caused by a gap in *our* tables, or by a page we watch, handled here and never shown to L1 | KVM `nested_vmx_l0_wants_exit`, EPT-violation case | yes - the `host_denied` outcome, with the L1-physical address the walk produced handed to `on_ept_violation` |
| E6 | EPT misconfiguration always ours, never L1's, because L2 never walks L1's tables directly | KVM `nested_vmx_l0_wants_exit`, EPT-misconfig case and its comment | yes - and *deliberately unlike KVM*: a misconfiguration in L1's own tables is reflected, because the walk that found it walked L1's tables and not the shadow |
| E7 | L1's INVEPT invalidates the shadow for the named EPTP, and an INVEPT type we do not report is refused | SDM 33.3 INVEPT; KVM `handle_invept` | yes - `discard_shadow_ept` for either type |
| E8 | A change to *our* EPT - arming a page watch, protecting a region - invalidates every shadow built over it | this tree: `invalidate_ept`, `ept_generation` | yes - `shadow_ept_pointer_for` compares generations and rebuilds |
| E9 | The memory type of a shadow leaf derived from the MTRRs as our own tables are, not taken from L1 | SDM Table 31-6 reserved-bit rule as already applied in `initialize_ept`; this tree: `mtrr_state::type_of` | yes - `compose_ept` takes the host walk's type |
| E10 | Large-page shadow leaves where both levels permit, to bound the size of the shadow | SDM 31.3.2 | yes - plus coarsening a uniform 4 KB run to one 2 MB leaf, which is what keeps eager affordable |
| E11 | A bounded pool for shadow paging structures, with a defined behaviour on exhaustion | this tree: the `ept` pool precedent | yes - 96 pages per processor, and exhaustion refuses the VM entry rather than entering on a partial table |
| E12 | The module and every watched page remain unreachable from L2 | this tree: `protect_module`, `watch_guest_page_writes` | yes - composed through `host_ept_lookup`, so our cleared permissions carry into every shadow leaf |

### F. Capability reporting

| # | Requirement | Established by | Status |
|---|---|---|---|
| F1 | Control MSRs report a subset of the hardware's allowed-1 settings, with allowed-0 preserved | SDM A.3.1 through A.5 | yes |
| F2 | `IA32_VMX_BASIC` reports our own revision id, 4096-byte regions, write-back, and the TRUE MSRs | SDM A.1 | yes |
| F3 | `IA32_VMX_MISC` narrowed: no VMWRITE to exit-information fields, CR3-target count zero | SDM A.6 | yes |
| F4 | `IA32_VMX_VMCS_ENUM` reports the highest index the shadow honours | SDM A.9 | yes |
| F5 | `IA32_VMX_EPT_VPID_CAP` reports exactly the EPT and VPID features we honour | SDM A.10 | yes - `supported_ept_vpid_capabilities`, narrowed from the hardware's |
| F6 | `IA32_VMX_VMFUNC` reports what we honour | SDM Table 27-7 bit 13; SDM 33.3 VMFUNC | n/a - zero, and VMFUNC takes `#UD` |
| F7 | `IA32_FEATURE_CONTROL` virtualized with write-once semantics | SDM Table 7-1, bit 0 | yes |
| F8 | The CR0/CR4 fixed-bit MSRs passed through, since they describe the real processor | SDM A.7, A.8 | yes |

### G. What Hyper-V specifically needs

Hyper-V is the target, so these are called out separately. Each is a row
above, named here so that "can Hyper-V launch" has an answer rather than an
essay.

| Requirement | Row | Status |
|---|---|---|
| EPT, without which it does not start at all | E1-E12, F5 | written |
| Secondary controls, unrestricted guest, VPID | B5, F5 | written |
| A VMLAUNCH that actually runs L2 | B1-B17 | written |
| Exit reflection for the exits its own guest takes | C1-C10, D1-D9 | written |
| MSR bitmap merging, since it traps a great many MSRs | B11, D8 | written |
| Event injection into L2 and back out again | B14, C4 | written |

**Every row above is now written, and not one of them has executed.** That
is a different answer from "yes" and the distinction is the whole of what
is left: the checklist measures coverage against the SDM and KVM, which is
what reading can establish, and it cannot establish that any of it works.
The "Run:" sections above are what has executed: the instruction emulation,
the capability MSRs, and a second-level guest entered and exited with the
exit reflected - twice, the second time through a shadow extended page
table composed from the guest hypervisor's own. What stands between this
and an answer is a real guest hypervisor, which takes exits the probe never
reaches.

### The switch-on build overwrites the switch-off binary, and the check caught it

A `-DZPP_NESTED_VMX=ON` build in a second build directory still writes
`out/release/x86_64/zpp_hypervisor`, because the output directory follows
`CMAKE_BUILD_TYPE` and not the build directory. Building the switch-on
release and then `cmake --build --preset release` leaves the switch-*on*
binary in place: the plain preset's own outputs are up to date by its own
stamps, so it does not relink.

Which means every measurement of the switch-off binary taken after a
switch-on build is a measurement of the wrong file, and the recorded
baseline would have been a hash of the switch-on release.

`check-nested-absent.sh` caught it, on the string check rather than the
hash - eight strings only nested code emits, in a binary compiled with
`ZPP_NESTED_VMX=0`. That is worth recording as evidence the check has
teeth, because its own header says the string check is the one that proves
the *code* is gone rather than merely unreachable, and this is the first
time it has had something to find.

The procedure that avoids it: delete `out/<config>/x86_64/zpp_hypervisor`
before rebuilding the plain preset, so ninja has a missing output to
relink. Rejected: giving the nested build directories an output directory
of their own, which is a change to the build system for the sake of an
ad-hoc developer workflow, and which would also stop the smoke scripts -
they read `out/<config>` by name - from being pointed at a switch-on build
at all.

### The host stack was a quarter of what one shadow build needs

Found by reading the exit path top to bottom before trying to run any of
it, and worth recording because of what its symptom would have been.

Every VM exit runs on `host_vm_launch_stack`, a local of `vm_launch`, which
was 0x1500 bytes with two 928-byte contexts at the top - about 4.6 KB
usable. The eager shadow builder descends four levels of a guest
hypervisor's extended page tables and reads a whole 4096-byte table into a
local at each, so `build_shadow_ept` alone wants sixteen kilobytes. The
first shadow build would have run off the bottom of that array and into
whatever lay below it in `vm_launch`'s frame.

Three things make it the worst shape a bug can have here: it is silent, it
only happens with nested VMX switched on, and what it corrupts is the frame
of the function that owns the guest - so the failure would have appeared
somewhere unrelated, on the one configuration nobody had run.

The stack is now 0x8000. It costs nothing: it is a local of a function that
never returns, on the 512 KB per-processor stack `launch_on_cpu` already
reserved, so the whole of it comes out of memory that was allocated and
unused. Rejected: moving the builder's four tables into per-processor
members, which trades 16 KB of a stack nobody was using for 512 KB of .bss
and makes the builder non-reentrant by construction rather than by
accident.

The general lesson, and it is the one this tree keeps relearning: a
code path that has never executed has never had its *stack* checked either.

### The MSR areas, and the one restriction left in them

The three MSR areas are processed in software, and the processor is given
none of its own. Handing it the guest hypervisor's addresses would have
worked - a first-level guest-physical address is a host physical one here -
and is exactly what must not happen: the processor reads and *writes* those
lists in root operation, where extended page tables do not apply, so a
guest hypervisor could name this module's own pages as its VM-exit
MSR-store area and have the processor write MSR values into them. Every
other protection this VMM has is an extended page-table permission and none
of them would apply.

What is restricted: the *indices* an area may name are a list, in
`msr_area_index_handled`. SDM 29.4's last failure condition is "an attempt
to write bits 127:64 to the MSR indexed by bits 31:0 of the entry would
cause a general-protection exception if executed via WRMSR with CPL = 0",
and answering that in general needs a WRMSR that can fault and recover.
This VMM has none: `host_exception_recovery` is a single shared context
that `main` disarms before the guest ever runs, so a #GP in root operation
stops the processor - and a guest hypervisor's VM entry is a guest
instruction, which may never do that.

So an index outside the list fails the entry, which is an outcome the
architecture already has a name for and which the guest hypervisor is told
about. The direction is the safe one: too strict, never too permissive.

**Removing the restriction needs a per-processor host exception recovery
point.** That is a change to the exception path every processor shares,
which is why it was not made here. Two of the listed indices carry a value
check as well, because their *value* can fault where their index cannot:
IA32_EFER, whose LME bit cannot change while CR0.PG is 1 - the SDM's own
footnote to 29.4 - and IA32_PAT, whose bytes must each be one of the six
defined memory types.

Rejected: attempting the write and recovering from the fault, for the
reason above. Rejected: refusing every area outright, which is where this
started - Hyper-V uses them, so that is a refusal of the target.

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

### The memory budget, and where it goes

Settled rather than left open, because the shape of the pool decides the
shape of the code that draws from it. Sized off `max_cpus` (32) throughout,
never off the machine this was written on.

| What | Size | Why |
|---|---|---|
| Shadow root, one per processor | 32 x 4 KB = 128 KB | A shadow is per processor - see below |
| Shadow table pool, per processor | 32 x 96 x 4 KB = 12 MB | 96 tables is one page-directory-pointer table plus 95 page directories, which at 2 MB leaves covers 95 GB of second-level address space per processor |
| Second real VMCS, one per processor | 32 x 4 KB = 128 KB | Row B1 |
| **Total** | **~12.3 MB** | Against a class already at ~30 MB |

**Per processor rather than shared per EPTP12.** Both were considered. Shared
would halve the fault cost when a guest hypervisor runs the same second-level
guest on many processors, which is the normal case - but it needs the tables
locked during a fill, because two processors can fault into the same page
directory at once, and that lock sits on the hottest path nested EPT has.
Per processor needs no synchronisation at all, and the cost is memory that
the budget above shows is affordable. Revisit if the fault count measured
against a real guest hypervisor says the duplication hurts; the fault count
is the measurement that decides it, and it cannot be guessed.

**The pool has a hard cap and a defined behaviour on exhaustion: discard the
whole shadow and start again.** Not a failure, and not an unbounded pool. This
is the one place where "over-invalidation is always safe" pays for itself
directly - SDM 31.4.3.2 permits a processor to "invalidate any cached mappings
at any time", so throwing the shadow away costs faults and never correctness.
A shadow is derived state; that is the property that makes the bound safe, and
it is why 96 tables per processor is a tuning parameter rather than a
correctness one.

The alternative rejected: growing the pool on demand from the heap. The fill
path runs inside a VM exit, and this codebase's heap is a fixed arena whose
`operator new` traps on exhaustion - so a heap allocation there converts a
tunable into a dead processor.

### What still has to be decided by measurement

- The pool size. 96 tables per processor is a first number, not a measured
  one: it is a straight trade of memory against how often the
  flush-and-rebuild path runs, and it needs the fault count from a real guest
  hypervisor to settle.
- Whether the per-processor duplication of shadows is worth the fault cost it
  saves, per the paragraph above.

## The channel stops at the controller reset, and the clock was never the cause

Settled on the rig, by reading the resident side's own memory through the
emulator's monitor while Windows ran. Recorded because two earlier
readings of this were wrong in ways that were reasonable at the time.

What was measured, in order:

- Windows boots under the VMM, reaches user mode, and settles. Seven of
  eight processors park at one kernel address and one processor rotates
  through the VMM's own text - so exits are happening.
- `heartbeat_exits_seen` per processor, sampled twenty seconds apart:
  processor 0 goes from 218,502 to 243,632. That is **about 1,250 exits
  per second per processor**, unaided.
- `ring_storage::written` keeps climbing throughout, so records are still
  being produced.
- The sink's `sequence` and `next_block_index` are frozen at 18, which is
  exactly the nineteen blocks the disk holds for that boot.
- `configure_reject` reads `none`, so configuration succeeded; the gate
  reads zero, so nothing is holding it.
- `queue_pair<64>::bound` is **entirely zero**. `forget()` ran.

So the channel stopped because the guest reset the controller about seven
seconds in, `note_controller_write` saw CC.EN clear, and the sink did what
it is written to do. Everything upstream of the device was healthy.

Two corrections that cost time and should not be re-derived:

- **The log's clock is not what stops the channel.** A guest taking 1,250
  exits per second per processor needs no manufactured ones. The earlier
  reading - four heartbeats over four and a half seconds and then silence
  - came from a boot that hung early, and was generalised to a settled
  guest, which it does not describe. The fallback clock added for it is
  still correct for a genuinely idle guest and is cheap, so it stays; it
  is simply not the fix for this.
- **`lost_to_reset` reading zero is not evidence that nothing was lost.**
  It only ever reaches the medium inside the *next* block's header, and
  after a reset there is no next block. The same is true of every counter
  in that header. Read them from memory, not from the disk, when the
  question is what happened after the last block.

What that leaves is the reset itself, and the note under
`rebuild_channel_after_reset` had gone stale: it says the rebuild needs a
VMM that emulates the guest's write rather than stepping over it, and that
"this one does not have one yet". It does - `zpp/arch/x86_64/decoder.h`,
with its own check in CI. The remaining work is the four defects listed on
`emulate_watched_page_writes`, of which the guest page table walk at exit
time is the substantial one: there is no walker today, and
`os_page_table` is built once from the launch-time CR3, so it is only
right while the guest is still on the firmware's identity map.

### The nested build runs on the rig, and nothing exercised it

First hardware run of the nested code, with `ZPP_NESTED_VMX=ON` deployed
to the real machine and the passed-through NVMe.

**Windows boots and runs normally.** Stable kernel addresses across four
minutes, no boot loop, no regression against the switch-off build. That is
worth having on its own: the nested code is compiled into every path the
guest takes and does not disturb a guest that never uses it.

**Nothing nested happened.** Read from the running VMM's own memory:

    guest_in_vmx_operation[0..7]   all zero
    l2_entries                     0
    running_l2                     0
    vmcs02_launched                0
    nested_entry_failed            0

The first line is the one that settles it: the guest never executed
`VMXON`. So this is not our code refusing a guest hypervisor, and not a
launch that failed - nothing ever asked. Virtualization-based security is
off in that Windows installation, and with it off no Hyper-V starts, which
is exactly what the guest-facing notes predict.

So the reflection, save-back and shadow paths remain exercised only by
`verify_nested::present` under Bochs, where they do run end to end. What
is still unrun is a *real* guest hypervisor, and reaching it needs VBS
turned on inside the guest - Core Isolation, or `hypervisorlaunchtype` in
the boot configuration. That is a change inside Windows, not something
this side can arrange, and it is the one remaining step before any claim
that Hyper-V boots nested.
## The queue reservation, built to the corrected ordering and switched off

Written against the two causes NVME-LOG.md's last two sections established -
the identifier collision and the Set Features ordering - and switched off in
two pieces so the next boot can tell them apart.

**`diag::rebuild_channel_after_reset`, steps 1 to 3.** At the CC.EN 0 to 1
edge, borrow the guest's admin queue once and issue a single
`Set Features (Number of Queues)` asking for the maximum. Create nothing. The
guest's own Set Features arrives later with still no I/O queue in existence,
so it completes and reports the allocation reserved here - which is at least
what it asked for.

**Corrected by measurement: a completion patch is needed after all, and it
is now built.** The claim that the ordering made one obsolete assumed the
reservation could exceed what the guest wants. It cannot on this controller.
Measured with the reservation on: the grant is DW0 = `0x000f000f`, sixteen
submission and sixteen completion queues, which is everything it has - and
the guest asks for sixteen submission queues and creates all sixteen. So
there are eight spare completion identifiers and no spare submission
identifier at any allocation. See below.

**`diag::create_channel_queue_after_guest`, steps 4 and 5.** Create our own
pair once the guest has created its own, at an identifier above them.

Splitting them is the control experiment and is the reason to run two boots
rather than one:

- Reservation on, creation off: everything the borrow does happens, at the
  same instant it happened when the guest boot looped, and nothing is
  created. A guest that boots clears the borrow. A guest that does not
  indicts it, and the second switch never has to be tried.
- Both on: the only difference is a queue pair created behind a live driver.

### What was rejected, and why

- **Raising the identifier alone.** Measured already and recorded in
  NVME-LOG.md: 4 collides and boot loops, 32 exceeds the allocation and is
  refused with `0x4101`. Both are failure modes of the same defect.
- **Creating and reserving in one borrow at the CC.EN edge, then deleting
  our queues before handing back.** It reads as a way to keep one borrow: no
  I/O queue exists when the guest's Set Features arrives, so 5.2.30.1.5 is
  satisfied on the "currently exist" reading. It is rejected because the
  specification's words are "after creation of any I/O Submission and/or I/O
  Completion Queues", which a controller may reasonably implement as a latch
  set by the first create since the reset. Whether this controller does
  cannot be established from here, and getting it wrong aborts the guest's
  own negotiation - which Linux turns into zero I/O queues and no block
  device.
- **Choosing the identifier from the allocation.** The guest is told the
  whole allocation, which after asking for the maximum is far more than it
  wants, and no run has yet shown what Windows does with an allocation larger
  than its request. The identifier comes from what it is *seen to create*,
  with its own requested count as the margin - also an observation of the
  guest rather than of the controller.
- **A permanent doorbell watch.** With a stride of zero every I/O doorbell
  shares the admin page, so the watch is armed at the enable and removed as
  soon as there is nothing left to see - on a created queue, on a refusal,
  and immediately after the reservation in the control build. Left armed it
  is an exit per disk command, which would confound the very run it is there
  to support.

## Reducing the grant, so that there is an identifier to create at

`diag::reduce_guest_queue_grant`, off, and it needs
`rebuild_channel_after_reset` - a `static_assert` says so. Written because
of the measurement above: the controller has no spare submission queue at
any allocation, so the only remaining lever is what the guest *believes* it
was granted.

`hypervisor::patch_guest_queue_grant` runs from the guest's own admin
doorbell exit, finds the completion for the command identifier the guest put
on its Set Features, and rewrites four bytes of DW0 - NSQA fifteen instead
of sixteen. Nothing is submitted, no doorbell is rung, no lap runs, and the
controller's completion queue tail is not disturbed: the completion is the
guest's own, at its own slot with its own phase, and only DW0 is a lie.
`create_channel_queue` then finds submission queue sixteen free and inside
the allocation.

### What was rejected, and why

- **Substitution**, which is the shape NVME-LOG.md recommended: put a
  command of ours into the guest's submission slot while the doorbell is
  held, and rewrite the resulting completion's identifier as well as DW0.
  Rejected twice over. The premise is false - `page_watch::handler` is
  called *after* the store has been applied, so the doorbell has already
  rung and nothing here ever runs with it pending; making that untrue means
  deferring a store the watch currently applies, on the one path that must
  not be wrong. And it would put a completion carrying one of *our*
  identifiers into the guest's admin queue for a window, which is the exact
  exposure this design avoids everywhere else.
- **Reducing both halves unconditionally.** A half is reduced only where the
  guest's own request leaves nothing above it. On this machine that is the
  submission half alone, so the guest's completion queue arrangement is
  passed through byte for byte and one fewer thing changes.
- **Believing the entry on its command identifier alone.** The edited entry
  must also carry a DW0 equal to the allocation our own reservation was
  granted - which 5.2.30.1.5 guarantees, ours being the first Set Features
  after the reset. That is both the discriminator against a reused
  identifier and a test of the ordering argument; a disagreement is recorded
  as `0xe8` and nothing is edited.

### What is unsolved, stated rather than hidden

The admin queue's interrupt is not masked for the borrow. NVME-LOG.md
requires MSI-X vector 0 masked, and there is no MSI-X table access on the
resident side. What stands in for it is an argument about timing: the only
processor that can take that interrupt is the one held inside the exit with
interrupts disabled, so it stays pending in its local APIC and is delivered
after the restore, finding the guest's own completions and nothing else. If
the guest misbehaves *after* a successful create rather than during it, this
is the first thing to look at.

**The grant reduction does not add to that exposure, and the reason is worth
keeping.** Losing that race there means a guest interrupt service routine
reads the completion before it is edited - and what it finds is the guest's
own entry, with the guest's own identifier and the controller's own status,
saying sixteen rather than fifteen. The guest then creates sixteen
submission queues, the create finds no room and refuses, and the guest
boots. The failure mode is *no channel*, which is what the switch being off
already gives.

If the mask is ever built, the cheapest form is probably the **function
mask** in the device's MSI-X Message Control register, which lives in PCI
configuration space rather than in the table BAR - so it needs no second BAR
sized, mapped and validated, only the controller's bus, device and function
handed across by the loader and a configuration space access mechanism the
resident side does not have. The bit's position and the semantics of masking
a pending vector are recalled rather than looked up; check both before
writing any of it.

The wake probe is used again, in both borrows. That is only safe because
`1e8791f` made `on_host_exception` return for vector 2; before it, a probe
that reached a processor in root mode halted it for ever. Every passive wait
this codebase added to work around that is a wait that times out rather than
succeeds, which is why the probe is not optional.

### Where the release binary moved, and why

`scripts/ci/nested-off-baseline` is re-recorded. The whole of the movement is
**208 bytes in `hypervisor::hypervisor()`** and nothing else: comparing the
release object symbol by symbol, every other function is byte identical and
the new ones compile to one to six bytes each and are collected away, since
`ZPP_DIAG=0` discards their bodies and nothing calls them. The 208 bytes are
the constructor's stores for the new instrumentation fields, which are
`volatile` - so they are written even though the object lives in `.bss` and
is already zero. That is the same trade `configure_reject` documents: a field
whose only reader is a debugger measures nothing unless it is volatile.
`.bss`, `.data` and `.rodata` are unchanged in size.

It moved again for the grant reduction, and the second cause is the one that
would have wasted an afternoon. Nine instructions in the constructor, for
the same reason as before - and forty bytes in `.data.rel.ro`, every one of
them a **line number**. `zpp::hypervisor::log` takes a defaulted
`std::source_location`, so each call site carries a 24 byte
`{line, column, file, function}` record, and each below an insertion point
shifts by exactly the lines added - twenty three of them, comments included.
Comparing the two disassemblies mnemonic by mnemonic showed nine added
instructions and nothing else, which is the check to run rather than reading
the hash and guessing.

### Hyper-V probes VMX and declines, and the rig is why

Corrected from the earlier entry, which said virtualization based security
was off. It is on. The guest still never executes VMXON, and the reason is
measurable rather than inferable.

Instrumented and read from the running VMM:

    cpuid leaf 1 ecx reported   0x77fab22b   bit 5 set - VMX was offered
    capability MSR reads        28           it read the whole set
    guest_in_vmx_operation      0            and then did not enter

So it looked and declined. What it was told, beside what the hardware
underneath actually offers:

    msr             hardware              advertised
    PROCBASED2      0x001118fe00000000    0x001118ee00000000
    EPT_VPID_CAP    0x00000f0106334041    0x00000f0106134040

Two different findings in that table.

**We narrow three things the hardware does offer**: secondary control bit
4, virtualize x2APIC mode; and in the extended page table capabilities bit
0, execute-only translations, and bit 21, accessed and dirty flags. Each
is presumably narrowed because the shadow builder does not implement it,
and each is a candidate to restore - but only alongside the support, since
advertising a capability this VMM does not honour is worse than withholding
it.

**Neither column has secondary control bit 14, VMCS shadowing, nor bit 22,
mode-based execute control.** The hardware does not offer them, so nothing
here can. That is the rig rather than this VMM: the outer hypervisor is
QEMU with `hv-passthrough`, which filters the VMX capability MSRs through
enlightened VMCS version 1 - already recorded as the reason the preemption
timer is refused. Mode-based execute control is what hypervisor-protected
code integrity is built on, so a Hyper-V that wants it cannot get it here
whatever this side advertises.

What follows: restoring the three narrowed bits is worth trying and is
cheap, but it should not be expected to be sufficient. Testing a real
nested Hyper-V may need an outer configuration that passes the full
capability set through, or bare metal. Either way the question is now a
specific one about two named bits rather than "does it work".

### Hyper-V does launch a second-level guest here, and the capability set is the gate

The measurement that turns "why does it decline" into "which capabilities
must be implemented", and the first time a *real* guest hypervisor has run
anything under this VMM rather than the Bochs harness doing it.

Behind a diagnostic in `nested_vmx_capability_msr` that reports the
hardware's capability set unnarrowed - a lie while it is on, since every
bit normally withheld is withheld because nothing here honours it, and
therefore never to be left on.

    narrowed, as shipped        unnarrowed, diagnostic only
    ------------------------    ---------------------------
    VMXON never executed        vmcs02_launched   1
    l2_entries        0         l2_entries       17
    Windows boots               28 boots, looping

Seventeen second-level entries, `nested_entry_failed` zero, and then the
guest loops. So the decline is not Hyper-V rejecting this VMM on some
principle and not virtualization based security being off: it reads the
capability MSRs, finds the set too small, and stands down cleanly - which
is the behaviour the guest-facing notes always described. Widen the set
and it engages immediately.

The difference between the two columns is three bits, and they are the
work:

- secondary control bit 4, virtualize x2APIC mode. Withheld because the
  virtual-APIC page and its state are not maintained here.
- extended page table capability bit 0, execute-only translations.
  Withheld because `execute_only_translations_offered` reports false and
  `ept_permissions::normalised` must agree with it.
- extended page table capability bit 21, accessed and dirty flags.
  Withheld because the shadow never sets them, so a guest hypervisor
  reading them would find nothing ever accessed.

What is not the gate, and both were suspected: mode-based execute control
and VMCS shadowing. The processor has mode-based execute control - bit 22
is set in bare metal's IA32_VMX_PROCBASED_CTLS2, `0x005fbcff` - and the
outer hypervisor withholds it either way, with `hv-passthrough` (`0x001118fe`)
and without it (`0x001378ff`). Since Hyper-V engages regardless, it does
not need either. Removing `hv-passthrough` is therefore not indicated, and
it is wanted for other reasons.

The honest next step is to implement one of the three and offer only that
one, rather than widening the set and finding out afterwards which promise
was broken - the loop above is what a broken promise looks like.

### Correction: the widening experiment did not widen what it was read as

The entry above concludes that three bits are the work, and two of the
three are wrong. The diagnostic switch lived inside `narrow`, and
`narrow` is applied to five capability MSRs - pin-based, primary,
secondary, exit and entry controls. IA32_VMX_EPT_VPID_CAP is not one of
them: it is masked directly, `hardware & supported_ept_vpid_capabilities`,
on its own line and outside the lambda.

So during the run in which a guest hypervisor engaged and launched
seventeen second-level entries, **execute-only translations and accessed
and dirty flags were still withheld**. It engaged without them. They are
not the gate, and the measurement that named them is the measurement that
exonerates them.

What was actually widened is every control the five masks withhold, which
is far more than one bit:

- pin-based: whatever the processor offers beyond external-interrupt
  exiting, NMI exiting and virtual NMIs.
- primary: **bit 21, use TPR shadow**, and anything else unlisted.
- secondary: bit 4, virtualize x2APIC mode - measured, the only
  difference on this rig.
- exit and entry controls: every unlisted bit, which on a current
  processor is a dozen or so apiece.

**The prime suspect is the TPR shadow, and it is not independent of bit
4.** SDM 29.2.1.1: "If the 'use TPR shadow' VM-execution control is 0,
the following VM-execution controls must also be 0: 'virtualize x2APIC
mode', 'APIC-register virtualization', 'virtual-interrupt delivery', and
'IPI virtualization'." So the set as shipped is one no processor
presents - and offering bit 4 alone, as the entry above proposed, would
produce a set that is *architecturally impossible*: a guest hypervisor
may set virtualize x2APIC mode only together with a control it is
forbidden. The two have to be offered together or not at all.

Worth noting what the rig's own secondary set contains: bits 1, 2, 3, 4,
5, 6, 7, 11, 12, 16, 20. Virtualize APIC accesses, APIC-register
virtualization and virtual-interrupt delivery are all absent from the
hardware column too. So bit 4 is the *only* APIC capability available
here, and it is exactly the one that needs the TPR shadow beside it.

### The diagnostic is a mask now, so a boot answers a smaller question

`report_hardware_capabilities_unnarrowed` was a `bool` and is now a set
of group bits - `unnarrow_pin_based`, `unnarrow_primary`,
`unnarrow_secondary`, `unnarrow_exits`, `unnarrow_entries`,
`unnarrow_ept_vpid` - with the extended page table capabilities included,
which is the omission above. Still `unnarrow_nothing`, still a lie while
it is on, still never to ship.

The reason is the one that entry demonstrates: a run that widens five
MSRs at once answers "the capability set is the reason" and cannot answer
"which capability", and the second question is the one worth a reboot. On
a target where each variable costs a boot, the switch should cost one
variable. Suggested order, cheapest inference first:
`unnarrow_primary` alone, then `unnarrow_secondary` alone, then the two
together - the pairing SDM 29.2.1.1 forces.

### The extended-page-table pointer's accessed-and-dirty bit is checked

IA32_VMX_EPT_VPID_CAP bit 21 is withheld, and until now that was only
half of withholding it: `build_vmcs02` validated the guest hypervisor's
EPT pointer for memory type, page-walk length, reserved bits 11:7 and
address width, and said nothing about bit 6. A pointer with bit 6 set was
accepted and the shadow then never set an accessed or a dirty bit in any
entry it wrote, so a guest hypervisor polling its own tables would find
nothing ever touched - the quiet half-answer this codebase's guest-facing
notes warn about, arrived at by omission rather than by choice.

KVM pairs the two in `nested_vmx_check_eptp`, under the comment "AD, if
set, should be supported", and the check added here is that one: the bit
is refused unless `nested_vmx_capability_msr` reports the capability. It
is written against the reported value rather than against the constant,
so if bit 21 is ever implemented and offered the check follows without
being edited.

### What the guest hypervisor actually asks for, read from its own VMCS

The bisection was abandoned in favour of this, and it should have been done
first. Four boots widening one group of capability MSRs at a time - primary
alone, secondary alone, exit with entry, primary with secondary - all left
`l2_entries` at zero. Only widening all five together made it engage, and no
amount of that names the control it wants.

So capture what it *writes* instead. On the engaging configuration, the
first `build_vmcs02` records the guest hypervisor's own control fields:

    pin        0x0000001e
    primary    0xa4206dfa
    secondary  0x00000000
    exit       0x0003efff
    entry      0x000013ff

Read against our masks, two things follow and both retire earlier guesses.

**It sets primary bit 21, use TPR shadow.** `supported_primary_controls`
withholds it and `build_vmcs02` strips it from vmcs02 unconditionally. So
in the engaging run the guest hypervisor was told it could have TPR shadow,
set it, and had it silently removed - with no compensating CR8-load or
CR8-store exiting, since neither our own VMCS nor a first level using TPR
shadow sets those. Every `mov cr8` its second-level guest executed
therefore reached the *physical* control register, changing the real
processor's interrupt priority, and the virtual-APIC page it was told to
expect was never updated. Seventeen entries and then a loop is what that
produces. This is a promise broken in vmcs02 rather than a missing
capability, which is why widening the advertisement alone made things
worse.

**It sets no secondary controls at all** - not virtualize x2APIC mode, not
even EPT. That retires the whole of the previous entry's bit 4 argument,
including the SDM 29.2.1.1 pairing: the pairing is real, but nothing here
is trying to use the paired control. Withholding bit 4 is fine.

Pin asks for its required-1 bits plus NMI exiting and nothing else, so no
pin control is implicated either.

The work is therefore one item, not three: honour TPR shadow. Advertise
primary bit 21; in `build_vmcs02`, when vmcs12 sets it, validate the
virtual-APIC address vmcs12 names, write it and the TPR threshold into
vmcs02, and stop stripping the control. Where it is stripped anyway, force
CR8 load and store exiting so the physical register is never reached. The
exit side already exists - `l1_wants_l2_exit` answers
`tpr_below_threshold` against this control.

The exit and entry values above are recorded for the same comparison
against `supported_exit_controls` and `supported_entry_controls`, which has
now been done - see the next entry but one.

### The TPR shadow is honoured, and what to read to tell whether it worked

The one item the entry above named. Primary bit 21 is now in
`supported_primary_controls` and `build_vmcs02` reaches one of three
outcomes when vmcs12 sets it, rather than stripping it:

- **honoured** - the virtual-APIC address goes into vmcs02 and vmcs12's TPR
  threshold beside it, and the control stays set. KVM writes the same pair,
  in `nested_get_vmcs12_pages` for the address and `prepare_vmcs02_early`
  for the threshold.
- **replaced** - for an address this VMM will not let the processor touch,
  where vmcs12 also intercepts **both** CR8 accesses. With both intercepts
  the processor never consults the page at all, so the control is dropped
  and CR8 load and store exiting forced in its place, and those exits go to
  the guest hypervisor, which asked for them. KVM's own fallback, same
  condition, in `nested_get_vmcs12_pages`.
- **refused**, error 7, for anything else. KVM's comment on that path is
  worth keeping: failing the entry is "_not_ what the processor does but
  it's basically the only possibility we have".

**The address check is the part that had to be got right, because unlike
the MSR areas this address is handed to the processor.** SDM 29.2.1.1 gives
two of the four checks - bits 11:0 zero, and the physical-address width
checks of 28.2.1 - and KVM's `nested_vmx_check_tpr_shadow_controls` does
exactly those through `page_address_valid`. The other two are this VMM's:

- zero is refused, which a processor would not do. A shadow VMCS starts
  zeroed, so zero is precisely what a guest hypervisor that set the control
  and never wrote the field leaves behind, and page zero holds the guest's
  own real-mode interrupt vector table.
- the page must be one the guest itself may read and write under this VMM's
  extended page tables, asked through `host_ept_lookup`. **The processor
  reads and writes the virtual-APIC page in root operation, where extended
  page tables do not apply**, so the module's pages, the log queue storage
  and every watched page - each hidden by EPT permissions and by nothing
  else - would otherwise be writable by the processor on a guest
  hypervisor's behalf. Asking the tables rather than keeping a list is what
  stops the check drifting from what is actually protected.

Rejected, with the reason, so it is not re-proposed:

- **offering secondary bit 4, virtualize x2APIC mode, alongside this.** The
  earlier entry argued the pair from SDM 29.2.1.1's requirement that the TPR
  shadow be set for it. The capture retires that: the guest hypervisor sets
  no secondary control at all. The pairing is real but runs the other way
  now - the TPR shadow is the prerequisite those controls could later be
  built on, and each still needs its own state first.
- **forcing CR8 load and store exiting wherever the control is not set.**
  KVM does this unconditionally in `prepare_vmcs02_early`'s
  `#ifdef CONFIG_X86_64` else branch, and it is wrong here. A guest
  hypervisor that never set the TPR shadow does not believe its guest's CR8
  is virtualized, so the second-level guest owning the physical register is
  what bare hardware would do; forcing the intercepts would manufacture
  exits `l1_wants_l2_exit` declines and the ordinary control-register
  handler stops the processor on - it answers MOV to CR4 and nothing else.
  The intercepts are forced only in the **replaced** case, where vmcs12
  already carries both and the exits therefore reflect.
- **checking that the threshold's bits 3:0 do not exceed VTPR's bits 7:4.**
  SDM 29.2.1.1 says *should*, not *must*, it would cost a guest page read
  on every entry, and KVM does not check it either. Its companion rule -
  bits 31:4 of the threshold must be 0 whenever virtual-interrupt delivery
  is 0, which here is always - **is** checked.

The exit side needed no change. `l1_wants_l2_exit` answers
`tpr_below_threshold` against primary bit 21, and that is right: vmcs02
carries the control only where vmcs12 set it and this VMM never sets it for
itself, so the exit is always the guest hypervisor's and
`l0_wants_l2_exit` correctly does not name it. Two static_asserts were added
for the other half of the promise - that a shadow VMCS has storage for the
virtual-APIC address and the TPR threshold at all, since advertising a
control whose fields answer VMWRITE with error 12 would be the same
half-answered interface withholding it was.

**What to read on the rig.** The done-condition is a guest hypervisor
engaging with the shipped narrowed set plus bit 21 - no `unnarrow_*`
diagnostic - launching a second level, and Windows continuing to boot. In
order, from the running VMM's own memory:

- `vmcs12_controls_captured` must be 1, and `vmcs12_primary_controls`
  `0xa4206dfa` or near it with **bit 21 set**. If it is 0, nothing engaged
  and the rest says nothing - that is the failure to chase first, and it
  means bit 21 was not the whole gate.
- `vmcs02_launched` 1 and `l2_entries` climbing past seventeen - both
  per-processor arrays, so read the entry for whichever processor engaged.
  Seventeen was where the previous run stopped being useful; a number that
  keeps rising is the shape wanted, not a particular value.
- `nested_entry_failed`, also per processor, **false**. True here with the
  entry now carrying a virtual-APIC address means the address or the
  threshold was refused by the *processor* rather than by the checks above,
  and the log line `second level entry refused by the processor` says with
  what.
- the log for `guest {} refused: error 21` - `nested_controls_unsupported`,
  which is what the three virtual-APIC address checks and the threshold
  check answer with. Its presence with `l2_entries` at zero is this change
  refusing something the previous build silently accepted; error 22 there is
  the host-state check instead and unrelated.
- Windows itself. A boot that completes is the measurement; a loop after
  seventeen entries is the *old* symptom and would mean the TPR shadow was
  not the whole of it.

### Neither the exit nor the entry controls withhold anything the guest set

The comparison the capture was recorded for, done against all four masks
rather than the two, because the same arithmetic answers all of them. The
default1 classes are from SDM A.3.1, A.3.2, A.4.1 and A.5, quoted rather
than remembered - a bit in one of those classes is reported as settable
whatever the mask says, because `narrow` puts the allowed-0 half back:
`allowed_1 = (allowed_1 & supported) | allowed_0`.

| group | measured | default1 bits it set | beyond default1 | withheld **and** set |
|---|---|---|---|---|
| pin | `0x0000001e` | 1, 2, 4 | 3 - NMI exiting | none |
| primary | `0xa4206dfa` | 1, 4, 5, 6, 8, 13, 14, 26 | 3, 7, 10, 11, **21**, 29, 31 | none, now that 21 is offered |
| exit | `0x0003efff` | 0-8, 10, 11, 13, 14, 16, 17 | 9 - host address-space size; 15 - acknowledge interrupt on exit | none |
| entry | `0x000013ff` | 0-8, 12 | 9 - IA-32e mode guest | none |

So **primary bit 21 was the only withheld control the guest hypervisor
set**, in any group. There is nothing else to implement from this capture,
which is worth stating plainly because the two previous entries each named
capabilities that turned out to be innocent.

Two things fall out of the table that are not gaps today and would be:

- **exit bit 15, acknowledge interrupt on exit, is offered and not
  honoured.** It only means anything on an external-interrupt exit, and the
  guest hypervisor does not set pin bit 0 - so it can never take one. If
  one ever did, `reflect_l2_exit` would hand it vmcs02's
  interruption-information field, and vmcs02's exit controls are *this*
  VMM's, which do not acknowledge - so the field's valid bit would be clear
  and the interrupt still pending in the local APIC. Honouring it means
  acknowledging the interrupt during reflection and synthesising the field,
  which is why it is written down rather than done.
- **exit bit 12, save VMX-preemption timer value, is clear**, which agrees
  with the pin-based mask not offering the timer. That is the one place the
  four masks are visibly consistent with each other, and it is worth
  noticing because it is the shape the rest should keep: an exit control
  that saves state for a control that is not offered would be a promise
  about a field nothing writes.

### Advertising the TPR shadow moves the failure later rather than fixing it

Measured, and it is the reason the nested switch must not be treated as
close to working.

`465ae43` advertises primary bit 21 and honours it in `build_vmcs02`. With
`ZPP_NESTED_VMX=ON` and the shipped narrowed set otherwise, the guest boots
to the kernel on **processor zero only**, which then spins inside a twenty
byte range, while the other seven sit parked at one low address and never
enter the kernel. It reproduces identically across boots, at different
kernel base addresses, so it is deterministic and not a flaky boot.

Reverting just the two source files that commit touched, and rebuilding the
same nested configuration, boots all eight processors into the kernel with
one in user mode. So the commit causes it.

**But not through the code it added.** `vmcs12_controls_captured` reads 0 in
the failing run, so `build_vmcs02` never ran: no address was validated, no
CR8 exiting was forced, no vmcs02 was built. `supported_primary_controls`
feeds exactly one place, the capability MSR. The only thing that reached
the guest was a different value in `IA32_VMX_PROCBASED_CTLS`.

So the guest hypervisor reads bit 21, gets *further* into its own start-up
than it did before, and wedges there instead of standing down cleanly. Put
beside the other two measurements it is a progression, not a contradiction:

    shipped set          declines, never enters VMX operation, Windows boots
    plus bit 21          gets further, and the guest wedges before its
                         application processors start
    whole set widened    engages, seventeen L2 entries, then a boot loop

Which says bit 21 is **necessary and not sufficient**, and that the earlier
reading of "primary alone did not engage, so the TPR shadow is not the
gate" was too strong: not engaging and not getting further are different
outcomes, and only the first was checked.

The commit is kept rather than reverted, because with the switch off it is
inert - the switch-off release binary hashes identically to the recorded
baseline, verified independently. What is not safe is the switch itself: it
now has a known deterministic regression under it, and that has to be
understood before the switch is used for anything else. The next thing to
measure is `nested_capability_reads` in the failing run against the
shipped run - a higher count is direct evidence of "got further" rather
than an inference from where the processors ended up.

### The wedge, under a debugger: the guest is waiting for processors firmware still owns

Debugged with gdb against the QEMU stub, reading only - no single stepping,
which on this target ORs TF into the guest's RFLAGS and takes an entry
failure on the next resume.

**The seven idle processors never left firmware.** At their RIP:

    0x7f96b019: mov %rsp,%rax ; sub $8,%eax ; xor ecx,ecx ; xor edx,edx
    0x7f96b024: monitor %rax,%ecx,%edx
    0x7f96b027: mov %rbx,%rax ; shl $4,%eax
    0x7f96b02d: mwait %eax,%ecx
    0x7f96b030: jmp 0x7f96b019          <- all seven are here
    0x7f96b032: cli ; hlt ; jmp 0x7f96b032

That is the firmware's own application-processor wait loop, monitoring its
wakeup semaphore. Not Windows, and not this VMM.

**Processor zero is in the Windows kernel waiting for them**, spinning over
three globals with every branch returning to the top of the loop.

And from the VMM's own memory:

    next_virtual_processor   2            exactly one processor was handed over
    watched_apic_page        0xfee00000   the guest is in xAPIC mode
    resumes_reached[0]       6771         processor zero is alive and exiting
    resumes_reached[1]       0            processor one has never taken an exit
    nested_capability_reads  14           against 28 whenever it does not wedge
    vmcs12_controls_captured 0            build_vmcs02 never ran

So the hand-over started one processor and then stopped, and the start-up
IPIs are travelling through the APIC *page* rather than the x2APIC MSR,
because the guest is in xAPIC mode.

**A correction to the previous entry, and to how the bisection was read.**
The run that widened all primary controls reported RIP `fffff80479942262`,
which is the same `...942262` offset as this spin loop. That run wedged in
exactly this way, and was recorded as booting normally because only the
loader-run count and a single RIP were checked. Both runs that advertised
primary bit 21 wedged, and `nested_capability_reads` reads 14 in both
against 28 in every run that did not. The correlation with bit 21 is two
for two.

Which reframes the question. It is not how honouring the TPR shadow breaks
vmcs02 - `build_vmcs02` never runs. It is **how advertising primary bit 21
in IA32_VMX_PROCBASED_CTLS stalls the guest's own application-processor
start-up.** The guest evidently does something different when it believes
the TPR shadow exists, and whatever that is meets our start-up IPI
interception. Item 6 above records that xAPIC MMIO interrupt-command
interception was fixed and never run; it is directly in this path and
should be read with suspicion.

The lesson about method, which cost a boot and a wrong entry: **a kernel
RIP is not evidence of a healthy boot.** Two of the runs called "booted
normally" were spinning at the same instruction. Read all eight
processors, or read `resumes_reached`, before calling a boot good.

### Why Hyper-V does not start here, from the guest's own report

Settled by looking at what Windows says about itself rather than by
inferring from our counters, and it moves the whole question.

`msinfo32` under this VMM, with nesting compiled in:

    Virtualization-based security         Not enabled
    Secure Boot State                    Unsupported
    Kernel DMA Protection                On
    Hyper-V - VM Monitor Mode Extensions            Yes
    Hyper-V - Second Level Address Translation      Yes
    Hyper-V - Virtualization Enabled in Firmware    Yes
    Hyper-V - Data Execution Protection             Yes

All four hardware prerequisites report **Yes**, so the VMX this VMM
advertises is being seen and accepted as sufficient. And
virtualization-based security is **not enabled** - so no Hyper-V starts,
and there is nothing to nest. Confirmed on our side the same run:
`guest_vmxon_count` zero on every processor, `l2_entries` zero,
`vmcs12_controls_captured` zero.

**Secure Boot reads Unsupported**, and that is a standard prerequisite for
virtualization-based security. Chainloading the boot manager from our own
loader is what removes it: nothing in the chain is signed, so the firmware
cannot report a secure boot state to the operating system.

Which reframes every capability-set measurement taken so far. The
bisection was measuring a **second-order** effect: widening the advertised
set once pushed Windows over the line into enabling the feature - that is
the run with seventeen L2 entries - but in the shipped configuration the
feature never arms, so nothing reads the capability MSRs in earnest. Two
consequences:

- "Hyper-V declines our capability set" was never established. It does not
  get as far as deciding.
- The run that engaged is still the only evidence about what a real guest
  hypervisor needs, and it remains valid.

What would make it startable, in order of cost:

1. **Let the feature arm without demanding platform security.** The policy
   that gates it on Secure Boot is a guest-side setting; with it relaxed
   the feature can enable on a machine whose firmware cannot attest. This
   is a change inside the guest, not here.
2. Make the chain attestable, which means signing the loader and giving
   the firmware a secure boot configuration. Much larger, and it would
   have to be maintained.

Until one of those, a nested Hyper-V cannot be reached on this rig
whatever this VMM advertises, and no amount of capability work will change
it. That is worth stating plainly because a great deal of effort went into
the capability set on the assumption that it was the gate.

### What a smaller bare-metal reference advertises, beside us

Compared bit by bit against an implementation of comparable size that
carries a Windows guest with a nested hypervisor. Differences that matter,
with ours first:

- **The TPR shadow.** It does not advertise primary bit 21 at all, and
  offers CR8-load and CR8-store exiting instead. That is what the
  withdrawal above rests on, and it is the difference that fixed the
  application-processor stall.
- **IA32_VMX_BASIC bit 54.** It reports instruction information for INS and
  OUTS; we clear it because nothing here supplies it. Ours is the honest
  answer and should stay, but the difference is worth knowing if a guest
  hypervisor ever keys on it.
- **Entry control, load IA32_PERF_GLOBAL_CTRL.** It advertises this; we do
  not. The measured capture shows the guest hypervisor does not set it, so
  it is not required.
- **Extended page table capabilities.** It hides 2 MB and 1 GB leaves as
  well as accessed and dirty flags. We advertise the large-page bits
  because the shadow builder reads them, which is defensible - but it is
  the more generous answer of the two.
- **IA32_FEATURE_CONTROL.** It *constructs* the value as lock plus
  VMX-outside-SMX; we pass the hardware register through. Passing through
  is wider: it carries whatever else the platform set, including the inside
  SMX bit and any SGX bits.
- **CPUID.** It masks safer mode extensions and hides MONITOR and MWAIT.
  We deliberately leave MONITOR and MWAIT as hardware reports them, for
  the reason recorded where those controls are declared - concealing them
  produced a bugcheck. Worth not "fixing" by imitation.
- **TSC scaling.** It advertises the secondary control; we do not.

### What we do wrong: we hide the interface the guest needs to nest

Hyper-V boots on this rig **without** us and does not boot **with** us, and
the difference is not the VMX capability set. It is what the guest is told
it is running on.

Without us, the outer hypervisor exposes a full set of Hyper-V
enlightenments - the test rig's own configuration does this deliberately -
so Windows sees a hypervisor it recognises, one that advertises
nested-virtualization support, and it arms virtualization-based security on
top of it.

With us, `on_cpuid` answers the **whole** range `0x40000000`-`0x4fffffff`
with `ZppZppZppZpp` and a diagnostic leaf, and nothing else. No interface
version, no hypercall MSR, no VP index, no reference counter, and nothing
about nested virtualization. So Windows sees an unrecognised hypervisor
offering no enlightenments, and declines to arm the feature - which is
exactly the measurement: `guest_vmxon_count` zero on every processor,
virtualization-based security reported "not enabled", and all four
*hardware* prerequisites reported present because those come from leaf 1
and the extended page tables, which we do pass through.

The comparison that makes it plain: a smaller bare-metal reference of the
same size class, which carries a Windows guest with a nested hypervisor,
does the opposite on purpose. It reports its own vendor signature at the
first leaf and then **supplies the Hyper-V interface leaves anyway** - the
interface version at the second leaf, and a feature leaf advertising the
hypercall MSR, the VP index MSR, the reference counter and the frequency
MSRs. It presents itself as itself *plus* an interface the guest knows how
to use.

**Answering the whole range was the right fix for the problem it solved and
is the wrong answer for this one.** It exists because a guest that reads our
vendor from one leaf and a Hyper-V interface from the next acts on the more
specific claim and starts using synthetic MSRs - and that cost an afternoon
and a `0xc000000d`. Both facts are true at once: hiding the interface keeps
the guest from asking for things we cannot do, and it is also what stops it
nesting.

Which names the work, and it has two halves that must land together:

1. **Present the interface**: the version leaf, and a feature leaf naming
   only what is actually backed. For nesting specifically that includes
   whatever leaf reports nested-virtualization support, since that is what
   a guest hypervisor looks for before enabling itself.
2. **Back it.** The synthetic MSRs live at `0x40000000` and above, which is
   **outside both ranges the MSR bitmap covers**, so every access exits
   unconditionally and the handler answers `#GP` - by design, and correctly,
   for a VMM that claims no interface. Claiming one means those accesses
   have to be answered, and the cheapest honest way here is to forward them
   to the hypervisor underneath, which does implement them. A build that
   does half of this is the `0xc000000d` failure again, on purpose.

The second half is why this is not a one-line change, and why it should be
built behind a switch and measured rather than assumed.

### Passing the interface through did not make the feature arm

The hypothesis in the previous entry was tested and is **not confirmed**.

With the switch on - the whole hypervisor CPUID range left to whatever this
VMM runs under, and synthetic MSR accesses forwarded down instead of
faulted - measured on the rig with nesting compiled in:

    synthetic_msr_accesses   0
    guest_vmxon_count        0 on every processor
    l2_entries               0
    next_virtual_processor   9    all eight processors adopted, guest healthy

The guest booted normally and **took nothing**. Not one synthetic MSR was
touched, so it is not that the interface was offered and refused - it never
looked. Which means hiding the interface is not what stops the feature
arming, and the reasoning that said it was has a hole in it.

What is now known, and worth keeping separate from what is not:

- The CPUID handler does execute the real instruction first and edit the
  answer, so with the switch on the range genuinely carries the underlying
  hypervisor's leaves. The mechanism works; the guest did not respond to it.
- Every hardware prerequisite the guest reports is present, on both
  settings.
- No processor enters VMX operation on either setting, now measured with
  counters rather than inferred from a state flag.

So the difference between arming without this VMM and not arming with it is
still unexplained, and the next thing to do is stop guessing at it from
this side. The guest decides this, and the guest can be asked: its own
event log records why the feature did not start, and that is a far more
direct instrument than any counter here. Reading it needs the guest's
filesystem, which is reachable read-only from the host side.

The switch is left off. It costs nothing off, it is honest about needing
something underneath, and it is the right shape if the interface ever does
turn out to matter.

### Announcing a hypervisor works, and the guest then starts its processors somewhere else

The measurement that names the actual chain, and it corrects the previous
two entries.

**The guest never looked for a hypervisor.** Recorded over a boot: 49,860
CPUID leaves asked, and **zero** of them in the range `0x40000000`
-`0x4fffffff`. Which is why the earlier attempt at presenting the interface
proved nothing - the range was passed through faithfully and the guest never
asked. `cpuid_leaf_1_ecx_reported` says why: bit 31 clear. `on_cpuid`
deliberately clears it so this VMM is indistinguishable from bare metal.

**Announcing one changes everything downstream.** With bit 31 set and the
range passed through:

    hypervisor-range leaves asked      0  ->  225
    synthetic MSR accesses             0  ->    9
    next_virtual_processor             9  ->    2

So the guest looks, finds an interface, and **uses** it. And its
application-processor adoption collapses from all eight to none - the same
shape as the TPR shadow stall, and now with an explanation rather than a
suspicion: a guest that believes it is running under a Hyper-V-compatible
hypervisor starts its processors through an **enlightened hypercall**, not
through INIT-SIPI-SIPI. Our start-up path is the interrupt command
register. Nothing rings it, so nothing is adopted.

That is the whole difference between forwarding an interface and
implementing one. Forwarding sends the hypercall to whatever is underneath,
which starts nothing on our behalf and tells us nothing. The reference this
is compared against does not forward: it has a hypervisor-interface
implementation of its own - hypercall page, guest OS identity, VP index,
reference counter - and answers those calls itself.

Which gives the honest shape of the remaining work, and it is larger than
any switch:

1. Announce a hypervisor (one bit, done, behind the switch).
2. Present the interface *and answer it*: at minimum the hypercall page and
   the VP index, and specifically whatever call starts a virtual processor,
   because that is the one our own processor hand-over depends on.
3. Only then does the question "does a guest hypervisor start" become
   answerable, because only then is the guest's own start-up working under
   an announced hypervisor.

Also worth stating plainly: the bare-metal reading of the feature's
requirements is the likely reason it never armed. A guest that thinks it is
on bare metal demands Secure Boot for virtualization-based security, and
this rig reports Secure Boot unsupported. A guest that knows it is
virtualized takes a different path. That is consistent with the feature
arming on this rig without this VMM present, and it is now testable rather
than assumed - but only once step 2 exists, since step 1 alone breaks the
processors.

The switch is left off. It is honest about needing something underneath, and
it now also breaks processor start-up, which is recorded in its comment.

### The blocker is the instruction decoder, not the interface

Announced a hypervisor claiming **no features at all** - vendor leaf ours,
interface signature in the second leaf, every other leaf in the range zero.
The guest looked (105 hypervisor-range leaves) and used nothing (zero
synthetic MSR accesses), exactly as intended. And its
application-processor adoption still collapsed: `next_virtual_processor` 2,
`resumes_reached[1]` zero.

So the collapse is **not** the enlightened hypercall start-up. Nothing was
claimed and nothing was used. What it is:

    APIC-page writes emulated    179
    APIC-page writes undecoded   148

**Forty-five per cent of the guest's writes to the local APIC page are
instructions `decode_memory_store` does not handle**, and the interrupt
command handler correctly refuses to act on one it could not decode -
guessing which register was written would mean sending an interrupt nobody
asked for. So when the guest takes a different path through its own APIC
code, the start-up IPI lands among the undecoded writes and is never seen.

That is one mechanism for three symptoms, and it retires two explanations:

- Advertising the TPR shadow "stalling application-processor start-up" is
  this, not anything about the control.
- Announcing a hypervisor "making the guest start processors through a
  hypercall" is also this. No hypercall was available to it.

It also explains why the shipped configuration works: on that path the
guest happens to use forms the decoder covers. The coverage was never
sufficient - it was sufficient *for one code path*, which is not a property
worth relying on, and `apic_writes_undecoded` existing at all was the clue.

The work is bounded and unglamorous: find which forms those 148 writes use
and extend the decoder to cover them. `decode_memory_store` handles the MOV
forms that store a register or an immediate and refuses everything else -
deliberately, and that refusal is what is now costing the processors. The
instrument for naming them is a ring of the undecoded instructions' first
bytes, which is a small change to the same handler that counts them.

Until then, announcing a hypervisor and advertising the TPR shadow both
stay off, and both switches record that the reason is the decoder rather
than anything about what they announce.

## Emulate INIT ourselves instead of forwarding it to hardware

Application processors do not reliably start under a guest hypervisor,
and the reason is not in this tree's logic - it is that the sequence is
handed to the layer below at exactly the moment that layer is entitled
to throw half of it away.

`filter_local_apic_write` decodes a start-up IPI and applies it to each
target through this VMM's own roster, but an INIT is passed through as
the guest wrote it, on the reasoning that INIT is what leaves a target in
wait-for-SIPI and there is nothing to improve on. That is true on bare
metal. It is false when this VMM is itself a guest:

```c
/* KVM, vmx.c */
bool vmx_apic_init_signal_blocked(struct kvm_vcpu *vcpu)
{
        return to_vmx(vcpu)->nested.vmxon && !is_guest_mode(vcpu);
}

/* KVM, lapic.c, kvm_apic_accept_events */
if (!kvm_apic_init_sipi_allowed(vcpu)) {
        clear_bit(KVM_APIC_SIPI, &apic->pending_events);   /* discarded */
        return 0;
}
```

`nested.vmxon && !is_guest_mode` is precisely "zpp has executed VMXON and
is in VMX root mode right now" - any instant a processor is inside this
VMM's own code. In that state KVM does not defer the start-up IPI, it
**drops** it, keeping only the INIT pending. Windows allows about 210
microseconds from the INIT to the first start-up IPI and 200 more to the
second, while this VMM enters root mode thousands of times a second
filling shadow EPT leaves - so the sequence is lost often, and which
processors survive it varies run to run. Measured: one run left all seven
application processors in wait-for-SIPI, the next left six running in the
firmware's park loop and one in wait-for-SIPI, from identical builds.

Under KVM alone the same Windows and the same Hyper-V start their
processors every time, because `nested.vmxon` is false for those vCPUs
and the sequence is never blocked. **The difference is ours.**

So the delivery must not depend on the layer below. INIT should be
emulated for the targets the same way the start-up IPI already is: the
command is decoded here, each target named by the roster is marked, and
the target applies wait-for-SIPI to its own VMCS - a VMCS can only be
written by the processor it is current on, so this needs a way to make a
target exit promptly and a per-slot pending-INIT flag, not a remote VMCS
write.

Two things to keep when doing it:

- `emulate_init_signal` must stay as short as it is, and for the reason
  written there rather than for tidiness: every instruction between the
  INIT and the resume is a chance to lose the IPI.
- The start-up IPI side is already right and was made right by this
  session's fix: KVM's `kvm_apic_accept_events` tests only whether the
  target is in wait-for-SIPI, so that is what this tests too. Do not
  reintroduce a flag that means "already started" - two of them went
  stale here and cost a boot each.

## Findings from a static review against the SDM and KVM

Raised by a read-only review of the nested path while chasing a Windows
boot under Hyper-V that reached the spinner and quiesced. Ranked by how
close they are to live. The first two of the list were fixed immediately
(`8800804`, and the disk sink in `14e53da`); these are what is left.

**NMIs are never reflected to L1.** `l0_wants_l2_exit`
(`nested_entry.cpp:1426-1438`) claims every `exception_or_nmi` of NMI
type for this VMM, so the ordinary handler injects it into *L2* through
vmcs02's entry-interruption field. KVM takes the exit in L0 and then
reflects when L1 asked for it - `nested.c:4309-4324`, gated on
`nested_exit_on_nmi` (`nested.h:234-237`). The recorded Hyper-V vmcs12
capture is `pin 0x0000001e`, and bit 3 of that is NMI exiting: Hyper-V
asks and never receives one. Fix: claim the NMI only when it is this
VMM's own wake, and let `l1_wants_l2_exit` reflect the rest.
`reflect_l2_exit` already copies the interruption information verbatim.

**"Acknowledge interrupt on exit" is advertised and not implemented.**
Offered in `supported_exit_controls` (`nested_vmx.h:485`), dropped at
`nested_entry.cpp:1229` where vmcs02 takes `exit01` unchanged, and never
emulated - while external-interrupt exits *are* reflected. SDM 30:
without the control "the interrupt controller is not acknowledged and the
interrupt remains pending", and the interruption-information field "is
marked invalid ... and the remainder of the field is undefined". KVM
emulates it in software (`nested.c:4335-4345`) and says outright that the
hardware field cannot be forwarded (`nested.c:6569-6574`). Latent only
because the measured capture has external-interrupt exiting clear. The
honest one-line move is to stop advertising bit 15 until the vector is
synthesised from the LAPIC on reflection.

**An event interrupted mid-delivery is dropped unless the exit was
reflected.** `reflect_l2_exit` propagates IDT-vectoring
(`nested_entry.cpp:2049-2052`); the `deferred` and `handled` paths never
read `idt_vectoring_information_field`, and nothing else in the tree
does. KVM re-queues on every exit - `__vmx_complete_interrupts`,
`vmx.c:7105-7157`. This is live for **L1** as well as L2, and watched
pages are device registers, so it coincides exactly with interrupt-heavy
code. Fix: before resuming an exit that was not reflected, copy a valid
IDT-vectoring field into the entry-interruption field, with the error
code and, for software types, the instruction length.

**Every INVEPT names EPT01's root.** `hypervisor.cpp:860-885` hardcodes
`epml4_physical`, and `discard_shadow_ept{,_for}` issue none at all. SDM
31.4.3.1 scopes single-context INVEPT to "the EPTRTA specified in the
INVEPT descriptor", and 31.4.3.2 says neither INVVPID nor a VM transition
is required to invalidate guest-physical mappings. So L1's own
write-protection of its guest's pages can fail to take effect, and a
re-used shadow slot can serve stale mappings. Dormant while the measured
Hyper-V capture has `secondary 0x00000000` - no EPT of its own - so L2
runs on EPT01. Fix: an all-context INVEPT (type 2, already advertised)
after each shadow discard and slot release.

**The exit-driven log channel dies on any processor that entered L2.**
Two halves. `nested_entry.cpp:1089-1092` masks the preemption timer out
of the *union* of both sides' pin controls, so vmcs01's own timer is
removed too - KVM masks only vmcs12's contribution (`nested.c:2352-2354`)
- which also makes `l0_wants_l2_exit`'s preemption-timer case dead code.
And `arm_controller_poll` (`hypervisor.cpp:1276-1294`) reads and writes
whichever VMCS is current, so from the exit tail it arms *vmcs02* and
latches `armed_here`, never arming vmcs01 again.

**`record_exit` reads vmcs01 for a reflected L2 exit.** `reflect_l2_exit`
has already executed `vmptrld` on this VMM's own region
(`nested_entry.cpp:2082-2088`) by the time `record_exit` runs, so a
reflected exit is recorded with the L2 exit *reason* beside vmcs01's
stale qualification, RIP, activity state and CS. It corrupts the
instrument being used to chase all of the above. Fix: sample the fields
in `on_l2_exit` before the VMCS switch and pass them in.

Smaller, recorded so they are not re-derived: `translate_guest_linear`
has no EPT12 level and walks L2-physical addresses as host-physical
(benign only while `secondary12 == 0`); a stepped watched page is
reopened in EPT01 only, so the shadow leaf stays writable for L2;
emulating APIC MMIO on a processor already in x2APIC mode writes a dead
window (SDM 13.12.2); `started_by_guest_start_up_ipi` is written in two
places and read nowhere; vmcs01's exception bitmap is read but never
written.

Checked and found correct, so they need not be looked at again:
interrupts are not lost while this VMM is in root mode (vmcs01 sets NMI
exiting only, and a VM exit clears IF, so an arriving interrupt stays
pending in the LAPIC); the APIC-virtualization secondary controls are
rejected by `within_capability` rather than silently ignored; the TPR
shadow is offered and honoured including KVM's CR8-exiting substitution;
page-fault filtering in `l1_wants_l2_exit` is truth-table identical to
`nested_vmx_is_page_fault_vmexit`; `reflect_l2_exit`'s field set is
complete against `prepare_vmcs12`; and `filter_local_apic_write`'s ICR
composition matches `kvm_apic_send_ipi`.

## What the quiesce actually is, measured

Two independent boots stopped at the same place, with counters within a
few percent of each other, so this is deterministic rather than a race.
Everything below was read off a live wedged guest without restarting it.

**The guest is idle, not wedged.** All eight processors sit at the
instruction *after* a `hlt`, in Hyper-V's own idle loop:

```
cli
cmp  dword ptr gs:[0x340], 0
jg   done
sti
hlt
jmp  done+1        <-- RIP is here, on all eight
```

`info lapic` on every processor: ISR empty, IRR empty, TPR 0, PPR 0. No
stuck in-service vector, which was the first hypothesis and is wrong.

**Hyper-V has stopped scheduling the root partition.** `l2_entries`
freezes at ~82,000 on the boot processor and 320-420 on the other seven,
and never moves again. The application processors' exit rings still hold
the healthy shape - VMREAD, VMWRITE, VMRESUME, then a Windows kernel RIP
at `fffff801c1......` - so the root partition did run, and then stopped
being entered.

**No interrupt reaches any processor.** KVM tracepoints
(`kvm_apic_ipi`, `kvm_apic_accept_irq`, `kvm_inj_virq`,
`kvm_msi_set_irq`) over twenty seconds on the wedged guest recorded
*nothing*. Check the host's uptime against the trace timestamps before
believing a capture: the first one here looked like it had events, and
they were 110 minutes old, left in the ring buffer from an earlier run.

**The passed-through NVMe has no MSI-X.** `/proc/interrupts` on the host
shows no `vfio-msi` line for it at all, only a flat INTx count. So
either Windows never finished bringing storage up, or it did and nothing
has asked the disk for anything since.

**The timer is armed for 2.4 seconds and re-armed every ~100
microseconds.** `initial_count` 2,390,924,802 at divide-by-1, and
`current_count` sampled three times *increases* between samples while
`initial_count` stays put - which only happens if the register is being
written again. The loop is: write the timer's initial count (0x380),
write the end of interrupt (0xb0), repeat. 1.25 million exits on the
boot processor, its whole exit ring, two alternating RIPs, both plain
stores to the local APIC page:

```
mov  [r8+rax], edx          ; APIC[ecx] = edx     -> RIP ...457efe
mov  dword [rax+0xb0], 0    ; end of interrupt    -> RIP ...457ae1
```

So the timer never expires; something else wakes the processor, it
finds no work, acknowledges and re-arms. What that something is is not
yet identified - it is not the local APIC, because nothing is ever
pending in it, and it is not KVM, because KVM delivers nothing.

Ruled out by measurement, so they need not be re-proposed:

- **A stuck ISR vector blocking lower-priority delivery.** ISR is empty
  on all eight.
- **The TSC.** `tsc_offset` is zero in vmcs01 and the guest reads raw
  hardware, so a mis-scaled deadline cannot be the cause.
- **APIC virtualization being advertised and not implemented.** None of
  the APICv secondary controls are offered - `supported_secondary_controls`
  lists eleven bits and none of them is one - and the captured Hyper-V
  vmcs12 has `secondary 0x00000000`, so it asks for none of them.
- **Posted interrupts.** Same: the pin control is stripped in
  `build_vmcs02`, and the capture shows `pin 0x1e`, which does not
  include it. Note the capture also has **external-interrupt exiting
  clear**, so Hyper-V expects device interrupts to be delivered straight
  into whichever guest is running, through its own IDT.

**Hyper-V sets HLT exiting for the root partition** - primary
`0xa4206dfa`, bit 7 - so a halting Windows produces an exit Hyper-V
handles, and it is reflected correctly. vmcs01 sets no HLT exiting at
all, which is why `basic_reason::hlt` has no case in the main handler.

### Reading a wedged guest without gdb

`add-symbol-file` plus `$h->member` needs a processor inside the module,
and a healthy guest spends its time outside it - thirty attaches in a
row found none. The monitor's `xp` reads *physical* memory, bypassing
EPT and paging entirely, and the module base printed on serial is a
physical address, so any member can be read live with offsets computed
offline:

```sh
x86_64-elf-gdb -q -batch out/debug/x86_64/zpp_hypervisor \
  -ex "ptype 'zpp::hypervisor::hypervisor::record_exit'" \
  -ex "print/x (long)&(('zpp::hypervisor::hypervisor' *)0)->l2_entries"
printf 'xp/8gx 0x6a85af20\n' | nc -w 6 <rig> 4446
```

The `ptype` first is not optional - it expands the compilation unit, and
without it the class name does not resolve and every offset query
answers `No type "hypervisor" within class or namespace "zpp"`.

### The next three measurements, in order

Named here because the run they need was lost to a rig reboot, and
because two of them are one command each.

**1. Read `gs:[0x340]` on every processor.** This is the whole question.
The idle loop is `cli; cmp dword ptr gs:[0x340], 0; jg done; sti; hlt`,
so that counter is Hyper-V's "do I have work". If it is non-zero the
processor never halts at all - it takes the branch, returns, finds the
same value and comes straight back, which is a spin, not an idle. That
would explain the measured `resume_activity_state` of 0 on all eight
processors, since a guest that never halts never has an activity state
other than active. If it is zero, the processor really is halting and
something is waking it, and the question becomes what.

`info registers -a` gives GS base per processor; the counter is at base
+ 0x340 and reads through the monitor's `x`.

**2. Sample `exit_total` twice, not just `l2_entries`.** Tonight's dump
established that `l2_entries` is frozen, which says the root partition
is not being entered. It did *not* establish whether the boot processor
is still taking exits, because `exit_total` was read once. The APIC
ping-pong in the ring could be history rather than the present. The
local APIC's `current_count` increasing between samples says the timer
is being re-armed live, which is strong but indirect - `exit_total`
twice settles it outright, and both reads are `xp` through the monitor
with the offsets already recorded above.

**3. Then, and only then, the reference boot.** `~/vm/boot-kvm.sh` boots
the same Windows on the same hardware with no zpp in the path, using
`RELEASEX64_OVMF_VARS.fd.kvmrun` and six processors. Two things worth
comparing, both cheap: whether `/proc/interrupts` grows `vfio-msi`
entries for the NVMe on a working boot - tonight's wedged run had none
at all, only a flat INTx line - and what `kvm_apic_ipi` and
`kvm_apic_accept_irq` look like across the equivalent phase.

It is third rather than first because it costs a boot and answers a
comparative question, while the two above are reads against a guest that
is already in the state being asked about.

### What is *not* waking the boot processor, by elimination

Worth writing down because it narrows measurement 1 above to the only
remaining candidate.

The boot processor is demonstrably still doing something at about ten
thousand iterations a second. It is not being woken by:

- **the local APIC**, which has an empty ISR and IRR on every processor;
- **KVM**, which delivered no interrupt of any kind in a twenty second
  capture;
- **the VMX-preemption timer**, and this is the interesting one. It
  cannot be firing on any processor that has entered L2. `build_vmcs02`
  masks the timer out of the *union* of both sides' pin controls
  (`nested_entry.cpp:1089-1092`), so vmcs02 does not have the control;
  and `arm_controller_poll` reads and writes whichever VMCS is current,
  so from the exit tail it arms **vmcs02** and latches `armed_here`,
  never arming vmcs01 again. Both halves are already recorded above as
  the reason the exit-driven log channel dies; the consequence here is
  that the timer stops firing entirely once a processor runs a
  second-level guest.

Which leaves the guest's own code. The idle loop only reaches its `hlt`
when `gs:[0x340]` is zero; otherwise it branches straight to the return
and the caller comes back round. A processor that never executes the
`hlt` needs nothing to wake it, is never in a non-active activity state,
and burns a core - which is exactly the set of things measured. That is
why reading that counter is the first thing to do.

## Second static review: what it adds

Ranked by how well each explains the quiesce. The first item changes how
every other measurement here should be read.

**The local APIC values in this document are this VMM's decode, not
observations.** `watch_local_apic` write-protects the page, so every
guest write faults, and `on_ept_violation` resolves the register offset
from three sources - the guest-linear address when qualification bit 7
is set, otherwise the instruction's own addressing bytes, otherwise
nothing. The comment there records that on this machine *every* such
violation arrives with bit 7 clear. So "Hyper-V armed its timer for 2.39
billion counts" is a reading of an instruction, and both the register
and the value depend on the decoder being right about it. The only
existing guard is the instruction-length cross-check, and it is skipped
whenever the VMCS reports length zero, which SDM 30.2.5 permits.

If the decode is wrong the failure writes itself: a wrong value in the
timer's initial count is a scheduler tick that never arrives, which is
exactly "Hyper-V stops scheduling the root partition" with nothing
faulting. `physical_offset_present`, `physical_offset_agreed` and
`physical_offset_disagreed` were added to settle it - see the commit for
why the SDM and the recorded measurement disagree, and why running under
KVM means neither is decisive on its own.

**This VMM cannot be swallowing an interrupt that was raised.** vmcs01's
pin controls are NMI exiting and nothing else, vmcs02's are
`(pin01|pin12) & ~(preemption|posted)`, and the captured pin12 `0x1e`
has bit 0 clear - so at every level an external interrupt goes straight
into the running guest's IDT with no exit, and nothing in the tree
touches IRR, ISR or TPR. Zero KVM interrupt events over twenty seconds
therefore means **no interrupt is ever raised**, not that one is lost.
That is a much stronger statement than the capture alone supports and it
redirects the search entirely.

**`l0_wants_l2_exit` claims exits the guest hypervisor also asked for,
and never reflects them.** `intercept_msr(ia32_apic_base, true, true)`
is armed unconditionally, so every root-partition access to `0x1b` is
answered here - a read with the raw physical register, a write against
the real APIC - and Hyper-V, which certainly has `0x1b` in its own MSR
bitmap, is never told. KVM's `nested_vmx_l0_wants_exit`
(`.references/kvm/nested.c:6330-6395`) names no MSR and no I/O case at
all; those fall through to `nested_vmx_l1_wants_exit`. The same shape
covers the ACPI sleep port and MSR `0x830`. Fix: claim only where
`!l1_wants_l2_exit(...)`, and where the notification is needed *and* L1
wants the exit, observe and still reflect.

**vmcs02 shares vmcs01's VPID**, so `nested_transition_flush` issues a
single-context INVVPID on that VPID on every entry and every exit - SDM
31.4.3.1 makes that invalidate all PCIDs and all EPTRTAs, so Hyper-V's
own mappings go with the root partition's, twice per transition, and
under KVM each INVVPID is itself an exit. Not a wedge; a large
multiplier on the cost of the state Hyper-V is trying to make progress
from, and worth knowing before any timing argument is made.

**The diagnostic pump is not on the critical path** with only
`sink::counter` present - four records into a per-processor ring, no
lock. The expensive one was `zpp::hypervisor::log`, now excluded for the
three per-tick APIC registers, and still called for every IPI the guest
sends.

Checked and cleared by this review, so they need not be looked at again:
no `tsc_offset` is ever written to vmcs01; vmcs01 sets no RDTSC, HLT,
MWAIT or MONITOR exiting; only the ACPI sleep port is in the I/O
bitmaps, so there is no PCI-configuration interception; the whole
preemption-timer poll, NVMe borrow and wake-NMI machinery is behind the
disk sink and therefore dead in this build; and a stuck monitor trap
flag is excluded because an MTF exit with no step in progress reaches
`on_unhandled_exit`, which never fired.

## Third static review: one correction and two more

**Correction to the vmcs12 capture decode, and it matters.** `exit12 =
0x0003efff` has **bit 15 set**, which is "acknowledge interrupt on
exit". So Hyper-V *does* ask for it, this VMM advertises it in
`supported_exit_controls` and drops it. That was already recorded as a
gap; what was wrong was the reason it is latent. It is latent because
`pin12 = 0x1e` has bit 0 clear, so no external-interrupt exit ever
reaches the reflection path for *this* vmcs12 - and it stops being
latent the moment Hyper-V builds a vmcs12 for a child partition. Also
confirmed from the same tables: `entry12 = 0x13ff` is default-1 plus
IA-32e mode guest only, so `build_vmcs02`'s unconditional copies of the
PAT, EFER and PERF_GLOBAL_CTRL guest fields are inert here.

**The shadow EPT is not involved and can stop being looked at.**
`build_vmcs02` gates the whole shadow on `secondary12 & enable_ept`, and
the capture has `secondary12 == 0`, so vmcs02 gets EPT01's own root and
`on_l2_ept_fault` returns `deferred` on its first test. `build_shadow_ept`,
`compose_ept`, `shadow_ept_entry`, the slot pool and the eviction
machinery are all unreachable in the configuration that was measured.

The consequence is the useful part: **with no second-level EPT, the root
partition runs directly on EPT01 - including this VMM's write protection
of the local APIC page.** So the timer programming in the exit ring is
*Windows'*, decoded by this VMM, not Hyper-V's. That is the same
decoder question as before, one level further down than it looked.

**`IA32_BNDCFGS` is advertised and not implemented.** Exit-control bit
23, "clear IA32_BNDCFGS", is in `supported_exit_controls`, and
`load_l1_host_state` never writes the MSR. KVM's
`load_vmcs12_host_state` has `if (vmcs12->vm_exit_controls &
VM_EXIT_CLEAR_BNDCFGS) vmcs_write64(GUEST_BNDCFGS, 0)`. Latent - the
capture has bit 23 clear - and left alone for now rather than
implemented, because the honest fix is a `wrmsr` of an MSR that only
exists where MPX does, and a `#GP` on it in the host reaches the halt
loop with no recovery. **Either implement it or stop advertising it;
half-answering an interface is what this file is full of.**

**A refused MSR area now names the MSR.** The whitelist in
`msr_area_index_handled` is closed, the areas are guest memory a guest
hypervisor rewrites with no exit, and the check runs on every entry - so
the first index it does not know refuses that virtual processor's every
subsequent entry, permanently, with VM-instruction error 7 and nothing
faulting. That is indistinguishable from the boot being chased, and
until now the refusal named no MSR. Candidates a Windows host plausibly
autoloads and this list does not carry: `0x38f` PERF_GLOBAL_CTRL,
`0x1c4`/`0x1c5` XFD and XFD_ERR, `0x6a0`/`0x6a2` U_CET and S_CET, the
`0x6a4`-`0x6a8` shadow-stack set, `0x6e1` PKRS, `0x122` TSX_CTRL, `0xe1`
UMWAIT_CONTROL, `0x570`/`0x571` RTIT_CTL and STATUS.

**`on_nested_vmx_msr_read`/`write` index per-processor state with
`vmcs.vpid() - 1`, and vmcs02 carries vpid01.** So an access made by the
*second-level* guest is answered out of the first level's slot: the root
partition reading `0x3a` gets Hyper-V's `guest_feature_control`, and
writing it takes a `#GP` because Hyper-V's lock bit is set. Wrong level
in both directions. Part of the same family as `l0_wants_l2_exit`
claiming MSR exits without reflecting them, and it disappears with the
same fix.

## Fourth static review: the decoder is not the problem, and a correction

**The decoder is correct, and this was tested rather than read.** A
differential harness compiled `instruction.h` natively and compared it
against LLVM on a generated corpus: **10,251 accepted instructions with
zero length mismatches**, and **18,798 with a memory operand and zero
effective-address mismatches**, including SIB base=101 with no base,
index=100 meaning no index, REX.X and REX.B on those, RIP-relative, and
the `0x67` 32-bit truncation. The two forms in the exit ring were also
checked by hand: `41 89 14 00` is `mov [r8+rax], edx`, length 4; `c7 80
b0 00 00 00 00 00 00 00` is `mov dword [rax+0xb0], 0`, length 10. Both
exact.

So "a decode picks the wrong register or the wrong value" is **refuted**
for 64-bit code, and the `physical_offset_*` counters added earlier
answer a narrower question than they were added for - whether the offset
*source* is sound, not whether the decode is.

**Correction, and it matters more than the finding.** The previous
section said this VMM's decoder sits in the path of *the root
partition's* local APIC accesses. That cannot be what the live ping-pong
on the boot processor is: `l2_entries` is frozen, so the second level is
not being entered at all, so the APIC writes happening *now* - proved
live by the timer's current count rising between samples - are **Hyper-V's
own idle tick, at the first level**. The root partition's APIC traffic
is history in the ring. Every statement about "what the guest wrote"
has to say which guest.

**A pending step is destroyed by the next `build_vmcs02`** - fixed, see
the commit. The reason it is worth reading twice is the failure mode: it
leaves the local APIC page **writable for every processor**, so no
further violation is taken on it, nothing re-closes it, and this VMM
goes blind to every interrupt command, INIT and start-up IPI from that
moment, with no counter moving and nothing recorded. `zppstat` now
prints any processor whose step is still pending, because that is the
one-read measurement that settles whether it fired.

Evidence against its having fired on the boot measured so far: the boot
processor's exit ring is still full of violations on the APIC page, and
those only happen while the page is closed.

### Still open from this review, in order

**An MTF exit does not mean the stepped instruction retired.**
`on_monitor_trap_flag` assumes it did, closes the page, reads the
register back and reports a write. SDM `sdm.txt:201479-201481` and
`:201493-201497`: a monitor-trap-flag exit is also pending on the
boundary following delivery of a pending event, and following delivery
of a *fault* the instruction took - in both cases the guest's write has
not happened. On the APIC page that hands `on_interrupt_command` a stale
interrupt command register, which can adopt a start-up IPI nobody sent;
and when the instruction re-faults and is handled again, the same write
is processed twice. Fix: record `guest_rip` when arming the step and
report nothing if it has not moved.

**The close after a step invalidates only the local processor.**
`invalidate_ept`'s "safe direction" argument covers *arming* a watch - a
stale permissive entry means a missed observation, never a wrong one -
and does not cover the step, which makes the page writable and then
read-only again with a local INVEPT each time. Between the two, any of
the other seven processors can cache a writable translation and write
the APIC page with no exit at all. SDM `sdm.txt:206480-206484` requires
the shootdown; KVM does it with an IPI to every vCPU
(`kvm_flush_remote_tlbs`). The `ept_generation` counter to hang it on
already exists.

**Emulated APIC accesses are performed at widths KVM discards.** The
decoder accepts 1-, 2- and 8-byte stores and nothing checks width or
alignment. SDM `sdm.txt:170419-170424`: APIC registers "should be
accessed using 128-bit aligned 32-bit loads or stores", and any access
touching bytes 4 through 15 of a register "may cause undefined
behavior". KVM's `apic_mmio_write` drops anything that is not a 4-byte
aligned access, silently - while this VMM performs it and advances RIP.
Reads are worse: `kvm_lapic_reg_read` refuses any register outside
`kvm_lapic_readable_reg_mask`, `apic_mmio_read` ignores the refusal and
leaves the buffer untouched, and the end-of-interrupt register at 0xb0
is not in that mask - so reading it yields undefined data on this rig.

## Fifth static review: two corrections to what was believed measured

Both of these are retractions of statements made above on the strength
of counters that do not say what they were read as saying.

**"No VM entry is being refused" was not a measurement.** Only one
refusal path logs - the `build_vmcs02` branch at `nested_vmx.cpp:1510` -
and the three early returns in `on_guest_vmlaunch` hand back a plain
VMfail with nothing recorded at all: no log line, and
`vmx_instructions_refused` counts only the `#UD` path. So the absence of
a "guest vmlaunch refused" line is consistent with a guest hypervisor
being told error 5 thousands of times a second. Counters added; until
they have been read, treat "Hyper-V stopped asking" as unproven.

**`resume_activity_state == 0` does not mean the halt is not sticking.**
`resume_guest` samples it from whichever VMCS is current, and vmcs01
sets no HLT exiting - so a first-level `HLT` produces **no exit and no
resume**, and nothing ever writes the field for a halted L1. The value
read was left over from before the halt. Worse, on the successful
VMLAUNCH path `resume_guest` runs with vmcs02 current, so all four
resume fields and `record_exit` describe **L2** while the reason says
`vmlaunch` - the same defect as `record_exit` reading vmcs01 on a
reflect, in the other direction. The elimination argument recorded above
therefore rests on one leg fewer than it appeared to.

### The strongest single explanation found so far

**`VMPTRLD` of the VMCS that is already current used to destroy the
cached vmcs12** - fixed, see the commit. Worth restating because it fits
the evidence better than anything else and it explains the *absence* of
a log line rather than being contradicted by it:

- Here the cache is authoritative and the guest's region is stale.
  Nothing flushes on `VMWRITE` and `reflect_l2_exit` does not flush
  either, so from a guest hypervisor's first `VMWRITE` the region holds
  a revision identifier and 3584 bytes of zero.
- A redundant `VMPTRLD` replaced the live vmcs12 with that: launch state
  back to clear, every control zero, and the saved second-level guest
  state gone.
- The next `VMRESUME` then fails `vmresume_with_non_launched_vmcs`,
  which is one of the *silent* paths. `l2_entries` never moves again,
  nothing is logged, no counter moves, and that virtual processor can
  never be entered again - even `VMCLEAR` and `VMLAUNCH` would enter it
  at RIP zero, because the state was in the cache.

**The confirmation needs no rebuild and no reboot beyond getting the rig
back.** Read the cached vmcs12 out of the wedged guest with `xp`, using
the offsets `16 + ((width * 4 + type) * 28 + index) * 8`: offset 8 is
the launch state, `0x710` the pin controls, `0x718` the primary
controls, `0xCC8` the guest RIP. Zeroes there on a processor whose
`l2_entries` is non-zero mean the cache was wiped; `0x1e` and
`0xa4206dfa` mean it was not and this is refuted.

### Also open from this review

- **The deferred control-register handler applies L1's VMX state to
  L2.** Reached with vmcs02 current whenever `l1_wants_l2_exit` declines
  a CR0/CR4 access, it refuses a CR4 write clearing VMXE on the strength
  of `guest_in_vmx_operation[cpu]` - which is *Hyper-V's* flag - and
  injects `#GP` into the root partition for an architecturally legal
  write. It also writes `guest_cr0`/`guest_cr4` from the raw L2 value;
  KVM merges L1's owned bits back in `handle_set_cr0`/`handle_set_cr4`
  (`vmx.c:5428-5469`) for exactly this case. Narrow trigger - vmcs01's
  masks are only CR0.NE and CR4.VMXE - but wrong.
- **Exceptions this VMM injects into L2 bypass vmcs12's exception
  bitmap.** `inject_general_protection_fault` and
  `inject_invalid_opcode_exception` write vmcs02's entry-interruption
  field directly, so a vector L1 asked to intercept is delivered into
  L2's IDT with L1 never told. KVM routes injected exceptions through
  `nested_vmx_is_exception_vmexit` first.
- **`merge_nested_bitmaps` does 12 KB of byte-wise work per entry for
  nothing** when vmcs12's MSR-bitmap and unconditional-I/O controls are
  clear, which the capture says they are: the MSR page it builds is then
  discarded by `build_vmcs02` clearing the control, and the two I/O
  pages are bit-for-bit copies of this VMM's own. Point vmcs02 at the
  originals instead.

Checked and clean, so they need not be looked at again: `vmx_succeed`,
`vmx_fail` and `vmx_fail_invalid` match SDM 33.2 exactly and write
vmcs01's guest RFLAGS rather than the captured context, which is right;
vmcs12 field coverage is adequate, with read-only enforcement paired
correctly against `IA32_VMX_MISC` bit 29; and `l1_wants_l2_exit`'s MSR
case is right for this configuration, returning true immediately because
vmcs12's MSR bitmaps are clear.

## Sixth review: the nested VMX state machine is not broken, and now has a test

`tests/nested_vmx/build.sh` compiles the real `vmcs12.h` and the real
`nested_vmx.cpp` natively and runs **400 checks in about two seconds.
Zero failures.** An exhaustive sweep of all 32768 field encodings gives
420 accepted, 0 wrongly accepted, 0 wrongly refused, no aliasing in the
`[width][type][index]` slot map, `read_only()` exactly equal to
`type == exit_information`, and every Table 27-22 reservation enforced.
Round trips, the launch-state machine against SDM 33.3's orderings and
error numbers, the `#UD`/`#GP` preconditions, operand decode and
effective addresses are all correct.

**So the VMX emulation is not where the boot is being lost**, on the
evidence available, and the same is now true of the instruction decoder.
Both were suspected across multiple rounds and both were settled by
testing rather than by reading. That is the technique to reach for
first.

Two things the sweep reports rather than fails: about 260 encodings are
accepted for fields that exist on no processor (KVM refuses these with
`VMXERR_UNSUPPORTED_VMCS_COMPONENT` via `get_vmcs12_field_offset`), and
`index_capacity = 28` refuses four real Appendix B fields at index 28 or
above with error 12, consistent with not advertising those features.

**The launch state is set in `reflect_l2_exit`, not in
`on_guest_vmlaunch`, and that is correct** - KVM's `prepare_vmcs12` sets
`launch_state = 1` only for a non-entry-failure exit, and SDM
`sdm.txt:207978` puts the transition after MSR loading. Checked because
it looked wrong; it is not. Do not "fix" it.

### Still open from the save/load audit

- **`guest_ia32_pat` and `guest_ia32_efer` are saved into vmcs12 from
  VMCS fields the processor never writes.** vmcs02's exit controls are
  this VMM's verbatim and carry neither save control, so those fields
  still hold what `build_vmcs02` wrote at entry - vmcs12's own values. A
  guest hypervisor that sets either save control reads back what it
  supplied rather than what its guest ran with. SDM
  `.references/sdm.txt:204505`, `:204507`; KVM reads software state in
  `sync_vmcs02_to_vmcs12`. Dormant: Hyper-V's `0x0003efff` has bits 18
  and 20 clear.
- **`guest_ia32_debugctl` is saved unconditionally**, being in
  `guest_state_fields` as well as under the "save debug controls"
  branch. SDM `:204502` makes DR7 *and* IA32_DEBUGCTL conditional on
  that control; DR7 is handled right, this is not. KVM never syncs it
  back at all. Dormant: Hyper-V sets the control.
- `guest_ia32_bndcfgs` being saved unconditionally is **correct** - SDM
  `:27276`, "VM exits always save IA32_BNDCFGS into BNDCFGS field of
  VMCS". Do not change it.

## Seventh review: the exit reflection decision, tested — and four defects fixed

`tests/nested_exit/build.sh` compiles the whole of `nested_entry.cpp`
natively and drives `l0_wants_l2_exit` and `l1_wants_l2_exit` the way a
VM exit would. **733 checks.** It covers every basic exit reason 0-69
with its gate both ways *and* a cross-talk pass that sets every other
control bit and leaves only the gate clear - which is what catches a case
reading the wrong bit number and a two-setting test cannot. Also all four
MSR bitmap quadrants and the eight range boundaries, the CR decision in
full, all 31 non-page-fault vectors, both SDM 28.2 page-fault worked
examples, and the I/O bitmaps including the A/B boundary at 0x8000.

**It executed this session's CR0/CR4/CLTS/LMSW read-shadow fixes for the
first time. They pass.**

Four real divergences, all now fixed:

- **`l0_wants_l2_exit` swallowed MSR and I/O exits the guest hypervisor
  had also asked for.** It consulted this VMM's own bitmap first and kept
  the exit, so a second-level guest touching IA32_APIC_BASE,
  IA32_FEATURE_CONTROL, the VMX capability range or the ACPI sleep port
  was answered here and Hyper-V never learned. That set is exactly what a
  guest hypervisor presenting VMX to its own guest intercepts, so it was
  not a corner. KVM names none of them in `nested_vmx_l0_wants_exit`.
  This was on the open list for several rounds; the test is what made it
  concrete enough to fix confidently.
- **"Unconditional I/O exiting" is ignored when "use I/O bitmaps" is
  set** - SDM 28.1.3, `.references/sdm.txt:200725`, in parentheses. This
  tested unconditional first, so a guest hypervisor setting both got
  every I/O instruction reflected including ports it had cleared.
- **A wrapping I/O access exits** - same SDM sentence, `:200724`. A
  four-byte access at port 0xffff broke out of the loop and answered
  "not intercepted".

Differences from KVM that are *not* defects are asserted rather than
ignored, so one that stops reproducing fails the suite: the seven exit
reasons whose controls this VMM does not offer, and the three MOV-from-CR
forms where the architecture delivers no exit at all.

**Two harnesses now, 1133 checks, and neither needs the rig.** That is
the lesson worth carrying: the instruction decoder and the VMX state
machine were both suspected across multiple rounds and both were cleared
by testing, while the two real bugs in this area - the reflection
decision and the I/O rules - were found by the same tests within minutes
of them existing. Reach for a harness before another reading.

## Eighth review: the decoder's missing forms, added and proved

The decoder's coverage is not a nicety. Every form it refuses takes the
**stepping path** instead - make the page writable, arm the monitor trap
flag, let the guest's own instruction run, close the page again - and
that path still carries three open defects recorded above: a pending
step destroyed by the next `build_vmcs02`, a close that invalidates only
the local processor's EPT cache so any other processor can write the
page unobserved, and a read-back that reports a write the instruction
never performed. So a form learned here is not one access decoded
instead of stepped, it is one access that stops being exposed to all
three.

Five families were recorded as absent. Four were added, one defect was
found while adding them, and nothing was left refused for lack of
effort - the two that needed new machinery got it.

**CMP `r/m, r`, `0x38` and `0x39`.** Recorded above as "missing coverage
rather than a bug", which it was. Nothing new was needed: memory is the
first operand, so the flags are the ones `flags_after`'s `subtracting`
path already computed for `0x81 /7`, and `compares` switches it on.
`0x3a`/`0x3b` stay refused - the reversed direction subtracts the other
way round and there is nowhere in `flags_after` to say which side is
which.

**BT, BTS, BTR, BTC with a register offset, `0F A3`/`AB`/`B3`/`BB`.**
The immediate forms were already there, which was backwards: a constant
bit number is what a compiler folds, and the offset a driver computes
reaches a register.

**XADD, `0F C0`/`C1`.** Eleven lines and no new machinery. The sum is
`combine_with::add` and the register takes what memory held, which is
exactly what `result_for_register` already returns.

**INC and DEC, `FE`/`FF /0 /1`.** ADD and SUB of one everywhere except
the carry flag, which is the whole reason the instruction exists -
`.references/sdm.txt:53379`, "without disturbing the CF flag". A new
`preserves_carry` puts it back. `FF /2` through `/7` stay refused, and
this is where refusing matters most in the file: they are CALL, far
CALL, JMP, far JMP, PUSH and an invalid opcode, every one of which reads
the operand rather than modifying it, so emulating one as an increment
would write a device register the guest never asked to write *and* run
on from a control transfer that never happened.

**CMPXCHG, `0F B0`/`B1`.** The one that needed thinking about, and the
answer was still yes. Three things about it are unlike everything else:
what memory ends up holding depends on a register the encoding never
names, so the accumulator is captured at decode into `compare_value` and
`apply` stays pure; the flags are those of `accumulator - memory`, the
other way round from every other subtraction here, which the SDM does
not state and Bochs does; and the accumulator is written on
the failing branch **only**, which is not the same as writing it what it
already holds - a 4-byte write would clear the upper half of the 64-bit
register. Memory is written on both branches, which is the SDM's own
statement (`:42797`) and not a simplification.

### The defect found on the way

`0F BA /4-/7` took the immediate bit number **modulo the operand width**,
citing the SDM's "the offset is taken modulo the operand size". That
sentence is about a *register* bit base (`:38974`). A memory bit base has
no such limit: the processor moves the access to `Effective Address + (4
* (BitOffset DIV 32))` (`:38988`), for the immediate form as well as
the register one.

So `btsl $40, (%rax)` sets bit 8 of the dword at `[rax+4]`, and this
decoder set bit 8 of the dword at `[rax]` - the right bit of the wrong
word, with the word the guest named left untouched. Silent in both
directions, and on a watched page it is a write to a device register
four bytes from the one that was asked for.

Refused rather than followed, in both the immediate and the new register
form. The caller cannot use a moved address: `on_ept_violation` keeps
only the low twelve bits of what `effective_address` returns and pastes
them onto the page the violation reported, so an adjustment that left
the page would land back inside it. The architecture also says not to
point this at a device at all (`:38992`).

### What is still refused, and why - so it is not re-proposed

- **`0x3a`/`0x3b`, and the whole `0x02`-style direction.** Memory is the
  source and the register the destination. `apply` returns what memory
  should hold and there is no register-destination arithmetic path;
  adding one is a bigger change than the coverage is worth.
- **ADC and SBB.** They need the incoming carry flag, which `apply` does
  not receive. It could be threaded through - `flags_after` already takes
  `before` - and that is the shape of the fix if a guest is ever measured
  using them on a watched page.
- **NOT, NEG, MUL, IMUL, DIV, IDIV (`F6`/`F7 /2-/7`).** NOT and NEG would
  be easy; the rest write registers this decoder has no way to name.
- **Bit offsets outside the operand,** for the reason above.
- **Sixteen-bit code, and the address-size prefix in 32-bit code.** Both
  move where the instruction ends, so the answer would be wrong rather
  than incomplete.
- **Anything naming encoding four without REX.B,** which is the *host*
  stack pointer in the context handed to the decoder.

### The harness, committed this time

`tests/decoder/` - the differential comparison an earlier round ran and
threw away. It generates a corpus as assembly text, assembles it with
`llvm-mc` one ELF section per instruction, reads the bytes back out of
`llvm-objdump`, and compares `decode`'s length against the section size
and `effective_address` against the operand the disassembler printed.
Semantics cannot be settled that way - LLVM does not execute and the
host is arm64 - so `apply`, `flags_after` and `result_for_register` are
compared against a model written from the SDM, deliberately derived the
other way round: carry and overflow come from doing the arithmetic in
128 bits and asking whether it fits.

**7434 instructions, 53568 checks.** 7388 accepted with their lengths
compared, 5832 effective addresses compared, 46 forms that must be
refused, 1778 refused again as 16-bit code, and 52 instructions modelled
over 17 memory values and 5 incoming flag words. Three mutations were
run to show the checks bite: dropping CMPXCHG's conditional register
write fails 10, reversing its comparison fails 400, clearing
`preserves_carry` fails 257.

`tests/watched_page/` asserted the *absence* of CMP against a register.
That assertion is now the emulation, end to end, plus a second case with
memory below the register - which is what tells the two directions
apart, since a compare with the operands swapped sets carry and sign
backwards and the guest branches on them immediately.

## The boot stops because every processor is idle, not because anything fails

Measured 2026-08-10 on the rig, across four boots, three of them with the
same binary. **Nothing in the hypervisor is failing.** The done-condition
work has been looking for a broken mechanism and there is not one.

All eight processors sit in Hyper-V's own idle loop, halted:

```
cli
cmpl  $0x0, %gs:0x340     ; any pending work?
jg    done                ; yes -> return
sti
hlt                       ; no -> halt until an interrupt
b5e:  jmp  ...            ; <-- RIP sits here on all eight
```

Read out of a live guest: every vCPU has `RIP = ...a6b5e`, CPL 0,
`RFL = 0x246` (IF set), different CR3s, and that RIP is the instruction
*after* the `HLT` - which is where a processor rests in the HLT activity
state. `gs:0x340` is Hyper-V's per-processor pending-work word and it is
zero. The seven application processors wrote `TMICT = 0` within 37 ms of
each other and so have no timer left; the boot processor keeps a 2.4 s
tick, wakes, finds no work, and halts again.

So the machine is **idle with no wakeup source**, not deadlocked and not
spinning. The only thing that can restart it is a device interrupt, and
**not one is ever delivered**: every vector the guest accepts is its own
IPI or timer traffic (255, 239, 236, 237, INIT, SIPI, 47), and all 3,888
`kvm_msi_set_irq` events are the all-zero placeholder `dst 0 vec 0`. A
reference boot of the same guest directly on KVM, to the login screen,
programs 7,121 real routes (`vec 96` to each of dst 0-7, plus 162, 80,
81, 176).

The freeze is exactly reproducible: `l2_entries` on the boot processor
stops at ~82,300 and each application processor at ~405-421, against
~500,000 per processor in the reference.

### Eliminated by measurement, so do not re-propose

Each of these was a live hypothesis and each is dead. The measurement is
given so it does not have to be re-derived.

- **Application-processor start-up is not broken.** 7 start-up IPIs go to
  hardware and 14 are dropped as `activity 0x0 is not wait-for-sipi` -
  and those drops are *correct*: hardware ignores a start-up IPI to a
  processor that is not in wait-for-SIPI. Identical counts before and
  after the activity-state series, which changed nothing observable.
- **Interrupt commands are delivered.** The last four IPI commands in the
  log ring map one-to-one onto KVM's `kvm_apic_ipi` with the right
  destinations, each followed by `kvm_apic_accept_irq` on the right APIC
  ids. An earlier session reached the same conclusion; see the stash
  named `icr-interception: premise refuted, SIPI is delivered`.
- **The monitor-trap single-step path never runs.** `stepped_writes` 0,
  `apic_writes_undecoded` 0, `watched_accesses` all zero. Anything that
  needs a stepped local-APIC write - a doubly-acted interrupt command, or
  the shared page opened during the step - cannot be firing.
- **Nested entries are not refused.** `nested_vmfail_count` 0 on all
  eight.
- **EPT memory types are right.** Read live: NVMe BAR0 `0x7011108000` ->
  `0x7011000487` and GPU BAR0 `0x7010000000` -> `0x7010000487`, bits 5:3
  = 0, uncacheable; DRAM `0x100000000` -> `0x1000004b7`, bits 5:3 = 6,
  write-back. `63a5d17` is what makes that true. Cached writes to the
  MSI-X table are therefore not the mechanism.

  The comment in `initialize_ept` is still wrong about *why*: it credits
  `IA32_MTRR_DEF_TYPE` being UC, and this firmware sets it to **WB**
  (`def type 0xc06`, type 6). The BARs come out UC because a variable
  MTRR covers the 64-bit aperture, not because of the default.
- **The TPR shadow and the xAPIC/x2APIC transition are implemented
  correctly** - audited against `lapic.c` and `vmx.c`, no divergence that
  can lose an interrupt.

### Where to look next

Why no device interrupt is ever delivered, given the guest is idle and
waiting for one. The two branches, and neither is settled:

- the guest never programs MSI-X at all, in which case the absence is a
  symptom of stopping earlier and the question is what it stopped for;
- or it programs it and the write never reaches the emulation behind
  VFIO, in which case the route is never created.

**Settled, from a capture started at power-on rather than part way in.**
The first branch is the right one: the guest never programs a route at
all. That capture spans 468 s, application-processor start-up begins
234 s before its end and finishes 77 s before it, and no real route ever
appears. The reference programs its first one **13 s after**
application-processor start-up, so this ran roughly eighteen times longer
than needed for an absence to mean something.

So the missing device interrupts are a **symptom**. The guest stops
somewhere between starting its application processors and initialising
storage, and it is that stop which has to be found - not the interrupt
path, which nothing has yet shown to be broken.

### Two cheap instruments that would have saved a day

- **Log the eight variable MTRRs**, not just the summary at
  `hypervisor.cpp:655`. Their coverage had to be recovered by reading EPT
  entries out of physical memory by hand; one line each costs eight ring
  slots at boot.
- **Count device-vector deliveries.** Everything above was inferred from
  KVM's trace because the hypervisor itself says nothing about interrupts
  arriving at the guest.

### The divergence point, measured against the reference

The same forty seconds after application-processor start-up, in both
runs, counted through KVM's own APIC so the two are comparable (unlike
MSR or CPUID exits, which zpp answers itself and KVM therefore never
sees):

| vector | reference, boots | behind zpp, hangs |
|---|---|---|
| SIPI 159 / INIT 0 | 56 / 49 | **56 / 49** |
| **239 (0xef)** | **84,286** | **29** |
| 255 (0xff) | 18,501 | 0 |
| 47 (0x2f) | 15,479 | 0 |
| 96 (a device vector) | 3,261 | 0 |
| 32 (0x20) | 511 | 1,002 |

Start-up traffic is **identical on both sides**, which settles from a
third direction that application-processor start-up is not where these
diverge. What differs is everything after it: the moment the reference
has its processors, its interrupt traffic climbs to roughly 2,100 a
second on vector `0xef` and stays there, while behind zpp that vector is
delivered twenty-nine times in forty seconds and vectors `0xff`, `0x2f`
and `0x60` never arrive at all.

So the divergence is not in *starting* the processors but in whatever the
guest does with them immediately afterwards. Note this is still an
observation about the symptom - a guest that has gone idle delivers few
interrupts by definition - so the open question is unchanged and the
value here is the timestamp: the two runs are indistinguishable up to
start-up and separate immediately after it. That is the window to
instrument.

### Vector 0xef is the local APIC timer, and it is the thing that stops

Counted over the whole reference capture: vector `0xef` is accepted
**244,167** times and carried by **zero** interrupt commands, so it is not
an IPI - it is the local APIC timer. The same capture writes
`APIC_TMICT` **539,169** times, about 1,500 a second, which is a one-shot
timer being re-armed on every tick, and `APIC_TDCR` 49,558 times.

Behind zpp, a sixty second capture of the settled guest holds **30**
`APIC_TMICT` writes. Roughly three thousand times fewer.

Injected vectors in the reference, for whoever needs to recognise them
later: `0xef` 249,279, `0x20` 123,951, `0xff` 70,693, `0x2f` 35,622,
`0xd1` 23,383, `0x60` 3,936.

This is **not yet a cause**. A guest that has gone tickless-idle stops
re-arming its timer by construction, and the boot processor does still
hold a ~2.386 s one-shot, wakes on it, finds `gs:0x340` zero and halts
again. What it settles is which mechanism the working boot leans on, and
it names two questions the instrumented build should answer directly:

- which MSRs the guest touches in the window, and whether it ever asks
  for **TSC-deadline mode** (`IA32_TSC_DEADLINE`, MSR `0x6e0`) rather
  than `TMICT`. zpp answers MSRs behind its own bitmap, so KVM's trace
  can never see this and every capture so far has been blind to it;
- whether any timer arming the guest performs is being lost rather than
  declined.

### Correction: being halted in that idle loop is not the symptom

The reference was read the same way, at the login screen, and looks the
same:

```
5 x RIP=fffff86f211a6b5e   <- the same address, one past the HLT
2 x RIP=fffff86f211a8544
1 x RIP=fffff86f211311c2
```

So a **healthy** Windows parks most of its processors on exactly the
instruction the failing one does. "All eight halted in the guest
hypervisor's idle loop with its pending-work word zero" describes *idle*,
and idle is the correct state for a machine with nothing to run. It is
not evidence of a fault, and the entry above should be read that way -
what distinguishes the two is only that the reference goes on to do work
and the other never does.

That also settles a live worry: the Windows installation is healthy.
After roughly eight hard kills in a session it was reasonable to suspect
the recovery path, which is single-processor and would mimic a
hypervisor failure. It boots normally without the hypervisor, so it is
not that.

The lesson generalises past this bug: **compare against the reference in
the same state before calling anything a symptom.** Two of the strongest
looking observations this session - this one, and the ratio of local APIC
timer arming - dissolve once the comparison is made at the same phase
rather than between a booting guest and a settled one.

### The guest never resumes work after start-up, measured at the same phase

Events per second either side of application-processor start-up, both
captures, same event set:

| | reference | behind zpp |
|---|---|---|
| first second | 44,978 | 202 |
| next ten | 106,040 -> 233,475, then a steady 200,000-300,000 | a steady **100** |
| after that | continues | **~0.5** |

This is the comparison the earlier ones should have been. It is
phase-aligned, it does not compare a booting guest against a settled one,
and it survives: even during its busiest ten seconds the guest behind zpp
runs at **100 events a second against the reference's 200,000**, a factor
of about two thousand. A hundred a second is a bare timer tick and
nothing else, and at ten seconds past start-up even that stops, leaving
the ~2.4 s one-shot.

So the guest does not slow down or wedge part way through work - **it
never resumes work at all** once its application processors are started.
That is a much narrower statement than "it goes idle", and it fits the
other number that has been sitting in plain sight all along: each
application processor enters second-level guest execution only ~350-420
times before stopping, against ~500,000 each in the reference.

The shape that predicts all of it is an **application-processor
rendezvous**: the boot processor starts the others, waits for them to
report ready, and they never finish reporting. Start-up itself is known
good - the start-up IPI traffic is identical on both sides - so the
failure would be in what an application processor does *after* it starts
and before it checks in.

That is what the exit-reason histogram and the MSR/hypercall recording on
`diag/post-startup-window` exist to answer: on a frozen guest, what did
an application processor spend its ~400 entries on, and what did it ask
for last.

### What the processors actually spend their exits on

First read of the per-processor exit-reason histogram, taken at the
freeze (`l2_entries` 82,315 on the boot processor, 420/396/378 on the
first three application processors).

Boot processor, 1,267,558 exits:

| reason | count |
|---|---|
| 23 VMREAD | 462,566 |
| 48 EPT violation | 375,461 |
| 25 VMWRITE | 111,853 |
| 24 VMRESUME | 74,066 |
| 18 VMCALL | 73,306 |
| 21 VMPTRLD | 36,794 |
| 50 INVEPT | 14,499 |
| 10 CPUID | 6,431 |
| 31 RDMSR / 32 WRMSR | 1,151 / 60 |

VMRESUME (74,066) matches `l2_entries` (74,069 when read) to three
counts, and VMCALL is nearly the same again - so essentially every entry
into the second-level guest ends in a hypercall from Windows to its
hypervisor, which is what a healthy root partition does.

An application processor, 5,225 exits: 417 VMRESUME, 2,169 VMREAD, 1,261
VMWRITE, 2,866 CPUID, 646 EPT violations, 89 RDMSR, 58 WRMSR, 2 HLT, and
exactly **one INIT and one start-up IPI**. Its last four exits are an
alternating VMREAD/VMWRITE pair at two fixed instruction addresses, with
the new detail field zero, after which it halts - and a halt leaves no
exit, because vmcs01 does not set HLT exiting. Nothing faults, nothing is
refused, and no unusual MSR or hypercall appears at the end.

**The overhead this exposes is worth recording on its own.** VMREAD and
VMWRITE together are 574,419 exits, **45% of everything the boot
processor does**, because this tree does not enable VMCS shadowing - so
every field a guest hypervisor touches in its own VMCS traps. Add the
375,461 EPT violations, most of them the write-protected local APIC page,
and three quarters of all exits are the cost of nesting rather than work.
That is not a correctness bug and it is not the hang - the freeze lands on
the same `l2_entries` value across boots, which slowness alone would not
do - but it should be measured before anything concludes the guest is
merely slow, and VMCS shadowing is the obvious lever if it ever matters.

### The hang is processor-count dependent

`ZPP_CPUS` now overrides the guest's processor count in both launchers,
so this is a single-variable experiment. Booted on **two** instead of
eight:

| | eight processors | two processors |
|---|---|---|
| boot processor `l2_entries` | 82,315, frozen | 82,036, frozen |
| application processor `l2_entries` | ~405, frozen | 2,199 -> 24,610 -> 45,165 -> **75,620**, still climbing |

The boot processor stops at the same place either way - within 300 of the
same count, across every boot measured - but with two processors the
application processor **keeps working indefinitely** instead of stopping
at a few hundred entries. Sampled mid-run its RIP is inside zpp's own
module (base + 0xe3b3) in root operation, so it is genuinely executing
rather than parked.

That is the first variable found that changes the outcome, and it points
back at the application processors after all - which the identical
start-up IPI traffic had seemed to rule out. The two are compatible:
*starting* a processor works, and something about running **eight** of
them does not. Worth testing 4 and 6 to find where it breaks, since a
threshold would say a great deal about which resource runs out.

Do not read this as "two processors boots Windows" until it has been seen
to reach the login screen - at the time of writing it is progressing, not
finished.

The two-processor guest kept going: 2,199 -> 24,610 -> 45,165 -> 75,620
-> 233,249 -> 262,651 second-level entries on the application processor,
at roughly 260 a second while working and settling to about 100 a second
- which is the shape of a guest that has finished booting and gone idle,
though that was not confirmed on the screen at the time of writing.

The boot processor stays frozen at 82,036 throughout, so even here it is
only the application processor doing the work. That is itself odd for a
healthy two-processor Windows and should not be waved away: it may mean
the same fault still bites the boot processor and Windows simply
schedules around it when there is another processor to use.

### The threshold is between one and three application processors

Bisected with `ZPP_CPUS`, same binary and same guest each time:

| processors | application processors | outcome |
|---|---|---|
| 2 | 1 | boot processor frozen at 82,036; the application processor passes **312,000** entries and keeps going |
| 4 | 3 | boot processor frozen at 82,134; application processors frozen at **375 / 348 / 323** |
| 8 | 7 | boot processor frozen at 82,315; application processors frozen at **~405 / 396 / 378** |

The boot processor stops at the same count in all three - within 300 of
82,300 - so whatever stops *it* is independent of how many processors
exist. What depends on the count is whether the application processors
keep running, and the boundary lies between one and three of them.

**That reopens a finding from the local APIC audit that had been set
aside.** `start_up_lock` (`hypervisor.cpp:6605`) serialises the
*callers* of the start-up path rather than its *targets*, and the
trampoline has a single, non-reentrant stack (`:6650`). One application
processor can never collide with another; two or more can. The bisect and
that code agree, which is a good deal more than either says alone.

Not yet confirmed, and the honest gaps:

- **Three processors settled it: two application processors also fail.**
  Boot processor frozen at 82,068, the two application processors at
  **365** and **331** - the same few hundred entries as four and eight.
  So the boundary is exact: **one application processor works, two or
  more do not.** That is the signature of a shared resource with no
  mutual exclusion between *targets*, which is precisely what the audit
  described.
- the two-processor guest was never confirmed on the screen, so "it
  works" means "it keeps doing work", not "it reaches the login screen".
- the boot processor freezing at the same count regardless is
  **unexplained by any of this** and may be a second, separate fault.

A note on the harness rather than the hypervisor: an odd `ZPP_CPUS`
originally produced a fractional `smp.cores` and qemu refused to start,
which is why both launchers now drop to one thread per core when the
count will not divide evenly.

### Four concurrency defects on the start-up path, and what is left

Found by reading the path the bisect above implicates, and pinned by
`tests/ap_start_up`. In the order they matter:

1. **The VMXON and VMCS region addresses were handed from
   `initialize_vmx` to `enter_root_mode` through two shared members.**
   That is the only piece of a launch that was not indexed by the slot,
   and the window between the write and the VMXON is protected by
   nothing on the executing processor - `start_up_lock` is held by the
   *starter*, and it is released on its own timeout as well as on
   success. A second processor arriving inside the window sent the
   first one's VMPTRLD at its region. One VMCS on two logical
   processors is undefined, not merely racy: SDM 25.1 says software
   must not do it and the processor does not check. Fixed by deleting
   the members - `enter_root_mode` takes its slot and derives both as
   locals.

   This is the one that fits the measurement. Two processors sharing a
   VMCS do not fail to start; they start, run, and then corrupt each
   other's guest state on every exit, which is exactly "adopted, and
   then stopped after a few hundred second-level entries".

2. **The target published its wait-for-SIPI activity state after its
   wait instead of before it.** `start_up_processor` gates a guest's
   start-up IPI on `resume_activity_state[slot]` and only then
   compare-exchanges into the mailbox, so a target whose record still
   says `active` has its IPI dropped - and dropped is returned as
   `adopted`, which swallows the guest's write. That record is written
   by the exit path on the way *out* of the handler, so for the whole
   of `emulate_init_signal`'s software wait - up to two million
   iterations - it held the value from before the INIT. The software
   hand-off was therefore unreachable from `start_up_processor`, which
   is the one path a guest's own INIT-SIPI-SIPI takes.
   `enter_or_park_l2` already published `l2_activity_state` before
   waiting, which is why the second-level hand-off worked and this one
   did not.

3. **`processor_slot` was an unsynchronised allocator.** Scan then
   append, reachable from every processor's exit handler. Two
   processors asking about two different unknown identifiers both took
   the same slot - and a slot is the VPID, the VMXON region and the
   VMCS region. 6,149 collisions over 400 rounds of 8 in the harness
   without a lock, 0 with one.

4. **`start_application_processor` cleared `start_up_launched[slot]` for
   a processor that could already be running.** That flag is what
   `wait_for_ept_acknowledgement` uses to decide who must answer an
   extended page table change; a processor whose flag is clear is
   skipped on the argument that it holds no translation. Clearing it
   for a running processor removes it from every rendezvous from then
   on, silently and for good, because only `main` sets it again. The
   first attempt at the fix - re-testing `processor_virtualized` under
   the lock - was **not enough**, and the harness said so: 5 rounds in
   400 still cleared it, because the target marks itself without taking
   that lock. The clear is gone entirely instead.

#### The two-million-iteration spin, since it was asked about directly

It is not the defect, and it costs more than it looks.
`start_application_processor` sends the start-up IPI and then spins up
to 2,000,000 times waiting for `start_up_launched[slot]`, inside a VM
exit, holding `start_up_lock`. What that buys and what it costs:

- **It does serialise the targets**, indirectly and by accident of
  where the flag is written: the target has left the shared trampoline
  stack and taken its stack index long before `main` sets the flag. So
  on the success path the single trampoline and the single stack really
  are safe. That is worth knowing, because it is not what the comment
  claimed and it is not what the lock's name suggests.
- **The hole is the timeout, not the lock.** Giving up does not stop
  the target. A processor that is merely slow is still on that stack
  and still inside the VMX region window when the lock is released.
  Closing that needs the target to acknowledge - a second flag it sets
  once it is off the shared stack, which the starter waits on before
  releasing - and is a bigger change than any of the four above.
- **It can starve another processor's start-up.** A second sender
  blocks on `start_up_lock` inside *its own* VM exit, in VMX root mode,
  and a layer below discards start-up IPIs and blocks INIT for every
  instant a processor is in root mode (KVM's
  `vmx_apic_init_signal_blocked` is `nested.vmxon && !is_guest_mode`).
  So a sender waiting on the lock is a processor that cannot itself be
  started. With one application processor there is no second sender;
  with three there are. It is a plausible second-order contributor and
  it was not measured.
- It is also `pause` in root mode, which is non-root to the layer
  below, so pause-loop exiting fires and the vCPU is descheduled
  repeatedly. Two million of those is not a short delay.

Reducing the bound is not obviously right - it trades a rare hang for a
more frequent unadopted processor - so it is left alone and written
down.

#### Reading a boot for these

- **Fix 1**: every processor should reach `launching guest on virtual
  processor N` with a distinct N, and `exit_reason_counts[cpu]` should
  keep rising for each. The failure it fixes shows as two processors
  whose exit rings interleave nonsense - a slot reporting exits that
  belong to another - or as a `vm_entry_failure` on a VMCS a processor
  did not build.
- **Fix 2**: `guest start-up ipi for cpu N, vector V, handed over` is
  the line that was never printed before. Its absence, next to
  `cpu N init: ... software wait 1`, is the defect. One of those per
  application processor per INIT-SIPI-SIPI is the fixed state.
- **Fix 3**: `number_of_known_processors` should equal the processor
  count, and `apic_id[]` should hold each identifier exactly once.
- **Fix 4**: `unresponsive_processors` should stay at whatever it was;
  a processor silently dropped from the rendezvous makes it *not* rise
  when it should have.

The number the bisect reads - second-level entries per application
processor - is the one to compare. Fixed means they keep rising past
the few hundred they stop at now, toward the ~500,000 of a healthy run,
rather than any particular value.

## The freeze is a lost wake-up, not a start-up race

Measured on 2026-08-10, four processors, and then measured again on a
second build with the start-up concurrency fixes in. **The two readings
are identical**, so those fixes - real defects, with a harness that
counts 6,149 slot collisions in 400 rounds without them - are not this
failure. Recorded here so the next person does not re-derive them as a
candidate.

What the frozen machine actually holds, read through the QEMU monitor
with no debugger attached:

- **Every processor is halted in Hyper-V's idle loop**, at the same
  offset `a6b5e` in both boots, and the bytes there settle what that
  means:

      b50: fa                          cli
      b51: 65 83 3c 25 40 03 00 00 00  cmpl $0x0, %gs:0x340
      b5a: 7f 04                       jg   b60
      b5c: fb                          sti
      b5d: f4                          hlt
      b5e: eb 01                       jmp  b61      <-- RIP is here

  RIP one past the `hlt`, `RFL=0x246` so interrupts are enabled, and
  `HLT=0` from the monitor because the halt is in non-root operation
  rather than in KVM's own halt path. `HLT` exits number 1 or 2 per
  processor for the whole boot, so halting is not being trapped: the
  processor really is stopped, waiting for an interrupt.

- **Nothing will ever wake them.** `info lapic` per processor:

      cpu0  LVTT one-shot vec 0xef   initial_count = 2,379,522,644
      cpu1  LVTT one-shot vec 0xef   initial_count = 0
      cpu2  LVTT one-shot vec 0xef   initial_count = 0
      all   IRR (none)   ISR (none)

  The application processors have **no timer armed at all** and nothing
  pending. The boot processor's is armed with ~2.38e9 at divide-by-1,
  which against KVM's 1 GHz APIC bus is a period of about **2.4 seconds**
  where a scheduler tick should be near a millisecond. It is re-armed to
  the same value every time it fires, so cpu0 limps at roughly one
  exit per second - 20 exits in 24 s, measured - while the others are
  frozen exactly.

**That is the 2000x timer-arming rate gap from the earlier phase-aligned
comparison, seen from the other end.** It was recorded then as a rate;
this is the register it comes out of.

So the question to answer is no longer "what stops the application
processors" but **"why does the guest compute an APIC timer count three
orders of magnitude too large, and why do the application processors
never get one at all"**. A count of ~2.38e9 at a plausible ~2.4 GHz TSC
is suspiciously exactly one second, which points at the guest deriving
the APIC timer's rate from the TSC's.

Eliminated while getting here, none of them worth re-testing:

- not a deadlock in this VMM - `start_up_lock` and
  `mapping_window_lock` both read 0, and `unhandled_exit` and
  `vm_entry_failure` are all-zero;
- not a failure to adopt - all four `start_up_launched` bytes read 1,
  and the application processors each run ~350 second-level entries and
  ~4,500 exits before stopping;
- not an unhandled exit - the last thing in every stalled processor's
  exit ring is an ordinary `VMWRITE` in the middle of Hyper-V's normal
  `VMRESUME -> exit -> VMREAD x4 -> VMWRITE -> VMRESUME` cycle;
- not lost TMICT observation - writes to `0x380` are deliberately not
  logged (`hypervisor.cpp:6002`), so their absence from the log ring
  says nothing.

### A boot that reaches no hypervisor looks exactly like a hypervisor bug

`scripts/rig-boot-test.sh` reset the guest's NVRAM from
`RELEASEX64_OVMF_VARS.fd.orig` before every run. No saved copy of that
file carries a boot option naming `\EFI\zpp\zpp_loader.efi`, so the reset
booted Windows with nothing underneath it - and every counter read
afterwards was a real number describing a machine this VMM was never on.
Removed, and the tell is now checked and fatal before anything is
measured: the loader says `zpp:` on serial long before the firmware hands
over.

### The reference settles it: 1214x, and the application processors get none

The same guest, the same four processors, booted by `boot-kvm.sh` with
nothing of ours underneath, read the same way once it had settled:

| processor | reference `initial_count` | under this VMM |
|---|---|---|
| boot        | 1,961,755 | 2,379,522,644 |
| application | 1,957,062 | 0 |
| application | 1,944,081 | 0 |

Every processor's timer is armed in the reference, all to about
1.95e6 - roughly a 2 ms tick against KVM's 1 GHz APIC bus, which is
what a scheduler tick should look like. Under this VMM the boot
processor's count is **1214x larger** and the application processors
have **no timer armed at all**.

The reference also parks three of its four processors at the *same*
`a6b5e` idle-loop address, which is the second time that address has
been misread here as a symptom. It is not. Idling there is what a
healthy machine does; what distinguishes the failure is that our
processors idle there with nothing armed to wake them.

So the defect is in what the guest is told about time, not in how it is
started. The next thing to read is the synthetic MSR path
(`hypervisor.cpp:9778`), which passes the whole `0x40000000`-`0x4fffffff`
range straight through to the layer underneath when
`nested_vmx::pass_through_hypervisor_interface` is on - including
`0x40000020` `TIME_REF_COUNT`, `0x40000022` `TSC_FREQUENCY` and
`0x40000023` `APIC_FREQUENCY`. A count of ~2.38e9 against a plausible
~2.4 GHz TSC is suspiciously exactly one second, so the guest deriving
the APIC timer's rate from the TSC's is the first hypothesis to test.
Both the read of `0x40000020` and a write of `0x40000071` - the
synthetic ICR - appear in the stalled processors' exit rings, so the
guest is using that interface.

**A caveat on that, before anyone acts on it.** "The guest is told the
wrong thing about time" is one reading of those numbers and not the only
one. The other is that 2.38e9 is not a mis-scaled tick at all but an
honest long timeout - about 2.4 seconds against a 1 GHz bus - which is
what a hypervisor waiting on something that never arrives would arm. On
that reading the missing timers are a *consequence* of Hyper-V never
getting far enough to start scheduling, not the cause of it, and the
1214x is a comparison between two different kinds of timer rather than
the same one mis-programmed.

Both switches that would hand the guest a frequency are off -
`nested_vmx::pass_through_hypervisor_interface` and
`announce_hypervisor`, `nested_vmx.h:78` and `:150` - so the guest's
reads of `0x40000022` and `0x40000023` take the `#GP` this VMM gives any
MSR it does not implement, and it must calibrate. The reference is
simply told, because the rig runs `-cpu host,...,hv-passthrough`.

What separates the two readings is cheap and has not been done: arm the
KVM trace, boot both, and compare `kvm_apic_accept_irq` for vector
`0xef` over the same phase of boot. If ours accepts them at 1/1214 of
the reference's rate, the counts are the same timer mis-scaled. If ours
stops accepting them entirely at a point, something else wedged first
and the timer is downstream of it. Do that before changing a switch.

### The discriminator ran, and it says mis-scaled rather than downstream

The measurement named above was taken: `kvm_apic_accept_irq` streamed
across a four-processor boot under this VMM, bucketed by ten seconds.

Vector `0xef` arrives at **4 to 5 per ten seconds - about 0.42 Hz** and
does so for the *entire* live window, including the stretch where the
boot processor was climbing normally from zero to its 82,000 second-level
entries. It is not a rate that was healthy and then collapsed when
something wedged.

That number closes the loop with the register:

- programmed count 2.38e9 at divide-by-1 against KVM's 1 GHz bus is a
  period of 2.38 s, so **0.42 Hz** - exactly what was counted;
- the reference's 1.95e6 is 1.95 ms, so **512 Hz** per processor;
- **512 / 0.42 = 1219**, which is the same 1214x the counts gave.

Two independent measurements - a register read and an interrupt count -
agreeing on the same ratio. So of the two readings offered above, the
mis-scaled one is right and the "honest long timeout armed after
something else wedged" one is wrong: the timer was never running at the
correct rate, from early boot onward, well before anything stopped.

One honesty note on the comparison: the reference's 512 Hz is derived
from the count it programs, not counted in a trace of its own. The
tracing was armed after the reference run had finished, and the ring
still held stale events from earlier captures - timestamps spanning
4316 to 10804 - so the bulk of that file's vector `0xef` lines belong to
older boots and must not be counted. Only the live window
(10350 onward) is this boot. A reference capture over the same
tracepoint would make the comparison direct and has not been done.

### Correction: handing the guest the interface changes nothing

The commit above concluded, from the accepted-interrupt rate agreeing
with the programmed count, that the timer is mis-scaled and the freeze
follows from it. **That conclusion is wrong, and the experiment that
overturns it is one boot.**

`nested_vmx::pass_through_hypervisor_interface` was turned on, which
stops this VMM answering the hypervisor CPUID range at all - the guest
sees the outer layer's full Hyper-V enlightenments - and forwards every
synthetic MSR down, including the frequency MSRs whose absence was the
proposed cause. Booted on four processors:

- during boot the application processors now arm their timers to
  `0xFFFFFFFF` at divide-by-2, which is the arm-at-max-and-read-TMCCT
  shape of a calibration, so they get further than before;
- **the end state is identical.** Boot processor frozen at 82,124,
  application processors at 374 / 351 / 323, every processor parked one
  byte past the `hlt` in the idle loop, `IRR` and `ISR` empty, the
  application processors' timers back to `initial_count = 0` and the
  boot processor's at 2,382,117,305 - the *same* count as without the
  interface.

The same count with and without the frequency MSRs available is the
point: that count is not a calibration artefact. So 2.38e9 is more
likely an honest ~2.4 s wait armed by a hypervisor that has stopped
having work to do, and **the timer state is downstream of the freeze
rather than its cause** - which is exactly the alternative reading
recorded two commits ago and then dismissed too early.

What still stands, because it was measured rather than inferred: every
processor is halted with nothing pending and nothing armed to wake it,
no lock is held, no exit is unhandled, no entry failed, and the
application processors each stop mid-cycle after about 350 second-level
entries. What does not stand is the claim about *why*.

**Direction, and a constraint on it.** This has to work with no
`hv-passthrough` underneath and no forwarding to an outer layer -
`pass_through_hypervisor_interface` is therefore not a candidate fix and
has been turned back off. Whatever the guest needs has to be implemented
here. But this experiment says the missing interface is not what stops
the boot, so the next question is the one the exit rings pose directly:
what is Hyper-V waiting for when it stops scheduling on an application
processor after ~350 entries.
