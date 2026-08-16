#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
#include "zpp/hypervisor/enlightened_vmcs.h"
#include "zpp/hypervisor/hypervisor.h"
#include <cstdint>

namespace zpp::hypervisor
{

using field = arch::x86_64::vmx::vmcs::field;

/**
 * Reads an enlightened VMCS into this VMM's cached vmcs12.
 *
 * **What this replaces.** Without it the guest hypervisor tells this VMM
 * its VMCS one field at a time, by executing VMREAD and VMWRITE that trap
 * - measured at 54% of a trust-level round trip. With it, the guest
 * hypervisor writes the structure in memory and executes none of them,
 * and this VMM reads the whole thing once per entry.
 *
 * **Every field is copied unconditionally in this version, and that is
 * deliberate.** The structure carries `hv_clean_fields`, a bitmap of
 * which groups the guest hypervisor has *not* changed, and honouring it
 * is worth real work - KVM does, in `copy_enlightened_to_vmcs12`,
 * `.references/kvm/nested.c:1654`. But a field placed in the wrong group
 * silently stops being refreshed when it changes, which is corruption of
 * the second-level guest rather than a fault, and this file's record on
 * silent-wrong-value bugs is that they cost boots and reset loops.
 *
 * The gating is therefore a later, incremental change - one group at a
 * time, each verified against the reference - and the win that matters
 * does not depend on it: the exits disappear because the guest hypervisor
 * stops executing the instructions, not because of how much this VMM
 * copies afterwards.
 *
 * The two fields KVM copies outside every group are copied first here for
 * the same reason it does: the guest hypervisor is permitted to change
 * them without marking any group dirty.
 */
void hypervisor::copy_enlightened_to_vmcs12(
    std::size_t cpu, const hyperv::enlightened_vmcs & evmcs)
{
    if (cpu >= max_cpus) {
        return;
    }

    auto & shadow = this->guest_vmcs12[cpu];

    auto put = [&](field which, std::uint64_t value) {
        shadow.write(which, value);
    };

    // Outside every clean-field group, exactly as KVM treats them.
    put(field::tpr_threshold, evmcs.tpr_threshold);
    put(field::guest_rip, evmcs.guest_rip);

    // Guest basic state.
    put(field::guest_rsp, evmcs.guest_rsp);
    put(field::guest_rflags, evmcs.guest_rflags);
    put(field::guest_interruptibility_state,
        evmcs.guest_interruptibility_info);

    // Controls.
    put(field::primary_processor_based_vm_execution_controls,
        evmcs.cpu_based_vm_exec_control);
    put(field::pin_based_vm_execution_controls,
        evmcs.pin_based_vm_exec_control);
    put(field::secondary_processor_based_vm_execution_controls,
        evmcs.secondary_vm_exec_control);
    put(field::vm_exit_controls, evmcs.vm_exit_controls);
    put(field::vm_entry_controls, evmcs.vm_entry_controls);
    put(field::exception_bitmap, evmcs.exception_bitmap);
    put(field::page_fault_error_code_mask,
        evmcs.page_fault_error_code_mask);
    put(field::page_fault_error_code_match,
        evmcs.page_fault_error_code_match);

    // Event injection.
    put(field::vm_entry_interruption_information_field,
        evmcs.vm_entry_intr_info_field);
    put(field::vm_entry_exception_error_code,
        evmcs.vm_entry_exception_error_code);
    put(field::vm_entry_instruction_length,
        evmcs.vm_entry_instruction_len);

    // Address-space controls.
    put(field::ept_pointer, evmcs.ept_pointer);
    put(field::vpid, evmcs.virtual_processor_id);
    put(field::vmcs_link_pointer, evmcs.vmcs_link_pointer);
    put(field::tsc_offset, evmcs.tsc_offset);
    put(field::virtual_apic_address, evmcs.virtual_apic_page_addr);
    put(field::xss_exiting_bitmap, evmcs.xss_exit_bitmap);

    // Bitmaps.
    put(field::io_bitmap_a, evmcs.io_bitmap_a);
    put(field::io_bitmap_b, evmcs.io_bitmap_b);
    put(field::msr_bitmap, evmcs.msr_bitmap);

    // Control registers and their shadows.
    put(field::cr0_guest_host_mask, evmcs.cr0_guest_host_mask);
    put(field::cr4_guest_host_mask, evmcs.cr4_guest_host_mask);
    put(field::cr0_read_shadow, evmcs.cr0_read_shadow);
    put(field::cr4_read_shadow, evmcs.cr4_read_shadow);
    put(field::guest_cr0, evmcs.guest_cr0);
    put(field::guest_cr3, evmcs.guest_cr3);
    put(field::guest_cr4, evmcs.guest_cr4);
    put(field::guest_dr7, evmcs.guest_dr7);

    // Guest segment selectors, limits, access rights and bases.
    put(field::guest_es_selector, evmcs.guest_es_selector);
    put(field::guest_cs_selector, evmcs.guest_cs_selector);
    put(field::guest_ss_selector, evmcs.guest_ss_selector);
    put(field::guest_ds_selector, evmcs.guest_ds_selector);
    put(field::guest_fs_selector, evmcs.guest_fs_selector);
    put(field::guest_gs_selector, evmcs.guest_gs_selector);
    put(field::guest_ldtr_selector, evmcs.guest_ldtr_selector);
    put(field::guest_tr_selector, evmcs.guest_tr_selector);

    put(field::guest_es_limit, evmcs.guest_es_limit);
    put(field::guest_cs_limit, evmcs.guest_cs_limit);
    put(field::guest_ss_limit, evmcs.guest_ss_limit);
    put(field::guest_ds_limit, evmcs.guest_ds_limit);
    put(field::guest_fs_limit, evmcs.guest_fs_limit);
    put(field::guest_gs_limit, evmcs.guest_gs_limit);
    put(field::guest_ldtr_limit, evmcs.guest_ldtr_limit);
    put(field::guest_tr_limit, evmcs.guest_tr_limit);
    put(field::guest_gdtr_limit, evmcs.guest_gdtr_limit);
    put(field::guest_idtr_limit, evmcs.guest_idtr_limit);

    put(field::guest_es_access_rights, evmcs.guest_es_ar_bytes);
    put(field::guest_cs_access_rights, evmcs.guest_cs_ar_bytes);
    put(field::guest_ss_access_rights, evmcs.guest_ss_ar_bytes);
    put(field::guest_ds_access_rights, evmcs.guest_ds_ar_bytes);
    put(field::guest_fs_access_rights, evmcs.guest_fs_ar_bytes);
    put(field::guest_gs_access_rights, evmcs.guest_gs_ar_bytes);
    put(field::guest_ldtr_access_rights, evmcs.guest_ldtr_ar_bytes);
    put(field::guest_tr_access_rights, evmcs.guest_tr_ar_bytes);

    put(field::guest_es_base, evmcs.guest_es_base);
    put(field::guest_cs_base, evmcs.guest_cs_base);
    put(field::guest_ss_base, evmcs.guest_ss_base);
    put(field::guest_ds_base, evmcs.guest_ds_base);
    put(field::guest_fs_base, evmcs.guest_fs_base);
    put(field::guest_gs_base, evmcs.guest_gs_base);
    put(field::guest_ldtr_base, evmcs.guest_ldtr_base);
    put(field::guest_tr_base, evmcs.guest_tr_base);
    put(field::guest_gdtr_base, evmcs.guest_gdtr_base);
    put(field::guest_idtr_base, evmcs.guest_idtr_base);

    // Remaining guest state.
    put(field::guest_ia32_debugctl, evmcs.guest_ia32_debugctl);
    put(field::guest_ia32_pat, evmcs.guest_ia32_pat);
    put(field::guest_ia32_efer, evmcs.guest_ia32_efer);
    put(field::guest_pdpte_0, evmcs.guest_pdptr0);
    put(field::guest_pdpte_1, evmcs.guest_pdptr1);
    put(field::guest_pdpte_2, evmcs.guest_pdptr2);
    put(field::guest_pdpte_3, evmcs.guest_pdptr3);
    put(field::guest_pending_debug_exceptions,
        evmcs.guest_pending_dbg_exceptions);
    put(field::guest_ia32_sysenter_esp, evmcs.guest_sysenter_esp);
    put(field::guest_ia32_sysenter_eip, evmcs.guest_sysenter_eip);
    put(field::guest_ia32_sysenter_cs, evmcs.guest_sysenter_cs);
    put(field::guest_activity_state, evmcs.guest_activity_state);
    put(field::guest_ia32_bndcfgs, evmcs.guest_bndcfgs);

    // Host state, which the reflection path loads back.
    put(field::host_cr0, evmcs.host_cr0);
    put(field::host_cr3, evmcs.host_cr3);
    put(field::host_cr4, evmcs.host_cr4);
    put(field::host_ia32_pat, evmcs.host_ia32_pat);
    put(field::host_ia32_efer, evmcs.host_ia32_efer);
    put(field::host_ia32_sysenter_esp, evmcs.host_ia32_sysenter_esp);
    put(field::host_ia32_sysenter_eip, evmcs.host_ia32_sysenter_eip);
    put(field::host_ia32_sysenter_cs, evmcs.host_ia32_sysenter_cs);
    put(field::host_rip, evmcs.host_rip);
    put(field::host_rsp, evmcs.host_rsp);
    put(field::host_fs_base, evmcs.host_fs_base);
    put(field::host_gs_base, evmcs.host_gs_base);
    put(field::host_tr_base, evmcs.host_tr_base);
    put(field::host_gdtr_base, evmcs.host_gdtr_base);
    put(field::host_idtr_base, evmcs.host_idtr_base);

    put(field::host_es_selector, evmcs.host_es_selector);
    put(field::host_cs_selector, evmcs.host_cs_selector);
    put(field::host_ss_selector, evmcs.host_ss_selector);
    put(field::host_ds_selector, evmcs.host_ds_selector);
    put(field::host_fs_selector, evmcs.host_fs_selector);
    put(field::host_gs_selector, evmcs.host_gs_selector);
    put(field::host_tr_selector, evmcs.host_tr_selector);

    // Message-passing addresses and counts.
    put(field::vm_exit_msr_store_address, evmcs.vm_exit_msr_store_addr);
    put(field::vm_exit_msr_load_address, evmcs.vm_exit_msr_load_addr);
    put(field::vm_entry_msr_load_address, evmcs.vm_entry_msr_load_addr);
    put(field::vm_exit_msr_store_count, evmcs.vm_exit_msr_store_count);
    put(field::vm_exit_msr_load_count, evmcs.vm_exit_msr_load_count);
    put(field::vm_entry_msr_load_count, evmcs.vm_entry_msr_load_count);
    put(field::cr3_target_count, evmcs.cr3_target_count);

    this->evmcs_reads[cpu] = this->evmcs_reads[cpu] + 1;
}

} // namespace zpp::hypervisor
