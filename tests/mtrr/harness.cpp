/*
 * Regression harness for the MTRR memory-type derivation, and for the
 * question the EPT builder asks it.
 *
 * Why this is worth a harness of its own: under EPT the guest's MTRRs are
 * not consulted by hardware at all, so whatever
 * arch::x86_64::mtrr_state derives is the *only* memory type the guest's
 * physical address space has. SDM Vol. 3C 31.3.7.2, quoted in mtrr.h:
 * "The MTRRs have no effect on the memory type used for an access to a
 * guest-physical address." A wrong answer here is therefore not a
 * performance bug - it is cached MMIO, and the guest has no way to
 * correct it.
 *
 * Two defects this pins, both found on hardware and neither reachable
 * from an emulator:
 *
 *   63a5d17  Every range no variable MTRR covered got write-back,
 *            **including the MMIO hole above the top of DRAM**. The
 *            default type, the fixed ranges and the overlap precedence
 *            rules were all missing. realistic_machine() below is that
 *            case, stated as an assertion.
 *
 *   83012e7  The variable-range array was sized 8 and filled to
 *            IA32_MTRRCAP.VCNT with no bound. Real Intel client parts
 *            report 10 where the rig reports 8, so two entries were
 *            written past the end onto the capabilities member the same
 *            loop had just read.
 *
 * Hosted, native, no emulator and no target. mtrr.h depends on
 * memory_type.h, msr.h, <cstddef>, <cstdint>, <iterator>, <optional> and
 * <type_traits> and on nothing else, so the header under test compiles
 * here exactly as the hypervisor compiles it - the same reason
 * tests/elf_relocate and tests/decoder need no shim.
 *
 * The whole API is constexpr, so most claims below are made twice: once
 * as a static_assert, where the compile *is* the test and a
 * non-terminating loop is a compile error rather than a hang, and once at
 * runtime, where a failure prints which address disagreed and by how
 * much. Neither alone is enough - a static_assert that fails stops the
 * build with one message, and the interesting failures here are the ones
 * that need a diff.
 *
 * SDM line numbers are into .references/sdm.txt as fetched by
 * scripts/fetch-references.sh, and every one of them was looked up while
 * writing this rather than recalled.
 */
#include "zpp/arch/x86_64/mtrr.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <print>
#include <string>

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
    std::println("FAIL: {}\n  expected 0x{:x}\n  actual   0x{:x}",
                 what,
                 expected,
                 actual);
}

/**
 * The five names an MTRR or an EPT entry may carry, plus a spelling for
 * everything else - a reserved value is exactly what
 * valid_or_uncachable() exists to catch, so it has to be printable.
 */
const char * name_of(zpp::arch::x86_64::memory_type type)
{
    switch (type) {
    case zpp::arch::x86_64::memory_type::uncachable:
        return "UC";
    case zpp::arch::x86_64::memory_type::write_combining:
        return "WC";
    case zpp::arch::x86_64::memory_type::write_through:
        return "WT";
    case zpp::arch::x86_64::memory_type::write_protected:
        return "WP";
    case zpp::arch::x86_64::memory_type::write_back:
        return "WB";
    default:
        return "reserved";
    }
}

void check_type(zpp::arch::x86_64::memory_type expected,
                zpp::arch::x86_64::memory_type actual,
                const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}\n  expected {} ({})\n  actual   {} ({})",
                 what,
                 name_of(expected),
                 static_cast<int>(expected),
                 name_of(actual),
                 static_cast<int>(actual));
}

/**
 * The same for the answer uniform_type_of gives, whose empty state is not
 * a failure but a *result*: it is what tells the EPT builder to split a
 * 2 MB region into 4 KB entries.
 */
void check_optional_type(
    std::optional<zpp::arch::x86_64::memory_type> expected,
    std::optional<zpp::arch::x86_64::memory_type> actual,
    const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}\n  expected {}\n  actual   {}",
                 what,
                 expected ? name_of(*expected) : "nothing",
                 actual ? name_of(*actual) : "nothing");
}

constexpr std::uint64_t page_size = 0x1000;
constexpr std::uint64_t large_page_size = 0x200000;
constexpr std::uint64_t megabyte = 0x100000;
constexpr std::uint64_t gigabyte = 0x40000000;

/**
 * The five legal types, in the order the SDM's own encoding table gives
 * them. Used to paint the fixed ranges with something that changes from
 * sub-range to sub-range, so an off-by-one in the byte index cannot be
 * mistaken for a pass.
 */
constexpr zpp::arch::x86_64::memory_type legal_types[]{
    zpp::arch::x86_64::memory_type::uncachable,
    zpp::arch::x86_64::memory_type::write_combining,
    zpp::arch::x86_64::memory_type::write_through,
    zpp::arch::x86_64::memory_type::write_protected,
    zpp::arch::x86_64::memory_type::write_back,
};

/**
 * A state with the MTRRs on, the fixed ranges present and enabled, and
 * nothing programmed. Every builder below starts here.
 */
constexpr zpp::arch::x86_64::mtrr_state empty_state()
{
    zpp::arch::x86_64::mtrr_state state{};
    state.capabilities.fixed_range_registers_supported(true);
    state.capabilities.write_combining(true);
    state.capabilities.variable_range_register_count(10);
    state.default_type.type(zpp::arch::x86_64::memory_type::uncachable);
    state.default_type.fixed_range_enabled(true);
    state.default_type.enabled(true);
    return state;
}

/**
 * Programs one variable-range pair the way firmware does: a base in
 * IA32_MTRR_PHYSBASEn and a mask in IA32_MTRR_PHYSMASKn, decoded through
 * make_mtrr rather than written into the mtrr struct directly.
 *
 * Going through the registers is the point. A test that filled
 * mtrr::physical_base and mtrr::size by hand would never exercise the
 * encoding, and the encoding is where 63a5d17's masking defect lived.
 */
constexpr void add_variable(zpp::arch::x86_64::mtrr_state & state,
                            std::uint64_t base,
                            std::uint64_t size,
                            zpp::arch::x86_64::memory_type type)
{
    zpp::arch::x86_64::mtrr_variable_base physical_base{};
    physical_base.page_number(base >> 12);
    physical_base.memory_type(type);

    zpp::arch::x86_64::mtrr_variable_mask physical_mask{};
    physical_mask.valid(true);
    physical_mask.physical_mask((~(size - 1) & 0xffffffffff000ull) >> 12);

    state.variable[state.variable_count++] =
        zpp::arch::x86_64::make_mtrr(physical_base, physical_mask);
}

/**
 * Packs eight memory types into one fixed-range register. Bits 7:0
 * describe the lowest addressed sub-range, which is the direction
 * Table 14-9 reads.
 */
constexpr std::uint64_t
fixed_register(const zpp::arch::x86_64::memory_type (
    &types)[zpp::arch::x86_64::mtrr_fixed_range::sub_ranges])
{
    std::uint64_t value{};
    for (std::size_t i{};
         i < zpp::arch::x86_64::mtrr_fixed_range::sub_ranges;
         ++i) {
        value |= std::uint64_t(static_cast<unsigned>(types[i])) << (i * 8);
    }
    return value;
}

/**
 * Gives every sub-range of one fixed-range register the same type.
 */
constexpr std::uint64_t
fixed_register_of(zpp::arch::x86_64::memory_type type)
{
    std::uint64_t value{};
    for (std::size_t i{};
         i < zpp::arch::x86_64::mtrr_fixed_range::sub_ranges;
         ++i) {
        value |= std::uint64_t(static_cast<unsigned>(type)) << (i * 8);
    }
    return value;
}

/**
 * The type this harness expects at sub-range `sub` of fixed register
 * `reg` in the painted state below. Cycling through five types over
 * eighty-eight sub-ranges means no two adjacent sub-ranges agree and the
 * pattern does not repeat with any power of two, so a wrong shift or a
 * wrong register index lands on a different answer.
 */
constexpr zpp::arch::x86_64::memory_type painted_type(std::size_t reg,
                                                      std::size_t sub)
{
    return legal_types
        [((reg * zpp::arch::x86_64::mtrr_fixed_range::sub_ranges) + sub) %
         5];
}

constexpr zpp::arch::x86_64::mtrr_state painted_fixed_state()
{
    auto state = empty_state();

    // Write-back everywhere the variable ranges reach, so that anything
    // the fixed ranges are supposed to answer for cannot accidentally
    // agree with what would happen if they were skipped.
    state.default_type.type(zpp::arch::x86_64::memory_type::write_back);

    for (std::size_t reg{};
         reg < zpp::arch::x86_64::mtrr_state::fixed_range_count;
         ++reg) {
        zpp::arch::x86_64::memory_type
            types[zpp::arch::x86_64::mtrr_fixed_range::sub_ranges]{};
        for (std::size_t sub{};
             sub < zpp::arch::x86_64::mtrr_fixed_range::sub_ranges;
             ++sub) {
            types[sub] = painted_type(reg, sub);
        }
        state.fixed[reg] = fixed_register(types);
    }

    return state;
}

/**
 * Top of DRAM on the modelled machine. Everything above it up to 4 GB is
 * the MMIO hole, which no variable range covers - so it comes out as the
 * default type, and the default type is what firmware sets to UC.
 *
 * 2 MB aligned deliberately: a real top-of-DRAM is, and it makes the
 * split count below a statement about the legacy aperture alone rather
 * than about an artefact of the number chosen here.
 */
constexpr std::uint64_t top_of_dram = 0xa0000000;

/**
 * An MTRR set the shape firmware programs one: write-back over DRAM,
 * nothing at all over the MMIO hole, and the fixed ranges describing the
 * legacy megabyte with the 0xa0000-0x100000 aperture marked
 * uncacheable.
 *
 * This is 63a5d17's case. Before that commit the derivation saw neither
 * the default type nor the fixed ranges, so every address here that is
 * not inside a variable range - the whole MMIO hole included - was given
 * write-back.
 */
constexpr zpp::arch::x86_64::mtrr_state realistic_machine()
{
    auto state = empty_state();

    // Firmware's default. SDM Vol. 3A 14.11.2.1, .references/sdm.txt
    // 173872-173874: "Intel recommends the use of the UC (uncached)
    // memory type for all physical memory addresses where memory does
    // not exist."
    state.default_type.type(zpp::arch::x86_64::memory_type::uncachable);

    // 0x00000-0x80000 and 0x80000-0xa0000: conventional memory.
    state.fixed[0] =
        fixed_register_of(zpp::arch::x86_64::memory_type::write_back);
    state.fixed[1] =
        fixed_register_of(zpp::arch::x86_64::memory_type::write_back);

    // 0xa0000-0xc0000: the legacy VGA aperture.
    state.fixed[2] =
        fixed_register_of(zpp::arch::x86_64::memory_type::uncachable);

    // 0xc0000-0x100000: the option ROM and BIOS shadow region, which
    // this machine leaves uncacheable throughout.
    for (std::size_t i{3};
         i < zpp::arch::x86_64::mtrr_state::fixed_range_count;
         ++i) {
        state.fixed[i] =
            fixed_register_of(zpp::arch::x86_64::memory_type::uncachable);
    }

    // DRAM, as the two power-of-two ranges a real firmware needs to
    // describe 2.5 GB: a variable range's size must be a power of two
    // and its base aligned to it. SDM Vol. 3A 14.11.4,
    // .references/sdm.txt:174370-174375.
    add_variable(state,
                 0,
                 2 * gigabyte,
                 zpp::arch::x86_64::memory_type::write_back);
    add_variable(state,
                 2 * gigabyte,
                 gigabyte / 2,
                 zpp::arch::x86_64::memory_type::write_back);

    return state;
}

constexpr auto g_realistic = realistic_machine();

/*
 * The compile is the test for these. Stated here at namespace scope
 * rather than inside a function so a change that makes any of them
 * non-constant - or non-terminating - fails the build outright.
 */
static_assert(zpp::arch::x86_64::memory_type::write_back ==
              g_realistic.type_of(0));
static_assert(zpp::arch::x86_64::memory_type::write_back ==
              g_realistic.type_of(0x9ffff));
static_assert(zpp::arch::x86_64::memory_type::uncachable ==
              g_realistic.type_of(0xa0000));
static_assert(zpp::arch::x86_64::memory_type::uncachable ==
              g_realistic.type_of(0xfffff));
static_assert(zpp::arch::x86_64::memory_type::write_back ==
              g_realistic.type_of(megabyte));
static_assert(zpp::arch::x86_64::memory_type::write_back ==
              g_realistic.type_of(top_of_dram - 1));
static_assert(zpp::arch::x86_64::memory_type::uncachable ==
              g_realistic.type_of(top_of_dram));
static_assert(zpp::arch::x86_64::memory_type::uncachable ==
              g_realistic.type_of(0xfee00000));

/**
 * 83012e7, stated where it cannot be got wrong again.
 *
 * IA32_MTRRCAP.VCNT is an eight bit field, so 255 is the architectural
 * maximum a processor may report and the array must hold that many
 * whatever any particular machine says. SDM Vol. 3A 14.11.1,
 * .references/sdm.txt:173871: "VCNT (variable range registers count)
 * field, bits 0 through 7". The same field in SDM Vol. 4 Table 2-2,
 * .references/sdm.txt:225388: "7:0 VCNT: The number of variable memory
 * type ranges in the processor."
 */
constexpr zpp::arch::x86_64::mtrr_state g_probe{};
static_assert(255 <=
                  zpp::arch::x86_64::mtrr_state::maximum_variable_ranges,
              "the variable array must hold every count VCNT can "
              "report, not the count one machine happens to report - "
              "83012e7 sized it 8 where real parts say 10");
static_assert(255 <= std::size(g_probe.variable),
              "and the array itself, not merely the constant naming its "
              "size");

/**
 * The fixed-range table covers exactly the first megabyte, without a gap
 * and without an overlap. SDM Vol. 3A 14.11.2.2, .references/sdm.txt
 * 173942-173950.
 */
static_assert(0x100000 ==
              zpp::arch::x86_64::mtrr_state::fixed_range_limit);
static_assert(11 == std::size(zpp::arch::x86_64::mtrr_fixed_ranges));
static_assert(0 == zpp::arch::x86_64::mtrr_fixed_ranges[0].base);
static_assert(0x100000 == zpp::arch::x86_64::mtrr_fixed_ranges[10].end());

/**
 * A zero PhysMask with the valid bit set is declined, and the assertion
 * has to be made at compile time to mean what it says.
 *
 * make_mtrr derives the size by shifting the mask right until a one
 * falls out. A zero mask never terminates that loop, so the early return
 * in make_mtrr is load-bearing for *termination*, not only for
 * semantics - and a constant evaluation is the one context where a
 * non-terminating loop is reported rather than hung on. Firmware that
 * leaves a mask at zero with the valid bit set is not hypothetical: it
 * is what a partially initialised pair looks like.
 */
constexpr zpp::arch::x86_64::mtrr zero_mask_range()
{
    zpp::arch::x86_64::mtrr_variable_base base{};
    base.page_number(0x40000);
    base.memory_type(zpp::arch::x86_64::memory_type::write_back);

    zpp::arch::x86_64::mtrr_variable_mask mask{};
    mask.valid(true);
    mask.physical_mask(0);

    return zpp::arch::x86_64::make_mtrr(base, mask);
}

static_assert(!zero_mask_range().valid,
              "a zero PhysMask matches every address, which no firmware "
              "means - and the guard that declines it is also what makes "
              "make_mtrr's size loop terminate");

/**
 * The five types an MTRR and an EPT entry may name, and the reserved
 * values that must not reach an EPT entry.
 *
 * SDM Vol. 3A 14.11.2.1, .references/sdm.txt:173924: "The legal values
 * for this field are 0, 1, 4, 5, and 6. All other values result in a
 * general-protection exception (#GP) being generated."
 */
void legal_memory_types()
{
    check(zpp::arch::x86_64::is_valid(
              zpp::arch::x86_64::memory_type::uncachable),
          "UC is a legal type");
    check(zpp::arch::x86_64::is_valid(
              zpp::arch::x86_64::memory_type::write_combining),
          "WC is a legal type");
    check(zpp::arch::x86_64::is_valid(
              zpp::arch::x86_64::memory_type::write_through),
          "WT is a legal type");
    check(zpp::arch::x86_64::is_valid(
              zpp::arch::x86_64::memory_type::write_protected),
          "WP is a legal type");
    check(zpp::arch::x86_64::is_valid(
              zpp::arch::x86_64::memory_type::write_back),
          "WB is a legal type");

    for (auto value : {2, 3, 7, 8, 0xff}) {
        check(!zpp::arch::x86_64::is_valid(
                  zpp::arch::x86_64::memory_type(value)),
              "type " + std::to_string(value) +
                  " is reserved, and writing it into an EPT entry is an "
                  "EPT misconfiguration on the first access");
    }

    // A reserved value cannot legitimately be in an MTRR - WRMSR faults
    // on one - so this only fires on firmware that broke the rule. It is
    // handled anyway because the alternative is an unhandled exit that
    // stops the CPU.
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               zpp::arch::x86_64::mtrr_state::valid_or_uncachable(
                   zpp::arch::x86_64::memory_type(3)),
               "a reserved type is answered as uncacheable");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               zpp::arch::x86_64::mtrr_state::valid_or_uncachable(
                   zpp::arch::x86_64::memory_type::write_back),
               "a legal type is answered unchanged");

    // And through the whole derivation, from each of the three places a
    // type can enter it: a variable range, a fixed range, and the
    // default.
    auto variable = empty_state();
    variable.default_type.fixed_range_enabled(false);
    add_variable(
        variable, 0, large_page_size, zpp::arch::x86_64::memory_type(3));
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               variable.type_of(0x1000),
               "a reserved type in a variable range comes out UC");

    auto fixed = empty_state();
    fixed.fixed[0] = fixed_register_of(zpp::arch::x86_64::memory_type(7));
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               fixed.type_of(0x1000),
               "a reserved type in a fixed range comes out UC");

    auto def = empty_state();
    def.default_type.fixed_range_enabled(false);
    def.default_type.type(zpp::arch::x86_64::memory_type(2));
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               def.type_of(4 * gigabyte),
               "a reserved default type comes out UC");
}

/**
 * The register accessors, each against the bit the SDM puts it in.
 *
 * Worth stating separately from the derivation because a wrong bit here
 * is silent: fixed_range_enabled reading bit 11 instead of bit 10 would
 * make a normally programmed machine still look plausible.
 *
 * SDM Vol. 3A 14.11.2.1, .references/sdm.txt:173929 and 173933 for the
 * two enables; 14.11.1, .references/sdm.txt:173871-173882 for the
 * capability bits.
 */
void register_encodings()
{
    zpp::arch::x86_64::mtrr_capabilities capabilities{};
    capabilities.variable_range_register_count(10);
    check_equal(
        10, capabilities.variable_range_register_count(), "VCNT of 10");
    check_equal(10, capabilities.value(), "VCNT lives in bits 7:0");

    capabilities.variable_range_register_count(255);
    check_equal(255,
                capabilities.variable_range_register_count(),
                "VCNT holds its architectural maximum");

    capabilities.fixed_range_registers_supported(true);
    check_equal(0x1ff, capabilities.value(), "FIX is bit 8");
    capabilities.write_combining(true);
    check_equal(0x5ff, capabilities.value(), "WC is bit 10");
    capabilities.system_management_range_register(true);
    check_equal(0xdff, capabilities.value(), "SMRR is bit 11");
    check(capabilities.fixed_range_registers_supported() &&
              capabilities.write_combining() &&
              capabilities.system_management_range_register(),
          "and all three read back");

    zpp::arch::x86_64::mtrr_default_type default_type{};
    default_type.type(zpp::arch::x86_64::memory_type::write_back);
    check_equal(6, default_type.value(), "the default type is bits 7:0");
    default_type.fixed_range_enabled(true);
    check_equal(0x406, default_type.value(), "FE is bit 10");
    default_type.enabled(true);
    check_equal(0xc06, default_type.value(), "E is bit 11");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               default_type.type(),
               "the type field survives both enables being set");

    // The two enables are independent, which matters because
    // fixed_ranges_in_use() is a conjunction of one of them with a
    // capability bit.
    default_type.fixed_range_enabled(false);
    check(default_type.enabled() && !default_type.fixed_range_enabled(),
          "clearing FE leaves E alone");

    zpp::arch::x86_64::mtrr_variable_base base{};
    base.page_number(0xdeadb);
    base.memory_type(zpp::arch::x86_64::memory_type::write_combining);
    check_equal(0xdeadb, base.page_number(), "PhysBase is bits 51:12");
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               base.memory_type(),
               "the type is bits 7:0 of PHYSBASE");
    check_equal(0xdeadb001, base.value(), "and the two do not collide");

    zpp::arch::x86_64::mtrr_variable_mask mask{};
    mask.physical_mask(0xfffff80000);
    check_equal(
        0xfffff80000, mask.physical_mask(), "PhysMask is bits 51:12");
    check(!mask.valid(), "the valid bit starts clear");
    mask.valid(true);
    check(mask.valid(), "V is bit 11");
    check_equal(0xfffff80000800,
                mask.value(),
                "and V does not collide with PhysMask");
}

/**
 * Rule 1: below 1 MB the fixed ranges win, and every sub-range of every
 * one of the eleven registers is read from the right byte.
 *
 * SDM Vol. 3A 14.11.4.1, .references/sdm.txt:174380: "If the physical
 * address falls within the first 1 MByte of physical memory and fixed
 * MTRRs are enabled, the processor uses the memory type stored for the
 * appropriate fixed-range MTRR." And 14.11.2.1, .references/sdm.txt
 * 173929-173931: "When the fixed-range MTRRs are enabled, they take
 * priority over the variable-range MTRRs when overlaps in ranges occur."
 */
void fixed_ranges_take_precedence()
{
    // The table itself: contiguous, in address order, ending exactly at
    // the limit the derivation uses.
    check_equal(zpp::arch::x86_64::mtrr_state::fixed_range_count,
                std::size(zpp::arch::x86_64::mtrr_fixed_ranges),
                "eleven fixed-range registers");
    check_equal(0,
                zpp::arch::x86_64::mtrr_fixed_ranges[0].base,
                "the table starts at address zero");
    check_equal(zpp::arch::x86_64::mtrr_state::fixed_range_limit,
                zpp::arch::x86_64::mtrr_fixed_ranges
                    [std::size(zpp::arch::x86_64::mtrr_fixed_ranges) - 1]
                        .end(),
                "and stops at exactly 1 MB - the limit type_of compares "
                "against, so a gap between the two would be governed by "
                "neither");

    for (std::size_t i{1};
         i < std::size(zpp::arch::x86_64::mtrr_fixed_ranges);
         ++i) {
        check_equal(zpp::arch::x86_64::mtrr_fixed_ranges[i - 1].end(),
                    zpp::arch::x86_64::mtrr_fixed_ranges[i].base,
                    "fixed range " + std::to_string(i) + " begins where " +
                        std::to_string(i - 1) + " ends, with no gap");
    }

    // The three groups and their sizes, from Table 14-9. Cited at
    // .references/sdm.txt:173942 (64 KB), 173945 (16 KB) and 173948
    // (4 KB).
    check_equal(0x10000,
                zpp::arch::x86_64::mtrr_fixed_ranges[0].sub_range_size,
                "FIX64K_00000 maps eight 64 KB sub-ranges");
    check_equal(zpp::arch::x86_64::msr::mtrr::fix64k_00000,
                zpp::arch::x86_64::mtrr_fixed_ranges[0].msr,
                "and it is MSR 0x250");
    check_equal(0x80000,
                zpp::arch::x86_64::mtrr_fixed_ranges[0].end(),
                "so it covers 0 to 0x7ffff");
    check_equal(0x4000,
                zpp::arch::x86_64::mtrr_fixed_ranges[1].sub_range_size,
                "FIX16K_80000 maps eight 16 KB sub-ranges");
    check_equal(0x80000,
                zpp::arch::x86_64::mtrr_fixed_ranges[1].base,
                "from 0x80000");
    check_equal(0xa0000,
                zpp::arch::x86_64::mtrr_fixed_ranges[2].base,
                "FIX16K_A0000 covers the legacy VGA aperture, which "
                "firmware marks UC");
    check_equal(0xc0000,
                zpp::arch::x86_64::mtrr_fixed_ranges[2].end(),
                "up to 0xc0000");
    check_equal(0x1000,
                zpp::arch::x86_64::mtrr_fixed_ranges[3].sub_range_size,
                "the eight FIX4K registers map 4 KB sub-ranges");
    check_equal(0xc0000,
                zpp::arch::x86_64::mtrr_fixed_ranges[3].base,
                "starting at 0xc0000, the option ROM region");
    check_equal(zpp::arch::x86_64::msr::mtrr::fix4k_f8000,
                zpp::arch::x86_64::mtrr_fixed_ranges[10].msr,
                "and the last of them is MSR 0x26f");

    // Every sub-range of every register, at both ends of the sub-range,
    // with a variable range deliberately covering all of it in a
    // different type. Anything that consulted the variable range, or
    // took the wrong byte out of the register, disagrees here.
    auto state = painted_fixed_state();
    add_variable(state,
                 0,
                 4 * megabyte,
                 zpp::arch::x86_64::memory_type::write_combining);

    for (std::size_t reg{};
         reg < zpp::arch::x86_64::mtrr_state::fixed_range_count;
         ++reg) {
        const auto & range = zpp::arch::x86_64::mtrr_fixed_ranges[reg];
        for (std::size_t sub{};
             sub < zpp::arch::x86_64::mtrr_fixed_range::sub_ranges;
             ++sub) {
            auto first = range.base + (sub * range.sub_range_size);
            auto last = first + range.sub_range_size - 1;
            auto expected = painted_type(reg, sub);
            auto where = "fixed register " + std::to_string(reg) +
                         " sub-range " + std::to_string(sub);

            check_type(expected, state.type_of(first), where + " begins");
            check_type(expected, state.type_of(last), where + " ends");

            // The next place the answer can change is the end of this
            // sub-range and nothing sooner, which is what makes a range
            // query cost the number of MTRRs rather than the number of
            // pages.
            check_equal(first + range.sub_range_size,
                        state.next_boundary_after(first),
                        where + "'s boundary is its own end");
        }
    }

    // The three group boundaries, stated separately because they are
    // where a table written with the wrong sub-range size still passes
    // everywhere else.
    check_type(painted_type(0, 7),
               state.type_of(0x7ffff),
               "0x7ffff is the last 64 KB sub-range");
    check_type(painted_type(1, 0),
               state.type_of(0x80000),
               "0x80000 is the first 16 KB sub-range");
    check_type(painted_type(2, 0),
               state.type_of(0xa0000),
               "0xa0000 opens the second 16 KB register");
    check_type(painted_type(3, 0),
               state.type_of(0xc0000),
               "0xc0000 opens the first 4 KB register");
    check_type(painted_type(10, 7),
               state.type_of(0xfffff),
               "0xfffff is the last 4 KB sub-range");

    // And exactly where they stop. One byte past the table the variable
    // range governs, which is the WC one added above rather than
    // anything the fixed registers say.
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               state.type_of(megabyte),
               "at 1 MB the fixed ranges stop and the variable ranges "
               "take over");
    check_equal(megabyte,
                state.next_boundary_after(0xfffff),
                "and 1 MB is the boundary the last sub-range reports, so "
                "a walk needs no separate step for it");
}

/**
 * The two enables, each with the other left alone.
 *
 * SDM Vol. 3A 14.11.4.1, .references/sdm.txt:174377: "If the MTRRs are
 * not enabled (by setting the E flag in the IA32_MTRR_DEF_TYPE MSR),
 * then all memory accesses are of the UC memory type."
 *
 * And 14.11.2.1, .references/sdm.txt:173931-173932: "If the fixed-range
 * MTRRs are disabled, the variable-range MTRRs can still be used and can
 * map the range ordinarily covered by the fixed-range MTRRs."
 */
void the_two_enables()
{
    // E clear. Everything says write-back and the answer is still UC.
    auto disabled = empty_state();
    disabled.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    for (auto & value : disabled.fixed) {
        value =
            fixed_register_of(zpp::arch::x86_64::memory_type::write_back);
    }
    add_variable(disabled,
                 0,
                 4 * gigabyte,
                 zpp::arch::x86_64::memory_type::write_back);
    disabled.default_type.enabled(false);

    for (auto address : {std::uint64_t{},
                         std::uint64_t{0xa0000},
                         megabyte,
                         gigabyte,
                         top_of_dram,
                         std::uint64_t{1} << 40}) {
        check_type(zpp::arch::x86_64::memory_type::uncachable,
                   disabled.type_of(address),
                   "with E clear every address is UC, including 0x" +
                       std::to_string(address));
    }

    // One type for all of physical memory means there is no boundary at
    // all - and that is what lets uniform_type_of answer a 512 GB
    // question in a single step instead of walking it.
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                disabled.next_boundary_after(0),
                "with E clear there is no boundary above zero");
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                disabled.next_boundary_after(top_of_dram),
                "nor above anything else");
    check_optional_type(
        zpp::arch::x86_64::memory_type::uncachable,
        disabled.uniform_type_of(0, std::uint64_t{512} * gigabyte),
        "and a 512 GB range is uniformly UC, in one step");

    // FE clear. The fixed registers still say UC and the region below
    // 1 MB is governed by the variable ranges and the default type.
    auto fixed_off = empty_state();
    fixed_off.default_type.type(
        zpp::arch::x86_64::memory_type::write_back);
    for (auto & value : fixed_off.fixed) {
        value =
            fixed_register_of(zpp::arch::x86_64::memory_type::uncachable);
    }
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               fixed_off.type_of(0x1000),
               "with FE set the fixed registers govern below 1 MB");

    fixed_off.default_type.fixed_range_enabled(false);
    check_type(zpp::arch::x86_64::memory_type::write_back,
               fixed_off.type_of(0x1000),
               "with FE clear the default type governs below 1 MB");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               fixed_off.type_of(0xa0000),
               "including the legacy aperture the fixed registers "
               "marked UC");

    add_variable(fixed_off,
                 0,
                 0x80000,
                 zpp::arch::x86_64::memory_type::write_combining);
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               fixed_off.type_of(0x1000),
               "and a variable range may map the region the fixed "
               "registers ordinarily cover");

    // 1 MB is not a boundary with FE clear: the variable ranges and the
    // default govern either side of it alike, so only their own edges
    // matter. A walk that inserted 1 MB unconditionally would split the
    // first 2 MB region of every such machine for nothing.
    check_equal(0x80000,
                fixed_off.next_boundary_after(0x1000),
                "the only boundary below 1 MB here is the variable "
                "range's own end");
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                fixed_off.next_boundary_after(0x80000),
                "and 1 MB itself is not a boundary when FE is clear");

    // The capability bit and the enable are a conjunction, and both
    // halves are needed: FIX says the registers exist at all - reading
    // an MSR a processor does not implement raises #GP - and FE says
    // firmware turned them on. SDM Example 14-5, Get4KMemType(),
    // .references/sdm.txt:174468: "IF IA32_MTRRCAP.FIX AND
    // MTRRdefType.FE".
    auto unsupported = empty_state();
    unsupported.default_type.type(
        zpp::arch::x86_64::memory_type::write_back);
    for (auto & value : unsupported.fixed) {
        value =
            fixed_register_of(zpp::arch::x86_64::memory_type::uncachable);
    }
    check(unsupported.fixed_ranges_in_use(),
          "FIX set and FE set means the fixed ranges are in use");

    unsupported.capabilities.fixed_range_registers_supported(false);
    check(!unsupported.fixed_ranges_in_use(),
          "FIX clear means they are not, whatever FE says");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               unsupported.type_of(0x1000),
               "so their contents are not consulted");
}

/**
 * Rule 2, the overlap precedence, one clause at a time and in both
 * orders.
 *
 * The order matters more than it looks. mtrr.h's own comment records
 * that KVM tests only the range it has just reached, so a UC range
 * followed by a WB range comes out WB there; the rule as the SDM writes
 * it does not depend on the order the ranges are visited in, and this
 * implementation follows the rule. Every case below is therefore
 * asserted with the ranges added both ways round.
 *
 * SDM Vol. 3A 14.11.4.1, .references/sdm.txt:
 *   174384  "If one variable memory range matches, the processor uses
 *            the memory type stored in the IA32_MTRR_PHYSBASEn register
 *            for that range."
 *   174386  "If two or more variable memory ranges match and the memory
 *            types are identical, then that memory type is used."
 *   174388  "If two or more variable memory ranges match and one of the
 *            memory types is UC, the UC memory type used."
 *   174390  "If two or more variable memory ranges match and the memory
 *            types are WT and WB, the WT memory type is used."
 *   174392  "For overlaps not defined by the above rules, processor
 *            behavior is undefined."
 *   174393  "If no fixed or variable memory range matches, the processor
 *            uses the default memory type."
 *
 * Note the SDM's own Get4KMemType() pseudocode, .references/sdm.txt
 * 174467-174481, does *not* implement this: it returns the type of the
 * first matching variable range and never looks at a second. The prose
 * is normative and the pseudocode is a simplification. mtrr.h says so,
 * and that difference is what 63a5d17 fixed.
 */
void overlap_precedence()
{
    // Two ranges over the same 2 MB, in the given order, with the fixed
    // ranges out of the way and a default nothing else uses so an
    // unmatched address is obvious.
    auto overlap = [](zpp::arch::x86_64::memory_type first,
                      zpp::arch::x86_64::memory_type second) {
        auto state = empty_state();
        state.default_type.fixed_range_enabled(false);
        state.default_type.type(
            zpp::arch::x86_64::memory_type::write_protected);
        add_variable(state, gigabyte, large_page_size, first);
        add_variable(state, gigabyte, large_page_size, second);
        return state.type_of(gigabyte + 0x1000);
    };

    // One match.
    auto single = empty_state();
    single.default_type.fixed_range_enabled(false);
    add_variable(single,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_combining);
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               single.type_of(gigabyte),
               "one matching range answers with its own type");

    // Identical types.
    for (auto type : legal_types) {
        check_type(type,
                   overlap(type, type),
                   std::string("two ranges of ") + name_of(type) +
                       " answer " + name_of(type));
    }

    // UC wins, from either side, against every other type.
    for (auto type : legal_types) {
        if (zpp::arch::x86_64::memory_type::uncachable == type) {
            continue;
        }
        check_type(
            zpp::arch::x86_64::memory_type::uncachable,
            overlap(zpp::arch::x86_64::memory_type::uncachable, type),
            std::string("UC then ") + name_of(type) + " is UC");
        check_type(
            zpp::arch::x86_64::memory_type::uncachable,
            overlap(type, zpp::arch::x86_64::memory_type::uncachable),
            std::string(name_of(type)) +
                " then UC is UC - this is the clause KVM's "
                "implementation is order-dependent on");
    }

    // WT and WB give WT, either way round.
    check_type(zpp::arch::x86_64::memory_type::write_through,
               overlap(zpp::arch::x86_64::memory_type::write_through,
                       zpp::arch::x86_64::memory_type::write_back),
               "WT then WB is WT");
    check_type(zpp::arch::x86_64::memory_type::write_through,
               overlap(zpp::arch::x86_64::memory_type::write_back,
                       zpp::arch::x86_64::memory_type::write_through),
               "WB then WT is WT");

    // Everything else is an overlap the SDM leaves undefined, and this
    // implementation answers uncacheable. mtrr.h states why, and it is
    // worth quoting rather than paraphrasing because the choice is
    // deliberately *not* KVM's:
    //
    //   "KVM answers write-back here, with a comment saying so. This
    //    answers uncacheable instead, because the two mistakes are not
    //    symmetric under EPT: the guest cannot weaken a type this VMM
    //    handed it, so a wrongly cacheable device range corrupts
    //    silently while a wrongly uncacheable one is only slow. No
    //    firmware programs an overlap of this shape - it is undefined on
    //    hardware too - so neither answer is expected to be used."
    //
    // So these assertions pin a decision, not an architectural fact. If
    // they ever have to change, the thing to change with them is that
    // comment.
    for (auto first : legal_types) {
        for (auto second : legal_types) {
            if (first == second) {
                continue;
            }
            if (zpp::arch::x86_64::memory_type::uncachable == first ||
                zpp::arch::x86_64::memory_type::uncachable == second) {
                continue;
            }

            auto write_through_or_back =
                [](zpp::arch::x86_64::memory_type type) {
                    return zpp::arch::x86_64::memory_type::write_through ==
                               type ||
                           zpp::arch::x86_64::memory_type::write_back ==
                               type;
                };
            if (write_through_or_back(first) &&
                write_through_or_back(second)) {
                continue;
            }

            check_type(zpp::arch::x86_64::memory_type::uncachable,
                       overlap(first, second),
                       std::string("the undefined overlap ") +
                           name_of(first) + " with " + name_of(second) +
                           " is answered UC, deliberately and not as "
                           "KVM answers it");
        }
    }

    // Three ranges, where the UC one is neither first nor last. The
    // combination is carried across the whole loop rather than being a
    // pairwise decision, so a UC anywhere in the set wins.
    auto three = empty_state();
    three.default_type.fixed_range_enabled(false);
    add_variable(three,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_through);
    add_variable(three,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::uncachable);
    add_variable(three,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_back);
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               three.type_of(gigabyte),
               "UC in the middle of three matching ranges still wins");

    // WT accumulated from a WT/WB pair, then met by a third WB, stays
    // WT rather than becoming an undefined overlap.
    auto accumulate = empty_state();
    accumulate.default_type.fixed_range_enabled(false);
    add_variable(accumulate,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_back);
    add_variable(accumulate,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_through);
    add_variable(accumulate,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_back);
    check_type(zpp::arch::x86_64::memory_type::write_through,
               accumulate.type_of(gigabyte),
               "WB, WT and WB together are WT");

    // Rule 3: no match at all.
    auto unmatched = empty_state();
    unmatched.default_type.fixed_range_enabled(false);
    unmatched.default_type.type(
        zpp::arch::x86_64::memory_type::write_combining);
    add_variable(unmatched,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_back);
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               unmatched.type_of(2 * gigabyte),
               "an address no range matches gets the default type");
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               unmatched.type_of(gigabyte - 1),
               "including one byte below a range");
    check_type(zpp::arch::x86_64::memory_type::write_combining,
               unmatched.type_of(gigabyte + large_page_size),
               "and one byte past its end");

    // A pair whose valid bit is clear takes no part in any of it.
    auto invalid = empty_state();
    invalid.default_type.fixed_range_enabled(false);
    invalid.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    zpp::arch::x86_64::mtrr_variable_base base{};
    base.page_number(gigabyte >> 12);
    base.memory_type(zpp::arch::x86_64::memory_type::uncachable);
    zpp::arch::x86_64::mtrr_variable_mask mask{};
    mask.physical_mask((~(large_page_size - 1) & 0xffffffffff000ull) >>
                       12);
    invalid.variable[invalid.variable_count++] =
        zpp::arch::x86_64::make_mtrr(base, mask);
    check(!invalid.variable[0].valid,
          "a pair with V clear decodes as invalid");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               invalid.type_of(gigabyte),
               "and is skipped by the derivation");
}

/**
 * The variable-range encoding: PhysBase masked by PhysMask, the size
 * derived from the mask, and the zero mask declined.
 *
 * SDM Vol. 3A 14.11.2.3, .references/sdm.txt:173985: "Address_Within_-
 * Range AND PhysMask = PhysBase AND PhysMask". The bits of PhysBase the
 * mask clears take no part in that comparison, so the range begins at
 * PhysBase AND PhysMask and not at PhysBase - which is what 63a5d17
 * fixed, and what makes firmware that leaves a base unaligned to its own
 * size still name the aligned range.
 */
void variable_range_encoding()
{
    // A 2 MB range whose base register holds 0x40001000: one page above
    // the aligned base. Hardware matches every address in
    // [0x40000000, 0x40200000) because the low bits are masked out of
    // the comparison, so the derivation must name that range and not
    // [0x40001000, 0x40201000).
    zpp::arch::x86_64::mtrr_variable_base base{};
    base.page_number(0x40001000 >> 12);
    base.memory_type(zpp::arch::x86_64::memory_type::uncachable);

    zpp::arch::x86_64::mtrr_variable_mask mask{};
    mask.valid(true);
    mask.physical_mask((~(large_page_size - 1) & 0xffffffffff000ull) >>
                       12);

    auto range = zpp::arch::x86_64::make_mtrr(base, mask);
    check(range.valid, "a mask with V set decodes as valid");
    check_equal(0x40000000,
                range.physical_base,
                "the range starts at PhysBase AND PhysMask, not at "
                "PhysBase - 63a5d17");
    check_equal(large_page_size,
                range.size,
                "and its size is the run of zeroes below the mask");
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               range.type,
               "the type comes from the base register");

    auto state = empty_state();
    state.default_type.fixed_range_enabled(false);
    state.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    state.variable[state.variable_count++] = range;

    // Both ends, and both ends of what the unmasked reading would have
    // produced. An implementation that started at PhysBase answers WB at
    // the first of these and UC at the last.
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               state.type_of(0x40000000),
               "the aligned base is inside the range");
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               state.type_of(0x401fffff),
               "and so is its last byte");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               state.type_of(0x3fffffff),
               "one byte below is outside");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               state.type_of(0x40200000),
               "and so is the first byte past the aligned end - an "
               "unmasked base would have run 0x1000 further");

    // The size, over every power of two a mask can express from 4 KB to
    // 4 GB. A full mask names a single page; each zero below the run
    // doubles it.
    for (std::size_t shift{12}; shift <= 32; ++shift) {
        auto size = std::uint64_t{1} << shift;
        zpp::arch::x86_64::mtrr_variable_base sized_base{};
        sized_base.page_number(0);
        sized_base.memory_type(zpp::arch::x86_64::memory_type::write_back);

        zpp::arch::x86_64::mtrr_variable_mask sized_mask{};
        sized_mask.valid(true);
        sized_mask.physical_mask((~(size - 1) & 0xffffffffff000ull) >> 12);

        check_equal(
            size,
            zpp::arch::x86_64::make_mtrr(sized_base, sized_mask).size,
            "a mask with " + std::to_string(shift - 12) +
                " zeroes below its run names a range of 0x" +
                std::to_string(size) + " bytes");
    }

    // A zero PhysMask with V set. The match rule is satisfied for every
    // address by such a mask, so honouring it would name the whole
    // physical address space - which no firmware means, because the
    // default type is what says something about every uncovered
    // address. It is declined rather than honoured, and mtrr.h records
    // the second reason: the range used to be left valid with a size of
    // zero, and a zero-size range still matched any address at or above
    // its base under a comparison written as (address >= base + size).
    zpp::arch::x86_64::mtrr_variable_base wide_base{};
    wide_base.page_number(0x40000);
    wide_base.memory_type(zpp::arch::x86_64::memory_type::uncachable);

    zpp::arch::x86_64::mtrr_variable_mask zero_mask{};
    zero_mask.valid(true);
    zero_mask.physical_mask(0);

    auto declined = zpp::arch::x86_64::make_mtrr(wide_base, zero_mask);
    check(!declined.valid, "a zero PhysMask is declined, not honoured");
    check_equal(0, declined.size, "and names nothing");

    auto with_declined = empty_state();
    with_declined.default_type.fixed_range_enabled(false);
    with_declined.default_type.type(
        zpp::arch::x86_64::memory_type::write_back);
    with_declined.variable[with_declined.variable_count++] = declined;
    for (auto address : {std::uint64_t{},
                         std::uint64_t{0x40000000},
                         std::uint64_t{0x40001000},
                         std::uint64_t{1} << 40}) {
        check_type(zpp::arch::x86_64::memory_type::write_back,
                   with_declined.type_of(address),
                   "a declined range matches nothing, at or above its "
                   "base alike");
    }
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                with_declined.next_boundary_after(0),
                "and contributes no boundary");
}

/**
 * next_boundary_after is strictly greater than its argument, for every
 * input, in every shape of state this harness can build.
 *
 * This is the one property uniform_type_of's loop depends on for
 * termination. mtrr.h's own comment on the guard that would catch a
 * violation says what the alternative is: "an unbounded loop during EPT
 * construction on the boot processor, which is indistinguishable from a
 * dead machine". The guard costs nothing and stays; this is the check
 * that says it is never needed.
 */
void boundaries_are_strictly_increasing()
{
    auto sweep = [](const zpp::arch::x86_64::mtrr_state & state,
                    const char * what) {
        // Dense across the fixed ranges, where the sub-range sizes are
        // 4 KB and an off-by-one is expressible.
        for (std::uint64_t address{}; address < 0x110000;
             address += page_size) {
            auto next = state.next_boundary_after(address);
            check(next > address,
                  std::string(what) + ": boundary above 0x" +
                      std::to_string(address) + " is strictly greater");
        }

        // Sparse across 4 GB, and over each variable range's own edges
        // one byte either side - which is where an implementation that
        // wrote >= instead of > would return the address it was given.
        for (std::uint64_t address{}; address < 4 * gigabyte;
             address += 8 * megabyte) {
            check(state.next_boundary_after(address) > address,
                  std::string(what) + ": boundary above 0x" +
                      std::to_string(address) + " is strictly greater");
        }

        for (std::size_t i{}; i < state.variable_count; ++i) {
            const auto & range = state.variable[i];
            for (auto address : {range.physical_base,
                                 range.physical_base + range.size,
                                 range.physical_base + range.size - 1}) {
                check(state.next_boundary_after(address) > address,
                      std::string(what) +
                          ": boundary above a range edge at 0x" +
                          std::to_string(address) +
                          " is strictly greater");
            }
        }
    };

    sweep(realistic_machine(), "realistic machine");

    auto disabled = realistic_machine();
    disabled.default_type.enabled(false);
    sweep(disabled, "MTRRs disabled");

    auto fixed_off = realistic_machine();
    fixed_off.default_type.fixed_range_enabled(false);
    sweep(fixed_off, "fixed ranges off");

    auto painted = painted_fixed_state();
    sweep(painted, "painted fixed ranges, no variable ranges");

    // A pathological set: many ranges, overlapping, unaligned bases,
    // touching the top of the address space, plus a declined pair.
    auto pathological = empty_state();
    pathological.default_type.type(
        zpp::arch::x86_64::memory_type::write_back);
    add_variable(pathological,
                 0,
                 4 * gigabyte,
                 zpp::arch::x86_64::memory_type::write_back);
    add_variable(pathological,
                 0,
                 page_size,
                 zpp::arch::x86_64::memory_type::uncachable);
    add_variable(pathological,
                 0xfee00000,
                 page_size,
                 zpp::arch::x86_64::memory_type::uncachable);
    add_variable(pathological,
                 gigabyte + page_size,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::write_through);
    zpp::arch::x86_64::mtrr_variable_base top_base{};
    top_base.page_number(0xffffffffff);
    top_base.memory_type(zpp::arch::x86_64::memory_type::uncachable);
    zpp::arch::x86_64::mtrr_variable_mask top_mask{};
    top_mask.valid(true);
    top_mask.physical_mask(0xffffffffff);
    pathological.variable[pathological.variable_count++] =
        zpp::arch::x86_64::make_mtrr(top_base, top_mask);
    sweep(pathological, "pathological range set");

    // A range at the very top of the physical address space, where a
    // base-plus-size comparison would wrap. mtrr.h writes the membership
    // test as a subtraction for exactly this reason.
    auto top = pathological.variable[pathological.variable_count - 1];
    check_equal(0xffffffffff000,
                top.physical_base,
                "a full mask names a single page at the top of the "
                "52 bit physical address space");
    check_equal(page_size, top.size, "of 4 KB");
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               pathological.type_of(0xffffffffff000),
               "and that page is matched without the end wrapping");

    // Nothing above it can change the answer, so there is no boundary.
    check_equal(
        zpp::arch::x86_64::mtrr_state::no_boundary,
        pathological.next_boundary_after(0xffffffffff000 + page_size),
        "past the highest range end there is no boundary");
}

/**
 * The boundaries the realistic machine reports, named one at a time.
 */
void boundaries_of_a_real_machine()
{
    auto state = realistic_machine();

    check_equal(0x10000,
                state.next_boundary_after(0),
                "inside the fixed ranges the sub-range end is the next "
                "boundary");
    check_equal(0x10000,
                state.next_boundary_after(0xffff),
                "and it does not move within a sub-range");
    check_equal(0x84000,
                state.next_boundary_after(0x80000),
                "16 KB sub-ranges from 0x80000");
    check_equal(0xa4000,
                state.next_boundary_after(0xa0000),
                "and across the aperture");
    check_equal(0xc1000,
                state.next_boundary_after(0xc0000),
                "4 KB sub-ranges from 0xc0000");
    check_equal(megabyte,
                state.next_boundary_after(0xff000),
                "the last sub-range ends at exactly 1 MB, where the "
                "variable ranges take over");

    // Above 1 MB only the variable ranges' own edges matter. The first
    // DRAM range ends where the second begins, so 2 GB appears once as
    // an end and once as a base and the lower of the two candidates is
    // what is reported.
    check_equal(2 * gigabyte,
                state.next_boundary_after(megabyte),
                "the next boundary above 1 MB is the end of the first "
                "DRAM range");
    check_equal(2 * gigabyte,
                state.next_boundary_after(2 * gigabyte - 1),
                "one byte below it");
    check_equal(top_of_dram,
                state.next_boundary_after(2 * gigabyte),
                "then the top of DRAM");
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                state.next_boundary_after(top_of_dram),
                "above which nothing can change the answer - the MMIO "
                "hole and everything past it is the default type");
    check_equal(zpp::arch::x86_64::mtrr_state::no_boundary,
                state.next_boundary_after(std::uint64_t{1} << 40),
                "including a terabyte up");
}

/**
 * uniform_type_of: the question the EPT builder actually asks.
 *
 * Nothing is not a failure - it is the answer that says a large page
 * cannot describe the range, and costs a split into 4 KB entries.
 */
void uniform_ranges()
{
    // One UC region of exactly 2 MB, 2 MB aligned, in a WB world. Every
    // interesting alignment is expressible against it.
    auto state = empty_state();
    state.default_type.fixed_range_enabled(false);
    state.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    add_variable(state,
                 gigabyte,
                 large_page_size,
                 zpp::arch::x86_64::memory_type::uncachable);

    check_optional_type(
        zpp::arch::x86_64::memory_type::write_back,
        state.uniform_type_of(gigabyte - large_page_size, large_page_size),
        "a 2 MB region ending exactly at the boundary is uniform");
    check_optional_type(
        zpp::arch::x86_64::memory_type::uncachable,
        state.uniform_type_of(gigabyte, large_page_size),
        "a 2 MB region starting exactly at the boundary is uniform");
    check_optional_type(
        zpp::arch::x86_64::memory_type::write_back,
        state.uniform_type_of(gigabyte + large_page_size, large_page_size),
        "and the region after it is uniform again");

    check_optional_type(
        {},
        state.uniform_type_of(gigabyte - megabyte, large_page_size),
        "a 2 MB region straddling the boundary reports nothing, which "
        "is what tells the EPT builder to split to 4 KB");
    check_optional_type(
        {},
        state.uniform_type_of(gigabyte + megabyte, large_page_size),
        "and so does one straddling the far edge");

    // The exact boundary cases at both ends, at page granularity.
    check_optional_type(
        zpp::arch::x86_64::memory_type::write_back,
        state.uniform_type_of(gigabyte - page_size, page_size),
        "the last page below the boundary is WB");
    check_optional_type(zpp::arch::x86_64::memory_type::uncachable,
                        state.uniform_type_of(gigabyte, page_size),
                        "the first page at the boundary is UC");
    check_optional_type(
        {},
        state.uniform_type_of(gigabyte - page_size, 2 * page_size),
        "two pages spanning the boundary are not uniform");
    check_optional_type(
        zpp::arch::x86_64::memory_type::uncachable,
        state.uniform_type_of(gigabyte + large_page_size - page_size,
                              page_size),
        "the last page of the range is UC");
    check_optional_type(
        {},
        state.uniform_type_of(gigabyte + large_page_size - page_size,
                              2 * page_size),
        "and one page further is not uniform");

    // A whole 512 GB question over a machine with one type: the walk
    // must terminate on the no_boundary answer rather than stepping.
    auto flat = empty_state();
    flat.default_type.fixed_range_enabled(false);
    flat.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    check_optional_type(
        zpp::arch::x86_64::memory_type::write_back,
        flat.uniform_type_of(0, std::uint64_t{512} * gigabyte),
        "with no MTRRs at all a 512 GB range is uniformly the default "
        "type");

    // A sub-2 MB range inside the fixed ranges, where the granularity
    // is 4 KB and the fixed registers disagree with each other.
    auto painted = painted_fixed_state();
    check_optional_type(painted_type(3, 0),
                        painted.uniform_type_of(0xc0000, page_size),
                        "a single 4 KB fixed sub-range is uniform");
    check_optional_type(
        {},
        painted.uniform_type_of(0xc0000, 2 * page_size),
        "two adjacent 4 KB sub-ranges painted differently are not");
    check_optional_type(
        {},
        painted.uniform_type_of(0, large_page_size),
        "and the first 2 MB of a painted machine is not uniform");
}

/**
 * uniform_type_of against a naive per-page walk, over the machine the
 * EPT builder runs on.
 *
 * The two must agree exactly, in both directions. The direction that
 * matters is "reports nothing": the fast path could report nothing for
 * a reason that is not a change of type - the defensive `next <=
 * address` guard is one such reason - and the EPT builder would still
 * produce a correct table, just a needlessly split one. Only a
 * differential against a walk that cannot be wrong distinguishes the
 * two.
 */
void uniform_agrees_with_a_naive_walk()
{
    auto state = realistic_machine();

    auto naive = [&](std::uint64_t address, std::uint64_t size)
        -> std::optional<zpp::arch::x86_64::memory_type> {
        auto type = state.type_of(address);
        for (std::uint64_t offset{}; offset < size; offset += page_size) {
            if (state.type_of(address + offset) != type) {
                return {};
            }
        }
        return type;
    };

    // The first 64 MB, which contains every fixed-range boundary, and
    // the four 2 MB regions either side of each variable-range edge.
    auto compare = [&](std::uint64_t address) {
        check_optional_type(
            naive(address, large_page_size),
            state.uniform_type_of(address, large_page_size),
            "uniform_type_of agrees with a per-page "
            "walk over the 2 MB region at 0x" +
                std::to_string(address));
    };

    for (std::uint64_t address{}; address < 64 * megabyte;
         address += large_page_size) {
        compare(address);
    }

    for (auto edge : {2 * gigabyte, top_of_dram}) {
        for (int i{-2}; i < 2; ++i) {
            compare(edge + (std::uint64_t)(i * (int)large_page_size));
        }
    }
}

/**
 * The whole point, stated as the EPT builder states it.
 *
 * hypervisor.cpp walks 2 MB regions, asks uniform_type_of for each, and
 * splits the region into 512 4 KB entries when the answer is nothing.
 * That loop is reproduced here over the first 4 GB, and what is asserted
 * is both halves of 63a5d17: the MMIO hole above the top of DRAM comes
 * out uncacheable, and exactly one region needs splitting.
 *
 * The split count is worth pinning because the pool of 4 KB tables is
 * statically sized. hypervisor.cpp bounds it at 2 * VCNT + 1 regions,
 * and records the measurement it was written against: "VCNT is 10 on
 * the machine this was written for, where one region is mixed: the
 * first, holding the legacy 0xa0000 aperture."
 */
void the_ept_builder_splits_where_expected()
{
    auto state = realistic_machine();

    std::size_t mixed{};
    std::size_t first_mixed{};
    std::size_t uncacheable_regions{};

    for (std::uint64_t region{}; region < (4 * gigabyte) / large_page_size;
         ++region) {
        auto address = region * large_page_size;
        auto type = state.uniform_type_of(address, large_page_size);
        if (!type) {
            if (!mixed) {
                first_mixed = region;
            }
            ++mixed;
            continue;
        }
        if (zpp::arch::x86_64::memory_type::uncachable == *type) {
            ++uncacheable_regions;
        }
    }

    check_equal(1,
                mixed,
                "exactly one 2 MB region of the first 4 GB needs "
                "splitting to 4 KB");
    check_equal(0,
                first_mixed,
                "and it is the first, which is the one holding the "
                "legacy 0xa0000 aperture");

    // Everything from the top of DRAM to 4 GB, and nothing below it.
    check_equal((4 * gigabyte - top_of_dram) / large_page_size,
                uncacheable_regions,
                "every 2 MB region of the MMIO hole is uniformly "
                "uncacheable - before 63a5d17 every one of them was "
                "given write-back, which under EPT is cached MMIO the "
                "guest cannot correct");

    // The hole's ends, named rather than counted.
    check_optional_type(
        zpp::arch::x86_64::memory_type::write_back,
        state.uniform_type_of(top_of_dram - large_page_size,
                              large_page_size),
        "the last 2 MB of DRAM is write-back");
    check_optional_type(
        zpp::arch::x86_64::memory_type::uncachable,
        state.uniform_type_of(top_of_dram, large_page_size),
        "the first 2 MB of the MMIO hole is uncacheable");
    check_type(zpp::arch::x86_64::memory_type::uncachable,
               state.type_of(0xfee00000),
               "and so is the local APIC page, which sits in it");

    // The split region, entry by entry, the way hypervisor.cpp fills a
    // 4 KB table. The legacy aperture is the reason the region is split
    // at all, so the assertion is that the aperture and only the
    // aperture comes out uncacheable.
    std::size_t uncacheable_pages{};
    for (std::size_t k{}; k < 512; ++k) {
        auto address = k * page_size;
        auto type = state.type_of(address);
        if (zpp::arch::x86_64::memory_type::uncachable == type) {
            ++uncacheable_pages;
            check(address >= 0xa0000 && address < megabyte,
                  "the only uncacheable pages of the first 2 MB are in "
                  "the legacy region, not at 0x" +
                      std::to_string(address));
        }
    }
    check_equal((megabyte - 0xa0000) / page_size,
                uncacheable_pages,
                "0xa0000 to 1 MB is uncacheable, 96 pages of it");
    check_type(zpp::arch::x86_64::memory_type::write_back,
               state.type_of(0),
               "and the rest of the first 2 MB is write-back DRAM a "
               "guest runs code out of - which is why the region is "
               "split rather than given one conservative type");
}

/**
 * A machine reporting VCNT of 10, filled to the count, and a machine
 * reporting the architectural maximum.
 *
 * 83012e7 again, from the direction the fill loop sees it: the array
 * must hold whatever VCNT says, and every entry up to variable_count
 * must be read.
 */
void every_variable_range_is_consulted()
{
    auto state = empty_state();
    state.default_type.fixed_range_enabled(false);
    state.default_type.type(zpp::arch::x86_64::memory_type::write_back);

    // Ten ranges, which is what a real Intel client part reports and
    // what overran an array sized 8. The last two are the ones that were
    // dropped, so they are the ones asserted about.
    for (std::size_t i{}; i < 10; ++i) {
        add_variable(state,
                     gigabyte + (i * large_page_size),
                     large_page_size,
                     zpp::arch::x86_64::memory_type::uncachable);
    }
    check_equal(10, state.variable_count, "ten variable ranges");

    for (std::size_t i{}; i < 10; ++i) {
        check_type(zpp::arch::x86_64::memory_type::uncachable,
                   state.type_of(gigabyte + (i * large_page_size)),
                   "range " + std::to_string(i) +
                       " of ten is consulted - the ninth and tenth are "
                       "what an array sized 8 dropped");
    }

    // And the architectural maximum, every entry distinct, so a loop
    // that stopped early or an index that wrapped is visible.
    auto full = empty_state();
    full.default_type.fixed_range_enabled(false);
    full.default_type.type(zpp::arch::x86_64::memory_type::write_back);
    for (std::size_t i{};
         i < zpp::arch::x86_64::mtrr_state::maximum_variable_ranges;
         ++i) {
        add_variable(full,
                     gigabyte + (i * large_page_size),
                     large_page_size,
                     zpp::arch::x86_64::memory_type::uncachable);
    }
    check_equal(zpp::arch::x86_64::mtrr_state::maximum_variable_ranges,
                full.variable_count,
                "the array holds every range VCNT can report");
    check_type(
        zpp::arch::x86_64::memory_type::uncachable,
        full.type_of(
            gigabyte +
            ((zpp::arch::x86_64::mtrr_state::maximum_variable_ranges - 1) *
             large_page_size)),
        "and the last of them is consulted");
    check_type(
        zpp::arch::x86_64::memory_type::write_back,
        full.type_of(
            gigabyte +
            (zpp::arch::x86_64::mtrr_state::maximum_variable_ranges *
             large_page_size)),
        "with nothing past it");

    // The capabilities member is what the overrun corrupted, by sitting
    // immediately after the array. Nothing can prove a layout from here,
    // but the count the state was built with must survive filling the
    // array to its maximum.
    check_equal(10,
                full.capabilities.variable_range_register_count(),
                "filling the array to its maximum does not disturb the "
                "capabilities that follow it - 83012e7 measured them "
                "exactly 8 * sizeof(mtrr) apart");
}

} // namespace

int main()
{
    legal_memory_types();
    register_encodings();
    fixed_ranges_take_precedence();
    the_two_enables();
    overlap_precedence();
    variable_range_encoding();
    boundaries_are_strictly_increasing();
    boundaries_of_a_real_machine();
    uniform_ranges();
    uniform_agrees_with_a_naive_walk();
    the_ept_builder_splits_where_expected();
    every_variable_range_is_consulted();

    std::println("mtrr: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
