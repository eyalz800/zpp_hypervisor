#include "zpp/arch/x86_64/os_page_table.h"
#include "zpp/arch/x86_64/pte.h"
#include "zpp/arch/x86_64/virtual_address.h"

namespace zpp::arch::x86_64
{
os_page_table::os_page_table(
    std::uint64_t cr3,
    std::uint64_t (*physical_to_virtual)(std::uint64_t)) :
    pml4(reinterpret_cast<std::uint64_t *>(
        physical_to_virtual ? physical_to_virtual(cr3 & 0xfffffffffffff000)
                            : 0)),
    physical_to_virtual(physical_to_virtual)
{
}

std::uint64_t os_page_table::virtual_to_physical(std::uint64_t value) const
{
    // No callback means the platform identity maps, so the address is
    // its own answer. That is the UEFI case, per zpp_loader_parameters.
    if (!physical_to_virtual) {
        return value;
    }

    auto address_structure = virtual_address(value);

    // Each level goes back through the callback because an entry names
    // the *physical* address of the next table, and this runs on the
    // OS's virtual mapping - following one as a pointer would read
    // whatever the OS happens to have at that virtual address.
    auto pml4e = pte(pml4[address_structure.pml4e()]);

    auto pdpt = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pml4e.page_number() << 12));

    auto pdpte = pte(pdpt[address_structure.pdpte()]);

    // Correctness, not an optimisation: where PS is set the page number
    // names a data page, so descending into it would read whatever the
    // OS put there and call it a page table. No such case at the pml4
    // level - SDM 5.5.4 makes PS reserved in a PML4E.
    if (pdpte.large()) {
        return (pdpte.page_number() << 30) +
               address_structure.huge_offset();
    }

    auto pd = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pdpte.page_number() << 12));

    auto pde = pte(pd[address_structure.pde()]);

    if (pde.large()) {
        return (pde.page_number() << 21) +
               address_structure.large_offset();
    }

    auto pt = reinterpret_cast<std::uint64_t *>(
        physical_to_virtual(pde.page_number() << 12));

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

os_page_table::operator bool() const
{
    return (nullptr != pml4);
}

} // namespace zpp::arch::x86_64