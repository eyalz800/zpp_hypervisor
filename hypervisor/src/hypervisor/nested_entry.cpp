#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/memory_type.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include "zpp/hypervisor/guest_windows.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include "zpp/scope_exit.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>

namespace zpp::hypervisor
{
namespace
{
using arch::x86_64::vmx::vmcs12;
using arch::x86_64::vmx::vmcs_field_encoding;
using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
using field = arch::x86_64::vmx::vmcs::field;

/**
 * The recovery-context field the entry stubs read is CR3-target value 0.
 * asm.h spells the encoding literally, because inline assembly cannot see
 * a constant expression; this is what keeps the two spellings honest.
 */
static_assert(arch::x86_64::vmx::nested_entry_recovery_field ==
                  field::cr3_target_value_0,
              "The nested entry stubs and the VMCS field disagree.");

/**
 * The two fields the TPR shadow is carried in, checked against what a
 * shadow VMCS has storage for.
 *
 * Offering primary bit 21 is a promise a guest hypervisor can *write*
 * these, and `vmcs_field_encoding::valid` is what decides that - a field
 * whose index is past `index_capacity` is answered with error 12 and the
 * control becomes unusable. Both are 64-bit and 32-bit control fields at
 * indices 9 and 14, comfortably inside it, but the pairing is checked here
 * rather than worked out by hand again: advertising a control whose fields
 * cannot be written would be the same half-answered interface withholding
 * it was.
 * @{
 */
static_assert(vmcs_field_encoding(
                  static_cast<std::uint64_t>(field::virtual_apic_address))
                  .valid(),
              "A guest hypervisor could not write the virtual-APIC "
              "address the TPR shadow needs.");

static_assert(
    vmcs_field_encoding(static_cast<std::uint64_t>(field::tpr_threshold))
        .valid(),
    "A guest hypervisor could not write the TPR threshold.");
/**
 * @}
 */

/**
 * The pin-based controls this code names, from SDM Table 25-5.
 * @{
 */
constexpr std::uint64_t pin_external_interrupt = 1ull << 0;
constexpr std::uint64_t pin_preemption_timer = 1ull << 6;
constexpr std::uint64_t pin_posted_interrupts = 1ull << 7;
/**
 * @}
 */

/**
 * The primary processor-based controls this code names, from SDM Table
 * 25-6.
 * @{
 */
constexpr std::uint64_t primary_interrupt_window = 1ull << 2;
constexpr std::uint64_t primary_tsc_offsetting = 1ull << 3;
constexpr std::uint64_t primary_hlt_exiting = 1ull << 7;
constexpr std::uint64_t primary_invlpg_exiting = 1ull << 9;
constexpr std::uint64_t primary_mwait_exiting = 1ull << 10;
constexpr std::uint64_t primary_rdpmc_exiting = 1ull << 11;
constexpr std::uint64_t primary_rdtsc_exiting = 1ull << 12;
constexpr std::uint64_t primary_cr3_load_exiting = 1ull << 15;
constexpr std::uint64_t primary_cr3_store_exiting = 1ull << 16;
constexpr std::uint64_t primary_cr8_load_exiting = 1ull << 19;
constexpr std::uint64_t primary_cr8_store_exiting = 1ull << 20;
constexpr std::uint64_t primary_tpr_shadow = 1ull << 21;
constexpr std::uint64_t primary_nmi_window = 1ull << 22;
constexpr std::uint64_t primary_mov_dr_exiting = 1ull << 23;
constexpr std::uint64_t primary_unconditional_io = 1ull << 24;
constexpr std::uint64_t primary_io_bitmaps = 1ull << 25;
constexpr std::uint64_t primary_monitor_trap_flag = 1ull << 27;
constexpr std::uint64_t primary_msr_bitmaps = 1ull << 28;
constexpr std::uint64_t primary_monitor_exiting = 1ull << 29;
constexpr std::uint64_t primary_pause_exiting = 1ull << 30;
constexpr std::uint64_t primary_secondary_controls = 1ull << 31;
/**
 * @}
 */

/**
 * The secondary processor-based controls this code names, from SDM Table
 * 25-7.
 * @{
 */
constexpr std::uint64_t secondary_enable_ept = 1ull << 1;
constexpr std::uint64_t secondary_descriptor_table_exiting = 1ull << 2;
constexpr std::uint64_t secondary_enable_vpid = 1ull << 5;
constexpr std::uint64_t secondary_wbinvd_exiting = 1ull << 6;
constexpr std::uint64_t secondary_unrestricted_guest = 1ull << 7;
constexpr std::uint64_t secondary_pause_loop_exiting = 1ull << 10;
constexpr std::uint64_t secondary_rdrand_exiting = 1ull << 11;
constexpr std::uint64_t secondary_enable_invpcid = 1ull << 12;
constexpr std::uint64_t secondary_rdseed_exiting = 1ull << 16;
constexpr std::uint64_t secondary_enable_xsaves = 1ull << 20;
constexpr std::uint64_t secondary_tsc_scaling = 1ull << 25;

/**
 * The product of two values shifted right by the time-stamp counter's
 * scaling fraction, which SDM 27.6.5 fixes at 48 bits: "It then shifts
 * the value of the product right 48 bits and returns the sum of that
 * shifted value and the value of the TSC offset."
 *
 * A 64 by 64 product needs 128 bits before the shift or it is simply
 * wrong, and there is no standard 128-bit integer to say that in. The
 * builtin is the only spelling available and it generates no call - a
 * `mul` and a `shrd` - which matters on a path taken by every entry.
 *
 * Signed for the offset and unsigned for the multiplier, which is KVM's
 * split too: `mul_s64_u64_shr` in `kvm_calc_nested_tsc_offset` against
 * `mul_u64_u64_shr` in `kvm_calc_nested_tsc_multiplier`. The offset is a
 * two's complement quantity and shifting it as unsigned turns a guest
 * hypervisor's negative offset into an enormous positive one.
 * @{
 */
constexpr std::uint64_t tsc_scaling_fraction = 48;
constexpr std::uint64_t tsc_scaling_default = 1ull << tsc_scaling_fraction;

constexpr std::uint64_t scaled_product(std::uint64_t left,
                                       std::uint64_t right)
{
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(left) * right) >>
        tsc_scaling_fraction);
}

constexpr std::uint64_t signed_scaled_product(std::uint64_t left,
                                              std::uint64_t right)
{
    return static_cast<std::uint64_t>(
        (static_cast<__int128>(static_cast<std::int64_t>(left)) *
         static_cast<__int128>(right)) >>
        tsc_scaling_fraction);
}
/** @} */
constexpr std::uint64_t secondary_mode_based_execute = 1ull << 22;
/**
 * @}
 */

/**
 * The VM-exit controls this code names, from SDM Table 25-13.
 * @{
 */
constexpr std::uint64_t exit_save_debug_controls = 1ull << 2;
constexpr std::uint64_t exit_host_address_space_size = 1ull << 9;
constexpr std::uint64_t exit_acknowledge_interrupt = 1ull << 15;
constexpr std::uint64_t exit_save_ia32_pat = 1ull << 18;
constexpr std::uint64_t exit_load_ia32_pat = 1ull << 19;
constexpr std::uint64_t exit_save_ia32_efer = 1ull << 20;
constexpr std::uint64_t exit_load_ia32_efer = 1ull << 21;
/**
 * @}
 */

/**
 * The VM-entry controls this code names, from SDM Table 25-16.
 * @{
 */
constexpr std::uint64_t entry_ia32e_mode_guest = 1ull << 9;
/**
 * @}
 */

/**
 * CR4.VMXE, which a guest of any level must have set in the real register
 * because IA32_VMX_CR4_FIXED0 requires it in VMX operation, and must not
 * see set unless it put it there itself.
 */
constexpr std::uint64_t cr4_vmxe = 1ull << 13;
constexpr std::uint64_t cr4_smxe = 1ull << 14;

/**
 * The valid bit of an interruption-information field, SDM Table 25-19.
 */
constexpr std::uint64_t interruption_valid = 1ull << 31;

/**
 * The interruption type field of one, bits 10:8, and the value that means
 * a non-maskable interrupt. SDM Table 25-19.
 * @{
 */
constexpr std::uint64_t interruption_type_shift = 8;
constexpr std::uint64_t interruption_type_mask = 0x7;
constexpr std::uint64_t interruption_type_nmi = 2;
constexpr std::uint64_t interruption_vector_mask = 0xff;
constexpr std::uint64_t interruption_information_valid = 1ull << 31;
/**
 * @}
 */

/**
 * One entry of a VM-entry or VM-exit MSR area, SDM 27.7.2, Figure 27-1:
 * bits 31:0 the MSR index, bits 63:32 reserved and required to be zero,
 * bits 127:64 the value.
 */
struct msr_area_entry
{
    std::uint32_t index{};
    std::uint32_t reserved{};
    std::uint64_t value{};
};

static_assert(sizeof(msr_area_entry) == 16);

/**
 * How many entries an MSR area may name.
 *
 * SDM A.6 puts the limit in IA32_VMX_MISC bits 27:25, as "(N+1)*512" - so
 * zero there is 512, which is what `nested_vmx_capability_msr` narrows the
 * field to. Reported rather than assumed, so a guest hypervisor building a
 * longer list is told rather than surprised.
 */
constexpr std::uint64_t msr_area_capacity = 512;

/**
 * Whether an MSR area may name this index at all.
 *
 * The three rules SDM 29.4 and SDM 30.4 share, and KVM's
 * `nested_vmx_msr_check_common` applies:
 *
 * - bits 31:8 equal to 000008H, which is the x2APIC register range. A
 *   local APIC in x2APIC mode is reached through those, and this VMM
 *   intercepts the interrupt command register among them.
 * - the microcode update registers, IA32_BIOS_UPDT_TRIG and
 *   IA32_BIOS_SIGN_ID.
 * - the reserved dword, which must be zero.
 */
constexpr bool msr_area_index_allowed(const msr_area_entry & entry)
{
    constexpr std::uint32_t x2apic_page = 0x8;
    constexpr std::uint32_t bios_update_trigger = 0x79;
    constexpr std::uint32_t bios_signature = 0x8b;

    if (0 != entry.reserved) {
        return false;
    }

    if (x2apic_page == (entry.index >> 8)) {
        return false;
    }

    return (bios_update_trigger != entry.index) &&
           (bios_signature != entry.index);
}

/**
 * Whether this VMM will read or write the named MSR on a guest
 * hypervisor's behalf.
 *
 * **A list rather than a rule, and the direction is the safe one.** SDM
 * 29.4's last failure condition is "an attempt to write bits 127:64 to the
 * MSR indexed by bits 31:0 of the entry would cause a general-protection
 * exception if executed via WRMSR with CPL = 0", and SDM 30.4 says the
 * same of RDMSR. Answering that question in general needs a WRMSR that can
 * fault and recover, and this VMM has no such thing: the host exception
 * recovery point is a single shared context that `main` disarms before the
 * guest ever runs, so a #GP in root operation stops the processor. A guest
 * hypervisor's VM entry is a guest instruction, and no guest instruction
 * may do that.
 *
 * So an index outside this list fails the entry rather than being
 * attempted, which is an outcome the architecture already has a name for -
 * the same failure a processor reports for an MSR it will not load. A
 * guest hypervisor is told; nothing is silently skipped.
 *
 * The list is what a hypervisor actually puts in these areas: the two
 * memory-typing and paging-mode MSRs, the system-call set, the two base
 * registers a context switch needs, the speculation controls, and the time
 * stamp counter.
 *
 * Removing the restriction needs a fault-tolerant WRMSR, which needs a
 * per-processor host exception recovery point. BACKLOG.md records that as
 * the change, since it touches the exception path every processor shares.
 */
constexpr bool msr_area_index_handled(std::uint32_t index)
{
    switch (index) {
    case 0x10:       // IA32_TIME_STAMP_COUNTER.
    case 0x48:       // IA32_SPEC_CTRL.
    case 0x49:       // IA32_PRED_CMD.
    case 0x10b:      // IA32_FLUSH_CMD.
    case 0x174:      // IA32_SYSENTER_CS.
    case 0x175:      // IA32_SYSENTER_ESP.
    case 0x176:      // IA32_SYSENTER_EIP.
    case 0x1d9:      // IA32_DEBUGCTL.
    case 0x277:      // IA32_PAT.
    case 0xd90:      // IA32_BNDCFGS.
    case 0xda0:      // IA32_XSS.
    case 0xc0000080: // IA32_EFER.
    case 0xc0000081: // IA32_STAR.
    case 0xc0000082: // IA32_LSTAR.
    case 0xc0000083: // IA32_CSTAR.
    case 0xc0000084: // IA32_FMASK.
    case 0xc0000102: // IA32_KERNEL_GS_BASE.
    case 0xc0000103: // IA32_TSC_AUX.
        return true;
    default:
        return false;
    }
}

/**
 * Whether writing this value to this MSR would fault, for the two in the
 * list above whose *value* can fault rather than only their index.
 *
 * IA32_EFER is the one the SDM calls out itself, in the footnote to SDM
 * 29.4: "If CR0.PG = 1, WRMSR to the IA32_EFER MSR causes a
 * general-protection exception if it would modify the LME bit". Root
 * operation always has CR0.PG = 1 here, so a guest hypervisor asking to
 * change LME is asking for a fault. Its reserved bits fault too - SCE,
 * LME, LMA and NXE are the whole of it.
 *
 * IA32_PAT faults on a byte that is not one of the six defined memory
 * types, SDM Table 13-11: 0, 1, 4, 5, 6 and 7, with 2 and 3 reserved.
 */
inline bool msr_area_value_writable(std::uint32_t index,
                                    std::uint64_t value)
{
    constexpr std::uint32_t ia32_efer = 0xc0000080;
    constexpr std::uint32_t ia32_pat = 0x277;

    if (ia32_efer == index) {
        constexpr std::uint64_t efer_defined =
            (1ull << 0) | (1ull << 8) | (1ull << 10) | (1ull << 11);
        constexpr std::uint64_t efer_lme = 1ull << 8;

        if (0 != (value & ~efer_defined)) {
            return false;
        }

        auto current = arch::x86_64::rdmsr(
            arch::x86_64::msr::ia32_extended_feature_enable);

        return (value & efer_lme) == (current & efer_lme);
    }

    if (ia32_pat == index) {
        for (auto byte = 0; byte < 8; ++byte) {
            auto type = (value >> (byte * 8)) & 0xff;

            if ((2 == type) || (3 == type) || (type > 7)) {
                return false;
            }
        }
    }

    return true;
}

/**
 * The host-state area, which vmcs02 takes unchanged from the VMCS that
 * runs the guest hypervisor.
 *
 * It is this VMM's rather than the guest hypervisor's because the VM exit
 * a second-level guest takes comes *here*: the processor knows one host
 * and it is this one. The guest hypervisor's own host state is loaded into
 * the guest-state area of vmcs01 instead, and only when an exit is
 * actually reflected to it. KVM says the same of the exit controls in
 * `prepare_vmcs02_early`: "L2->L1 exit controls are emulated - the
 * hardware exit is to L0 so we should use its exit controls".
 *
 * IA32_PAT, IA32_EFER and IA32_PERF_GLOBAL_CTRL are absent because the
 * exit controls copied alongside do not load them, so the processor never
 * reads those fields. They are host state that does not apply.
 */
constexpr field host_state_fields[] = {
    field::host_es_selector,
    field::host_cs_selector,
    field::host_ss_selector,
    field::host_ds_selector,
    field::host_fs_selector,
    field::host_gs_selector,
    field::host_tr_selector,
    field::host_ia32_sysenter_cs,
    field::host_cr0,
    field::host_cr3,
    field::host_cr4,
    field::host_fs_base,
    field::host_gs_base,
    field::host_tr_base,
    field::host_gdtr_base,
    field::host_idtr_base,
    field::host_ia32_sysenter_esp,
    field::host_ia32_sysenter_eip,
    field::host_rsp,
    field::host_rip,
};

/**
 * The guest-state fields carried in both directions between the guest
 * hypervisor's VMCS and the one that runs its guest.
 *
 * One list rather than two, because the two directions must agree: a field
 * loaded on the way in and not saved on the way out is a field the
 * second-level guest's changes to are silently discarded, which is exactly
 * the class of bug that shows up somewhere unrelated. SDM 27.4 defines the
 * guest-state area and SDM 30.3 defines what a VM exit saves back into it;
 * everything unconditional in the second list is here.
 *
 * What is deliberately absent, and handled by name instead: CR0, CR4 and
 * their read shadows, because the masks make them a computation rather
 * than a copy; RIP, RSP and RFLAGS, which are saved back but loaded from
 * the guest hypervisor's own values; DR7, IA32_PAT and IA32_EFER, whose
 * save-back is conditional on the guest hypervisor's exit controls; the
 * interruptibility state, which the entry-event path also writes; and the
 * activity state, which is not a copy in either direction because vmcs02
 * is not entered in every state vmcs12 may name - see `enter_or_park_l2`.
 */
constexpr field guest_state_fields[] = {
    field::guest_es_selector,
    field::guest_cs_selector,
    field::guest_ss_selector,
    field::guest_ds_selector,
    field::guest_fs_selector,
    field::guest_gs_selector,
    field::guest_ldtr_selector,
    field::guest_tr_selector,
    field::guest_es_limit,
    field::guest_cs_limit,
    field::guest_ss_limit,
    field::guest_ds_limit,
    field::guest_fs_limit,
    field::guest_gs_limit,
    field::guest_ldtr_limit,
    field::guest_tr_limit,
    field::guest_gdtr_limit,
    field::guest_idtr_limit,
    field::guest_es_access_rights,
    field::guest_cs_access_rights,
    field::guest_ss_access_rights,
    field::guest_ds_access_rights,
    field::guest_fs_access_rights,
    field::guest_gs_access_rights,
    field::guest_ldtr_access_rights,
    field::guest_tr_access_rights,
    field::guest_es_base,
    field::guest_cs_base,
    field::guest_ss_base,
    field::guest_ds_base,
    field::guest_fs_base,
    field::guest_gs_base,
    field::guest_ldtr_base,
    field::guest_tr_base,
    field::guest_gdtr_base,
    field::guest_idtr_base,
    field::guest_cr3,
    field::guest_pending_debug_exceptions,
    field::guest_ia32_sysenter_esp,
    field::guest_ia32_sysenter_eip,
    field::guest_ia32_sysenter_cs,
    field::guest_ia32_debugctl,
    field::guest_pdpte_0,
    field::guest_pdpte_1,
    field::guest_pdpte_2,
    field::guest_pdpte_3,
};

/**
 * vmcs02's control fields, which `write_vmcs02_control` may elide.
 *
 * Two properties are required of every field here, and the second is
 * the one that is easy to lose.
 *
 * The processor never saves over a VM-execution, VM-exit or VM-entry
 * control, so what was last written is still there - unlike the guest
 * state fields next door, which it overwrites on every exit.
 *
 * And nothing writes them but `build_vmcs02`, which runs with vmcs02
 * current by construction. That is why the pin-based and primary
 * controls are **not** here even though they are controls: `resume.cpp`
 * and `local_apic.cpp` write them too, and the CR-access handler in
 * `exit_dispatch.cpp` writes the CR0 and CR4 read shadows, so for those
 * four the cache would describe a field somebody else had moved. The
 * primary controls are also the one entry in the list that genuinely
 * changes almost every entry - the interrupt window is armed and
 * disarmed constantly - so caching them was worth very little anyway.
 * Everything remaining is written on vmcs01 only during one-time
 * initialisation, or in `set_vmcs_shadowing`, which runs while the
 * guest hypervisor's own instruction is being handled and vmcs01 is
 * current.
 *
 * See `control_cache` for the three fields that look like controls and
 * are not - the entry interruption-information field, the preemption
 * timer value, and the two that accompany an injection.
 *
 * `tests/nested_exit` is what enforces the second property: it pokes a
 * field directly and rebuilds, which is exactly the shape of a caller
 * that has moved vmcs02 out from under the cache.
 */
constexpr field control_fields[] = {
    field::secondary_processor_based_vm_execution_controls,
    field::vm_exit_controls,
    field::vm_entry_controls,
    field::exception_bitmap,
    field::page_fault_error_code_mask,
    field::page_fault_error_code_match,
    field::cr0_guest_host_mask,
    field::cr4_guest_host_mask,
    field::ept_pointer,
    field::vpid,
    field::vmcs_link_pointer,
    field::msr_bitmap,
    field::io_bitmap_a,
    field::io_bitmap_b,
    field::virtual_apic_address,
    field::tpr_threshold,
    field::cr3_target_value_0,
    field::cr3_target_count,
    field::tsc_offset,
    field::tsc_multiplier,
    field::vm_entry_msr_load_count,
    field::vm_exit_msr_load_count,
    field::vm_exit_msr_store_count,
};

/**
 * The effective CR0 and CR4 a second-level guest reads, which is what its
 * read shadow under this VMM has to answer with.
 *
 * A guest reads `(shadow & mask) | (register & ~mask)`. Under the guest
 * hypervisor alone that is vmcs12's own three fields; under this VMM the
 * mask is wider - it includes the bits this VMM owns - so the shadow has
 * to carry the whole answer rather than half of it. Writing the effective
 * value into the shadow does that for every bit the wider mask covers,
 * and the bits it does not cover are outside vmcs12's mask too, where the
 * real register already agrees. KVM computes the same value in
 * `nested_read_cr0` and `nested_read_cr4`.
 * @{
 */
constexpr std::uint64_t effective_control_register(std::uint64_t value,
                                                   std::uint64_t shadow,
                                                   std::uint64_t mask)
{
    return (value & ~mask) | (shadow & mask);
}
/**
 * @}
 */

} // namespace

std::expected<void, zpp::error> hypervisor::check_nested_msr_area(
    std::uint64_t address, std::uint64_t count, bool loading)
{
    if (0 == count) {
        return {};
    }

    // SDM 29.2.1.1 checks the address itself as a control: 16-byte aligned
    // and the last byte within the processor's physical-address width.
    // That part is the architecture's and applies whether or not the
    // contents are looked at.
    auto bytes = count * sizeof(msr_area_entry);
    auto limit = 1ull << physical_address_bits();

    if ((0 != (address & 0xf)) || (count > msr_area_capacity) ||
        (address >= limit) || (bytes > (limit - address))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // The contents, which is where this diverges from a processor and does
    // so deliberately. A processor checks each entry as it processes it -
    // at VM entry for the entry area, and at VM *exit* for the two exit
    // areas, where a failure is a VMX abort and a VMX abort is a shutdown.
    // There is no shutdown available here that does not take the whole
    // machine with it, so all three are checked up front instead, and a
    // guest hypervisor is refused before its guest runs rather than after.
    //
    // What that costs is precision about which failure it was; what it
    // buys is that the exit path cannot fail on anything but memory that
    // stopped being readable, which is the one case left for the abort
    // below.
    for (std::uint64_t i{}; i < count; ++i) {
        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index) ||
            (loading &&
             !msr_area_value_writable(entry.index, entry.value))) {
            // Which MSR, because the caller only gets an error code.
            //
            // The list this checks against is closed, the areas are guest
            // memory a guest hypervisor rewrites with no exit, and the
            // check is stateless and runs on every entry - so the first
            // index it does not know refuses that virtual processor's
            // every subsequent entry, permanently and silently. Without
            // this line the refusal names no MSR and the next boot
            // answers nothing; with it, the fix is to add whatever it
            // names to msr_area_index_handled.
            log("msr area at {} ({}) refuses index {}, value {}",
                address,
                loading ? "load" : "store",
                entry.index,
                entry.value);

            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }
    }

    return {};
}

std::expected<void, zpp::error> hypervisor::load_nested_msrs(
    std::size_t cpu, std::uint64_t address, std::uint64_t count)
{
    for (std::uint64_t i{}; i < count; ++i) {
        // SDM 29.8: the exit qualification of an MSR-loading failure is
        // "the number of the entry that caused the problem (1 for the
        // first entry, 2 for the second, etc.)".
        this->nested_msr_failure_entry[cpu] = i + 1;

        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        // Re-checked rather than trusted, because the area is guest memory
        // and nothing stops a guest hypervisor rewriting it between the
        // check and here. The check exists to give a good answer early;
        // this exists so that a bad one cannot reach WRMSR.
        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index) ||
            !msr_area_value_writable(entry.index, entry.value)) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        arch::x86_64::wrmsr(entry.index, entry.value);
    }

    return {};
}

std::expected<void, zpp::error>
hypervisor::store_nested_msrs(std::uint64_t address, std::uint64_t count)
{
    for (std::uint64_t i{}; i < count; ++i) {
        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index)) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        entry.value = arch::x86_64::rdmsr(entry.index);

        auto written = write_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<const std::byte *>(&entry),
                      sizeof(entry)));
        if (!written) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }
    }

    return {};
}

bool hypervisor::own_msr_intercepted(std::uint32_t index, bool write) const
{
    // The same four 1024-byte bitmaps intercept_msr writes, read back.
    // SDM 27.6.9, "MSR-Bitmap Address".
    std::size_t base{};
    std::uint32_t bit{};

    if (index < 0x2000) {
        base = write ? 0x800 : 0x000;
        bit = index;
    } else if ((index >= 0xc0000000) && (index < 0xc0002000)) {
        base = write ? 0xc00 : 0x400;
        bit = index - 0xc0000000;
    } else {
        // Outside both ranges no bitmap is consulted and the access exits
        // unconditionally (SDM 28.1.3), so this VMM's bitmap did not ask
        // for it and the answer here is no.
        //
        // That matters more than it looks. This VMM answers an
        // out-of-range MSR with a general protection fault, which is what
        // bare hardware gives - but for a *second-level* guest the machine
        // is the guest hypervisor, and the synthetic MSR ranges a
        // hypervisor presents to its guest live exactly there: Hyper-V's
        // are at 40000000H upwards. Claiming those would have this VMM
        // fault an interface the guest hypervisor implements.
        return false;
    }

    return 0 != (this->msr_bitmap[base + (bit / 8)] & (1u << (bit % 8)));
}

bool hypervisor::own_io_port_intercepted(std::uint16_t port) const
{
    // Bitmap A covers ports 0000H-7FFFH and bitmap B 8000H-FFFFH. SDM
    // 27.6.4, "I/O-Bitmap Addresses".
    const auto & bitmap =
        (port < 0x8000) ? this->io_bitmap_a : this->io_bitmap_b;
    auto bit = static_cast<std::size_t>(port & 0x7fff);

    return 0 != (bitmap[bit / 8] & (1u << (bit % 8)));
}

std::expected<void, zpp::error>
hypervisor::merge_nested_bitmaps(std::size_t cpu)
{
    // Phase timing; see `phase_cycles`. Timed because `build_vmcs02`
    // costs 220,653 cycles a call while issuing fifteen VMCS writes and
    // one 5,127 cycle VMPTRLD, and this is the only other thing in it
    // big enough to hold the rest.
    auto merge_start = arch::x86_64::rdtsc();
    auto merge_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][8] +=
                arch::x86_64::rdtsc() - merge_start;
            this->phase_calls[cpu][8] += 1;
        }
    });

    auto & shadow = this->guest_vmcs12[cpu];

    auto msr_source = shadow.read(field::msr_bitmap);
    auto io_a_source = shadow.read(field::io_bitmap_a);
    auto io_b_source = shadow.read(field::io_bitmap_b);

    // Every VM entry, with no cache on the bitmap addresses.
    //
    // A cache on the addresses was written first and is wrong, which is
    // worth recording because it looks obviously right: the addresses are
    // VMCS fields and a guest hypervisor changes them rarely, so caching
    // on them seems free. But the *contents* live in guest memory the
    // guest hypervisor writes directly, with no VMWRITE and no exit - a
    // hypervisor that stops intercepting an MSR clears a bit in the page
    // it already named. Cached on the address, this VMM would have gone on
    // trapping what the guest hypervisor stopped asking for and, worse,
    // gone on *not* trapping what it started asking for. KVM re-merges on
    // every entry too, in `nested_vmx_prepare_msr_bitmap`.
    //
    // What it costs is one guest page read per area the guest hypervisor
    // actually uses, per entry - four kilobytes for a hypervisor that uses
    // MSR bitmaps and no I/O bitmaps, which is the usual shape. Caching it
    // correctly needs a way to notice a write to those pages, and this VMM
    // has one: `watch_guest_page_writes`. That is the optimisation, and
    // the measurement that would justify it is the entry rate of a real
    // guest hypervisor, which nothing has yet run.

    // The guest hypervisor's page is read straight into the destination
    // and this VMM's own is then or'd on top, rather than the other way
    // round with a scratch page in between. There is nowhere to put a
    // scratch page: the host stack a VM exit runs on is under four
    // kilobytes, so a page-sized local would run off the end of it, and a
    // per-processor scratch member would cost what it saves.
    //
    // Reading into the destination means a failed read leaves a
    // half-merged bitmap behind. Harmless, because a failure refuses the
    // VM entry and the next attempt merges again from scratch - so the
    // half-merged state is never entered with.
    //
    // The merge itself is the union. A bit set on either side is an exit,
    // which is what makes it safe: every exit that happens is one of the
    // two asked for, and whichever asked for it knows how to answer it.
    auto merge_page =
        [&](std::uint64_t from,
            const void * ours,
            std::uint8_t * into,
            bool read_theirs,
            std::size_t which) -> std::expected<void, zpp::error> {
        // The union with nothing is this VMM's own page, unchanged.
        //
        // Worth the flag rather than the memset-and-or it replaces,
        // because it is the *usual* case and it was being recomputed
        // from scratch on every entry: a guest hypervisor that uses MSR
        // bitmaps and no I/O bitmaps - which is Hyper-V's shape, and
        // the shape the comment above predicted - took two of these
        // three pages down this branch every time, so two thirds of the
        // work produced a value that could not have changed.
        //
        // What invalidates it is a write to *this VMM's* own bitmap,
        // which happens outside this function: `forget_nested_bitmaps`
        // exists for that and `intercept_interrupt_command` calls it.
        if (!read_theirs) {
            if (this->nested_bitmap_is_ours[cpu][which]) {
                return {};
            }

            std::memcpy(into, ours, page_size);
            this->nested_bitmap_is_ours[cpu][which] = true;
            return {};
        }

        this->nested_bitmap_is_ours[cpu][which] = false;

        // Phase 10 is the guest page read alone, so it can be told apart
        // from the union that follows it. The proposed fix - keeping the
        // mapping across entries - only removes `map_window_at`, and the
        // copy through it is cold-cache traffic that the fix would not
        // touch. Sizing the change off the merge total would repeat a
        // mistake made twice already in this file.
        auto read_start = arch::x86_64::rdtsc();
        auto read = read_guest_physical(
            from,
            std::span(reinterpret_cast<std::byte *>(into), page_size));
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][10] +=
                arch::x86_64::rdtsc() - read_start;
            this->phase_calls[cpu][10] += 1;
        }

        if (!read) {
            return std::unexpected(read.error());
        }

        // Quadwords rather than bytes: the same union in an eighth of
        // the iterations. Both sides are whole VMCS-referenced pages and
        // so are page aligned by construction, which is what makes the
        // wider access well defined here.
        auto mine = static_cast<const std::uint64_t *>(ours);
        auto target = reinterpret_cast<std::uint64_t *>(into);

        for (std::size_t i{}; i < page_size / sizeof(std::uint64_t); ++i) {
            target[i] |= mine[i];
        }

        return {};
    };

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);

    auto their_msr_bitmap = 0 != (primary12 & primary_msr_bitmaps);
    auto their_io_bitmaps = 0 != (primary12 & primary_io_bitmaps);

    if (auto merged = merge_page(msr_source,
                                 this->msr_bitmap,
                                 this->nested_msr_bitmap[cpu],
                                 their_msr_bitmap,
                                 0);
        !merged) {
        return merged;
    }

    if (auto merged = merge_page(io_a_source,
                                 this->io_bitmap_a,
                                 this->nested_io_bitmap[cpu],
                                 their_io_bitmaps,
                                 1);
        !merged) {
        return merged;
    }

    if (auto merged = merge_page(io_b_source,
                                 this->io_bitmap_b,
                                 this->nested_io_bitmap[cpu] + page_size,
                                 their_io_bitmaps,
                                 2);
        !merged) {
        return merged;
    }

    // Once, not per entry. These name fixed per-processor buffers, so
    // the answer cannot change, and asking for it walks the host page
    // table - which is the same kind of work this function was already
    // doing three times over for no reason.
    if (0 == this->nested_msr_bitmap_physical[cpu]) {
        this->nested_msr_bitmap_physical[cpu] =
            this->host_page_table.virtual_to_physical(
                this->nested_msr_bitmap[cpu]);
        this->nested_io_bitmap_physical[cpu] =
            this->host_page_table.virtual_to_physical(
                this->nested_io_bitmap[cpu]);
    }

    return {};
}

void hypervisor::forget_vmcs02_contents(std::size_t cpu)
{
    if (cpu >= max_cpus) {
        return;
    }

    this->vmcs02_host_written[cpu] = false;

    for (auto & valid : this->control_cache_valid[cpu]) {
        valid = false;
    }
}

void hypervisor::write_vmcs02_control(std::size_t cpu,
                                      field control,
                                      std::uint64_t value)
{
    static_assert(std::size(control_fields) <= control_cache_capacity,
                  "control_cache is too small for the field list");

    if (cpu >= max_cpus) {
        this->vmcs.write(control, value);
        return;
    }

    // A linear scan over twenty-seven constants, against a VMWRITE that
    // costs about 4,500 cycles here because this VMM is itself KVM's
    // guest and the instruction traps. The comparison is free by
    // comparison, so there is nothing to gain from a smarter index and a
    // table to keep in step with the list.
    for (std::size_t i{}; i < std::size(control_fields); ++i) {
        if (control_fields[i] != control) {
            continue;
        }

        if (this->control_cache_valid[cpu][i] &&
            (this->control_cache[cpu][i] == value)) {
            this->control_writes_skipped[cpu] += 1;
            return;
        }

        this->vmcs.write(control, value);
        this->control_cache[cpu][i] = value;
        this->control_cache_valid[cpu][i] = true;
        this->control_writes_done[cpu] += 1;
        return;
    }

    // Not on the list, so not argued for. Written straight through
    // rather than added to the cache by accident.
    this->vmcs.write(control, value);
}

std::expected<void, zpp::error> hypervisor::build_vmcs02(std::size_t cpu)
{
    // Phase timing; see `phase_cycles`.
    auto phase_start = arch::x86_64::rdtsc();
    auto phase_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][2] +=
                arch::x86_64::rdtsc() - phase_start;
            this->phase_calls[cpu][2] += 1;
        }
    });

    namespace vmx_msr = arch::x86_64::vmx::msr;

    if constexpr (!nested_vmx::enabled) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    auto pin12 = shadow.read(field::pin_based_vm_execution_controls);
    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};
    auto exit12 = shadow.read(field::vm_exit_controls);
    auto entry12 = shadow.read(field::vm_entry_controls);

    // What the guest hypervisor actually asked for, recorded once.
    //
    // Only the first entry's values are kept: a hypervisor that runs
    // many second-level guests would otherwise leave the last one's
    // controls here, and the question this answers is what it needs at
    // all, which the first launch already settles.
    if (0 == this->vmcs12_controls_captured) {
        this->vmcs12_pin_controls = pin12;
        this->vmcs12_primary_controls = primary12;
        this->vmcs12_secondary_controls = shadow.read(
            field::secondary_processor_based_vm_execution_controls);
        this->vmcs12_exit_controls = exit12;
        this->vmcs12_entry_controls = entry12;
        this->vmcs12_controls_captured = 1;
    }

    // Every control the guest hypervisor set has to be one the capability
    // MSRs told it it could set, and every control they said must be 1 has
    // to be 1. SDM 29.2.1.1 makes that the first check on the VM-execution
    // controls, and adjust_msr applied to the narrowed MSR is exactly it:
    // a value that already satisfies both halves is its own fixed point.
    //
    // Checked against the *narrowed* MSRs rather than the hardware's,
    // which is the point of narrowing them. Without this a guest
    // hypervisor could set a control this VMM never offered - virtualized
    // APIC accesses, say - and the union below would hand it to the
    // processor with nothing here maintaining the state it needs.
    auto within_capability = [&](std::size_t msr, std::uint64_t value) {
        return value == arch::x86_64::vmx::adjust_msr(
                            nested_vmx_capability_msr(msr), value);
    };

    if (!within_capability(vmx_msr::true_pin_based_controls, pin12) ||
        !within_capability(vmx_msr::true_processor_based_controls,
                           primary12) ||
        !within_capability(vmx_msr::true_exit_controls, exit12) ||
        !within_capability(vmx_msr::true_entry_controls, entry12) ||
        ((0 != (primary12 & primary_secondary_controls)) &&
         !within_capability(vmx_msr::processor_based_contorls_2,
                            secondary12))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // The two consistency rules SDM 29.2.1.1 states about NMIs, which are
    // the only control combinations reachable here that a union of the two
    // sides cannot repair. Both are also KVM's
    // `nested_vmx_check_nmi_controls`.
    constexpr std::uint64_t pin_nmi_exiting = 1ull << 3;
    constexpr std::uint64_t pin_virtual_nmis = 1ull << 5;

    if (((0 == (pin12 & pin_nmi_exiting)) &&
         (0 != (pin12 & pin_virtual_nmis))) ||
        ((0 == (pin12 & pin_virtual_nmis)) &&
         (0 != (primary12 & primary_nmi_window)))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // All three MSR areas are checked here, up front, rather than each
    // where a processor would look at it. See check_nested_msr_area for
    // why: a failure in either exit area is a VMX abort, and a VMX abort
    // is a shutdown there is no way to perform on one guest's behalf.
    auto entry_load_address =
        shadow.read(field::vm_entry_msr_load_address);
    auto entry_load_count = shadow.read(field::vm_entry_msr_load_count);
    auto exit_load_address = shadow.read(field::vm_exit_msr_load_address);
    auto exit_load_count = shadow.read(field::vm_exit_msr_load_count);
    auto exit_store_address =
        shadow.read(field::vm_exit_msr_store_address);
    auto exit_store_count = shadow.read(field::vm_exit_msr_store_count);

    if (auto checked = check_nested_msr_area(
            entry_load_address, entry_load_count, true);
        !checked) {
        return checked;
    }

    if (auto checked = check_nested_msr_area(
            exit_load_address, exit_load_count, true);
        !checked) {
        return checked;
    }

    if (auto checked = check_nested_msr_area(
            exit_store_address, exit_store_count, false);
        !checked) {
        return checked;
    }

    // A 64-bit host is the only shape this can put back, since the exit
    // comes here and the guest hypervisor is resumed in whatever mode its
    // own host-state area describes. SDM 29.2.2 makes this an error 8
    // condition on its own terms - the host-state checks are where the
    // address-space size and the CS selector are validated together.
    if (0 == (exit12 & exit_host_address_space_size)) {
        return std::unexpected(
            zpp::error{error::nested_host_state_unsupported});
    }

    // The second level of address translation, which is either a shadow
    // composed out of the guest hypervisor's tables or - when it uses
    // none - this VMM's own.
    //
    // The second case is not a shortcut. Without extended page tables of
    // its own a guest hypervisor shadow-pages instead, so its guest's
    // physical addresses *are* its own, and its own are what this VMM's
    // identity map already translates with the module and every watched
    // page removed. So the protections are still in force, which is the
    // whole reason BACKLOG.md rejects handing the guest hypervisor's EPT
    // pointer straight to the processor.
    std::uint64_t eptp02{};

    if (0 != (secondary12 & secondary_enable_ept)) {
        auto eptp12 = shadow.read(field::ept_pointer);

        // The checks SDM 29.2.1.1 puts on the pointer, in the same order
        // as KVM's `nested_vmx_check_eptp`: a memory type this VMM
        // reports, a page-walk length it reports, the accessed-and-dirty
        // bit only where that capability is reported, and no bit set
        // above what the processor can address. Bits 5:3 hold the walk
        // length minus one, bit 6 enables accessed and dirty flags, and
        // bits 11:7 are reserved.
        constexpr std::uint64_t eptp_memory_type_mask = 0x7;
        constexpr std::uint64_t eptp_walk_length_mask = 0x38;
        constexpr std::uint64_t eptp_walk_length_4 = 3ull << 3;
        constexpr std::uint64_t eptp_access_and_dirty = 0x40;
        constexpr std::uint64_t eptp_reserved = 0xf80;

        // IA32_VMX_EPT_VPID_CAP bit 21, from SDM A.10. The shadow never
        // sets accessed or dirty in an entry it writes, so a guest
        // hypervisor asking for them here would poll its own tables and
        // find nothing ever touched. The capability MSR withholds the
        // bit; this is the other half of withholding it, and without it
        // the pointer is accepted and the promise quietly broken. KVM
        // pairs the two the same way, in `nested_vmx_check_eptp`'s "AD,
        // if set, should be supported".
        constexpr std::uint64_t ept_cap_access_and_dirty = 1ull << 21;

        // The other three capability bits, from the same appendix, and
        // they were the asymmetry left beside the one above: the memory
        // type and the walk length were checked against constants while
        // accessed-and-dirty was checked against the capability.
        //
        // SDM 29.2.1.1 (.references/sdm.txt:202156): "The EPT memory type
        // (bits 2:0) must be a value supported by the processor as
        // indicated in the IA32_VMX_EPT_VPID_CAP MSR", and the line below
        // says the same of the walk length. Appendix A.10 gives the bits:
        // 8 for uncacheable (.references/sdm.txt:223503), 14 for
        // write-back (:223505), 6 for a page-walk length of 4 (:223501).
        //
        // It is a promise broken in the direction that matters. What this
        // VMM reports is `hardware & supported_ept_vpid_capabilities`, so
        // on a processor that does not report uncacheable paging
        // structures it told a guest hypervisor exactly that and then
        // accepted a pointer asking for them - the capability MSR and the
        // check disagreeing about the same machine.
        //
        // KVM tests the reported bits: VMX_EPTP_UC_BIT, VMX_EPTP_WB_BIT
        // and VMX_EPT_PAGE_WALK_4_BIT in `nested_vmx_check_eptp`
        // (.references/kvm/nested.c:2794, v6.12).
        constexpr std::uint64_t ept_cap_walk_length_4 = 1ull << 6;
        constexpr std::uint64_t ept_cap_uncachable = 1ull << 8;
        constexpr std::uint64_t ept_cap_write_back = 1ull << 14;

        auto capability =
            nested_vmx_capability_msr(vmx_msr::vpid_ept_capability);

        auto memory_type = eptp12 & eptp_memory_type_mask;
        auto is_uncachable =
            (memory_type == static_cast<std::uint64_t>(
                                arch::x86_64::memory_type::uncachable)) &&
            (0 != (capability & ept_cap_uncachable));
        auto is_write_back =
            (memory_type == static_cast<std::uint64_t>(
                                arch::x86_64::memory_type::write_back)) &&
            (0 != (capability & ept_cap_write_back));
        auto walk_length_supported =
            0 != (capability & ept_cap_walk_length_4);

        auto address_mask =
            ((1ull << physical_address_bits()) - 1) & ~0xfffull;

        auto access_and_dirty_offered =
            0 != (capability & ept_cap_access_and_dirty);

        if ((!is_uncachable && !is_write_back) || !walk_length_supported ||
            (eptp_walk_length_4 != (eptp12 & eptp_walk_length_mask)) ||
            (!access_and_dirty_offered &&
             (0 != (eptp12 & eptp_access_and_dirty))) ||
            (0 != (eptp12 & eptp_reserved)) ||
            (0 != (eptp12 & ~(address_mask | 0xfffull)))) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }

        auto shadow_pointer = shadow_ept_pointer_for(cpu, eptp12);
        if (!shadow_pointer) {
            return std::unexpected(shadow_pointer.error());
        }

        eptp02 = *shadow_pointer;
    } else {
        arch::x86_64::vmx::ept_pointer pointer;
        pointer.memory_type(arch::x86_64::memory_type::write_back);
        pointer.page_walk_length(4);
        pointer.page_number(this->epml4_physical >> 12);
        eptp02 = pointer;
    }

    // The TPR shadow, which a real guest hypervisor was measured setting -
    // primary 0xa4206dfa, bit 21 - and which withholding cost seventeen
    // second-level entries and then a boot loop. nested_vmx.h carries the
    // whole capture. Three outcomes here: honoured, replaced, or refused.
    auto tpr_shadow12 = 0 != (primary12 & primary_tpr_shadow);
    auto honour_tpr_shadow = false;
    std::uint64_t virtual_apic12{};
    std::uint64_t tpr_threshold12{};

    if (tpr_shadow12) {
        virtual_apic12 = shadow.read(field::virtual_apic_address);
        tpr_threshold12 = shadow.read(field::tpr_threshold);

        // SDM 29.2.1.1 puts one check on the threshold that always
        // applies here: "If the 'use TPR shadow' VM-execution control is 1
        // and the 'virtual-interrupt delivery' VM-execution control is 0,
        // bits 31:4 of the TPR threshold VM-execution control field must
        // be 0." Virtual-interrupt delivery is not offered, so the second
        // half of that condition is always true.
        //
        // The same paragraph has a second rule about the threshold - that
        // its bits 3:0 "should not be greater than the value of bits 7:4
        // of VTPR" - which is deliberately not checked. It says *should*
        // rather than *must*, it would cost a guest page read on every
        // entry, and KVM does not check it either.
        if (0 != (tpr_threshold12 & ~0xfull)) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }

        // The address, which is the part that has to be got right. SDM
        // 29.2.1.1 lists the virtual-APIC address among the fields whose
        // "bits 11:0 must be 0, and the fields are also subject to the
        // checks on physical-address width described above in Section
        // 28.2.1". KVM checks exactly those two, in
        // `nested_vmx_check_tpr_shadow_controls` through
        // `page_address_valid`.
        //
        // Two checks are added to them, and both are this VMM's rather
        // than the architecture's.
        //
        // Zero is refused. A processor would accept it - page zero is 4 KB
        // aligned and within any width - but a guest hypervisor that set
        // the control and never wrote the field leaves exactly zero
        // behind, since a shadow VMCS starts zeroed, and page zero holds
        // the real-mode interrupt vector table. Accepting it would have
        // the processor write VTPR over the guest's own IVT.
        //
        // Then the one that matters. **The processor reads and writes this
        // page in root operation, where extended page tables do not
        // apply** - so none of this VMM's protections cover those
        // accesses. The module's own pages, the log queue storage and
        // every watched page are hidden from the guest by extended
        // page-table permissions and by nothing else, so a guest
        // hypervisor naming one of them here would have the processor
        // write into it on its behalf. That is the same hazard the MSR
        // areas carry, and the reason their addresses are never handed to
        // the processor at all; this one has to be, because the whole
        // point of the control is that the processor uses the page.
        //
        // host_ept_lookup answers it directly and without allocating: it
        // is this VMM's own translation for the address with the
        // permissions accumulated down the walk. The test is therefore the
        // right one rather than a list that can drift - may the guest
        // itself read and write this page? If it may not, the processor
        // may not either. The extended page tables are an identity map of
        // the first 512 GB (see initialize_ept), so the L1-physical
        // address vmcs12 names is also the host-physical address vmcs02
        // gets, and a lookup that returns anything but `mapped` is already
        // an address outside that map.
        auto address_limit = 1ull << physical_address_bits();
        auto ours = host_ept_lookup(virtual_apic12);

        auto usable =
            (0 != virtual_apic12) && (0 == (virtual_apic12 & 0xfff)) &&
            (virtual_apic12 < address_limit) &&
            (arch::x86_64::vmx::ept_walk_status::mapped == ours.status) &&
            ours.permissions.read() && ours.permissions.write();

        // A page this VMM will not let the processor touch does not have
        // to refuse the entry, and refusing it is the worse of the two
        // answers where the guest hypervisor also asked for CR8-load and
        // CR8-store exiting. With both of those set the processor never
        // consults the virtual-APIC page at all - SDM 27.6.8 makes MOV CR8
        // the only operation that reads it here, since the other two need
        // controls this VMM withholds, and CR8 exiting means MOV CR8
        // never completes - so the control can be dropped with nothing
        // lost, and the exits it would have replaced go to the guest
        // hypervisor, which asked for them. That is KVM's fallback in
        // `nested_get_vmcs12_pages`, under the comment "the processor will
        // never use the TPR shadow, simply clear the bit from the
        // execution control"; the same function calls failing the entry
        // for any other configuration "_not_ what the processor does but
        // it's basically the only possibility we have", which is the case
        // below.
        //
        // Both controls, not either: a guest hypervisor that intercepts
        // the load and not the store still expects `mov rax, cr8` to read
        // VTPR out of the page.
        constexpr std::uint64_t cr8_exiting =
            primary_cr8_load_exiting | primary_cr8_store_exiting;

        // Switched off deliberately, which is not the same as refusing a
        // page. `nested_vmx::tpr_shadow_offered` is the one variable
        // between two boots, and the branch below forces CR8 exiting and
        // answers those exits here - so the entry must not be failed for
        // want of a control the guest hypervisor was never going to set.
        if (!nested_vmx::tpr_shadow_offered) {
            honour_tpr_shadow = false;
        } else if (usable) {
            honour_tpr_shadow = true;
        } else if (cr8_exiting != (primary12 & cr8_exiting)) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }
    }

    if (auto merged = merge_nested_bitmaps(cpu); !merged) {
        return merged;
    }

    // Everything that is this VMM's, read out of the VMCS that runs the
    // guest hypervisor before that one stops being current - and read
    // **once**, not on every entry. See `host_state_cache`: this is
    // twenty-eight VMCS reads of state that is fixed for the life of the
    // processor, and a VMREAD costs 1.76 microseconds here.
    static_assert(std::size(host_state_fields) <= 24,
                  "host_state_cache is too small for the field list");

    if (!this->host_state_cached[cpu]) {
        for (std::size_t i{}; i < std::size(host_state_fields); ++i) {
            this->host_state_cache[cpu][i] =
                vmcs.read(host_state_fields[i]);
        }

        this->host_controls_cache[cpu][0] =
            vmcs.pin_based_vm_execution_controls();
        this->host_controls_cache[cpu][1] =
            vmcs.primary_processor_based_vm_execution_controls();
        this->host_controls_cache[cpu][2] =
            vmcs.secondary_processor_based_vm_execution_controls();
        this->host_controls_cache[cpu][3] = vmcs.vm_exit_controls();
        this->host_controls_cache[cpu][4] =
            vmcs.read(field::exception_bitmap);
        this->host_controls_cache[cpu][5] =
            vmcs.read(field::cr0_guest_host_mask);
        this->host_controls_cache[cpu][6] =
            vmcs.read(field::cr4_guest_host_mask);
        this->host_controls_cache[cpu][7] = vmcs.vpid();
        this->host_state_cached[cpu] = true;
    }

    auto * host_values = this->host_state_cache[cpu];
    auto pin01 = this->host_controls_cache[cpu][0];
    auto primary01 = this->host_controls_cache[cpu][1];
    auto secondary01 = this->host_controls_cache[cpu][2];
    auto exit01 = this->host_controls_cache[cpu][3];
    auto exception_bitmap01 = this->host_controls_cache[cpu][4];
    auto cr0_mask01 = this->host_controls_cache[cpu][5];
    auto cr4_mask01 = this->host_controls_cache[cpu][6];
    auto vpid01 = this->host_controls_cache[cpu][7];

    // From here nothing may fail: vmcs02 is about to become current, and a
    // caller that answered VMfail with it current would resume the guest
    // hypervisor on the wrong VMCS.
    // Phase timing; see `phase_cycles`. Timed on its own because the
    // rest of this function is now nearly free and the phase is not.
    auto switch_start = arch::x86_64::rdtsc();
    auto switch_failed =
        arch::x86_64::vmx::vmptrld(&this->vmcs02_physical[cpu]);

    if (cpu < max_cpus) {
        this->phase_cycles[cpu][6] += arch::x86_64::rdtsc() - switch_start;
        this->phase_calls[cpu][6] += 1;
    }

    if (switch_failed) {
        return std::unexpected(zpp::error{error::vmptrld_failed});
    }

    // Once per vmcs02, not once per entry. See `vmcs02_host_written`:
    // the region is cleared where it is created and never again, so what
    // was written the first time is still there.
    if (!this->vmcs02_host_written[cpu]) {
        for (std::size_t i{}; i < std::size(host_state_fields); ++i) {
            vmcs.write(host_state_fields[i], host_values[i]);
        }
        this->vmcs02_host_written[cpu] = true;
    }

    // Pin-based controls: the union, less the preemption timer, which is
    // this VMM's alone. The capability MSRs do not offer it to a guest
    // hypervisor, and this VMM arms it to drive its own log - so a guest
    // hypervisor's copy of the bit means nothing and its exits are not
    // reflected.
    // External-interrupt exiting is masked out for the same reason the
    // preemption timer is: with `ZPP_VIRTUALIZE_APIC` on it is **this
    // VMM's**, set in vmcs01 so that interrupts can be taken and injected
    // into the guest hypervisor. A guest hypervisor's guest must not
    // inherit it.
    //
    // Inheriting it livelocks, and did: with the bit in vmcs02 every
    // external interrupt arriving while a second-level guest runs exits
    // here, `l1_wants_l2_exit` declines it because pin12 never asked, and
    // the interrupt is deferred rather than delivered - so it is still
    // pending, and the entry that follows exits again immediately.
    // Measured on the rig as a solid run of one hypercall from one
    // address, two hundred of two hundred working exits.
    //
    // The tree predicted this. The comment on the acknowledge-interrupt
    // composition below says "this VMM never sets that control itself...
    // If pin01 ever sets it, this needs a third: and not pin01's." This
    // is that third condition.
    auto pin02 =
        (pin01 | pin12) & ~(pin_preemption_timer | pin_posted_interrupts |
                            pin_external_interrupt);

    // Put back only where the guest hypervisor asked for it, which is
    // what makes the exit its own to handle.
    pin02 |= pin12 & pin_external_interrupt;

    // The profiler's clock, which is the timer put back. See
    // `nested_vmx::profile_l2` for why this is the only instrument that
    // can see a guest spinning on memory.
    if constexpr (nested_vmx::profile_l2) {
        pin02 |= pin_preemption_timer;
    }

    write_vmcs02_control(
        cpu,
        field::pin_based_vm_execution_controls,
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_pin_based_controls),
            pin02));

    if constexpr (nested_vmx::profile_l2) {
        // Reloaded from this field on every entry, because "save
        // VMX-preemption timer value" stays clear in the exit controls -
        // SDM 26.6.4 - so one write here keeps producing exits at the
        // same interval.
        vmcs.write(field::vmx_preemption_timer_value,
                   nested_vmx::profile_timer_value);
    }

    // Primary controls: the union, with the two window controls taken from
    // the guest hypervisor alone. A window exit says "the guest can take
    // an interrupt now", which is an answer to a question only whoever
    // asked it can act on - so inheriting this VMM's would produce exits
    // with nothing to do, and inheriting the guest hypervisor's produces
    // exits it is waiting for. KVM does the same in
    // `prepare_vmcs02_early`.
    auto primary =
        (primary01 & ~(primary_interrupt_window | primary_nmi_window)) |
        primary12;

    // A step in progress survives the rebuild.
    //
    // The monitor trap flag is armed on whichever VMCS is current, and
    // with no second-level extended page tables a watched-page violation
    // taken by a second-level guest is handled with **vmcs02** current -
    // so that is where the bit lands. It is not in `primary01`, and a
    // guest hypervisor has no reason to set it in `primary12`, so this
    // recomputation drops it. Anything reflected to the guest hypervisor
    // before the step completes rebuilds these controls, and then:
    //
    // - the trap exit never arrives, so `on_monitor_trap_flag` never
    //   runs and never closes the page;
    // - `stepping_watch[cpu]` stays set for ever, so `l0_wants_l2_exit`
    //   claims every later trap exit;
    // - and the watched page - the local APIC's - is left **writable for
    //   every processor**, because that is what opening it did. No
    //   further violation is taken on it and nothing re-closes it, so
    //   this VMM stops seeing local APIC writes entirely: every
    //   interrupt command, INIT and start-up IPI after that point is
    //   invisible, with nothing recorded anywhere.
    //
    // KVM does not lose it either: `vmx_update_emulated_instruction`
    // records `nested.mtf_pending` and `vmx_check_nested_events`
    // delivers it as a monitor-trap-flag VM exit, so it survives round
    // trips through L1 and is even part of migration state.
    if (this->stepping_watch[cpu]) {
        primary |= primary_monitor_trap_flag;
    }

    // A second user of this bit was added here to single step the guest
    // after an injected event, and **removed again**. It answered the
    // question it was written for - the first landing after every
    // injection of 0xd1 was an EPT violation with the instruction
    // pointer unmoved, which named the boot bug - and then it kept
    // firing: 30,968 times on the boot processor in one boot, against
    // the sixteen it was meant to take.
    //
    // At that rate it is not a measurement, it is a behaviour change in
    // the shipped path, and it landed in the same window that took
    // `guest vmxon` from eight processors of eight to one. Diagnostics
    // that perturb what they measure belong behind a switch or not at
    // all; this one is not needed again, so it is gone rather than
    // switched. The passive counters it came with stay.

    // The TPR shadow, decided above. Three branches, and the difference
    // between them is which of them is allowed to leave the second-level
    // guest's `mov cr8` reaching the physical control register.
    if (honour_tpr_shadow) {
        this->tpr_shadow_honoured[cpu] =
            this->tpr_shadow_honoured[cpu] + 1;

        // Honoured. The control stays set - it is already in `primary`
        // from the union - and the two fields behind it are written from
        // vmcs12: the page the processor will virtualize VTPR in, and the
        // threshold below which it exits. KVM copies the same pair, in
        // `nested_get_vmcs12_pages` for the address and
        // `prepare_vmcs02_early` for the threshold.
        //
        // The address is written unchanged because the extended page
        // tables are an identity map, so vmcs12's L1-physical address is
        // the host-physical one this field takes. Nothing pins the page:
        // there is no paging here and no memory hot-unplug, and the
        // validation above is what stands in for KVM's kvm_vcpu_map.
        write_vmcs02_control(
            cpu, field::virtual_apic_address, virtual_apic12);
        write_vmcs02_control(cpu, field::tpr_threshold, tpr_threshold12);

        // A histogram of what the guest hypervisor arms, because the
        // whole interrupt question turns on it and nothing recorded it.
        //
        // Vector 0x2f is asked for 297,465 times in a boot and delivered
        // 1,116, and the guest hypervisor's own way of being told it may
        // now deliver is the TPR-below-threshold exit - which fires
        // 1,141 times, so every one that fires does produce a delivery.
        // Either it never arms the notification, in which case the
        // interrupt is its business and not ours, or it arms it and the
        // exit does not fire, in which case the fault is here. An
        // all-zero histogram says the first outright.
        if (cpu < max_cpus) {
            this->l2_tpr_threshold_seen[cpu][tpr_threshold12 & 0xf] += 1;

            // Carried across rather than compared here: from
            // build_vmcs02 the read of the virtual task priority is
            // not available, and the same read of the same offset of
            // the same page works unconditionally from save_l2_state.
            this->nested_tpr_threshold[cpu] = tpr_threshold12;
        }

        // Kept so the task priority behind it can be read back. See
        // `interrupt_request_vtpr`.
        this->nested_virtual_apic_address[cpu] = virtual_apic12;

        // And sampled here, on the page about to be entered, which is
        // the only place that is the right page. See `l2_entry_vtpr`.
        if ((cpu < max_cpus) && (0 != virtual_apic12)) {
            constexpr std::uint64_t virtual_task_priority = 0x80;
            std::uint8_t vtpr{};

            if (read_guest_physical(
                    virtual_apic12 + virtual_task_priority,
                    std::span(reinterpret_cast<std::byte *>(&vtpr),
                              sizeof(vtpr)))) {
                this->l2_entry_vtpr[cpu][vtpr] += 1;
            }
        }
    } else if (tpr_shadow12) {
        // Asked for and not honoured, which the branch above only reaches
        // for a virtual-APIC page this VMM refuses to let the processor
        // touch *and* a guest hypervisor that intercepts both CR8
        // accesses. Removing the control alone would be the bug this
        // whole change exists to fix, so the intercepts it relies on are
        // forced rather than assumed - redundant today, since they came
        // out of vmcs12 through the union above, and load bearing the
        // moment anything narrows that union. KVM forces the same pair in
        // `prepare_vmcs02_early`: "else exec_control |=
        // CPU_BASED_CR8_LOAD_EXITING | CPU_BASED_CR8_STORE_EXITING".
        //
        // Both exits reflect to the guest hypervisor, because
        // `l1_wants_l2_exit` answers CR8 accesses against exactly these
        // two controls and this branch requires both of them set.
        this->tpr_shadow_refused[cpu] = this->tpr_shadow_refused[cpu] + 1;
        primary &= ~primary_tpr_shadow;
        primary |= primary_cr8_load_exiting | primary_cr8_store_exiting;

        // Kept so `on_nested_cr8_access` can answer against the same page
        // and the same threshold the processor would have used. Without
        // the threshold the emulation would be silently one-way: the
        // guest's priority would fall and the guest hypervisor would
        // never be told it may deliver.
        this->nested_virtual_apic_address[cpu] = virtual_apic12;
        this->nested_tpr_threshold[cpu] = tpr_threshold12;
    } else {
        // Never asked for, so the bit can only be here from this VMM's
        // own controls - which do not set it today, making this a guard
        // rather than a case. It is removed and *nothing* is forced in its
        // place, which is the opposite of the branch above and is
        // deliberate: a guest hypervisor that did not ask for the TPR
        // shadow does not believe its guest's CR8 is virtualized, so the
        // architecture's answer is that the second-level guest owns the
        // physical register, exactly as it would on bare hardware.
        // Forcing CR8 exiting here would manufacture exits neither side
        // asked for, which `l1_wants_l2_exit` would decline and the
        // ordinary control-register handler would stop the processor on -
        // it answers MOV to CR4 and nothing else.
        this->tpr_shadow_absent[cpu] = this->tpr_shadow_absent[cpu] + 1;
        primary &= ~primary_tpr_shadow;
    }

    // The bitmaps, whose controls follow the merge rather than either
    // side. "Use MSR bitmaps" clear means *every* MSR access exits, which
    // is what a guest hypervisor that set no bitmap asked for - so the
    // control is the guest hypervisor's, and the bitmap behind it is the
    // union.
    if (0 != (primary12 & primary_msr_bitmaps)) {
        primary |= primary_msr_bitmaps;
        write_vmcs02_control(
            cpu, field::msr_bitmap, this->nested_msr_bitmap_physical[cpu]);
    } else {
        primary &= ~primary_msr_bitmaps;
    }

    // I/O the same way, with one asymmetry: this VMM always uses bitmaps
    // and has ports of its own in them, so a guest hypervisor that uses
    // neither still gets bitmap-driven exits - its own guest's I/O reaches
    // it only for the ports this VMM watches, which is exactly right,
    // since it asked for none.
    if (0 != (primary12 & primary_unconditional_io)) {
        primary |= primary_unconditional_io;
        primary &= ~primary_io_bitmaps;
    } else {
        primary &= ~primary_unconditional_io;
        primary |= primary_io_bitmaps;
        write_vmcs02_control(
            cpu, field::io_bitmap_a, this->nested_io_bitmap_physical[cpu]);
        write_vmcs02_control(cpu,
                             field::io_bitmap_b,
                             this->nested_io_bitmap_physical[cpu] +
                                 page_size);
    }

    // Extended page tables are always in use for a second-level guest, so
    // the secondary controls are always activated whatever the guest
    // hypervisor asked.
    primary |= primary_secondary_controls;

    write_vmcs02_control(
        cpu,
        field::primary_processor_based_vm_execution_controls,
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_processor_based_controls),
            primary));

    // Secondary controls: the union, with three corrections.
    //
    // Unrestricted guest comes from the guest hypervisor alone. With it
    // on, a guest may run with CR0.PE clear, and the guest-state checks
    // SDM 29.3.1 applies are relaxed accordingly - so inheriting this
    // VMM's copy would accept a second-level guest state its own
    // hypervisor's VMCS says is invalid. KVM clears it for the same
    // reason.
    //
    // Mode-based execute control is cleared, and this is the one that is
    // not obvious. The capability MSRs do not offer it, so bit 10 of the
    // guest hypervisor's own extended page-table entries means nothing it
    // chose - it has no reason ever to set it. The shadow builder
    // intersects that bit with this VMM's, so composing it would leave
    // every shadow leaf denying user-mode execute, and the second-level
    // guest would fault on the first instruction it ran in user mode. With
    // the control clear the processor ignores bit 10 entirely and bit 2
    // governs both modes, which is what both levels meant.
    //
    // Extended page tables and VPIDs are always on, because the pointer
    // written below is always a real one and the VPID always non-zero.
    auto secondary =
        (secondary01 | secondary12) &
        ~(secondary_mode_based_execute | secondary_unrestricted_guest);

    secondary |= secondary12 & secondary_unrestricted_guest;
    secondary |= secondary_enable_ept | secondary_enable_vpid;

    auto secondary02 = arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::processor_based_contorls_2),
        secondary);

    write_vmcs02_control(
        cpu,
        field::secondary_processor_based_vm_execution_controls,
        secondary02);

    // What the guest hypervisor asked for against what it got. See
    // `control_pin_requested` - the machine boots under KVM and not here,
    // so a control it set and did not get back is exactly the shape of
    // difference worth looking for.
    if (cpu < max_cpus) {
        this->control_pin_requested[cpu] = pin12;
        this->control_pin_granted[cpu] =
            vmcs.pin_based_vm_execution_controls();
        this->control_primary_requested[cpu] = primary12;
        this->control_primary_granted[cpu] =
            vmcs.primary_processor_based_vm_execution_controls();
        this->control_secondary_requested[cpu] = secondary12;
        this->control_secondary_granted[cpu] = secondary02;
    }

    // Every control either side has *ever* asked for, and every one this
    // VMM ever actually wrote, accumulated rather than sampled.
    //
    // `vmcs12_secondary_controls` beside them records the first entry
    // only, and the first entry is too early to mean anything: it comes
    // back zero while the primary controls have bit 31 set to activate
    // the secondary ones and the shadow extended page tables are
    // demonstrably in use, so the guest hypervisor had not written the
    // field yet at the moment it was read. A record that is empty for a
    // reason unrelated to the question invites exactly one wrong
    // conclusion, which is that nothing was asked for.
    //
    // The difference between the two words below is the whole point. A
    // control set in `asked` and clear in `written` is one this VMM took
    // away - `adjust_msr` clears anything the hardware does not offer,
    // and the composition above removes two deliberately - and a guest
    // hypervisor that asked for a capability, was not told it was
    // refused, and then relied on it is this project's recurring failure
    // stated exactly. The ones being looked for are virtual-interrupt
    // delivery, APIC-register virtualization and virtualize-APIC-
    // accesses, because a guest hypervisor delivering interrupts to its
    // guest through a virtual APIC this VMM does not maintain would stop
    // exactly the way the rig stops.
    this->vmcs12_secondary_asked =
        this->vmcs12_secondary_asked | secondary12;
    this->vmcs02_secondary_written =
        this->vmcs02_secondary_written | secondary02;
    this->vmcs12_primary_asked = this->vmcs12_primary_asked | primary12;
    this->vmcs12_pin_asked = this->vmcs12_pin_asked | pin12;

    // Exit controls are this VMM's, because the exit comes here - with
    // one exception, and the exception is the point.
    //
    // The controls divide into three groups and only one of them can be
    // delegated. The *load* group - host address-space size, load
    // IA32_PAT, load IA32_EFER - describes what a VM exit loads into
    // host state, and vmcs02's host state is this VMM's, so composing
    // them would load the guest hypervisor's host state into this VMM on
    // an exit that is not going there. The *save* group is emulated
    // instead of delegated: `save_l2_state` reads exit12 itself and
    // writes the guest hypervisor's guest-state area conditionally, so
    // the hardware does not need to be told.
    //
    // "Acknowledge interrupt on exit" is in neither group, and it is the
    // one thing here that cannot be emulated at all. SDM 30.2: "An
    // external interrupt does not acknowledge the interrupt controller
    // and the interrupt remains pending, unless the 'acknowledge
    // interrupt on exit' VM-exit control is 1. In such a case, the
    // interrupt controller is acknowledged and the interrupt is no
    // longer pending." SDM 27.9.2 adds the other half: the exiting-event
    // identification field - the vector, in bits 7:0 - is provided for
    // external interrupts only while that control is 1.
    //
    // Only the processor can take a vector from the interrupt
    // controller. So a guest hypervisor that asked for the control and
    // was given an exit without it learns nothing: the reason says an
    // external interrupt arrived, the vector field is not valid, and the
    // interrupt is still pending behind it. It cannot dispatch the
    // interrupt, and this VMM is not going to either.
    //
    // KVM makes the same control mandatory for itself -
    // KVM_REQUIRED_VMX_VM_EXIT_CONTROLS in vmx-internal.h - and reads
    // vmcs12's copy in `nested_exit_intr_ack_set`.
    //
    // Conditioned on the exit being one that will be reflected, because
    // acknowledging *consumes* the interrupt and an acknowledgement on
    // an exit this VMM keeps would drop it. `l1_wants_l2_exit` decides
    // that for an external interrupt on pin12's external-interrupt
    // exiting alone, so the two conditions below are the whole of what
    // this composition adds.
    //
    // With `ZPP_VIRTUALIZE_APIC` on, pin01 sets external-interrupt
    // exiting too and exit01 already carries the acknowledge - so vmcs02
    // inherits it here whatever vmcs12 says, and an exit this VMM keeps
    // *is* acknowledged. That used to be the reason for the warning this
    // paragraph replaces. It is answered rather than avoided now:
    // `queue_external_interrupt` records the vector in a per-processor
    // bitmap and `deliver_pending_external_interrupt` refuses to put it
    // into a second-level guest, holding it until vmcs01 is current
    // again. Withholding the acknowledge instead would be worse - the
    // exit would report an external interrupt with no vector, and this
    // VMM could neither dispatch it nor give it back.
    auto exit02 = exit01;

    if ((0 != (exit12 & exit_acknowledge_interrupt)) &&
        (0 != (pin12 & pin_external_interrupt))) {
        exit02 = exit02 | exit_acknowledge_interrupt;
    }

    exit02 = arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_exit_controls), exit02);

    write_vmcs02_control(cpu, field::vm_exit_controls, exit02);

    // Same comparison as the execution controls above, for the group
    // that has never had one. A bit set in `asked` and clear in
    // `written` is an exit control the guest hypervisor requested and
    // did not get.
    this->vmcs12_exit_asked = this->vmcs12_exit_asked | exit12;
    this->vmcs02_exit_written = this->vmcs02_exit_written | exit02;

    // Entry controls are the guest hypervisor's, unchanged. They describe
    // what VM entry loads into *its* guest, which is a decision it owns
    // completely - including "IA-32e mode guest", which has to agree with
    // the CR0 and CR4 it wrote beside them or the entry fails its own
    // consistency check and is reflected as such.
    write_vmcs02_control(
        cpu,
        field::vm_entry_controls,
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_entry_controls), entry12));

    write_vmcs02_control(cpu, field::ept_pointer, eptp02);
    write_vmcs02_control(cpu, field::vpid, vpid01);

    // All ones is "no linked VMCS". VMCS shadowing is not offered, so the
    // guest hypervisor's own link pointer is not consulted.
    write_vmcs02_control(cpu, field::vmcs_link_pointer, ~std::uint64_t{});

    // Where a refused entry unwinds to. See asm.h: the stubs read this
    // field because on a refusal nothing has been reloaded and there is no
    // other per-processor thing left addressable.
    write_vmcs02_control(cpu,
                         field::cr3_target_value_0,
                         reinterpret_cast<std::uint64_t>(
                             &this->nested_entry_recovery[cpu]));
    write_vmcs02_control(cpu, field::cr3_target_count, 0);

    // The exception bitmap is the bitwise or of what the guest hypervisor
    // wants to trap and what this VMM does, which is the merge KVM
    // describes on `vmx_update_exception_bitmap`. The page-fault
    // error-code mask and match come from the guest hypervisor unchanged,
    // because this VMM traps no page faults of its own - if it ever does,
    // both must go to zero so that every page fault exits and the
    // filtering moves into the reflect decision.
    write_vmcs02_control(cpu,
                         field::exception_bitmap,
                         exception_bitmap01 |
                             shadow.read(field::exception_bitmap));
    write_vmcs02_control(cpu,
                         field::page_fault_error_code_mask,
                         shadow.read(field::page_fault_error_code_mask));
    write_vmcs02_control(cpu,
                         field::page_fault_error_code_match,
                         shadow.read(field::page_fault_error_code_match));

    // The control-register masks are the union too, and the read shadows
    // then have to carry the whole answer rather than half of it - see
    // effective_control_register.
    auto cr0_mask12 = shadow.read(field::cr0_guest_host_mask);
    auto cr4_mask12 = shadow.read(field::cr4_guest_host_mask);
    auto cr0_12 = shadow.read(field::guest_cr0);
    auto cr4_12 = shadow.read(field::guest_cr4);

    write_vmcs02_control(
        cpu, field::cr0_guest_host_mask, cr0_mask01 | cr0_mask12);
    write_vmcs02_control(
        cpu, field::cr4_guest_host_mask, cr4_mask01 | cr4_mask12);

    write_vmcs02_control(
        cpu,
        field::cr0_read_shadow,
        effective_control_register(
            cr0_12, shadow.read(field::cr0_read_shadow), cr0_mask12));
    write_vmcs02_control(
        cpu,
        field::cr4_read_shadow,
        effective_control_register(
            cr4_12, shadow.read(field::cr4_read_shadow), cr4_mask12));

    // VMXE is forced into the real register for the same reason it is for
    // the guest hypervisor: IA32_VMX_CR4_FIXED0 requires it in VMX
    // operation, so a guest-state area without it fails VM entry. The read
    // shadow above answers for the bit, so nothing sees it.
    vmcs.guest_cr0(cr0_12);

    // VMXE forced in, SMXE forced out, for the two reasons stated where
    // vmcs01 does the same: a processor in VMX operation must have VMXE
    // (IA32_VMX_CR4_FIXED0), and nothing here implements SMX. A
    // second-level guest running with CR4.SMXE set would exit on GETSEC -
    // SDM 28.1.2 - and that exit belongs to neither level: this VMM does
    // not offer the feature and a guest hypervisor was never told its
    // guest had it either, since the CPUID concealment applies to every
    // level below this one.
    vmcs.guest_cr4((cr4_12 | cr4_vmxe) & ~cr4_smxe);

    {
        auto fresh = this->guest_state_fresh[cpu];
        std::size_t index{};

        for (auto guest_field : guest_state_fields) {
            auto value = shadow.read(guest_field);

            if (fresh && (this->guest_state_cache[cpu][index] == value)) {
                this->guest_state_writes_skipped[cpu] += 1;
                ++index;
                continue;
            }

            vmcs.write(guest_field, value);
            this->guest_state_cache[cpu][index] = value;
            this->guest_state_writes_done[cpu] += 1;
            ++index;
        }

        // Consumed: the next elision needs its own save to justify it.
        this->guest_state_fresh[cpu] = false;
    }

    // The activity state is decided, never copied.
    //
    // Active until `enter_or_park_l2` says otherwise, so that vmcs02 is
    // never left holding a state nothing here chose. That matters because
    // this function runs on every entry attempt while the decision below
    // it may end in the entry not happening at all: a leftover
    // wait-for-SIPI here would be entered by any later resume that skipped
    // the decision, and a processor entered in that state blocks external
    // interrupts, NMIs, INIT and SMIs (SDM 29.7.2) with nothing in vmcs02
    // able to end it - not even this VMM's preemption timer, which
    // build_vmcs02 strips.
    vmcs.write(field::guest_activity_state,
               arch::x86_64::vmx::activity_state::active);

    vmcs.guest_rip(shadow.read(field::guest_rip));
    vmcs.guest_rsp(shadow.read(field::guest_rsp));
    vmcs.guest_rflags(shadow.read(field::guest_rflags));
    vmcs.guest_dr7(shadow.read(field::guest_dr7));
    vmcs.write(field::guest_ia32_pat, shadow.read(field::guest_ia32_pat));
    vmcs.write(field::guest_ia32_efer,
               shadow.read(field::guest_ia32_efer));

    // Carried because the entry control that loads it is now offered.
    //
    // A guest hypervisor that sets "load IA32_BNDCFGS" expects the value
    // in its own VMCS to be the one its guest runs with, so leaving this
    // field at whatever the entered VMCS happened to hold would honour
    // the control's presence and not its meaning. Copied unconditionally
    // rather than under the control bit, exactly as IA32_PAT and
    // IA32_EFER above are: the processor ignores the field when the
    // control is clear, so there is nothing to gate and a gate would
    // only be another thing to get wrong.
    vmcs.write(field::guest_ia32_bndcfgs,
               shadow.read(field::guest_ia32_bndcfgs));
    vmcs.write(field::guest_interruptibility_state,
               shadow.read(field::guest_interruptibility_state));

    // The time stamp counter offset composes across levels: what this VMM
    // applies to the guest hypervisor, plus what the guest hypervisor
    // applies to its own guest. KVM's `kvm_calc_nested_tsc_offset` is the
    // same sum. This VMM applies none today, so the sum is the guest
    // hypervisor's, and writing it as a sum is what keeps it correct if
    // that changes.
    auto tsc_offset01 = (0 != (primary01 & primary_tsc_offsetting))
                            ? vmcs.read(field::tsc_offset)
                            : std::uint64_t{};
    auto tsc_offset12 = (0 != (primary12 & primary_tsc_offsetting))
                            ? shadow.read(field::tsc_offset)
                            : std::uint64_t{};

    // The multiplier, which has to be composed before the offset because
    // the offset is scaled by it. SDM 27.6.5 orders the two: "the
    // contents of the time-stamp counter is first multiplied by the TSC
    // multiplier before adding the TSC offset", so what the second-level
    // guest must see is
    //
    //     ((tsc * m01 >> 48) + o01) * m12 >> 48 + o12
    //
    // and multiplying that out gives the pair below - the multipliers
    // composed, and *this VMM's* offset scaled by the guest
    // hypervisor's before its own is added. KVM reaches the same two in
    // `kvm_calc_nested_tsc_multiplier` and `kvm_calc_nested_tsc_offset`,
    // and gates the guest hypervisor's multiplier on both controls
    // exactly as `vmx_get_l2_tsc_multiplier` does - scaling means
    // nothing without offsetting, which SDM 27.6.5 also says: the field
    // applies "if this control is 1 (and the 'RDTSC exiting' control is
    // 0 and the 'use TSC offsetting' control is 1)".
    //
    // Both levels default to 1.0 in 48-bit fixed point when they are not
    // scaling, so a machine where neither does composes to exactly the
    // sum this used to be.
    auto scaling12 = (0 != (secondary12 & secondary_tsc_scaling)) &&
                     (0 != (primary12 & primary_tsc_offsetting));

    auto multiplier01 = (0 != (secondary01 & secondary_tsc_scaling))
                            ? vmcs.read(field::tsc_multiplier)
                            : tsc_scaling_default;
    auto multiplier12 = scaling12 ? shadow.read(field::tsc_multiplier)
                                  : tsc_scaling_default;

    auto scaled = tsc_scaling_default != multiplier12;

    write_vmcs02_control(
        cpu,
        field::tsc_offset,
        (scaled ? signed_scaled_product(tsc_offset01, multiplier12)
                : tsc_offset01) +
            tsc_offset12);

    // Written only when the control that reads it is set, since the
    // field does not exist on a processor that does not offer the
    // control and a VMWRITE to it would fail there.
    if (0 != (secondary02 & secondary_tsc_scaling)) {
        write_vmcs02_control(
            cpu,
            field::tsc_multiplier,
            scaled ? scaled_product(multiplier01, multiplier12)
                   : multiplier01);
    }

    // The event the guest hypervisor asked to inject, taken from its VMCS
    // on the entry that starts its guest running. SDM 27.8.3 makes the
    // three fields a set: the information field's valid bit decides
    // whether the other two are read at all.
    auto injection =
        shadow.read(field::vm_entry_interruption_information_field);

    // Nothing staged, and the guest is asking for something its own
    // priority allows: deliver it. The guest hypervisor's own injection
    // always wins, because this only runs when it made none.
    if constexpr (nested_vmx::deliver_self_ipi) {
        constexpr std::uint64_t valid = 1ull << 31;
        constexpr std::uint64_t external = 0ull << 8;
        constexpr std::uint64_t priority_class = 4;

        if ((cpu < max_cpus) && (0 == (injection & valid)) &&
            (0 != this->l2_self_ipi_pending[cpu]) &&
            (0 != this->nested_virtual_apic_address[cpu])) {
            constexpr std::uint64_t virtual_task_priority = 0x80;
            std::uint8_t vtpr{};

            auto read = read_guest_physical(
                this->nested_virtual_apic_address[cpu] +
                    virtual_task_priority,
                std::span(reinterpret_cast<std::byte *>(&vtpr),
                          sizeof(vtpr)));

            auto vector = this->l2_self_ipi_pending[cpu];

            // The task priority is not the whole of "may this be
            // delivered". An external interrupt injected while the guest
            // has RFLAGS.IF clear, or while it is inside the
            // one-instruction shadow after STI or a MOV to SS, arrives
            // in a critical section that had disabled interrupts to keep
            // one - and the VM entry does **not** refuse it, so nothing
            // catches the mistake. SDM 27.2.1.3 lists the checks on an
            // injected event and RFLAGS.IF is not among them for an
            // external interrupt; KVM's own equivalent is the
            // `vmx_interrupt_allowed` test it makes before injecting,
            // rather than anything the processor does.
            //
            // Read out of vmcs12 rather than the VMCS, which is free:
            // `build_vmcs02` has these two values in hand from the guest
            // hypervisor's own guest-state area, and they are what the
            // entry below is about to load.
            constexpr std::uint64_t rflags_interrupt_enable = 1ull << 9;
            constexpr std::uint64_t blocking_by_sti = 1ull << 0;
            constexpr std::uint64_t blocking_by_mov_ss = 1ull << 1;

            auto blocking =
                shadow.read(field::guest_interruptibility_state);

            auto interruptible =
                (0 != (shadow.read(field::guest_rflags) &
                       rflags_interrupt_enable)) &&
                (0 == (blocking & (blocking_by_sti | blocking_by_mov_ss)));

            if (read && interruptible &&
                ((vector >> priority_class) >
                 (std::uint64_t{vtpr} >> priority_class))) {
                injection = valid | external | vector;
                this->l2_self_ipi_pending[cpu] = 0;
                this->l2_self_ipi_delivered[cpu] =
                    this->l2_self_ipi_delivered[cpu] + 1;
            } else {
                this->l2_self_ipi_held[cpu] =
                    this->l2_self_ipi_held[cpu] + 1;
            }
        }
    }

    vmcs.write(field::vm_entry_interruption_information_field, injection);

    if (0 != (injection & interruption_valid)) {
        vmcs.write(field::vm_entry_exception_error_code,
                   shadow.read(field::vm_entry_exception_error_code));
        vmcs.write(field::vm_entry_instruction_length,
                   shadow.read(field::vm_entry_instruction_length));

        // Counted by vector, the same shape as `l2_external_vector` and
        // for the same reason: to name what is arriving rather than
        // infer it. See the declaration - the one vector this is here
        // to look for is `0xd1`, the synthetic interrupt the root
        // partition's timer messages are delivered on.
        if (cpu < max_cpus) {
            auto vector = injection & interruption_vector_mask;
            this->l2_injected_vector[cpu][vector] =
                this->l2_injected_vector[cpu][vector] + 1;

            // Where the guest was when the event was injected, and a
            // flag for the exit handler to fill in where it went. See
            // the declarations: this is the first thing in the
            // investigation that observes the guest rather than the
            // hand-over, and it is the only way to tell a handler that
            // ran and failed from a vector that was never taken.
            constexpr std::uint64_t synthetic_interrupt_3 = 0xd1;

            if (synthetic_interrupt_3 == vector) {
                auto slot = this->injection_landing_count[cpu] %
                            injection_landing_capacity;
                this->injection_from_rip[cpu][slot] =
                    shadow.read(field::guest_rip);
                this->injection_to_rip[cpu][slot] = 0;
                this->injection_landing_armed[cpu] = 1;
            }
        }
    }

    // The processor is given no MSR areas of its own. The three the guest
    // hypervisor named are processed here instead, in software, and
    // handing its addresses to the processor is what must not happen: the
    // processor reads and *writes* those lists in root operation, where
    // extended page tables do not apply - so a guest hypervisor could name
    // this module's own physical pages as its VM-exit MSR-store area and
    // have the processor write MSR values into them. Every other
    // protection this VMM has is an extended page-table permission, and
    // none of them would apply.
    write_vmcs02_control(cpu, field::vm_entry_msr_load_count, 0);
    write_vmcs02_control(cpu, field::vm_exit_msr_load_count, 0);
    write_vmcs02_control(cpu, field::vm_exit_msr_store_count, 0);

    // SDM 29, step 4: the MSR loads are the last thing a VM entry does
    // before the launch state changes. Done here, at the end, for the same
    // reason - and after the guest state is in vmcs02, so that a failure
    // leaves the same thing behind a processor's would.
    this->nested_msr_load_failed[cpu] = false;

    if (auto loaded =
            load_nested_msrs(cpu, entry_load_address, entry_load_count);
        !loaded) {
        this->nested_msr_load_failed[cpu] = true;
        return std::unexpected(
            zpp::error{error::nested_msr_area_unsupported});
    }

    // The transition itself. A guest hypervisor without VPIDs of its own
    // expects VM entry to flush, and a second-level guest shares this
    // VMM's VPID - so the flush has to be performed rather than left to
    // hardware, which will not do it for a non-zero VPID.
    nested_transition_flush();

    return {};
}

void hypervisor::nested_transition_flush()
{
    // Single-context, on this VMM's own VPID, which SDM 31.4.3.1 makes
    // invalidate "linear mappings and combined mappings associated with
    // that VPID ... for all PCIDs and, for combined mappings, all
    // EPTRTAs". That covers both levels at once: the guest hypervisor's
    // mappings and its guest's differ only in the extended page-table
    // root, and this invalidates every root.
    //
    // Over-invalidation rather than precision, deliberately. The
    // alternative is a VPID of its own for the second level, which buys
    // back the mappings this throws away and costs a second identifier per
    // processor plus the bookkeeping to keep it in step with the guest
    // hypervisor's own. The measurement that would justify it is the exit
    // rate of a real guest hypervisor, which nothing has yet run.
    constexpr std::uint64_t single_context = 1;

    struct alignas(0x10) invvpid_descriptor
    {
        std::uint64_t vpid{};
        std::uint64_t linear_address{};
    };

    invvpid_descriptor descriptor{this->vmcs.vpid(), 0};

    if (arch::x86_64::vmx::invvpid(single_context, &descriptor)) {
        log("invvpid failed on a nested transition, cpu {}",
            this->vmcs.vpid());
    }
}

// The captured context is no longer read - the two cases that needed a
// guest register, the MSR number in ECX and the I/O port, are gone. It
// stays in the signature because this and `l1_wants_l2_exit` are a pair
// called from one place with one set of arguments, and because the next
// case to need it will.
bool hypervisor::l0_wants_l2_exit(std::size_t cpu,
                                  arch::x86_64::vmx::exit_reason reason,
                                  const arch::x86_64::context &)
{
    auto & vmcs = this->vmcs;

    // The question this answers is narrow on purpose: not "can this VMM
    // handle it" but "must it", whatever the guest hypervisor asked. KVM
    // splits the decision the same way in `nested_vmx_l0_wants_exit`, and
    // asks it first, because an exit this VMM needs is one no reflection
    // may take away.
    switch (reason.basic()) {
    case basic_reason::exception_or_nmi: {
        // A non-maskable interrupt is this VMM's whoever is running: NMI
        // exiting is set in its own pin controls and the handler hands the
        // interrupt back to the guest's world. Everything else in this
        // exit is an exception, and exceptions belong to whoever put the
        // vector in the exception bitmap.
        auto information =
            vmcs.read(field::vm_exit_interruption_information);
        auto type = (information >> interruption_type_shift) &
                    interruption_type_mask;

        return interruption_type_nmi == type;
    }

    case basic_reason::ept_violation:
    case basic_reason::ept_misconfiguration:
        // Always, and the composition decides afterwards whose fault it
        // was. The processor walked the *shadow*, which is neither side's
        // table, so nothing about the exit as delivered describes what the
        // guest hypervisor's tables say - that has to be worked out here.
        // KVM reaches the same conclusion in `nested_vmx_l0_wants_exit`.
        return true;

    case basic_reason::vmx_preemption_timer:
        // This VMM's clock. The capability MSRs do not offer the timer, so
        // a guest hypervisor cannot have armed it.
        return true;

    case basic_reason::monitor_trap_flag:
        // Only while this VMM is stepping a watched write. A guest
        // hypervisor may set the flag itself - it is in the primary
        // controls it is offered - and then the exit is its own.
        return this->stepping_watch[cpu];

        // MSR accesses and I/O are deliberately absent, and that is a
        // change from what this used to do.
        //
        // They used to be claimed here whenever *this VMM's* bitmap named
        // them, which took them away from a guest hypervisor that had
        // asked for them too. The ones this VMM arms are exactly the set a
        // guest hypervisor presenting VMX to its own guest also arms:
        // IA32_APIC_BASE unconditionally, and IA32_FEATURE_CONTROL with
        // the whole VMX capability range when nested VMX is on. So a
        // second-level guest touching one of those was answered here and
        // its own hypervisor never learned it had - which is the shape of
        // a guest hypervisor that stops making progress with nothing
        // faulting.
        //
        // KVM names neither in `nested_vmx_l0_wants_exit`; the decision is
        // `nested_vmx_l1_wants_exit`'s and the exit is reflected. L0's own
        // interest is served a moment later, when the guest hypervisor
        // performs the access itself and exits from *its* context - which
        // is the right order, because the machine the second-level guest
        // sees is the guest hypervisor's, not this one's.
        //
        // Nothing else is needed to keep this VMM's own interest:
        // `on_l2_exit` already routes an exit neither side asked for to
        // the ordinary handler, so an access only this VMM wanted still
        // lands there.
        //
        // Both were found by tests/nested_exit, which exercises this
        // decision against KVM's for every exit reason.

    default:
        return false;
    }
}

bool hypervisor::l1_wants_l2_exit(std::size_t cpu,
                                  arch::x86_64::vmx::exit_reason reason,
                                  const arch::x86_64::context & context)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};
    auto pin12 = shadow.read(field::pin_based_vm_execution_controls);

    auto primary_set = [&](std::uint64_t control) {
        return 0 != (primary12 & control);
    };

    auto secondary_set = [&](std::uint64_t control) {
        return 0 != (secondary12 & control);
    };

    switch (reason.basic()) {
    case basic_reason::exception_or_nmi: {
        // Filtered through the guest hypervisor's own exception bitmap,
        // with the page-fault error-code mask and match applied to vector
        // 14. SDM 27.6.3: a page fault exits if
        // "(error_code & mask) == match" agrees with the bitmap bit.
        auto information =
            vmcs.read(field::vm_exit_interruption_information);
        auto vector = information & interruption_vector_mask;
        auto bitmap = shadow.read(field::exception_bitmap);

        constexpr std::uint64_t page_fault_vector = 14;

        if (page_fault_vector == vector) {
            auto error_code =
                vmcs.read(field::vm_exit_interruption_error_code);
            auto mask = shadow.read(field::page_fault_error_code_mask);
            auto match = shadow.read(field::page_fault_error_code_match);
            auto in_bitmap = 0 != (bitmap & (1ull << page_fault_vector));

            return in_bitmap == ((error_code & mask) == match);
        }

        return 0 != (bitmap & (1ull << vector));
    }

    case basic_reason::external_interrupt:
        // Unlike KVM, which always takes this for itself because its host
        // has interrupt handlers to run, this VMM does not set
        // external-interrupt exiting by default - interrupts are the
        // guest's, which owns the interrupt controller. So ordinarily the
        // control can only be set because the guest hypervisor asked, and
        // the exit can only be its own.
        //
        // With `ZPP_VIRTUALIZE_APIC` on, pin01 sets it as well, and this
        // answer stays right: an interrupt the guest hypervisor asked to
        // see is still its own, and one it did not ask to see is kept
        // here and queued for the *first-level* guest rather than put
        // into the second-level one.
        return 0 != (pin12 & pin_external_interrupt);

    case basic_reason::interrupt_window:
        return primary_set(primary_interrupt_window);
    case basic_reason::nmi_window:
        return primary_set(primary_nmi_window);
    case basic_reason::hlt:
        return primary_set(primary_hlt_exiting);
    case basic_reason::invlpg:
        return primary_set(primary_invlpg_exiting);
    case basic_reason::rdpmc:
        return primary_set(primary_rdpmc_exiting);
    case basic_reason::rdtsc:
    case basic_reason::rdtscp:
        return primary_set(primary_rdtsc_exiting);
    case basic_reason::mov_debug_register:
        return primary_set(primary_mov_dr_exiting);
    case basic_reason::mwait:
        return primary_set(primary_mwait_exiting);
    case basic_reason::monitor:
        return primary_set(primary_monitor_exiting);
    case basic_reason::monitor_trap_flag:
        return primary_set(primary_monitor_trap_flag);
    case basic_reason::pause:
        return primary_set(primary_pause_exiting) ||
               secondary_set(secondary_pause_loop_exiting);
    case basic_reason::rdrand:
        return secondary_set(secondary_rdrand_exiting);
    case basic_reason::rdseed:
        return secondary_set(secondary_rdseed_exiting);
    case basic_reason::wbinvd:
        return secondary_set(secondary_wbinvd_exiting);
    case basic_reason::gdtr_or_idtr:
    case basic_reason::ldtr_or_tr:
        return secondary_set(secondary_descriptor_table_exiting);
    case basic_reason::xsaves:
    case basic_reason::xrstors:
        return secondary_set(secondary_enable_xsaves);
    case basic_reason::invpcid:
        return secondary_set(secondary_enable_invpcid) &&
               primary_set(primary_invlpg_exiting);
    case basic_reason::tpr_below_threshold:
        // Correct without qualification now that the control is honoured,
        // and it was correct before only because the exit could not
        // happen. The exit exists only where vmcs02 has the TPR shadow set
        // (SDM 27.6.8: the threshold "exists only on processors that
        // support the 1-setting of the 'use TPR shadow' VM-execution
        // control"), `build_vmcs02` sets it in vmcs02 only where vmcs12
        // set it, and this VMM never sets it for itself - so the exit is
        // always the guest hypervisor's, never shared. `l0_wants_l2_exit`
        // therefore does not name it.
        return primary_set(primary_tpr_shadow);

    case basic_reason::control_register_access: {
        // SDM Table 28-3, "Exit Qualification for Control-Register
        // Accesses": bits 3:0 the register number, bits 5:4 the access
        // type, bits 11:8 the general purpose register. Same shape as
        // KVM's `nested_vmx_exit_handled_cr`.
        auto qualification = vmcs.exit_qualification();
        auto number = qualification & 0xf;
        auto access = (qualification >> 4) & 0x3;

        constexpr std::uint64_t access_move_to = 0;
        constexpr std::uint64_t access_move_from = 1;
        constexpr std::uint64_t access_clts = 2;
        constexpr std::uint64_t access_lmsw = 3;

        auto cr0_mask = shadow.read(field::cr0_guest_host_mask);
        auto cr4_mask = shadow.read(field::cr4_guest_host_mask);

        switch (access) {
        case access_move_to:
            switch (number) {
            case 0:
            case 4: {
                // A write exits to the guest hypervisor only if it changes
                // a bit the guest hypervisor owns, which is what SDM
                // 26.1.3 makes the guest/host mask mean - so the decision
                // needs the value being written, not just the mask.
                //
                // Comparing rather than testing the mask matters here, and
                // for CR4 specifically. This VMM masks VMXE for itself, so
                // *every* CR4 write by a second-level guest exits whether
                // or not the guest hypervisor asked. Reflecting all of
                // them because its mask happens to be non-empty would hand
                // it exits for bits it does not own, and a hypervisor that
                // trusts the architecture will act on the ones it sees.
                //
                // The register the value is in is named by bits 11:8, and
                // it is a *second-level* register - so it is in the
                // captured context, not in the VMCS. guest_register knows
                // that RSP is the exception.
                auto gpr = (qualification >> 8) & 0xf;
                auto written = guest_register(context, gpr);

                auto mask = (0 == number) ? cr0_mask : cr4_mask;

                // Against the *read shadow*, which is what the guest
                // hypervisor decided its guest should believe the
                // register holds - not against the register itself.
                //
                // SDM 28.1.3: "MOV to CR0 ... causes a VM exit unless the
                // value of its source operand matches, for the position
                // of each bit set in the CR0 guest/host mask, the
                // corresponding bit in the CR0 read shadow", and the same
                // sentence for CR4. KVM's nested_vmx_exit_handled_cr
                // spells it `vmcs12->cr0_guest_host_mask & (val ^
                // vmcs12->cr0_read_shadow)`.
                //
                // This used to compare against vmcs02's guest CR0, which
                // build_vmcs02 writes from vmcs12's *guest* field. The two
                // are different fields and are meant to differ - a guest
                // hypervisor owns a bit precisely so it can show its guest
                // something other than what the register holds. Wherever
                // they differed the decision was made on the wrong
                // operand, in both directions: an exit reflected that was
                // never wanted, and - the one that costs a boot - an exit
                // swallowed that the guest hypervisor was waiting for.
                auto shadow_value =
                    (0 == number) ? shadow.read(field::cr0_read_shadow)
                                  : shadow.read(field::cr4_read_shadow);

                return 0 != ((written ^ shadow_value) & mask);
            }
            case 3:
                return primary_set(primary_cr3_load_exiting);
            case 8:
                return primary_set(primary_cr8_load_exiting);
            default:
                return true;
            }
        case access_move_from:
            switch (number) {
            case 3:
                return primary_set(primary_cr3_store_exiting);
            case 8:
                return primary_set(primary_cr8_store_exiting);
            default:
                return true;
            }
        case access_clts: {
            // CLTS clears CR0.TS, so it exits to the guest hypervisor only
            // if that bit is one it owns *and* one it is showing as set.
            //
            // SDM 28.1.3: "The CLTS instruction causes a VM exit if the
            // bits in position 3 (corresponding to CR0.TS) are set in both
            // the CR0 guest/host mask and the CR0 read shadow." 28.3
            // gives the other half: mask set and shadow clear means CLTS
            // "completes but does not change the contents of CR0.TS" - no
            // exit. Testing the mask alone reflected that case too. KVM:
            // nested.c case 2, `(mask & X86_CR0_TS) && (read_shadow &
            // X86_CR0_TS)`.
            constexpr std::uint64_t task_switched = 1ull << 3;
            auto shadow_value = shadow.read(field::cr0_read_shadow);

            return (0 != (cr0_mask & task_switched)) &&
                   (0 != (shadow_value & task_switched));
        }
        case access_lmsw:
        default: {
            // LMSW writes CR0's low four bits, and whether that exits
            // depends on the value it would write - which is in the
            // qualification, not in a register.
            //
            // SDM 28.1.3 splits it in two, because "LMSW never clears bit
            // 0 of CR0 (CR0.PE)":
            //
            //   - PE exits only if the bit is set in both the mask and the
            //     source operand while clear in the read shadow. A source
            //     with PE clear cannot clear it, so it is not a change.
            //   - bits 3:1 exit if the mask owns the bit and the source
            //     and the read shadow disagree about it.
            //
            // SDM Table 28-3 puts the source data in bits 31:16 of the
            // exit qualification. KVM builds the same two-part test in
            // nested_vmx_exit_handled_cr's LMSW case.
            //
            // This used to return "any of the low four bits is owned",
            // which reflects every LMSW a second-level guest executes
            // whatever it writes.
            constexpr std::uint64_t protection_enable = 1ull << 0;
            constexpr std::uint64_t lmsw_upper_bits = 0xeull;

            auto source = (qualification >> 16) & 0xffff;
            auto shadow_value = shadow.read(field::cr0_read_shadow);

            if ((0 != (cr0_mask & protection_enable)) &&
                (0 != (source & protection_enable)) &&
                (0 == (shadow_value & protection_enable))) {
                return true;
            }

            return 0 !=
                   (cr0_mask & lmsw_upper_bits & (source ^ shadow_value));
        }
        }
    }

    case basic_reason::io_instruction: {
        // SDM Table 28-5: bits 2:0 the size, bit 3 the direction, bit 4
        // string, bit 5 REP, bit 6 operand encoding, bits 31:16 the port.
        // The bitmaps win when both are set. SDM 28.1.3
        // (.references/sdm.txt:200725) puts it in parentheses: "the
        // 'unconditional I/O exiting' VM-execution control is ignored if
        // the 'use I/O bitmaps' VM-execution control is 1". Testing
        // unconditional first reflected every I/O instruction to a guest
        // hypervisor that had set both - including the ports it had
        // explicitly cleared in its own bitmap - and both controls are
        // offered, so that is a configuration it can reach.
        if (!primary_set(primary_io_bitmaps)) {
            return primary_set(primary_unconditional_io);
        }

        auto qualification = vmcs.exit_qualification();
        auto port = static_cast<std::uint32_t>(qualification >> 16);
        auto size = static_cast<std::uint32_t>((qualification & 0x7) + 1);

        // Every byte of the access is checked, because a wide access whose
        // first port is not intercepted may still touch one that is. KVM's
        // `nested_vmx_exit_handled_io` walks the same range.
        for (std::uint32_t i{}; i < size; ++i) {
            auto at = port + i;

            // A wrapping access exits, it does not stop being checked.
            // SDM 28.1.3 (.references/sdm.txt:200724): "If an I/O
            // operation 'wraps around' the 16-bit I/O-port space
            // (accesses ports FFFFH and 0000H), the I/O instruction
            // causes a VM exit." Breaking out of the loop answered
            // "not intercepted" for a four-byte access at port 0xffff
            // against an empty bitmap; KVM returns true the moment the
            // port reaches 0x10000.
            if (at > 0xffff) {
                return true;
            }

            auto base = (at < 0x8000) ? shadow.read(field::io_bitmap_a)
                                      : shadow.read(field::io_bitmap_b);
            auto bit = at & 0x7fff;

            std::uint8_t byte{};
            auto read = read_guest_physical(
                base + (bit / 8),
                std::span(reinterpret_cast<std::byte *>(&byte), 1));

            // A bitmap this VMM cannot read is treated as intercepting.
            // The guest hypervisor named the page; if it is unreadable
            // that is its problem to see, and reflecting shows it.
            if (!read || (0 != (byte & (1u << (bit % 8))))) {
                return true;
            }
        }

        return false;
    }

    case basic_reason::rdmsr:
    case basic_reason::wrmsr: {
        if (!primary_set(primary_msr_bitmaps)) {
            // No bitmap means every MSR access exits, which is what the
            // guest hypervisor asked for.
            return true;
        }

        auto index = static_cast<std::uint32_t>(context.rcx);
        auto write = basic_reason::wrmsr == reason.basic();

        std::size_t base{};
        std::uint32_t bit{};

        if (index < 0x2000) {
            base = write ? 0x800 : 0x000;
            bit = index;
        } else if ((index >= 0xc0000000) && (index < 0xc0002000)) {
            base = write ? 0xc00 : 0x400;
            bit = index - 0xc0000000;
        } else {
            // Outside both ranges the bitmap is not consulted and the
            // access exits unconditionally, so it is the guest
            // hypervisor's. SDM 28.1.3.
            return true;
        }

        std::uint8_t byte{};
        auto read = read_guest_physical(
            shadow.read(field::msr_bitmap) + base + (bit / 8),
            std::span(reinterpret_cast<std::byte *>(&byte), 1));

        return !read || (0 != (byte & (1u << (bit % 8))));
    }

    default:
        // Everything else is the guest hypervisor's, which is KVM's
        // default in `nested_vmx_l1_wants_exit` and is the safe direction:
        // the exits that reach here unconditionally - triple fault, task
        // switch, CPUID, INVD, XSETBV, every VMX instruction, invalid
        // guest state - are all ones it must see, and reflecting one it
        // did not expect is an error it can report where absorbing one it
        // was waiting for is a hang.
        //
        // The VMX instructions being reflected is what makes three levels
        // of nesting work: a guest hypervisor emulating them for its own
        // guest gets them, exactly as this VMM gets them from the level
        // above.
        return true;
    }
}

void hypervisor::save_l2_state(std::size_t cpu)
{
    // Two histograms, sampled on every second-level exit, because the
    // whole investigation so far has read one value at one instruction
    // and generalised from it.
    //
    // `interrupt_request_vtpr` samples the task priority only when the
    // guest writes the synthetic interrupt command, which is a request
    // made *at* DISPATCH_LEVEL - so of course every sample said
    // DISPATCH_LEVEL. It cannot say what the distribution is, and the
    // distribution is the question: a guest that never returns to
    // PASSIVE_LEVEL and a guest that returns constantly look identical
    // through that keyhole.
    //
    // The privilege level is here for a blunter reason. The condition
    // this VMM is being built to meet is a guest that reaches user
    // mode, and nothing in the tree has ever counted whether it does.
    // Ring 3 entries appearing at all is the difference between "slow"
    // and "never got there".
    if (cpu < max_cpus) {
        auto selector = this->vmcs.guest_cs_selector();
        this->l2_cpl_seen[cpu][selector & 3] += 1;

        constexpr std::uint64_t virtual_task_priority_offset = 0x80;
        auto page = this->nested_virtual_apic_address[cpu];

        if (0 != page) {
            std::uint8_t vtpr{};
            static_cast<void>(read_guest_physical(
                page + virtual_task_priority_offset,
                std::span(reinterpret_cast<std::byte *>(&vtpr),
                          sizeof(vtpr))));
            this->l2_vtpr_class_seen[cpu][vtpr >> 4] += 1;

            // SDM 27.6.7 and 30.1.2: a TPR-below-threshold exit is
            // owed when bits 3:0 of the threshold **exceed** bits 7:4
            // of the virtual task priority - exceed, not reach, so a
            // threshold of 2 against 0x20 is false. Here, because this
            // is the one place the read is known to work.
            //
            // Often true with no reason-43 exit following means what
            // reaches vmcs02 is not doing what the VMCS says, and the
            // fault is here. Never true means the guest hypervisor
            // only arms the threshold while its guest is already above
            // it, the check is correctly silent, and delivery can only
            // come from TPR virtualization during execution.
            if (auto threshold = this->nested_tpr_threshold[cpu];
                0 != threshold) {
                if ((threshold & 0xf) > (std::uint64_t{vtpr} >> 4)) {
                    this->l2_tpr_would_fire[cpu] =
                        this->l2_tpr_would_fire[cpu] + 1;
                } else {
                    this->l2_tpr_armed_above[cpu] =
                        this->l2_tpr_armed_above[cpu] + 1;
                }
            }
        }
    }

    // Phase timing; see `phase_cycles`.
    auto phase_start = arch::x86_64::rdtsc();
    auto phase_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][0] +=
                arch::x86_64::rdtsc() - phase_start;
            this->phase_calls[cpu][0] += 1;
        }
    });

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Everything unconditional first, in the same order it was loaded, so
    // that the two lists cannot drift apart.
    static_assert(std::size(guest_state_fields) <= 48,
                  "guest_state_cache is too small for the field list");

    {
        std::size_t index{};
        for (auto guest_field : guest_state_fields) {
            auto value = vmcs.read(guest_field);
            shadow.write(guest_field, value);

            // What vmcs02 actually holds, which is the only thing the
            // elision in build_vmcs02 may compare against. See
            // `guest_state_cache`.
            if (cpu < max_cpus) {
                this->guest_state_cache[cpu][index] = value;
            }
            ++index;
        }
        if (cpu < max_cpus) {
            this->guest_state_fresh[cpu] = true;
        }
    }

    shadow.write(field::guest_rip, vmcs.guest_rip());
    shadow.write(field::guest_rsp, vmcs.guest_rsp());
    shadow.write(field::guest_rflags, vmcs.guest_rflags());
    auto interruptibility12 =
        vmcs.read(field::guest_interruptibility_state);

    // The activity state is reconstructed rather than read back, for the
    // states this VMM holds outside the VMCS.
    //
    // A second-level guest that ran is described by the field, and SDM
    // 30.3 saves it there; one that never ran is described only by
    // `l2_activity_state`, because `enter_or_park_l2` held it in root
    // operation instead of entering. `running_l2` is exactly that
    // distinction: it is set by the entry and cleared below.
    //
    // KVM composes the same field the same way and from the same kind of
    // record - `sync_vmcs02_to_vmcs12` writes HLT or wait-for-SIPI out of
    // `mp_state` and active otherwise
    // (.references/kvm/nested.c:4539-4544) - never out of hardware. It
    // can afford to: the only values it ever writes into the real field
    // are active, at reset and to undo a HLT hardware entered on its own
    // (.references/kvm/vmx.c:4922 and 1827, which are the only writes in
    // the tree).
    auto activity12 = this->running_l2[cpu]
                          ? vmcs.read(field::guest_activity_state)
                          : this->l2_activity_state[cpu];

    // The two fields above are a pair, and until this they were composed
    // as though they were not.
    //
    // SDM 29.3.1.5: "The activity-state field must indicate the active
    // state if the interruptibility-state field indicates blocking by
    // either MOV-SS or by STI (if either bit 0 or bit 1 in that field is
    // 1)" (.references/sdm.txt:202605). It is a check on VM *entry*, and
    // the entry that would fail it is the guest hypervisor's own next
    // VMRESUME of the vmcs12 being written here - so the cost of getting
    // it wrong lands one layer up, on an entry whose state the guest
    // hypervisor did not compose and cannot diagnose. That is the worst
    // shape a fault can have in this tree, and it is why this is a guard
    // rather than an analysis.
    //
    // The two came from unrelated places. Interruptibility is always
    // hardware's. Activity is hardware's only while `running_l2`, and
    // `l2_activity_state`'s otherwise - and that one is written by
    // `enter_or_park_l2` *before* it decides whether to enter, so a pass
    // that records a state and then holds the processor in root operation
    // leaves the two free to disagree. Hardware alone cannot produce the
    // forbidden pair, since VM entry applied this same rule and a
    // processor that is halted or waiting for a start-up IPI executes
    // nothing that could raise a shadow.
    //
    // Resolved by clearing the blocking bits rather than by forcing the
    // activity state to active, because only one of those is true: a
    // processor in HLT or wait-for-SIPI has retired no instruction, so it
    // has no shadow, and the stale value read out of vmcs02 is the half
    // that is wrong. Forcing activity to active would instead tell the
    // guest hypervisor its processor was running, which is the thing
    // `enter_or_park_l2` exists to avoid saying.
    constexpr std::uint64_t blocking_by_sti_or_mov_ss = 0x3;

    if (arch::x86_64::vmx::activity_state::active != activity12) {
        interruptibility12 &= ~blocking_by_sti_or_mov_ss;
    }

    shadow.write(field::guest_interruptibility_state, interruptibility12);
    shadow.write(field::guest_activity_state, activity12);

    // The control registers, put back through the same masks they were
    // built with. What the second-level guest owns is the real register;
    // what its hypervisor owns is what it last wrote into vmcs12, and
    // saving the real value over that would tell it its own masked bits
    // had changed underneath it. KVM's `vmcs12_guest_cr0` and
    // `vmcs12_guest_cr4` compose the same two halves.
    auto cr0_mask12 = shadow.read(field::cr0_guest_host_mask);
    auto cr4_mask12 = shadow.read(field::cr4_guest_host_mask);

    shadow.write(field::guest_cr0,
                 (vmcs.guest_cr0() & ~cr0_mask12) |
                     (shadow.read(field::guest_cr0) & cr0_mask12));

    // VMXE is removed on the way back for the same reason it was forced in
    // on the way out: the bit is this VMM's, and a guest hypervisor that
    // never set it in its own guest's CR4 must not find it there.
    shadow.write(field::guest_cr4,
                 ((vmcs.guest_cr4() & ~cr4_vmxe) & ~cr4_mask12) |
                     (shadow.read(field::guest_cr4) & cr4_mask12));

    // "IA-32e mode guest" is a guest state bit wearing a control's
    // clothing, and SDM 30.3 has a VM exit update it. KVM says the same
    // in `sync_vmcs02_to_vmcs12`.
    shadow.write(
        field::vm_entry_controls,
        (shadow.read(field::vm_entry_controls) & ~entry_ia32e_mode_guest) |
            (vmcs.vm_entry_controls() & entry_ia32e_mode_guest));

    // The three saved conditionally, on the guest hypervisor's own exit
    // controls. SDM 30.4, "Saving MSRs", and SDM 30.3 for DR7.
    auto exit12 = shadow.read(field::vm_exit_controls);

    if (0 != (exit12 & exit_save_debug_controls)) {
        shadow.write(field::guest_dr7, vmcs.guest_dr7());
        shadow.write(field::guest_ia32_debugctl,
                     vmcs.read(field::guest_ia32_debugctl));
    }

    if (0 != (exit12 & exit_save_ia32_pat)) {
        shadow.write(field::guest_ia32_pat,
                     vmcs.read(field::guest_ia32_pat));
    }

    if (0 != (exit12 & exit_save_ia32_efer)) {
        shadow.write(field::guest_ia32_efer,
                     vmcs.read(field::guest_ia32_efer));
    }

    // The other half of carrying it in. Unconditional for the same
    // reason the load above is: there is no "save IA32_BNDCFGS" exit
    // control to test - the processor always writes the field on exit -
    // so a guest hypervisor reading it back expects what its guest left
    // there, and anything else silently loses the guest's bounds
    // configuration across every exit.
    shadow.write(field::guest_ia32_bndcfgs,
                 vmcs.read(field::guest_ia32_bndcfgs));
}

void hypervisor::host_write(std::size_t cpu,
                            field which,
                            std::uint64_t value)
{
    // Recorded in call order, which is what makes the index stable: the
    // sequence of writes below is fixed by the code and not by the
    // guest, so slot N is the same field on every call and the audit
    // above can compare across reflections without carrying a lookup.
    if (cpu < max_cpus) {
        if (auto index = this->l1_host_written[cpu];
            index < l1_host_field_count) {
            this->l1_host_field[cpu][index] =
                static_cast<std::uint64_t>(which);
            this->l1_host_value[cpu][index] = value;
        }

        this->l1_host_written[cpu] = this->l1_host_written[cpu] + 1;
    }

    this->vmcs.write(which, value);
}

void hypervisor::load_l1_host_state(std::size_t cpu)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // One field of the previous call's table, checked against what
    // vmcs01 holds now, before anything below overwrites it.
    //
    // This function is 14.1% of the wall clock in fifty-two VMWRITEs
    // that almost all restate a constant or an unchanged vmcs12 host
    // field, and it cannot be elided against a cache of what was last
    // written: SDM 30.3.2 has every VM exit save the guest hypervisor's
    // own segment bases, limits and access rights over them, so the
    // cache describes something the processor has since overwritten.
    // That is exactly what killed `ZPP_LAZY_GUEST_STATE` one VMCS over.
    //
    // What *would* justify eliding a particular field is knowing the
    // processor never actually changes it - and that is a measurement,
    // not an assumption about what Hyper-V's exit path does to its own
    // registers. This tree has been wrong three times running about what
    // may be assumed of that guest, so it is measured instead.
    //
    // One field per reflection, round robin, rather than the whole table
    // every few thousand. It costs a single VMREAD on a path that makes
    // a hundred - about 1% - and it samples each field at ten thousand
    // different moments over a boot instead of at a handful, which is
    // the difference between "stable when I looked" and "stable".
    if (cpu < max_cpus) {
        if (auto recorded = this->l1_host_count[cpu]; 0 != recorded) {
            auto index = this->l1_host_audits[cpu] % recorded;

            auto now = vmcs.read(
                static_cast<field>(this->l1_host_field[cpu][index]));

            if (now != this->l1_host_value[cpu][index]) {
                this->l1_host_changed[cpu][index] += 1;
            }

            this->l1_host_audits[cpu] = this->l1_host_audits[cpu] + 1;
        }

        this->l1_host_written[cpu] = 0;
    }

    auto exit12 = shadow.read(field::vm_exit_controls);

    auto host_cr0_12 = shadow.read(field::host_cr0);
    auto host_cr4_12 = shadow.read(field::host_cr4);

    host_write(cpu, field::guest_cr0, host_cr0_12);
    host_write(cpu, field::guest_cr3, shadow.read(field::host_cr3));
    host_write(cpu, field::guest_cr4, host_cr4_12 | cr4_vmxe);

    // The read shadows have to follow, or the guest hypervisor reads back
    // the state of its own guest. CR4's is the one that matters: VMXE is
    // in this VMM's mask and forced into the register above, so without
    // this the guest hypervisor would read a CR4 it never wrote.
    host_write(cpu, field::cr0_read_shadow, host_cr0_12);
    host_write(cpu, field::cr4_read_shadow, host_cr4_12);

    host_write(cpu, field::guest_rip, shadow.read(field::host_rip));
    host_write(cpu, field::guest_rsp, shadow.read(field::host_rsp));

    // SDM 30.5.3 (.references/sdm.txt:204918): "RFLAGS is cleared,
    // except bit 1, which is always set".
    constexpr std::uint64_t rflags_reserved_one = 1ull << 1;
    host_write(cpu, field::guest_rflags, rflags_reserved_one);

    // SDM 30.5.5 (.references/sdm.txt:204944): "There is no blocking by
    // STI or by MOV SS after a VM exit", and the activity state is
    // active. A hypervisor arriving at its own exit handler is running.
    host_write(cpu, field::guest_interruptibility_state, 0);
    host_write(cpu,
               field::guest_activity_state,
               arch::x86_64::vmx::activity_state::active);
    host_write(cpu, field::guest_pending_debug_exceptions, 0);

    // SDM 30.5.2, "Loading Host Segment and Descriptor-Table Registers"
    // (.references/sdm.txt:204861).
    // The selectors come from the host-state area; everything else about
    // each segment is fixed by the architecture rather than stored, which
    // is why these are constants rather than copies.
    //
    // Access-rights encoding, SDM Table 25-2: bits 3:0 type, bit 4 S, bits
    // 6:5 DPL, bit 7 P, bit 13 L, bit 14 D/B, bit 15 G, bit 16 unusable.
    constexpr std::uint64_t code_access_long = 0xa09b;
    constexpr std::uint64_t code_access_legacy = 0xc09b;
    constexpr std::uint64_t data_access = 0xc093;
    constexpr std::uint64_t task_access = 0x008b;
    constexpr std::uint64_t unusable_access = 0x10000;
    constexpr std::uint64_t flat_limit = 0xffffffff;
    constexpr std::uint64_t task_limit = 0x67;
    constexpr std::uint64_t descriptor_table_limit = 0xffff;

    auto in_ia32e_mode = 0 != (exit12 & exit_host_address_space_size);

    host_write(cpu,
               field::guest_cs_selector,
               shadow.read(field::host_cs_selector));
    host_write(cpu, field::guest_cs_base, 0);
    host_write(cpu, field::guest_cs_limit, flat_limit);
    host_write(cpu,
               field::guest_cs_access_rights,
               in_ia32e_mode ? code_access_long : code_access_legacy);

    // The five data segments, all with the same fixed shape. FS and GS
    // are the two exceptions and only in the base, which the host-state
    // area has to carry because a long-mode descriptor cannot.
    struct data_segment
    {
        field selector;
        field vmcs_selector;
        field vmcs_base;
        field vmcs_limit;
        field vmcs_access;
        std::uint64_t base;
    };

    const data_segment data_segments[] = {
        {field::host_ss_selector,
         field::guest_ss_selector,
         field::guest_ss_base,
         field::guest_ss_limit,
         field::guest_ss_access_rights,
         0},
        {field::host_ds_selector,
         field::guest_ds_selector,
         field::guest_ds_base,
         field::guest_ds_limit,
         field::guest_ds_access_rights,
         0},
        {field::host_es_selector,
         field::guest_es_selector,
         field::guest_es_base,
         field::guest_es_limit,
         field::guest_es_access_rights,
         0},
        {field::host_fs_selector,
         field::guest_fs_selector,
         field::guest_fs_base,
         field::guest_fs_limit,
         field::guest_fs_access_rights,
         shadow.read(field::host_fs_base)},
        {field::host_gs_selector,
         field::guest_gs_selector,
         field::guest_gs_base,
         field::guest_gs_limit,
         field::guest_gs_access_rights,
         shadow.read(field::host_gs_base)},
    };

    for (const auto & segment : data_segments) {
        host_write(
            cpu, segment.vmcs_selector, shadow.read(segment.selector));
        host_write(cpu, segment.vmcs_base, segment.base);
        host_write(cpu, segment.vmcs_limit, flat_limit);
        host_write(cpu, segment.vmcs_access, data_access);
    }

    host_write(cpu,
               field::guest_tr_selector,
               shadow.read(field::host_tr_selector));
    host_write(
        cpu, field::guest_tr_base, shadow.read(field::host_tr_base));
    host_write(cpu, field::guest_tr_limit, task_limit);
    host_write(cpu, field::guest_tr_access_rights, task_access);

    // LDTR is unusable after a VM exit, whatever it was.
    host_write(cpu, field::guest_ldtr_selector, 0);
    host_write(cpu, field::guest_ldtr_base, 0);
    host_write(cpu, field::guest_ldtr_limit, 0);
    host_write(cpu, field::guest_ldtr_access_rights, unusable_access);

    host_write(
        cpu, field::guest_gdtr_base, shadow.read(field::host_gdtr_base));
    host_write(cpu, field::guest_gdtr_limit, descriptor_table_limit);
    host_write(
        cpu, field::guest_idtr_base, shadow.read(field::host_idtr_base));
    host_write(cpu, field::guest_idtr_limit, descriptor_table_limit);

    host_write(cpu,
               field::guest_ia32_sysenter_cs,
               shadow.read(field::host_ia32_sysenter_cs));
    host_write(cpu,
               field::guest_ia32_sysenter_esp,
               shadow.read(field::host_ia32_sysenter_esp));
    host_write(cpu,
               field::guest_ia32_sysenter_eip,
               shadow.read(field::host_ia32_sysenter_eip));

    // SDM 30.5.1 (.references/sdm.txt:204807): "DR7 is set to 400H", and
    // IA32_DEBUGCTL to 0 two lines below it.
    constexpr std::uint64_t dr7_after_exit = 0x400;
    host_write(cpu, field::guest_dr7, dr7_after_exit);
    host_write(cpu, field::guest_ia32_debugctl, 0);

    // IA32_PAT and IA32_EFER are written to the *registers* rather than to
    // the guest-state area, and that is not a shortcut - it is the only
    // thing that works here. This VMM's own VM-entry controls do not load
    // either, so a value put in the field would never be read; the
    // architecture's "load IA32_PAT on VM exit" means the register, and
    // the register is what the guest hypervisor will be running with.
    //
    // The alternative - adding the two load controls to this VMM's own
    // VMCS - was rejected because it changes the VMCS that runs the
    // ordinary guest, which is the code path a working Windows boot
    // depends on, for the sake of a path that only exists with nested VMX
    // switched on.
    // The table is complete from here, so a reader that sees a non-zero
    // count sees a table whose every entry was filled by this call.
    if (cpu < max_cpus) {
        this->l1_host_count[cpu] = this->l1_host_written[cpu];
    }

    if (0 != (exit12 & exit_load_ia32_pat)) {
        arch::x86_64::wrmsr(arch::x86_64::msr::ia32_pat,
                            shadow.read(field::host_ia32_pat));
    }

    if (0 != (exit12 & exit_load_ia32_efer)) {
        arch::x86_64::wrmsr(
            arch::x86_64::msr::ia32_extended_feature_enable,
            shadow.read(field::host_ia32_efer));
    } else {
        // LMA and LME are not part of that control's remit. SDM 30.5.1,
        // ".references/sdm.txt:204822": "The LMA and LME bits in the
        // IA32_EFER MSR are each loaded with the setting of the 'host
        // address-space size' VM-exit control" - unconditionally, on
        // every VM exit, whether or not the whole MSR is being loaded.
        //
        // Today this is masked by an accident: vmcs02 inherits this
        // VMM's own exit controls, which do carry host address-space
        // size, so the hardware exit has already put both bits back to
        // one before this runs. It breaks for a 32-bit guest hypervisor
        // - one whose vmcs12 clears the control - which would be
        // resumed in long mode with a host state that says otherwise.
        //
        // KVM writes the same three ways round in load_vmcs12_host_state:
        // the load control if set, otherwise LMA|LME from the
        // host-address-space-size bit, otherwise cleared.
        constexpr std::uint64_t efer_lme = 1ull << 8;
        constexpr std::uint64_t efer_lma = 1ull << 10;

        auto efer = arch::x86_64::rdmsr(
            arch::x86_64::msr::ia32_extended_feature_enable);

        if (0 != (exit12 & exit_host_address_space_size)) {
            efer |= (efer_lme | efer_lma);
        } else {
            efer &= ~(efer_lme | efer_lma);
        }

        arch::x86_64::wrmsr(
            arch::x86_64::msr::ia32_extended_feature_enable, efer);
    }
}

hypervisor::l2_entry_outcome hypervisor::enter_or_park_l2(std::size_t cpu)
{
    namespace activity = arch::x86_64::vmx::activity_state;

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // SDM 29.8 gives 33 for "VM-entry failure due to invalid guest state"
    // and sets bit 31 of the exit reason to say a VM entry failed. The
    // qualification is zero: 29.8 lists the four non-zero values and none
    // of them is the activity state, so "In most cases, the exit
    // qualification is cleared to 0" applies. KVM writes the same pair -
    // EXIT_REASON_INVALID_STATE with ENTRY_FAIL_DEFAULT, which is 0
    // (.references/kvm/nested.c:3568-3572).
    constexpr std::uint64_t entry_failure = 1ull << 31;
    auto refuse = [&](const char * why, std::uint64_t what) {
        log("cpu {} second level entry refused: {} ({})", cpu, why, what);
        reflect_l2_exit(cpu,
                        entry_failure |
                            static_cast<std::uint64_t>(
                                basic_reason::entry_invalid_guest_state),
                        0);

        // After the reflection, never before it: save_l2_state writes
        // this into vmcs12, and the record has to describe what the
        // processor is doing *now*, which is running the guest
        // hypervisor's own code.
        this->l2_activity_state[cpu] = activity::active;
        return l2_entry_outcome::reflected;
    };

    auto activity12 = shadow.read(field::guest_activity_state);

    // The first check of SDM 29.3.1.5, "Checks on Guest Non-Register
    // State": the field must name an activity state the implementation
    // supports, and IA32_VMX_MISC is where software is told which those
    // are. This VMM reports HLT and wait-for-SIPI and not shutdown - see
    // nested_vmx_capability_msr - so those three plus active are the
    // whole of what may be accepted. KVM's set is identical, in
    // `nested_check_guest_non_reg_state`
    // (.references/kvm/nested.c:3117-3119).
    if ((activity::active != activity12) &&
        (activity::hlt != activity12) &&
        (activity::wait_for_start_up_ipi != activity12)) {
        return refuse("activity state not supported", activity12);
    }

    this->l2_activity_state[cpu] = activity12;

    if (activity::wait_for_start_up_ipi != activity12) {
        // Active and HLT are entered in hardware, and HLT deliberately so.
        //
        // Forcing HLT to active would be worse than wrong, it would be
        // loud: a halted second-level guest would start executing
        // instructions at whatever RIP its hypervisor left in vmcs12.
        // KVM can force it because it has somewhere to put the vCPU -
        // `kvm_emulate_halt_noskip` blocks the thread until an event
        // arrives (.references/kvm/nested.c:3766-3779). This VMM has no
        // scheduler and nothing to block on: the event that ends a halt
        // is a physical interrupt or NMI, its host runs with interrupts
        // disabled and sets no "external-interrupt exiting", so root
        // operation can neither observe one nor deliver it.
        //
        // Nothing is lost by letting the processor sit there, and SDM
        // 29.7.2's own list is why: the active state and the HLT state
        // block exactly the same events - start-up IPIs and nothing else.
        // A processor in HLT still takes external interrupts and NMIs,
        // which is precisely how its hypervisor gets it back. That is not
        // true of the other two inactive states, which is why only they
        // are refused or held.
        vmcs.write(field::guest_activity_state, activity12);
        return l2_entry_outcome::entered;
    }

    // Wait-for-SIPI, which is the one this VMM must not hand to hardware.
    //
    // SDM 29.7.2: it "blocks external interrupts, non-maskable interrupts
    // (NMIs), INIT signals, and system-management interrupts (SMIs). Such
    // events do not cause VM exits if they arrive while a logical
    // processor is in the wait-for-SIPI state and in VMX non-root
    // operation" (.references/sdm.txt:203152). Only a start-up IPI ends
    // it. So a processor entered in it is gone as far as this VMM is
    // concerned - and unlike bare metal there is no guarantee it will
    // ever be sent one, because this VMM intercepts the guest's write to
    // the interrupt command register and may answer it itself.
    //
    // Two checks first, and they exist *because* the state is not handed
    // to hardware: the processor would have made them, and forcing the
    // field means it no longer does. Both are SDM 29.3.1.5 - "The
    // activity-state field must indicate the active state if the
    // interruptibility-state field indicates blocking by either MOV-SS or
    // by STI", and, for an entry that is injecting, "Wait-for-SIPI. No
    // events are allowed."
    constexpr std::uint64_t blocking_by_sti_or_mov_ss = 0x3;

    if (0 != (shadow.read(field::guest_interruptibility_state) &
              blocking_by_sti_or_mov_ss)) {
        return refuse("wait-for-sipi with blocking by sti or mov ss",
                      shadow.read(field::guest_interruptibility_state));
    }

    if (0 != (shadow.read(field::vm_entry_interruption_information_field) &
              interruption_valid)) {
        return refuse(
            "wait-for-sipi with an event to inject",
            shadow.read(field::vm_entry_interruption_information_field));
    }

    // Waited for in root operation instead, on the hand-off this VMM
    // already uses for its own processors coming out of an INIT. Reusing
    // it is the point: a sender that sees a target listening there
    // swallows the guest's write and hands the vector over, which is the
    // only delivery that works while the target is in root mode.
    if (auto vector = wait_for_l2_start_up_ipi(cpu)) {
        // What the hardware would have given the guest hypervisor: SDM
        // 28.2 makes a start-up IPI arriving in the wait-for-SIPI state a
        // VM exit, with the vector in the exit qualification. KVM
        // synthesises the identical exit from its own record of the same
        // state - `vmx_check_nested_events` reflects
        // EXIT_REASON_SIPI_SIGNAL with `apic->sipi_vector & 0xFF` when
        // mp_state is KVM_MP_STATE_INIT_RECEIVED
        // (.references/kvm/nested.c:4240-4243).
        //
        // vmcs12's activity state stays at wait-for-SIPI across it, which
        // save_l2_state does out of l2_activity_state above - and is what
        // KVM saves too, since the exit does not clear mp_state.
        log("cpu {} second level start-up ipi, vector {}", cpu, *vector);

        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::start_up_ipi),
            *vector);

        // And the record goes back to describing this processor rather
        // than the guest it was holding, again after the reflection so
        // that vmcs12 keeps the wait-for-SIPI above. It is read by
        // start_up_processor, which must not hand a second vector to a
        // mailbox nobody is spinning on any more - the processor is
        // running the guest hypervisor's exit handler now.
        this->l2_activity_state[cpu] = activity::active;
        return l2_entry_outcome::reflected;
    }

    // Nothing yet. Back to the guest hypervisor with its own VMCS current
    // and RIP still on the VMLAUNCH, so it executes it again and this
    // decision is taken afresh.
    //
    // Not a spin that goes nowhere. It is what keeps this processor
    // reachable: every pass runs the exit handler, so the diagnostic
    // channel is fed, the poll is re-armed and the log records that this
    // processor is parked rather than lost. The alternative - waiting
    // here for ever - is the same darkness the hardware state produces,
    // only in root mode.
    auto region = own_vmcs_region_physical();
    if ((0 == region) || arch::x86_64::vmx::vmptrld(&region)) {
        // Same reasoning as reflect_l2_exit: without its own VMCS there is
        // no guest hypervisor left to go back to.
        __builtin_trap();
    }

    // Counted rather than logged. A log line per pass would be thousands
    // a second and would evict everything else in the ring; a count says
    // the same thing and says it about every pass. See the declaration
    // for what reading it settles.
    this->l2_start_up_waits[cpu] = this->l2_start_up_waits[cpu] + 1;

    return l2_entry_outcome::retry;
}

void hypervisor::reflect_l2_exit(std::size_t cpu,
                                 arch::x86_64::vmx::exit_reason reason,
                                 std::uint64_t qualification)
{
    // Phase timing; see `phase_cycles`.
    auto reflect_start = arch::x86_64::rdtsc();
    auto reflect_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][1] +=
                arch::x86_64::rdtsc() - reflect_start;
            this->phase_calls[cpu][1] += 1;
        }
    });

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Recorded here, first, and for the same reason the guest state is
    // read here: this is the last moment the VMCS that ran the
    // second-level guest is current, so the instruction pointer below is
    // that guest's own and not the guest hypervisor's.
    //
    // A separate ring from the one every exit goes into, because that one
    // cannot answer this. The guest hypervisor's own traffic drowns it -
    // at the freeze on the rig every one of the boot processor's newest
    // sixteen entries was a write to its local APIC page - so what the
    // root partition was doing has always been evicted by the time there
    // is anything to read.
    if (cpu < max_cpus) {
        auto msr_index = this->l2_exit_detail[cpu];
        auto & count = this->l2_exit_trace_count[cpu];
        auto & slot =
            this->l2_exit_trace[cpu][count % l2_exit_trace_capacity];

        // The field means what its name says for the two reasons that
        // report an address, and the register number for everything else.
        //
        // It used to mean the register number for all of them, which made
        // the ring's account of a reflected extended-page-table fault
        // read `0x0` - the second-level guest's RCX, which is not
        // interesting and is not what the field is documented to hold.
        // A whole boot's worth of faults at one instruction pointer was
        // read that way before it was noticed that the one datum needed to
        // identify the page was being overwritten with a register.
        auto reports_an_address =
            (basic_reason::ept_violation == reason.basic()) ||
            (basic_reason::ept_misconfiguration == reason.basic());

        slot = exit_trace_entry{
            .reason = reason.value(),
            .qualification = qualification,
            .activity_state = vmcs.guest_activity_state(),
            .cs_selector = vmcs.guest_cs_selector(),
            .rip = vmcs.guest_rip(),
            .guest_physical = reports_an_address
                                  ? vmcs.guest_physical_address()
                                  : this->l2_exit_detail[cpu],
            .repeated = 1,
            .detail_value = this->l2_exit_detail_value[cpu],
            .detail = this->l2_entries[cpu],
        };

        count = count + 1;

        // The same record again, in a ring the idle loop cannot reach.
        //
        // The ring above is 256 deep and that is not deep enough for the
        // question that matters now. A second-level guest waiting for
        // something spins its idle loop at about a hundred exits a
        // second - the reference-clock read, the end-of-interrupt, the
        // end-of-message, the timer re-arm - so whatever it did *before*
        // it started waiting is evicted within seconds, and the failure
        // being chased is minutes old by the time anything reads it.
        //
        // Filtered by what the loop is made of rather than by where it
        // is: the addresses move with every boot, the synthetic MSR
        // indices do not. What is left is the work.
        constexpr std::uint32_t reference_count_msr = 0x40000020;
        constexpr std::uint32_t synthetic_eoi_msr = 0x40000070;
        constexpr std::uint32_t synthetic_icr_msr = 0x40000071;
        constexpr std::uint32_t end_of_message_msr = 0x40000084;
        constexpr std::uint32_t synthetic_timer_count_msr = 0x400000b1;

        auto index = static_cast<std::uint32_t>(msr_index);
        auto is_idle_msr = ((basic_reason::rdmsr == reason.basic()) ||
                            (basic_reason::wrmsr == reason.basic())) &&
                           ((reference_count_msr == index) ||
                            (synthetic_eoi_msr == index) ||
                            (synthetic_icr_msr == index) ||
                            (end_of_message_msr == index) ||
                            (synthetic_timer_count_msr == index));

        // External interrupts go too, and they are most of what the
        // first version of this ring caught: the timer arrives *during*
        // the reference-clock poll, so an idle guest produces one of
        // these per tick and they filled 4096 slots with the same
        // instruction pointer. Nothing is lost by dropping them here -
        // `l2_external_vector` already counts every one by vector, which
        // is the question they answer. This ring answers a different
        // one: what the guest was *doing*.
        if (!is_idle_msr &&
            (basic_reason::external_interrupt != reason.basic()) &&
            (basic_reason::interrupt_window != reason.basic())) {
            auto & working = this->l2_working_trace_count[cpu];

            this->l2_working_trace[cpu][working %
                                        l2_working_trace_capacity] = slot;
            working = working + 1;
        }

        // Cleared, so an exit that carries no register number shows zero
        // rather than the last one that did.
        this->l2_exit_detail[cpu] = 0;
        this->l2_exit_detail_value[cpu] = 0;
    }

    // The interrupted event becomes the guest hypervisor's, not ours.
    //
    // Below, this reflection copies the interrupted-event field into its
    // VMCS, which is the architecture's way of telling it that a delivery
    // it started did not finish - so it is the one that decides what
    // happens next. Re-injecting it here as well would deliver the same
    // event twice, once by each level.
    if (cpu < max_cpus) {
        this->pending_event[cpu] = 0;
    }

    // The guest state first, while the VMCS that ran the second-level
    // guest is still current - unless this is a VM-entry failure, which
    // saves none of it.
    //
    // SDM 29.8 lists what an entry failure does *not* do, and the third
    // item is "The guest-state area is not modified." KVM's is the same
    // shape: `nested_vmx_enter_non_root_mode`'s `vmentry_fail_vmexit`
    // label calls `load_vmcs12_host_state` and writes the exit reason,
    // and reaches no `sync_vmcs02_to_vmcs12` on that path
    // (.references/kvm/nested.c:3640-3651).
    //
    // Not a tidy-up. It is what makes an entry failure reflectable from
    // a point where the second-level guest never ran and vmcs02 may not
    // even be the current VMCS: save_l2_state reads whichever VMCS *is*
    // current, so on that path it would copy the guest hypervisor's own
    // registers into its guest's state area.
    if (!reason.entry_failure()) {
        save_l2_state(cpu);
    }

    shadow.write(field::exit_reason, reason.value());
    shadow.write(field::exit_qualification, qualification);

    // Phase timing for the exit-information block below; see
    // `phase_cycles`. Split out from `reflect_l2_exit` as a whole because
    // what is left of that function after `save_l2_state` and
    // `load_l1_host_state` is 26% of the wall clock and was attributed to
    // nothing - and eight of the VMREADs in it are unconditional where
    // the architecture defines only three of them for most exits.
    auto info_start = arch::x86_64::rdtsc();

    if (!reason.entry_failure()) {
        // SDM 33.3, VMLAUNCH: the launch state becomes launched once an
        // entry has completed, and an entry that failed after loading
        // guest state never reached that step. KVM writes the same
        // condition in `prepare_vmcs12`.
        shadow.state(vmcs12::launch_state::launched);

        // SDM 30.2: the rest of the exit-information fields are written
        // only for an ordinary exit. On an entry failure the architecture
        // updates the reason and the qualification and leaves the others
        // alone, so writing them would fabricate an account of an
        // instruction that never ran.
        shadow.write(field::guest_linear_address,
                     vmcs.read(field::guest_linear_address));
        shadow.write(field::guest_physical_address,
                     vmcs.read(field::guest_physical_address));
        shadow.write(field::vm_exit_interruption_information,
                     vmcs.read(field::vm_exit_interruption_information));
        shadow.write(field::vm_exit_interruption_error_code,
                     vmcs.read(field::vm_exit_interruption_error_code));
        shadow.write(field::vm_exit_instruction_length,
                     vmcs.read(field::vm_exit_instruction_length));
        shadow.write(field::vm_exit_instruction_information,
                     vmcs.read(field::vm_exit_instruction_information));

        // SDM 30.2.4, "Information for VM Exits During Event Delivery".
        // Copied from the hardware's own report rather than reconstructed,
        // because everything the second-level guest was in the middle of
        // delivering was put there by an entry this VMM built out of
        // vmcs12 - so the processor's account is the guest hypervisor's
        // account. The rule that a double or triple fault is never
        // reported as occurring during delivery is the processor's to
        // apply, and it applied it.
        shadow.write(field::idt_vectoring_information_field,
                     vmcs.read(field::idt_vectoring_information_field));
        shadow.write(field::idt_vectoring_error_code,
                     vmcs.read(field::idt_vectoring_error_code));

        // SDM 30.2: the valid bit of the VM-entry interruption-information
        // field is cleared on every VM exit. Emulated rather than read
        // back, because the field being read back is vmcs02's and the one
        // the guest hypervisor will next look at is vmcs12's.
        shadow.write(
            field::vm_entry_interruption_information_field,
            shadow.read(field::vm_entry_interruption_information_field) &
                ~interruption_valid);
    }

    if (cpu < max_cpus) {
        this->phase_cycles[cpu][13] += arch::x86_64::rdtsc() - info_start;
        this->phase_calls[cpu][13] += 1;
    }

    // SDM 30.4: the VM-exit MSR-store area is processed after the guest
    // state is saved and before host state is loaded, so it reads the
    // values the second-level guest was running with.
    //
    // Skipped on an entry failure for the same reason the guest state is,
    // and from the same list: SDM 29.8's "No MSRs are saved into the
    // VM-exit MSR-store area." The MSR-*load* area below is not on that
    // list - 29.8 step 4 performs it - so only this half moves.
    auto aborted = false;

    if (!reason.entry_failure()) {
        if (auto stored = store_nested_msrs(
                shadow.read(field::vm_exit_msr_store_address),
                shadow.read(field::vm_exit_msr_store_count));
            !stored) {
            aborted = true;
            log("cpu {} could not store the guest hypervisor's exit "
                "msrs: {}",
                cpu,
                stored.error().code());
        }
    }

    // Back onto the VMCS that runs the guest hypervisor.
    //
    // Phase timing; see `phase_cycles`. The pair with the one in
    // build_vmcs02: together they are every VMCS switch a round trip
    // makes, so phases 6 and 7 price the whole of it.
    auto region = own_vmcs_region_physical();
    auto switch_start = arch::x86_64::rdtsc();
    auto switch_failed =
        (0 == region) || arch::x86_64::vmx::vmptrld(&region);

    if (cpu < max_cpus) {
        this->phase_cycles[cpu][7] += arch::x86_64::rdtsc() - switch_start;
        this->phase_calls[cpu][7] += 1;
    }

    if (switch_failed) {
        // Not recoverable: without its own VMCS there is no guest
        // hypervisor to return to and nothing to resume. Same reasoning as
        // vmcs::write.
        __builtin_trap();
    }

    this->running_l2[cpu] = false;

    // Anything queued for the second-level guest is dropped here, which
    // vmcs02's own field holding it makes automatic: the next entry
    // rewrites it from vmcs12, and vmcs12's valid bit was just cleared.
    {
        // Phase timing; see `phase_cycles`.
        auto host_start = arch::x86_64::rdtsc();
        zpp::scope_exit host_stop{[&] {
            if (cpu < max_cpus) {
                this->phase_cycles[cpu][12] +=
                    arch::x86_64::rdtsc() - host_start;
                this->phase_calls[cpu][12] += 1;
            }
        }};

        load_l1_host_state(cpu);
    }

    // SDM 30.6: and the VM-exit MSR-load area after host state, which is
    // why this is here rather than beside the store above.
    if (auto loaded =
            load_nested_msrs(cpu,
                             shadow.read(field::vm_exit_msr_load_address),
                             shadow.read(field::vm_exit_msr_load_count));
        !loaded) {
        aborted = true;
        log("cpu {} could not load the guest hypervisor's exit msrs: {}",
            cpu,
            loaded.error().code());
    }

    nested_transition_flush();

    this->l2_exits_reflected[cpu] = this->l2_exits_reflected[cpu] + 1;

    // So the ring written at the end of this exit says whose instruction
    // pointer it holds. From here the current VMCS is vmcs01 and the
    // guest hypervisor's host state is loaded, so `guest_rip` no longer
    // answers about the guest that faulted.
    this->exit_reflected[cpu] = 1;

    // A failure in either exit area is a VMX abort, SDM 30.4 and SDM 30.6.
    // A real abort is a shutdown, which is not something to do to a whole
    // machine because one guest's hypervisor named a page that stopped
    // being readable - so this takes KVM's answer in `nested_vmx_abort`
    // instead and kills the *guest hypervisor's* virtual machine, by
    // giving it the triple fault its own guest would have taken. That is
    // reachable only through unreadable guest memory: every other reason
    // an entry in one of those areas can fail was checked before the
    // second-level guest ever ran.
    if (aborted) {
        this->guest_vmcs12[cpu].write(
            field::exit_reason,
            static_cast<std::uint64_t>(basic_reason::triple_fault));
        this->guest_vmcs12[cpu].write(field::exit_qualification, 0);
    }

    // Last, after every write above. The guest hypervisor is about to run
    // its exit handler, and the whole point of shadowing is that it reads
    // the exit information out of the shadow region without exiting - so
    // that region has to hold what was just written, and this is the only
    // moment between the writes and the reads.
    //
    // Safe to overwrite the region rather than merge into it: the guest
    // hypervisor has not run since on_guest_vmlaunch collected its silent
    // writes, so the shadow's writable fields and the cache agree.
    copy_vmcs12_to_shadow(cpu);
}

namespace
{
/**
 * The general-purpose register an exit qualification names.
 *
 * Spelled out rather than indexed, for the reason the control-register
 * handler in `exit_dispatch.cpp` gives for doing the same: the encoding
 * is the architecture's register numbering and the context stores them in
 * whatever order its assembly pushed them, so an index into the structure
 * would be right only by coincidence.
 */
std::uint64_t * general_purpose_register(arch::x86_64::context & context,
                                         std::uint64_t number)
{
    switch (number) {
    case 0:
        return &context.rax;
    case 1:
        return &context.rcx;
    case 2:
        return &context.rdx;
    case 3:
        return &context.rbx;
    case 4:
        return &context.rsp;
    case 5:
        return &context.rbp;
    case 6:
        return &context.rsi;
    case 7:
        return &context.rdi;
    case 8:
        return &context.r8;
    case 9:
        return &context.r9;
    case 10:
        return &context.r10;
    case 11:
        return &context.r11;
    case 12:
        return &context.r12;
    case 13:
        return &context.r13;
    case 14:
        return &context.r14;
    case 15:
        return &context.r15;
    default:
        return nullptr;
    }
}

} // namespace

bool hypervisor::on_nested_cr8_access(std::size_t cpu,
                                      std::uint64_t qualification,
                                      arch::x86_64::context & context,
                                      bool & advance_rip)
{
    // SDM Table 28-3: bits 3:0 the register, bits 5:4 the access type -
    // 0 is MOV to, 1 is MOV from - and bits 11:8 the general-purpose
    // register.
    constexpr std::uint64_t register_mask = 0xf;
    constexpr std::uint64_t access_shift = 4;
    constexpr std::uint64_t access_mask = 0x3;
    constexpr std::uint64_t gpr_shift = 8;
    constexpr std::uint64_t gpr_mask = 0xf;
    constexpr std::uint64_t control_register_8 = 8;
    constexpr std::uint64_t access_move_to = 0;
    constexpr std::uint64_t access_move_from = 1;

    // SDM 30.1.1 again: VTPR is the byte at offset 0x80 on the page.
    constexpr std::uint64_t virtual_task_priority_offset = 0x80;

    // SDM 27.6.8: the threshold is compared against bits 7:4 of VTPR -
    // the priority *class* - not against the whole byte.
    constexpr std::uint64_t priority_class_shift = 4;

    if ((cpu >= max_cpus) || !this->running_l2[cpu]) {
        return false;
    }

    auto page = this->nested_virtual_apic_address[cpu];
    if ((control_register_8 != (qualification & register_mask)) ||
        (0 == page)) {
        return false;
    }

    auto access = (qualification >> access_shift) & access_mask;
    auto gpr = (qualification >> gpr_shift) & gpr_mask;

    auto slot = general_purpose_register(context, gpr);
    if (nullptr == slot) {
        return false;
    }

    std::uint8_t vtpr{};
    auto at = page + virtual_task_priority_offset;

    if (access_move_from == access) {
        if (auto read = read_guest_physical(
                at,
                std::span(reinterpret_cast<std::byte *>(&vtpr),
                          sizeof(vtpr)));
            !read) {
            return false;
        }

        // CR8 is the priority *class*, which is VTPR's high nibble. A
        // guest reading back what it wrote depends on this being the
        // inverse of the write below, and a guest whose CR8 reads four
        // bits too large raises its own interrupt priority every time it
        // saves and restores one.
        *slot = vtpr >> priority_class_shift;
        this->nested_cr8_reads[cpu] = this->nested_cr8_reads[cpu] + 1;
        return true;
    }

    if (access_move_to != access) {
        return false;
    }

    constexpr std::uint64_t priority_class_mask = 0xf;

    // SDM 2.5, CR8: "Reserved bits ... must be written with zeros.
    // Writing a nonzero value to these bits will cause a
    // general-protection exception." The guest is given the fault
    // hardware would have given it rather than having the value
    // silently truncated, which is the rule this project applies to
    // everything else it emulates.
    if (0 != (*slot & ~priority_class_mask)) {
        inject_general_protection_fault();
        advance_rip = false;
        return true;
    }

    vtpr = static_cast<std::uint8_t>((*slot & priority_class_mask)
                                     << priority_class_shift);

    if (auto written = write_guest_physical(
            at,
            std::span(reinterpret_cast<const std::byte *>(&vtpr),
                      sizeof(vtpr)));
        !written) {
        return false;
    }

    this->nested_cr8_writes[cpu] = this->nested_cr8_writes[cpu] + 1;

    // And the exit the processor would have raised. SDM 27.6.8: the
    // TPR-below-threshold exit occurs "if the value of bits 3:0 of the
    // TPR threshold VM-execution control field is greater than the value
    // of bits 7:4 of VTPR".
    //
    // Emulating the write and not this would be the worst of both: the
    // guest's priority would fall, the guest hypervisor would never be
    // told, and every interrupt it was holding would stay held. That is
    // the failure this switch exists to test for, so producing it here by
    // omission would make the experiment answer itself.
    constexpr std::uint64_t threshold_mask = 0xf;

    auto threshold = this->nested_tpr_threshold[cpu] & threshold_mask;

    if (threshold > (vtpr >> priority_class_shift)) {
        this->nested_cr8_below_threshold[cpu] =
            this->nested_cr8_below_threshold[cpu] + 1;

        // **The instruction pointer is advanced here, before reflecting,
        // and the caller is told not to advance it again.**
        //
        // This was the other way round and it corrupted the guest
        // hypervisor. `reflect_l2_exit` switches the current VMCS back to
        // vmcs01 and loads that hypervisor's host state, so an advance
        // performed afterwards lands on *its* instruction pointer rather
        // than its guest's - it resumes a few bytes into whatever it was
        // executing, which is a reset a moment later. Measured: four
        // loader boots in one run, and the counter below at one.
        //
        // The advance itself is what the architecture requires anyway.
        // The write has taken effect, so the exit is a trap rather than a
        // fault and the guest hypervisor must see its guest positioned
        // after the instruction.
        this->vmcs.guest_rip(this->vmcs.guest_rip() +
                             this->vmcs.vm_exit_instruction_length());

        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::tpr_below_threshold),
            0);

        advance_rip = false;
    }

    return true;
}

bool hypervisor::is_guest_kernel_image(std::size_t cpu,
                                       std::uint64_t base,
                                       std::uint64_t headers)
{
    // **A header match is not an identification.** Every image in the
    // address space begins with the same two signatures - the secure
    // kernel's does, and every driver's does - and the relative address
    // this VMM then reads a pointer from belongs to exactly one of them.
    // Reading it from the wrong image gives a number rather than a
    // failure, which is the shape of answer this project refuses.
    //
    // So the image names itself. The export directory carries the name it
    // was linked as, which for the kernel is `ntoskrnl.exe` whichever
    // file it was loaded from.
    //
    // PE32+ layout, from the specification: the optional header follows
    // the four-byte signature and the twenty-byte file header, its data
    // directories begin 112 bytes into it, the first is the export
    // directory, and that directory's name is a relative address twelve
    // bytes in.
    constexpr std::uint64_t optional_header = 4 + 20;
    constexpr std::uint64_t data_directories = 112;
    constexpr std::uint64_t export_directory_name = 12;

    auto read = [&](std::uint64_t linear, auto & into) -> bool {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        return read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&into),
                             sizeof(into)))
            .has_value();
    };

    std::uint32_t directory{};
    if (!read(base + headers + optional_header + data_directories,
              directory) ||
        (0 == directory)) {
        return false;
    }

    std::uint32_t name{};
    if (!read(base + directory + export_directory_name, name) ||
        (0 == name)) {
        return false;
    }

    // The image's own size, which the stack scan needs to decide whether
    // an address on the stack points into it. Offset 56 of the optional
    // header, per the PE specification.
    constexpr std::uint64_t size_of_image_field = 56;

    std::uint32_t size{};
    if (read(base + headers + optional_header + size_of_image_field,
             size)) {
        this->guest_kernel_size = size;
    }

    // "ntoskrnl", read as two words so it needs no string comparison and
    // no assumption about what follows.
    constexpr std::uint32_t first_half = 0x736f746e;  // "ntos"
    constexpr std::uint32_t second_half = 0x6c6e726b; // "krnl"

    std::uint32_t front{};
    std::uint32_t back{};

    return read(base + name, front) && (first_half == front) &&
           read(base + name + sizeof(front), back) &&
           (second_half == back);
}

std::uint64_t hypervisor::find_guest_kernel_base(std::size_t cpu)
{
    if (0 != this->guest_kernel_base) {
        return this->guest_kernel_base;
    }

    constexpr std::uint64_t kernel_address_floor = 0xffff800000000000;
    constexpr std::uint64_t two_megabytes = 0x200000;

    // The signatures a PE image starts with: `MZ` at the top, and `PE\0\0`
    // at the offset the field at 0x3c names. Both are checked, because a
    // single two-byte match over a hundred megabytes of kernel memory is
    // not evidence of anything.
    constexpr std::uint16_t dos_signature = 0x5a4d;
    constexpr std::uint32_t pe_signature = 0x00004550;
    constexpr std::uint64_t pe_offset_field = 0x3c;

    auto read = [&](std::uint64_t linear, auto & into) -> bool {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        return read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&into),
                             sizeof(into)))
            .has_value();
    };

    // Started from an interrupt handler, not from the instruction
    // pointer.
    //
    // The instruction pointer was the first attempt and it does not work
    // while the guest is doing the thing worth watching: through the
    // whole virtual-trust-level protection pass - twelve minutes, and
    // every sample taken in it - the guest is executing its hypercall
    // page, which is its own allocation and not part of any image. The
    // scan then walks down from an address that has no kernel below it.
    //
    // The interrupt descriptor table has no such problem. Its base is in
    // the VMCS, free to read, and every gate in it points at a handler
    // inside the kernel image whatever the guest is doing. Entry zero is
    // the divide-error fault, which exists on every processor and is
    // never a driver's.
    //
    // SDM 7.14.1 gives the 64-bit gate: the offset is split across bits
    // 15:0 at byte 0, 31:16 at byte 6, and 63:32 at byte 8.
    struct interrupt_gate
    {
        std::uint16_t offset_low{};
        std::uint16_t selector{};
        std::uint16_t attributes{};
        std::uint16_t offset_middle{};
        std::uint32_t offset_high{};
        std::uint32_t reserved{};
    };

    auto idt =
        this->vmcs.read(arch::x86_64::vmx::vmcs::field::guest_idtr_base);

    if (idt < kernel_address_floor) {
        return 0;
    }

    interrupt_gate gate{};
    if (!read(idt, gate)) {
        return 0;
    }

    auto rip = static_cast<std::uint64_t>(gate.offset_low) |
               (static_cast<std::uint64_t>(gate.offset_middle) << 16) |
               (static_cast<std::uint64_t>(gate.offset_high) << 32);

    if (rip < kernel_address_floor) {
        return 0;
    }

    auto candidate = rip & ~(two_megabytes - 1);

    for (std::size_t step{}; step < guest_windows::kernel_base_scan_limit;
         ++step) {
        std::uint16_t magic{};
        if (read(candidate, magic) && (dos_signature == magic)) {
            std::uint32_t at{};
            std::uint32_t signature{};

            if (read(candidate + pe_offset_field, at) &&
                read(candidate + at, signature) &&
                (pe_signature == signature) &&
                is_guest_kernel_image(cpu, candidate, at)) {
                this->guest_kernel_base = candidate;
                log("second-level guest kernel image at {}", candidate);
                return candidate;
            }
        }

        if (candidate < two_megabytes) {
            break;
        }

        candidate -= two_megabytes;
    }

    return 0;
}

void hypervisor::record_profile_context(
    std::uint64_t rip, const arch::x86_64::context & context)
{
    auto slot = this->profile_context_count % profile_context_capacity;

    this->profile_contexts[slot] = profile_context{
        .rip = rip,
        .rax = context.rax,
        .rcx = context.rcx,
        .rdx = context.rdx,
        .rbx = context.rbx,
        .rsi = context.rsi,
        .rdi = context.rdi,
        .r8 = context.r8,
    };

    // And what the polled pointer maps to.
    //
    // **Every sample, not every thirty-second.** Rate-limiting this was
    // an error that produced a false conclusion: the profiler fires only
    // when the guest runs a long time without exiting, which on a settled
    // machine is about once a minute, so a value refreshed every
    // thirty-two samples is refreshed every half hour. Three readings
    // taken seconds apart thenreturned the same number and were read as
    // three independent observations agreeing - which is what "it is a
    // poll, not a scan" rested on. They were one observation read three
    // times.
    //
    // The walk is expensive and the sample rate is what bounds it, not
    // this counter. When the profiler is quiet this costs almost nothing;
    // when it is loud the guest is spinning and the cost is worth paying.
    if (true) {
        if (auto physical = translate_guest_linear(context.rcx)) {
            if (auto reachable =
                    l2_physical_to_l1(this->vmcs.vpid() - 1, *physical)) {
                this->profile_pointer_virtual = context.rcx;
                this->profile_pointer_physical = *reachable;
                this->profile_pointer_rip = rip;
            }
        }
    }

    this->profile_context_count = this->profile_context_count + 1;
}

void hypervisor::record_profile_sample(std::uint64_t rip)
{
    this->profile_samples = this->profile_samples + 1;

    for (std::size_t i{}; i < profile_capacity; ++i) {
        if (this->profile_rip[i] == rip) {
            this->profile_hits[i] = this->profile_hits[i] + 1;
            return;
        }

        if (0 == this->profile_rip[i]) {
            this->profile_rip[i] = rip;
            this->profile_hits[i] = 1;
            return;
        }
    }

    // A full table is emptied rather than left to reject everything
    // after it.
    //
    // The alternative was tried and it fails at exactly the moment that
    // matters: the guest touches hundreds of addresses while it is
    // working, so the table fills during the boot and then has no room
    // for the handful it spins on afterwards - measured, 143 rejections
    // against 250 samples, and none of the rejected ones was from the
    // stall. Emptying keeps the table describing a recent window, which
    // is what a profile of a machine that changes behaviour is for.
    //
    // Counted, so a table read while it is churning is not mistaken for
    // one that settled.
    for (std::size_t i{}; i < profile_capacity; ++i) {
        this->profile_rip[i] = 0;
        this->profile_hits[i] = 0;
    }

    this->profile_rip[0] = rip;
    this->profile_hits[0] = 1;
    this->profile_overflow = this->profile_overflow + 1;
}

void hypervisor::sample_guest_stack(std::size_t cpu)
{
    auto base = this->guest_kernel_base;
    auto size = this->guest_kernel_size;

    if ((0 == base) || (0 == size)) {
        return;
    }

    auto stack = this->vmcs.guest_rsp();

    constexpr std::uint64_t kernel_address_floor = 0xffff800000000000;

    if (stack < kernel_address_floor) {
        return;
    }

    this->guest_stack_pointer = stack;
    this->guest_stack_rip = this->vmcs.guest_rip();
    this->guest_stack_count = 0;

    for (std::size_t word{};
         (word < guest_stack_words) &&
         (this->guest_stack_count < guest_stack_capacity);
         ++word) {
        auto at = stack + (word * sizeof(std::uint64_t));

        auto physical = translate_guest_linear(at);
        if (!physical) {
            // A gap in the stack's mapping ends the scan rather than
            // being stepped over: past an unmapped page the addresses
            // read belong to something else entirely.
            break;
        }

        std::uint64_t value{};
        if (!read_guest_memory(
                cpu,
                *physical,
                std::span(reinterpret_cast<std::byte *>(&value),
                          sizeof(value)))) {
            break;
        }

        // Inside the kernel image, which is what a return address into it
        // looks like. Nothing else about it is checked - see the
        // declaration for why this is a candidate list and not a stack.
        if ((value >= base) && (value < (base + size))) {
            this->guest_stack_trace[this->guest_stack_count] = value;
            this->guest_stack_count = this->guest_stack_count + 1;
        }
    }

    sample_interrupted_stack(cpu, stack, base, size);
}

void hypervisor::sample_interrupted_stack(std::size_t cpu,
                                          std::uint64_t stack,
                                          std::uint64_t base,
                                          std::uint64_t size)
{
    // The five quadwords hardware pushes, by their shape. SDM 7.14.2
    // gives the order - RIP, CS, RFLAGS, RSP, SS at increasing addresses
    // - and Windows runs its kernel at code selector 0x10 with a stack
    // selector of 0x18 or zero.
    constexpr std::uint64_t kernel_code_selector = 0x10;
    constexpr std::uint64_t kernel_stack_selector = 0x18;
    constexpr std::uint64_t rflags_always_one = 1ull << 1;
    constexpr std::uint64_t kernel_address_floor = 0xffff800000000000;
    constexpr std::size_t frame_words = 5;

    auto read = [&](std::uint64_t at, std::uint64_t & into) -> bool {
        auto physical = translate_guest_linear(at);
        if (!physical) {
            return false;
        }

        return read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&into),
                             sizeof(into)))
            .has_value();
    };

    this->guest_interrupted_count = 0;
    this->guest_interrupted_rsp = 0;
    this->guest_interrupted_rip = 0;

    for (std::size_t word{}; (word + frame_words) < guest_stack_words;
         ++word) {
        std::uint64_t frame[frame_words]{};
        auto readable = true;

        for (std::size_t i{}; i < frame_words; ++i) {
            if (!read(stack + ((word + i) * sizeof(std::uint64_t)),
                      frame[i])) {
                readable = false;
                break;
            }
        }

        if (!readable) {
            break;
        }

        // Two of the five are load bearing and the rest are not.
        //
        // The code selector and the stack pointer identify the frame:
        // `0x10` is the kernel's, and a canonical kernel address is what
        // an interrupted kernel thread's stack pointer looks like.
        // Requiring the *interrupted instruction pointer* to be inside
        // ntoskrnl as well was too strict - a thread interrupted in a
        // driver, in the hypercall page or in the HAL has a perfectly
        // valid frame this would reject - and so was pinning the stack
        // selector, which is pushed as zero as often as `0x18`.
        //
        // Measured: with those two required, no frame was found at all in
        // sixteen kilobytes of a stack that certainly contains one.
        auto shaped = (kernel_code_selector == frame[1]) &&
                      (0 != (frame[2] & rflags_always_one)) &&
                      (frame[3] >= kernel_address_floor) &&
                      (frame[0] >= kernel_address_floor);

        static_cast<void>(kernel_stack_selector);

        if (!shaped) {
            continue;
        }

        this->guest_interrupted_rip = frame[0];
        this->guest_interrupted_rsp = frame[3];
        break;
    }

    if (0 == this->guest_interrupted_rsp) {
        return;
    }

    // And the thread's own stack, from the pointer the frame carried.
    for (std::size_t word{};
         (word < guest_stack_words) &&
         (this->guest_interrupted_count < guest_stack_capacity);
         ++word) {
        std::uint64_t value{};
        if (!read(this->guest_interrupted_rsp +
                      (word * sizeof(std::uint64_t)),
                  value)) {
            break;
        }

        if ((value >= base) && (value < (base + size))) {
            this->guest_interrupted_trace[this->guest_interrupted_count] =
                value;
            this->guest_interrupted_count =
                this->guest_interrupted_count + 1;
        }
    }
}

void hypervisor::refresh_guest_threads(std::size_t cpu)
{
    if (0 == this->guest_thread_list_count) {
        return;
    }

    auto read = [&](std::uint64_t linear, std::uint64_t & into) -> bool {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        std::uint64_t value{};
        auto got = read_guest_memory(
            cpu,
            *physical,
            std::span(reinterpret_cast<std::byte *>(&value),
                      sizeof(value)));
        if (!got) {
            return false;
        }

        into = value;
        return true;
    };

    constexpr std::uint64_t byte_mask = 0xff;

    for (std::size_t i{}; i < this->guest_thread_list_count; ++i) {
        auto & entry = this->guest_thread_list[i];

        std::uint64_t word{};
        if (read(entry.thread + guest_windows::kthread_state, word)) {
            entry.state = word & byte_mask;
        }
        if (read(entry.thread + guest_windows::kthread_wait_reason,
                 word)) {
            entry.wait_reason = word & byte_mask;
        }
        if (read(entry.thread + guest_windows::kthread_wait_irql, word)) {
            entry.wait_irql = word & byte_mask;
        }
    }

    this->guest_thread_refreshes = this->guest_thread_refreshes + 1;
}

void hypervisor::walk_guest_threads(std::size_t cpu, std::uint64_t thread)
{
    // Until it finds a process with more than one thread, and then never
    // again.
    //
    // "Once, on the first plausible thread" was the first rule and it
    // caught the idle process, whose list is one thread long by
    // construction. "Only from a thread that is not the idle thread" was
    // the second and it never fired at all: this processor alternates
    // between two virtual trust levels, and every sample that read
    // cleanly - that is, every sample from the level whose offsets these
    // are - found it idle.
    //
    // So the condition is on the answer rather than on the question. A
    // list of one is the idle process and worth replacing; a longer one
    // is the system process and worth keeping. The walk stays bounded
    // because it stops for good as soon as it succeeds.
    constexpr std::uint64_t threads_worth_keeping = 2;

    if (this->guest_thread_list_count >= threads_worth_keeping) {
        return;
    }

    auto read = [&](std::uint64_t linear, std::uint64_t & into) -> bool {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        std::uint64_t value{};
        auto got = read_guest_memory(
            cpu,
            *physical,
            std::span(reinterpret_cast<std::byte *>(&value),
                      sizeof(value)));
        if (!got) {
            return false;
        }

        into = value;
        return true;
    };

    constexpr std::uint64_t kernel_address_floor = 0xffff800000000000;
    constexpr std::uint64_t byte_mask = 0xff;

    // The system process, named by a global in the image rather than
    // reached from whatever thread happens to be running - see
    // `ps_initial_system_process` for why the latter cannot work here.
    auto base = find_guest_kernel_base(cpu);
    if (0 == base) {
        return;
    }

    std::uint64_t process{};
    if (!read(base + guest_windows::ps_initial_system_process, process) ||
        (process < kernel_address_floor)) {
        return;
    }

    static_cast<void>(thread);

    // The list head is inside the process object, and its first entry
    // points at a *field* of the first thread rather than at the thread -
    // which is what a doubly linked list of embedded links means, and
    // what the subtraction below undoes.
    auto head = process + guest_windows::eprocess_thread_list_head;

    std::uint64_t link{};
    if (!read(head, link)) {
        return;
    }

    this->guest_thread_list_process = process;
    this->guest_thread_list_walked = this->guest_thread_list_walked + 1;

    std::size_t found{};
    while ((found < guest_windows::thread_walk_limit) &&
           (link >= kernel_address_floor) && (link != head)) {
        auto entry = link - guest_windows::ethread_thread_list_entry;

        guest_thread_entry recorded;
        recorded.thread = entry;

        static_cast<void>(
            read(entry + guest_windows::ethread_start_address,
                 recorded.start_address));

        std::uint64_t word{};
        if (read(entry + guest_windows::kthread_state, word)) {
            recorded.state = word & byte_mask;
        }
        if (read(entry + guest_windows::kthread_wait_reason, word)) {
            recorded.wait_reason = word & byte_mask;
        }
        if (read(entry + guest_windows::kthread_wait_irql, word)) {
            recorded.wait_irql = word & byte_mask;
        }

        this->guest_thread_list[found] = recorded;
        ++found;

        if (!read(link, link)) {
            break;
        }
    }

    this->guest_thread_list_count = found;
}

namespace
{
/** What a PE image starts with, and where it keeps its headers. */
constexpr std::uint16_t dos_signature = 0x5a4d;    // "MZ"
constexpr std::uint32_t pe_signature = 0x00004550; // "PE\0\0"
constexpr std::uint64_t dos_lfanew = 0x3c;
constexpr std::uint64_t optional_header = 0x18;
constexpr std::uint64_t magic_pe32_plus = 0x20b;
constexpr std::uint64_t data_directory_64 = 0x70;
constexpr std::uint64_t data_directory_32 = 0x60;
constexpr std::uint64_t export_name_field = 0x0c;

/**
 * The debug directory, and the CodeView record it points at.
 *
 * Most drivers export nothing, so they have no export directory and no
 * name in it - which is why the first attempt at this came back empty for
 * exactly the image that mattered. What every Windows driver does carry
 * is a CodeView record naming the symbol file it was built with, and the
 * stem of that path is the module's name. Data directory six, entries of
 * 28 bytes, type 2 is CodeView; the record is "RSDS", a 16 byte GUID, a
 * four byte age, then the path.
 */
/** IMAGE_EXPORT_DIRECTORY, past the Name field already used above. */
constexpr std::uint64_t export_count_names = 0x18;
constexpr std::uint64_t export_functions = 0x1c;
constexpr std::uint64_t export_names = 0x20;
constexpr std::uint64_t export_ordinals = 0x24;

/** LDR_DATA_TABLE_ENTRY, linked through its first member. */
constexpr std::uint64_t ldr_dll_base = 0x30;
constexpr std::uint64_t ldr_base_name = 0x58;
constexpr std::uint64_t unicode_length = 0x00;
constexpr std::uint64_t unicode_buffer = 0x08;
constexpr std::uint64_t module_walk_limit = 512;

constexpr std::uint64_t debug_directory_index = 6;
constexpr std::uint64_t directory_entry_size = 8;
constexpr std::uint64_t debug_entry_size = 28;
constexpr std::uint64_t debug_type_field = 0x0c;
constexpr std::uint64_t debug_address_field = 0x14;
constexpr std::uint32_t debug_type_codeview = 2;
constexpr std::uint64_t codeview_path = 24;

/** How far back to look. Larger than any driver on this machine and
 * bounded so a wrong address cannot walk the address space. */
constexpr std::uint64_t image_search_pages = 4096;
} // namespace

std::uint64_t hypervisor::image_base_of(std::size_t cpu,
                                        std::uint64_t address)
{
    constexpr std::uint64_t image_page = 0x1000;

    auto at = address & ~(image_page - 1);

    for (std::uint64_t i{}; i < image_search_pages;
         ++i, at -= image_page) {
        std::uint16_t magic{};

        auto physical = translate_guest_linear(at);
        if (!physical) {
            continue;
        }

        if (!read_guest_memory(
                cpu,
                *physical,
                std::span(reinterpret_cast<std::byte *>(&magic),
                          sizeof(magic)))) {
            continue;
        }

        if (dos_signature != magic) {
            continue;
        }

        // "MZ" alone is not enough - it is two common bytes. The PE
        // signature the DOS header points at is what makes it an image.
        std::uint32_t lfanew{};
        auto header = translate_guest_linear(at + dos_lfanew);
        if (!header ||
            !read_guest_memory(
                cpu,
                *header,
                std::span(reinterpret_cast<std::byte *>(&lfanew),
                          sizeof(lfanew)))) {
            continue;
        }

        std::uint32_t signature{};
        auto sig = translate_guest_linear(at + lfanew);
        if (!sig ||
            !read_guest_memory(
                cpu,
                *sig,
                std::span(reinterpret_cast<std::byte *>(&signature),
                          sizeof(signature)))) {
            continue;
        }

        if (pe_signature == signature) {
            return at;
        }
    }

    return 0;
}

void hypervisor::image_name_of(std::size_t cpu,
                               std::uint64_t base,
                               std::span<char> into)
{
    if (into.empty()) {
        return;
    }

    into[0] = '\0';

    if (0 == base) {
        return;
    }

    auto word = [&](std::uint64_t at, auto & value) {
        auto physical = translate_guest_linear(at);
        return physical &&
               read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&value),
                             sizeof(value)))
                   .has_value();
    };

    std::uint32_t lfanew{};
    if (!word(base + dos_lfanew, lfanew)) {
        return;
    }

    auto optional = base + lfanew + optional_header;

    std::uint16_t magic{};
    if (!word(optional, magic)) {
        return;
    }

    // The export directory is data directory zero, and where the
    // directories start depends on the optional header's form.
    auto directories =
        optional + ((magic_pe32_plus == magic) ? data_directory_64
                                               : data_directory_32);

    std::uint32_t export_rva{};
    if (!word(directories, export_rva) || (0 == export_rva)) {
        return;
    }

    std::uint32_t name_rva{};
    if (!word(base + export_rva + export_name_field, name_rva) ||
        (0 == name_rva)) {
        return;
    }

    copy_image_string(cpu, base + name_rva, into);
}

void hypervisor::image_debug_name_of(std::size_t cpu,
                                     std::uint64_t base,
                                     std::span<char> into)
{
    if (into.empty()) {
        return;
    }

    into[0] = '\0';

    if (0 == base) {
        return;
    }

    auto word = [&](std::uint64_t at, auto & value) {
        auto physical = translate_guest_linear(at);
        return physical &&
               read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&value),
                             sizeof(value)))
                   .has_value();
    };

    std::uint32_t lfanew{};
    if (!word(base + dos_lfanew, lfanew)) {
        return;
    }

    auto optional = base + lfanew + optional_header;

    std::uint16_t magic{};
    if (!word(optional, magic)) {
        return;
    }

    auto directories =
        optional + ((magic_pe32_plus == magic) ? data_directory_64
                                               : data_directory_32);

    std::uint32_t debug_rva{};
    std::uint32_t debug_size{};
    auto entry =
        directories + (debug_directory_index * directory_entry_size);

    if (!word(entry, debug_rva) || !word(entry + 4, debug_size) ||
        (0 == debug_rva)) {
        return;
    }

    for (std::uint64_t at{}; (at + debug_entry_size) <= debug_size;
         at += debug_entry_size) {
        std::uint32_t type{};
        std::uint32_t address{};

        if (!word(base + debug_rva + at + debug_type_field, type) ||
            !word(base + debug_rva + at + debug_address_field, address)) {
            return;
        }

        if ((debug_type_codeview != type) || (0 == address)) {
            continue;
        }

        copy_image_string(cpu, base + address + codeview_path, into);
        return;
    }
}

void hypervisor::copy_image_string(std::size_t cpu,
                                   std::uint64_t at,
                                   std::span<char> into)
{
    auto word = [&](std::uint64_t from, auto & value) {
        auto physical = translate_guest_linear(from);
        return physical &&
               read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&value),
                             sizeof(value)))
                   .has_value();
    };

    for (std::size_t i{}; i < (into.size() - 1); ++i) {
        char c{};
        if (!word(at + i, c) || ('\0' == c)) {
            into[i] = '\0';
            return;
        }
        into[i] = c;
    }

    into[into.size() - 1] = '\0';
}

std::uint64_t hypervisor::image_export(std::size_t cpu,
                                       std::uint64_t base,
                                       const char * name)
{
    // One translation per *page*, not per access.
    //
    // The first version of this translated every byte, and every
    // translation is a walk of the guest's page tables followed by the
    // guest hypervisor's extended ones. A kernel exports thousands of
    // names, so comparing them a byte at a time is millions of walks
    // inside a single VM exit - it never finished, returned zero, and the
    // caller read that as "not exported". A scan is sequential by nature
    // and stays on a page for 4096 of those bytes.
    constexpr std::uint64_t page_mask = 0xfff;

    std::uint64_t cached_page = ~std::uint64_t{};
    std::uint64_t cached_physical{};

    auto word = [&](std::uint64_t at, auto & value) {
        auto into = std::span(reinterpret_cast<std::byte *>(&value),
                              sizeof(value));

        // A read straddling two pages cannot use one translation, and
        // the aligned fields here never do - so it is answered the slow
        // way rather than made a special case of.
        if (((at & page_mask) + sizeof(value)) > (page_mask + 1)) {
            auto physical = translate_guest_linear(at);
            return physical &&
                   read_guest_memory(cpu, *physical, into).has_value();
        }

        if (auto page = at & ~page_mask; page != cached_page) {
            auto physical = translate_guest_linear(page);
            if (!physical) {
                return false;
            }

            cached_page = page;
            cached_physical = *physical;
        }

        return read_guest_memory(
                   cpu, cached_physical + (at & page_mask), into)
            .has_value();
    };

    std::uint32_t lfanew{};
    if ((0 == base) || !word(base + dos_lfanew, lfanew)) {
        return 0;
    }

    auto optional = base + lfanew + optional_header;

    std::uint16_t magic{};
    if (!word(optional, magic)) {
        return 0;
    }

    auto directories =
        optional + ((magic_pe32_plus == magic) ? data_directory_64
                                               : data_directory_32);

    std::uint32_t exports{};
    if (!word(directories, exports) || (0 == exports)) {
        return 0;
    }

    std::uint32_t count{};
    std::uint32_t names{};
    std::uint32_t ordinals{};
    std::uint32_t functions{};

    if (!word(base + exports + export_count_names, count) ||
        !word(base + exports + export_names, names) ||
        !word(base + exports + export_ordinals, ordinals) ||
        !word(base + exports + export_functions, functions)) {
        return 0;
    }

    for (std::uint32_t i{}; i < count; ++i) {
        std::uint32_t name_rva{};
        if (!word(base + names + (i * 4), name_rva)) {
            return 0;
        }

        auto matched = true;
        for (std::size_t k{};; ++k) {
            char c{};
            if (!word(base + name_rva + k, c)) {
                return 0;
            }

            if (c != name[k]) {
                matched = false;
                break;
            }

            if ('\0' == c) {
                break;
            }
        }

        if (!matched) {
            continue;
        }

        std::uint16_t ordinal{};
        std::uint32_t address{};

        if (!word(base + ordinals + (i * 2), ordinal) ||
            !word(base + functions + (ordinal * 4), address)) {
            return 0;
        }

        return base + address;
    }

    return 0;
}

void hypervisor::module_name_of(std::size_t cpu,
                                std::uint64_t kernel,
                                std::uint64_t image,
                                std::span<char> into)
{
    if (into.empty()) {
        return;
    }

    into[0] = '\0';

    auto word = [&](std::uint64_t at, auto & value) {
        auto physical = translate_guest_linear(at);
        return physical &&
               read_guest_memory(
                   cpu,
                   *physical,
                   std::span(reinterpret_cast<std::byte *>(&value),
                             sizeof(value)))
                   .has_value();
    };

    auto head = image_export(cpu, kernel, "PsLoadedModuleList");

    this->l2_module_list = head;
    this->l2_modules_walked = 0;
    this->l2_first_module_base = 0;

    if (0 == head) {
        return;
    }

    std::uint64_t entry{};
    if (!word(head, entry)) {
        return;
    }

    for (std::uint64_t i{};
         (i < module_walk_limit) && (entry != head) && (0 != entry);
         ++i) {
        std::uint64_t dll_base{};
        if (!word(entry + ldr_dll_base, dll_base)) {
            return;
        }

        this->l2_modules_walked = i + 1;

        if (0 == i) {
            this->l2_first_module_base = dll_base;
        }

        if (dll_base == image) {
            std::uint16_t length{};
            std::uint64_t buffer{};

            if (!word(entry + ldr_base_name + unicode_length, length) ||
                !word(entry + ldr_base_name + unicode_buffer, buffer)) {
                return;
            }

            // UTF-16 in, and the names are ASCII, so the high byte is
            // dropped rather than decoded.
            auto characters = static_cast<std::size_t>(length) / 2;
            std::size_t k{};

            for (; (k < characters) && (k < (into.size() - 1)); ++k) {
                std::uint16_t wide{};
                if (!word(buffer + (k * 2), wide)) {
                    break;
                }
                into[k] = static_cast<char>(wide & 0xff);
            }

            into[k] = '\0';
            return;
        }

        if (!word(entry, entry)) {
            return;
        }
    }
}

namespace
{
/**
 * `floor((numerator << 64) / denominator)`, for `numerator <
 * denominator` so the result fits.
 *
 * Shift and subtract rather than a 128-bit division, because there is no
 * runtime library here to supply `__udivti3` and a link failure at this
 * depth is a bad way to find that out. Sixty-four iterations, once.
 */
[[maybe_unused]] constexpr std::uint64_t
shifted_quotient(std::uint64_t numerator, std::uint64_t denominator)
{
    if ((0 == denominator) || (numerator >= denominator)) {
        return 0;
    }

    std::uint64_t quotient{};
    std::uint64_t remainder = numerator;

    for (int i{}; i < 64; ++i) {
        auto carry = remainder >> 63;
        remainder <<= 1;
        quotient <<= 1;

        if ((0 != carry) || (remainder >= denominator)) {
            remainder -= denominator;
            quotient |= 1;
        }
    }

    return quotient;
}

/** `((tsc * scale) >> 64)`, the reference TSC page's own arithmetic. */
[[maybe_unused]] constexpr std::uint64_t scaled_tsc(std::uint64_t tsc,
                                                    std::uint64_t scale)
{
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(tsc) * scale) >> 64);
}
} // namespace

void hypervisor::publish_reference_tsc_page(std::size_t cpu)
{
    if constexpr (!nested_vmx::publish_reference_tsc) {
        return;
    } else {
        if ((cpu >= max_cpus) || (0 != this->reference_published[cpu])) {
            return;
        }

        auto enabled = this->l2_reference_tsc_written[cpu];
        if (0 == (enabled & 1)) {
            return;
        }

        // Three samples: two to fit with, and the newest to check the fit
        // against. Fitting to two adjacent reads would divide by a tiny
        // time-stamp delta and amplify every rounding error in it, so the
        // pair is taken from opposite ends of the ring.
        auto count = this->reference_read_count[cpu];
        if (count < reference_sample_capacity) {
            return;
        }

        auto newest = (count - 1) % reference_sample_capacity;
        auto oldest = count % reference_sample_capacity;

        auto t1 = this->reference_read_tsc[cpu][oldest];
        auto r1 = this->reference_read_value[cpu][oldest];
        auto t2 = this->reference_read_tsc[cpu][newest];
        auto r2 = this->reference_read_value[cpu][newest];

        if ((t2 <= t1) || (r2 <= r1)) {
            return;
        }

        // The counter is 10 MHz and the time-stamp counter is gigahertz,
        // so the ratio is well under one and the quotient fits.
        auto scale = shifted_quotient(r2 - r1, t2 - t1);
        if (0 == scale) {
            return;
        }

        auto offset = r2 - scaled_tsc(t2, scale);

        // Checked against a sample from inside the baseline, which the
        // fit did not use. A pair always reproduces itself, so checking
        // against either end of it would prove nothing - and that is what
        // the first version did.
        constexpr std::uint64_t tolerance = 1000;

        auto middle = (count - (reference_sample_capacity / 2)) %
                      reference_sample_capacity;
        auto tm = this->reference_read_tsc[cpu][middle];
        auto rm = this->reference_read_value[cpu][middle];

        if ((0 == tm) || (tm <= t1) || (tm >= t2)) {
            return;
        }

        auto predicted = scaled_tsc(tm, scale) + offset;
        auto difference =
            (predicted > rm) ? (predicted - rm) : (rm - predicted);

        if (difference > tolerance) {
            this->reference_fit_error[cpu] = difference;
            return;
        }

        this->reference_scale[cpu] = scale;
        this->reference_offset[cpu] = offset;

        // The page is a second-level guest-physical address, so it needs
        // the guest hypervisor's extended tables to reach.
        constexpr std::uint64_t page_bits = ~std::uint64_t{0xfff};
        auto physical = l2_physical_to_l1(cpu, enabled & page_bits);
        if (!physical) {
            return;
        }

        // Scale and offset first, sequence last. The interface has the
        // guest read the sequence, the data, then the sequence again, so
        // a non-zero sequence must never be visible before what it
        // describes.
        struct
        {
            std::uint32_t sequence;
            std::uint32_t reserved;
            std::uint64_t scale;
            std::uint64_t offset;
        } page{0, 0, scale, offset};

        auto body = std::span(reinterpret_cast<const std::byte *>(&page) +
                                  sizeof(std::uint64_t),
                              sizeof(page) - sizeof(std::uint64_t));

        if (!write_guest_physical(*physical + sizeof(std::uint64_t),
                                  body)) {
            return;
        }

        std::uint32_t sequence = 1;
        if (!write_guest_physical(
                *physical,
                std::span(reinterpret_cast<const std::byte *>(&sequence),
                          sizeof(sequence)))) {
            return;
        }

        this->reference_published[cpu] = 1;

        log("cpu {} published reference tsc page at {} scale {} offset {}",
            cpu,
            enabled & page_bits,
            scale,
            offset);
    }
}

void hypervisor::capture_poll_site(std::size_t cpu)
{
    auto rip = this->vmcs.guest_rip();
    auto rsp = this->vmcs.guest_rsp();

    // Backwards far enough to take in the head of the loop, since the
    // poll itself is the bottom of it.
    constexpr std::uint64_t behind = 0x60;
    auto from = rip - behind;

    auto fetch = [&](std::uint64_t linear, std::span<std::byte> into) {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        return read_guest_memory(cpu, *physical, into).has_value();
    };

    // Byte at a time, because the range crosses a page boundary whenever
    // the loop does and a single translation would then read the wrong
    // second page. Slow, and it happens once.
    for (std::size_t i{}; i < l2_poll_code_size; ++i) {
        if (!fetch(from + i,
                   std::span(reinterpret_cast<std::byte *>(
                                 &this->l2_poll_code[i]),
                             1))) {
            return;
        }
    }

    for (std::size_t i{}; i < l2_poll_stack_words; ++i) {
        if (!fetch(rsp + (i * sizeof(std::uint64_t)),
                   std::span(reinterpret_cast<std::byte *>(
                                 &this->l2_poll_stack[i]),
                             sizeof(std::uint64_t)))) {
            break;
        }
    }

    this->l2_poll_code_base = from;
    this->l2_poll_rip = rip;
    this->l2_poll_rsp = rsp;

    // Which images those addresses belong to. The poll is in the kernel;
    // the interesting one is the first return address that is not.
    this->l2_kernel_base = image_base_of(cpu, rip);
    image_name_of(cpu, this->l2_kernel_base, this->l2_kernel_name);

    constexpr std::uint64_t kernel_space = 0xffff800000000000;

    for (auto entry : this->l2_poll_stack) {
        if (entry < kernel_space) {
            continue;
        }

        auto base = image_base_of(cpu, entry);
        if ((0 == base) || (base == this->l2_kernel_base)) {
            continue;
        }

        this->l2_driver_base = base;
        this->l2_driver_address = entry;

        // Exports first, then the symbol file, since most drivers export
        // nothing and only the second names them.
        image_name_of(cpu, base, this->l2_driver_name);

        if ('\0' == this->l2_driver_name[0]) {
            image_debug_name_of(cpu, base, this->l2_driver_name);
        }

        // And the kernel's own list last, which is the only one that
        // works for a driver that exports nothing and whose symbol-file
        // record is not resident - which is this one.
        if ('\0' == this->l2_driver_name[0]) {
            module_name_of(
                cpu, this->l2_kernel_base, base, this->l2_driver_name);
        }

        break;
    }

    // Last, so a reader that sees this set sees everything above it.
    this->l2_poll_captured = 1;
}

void hypervisor::capture_vtl_switch(std::size_t cpu,
                                    std::size_t kind,
                                    arch::x86_64::context & context)
{
    if ((cpu >= max_cpus) || (kind >= vtl_kinds)) {
        return;
    }

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Slot 4 is the VMCS's guest RSP and not the context's, and slots 16
    // to 19 are not registers the exit handler holds at all. See
    // `vtl_differed` for the layout.
    std::uint64_t now[vtl_slot_count] = {
        context.rax,         context.rbx,
        context.rcx,         context.rdx,
        vmcs.guest_rsp(),    context.rbp,
        context.rsi,         context.rdi,
        context.r8,          context.r9,
        context.r10,         context.r11,
        context.r12,         context.r13,
        context.r14,         context.r15,
        vmcs.guest_rip(),    vmcs.guest_cr3(),
        vmcs.guest_rflags(), shadow.read(field::ept_pointer),
    };

    auto count = this->vtl_switches[cpu][kind];

    // Against the previous switch of this kind, so the answer is about
    // the loop rather than about how the boot reached it.
    if (0 != count) {
        for (std::size_t i{}; i < vtl_slot_count; ++i) {
            if (now[i] != this->vtl_previous[cpu][kind][i]) {
                this->vtl_differed[cpu][kind][i] += 1;
            }
        }
    }

    for (std::size_t i{}; i < vtl_slot_count; ++i) {
        this->vtl_previous[cpu][kind][i] = now[i];
        this->vtl_latest[cpu][kind][i] = now[i];
    }

    this->vtl_switches[cpu][kind] = count + 1;

    // The timer arm happens eight times in a whole boot, so its one
    // capture is the first. The trust-level sides are **re-captured**,
    // every `vtl_recapture` switches, and that is the point of them now
    // rather than a refinement of it.
    //
    // A single capture cannot answer the question the loop poses. Over
    // ninety seconds the second-level guest executes two addresses and
    // nothing else, so what is wanted is whether the *state* behind
    // those two addresses advances - a stack that moves, an argument
    // that counts, a frame that changes - or whether it is the identical
    // call repeated for ever. One snapshot says neither. Two snapshots
    // ninety seconds apart say it outright, and the counters beside them
    // stay cumulative so nothing is lost by overwriting.
    if (timer_arm_kind == kind) {
        if (0 != count) {
            return;
        }
    } else if ((count < vtl_capture_at) ||
               (0 != (count % vtl_recapture))) {
        return;
    }

    for (std::size_t i{}; i < vtl_slot_count; ++i) {
        this->vtl_first[cpu][kind][i] = now[i];
    }

    // The stack, in this side's own address space, which is why it is
    // taken here rather than from a reader outside: a CR3 sampled from
    // out there is whichever trust level exited last, and half the time
    // that is the other one.
    auto rip = now[16];
    auto rsp = now[4];

    for (std::size_t i{}; i < vtl_stack_words; ++i) {
        auto physical =
            translate_guest_linear(rsp + (i * sizeof(std::uint64_t)));
        if (!physical) {
            break;
        }

        std::uint64_t word{};
        if (!read_guest_memory(
                cpu,
                *physical,
                std::span(reinterpret_cast<std::byte *>(&word),
                          sizeof(word)))) {
            break;
        }

        this->vtl_stack[kind][i] = word;
    }

    this->vtl_rip[kind] = rip;
    this->vtl_rsp[kind] = rsp;
    this->vtl_cr3[kind] = now[17];

    // The hypercall page is its own page and belongs to no image, so
    // this is expected to come back empty - it is recorded so that an
    // empty answer is distinguishable from a capture that never ran.
    this->vtl_image_base[kind] = image_base_of(cpu, rip);
    image_name_of(
        cpu, this->vtl_image_base[kind], this->vtl_image_name[kind]);

    // The first return address above it that resolves to an image, which
    // is the caller. Same trick as `capture_poll_site`, and the same
    // three ways of naming what it finds.
    constexpr std::uint64_t kernel_space = 0xffff800000000000;

    for (auto entry : this->vtl_stack[kind]) {
        if (entry < kernel_space) {
            continue;
        }

        auto base = image_base_of(cpu, entry);
        if (0 == base) {
            continue;
        }

        this->vtl_caller_base[kind] = base;
        this->vtl_caller_address[kind] = entry;

        // And the instructions around it, because the loop's whole body
        // is here and nothing else can show it.
        //
        // Over ninety seconds and 2,688 working exits the second-level
        // guest executed exactly two addresses - the two hypercall stubs
        // - alternating, with no third address appearing and none
        // retiring. So there is no other work to find: whatever it tests
        // between the return of one call and the start of the next is
        // the whole of what it is waiting for, it takes no exit doing
        // it, and it is a few instructions either side of this return
        // address.
        //
        // Centred rather than forward-only: the call itself and the
        // setup before it say what is being asked, and the test and
        // branch after it say what answer is being refused.
        constexpr std::uint64_t behind = vtl_code_behind;

        for (std::size_t i{}; i < vtl_code_size; ++i) {
            auto physical = translate_guest_linear(entry - behind + i);
            if (!physical) {
                break;
            }

            if (!read_guest_memory(cpu,
                                   *physical,
                                   std::span(reinterpret_cast<std::byte *>(
                                                 &this->vtl_code[kind][i]),
                                             1))) {
                break;
            }
        }

        this->vtl_code_base[kind] = entry - behind;

        // And whatever the page-aligned pointer on the stack refers to.
        //
        // Registers and stack are byte-identical every iteration, so
        // the content of the secure call is in memory - and the only
        // candidate the captures have turned up is slot +0x080 of the
        // securekernel side, which holds a page-aligned address and
        // holds the same one again at +0x098. A structure pointer
        // passed by both levels is exactly what a call whose arguments
        // are not in registers looks like.
        //
        // Guest *virtual*, so through translate_guest_linear, and the
        // outcome recorded rather than inferred - an all-zero buffer
        // has already been mistaken for an answer twice in this file.
        // Any page-aligned kernel pointer on this side's stack, and -
        // for the first trust level - the one the *second* level was
        // holding, carried across.
        //
        // The second level holds `0xfffff804345dc000` at slot +0x080 and
        // does not map it; the first level's CR3 is the one its
        // mappings are under, so the address has to be followed from
        // there rather than from where it was found. Slot +0x080 is
        // empty on the first level's own stack, so the two are not the
        // same shape and neither can be assumed of the other.
        constexpr std::size_t pointer_slot = 0x80 / 8;

        if (timer_arm_kind != kind) {
            auto candidate = this->vtl_stack[kind][pointer_slot];

            if ((0 != candidate) && (0 == (candidate & 0xfff))) {
                this->vtl_follow_at = candidate;
            }
        }

        auto shared = this->vtl_stack[kind][pointer_slot];

        // Nothing page aligned on this side's own stack: fall back to
        // what the other side was holding, which is the whole point of
        // carrying it.
        if ((0 == shared) || (0 != (shared & 0xfff))) {
            shared = this->vtl_follow_at;
        }

        if ((0 != shared) && (0 == (shared & 0xfff))) {
            this->vtl_shared_at[kind] = shared;
            this->vtl_shared_read[kind] = 0;

            for (std::size_t at{}; at < vtl_shared_size; at += 8) {
                auto physical = translate_guest_linear(shared + at);
                if (!physical) {
                    // An optional, not an expected - the walk says only
                    // that it found nothing, so the marker is the whole
                    // of what can be recorded.
                    this->vtl_shared_error[kind] = (1ull << 32);
                    break;
                }

                if (auto got = read_guest_memory(
                        cpu,
                        *physical,
                        std::span(reinterpret_cast<std::byte *>(
                                      &this->vtl_shared[kind][at]),
                                  8));
                    !got) {
                    this->vtl_shared_error[kind] =
                        static_cast<std::uint64_t>(got.error().code()) |
                        (2ull << 32);
                    break;
                }

                this->vtl_shared_read[kind] = at + 8;
            }
        }

        image_name_of(cpu, base, this->vtl_caller_name[kind]);

        if ('\0' == this->vtl_caller_name[kind][0]) {
            image_debug_name_of(cpu, base, this->vtl_caller_name[kind]);
        }

        // And the kernel's own list last, which needs a kernel base and
        // so only answers for the side that has one. `l2_kernel_base` is
        // whichever image `capture_poll_site` found, and that runs in
        // the ordinary trust level - so this is the right list for kind
        // 0 and the wrong one for kind 1, where it finds nothing rather
        // than the wrong thing.
        if (('\0' == this->vtl_caller_name[kind][0]) &&
            (0 != this->l2_kernel_base)) {
            module_name_of(cpu,
                           this->l2_kernel_base,
                           base,
                           this->vtl_caller_name[kind]);
        }

        break;
    }

    // And the page the two trust levels talk through, in this side's
    // own view of guest physical memory.
    //
    // The registers and the stack are byte-identical across thousands of
    // switches, so whatever makes this call happen again is here. The
    // interface puts the VTL control structure at offset 0x100 - the
    // entry reason, the pending-event flags and the return registers -
    // and the APIC assist at offset 0. Both fit in the first 512 bytes.
    //
    // Read through `l2_physical_to_l1` first, because the address in the
    // register is the *second-level* guest's physical address and this
    // VMM's own mapping window takes the first level's. Each trust level
    // has its own page and its own extended-page-table root, so a read
    // that skipped that step would resolve one level's address in the
    // other's tables and answer with something plausible and wrong.
    constexpr std::uint64_t vp_assist_enabled = 1;
    constexpr std::uint64_t page_mask = ~0xfffull;

    for (std::size_t which{}; which < 2; ++which) {
        auto msr = this->l2_vp_assist[cpu][which];
        if (0 == (msr & vp_assist_enabled)) {
            continue;
        }

        // How far it got, and why it stopped. Without this an all-zero
        // buffer is indistinguishable from a read that failed on its
        // first quadword - and the first attempt at this *did* come back
        // all zeroes, which is exactly the reading that would have been
        // recorded as "the page is empty".
        this->vtl_assist_read[kind][which] = 0;
        this->vtl_assist_error[kind][which] = 0;

        for (std::size_t i{}; i < vtl_assist_size; i += 8) {
            auto first = l2_physical_to_l1(cpu, (msr & page_mask) + i);
            if (!first) {
                this->vtl_assist_error[kind][which] =
                    static_cast<std::uint64_t>(first.error().code()) |
                    (1ull << 32);
                break;
            }

            if (auto got = read_guest_physical(
                    *first,
                    std::span(reinterpret_cast<std::byte *>(
                                  &this->vtl_assist[kind][which][i]),
                              8));
                !got) {
                this->vtl_assist_error[kind][which] =
                    static_cast<std::uint64_t>(got.error().code()) |
                    (2ull << 32);
                break;
            }

            this->vtl_assist_read[kind][which] = i + 8;
            this->vtl_assist_first[kind][which] = *first;
        }
    }

    // Last, so a reader that sees this set sees everything above it.
    this->vtl_captured[kind] = 1;
}

void hypervisor::sample_guest_thread(std::size_t cpu)
{
    if (cpu >= max_cpus) {
        return;
    }

    // Only one entry in every period, for the reason the declaration
    // gives: four dependent reads through two levels of translation, on
    // the hottest path here.
    if (0 != (this->l2_entries[cpu] % guest_thread_sample_period)) {
        return;
    }

    // A 64-bit read of a guest linear address, through the guest's own
    // page tables and then the guest hypervisor's extended ones. Both
    // steps are `translate_guest_linear`'s now - see `l2_physical_to_l1`
    // for why the second exists.
    auto read = [&](std::uint64_t linear, std::uint64_t & into) -> bool {
        auto physical = translate_guest_linear(linear);
        if (!physical) {
            return false;
        }

        std::uint64_t value{};
        auto got = read_guest_memory(
            cpu,
            *physical,
            std::span(reinterpret_cast<std::byte *>(&value),
                      sizeof(value)));
        if (!got) {
            return false;
        }

        into = value;
        return true;
    };

    // The kernel image first, and unconditionally.
    //
    // It used to be found inside the thread walk, which runs only for a
    // sample this VMM can make sense of - and those are the samples from
    // one of the two virtual trust levels, which are a minority. So the
    // image was not located until twenty minutes into a boot, and
    // everything that depends on it - the stack scan above all - sat idle
    // until then. The identification refuses the wrong image by name, so
    // trying on every sample costs a few reads and cannot record a wrong
    // answer.
    static_cast<void>(find_guest_kernel_base(cpu));

    guest_thread_sample sample;

    // SDM 27.4.1 keeps the guest's GS base in the VMCS, which in kernel
    // mode is the processor control region. Nothing else here is
    // architectural - every offset below belongs to another operating
    // system's build and is documented as such.
    sample.gs_base =
        this->vmcs.read(arch::x86_64::vmx::vmcs::field::guest_gs_base);

    if (0 == sample.gs_base) {
        return;
    }

    if (!read(sample.gs_base + guest_windows::kpcr_current_prcb,
              sample.prcb) ||
        (0 == sample.prcb)) {
        return;
    }

    if (!read(sample.prcb + guest_windows::kprcb_current_thread,
              sample.thread) ||
        (0 == sample.thread)) {
        return;
    }

    // **Two operating systems share this processor and only one of them
    // has these offsets.** Measured: the samples alternate between a GS
    // base of 0xfffff80683294000, where every field reads coherently, and
    // one of 0xfffff8068ac2ff80, where the thread pointer comes back as
    // 0xfffff806 and the idle thread as zero. The second is the secure
    // kernel's own processor region - its structures are
    // securekernel.exe's, not ntoskrnl.exe's, and following one image's
    // offsets through the other's memory is exactly the fabricated answer
    // this project refuses everywhere else.
    //
    // Nothing here can tell them apart by identity, so it is told apart
    // by plausibility: a thread pointer is a canonical kernel address.
    // That rejects the truncated reads without pretending to know which
    // trust level is running.
    constexpr std::uint64_t kernel_address_floor = 0xffff800000000000;

    if (sample.thread < kernel_address_floor) {
        return;
    }

    static_cast<void>(read(sample.prcb + guest_windows::kprcb_idle_thread,
                           sample.idle_thread));
    static_cast<void>(
        read(sample.thread + guest_windows::ethread_start_address,
             sample.start_address));

    // The three single-byte fields, read as words and masked. One read
    // each rather than one read of the enclosing quadword, because they
    // are not adjacent and a shared read would tie this to their spacing
    // as well as their offsets.
    constexpr std::uint64_t byte_mask = 0xff;

    std::uint64_t word{};
    if (read(sample.thread + guest_windows::kthread_state, word)) {
        sample.state = word & byte_mask;
    }
    if (read(sample.thread + guest_windows::kthread_wait_reason, word)) {
        sample.wait_reason = word & byte_mask;
    }
    if (read(sample.thread + guest_windows::kthread_wait_irql, word)) {
        sample.wait_irql = word & byte_mask;
    }

    // Refreshed every sample, once the list exists.
    //
    // The walk itself runs once - it is expensive and the membership does
    // not change while nothing happens - but *what those threads are
    // doing* is the whole question, and the one snapshot taken when the
    // list was built is from whatever moment happened to work. Here it
    // caught `Phase1Initialization` still running.
    refresh_guest_threads(cpu);
    sample_guest_stack(cpu);

    // And, once, everything else that thread's process is running.
    //
    // **Not from the idle thread**, which was the first attempt and
    // answered with a list of one: the idle thread belongs to the *idle*
    // process, which has exactly one thread per processor and never any
    // others. What is wanted is a thread of the system process, and any
    // sample taken while the guest is doing work is one - so the walk
    // waits for a sample whose thread is not the idle thread rather than
    // taking the first that reads cleanly.
    walk_guest_threads(cpu, sample.thread);

    // And where the task priority sits, which is what decides whether
    // the deferred-call vector can be delivered at all. See
    // `guest_priority_class`.
    if (auto page = this->nested_virtual_apic_address[cpu]; 0 != page) {
        constexpr std::uint64_t virtual_task_priority_offset = 0x80;
        constexpr std::uint64_t priority_class_shift = 4;

        std::uint8_t vtpr{};
        if (read_guest_physical(
                page + virtual_task_priority_offset,
                std::span(reinterpret_cast<std::byte *>(&vtpr),
                          sizeof(vtpr)))) {
            auto klass = vtpr >> priority_class_shift;
            this->guest_priority_class[cpu][klass] =
                this->guest_priority_class[cpu][klass] + 1;
        }
    }

    // Whether a software interrupt is outstanding. See
    // `kprcb_interrupt_request` - one clear reading settles what the
    // deferred-call ratio cannot.
    {
        std::uint64_t requested{};
        if (read(sample.prcb + guest_windows::kprcb_interrupt_request,
                 requested)) {
            constexpr std::uint64_t byte_mask = 0xff;

            if (0 != (requested & byte_mask)) {
                this->guest_interrupt_requested[cpu] =
                    this->guest_interrupt_requested[cpu] + 1;
            } else {
                this->guest_interrupt_idle[cpu] =
                    this->guest_interrupt_idle[cpu] + 1;
            }
        }
    }

    auto slot = this->guest_thread_sample_count[cpu] %
                guest_thread_sample_capacity;
    this->guest_thread_samples[cpu][slot] = sample;
    this->guest_thread_sample_count[cpu] =
        this->guest_thread_sample_count[cpu] + 1;
}

void hypervisor::record_interrupt_request(std::size_t cpu,
                                          std::uint64_t command)
{
    if (cpu >= max_cpus) {
        return;
    }

    // SDM 30.1.1: "VTPR: the value of bits 7:0 of the byte at offset 080H
    // on the virtual-APIC page". The processor keeps it there for the
    // guest hypervisor, and it is the value the guest hypervisor's own
    // decision to deliver or hold an interrupt is made against - so it is
    // the only reading that separates "the guest really is at
    // DISPATCH_LEVEL" from "the guest hypervisor is looking at a page
    // this VMM pointed somewhere else".
    constexpr std::uint64_t virtual_task_priority_offset = 0x80;

    auto page = this->nested_virtual_apic_address[cpu];
    std::uint8_t vtpr{};

    if (0 != page) {
        // Read rather than trusted: the address came out of vmcs12 and is
        // used as a host-physical one, which is only sound because the
        // extended page tables are an identity map. A failed read leaves
        // the sample zero and is not otherwise reported - this is a
        // diagnostic, and one that stops a processor to complain would be
        // worse than the question it answers.
        static_cast<void>(read_guest_physical(
            page + virtual_task_priority_offset,
            std::span(reinterpret_cast<std::byte *>(&vtpr),
                      sizeof(vtpr))));
    }

    auto slot =
        this->interrupt_request_count[cpu] % interrupt_request_capacity;
    this->interrupt_request_vtpr[cpu][slot] = vtpr;
    this->interrupt_request_command[cpu][slot] = command;
    this->interrupt_request_count[cpu] =
        this->interrupt_request_count[cpu] + 1;

    // SDM Figure 12-12 puts the vector in bits 7:0 of the interrupt
    // command register, and the Hyper-V interface keeps that layout for
    // its synthetic one.
    constexpr std::uint64_t interrupt_command_vector_mask = 0xff;

    auto vector = command & interrupt_command_vector_mask;
    this->interrupt_request_vector[cpu][vector] =
        this->interrupt_request_vector[cpu][vector] + 1;
}

hypervisor::l2_exit_outcome
hypervisor::on_l2_ept_fault(std::size_t cpu,
                            arch::x86_64::vmx::exit_reason reason,
                            arch::x86_64::context & context,
                            bool & advance_rip)
{
    // Phase timing; see `phase_cycles`. Added because the plan built on
    // this being 41.5% of exits assumes it is also a large share of the
    // *time*, and an exit handled here never leaves this VMM - no
    // reflection, no VMCS switch, no second-level state save - so it may
    // be an order of magnitude cheaper than a round trip and worth a
    // twentieth of what the count suggests. Counting exits and spending
    // cycles are different things and this tree has confused them before.
    auto fault_start = arch::x86_64::rdtsc();
    auto fault_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][9] +=
                arch::x86_64::rdtsc() - fault_start;
            this->phase_calls[cpu][9] += 1;
        }
    });

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Nothing retired: the faulting access has not happened yet, and the
    // whole point of every outcome below is to let it happen or to hand
    // the fault to whoever can explain it.
    advance_rip = false;

    auto guest_physical = vmcs.guest_physical_address();
    auto qualification = vmcs.exit_qualification();
    auto rip = vmcs.guest_rip();

    // How many times this exact fault has repeated, and everything the
    // branches below learn about it on the way past.
    //
    // Compared among faults only, ignoring whatever exits happen in
    // between. A livelocked second-level guest still takes its timer
    // interrupts, and a counter reset by one of those would never reach
    // any threshold - which is how the last of these went unnoticed for
    // as long as it did.
    if ((guest_physical == this->l2_ept_fault_address[cpu]) &&
        (qualification == this->l2_ept_fault_qualification[cpu]) &&
        (rip == this->l2_ept_fault_rip[cpu])) {
        this->l2_ept_fault_repeats[cpu] =
            this->l2_ept_fault_repeats[cpu] + 1;
    } else {
        this->l2_ept_fault_address[cpu] = guest_physical;
        this->l2_ept_fault_qualification[cpu] = qualification;
        this->l2_ept_fault_rip[cpu] = rip;
        this->l2_ept_fault_repeats[cpu] = 1;
    }

    auto repeats = this->l2_ept_fault_repeats[cpu];

    l2_ept_stall_record probe{};
    probe.repeats = repeats;
    probe.rip = rip;
    probe.guest_physical = guest_physical;
    probe.qualification = qualification;

    // Every return below goes through this, so a branch cannot be added
    // without saying which it is.
    auto finish = [&](l2_ept_disposition disposition,
                      l2_exit_outcome outcome) {
        probe.disposition = disposition;

        this->l2_ept_dispositions[cpu]
                                 [static_cast<std::size_t>(disposition)] +=
            1;

        // Exactly at the threshold rather than past it. The first stall is
        // the one that explains the boot, and a line per repeat afterwards
        // would evict the sequence that led into it.
        if (l2_ept_stall_threshold == repeats) {
            probe.occurred = 1;
            this->l2_ept_stall[cpu] = probe;

            log("cpu {} second level made no progress: {} faults at rip "
                "{} for {}, qualification {}, disposition {}, guest walk "
                "{} permissions {}, composition {} permissions {}, "
                "shadow {} permissions {}",
                cpu,
                repeats,
                rip,
                guest_physical,
                qualification,
                disposition,
                probe.guest_walk_status,
                probe.guest_walk_permissions,
                probe.composition_outcome,
                probe.composition_permissions,
                probe.shadow_status,
                probe.shadow_permissions);
        }

        return outcome;
    };

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};

    // Without extended page tables of its own the guest hypervisor's guest
    // runs on this VMM's, so the address that faulted is one of this VMM's
    // own guest-physical addresses and the ordinary handler is the right
    // one. That is the same code path the guest hypervisor's own faults
    // take, which is what makes it correct rather than convenient.
    if (0 == (secondary12 & secondary_enable_ept)) {
        return finish(l2_ept_disposition::without_ept,
                      l2_exit_outcome::deferred);
    }

    auto eptp12 = shadow.read(field::ept_pointer);
    probe.ept_pointer = eptp12;

    // Walk the guest hypervisor's tables for the address the processor
    // reported, which is a second-level guest-physical one. The processor
    // walked the *shadow*, so its account of which permissions were
    // missing describes neither side's table on its own.
    auto guest_walk = arch::x86_64::vmx::walk_ept(
        eptp12 & (((1ull << 52) - 1) & ~0xfffull),
        guest_physical,
        physical_address_bits(),
        execute_only_translations_offered,
        [&](std::uint64_t at) -> std::optional<arch::x86_64::vmx::epte> {
            std::uint64_t value{};
            auto read = read_guest_physical(
                at,
                std::span(reinterpret_cast<std::byte *>(&value),
                          sizeof(value)));
            if (!read) {
                return std::nullopt;
            }
            return arch::x86_64::vmx::epte(value);
        });

    auto host_walk = host_ept_lookup(guest_walk.physical_address);

    auto composition = arch::x86_64::vmx::compose_ept(
        guest_walk, host_walk, execute_only_translations_offered);

    // Everything the branches below decide from, captured while it is in
    // hand. A stall is recognised by a count, and a count says nothing
    // about which of two tables refused what.
    probe.guest_walk_status =
        static_cast<std::uint64_t>(guest_walk.status);
    probe.guest_walk_physical = guest_walk.physical_address;
    probe.guest_walk_shift = guest_walk.page_shift;
    probe.guest_walk_permissions = guest_walk.permissions.bits();
    probe.host_walk_status = static_cast<std::uint64_t>(host_walk.status);
    probe.host_walk_permissions = host_walk.permissions.bits();
    probe.composition_outcome =
        static_cast<std::uint64_t>(composition.outcome);
    probe.composition_shift = composition.page_shift;
    probe.composition_permissions = composition.permissions.bits();

    // What the shadow itself holds, which is the one account nothing else
    // here carries: the processor walked it, and the qualification it
    // produced is that walk's, but the entry it stopped on is not saved
    // anywhere. A stall whose shadow says "present, readable" while the
    // qualification says a read was refused is a stale cached translation
    // rather than a missing mapping, and the two need opposite fixes.
    //
    // Only once the fault has already repeated, because this is a walk of
    // four tables through a reverse map and the fault path is the hottest
    // one this VMM has - four hundred thousand of them in a bad minute. A
    // first fault is ordinary and needs no account of itself; a second one
    // for the same address is already the thing being watched for.
    if (repeats > 1) {
        auto in_shadow = shadow_ept_lookup(cpu, guest_physical);
        probe.shadow_status = static_cast<std::uint64_t>(in_shadow.status);
        probe.shadow_permissions = in_shadow.permissions.bits();
    }

    switch (composition.outcome) {
    case arch::x86_64::vmx::ept_compose_outcome::reflect_violation:
        // The guest hypervisor's own tables do not map it. Its guest would
        // have taken this exact fault on bare metal, so it gets it - with
        // a qualification synthesised from the walk of *its* tables rather
        // than forwarded from the shadow's, which is the whole of
        // reflected_ept_violation_qualification's reason for existing.
        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::ept_violation),
            arch::x86_64::vmx::reflected_ept_violation_qualification(
                qualification, guest_walk, false));
        return finish(l2_ept_disposition::reflected_walk,
                      l2_exit_outcome::reflected);

    case arch::x86_64::vmx::ept_compose_outcome::reflect_misconfiguration:
        // Its tables hold a value the processor rejects. Reflected for the
        // same reason, and with no qualification, because SDM 30.2.1 does
        // not list EPT misconfiguration among the exits that save one.
        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::ept_misconfiguration),
            0);
        return finish(l2_ept_disposition::reflected_misconfiguration,
                      l2_exit_outcome::reflected);

    case arch::x86_64::vmx::ept_compose_outcome::composed: {
        // "Composed" means the intersection is non-empty, NOT that it
        // permits what faulted, and the difference is a watched page.
        //
        // A watch clears write and keeps read and execute - see the
        // `write(false)` in the watch setup - so a second-level guest
        // writing a watched page composes to a perfectly valid read and
        // execute mapping. `compose_ept` reports `host_denied` only when
        // the intersection is *empty*, which that is not, so the write
        // arrived here, a read-only leaf was installed, the write was
        // resumed, and it faulted again on the leaf just installed.
        //
        // Measured, and it is what ends a Windows boot under Hyper-V:
        // the last eight exits before the guest gave up were all exit
        // reason 48 at one unchanging RIP with qualification 0x1aa - a
        // write, to a page reported readable and executable but not
        // writable - and 411,333 shadow leaves had been installed for
        // 88,281 second-level entries. One leaf per fault, no progress.
        // The watched pages are the local APIC page and the disk
        // controller's registers, which a guest writes constantly.
        //
        // So the access decides, not the intersection - and then which
        // side of the composition lacked it decides who answers.
        auto access_read = 0 != (qualification & (1ull << 0));
        auto access_write = 0 != (qualification & (1ull << 1));
        auto access_fetch = 0 != (qualification & (1ull << 2));

        auto permits =
            [&](const arch::x86_64::vmx::ept_permissions & permissions) {
                return (!access_read || permissions.read()) &&
                       (!access_write || permissions.write()) &&
                       (!access_fetch || permissions.execute());
            };

        // Which table refused it decides who answers, and the two are not
        // interchangeable.
        //
        // The guest hypervisor's tables mapping the address is not the
        // same as their permitting the access. Removing write from a page
        // it has mapped is how a hypervisor watches one - it is what this
        // VMM does to the local APIC page, and what Hyper-V does to its
        // guest's - and the intersection with ours is then still
        // non-empty, so it arrives here as `composed` rather than as a
        // walk failure. Attributing that to this VMM hands its guest's
        // trap to the wrong level: the watched-page machinery below would
        // emulate an access the guest hypervisor was waiting to be told
        // about, and a page only *it* watches would find nothing here
        // watching it and stop the processor.
        //
        // So the guest hypervisor's own permissions are tested first. Its
        // guest would have taken this fault on bare metal, which is the
        // same test the walk-failure case above applies, and the
        // qualification is synthesised the same way - bits 3, 4 and 5 come
        // from the walk of *its* tables, which is exactly what it needs to
        // see to know which permission it removed.
        if (!permits(guest_walk.permissions)) {
            reflect_l2_exit(
                cpu,
                static_cast<std::uint64_t>(basic_reason::ept_violation),
                arch::x86_64::vmx::reflected_ept_violation_qualification(
                    qualification, guest_walk, false));
            return finish(l2_ept_disposition::reflected_permission,
                          l2_exit_outcome::reflected);
        }

        // Left over: the guest hypervisor permits it and the composition
        // does not, so the permission missing is this VMM's own.
        if (!permits(composition.permissions)) {
            if (!on_ept_violation(
                    cpu, context, guest_walk.physical_address)) {
                log("cpu {} second level {} to {} at first level {}, "
                    "which nothing here watches",
                    cpu,
                    access_write ? "write" : "access",
                    guest_physical,
                    guest_walk.physical_address);
                record_exit(reason, context);
                on_unhandled_exit(reason);

                return finish(l2_ept_disposition::unwatched,
                              l2_exit_outcome::handled);
            }

            return finish(l2_ept_disposition::watched,
                          l2_exit_outcome::handled);
        }

        // Both levels permit it, so the shadow is behind and this is not
        // a fault at all - it is a mapping that has to be put there.
        //
        // Rebuilding when the source or the generation moved is not
        // enough, and the reason is architectural rather than a bug in
        // the rebuild: a guest hypervisor that changes an EPT entry from
        // not-present to present **is not required to invalidate
        // anything**. SDM 31.4.3.3, "Guidelines for Use of the INVEPT
        // Instruction", requires invalidation when an entry is made
        // *more restrictive* and lists making one less restrictive among
        // the cases where software may skip it. So a shadow built from a
        // walk never learns about a page mapped after it was built,
        // nothing moves the source or the generation, and the access is
        // resumed to fault identically for ever.
        //
        // Measured on the rig, and it is what a Windows guest does within
        // seconds of Hyper-V launching: exit reason 48 filling the boot
        // processor's whole exit ring, qualification 0x184 - an
        // instruction fetch, linear address valid - at one unchanging
        // guest RIP, 1,062,627 exits, while the second level managed a
        // hundred entries in total. The comment that used to be here
        // called a loop the way a genuine bug would show itself. This is
        // that loop, and the bug is the assumption above it.
        //
        // So the faulting mapping is installed rather than the whole
        // shadow rebuilt. One leaf, not eleven thousand regions - the
        // rebuild is still there for what it is for, which is the source
        // or the generation actually moving.
        auto pointer = shadow_ept_pointer_for(cpu, eptp12);
        if (!pointer) {
            log("cpu {} shadow ept rebuild failed after an l2 fault at "
                "{}: error {}",
                cpu,
                guest_physical,
                pointer.error().code());
            record_exit(reason, context);
            on_unhandled_exit(reason);
            return finish(l2_ept_disposition::pointer_failed,
                          l2_exit_outcome::handled);
        }

        vmcs.ept_pointer(*pointer);

        auto page =
            guest_physical & ~((1ull << composition.page_shift) - 1);

        if (auto installed = fill_shadow_leaf(
                cpu, page, guest_walk, composition.page_shift);
            !installed) {
            log("cpu {} could not install a shadow leaf for {}: error {}",
                cpu,
                page,
                installed.error().code());
            record_exit(reason, context);
            on_unhandled_exit(reason);
            return finish(l2_ept_disposition::install_failed,
                          l2_exit_outcome::handled);
        }

        // The shadow's entry for this address has just changed from
        // permitting nothing to permitting something, and this processor
        // may hold the old one. Locally only: no other processor can have
        // cached a translation through a shadow that is this one's alone.
        invalidate_ept_locally();

        this->shadow_ept_leaves_filled[cpu] =
            this->shadow_ept_leaves_filled[cpu] + 1;

        // The handler's own work, read back.
        //
        // Installing a mapping is the one disposition here that claims to
        // have *fixed* something, and the claim is checkable: the access
        // that faulted must now be permitted by what is in the table. When
        // it is not, the guest is about to be resumed onto the identical
        // fault and will be for ever, and every counter in this class will
        // go on looking healthy while it happens - `leaves_filled` in
        // particular climbs beautifully.
        //
        // That is not hypothetical. It has happened twice: a watched page
        // composing to a valid read-and-execute leaf that a *write* then
        // faulted on again, 411,333 leaves for 88,281 entries; and a 2 MB
        // leaf where the composition was only valid for 4 KB. Both were
        // found by hand, days later, from a ring full of one address.
        //
        // Only past the first repeat, since the walk is not free and a
        // single fault cannot be a loop yet.
        if (repeats > 1) {
            auto after = shadow_ept_lookup(cpu, guest_physical);

            probe.shadow_status = static_cast<std::uint64_t>(after.status);
            probe.shadow_permissions = after.permissions.bits();

            if (!permits(after.permissions)) {
                this->shadow_ept_leaves_that_did_not_help[cpu] =
                    this->shadow_ept_leaves_that_did_not_help[cpu] + 1;

                log("cpu {} installed a shadow leaf for {} that still "
                    "refuses the access: qualification {}, composed {}, "
                    "installed {}",
                    cpu,
                    page,
                    qualification,
                    composition.permissions.bits(),
                    after.permissions.bits());
            }
        }

        return finish(l2_ept_disposition::installed,
                      l2_exit_outcome::handled);
    }

    case arch::x86_64::vmx::ept_compose_outcome::host_denied:
    default:
        // This VMM's own tables refused it, so the guest hypervisor's are
        // innocent and must not be told otherwise - it would go looking
        // for a bug in tables that permit the access.
        //
        // Which leaves the watched-page machinery, keyed on this VMM's own
        // guest-physical addresses. The walk of the guest hypervisor's
        // tables just produced one, so it is handed over rather than taken
        // from the VMCS: the field the processor wrote holds a
        // second-level address, which no watch is keyed on.
        //
        // Opening a watched page changes this VMM's own tables and so
        // bumps the generation the shadow was built against. Nothing is
        // done about that here, because nothing needs to be: the resumed
        // access faults again, the composition then says `composed`, and
        // the branch above rebuilds. It costs a rebuild per stepped write
        // and is paid only by a guest hypervisor whose guest touches a
        // page this VMM watches - which is the disk channel's queue, owned
        // by the first-level guest's own operating system.
        //
        // A page nothing watches stops the processor, exactly as the
        // first-level guest's own access to the module does. BACKLOG.md
        // records that as a limit rather than a design.
        if (!on_ept_violation(cpu, context, guest_walk.physical_address)) {
            log("cpu {} second level touched {} at first level {}, which "
                "nothing here watches",
                cpu,
                guest_physical,
                guest_walk.physical_address);
            record_exit(reason, context);
            on_unhandled_exit(reason);

            return finish(l2_ept_disposition::unwatched,
                          l2_exit_outcome::handled);
        }

        return finish(l2_ept_disposition::watched,
                      l2_exit_outcome::handled);
    }
}

hypervisor::l2_exit_outcome
hypervisor::on_l2_exit(std::size_t cpu,
                       arch::x86_64::vmx::exit_reason reason,
                       arch::x86_64::context & context,
                       bool & advance_rip)
{
    // SDM 29, step 5: the launch state becomes launched only after the
    // guest-state checks and the MSR loads have passed, so an entry
    // failure leaves it where it was and the next attempt has to be a
    // VMLAUNCH again.
    if (!reason.entry_failure()) {
        this->vmcs02_launched[cpu] = true;
    }

    if (reason.entry_failure()) {
        // The processor accepted the controls and the host state, loaded
        // the guest state the guest hypervisor wrote, and then found it
        // inconsistent. That is its guest's state and its own account to
        // receive: SDM 29.8 delivers it as a VM exit with bit 31 of the
        // reason set, and the guest hypervisor's own error handling is
        // written around exactly that.
        log("cpu {} second level entry failed after loading guest state, "
            "reason {} qualification {}",
            cpu,
            reason.value(),
            this->vmcs.exit_qualification());

        reflect_l2_exit(cpu, reason, this->vmcs.exit_qualification());
        advance_rip = false;
        return l2_exit_outcome::reflected;
    }

    // Extended page-table faults are decided by composing the two levels
    // rather than by the table below, because which level refused the
    // access is not something the exit itself says.
    if ((basic_reason::ept_violation == reason.basic()) ||
        (basic_reason::ept_misconfiguration == reason.basic())) {
        auto outcome = on_l2_ept_fault(cpu, reason, context, advance_rip);
        if (l2_exit_outcome::deferred != outcome) {
            this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        }
        return outcome;
    }

    // Two questions in order, which is KVM's shape in
    // `nested_vmx_reflect_vmexit` and is the order that matters: an exit
    // this VMM must have is one no reflection may take away, and only
    // after that does the guest hypervisor's own configuration decide.
    if (l0_wants_l2_exit(cpu, reason, context)) {
        this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        return l2_exit_outcome::deferred;
    }

    if (!l1_wants_l2_exit(cpu, reason, context)) {
        // Neither side asked for it, which can only happen where this
        // VMM's own controls are wider than the union it built - the
        // MONITOR and MWAIT intercepts are the two. The ordinary handler
        // answers them the same way it does for any guest.
        this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        return l2_exit_outcome::deferred;
    }

    // Carried into the ring, and only here, where the second-level
    // guest's registers are still the ones in hand.
    if (cpu < max_cpus) {
        this->l2_exit_detail[cpu] = context.rcx;

        // And what went through that register, for the MSR reasons. See
        // `exit_trace_entry::detail_value`: the reason and the register
        // together cannot tell a clock being waited on from a clock that
        // stopped, and the value can.
        constexpr std::uint64_t low_half_mask = 0xffffffff;
        constexpr std::uint64_t high_half_shift = 32;

        this->l2_exit_detail_value[cpu] =
            ((context.rdx & low_half_mask) << high_half_shift) |
            (context.rax & low_half_mask);

        // And a census of the synthetic range that outlives both rings.
        // See `l2_synthetic_msr_reads`.
        constexpr std::uint64_t synthetic_msr_block = 0xffffff00;
        constexpr std::uint64_t synthetic_msr_base = 0x40000000;

        if (synthetic_msr_base == (context.rcx & synthetic_msr_block)) {
            auto slot = context.rcx & 0xff;

            if (basic_reason::rdmsr == reason.basic()) {
                this->l2_synthetic_msr_reads[cpu][slot] += 1;

                // Once, at the poll, and only after the loop has clearly
                // settled - an early capture would catch the boot path
                // reading the counter rather than the loop that never
                // leaves. See `l2_poll_code`.
                constexpr std::uint64_t reference_count_slot = 0x20;
                constexpr std::uint64_t settled = 200000;

                // Retried, not one-shot, and throttled because the
                // scan behind it walks pages looking for a PE header.
                //
                // One capture per boot was the first shape and it is not
                // enough: the loop reaches the poll by more than one call
                // path, and a sixty-four word window caught the driver's
                // frames in one boot and nothing but the kernel in the
                // next two. Retrying until a frame outside the kernel
                // turns up costs a few hundred scans and removes the
                // reboot from the loop.
                constexpr std::uint64_t retry_every = 512;

                if ((reference_count_slot == slot) &&
                    (0 == this->l2_driver_base) &&
                    (this->l2_synthetic_msr_reads[cpu][slot] > settled) &&
                    (0 == (this->l2_synthetic_msr_reads[cpu][slot] %
                           retry_every))) {
                    capture_poll_site(cpu);
                }
            } else if (basic_reason::wrmsr == reason.basic()) {
                this->l2_synthetic_msr_writes[cpu][slot] += 1;

                // Where the trust levels talk to each other. See
                // `l2_vp_assist`: the loop's decision to call again is
                // made from neither registers nor stack, both of which
                // are byte-identical across thousands of switches, so it
                // is made from this page.
                //
                // Recorded per trust level, keyed on the extended-page-
                // table pointer in force, because the register is
                // per-VTL and each level configures its own - one slot
                // would hold whichever wrote last and there would be no
                // way to tell which.
                constexpr std::uint64_t vp_assist_slot = 0x73;

                if (vp_assist_slot == slot) {
                    auto eptp =
                        this->guest_vmcs12[cpu].read(field::ept_pointer);

                    // The slot already claimed by this level, or the
                    // first free one. Two levels, two slots, and a third
                    // would mean the assumption that there are two is
                    // wrong - which `l2_vp_assist_eptp` makes visible
                    // rather than silently overwriting.
                    for (std::size_t which{}; which < 2; ++which) {
                        if ((this->l2_vp_assist_eptp[cpu][which] ==
                             eptp) ||
                            (0 == this->l2_vp_assist_eptp[cpu][which])) {
                            this->l2_vp_assist[cpu][which] =
                                this->l2_exit_detail_value[cpu];
                            this->l2_vp_assist_eptp[cpu][which] = eptp;
                            break;
                        }
                    }
                }

                // Kept so the page can be read from outside. See
                // `l2_reference_tsc_written`.
                constexpr std::uint64_t reference_tsc_slot = 0x21;

                if (reference_tsc_slot == slot) {
                    this->l2_reference_tsc_written[cpu] =
                        this->l2_exit_detail_value[cpu];
                }
            }
        }

        // And the two hypercalls that switch virtual trust level, which
        // are the only work this guest does that is not the idle loop.
        //
        // The call code is the low sixteen bits of the hypercall input
        // value in RCX, which is the interface this VMM announces
        // through the hypercall page MSR - not KVM's, which takes it in
        // RAX. Both have been seen on this path, so the reason is
        // checked as well as the register.
        constexpr std::uint64_t hypercall_code_mask = 0xffff;
        constexpr std::uint64_t vtl_call_code = 0x11;
        constexpr std::uint64_t vtl_return_code = 0x12;

        if (basic_reason::vmcall == reason.basic()) {
            auto code = context.rcx & hypercall_code_mask;

            if (vtl_call_code == code) {
                capture_vtl_switch(cpu, 0, context);
            } else if (vtl_return_code == code) {
                capture_vtl_switch(cpu, 1, context);
            }
        }

        // The vector of an external interrupt on its way to the guest
        // hypervisor. Available only because "acknowledge interrupt on
        // exit" now reaches vmcs02 - see build_vmcs02 - and read here
        // rather than in `reflect_l2_exit` because the valid bit has to
        // be tested against the reason that produced it.
        //
        // Where the guest went after an entry that injected `0xd1`, at
        // the first exit after it. Taken here because this is the last
        // point at which the second-level guest's own VMCS is current,
        // so the instruction pointer is its own.
        if (0 != this->injection_landing_armed[cpu]) {
            auto slot = this->injection_landing_count[cpu] %
                        injection_landing_capacity;
            this->injection_to_rip[cpu][slot] = this->vmcs.guest_rip();
            this->injection_to_reason[cpu][slot] = reason.value();
            this->injection_landing_count[cpu] =
                this->injection_landing_count[cpu] + 1;
            this->injection_landing_armed[cpu] = 0;
        }

        // SDM 27.9.2: bits 7:0 are the vector, bit 31 is validity.
        if (basic_reason::external_interrupt == reason.basic()) {
            auto information =
                this->vmcs.read(field::vm_exit_interruption_information);

            if (0 != (information & interruption_information_valid)) {
                auto vector = information & interruption_vector_mask;
                this->l2_external_vector[cpu][vector] =
                    this->l2_external_vector[cpu][vector] + 1;
            }
        }

        // The two halves of the synthetic timer comparison the machine
        // stops on, taken on the way past. Both MSRs are outside the
        // bitmap's ranges, so they exit unconditionally and are about to
        // be reflected - this is the last point at which the
        // second-level guest's registers are the ones in the processor.
        //
        // The write's value is here. The read's answer is not: Hyper-V
        // produces it after the reflection, so all that can be done here
        // is to note that one is owed, and `on_guest_vmlaunch` collects
        // it from the registers Hyper-V loads before its VMRESUME.
        constexpr std::uint32_t time_reference_count = 0x40000020;
        constexpr std::uint32_t synthetic_timer0_config = 0x400000b0;
        constexpr std::uint32_t synthetic_timer0_count = 0x400000b1;

        auto index = static_cast<std::uint32_t>(context.rcx);

        // A deliberate stretch of the second-level guest's own timer
        // period, off unless asked for. See ZPP_STRETCH_GUEST_TIMER.
        //
        // This exists to separate two explanations that every
        // measurement so far is equally consistent with. The guest arms
        // its synthetic timer for 1.74 ms and a tick costs this VMM
        // about 1.97 ms, so it never leaves its clock handler: 92,116
        // consecutive requests for vector 0x2f, every one of them with
        // the virtual-APIC page's task priority at 0xd0, which refuses
        // priority 2 correctly. That is either a machine too slow to
        // finish a tick inside a tick - a price - or a handler that
        // would not finish however long it were given - a block. The
        // two look identical from outside, and no amount of further
        // optimisation distinguishes them, because optimisation only
        // moves the machine along the axis they share.
        //
        // Multiplying the period does distinguish them, and does it
        // without needing the 2.3x that would otherwise be required:
        // give the handler eight times its budget and either it
        // completes and the guest makes progress, or it does not and
        // speed was never the question. `ZPP_SLOW_EXITS` is the same
        // experiment pointed the other way and can only retreat from
        // the threshold, never cross it.
        //
        // Only a *period* is scaled. The Hyper-V interface defines the
        // count of a periodic timer as a period in 100 ns units and the
        // count of a one-shot as an absolute expiry in reference-counter
        // units, and multiplying an absolute time is meaningless. The
        // two are separated by size, as measured on this rig: periods
        // are 17,400 and 156,250, absolute deadlines are around 1.5e9.
        // One second in 100 ns units sits between them with three orders
        // of magnitude either side.
        //
        // **Diagnostic only.** A guest whose clock is eight times slow
        // is a guest being lied to about time, which is the one thing
        // this VMM is otherwise careful never to do.
//
// **Superseded by the floor below, and kept because the two are
// different claims.** A multiplier scales whatever the guest
// asked for, so it is a lie of unbounded size and it is what
// turned a 64 Hz tick into a 2 Hz one and bugchecked the guest
// sixteen times in one boot. A floor only ever refuses a period
// *shorter* than one the same guest chose for itself minutes
// earlier, and stops refusing the moment it asks for anything
// longer.
#ifndef ZPP_STRETCH_GUEST_TIMER
#define ZPP_STRETCH_GUEST_TIMER 1
#endif
        if constexpr (1 != ZPP_STRETCH_GUEST_TIMER) {
            if ((basic_reason::wrmsr == reason.basic()) &&
                (synthetic_timer0_count == index)) {
                constexpr std::uint64_t period_limit = 10000000;
                auto value =
                    (context.rax & 0xffffffff) | (context.rdx << 32);

                if ((0 != value) && (value < period_limit)) {
                    value *= ZPP_STRETCH_GUEST_TIMER;
                    context.rax = value & 0xffffffff;
                    context.rdx = value >> 32;

                    if (cpu < max_cpus) {
                        this->guest_timer_stretched[cpu] =
                            this->guest_timer_stretched[cpu] + 1;
                    }
                }
            }
        }

        // A floor under the second-level guest's tick period, off
        // unless asked for. See ZPP_TICK_FLOOR, in 100 ns units.
        //
        // The tick rate is the binding constraint on this boot and
        // nothing else here can move it. Three of the four round trips a
        // tick costs are the guest's writes of the synthetic
        // end-of-interrupt, interrupt command and end-of-message
        // registers, and those lie outside both ranges an MSR bitmap can
        // describe - SDM 26.6.9 - so they exit *unconditionally*. They
        // cannot be filtered, they cannot be made cheaper, and each is a
        // full reflection. The only variable is how many ticks there
        // are.
        //
        // Measured on the rig on 2026-08-15: the guest arms 156,250 -
        // 15.625 ms, the ordinary 64 Hz tick - and then seventy-eight
        // seconds later re-arms *periodic* at 17,400, 1.74 ms, and never
        // changes it again. `capture_vtl_switch`'s third kind caught the
        // site: `ntoskrnl.exe`+0x3a57f8, called from +0x35d4ed, with
        // vector 0xd1 and IRQL 0xd on the stack - so the clock handler
        // re-arms its own period from inside itself. At 64 Hz those four
        // round trips are 7% of the wall clock. At 575 Hz they are most
        // of it, and the guest is at CLOCK_LEVEL on 68% of its entries
        // with ring 3 never entered once.
        //
        // **This is still a lie about time and it is bounded.** The
        // floor is the period this same guest ran with for its first
        // seventy-eight seconds, not a number invented here, and a
        // request for anything longer passes through untouched. What it
        // costs is that the guest's tick-driven system time advances
        // more slowly than its reference counter, which stays honest -
        // the two disagreeing is what is being traded for the budget to
        // finish a tick inside a tick.
        //
        // Periods only, by the same size test the stretch above uses and
        // for the same reason: the interface defines a periodic count as
        // a period and a one-shot count as an absolute expiry, and a
        // floor under an absolute time would push every deadline into
        // the future. `l2_stimer_config`'s periodic bit is checked as
        // well, so the test is the interface's own answer and not only
        // the magnitude.
#ifndef ZPP_TICK_FLOOR
#define ZPP_TICK_FLOOR 0
#endif
        if constexpr (0 != ZPP_TICK_FLOOR) {
            constexpr std::uint64_t floor_value = ZPP_TICK_FLOOR;
            constexpr std::uint64_t period_limit = 10000000;
            constexpr std::uint64_t config_periodic = 1ull << 1;

            if ((basic_reason::wrmsr == reason.basic()) &&
                (synthetic_timer0_count == index) && (cpu < max_cpus) &&
                (0 != (this->l2_stimer_config[cpu] & config_periodic))) {
                auto value =
                    (context.rax & 0xffffffff) | (context.rdx << 32);

                if ((0 != value) && (value < period_limit) &&
                    (value < floor_value)) {
                    context.rax = floor_value & 0xffffffff;
                    context.rdx = floor_value >> 32;

                    this->guest_tick_floored[cpu] =
                        this->guest_tick_floored[cpu] + 1;
                }
            }
        }

        // Every synthetic MSR the second-level guest touches, counted.
        // See the declaration for what this settles; the short version is
        // that the end-of-message register at 0x40000084 is the one thing
        // that says whether the interface's message protocol ever
        // completed a round.
        if (auto offset = index - 0x40000000u;
            (index >= 0x40000000u) && (offset < synthetic_msr_capacity)) {
            if (basic_reason::rdmsr == reason.basic()) {
                this->synthetic_msr_reads[cpu][offset] =
                    this->synthetic_msr_reads[cpu][offset] + 1;
            } else if (basic_reason::wrmsr == reason.basic()) {
                this->synthetic_msr_writes[cpu][offset] =
                    this->synthetic_msr_writes[cpu][offset] + 1;
                this->synthetic_msr_last_write_tsc[cpu][offset] =
                    arch::x86_64::rdtsc();
                this->synthetic_msr_last_value[cpu][offset] =
                    (context.rax & 0xffffffff) | (context.rdx << 32);
            }
        }

        // The state a reflected `hlt` hands over, taken while vmcs02 is
        // still current so these are the second-level guest's own values
        // - the same ones `save_l2_state` is about to write into vmcs12,
        // read from the same place it reads them.
        if (basic_reason::hlt == reason.basic()) {
            this->hlt_reflect_rflags[cpu] = this->vmcs.guest_rflags();
            this->hlt_reflect_interruptibility[cpu] =
                this->vmcs.read(arch::x86_64::vmx::vmcs::field::
                                    guest_interruptibility_state);
            this->hlt_reflect_activity[cpu] = this->vmcs.read(
                arch::x86_64::vmx::vmcs::field::guest_activity_state);
            this->hlt_reflect_rip[cpu] = this->vmcs.guest_rip();
            this->hlt_reflect_tsc[cpu] = arch::x86_64::rdtsc();
            this->hlt_reflect_count[cpu] =
                this->hlt_reflect_count[cpu] + 1;
        }

        // The interrupt the guest is asking for, and the task priority in
        // force as it asks. See `interrupt_request_vtpr` for why this is
        // the one moment worth a guest memory read: the guest asks for
        // vector 0x2f a quarter of a million times and receives it seven,
        // and whether that is the guest hypervisor's fault or its guest's
        // is decided by one byte on the virtual-APIC page.
        constexpr std::uint32_t synthetic_interrupt_command = 0x40000071;

        if ((basic_reason::wrmsr == reason.basic()) &&
            (synthetic_interrupt_command == index)) {
            auto command =
                (context.rax & 0xffffffff) | (context.rdx << 32);

            record_interrupt_request(cpu, command);

            // Held until the guest's own priority allows it. See
            // `nested_vmx::deliver_self_ipi`; SDM Figure 12-12 puts the
            // vector in bits 7:0 and the destination shorthand in 19:18,
            // and 01 there is "self".
            //
            // **The shorthand is not how this guest says "me".** Measured
            // on the rig: every one of 297,465 writes of this register
            // carries `0x4002f` in the low half and zero in the high one
            // - vector 0x2f, fixed delivery, level asserted, destination
            // shorthand **00**, and a physical destination of APIC id 0.
            // Testing the shorthand alone therefore matched nothing at
            // all, and the three `l2_self_ipi_*` counters would have read
            // zero on a run with the switch on and been read as "the
            // guest never asks", which is the opposite of the truth.
            //
            // A physical destination naming this processor is the same
            // request by the other spelling, and it is the spelling the
            // Hyper-V synthetic register uses: its high half is the
            // x2APIC destination field rather than a second shorthand.
            //
            // `0` is taken as "this processor" because the second-level
            // guest here has one virtual processor and its APIC id is
            // zero. That is an assumption about the guest rather than
            // about the architecture, so it is counted rather than
            // trusted: `l2_ipi_not_self` rises for any command that is
            // neither spelling, and a run where it is non-zero has found
            // a destination this rule would deliver to the wrong
            // processor.
            if constexpr (nested_vmx::deliver_self_ipi) {
                constexpr std::uint64_t shorthand_mask = 3ull << 18;
                constexpr std::uint64_t shorthand_self = 1ull << 18;
                constexpr std::uint64_t destination_shift = 32;

                auto shorthand = command & shorthand_mask;
                auto destination = command >> destination_shift;

                auto to_self = (shorthand_self == shorthand) ||
                               ((0 == shorthand) && (0 == destination));

                if (cpu < max_cpus) {
                    if (to_self) {
                        this->l2_self_ipi_pending[cpu] = command & 0xff;
                    } else {
                        this->l2_ipi_not_self[cpu] += 1;
                    }
                }
            }
        }

        if ((basic_reason::rdmsr == reason.basic()) &&
            (time_reference_count == index)) {
            this->reference_read_pending[cpu] = true;
        } else if (basic_reason::wrmsr == reason.basic()) {
            // The count and the configuration in one ring, tagged, so
            // their *order* survives - which is the whole reason to
            // record the configuration at all.
            //
            // A count means two different things depending on it: the
            // Hyper-V interface defines the count of a periodic timer as
            // a period in 100 ns units, and the count of a one-shot timer
            // as an absolute expiration time in reference-counter units.
            // Measured on the rig, the root partition writes 156,250 -
            // 15.625 ms as a period, and a time nine hours in the past as
            // an absolute deadline, when the reference counter stands at
            // about 1.5e9. Those are not the same claim and nothing
            // recorded so far distinguishes them.
            //
            // The configuration also carries the enable bit and the
            // synthetic interrupt source the expiry is posted to, and
            // "enabled" is the specific thing in question: the guest
            // hypervisor settles on a 2.38 second one-shot deadline on
            // its own local APIC while a 15.625 ms synthetic timer is
            // supposedly armed, which is what a hypervisor does when it
            // believes nothing is due.
            //
            // Two configuration writes precede each count write, so the
            // ring holds triples and the tag is what makes them readable.
            auto tag = std::uint64_t{};

            if (synthetic_timer0_count == index) {
                tag = 1;
            } else if (synthetic_timer0_config == index) {
                tag = 2;
            }

            if (0 != tag) {
                auto slot = this->stimer_arm_count[cpu] %
                            reference_sample_capacity;
                this->stimer_arm_value[cpu][slot] =
                    (context.rax & 0xffffffff) | (context.rdx << 32);
                this->stimer_arm_tsc[cpu][slot] = arch::x86_64::rdtsc();
                this->stimer_arm_kind[cpu][slot] = tag;
                this->stimer_arm_count[cpu] =
                    this->stimer_arm_count[cpu] + 1;

                // And who asked, the first time a periodic count is
                // programmed short enough to matter.
                //
                // The tick rate is the binding constraint on this whole
                // boot and it is the second-level guest's own choice.
                // Measured on the rig: it arms 156,250 - 15.625 ms, the
                // ordinary 64 Hz tick - and then seventy-eight seconds
                // later re-arms at **17,400**, 1.74 ms, and never
                // changes it again. Three of the four round trips a
                // tick costs are synthetic-MSR writes that exit
                // unconditionally, so at 575 Hz they alone are most of
                // the wall clock; at 64 Hz the same four are seven per
                // cent of it. Nothing about that is fixable from
                // underneath unless it is known which component asked.
                //
                // One capture, on the first short periodic count, and
                // nothing after it: the register diff would be
                // meaningless across eight arms in a whole boot, and
                // what is wanted is the stack.
                constexpr std::uint64_t short_period = 100000;
                constexpr std::uint64_t config_periodic = 1ull << 1;

                if ((1 == tag) && (cpu < max_cpus) &&
                    (0 == this->vtl_captured[timer_arm_kind]) &&
                    (0 !=
                     (this->l2_stimer_config[cpu] & config_periodic)) &&
                    (0 != this->stimer_arm_value[cpu][slot]) &&
                    (this->stimer_arm_value[cpu][slot] < short_period)) {
                    capture_vtl_switch(cpu, timer_arm_kind, context);
                }

                if (2 == tag) {
                    this->l2_stimer_config[cpu] =
                        this->stimer_arm_value[cpu][slot];
                }
            }
        }
    }

    reflect_l2_exit(cpu, reason, this->vmcs.exit_qualification());
    advance_rip = false;
    return l2_exit_outcome::reflected;
}

} // namespace zpp::hypervisor
