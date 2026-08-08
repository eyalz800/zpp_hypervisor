// Tests for the nested EPT walker and the permission composition, as
// static_asserts - so the compile is the test run, exactly as with
// decoder-test.cpp beside it.
//
// Hosted rather than freestanding for the same reason: walk_ept is pure.
// It reaches memory only through the callable it is handed, so a table can
// be an array in this file and no target is needed to check the
// architectural rules.
//
// This is the only verification of any of nested VMX that does not need
// hardware, which is why it is worth having: every rule below is one this
// VMM would otherwise only have read.
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include <array>
#include <cstdio>
#include <optional>

using namespace zpp::arch::x86_64;
using namespace zpp::arch::x86_64::vmx;

// ---------------------------------------------------------------------------
// ept_permissions: intersection and normalisation.
// ---------------------------------------------------------------------------

constexpr auto rwx = ept_permissions(true, true, true, true);
constexpr auto read_only = ept_permissions(true, false, false, false);
constexpr auto nothing = ept_permissions();

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
constexpr auto write_no_read = ept_permissions(false, true, false, false);
static_assert(write_no_read.normalised(false) == nothing);
static_assert(write_no_read.normalised(true) == nothing);

// Execute without read is legal only where execute-only translations are
// reported, and then only for execute - never for write.
constexpr auto execute_no_read =
    ept_permissions(false, false, true, false);
static_assert(execute_no_read.normalised(false) == nothing);
static_assert(execute_no_read.normalised(true) == execute_no_read);

constexpr auto user_execute_no_read =
    ept_permissions(false, false, false, true);
static_assert(user_execute_no_read.normalised(false) == nothing);
static_assert(user_execute_no_read.normalised(true) ==
              user_execute_no_read);

// Write and execute without read: the write must go even where
// execute-only is available.
constexpr auto write_execute_no_read =
    ept_permissions(false, true, true, false);
static_assert(write_execute_no_read.normalised(true) == execute_no_read);

// Anything with read is already legal and must be left exactly alone.
static_assert(rwx.normalised(false) == rwx);
static_assert(read_only.normalised(false) == read_only);

// Normalisation only ever removes. Checked as a property rather than by
// example, because "it never adds read" is the safety argument.
constexpr bool never_adds(ept_permissions before, bool execute_only)
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
        auto before = ept_permissions(
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
        auto before = ept_permissions(
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

constexpr epte table_entry(std::uint64_t page_number)
{
    epte entry;
    entry.read(true);
    entry.write(true);
    entry.execute(true);
    entry.execute_user(true);
    entry.page_number(page_number);
    return entry;
}

constexpr epte leaf_entry(std::uint64_t page_number,
                          bool large,
                          memory_type type = memory_type::write_back)
{
    epte entry;
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
    epte pml4{table_entry(pdpt_at >> 12)};
    epte pdpte{table_entry(pd_at >> 12)};
    epte pde{table_entry(pt_at >> 12)};
    epte pte{leaf_entry(0x9abcd, false)};
};

constexpr auto reader_for(const tree & of)
{
    return [&of](std::uint64_t at) -> std::optional<epte> {
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

constexpr ept_walk_result walk(const tree & of,
                               std::uint64_t guest_physical = translated,
                               bool execute_only = false)
{
    return walk_ept(pml4_at,
                    guest_physical,
                    physical_address_bits,
                    execute_only,
                    reader_for(of));
}

// The plain case: four levels, a 4 KB leaf.
static_assert(walk(tree{}).status == ept_walk_status::mapped);
static_assert(walk(tree{}).page_shift == 12);
static_assert(walk(tree{}).physical_address ==
              ((0x9abcdull << 12) | 0x678));
static_assert(walk(tree{}).permissions == rwx);
static_assert(walk(tree{}).type == memory_type::write_back);

// A 2 MB leaf at the page-directory level. The offset kept is bits 20:0 of
// the address, per SDM 31.3.2's "Bits 20:0 are from the original
// guest-physical address".
constexpr tree two_megabyte = [] {
    tree of;
    of.pde = leaf_entry(0, true);
    of.pde.large_page_number(0x123);
    return of;
}();

static_assert(walk(two_megabyte).status == ept_walk_status::mapped);
static_assert(walk(two_megabyte).page_shift == 21);
static_assert(walk(two_megabyte).physical_address ==
              ((0x123ull << 21) | (translated & 0x1fffff)));

// A 1 GB leaf at the page-directory-pointer level.
constexpr tree one_gigabyte = [] {
    tree of;
    of.pdpte = leaf_entry(0, true);
    of.pdpte = epte(of.pdpte.value() | (0x5ull << 30));
    return of;
}();

static_assert(walk(one_gigabyte).status == ept_walk_status::mapped);
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

static_assert(walk(read_only_pdpte).status == ept_walk_status::mapped);
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

static_assert(walk(absent_pde).status == ept_walk_status::not_present);
static_assert(walk(absent_pde).permissions == nothing);

// Write without read is a misconfiguration, at any level.
constexpr tree write_only_pde = [] {
    tree of;
    of.pde.read(false);
    return of;
}();

static_assert(walk(write_only_pde).status ==
              ept_walk_status::misconfigured);

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
              ept_walk_status::misconfigured);
static_assert(walk(execute_only_pde, translated, true).status ==
              ept_walk_status::mapped);

// A reserved memory type in a leaf is a misconfiguration. 2, 3 and 7 are
// the reserved values per SDM 31.3.7.2.
constexpr tree reserved_type = [] {
    tree of;
    of.pte = leaf_entry(0x9abcd, false, memory_type(3));
    return of;
}();

static_assert(walk(reserved_type).status ==
              ept_walk_status::misconfigured);

// A memory type in a *non-leaf* entry is a reserved bit rather than a
// type: SDM Table 31-6 reserves bits 6:3 of a page-directory entry that
// references a page table. initialize_ept already clears them for its own
// splits; here the walker has to notice when a guest does not.
constexpr tree type_in_table_entry = [] {
    tree of;
    of.pde.type(memory_type::write_back);
    return of;
}();

static_assert(walk(type_in_table_entry).status ==
              ept_walk_status::misconfigured);

// An address bit at or above the processor's physical-address width is a
// reserved bit, so also a misconfiguration.
constexpr tree address_too_wide = [] {
    tree of;
    of.pde = epte(of.pde.value() | (1ull << physical_address_bits));
    return of;
}();

static_assert(walk(address_too_wide).status ==
              ept_walk_status::misconfigured);

// A 2 MB leaf must have bits 20:12 of its address clear - SDM Table 31-5.
constexpr tree misaligned_large_leaf = [] {
    tree of;
    of.pde = leaf_entry(0, true);
    of.pde.large_page_number(0x123);
    of.pde = epte(of.pde.value() | (1ull << 13));
    return of;
}();

static_assert(walk(misaligned_large_leaf).status ==
              ept_walk_status::misconfigured);

// Bits 51:48 of a guest-physical address must be zero with 4-level EPT.
static_assert(walk(tree{}, translated | (1ull << 48)).status ==
              ept_walk_status::address_out_of_range);
static_assert(walk(tree{}, translated | (1ull << 48)).permissions ==
              nothing);

// A page-directory entry with bit 7 set is a leaf; a PML4 entry with bit 7
// set is *reserved* and therefore a misconfiguration, not a 512 GB page.
constexpr tree large_pml4e = [] {
    tree of;
    of.pml4.large(true);
    return of;
}();

static_assert(walk(large_pml4e).status == ept_walk_status::misconfigured);

// An entry that cannot be read at all is reported as absent rather than
// mapped, which is what a table outside guest memory has to come out as.
constexpr tree unreachable_table = [] {
    tree of;
    of.pml4 = table_entry(0x7ffff);
    return of;
}();

static_assert(walk(unreachable_table).status ==
              ept_walk_status::not_present);

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
    of.pde.type(memory_type::write_back);
    return of;
}();

static_assert(walk(absent_with_reserved_bit).status ==
              ept_walk_status::not_present);

// ---------------------------------------------------------------------------
// compose_ept: who owns the fault, and what gets installed.
// ---------------------------------------------------------------------------

constexpr ept_walk_result
mapped_at(std::uint64_t physical,
          std::uint64_t shift,
          ept_permissions permissions,
          memory_type type = memory_type::write_back)
{
    ept_walk_result result;
    result.status = ept_walk_status::mapped;
    result.physical_address = physical;
    result.page_shift = shift;
    result.permissions = permissions;
    result.type = type;
    return result;
}

constexpr ept_walk_result failed_with(ept_walk_status status)
{
    ept_walk_result result;
    result.status = status;
    result.permissions = ept_permissions();
    return result;
}

constexpr auto host_rwx_2mb =
    mapped_at(0x40000000, 21, rwx, memory_type::write_back);

// The ordinary case: both sides map it, both grant everything.
constexpr auto plain =
    compose_ept(mapped_at(0x8000, 12, rwx), host_rwx_2mb, false);
static_assert(plain.outcome == ept_compose_outcome::composed);
static_assert(plain.permissions == rwx);
static_assert(plain.physical_address == 0x40000000);

// The page size is the *smaller* of the two. A 2 MB mapping on our side
// under a 4 KB one of L1's must not become a 2 MB shadow leaf, or the
// shadow grants a whole 2 MB the permissions of one page.
static_assert(plain.page_shift == 12);
static_assert(compose_ept(mapped_at(0, 21, rwx), host_rwx_2mb, false)
                  .page_shift == 21);
static_assert(compose_ept(mapped_at(0, 30, rwx), host_rwx_2mb, false)
                  .page_shift == 21);

// The memory type is ours, never L1's - the recorded divergence.
static_assert(compose_ept(mapped_at(0, 12, rwx, memory_type::uncachable),
                          mapped_at(0, 21, rwx, memory_type::write_back),
                          false)
                  .type == memory_type::write_back);

// Permissions intersect. A page we watch - write removed on our side -
// stays installable as read-only rather than becoming L1's business.
constexpr auto watched =
    compose_ept(mapped_at(0, 12, rwx),
                mapped_at(0, 21, ept_permissions(true, false, true, true)),
                false);
static_assert(watched.outcome == ept_compose_outcome::composed);
static_assert(!watched.permissions.write());
static_assert(watched.permissions.read());

// A gap in L1's tables is L1's to hear about, and the two ways of having
// one produce the same answer.
static_assert(compose_ept(failed_with(ept_walk_status::not_present),
                          host_rwx_2mb,
                          false)
                  .outcome == ept_compose_outcome::reflect_violation);
static_assert(
    compose_ept(failed_with(ept_walk_status::address_out_of_range),
                host_rwx_2mb,
                false)
        .outcome == ept_compose_outcome::reflect_violation);

// A misconfiguration in L1's tables is reflected as one, not as a
// violation.
static_assert(compose_ept(failed_with(ept_walk_status::misconfigured),
                          host_rwx_2mb,
                          false)
                  .outcome ==
              ept_compose_outcome::reflect_misconfiguration);

// L1's tables are consulted first, so a fault they explain is theirs even
// where ours would also have refused. Getting this order wrong absorbs a
// fault L1 is waiting for.
static_assert(compose_ept(failed_with(ept_walk_status::not_present),
                          failed_with(ept_walk_status::not_present),
                          false)
                  .outcome == ept_compose_outcome::reflect_violation);

// Our own gap, with L1's tables fine, is ours.
static_assert(compose_ept(mapped_at(0, 12, rwx),
                          failed_with(ept_walk_status::not_present),
                          false)
                  .outcome == ept_compose_outcome::host_denied);
static_assert(compose_ept(mapped_at(0, 12, rwx),
                          failed_with(ept_walk_status::misconfigured),
                          false)
                  .outcome == ept_compose_outcome::host_denied);

// A module page - everything cleared on our side - is ours, and is never
// reflected. L1's tables map it perfectly well and it must not be told
// otherwise.
static_assert(compose_ept(mapped_at(0, 12, rwx),
                          mapped_at(0, 21, nothing),
                          false)
                  .outcome == ept_compose_outcome::host_denied);

// Two permission sets overlapping in nothing is also ours, for the same
// reason: L1's walk succeeded, so saying its tables refused would be
// false.
static_assert(compose_ept(mapped_at(0, 12, read_only),
                          mapped_at(0, 21, execute_no_read),
                          true)
                  .outcome == ept_compose_outcome::host_denied);

// And whatever is composed is always something the processor accepts,
// which is normalisation applied after the intersection rather than
// before.
constexpr bool composition_always_legal()
{
    for (int i = 1; i < 16; ++i) {
        for (int j = 1; j < 16; ++j) {
            auto left = ept_permissions(
                0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
            auto right = ept_permissions(
                0 != (j & 1), 0 != (j & 2), 0 != (j & 4), 0 != (j & 8));

            for (auto execute_only : {false, true}) {
                auto composed = compose_ept(mapped_at(0, 12, left),
                                            mapped_at(0, 21, right),
                                            execute_only);

                if (ept_compose_outcome::composed != composed.outcome) {
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
            auto left = ept_permissions(
                0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8));
            auto right = ept_permissions(
                0 != (j & 1), 0 != (j & 2), 0 != (j & 4), 0 != (j & 8));

            for (auto execute_only : {false, true}) {
                auto composed = compose_ept(mapped_at(0, 12, left),
                                            mapped_at(0, 21, right),
                                            execute_only);

                if (ept_compose_outcome::composed != composed.outcome) {
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
    reflected_ept_violation_qualification(
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
static_assert(0 == (reflected_ept_violation_qualification(
                        every_bit, mapped_at(0, 12, rwx), false) &
                    (1ull << 6)));

// The permission bits reflect L1's tables and not hardware's. A read-only
// mapping in L1 reports readable and not writable, even though the
// qualification handed in claims writable.
constexpr auto reflected_read_only = reflected_ept_violation_qualification(
    every_bit, mapped_at(0, 12, read_only), true);
static_assert(0 != (reflected_read_only & (1ull << 3)));
static_assert(0 == (reflected_read_only & (1ull << 4)));
static_assert(0 == (reflected_read_only & (1ull << 5)));
static_assert(0 == (reflected_read_only & (1ull << 6)));

// A walk that found nothing present reports no permissions at all - Note 2
// and Note 3 to Table 30-7 - and the walker having cleared them is what
// makes that fall out rather than needing a case here.
constexpr auto reflected_absent = reflected_ept_violation_qualification(
    every_bit, failed_with(ept_walk_status::not_present), true);
static_assert(0 == (reflected_absent & (0xfull << 3)));

constexpr auto reflected_out_of_range =
    reflected_ept_violation_qualification(
        every_bit,
        failed_with(ept_walk_status::address_out_of_range),
        true);
static_assert(0 == (reflected_out_of_range & (0xfull << 3)));

// And with nothing set in the incoming qualification, nothing is invented.
static_assert(0 ==
              reflected_ept_violation_qualification(
                  0, failed_with(ept_walk_status::not_present), true));

int main()
{
    std::printf("all nested ept static_asserts passed\n");
    return 0;
}
