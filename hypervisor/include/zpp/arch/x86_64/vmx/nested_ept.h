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

/**
 * What composing two walks of two levels of extended page tables produced,
 * and therefore who owns the fault that provoked it.
 */
enum class ept_compose_outcome
{
    /**
     * Both walks succeeded and the composition is a mapping the shadow can
     * hold.
     */
    composed,

    /**
     * The guest hypervisor's own tables do not map the address. Its guest
     * would have taken an EPT violation on real hardware, so the exit is
     * reflected to it.
     */
    reflect_violation,

    /**
     * The guest hypervisor's own tables hold a value the processor
     * rejects. Reflected as an EPT misconfiguration for the same reason.
     */
    reflect_misconfiguration,

    /**
     * This VMM's own tables are what denied the access - the module, a
     * watched page, or an address past the identity map. Never shown to
     * the guest hypervisor, whose tables are innocent and whose view of
     * its own guest would be wrong if it were told otherwise.
     */
    host_denied,
};

/**
 * The composition of one guest-physical address through both levels.
 */
struct ept_composition
{
    ept_compose_outcome outcome{ept_compose_outcome::host_denied};

    /**
     * The permissions to install: the intersection of both walks, with any
     * combination the processor rejects removed.
     */
    ept_permissions permissions{};

    /**
     * The host physical address the composition resolves to, offset
     * included.
     */
    std::uint64_t physical_address{};

    /**
     * The larger page the two walks agree on, as a shift. A leaf may be
     * installed at this size and no larger: a 2 MB mapping in the guest
     * hypervisor's tables over a 4 KB entry of ours has to be split, or
     * the shadow would grant the whole 2 MB the permissions of one page.
     */
    std::uint64_t page_shift{};

    /**
     * The memory type, taken from *our* walk.
     *
     * A known and deliberate divergence, recorded in BACKLOG.md rather
     * than hidden: the type describes a physical page, and which type a
     * physical page needs is settled by the memory-type range registers,
     * which this VMM's own tables are derived from. A guest hypervisor's
     * choice for its own guest is a policy about memory whose physical
     * layout it does not own. So an L1 that maps a page uncacheable gets
     * our derivation instead.
     */
    memory_type type{};
};

/**
 * Composes a walk of the guest hypervisor's extended page tables with a
 * walk of this VMM's own.
 *
 * The first walk translates a second-level guest-physical address to a
 * first-level one; the second translates that to a host physical address.
 * Composing them is what a shadow extended page table holds, and doing it
 * here - on two results, with no memory access of its own - is what makes
 * the rules testable.
 */
constexpr ept_composition compose_ept(const ept_walk_result & guest,
                                      const ept_walk_result & host,
                                      bool execute_only_supported)
{
    ept_composition composition;

    // The guest hypervisor's tables first, because a fault they explain is
    // one this VMM must not absorb. Order matters: an address its tables
    // do not map is its business even if ours would also have refused it.
    switch (guest.status) {
    case ept_walk_status::misconfigured:
        composition.outcome =
            ept_compose_outcome::reflect_misconfiguration;
        return composition;
    case ept_walk_status::not_present:
    case ept_walk_status::address_out_of_range:
        composition.outcome = ept_compose_outcome::reflect_violation;
        return composition;
    case ept_walk_status::mapped:
        break;
    }

    // Ours second. A misconfiguration here is a bug in this VMM rather
    // than anything the guest did, and it is reported as ours so that a
    // caller can stop rather than hand the guest hypervisor a fault it
    // cannot explain.
    if (ept_walk_status::mapped != host.status) {
        composition.outcome = ept_compose_outcome::host_denied;
        return composition;
    }

    auto permissions = guest.permissions.intersected_with(host.permissions)
                           .normalised(execute_only_supported);

    // An empty intersection is attributed to us rather than reflected, and
    // that is the conservative direction on purpose. Reaching here means
    // the guest hypervisor's tables *do* map the address - its walk
    // succeeded - so telling it that its own tables denied the access
    // would be false. What is left is either our protection, which is ours
    // to handle, or two permission sets that overlap in nothing, which is
    // also not something the guest hypervisor can act on.
    if (!permissions.present()) {
        composition.outcome = ept_compose_outcome::host_denied;
        return composition;
    }

    composition.outcome = ept_compose_outcome::composed;
    composition.permissions = permissions;
    composition.physical_address = host.physical_address;
    composition.page_shift = (guest.page_shift < host.page_shift)
                                 ? guest.page_shift
                                 : host.page_shift;
    composition.type = host.type;

    return composition;
}

/**
 * The exit qualification to give the guest hypervisor for a reflected EPT
 * violation.
 *
 * **It has to be synthesised rather than forwarded, and forwarding it is a
 * subtle lie.** SDM Table 30-7 defines bits 3, 4, 5 and 6 as "the
 * logical-AND of bit 0 / bit 1 / bit 2 / bit 10 in the EPT
 * paging-structure entries used to translate the guest-physical address" -
 * and the entries hardware used were the *shadow's*, which hold the
 * intersection of both levels. Passing them on would tell a guest
 * hypervisor that its own tables refused an access they permit, and it
 * would then go looking for a bug in them.
 *
 * So those four come from the walk of its tables alone. Note 2 to the same
 * table gives the case where they are not permissions at all: bits 5:3 are
 * "cleared to 0" if any entry used "is not present" or if "4-level EPT is
 * in use and the guest-physical address sets any bits in the range 51:48".
 * Note 3 says the same of bit 6, separately and only when mode-based
 * execute control is 1. Both fall out of the walk having already cleared
 * its accumulated permissions in those two cases.
 *
 * Everything describing the *access* rather than the tables is kept as
 * hardware reported it: bits 2:0, the access type; bit 7, whether the
 * guest linear address is valid, and bit 8, whether the access was to a
 * paging-structure entry; bit 12, NMI unblocking due to IRET; bit 13, a
 * shadow-stack access; and bit 16, an access asynchronous to instruction
 * execution.
 *
 * Everything else is cleared rather than forwarded, because every one of
 * them is a capability this VMM does not report and whose value the SDM
 * therefore leaves undefined: bits 11:9 need "advanced VM-exit information
 * for EPT violations", which Note 4 makes an IA32_VMX_EPT_VPID_CAP bit;
 * bit 14 needs supervisor shadow-stack control enabled through the EPT
 * pointer; bit 15 needs guest-paging verification. Forwarding an undefined
 * bit is how a guest comes to depend on one.
 */
constexpr std::uint64_t
reflected_ept_violation_qualification(std::uint64_t hardware_qualification,
                                      const ept_walk_result & guest,
                                      bool mode_based_execute_control)
{
    // What describes the access, and is therefore hardware's to report.
    constexpr std::uint64_t about_the_access =
        (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 7) |
        (1ull << 8) | (1ull << 12) | (1ull << 13) | (1ull << 16);

    auto qualification = hardware_qualification & about_the_access;

    if (guest.permissions.read()) {
        qualification |= (1ull << 3);
    }

    if (guest.permissions.write()) {
        qualification |= (1ull << 4);
    }

    if (guest.permissions.execute()) {
        qualification |= (1ull << 5);
    }

    // Bit 6 exists only with mode-based execute control. Table 30-7: "If
    // the 'mode-based execute control' VM-execution control is 0, the
    // value of this bit is undefined." Left clear in that case rather than
    // filled in with the user-execute permission, so nothing can come to
    // rely on it.
    if (mode_based_execute_control && guest.permissions.execute_user()) {
        qualification |= (1ull << 6);
    }

    return qualification;
}

} // namespace zpp::arch::x86_64::vmx
