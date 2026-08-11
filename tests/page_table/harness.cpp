/*
 * Regression harness for the host page table, the OS page table walker
 * and the paging-structure helpers they are both built out of:
 * zpp/arch/x86_64/virtual_address.h, pte.h, page_table.h,
 * os_page_table.h and memory_type.h.
 *
 * None of it had any coverage. That matters more here than the line
 * count suggests, because everything the VMM does after it loads its own
 * CR3 is reached through these five headers: the module it executes
 * from, the local APIC page an exit handler reads, the queue memory the
 * diagnostic channel writes, and the physical addresses handed to the
 * VMCS. A wrong answer from virtual_to_physical is not a wrong number -
 * it is a #PF taken inside an exit handler, where there is no recovery
 * point, so the processor halts and the guest's other processors spin
 * behind it. hypervisor.cpp records that exact failure twice, once at
 * the APIC page and once at the controller BAR.
 *
 * Hosted, native, no emulator and no target. page_table.h depends on
 * pte.h, virtual_address.h, <cstdint>, <type_traits>, <utility> and
 * <iterator>, os_page_table.h on <cstdint> alone, and neither .cpp
 * reaches any assembly wrapper - so the code under test compiles here
 * exactly as the hypervisor compiles it and needs no shim, the same
 * reason tests/elf_relocate and tests/mtrr need none. build.sh compiles
 * the two real translation units out of hypervisor/src/arch/x86_64/
 * rather than a copy of them.
 *
 * virtual_address, pte and memory_type are entirely constexpr, so their
 * claims are made twice: once as a static_assert, where the compile *is*
 * the test, and once at runtime, where a failure prints which bit
 * disagreed. page_table and os_page_table are compiled code with member
 * state, so they are runtime only.
 *
 * The strongest thing here is not any single assertion, it is the
 * differential in hardware_walk(): the built table is walked the way the
 * processor walks it - entry by entry, following the physical address
 * each one holds - and that answer is compared against what
 * page_table::virtual_to_physical claims. They disagree in exactly the
 * places recorded below and nowhere else.
 *
 * ---------------------------------------------------------------------
 * Three defects this harness pins. None is fixed here: every check
 * asserts what the code does *today*, with the SDM's answer written
 * beside it, so that a fix turns a named check red and says so in its
 * own message.
 *
 *   1. A large-page translation comes out shifted, in both walkers.
 *      page_table::virtual_to_physical and
 *      os_page_table::virtual_to_physical compute a 2 MB page's base as
 *      `page_number() << 21`, but page_number() is already bits 51:12 of
 *      the entry (SDM Table 5-20, sdm.txt:157306) while a PS=1 PDE names
 *      its page in bits 51:21 (Table 5-18, sdm.txt:157243). The answer
 *      is the true base shifted left by nine. The 1 GB path is the same
 *      defect shifted left by eighteen. vmx/ept.h has the accessor this
 *      wants - large_page_number(), bits 51:21, used correctly by
 *      epte_for - and pte.h has no equivalent.
 *
 *      Live on the Windows and Linux loaders, where physical_to_virtual
 *      is non-null and the OS page table being walked really does use
 *      2 MB leaves. Not live under UEFI, where the callback is null and
 *      the walk is skipped entirely, and not live for page_table, which
 *      has no way to *create* a large entry - map_page_from writes a
 *      4 KB leaf every time.
 *
 *   2. An unmapped address is not refused, and cannot be. Neither
 *      virtual_to_physical checks the present bit at any level, and the
 *      return type is a bare std::uint64_t with no failure value. For
 *      page_table the answer on an unmapped page is the offset within
 *      the page, which is zero only when the address is page aligned -
 *      so the two callers that test the result as a boolean
 *      (read_guest_word and apply_guest_store in hypervisor.cpp) pass
 *      their guard for any unmapped address whose low twelve bits are
 *      not zero, and then dereference it. That is the fault those two
 *      guards exist to prevent. It runs the other way too: a page
 *      legitimately mapped to physical zero is refused.
 *
 *   3. pte::protection_key() is one bit low. It reads and writes bits
 *      61:58; SDM Table 5-20 puts the protection key at bits 62:59
 *      (sdm.txt:157313). Latent - nothing in the tree calls it, and
 *      nothing sets CR4.PKE.
 *
 *   And one thing that looks like a defect and is not: pte::pat() and
 *   pte::large() are the same bit, 7. SDM 14.12.3 (sdm.txt:174684) says
 *   "The PAT bit is bit 7 in page-table entries that point to 4-KByte
 *   pages and bit 12 in paging-structure entries that point to larger
 *   pages", and Table 5-20 against Table 5-18 agrees: bit 7 is PAT in a
 *   PTE and PS in a PDE. No entry can need both meanings, so the alias
 *   is right everywhere except the PAT bit of a large page, which this
 *   tree never sets and has no accessor for.
 * ---------------------------------------------------------------------
 *
 * SDM line numbers are into .references/sdm.txt as fetched by
 * scripts/fetch-references.sh, and every one was looked up while writing
 * this rather than recalled.
 */
#include "zpp/arch/x86_64/memory_type.h"
#include "zpp/arch/x86_64/os_page_table.h"
#include "zpp/arch/x86_64/page_table.h"
#include "zpp/arch/x86_64/pte.h"
#include "zpp/arch/x86_64/virtual_address.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace
{
using zpp::arch::x86_64::memory_type;
using zpp::arch::x86_64::os_page_table;
using zpp::arch::x86_64::page_table;
using zpp::arch::x86_64::pte;
using zpp::arch::x86_64::virtual_address;

using protection = page_table::protection;

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

std::string hex(std::uint64_t value)
{
    return std::format("0x{:x}", value);
}

constexpr std::uint64_t page_size = 0x1000;
constexpr std::uint64_t large_page_size = 0x200000;
constexpr std::uint64_t huge_page_size = 0x40000000;

/**
 * The 512 GB a single PML4 entry controls. SDM 5.5.4: "Because a PML4E
 * is identified using bits 47:39 of the linear address, it controls
 * access to a 512-GByte region of the linear-address space"
 * (sdm.txt:157012).
 */
constexpr std::uint64_t pml4e_span = 0x8000000000ull;

constexpr std::uint64_t mask_of(unsigned low, unsigned high)
{
    return (~std::uint64_t{} >> (63 - (high - low))) << low;
}

/*
 * =====================================================================
 * 1. virtual_address
 *
 * SDM Vol. 3A 5.5.4, "Linear-Address Translation with 4-Level Paging and
 * 5-Level Paging" (sdm.txt:156894), gives the four index fields as the
 * bits of the linear address that select an entry in each structure:
 *
 *   pml4e   bits 47:39   sdm.txt:157010
 *   pdpte   bits 38:30   sdm.txt:157021
 *   pde     bits 29:21   sdm.txt:157042
 *   pte     bits 20:12   sdm.txt:157058
 *
 * and the offsets as the bits taken from the linear address unchanged
 * once the walk terminates:
 *
 *   4 KB page   bits 11:0    sdm.txt:157065
 *   2 MB page   bits 20:0    sdm.txt:157052
 *   1 GB page   bits 29:0    sdm.txt:157031
 *
 * Four indices and not five: there is no PML5 accessor here, so 5-level
 * paging is not representable and is not tested. That is a property of
 * the type rather than a gap in this harness - a PML5E is selected by
 * bits 56:48 of the linear address (sdm.txt:156998), which every
 * accessor below treats as bits that land nowhere.
 * =====================================================================
 */

/**
 * One accessor pair, described by the *range* the SDM gives it rather
 * than by the shift the header uses. That is the point of the table: the
 * expectation below is derived from "bits 47:39" and not from the
 * expression under test, so a field shifted by one bit fails instead of
 * agreeing with itself.
 */
struct address_field
{
    const char * name;
    unsigned low;
    unsigned high;
    std::uint64_t (*read)(const virtual_address &);
    void (*write)(virtual_address &, std::uint64_t);
};

constexpr address_field address_fields[]{
    {"offset",
     0,
     11,
     [](const virtual_address & a) { return a.offset(); },
     [](virtual_address & a, std::uint64_t v) { a.offset(v); }},
    {"large_offset",
     0,
     20,
     [](const virtual_address & a) { return a.large_offset(); },
     [](virtual_address & a, std::uint64_t v) { a.large_offset(v); }},
    {"huge_offset",
     0,
     29,
     [](const virtual_address & a) { return a.huge_offset(); },
     [](virtual_address & a, std::uint64_t v) { a.huge_offset(v); }},
    {"pte",
     12,
     20,
     [](const virtual_address & a) { return a.pte(); },
     [](virtual_address & a, std::uint64_t v) { a.pte(v); }},
    {"pde",
     21,
     29,
     [](const virtual_address & a) { return a.pde(); },
     [](virtual_address & a, std::uint64_t v) { a.pde(v); }},
    {"pdpte",
     30,
     38,
     [](const virtual_address & a) { return a.pdpte(); },
     [](virtual_address & a, std::uint64_t v) { a.pdpte(v); }},
    {"pml4e",
     39,
     47,
     [](const virtual_address & a) { return a.pml4e(); },
     [](virtual_address & a, std::uint64_t v) { a.pml4e(v); }},
};

/**
 * The low 48 bits, reassembled out of the four indices and the offset.
 * virtual_address exposes no raw value, so this is how a setter's effect
 * on everything *else* is observed. Bits 63:48 are unobservable through
 * the class and are covered instead by the sweep below, which shows that
 * nothing reads them.
 */
constexpr std::uint64_t low_48_of(const virtual_address & address)
{
    return (address.pml4e() << 39) | (address.pdpte() << 30) |
           (address.pde() << 21) | (address.pte() << 12) |
           address.offset();
}

/**
 * Walks a single set bit through all sixty-four positions and asks every
 * accessor what it sees. Systematic rather than sampled: a field that
 * moved by one bit is caught twice, at the bit it gained and at the bit
 * it lost, whichever way it moved.
 *
 * Bits 63:48 are expected to land nowhere, which is this harness's
 * answer to "does the type have an opinion about a non-canonical
 * address": it does not. It masks out the bits it wants and never
 * validates.
 */
constexpr bool address_bits_land_where_the_sdm_says()
{
    for (const auto & field : address_fields) {
        for (unsigned bit{}; bit < 64; ++bit) {
            virtual_address address{std::uint64_t{1} << bit};

            std::uint64_t expected{};
            if ((bit >= field.low) && (bit <= field.high)) {
                expected = std::uint64_t{1} << (bit - field.low);
            }

            if (field.read(address) != expected) {
                return false;
            }
        }
    }
    return true;
}

static_assert(address_bits_land_where_the_sdm_says());

/**
 * Every setter writes the whole of its own field and nothing outside it,
 * checked from a seed with bits in every field so that a setter which
 * cleared a neighbour is visible.
 */
constexpr bool address_setters_touch_only_their_field()
{
    constexpr std::uint64_t seed = 0x0000a5c3f0f0f0f0ull;

    for (const auto & field : address_fields) {
        auto mask = mask_of(field.low, field.high);

        virtual_address set{seed};
        field.write(set, ~std::uint64_t{});
        if (low_48_of(set) != ((seed | mask) & mask_of(0, 47))) {
            return false;
        }

        virtual_address cleared{seed};
        field.write(cleared, 0);
        if (low_48_of(cleared) != ((seed & ~mask) & mask_of(0, 47))) {
            return false;
        }
    }
    return true;
}

static_assert(address_setters_touch_only_their_field());

void virtual_address_decomposition()
{
    // The same sweep as the static_assert above, run again so a failure
    // names the field and the bit instead of stopping the build with one
    // message.
    for (const auto & field : address_fields) {
        for (unsigned bit{}; bit < 64; ++bit) {
            virtual_address address{std::uint64_t{1} << bit};
            std::uint64_t expected{};
            if ((bit >= field.low) && (bit <= field.high)) {
                expected = std::uint64_t{1} << (bit - field.low);
            }
            check_equal(expected,
                        field.read(address),
                        std::string("virtual_address::") + field.name +
                            " sees bit " + std::to_string(bit));
        }

        auto mask = mask_of(field.low, field.high);
        virtual_address cleared{~std::uint64_t{}};
        field.write(cleared, 0);
        check_equal(mask_of(0, 47) & ~mask,
                    low_48_of(cleared),
                    std::string("virtual_address::") + field.name +
                        " clears exactly its own bits");
    }

    // Every boundary between two levels, from both sides. The low side
    // is all ones in the fields below, the high side is the first
    // address that carries into the field above - which is where an
    // off-by-one in a mask shows up as a carry that does not happen.
    struct boundary
    {
        std::uint64_t address;
        std::uint64_t pml4e;
        std::uint64_t pdpte;
        std::uint64_t pde;
        std::uint64_t pte;
        std::uint64_t offset;
        const char * what;
    };

    constexpr boundary boundaries[]{
        {0, 0, 0, 0, 0, 0, "address zero"},
        {page_size - 1, 0, 0, 0, 0, 0xfff, "the last byte of page 0"},
        {page_size, 0, 0, 0, 1, 0, "the first byte of page 1"},
        {large_page_size - 1,
         0,
         0,
         0,
         511,
         0xfff,
         "the last byte below 2 MB"},
        {large_page_size, 0, 0, 1, 0, 0, "2 MB exactly"},
        {huge_page_size - 1,
         0,
         0,
         511,
         511,
         0xfff,
         "the last byte below 1 GB"},
        {huge_page_size, 0, 1, 0, 0, 0, "1 GB exactly"},
        {pml4e_span - 1,
         0,
         511,
         511,
         511,
         0xfff,
         "the last byte below 512 GB"},
        {pml4e_span, 1, 0, 0, 0, 0, "512 GB exactly"},

        // The top of the canonical low half and the bottom of the
        // canonical high half. Adjacent in the architecture, 16 TB apart
        // as integers, and the type has no opinion about the gap.
        {0x00007fffffffffffull,
         0xff,
         511,
         511,
         511,
         0xfff,
         "the top of the canonical low half - bit 47 is clear there, so "
         "the PML4 index is 0xff and not 0x1ff"},
        {0xffff800000000000ull,
         256,
         0,
         0,
         0,
         0,
         "the bottom of the canonical high half"},

        // Non-canonical, decomposed exactly as if it were not: the sign
        // extension bits are discarded rather than rejected. Worth
        // stating, because it is what makes the host table's answer for
        // such an address a plausible number rather than an error.
        {0x0001000000000000ull,
         0,
         0,
         0,
         0,
         0,
         "a non-canonical address decomposes as its low 48 bits"},
        {0xdead800000001234ull,
         256,
         0,
         0,
         1,
         0x234,
         "and so does a thoroughly non-canonical one"},
    };

    for (const auto & test : boundaries) {
        virtual_address address{test.address};
        check_equal(test.pml4e,
                    address.pml4e(),
                    std::string("pml4e of ") + test.what);
        check_equal(test.pdpte,
                    address.pdpte(),
                    std::string("pdpte of ") + test.what);
        check_equal(
            test.pde, address.pde(), std::string("pde of ") + test.what);
        check_equal(
            test.pte, address.pte(), std::string("pte of ") + test.what);
        check_equal(test.offset,
                    address.offset(),
                    std::string("offset of ") + test.what);
    }

    // The two larger offsets, which are what the large-page branches of
    // both walkers add to the page base.
    virtual_address inside{0x1234ab7ffull};
    check_equal(0x7ff, inside.offset(), "offset within a 4 KB page");
    check_equal(0xab7ff,
                inside.large_offset(),
                "offset within a 2 MB page is bits 20:0");
    check_equal(0x234ab7ff,
                inside.huge_offset(),
                "offset within a 1 GB page is bits 29:0");
}

/*
 * =====================================================================
 * 2. pte
 *
 * SDM Vol. 3A Table 5-20, "Format of a Page-Table Entry that Maps a
 * 4-KByte Page" (sdm.txt:157287), gives every bit this class exposes:
 *
 *   0  (P)    present                     sdm.txt:157291
 *   1  (R/W)  write                       sdm.txt:157292
 *   2  (U/S)  user                        sdm.txt:157293
 *   3  (PWT)  write_through               sdm.txt:157295
 *   4  (PCD)  page_level_cache_disable    sdm.txt:157297
 *   5  (A)    access                      sdm.txt:157299
 *   6  (D)    dirty                       sdm.txt:157300
 *   7  (PAT)  pat                         sdm.txt:157301
 *   8  (G)    global                      sdm.txt:157302
 *   51:12     page_number                 sdm.txt:157306, 157311
 *   62:59     protection key              sdm.txt:157313
 *   63 (XD)   execute_disable             sdm.txt:157315
 *
 * and Table 5-18, "Format of a Page-Directory Entry that Maps a 2-MByte
 * Page" (sdm.txt:157221), gives bit 7 its other meaning:
 *
 *   7  (PS)   large                       sdm.txt:157235
 * =====================================================================
 */

struct entry_field
{
    const char * name;
    unsigned low;
    unsigned high;
    std::uint64_t (*read)(const pte &);
    void (*write)(pte &, std::uint64_t);
};

constexpr entry_field entry_fields[]{
    {"present",
     0,
     0,
     [](const pte & e) -> std::uint64_t { return e.present(); },
     [](pte & e, std::uint64_t v) { e.present(v); }},
    {"write",
     1,
     1,
     [](const pte & e) -> std::uint64_t { return e.write(); },
     [](pte & e, std::uint64_t v) { e.write(v); }},
    {"user",
     2,
     2,
     [](const pte & e) -> std::uint64_t { return e.user(); },
     [](pte & e, std::uint64_t v) { e.user(v); }},
    {"write_through",
     3,
     3,
     [](const pte & e) -> std::uint64_t { return e.write_through(); },
     [](pte & e, std::uint64_t v) { e.write_through(v); }},
    {"page_level_cache_disable",
     4,
     4,
     [](const pte & e) -> std::uint64_t {
         return e.page_level_cache_disable();
     },
     [](pte & e, std::uint64_t v) { e.page_level_cache_disable(v); }},
    {"access",
     5,
     5,
     [](const pte & e) -> std::uint64_t { return e.access(); },
     [](pte & e, std::uint64_t v) { e.access(v); }},
    {"dirty",
     6,
     6,
     [](const pte & e) -> std::uint64_t { return e.dirty(); },
     [](pte & e, std::uint64_t v) { e.dirty(v); }},
    {"pat",
     7,
     7,
     [](const pte & e) -> std::uint64_t { return e.pat(); },
     [](pte & e, std::uint64_t v) { e.pat(v); }},
    {"large",
     7,
     7,
     [](const pte & e) -> std::uint64_t { return e.large(); },
     [](pte & e, std::uint64_t v) { e.large(v); }},
    {"global",
     8,
     8,
     [](const pte & e) -> std::uint64_t { return e.global(); },
     [](pte & e, std::uint64_t v) { e.global(v); }},
    {"page_number",
     12,
     51,
     [](const pte & e) { return e.page_number(); },
     [](pte & e, std::uint64_t v) { e.page_number(v); }},

    // 58 and not 59, deliberately. See defect 3 in the header comment:
    // the SDM puts the protection key at bits 62:59 (sdm.txt:157313) and
    // this class reads bits 61:58. The range here describes the code, so
    // that a fix fails this table and has to be written down here too.
    {"protection_key",
     58,
     61,
     [](const pte & e) { return e.protection_key(); },
     [](pte & e, std::uint64_t v) { e.protection_key(v); }},
    {"execute_disable",
     63,
     63,
     [](const pte & e) -> std::uint64_t { return e.execute_disable(); },
     [](pte & e, std::uint64_t v) { e.execute_disable(v); }},
};

constexpr bool entry_bits_land_where_the_sdm_says()
{
    for (const auto & field : entry_fields) {
        for (unsigned bit{}; bit < 64; ++bit) {
            pte entry{std::uint64_t{1} << bit};

            std::uint64_t expected{};
            if ((bit >= field.low) && (bit <= field.high)) {
                expected = std::uint64_t{1} << (bit - field.low);
            }

            if (field.read(entry) != expected) {
                return false;
            }
        }
    }
    return true;
}

static_assert(entry_bits_land_where_the_sdm_says());

/**
 * Every setter writes its whole field and nothing else, checked against
 * the raw value - which pte does expose - so this is a statement about
 * all sixty-four bits rather than about the accessors agreeing with each
 * other.
 */
constexpr bool entry_setters_touch_only_their_field()
{
    for (const auto & field : entry_fields) {
        auto mask = mask_of(field.low, field.high);

        pte from_zero{};
        field.write(from_zero, ~std::uint64_t{});
        if (from_zero.value() != mask) {
            return false;
        }

        pte from_ones{~std::uint64_t{}};
        field.write(from_ones, 0);
        if (from_ones.value() != ~mask) {
            return false;
        }
    }
    return true;
}

static_assert(entry_setters_touch_only_their_field());

/**
 * The page frame number over the full width the format allows: bits
 * 51:12, forty bits, so the top representable frame is 0xffffffffff and
 * the top bit of the field is entry bit 51. A wider value is truncated
 * rather than allowed to spill into the ignored bits above it
 * (Table 5-20, sdm.txt:157312).
 */
constexpr bool page_numbers_round_trip()
{
    constexpr std::uint64_t values[]{
        0,
        1,
        0xfffff,
        0x8000000000ull, // the top bit of the field, entry bit 51
        0xffffffffffull, // every bit of the field
    };

    for (auto value : values) {
        pte entry{};
        entry.page_number(value);
        if (entry.page_number() != value) {
            return false;
        }
        if (entry.value() != (value << 12)) {
            return false;
        }
    }

    pte entry{};
    entry.page_number(0x10000000000ull);
    return (entry.page_number() == 0) && (entry.value() == 0);
}

static_assert(page_numbers_round_trip());

void entry_accessors()
{
    for (const auto & field : entry_fields) {
        for (unsigned bit{}; bit < 64; ++bit) {
            pte entry{std::uint64_t{1} << bit};
            std::uint64_t expected{};
            if ((bit >= field.low) && (bit <= field.high)) {
                expected = std::uint64_t{1} << (bit - field.low);
            }
            check_equal(expected,
                        field.read(entry),
                        std::string("pte::") + field.name + " sees bit " +
                            std::to_string(bit));
        }

        auto mask = mask_of(field.low, field.high);

        pte from_zero{};
        field.write(from_zero, ~std::uint64_t{});
        check_equal(mask,
                    from_zero.value(),
                    std::string("pte::") + field.name +
                        " written into an empty entry sets exactly its "
                        "own bits");

        pte from_ones{~std::uint64_t{}};
        field.write(from_ones, 0);
        check_equal(~mask,
                    from_ones.value(),
                    std::string("pte::") + field.name +
                        " cleared in a full entry clears exactly its own "
                        "bits");
    }

    // Every flag set in turn, cumulatively, and read back. The sweep
    // above proves each accessor's position in isolation; this proves
    // they coexist, which is the shape map_page_from actually writes.
    pte entry{};
    entry.page_number(0xabcde);
    entry.present(true);
    entry.write(true);
    entry.user(true);
    entry.write_through(true);
    entry.page_level_cache_disable(true);
    entry.access(true);
    entry.dirty(true);
    entry.global(true);
    entry.execute_disable(true);

    check(entry.present(), "present survives every other flag");
    check(entry.write(), "write survives every other flag");
    check(entry.user(), "user survives every other flag");
    check(entry.write_through(),
          "write_through survives every other flag");
    check(entry.page_level_cache_disable(),
          "page_level_cache_disable survives every other flag");
    check(entry.access(), "access survives every other flag");
    check(entry.dirty(), "dirty survives every other flag");
    check(entry.global(), "global survives every other flag");
    check(entry.execute_disable(),
          "execute_disable survives every other flag");
    check_equal(0xabcde,
                entry.page_number(),
                "and the page number is undisturbed by all nine");
    check(!entry.large(),
          "while PS stays clear - none of the nine reaches bit 7");

    check_equal(0x80000000abcde17full,
                entry.value(),
                "the composed entry is exactly those bits and no others");
    check_equal(entry.value(),
                std::uint64_t(entry),
                "operator uint64_t agrees with value()");

    // pat() and large() are the same bit, and that is correct. SDM
    // 14.12.3 (sdm.txt:174684) says the PAT bit is bit 7 in an entry
    // mapping a 4 KB page and bit 12 in one mapping a larger page, while
    // Table 5-18 (sdm.txt:157235) makes bit 7 the PS flag there. So the
    // alias is only wrong for the PAT bit of a large page, which nothing
    // in this tree sets and no accessor here can reach.
    pte aliased{};
    aliased.pat(true);
    check(aliased.large(),
          "pat() and large() are the same bit 7 - correct for a 4 KB PTE "
          "and for a PS flag, and bit 12 has no accessor at all");
    check_equal(0x80, aliased.value(), "and that bit is bit 7");

    // Defect 3, asserted as it behaves.
    pte keyed{};
    keyed.protection_key(0xf);
    check_equal(0x3c00000000000000ull,
                keyed.value(),
                "protection_key writes bits 61:58 - the SDM says 62:59 "
                "(sdm.txt:157313), so this check is a defect record and "
                "must be inverted when it is fixed");
    check(!keyed.execute_disable(),
          "though it stops below bit 63, so it cannot forge "
          "execute_disable");

    check(page_numbers_round_trip(),
          "page numbers round trip over the whole of bits 51:12");
}

/*
 * =====================================================================
 * 3. page_table
 * =====================================================================
 */

/**
 * A source table that answers every question with the address it was
 * asked about. That is what the loader passes under UEFI -
 * os_page_table with a null physical_to_virtual returns its argument
 * unchanged - and it is what makes the hardware-style walk below
 * possible: with an identity map the physical address written into an
 * entry is also a host pointer this harness can follow.
 */
struct identity_source
{
    std::uint64_t virtual_to_physical(std::uint64_t value) const
    {
        return value;
    }

    std::uint64_t virtual_to_physical(const void * value) const
    {
        return reinterpret_cast<std::uint64_t>(value);
    }
};

/**
 * A source table that scatters, to exercise the claim map_from makes in
 * its own comment: "Translated per page, since the source table is free
 * to have scattered the range across physical memory." The mapping is
 * page granular, injective on the page number and deliberately not order
 * preserving, so a walker that translated the base once and added would
 * land on the wrong page for every page but the first.
 *
 * Pointers are still answered identically. Only data pages are
 * scattered: the table's own storage has to translate to its real host
 * address or the walk below cannot follow it, and the real
 * initialize_host_page_table has the same requirement - host_cr3 is the
 * physical address the source gives for the table's own head.
 */
struct scatter_source
{
    static std::uint64_t physical_of(std::uint64_t value)
    {
        auto page = value >> 12;
        return (((page * 0x1f) + 0x40000) << 12) | (value & 0xfff);
    }

    std::uint64_t virtual_to_physical(std::uint64_t value) const
    {
        return physical_of(value);
    }

    std::uint64_t virtual_to_physical(const void * value) const
    {
        return reinterpret_cast<std::uint64_t>(value);
    }
};

/**
 * The result of walking a built table the way the processor walks it.
 */
struct walk_result
{
    int level{}; // 4 = pml4e, 3 = pdpte, 2 = pde, 1 = pte
    bool present{};
    bool large{};
    pte entry{};
    std::uint64_t physical{};
};

/**
 * Walks the table entry by entry, following the physical address in each
 * one as a host pointer - legitimate only because every table here is
 * built through identity_source.
 *
 * This is the differential. Everything else in this section states what
 * an entry should contain; this states what a processor loaded with this
 * table would do, computed from the SDM's own rules (sdm.txt:157006
 * onwards), and the walker under test is compared against it.
 */
walk_result hardware_walk(const page_table & table, std::uint64_t address)
{
    virtual_address structure{address};

    // head() is pml4[0] and the rest of the array follows it, which is
    // exactly how initialize_host_page_table uses it to compose
    // host_cr3.
    const auto * pml4 = &table.head();

    const auto & pml4e = pml4[structure.pml4e()];
    if (!pml4e.present()) {
        return {4, false, false, pml4e, 0};
    }

    const auto * pdpt =
        reinterpret_cast<const pte *>(pml4e.page_number() << 12);
    const auto & pdpte = pdpt[structure.pdpte()];
    if (!pdpte.present()) {
        return {3, false, false, pdpte, 0};
    }
    if (pdpte.large()) {
        // SDM 5.5.4: "Bits 51:30 are from the PDPTE", "Bits 29:0 are
        // from the original linear address" (sdm.txt:157030).
        return {3,
                true,
                true,
                pdpte,
                (pdpte.value() & mask_of(30, 51)) +
                    structure.huge_offset()};
    }

    const auto * pd =
        reinterpret_cast<const pte *>(pdpte.page_number() << 12);
    const auto & pde = pd[structure.pde()];
    if (!pde.present()) {
        return {2, false, false, pde, 0};
    }
    if (pde.large()) {
        // SDM 5.5.4: "Bits 51:21 are from the PDE", "Bits 20:0 are from
        // the original linear address" (sdm.txt:157051).
        return {2,
                true,
                true,
                pde,
                (pde.value() & mask_of(21, 51)) +
                    structure.large_offset()};
    }

    const auto * pt =
        reinterpret_cast<const pte *>(pde.page_number() << 12);
    const auto & entry = pt[structure.pte()];
    if (!entry.present()) {
        return {1, false, false, entry, 0};
    }

    // SDM 5.5.4: "Bits 51:12 are from the PTE", "Bits 11:0 are from the
    // original linear address" (sdm.txt:157064).
    return {1,
            true,
            false,
            entry,
            (entry.page_number() << 12) + structure.offset()};
}

/**
 * Which of the two page directories an address folds onto. 512 PDPT
 * slots share two of them, so the divisor is 256 and the choice is
 * address bit 38 alone - detail/page_table.h says exactly that, and this
 * is the same expression written once for the harness.
 */
constexpr std::uint64_t directory_of(std::uint64_t address)
{
    return (address >> 38) & 1;
}

/**
 * The ranges the page_table tests map. Listed in one place because they
 * are also what fresh_table() checks its own storage against.
 */
struct test_range
{
    std::uint64_t base;
    std::uint64_t size;
};

constexpr test_range test_ranges[]{
    {0x1000, 2 * page_size},
    {0x1ff000, 2 * page_size},
    {0x3ffff000, 2 * page_size},
    {0x7fffffff000ull, 2 * page_size},
    {0x10000000, 9 * page_size},
    {0x20000000, 2 * page_size},
    {0x30000000, 2 * page_size},
    {0x31000000, 2 * page_size},
    {0x32000000, 2 * page_size},
    {0x33000000, 2 * page_size},
    {0x40000000, 4 * page_size},
    {0x50000000, page_size},
    {0x60000000, 2 * page_size},
    {0x70000000, page_size},
    {0x80000000, page_size},
    {pml4e_span + page_size, page_size},
};

/**
 * True if the table's own storage shares a page directory entry with any
 * address the tests use.
 *
 * It has to be asked, because map_self maps the object's own 1028
 * contiguous pages at their real host addresses and this structure folds
 * 512 GB onto two page directories - so where the host allocator happens
 * to put the object decides which test addresses would silently share a
 * leaf with it. About one allocation in twenty would, and the resulting
 * failure would look exactly like a translation bug. Compared at page
 * directory granularity rather than page granularity because two of the
 * tests overwrite a whole directory entry.
 */
bool storage_collides_with_a_test_address(const page_table & table)
{
    auto base = reinterpret_cast<std::uint64_t>(&table.head());
    auto end = base + sizeof(page_table);

    for (auto page = base & ~(large_page_size - 1); page < end;
         page += large_page_size) {
        for (const auto & range : test_ranges) {
            for (auto address = range.base;
                 address < range.base + range.size;
                 address += page_size) {
                if ((directory_of(page) == directory_of(address)) &&
                    (virtual_address(page).pde() ==
                     virtual_address(address).pde())) {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * Rejected allocations, kept alive so the allocator cannot hand the same
 * address back on the next attempt.
 */
std::vector<std::unique_ptr<page_table>> g_rejected;

/**
 * A page table on the host heap, already mapped into itself, whose
 * storage does not collide with any test address.
 *
 * map_self first, always: map_page resolves the sub-table addresses
 * through *this*, so before map_self they do not resolve at all. What
 * happens when it is skipped is itself a check, below.
 *
 * alignof is 4096 because every member is alignas(4096), so this goes
 * through the aligned operator new - the same requirement the processor
 * has of the real thing.
 */
std::unique_ptr<page_table> fresh_table()
{
    for (int attempt{}; attempt < 32; ++attempt) {
        auto table = std::make_unique<page_table>();
        if (storage_collides_with_a_test_address(*table)) {
            g_rejected.push_back(std::move(table));
            continue;
        }
        table->map_self(identity_source{});
        return table;
    }

    check(false,
          "no allocation in 32 attempts placed the table clear of the "
          "test addresses - this is a harness problem, not a page table "
          "one");
    return std::make_unique<page_table>();
}

/**
 * The storage bound, which is the mechanical form of CLAUDE.md's "the
 * host page table stays narrow".
 *
 * 1 + 1 + 2 + 1024 pages: one PML4, one PDPT, two page directories and
 * 2 * 512 page tables. Two directories are what 512 PDPT slots fold
 * onto, so the *distinct* mappings this structure can hold are
 * 2 (bit 38) * 512 (bits 29:21) * 512 (bits 20:12) pages = 2 GB, and
 * every address outside that aliases onto one inside it. There is no
 * allocation anywhere in page_table, so this is the whole of it.
 */
static_assert(sizeof(page_table) == 1028 * 4096);
static_assert(alignof(page_table) == 4096);

void mapping_round_trips()
{
    auto table = fresh_table();
    identity_source source;

    // The composition initialize_host_page_table does: the physical
    // address of the table's own head is what goes into CR3, and it can
    // only be asked of the table itself once map_self has run.
    auto head_address = reinterpret_cast<std::uint64_t>(&table->head());
    check_equal(head_address,
                table->virtual_to_physical(&table->head()),
                "map_self makes the table's own head resolve to itself, "
                "which is what host_cr3 is composed from");

    constexpr std::uint64_t base = 0x40000000;
    constexpr std::size_t size = 4 * page_size;

    table->map_from(base,
                    size,
                    protection::read | protection::write |
                        protection::execute,
                    source);

    for (std::uint64_t offset{}; offset < size; offset += page_size) {
        check_equal(base + offset,
                    table->virtual_to_physical(base + offset),
                    "an identity mapping round trips at " +
                        hex(base + offset));

        auto walk = hardware_walk(*table, base + offset);
        check(walk.present && (walk.level == 1),
              "and the walk terminates at a present 4 KB leaf at " +
                  hex(base + offset));
        check_equal(base + offset,
                    walk.physical,
                    "and a processor would agree at " +
                        hex(base + offset));
    }

    check_equal(base + 0x123,
                table->virtual_to_physical(base + 0x123),
                "a byte part way into a mapped page keeps its offset");

    check_equal(
        table->virtual_to_physical(base + 0x123),
        table->virtual_to_physical(
            reinterpret_cast<const void *>(base + 0x123)),
        "the pointer overload of virtual_to_physical forwards to the "
        "integer one");

    // Protection lands in the leaf only, and every level above stays
    // permissive. SDM 5.6.1 takes access rights as the conjunction over
    // the whole walk, so a restriction written higher up would apply to
    // every mapping under that entry - and those tables are shared.
    auto & leaf = table->page_table_entry(base);
    check(leaf.present(), "the leaf is present");
    check(leaf.write(), "the leaf is writable when write was asked for");
    check(!leaf.execute_disable(),
          "and executable when execute was asked for");
    check(!leaf.large(), "and it is a 4 KB leaf, never a large page");

    virtual_address structure{base};
    const auto * pml4 = &table->head();
    const auto & pml4e = pml4[structure.pml4e()];
    check(pml4e.present() && pml4e.write() && !pml4e.execute_disable(),
          "the PML4 entry above the mapping is present, writable and "
          "executable - SDM 5.6.1 makes rights the conjunction over the "
          "walk, so a restriction here would cost 512 GB of mappings");

    const auto * pdpt =
        reinterpret_cast<const pte *>(pml4e.page_number() << 12);
    const auto & pdpte = pdpt[structure.pdpte()];
    check(pdpte.present() && pdpte.write() && !pdpte.execute_disable(),
          "and so is the PDPT entry");

    const auto * pd =
        reinterpret_cast<const pte *>(pdpte.page_number() << 12);
    const auto & pde = pd[structure.pde()];
    check(pde.present() && pde.write() && !pde.execute_disable(),
          "and so is the page directory entry");

    // A read-only, non-executable mapping - the shape the APIC page and
    // the controller register pages get.
    constexpr std::uint64_t read_only = 0x50000000;
    table->map_from(read_only, page_size, protection::read, source);

    auto & restricted = table->page_table_entry(read_only);
    check(restricted.present(), "a read-only mapping is present");
    check(!restricted.write(), "and not writable");
    check(restricted.execute_disable(),
          "and marked execute-disable, since execute was not asked for");
    check_equal(read_only,
                table->virtual_to_physical(read_only),
                "and it still translates");

    // And the write-only-looking case: protection is a bitmask, so read
    // alone and write alone differ only in the leaf's R/W bit. There is
    // no "not readable" to express - x86-64 paging has no such bit
    // outside EPT, which is why protection::read has no effect of its
    // own here.
    constexpr std::uint64_t writable = 0x50000000;
    table->map_from(
        writable, page_size, protection::read | protection::write, source);
    check(table->page_table_entry(writable).write(),
          "remapping the same page writable sets R/W back");
}

void scattered_sources_are_translated_per_page()
{
    auto table = fresh_table();
    scatter_source scattered;

    constexpr std::uint64_t base = 0x10000000;
    constexpr std::size_t pages = 8;

    table->map_from(base,
                    pages * page_size,
                    protection::read | protection::write,
                    scattered);

    for (std::size_t i{}; i < pages; ++i) {
        auto address = base + (i * page_size);
        check_equal(scatter_source::physical_of(address),
                    table->virtual_to_physical(address),
                    "page " + std::to_string(i) +
                        " of a scattered range is translated on its own "
                        "- map_from asks the source once per page");
        check_equal(table->virtual_to_physical(address),
                    hardware_walk(*table, address).physical,
                    "and a processor would agree for page " +
                        std::to_string(i));
    }

    // A mapping is not a range: the page after the last one mapped is
    // untouched.
    auto after = base + (pages * page_size);
    check(!hardware_walk(*table, after).present,
          "the page after a scattered range is not present");
}

void unmapped_addresses_are_not_refused()
{
    auto table = fresh_table();

    // Defect 2. There is no present check anywhere in
    // page_table::virtual_to_physical and no failure value in its return
    // type, so an address nothing mapped is answered with the page
    // number of a zeroed entry shifted up, plus the offset - the offset
    // alone.
    constexpr std::uint64_t unmapped = 0x60000000;

    check(!hardware_walk(*table, unmapped).present,
          "nothing maps the address this check uses");

    check_equal(0,
                table->virtual_to_physical(unmapped),
                "an unmapped, page aligned address answers zero rather "
                "than being refused");

    // And this is the part that matters. read_guest_word and
    // apply_guest_store both test the result as a boolean, take zero to
    // mean "not mapped", and dereference the address otherwise. For any
    // unmapped address whose low twelve bits are not zero that test
    // passes.
    check_equal(0x8,
                table->virtual_to_physical(unmapped + 8),
                "while an unmapped address eight bytes into its page "
                "answers 8, which is non-zero - so the boolean guard in "
                "read_guest_word and apply_guest_store passes and the "
                "address is dereferenced anyway");

    // The ambiguity runs the other way too: a page legitimately mapped
    // to physical zero is indistinguishable from an unmapped one.
    constexpr std::uint64_t mapped_low = 0x70000000;
    table->map_from(mapped_low,
                    page_size,
                    protection::read | protection::write,
                    scatter_source{});
    table->page_table_entry(mapped_low).page_number(0);
    check(hardware_walk(*table, mapped_low).present,
          "a page mapped to physical zero is present");
    check_equal(0,
                table->virtual_to_physical(mapped_low),
                "yet it answers zero as well, so the same guard refuses "
                "a mapping that exists");
}

void boundaries_between_levels()
{
    struct span
    {
        std::uint64_t base;
        const char * what;
        int level; // the index that differs across the boundary
    };

    // Two pages straddling a boundary, so they differ in exactly one
    // index and agree in every index above it.
    constexpr span spans[]{
        {large_page_size - page_size, "a 2 MB boundary", 2},
        {huge_page_size - page_size, "a 1 GB boundary", 3},
        {pml4e_span - page_size, "a 512 GB boundary", 4},
    };

    for (const auto & test : spans) {
        // A fresh table per span. Sharing one would be a different test:
        // the high side of the 512 GB span and the high side of the 1 GB
        // span land on the same leaf, which is the aliasing this
        // structure has and is asserted about on its own below.
        auto table = fresh_table();
        identity_source source;

        table->map_from(test.base,
                        2 * page_size,
                        protection::read | protection::write,
                        source);

        auto low = test.base;
        auto high = test.base + page_size;

        check_equal(low,
                    table->virtual_to_physical(low),
                    std::string("the page below ") + test.what +
                        " translates");
        check_equal(high,
                    table->virtual_to_physical(high),
                    std::string("the page above ") + test.what +
                        " translates");

        auto low_walk = hardware_walk(*table, low);
        auto high_walk = hardware_walk(*table, high);
        check(low_walk.present && high_walk.present,
              std::string("both sides of ") + test.what +
                  " are present in the built structure");
        check_equal(low,
                    low_walk.physical,
                    std::string("and a processor agrees below ") +
                        test.what);
        check_equal(high,
                    high_walk.physical,
                    std::string("and above ") + test.what);

        // The two pages really are in different structures at the level
        // the boundary names, which is what makes this a boundary test
        // rather than two mappings that happen to work.
        virtual_address a{low};
        virtual_address b{high};
        switch (test.level) {
        case 2:
            check(a.pde() != b.pde(),
                  "the 2 MB boundary changes the page directory index");
            break;
        case 3:
            check(a.pdpte() != b.pdpte(),
                  "the 1 GB boundary changes the PDPT index");
            break;
        case 4:
            check(a.pml4e() != b.pml4e(),
                  "the 512 GB boundary changes the PML4 index");
            break;
        default:
            break;
        }

        // No large entry is ever produced. map_page_from writes a 4 KB
        // leaf and sets present on every level above it; nothing in
        // page_table sets PS at all, so there is also nothing to split -
        // unlike epte_for, which splits a 2 MB EPT entry on demand.
        check(!low_walk.large && !high_walk.large,
              std::string("and neither side of ") + test.what +
                  " became a large page - page_table cannot create one");
    }
}

void repeated_and_overlapping_mappings()
{
    auto table = fresh_table();
    identity_source source;

    constexpr std::uint64_t base = 0x20000000;

    // Idempotent, which initialize_host_page_table relies on where it
    // maps the four controller register addresses a page at a time and
    // says so: "The four usually share one page ... so this is normally
    // a single mapping done four times, and map_from is idempotent."
    table->map_from(
        base, page_size, protection::read | protection::write, source);
    auto first = table->page_table_entry(base).value();

    table->map_from(
        base, page_size, protection::read | protection::write, source);
    check_equal(first,
                table->page_table_entry(base).value(),
                "mapping the same page twice the same way changes "
                "nothing - map_from is idempotent");

    // Overlapping ranges: the later mapping wins, whole entry, including
    // the protection. There is no merge and no refusal.
    table->map_from(base, 2 * page_size, protection::read, source);
    auto & leaf = table->page_table_entry(base);
    check(!leaf.write(),
          "an overlapping mapping replaces the protection of the pages "
          "it covers rather than merging with it");
    check(leaf.execute_disable(),
          "and takes the execute permission with it");
    check_equal(base,
                table->virtual_to_physical(base),
                "while the translation itself is unchanged");

    // And a remap to a different physical page takes effect at once.
    table->map_from(base,
                    page_size,
                    protection::read | protection::write,
                    scatter_source{});
    check_equal(scatter_source::physical_of(base),
                table->virtual_to_physical(base),
                "remapping a page to a different physical address "
                "replaces the old one");
}

void alignment_of_the_address_and_the_size()
{
    auto table = fresh_table();
    identity_source source;

    // Size rounds up: the page count is ceil(size / 4096), so one byte
    // maps one page and one byte over a page maps two.
    constexpr std::uint64_t single = 0x30000000;
    table->map_from(single, 1, protection::read, source);
    check(hardware_walk(*table, single).present,
          "a one byte range maps the page containing it");
    check(!hardware_walk(*table, single + page_size).present,
          "and not the page after it");

    constexpr std::uint64_t two = 0x31000000;
    table->map_from(two, page_size + 1, protection::read, source);
    check(hardware_walk(*table, two).present &&
              hardware_walk(*table, two + page_size).present,
          "a range one byte over a page maps two pages");

    constexpr std::uint64_t none = 0x32000000;
    table->map_from(none, 0, protection::read, source);
    check(!hardware_walk(*table, none).present,
          "a zero length range maps nothing");

    // An unaligned base still covers the page it starts in: the leaf is
    // selected by bits 20:12, so the offset is discarded on the way in,
    // and the physical address written is the source's answer with its
    // own offset discarded by the shift. That is the shape elf_file's
    // preferred-base rounding depends on elsewhere in the tree.
    constexpr std::uint64_t unaligned = 0x33000800;
    table->map_from(unaligned, page_size, protection::read, source);
    auto walk = hardware_walk(*table, unaligned);
    check(walk.present,
          "an unaligned base still covers the page it starts in");
    check_equal(unaligned >> 12,
                walk.entry.page_number(),
                "and the entry names the page containing it, with the "
                "offset dropped rather than carried into the frame");
    check_equal(unaligned,
                walk.physical,
                "so the byte itself still translates to itself");

    // But the tail is not covered, which is what the comment in
    // detail/page_table.h warns about: "Rounding the size up assumes a
    // page aligned base; an unaligned one would leave the last partial
    // page unmapped. Every caller passes an aligned base."
    //
    // Checked against the callers, all five: module_base comes from
    // allocate_rwx, apic_base is masked with 0xffffff000, the controller
    // register pages are masked with ~(page_size - 1), the AP start-up
    // memory is rejected outright unless aligned (hypervisor.cpp,
    // "start-up memory at {} is not page aligned"), and the diagnostic
    // channel's queue storage is validated page aligned by
    // handover::valid() in zpp/nvme/log_format.h. The assumption holds
    // today; this check is what says so if a sixth caller appears.
    check(!hardware_walk(*table, unaligned + page_size).present,
          "while the tail past the end of that page is left unmapped, "
          "which is why every caller passes an aligned base");
}

void the_structure_aliases_and_that_is_the_bound()
{
    auto table = fresh_table();
    identity_source source;

    // 512 PDPT slots share two page directories, so the choice is
    // address bit 38 alone and the 256 slots on each side share
    // everything below them. detail/page_table.h says so, and says
    // nothing detects it. This is what it costs.
    constexpr std::uint64_t first = 1 * huge_page_size;
    constexpr std::uint64_t second = 2 * huge_page_size;

    virtual_address a{first};
    virtual_address b{second};
    check(a.pdpte() != b.pdpte(),
          "the two addresses differ in their PDPT index");
    check((a.pde() == b.pde()) && (a.pte() == b.pte()) &&
              (directory_of(first) == directory_of(second)),
          "and agree in bit 38 and in everything below the PDPT index");

    table->map_from(
        first, page_size, protection::read | protection::write, source);
    check_equal(first,
                table->virtual_to_physical(first),
                "the first of the two maps");

    table->map_from(
        second, page_size, protection::read | protection::write, source);
    check_equal(second,
                table->virtual_to_physical(second),
                "and so does the second");

    check_equal(second,
                table->virtual_to_physical(first),
                "but the second silently replaced the first: two "
                "addresses agreeing in bit 38 and bits 29:12 share one "
                "leaf however far apart they are");
    check_equal(second,
                hardware_walk(*table, first).physical,
                "and a processor would make the same substitution - the "
                "structure aliases, the walker does not lie about it");

    // Bits 47:39 are ignored outright. Every PML4 entry map_page_from
    // writes points at the same single PDPT, so two addresses 512 GB
    // apart translate identically - and virtual_to_physical does not
    // even read the PML4, which is why it starts at the PDPT and says
    // so.
    constexpr std::uint64_t low_half = 0x1000;
    constexpr std::uint64_t high_half = pml4e_span + 0x1000;
    table->map_from(
        low_half, page_size, protection::read | protection::write, source);
    check_equal(low_half,
                table->virtual_to_physical(low_half),
                "a page in the first 512 GB maps");
    check_equal(low_half,
                table->virtual_to_physical(high_half),
                "and the address 512 GB above it answers the same, "
                "because virtual_to_physical starts at the PDPT and "
                "never reads the PML4 index at all");

    // Which is more than aliasing: for that address a processor would
    // fault, because nothing has written the PML4 entry covering the
    // second 512 GB and the walk stops there. This walker answers
    // anyway - the same shape as defect 2, one level up.
    check(!hardware_walk(*table, high_half).present,
          "a processor would fault on it instead: the PML4 entry for "
          "the second 512 GB has never been written, and skipping the "
          "PML4 is what lets virtual_to_physical answer for it");

    // Once something does map an address up there the entry appears,
    // pointing at the same single PDPT, and the two agree again - on the
    // aliased answer.
    table->map_from(high_half,
                    page_size,
                    protection::read | protection::write,
                    source);
    check_equal(high_half,
                hardware_walk(*table, high_half).physical,
                "and once that region is mapped a processor agrees");
    check_equal(high_half,
                table->virtual_to_physical(low_half),
                "while the page in the first 512 GB now answers with "
                "the second one's physical address, the alias having "
                "closed from the other side");

    // So the bound worth stating is 2 * 512 * 512 pages of distinct
    // mappings, two gigabytes, addressed by bit 38 and bits 29:12.
    // CLAUDE.md's "the host page table stays narrow" is not a style
    // preference here, it is the size of the structure: there is no
    // allocation in page_table and nothing to grow.
    check_equal(1028ull * page_size,
                sizeof(page_table),
                "the whole structure is 1028 pages, which is 2 GB of "
                "distinct mappings and no more");
}

void map_page_before_map_self_is_wrong()
{
    // map_page resolves the sub-table addresses through *this*, so on a
    // table that has not mapped itself the answer is whatever an empty
    // table gives - the offset of a page aligned array, which is zero.
    // The result is a table whose PML4 entry names physical address
    // zero. This is why self_map_from exists and why map_self is the
    // first thing initialize_host_page_table does; it is stated here so
    // that a caller which gets the order wrong has something to
    // recognise.
    auto table = std::make_unique<page_table>();
    table->map_page(
        0x40000000, 0x40000000, protection::read | protection::write);

    const auto * pml4 = &table->head();
    const auto & pml4e = pml4[virtual_address(0x40000000).pml4e()];
    check(pml4e.present(),
          "map_page on a table that has not mapped itself does produce a "
          "present PML4 entry");
    check_equal(0,
                pml4e.page_number(),
                "but it names physical address zero rather than the "
                "PDPT, because it asked itself where the PDPT was and "
                "did not know yet - hence map_self first");

    // The leaf is written correctly even so, because map_page_from
    // indexes the member arrays directly and only the *contents* of the
    // upper entries come from the source table. So the failure is
    // invisible to virtual_to_physical, which also indexes the members
    // directly, and visible only to the processor.
    check_equal(0x40000000,
                table->virtual_to_physical(0x40000000),
                "and virtual_to_physical still answers correctly, "
                "because it indexes the members rather than following "
                "the addresses - only a processor would notice");
}

void large_entries_are_translated_too_high()
{
    // Defect 1 at the page_table walker. page_table cannot create a
    // large entry - map_page_from writes a 4 KB leaf every time - so one
    // is installed by hand, exactly as firmware would have it: SDM
    // Table 5-18 (sdm.txt:157221) puts the 2 MB page's address in bits
    // 51:21 with PS set at bit 7.
    //
    // Reaching a directory entry needs a const_cast in spirit: the API
    // exposes a const head and a mutable leaf and nothing between, so
    // the pointer is rebuilt from the entry contents. The memory is this
    // harness's own object, so the write is defined.
    auto table = fresh_table();
    identity_source source;

    constexpr std::uint64_t address = 0x40000000;
    constexpr std::uint64_t physical = 0x80000000;

    table->map_from(
        address, page_size, protection::read | protection::write, source);

    virtual_address structure{address};
    const auto * pml4 = &table->head();
    const auto & pml4e = pml4[structure.pml4e()];
    const auto * const_pdpt =
        reinterpret_cast<const pte *>(pml4e.page_number() << 12);
    auto * pd = reinterpret_cast<pte *>(
        const_pdpt[structure.pdpte()].page_number() << 12);
    auto & pde = pd[structure.pde()];

    pde = pte{physical | 0x83}; // PS | R/W | P
    check(pde.large(), "the hand-installed PDE is a 2 MB entry");

    auto walk = hardware_walk(*table, address + 0x1234);
    check_equal(physical + 0x1234,
                walk.physical,
                "a processor translates the 2 MB page from bits 51:21 "
                "of the entry (SDM Table 5-18, sdm.txt:157243)");

    // These two used to assert the defect: virtual_to_physical answered
    // `page_number() << 21`, and `page_number()` is already bits 51:12,
    // so the answer was the true base shifted left by nine. 566a633
    // added `large_page_number()` to pte.h - the accessor ept.h had all
    // along, which is the whole reason the two sides disagreed - and the
    // pair is now stated the right way round.
    //
    // The first of the two is the one that matters: the class and a
    // processor have to give the same answer. It is asserted against
    // `hardware_walk`'s result rather than against a literal, so it
    // cannot be satisfied by both of them being wrong the same way.
    auto answered = table->virtual_to_physical(address + 0x1234);
    check_equal(walk.physical,
                answered,
                "virtual_to_physical agrees with a processor's own walk "
                "of the 2 MB page - 566a633");
    check_equal(physical + 0x1234,
                answered,
                "which is bits 51:21 of the entry plus bits 20:0 of the "
                "address, not page_number() << 21");

    // page_table_entry stops at the entry that terminates the walk,
    // which for a large page is the directory entry itself. Descending
    // past it would hand back a leaf the processor never consults, and a
    // write to that leaf would appear to work and do nothing.
    check(&table->page_table_entry(address) == &pde,
          "page_table_entry returns the 2 MB directory entry itself, not "
          "a leaf underneath it");

    // The same defect one level up. SDM Table 5-16 (sdm.txt:157030) puts
    // a 1 GB page's address in bits 51:30, so the error is eighteen bits
    // rather than nine.
    auto huge = fresh_table();
    huge->map_from(
        address, page_size, protection::read | protection::write, source);

    const auto * huge_pml4 = &huge->head();
    auto * huge_pdpt = reinterpret_cast<pte *>(
        huge_pml4[structure.pml4e()].page_number() << 12);
    auto & pdpte = huge_pdpt[structure.pdpte()];
    pdpte = pte{huge_page_size | 0x83};

    check_equal(huge_page_size + 0x1234,
                hardware_walk(*huge, address + 0x1234).physical,
                "a processor translates the 1 GB page from bits 51:30 "
                "of the entry");
    check_equal(huge_page_size + 0x1234,
                huge->virtual_to_physical(address + 0x1234),
                "and virtual_to_physical agrees with it - bits 51:30 of "
                "the entry, not page_number() << 30");
    check(&huge->page_table_entry(address) == &pdpte,
          "and page_table_entry stops at the 1 GB entry");
}

/*
 * =====================================================================
 * 4. os_page_table
 *
 * The walker over a table this VMM did not build. It is given a CR3 and
 * a callback that turns a physical address inside that table into a
 * virtual one, because the walk runs on the OS's own mapping and an
 * entry names the *physical* address of the next structure.
 *
 * The fake physical memory below is a small array of pages with physical
 * address == index * 4096, so the callback is a real function of the
 * physical address rather than a lookup that could accidentally be
 * right.
 * =====================================================================
 */

constexpr std::size_t fake_page_count = 12;

alignas(4096) std::uint64_t g_physical_memory[fake_page_count][512]{};

/**
 * Page indices, by role. Page 0 is deliberately not one of the tables:
 * it is where an entry with a zero page number leads, so it is what a
 * not-present entry walks into, and its contents are the answer to "what
 * does this walker do instead of refusing".
 */
enum : std::size_t
{
    zero_page = 0,
    pml4_page = 1,
    pdpt_page = 2,
    pd_page = 3,
    pt_page = 4,
    data_page = 5,
    decoy_page = 6,
};

std::size_t g_translation_calls{};
std::uint64_t g_last_translation{};
std::size_t g_out_of_range_translations{};

/**
 * Stands in for the loader's physical-to-virtual callback -
 * MmGetVirtualForPhysical on Windows, __va on Linux. Out of range
 * answers with the last page rather than a null pointer, so a walk that
 * goes somewhere it should not lands in defined memory and can be
 * asserted about instead of taking the harness down.
 */
std::uint64_t fake_physical_to_virtual(std::uint64_t physical)
{
    ++g_translation_calls;
    g_last_translation = physical;

    auto page = physical >> 12;
    if (page >= fake_page_count) {
        ++g_out_of_range_translations;
        page = fake_page_count - 1;
    }

    return reinterpret_cast<std::uint64_t>(&g_physical_memory[page][0]) +
           (physical & 0xfff);
}

constexpr std::uint64_t fake_physical_of(std::size_t page)
{
    return static_cast<std::uint64_t>(page) * page_size;
}

/**
 * The address every os_page_table check walks, chosen so that all four
 * indices differ from each other and none is zero - an index confused
 * for another one then lands on an empty slot rather than on the right
 * answer.
 */
constexpr std::uint64_t walked_address =
    (1ull << 39) | (2ull << 30) | (3ull << 21) | (4ull << 12) | 0x123;

/**
 * Builds a four level table in the fake physical memory: one present
 * entry at each level, selected by the indices of walked_address, with
 * the leaf naming data_page.
 *
 * 0x67 is present, write, user, accessed, dirty - everything but PS.
 */
void build_fake_table()
{
    for (auto & page : g_physical_memory) {
        for (auto & entry : page) {
            entry = 0;
        }
    }

    virtual_address structure{walked_address};

    g_physical_memory[pml4_page][structure.pml4e()] =
        fake_physical_of(pdpt_page) | 0x67;
    g_physical_memory[pdpt_page][structure.pdpte()] =
        fake_physical_of(pd_page) | 0x67;
    g_physical_memory[pd_page][structure.pde()] =
        fake_physical_of(pt_page) | 0x67;
    g_physical_memory[pt_page][structure.pte()] =
        fake_physical_of(data_page) | 0x67;

    // A sentinel in the first PML4 slot, which walked_address does not
    // use, so that head() has something of its own to return.
    g_physical_memory[pml4_page][0] = 0xabcde000 | 0x67;

    // Physical page zero, and the page it names. Every slot in both
    // looks like a perfectly ordinary present entry, which is what a
    // real physical page zero looks like to a walker that does not check
    // anything: the real-mode interrupt vector table is 1024 entries of
    // plausible-looking nonzero words. Both pages point at decoy_page so
    // that a walk which falls into either terminates somewhere nameable.
    for (auto & entry : g_physical_memory[zero_page]) {
        entry = fake_physical_of(decoy_page) | 0x67;
    }
    for (auto & entry : g_physical_memory[decoy_page]) {
        entry = fake_physical_of(decoy_page) | 0x67;
    }

    g_translation_calls = 0;
    g_last_translation = 0;
    g_out_of_range_translations = 0;
}

void the_os_walk_finds_the_right_page()
{
    build_fake_table();

    os_page_table table(fake_physical_of(pml4_page),
                        fake_physical_to_virtual);

    check(bool(table),
          "an os_page_table built with a callback is initialized");
    check_equal(0xabcde067,
                table.head(),
                "and its head is the first entry of the PML4 that CR3 "
                "named");

    check_equal(fake_physical_of(data_page) + 0x123,
                table.virtual_to_physical(walked_address),
                "the four level walk finds the page the leaf names and "
                "keeps the offset");

    check_equal(table.virtual_to_physical(walked_address),
                table.virtual_to_physical(
                    reinterpret_cast<const void *>(walked_address)),
                "and the pointer overload forwards to it");

    // Each level really was consulted through the callback: three
    // structures below the PML4, which the constructor resolved once.
    g_translation_calls = 0;
    static_cast<void>(table.virtual_to_physical(walked_address));
    check_equal(3,
                g_translation_calls,
                "one callback per structure below the PML4 - an entry "
                "names a physical address, so following it as a pointer "
                "would read whatever the OS has at that virtual address");

    // Every index is used, and used at the right level: moving one index
    // moves the answer to a slot that was never filled in.
    virtual_address elsewhere{walked_address};
    elsewhere.pde(elsewhere.pde() + 1);
    check(table.virtual_to_physical(low_48_of(elsewhere) | (1ull << 39)) !=
              fake_physical_of(data_page) + 0x123,
          "changing the page directory index alone changes the answer");

    // The CR3 the constructor is given is masked before use: the low
    // twelve bits are PCID, or PWT and PCD, depending on CR4.PCIDE, and
    // never part of the address.
    g_last_translation = 0;
    os_page_table with_pcid(fake_physical_of(pml4_page) | 0xfff,
                            fake_physical_to_virtual);
    check_equal(fake_physical_of(pml4_page),
                g_last_translation,
                "the constructor masks the low twelve bits out of CR3 "
                "before asking for its virtual address");
    check_equal(fake_physical_of(data_page) + 0x123,
                with_pcid.virtual_to_physical(walked_address),
                "and the resulting table still walks");

    check_equal(0,
                g_out_of_range_translations,
                "and no walk asked for a physical page outside the fake "
                "memory");
}

void the_os_walk_handles_large_pages()
{
    virtual_address structure{walked_address};

    // A 2 MB leaf at the page directory. Its address field is bits 51:21
    // (SDM Table 5-18, sdm.txt:157243), so the page it names is 2 MB
    // aligned and therefore outside the fake memory - which is fine,
    // because what is asserted is the arithmetic, not the contents.
    build_fake_table();
    constexpr std::uint64_t large_base = 0x200000;
    g_physical_memory[pd_page][structure.pde()] = large_base | 0xe7;

    os_page_table table(fake_physical_of(pml4_page),
                        fake_physical_to_virtual);

    // This is the copy of the walk that was live: the Windows and Linux
    // loaders pass a real physical_to_virtual, and the tables they hand
    // over do use 2 MB leaves. Under UEFI the callback is null and the
    // walk returns its argument untouched, which is why the rig - which
    // boots only UEFI - never produced a wrong answer here.
    //
    // Asserted the right way round since 566a633. Stated as base plus
    // bits 20:0 rather than as any shift of the entry, because that is
    // what SDM Table 5-18 says the processor computes, and a check
    // written in terms of `page_number()` would be agreeing with the
    // accessor rather than with the architecture.
    auto answered = table.virtual_to_physical(walked_address);
    check_equal(large_base + structure.large_offset(),
                answered,
                "the 2 MB walk answers base plus bits 20:0, which is "
                "what the SDM computes - 566a633");
    check_equal(large_base + 0x4123,
                large_base + structure.large_offset(),
                "(and bits 20:0 of the address are 0x4123)");

    // A 1 GB leaf at the page directory pointer table, off by eighteen
    // bits for the same reason.
    build_fake_table();
    constexpr std::uint64_t huge_base = 0x40000000;
    g_physical_memory[pdpt_page][structure.pdpte()] = huge_base | 0xe7;

    os_page_table huge(fake_physical_of(pml4_page),
                       fake_physical_to_virtual);
    check_equal(huge_base + structure.huge_offset(),
                huge.virtual_to_physical(walked_address),
                "and the 1 GB page answers base plus bits 29:0");
    check_equal(0x604123,
                structure.huge_offset(),
                "(and bits 29:0 of the address are 0x604123)");

    // PS at the PML4 level is reserved - SDM 5.5.4, "The PS flag is
    // reserved in a PML5E or a PML4E" (sdm.txt:157075) - and this walker
    // agrees with that by not looking at it. Setting it changes nothing,
    // which is the behaviour os_page_table.cpp's comment claims.
    build_fake_table();
    g_physical_memory[pml4_page][structure.pml4e()] |= 0x80;
    os_page_table ignored(fake_physical_of(pml4_page),
                          fake_physical_to_virtual);
    check_equal(fake_physical_of(data_page) + 0x123,
                ignored.virtual_to_physical(walked_address),
                "PS in a PML4 entry is reserved, and the walk descends "
                "past it as if it were clear");
}

void a_not_present_entry_is_not_refused()
{
    virtual_address structure{walked_address};

    // Defect 2, in the walker where it is most dangerous. There is no
    // present check at any level and no failure value in the return
    // type, so clearing an entry does not stop the walk - it redirects
    // it to whatever physical page zero contains, which on a real
    // machine is the real-mode interrupt vector table and the BIOS data
    // area. Here that page is filled with entries that look present and
    // name decoy_page, so the walk runs to the end and produces a
    // plausible, wrong address.
    struct level
    {
        std::size_t page;
        std::uint64_t index;
        const char * name;
    };

    const level levels[]{
        {pml4_page, structure.pml4e(), "PML4 entry"},
        {pdpt_page, structure.pdpte(), "PDPT entry"},
        {pd_page, structure.pde(), "page directory entry"},
    };

    for (const auto & test : levels) {
        build_fake_table();
        g_physical_memory[test.page][test.index] = 0;

        os_page_table table(fake_physical_of(pml4_page),
                            fake_physical_to_virtual);

        auto answer = table.virtual_to_physical(walked_address);

        check(answer != 0,
              std::string("a not-present ") + test.name +
                  " does not stop the walk: it answers " + hex(answer) +
                  " rather than refusing");
        check_equal(fake_physical_of(decoy_page) + 0x123,
                    answer,
                    std::string("and the answer is whatever physical "
                                "page zero's contents lead to, which is "
                                "what makes it plausible: ") +
                        test.name);
    }

    // The leaf is the one level where the wrong answer is quiet rather
    // than plausible: a zeroed PTE names page number zero, so the result
    // is the offset alone - non-zero for any address that is not page
    // aligned, which is defect 2 again from the other end.
    build_fake_table();
    g_physical_memory[pt_page][structure.pte()] = 0;
    os_page_table table(fake_physical_of(pml4_page),
                        fake_physical_to_virtual);
    check_equal(0x123,
                table.virtual_to_physical(walked_address),
                "a not-present leaf answers the offset within the page");
}

void the_uefi_case_has_no_callback()
{
    // With no callback the platform identity maps and the address is its
    // own answer - the UEFI case, per zpp_loader_parameters, and the
    // reason initialize_host_page_table can walk the OS table there at
    // all.
    os_page_table table(0x1234000, nullptr);

    check_equal(0xdeadbeef,
                table.virtual_to_physical(std::uint64_t{0xdeadbeef}),
                "with no callback the walk is skipped and the address is "
                "returned unchanged");
    check_equal(0xffff800000001234ull,
                table.virtual_to_physical(0xffff800000001234ull),
                "including a canonical high-half address, which no walk "
                "of this fake memory would have produced");

    // And the trap in that: the object converts to false, because the
    // constructor only sets its PML4 pointer when there is a callback to
    // resolve one with. So `if (table)` is false on the one platform
    // where the table works perfectly. Nothing in the tree tests it that
    // way today - this check is here so that a caller which starts to
    // finds out from a test rather than from a boot.
    check(!bool(table),
          "yet the object converts to false, since there is no PML4 "
          "pointer to hold - operator bool means 'has a callback', not "
          "'usable'");

    os_page_table empty;
    check(!bool(empty),
          "a default constructed os_page_table is false too");
}

/*
 * =====================================================================
 * 5. memory types
 *
 * A page table entry here carries no memory type of its own: the type of
 * a page comes from the PAT, PCD and PWT bits selecting one of eight
 * entries in IA32_PAT, which is SDM 14.12.3 (sdm.txt:174684): "The PAT
 * bit is bit 7 in page-table entries that point to 4-KByte pages and bit
 * 12 in paging-structure entries that point to larger pages. The PCD and
 * PWT bits are bits 4 and 3, respectively".
 *
 * So the memory type test for these headers is two things: that those
 * three bits are where the SDM puts them, and that the memory_type
 * encodings are the architecture's. The derivation that consumes them is
 * tests/mtrr's subject, not this one's.
 * =====================================================================
 */

// SDM Vol. 3A Table 14-8, "Memory Types That Can Be Encoded in MTRRs"
// (sdm.txt:173830), and Table 14-10, "Memory Types That Can Be Encoded
// With PAT" (sdm.txt:174665), which agree on all five of these.
static_assert(static_cast<int>(memory_type::uncachable) == 0);
static_assert(static_cast<int>(memory_type::write_combining) == 1);
static_assert(static_cast<int>(memory_type::write_through) == 4);
static_assert(static_cast<int>(memory_type::write_protected) == 5);
static_assert(static_cast<int>(memory_type::write_back) == 6);

void memory_type_encodings()
{
    check_equal(0,
                static_cast<int>(memory_type::uncachable),
                "UC is 00H (SDM Table 14-8, sdm.txt:173832)");
    check_equal(
        1, static_cast<int>(memory_type::write_combining), "WC is 01H");
    check_equal(4,
                static_cast<int>(memory_type::write_through),
                "WT is 04H - 02H and 03H are reserved, which is why "
                "there is no enumerator between them");
    check_equal(
        5, static_cast<int>(memory_type::write_protected), "WP is 05H");
    check_equal(6, static_cast<int>(memory_type::write_back), "WB is 06H");

    // 07H is UC- in the PAT (Table 14-10, sdm.txt:174672) and reserved
    // in an MTRR. This enum is used for MTRRs and for EPT entries as
    // well as for reasoning about the PAT, so it has no enumerator for
    // it, and every value it does have is legal in all three.
    for (auto type : {memory_type::uncachable,
                      memory_type::write_combining,
                      memory_type::write_through,
                      memory_type::write_protected,
                      memory_type::write_back}) {
        auto value = static_cast<int>(type);
        check((value != 2) && (value != 3) && (value != 7),
              "no memory_type enumerator carries an encoding that would "
              "#GP in an MTRR: " +
                  std::to_string(value));
    }

    // The three bits that index the PAT, in a page table entry. Their
    // positions are the whole of what a page table entry says about the
    // memory type of the page it maps.
    pte entry{};
    entry.write_through(true);
    check_equal(1ull << 3, entry.value(), "PWT is bit 3");
    entry = pte{};
    entry.page_level_cache_disable(true);
    check_equal(1ull << 4, entry.value(), "PCD is bit 4");
    entry = pte{};
    entry.pat(true);
    check_equal(1ull << 7,
                entry.value(),
                "and PAT is bit 7 for an entry mapping a 4 KB page");

    // A memory type set on an entry reads back: the three bits round
    // trip together, over all eight indices, and independently of each
    // other.
    for (unsigned index{}; index < 8; ++index) {
        pte typed{};
        typed.write_through(index & 1);
        typed.page_level_cache_disable((index >> 1) & 1);
        typed.pat((index >> 2) & 1);

        auto read_back = (typed.write_through() ? 1u : 0u) |
                         (typed.page_level_cache_disable() ? 2u : 0u) |
                         (typed.pat() ? 4u : 0u);
        check_equal(index,
                    read_back,
                    "PAT index " + std::to_string(index) +
                        " reads back off a page table entry");

        // And the entry says nothing else: the memory type costs three
        // bits and disturbs neither the present bit nor the frame.
        check(!typed.present() && (typed.page_number() == 0),
              "PAT index " + std::to_string(index) +
                  " leaves the rest of the entry alone");
    }
}

} // namespace

int main()
{
    // Unbuffered, which matters here and not in the other harnesses:
    // hardware_walk follows the physical addresses out of the structure
    // under test as pointers, so a table built badly enough takes the
    // process down rather than answering wrongly. Measured: writing the
    // leaf page number one bit off segfaults inside the walk. The FAIL
    // lines printed before that are then the whole of the evidence, and
    // a block buffered stdout loses every one of them.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    virtual_address_decomposition();
    entry_accessors();

    mapping_round_trips();
    scattered_sources_are_translated_per_page();
    unmapped_addresses_are_not_refused();
    boundaries_between_levels();
    repeated_and_overlapping_mappings();
    alignment_of_the_address_and_the_size();
    the_structure_aliases_and_that_is_the_bound();
    map_page_before_map_self_is_wrong();
    large_entries_are_translated_too_high();

    the_os_walk_finds_the_right_page();
    the_os_walk_handles_large_pages();
    a_not_present_entry_is_not_refused();
    the_uefi_case_has_no_callback();

    memory_type_encodings();

    std::println(
        "page_table: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
