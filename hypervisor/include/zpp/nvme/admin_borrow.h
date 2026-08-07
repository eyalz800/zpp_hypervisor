#pragma once
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/nvme/command.h"
#include "zpp/nvme/registers.h"

#include <cstddef>
#include <cstdint>

/**
 * Borrowing the guest's admin queue, and handing it back untouched.
 *
 * This is the one operation the whole disk log channel rests on, and the
 * reasoning is in NVME-LOG.md. In one paragraph:
 *
 * A completion of ours lands in the guest's admin completion queue at the
 * slot the guest is watching, and the guest's head and phase are software
 * variables inside its driver that we cannot write. Reverting the phase
 * bit leaves the controller's tail one ahead of the guest forever, and a
 * controller's completion queue tail cannot be moved backwards - it is
 * neither a register nor in memory. So the queue is not restored by
 * putting the tail back. It is restored by taking the tail **all the way
 * round**, a whole number of times, until it and the phase arrive back
 * where they started.
 *
 * The count that does it, on a submission queue of `s` entries and a
 * completion queue of `c`:
 *
 *     L = lcm(s, 2 * c)
 *
 * The two is the part that is wrong by inspection. One lap returns the
 * tail and **inverts the phase**, so the guest reads its next genuine
 * completion as stale and stops - on the boot disk. `2 * c` is what
 * brings the phase back too. scripts/nvme-lap-model.py asserts exactly
 * this, and asserts that every other length fails; run it before
 * changing any of this.
 *
 * Two properties make the restore legal rather than merely plausible:
 *
 * - The controller never reads a completion queue. Base 2.0 3.3.1.2, the
 *   tail "is only used internally by the controller and is not visible to
 *   the host". So putting the memory back byte for byte cannot confuse
 *   it.
 * - Neither doorbell has to be remembered. After a whole number of laps
 *   the last value we write to each is the guest's own. That matters,
 *   because doorbells cannot be read: PCIe Transport 1.0c 3.1.2.1, "the
 *   host should not read the doorbell registers. If a doorbell register
 *   is read, the value returned is vendor specific."
 *
 * What this header does **not** do is decide when to run. It is handed a
 * quiescent admin queue by its caller and does not look for one, because
 * quiescence is established from the doorbell trap and belongs there.
 */
namespace zpp::nvme
{
/**
 * Why a borrow did not happen, or did not finish.
 *
 * Distinct values rather than a bool, because every one of these is a
 * different thing to go and look at, and a channel that goes quiet has
 * to say which.
 */
enum class borrow_result
{
    ok,

    /**
     * The controller is not in a state to be asked. Checked before the
     * first command rather than discovered during, because **a borrow
     * cannot be abandoned once begun** - stopping halfway leaves the
     * admin queue desynchronised and the guest's driver will fail.
     */
    controller_not_ready,

    /**
     * The admin queue is deeper than the compiled bound allows. Windows
     * uses 256 entries, which is 512 commands; the bound exists so that
     * an unusually deep queue is refused rather than turning into a
     * multi-millisecond VM exit nobody predicted.
     */
    queue_too_deep,

    /**
     * More completions arrived from the guest during the window than
     * there is room to hold and replay.
     */
    too_many_foreign_completions,

    /**
     * A command of ours never completed. The queue is left desynchronised
     * and the caller's only remaining move is to reset the controller,
     * which is an event the guest's driver is built to survive.
     */
    timed_out,
};

/**
 * One borrow of the guest's admin queue.
 *
 * Not a template and not stateful: constructed on the caller's stack for
 * the duration of one borrow and gone afterwards. The snapshot buffers
 * are the caller's, because they are large and their size depends on the
 * guest's chosen queue depth.
 */
class admin_borrow
{
public:
    /**
     * The deepest admin completion queue this will borrow.
     *
     * A borrow costs `lcm(s, 2 * c)` commands, so the cost is linear in
     * the depth. Linux uses 32 entries (64 commands) and Windows
     * stornvme 256 (512 commands, the dominant cost of an epoch). 256 is
     * therefore the realistic maximum, and anything past it is refused
     * rather than paid for.
     */
    static constexpr std::uint32_t max_depth = 256;

    /**
     * How many guest completions can arrive mid borrow and still be
     * replayed. Both drivers keep one Asynchronous Event Request
     * outstanding permanently, and Modern Standby adds a Set Features
     * every 50 ms or so of idleness, so this is not a theoretical case -
     * but a handful is generous for a window measured in microseconds.
     */
    static constexpr std::uint32_t max_foreign = 16;

    /**
     * Command identifiers we use. Chosen high so that they cannot
     * collide with a guest command in flight, and so that a completion
     * is recognisably ours by inspection.
     * @{
     */
    static constexpr std::uint16_t first_command_id = 0xf000;
    static constexpr std::uint16_t last_command_id = 0xf7ff;
    /**
     * @}
     */

    /**
     * Everything one borrow needs to know about where the guest's admin
     * queue is. Read entirely from the controller's own registers - the
     * guest's queue is fully discoverable without guessing at anything
     * in its memory.
     */
    struct queues
    {
        submission_entry * submission{};
        completion_entry * completion{};
        std::uint32_t submission_depth{};
        std::uint32_t completion_depth{};

        /** Where the controller will post next, and with which phase. */
        std::uint32_t completion_tail{};
        bool completion_phase{};

        /** Where the controller will fetch next. */
        std::uint32_t submission_tail{};
    };

    /**
     * Caller-provided scratch for the snapshot. Sized by the guest's
     * depth, so it cannot be a member without fixing the depth here.
     */
    struct snapshot
    {
        submission_entry * submission{};
        completion_entry * completion{};
    };

    /**
     * Works out where the controller has got to, purely from the
     * completion queue's contents.
     *
     * This is worth stating because it removes an assumption the design
     * would otherwise need. The host is required to zero a completion
     * queue before enabling it, and only the controller writes it after
     * that, so the phase bits alone locate the tail exactly:
     *
     * - slots before the tail carry the current lap's phase,
     * - slots from the tail on carry the previous lap's,
     * - and if every slot agrees, the tail is 0 and the current phase is
     *   the inverse of what is in memory - which is also right for a
     *   queue nothing has been posted to yet, since it is all zeroes and
     *   the first phase is 1.
     *
     * The submission tail comes from the same place. Every completion
     * carries SQHD, the controller's submission head at the time, and on
     * a quiescent queue that equals the guest's tail. So neither
     * doorbell has to have been observed for a borrow to be possible.
     */
    static void locate(queues & where)
    {
        auto depth = where.completion_depth;
        auto first = where.completion[0].phase();

        std::uint32_t tail{};
        bool uniform = true;
        for (std::uint32_t i{1}; i < depth; ++i) {
            if (where.completion[i].phase() != first) {
                tail = i;
                uniform = false;
                break;
            }
        }

        where.completion_tail = uniform ? 0u : tail;
        where.completion_phase = uniform ? !first : first;

        // The newest completion is the one before the tail, wrapping.
        // If nothing has ever been posted its SQHD is meaningless, but
        // so is the queue: the submission head is 0 too.
        if (uniform && !where.completion[0].phase()) {
            where.submission_tail = 0;
            return;
        }

        auto newest = (where.completion_tail + depth - 1) % depth;
        where.submission_tail =
            where.completion[newest].submission_queue_head();
    }

    /**
     * How many commands one borrow of these queues must issue.
     *
     * `lcm(s, 2 * c)`. Spelled out rather than called from a library
     * because there is no library here, and because the doubling is the
     * part a reader has to see.
     */
    static constexpr std::uint32_t length(std::uint32_t submission_depth,
                                          std::uint32_t completion_depth)
    {
        auto a = submission_depth;
        auto b = 2 * completion_depth;
        auto x = a;
        auto y = b;
        while (0 != y) {
            auto t = y;
            y = x % y;
            x = t;
        }
        return (0 == x) ? 0 : ((a / x) * b);
    }

    /**
     * Runs one borrow.
     *
     * `payload` is the commands the borrow exists for - at most two in
     * this design, the Create I/O Completion Queue and the Create I/O
     * Submission Queue. Everything else in the lap is filler.
     *
     * `payload_status` receives each payload command's completion status
     * so the caller can tell a created queue from a refused one.
     *
     * The guest must be stopped and the admin interrupt vector masked
     * before this is called. Neither is checked here, because neither is
     * checkable from here - they are the caller's contract, stated in
     * NVME-LOG.md.
     */
    static borrow_result run(volatile void * bar,
                             std::uint32_t doorbell_stride,
                             queues & where,
                             const snapshot & saved,
                             const submission_entry * payload,
                             std::uint32_t payload_count,
                             std::uint16_t * payload_status,
                             std::uint64_t spin_budget,
                             std::uint32_t laps = 1)
    {
        if ((where.completion_depth > max_depth) ||
            (where.submission_depth > max_depth) ||
            (0 == where.completion_depth) ||
            (0 == where.submission_depth)) {
            return borrow_result::queue_too_deep;
        }

        // Any whole number of laps restores both pointers and the phase,
        // so `laps` is legal to vary and is only ever used to measure:
        // one lap against the firmware's shallow queue says almost
        // nothing about the cost against a guest's deep one, because it
        // cannot separate the fixed cost of a borrow from the marginal
        // cost of a command. Timing several lengths can.
        auto total =
            length(where.submission_depth, where.completion_depth) * laps;
        if ((0 == total) || (payload_count > total)) {
            return borrow_result::queue_too_deep;
        }

        // Everything the guest owns, kept so it can be put back exactly.
        // The submission queue is restored as well as the completion
        // queue: no driver reads a submitted entry back, but restoring
        // it costs one copy and removes the question entirely.
        for (std::uint32_t i{}; i < where.submission_depth; ++i) {
            saved.submission[i] = where.submission[i];
        }
        for (std::uint32_t i{}; i < where.completion_depth; ++i) {
            saved.completion[i] = where.completion[i];
        }

        auto submission_doorbell = doorbell(
            bar, submission_queue_doorbell_offset(0, doorbell_stride));
        auto completion_doorbell = doorbell(
            bar, completion_queue_doorbell_offset(0, doorbell_stride));

        auto resume_tail = where.completion_tail;
        auto resume_phase = where.completion_phase;

        completion_entry foreign[max_foreign]{};
        std::uint32_t foreign_count{};

        auto sq_tail = where.submission_tail;
        auto cq_head = where.completion_tail;
        auto cq_phase = where.completion_phase;

        std::uint32_t issued{};
        std::uint32_t reaped{};

        while (reaped < total) {
            // Fill the submission queue, never closer than one entry to
            // full - 3.3.3.3, "one slot in each queue is not available
            // for use due to Head and Tail entry pointer definition".
            auto outstanding = issued - reaped;
            while ((issued < total) &&
                   (outstanding < (where.submission_depth - 1))) {
                auto id = static_cast<std::uint16_t>(first_command_id +
                                                     (issued & 0x7ff));

                auto command =
                    (issued < payload_count) ? payload[issued] : filler();
                command.command_dword0 =
                    (command.command_dword0 & 0x0000ffffu) |
                    (static_cast<std::uint32_t>(id) << 16);

                where.submission[sq_tail] = command;
                sq_tail = (sq_tail + 1) % where.submission_depth;
                ++issued;
                ++outstanding;
            }

            arch::x86_64::order_stores();
            arch::x86_64::write32(submission_doorbell, sq_tail);

            // Drain whatever has landed.
            auto spun = spin_budget;
            while (reaped < issued) {
                auto & entry = where.completion[cq_head];
                if (entry.phase() != cq_phase) {
                    if (0 == spun--) {
                        return borrow_result::timed_out;
                    }
                    continue;
                }
                arch::x86_64::order_loads();

                auto id = entry.command_id();
                if ((id < first_command_id) || (id > last_command_id)) {
                    // The guest's own - an Asynchronous Event Request
                    // completing, or Modern Standby's Set Features. It
                    // still has to reach the guest, so it is held and
                    // replayed into the slot it would have occupied.
                    // Our own count stays a whole number of laps; these
                    // are extra postings on top, which is exactly the
                    // case the model covers.
                    if (foreign_count >= max_foreign) {
                        return borrow_result::too_many_foreign_completions;
                    }
                    foreign[foreign_count++] = entry;
                } else {
                    auto index =
                        static_cast<std::uint32_t>(id - first_command_id);
                    if ((index < payload_count) && payload_status) {
                        payload_status[index] = entry.status();
                    }
                    ++reaped;
                }

                cq_head = (cq_head + 1) % where.completion_depth;
                if (0 == cq_head) {
                    cq_phase = !cq_phase;
                }
                arch::x86_64::write32(completion_doorbell, cq_head);
                spun = spin_budget;
            }
        }

        // Hand it back. The memory first, so that nothing of ours is
        // visible for even an instant after the doorbells say the queue
        // is where the guest left it.
        for (std::uint32_t i{}; i < where.submission_depth; ++i) {
            where.submission[i] = saved.submission[i];
        }
        for (std::uint32_t i{}; i < where.completion_depth; ++i) {
            where.completion[i] = saved.completion[i];
        }

        // Anything of the guest's that completed inside the window, put
        // back in order at the slots it would have had.
        auto slot = resume_tail;
        auto phase = resume_phase;
        for (std::uint32_t i{}; i < foreign_count; ++i) {
            where.completion[slot] = foreign[i];
            // The phase bit is DW3 bit 16, and it is the one field that
            // has to say where the entry now sits rather than where it
            // originally landed.
            where.completion[slot].status_information =
                (foreign[i].status_information & ~(1u << 16)) |
                (phase ? (1u << 16) : 0u);
            slot = (slot + 1) % where.completion_depth;
            if (0 == slot) {
                phase = !phase;
            }
        }

        arch::x86_64::order_stores();

        // Both counters have come round to where they were, so these are
        // the guest's own values rather than remembered ones.
        arch::x86_64::write32(submission_doorbell, where.submission_tail);
        arch::x86_64::write32(completion_doorbell, resume_tail);

        where.completion_tail = resume_tail;
        where.completion_phase = resume_phase;
        return borrow_result::ok;
    }

private:
    /**
     * The command the lap is padded with.
     *
     * Get Features for Arbitration, reading the current value. Mandatory
     * in every controller, side effect free, and - the reason it is the
     * right choice rather than merely an adequate one - it carries **no
     * data pointer, so it performs no DMA at all**. A filler that did
     * would be able to fail for reasons the two real commands would not,
     * which would turn a padded lap into a second failure mode.
     */
    static submission_entry filler()
    {
        return get_features(feature_identifier::arbitration, 0);
    }

    static volatile void * doorbell(volatile void * bar,
                                    std::uint32_t offset)
    {
        return static_cast<volatile std::uint8_t *>(bar) + offset;
    }
};

} // namespace zpp::nvme
