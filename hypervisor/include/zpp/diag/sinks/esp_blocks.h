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
     * How long the *waiting* paths may spin: the flush taken before the
     * guest disables the controller, and the drain behind it. A count
     * rather than a duration, because there is no clock here that a VM
     * exit handler may block on.
     *
     * The steady-state write path does not use it and no longer spins at
     * all. It used to, and that was the whole failure: a budget in polls
     * is a different amount of time in every build, and once the guest's
     * driver was using the controller no destination read came back
     * inside it. Every block was then discarded even though every read
     * completed a moment later - 1975 discarded against 65 submissions
     * with nothing failed and nothing refused.
     */
    static inline std::uint64_t spin_budget{1u << 22};

    /**
     * The block being filled, and where in the file it goes.
     * @{
     */
    // The reasons configure() can turn a hand-over down, in the order
    // it checks them.
    enum class reject : std::uint32_t
    {
        untried = 0,
        none,
        magic,
        target_unusable,
        storage_unusable,
        no_translator,
        bind_storage,
        not_ready,
    };

    // Volatile because nothing in this program ever reads it.
    //
    // Without that the stores are provably dead and the optimizer removes
    // them, leaving the symbol in .bss reading its zero initializer for
    // ever - which is exactly what happened: the field reported untried
    // on a run where epoch, boot_id and physical_of all proved configure()
    // had succeeded. An instrumentation field whose only reader is a
    // debugger has to say so, or it measures nothing and says it
    // confidently.
    static inline volatile reject configure_reject{reject::untried};

    static inline std::uint32_t staged_records{};

    /**
     * What `staged_records` was when the block's header was last built,
     * and zero when no block is in flight. See flush().
     */
    static inline std::uint32_t header_records{};

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
     * Blocks that did not go out. Counted here and reported in the next
     * block's header, so that falling behind is visible to the reader
     * rather than only to a debugger.
     *
     * One per *failed* attempt. A block whose destination read is still
     * out on the device is not counted: it has not failed, it is waiting,
     * and it stays staged until it goes out. Which of the failures it was
     * is in `queues::results`, indexed by the write_result enumerator -
     * this counter has never been able to say, and reading it as though
     * it could is what cost the run that saw 1975 of them.
     */
    static inline std::uint64_t dropped{};

    /**
     * Records thrown away because the block they belonged in was full and
     * still waiting on its destination read.
     *
     * Records rather than blocks, so it is a different unit from
     * `dropped` and a different unit from the ring's own `lost`. The pump
     * cannot be told "not now" - four refusals in a row and it marks this
     * sink dead for the boot, see failures_tolerated in diag/config.h -
     * so a record that arrives while a block is stuck is taken and
     * dropped rather than refused.
     */
    static inline std::uint64_t records_dropped{};

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
        // Why a hand-over was turned down, for a reader that has no
        // other channel.
        //
        // Worth a static of its own: from outside, a sink that never
        // configured and a sink that configured and was later forgotten
        // look identical - both answer ready() false and leave the
        // counters at zero - and they need opposite fixes. Cheap enough
        // to keep, since it is one store on a path that runs once.
        configure_reject = reject::none;

        if (!handover.usable()) {
            // Split by hand rather than asking the hand-over, because
            // usable() is one expression and its answer alone does not
            // say which half failed - and the two mean different things:
            // a wrong magic is a hand-over that never happened, an
            // unusable target or storage is one that happened and did
            // not survive.
            if (nvme::channel_handover::valid_magic != handover.magic) {
                configure_reject = reject::magic;
            } else if (!handover.target.usable()) {
                configure_reject = reject::target_unusable;
            } else {
                configure_reject = reject::storage_unusable;
            }
            return false;
        }

        if (nullptr == translate) {
            configure_reject = reject::no_translator;
            return false;
        }

        // The same storage the loader created the queues against, not a
        // second copy of it. Two copies is what this used to be, and the
        // controller only ever knew about one of them - see the comment
        // on queue_pair's pointers.
        if (!queues::bind_storage(handover.queue_storage)) {
            configure_reject = reject::bind_storage;
            return false;
        }

        // And where those queues stand, which is the other half of
        // inheriting them.
        queues::bind_position(handover.submission_tail,
                              handover.completion_head,
                              0 != handover.completion_phase);

        target = handover.target;
        physical_of = translate;

        // Stamp this boot, so its blocks can be told from the ones
        // already on the medium.
        //
        // The region is never erased - it is written in place and wraps
        // - so a reader looking at it sees blocks from every boot that
        // ever wrote, and the sequence numbers restart from zero each
        // time. Without something per boot they cannot be ordered or
        // even separated, which is the whole reason the field exists.
        // It was declared and written into every header and never
        // assigned, so every block ever written carried boot zero.
        //
        // The time stamp counter is the only thing available here that
        // differs between boots: there is no clock, no random source and
        // no storage that survives. It is not unique in principle - two
        // machines could agree - but the disk GUID in the signature
        // already says which machine, so this only has to separate boots
        // of one, and a counter that has been running since power on
        // does that.
        boot_id = timestamp();

        // A boot that somehow reads zero would be indistinguishable from
        // the unset case this is fixing, so it is nudged rather than
        // left ambiguous.
        if (0 == boot_id) {
            boot_id = 1;
        }

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

        if (!ready()) {
            configure_reject = reject::not_ready;
            return false;
        }

        return true;
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
    static void note_controller_write(std::uint32_t written)
    {
        if (!queues::bound.live()) {
            return;
        }

        // The value the guest wrote, handed in, rather than read back
        // from the device.
        //
        // Reading it back is a race with the guest, and it is the same
        // race the caller was rewritten to remove: a driver writes the
        // configuration register twice in succession, disable then
        // enable, and a read that lands after the second one sees the
        // controller enabled and concludes no reset happened. The caller
        // decodes the instruction and therefore knows what was written;
        // there is no reason to ask the device and every reason not to.
        //
        // It still works when the caller falls back to stepping over the
        // write, because the fallback reads the register once, at the
        // same point, and passes that - one source of truth either way
        // rather than two that can disagree.
        auto configuration = nvme::controller_configuration{written};
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
     * Takes a queue pair that has just been created again, after the
     * guest's reset destroyed the last one.
     *
     * The target, the storage and the translation hook all survive a
     * reset - they are properties of the medium and of memory nobody
     * reclaimed - so only the controller's side of it is rebuilt: the
     * doorbells, the identifiers, and a position of nothing, because the
     * queues are new and empty.
     *
     * The epoch moves, which is what makes any write still in flight
     * against the old queue refuse itself rather than ring a doorbell the
     * controller has forgotten.
     *
     * The two identifiers are separate parameters because they are
     * separate spaces, and after a guest has configured the controller
     * they genuinely differ. Measured on the rig: Windows creates
     * completion queues 1 to 8 and submission queues 1 to 16, pairing two
     * submission queues onto each completion queue, so the first free
     * identifier is 9 in one space and 17 in the other. They were one
     * parameter while the only other user of this controller was firmware
     * that created a single queue and numbered it 1.
     */
    static void adopt_rebuilt_queue(volatile std::uint8_t * bar,
                                    std::uint32_t stride,
                                    std::uint16_t submission_id,
                                    std::uint16_t completion_id,
                                    std::uint32_t namespace_id)
    {
        auto doorbell = [&](std::uint32_t index)->volatile void *
        {
            return bar +
                   nvme::offset_of(nvme::register_offset::doorbell_base) +
                   (index * (4u << stride));
        };

        queues::bound = typename queues::binding{
            .submission_doorbell = doorbell(2u * submission_id),
            .completion_doorbell = doorbell((2u * completion_id) + 1u),
            .status_register =
                bar + nvme::offset_of(nvme::register_offset::status),
            .configuration_register =
                bar +
                nvme::offset_of(nvme::register_offset::configuration),
            .submission_id = submission_id,
            .completion_id = completion_id,
            .namespace_id = namespace_id,
            .epoch = ++epoch,
        };

        // New queues, so nothing is outstanding and the first completion
        // will carry phase one. forget() already zeroed the completion
        // ring, which is what makes that safe to assume.
        queues::bind_position(0, 0, true);
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
     * Returns true whenever the record was taken, **and also when it was
     * dropped** - a refusal would make the pump mark this sink dead after
     * a handful of them, and a device that is momentarily behind is not a
     * dead device. What was dropped is counted instead: blocks in
     * `dropped`, records in `records_dropped`.
     */
    static bool write(const record & entry)
    {
        if (!ready()) {
            return false;
        }

        // A full block that has not gone out yet holds the staging
        // buffer, so there is nowhere to put this record until it does.
        //
        // Retried first, because the retry is what makes the block leave:
        // its destination read was issued on an earlier pass and this is
        // where it is collected. Only if it is still out on the device is
        // the record dropped - and dropped rather than refused, because
        // four refusals in a row take this sink off the pump for the rest
        // of the boot.
        if (staged_records >= records_per_block()) {
            flush();
            if (staged_records >= records_per_block()) {
                ++records_dropped;
                return true;
            }
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
     * Writes the block being filled if it has waited long enough, and
     * carries on a block that is waiting on the device.
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
        if (0 == staged_records) {
            return;
        }

        // A block that filled and has not gone out is retried on every
        // pass rather than on a deadline. The deadline bounds how long a
        // *partial* block may wait for records that may never come; a
        // full one is only waiting for the device, and the sooner its
        // destination read is collected the sooner it leaves.
        if (staged_records >= records_per_block()) {
            flush();
            return;
        }

        if ((0 != staged_deadline) && (timestamp() >= staged_deadline)) {
            flush();
        }
    }

    /**
     * Writes whatever is staged, now, and waits for it to land.
     *
     * For the moment before the guest takes the controller away: a write
     * to its register page can be the one that clears CC.EN, and after
     * that the queues are gone and anything staged is only countable, not
     * writable. Draining as well as flushing matters because a submitted
     * command whose completion is never reaped is a block the medium may
     * not have.
     */
    static void flush_pending()
    {
        if (!ready()) {
            return;
        }

        if (0 != staged_records) {
            // The waiting form here and only here. Coming back on a later
            // pass is the right answer everywhere else; on this path there
            // is no later pass, because the register write that brought us
            // here can be the one that takes the queues away.
            flush(true);
        }

        queues::drain(spin_budget);
    }

    /**
     * Writes the block being filled.
     *
     * `wait` decides what happens when the destination read has not come
     * back yet: false leaves the block staged and returns, so the next
     * pass collects it, and true spins for it because the caller has no
     * next pass. Leaving it staged is the steady-state answer - the write
     * path runs inside a VM exit handler and the guest is waiting.
     */
    static void flush(bool wait = false)
    {
        if (0 == staged_records) {
            return;
        }

        // Built once per shape of the block, not once per attempt.
        //
        // A retry while the destination read is out changes nothing about
        // the block, and rebuilding it would re-checksum four kilobytes -
        // very nearly a thousand words - on every VM exit for the whole of
        // the wait. That cost lands on the guest's exit path, which is the
        // one place in this program that has to stay cheap.
        //
        // `header_records` is what `staged_records` was when the header
        // was last built, and zero when there is no block in flight. So a
        // partial block that took another record on while its read was out
        // is rebuilt, and a full one that is only waiting is not.
        if (staged_records != header_records) {
            auto * header =
                reinterpret_cast<nvme::block_header *>(queues::staging);
            *header = nvme::block_header{};

            // The signature the destination guard will demand of this
            // block the next time round, put back as the reservation
            // stamped it. A block that came out of here without one could
            // be written once and never again.
            header->signature.signature_magic =
                nvme::block_signature::magic;
            header->signature.file_id = target.file_id;
            header->signature.block_index = next_block_index;
            for (std::size_t i{}; i < sizeof(target.disk_guid); ++i) {
                header->signature.disk_guid[i] = target.disk_guid[i];
                header->signature.partition_guid[i] =
                    target.partition_guid[i];
            }

            header->block_magic = nvme::block_header::magic;
            header->boot_id = boot_id;
            header->epoch = epoch;
            header->sequence = sequence;
            header->block_index = next_block_index;
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

            header_records = staged_records;
        }

        auto result =
            wait ? queues::submit(target,
                                  next_block_index,
                                  epoch,
                                  physical_of,
                                  spin_budget)
                 : queues::try_submit(
                       target, next_block_index, epoch, physical_of);

        // Still out on the device. The block keeps the staging buffer and
        // its place in the file, and the next pass collects the read - so
        // a slow controller costs this block latency rather than costing
        // the block. The header is rebuilt on the retry, which is why
        // nothing above this is conditional on it being the first attempt.
        if (nvme::write_result::verify_pending == result) {
            return;
        }

        staged_records = 0;
        staged_deadline = 0;
        header_records = 0;
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
