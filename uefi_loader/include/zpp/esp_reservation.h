#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/diag/config.h"
#include "zpp/error.h"
#include "zpp/nvme/log_format.h"

#include <cstdint>

/**
 * Taking a range of logical blocks away from the EFI system partition's
 * file system, so the resident hypervisor has somewhere to write that no
 * file system can ever hand to anybody else.
 *
 * The resident side has no boot services, no FAT driver and no way to ask
 * where anything is. It has a queue pair, a namespace identifier and an
 * LBA. So the destination has to be established here and handed across as
 * numbers, which is what `zpp::nvme::log_target` is for.
 *
 * ## Why not a file
 *
 * The obvious answer - create a file, resolve it to extents, write to
 * those - was built and then abandoned, and the reason is worth keeping
 * because it is not obvious and it is not fixable:
 *
 * **Deleting a file does not touch its data blocks.** If anything deletes
 * the log file mid-boot, and clearing an EFI system partition that has
 * filled up is a documented practice, FAT frees the clusters while our
 * per-block signature is still sitting in them. The resident side's
 * destination guard reads that signature, concludes the block is still
 * ours, and keeps writing - into clusters FAT has meanwhile handed to
 * whatever a firmware update wrote next, which on this partition is a
 * boot manager. Re-validating the file system before every write narrows
 * that window and cannot close it: the delete can land between the
 * validation and the write.
 *
 * The failure mode is "corrupt the machine's boot loader", so narrowing
 * is not good enough.
 *
 * ## What this does instead
 *
 * FAT32's parameter block carries its own total sector count, and nothing
 * requires it to equal the partition it sits in. A file system smaller
 * than its partition is ordinary and every driver honours it, because the
 * count is what the driver derives its cluster count from. So the file
 * system is shrunk by the size of the reservation, leaving a tail inside
 * the partition that no cluster maps to.
 *
 * That tail cannot be allocated, deleted, moved, defragmented or handed
 * out, because as far as every FAT implementation is concerned it is not
 * part of the volume. It needs no extent map, no per-write revalidation
 * and no file. It is a fixed range of LBAs.
 *
 * ## What it costs, and the one thing that can undo it
 *
 * It costs the partition the space, permanently and visibly - the free
 * space a user sees drops by the reservation, which is honest.
 *
 * The single way it can be undone is something *growing* the file system
 * back to fill the partition, which a resizing tool would do. Then the
 * tail becomes allocatable again while the resident side still believes
 * it owns it. That is why the total sector count left behind is recorded
 * and handed over: the resident side can read the parameter block back
 * and refuse if it has changed. See `filesystem_total_sectors` for where
 * that field wants to live.
 *
 * ## Two boots to come up, deliberately
 *
 * The boot that performs the shrink does **not** hand over a usable
 * target. The firmware's FAT driver mounted the volume before this ran
 * and computed its cluster count from the old, larger total; writing a
 * new total behind its back does not tell it. For the rest of that boot
 * the driver would still happily allocate a cluster inside the range we
 * just reserved - and this loader itself writes a trace file to that
 * partition later in the same boot.
 *
 * So the shrink boot shrinks and reports, and nothing else. Every boot
 * after it finds the gap already there, mounts a file system that ends
 * where we left it, and hands over a target. This is also what makes the
 * whole thing idempotent: the steady state is "the gap is already large
 * enough", and that path writes nothing at all.
 *
 * Debug only, in the shape `zpp/nvme_selftest.h` uses. Gated on the
 * diagnostic facility as a whole rather than on the disk sink's own
 * policy row, because `zpp::diag::policy_of(sink::esp_blocks).present` is
 * `false` today and gating on it would compile this out of every build
 * and leave it never once executed. `zpp::sleep_control_finder` is gated
 * the same way, for the same reason.
 */
namespace zpp
{
/**
 * Every way this can refuse.
 *
 * Enumerated rather than collapsed into a bool because the refusals are
 * the interesting part. Each names one belief about the medium that
 * turned out to be false, and the trace prints the message - which is the
 * whole diagnosis on a machine where this is the only channel.
 */
enum class esp_reservation_error : int
{
    success,

    /**
     * The volume was never reached: the loader could not find out which
     * device it came from, or that device carries none of the protocols
     * this needs.
     * @{
     */
    no_loaded_image,
    no_device_path,
    no_block_io,
    /**
     * @}
     */

    /**
     * The medium's logical block is larger than the fixed buffers here.
     */
    unsupported_block_size,

    /**
     * Reading or writing the medium through Block IO failed outright.
     * @{
     */
    read_failed,
    write_failed,
    /**
     * @}
     */

    /**
     * Not FAT32. Either the boot sector is not one, or the volume is
     * FAT12/FAT16 - which this deliberately does not implement, because
     * their totals live in different fields and an EFI system partition
     * is FAT32 on anything this would run on.
     * @{
     */
    not_fat32,
    bpb_inconsistent,
    /**
     * @}
     */

    /**
     * The parameter block's bytes per sector is not the medium's logical
     * block size. Every sector number computed below would be in the
     * wrong unit.
     */
    sector_size_mismatch,

    /**
     * The file system claims more sectors than the partition holds. The
     * volume is already inconsistent and nothing here will improve it.
     */
    partition_too_small,

    /**
     * Shrinking by the wanted amount would leave a file system that is
     * no longer FAT32 by the specification's own determination rule, or
     * one with less free space than is sane to leave a machine with.
     * @{
     */
    too_few_clusters_left,
    too_little_free_left,
    /**
     * @}
     */

    /**
     * A cluster that would fall outside the new total is allocated.
     * Excluding it would orphan whatever it holds without a word, and on
     * this partition that is somebody's boot loader. Nothing is written.
     */
    region_in_use,

    /**
     * The new total was written and did not read back as written.
     */
    write_did_not_take,

    /**
     * The reserved range is not carrying the signature the resident
     * side's destination guard requires, and stamping it did not fix
     * that.
     */
    stamp_failed,

    /**
     * The total about to be written is not a shrink of at most the
     * configured amount.
     *
     * A backstop, not a condition anything is expected to reach. The
     * total is computed from the partition's size rather than from the
     * file system's current one, which makes the result the same
     * absolute number however many times this runs - a second boot
     * cannot take a second bite. This checks that property immediately
     * before the only write that could violate it, so that a future
     * change which breaks it fails loudly on the first boot instead of
     * eating the volume sixty four megabytes at a time.
     */
    shrink_out_of_bounds,
};

/**
 * The category for the enumeration above.
 */
inline const zpp::error_category & category(esp_reservation_error)
{
    constexpr static auto error_category = zpp::make_error_category(
        "esp_reservation",
        esp_reservation_error::success,
        [](auto code) -> std::string_view {
            switch (code) {
            case esp_reservation_error::success:
                return zpp::error::no_error;
            case esp_reservation_error::no_loaded_image:
                return "No loaded image protocol on our own handle.";
            case esp_reservation_error::no_device_path:
                return "The boot volume has no device path.";
            case esp_reservation_error::no_block_io:
                return "The boot volume has no block IO protocol.";
            case esp_reservation_error::unsupported_block_size:
                return "The medium's logical block is too large.";
            case esp_reservation_error::read_failed:
                return "Reading the medium failed.";
            case esp_reservation_error::write_failed:
                return "Writing the medium failed.";
            case esp_reservation_error::not_fat32:
                return "The boot volume is not FAT32.";
            case esp_reservation_error::bpb_inconsistent:
                return "The FAT32 parameter block contradicts itself.";
            case esp_reservation_error::sector_size_mismatch:
                return "Bytes per sector is not the media block size.";
            case esp_reservation_error::partition_too_small:
                return "The file system claims more than the partition.";
            case esp_reservation_error::too_few_clusters_left:
                return "Shrinking would stop the volume being FAT32.";
            case esp_reservation_error::too_little_free_left:
                return "Shrinking would leave too little free space.";
            case esp_reservation_error::region_in_use:
                return "A cluster in the region to exclude is in use.";
            case esp_reservation_error::write_did_not_take:
                return "The new total sector count did not read back.";
            case esp_reservation_error::stamp_failed:
                return "The reserved range could not be signed.";
            case esp_reservation_error::shrink_out_of_bounds:
                return "The new total is not a shrink of at most the "
                       "configured amount.";
            default:
                return "Unknown error.";
            }
        });
    return error_category;
}

/**
 * The reservation at the end of the EFI system partition.
 */
struct esp_reservation
{
    /**
     * Whether this build carries it. See the file comment for why this
     * is the facility's own switch rather than the disk sink's row.
     */
    static constexpr bool enabled = diag::enabled;

    /**
     * How much to take, in bytes, and the least worth taking.
     *
     * Both come from the diagnostic configuration rather than being
     * spelled here, because that is where everything else about this
     * facility is decided.
     *
     * The first is a request. A partition that cannot give it gives what
     * it can, down to the second - a smaller log is worth having, and a
     * machine that declines to log at all because it could not spare
     * sixty four megabytes is worth nothing.
     * @{
     */
    static constexpr std::uint64_t reservation_bytes =
        std::uint64_t{diag::esp_reservation_megabytes} * 1024 * 1024;

    static constexpr std::uint64_t minimum_reservation_bytes =
        std::uint64_t{diag::esp_reservation_minimum_megabytes} * 1024 *
        1024;
    /**
     * @}
     */

    /**
     * The fewest data clusters the file system may be left with.
     *
     * Not a comfort margin - this is the specification's own rule for
     * what a volume *is*. A FAT volume's type is determined by its
     * cluster count and nothing else, and below 65525 clusters it is
     * FAT16. Shrinking past this line would not produce a smaller FAT32
     * volume, it would produce a volume every driver reads with the
     * wrong FAT entry width. Refused.
     */
    static constexpr std::uint32_t minimum_clusters_left = 65525;

    /**
     * The least free space worth leaving a machine with. Firmware
     * updates, boot managers and this loader's own trace file all write
     * here.
     */
    static constexpr std::uint64_t minimum_free_bytes =
        16ull * 1024 * 1024;

    /**
     * Which channel the block signatures claim to belong to.
     *
     * Stable across boots and derived from nothing, because it has to
     * mean the same to the loader that stamps it and the resident side
     * that checks it. "ZPPLOG01" as bytes.
     */
    static constexpr std::uint64_t file_id = 0x3130474f4c50505aull;

    /**
     * What was established, or a zeroed structure when anything refused
     * or when this is the boot that performed the shrink.
     *
     * Static so the address handed across the launch boundary outlives
     * this loader's frames, and constant initialized so it costs no
     * `.init_array` entry.
     */
    static inline constinit nvme::log_target target{};

    /**
     * The total sector count the file system was left claiming.
     *
     * **This wants to be in `zpp::nvme::log_target`, and is not, because
     * that header is not ours to edit.** What the resident side needs is
     * one field beside `block_size`:
     *
     *     std::uint32_t filesystem_total_sectors{};
     *
     * with the sense "the reservation begins where the file system ends,
     * and is only valid while the file system still ends there". The
     * resident side reads sector zero of the partition, takes the 32 bit
     * total at offset 32, and refuses the channel if it is larger than
     * this. That is the whole check, and it is what catches a resizing
     * tool having grown the volume back over the reservation between one
     * boot and the next.
     *
     * Exposed here in the meantime so the check can be wired up without
     * this component changing.
     */
    static inline constinit std::uint32_t filesystem_total_sectors{};

    /**
     * Whether this boot is the one that performed the shrink.
     *
     * When set, `target` is deliberately left unusable - see the file
     * comment on why a reservation is not usable in the boot that
     * created it.
     */
    static inline constinit bool shrank_this_boot{};

    /**
     * Establishes the reservation, or refuses, reporting through
     * `zpp::trace`.
     *
     * Never fails the boot: every refusal is traced and `target` is left
     * zeroed, because a diagnostic that can stop a machine booting is
     * worse than no diagnostic.
     *
     * Takes the image handle rather than a device handle so the call
     * site needs no setup - which volume this loader came from is the
     * one thing it can always find out about itself.
     */
    static void establish(EFI_HANDLE image_handle,
                          EFI_SYSTEM_TABLE * system_table)
    {
        if constexpr (enabled) {
            execute(image_handle, system_table);
        } else {
            static_cast<void>(image_handle);
            static_cast<void>(system_table);
        }
    }

private:
    /**
     * The implementation, out of line so the header costs nothing.
     * Defined only when enabled - see esp_reservation.cpp.
     */
    static void execute(EFI_HANDLE image_handle,
                        EFI_SYSTEM_TABLE * system_table);
};

} // namespace zpp
