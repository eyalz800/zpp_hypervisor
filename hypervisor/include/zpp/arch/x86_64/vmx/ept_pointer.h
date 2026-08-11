#pragma once
#include "zpp/arch/x86_64/memory_type.h"
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace zpp::arch::x86_64::vmx
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
    constexpr arch::x86_64::memory_type memory_type() const
    {
        return arch::x86_64::memory_type((m_value >> 0) & 0x7);
    }

    /**
     * Sets the memory type field to the specified value.
     */
    constexpr void memory_type(arch::x86_64::memory_type value)
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
     * Returns whether accessed and dirty flags are enabled for the
     * extended page tables this pointer names.
     *
     * Bit 6. SDM 29.2.1.1: "Bit 6 (enable bit for accessed and dirty
     * flags for EPT) must be 0 if bit 21 of the IA32_VMX_EPT_VPID_CAP
     * MSR ... is read as 0" (.references/sdm.txt:202160).
     *
     * It used to read bit 8, which is a different thing one level down:
     * bit 8 of a leaf *entry* is that page's accessed flag, and SDM
     * Table 31-7 (.references/sdm.txt:205542) makes it meaningful only
     * "If bit 6 of EPTP is 1". Reading the enable out of the flag it
     * enables confuses the pointer with what it points at.
     */
    constexpr bool access_and_dirty() const
    {
        return 0 != (m_value & (1ull << 6));
    }

    /**
     * Enables or disables accessed and dirty flags.
     *
     * One bit, written as one bit. The previous version was copied from
     * `page_walk_length` above, which legitimately stores its value
     * minus one - so `access_and_dirty(true)` evaluated
     * `((1 - 1) & 0x7) << 8` and cleared three bits, and
     * `access_and_dirty(false)` evaluated `((0 - 1) & 0x7) << 8` and set
     * bits 10:8. Those are reserved, and `build_vmcs02` refuses a guest
     * hypervisor's pointer that has them - so *clearing* this flag
     * produced a pointer this VMM would reject.
     *
     * Nothing called either, and that is not a coincidence:
     * `build_vmcs02` tests bit 6 with a literal `0x40` rather than
     * through this class. A class that answers wrongly is a class its
     * neighbours route around.
     */
    constexpr void access_and_dirty(bool value)
    {
        m_value = (m_value & ~(1ull << 6)) |
                  (value ? (1ull << 6) : std::uint64_t{});
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

} // namespace zpp::arch::x86_64::vmx
