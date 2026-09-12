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

/**
 * Whether `value` is a legal CR0 or CR4 in VMX operation, judged against
 * the IA32_VMX_CR{0,4}_FIXED0 and _FIXED1 pair for that register.
 *
 * SDM A.7 and A.8 (.references/sdm.txt:223460 and :223468) define the
 * pair the same way for both registers: "If bit X is 1 in
 * IA32_VMX_CR4_FIXED0, then that bit of CR4 is fixed to 1 in VMX
 * operation. Similarly, if bit X is 0 in IA32_VMX_CR4_FIXED1, then that
 * bit is fixed to 0". So a bit clear in `value` and set in `fixed_0` is
 * illegal, and so is a bit set in `value` and clear in `fixed_1` - which
 * is the whole of the test, and is KVM's `fixed_bits_valid`
 * (.references/kvm/nested.h:258).
 *
 * `exempt` names bits to judge neither way, and both callers here need
 * one. It is symmetric - removed from the "must be 1" half *and* the
 * "must be 0" half - because the two bits it is used for sit on
 * opposite sides: CR4.VMXE is fixed to 1 on any processor with VMX and
 * CR4.SMXE is fixed to 0 on any processor without SMX, and this VMM
 * forces the first on and the second off in the register whatever the
 * guest wrote. KVM exempts by clearing `fixed0` alone
 * (`nested_guest_cr0_valid`, nested.h:263, drops CR0.PE and CR0.PG for
 * an unrestricted guest), which is enough only for the one-sided case.
 *
 * Note what this does *not* do, since the omission is deliberate: it
 * takes no CPUID leaf. KVM gates CR4 bits on the guest's own CPUID -
 * `cr4_guest_rsvd_bits` from `__cr4_reserved_bits`, and for a nested
 * guest `nested_vmx_cr_fixed1_bits_update` (.references/kvm/vmx.c:7703)
 * rebuilds the whole allowed-1 mask out of CPUID. That would be
 * redundant here and checkably so: the only CR4-relevant CPUID bits
 * this VMM edits are leaf 1 ECX[5] (VMX) and ECX[6] (SMX), which are
 * exactly the two bits `exempt` covers at both call sites. A third
 * edited CPUID feature bit with a CR4 bit behind it would break that
 * argument and would need the table.
 */
constexpr bool fixed_bits_valid(std::uint64_t value,
                                std::uint64_t fixed_0,
                                std::uint64_t fixed_1,
                                std::uint64_t exempt = 0)
{
    return 0 == (((~value & fixed_0) | (value & ~fixed_1)) & ~exempt);
}

} // namespace zpp::arch::x86_64::vmx
