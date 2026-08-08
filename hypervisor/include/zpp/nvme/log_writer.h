#pragma once
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/nvme/command.h"
#include "zpp/nvme/log_format.h"
#include "zpp/nvme/registers.h"

#include <cstddef>
#include <cstdint>

/**
 * The private queue pair, and the only path that writes to the medium.
 *
 * Once the queue pair exists this is all there is: build a 64 byte
 * command, ring our own doorbell, and poll our own completion queue. No
 * interception, nothing shared with the guest, and no interrupt - the
 * completion queue is created with IEN clear precisely so that it
 * consumes none of the vectors the guest owns.
 *
 * Two rules here are correctness rather than economy, and both come from
 * what a mistake would cost on a disk holding somebody's Windows:
 *
 * 1. **The guard read before every doorbell ring.** A queue destroyed by
 *    a guest reset leaves a doorbell that is no longer backed by
 *    anything, and PCIe Transport 1.0c 3.1.2.1 says "writing to a
 *    non-existent Submission Queue Tail Doorbell has undefined results".
 *    Worse than undefined in practice: the reference implementation
 *    answers by posting asynchronous event 00h, Write to Invalid
 *    Doorbell Register, **into the guest's admin completion queue**
 *    whenever an Asynchronous Event Request is outstanding - which, for
 *    both Linux and Windows, is always. So a stale ring surfaces as a
 *    disk error attributed to the guest.
 *
 * 2. **The signature check before every write.** The destination is read
 *    first and must carry the signature the loader placed there this
 *    boot. That turns "the loader parsed FAT correctly" from a belief
 *    into a measurement taken per write, and it is what makes writing
 *    next to the Windows boot manager acceptable at all.
 *
 * Neither can be traded away for throughput. This is a debug channel;
 * losing a record is free and losing the machine is not.
 */
namespace zpp::nvme
{
/**
 * Why a write did not happen.
 */
enum class write_result
{
    ok,

    /** No queue pair - never brought up, or a reset took it. */
    no_queues,

    /**
     * The controller is not in the state we created our queues in. The
     * guard read caught it, which is the point of the guard read.
     */
    epoch_changed,

    /** The logical block index is past the end of the file's extents. */
    out_of_range,

    /**
     * The destination did not carry the loader's signature. Either the
     * extent table is wrong or something else has taken the file's
     * blocks. Either way this is the last thing that should ever happen
     * quietly.
     */
    signature_mismatch,

    /** The submission queue is full. Backpressure, not an error. */
    queue_full,

    /** A submitted command did not complete inside its budget. */
    timed_out,

    /**
     * The destination read is out on the device and has not come back
     * yet. Not a failure and **not a drop**: the caller keeps the block
     * and asks again.
     *
     * This exists because the write path runs inside a VM exit handler.
     * The destination check needs a read to complete, and the only two
     * shapes available are "spin until it does" and "come back later".
     * Spinning was measured wrong on the medium: with the guest's own
     * driver using the controller, every read exceeded the spin budget,
     * every block was discarded, and the channel wrote one block and
     * then nothing for the rest of the boot.
     */
    verify_pending,

    /**
     * Not a result. The number of enumerators above it, so `results` can
     * be indexed by one without a table that has to be kept in step.
     */
    count,
};

/**
 * The private queue pair.
 *
 * A class template on its capacity, which is the idiom counter.h
 * documents and the reason it is used here: a static data member of a
 * class template is only emitted if the specialisation is odr-used, so a
 * build with the channel switched off carries no queues, no staging
 * buffer and no code - not an unused copy of them.
 *
 * Every buffer is exactly one page, so there are **no PRP lists
 * anywhere**. That is not a coincidence: a submission queue of 64 entries
 * is 4096 bytes exactly, a completion queue of 64 is 1024, and one record
 * block is 4096. PRP2 stays zero throughout, which removes an entire
 * class of mistake from the write path.
 *
 * The storage is hidden from the guest, and that is essential rather than
 * tidy: the submission queue holds raw commands carrying logical block
 * addresses, and a guest write into it would make the controller execute
 * them. EPT does not affect DMA, so the controller still reads memory the
 * guest cannot touch, which is exactly the asymmetry wanted.
 *
 * It used to be hidden for free, by living inside the module that
 * `protect_module` covers. It no longer lives there - see bind_storage -
 * so the resident side hides it explicitly through `protect_region`. That
 * is a thing to keep in step: storage that moves out from under that call
 * is storage the guest can write commands into.
 */
template <std::uint32_t Entries>
class queue_pair
{
public:
    static_assert((Entries >= 2) && (0 == (Entries & (Entries - 1))),
                  "a queue depth has to be a power of two, at least 2");
    static_assert(Entries * sizeof(submission_entry) <= 4096,
                  "the submission queue has to fit one page, or it "
                  "needs a PRP list and this design has none");

    /**
     * How the four buffers are laid out in the storage handed to
     * bind_storage, one page each.
     *
     * A page each because Create I/O Queue requires a page aligned
     * physically contiguous buffer when PC is set, and PC is always set
     * here. The completion queue needs only a quarter of its page at
     * this depth; the waste is a kilobyte and buys the alignment the
     * controller demands.
     * @{
     */
    static constexpr std::size_t submission_offset = 0;
    static constexpr std::size_t completion_offset = 4096;
    static constexpr std::size_t staging_offset = 8192;
    static constexpr std::size_t scratch_offset = 12288;

    /**
     * An admin queue pair of this VMM's own, for the window in which it
     * drives the controller itself.
     *
     * Not the guest's admin queue, and that is the point: borrowing that
     * one means operating on memory a live driver owns, which is
     * unabandonable once started and was measured timing out. These two
     * pages are ours, are only ever pointed at by ASQ and ACQ while the
     * guest has itself disabled the controller, and are unprogrammed
     * again before it is handed back.
     * @{
     */
    static constexpr std::size_t admin_submission_offset = 16384;
    static constexpr std::size_t admin_completion_offset = 20480;
    /**
     * @}
     */

    static constexpr std::size_t storage_bytes = 24576;
    /**
     * @}
     */

    /**
     * The queues and buffers, in storage somebody else owns.
     *
     * Pointers rather than arrays, and that is the whole point. They
     * used to be `static inline` arrays, which meant the copy in the
     * loader's binary and the copy in the resident module were two
     * different objects at two different addresses. The loader created
     * the controller's queues against *its* arrays, handed over the
     * doorbells, and the resident side then wrote commands into *its*
     * arrays and rang a doorbell for queues the controller believed were
     * somewhere else entirely. Nothing was ever fetched from where it
     * was written and no completion ever arrived - measured as
     * `submitted = 1, completed = 0` with nothing refused.
     *
     * Worse than useless: the loader's arrays live in its own image,
     * which is EfiLoaderData, which the operating system reclaims after
     * ExitBootServices. Ringing that doorbell asks the controller to
     * fetch a command out of whatever Windows has since put there and
     * execute it, with whatever opcode and addresses those bytes happen
     * to spell.
     *
     * So the storage is allocated once, as EfiReservedMemoryType - the
     * one kind no operating system may account for or reuse, the same
     * kind the module itself lives in - and both sides are pointed at
     * it. There is then exactly one submission queue, and it is the one
     * the controller was told about.
     * @{
     */
    static inline submission_entry * submissions{};
    static inline completion_entry * completions{};
    static inline std::uint8_t * staging{};
    static inline std::uint8_t * scratch{};
    static inline submission_entry * admin_submissions{};
    static inline completion_entry * admin_completions{};
    /**
     * @}
     */

    /**
     * Points the four at one contiguous, page aligned, permanently
     * allocated region of at least storage_bytes.
     *
     * Called by whoever owns that region: the loader before it creates
     * the queues, and the resident side with the same address once it
     * has been handed over. Refuses a misaligned base rather than
     * letting the controller reject the queues later for a reason that
     * would be much harder to read.
     */
    static bool bind_storage(void * base)
    {
        if ((nullptr == base) ||
            (0 != (reinterpret_cast<std::uintptr_t>(base) & 0xfff))) {
            return false;
        }

        auto * bytes = static_cast<std::uint8_t *>(base);
        submissions = reinterpret_cast<submission_entry *>(
            bytes + submission_offset);
        completions = reinterpret_cast<completion_entry *>(
            bytes + completion_offset);
        staging = bytes + staging_offset;
        scratch = bytes + scratch_offset;
        admin_submissions = reinterpret_cast<submission_entry *>(
            bytes + admin_submission_offset);
        admin_completions = reinterpret_cast<completion_entry *>(
            bytes + admin_completion_offset);
        return true;
    }

    /**
     * Whether storage has been bound. Every path that touches a queue
     * checks this, because a null here is a wild write rather than a
     * refusal.
     */
    static bool storage_bound()
    {
        return nullptr != submissions;
    }

    /**
     * Where in the queues the previous owner left off.
     *
     * Sharing the memory is not enough on its own, because a queue is
     * half memory and half position. The loader submits at least one
     * command of its own - the proof write - so by hand-over time the
     * controller's internal submission head has advanced, one completion
     * has been consumed, and the phase bit the next completion will
     * carry is whatever the wrap left it as.
     *
     * A resident side starting from zero disagrees with the controller
     * about all three, and the first disagreement is fatal in a way that
     * looks like nothing at all: it writes at index zero and rings the
     * doorbell with tail one, the controller compares that against a
     * head it has already advanced to one, concludes the queue is empty,
     * and fetches nothing. The command is never executed and no
     * completion is ever posted. `submitted = 1, completed = 0`, exactly
     * as if the memory were still wrong.
     *
     * So the position travels with the storage. The counters do not need
     * to: `reap` only cares that `completed` and `submitted` start equal,
     * because their difference is what "outstanding" means.
     * @{
     */
    static void bind_position(std::uint32_t tail,
                              std::uint32_t head,
                              bool phase)
    {
        submission_tail = tail % Entries;
        completion_head = head % Entries;
        completion_phase = phase;
        submitted = 0;
        completed = 0;

        // Command identifiers are `submitted & 0xffff`, so resetting the
        // counters restarts them. Any verify still believed to be
        // outstanding is against identifiers that are about to be handed
        // out again, and would be matched by the wrong completion.
        verify_outstanding = false;
        verify_landed = false;
    }

    static std::uint32_t tail_position()
    {
        return submission_tail;
    }

    static std::uint32_t head_position()
    {
        return completion_head;
    }

    static bool phase_position()
    {
        return completion_phase;
    }
    /**
     * @}
     */

    /**
     * Where our doorbells are, and which identifiers the controller gave
     * us. Filled in by the bring-up; zero means no queue pair.
     */
    struct binding
    {
        volatile void * submission_doorbell{};
        volatile void * completion_doorbell{};
        volatile void * status_register{};
        volatile void * configuration_register{};
        std::uint16_t submission_id{};
        std::uint16_t completion_id{};
        std::uint32_t namespace_id{};

        /**
         * Which controller epoch these belong to. Compared on every
         * guard read, so a reset that happened between two writes
         * cannot be missed by a stale pointer still looking valid.
         */
        std::uint32_t epoch{};

        constexpr bool live() const
        {
            return (nullptr != submission_doorbell) &&
                   (nullptr != completion_doorbell) &&
                   (nullptr != status_register);
        }
    };

    static inline binding bound{};

    /** Our own position in our own queues. */
    static inline std::uint32_t submission_tail{};
    static inline std::uint32_t completion_head{};
    static inline bool completion_phase{true};

    /**
     * Counters, all of them readable from a debugger and all of them
     * copied into every block header so that a reader can tell a quiet
     * channel from a dead one without a debugger at all.
     * @{
     */
    static inline std::uint64_t submitted{};
    static inline std::uint64_t completed{};
    static inline std::uint64_t failed{};
    static inline std::uint64_t refused_guard{};
    static inline std::uint64_t refused_signature{};
    static inline std::uint64_t lost_to_reset{};
    /**
     * @}
     */

    /**
     * How many entries `results` has: one per write_result enumerator.
     */
    static constexpr std::size_t result_kinds =
        static_cast<std::size_t>(write_result::count);

    /**
     * **Every return from submit, counted by which return it was.**
     *
     * The counters above answer "what did the device do". They cannot
     * answer "why did a block not go out", because three of submit's
     * returns used to increment nothing at all - out_of_range,
     * queue_full and timed_out - and the caller counted one `dropped`
     * for all of them. A run that discarded 1975 blocks with
     * `submitted == completed`, `failed`, `refused_guard` and
     * `refused_signature` all zero therefore said only "not the device",
     * and the choice between the remaining three was argued from the
     * source rather than read off the machine.
     *
     * The whole point of this array is that the argument is not needed.
     * Read it and the answer is a number beside a name.
     *
     * Indexed by `static_cast<std::size_t>(result)`, incremented on
     * **every** return including `ok`, so the entries sum to the number
     * of calls and `dropped` has to equal the sum of the ones that are
     * not `ok`. A disagreement between those two is itself a finding.
     */
    static inline std::uint64_t results[result_kinds]{};

    /**
     * The destination read that is out on the device, if there is one.
     *
     * The check itself is unchanged - the block is read and must carry
     * the loader's signature - but waiting for it is no longer done by
     * spinning. The command is issued on one call and collected on a
     * later one, so a slow controller costs the block a few more VM
     * exits of latency instead of costing the block.
     *
     * `verify_landed` is set by `reap`, and only by the completion whose
     * command identifier matches `verify_id`. Matching on the identifier
     * rather than on "the completion count moved" is what makes the
     * check honest: with a previous block's write still outstanding, a
     * count that moved says only that *something* completed, and acting
     * on it reads a scratch buffer the read has not filled yet.
     * @{
     */
    static inline bool verify_outstanding{};
    static inline bool verify_landed{};
    static inline std::uint16_t verify_id{};
    static inline std::uint64_t verify_block{};
    static inline std::uint64_t verify_issued{};

    /**
     * How long the destination read took, in time stamp counter ticks,
     * last time and at worst. This is the measurement the spin budget
     * never had: a budget in *iterations* is a different amount of time
     * in every build and on every processor, so "the read did not
     * complete in 1<<22 polls" never said how long that was.
     * @{
     */
    static inline std::uint64_t verify_ticks_last{};
    static inline std::uint64_t verify_ticks_max{};
    /**
     * @}
     */

    /**
     * How many verifies were given up on, having been outstanding
     * longer than `verify_ticks_budget`.
     *
     * Given up on for reporting only: the command is left outstanding on
     * purpose, because it will DMA into `scratch` whenever it does land
     * and a second read must not be issued on top of it. So the next
     * attempt waits for this one before starting a fresh verify.
     */
    static inline std::uint64_t verify_abandoned{};

    /**
     * How long a destination read may be outstanding before the block
     * waiting on it is given up.
     *
     * A tick count rather than a poll count, so it means the same wall
     * clock time in a debug build and a release one. Roughly a second
     * and a half on any processor this runs on, which is far longer than
     * a saturated NVMe controller's worst read and far shorter than a
     * boot: the intent is to catch a command the controller has lost,
     * not to bound the device's latency.
     */
    static inline std::uint64_t verify_ticks_budget{1ull << 32};
    /**
     * @}
     */

    /**
     * Forgets the queue pair. Called the moment a reset is observed, so
     * that nothing rings a doorbell the controller no longer backs.
     *
     * The completion queue is zeroed as well as forgotten. A stale entry
     * left from the previous epoch carries phase 1, and the next epoch
     * starts expecting phase 1, so leaving it would make the first poll
     * read a completion that never happened.
     */
    static void forget()
    {
        if (!storage_bound()) {
            return;
        }
        lost_to_reset += (submitted - completed);
        bound = binding{};
        submission_tail = 0;
        completion_head = 0;
        completion_phase = true;
        submitted = 0;
        completed = 0;

        // The queue the verify read was sitting on no longer exists, so
        // that command will never complete and will never DMA into
        // scratch. Forgetting it here is what lets the next epoch start a
        // fresh verify instead of waiting for one that cannot arrive.
        verify_outstanding = false;
        verify_landed = false;
        for (std::uint32_t i{}; i < Entries; ++i) {
            completions[i] = completion_entry{};
        }
    }

    /**
     * The guard read. Everything that rings a doorbell goes through it.
     *
     * Reading two registers costs a pair of uncached accesses, which
     * against a write every few milliseconds is nothing, and it reduces
     * the window in which a doorbell could be rung into a destroyed
     * queue from milliseconds to the few nanoseconds between this read
     * and the store below it.
     */
    static bool controller_still_ours(std::uint32_t current_epoch)
    {
        if (!bound.live() || (bound.epoch != current_epoch)) {
            return false;
        }

        auto status =
            controller_status{arch::x86_64::read32(bound.status_register)};
        if (!status.ready() || status.fatal_status()) {
            return false;
        }

        auto configuration = controller_configuration{
            arch::x86_64::read32(bound.configuration_register)};
        return configuration.enable() &&
               (shutdown_notification::none ==
                configuration.shutdown_notification());
    }

    /**
     * Reaps whatever has completed. Never waits: the write path is
     * called from a VM exit handler with a guest waiting to be resumed.
     */
    /**
     * Reaps until nothing is outstanding, or the budget runs out.
     *
     * For handing the queues on. Whoever inherits them starts with its
     * own counters at zero, meaning "nothing outstanding", so anything
     * left unreaped becomes a completion it will attribute to its own
     * first command - and then believe a write succeeded that it never
     * issued.
     *
     * The loader leaves exactly that behind: it submits a verify read
     * and a write, reaps the read while waiting for it, and hands over
     * with the write's completion still sitting in the queue. Measured
     * as tail 2, head 1.
     *
     * Bounded, and a failure to drain is the caller's to act on: handing
     * over a queue that would not settle is worse than handing over
     * none.
     */
    static bool drain(std::uint64_t budget)
    {
        while (completed < submitted) {
            reap();
            if (0 == budget--) {
                return false;
            }
        }
        return true;
    }

    static void reap()
    {
        while (completed < submitted) {
            auto & entry = completions[completion_head];
            if (entry.phase() != completion_phase) {
                return;
            }
            arch::x86_64::order_loads();

            // Which command this was, before the entry is stepped over.
            // The identifier is the only thing that ties a completion to
            // the read the destination check is waiting for; the count
            // does not, because a previous block's write can be
            // outstanding at the same time.
            if (verify_outstanding && (verify_id == entry.command_id())) {
                verify_landed = true;
                verify_ticks_last = arch::x86_64::rdtsc() - verify_issued;
                if (verify_ticks_last > verify_ticks_max) {
                    verify_ticks_max = verify_ticks_last;
                }
            }

            if (0 != entry.status()) {
                ++failed;
            }
            ++completed;

            completion_head = (completion_head + 1) % Entries;
            if (0 == completion_head) {
                completion_phase = !completion_phase;
            }
            arch::x86_64::write32(bound.completion_doorbell,
                                  completion_head);
        }
    }

    /**
     * Moves one block along, without ever waiting for the device.
     *
     * Called repeatedly for the same `block_index` until it answers
     * something other than `verify_pending`. Each call does as much as
     * can be done without blocking: issue the destination read, or
     * collect it and issue the write.
     *
     * **This is the call the write path uses**, and the reason it exists
     * is measured. The path runs inside a VM exit handler with a guest
     * waiting, so the destination read used to be waited for by spinning
     * on a poll count. Once the guest's own driver was using the
     * controller, every read exceeded that budget: the block was
     * discarded, the read completed a moment later anyway, and the
     * channel wrote one block per epoch and then nothing - 1975 blocks
     * discarded against 65 submissions, none failed and none refused.
     * Coming back on the next exit costs the block microseconds of
     * latency and costs the guest nothing.
     *
     * `target` is the loader's validated extent table and is the only
     * way a destination can be named, so an out of range index is a
     * refusal rather than a write somewhere else - see log_target's
     * lba_of.
     *
     * `physical_of` turns one of our own buffers into the address the
     * controller will use. Under the host page table that is a lookup;
     * while boot services are alive it is the identity.
     */
    static write_result
    try_submit(const log_target & target,
               std::uint64_t block_index,
               std::uint32_t current_epoch,
               std::uint64_t (*physical_of)(const void *))
    {
        return note(
            attempt(target, block_index, current_epoch, physical_of));
    }

    /**
     * The waiting form, for callers that can afford to block.
     *
     * Two of them can: the loader's proof write, which runs while boot
     * services are alive and the controller is otherwise idle, and the
     * flush taken immediately before the guest disables the controller,
     * where anything not written now is not writable at all.
     *
     * `spin_budget` bounds the polling only. The full attempt is not
     * repeated per poll on purpose: its guard read costs two uncached
     * register accesses, and at a budget in the millions that is seconds
     * of bus traffic rather than a spin.
     */
    static write_result submit(const log_target & target,
                               std::uint64_t block_index,
                               std::uint32_t current_epoch,
                               std::uint64_t (*physical_of)(const void *),
                               std::uint64_t spin_budget)
    {
        auto result =
            attempt(target, block_index, current_epoch, physical_of);

        auto spun = spin_budget;
        while (write_result::verify_pending == result) {
            if (0 == spun--) {
                result = write_result::timed_out;
                break;
            }

            reap();
            if (verify_outstanding && !verify_landed) {
                continue;
            }

            result =
                attempt(target, block_index, current_epoch, physical_of);
        }

        return note(result);
    }

private:
    /**
     * Counts a result by which result it was, and passes it through.
     */
    static write_result note(write_result result)
    {
        ++results[static_cast<std::size_t>(result)];
        return result;
    }

    /**
     * One step of the block, uncounted. Never blocks.
     */
    static write_result attempt(const log_target & target,
                                std::uint64_t block_index,
                                std::uint32_t current_epoch,
                                std::uint64_t (*physical_of)(const void *))
    {
        // Storage as well as doorbells. A bound queue whose memory was
        // never supplied would write commands through a null pointer,
        // which is a wild store rather than a refusal.
        if (!bound.live() || !storage_bound()) {
            return write_result::no_queues;
        }

        std::uint64_t lba{};
        auto per_block = block_size / target.block_size;

        // Refused here as well as in log_target::usable(), because this
        // is the arithmetic that goes wrong: a zero makes every index
        // resolve to the region's first LBA and makes the block count
        // zero, which the write command encodes as 0xffff - a 512 MB
        // write past the region. A destination that got this far with a
        // zero is one the checks upstream did not catch, which is exactly
        // when the value of a second check is highest.
        if (0 == per_block) {
            return write_result::out_of_range;
        }

        if (!target.lba_of(block_index * per_block, lba)) {
            return write_result::out_of_range;
        }

        if (!controller_still_ours(current_epoch)) {
            ++refused_guard;
            return write_result::epoch_changed;
        }

        reap();

        if (verify_outstanding) {
            if (!verify_landed) {
                if ((arch::x86_64::rdtsc() - verify_issued) <
                    verify_ticks_budget) {
                    return write_result::verify_pending;
                }

                // Long enough that the controller has lost it rather
                // than being slow. Reported, and deliberately still
                // outstanding: whenever it does land it DMAs into
                // scratch, so a second read must not be issued on top of
                // it. The next attempt waits for this one and then starts
                // a fresh verify.
                ++verify_abandoned;
                return write_result::timed_out;
            }

            auto verified_block = verify_block;
            verify_outstanding = false;
            verify_landed = false;

            // The read that landed was for some other block - the only
            // way that happens is a block given up on above - so it says
            // nothing about this one. Fall through and start again.
            if (verified_block == block_index) {
                if (!destination_is_ours(target, block_index)) {
                    ++refused_signature;
                    return write_result::signature_mismatch;
                }

                auto command = write(target.namespace_id,
                                     lba,
                                     static_cast<std::uint16_t>(per_block),
                                     physical_of(staging),
                                     true);
                return issue(command, current_epoch);
            }
        }

        if ((submitted - completed) >= (Entries - 1)) {
            return write_result::queue_full;
        }

        // The destination has to prove it is ours before it is
        // overwritten, so the block is read first. Issued here and
        // collected above, on a later call.
        auto command = read(target.namespace_id,
                            lba,
                            static_cast<std::uint16_t>(per_block),
                            physical_of(scratch));

        verify_landed = false;
        verify_outstanding = true;
        verify_block = block_index;
        verify_issued = arch::x86_64::rdtsc();

        // The identifier issue() is about to use, recorded before it is
        // used so that reap() can recognise the completion. issue()
        // takes it from `submitted`, which it only advances once the
        // doorbell has been rung.
        verify_id = static_cast<std::uint16_t>(submitted & 0xffff);

        if (auto issued = issue(command, current_epoch);
            write_result::ok != issued) {
            verify_outstanding = false;
            return issued;
        }

        return write_result::verify_pending;
    }

    /**
     * Whether what was read back into scratch is the block this channel
     * owns. No waiting and no device access: the read has already landed.
     */
    static bool destination_is_ours(const log_target & target,
                                    std::uint64_t block_index)
    {
        auto found = reinterpret_cast<const block_signature *>(scratch);
        if ((block_signature::magic != found->signature_magic) ||
            (target.file_id != found->file_id) ||
            (block_index != found->block_index)) {
            return false;
        }

        for (std::size_t i{}; i < sizeof(found->partition_guid); ++i) {
            if (found->partition_guid[i] != target.partition_guid[i]) {
                return false;
            }
        }

        // The disk as well as the partition.
        //
        // The partition GUID alone is very nearly enough, being unique
        // per partition rather than per position, but "very nearly" is
        // the wrong standard for the one check standing between this
        // code and somebody's installation. A machine with more than one
        // NVMe namespace addresses them by an id this structure also
        // carries, and an identifier that was copied rather than
        // generated - a cloned disk, an image restored onto a second
        // drive - collides by construction. Both GUIDs together mean a
        // block has to prove it is on the disk the reservation signed,
        // not merely that it looks like the right partition.
        for (std::size_t i{}; i < sizeof(found->disk_guid); ++i) {
            if (found->disk_guid[i] != target.disk_guid[i]) {
                return false;
            }
        }

        return true;
    }

    /**
     * Puts one command in the submission queue and rings the doorbell.
     * The guard read is repeated here rather than trusted from the
     * caller, because this is the function that does the ringing.
     */
    static write_result issue(submission_entry command,
                              std::uint32_t current_epoch)
    {
        if ((submitted - completed) >= (Entries - 1)) {
            return write_result::queue_full;
        }

        auto id = static_cast<std::uint16_t>(submitted & 0xffff);
        command.command_dword0 = (command.command_dword0 & 0x0000ffffu) |
                                 (static_cast<std::uint32_t>(id) << 16);

        submissions[submission_tail] = command;
        submission_tail = (submission_tail + 1) % Entries;

        arch::x86_64::order_stores();

        if (!controller_still_ours(current_epoch)) {
            // Back the entry out rather than ring. Nothing has been
            // handed to the controller yet, so this costs only the
            // record that was about to go.
            submission_tail = (submission_tail + Entries - 1) % Entries;
            ++refused_guard;
            return write_result::epoch_changed;
        }

        arch::x86_64::write32(bound.submission_doorbell, submission_tail);
        ++submitted;
        return write_result::ok;
    }
};

} // namespace zpp::nvme
