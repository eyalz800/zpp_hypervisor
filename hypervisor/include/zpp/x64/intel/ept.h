#pragma once
#include "zpp/x64/memory_type.h"
#include <cstdint>
#include <type_traits>

namespace zpp::x64::intel
{
/**
 * Represents an extended page table entry structure.
 */
class epte
{
public:
    /**
     * Constructs an empty extended page table entry structure.
     */
    constexpr epte() = default;

    /**
     * Constructs an extended page table entry structure from a given
     * value.
     */
    constexpr epte(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the read bit.
     */
    constexpr bool read() const
    {
        return m_value & 1;
    }

    /**
     * Sets the read bit to the specified value.
     */
    constexpr void read(bool value)
    {
        m_value = (m_value & ~0x1ull) | std::uint64_t{value};
    }

    /**
     * Returns the write bit.
     */
    constexpr bool write() const
    {
        return m_value & (1 << 1);
    }

    /**
     * Sets the write bit to the specified value.
     */
    constexpr void write(bool value)
    {
        m_value = (m_value & ~(0x1ull << 1)) | (std::uint64_t{value} << 1);
    }

    /**
     * Returns the execute bit.
     */
    constexpr bool execute() const
    {
        return m_value & (1 << 2);
    }

    /**
     * Sets the execute bit to the specified value.
     */
    constexpr void execute(bool value)
    {
        m_value = (m_value & ~(0x1ull << 2)) | (std::uint64_t{value} << 2);
    }

    /**
     * Returns the memory type field.
     */
    constexpr memory_type type() const
    {
        return memory_type((m_value >> 3) & 0x7);
    }

    /**
     * Sets the memory type field to the specified value.
     */
    constexpr void type(memory_type value)
    {
        m_value =
            ((m_value & ~(0x7ull << 3)) |
             ((std::underlying_type_t<decltype(value)>(value) & 0xfull)
              << 3));
    }

    /**
     * Returns the large bit.
     */
    constexpr bool large() const
    {
        return m_value & (1 << 7);
    }

    /**
     * Sets the large bit to the specified value.
     */
    constexpr void large(bool value)
    {
        m_value = (m_value & ~(0x1ull << 7)) | (std::uint64_t{value} << 7);
    }

    /**
     * Returns the accessed bit.
     */
    constexpr bool accessed() const
    {
        return m_value & (1 << 8);
    }

    /**
     * Sets the accessed bit to the specified value.
     */
    constexpr void accessed(bool value)
    {
        m_value = (m_value & ~(0x1ull << 8)) | (std::uint64_t{value} << 8);
    }

    /**
     * Returns the execute user bit.
     */
    constexpr bool execute_user() const
    {
        return m_value & (1 << 10);
    }

    /**
     * Sets the execute user bit to the specified value.
     */
    constexpr void execute_user(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 10)) | (std::uint64_t{value} << 10);
    }

    /**
     * Returns the page number field.
     */
    constexpr std::uint64_t page_number() const
    {
        return ((m_value >> 12) & 0xffffffffffull);
    }

    /**
     * Sets the page number to the specified value.
     */
    constexpr void page_number(std::uint64_t value)
    {
        m_value = ((m_value & ~0xffffffffff000ull) |
                   ((value & 0xffffffffffull) << 12));
    }

    /**
     * Returns the large page number field.
     */
    constexpr std::uint64_t large_page_number() const
    {
        return ((m_value >> 21) & 0x7fffffffull);
    }

    /**
     * Sets the large page number to the specified value.
     */
    constexpr void large_page_number(std::uint64_t value)
    {
        m_value = ((m_value & ~(0x7fffffffull << 21)) |
                   ((value & 0x7fffffffull) << 21));
    }

    /**
     * Returns the suppress #VE (VM-Exit) bit.
     */
    constexpr bool suppress_ve() const
    {
        return m_value & (1ull << 63);
    }

    /**
     * Sets the suppress #VE (VM-Exit) bit to the specified value.
     */
    constexpr void suppress_ve(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 63)) | (std::uint64_t{value} << 63);
    }

    /**
     * Convert the entry to an integral representation.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Convert the entry to an integral representation.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the entry.
     */
    std::uint64_t m_value{};
};

} // namespace zpp::x64::intel
