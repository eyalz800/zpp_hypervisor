# Finding an NVMe controller behind Intel VMD

## The failure

`find_controller` in `uefi_loader/src/nvme_selftest.cpp` walks all 256 buses
through the CF8/CFC ports looking for class `01/08/02`. On a machine with Intel
Volume Management Device enabled in firmware that walk finds **nothing**, and the
loader reports "no nvme controller on any bus" on a machine it has just booted
from an NVMe drive.

The report is not wrong. VMD is a root complex integrated endpoint that takes a
set of PCI Express root ports *out of* the host's configuration space and
republishes them in a domain of its own. There genuinely is no NVMe controller
where the scan is looking. Nothing about the scan can be fixed; a second
mechanism has to be used.

This is the same shape of mistake the existing comment in that function already
records — the scan looked only at bus zero, passed under QEMU because the
emulated controller lands on bus zero, and found nothing on a real machine. The
difference is that this one cannot be found by widening a loop.

## What VMD looks like

Everything below was read in Linux's `drivers/pci/controller/vmd.c` at `v6.12`,
which is the reference implementation, and in `include/linux/pci-ecam.h` from the
same tag.

- The endpoint is an **Intel RAID bus controller**: vendor `8086`, class
  `01/04/00`. On client platforms it is at `00:0e.0`.
- Its **BAR0 is the children's configuration space**, ECAM shaped.
  `VMD_CFGBAR` is `0`, and `vmd_cfg_addr` indexes the mapping with
  `PCIE_ECAM_OFFSET(bus, devfn, reg)` — bus at bit 20, device and function packed
  into a byte at bit 12, register in the low twelve bits. One megabyte per bus.
- The window's **first bus is not necessarily zero**.
  `vmd_get_bus_number_start` reads a capability word at `0x40`; if its low bit is
  set it reads a configuration word at `0x44` whose bits 9:8 select a start of
  `0`, `128` or `224`. There is a fourth encoding, and that function fails on it.
- **Config writes through the window are posted.** `vmd_pci_write`'s comment:
  the hardware "converts non-posted config writes to posted memory writes", so it
  reads the same location back to force the completion.
- **Access may need serializing.** `vmd_pci_read`'s comment: the "CPU may
  deadlock if config space is not serialized on some versions of this hardware".
  Linux takes a spinlock across every access.
- **A child's BARs are real host physical addresses**, inside the endpoint's own
  MEMBAR1 (BAR2) and MEMBAR2 (BAR4). Nothing is translated, so an NVMe register
  block found this way is used exactly as one found the ordinary way.
- **A child's requester id is not its own.** `pci_real_dma_dev` in
  `arch/x86/pci/common.c` returns, for any device on a VMD bus, the VMD's own
  `pci_dev`. DMA and MSI from behind VMD are sourced with the endpoint's
  requester id.

## What was built

Three pieces, in the order they layer.

### `hypervisor/include/zpp/arch/x86_64/pci_ecam.h`

Configuration space through a memory mapped window. Its own header rather than
another member of `pci_config`, because the two are not the same kind of thing:
the ports are architectural and there is exactly one of them, so `pci_config` is
stateless and every accessor is static. A window has to be told where it is and
how large it is, and there can be more than one at a time — which is the whole
point of the VMD case. Folding a constructor and two members into `pci_config`
would have made every existing static caller pass state it does not have.

Reads in three widths; writes in three widths, each followed by a read back of
the same location for the posted-write reason above. An access the window does
not cover reads as all ones, which is what an absent function reads as through
either mechanism — so every caller's existing "is anything there" test already
refuses it and no caller learns a second way of saying nothing.

No lock is taken. The only caller runs in the UEFI loader before any application
processor has been started and before any guest exists, so there is exactly one
accessor. This is stated in the header rather than left implicit, because the
resident side will not have that property: anything reaching a window from more
than one processor has to wrap it in a `zpp::spin_lock` of its own, and the class
cannot do it for them, since the lock has to cover a whole read-modify-write
rather than one access.

The base handed in is a **mapped** address, not a physical one. Under boot
services those are the same; the resident side has to map the window itself.

### `hypervisor/include/zpp/arch/x86_64/vmd.h`

The VMD knowledge. Read only throughout: it finds what is there and refuses when
it cannot prove what it found. It programs nothing, and in particular assigns no
bus numbers and no windows, because on a machine where this matters the firmware
has already done so and is booting from the result.

`vmd::domain` holds the endpoint's address in the host's configuration space, the
child window, and the bus range. `vmd::find(class_code, domain, child)` walks the
host for endpoints and each endpoint's window for a child of the wanted class.

**Matched by class and vendor, not by a device identifier list.** `vmd.c` carries
such a list, but it needs one for a different reason: its entries select
per-silicon feature flags — shadow membars, bus restrictions, MSI remap bypass —
and none of those are used here. A list is a liability for a plain read only
enumeration, because it is wrong for every part that ships after it was written.
The evidence that a device is a VMD is available directly, and it is what is
checked: an Intel RAID bus controller whose BAR0 is a memory window that decodes
as configuration space and holds a child of the class being looked for.

### `uefi_loader/src/nvme_selftest.cpp`

`find_controller` is now the two mechanisms in order: the ordinary scan, renamed
`find_on_host` and otherwise untouched, and then VMD. That order and not the
other, because the ordinary scan is right on every machine without VMD and a VMD
decode is only ever reached on a machine where it already came up empty. There is
no machine where both find something and the answers differ — a controller behind
a VMD is not findable by the first scan at all.

The result is a `location`, which carries the controller's address, the
**requester id separately**, and the window when there is one. All configuration
space reads below it go through one dispatch, so the BAR validation chain is the
same chain in both cases rather than a second copy that could drift.

The trace says which mechanism found it, and on the VMD path it also reports the
endpoint's bus/device/function, the CFGBAR, the first child bus and the number of
buses scanned. The requester id is always reported on its own three lines,
because it is the value that is not obvious and the one remapping hardware is
programmed with.

## The requester id, and the change `reserved_region.cpp` needs

`reserved_region.cpp` builds a DMAR device scope entry naming the NVMe
controller. Behind VMD that entry would be wrong twice over: the controller's bus
number is a number inside the VMD domain and means something else in the host's
configuration space, and the transactions the entry is meant to scope do not
carry the controller's requester id in the first place. What has to be named is
the **VMD endpoint**.

That file was deliberately not edited here. The change it needs is one condition,
in its own `find_controller`, at the class test:

```cpp
// was
if (class_nvme == (classes >> 8)) {
// becomes
if ((class_nvme == (classes >> 8)) ||
    arch::x86_64::vmd::hosts_class(at, class_nvme)) {
```

plus `#include "zpp/arch/x86_64/vmd.h"` alongside the `pci.h` it already
includes.

It is one condition and no more because of how that function already works: it
writes `found.node[depth]` **before** the class test, so when the VMD endpoint at
`00:0e.0` matches, the recorded path is `start_bus 0`, depth 1, `{0x0e, 0}` — the
device scope path of the endpoint, which is exactly the right answer. Nothing
downstream changes: the scoping unit lookup, the existing-region check and the
emitted entry all take that path as they are.

`hosts_class` is cheap to call from inside a walk. It checks vendor and class
first, so anything that is not an Intel RAID controller costs two reads.

One honest caveat about ordering. That function is depth first by device number,
so on a machine that has both a VMD at `00:0e.0` and an ordinary NVMe behind a
root port at a device number below `0x0e`, the ordinary one still wins; above
`0x0e`, the VMD wins. `nvme_selftest.cpp` does not have that ambiguity because it
runs the two mechanisms as separate passes. If it ever matters, the same
separation is the fix there too.

## What is refused, and why refusal is the right answer

Every check below is a thing that could not be validated rather than a thing
known to be wrong. A machine with no VMD and a machine with a VMD this code does
not understand both end up refusing rather than interpreting whatever the window
happens to hold, which is the house style `validate_bar` already sets.

- Vendor is not Intel, or class is not `01/04/00` — not a candidate.
- BAR0 is an IO BAR, or a 32 bit memory BAR — CFGBAR is neither.
- BAR0 is zero. An unassigned window is not an address, and reading one would be
  reading the low megabyte of physical memory.
- The endpoint's memory space decode is off. Read, never set: the firmware owns
  this device and is booting from what is behind it.
- The bus start field holds the fourth encoding, which `vmd_get_bus_number_start`
  itself gives up on.
- The window cannot be shown to be even one bus wide (see the bound below).
- **The first bus of the window does not decode** — no function on it answers
  with a vendor id that is neither all ones nor zero. On a real VMD that bus
  carries the republished root ports, so a window with nothing on it is not
  configuration space and every conclusion drawn from it would be drawn from
  whatever memory is there.
- No child of the wanted class is found, at which point the whole VMD path
  reports nothing found rather than reporting a device it could not identify.
- The child's BAR fails the existing `validate_bar` chain — unassigned, not a 64
  bit window, not page aligned, decode off, bus mastering off.

### The bus count bound

The window's true size can only be learned by writing all ones into the BAR and
reading back what sticks. That is a write to a live device the firmware is
booting from, so it is not done. The size is bounded from above instead, from two
facts that cost nothing:

- a BAR is naturally aligned, so the **lowest set bit of its base** is an upper
  bound on its size;
- `vmd_probe` refuses a CFGBAR smaller than one megabyte, so one bus is the floor.

The scan takes the smaller of that bound and a cap of 32 buses, which is the
32 MB window the parts this was written for carry. Reading past the end of the
window would be reads of addresses belonging to something else, and a read is not
free of consequence on a device register.

## What has been executed and what has not

QEMU does not emulate VMD, so the VMD path **has never run**. It compiles, and
that is all that can be said for it. Concretely:

- **Executed**, on QEMU q35 with the NVMe behind a `pcie-root-port` and an
  `intel-iommu` present: the ordinary path, end to end, unchanged. The controller
  is found at bus `0x1`, the requester id is reported equal to its own address,
  and the borrow verdict is "restored the admin queue byte for byte". The run was
  compared against a baseline build of the same tree with these changes stashed,
  and every selftest line matches, including the pre-existing proof-write refusal
  (`result 0x4`, `refused_signature 1`) which is present in both and is not a
  regression.
- **Executed**: the ordinary path's dispatch through `read_config`, since that is
  the path the QEMU run takes for every configuration read.
- **Compiled only**: `pci_ecam` in its entirety, every VMD register decode, the
  bus start selection, the alignment bound, the child scan, and every refusal
  listed above. None of it has been run against hardware or against an emulator.

## Risks

- **The whole VMD path is unexercised.** It is written from a reading of
  `vmd.c` and nothing more. The first bare metal boot on a VMD machine is its
  first execution.
- **The 32 bus cap is an assumption about the parts**, not a measurement of the
  window. If a platform ships a larger CFGBAR, a controller on bus 32 or beyond
  is missed and the path reports nothing found — a refusal, not a wrong answer,
  but a silent one.
- **The alignment bound can be conservative to the point of refusing.** A CFGBAR
  the firmware placed on an address whose lowest set bit is below one megabyte
  is refused outright. That should not happen — a BAR is naturally aligned — but
  it is worth knowing that is the failure mode rather than a partial scan.
- **The class-and-vendor match could in principle admit something that is not a
  VMD.** An Intel RAID controller whose BAR0 happens to be a 64 bit memory window
  will be probed as though it were configuration space. Those probes are reads,
  and the path only succeeds if it also finds a device reporting the NVMe class
  inside, so a false positive has to survive a long chain — but it is reads to a
  device's registers, which is not nothing.
- **No lock.** Correct for the loader, wrong the moment anything reaches a window
  from a second processor. `vmd.c` says the CPU may deadlock, not merely that the
  data may be wrong.
- **Nothing here reasons about MSI.** Interrupts from behind VMD are remapped by
  the endpoint and carry its requester id; only DMA scoping is addressed.
