#pragma once
#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * Where each VMCS field lives inside Hyper-V's *enlightened* VMCS.
 *
 * **Why this exists.** On a processor whose layer below will not use a
 * shadow VMCS on our behalf, every VMREAD and VMWRITE this VMM executes
 * is an exit to that layer - measured at about 4,600 cycles, against some
 * thirty-six accesses an exit, which is essentially the whole cost of an
 * exit. The enlightened VMCS replaces those instructions with plain loads
 * and stores to a shared page, so the cost does not get smaller, it stops
 * being paid.
 *
 * **This table is generated, not typed.** It is a packed ABI of about a
 * hundred and fifty fields, and a single transcription slip puts a field
 * at a plausible wrong offset, where it is read as data rather than as an
 * error. The offsets below are accumulated from `struct
 * hv_enlightened_vmcs` in declaration order, so the only thing that can
 * be wrong is the order, and that is checkable by reading it once.
 *
 * The field names are matched against that structure's *member names*,
 * which is the same key KVM's `EVMCS1_FIELD` uses, so the two tables
 * agree by construction rather than by coincidence.
 *
 * **Not every field is here, and that is the ABI's doing.** Twenty-two of
 * this tree's encodings have no home in the enlightened VMCS: the posted
 * interrupt fields, the page-modification log, VMFUNC and its EPTP list,
 * sub-page permissions, the APIC-access page, and the VMREAD and VMWRITE
 * bitmaps. This VMM uses none of them on the second-level path except the
 * bitmaps, and those are VMCS shadowing, which cannot be combined with
 * this - KVM refuses the same combination in `nested.c`. A field with no
 * entry returns `size == 0`, and the caller must fall back to a real
 * VMCS rather than guess.
 */
struct evmcs_slot
{
    /** Byte offset into the enlightened VMCS page. */
    std::uint16_t offset;

    /** Width in bytes, or zero when the field has no enlightened home. */
    std::uint16_t size;
};

/**
 * The enlightened VMCS revision, which the layer below checks first.
 * `HV_VMX_ENLIGHTENED_VMCS_VERSION` in the kernel's headers.
 */
inline constexpr std::uint32_t evmcs_revision = 0x1;

/** Offset of `hv_clean_fields`, which says what the layer below may keep. */
inline constexpr std::size_t evmcs_clean_fields_offset = 824;

/**
 * Zero means "every field is dirty, re-read all of them".
 *
 * Deliberately the only value this VMM writes for now. The clean-fields
 * bitmap is an optimisation on top of the optimisation - it lets the
 * layer below skip re-reading groups that did not change - and getting a
 * bit wrong means it reads a stale field and runs the guest with it,
 * which is silent. Correct first.
 */
inline constexpr std::uint32_t evmcs_all_dirty = 0;

/**
 * The size of the defined part of the structure. The page is a full page;
 * only the first kilobyte carries fields.
 */
inline constexpr std::size_t evmcs_defined_size = 1024;

/**
 * Where a VMCS field encoding lives in the enlightened VMCS.
 *
 * Returns `{0, 0}` for an encoding the enlightened VMCS does not carry.
 */
constexpr evmcs_slot evmcs_offset_of(std::uint64_t encoding)
{
    switch (encoding) {
    case 0x0: return {632, 2};  // vpid -> virtual_processor_id
    case 0x800: return {128, 2};  // guest_es_selector -> guest_es_selector
    case 0x802: return {130, 2};  // guest_cs_selector -> guest_cs_selector
    case 0x804: return {132, 2};  // guest_ss_selector -> guest_ss_selector
    case 0x806: return {134, 2};  // guest_ds_selector -> guest_ds_selector
    case 0x808: return {136, 2};  // guest_fs_selector -> guest_fs_selector
    case 0x80a: return {138, 2};  // guest_gs_selector -> guest_gs_selector
    case 0x80c: return {140, 2};  // guest_ldtr_selector -> guest_ldtr_selector
    case 0x80e: return {142, 2};  // guest_tr_selector -> guest_tr_selector
    case 0xc00: return {8, 2};  // host_es_selector -> host_es_selector
    case 0xc02: return {10, 2};  // host_cs_selector -> host_cs_selector
    case 0xc04: return {12, 2};  // host_ss_selector -> host_ss_selector
    case 0xc06: return {14, 2};  // host_ds_selector -> host_ds_selector
    case 0xc08: return {16, 2};  // host_fs_selector -> host_fs_selector
    case 0xc0a: return {18, 2};  // host_gs_selector -> host_gs_selector
    case 0xc0c: return {20, 2};  // host_tr_selector -> host_tr_selector
    case 0x2000: return {104, 8};  // io_bitmap_a -> io_bitmap_a
    case 0x2002: return {112, 8};  // io_bitmap_b -> io_bitmap_b
    case 0x2004: return {120, 8};  // msr_bitmap -> msr_bitmap
    case 0x2006: return {320, 8};  // vm_exit_msr_store_address -> vm_exit_msr_store_addr
    case 0x2008: return {328, 8};  // vm_exit_msr_load_address -> vm_exit_msr_load_addr
    case 0x200a: return {336, 8};  // vm_entry_msr_load_address -> vm_entry_msr_load_addr
    case 0x2010: return {400, 8};  // tsc_offset -> tsc_offset
    case 0x2012: return {408, 8};  // virtual_apic_address -> virtual_apic_page_addr
    case 0x201a: return {624, 8};  // ept_pointer -> ept_pointer
    case 0x202c: return {960, 8};  // xss_exiting_bitmap -> xss_exit_bitmap
    case 0x202e: return {968, 8};  // encls_exiting_bitmap -> encls_exiting_bitmap
    case 0x2032: return {984, 8};  // tsc_multiplier -> tsc_multiplier
    case 0x2400: return {680, 8};  // guest_physical_address -> guest_physical_address
    case 0x2800: return {416, 8};  // vmcs_link_pointer -> vmcs_link_pointer
    case 0x2802: return {424, 8};  // guest_ia32_debugctl -> guest_ia32_debugctl
    case 0x2804: return {432, 8};  // guest_ia32_pat -> guest_ia32_pat
    case 0x2806: return {440, 8};  // guest_ia32_efer -> guest_ia32_efer
    case 0x2808: return {904, 8};  // guest_ia32_perf_global_ctrl -> guest_ia32_perf_global_ctrl
    case 0x280a: return {448, 8};  // guest_pdpte_0 -> guest_pdptr0
    case 0x280c: return {456, 8};  // guest_pdpte_1 -> guest_pdptr1
    case 0x280e: return {464, 8};  // guest_pdpte_2 -> guest_pdptr2
    case 0x2810: return {472, 8};  // guest_pdpte_3 -> guest_pdptr3
    case 0x2812: return {896, 8};  // guest_ia32_bndcfgs -> guest_bndcfgs
    case 0x2c00: return {24, 8};  // host_ia32_pat -> host_ia32_pat
    case 0x2c02: return {32, 8};  // host_ia32_efer -> host_ia32_efer
    case 0x2c04: return {976, 8};  // host_ia32_perf_global_ctrl -> host_ia32_perf_global_ctrl
    case 0x4000: return {92, 4};  // pin_based_vm_execution_controls -> pin_based_vm_exec_control
    case 0x4002: return {788, 4};  // primary_processor_based_vm_execution_controls -> cpu_based_vm_exec_control
    case 0x4004: return {792, 4};  // exception_bitmap -> exception_bitmap
    case 0x4006: return {376, 4};  // page_fault_error_code_mask -> page_fault_error_code_mask
    case 0x4008: return {380, 4};  // page_fault_error_code_match -> page_fault_error_code_match
    case 0x400a: return {384, 4};  // cr3_target_count -> cr3_target_count
    case 0x400c: return {96, 4};  // vm_exit_controls -> vm_exit_controls
    case 0x400e: return {388, 4};  // vm_exit_msr_store_count -> vm_exit_msr_store_count
    case 0x4010: return {392, 4};  // vm_exit_msr_load_count -> vm_exit_msr_load_count
    case 0x4012: return {796, 4};  // vm_entry_controls -> vm_entry_controls
    case 0x4014: return {396, 4};  // vm_entry_msr_load_count -> vm_entry_msr_load_count
    case 0x4016: return {800, 4};  // vm_entry_interruption_information_field -> vm_entry_intr_info_field
    case 0x4018: return {804, 4};  // vm_entry_exception_error_code -> vm_entry_exception_error_code
    case 0x401a: return {808, 4};  // vm_entry_instruction_length -> vm_entry_instruction_len
    case 0x401c: return {812, 4};  // tpr_threshold -> tpr_threshold
    case 0x401e: return {100, 4};  // secondary_processor_based_vm_execution_controls -> secondary_vm_exec_control
    case 0x4400: return {688, 4};  // vm_instruction_error -> vm_instruction_error
    case 0x4402: return {692, 4};  // exit_reason -> vm_exit_reason
    case 0x4404: return {696, 4};  // vm_exit_interruption_information -> vm_exit_intr_info
    case 0x4406: return {700, 4};  // vm_exit_interruption_error_code -> vm_exit_intr_error_code
    case 0x4408: return {704, 4};  // idt_vectoring_information_field -> idt_vectoring_info_field
    case 0x440a: return {708, 4};  // idt_vectoring_error_code -> idt_vectoring_error_code
    case 0x440c: return {712, 4};  // vm_exit_instruction_length -> vm_exit_instruction_len
    case 0x440e: return {716, 4};  // vm_exit_instruction_information -> vmx_instruction_info
    case 0x4800: return {144, 4};  // guest_es_limit -> guest_es_limit
    case 0x4802: return {148, 4};  // guest_cs_limit -> guest_cs_limit
    case 0x4804: return {152, 4};  // guest_ss_limit -> guest_ss_limit
    case 0x4806: return {156, 4};  // guest_ds_limit -> guest_ds_limit
    case 0x4808: return {160, 4};  // guest_fs_limit -> guest_fs_limit
    case 0x480a: return {164, 4};  // guest_gs_limit -> guest_gs_limit
    case 0x480c: return {168, 4};  // guest_ldtr_limit -> guest_ldtr_limit
    case 0x480e: return {172, 4};  // guest_tr_limit -> guest_tr_limit
    case 0x4810: return {176, 4};  // guest_gdtr_limit -> guest_gdtr_limit
    case 0x4812: return {180, 4};  // guest_idtr_limit -> guest_idtr_limit
    case 0x4814: return {184, 4};  // guest_es_access_rights -> guest_es_ar_bytes
    case 0x4816: return {188, 4};  // guest_cs_access_rights -> guest_cs_ar_bytes
    case 0x4818: return {192, 4};  // guest_ss_access_rights -> guest_ss_ar_bytes
    case 0x481a: return {196, 4};  // guest_ds_access_rights -> guest_ds_ar_bytes
    case 0x481c: return {200, 4};  // guest_fs_access_rights -> guest_fs_ar_bytes
    case 0x481e: return {204, 4};  // guest_gs_access_rights -> guest_gs_ar_bytes
    case 0x4820: return {208, 4};  // guest_ldtr_access_rights -> guest_ldtr_ar_bytes
    case 0x4822: return {212, 4};  // guest_tr_access_rights -> guest_tr_ar_bytes
    case 0x4824: return {784, 4};  // guest_interruptibility_state -> guest_interruptibility_info
    case 0x4826: return {504, 4};  // guest_activity_state -> guest_activity_state
    case 0x482a: return {508, 4};  // guest_ia32_sysenter_cs -> guest_sysenter_cs
    case 0x4c00: return {88, 4};  // host_ia32_sysenter_cs -> host_ia32_sysenter_cs
    case 0x6000: return {512, 8};  // cr0_guest_host_mask -> cr0_guest_host_mask
    case 0x6002: return {520, 8};  // cr4_guest_host_mask -> cr4_guest_host_mask
    case 0x6004: return {528, 8};  // cr0_read_shadow -> cr0_read_shadow
    case 0x6006: return {536, 8};  // cr4_read_shadow -> cr4_read_shadow
    case 0x6008: return {344, 8};  // cr3_target_value_0 -> cr3_target_value0
    case 0x600a: return {352, 8};  // cr3_target_value_1 -> cr3_target_value1
    case 0x600c: return {360, 8};  // cr3_target_value_2 -> cr3_target_value2
    case 0x600e: return {368, 8};  // cr3_target_value_3 -> cr3_target_value3
    case 0x6400: return {720, 8};  // exit_qualification -> exit_qualification
    case 0x6402: return {728, 8};  // io_rcx -> exit_io_instruction_ecx
    case 0x6404: return {736, 8};  // io_rsi -> exit_io_instruction_esi
    case 0x6406: return {744, 8};  // io_rdi -> exit_io_instruction_edi
    case 0x6408: return {752, 8};  // io_rip -> exit_io_instruction_eip
    case 0x640a: return {760, 8};  // guest_linear_address -> guest_linear_address
    case 0x6800: return {544, 8};  // guest_cr0 -> guest_cr0
    case 0x6802: return {552, 8};  // guest_cr3 -> guest_cr3
    case 0x6804: return {560, 8};  // guest_cr4 -> guest_cr4
    case 0x6806: return {216, 8};  // guest_es_base -> guest_es_base
    case 0x6808: return {224, 8};  // guest_cs_base -> guest_cs_base
    case 0x680a: return {232, 8};  // guest_ss_base -> guest_ss_base
    case 0x680c: return {240, 8};  // guest_ds_base -> guest_ds_base
    case 0x680e: return {248, 8};  // guest_fs_base -> guest_fs_base
    case 0x6810: return {256, 8};  // guest_gs_base -> guest_gs_base
    case 0x6812: return {264, 8};  // guest_ldtr_base -> guest_ldtr_base
    case 0x6814: return {272, 8};  // guest_tr_base -> guest_tr_base
    case 0x6816: return {280, 8};  // guest_gdtr_base -> guest_gdtr_base
    case 0x6818: return {288, 8};  // guest_idtr_base -> guest_idtr_base
    case 0x681a: return {568, 8};  // guest_dr7 -> guest_dr7
    case 0x681c: return {768, 8};  // guest_rsp -> guest_rsp
    case 0x681e: return {816, 8};  // guest_rip -> guest_rip
    case 0x6820: return {776, 8};  // guest_rflags -> guest_rflags
    case 0x6822: return {480, 8};  // guest_pending_debug_exceptions -> guest_pending_dbg_exceptions
    case 0x6824: return {488, 8};  // guest_ia32_sysenter_esp -> guest_sysenter_esp
    case 0x6826: return {496, 8};  // guest_ia32_sysenter_eip -> guest_sysenter_eip
    case 0x6c00: return {40, 8};  // host_cr0 -> host_cr0
    case 0x6c02: return {48, 8};  // host_cr3 -> host_cr3
    case 0x6c04: return {56, 8};  // host_cr4 -> host_cr4
    case 0x6c06: return {576, 8};  // host_fs_base -> host_fs_base
    case 0x6c08: return {584, 8};  // host_gs_base -> host_gs_base
    case 0x6c0a: return {592, 8};  // host_tr_base -> host_tr_base
    case 0x6c0c: return {600, 8};  // host_gdtr_base -> host_gdtr_base
    case 0x6c0e: return {608, 8};  // host_idtr_base -> host_idtr_base
    case 0x6c10: return {64, 8};  // host_ia32_sysenter_esp -> host_ia32_sysenter_esp
    case 0x6c12: return {72, 8};  // host_ia32_sysenter_eip -> host_ia32_sysenter_eip
    case 0x6c14: return {616, 8};  // host_rsp -> host_rsp
    case 0x6c16: return {80, 8};  // host_rip -> host_rip
    default: return {0, 0};
    }
}

// Anchors, checked by hand against the structure rather than against the
// generator that produced the rest. `host_rip` is the fourth quadword
// after seven selectors and their padding: 4 + 4 + 14 + 2 + 7 * 8 = 80.
static_assert(80 == evmcs_offset_of(vmcs_fields::host_rip).offset);
static_assert(8 == evmcs_offset_of(vmcs_fields::host_rip).size);
static_assert(0 == evmcs_offset_of(vmcs_fields::pml_address).size);
static_assert(0 == evmcs_offset_of(vmcs_fields::vmread_bitmap_address).size);
static_assert(2 == evmcs_offset_of(vmcs_fields::vpid).size);

} // namespace zpp::arch::x86_64::vmx
