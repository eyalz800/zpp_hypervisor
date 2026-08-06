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
