# Asking the guest to reserve a window

`DIAGNOSTICS.md` establishes that remapping hardware translates a *device's*
DMA regardless of which software programmed the device, so a translating
domain that does not contain our buffers stops the channel with every
controller register still reading healthy. It lists five answers and
recommends measuring first. `zpp/nvme/iommu_gate.h` was that measurement.
This is the first of the two answers `zpp/nvme/dma_reachability.h` names,
built.

Everything cited here is from the Intel Virtualization Technology for
Directed I/O Architecture Specification, **revision 5.20**, order number
D51397-019, and from Linux `drivers/iommu/intel/` at v6.12. Only sections
actually read are cited. The existing comments in `iommu_gate.h` cite
revision 4.0 for the parts that predate this work; the section numbers for
those parts are the same in both, but they have not been re-checked against
5.20 and are left as they were found.

## Why a declared hole rather than a page table entry

The tempting version is to walk the guest's second-stage tables and add an
identity mapping for our window. It is fewer moving parts and it needs no
ACPI surgery. It is also wrong for a reason that has nothing to do with
etiquette: **the entry does not survive.**

A guest tears a domain down and rebuilds it on a function level reset, on
every D3 entry and exit, on a driver reload, and on a bugcheck's dump path.
Each rebuild starts from empty page tables, and an entry nobody's data
structures know about is not replayed. Worse, the teardown walks the tree
and frees the pages in it - so a table allocated by us and hung off the
guest's tree goes into the guest's own free list. That is the reason
`iommu_gate::reach` **refuses** when an intermediate level is missing rather
than allocating one, and it is not a conservative choice; it is the only
correct one.

A reserved region is a different kind of object. VT-d 5.20 3.16:

> For legacy compatibility, system software is expected to setup identity
> mapping in second-stage translation (with read and write privileges) for
> these reserved address ranges, for the specified devices. For these
> devices, the system software is also responsible for ensuring that any
> input addresses used for device accesses to OS-visible memory do not
> overlap with the reserved system memory address ranges.

The second sentence is the one that matters. The range is not merely mapped
once; it is excluded from the allocator that hands addresses to drivers, and
the exclusion is re-derived from the table at every domain creation. A guest
that honours it therefore restores the property after every one of the
events above, without knowing we exist.

What it costs the guest is one range, in one device's address space,
declared in a table the guest can read and report. That is why
`dma_reachability.h` makes it the default and keeps taking the hardware as
the fallback rather than the other way round.

## Half one: the loader

`uefi_loader/src/reserved_region.cpp`, run before the boot manager is
chainloaded.

1. Find the ACPI 2.0 configuration table, the XSDT, and the DMAR.
2. Verify the DMAR's checksum and that its structure list parses - every
   structure's length at least 4, none running past the table, and the last
   ending exactly at the end. A zero length would otherwise spin.
3. Walk PCI from bus 0 depth first for the first NVM Express controller,
   recording the {Device, Function} path to it through each bridge.
4. Verify the controller is under some remapping unit's scope. VT-d 5.20 8.4
   requires it: the devices an RMRR names "must be devices under the scope of
   one of the remapping hardware units reported in DRHD". An explicit device
   scope entry wins over `INCLUDE_PCI_ALL`, because 8.3 says the catch-all
   covers everything "except devices reported under the scope of other
   remapping hardware units for the same Segment".
5. Refuse if firmware already declares a region for this controller.
6. Allocate 16 KB as `EfiReservedMemoryType`. 8.4 requires it: "BIOS must
   report the RMRR reported memory addresses as reserved (or as EFI runtime)
   in the system memory map returned through methods such as INT15, EFI
   GetMemoryMap etc."
7. Build the RMRR. Base 4 KB aligned, Limit the last byte and strictly
   greater than Base, `Limit - Base + 1` a multiple of 4 KB - all three from
   8.4, all three checked rather than assumed, because a region failing any
   of them is one a guest may reject wholesale and take the firmware's own
   regions with it.
8. Insert it **in numerical order**. 8.2: "BIOS implementations must report
   these remapping structure types in numerical order. i.e., All remapping
   structures of type 0 (DRHD) enumerated before remapping structures of type
   1 (RMRR), and so forth." So the RMRR goes after the last DRHD or RMRR and
   before the first ATSR, RHSA, ANDD, SATC or SIDP. Appending at the end of
   the table is a specification violation whenever any of those is present.
9. A table cannot grow in place, so allocate an `EfiACPIReclaimMemory` copy
   with room, splice, fix Length and the checksum, and repoint the XSDT
   entry - and the RSDT entry too, fixing both those checksums. The copy is
   allocated **below four gigabytes** on purpose: an RSDT entry is 32 bits
   wide, so a table above that could be reached from one list and not the
   other, and the two would disagree about where the DMAR is.

If any step refuses, nothing has been written and the firmware's own table is
untouched. The rewritten table is re-parsed before it is published, so a
splice that produced nonsense is caught before anything points at it.

### Type 01h endpoint, not Type 02h sub-hierarchy

Both were considered. The region names the controller itself, Type 01h.

- 8.4 describes the device scope as identifying "devices requiring access to
  the specified reserved memory region". A sub-hierarchy entry names a
  bridge, and 8.3.1 defines what that stands for: "the collection of PCI
  controllers that are downstream to a specific PCI-PCI bridge". So it puts
  the same hole in the address space of *every* device behind the bridge.
  More of the guest's protection given up than the job needs.
- The usual argument for 02h is bus renumbering, and it does not apply. A
  scope path is not a bus number: 8.3.1's own pseudocode resolves it by
  reading each bridge's secondary bus register at the time of use
  (`bus = read_secondary_bus_reg(bus, dev, func)`), so the path survives the
  rebalancing 8.3.5 describes without being rewritten.
- On a root-complex integrated controller there is no bridge to name at all,
  and 8.3.1 says such a device's "requester-id ... [is] static and not
  impacted by system software bus rebalancing actions". 02h is unavailable
  exactly where it would be least needed.

`scope_covers` still *understands* Type 02h, because firmware writes them -
and on the QEMU rig it is a Type 02h entry naming the root port that puts our
controller under the DRHD's scope. Reading them and writing them are separate
decisions.

## Half two: the resident side

`hypervisor/include/zpp/nvme/iommu_gate.h`, extended from a per-device
question to a per-range one, and `reserved_region_strategy.h` on top of it.

`translation_verdict` answered "is this device translated", which was the
right question while nothing could be done about it. `reach_verdict` answers
"is *this range* reachable, and if not, why not", with one enumerator per
cause. Three mean reachable, one means installed, one means would-install,
and fifteen are named refusals.

The walk supports both translation table modes:

- **Legacy, TTM 00b.** Root entry at `bus * 16`, low quadword bit 0 present
  with bits 63:12 the context table. Context entry at `devfn * 16`, bits 3:2
  the translation type, bits 63:12 SSPTPTR, and the address width in bits 2:0
  of the high quadword. VT-d 5.20 9.1 and 9.3; Linux `iommu_context_addr()`
  and `dmar_fault_dump_ptes()`.
- **Scalable, TTM 01b.** Root entry LP/LCTP in the low quadword for
  `devfn < 0x80` and UP/UCTP in the high for the rest - which is the same
  test Linux makes, `if (devfn >= 0x80) { devfn -= 0x80; entry = &root->hi; }`
  followed by `devfn *= 2` for the 32 byte context entry. Context entry P at
  bit 0, PASIDDIRPTR at 63:12, PDTS at 11:9, RID_PASID at 83:64. PASID
  directory entry 8 bytes at `pasid >> 6`. PASID table entry 64 bytes at
  `pasid & 0x3f`, with P at bit 0, AW at 4:2, PGTT at 8:6 and SSPTPTR at
  (HAW-1):12. VT-d 5.20 9.2, 9.4, 9.5 and 9.6.

`RID_PASID` is read rather than assumed zero, but **only when `ECAP.RPS` says
the field exists** - 11.4.3 says hardware reporting it clear "uses the value
of 0 for RID_PASID" whatever is written there, so reading it unconditionally
would follow a field hardware ignores. The PASID directory index is bounded
against `2^(PDTS+7)` before it is used, because an index past the end reads
whatever the guest keeps after the directory.

### The install, and the invalidation that is not issued

Only ever 0 to present. A present entry is never modified and nothing is ever
unmapped: leaving the mapping in place forever is the correct outcome, and
removing it is the direction that would need an invalidation. The entry is
`(hpa & addr_mask) | R | W`, with SNP at bit 11 **only** when `ECAP.SC` allows
it, since Table 47 makes the field "reserved(0) by hardware implementations
not supporting Snoop Control" and a reserved bit set in a present entry is a
fault rather than an ignored hint. There is no execute permission to withhold:
bit 2 is `IGN` in every second-stage entry type, Tables 41 to 47, and Linux's
`dma_pte` bits are READ, WRITE, LARGE_PAGE and SNP with no EXEC.

`CLFLUSH` when `ECAP.C` is clear, because the unit's page walker then does not
snoop and would read the stale line out of memory. Then `SFENCE` - not
`order_stores()`, which is a release fence and therefore a compiler barrier
and nothing more on this architecture, and a `CLFLUSH` is not ordered by one.

**No invalidation is issued.** VT-d 5.20 6.5.3.3, the note heading Table 28:

> Invalidations described in the table are required when the entry being
> changed is present or when Caching Mode (CM) is reported as 1.

Neither holds: the entry was not present, and `CAP.CM = 1` is refused before
the walk begins. Linux agrees in code -
`cache_tag_flush_range_np()` in `drivers/iommu/intel/cache.c` takes the
`!cap_caching_mode(iommu->cap)` branch, flushes the write buffer and
`continue`s, queuing no descriptor at all.

This matters more than a paragraph of specification. The only channel for an
invalidation is a queue whose tail register the guest's operating system
owns; appending to it is the step to avoid, not merely to optimise away. The
register-based path is not an escape either - it is deprecated and
non-functional on units reporting a major version of 6 or above, which report
every request as an error rather than performing it.

The gate keeps its existing refusals of `CAP.CM = 1` and `CAP.RWBF = 1`, and
adds refusals for abort-DMA mode, the reserved mode, a window that is not
whole 4 KB pages, a window above `CAP.MGAW` or above the domain's own AGAW,
an absent root, context, PASID directory or PASID table entry, first-stage-
only and nested translation, a missing intermediate level, a leaf mapping
something else, a leaf without write permission, and a large page covering the
window.

## What was proven, and what was not

Run under QEMU on `q35` with `-device intel-iommu` and the controller **behind
a root port**, not on bus zero:

```
qemu-system-x86_64 -machine q35 -m 2048 -bios build/bochs/OVMF.fd \
  -drive file=esp.img,if=none,id=nvm,format=raw \
  -device pcie-root-port,id=rp0,bus=pcie.0,slot=1 \
  -device nvme,serial=zpp0001,drive=nvm,bus=rp0 \
  -device intel-iommu -serial file:/tmp/rmrr.serial -display none -no-reboot
```

**Proven by running it.** The firmware boots from the controller at
`PciRoot(0x0)/Pci(0x3,0x0)/Pci(0x0,0x0)`, so the topology under test is a real
two-level one. The scan found it at bus 1, device 0, function 0 with a path of
depth 2, `(3,0)` then `(0,0)`. It resolved the DMAR at `7f775000`, length
`0x80`, checksum good. It found the controller under the DRHD at `fed90000` by
an **explicit** scope entry rather than by the catch-all - and reading the
table back byte for byte shows why: QEMU's DRHD carries flags `0x00`, no
`INCLUDE_PCI_ALL` at all, and eight scope entries of which one is a Type 02h
sub-hierarchy naming the root port at `0:03.0`. The prefix match is the thing
that fired.

It reserved `7f731000` for `0x4000` bytes, spliced the RMRR at offset `0x80`,
and repointed one XSDT entry and one RSDT entry. Dumping the rewritten table
confirms all of it: signature `DMAR`, length `0xa2` matching the 162 bytes
actually present, checksum byte `0x52` with the whole table summing to zero,
the RMRR at offset 128 with Type `0x0001`, Length `0x0022`, Base `7f731000`,
Limit `7f734fff` - so `Limit - Base + 1` is exactly `0x4000` - and a Type 01h
device scope of length 10 carrying the two path pairs `(3,0)` and `(0,0)`.

The resident-side walk was then run read-only against the register block the
DRHD named, and answered `translation off - reachable`. That is the correct
answer under boot services, and it is not a null result: it means `VER_REG` at
`fed90000` read as a real version rather than zero or all ones, so the base
parsed out of the DRHD is a register block.

**Not proven, and this is the important half of the report.**

- **No guest has ever read the injected table.** Nothing here shows an
  operating system parsing the RMRR, reserving the range, or identity mapping
  it. That is the entire premise of the strategy and it is untested.
- **The install path has never executed.** Every run so far ends at
  `no_translation`, because nothing enabled translation. The walk down root,
  context and second-stage tables, the leaf comparison, the CLFLUSH and the
  store have been compiled and reasoned about, not run.
- **The scalable-mode walk has never executed.** `x-scalable-mode` was not
  exercised.
- **Nothing has run on hardware.**
- **The claim that Windows re-applies reservations at every domain creation is
  argued, not measured here.** It is the load-bearing assumption behind
  preferring this over a page table entry, and it deserves a measurement of
  its own.
- The 16 KB window is allocated with `AllocateAnyPages`, so it may land above
  four gigabytes. Nothing checked whether that matters to a guest's reserved
  region handling.

## The hooks the merge has to add

Three lines, none of them in a file this work owns.

1. `uefi_loader/src/main.cpp`, beside the existing `nvme_selftest::run();`
   (add `#include "zpp/reserved_region.h"` and a `using` beside the others):

   ```cpp
   reserved_region::install(system_table);
   ```

   It must run before the boot manager is started and after the firmware has
   published its tables. Immediately after `nvme_selftest::run()` satisfies
   both, and was where it was run for the measurements above.

2. Hand `zpp::reserved_region::window`, the controller's bus/device/function
   and `unit_registers` across to the resident side, and call

   ```cpp
   reserved_region_strategy::configure({...});
   ```

   once from the boot processor before the guest runs. There is no channel for
   this yet - `zpp_loader_parameters` in `loader/` is the natural place and
   this work does not own it.

3. Wherever the disk sink's guard read happens, before each doorbell:

   ```cpp
   if (!may_submit(reserved_region_strategy::ensure_reachable(window))) { ... }
   ```

   `reserved_region_strategy::explain()` names the cause for the line that
   reports the refusal.
