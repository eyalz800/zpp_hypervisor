#pragma once
#include <cstdint>
#include <type_traits>

namespace zpp::arch::x86_64
{
/**
 * Represents a 64 bit IDT gate descriptor, which unlike a segment
 * descriptor is always sixteen bytes wide.
 */
class interrupt_gate
{
public:
    /**
     * Gate types. An interrupt gate clears the interrupt flag on entry, a
     * trap gate leaves it as it was.
     */
    enum class gate_type
    {
        interrupt = 0xe,
        trap = 0xf,
    };

    /**
     * Constructs an empty gate descriptor.
     */
    constexpr interrupt_gate() = default;

    /**
     * Creates a gate descriptor from the entry and extended values.
     */
    constexpr explicit interrupt_gate(std::uint64_t entry,
                                      std::uint64_t extended = {}) :
        m_entry(entry), m_extended(extended)
    {
    }

    /**
     * Returns the offset field, which is the address of the handler.
     */
    constexpr std::uint64_t offset() const
    {
        return (m_entry & 0xffffull) |
               (((m_entry >> 48) & 0xffffull) << 16) |
               ((m_extended & 0xffffffffull) << 32);
    }

    /**
     * Sets the offset field, which is the address of the handler.
     */
    constexpr void offset(std::uint64_t value)
    {
        m_entry = (m_entry & ~0xffff00000000ffffull) |
                  (value & 0xffffull) |
                  (((value >> 16) & 0xffffull) << 48);
        m_extended = (m_extended & ~0xffffffffull) |
                     ((value >> 32) & 0xffffffffull);
    }

    /**
     * Returns the code segment selector of the handler.
     */
    constexpr std::uint16_t selector() const
    {
        return (m_entry >> 16) & 0xffffull;
    }

    /**
     * Sets the code segment selector of the handler.
     */
    constexpr void selector(std::uint16_t value)
    {
        m_entry =
            (m_entry & ~(0xffffull << 16)) | (std::uint64_t{value} << 16);
    }

    /**
     * Returns the interrupt stack table index, where zero means the
     * handler runs on the stack that was already in use.
     */
    constexpr std::uint64_t interrupt_stack_table() const
    {
        return (m_entry >> 32) & 0x7ull;
    }

    /**
     * Sets the interrupt stack table index.
     */
    constexpr void interrupt_stack_table(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0x7ull << 32)) | ((value & 0x7ull) << 32);
    }

    /**
     * Returns the type field of the gate descriptor.
     */
    constexpr gate_type type() const
    {
        return gate_type((m_entry >> 40) & 0xfull);
    }

    /**
     * Sets the type field of the gate descriptor.
     */
    constexpr void type(gate_type value)
    {
        m_entry =
            (m_entry & ~(0xfull << 40)) |
            ((std::underlying_type_t<gate_type>(value) & 0xfull) << 40);
    }

    /**
     * Returns the privilege level field of the gate descriptor.
     */
    constexpr std::uint64_t privilege_level() const
    {
        return (m_entry >> 45) & 0x3ull;
    }

    /**
     * Sets the privilege level field of the gate descriptor.
     */
    constexpr void privilege_level(std::uint64_t value)
    {
        m_entry = (m_entry & ~(0x3ull << 45)) | ((value & 0x3ull) << 45);
    }

    /**
     * Returns the present bit of the gate descriptor.
     */
    constexpr bool present() const
    {
        return (m_entry >> 47) & 0x1ull;
    }

    /**
     * Sets the present bit of the gate descriptor.
     */
    constexpr void present(bool value)
    {
        m_entry =
            (m_entry & ~(0x1ull << 47)) | (std::uint64_t{value} << 47);
    }

    /**
     * Returns the basic integral entry value of the gate descriptor.
     */
    constexpr std::uint64_t basic_value() const
    {
        return m_entry;
    }

    /**
     * Returns the extended integral entry value of the gate descriptor.
     */
    constexpr std::uint64_t extended_value() const
    {
        return m_extended;
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

} // namespace zpp::arch::x86_64
