#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/pte.h"
#include "zpp/arch/x86_64/virtual_address.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/scope_exit.h"
#include <cstring>
#include <iterator>
#include <utility>

namespace zpp::hypervisor
{
namespace
{
/**
 * The top of what the extended page tables describe.
 *
 * initialize_ept builds one PML4 entry over 512 one-gigabyte page
 * directories, and says so: "It covers the 512 GB the identity map below
 * describes; above that the entries stay not-present". Every guest
 * physical access here is the identity of a host physical one, so an
 * address past this has no entry, and reaching it through the window would
 * fault in root mode, where there is no recovery point.
 */
constexpr std::uint64_t guest_physical_limit = 512ull * 1024 * 1024 * 1024;

/**
 * How much of a copy lands in one page, starting at the given offset.
 */
constexpr std::size_t bytes_in_page(std::uint64_t offset,
                                    std::size_t remaining)
{
    auto to_page_end = hypervisor::page_size - offset;
    return (remaining < to_page_end) ? remaining : to_page_end;
}

/**
 * Whether a copy of this size at this address stays inside the identity
 * map, computed without overflowing.
 */
constexpr bool within_guest_physical(std::uint64_t guest_physical,
                                     std::size_t size)
{
    return (guest_physical < guest_physical_limit) &&
           (size <= (guest_physical_limit - guest_physical));
}

} // namespace

/**
 * Which callers reach guest memory, by return address.
 *
 * `map_window_at` is 126 calls an exit at 1,088 cycles each - about
 * 137,000 of the 418,000 an exit spends inside this VMM, a third of it,
 * and the one large cost here that is *measured directly* rather than
 * derived from the launch-time price list, which a controlled removal
 * has since shown does not predict the marginal cost of a VMCS read.
 *
 * Only 39.5 of those calls are accounted for - `l2_translate_entries`
 * says the extended-page-table walks read that many entries an exit,
 * and each entry is one repoint of the window. The other 86 have twice
 * been guessed at and twice been wrong: the MSR autoload areas removed
 * five, the VTL assist capture removed none that mattered.
 *
 * So this stops guessing and asks the callers. A return address is a
 * name once symbolized against the module, and it costs one register.
 */
void hypervisor::note_guest_memory_caller(std::uint64_t caller)
{
    auto slot = ((caller >> 4) ^ (caller >> 11)) & (guest_memory_callers - 1);

    if (0 == this->guest_memory_caller_hits[slot]) {
        this->guest_memory_caller[slot] = caller;
    }

    if (this->guest_memory_caller[slot] == caller) {
        this->guest_memory_caller_hits[slot] =
            this->guest_memory_caller_hits[slot] + 1;
    } else {
        this->guest_memory_caller_overflow =
            this->guest_memory_caller_overflow + 1;
    }
}

std::expected<void, zpp::error> hypervisor::read_guest_physical(
    std::uint64_t guest_physical, std::span<std::byte> into)
{
    note_guest_memory_caller(reinterpret_cast<std::uint64_t>(
        __builtin_return_address(0)));

    // Refused rather than clamped. A short read would leave the caller
    // acting on a half-filled buffer whose tail is whatever it held
    // before, which is the kind of failure that reads as a guest bug.
    if (!within_guest_physical(guest_physical, into.size())) {
        return std::unexpected(
            zpp::error{error::guest_memory_unreachable});
    }

    // One page at a time, re-pointing the window for each, so the size of
    // the copy is not a constraint on the size of the window.
    std::size_t done{};
    while (done < into.size()) {
        auto at = guest_physical + done;
        auto offset = at & (page_size - 1);
        auto count = bytes_in_page(offset, into.size() - done);

        this->mapping_window_lock.lock();
        scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

        // Phase 11 is the mapping alone. The copy through it is cold
        // cache traffic and would survive a kept mapping; only this
        // would go away, so the two are timed apart before anything is
        // built on either.
        auto map_start = arch::x86_64::rdtsc();
        auto window = map_window_at(transfer_window_first_page, at, 1);
        this->phase_cycles[0][11] += arch::x86_64::rdtsc() - map_start;
        this->phase_calls[0][11] += 1;

        if (nullptr == window) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        std::memcpy(into.data() + done, window, count);
        done += count;
    }

    return {};
}

std::expected<void, zpp::error> hypervisor::write_guest_physical(
    std::uint64_t guest_physical, std::span<const std::byte> from)
{
    note_guest_memory_caller(reinterpret_cast<std::uint64_t>(
        __builtin_return_address(0)));

    if (!within_guest_physical(guest_physical, from.size())) {
        return std::unexpected(
            zpp::error{error::guest_memory_unreachable});
    }

    std::size_t done{};
    while (done < from.size()) {
        auto at = guest_physical + done;
        auto offset = at & (page_size - 1);
        auto count = bytes_in_page(offset, from.size() - done);

        this->mapping_window_lock.lock();
        scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

        auto window = map_window_at(transfer_window_first_page, at, 1);
        if (nullptr == window) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        std::memcpy(window, from.data() + done, count);
        done += count;
    }

    return {};
}

std::expected<std::uint64_t, zpp::error>
hypervisor::guest_linear_to_physical(std::uint64_t linear)
{
    // Five-level paging is refused rather than approximated. A four-level
    // walk of a five-level table reads the PML5 as though it were a PML4
    // and lands on an unrelated page, which is worse than saying no.
    constexpr std::uint64_t cr4_la57 = 1ull << 12;
    if (0 != (this->vmcs.guest_cr4() & cr4_la57)) {
        return std::unexpected(
            zpp::error{error::guest_address_not_mapped});
    }

    // Paging off means the linear address is already the physical one.
    // Worth handling rather than refusing: an application processor coming
    // out of a start-up IPI runs with CR0.PG clear, which is what the
    // unrestricted guest control exists for.
    constexpr std::uint64_t cr0_pg = 1ull << 31;
    if (0 == (this->vmcs.guest_cr0() & cr0_pg)) {
        return linear;
    }

    // From the VMCS, not from os_page_table. The launch-time table is a
    // snapshot of whatever CR3 held then, and a guest that has moved onto
    // its own tables since would be walked through the wrong ones - see
    // the declaration for what that costs.
    constexpr std::uint64_t address_mask = 0xffffffffff000ull;
    auto table = this->vmcs.guest_cr3() & address_mask;

    arch::x86_64::virtual_address address(linear);

    // The four levels, outermost first, with the index each is walked by.
    const std::uint64_t indices[]{
        address.pml4e(),
        address.pdpte(),
        address.pde(),
        address.pte(),
    };

    // What is left of the linear address when a walk stops at each level,
    // and the mask over exactly those bits: one gigabyte at the page
    // directory pointer level, two megabytes at the page directory level,
    // four kilobytes at the leaf. The first entry of each is never used -
    // a PML4 entry cannot map a page - and is present so all three arrays
    // are indexed by the same level.
    const std::uint64_t offsets[]{
        0,
        address.huge_offset(),
        address.large_offset(),
        address.offset(),
    };

    constexpr std::uint64_t masks[]{
        0,
        0x3fffffff,
        0x1fffff,
        0xfff,
    };

    for (std::size_t level{}; level < std::size(indices); ++level) {
        arch::x86_64::pte entry;

        auto at = table + (indices[level] * sizeof(entry));
        auto read = read_guest_physical(
            at,
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(read.error());
        }

        if (!entry.present()) {
            return std::unexpected(
                zpp::error{error::guest_address_not_mapped});
        }

        auto last = (std::size(indices) - 1) == level;

        // A leaf, either because this is the last level or because the
        // large bit says the entry maps a page directly. The bit is
        // reserved at the top level, so it is only consulted below it.
        if (last || ((0 != level) && entry.large())) {
            // Masked rather than trusted. The low bits of a large page's
            // address field are reserved and must be zero, and reserved
            // bits are exactly the ones a stale or hostile table has
            // wrong - so the offset comes from the linear address alone.
            auto page = (entry.page_number() << 12) & ~masks[level];
            return page | offsets[level];
        }

        table = entry.page_number() << 12;
    }

    // Unreachable: the last iteration always returns.
    std::unreachable();
}

} // namespace zpp::hypervisor
