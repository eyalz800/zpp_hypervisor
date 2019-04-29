#pragma once
#include <cstdint>

namespace zpp::x64
{
/**
 * Represents the virtual address structure.
 */
class virtual_address
{
public:
    /**
     * Constructs a zero virtual address structure.
     */
    constexpr virtual_address() = default;

    /**
     * Constructs a virtual address structure from a given address.
     */
    constexpr virtual_address(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the offset within a regular page.
     */
    constexpr std::uint64_t offset() const
    {
        return m_value & 0xfff;
    }

    /**
     * Sets the offset within a regular page.
     */
    constexpr void offset(std::uint64_t value)
    {
        m_value = ((m_value & 0xfffffffffffff000) | (value & 0xfff));
    }

    /**
     * Returns the offset within a large page.
     */
    constexpr std::uint64_t large_offset() const
    {
        return m_value & 0x1fffff;
    }

    /**
     * Sets the offset within a large page.
     */
    constexpr void large_offset(std::uint64_t value)
    {
        m_value = ((m_value & 0xffffffffffe00000) | (value & 0x1fffff));
    }

    /**
     * Returns the offset within a huge page.
     */
    constexpr std::uint64_t huge_offset() const
    {
        return m_value & 0x3fffffff;
    }

    /**
     * Sets the offset within a huge page.
     */
    constexpr void huge_offset(std::uint64_t value)
    {
        m_value = ((m_value & 0xffffffffc0000000) | (value & 0x3fffffff));
    }

    /**
     * Returns the page table entry index.
     */
    constexpr std::uint64_t pte() const
    {
        return ((m_value >> 12) & 0x1ff);
    }

    /**
     * Sets the page table entry index.
     */
    constexpr void pte(std::uint64_t value)
    {
        m_value =
            ((m_value & 0xffffffffffe00fff) | ((value & 0x1ff) << 12));
    }

    /**
     * Returns the page directory table entry index.
     */
    constexpr std::uint64_t pde() const
    {
        return ((m_value >> (12 + 9)) & 0x1ff);
    }

    /**
     * Sets the page directory table index.
     */
    constexpr void pde(std::uint64_t value)
    {
        m_value = ((m_value & 0xffffffffc01fffff) |
                   ((value & 0x1ff) << (12 + 9)));
    }

    /**
     * Returns the page directory pointer table entry index.
     */
    constexpr std::uint64_t pdpte() const
    {
        return ((m_value >> (12 + 9 * 2)) & 0x1ff);
    }

    /**
     * Sets the page directory pointer table entry index.
     */
    constexpr void pdpte(std::uint64_t value)
    {
        m_value = ((m_value & 0xffffff803fffffff) |
                   ((value & 0x1ff) << (12 + 9 * 2)));
    }

    /**
     * Returns the page level 4 entry index.
     */
    constexpr std::uint64_t pml4e() const
    {
        return ((m_value >> (12 + 9 * 3)) & 0x1ff);
    }

    /**
     * Sets the page level 4 entry index.
     */
    constexpr void pml4e(std::uint64_t value)
    {
        m_value = ((m_value & 0xffff007fffffffff) |
                   ((value & 0x1ff) << (12 + 9 * 3)));
    }

private:
    /**
     * The integral representation of the virtual address.
     */
    std::uint64_t m_value{};
};

} // namespace zpp::x64
