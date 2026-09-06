/**
 * The user-mode (address space, instruction pointer) census.
 *
 * `zpp/hypervisor/user_rip_census.h` depends on `<cstdint>`, `<cstddef>`
 * and `<span>` and on nothing else, so the header under test compiles
 * here exactly as the hypervisor compiles it - no shim, no stand-in and
 * no hypervisor object. That is why the functions are free functions in
 * the first place: `note_hot_rip`, the census this one is modelled on,
 * is a member of a class no host can construct, and in consequence its
 * eviction rule has never been executed by anything but a real boot.
 *
 * What it pins, in the order the failures would matter:
 *
 *  - **A cr3 that could not be obtained must read as "not recorded".**
 *    `note_user_rip` returns false and writes nothing, so no row can
 *    ever carry cr3 zero and "not recorded" is distinguishable from
 *    "recorded as zero". Every instrument in this tree that lacked that
 *    property has at some point reported health for ever after the
 *    thing it counts stopped.
 *  - **One address in two address spaces is two rows.** This is the
 *    whole point of the instrument. A module walk cannot separate them,
 *    because ASLR gives a system DLL one base per boot rather than one
 *    per process - `win32u.dll` was found at the identical address in
 *    `csrss.exe` and in `WerFault.exe`, both walks proof-passed. If the
 *    key were the address alone this census would be the same dead end
 *    with a larger table.
 *  - **The mask.** Both sides of the join carry low bits: every
 *    `DirectoryTableBase` in the guest ends `...002`. A process whose
 *    context identifier moves must stay one row, and the stored value
 *    must be the masked one so the reader's compare can succeed.
 *  - **Decay, with a negative control.** A hot pair keeps its slot
 *    however late it appears and a cold one loses it. The control is
 *    the arithmetic that makes that true: a row with n hits survives
 *    exactly n collisions, no more and no fewer.
 *  - **The dictionary is not the table.** It is linear and does not
 *    evict, which is what lets it disagree with the hashed table and
 *    report that table's saturation. A test that only exercised one of
 *    them would not notice if they were quietly made the same thing.
 */
#include "zpp/hypervisor/user_rip_census.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
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
    std::println(
        "FAIL: {}\n  expected {}\n  actual   {}", what, expected, actual);
}

using zpp::hypervisor::cr3_page_frame_mask;
using zpp::hypervisor::is_user_address;
using zpp::hypervisor::note_user_cr3;
using zpp::hypervisor::note_user_rip;
using zpp::hypervisor::user_address_limit;
using zpp::hypervisor::user_rip_slot;

/** The singleton's shape, at a capacity a test can reason about. */
constexpr std::size_t capacity = 64;

struct table
{
    std::array<std::uint64_t, capacity> cr3s{};
    std::array<std::uint64_t, capacity> rips{};
    std::array<std::uint64_t, capacity> hits{};
    std::uint64_t overflow{};

    bool note(std::uint64_t cr3, std::uint64_t rip)
    {
        return note_user_rip(cr3s, rips, hits, overflow, cr3, rip);
    }

    /** The hits recorded against a pair, wherever it landed. */
    std::uint64_t hits_of(std::uint64_t cr3, std::uint64_t rip) const
    {
        auto frame = cr3 & cr3_page_frame_mask;
        for (std::size_t i{}; i < capacity; ++i) {
            if ((cr3s[i] == frame) && (rips[i] == rip)) {
                return hits[i];
            }
        }
        return 0;
    }

    std::size_t rows() const
    {
        std::size_t count{};
        for (std::size_t i{}; i < capacity; ++i) {
            if (0 != hits[i]) {
                ++count;
            }
        }
        return count;
    }
};

// The addresses this census exists to attribute, taken from the dump
// that raised the question rather than invented: `win32u.dll + 0x32e5`
// and the address two bytes past it, which is the `syscall` and the
// instruction after it rather than a two-byte loop.
constexpr std::uint64_t win32u_syscall = 0x7ffe0eb132e5ull;
constexpr std::uint64_t win32u_after = 0x7ffe0eb132e7ull;
constexpr std::uint64_t ntdll_deep = 0x7ffe11000665ull;

// Two of the guest's own address spaces, with the `...002` low bits
// every `DirectoryTableBase` in it carries.
constexpr std::uint64_t csrss_cr3 = 0x1ab356002ull;
constexpr std::uint64_t werfault_cr3 = 0x1b8fec002ull;

void the_user_half_is_what_is_sampled()
{
    check(is_user_address(win32u_syscall),
          "a win32u address is in the user half");
    check(is_user_address(0), "zero is in the user half");
    check(is_user_address(user_address_limit - 1),
          "the last user address is in the user half");

    // The kernel half, which is deliberately not censused: every
    // process shares it, so attributing one of its addresses says
    // nothing and would cost the table the population it can help.
    check(!is_user_address(user_address_limit),
          "the first non-canonical address is not user mode");
    check(!is_user_address(0xfffff80000000000ull),
          "an ntoskrnl address is not user mode");
    check(!is_user_address(~std::uint64_t{}),
          "the top of the address space is not user mode");
}

void a_cr3_that_could_not_be_obtained_is_refused()
{
    table t{};

    check(!t.note(0, win32u_syscall),
          "a zero cr3 is refused rather than recorded");
    check_equal(0, t.rows(), "a refused sample writes no row");
    check_equal(0, t.overflow, "a refused sample is not contention");

    // The sharp case: low bits set, page frame zero. That is what a
    // half-written or wrongly-current VMCS field looks like, and it
    // must be refused for the same reason a bare zero is.
    check(!t.note(0x002, win32u_syscall),
          "a cr3 whose page frame is zero is refused");
    check_equal(0, t.rows(), "and still writes no row");

    // And the positive control, so "refuses everything" cannot pass
    // this test: the same address with a real cr3 is recorded.
    check(t.note(csrss_cr3, win32u_syscall), "a real cr3 is recorded");
    check_equal(1, t.rows(), "and takes exactly one row");

    // No row can carry cr3 zero. This is the property the reader
    // prints, so it is asserted rather than assumed.
    for (std::size_t i{}; i < capacity; ++i) {
        if (0 != t.hits[i]) {
            check(0 != t.cr3s[i], "no occupied row carries cr3 zero");
        }
    }
}

void one_address_in_two_address_spaces_is_two_rows()
{
    table t{};

    for (int i{}; i < 10; ++i) {
        t.note(csrss_cr3, win32u_syscall);
    }
    for (int i{}; i < 4; ++i) {
        t.note(werfault_cr3, win32u_syscall);
    }

    check_equal(2,
                t.rows(),
                "the same address in two address spaces is two rows - "
                "this is the whole reason the census exists, since a "
                "module walk finds win32u at one base in both");
    check_equal(10,
                t.hits_of(csrss_cr3, win32u_syscall),
                "csrss's share of the address is its own");
    check_equal(4,
                t.hits_of(werfault_cr3, win32u_syscall),
                "and WerFault's is separate from it");
}

void two_addresses_in_one_address_space_are_two_rows()
{
    table t{};

    t.note(csrss_cr3, win32u_syscall);
    t.note(csrss_cr3, win32u_after);
    t.note(csrss_cr3, ntdll_deep);

    check_equal(3,
                t.rows(),
                "three addresses in one address space are three rows");
    check_equal(1,
                t.hits_of(csrss_cr3, win32u_after),
                "the address two bytes on is its own row, not the "
                "same one - it is the instruction after a syscall");
}

void the_page_frame_is_what_is_stored_and_compared()
{
    table t{};

    t.note(csrss_cr3, win32u_syscall);

    auto frame = csrss_cr3 & cr3_page_frame_mask;
    check_equal(0x1ab356000ull, frame, "the mask drops the ...002");
    check_equal(1,
                t.hits_of(frame, win32u_syscall),
                "the stored value is the masked one, so a reader that "
                "masks DirectoryTableBase matches it");

    // A context identifier that moves must not split the process.
    // Unmasked, this is a different number and would be a second row -
    // which is precisely the silent join failure the mask prevents.
    t.note(csrss_cr3 ^ 0x00f, win32u_syscall);
    check_equal(1,
                t.rows(),
                "the same page frame with a different context "
                "identifier is one row, not two");
    check_equal(2,
                t.hits_of(frame, win32u_syscall),
                "and both samples land on it");

    // Bit 63, which CR3 uses to ask the processor not to invalidate,
    // must not split it either.
    t.note(csrss_cr3 | (1ull << 63), win32u_syscall);
    check_equal(1, t.rows(), "nor does bit 63 split it");
    check_equal(3,
                t.hits_of(frame, win32u_syscall),
                "and that sample lands on it too");
}

void a_hot_pair_keeps_its_slot_and_a_cold_one_loses_it()
{
    table t{};

    // Two pairs that collide, found rather than assumed - the point of
    // the test is the eviction rule, and a pair chosen for convenience
    // that did not actually collide would make every check below pass
    // for the wrong reason.
    std::uint64_t hot_rip{};
    std::uint64_t cold_rip{};
    auto want = user_rip_slot(
        csrss_cr3 & cr3_page_frame_mask, win32u_syscall, capacity);

    hot_rip = win32u_syscall;
    for (std::uint64_t candidate = 0x10000; candidate < 0x200000;
         candidate += 2) {
        if (candidate == hot_rip) {
            continue;
        }
        if (user_rip_slot(csrss_cr3 & cr3_page_frame_mask,
                          candidate,
                          capacity) == want) {
            cold_rip = candidate;
            break;
        }
    }

    check(0 != cold_rip, "a colliding pair exists to test eviction with");

    // The hot one first, 100 times.
    for (int i{}; i < 100; ++i) {
        t.note(csrss_cr3, hot_rip);
    }
    check_equal(
        100, t.hits_of(csrss_cr3, hot_rip), "the hot pair accumulates");

    // The cold one 99 times, which is one short of displacing it. The
    // negative control for the check after: if the rule were "the last
    // writer wins" this would already have taken the slot.
    for (int i{}; i < 99; ++i) {
        t.note(csrss_cr3, cold_rip);
    }
    check_equal(1,
                t.hits_of(csrss_cr3, hot_rip),
                "99 collisions decay a 100-hit row to 1 and do not "
                "displace it");
    check_equal(0,
                t.hits_of(csrss_cr3, cold_rip),
                "and the cold pair has taken no row while it is "
                "occupied");
    check_equal(99, t.overflow, "each collision counts as contention");

    // The hundredth empties it, and the hundred-and-first claims it.
    t.note(csrss_cr3, cold_rip);
    t.note(csrss_cr3, cold_rip);
    check_equal(1,
                t.hits_of(csrss_cr3, cold_rip),
                "the pair that keeps arriving eventually takes the "
                "slot - a hot address that appears late is not shut "
                "out, which is what the linear version got wrong");
    check_equal(
        0, t.hits_of(csrss_cr3, hot_rip), "and the decayed one is gone");
}

void the_dictionary_counts_what_the_table_evicts()
{
    std::array<std::uint64_t, 4> seen{};
    std::array<std::uint64_t, 4> hits{};
    std::uint64_t overflow{};

    for (int i{}; i < 7; ++i) {
        check(note_user_cr3(seen, hits, overflow, csrss_cr3),
              "a real cr3 is counted");
    }
    for (int i{}; i < 3; ++i) {
        note_user_cr3(seen, hits, overflow, werfault_cr3);
    }

    check_equal(csrss_cr3,
                seen[0],
                "the dictionary keeps it UNMASKED, "
                "so the ...002 can be read rather "
                "than assumed");
    check_equal(7, hits[0], "and counts every sample");
    check_equal(werfault_cr3, seen[1], "the second address space too");
    check_equal(3, hits[1], "with its own count");
    check_equal(0, overflow, "nothing overflowed yet");

    // A zero cr3 is refused here too, and for the same reason.
    check(!note_user_cr3(seen, hits, overflow, 0),
          "a zero cr3 is refused by the dictionary as well");
    check_equal(0, overflow, "a refusal is not an overflow");

    // Saturation is reported rather than silent. `cr3_seen` truncated
    // quietly until it grew this counter, and the effect was that "no
    // page table maps it" and "the one that did was the ninth" were the
    // same reading.
    note_user_cr3(seen, hits, overflow, 0x200000002ull);
    note_user_cr3(seen, hits, overflow, 0x300000002ull);
    check_equal(4, seen.size(), "the dictionary is full");
    note_user_cr3(seen, hits, overflow, 0x400000002ull);
    check_equal(1,
                overflow,
                "a sample that finds no slot is counted, so a "
                "saturated dictionary says so instead of dropping");

    // And it still counts a resident address space after saturating,
    // which is what makes it a usable control rather than a table that
    // stops.
    note_user_cr3(seen, hits, overflow, csrss_cr3);
    check_equal(8,
                hits[0],
                "a resident space is still counted after "
                "the dictionary saturates");
}

void the_dictionary_does_not_evict_when_the_table_does()
{
    // The two instruments over one stream, which is the disagreement
    // the pair is built to produce. Every sample belongs to one address
    // space; the addresses are spread far wider than the table, so the
    // table's rows are a fraction of them and the dictionary's are all
    // of them.
    table t{};
    std::array<std::uint64_t, 4> seen{};
    std::array<std::uint64_t, 4> hits{};
    std::uint64_t dict_overflow{};

    constexpr std::uint64_t samples = 4000;
    for (std::uint64_t i{}; i < samples; ++i) {
        auto rip = 0x7ff000000000ull + (i * 16);
        t.note(werfault_cr3, rip);
        note_user_cr3(seen, hits, dict_overflow, werfault_cr3);
    }

    check_equal(samples,
                hits[0],
                "the dictionary counts every sample, because it does "
                "not evict");
    check(t.rows() <= capacity,
          "the pair table cannot hold more rows than it has slots");

    std::uint64_t table_total{};
    for (std::size_t i{}; i < capacity; ++i) {
        table_total += t.hits[i];
    }
    check(table_total < samples,
          "the pair table has lost samples to eviction, which the "
          "dictionary has not - a process with a large share in the "
          "dictionary and no row in the table is this, and is the "
          "reading the pair exists to make possible");
    check(0 != t.overflow,
          "and the contention counter says so rather than the "
          "shortfall having to be inferred");
}

} // namespace

int main()
{
    the_user_half_is_what_is_sampled();
    a_cr3_that_could_not_be_obtained_is_refused();
    one_address_in_two_address_spaces_is_two_rows();
    two_addresses_in_one_address_space_are_two_rows();
    the_page_frame_is_what_is_stored_and_compared();
    a_hot_pair_keeps_its_slot_and_a_cold_one_loses_it();
    the_dictionary_counts_what_the_table_evicts();
    the_dictionary_does_not_evict_when_the_table_does();

    std::println(
        "user_rip_census: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
