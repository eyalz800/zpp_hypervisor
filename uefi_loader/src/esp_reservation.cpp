#include "zpp/esp_reservation.h"

#include "zpp/trace.h"

#if ZPP_DIAG

extern "C" {
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
}

#include <cstring>
#include <expected>

namespace zpp
{
namespace
{
using code = esp_reservation_error;

/**
 * The protocols this needs, by GUID.
 *
 * Spelled out rather than linked against, for the reason `main.cpp`
 * spells its own out: `-nostdlib` means there is no EDK2 library here to
 * define the `gEfi...Guid` symbols, only its headers.
 * @{
 */
EFI_GUID g_block_io_guid = {
    0x964E5B21,
    0x6459,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

EFI_GUID g_device_path_guid = {
    0x09576E91,
    0x6D3F,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

EFI_GUID g_loaded_image_guid = {
    0x5B1B31A1,
    0x9562,
    0x11D2,
    {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
/**
 * @}
 */

EFI_BOOT_SERVICES * g_boot_services{};

/**
 * The largest logical block this handles. Every 4 KB native namespace
 * and every 512 byte one is covered; anything larger is refused rather
 * than truncated into these buffers.
 */
constexpr std::uint32_t max_block_size = 4096;

/**
 * How much is read or written in one Block IO call.
 *
 * File scope rather than local, and not for taste: a stack frame over one
 * page makes the MSVC ABI emit a call to `__chkstk`, which does not exist
 * in a freestanding loader. `nvme_selftest.cpp` explains this at its own
 * buffers and every resolve-once path in this loader is written this way.
 */
constexpr std::size_t chunk_bytes = 64 * 1024;

alignas(max_block_size) std::uint8_t g_sector[max_block_size]{};
alignas(max_block_size) std::uint8_t g_scratch[max_block_size]{};
alignas(max_block_size) std::uint8_t g_chunk[chunk_bytes]{};

/**
 * Unaligned loads and stores against the boot sector.
 *
 * The BIOS parameter block is a byte layout with no alignment guarantees
 * whatsoever - `BPB_TotSec32` sits at offset 32 and `BPB_RootClus` at 44,
 * neither of which is aligned to anything in a buffer that starts at an
 * arbitrary place. Read through `memcpy` rather than through a cast, so
 * the compiler is never asked to produce an aligned access it is entitled
 * to assume is safe.
 * @{
 */
std::uint16_t load16(const std::uint8_t * at)
{
    std::uint16_t value{};
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint32_t load32(const std::uint8_t * at)
{
    std::uint32_t value{};
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint64_t load64(const std::uint8_t * at)
{
    std::uint64_t value{};
    std::memcpy(&value, at, sizeof(value));
    return value;
}

void store32(std::uint8_t * at, std::uint32_t value)
{
    std::memcpy(at, &value, sizeof(value));
}
/**
 * @}
 */

/**
 * Where each field this reads lives in the boot sector.
 *
 * Named offsets rather than a packed structure, because a structure would
 * have to be declared packed to be correct and the offsets are what the
 * specification is written in - so a reader can check each one against it
 * without counting members.
 * @{
 */
constexpr std::size_t bpb_bytes_per_sector = 11;
constexpr std::size_t bpb_sectors_per_cluster = 13;
constexpr std::size_t bpb_reserved_sectors = 14;
constexpr std::size_t bpb_number_of_fats = 16;
constexpr std::size_t bpb_root_entry_count = 17;
constexpr std::size_t bpb_total_sectors_16 = 19;
constexpr std::size_t bpb_sectors_per_fat_16 = 22;
constexpr std::size_t bpb_total_sectors_32 = 32;
constexpr std::size_t bpb_sectors_per_fat_32 = 36;
constexpr std::size_t bpb_extended_flags = 40;
constexpr std::size_t bpb_fs_info_sector = 48;
constexpr std::size_t bpb_backup_boot_sector = 50;
constexpr std::size_t bs_file_system_type = 82;
constexpr std::size_t boot_signature_offset = 510;
constexpr std::uint16_t boot_signature = 0xaa55;
/**
 * @}
 */

/**
 * The file system information sector's three signatures and the two
 * counts this invalidates. All four are fixed by the specification.
 * @{
 */
constexpr std::size_t fs_info_lead_signature_offset = 0;
constexpr std::size_t fs_info_struct_signature_offset = 484;
constexpr std::size_t fs_info_free_count_offset = 488;
constexpr std::size_t fs_info_next_free_offset = 492;
constexpr std::size_t fs_info_trail_signature_offset = 508;
constexpr std::uint32_t fs_info_lead_signature = 0x41615252;
constexpr std::uint32_t fs_info_struct_signature = 0x61417272;
constexpr std::uint32_t fs_info_trail_signature = 0xaa550000;

/**
 * What the specification calls "unknown" for both counts. Written rather
 * than a recomputed figure, because a wrong count is worse than an absent
 * one - a driver believes it and allocates from it, while "unknown" makes
 * it count for itself.
 */
constexpr std::uint32_t fs_info_unknown = 0xffffffff;
/**
 * @}
 */

/**
 * Everything the resident side needs about the medium, and everything the
 * arithmetic below needs about the partition.
 */
struct volume
{
    EFI_BLOCK_IO_PROTOCOL * block_io{};
    std::uint32_t block_size{};
    std::uint32_t media_id{};

    /**
     * Absolute, from the device path. Block IO on a partition handle
     * addresses the partition, and the resident side's queue addresses
     * the namespace, so this is what converts between them and it is the
     * one number that must not be assumed.
     */
    std::uint64_t partition_start{};

    /**
     * How many logical blocks the partition holds.
     */
    std::uint64_t partition_sectors{};

    std::uint32_t namespace_id{};
    std::uint8_t partition_guid[16]{};
    std::uint8_t disk_guid[16]{};
};

/**
 * The parameter block, once it has been read and believed.
 */
struct fat32
{
    std::uint32_t bytes_per_sector{};
    std::uint32_t sectors_per_cluster{};
    std::uint32_t reserved_sectors{};
    std::uint32_t number_of_fats{};
    std::uint32_t sectors_per_fat{};

    /**
     * The sector the *active* FAT starts at, which is not necessarily the
     * first one: the extended flags word can nominate a single active
     * copy, and reading a mirror that is no longer being updated would
     * answer "free" for clusters that are not.
     */
    std::uint32_t active_fat_start{};

    std::uint32_t first_data_sector{};
    std::uint32_t total_sectors{};

    /**
     * Data clusters, so the highest valid cluster number is this plus
     * one - cluster numbering starts at two.
     */
    std::uint32_t cluster_count{};

    std::uint32_t fs_info_sector{};
    std::uint32_t backup_boot_sector{};
};

std::expected<void, zpp::error> read_sectors(const volume & where,
                                             std::uint64_t sector,
                                             std::uint32_t count,
                                             void * into)
{
    if (EFI_ERROR(where.block_io->ReadBlocks(where.block_io,
                                             where.media_id,
                                             sector,
                                             count * where.block_size,
                                             into))) {
        return std::unexpected(zpp::error{code::read_failed});
    }
    return {};
}

std::expected<void, zpp::error> write_sectors(const volume & where,
                                              std::uint64_t sector,
                                              std::uint32_t count,
                                              void * from)
{
    if (EFI_ERROR(where.block_io->WriteBlocks(where.block_io,
                                              where.media_id,
                                              sector,
                                              count * where.block_size,
                                              from))) {
        return std::unexpected(zpp::error{code::write_failed});
    }
    return {};
}

/**
 * A device path node's length, assembled by hand because it is a two byte
 * field rather than an aligned integer.
 */
std::size_t node_length(const EFI_DEVICE_PATH * node)
{
    return node->Length[0] |
           (static_cast<std::size_t>(node->Length[1]) << 8);
}

/**
 * Reads the disk's GUID out of the GPT header on the parent device.
 *
 * The partition's own Block IO cannot see it: it addresses the partition,
 * and the header lives at LBA 1 of the whole disk. So the parent is found
 * by device path - the partition's path with its hard drive node removed
 * is exactly the disk's path - and read directly.
 *
 * Absence is not a failure. A raw FAT medium with no partition table has
 * no disk GUID to report, and the reservation does not depend on one.
 */
void read_disk_guid(const EFI_DEVICE_PATH * partition_path,
                    std::size_t prefix_bytes,
                    std::uint8_t (&into)[16])
{
    constexpr std::size_t gpt_header_sector = 1;
    constexpr std::size_t gpt_disk_guid_offset = 56;
    constexpr char gpt_signature[] = "EFI PART";

    std::size_t handle_count{};
    EFI_HANDLE * handles{};
    if (EFI_ERROR(g_boot_services->LocateHandleBuffer(ByProtocol,
                                                      &g_block_io_guid,
                                                      nullptr,
                                                      &handle_count,
                                                      &handles))) {
        return;
    }

    for (std::size_t i{}; i < handle_count; ++i) {
        EFI_DEVICE_PATH * candidate{};
        if (EFI_ERROR(g_boot_services->HandleProtocol(
                handles[i],
                &g_device_path_guid,
                reinterpret_cast<void **>(&candidate)))) {
            continue;
        }

        // The parent is the path that ends exactly where the hard drive
        // node began. Compared as bytes rather than node by node, since
        // an equal prefix of equal length is the whole test.
        std::size_t length{};
        for (auto * node = candidate;
             END_DEVICE_PATH_TYPE != node->Type;) {
            auto step = node_length(node);
            if (step < sizeof(EFI_DEVICE_PATH)) {
                length = 0;
                break;
            }
            length += step;
            node = reinterpret_cast<EFI_DEVICE_PATH *>(
                reinterpret_cast<std::uint8_t *>(node) + step);
        }
        if ((0 == length) || (length != prefix_bytes)) {
            continue;
        }
        if (std::memcmp(candidate, partition_path, prefix_bytes)) {
            continue;
        }

        EFI_BLOCK_IO_PROTOCOL * disk{};
        if (EFI_ERROR(g_boot_services->HandleProtocol(
                handles[i],
                &g_block_io_guid,
                reinterpret_cast<void **>(&disk)))) {
            continue;
        }
        if (!disk->Media || (disk->Media->BlockSize > max_block_size)) {
            continue;
        }
        if (EFI_ERROR(disk->ReadBlocks(disk,
                                       disk->Media->MediaId,
                                       gpt_header_sector,
                                       disk->Media->BlockSize,
                                       g_scratch))) {
            continue;
        }
        if (std::memcmp(
                g_scratch, gpt_signature, sizeof(gpt_signature) - 1)) {
            continue;
        }

        std::memcpy(into, g_scratch + gpt_disk_guid_offset, sizeof(into));
        break;
    }

    g_boot_services->FreePool(handles);
}

/**
 * Finds the volume this loader was loaded from and everything about it
 * that is not in the file system: where the partition starts and ends,
 * which namespace carries it, and the two GUIDs that name it.
 *
 * The partition start and size come from the device path's hard drive
 * node rather than from any assumption, because Block IO on a partition
 * handle is relative to the partition while the resident side's queue is
 * not - so the offset between them is the difference between writing to
 * the reservation and writing to whatever is that far into the namespace.
 */
std::expected<volume, zpp::error> identify_volume(EFI_HANDLE image_handle)
{
    EFI_LOADED_IMAGE_PROTOCOL * image{};
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            image_handle,
            &g_loaded_image_guid,
            reinterpret_cast<void **>(&image))) ||
        !image->DeviceHandle) {
        return std::unexpected(zpp::error{code::no_loaded_image});
    }

    volume found{};
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            image->DeviceHandle,
            &g_block_io_guid,
            reinterpret_cast<void **>(&found.block_io))) ||
        !found.block_io->Media) {
        return std::unexpected(zpp::error{code::no_block_io});
    }

    auto * media = found.block_io->Media;
    found.block_size = media->BlockSize;
    found.media_id = media->MediaId;
    if ((0 == found.block_size) || (found.block_size > max_block_size) ||
        (0 != (found.block_size & (found.block_size - 1)))) {
        return std::unexpected(zpp::error{code::unsupported_block_size});
    }
    found.partition_sectors = media->LastBlock + 1;

    EFI_DEVICE_PATH * path{};
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            image->DeviceHandle,
            &g_device_path_guid,
            reinterpret_cast<void **>(&path)))) {
        return std::unexpected(zpp::error{code::no_device_path});
    }

    std::size_t prefix_bytes{};
    bool found_hard_drive = false;
    for (auto * node = path; END_DEVICE_PATH_TYPE != node->Type;) {
        auto step = node_length(node);
        if (step < sizeof(EFI_DEVICE_PATH)) {
            break;
        }
        auto * bytes = reinterpret_cast<const std::uint8_t *>(node);

        if ((MESSAGING_DEVICE_PATH == node->Type) &&
            (MSG_NVME_NAMESPACE_DP == node->SubType) &&
            (step >= sizeof(NVME_NAMESPACE_DEVICE_PATH))) {
            found.namespace_id = load32(bytes + sizeof(EFI_DEVICE_PATH));
        }

        if ((MEDIA_DEVICE_PATH == node->Type) &&
            (MEDIA_HARDDRIVE_DP == node->SubType) &&
            (step >= sizeof(HARDDRIVE_DEVICE_PATH))) {
            // Offsets within HARDDRIVE_DEVICE_PATH, taken from the
            // structure in Protocol/DevicePath.h: the header, then the
            // partition number, start, size, signature, MBR type and
            // signature type. Read by offset for the alignment reason
            // load32 exists for - a device path node is byte packed and
            // PartitionStart is at offset 8 of a structure the firmware
            // placed wherever it liked.
            constexpr std::size_t start_offset = 8;
            constexpr std::size_t size_offset = 16;
            constexpr std::size_t signature_offset = 24;
            constexpr std::size_t signature_type_offset = 41;
            constexpr std::uint8_t signature_type_guid = 2;

            found.partition_start = load64(bytes + start_offset);

            // Both descriptions of the same partition have to be
            // satisfied, so the smaller wins. They agree on every sane
            // medium; taking the smaller means a disagreement cannot
            // produce a reservation that runs off the end of either.
            auto declared = load64(bytes + size_offset);
            if (declared && (declared < found.partition_sectors)) {
                found.partition_sectors = declared;
            }

            if (signature_type_guid == bytes[signature_type_offset]) {
                std::memcpy(found.partition_guid,
                            bytes + signature_offset,
                            sizeof(found.partition_guid));
            }

            found_hard_drive = true;
            prefix_bytes = static_cast<std::size_t>(
                bytes - reinterpret_cast<const std::uint8_t *>(path));
            break;
        }

        prefix_bytes += step;
        node = reinterpret_cast<EFI_DEVICE_PATH *>(
            reinterpret_cast<std::uint8_t *>(node) + step);
    }

    if (found_hard_drive) {
        read_disk_guid(path, prefix_bytes, found.disk_guid);
    } else {
        // A medium with no partition table at all - which is what the
        // Bochs and QEMU test image is, a bare FAT file system written
        // straight to the device. The partition then *is* the device, so
        // the start is zero and the size is the medium's. Said out loud
        // rather than defaulted, because "partition start is zero" is
        // the single assumption that would silently corrupt a real disk
        // if it were ever made rather than derived.
        trace::line("esp reservation: no hard drive node, the volume is "
                    "the whole device and starts at lba 0");
    }

    return found;
}

/**
 * Reads sector zero and believes it only after every check.
 */
std::expected<fat32, zpp::error> read_parameters(const volume & where)
{
    if (auto read = read_sectors(where, 0, 1, g_sector); !read) {
        return std::unexpected(read.error());
    }

    if (boot_signature != load16(g_sector + boot_signature_offset)) {
        return std::unexpected(zpp::error{code::not_fat32});
    }

    // The type field is advisory by the specification's own admission,
    // so it is a first filter rather than the answer. The cluster count
    // check below is what actually decides, and it is the rule the
    // specification gives.
    if (std::memcmp(g_sector + bs_file_system_type, "FAT32", 5)) {
        return std::unexpected(zpp::error{code::not_fat32});
    }

    fat32 layout{};
    layout.bytes_per_sector = load16(g_sector + bpb_bytes_per_sector);
    layout.sectors_per_cluster = g_sector[bpb_sectors_per_cluster];
    layout.reserved_sectors = load16(g_sector + bpb_reserved_sectors);
    layout.number_of_fats = g_sector[bpb_number_of_fats];
    layout.sectors_per_fat = load32(g_sector + bpb_sectors_per_fat_32);
    layout.total_sectors = load32(g_sector + bpb_total_sectors_32);
    layout.fs_info_sector = load16(g_sector + bpb_fs_info_sector);
    layout.backup_boot_sector = load16(g_sector + bpb_backup_boot_sector);

    // The two fields that must be zero on FAT32, and the reason this
    // refuses rather than coping with them. A non-zero 16 bit
    // sectors-per-FAT or root entry count means the volume is FAT12 or
    // FAT16 wearing a FAT32 type string, and a non-zero 16 bit total
    // means there are two totals - so writing one of them would leave a
    // volume that says two different things about its own size. Half an
    // answer is what this whole component exists to avoid.
    if (0 != load16(g_sector + bpb_sectors_per_fat_16)) {
        return std::unexpected(zpp::error{code::not_fat32});
    }
    if (0 != load16(g_sector + bpb_root_entry_count)) {
        return std::unexpected(zpp::error{code::not_fat32});
    }
    if (0 != load16(g_sector + bpb_total_sectors_16)) {
        return std::unexpected(zpp::error{code::bpb_inconsistent});
    }

    if (layout.bytes_per_sector != where.block_size) {
        return std::unexpected(zpp::error{code::sector_size_mismatch});
    }
    if ((0 == layout.sectors_per_cluster) ||
        (0 != (layout.sectors_per_cluster &
               (layout.sectors_per_cluster - 1)))) {
        return std::unexpected(zpp::error{code::bpb_inconsistent});
    }
    if ((0 == layout.reserved_sectors) || (0 == layout.number_of_fats) ||
        (0 == layout.sectors_per_fat) || (0 == layout.total_sectors)) {
        return std::unexpected(zpp::error{code::bpb_inconsistent});
    }

    // Which copy of the FAT is the live one. Bit 7 of the extended flags
    // means mirroring is disabled and the low four bits name the active
    // copy; otherwise all copies are kept in step and the first will do.
    auto flags = load16(g_sector + bpb_extended_flags);
    std::uint32_t active = 0;
    if (0 != (flags & 0x80)) {
        active = flags & 0xf;
        if (active >= layout.number_of_fats) {
            return std::unexpected(zpp::error{code::bpb_inconsistent});
        }
    }
    layout.active_fat_start =
        layout.reserved_sectors + (active * layout.sectors_per_fat);

    layout.first_data_sector =
        layout.reserved_sectors +
        (layout.number_of_fats * layout.sectors_per_fat);
    if (layout.total_sectors <= layout.first_data_sector) {
        return std::unexpected(zpp::error{code::bpb_inconsistent});
    }
    layout.cluster_count =
        (layout.total_sectors - layout.first_data_sector) /
        layout.sectors_per_cluster;

    // The specification's determination rule, and the only thing that
    // actually says what this volume is.
    if (layout.cluster_count < esp_reservation::minimum_clusters_left) {
        return std::unexpected(zpp::error{code::not_fat32});
    }

    if (layout.total_sectors > where.partition_sectors) {
        return std::unexpected(zpp::error{code::partition_too_small});
    }

    return layout;
}

/**
 * What one pass over the FAT found.
 */
struct fat_scan
{
    /**
     * How many of the clusters that would survive the shrink are free.
     * Counted so the shrink can refuse to leave a machine with nowhere
     * to write.
     */
    std::uint32_t free_kept{};

    /**
     * The first allocated cluster that would fall outside the new total,
     * or zero when there is none. **This is the check that must not be
     * skipped**: excluding a cluster that holds data orphans it without
     * a word, and on this partition that data is somebody's boot loader.
     */
    std::uint32_t first_in_use{};
};

/**
 * Walks the whole active FAT once.
 *
 * One pass rather than two, and chunked rather than a sector at a time,
 * because a sixteen gigabyte partition with four kilobyte clusters has a
 * sixteen megabyte FAT and this runs in a boot.
 */
std::expected<fat_scan, zpp::error> scan_fat(const volume & where,
                                             const fat32 & layout,
                                             std::uint32_t kept_clusters)
{
    fat_scan result{};

    auto entries_per_sector = layout.bytes_per_sector / 4;
    auto highest_cluster = layout.cluster_count + 1;
    auto first_excluded = kept_clusters + 2;
    auto sectors_needed =
        ((highest_cluster + 1) + entries_per_sector - 1) /
        entries_per_sector;
    if (sectors_needed > layout.sectors_per_fat) {
        return std::unexpected(zpp::error{code::bpb_inconsistent});
    }

    auto chunk_sectors =
        static_cast<std::uint32_t>(chunk_bytes / layout.bytes_per_sector);

    for (std::uint32_t at{}; at < sectors_needed; at += chunk_sectors) {
        auto count = sectors_needed - at;
        if (count > chunk_sectors) {
            count = chunk_sectors;
        }

        if (auto read = read_sectors(
                where, layout.active_fat_start + at, count, g_chunk);
            !read) {
            return std::unexpected(read.error());
        }

        auto first_entry = at * entries_per_sector;
        auto entries = count * entries_per_sector;
        for (std::uint32_t i{}; i < entries; ++i) {
            auto cluster = first_entry + i;
            if (cluster < 2) {
                continue;
            }
            if (cluster > highest_cluster) {
                break;
            }

            auto value = load32(g_chunk + (i * 4)) & 0x0fffffff;
            if (cluster >= first_excluded) {
                if ((0 != value) && (0 == result.first_in_use)) {
                    result.first_in_use = cluster;
                }
            } else if (0 == value) {
                ++result.free_kept;
            }
        }
    }

    return result;
}

/**
 * Invalidates a file system information sector's counts.
 *
 * Set to "unknown" rather than recomputed. The count is a hint a driver
 * is entitled to believe, and after the shrink the old figure counts
 * clusters that no longer exist - so leaving it would be handing a driver
 * a number that is wrong in the direction that makes it allocate. Nothing
 * requires the sector to be present or valid, so an absent one is not a
 * failure.
 */
void invalidate_fs_info(const volume & where, std::uint32_t sector)
{
    if ((0 == sector) || (0xffff == sector)) {
        return;
    }
    if (!read_sectors(where, sector, 1, g_scratch)) {
        return;
    }
    if ((fs_info_lead_signature !=
         load32(g_scratch + fs_info_lead_signature_offset)) ||
        (fs_info_struct_signature !=
         load32(g_scratch + fs_info_struct_signature_offset)) ||
        (fs_info_trail_signature !=
         load32(g_scratch + fs_info_trail_signature_offset))) {
        return;
    }

    store32(g_scratch + fs_info_free_count_offset, fs_info_unknown);
    store32(g_scratch + fs_info_next_free_offset, fs_info_unknown);
    if (write_sectors(where, sector, 1, g_scratch)) {
        trace::hex_line("esp reservation: fs info invalidated at sector ",
                        sector);
    }
}

/**
 * Puts right a free cluster count that describes a volume this size no
 * longer is.
 *
 * Called on the steady state path, and it exists because of something
 * observed rather than imagined: the firmware's FAT driver mounted the
 * volume before the shrink, and when it wrote a file later in that same
 * boot it rewrote the information sector with a count computed from its
 * stale, larger cluster map. That figure then sits on the medium
 * describing clusters that no longer exist, and it is not self
 * correcting - the next driver to mount believes what it finds. `mdir`
 * reported a gigabyte partition as having more free space than the file
 * system had clusters.
 *
 * Only ever set to "unknown", and only when the count is impossible.
 * A count that merely disagrees with the FAT is not this component's
 * business.
 */
void repair_fs_info(const volume & where, const fat32 & layout)
{
    auto sector = layout.fs_info_sector;
    if ((0 == sector) || (0xffff == sector)) {
        return;
    }
    if (!read_sectors(where, sector, 1, g_scratch)) {
        return;
    }
    if ((fs_info_lead_signature !=
         load32(g_scratch + fs_info_lead_signature_offset)) ||
        (fs_info_struct_signature !=
         load32(g_scratch + fs_info_struct_signature_offset)) ||
        (fs_info_trail_signature !=
         load32(g_scratch + fs_info_trail_signature_offset))) {
        return;
    }

    auto free_count = load32(g_scratch + fs_info_free_count_offset);
    if ((fs_info_unknown == free_count) &&
        (fs_info_unknown ==
         load32(g_scratch + fs_info_next_free_offset))) {
        return;
    }
    if ((free_count <= layout.cluster_count) &&
        (load32(g_scratch + fs_info_next_free_offset) <=
         (layout.cluster_count + 1))) {
        return;
    }

    trace::hex_line("esp reservation: stale free count, was ", free_count);
    store32(g_scratch + fs_info_free_count_offset, fs_info_unknown);
    store32(g_scratch + fs_info_next_free_offset, fs_info_unknown);
    if (write_sectors(where, sector, 1, g_scratch)) {
        where.block_io->FlushBlocks(where.block_io);
    }
}

/**
 * Writes the new total into the boot sector, its backup, and invalidates
 * both file system information sectors, then reads the result back.
 *
 * The backup matters as much as the primary. A repair tool that finds
 * them disagreeing restores the backup over the primary, which would put
 * the old, larger total back and hand the reservation to the allocator
 * with the resident side still writing into it.
 */
std::expected<void, zpp::error> shrink(const volume & where,
                                       const fat32 & layout,
                                       std::uint32_t new_total)
{
    if (auto read = read_sectors(where, 0, 1, g_sector); !read) {
        return std::unexpected(read.error());
    }
    store32(g_sector + bpb_total_sectors_32, new_total);
    if (auto written = write_sectors(where, 0, 1, g_sector); !written) {
        return std::unexpected(written.error());
    }

    auto backup = layout.backup_boot_sector;
    auto backup_usable = (0 != backup) && (0xffff != backup) &&
                         (backup < layout.reserved_sectors);
    if (backup_usable) {
        if (read_sectors(where, backup, 1, g_scratch) &&
            (boot_signature ==
             load16(g_scratch + boot_signature_offset)) &&
            (0 ==
             std::memcmp(g_scratch + bs_file_system_type, "FAT32", 5))) {
            store32(g_scratch + bpb_total_sectors_32, new_total);
            if (write_sectors(where, backup, 1, g_scratch)) {
                trace::hex_line(
                    "esp reservation: backup boot sector updated at ",
                    backup);
            }
        }
    }

    invalidate_fs_info(where, layout.fs_info_sector);
    if (backup_usable) {
        // The backup set is a copy of the whole reserved area's first
        // sectors, so the backup information sector sits immediately
        // after the backup boot sector.
        invalidate_fs_info(where, backup + 1);
    }

    where.block_io->FlushBlocks(where.block_io);

    // Read it back rather than trusting the write. A medium that
    // silently discards a write to sector zero - a read-only mount, a
    // write cache that never lands - would otherwise leave this boot
    // believing in a reservation that is not there, and the *next* boot
    // handing the resident side a range the allocator still owns.
    if (auto read = read_sectors(where, 0, 1, g_scratch); !read) {
        return std::unexpected(read.error());
    }
    if (new_total != load32(g_scratch + bpb_total_sectors_32)) {
        return std::unexpected(zpp::error{code::write_did_not_take});
    }

    return {};
}

/**
 * Whether the reserved range already carries the signature the resident
 * side's destination guard requires, and stamping it when it does not.
 *
 * The guard in `zpp/nvme/log_writer.h` reads its destination before every
 * write and refuses anything that is not already signed. That check is
 * what makes writing next to a boot manager acceptable, so it is not
 * being relaxed - the range is signed instead, once, in the first boot
 * that finds the reservation already in place.
 *
 * Only the first and last block are read to decide. A partially stamped
 * range can only come from this function being interrupted, and the last
 * block is the last thing it writes.
 */
std::expected<void, zpp::error> stamp(const volume & where,
                                      std::uint64_t region_start,
                                      std::uint64_t region_blocks,
                                      std::uint32_t sectors_per_block,
                                      const nvme::block_signature & model)
{
    auto signed_at = [&](std::uint64_t index) -> bool {
        if (!read_sectors(where,
                          region_start + (index * sectors_per_block),
                          1,
                          g_scratch)) {
            return false;
        }
        const auto * found =
            reinterpret_cast<const nvme::block_signature *>(g_scratch);
        return (nvme::block_signature::magic == found->signature_magic) &&
               (model.file_id == found->file_id) &&
               (index == found->block_index);
    };

    if (signed_at(0) && signed_at(region_blocks - 1)) {
        trace::line("esp reservation: range already signed");
        return {};
    }

    trace::line("esp reservation: signing the reserved range");

    auto blocks_per_chunk =
        static_cast<std::uint32_t>(chunk_bytes / nvme::block_size);
    for (std::uint64_t index{}; index < region_blocks;
         index += blocks_per_chunk) {
        auto blocks = region_blocks - index;
        if (blocks > blocks_per_chunk) {
            blocks = blocks_per_chunk;
        }

        std::memset(g_chunk, 0, blocks * nvme::block_size);
        for (std::uint64_t i{}; i < blocks; ++i) {
            auto stamped = model;
            stamped.block_index = index + i;
            std::memcpy(g_chunk + (i * nvme::block_size),
                        &stamped,
                        sizeof(stamped));
        }

        if (auto written = write_sectors(
                where,
                region_start + (index * sectors_per_block),
                static_cast<std::uint32_t>(blocks * sectors_per_block),
                g_chunk);
            !written) {
            return std::unexpected(written.error());
        }
    }

    where.block_io->FlushBlocks(where.block_io);

    // Proved by reading it back through the same path the resident side
    // will use, at the LBAs the target will name. Everything above is a
    // belief about the medium; this is the measurement.
    if (!signed_at(0) || !signed_at(region_blocks - 1)) {
        return std::unexpected(zpp::error{code::stamp_failed});
    }

    return {};
}

/**
 * Zeroes the handover structure in place.
 *
 * `memset` rather than `target = nvme::log_target{}`, and this is not a
 * style choice: the temporary that assignment materialises is the four
 * kilobytes of the extent table, which puts the enclosing frame over a
 * page - and a stack frame over one page makes the MSVC ABI emit a call
 * to `__chkstk`, which does not exist in a freestanding loader. It was a
 * link error the first time it was written the obvious way.
 */
void clear_target()
{
    std::memset(
        &esp_reservation::target, 0, sizeof(esp_reservation::target));
}

/**
 * The whole thing, as a chain of refusals.
 */
std::expected<void, zpp::error> establish(EFI_HANDLE image_handle)
{
    auto where = identify_volume(image_handle);
    if (!where) {
        return std::unexpected(where.error());
    }

    trace::hex_line("esp reservation: block size ", where->block_size);
    trace::hex_line("esp reservation: partition start lba ",
                    where->partition_start);
    trace::hex_line("esp reservation: partition sectors ",
                    where->partition_sectors);
    trace::hex_line("esp reservation: namespace id ", where->namespace_id);

    auto layout = read_parameters(*where);
    if (!layout) {
        return std::unexpected(layout.error());
    }

    trace::hex_line("esp reservation: sectors per cluster ",
                    layout->sectors_per_cluster);
    trace::hex_line("esp reservation: first data sector ",
                    layout->first_data_sector);
    trace::hex_line("esp reservation: filesystem total sectors ",
                    layout->total_sectors);
    trace::hex_line("esp reservation: data clusters ",
                    layout->cluster_count);

    auto wanted_sectors = static_cast<std::uint32_t>(
        esp_reservation::reservation_bytes / where->block_size);
    auto existing_gap = where->partition_sectors - layout->total_sectors;

    auto new_total = layout->total_sectors;
    auto shrank = false;

    if (existing_gap >= wanted_sectors) {
        // The steady state, and the reason this is safe to run every
        // boot: nothing is written, nothing is checked against the
        // allocator, and the answer is the same one as last time.
        trace::hex_line("esp reservation: already reserved, gap sectors ",
                        existing_gap);
        repair_fs_info(*where, *layout);
    } else {
        // Land the new total on a cluster boundary. A total that ends
        // part way through a cluster leaves the driver with a cluster it
        // counts and cannot fully address, and leaves us with a
        // reservation that overlaps one.
        auto target_total = static_cast<std::uint32_t>(
            where->partition_sectors - wanted_sectors);
        if ((where->partition_sectors < wanted_sectors) ||
            (target_total <= layout->first_data_sector)) {
            return std::unexpected(
                zpp::error{code::too_few_clusters_left});
        }

        auto kept_clusters = (target_total - layout->first_data_sector) /
                             layout->sectors_per_cluster;
        new_total = layout->first_data_sector +
                    (kept_clusters * layout->sectors_per_cluster);

        if (kept_clusters < esp_reservation::minimum_clusters_left) {
            return std::unexpected(
                zpp::error{code::too_few_clusters_left});
        }

        auto scan = scan_fat(*where, *layout, kept_clusters);
        if (!scan) {
            return std::unexpected(scan.error());
        }

        if (0 != scan->first_in_use) {
            trace::hex_line("esp reservation: cluster in use at ",
                            scan->first_in_use);
            return std::unexpected(zpp::error{code::region_in_use});
        }

        auto free_bytes = static_cast<std::uint64_t>(scan->free_kept) *
                          layout->sectors_per_cluster * where->block_size;
        trace::hex_line("esp reservation: free bytes left after shrink ",
                        free_bytes);
        if (free_bytes < esp_reservation::minimum_free_bytes) {
            return std::unexpected(zpp::error{code::too_little_free_left});
        }

        trace::hex_line("esp reservation: shrinking total sectors to ",
                        new_total);
        if (auto written = shrink(*where, *layout, new_total); !written) {
            return std::unexpected(written.error());
        }
        shrank = true;
    }

    esp_reservation::filesystem_total_sectors = new_total;
    esp_reservation::shrank_this_boot = shrank;

    if (shrank) {
        // Not this boot. The firmware's FAT driver mounted the volume
        // before this ran and its idea of the cluster count is the old
        // one, so for the rest of this boot it would still allocate
        // inside the range just reserved - and this loader writes a
        // trace file to this very partition later on. Next boot the
        // driver mounts a file system that ends where we left it and the
        // range is unreachable by construction.
        trace::line("esp reservation: reserved this boot, so the channel "
                    "stays off until the next one - the mounted file "
                    "system still believes it owns the range");
        return {};
    }

    auto sectors_per_block =
        static_cast<std::uint32_t>(nvme::block_size / where->block_size);
    if (0 == sectors_per_block) {
        return std::unexpected(zpp::error{code::unsupported_block_size});
    }

    // Whole blocks only. The resident side addresses the channel in
    // units of nvme::block_size, so a trailing part of one is not
    // addressable and is left out rather than rounded into.
    auto region_blocks =
        (where->partition_sectors - new_total) / sectors_per_block;
    if (0 == region_blocks) {
        return std::unexpected(zpp::error{code::too_few_clusters_left});
    }

    nvme::block_signature model{};
    model.signature_magic = nvme::block_signature::magic;
    model.file_id = esp_reservation::file_id;
    std::memcpy(
        model.disk_guid, where->disk_guid, sizeof(model.disk_guid));
    std::memcpy(model.partition_guid,
                where->partition_guid,
                sizeof(model.partition_guid));

    if (auto signed_range = stamp(
            *where, new_total, region_blocks, sectors_per_block, model);
        !signed_range) {
        return std::unexpected(signed_range.error());
    }

    auto & target = esp_reservation::target;
    clear_target();
    target.namespace_id = where->namespace_id;
    target.block_size = where->block_size;
    std::memcpy(
        target.disk_guid, where->disk_guid, sizeof(target.disk_guid));
    std::memcpy(target.partition_guid,
                where->partition_guid,
                sizeof(target.partition_guid));
    target.file_id = esp_reservation::file_id;
    target.extent_count = 1;
    target.extents[0].first_lba = where->partition_start + new_total;
    target.extents[0].block_count = region_blocks * sectors_per_block;

    // Last, so a structure that was half filled and then refused can
    // never read as usable. Everything above is either written or the
    // whole thing is left zeroed.
    // What the file system claimed when the reservation was made. The
    // resident side compares the BPB against this and refuses if the
    // file system has grown back over the region - the one thing the
    // block signature cannot catch, since a grown file system hands the
    // clusters to a new file whose data has not been written yet, so
    // our own signature is still sitting in them.
    target.filesystem_total_sectors =
        esp_reservation::filesystem_total_sectors;

    target.magic = nvme::log_target::valid_magic;

    trace::hex_line("esp reservation: reserved first lba ",
                    target.extents[0].first_lba);
    trace::hex_line("esp reservation: reserved lba count ",
                    target.extents[0].block_count);
    trace::hex_line("esp reservation: reserved blocks of 4k ",
                    region_blocks);
    return {};
}

} // namespace

void esp_reservation::execute(EFI_HANDLE image_handle,
                              EFI_SYSTEM_TABLE * system_table)
{
    g_boot_services = system_table->BootServices;

    trace::line("esp reservation: begin");

    if (auto result = zpp::establish(image_handle); !result) {
        // Left zeroed on every refusal, so `usable()` is false and the
        // resident side reads it as "no channel" rather than as a
        // channel pointing at LBA zero.
        zpp::clear_target();

        char buffer[trace::line_capacity]{};
        auto end =
            trace::append_text(buffer, "esp reservation: refused - ");
        auto message = result.error().message();
        for (auto character : message) {
            *end++ = character;
        }
        *end = 0;
        trace::line(buffer);
        return;
    }

    trace::line(target.usable() ? "esp reservation: channel ready"
                                : "esp reservation: no channel this boot");
}

} // namespace zpp

#endif
