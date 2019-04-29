#pragma once
#include <cstddef>
#include <cstdint>

namespace zpp::x64::intel
{
/**
 * MSR addresses.
 */
namespace msr
{
enum type : std::size_t
{
    ia32_extended_feature_enable = 0xc0000080,
    ia32_mtrr_capability = 0xfe,
    ia32_debug_control = 0x1d9,
    ia32_fs_base = 0xC0000100,
    ia32_gs_base = 0xC0000101,
};
} // namespace msr

/**
 * VMX MSR addresses
 */
namespace msr::vmx
{
static constexpr std::size_t begin = 0x480;
static constexpr std::size_t end = 0x492;
static constexpr std::size_t size = end - begin;

enum type : std::size_t
{
    basic = 0x480,
    pin_based_controls = 0x481,
    processor_based_contorls = 0x482,
    exit_controls = 0x483,
    entry_controls = 0x484,
    misc = 0x485,
    cr0_fixed_0 = 0x486,
    cr0_fixed_1 = 0x487,
    cr4_fixed_0 = 0x488,
    cr4_fixed_1 = 0x489,
    vmcs_enum = 0x48a,
    processor_based_contorls_2 = 0x48b,
    vpid_ept_capability = 0x48c,
    true_pin_based_controls = 0x48d,
    true_processor_based_controls = 0x48e,
    true_exit_controls = 0x48f,
    true_entry_controls = 0x490,
    vm_functions = 0x491,
};
} // namespace msr::vmx

/**
 * MTRR msr addresses.
 */
namespace msr::mtrr
{
enum type : std::size_t
{
    physbase_0 = 0x200,
    physmask_0,
    physbase_1,
    physmask_1,
    physbase_2,
    physmask_2,
    physbase_3,
    physmask_3,
    physbase_4,
    physmask_4,
    physbase_5,
    physmask_5,
    physbase_6,
    physmask_6,
    physbase_7,
    physmask_7,
};
} // namespace msr::mtrr

/**
 * Consults a value with a specified MSR and returns the adjusted value.
 */
inline std::uint64_t adjust_msr(std::uint64_t msr, std::uint64_t value)
{
    value &= ((0xffffffff00000000 & msr) >> 32);
    value |= 0xffffffff & msr;
    return value;
}

} // namespace zpp::x64::intel