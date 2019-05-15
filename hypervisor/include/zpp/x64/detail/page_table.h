#pragma once
#include "../page_table.h"

namespace zpp::x64
{
constexpr page_table::protection operator|(page_table::protection left,
                                           page_table::protection right)
{
    return page_table::protection(
        std::underlying_type_t<page_table::protection>(left) |
        std::underlying_type_t<page_table::protection>(right));
}

constexpr bool operator&(page_table::protection left,
                         page_table::protection right)
{
    return bool(std::underlying_type_t<page_table::protection>(left) &
                std::underlying_type_t<page_table::protection>(right));
}

template <typename PageTable>
void page_table::map_page_from(std::uint64_t address,
                               std::uint64_t physical_address,
                               protection protection,
                               PageTable && other_page_table)
{
    // Parse the virtual address.
    auto address_structure = virtual_address(address);

    // The pml4e entry inside the pml4 table.
    auto & pml4e = pml4[address_structure.pml4e()];

    // Map to the single page directory pointer table that we have.
    pml4e.page_number(other_page_table.virtual_to_physical(pdpt) >> 12);

    // Mark pml4e as present and writable.
    pml4e.write(true);
    pml4e.present(true);

    // Fetch the page directory pointer table entry.
    auto & pdpte = pdpt[address_structure.pdpte()];

    // Fetch the page directory according to how many page directories we
    // have.
    auto pd_index =
        address_structure.pdpte() /
        (std::extent_v<decltype(pdpt)> / std::extent_v<decltype(pds)>);
    auto & pd = pds[pd_index];

    // Map to the page directory, make present and writable.
    pdpte.page_number(other_page_table.virtual_to_physical(pd) >> 12);
    pdpte.write(true);
    pdpte.present(true);

    // Fetch the page directory entry.
    auto & pde = pd[address_structure.pde()];

    // Fetch the page table according to how many page tables we have.
    auto & pt = pts[pd_index][address_structure.pde()];

    // Map to the page table, make present and writable.
    pde.page_number(other_page_table.virtual_to_physical(pt) >> 12);
    pde.write(true);
    pde.present(true);

    // Fetch the page table entry.
    auto & pte = pt[address_structure.pte()];

    // Assign page number and protection.
    pte.page_number(physical_address >> 12);
    pte.write(protection & page_table::protection::write);
    pte.execute_disable(!(protection & page_table::protection::execute));
    pte.present(true);
}

template <typename PageTable>
void page_table::map_from(std::uint64_t base_address,
                          std::size_t size,
                          protection protection,
                          PageTable && other_page_table)
{
    // Calculate the number of pages.
    auto number_of_pages = (size + (page_size - 1)) / page_size;

    // Iterate all the pages.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        // Calculate the virtual address according to page index.
        auto address = base_address + (i * page_size);

        // Convert the virtual address to physical address.
        auto physical_address =
            other_page_table.virtual_to_physical(address);

        // Map the page.
        map_page(address, physical_address, protection);
    }
}

template <typename PageTable>
void page_table::map_from(const void * base_address,
                          std::size_t size,
                          protection protection,
                          PageTable && other_page_table)
{
    // Convert the address argument to integral type.
    return map_from(reinterpret_cast<std::uint64_t>(base_address),
                    size,
                    protection,
                    std::forward<PageTable>(other_page_table));
}

template <typename PageTable>
void page_table::uninitialized_map_from(std::uint64_t base_address,
                                        std::size_t size,
                                        protection protection,
                                        PageTable && other_page_table)
{
    // Calculate the number of pages.
    auto number_of_pages = (size + (page_size - 1)) / page_size;

    // Iterate all the pages.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        // Calculate the virtual address according to page index.
        auto address = base_address + (i * page_size);

        // Convert the virtual address to physical address.
        auto physical_address =
            other_page_table.virtual_to_physical(address);

        // Map the page from the other page table.
        map_page_from(
            address, physical_address, protection, other_page_table);
    }
}

template <typename PageTable>
void page_table::uninitialized_map_from(const void * base_address,
                                        std::size_t size,
                                        protection protection,
                                        PageTable && other_page_table)
{
    // Convert the address pointer to integral type.
    return uninitialized_map_from(
        reinterpret_cast<std::uint64_t>(base_address),
        size,
        protection,
        std::forward<PageTable>(other_page_table));
}

template <typename PageTable>
void page_table::map_self(PageTable && other_page_table)
{
    // Map the pml4 from other page table.
    uninitialized_map_from(pml4,
                           sizeof(pml4),
                           protection::read | protection::write,
                           std::forward<PageTable>(other_page_table));

    // Map the pdpt from other page table.
    uninitialized_map_from(pdpt,
                           sizeof(pdpt),
                           protection::read | protection::write,
                           std::forward<PageTable>(other_page_table));

    // Map the pd from other page table.
    uninitialized_map_from(pds,
                           sizeof(pds),
                           protection::read | protection::write,
                           std::forward<PageTable>(other_page_table));

    // Map the pt from other page table.
    uninitialized_map_from(pts,
                           sizeof(pts),
                           protection::read | protection::write,
                           std::forward<PageTable>(other_page_table));
}

} // namespace zpp::x64
