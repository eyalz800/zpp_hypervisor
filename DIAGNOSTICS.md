# Getting records off the machine

The hypervisor's problem is that it has no channel. There is no console, the
serial port belongs to the guest, a debugger sees guest state only, and a
bare-metal failure that freezes the machine takes its log with it. Nested
virtualization is expected to produce many bugs, some of them reproducible
only on bare metal, so this stopped being a convenience.

What follows is what was established about each possible channel, so that
nobody re-derives it. Where a claim was measured it says so; where it comes
from a specification the section is named; and the things that turned out to
be **wrong** are kept, because two of them were believed for a while.

## The one thing that decides everything

**Does the last thing written get moved by hardware, with no further
involvement from the processor?** A freeze has three shapes and they need
different answers:

1. The guest is dead and the hypervisor still runs. Almost anything works.
2. The hypervisor is wedged - spinning in root mode, or deadlocked. Only a
   channel whose data was already handed to a device survives.
3. The processor retires nothing at all. Same as 2, and we cannot even
   trigger a reset.

Two families pass the test: a bus-master DMA engine with a descriptor
already queued, and the display scanout engine, which re-reads the
framebuffer forever with no processor involvement whatsoever.

## Measured on the development target

An i7-8565U laptop, Whiskey Lake-U, 300-series PCH. Read from Linux on the
machine itself, so these are observations rather than assumptions.

- **No Ethernet controller and no serial port.** `eth0` is a USB dongle, a
  Realtek `r8152` behind the xHCI at `00:14.0`. The Intel LPSS UART at
  `00:1e.0` exists and its pins are almost certainly unrouted.
- **VT-d is enabled in firmware**, and the DMAR table is the firmware's own:
  `ACPI: DMAR ... INTEL EDK2`, `DMAR: IOMMU enabled`, host address width 39,
  a DRHD at `fed91000` with `INCLUDE_PCI_ALL`, and the xHCI in IOMMU group 5
  under `dmar1`. **Two RMRRs already exist**, so this platform demonstrably
  honours them.
- **Hibernation is disabled** - no `hiberfil.sys`, while `pagefile.sys` and
  `swapfile.sys` are both present. No boot of this machine has ever been a
  resume.
- TinyCore's kernel has no `CONFIG_USB_XHCI_DBGCAP`, so the free
  Linux-as-debug-target experiment needs a different live distro.

## Memory that survives a reset: dead, and it fails silently

This was tried and reverted. It fails for three independent reasons, and it
is worth keeping all three because each alone is fatal.

1. **The machine has to be hard powered off** to escape a freeze, which
   drops DRAM.
2. **Intel TME re-keys memory at every boot** from an on-die RNG. Memory
   written before a reset decrypts under a different key afterwards, so the
   ring comes back as noise that reads as *no records* rather than as an
   error.
3. **The TCG Platform Reset Attack Mitigation specification** requires
   firmware, on booting "following a reset without OS shutdown or an
   incomplete shutdown", to overwrite all of system memory. That is exactly
   the situation we would be creating.

Every working reserved-DRAM console - coreboot's CBMEM, ChromeOS ramoops -
depends on *being* the firmware that reserves and preserves the region. We
are not.

## ACPI ERST and the rest of APEI: absent on laptops

Counted across 785 real `acpidump` captures rather than argued:

| Table | Notebooks (435) | Servers (20) |
|---|---|---|
| ERST | **3 (0.7%)** | 13 (65%) |
| BERT | 3 | 13 |
| HEST | 4 | 13 |

The three notebooks are Dell Precision and Latitude business SKUs. APEI is a
server feature. It is also contested - Windows' WHEA uses the same store,
with no ownership arbitration and a stateful four-step protocol.

From the same corpus, incidentally: **207 of 215 laptops that declare a
debug port at all declare a 16550, usually at `SystemIO 0x3F8`** - on
machines with no visible serial port. A blind `outb` there is ten lines and
occasionally a windfall. It is never a channel to count on.

## Writing to disk while the guest is alive: impossible

Settled from the NVM Express Base Specification 2.0e. Four reasons, any one
of them sufficient:

- `CC.EN` 1→0 **deletes all I/O submission and completion queues**, and an
  operating system driver resets the controller unconditionally when it
  takes over.
- `Set Features (Number of Queues)` "shall only be issued during
  initialization prior to creation of any I/O Submission and/or Completion
  Queues", and otherwise "shall abort with status code of Command Sequence
  Error". Queues of ours surviving into the guest's initialization would
  therefore break the guest.
- Creating a queue is an *admin* command, and `AQA`/`ASQ`/`ACQ` "are only
  allowed to be modified when this bit is cleared to 0". While the guest
  runs, the only admin queue in existence is the guest's.
- The guest resets the controller repeatedly in normal operation - on any
  command timeout, and on every D3 entry and exit, which with modern standby
  happens many times an hour.

The submission queue tail doorbell is also an absolute value that "shall
overwrite any previous Submission Queue Tail entry pointer value provided",
and "the host should not read the doorbell registers". Two writers cannot
compose.

**Death-time writing is viable, and is what everyone does.** Windows' crash
dump path loads a second, `dump_`-prefixed copy of the miniport and resets
the controller out from under the live driver. Linux's `pstore/blk` rules
say it outright: "Reset your block device and controller if necessary."
kdump boots a second kernel with `reset_devices`. None of them attempts
coexistence, and the specification explains why.

## The xHCI Debug Capability: the one designed for this

The specification puts the answer in writing. §7.6.2: the capability "is
able to function completely independently of the xHCI interface used by a
system host controller driver", and "can be initialized before or after the
system host controller driver loads". §7.6: the claimed root port "appears
through the xHCI as a fully functional Root Hub port that never sees a
device attach", reports no available bandwidth, and generates no attach
events. So the guest sees an empty port - the most ordinary state a port can
be in - and **no interception, no EPT work and no instruction decoder are
needed.**

The port is auto-assigned only when the *debug cable* is attached, not by an
ordinary device, so enabling the capability before the guest boots steals
nothing from it.

What survives a wedged processor, and this is the point: §4.9.2, the
controller walks the transfer ring on its own and consumes every descriptor
whose cycle bit matches. Per record the processor does four things - copy
the bytes, store one descriptor, fence, ring one doorbell. There is no
interrupter at all.

**Two conditions, and no reference implementation gets the second right.**
Ring the doorbell on every record, so a freeze cannot strand a batch. And
size the **event ring larger than the transfer ring**: §4.9.4 has a full
event ring stop the controller processing the transfer ring, and with
completion interrupts requested on every descriptor a wedged processor never
advances the dequeue pointer. A wedged processor cannot add descriptors, so
making the event ring bigger renders the condition unreachable by
arithmetic rather than by code.

What can still knock it down: a host controller reset, guest writes to the
port's status register, a cleared bus-master bit, and D3. The answer to all
of them is to poll the control register and re-arm - which is what a
production hypervisor ships, on a 100 µs timer, with the documented cost
that a reset "may cause small portion of the console output to be lost".
`DCST.SBR` is a read-only bit saying whether a host controller reset even
affects the capability on a given part.

Windows' own USB3 kernel debugging is this capability, under a running
Windows, on a controller Windows drives. Its mechanism for not fighting it
turns out to be no mechanism at all.

**No emulator models it.** QEMU's extended capability chain has exactly two
Supported Protocol entries and terminates; ID `0x0A` is absent. Bochs is
worse than absent - it declares the capability in a static table with the
comment that it is "not needed or used by this emulation" and implements
nothing, so enabling it reads back as enabled from plain memory while the
run bit never sets. **A bring-up that waits unboundedly for that bit hangs
Bochs at boot and looks like our own defect.** Bounded polls with a reported
verdict are therefore a correctness requirement, not a style preference.

## The display: the only channel an IOMMU cannot touch

The scanout engine re-reads the framebuffer at refresh rate with no
processor involvement, so a machine frozen in any of the three shapes above
still shows whatever was last drawn.

Linux merged a QR-code panic screen in 6.12 and Fedora ships it, with a
decoder, and deliberately implemented only the subset of the QR
specification it needed. That is the precedent for making this channel
machine-readable rather than photographable.

**The open question, and it is one experiment:** `drm_panic` needs each
driver to implement `get_scanout_buffer`, because only the driver knows
which surface is currently being scanned out. That predicts that writing the
pre-boot framebuffer may silently do nothing once the guest's display driver
has taken over. Boot the guest, paint the framebuffer from a VM exit, and
see whether the panel changes. Until that is run, this channel is proven
only before the guest starts.

## The IOMMU problem, which is common to every DMA channel

VT-d translates a *device's* DMA regardless of which software programmed the
device. So a translating domain that does not contain our ring addresses
kills the Debug Capability, an Ethernet dongle and an NVMe write
identically, with every register still reading healthy. Microsoft documents
that the inbox xHCI, AHCI and NVMe drivers support DMA remapping and may opt
in when VT-d is available even with Kernel DMA Protection turned off.

The display channel is the exception, because we write it with processor
stores and the display engine reads it. No DMA of ours is involved.

Options, cheapest first, none of them yet settled:

1. **Unlink `DMAR` from the XSDT** in the loader before chainloading. The
   guest sees no VT-d. Crude, and it removes the guest's DMA protection
   wholesale.
2. **Inject an RMRR** covering our pages, scoped to the device. This uses
   the architecture's own mechanism, and USB controllers are its most
   traditional consumer. A *declared* hole: the guest knows about it.
3. **Patch the guest's IOMMU page tables** from the hypervisor - walk
   `RTADDR_REG` to the root and context tables, add identity mappings,
   invalidate, and re-apply when the guest rebuilds them. No ACPI surgery
   and no decoder, but an *undeclared* hole: the guest keeps reporting
   protection it no longer entirely has.
4. Own the IOMMU and hide it from the guest.
5. Virtualize it and shadow the guest's tables. Almost certainly
   disproportionate for a log channel.

**Before any of that, measure whether the problem is live.** DMA remapping
is opt-in per driver. If the guest leaves the controller in passthrough,
none of this is needed.

Whatever is chosen must be debug-only and **loud**. Degrading a machine's
DMA protection to chase a hang is fine; doing it silently on a build
believed to be clean is not.

## What is built

`diag/include/zpp/diag/` is the facility every channel plugs into: one
`constexpr` table controlling every sink by severity and category, a
per-processor ring, and sinks as *readers with private cursors* rather than
callees - which is what makes "send everything stored, then continue live"
a cursor initialised to the oldest record rather than a mechanism. The write
path takes no lock, allocates nothing and cannot fail; a full ring
overwrites its oldest and the loss is derivable from two counters.

Release is proven unaffected: two release builds, one with the facility
requested and one without, are byte-for-byte identical.

Draining is pumped from the VM exit handler, bounded; from the halt paths,
unbounded; and from the loader. There is no thread and no timer to do it,
and no prior art was found for that arrangement - Zephyr requires a thread,
Linux's `printk` drains from `console_unlock()`. It is the part most worth
reviewing.

Prior art worth reading before changing any of it: Linux's `printk`
validates the retention model point for point - a per-console cursor, a
late-registering console rewound to the oldest record, and loss reported to
the reader in its own output. `defmt` and `pw_tokenizer` validate the
`consteval` event identifier that keeps format strings out of the binary.
Project Mu's Advanced Logger is the closest whole-system analogue: a memory
buffer, a tiny variable holding only its *address*, a flush to a file, and a
decoder.
