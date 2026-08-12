#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <cstdint>

/**
 * VMCS shadowing: letting the guest hypervisor read and write its own
 * VMCS without an exit.
 *
 * Separate from nested_vmx.cpp because this is the only part of the
 * nested state machine that executes VMX instructions. That file is
 * compiled natively by tests/nested_vmx as a differential harness, on a
 * machine with no VMX at all, and VMPTRLD in it would not assemble.
 * Everything here is stubbed in that harness's shim instead, which is
 * also what a processor that does not offer the control does.
 */
namespace zpp::hypervisor
{
using arch::x86_64::vmx::vmcs_field_encoding;

namespace
{
using field = arch::x86_64::vmx::vmcs::field;

/**
 * The fields the guest hypervisor may read without an exit.
 *
 * The exit-information fields are here and not in the writable list
 * because they are read-only to a guest hypervisor: it reads them on
 * every exit it takes, and VMWRITE to one of them faults unless the
 * processor reports "VMWRITE to any supported field", which this VMM does
 * not report. So they need copying in one direction only, at the point
 * this VMM writes them - the reflection.
 */
constexpr field shadow_read_only_fields[] = {
    field::exit_reason,
    field::exit_qualification,
    field::vm_exit_interruption_information,
    field::vm_exit_interruption_error_code,
    field::vm_exit_instruction_length,
    field::vm_exit_instruction_information,
    field::idt_vectoring_information_field,
    field::idt_vectoring_error_code,
    field::guest_physical_address,
    field::guest_linear_address,
};

/**
 * The fields the guest hypervisor may both read and write without an
 * exit.
 *
 * Every one of these has to be copied *both* ways, and the ordering rule
 * that keeps that correct is in copy_shadow_to_vmcs12: the guest
 * hypervisor's silent writes are collected before this VMM overwrites the
 * region, never after.
 *
 * Guest state that a VM exit reports and a VM entry consumes - RIP, RSP,
 * RFLAGS, the control registers, interruptibility - is the bulk of it,
 * because that is what an exit handler reads on the way in and writes on
 * the way out. The entry-interruption fields are here for the same
 * reason: injecting an event is a write, and it happens on the path this
 * is trying to make free.
 */
constexpr field shadow_read_write_fields[] = {
    // Measured, and it was not on KVM's list. With everything else here
    // shadowed, Hyper-V's remaining VMREAD traffic was 45,866 reads of
    // which 45,866 were DR7 and six were anything else - one per exit it
    // handles, from its own exit path. KVM's vmcs_shadow_fields.h does
    // not shadow it, which is the whole argument for measuring the guest
    // in front of you rather than copying another VMM's list.
    field::guest_dr7,
    field::guest_rip,
    field::guest_rsp,
    field::guest_rflags,
    field::guest_cr0,
    field::guest_cr3,
    field::guest_cr4,
    field::cr0_read_shadow,
    field::cr4_read_shadow,
    field::cr0_guest_host_mask,
    field::cr4_guest_host_mask,
    field::guest_interruptibility_state,
    field::guest_activity_state,
    field::vm_entry_interruption_information_field,
    field::vm_entry_exception_error_code,
    field::vm_entry_instruction_length,
    field::exception_bitmap,
    field::pin_based_vm_execution_controls,
    field::primary_processor_based_vm_execution_controls,
    field::tpr_threshold,
    field::guest_cs_selector,
    field::guest_cs_access_rights,
    field::guest_ss_access_rights,
    field::guest_interrupt_status,
    field::host_fs_base,
    field::host_gs_base,
};

/**
 * Clears a field's bit in a bitmap, meaning "answer this one from the
 * shadow region rather than exiting".
 *
 * The bitmaps are indexed by the encoding directly, and only encodings
 * below 0x8000 are indexable - SDM 26.2 sends anything above that to a VM
 * exit whatever the bitmap says. Every encoding this VMM shadows is far
 * below it, so a value that is not is a mistake rather than a case to
 * handle, and it is left set.
 */
[[maybe_unused]] constexpr void permit_field(std::uint8_t * bitmap,
                                             std::uint64_t encoding)
{
    if (encoding >= 0x8000) {
        return;
    }

    bitmap[encoding / 8] &=
        static_cast<std::uint8_t>(~(1u << (encoding % 8)));
}

} // namespace

/**
 * Builds the VMREAD and VMWRITE bitmaps, once, and records whether the
 * processor offers the control at all.
 *
 * Called from every processor's VMCS setup and does its work on the
 * first: the contents do not depend on the processor, and the alternative
 * - a separate boot-processor-only initialisation step - is one more
 * ordering constraint for no gain.
 */
void hypervisor::initialize_vmcs_shadowing()
{
    if constexpr (!nested_vmx::enabled ||
                  !nested_vmx::shadow_vmcs_enabled) {
        // Left false, so set_vmcs_shadowing and both copies are no-ops
        // and the guest hypervisor exits for every field as it always
        // did. Said out loud, because a run that was meant to have this
        // on and did not must not be read as the feature failing.
        this->vmcs_shadowing_enabled = false;
        log("vmcs shadowing switched off in this build");
        return;
    } else {
        if (0 != this->vmcs_shadow_read_bitmap_physical) {
            return;
        }

        // The allowed-1 settings live in the high half of the capability
        // MSR. A processor that does not offer the control gets the
        // bitmaps written and the control never requested, which costs two
        // VMCS fields and changes nothing else.
        constexpr auto shadowing = arch::x86_64::vmx::
            vm_execution_controls::secondary::vmcs_shadowing;
        this->vmcs_shadowing_enabled =
            0 !=
            ((this->cached_vmx_msr(
                  arch::x86_64::vmx::msr::processor_based_contorls_2) >>
              32) &
             shadowing);

        // All ones is "exit for everything", which is what this VMM did
        // before there were bitmaps at all - so a field left out of the
        // lists below behaves exactly as it used to.
        for (auto & byte : this->vmcs_shadow_read_bitmap) {
            byte = 0xff;
        }
        for (auto & byte : this->vmcs_shadow_write_bitmap) {
            byte = 0xff;
        }

        for (auto entry : shadow_read_only_fields) {
            permit_field(this->vmcs_shadow_read_bitmap,
                         static_cast<std::uint64_t>(entry));
        }

        for (auto entry : shadow_read_write_fields) {
            permit_field(this->vmcs_shadow_read_bitmap,
                         static_cast<std::uint64_t>(entry));
            permit_field(this->vmcs_shadow_write_bitmap,
                         static_cast<std::uint64_t>(entry));
        }

        this->vmcs_shadow_read_bitmap_physical =
            this->host_page_table.virtual_to_physical(
                this->vmcs_shadow_read_bitmap);
        this->vmcs_shadow_write_bitmap_physical =
            this->host_page_table.virtual_to_physical(
                this->vmcs_shadow_write_bitmap);

        log("vmcs shadowing {}",
            this->vmcs_shadowing_enabled ? "available" : "not offered");
    }
}

/**
 * Turns the control on or off for this processor, together with the link
 * pointer it requires.
 *
 * The two move together and must: SDM 27.3.1.5 makes a VM entry fail if
 * the control is set and the link pointer does not name a valid shadow
 * region. So "no VMCS of the guest hypervisor's is current" is expressed
 * by clearing both, which is also what KVM's vmx_disable_shadow_vmcs
 * does.
 */
void hypervisor::set_vmcs_shadowing(std::size_t cpu, bool enabled)
{
    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        constexpr auto shadowing = arch::x86_64::vmx::
            vm_execution_controls::secondary::vmcs_shadowing;

        auto controls =
            this->vmcs.secondary_processor_based_vm_execution_controls();

        // No adjust_msr on either write below: the capability checked
        // above is the same test, made once. `vmcs_shadowing_enabled` is
        // assigned from the allowed-1 half of IA32_VMX_PROCBASED_CTLS2
        // and this function returns early when it is false.
        if (enabled) {
            this->vmcs.vmcs_link_pointer(this->shadow_vmcs_physical[cpu]);
            this->vmcs.secondary_processor_based_vm_execution_controls(
                controls | shadowing);
            copy_vmcs12_to_shadow(cpu);
        } else {
            // Same capability checked above, and SDM A.3.3 reserves the
            // allowed-0 half of the secondary controls to zero, so
            // clearing a bit there can violate neither half.
            this->vmcs.secondary_processor_based_vm_execution_controls(
                controls & ~static_cast<std::uint64_t>(shadowing));
            this->vmcs.vmcs_link_pointer(~std::uint64_t{});
        }
    }
}

/**
 * Publishes this VMM's cached vmcs12 into the shadow region, so the guest
 * hypervisor's next VMREAD of a shadowed field finds what this VMM would
 * have answered.
 *
 * Loading the region makes it current, which is the only way its fields
 * can be written - the format is not architecturally defined and must not
 * be written as memory. It is cleared again afterwards so its contents
 * reach memory rather than staying in whatever the processor caches, and
 * the VMCS that was current is put back. VMPTRST rather than a remembered
 * pointer because this is called from both the vmcs01 and the reflection
 * paths, and a wrong restore here would be a VM entry against the wrong
 * VMCS.
 */
void hypervisor::copy_vmcs12_to_shadow(std::size_t cpu)
{
    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        std::uint64_t previous{};
        if (arch::x86_64::vmx::vmptrst(&previous)) {
            return;
        }

        if (arch::x86_64::vmx::vmptrld(&this->shadow_vmcs_physical[cpu])) {
            return;
        }

        auto & cached = this->guest_vmcs12[cpu];
        for (auto entry : shadow_read_only_fields) {
            this->vmcs.write(entry,
                             cached.read(vmcs_field_encoding(
                                 static_cast<std::uint64_t>(entry))));
        }
        for (auto entry : shadow_read_write_fields) {
            this->vmcs.write(entry,
                             cached.read(vmcs_field_encoding(
                                 static_cast<std::uint64_t>(entry))));
        }

        arch::x86_64::vmx::vmclear(&this->shadow_vmcs_physical[cpu]);
        arch::x86_64::vmx::vmptrld(&previous);

        this->vmcs_shadow_stores[cpu] = this->vmcs_shadow_stores[cpu] + 1;
    }
}

/**
 * Collects the writable fields back out of the shadow region, because the
 * guest hypervisor may have written any of them without this VMM seeing
 * it - which is the entire point of the feature.
 *
 * Must run before anything reads the cached vmcs12 after the guest
 * hypervisor has had a chance to run, and before any copy in the other
 * direction, which would otherwise discard those writes.
 */
void hypervisor::copy_shadow_to_vmcs12(std::size_t cpu)
{
    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        std::uint64_t previous{};
        if (arch::x86_64::vmx::vmptrst(&previous)) {
            return;
        }

        if (arch::x86_64::vmx::vmptrld(&this->shadow_vmcs_physical[cpu])) {
            return;
        }

        auto & cached = this->guest_vmcs12[cpu];
        for (auto entry : shadow_read_write_fields) {
            cached.write(
                vmcs_field_encoding(static_cast<std::uint64_t>(entry)),
                this->vmcs.read(entry));
        }

        arch::x86_64::vmx::vmclear(&this->shadow_vmcs_physical[cpu]);
        arch::x86_64::vmx::vmptrld(&previous);

        this->vmcs_shadow_loads[cpu] = this->vmcs_shadow_loads[cpu] + 1;
    }
}
} // namespace zpp::hypervisor
