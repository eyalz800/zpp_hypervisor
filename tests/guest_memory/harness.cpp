/**
 * The three functions that reach into a guest's memory:
 * `read_guest_physical`, `write_guest_physical` and
 * `guest_linear_to_physical`, all out of the real
 * hypervisor/src/hypervisor/guest_memory.cpp compiled against the real
 * hypervisor class.
 *
 * Why these are worth a harness of their own. Everything nested reads a
 * guest hypervisor's memory through the first two - vmcs12, the VMXON
 * region, the guest's own extended page tables - and the third is how a
 * linear address in a guest instruction becomes something this VMM can
 * touch at all. A wrong answer here is not a crash: it is a *plausible*
 * address, read successfully, belonging to the wrong page. That is the
 * failure mode that sends an investigation to whatever consumed the
 * value.
 *
 * What is stood in for, and it is one function: `map_window_at`, which
 * on the target edits the host page table and returns a mapping of a
 * guest-physical page. It cannot run here - it writes real paging
 * structures and invalidates real TLB entries - so the harness defines
 * it over an array of host memory. Everything else is the real code:
 * the bounds arithmetic, the page-at-a-time loop, the lock discipline
 * and the whole four-level walk.
 *
 * The expectations were written from SDM Vol. 3A Chapter 5 (4-level
 * paging) and from KVM before reading guest_memory.cpp, which is what
 * makes them worth asserting. Each carries its citation.
 */
#include "zpp/hypervisor/hypervisor.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace
{
/**
 * The fake guest physical memory.
 *
 * Small, and addressed by *guest* physical address rather than by host
 * pointer: `map_window_at` below is what turns one into the other, which
 * is exactly the indirection the real one provides. So a guest physical
 * address here is an offset into this array and nothing about the code
 * under test has to be relaxed to allow it.
 *
 * Sixteen pages, which is enough for a four-level walk (four tables), a
 * target page, and room either side of a copy that straddles a page
 * boundary.
 */
constexpr std::size_t page_size = 0x1000;
constexpr std::size_t memory_pages = 16;

alignas(page_size) std::uint8_t g_memory[memory_pages * page_size]{};

/**
 * How many times the window was pointed somewhere, and where last.
 *
 * The page-at-a-time loop is the thing being measured by the first of
 * these: a copy of one byte either side of a page boundary must map
 * twice, and a copy inside one page must map once. Nothing else can see
 * that from outside.
 */
std::uint64_t g_window_maps{};
std::uint64_t g_window_last_physical{};

/**
 * A guest physical address the window refuses, standing for a page with
 * no extended-page-table entry. Zero means "refuse nothing", and zero is
 * safe as a sentinel because page zero is never used by these cases.
 */
std::uint64_t g_window_refuses_page{};

void reset_memory()
{
    std::memset(g_memory, 0, sizeof(g_memory));
    g_window_maps = 0;
    g_window_last_physical = 0;
    g_window_refuses_page = 0;
}

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
    std::println("FAIL: {}\n  expected 0x{:x}\n  actual   0x{:x}",
                 what,
                 expected,
                 actual);
}

} // namespace

namespace zpp::hypervisor
{
hypervisor & hypervisor::instance()
{
    static hypervisor the;
    return the;
}

/**
 * The one stand-in, and the reason it is one.
 *
 * The real `map_window_at` writes a page-table entry in the host's own
 * paging structures, issues `invlpg`, and hands back the virtual address
 * the entry now names. None of those three can happen on the machine
 * running this harness. What it *means* - "give me a pointer through
 * which this guest physical page can be read and written, or nullptr if
 * it cannot be reached" - is what the array above provides.
 *
 * Two details of the real one's contract are reproduced rather than
 * simplified, because the caller depends on both:
 *
 *  - the address it is given is not page aligned, and the pointer it
 *    returns is the mapping *plus the offset within the page*. A
 *    stand-in that returned the page base reads entry 0 of every paging
 *    structure and copies to the wrong place inside a page, and both
 *    look like plausible answers.
 *  - a run of `pages` pages starting part way into one spans
 *    `pages + 1`, which is what decides whether it fits.
 *
 * `first_page` is ignored: the real one uses it so two runs can be
 * mapped at once without overwriting each other, and no case here maps
 * two at once.
 */
void * hypervisor::map_window_at(std::size_t,
                                 std::uint64_t physical_address,
                                 std::size_t pages)
{
    ++g_window_maps;
    g_window_last_physical = physical_address;

    auto offset = physical_address % page_size;
    auto page = physical_address / page_size;
    auto span = pages + ((0 != offset) ? 1 : 0);

    if ((0 != g_window_refuses_page) && (page == g_window_refuses_page)) {
        return nullptr;
    }

    if ((page + span) > memory_pages) {
        return nullptr;
    }

    return &g_memory[physical_address];
}
/**
 * The guest hypervisor's extended-page-table step, which
 * `guest_linear_to_physical` now takes once per level so its answers
 * match its sibling's in a nested guest.
 *
 * **Identity here, not a refusal.** This harness has no second-level
 * guest and no extended tables, so a guest-physical address is a host
 * one - which is exactly what the walk assumed before the step existed.
 * Refusing would make every walk in this suite fail.
 */
std::expected<std::uint64_t, zpp::error>
hypervisor::l2_physical_to_l1(std::size_t, std::uint64_t physical)
{
    return physical;
}

} // namespace zpp::hypervisor

namespace
{
using hypervisor = zpp::hypervisor::hypervisor;

/**
 * A machine with a guest whose paging is off, which is the state an
 * application processor is in when it comes out of a start-up IPI.
 */
std::unique_ptr<hypervisor> make()
{
    reset_memory();
    std::memset(&zpp::arch::x86_64::vmx::g_vmcs,
                0,
                sizeof(zpp::arch::x86_64::vmx::g_vmcs));
    zpp::arch::x86_64::vmx::g_vmcs_valid = true;

    auto state = std::make_unique<hypervisor>();
    state->vmcs.vpid(1);
    return state;
}

/**
 * The top of what the extended page tables describe, which is what
 * bounds a guest physical access.
 *
 * 512 GB, because `initialize_ept` builds one PML4 entry over 512
 * one-gigabyte page directories. Spelled here rather than imported so
 * the expectation is stated independently of the constant the code uses
 * - if that constant changes, this says so rather than agreeing.
 */
constexpr std::uint64_t guest_physical_limit = 512ull * 1024 * 1024 * 1024;

/*
 * The paging-structure bits the walk reads. SDM Vol. 3A Table 5-15
 * onwards: bit 0 present, bit 7 page size, bits 51:12 the address.
 */
constexpr std::uint64_t entry_present = 1ull << 0;
constexpr std::uint64_t entry_writable = 1ull << 1;
constexpr std::uint64_t entry_large = 1ull << 7;

/*
 * The control-register bits the walk consults.
 */
constexpr std::uint64_t cr0_paging = 1ull << 31;
constexpr std::uint64_t cr4_la57 = 1ull << 12;

std::uint64_t * table_at(std::uint64_t physical)
{
    return reinterpret_cast<std::uint64_t *>(&g_memory[physical]);
}

// === read_guest_physical and write_guest_physical =====================

/**
 * A copy that fits inside one page maps the window exactly once.
 *
 * The loop is per page, not per byte, and nothing outside can see the
 * difference except by counting - which is why the count is asserted
 * rather than only the bytes.
 */
void a_copy_inside_one_page_maps_once()
{
    auto state = make();
    constexpr std::uint64_t at = 3 * page_size + 0x40;

    std::uint8_t pattern[8]{1, 2, 3, 4, 5, 6, 7, 8};
    auto wrote = state->write_guest_physical(
        at,
        std::span(reinterpret_cast<const std::byte *>(pattern),
                  sizeof(pattern)));

    check(wrote.has_value(), "a write inside the identity map succeeds");
    check_equal(1, g_window_maps, "and maps the window exactly once");
    check_equal(
        at, g_window_last_physical, "at the address it was asked for");

    std::uint8_t back[8]{};
    g_window_maps = 0;
    auto read = state->read_guest_physical(
        at, std::span(reinterpret_cast<std::byte *>(back), sizeof(back)));

    check(read.has_value(), "and reading it back succeeds");
    check_equal(1, g_window_maps, "in one mapping as well");
    check(0 == std::memcmp(pattern, back, sizeof(pattern)),
          "and the bytes are the ones that went in");
}

/**
 * A copy that straddles a page boundary maps twice and lands in both
 * pages.
 *
 * This is the case a window-sized copy limit would have got wrong, and
 * the only evidence from outside is where the bytes ended up: the tail
 * of one page and the head of the next, with nothing in between
 * disturbed.
 */
void a_copy_across_a_page_boundary_maps_twice()
{
    auto state = make();
    constexpr std::uint64_t at = 4 * page_size - 4;

    std::uint8_t pattern[8]{
        0xa0, 0xa1, 0xa2, 0xa3, 0xb0, 0xb1, 0xb2, 0xb3};
    auto wrote = state->write_guest_physical(
        at,
        std::span(reinterpret_cast<const std::byte *>(pattern),
                  sizeof(pattern)));

    check(wrote.has_value(), "a straddling write succeeds");
    check_equal(2, g_window_maps, "and maps the window twice");
    check_equal(4 * page_size,
                g_window_last_physical,
                "the second time at the page that follows");

    check_equal(0xa0, g_memory[at], "the first byte is in the tail");
    check_equal(0xa3, g_memory[at + 3], "and so is the fourth");
    check_equal(0xb0,
                g_memory[4 * page_size],
                "the fifth is the first byte of the next page");
    check_equal(0xb3, g_memory[4 * page_size + 3], "and so is the last");
}

/**
 * A copy that would leave the identity map is refused whole, not
 * clamped.
 *
 * Refused rather than shortened, and the distinction is the point: a
 * short read leaves the caller acting on a buffer whose tail is whatever
 * it held before, which reads as a guest bug rather than as a refusal.
 */
void a_copy_past_the_identity_map_is_refused()
{
    auto state = make();
    std::uint8_t buffer[8]{};
    auto into =
        std::span(reinterpret_cast<std::byte *>(buffer), sizeof(buffer));

    auto beyond = state->read_guest_physical(guest_physical_limit, into);
    check(!beyond.has_value(),
          "an address at the top of the identity map is refused");
    check_equal(0, g_window_maps, "without mapping anything");

    auto straddling =
        state->read_guest_physical(guest_physical_limit - 4, into);
    check(!straddling.has_value(),
          "and so is a copy that starts inside it and ends past it");
    check_equal(0, g_window_maps, "also without mapping anything");

    // The arithmetic that decides this must not overflow: an address
    // just inside the limit plus a size near 2^64 wraps to a small
    // number in the naive spelling, and the check passes.
    auto huge = state->read_guest_physical(
        guest_physical_limit - page_size,
        std::span(reinterpret_cast<std::byte *>(buffer),
                  static_cast<std::size_t>(~0ull - page_size)));
    check(!huge.has_value(),
          "a size chosen to overflow the bound arithmetic is refused "
          "too");
}

/**
 * A page the window cannot reach fails the copy rather than being
 * skipped.
 */
void a_page_the_window_refuses_fails_the_copy()
{
    auto state = make();
    g_window_refuses_page = 5;

    std::uint8_t buffer[8]{};
    auto read = state->read_guest_physical(
        5 * page_size,
        std::span(reinterpret_cast<std::byte *>(buffer), sizeof(buffer)));

    check(!read.has_value(), "an unreachable page refuses the read");

    auto wrote = state->write_guest_physical(
        5 * page_size,
        std::span(reinterpret_cast<const std::byte *>(buffer),
                  sizeof(buffer)));
    check(!wrote.has_value(), "and refuses the write");
}

/**
 * A copy of nothing succeeds and maps nothing.
 *
 * Worth pinning because the loop is `while (done < size)`, and a
 * different spelling - a do-while, or a page count rounded up - maps a
 * page for a copy of zero bytes. Mapping a window has a cost and takes a
 * lock, so doing it for no bytes is not free.
 */
void an_empty_copy_maps_nothing()
{
    auto state = make();
    auto read =
        state->read_guest_physical(page_size, std::span<std::byte>{});

    check(read.has_value(), "a zero-length read succeeds");
    check_equal(0, g_window_maps, "and maps nothing");
}

// === guest_linear_to_physical =========================================

/**
 * Build a four-level table over the fake memory and return the CR3
 * value naming it.
 *
 * One entry per level, all at index zero except the ones the case
 * chooses, which keeps the tables small enough to sit in the array
 * above. The leaf is set by the caller through the returned pointers.
 */
struct walk_tables
{
    std::uint64_t cr3{};
    std::uint64_t * pml4{};
    std::uint64_t * pdpt{};
    std::uint64_t * pd{};
    std::uint64_t * pt{};
};

walk_tables build_tables()
{
    // Pages 1..4 are the four levels, so page 0 stays the "never used"
    // sentinel the window refusal relies on and pages 5+ are free to be
    // targets.
    walk_tables built{};
    built.cr3 = 1 * page_size;
    built.pml4 = table_at(1 * page_size);
    built.pdpt = table_at(2 * page_size);
    built.pd = table_at(3 * page_size);
    built.pt = table_at(4 * page_size);

    built.pml4[0] = (2 * page_size) | entry_present | entry_writable;
    built.pdpt[0] = (3 * page_size) | entry_present | entry_writable;
    built.pd[0] = (4 * page_size) | entry_present | entry_writable;
    return built;
}

/**
 * Paging off means the linear address is already the physical one.
 *
 * SDM Vol. 3A 5.1.1: "If CR0.PG = 0, paging is not used. Linear
 * addresses are treated as physical addresses." Reached in practice by
 * an application processor that has just taken a start-up IPI, which is
 * what the unrestricted-guest control exists for.
 */
void paging_off_is_the_identity()
{
    auto state = make();
    state->vmcs.guest_cr0(0);

    auto answer = state->guest_linear_to_physical(0x1234'5678);
    check(answer.has_value(), "a walk with paging off succeeds");
    check_equal(0x1234'5678, *answer, "and answers with its argument");
}

/**
 * Five-level paging is refused rather than walked as though it were
 * four.
 *
 * SDM Vol. 3A 5.5: with CR4.LA57 = 1 the top structure is a PML5, and a
 * four-level walk of one reads the PML5 entry as a PML4 entry and lands
 * on an unrelated page. Refusing is the only answer that cannot be
 * mistaken for a result.
 */
void five_level_paging_is_refused()
{
    auto state = make();
    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr4(cr4_la57);

    auto answer = state->guest_linear_to_physical(0x1000);
    check(!answer.has_value(), "CR4.LA57 refuses the walk");
    check_equal(0, g_window_maps, "without reading any table");
}

/**
 * The ordinary four-level walk of a 4 KB page.
 *
 * Four reads, one per level, and the answer is the leaf's frame with the
 * low twelve bits of the linear address on the end.
 */
void a_four_kilobyte_page_is_walked_to_its_frame()
{
    auto state = make();
    auto tables = build_tables();
    constexpr std::uint64_t frame = 9 * page_size;
    constexpr std::uint64_t offset = 0x123;

    tables.pt[0] = frame | entry_present | entry_writable;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    g_window_maps = 0;
    auto answer = state->guest_linear_to_physical(offset);

    check(answer.has_value(), "a mapped 4 KB page is found");
    check_equal(frame | offset,
                *answer,
                "and the answer is the frame plus the page offset");
    check_equal(
        4, g_window_maps, "reached in four reads, one per paging level");
}

/**
 * A two-megabyte page stops at the page directory, and the offset is the
 * whole 21 bits.
 *
 * SDM Vol. 3A Table 5-18: a PDE with PS = 1 maps a 2 MB page, its
 * address is bits 51:21, and bits 20:12 are reserved and must be 0. The
 * offset therefore comes from the linear address rather than from the
 * entry - which is the same thing the code must do to be right about a
 * table whose reserved bits are *not* 0, below.
 */
void a_two_megabyte_page_stops_at_the_directory()
{
    auto state = make();
    auto tables = build_tables();
    constexpr std::uint64_t large_page = 0x40'0000;
    constexpr std::uint64_t offset = 0x1f'2345;

    tables.pd[0] = large_page | entry_present | entry_large;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    g_window_maps = 0;
    auto answer = state->guest_linear_to_physical(offset);

    check(answer.has_value(), "a 2 MB mapping is found");
    check_equal(large_page | offset,
                *answer,
                "the answer carries the whole 21-bit offset");
    check_equal(3,
                g_window_maps,
                "and the walk stops after three levels rather than "
                "reading a page table that is not there");
}

/**
 * A one-gigabyte page stops at the page directory pointer table.
 *
 * SDM Vol. 3A Table 5-15: a PDPTE with PS = 1 maps a 1 GB page, address
 * bits 51:30, and bits 29:12 reserved.
 */
void a_one_gigabyte_page_stops_at_the_pointer_table()
{
    auto state = make();
    auto tables = build_tables();
    constexpr std::uint64_t huge_page = 0x4000'0000;
    constexpr std::uint64_t offset = 0x1234'5678;

    tables.pdpt[0] = huge_page | entry_present | entry_large;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    g_window_maps = 0;
    auto answer = state->guest_linear_to_physical(offset);

    check(answer.has_value(), "a 1 GB mapping is found");
    check_equal(huge_page | offset,
                *answer,
                "the answer carries the whole 30-bit offset");
    check_equal(2, g_window_maps, "after two levels");
}

/**
 * The reserved bits of a large entry's address are ignored, not
 * believed.
 *
 * SDM Vol. 3A Table 5-18 makes bits 20:12 of a 2 MB PDE reserved, and a
 * reserved bit is exactly the one a stale or hostile table has set. A
 * walk that ORs the entry's low address bits into the answer lands on a
 * page the guest did not name; masking them off is what keeps the
 * answer inside the 2 MB page the entry actually maps.
 *
 * Note what this is *not*: on real hardware a reserved bit set makes the
 * entry a reserved-bit page fault. This VMM is not delivering a fault
 * here, it is answering a question, and the safe answer is the one that
 * stays inside the mapping.
 */
void a_large_entry_with_reserved_bits_set_is_masked()
{
    auto state = make();
    auto tables = build_tables();
    constexpr std::uint64_t large_page = 0x40'0000;
    constexpr std::uint64_t reserved_junk = 0x1f'f000;
    constexpr std::uint64_t offset = 0x11;

    tables.pd[0] =
        large_page | reserved_junk | entry_present | entry_large;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    auto answer = state->guest_linear_to_physical(offset);

    check(answer.has_value(), "the walk still answers");
    check_equal(large_page | offset,
                *answer,
                "and the reserved bits of the entry's address are "
                "masked off rather than added to the result");
}

/**
 * A not-present entry at any level refuses the walk, and refuses it at
 * that level rather than reading on.
 */
void a_not_present_entry_refuses_at_its_own_level()
{
    struct
    {
        int level;
        std::uint64_t expected_reads;
        const char * what;
    } constexpr cases[]{
        {0, 1, "the PML4 entry"},
        {1, 2, "the page directory pointer entry"},
        {2, 3, "the page directory entry"},
        {3, 4, "the page table entry"},
    };

    for (const auto & one : cases) {
        auto state = make();
        auto tables = build_tables();
        tables.pt[0] = (9 * page_size) | entry_present;

        std::uint64_t * levels[]{
            tables.pml4, tables.pdpt, tables.pd, tables.pt};
        levels[one.level][0] &= ~entry_present;

        state->vmcs.guest_cr0(cr0_paging);
        state->vmcs.guest_cr3(tables.cr3);

        g_window_maps = 0;
        auto answer = state->guest_linear_to_physical(0);

        check(!answer.has_value(),
              std::string{"a walk stops where "} + one.what +
                  " is not present");
        check_equal(one.expected_reads,
                    g_window_maps,
                    std::string{"and it stops there: "} + one.what +
                        " is the last entry read");
    }
}

/**
 * The index each level is walked by comes from its own nine bits of the
 * linear address.
 *
 * SDM Vol. 3A Figure 5-8: bits 47:39 index the PML4, 38:30 the PDPT,
 * 29:21 the PD and 20:12 the PT. Checked one level at a time, with a
 * distinct index at each, so a walk that used the wrong slice lands on
 * a not-present entry rather than on a plausible one.
 */
void each_level_is_indexed_by_its_own_nine_bits()
{
    auto state = make();
    auto tables = build_tables();

    constexpr std::uint64_t pml4_index = 1;
    constexpr std::uint64_t pdpt_index = 2;
    constexpr std::uint64_t pd_index = 3;
    constexpr std::uint64_t pt_index = 4;
    constexpr std::uint64_t frame = 9 * page_size;
    constexpr std::uint64_t offset = 0x55;

    auto linear = (pml4_index << 39) | (pdpt_index << 30) |
                  (pd_index << 21) | (pt_index << 12) | offset;

    tables.pml4[0] = 0;
    tables.pdpt[0] = 0;
    tables.pd[0] = 0;
    tables.pml4[pml4_index] =
        (2 * page_size) | entry_present | entry_writable;
    tables.pdpt[pdpt_index] =
        (3 * page_size) | entry_present | entry_writable;
    tables.pd[pd_index] = (4 * page_size) | entry_present | entry_writable;
    tables.pt[pt_index] = frame | entry_present | entry_writable;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    auto answer = state->guest_linear_to_physical(linear);

    check(answer.has_value(),
          "a walk with a distinct index at every level succeeds");
    check_equal(frame | offset,
                *answer,
                "and lands on the frame those four indices name");
}

/**
 * CR3's low twelve bits are not part of the table address.
 *
 * SDM Vol. 3A Table 5-13: with 4-level paging and CR4.PCIDE = 0, bits
 * 11:5 of CR3 are ignored, bits 4:3 are PWT and PCD. A walk that used
 * CR3 whole would read the first table at an unaligned address, which
 * on this fixture reads a neighbouring entry and answers plausibly.
 */
void the_low_bits_of_cr3_are_not_part_of_the_address()
{
    auto state = make();
    auto tables = build_tables();
    constexpr std::uint64_t frame = 9 * page_size;

    tables.pt[0] = frame | entry_present;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3 | 0xfff);

    auto answer = state->guest_linear_to_physical(0);

    check(answer.has_value(), "a walk from a CR3 with flags set works");
    check_equal(
        frame, *answer, "and reads the table at the aligned address");
}

/**
 * The walk goes through read_guest_physical, so a level the window
 * cannot reach refuses the walk rather than reading rubbish.
 */
void a_table_the_window_cannot_reach_refuses_the_walk()
{
    auto state = make();
    auto tables = build_tables();
    tables.pt[0] = (9 * page_size) | entry_present;

    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(tables.cr3);

    // The page directory, which is the third read.
    g_window_refuses_page = 3;

    auto answer = state->guest_linear_to_physical(0);
    check(!answer.has_value(),
          "a table page the window refuses refuses the walk");
}

/**
 * A table whose address is outside the identity map refuses the walk.
 *
 * The bound is read_guest_physical's, and this is what connects the two
 * halves of this harness: a guest that writes a nonsense CR3 does not
 * get a nonsense answer, it gets a refusal, because every level goes
 * through the same bounds test as any other guest read.
 */
void a_table_past_the_identity_map_refuses_the_walk()
{
    auto state = make();
    state->vmcs.guest_cr0(cr0_paging);
    state->vmcs.guest_cr3(guest_physical_limit);

    auto answer = state->guest_linear_to_physical(0);
    check(!answer.has_value(),
          "a CR3 past the top of the identity map refuses the walk");
    check_equal(0, g_window_maps, "without mapping anything");
}

} // namespace

int main()
{
    a_copy_inside_one_page_maps_once();
    a_copy_across_a_page_boundary_maps_twice();
    a_copy_past_the_identity_map_is_refused();
    a_page_the_window_refuses_fails_the_copy();
    an_empty_copy_maps_nothing();

    paging_off_is_the_identity();
    five_level_paging_is_refused();
    a_four_kilobyte_page_is_walked_to_its_frame();
    a_two_megabyte_page_stops_at_the_directory();
    a_one_gigabyte_page_stops_at_the_pointer_table();
    a_large_entry_with_reserved_bits_set_is_masked();
    a_not_present_entry_refuses_at_its_own_level();
    each_level_is_indexed_by_its_own_nine_bits();
    the_low_bits_of_cr3_are_not_part_of_the_address();
    a_table_the_window_cannot_reach_refuses_the_walk();
    a_table_past_the_identity_map_refuses_the_walk();

    std::println(
        "guest_memory: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
