#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Hyper-V's enlightened VMCS, and the virtual-processor assist page that
 * arms it.
 *
 * **Why this exists.** Windows with virtualization-based security boots on
 * KVM alone in about five minutes; putting this VMM between them
 * livelocks it. Nesting cost does not explain that, because KVM is doing
 * the same nesting - what KVM has and this VMM lacks is enlightened VMCS.
 * With it armed, the guest hypervisor stops executing VMREAD, VMWRITE and
 * VMPTRLD altogether and reads and writes the structure below in memory
 * instead. Measured on the rig, those instructions are **54% of a
 * trust-level round trip** - `vmresume` 71.1, `vmread` 5.1, `vmptrld` 4.0
 * against 2.0 `vmcall` of actual work - and this file had recorded them
 * as unavoidable nesting tax. They are not: they are what this removes.
 *
 * **The layout is Hyper-V's, not the architecture's.** It is defined by
 * the Top Level Functional Specification and transcribed here from
 * `.references/xen/xen/arch/x86/include/asm/guest/hyperv-tlfs.h:583`. A
 * field at the wrong offset does not fault - it silently hands the guest
 * hypervisor a value belonging to a different field, which is corruption
 * of the guest rather than a visible failure. Nothing here may be
 * reordered, and the padding members are load-bearing.
 *
 * The names are the specification's, deliberately, including where they
 * disagree with the spellings this tree uses elsewhere -
 * `guest_es_ar_bytes` rather than `guest_es_access_rights`. Renaming them
 * would break the one property that makes this checkable against the
 * reference.
 */
namespace zpp::hypervisor::hyperv
{

/**
 * Which groups of fields the guest hypervisor has changed since the last
 * entry. A set bit means **clean** - unchanged, and safe not to re-read.
 *
 * This is the second half of what enlightened VMCS buys, and it is worth
 * as much as the first: every elision this VMM built by measurement - the
 * host-state cache, the deferred guest-state copy, the write-side hot
 * field elision - is a worse re-derivation of what the level above is
 * willing to simply state. KVM consumes these in
 * `copy_enlightened_to_vmcs12`, `.references/kvm/nested.c:1654`, one
 * `if` per group.
 *
 * From `hyperv-tlfs.h:769-787`.
 */
enum class clean_field : std::uint32_t
{
    none = 0,
    io_bitmap = 1u << 0,
    msr_bitmap = 1u << 1,
    control_group_2 = 1u << 2,
    control_group_1 = 1u << 3,
    control_processor = 1u << 4,
    control_event = 1u << 5,
    control_entry = 1u << 6,
    control_exception = 1u << 7,
    control_registers = 1u << 8,
    control_translation = 1u << 9,
    guest_basic = 1u << 10,
    guest_group_1 = 1u << 11,
    guest_group_2 = 1u << 12,
    host_pointer = 1u << 13,
    host_group_1 = 1u << 14,
    enlightenments_control = 1u << 15,
    all = 0xffff,
};

constexpr bool is_clean(std::uint32_t fields, clean_field which)
{
    return 0 != (fields & static_cast<std::uint32_t>(which));
}

/**
 * The enlightened VMCS itself.
 *
 * Transcribed field for field from the reference. `padding` members are
 * part of the layout and are named as the specification names them so
 * that the two can be compared line by line.
 */
struct enlightened_vmcs
{
    std::uint32_t revision_id;
    std::uint32_t abort;

    std::uint16_t host_es_selector;
    std::uint16_t host_cs_selector;
    std::uint16_t host_ss_selector;
    std::uint16_t host_ds_selector;
    std::uint16_t host_fs_selector;
    std::uint16_t host_gs_selector;
    std::uint16_t host_tr_selector;

    std::uint16_t padding16_1;

    std::uint64_t host_ia32_pat;
    std::uint64_t host_ia32_efer;

    std::uint64_t host_cr0;
    std::uint64_t host_cr3;
    std::uint64_t host_cr4;

    std::uint64_t host_ia32_sysenter_esp;
    std::uint64_t host_ia32_sysenter_eip;
    std::uint64_t host_rip;
    std::uint32_t host_ia32_sysenter_cs;

    std::uint32_t pin_based_vm_exec_control;
    std::uint32_t vm_exit_controls;
    std::uint32_t secondary_vm_exec_control;

    std::uint64_t io_bitmap_a;
    std::uint64_t io_bitmap_b;
    std::uint64_t msr_bitmap;

    std::uint16_t guest_es_selector;
    std::uint16_t guest_cs_selector;
    std::uint16_t guest_ss_selector;
    std::uint16_t guest_ds_selector;
    std::uint16_t guest_fs_selector;
    std::uint16_t guest_gs_selector;
    std::uint16_t guest_ldtr_selector;
    std::uint16_t guest_tr_selector;

    std::uint32_t guest_es_limit;
    std::uint32_t guest_cs_limit;
    std::uint32_t guest_ss_limit;
    std::uint32_t guest_ds_limit;
    std::uint32_t guest_fs_limit;
    std::uint32_t guest_gs_limit;
    std::uint32_t guest_ldtr_limit;
    std::uint32_t guest_tr_limit;
    std::uint32_t guest_gdtr_limit;
    std::uint32_t guest_idtr_limit;

    std::uint32_t guest_es_ar_bytes;
    std::uint32_t guest_cs_ar_bytes;
    std::uint32_t guest_ss_ar_bytes;
    std::uint32_t guest_ds_ar_bytes;
    std::uint32_t guest_fs_ar_bytes;
    std::uint32_t guest_gs_ar_bytes;
    std::uint32_t guest_ldtr_ar_bytes;
    std::uint32_t guest_tr_ar_bytes;

    std::uint64_t guest_es_base;
    std::uint64_t guest_cs_base;
    std::uint64_t guest_ss_base;
    std::uint64_t guest_ds_base;
    std::uint64_t guest_fs_base;
    std::uint64_t guest_gs_base;
    std::uint64_t guest_ldtr_base;
    std::uint64_t guest_tr_base;
    std::uint64_t guest_gdtr_base;
    std::uint64_t guest_idtr_base;

    std::uint64_t padding64_1[3];

    std::uint64_t vm_exit_msr_store_addr;
    std::uint64_t vm_exit_msr_load_addr;
    std::uint64_t vm_entry_msr_load_addr;

    std::uint64_t cr3_target_value0;
    std::uint64_t cr3_target_value1;
    std::uint64_t cr3_target_value2;
    std::uint64_t cr3_target_value3;

    std::uint32_t page_fault_error_code_mask;
    std::uint32_t page_fault_error_code_match;

    std::uint32_t cr3_target_count;
    std::uint32_t vm_exit_msr_store_count;
    std::uint32_t vm_exit_msr_load_count;
    std::uint32_t vm_entry_msr_load_count;

    std::uint64_t tsc_offset;
    std::uint64_t virtual_apic_page_addr;
    std::uint64_t vmcs_link_pointer;

    std::uint64_t guest_ia32_debugctl;
    std::uint64_t guest_ia32_pat;
    std::uint64_t guest_ia32_efer;

    std::uint64_t guest_pdptr0;
    std::uint64_t guest_pdptr1;
    std::uint64_t guest_pdptr2;
    std::uint64_t guest_pdptr3;

    std::uint64_t guest_pending_dbg_exceptions;
    std::uint64_t guest_sysenter_esp;
    std::uint64_t guest_sysenter_eip;

    std::uint32_t guest_activity_state;
    std::uint32_t guest_sysenter_cs;

    std::uint64_t cr0_guest_host_mask;
    std::uint64_t cr4_guest_host_mask;
    std::uint64_t cr0_read_shadow;
    std::uint64_t cr4_read_shadow;
    std::uint64_t guest_cr0;
    std::uint64_t guest_cr3;
    std::uint64_t guest_cr4;
    std::uint64_t guest_dr7;

    std::uint64_t host_fs_base;
    std::uint64_t host_gs_base;
    std::uint64_t host_tr_base;
    std::uint64_t host_gdtr_base;
    std::uint64_t host_idtr_base;
    std::uint64_t host_rsp;

    std::uint64_t ept_pointer;

    std::uint16_t virtual_processor_id;
    std::uint16_t padding16_2[3];

    std::uint64_t padding64_2[5];
    std::uint64_t guest_physical_address;

    std::uint32_t vm_instruction_error;
    std::uint32_t vm_exit_reason;
    std::uint32_t vm_exit_intr_info;
    std::uint32_t vm_exit_intr_error_code;
    std::uint32_t idt_vectoring_info_field;
    std::uint32_t idt_vectoring_error_code;
    std::uint32_t vm_exit_instruction_len;
    std::uint32_t vmx_instruction_info;

    std::uint64_t exit_qualification;
    std::uint64_t exit_io_instruction_ecx;
    std::uint64_t exit_io_instruction_esi;
    std::uint64_t exit_io_instruction_edi;
    std::uint64_t exit_io_instruction_eip;

    std::uint64_t guest_linear_address;
    std::uint64_t guest_rsp;
    std::uint64_t guest_rflags;

    std::uint32_t guest_interruptibility_info;
    std::uint32_t cpu_based_vm_exec_control;
    std::uint32_t exception_bitmap;
    std::uint32_t vm_entry_controls;
    std::uint32_t vm_entry_intr_info_field;
    std::uint32_t vm_entry_exception_error_code;
    std::uint32_t vm_entry_instruction_len;
    std::uint32_t tpr_threshold;

    std::uint64_t guest_rip;

    std::uint32_t hv_clean_fields;
    std::uint32_t hv_padding_32;
    std::uint32_t hv_synthetic_controls;

    /**
     * Spelled as a word rather than as the reference's bitfield, because
     * a bitfield's layout is implementation defined and this structure is
     * shared with another party. Bit 0 is `nested_flush_hypercall` and
     * bit 1 is `msr_bitmap`, which is the one KVM tests at
     * `.references/kvm/nested.c:646`.
     */
    std::uint32_t hv_enlightenments_control;
    std::uint32_t hv_vp_id;

    std::uint64_t hv_vm_id;
    std::uint64_t partition_assist_page;
    std::uint64_t padding64_4[4];
    std::uint64_t guest_bndcfgs;
    std::uint64_t padding64_5[7];
    std::uint64_t xss_exit_bitmap;
    std::uint64_t padding64_6[7];
};

/** Bit 1 of `hv_enlightenments_control`. */
constexpr std::uint32_t enlightenment_msr_bitmap = 1u << 1;

/**
 * The structure is shared with the guest hypervisor and must be exactly
 * what it expects. These do not prove the transcription correct - only a
 * real guest hypervisor writing into it does that - but they catch the
 * mistakes a transcription actually makes: a dropped padding member, a
 * width written as 32 where the specification says 64, a member added in
 * the wrong place.
 *
 * The offsets are the specification's own, and the last one doubles as
 * the size check: the structure occupies one page.
 */
static_assert(0 == offsetof(enlightened_vmcs, revision_id));
static_assert(8 == offsetof(enlightened_vmcs, host_es_selector));
static_assert(24 == offsetof(enlightened_vmcs, host_ia32_pat));
static_assert(624 == offsetof(enlightened_vmcs, ept_pointer));
static_assert(816 == offsetof(enlightened_vmcs, guest_rip));
static_assert(824 == offsetof(enlightened_vmcs, hv_clean_fields));

// The exact size rather than a bound, because a bound passes for a
// transcription that dropped a field and an exact figure does not. The
// guest hypervisor allocates a page for it and uses the first kilobyte.
static_assert(1024 == sizeof(enlightened_vmcs));

/**
 * The page through which the guest hypervisor arms all of this.
 *
 * From `hyperv-tlfs.h:573`. `enlighten_vmentry` is the switch and
 * `current_nested_vmcs` is the guest-physical address of the structure
 * above - **written here instead of being loaded with VMPTRLD**, which is
 * why arming it makes that instruction stop exiting.
 *
 * This VMM already reads this page: `HV_X64_MSR_VP_ASSIST_PAGE` is
 * intercepted and the address recorded, and `vtl_assist_read` reads it.
 * The hookup is therefore a use of something already in hand rather than
 * new machinery.
 */
struct vp_assist_page
{
    std::uint32_t apic_assist;
    std::uint32_t reserved1;
    std::uint64_t vtl_control[3];

    /**
     * `hv_nested_enlightenments_control` in the reference, which is two
     * 32-bit words - `features`, whose bit 0 is `directhypercall`, and a
     * reserved `hypercallControls`. Spelled as the two words rather than
     * as the reference's bitfields, because bitfield layout is
     * implementation defined and this page is shared.
     */
    std::uint32_t nested_features;
    std::uint32_t nested_hypercall_controls;

    std::uint8_t enlighten_vmentry;
    std::uint8_t reserved2[7];
    std::uint64_t current_nested_vmcs;
};

// These were guessed once and the assertions caught it: `nested_control`
// is at 32, not 40. Kept as a reminder that an offset nobody computed is
// an offset nobody checked - and here that is the difference between
// reading the guest hypervisor's switch and reading eight bytes of
// something else.
static_assert(0 == offsetof(vp_assist_page, apic_assist));
static_assert(32 == offsetof(vp_assist_page, nested_features));
static_assert(40 == offsetof(vp_assist_page, enlighten_vmentry));
static_assert(48 == offsetof(vp_assist_page, current_nested_vmcs));

} // namespace zpp::hypervisor::hyperv
