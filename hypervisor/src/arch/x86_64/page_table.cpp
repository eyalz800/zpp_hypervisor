#include "zpp/arch/x86_64/page_table.h"
#include "zpp/arch/x86_64/virtual_address.h"
#include <iterator>
#include <type_traits>

namespace zpp::arch::x86_64
{
void page_table::map_page(std::uint64_t address,
                          std::uint64_t physical_address,
                          protection protection)
{
    // Resolves the sub-table addresses through this table, so it is
    // only correct once map_self has run. Before that, self_map_from.
    return map_page_from(address, physical_address, protection, *this);
}

void page_table::add_protection(std::uint64_t base_address,
                                std::size_t size,
                                protection protection)
{
    auto first = base_address & ~std::uint64_t{page_size - 1};
    auto last = (base_address + size + (page_size - 1)) &
                ~std::uint64_t{page_size - 1};

    for (auto address = first; address < last; address += page_size) {
        // Whichever entry terminates the walk, which is the one the
        // permission has to go in - the levels above are shared with
        // every other mapping under them and stay permissive, as
        // map_page_from's own comment explains.
        auto & entry = page_table_entry(address);

        // Set, never cleared. This is the union: a page two segments
        // share is visited once per segment and keeps what either asked
        // for. It also means the caller has to have established the
        // floor first, because nothing here can lower one.
        if (protection & page_table::protection::write) {
            entry.write(true);
        }

        if (protection & page_table::protection::execute) {
            entry.execute_disable(false);
        }
    }

    // No TLB invalidation, and that is a precondition rather than an
    // omission: this rewrites entries in a table the calling processor is
    // not yet running on, so there is nothing cached to be stale. Loading
    // CR3 with it is what makes these visible, and that flushes
    // everything non-global - nothing here sets the global bit.
}

void page_table::add_protection(const void * base_address,
                                std::size_t size,
                                protection protection)
{
    return add_protection(
        reinterpret_cast<std::uint64_t>(base_address), size, protection);
}

std::uint64_t page_table::virtual_to_physical(std::uint64_t value) const
{
    // Starts at the pdpt, not the pml4, because every pml4 entry
    // map_page_from writes points at the same single pdpt - reading it
    // could only confirm what is already known.
    auto address_structure = virtual_address(value);

    auto pdpte = pdpt[address_structure.pdpte()];

    if (pdpte.large()) {
        return (pdpte.huge_page_number() << 30) +
               address_structure.huge_offset();
    }

    // The same fold map_page_from applies, and it has to stay the same
    // expression - picking the other directory would report a physical
    // address for a page this table never mapped, and one answer this
    // produces is the value loaded into the host CR3.
    auto pd_index =
        address_structure.pdpte() / (std::size(pdpt) / std::size(pds));
    auto pd = pds[pd_index];

    auto pde = pd[address_structure.pde()];

    if (pde.large()) {
        return (pde.large_page_number() << 21) +
               address_structure.large_offset();
    }

    auto pt = pts[pd_index][address_structure.pde()];

    return (pt[address_structure.pte()].page_number() << 12) +
           address_structure.offset();
}

std::uint64_t page_table::virtual_to_physical(const void * value) const
{
    return virtual_to_physical(reinterpret_cast<std::uint64_t>(value));
}

const arch::x86_64::pte & page_table::head() const
{
    return *pml4;
}

arch::x86_64::pte & page_table::page_table_entry(std::uint64_t address)
{
    // Returns whichever entry terminates the walk, so that changing it
    // changes this address. Descending past a large-page entry would
    // hand back a leaf the processor never consults, and a write to it
    // would appear to work and do nothing.
    auto address_structure = virtual_address(address);

    auto & pdpte = pdpt[address_structure.pdpte()];
    if (pdpte.large()) {
        return pdpte;
    }

    auto pd_index =
        address_structure.pdpte() / (std::size(pdpt) / std::size(pds));
    auto pd = pds[pd_index];

    auto & pde = pd[address_structure.pde()];
    if (pde.large()) {
        return pde;
    }

    auto pt = pts[pd_index][address_structure.pde()];

    return pt[address_structure.pte()];
}

} // namespace zpp::arch::x86_64