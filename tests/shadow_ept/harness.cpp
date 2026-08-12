/*
 * The shadow extended page table fault path: the real nested_ept.cpp,
 * compiled natively against the real hypervisor class.
 *
 * This is the half of nested EPT that has state. tests/nested_ept covers
 * the rules, which are pure; everything here is about the pool, the slots,
 * the tables that get made and freed, and the one property that decides
 * whether a second-level guest makes progress at all:
 *
 *     **a leaf installed for a faulting access must permit that access.**
 *
 * Every path in `on_l2_ept_fault` that installs a leaf then resumes the
 * guest. If the leaf does not permit what faulted, the guest faults again
 * at the same RIP on the same address, and nothing in the VMM notices -
 * `l2_exits_handled` climbs, the log says the fault was handled, and the
 * guest never retires another instruction. That livelock has been reached
 * twice on real hardware, once as qualification 0x1aa on a watched page
 * and once as 0x184 on an instruction fetch, and in both cases the
 * handler believed it had succeeded. `shadow_ept_lookup` is the oracle
 * that turns "handled" into a checkable claim, and it is used by almost
 * every case below.
 *
 * What is real and what is not. The class, the shadow pool, the slot
 * allocator, the leaf installer, the splitter, the collector, the refresh
 * and the host-side lookup are the hypervisor's own, compiled from
 * hypervisor/src/hypervisor/nested_ept.cpp. The host page table and its
 * walker are the real ones out of arch/x86_64/page_table.cpp. What the
 * harness supplies is *input*: the VMM's own extended page tables, the
 * guest hypervisor's, and the guest-physical memory the latter lives in.
 *
 * The one thing worth being uneasy about is that the VMM's own tables are
 * built here rather than by `initialize_ept`, which lives in a translation
 * unit that pulls in the whole VMM. They are built to the same shape - a
 * complete identity map of the first 512 GB as 2 MB leaves - and
 * `host_ept_lookup` documents that shape as what it indexes rather than
 * walks, so the fixture is written against the same statement the code is.
 * A change to that shape breaks both, which is the property that matters.
 */
#include "support/identity_page_table.h"
#include "zpp/hypervisor/hypervisor.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <print>
#include <vector>

namespace
{

/**
 * The report. Counted rather than aborted on, so one broken rule does not
 * hide the rest.
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
 * The guest hypervisor's memory, a page at a time.
 *
 * `refresh_shadow_ept_for` is the one function here that reads it: it
 * re-walks the guest hypervisor's own extended page tables to find out
 * what a mapping should now be. So this is where those tables live, and
 * the walk that reads them is the real one.
 */
std::map<std::uint64_t, std::vector<std::byte>> g_guest_pages;

std::vector<std::byte> & guest_page_of(std::uint64_t physical)
{
    constexpr std::uint64_t page_offset_mask = 0xfff;
    constexpr std::size_t page_bytes = 4096;

    auto base = physical & ~page_offset_mask;
    auto found = g_guest_pages.find(base);
    if (g_guest_pages.end() == found) {
        found =
            g_guest_pages.emplace(base, std::vector<std::byte>(page_bytes))
                .first;
    }
    return found->second;
}

/**
 * How many times the VMM asked for a local extended-page-table
 * invalidation.
 *
 * Counted rather than discarded, because "the shadow entry changed" and
 * "this processor was told about it" are separate claims and the second
 * one is invisible from the tables. A leaf installed without an
 * invalidation is a leaf the processor may not use, which is the same
 * livelock by another route.
 */
std::size_t g_invalidations{};

} // namespace

namespace zpp::hypervisor
{

hypervisor & hypervisor::instance()
{
    static hypervisor the;
    return the;
}

std::expected<void, zpp::error> hypervisor::read_guest_physical(
    std::uint64_t physical, std::span<std::byte> into)
{
    constexpr std::uint64_t page_offset_mask = 0xfff;
    constexpr std::size_t page_bytes = 4096;

    // A page at a time, because a read may straddle one - which is what
    // the real implementation does and what the callers of this rely on.
    std::size_t done{};
    while (done < into.size()) {
        auto at = physical + done;
        auto offset = at & page_offset_mask;
        auto count = into.size() - done;
        if (count > (page_bytes - offset)) {
            count = page_bytes - offset;
        }

        std::memcpy(
            into.data() + done, guest_page_of(at).data() + offset, count);
        done += count;
    }

    return {};
}

void hypervisor::invalidate_ept_locally()
{
    ++g_invalidations;
}

} // namespace zpp::hypervisor

namespace
{

zpp::hypervisor::hypervisor & hv()
{
    return zpp::hypervisor::hypervisor::instance();
}

constexpr std::size_t entries_per_table = 512;
constexpr std::uint64_t page_bytes = 4096;

/**
 * The page shifts, SDM 31.3.2.
 * @{
 */
constexpr std::uint64_t shift_4kb = 12;
constexpr std::uint64_t shift_2mb = 21;
constexpr std::uint64_t shift_1gb = 30;
/**
 * @}
 */

constexpr std::uint64_t bytes_2mb = 1ull << shift_2mb;
constexpr std::uint64_t bytes_1gb = 1ull << shift_1gb;

/**
 * The processor these cases are judged against.
 *
 * The shim `cpuid` answers zero, so `physical_address_bits` takes its
 * documented floor of 36 - which is the architectural minimum for
 * physical addressing and is therefore the width that refuses the most
 * while remaining possible. Written down here because the reserved-bit
 * cases below depend on it and a silent change would turn them into
 * different cases.
 */
constexpr std::uint64_t expected_physical_address_bits = 36;

/**
 * The processor this suite drives. One, because a shadow pool is
 * per-processor and nothing here is about two of them sharing.
 */
constexpr std::size_t cpu = 0;

/**
 * Guest-physical addresses used by the cases, chosen so that no two share
 * a page directory, a page-directory-pointer entry or a 2 MB region unless
 * a case is about their sharing.
 * @{
 */
constexpr std::uint64_t address_a = 0x11223000;
constexpr std::uint64_t address_b = 0x22446000;
constexpr std::uint64_t address_in_a_region = address_a + (7 * page_bytes);
/**
 * @}
 */

/**
 * The guest hypervisor's extended page table roots the cases use, as
 * physical addresses in its own guest-physical space.
 *
 * Distinct pages, because `shadow_ept_pointer_for` keys a slot on bits
 * 51:12 of the pointer - SDM 31.4.2 defines the EPTRTA that way - so two
 * roots differing anywhere below bit 12 are the same shadow.
 * @{
 */
constexpr std::uint64_t root_1 = 0x1000000;
constexpr std::uint64_t root_2 = 0x1001000;
constexpr std::uint64_t root_3 = 0x1002000;
constexpr std::uint64_t root_4 = 0x1003000;
constexpr std::uint64_t root_5 = 0x1004000;
/**
 * @}
 */

/**
 * A page directory of this VMM's own that a case has split, so a 4 KB
 * entry of ours can sit under a 2 MB entry of the guest hypervisor's.
 *
 * Registered in `module_physical_to_virtual` beside the shadow pool,
 * because `host_ept_lookup` reaches a split table through that map for
 * exactly the reason the module is not identity mapped.
 */
alignas(page_bytes) zpp::arch::x86_64::vmx::epte
    g_split_page_table[entries_per_table]{};

zpp::arch::x86_64::vmx::epte
leaf(std::uint64_t physical,
     std::uint64_t shift,
     const zpp::arch::x86_64::vmx::ept_permissions & permissions,
     zpp::arch::x86_64::memory_type type =
         zpp::arch::x86_64::memory_type::write_back)
{
    zpp::arch::x86_64::vmx::epte entry;
    permissions.apply_to(entry);
    entry.type(type);
    if (shift_4kb != shift) {
        entry.large(true);
    }
    return zpp::arch::x86_64::vmx::epte(
        entry.value() | (physical & ~((1ull << shift) - 1)));
}

zpp::arch::x86_64::vmx::epte reference(std::uint64_t child_physical)
{
    zpp::arch::x86_64::vmx::epte entry;
    zpp::arch::x86_64::vmx::ept_permissions::all().apply_to(entry);
    entry.page_number(child_physical >> shift_4kb);
    return entry;
}

/**
 * This VMM's own extended page tables, in the shape `initialize_ept`
 * builds and `host_ept_lookup` indexes: a complete identity map of the
 * first 512 GB, as one page-map level-4 entry over 512 page-directory
 * -pointer entries over 2 MB leaves.
 *
 * Rebuilt between cases that damage it, so a case cannot inherit a hole
 * another one made.
 */
void build_host_ept()
{
    zpp::arch::x86_64::vmx::ept_permissions::all().apply_to(hv().epml4[0]);
    hv().epml4[0].page_number(
        hv().host_page_table.virtual_to_physical(hv().epdpt) >> shift_4kb);

    for (std::size_t gigabyte{}; gigabyte < entries_per_table;
         ++gigabyte) {
        zpp::arch::x86_64::vmx::ept_permissions::all().apply_to(
            hv().epdpt[gigabyte]);
        hv().epdpt[gigabyte].page_number(
            hv().host_page_table.virtual_to_physical(hv().epd[gigabyte]) >>
            shift_4kb);

        for (std::size_t region{}; region < entries_per_table; ++region) {
            hv().epd[gigabyte][region] =
                leaf((gigabyte * bytes_1gb) + (region * bytes_2mb),
                     shift_2mb,
                     zpp::arch::x86_64::vmx::ept_permissions::all());
        }
    }
}

/**
 * The 2 MB entry of ours covering a physical address.
 */
zpp::arch::x86_64::vmx::epte & host_region_of(std::uint64_t physical)
{
    return hv()
        .epd[physical >> shift_1gb][(physical >> shift_2mb) & 0x1ff];
}

/**
 * A walk result standing for the guest hypervisor's own mapping of an
 * address, built rather than walked.
 *
 * `install_shadow_leaf` takes the guest walk as a parameter, so a case
 * that is about installation supplies one directly; the cases about
 * refreshing build real tables in guest memory instead, because that is
 * the path that walks them.
 */
zpp::arch::x86_64::vmx::ept_walk_result
guest_mapping(std::uint64_t physical,
              std::uint64_t shift,
              const zpp::arch::x86_64::vmx::ept_permissions & permissions =
                  zpp::arch::x86_64::vmx::ept_permissions::all())
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = zpp::arch::x86_64::vmx::ept_walk_status::mapped;
    result.physical_address = physical;
    result.page_shift = shift;
    result.permissions = permissions;
    result.type = zpp::arch::x86_64::memory_type::write_back;
    return result;
}

/**
 * Puts every processor's shadow state back to what a freshly launched VMM
 * has, so cases do not inherit each other's slots.
 */
void reset_shadows()
{
    for (std::size_t slot{};
         slot < zpp::hypervisor::hypervisor::shadow_ept_slots;
         ++slot) {
        hv().release_shadow_slot(cpu, slot);
    }

    hv().shadow_ept_current_slot[cpu] = 0;
    hv().shadow_ept_next_victim[cpu] = 0;
    hv().shadow_ept_cache_hits[cpu] = 0;
    hv().shadow_ept_builds[cpu] = 0;
    hv().shadow_ept_evictions[cpu] = 0;
    hv().shadow_ept_reclaims[cpu] = 0;
    hv().shadow_ept_resets[cpu] = 0;
    hv().shadow_ept_splits[cpu] = 0;
    hv().shadow_ept_refreshes[cpu] = 0;
    hv().shadow_ept_refresh_leaves[cpu] = 0;
    hv().shadow_ept_refresh_overflows[cpu] = 0;

    build_host_ept();
    g_invalidations = 0;
}

/**
 * Takes a slot for a root and returns it, so a case that is about
 * installation does not have to care how a slot is chosen.
 */
std::size_t take_slot(std::uint64_t root)
{
    auto pointer = hv().shadow_ept_pointer_for(cpu, root);
    check_true("fixture.slot_taken", pointer.has_value());
    return hv().shadow_ept_current_slot[cpu];
}

/**
 * How many tables of the shared pool this slot currently owns.
 */
std::size_t tables_owned_by(std::size_t slot)
{
    std::size_t owned{};
    for (std::size_t i{};
         i < zpp::hypervisor::hypervisor::shadow_ept_tables_per_cpu;
         ++i) {
        if ((slot + 1) == hv().shadow_ept_table_slot[cpu][i]) {
            ++owned;
        }
    }
    return owned;
}

// === host_ept_lookup
// =====================================================

void test_host_ept_lookup()
{
    reset_shadows();

    check("host_ept.physical_address_bits",
          expected_physical_address_bits,
          hv().physical_address_bits());

    // An ordinary address inside the identity map, answered from the 2 MB
    // leaf without a walk.
    auto found = hv().host_ept_lookup(address_a);

    check("host_ept.identity.status",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(found.status));
    check("host_ept.identity.page_shift", shift_2mb, found.page_shift);
    check("host_ept.identity.physical_address",
          address_a,
          found.physical_address);
    check_true("host_ept.identity.permits_everything",
               found.permissions.read() && found.permissions.write() &&
                   found.permissions.execute());

    // Past the identity map. 512 GB is the whole of what `initialize_ept`
    // builds, and an address beyond it has no entry to find rather than an
    // absent one - which is a distinction the caller does not need and the
    // lookup therefore does not make.
    constexpr std::uint64_t past_the_identity_map = 512ull * bytes_1gb;

    check("host_ept.past_the_identity_map",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().host_ept_lookup(past_the_identity_map).status));

    check_false(
        "host_ept.past_the_identity_map_grants_nothing",
        hv().host_ept_lookup(past_the_identity_map).permissions.present());

    // A region this VMM has taken write permission away from, which is
    // what watching a page does - and the case that decides whether a
    // second-level guest's write to a watched page is answered here or
    // reflected.
    host_region_of(address_b) = leaf(
        address_b & ~(bytes_2mb - 1),
        shift_2mb,
        zpp::arch::x86_64::vmx::ept_permissions(true, false, true, true));

    auto watched = hv().host_ept_lookup(address_b);
    check_true("host_ept.watched_region_keeps_read",
               watched.permissions.read());
    check_false("host_ept.watched_region_loses_write",
                watched.permissions.write());

    // A region absent altogether, which is what the module's own pages
    // look like.
    host_region_of(address_b) = zpp::arch::x86_64::vmx::epte{};
    check("host_ept.absent_region",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().host_ept_lookup(address_b).status));

    build_host_ept();

    // A 2 MB region this VMM has split into 4 KB pages, reached through
    // `module_physical_to_virtual` rather than dereferenced - the module
    // is not identity mapped in the host page table, which is why the
    // reverse map exists at all.
    auto region_base = address_b & ~(bytes_2mb - 1);

    for (std::size_t i{}; i < entries_per_table; ++i) {
        g_split_page_table[i] =
            leaf(region_base + (i * page_bytes),
                 shift_4kb,
                 zpp::arch::x86_64::vmx::ept_permissions::all());
    }

    // One page of the split denied, which is the shape a watched page
    // really has: 4 KB, not a whole 2 MB region.
    auto denied_index = (address_b >> shift_4kb) & 0x1ff;
    g_split_page_table[denied_index] = zpp::arch::x86_64::vmx::epte{};

    host_region_of(address_b) = reference(
        hv().host_page_table.virtual_to_physical(g_split_page_table));

    check("host_ept.split.page_shift",
          shift_4kb,
          hv().host_ept_lookup(address_b + page_bytes).page_shift);

    check("host_ept.split.denied_page",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(
              hv().host_ept_lookup(address_b).status));

    // The denied page is *mapped with no permissions* rather than
    // reported absent, because the lookup indexes rather than walks and
    // an all-zero leaf is still a leaf to it. What makes that safe is the
    // composition: an empty intersection is host-denied, which is the case
    // asserted in tests/nested_ept. Recorded here so the two halves of
    // that argument sit beside each other.
    check_false("host_ept.split.denied_page_grants_nothing",
                hv().host_ept_lookup(address_b).permissions.present());

    // A split whose table this VMM cannot find is not the same as an
    // absent one: it means the tree is not what it is assumed to be, and
    // refusing is the only safe answer.
    host_region_of(address_b) = reference(0xdeadb000);

    check("host_ept.split_table_not_in_the_module_map",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().host_ept_lookup(address_b).status));

    build_host_ept();
}

// === shadow_ept_pointer_for: the slot cache
// ==============================

void test_slot_cache()
{
    reset_shadows();

    // The first ask for a root builds.
    auto first = hv().shadow_ept_pointer_for(cpu, root_1);
    check_true("slots.first_ask_succeeds", first.has_value());
    check("slots.first_ask_builds", 1, hv().shadow_ept_builds[cpu]);
    check("slots.first_ask_is_not_a_hit",
          0,
          hv().shadow_ept_cache_hits[cpu]);

    // The pointer's own fields. SDM Table 25-9: bits 2:0 the memory type,
    // bits 5:3 the page-walk length minus one, and bits 51:12 the root.
    constexpr std::uint64_t pointer_memory_type_mask = 0x7;
    constexpr std::uint64_t pointer_walk_length_shift = 3;
    constexpr std::uint64_t pointer_walk_length_mask = 0x7;

    check("slots.pointer_memory_type",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::memory_type::write_back),
          *first & pointer_memory_type_mask);

    // Four levels, encoded as three. The capability MSR reports a
    // page-walk length of four and not five, and SDM 29.2.1.1 then checks
    // a guest hypervisor's own pointer against that on its behalf - so a
    // wrong value here is an entry failure with nothing to say why.
    constexpr std::uint64_t page_walk_length_minus_one = 3;

    check("slots.pointer_page_walk_length",
          page_walk_length_minus_one,
          (*first >> pointer_walk_length_shift) &
              pointer_walk_length_mask);

    check("slots.pointer_names_the_root",
          hv().host_page_table.virtual_to_physical(
              hv().shadow_epml4[cpu][hv().shadow_ept_current_slot[cpu]]) >>
              shift_4kb,
          *first >> shift_4kb);

    // Asking again for the same root is a hit, and the whole point of
    // keeping more than one shadow: a guest hypervisor switching between
    // its guests' tables must not pay a rebuild for the switch.
    auto again = hv().shadow_ept_pointer_for(cpu, root_1);
    check("slots.second_ask_is_a_hit", 1, hv().shadow_ept_cache_hits[cpu]);
    check(
        "slots.second_ask_does_not_build", 1, hv().shadow_ept_builds[cpu]);
    check("slots.second_ask_is_the_same_pointer", *first, *again);

    // Bits below 12 of the pointer are not part of the identity. SDM
    // 31.4.2 defines the EPTRTA as bits 51:12 and associates mappings with
    // it, so two pointers differing only in memory type or walk length
    // describe the same tables - and rebuilding for one would be a rebuild
    // per entry on a guest hypervisor that varies them.
    constexpr std::uint64_t pointer_low_bits = 0x5e;

    static_cast<void>(
        hv().shadow_ept_pointer_for(cpu, root_1 | pointer_low_bits));
    check("slots.low_pointer_bits_are_not_part_of_the_identity",
          2,
          hv().shadow_ept_cache_hits[cpu]);

    // Four distinct roots fill the four slots with no eviction.
    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_2));
    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_3));
    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_4));

    check("slots.four_roots_fill_four_slots",
          zpp::hypervisor::hypervisor::shadow_ept_slots,
          hv().shadow_ept_builds[cpu]);
    check("slots.four_roots_evict_nothing",
          0,
          hv().shadow_ept_evictions[cpu]);

    // A fifth evicts, round robin.
    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_5));
    check("slots.fifth_root_evicts", 1, hv().shadow_ept_evictions[cpu]);

    // And the root it replaced is gone rather than lingering: a slot whose
    // source still named the old root would answer a later ask for it with
    // a shadow built from someone else's tables.
    std::size_t still_holding_root_1{};
    for (std::size_t slot{};
         slot < zpp::hypervisor::hypervisor::shadow_ept_slots;
         ++slot) {
        if (root_1 == hv().shadow_ept_source[cpu][slot]) {
            ++still_holding_root_1;
        }
    }
    check("slots.evicted_root_is_forgotten", 0, still_holding_root_1);

    // A rebuild invalidates, because the slot's tables have just been
    // reused under a pointer this processor may hold mappings against.
    check_true("slots.rebuild_invalidates", g_invalidations >= 1);

    // This VMM's own tables moving invalidates every shadow composed over
    // them, and that is what the generation is for. A page watch armed
    // after a shadow was built changes permissions the shadow already
    // composed, and comparing generations is how that is noticed without
    // an interprocessor interrupt.
    reset_shadows();

    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_1));
    check("generation.build_before_the_bump",
          1,
          hv().shadow_ept_builds[cpu]);

    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_1));
    check("generation.hit_before_the_bump",
          1,
          hv().shadow_ept_cache_hits[cpu]);

    hv().ept_generation.fetch_add(1, std::memory_order_release);

    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_1));
    check("generation.bump_forces_a_rebuild",
          2,
          hv().shadow_ept_builds[cpu]);
    check("generation.bump_is_not_a_hit",
          1,
          hv().shadow_ept_cache_hits[cpu]);
}

// === install_shadow_leaf, and the livelock invariant
// =====================

/**
 * The single most valuable assertion in this file.
 *
 * A fault arrives for an access; the handler composes, installs a leaf and
 * resumes. If the leaf does not permit the access, the same fault arrives
 * again, and again, for ever. So: install what a fault would install, then
 * ask the shadow what the processor would find, and require it to permit
 * what faulted.
 */
void check_lookup_permits(
    const char * name,
    std::uint64_t guest_physical,
    const zpp::arch::x86_64::vmx::ept_permissions & wanted)
{
    auto found = hv().shadow_ept_lookup(cpu, guest_physical);

    if (zpp::arch::x86_64::vmx::ept_walk_status::mapped != found.status) {
        check(name,
              static_cast<std::uint64_t>(
                  zpp::arch::x86_64::vmx::ept_walk_status::mapped),
              static_cast<std::uint64_t>(found.status));
        return;
    }

    check_true(name,
               (!wanted.read() || found.permissions.read()) &&
                   (!wanted.write() || found.permissions.write()) &&
                   (!wanted.execute() || found.permissions.execute()));
}

void test_install_and_lookup()
{
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    auto anything = zpp::arch::x86_64::vmx::ept_permissions::all();

    // Nothing installed yet, so the shadow answers not-present - which is
    // the state every shadow starts in and the reason every access to a
    // fresh one faults.
    check("install.fresh_shadow_is_empty",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, address_a).status));

    // A 4 KB mapping, installed and then looked up.
    auto installed = hv().install_shadow_leaf(
        cpu, address_a, guest_mapping(address_a, shift_4kb), shift_4kb);
    check_true("install.4kb_succeeds", installed.has_value());

    auto found = hv().shadow_ept_lookup(cpu, address_a);
    check("install.4kb.status",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(found.status));
    check("install.4kb.page_shift", shift_4kb, found.page_shift);
    check(
        "install.4kb.physical_address", address_a, found.physical_address);
    check_lookup_permits(
        "install.4kb.permits_everything", address_a, anything);

    // The memory type is ours - the documented divergence in
    // `compose_ept` - and it has to survive into the entry, because a
    // shadow leaf with a reserved type is an EPT misconfiguration and
    // SDM 30.2.1 gives one no qualification to say so.
    check("install.4kb.memory_type",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::memory_type::write_back),
          static_cast<std::uint64_t>(found.type));

    // **The property, swept.** For every combination of what the guest
    // hypervisor's tables grant and what ours do, a leaf that gets
    // installed must permit exactly the intersection - no less, which is
    // the livelock, and no more, which is the privilege escalation.
    constexpr unsigned permission_combinations = 16;

    for (unsigned guest_index = 1; guest_index < permission_combinations;
         ++guest_index) {
        for (unsigned host_index = 1; host_index < permission_combinations;
             ++host_index) {
            reset_shadows();
            static_cast<void>(take_slot(root_1));

            auto guest_permissions =
                zpp::arch::x86_64::vmx::ept_permissions(
                    0 != (guest_index & 1),
                    0 != (guest_index & 2),
                    0 != (guest_index & 4),
                    0 != (guest_index & 8));

            auto host_permissions =
                zpp::arch::x86_64::vmx::ept_permissions(
                    0 != (host_index & 1),
                    0 != (host_index & 2),
                    0 != (host_index & 4),
                    0 != (host_index & 8));

            // Ours, applied to the whole 2 MB region the address sits in,
            // because that is the granularity `initialize_ept` builds at.
            host_region_of(address_a) = leaf(
                address_a & ~(bytes_2mb - 1), shift_2mb, host_permissions);

            auto composition = zpp::arch::x86_64::vmx::compose_ept(
                guest_mapping(address_a, shift_4kb, guest_permissions),
                hv().host_ept_lookup(address_a),
                zpp::hypervisor::hypervisor::
                    execute_only_translations_offered);

            static_cast<void>(hv().install_shadow_leaf(
                cpu,
                address_a,
                guest_mapping(address_a, shift_4kb, guest_permissions),
                shift_4kb));

            auto shadow = hv().shadow_ept_lookup(cpu, address_a);

            if (zpp::arch::x86_64::vmx::ept_compose_outcome::composed !=
                composition.outcome) {
                // Not composable, so nothing may be installed. An entry
                // put there anyway would be one the fault path never
                // decided on - and the fault path exists precisely because
                // that decision depends on state a table cannot hold.
                if (zpp::arch::x86_64::vmx::ept_walk_status::mapped ==
                    shadow.status) {
                    check("install.uncomposable_leaves_the_shadow_absent",
                          (guest_index << 8) | host_index,
                          ~0ull);
                }
                continue;
            }

            if (shadow.permissions != composition.permissions) {
                check("install.leaf_permits_exactly_the_composition",
                      (guest_index << 8) | host_index,
                      ~0ull);
            }
        }
    }

    check_true("install.uncomposable_leaves_the_shadow_absent", true);
    check_true("install.leaf_permits_exactly_the_composition", true);

    reset_shadows();
}

void test_install_sizes_and_splitting()
{
    // A 2 MB install where both sides agree on 2 MB stays one entry.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    auto region = address_a & ~(bytes_2mb - 1);

    check_true(
        "install.2mb_succeeds",
        hv().install_shadow_leaf(
                cpu, region, guest_mapping(region, shift_2mb), shift_2mb)
            .has_value());

    check("install.2mb.page_shift",
          shift_2mb,
          hv().shadow_ept_lookup(cpu, address_a).page_shift);
    check("install.2mb.no_split", 0, hv().shadow_ept_splits[cpu]);

    // Two tables: the page-directory-pointer table and the page
    // directory. The leaf is *in* the page directory, so no page table is
    // needed - which is the whole saving a large mapping buys.
    check("install.2mb.tables_used", 2, tables_owned_by(0));

    // A 2 MB install where our own tables describe the region at 4 KB has
    // to be split, or the shadow would grant a whole 2 MB region the
    // permissions of one page - the silent privilege escalation nested EPT
    // exists to prevent.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    for (std::size_t i{}; i < entries_per_table; ++i) {
        g_split_page_table[i] =
            leaf(region + (i * page_bytes),
                 shift_4kb,
                 zpp::arch::x86_64::vmx::ept_permissions::all());
    }

    // One page of ours read-only, which is what a watched page is.
    constexpr std::size_t watched_index = 9;
    g_split_page_table[watched_index] = leaf(
        region + (watched_index * page_bytes),
        shift_4kb,
        zpp::arch::x86_64::vmx::ept_permissions(true, false, true, true));

    host_region_of(region) = reference(
        hv().host_page_table.virtual_to_physical(g_split_page_table));

    check_true(
        "split.install_at_2mb_succeeds",
        hv().install_shadow_leaf(
                cpu, region, guest_mapping(region, shift_2mb), shift_2mb)
            .has_value());

    check("split.counted", 1, hv().shadow_ept_splits[cpu]);

    // Every one of the 512 pages, and each mapping the right host page.
    // A splitter that reused the region's base for every page would pass
    // a presence check and translate the whole region to one page.
    std::size_t mapped{};
    std::size_t wrong_address{};

    for (std::size_t i{}; i < entries_per_table; ++i) {
        auto at = region + (i * page_bytes);
        auto shadow = hv().shadow_ept_lookup(cpu, at);

        if (zpp::arch::x86_64::vmx::ept_walk_status::mapped !=
            shadow.status) {
            continue;
        }

        ++mapped;

        if (shadow.physical_address != at) {
            ++wrong_address;
        }

        if (shift_4kb != shadow.page_shift) {
            ++wrong_address;
        }
    }

    check("split.every_page_is_mapped", entries_per_table, mapped);
    check("split.every_page_maps_itself", 0, wrong_address);

    // And the watched page kept read while losing write, which is the
    // whole reason the split happened.
    auto watched =
        hv().shadow_ept_lookup(cpu, region + (watched_index * page_bytes));
    check_true("split.watched_page_keeps_read",
               watched.permissions.read());
    check_false("split.watched_page_loses_write",
                watched.permissions.write());

    // A page our own tables refuse altogether leaves a hole rather than
    // failing the whole split: the other 511 are still installable, and
    // the hole faults back in and is decided then.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    for (std::size_t i{}; i < entries_per_table; ++i) {
        g_split_page_table[i] =
            leaf(region + (i * page_bytes),
                 shift_4kb,
                 zpp::arch::x86_64::vmx::ept_permissions::all());
    }

    constexpr std::size_t denied_index = 17;
    g_split_page_table[denied_index] = zpp::arch::x86_64::vmx::epte{};

    host_region_of(region) = reference(
        hv().host_page_table.virtual_to_physical(g_split_page_table));

    check_true(
        "split.hole_does_not_fail_the_split",
        hv().install_shadow_leaf(
                cpu, region, guest_mapping(region, shift_2mb), shift_2mb)
            .has_value());

    check("split.hole_is_absent",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu,
                                     region + (denied_index * page_bytes))
                  .status));

    check("split.the_page_after_the_hole_is_mapped",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(
                      cpu, region + ((denied_index + 1) * page_bytes))
                  .status));

    build_host_ept();

    // === A guest hypervisor mapping at 1 GB ============================
    //
    // A 1 GB mapping in the guest hypervisor's tables over this VMM's own
    // 2 MB leaves composes at 2 MB - `compose_ept` takes the smaller of
    // the two, which tests/nested_ept asserts directly - so one 2 MB
    // entry is the leaf that belongs there.
    //
    // **These are the four checks that recorded the defect, flipped.**
    // What used to happen: `install_shadow_leaf` tested
    // `composition.page_shift < shift` and sent everything matching to
    // `install_shadow_split`, which is written for the 2 MB-to-4 KB case
    // alone and masks the address to a 2 MB region. A gigabyte therefore
    // became 512 entries of 4 KB covering 2 MB of it, the other 1022 MB
    // still absent, and a page table out of a 96-table pool for every
    // 2 MB region touched - about 188 MB of address space before
    // `fill_shadow_leaf` starts reclaiming and resetting.
    // `verify_nested` builds its own EPT12 as four 1 GB leaves, so the
    // tree's own probe took that path.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    auto gigabyte = address_a & ~(bytes_1gb - 1);

    check_true("install.1gb_over_2mb_succeeds",
               hv().install_shadow_leaf(cpu,
                                        gigabyte,
                                        guest_mapping(gigabyte, shift_1gb),
                                        shift_1gb)
                   .has_value());

    check("install.1gb_over_2mb_installs_at_2mb",
          shift_2mb,
          hv().shadow_ept_lookup(cpu, gigabyte).page_shift);

    // No split, which is the whole of the change: the splitter keeps the
    // one case it was written for and nothing else reaches it.
    check("install.1gb_over_2mb_costs_no_split",
          0,
          hv().shadow_ept_splits[cpu]);

    // Still only 2 MB of the gigabyte, and that is correct rather than a
    // leftover. A leaf goes in for the address that faulted, at the
    // largest size both walks agreed on; the rest faults in the same way
    // when it is touched. Filling all 512 regions eagerly would be
    // guessing at what the second-level guest will reach, which is the
    // guess the lazy fill exists to stop making.
    check("install.1gb_over_2mb_maps_one_region",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, gigabyte + bytes_2mb).status));

    // Two tables now, not three: a page-directory-pointer table and a
    // page directory, with the leaf in the directory. The page table the
    // split used to need is what a 2 MB leaf saves, and it is the number
    // that decides how much of a guest's address space fits in the pool.
    constexpr std::size_t tables_for_a_2mb_leaf = 2;

    check("install.1gb_over_2mb_pool_cost",
          tables_for_a_2mb_leaf,
          tables_owned_by(hv().shadow_ept_current_slot[cpu]));

    // And the neighbouring region shares that page directory rather than
    // needing another, which is the pool saving stated as the thing that
    // actually matters: the 512 regions of a gigabyte cost 512 page
    // tables under the old behaviour and one directory under this.
    static_cast<void>(
        hv().install_shadow_leaf(cpu,
                                 gigabyte + bytes_2mb,
                                 guest_mapping(gigabyte, shift_1gb),
                                 shift_1gb));

    check("install.1gb_over_2mb_second_region_shares_the_directory",
          tables_for_a_2mb_leaf,
          tables_owned_by(hv().shadow_ept_current_slot[cpu]));

    check("install.1gb_over_2mb_second_region_is_mapped",
          shift_2mb,
          hv().shadow_ept_lookup(cpu, gigabyte + bytes_2mb).page_shift);
}

void test_large_entry_in_the_way()
{
    // A 2 MB mapping goes in first, and a later fault inside it needs
    // 4 KB - because our own tables split that region or watch a page of
    // it. Descending through the large entry would treat a guest frame
    // number as if it addressed a table, which is exactly what it did:
    // measured as `could not install a shadow leaf for 0x11c4c9000: error
    // 0x4`, followed by an unhandled EPT violation.
    //
    // The answer is to drop it rather than split it into 512 equivalents,
    // and the addresses it covered then fault back in one at a time. Both
    // halves of that are asserted, because only asserting the first would
    // let a "fix" that silently kept the large entry pass.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    auto region = address_a & ~(bytes_2mb - 1);

    check_true(
        "large_in_the_way.2mb_goes_in_first",
        hv().install_shadow_leaf(
                cpu, region, guest_mapping(region, shift_2mb), shift_2mb)
            .has_value());

    check("large_in_the_way.neighbour_is_covered_by_the_large_entry",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, address_in_a_region).status));

    check_true(
        "large_in_the_way.4kb_inside_it_succeeds",
        hv().install_shadow_leaf(cpu,
                                 address_a,
                                 guest_mapping(address_a, shift_4kb),
                                 shift_4kb)
            .has_value());

    // The 4 KB mapping is there, at 4 KB.
    auto small = hv().shadow_ept_lookup(cpu, address_a);
    check("large_in_the_way.the_4kb_leaf_is_there",
          shift_4kb,
          small.page_shift);
    check("large_in_the_way.the_4kb_leaf_translates",
          address_a,
          small.physical_address);

    // And the rest of the region lost its mapping, which is the documented
    // cost of dropping rather than splitting.
    check("large_in_the_way.the_rest_of_the_region_is_dropped",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, address_in_a_region).status));
}

// === pool pressure
// =======================================================

void test_pool_pressure()
{
    // Exhausting the pool from one shadow. `install_shadow_leaf` refuses
    // with `out_of_shadow_ept_tables` and installs nothing; it is
    // `fill_shadow_leaf` above it that knows how to recover, in three
    // stages, and each stage is a case.
    reset_shadows();
    static_cast<void>(take_slot(root_1));

    // Addresses one gigabyte apart, so each needs a page directory of its
    // own and the pool drains predictably: one page-directory-pointer
    // table shared, then a directory plus a table per address.
    auto address_at = [](std::size_t index) {
        return (index + 1) * bytes_1gb;
    };

    std::size_t installed{};
    std::size_t refusals{};

    for (std::size_t i{};
         i < zpp::hypervisor::hypervisor::shadow_ept_tables_per_cpu;
         ++i) {
        auto at = address_at(i);
        auto result = hv().install_shadow_leaf(
            cpu, at, guest_mapping(at, shift_4kb), shift_4kb);

        if (result) {
            ++installed;
            continue;
        }

        ++refusals;
        check(
            "pool.refusal_is_out_of_tables",
            static_cast<std::uint64_t>(zpp::hypervisor::hypervisor::error::
                                           out_of_shadow_ept_tables),
            static_cast<std::uint64_t>(result.error().code()));
        break;
    }

    check_true("pool.a_single_shadow_can_exhaust_it", refusals >= 1);
    check_true("pool.some_installs_succeeded_first", installed >= 1);

    // The whole pool is now owned by this one slot, which is what makes
    // the recovery stages below distinguishable.
    check("pool.every_table_is_owned",
          zpp::hypervisor::hypervisor::shadow_ept_tables_per_cpu,
          tables_owned_by(hv().shadow_ept_current_slot[cpu]));

    // Stage three: the current shadow alone does not fit, so it is reset.
    // Slow, and the only answer that makes progress - refusing would stop
    // the processor.
    auto next = address_at(installed + 1);
    check_true(
        "pool.fill_recovers_by_resetting",
        hv().fill_shadow_leaf(
                cpu, next, guest_mapping(next, shift_4kb), shift_4kb)
            .has_value());

    check("pool.reset_is_counted", 1, hv().shadow_ept_resets[cpu]);

    // And the mapping it was recovering for is actually there, which is
    // the point of recovering at all.
    check_lookup_permits("pool.the_mapping_that_forced_the_reset_is_there",
                         next,
                         zpp::arch::x86_64::vmx::ept_permissions::all());

    // Stage two: the *other* slots hold tables and dropping them is
    // enough. Each is a shadow of a guest the guest hypervisor is not
    // running at this instant, so losing one costs the faults to fill it
    // again and nothing else.
    reset_shadows();

    // Two shadows, the first filled until the pool is gone.
    static_cast<void>(take_slot(root_2));
    auto victim_slot = hv().shadow_ept_current_slot[cpu];

    for (std::size_t i{};
         i < zpp::hypervisor::hypervisor::shadow_ept_tables_per_cpu;
         ++i) {
        auto at = address_at(i);
        if (!hv().install_shadow_leaf(
                cpu, at, guest_mapping(at, shift_4kb), shift_4kb)) {
            break;
        }
    }

    // The second slot now needs a table and there is none.
    static_cast<void>(hv().shadow_ept_pointer_for(cpu, root_3));
    auto asking_slot = hv().shadow_ept_current_slot[cpu];
    check_true("pool.two_slots_are_distinct", victim_slot != asking_slot);

    check_true("pool.fill_recovers_by_reclaiming",
               hv().fill_shadow_leaf(cpu,
                                     address_a,
                                     guest_mapping(address_a, shift_4kb),
                                     shift_4kb)
                   .has_value());

    check("pool.reclaim_is_counted", 1, hv().shadow_ept_reclaims[cpu]);
    check("pool.reclaim_did_not_need_a_reset",
          0,
          hv().shadow_ept_resets[cpu]);

    check_lookup_permits("pool.the_reclaimed_fill_is_there",
                         address_a,
                         zpp::arch::x86_64::vmx::ept_permissions::all());

    // The slot that was dropped lost its tables *and* its root. A root
    // still naming tables the pool has handed to someone else is two
    // shadows sharing a subtree, with no fault anywhere to say so.
    check("pool.the_reclaimed_slot_owns_nothing",
          0,
          tables_owned_by(victim_slot));

    std::size_t non_empty_root_entries{};
    for (std::size_t i{}; i < entries_per_table; ++i) {
        if (zpp::arch::x86_64::vmx::ept_permissions::of(
                hv().shadow_epml4[cpu][victim_slot][i])
                .present()) {
            ++non_empty_root_entries;
        }
    }

    check("pool.the_reclaimed_slot_has_an_empty_root",
          0,
          non_empty_root_entries);

    check("pool.the_reclaimed_slot_has_no_source",
          0,
          hv().shadow_ept_source[cpu][victim_slot]);
}

// === collect_shadow_leaves and refresh_shadow_ept_for
// ====================

/**
 * Builds a four-level extended page table for the guest hypervisor in its
 * own memory, mapping one address at 4 KB.
 *
 * The refresh path walks these for real, through `read_guest_physical`, so
 * this is the one fixture here that has to be a table rather than a walk
 * result.
 */
void map_in_guest_tables(std::uint64_t root,
                         std::uint64_t guest_physical,
                         std::uint64_t physical,
                         bool present = true)
{
    // Tables handed out below the roots the cases use, so nothing
    // overlaps.
    static std::uint64_t next_table = 0x2000000;

    struct level
    {
        std::uint64_t shift;
    };

    constexpr std::uint64_t shifts[]{39, 30, 21, 12};

    auto table = root;

    for (std::size_t i{}; i < 3; ++i) {
        auto index = (guest_physical >> shifts[i]) & 0x1ff;
        auto & entry = *reinterpret_cast<zpp::arch::x86_64::vmx::epte *>(
            guest_page_of(table).data() + (index * sizeof(std::uint64_t)));

        if (!zpp::arch::x86_64::vmx::ept_permissions::of(entry)
                 .present()) {
            auto child = next_table;
            next_table += page_bytes;
            entry = reference(child);
        }

        table = entry.page_number() << shift_4kb;
    }

    auto index = (guest_physical >> shifts[3]) & 0x1ff;
    auto & entry = *reinterpret_cast<zpp::arch::x86_64::vmx::epte *>(
        guest_page_of(table).data() + (index * sizeof(std::uint64_t)));

    entry = present ? leaf(physical,
                           shift_4kb,
                           zpp::arch::x86_64::vmx::ept_permissions::all())
                    : zpp::arch::x86_64::vmx::epte{};
}

void test_collect_and_refresh()
{
    reset_shadows();
    g_guest_pages.clear();

    auto slot = take_slot(root_1);

    // Nothing installed, nothing to collect.
    check(
        "collect.empty_shadow", 0, hv().collect_shadow_leaves(cpu, slot));

    // Two mappings at two sizes, so the collector's leaf discrimination is
    // exercised rather than only its lowest level: a 2 MB leaf lives in a
    // page directory and a 4 KB one in a page table, and a collector that
    // only recognised the lowest level would silently drop every large
    // mapping a shadow holds.
    //
    // No third mapping at 1 GB, deliberately: nothing installs a 1 GB
    // shadow leaf today, because a guest hypervisor's gigabyte composes
    // against this VMM's own 2 MB tables and then goes down the 4 KB
    // splitter - which is the defect
    // `install.1gb_over_2mb_installs_at_2mb` above covers. A
    // case here that asked for one would be measuring that instead of the
    // collector.
    auto region = address_b & ~(bytes_2mb - 1);

    static_cast<void>(hv().install_shadow_leaf(
        cpu, address_a, guest_mapping(address_a, shift_4kb), shift_4kb));
    static_cast<void>(hv().install_shadow_leaf(
        cpu, region, guest_mapping(region, shift_2mb), shift_2mb));

    check("collect.finds_every_leaf",
          2,
          hv().collect_shadow_leaves(cpu, slot));

    // And each with the address and size it was installed at, which is
    // what the refresh re-walks from. A collector that reported the right
    // *count* with the wrong addresses would silently move every mapping.
    std::size_t found_4kb{};
    std::size_t found_2mb{};

    auto collected = hv().collect_shadow_leaves(cpu, slot);
    for (std::size_t i{}; i < collected; ++i) {
        auto one = hv().shadow_ept_refresh_list[cpu][i];
        if ((address_a & ~(page_bytes - 1)) == one.guest_physical &&
            shift_4kb == one.shift) {
            ++found_4kb;
        }
        if (region == one.guest_physical && shift_2mb == one.shift) {
            ++found_2mb;
        }
    }

    check("collect.reports_the_4kb_leaf_at_its_address", 1, found_4kb);
    check("collect.reports_the_2mb_leaf_at_its_address", 1, found_2mb);

    // The refresh: the guest hypervisor changed its tables and said so
    // with INVEPT, but did not say *what*. Every mapping composed from
    // those tables is therefore suspect and every one is composed again -
    // which must keep what is still mapped and lose what is not.
    reset_shadows();
    g_guest_pages.clear();

    static_cast<void>(take_slot(root_1));

    // Two addresses, both mapped by the guest hypervisor and both in the
    // shadow.
    map_in_guest_tables(root_1, address_a, address_a);
    map_in_guest_tables(root_1, address_b, address_b);

    static_cast<void>(hv().install_shadow_leaf(
        cpu, address_a, guest_mapping(address_a, shift_4kb), shift_4kb));
    static_cast<void>(hv().install_shadow_leaf(
        cpu, address_b, guest_mapping(address_b, shift_4kb), shift_4kb));

    // The guest hypervisor now unmaps one of them and invalidates.
    map_in_guest_tables(root_1, address_b, address_b, false);

    hv().refresh_shadow_ept_for(cpu, root_1);

    check("refresh.counted", 1, hv().shadow_ept_refreshes[cpu]);

    // Kept, because the guest hypervisor still maps it. Losing it would
    // cost a fault; the refresh exists precisely to avoid that, and a
    // refresh that kept nothing would be a discard wearing its name.
    check("refresh.keeps_what_is_still_mapped",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::mapped),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, address_a).status));

    // Lost, because it is not. **This is the direction that matters**: a
    // refresh that kept a mapping the guest hypervisor has removed would
    // leave its guest reading a page it had revoked, which is the whole
    // security property nested EPT provides.
    check("refresh.loses_what_the_guest_hypervisor_removed",
          static_cast<std::uint64_t>(
              zpp::arch::x86_64::vmx::ept_walk_status::not_present),
          static_cast<std::uint64_t>(
              hv().shadow_ept_lookup(cpu, address_b).status));

    // The refresh also invalidates, because the entries it rewrote
    // replace ones this processor may still have cached.
    check_true("refresh.invalidates", g_invalidations >= 1);

    // A root no slot was built from is not refreshed, and touching one
    // would be a rebuild of a shadow nobody asked about.
    auto before = hv().shadow_ept_refreshes[cpu];
    hv().refresh_shadow_ept_for(cpu, root_5);
    check("refresh.ignores_a_root_no_slot_holds",
          before,
          hv().shadow_ept_refreshes[cpu]);
}

} // namespace

int main()
{
    // The host page table, and the reverse map the shadow pool is reached
    // through. Both are the real ones: `map_identity` fills the real
    // `page_table` using the real walker, and the reverse map is filled
    // the way `initialize_module_physical_to_virtual` fills it - page by
    // page, through the host table, because that is the mapping these
    // addresses are reached through once a processor has switched.
    //
    // Only the pages the shadow machinery names are registered, rather
    // than a whole module: the pool, the roots, this VMM's own tables and
    // the one split table a case needs. A page missing from this map is
    // reported by the code under test as a table it cannot find, which is
    // a real answer rather than a crash - so an omission here shows up as
    // a named failure.
    const zpp::tests::identity_range ranges[]{
        {&hv().host_page_table, sizeof(hv().host_page_table)},
        {hv().shadow_epml4, sizeof(hv().shadow_epml4)},
        {hv().shadow_ept_tables, sizeof(hv().shadow_ept_tables)},
        {hv().epml4, sizeof(hv().epml4)},
        {hv().epdpt, sizeof(hv().epdpt)},
        {hv().epd, sizeof(hv().epd)},
        {g_split_page_table, sizeof(g_split_page_table)},
    };

    zpp::tests::map_identity(hv().host_page_table, ranges);

    for (const auto & one : ranges) {
        auto base = reinterpret_cast<std::uintptr_t>(one.base);
        for (std::size_t offset{}; offset < one.size;
             offset += page_bytes) {
            auto address = base + offset;
            hv().module_physical_to_virtual.emplace(
                hv().host_page_table.virtual_to_physical(address),
                address);
        }
    }

    test_host_ept_lookup();
    test_slot_cache();
    test_install_and_lookup();
    test_install_sizes_and_splitting();
    test_large_entry_in_the_way();
    test_pool_pressure();
    test_collect_and_refresh();

    std::println(
        "shadow_ept: {} checks, {} failures", g_checks, g_failures);

    return (0 == g_failures) ? 0 : 1;
}
