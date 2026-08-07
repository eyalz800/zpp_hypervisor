#pragma once
#include "../page_table.h"
#include <iterator>

namespace zpp::arch::x86_64
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
    auto address_structure = virtual_address(address);

    // Rewritten every call rather than tested, because every level is
    // already a member: pointing an entry at the table it already holds
    // is idempotent and cheaper than the branch that would skip it.
    auto & pml4e = pml4[address_structure.pml4e()];
    pml4e.page_number(other_page_table.virtual_to_physical(pdpt) >> 12);
    pml4e.write(true);
    pml4e.present(true);

    auto & pdpte = pdpt[address_structure.pdpte()];

    // 512 pdpt slots share two page directories, so the divisor is 256
    // and the choice is address bit 38 alone. The table therefore
    // aliases: two addresses agreeing in bit 38 and bits 29:12 land on
    // the same leaf however far apart they are, and the second mapping
    // silently replaces the first. That is the price of storage fixed
    // at compile time, and nothing here detects it.
    auto pd_index =
        address_structure.pdpte() / (std::size(pdpt) / std::size(pds));
    auto & pd = pds[pd_index];

    pdpte.page_number(other_page_table.virtual_to_physical(pd) >> 12);
    pdpte.write(true);
    pdpte.present(true);

    auto & pde = pd[address_structure.pde()];
    auto & pt = pts[pd_index][address_structure.pde()];

    pde.page_number(other_page_table.virtual_to_physical(pt) >> 12);
    pde.write(true);
    pde.present(true);

    // Protection goes in the leaf only, and the levels above stay
    // permissive. SDM 5.6.1 takes access rights as the conjunction over
    // the whole walk, so a restriction expressed higher up would apply
    // to the whole gigabyte or two megabytes that entry covers - and
    // those tables are shared with every other mapping inside it.
    auto & pte = pt[address_structure.pte()];
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
    // Rounding the size up assumes a page aligned base; an unaligned
    // one would leave the last partial page unmapped. Every caller
    // passes an aligned base.
    auto number_of_pages = (size + (page_size - 1)) / page_size;

    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto address = base_address + (i * page_size);

        // Translated per page, since the source table is free to have
        // scattered the range across physical memory.
        auto physical_address =
            other_page_table.virtual_to_physical(address);

        map_page(address, physical_address, protection);
    }
}

template <typename PageTable>
void page_table::map_from(const void * base_address,
                          std::size_t size,
                          protection protection,
                          PageTable && other_page_table)
{
    return map_from(reinterpret_cast<std::uint64_t>(base_address),
                    size,
                    protection,
                    std::forward<PageTable>(other_page_table));
}

template <typename PageTable>
void page_table::self_map_from(std::uint64_t base_address,
                               std::size_t size,
                               protection protection,
                               PageTable && other_page_table)
{
    auto number_of_pages = (size + (page_size - 1)) / page_size;

    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto address = base_address + (i * page_size);
        auto physical_address =
            other_page_table.virtual_to_physical(address);

        // map_page_from rather than map_page, which is the whole
        // difference from map_from above: map_page would resolve the
        // sub-table addresses through this table, and while it is being
        // built those translations do not exist yet.
        map_page_from(
            address, physical_address, protection, other_page_table);
    }
}

template <typename PageTable>
void page_table::self_map_from(const void * base_address,
                               std::size_t size,
                               protection protection,
                               PageTable && other_page_table)
{
    return self_map_from(reinterpret_cast<std::uint64_t>(base_address),
                         size,
                         protection,
                         std::forward<PageTable>(other_page_table));
}

template <typename PageTable>
void page_table::map_self(PageTable && other_page_table)
{
    // The processor walks these through physical addresses and never
    // needs them mapped; this VMM does, because it keeps editing them
    // after switching to this table, when the only virtual addresses
    // that exist are the ones mapped here. Leaving a level out gives a
    // table that can never be changed again.
    self_map_from(pml4,
                  sizeof(pml4),
                  protection::read | protection::write,
                  std::forward<PageTable>(other_page_table));

    self_map_from(pdpt,
                  sizeof(pdpt),
                  protection::read | protection::write,
                  std::forward<PageTable>(other_page_table));

    self_map_from(pds,
                  sizeof(pds),
                  protection::read | protection::write,
                  std::forward<PageTable>(other_page_table));

    self_map_from(pts,
                  sizeof(pts),
                  protection::read | protection::write,
                  std::forward<PageTable>(other_page_table));
}

} // namespace zpp::arch::x86_64
