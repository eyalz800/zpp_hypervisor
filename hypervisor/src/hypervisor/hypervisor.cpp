#include "zpp/hypervisor/hypervisor.h"
#include "zpp/elf/module_region.h"
#include "zpp/maybe.h"
#include "zpp/scope_guard.h"
#include "zpp/x64/asm.h"
#include "zpp/x64/generic.h"
#include "zpp/x64/intel/asm.h"
#include "zpp/x64/intel/ept_pointer.h"
#include "zpp/x64/intel/vmcs.h"
#include "zpp/x64/intel/vmx_exit_reason.h"
#include "zpp/x64/page_table.h"
#include "zpp/x64/segment_descriptor.h"
#include "zpp/x64/vm_exit_entry.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace zpp::hypervisor
{
void hypervisor::initialize_registers()
{
    // Load control registers.
    this->guest_cr0 = x64::cr0();
    this->guest_cr3 = x64::cr3();
    this->guest_cr4 = x64::cr4();
    this->guest_dr7 = x64::dr7();

    // Load debug control register.
    this->ia32_debug_control =
        x64::intel::rdmsr(x64::intel::msr::ia32_debug_control);

    // Get the FS and GS base.
    this->ia32_fs_base = x64::intel::rdmsr(x64::intel::msr::ia32_fs_base);
    this->ia32_gs_base = x64::intel::rdmsr(x64::intel::msr::ia32_gs_base);

    // Fetch the GDT register.
    x64::gdt_layout sgdt_layout{};
    x64::sgdt(sgdt_layout.data());
    this->gdtr.limit = sgdt_layout.limit;
    this->gdtr.base = sgdt_layout.base;

    // Fetch the IDT register.
    x64::idt_layout sidt_layout{};
    x64::sidt(sidt_layout.data());
    this->idtr.limit = sidt_layout.limit;
    this->idtr.base = sidt_layout.base;

    // Load the LDTR and TR register.
    x64::sldt(&this->guest_ldtr);
    x64::str(&this->guest_tr);
}

void hypervisor::initialize_module_region()
{
    std::tie(this->module_base, this->module_size) =
        elf::get_module_region();
}

void hypervisor::initialize_os_page_table()
{
    this->os_page_table =
        x64::os_page_table(this->guest_cr3, this->physical_to_virtual);
}

void hypervisor::initialize_host_page_table()
{
    // Map the host page table into its own.
    this->host_page_table.map_self(this->os_page_table);

    // Map module pages.
    this->host_page_table.map_from(
        this->module_base,
        this->module_size,
        x64::page_table::protection::read |
            x64::page_table::protection::write |
            x64::page_table::protection::execute,
        this->os_page_table);

    // Assign the host cr3.
    this->host_cr3 = this->host_page_table.virtual_to_physical(
                         &this->host_page_table.head()) |
                     (this->guest_cr3 & 0xfff);
}

bool hypervisor::initialize_module_physical_to_virtual()
{
    // The number of pages inside the module.
    auto number_of_pages = this->module_size / page_size;

    // If there are more pages than possible, return false.
    if (number_of_pages >= this->module_physical_to_virtual.capacity()) {
        return false;
    }

    // Iterate all pages and perform the virtual to physical conversion.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        // Calculate the virtual address.
        auto address = this->module_base + (page_size * i);

        // Calculate the physical address.
        auto physical_address =
            this->host_page_table.virtual_to_physical(address);

        // Insert the mapping.
        this->module_physical_to_virtual.emplace(physical_address,
                                                 address);
    }

    return true;
}

void hypervisor::initialize_host_gdt()
{
    // Set the cs and tr indices.
    auto cs_index = 1;
    auto tr_index = 2;

    // Initialize the code segment.
    x64::segment_descriptor code_segment;
    code_segment.limit(0xfffff);
    code_segment.base(0);
    code_segment.type(
        x64::segment_descriptor::segment_type::code_execute_read);
    code_segment.system(false);
    code_segment.privilege_level(0);
    code_segment.present(true);
    code_segment.available_for_system_use(false);
    code_segment.code_64_bit(true);
    code_segment.default_operation_size(false);
    code_segment.granularity(true);
    this->host_gdt[cs_index] = code_segment.basic_value();
    this->host_cs = cs_index << 3;

    // Initialize the task state segment.
    x64::segment_descriptor task_state_segment;
    task_state_segment.limit(sizeof(this->host_tss) - 1);
    task_state_segment.base_extended(
        reinterpret_cast<std::uint64_t>(this->host_tss));
    task_state_segment.type(
        x64::segment_descriptor::segment_type::tss_available);
    task_state_segment.system(true);
    task_state_segment.privilege_level(0);
    task_state_segment.present(true);
    task_state_segment.available_for_system_use(false);
    task_state_segment.code_64_bit(false);
    task_state_segment.default_operation_size(false);
    task_state_segment.granularity(false);
    this->host_gdt[tr_index] = task_state_segment.basic_value();
    this->host_gdt[tr_index + 1] = task_state_segment.extended_value();
    this->host_tr = tr_index << 3;
}

void hypervisor::initialize_host_idt()
{
    // Do nothing for now.
}

void hypervisor::initialize_intermediate_gdt()
{
    // Copy OS GDT into intermediate GDT.
    std::memcpy(this->intermediate_gdt,
                reinterpret_cast<const char *>(this->gdtr.base),
                this->gdtr.limit + 1);
}

void hypervisor::load_intermediate_gdt()
{
    // Load the intermediate GDT.
    x64::gdt_layout lgdt_layout{};
    lgdt_layout.base =
        reinterpret_cast<std::uint64_t>(this->intermediate_gdt);
    lgdt_layout.limit = this->gdtr.limit;
    x64::lgdt(lgdt_layout.data());
}

void hypervisor::load_os_gdt()
{
    // Load the OS GDT.
    x64::gdt_layout lgdt_layout{};
    lgdt_layout.base = reinterpret_cast<std::uint64_t>(this->gdtr.base);
    lgdt_layout.limit = this->gdtr.limit;
    x64::lgdt(lgdt_layout.data());
}

void hypervisor::initialize_vmx_msrs()
{
    for (auto msr = x64::intel::msr::vmx::begin;
         msr < x64::intel::msr::vmx::end;
         ++msr) {
        this->cached_vmx_msr(msr) = x64::intel::rdmsr(msr);
    }
}

std::uint64_t & hypervisor::cached_vmx_msr(std::size_t msr)
{
    return this->vmx_msrs[msr - x64::intel::msr::vmx::begin];
}

void hypervisor::initialize_mtrrs()
{
    // Read the MTRR capabilities MSR.
    this->mtrr_capabilities = x64::intel::mtrr_capabilities(
        x64::intel::rdmsr(x64::intel::msr::ia32_mtrr_capability));

    // The MTRR variable count.
    auto variable_count =
        this->mtrr_capabilities.variable_range_register_count();

    // Iterate all MTRR registers, and read them.
    for (std::size_t i{}; i < variable_count; ++i) {
        // Read the base value.
        auto mtrr_base = x64::intel::mtrr_variable_base(
            x64::intel::rdmsr(x64::intel::msr::mtrr::physbase_0 + i * 2));

        // Read the mask value.
        auto mtrr_mask = x64::intel::mtrr_variable_mask(
            x64::intel::rdmsr(x64::intel::msr::mtrr::physmask_0 + i * 2));

        // Initialize the MTRR object.
        auto & mtrr = this->mtrrs[i];
        mtrr.type = mtrr_base.memory_type();
        mtrr.valid = mtrr_mask.valid();
        mtrr.physical_base = (mtrr_base.page_number() << 12);

        // If the mask is zero, continue.
        if (!mtrr_mask.physical_mask()) {
            mtrr.size = {};
            continue;
        }

        // Compute the size of the MTRR according to the number of 0s at
        // the lower bits of the physical mask. The rule is that
        // (address_in_range & mask == mask & base). Each found zero bit
        // multiples the size by 2, where the minimum size is a page size,
        // until 1 is reached.
        mtrr.size = page_size;
        for (auto i = mtrr_mask.physical_mask(); !(i & 1); i = (i >> 1)) {
            mtrr.size <<= 1;
        }
    }
}

void hypervisor::initialize_ept()
{
    // Fill the first epml4e for 512 GB of ram.
    this->epml4->read(true);
    this->epml4->write(true);
    this->epml4->execute(true);
    this->epml4->execute_user(true);
    this->epml4->page_number(
        this->host_page_table.virtual_to_physical(&this->epdpt) >> 12);

    // Fill a temporary RWX pdpte.
    x64::intel::epte rwx_pdpte;
    rwx_pdpte.read(true);
    rwx_pdpte.write(true);
    rwx_pdpte.execute(true);
    rwx_pdpte.execute_user(true);

    // Map every epdpt entry to a unique epd.
    for (std::size_t i{}; i < std::extent_v<decltype(this->epdpt)>; ++i) {
        this->epdpt[i] = rwx_pdpte;
        this->epdpt[i].page_number(
            this->host_page_table.virtual_to_physical(this->epd[i]) >> 12);
    }

    // Fill a temporary RWX pde.
    x64::intel::epte rwx_pde;
    rwx_pde.read(true);
    rwx_pde.write(true);
    rwx_pde.execute(true);
    rwx_pde.execute_user(true);
    rwx_pde.large(true);

    // Fill the page directory table entries with large pages.
    std::size_t large_page_number{};
    for (std::size_t i{}; i < std::extent_v<decltype(this->epd)>; ++i) {
        for (std::size_t j{};
             j <
             std::extent_v<std::remove_reference_t<decltype(*this->epd)>>;
             ++j) {
            // Calculate the physical address from the large page number.
            auto physical_address = (large_page_number << 21);

            // The current epde.
            auto & epde = this->epd[i][j];
            epde = rwx_pde;
            epde.large_page_number(large_page_number);

            // Advance to the next large page number.
            ++large_page_number;

            // Find MTRR.
            auto mtrr = std::find_if(
                std::begin(this->mtrrs),
                std::end(this->mtrrs),
                [&](auto & mtrr) {
                    // If MTRR is not valid, continue.
                    if (!mtrr.valid) {
                        return false;
                    }

                    // If below base, continue.
                    if (physical_address + ((1ull << 21) - 1) <
                        mtrr.physical_base) {
                        return false;
                    }

                    // If above end, continue.
                    if (physical_address >=
                        mtrr.physical_base + mtrr.size) {
                        return false;
                    }
                    return true;
                });

            // If MTRR not found, set to write back.
            if (std::end(this->mtrrs) == mtrr) {
                epde.type(x64::memory_type::write_back);
                continue;
            }

            // Set the type to be the MTRR type.
            epde.type(mtrr->type);
        }
    }
}

bool hypervisor::protect_module()
{
    std::size_t ept_index = 0;
    auto ept_count = std::extent_v<decltype(this->ept)>;
    auto number_of_pages = this->module_size / page_size;
    auto & host_page_table = this->host_page_table;

    // Iterate all pages.
    for (std::size_t i{}; i < number_of_pages;) {
        // Calculate the address.
        auto address = this->module_base + (i * page_size);

        // Get the physical address.
        auto physical_address =
            host_page_table.virtual_to_physical(address);

        // Get the epde.
        auto & epde = this->epd[physical_address >> 30]
                               [(physical_address >> 21) & 0x1ff];

        // If the epde is not large, get the relevant epte and update it.
        if (!epde.large()) {
            // The ept physical address.
            auto ept_physical_address = epde.page_number() << 12;

            // Find the virtual address of the ept.
            auto ept = reinterpret_cast<x64::intel::epte *>(
                this->module_physical_to_virtual
                    .find(ept_physical_address)
                    ->second);

            // Protect the epte.
            auto & epte = ept[(physical_address >> 12) & 0x1ff];
            epte.read(false);
            epte.write(false);
            epte.execute(false);
            epte.execute_user(false);

            // Continue to the next page.
            ++i;
            continue;
        }

        // Convert large epde into ept table.
        auto & ept = this->ept[ept_index];
        auto memory_type = epde.type();
        auto page_number = (epde.large_page_number() << (21 - 12));
        for (std::size_t j{}; j < 512; ++j) {
            auto & epte = ept[j];
            epte.read(true);
            epte.write(true);
            epte.execute(true);
            epte.execute_user(true);
            epte.page_number(page_number + j);
            epte.type(memory_type);
        }

        // Make the epde point to ept.
        epde.large(false);
        epde.type({});
        epde.page_number(host_page_table.virtual_to_physical(ept) >> 12);

        // Move to the next ept.
        ++ept_index;

        // If out of ept entries, return false.
        if (ept_index == ept_count) {
            return false;
        }

        // Protect our module epte.
        auto & epte = ept[(physical_address >> 12) & 0x1ff];
        epte.read(false);
        epte.write(false);
        epte.execute(false);
        epte.execute_user(false);

        // Move to the next page.
        ++i;
    }

    return true;
}

void hypervisor::initialize_vmx()
{
    namespace msr = x64::intel::msr;

    // The VMX and VMCS regions.
    auto & vmx = this->vmx[this->next_virtual_processor - 1];
    auto & vmx_vmcs = this->vmx_vmcs[this->next_virtual_processor - 1];

    // Get the value of the basic VMX msr.
    const auto & basic_msr = this->cached_vmx_msr(msr::vmx::basic);

    // Convert virtual addresses to physical addresses for VMX state.
    this->vmx_physical = this->host_page_table.virtual_to_physical(&vmx);
    this->vmcs_physical =
        this->host_page_table.virtual_to_physical(&vmx_vmcs);
    this->epml4_physical =
        this->host_page_table.virtual_to_physical(&this->epml4);
    this->msr_bitmap_physical =
        this->host_page_table.virtual_to_physical(&this->msr_bitmap);

    // Assign the revision IDs for the VMX and VMCS regions.
    vmx.revision_id = basic_msr & 0xffffffff;
    vmx_vmcs.revision_id = basic_msr & 0xffffffff;

    // Set host cr0 and cr4 to guest values.
    this->host_cr0 = this->guest_cr0;
    this->host_cr4 = this->guest_cr4;

    // Adjust the cr0 according to the MSR restrictions.
    this->host_cr0 &=
        this->cached_vmx_msr(msr::vmx::cr0_fixed_1) & 0xffffffff;
    this->host_cr0 |=
        this->cached_vmx_msr(msr::vmx::cr0_fixed_0) & 0xffffffff;

    // Adjust the cr4 according to the MSR restrictions.
    this->host_cr4 &=
        this->cached_vmx_msr(msr::vmx::cr4_fixed_1) & 0xffffffff;
    this->host_cr4 |=
        this->cached_vmx_msr(msr::vmx::cr4_fixed_0) & 0xffffffff;
}

bool hypervisor::enter_root_mode()
{
    // Backup cr0 and cr4.
    auto cr0 = x64::cr0();
    auto cr4 = x64::cr4();

    // Change cr0.
    x64::cr0(this->host_cr0);
    scope_guard restore_cr0 = [&] { x64::cr0(cr0); };

    // Change cr4.
    x64::cr4(this->host_cr4);
    scope_guard restore_cr4 = [&] { x64::cr4(cr4); };

    // Turn on vmx.
    if (x64::intel::vmxon(&this->vmx_physical)) {
        return false;
    }
    scope_guard turn_off_vmx{x64::intel::vmxoff};

    // Clear the vmcs.
    if (x64::intel::vmclear(&this->vmcs_physical)) {
        return false;
    }

    // Load the vmcs structure.
    if (x64::intel::vmptrld(&this->vmcs_physical)) {
        return false;
    }

    // Cancel all guards.
    turn_off_vmx.cancel();
    restore_cr4.cancel();
    restore_cr0.cancel();
    return true;
}

void hypervisor::setup_vmcs(x64::context & guest_context)
{
    namespace msr = x64::intel::msr;

    auto & vmcs = this->vmcs;

    // Set invalid link pointer.
    vmcs.vmcs_link_pointer(0xffffffffffffffffull);

    // Set virtual processor id.
    vmcs.vpid(this->next_virtual_processor);

    // Setup the EPT pointer.
    x64::intel::ept_pointer eptp;
    eptp.memory_type(x64::memory_type::write_back);
    eptp.page_walk_length(4);
    eptp.page_number(this->epml4_physical >> 12);
    vmcs.ept_pointer(eptp);

    // Set msr bitmap.
    vmcs.msr_bitmap(this->msr_bitmap_physical);

    // Secondary execution control.
    vmcs.secondary_processor_based_vm_execution_controls(
        x64::intel::adjust_msr(
            this->cached_vmx_msr(msr::vmx::processor_based_contorls_2),
            x64::intel::vm_execution_controls::secondary::enable_ept |
                x64::intel::vm_execution_controls::secondary::enable_vpid |
                x64::intel::vm_execution_controls::secondary::
                    enable_rdtscp |
                x64::intel::vm_execution_controls::secondary::
                    enable_invpcid |
                x64::intel::vm_execution_controls::secondary::
                    enable_xsaves_xrstors |
                x64::intel::vm_execution_controls::secondary::
                    mode_based_execute_control));

    // Pin based execution controls.
    vmcs.pin_based_vm_execution_controls(x64::intel::adjust_msr(
        this->cached_vmx_msr(msr::vmx::true_pin_based_controls), 0));

    // Primary execution controls.
    vmcs.primary_processor_based_vm_execution_controls(
        x64::intel::adjust_msr(
            this->cached_vmx_msr(msr::vmx::true_processor_based_controls),
            x64::intel::vm_execution_controls::primary::
                    enable_secondary_controls |
                x64::intel::vm_execution_controls::primary::
                    enable_msr_bitmaps));

    // VM exit in 64 bit address space.
    vmcs.vm_exit_controls(x64::intel::adjust_msr(
        this->cached_vmx_msr(msr::vmx::true_exit_controls),
        x64::intel::vm_exit_controls::host_address_space_size));

    // VM entry in 64 bit address space.
    vmcs.vm_entry_controls(x64::intel::adjust_msr(
        this->cached_vmx_msr(msr::vmx::true_entry_controls),
        x64::intel::vm_entry_controls::ia_32e_mode_guest));

    // Get the GDT base.
    auto intermediate_gdt_base =
        reinterpret_cast<std::uint64_t>(this->intermediate_gdt);

    // Write segment information.
    auto descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.cs);
    vmcs.guest_cs_selector(guest_context.cs);
    vmcs.guest_cs_limit(descriptor.limit());
    vmcs.guest_cs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_cs_base(descriptor.context_dependent_base());
    vmcs.host_cs_selector(this->host_cs);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.ds);
    vmcs.guest_ds_selector(guest_context.ds);
    vmcs.guest_ds_limit(descriptor.limit());
    vmcs.guest_ds_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ds_base(descriptor.context_dependent_base());
    vmcs.host_ds_selector(0);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.es);
    vmcs.guest_es_selector(guest_context.es);
    vmcs.guest_es_limit(descriptor.limit());
    vmcs.guest_es_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_es_base(descriptor.context_dependent_base());
    vmcs.host_es_selector(0);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.fs);
    vmcs.guest_fs_selector(guest_context.fs);
    vmcs.guest_fs_limit(descriptor.limit());
    vmcs.guest_fs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_fs_base(descriptor.context_dependent_base());
    vmcs.host_fs_base(reinterpret_cast<std::uint64_t>(this->fs_data));
    vmcs.host_fs_selector(0);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.gs);
    vmcs.guest_gs_selector(guest_context.gs);
    vmcs.guest_gs_limit(descriptor.limit());
    vmcs.guest_gs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_gs_base(this->ia32_gs_base);
    vmcs.host_gs_base(reinterpret_cast<std::uint64_t>(this->gs_data));
    vmcs.host_gs_selector(0);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.ss);
    vmcs.guest_ss_selector(guest_context.ss);
    vmcs.guest_ss_limit(descriptor.limit());
    vmcs.guest_ss_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ss_base(descriptor.context_dependent_base());
    vmcs.host_ss_selector(0);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, this->guest_tr);
    vmcs.guest_tr_selector(this->guest_tr);
    vmcs.guest_tr_limit(x64::load_segment_limit(this->guest_tr));
    vmcs.guest_tr_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_tr_base(descriptor.context_dependent_base());
    vmcs.host_tr_base(reinterpret_cast<std::uint64_t>(this->host_tss));
    vmcs.host_tr_selector(this->host_tr);

    descriptor = x64::segment_descriptor::from_memory(
        intermediate_gdt_base, this->guest_ldtr);
    vmcs.guest_ldtr_selector(this->guest_ldtr);
    vmcs.guest_ldtr_limit(descriptor.limit());
    vmcs.guest_ldtr_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ldtr_base(descriptor.context_dependent_base());

    // Set gdtr information.
    vmcs.guest_gdtr_limit(this->gdtr.limit);
    vmcs.guest_gdtr_base(this->gdtr.base);
    vmcs.host_gdtr_base(reinterpret_cast<std::uint64_t>(this->host_gdt));

    // Set idtr information.
    vmcs.guest_idtr_limit(this->idtr.limit);
    vmcs.guest_idtr_base(this->idtr.base);
    vmcs.host_idtr_base(reinterpret_cast<std::uintptr_t>(this->host_idt));

    // Load CR0
    vmcs.cr0_read_shadow(this->guest_cr0);
    vmcs.guest_cr0(this->host_cr0);
    vmcs.host_cr0(this->host_cr0);

    // Load CR3
    vmcs.guest_cr3(this->guest_cr3);
    vmcs.host_cr3(this->host_cr3);

    // Load CR4
    vmcs.cr4_read_shadow(this->guest_cr4);
    vmcs.guest_cr4(this->host_cr4);
    vmcs.host_cr4(this->host_cr4);

    // Load debug MSR and register.
    vmcs.guest_ia32_debugctl(this->ia32_debug_control);
    vmcs.guest_dr7(this->guest_dr7);

    // Load rflags.
    vmcs.guest_rflags(guest_context.rflags);
}

template <typename VmmCode>
void hypervisor::vm_launch(x64::context & guest_context,
                           VmmCode && vmm_code)
{
    auto & vmcs = this->vmcs;

    // Allocate a small stack for the host VM exit.
    alignas(0x10) unsigned char host_vm_launch_stack[0x1500]{};

    // The host VM exit rsp.
    auto host_rsp = reinterpret_cast<std::uint64_t>(
        std::end(host_vm_launch_stack) - (2 * sizeof(x64::context)));

    // Construct the guest context on the host stack.
    auto & local_guest_context =
        *::new (reinterpret_cast<void *>(host_rsp)) x64::context;

    // Construct the host context on the host stack.
    auto & host_context =
        *::new (reinterpret_cast<void *>(host_rsp + sizeof(x64::context)))
            x64::context;

    // Write host rip.
    vmcs.host_rip(reinterpret_cast<std::uint64_t>(x64::vm_exit_entry));

    // Write host rsp.
    vmcs.host_rsp(host_rsp);

    // Write guest rip, rsp.
    vmcs.guest_rip(guest_context.rip);
    vmcs.guest_rsp(guest_context.rsp);

    // VM exit flag which is set to false initially and switched to true
    // later to change the flow of this code on VM exit.
    std::atomic<bool> vm_exit_flag;

    // Set VM exit flag to false, when there is a VM
    // exit it will be already be true, following the VM exit path
    // in this code rather than the VM launch.
    vm_exit_flag = false;

    // Just to be sure the compiler does not move stuff around,
    // we pass the vm_exit_flag address and guest_context address
    // inside the capture context.
    // This way the compiler cannot reason about destroying
    // our host_context, vm_exit_flag and host_vm_launch_stack because
    // guest_context which is last referenced in this function is sent to
    // the restore context function and can theoretically depend on
    // host_context, vm_exit_flag, and host_vm_launch_stack, after the
    // capture host context.
    host_context.rax = reinterpret_cast<std::uint64_t>(&vm_exit_flag);
    host_context.rbx = reinterpret_cast<std::uint64_t>(&guest_context);

    // Capture host context.
    x64::capture_context(&host_context);

    // If VM exit, call the VMM code.
    if (vm_exit_flag) {
        // Call VMM code, which never returns.
        vmm_code(local_guest_context);

        // Error: vmm_code returned.
        return;
    }

    // Update the segment selectors for host.
    host_context.cs = this->host_cs;
    host_context.ds = 0;
    host_context.es = 0;
    host_context.fs = 0;
    host_context.gs = 0;
    host_context.ss = 0;

    // Increment the virtual processor id.
    ++this->next_virtual_processor;

    // The next time we arrive after the capture context is due to VM
    // exit.
    vm_exit_flag = true;

    // Start executing the guest at vmlaunch.
    guest_context.rip =
        reinterpret_cast<std::uint64_t>(x64::intel::vmlaunch);

    // Set rflags to host rflags, to leave interrupts disabled.
    guest_context.rflags = host_context.rflags;

    // Set return value of guest to success.
    guest_context.rax = 0;

    // Launch the VM.
    x64::restore_context(&guest_context);
}

int hypervisor::main(x64::context & caller_context)
{
    // Fetch parameters.
    auto cpuid = caller_context.rdi;
    auto physical_to_virtual =
        reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(
            caller_context.rsi);

    // Disable interrupts.
    x64::disable_interrupts();

    // Guard to enable interrupts.
    scope_guard restore_interrupts{x64::enable_interrupts};

    // Initialize page table operations.
    this->physical_to_virtual = physical_to_virtual;

    // Initialize special registers.
    initialize_registers();

    // Perform only on first CPU load.
    if (0 == cpuid) {
        // Initialize memory region.
        initialize_module_region();

        // Initialize OS page table.
        initialize_os_page_table();

        // Initialize host page table.
        initialize_host_page_table();

        // Initialize module physical to virtual translation.
        if (!initialize_module_physical_to_virtual()) {
            return 1;
        }

        // Initialize host IDT.
        initialize_host_idt();

        // Initialize host GDT.
        initialize_host_gdt();
    }

    // Initialize intermediate GDT.
    initialize_intermediate_gdt();

    // Load intermediate GDT.
    load_intermediate_gdt();

    // Guard to restore GDT.
    scope_guard restore_gdt = [&] { load_os_gdt(); };

    // Switch page tables.
    x64::cr3(this->host_cr3);

    // Guard to restore cr3.
    scope_guard restore_cr3 = [&] { x64::cr3(this->guest_cr3); };

    // Perform only on first CPU load.
    if (0 == cpuid) {
        // Initialize VMX MSRS.
        initialize_vmx_msrs();

        // Initialize MTRRS.
        initialize_mtrrs();

        // Initialize the EPT.
        initialize_ept();

        // Protect module.
        if (!protect_module()) {
            return 1;
        }
    }

    // Initialize vmx.
    initialize_vmx();

    // Enter root mode.
    if (!enter_root_mode()) {
        return 1;
    }

    // Guard to turn off vmx.
    scope_guard turn_off_vmx{x64::intel::vmxoff};

    // Setup vmcs.
    setup_vmcs(caller_context);

    // Launch VM.
    vm_launch(caller_context, [&](auto & context) {
        using basic_reason = x64::intel::exit_reason::basic_reason;
        auto & vmcs = this->vmcs;

        // Virtual processor id.
        auto vpid = vmcs.vpid().value();
        static_cast<void>(vpid);

        // The basic exit reason.
        basic_reason reason{};

        // Get the exit reason.
        if (auto exit_reason = vmcs.exit_reason(); !exit_reason) {
            return;
        } else {
            reason = x64::intel::exit_reason(exit_reason.value()).basic();
        }

        // Get the guest RIP.
        context.rip = vmcs.guest_rip().value();

        // Check the exit reason.
        switch (reason) {
        case basic_reason::cpuid: {
            std::uint32_t cpuid_result[4]{};

            // Execute the cpuid instruction.
            x64::cpuid(context.rax, context.rcx, cpuid_result);

            // If needs to set hypervisor present bit.
            if (1 == context.rax) {
                // Set hypervisor present bit.
                cpuid_result[2] |= (1 << 31);
            } else if ((1 << 30) == context.rax) {
                // HyperVisor Name: ZppZppZppZpp.
                cpuid_result[1] = 0x5a70705a;
                cpuid_result[2] = 0x705a7070;
                cpuid_result[3] = 0x70705a70;
            }

            // Place the cpuid result into the context.
            context.rax = cpuid_result[0];
            context.rbx = cpuid_result[1];
            context.rcx = cpuid_result[2];
            context.rdx = cpuid_result[3];
            break;
        }
        default: {
            break;
        }
        }

        // Update RIP.
        context.rip += vmcs.vm_exit_instruction_length().value();
        vmcs.guest_rip(context.rip);

        // Resume the VM.
        context.rip =
            reinterpret_cast<std::uint64_t>(x64::intel::vmresume);

        // Restore VM.
        x64::restore_context(&context);
    });

    return 1;
}

void hypervisor::launch_on_cpu_private_stack(hypervisor & hypervisor,
                                             x64::context & caller_context)
{
    // Invoke the main function.
    auto result = hypervisor.main(caller_context);

    // Use result as return value.
    caller_context.rax = result;

    // Restore context to caller.
    x64::restore_context(&caller_context);
}

void hypervisor::launch_on_cpu(x64::context & caller_context)
{
    // Fetch the stack the hypervisor will launch with.
    auto & stack = this->stack[this->available_stack_index];

    // Compute the stack top.
    auto stack_top = stack + sizeof(stack) - sizeof(x64::context);

    // Copy construct caller context into the new stack top.
    auto copied_caller_context =
        ::new (stack_top) x64::context(caller_context);

    // Use the caller context as launch context.
    auto & launch_context = caller_context;

    // Set instruction pointer to the launch function.
    launch_context.rip =
        reinterpret_cast<std::uint64_t>(launch_on_cpu_private_stack);

    // Set stack pointer to the stack top.
    launch_context.rsp = reinterpret_cast<std::uint64_t>(stack_top);

    // Simulate call instruction for proper stack alignment.
    launch_context.rsp -= sizeof(std::uint64_t);

    // Set the this pointer.
    launch_context.rdi = reinterpret_cast<std::uint64_t>(this);

    // Set first argument to copied caller context pointer.
    launch_context.rsi =
        reinterpret_cast<std::uint64_t>(copied_caller_context);

    // Increment stack index.
    ++this->available_stack_index;

    // Restore context to launch context.
    x64::restore_context(&launch_context);
}

} // namespace zpp::hypervisor
