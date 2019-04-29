#pragma once
#include "zpp/x64/memory_type.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zpp::x64::intel
{
/**
 * Represents an MTRR, memory type range register.
 */
struct mtrr
{
    /**
     * The physical base.
     */
    std::uint64_t physical_base{};

    /**
     * The size of the MTRR.
     */
    std::uint64_t size{};

    /**
     * The MTRR type.
     */
    x64::memory_type type{};

    /**
     * True if valid, else false.
     */
    bool valid{};
};

/**
 * Represents the MTRR capabilities of the processor.
 */
class mtrr_capabilities
{
public:
    /**
     * Creates an empty mtrr capabilities structure.
     */
    constexpr mtrr_capabilities() = default;

    /**
     * Creates an mtrr capabilities structure from an integral
     * representation.
     */
    constexpr mtrr_capabilities(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the number of range registers.
     */
    constexpr std::uint64_t variable_range_register_count() const
    {
        return m_value & 0xff;
    }

    /**
     * Sets the number of range registers.
     */
    constexpr void variable_range_register_count(std::uint64_t value)
    {
        m_value = (m_value & ~0xff) | (value & 0xff);
    }

    /**
     * Returns true if fixed range registers are supported, else false.
     */
    constexpr bool fixed_range_registers_supported() const
    {
        return m_value & (1 << 8);
    }

    /**
     * Sets the fixed range registers supported bit.
     */
    constexpr void fixed_range_registers_supported(bool value)
    {
        m_value = (m_value & ~(0x1ull << 8)) | (std::uint64_t{value} << 8);
    }

    /**
     * Returns the write combining bit.
     */
    constexpr bool write_combining() const
    {
        return m_value & (1 << 10);
    }

    /**
     * Sets the write combining bit to the specified value.
     */
    constexpr void write_combining(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 10)) | (std::uint64_t{value} << 10);
    }

    /**
     * Returns the system-management-range-register bit.
     */
    constexpr bool system_management_range_register() const
    {
        return m_value & (1 << 11);
    }

    /**
     * Sets the system-management-range-register bit to the specified
     * value.
     */
    constexpr void system_management_range_register(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 11)) | (std::uint64_t{value} << 11);
    }

    /**
     * Returns the integral representation of the mtrr capabilities.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Returns the integral representation of the mtrr capabilities.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the mtrr capabilities.
     */
    std::uint64_t m_value{};
};

/**
 * Represents the MTRR variable base.
 */
class mtrr_variable_base
{
public:
    /**
     * Constructs an empty MTRR variable base.
     */
    constexpr mtrr_variable_base() = default;

    /**
     * Constructs an MTRR variable base from the specified value.
     */
    constexpr mtrr_variable_base(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the memory type.
     */
    constexpr memory_type memory_type() const
    {
        return x64::memory_type(m_value & 0xff);
    }

    /**
     * Sets the memory type to the specified value.
     */
    constexpr void memory_type(std::uint64_t value)
    {
        m_value = (m_value & ~0xff) | (value & 0xff);
    }

    /**
     * Sets the memory type to the specified value.
     */
    constexpr void memory_type(x64::memory_type value)
    {
        memory_type(std::underlying_type_t<x64::memory_type>(value));
    }

    /**
     * Returns the physical base page frame number.
     */
    constexpr std::uint64_t page_number() const
    {
        return ((m_value >> 12) & 0xffffffffff);
    }

    /**
     * Sets the physical base page frame number to the specified value.
     */
    constexpr void page_number(std::uint64_t value)
    {
        m_value =
            (m_value & ~0xffffffffff000) | ((value & 0xffffffffff) << 12);
    }

    /**
     * Returns the integral representation of the MTRR variable base.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Returns the integral representation of the MTRR variable base.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the MTRR variable base.
     */
    std::uint64_t m_value{};
};

/**
 * Represents the MTRR variable mask.
 * The rule is that (address_in_range & mask == mask & base).
 */
class mtrr_variable_mask
{
public:
    /**
     * Creates an empty mtrr variable mask.
     */
    constexpr mtrr_variable_mask() = default;
    constexpr mtrr_variable_mask(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns true where valid, else false.
     */
    constexpr bool valid() const
    {
        return m_value & (1 << 11);
    }

    /**
     * Sets the valid bit of the mtrr to the specified value.
     */
    constexpr void valid(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 11)) | (std::uint64_t{value} << 11);
    }

    /**
     * Returns the physical mask field.
     * The rule is that (address_in_range & mask == mask & base).
     */
    constexpr std::uint64_t physical_mask() const
    {
        return ((m_value >> 12) & 0xffffffffff);
    }

    /**
     * Sets the physical mask field to the specified value.
     */
    constexpr void physical_mask(std::uint64_t value)
    {
        m_value =
            (m_value & ~0xffffffffff000) | ((value & 0xffffffffff) << 12);
    }

    /**
     * Returns the integral representation of the mtrr variable mask.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Returns the integral representation of the mtrr variable mask.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the mtrr variable mask.
     */
    std::uint64_t m_value{};
};

} // namespace zpp::x64::intel
