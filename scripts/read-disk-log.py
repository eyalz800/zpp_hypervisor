#!/usr/bin/env python3
"""Read the hypervisor's disk log out of a raw dump of the reserved region.

This is the far end of the channel, and deliberately the dumbest thing that
can work: it takes bytes off the medium and scans them. It does not parse
FAT, does not read a directory entry, and does not need the extent list to
have been resolved correctly - which is what makes it an independent check
on that resolution rather than a consumer of it.

    ssh tc@host 'sudo dd if=/dev/nvme0n1 bs=512 skip=33456128 count=131072' \
      > region.bin
    scripts/read-disk-log.py region.bin

Two kinds of block live in there and both are ours:

  - blocks the reservation stamped and nothing has written yet, which carry
    a signature and no header
  - blocks the hypervisor has written, which carry a signature *and* a
    header, because a block that came out of the writer has to still be one
    the writer will accept next time round

So a region full of stamps and no headers means the reservation worked and
the resident side never wrote - which is a different failure from a region
with no stamps at all, and worth being able to tell apart at a glance.
"""
import struct
import sys

BLOCK = 4096

SIGNATURE_MAGIC = 0x4B4C42474F4C505A
HEADER_MAGIC = 0x314B4C42474F4C5A

# From llvm-dwarfdump on the built hypervisor. The signature comes first
# because the write path reads offset zero of the destination and demands
# one there before it will overwrite anything.
OFF_SIGNATURE_MAGIC = 0x00
OFF_SIG_FILE_ID = 0x28
OFF_SIG_BLOCK_INDEX = 0x30
OFF_BLOCK_MAGIC = 0x38
OFF_BOOT_ID = 0x40
OFF_EPOCH = 0x48
OFF_SEQUENCE = 0x50
OFF_BLOCK_INDEX = 0x58
OFF_RECORD_COUNT = 0x60
OFF_RECORD_SIZE = 0x64
OFF_RECORDS_LOST = 0x68
OFF_RECORDS_REFUSED = 0x70
OFF_BLOCKS_DROPPED = 0x78
OFF_BLOCKS_LOST_TO_RESET = 0x80


def u64(b, off):
    return struct.unpack_from("<Q", b, off)[0]


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    with open(sys.argv[1], "rb") as handle:
        data = handle.read()

    stamped = 0
    written = []
    foreign = 0

    for start in range(0, len(data) - BLOCK + 1, BLOCK):
        block = data[start:start + BLOCK]
        if u64(block, OFF_SIGNATURE_MAGIC) != SIGNATURE_MAGIC:
            if any(block):
                foreign += 1
            continue

        if u64(block, OFF_BLOCK_MAGIC) != HEADER_MAGIC:
            stamped += 1
            continue

        written.append({
            "at": start // BLOCK,
            "boot_id": u64(block, OFF_BOOT_ID),
            "epoch": u32(block, OFF_EPOCH),
            "sequence": u64(block, OFF_SEQUENCE),
            "block_index": u64(block, OFF_BLOCK_INDEX),
            "records": u32(block, OFF_RECORD_COUNT),
            "record_size": u32(block, OFF_RECORD_SIZE),
            "lost": u64(block, OFF_RECORDS_LOST),
            "refused": u64(block, OFF_RECORDS_REFUSED),
            "dropped": u64(block, OFF_BLOCKS_DROPPED),
            "lost_to_reset": u64(block, OFF_BLOCKS_LOST_TO_RESET),
        })

    total = len(data) // BLOCK
    print(f"{total} blocks read from {sys.argv[1]}")
    print(f"  {stamped:6d} stamped by the reservation, never written")
    print(f"  {len(written):6d} written by the hypervisor")
    if foreign:
        print(f"  {foreign:6d} carrying something that is not ours")

    if not written:
        print()
        print("No block carries a header. The reservation established the")
        print("region and the resident side never completed a write to it.")
        print("Read the queue counters rather than the medium - see the")
        print("boot-windows-rig skill.")
        return 1

    written.sort(key=lambda entry: entry["sequence"])
    print()
    print("  seq      block  epoch  recs  lost  refused  dropped  reset")
    for entry in written:
        print("  {sequence:<8d} {block_index:<6d} {epoch:<6d} "
              "{records:<5d} {lost:<5d} {refused:<8d} {dropped:<8d} "
              "{lost_to_reset}".format(**entry))

    boots = {entry["boot_id"] for entry in written}
    print()
    print(f"{len(boots)} boot(s) represented: "
          + ", ".join(f"{boot:#018x}" for boot in sorted(boots)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
