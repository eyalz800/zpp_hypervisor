#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/format.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zpp::diag
{
/**
 * One retained record: a fixed size slot, no pointers in it.
 *
 * Fixed size because the write path cannot allocate and a variable layout
 * would need a second pass to find the next record. No pointers because a
 * record outlives everything it was built from: it is read minutes later
 * by a debugger, or by a streaming sink replaying from the beginning, and
 * a pointer into a VM exit handler's stack would be a fault waiting for a
 * reader. A logged string is therefore copied into the payload, truncated
 * if it does not fit, and never referred to.
 *
 * The payload holds either the rendered text or the packed record, and
 * `shape` says which. Both never appear in one build for one record: the
 * writer produces the forms the enabled sinks between them asked for, and
 * where both are asked for, both are retained as separate records rather
 * than as one slot with two payloads. That costs a slot and keeps the
 * layout flat, which is the trade this codebase wants.
 */
struct record
{
    /**
     * Payload capacity, from the configured slot size less this header.
     * Stated as an arithmetic identity rather than a literal so a change
     * to the slot size cannot silently overflow the slot - the
     * static_assert below is what enforces it, and it has already caught
     * this number being wrong: the fields below come to 26 bytes and the
     * struct's alignment rounds the header to 32.
     */
    static constexpr std::size_t header_size = 32;
    static constexpr std::size_t payload_capacity =
        ring.record_size - header_size;

    /**
     * Which record this is in its processor's stream. Monotonic and never
     * reduced modulo anything, so a gap in it is the definition of a lost
     * record and a reader needs nothing else to detect one.
     */
    std::uint64_t sequence{};

    /**
     * When, by whatever config::timestamp() counts. What puts the
     * processors' streams back into one order when they are read
     * together - there is no shared counter on the write path to do it,
     * deliberately.
     */
    std::uint64_t when{};

    /**
     * What the record format identifies the line by. Present in a text
     * record too: it is what a reader groups repeated lines by without
     * parsing them.
     */
    std::uint32_t event{};

    std::uint16_t length{};
    severity level{};
    category which{};
    form shape{};

    /**
     * Set when the line did not fit. A reader must be able to tell a short
     * line from a cut one.
     */
    bool truncated{};

    std::uint8_t payload[payload_capacity]{};
};

static_assert(sizeof(record) == ring.record_size,
              "a record has to fill its slot exactly, or the ring's index "
              "arithmetic addresses the wrong bytes");

/**
 * The retention ring's storage.
 *
 * A class template parameterised on the configuration, and that is the
 * whole reason it is a template: a static data member of a class template
 * is only emitted if the specialisation is used, and every use of this one
 * is inside an `if constexpr` on config::enabled. So a disabled build has
 * no ring, not a ring of length zero and not a ring nobody writes to - the
 * .bss is not there. A `static inline` member of a plain class would be
 * emitted unconditionally, which is exactly what exit_trace does today.
 *
 * Both members are constant initialized - a POD array and std::atomic's
 * constexpr default constructor - so this costs no .init_array entry.
 * scripts/ci/check-invariants.sh is what proves it rather than this
 * comment.
 */
template <ring_configuration Configuration>
struct ring_storage
{
    static inline record slots[Configuration.processors]
                              [Configuration.records]{};

    /**
     * How many records each processor has written. The only cross
     * processor communication in the whole write path, and it is a release
     * store to a word only that processor writes.
     */
    static inline std::atomic<std::uint64_t>
        written[Configuration.processors]{};
};

/**
 * The ring, as the write path sees it.
 *
 * The write path cannot fail, cannot block and cannot be contended:
 *
 * - Per processor, so there is no lock. A lock here would be the one thing
 *   a VM exit handler must never take - another processor could be holding
 *   it and never coming back, and zpp::spin_lock is not recursive, so even
 *   a fault inside a logging processor would deadlock it against itself.
 * - It always accepts. A full ring overwrites its oldest record, so the
 *   writer has no failure to report and nothing to count. Every loss is
 *   detected on the reading side, where there is time to deal with it.
 * - Bounded work: one slot filled, one release store.
 *
 * That is the argument for the pull model in one paragraph. Anything that
 * has to talk to a device - a screen, a UART, a USB endpoint, a disk - is
 * a reader with a cursor of its own, driven from somewhere that is allowed
 * to be slow.
 */
struct ring_writer
{
    /**
     * Appends one record to this processor's ring.
     *
     * The slot is filled first and the count published afterwards, with
     * release ordering: a reader that sees the count sees the whole slot.
     * On x86 the stores retire in order anyway, so this costs nothing but
     * it is what makes the ordering a property of the code rather than of
     * the target.
     */
    static void append(std::size_t processor, const record & source)
    {
        auto & store = ring_storage<ring>::written[processor];
        auto sequence = store.load(std::memory_order_relaxed);

        auto & slot =
            ring_storage<ring>::slots[processor]
                                     [sequence & (ring.records - 1)];
        slot = source;
        slot.sequence = sequence;

        store.store(sequence + 1, std::memory_order_release);
    }
};

/**
 * A reader's position in the whole ring, and what it has missed.
 *
 * One of these per sink. Several may exist at once and at different
 * positions - the framebuffer showing the tail, a streaming sink replaying
 * from the oldest record it can still see, a disk sink somewhere between
 * them - and they do not interact: a cursor is private to its sink and the
 * writer does not know any of them exist. Nothing a reader does can slow
 * the write path down or stop it.
 *
 * This is the shape Linux's printk uses for its consoles - a per-console
 * sequence number into one ring, with a late-registering console replaying
 * from the beginning - and it is the shape that answers "send everything
 * already stored, then continue live" without the write path knowing which
 * sinks want that.
 */
struct cursor
{
    /**
     * The next record wanted from each processor.
     */
    std::uint64_t next[ring.processors]{};

    /**
     * How many records this reader never saw because the writer lapped it.
     * Counted rather than prevented: a reader that could hold the writer
     * up is a reader that can hang the hypervisor.
     */
    std::uint64_t lost{};

    /**
     * Records handed to the sink, and records the sink refused. Separate,
     * because a sink that is being offered records and dropping them is a
     * different failure from a pump that is never running.
     * @{
     */
    std::uint64_t delivered{};
    std::uint64_t refused{};
    /**
     * @}
     */

    /**
     * Whether this reader is done being offered records - a sink that
     * failed more times in a row than its policy tolerates. Recorded so
     * the reason a channel went quiet is readable rather than inferred.
     */
    bool dead{};

    /**
     * Consecutive failures so far.
     */
    std::uint32_t failures{};
};

/**
 * Reading out of the ring.
 *
 * Every check here is on the reader's side, which is the point: the writer
 * has no idea it is being read.
 */
struct ring_reader
{
    /**
     * Takes the next record for this reader out of the given processor's
     * ring, oldest first, or reports that there is nothing yet.
     *
     * Returns whether `into` was filled. Advances the cursor either way,
     * so a reader can never stall on a record it will never see.
     *
     * The lapping check is what makes a lock unnecessary. A reader can
     * only be handed a torn slot if the writer wrapped all the way round
     * during the copy, and that is visible in the published count both
     * before and after - so the copy is validated rather than protected.
     * One retry, then the record is counted lost. This is a seqlock with
     * the ring's own counter as the sequence.
     */
    static bool next(cursor & position,
                     std::size_t processor,
                     record & into)
    {
        auto & store = ring_storage<ring>::written[processor];

        for (int attempt{}; attempt < 2; ++attempt) {
            auto published = store.load(std::memory_order_acquire);
            auto wanted = position.next[processor];

            if (wanted >= published) {
                return false;
            }

            // Lapped: the oldest record still in the ring is newer than
            // the one this reader wanted.
            if ((published - wanted) > ring.records) {
                auto oldest = published - ring.records;
                position.lost += (oldest - wanted);
                position.next[processor] = oldest;
                wanted = oldest;
            }

            into = ring_storage<ring>::slots[processor]
                                            [wanted & (ring.records - 1)];

            auto after = store.load(std::memory_order_acquire);
            if ((after - wanted) <= ring.records) {
                position.next[processor] = wanted + 1;
                return into.sequence == wanted;
            }
        }

        // Overwritten twice while being read. The writer wins; say so.
        ++position.lost;
        position.next[processor] =
            store.load(std::memory_order_acquire) - ring.records;
        return false;
    }
};

} // namespace zpp::diag
