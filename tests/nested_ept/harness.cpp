/*
 * The nested extended page table rules: `ept_permissions`,
 * `misconfigured`, `walk_ept`, `compose_ept` and
 * `reflected_ept_violation_qualification`.
 *
 * `zpp/arch/x86_64/vmx/nested_ept.h` depends on `ept.h` and
 * `memory_type.h` and on nothing else, so the header under test compiles
 * here exactly as the hypervisor compiles it. Every function in it is
 * `constexpr` and pure - `walk_ept` reaches memory only through a callback
 * - which is what makes this the one part of the nested machinery that can
 * be exercised exhaustively with no processor, no emulator and no shadow
 * pool.
 *
 * **Why this file exists.** Everything nested rests on these five
 * functions, and until now the only thing that ever called them was a
 * second-level guest fault on real hardware. A wrong answer there is not a
 * crash: it is a shadow entry that grants slightly the wrong thing, and
 * the symptom is a guest that livelocks on one address a million exits
 * later. `check-nested-ept.sh` asserted a handful of `static_assert`s over
 * the same header; this replaces reading the rules with running them.
 *
 * Two tiers, and both are deliberate:
 *
 *   - `static_assert`, where the case is a constant expression. The
 *     compile is then the test, and it cannot be skipped by a run that
 *     stops early.
 *   - the same case again at run time, so a failure names itself. A
 *     `static_assert` that fires says only that some assertion in this
 *     file is wrong.
 *
 * Nothing here is stubbed, shimmed or copied. The one thing constructed
 * for the test is the *memory* the walk reads, which is what the callback
 * parameter exists for.
 */
#include "zpp/arch/x86_64/vmx/nested_ept.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>

namespace
{

/**
 * The report.
 *
 * Counted rather than aborted on, so one wrong rule does not hide the
 * other forty: every case runs and the exit status is the verdict.
 * @{
 */
std::size_t g_checks{};
std::size_t g_failures{};

void check(const char * name, std::uint64_t expected, std::uint64_t actual)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }

    ++g_failures;
    std::println(
        "FAIL {}: expected {:#x}, got {:#x}", name, expected, actual);
}

void check_true(const char * name, bool value)
{
    check(name, 1, value ? 1 : 0);
}

void check_false(const char * name, bool value)
{
    check(name, 0, value ? 1 : 0);
}
/**
 * @}
 */

/**
 * The four permission bits, as an index, so a case can sweep every
 * combination rather than naming three of them.
 *
 * Bit 0 is read, bit 1 write, bit 2 execute and bit 3 user-execute, which
 * is the order SDM Tables 31-1 through 31-7 give them in - bits 2:0 of an
 * entry plus bit 10 for the user half under mode-based execute control.
 * @{
 */
constexpr unsigned permission_combinations = 16;
constexpr unsigned permission_read = 1u << 0;
constexpr unsigned permission_write = 1u << 1;
constexpr unsigned permission_execute = 1u << 2;
constexpr unsigned permission_execute_user = 1u << 3;

constexpr zpp::arch::x86_64::vmx::ept_permissions
permissions_of_index(unsigned index)
{
    return zpp::arch::x86_64::vmx::ept_permissions(
        0 != (index & permission_read),
        0 != (index & permission_write),
        0 != (index & permission_execute),
        0 != (index & permission_execute_user));
}

constexpr unsigned index_of_permissions(
    const zpp::arch::x86_64::vmx::ept_permissions & permissions)
{
    return (permissions.read() ? permission_read : 0u) |
           (permissions.write() ? permission_write : 0u) |
           (permissions.execute() ? permission_execute : 0u) |
           (permissions.execute_user() ? permission_execute_user : 0u);
}
/**
 * @}
 */

/**
 * Whether the processor offers execute-only translations, as the two
 * settings this header takes it as.
 *
 * IA32_VMX_EPT_VPID_CAP bit 0 (SDM A.10), and the reason both settings are
 * swept rather than one assumed: it is the single input that changes what
 * `normalised` is allowed to keep, and a rule tested under one setting
 * only is a rule half tested.
 * @{
 */
constexpr bool execute_only_offered = true;
constexpr bool execute_only_not_offered = false;
/**
 * @}
 */

/**
 * The physical-address width these cases judge reserved bits against.
 *
 * Thirty-six is the architectural floor for physical addressing and is
 * what `hypervisor::physical_address_bits` falls back to when CPUID
 * reports nothing, so it is the value that makes the reserved-bit cases
 * reproducible without a processor. A second, wider width is used where a
 * case is about the boundary moving rather than about a fixed bit.
 * @{
 */
constexpr std::uint64_t narrow_physical_address_bits = 36;
constexpr std::uint64_t wide_physical_address_bits = 48;
/**
 * @}
 */

/**
 * The levels of a 4-level walk, named rather than written as numbers, so a
 * case reads as the structure it is about.
 *
 * SDM 31.3.2: the PML4E is selected by bits 47:39, the PDPTE by 38:30, the
 * PDE by 29:21 and the PTE by 20:12. `walk_ept` counts the leaf as level
 * 0, so these run the other way from the table names.
 * @{
 */
constexpr std::uint64_t level_page_table = 0;         // 4 KB leaf.
constexpr std::uint64_t level_page_directory = 1;     // 2 MB leaf.
constexpr std::uint64_t level_page_directory_ptr = 2; // 1 GB leaf.
constexpr std::uint64_t level_page_map_level_4 = 3;
/**
 * @}
 */

/**
 * The page shifts, from the same section.
 * @{
 */
constexpr std::uint64_t shift_4kb = 12;
constexpr std::uint64_t shift_2mb = 21;
constexpr std::uint64_t shift_1gb = 30;
/**
 * @}
 */

constexpr std::size_t entries_per_table = 512;
constexpr std::uint64_t entry_bytes = sizeof(std::uint64_t);

/**
 * Memory for the walk to read, addressed the way the processor addresses
 * it.
 *
 * A walk names an entry by physical address, so what a test has to supply
 * is a map from physical address to entry - which is exactly the callback
 * `walk_ept` takes. Tables are handed out at made-up but plausible
 * addresses: 4 KB aligned, and low enough to sit under every
 * physical-address width these cases use.
 *
 * `unreadable` is the fifth case the walk has to answer and the only one
 * that cannot be expressed as an entry value: a caller that cannot reach
 * guest memory returns an empty optional, and the walk must report that as
 * not present rather than as whatever was last in the accumulator.
 */
class table_space
{
public:
    static constexpr std::size_t table_capacity = 8;

    /**
     * Where the first table is pretended to live. One megabyte, so that
     * every table address is 4 KB aligned, non-zero - a zero root would
     * make "the walk read from the root" indistinguishable from "the walk
     * read from nowhere" - and far below the narrow width above.
     */
    static constexpr std::uint64_t base_physical = 0x100000;

    std::uint64_t allocate()
    {
        auto index = m_used;
        ++m_used;
        return base_physical + (index * (entries_per_table * entry_bytes));
    }

    zpp::arch::x86_64::vmx::epte & at(std::uint64_t table_physical,
                                      std::uint64_t index)
    {
        auto table = (table_physical - base_physical) /
                     (entries_per_table * entry_bytes);
        return m_tables[table][index];
    }

    /**
     * Refuses every read of the table at this address, standing in for
     * guest memory the VMM cannot reach.
     */
    void refuse_reads_of(std::uint64_t table_physical)
    {
        m_refused = table_physical;
    }

    std::optional<zpp::arch::x86_64::vmx::epte>
    read(std::uint64_t physical) const
    {
        auto offset = physical - base_physical;
        auto table = offset / (entries_per_table * entry_bytes);
        auto index =
            (offset % (entries_per_table * entry_bytes)) / entry_bytes;

        if (table >= table_capacity) {
            return std::nullopt;
        }

        if (m_refused ==
            (base_physical + (table * entries_per_table * entry_bytes))) {
            return std::nullopt;
        }

        return m_tables[table][index];
    }

private:
    zpp::arch::x86_64::vmx::epte m_tables[table_capacity]
                                         [entries_per_table]{};
    std::size_t m_used{};
    std::uint64_t m_refused{~std::uint64_t{}};
};

/**
 * An entry that references the next table down, granting everything.
 *
 * The memory type is deliberately left zero: SDM Table 31-6 reserves bits
 * 6:3 in an entry that references another table, so a type set here would
 * be a misconfiguration rather than a memory type - which is a case of its
 * own below rather than an accident in every other case.
 */
constexpr zpp::arch::x86_64::vmx::epte
table_entry(std::uint64_t child_physical,
            const zpp::arch::x86_64::vmx::ept_permissions & permissions =
                zpp::arch::x86_64::vmx::ept_permissions::all())
{
    zpp::arch::x86_64::vmx::epte entry;
    permissions.apply_to(entry);
    entry.page_number(child_physical >> shift_4kb);
    return entry;
}

/**
 * An entry that maps a page of the size its level implies.
 */
constexpr zpp::arch::x86_64::vmx::epte
leaf_entry(std::uint64_t page_physical,
           std::uint64_t shift,
           const zpp::arch::x86_64::vmx::ept_permissions & permissions =
               zpp::arch::x86_64::vmx::ept_permissions::all(),
           zpp::arch::x86_64::memory_type type =
               zpp::arch::x86_64::memory_type::write_back)
{
    zpp::arch::x86_64::vmx::epte entry;
    permissions.apply_to(entry);
    entry.type(type);

    if (shift_4kb != shift) {
        entry.large(true);
    }

    // Masked to the page size rather than trusted, for the reason
    // `install_shadow_leaf` gives: the low bits of a large entry's address
    // field are reserved, and a set reserved bit is a misconfiguration
    // rather than a wrong address.
    return zpp::arch::x86_64::vmx::epte(
        entry.value() | (page_physical & ~((1ull << shift) - 1)));
}

/**
 * A walk result standing for a mapping, built directly rather than walked.
 *
 * `compose_ept` takes two walk results and reaches no memory, so composing
 * is tested against constructed results - which is the only way to reach
 * the combinations a real pair of tables would take a whole machine to
 * produce.
 */
constexpr zpp::arch::x86_64::vmx::ept_walk_result
mapped_result(std::uint64_t physical_address,
              std::uint64_t page_shift,
              const zpp::arch::x86_64::vmx::ept_permissions & permissions,
              zpp::arch::x86_64::memory_type type =
                  zpp::arch::x86_64::memory_type::write_back)
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = zpp::arch::x86_64::vmx::ept_walk_status::mapped;
    result.physical_address = physical_address;
    result.page_shift = page_shift;
    result.permissions = permissions;
    result.type = type;
    return result;
}

constexpr zpp::arch::x86_64::vmx::ept_walk_result
failed_result(zpp::arch::x86_64::vmx::ept_walk_status status)
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = status;
    result.permissions = zpp::arch::x86_64::vmx::ept_permissions();
    return result;
}

// === ept_permissions
// =====================================================
//
// The intersection is what composing two levels *is*, and `normalised` is
// what stops the intersection producing a combination the processor
// rejects. Both are swept exhaustively rather than spot checked: there are
// sixteen permission sets and two settings of execute-only support, so the
// whole input space is 512 pairs and there is no reason to sample it.

/**
 * The intersection is the logical-AND of each bit, and nothing else.
 *
 * Stated as an identity over the index encoding so it can be a
 * `static_assert` over the whole space rather than four spot checks.
 */
constexpr bool intersection_is_bitwise_and()
{
    for (unsigned left{}; left < permission_combinations; ++left) {
        for (unsigned right{}; right < permission_combinations; ++right) {
            auto combined = permissions_of_index(left).intersected_with(
                permissions_of_index(right));
            if (index_of_permissions(combined) != (left & right)) {
                return false;
            }
        }
    }

    return true;
}

static_assert(intersection_is_bitwise_and(),
              "the intersection of two permission sets must be the "
              "logical-AND of each bit");

/**
 * `normalised` only ever removes.
 *
 * This is the safety property the header states in prose - "Dropping
 * permissions rather than adding read is the only safe direction. Adding
 * read would grant an access neither side granted" - and it is checkable
 * over the whole space: the result's bits must be a subset of the input's.
 */
constexpr bool normalised_never_adds()
{
    for (unsigned index{}; index < permission_combinations; ++index) {
        auto before = permissions_of_index(index);

        for (auto offered :
             {execute_only_not_offered, execute_only_offered}) {
            auto after = index_of_permissions(before.normalised(offered));
            if (after != (after & index)) {
                return false;
            }
        }
    }

    return true;
}

static_assert(normalised_never_adds(),
              "normalising permissions must never grant one that was not "
              "already there");

/**
 * `normalised` is idempotent: normalising a normalised set changes
 * nothing. If it were not, the composition would depend on how many times
 * it happened to be applied.
 */
constexpr bool normalised_is_idempotent()
{
    for (unsigned index{}; index < permission_combinations; ++index) {
        for (auto offered :
             {execute_only_not_offered, execute_only_offered}) {
            auto once = permissions_of_index(index).normalised(offered);
            if (once.normalised(offered) != once) {
                return false;
            }
        }
    }

    return true;
}

static_assert(normalised_is_idempotent(),
              "normalising an already normalised permission set must "
              "change nothing");

/**
 * The output of `normalised` is a combination the processor accepts, which
 * is what `ept_walk::misconfigured` decides. Stated against that function
 * rather than restated here, so the two cannot drift apart.
 */
constexpr bool normalised_output_is_never_misconfigured()
{
    for (unsigned index{}; index < permission_combinations; ++index) {
        for (auto offered :
             {execute_only_not_offered, execute_only_offered}) {
            auto permissions =
                permissions_of_index(index).normalised(offered);

            zpp::arch::x86_64::vmx::epte entry;
            permissions.apply_to(entry);
            entry.type(zpp::arch::x86_64::memory_type::write_back);

            if (zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    entry,
                    level_page_table,
                    true,
                    narrow_physical_address_bits,
                    offered)) {
                return false;
            }
        }
    }

    return true;
}

static_assert(normalised_output_is_never_misconfigured(),
              "a normalised permission set written into a leaf must not "
              "be a misconfiguration");

void test_permissions()
{
    check_true(
        "permissions.all_grants_everything",
        zpp::arch::x86_64::vmx::ept_permissions::all().read() &&
            zpp::arch::x86_64::vmx::ept_permissions::all().write() &&
            zpp::arch::x86_64::vmx::ept_permissions::all().execute() &&
            zpp::arch::x86_64::vmx::ept_permissions::all().execute_user());

    check_true("permissions.all_is_present",
               zpp::arch::x86_64::vmx::ept_permissions::all().present());

    check_false("permissions.default_is_absent",
                zpp::arch::x86_64::vmx::ept_permissions().present());

    // SDM 31.3.2: "An EPT paging-structure entry is present if any of bits
    // 2:0 is 1; otherwise, the entry is not present", with the note that
    // follows: "If the 'mode-based execute control for EPT' VM-execution
    // control is 1, an EPT paging-structure entry is present if any of
    // bits 2:0 or bit 10 is 1."
    //
    // **The condition is load-bearing and this test used to drop it.** It
    // read the note as "mode-based execute control adds bit 10" and then
    // concluded "every non-empty set is present", which is true only when
    // that control is 1. It never is here - MBEC is not in
    // `supported_secondary_controls`, so the guest hypervisor cannot ask
    // for it and vmcs02 runs with it clear. The old assertion therefore
    // pinned the wrong rule, and `ept_permissions::present` matched it
    // while contradicting its own SDM quotation.
    //
    // What that cost: an entry in the guest hypervisor's EPT with bits
    // 2:0 clear and bit 10 set is NOT PRESENT to the processor - an
    // ordinary EPT violation - but was present-and-misconfigured here,
    // which synthesises exit reason 49 into vmcs12 and tells Hyper-V its
    // own paging structures are corrupt. KVM agrees with the processor:
    // `FNAME(is_present_gpte)` for `PTTYPE_EPT` is `pte & 7`.
    //
    // So: present iff any of bits 2:0. Bit 10 alone is index 8, and that
    // one case is the whole point of the loop.
    constexpr unsigned execute_user_bit = 8;
    for (unsigned index{}; index < permission_combinations; ++index) {
        auto permissions = permissions_of_index(index);
        auto expected = (0 != (index & ~execute_user_bit));
        if (permissions.present() != expected) {
            // The index cannot be the sentinel, so this can only report a
            // failure - a failure path whose two arguments could ever be
            // equal is a failure that passes.
            check("permissions.present_is_any_bit_set", ~0ull, index);
        }
    }
    check_true("permissions.present_is_any_bit_set", true);

    // The case the loop above exists for, pinned by name so a regression
    // says what it broke rather than printing an index.
    check_false(
        "permissions.execute_user_alone_is_absent_without_mbec",
        permissions_of_index(execute_user_bit).present());

    // `of` and `apply_to` are the two ends of the same conversion, so a
    // round trip through an entry has to be the identity. A drift here
    // would put a permission into a shadow leaf that the composition never
    // computed.
    for (unsigned index{}; index < permission_combinations; ++index) {
        zpp::arch::x86_64::vmx::epte entry;
        permissions_of_index(index).apply_to(entry);

        if (index_of_permissions(
                zpp::arch::x86_64::vmx::ept_permissions::of(entry)) !=
            index) {
            check("permissions.entry_round_trip", index, ~0ull);
        }
    }
    check_true("permissions.entry_round_trip", true);

    check_true("permissions.intersection_is_bitwise_and",
               intersection_is_bitwise_and());
    check_true("permissions.normalised_never_adds",
               normalised_never_adds());
    check_true("permissions.normalised_is_idempotent",
               normalised_is_idempotent());
    check_true("permissions.normalised_output_is_accepted",
               normalised_output_is_never_misconfigured());

    // The three named combinations the header's prose is about.
    //
    // Write without read is a misconfiguration on every processor - SDM
    // 31.3.3.1, "Bit 0 of the entry is clear ... and ... Bit 1 is set" -
    // so it is dropped whether or not execute-only is offered.
    auto write_only =
        zpp::arch::x86_64::vmx::ept_permissions(false, true, false, false);

    check_false("permissions.write_without_read_dropped_without_xo",
                write_only.normalised(execute_only_not_offered).present());
    check_false("permissions.write_without_read_dropped_with_xo",
                write_only.normalised(execute_only_offered).present());

    // Execute without read is legal only where the processor reports
    // execute-only translations.
    auto execute_only =
        zpp::arch::x86_64::vmx::ept_permissions(false, false, true, false);

    check_false(
        "permissions.execute_without_read_dropped_without_xo",
        execute_only.normalised(execute_only_not_offered).present());
    check_true("permissions.execute_without_read_kept_with_xo",
               execute_only.normalised(execute_only_offered).execute());

    // And the user half of execute, which mode-based execute control
    // splits out as bit 10, follows the same rule.
    auto execute_user_only =
        zpp::arch::x86_64::vmx::ept_permissions(false, false, false, true);

    check_false(
        "permissions.execute_user_without_read_dropped_without_xo",
        execute_user_only.normalised(execute_only_not_offered).present());
    check_true(
        "permissions.execute_user_without_read_kept_with_xo",
        execute_user_only.normalised(execute_only_offered).execute_user());

    // A read keeps everything, which is the early return in `normalised`
    // and the case that must not be disturbed by the two above.
    check_true("permissions.read_keeps_everything",
               zpp::arch::x86_64::vmx::ept_permissions::all().normalised(
                   execute_only_not_offered) ==
                   zpp::arch::x86_64::vmx::ept_permissions::all());

    // The intersection that produces write-without-read out of two
    // perfectly legal sets, which is the whole reason `normalised` is
    // called on the result rather than trusted from the inputs: read-only
    // intersected with write-and-execute leaves nothing, and
    // read-and-write intersected with execute-and-write leaves write
    // alone.
    auto read_and_write =
        zpp::arch::x86_64::vmx::ept_permissions(true, true, false, false);
    auto write_and_execute =
        zpp::arch::x86_64::vmx::ept_permissions(false, true, true, false);

    check_true("permissions.intersection_can_produce_write_only",
               read_and_write.intersected_with(write_and_execute) ==
                   write_only);
    check_false("permissions.that_intersection_normalises_away",
                read_and_write.intersected_with(write_and_execute)
                    .normalised(execute_only_not_offered)
                    .present());
}

// === misconfigured
// =======================================================
//
// SDM 31.3.3.1's list, condition by condition. Every one of these produces
// an exit taken on every attempt for ever if it reaches a shadow table,
// and the exit carries no qualification to say why - SDM 30.2.1 does not
// list EPT misconfiguration among the exits that save one.

void test_misconfiguration()
{
    // The reserved bits of an entry that references another table differ
    // by level: SDM Table 31-2 reserves bits 7:3 of a PML4E, Table 31-4
    // the same of a PDPTE referencing a page directory, and Table 31-6
    // bits 6:3 of a PDE referencing a page table - bit 7 there being the
    // discriminator rather than reserved.
    constexpr std::uint64_t bit_3 = 1ull << 3;
    constexpr std::uint64_t bit_6 = 1ull << 6;
    constexpr std::uint64_t bit_7 = 1ull << 7;

    auto reference = table_entry(table_space::base_physical);

    check_true("misconfigured.pml4e_bit3_reserved",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   zpp::arch::x86_64::vmx::epte(reference.value() | bit_3),
                   level_page_map_level_4,
                   false,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    check_true("misconfigured.pdpte_reference_bit3_reserved",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   zpp::arch::x86_64::vmx::epte(reference.value() | bit_3),
                   level_page_directory_ptr,
                   false,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    check_true("misconfigured.pde_reference_bit6_reserved",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   zpp::arch::x86_64::vmx::epte(reference.value() | bit_6),
                   level_page_directory,
                   false,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    // Bit 7 of a page-directory entry is what says whether it maps a 2 MB
    // page, so it is not reserved there. A walker that used the wider mask
    // at every level would call every large mapping a misconfiguration,
    // which is a failure that only shows up on a guest hypervisor whose
    // tables use large pages - which is all of them.
    check_false(
        "misconfigured.pde_bit7_is_the_discriminator_not_reserved",
        zpp::arch::x86_64::vmx::ept_walk::misconfigured(
            zpp::arch::x86_64::vmx::epte(reference.value() | bit_7),
            level_page_directory,
            false,
            narrow_physical_address_bits,
            execute_only_not_offered));

    // But bit 7 *is* reserved in a PML4E - SDM Table 31-2 - which is the
    // architectural statement that a PML4E is never a leaf.
    check_true("misconfigured.pml4e_bit7_reserved",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   zpp::arch::x86_64::vmx::epte(reference.value() | bit_7),
                   level_page_map_level_4,
                   false,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    // A leaf's low address bits, which must be zero for the page size the
    // level implies: SDM Table 31-3 reserves bits 29:12 of a 1 GB PDPTE
    // and Table 31-5 bits 20:12 of a 2 MB PDE.
    constexpr std::uint64_t bit_12 = 1ull << 12;
    constexpr std::uint64_t bit_20 = 1ull << 20;

    auto gigabyte_leaf = leaf_entry(0, shift_1gb);
    auto large_leaf = leaf_entry(0, shift_2mb);
    auto small_leaf = leaf_entry(0, shift_4kb);

    check_true(
        "misconfigured.gigabyte_leaf_bit12_reserved",
        zpp::arch::x86_64::vmx::ept_walk::misconfigured(
            zpp::arch::x86_64::vmx::epte(gigabyte_leaf.value() | bit_12),
            level_page_directory_ptr,
            true,
            narrow_physical_address_bits,
            execute_only_not_offered));

    check_true(
        "misconfigured.gigabyte_leaf_bit20_reserved",
        zpp::arch::x86_64::vmx::ept_walk::misconfigured(
            zpp::arch::x86_64::vmx::epte(gigabyte_leaf.value() | bit_20),
            level_page_directory_ptr,
            true,
            narrow_physical_address_bits,
            execute_only_not_offered));

    check_true(
        "misconfigured.large_leaf_bit12_reserved",
        zpp::arch::x86_64::vmx::ept_walk::misconfigured(
            zpp::arch::x86_64::vmx::epte(large_leaf.value() | bit_12),
            level_page_directory,
            true,
            narrow_physical_address_bits,
            execute_only_not_offered));

    // Bit 12 of a 4 KB leaf is the lowest bit of the address, so nothing
    // is reserved there. This is the other side of the two above, and it
    // is what says the reserved mask is derived from the level rather than
    // applied uniformly.
    check_false(
        "misconfigured.small_leaf_bit12_is_address",
        zpp::arch::x86_64::vmx::ept_walk::misconfigured(
            zpp::arch::x86_64::vmx::epte(small_leaf.value() | bit_12),
            level_page_table,
            true,
            narrow_physical_address_bits,
            execute_only_not_offered));

    // The memory type of a leaf. SDM 31.3.7.2: "0 = UC; 1 = WC; 4 = WT;
    // 5 = WP; and 6 = WB. Other values are reserved and cause EPT
    // misconfigurations". Two, three and seven are the reserved encodings
    // that fit in the three-bit field.
    constexpr std::uint64_t memory_type_shift = 3;
    constexpr std::uint64_t reserved_memory_types[]{2, 3, 7};

    for (auto type : reserved_memory_types) {
        auto entry = zpp::arch::x86_64::vmx::epte(
            zpp::arch::x86_64::vmx::ept_permissions::all().read()
                ? (leaf_entry(
                       0,
                       shift_4kb,
                       zpp::arch::x86_64::vmx::ept_permissions::all(),
                       zpp::arch::x86_64::memory_type::uncachable)
                       .value() |
                   (type << memory_type_shift))
                : 0);

        if (!zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                entry,
                level_page_table,
                true,
                narrow_physical_address_bits,
                execute_only_not_offered)) {
            check("misconfigured.reserved_memory_type", type, 0);
        }
    }
    check_true("misconfigured.reserved_memory_type", true);

    // And the five the architecture defines, none of which may be refused.
    constexpr zpp::arch::x86_64::memory_type accepted_memory_types[]{
        zpp::arch::x86_64::memory_type::uncachable,
        zpp::arch::x86_64::memory_type::write_combining,
        zpp::arch::x86_64::memory_type::write_through,
        zpp::arch::x86_64::memory_type::write_protected,
        zpp::arch::x86_64::memory_type::write_back,
    };

    for (auto type : accepted_memory_types) {
        if (zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                leaf_entry(0,
                           shift_4kb,
                           zpp::arch::x86_64::vmx::ept_permissions::all(),
                           type),
                level_page_table,
                true,
                narrow_physical_address_bits,
                execute_only_not_offered)) {
            check("misconfigured.accepted_memory_type",
                  static_cast<std::uint64_t>(type),
                  ~0ull);
        }
    }
    check_true("misconfigured.accepted_memory_type", true);

    // The address width. SDM 31.3.3.1 includes "the setting of a bit in
    // the range 51:12 in position MAXPHYADDR or above" among the
    // misconfiguration conditions, so the boundary is where the answer
    // changes and both sides of it are the case.
    auto at_the_boundary =
        leaf_entry(1ull << narrow_physical_address_bits, shift_4kb);
    auto below_the_boundary =
        leaf_entry(1ull << (narrow_physical_address_bits - 1), shift_4kb);

    check_true("misconfigured.address_bit_at_maxphyaddr",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   at_the_boundary,
                   level_page_table,
                   true,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    check_false("misconfigured.address_bit_below_maxphyaddr",
                zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    below_the_boundary,
                    level_page_table,
                    true,
                    narrow_physical_address_bits,
                    execute_only_not_offered));

    // The same entry judged against a wider processor is not
    // misconfigured, which is what says the width is an input rather than
    // a constant.
    check_false("misconfigured.same_bit_accepted_by_a_wider_processor",
                zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    at_the_boundary,
                    level_page_table,
                    true,
                    wide_physical_address_bits,
                    execute_only_not_offered));

    // Nothing beyond the permission check applies to an entry that is not
    // present. SDM 31.3.3.1 guards its second group with "The entry is
    // present ... and", and SDM 31.4.3.4 says nothing is cached from one -
    // so a reserved bit in an absent entry has nothing to corrupt.
    //
    // This is the case that keeps a walk cheap: a zeroed table is the
    // common state of most of an address space, and calling every zero
    // entry with stray bits a misconfiguration would turn ordinary
    // violations into unhandled exits.
    auto absent_with_reserved_bits =
        zpp::arch::x86_64::vmx::epte(bit_3 | bit_6 | bit_7);

    check_false("misconfigured.absent_entry_is_never_misconfigured",
                zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    absent_with_reserved_bits,
                    level_page_map_level_4,
                    false,
                    narrow_physical_address_bits,
                    execute_only_not_offered));

    // The permission conditions, which is `normalised` stated from the
    // other side.
    zpp::arch::x86_64::vmx::epte write_without_read;
    zpp::arch::x86_64::vmx::ept_permissions(false, true, false, false)
        .apply_to(write_without_read);
    write_without_read.type(zpp::arch::x86_64::memory_type::write_back);

    check_true("misconfigured.write_without_read",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   write_without_read,
                   level_page_table,
                   true,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    zpp::arch::x86_64::vmx::epte execute_without_read;
    zpp::arch::x86_64::vmx::ept_permissions(false, false, true, false)
        .apply_to(execute_without_read);
    execute_without_read.type(zpp::arch::x86_64::memory_type::write_back);

    check_true("misconfigured.execute_without_read_without_xo",
               zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                   execute_without_read,
                   level_page_table,
                   true,
                   narrow_physical_address_bits,
                   execute_only_not_offered));

    check_false("misconfigured.execute_without_read_with_xo",
                zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    execute_without_read,
                    level_page_table,
                    true,
                    narrow_physical_address_bits,
                    execute_only_offered));
}

// === walk_ept
// ============================================================

/**
 * A four-level table mapping one address at 4 KB, with every entry
 * granting everything, which each case below then damages in one place.
 *
 * Returns the root's physical address. The address walked is fixed so the
 * indices are known, and it is deliberately not zero at any level -
 * an address of zero indexes entry zero of every table, which would let a
 * walk that used the wrong shift still land on the right entry.
 */
constexpr std::uint64_t walked_address =
    // PML4 index 1, PDPT index 2, PD index 3, PT index 4, plus an offset
    // inside the page, so that a walk using the wrong shift at any level
    // lands somewhere with no entry rather than accidentally succeeding.
    (1ull << 39) | (2ull << 30) | (3ull << 21) | (4ull << 12) | 0x678;

/**
 * The page the walk should end on, and one distinct page per size so a
 * result can say which leaf answered.
 * @{
 */
constexpr std::uint64_t mapped_page_4kb = 0x2000000;
constexpr std::uint64_t mapped_page_2mb = 0x4000000;
constexpr std::uint64_t mapped_page_1gb = 0x40000000;
/**
 * @}
 */

struct built_tables
{
    std::uint64_t root{};
    std::uint64_t page_directory_pointer_table{};
    std::uint64_t page_directory{};
    std::uint64_t page_table{};
};

built_tables build_four_levels(table_space & space)
{
    built_tables built;

    built.root = space.allocate();
    built.page_directory_pointer_table = space.allocate();
    built.page_directory = space.allocate();
    built.page_table = space.allocate();

    space.at(built.root,
             zpp::arch::x86_64::vmx::ept_walk::index_of(
                 walked_address, level_page_map_level_4)) =
        table_entry(built.page_directory_pointer_table);

    space.at(built.page_directory_pointer_table,
             zpp::arch::x86_64::vmx::ept_walk::index_of(
                 walked_address, level_page_directory_ptr)) =
        table_entry(built.page_directory);

    space.at(built.page_directory,
             zpp::arch::x86_64::vmx::ept_walk::index_of(
                 walked_address, level_page_directory)) =
        table_entry(built.page_table);

    space.at(built.page_table,
             zpp::arch::x86_64::vmx::ept_walk::index_of(
                 walked_address, level_page_table)) =
        leaf_entry(mapped_page_4kb, shift_4kb);

    return built;
}

zpp::arch::x86_64::vmx::ept_walk_result
walk(const table_space & space,
     std::uint64_t root,
     std::uint64_t address = walked_address,
     bool execute_only = execute_only_not_offered,
     std::uint64_t address_bits = narrow_physical_address_bits)
{
    return zpp::arch::x86_64::vmx::walk_ept(
        root, address, address_bits, execute_only, [&](std::uint64_t at) {
            return space.read(at);
        });
}

void test_walk_sizes()
{
    // 4 KB, the full four levels.
    {
        table_space space;
        auto built = build_four_levels(space);
        auto result = walk(space, built.root);

        check("walk.4kb.status",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::mapped),
              static_cast<std::uint64_t>(result.status));
        check("walk.4kb.page_shift", shift_4kb, result.page_shift);

        // The offset within the page is carried through, which is what
        // makes the result an address rather than a frame number. A walker
        // that dropped it would answer every access in a page with the
        // page's own base - plausible, wrong, and invisible until
        // something read a structure that straddled an offset.
        check("walk.4kb.physical_address",
              mapped_page_4kb |
                  (walked_address & ((1ull << shift_4kb) - 1)),
              result.physical_address);

        check("walk.4kb.memory_type",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::memory_type::write_back),
              static_cast<std::uint64_t>(result.type));
    }

    // 2 MB, the page-directory entry being the leaf.
    {
        table_space space;
        auto built = build_four_levels(space);
        space.at(built.page_directory,
                 zpp::arch::x86_64::vmx::ept_walk::index_of(
                     walked_address, level_page_directory)) =
            leaf_entry(mapped_page_2mb, shift_2mb);

        auto result = walk(space, built.root);

        check("walk.2mb.status",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::mapped),
              static_cast<std::uint64_t>(result.status));
        check("walk.2mb.page_shift", shift_2mb, result.page_shift);
        check("walk.2mb.physical_address",
              mapped_page_2mb |
                  (walked_address & ((1ull << shift_2mb) - 1)),
              result.physical_address);
    }

    // 1 GB, the page-directory-pointer entry being the leaf.
    {
        table_space space;
        auto built = build_four_levels(space);
        space.at(built.page_directory_pointer_table,
                 zpp::arch::x86_64::vmx::ept_walk::index_of(
                     walked_address, level_page_directory_ptr)) =
            leaf_entry(mapped_page_1gb, shift_1gb);

        auto result = walk(space, built.root);

        check("walk.1gb.status",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::mapped),
              static_cast<std::uint64_t>(result.status));
        check("walk.1gb.page_shift", shift_1gb, result.page_shift);
        check("walk.1gb.physical_address",
              mapped_page_1gb |
                  (walked_address & ((1ull << shift_1gb) - 1)),
              result.physical_address);
    }

    // A page-map level-4 entry with bit 7 set is *not* a 512 GB leaf. SDM
    // Table 31-2 reserves bit 7 there, so the walk must call it a
    // misconfiguration rather than descend or map.
    {
        table_space space;
        auto built = build_four_levels(space);
        auto & entry =
            space.at(built.root,
                     zpp::arch::x86_64::vmx::ept_walk::index_of(
                         walked_address, level_page_map_level_4));
        entry.large(true);

        auto result = walk(space, built.root);
        check("walk.pml4e_is_never_a_leaf",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::misconfigured),
              static_cast<std::uint64_t>(result.status));
    }
}

void test_walk_failures()
{
    struct level_case
    {
        const char * name;
        std::uint64_t level;
    };

    // Each of the four levels, by name, so a failure says where the walk
    // stopped rather than that some walk stopped.
    constexpr level_case levels[]{
        {"pml4e", level_page_map_level_4},
        {"pdpte", level_page_directory_ptr},
        {"pde", level_page_directory},
        {"pte", level_page_table},
    };

    for (const auto & one : levels) {
        // Not present at this level.
        {
            table_space space;
            auto built = build_four_levels(space);

            std::uint64_t table[]{built.page_table,
                                  built.page_directory,
                                  built.page_directory_pointer_table,
                                  built.root};

            space.at(table[one.level],
                     zpp::arch::x86_64::vmx::ept_walk::index_of(
                         walked_address, one.level)) =
                zpp::arch::x86_64::vmx::epte{};

            auto result = walk(space, built.root);

            if (zpp::arch::x86_64::vmx::ept_walk_status::not_present !=
                result.status) {
                check("walk.not_present_at_level", one.level, ~0ull);
            }

            // Note 2 to SDM Table 30-7: the permission bits of the
            // qualification are "cleared to 0" if any entry used is not
            // present. The walk clears its accumulator to make that fall
            // out, and a walk that left the bits it had accumulated so far
            // would tell a guest hypervisor its tables granted access to
            // an address they do not map.
            if (result.permissions.present()) {
                check("walk.not_present_clears_permissions",
                      one.level,
                      ~0ull);
            }
        }

        // Unreadable at this level, which is a different input with the
        // same required answer: a caller that cannot reach guest memory
        // hands back an empty optional.
        {
            table_space space;
            auto built = build_four_levels(space);

            std::uint64_t table[]{built.page_table,
                                  built.page_directory,
                                  built.page_directory_pointer_table,
                                  built.root};

            space.refuse_reads_of(table[one.level]);

            auto result = walk(space, built.root);

            if (zpp::arch::x86_64::vmx::ept_walk_status::not_present !=
                result.status) {
                check("walk.unreadable_at_level", one.level, ~0ull);
            }

            if (result.permissions.present()) {
                check("walk.unreadable_clears_permissions",
                      one.level,
                      ~0ull);
            }
        }

        // Misconfigured at this level, by setting a bit reserved there.
        // Bit 3 is reserved in every entry that references another table
        // and is the memory type in a leaf, so the leaf level uses a
        // reserved memory type instead - which is the same condition from
        // SDM 31.3.3.1's third group.
        {
            table_space space;
            auto built = build_four_levels(space);

            std::uint64_t table[]{built.page_table,
                                  built.page_directory,
                                  built.page_directory_pointer_table,
                                  built.root};

            constexpr std::uint64_t bit_3 = 1ull << 3;
            constexpr std::uint64_t reserved_memory_type_7 = 7ull << 3;

            auto & entry =
                space.at(table[one.level],
                         zpp::arch::x86_64::vmx::ept_walk::index_of(
                             walked_address, one.level));

            entry = zpp::arch::x86_64::vmx::epte(
                entry.value() |
                ((level_page_table == one.level) ? reserved_memory_type_7
                                                 : bit_3));

            auto result = walk(space, built.root);

            if (zpp::arch::x86_64::vmx::ept_walk_status::misconfigured !=
                result.status) {
                check("walk.misconfigured_at_level", one.level, ~0ull);
            }
        }
    }

    check_true("walk.not_present_at_every_level", true);
    check_true("walk.unreadable_at_every_level", true);
    check_true("walk.misconfigured_at_every_level", true);

    // The address itself out of range. SDM 31.3.2: "With 4-level EPT, bits
    // 51:48 of the guest-physical address must all be zero; otherwise, an
    // EPT violation occurs."
    {
        table_space space;
        auto built = build_four_levels(space);

        constexpr std::uint64_t bit_48 = 1ull << 48;

        auto result = walk(space, built.root, walked_address | bit_48);

        check("walk.address_out_of_range.status",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::
                      address_out_of_range),
              static_cast<std::uint64_t>(result.status));

        // Note 2 to SDM Table 30-7 gives this the same consequence as a
        // not-present walk, and it is a separate status precisely so the
        // two reasons stay distinguishable while the consequence is
        // shared.
        check_false("walk.address_out_of_range.clears_permissions",
                    result.permissions.present());

        // The walk must not have read anything: the address is refused
        // before the root is indexed. Asserted by refusing every read of
        // the root and seeing the same answer.
        table_space refusing;
        auto refused = build_four_levels(refusing);
        refusing.refuse_reads_of(refused.root);

        check("walk.address_out_of_range.is_decided_before_any_read",
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::
                      address_out_of_range),
              static_cast<std::uint64_t>(
                  walk(refusing, refused.root, walked_address | bit_48)
                      .status));
    }
}

void test_walk_permission_accumulation()
{
    // The accumulated permissions are the logical-AND across every entry
    // used, which SDM Table 30-7 requires of anything reporting them: bits
    // 3, 4, 5 and 6 of the qualification are each "the logical-AND of bit
    // 0 / bit 1 / bit 2 / bit 10 in the EPT paging-structure entries used
    // to translate the guest-physical address".
    //
    // Swept over every level: one level at a time is restricted and the
    // result must lose exactly what that level withheld, whatever the leaf
    // grants. A walker that reported the leaf's own permissions would pass
    // the leaf case and fail the other three.
    struct restriction
    {
        const char * name;
        zpp::arch::x86_64::vmx::ept_permissions permissions;
        unsigned expected;
    };

    constexpr restriction restrictions[]{
        {"read_only",
         zpp::arch::x86_64::vmx::ept_permissions(
             true, false, false, false),
         permission_read},
        {"read_write",
         zpp::arch::x86_64::vmx::ept_permissions(true, true, false, false),
         permission_read | permission_write},
        {"read_execute",
         zpp::arch::x86_64::vmx::ept_permissions(true, false, true, false),
         permission_read | permission_execute},
    };

    constexpr std::uint64_t restricted_levels[]{
        level_page_map_level_4,
        level_page_directory_ptr,
        level_page_directory,
        level_page_table,
    };

    for (auto level : restricted_levels) {
        for (const auto & one : restrictions) {
            table_space space;
            auto built = build_four_levels(space);

            std::uint64_t table[]{built.page_table,
                                  built.page_directory,
                                  built.page_directory_pointer_table,
                                  built.root};

            auto index = zpp::arch::x86_64::vmx::ept_walk::index_of(
                walked_address, level);

            auto & entry = space.at(table[level], index);

            entry = (level_page_table == level)
                        ? leaf_entry(
                              mapped_page_4kb, shift_4kb, one.permissions)
                        : table_entry(entry.page_number() << shift_4kb,
                                      one.permissions);

            auto result = walk(space, built.root);

            if (index_of_permissions(result.permissions) != one.expected) {
                check("walk.permissions_are_the_and_across_levels",
                      (level << 8) | one.expected,
                      (level << 8) |
                          index_of_permissions(result.permissions));
            }
        }
    }

    check_true("walk.permissions_are_the_and_across_levels", true);

    // Two levels restricting different permissions, so the result is
    // narrower than either of them. This is the case a walker that
    // *replaced* rather than intersected would get wrong while still
    // passing every single-level case above.
    {
        table_space space;
        auto built = build_four_levels(space);

        space.at(built.page_directory_pointer_table,
                 zpp::arch::x86_64::vmx::ept_walk::index_of(
                     walked_address, level_page_directory_ptr)) =
            table_entry(built.page_directory,
                        zpp::arch::x86_64::vmx::ept_permissions(
                            true, true, false, false));

        space.at(built.page_directory,
                 zpp::arch::x86_64::vmx::ept_walk::index_of(
                     walked_address, level_page_directory)) =
            table_entry(built.page_table,
                        zpp::arch::x86_64::vmx::ept_permissions(
                            true, false, true, false));

        auto result = walk(space, built.root);

        check("walk.two_levels_intersect",
              permission_read,
              index_of_permissions(result.permissions));
    }

    // The leaf granting everything does not restore what a parent removed,
    // which is the same statement made from the leaf's side and is what
    // "not the leaf's" in the header's comment means.
    {
        table_space space;
        auto built = build_four_levels(space);

        space.at(built.root,
                 zpp::arch::x86_64::vmx::ept_walk::index_of(
                     walked_address, level_page_map_level_4)) =
            table_entry(built.page_directory_pointer_table,
                        zpp::arch::x86_64::vmx::ept_permissions(
                            true, false, false, false));

        auto result = walk(space, built.root);

        check_false("walk.leaf_cannot_restore_a_parents_permission",
                    result.permissions.write());
    }
}

// === compose_ept
// =========================================================

void test_composition_outcomes()
{
    auto anything = zpp::arch::x86_64::vmx::ept_permissions::all();
    auto host = mapped_result(mapped_page_4kb, shift_4kb, anything);

    // The guest hypervisor's tables are consulted first, and every way its
    // walk can fail is its own to answer.
    check(
        "compose.guest_not_present_reflects_a_violation",
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::vmx::ept_compose_outcome::
                reflect_violation),
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::vmx::compose_ept(
                failed_result(
                    zpp::arch::x86_64::vmx::ept_walk_status::not_present),
                host,
                execute_only_not_offered)
                .outcome));

    check("compose.guest_out_of_range_reflects_a_violation",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::
                  reflect_violation),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(
                  failed_result(zpp::arch::x86_64::vmx::ept_walk_status::
                                    address_out_of_range),
                  host,
                  execute_only_not_offered)
                  .outcome));

    check("compose.guest_misconfigured_reflects_a_misconfiguration",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::
                  reflect_misconfiguration),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(
                  failed_result(zpp::arch::x86_64::vmx::ept_walk_status::
                                    misconfigured),
                  host,
                  execute_only_not_offered)
                  .outcome));

    // **Order matters, and this is the case that proves it is ordered.**
    // Both walks failed; the answer has to be the guest hypervisor's,
    // because an address its tables do not map is its business even where
    // ours would also have refused it. Telling it "host denied" instead
    // would send it looking for a fault it cannot see, and telling its
    // guest nothing at all would hang that guest.
    check(
        "compose.guest_is_answered_before_the_host",
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::vmx::ept_compose_outcome::
                reflect_violation),
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::vmx::compose_ept(
                failed_result(
                    zpp::arch::x86_64::vmx::ept_walk_status::not_present),
                failed_result(
                    zpp::arch::x86_64::vmx::ept_walk_status::not_present),
                execute_only_not_offered)
                .outcome));

    auto guest = mapped_result(mapped_page_2mb, shift_4kb, anything);

    // Ours failing is ours to answer, and never reflected: the guest
    // hypervisor's tables are innocent and it must not be told otherwise.
    for (auto status :
         {zpp::arch::x86_64::vmx::ept_walk_status::not_present,
          zpp::arch::x86_64::vmx::ept_walk_status::misconfigured,
          zpp::arch::x86_64::vmx::ept_walk_status::address_out_of_range}) {
        if (zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied !=
            zpp::arch::x86_64::vmx::compose_ept(
                guest, failed_result(status), execute_only_not_offered)
                .outcome) {
            check("compose.host_failure_is_never_reflected",
                  static_cast<std::uint64_t>(status),
                  ~0ull);
        }
    }
    check_true("compose.host_failure_is_never_reflected", true);

    // Both mapped and the intersection empty is also ours, and that is the
    // conservative direction stated in the header: its walk succeeded, so
    // telling it its tables denied the access would be false.
    check("compose.empty_intersection_is_host_denied",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(
                  mapped_result(mapped_page_2mb,
                                shift_4kb,
                                zpp::arch::x86_64::vmx::ept_permissions(
                                    true, false, false, false)),
                  mapped_result(mapped_page_4kb,
                                shift_4kb,
                                zpp::arch::x86_64::vmx::ept_permissions(
                                    false, false, true, false)),
                  execute_only_not_offered)
                  .outcome));

    // And an intersection that is non-empty only until it is normalised.
    // Read-and-write over execute-and-write leaves write alone, which is
    // not a mapping any processor accepts - so the composition has to be
    // host-denied rather than a leaf that faults for ever.
    //
    // **This is the case that turns a subtle rule into a livelock.** A
    // composition that installed write-without-read would produce an EPT
    // misconfiguration on every access to that page, and SDM 30.2.1 gives
    // a misconfiguration no exit qualification, so nothing downstream
    // would even say which permission was wrong.
    check("compose.intersection_that_normalises_to_nothing",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(
                  mapped_result(mapped_page_2mb,
                                shift_4kb,
                                zpp::arch::x86_64::vmx::ept_permissions(
                                    true, true, false, false)),
                  mapped_result(mapped_page_4kb,
                                shift_4kb,
                                zpp::arch::x86_64::vmx::ept_permissions(
                                    false, true, true, false)),
                  execute_only_not_offered)
                  .outcome));

    // Execute-only on both sides composes only where the processor can
    // express it, and is denied where it cannot - the same inputs, two
    // answers, decided by the capability bit alone.
    auto execute_only_walk =
        mapped_result(mapped_page_4kb,
                      shift_4kb,
                      zpp::arch::x86_64::vmx::ept_permissions(
                          false, false, true, false));

    check("compose.execute_only_composes_where_offered",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::composed),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(execute_only_walk,
                                                  execute_only_walk,
                                                  execute_only_offered)
                  .outcome));

    check("compose.execute_only_denied_where_not_offered",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied),
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::compose_ept(execute_only_walk,
                                                  execute_only_walk,
                                                  execute_only_not_offered)
                  .outcome));
}

void test_composition_contents()
{
    // The permissions are the intersection, normalised. Swept over the
    // whole space rather than spot checked, because this is the value that
    // ends up in a shadow leaf and every combination of two levels is one
    // a guest hypervisor can produce.
    for (unsigned left{}; left < permission_combinations; ++left) {
        for (unsigned right{}; right < permission_combinations; ++right) {
            for (auto offered :
                 {execute_only_not_offered, execute_only_offered}) {
                auto composition = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_result(mapped_page_2mb,
                                  shift_4kb,
                                  permissions_of_index(left)),
                    mapped_result(mapped_page_4kb,
                                  shift_4kb,
                                  permissions_of_index(right)),
                    offered);

                auto wanted =
                    permissions_of_index(left)
                        .intersected_with(permissions_of_index(right))
                        .normalised(offered);

                // An empty result is host-denied and carries no
                // permissions, which is the outcome case above rather than
                // this one.
                if (!wanted.present()) {
                    continue;
                }

                if (composition.permissions != wanted) {
                    check("compose.permissions_are_the_normalised_"
                          "intersection",
                          (left << 8) | right,
                          index_of_permissions(composition.permissions));
                }
            }
        }
    }

    check_true("compose.permissions_are_the_normalised_intersection",
               true);

    auto anything = zpp::arch::x86_64::vmx::ept_permissions::all();

    // The page size is the smaller of the two, in both directions. A 1 GB
    // mapping in the guest hypervisor's tables over a 4 KB entry of ours
    // installed at 1 GB would grant a whole gigabyte the permissions of
    // one page, which is the silent privilege escalation nested EPT exists
    // to prevent - and the reverse would install a leaf at a size the
    // guest hypervisor's tables do not describe.
    check("compose.page_shift.guest_1gb_over_host_4kb",
          shift_4kb,
          zpp::arch::x86_64::vmx::compose_ept(
              mapped_result(mapped_page_1gb, shift_1gb, anything),
              mapped_result(mapped_page_4kb, shift_4kb, anything),
              execute_only_not_offered)
              .page_shift);

    check("compose.page_shift.guest_4kb_under_host_2mb",
          shift_4kb,
          zpp::arch::x86_64::vmx::compose_ept(
              mapped_result(mapped_page_4kb, shift_4kb, anything),
              mapped_result(mapped_page_2mb, shift_2mb, anything),
              execute_only_not_offered)
              .page_shift);

    check("compose.page_shift.guest_2mb_under_host_1gb",
          shift_2mb,
          zpp::arch::x86_64::vmx::compose_ept(
              mapped_result(mapped_page_2mb, shift_2mb, anything),
              mapped_result(mapped_page_1gb, shift_1gb, anything),
              execute_only_not_offered)
              .page_shift);

    check("compose.page_shift.equal_sizes_are_kept",
          shift_2mb,
          zpp::arch::x86_64::vmx::compose_ept(
              mapped_result(mapped_page_2mb, shift_2mb, anything),
              mapped_result(mapped_page_2mb, shift_2mb, anything),
              execute_only_not_offered)
              .page_shift);

    // The physical address is ours. The guest hypervisor's walk produced a
    // first-level guest-physical address, which is the *input* to our walk
    // rather than anything to install - a composition that kept it would
    // put a guest-physical address into a table the processor reads as
    // host-physical.
    check("compose.physical_address_is_the_hosts",
          mapped_page_4kb,
          zpp::arch::x86_64::vmx::compose_ept(
              mapped_result(mapped_page_2mb, shift_4kb, anything),
              mapped_result(mapped_page_4kb, shift_4kb, anything),
              execute_only_not_offered)
              .physical_address);

    // The memory type is ours too, and that is a deliberate divergence
    // rather than an oversight - the header records it and BACKLOG.md
    // carries it. The type describes a physical page, and which type a
    // physical page needs is settled by the memory-type range registers,
    // which our own tables are derived from.
    //
    // Asserted with the two sides disagreeing, because a case where they
    // agree cannot tell which one was taken.
    check(
        "compose.memory_type_is_the_hosts",
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::memory_type::write_back),
        static_cast<std::uint64_t>(
            zpp::arch::x86_64::vmx::compose_ept(
                mapped_result(mapped_page_2mb,
                              shift_4kb,
                              anything,
                              zpp::arch::x86_64::memory_type::uncachable),
                mapped_result(mapped_page_4kb,
                              shift_4kb,
                              anything,
                              zpp::arch::x86_64::memory_type::write_back),
                execute_only_not_offered)
                .type));
}

// === reflected_ept_violation_qualification
// ===============================

void test_reflected_qualification()
{
    // The bit numbers are SDM Table 30-7's, and they are named here
    // because the whole case is about which of them survive.
    constexpr std::uint64_t qualification_read = 1ull << 0;
    constexpr std::uint64_t qualification_write = 1ull << 1;
    constexpr std::uint64_t qualification_fetch = 1ull << 2;
    constexpr std::uint64_t qualification_readable = 1ull << 3;
    constexpr std::uint64_t qualification_writable = 1ull << 4;
    constexpr std::uint64_t qualification_executable = 1ull << 5;
    constexpr std::uint64_t qualification_user_executable = 1ull << 6;
    constexpr std::uint64_t qualification_linear_valid = 1ull << 7;
    constexpr std::uint64_t qualification_translation = 1ull << 8;
    constexpr std::uint64_t qualification_nmi_unblocking = 1ull << 12;
    constexpr std::uint64_t qualification_shadow_stack = 1ull << 13;
    constexpr std::uint64_t qualification_asynchronous = 1ull << 16;

    // Bits 11:9 need "advanced VM-exit information for EPT violations",
    // bit 14 supervisor shadow-stack control through the EPT pointer, and
    // bit 15 guest-paging verification. None is reported by this VMM, so
    // the SDM leaves their value undefined and forwarding one is how a
    // guest comes to depend on it.
    constexpr std::uint64_t qualification_undefined =
        (1ull << 9) | (1ull << 10) | (1ull << 11) | (1ull << 14) |
        (1ull << 15);

    constexpr std::uint64_t about_the_access =
        qualification_read | qualification_write | qualification_fetch |
        qualification_linear_valid | qualification_translation |
        qualification_nmi_unblocking | qualification_shadow_stack |
        qualification_asynchronous;

    auto nothing = failed_result(
        zpp::arch::x86_64::vmx::ept_walk_status::not_present);

    // Everything describing the access is hardware's to report and is kept
    // exactly as it arrived.
    check("qualification.access_bits_are_kept",
          about_the_access,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              about_the_access, nothing, false));

    // **The permission bits come from the walk of the guest hypervisor's
    // tables, never from hardware's qualification.** Hardware walked the
    // *shadow*, whose entries hold the intersection of both levels, so
    // forwarding them would tell a guest hypervisor that its own tables
    // refused an access they permit - and it would then go looking for a
    // bug in tables that are correct.
    //
    // Asserted with hardware claiming all three and the walk granting
    // none.
    check("qualification.permission_bits_are_not_forwarded",
          0,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              qualification_readable | qualification_writable |
                  qualification_executable,
              nothing,
              false) &
              (qualification_readable | qualification_writable |
               qualification_executable));

    // And with the walk granting them while hardware claims none, which is
    // the direction that actually happens: our own tables removed write
    // from a page the guest hypervisor maps writable.
    auto granting_everything =
        mapped_result(mapped_page_4kb,
                      shift_4kb,
                      zpp::arch::x86_64::vmx::ept_permissions::all());

    check("qualification.permission_bits_come_from_the_guest_walk",
          qualification_readable | qualification_writable |
              qualification_executable,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              0, granting_everything, false) &
              (qualification_readable | qualification_writable |
               qualification_executable));

    // Bit 6 exists only with mode-based execute control. Table 30-7: "If
    // the 'mode-based execute control' VM-execution control is 0, the
    // value of this bit is undefined", so it is left clear rather than
    // filled in - nothing can then come to rely on it.
    check("qualification.user_execute_bit_clear_without_the_control",
          0,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              qualification_user_executable, granting_everything, false) &
              qualification_user_executable);

    check("qualification.user_execute_bit_set_with_the_control",
          qualification_user_executable,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              0, granting_everything, true) &
              qualification_user_executable);

    // The bits whose meaning depends on a capability this VMM does not
    // report are cleared rather than forwarded.
    check("qualification.undefined_bits_are_cleared",
          0,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              ~std::uint64_t{}, granting_everything, true) &
              qualification_undefined);

    // Everything at once: an access that hardware reported as a write to a
    // page the guest hypervisor maps read-only, which is exactly what a
    // second-level guest writing a page its own hypervisor watches
    // produces.
    auto read_only_in_the_guests_tables = mapped_result(
        mapped_page_4kb,
        shift_4kb,
        zpp::arch::x86_64::vmx::ept_permissions(true, false, true, false));

    check("qualification.watched_page_write_reflects_correctly",
          qualification_write | qualification_linear_valid |
              qualification_translation | qualification_readable |
              qualification_executable,
          zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              qualification_write | qualification_linear_valid |
                  qualification_translation | qualification_readable |
                  qualification_writable | qualification_executable,
              read_only_in_the_guests_tables,
              false));
}

} // namespace

int main()
{
    test_permissions();
    test_misconfiguration();
    test_walk_sizes();
    test_walk_failures();
    test_walk_permission_accumulation();
    test_composition_outcomes();
    test_composition_contents();
    test_reflected_qualification();

    std::println(
        "nested_ept: {} checks, {} failures", g_checks, g_failures);

    return (0 == g_failures) ? 0 : 1;
}
