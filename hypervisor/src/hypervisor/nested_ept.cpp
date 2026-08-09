#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/generic.h"
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <cstring>
#include <optional>

namespace zpp::hypervisor
{
namespace
{
using arch::x86_64::memory_type;
using arch::x86_64::vmx::compose_ept;
using arch::x86_64::vmx::ept_compose_outcome;
using arch::x86_64::vmx::ept_permissions;
using arch::x86_64::vmx::ept_walk_result;
using arch::x86_64::vmx::ept_walk_status;
using arch::x86_64::vmx::epte;

/**
 * How many entries a paging structure holds.
 */
constexpr std::size_t entries_per_table = 512;

/**
 * The shift of each level's page size, named so the loops below read as
 * the levels they walk rather than as arithmetic.
 * @{
 */
constexpr std::uint64_t page_shift_4kb = 12;
constexpr std::uint64_t page_shift_2mb = 21;
constexpr std::uint64_t page_shift_1gb = 30;
/**
 * @}
 */

/**
 * One page of a guest hypervisor's extended page table, read out of guest
 * memory in a single copy.
 *
 * A whole page at a time rather than an entry at a time, and that is the
 * difference between a build that is affordable and one that is not: an
 * entry at a time costs a window mapping, an `invlpg` and a lock
 * acquisition each, so a page directory would cost five hundred and twelve
 * of them instead of one.
 */
struct guest_table
{
    epte entries[entries_per_table]{};
};

/**
 * A walk result standing for one entry that maps a page, as though a walk
 * had ended on it.
 *
 * Used where the structure is already known - a large leaf found while
 * descending, or one of this VMM's own entries reached by index - so that
 * everything downstream sees the one shape `compose_ept` takes.
 */
constexpr ept_walk_result leaf_result(const epte & entry,
                                      std::uint64_t guest_physical,
                                      std::uint64_t shift,
                                      ept_permissions above)
{
    ept_walk_result result;
    result.status = ept_walk_status::mapped;
    result.page_shift = shift;
    result.permissions =
        above.intersected_with(ept_permissions::of(entry));
    result.type = entry.type();

    auto page =
        entry.value() & (((1ull << 52) - 1) & ~((1ull << shift) - 1));
    result.physical_address =
        page | (guest_physical & ((1ull << shift) - 1));

    return result;
}

} // namespace

std::uint64_t hypervisor::physical_address_bits()
{
    // CPUID.80000008H:EAX[7:0], which SDM 31.3.3.1 makes the boundary for
    // the reserved address bits of an extended page table entry: an entry
    // setting "a bit in the range 51:12 in position MAXPHYADDR or above"
    // is an EPT misconfiguration.
    //
    // Cached because the walker needs it per entry and CPUID is a
    // serialising instruction. Read once on the first call rather than at
    // launch, so this stays usable from anywhere.
    if (0 == this->cached_physical_address_bits) {
        std::uint32_t result[4]{};
        arch::x86_64::cpuid(0x80000008, 0, result);

        auto bits = result[0] & 0xff;

        // A processor that reports nothing here is one this VMM cannot
        // judge reserved bits against. Thirty six is the architectural
        // floor for physical addressing, so it is the value that refuses
        // the most while remaining possible - erring towards calling an
        // entry misconfigured rather than accepting one hardware would
        // reject.
        this->cached_physical_address_bits = (0 != bits) ? bits : 36;
    }

    return this->cached_physical_address_bits;
}

ept_walk_result hypervisor::host_ept_lookup(std::uint64_t physical_address)
{
    // This VMM's own tables, reached by index rather than by walking them.
    // initialize_ept builds a complete identity map of the first 512 GB
    // out of members of this class, so the structure is known: one
    // page-map level-4 entry, five hundred and twelve page directories,
    // and either a 2 MB leaf or a split table below each. epte_for already
    // indexes it the same way and says so.
    ept_walk_result result;

    constexpr std::uint64_t identity_limit = 512ull * 1024 * 1024 * 1024;

    if (physical_address >= identity_limit) {
        result.status = ept_walk_status::not_present;
        result.permissions = ept_permissions();
        return result;
    }

    // The two levels above the page directory. Both are built granting
    // everything, so intersecting them changes nothing today - it is done
    // anyway because a future protection applied at a coarser level would
    // otherwise be silently dropped from every composition.
    auto above = ept_permissions::of(this->epml4[0])
                     .intersected_with(ept_permissions::of(
                         this->epdpt[physical_address >> page_shift_1gb]));

    auto & epde = this->epd[physical_address >> page_shift_1gb]
                           [(physical_address >> page_shift_2mb) & 0x1ff];

    if (epde.large()) {
        return leaf_result(epde, physical_address, page_shift_2mb, above);
    }

    // Split, so the leaf is in a table the entry names by physical
    // address. The module is not identity mapped in the host page table,
    // hence the reverse map rather than a dereference - the same reason
    // epte_for gives.
    auto table_physical = epde.page_number() << 12;
    auto found = this->module_physical_to_virtual.find(table_physical);
    if (this->module_physical_to_virtual.end() == found) {
        result.status = ept_walk_status::not_present;
        result.permissions = ept_permissions();
        return result;
    }

    above = above.intersected_with(ept_permissions::of(epde));

    auto table = reinterpret_cast<const epte *>(found->second);
    auto & leaf = table[(physical_address >> page_shift_4kb) & 0x1ff];

    return leaf_result(leaf, physical_address, page_shift_4kb, above);
}

std::expected<arch::x86_64::vmx::epte *, zpp::error>
hypervisor::shadow_ept_table(std::size_t cpu)
{
    // A scan of the shared pool rather than a bump, because the pool is
    // shared between this processor's shadows and they are freed
    // individually. Ninety-six entries and about sixty allocations per
    // build, so the scan costs nothing next to the walk it serves.
    auto slot = this->shadow_ept_current_slot[cpu];

    std::size_t index{};
    for (;; ++index) {
        if (index >= shadow_ept_tables_per_cpu) {
            return std::unexpected(
                zpp::error{error::out_of_shadow_ept_tables});
        }

        if (shadow_table_free == this->shadow_ept_table_slot[cpu][index]) {
            break;
        }
    }

    this->shadow_ept_table_slot[cpu][index] =
        static_cast<std::uint8_t>(slot + 1);
    this->shadow_ept_tables_used[cpu][slot] =
        this->shadow_ept_tables_used[cpu][slot] + 1;

    auto table = this->shadow_ept_tables[cpu][index];

    // Zeroed on handing out rather than on release, so a rebuild costs
    // nothing for the part of the pool it does not use. An entry of all
    // zeroes is not present - SDM 31.3.2's presence rule - which is the
    // right state for a slot the build never fills.
    std::memset(table, 0, sizeof(epte) * entries_per_table);

    return table;
}

std::expected<void, zpp::error>
hypervisor::install_shadow_leaf(std::size_t cpu,
                                std::uint64_t guest_physical,
                                const ept_walk_result & guest,
                                std::uint64_t shift)
{
    auto composition = compose_ept(guest,
                                   host_ept_lookup(guest.physical_address),
                                   execute_only_translations_offered);

    // Anything not composable is left absent in the shadow, and that is
    // deliberate rather than a gap: an absent entry faults, and the fault
    // path walks the guest hypervisor's tables again and decides then
    // whether to reflect a violation, reflect a misconfiguration, or
    // handle it here. Building that decision into the table would freeze
    // an answer that depends on state - a watched page's permissions
    // change while the guest runs - so the table holds only what is
    // unconditionally true.
    if (ept_compose_outcome::composed != composition.outcome) {
        return {};
    }

    // Never larger than both walks agreed on, and never larger than the
    // caller is installing at. A 2 MB leaf over a composition valid for
    // only 4 KB would grant a whole region the permissions of one page,
    // which is the silent privilege escalation nested EPT exists to
    // prevent.
    if (composition.page_shift < shift) {
        return install_shadow_split(cpu, guest_physical, guest);
    }

    auto entry = shadow_ept_entry(cpu, guest_physical, shift);
    if (!entry) {
        return std::unexpected(entry.error());
    }

    epte leaf;
    composition.permissions.apply_to(leaf);
    leaf.type(composition.type);

    if (page_shift_4kb != shift) {
        leaf.large(true);
    }

    // The address is masked to the page size rather than trusted. The low
    // bits of a large entry's address field are reserved and must be zero
    // - SDM Table 31-5 reserves bits 20:12 of a 2 MB entry - and a set
    // reserved bit is a misconfiguration rather than a wrong address.
    auto page = composition.physical_address & ~((1ull << shift) - 1);
    leaf = epte(leaf.value() | page);

    **entry = leaf;
    return {};
}

std::expected<void, zpp::error>
hypervisor::install_shadow_split(std::size_t cpu,
                                 std::uint64_t guest_physical,
                                 const ept_walk_result & guest)
{
    // A 2 MB region the composition could not describe with one entry,
    // filled one page at a time. The guest hypervisor's side is uniform
    // across the region - a caller only reaches here with a large or
    // coarsened mapping - so what varies is ours, which is why each page
    // is composed against a fresh lookup while the guest walk is reused
    // with its address adjusted.
    auto region = guest_physical & ~((1ull << page_shift_2mb) - 1);

    this->shadow_ept_splits[cpu] = this->shadow_ept_splits[cpu] + 1;

    for (std::size_t i{}; i < entries_per_table; ++i) {
        auto page = region + (i * (1ull << page_shift_4kb));

        auto within = guest;
        within.page_shift = page_shift_4kb;
        within.physical_address =
            (guest.physical_address & ~((1ull << guest.page_shift) - 1)) |
            (page & ((1ull << guest.page_shift) - 1));

        if (auto result =
                install_shadow_leaf(cpu, page, within, page_shift_4kb);
            !result) {
            return result;
        }
    }

    return {};
}

std::expected<arch::x86_64::vmx::epte *, zpp::error>
hypervisor::shadow_ept_entry(std::size_t cpu,
                             std::uint64_t guest_physical,
                             std::uint64_t shift)
{
    // Down from the root, making tables as needed. Four-level only, which
    // the capability MSR promises: IA32_VMX_EPT_VPID_CAP reports a
    // page-walk length of four and not five, and SDM 29.2.1.1 then checks
    // the guest hypervisor's own pointer against that on its behalf.
    auto * table =
        this->shadow_epml4[cpu][this->shadow_ept_current_slot[cpu]];

    for (auto level = std::uint64_t{3};; --level) {
        auto index = (guest_physical >> (12 + (9 * level))) & 0x1ff;
        auto & entry = table[index];

        if ((12 + (9 * level)) == shift) {
            return &entry;
        }

        // A larger mapping already covering this address, where a
        // smaller one is now wanted.
        //
        // Impossible while the shadow was built top down in one pass, and
        // routine now that it is filled a fault at a time: a 2 MB
        // mapping goes in first, and a later fault inside it needs 4 KB
        // because this VMM's own tables split that region or watch a page
        // of it. Descending through the large entry instead treats a
        // guest frame number as if it addressed a table - which is
        // exactly what it did, and the walk then failed to find that
        // "table" among the module's pages and stopped the processor.
        // Measured: `could not install a shadow leaf for 0x11c4c9000:
        // error 0x4`, followed by an unhandled EPT violation.
        //
        // Dropped rather than split into a full table of equivalents.
        // The addresses it covered lose their mapping and fault back in
        // one at a time, which is what this shadow does with everything
        // else and costs a fault each; reconstructing 512 entries here
        // would cost the same walk this design exists to avoid.
        if (ept_permissions::of(entry).present() && entry.large()) {
            entry = epte{};
        }

        if (!ept_permissions::of(entry).present()) {
            auto created = shadow_ept_table(cpu);
            if (!created) {
                return std::unexpected(created.error());
            }

            epte parent;
            parent.read(true);
            parent.write(true);
            parent.execute(true);
            parent.execute_user(true);
            parent.page_number(
                this->host_page_table.virtual_to_physical(*created) >> 12);

            // The memory type is deliberately left zero. In an entry that
            // references another table those bits are not a type but
            // reserved
            // - SDM Table 31-6, "bits 6:3 Reserved (must be 0)" - and a
            // reserved value set here is an EPT misconfiguration rather
            // than a wrong memory type. initialize_ept comments on the
            // same rule for its own splits.
            entry = parent;
        }

        auto next = entry.page_number() << 12;
        auto found = this->module_physical_to_virtual.find(next);
        if (this->module_physical_to_virtual.end() == found) {
            return std::unexpected(
                zpp::error{error::physical_to_virtual_capacity_error});
        }

        table = reinterpret_cast<epte *>(found->second);
    }
}

std::expected<std::uint64_t, zpp::error>
hypervisor::shadow_ept_pointer_for(std::size_t cpu, std::uint64_t eptp12)
{
    auto root = eptp12 & (((1ull << 52) - 1) & ~0xfffull);

    // Rebuilt when no shadow this processor holds was built from this
    // pointer, and when this VMM's own tables have moved under the one
    // that was. The second is the case that would otherwise be silent: a
    // page watch armed or a region protected changes permissions the
    // shadow already composed, and a shadow built before that would go on
    // granting what our tables no longer do.
    auto generation = this->ept_generation.load(std::memory_order_acquire);

    for (std::size_t slot{}; slot < shadow_ept_slots; ++slot) {
        if ((root != this->shadow_ept_source[cpu][slot]) ||
            (generation != this->shadow_ept_generation_seen[cpu][slot])) {
            continue;
        }

        // A guest hypervisor switching between its guests' tables comes
        // back here, and this is the switch being free rather than
        // costing a walk of the whole address space. KVM answers the same
        // question with nested_ept_root_matches over its cached roots.
        this->shadow_ept_current_slot[cpu] = slot;
        this->shadow_ept_cache_hits[cpu] =
            this->shadow_ept_cache_hits[cpu] + 1;
        return this->shadow_ept_pointer[cpu][slot];
    }

    // Nothing matched, so one has to be built. A slot never used is taken
    // first; otherwise the round robin victim is replaced, and that
    // replacement is counted - a set that is too small shows up as
    // evictions rather than as a mystery.
    auto chosen = shadow_ept_slots;
    for (std::size_t slot{}; slot < shadow_ept_slots; ++slot) {
        if (0 == this->shadow_ept_source[cpu][slot]) {
            chosen = slot;
            break;
        }
    }

    if (shadow_ept_slots == chosen) {
        chosen = this->shadow_ept_next_victim[cpu] % shadow_ept_slots;
        this->shadow_ept_next_victim[cpu] = chosen + 1;
        this->shadow_ept_evictions[cpu] =
            this->shadow_ept_evictions[cpu] + 1;
    }

    // Emptied rather than built, which is the whole of the change from
    // walking the guest hypervisor's tables up front.
    //
    // Nothing is composed here. The shadow starts with every entry
    // not-present, every access to it faults, and the fault installs the
    // one mapping it needed - which is what KVM does, its nested EPT
    // being the shadow MMU filled by kvm_mmu_page_fault rather than a
    // structure built ahead of use.
    //
    // What the eager build cost, measured on the rig: a guest hypervisor
    // that unmapped 511 pages issued INVEPT and this VMM walked
    // twenty-two thousand regions to answer it, hundreds of times over -
    // visible in the log as the same root rebuilt with the region count
    // ticking down 0x5d18, 0x5b19, 0x591a. It also had to guess how much
    // of the address space the second-level guest would touch, and the
    // answer was "far less than all of it".
    release_shadow_slot(cpu, chosen);
    std::memset(this->shadow_epml4[cpu][chosen],
                0,
                sizeof(epte) * entries_per_table);

    arch::x86_64::vmx::ept_pointer pointer;
    pointer.memory_type(memory_type::write_back);
    pointer.page_walk_length(4);
    pointer.page_number(this->host_page_table.virtual_to_physical(
                            this->shadow_epml4[cpu][chosen]) >>
                        12);

    this->shadow_ept_pointer[cpu][chosen] = pointer;
    this->shadow_ept_source[cpu][chosen] = root;
    this->shadow_ept_generation_seen[cpu][chosen] = generation;
    this->shadow_ept_current_slot[cpu] = chosen;
    this->shadow_ept_builds[cpu] = this->shadow_ept_builds[cpu] + 1;

    // This processor may hold mappings from whatever was in this slot
    // before, against a pointer that has just been reused.
    invalidate_ept_locally();

    return this->shadow_ept_pointer[cpu][chosen];
}

std::expected<void, zpp::error> hypervisor::fill_shadow_leaf(
    std::size_t cpu,
    std::uint64_t guest_physical,
    const arch::x86_64::vmx::ept_walk_result & guest,
    std::uint64_t shift)
{
    auto installed =
        install_shadow_leaf(cpu, guest_physical, guest, shift);
    if (installed) {
        return {};
    }

    if (static_cast<int>(error::out_of_shadow_ept_tables) !=
        installed.error().code()) {
        return installed;
    }

    // The other slots first. Each is a shadow of a guest the guest
    // hypervisor is not running at this instant, so losing one costs the
    // faults to fill it again and nothing else.
    auto current = this->shadow_ept_current_slot[cpu];
    for (std::size_t slot{}; slot < shadow_ept_slots; ++slot) {
        if (slot != current) {
            release_shadow_slot(cpu, slot);
        }
    }

    this->shadow_ept_reclaims[cpu] = this->shadow_ept_reclaims[cpu] + 1;

    installed = install_shadow_leaf(cpu, guest_physical, guest, shift);
    if (installed) {
        return {};
    }

    if (static_cast<int>(error::out_of_shadow_ept_tables) !=
        installed.error().code()) {
        return installed;
    }

    // Still not enough, so this shadow alone has outgrown the pool.
    // Resetting it loses every mapping filled so far and they fault back
    // in, which is slow and is the only answer that makes progress -
    // where refusing would stop the processor. Counted separately,
    // because one of these means the pool is too small for a single
    // shadow and no amount of slot juggling will help.
    release_shadow_slot(cpu, current);
    std::memset(this->shadow_epml4[cpu][current],
                0,
                sizeof(epte) * entries_per_table);

    this->shadow_ept_resets[cpu] = this->shadow_ept_resets[cpu] + 1;

    log("cpu {} shadow ept reset: one shadow does not fit the pool", cpu);

    return install_shadow_leaf(cpu, guest_physical, guest, shift);
}

void hypervisor::release_shadow_slot(std::size_t cpu, std::size_t slot)
{
    if (slot >= shadow_ept_slots) {
        return;
    }

    this->shadow_ept_source[cpu][slot] = 0;
    this->shadow_ept_generation_seen[cpu][slot] = 0;

    for (std::size_t i{}; i < shadow_ept_tables_per_cpu; ++i) {
        if ((slot + 1) == this->shadow_ept_table_slot[cpu][i]) {
            this->shadow_ept_table_slot[cpu][i] = shadow_table_free;
        }
    }

    this->shadow_ept_tables_used[cpu][slot] = 0;
}

void hypervisor::discard_shadow_ept_for(std::size_t cpu,
                                        std::uint64_t root)
{
    for (std::size_t slot{}; slot < shadow_ept_slots; ++slot) {
        if (root == this->shadow_ept_source[cpu][slot]) {
            release_shadow_slot(cpu, slot);
        }
    }
}

void hypervisor::discard_shadow_ept(std::size_t cpu)
{
    // Every slot, because INVEPT's all-context type names them all and the
    // single-context type names one this VMM cannot distinguish from the
    // others without keeping the guest hypervisor's own pointer per slot
    // - which it does, but discarding more than was asked for is the safe
    // direction and SDM 31.4.3.2 permits it outright.
    //
    // Zeroing the source is enough to force a rebuild, and is cheaper than
    // rebuilding here: the next entry needs the shadow, and nothing
    // between now and then reads it. Deliberately not freeing the tables,
    // which the rebuild does for the slot it takes.
    for (std::size_t slot{}; slot < shadow_ept_slots; ++slot) {
        release_shadow_slot(cpu, slot);
    }
}

} // namespace zpp::hypervisor
