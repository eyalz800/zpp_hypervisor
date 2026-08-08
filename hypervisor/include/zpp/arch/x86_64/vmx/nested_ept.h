#pragma once
#include "zpp/arch/x86_64/vmx/ept.h"
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * The four access permissions an extended page table entry carries, and
 * the rules for combining them.
 *
 * A separate type from `epte` because combining is the whole of what
 * nested EPT does with permissions, and doing it on raw entries invites
 * mixing a permission with an address. There are four rather than three
 * because mode-based execute control splits execute in two - SDM Tables
 * 31-1 through 31-7 give bit 2 as "execute access for supervisor-mode
 * linear addresses" and bit 10 as "Execute access for user-mode linear
 * addresses" when that control is 1.
 */
class ept_permissions
{
public:
    constexpr ept_permissions() = default;

    constexpr ept_permissions(bool read,
                              bool write,
                              bool execute,
                              bool execute_user) :
        m_read(read),
        m_write(write),
        m_execute(execute),
        m_execute_user(execute_user)
    {
    }

    /**
     * Everything permitted, which is what an accumulator starts as: the
     * combination below is an intersection, so the identity is "all".
     */
    static constexpr ept_permissions all()
    {
        return ept_permissions(true, true, true, true);
    }

    /**
     * The permissions an entry grants.
     */
    static constexpr ept_permissions of(const epte & entry)
    {
        return ept_permissions(entry.read(),
                               entry.write(),
                               entry.execute(),
                               entry.execute_user());
    }

    constexpr bool read() const
    {
        return m_read;
    }

    constexpr bool write() const
    {
        return m_write;
    }

    constexpr bool execute() const
    {
        return m_execute;
    }

    constexpr bool execute_user() const
    {
        return m_execute_user;
    }

    /**
     * Whether an entry carrying these permissions would be present at all.
     *
     * SDM 31.3.2: "An EPT paging-structure entry is present if any of bits
     * 2:0 is 1; otherwise, the entry is not present", with the note that
     * follows: "If the 'mode-based execute control for EPT' VM-execution
     * control is 1, an EPT paging-structure entry is present if any of
     * bits 2:0 or bit 10 is 1."
     *
     * This is why an empty intersection needs no special case anywhere: no
     * permissions means not present, and not present means the guest takes
     * an EPT violation, which is exactly the wanted answer.
     */
    constexpr bool present() const
    {
        return m_read || m_write || m_execute || m_execute_user;
    }

    /**
     * The intersection of two permission sets, which is what composing two
     * levels of extended page tables does: an access is allowed only if
     * both the guest hypervisor's tables and this VMM's own allow it.
     */
    constexpr ept_permissions
    intersected_with(const ept_permissions & other) const
    {
        return ept_permissions(m_read && other.m_read,
                               m_write && other.m_write,
                               m_execute && other.m_execute,
                               m_execute_user && other.m_execute_user);
    }

    /**
     * The permissions with any combination the processor rejects removed.
     *
     * **This is not tidying, and skipping it is a bug that never stops.**
     * An intersection can perfectly well produce write-without-read, and
     * SDM 31.3.3.1 makes that an EPT *misconfiguration* rather than a
     * restrictive mapping: "An EPT misconfiguration occurs if translation
     * of a guest-physical address encounters an EPT paging-structure entry
     * that meets any of the following conditions: Bit 0 of the entry is
     * clear (indicating that data reads are not allowed) and any of the
     * following hold: Bit 1 is set (indicating that data writes are
     * allowed)."
     *
     * The same list continues: "The processor does not support
     * execute-only translations and either of the following hold: Bit 2 is
     * set ... the 'mode-based execute control for EPT' VM-execution
     * control is 1 and bit 10 is set." So execute-without-read is legal
     * only where the processor reports execute-only translations, which is
     * IA32_VMX_EPT_VPID_CAP bit 0 (SDM A.10).
     *
     * A misconfiguration written into this VMM's own shadow tables is an
     * exit taken on every attempt, for ever, with no guest instruction
     * ever retiring - and the exit carries no exit qualification to say
     * why, because SDM 30.2.1 does not list EPT misconfiguration among the
     * exits that save one.
     *
     * Dropping permissions rather than adding read is the only safe
     * direction. Adding read would grant an access neither side granted.
     */
    constexpr ept_permissions normalised(bool execute_only_supported) const
    {
        if (m_read) {
            return *this;
        }

        // No read, so no write, and no execute of either kind unless the
        // processor can express execute-only.
        if (execute_only_supported) {
            return ept_permissions(
                false, false, m_execute, m_execute_user);
        }

        return ept_permissions();
    }

    /**
     * Writes these permissions into an entry, leaving everything else in
     * it alone.
     */
    constexpr void apply_to(epte & entry) const
    {
        entry.read(m_read);
        entry.write(m_write);
        entry.execute(m_execute);
        entry.execute_user(m_execute_user);
    }

    constexpr bool operator==(const ept_permissions &) const = default;

private:
    bool m_read{};
    bool m_write{};
    bool m_execute{};
    bool m_execute_user{};
};

/**
 * How a walk of an extended page table ended.
 */
enum class ept_walk_status
{
    /**
     * The walk reached an entry that maps a page.
     */
    mapped,

    /**
     * Some entry along the way was not present, which SDM 31.3.3.2 makes
     * an EPT violation: "Translation of the guest-physical address
     * encounters an EPT paging-structure entry that is not present".
     */
    not_present,

    /**
     * Some entry along the way held a value the processor rejects, which
     * SDM 31.3.3.1 makes an EPT misconfiguration.
     */
    misconfigured,

    /**
     * The address itself is out of range. SDM 31.3.2: "With 4-level EPT,
     * bits 51:48 of the guest-physical address must all be zero;
     * otherwise, an EPT violation occurs."
     *
     * Separate from not_present because the two produce the same exit for
     * different reasons, and because Note 2 to SDM Table 30-7 makes them
     * share one consequence worth keeping visible: the permission bits of
     * the exit qualification are "cleared to 0" in both cases.
     */
    address_out_of_range,
};

/**
 * Where a walk ended and what it found.
 */
struct ept_walk_result
{
    ept_walk_status status{ept_walk_status::not_present};

    /**
     * The physical address the walk produced, offset within the page
     * included. Meaningless unless the status is mapped.
     */
    std::uint64_t physical_address{};

    /**
     * How large the page the walk ended on is, as a shift: 12 for 4 KB, 21
     * for 2 MB, 30 for 1 GB.
     *
     * Kept because composing two walks has to end on the smaller of the
     * two page sizes, and because it decides which shadow level a leaf may
     * be written at.
     */
    std::uint64_t page_shift{};

    /**
     * The logical-AND of each permission across every entry the walk used.
     *
     * Accumulated rather than taken from the leaf, and that is what SDM
     * Table 30-7 requires of anything reporting it: bits 3, 4, 5 and 6 of
     * the EPT-violation exit qualification are each "the logical-AND of
     * bit 0 / bit 1 / bit 2 / bit 10 in the EPT paging-structure entries
     * used to translate the guest-physical address".
     */
    ept_permissions permissions{ept_permissions::all()};

    /**
     * The memory type from the leaf, valid only when the status is mapped
     * and only meaningful for the last entry - SDM Tables 31-3, 31-5 and
     * 31-7 put the field in bits 5:3 of a leaf, while Table 31-6 reserves
     * those bits in an entry that references a table.
     */
    memory_type type{};
};

/**
 * How many levels a 4-level extended page table walk has, and the shift of
 * the guest-physical address bits each level is indexed by.
 *
 * SDM 31.3.2 gives them: the PML4E is selected by "bits 47:39", the PDPTE
 * by "bits 38:30", the PDE by "bits 29:21" and the PTE by "bits 20:12".
 */
namespace ept_walk
{
constexpr std::uint64_t levels = 4;

/**
 * The shift for a level, counting the leaf as level 0.
 */
constexpr std::uint64_t shift_of(std::uint64_t level)
{
    return 12 + (9 * level);
}

/**
 * The index into the table at a level.
 */
constexpr std::uint64_t index_of(std::uint64_t guest_physical,
                                 std::uint64_t level)
{
    return (guest_physical >> shift_of(level)) & 0x1ff;
}

/**
 * Whether an address is one 4-level EPT can translate at all.
 */
constexpr bool address_in_range(std::uint64_t guest_physical)
{
    return 0 == (guest_physical >> 48);
}

/**
 * Whether an entry at a level holds a value the processor rejects.
 *
 * The conditions are SDM 31.3.3.1's, in the order that section gives them,
 * and every one of them is checked here rather than assumed away:
 *
 * - the permission combinations, which `ept_permissions::normalised`
 *   describes at length,
 * - a reserved bit set, "This includes the setting of a bit in the range
 *   51:12 in position MAXPHYADDR or above",
 * - a leaf whose "value of bits 5:3 (EPT memory type) is 2, 3, or 7 (these
 *   values are reserved)".
 *
 * The reserved bits differ by level and by whether the entry is a leaf, so
 * this takes both. SDM Table 31-2 reserves bits 7:3 of a PML4E - which is
 * also why a PML4E can never map a page - Table 31-4 the same of a PDPTE
 * that references a page directory, Table 31-6 bits 6:3 of a PDE that
 * references a page table, Table 31-3 bits 29:12 of a 1 GB PDPTE and Table
 * 31-5 bits 20:12 of a 2 MB PDE.
 */
constexpr bool misconfigured(const epte & entry,
                             std::uint64_t level,
                             bool leaf,
                             std::uint64_t physical_address_bits,
                             bool execute_only_supported)
{
    auto permissions = ept_permissions::of(entry);

    // The permission combinations. Written as "the normalised form differs
    // from what is there", which is the same test spelled once: normalised
    // only ever removes what the processor rejects.
    if (permissions != permissions.normalised(execute_only_supported)) {
        return true;
    }

    // Nothing further applies to an entry that is not present.
    // SDM 31.3.3.1 guards its second group with "The entry is present (see
    // Section 31.3.2) and ...", and SDM 31.4.3.4 says the same from the
    // other side: no information is cached from an entry that is not
    // present, so there is nothing for a reserved bit in one to corrupt.
    if (!permissions.present()) {
        return false;
    }

    auto value = entry.value();

    // Anything at or above the processor's physical-address width, within
    // bits 51:12.
    auto address_reserved =
        ((1ull << 52) - 1) & ~((1ull << physical_address_bits) - 1);
    if (0 != (value & address_reserved)) {
        return true;
    }

    if (!leaf) {
        // Bits 7:3, except in a page-directory entry that references a
        // page table, where bit 7 is the discriminator rather than
        // reserved and is 0 here by definition.
        constexpr std::uint64_t upper_levels_reserved = 0xf8;
        constexpr std::uint64_t page_directory_reserved = 0x78;

        auto reserved =
            (1 == level) ? page_directory_reserved : upper_levels_reserved;

        return 0 != (value & reserved);
    }

    // A leaf's low address bits, which must be zero for the page size the
    // level implies. Nothing to check at the lowest level, where every bit
    // from 12 up is address.
    if (level > 0) {
        auto reserved = ((1ull << shift_of(level)) - 1) & ~0xfffull;
        if (0 != (value & reserved)) {
            return true;
        }
    }

    // The memory type. SDM 31.3.7.2: "0 = UC; 1 = WC; 4 = WT; 5 = WP; and
    // 6 = WB. Other values are reserved and cause EPT misconfigurations".
    switch (entry.type()) {
    case memory_type::uncachable:
    case memory_type::write_combining:
    case memory_type::write_through:
    case memory_type::write_protected:
    case memory_type::write_back:
        return false;
    default:
        return true;
    }
}

} // namespace ept_walk

/**
 * Walks an extended page table for a guest-physical address.
 *
 * Pure, and deliberately so: everything it touches comes through `read`,
 * which is handed the physical address of an entry and returns it. That
 * keeps the architectural rules - the indices, the leaf discrimination,
 * the misconfiguration conditions, the permission accumulation - separable
 * from how a caller reaches guest memory, and it is what lets them be
 * tested without a processor.
 *
 * `read` is `std::optional<epte>(std::uint64_t physical_address)`, or
 * anything convertible: an empty result means the entry could not be read,
 * which is reported as not present. A caller that cannot read an entry is
 * a caller whose guest handed it an address outside memory, and refusing
 * is the same answer the processor gives for a table it cannot reach.
 */
template <typename ReadEntry>
constexpr ept_walk_result walk_ept(std::uint64_t table_physical_address,
                                   std::uint64_t guest_physical,
                                   std::uint64_t physical_address_bits,
                                   bool execute_only_supported,
                                   ReadEntry && read)
{
    ept_walk_result result;

    if (!ept_walk::address_in_range(guest_physical)) {
        // Note 2 to SDM Table 30-7 has this share its consequence with a
        // not-present walk: the permission bits of the exit qualification
        // are cleared when "4-level EPT is in use and the guest-physical
        // address sets any bits in the range 51:48".
        result.status = ept_walk_status::address_out_of_range;
        result.permissions = ept_permissions();
        return result;
    }

    auto table = table_physical_address;

    for (auto level = ept_walk::levels; level-- > 0;) {
        auto index = ept_walk::index_of(guest_physical, level);

        auto entry = read(table + (index * sizeof(std::uint64_t)));
        if (!entry) {
            result.status = ept_walk_status::not_present;
            result.permissions = ept_permissions();
            return result;
        }

        auto permissions = ept_permissions::of(*entry);

        if (!permissions.present()) {
            result.status = ept_walk_status::not_present;
            // Cleared rather than accumulated, per Note 2 to SDM Table
            // 30-7: the permission bits are "cleared to 0" if "any of EPT
            // paging-structure entries used to translate the
            // guest-physical address ... is not present".
            result.permissions = ept_permissions();
            return result;
        }

        // A leaf either because the level says so - the lowest level is
        // always a page - or because bit 7 says so, which SDM Tables 31-3
        // and 31-5 give as "Must be 1 (otherwise, this entry references an
        // EPT page directory/table)". A PML4E is never a leaf: Table 31-2
        // reserves bit 7 in it.
        auto leaf = (0 == level) ||
                    ((level < (ept_walk::levels - 1)) && entry->large());

        if (ept_walk::misconfigured(*entry,
                                    level,
                                    leaf,
                                    physical_address_bits,
                                    execute_only_supported)) {
            result.status = ept_walk_status::misconfigured;
            return result;
        }

        result.permissions = result.permissions.intersected_with(
            ept_permissions::of(*entry));

        if (leaf) {
            auto shift = ept_walk::shift_of(level);
            auto page = entry->value() &
                        (((1ull << 52) - 1) & ~((1ull << shift) - 1));

            result.status = ept_walk_status::mapped;
            result.page_shift = shift;
            result.physical_address =
                page | (guest_physical & ((1ull << shift) - 1));
            result.type = entry->type();
            return result;
        }

        table = entry->page_number() << 12;
    }

    // Unreachable: level 0 is always a leaf and returns above.
    return result;
}

} // namespace zpp::arch::x86_64::vmx
