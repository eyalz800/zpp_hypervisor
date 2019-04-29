#include "zpp/x64/os_page_table.h"
#include "zpp/x64/pte.h"
#include "zpp/x64/virtual_address.h"

namespace zpp::x64
{
os_page_table::os_page_table(
    std::uint64_t cr3,
    std::uint64_t (*physical_to_virtual)(std::uint64_t)) :
    pml4(reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(cr3 & 0xfffffffffffff000))),
    physical_to_virtual(physical_to_virtual)
{
}

std::uint64_t os_page_table::virtual_to_physical(std::uint64_t value) const
{
    // Parse the virtual address.
    auto address_structure = virtual_address(value);

    // The pml4e entry inside the pml4 table.
    auto pml4e = pte(pml4[address_structure.pml4e()]);

    // Convert physical page directory pointer table back to virtual.
    auto pdpt = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pml4e.page_number() << 12));

    // Fetch the page directory pointer table entry.
    auto pdpte = pte(pdpt[address_structure.pdpte()]);

    // If large, return the address now.
    if (pdpte.large()) {
        return (pdpte.page_number() << 30) +
               address_structure.huge_offset();
    }

    // Convert physical page directory back to virtual.
    auto pd = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pdpte.page_number() << 12));

    // Fetch the page directory entry.
    auto pde = pte(pd[address_structure.pde()]);

    // If large, return the address now.
    if (pde.large()) {
        return (pde.page_number() << 21) +
               address_structure.large_offset();
    }

    // Convert physical page table back to virtual.
    auto pt = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pde.page_number() << 12));

    // Return the address.
    return (pte(pt[address_structure.pte()]).page_number() << 12) +
           address_structure.offset();
}

std::uint64_t os_page_table::virtual_to_physical(const void * value) const
{
    return virtual_to_physical(reinterpret_cast<std::uint64_t>(value));
}

const std::uint64_t & os_page_table::head() const
{
    return *pml4;
}

} // namespace zpp::x64