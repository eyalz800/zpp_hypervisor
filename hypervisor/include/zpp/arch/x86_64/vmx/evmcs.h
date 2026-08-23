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
/**
 * Every field the enlightened VMCS carries, as `{encoding, offset, size}`.
 *
 * Generated, and the source of truth for the lookup below rather than a
 * duplicate of it - the table is built from this array at compile time,
 * so the two cannot drift.
 */
struct evmcs_field_entry
{
    std::uint64_t encoding;
    std::uint16_t offset;
    std::uint8_t size;
};

inline constexpr evmcs_field_entry evmcs_field_table[] = {
    {0x0, 632, 2},  // vpid -> virtual_processor_id
    {0x800, 128, 2},  // guest_es_selector -> guest_es_selector
    {0x802, 130, 2},  // guest_cs_selector -> guest_cs_selector
    {0x804, 132, 2},  // guest_ss_selector -> guest_ss_selector
    {0x806, 134, 2},  // guest_ds_selector -> guest_ds_selector
    {0x808, 136, 2},  // guest_fs_selector -> guest_fs_selector
    {0x80a, 138, 2},  // guest_gs_selector -> guest_gs_selector
    {0x80c, 140, 2},  // guest_ldtr_selector -> guest_ldtr_selector
    {0x80e, 142, 2},  // guest_tr_selector -> guest_tr_selector
    {0xc00, 8, 2},  // host_es_selector -> host_es_selector
    {0xc02, 10, 2},  // host_cs_selector -> host_cs_selector
    {0xc04, 12, 2},  // host_ss_selector -> host_ss_selector
    {0xc06, 14, 2},  // host_ds_selector -> host_ds_selector
    {0xc08, 16, 2},  // host_fs_selector -> host_fs_selector
    {0xc0a, 18, 2},  // host_gs_selector -> host_gs_selector
    {0xc0c, 20, 2},  // host_tr_selector -> host_tr_selector
    {0x2000, 104, 8},  // io_bitmap_a -> io_bitmap_a
    {0x2002, 112, 8},  // io_bitmap_b -> io_bitmap_b
    {0x2004, 120, 8},  // msr_bitmap -> msr_bitmap
    {0x2006, 320, 8},  // vm_exit_msr_store_address -> vm_exit_msr_store_addr
    {0x2008, 328, 8},  // vm_exit_msr_load_address -> vm_exit_msr_load_addr
    {0x200a, 336, 8},  // vm_entry_msr_load_address -> vm_entry_msr_load_addr
    {0x2010, 400, 8},  // tsc_offset -> tsc_offset
    {0x2012, 408, 8},  // virtual_apic_address -> virtual_apic_page_addr
    {0x201a, 624, 8},  // ept_pointer -> ept_pointer
    {0x202c, 960, 8},  // xss_exiting_bitmap -> xss_exit_bitmap
    {0x202e, 968, 8},  // encls_exiting_bitmap -> encls_exiting_bitmap
    {0x2032, 984, 8},  // tsc_multiplier -> tsc_multiplier
    {0x2400, 680, 8},  // guest_physical_address -> guest_physical_address
    {0x2800, 416, 8},  // vmcs_link_pointer -> vmcs_link_pointer
    {0x2802, 424, 8},  // guest_ia32_debugctl -> guest_ia32_debugctl
    {0x2804, 432, 8},  // guest_ia32_pat -> guest_ia32_pat
    {0x2806, 440, 8},  // guest_ia32_efer -> guest_ia32_efer
    {0x2808, 904, 8},  // guest_ia32_perf_global_ctrl -> guest_ia32_perf_global_ctrl
    {0x280a, 448, 8},  // guest_pdpte_0 -> guest_pdptr0
    {0x280c, 456, 8},  // guest_pdpte_1 -> guest_pdptr1
    {0x280e, 464, 8},  // guest_pdpte_2 -> guest_pdptr2
    {0x2810, 472, 8},  // guest_pdpte_3 -> guest_pdptr3
    {0x2812, 896, 8},  // guest_ia32_bndcfgs -> guest_bndcfgs
    {0x2c00, 24, 8},  // host_ia32_pat -> host_ia32_pat
    {0x2c02, 32, 8},  // host_ia32_efer -> host_ia32_efer
    {0x2c04, 976, 8},  // host_ia32_perf_global_ctrl -> host_ia32_perf_global_ctrl
    {0x4000, 92, 4},  // pin_based_vm_execution_controls -> pin_based_vm_exec_control
    {0x4002, 788, 4},  // primary_processor_based_vm_execution_controls -> cpu_based_vm_exec_control
    {0x4004, 792, 4},  // exception_bitmap -> exception_bitmap
    {0x4006, 376, 4},  // page_fault_error_code_mask -> page_fault_error_code_mask
    {0x4008, 380, 4},  // page_fault_error_code_match -> page_fault_error_code_match
    {0x400a, 384, 4},  // cr3_target_count -> cr3_target_count
    {0x400c, 96, 4},  // vm_exit_controls -> vm_exit_controls
    {0x400e, 388, 4},  // vm_exit_msr_store_count -> vm_exit_msr_store_count
    {0x4010, 392, 4},  // vm_exit_msr_load_count -> vm_exit_msr_load_count
    {0x4012, 796, 4},  // vm_entry_controls -> vm_entry_controls
    {0x4014, 396, 4},  // vm_entry_msr_load_count -> vm_entry_msr_load_count
    {0x4016, 800, 4},  // vm_entry_interruption_information_field -> vm_entry_intr_info_field
    {0x4018, 804, 4},  // vm_entry_exception_error_code -> vm_entry_exception_error_code
    {0x401a, 808, 4},  // vm_entry_instruction_length -> vm_entry_instruction_len
    {0x401c, 812, 4},  // tpr_threshold -> tpr_threshold
    {0x401e, 100, 4},  // secondary_processor_based_vm_execution_controls -> secondary_vm_exec_control
    {0x4400, 688, 4},  // vm_instruction_error -> vm_instruction_error
    {0x4402, 692, 4},  // exit_reason -> vm_exit_reason
    {0x4404, 696, 4},  // vm_exit_interruption_information -> vm_exit_intr_info
    {0x4406, 700, 4},  // vm_exit_interruption_error_code -> vm_exit_intr_error_code
    {0x4408, 704, 4},  // idt_vectoring_information_field -> idt_vectoring_info_field
    {0x440a, 708, 4},  // idt_vectoring_error_code -> idt_vectoring_error_code
    {0x440c, 712, 4},  // vm_exit_instruction_length -> vm_exit_instruction_len
    {0x440e, 716, 4},  // vm_exit_instruction_information -> vmx_instruction_info
    {0x4800, 144, 4},  // guest_es_limit -> guest_es_limit
    {0x4802, 148, 4},  // guest_cs_limit -> guest_cs_limit
    {0x4804, 152, 4},  // guest_ss_limit -> guest_ss_limit
    {0x4806, 156, 4},  // guest_ds_limit -> guest_ds_limit
    {0x4808, 160, 4},  // guest_fs_limit -> guest_fs_limit
    {0x480a, 164, 4},  // guest_gs_limit -> guest_gs_limit
    {0x480c, 168, 4},  // guest_ldtr_limit -> guest_ldtr_limit
    {0x480e, 172, 4},  // guest_tr_limit -> guest_tr_limit
    {0x4810, 176, 4},  // guest_gdtr_limit -> guest_gdtr_limit
    {0x4812, 180, 4},  // guest_idtr_limit -> guest_idtr_limit
    {0x4814, 184, 4},  // guest_es_access_rights -> guest_es_ar_bytes
    {0x4816, 188, 4},  // guest_cs_access_rights -> guest_cs_ar_bytes
    {0x4818, 192, 4},  // guest_ss_access_rights -> guest_ss_ar_bytes
    {0x481a, 196, 4},  // guest_ds_access_rights -> guest_ds_ar_bytes
    {0x481c, 200, 4},  // guest_fs_access_rights -> guest_fs_ar_bytes
    {0x481e, 204, 4},  // guest_gs_access_rights -> guest_gs_ar_bytes
    {0x4820, 208, 4},  // guest_ldtr_access_rights -> guest_ldtr_ar_bytes
    {0x4822, 212, 4},  // guest_tr_access_rights -> guest_tr_ar_bytes
    {0x4824, 784, 4},  // guest_interruptibility_state -> guest_interruptibility_info
    {0x4826, 504, 4},  // guest_activity_state -> guest_activity_state
    {0x482a, 508, 4},  // guest_ia32_sysenter_cs -> guest_sysenter_cs
    {0x4c00, 88, 4},  // host_ia32_sysenter_cs -> host_ia32_sysenter_cs
    {0x6000, 512, 8},  // cr0_guest_host_mask -> cr0_guest_host_mask
    {0x6002, 520, 8},  // cr4_guest_host_mask -> cr4_guest_host_mask
    {0x6004, 528, 8},  // cr0_read_shadow -> cr0_read_shadow
    {0x6006, 536, 8},  // cr4_read_shadow -> cr4_read_shadow
    {0x6008, 344, 8},  // cr3_target_value_0 -> cr3_target_value0
    {0x600a, 352, 8},  // cr3_target_value_1 -> cr3_target_value1
    {0x600c, 360, 8},  // cr3_target_value_2 -> cr3_target_value2
    {0x600e, 368, 8},  // cr3_target_value_3 -> cr3_target_value3
    {0x6400, 720, 8},  // exit_qualification -> exit_qualification
    {0x6402, 728, 8},  // io_rcx -> exit_io_instruction_ecx
    {0x6404, 736, 8},  // io_rsi -> exit_io_instruction_esi
    {0x6406, 744, 8},  // io_rdi -> exit_io_instruction_edi
    {0x6408, 752, 8},  // io_rip -> exit_io_instruction_eip
    {0x640a, 760, 8},  // guest_linear_address -> guest_linear_address
    {0x6800, 544, 8},  // guest_cr0 -> guest_cr0
    {0x6802, 552, 8},  // guest_cr3 -> guest_cr3
    {0x6804, 560, 8},  // guest_cr4 -> guest_cr4
    {0x6806, 216, 8},  // guest_es_base -> guest_es_base
    {0x6808, 224, 8},  // guest_cs_base -> guest_cs_base
    {0x680a, 232, 8},  // guest_ss_base -> guest_ss_base
    {0x680c, 240, 8},  // guest_ds_base -> guest_ds_base
    {0x680e, 248, 8},  // guest_fs_base -> guest_fs_base
    {0x6810, 256, 8},  // guest_gs_base -> guest_gs_base
    {0x6812, 264, 8},  // guest_ldtr_base -> guest_ldtr_base
    {0x6814, 272, 8},  // guest_tr_base -> guest_tr_base
    {0x6816, 280, 8},  // guest_gdtr_base -> guest_gdtr_base
    {0x6818, 288, 8},  // guest_idtr_base -> guest_idtr_base
    {0x681a, 568, 8},  // guest_dr7 -> guest_dr7
    {0x681c, 768, 8},  // guest_rsp -> guest_rsp
    {0x681e, 816, 8},  // guest_rip -> guest_rip
    {0x6820, 776, 8},  // guest_rflags -> guest_rflags
    {0x6822, 480, 8},  // guest_pending_debug_exceptions -> guest_pending_dbg_exceptions
    {0x6824, 488, 8},  // guest_ia32_sysenter_esp -> guest_sysenter_esp
    {0x6826, 496, 8},  // guest_ia32_sysenter_eip -> guest_sysenter_eip
    {0x6c00, 40, 8},  // host_cr0 -> host_cr0
    {0x6c02, 48, 8},  // host_cr3 -> host_cr3
    {0x6c04, 56, 8},  // host_cr4 -> host_cr4
    {0x6c06, 576, 8},  // host_fs_base -> host_fs_base
    {0x6c08, 584, 8},  // host_gs_base -> host_gs_base
    {0x6c0a, 592, 8},  // host_tr_base -> host_tr_base
    {0x6c0c, 600, 8},  // host_gdtr_base -> host_gdtr_base
    {0x6c0e, 608, 8},  // host_idtr_base -> host_idtr_base
    {0x6c10, 64, 8},  // host_ia32_sysenter_esp -> host_ia32_sysenter_esp
    {0x6c12, 72, 8},  // host_ia32_sysenter_eip -> host_ia32_sysenter_eip
    {0x6c14, 616, 8},  // host_rsp -> host_rsp
    {0x6c16, 80, 8},  // host_rip -> host_rip
};

/**
 * The same list as a direct-mapped table, because the switch this
 * replaces was the single largest cost in the enlightened configuration.
 *
 * **Measured, and it is the whole exit.** With the enlightened VMCS in
 * use a field access is a load, so the price should be a load - and it
 * was **414 cycles**. A `vmread` reflected from the guest hypervisor
 * takes 17.4 reads and 2 writes, and 19.4 x 414 is 8,032 against the
 * 8,027 cycles that exit actually cost. The per-access overhead *was*
 * the exit, and 46.4% of this VMM's handler time was two exit reasons
 * made of nothing else.
 *
 * A 134-case switch over sparse encodings is not a jump table; the
 * compiler emits a binary search, and its branches are unpredictable
 * because consecutive accesses ask for unrelated fields.
 *
 * **The shape is free.** A VMCS encoding's bit 0 is the access type and
 * is zero for every field here, bits 8:1 index within a group and bits
 * 15:9 name the group. Both together reconstruct the encoding exactly,
 * so the mapping is injective and no entry can shadow another. Measured
 * over the real list: 55 groups, and the widest holds 26 fields - so a
 * 55 x 32 table of `std::uint16_t` is **3,520 bytes**, small enough to
 * stay in cache, against a search that was not.
 *
 * Offset and size are packed together because the offset is at most 984
 * and the size is one of 2, 4 and 8: `(offset << 4) | size`.
 * @{
 */
inline constexpr std::size_t evmcs_map_groups = 55;
inline constexpr std::size_t evmcs_map_slots = 32;

struct evmcs_map
{
    std::uint16_t packed[evmcs_map_groups][evmcs_map_slots]{};
};

consteval evmcs_map evmcs_build_map()
{
    evmcs_map map{};

    for (auto entry : evmcs_field_table) {
        auto group = entry.encoding >> 9;
        auto slot = (entry.encoding & 0x1ff) >> 1;

        map.packed[group][slot] =
            static_cast<std::uint16_t>((entry.offset << 4) | entry.size);
    }

    return map;
}

inline constexpr evmcs_map evmcs_offsets = evmcs_build_map();
/** @} */

/**
 * Where a VMCS field encoding lives in the enlightened VMCS.
 *
 * Returns `{0, 0}` for an encoding the enlightened VMCS does not carry,
 * which includes every encoding outside the table's shape - an odd one
 * (the high half of a 64-bit field), a group past the last, or an index
 * past the widest group. None of those name a field the structure has.
 */
constexpr evmcs_slot evmcs_offset_of(std::uint64_t encoding)
{
    auto group = encoding >> 9;
    auto slot = (encoding & 0x1ff) >> 1;

    if ((0 != (encoding & 1)) || (group >= evmcs_map_groups) ||
        (slot >= evmcs_map_slots)) {
        return {0, 0};
    }

    auto packed = evmcs_offsets.packed[group][slot];

    return {static_cast<std::uint16_t>(packed >> 4),
            static_cast<std::uint16_t>(packed & 0xf)};
}

// **Every entry, not a sample.** The table replaced a 134-case switch,
// and a lookup that agrees with it on five anchors and disagrees on the
// other 129 would be silent: a wrong offset reads a neighbouring field,
// which is a plausible value. This checks the whole list at compile time,
// so the replacement is proved rather than spot-checked.
consteval bool evmcs_map_matches_table()
{
    for (auto entry : evmcs_field_table) {
        auto found = evmcs_offset_of(entry.encoding);

        if ((found.offset != entry.offset) ||
            (found.size != entry.size)) {
            return false;
        }
    }

    return true;
}

static_assert(evmcs_map_matches_table());

// Anchors, checked by hand against the structure rather than against the
// generator that produced the rest. `host_rip` is the fourth quadword
// after seven selectors and their padding: 4 + 4 + 14 + 2 + 7 * 8 = 80.
static_assert(80 == evmcs_offset_of(vmcs_fields::host_rip).offset);
static_assert(8 == evmcs_offset_of(vmcs_fields::host_rip).size);
static_assert(0 == evmcs_offset_of(vmcs_fields::pml_address).size);
static_assert(0 == evmcs_offset_of(vmcs_fields::vmread_bitmap_address).size);
static_assert(2 == evmcs_offset_of(vmcs_fields::vpid).size);

} // namespace zpp::arch::x86_64::vmx
