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
 * Whether a run of 4 KB entries is equivalent to one large page.
 *
 * The shadow has to reproduce the *effective* translation, not the table
 * structure that produced it, so a guest hypervisor that maps two
 * megabytes as five hundred and twelve contiguous pages with identical
 * permissions has described exactly what one large entry describes.
 * Coarsening it is an equivalence, not an approximation.
 *
 * It is also what keeps eager construction affordable. Without it, a guest
 * hypervisor using 4 KB tables over ordinary contiguous memory - which is
 * the common case - would need one shadow leaf table per two megabytes,
 * and sixteen gigabytes of that is eight thousand tables where the
 * coarsened form needs sixteen.
 *
 * Every one of the four conditions is necessary. Permissions and memory
 * type must match because a large entry has one of each for the whole
 * range; contiguity because a large entry names one base address; and
 * presence because an absent entry inside the run is a hole the guest
 * hypervisor is entitled to fault on.
 */
constexpr std::optional<ept_walk_result>
coarsened(const guest_table & table, std::uint64_t guest_physical)
{
    auto first = ept_permissions::of(table.entries[0]);
    if (!first.present()) {
        return std::nullopt;
    }

    auto base = table.entries[0].page_number();
    auto type = table.entries[0].type();

    for (std::size_t i = 1; i < entries_per_table; ++i) {
        const auto & entry = table.entries[i];

        if (ept_permissions::of(entry) != first) {
            return std::nullopt;
        }

        if (entry.page_number() != (base + i)) {
            return std::nullopt;
        }

        if (entry.type() != type) {
            return std::nullopt;
        }
    }

    ept_walk_result result;
    result.status = ept_walk_status::mapped;
    result.page_shift = page_shift_2mb;
    result.permissions = first;
    result.type = type;
    result.physical_address =
        (base << page_shift_4kb) |
        (guest_physical & ((1ull << page_shift_2mb) - 1));

    return result;
}

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

std::expected<void, zpp::error>
hypervisor::build_shadow_ept(std::size_t cpu, std::uint64_t eptp12)
{
    auto & vmcs = this->vmcs;
    static_cast<void>(vmcs);

    // A fresh shadow every time, which is what makes this design the
    // simple one: there is no incremental state to keep consistent with
    // the guest hypervisor's tables, so nothing can be stale. The root is
    // reused rather than reallocated, keeping the EPT pointer stable -
    // SDM 31.4.2 associates cached mappings with bits 51:12 of the
    // pointer, so a stable root plus the global invalidation at the end is
    // a complete story.
    auto slot = this->shadow_ept_current_slot[cpu];

    std::memset(this->shadow_epml4[cpu][slot],
                0,
                sizeof(epte) * entries_per_table);

    // This slot's tables only. The others belong to shadows that are
    // still valid, which is the entire point of keeping more than one.
    for (std::size_t i{}; i < shadow_ept_tables_per_cpu; ++i) {
        if ((slot + 1) == this->shadow_ept_table_slot[cpu][i]) {
            this->shadow_ept_table_slot[cpu][i] = shadow_table_free;
        }
    }

    this->shadow_ept_tables_used[cpu][slot] = 0;
    this->shadow_ept_regions_built[cpu] = 0;
    this->shadow_ept_splits[cpu] = 0;

    auto bits = physical_address_bits();
    auto root = eptp12 & (((1ull << 52) - 1) & ~0xfffull);

    // Read a whole table at a time. Per entry would cost a window mapping,
    // an invlpg and a lock acquisition each - see guest_table.
    auto read_table = [&](std::uint64_t at, guest_table & into) -> bool {
        return read_guest_physical(
                   at,
                   std::span(reinterpret_cast<std::byte *>(&into),
                             sizeof(into)))
            .has_value();
    };

    // Whether an entry the descent found is one to stop at, taking the
    // architecture's rules from the walker rather than restating them.
    auto rejected = [&](const epte & entry,
                        std::uint64_t level,
                        bool leaf) {
        return arch::x86_64::vmx::ept_walk::misconfigured(
            entry, level, leaf, bits, execute_only_translations_offered);
    };

    guest_table pml4;
    if (!read_table(root, pml4)) {
        return std::unexpected(
            zpp::error{error::guest_memory_unreachable});
    }

    // Descend the guest hypervisor's own tables rather than sweeping the
    // address space, so the cost is proportional to what it actually
    // mapped instead of to how much could be addressed. Sweeping 512 GB at
    // 2 MB would be two hundred and sixty thousand walks per launch
    // whatever the guest had mapped.
    for (std::size_t a{}; a < entries_per_table; ++a) {
        auto pml4_permissions = ept_permissions::of(pml4.entries[a]);
        if (!pml4_permissions.present() ||
            rejected(pml4.entries[a], 3, false)) {
            continue;
        }

        guest_table pdpt;
        if (!read_table(pml4.entries[a].page_number() << 12, pdpt)) {
            continue;
        }

        for (std::size_t b{}; b < entries_per_table; ++b) {
            const auto & pdpte = pdpt.entries[b];
            auto pdpt_permissions = pml4_permissions.intersected_with(
                ept_permissions::of(pdpte));

            if (!ept_permissions::of(pdpte).present()) {
                continue;
            }

            auto gigabyte = (a << 39) | (b << page_shift_1gb);

            if (pdpte.large()) {
                if (rejected(pdpte, 2, true)) {
                    continue;
                }

                // A one gigabyte mapping, installed as five hundred and
                // twelve 2 MB entries rather than one. This VMM's own
                // tables are 2 MB granular, so nothing coarser can be
                // composed without asserting a uniformity our side has not
                // been checked for.
                for (std::size_t i{}; i < entries_per_table; ++i) {
                    auto at = gigabyte + (i * (1ull << page_shift_2mb));
                    auto guest = leaf_result(
                        pdpte, at, page_shift_1gb, pml4_permissions);
                    guest.page_shift = page_shift_2mb;

                    if (auto result = install_shadow_leaf(
                            cpu, at, guest, page_shift_2mb);
                        !result) {
                        return result;
                    }

                    this->shadow_ept_regions_built[cpu] =
                        this->shadow_ept_regions_built[cpu] + 1;
                }
                continue;
            }

            if (rejected(pdpte, 2, false)) {
                continue;
            }

            guest_table page_directory;
            if (!read_table(pdpte.page_number() << 12, page_directory)) {
                continue;
            }

            for (std::size_t c{}; c < entries_per_table; ++c) {
                const auto & pde = page_directory.entries[c];
                if (!ept_permissions::of(pde).present()) {
                    continue;
                }

                auto at = gigabyte + (c << page_shift_2mb);

                if (pde.large()) {
                    if (rejected(pde, 1, true)) {
                        continue;
                    }

                    auto guest = leaf_result(
                        pde, at, page_shift_2mb, pdpt_permissions);

                    if (auto result = install_shadow_leaf(
                            cpu, at, guest, page_shift_2mb);
                        !result) {
                        return result;
                    }

                    this->shadow_ept_regions_built[cpu] =
                        this->shadow_ept_regions_built[cpu] + 1;
                    continue;
                }

                if (rejected(pde, 1, false)) {
                    continue;
                }

                guest_table page_table;
                if (!read_table(pde.page_number() << 12, page_table)) {
                    continue;
                }

                auto within = ept_permissions::of(pde).intersected_with(
                    pdpt_permissions);

                // The coarsening that keeps this affordable. A region the
                // guest hypervisor described with five hundred and twelve
                // contiguous, identically permitted pages is exactly what
                // one large entry describes, so it is installed as one.
                if (auto uniform = coarsened(page_table, at)) {
                    uniform->permissions =
                        within.intersected_with(uniform->permissions);

                    if (auto result = install_shadow_leaf(
                            cpu, at, *uniform, page_shift_2mb);
                        !result) {
                        return result;
                    }

                    this->shadow_ept_regions_built[cpu] =
                        this->shadow_ept_regions_built[cpu] + 1;
                    continue;
                }

                this->shadow_ept_splits[cpu] =
                    this->shadow_ept_splits[cpu] + 1;

                for (std::size_t d{}; d < entries_per_table; ++d) {
                    const auto & pte = page_table.entries[d];
                    if (!ept_permissions::of(pte).present() ||
                        rejected(pte, 0, true)) {
                        continue;
                    }

                    auto page = at + (d << page_shift_4kb);
                    auto guest =
                        leaf_result(pte, page, page_shift_4kb, within);

                    if (auto result = install_shadow_leaf(
                            cpu, page, guest, page_shift_4kb);
                        !result) {
                        return result;
                    }

                    this->shadow_ept_regions_built[cpu] =
                        this->shadow_ept_regions_built[cpu] + 1;
                }
            }
        }
    }

    arch::x86_64::vmx::ept_pointer pointer;
    pointer.memory_type(memory_type::write_back);
    pointer.page_walk_length(4);
    pointer.page_number(this->host_page_table.virtual_to_physical(
                            this->shadow_epml4[cpu][slot]) >>
                        12);

    this->shadow_ept_pointer[cpu][slot] = pointer;
    this->shadow_ept_source[cpu][slot] = root;
    this->shadow_ept_generation_seen[cpu][slot] =
        this->ept_generation.load(std::memory_order_acquire);

    // Globally, and without trying to be clever about scope. The shadow's
    // every entry has just been rewritten, and SDM 31.4.3.2 permits a
    // processor to "invalidate any cached mappings at any time" - so the
    // broad answer is an architecturally sanctioned one rather than a
    // shortcut, and a narrower one would need per-address bookkeeping that
    // buys nothing at build time.
    invalidate_ept_locally();

    log("cpu {} shadow ept built from {} into slot {}: {} regions, {} "
        "splits, {} tables",
        cpu,
        root,
        slot,
        this->shadow_ept_regions_built[cpu],
        this->shadow_ept_splits[cpu],
        this->shadow_ept_tables_used[cpu][slot]);

    return {};
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

    this->shadow_ept_current_slot[cpu] = chosen;
    this->shadow_ept_builds[cpu] = this->shadow_ept_builds[cpu] + 1;

    if (auto result = build_shadow_ept(cpu, eptp12); !result) {
        // A failed build leaves the slot unusable, and saying it holds
        // the pointer it failed on would hand it out next time.
        this->shadow_ept_source[cpu][chosen] = 0;
        return std::unexpected(result.error());
    }

    return this->shadow_ept_pointer[cpu][chosen];
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
        this->shadow_ept_source[cpu][slot] = 0;
    }
}

} // namespace zpp::hypervisor
