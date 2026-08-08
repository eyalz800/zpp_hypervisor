#pragma once
#include "zpp/arch/x86_64/vmx/vmx.h"
#include <cstdint>

namespace zpp::hypervisor::nested_vmx
{
/**
 * Whether the guest is told VT-x exists.
 *
 * Off by default, and the default is the whole of the decision. With it
 * off, CPUID leaf 1 ECX[5] reports no VMX, CR4.VMXE reads back clear
 * through its shadow, and the capability MSRs are not intercepted - so a
 * guest hypervisor stands down before it executes anything, and every VMX
 * instruction it might still attempt takes the #UD the exit handler gives
 * it. That is a consistent machine: no VMX in CPUID, no VMXE in CR4, and
 * #UD on the instructions. It is the state this VMM boots Windows in
 * today, VBS included, because Hyper-V launches ahead of Windows whenever
 * VBS is on and hands straight off when it finds no VMX.
 *
 * With it on, the guest is told VMX exists and the machinery below
 * answers for it: VMXON through VMPTRST keep a shadow VMCS per processor,
 * the capability MSRs report a subset of the hardware's, and
 * IA32_FEATURE_CONTROL is answered rather than passed through.
 *
 * **What is still missing, and why it is off:** VMLAUNCH and VMRESUME do
 * not run a second-level guest. There is no vmcs02 built from the shadow,
 * no reflection of a second-level exit back to the first level, and no
 * shadow of the first level's extended page tables. So a guest hypervisor
 * gets as far as a fully written VMCS and is then refused, which for
 * Hyper-V means a failure at launch rather than a clean stand-down. That
 * is strictly worse than not being told about VMX at all, which is why
 * this is off until those three exist.
 *
 * Turning it on: -DZPP_NESTED_VMX=ON. Useful for exercising the
 * instruction emulation and the capability reporting under a debugger,
 * where the refusal at VMLAUNCH is the expected end of the run.
 */
inline constexpr bool enabled =
#if defined(ZPP_NESTED_VMX) && ZPP_NESTED_VMX
    true;
#else
    false;
#endif

/**
 * The value the guest's current-VMCS pointer takes when there is none.
 *
 * All ones rather than zero, because zero names physical page zero. SDM
 * 33.3, VMXON, sets "current-VMCS pointer := FFFFFFFF_FFFFFFFFH" on
 * entering VMX operation, so this is the architecture's own sentinel and
 * not one invented here.
 */
constexpr std::uint64_t no_current_vmcs = ~std::uint64_t{};

/**
 * The VM-instruction error numbers, from SDM Table 33-1, "VM-Instruction
 * Error Numbers". Only the ones reachable from here are named; the table
 * has more, and every one of the rest belongs to the dual-monitor
 * treatment of system-management interrupts, which this does not
 * implement.
 */
enum class instruction_error : std::uint64_t
{
    vmcall_in_vmx_root_operation = 1,
    vmclear_invalid_address = 2,
    vmclear_with_vmxon_pointer = 3,
    vmlaunch_with_non_clear_vmcs = 4,
    vmresume_with_non_launched_vmcs = 5,
    entry_invalid_control_field = 7,
    entry_invalid_host_state_field = 8,
    vmptrld_invalid_address = 9,
    vmptrld_with_vmxon_pointer = 10,
    vmptrld_incorrect_revision_id = 11,
    unsupported_vmcs_component = 12,
    vmwrite_to_read_only_component = 13,
    vmxon_in_vmx_root_operation = 15,
    invalid_operand_to_invept_invvpid = 28,
};

/**
 * The pin-based controls a first-level hypervisor is permitted to set.
 *
 * A capability MSR reports allowed-0 settings in bits 31:0 and allowed-1
 * settings in bits 63:32 (SDM A.3.1), so this is the mask the hardware's
 * allowed-1 half is narrowed by. Narrowing is the point: every control
 * reported here is one whose effect would have to be honoured when a
 * second-level guest runs, and reporting one that is then ignored is the
 * half-answered interface this codebase warns about.
 *
 * External-interrupt and NMI exiting are the two a hypervisor cannot do
 * without, since without them it cannot own its own guest's interrupts.
 * The VMX-preemption timer is deliberately absent, and there is a second
 * reason beyond honesty: the test rig runs QEMU with hv-passthrough, which
 * filters the capability MSRs through enlightened VMCS version 1 and
 * reports the timer as not permitted anyway - so a build that asked for it
 * would fail its own VM entry on the machine it is developed on. Hence the
 * mask is applied *to* the hardware's value rather than replacing it.
 */
constexpr std::uint64_t supported_pin_based_controls =
    (1ull << 0) | // External-interrupt exiting.
    (1ull << 3) | // NMI exiting.
    (1ull << 5);  // Virtual NMIs.

/**
 * The primary processor-based controls a first-level hypervisor may set.
 *
 * The MSR bitmaps and the secondary controls are the two that matter: a
 * hypervisor without the bitmaps traps every MSR access its guest makes,
 * and one without the secondary controls has no EPT and no unrestricted
 * guest. The rest are the plain intercepts, each of which costs nothing
 * to honour because it only ever adds an exit.
 *
 * Absent on purpose: TPR shadow and the CR3-target list. Both need state
 * of their own in the second-level VMCS that nothing here maintains.
 */
constexpr std::uint64_t supported_primary_controls =
    (1ull << 2) |  // Interrupt-window exiting.
    (1ull << 3) |  // Use TSC offsetting.
    (1ull << 7) |  // HLT exiting.
    (1ull << 9) |  // INVLPG exiting.
    (1ull << 10) | // MWAIT exiting.
    (1ull << 11) | // RDPMC exiting.
    (1ull << 12) | // RDTSC exiting.
    (1ull << 15) | // CR3-load exiting.
    (1ull << 16) | // CR3-store exiting.
    (1ull << 19) | // CR8-load exiting.
    (1ull << 20) | // CR8-store exiting.
    (1ull << 22) | // NMI-window exiting.
    (1ull << 23) | // MOV-DR exiting.
    (1ull << 24) | // Unconditional I/O exiting.
    (1ull << 25) | // Use I/O bitmaps.
    (1ull << 27) | // MONITOR trap flag.
    (1ull << 28) | // Use MSR bitmaps.
    (1ull << 29) | // MONITOR exiting.
    (1ull << 30) | // PAUSE exiting.
    (1ull << 31);  // Activate secondary controls.

/**
 * The secondary processor-based controls a first-level hypervisor may set.
 *
 * Extended page tables and VPIDs are both absent, and that absence is the
 * single fact that decides whether a real guest hypervisor can run here.
 * Hyper-V and every other modern one require EPT, so this is what makes
 * the refusal at VMLAUNCH honest rather than surprising: a first-level
 * hypervisor is told there is no EPT, and IA32_VMX_EPT_VPID_CAP reads as
 * zero to agree with it.
 *
 * Supporting them means shadowing the first level's extended page tables
 * against this VMM's own, which has to combine two sets of permissions
 * per page and rebuild on the first level's INVEPT. Nothing here does
 * that, and reporting the control without doing it would hand a guest a
 * page table that is never consulted.
 */
constexpr std::uint64_t supported_secondary_controls =
    (1ull << 2) |  // Descriptor-table exiting.
    (1ull << 3) |  // Enable RDTSCP.
    (1ull << 6) |  // WBINVD exiting.
    (1ull << 10) | // PAUSE-loop exiting.
    (1ull << 11) | // RDRAND exiting.
    (1ull << 12) | // Enable INVPCID.
    (1ull << 16) | // RDSEED exiting.
    (1ull << 20);  // Enable XSAVES/XRSTORS.

/**
 * The VM-exit controls a first-level hypervisor may set.
 *
 * "Host address-space size" is here because a 64-bit hypervisor cannot do
 * without it, and the MSR-area ones because saving and loading IA32_PAT
 * and IA32_EFER across a transition is a field copy and nothing more.
 * "Save VMX-preemption timer value" is absent to agree with the pin-based
 * mask, which does not offer the timer.
 */
constexpr std::uint64_t supported_exit_controls =
    (1ull << 2) |  // Save debug controls.
    (1ull << 9) |  // Host address-space size.
    (1ull << 15) | // Acknowledge interrupt on exit.
    (1ull << 18) | // Save IA32_PAT.
    (1ull << 19) | // Load IA32_PAT.
    (1ull << 20) | // Save IA32_EFER.
    (1ull << 21);  // Load IA32_EFER.

/**
 * The VM-entry controls a first-level hypervisor may set.
 *
 * "Entry to SMM" and "deactivate dual-monitor treatment" are absent: SDM
 * Table 27-17 says both "must be 0 for any VM entry from outside SMM",
 * and every entry reachable here is from outside SMM.
 */
constexpr std::uint64_t supported_entry_controls =
    (1ull << 2) |  // Load debug controls.
    (1ull << 9) |  // IA-32e mode guest.
    (1ull << 14) | // Load IA32_PAT.
    (1ull << 15);  // Load IA32_EFER.

} // namespace zpp::hypervisor::nested_vmx
