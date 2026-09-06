#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zpp::hypervisor
{
/**
 * The (address space, instruction pointer) census for user-mode guest
 * code, as free functions so it can be tested on a host.
 *
 * **Why this exists.** `interrupted_rip` and `quiet_rip` sample where
 * the second-level guest is, and filtered to the user half of the
 * address space one dump held 106,743 samples over 1,973 distinct
 * addresses, the hottest resolving to `win32u.dll + 0x32e5` - a win32k
 * system-call stub. Nothing could say **which process** was making
 * those calls, and two routes were tried and both are closed:
 *
 * - **Module attribution cannot.** ASLR randomises a system DLL's base
 *   once per boot, not per process, so `win32u.dll` sits at the
 *   identical address in `csrss.exe` and in `WerFault.exe` - both walks
 *   proof-passed and both named the same base (`108d445`).
 * - **The exit ring cannot.** It holds the last few hundred exits and
 *   contained zero user-mode instruction pointers, the guest having
 *   been in kernel mode when it stopped. A ring is a snapshot and the
 *   census is cumulative, so they cannot be joined after the fact
 *   (`a622eef`).
 *
 * What is left is the address space itself. `_KPROCESS.Directory-
 * TableBase` maps a CR3 back to a process, and `guest-user-module.py`
 * already reads it for every process, so a user-mode address carrying
 * the CR3 that executed it joins to a name with no new guest walk.
 *
 * **Kernel addresses are deliberately not censused here.** Every
 * process shares them, so attributing one says nothing, and censusing
 * them would multiply the table by the population it cannot help.
 *
 * Free functions taking spans rather than members, for one reason:
 * `note_hot_rip` is a member of a class no host can construct, so its
 * eviction behaviour has never been tested by anything. This header
 * depends on the standard library alone and `tests/user_rip_census`
 * compiles it and nothing else.
 */

/**
 * The first address above the user half of a 4-level address space.
 *
 * A guest instruction pointer below this is user-mode code. The test is
 * the address form rather than the code-segment privilege level
 * deliberately: the privilege level is a second VMCS read, this is
 * free, and it is exactly the filter the dumps were read with when the
 * 106,743 figure was produced - so the population this censuses is the
 * population that raised the question.
 *
 * Windows maps its kernel in the upper half and enforces SMEP, so the
 * two answers agree on this guest. Where they could disagree - a kernel
 * executing from a lower-half mapping - the address form is still an
 * honest description of what was sampled, and the reader says which
 * test was applied rather than calling the rows "user mode".
 */
inline constexpr std::uint64_t user_address_limit = 1ull << 47;

/**
 * The page-frame bits of CR3, which is what a join must key on.
 *
 * Bits 12 through 51 are the page frame; bits 0 through 11 carry the
 * process-context identifier when CR4.PCIDE is set, and bit 63 asks the
 * processor not to invalidate translations. **Every
 * `DirectoryTableBase` observed in this guest ends `...002`** - the
 * system process reads `0x1ae002` - so both sides of the join carry low
 * bits and both must be masked the same way, or the join silently
 * matches nothing and reads as "no process executes".
 *
 * Masked here, at the sample, so one address space is one row however
 * its context identifier moves. The **unmasked** value is kept beside
 * it in the small dictionary `note_user_cr3` fills, so the identifier
 * is not lost and the two can disagree - which is the whole argument of
 * CLAUDE.md's "census two fields, not one, and let them disagree".
 */
inline constexpr std::uint64_t cr3_page_frame_mask = 0x000ffffffffff000ull;

/** Whether `rip` is an address user-mode code can execute from. */
constexpr bool is_user_address(std::uint64_t rip)
{
    return rip < user_address_limit;
}

/**
 * Hashes a (CR3, instruction pointer) pair to a slot.
 *
 * `note_hot_rip`'s mixing constant and shift, over the two words
 * combined so the table is keyed on the **pair**: the point of the
 * instrument is that one address executed by two processes is two rows,
 * which is precisely what the module walk could not produce.
 *
 * `capacity` must be a power of two - the slot is masked, not divided,
 * because this runs on the second-level entry path.
 */
constexpr std::size_t user_rip_slot(std::uint64_t cr3,
                                    std::uint64_t rip,
                                    std::size_t capacity)
{
    constexpr std::uint64_t mix = 0x9e3779b97f4a7c15ull;
    constexpr std::uint64_t second = 0xbf58476d1ce4e5b9ull;

    auto key = rip ^ (cr3 * second);
    return static_cast<std::size_t>(((key * mix) >> 45) & (capacity - 1));
}

/**
 * One sample into the hashed (CR3, instruction pointer) table.
 *
 * Returns **false and writes nothing** when the CR3 could not be
 * obtained, which here means it masked to zero. That is the third
 * design constraint made structural rather than documented: no row this
 * function writes can carry CR3 zero, so "not recorded" and "recorded
 * as zero" are not the same reading, and the caller counts the
 * refusals in a member of their own.
 *
 * Zero is refused rather than stored because it is not a CR3 any
 * Windows process runs under, and because the failure it guards is
 * real: the value is read out of vmcs02, and a read taken with the
 * wrong VMCS current, or before that field has ever been written,
 * produces exactly zero.
 *
 * Eviction is `note_hot_rip`'s, for the reason recorded there: a linear
 * table claiming slots in arrival order lost 1,300,610 of 1,399,906
 * samples on one run and printed the 7% that fitted as a flat
 * distribution, which read as a finding. A colliding sample decays the
 * resident entry instead, so a hot pair keeps its slot however late it
 * first appears and `overflow` counts contention rather than lost hot
 * addresses.
 *
 * **All four spans are read-modify-written on plain words, so the rows
 * handed in must belong to one processor.** No lock, for the reason
 * `note_hot_rip` gives: this is the second-level entry path and a
 * contended lock there would cost more than the census is worth.
 */
constexpr bool note_user_rip(std::span<std::uint64_t> cr3s,
                             std::span<std::uint64_t> rips,
                             std::span<std::uint64_t> hits,
                             std::uint64_t & overflow,
                             std::uint64_t cr3,
                             std::uint64_t rip)
{
    auto frame = cr3 & cr3_page_frame_mask;

    if (0 == frame) {
        return false;
    }

    auto slot = user_rip_slot(frame, rip, hits.size());

    if ((cr3s[slot] == frame) && (rips[slot] == rip)) {
        hits[slot] = hits[slot] + 1;
        return true;
    }

    if (0 == hits[slot]) {
        cr3s[slot] = frame;
        rips[slot] = rip;
        hits[slot] = 1;
        return true;
    }

    // Occupied by another pair: decay it rather than drop the sample.
    hits[slot] = hits[slot] - 1;
    overflow = overflow + 1;
    return true;
}

/**
 * One sample into the small dictionary of **unmasked** CR3 values seen
 * executing user-mode code.
 *
 * The second field, and it exists because the table above cannot report
 * its own saturation in a way a reader can act on. A direct-mapped
 * table with decay keeps the hottest pairs and churns the rest, so an
 * address space that executes a great deal while spreading itself over
 * many addresses can be absent from every printed row. This dictionary
 * is linear and bounded, so it counts **every** user-mode sample
 * against its address space whatever the pair table did with it - and
 * if the two disagree, the disagreement is the reading.
 *
 * Linear because it is small: fourteen processes plus the guest
 * hypervisor's own two page tables was the whole population of the dump
 * that raised the question. `overflow` counts samples that found no
 * slot, so a saturated dictionary says so rather than dropping quietly,
 * which is what `cr3_seen` did until it grew the same counter.
 *
 * Unmasked on purpose: this is where the process-context identifier
 * survives, so `...002` can be read rather than assumed.
 */
constexpr bool note_user_cr3(std::span<std::uint64_t> cr3s,
                             std::span<std::uint64_t> hits,
                             std::uint64_t & overflow,
                             std::uint64_t cr3)
{
    if (0 == (cr3 & cr3_page_frame_mask)) {
        return false;
    }

    for (std::size_t i{}; i < cr3s.size(); ++i) {
        if (cr3s[i] == cr3) {
            hits[i] = hits[i] + 1;
            return true;
        }

        if (0 == cr3s[i]) {
            cr3s[i] = cr3;
            hits[i] = 1;
            return true;
        }
    }

    overflow = overflow + 1;
    return true;
}
} // namespace zpp::hypervisor
