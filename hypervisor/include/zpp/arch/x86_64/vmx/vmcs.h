#pragma once
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
#include "zpp/error.h"
#include <cstdint>
#include <expected>

namespace zpp::arch::x86_64::vmx
{
/**
 * How many VMCS field accesses this processor has executed.
 *
 * A diagnostic, and the only way to stop guessing at the number that
 * decides everything about this VMM's cost. Nested under a hypervisor
 * that does not offer VMCS shadowing, every one of these is an exit to
 * the layer below - measured at about 3,735 cycles for a read and 2,542
 * for a write, which this VMM prints at launch - so the access *count*
 * per exit is the cost, and it had been estimated twice from cycles
 * divided by those prices and both estimates informed a wrong decision.
 *
 * Deliberately not per-processor and not atomic. One counter that is
 * occasionally short by a racing increment answers "about how many per
 * exit" exactly as well as an exact one, and a `lock` prefix here would
 * be a real cost added to the hot path to measure the hot path.
 *
 * `constinit` and never read by anything that decides: it is compiled
 * in unconditionally because a switch would leave the number available
 * only in a build nobody runs, which is how `ZPP_PUBLISH_REFERENCE_TSC`
 * came to be measured against a stale object file.
 */
inline constinit std::uint64_t vmcs_reads_taken{};
inline constinit std::uint64_t vmcs_writes_taken{};

/**
 * Which fields those reads name, as a table rather than a ring.
 *
 * The count above says 54 reads an exit and the guest-state deferral
 * already reports 0.9 of them, so 53 are something else and no
 * instrument here could say what. KVM's hot exit path reads about eight
 * fields - `sync_vmcs02_to_vmcs12` reads RFLAGS, two AR bytes and the
 * interruptibility state, takes RIP and RSP from its own register cache
 * and computes the activity state - and defers the rest behind
 * `need_sync_vmcs02_to_vmcs12_rare` until L1 actually reads one. Which
 * of our fifty-three are that same rare set is the whole question, and
 * it is answerable only by field.
 *
 * Direct-mapped on the low bits of the encoding and never evicted: a
 * collision loses a field rather than corrupting a count, and the top
 * of the table is what matters. `overflow` says how much was lost, so a
 * table that is too small says so rather than quietly under-reporting.
 */
inline constexpr std::size_t vmcs_read_slots = 64;
inline constinit std::uint64_t vmcs_read_field[vmcs_read_slots]{};
inline constinit std::uint64_t vmcs_read_hits[vmcs_read_slots]{};
inline constinit std::uint64_t vmcs_read_overflow{};

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
     * Write a value to a VMCS field, reporting failure.
     *
     * Only worth calling for a field whose existence depends on a CPU
     * capability, which is the one case where failure is information
     * rather than a bug. Everything else should use write.
     */
    std::expected<void, zpp::error> try_write(field field,
                                              std::uint64_t value) const
    {
        if (0 != vmwrite(field, value)) {
            return std::unexpected(zpp::error{error::fail});
        }

        return {};
    }

    /**
     * Read a specific VMCS field, reporting failure. See try_write.
     */
    std::expected<std::uint64_t, zpp::error> try_read(field field) const
    {
        std::uint64_t value{};
        if (0 != vmread(field, &value)) {
            return std::unexpected(zpp::error{error::fail});
        }
        return value;
    }

    /**
     * Write a value to a VMCS field. Traps if the write fails.
     *
     * There is no such thing as a recoverable vmwrite failure. The
     * instruction fails in exactly three ways - there is no current VMCS,
     * the field encoding does not exist on this processor, or the field is
     * read only - and every one of them is a bug in this code rather than
     * a condition a caller could do anything about. A *wrong value* does
     * not fail here at all; it surfaces later as a VM entry failure.
     *
     * So failure traps, in the same spirit as operator new in the CRT:
     * with -fno-exceptions there is nothing to throw, and a returned error
     * that nobody looks at is worse than a stop. That also keeps the
     * hundred-odd accessors below free of an error type that callers would
     * have had to either check or deliberately discard, which is how these
     * failures came to be ignored in the first place.
     */
    void write(field field, std::uint64_t value) const
    {
        vmcs_writes_taken = vmcs_writes_taken + 1;

        if (0 != vmwrite(field, value)) {
            __builtin_trap();
        }
    }

    /**
     * Read a specific VMCS field. Traps if the read fails, for the same
     * reasons as write.
     */
    std::uint64_t read(field field) const
    {
        vmcs_reads_taken = vmcs_reads_taken + 1;

        auto encoding = static_cast<std::uint64_t>(field);
        auto slot = ((encoding >> 1) ^ (encoding >> 9)) &
                    (vmcs_read_slots - 1);

        if (0 == vmcs_read_hits[slot]) {
            vmcs_read_field[slot] = encoding;
        }

        if (vmcs_read_field[slot] == encoding) {
            vmcs_read_hits[slot] = vmcs_read_hits[slot] + 1;
        } else {
            vmcs_read_overflow = vmcs_read_overflow + 1;
        }

        std::uint64_t value{};
        if (0 != vmread(field, &value)) {
            __builtin_trap();
        }
        return value;
    }

    /**
     * Functions to read and write specific VMCS fields.
     * @{
     */

    std::uint64_t vpid() const
    {
        return read(field::vpid);
    }

    void vpid(std::uint64_t value) const
    {
        return write(field::vpid, value);
    }

    std::uint64_t posted_interrupt_notification_vector() const
    {
        return read(field::posted_interrupt_notification_vector);
    }

    void posted_interrupt_notification_vector(std::uint64_t value) const
    {
        return write(field::posted_interrupt_notification_vector, value);
    }

    std::uint64_t eptp_index() const
    {
        return read(field::eptp_index);
    }

    void eptp_index(std::uint64_t value) const
    {
        return write(field::eptp_index, value);
    }

    std::uint64_t guest_es_selector() const
    {
        return read(field::guest_es_selector);
    }

    void guest_es_selector(std::uint64_t value) const
    {
        return write(field::guest_es_selector, value);
    }

    std::uint64_t guest_cs_selector() const
    {
        return read(field::guest_cs_selector);
    }

    void guest_cs_selector(std::uint64_t value) const
    {
        return write(field::guest_cs_selector, value);
    }

    std::uint64_t guest_ss_selector() const
    {
        return read(field::guest_ss_selector);
    }

    void guest_ss_selector(std::uint64_t value) const
    {
        return write(field::guest_ss_selector, value);
    }

    std::uint64_t guest_ds_selector() const
    {
        return read(field::guest_ds_selector);
    }

    void guest_ds_selector(std::uint64_t value) const
    {
        return write(field::guest_ds_selector, value);
    }

    std::uint64_t guest_fs_selector() const
    {
        return read(field::guest_fs_selector);
    }

    void guest_fs_selector(std::uint64_t value) const
    {
        return write(field::guest_fs_selector, value);
    }

    std::uint64_t guest_gs_selector() const
    {
        return read(field::guest_gs_selector);
    }

    void guest_gs_selector(std::uint64_t value) const
    {
        return write(field::guest_gs_selector, value);
    }

    std::uint64_t guest_ldtr_selector() const
    {
        return read(field::guest_ldtr_selector);
    }

    void guest_ldtr_selector(std::uint64_t value) const
    {
        return write(field::guest_ldtr_selector, value);
    }

    std::uint64_t guest_tr_selector() const
    {
        return read(field::guest_tr_selector);
    }

    void guest_tr_selector(std::uint64_t value) const
    {
        return write(field::guest_tr_selector, value);
    }

    std::uint64_t guest_interrupt_status() const
    {
        return read(field::guest_interrupt_status);
    }

    void guest_interrupt_status(std::uint64_t value) const
    {
        return write(field::guest_interrupt_status, value);
    }

    std::uint64_t pml_index() const
    {
        return read(field::pml_index);
    }

    void pml_index(std::uint64_t value) const
    {
        return write(field::pml_index, value);
    }

    std::uint64_t host_es_selector() const
    {
        return read(field::host_es_selector);
    }

    void host_es_selector(std::uint64_t value) const
    {
        return write(field::host_es_selector, value);
    }

    std::uint64_t host_cs_selector() const
    {
        return read(field::host_cs_selector);
    }

    void host_cs_selector(std::uint64_t value) const
    {
        return write(field::host_cs_selector, value);
    }

    std::uint64_t host_ss_selector() const
    {
        return read(field::host_ss_selector);
    }

    void host_ss_selector(std::uint64_t value) const
    {
        return write(field::host_ss_selector, value);
    }

    std::uint64_t host_ds_selector() const
    {
        return read(field::host_ds_selector);
    }

    void host_ds_selector(std::uint64_t value) const
    {
        return write(field::host_ds_selector, value);
    }

    std::uint64_t host_fs_selector() const
    {
        return read(field::host_fs_selector);
    }

    void host_fs_selector(std::uint64_t value) const
    {
        return write(field::host_fs_selector, value);
    }

    std::uint64_t host_gs_selector() const
    {
        return read(field::host_gs_selector);
    }

    void host_gs_selector(std::uint64_t value) const
    {
        return write(field::host_gs_selector, value);
    }

    std::uint64_t host_tr_selector() const
    {
        return read(field::host_tr_selector);
    }

    void host_tr_selector(std::uint64_t value) const
    {
        return write(field::host_tr_selector, value);
    }

    std::uint64_t io_bitmap_a() const
    {
        return read(field::io_bitmap_a);
    }

    void io_bitmap_a(std::uint64_t value) const
    {
        return write(field::io_bitmap_a, value);
    }

    std::uint64_t io_bitmap_b() const
    {
        return read(field::io_bitmap_b);
    }

    void io_bitmap_b(std::uint64_t value) const
    {
        return write(field::io_bitmap_b, value);
    }

    std::uint64_t msr_bitmap() const
    {
        return read(field::msr_bitmap);
    }

    void msr_bitmap(std::uint64_t value) const
    {
        return write(field::msr_bitmap, value);
    }

    std::uint64_t vm_exit_msr_store_address() const
    {
        return read(field::vm_exit_msr_store_address);
    }

    void vm_exit_msr_store_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_address, value);
    }

    std::uint64_t vm_exit_msr_load_address() const
    {
        return read(field::vm_exit_msr_load_address);
    }

    void vm_exit_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_address, value);
    }

    std::uint64_t vm_entry_msr_load_address() const
    {
        return read(field::vm_entry_msr_load_address);
    }

    void vm_entry_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_address, value);
    }

    std::uint64_t excecutive_vmcs_pointer() const
    {
        return read(field::excecutive_vmcs_pointer);
    }

    void excecutive_vmcs_pointer(std::uint64_t value) const
    {
        return write(field::excecutive_vmcs_pointer, value);
    }

    std::uint64_t pml_address() const
    {
        return read(field::pml_address);
    }

    void pml_address(std::uint64_t value) const
    {
        return write(field::pml_address, value);
    }

    std::uint64_t tsc_offset() const
    {
        return read(field::tsc_offset);
    }

    void tsc_offset(std::uint64_t value) const
    {
        return write(field::tsc_offset, value);
    }

    std::uint64_t virtual_apic_address() const
    {
        return read(field::virtual_apic_address);
    }

    void virtual_apic_address(std::uint64_t value) const
    {
        return write(field::virtual_apic_address, value);
    }

    std::uint64_t apic_access_address() const
    {
        return read(field::apic_access_address);
    }

    void apic_access_address(std::uint64_t value) const
    {
        return write(field::apic_access_address, value);
    }

    std::uint64_t posted_interrupt_descriptor_address() const
    {
        return read(field::posted_interrupt_descriptor_address);
    }

    void posted_interrupt_descriptor_address(std::uint64_t value) const
    {
        return write(field::posted_interrupt_descriptor_address, value);
    }

    std::uint64_t vm_function_controls() const
    {
        return read(field::vm_function_controls);
    }

    void vm_function_controls(std::uint64_t value) const
    {
        return write(field::vm_function_controls, value);
    }

    std::uint64_t ept_pointer() const
    {
        return read(field::ept_pointer);
    }

    void ept_pointer(std::uint64_t value) const
    {
        return write(field::ept_pointer, value);
    }

    std::uint64_t eio_exit_bitmap_0() const
    {
        return read(field::eio_exit_bitmap_0);
    }

    void eio_exit_bitmap_0(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_0, value);
    }

    std::uint64_t eio_exit_bitmap_1() const
    {
        return read(field::eio_exit_bitmap_1);
    }

    void eio_exit_bitmap_1(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_1, value);
    }

    std::uint64_t eio_exit_bitmap_2() const
    {
        return read(field::eio_exit_bitmap_2);
    }

    void eio_exit_bitmap_2(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_2, value);
    }

    std::uint64_t eio_exit_bitmap_3() const
    {
        return read(field::eio_exit_bitmap_3);
    }

    void eio_exit_bitmap_3(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_3, value);
    }

    std::uint64_t eptp_list_address() const
    {
        return read(field::eptp_list_address);
    }

    void eptp_list_address(std::uint64_t value) const
    {
        return write(field::eptp_list_address, value);
    }

    std::uint64_t vmread_bitmap_address() const
    {
        return read(field::vmread_bitmap_address);
    }

    void vmread_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmread_bitmap_address, value);
    }

    std::uint64_t vmwrite_bitmap_address() const
    {
        return read(field::vmwrite_bitmap_address);
    }

    void vmwrite_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmwrite_bitmap_address, value);
    }

    std::uint64_t virtualization_exception_information_address() const
    {
        return read(field::virtualization_exception_information_address);
    }

    void
    virtualization_exception_information_address(std::uint64_t value) const
    {
        return write(field::virtualization_exception_information_address,
                     value);
    }

    std::uint64_t xss_exiting_bitmap() const
    {
        return read(field::xss_exiting_bitmap);
    }

    void xss_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::xss_exiting_bitmap, value);
    }

    std::uint64_t encls_exiting_bitmap() const
    {
        return read(field::encls_exiting_bitmap);
    }

    void encls_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::encls_exiting_bitmap, value);
    }

    std::uint64_t sub_page_permission_table_pointer() const
    {
        return read(field::sub_page_permission_table_pointer);
    }

    void sub_page_permission_table_pointer(std::uint64_t value) const
    {
        return write(field::sub_page_permission_table_pointer, value);
    }

    std::uint64_t tsc_multiplier() const
    {
        return read(field::tsc_multiplier);
    }

    void tsc_multiplier(std::uint64_t value) const
    {
        return write(field::tsc_multiplier, value);
    }

    std::uint64_t guest_physical_address() const
    {
        return read(field::guest_physical_address);
    }

    void guest_physical_address(std::uint64_t value) const
    {
        return write(field::guest_physical_address, value);
    }

    std::uint64_t vmcs_link_pointer() const
    {
        return read(field::vmcs_link_pointer);
    }

    void vmcs_link_pointer(std::uint64_t value) const
    {
        return write(field::vmcs_link_pointer, value);
    }

    std::uint64_t guest_ia32_debugctl() const
    {
        return read(field::guest_ia32_debugctl);
    }

    void guest_ia32_debugctl(std::uint64_t value) const
    {
        return write(field::guest_ia32_debugctl, value);
    }

    std::uint64_t guest_ia32_pat() const
    {
        return read(field::guest_ia32_pat);
    }

    void guest_ia32_pat(std::uint64_t value) const
    {
        return write(field::guest_ia32_pat, value);
    }

    std::uint64_t guest_ia32_efer() const
    {
        return read(field::guest_ia32_efer);
    }

    void guest_ia32_efer(std::uint64_t value) const
    {
        return write(field::guest_ia32_efer, value);
    }

    std::uint64_t guest_ia32_perf_global_ctrl() const
    {
        return read(field::guest_ia32_perf_global_ctrl);
    }

    void guest_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::guest_ia32_perf_global_ctrl, value);
    }

    std::uint64_t guest_pdpte_0() const
    {
        return read(field::guest_pdpte_0);
    }

    void guest_pdpte_0(std::uint64_t value) const
    {
        return write(field::guest_pdpte_0, value);
    }

    std::uint64_t guest_pdpte_1() const
    {
        return read(field::guest_pdpte_1);
    }

    void guest_pdpte_1(std::uint64_t value) const
    {
        return write(field::guest_pdpte_1, value);
    }

    std::uint64_t guest_pdpte_2() const
    {
        return read(field::guest_pdpte_2);
    }

    void guest_pdpte_2(std::uint64_t value) const
    {
        return write(field::guest_pdpte_2, value);
    }

    std::uint64_t guest_pdpte_3() const
    {
        return read(field::guest_pdpte_3);
    }

    void guest_pdpte_3(std::uint64_t value) const
    {
        return write(field::guest_pdpte_3, value);
    }

    std::uint64_t guest_ia32_bndcfgs() const
    {
        return read(field::guest_ia32_bndcfgs);
    }

    void guest_ia32_bndcfgs(std::uint64_t value) const
    {
        return write(field::guest_ia32_bndcfgs, value);
    }

    std::uint64_t host_ia32_pat() const
    {
        return read(field::host_ia32_pat);
    }

    void host_ia32_pat(std::uint64_t value) const
    {
        return write(field::host_ia32_pat, value);
    }

    std::uint64_t host_ia32_efer() const
    {
        return read(field::host_ia32_efer);
    }

    void host_ia32_efer(std::uint64_t value) const
    {
        return write(field::host_ia32_efer, value);
    }

    std::uint64_t host_ia32_perf_global_ctrl() const
    {
        return read(field::host_ia32_perf_global_ctrl);
    }

    void host_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::host_ia32_perf_global_ctrl, value);
    }

    std::uint64_t pin_based_vm_execution_controls() const
    {
        return read(field::pin_based_vm_execution_controls);
    }

    void pin_based_vm_execution_controls(std::uint64_t value) const
    {
        return write(field::pin_based_vm_execution_controls, value);
    }

    std::uint64_t primary_processor_based_vm_execution_controls() const
    {
        return read(field::primary_processor_based_vm_execution_controls);
    }

    void primary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(field::primary_processor_based_vm_execution_controls,
                     value);
    }

    std::uint64_t exception_bitmap() const
    {
        return read(field::exception_bitmap);
    }

    void exception_bitmap(std::uint64_t value) const
    {
        return write(field::exception_bitmap, value);
    }

    std::uint64_t page_fault_error_code_mask() const
    {
        return read(field::page_fault_error_code_mask);
    }

    void page_fault_error_code_mask(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_mask, value);
    }

    std::uint64_t page_fault_error_code_match() const
    {
        return read(field::page_fault_error_code_match);
    }

    void page_fault_error_code_match(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_match, value);
    }

    std::uint64_t cr3_target_count() const
    {
        return read(field::cr3_target_count);
    }

    void cr3_target_count(std::uint64_t value) const
    {
        return write(field::cr3_target_count, value);
    }

    std::uint64_t vm_exit_controls() const
    {
        return read(field::vm_exit_controls);
    }

    void vm_exit_controls(std::uint64_t value) const
    {
        return write(field::vm_exit_controls, value);
    }

    std::uint64_t vm_exit_msr_store_count() const
    {
        return read(field::vm_exit_msr_store_count);
    }

    void vm_exit_msr_store_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_count, value);
    }

    std::uint64_t vm_exit_msr_load_count() const
    {
        return read(field::vm_exit_msr_load_count);
    }

    void vm_exit_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_count, value);
    }

    std::uint64_t vm_entry_controls() const
    {
        return read(field::vm_entry_controls);
    }

    void vm_entry_controls(std::uint64_t value) const
    {
        return write(field::vm_entry_controls, value);
    }

    std::uint64_t vm_entry_msr_load_count() const
    {
        return read(field::vm_entry_msr_load_count);
    }

    void vm_entry_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_count, value);
    }

    std::uint64_t vm_entry_interruption_information_field() const
    {
        return read(field::vm_entry_interruption_information_field);
    }

    void vm_entry_interruption_information_field(std::uint64_t value) const
    {
        return write(field::vm_entry_interruption_information_field,
                     value);
    }

    std::uint64_t vm_entry_exception_error_code() const
    {
        return read(field::vm_entry_exception_error_code);
    }

    void vm_entry_exception_error_code(std::uint64_t value) const
    {
        return write(field::vm_entry_exception_error_code, value);
    }

    std::uint64_t vm_entry_instruction_length() const
    {
        return read(field::vm_entry_instruction_length);
    }

    void vm_entry_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_entry_instruction_length, value);
    }

    std::uint64_t tpr_threshold() const
    {
        return read(field::tpr_threshold);
    }

    void tpr_threshold(std::uint64_t value) const
    {
        return write(field::tpr_threshold, value);
    }

    std::uint64_t secondary_processor_based_vm_execution_controls() const
    {
        return read(
            field::secondary_processor_based_vm_execution_controls);
    }

    void secondary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(
            field::secondary_processor_based_vm_execution_controls, value);
    }

    std::uint64_t ple_gap() const
    {
        return read(field::ple_gap);
    }

    void ple_gap(std::uint64_t value) const
    {
        return write(field::ple_gap, value);
    }

    std::uint64_t ple_window() const
    {
        return read(field::ple_window);
    }

    void ple_window(std::uint64_t value) const
    {
        return write(field::ple_window, value);
    }

    std::uint64_t vm_instruction_error() const
    {
        return read(field::vm_instruction_error);
    }

    void vm_instruction_error(std::uint64_t value) const
    {
        return write(field::vm_instruction_error, value);
    }

    std::uint64_t exit_reason() const
    {
        return read(field::exit_reason);
    }

    void exit_reason(std::uint64_t value) const
    {
        return write(field::exit_reason, value);
    }

    std::uint64_t vm_exit_interruption_information() const
    {
        return read(field::vm_exit_interruption_information);
    }

    void vm_exit_interruption_information(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_information, value);
    }

    std::uint64_t vm_exit_interruption_error_code() const
    {
        return read(field::vm_exit_interruption_error_code);
    }

    void vm_exit_interruption_error_code(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_error_code, value);
    }

    std::uint64_t idt_vectoring_information_field() const
    {
        return read(field::idt_vectoring_information_field);
    }

    void idt_vectoring_information_field(std::uint64_t value) const
    {
        return write(field::idt_vectoring_information_field, value);
    }

    std::uint64_t idt_vectoring_error_code() const
    {
        return read(field::idt_vectoring_error_code);
    }

    void idt_vectoring_error_code(std::uint64_t value) const
    {
        return write(field::idt_vectoring_error_code, value);
    }

    std::uint64_t vm_exit_instruction_length() const
    {
        return read(field::vm_exit_instruction_length);
    }

    void vm_exit_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_length, value);
    }

    std::uint64_t vm_exit_instruction_information() const
    {
        return read(field::vm_exit_instruction_information);
    }

    void vm_exit_instruction_information(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_information, value);
    }

    std::uint64_t guest_es_limit() const
    {
        return read(field::guest_es_limit);
    }

    void guest_es_limit(std::uint64_t value) const
    {
        return write(field::guest_es_limit, value);
    }

    std::uint64_t guest_cs_limit() const
    {
        return read(field::guest_cs_limit);
    }

    void guest_cs_limit(std::uint64_t value) const
    {
        return write(field::guest_cs_limit, value);
    }

    std::uint64_t guest_ss_limit() const
    {
        return read(field::guest_ss_limit);
    }

    void guest_ss_limit(std::uint64_t value) const
    {
        return write(field::guest_ss_limit, value);
    }

    std::uint64_t guest_ds_limit() const
    {
        return read(field::guest_ds_limit);
    }

    void guest_ds_limit(std::uint64_t value) const
    {
        return write(field::guest_ds_limit, value);
    }

    std::uint64_t guest_fs_limit() const
    {
        return read(field::guest_fs_limit);
    }

    void guest_fs_limit(std::uint64_t value) const
    {
        return write(field::guest_fs_limit, value);
    }

    std::uint64_t guest_gs_limit() const
    {
        return read(field::guest_gs_limit);
    }

    void guest_gs_limit(std::uint64_t value) const
    {
        return write(field::guest_gs_limit, value);
    }

    std::uint64_t guest_ldtr_limit() const
    {
        return read(field::guest_ldtr_limit);
    }

    void guest_ldtr_limit(std::uint64_t value) const
    {
        return write(field::guest_ldtr_limit, value);
    }

    std::uint64_t guest_tr_limit() const
    {
        return read(field::guest_tr_limit);
    }

    void guest_tr_limit(std::uint64_t value) const
    {
        return write(field::guest_tr_limit, value);
    }

    std::uint64_t guest_gdtr_limit() const
    {
        return read(field::guest_gdtr_limit);
    }

    void guest_gdtr_limit(std::uint64_t value) const
    {
        return write(field::guest_gdtr_limit, value);
    }

    std::uint64_t guest_idtr_limit() const
    {
        return read(field::guest_idtr_limit);
    }

    void guest_idtr_limit(std::uint64_t value) const
    {
        return write(field::guest_idtr_limit, value);
    }

    std::uint64_t guest_es_access_rights() const
    {
        return read(field::guest_es_access_rights);
    }

    void guest_es_access_rights(std::uint64_t value) const
    {
        return write(field::guest_es_access_rights, value);
    }

    std::uint64_t guest_cs_access_rights() const
    {
        return read(field::guest_cs_access_rights);
    }

    void guest_cs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_cs_access_rights, value);
    }

    std::uint64_t guest_ss_access_rights() const
    {
        return read(field::guest_ss_access_rights);
    }

    void guest_ss_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ss_access_rights, value);
    }

    std::uint64_t guest_ds_access_rights() const
    {
        return read(field::guest_ds_access_rights);
    }

    void guest_ds_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ds_access_rights, value);
    }

    std::uint64_t guest_fs_access_rights() const
    {
        return read(field::guest_fs_access_rights);
    }

    void guest_fs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_fs_access_rights, value);
    }

    std::uint64_t guest_gs_access_rights() const
    {
        return read(field::guest_gs_access_rights);
    }

    void guest_gs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_gs_access_rights, value);
    }

    std::uint64_t guest_ldtr_access_rights() const
    {
        return read(field::guest_ldtr_access_rights);
    }

    void guest_ldtr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ldtr_access_rights, value);
    }

    std::uint64_t guest_tr_access_rights() const
    {
        return read(field::guest_tr_access_rights);
    }

    void guest_tr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_tr_access_rights, value);
    }

    std::uint64_t guest_interruptibility_state() const
    {
        return read(field::guest_interruptibility_state);
    }

    void guest_interruptibility_state(std::uint64_t value) const
    {
        return write(field::guest_interruptibility_state, value);
    }

    std::uint64_t guest_activity_state() const
    {
        return read(field::guest_activity_state);
    }

    void guest_activity_state(std::uint64_t value) const
    {
        return write(field::guest_activity_state, value);
    }

    std::uint64_t guest_smbase() const
    {
        return read(field::guest_smbase);
    }

    void guest_smbase(std::uint64_t value) const
    {
        return write(field::guest_smbase, value);
    }

    std::uint64_t guest_ia32_sysenter_cs() const
    {
        return read(field::guest_ia32_sysenter_cs);
    }

    void guest_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_cs, value);
    }

    std::uint64_t vmx_preemption_timer_value() const
    {
        return read(field::vmx_preemption_timer_value);
    }

    void vmx_preemption_timer_value(std::uint64_t value) const
    {
        return write(field::vmx_preemption_timer_value, value);
    }

    std::uint64_t host_ia32_sysenter_cs() const
    {
        return read(field::host_ia32_sysenter_cs);
    }

    void host_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_cs, value);
    }

    std::uint64_t cr0_guest_host_mask() const
    {
        return read(field::cr0_guest_host_mask);
    }

    void cr0_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr0_guest_host_mask, value);
    }

    std::uint64_t cr4_guest_host_mask() const
    {
        return read(field::cr4_guest_host_mask);
    }

    void cr4_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr4_guest_host_mask, value);
    }

    std::uint64_t cr0_read_shadow() const
    {
        return read(field::cr0_read_shadow);
    }

    void cr0_read_shadow(std::uint64_t value) const
    {
        return write(field::cr0_read_shadow, value);
    }

    std::uint64_t cr4_read_shadow() const
    {
        return read(field::cr4_read_shadow);
    }

    void cr4_read_shadow(std::uint64_t value) const
    {
        return write(field::cr4_read_shadow, value);
    }

    std::uint64_t cr3_target_value_0() const
    {
        return read(field::cr3_target_value_0);
    }

    void cr3_target_value_0(std::uint64_t value) const
    {
        return write(field::cr3_target_value_0, value);
    }

    std::uint64_t cr3_target_value_1() const
    {
        return read(field::cr3_target_value_1);
    }

    void cr3_target_value_1(std::uint64_t value) const
    {
        return write(field::cr3_target_value_1, value);
    }

    std::uint64_t cr3_target_value_2() const
    {
        return read(field::cr3_target_value_2);
    }

    void cr3_target_value_2(std::uint64_t value) const
    {
        return write(field::cr3_target_value_2, value);
    }

    std::uint64_t cr3_target_value_3() const
    {
        return read(field::cr3_target_value_3);
    }

    void cr3_target_value_3(std::uint64_t value) const
    {
        return write(field::cr3_target_value_3, value);
    }

    /**
     * The exit qualification. Read only, and its meaning depends
     * entirely on the exit reason - for a start-up IPI it carries the
     * vector in its low eight bits.
     */
    std::uint64_t exit_qualification() const
    {
        return read(field::exit_qualification);
    }

    std::uint64_t guest_linear_address() const
    {
        return read(field::guest_linear_address);
    }

    std::uint64_t guest_cr0() const
    {
        return read(field::guest_cr0);
    }

    void guest_cr0(std::uint64_t value) const
    {
        return write(field::guest_cr0, value);
    }

    std::uint64_t guest_cr3() const
    {
        return read(field::guest_cr3);
    }

    void guest_cr3(std::uint64_t value) const
    {
        return write(field::guest_cr3, value);
    }

    std::uint64_t guest_cr4() const
    {
        return read(field::guest_cr4);
    }

    void guest_cr4(std::uint64_t value) const
    {
        return write(field::guest_cr4, value);
    }

    std::uint64_t guest_es_base() const
    {
        return read(field::guest_es_base);
    }

    void guest_es_base(std::uint64_t value) const
    {
        return write(field::guest_es_base, value);
    }

    std::uint64_t guest_cs_base() const
    {
        return read(field::guest_cs_base);
    }

    void guest_cs_base(std::uint64_t value) const
    {
        return write(field::guest_cs_base, value);
    }

    std::uint64_t guest_ss_base() const
    {
        return read(field::guest_ss_base);
    }

    void guest_ss_base(std::uint64_t value) const
    {
        return write(field::guest_ss_base, value);
    }

    std::uint64_t guest_ds_base() const
    {
        return read(field::guest_ds_base);
    }

    void guest_ds_base(std::uint64_t value) const
    {
        return write(field::guest_ds_base, value);
    }

    std::uint64_t guest_fs_base() const
    {
        return read(field::guest_fs_base);
    }

    void guest_fs_base(std::uint64_t value) const
    {
        return write(field::guest_fs_base, value);
    }

    std::uint64_t guest_gs_base() const
    {
        return read(field::guest_gs_base);
    }

    void guest_gs_base(std::uint64_t value) const
    {
        return write(field::guest_gs_base, value);
    }

    std::uint64_t guest_ldtr_base() const
    {
        return read(field::guest_ldtr_base);
    }

    void guest_ldtr_base(std::uint64_t value) const
    {
        return write(field::guest_ldtr_base, value);
    }

    std::uint64_t guest_tr_base() const
    {
        return read(field::guest_tr_base);
    }

    void guest_tr_base(std::uint64_t value) const
    {
        return write(field::guest_tr_base, value);
    }

    std::uint64_t guest_gdtr_base() const
    {
        return read(field::guest_gdtr_base);
    }

    void guest_gdtr_base(std::uint64_t value) const
    {
        return write(field::guest_gdtr_base, value);
    }

    std::uint64_t guest_idtr_base() const
    {
        return read(field::guest_idtr_base);
    }

    void guest_idtr_base(std::uint64_t value) const
    {
        return write(field::guest_idtr_base, value);
    }

    std::uint64_t guest_dr7() const
    {
        return read(field::guest_dr7);
    }

    void guest_dr7(std::uint64_t value) const
    {
        return write(field::guest_dr7, value);
    }

    std::uint64_t guest_rsp() const
    {
        return read(field::guest_rsp);
    }

    void guest_rsp(std::uint64_t value) const
    {
        return write(field::guest_rsp, value);
    }

    std::uint64_t guest_rip() const
    {
        return read(field::guest_rip);
    }

    void guest_rip(std::uint64_t value) const
    {
        return write(field::guest_rip, value);
    }

    std::uint64_t guest_rflags() const
    {
        return read(field::guest_rflags);
    }

    void guest_rflags(std::uint64_t value) const
    {
        return write(field::guest_rflags, value);
    }

    std::uint64_t guest_pending_debug_exceptions() const
    {
        return read(field::guest_pending_debug_exceptions);
    }

    void guest_pending_debug_exceptions(std::uint64_t value) const
    {
        return write(field::guest_pending_debug_exceptions, value);
    }

    std::uint64_t guest_ia32_sysenter_esp() const
    {
        return read(field::guest_ia32_sysenter_esp);
    }

    void guest_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_esp, value);
    }

    std::uint64_t guest_ia32_sysenter_eip() const
    {
        return read(field::guest_ia32_sysenter_eip);
    }

    void guest_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_eip, value);
    }

    std::uint64_t host_cr0() const
    {
        return read(field::host_cr0);
    }

    void host_cr0(std::uint64_t value) const
    {
        return write(field::host_cr0, value);
    }

    std::uint64_t host_cr3() const
    {
        return read(field::host_cr3);
    }

    void host_cr3(std::uint64_t value) const
    {
        return write(field::host_cr3, value);
    }

    std::uint64_t host_cr4() const
    {
        return read(field::host_cr4);
    }

    void host_cr4(std::uint64_t value) const
    {
        return write(field::host_cr4, value);
    }

    std::uint64_t host_fs_base() const
    {
        return read(field::host_fs_base);
    }

    void host_fs_base(std::uint64_t value) const
    {
        return write(field::host_fs_base, value);
    }

    std::uint64_t host_gs_base() const
    {
        return read(field::host_gs_base);
    }

    void host_gs_base(std::uint64_t value) const
    {
        return write(field::host_gs_base, value);
    }

    std::uint64_t host_tr_base() const
    {
        return read(field::host_tr_base);
    }

    void host_tr_base(std::uint64_t value) const
    {
        return write(field::host_tr_base, value);
    }

    std::uint64_t host_gdtr_base() const
    {
        return read(field::host_gdtr_base);
    }

    void host_gdtr_base(std::uint64_t value) const
    {
        return write(field::host_gdtr_base, value);
    }

    std::uint64_t host_idtr_base() const
    {
        return read(field::host_idtr_base);
    }

    void host_idtr_base(std::uint64_t value) const
    {
        return write(field::host_idtr_base, value);
    }

    std::uint64_t host_ia32_sysenter_esp() const
    {
        return read(field::host_ia32_sysenter_esp);
    }

    void host_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_esp, value);
    }

    std::uint64_t host_ia32_sysenter_eip() const
    {
        return read(field::host_ia32_sysenter_eip);
    }

    void host_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_eip, value);
    }

    std::uint64_t host_rsp() const
    {
        return read(field::host_rsp);
    }

    void host_rsp(std::uint64_t value) const
    {
        return write(field::host_rsp, value);
    }

    std::uint64_t host_rip() const
    {
        return read(field::host_rip);
    }

    void host_rip(std::uint64_t value) const
    {
        return write(field::host_rip, value);
    }

    /**
     * @}
     */
};

} // namespace zpp::arch::x86_64::vmx
