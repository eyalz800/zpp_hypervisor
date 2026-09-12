#pragma once
#include "zpp/arch/x86_64/pte.h"
#include "zpp/arch/x86_64/virtual_address.h"
#include <cstdint>
#include <type_traits>
#include <utility>

namespace zpp::arch::x86_64
{
/**
 * Represents a page table structure.
 */
class page_table
{
public:
    /**
     * Constructs an empty page table.
     */
    page_table() = default;

    /**
     * Disable copy constructor due to address sensitivity.
     */
    page_table(const page_table &) = delete;

    /**
     * Disable copy assignment due to address sensitivity.
     */
    page_table & operator=(const page_table &) = delete;

    /**
     * Represents the page protection.
     */
    enum class protection : int;

    /**
     * Converts a virtual address to physical address.
     */
    std::uint64_t virtual_to_physical(std::uint64_t value) const;

    /**
     * Converts a virtual address to physical address.
     */
    std::uint64_t virtual_to_physical(const void * value) const;

    /**
     * Maps a single page to the page table with a given protection.
     */
    void map_page(std::uint64_t address,
                  std::uint64_t physical_address,
                  protection protection);

    /**
     * Adds protection to a range that is already mapped, leaving the
     * translation alone. Permissions are added and never taken away, and
     * the range is rounded outward to whole pages at both ends.
     *
     * Both of those are what makes this usable for ELF segment
     * permissions. A page is the finest granularity a page table has and
     * a segment boundary need not be page aligned, so two segments can
     * share a page and the page has to satisfy both - the union of what
     * they ask for. Rounding outward can only grant a neighbour a
     * permission it did not ask for; rounding inward takes one away from
     * a segment that did, and what it takes is the last page of one
     * segment or the first page of the next.
     */
    void add_protection(std::uint64_t base_address,
                        std::size_t size,
                        protection protection);

    /**
     * Adds protection to a range that is already mapped.
     */
    void add_protection(const void * base_address,
                        std::size_t size,
                        protection protection);

    /**
     * Maps the page table itself using another page table.
     */
    template <typename PageTable>
    void map_self(PageTable && other_page_table);

    /**
     * Maps an address from another page table.
     */
    template <typename PageTable>
    void map_from(std::uint64_t base_address,
                  std::size_t size,
                  protection protection,
                  PageTable && other_page_table);

    /**
     * Maps an address from another page table.
     */
    template <typename PageTable>
    void map_from(const void * base_address,
                  std::size_t size,
                  protection protection,
                  PageTable && other_page_table);

    /**
     * Returns the head of the initial page table in the translation.
     */
    const arch::x86_64::pte & head() const;

    /**
     * Returns the page table entry of a given address.
     */
    arch::x86_64::pte & page_table_entry(std::uint64_t address);

private:
    /**
     * Maps a single page from another page table.
     */
    template <typename PageTable>
    void map_page_from(std::uint64_t address,
                       std::uint64_t physical_address,
                       protection protection,
                       PageTable && other_page_table);

    /**
     * Maps an address from another page table while the
     * object has not completed initialization for self mapping.
     */
    template <typename PageTable>
    void self_map_from(std::uint64_t base_address,
                       std::size_t size,
                       protection protection,
                       PageTable && other_page_table);

    /**
     * Maps an address from another page table while the
     * object has not completed initialization for self mapping.
     */
    template <typename PageTable>
    void self_map_from(const void * base_address,
                       std::size_t size,
                       protection protection,
                       PageTable && other_page_table);

private:
    /**
     * The page size.
     */
    static constexpr auto page_size = 0x1000;

    /**
     * The page table level 4 page.
     */
    alignas(page_size) arch::x86_64::pte pml4[512];

    /**
     * The page directory pointer table.
     */
    alignas(page_size) arch::x86_64::pte pdpt[512];

    /**
     * The page directory tables.
     */
    alignas(page_size) arch::x86_64::pte pds[2][512];

    /**
     * The page tables.
     */
    alignas(page_size) arch::x86_64::pte pts[2][512][512];
};

enum class page_table::protection : int
{
    read = (1 << 0),
    write = (1 << 1),
    execute = (1 << 2),
};

} // namespace zpp::arch::x86_64

#include "detail/page_table.h"