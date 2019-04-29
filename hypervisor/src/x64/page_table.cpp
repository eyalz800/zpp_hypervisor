#include "zpp/x64/page_table.h"
#include "zpp/x64/virtual_address.h"
#include <type_traits>

namespace zpp::x64
{
void page_table::map_page(std::uint64_t address,
                          std::uint64_t physical_address,
                          protection protection)
{
    // Use this page table to map the address.
    return map_page_from(address, physical_address, protection, *this);
}

std::uint64_t page_table::virtual_to_physical(std::uint64_t value) const
{
    // Parse the virtual address.
    auto address_structure = virtual_address(value);

    // Fetch the page directory pointer table entry.
    auto pdpte = pdpt[address_structure.pdpte()];

    // If large, return the address now.
    if (pdpte.large()) {
        return (pdpte.page_number() << 30) +
               address_structure.huge_offset();
    }

    // Fetch the page directory according to how many page directories we
    // have.
    auto pd_index =
        address_structure.pdpte() /
        (std::extent_v<decltype(pdpt)> / std::extent_v<decltype(pds)>);
    auto pd = pds[pd_index];

    // Fetch the page directory entry.
    auto pde = pd[address_structure.pde()];

    // If large, return the address now.
    if (pde.large()) {
        return (pde.page_number() << 21) +
               address_structure.large_offset();
    }

    // Fetch the page table according to how many page tables we have.
    auto pt = pts[pd_index][address_structure.pde()];

    // Return the address.
    return (pt[address_structure.pte()].page_number() << 12) +
           address_structure.offset();
}

std::uint64_t page_table::virtual_to_physical(const void * value) const
{
    return virtual_to_physical(reinterpret_cast<std::uint64_t>(value));
}

const x64::pte & page_table::head() const
{
    return *pml4;
}

x64::pte & page_table::page_table_entry(std::uint64_t address)
{
    // Parse the virtual address.
    auto address_structure = virtual_address(address);

    // Fetch the page directory pointer table entry.
    auto & pdpte = pdpt[address_structure.pdpte()];

    // If large, return the pte.
    if (pdpte.large()) {
        return pdpte;
    }

    // Fetch the page directory according to how many page directories we
    // have.
    auto pd_index =
        address_structure.pdpte() /
        (std::extent_v<decltype(pdpt)> / std::extent_v<decltype(pds)>);
    auto pd = pds[pd_index];

    // Fetch the page directory entry.
    auto & pde = pd[address_structure.pde()];

    // If large, return the pte.
    if (pde.large()) {
        return pde;
    }

    // Fetch the page table according to how many page tables we have.
    auto pt = pts[pd_index][address_structure.pde()];

    // Return the address.
    return pt[address_structure.pte()];
}

} // namespace zpp::x64