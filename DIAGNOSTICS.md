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

## The requirement, stated so that a channel can be judged against it

A channel is only a channel here if it satisfies all three:

1. **A program can read it.** Not a person looking at a screen, not a
   photograph. The whole point is that the records are analysed, and at
   nested-virtualization volume a human in the loop is no loop at all.
2. **It works while the guest is completely stuck.** That is the failure
   being chased. Note this is not the same as the *hypervisor* being stuck:
   a wedged guest still takes VM exits, so our code still runs and can still
   drive a device. Only a wedged hypervisor needs the stronger property of
   data already handed to hardware.
3. **It cannot fail silently.** A channel that stops working and reports
   nothing is worse than no channel, because it is trusted.

That third one is what removed the display, below.

## The display: removed, and why

Built, verified under emulation, merged, and then **reverted**. Kept here so
nobody re-adds it without answering the question that killed it.

The attraction was real: the scanout engine re-reads the framebuffer with no
processor involvement, so a frozen machine keeps displaying the last thing
drawn, and it is the only candidate an IOMMU cannot touch, because we write
it with processor stores rather than DMA. Linux merged a QR-code panic
screen in 6.12, so machine-readability had a precedent.

It fails requirement 1 outright - no program of ours can read a panel - and
it probably fails requirement 3. `drm_panic` needs every driver to implement
`get_scanout_buffer`, because only the driver knows which surface is
currently being scanned out. That predicts that once the guest's display
driver takes over, our writes to the pre-boot framebuffer go into memory
nobody reads, and produce no error while doing it. There is a window where
it demonstrably works - our bring-up, the boot manager, early kernel, which
is where the recovery-screen failure lived - but that window closes exactly
where the nested-virtualization work begins.

The experiment that would settle it, if it is ever worth a boot: paint the
framebuffer from a VM exit well after the guest has booted, and see whether
the panel changes. Until someone runs it, this is a channel that might
silently do nothing, which is the one thing this project will not ship.

## Wanted later: breaking in on a wedged guest

Not now, but do not design it away, because exactly one candidate can
support it.

The want is to interrupt out of a stuck guest on a signal from outside -
press something on the development machine and take control, rather than
watching a frozen screen and power cycling. Two pieces are needed.

**Guaranteed control, which is the VMX preemption timer.** A wedged guest
may take no VM exits at all: a spin loop with interrupts disabled yields
nothing, so "the hypervisor is still alive" is an assumption rather than a
property. The preemption timer counts down in VMX non-root operation and
forces an exit regardless of what the guest is doing. One pin-based control
and one VMCS field per entry buys a guaranteed periodic foothold in a guest
that has stopped cooperating. Worth building for its own sake, since it also
underwrites every other channel's claim to work during a freeze.

**A way in, and only the Debug Capability has one.** It has two bulk
endpoints, IN as well as OUT; every other channel considered here is write
only. So the same cable that carries the log out is a path for the host to
send a byte in, polled from the timer-forced exit. The port already builds
the IN endpoint context, because the specification requires both - it simply
never queues a transfer on it. Adding the read path is small; the protocol
on top of it is the real work, and none of it needs deciding yet.

This is the one capability that distinguishes the Debug Capability from
storage as a channel, over and above what either does for logging.

## The IOMMU: measured on the target, and it is not a problem there

Read from the machine rather than reasoned about, and it retires a large
amount of planned work.

**Windows does not translate for either candidate device.** From the SYSTEM
hive, read offline over SSH from the mounted volume:

```
xHCI VEN_8086&DEV_9DED  instance 3&11583659&1&A0  location (0,20,0)
     Device Parameters\DMA Management : ABSENT -> per-driver value governs

Services\USBXHCI\Parameters\DmaRemappingCompatible  = 2
Services\stornvme\Parameters\DmaRemappingCompatible = 2
Services\storahci\Parameters\DmaRemappingCompatible = 2
Services\pci                                        = 1
Services\Usb4HostRouter                             = 1
```

Microsoft documents `2` as opt-in only when the device is external or when
Driver Verifier's DMA verification is on. Both candidates are internal, so
they stay in a passthrough domain. Note `pci` and `Usb4HostRouter` opt in
unconditionally - so a controller behind Thunderbolt or USB4 *would* be
translated, which is why this must be detected rather than assumed.

**Kernel DMA Protection is off on this platform anyway.** From the DMAR:

```
DMAR flags = 0x01   DMA_CTRL_PLATFORM_OPT_IN (bit 2) = 0
RMRR 0x974c8000-0x974e7fff (128 KiB)  scope PCI Endpoint 00:14.0   the xHCI
RMRR 0x9b800000-0x9fffffff  (72 MiB)  scope PCI Endpoint 00:02.0   the iGPU
```

Microsoft names that flag as the Intel-side prerequisite for Kernel DMA
Protection. The firmware does not set it, so there is no protection to
damage and nothing to stop reporting.

**And the firmware already ships an RMRR scoped to the xHCI.** So if a
reserved region ever is needed, it is not a novel request on this hardware -
the vendor makes the same one, for the same device, and Windows boots with
it. That is the classic USB legacy-emulation buffer, and the 72 MB one is
integrated graphics.

### What still has to be built, because this is machine-specific

Detect it at runtime rather than assuming it, read-only, on any machine:
`GSTS.TES` for whether translation is on, `RTADDR.TTM` for the mode, then
the root entry for the bus and the context entry for the device, whose bits
3:2 give the translation type - `10b` is pass-through. Four outcomes, all
distinguishable, and if it is translating, walk the second-stage tables and
see whether our pages are mapped. AMD's IVRS needs the equivalent.

Report the verdict in the log's first lines, so no future investigation
repeats this and so a machine where the channel cannot work says so at once.

A bug not to copy from the prior art: testing `RTADDR & (1 << 11)` for an
extended root table misparses scalable mode, which is bit 10. Test the whole
`TTM` field.

### Why this was measured rather than tried

DMA outside an RMRR after `ExitBootServices` can raise bugcheck `0xE6`,
subcode `0x26`, "IOMMU detected DMA violation" - and it does not need Driver
Verifier to be enabled. Arming a channel to see whether its DMA lands is not
a benign probe; it can take the machine down mid-write.

### If a machine does translate, in order of preference

1. **Inject an RMRR** scoped to the device and covering only the DMA pages -
   for the debug capability that is five pages, twenty kilobytes, not the
   whole module, which carries a twenty megabyte heap and executable code. A
   *declared* hole the operating system records and can be queried about.
2. **Add mappings to the guest's own second-stage tables.** Cheap in one
   specific way: turning a not-present entry present needs no invalidation on
   hardware reporting `CAP.CM = 0`, and the specification says real hardware
   must support that mode. Never touch a present entry. But it is an
   *undeclared* hole, and the guest goes on reporting protection it no longer
   entirely has - so it must be logged loudly if it ever fires.
3. Unlinking the DMAR wholesale removes the guest's DMA protection silently
   and is the option of last resort.

Prior art for the first two exists and is worth reading before writing
either: a bare-metal hypervisor that installs under a running operating
system implements both, RMRR injection and force-mapping, in one file.
Intel's own firmware uses an RMRR for exactly this purpose, for exactly this
device class - its USB driver reports its DMA buffer that way rather than
bypassing translation.

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
