#pragma once
#include "zpp/x64/memory_type.h"
#include <cstdint>
#include <cstring>

namespace zpp::x64::intel
{
/**
 * Represents the EPT pointer.
 */
class ept_pointer
{
public:
    /**
     * Creates an empty EPT pointer.
     */
    constexpr ept_pointer() = default;

    /**
     * Creates an EPT pointer from a specified integral value.
     */
    constexpr ept_pointer(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the memory type field.
     */
    constexpr x64::memory_type memory_type() const
    {
        return x64::memory_type((m_value >> 0) & 0x7);
    }

    /**
     * Sets the memory type field to the specified value.
     */
    constexpr void memory_type(x64::memory_type value)
    {
        m_value = ((m_value & ~0x7) |
                   ((std::underlying_type_t<decltype(value)>(value) & 0x7)
                    << 0));
    }

    /**
     * Returns the page walk length.
     * Note: The page walk length returned starts from 1 as opposed to the
     * integral representation that starts from 0.
     */
    constexpr std::uint64_t page_walk_length() const
    {
        return ((m_value >> 3) & 0x7) + 1;
    }

    /**
     * Sets the page walk length.
     * Note: The page walk length expected starts from 1 as opposed to the
     * integral representation that starts from 0.
     */
    constexpr void page_walk_length(std::uint64_t value)
    {
        m_value = ((m_value & ~(0x7 << 3)) | (((value - 1) & 0x7) << 3));
    }

    /**
     * Returns the access and dirty bit.
     */
    constexpr bool access_and_dirty() const
    {
        return m_value & (1 << 8);
    }

    /**
     * Sets the access and dirty bit to the specified value.
     */
    constexpr void access_and_dirty(bool value)
    {
        m_value = ((m_value & ~(0x7 << 8)) | (((value - 1) & 0x7) << 8));
    }

    /**
     * Returns the page number.
     */
    constexpr std::uint64_t page_number() const
    {
        return ((m_value >> 12) & 0xffffffffff);
    }

    /**
     * Sets the page number to the specified value.
     */
    constexpr void page_number(std::uint64_t value)
    {
        m_value =
            (m_value & ~0xffffffffff000) | ((value & 0xffffffffff) << 12);
    }

    /**
     * Returns the integral representation of the EPT pointer.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Returns the integral representation of the EPT pointer.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the EPT pointer.
     */
    std::uint64_t m_value{};
};

} // namespace zpp::x64::intel
