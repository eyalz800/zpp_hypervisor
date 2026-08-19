// Tests for the nested EPT walker, the permission composition, the shadow
// table pool and the two places a guest hypervisor's own extended page
// tables are judged.
//
// Two tiers, and which tier a rule lands in is decided by the code under
// test rather than by preference:
//
//  1. Everything in nested_ept.h is pure - walk_ept reaches memory only
//     through the callable it is handed, and compose_ept touches none at
//     all - so those rules are `static_assert`s and the compile *is* the
//     run, exactly as with decoder-test.cpp beside it.
//
//  2. The shadow table pool, the shadow walk, the second-level fault
//     decision and the EPT-pointer check are members of `hypervisor`,
//     writing into arrays that are megabytes of that class. They cannot be
//     constant-evaluated, so they run from `main` and report a count.
//     Nothing about them is modelled: check-nested-ept.sh cuts the real
//     bodies out of nested_ept.cpp and nested_entry.cpp by name and by
//     anchor, and this file supplies a stand-in `hypervisor` carrying only
//     the members they touch - the same arrangement tests/watched_page
//     uses, and for the same reason. A renamed function or a moved
//     fragment fails the extraction rather than silently testing nothing.
//
// This is the only verification of any of nested VMX that does not need
// hardware, which is why it is worth having: every rule below is one this
// VMM would otherwise only have read.
//
// Every SDM citation here was looked up in `.references/sdm.txt` and
// carries the line it was read at, so the next person can check it in one
// command rather than trusting a section number. Note the table numbering:
// this revision calls the EPT-violation exit qualification **Table 30-7**
// (sdm.txt:203865). Older revisions number it 28-7; if a citation here
// does not land, the revision moved, not the rule.
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include <array>
#include <cstdio>
#include <optional>

// The sixteen permission sets, indexed by the four bits in the order
// `ept_permissions` takes them: read, write, execute, execute_user.
constexpr zpp::arch::x86_64::vmx::ept_permissions permissions_number(int i)
{
    return zpp::arch::x86_64::vmx::ept_permissions(
        0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
}

// ---------------------------------------------------------------------------
// ept_permissions: intersection and normalisation.
// ---------------------------------------------------------------------------

constexpr auto rwx =
    zpp::arch::x86_64::vmx::ept_permissions(true, true, true, true);
constexpr auto read_only =
    zpp::arch::x86_64::vmx::ept_permissions(true, false, false, false);
constexpr auto nothing = zpp::arch::x86_64::vmx::ept_permissions();

static_assert(rwx.present());
static_assert(!nothing.present());

// Intersection is per bit, so a read-only side makes the result read-only.
static_assert(rwx.intersected_with(read_only) == read_only);
static_assert(read_only.intersected_with(rwx) == read_only);
static_assert(rwx.intersected_with(nothing) == nothing);

// An empty intersection is simply not present, which is the whole point of
// needing no special case for it.
static_assert(!rwx.intersected_with(nothing).present());

// Normalisation: write without read is what an intersection can produce
// and what SDM 31.3.3.1 makes a misconfiguration.
constexpr auto write_no_read =
    zpp::arch::x86_64::vmx::ept_permissions(false, true, false, false);
static_assert(write_no_read.normalised(false) == nothing);
static_assert(write_no_read.normalised(true) == nothing);

// Execute without read is legal only where execute-only translations are
// reported, and then only for execute - never for write.
constexpr auto execute_no_read =
    zpp::arch::x86_64::vmx::ept_permissions(false, false, true, false);
static_assert(execute_no_read.normalised(false) == nothing);
static_assert(execute_no_read.normalised(true) == execute_no_read);

constexpr auto user_execute_no_read =
    zpp::arch::x86_64::vmx::ept_permissions(false, false, false, true);
static_assert(user_execute_no_read.normalised(false) == nothing);
static_assert(user_execute_no_read.normalised(true) ==
              user_execute_no_read);

// Write and execute without read: the write must go even where
// execute-only is available.
constexpr auto write_execute_no_read =
    zpp::arch::x86_64::vmx::ept_permissions(false, true, true, false);
static_assert(write_execute_no_read.normalised(true) == execute_no_read);

// Anything with read is already legal and must be left exactly alone.
static_assert(rwx.normalised(false) == rwx);
static_assert(read_only.normalised(false) == read_only);

// Normalisation only ever removes. Checked as a property rather than by
// example, because "it never adds read" is the safety argument.
constexpr bool never_adds(zpp::arch::x86_64::vmx::ept_permissions before,
                          bool execute_only)
{
    auto after = before.normalised(execute_only);
    return (!after.read() || before.read()) &&
           (!after.write() || before.write()) &&
           (!after.execute() || before.execute()) &&
           (!after.execute_user() || before.execute_user());
}

constexpr bool never_adds_anywhere()
{
    for (int i = 0; i < 16; ++i) {
        auto before = zpp::arch::x86_64::vmx::ept_permissions(
            0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
        if (!never_adds(before, false) || !never_adds(before, true)) {
            return false;
        }
    }
    return true;
}

static_assert(never_adds_anywhere());

// And the result of normalisation is always something the processor
// accepts: read clear implies write clear, and implies execute clear
// unless execute-only is available.
constexpr bool always_legal()
{
    for (int i = 0; i < 16; ++i) {
        auto before = zpp::arch::x86_64::vmx::ept_permissions(
            0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));

        for (auto execute_only : {false, true}) {
            auto after = before.normalised(execute_only);

            if (after.read()) {
                continue;
            }

            if (after.write()) {
                return false;
            }

            if (!execute_only &&
                (after.execute() || after.execute_user())) {
                return false;
            }
        }
    }
    return true;
}

static_assert(always_legal());

// ---------------------------------------------------------------------------
// Normalisation, exhaustively, against the two SDM rules spelled out one
// at a time rather than as a property.
//
// SDM 31.3.3.1 (sdm.txt:205511) lists the misconfiguration conditions:
// "Bit 0 of the entry is clear (indicating that data reads are not
// allowed) and any of the following hold: Bit 1 is set", then
// (sdm.txt:205515) "The processor does not support execute-only
// translations and either of the following hold: Bit 2 is set ... the
// 'mode-based execute control for EPT' VM-execution control is 1 and bit
// 10 is set".
//
// Written as sixteen cells so a failure names the rule it broke, which the
// property-shaped checks above cannot: `always_legal()` failing says only
// that some cell is wrong.
// ---------------------------------------------------------------------------

// Rule one: read clear implies write clear, whatever the processor
// supports. There is no capability that makes write-without-read legal.
constexpr bool write_needs_read_everywhere()
{
    for (int i = 0; i < 16; ++i) {
        for (auto execute_only : {false, true}) {
            auto after = permissions_number(i).normalised(execute_only);
            if (!after.read() && after.write()) {
                return false;
            }
        }
    }
    return true;
}

static_assert(write_needs_read_everywhere());

// Rule two: read clear implies both execute bits clear *unless*
// execute-only translations are supported, which SDM 31.3.3.1 makes
// IA32_VMX_EPT_VPID_CAP bit 0 (sdm.txt:223498, "If bit 0 is read as 1, the
// processor supports execute-only translations by EPT").
constexpr bool execute_needs_read_without_the_capability()
{
    for (int i = 0; i < 16; ++i) {
        auto after = permissions_number(i).normalised(false);
        if (!after.read() && (after.execute() || after.execute_user())) {
            return false;
        }
    }
    return true;
}

static_assert(execute_needs_read_without_the_capability());

// And with the capability, execute-without-read is kept exactly as it was
// - both halves of it, since bit 10 is named in the same sentence as bit
// 2. Only the write goes.
constexpr bool execute_only_is_kept_with_the_capability()
{
    for (int i = 0; i < 16; ++i) {
        auto before = permissions_number(i);
        auto after = before.normalised(true);

        if (before.read()) {
            continue;
        }

        if ((after.execute() != before.execute()) ||
            (after.execute_user() != before.execute_user())) {
            return false;
        }
    }
    return true;
}

static_assert(execute_only_is_kept_with_the_capability());

// Normalisation is idempotent. Worth pinning because `walk_ept` uses "the
// normalised form differs from what is there" as its misconfiguration
// test, which is only the same test spelled once if applying it twice
// changes nothing.
constexpr bool normalisation_is_idempotent()
{
    for (int i = 0; i < 16; ++i) {
        for (auto execute_only : {false, true}) {
            auto once = permissions_number(i).normalised(execute_only);
            if (once.normalised(execute_only) != once) {
                return false;
            }
        }
    }
    return true;
}

static_assert(normalisation_is_idempotent());

// ---------------------------------------------------------------------------
// Composition across the two levels, all sixteen by sixteen.
//
// The permission bits of a translation are a logical-AND across every
// entry used - SDM Table 30-7 bit 3 (sdm.txt:203870), "The logical-AND of
// bit 0 in the EPT paging-structure entries used to translate the
// guest-physical address" - and a shadow's entries stand for both levels
// at once. So the composition has to be the intersection and nothing else.
//
// Each rule below is separate, so a failure names which one broke. The
// four together are the whole of what `compose_ept` may do with
// permissions: intersect, normalise, never widen, and never invent a
// mapping.
// ---------------------------------------------------------------------------

constexpr zpp::arch::x86_64::vmx::ept_walk_result
mapped_with(zpp::arch::x86_64::vmx::ept_permissions permissions,
            std::uint64_t shift = 12)
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = zpp::arch::x86_64::vmx::ept_walk_status::mapped;
    result.page_shift = shift;
    result.permissions = permissions;
    result.type = zpp::arch::x86_64::memory_type::write_back;
    return result;
}

// Rule one: where anything is composed at all, it is exactly the
// intersection of the two levels with the processor's own rules applied
// after - not before, which would let a level's illegal-looking half
// survive into the shadow.
//
// The wanted value is built out of the index bits rather than by calling
// `intersected_with`, deliberately. Composing the answer with the same
// helper the implementation composes it with makes the check agree with
// whatever that helper does, which is exactly the shape of a test that
// passes while the code is wrong - and it was, until an experiment
// replaced the intersection with a union and this cell stayed green.
constexpr bool composition_is_the_normalised_intersection()
{
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            auto guest = permissions_number(i);
            auto host = permissions_number(j);

            for (auto execute_only : {false, true}) {
                auto composed = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_with(guest),
                    mapped_with(host, 21),
                    execute_only);

                auto wanted =
                    permissions_number(i & j).normalised(execute_only);

                if (!wanted.present()) {
                    // Nothing left to install. Never `composed`, and
                    // never reflected either - see the outcome rule
                    // below.
                    if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                            composed == composed.outcome) {
                        return false;
                    }
                    continue;
                }

                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        composed != composed.outcome) {
                    return false;
                }

                if (composed.permissions != wanted) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(composition_is_the_normalised_intersection());

// Rule two: the intersection is symmetric, so composing the two levels the
// other way round produces the same permissions. Not a tautology about
// `intersected_with` - it is the check that `compose_ept` has not started
// preferring one side's bits, which is exactly what "take the leaf's
// permissions" would look like.
constexpr bool composition_is_symmetric_in_permissions()
{
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            for (auto execute_only : {false, true}) {
                auto forward = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_with(permissions_number(i)),
                    mapped_with(permissions_number(j), 21),
                    execute_only);
                auto backward = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_with(permissions_number(j)),
                    mapped_with(permissions_number(i), 21),
                    execute_only);

                if (forward.outcome != backward.outcome) {
                    return false;
                }

                if (forward.permissions != backward.permissions) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(composition_is_symmetric_in_permissions());

// Rule three: composition never grants what a level withheld. Stated per
// bit and over the whole square including the empty sets, which the
// earlier `composition_never_widens` skips - it starts at 1 because it is
// about the composed cases, and this one is about all of them.
constexpr bool composition_grants_nothing_new()
{
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            auto guest = permissions_number(i);
            auto host = permissions_number(j);

            for (auto execute_only : {false, true}) {
                auto got = zpp::arch::x86_64::vmx::compose_ept(
                               mapped_with(guest),
                               mapped_with(host, 21),
                               execute_only)
                               .permissions;

                if (got.read() && !(guest.read() && host.read())) {
                    return false;
                }
                if (got.write() && !(guest.write() && host.write())) {
                    return false;
                }
                if (got.execute() &&
                    !(guest.execute() && host.execute())) {
                    return false;
                }
                if (got.execute_user() &&
                    !(guest.execute_user() && host.execute_user())) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(composition_grants_nothing_new());

// Rule four: an entry either level left absent is never composed into a
// present one, and an empty overlap is never reflected. Both walks
// succeeded, so saying the guest hypervisor's tables refused the access
// would be false - see `compose_ept`'s own note.
constexpr bool an_empty_side_is_never_reflected()
{
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            for (auto execute_only : {false, true}) {
                auto composed = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_with(permissions_number(i)),
                    mapped_with(permissions_number(j), 21),
                    execute_only);

                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        composed == composed.outcome) {
                    continue;
                }

                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        host_denied != composed.outcome) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(an_empty_side_is_never_reflected());

// ---------------------------------------------------------------------------
// A tiny extended page table to walk, built in this file.
//
// Four tables at made-up physical addresses, so the walk's index
// arithmetic and leaf discrimination are exercised against something whose
// right answer is written down beside it.
// ---------------------------------------------------------------------------

constexpr std::uint64_t pml4_at = 0x1000;
constexpr std::uint64_t pdpt_at = 0x2000;
constexpr std::uint64_t pd_at = 0x3000;
constexpr std::uint64_t pt_at = 0x4000;

constexpr std::uint64_t physical_address_bits = 46;

// The guest-physical address every table below is built to translate,
// chosen so that each level's index is different and none is zero: PML4E
// 1, PDPTE 2, PDE 3, PTE 4.
constexpr std::uint64_t translated =
    (1ull << 39) | (2ull << 30) | (3ull << 21) | (4ull << 12) | 0x678;

constexpr zpp::arch::x86_64::vmx::epte
table_entry(std::uint64_t page_number)
{
    zpp::arch::x86_64::vmx::epte entry;
    entry.read(true);
    entry.write(true);
    entry.execute(true);
    entry.execute_user(true);
    entry.page_number(page_number);
    return entry;
}

constexpr zpp::arch::x86_64::vmx::epte
leaf_entry(std::uint64_t page_number,
           bool large,
           zpp::arch::x86_64::memory_type type =
               zpp::arch::x86_64::memory_type::write_back)
{
    zpp::arch::x86_64::vmx::epte entry;
    entry.read(true);
    entry.write(true);
    entry.execute(true);
    entry.execute_user(true);
    entry.large(large);
    entry.page_number(page_number);
    entry.type(type);
    return entry;
}

// One tree, with a knob per test so each case is a small edit of the same
// shape rather than a fresh table.
struct tree
{
    zpp::arch::x86_64::vmx::epte pml4{table_entry(pdpt_at >> 12)};
    zpp::arch::x86_64::vmx::epte pdpte{table_entry(pd_at >> 12)};
    zpp::arch::x86_64::vmx::epte pde{table_entry(pt_at >> 12)};
    zpp::arch::x86_64::vmx::epte pte{leaf_entry(0x9abcd, false)};
};

constexpr auto reader_for(const tree & of)
{
    return [&of](std::uint64_t at)
               -> std::optional<zpp::arch::x86_64::vmx::epte> {
        switch (at) {
        case pml4_at + (1 * 8):
            return of.pml4;
        case pdpt_at + (2 * 8):
            return of.pdpte;
        case pd_at + (3 * 8):
            return of.pde;
        case pt_at + (4 * 8):
            return of.pte;
        default:
            // Any other address is a slot this test never filled in, which
            // stands for an entry the guest never wrote: absent.
            return std::nullopt;
        }
    };
}

constexpr zpp::arch::x86_64::vmx::ept_walk_result
walk(const tree & of,
     std::uint64_t guest_physical = translated,
     bool execute_only = false)
{
    return zpp::arch::x86_64::vmx::walk_ept(pml4_at,
                                            guest_physical,
                                            physical_address_bits,
                                            execute_only,
                                            reader_for(of));
}

// The plain case: four levels, a 4 KB leaf.
static_assert(walk(tree{}).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::mapped);
static_assert(walk(tree{}).page_shift == 12);
static_assert(walk(tree{}).physical_address ==
              ((0x9abcdull << 12) | 0x678));
static_assert(walk(tree{}).permissions == rwx);
static_assert(walk(tree{}).type ==
              zpp::arch::x86_64::memory_type::write_back);

// A 2 MB leaf at the page-directory level. The offset kept is bits 20:0 of
// the address, per SDM 31.3.2's "Bits 20:0 are from the original
// guest-physical address".
constexpr tree two_megabyte = [] {
    tree of;
    of.pde = leaf_entry(0, true);
    of.pde.large_page_number(0x123);
    return of;
}();

static_assert(walk(two_megabyte).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::mapped);
static_assert(walk(two_megabyte).page_shift == 21);
static_assert(walk(two_megabyte).physical_address ==
              ((0x123ull << 21) | (translated & 0x1fffff)));

// A 1 GB leaf at the page-directory-pointer level.
constexpr tree one_gigabyte = [] {
    tree of;
    of.pdpte = leaf_entry(0, true);
    of.pdpte =
        zpp::arch::x86_64::vmx::epte(of.pdpte.value() | (0x5ull << 30));
    return of;
}();

static_assert(walk(one_gigabyte).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::mapped);
static_assert(walk(one_gigabyte).page_shift == 30);
static_assert(walk(one_gigabyte).physical_address ==
              ((0x5ull << 30) | (translated & 0x3fffffff)));

// Permissions accumulate across levels rather than coming from the leaf. A
// read-only entry high up makes the whole translation read-only even
// though the leaf grants everything - which is what SDM Table 30-7's
// "logical-AND of bit 1 in the EPT paging-structure entries used"
// describes.
constexpr tree read_only_pdpte = [] {
    tree of;
    of.pdpte.write(false);
    return of;
}();

static_assert(walk(read_only_pdpte).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::mapped);
static_assert(!walk(read_only_pdpte).permissions.write());
static_assert(walk(read_only_pdpte).permissions.read());

// An absent entry stops the walk, and clears the reported permissions
// rather than reporting what the levels above granted - Note 2 to SDM
// Table 30-7.
constexpr tree absent_pde = [] {
    tree of;
    of.pde.read(false);
    of.pde.write(false);
    of.pde.execute(false);
    of.pde.execute_user(false);
    return of;
}();

static_assert(walk(absent_pde).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::not_present);
static_assert(walk(absent_pde).permissions == nothing);

// Write without read is a misconfiguration, at any level.
constexpr tree write_only_pde = [] {
    tree of;
    of.pde.read(false);
    return of;
}();

static_assert(walk(write_only_pde).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// Execute without read is a misconfiguration only where execute-only
// translations are not supported. The same table gives both answers, which
// is the point of the capability being a parameter.
constexpr tree execute_only_pde = [] {
    tree of;
    of.pde.read(false);
    of.pde.write(false);
    of.pde.execute_user(false);
    return of;
}();

static_assert(walk(execute_only_pde, translated, false).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);
static_assert(walk(execute_only_pde, translated, true).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::mapped);

// A reserved memory type in a leaf is a misconfiguration. 2, 3 and 7 are
// the reserved values per SDM 31.3.7.2.
constexpr tree reserved_type = [] {
    tree of;
    of.pte = leaf_entry(0x9abcd, false, zpp::arch::x86_64::memory_type(3));
    return of;
}();

static_assert(walk(reserved_type).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// A memory type in a *non-leaf* entry is a reserved bit rather than a
// type: SDM Table 31-6 reserves bits 6:3 of a page-directory entry that
// references a page table. initialize_ept already clears them for its own
// splits; here the walker has to notice when a guest does not.
constexpr tree type_in_table_entry = [] {
    tree of;
    of.pde.type(zpp::arch::x86_64::memory_type::write_back);
    return of;
}();

static_assert(walk(type_in_table_entry).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// An address bit at or above the processor's physical-address width is a
// reserved bit, so also a misconfiguration.
constexpr tree address_too_wide = [] {
    tree of;
    of.pde = zpp::arch::x86_64::vmx::epte(of.pde.value() |
                                          (1ull << physical_address_bits));
    return of;
}();

static_assert(walk(address_too_wide).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// A 2 MB leaf must have bits 20:12 of its address clear - SDM Table 31-5.
constexpr tree misaligned_large_leaf = [] {
    tree of;
    of.pde = leaf_entry(0, true);
    of.pde.large_page_number(0x123);
    of.pde = zpp::arch::x86_64::vmx::epte(of.pde.value() | (1ull << 13));
    return of;
}();

static_assert(walk(misaligned_large_leaf).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// Bits 51:48 of a guest-physical address must be zero with 4-level EPT.
static_assert(
    walk(tree{}, translated | (1ull << 48)).status ==
    zpp::arch::x86_64::vmx::ept_walk_status::address_out_of_range);
static_assert(walk(tree{}, translated | (1ull << 48)).permissions ==
              nothing);

// A page-directory entry with bit 7 set is a leaf; a PML4 entry with bit 7
// set is *reserved* and therefore a misconfiguration, not a 512 GB page.
constexpr tree large_pml4e = [] {
    tree of;
    of.pml4.large(true);
    return of;
}();

static_assert(walk(large_pml4e).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::misconfigured);

// An entry that cannot be read at all is reported as absent rather than
// mapped, which is what a table outside guest memory has to come out as.
constexpr tree unreachable_table = [] {
    tree of;
    of.pml4 = table_entry(0x7ffff);
    return of;
}();

static_assert(walk(unreachable_table).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::not_present);

// A reserved bit in an entry that is *not present* is not a
// misconfiguration: SDM 31.3.3.1 guards that whole group with "The entry
// is present ... and", and 31.4.3.4 says nothing is cached from an absent
// entry.
constexpr tree absent_with_reserved_bit = [] {
    tree of;
    of.pde.read(false);
    of.pde.write(false);
    of.pde.execute(false);
    of.pde.execute_user(false);
    of.pde.type(zpp::arch::x86_64::memory_type::write_back);
    return of;
}();

static_assert(walk(absent_with_reserved_bit).status ==
              zpp::arch::x86_64::vmx::ept_walk_status::not_present);

// A permission set the walker rejects is exactly one normalisation would
// have changed, over all sixteen and both capability settings. The two are
// the same rule, and `ept_walk::misconfigured` spelling it as "the
// normalised form differs from what is there" is only correct while they
// agree.
constexpr bool normalised_agrees_with_misconfigured()
{
    for (int i = 1; i < 16; ++i) {
        for (auto execute_only : {false, true}) {
            auto before = permissions_number(i);

            zpp::arch::x86_64::vmx::epte entry;
            before.apply_to(entry);
            entry.page_number(1);
            entry.type(zpp::arch::x86_64::memory_type::write_back);

            auto rejected =
                zpp::arch::x86_64::vmx::ept_walk::misconfigured(
                    entry, 0, true, physical_address_bits, execute_only);

            if (rejected != (before != before.normalised(execute_only))) {
                return false;
            }
        }
    }
    return true;
}

static_assert(normalised_agrees_with_misconfigured());

// The same square driven through the whole walker rather than through the
// one predicate: a leaf carrying each permission set, and the walk's
// verdict. `mapped` exactly where the set is legal and present,
// `not_present` where it is empty, `misconfigured` otherwise - and the
// permissions reported are the set itself, since every entry above the
// leaf grants everything.
constexpr bool the_walk_agrees_with_the_rules()
{
    for (int i = 0; i < 16; ++i) {
        for (auto execute_only : {false, true}) {
            auto wanted = permissions_number(i);

            tree of;
            wanted.apply_to(of.pte);

            auto result = walk(of, translated, execute_only);

            if (!wanted.present()) {
                if (zpp::arch::x86_64::vmx::ept_walk_status::not_present !=
                    result.status) {
                    return false;
                }
                continue;
            }

            if (wanted != wanted.normalised(execute_only)) {
                if (zpp::arch::x86_64::vmx::ept_walk_status::
                        misconfigured != result.status) {
                    return false;
                }
                continue;
            }

            if (zpp::arch::x86_64::vmx::ept_walk_status::mapped !=
                result.status) {
                return false;
            }

            if (result.permissions != wanted) {
                return false;
            }
        }
    }
    return true;
}

static_assert(the_walk_agrees_with_the_rules());

// ---------------------------------------------------------------------------
// compose_ept: who owns the fault, and what gets installed.
// ---------------------------------------------------------------------------

constexpr zpp::arch::x86_64::vmx::ept_walk_result
mapped_at(std::uint64_t physical,
          std::uint64_t shift,
          zpp::arch::x86_64::vmx::ept_permissions permissions,
          zpp::arch::x86_64::memory_type type =
              zpp::arch::x86_64::memory_type::write_back)
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = zpp::arch::x86_64::vmx::ept_walk_status::mapped;
    result.physical_address = physical;
    result.page_shift = shift;
    result.permissions = permissions;
    result.type = type;
    return result;
}

constexpr zpp::arch::x86_64::vmx::ept_walk_result
failed_with(zpp::arch::x86_64::vmx::ept_walk_status status)
{
    zpp::arch::x86_64::vmx::ept_walk_result result;
    result.status = status;
    result.permissions = zpp::arch::x86_64::vmx::ept_permissions();
    return result;
}

constexpr auto host_rwx_2mb = mapped_at(
    0x40000000, 21, rwx, zpp::arch::x86_64::memory_type::write_back);

// The ordinary case: both sides map it, both grant everything.
constexpr auto plain = zpp::arch::x86_64::vmx::compose_ept(
    mapped_at(0x8000, 12, rwx), host_rwx_2mb, false);
static_assert(plain.outcome ==
              zpp::arch::x86_64::vmx::ept_compose_outcome::composed);
static_assert(plain.permissions == rwx);
static_assert(plain.physical_address == 0x40000000);

// The page size is the *smaller* of the two. A 2 MB mapping on our side
// under a 4 KB one of L1's must not become a 2 MB shadow leaf, or the
// shadow grants a whole 2 MB the permissions of one page.
static_assert(plain.page_shift == 12);
static_assert(zpp::arch::x86_64::vmx::compose_ept(mapped_at(0, 21, rwx),
                                                  host_rwx_2mb,
                                                  false)
                  .page_shift == 21);
static_assert(zpp::arch::x86_64::vmx::compose_ept(mapped_at(0, 30, rwx),
                                                  host_rwx_2mb,
                                                  false)
                  .page_shift == 21);

// The memory type is ours, never L1's - the recorded divergence.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        mapped_at(0, 12, rwx, zpp::arch::x86_64::memory_type::uncachable),
        mapped_at(0, 21, rwx, zpp::arch::x86_64::memory_type::write_back),
        false)
        .type == zpp::arch::x86_64::memory_type::write_back);

// Permissions intersect. A page we watch - write removed on our side -
// stays installable as read-only rather than becoming L1's business.
constexpr auto watched = zpp::arch::x86_64::vmx::compose_ept(
    mapped_at(0, 12, rwx),
    mapped_at(
        0,
        21,
        zpp::arch::x86_64::vmx::ept_permissions(true, false, true, true)),
    false);
static_assert(watched.outcome ==
              zpp::arch::x86_64::vmx::ept_compose_outcome::composed);
static_assert(!watched.permissions.write());
static_assert(watched.permissions.read());

// A gap in L1's tables is L1's to hear about, and the two ways of having
// one produce the same answer.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        host_rwx_2mb,
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::reflect_violation);
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        failed_with(
            zpp::arch::x86_64::vmx::ept_walk_status::address_out_of_range),
        host_rwx_2mb,
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::reflect_violation);

// A misconfiguration in L1's tables is reflected as one, not as a
// violation.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        failed_with(
            zpp::arch::x86_64::vmx::ept_walk_status::misconfigured),
        host_rwx_2mb,
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::reflect_misconfiguration);

// L1's tables are consulted first, so a fault they explain is theirs even
// where ours would also have refused. Getting this order wrong absorbs a
// fault L1 is waiting for.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::reflect_violation);

// Our own gap, with L1's tables fine, is ours.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        mapped_at(0, 12, rwx),
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied);
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(
        mapped_at(0, 12, rwx),
        failed_with(
            zpp::arch::x86_64::vmx::ept_walk_status::misconfigured),
        false)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied);

// A module page - everything cleared on our side - is ours, and is never
// reflected. L1's tables map it perfectly well and it must not be told
// otherwise.
static_assert(zpp::arch::x86_64::vmx::compose_ept(
                  mapped_at(0, 12, rwx), mapped_at(0, 21, nothing), false)
                  .outcome ==
              zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied);

// Two permission sets overlapping in nothing is also ours, for the same
// reason: L1's walk succeeded, so saying its tables refused would be
// false.
static_assert(
    zpp::arch::x86_64::vmx::compose_ept(mapped_at(0, 12, read_only),
                                        mapped_at(0, 21, execute_no_read),
                                        true)
        .outcome ==
    zpp::arch::x86_64::vmx::ept_compose_outcome::host_denied);

// And whatever is composed is always something the processor accepts,
// which is normalisation applied after the intersection rather than
// before.
constexpr bool composition_always_legal()
{
    for (int i = 1; i < 16; ++i) {
        for (int j = 1; j < 16; ++j) {
            auto left = zpp::arch::x86_64::vmx::ept_permissions(
                0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
            auto right = zpp::arch::x86_64::vmx::ept_permissions(
                0 != (j & 1), 0 != (j & 2), 0 != (j & 4), 0 != (j & 8));

            for (auto execute_only : {false, true}) {
                auto composed = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_at(0, 12, left),
                    mapped_at(0, 21, right),
                    execute_only);

                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        composed != composed.outcome) {
                    continue;
                }

                auto permissions = composed.permissions;

                if (permissions.read()) {
                    continue;
                }

                if (permissions.write()) {
                    return false;
                }

                if (!execute_only && (permissions.execute() ||
                                      permissions.execute_user())) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(composition_always_legal());

// A composition never grants what either side withheld - the property that
// makes the shadow safe rather than merely plausible.
constexpr bool composition_never_widens()
{
    for (int i = 1; i < 16; ++i) {
        for (int j = 1; j < 16; ++j) {
            auto left = zpp::arch::x86_64::vmx::ept_permissions(
                0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
            auto right = zpp::arch::x86_64::vmx::ept_permissions(
                0 != (j & 1), 0 != (j & 2), 0 != (j & 4), 0 != (j & 8));

            for (auto execute_only : {false, true}) {
                auto composed = zpp::arch::x86_64::vmx::compose_ept(
                    mapped_at(0, 12, left),
                    mapped_at(0, 21, right),
                    execute_only);

                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        composed != composed.outcome) {
                    continue;
                }

                auto got = composed.permissions;

                if (got.read() && !(left.read() && right.read())) {
                    return false;
                }
                if (got.write() && !(left.write() && right.write())) {
                    return false;
                }
                if (got.execute() &&
                    !(left.execute() && right.execute())) {
                    return false;
                }
                if (got.execute_user() &&
                    !(left.execute_user() && right.execute_user())) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(composition_never_widens());

// ---------------------------------------------------------------------------
// The reflected exit qualification.
// ---------------------------------------------------------------------------

// Everything hardware reported about the *tables* must be replaced, and
// everything it reported about the *access* kept. Start from a
// qualification with every bit set, so anything not deliberately kept
// shows up as dropped.
constexpr std::uint64_t every_bit = ~std::uint64_t{};

constexpr auto reflected_all_granted =
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        every_bit, mapped_at(0, 12, rwx), true);

// The access bits survive: 2:0 access type, 7 linear address valid, 8
// translation of a linear address, 12 NMI unblocking, 13 shadow stack, 16
// asynchronous.
static_assert(0 != (reflected_all_granted & 0x7));
static_assert(0 != (reflected_all_granted & (1ull << 7)));
static_assert(0 != (reflected_all_granted & (1ull << 8)));
static_assert(0 != (reflected_all_granted & (1ull << 12)));
static_assert(0 != (reflected_all_granted & (1ull << 13)));
static_assert(0 != (reflected_all_granted & (1ull << 16)));

// The permission bits are L1's, so with everything granted they are all
// set.
static_assert(0 != (reflected_all_granted & (1ull << 3)));
static_assert(0 != (reflected_all_granted & (1ull << 4)));
static_assert(0 != (reflected_all_granted & (1ull << 5)));
static_assert(0 != (reflected_all_granted & (1ull << 6)));

// The bits belonging to capabilities this VMM does not report are cleared
// rather than forwarded, even though hardware had them set: 11:9 advanced
// VM-exit information, 14 supervisor shadow stack, 15 guest-paging
// verification, and everything above 16.
static_assert(0 == (reflected_all_granted & (0x7ull << 9)));
static_assert(0 == (reflected_all_granted & (1ull << 14)));
static_assert(0 == (reflected_all_granted & (1ull << 15)));
static_assert(0 == (reflected_all_granted >> 17));

// Bit 6 is left clear without mode-based execute control, whatever L1's
// tables say, because Table 30-7 leaves its value undefined there.
static_assert(
    0 == (zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
              every_bit, mapped_at(0, 12, rwx), false) &
          (1ull << 6)));

// The permission bits reflect L1's tables and not hardware's. A read-only
// mapping in L1 reports readable and not writable, even though the
// qualification handed in claims writable.
constexpr auto reflected_read_only =
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        every_bit, mapped_at(0, 12, read_only), true);
static_assert(0 != (reflected_read_only & (1ull << 3)));
static_assert(0 == (reflected_read_only & (1ull << 4)));
static_assert(0 == (reflected_read_only & (1ull << 5)));
static_assert(0 == (reflected_read_only & (1ull << 6)));

// A walk that found nothing present reports no permissions at all - Note 2
// and Note 3 to Table 30-7 - and the walker having cleared them is what
// makes that fall out rather than needing a case here.
constexpr auto reflected_absent =
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        every_bit,
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        true);
static_assert(0 == (reflected_absent & (0xfull << 3)));

constexpr auto reflected_out_of_range =
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        every_bit,
        failed_with(
            zpp::arch::x86_64::vmx::ept_walk_status::address_out_of_range),
        true);
static_assert(0 == (reflected_out_of_range & (0xfull << 3)));

// And with nothing set in the incoming qualification, nothing is invented.
static_assert(
    0 ==
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        0,
        failed_with(zpp::arch::x86_64::vmx::ept_walk_status::not_present),
        true));

// ---------------------------------------------------------------------------
// The same qualification, exhaustively: every access type against every
// permission set, both with and without mode-based execute control.
//
// SDM Table 30-7 (sdm.txt:203865) row by row, and each row below names the
// line it was read at:
//
//  bits 2:0 - "Set if the access causing the EPT violation was a data
//             read / a data write / an instruction fetch"
//             (sdm.txt:203867-203869). Hardware's, and kept.
//  bits 5:3 - "The logical-AND of bit 0 / bit 1 / bit 2 in the EPT
//             paging-structure entries used" (sdm.txt:203870-203878).
//             Synthesised from the walk of the guest hypervisor's tables,
//             because the entries hardware used were the shadow's.
//  bit 6    - "If the 'mode-based execute control' VM-execution control is
//             0, the value of this bit is undefined" (sdm.txt:203879).
// ---------------------------------------------------------------------------

// The three access bits, kept exactly as hardware reported them and never
// derived from anything else. Driven with every permission set so that a
// synthesis reaching into bits 2:0 shows up here rather than as a guest
// hypervisor being told the wrong access faulted.
constexpr bool the_access_bits_are_hardware_s()
{
    for (int access = 0; access < 8; ++access) {
        for (int i = 0; i < 16; ++i) {
            for (auto mode_based : {false, true}) {
                auto got = zpp::arch::x86_64::vmx::
                    reflected_ept_violation_qualification(
                        std::uint64_t(access),
                        mapped_at(0, 12, permissions_number(i)),
                        mode_based);

                if (std::uint64_t(access) != (got & 0x7)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(the_access_bits_are_hardware_s());

// Bits 5:3 are the guest hypervisor's own permissions, bit for bit, and
// bit 6 is its user-execute permission only where mode-based execute
// control is on. Driven with an incoming qualification of all ones, so a
// bit that is forwarded rather than synthesised reads as set and is
// caught.
constexpr bool the_permission_bits_are_the_guest_s()
{
    for (int i = 0; i < 16; ++i) {
        auto permissions = permissions_number(i);

        for (auto mode_based : {false, true}) {
            auto got = zpp::arch::x86_64::vmx::
                reflected_ept_violation_qualification(
                    every_bit, mapped_at(0, 12, permissions), mode_based);

            if (permissions.read() != (0 != (got & (1ull << 3)))) {
                return false;
            }
            if (permissions.write() != (0 != (got & (1ull << 4)))) {
                return false;
            }
            if (permissions.execute() != (0 != (got & (1ull << 5)))) {
                return false;
            }

            auto wanted_user = mode_based && permissions.execute_user();
            if (wanted_user != (0 != (got & (1ull << 6)))) {
                return false;
            }
        }
    }
    return true;
}

static_assert(the_permission_bits_are_the_guest_s());

// Nothing outside the bits named above ever survives, whatever hardware
// reported. Stated as a mask rather than bit by bit so that a bit added to
// the "keep" set has to be added here too - which is the point, since
// every bit not listed is one whose meaning depends on a capability this
// VMM does not report.
constexpr std::uint64_t carried_bits =
    (0x7ull << 0) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) |
    (1ull << 7) | (1ull << 8) | (1ull << 12) | (1ull << 13) | (1ull << 16);

constexpr bool nothing_outside_the_carried_bits()
{
    for (int i = 0; i < 16; ++i) {
        for (auto mode_based : {false, true}) {
            auto got = zpp::arch::x86_64::vmx::
                reflected_ept_violation_qualification(
                    every_bit,
                    mapped_at(0, 12, permissions_number(i)),
                    mode_based);

            if (0 != (got & ~carried_bits)) {
                return false;
            }
        }
    }
    return true;
}

static_assert(nothing_outside_the_carried_bits());

// The withheld bits, named one at a time, because "cleared by a mask" and
// "cleared on purpose" are different claims and only the second survives
// somebody widening the mask.
//
// Bits 11:9 need "advanced VM-exit information for EPT violations", which
// Note 4 to Table 30-7 (sdm.txt:203945) makes a bit of
// IA32_VMX_EPT_VPID_CAP. This VMM does not report it, so the SDM leaves
// all three undefined and forwarding one is how a guest comes to depend
// on it.
static_assert(0 == (reflected_all_granted & (0x7ull << 9)));

// Bit 14 is defined only "if supervisor shadow-stack control is enabled
// (by setting bit 7 of EPTP)" (sdm.txt:203925), and bit 7 of the EPT
// pointer is refused outright - see the eptp checks below, which make
// bits 11:7 reserved.
static_assert(0 == (reflected_all_granted & (1ull << 14)));

// Bit 15 needs guest-paging verification (sdm.txt:203929), which is a
// secondary control this VMM neither sets nor offers.
static_assert(0 == (reflected_all_granted & (1ull << 15)));

// Bit 22 in particular, which the shadow-EPT audit recorded as withheld.
// It is not a defined bit in this SDM revision at all - Table 30-7 ends
// "63:17 Not currently defined" (sdm.txt:203934) - and the reason it is
// worth an assertion of its own rather than being left to the mask is that
// it was *reported* as deliberately withheld, and a bit somebody believes
// is a decision must be one.
static_assert(0 == (reflected_all_granted & (1ull << 22)));

// The whole of 63:17, for the same reason and in one line.
static_assert(0 == (reflected_all_granted >> 17));

// Note 2 to Table 30-7 (sdm.txt:203939): "Bits 5:3 are cleared to 0 if
// either (1) any of EPT paging-structure entries used to translate the
// guest-physical address of the access causing the EPT violation is not
// present; or (2) 4-level EPT is in use and the guest-physical address
// sets any bits in the range 51:48". Note 3 (sdm.txt:203942) says the same
// of bit 6. Both fall out of the walker having cleared its accumulated
// permissions, which is what makes them a property of the walk rather than
// a case here - so both walk failures are driven through, with mode-based
// execute control on and off.
constexpr bool a_failed_walk_reports_no_permissions()
{
    for (auto status :
         {zpp::arch::x86_64::vmx::ept_walk_status::not_present,
          zpp::arch::x86_64::vmx::ept_walk_status::address_out_of_range,
          zpp::arch::x86_64::vmx::ept_walk_status::misconfigured}) {
        for (auto mode_based : {false, true}) {
            auto got = zpp::arch::x86_64::vmx::
                reflected_ept_violation_qualification(
                    every_bit, failed_with(status), mode_based);

            if (0 != (got & (0xfull << 3))) {
                return false;
            }
        }
    }
    return true;
}

static_assert(a_failed_walk_reports_no_permissions());

// ---------------------------------------------------------------------------
// Which level's refusal produces which exit, as a table over every access
// type and every pair of permissions.
//
// This is the shape of the rule `9d9c525` fixed. The permissions alone do
// not decide it: `compose_ept` reports `composed` whenever the
// intersection is non-empty, and a page the guest hypervisor *watches* -
// write cleared, read and execute kept - intersects to a perfectly valid
// read-and-execute mapping. So a write to it arrives as `composed` and
// looks exactly like a fault on a page only this VMM protects.
//
// The rule is that the guest hypervisor's own tables are consulted first.
// The two statements below are the specification of that, and neither is
// derived from the order the implementation uses:
//
//   - if the guest hypervisor's own permissions refuse the access, the
//     exit is *its* business and must be reflected, whatever ours say,
//   - if only ours refuse it, it is ours and must not be reflected.
//
// The decision itself is not pure - it writes the guest hypervisor's VMCS
// and reaches the watched-page machinery - so it is driven from `main`
// against the real body, cut out of nested_entry.cpp. What can be settled
// here is the classification the rows are graded against.
// ---------------------------------------------------------------------------

// Which access an exit qualification describes, in the bits SDM Table 30-7
// gives them (sdm.txt:203867).
constexpr std::uint64_t access_bit_read = 1ull << 0;
constexpr std::uint64_t access_bit_write = 1ull << 1;
constexpr std::uint64_t access_bit_fetch = 1ull << 2;

constexpr bool
permits(const zpp::arch::x86_64::vmx::ept_permissions & permissions,
        std::uint64_t access)
{
    return ((0 == (access & access_bit_read)) || permissions.read()) &&
           ((0 == (access & access_bit_write)) || permissions.write()) &&
           ((0 == (access & access_bit_fetch)) || permissions.execute());
}

// The named case, and the reason the ordering exists at all: a page the
// guest hypervisor watches composes to a *valid* mapping, so its own fault
// is invisible in the outcome and visible only in its permissions.
constexpr auto watched_by_the_guest_hypervisor =
    zpp::arch::x86_64::vmx::ept_permissions(true, false, true, true);

constexpr auto guest_watched_page = zpp::arch::x86_64::vmx::compose_ept(
    mapped_with(watched_by_the_guest_hypervisor),
    mapped_with(rwx, 21),
    false);

static_assert(guest_watched_page.outcome ==
              zpp::arch::x86_64::vmx::ept_compose_outcome::composed);
static_assert(guest_watched_page.permissions.read());
static_assert(guest_watched_page.permissions.execute());
static_assert(!guest_watched_page.permissions.write());

// So the composition permits a read and refuses a write, and the level
// that refused it is the guest hypervisor's - which is exactly what the
// composition cannot say and the walk of its tables can.
static_assert(permits(guest_watched_page.permissions, access_bit_read));
static_assert(!permits(guest_watched_page.permissions, access_bit_write));
static_assert(!permits(watched_by_the_guest_hypervisor, access_bit_write));

// The mirror image, which must go the other way: a page *this VMM* watches
// over tables that grant everything. Same composition, opposite owner.
constexpr auto watched_here =
    zpp::arch::x86_64::vmx::ept_permissions(true, false, true, true);

constexpr auto host_watched_page = zpp::arch::x86_64::vmx::compose_ept(
    mapped_with(rwx), mapped_with(watched_here, 21), false);

static_assert(host_watched_page.outcome ==
              zpp::arch::x86_64::vmx::ept_compose_outcome::composed);
static_assert(!permits(host_watched_page.permissions, access_bit_write));
static_assert(permits(rwx, access_bit_write));

// The qualification a reflected fault carries says which permission the
// guest hypervisor removed, which is the whole of what it needs. A write
// to a page it watches reports readable and executable and *not* writable,
// even though hardware reported every bit set.
constexpr auto reflected_for_the_watched_page =
    zpp::arch::x86_64::vmx::reflected_ept_violation_qualification(
        every_bit | access_bit_write,
        mapped_at(0, 12, watched_by_the_guest_hypervisor),
        false);

static_assert(0 != (reflected_for_the_watched_page & access_bit_write));
static_assert(0 != (reflected_for_the_watched_page & (1ull << 3)));
static_assert(0 == (reflected_for_the_watched_page & (1ull << 4)));
static_assert(0 != (reflected_for_the_watched_page & (1ull << 5)));

// And the classification itself, over every access type and every pair:
// each cell belongs to exactly one of three owners, and no cell is
// unowned. The runtime rows below grade the real decision against this.
enum class fault_owner
{
    // The guest hypervisor's own tables refuse it.
    reflect_to_the_guest_hypervisor,

    // Its tables permit it and the composition does not.
    ours,

    // Both permit it, so nothing refused it and the shadow is behind.
    install,
};

constexpr fault_owner
owner_of(zpp::arch::x86_64::vmx::ept_permissions guest,
         zpp::arch::x86_64::vmx::ept_permissions host,
         std::uint64_t access,
         bool execute_only)
{
    if (!permits(guest, access)) {
        return fault_owner::reflect_to_the_guest_hypervisor;
    }

    auto composed = zpp::arch::x86_64::vmx::compose_ept(
        mapped_with(guest), mapped_with(host, 21), execute_only);

    if (zpp::arch::x86_64::vmx::ept_compose_outcome::composed !=
        composed.outcome) {
        return fault_owner::ours;
    }

    if (!permits(composed.permissions, access)) {
        return fault_owner::ours;
    }

    return fault_owner::install;
}

// The property that makes the ordering right, stated without reference to
// any ordering: a cell the guest hypervisor's tables refuse is never ours,
// no matter what ours say about it. Getting this wrong absorbs a fault the
// guest hypervisor is waiting for.
constexpr bool a_guest_hypervisor_gap_is_never_absorbed()
{
    for (auto access :
         {access_bit_read, access_bit_write, access_bit_fetch}) {
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                auto guest = permissions_number(i);
                if (permits(guest, access)) {
                    continue;
                }

                for (auto execute_only : {false, true}) {
                    if (fault_owner::reflect_to_the_guest_hypervisor !=
                        owner_of(guest,
                                 permissions_number(j),
                                 access,
                                 execute_only)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static_assert(a_guest_hypervisor_gap_is_never_absorbed());

// And the converse: a cell only *we* refuse is never reflected. Telling a
// guest hypervisor its own tables denied an access they permit sends it
// looking for a bug in them.
constexpr bool our_own_gap_is_never_reflected()
{
    for (auto access :
         {access_bit_read, access_bit_write, access_bit_fetch}) {
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                auto guest = permissions_number(i);
                auto host = permissions_number(j);

                if (!permits(guest, access)) {
                    continue;
                }

                for (auto execute_only : {false, true}) {
                    auto owner =
                        owner_of(guest, host, access, execute_only);

                    if (fault_owner::reflect_to_the_guest_hypervisor ==
                        owner) {
                        return false;
                    }

                    auto wanted = permits(host, access)
                                      ? fault_owner::install
                                      : fault_owner::ours;

                    // Normalisation can remove a permission the
                    // intersection kept - execute without read, where
                    // execute-only translations are not offered - so a
                    // cell both levels permit can still be ours. That is
                    // a legal answer and the only one; it is never the
                    // guest hypervisor's.
                    if ((owner != wanted) &&
                        (fault_owner::ours != owner)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static_assert(our_own_gap_is_never_reflected());

// ===========================================================================
// Tier two: the real bodies, cut out of the hypervisor's own sources.
//
// Everything below runs rather than compiling, because everything below
// writes into arrays that are members of `hypervisor` and are megabytes
// long. Nothing here re-implements what it checks: the four fragments in
// `nested-ept-extracted.inc` are cut verbatim out of nested_ept.cpp and
// nested_entry.cpp by check-nested-ept.sh, and the class below is a
// stand-in carrying only the members those fragments name - the
// tests/watched_page arrangement, which exists so a renamed function or a
// moved fragment fails the extraction instead of silently testing nothing.
//
// Two deliberate divergences from the real class, both of which would
// otherwise be invisible:
//
//   - `on_unhandled_exit` is [[noreturn]] in the real header and returns
//     here. The fragments only reach it after a log and a record, and
//     giving it a return lets the row that provokes it be graded rather
//     than ending the process.
//   - `shadow_ept_tables_per_cpu` and `shadow_ept_slots` are spelled as
//     constants here rather than derived from `nested_vmx::enabled`.
//     check-nested-ept.sh greps hypervisor.h for both values, so the two
//     cannot drift apart quietly.
// ===========================================================================

#include "zpp/arch/x86_64/vmx/msr.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/error.h"
#include <cstddef>
#include <cstring>
#include <expected>
#include <map>

#if !__has_include("nested-ept-extracted.inc")
#error "nested-ept-extracted.inc is generated by check-nested-ept.sh"
#endif

namespace zpp::hypervisor
{
using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;

/**
 * The log, reduced to a sink. Same shape as the real one - constructed as
 * a temporary at the call site - so the extracted bodies compile against
 * the source the hypervisor does.
 */
template <typename... Types>
struct log
{
    log(const char *, Types &&...)
    {
    }
};

template <typename... Types>
log(const char *, Types &&...) -> log<Types...>;

/**
 * Stands in for `arch::x86_64::context`, which the fault decision only
 * passes through.
 */
struct guest_context
{
};

class hypervisor
{
public:
    enum class error
    {
        success = 0,
        physical_to_virtual_capacity_error = 4,
        out_of_shadow_ept_tables = 20,
        nested_controls_unsupported = 21,
    };

    enum class l2_exit_outcome
    {
        reflected,
        handled,

        // Reached only by falling off the end of the extracted fragment,
        // which in the real function is where the mapping is installed.
        deferred,
    };

    /**
     * What the real fault path did about a fault, mirrored here because
     * the extracted fragment names these.
     *
     * The order has to match the real enumeration, since the tests below
     * assert on the value rather than on the name and a reordering there
     * would silently change what they assert.
     */
    enum class l2_ept_disposition : std::uint64_t
    {
        none,
        without_ept,
        reflected_walk,
        reflected_misconfiguration,
        reflected_permission,
        watched,
        unwatched,
        installed,
        install_failed,
        pointer_failed,
    };

    /**
     * The real function's stall detector, reduced to what the extracted
     * fragment needs from it: a record of the branch taken, and the
     * outcome passed straight back.
     *
     * Kept rather than stubbed away, because *which* branch answered a
     * fault is exactly what the tests below are about. Every one of the
     * dispositions is a correct answer to some fault and a livelock in
     * answer to the wrong one, and asserting the outcome alone cannot
     * tell a reflection to the guest hypervisor from a reflection this
     * VMM decided on its own behalf.
     */
    l2_exit_outcome finish(l2_ept_disposition disposition,
                           l2_exit_outcome outcome)
    {
        this->last_disposition = disposition;
        return outcome;
    }

    l2_ept_disposition last_disposition{};

    static constexpr std::size_t max_cpus = 2;
    static constexpr std::size_t page_size = 0x1000;

    static constexpr std::size_t shadow_ept_tables_per_cpu = 96;
    static constexpr std::size_t shadow_ept_slots = 4;
    static constexpr std::uint8_t shadow_table_free = 0;
    static constexpr bool execute_only_translations_offered = false;

    // --------------------------------------------- the code under test
    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    shadow_ept_table(std::size_t cpu);

    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    shadow_ept_entry(std::size_t cpu,
                     std::uint64_t guest_physical,
                     std::uint64_t shift);

    void release_shadow_slot(std::size_t cpu, std::size_t slot);

    /**
     * The composed branch of `on_l2_ept_fault`, wrapped so it can be
     * called: everything before it decides nothing about ownership, and
     * everything after it installs a mapping.
     */
    l2_exit_outcome l2_fault_decision(
        std::size_t cpu,
        std::uint64_t reason,
        guest_context & context,
        std::uint64_t guest_physical,
        std::uint64_t qualification,
        const arch::x86_64::vmx::ept_walk_result & guest_walk,
        const arch::x86_64::vmx::ept_composition & composition);

    /**
     * The checks `build_vmcs02` puts on the guest hypervisor's own EPT
     * pointer, wrapped the same way.
     */
    std::expected<void, zpp::error> eptp_accepted(std::uint64_t eptp12);

    // ------------------------------------------- defined by the harness
    std::uint64_t physical_address_bits();
    std::uint64_t nested_vmx_capability_msr(std::size_t msr);

    void reflect_l2_exit(std::size_t cpu,
                         std::uint64_t reason,
                         std::uint64_t qualification);

    bool on_ept_violation(std::size_t cpu,
                          guest_context & context,
                          std::uint64_t physical_address);

    void record_exit(std::size_t cpu,
                     std::uint64_t reason,
                     guest_context & context);
    void on_unhandled_exit(std::uint64_t reason);

    struct page_table_stub
    {
        std::uint64_t virtual_to_physical(const void * address) const;
    };

    // --------------------------------------------------------- state
    page_table_stub host_page_table{};
    std::map<std::uint64_t, std::uint64_t> module_physical_to_virtual{};

    alignas(page_size) arch::x86_64::vmx::epte
        shadow_epml4[max_cpus][shadow_ept_slots][512]{};
    alignas(page_size) arch::x86_64::vmx::epte
        shadow_ept_tables[max_cpus][shadow_ept_tables_per_cpu][512]{};

    std::uint8_t shadow_ept_table_slot[max_cpus]
                                      [shadow_ept_tables_per_cpu]{};
    std::size_t shadow_ept_current_slot[max_cpus]{};
    std::size_t shadow_ept_tables_used[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_source[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_generation_seen[max_cpus][shadow_ept_slots]{};

    // ------------------------------------------------ instrumentation
    std::uint64_t reported_physical_address_bits{46};
    std::uint64_t reported_ept_capability{};

    std::uint64_t reflections{};
    std::uint64_t last_reflected_reason{};
    std::uint64_t last_reflected_qualification{};

    std::uint64_t ept_violations{};
    std::uint64_t last_ept_violation_address{};
    bool something_here_watches{true};

    std::uint64_t unhandled_exits{};
};

/**
 * The hypervisor error category, so zpp::error can carry the codes above.
 */
inline const zpp::error_category & category(hypervisor::error)
{
    constexpr static auto error_category = zpp::make_error_category(
        "hypervisor",
        hypervisor::error::success,
        [](auto) -> std::string_view { return "hypervisor"; });
    return error_category;
}

/**
 * The pool's physical addresses, invented here. Nothing dereferences them
 * - the shadow walk goes back through `module_physical_to_virtual`, which
 * is the reverse map the real one uses because the module is not identity
 * mapped.
 */
constexpr std::uint64_t pool_physical_base = 0x200000;

hypervisor & instance();

std::uint64_t hypervisor::physical_address_bits()
{
    return this->reported_physical_address_bits;
}

std::uint64_t hypervisor::nested_vmx_capability_msr(std::size_t msr)
{
    return (arch::x86_64::vmx::msr::vpid_ept_capability == msr)
               ? this->reported_ept_capability
               : 0;
}

void hypervisor::reflect_l2_exit(std::size_t,
                                 std::uint64_t reason,
                                 std::uint64_t qualification)
{
    ++this->reflections;
    this->last_reflected_reason = reason;
    this->last_reflected_qualification = qualification;
}

bool hypervisor::on_ept_violation(std::size_t,
                                  guest_context &,
                                  std::uint64_t physical_address)
{
    ++this->ept_violations;
    this->last_ept_violation_address = physical_address;
    return this->something_here_watches;
}

void hypervisor::record_exit(std::size_t, std::uint64_t, guest_context &)
{
}

void hypervisor::on_unhandled_exit(std::uint64_t)
{
    ++this->unhandled_exits;
}

std::uint64_t hypervisor::page_table_stub::virtual_to_physical(
    const void * address) const
{
    auto & self = instance();

    for (std::size_t cpu{}; cpu < max_cpus; ++cpu) {
        for (std::size_t i{}; i < shadow_ept_tables_per_cpu; ++i) {
            if (address == &self.shadow_ept_tables[cpu][i][0]) {
                return pool_physical_base +
                       (((cpu * shadow_ept_tables_per_cpu) + i) *
                        page_size);
            }
        }
    }

    return 0;
}

} // namespace zpp::hypervisor

#include "nested-ept-extracted.inc"

// ---------------------------------------------------------------------------
// The harness.
// ---------------------------------------------------------------------------

namespace zpp::hypervisor
{
namespace
{
hypervisor the_hypervisor;
} // namespace

hypervisor & instance()
{
    return the_hypervisor;
}

} // namespace zpp::hypervisor

using zpp::hypervisor::guest_context;
using zpp::hypervisor::hypervisor;

namespace
{
std::size_t checks{};
std::size_t failures{};

void check(bool condition, const char * what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("nested_ept: FAILED %s\n", what);
    }
}

/**
 * Puts the pool back to the state a freshly constructed hypervisor has,
 * and rebuilds the reverse map the shadow walk reaches its own tables
 * through.
 */
void reset_pool(hypervisor & of)
{
    std::memset(of.shadow_epml4, 0, sizeof(of.shadow_epml4));
    std::memset(of.shadow_ept_tables, 0, sizeof(of.shadow_ept_tables));
    std::memset(of.shadow_ept_table_slot,
                hypervisor::shadow_table_free,
                sizeof(of.shadow_ept_table_slot));
    std::memset(
        of.shadow_ept_current_slot, 0, sizeof(of.shadow_ept_current_slot));
    std::memset(
        of.shadow_ept_tables_used, 0, sizeof(of.shadow_ept_tables_used));
    std::memset(of.shadow_ept_source, 0, sizeof(of.shadow_ept_source));
    std::memset(of.shadow_ept_generation_seen,
                0,
                sizeof(of.shadow_ept_generation_seen));

    of.module_physical_to_virtual.clear();

    for (std::size_t cpu{}; cpu < hypervisor::max_cpus; ++cpu) {
        for (std::size_t i{}; i < hypervisor::shadow_ept_tables_per_cpu;
             ++i) {
            auto physical =
                zpp::hypervisor::pool_physical_base +
                (((cpu * hypervisor::shadow_ept_tables_per_cpu) + i) *
                 hypervisor::page_size);

            of.module_physical_to_virtual[physical] =
                reinterpret_cast<std::uint64_t>(
                    &of.shadow_ept_tables[cpu][i][0]);
        }
    }
}

std::size_t free_tables(const hypervisor & of, std::size_t cpu)
{
    std::size_t count{};
    for (std::size_t i{}; i < hypervisor::shadow_ept_tables_per_cpu; ++i) {
        if (hypervisor::shadow_table_free ==
            of.shadow_ept_table_slot[cpu][i]) {
            ++count;
        }
    }
    return count;
}

/**
 * The shadow's own entry for an address at a level, followed the way the
 * hypervisor follows it - through the reverse map, because the module is
 * not identity mapped.
 */
zpp::arch::x86_64::vmx::epte *
shadow_entry_at(hypervisor & of,
                std::size_t cpu,
                std::size_t slot,
                std::uint64_t guest_physical,
                std::uint64_t level)
{
    auto * table = &of.shadow_epml4[cpu][slot][0];

    for (auto walking = std::uint64_t{3};; --walking) {
        auto index = (guest_physical >> (12 + (9 * walking))) & 0x1ff;
        auto & entry = table[index];

        if (walking == level) {
            return &entry;
        }

        if (!zpp::arch::x86_64::vmx::ept_permissions::of(entry)
                 .present() ||
            entry.large()) {
            return nullptr;
        }

        auto found =
            of.module_physical_to_virtual.find(entry.page_number() << 12);
        if (of.module_physical_to_virtual.end() == found) {
            return nullptr;
        }

        table = reinterpret_cast<zpp::arch::x86_64::vmx::epte *>(
            found->second);
    }
}

// The address every shadow test below is built around, chosen so each
// level's index is different and none is zero.
constexpr std::uint64_t shadow_address =
    (1ull << 39) | (2ull << 30) | (3ull << 21);

// -------------------------------------------------------------------------
// `23bdddc`: a large shadow mapping standing where a smaller one is
// needed.
//
// The walk descended whenever an entry at a level was *present*, with no
// test for whether it was a leaf. That could not happen while the shadow
// was built top down in one pass, and is routine once it is filled a fault
// at a time: a 2 MB mapping goes in first, and a later fault inside it
// needs 4 KB because this VMM's own tables split that region or watch a
// page of it. The walk then took the leaf's *guest frame number* for a
// table address, failed to find it among the module's pages, and returned
// physical_to_virtual_capacity_error - which reads as "the module's page
// map is too small" and was nothing of the kind.
//
// Measured then: `could not install a shadow leaf for 0x11c4c9000: error
// 0x4`, followed by an unhandled EPT violation. Error 4 is what this test
// asserts against, so a regression reproduces the reported symptom exactly
// rather than merely failing.
// -------------------------------------------------------------------------
void a_large_leaf_is_dropped_for_a_smaller_one()
{
    auto & of = zpp::hypervisor::instance();
    reset_pool(of);
    of.shadow_ept_current_slot[0] = 0;

    auto large = of.shadow_ept_entry(0, shadow_address, 21);
    check(large.has_value(), "a 2 MB shadow entry can be reached at all");
    if (!large) {
        return;
    }

    // Two tables above a page-directory entry: the page-directory-pointer
    // table and the page directory itself.
    check(free_tables(of, 0) ==
              (hypervisor::shadow_ept_tables_per_cpu - 2),
          "reaching a 2 MB entry takes two tables from the pool");

    zpp::arch::x86_64::vmx::epte leaf;
    rwx.apply_to(leaf);
    leaf.large(true);
    leaf.type(zpp::arch::x86_64::memory_type::write_back);
    leaf.large_page_number(0x8000);
    **large = leaf;

    auto guest_frame = (*large)->page_number();
    check(0 != guest_frame, "the large leaf names a page");
    check(of.module_physical_to_virtual.end() ==
              of.module_physical_to_virtual.find(guest_frame << 12),
          "the large leaf's frame is guest RAM, not one of ours");

    auto before = free_tables(of, 0);

    auto small = of.shadow_ept_entry(0, shadow_address + 0x3000, 12);

    if (!small) {
        ++checks;
        ++failures;
        std::printf("nested_ept: FAILED a 4 KB entry inside a 2 MB leaf, "
                    "error %d\n",
                    small.error().code());
        return;
    }

    check(true, "a 4 KB entry can be reached inside a 2 MB leaf");

    auto * page_directory_entry =
        shadow_entry_at(of, 0, 0, shadow_address, 1);
    check(nullptr != page_directory_entry,
          "the page-directory entry is still reachable");
    if (nullptr == page_directory_entry) {
        return;
    }

    check(!page_directory_entry->large(),
          "the large entry was dropped rather than descended into");
    check(
        zpp::arch::x86_64::vmx::ept_permissions::of(*page_directory_entry)
            .present(),
        "a table entry replaced it");
    check(of.module_physical_to_virtual.end() !=
              of.module_physical_to_virtual.find(
                  page_directory_entry->page_number() << 12),
          "the replacement names a table this VMM allocated");

    check(free_tables(of, 0) == (before - 1),
          "exactly one table was taken to replace the large entry");

    // The returned entry is inside the table that was just created, and is
    // absent - a table is zeroed on being handed out, and an entry of all
    // zeroes is not present per SDM 31.3.2 (sdm.txt:205446).
    auto found = of.module_physical_to_virtual.find(
        page_directory_entry->page_number() << 12);
    auto * created =
        reinterpret_cast<zpp::arch::x86_64::vmx::epte *>(found->second);

    check(*small == (created + 3),
          "the 4 KB entry is the right slot of the new table");
    check(!zpp::arch::x86_64::vmx::ept_permissions::of(**small).present(),
          "the new table is handed out empty");

    // And the mappings the large entry covered are gone rather than
    // silently retained, which is what makes them fault back in one at a
    // time.
    for (std::size_t i{}; i < 512; ++i) {
        if (zpp::arch::x86_64::vmx::ept_permissions::of(created[i])
                .present()) {
            check(false, "the dropped 2 MB region left a mapping behind");
            return;
        }
    }
    check(true, "the dropped 2 MB region left no mapping behind");
}

// -------------------------------------------------------------------------
// `e49ec9f`: releasing a shadow slot must return its tables to the shared
// pool.
//
// A slot that kept them while reporting it held nothing owned pages no
// build could reach and no build would reclaim, and four of those exhaust
// a ninety-six table pool with no shadow live at all. It had not bitten
// yet only because a discarded slot was always the next one rebuilt.
// -------------------------------------------------------------------------
void releasing_a_slot_returns_its_tables()
{
    auto & of = zpp::hypervisor::instance();
    reset_pool(of);

    auto empty = free_tables(of, 0);
    check(hypervisor::shadow_ept_tables_per_cpu == empty,
          "the pool starts wholly free");

    // Four cycles, because the leak this pins showed up only once the
    // pool had been round several times: one cycle leaves plenty free.
    for (std::size_t cycle{}; cycle < 4; ++cycle) {
        for (std::size_t slot{}; slot < hypervisor::shadow_ept_slots;
             ++slot) {
            of.shadow_ept_current_slot[0] = slot;
            of.shadow_ept_source[0][slot] = 0x1000 + slot;

            for (std::size_t page{}; page < 3; ++page) {
                auto address = shadow_address + (slot * (1ull << 30)) +
                               (page * (1ull << 21));

                auto entry = of.shadow_ept_entry(0, address, 12);
                check(entry.has_value(),
                      "a shadow entry can be reached in every slot");
                if (!entry) {
                    return;
                }
            }

            check(0 != of.shadow_ept_tables_used[0][slot],
                  "a slot that was built into reports tables in use");
        }

        check(free_tables(of, 0) < empty,
              "building four shadows takes tables from the pool");

        for (std::size_t slot{}; slot < hypervisor::shadow_ept_slots;
             ++slot) {
            of.release_shadow_slot(0, slot);
        }

        check(free_tables(of, 0) == empty,
              "releasing every slot returns every table to the pool");

        for (std::size_t slot{}; slot < hypervisor::shadow_ept_slots;
             ++slot) {
            check(0 == of.shadow_ept_tables_used[0][slot],
                  "a released slot reports no tables in use");
            check(0 == of.shadow_ept_source[0][slot],
                  "a released slot reports no source");

            // And its root is empty.
            //
            // A released slot's PML4 named tables the pool has just
            // marked free, so a walk through it would follow entries
            // into tables another slot is now filling. Safe until now
            // only because both callers memset the root on the very next
            // line - which is a contract kept by convention, in two
            // places, with nothing saying so. A third caller that
            // released a slot without knowing that would get a shadow
            // sharing tables with its neighbour and no fault anywhere.
            auto * root = of.shadow_epml4[0][slot];
            auto root_is_empty = true;
            for (std::size_t i{}; i < 512; ++i) {
                if (0 != root[i].value()) {
                    root_is_empty = false;
                }
            }
            check(root_is_empty,
                  "a released slot's root is empty - it must not still "
                  "name tables the pool has handed back");
        }

        // Releasing empties the root, and the EPT pointer still stays
        // stable across rebuilds - the root's *address* is never
        // recycled, which is what the pointer names, and clearing its
        // contents does not move it.
        //
        // This used to be the other way round, with the clearing left to
        // the callers: `shadow_ept_pointer_for` and
        // `refresh_shadow_ept_for` both memset the root on the line after
        // releasing, and `discard_shadow_ept` did not, being safe only
        // because it also cleared the source so nothing could reach the
        // slot before a rebuild. Three callers, two conventions and one
        // exception, with nothing saying so - which is a contract waiting
        // for a fourth caller.
        check(!zpp::arch::x86_64::vmx::ept_permissions::of(
                   of.shadow_epml4[0][0][1])
                   .present(),
              "releasing a slot empties its root, so no caller has to "
              "remember to");

        std::memset(of.shadow_epml4[0], 0, sizeof(of.shadow_epml4[0]));
    }

    // Releasing one slot must not take another's tables with it. The
    // ownership byte is the slot's index plus one for exactly this
    // reason.
    reset_pool(of);

    for (std::size_t slot{}; slot < 2; ++slot) {
        of.shadow_ept_current_slot[0] = slot;
        static_cast<void>(of.shadow_ept_entry(
            0, shadow_address + (slot * (1ull << 30)), 12));
    }

    auto both = free_tables(of, 0);
    of.release_shadow_slot(0, 0);

    check(free_tables(of, 0) > both, "releasing one slot frees something");
    check(free_tables(of, 0) < empty,
          "releasing one slot does not free the other's tables");
    check(0 != of.shadow_ept_tables_used[0][1],
          "the other slot still owns its tables");

    // And the whole pool is usable again afterwards, which is the property
    // the leak destroyed: a pool that reports free tables it cannot hand
    // out fails as "out of shadow ept tables" with nothing live.
    reset_pool(of);
    of.shadow_ept_current_slot[0] = 0;

    std::size_t handed_out{};
    for (std::size_t i{}; i < hypervisor::shadow_ept_tables_per_cpu; ++i) {
        if (of.shadow_ept_table(0)) {
            ++handed_out;
        }
    }

    check(hypervisor::shadow_ept_tables_per_cpu == handed_out,
          "every table in the pool can be handed out");
    check(!of.shadow_ept_table(0).has_value(),
          "the pool refuses rather than overruns");

    of.release_shadow_slot(0, 0);
    check(free_tables(of, 0) == empty,
          "releasing returns a wholly consumed pool");
}

// -------------------------------------------------------------------------
// `ept_pointer`'s own accessors.
//
// The class nothing calls, which is why it was wrong in three ways at
// once - and why it is worth fixing rather than deleting: `build_vmcs02`
// tests bit 6 with a literal `0x40` because reaching for the accessor
// would have given the wrong answer, so the class being wrong is the
// reason the code beside it does not use it.
//
// SDM 29.2.1.1 (sdm.txt:202160): "Bit 6 (enable bit for accessed and
// dirty flags for EPT) must be 0 if bit 21 of the IA32_VMX_EPT_VPID_CAP
// MSR ... is read as 0". Bit 8 is a different thing entirely - it is the
// accessed flag of a leaf *entry*, and SDM Table 31-7 (sdm.txt:205542)
// makes it meaningful only "If bit 6 of EPTP is 1". Reading the enable
// out of bit 8 confuses the pointer with the entry it points at.
//
// Static, because every one of these is `constexpr` and the compile is
// then the test. A runtime tier would add nothing: there is no state and
// no ordering, only arithmetic that is either right or wrong.
//
// **Not reachable under Bochs**, and worth saying why rather than
// leaving it as an omission: nothing in the tree calls these accessors,
// so no instruction a guest can execute reaches them. A Bochs case would
// have to call them from the guest test suite, which would be testing a
// copy of the arithmetic compiled into the loader rather than the
// hypervisor's use of it - and the hypervisor has no use of it yet. When
// `build_vmcs02` stops testing bit 6 with a literal and reaches for the
// accessor instead, the Bochs suite's existing EPT cases cover it for
// free.
// -------------------------------------------------------------------------
static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{};
        pointer.access_and_dirty(true);
        return pointer.value();
    }() == (1ull << 6),
    "setting accessed-and-dirty must set bit 6 of the EPT pointer and "
    "nothing else - SDM 29.2.1.1, sdm.txt:202160");

static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{1ull << 6};
        return pointer.access_and_dirty();
    }(),
    "and reading it back must read bit 6, not bit 8 - bit 8 is a leaf "
    "entry's accessed flag, which Table 31-7 makes meaningful only when "
    "this bit is set (sdm.txt:205542)");

static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{1ull << 8};
        return pointer.access_and_dirty();
    }() == false,
    "a pointer with bit 8 set and bit 6 clear does not have accessed and "
    "dirty flags enabled");

static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{~std::uint64_t{}};
        pointer.access_and_dirty(false);
        return pointer.value() & (1ull << 6);
    }() == 0,
    "clearing it clears bit 6");

// And the bits it must not disturb on the way. The setter was copied
// from `page_walk_length`, which legitimately stores its value minus
// one - so `access_and_dirty(false)` evaluated `((0 - 1) & 0x7) << 8`
// and **set** bits 10:8, which are reserved and which `build_vmcs02`
// refuses in a guest hypervisor's own pointer. A setter that makes a
// pointer invalid by clearing a flag is worse than one that does
// nothing.
static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{};
        pointer.access_and_dirty(false);
        return pointer.value();
    }() == 0,
    "clearing accessed-and-dirty on an empty pointer leaves it empty - "
    "it must not set bits 10:8, which SDM 29.2.1.1 reserves and which "
    "build_vmcs02 refuses");

static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{};
        pointer.memory_type(zpp::arch::x86_64::memory_type::write_back);
        pointer.page_walk_length(4);
        pointer.page_number(0x100);
        auto before = pointer.value();
        pointer.access_and_dirty(true);
        pointer.access_and_dirty(false);
        return pointer.value() == before;
    }(),
    "and setting it then clearing it leaves every other field as it was, "
    "so the accessor composes with the ones beside it");

static_assert(
    [] {
        zpp::arch::x86_64::vmx::ept_pointer pointer{};
        pointer.memory_type(zpp::arch::x86_64::memory_type::write_back);
        pointer.page_walk_length(4);
        pointer.page_number(0x100);
        pointer.access_and_dirty(true);
        return (pointer.page_walk_length() == 4) &&
               (pointer.page_number() == 0x100) &&
               (pointer.memory_type() ==
                zpp::arch::x86_64::memory_type::write_back);
    }(),
    "setting accessed-and-dirty does not disturb the walk length, the "
    "root or the memory type");

// -------------------------------------------------------------------------
// `e4e7e96`: the guest hypervisor's own EPT pointer.
//
// SDM 29.2.1.1 (sdm.txt:202154) gives the checks, and KVM applies the same
// five in the same order in `nested_vmx_check_eptp`
// (.references/kvm/nested.c:2790). The one that was missing is bit 6:
// "Bit 6 (enable bit for accessed and dirty flags for EPT) must be 0 if
// bit 21 of the IA32_VMX_EPT_VPID_CAP MSR ... is read as 0"
// (sdm.txt:202160), which is KVM's "AD, if set, should be supported"
// (.references/kvm/nested.c:2826). The shadow sets neither flag, the
// capability MSR withholds the bit, and without this check the pointer was
// accepted and the promise quietly broken.
// -------------------------------------------------------------------------
constexpr std::uint64_t eptp_walk_length_4 = 3ull << 3;
constexpr std::uint64_t eptp_accessed_and_dirty = 1ull << 6;
constexpr std::uint64_t ept_capability_accessed_and_dirty = 1ull << 21;

std::uint64_t eptp_with(std::uint64_t memory_type_value,
                        std::uint64_t walk_length_field,
                        std::uint64_t extra,
                        std::uint64_t root = 0x100000)
{
    return memory_type_value | walk_length_field | extra | root;
}

void the_eptp_checks_hold()
{
    auto & of = zpp::hypervisor::instance();
    of.reported_physical_address_bits = 46;

    // A realistic report: uncacheable and write-back paging structures
    // and a four-level walk, which is what this VMM's own
    // `supported_ept_vpid_capabilities` offers on hardware that has them.
    //
    // Set rather than left at zero, and that is the change: these checks
    // now *depend* on it. SDM 29.2.1.1 (sdm.txt:202156) says "The EPT
    // memory type (bits 2:0) must be a value supported by the processor
    // as indicated in the IA32_VMX_EPT_VPID_CAP MSR", and the line below
    // says the same of the walk length. Both used to be tested against
    // constants, so a capability of zero admitted both anyway - the
    // asymmetry e4e7e96 removed for bit 6 and left beside it. The
    // capability-driven cases at the end of this function are what pin
    // the fix.
    constexpr std::uint64_t capability_walk_length_4 = 1ull << 6;
    constexpr std::uint64_t capability_uncacheable = 1ull << 8;
    constexpr std::uint64_t capability_write_back = 1ull << 14;

    of.reported_ept_capability = capability_walk_length_4 |
                                 capability_uncacheable |
                                 capability_write_back;

    auto write_back = static_cast<std::uint64_t>(
        zpp::arch::x86_64::memory_type::write_back);
    auto uncachable = static_cast<std::uint64_t>(
        zpp::arch::x86_64::memory_type::uncachable);

    auto plain = eptp_with(write_back, eptp_walk_length_4, 0);

    check(of.eptp_accepted(plain).has_value(),
          "a write-back four-level pointer is accepted");
    check(of.eptp_accepted(eptp_with(uncachable, eptp_walk_length_4, 0))
              .has_value(),
          "an uncacheable four-level pointer is accepted");

    // The three memory types the SDM permits in an EPT entry but which
    // the pointer may not carry, plus the reserved ones.
    for (auto refused : {1ull, 2ull, 3ull, 4ull, 5ull, 7ull}) {
        check(!of.eptp_accepted(eptp_with(refused, eptp_walk_length_4, 0))
                   .has_value(),
              "a pointer with an unreported memory type is refused");
    }

    // Bits 5:3 hold the walk length minus one. Only four is reported, so
    // three and five are both refused - and five in particular, because a
    // 5-level walk is the one a processor might really support.
    for (auto field : {0ull, 1ull, 2ull, 4ull, 5ull, 6ull, 7ull}) {
        check(!of.eptp_accepted(eptp_with(write_back, field << 3, 0))
                   .has_value(),
              "a pointer with an unreported walk length is refused");
    }

    // Bit 6 with the capability withheld, which is the whole of e4e7e96.
    // The other three stay reported, or the refusal below would be for
    // the wrong reason - which is exactly the trap a capability of zero
    // used to hide.
    check(!of.eptp_accepted(eptp_with(write_back,
                                      eptp_walk_length_4,
                                      eptp_accessed_and_dirty))
               .has_value(),
          "accessed and dirty flags are refused while unreported");

    // And accepted once reported, so the check follows the capability
    // rather than a constant - which is what lets the bit be implemented
    // without this becoming wrong.
    of.reported_ept_capability = ept_capability_accessed_and_dirty |
                                 capability_walk_length_4 |
                                 capability_write_back;
    check(of.eptp_accepted(eptp_with(write_back,
                                     eptp_walk_length_4,
                                     eptp_accessed_and_dirty))
              .has_value(),
          "accessed and dirty flags are accepted once reported");
    of.reported_ept_capability = 0;

    // === The memory type and the walk length follow the capability ===
    //
    // SDM 29.2.1.1 (sdm.txt:202156): "The EPT memory type (bits 2:0) must
    // be a value supported by the processor as indicated in the
    // IA32_VMX_EPT_VPID_CAP MSR", and the line below it says the same of
    // the walk length. Appendix A.10 gives the bits: 8 for uncacheable
    // (sdm.txt:223503), 14 for write-back (:223505), 6 for a page-walk
    // length of 4 (:223501).
    //
    // Bit 6 of the *pointer* was made to follow the capability by
    // e4e7e96. These two were left testing constants, which is the same
    // asymmetry one line over - and it is a promise broken in the
    // direction that matters: this VMM reports
    // `hardware & supported_ept_vpid_capabilities`, so on a processor
    // that does not report uncacheable paging structures it would tell a
    // guest hypervisor so and then accept a pointer asking for them.
    //
    // KVM tests the reported bits - VMX_EPTP_UC_BIT, VMX_EPTP_WB_BIT and
    // VMX_EPT_PAGE_WALK_4_BIT in `nested_vmx_check_eptp`
    // (.references/kvm/nested.c:2794, v6.12).
    {
        constexpr std::uint64_t capability_uncacheable = 1ull << 8;
        constexpr std::uint64_t capability_write_back = 1ull << 14;
        constexpr std::uint64_t capability_walk_length_4 = 1ull << 6;

        of.reported_ept_capability = capability_walk_length_4;
        check(
            !of.eptp_accepted(eptp_with(write_back, eptp_walk_length_4, 0))
                 .has_value(),
            "write-back is refused while the capability does not report "
            "it");
        check(
            !of.eptp_accepted(eptp_with(uncachable, eptp_walk_length_4, 0))
                 .has_value(),
            "and uncacheable likewise");

        of.reported_ept_capability =
            capability_walk_length_4 | capability_write_back;
        check(
            of.eptp_accepted(eptp_with(write_back, eptp_walk_length_4, 0))
                .has_value(),
            "write-back is accepted once reported");
        check(
            !of.eptp_accepted(eptp_with(uncachable, eptp_walk_length_4, 0))
                 .has_value(),
            "and reporting write-back does not admit uncacheable");

        of.reported_ept_capability =
            capability_walk_length_4 | capability_uncacheable;
        check(
            of.eptp_accepted(eptp_with(uncachable, eptp_walk_length_4, 0))
                .has_value(),
            "uncacheable is accepted once reported");
        check(
            !of.eptp_accepted(eptp_with(write_back, eptp_walk_length_4, 0))
                 .has_value(),
            "and reporting uncacheable does not admit write-back");

        of.reported_ept_capability = capability_write_back;
        check(
            !of.eptp_accepted(eptp_with(write_back, eptp_walk_length_4, 0))
                 .has_value(),
            "a four-level walk is refused while the capability does not "
            "report one");

        of.reported_ept_capability =
            capability_write_back | capability_walk_length_4;
        check(
            of.eptp_accepted(eptp_with(write_back, eptp_walk_length_4, 0))
                .has_value(),
            "and accepted once reported");

        // Put back the realistic report, because the cases after this
        // one are about the reserved bits and the address width and
        // would otherwise be refused for a reason they are not testing.
        of.reported_ept_capability = capability_walk_length_4 |
                                     capability_uncacheable |
                                     capability_write_back;
    }

    // Reserved bits 11:7, one at a time (sdm.txt:202163). Bit 7 is
    // supervisor shadow-stack control, which is why exit qualification bit
    // 14 above can never be meaningful here.
    for (auto bit = 7; bit <= 11; ++bit) {
        check(
            !of.eptp_accepted(
                   eptp_with(write_back, eptp_walk_length_4, 1ull << bit))
                 .has_value(),
            "a pointer with a reserved bit set is refused");
    }

    // The address width. Bit 45 is inside 46 bits and bit 46 is not.
    check(of.eptp_accepted(
                eptp_with(write_back, eptp_walk_length_4, 0, 1ull << 45))
              .has_value(),
          "a root inside the address width is accepted");
    check(!of.eptp_accepted(
                 eptp_with(write_back, eptp_walk_length_4, 0, 1ull << 46))
               .has_value(),
          "a root past the address width is refused");
    check(!of.eptp_accepted(
                 eptp_with(write_back, eptp_walk_length_4, 0, 1ull << 63))
               .has_value(),
          "a root with bit 63 set is refused");

    // And the width is the processor's rather than a constant.
    of.reported_physical_address_bits = 39;
    check(!of.eptp_accepted(
                 eptp_with(write_back, eptp_walk_length_4, 0, 1ull << 45))
               .has_value(),
          "the address check follows the reported width");
    of.reported_physical_address_bits = 46;

    check(!of.eptp_accepted(0).has_value(),
          "an all-zero pointer is refused, its walk length being one");
}

// -------------------------------------------------------------------------
// `9d9c525`: which level's refusal produces which exit.
//
// Every access type against every pair of permissions, driven through the
// real decision. The classification each row is graded against is
// `owner_of` above, which says only *which level lacks the permission* -
// it knows nothing about the order the implementation consults them in,
// which is the point.
// -------------------------------------------------------------------------
void the_owner_of_every_fault()
{
    auto & of = zpp::hypervisor::instance();
    guest_context context;

    // Something here watches every page, so a fault attributed to this VMM
    // is answered rather than stopping the processor. The row that checks
    // the other way round is below.
    of.something_here_watches = true;

    constexpr std::uint64_t first_level_address = 0x40000;
    constexpr std::uint64_t second_level_address = 0x123000;

    for (auto access :
         {access_bit_read, access_bit_write, access_bit_fetch}) {
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                auto guest = permissions_number(i);
                auto host = permissions_number(j);

                auto guest_walk = mapped_with(guest);
                guest_walk.physical_address = first_level_address;

                auto composition = zpp::arch::x86_64::vmx::compose_ept(
                    guest_walk, mapped_with(host, 21), false);

                // Only the composed branch is under test here. The other
                // outcomes are decided before it and are covered by the
                // static assertions above.
                if (zpp::arch::x86_64::vmx::ept_compose_outcome::
                        composed != composition.outcome) {
                    continue;
                }

                of.reflections = 0;
                of.ept_violations = 0;
                of.unhandled_exits = 0;

                auto outcome = of.l2_fault_decision(0,
                                                    48,
                                                    context,
                                                    second_level_address,
                                                    access,
                                                    guest_walk,
                                                    composition);

                char what[160];
                std::snprintf(what,
                              sizeof(what),
                              "access %llu, guest %d, host %d",
                              static_cast<unsigned long long>(access),
                              i,
                              j);

                switch (owner_of(guest, host, access, false)) {
                case fault_owner::reflect_to_the_guest_hypervisor:
                    check(hypervisor::l2_exit_outcome::reflected ==
                                  outcome &&
                              (1 == of.reflections) &&
                              (0 == of.ept_violations),
                          what);

                    // The qualification it is given says which permission
                    // it removed, from the walk of its own tables.
                    check(guest.read() ==
                              (0 != (of.last_reflected_qualification &
                                     (1ull << 3))),
                          "a reflected qualification reports L1's read");
                    check(guest.write() ==
                              (0 != (of.last_reflected_qualification &
                                     (1ull << 4))),
                          "a reflected qualification reports L1's write");
                    check(guest.execute() ==
                              (0 != (of.last_reflected_qualification &
                                     (1ull << 5))),
                          "a reflected qualification reports L1's "
                          "execute");
                    check(access ==
                              (of.last_reflected_qualification & 0x7),
                          "a reflected qualification keeps the access");
                    check(static_cast<std::uint64_t>(
                              zpp::hypervisor::basic_reason::
                                  ept_violation) ==
                              of.last_reflected_reason,
                          "a reflected fault is an EPT violation");
                    break;

                case fault_owner::ours:
                    check(hypervisor::l2_exit_outcome::handled ==
                                  outcome &&
                              (0 == of.reflections) &&
                              (1 == of.ept_violations),
                          what);
                    check(first_level_address ==
                              of.last_ept_violation_address,
                          "the watch is keyed on the first-level "
                          "address");
                    break;

                case fault_owner::install:
                    check(hypervisor::l2_exit_outcome::deferred ==
                                  outcome &&
                              (0 == of.reflections) &&
                              (0 == of.ept_violations),
                          what);
                    break;
                }
            }
        }
    }
}

// The two named cases, spelled out rather than left inside the sweep,
// because they are the ones that motivated the ordering and the ones a
// reader will look for.
void a_page_the_guest_hypervisor_watches()
{
    auto & of = zpp::hypervisor::instance();
    guest_context context;
    of.something_here_watches = true;

    auto guest_walk = mapped_with(watched_by_the_guest_hypervisor);
    guest_walk.physical_address = 0x40000;

    auto composition = zpp::arch::x86_64::vmx::compose_ept(
        guest_walk, mapped_with(rwx, 21), false);

    check(zpp::arch::x86_64::vmx::ept_compose_outcome::composed ==
              composition.outcome,
          "a page L1 watches composes to a valid mapping");

    of.reflections = 0;
    of.ept_violations = 0;

    auto outcome = of.l2_fault_decision(0,
                                        48,
                                        context,
                                        0x123000,
                                        access_bit_write,
                                        guest_walk,
                                        composition);

    check(hypervisor::l2_exit_outcome::reflected == outcome,
          "a write to a page L1 watches is reflected to L1");
    check(1 == of.reflections, "and it is reflected exactly once");
    check(0 == of.ept_violations,
          "and it never reaches the watched-page machinery here");
    check(0 == (of.last_reflected_qualification & (1ull << 4)),
          "and L1 is told its own tables refused the write");
}

void a_page_this_vmm_watches()
{
    auto & of = zpp::hypervisor::instance();
    guest_context context;
    of.something_here_watches = true;

    auto guest_walk = mapped_with(rwx);
    guest_walk.physical_address = 0x40000;

    auto composition = zpp::arch::x86_64::vmx::compose_ept(
        guest_walk, mapped_with(watched_here, 21), false);

    check(zpp::arch::x86_64::vmx::ept_compose_outcome::composed ==
              composition.outcome,
          "a page we watch composes to a valid mapping too");

    of.reflections = 0;
    of.ept_violations = 0;

    auto outcome = of.l2_fault_decision(0,
                                        48,
                                        context,
                                        0x123000,
                                        access_bit_write,
                                        guest_walk,
                                        composition);

    check(hypervisor::l2_exit_outcome::handled == outcome,
          "a write to a page we watch is answered here");
    check(0 == of.reflections,
          "and L1 is never told its own tables refused it");
    check(1 == of.ept_violations, "and the watch is consulted");

    // A page nothing here watches stops the processor rather than being
    // resumed from, which is the "which nothing here watches" path.
    of.something_here_watches = false;
    of.unhandled_exits = 0;
    of.reflections = 0;

    static_cast<void>(of.l2_fault_decision(0,
                                           48,
                                           context,
                                           0x123000,
                                           access_bit_write,
                                           guest_walk,
                                           composition));

    check(1 == of.unhandled_exits,
          "a page nothing here watches stops the processor");
    check(0 == of.reflections, "and is still never blamed on L1's tables");

    of.something_here_watches = true;
}

} // namespace

int main()
{
    a_large_leaf_is_dropped_for_a_smaller_one();
    releasing_a_slot_returns_its_tables();
    the_eptp_checks_hold();
    the_owner_of_every_fault();
    a_page_the_guest_hypervisor_watches();
    a_page_this_vmm_watches();

    std::printf(
        "nested_ept: %zu checks, %zu failures\n", checks, failures);

    return (0 == failures) ? 0 : 1;
}
