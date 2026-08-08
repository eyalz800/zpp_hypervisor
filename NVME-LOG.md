# Logging to the disk while the guest owns it

`DIAGNOSTICS.md` records that writing to disk while the guest is alive is
impossible, and gives four reasons from the NVM Express Base Specification.
Every one of them is correct. This document is how the channel is built
anyway, which reason turned out to be a lever rather than an obstacle, and
what it costs.

Read `DIAGNOSTICS.md` first. It says what makes something a channel here,
and this one is judged against the same three rules: a program can read it,
it works while the guest is stuck, and it cannot fail silently.

## The shape of the answer

**Create our own submission queue *and* our own completion queue.**

Every scheme that shares a queue with the guest fails at the same point:
our completion lands in the guest's completion queue carrying a command
identifier it never issued. If our submission queue is bound to *our*
completion queue, the guest is never involved. The steady state is then
trivial and shares nothing - write a 64 byte command into our submission
queue, ring our doorbell, poll our completion queue - and there is no
interception on the hot path at all.

So the entire problem reduces to **creating that queue pair while the guest
owns the only admin queue**. That is a one-time operation, repeated when
the guest resets the controller, and being one-time it can afford to be
careful and expensive in ways a per-write path could not.

## Three routes that do not work

Kept because two of them look plausible until worked through, and the third
is right in outline and inverted in detail.

### Borrowing the guest's admin queue and restoring it: impossible

Submit our command into the guest's admin submission queue, let the
completion land in the guest's admin completion queue, read it, and put
things back.

It cannot be put back. Our completion queue entry lands at the guest's head
slot `H`. The guest's `cq_head` and `cq_phase` are **software variables
inside its driver**; we cannot write them. Reverting the phase bit at `H`
makes the guest see "nothing new" while the controller's internal tail has
already moved to `H+1`, so its next real completion goes to a slot the
guest is not looking at. There is no way to move a controller's completion
queue tail backwards - it is not a register and it is not in memory. **The
restoration is not delicate, it is not possible.**

What *is* possible is the complement: do not put the tail back, take it all
the way round. That is the lap, below.

### Taking the controller briefly with `CC.EN` cleared: self-defeating

With `CC.EN` clear, `AQA`/`ASQ`/`ACQ` become writable, so point the admin
queue at memory of our own, create our queues, and restore the guest's
pointers.

The restoration destroys the thing it was performed for. Those three
registers are only writable while `CC.EN` is clear, and clearing `CC.EN` is
a Controller Reset, which deletes all I/O submission and completion queues
- including the ones just created. No ordering escapes this. It is dead
unconditionally, and for a reason stronger than the guest observing
`CSTS.RDY` toggle.

The only escape would be never handing the admin queue back and emulating
it for the guest instead - trapping its doorbell, proxying every command,
synthesising completions with the right phase and identifier, and injecting
its interrupts. That is full admin queue virtualisation. It works in
principle and any bug in it wedges the boot disk. Rejected on the standing
rule that simplicity and fail-proofness outrank capability.

### Reserving a queue identifier: necessary, and inverted from the intuition

This one is right in outline. The detail that matters is **which** of the
request and the completion gets edited, and the intuitive choice is the
wrong one.

Editing the guest's **request** downward also reduces what the controller
allocates, so there is no spare identifier afterwards - the guest asks for
`N-1`, the controller allocates `N-1`, and identifiers above that are
invalid for us too. It has to be the **completion**: let the controller
allocate `A`, and tell the guest `A-1`. The controller then holds `A`
identifiers valid while the guest can only reach `A-1` of them.

That it is necessary at all comes from Create I/O SQ/CQ, Dword 10, in the
Base Specification 2.0c (Figures 156 and 160): the queue identifier *"shall
not exceed the value reported in the Number of Queues feature… If the value
specified is 0h, exceeds the Number of Queues reported, or corresponds to
an identifier already in use, the controller should return an error of
Invalid Queue Identifier."* Identifiers are bounded by the allocation, so a
spare one has to be manufactured.

That it is sufficient comes from the drivers. Linux clamps in
`nvme_set_queue_count` (`drivers/nvme/host/core.c`):

```c
nr_io_queues = min(result & 0xffff, result >> 16) + 1;
*count = min(*count, nr_io_queues);
```

and physically cannot reach higher, because `db_bar_size()` only maps the
doorbell BAR as far as `nr_io_queues`. Windows' `stornvme` clamps the same
way, to `NSQA+1` and `NCQA+1`. Neither driver has ever been observed to use
an identifier above what it was told, and both number their queues
sequentially from 1 with no gaps.

But reserving an identifier creates nothing. Creating still needs two admin
commands, and those need the lap.

## The lap

Do not restore the completion queue tail. **Advance it a whole number of
laps, so that it and the phase arrive back where they started.**

The controller's rule, Base 2.0 section 3.3.1.2: it posts at slot `T` with
phase bit `P`, advances `T`, and inverts `P` each time `T` wraps to zero.
The guest's rule is the mirror. So after exactly `2 * D` postings on a
queue of `D` entries, the tail is back at its original slot **and** the
phase is back to its original value, and the guest - which has consumed
nothing - is still looking at the slot the controller is about to write.

The submission side needs only its tail returned, which is any multiple of
its own depth. So one borrow issues

    L = lcm(ASQS + 1, 2 * (ACQS + 1))

commands, of which two are the ones we wanted. The rest are filler.

**One lap is not enough, and this is the trap.** `L = D` returns the tail
and leaves the phase inverted, so the guest reads its next genuine
completion as stale and stops. It looks right and it hangs the boot disk.
`scripts/nvme-lap-model.py` asserts the property and its negation directly:
it models both sides and checks that the guest stays in sync for a thousand
subsequent completions iff `L % (2 * D) == 0`. Run it before changing any
of this.

Restoring the queue *memory* byte for byte afterwards is legal because the
controller never reads a completion queue - Base 2.0 section 3.3.1.2, the
tail *"is only used internally by the controller and is not visible to the
host"*. Restoring the submission queue's contents matters more than it
looks: we overwrite the guest's entries with ours, and although no driver
reads a submitted entry back, putting them back costs one memcpy and
removes the question.

Neither doorbell has to be remembered. After a whole number of laps the
last head and tail values we write are the guest's own, because both
counters have come round to where they were. That is fortunate, since the
submission and completion doorbells cannot be read back at all: PCIe
Transport 1.0c sections 3.1.2.1 and 3.1.2.2, *"the host should not read the
doorbell registers. If a doorbell register is read, the value returned is
vendor specific."*

### The filler

`Get Features (FID 01h, Arbitration, current value)`. Mandatory, read only,
and - the reason it is the right choice - **no data pointer and therefore
no DMA at all**, so the filler cannot fail for a reason the two real
commands would not also fail for.

### Completions that are not ours

A guest command may complete inside the window - an Asynchronous Event
Request, which both drivers keep permanently outstanding, or the
`Set Features (Power Management)` that Modern Standby issues roughly every
50 ms of idleness. The requirement generalises cleanly: **the guest must
consume exactly as many entries as the controller posted.** Keep our own
submissions at a multiple of `2 * D`, and replay each foreign entry into
the restored slot it would have occupied. The model checks this case too.

### The interrupt

Every one of our `L` completions signals the admin queue's interrupt, which
is fixed at MSI-X vector 0. A guest interrupt service routine running on
another processor would see our entries, and an unrecognised command
identifier handed to a closed-source boot disk miniport is not a risk worth
taking. **Vector 0 is masked in the controller's MSI-X table for the
duration of the borrow and unmasked afterwards**, which costs one spurious
interrupt on unmask - a rescan that finds nothing, which every driver
tolerates.

If the MSI-X table cannot be validated, the mask cannot be applied, and
then **the lap must not run**. The whole safety argument is that the guest
never sees one of our completions.

### The one irreducible risk

**A lap cannot be abandoned once begun.** Stopping halfway leaves the
guest's admin queue desynchronised and its driver will fail. Guarded by a
precondition rather than by a rollback: refuse to start unless
`CSTS.RDY` is 1, `CSTS.CFS` is 0, and `L` is within a compiled bound.

If a lap cannot complete - the controller has stopped answering - the
escape is to **clear `CC.EN` ourselves**. That destroys everything
including our own queues, but a controller reset is precisely the event
both drivers are built to recover from, so the worst case degrades into
something the guest already handles.

## An epoch

An epoch begins at each `CC.EN` 0 -> 1 and ends at the next `CC.EN` -> 0 or
`CC.SHN` != 0. Three interceptions, in order:

1. **`CC.EN` 0 -> 1.** Arm the doorbell trap, zero our completion queue,
   clear all shadows. A new epoch means our queues no longer exist.

2. **The guest's `Set Features (Number of Queues)` doorbell.** Mask vector
   0, perform the store ourselves, poll the guest's admin completion queue
   for that command identifier, **decrement both halves of Dword 0**,
   unmask, resume. Costs no extra completion queue entry and needs no lap.
   Refuse if either allocation is below 2 - we will not take the guest's
   only queue.

3. **The guest's first `Create I/O Completion Queue` doorbell.** Run the
   lap, creating our completion queue at `A_cq` and our submission queue at
   `A_sq` bound to it. Restore, unmask, perform the guest's held store,
   and **remove the doorbell trap**.

Trigger 3 is what guarantees `Set Features` has already happened, which
matters because of the second of `DIAGNOSTICS.md`'s four reasons: that
feature *"shall only be issued during initialization prior to creation of
any I/O Submission and/or Completion Queues"* and otherwise aborts with
Command Sequence Error. Creating our queues before the guest's negotiation
would break the guest's negotiation. Creating them after is free.

Note that submission and completion queue identifiers are separate spaces,
and Windows 11 26H1 requests `2 x logical processors` submission queues
against `MSI-X messages - 1` completion queues, so the two counts routinely
differ. Reserve the top of each.

### Recovery

Expect epochs to end often. Every Windows D3 -> D0 is a full
re-initialisation - `ControllerReset`, fresh `AQA`/`ASQ`/`ACQ`, fresh
`Set Features`, queues recreated from identifier 1 - and so is a bugcheck:
`dump_stornvme` resets the controller unconditionally and creates only
identifier 1 with interrupts disabled. We are always at the top identifier,
so the dump path cannot collide with us; it simply ends our stream, and
everything up to the last flush is already on the medium.

Detection has three layers and the first is not optional:

1. **A guard read immediately before every doorbell ring.** `CC` and
   `CSTS` must show `EN=1, SHN=0, RDY=1, CFS=0` and the epoch generation
   must be unchanged. This is correctness, not economy: *"writing to a
   non-existent Submission Queue Tail Doorbell has undefined results"*, and
   the reference implementation (QEMU `nvme_process_db`) responds by
   posting asynchronous event `00h`, *Write to Invalid Doorbell Register*,
   **into the guest's admin completion queue** whenever an Asynchronous
   Event Request is outstanding - which, for both drivers, is always. A
   stale ring would surface as a disk error attributed to Windows.

2. **An EPT trap on BAR0 page 0** for prompt detection. Necessary because
   Windows' D3 entry deletes its I/O queues and sets `CC.SHN` **without
   clearing `CC.EN`**, so watching `EN` alone misses it.

3. **Liveness as the backstop that cannot fail silently.** Every submitted
   write must complete within a bound; the counters say so, and the next
   block written says so in band.

## Before anything: validating the BAR

An earlier failure on the parallel xHCI work transfers here and is worse.
A controller can come out of firmware enumeration with its BAR
**unassigned** - low dword `0x00000004`, a 64 bit memory window with every
address bit clear. Sizing code that only checks which bits stick when
writing all ones accepts `base = 0`.

For NVMe that would map physical page zero into the host page table, write
"doorbells" into the real mode interrupt vector table, and **EPT-protect
physical page zero**, splitting the 2 MB region that also carries this
tree's own application processor trampoline. The machine would fail as
something else entirely.

The chain, fail-closed at every link, before any doorbell:

1. BAR0 bit 0 clear (memory, not I/O) and bits 2:1 == `10b` (64 bit).
2. `base = (BAR1 << 32) | (BAR0 & ~0xF)`. **Reject `base == 0`** and reject
   anything not 4 KB aligned. An unassigned window is not an address.
3. The PCI command register must already have Memory Space Enable set. We
   do not set it - it is the guest's device.
4. Map two pages uncacheable at a chosen virtual address, **not** identity:
   `page_table` shares one page directory across pdptes 0 to 255, so an
   identity mapping of a BAR can alias the module's own mappings.
5. **Read `CAP` at offset 0 back through the mapping and refuse on 0 or all
   ones.** A controller whose `CAP` reads zero is not there, whatever the
   BAR said. Then check what a real controller must say: `CAP.MQES >= 1`,
   `CAP.DSTRD <= 0xF`, a plausible `VS`, and PCI class `010802h`.
6. Only then compute doorbell offsets from `CAP.DSTRD` and assert they land
   inside the mapped range.

The same treatment applies to the MSI-X table BAR reached through the
capability's BIR, with the added check that vector 0's message address
looks like a local APIC MSI (`0xFEExxxxx`). No validated table means no
mask, and no mask means no lap.

## The IOMMU gate, and where it goes

VT-d translates a device's DMA regardless of which software programmed the
device, so a translating domain that does not contain our addresses stops
the channel with every register still reading healthy.

**Do not probe by trying.** DMA outside a reserved region after
`ExitBootServices` can raise bugcheck `0xE6` subcode `0x26` without Driver
Verifier being involved. Arming a channel to find out whether its DMA lands
is not a benign experiment. The question is answered by **reading state**.

The loader parses the ACPI `DMAR` table and passes the DRHD register bases
across, so no ACPI parsing lands in the resident side. Offsets from Linux
`drivers/iommu/intel/iommu.h`:

```c
#define DMAR_VER_REG    0x0
#define DMAR_CAP_REG    0x8
#define DMAR_ECAP_REG   0x10
#define DMAR_GSTS_REG   0x1c
#define DMAR_RTADDR_REG 0x20
#define DMA_GSTS_TES    (((u32)1) << 31)
#define DMA_RTADDR_SMT  (((u64)1) << 10)
#define CONTEXT_TT_MULTI_LEVEL  0
#define CONTEXT_TT_DEV_IOTLB    1
#define CONTEXT_TT_PASS_THROUGH 2
```

`context_set_translation_type` does `context->lo |= (value & 3) << 2`,
which pins the translation type to bits 3:2 with `10b` meaning
pass-through. The walk, from `iommu_context_addr` in the same tree: the
root table is indexed by bus with 16 byte entries, `root->lo` bit 0 is
present and bits 63:12 are the context table pointer, and in legacy mode
the context entry is indexed by `devfn` directly.

The verdict:

- `GSTS.TES == 0` - no translation. **Safe.**
- `RTADDR` bit 10 set - scalable mode. **Refuse.** Not a hedge:
  `iommu_context_addr` does `devfn *= 2` in that mode because scalable
  context entries are 32 bytes and the pass-through decision moves into the
  PASID table entry, so bits 3:2 no longer mean what we would be reading.
- Root or context entry not present - DMA from the device is blocked.
  **Refuse.**
- Context present with bits 3:2 == `10b` - pass-through. **Safe.**
- Anything else, notably `00b`. **Refuse.**

**The gate goes immediately before the lap, not before the first write.**
Our filler commands carry no data and would succeed under any domain; it is
the two Create commands that hand the controller our physical addresses,
and they sit inside a lap that cannot be abandoned. Check first, then
commit. It is re-run each epoch, because the guest configures remapping
during its driver's initialisation - inside our epoch, not before it.

On the development target the answer is measured, not assumed:
`stornvme`'s `DmaRemappingCompatible` is 2, which Microsoft documents as
opt-in only for external devices or under Driver Verifier, and the NVMe
controller is internal; the firmware never set `DMA_CTRL_PLATFORM_OPT_IN`
in the DMAR flags. So the controller sits in a passthrough domain. That is
a fact about one machine - a controller behind Thunderbolt or USB4 *would*
be translated, since those opt in unconditionally - which is exactly why
this is detected rather than assumed. The verdict is logged in the first
lines the channel emits.

Nothing here modifies the IOMMU. No `DMAR` unlinking, no RMRR injection, no
page table patching. `DIAGNOSTICS.md` lists those as options; this channel
takes the one that document recommends first, which is to measure whether
the problem is live.

## Where the records go

**A pre-allocated file on the EFI system partition, written in place.**

The file earns its place for exactly one reason, and it is worth stating
plainly because the reader ignores the file entirely: **the FAT marks its
clusters allocated, so nothing else will claim them.** It is an interlock,
not a container.

That is what rules out the other candidate, unused space on the partition.
Unused space is owned by nothing, so a Windows Update servicing
`bootmgfw.efi`, a `bcdedit` write, or firmware staging a capsule can
legitimately take it while we are writing - all of them entirely
compliant. A dedicated partition would also work and was the earlier
recommendation, but it means repartitioning the disk, which is a larger
intrusion than this feature is worth.

### The division of labour

**The hypervisor parses neither GPT nor FAT.** The loader resolves
`\EFI\zpp\ZPPLOG.BIN` to absolute LBAs and hands across a validated extent
table; the resident side receives nothing but numbers. That is the single
biggest simplification available here and it is worth taking for its own
sake - a FAT parser on the resident side would be several hundred lines of
attack surface running with the guest live.

Resolution: partition base from `EFI_PARTITION_INFO_PROTOCOL`, cross
checked against the `HD()` node in the device path the loader already
walks; then the FAT32 BPB, the directory entry, and the cluster chain.

- **Do not demand contiguity.** EDK2's allocator fills holes first, and
  this tree's own trace log delete-and-recreate manufactures fragmentation.
  Build an extent list, cap it, and **refuse** if the file needs more
  extents than the cap. Refusing is safe; guessing is not.
- **Create the file once and re-validate every boot.** Creation is the only
  moment the layout can change, so do it rarely.
- **Never change the file's size.** Pre-filled at creation and written only
  in place, so the directory entry, the FAT and the FSInfo are never
  touched again and the volume cannot be left inconsistent.
- Handle the **no partition table** case. `scripts/bochs/setup.sh` builds
  `esp.img` with `mformat -F` and no partition table, so under emulation
  the volume starts at LBA 0 and there is no `HD()` node. That must resolve
  and succeed rather than fail by accident.

### Closure on the parse

Believing a FAT parser is not the same as having checked it. Two
independent closures, and neither is optional:

1. **In the loader, at resolution time.** Write a pattern through the file
   protocol, then read it back through Block IO at the LBA the extent table
   computes. If FAT was misparsed the bytes differ and the feature never
   arms.

2. **In the hypervisor, on every single write.** Read the target block
   first and require a signature the loader placed there this boot - magic,
   the disk GUID, the partition GUID, the file's identifier, and the
   block's own index within the file. That turns "we parsed FAT correctly"
   from a belief into a per-write measurement, and it is what makes writing
   next to the Windows boot manager acceptable at all. Combined with a
   bounded `logical block -> LBA` lookup over the handed-in table, **the
   hypervisor cannot express an out of range address.**

### Reading it back

`dd` the partition and scan for the block magic. **The reader does not
parse FAT and does not need the file to be a valid file.** Every block is
independently self describing and self locating - magic, boot identifier,
epoch, monotonic sequence, record count, the diagnostic cursor's `lost` and
`refused` counters, our own drop counters, and a checksum - so a block
found in isolation can be placed in order with reference to nothing else.
A torn block fails its checksum and is skipped; a gap in sequence numbers
is reported rather than hidden.

This also buys a third, independent cross-check on the extent resolution,
and it is worth using deliberately: **the reader discovers where blocks
actually landed, the loader traced where it intended them to land, and the
two can be diffed.** Under emulation that closes entirely offline - run,
then search `esp.img` from the host for the magic and compare against the
loader's trace.

## What has to be decoded

Deliberately split, because the two trapped pages need very different
amounts of machinery.

**BAR0 page 0 (`CC`, `CSTS`, `AQA`, `ASQ`, `ACQ`): no decoder at all.**
Mark the page read only in EPT; on a write violation make it writable, set
the Monitor Trap Flag, single step, re-protect, and **read the register
back**. Registers are readable, so the value needs no instruction
knowledge. Two exits per register write, and register writes are rare.

**BAR0 page 1 (the doorbells): two opcodes.** Doorbells are not readable,
so the value has to come from the instruction. Exactly:

- `MOV r/m32, r32` - opcode `89 /r`
- `MOV r/m32, imm32` - opcode `C7 /0 id`

each with an optional REX prefix. That is all `writel()` and
`StorPortWriteRegisterUlong` ever emit. The effective address is **not**
decoded - the VMCS supplies the guest physical address of the access - and
the length is **not** computed - the VMCS supplies
`vm_exit_instruction_length`. Instruction bytes come from guest RIP through
the existing `os_page_table(guest_cr3, physical_to_virtual)`.

Anything else: fall back to single stepping, record a missed doorbell, and
if it was a trigger, bring no channel up this epoch and say so loudly.

**The doorbell trap is armed only between `CC.EN` 0 -> 1 and our queues
existing, then removed**, so the steady state costs no exits at all. That
matters because `CAP.DSTRD` is 0 on real hardware, which puts every I/O
doorbell in the same 4 KB page as the admin ones - a permanent trap there
would take an exit per I/O submission.

## Rate and cost

Queues and buffers live **inside the module**, so `protect_module` already
denies the guest every access. That is essential rather than convenient:
our submission queue holds raw commands with logical block addresses, and a
guest write into it would make the controller execute them. EPT does not
affect DMA, so the controller still reads them.

Everything is a single page, so there are **no PRP lists anywhere**.

- Records are 128 bytes, so **32 to a 4 KB block**. Flush when the block
  fills or after a few milliseconds, whichever comes first.
- Write with **Force Unit Access**, so completion means non-volatile.
  Without it, "survives a wedged machine" is not true.
- Never wait. Completions are reaped on the next pump; a full submission
  queue is backpressure into our staging ring, counted and reported in
  band, never a stall.
- **Steady state cost per VM exit is a 128 byte copy and a counter.** On
  the one exit in 32 that submits, add a few stores, a fence, one guard
  read and one posted doorbell write.
- **The lap is the one expensive thing**: 512 admin commands against a
  Windows admin queue, once per controller reset, in a single VM exit.

Unlike the xHCI Debug Capability, NVMe needs a doorbell per command, so a
*wedged hypervisor* strands whatever has not been submitted. Ringing on
every block rather than batching bounds that loss to the current partial
block. A wedged *guest* is fine, which is the case being chased.

## What can be tested where, and what cannot

**Neither emulator can run the whole thing, and the split is unlucky.**

- **QEMU models NVMe fully** (`-device nvme`) but its TCG has no VT-x, so
  the hypervisor cannot launch. Everything up to VMX is testable: BAR
  validation, register discovery, the lap, the byte for byte restore, queue
  creation at a reserved identifier, a write landing at a known LBA, and
  whether the firmware's own NVMe driver survives.
- **Bochs emulates VMX** - it is why this project uses it - **but models no
  NVMe at all.** Not "incompletely": there is no device.

So the EPT doorbell trap, the Monitor Trap Flag single step, the decoder,
and the `Set Features` completion patch that depends on the trap cannot be
exercised under either. They are first exercised on hardware, which is
where the risk concentrates and where it should be stated rather than
discovered.

## When each command may be issued, which is not obvious

The ordering rule below is the one thing in this design that turns a working
channel into an unbootable guest, and it is easy to get wrong because the
constraint applies to only one of the three commands involved.

NVMe Base 5.2.30.1.5, verbatim:

> "The host should only submit a Set Features command for this Feature during
> initialization **prior to creation of any I/O Submission and/or I/O
> Completion Queues**. If a Set Features command is submitted for this Feature
> **after creation of any I/O Submission and/or I/O Completion Queues, then
> the controller shall abort that Set Features command with status code of
> Command Sequence Error**."

and:

> "After a Controller Level Reset (CLR), the **first** Set Features command
> for this Feature that the controller completes successfully shall allocate
> I/O queues... After that first successful Set Features command, the number
> of I/O queues allocated **shall not change until a CLR occurs**."

### What that forbids, and what it does not

It constrains **Set Features** only. Creating an I/O queue is not ordered
against anything - a Create I/O Completion Queue or Create I/O Submission
Queue may be issued at any point after the allocation exists.

So the sequence that breaks the guest is: reserve queues *and create them*
in one borrow, hand back, and let the guest's own Set Features arrive after
our queues exist. It is aborted with Command Sequence Error, and Linux turns
that into zero I/O queues and no block device - `nvme_set_queue_count` sets
`*count = 0` on any error status and `nvme_setup_io_queues` then returns
early. An unbootable root, from a diagnostic facility.

### The order to use

1. The guest resets the controller when it takes over, which is a CLR: every
   I/O queue is deleted and the allocation is cleared. **This is why the
   `CC.EN` 0 to 1 edge is the right place** - at that moment no I/O queue
   exists, the admin queue is empty because the controller could not have
   accepted a submission, and the processor that would make the first one is
   the one held inside the exit.
2. Borrow once there, and issue **Set Features (Number of Queues) alone**,
   asking for the maximum. Create nothing. The allocation is now frozen until
   the next reset.
3. The guest's own Set Features arrives later with still no I/O queues
   created, so it completes successfully and reports the allocation we
   reserved - which is at least what it asked for. Linux takes
   `min(requested, allocated)` and is content.
4. The guest creates its queues, contiguously from 1.
5. **Our own queues are created after that**, at an identifier above the
   guest's usage and inside the allocation. Nothing forbids this ordering;
   only step 2 was ever constrained.

### What is still open, stated precisely

Step 5 needs a second borrow, and unlike step 2 it happens while the guest is
live. That is the quiescence problem, and it is the *whole* of what remains -
it is not a Set Features problem, which is what it looked like at first.

The identifier also has to be chosen rather than assumed. Drivers allocate
contiguously from 1, so taking a low one collides and the controller answers
Invalid Queue Identifier. Take the highest inside the allocation.

### The order, as built

Written and switched off, in two switches rather than one:
`diag::rebuild_channel_after_reset` for steps 1 to 3 and
`diag::create_channel_queue_after_guest` for steps 4 and 5. The split is a
control experiment, not tidiness - with only the first on, every part of the
borrow happens at the same moment it did when the guest boot looped and
nothing is created, so a boot that survives clears the borrow itself and a
boot that does not indicts it.

`hypervisor::reserve_channel_queue_allocation` runs at the CC.EN 0 to 1 edge,
arms the doorbell page in holding mode, and borrows once for a single
`Set Features (Number of Queues)` asking for the maximum - CDW11 = `ffffffffh`,
both halves being zero's based. `admin_borrow::run` now reports each payload
command's DW0 as well as its status, because for this command DW0 *is* the
answer: the status only says the feature was accepted.

`hypervisor::observe_guest_admin_submissions` then reads every admin command
the guest submits off its own submission queue, keyed on the doorbell watch,
and keeps four numbers: the Number of Queues it asked for, the highest Create
I/O SQ and Create I/O CQ identifier it has issued, and how many of each it has
created. **No completion is patched.** The earlier section of this document
describing a patch scheme is obsolete under this ordering and building one
would be a mistake that has already been made once.

`hypervisor::create_channel_queue` fires when the guest has created at least as
many queues as it asked for in both spaces, and takes
`max(highest created, requested) + 1` in each - the requested count being a
margin against a driver that creates its set in an order this has not seen,
and being itself an observation of the guest rather than of the controller. If
that is outside the allocation nothing is created at all, which is the outcome
that lets the guest boot. On success the doorbell watch is removed, so the
steady state costs no exits.

What stands in for quiescence, all of it necessary and none of it sufficient
on its own: the creating processor is the one held inside the exit for the
guest's own doorbell write, so the thread driving storage initialisation
cannot submit; the doorbell page is held for the borrow, after an
acknowledgement wait that now probes with a wake NMI - usable again since a
non-maskable interrupt taken in root mode returns instead of halting the
processor that took it; and the borrow does not start until SQHD equals the
tail the guest last published, which is what proves the controller has fetched
every entry the borrow is about to overwrite.

**What is not solved, and is the thing to watch on the first hardware run:**
the admin queue's interrupt is not masked. This document says MSI-X vector 0
must be masked for the duration and there is no MSI-X table access on the
resident side to do it with. The argument standing in its place is that the
only processor which can take that interrupt is the one held inside the exit
with interrupts disabled, so it stays pending in the local APIC and is
delivered after the restore, where it finds the guest's own completions and
nothing else. That is an argument about *when* this runs rather than a
property of what it does, and it is the first thing to revisit if the guest
misbehaves after the create rather than during it.

### A correction worth keeping

The lap length was reported as wrong during review, on the grounds that it
assumed `2 * depth` while the submission and completion depths are
independent fields. It does not: `admin_borrow::length` computes
`lcm(s, 2 * c)` with Euclid, which is the right answer and already handles
unequal depths. The criticism was of code that was not there.

## What is built, and what is only written down

Honest inventory, because the gap matters more than the code here.

**Built, compiled by the freestanding toolchain, and linked:**
`zpp/nvme/registers.h`, `command.h`, `log_format.h`, `iommu_gate.h`,
`admin_borrow.h` (the lap), `log_writer.h` (the private queue pair, the
guard read and the signature check), and `zpp/diag/sinks/esp_blocks.h`.
`hypervisor/src/diag/instantiate.cpp` exists only to force the templates
to be instantiated, because nothing calls the pump yet and an
uninstantiated template is parsed but not checked.

**Verified:** the lap arithmetic, by `scripts/nvme-lap-model.py`, over
every admin queue depth a real driver uses and with foreign completions;
`log_target::lba_of` refusing an out of range index, as a `static_assert`;
the release binary being byte for byte identical with the facility
compiled in and out.

**Written down but not built:** the EPT violation handler, the Monitor
Trap Flag single step, the two opcode decoder, the loader's FAT
resolution, and the hand-over of a `log_target` to the resident side.
Nothing is wired into the VM exit path, so `esp_blocks_for::ready()`
answers false and the channel is inert. The wiring points are named in
that header.

**Never executed against a device, real or emulated.** The lap has been
executed only against the model. That is the single largest gap and it
should be closed under QEMU - which models NVMe fully - before anything
is attempted on hardware.

## Measured: the queue identifier, and why a higher one is not the answer

The ordering hazard written down above stopped being theoretical. Both
halves of it were run on the rig, with `rebuild_channel_after_reset` on and
the guest's `CC` writes emulated rather than stepped.

| Channel's I/O queue identifier | Create I/O SQ/CQ result | Guest |
|---|---|---|
| 4 | accepted, status 0 | boot loop, `INACCESSIBLE_BOOT_DEVICE` |
| 32 | refused, status `0x4101` | boots to the login screen |

`0x4101` decodes as DNR set, status code type 1 (command specific) and
status code 01h, which is **Invalid Queue Identifier** - the answer Figures
156 and 160 give for an identifier that exceeds the Number of Queues
allocation. There is no allocation at that moment: the rebuild runs at the
`CC.EN` edge, before the guest's own Set Features, and 5.2.30.1.5 says the
allocation is cleared by a controller level reset and established by the
first Set Features completed after it.

So the two outcomes are the two failure modes of the same defect, and
neither is a channel:

- An identifier the guest will want is accepted by this controller even
  with no allocation in force, and then collides. The guest's own create
  for that identifier is refused, its storage initialisation fails, and it
  bugchecks and resets - which re-enters the rebuild, which recreates the
  queue, which is why it never converges.
- An identifier the guest will not want is out of range and refused, so
  there is no private queue at all. The guest boots precisely because
  nothing was created behind its back.

The second run is the useful control: it isolates queue creation as the
thing that breaks the guest, since everything else about the borrow ran
identically and the admin queue still came back byte for byte.

What this rules out: raising the identifier, on its own, in any form. What
it leaves is the sequence this file already prescribes - reserve by
patching the guest's Set Features completion, and create ours only after
the guest has created its own.

## What the guest's driver actually does, observed

Read off the guest's own admin submission queue during a Windows boot on
the rig, with `diag::observe_controller_admin` on. Every number that the
reservation design rested on a guess about is here.

**Set Features (Number of Queues)** is issued once, `value = 0x0007000f`.
Both fields are zero's based, so that is **sixteen submission queues and
eight completion queues requested**, on an eight processor machine.

**It then creates exactly what it asked for**, and nothing else:

    Create I/O CQ   QID 1..8    queue size 1024
    Create I/O SQ   QID 1..16   queue size 1024

with each submission queue's DW11 naming its completion queue, so
submission queues nine to sixteen pair back onto completion queues one to
eight. Two submission queues per completion queue, one pair per processor.

Three consequences, and the first two close open questions:

- **The identifier the channel has to reserve is above both ranges**:
  submission seventeen and completion nine at the least. Four, which the
  loader hardcoded, is inside both - chosen when the only other user of
  this controller was firmware that created one queue and numbered it 1.
- **Thirty two was refused for the reason the specification gives.**
  The controller allocated what the guest asked for, so thirty two exceeds
  the allocation in both spaces and Create returns Invalid Queue
  Identifier, `0x4101`.
- **This driver clamps its creates to its own request**, which is the
  behaviour the reservation scheme needs but only under an allocation
  equal to that request. It does not yet show what it would do with an
  allocation *larger* than it asked for, which is what the corrected
  ordering produces. Until that is observed, choose the identifier from
  what the guest is seen to create rather than from what it was told.

**The configuration register is written three times in a boot** -
`0x00460000`, `0x00460000`, `0x00460001` - which is the documented
disable-then-enable pair and nothing else. **No write ever set the
shutdown notification.** So a mechanism that triggers on the enable bit
fires about once per boot and never in steady state, which is the
measurement that decides what the excursion can be: a flush at reset, not
a channel.

**Not settled, and it looked settled:** the doorbell page watch counted
161 writes across the whole boot. A guest reading a Windows installation
submits far more than that, and its I/O doorbells are on this page - with
a stride of zero every queue's doorbell is within 0x80 of the admin one.
So the watch is not catching them, and the cost of a permanently trapped
doorbell page is **not** measured by this run. What is measured is that
the admin doorbell traps reliably and that a guest boots normally with the
page watched. The gap is worth understanding before that cost is quoted.

## The reservation works, and the controller has no spare submission queue

Measured on the rig, with the reservation on and creation off.

**Asking for the maximum has to ask for FFFEh, not FFFFh.** A Set Features
(Number of Queues) with CDW11 = FFFFFFFFh came back `0x4002` - do-not-retry
set, status code type 0, status code 02h, Invalid Field in Command. Both
halves are zero's based, so FFFFh asks for 65536 and is the reserved
encoding; FFFEh asks for 65535 and is accepted.

**With that fixed the reservation is granted, and the grant is the
problem.** DW0 comes back `0x000f000f`: NSQA and NCQA both 15, zero's
based, so the controller's maximum is **sixteen submission queues and
sixteen completion queues**. That is everything it has - it is what
answering "give me all of them" returns.

Against what the guest was already observed to do:

    space        controller max   guest creates   spare
    completion   16               1..8            9..16
    submission   16               1..16           none

The guest clamps to its own request rather than to the allocation - it
asked for sixteen submission queues and created sixteen - so reserving a
larger allocation does not hold anything back from it. There is room for
our completion queue and none at all for our submission queue.

**So the completion patch is necessary after all**, and the earlier
reasoning that retired it was right about the ordering and wrong about the
arithmetic. The ordering argument still holds: our Set Features is the
first after the reset, the guest's is the second, and the allocation
cannot change between them, so the guest is told what we reserved. What
that argument assumed is that the reservation could exceed what the guest
wants. On this controller it cannot, because the guest wants all of it.

What has to happen instead is to reduce what the guest *believes* it was
granted: report NSQA fifteen rather than sixteen in the completion it
reads, so it creates submission queues one to fifteen and leaves sixteen.
The completion is a DMA write by the controller and there is no exit for
it, so this needs one of the interception shapes NVME-LOG already
describes - and the cheapest is the one that substitutes our own command
into the guest's submission slot while the doorbell is held, since the
controller then posts a completion we asked for and we rewrite it in place
before the guest is resumed.

Worth noting for whoever builds it: only the submission half needs
reducing. The completion half already has eight spare identifiers.

## Reducing the grant, as built

`diag::reduce_guest_queue_grant`, off by default, and it needs
`rebuild_channel_after_reset` - a `static_assert` says so, because without
the reservation there is no allocation to hold anything back from and
reducing the answer would take a queue away from the guest and leave
nothing valid above it.

`hypervisor::patch_guest_queue_grant` runs from the guest's own admin
doorbell exit, after `observe_guest_admin_submissions` has read the
command off the guest's submission queue and taken its command identifier
from CDW0 bits 31:16. It maps the guest's admin completion queue through
the window, finds the completion for that identifier, and **rewrites four
bytes of DW0 in place**. That is the whole of it.

Nothing is submitted, nothing is injected, and no doorbell is rung. The
completion is the guest's own, posted by the controller for a command the
guest itself issued, landing at the guest's own slot with the guest's own
phase - so the controller's internal tail is exactly where it would have
been, and none of the lap machinery is involved. Restoring is legal for
the same reason the lap's restore is: Base 2.0 3.3.1.2, a controller never
reads a completion queue.

### Why not substitution, which this document favoured

The shape recommended above - put a command of ours into the guest's
submission slot while the doorbell is held, then rewrite the completion's
command identifier *and* DW0 - was tried on paper and rejected for two
reasons, both worth keeping so it is not re-proposed:

- **The doorbell has already rung.** `page_watch::handler` is documented
  as "called after the write has taken effect", and `on_ept_violation`
  applies the decoded store before calling the handler. So there is no
  point at which our code runs with the guest's doorbell write pending,
  and the premise that "the controller has not yet fetched the entry" is
  false as the machinery stands. Making it true means deferring a store
  the watch currently applies, which is new machinery on the one path
  that must not be wrong.
- **A foreign command identifier is the worse exposure.** Substituting
  puts a completion carrying *our* identifier into the guest's admin
  queue for a window. Leaving the guest's own command in place puts a
  completion carrying the guest's identifier there, with the controller's
  own status, differing from the truth only in DW0. Under the unmasked
  admin interrupt below, those two failure modes are not comparable.

### The race that is real, and how it is handled

Because the doorbell rings before the handler runs, the controller's
completion can arrive **before** this code looks for it. A Set Features is
two DMA round trips; reading the guest's submission entry through the
window is a comparable number of microseconds. Either can win, so both
are handled: the queue's already-posted region is searched newest first
over `[0, tail)` before anything is waited for, and only then is the tail
polled. `channel_grant_already_posted` records which side of the race the
run was on, which is a measurement nobody has yet.

Newest-first is what makes matching on a command identifier safe against a
driver that reuses one earlier in the epoch. On top of that the entry's
DW0 must equal the allocation our own reservation was granted - which
5.2.30.1.5 guarantees, since ours is the first Set Features after the
reset and the allocation cannot change until the next one. That check is
both the discriminator and a test of the ordering argument: a
disagreement is recorded as `0xe8` and nothing is edited.

### The consequence nobody would guess

`create_channel_queue` had to change as well, and without that change the
reduction buys nothing at all. Its identifier was
`max(highest created, requested) + 1`, where `requested` is the guest's
own request read off its submission queue - a margin against a driver
creating its set in an unexpected order. With the grant reduced the guest
asks for sixteen, is told fifteen, and creates fifteen; a margin of
sixteen still chooses seventeen, which is outside the allocation and
refused with Invalid Queue Identifier. So the request is now clamped by
what the guest was *told* - `hypervisor::queue_limit` - and the same
clamp decides when the guest has finished creating, since waiting for its
full request to be met would wait for ever.

### The admin interrupt, again, and why it matters less here

Vector 0 is still not masked and there is still no MSI-X table access on
the resident side. What is different is the cost of losing that race
under *this* mechanism. A guest interrupt service routine on another
processor that read the completion before it was edited would find the
guest's own entry, with the guest's own command identifier and the
controller's own status, saying sixteen rather than fifteen. The guest
then creates sixteen submission queues, `create_channel_queue` finds no
room and refuses, and the guest boots. **The failure mode is no channel,
not a confused driver** - the same outcome as the switch being off.

That is not an argument that masking is unnecessary for the lap in
`create_channel_queue_after_guest`, where a completion carrying one of our
own identifiers really can reach the guest's driver. It is only an
argument that the reduction does not add to that exposure.

If it is to be built, the cheapest form is probably the **function mask**
in the device's MSI-X Message Control register, which is in PCI
configuration space rather than in the table BAR, so it needs no second
BAR to be sized, mapped and validated - only the controller's bus, device
and function handed across by the loader, and a configuration space
access mechanism the resident side does not currently have. The bit's
position and the semantics of masking while a vector is pending are
stated from memory here and have not been checked against the PCIe
specification, so check them before writing any of it.

### What the two halves do, and what is measured

Reduction is applied per space, and only where the guest's own request
leaves nothing above it. On this machine that is the submission half
alone: sixteen requested against sixteen granted, so NSQA is shown as
fifteen, while eight completion queues requested against sixteen granted
already leaves nine to sixteen free and NCQA is passed through exactly as
the controller wrote it.

**Never run on hardware.** Neither emulator can reach it, for the reasons
in the split above: the doorbell trap needs VT-x, which Bochs has and QEMU
does not, and the completion needs a modelled NVMe controller, which QEMU
has and Bochs does not.
