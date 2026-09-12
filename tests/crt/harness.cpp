/*
 * Regression harness for the freestanding C runtime: zpp::heap, the
 * string and memory primitives, zpp::scope_exit, zpp::allocator and the
 * container aliases, zpp::error, and the init/fini array machinery in
 * crt.cpp.
 *
 * None of this had any coverage before, and all of it is the layer every
 * other layer stands on. A wrong answer here does not fail near itself:
 * the two defects pinned below both surfaced as something else entirely.
 *
 *   b2b66c6  heap::allocate rounded its size up to the default alignment
 *            without checking for wrap, so a huge request became a small
 *            *successful* allocation and the caller wrote off the end of
 *            it. Stated below as allocate(SIZE_MAX) and
 *            allocate(SIZE_MAX - 8) returning null.
 *
 *   b2b66c6  memmove copied forward unconditionally, so any overlapping
 *            range with the destination above the source got the first
 *            byte smeared across it. That is not an exotic call: libc++
 *            lowers std::move_backward on trivially copyable types
 *            straight to memmove, so ordinary container code - a vector
 *            insert - reaches it. Stated below at every destination
 *            offset from -32 to +32.
 *
 *   b2b66c6  zpp::error's converting constructor was unconstrained,
 *            which made the type constructible from anything and sent
 *            std::expected's own constraints into infinite recursion.
 *            Stated below as a static_assert that an int is not an
 *            error.
 *
 * Hosted, native, no emulator and no target - the whole point is that
 * none of this needs one.
 *
 * How crt.cpp is compiled here, and why it is not simply linked. It is a
 * *replacement* C runtime, so linking it into a hosted binary preempts
 * the host's memcpy - destroying the oracle this harness compares
 * against - the host's __cxa_atexit and __dso_handle, and every operator
 * new. That last one routes the standard library's own pre-main
 * initializers through a heap that zpp::crt::init::main() has not brought
 * up yet, which traps: the harness would die at start-up for a reason
 * that says nothing about the hypervisor.
 *
 * So the C-linkage names are renamed with macros below, after every
 * header crt.cpp includes has already been included, and the file is
 * *included* rather than linked. The two things a macro cannot rename
 * are not in it: `operator` is a keyword, so the twenty allocation
 * operators live in crt/operators.cpp, and the six linker-synthesized
 * array bounds cannot be reproduced on a host at all, so they live in
 * crt/init_array.cpp behind three accessors this file supplies instead.
 * Both of those are translation units this harness does not compile.
 *
 * That arrangement replaces one where build.sh generated a real.cpp by
 * deleting those two things out of crt.cpp with awk at build time. The
 * cuts are in the source layout now, so they cannot silently stop
 * matching.
 */
#include "zpp/containers.h"
#include "zpp/crt.h"
#include "zpp/error.h"
#include "zpp/heap.h"
#include "zpp/scope_exit.h"
#include "zpp/spin_lock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <new>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define ZPP_TEST_HAVE_FORK 1
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}", what);
}

void check_equal(std::uint64_t expected,
                 std::uint64_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}\n  expected {} (0x{:x})\n  actual   {} "
                 "(0x{:x})",
                 what,
                 expected,
                 expected,
                 actual,
                 actual);
}

/*
 * Stand-ins for the three linker-synthesized arrays.
 *
 * The real ones are declared in hypervisor/src/crt/init_array.cpp, which
 * this harness does not compile, and the reason is the whole difficulty:
 * the bounds are arrays of unknown bound whose addresses the linker fills
 * in, and only a linked image places two separately declared arrays
 * contiguously. So crt.cpp asks for them through the three accessors
 * below, and the three below are this harness's.
 *
 * Everything the code under test does with them is unchanged - it walks
 * a range forward for the init arrays and backward for the fini array -
 * and a test can aim a range at whatever it likes, which is how the
 * CLAUDE.md claim about the preinit bounds gets stated further down: the
 * walk is bounded by the range and by nothing else.
 */
std::span<const zpp::crt::init::array_entry> g_preinit_array{};
std::span<const zpp::crt::init::array_entry> g_init_array{};
std::span<const zpp::crt::init::array_entry> g_fini_array{};

} // namespace

namespace zpp::crt::init
{
std::span<const array_entry> preinit_array()
{
    return g_preinit_array;
}

std::span<const array_entry> init_array()
{
    return g_init_array;
}

std::span<const array_entry> fini_array()
{
    return g_fini_array;
}

} // namespace zpp::crt::init

// Every header crt.cpp includes is already included above, so no macro
// below is ever live while a system or project header is being parsed.
// They are undefined again immediately after, so the rest of this file
// sees the host's names.
#define memcpy zpp_test_memcpy
#define memmove zpp_test_memmove
#define memset zpp_test_memset
#define memcmp zpp_test_memcmp
#define strlen zpp_test_strlen
#define __cxa_pure_virtual zpp_test_cxa_pure_virtual
#define __cxa_atexit zpp_test_cxa_atexit
#define __cxa_finalize zpp_test_cxa_finalize
#define __cxa_guard_acquire zpp_test_cxa_guard_acquire
#define __cxa_guard_release zpp_test_cxa_guard_release
#define __cxa_guard_abort zpp_test_cxa_guard_abort
#define __dso_handle zpp_test_dso_handle

#include "crt.cpp"

#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#undef __cxa_pure_virtual
#undef __cxa_atexit
#undef __cxa_finalize
#undef __cxa_guard_acquire
#undef __cxa_guard_release
#undef __cxa_guard_abort
#undef __dso_handle

/*
 * crt.cpp is included into this translation unit, so everything in its
 * unnamed namespace is visible from here: g_heap_storage,
 * global_heap_size, max_registered_destructors and
 * g_registered_destructor_count. That is deliberate. It is what lets the
 * tests below assert against the real storage rather than against a copy
 * of its constants. The three allocation helpers are not in it - they
 * are zpp::crt names now, because crt/operators.cpp calls them from
 * another translation unit.
 */

namespace
{

// ---------------------------------------------------------------------
// zpp::heap
// ---------------------------------------------------------------------

constexpr std::size_t header_overhead =
    32; // sizeof(block_header), rounded

unsigned char pattern_byte(std::size_t index)
{
    return static_cast<unsigned char>((index * 31u + 7u) & 0xff);
}

bool is_aligned(const void * pointer, std::size_t alignment)
{
    return !(reinterpret_cast<std::uintptr_t>(pointer) & (alignment - 1));
}

void heap_round_trips()
{
    std::vector<std::byte> arena(64 * 1024);
    zpp::heap heap;

    // A heap that has not been given storage hands out nothing. This is
    // the mechanism behind the CLAUDE.md rule that allocating before
    // crt::init::main() traps: the heap does not check, it simply has an
    // empty free list, and operator new turns the null into a trap.
    check(!heap.allocate(16),
          "a heap with no storage yet allocates nothing");
    check_equal(0, heap.capacity(), "and reports no capacity");

    heap.init(std::span{arena});
    check_equal(arena.size(), heap.capacity(), "capacity is the arena");

    auto * first = static_cast<unsigned char *>(heap.allocate(100));
    check(first, "a 100 byte allocation succeeds");
    check(first >= reinterpret_cast<unsigned char *>(arena.data()) &&
              first + 100 <=
                  reinterpret_cast<unsigned char *>(arena.data()) +
                      arena.size(),
          "and lies inside the arena");

    for (std::size_t i{}; i < 100; ++i) {
        first[i] = pattern_byte(i);
    }

    auto * second = static_cast<unsigned char *>(heap.allocate(100));
    check(second, "a second allocation succeeds");
    check(second + 100 <= first || first + 100 <= second,
          "and does not overlap the first");

    std::size_t disturbed{};
    for (std::size_t i{}; i < 100; ++i) {
        if (first[i] != pattern_byte(i)) {
            ++disturbed;
        }
    }
    check_equal(0,
                disturbed,
                "the second allocation disturbed no byte of the "
                "first");

    heap.deallocate(first);
    heap.deallocate(second);
    heap.deallocate(nullptr); // Must be a no-op, not a fault.

    // init is one-shot. A second call must not reset the free list and
    // orphan every live allocation - which is exactly the reason
    // heap.h's abandon_lock comment gives for not re-running init on a
    // resume.
    std::vector<std::byte> other(4 * 1024);
    heap.init(std::span{other});
    check_equal(arena.size(),
                heap.capacity(),
                "init on an already initialized heap is ignored");

    // abandon_lock is only ever called by a processor that is alone, and
    // there is no way to observe the forced release from outside. What
    // can be stated is that it leaves the heap usable, which is the
    // whole purpose of the call.
    heap.abandon_lock();
    check(heap.allocate(32),
          "the heap still allocates after abandon_lock");
}

void heap_alignment()
{
    std::vector<std::byte> arena(64 * 1024);
    check(is_aligned(arena.data(), zpp::heap::default_alignment),
          "the test arena is itself aligned, or nothing below means "
          "anything");

    zpp::heap heap;
    heap.init(std::span{arena});

    std::size_t misaligned{};
    for (std::size_t size = 1; size <= 200; ++size) {
        auto * pointer = heap.allocate(size);
        if (!pointer) {
            continue;
        }
        if (!is_aligned(pointer, zpp::heap::default_alignment)) {
            ++misaligned;
        }
    }
    check_equal(0,
                misaligned,
                "every allocation is aligned to heap::default_alignment, "
                "for every size from 1 to 200");
}

void heap_rejects_sizes_that_would_wrap()
{
    std::vector<std::byte> arena(64 * 1024);
    zpp::heap heap;
    heap.init(std::span{arena});

    // b2b66c6. allocate rounds up to default_alignment; without the
    // guard the round-up wraps and a request for the whole address space
    // becomes a small allocation that *succeeds*, which is far worse
    // than failing.
    check(!heap.allocate(~std::size_t{}),
          "allocate(SIZE_MAX) fails rather than wrapping - b2b66c6");
    check(!heap.allocate(~std::size_t{} - 8),
          "allocate(SIZE_MAX - 8) fails rather than wrapping - b2b66c6");

    // The largest size that does not wrap. It must still fail, on the
    // ordinary path, because no arena is that large.
    check(
        !heap.allocate(~std::size_t{} - zpp::heap::default_alignment + 1),
        "the largest non-wrapping size still fails for want of room");

    check(!heap.allocate(0), "a zero sized allocation yields nothing");

    // Nothing above may have consumed the arena.
    check(heap.allocate(arena.size() - header_overhead),
          "and none of those failures took any of the arena with them");
}

void heap_coalesces()
{
    constexpr std::size_t arena_size = 64 * 1024;
    std::vector<std::byte> arena(arena_size);
    zpp::heap heap;
    heap.init(std::span{arena});

    // b2b66c6 again, from the other side: allocate/free cycles must not
    // fragment the arena. Without coalescing the arena is permanently
    // cut into the shape of whatever the first workload happened to be.
    for (int round{}; round < 4; ++round) {
        std::vector<void *> blocks;
        for (std::size_t i{}; i < 64; ++i) {
            auto * pointer = heap.allocate(256);
            if (!pointer) {
                break;
            }
            blocks.push_back(pointer);
        }
        check_equal(64,
                    blocks.size(),
                    "round " + std::to_string(round) +
                        " fits 64 blocks of 256 bytes");

        // Free in an order that leaves holes if nothing merges: every
        // other block first, then the rest.
        for (std::size_t i{}; i < blocks.size(); i += 2) {
            heap.deallocate(blocks[i]);
        }
        for (std::size_t i = 1; i < blocks.size(); i += 2) {
            heap.deallocate(blocks[i]);
        }
    }

    auto * whole = heap.allocate(arena_size - header_overhead);
    check(whole,
          "after four allocate/free rounds the arena is one free block "
          "again, and nearly all of it can be handed out at once - "
          "b2b66c6");
    heap.deallocate(whole);
}

void heap_exhaustion_returns_null()
{
    std::vector<std::byte> arena(16 * 1024);
    zpp::heap heap;
    heap.init(std::span{arena});

    std::vector<void *> blocks;
    while (auto * pointer = heap.allocate(512)) {
        blocks.push_back(pointer);
    }

    // The point is that the loop above terminated by being told no,
    // rather than by trapping or by handing out memory twice.
    check(!blocks.empty(), "an exhaustible arena hands out something");
    check(!heap.allocate(512),
          "and then reports exhaustion by returning null");

    heap.deallocate(blocks.back());
    blocks.back() = heap.allocate(512);
    check(blocks.back(), "freeing one block makes room for one more");

    // A refused 512 does not mean the arena is empty - split_block
    // leaves remainders too small for that request but big enough for a
    // smaller one. Drain those the same way, and the same answer has to
    // come back.
    std::size_t crumbs{};
    while (auto * pointer = heap.allocate(1)) {
        blocks.push_back(pointer);
        if (++crumbs > arena.size()) {
            break; // A heap that never says no is the failure below.
        }
    }
    check(crumbs <= arena.size(),
          "draining the arena a byte at a time terminates, rather than "
          "handing out memory that is not there");
    check(!heap.allocate(1),
          "and the last word is null, not a trap and not a duplicate "
          "pointer");
}

/**
 * A deterministic generator, deliberately written out rather than taken
 * from <random>: the distributions there are not specified to produce
 * the same sequence across implementations, and a soak whose sequence
 * differs between macOS and the CI runner is a soak that cannot be
 * reproduced from a failure report.
 */
std::uint64_t next_random(std::uint64_t & state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}

void heap_soak()
{
    constexpr std::size_t arena_size = 256 * 1024;
    std::vector<std::byte> arena(arena_size);
    zpp::heap heap;
    heap.init(std::span{arena});

    auto * base = reinterpret_cast<unsigned char *>(arena.data());

    struct live_block
    {
        unsigned char * pointer;
        std::size_t size;
        unsigned char fill;
    };

    std::vector<live_block> live;
    std::uint64_t state{0x5eed'0000'0000'0001ull};

    std::size_t escaped_the_arena{};
    std::size_t overlapped{};
    std::size_t corrupted{};
    std::size_t misaligned{};
    std::size_t allocations{};

    for (std::size_t step{}; step < 20000; ++step) {
        auto grow =
            live.empty() || (live.size() < 96 && (next_random(state) & 1));

        if (grow) {
            auto size =
                static_cast<std::size_t>(next_random(state) % 512) + 1;
            auto * pointer =
                static_cast<unsigned char *>(heap.allocate(size));
            if (!pointer) {
                // A full arena is not a defect. Drop something and move
                // on.
                auto victim = next_random(state) % live.size();
                heap.deallocate(live[victim].pointer);
                live.erase(live.begin() +
                           static_cast<std::ptrdiff_t>(victim));
                continue;
            }

            ++allocations;
            if (pointer < base || pointer + size > base + arena_size) {
                ++escaped_the_arena;
            }
            if (!is_aligned(pointer, zpp::heap::default_alignment)) {
                ++misaligned;
            }
            for (const auto & other : live) {
                if (pointer < other.pointer + other.size &&
                    other.pointer < pointer + size) {
                    ++overlapped;
                    break;
                }
            }

            auto fill = static_cast<unsigned char>(next_random(state));
            std::memset(pointer, fill, size);
            live.push_back({pointer, size, fill});
            continue;
        }

        auto victim = next_random(state) % live.size();
        const auto & block = live[victim];
        for (std::size_t i{}; i < block.size; ++i) {
            if (block.pointer[i] != block.fill) {
                ++corrupted;
                break;
            }
        }
        heap.deallocate(block.pointer);
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
    }

    for (const auto & block : live) {
        for (std::size_t i{}; i < block.size; ++i) {
            if (block.pointer[i] != block.fill) {
                ++corrupted;
                break;
            }
        }
    }

    check(allocations > 5000,
          "the soak actually allocated something - " +
              std::to_string(allocations) + " times");
    check_equal(0, escaped_the_arena, "no allocation escaped the arena");
    check_equal(0, overlapped, "no two live allocations overlapped");
    check_equal(0,
                corrupted,
                "no live allocation's contents were disturbed by a "
                "later allocate or deallocate");
    check_equal(0, misaligned, "every allocation stayed aligned");

    for (const auto & block : live) {
        heap.deallocate(block.pointer);
    }
    auto * whole = heap.allocate(arena_size - header_overhead);
    check(whole,
          "and after a randomised workload the whole arena coalesces "
          "back into one block");
    heap.deallocate(whole);
}

// ---------------------------------------------------------------------
// The over-alignment helpers that operator new(align_val_t) calls
// ---------------------------------------------------------------------

void aligned_allocation()
{
    // The three helpers in crt.cpp that the allocation operators call.
    // The operators themselves are in crt/operators.cpp and are not
    // compiled here - a hosted binary cannot have them without routing
    // the standard library's own allocations through the arena - but the
    // helpers are, because the over-allocate-and-stash arithmetic is
    // where anything interesting could go wrong.
    static constexpr std::size_t alignments[] = {
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    static constexpr std::size_t sizes[] = {1, 16, 100, 1000, 4096};

    auto * arena_begin = reinterpret_cast<unsigned char *>(g_heap_storage);
    auto * arena_end = arena_begin + global_heap_size;

    std::size_t misaligned{};
    std::size_t escaped{};

    for (auto alignment : alignments) {
        for (auto size : sizes) {
            auto * pointer = static_cast<unsigned char *>(
                zpp::crt::allocate_aligned_or_trap(size, alignment));
            if (!is_aligned(pointer, alignment)) {
                ++misaligned;
            }
            if (pointer < arena_begin || pointer + size > arena_end) {
                ++escaped;
            }
            // The stashed base pointer sits just below the returned
            // address for over-aligned requests; writing the whole
            // allocation must not disturb it, which the deallocate
            // below is what checks.
            std::memset(pointer, 0xa5, size);
            zpp::crt::deallocate_aligned(pointer, alignment);
        }
    }

    check_equal(0,
                misaligned,
                "allocate_aligned_or_trap honours every power of two "
                "alignment from 1 to 4096");
    check_equal(0,
                escaped,
                "and every over-aligned allocation stays inside the "
                "global arena, including the bytes past the returned "
                "address");

    // Repeat the whole sweep. If deallocate_aligned recovered the wrong
    // base pointer the free list would be corrupt by now.
    std::size_t second_pass_failures{};
    for (auto alignment : alignments) {
        auto * pointer = static_cast<unsigned char *>(
            zpp::crt::allocate_aligned_or_trap(4096, alignment));
        if (!pointer || !is_aligned(pointer, alignment)) {
            ++second_pass_failures;
        }
        zpp::crt::deallocate_aligned(pointer, alignment);
    }
    check_equal(0,
                second_pass_failures,
                "the arena survives a second over-aligned sweep, so "
                "deallocate_aligned recovered the right base each time");

    zpp::crt::deallocate_aligned(nullptr,
                                 4096); // Must be a no-op, not a fault.
    check(true, "deallocate_aligned(nullptr) is a no-op");

    auto * plain = zpp::crt::allocate_or_trap(0);
    check(plain,
          "a zero sized allocation still yields a unique pointer, since "
          "two objects may not share an address");
    zpp::crt::heap().deallocate(plain);
}

// ---------------------------------------------------------------------
// memcpy / memmove / memset / memcmp / strlen
// ---------------------------------------------------------------------

constexpr std::size_t buffer_size = 192;

void memory_copy()
{
    // Non-overlapping ranges only: memcpy's contract says nothing about
    // overlap, and memory_move below is where overlap is stated.
    static constexpr std::size_t lengths[] = {
        0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127};

    std::size_t mismatches{};
    std::size_t wrong_returns{};
    std::size_t overruns{};

    std::vector<unsigned char> source(buffer_size);
    for (std::size_t i{}; i < buffer_size; ++i) {
        source[i] = pattern_byte(i);
    }

    for (auto length : lengths) {
        for (std::size_t from{}; from < 16; ++from) {
            for (std::size_t to{}; to < 16; ++to) {
                std::vector<unsigned char> destination(buffer_size, 0xcd);
                auto * returned = zpp_test_memcpy(
                    destination.data() + to, source.data() + from, length);
                if (returned != destination.data() + to) {
                    ++wrong_returns;
                }
                for (std::size_t i{}; i < length; ++i) {
                    if (destination[to + i] != source[from + i]) {
                        ++mismatches;
                        break;
                    }
                }
                for (std::size_t i{}; i < buffer_size; ++i) {
                    if (i >= to && i < to + length) {
                        continue;
                    }
                    if (destination[i] != 0xcd) {
                        ++overruns;
                        break;
                    }
                }
            }
        }
    }

    check_equal(0,
                mismatches,
                "memcpy copies every byte, at every combination of "
                "source and destination alignment and every length up "
                "to 127");
    check_equal(0, wrong_returns, "memcpy returns its destination");
    check_equal(0,
                overruns,
                "memcpy writes nothing outside the range it was given");
}

void memory_move()
{
    // Every destination offset from -32 to +32 relative to the source,
    // which is the sweep b2b66c6 would have failed: it copied forward
    // unconditionally, so every positive offset smaller than the length
    // smeared the leading bytes across the range.
    //
    // Two oracles, because either alone is weaker than it looks. The
    // host's memmove is the standard's own answer, and an expectation
    // computed byte by byte from the untouched original is independent
    // of the host having got it right.
    static constexpr std::size_t lengths[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64};
    constexpr std::size_t source_index = 64;

    std::vector<unsigned char> original(buffer_size);
    for (std::size_t i{}; i < buffer_size; ++i) {
        original[i] = pattern_byte(i);
    }

    std::size_t disagreements{};
    std::size_t against_host{};
    std::size_t wrong_returns{};
    int first_bad_offset{};
    std::size_t first_bad_length{};

    for (int offset = -32; offset <= 32; ++offset) {
        for (auto length : lengths) {
            auto destination_index = static_cast<std::size_t>(
                static_cast<int>(source_index) + offset);

            auto expected = original;
            for (std::size_t i{}; i < length; ++i) {
                expected[destination_index + i] =
                    original[source_index + i];
            }

            auto host = original;
            std::memmove(host.data() + destination_index,
                         host.data() + source_index,
                         length);

            auto actual = original;
            auto * returned =
                zpp_test_memmove(actual.data() + destination_index,
                                 actual.data() + source_index,
                                 length);

            if (returned != actual.data() + destination_index) {
                ++wrong_returns;
            }
            if (actual != expected) {
                if (!disagreements) {
                    first_bad_offset = offset;
                    first_bad_length = length;
                }
                ++disagreements;
            }
            if (actual != host) {
                ++against_host;
            }
        }
    }

    check_equal(0,
                disagreements,
                "memmove agrees with a byte by byte expectation at every "
                "destination offset from -32 to +32 - b2b66c6 copied "
                "forward unconditionally; first disagreement at offset " +
                    std::to_string(first_bad_offset) + " length " +
                    std::to_string(first_bad_length));
    check_equal(0,
                against_host,
                "memmove agrees with the host's memmove over the same "
                "sweep");
    check_equal(0, wrong_returns, "memmove returns its destination");

    // The exact shape of the defect, stated on its own so a failure
    // names it rather than leaving it inside a sweep of 910 cases.
    // libc++ lowers std::move_backward on a trivially copyable type
    // straight to memmove, so a vector insert reaches this.
    std::vector<unsigned char> smear(64);
    for (std::size_t i{}; i < smear.size(); ++i) {
        smear[i] = static_cast<unsigned char>(i);
    }
    zpp_test_memmove(smear.data() + 1, smear.data(), 32);
    bool smeared{};
    for (std::size_t i{}; i < 32; ++i) {
        if (smear[1 + i] != static_cast<unsigned char>(i)) {
            smeared = true;
        }
    }
    check(!smeared,
          "moving a range one byte forward shifts it rather than "
          "smearing its first byte across it - b2b66c6");
}

void memory_set()
{
    // Signed and out of range values included on purpose: memset takes
    // an int and stores it converted to unsigned char, so 0x15a must
    // write 0x5a and -1 must write 0xff.
    static constexpr int values[] = {0, 1, 0x5a, 0xff, 0x15a, -1};
    static constexpr std::size_t lengths[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 64, 127};

    std::size_t mismatches{};
    std::size_t overruns{};
    std::size_t wrong_returns{};

    for (auto value : values) {
        for (auto length : lengths) {
            for (std::size_t at{}; at < 16; ++at) {
                std::vector<unsigned char> host(buffer_size, 0xcd);
                std::vector<unsigned char> actual(buffer_size, 0xcd);

                std::memset(host.data() + at, value, length);
                auto * returned =
                    zpp_test_memset(actual.data() + at, value, length);

                if (returned != actual.data() + at) {
                    ++wrong_returns;
                }
                if (actual != host) {
                    ++mismatches;
                }
                for (std::size_t i{}; i < buffer_size; ++i) {
                    if (i >= at && i < at + length) {
                        continue;
                    }
                    if (actual[i] != 0xcd) {
                        ++overruns;
                        break;
                    }
                }
            }
        }
    }

    check_equal(0,
                mismatches,
                "memset agrees with the host for every value, including "
                "ones outside the range of an unsigned char, at every "
                "alignment and length");
    check_equal(0, wrong_returns, "memset returns its destination");
    check_equal(
        0, overruns, "memset writes nothing outside the range given");
}

int sign_of(int value)
{
    return (value > 0) - (value < 0);
}

void memory_compare()
{
    // Only the sign is compared. The standard fixes the sign of
    // memcmp's result and leaves the magnitude to the implementation,
    // so demanding equality with the host would pin something that was
    // never promised.
    std::size_t disagreements{};

    for (std::size_t length = 1; length <= 64; ++length) {
        for (std::size_t at{}; at < length; ++at) {
            std::vector<unsigned char> left(length);
            for (std::size_t i{}; i < length; ++i) {
                left[i] = pattern_byte(i);
            }
            auto right = left;
            right[at] = static_cast<unsigned char>(left[at] + 1);

            if (sign_of(
                    zpp_test_memcmp(left.data(), right.data(), length)) !=
                sign_of(std::memcmp(left.data(), right.data(), length))) {
                ++disagreements;
            }
            if (sign_of(
                    zpp_test_memcmp(right.data(), left.data(), length)) !=
                sign_of(std::memcmp(right.data(), left.data(), length))) {
                ++disagreements;
            }
            if (zpp_test_memcmp(left.data(), left.data(), length)) {
                ++disagreements;
            }
        }
    }

    check_equal(0,
                disagreements,
                "memcmp agrees in sign with the host at every difference "
                "position and every length up to 64");

    check_equal(0,
                static_cast<std::uint64_t>(
                    zpp_test_memcmp(nullptr, nullptr, 0) == 0 ? 0 : 1),
                "a zero length comparison is equal, and reads nothing");

    // Bytes compare as unsigned char, not as char. On a target where
    // char is signed the naive spelling gets this backwards, and 0x80
    // would sort below 0x00.
    unsigned char low[] = {0x00};
    unsigned char high[] = {0x80};
    check(zpp_test_memcmp(low, high, 1) < 0,
          "0x00 compares below 0x80: bytes are compared as unsigned");
    check(zpp_test_memcmp(high, low, 1) > 0,
          "and 0x80 compares above 0x00");

    // The first difference decides, and nothing past it is read.
    unsigned char first[] = {1, 9, 9};
    unsigned char second[] = {2, 0, 0};
    check(zpp_test_memcmp(first, second, 3) < 0,
          "the first differing byte decides the result");
}

void string_length()
{
    std::size_t disagreements{};
    for (std::size_t length{}; length <= 64; ++length) {
        std::vector<char> text(length + 1);
        for (std::size_t i{}; i < length; ++i) {
            // Never a zero byte before the terminator.
            text[i] = static_cast<char>('a' + (i % 26));
        }
        text[length] = '\0';

        if (zpp_test_strlen(text.data()) != std::strlen(text.data())) {
            ++disagreements;
        }
        if (zpp_test_strlen(text.data()) != length) {
            ++disagreements;
        }
    }
    check_equal(0,
                disagreements,
                "strlen agrees with the host for every length from 0 to "
                "64");

    const char embedded[] = {'a', 'b', '\0', 'c', 'd', '\0'};
    check_equal(2,
                zpp_test_strlen(embedded),
                "strlen stops at the first terminator, not the last");
}

// ---------------------------------------------------------------------
// zpp::scope_exit
// ---------------------------------------------------------------------

void scope_exit_runs_once()
{
    int count{};
    {
        zpp::scope_exit guard{[&] { ++count; }};
        check_equal(
            0, count, "scope_exit does not run its function eagerly");
    }
    check_equal(1, count, "scope_exit runs its function at scope exit");

    {
        zpp::scope_exit guard{[&] { ++count; }};
    }
    check_equal(2, count, "and runs it exactly once per guard, not more");
}

void scope_exit_release()
{
    int count{};
    {
        zpp::scope_exit guard{[&] { ++count; }};
        guard.release();
    }
    check_equal(0, count, "release() stops the function running");

    // Releasing twice is not an error, and does not resurrect it.
    {
        zpp::scope_exit guard{[&] { ++count; }};
        guard.release();
        guard.release();
    }
    check_equal(0, count, "releasing twice is still released");
}

void scope_exit_move_transfers_ownership()
{
    int count{};
    {
        zpp::scope_exit first{[&] { ++count; }};
        {
            // Spelled with decltype rather than by class template
            // argument deduction: the deduction guide would deduce
            // scope_exit<scope_exit<L>> from a scope_exit argument,
            // which is a wrapper around a wrapper and not what a move
            // means.
            decltype(first) second{std::move(first)};
            check_equal(
                0, count, "moving a guard does not run its function");
        }
        check_equal(1,
                    count,
                    "the moved-to guard runs the function at its own "
                    "scope exit");
    }
    check_equal(1,
                count,
                "and the moved-from guard does not run it a second "
                "time - move transfers ownership");
}

void scope_exit_runs_on_every_exit_path()
{
    // There are no exceptions here (-fno-exceptions), so the paths out
    // of a scope are the ordinary ones: falling off the end, return,
    // break, continue and goto.
    int count{};

    auto early_return = [&] {
        zpp::scope_exit guard{[&] { ++count; }};
        if (count == 0) {
            return;
        }
        ++count;
    };
    early_return();
    check_equal(1, count, "scope_exit runs on the return path");

    count = 0;
    for (int i{}; i < 4; ++i) {
        zpp::scope_exit guard{[&] { ++count; }};
        if (i == 2) {
            break;
        }
    }
    check_equal(
        3, count, "scope_exit runs on each iteration and on the break");

    count = 0;
    for (int i{}; i < 4; ++i) {
        zpp::scope_exit guard{[&] { ++count; }};
        if (i % 2) {
            continue;
        }
    }
    check_equal(4, count, "scope_exit runs on the continue path");

    count = 0;
    {
        zpp::scope_exit guard{[&] { ++count; }};
        goto out;
    }
out:
    check_equal(1, count, "scope_exit runs on the goto path");
}

// ---------------------------------------------------------------------
// zpp::allocator and the container aliases
// ---------------------------------------------------------------------

/**
 * True when T{} is usable in a constant expression, which is the
 * property CLAUDE.md attaches a cost to: zpp::allocator's default
 * constructor calls crt::heap() and so is not constexpr, which is why a
 * container given static storage duration costs an .init_array entry
 * rather than being baked into the binary.
 *
 * There is no standard trait for this, so it is spelled as a
 * substitution failure on a template argument that has to be a constant
 * expression.
 */
template <typename T, typename = void>
struct constant_default_constructible : std::false_type
{
};

template <typename T>
struct constant_default_constructible<
    T,
    std::void_t<std::integral_constant<int, (T{}, 0)>>> : std::true_type
{
};

static_assert(constant_default_constructible<int>::value,
              "the detector says yes to something that plainly is "
              "constant default constructible");
static_assert(constant_default_constructible<zpp::heap>::value,
              "zpp::heap is, which is why the global heap is constinit");
static_assert(
    !constant_default_constructible<zpp::allocator<int>>::value,
    "zpp::allocator's default constructor is NOT constexpr - it calls "
    "crt::heap(). That is what makes a namespace scope container cost "
    "an .init_array entry, and CLAUDE.md's advice to prefer locals or "
    "members follows from it. If this ever starts passing, the cost "
    "changed and the documentation is stale");

constinit zpp::heap g_probe_heap{};

// The other constructor is constexpr, and stays that way: an allocator
// built against an explicit heap is usable in constant initialization.
constexpr zpp::allocator<int> g_probe_allocator{g_probe_heap};

void containers_route_through_the_global_heap()
{
    auto * arena_begin =
        reinterpret_cast<const unsigned char *>(g_heap_storage);
    auto * arena_end = arena_begin + global_heap_size;

    auto in_arena = [&](const void * pointer) {
        auto * address = reinterpret_cast<const unsigned char *>(pointer);
        return address >= arena_begin && address < arena_end;
    };

    zpp::vector<int> numbers;
    for (int i{}; i < 1000; ++i) {
        numbers.push_back(i);
    }
    check_equal(1000,
                numbers.size(),
                "zpp::vector holds what it was "
                "given");
    check(numbers.front() == 0 && numbers.back() == 999,
          "and holds the right values");
    check(in_arena(numbers.data()),
          "zpp::vector's storage is inside the global heap arena");

    // Long enough that no small-string optimization can hold it, on
    // either standard library.
    zpp::string text(200, 'z');
    text += "tail";
    check_equal(204, text.size(), "zpp::string appends");
    check(text.back() == 'l', "and keeps its contents");
    check(in_arena(text.data()),
          "zpp::string's storage is inside the global heap arena");

    zpp::map<int, int> lookup;
    for (int i{}; i < 100; ++i) {
        lookup.emplace(i, i * 3);
    }
    check_equal(100, lookup.size(), "zpp::map holds its entries");
    check_equal(297, lookup.at(99), "and finds them again");
    check(in_arena(std::addressof(*lookup.begin())),
          "zpp::map's nodes are inside the global heap arena");

    zpp::list<int> chain;
    for (int i{}; i < 100; ++i) {
        chain.push_back(i);
    }
    check_equal(100, chain.size(), "zpp::list holds its entries");
    check(in_arena(std::addressof(*chain.begin())),
          "zpp::list's nodes are inside the global heap arena");

    zpp::set<int> unique;
    for (int i{}; i < 100; ++i) {
        unique.insert(i % 50);
    }
    check_equal(50, unique.size(), "zpp::set deduplicates");
    check(in_arena(std::addressof(*unique.begin())),
          "zpp::set's nodes are inside the global heap arena");

    // Two default constructed allocators are equal because they name the
    // same heap; one built against another heap is not. That is what
    // lets containers move storage between each other.
    check(zpp::allocator<int>{} == zpp::allocator<int>{},
          "default constructed allocators compare equal");
    check(!(zpp::allocator<int>{} == zpp::allocator<int>{g_probe_heap}),
          "an allocator on a different heap does not");
    check(g_probe_allocator == zpp::allocator<int>{g_probe_heap},
          "and the constexpr constructed one names the heap it was "
          "given");

    // The freed memory has to come back, or the fixed 20 MB arena is a
    // budget rather than a heap. Done through the allocator rather than
    // through a container so that exhaustion reports null here instead
    // of being dereferenced somewhere inside libc++.
    zpp::allocator<std::byte> bytes;
    std::size_t refusals{};
    for (int i{}; i < 1000; ++i) {
        auto * block = bytes.allocate(1024 * 1024);
        if (!block) {
            ++refusals;
            break;
        }
        bytes.deallocate(block, 1024 * 1024);
    }
    check_equal(0,
                refusals,
                "a thousand one megabyte round trips fit in a twenty "
                "megabyte arena, so the heap really does reuse freed "
                "memory");
}

// ---------------------------------------------------------------------
// zpp::error and std::expected
// ---------------------------------------------------------------------

} // namespace

namespace harness_errors
{
enum class code
{
    success = 0,
    wrong = 1,
    worse = 2,
};

inline const zpp::error_category & category(code)
{
    static constexpr auto error_category =
        zpp::make_error_category("harness_errors::code",
                                 code::success,
                                 [](auto value) -> std::string_view {
                                     switch (value) {
                                     case code::success:
                                         return zpp::error::no_error;
                                     case code::wrong:
                                         return "wrong";
                                     case code::worse:
                                         return "worse";
                                     }
                                     return "unknown";
                                 });
    return error_category;
}
} // namespace harness_errors

namespace
{

// b2b66c6. The converting constructor is constrained to enumerations. An
// unconstrained one makes zpp::error constructible from *anything*,
// which drives std::expected's own constraints - which ask whether the
// error type is constructible from the value type and vice versa - into
// infinite recursion. These are the assertions that stop that
// constraint being quietly deleted; the file compiling at all is the
// other half of the test.
static_assert(!std::is_constructible_v<zpp::error, int>,
              "zpp::error must not be constructible from an int - "
              "b2b66c6");
static_assert(!std::is_constructible_v<zpp::error, const char *>,
              "nor from a pointer");
static_assert(!std::is_constructible_v<zpp::error, double>,
              "nor from a double");
static_assert(!std::is_default_constructible_v<zpp::error>,
              "and there is no such thing as a default error");
static_assert(std::is_constructible_v<zpp::error, harness_errors::code>,
              "but an error code enumeration is exactly what it is for");
static_assert(
    std::is_constructible_v<std::expected<int, zpp::error>, int>,
    "std::expected<int, zpp::error> is constructible from its value "
    "type, which is the constraint that used to recurse");

std::expected<int, zpp::error> succeeds()
{
    return 7;
}

std::expected<int, zpp::error> fails()
{
    return std::unexpected(zpp::error{harness_errors::code::worse});
}

std::expected<void, zpp::error> fails_with_nothing_to_return()
{
    return std::unexpected(zpp::error{harness_errors::code::wrong});
}

void errors_carry_their_category()
{
    auto good = succeeds();
    check(good.has_value(), "a successful expected has a value");
    check_equal(7, *good, "and it is the one returned");

    auto bad = fails();
    check(!bad.has_value(), "a failed expected has none");
    check_equal(static_cast<int>(harness_errors::code::worse),
                static_cast<std::uint64_t>(bad.error().code()),
                "the error carries its code through std::expected");
    check(bad.error().message() == "worse",
          "and its category answers for the message");
    check(bad.error().category().name() == "harness_errors::code",
          "and names itself");

    // operator bool asks whether the code is the category's success
    // code, so a real failure is false.
    check(!static_cast<bool>(bad.error()),
          "a failure code does not test as success");
    check(static_cast<bool>(zpp::error{harness_errors::code::success}),
          "the success code does");

    auto nothing = fails_with_nothing_to_return();
    check(!nothing.has_value(),
          "std::expected<void, zpp::error> carries a failure too");
    check_equal(static_cast<int>(harness_errors::code::wrong),
                static_cast<std::uint64_t>(nothing.error().code()),
                "with its own code");
}

// ---------------------------------------------------------------------
// The init and fini array machinery
// ---------------------------------------------------------------------

std::vector<int> g_sequence;

void record(int value)
{
    g_sequence.push_back(value);
}

void preinit_must_not_run()
{
    record(800);
}
void init_first()
{
    record(101);
}
void init_second()
{
    record(102);
}
void init_third()
{
    record(103);
}
void init_past_the_end_first()
{
    record(901);
}
void init_past_the_end_second()
{
    record(902);
}
void fini_first()
{
    record(1000001);
}
void fini_second()
{
    record(1000002);
}
void fini_third()
{
    record(1000003);
}

zpp::crt::init::array_entry g_preinit_storage[] = {preinit_must_not_run};
zpp::crt::init::array_entry g_init_storage[] = {init_first,
                                                init_second,
                                                init_third,
                                                init_past_the_end_first,
                                                init_past_the_end_second};
zpp::crt::init::array_entry g_fini_storage[] = {
    fini_first, fini_second, fini_third};

void the_init_arrays_run_forward()
{
    // The preinit bounds are set equal, which is the state CLAUDE.md
    // records on the real target: lld gives both symbols link-time
    // address 0, PC-relative addressing turns that into the module base
    // at runtime, and the loop is a no-op only because they are equal.
    // The array they both point at holds a function that must therefore
    // never run - if the loop ever walked from the module base instead,
    // this is what would catch it.
    g_preinit_array = std::span(g_preinit_storage).first(0);

    // The init bounds cover the first three of five entries. The last
    // two exist so that "runs the array" and "runs from start to end"
    // are different claims: a loop bounded by anything other than the
    // two symbols would reach them.
    g_init_array = std::span(g_init_storage).first(3);

    zpp::crt::init::main();

    check_equal(3,
                g_sequence.size(),
                "crt::init::main() ran exactly the three init array "
                "entries between the two symbols");
    if (g_sequence.size() == 3) {
        check_equal(101, g_sequence[0], "in forward order, first");
        check_equal(102, g_sequence[1], "in forward order, second");
        check_equal(103, g_sequence[2], "in forward order, third");
    }

    std::size_t forbidden{};
    for (auto value : g_sequence) {
        if (value == 800 || value == 901 || value == 902) {
            ++forbidden;
        }
    }
    check_equal(0,
                forbidden,
                "the preinit loop did nothing because its two bounds are "
                "equal, and the init loop stopped at its end symbol "
                "rather than running off the array");

    check_equal(global_heap_size,
                zpp::crt::heap().capacity(),
                "and the global heap was brought up before any of them, "
                "so a constructor is free to allocate");

    // Every CPU may call this. Only the first call may do anything.
    zpp::crt::init::main();
    check_equal(3,
                g_sequence.size(),
                "a second call to crt::init::main() runs nothing again");

    g_sequence.clear();
}

void the_destructor_registry_is_fixed_and_ordered()
{
    check_equal(2048,
                max_registered_destructors,
                "the destructor registry is a fixed 2048 entries - it is "
                "written to from inside constructors and must not "
                "allocate");
    check_equal(0,
                g_registered_destructor_count,
                "and nothing has registered into it yet");
}

void cxa_finalize_defers_to_cleanup()
{
    // __cxa_finalize deliberately does nothing: ordering the registered
    // destructors against the fini array is cleanup()'s job, and doing
    // half of it here would make that ordering implicit.
    zpp_test_cxa_atexit(
        [](void * argument) {
            record(static_cast<int>(
                reinterpret_cast<std::uintptr_t>(argument)));
        },
        reinterpret_cast<void *>(std::uintptr_t{1}),
        nullptr);

    check_equal(1,
                g_registered_destructor_count,
                "__cxa_atexit records the registration");

    zpp_test_cxa_finalize(nullptr);
    check_equal(0,
                g_sequence.size(),
                "__cxa_finalize runs nothing - cleanup() owns the "
                "ordering");
    check_equal(1, g_registered_destructor_count, "and drops nothing");
}

void the_guard_functions_are_a_complete_abi()
{
    // -fno-threadsafe-statics means the compiler emits no calls to
    // these, so they are inert on the target. They exist so the ABI is
    // complete if the flag is ever dropped, and an inert function is
    // exactly the kind that rots unnoticed.
    alignas(8) std::int64_t guard{};

    check_equal(
        1,
        static_cast<std::uint64_t>(zpp_test_cxa_guard_acquire(&guard)),
        "the first acquire says initialize it");

    zpp_test_cxa_guard_release(&guard);
    check_equal(1,
                static_cast<std::uint64_t>(
                    reinterpret_cast<unsigned char *>(&guard)[0]),
                "release marks byte zero, which is where the Itanium ABI "
                "records that initialization completed");
    check_equal(0,
                static_cast<std::uint64_t>(
                    reinterpret_cast<unsigned char *>(&guard)[1]),
                "and drops the spin lock in byte one");
    check_equal(
        0,
        static_cast<std::uint64_t>(zpp_test_cxa_guard_acquire(&guard)),
        "a later acquire says it is already done");

    std::int64_t aborted{};
    check_equal(
        1,
        static_cast<std::uint64_t>(zpp_test_cxa_guard_acquire(&aborted)),
        "acquiring an untouched guard says initialize it");
    zpp_test_cxa_guard_abort(&aborted);
    check_equal(0,
                static_cast<std::uint64_t>(
                    reinterpret_cast<unsigned char *>(&aborted)[0]),
                "abort leaves initialization unfinished");
    check_equal(
        1,
        static_cast<std::uint64_t>(zpp_test_cxa_guard_acquire(&aborted)),
        "so the next acquire is told to try again");
    zpp_test_cxa_guard_release(&aborted);
}

void a_full_destructor_registry_traps()
{
    // The registry signals overflow with __builtin_trap() and nothing
    // else - there is no return code and no channel to report on, which
    // is the point: dropping a destructor would silently leak whatever
    // it releases. So the assertion cannot be made in this process, and
    // is made in a forked child instead: the child fills the registry to
    // its capacity, registers one more, and the parent asserts it died
    // of it. A registry that silently dropped the last one would leave
    // the child exiting 0.
    //
    // Where fork is unavailable this is simply not stated, and says so.
#if defined(ZPP_TEST_HAVE_FORK)
    std::fflush(nullptr);

    auto child = fork();
    if (!child) {
        // No core file and no crash reporter interest in a death this
        // test is expecting.
        rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);

        while (g_registered_destructor_count <
               max_registered_destructors) {
            zpp_test_cxa_atexit([](void *) {}, nullptr, nullptr);
        }

        // One past capacity. This call must not return.
        zpp_test_cxa_atexit([](void *) {}, nullptr, nullptr);
        _exit(0);
    }

    int status{};
    waitpid(child, &status, 0);
    check(WIFSIGNALED(status),
          "registering past the end of the destructor registry kills the "
          "process rather than silently dropping the destructor");
    if (WIFSIGNALED(status)) {
        auto signal_number = WTERMSIG(status);
        check(signal_number == SIGILL || signal_number == SIGTRAP ||
                  signal_number == SIGABRT,
              "and it dies of __builtin_trap - signal " +
                  std::to_string(signal_number));
    }
#else
    std::println("note: no fork(), the registry overflow trap is not "
                 "stated on this platform");
#endif
}

void cleanup_runs_destructors_then_the_fini_array_in_reverse()
{
    g_sequence.clear();

    // Fill the registry to exactly its capacity. Entry 1 was registered
    // by cxa_finalize_defers_to_cleanup, so this starts at 2.
    auto destructor = [](void * argument) {
        record(
            static_cast<int>(reinterpret_cast<std::uintptr_t>(argument)));
    };
    std::size_t refusals{};
    for (std::size_t i = g_registered_destructor_count + 1;
         i <= max_registered_destructors;
         ++i) {
        if (zpp_test_cxa_atexit(
                destructor, reinterpret_cast<void *>(i), nullptr)) {
            ++refusals;
        }
    }
    check_equal(0,
                refusals,
                "__cxa_atexit accepts every registration "
                "that fits");
    check_equal(max_registered_destructors,
                g_registered_destructor_count,
                "the registry fills to exactly its capacity");

    g_fini_array = std::span(g_fini_storage).first(3);

    zpp::crt::init::cleanup();

    check_equal(max_registered_destructors + 3,
                g_sequence.size(),
                "cleanup() ran every registered destructor and every "
                "fini array entry");

    if (g_sequence.size() != max_registered_destructors + 3) {
        return;
    }

    std::size_t out_of_order{};
    for (std::size_t i{}; i < max_registered_destructors; ++i) {
        if (g_sequence[i] !=
            static_cast<int>(max_registered_destructors - i)) {
            ++out_of_order;
        }
    }
    check_equal(0,
                out_of_order,
                "the __cxa_atexit destructors ran in reverse order of "
                "registration, all 2048 of them, so an object is "
                "destroyed before anything it was built on");

    check_equal(1000003,
                g_sequence[max_registered_destructors + 0],
                "the fini array ran in reverse: last entry first");
    check_equal(1000002,
                g_sequence[max_registered_destructors + 1],
                "then the middle one");
    check_equal(1000001,
                g_sequence[max_registered_destructors + 2],
                "then the first entry last");

    // The two paths are ordered against each other, not interleaved.
    // This is the claim CLAUDE.md records as having been run once under
    // Bochs with a probe that was then deleted.
    std::size_t last_destructor{};
    std::size_t first_fini{g_sequence.size()};
    for (std::size_t i{}; i < g_sequence.size(); ++i) {
        if (g_sequence[i] >= 1000000) {
            if (i < first_fini) {
                first_fini = i;
            }
        } else {
            last_destructor = i;
        }
    }
    check(last_destructor < first_fini,
          "every __cxa_atexit destructor ran before the first fini array "
          "entry");

    check_equal(0,
                g_registered_destructor_count,
                "and the registry is empty afterwards");

    auto ran = g_sequence.size();
    zpp::crt::init::cleanup();
    check_equal(ran,
                g_sequence.size(),
                "a second call to cleanup() runs nothing again");
}

} // namespace

int main()
{
    // The heap, the primitives, scope_exit and zpp::error all stand on
    // their own. They run before crt::init::main() deliberately: none of
    // them may need the global heap.
    heap_round_trips();
    heap_alignment();
    heap_rejects_sizes_that_would_wrap();
    heap_coalesces();
    heap_exhaustion_returns_null();
    heap_soak();

    memory_copy();
    memory_move();
    memory_set();
    memory_compare();
    string_length();

    scope_exit_runs_once();
    scope_exit_release();
    scope_exit_move_transfers_ownership();
    scope_exit_runs_on_every_exit_path();

    errors_carry_their_category();

    the_destructor_registry_is_fixed_and_ordered();

    // Everything past here needs the global heap, and the order matters:
    // main() is one-shot, cleanup() is one-shot, and cleanup() must be
    // last because it empties the registry.
    the_init_arrays_run_forward();
    aligned_allocation();
    containers_route_through_the_global_heap();
    the_guard_functions_are_a_complete_abi();
    cxa_finalize_defers_to_cleanup();
    a_full_destructor_registry_traps();
    cleanup_runs_destructors_then_the_fini_array_in_reverse();

    std::println("crt: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
