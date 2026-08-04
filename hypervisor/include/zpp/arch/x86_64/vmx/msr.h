#pragma once
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * The VMX capability MSR addresses. Separate from the architectural MSRs
 * next door because these exist only where VT-x does.
 */
namespace msr
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
} // namespace msr

/**
 * Consults a value with a specified MSR and returns the adjusted value.
 */
constexpr std::uint64_t adjust_msr(std::uint64_t msr, std::uint64_t value)
{
    value &= ((0xffffffff00000000 & msr) >> 32);
    value |= 0xffffffff & msr;
    return value;
}

} // namespace zpp::arch::x86_64::vmx
