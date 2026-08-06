#pragma once
#include <cstddef>
#include <cstdint>

/**
 * The contract between the loader and the resident hypervisor for the
 * disk log channel, and the layout of what lands on the medium.
 *
 * This header is shared: cmake/uefi-loader/CMakeLists.txt already puts
 * hypervisor/include on the loader's include path, so both sides see one
 * definition rather than two that can drift. It is the whole interface -
 * the loader parses GPT and FAT, and hands across nothing but numbers.
 *
 * See NVME-LOG.md for why the medium is a pre-allocated file on the EFI
 * system partition rather than a partition of our own, and why the reader
 * ignores the file entirely.
 */
namespace zpp::nvme
{
/**
 * One run of consecutive logical blocks belonging to the log file.
 *
 * A list of these rather than a base and a length, because contiguity
 * cannot be demanded: EDK2's allocator fills holes first, and this tree's
 * own trace log delete-and-recreate manufactures fragmentation. Refusing
 * a file that needs more extents than the table holds is safe; assuming
 * contiguity is not.
 */
struct extent
{
    /**
     * Absolute, with the partition base already added. The resident side
     * never learns that partitions exist.
     */
    std::uint64_t first_lba{};
    std::uint64_t block_count{};
};

/**
 * Everything the resident side is told about where to write.
 *
 * Deliberately inert data. There is no protocol here, no callback and no
 * pointer back into the loader: by the time this is used, boot services
 * are gone and the loader does not exist.
 */
struct log_target
{
    /**
     * How many extents the table holds. 256 covers a file of any size
     * this channel would want on a sanely aged file system, and the cap
     * exists so that a pathologically fragmented file is refused rather
     * than truncated.
     */
    static constexpr std::size_t max_extents = 256;

    /**
     * Set only when every check passed, and checked by the resident side
     * before the first write. A zeroed structure therefore reads as "no
     * channel" rather than as "write to LBA 0", which is the failure this
     * ordering exists to prevent.
     */
    static constexpr std::uint64_t valid_magic = 0x315447524154505aull;

    std::uint64_t magic{};

    /**
     * The controller carrying the file, as the loader found it walking
     * the device path of its own boot volume. Passed rather than
     * rediscovered, because the loader has the firmware's help and the
     * resident side does not.
     * @{
     */
    std::uint16_t pci_segment{};
    std::uint8_t pci_bus{};
    std::uint8_t pci_device{};
    std::uint8_t pci_function{};
    std::uint32_t namespace_id{};
    /**
     * @}
     */

    /**
     * The medium's logical block size, from the Block IO protocol. The
     * resident side writes whole 4 KB records, so this decides how many
     * logical blocks one record block spans.
     */
    std::uint32_t block_size{};

    /**
     * How many sectors the file system claimed when the reservation was
     * made.
     *
     * The reservation is a tail of the partition that the file system
     * has been shrunk out of, so it holds only while the file system
     * still ends where it was left. A resizing tool that grows it back
     * makes those blocks allocatable again, silently, and the block
     * signature would not notice: the clusters would be handed to a new
     * file whose data is written over ours, and until it is, our own
     * signature is still sitting there.
     *
     * So the resident side reads the count out of the BPB and refuses
     * the channel if it is larger than this. It is the only check that
     * catches the file system growing back over us.
     */
    std::uint32_t filesystem_total_sectors{};

    /**
     * Identity of the volume and the file, copied into every block's
     * signature so that a write can be checked against the medium rather
     * than against our own belief about it.
     * @{
     */
    std::uint8_t disk_guid[16]{};
    std::uint8_t partition_guid[16]{};
    std::uint64_t file_id{};
    /**
     * @}
     */

    std::uint32_t extent_count{};
    extent extents[max_extents]{};

    /**
     * How many blocks of `block_size` the extents cover between them.
     */
    constexpr std::uint64_t total_blocks() const
    {
        std::uint64_t total{};
        for (std::uint32_t i{}; i < extent_count; ++i) {
            total += extents[i].block_count;
        }
        return total;
    }

    /**
     * Maps a logical block index within the file to an absolute LBA.
     *
     * Returns false rather than a value when the index is past the end,
     * and that is the point of the whole shape: the resident side has no
     * other way to name a destination, so it **cannot express an address
     * outside the file**. An out of range index is a refusal, not a
     * write somewhere else.
     */
    constexpr bool lba_of(std::uint64_t index, std::uint64_t & lba) const
    {
        for (std::uint32_t i{}; i < extent_count; ++i) {
            if (index < extents[i].block_count) {
                lba = extents[i].first_lba + index;
                return true;
            }
            index -= extents[i].block_count;
        }
        return false;
    }

    /**
     * Whether this describes a usable destination at all.
     */
    constexpr bool usable() const
    {
        return (valid_magic == magic) && (0 != extent_count) &&
               (extent_count <= max_extents) && (0 != block_size) &&
               (0 == (block_size & (block_size - 1)));
    }
};

/**
 * What the loader writes into every block of the file at creation, and
 * what the resident side reads back and requires before overwriting it.
 *
 * This is the check that makes writing next to the Windows boot manager
 * acceptable. Parsing FAT correctly is a belief; finding this signature
 * at the LBA the extent table named is a measurement, taken per write. A
 * misdirected write would land on a block that does not carry it, and is
 * refused.
 */
struct block_signature
{
    static constexpr std::uint64_t magic = 0x4b4c42474f4c505aull;

    std::uint64_t signature_magic{};
    std::uint8_t disk_guid[16]{};
    std::uint8_t partition_guid[16]{};
    std::uint64_t file_id{};

    /**
     * Which block of the file this is. Checked against the index the
     * writer intended, so a correct signature at the wrong offset - the
     * shape a mis-resolved extent list would produce - is still caught.
     */
    std::uint64_t block_index{};
};

/**
 * The header on every block the hypervisor writes.
 *
 * Every field here exists so that **a block found in isolation can be
 * placed without reference to anything else.** The reader is `dd` over
 * the partition and a scan for the magic: it does not parse FAT, does not
 * read the directory entry, and does not need the extents to have been
 * resolved correctly - which is what makes it an independent cross-check
 * on the resolution rather than a consumer of it.
 */
struct block_header
{
    static constexpr std::uint64_t magic = 0x314b4c42474f4c5aull;

    std::uint64_t block_magic{};

    /**
     * Distinguishes this boot's blocks from those still on the medium
     * from previous boots, which are never erased - the file is written
     * in place and its size never changes.
     */
    std::uint64_t boot_id{};

    /**
     * Which controller epoch produced this. Bumped every time the guest
     * resets the controller and the queues are rebuilt, so a reader can
     * see the resets in the stream rather than inferring them from a
     * gap.
     */
    std::uint32_t epoch{};

    /**
     * Monotonic across the whole boot and never reduced modulo anything.
     * This is what puts blocks back in order and what makes a gap
     * detectable, so it is the one field a reader cannot do without.
     */
    std::uint64_t sequence{};

    /**
     * Which block of the file this was written to. Redundant with where
     * the reader found it, and deliberately so: the two disagreeing is
     * how a mis-resolved extent table is caught from the reading side.
     */
    std::uint64_t block_index{};

    std::uint32_t record_count{};
    std::uint32_t record_size{};

    /**
     * What was lost, so that quiet is never mistaken for nothing having
     * happened. The first two come from the diagnostic facility's own
     * cursor, the third from our staging ring when the device fell
     * behind, and the fourth from queues destroyed by a guest reset with
     * writes still outstanding.
     * @{
     */
    std::uint64_t records_lost{};
    std::uint64_t records_refused{};
    std::uint64_t blocks_dropped{};
    std::uint64_t blocks_lost_to_reset{};
    /**
     * @}
     */

    /**
     * Over everything in the block after this field. A torn or half
     * written block fails it and is skipped rather than being decoded
     * into nonsense.
     */
    std::uint32_t checksum{};
    std::uint32_t reserved{};
};

/**
 * How much of a block the records get. One block is one write, and it is
 * 4 KB because that is the write granularity every NVMe namespace this
 * would run on handles without a read-modify-write, whatever its logical
 * block size reports.
 * @{
 */
inline constexpr std::size_t block_size = 4096;

/**
 * What the loader hands the resident side so the channel can carry on.
 *
 * The loader is the only place that can establish any of this. It has
 * boot services, so it can walk a file system, resolve a file to logical
 * blocks and validate them; and it runs while the controller is quiet, so
 * it can create a queue pair without racing anybody. The hypervisor has
 * neither of those and cannot rediscover any of it later.
 *
 * Deliberately plain data with no pointers into the loader's own world.
 * Everything here is either a value or an address of something that
 * outlives boot services - the controller's doorbells are device
 * registers, and the queue memory is allocated as reserved rather than
 * as boot services data. A field that pointed at something the firmware
 * reclaims would be a use after free at the first exit.
 *
 * `physical_of` is deliberately **absent**. Turning a buffer into the
 * address a controller will use is the one thing that differs between
 * the two sides - the identity while boot services are alive, a host
 * page table lookup once resident - so the resident side supplies its
 * own rather than inheriting one that stops being true the moment the
 * page tables change.
 */
struct channel_handover
{
    /**
     * Set by the loader as the last thing it writes, and checked before
     * anything here is believed. A zeroed structure therefore reads as
     * "no channel" rather than as a channel pointing at LBA 0.
     */
    static constexpr std::uint64_t valid_magic = 0x314f444e41485a5aull;

    std::uint64_t magic{};

    /**
     * Where the blocks go, already validated by the loader.
     */
    log_target target{};

    /**
     * The private queue pair, as device register addresses and
     * identifiers. Mirrors queue_pair::binding rather than being it,
     * because that type is a template and this crosses a C boundary.
     * @{
     */
    volatile void * submission_doorbell{};
    volatile void * completion_doorbell{};
    volatile void * status_register{};
    volatile void * configuration_register{};
    std::uint16_t submission_id{};
    std::uint16_t completion_id{};
    std::uint32_t namespace_id{};
    /**
     * @}
     */

    /**
     * Whether every field above was established. Checked rather than
     * assumed, so a partially filled structure is refused.
     */
    constexpr bool usable() const
    {
        return (valid_magic == magic) && target.usable() &&
               (nullptr != submission_doorbell) &&
               (nullptr != completion_doorbell) &&
               (nullptr != status_register) &&
               (nullptr != configuration_register) && (0 != namespace_id);
    }
};
inline constexpr std::size_t block_payload =
    block_size - sizeof(block_header);
/**
 * @}
 */

static_assert(sizeof(block_header) < block_size,
              "the header has to leave room for at least one record");

/**
 * The checksum, spelled once so the writer and the reader cannot
 * disagree. Not a CRC: this detects a torn write, which is a whole
 * missing tail rather than a subtle corruption, and a sum over 32 bit
 * words costs nothing on the write path.
 */
constexpr std::uint32_t checksum_of(const std::uint32_t * words,
                                    std::size_t count)
{
    std::uint32_t sum{};
    for (std::size_t i{}; i < count; ++i) {
        sum = (sum << 1) | (sum >> 31);
        sum += words[i];
    }
    return sum;
}

} // namespace zpp::nvme
