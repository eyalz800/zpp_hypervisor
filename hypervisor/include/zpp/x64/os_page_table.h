#pragma once
#include <cstdint>

namespace zpp::x64
{
/**
 * References an OS page table using given CR3 and a function that converts
 * a physical address within the OS page table to virtual address.
 */
class os_page_table
{
public:
    /**
     * Construct an empty OS page table.
     */
    os_page_table() = default;

    /**
     * Construct the OS page table with CR3 and a function that converts
     * a physical address within the OS page table to virtual address.
     */
    explicit os_page_table(
        std::uint64_t cr3,
        std::uint64_t (*physical_to_virtual)(std::uint64_t));

    /**
     * Converts a virtual address to physical address.
     */
    std::uint64_t virtual_to_physical(std::uint64_t value) const;

    /**
     * Converts a virtual address to physical address.
     */
    std::uint64_t virtual_to_physical(const void * value) const;

    /**
     * Returns the head of the initial page table in the translation.
     */
    const std::uint64_t & head() const;

private:
    /**
     * The page table level 4 pointer.
     */
    std::uint64_t * pml4{};

    /**
     * A function to convert OS page table physical address to virtual
     * address.
     */
    std::uint64_t (*physical_to_virtual)(std::uint64_t);
};

} // namespace zpp::x64