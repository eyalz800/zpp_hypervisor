#pragma once
#include <cstdint>
#include <cstring>

namespace zpp::x64
{
/**
 * Represents a segment descriptor.
 */
struct segment_descriptor
{
public:
    /**
     * Constructs an empty segment descriptor.
     */
    segment_descriptor() = default;

    /**
     * Segment types.
     */
    enum class segment_type
    {
        data_read_only = 0,
        data_read_only_accessed = 1,
        data_read_write = 2,
        data_read_write_accessed = 3,
        data_read_only_expand_down = 4,
        data_read_only_expand_down_accessed = 5,
        data_read_write_expand_down = 6,
        data_read_write_expand_down_accessed = 7,
        code_execute_only = 8,
        code_execute_only_accessed = 9,
        tss_available = 9,
        code_execute_read = 10,
        code_execute_read_accessed = 11,
        tss_busy = 11,
        code_execute_only_conforming = 12,
        code_execute_only_conforming_accessed = 13,
        code_execute_read_conforming = 14,
        code_execute_read_conforming_accessed = 15,
    };

    /**
     * Creates a segment descriptor from the entry and extended values.
     */
    constexpr explicit segment_descriptor(std::uint64_t entry,
                                          std::uint64_t extended = {}) :
        m_entry(entry),
        m_extended(extended)
    {
    }

    /**
     * Create a segment descriptor from memory.
     */
    static segment_descriptor from_memory(std::uint64_t base,
                                          std::uint16_t selector)
    {
        segment_descriptor descriptor;

        if (!selector) {
            return descriptor;
        }

        auto address =
            reinterpret_cast<const char *>(base) + (selector & ~0x3);

        std::memcpy(
            &descriptor.m_entry, address, sizeof(descriptor.m_entry));
        if (selector == 0x1fff) {
            return descriptor;
        }

        std::memcpy(&descriptor.m_extended,
                    address + sizeof(descriptor.m_entry),
                    sizeof(descriptor.m_extended));

        return descriptor;
    }

    /**
     * Returns the limit field of the segment descriptor.
     */
    constexpr std::uint64_t limit() const
    {
        return limit_low() | (limit_high() << 16);
    }

    /**
     * Sets the limit field of the segment descriptor.
     */
    constexpr void limit(std::uint64_t value)
    {
        limit_low(value & 0xffff);
        limit_high((value >> 16) & 0xf);
    }

    /**
     * Returns the base field of the segment descriptor.
     */
    constexpr std::uint64_t base() const
    {
        return base_low() | (base_middle() << 16) | (base_high() << 24);
    }

    /**
     * Sets the base field of the segment descriptor.
     */
    constexpr void base(std::uint64_t value)
    {
        base_low(value & 0xffff);
        base_middle((value >> 16) & 0xff);
        base_high((value >> 24) & 0xff);
    }

    /**
     * Returns the base field together with the extended base.
     */
    constexpr std::uint64_t base_extended() const
    {
        return base() | (base_upper() << 32);
    }

    /**
     * Sets the base field together with the extended base.
     */
    constexpr void base_extended(std::uint64_t value)
    {
        base(value & 0xffffffff);
        base_upper((value >> 32) & 0xffffffff);
    }

    /**
     * Returns the type field of the segment descriptor.
     */
    constexpr segment_type type() const
    {
        return segment_type((m_entry >> (32 + 8)) & 0xf);
    }

    /**
     * Sets the type field of the segment descriptor to the specified
     * value.
     */
    constexpr void type(segment_type value)
    {
        m_entry =
            (m_entry & ~(0xfull << (32 + 8))) |
            ((std::underlying_type_t<decltype(value)>(value) & 0xfull)
             << (32 + 8));
    }

    /**
     * Returns the system bit of the segment descriptor, the bit is
     * returned as 1 if the segment is a system segment, else returns 0.
     * Note: The value of the bit in the integral representation is the
     * opposite.
     */
    constexpr bool system() const
    {
        return !((m_entry >> (32 + 12)) & 0x1);
    }

    /**
     * Sets the system bit of the segment descriptor.
     * A value of 1 makes the segment descriptor a system segment
     * descriptor, and a value of 0 makes it a non-system segment
     * descriptor. Note: The value of the bit in the integral
     * representation is the opposite.
     */
    constexpr void system(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 12))) |
                  ((std::uint64_t{!value} << (32 + 12)));
    }

    /**
     * Returns the privilege level field of the segment descriptor.
     */
    constexpr std::uint64_t privilege_level() const
    {
        return (m_entry >> (32 + 13)) & 0x3;
    }

    /**
     * Sets the privilege level field of the segment descriptor.
     */
    constexpr void privilege_level(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0x3ull << (32 + 13))) |
                  ((value & 0x3ull) << (32 + 13));
    }

    /**
     * Returns the present bit of the segment descriptor.
     */
    constexpr bool present() const
    {
        return (m_entry >> (32 + 15)) & 0x1;
    }

    /**
     * Sets the present bit of the segment descriptor to the specified
     * value.
     */
    constexpr void present(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 15))) |
                  ((std::uint64_t{value} << (32 + 15)));
    }

    /**
     * Returns the available-for-system-use bit of the segment descriptor.
     */
    constexpr bool available_for_system_use() const
    {
        return (m_entry >> (32 + 20)) & 0x1;
    }

    /**
     * Sets the available-for-system-use bit of the segment descriptor to
     * the specified value.
     */
    constexpr void available_for_system_use(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 20))) |
                  ((std::uint64_t{value} << (32 + 20)));
    }

    /**
     * Returns the code-64-bit bit of the segment descriptor.
     */
    constexpr bool code_64_bit() const
    {
        return (m_entry >> (32 + 21)) & 0x1;
    }

    /**
     * Sets the code-64-bit bit of the segment descriptor to the specified
     * value.
     */
    constexpr void code_64_bit(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 21))) |
                  ((std::uint64_t{value} << (32 + 21)));
    }

    /**
     * Returns the default operation size bit of the segment descriptor.
     */
    constexpr bool default_operation_size() const
    {
        return (m_entry >> (32 + 22)) & 0x1;
    }

    /**
     * Sets the default operation size of the segment descriptor to the
     * specified value.
     */
    constexpr void default_operation_size(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 22))) |
                  ((std::uint64_t{value} << (32 + 22)));
    }

    /**
     * Returns the granularity bit of the segment descriptor.
     */
    constexpr bool granularity() const
    {
        return (m_entry >> (32 + 23)) & 0x1;
    }

    /**
     * Sets the granularity bit of the segment descriptor to the specified
     * value.
     */
    constexpr void granularity(bool value)
    {
        m_entry = (m_entry & ~(0x1ull << (32 + 23))) |
                  ((std::uint64_t{value} << (32 + 23)));
    }

    /**
     * Returns the base or extended base field of the segment descriptor
     * according to whether or not the segment descriptor is system.
     */
    constexpr std::uint64_t context_dependent_base() const
    {
        return system() ? base_extended() : base();
    }

    /**
     * Returns the vmx-specific access rights field of the segment
     * descriptor.
     */
    constexpr std::uint32_t vmx_access_rights() const
    {
        return ((m_entry >> 40) & 0xffff) | (!present() << 16);
    }

    /**
     * Returns the basic integral entry value of the segment descriptor.
     */
    constexpr std::uint64_t basic_value() const
    {
        return m_entry;
    }

    /**
     * Returns the extended integral entry value of the segment descriptor.
     */
    constexpr std::uint64_t extended_value() const
    {
        return m_extended;
    }

private:
    /**
     * Returns the upper most extended base value.
     */
    constexpr std::uint64_t base_upper() const
    {
        return m_extended & 0xffffffffull;
    }

    /**
     * Sets the upper most extended base value.
     */
    constexpr void base_upper(std::uint64_t value)
    {
        m_extended =
            (m_extended & ~0xffffffffull) | (value & 0xffffffffull);
    }

    /**
     * Returns the lowest base value.
     */
    constexpr std::uint64_t base_low() const
    {
        return (m_entry >> 16) & 0xffff;
    }

    /**
     * Sets the lowest base value.
     */
    constexpr void base_low(std::uint64_t value)
    {
        m_entry =
            (m_entry & ~(0xffffull << 16)) | ((value & 0xffffull) << 16);
    }

    /**
     * Returns the middle base value.
     */
    constexpr std::uint64_t base_middle() const
    {
        return (m_entry >> 32) & 0xff;
    }

    /**
     * Sets the middle base value.
     */
    constexpr void base_middle(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0xffull << 32)) | ((value & 0xffull) << 32);
    }

    /**
     * Returns the high base value.
     */
    constexpr std::uint64_t base_high() const
    {
        return (m_entry >> (32 + 24)) & 0xff;
    }

    /**
     * Sets the high base value.
     */
    constexpr void base_high(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0xffull << (32 + 24))) |
                  ((value & 0xffull) << (32 + 24));
    }

    /**
     * Returns the low limit value.
     */
    constexpr std::uint64_t limit_low() const
    {
        return m_entry & 0xffff;
    }

    /**
     * Sets the low limit value.
     */
    constexpr void limit_low(std::uint64_t value)
    {
        m_entry = (m_entry & ~0xffffull) | (value & 0xffffull);
    }

    /**
     * Returns the high limit value.
     */
    constexpr std::uint64_t limit_high() const
    {
        return (m_entry >> (32 + 16)) & 0xf;
    }

    /**
     * Sets the high limit value.
     */
    constexpr void limit_high(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0xfull << (32 + 16))) |
                  ((value & 0xfull) << (32 + 16));
    }

private:
    /**
     * The basic integral representation of the entry.
     */
    std::uint64_t m_entry{};

    /**
     * The extended integral representation of the entry.
     */
    std::uint64_t m_extended{};
};

} // namespace zpp::x64
