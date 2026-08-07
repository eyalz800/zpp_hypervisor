#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/ring.h"
#include "zpp/diag/sink.h"
#include "zpp/nvme/log_format.h"
#include "zpp/nvme/log_writer.h"

#include <cstddef>
#include <cstdint>

namespace zpp::diag
{
/**
 * The boot disk, as raw blocks, through a driver of our own.
 *
 * The resident side's only route to a medium that survives the machine
 * being powered off, and the only channel here that a program can read
 * without a cable. NVME-LOG.md is the design; this is the part of it that
 * plugs into the facility, and it is deliberately the smallest part.
 *
 * The shape follows sinks/counter.h exactly, for the reason that file
 * gives: a static data member of a class template is emitted only if it
 * is odr-used, and the only use is inside `write`, which is only
 * instantiated if the fold in pump.h reaches it. So a build with this
 * channel switched off does not carry a staging buffer nobody fills - it
 * does not carry one at all. That matters more here than for the counter,
 * because the storage is 8 KB of queues plus two 4 KB buffers.
 *
 * What this class does and does not own:
 *
 * - It owns **assembling blocks**: packing records, filling the header,
 *   the checksum, and deciding when a block is due.
 * - It owns nothing about NVMe. Queues, doorbells, the guard read and the
 *   signature check are all in zpp::nvme::queue_pair, so the question
 *   "could this write to the wrong place" is answered in one file.
 * - It does not bring the channel up. Bringing it up means borrowing the
 *   guest's admin queue, which needs the doorbell trap, which needs the
 *   EPT violation handler - none of which is wired into the exit path
 *   yet. Until it is, `ready()` answers false and this costs nothing.
 */
template <sink Which>
struct esp_blocks_for
{
    static constexpr sink id = Which;

    /**
     * How deep our private queue is. 64 entries is one page of
     * submission queue exactly, which is why it is 64 and not a round
     * number - see the PRP note in log_writer.h.
     */
    static constexpr std::uint32_t queue_entries = 64;

    using queues = nvme::queue_pair<queue_entries>;

    /**
     * Where the loader said the file is. Zero until it is handed over,
     * and `usable()` is false while it is zero - so a build that never
     * receives one writes nowhere rather than writing to LBA 0.
     *
     * **This is the wiring point for the loader hand-over.** Nothing
     * assigns it yet.
     */
    static inline nvme::log_target target{};

    /**
     * Which controller epoch the queues belong to. Bumped by the reset
     * detection, which is the second wiring point and also absent.
     */
    static inline std::uint32_t epoch{};

    /**
     * Turns one of our buffers into the address the controller uses.
     * The identity while boot services are alive, a host page table
     * lookup once resident. Supplied rather than assumed, because
     * getting it wrong means DMA into somebody else's memory.
     *
     * The third wiring point.
     */
    static inline std::uint64_t (*physical_of)(const void *){};

    /**
     * How long a synchronous read may spin before the channel gives up
     * on it. A count rather than a duration, because there is no clock
     * here that a VM exit handler may block on.
     */
    static inline std::uint64_t spin_budget{1u << 22};

    /**
     * The block being filled, and where in the file it goes.
     * @{
     */
    static inline std::uint32_t staged_records{};

    /**
     * When the block being filled stops being allowed to wait, as a time
     * stamp counter reading. Zero while no block is being filled.
     *
     * A block is written when it fills, and a partial one would
     * otherwise sit unwritten for as long as the guest stayed quiet -
     * which is exactly backwards, since a guest that has gone quiet is
     * the case the log exists for. This is the bound on that wait.
     */
    static inline std::uint64_t staged_deadline{};

    /**
     * How long a partial block may wait, in time stamp counter ticks.
     *
     * A count rather than a duration, because nothing here knows the
     * counter's frequency and there is no clock a VM exit handler may
     * ask. Ten million ticks is a few milliseconds on any processor this
     * runs on - the requirement is "every few milliseconds", and being
     * wrong by a factor of two in either direction costs nothing.
     */
    static constexpr std::uint64_t staged_deadline_ticks = 10'000'000;
    static inline std::uint64_t next_block_index{};
    static inline std::uint64_t sequence{};
    static inline std::uint64_t boot_id{};
    /**
     * @}
     */

    /**
     * Records that did not fit because the device was behind. Counted
     * here and reported in the next block's header, so that falling
     * behind is visible to the reader rather than only to a debugger.
     */
    static inline std::uint64_t dropped{};

    /**
     * How many records one block holds. Derived rather than stated, so
     * the two cannot disagree.
     */
    static constexpr std::size_t records_per_block()
    {
        return nvme::block_payload / ring.record_size;
    }

    static_assert(records_per_block() >= 1,
                  "a record has to fit in a block with its header");

    /**
     * Takes the loader's hand-over and makes the channel live.
     *
     * The three wiring points above are filled here and nowhere else:
     * the target the loader validated, the queue pair it created, and
     * the address translation the resident side supplies for itself.
     *
     * Refuses a hand-over that does not check out rather than partially
     * applying it, so `ready()` can never become true on half a channel.
     * Returns whether the channel is now live.
     */
    static bool configure(const nvme::channel_handover & handover,
                          std::uint64_t (*translate)(const void *))
    {
        if (!handover.usable() || (nullptr == translate)) {
            return false;
        }

        // The same storage the loader created the queues against, not a
        // second copy of it. Two copies is what this used to be, and the
        // controller only ever knew about one of them - see the comment
        // on queue_pair's pointers.
        if (!queues::bind_storage(handover.queue_storage)) {
            return false;
        }

        // And where those queues stand, which is the other half of
        // inheriting them.
        queues::bind_position(handover.submission_tail,
                              handover.completion_head,
                              0 != handover.completion_phase);

        target = handover.target;
        physical_of = translate;

        queues::bound = typename queues::binding{
            .submission_doorbell = handover.submission_doorbell,
            .completion_doorbell = handover.completion_doorbell,
            .status_register = handover.status_register,
            .configuration_register = handover.configuration_register,
            .submission_id = handover.submission_id,
            .completion_id = handover.completion_id,
            .namespace_id = handover.namespace_id,
            .epoch = ++epoch,
        };

        return ready();
    }

    /**
     * Called when the guest has written the controller's configuration
     * register, which is the only way this side learns about a reset.
     *
     * A reset destroys every I/O queue on the controller, ours included,
     * and the guard read cannot infer that on its own: a *completed*
     * reset leaves CSTS.RDY set and CC.EN set, exactly as they were
     * before it. It catches a controller that is down and not one that
     * went down and came back, so without this the channel would go on
     * ringing a doorbell the controller no longer backs.
     *
     * The watch fires after the guest's write has been stepped over, so
     * the register read here is the value the guest just wrote.
     * Clearing CC.EN is the reset; everything else on this page - the
     * admin queue base registers, the doorbell stride - is the driver
     * setting itself up and is not our business.
     */
    static void note_controller_write()
    {
        if (!queues::bound.live()) {
            return;
        }

        auto configuration = nvme::controller_configuration{
            arch::x86_64::read32(queues::bound.configuration_register)};
        if (configuration.enable()) {
            return;
        }

        // Forget rather than try to survive it. What was outstanding is
        // counted into lost_to_reset, so the loss reaches the next
        // block's header instead of being silent, and ready() answers
        // false from here on. Recreating the queue means borrowing the
        // admin queue from a live guest driver, which is a larger job -
        // see BACKLOG.md.
        queues::forget();
    }

    /**
     * Whether the channel can be used at all in this boot.
     *
     * Four things have to be true, and every one of them is somebody
     * else's job to establish: the loader found and validated the file,
     * the IOMMU gate allowed it, the queue pair was created, and we know
     * how to turn a buffer into a physical address. Any of them missing
     * and the pump does not offer records here, which is what keeps this
     * inert until it is deliberately wired up.
     */
    static bool ready()
    {
        // The storage check is not redundant with the binding check,
        // even though configure binds the storage first and would not
        // set the binding if that failed. write() forms
        // `staging + offset` and stores through it, so a null here is
        // not a refusal - it is a write into low memory. Anything whose
        // failure mode is that severe gets checked where it is used and
        // not inferred from something nearby.
        return target.usable() && queues::bound.live() &&
               queues::storage_bound() && (nullptr != physical_of);
    }

    /**
     * Takes one record into the block being assembled, and submits the
     * block when it is full.
     *
     * Returns true whenever the record was taken. A full staging block
     * that cannot be submitted is a **drop**, counted and reported in
     * band, rather than a refusal - because a refusal would make the
     * pump mark this sink dead after a handful of them, and a device
     * that is momentarily behind is not a dead device.
     */
    static bool write(const record & entry)
    {
        if (!ready()) {
            return false;
        }

        auto offset = sizeof(nvme::block_header) +
                      (staged_records * ring.record_size);
        auto * slot = queues::staging + offset;
        const auto * source =
            reinterpret_cast<const std::uint8_t *>(&entry);
        for (std::size_t i{}; i < ring.record_size; ++i) {
            slot[i] = source[i];
        }
        if (1 == ++staged_records) {
            staged_deadline = timestamp() + staged_deadline_ticks;
        }

        if (staged_records >= records_per_block()) {
            flush();
        }
        return true;
    }

    /**
     * Submits the block being assembled, however full it is.
     *
     * Called when a block fills, and it is also the entry point a timer
     * or the halt path would use to bound how long a partial block can
     * sit unwritten - "every few milliseconds" is this, called from the
     * exit path once a deadline has passed. That deadline is the fourth
     * wiring point.
     */
    /**
     * Writes the block being filled if it has waited long enough.
     *
     * Called from the exit path on every pass, so the cost in the common
     * case is one time stamp read and a comparison. That is deliberate:
     * the alternative is a periodic exit of our own, which would cost
     * the guest something on every machine to serve a facility that is
     * off in release.
     *
     * It does mean the bound is only honoured while the guest is taking
     * exits at all. A guest that has stopped entirely stops flushing -
     * but a guest that has stopped entirely is also one whose last
     * records are about to be drained by a halt path, which empties the
     * ring regardless.
     */
    static void flush_if_due()
    {
        if ((0 != staged_records) && (0 != staged_deadline) &&
            (timestamp() >= staged_deadline)) {
            flush();
        }
    }

    static void flush()
    {
        if (0 == staged_records) {
            return;
        }

        auto * header =
            reinterpret_cast<nvme::block_header *>(queues::staging);
        *header = nvme::block_header{};

        // The signature the destination guard will demand of this block
        // the next time round, put back as the reservation stamped it.
        // A block that came out of here without one could be written
        // once and never again.
        header->signature.signature_magic = nvme::block_signature::magic;
        header->signature.file_id = target.file_id;
        header->signature.block_index = next_block_index;
        for (std::size_t i{}; i < sizeof(target.disk_guid); ++i) {
            header->signature.disk_guid[i] = target.disk_guid[i];
            header->signature.partition_guid[i] = target.partition_guid[i];
        }

        header->block_magic = nvme::block_header::magic;
        header->boot_id = boot_id;
        header->epoch = epoch;
        header->sequence = sequence;
        header->block_index = next_block_index;
        staged_deadline = 0;
        header->record_count = staged_records;
        header->record_size = ring.record_size;

        auto & position = reader<Which>::position;
        header->records_lost = position.lost;
        header->records_refused = position.refused;
        header->blocks_dropped = dropped;
        header->blocks_lost_to_reset = queues::lost_to_reset;

        // Over everything after the checksum field itself, so that a
        // torn write - which loses a tail - fails it.
        header->checksum = nvme::checksum_of(
            reinterpret_cast<const std::uint32_t *>(queues::staging) +
                (sizeof(nvme::block_header) / sizeof(std::uint32_t)),
            (nvme::block_size - sizeof(nvme::block_header)) /
                sizeof(std::uint32_t));

        auto result = queues::submit(
            target, next_block_index, epoch, physical_of, spin_budget);

        staged_records = 0;
        if (nvme::write_result::ok != result) {
            ++dropped;
            return;
        }

        ++sequence;
        next_block_index = (next_block_index + 1) % blocks_in_file();
    }

    /**
     * How many whole record blocks the file holds. The channel wraps
     * inside the file and never past it, and the file's size never
     * changes - so this is fixed for the boot.
     */
    static std::uint64_t blocks_in_file()
    {
        auto per_block = nvme::block_size / target.block_size;
        auto total = target.total_blocks() / per_block;
        return (0 == total) ? 1 : total;
    }
};

using esp_block_sink = esp_blocks_for<sink::esp_blocks>;

} // namespace zpp::diag
