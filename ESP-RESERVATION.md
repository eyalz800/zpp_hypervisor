# Reserving logical blocks on the EFI system partition

The resident hypervisor has no boot services, no FAT driver, and no way to
ask anybody where anything is. It has a queue pair, a namespace identifier
and an LBA. So the destination for its disk log has to be established by
the loader and handed across as numbers — `zpp::nvme::log_target`.

This document is about how that destination is obtained, why the obvious
way to obtain it is wrong, and what was actually proven about the way that
replaced it.

## The obvious way, and why it was abandoned

Create a file on the EFI system partition, give it a size so FAT allocates
clusters, read the partition raw, parse FAT32, walk the directory, walk the
cluster chain, and hand over the extents. That was built. It works. It is
also unsafe in a way that cannot be patched:

**Deleting a file does not touch its data blocks.**

The resident side's write guard (`zpp/nvme/log_writer.h`) reads its
destination before every write and requires a `block_signature` to already
be there. That is what makes writing next to a boot manager acceptable —
the loader stamps the file's blocks, and a misdirected write lands on a
block that does not carry the stamp and is refused.

But a delete leaves the stamp exactly where it was. If anything removes the
log file mid-boot — and clearing an EFI system partition that has filled up
is a documented practice — FAT frees those clusters with our signature
still sitting in them. The guard reads it, concludes the block is still
ours, and keeps writing. FAT meanwhile hands the same clusters to whatever
is written next, which on this partition is somebody's `bootmgfw.efi`.

Re-validating the file system before each write narrows the window. It
cannot close it: the delete can land between the validation and the write.
The failure mode is "corrupt the machine's boot loader", so narrowing is
not good enough, and the whole approach was dropped.

## What replaced it

FAT32's parameter block carries its own total sector count, and nothing
requires that count to equal the size of the partition it sits in. A file
system smaller than its partition is ordinary, and every FAT driver honours
it, because the count is precisely what the driver derives its cluster
count from.

So the loader shrinks the file system's declared total by the size of the
reservation, leaving a tail inside the partition that no cluster maps to.
That tail cannot be allocated, deleted, moved, defragmented or handed out,
because as far as every FAT implementation is concerned it is not part of
the volume. There is no extent map, no per-write revalidation and no file.
It is a fixed range of LBAs.

The cost is honest and visible: the partition's free space drops by the
reservation.

## The procedure

`uefi_loader/src/esp_reservation.cpp`, in order:

1. **Identify the volume.** `EFI_LOADED_IMAGE_PROTOCOL` on our own handle
   gives the device we booted from; `EFI_BLOCK_IO_PROTOCOL` gives the
   logical block size and the medium's extent; the device path gives the
   NVMe namespace identifier, and its `HARDDRIVE_DEVICE_PATH` node gives
   `PartitionStart`, `PartitionSize` and the partition GUID. The disk GUID
   comes from the GPT header on the *parent* device, found by matching the
   partition's device path with its hard drive node removed.

   **The partition start is read, never assumed.** Block IO on a partition
   handle is relative to the partition; the resident side's queue addresses
   the namespace. Getting that offset wrong is the difference between
   writing to the reservation and writing that far into somebody's disk.

2. **Read and believe the parameter block.** Boot signature, `FAT32` type
   string, 16-bit sectors-per-FAT zero, root entry count zero, 16-bit total
   zero, bytes-per-sector equal to the media block size, sectors-per-cluster
   a non-zero power of two, and a data cluster count of at least 65525 —
   which is the specification's own rule for what makes a volume FAT32
   rather than FAT16.

3. **Compare against the partition.** If the gap between the end of the file
   system and the end of the partition is already at least the reservation,
   *do nothing*. This is the steady state and it writes nothing at all,
   which is what makes running this every boot free.

4. **Prove the space is free before taking it.** One pass over the active
   FAT — the one the extended flags nominate, not blindly the first copy —
   confirming that every cluster which would fall outside the new total has
   a zero entry. If any is allocated, nothing is written and the boot
   reports `region_in_use`. Excluding a cluster that holds data orphans it
   silently, and on this partition that data is somebody's boot loader.

   The same pass counts free clusters in the region that survives, so a
   shrink that would leave a machine with nowhere to write is refused too.

5. **Shrink.** The new total is rounded *down* to a cluster boundary, then
   written to the boot sector and to the backup boot sector — both, because
   a repair tool that finds them disagreeing restores the backup over the
   primary and would put the reservation back into the allocator's hands.
   Both file system information sectors have their free count set to the
   specification's "unknown" value rather than left holding a figure that is
   now wrong. Then the boot sector is read back and the new total confirmed.

6. **Sign the range**, once, through raw Block IO, with the same
   `zpp::nvme::block_signature` the resident write guard requires. Only the
   first and last block are read to decide whether this is needed, because
   a partially signed range can only come from this being interrupted and
   the last block is the last thing written.

7. **Fill `log_target`**: one extent, absolute LBAs, block size, namespace
   id, both GUIDs, a stable file id — and `magic` last, so a structure that
   was half filled and then refused can never read as usable.

## Two boots to come up, deliberately

The boot that performs the shrink hands over **nothing**.

The firmware's FAT driver mounted the volume before the loader ran and
computed its cluster count from the old, larger total. Writing a new total
behind its back does not tell it. For the rest of that boot the driver
would still cheerfully allocate a cluster inside the range just reserved —
and this loader writes its own trace file to that partition later in the
same boot.

So the shrink boot shrinks and reports. Every boot after it finds the gap
already there, mounts a file system that ends where we left it, and hands
over a usable target. That is also what makes the whole thing idempotent.

This is not theoretical. On the shrink boot the firmware's FAT driver was
observed rewriting the file system information sector with a free cluster
count computed from its stale mount — a figure describing more clusters
than the volume now has. It is a hint rather than an allocation bound, so
nothing was harmed, but it is a stale mount writing to the medium after the
shrink, which is exactly the thing the two-boot rule exists for.
`repair_fs_info` puts that count back to "unknown" on the next boot.

## The one thing that can undo it

Something *growing* the file system back to fill the partition — a resizing
tool. The tail becomes allocatable again while the resident side still
believes it owns it.

That is checkable, cheaply, and the check needs one field that
`zpp::nvme::log_target` does not have. **The proposal, stated exactly:**

```cpp
// in struct log_target, beside block_size
std::uint32_t filesystem_total_sectors{};
```

with the sense "the reservation begins where the file system ends, and is
only valid while the file system still ends there". The resident side reads
sector zero of the partition, takes the 32-bit total at BPB offset 32, and
refuses the channel if it is larger than this value.

Until that field exists the loader exposes it as
`zpp::esp_reservation::filesystem_total_sectors`, so the check can be wired
up without this component changing.

## The call site

One line in `uefi_loader/src/main.cpp`, immediately after
`nvme_selftest::run();`:

```cpp
    zpp::esp_reservation::establish(image_handle, system_table);
```

and `#include "zpp/esp_reservation.h"` with the other project headers. It
takes the image handle rather than a device handle so it needs no setup —
which volume the loader came from is the one thing it can always find out
about itself. It never fails the boot: every refusal is traced and
`target` is left zeroed, which `usable()` reads as "no channel".

## Every case where it refuses

| Refusal | Cause |
| --- | --- |
| `no_loaded_image`, `no_device_path`, `no_block_io` | the volume was never reached |
| `unsupported_block_size` | logical block larger than 4096 |
| `read_failed`, `write_failed` | Block IO said no |
| `not_fat32` | no boot signature, no `FAT32` type string, a non-zero 16-bit sectors-per-FAT or root entry count, or fewer than 65525 data clusters |
| `bpb_inconsistent` | a non-zero 16-bit total (two totals, so writing one leaves the volume saying two different things), a zero or non-power-of-two sectors-per-cluster, a zero reserved count / FAT count / FAT size / total, an active FAT index past the FAT count, a total not past the first data sector, or a FAT too short for its own cluster count |
| `sector_size_mismatch` | bytes-per-sector is not the media block size |
| `partition_too_small` | the file system claims more sectors than the partition holds |
| `too_few_clusters_left` | the shrink would take the volume below 65525 clusters, so it would stop being FAT32 |
| `too_little_free_left` | fewer than 16 MB would remain free |
| `region_in_use` | a cluster in the region to be excluded is allocated |
| `write_did_not_take` | the new total did not read back as written |
| `stamp_failed` | the signature is not at the LBA the target names, after writing it |

Nothing is written on any of these. `log_target` is zeroed, not
approximate.

## What was proven, by reading bytes

Tested under QEMU with an emulated NVMe controller, against a purpose-built
1 GB FAT32 image (the `scripts/bochs/setup.sh` image is 64 MB, which is the
refusal case below). The original `build/bochs/esp.img` was never written
to; every run used a copy.

Baseline of the 1 GB image, read with Python:
`bytes_per_sector=512  sectors_per_cluster=8  reserved=32  num_fats=2
fatsz32=2044  tot32=2097152  first_data_sector=4120  data_clusters=261629`

**Boot 1 — the shrink.** Traced: `filesystem total sectors 0x200000`,
`data clusters 0x3fdfd`, `free bytes left after shrink 0x3bd22000`,
`shrinking total sectors to 0x1e0000`, `backup boot sector updated at 0x6`,
`fs info invalidated at sector 0x1`, then `reserved this boot, so the
channel stays off until the next one`, `no channel this boot`.

Proven by reading the image afterwards: `tot32=1966080` in the boot sector
**and** `1966080` in the backup boot sector at sector 6. Both changed, both
to the same value, which is `2097152 - 131072` and lands exactly on a
cluster boundary.

**Boot 2 — idempotent, and the signing.** Traced: `already reserved, gap
sectors 0x20000`, `signing the reserved range`, `reserved first lba
0x1e0000`, `reserved lba count 0x20000`, `reserved blocks of 4k 0x4000`,
`channel ready`. Nothing was shrunk.

Proven by reading the image at the LBAs the trace named:

```
block     0 lba  1966080: magic=0x4b4c42474f4c505a file_id=0x3130474f4c50505a block_index=0
block     1 lba  1966088: magic=0x4b4c42474f4c505a file_id=0x3130474f4c50505a block_index=1
block  8192 lba  2031616: magic=0x4b4c42474f4c505a file_id=0x3130474f4c50505a block_index=8192
block 16383 lba  2097144: magic=0x4b4c42474f4c505a file_id=0x3130474f4c50505a block_index=16383
```

`0x4b4c42474f4c505a` is `ZPLOGBLK`, `0x3130474f4c50505a` is `ZPPLOG01`.
The block index is right at both ends of a 16384 block range, which is what
says the range was addressed as intended rather than merely written to. The
eight sectors immediately before the region are still zero, so nothing
overran the boundary downwards.

**Boot 3 — nothing to do.** Traced: `already reserved`, `range already
signed`, `channel ready`. No write of any kind.

**The file system survived.** `mdir` after the shrink lists `/EFI/BOOT` and
`/EFI/zpp` with their files, and `BOOTX64.EFI` extracted with `mcopy` is
byte-identical to the built loader.

**FAT cannot reach the reservation.** 943,849,472 bytes of random data were
copied into the shrunk file system with `mcopy` until it reported
`No space left on device`. The reserved 64 MB was then hashed and compared
against the same range before the fill:

```
sha256 before fill: 0ad6babab4afb4f87aeb2e81abca873ca28ae2e55d07284f46d3cede70a72a0e
sha256 after  fill: 0ad6babab4afb4f87aeb2e81abca873ca28ae2e55d07284f46d3cede70a72a0e
IDENTICAL
```

The last cluster inside the file system holds fill data, so the fill really
did reach the boundary and stop there. This is the proof that matters: a
full file system does not touch the reservation.

**The refusal path was exercised.** The 64 MB `setup.sh` image reports
`data clusters 0x1f7fe` and then `refused - Shrinking would stop the volume
being FAT32`, because 64 MB minus 64 MB leaves nothing, and even a smaller
reservation would take it under 65525 clusters. The image was re-read
afterwards: `tot32=131072` in both the boot sector and the backup, unchanged.
Nothing was written on the refusal.

**The stale free count.** After the shrink boot the file system information
sector held `free=0x3fd20` — 261408 clusters, more than the 245245 the
volume now has — because the firmware's FAT driver rewrote it from its
stale mount. `mdir` believed it and reported a gigabyte of free space on a
960 MB file system. `repair_fs_info` sets an impossible count to "unknown";
after the next boot the sector reads `free=0xffffffff nextfree=0xffffffff`
and `mdir` reports 1,003,610,112 bytes free, which is the right answer.

### What is only traced, not proven by bytes

- The partition start and size coming from `HARDDRIVE_DEVICE_PATH`. The
  test medium is a bare FAT image with no partition table, so there is no
  hard drive node and the loader takes the whole-device path — which it
  says out loud (`no hard drive node, the volume is the whole device and
  starts at lba 0`) rather than defaulting silently. The device path branch
  is written and compiled but has only ever run against a partition start
  of zero. **This is the one thing to check first on real hardware**, and
  it is checkable from the trace alone: `partition start lba` must be
  non-zero on a GPT disk.
- The disk GUID, for the same reason — no GPT on the test medium, so
  `read_disk_guid` finds no parent and leaves it zeroed.
- Behaviour on a 4096-byte logical block namespace. The emulated controller
  reports 512.
- The `region_in_use` refusal. Constructing an allocated cluster in the tail
  requires a file system that was already full to its last cluster; it has
  not been staged.

### Unrelated, but it cost a run

Under QEMU's TCG the boot ends in `#UD` after the loader finishes, because
the hypervisor executes `vmxon` on a processor that does not implement VMX.
This has nothing to do with this component and predates it — the same crash
happens on a build of `develop` with none of this compiled in. Build with
`-DZPP_CHAINLOAD_ONLY=ON` to test loader-side work under TCG.


## The double boot, on the merged tree

Repeated after the work was merged and the call site wired into
`main.cpp`, because the agent's run predated both. A 1 GB FAT32 image,
booted twice under QEMU, then read from the host.

**Boot one - the shrink.**

```
esp reservation: backup boot sector updated at 0x6
esp reservation: fs info invalidated at sector 0x1
esp reservation: reserved this boot, so the channel stays off until the
                 next one - the mounted file system still believes it
                 owns the range
esp reservation: no channel this boot
```

Total sectors `2097152` to `1966080`, confirmed by reading the boot
sector **and** its backup at sector 6 directly.

**Boot two - the reservation is found.**

```
esp reservation: reserved first lba 0x1e0000
esp reservation: reserved lba count 0x20000
esp reservation: reserved blocks of 4k 0x4000
esp reservation: channel ready
```

**Read back from the host**, which is the part that counts:

```
first block   lba 1966080  magic ZPLOGBLK  file_id ZPPLOG01  index 0
second block  lba 1966088  magic ZPLOGBLK  file_id ZPPLOG01  index 1
last block    lba 2097144  magic ZPLOGBLK  file_id ZPPLOG01  index 16383
8 sectors before the region: all zero
```

Free space fell by 67,112,960 bytes, which is the 64 MB reserved - an
independent confirmation that the file system agrees about what it lost.

**That FAT cannot reach it**, which is the whole point: 1,003,488,629
bytes written into the volume until 114,688 bytes remained, and the
reserved region's sha256 is unchanged -
`0ad6babab4afb4f87aeb2e81abca873ca28ae2e55d07284f46d3cede70a72a0e`
before and after.

The boundary is exact. The data area's last sector is 1966079 and the
reservation begins at 1966080: they abut, with no gap and no overlap.

**Still unproven, and both need real hardware.** The
`HARDDRIVE_DEVICE_PATH` branch never ran - the test medium is a bare FAT
image with no partition table, so the partition starts at LBA 0 and the
loader says so rather than defaulting. On a GPT disk that must come back
non-zero, and it is one traced line to check. The disk GUID is unread
for the same reason.
