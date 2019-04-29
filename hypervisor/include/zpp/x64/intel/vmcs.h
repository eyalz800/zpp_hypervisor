#pragma once
#include "zpp/maybe.h"
#include "zpp/x64/intel/asm.h"
#include "zpp/x64/intel/vmcs_fields.h"
#include <cstdint>

namespace zpp::x64::intel
{
/**
 * The VMCS error type.
 */
enum class vmcs_error : int
{
    success,
    fail,
};

/**
 * Returns the VMCS error category.
 */
inline const zpp::error_category & category(vmcs_error)
{
    constexpr static auto error_category = zpp::make_error_category(
        "vmcs", vmcs_error::success, [](auto code) -> std::string_view {
            switch (code) {
            case vmcs_error::success:
                return zpp::error::no_error;
            case vmcs_error::fail:
                return "Fail.";
            default:
                return "Unknown error.";
            }
        });
    return error_category;
}

/**
 * Utility to access currently assigned CPU VMCS.
 */
class vmcs
{
public:
    /**
     * The VMCS error type.
     */
    using error = vmcs_error;

    /**
     * The VMCS field type.
     */
    using field = vmcs_fields::vmcs_field;

    /**
     * Construct the VMCS access utility.
     */
    vmcs() = default;

    /**
     * Write a value to a VMCS field.
     */
    zpp::error write(field field, std::uint64_t value) const
    {
        if (0 != vmwrite(field, value)) {
            return error::fail;
        }

        return error::success;
    }

    /**
     * Read a specific VMCS field.
     */
    zpp::maybe<std::uint64_t> read(field field) const
    {
        std::uint64_t value{};
        if (0 != vmread(field, &value)) {
            return error::fail;
        }
        return value;
    }

    /**
     * Functions to read and write specific VMCS fields.
     * @{
     */

    zpp::maybe<std::uint64_t> vpid() const
    {
        return read(field::vpid);
    }

    zpp::error vpid(std::uint64_t value) const
    {
        return write(field::vpid, value);
    }

    zpp::maybe<std::uint64_t> posted_interrupt_notification_vector() const
    {
        return read(field::posted_interrupt_notification_vector);
    }

    zpp::error
    posted_interrupt_notification_vector(std::uint64_t value) const
    {
        return write(field::posted_interrupt_notification_vector, value);
    }

    zpp::maybe<std::uint64_t> eptp_index() const
    {
        return read(field::eptp_index);
    }

    zpp::error eptp_index(std::uint64_t value) const
    {
        return write(field::eptp_index, value);
    }

    zpp::maybe<std::uint64_t> guest_es_selector() const
    {
        return read(field::guest_es_selector);
    }

    zpp::error guest_es_selector(std::uint64_t value) const
    {
        return write(field::guest_es_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_cs_selector() const
    {
        return read(field::guest_cs_selector);
    }

    zpp::error guest_cs_selector(std::uint64_t value) const
    {
        return write(field::guest_cs_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_ss_selector() const
    {
        return read(field::guest_ss_selector);
    }

    zpp::error guest_ss_selector(std::uint64_t value) const
    {
        return write(field::guest_ss_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_ds_selector() const
    {
        return read(field::guest_ds_selector);
    }

    zpp::error guest_ds_selector(std::uint64_t value) const
    {
        return write(field::guest_ds_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_fs_selector() const
    {
        return read(field::guest_fs_selector);
    }

    zpp::error guest_fs_selector(std::uint64_t value) const
    {
        return write(field::guest_fs_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_gs_selector() const
    {
        return read(field::guest_gs_selector);
    }

    zpp::error guest_gs_selector(std::uint64_t value) const
    {
        return write(field::guest_gs_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_ldtr_selector() const
    {
        return read(field::guest_ldtr_selector);
    }

    zpp::error guest_ldtr_selector(std::uint64_t value) const
    {
        return write(field::guest_ldtr_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_tr_selector() const
    {
        return read(field::guest_tr_selector);
    }

    zpp::error guest_tr_selector(std::uint64_t value) const
    {
        return write(field::guest_tr_selector, value);
    }

    zpp::maybe<std::uint64_t> guest_interrupt_status() const
    {
        return read(field::guest_interrupt_status);
    }

    zpp::error guest_interrupt_status(std::uint64_t value) const
    {
        return write(field::guest_interrupt_status, value);
    }

    zpp::maybe<std::uint64_t> pml_index() const
    {
        return read(field::pml_index);
    }

    zpp::error pml_index(std::uint64_t value) const
    {
        return write(field::pml_index, value);
    }

    zpp::maybe<std::uint64_t> host_es_selector() const
    {
        return read(field::host_es_selector);
    }

    zpp::error host_es_selector(std::uint64_t value) const
    {
        return write(field::host_es_selector, value);
    }

    zpp::maybe<std::uint64_t> host_cs_selector() const
    {
        return read(field::host_cs_selector);
    }

    zpp::error host_cs_selector(std::uint64_t value) const
    {
        return write(field::host_cs_selector, value);
    }

    zpp::maybe<std::uint64_t> host_ss_selector() const
    {
        return read(field::host_ss_selector);
    }

    zpp::error host_ss_selector(std::uint64_t value) const
    {
        return write(field::host_ss_selector, value);
    }

    zpp::maybe<std::uint64_t> host_ds_selector() const
    {
        return read(field::host_ds_selector);
    }

    zpp::error host_ds_selector(std::uint64_t value) const
    {
        return write(field::host_ds_selector, value);
    }

    zpp::maybe<std::uint64_t> host_fs_selector() const
    {
        return read(field::host_fs_selector);
    }

    zpp::error host_fs_selector(std::uint64_t value) const
    {
        return write(field::host_fs_selector, value);
    }

    zpp::maybe<std::uint64_t> host_gs_selector() const
    {
        return read(field::host_gs_selector);
    }

    zpp::error host_gs_selector(std::uint64_t value) const
    {
        return write(field::host_gs_selector, value);
    }

    zpp::maybe<std::uint64_t> host_tr_selector() const
    {
        return read(field::host_tr_selector);
    }

    zpp::error host_tr_selector(std::uint64_t value) const
    {
        return write(field::host_tr_selector, value);
    }

    zpp::maybe<std::uint64_t> io_bitmap_a() const
    {
        return read(field::io_bitmap_a);
    }

    zpp::error io_bitmap_a(std::uint64_t value) const
    {
        return write(field::io_bitmap_a, value);
    }

    zpp::maybe<std::uint64_t> io_bitmap_b() const
    {
        return read(field::io_bitmap_b);
    }

    zpp::error io_bitmap_b(std::uint64_t value) const
    {
        return write(field::io_bitmap_b, value);
    }

    zpp::maybe<std::uint64_t> msr_bitmap() const
    {
        return read(field::msr_bitmap);
    }

    zpp::error msr_bitmap(std::uint64_t value) const
    {
        return write(field::msr_bitmap, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_msr_store_address() const
    {
        return read(field::vm_exit_msr_store_address);
    }

    zpp::error vm_exit_msr_store_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_address, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_msr_load_address() const
    {
        return read(field::vm_exit_msr_load_address);
    }

    zpp::error vm_exit_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_address, value);
    }

    zpp::maybe<std::uint64_t> vm_entry_msr_load_address() const
    {
        return read(field::vm_entry_msr_load_address);
    }

    zpp::error vm_entry_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_address, value);
    }

    zpp::maybe<std::uint64_t> excecutive_vmcs_pointer() const
    {
        return read(field::excecutive_vmcs_pointer);
    }

    zpp::error excecutive_vmcs_pointer(std::uint64_t value) const
    {
        return write(field::excecutive_vmcs_pointer, value);
    }

    zpp::maybe<std::uint64_t> pml_address() const
    {
        return read(field::pml_address);
    }

    zpp::error pml_address(std::uint64_t value) const
    {
        return write(field::pml_address, value);
    }

    zpp::maybe<std::uint64_t> tsc_offset() const
    {
        return read(field::tsc_offset);
    }

    zpp::error tsc_offset(std::uint64_t value) const
    {
        return write(field::tsc_offset, value);
    }

    zpp::maybe<std::uint64_t> virtual_apic_address() const
    {
        return read(field::virtual_apic_address);
    }

    zpp::error virtual_apic_address(std::uint64_t value) const
    {
        return write(field::virtual_apic_address, value);
    }

    zpp::maybe<std::uint64_t> apic_access_address() const
    {
        return read(field::apic_access_address);
    }

    zpp::error apic_access_address(std::uint64_t value) const
    {
        return write(field::apic_access_address, value);
    }

    zpp::maybe<std::uint64_t> posted_interrupt_descriptor_address() const
    {
        return read(field::posted_interrupt_descriptor_address);
    }

    zpp::error
    posted_interrupt_descriptor_address(std::uint64_t value) const
    {
        return write(field::posted_interrupt_descriptor_address, value);
    }

    zpp::maybe<std::uint64_t> vm_function_controls() const
    {
        return read(field::vm_function_controls);
    }

    zpp::error vm_function_controls(std::uint64_t value) const
    {
        return write(field::vm_function_controls, value);
    }

    zpp::maybe<std::uint64_t> ept_pointer() const
    {
        return read(field::ept_pointer);
    }

    zpp::error ept_pointer(std::uint64_t value) const
    {
        return write(field::ept_pointer, value);
    }

    zpp::maybe<std::uint64_t> eio_exit_bitmap_0() const
    {
        return read(field::eio_exit_bitmap_0);
    }

    zpp::error eio_exit_bitmap_0(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_0, value);
    }

    zpp::maybe<std::uint64_t> eio_exit_bitmap_1() const
    {
        return read(field::eio_exit_bitmap_1);
    }

    zpp::error eio_exit_bitmap_1(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_1, value);
    }

    zpp::maybe<std::uint64_t> eio_exit_bitmap_2() const
    {
        return read(field::eio_exit_bitmap_2);
    }

    zpp::error eio_exit_bitmap_2(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_2, value);
    }

    zpp::maybe<std::uint64_t> eio_exit_bitmap_3() const
    {
        return read(field::eio_exit_bitmap_3);
    }

    zpp::error eio_exit_bitmap_3(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_3, value);
    }

    zpp::maybe<std::uint64_t> eptp_list_address() const
    {
        return read(field::eptp_list_address);
    }

    zpp::error eptp_list_address(std::uint64_t value) const
    {
        return write(field::eptp_list_address, value);
    }

    zpp::maybe<std::uint64_t> vmread_bitmap_address() const
    {
        return read(field::vmread_bitmap_address);
    }

    zpp::error vmread_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmread_bitmap_address, value);
    }

    zpp::maybe<std::uint64_t> vmwrite_bitmap_address() const
    {
        return read(field::vmwrite_bitmap_address);
    }

    zpp::error vmwrite_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmwrite_bitmap_address, value);
    }

    zpp::maybe<std::uint64_t>
    virtualization_exception_information_address() const
    {
        return read(field::virtualization_exception_information_address);
    }

    zpp::error
    virtualization_exception_information_address(std::uint64_t value) const
    {
        return write(field::virtualization_exception_information_address,
                     value);
    }

    zpp::maybe<std::uint64_t> xss_exiting_bitmap() const
    {
        return read(field::xss_exiting_bitmap);
    }

    zpp::error xss_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::xss_exiting_bitmap, value);
    }

    zpp::maybe<std::uint64_t> encls_exiting_bitmap() const
    {
        return read(field::encls_exiting_bitmap);
    }

    zpp::error encls_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::encls_exiting_bitmap, value);
    }

    zpp::maybe<std::uint64_t> sub_page_permission_table_pointer() const
    {
        return read(field::sub_page_permission_table_pointer);
    }

    zpp::error sub_page_permission_table_pointer(std::uint64_t value) const
    {
        return write(field::sub_page_permission_table_pointer, value);
    }

    zpp::maybe<std::uint64_t> tsc_multiplier() const
    {
        return read(field::tsc_multiplier);
    }

    zpp::error tsc_multiplier(std::uint64_t value) const
    {
        return write(field::tsc_multiplier, value);
    }

    zpp::maybe<std::uint64_t> guest_physical_address() const
    {
        return read(field::guest_physical_address);
    }

    zpp::error guest_physical_address(std::uint64_t value) const
    {
        return write(field::guest_physical_address, value);
    }

    zpp::maybe<std::uint64_t> vmcs_link_pointer() const
    {
        return read(field::vmcs_link_pointer);
    }

    zpp::error vmcs_link_pointer(std::uint64_t value) const
    {
        return write(field::vmcs_link_pointer, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_debugctl() const
    {
        return read(field::guest_ia32_debugctl);
    }

    zpp::error guest_ia32_debugctl(std::uint64_t value) const
    {
        return write(field::guest_ia32_debugctl, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_pat() const
    {
        return read(field::guest_ia32_pat);
    }

    zpp::error guest_ia32_pat(std::uint64_t value) const
    {
        return write(field::guest_ia32_pat, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_efer() const
    {
        return read(field::guest_ia32_efer);
    }

    zpp::error guest_ia32_efer(std::uint64_t value) const
    {
        return write(field::guest_ia32_efer, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_perf_global_ctrl() const
    {
        return read(field::guest_ia32_perf_global_ctrl);
    }

    zpp::error guest_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::guest_ia32_perf_global_ctrl, value);
    }

    zpp::maybe<std::uint64_t> guest_pdpte_0() const
    {
        return read(field::guest_pdpte_0);
    }

    zpp::error guest_pdpte_0(std::uint64_t value) const
    {
        return write(field::guest_pdpte_0, value);
    }

    zpp::maybe<std::uint64_t> guest_pdpte_1() const
    {
        return read(field::guest_pdpte_1);
    }

    zpp::error guest_pdpte_1(std::uint64_t value) const
    {
        return write(field::guest_pdpte_1, value);
    }

    zpp::maybe<std::uint64_t> guest_pdpte_2() const
    {
        return read(field::guest_pdpte_2);
    }

    zpp::error guest_pdpte_2(std::uint64_t value) const
    {
        return write(field::guest_pdpte_2, value);
    }

    zpp::maybe<std::uint64_t> guest_pdpte_3() const
    {
        return read(field::guest_pdpte_3);
    }

    zpp::error guest_pdpte_3(std::uint64_t value) const
    {
        return write(field::guest_pdpte_3, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_bndcfgs() const
    {
        return read(field::guest_ia32_bndcfgs);
    }

    zpp::error guest_ia32_bndcfgs(std::uint64_t value) const
    {
        return write(field::guest_ia32_bndcfgs, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_pat() const
    {
        return read(field::host_ia32_pat);
    }

    zpp::error host_ia32_pat(std::uint64_t value) const
    {
        return write(field::host_ia32_pat, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_efer() const
    {
        return read(field::host_ia32_efer);
    }

    zpp::error host_ia32_efer(std::uint64_t value) const
    {
        return write(field::host_ia32_efer, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_perf_global_ctrl() const
    {
        return read(field::host_ia32_perf_global_ctrl);
    }

    zpp::error host_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::host_ia32_perf_global_ctrl, value);
    }

    zpp::maybe<std::uint64_t> pin_based_vm_execution_controls() const
    {
        return read(field::pin_based_vm_execution_controls);
    }

    zpp::error pin_based_vm_execution_controls(std::uint64_t value) const
    {
        return write(field::pin_based_vm_execution_controls, value);
    }

    zpp::maybe<std::uint64_t>
    primary_processor_based_vm_execution_controls() const
    {
        return read(field::primary_processor_based_vm_execution_controls);
    }

    zpp::error primary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(field::primary_processor_based_vm_execution_controls,
                     value);
    }

    zpp::maybe<std::uint64_t> exception_bitmap() const
    {
        return read(field::exception_bitmap);
    }

    zpp::error exception_bitmap(std::uint64_t value) const
    {
        return write(field::exception_bitmap, value);
    }

    zpp::maybe<std::uint64_t> page_fault_error_code_mask() const
    {
        return read(field::page_fault_error_code_mask);
    }

    zpp::error page_fault_error_code_mask(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_mask, value);
    }

    zpp::maybe<std::uint64_t> page_fault_error_code_match() const
    {
        return read(field::page_fault_error_code_match);
    }

    zpp::error page_fault_error_code_match(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_match, value);
    }

    zpp::maybe<std::uint64_t> cr3_target_count() const
    {
        return read(field::cr3_target_count);
    }

    zpp::error cr3_target_count(std::uint64_t value) const
    {
        return write(field::cr3_target_count, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_controls() const
    {
        return read(field::vm_exit_controls);
    }

    zpp::error vm_exit_controls(std::uint64_t value) const
    {
        return write(field::vm_exit_controls, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_msr_store_count() const
    {
        return read(field::vm_exit_msr_store_count);
    }

    zpp::error vm_exit_msr_store_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_count, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_msr_load_count() const
    {
        return read(field::vm_exit_msr_load_count);
    }

    zpp::error vm_exit_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_count, value);
    }

    zpp::maybe<std::uint64_t> vm_entry_controls() const
    {
        return read(field::vm_entry_controls);
    }

    zpp::error vm_entry_controls(std::uint64_t value) const
    {
        return write(field::vm_entry_controls, value);
    }

    zpp::maybe<std::uint64_t> vm_entry_msr_load_count() const
    {
        return read(field::vm_entry_msr_load_count);
    }

    zpp::error vm_entry_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_count, value);
    }

    zpp::maybe<std::uint64_t>
    vm_entry_interruption_information_field() const
    {
        return read(field::vm_entry_interruption_information_field);
    }

    zpp::error
    vm_entry_interruption_information_field(std::uint64_t value) const
    {
        return write(field::vm_entry_interruption_information_field,
                     value);
    }

    zpp::maybe<std::uint64_t> vm_entry_exception_error_code() const
    {
        return read(field::vm_entry_exception_error_code);
    }

    zpp::error vm_entry_exception_error_code(std::uint64_t value) const
    {
        return write(field::vm_entry_exception_error_code, value);
    }

    zpp::maybe<std::uint64_t> vm_entry_instruction_length() const
    {
        return read(field::vm_entry_instruction_length);
    }

    zpp::error vm_entry_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_entry_instruction_length, value);
    }

    zpp::maybe<std::uint64_t> trr_threshold() const
    {
        return read(field::trr_threshold);
    }

    zpp::error trr_threshold(std::uint64_t value) const
    {
        return write(field::trr_threshold, value);
    }

    zpp::maybe<std::uint64_t>
    secondary_processor_based_vm_execution_controls() const
    {
        return read(
            field::secondary_processor_based_vm_execution_controls);
    }

    zpp::error secondary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(
            field::secondary_processor_based_vm_execution_controls, value);
    }

    zpp::maybe<std::uint64_t> ple_gap() const
    {
        return read(field::ple_gap);
    }

    zpp::error ple_gap(std::uint64_t value) const
    {
        return write(field::ple_gap, value);
    }

    zpp::maybe<std::uint64_t> ple_window() const
    {
        return read(field::ple_window);
    }

    zpp::error ple_window(std::uint64_t value) const
    {
        return write(field::ple_window, value);
    }

    zpp::maybe<std::uint64_t> vm_instruction_error() const
    {
        return read(field::vm_instruction_error);
    }

    zpp::error vm_instruction_error(std::uint64_t value) const
    {
        return write(field::vm_instruction_error, value);
    }

    zpp::maybe<std::uint64_t> exit_reason() const
    {
        return read(field::exit_reason);
    }

    zpp::error exit_reason(std::uint64_t value) const
    {
        return write(field::exit_reason, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_interruption_information() const
    {
        return read(field::vm_exit_interruption_information);
    }

    zpp::error vm_exit_interruption_information(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_information, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_interruption_error_code() const
    {
        return read(field::vm_exit_interruption_error_code);
    }

    zpp::error vm_exit_interruption_error_code(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_error_code, value);
    }

    zpp::maybe<std::uint64_t> idt_vectoring_information_field() const
    {
        return read(field::idt_vectoring_information_field);
    }

    zpp::error idt_vectoring_information_field(std::uint64_t value) const
    {
        return write(field::idt_vectoring_information_field, value);
    }

    zpp::maybe<std::uint64_t> idt_vectoring_error_code() const
    {
        return read(field::idt_vectoring_error_code);
    }

    zpp::error idt_vectoring_error_code(std::uint64_t value) const
    {
        return write(field::idt_vectoring_error_code, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_instruction_length() const
    {
        return read(field::vm_exit_instruction_length);
    }

    zpp::error vm_exit_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_length, value);
    }

    zpp::maybe<std::uint64_t> vm_exit_instruction_information() const
    {
        return read(field::vm_exit_instruction_information);
    }

    zpp::error vm_exit_instruction_information(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_information, value);
    }

    zpp::maybe<std::uint64_t> guest_es_limit() const
    {
        return read(field::guest_es_limit);
    }

    zpp::error guest_es_limit(std::uint64_t value) const
    {
        return write(field::guest_es_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_cs_limit() const
    {
        return read(field::guest_cs_limit);
    }

    zpp::error guest_cs_limit(std::uint64_t value) const
    {
        return write(field::guest_cs_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_ss_limit() const
    {
        return read(field::guest_ss_limit);
    }

    zpp::error guest_ss_limit(std::uint64_t value) const
    {
        return write(field::guest_ss_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_ds_limit() const
    {
        return read(field::guest_ds_limit);
    }

    zpp::error guest_ds_limit(std::uint64_t value) const
    {
        return write(field::guest_ds_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_fs_limit() const
    {
        return read(field::guest_fs_limit);
    }

    zpp::error guest_fs_limit(std::uint64_t value) const
    {
        return write(field::guest_fs_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_gs_limit() const
    {
        return read(field::guest_gs_limit);
    }

    zpp::error guest_gs_limit(std::uint64_t value) const
    {
        return write(field::guest_gs_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_ldtr_limit() const
    {
        return read(field::guest_ldtr_limit);
    }

    zpp::error guest_ldtr_limit(std::uint64_t value) const
    {
        return write(field::guest_ldtr_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_tr_limit() const
    {
        return read(field::guest_tr_limit);
    }

    zpp::error guest_tr_limit(std::uint64_t value) const
    {
        return write(field::guest_tr_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_gdtr_limit() const
    {
        return read(field::guest_gdtr_limit);
    }

    zpp::error guest_gdtr_limit(std::uint64_t value) const
    {
        return write(field::guest_gdtr_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_idtr_limit() const
    {
        return read(field::guest_idtr_limit);
    }

    zpp::error guest_idtr_limit(std::uint64_t value) const
    {
        return write(field::guest_idtr_limit, value);
    }

    zpp::maybe<std::uint64_t> guest_es_access_rights() const
    {
        return read(field::guest_es_access_rights);
    }

    zpp::error guest_es_access_rights(std::uint64_t value) const
    {
        return write(field::guest_es_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_cs_access_rights() const
    {
        return read(field::guest_cs_access_rights);
    }

    zpp::error guest_cs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_cs_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_ss_access_rights() const
    {
        return read(field::guest_ss_access_rights);
    }

    zpp::error guest_ss_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ss_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_ds_access_rights() const
    {
        return read(field::guest_ds_access_rights);
    }

    zpp::error guest_ds_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ds_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_fs_access_rights() const
    {
        return read(field::guest_fs_access_rights);
    }

    zpp::error guest_fs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_fs_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_gs_access_rights() const
    {
        return read(field::guest_gs_access_rights);
    }

    zpp::error guest_gs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_gs_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_ldtr_access_rights() const
    {
        return read(field::guest_ldtr_access_rights);
    }

    zpp::error guest_ldtr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ldtr_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_tr_access_rights() const
    {
        return read(field::guest_tr_access_rights);
    }

    zpp::error guest_tr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_tr_access_rights, value);
    }

    zpp::maybe<std::uint64_t> guest_interruptibility_state() const
    {
        return read(field::guest_interruptibility_state);
    }

    zpp::error guest_interruptibility_state(std::uint64_t value) const
    {
        return write(field::guest_interruptibility_state, value);
    }

    zpp::maybe<std::uint64_t> guest_activity_state() const
    {
        return read(field::guest_activity_state);
    }

    zpp::error guest_activity_state(std::uint64_t value) const
    {
        return write(field::guest_activity_state, value);
    }

    zpp::maybe<std::uint64_t> guest_smbase() const
    {
        return read(field::guest_smbase);
    }

    zpp::error guest_smbase(std::uint64_t value) const
    {
        return write(field::guest_smbase, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_sysenter_cs() const
    {
        return read(field::guest_ia32_sysenter_cs);
    }

    zpp::error guest_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_cs, value);
    }

    zpp::maybe<std::uint64_t> vmx_preemption_timer_value() const
    {
        return read(field::vmx_preemption_timer_value);
    }

    zpp::error vmx_preemption_timer_value(std::uint64_t value) const
    {
        return write(field::vmx_preemption_timer_value, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_sysenter_cs() const
    {
        return read(field::host_ia32_sysenter_cs);
    }

    zpp::error host_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_cs, value);
    }

    zpp::maybe<std::uint64_t> cr0_guest_host_mask() const
    {
        return read(field::cr0_guest_host_mask);
    }

    zpp::error cr0_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr0_guest_host_mask, value);
    }

    zpp::maybe<std::uint64_t> cr4_guest_host_mask() const
    {
        return read(field::cr4_guest_host_mask);
    }

    zpp::error cr4_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr4_guest_host_mask, value);
    }

    zpp::maybe<std::uint64_t> cr0_read_shadow() const
    {
        return read(field::cr0_read_shadow);
    }

    zpp::error cr0_read_shadow(std::uint64_t value) const
    {
        return write(field::cr0_read_shadow, value);
    }

    zpp::maybe<std::uint64_t> cr4_read_shadow() const
    {
        return read(field::cr4_read_shadow);
    }

    zpp::error cr4_read_shadow(std::uint64_t value) const
    {
        return write(field::cr4_read_shadow, value);
    }

    zpp::maybe<std::uint64_t> cr3_target_value_0() const
    {
        return read(field::cr3_target_value_0);
    }

    zpp::error cr3_target_value_0(std::uint64_t value) const
    {
        return write(field::cr3_target_value_0, value);
    }

    zpp::maybe<std::uint64_t> cr3_target_value_1() const
    {
        return read(field::cr3_target_value_1);
    }

    zpp::error cr3_target_value_1(std::uint64_t value) const
    {
        return write(field::cr3_target_value_1, value);
    }

    zpp::maybe<std::uint64_t> cr3_target_value_2() const
    {
        return read(field::cr3_target_value_2);
    }

    zpp::error cr3_target_value_2(std::uint64_t value) const
    {
        return write(field::cr3_target_value_2, value);
    }

    zpp::maybe<std::uint64_t> cr3_target_value_3() const
    {
        return read(field::cr3_target_value_3);
    }

    zpp::error cr3_target_value_3(std::uint64_t value) const
    {
        return write(field::cr3_target_value_3, value);
    }

    zpp::maybe<std::uint64_t> guest_cr0() const
    {
        return read(field::guest_cr0);
    }

    zpp::error guest_cr0(std::uint64_t value) const
    {
        return write(field::guest_cr0, value);
    }

    zpp::maybe<std::uint64_t> guest_cr3() const
    {
        return read(field::guest_cr3);
    }

    zpp::error guest_cr3(std::uint64_t value) const
    {
        return write(field::guest_cr3, value);
    }

    zpp::maybe<std::uint64_t> guest_cr4() const
    {
        return read(field::guest_cr4);
    }

    zpp::error guest_cr4(std::uint64_t value) const
    {
        return write(field::guest_cr4, value);
    }

    zpp::maybe<std::uint64_t> guest_es_base() const
    {
        return read(field::guest_es_base);
    }

    zpp::error guest_es_base(std::uint64_t value) const
    {
        return write(field::guest_es_base, value);
    }

    zpp::maybe<std::uint64_t> guest_cs_base() const
    {
        return read(field::guest_cs_base);
    }

    zpp::error guest_cs_base(std::uint64_t value) const
    {
        return write(field::guest_cs_base, value);
    }

    zpp::maybe<std::uint64_t> guest_ss_base() const
    {
        return read(field::guest_ss_base);
    }

    zpp::error guest_ss_base(std::uint64_t value) const
    {
        return write(field::guest_ss_base, value);
    }

    zpp::maybe<std::uint64_t> guest_ds_base() const
    {
        return read(field::guest_ds_base);
    }

    zpp::error guest_ds_base(std::uint64_t value) const
    {
        return write(field::guest_ds_base, value);
    }

    zpp::maybe<std::uint64_t> guest_fs_base() const
    {
        return read(field::guest_fs_base);
    }

    zpp::error guest_fs_base(std::uint64_t value) const
    {
        return write(field::guest_fs_base, value);
    }

    zpp::maybe<std::uint64_t> guest_gs_base() const
    {
        return read(field::guest_gs_base);
    }

    zpp::error guest_gs_base(std::uint64_t value) const
    {
        return write(field::guest_gs_base, value);
    }

    zpp::maybe<std::uint64_t> guest_ldtr_base() const
    {
        return read(field::guest_ldtr_base);
    }

    zpp::error guest_ldtr_base(std::uint64_t value) const
    {
        return write(field::guest_ldtr_base, value);
    }

    zpp::maybe<std::uint64_t> guest_tr_base() const
    {
        return read(field::guest_tr_base);
    }

    zpp::error guest_tr_base(std::uint64_t value) const
    {
        return write(field::guest_tr_base, value);
    }

    zpp::maybe<std::uint64_t> guest_gdtr_base() const
    {
        return read(field::guest_gdtr_base);
    }

    zpp::error guest_gdtr_base(std::uint64_t value) const
    {
        return write(field::guest_gdtr_base, value);
    }

    zpp::maybe<std::uint64_t> guest_idtr_base() const
    {
        return read(field::guest_idtr_base);
    }

    zpp::error guest_idtr_base(std::uint64_t value) const
    {
        return write(field::guest_idtr_base, value);
    }

    zpp::maybe<std::uint64_t> guest_dr7() const
    {
        return read(field::guest_dr7);
    }

    zpp::error guest_dr7(std::uint64_t value) const
    {
        return write(field::guest_dr7, value);
    }

    zpp::maybe<std::uint64_t> guest_rsp() const
    {
        return read(field::guest_rsp);
    }

    zpp::error guest_rsp(std::uint64_t value) const
    {
        return write(field::guest_rsp, value);
    }

    zpp::maybe<std::uint64_t> guest_rip() const
    {
        return read(field::guest_rip);
    }

    zpp::error guest_rip(std::uint64_t value) const
    {
        return write(field::guest_rip, value);
    }

    zpp::maybe<std::uint64_t> guest_rflags() const
    {
        return read(field::guest_rflags);
    }

    zpp::error guest_rflags(std::uint64_t value) const
    {
        return write(field::guest_rflags, value);
    }

    zpp::maybe<std::uint64_t> guest_pending_debug_exceptions() const
    {
        return read(field::guest_pending_debug_exceptions);
    }

    zpp::error guest_pending_debug_exceptions(std::uint64_t value) const
    {
        return write(field::guest_pending_debug_exceptions, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_sysenter_esp() const
    {
        return read(field::guest_ia32_sysenter_esp);
    }

    zpp::error guest_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_esp, value);
    }

    zpp::maybe<std::uint64_t> guest_ia32_sysenter_eip() const
    {
        return read(field::guest_ia32_sysenter_eip);
    }

    zpp::error guest_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_eip, value);
    }

    zpp::maybe<std::uint64_t> host_cr0() const
    {
        return read(field::host_cr0);
    }

    zpp::error host_cr0(std::uint64_t value) const
    {
        return write(field::host_cr0, value);
    }

    zpp::maybe<std::uint64_t> host_cr3() const
    {
        return read(field::host_cr3);
    }

    zpp::error host_cr3(std::uint64_t value) const
    {
        return write(field::host_cr3, value);
    }

    zpp::maybe<std::uint64_t> host_cr4() const
    {
        return read(field::host_cr4);
    }

    zpp::error host_cr4(std::uint64_t value) const
    {
        return write(field::host_cr4, value);
    }

    zpp::maybe<std::uint64_t> host_fs_base() const
    {
        return read(field::host_fs_base);
    }

    zpp::error host_fs_base(std::uint64_t value) const
    {
        return write(field::host_fs_base, value);
    }

    zpp::maybe<std::uint64_t> host_gs_base() const
    {
        return read(field::host_gs_base);
    }

    zpp::error host_gs_base(std::uint64_t value) const
    {
        return write(field::host_gs_base, value);
    }

    zpp::maybe<std::uint64_t> host_tr_base() const
    {
        return read(field::host_tr_base);
    }

    zpp::error host_tr_base(std::uint64_t value) const
    {
        return write(field::host_tr_base, value);
    }

    zpp::maybe<std::uint64_t> host_gdtr_base() const
    {
        return read(field::host_gdtr_base);
    }

    zpp::error host_gdtr_base(std::uint64_t value) const
    {
        return write(field::host_gdtr_base, value);
    }

    zpp::maybe<std::uint64_t> host_idtr_base() const
    {
        return read(field::host_idtr_base);
    }

    zpp::error host_idtr_base(std::uint64_t value) const
    {
        return write(field::host_idtr_base, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_sysenter_esp() const
    {
        return read(field::host_ia32_sysenter_esp);
    }

    zpp::error host_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_esp, value);
    }

    zpp::maybe<std::uint64_t> host_ia32_sysenter_eip() const
    {
        return read(field::host_ia32_sysenter_eip);
    }

    zpp::error host_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_eip, value);
    }

    zpp::maybe<std::uint64_t> host_rsp() const
    {
        return read(field::host_rsp);
    }

    zpp::error host_rsp(std::uint64_t value) const
    {
        return write(field::host_rsp, value);
    }

    zpp::maybe<std::uint64_t> host_rip() const
    {
        return read(field::host_rip);
    }

    zpp::error host_rip(std::uint64_t value) const
    {
        return write(field::host_rip, value);
    }

    /**
     * @}
     */
};

} // namespace zpp::x64::intel
