#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace zpp
{
/**
 * The hypervisor's log, written into memory that outlives the boot that
 * wrote it, so that a failure with no channel to report through still
 * leaves evidence behind.
 *
 * The problem this exists for is specific and was expensive. Once the
 * guest is running there is nothing left to ask: the development target
 * has no serial port, a debugger attached from outside sees guest state
 * only, and the diagnostic CPUID leaf needs a processor that is still
 * executing to ask the question. A bare metal boot that hangs therefore
 * produced a blank screen and no reason at all - five of them in one day,
 * none of which said anything about where the guest died. The loader's own
 * disk log does not cover it either: it is written once, at the chainload,
 * so it ends at the same line whether the boot went on to work or not.
 *
 * So the log goes somewhere that survives the machine being restarted. The
 * region is reserved by the loader at a fixed physical address, written
 * through by the hypervisor as each line is produced, and read back by the
 * *next* boot's loader before anything else is launched - at which point
 * there is a file system, boot services, and all the time in the world.
 * Reading a crash log becomes "restart, mount the EFI system partition,
 * read the file", which is a path that already works.
 *
 * That it survives at all is an assumption about the hardware rather than
 * something this code can guarantee: DRAM keeps its contents across a warm
 * restart because nothing clears it, not because anything promises to. A
 * cold power cycle loses it, and firmware that retrains memory may lose it
 * too. boot_count below is what turns that assumption into a measurement -
 * the loader increments it on every boot and traces it, so a value above
 * one is direct evidence that this machine's memory did survive.
 *
 * Three properties matter more than capacity here, and shape everything
 * below:
 *
 * - **A writer that is safe from a VM exit handler.** append_line
 *   allocates nothing, takes no lock of its own, and does a bounded amount
 *   of work: at most two memcpy calls and a header update. Its caller is
 *   log_storage::append, which already holds the log's lock, so this adds
 *   no second lock and therefore no way to deadlock. zpp::spin_lock is not
 *   recursive, so taking one here would be a bug rather than a cost.
 *
 * - **A stale buffer that is detectable rather than misread.** The bytes
 *   at a fixed address are whatever the last boot left, or what was never
 *   written at all. So the region carries a signature, a format version,
 *   its own capacity, a byte count and a checksum over all of them.
 *   valid() answers whether this is our region in our format; a reader
 *   that skipped that would happily print the previous decade of DRAM.
 *
 * - **Ordering that is reconstructible after a wrap.** The payload is a
 *   byte ring, and written counts every byte ever handed to it rather
 *   than the bytes currently present. That one monotonic counter says
 *   both how much is there and where the oldest byte is, so ordered() can
 *   hand back the contents oldest first with nothing else maintained.
 */
class crash_log
{
public:
    /**
     * Where the region is. Chosen here, not discovered, and that is the
     * whole point: the boot that reads the log is not the boot that wrote
     * it, and has nobody left to ask. An address the firmware picked is
     * unknowable by then - the loader's existing low memory reservation
     * lets firmware choose and landed at 0x9b000, which is fine for
     * something used within the same boot and useless for this.
     *
     * 256 MB, for reasons that are all about what else wants the address:
     *
     * - Well clear of the first megabyte, which the start-up trampoline
     *   reservation has to live in because a start-up IPI names its entry
     *   point by an eight bit page number. That region is also the one
     *   place firmware, option ROMs and every legacy convention compete
     *   for.
     * - Above the low few megabytes that boot loaders and kernels have
     *   traditionally loaded themselves into, so this is not sitting where
     *   an operating system image wants to be even before the memory map
     *   is consulted.
     * - Low enough to exist. A machine with a quarter of a gigabyte of RAM
     *   does not have this address at all, and the emulated rig runs with
     *   one gigabyte, so anything much higher stops being testable.
     * - Mid-range rather than high, because the top of low memory is where
     *   firmware puts its own permanent allocations, SMRAM and the ACPI
     *   regions. Between the loaders below and the firmware above, the
     *   middle is the part nothing has a claim on.
     *
     * None of that makes it *certain* that firmware will grant this
     * address on an arbitrary machine, which is why the loader asks for
     * exactly this one and reports loudly when it is refused rather than
     * quietly accepting a different one. An address chosen by fallback
     * would be unfindable on the next boot, so a fallback would defeat the
     * feature while appearing to work.
     */
    static constexpr std::uint64_t region_address = 0x10000000;

    /**
     * How much is reserved: 128 KB, which is 32 pages.
     *
     * Sized against the log it has to hold rather than picked round. The
     * in-memory log keeps 512 lines and a line runs to something under a
     * hundred bytes, so this holds well over a thousand - more lines than
     * a whole boot failure produces, which is the case that matters.
     * Beyond that the ring wraps and keeps the most recent, which is the
     * right thing to keep for a hang that happens long after boot.
     */
    static constexpr std::size_t region_size = 0x20000;

    /**
     * Where the payload starts, leaving the header a round 64 bytes so
     * that a hex dump of the region has the text beginning on its own
     * line.
     */
    static constexpr std::size_t payload_offset = 0x40;

    /**
     * How many payload bytes there are.
     */
    static constexpr std::size_t capacity = region_size - payload_offset;

    /**
     * The format version, which is checked rather than assumed. The reader
     * and the writer are separate builds in separate boots, and nothing
     * stops the region outliving a change to the layout below - so a
     * mismatch has to be recognised and discarded, not decoded.
     */
    static constexpr std::uint32_t version = 1;

    /**
     * The signature, as the eight ASCII bytes "ZppHyLog" in memory order,
     * so that the start of the region reads as text in a hex dump.
     *
     * Assembled from the characters rather than written as a hex constant
     * because the byte order of the constant that spells this is exactly
     * the kind of thing that is wrong once and then believed.
     */
    static constexpr std::uint64_t magic = [] {
        const char text[]{"ZppHyLog"};
        std::uint64_t value{};
        for (std::size_t i{}; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(text[i]))
                     << (i * 8);
        }
        return value;
    }();

    /**
     * What sits at the front of the region.
     *
     * Every field here exists to let a reader reject the region rather
     * than decode it. The layout is pinned with assertions below, because
     * the two ends of it are compiled by different builds with different
     * ABIs and only agree by construction.
     */
    struct header
    {
        /**
         * The signature above. First, so that the cheapest possible check
         * is also the first one.
         */
        std::uint64_t magic;

        /**
         * The format version, and the size of this structure. Both are
         * compared rather than trusted: a region written by an older build
         * matches the signature and means something different.
         * @{
         */
        std::uint32_t version;
        std::uint32_t header_size;
        /**
         * @}
         */

        /**
         * The payload capacity the writer used, which is what makes the
         * ring arithmetic below reconstructible by a reader that was built
         * with a different one.
         */
        std::uint64_t capacity;

        /**
         * Every byte ever handed to append_line this boot, whether or not
         * it is still present.
         *
         * Deliberately not "bytes currently valid". One monotonic counter
         * answers both questions a reader has - how much there is, and
         * where the oldest byte sits - and a writer that only ever adds to
         * it cannot leave the two disagreeing.
         */
        std::uint64_t written;

        /**
         * How many boots have stamped this region, incremented by the
         * loader each time it claims it.
         *
         * This is the measurement that decides whether the whole design
         * works on a given machine: memory surviving a warm restart is an
         * assumption, and a second boot reading back a count of two is the
         * evidence for it. It also tells a reader how old the log it is
         * holding is.
         */
        std::uint64_t boot_count;

        /**
         * A checksum over every byte of this header ahead of itself.
         *
         * The payload is deliberately not covered. A checksum over it
         * would have to be recomputed on every line, which is unbounded
         * work on a path that must be bounded - and the header alone is
         * enough to answer the question that matters, which is whether
         * this region is ours and its counters mean what they say. A guest
         * that scribbles on the payload corrupts text; one that scribbles
         * on the header gets the whole region discarded.
         *
         * Written last, after the bytes and after the count, so a machine
         * that dies mid-update leaves a mismatch rather than a plausible
         * lie.
         */
        std::uint64_t checksum;
    };

    /**
     * The header has to fit in the space reserved ahead of the payload,
     * and the layout has to be the one both builds expect.
     * @{
     */
    static_assert(sizeof(header) <= payload_offset);
    static_assert(offsetof(header, magic) == 0x00);
    static_assert(offsetof(header, version) == 0x08);
    static_assert(offsetof(header, header_size) == 0x0c);
    static_assert(offsetof(header, capacity) == 0x10);
    static_assert(offsetof(header, written) == 0x18);
    static_assert(offsetof(header, boot_count) == 0x20);
    static_assert(offsetof(header, checksum) == 0x28);
    /**
     * @}
     */

    /**
     * The fixed address has to be page aligned for the loader to be able
     * to ask for it at all, and the region has to be a whole number of
     * pages.
     * @{
     */
    static_assert(!(region_address & 0xfff));
    static_assert(!(region_size & 0xfff));
    /**
     * @}
     */

    /**
     * A log that is not attached to anything. Every operation on one is a
     * no-op, which is what the hypervisor holds when the loader could not
     * reserve the region - so nothing has to be conditional at the call
     * sites.
     */
    constexpr crash_log() = default;

    /**
     * A log over the given region, which must be at least region_size
     * bytes. Explicit rather than converting, because a span of arbitrary
     * bytes is not a crash log and should not silently become one.
     */
    constexpr explicit crash_log(std::span<std::byte> region) :
        m_region(region)
    {
    }

    /**
     * Whether there is a region at all. Checked by everything below, so an
     * unattached log costs one comparison rather than a branch at each
     * caller.
     */
    constexpr bool attached() const
    {
        return m_region.size() >= region_size;
    }

    /**
     * Whether the region holds a log this build can read.
     *
     * The bytes at a fixed physical address are whatever was last left
     * there, which may be a log from the previous boot, a log from a build
     * with a different layout, or memory nobody has ever written. Only the
     * first is decodable, and this is the question that separates them.
     */
    bool valid() const
    {
        if (!attached()) {
            return false;
        }

        const auto & head = header_at();
        return (magic == head.magic) && (version == head.version) &&
               (sizeof(header) == head.header_size) &&
               (capacity == head.capacity) &&
               (checksum_of_header() == head.checksum);
    }

    /**
     * Stamps a fresh header, discarding whatever the region held.
     *
     * Called by the loader once it has claimed the region and read out any
     * previous contents, which is also what marks those contents consumed
     * - clearing the byte count is what stops the same log being reported
     * again on every boot after the one that produced it.
     *
     * The boot count is carried forward by the caller rather than
     * incremented here, because only the caller knows whether the count it
     * read was one it should believe.
     */
    void initialize(std::uint64_t boots)
    {
        if (!attached()) {
            return;
        }

        auto & head = header_at();
        head.magic = magic;
        head.version = version;
        head.header_size = sizeof(header);
        head.capacity = capacity;
        head.written = 0;
        head.boot_count = boots;
        head.checksum = checksum_of_header();
    }

    /**
     * Appends one line, terminated so the result is readable as text.
     *
     * Safe to call from a VM exit handler, which is the constraint that
     * shapes it: no allocation, no lock of its own, and a bounded amount
     * of work regardless of the state of the ring. The caller holds the
     * log's lock, and this deliberately does not take one - zpp::spin_lock
     * is not recursive.
     *
     * A carriage return and a line feed rather than a bare line feed,
     * because the file this ends up in is read off an EFI system partition
     * on whatever machine is to hand, and the loader's own log uses the
     * same pair.
     */
    void append_line(std::span<const char> text)
    {
        if (!attached()) {
            return;
        }

        append(text);
        append(std::span<const char>{"\r\n", 2});
    }

    /**
     * How many bytes have been written this boot, which is not bounded by
     * the capacity - see the header field.
     */
    std::uint64_t written() const
    {
        return attached() ? header_at().written : 0;
    }

    /**
     * How many boots have claimed this region.
     */
    std::uint64_t boot_count() const
    {
        return attached() ? header_at().boot_count : 0;
    }

    /**
     * The contents in order, oldest first, as at most two runs.
     *
     * Two rather than one because the payload is a ring: once it has
     * wrapped, the oldest byte is in the middle and the contents are the
     * tail of the buffer followed by its head. Handed back as spans rather
     * than copied into one, so that a caller with no memory to spare - the
     * loader writes these straight to a file - needs none.
     */
    struct contents
    {
        std::span<const char> first{};
        std::span<const char> second{};
    };

    contents ordered() const
    {
        if (!valid()) {
            return {};
        }

        auto total = header_at().written;
        auto payload = payload_at();

        // Never wrapped, so it is simply the front of the buffer.
        if (total <= capacity) {
            return {payload.first(static_cast<std::size_t>(total)), {}};
        }

        // Wrapped, so the next byte due to be written is also the oldest
        // one present.
        auto start = static_cast<std::size_t>(total % capacity);
        return {payload.subspan(start), payload.first(start)};
    }

private:
    /**
     * Copies a run into the ring, wrapping if it reaches the end.
     *
     * At most two memcpy calls, because a run no longer than the capacity
     * can straddle the end of the buffer once and no more. A run longer
     * than the capacity keeps its tail - the most recent bytes are the
     * ones worth having, and this cannot happen for a log line in
     * practice.
     */
    void append(std::span<const char> text)
    {
        if (text.size() > capacity) {
            text = text.last(capacity);
        }

        auto & head = header_at();
        auto payload = payload_at();
        auto position = static_cast<std::size_t>(head.written % capacity);
        auto until_end = capacity - position;
        auto leading = (text.size() < until_end) ? text.size() : until_end;

        std::memcpy(payload.data() + position, text.data(), leading);
        if (text.size() > leading) {
            std::memcpy(payload.data(),
                        text.data() + leading,
                        text.size() - leading);
        }

        // The count after the bytes and the checksum after the count, so
        // that a machine that stops part way through this leaves something
        // a reader rejects rather than something it believes.
        head.written += text.size();
        head.checksum = checksum_of_header();
    }

    /**
     * The header, in place. Not a copy: every mutator above writes through
     * this, and a copy would make it possible to update one and read the
     * other.
     * @{
     */
    header & header_at()
    {
        return *reinterpret_cast<header *>(m_region.data());
    }

    const header & header_at() const
    {
        return *reinterpret_cast<const header *>(m_region.data());
    }
    /**
     * @}
     */

    /**
     * The payload, as characters, since everything in it is text.
     * @{
     */
    std::span<char> payload_at()
    {
        return {reinterpret_cast<char *>(m_region.data() + payload_offset),
                capacity};
    }

    std::span<const char> payload_at() const
    {
        return {reinterpret_cast<const char *>(m_region.data() +
                                               payload_offset),
                capacity};
    }
    /**
     * @}
     */

    /**
     * FNV-1a over every header byte ahead of the checksum field.
     *
     * Bounded at forty bytes, so this is affordable on the write path. FNV
     * rather than a sum because the fields it covers are mostly small
     * integers and zeroes, and a sum of those collides with other
     * plausible arrangements of the same - which is the case worth telling
     * apart, since the alternative to a valid header is usually a nearly
     * empty one.
     */
    std::uint64_t checksum_of_header() const
    {
        constexpr std::uint64_t offset_basis = 0xcbf29ce484222325ull;
        constexpr std::uint64_t prime = 0x100000001b3ull;

        auto bytes =
            reinterpret_cast<const unsigned char *>(m_region.data());
        auto value = offset_basis;
        for (std::size_t i{}; i < offsetof(header, checksum); ++i) {
            value = (value ^ bytes[i]) * prime;
        }
        return value;
    }

    /**
     * The region, or empty when there is none.
     */
    std::span<std::byte> m_region{};
};

} // namespace zpp
