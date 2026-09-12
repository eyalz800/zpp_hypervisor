#pragma once
#include "zpp/arch/x86_64/memory_type.h"
#include "zpp/arch/x86_64/msr.h"
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <type_traits>

namespace zpp::arch::x86_64
{
/**
 * True for the five memory types an MTRR, and an EPT paging-structure
 * entry, may name.
 *
 * SDM Vol. 3A 14.11.2.1, on the IA32_MTRR_DEF_TYPE type field: "The legal
 * values for this field are 0, 1, 4, 5, and 6. All other values result in
 * a general-protection exception (#GP) being generated."
 *
 * The same five are the legal EPT memory types, which is why this is worth
 * checking before a value read out of an MSR reaches an EPT entry. SDM
 * Vol. 3C 31.3.7.2: "The EPT memory type is specified in bits 5:3 of the
 * last EPT paging-structure entry: 0 = UC; 1 = WC; 4 = WT; 5 = WP; and 6 =
 * WB. Other values are reserved and cause EPT misconfigurations".
 */
constexpr bool is_valid(memory_type type)
{
    switch (type) {
    case memory_type::uncachable:
    case memory_type::write_combining:
    case memory_type::write_through:
    case memory_type::write_protected:
    case memory_type::write_back:
        return true;
    default:
        return false;
    }
}

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
    arch::x86_64::memory_type type{};

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
 * Represents IA32_MTRR_DEF_TYPE, which holds the memory type used for
 * every physical range no MTRR covers, plus the two enables that decide
 * which MTRRs are consulted at all.
 *
 * SDM Vol. 3A 14.11.2.1, "IA32_MTRR_DEF_TYPE MSR", and Figure 14-6.
 */
class mtrr_default_type
{
public:
    /**
     * Creates an empty MTRR default type.
     */
    constexpr mtrr_default_type() = default;

    /**
     * Creates an MTRR default type from an integral representation.
     */
    constexpr mtrr_default_type(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the default memory type, bits 7:0.
     *
     * SDM Vol. 3A 14.11.2.1: "Type field, bits 0 through 7 - Indicates the
     * default memory type used for those physical memory address ranges
     * that do not have a memory type specified for them by an MTRR".
     */
    constexpr memory_type type() const
    {
        return arch::x86_64::memory_type(m_value & 0xff);
    }

    /**
     * Sets the default memory type to the specified value.
     */
    constexpr void type(arch::x86_64::memory_type value)
    {
        m_value =
            (m_value & ~0xffull) |
            (std::underlying_type_t<arch::x86_64::memory_type>(value) &
             0xff);
    }

    /**
     * Returns the FE flag, bit 10, which enables the fixed-range MTRRs.
     *
     * SDM Vol. 3A 14.11.2.1: "FE (fixed MTRRs enabled) flag, bit 10 -
     * Fixed-range MTRRs are enabled when set ... When the fixed-range
     * MTRRs are enabled, they take priority over the variable-range MTRRs
     * when overlaps in ranges occur."
     */
    constexpr bool fixed_range_enabled() const
    {
        return m_value & (1 << 10);
    }

    /**
     * Sets the FE flag to the specified value.
     */
    constexpr void fixed_range_enabled(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 10)) | (std::uint64_t{value} << 10);
    }

    /**
     * Returns the E flag, bit 11, which enables the MTRRs at all.
     *
     * SDM Vol. 3A 14.11.2.1: "E (MTRRs enabled) flag, bit 11 - MTRRs are
     * enabled when set; all MTRRs are disabled when clear, and the UC
     * memory type is applied to all of physical memory."
     */
    constexpr bool enabled() const
    {
        return m_value & (1 << 11);
    }

    /**
     * Sets the E flag to the specified value.
     */
    constexpr void enabled(bool value)
    {
        m_value =
            (m_value & ~(0x1ull << 11)) | (std::uint64_t{value} << 11);
    }

    /**
     * Returns the integral representation of the MTRR default type.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Returns the integral representation of the MTRR default type.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the MTRR default type.
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
        return arch::x86_64::memory_type(m_value & 0xff);
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
    constexpr void memory_type(arch::x86_64::memory_type value)
    {
        memory_type(
            std::underlying_type_t<arch::x86_64::memory_type>(value));
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

/**
 * Decodes one variable-range MTRR pair into the range it names.
 */
constexpr mtrr make_mtrr(mtrr_variable_base base, mtrr_variable_mask mask)
{
    mtrr result{};
    result.type = base.memory_type();
    result.valid = mask.valid();

    // A zero mask satisfies the match rule below for every address, so it
    // would name the whole physical address space. No firmware expresses a
    // range that way - the default type is what says something about every
    // uncovered address - so it is declined rather than honoured.
    // Declining it here also removes a matching hazard: the range used to
    // be left valid with a size of zero, and a zero-size range still
    // matched any address at or above its base under a comparison written
    // as (address >= base + size).
    if (!mask.physical_mask()) {
        result.valid = false;
        return result;
    }

    // SDM Vol. 3A 14.11.2.3, on the PhysMask field: "Address_Within_Range
    // AND PhysMask = PhysBase AND PhysMask". The bits of PhysBase that the
    // mask clears take no part in that comparison, so the range begins at
    // PhysBase AND PhysMask rather than at PhysBase, and firmware that
    // leaves a base unaligned to its own size still names the aligned
    // range.
    result.physical_base = (base.page_number() & mask.physical_mask())
                           << 12;

    // A well formed mask is a run of ones at the top of the field, so the
    // number of zeroes below that run gives the size: each one doubles a
    // 4 KB minimum.
    result.size = 0x1000;
    for (auto bits = mask.physical_mask(); !(bits & 1); bits >>= 1) {
        result.size <<= 1;
    }

    return result;
}

/**
 * One fixed-range MTRR: the MSR holding it, the physical base of the first
 * of the eight sub-ranges it describes, and the size of each sub-range.
 */
struct mtrr_fixed_range
{
    /**
     * How many sub-ranges one fixed-range register describes. Each is one
     * byte of the 64 bit register.
     *
     * SDM Vol. 3A 14.11.2.2: "The fixed memory ranges are mapped with 11
     * fixed-range registers of 64 bits each. Each of these registers is
     * divided into 8-bit fields that are used to specify the memory type
     * for each of the sub-ranges the register controls".
     */
    static constexpr std::size_t sub_ranges = 8;

    /**
     * The MSR address of the register.
     */
    std::size_t msr{};

    /**
     * The physical base of the first sub-range, which is the one bits 7:0
     * describe.
     */
    std::uint64_t base{};

    /**
     * The size of each of the eight sub-ranges.
     */
    std::uint64_t sub_range_size{};

    /**
     * Returns the first address past the last sub-range.
     */
    constexpr std::uint64_t end() const
    {
        return base + (sub_ranges * sub_range_size);
    }
};

/**
 * The eleven fixed-range MTRRs and the addresses they map, in register
 * order. Within one register bits 7:0 describe the lowest addressed
 * sub-range and bits 63:56 the highest, which is the direction Table 14-9
 * reads: its columns run from 63:56 down to 7:0 while its address ranges
 * run from high to low.
 *
 * SDM Vol. 3A 14.11.2.2 and Table 14-9, "Address Mapping for Fixed-Range
 * MTRRs": IA32_MTRR_FIX64K_00000 "Maps the 512-KByte address range from 0H
 * to 7FFFFH ... divided into eight 64-KByte sub-ranges";
 * IA32_MTRR_FIX16K_80000 and IA32_MTRR_FIX16K_A0000 "Maps the two
 * 128-KByte address ranges from 80000H to BFFFFH ... divided into sixteen
 * 16-KByte sub-ranges, 8 ranges per register"; IA32_MTRR_FIX4K_C0000
 * through IA32_MTRR_FIX4K_F8000 "Maps eight 32-KByte address ranges from
 * C0000H to FFFFFH ... divided into sixty-four 4-KByte sub-ranges, 8
 * ranges per register".
 */
inline constexpr mtrr_fixed_range mtrr_fixed_ranges[] = {
    {msr::mtrr::fix64k_00000, 0x00000, 0x10000},
    {msr::mtrr::fix16k_80000, 0x80000, 0x04000},
    {msr::mtrr::fix16k_a0000, 0xa0000, 0x04000},
    {msr::mtrr::fix4k_c0000, 0xc0000, 0x01000},
    {msr::mtrr::fix4k_c8000, 0xc8000, 0x01000},
    {msr::mtrr::fix4k_d0000, 0xd0000, 0x01000},
    {msr::mtrr::fix4k_d8000, 0xd8000, 0x01000},
    {msr::mtrr::fix4k_e0000, 0xe0000, 0x01000},
    {msr::mtrr::fix4k_e8000, 0xe8000, 0x01000},
    {msr::mtrr::fix4k_f0000, 0xf0000, 0x01000},
    {msr::mtrr::fix4k_f8000, 0xf8000, 0x01000},
};

/**
 * The processor's whole MTRR state, and the derivation of a memory type
 * from it.
 *
 * The reason the whole of it is needed rather than the variable ranges
 * alone: under EPT the guest's MTRRs are not consulted by hardware at all,
 * so whatever this derives is the only memory type information the guest's
 * physical address space has. SDM Vol. 3C 31.3.7.2: "The MTRRs have no
 * effect on the memory type used for an access to a guest-physical
 * address." The effective type is "the combination of the EPT memory type
 * and the PAT memory type specified in Table 14-7 in Section 14.5.2.2,
 * using the EPT memory type in place of the MTRR memory type" - so an EPT
 * entry left write-back over a range firmware marked uncacheable gives
 * cached MMIO, and the guest has no way to correct it.
 */
struct mtrr_state
{
    /**
     * How many variable range MTRRs there can be. IA32_MTRRCAP.VCNT is an
     * eight bit field, so this is the architectural maximum rather than a
     * guess at what a machine will report.
     *
     * It was 8 in an earlier arrangement of this state, which is what QEMU
     * reports and is why nothing caught it: real Intel client parts
     * commonly report 10, and the fill loop ran to VCNT without a bound.
     * The two entries past the end landed on the capabilities member that
     * followed the array, so the capabilities were corrupted by the very
     * loop that had just read them, and the last two MTRRs were dropped
     * from the EPT derivation.
     *
     * SDM Vol. 4, Table 2-2, IA32_MTRRCAP: "VCNT (Variable Range
     * Registers Count) field, bits 7:0".
     */
    static constexpr std::size_t maximum_variable_ranges = 255;

    /**
     * How many fixed-range registers there are.
     */
    static constexpr std::size_t fixed_range_count = 11;

    /**
     * The first address the fixed-range MTRRs do not describe. SDM
     * Vol. 3A 14.11.4.1 rule 1 is phrased in terms of "the first 1 MByte
     * of physical memory", which is what the table above covers exactly.
     */
    static constexpr std::uint64_t fixed_range_limit = 0x100000;

    /**
     * Returned by next_boundary_after when no address above the one given
     * can change the derived memory type.
     */
    static constexpr std::uint64_t no_boundary = ~std::uint64_t{};

    static_assert(fixed_range_count == std::size(mtrr_fixed_ranges));

    /**
     * IA32_MTRRCAP.
     */
    mtrr_capabilities capabilities{};

    /**
     * IA32_MTRR_DEF_TYPE.
     */
    mtrr_default_type default_type{};

    /**
     * The raw contents of the eleven fixed-range registers, in the order
     * of mtrr_fixed_ranges.
     */
    std::uint64_t fixed[fixed_range_count]{};

    /**
     * How many entries of the variable array below the processor reports,
     * which is what every loop over it runs to. Entries past it are never
     * read, so their contents do not matter.
     */
    std::size_t variable_count{};

    /**
     * The decoded variable range MTRRs.
     */
    mtrr variable[maximum_variable_ranges]{};

    /**
     * True when the fixed-range registers both exist and are turned on,
     * which is together what makes them govern the first 1 MB.
     *
     * Both halves are needed. IA32_MTRRCAP.FIX says the registers exist at
     * all, and reading an MSR a processor does not implement raises #GP;
     * MTRRdefType.FE says firmware turned them on. SDM Vol. 3A Example
     * 14-5, Get4KMemType(), opens with exactly this conjunction: "IF
     * IA32_MTRRCAP.FIX AND MTRRdefType.FE".
     */
    constexpr bool fixed_ranges_in_use() const
    {
        return capabilities.fixed_range_registers_supported() &&
               default_type.fixed_range_enabled();
    }

    /**
     * Returns the type unchanged when it is one an EPT entry may name, and
     * uncacheable otherwise.
     *
     * A reserved value cannot legitimately be in an MTRR - WRMSR faults on
     * one - so this only fires on a processor or firmware that broke that
     * rule. It matters anyway because the alternative is writing the
     * reserved value into an EPT entry, and that is an EPT
     * misconfiguration on the first access: an exit this VMM does not
     * handle, which stops the CPU. Uncacheable is the choice that cannot
     * corrupt anything, only slow it down.
     */
    static constexpr memory_type valid_or_uncachable(memory_type type)
    {
        return is_valid(type) ? type : memory_type::uncachable;
    }

    /**
     * Returns the memory type the fixed-range MTRRs give an address below
     * 1 MB. Only meaningful when fixed_ranges_in_use().
     */
    constexpr memory_type
    fixed_type_of(std::uint64_t physical_address) const
    {
        for (std::size_t i{}; i < fixed_range_count; ++i) {
            const auto & range = mtrr_fixed_ranges[i];
            if (physical_address < range.base ||
                physical_address >= range.end()) {
                continue;
            }

            auto index =
                (physical_address - range.base) / range.sub_range_size;
            return valid_or_uncachable(
                memory_type((fixed[i] >> (index * 8)) & 0xff));
        }

        // The table covers 0 to 1 MB without a gap, so this is reachable
        // only for an address the caller should not have asked about.
        return valid_or_uncachable(default_type.type());
    }

    /**
     * Returns the memory type the MTRRs give a physical address.
     *
     * SDM Vol. 3A 14.11.4.1, "MTRR Precedences", implemented in the order
     * it states:
     *
     * "If the MTRRs are not enabled (by setting the E flag in the
     * IA32_MTRR_DEF_TYPE MSR), then all memory accesses are of the UC
     * memory type."
     *
     * 1. "If the physical address falls within the first 1 MByte of
     *    physical memory and fixed MTRRs are enabled, the processor uses
     *    the memory type stored for the appropriate fixed-range MTRR."
     * 2. "Otherwise, the processor attempts to match the physical address
     *    with a memory type set by the variable-range MTRRs", combining
     *    matches by the four rules below.
     * 3. "If no fixed or variable memory range matches, the processor uses
     *    the default memory type."
     *
     * Note the SDM's own Get4KMemType() pseudocode in Example 14-5 does
     * *not* implement step 2: it returns the type of the first matching
     * variable range and never looks at a second. The prose is the
     * normative statement and the pseudocode is a simplification, so the
     * prose is what this follows. That difference is the whole of what was
     * wrong here before.
     */
    constexpr memory_type type_of(std::uint64_t physical_address) const
    {
        if (!default_type.enabled()) {
            return memory_type::uncachable;
        }

        // Rule 1. The fixed ranges take priority over any variable range
        // that also covers the address, so a match here is final.
        if (fixed_ranges_in_use() &&
            physical_address < fixed_range_limit) {
            return fixed_type_of(physical_address);
        }

        // Rule 2.
        std::optional<memory_type> combined;
        for (std::size_t i{}; i < variable_count; ++i) {
            const auto & range = variable[i];
            if (!range.valid) {
                continue;
            }

            // Written as a subtraction rather than as
            // (physical_address >= base + size) so that a range reaching
            // the top of the address space cannot wrap.
            if (physical_address < range.physical_base ||
                (physical_address - range.physical_base) >= range.size) {
                continue;
            }

            auto type = valid_or_uncachable(range.type);

            // "If one variable memory range matches, the processor uses
            // the memory type stored in the IA32_MTRR_PHYSBASEn register
            // for that range."
            if (!combined) {
                combined = type;
                continue;
            }

            // "If two or more variable memory ranges match and the memory
            // types are identical, then that memory type is used."
            if (*combined == type) {
                continue;
            }

            // "If two or more variable memory ranges match and one of the
            // memory types is UC, the UC memory type used."
            //
            // Tested on both sides. KVM tests only the range it has just
            // reached (mtrr.c, kvm_mtrr_get_guest_memory_type: "if
            // (curr_type == MTRR_TYPE_UNCACHABLE) return
            // MTRR_TYPE_UNCACHABLE"), so a UC range followed by a WB range
            // falls through its write-through check - UC is not in its
            // wt_wb_mask - and comes out WB. The rule as written does not
            // depend on the order the ranges are visited in.
            if (memory_type::uncachable == type ||
                memory_type::uncachable == *combined) {
                return memory_type::uncachable;
            }

            // "If two or more variable memory ranges match and the memory
            // types are WT and WB, the WT memory type is used."
            if (is_write_through_or_back(type) &&
                is_write_through_or_back(*combined)) {
                combined = memory_type::write_through;
                continue;
            }

            // "For overlaps not defined by the above rules, processor
            // behavior is undefined."
            //
            // KVM answers write-back here, with a comment saying so. This
            // answers uncacheable instead, because the two mistakes are
            // not symmetric under EPT: the guest cannot weaken a type this
            // VMM handed it, so a wrongly cacheable device range corrupts
            // silently while a wrongly uncacheable one is only slow. No
            // firmware programs an overlap of this shape - it is undefined
            // on hardware too - so neither answer is expected to be used.
            return memory_type::uncachable;
        }

        if (combined) {
            return *combined;
        }

        // Rule 3.
        return valid_or_uncachable(default_type.type());
    }

    /**
     * Returns the lowest address strictly above the one given at which
     * type_of can change its answer, or no_boundary when nothing above it
     * can.
     *
     * This is what makes a range query cost the number of MTRRs rather
     * than the number of pages in the range.
     */
    constexpr std::uint64_t
    next_boundary_after(std::uint64_t physical_address) const
    {
        // One type for all of physical memory, so there is no boundary.
        if (!default_type.enabled()) {
            return no_boundary;
        }

        // Inside the fixed ranges the sub-range end is the next place the
        // answer can change, since nothing else is consulted there. The
        // last sub-range ends at exactly 1 MB, which is where the variable
        // ranges take over, so a walk needs no separate step for that.
        if (fixed_ranges_in_use() &&
            physical_address < fixed_range_limit) {
            for (const auto & range : mtrr_fixed_ranges) {
                if (physical_address < range.base ||
                    physical_address >= range.end()) {
                    continue;
                }

                auto index =
                    (physical_address - range.base) / range.sub_range_size;
                return range.base + ((index + 1) * range.sub_range_size);
            }
        }

        // Below 1 MB with the fixed ranges off, 1 MB is not a boundary:
        // the variable ranges and the default type govern either side of
        // it alike, so only their own edges matter.
        auto result = no_boundary;
        for (std::size_t i{}; i < variable_count; ++i) {
            const auto & range = variable[i];
            if (!range.valid) {
                continue;
            }

            if (range.physical_base > physical_address &&
                range.physical_base < result) {
                result = range.physical_base;
            }

            auto end = range.physical_base + range.size;
            if (end > physical_address && end < result) {
                result = end;
            }
        }

        return result;
    }

    /**
     * Returns the memory type of [physical_address, physical_address +
     * size) when every part of it has the same one, and nothing when the
     * range straddles a change of type.
     *
     * Nothing is the answer that says a large page cannot describe the
     * range.
     */
    constexpr std::optional<memory_type> uniform_type_of(
        std::uint64_t physical_address, std::uint64_t size) const
    {
        auto type = type_of(physical_address);
        auto end = physical_address + size;

        for (auto address = physical_address; address < end;) {
            if (type_of(address) != type) {
                return {};
            }

            auto next = next_boundary_after(address);

            // Every return above is strictly greater than the address
            // passed in, so this cannot fire. It stays because what it
            // would otherwise be is an unbounded loop during EPT
            // construction on the boot processor, which is
            // indistinguishable from a dead machine - and reporting no
            // uniform type merely costs a split into 4 KB entries, which
            // is always correct.
            if (next <= address) {
                return {};
            }

            address = next;
        }

        return type;
    }

private:
    /**
     * True for the two types rule 2's write-through case names.
     */
    static constexpr bool is_write_through_or_back(memory_type type)
    {
        return memory_type::write_through == type ||
               memory_type::write_back == type;
    }
};

} // namespace zpp::arch::x86_64
