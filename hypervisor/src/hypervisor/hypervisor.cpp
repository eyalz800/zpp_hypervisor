#include "zpp/hypervisor/hypervisor.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/exception_entry.h"
#include "zpp/arch/x86_64/generic.h"
#include "zpp/arch/x86_64/interrupt_gate.h"
#include "zpp/arch/x86_64/page_table.h"
#include "zpp/arch/x86_64/segment_descriptor.h"
#include "zpp/arch/x86_64/vm_exit_entry.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/crt.h"
#include "zpp/elf_file.h"
#include "zpp/elf_image_base.h"
#include "zpp/error.h"
#include "zpp/scope_exit.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace zpp::hypervisor
{
hypervisor & hypervisor::instance()
{
    static hypervisor instance;
    return instance;
}

void hypervisor::initialize_registers()
{
    // Load control registers.
    this->guest_cr0 = arch::x86_64::cr0();
    this->guest_cr3 = arch::x86_64::cr3();
    this->guest_cr4 = arch::x86_64::cr4();
    this->guest_dr7 = arch::x86_64::dr7();

    // Load debug control register.
    this->ia32_debug_control =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_debug_control);

    // Get the FS and GS base.
    this->ia32_fs_base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_fs_base);
    this->ia32_gs_base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_gs_base);

    // Fetch the GDT register.
    arch::x86_64::gdt_layout sgdt_layout{};
    arch::x86_64::sgdt(sgdt_layout.data());
    this->gdtr.limit = sgdt_layout.limit;
    this->gdtr.base = sgdt_layout.base;

    // Fetch the IDT register.
    arch::x86_64::idt_layout sidt_layout{};
    arch::x86_64::sidt(sidt_layout.data());
    this->idtr.limit = sidt_layout.limit;
    this->idtr.base = sidt_layout.base;

    // Load the LDTR and TR register.
    arch::x86_64::sldt(&this->guest_ldtr);
    arch::x86_64::str(&this->os_tr);
}

void hypervisor::initialize_module_region()
{
    // Compute the module base.
    this->module_base = elf_image_base();

    // Compute the module memory size.
    this->module_size =
        elf_file(this->module_base, elf_file::state::loaded).memory_size();
}

void hypervisor::initialize_os_page_table()
{
    this->os_page_table = arch::x86_64::os_page_table(
        this->guest_cr3, this->physical_to_virtual);
}

void hypervisor::initialize_host_page_table()
{
    // Map the host page table into its own.
    this->host_page_table.map_self(this->os_page_table);

    // Map module pages.
    this->host_page_table.map_from(
        this->module_base,
        this->module_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write |
            arch::x86_64::page_table::protection::execute,
        this->os_page_table);

    // Assign the host cr3.
    this->host_cr3 = this->host_page_table.virtual_to_physical(
                         &this->host_page_table.head()) |
                     (this->guest_cr3 & 0xfff);
}

std::expected<void, zpp::error>
hypervisor::initialize_module_physical_to_virtual()
{
    // The number of pages inside the module.
    auto number_of_pages = this->module_size / page_size;

    // If there are more pages than possible, return error.
    if (number_of_pages >= this->module_physical_to_virtual.capacity()) {
        return std::unexpected(
            zpp::error{error::physical_to_virtual_capacity_error});
    }

    // The module base.
    auto module_base = reinterpret_cast<std::uintptr_t>(this->module_base);

    // Iterate all pages and perform the virtual to physical conversion.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        // Calculate the virtual address.
        auto address = module_base + (page_size * i);

        // Calculate the physical address.
        auto physical_address =
            this->host_page_table.virtual_to_physical(address);

        // Insert the mapping.
        this->module_physical_to_virtual.emplace(physical_address,
                                                 address);
    }

    return {};
}

void hypervisor::initialize_host_gdt()
{
    // The index the OS keeps its own code segment at. The host IDT gates
    // have to name it, for the reason spelled out at the alias below, so
    // our own descriptors have to keep clear of it.
    auto os_cs_index = arch::x86_64::cs() >> 3;

    // Set the cs and tr indices. The code segment takes one entry and the
    // task segment two, so the pair occupies three consecutive entries.
    // Placing them at one or at four means whichever pair is chosen cannot
    // contain the OS code selector index.
    auto cs_index = (os_cs_index <= 3) ? 4 : 1;
    auto tr_index = cs_index + 1;

    // Initialize the code segment.
    arch::x86_64::segment_descriptor code_segment;
    code_segment.limit(0xfffff);
    code_segment.base(0);
    code_segment.type(
        arch::x86_64::segment_descriptor::segment_type::code_execute_read);
    code_segment.system(false);
    code_segment.privilege_level(0);
    code_segment.present(true);
    code_segment.available_for_system_use(false);
    code_segment.code_64_bit(true);
    code_segment.default_operation_size(false);
    code_segment.granularity(true);
    this->host_gdt[cs_index] = code_segment.basic_value();
    this->host_cs = cs_index << 3;

    // Alias the same code segment at the OS code selector index. The host
    // IDT is loaded under two different GDTs - the intermediate GDT before
    // the VM is launched, and the host GDT after a VM exit - and its gates
    // carry a single selector, which therefore has to name a 64 bit code
    // segment in both. The intermediate GDT is a copy of the OS GDT, so
    // that selector has to be the OS code selector, and this alias is what
    // makes it resolve here as well. See initialize_host_idt.
    this->host_gdt[os_cs_index] = code_segment.basic_value();

    // Initialize the task state segment.
    arch::x86_64::segment_descriptor task_state_segment;
    task_state_segment.limit(sizeof(this->host_tss) - 1);
    task_state_segment.base_extended(
        reinterpret_cast<std::uint64_t>(this->host_tss));
    task_state_segment.type(
        arch::x86_64::segment_descriptor::segment_type::tss_available);
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
    // The selector the gates name. Not host_cs: the IDT is loaded while
    // the intermediate GDT is active as well, where our own descriptors do
    // not exist, so the gates go through the OS code selector, which
    // initialize_host_gdt aliased into the host GDT for this.
    auto selector = arch::x86_64::cs();

    // One 64 bit interrupt gate per vector, each pointing at the stub for
    // that vector. Interrupt rather than trap gates, so a handler cannot
    // be interrupted, and no interrupt stack table entry, so a handler
    // runs on the stack that was already in use - which is the hypervisor
    // stack, and is where the faulting frame is.
    for (std::size_t vector{};
         vector < arch::x86_64::number_of_exception_vectors;
         ++vector) {
        arch::x86_64::interrupt_gate gate;
        gate.offset(arch::x86_64::exception_entry(vector));
        gate.selector(selector);
        gate.interrupt_stack_table(0);
        gate.type(arch::x86_64::interrupt_gate::gate_type::interrupt);
        gate.privilege_level(0);
        gate.present(true);

        this->host_idt[vector * 2] = gate.basic_value();
        this->host_idt[(vector * 2) + 1] = gate.extended_value();
    }

    this->host_idtr.base = reinterpret_cast<std::uint64_t>(this->host_idt);
    this->host_idtr.limit = (arch::x86_64::number_of_exception_vectors *
                             2 * sizeof(std::uint64_t)) -
                            1;
}

void hypervisor::load_host_idt()
{
    arch::x86_64::idt_layout lidt_layout{};
    lidt_layout.base = this->host_idtr.base;
    lidt_layout.limit = this->host_idtr.limit;
    arch::x86_64::lidt(lidt_layout.data());
}

void hypervisor::load_os_idt()
{
    arch::x86_64::idt_layout lidt_layout{};
    lidt_layout.base = this->idtr.base;
    lidt_layout.limit = this->idtr.limit;
    arch::x86_64::lidt(lidt_layout.data());
}

void hypervisor::on_host_exception(
    const arch::x86_64::exception_frame & frame)
{
    // Record before touching anything that could fault again, so there is
    // something to read even if this handler does not survive.
    this->host_exception = frame;
    this->host_exception_cr2 = arch::x86_64::cr2();

    // Take the recovery point, if there is one, and consume it - unwinding
    // to it twice would land on a frame that has already returned.
    auto recovery_flag = this->host_exception_recovery_flag;
    this->host_exception_recovery_flag = nullptr;

    // No recovery point. This is the state once the guest is running: the
    // VMCS points the host IDTR at our IDT, so an exception in the VMM
    // arrives here, but main's frame is long gone by then. Stop instead of
    // unwinding into it. A real VMM exception handler is what this wants
    // eventually.
    if (!recovery_flag) {
        for (;;) {
            arch::x86_64::disable_interrupts();
            arch::x86_64::halt();
        }
    }

    // Tell main it is arriving from an exception rather than from the
    // capture, then unwind. This is a longjmp, not an unwind of the C++
    // stack, so everything between the fault and main's frame is dropped -
    // but main's frame itself is intact, and that is where the guards that
    // put the machine back the way it was found live.
    *recovery_flag = true;
    arch::x86_64::restore_context(&this->host_exception_recovery);

    // restore_context does not return.
    std::unreachable();
}

void hypervisor::initialize_intermediate_gdt()
{
    // Fetch the intermediate GDT.
    auto & intermediate_gdt =
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1];

    // Fetch the guest TSS.
    auto & guest_tss = this->unprotected_memory
                           .guest_tss[this->next_virtual_processor - 1];

    // Copy OS Created GDT into our intermediate GDT.
    std::memcpy(intermediate_gdt,
                reinterpret_cast<const char *>(this->gdtr.base),
                this->gdtr.limit + 1);

    // If the TSS segment is present, just use the current OS GDT.
    if (auto task_state_segment =
            arch::x86_64::segment_descriptor::from_memory(
                reinterpret_cast<std::uint64_t>(intermediate_gdt),
                this->os_tr);
        task_state_segment.present()) {
        // Use the current gdtr base as guest GDT pointer.
        this->guest_gdt_pointer =
            reinterpret_cast<std::uint64_t *>(this->gdtr.base);

        // Use the guest GDT as current gdtr limit.
        this->guest_gdt_limit = this->gdtr.limit;

        // Set the intermediate GDT limit as current gdtr limit.
        this->intermediate_gdt_limit = this->gdtr.limit;

        // Use the current TR as the guest TR.
        this->guest_tr = this->os_tr;
        return;
    }

    // Set the guest GDT pointer to the intermediate GDT.
    this->guest_gdt_pointer =
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1];

    // Increase the intermediate GDT limit by one entry.
    this->intermediate_gdt_limit =
        this->gdtr.limit + (2 * sizeof(std::uint64_t));

    // Guest GDT limit is the same as intermediate GDT limit.
    this->guest_gdt_limit = this->intermediate_gdt_limit;

    // The index of the TSS segment.
    auto tr_index = (this->gdtr.limit + 1) / sizeof(std::uint64_t);

    // Create a task state segment.
    arch::x86_64::segment_descriptor task_state_segment;
    task_state_segment.limit(sizeof(guest_tss) - 1);
    task_state_segment.base_extended(
        reinterpret_cast<std::uint64_t>(guest_tss));
    task_state_segment.type(
        arch::x86_64::segment_descriptor::segment_type::tss_available);
    task_state_segment.system(true);
    task_state_segment.privilege_level(0);
    task_state_segment.present(true);
    task_state_segment.available_for_system_use(false);
    task_state_segment.code_64_bit(false);
    task_state_segment.default_operation_size(false);
    task_state_segment.granularity(false);

    // Assign the task state segment.
    intermediate_gdt[tr_index] = task_state_segment.basic_value();
    intermediate_gdt[tr_index + 1] = task_state_segment.extended_value();
    this->guest_tr = tr_index << 3;
}

void hypervisor::load_intermediate_gdt()
{
    // Load the intermediate GDT.
    arch::x86_64::gdt_layout lgdt_layout{};
    lgdt_layout.base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1]);
    lgdt_layout.limit = this->intermediate_gdt_limit;
    arch::x86_64::lgdt(lgdt_layout.data());

    // Load TSS segment if changed.
    if (this->guest_tr != this->os_tr) {
        arch::x86_64::ltr(&this->guest_tr);
    }
}

void hypervisor::load_os_gdt()
{
    // Load the OS GDT.
    arch::x86_64::gdt_layout lgdt_layout{};
    lgdt_layout.base = reinterpret_cast<std::uint64_t>(this->gdtr.base);
    lgdt_layout.limit = this->gdtr.limit;
    arch::x86_64::lgdt(lgdt_layout.data());

    // Load OS TSS segment if changed. Skipped when the OS had no task
    // segment at all, which is the UEFI case that
    // initialize_intermediate_gdt synthesizes one for: the selector to put
    // back would be null, and ltr rejects a null selector with a general
    // protection fault. There is no instruction that unloads the task
    // register, so the synthesized segment stays loaded - which is
    // harmless, because it lives in unprotected memory that outlives this
    // module either way.
    if (this->os_tr && this->guest_tr != this->os_tr) {
        arch::x86_64::ltr(&this->os_tr);
    }
}

void hypervisor::initialize_vmx_msrs()
{
    for (auto msr = arch::x86_64::vmx::msr::begin;
         msr < arch::x86_64::vmx::msr::end;
         ++msr) {
        this->cached_vmx_msr(msr) = arch::x86_64::rdmsr(msr);
    }
}

std::uint64_t & hypervisor::cached_vmx_msr(std::size_t msr)
{
    return this->vmx_msrs[msr - arch::x86_64::vmx::msr::begin];
}

void hypervisor::initialize_mtrrs()
{
    // Read the MTRR capabilities MSR.
    this->mtrr_capabilities = arch::x86_64::mtrr_capabilities(
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_mtrr_capability));

    // The MTRR variable count.
    auto variable_count =
        this->mtrr_capabilities.variable_range_register_count();

    // Iterate all MTRR registers, and read them.
    for (std::size_t i{}; i < variable_count; ++i) {
        // Read the base value.
        auto mtrr_base =
            arch::x86_64::mtrr_variable_base(arch::x86_64::rdmsr(
                arch::x86_64::msr::mtrr::physbase_0 + i * 2));

        // Read the mask value.
        auto mtrr_mask =
            arch::x86_64::mtrr_variable_mask(arch::x86_64::rdmsr(
                arch::x86_64::msr::mtrr::physmask_0 + i * 2));

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
    arch::x86_64::vmx::epte rwx_pdpte;
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
    arch::x86_64::vmx::epte rwx_pde;
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
                epde.type(arch::x86_64::memory_type::write_back);
                continue;
            }

            // Set the type to be the MTRR type.
            epde.type(mtrr->type);
        }
    }
}

std::expected<void, zpp::error> hypervisor::protect_module()
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
            auto ept = reinterpret_cast<arch::x86_64::vmx::epte *>(
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

        // If out of ept entries, return error.
        if (ept_index == ept_count) {
            return std::unexpected(zpp::error{error::out_of_ept_entries});
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

    return {};
}

void hypervisor::unprotect_guest_memory()
{
    auto number_of_pages = sizeof(this->unprotected_memory) / page_size;

    // Iterate all pages.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        // Calculate the address.
        auto address =
            reinterpret_cast<unsigned char *>(&this->unprotected_memory) +
            (i * page_size);

        // Get the physical address.
        auto physical_address =
            this->host_page_table.virtual_to_physical(address);

        // Get the epde.
        auto & epde = this->epd[physical_address >> 30]
                               [(physical_address >> 21) & 0x1ff];

        // The ept physical address.
        auto ept_physical_address = epde.page_number() << 12;

        // Find the virtual address of the ept.
        auto ept = reinterpret_cast<arch::x86_64::vmx::epte *>(
            this->module_physical_to_virtual.find(ept_physical_address)
                ->second);

        // Make epte accessible.
        auto & epte = ept[(physical_address >> 12) & 0x1ff];
        epte.read(true);
        epte.write(true);
        epte.execute(true);
        epte.execute_user(true);
    }
}

void hypervisor::initialize_vmx()
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    // The VMX and VMCS regions.
    auto & vmx = this->vmx[this->next_virtual_processor - 1];
    auto & vmx_vmcs = this->vmx_vmcs[this->next_virtual_processor - 1];

    // Get the value of the basic VMX msr.
    const auto & basic_msr = this->cached_vmx_msr(vmx_msr::basic);

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
        this->cached_vmx_msr(vmx_msr::cr0_fixed_1) & 0xffffffff;
    this->host_cr0 |=
        this->cached_vmx_msr(vmx_msr::cr0_fixed_0) & 0xffffffff;

    // Adjust the cr4 according to the MSR restrictions.
    this->host_cr4 &=
        this->cached_vmx_msr(vmx_msr::cr4_fixed_1) & 0xffffffff;
    this->host_cr4 |=
        this->cached_vmx_msr(vmx_msr::cr4_fixed_0) & 0xffffffff;
}

std::expected<void, zpp::error> hypervisor::enable_vmx_in_feature_control()
{
    // Bit 0 locks the register, and bit 2 is what actually permits VMXON
    // outside SMX. VMXON raises a general protection fault unless the lock
    // bit is set and bit 2 with it.
    constexpr std::uint64_t lock = 1ull << 0;
    constexpr std::uint64_t vmxon_outside_smx = 1ull << 2;

    auto feature_control =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_feature_control);

    // Already unlocked by the firmware, so set both bits ourselves.
    // Writing the lock bit is required: leaving it clear keeps vmxon
    // faulting.
    if (!(feature_control & lock)) {
        arch::x86_64::wrmsr(arch::x86_64::msr::ia32_feature_control,
                            feature_control | lock | vmxon_outside_smx);
        return {};
    }

    // Locked with VMXON disallowed. The register is write-once per reset,
    // so nothing here can change it - the firmware has turned
    // virtualization off and only the firmware can turn it back on.
    if (!(feature_control & vmxon_outside_smx)) {
        return std::unexpected(
            zpp::error{error::vmx_disabled_by_firmware});
    }

    return {};
}

void hypervisor::emulate_init_signal(arch::x86_64::context & context)
{
    auto & vmcs = this->vmcs;

    using segment_descriptor = arch::x86_64::segment_descriptor;

    // The state a processor holds after an INIT. SDM Table 12-1, "IA-32
    // and Intel 64 Processor States Following Power-up, Reset, or INIT",
    // INIT column.
    constexpr std::uint64_t rflags_after_init = 0x2;
    constexpr std::uint64_t rip_after_init = 0xfff0;
    constexpr std::uint64_t dr7_after_init = 0x400;
    // SDM Table 12-1 gives 0x60000010 in the CR0 row's INIT column, but
    // footnote 2 on that row qualifies it: "The CD and NW flags are
    // unchanged, bit 4 is set to 1, all other bits are cleared." The
    // 0x60000010 in the table is the power-up value, where CD and NW
    // happen to be set - taking it literally for an INIT would disable
    // this processor's caches for the rest of its life. Read from the VMCS
    // rather than from a constant, since which of the two bits are set is
    // the guest's business.
    constexpr std::uint64_t preserved_across_init =
        arch::x86_64::cr0_bits::cache_disable |
        arch::x86_64::cr0_bits::not_write_through;
    auto cr0_after_init = arch::x86_64::cr0_bits::extension_type |
                          (vmcs.guest_cr0() & preserved_across_init);
    constexpr std::uint64_t cr4_after_init = 0;
    constexpr std::uint64_t code_selector_after_init = 0xf000;
    constexpr std::uint64_t code_base_after_init = 0xffff0000;
    constexpr std::uint64_t real_mode_segment_limit = 0xffff;
    constexpr std::uint64_t descriptor_table_limit_after_init = 0xffff;

    // The two bits VMX will not let a guest clear: CR0.NE and CR4.VMXE
    // are required to be set by IA32_VMX_CR0_FIXED0 and
    // IA32_VMX_CR4_FIXED0, and unrestricted guest exempts only PE and PG
    // - so a literally architectural CR0 and CR4 would fail VM entry.
    // The architectural values go into the read shadows, which is where a
    // guest would look once those bits are owned by the host.
    constexpr std::uint64_t cr0_never_clear =
        arch::x86_64::cr0_bits::numeric_error;
    constexpr std::uint64_t cr4_never_clear =
        arch::x86_64::cr4_bits::vmx_enable;

    // A real mode segment: sixteen bit, byte granular, limit 0xffff. The
    // access rights the VMCS wants are the descriptor's, so they are
    // built out of a descriptor rather than written as a number.
    auto real_mode_segment = [](segment_descriptor::segment_type type,
                                bool system) {
        segment_descriptor descriptor;
        descriptor.limit(real_mode_segment_limit);
        descriptor.base(0);
        descriptor.type(type);
        descriptor.system(system);
        descriptor.privilege_level(0);
        descriptor.present(true);
        descriptor.available_for_system_use(false);
        descriptor.code_64_bit(false);
        descriptor.default_operation_size(false);
        descriptor.granularity(false);
        return descriptor.vmx_access_rights();
    };

    auto code_access_rights = real_mode_segment(
        segment_descriptor::segment_type::code_execute_read_accessed,
        false);
    auto data_access_rights = real_mode_segment(
        segment_descriptor::segment_type::data_read_write_accessed, false);
    auto ldtr_access_rights =
        real_mode_segment(segment_descriptor::segment_type::ldt, true);
    auto tr_access_rights = real_mode_segment(
        segment_descriptor::segment_type::tss_busy, true);

    // Wait for the start-up IPI, written first rather than last. This is
    // the state that makes the processor startable again, and a start-up
    // IPI arriving before it is set is discarded rather than queued - so
    // the window between the INIT exit and this write is a window in which
    // the IPI that was supposed to start this processor is lost. Nothing
    // below it can fail, but the shorter that window the better.
    vmcs.guest_activity_state(
        arch::x86_64::vmx::activity_state::wait_for_start_up_ipi);

    // Real mode, based at the reset vector. An application processor
    // never runs an instruction here - the start-up IPI that follows
    // redirects it - but the firmware is entitled to see this state.
    vmcs.guest_cr0(cr0_after_init | cr0_never_clear);
    vmcs.cr0_read_shadow(cr0_after_init);
    vmcs.guest_cr3(0);
    vmcs.guest_cr4(cr4_after_init | cr4_never_clear);
    vmcs.cr4_read_shadow(cr4_after_init);

    // Long mode is gone with CR0.PG, and the entry control has to agree
    // or VM entry fails its consistency checks.
    //
    // The guest IA32_EFER field is deliberately not written. Without the
    // "load IA32_EFER" VM-entry control - which this VMCS does not set -
    // the field is ignored, and VM entry instead loads EFER.LMA from the
    // control cleared below and leaves LME alone when CR0.PG is being
    // loaded as zero, which it is here. Writing the field would look like
    // it cleared EFER when it does nothing at all. Setting the control is
    // not the answer either: the guest can then change EFER with a WRMSR
    // that the all-zero MSR bitmap does not intercept, leaving the field
    // stale and the next entry loading the wrong value.
    vmcs.vm_entry_controls(
        vmcs.vm_entry_controls() &
        ~arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);

    vmcs.guest_rflags(rflags_after_init);
    vmcs.guest_rip(rip_after_init);
    vmcs.guest_rsp(0);
    vmcs.guest_dr7(dr7_after_init);

    vmcs.guest_cs_selector(code_selector_after_init);
    vmcs.guest_cs_base(code_base_after_init);
    vmcs.guest_cs_limit(real_mode_segment_limit);
    vmcs.guest_cs_access_rights(code_access_rights);

    // Every data segment is sixteen bit, based at zero, selector zero.
    vmcs.guest_ss_selector(0);
    vmcs.guest_ss_base(0);
    vmcs.guest_ss_limit(real_mode_segment_limit);
    vmcs.guest_ss_access_rights(data_access_rights);

    vmcs.guest_ds_selector(0);
    vmcs.guest_ds_base(0);
    vmcs.guest_ds_limit(real_mode_segment_limit);
    vmcs.guest_ds_access_rights(data_access_rights);

    vmcs.guest_es_selector(0);
    vmcs.guest_es_base(0);
    vmcs.guest_es_limit(real_mode_segment_limit);
    vmcs.guest_es_access_rights(data_access_rights);

    vmcs.guest_fs_selector(0);
    vmcs.guest_fs_base(0);
    vmcs.guest_fs_limit(real_mode_segment_limit);
    vmcs.guest_fs_access_rights(data_access_rights);

    vmcs.guest_gs_selector(0);
    vmcs.guest_gs_base(0);
    vmcs.guest_gs_limit(real_mode_segment_limit);
    vmcs.guest_gs_access_rights(data_access_rights);

    vmcs.guest_ldtr_selector(0);
    vmcs.guest_ldtr_base(0);
    vmcs.guest_ldtr_limit(real_mode_segment_limit);
    vmcs.guest_ldtr_access_rights(ldtr_access_rights);

    vmcs.guest_tr_selector(0);
    vmcs.guest_tr_base(0);
    vmcs.guest_tr_limit(real_mode_segment_limit);
    vmcs.guest_tr_access_rights(tr_access_rights);

    vmcs.guest_gdtr_base(0);
    vmcs.guest_gdtr_limit(descriptor_table_limit_after_init);
    vmcs.guest_idtr_base(0);
    vmcs.guest_idtr_limit(descriptor_table_limit_after_init);

    // The general purpose registers are architecturally defined after an
    // INIT too - same table - and they are not in the VMCS - they live in
    // the context this VMM saved on the way in and restores on the way
    // out, so rewriting only the VMCS left the processor holding whatever
    // the firmware had in them. The same table gives EAX zero, EDX the
    // family, model and stepping, and the rest zero.
    std::uint32_t identification[4]{};
    arch::x86_64::cpuid(1, 0, identification);

    context.rax = 0;
    context.rbx = 0;
    context.rcx = 0;
    context.rdx = identification[0];
    context.rsp = 0;
    context.rbp = 0;
    context.rsi = 0;
    context.rdi = 0;
    context.r8 = 0;
    context.r9 = 0;
    context.r10 = 0;
    context.r11 = 0;
    context.r12 = 0;
    context.r13 = 0;
    context.r14 = 0;
    context.r15 = 0;

    // Nothing is blocked or pending across an INIT.
    //
    // The event injection fields matter as much as the blocking ones and
    // are easy to miss, because they are how *this* VMM asks for an event
    // rather than how the guest reports one - inject_general_protection_
    // fault below writes them. Left set, the next VM entry would deliver
    // that vector through the descriptor tables just reset above: in real
    // mode, the interrupt vector table at physical address zero. The guest
    // would leave for somewhere arbitrary during VM entry, after every
    // piece of state a debugger can see. another implementation zeroes the same fields as
    // part of its idea of a clean VMCS.
    vmcs.vm_entry_interruption_information_field(0);
    vmcs.vm_entry_exception_error_code(0);
    vmcs.guest_interruptibility_state(0);
    vmcs.guest_pending_debug_exceptions(0);

    // DR6 is not a VMCS guest field - the guest and host share the
    // register - so the architectural post-INIT value has to be written to
    // the real one while running on this processor, the same way the
    // general purpose registers do.
    constexpr std::uint64_t dr6_after_init = 0xffff0ff0;
    arch::x86_64::dr6(dr6_after_init);

    // This processor is no longer one that a start-up IPI has started.
    if (auto cpu = vmcs.vpid() - 1; cpu < max_cpus) {
        this->started_by_start_up_ipi[cpu] = false;
    }

    // SDM 12.1: during an INIT "the TLBs and BTB are invalidated as with
    // a hardware reset", and the same paragraph describes INIT as the
    // method for "switching from protected to real-address mode" - which
    // is exactly the transition just made above. With VPID enabled the
    // processor tags its cached translations and they survive that
    // transition, so they have to be invalidated by hand. Paging is off
    // now, making linear addresses equal guest-physical ones, and a stale
    // entry from the long mode context would translate them anyway.
    //
    // Single-context, which invalidates the linear and combined mappings
    // tagged with this VPID and leaves other processors alone.
    constexpr std::uint64_t invvpid_single_context = 1;

    struct alignas(0x10) invvpid_descriptor
    {
        std::uint64_t vpid{};
        std::uint64_t linear_address{};
    };

    invvpid_descriptor descriptor{vmcs.vpid(), 0};
    if (arch::x86_64::vmx::invvpid(
            reinterpret_cast<void *>(invvpid_single_context),
            &descriptor)) {
        log("invvpid failed on cpu {}", vmcs.vpid());
    }
}

void hypervisor::emulate_start_up_ipi(std::uint64_t vector)
{
    // SDM 28.2: "Start-up IPIs (SIPIs). SIPIs cause VM exits. If a logical
    // processor is not in the wait-for-SIPI activity state" the SIPI is
    // discarded - so getting this exit at all means the processor was
    // waiting, and leaving that state is the VMM's job rather than the
    // hardware's. Matches KVM's kvm_vcpu_deliver_sipi_vector(), which sets
    // the same three fields and nothing else.
    auto & vmcs = this->vmcs;

    constexpr std::uint64_t real_mode_segment_limit = 0xffff;

    // Shifts that turn the vector into a segment. The vector is a page
    // number, so the segment is the vector scaled by a page, and the
    // selector is the segment base shifted down by the four bits real
    // mode already implies.
    constexpr std::uint64_t vector_to_selector_shift = 8;
    constexpr std::uint64_t vector_to_base_shift = 12;

    // A second start-up IPI for a processor already started is ignored.
    // INIT-SIPI-SIPI sends two, and the second would otherwise send a
    // processor that is already running back to its entry point.
    auto cpu = vmcs.vpid() - 1;
    if (cpu < max_cpus) {
        if (this->started_by_start_up_ipi[cpu]) {
            return;
        }
        this->started_by_start_up_ipi[cpu] = true;
    }

    // Execution begins at the start of that page, in real mode.
    vmcs.guest_cs_selector(vector << vector_to_selector_shift);
    vmcs.guest_cs_base(vector << vector_to_base_shift);
    vmcs.guest_cs_limit(real_mode_segment_limit);
    vmcs.guest_rip(0);

    // Runnable again.
    vmcs.guest_activity_state(arch::x86_64::vmx::activity_state::active);
}

void hypervisor::record_exit(arch::x86_64::vmx::exit_reason reason)
{
    auto & vmcs = this->vmcs;

    // The VPID was assigned as the virtual processor number, counting from
    // one, so this is the CPU index. Guarded anyway: an out of range index
    // here would corrupt whatever follows the ring.
    auto cpu = vmcs.vpid() - 1;
    if (cpu >= max_cpus) {
        return;
    }

    auto & count = this->exit_trace_count[cpu];
    auto & entry = this->exit_trace[cpu][count % exit_trace_capacity];

    entry.reason = reason.value();
    entry.qualification = vmcs.exit_qualification();
    entry.activity_state = vmcs.guest_activity_state();
    entry.cs_selector = vmcs.guest_cs_selector();
    entry.rip = vmcs.guest_rip();

    ++count;
}

void hypervisor::inject_general_protection_fault()
{
    constexpr std::uint64_t general_protection_vector = 13;

    // A fault, so the guest resumes at the instruction that caused it
    // rather than past it - which is why the exit handler must not advance
    // RIP when this is used.
    this->vmcs.vm_entry_interruption_information_field(
        general_protection_vector |
        arch::x86_64::vmx::vm_entry_interruption::hardware_exception |
        arch::x86_64::vmx::vm_entry_interruption::deliver_error_code |
        arch::x86_64::vmx::vm_entry_interruption::valid);

    // Zero, which is what a general protection fault that is not a
    // segment violation pushes.
    this->vmcs.vm_entry_exception_error_code(0);
}

void hypervisor::on_unhandled_exit(arch::x86_64::vmx::exit_reason reason)
{
    auto & vmcs = this->vmcs;
    auto & record = this->unhandled_exit;

    // Capture while the VMCS is still current on this CPU.
    record.reason = reason.value();
    record.qualification = vmcs.exit_qualification();
    record.guest_linear_address = vmcs.guest_linear_address();
    record.guest_rip = vmcs.guest_rip();
    record.guest_cs_selector = vmcs.guest_cs_selector();

    // Written last, so a debugger that finds this set knows the rest of
    // the record is complete rather than half filled in.
    record.occurred = 1;

    log("stopping, unhandled exit reason {}", reason.value());

    // The exit ring already holds the run up to this, and it stays
    // readable because this CPU stops here rather than letting the guest
    // proceed on corrupted state and take the machine down elsewhere.
    for (;;) {
        arch::x86_64::disable_interrupts();
        arch::x86_64::halt();
    }
}

void hypervisor::on_vm_entry_failure(arch::x86_64::vmx::exit_reason reason)
{
    auto & vmcs = this->vmcs;
    auto & record = this->vm_entry_failure;

    // Capture while the VMCS is still current on this CPU - after the halt
    // below there is no way to read any of it.
    record.reason = reason.value();
    record.qualification = vmcs.exit_qualification();
    record.instruction_error = vmcs.vm_instruction_error();
    record.activity_state = vmcs.guest_activity_state();
    record.interruptibility_state = vmcs.guest_interruptibility_state();
    record.entry_controls = vmcs.vm_entry_controls();
    record.guest_cr0 = vmcs.guest_cr0();
    record.guest_cr4 = vmcs.guest_cr4();
    record.guest_rflags = vmcs.guest_rflags();
    record.guest_rip = vmcs.guest_rip();
    record.guest_cs_selector = vmcs.guest_cs_selector();
    record.guest_cs_base = vmcs.guest_cs_base();
    record.guest_cs_access_rights = vmcs.guest_cs_access_rights();

    // Written last, so a debugger that finds this set knows the rest of
    // the record is complete rather than half filled in.
    record.occurred = 1;

    log("stopping, vm entry failed, reason {}", reason.value());

    // Stop. This CPU is not going to run a guest again, and pretending
    // otherwise is what made this failure invisible before.
    for (;;) {
        arch::x86_64::disable_interrupts();
        arch::x86_64::halt();
    }
}

std::expected<void, zpp::error> hypervisor::enter_root_mode()
{
    // Backup cr0 and cr4.
    auto cr0 = arch::x86_64::cr0();
    auto cr4 = arch::x86_64::cr4();

    // Change cr0.
    arch::x86_64::cr0(this->host_cr0);
    scope_exit restore_cr0{[&] { arch::x86_64::cr0(cr0); }};

    // Change cr4.
    arch::x86_64::cr4(this->host_cr4);
    scope_exit restore_cr4{[&] { arch::x86_64::cr4(cr4); }};

    // Let VMXON through in IA32_FEATURE_CONTROL. Without this vmxon raises
    // a general protection fault rather than failing with the carry flag,
    // which is how the missing host IDT used to turn into a triple fault.
    // The MSR is per logical processor, so this belongs here rather than
    // in the once-per-boot setup.
    if (auto result = enable_vmx_in_feature_control(); !result) {
        return result;
    }

    // Turn on vmx.
    if (arch::x86_64::vmx::vmxon(&this->vmx_physical)) {
        return std::unexpected(zpp::error{error::vmxon_failed});
    }
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    // Clear the vmcs.
    if (arch::x86_64::vmx::vmclear(&this->vmcs_physical)) {
        return std::unexpected(zpp::error{error::vmclear_failed});
    }

    // Load the vmcs structure.
    if (arch::x86_64::vmx::vmptrld(&this->vmcs_physical)) {
        return std::unexpected(zpp::error{error::vmptrld_failed});
    }

    // Cancel all guards.
    turn_off_vmx.release();
    restore_cr4.release();
    restore_cr0.release();
    return {};
}

void hypervisor::setup_vmcs(arch::x86_64::context & guest_context)
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    auto & vmcs = this->vmcs;

    // Zero the VMX abort indicator, as the SDM recommends for any VMCS
    // this VMM uses. A VMX abort is a failure during a VM *exit*: it puts
    // the processor into a shutdown state that only RESET leaves, and it
    // is not a VM entry failure, so nothing else here would notice one.
    // The indicator names the cause - but only if it was known to be zero
    // beforehand, which is what this is for.
    this->vmx_vmcs[this->next_virtual_processor - 1].abort_indicator = 0;

    // Set invalid link pointer.
    vmcs.vmcs_link_pointer(0xffffffffffffffffull);

    // Set virtual processor id.
    vmcs.vpid(this->next_virtual_processor);

    // Setup the EPT pointer.
    arch::x86_64::vmx::ept_pointer eptp;
    eptp.memory_type(arch::x86_64::memory_type::write_back);
    eptp.page_walk_length(4);
    eptp.page_number(this->epml4_physical >> 12);
    vmcs.ept_pointer(eptp);

    // Set msr bitmap.
    vmcs.msr_bitmap(this->msr_bitmap_physical);

    // Secondary execution control.
    vmcs.secondary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::processor_based_contorls_2),
            arch::x86_64::vmx::vm_execution_controls::secondary::
                    enable_ept |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    enable_vpid |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    unrestricted_guest |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    enable_rdtscp |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    enable_invpcid |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    enable_xsaves_xrstors |
                arch::x86_64::vmx::vm_execution_controls::secondary::
                    mode_based_execute_control));

    // Pin based execution controls.
    vmcs.pin_based_vm_execution_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_pin_based_controls), 0));

    // Primary execution controls.
    vmcs.primary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_processor_based_controls),
            arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_secondary_controls |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_msr_bitmaps));

    // VM exit in 64 bit address space.
    vmcs.vm_exit_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_exit_controls),
        arch::x86_64::vmx::vm_exit_controls::host_address_space_size));

    // VM entry in 64 bit address space.
    vmcs.vm_entry_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_entry_controls),
        arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest));

    // Get the GDT base.
    auto intermediate_gdt_base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1]);

    // Write segment information.
    auto descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.cs);
    vmcs.guest_cs_selector(guest_context.cs);
    vmcs.guest_cs_limit(descriptor.effective_limit());
    vmcs.guest_cs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_cs_base(descriptor.context_dependent_base());
    vmcs.host_cs_selector(this->host_cs);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.ds);
    vmcs.guest_ds_selector(guest_context.ds);
    vmcs.guest_ds_limit(descriptor.effective_limit());
    vmcs.guest_ds_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ds_base(descriptor.context_dependent_base());
    vmcs.host_ds_selector(0);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.es);
    vmcs.guest_es_selector(guest_context.es);
    vmcs.guest_es_limit(descriptor.effective_limit());
    vmcs.guest_es_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_es_base(descriptor.context_dependent_base());
    vmcs.host_es_selector(0);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.fs);
    vmcs.guest_fs_selector(guest_context.fs);
    vmcs.guest_fs_limit(descriptor.effective_limit());
    vmcs.guest_fs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_fs_base(descriptor.context_dependent_base());
    vmcs.host_fs_base(reinterpret_cast<std::uint64_t>(this->fs_data));
    vmcs.host_fs_selector(0);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.gs);
    vmcs.guest_gs_selector(guest_context.gs);
    vmcs.guest_gs_limit(descriptor.effective_limit());
    vmcs.guest_gs_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_gs_base(this->ia32_gs_base);
    vmcs.host_gs_base(reinterpret_cast<std::uint64_t>(this->gs_data));
    vmcs.host_gs_selector(0);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, guest_context.ss);
    vmcs.guest_ss_selector(guest_context.ss);
    vmcs.guest_ss_limit(descriptor.effective_limit());
    vmcs.guest_ss_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ss_base(descriptor.context_dependent_base());
    vmcs.host_ss_selector(0);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, this->guest_tr);
    vmcs.guest_tr_selector(this->guest_tr);
    vmcs.guest_tr_limit(descriptor.effective_limit());
    vmcs.guest_tr_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_tr_base(descriptor.context_dependent_base());
    vmcs.host_tr_base(reinterpret_cast<std::uint64_t>(this->host_tss));
    vmcs.host_tr_selector(this->host_tr);

    descriptor = arch::x86_64::segment_descriptor::from_memory(
        intermediate_gdt_base, this->guest_ldtr);
    vmcs.guest_ldtr_selector(this->guest_ldtr);
    vmcs.guest_ldtr_limit(descriptor.effective_limit());
    vmcs.guest_ldtr_access_rights(descriptor.vmx_access_rights());
    vmcs.guest_ldtr_base(descriptor.context_dependent_base());

    // Set gdtr information.
    vmcs.guest_gdtr_limit(this->guest_gdt_limit);
    vmcs.guest_gdtr_base(
        reinterpret_cast<std::uint64_t>(this->guest_gdt_pointer));
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
void hypervisor::vm_launch(arch::x86_64::context & guest_context,
                           VmmCode && vmm_code)
{
    auto & vmcs = this->vmcs;

    // Allocate a small stack for the host VM exit.
    alignas(0x10) unsigned char host_vm_launch_stack[0x1500]{};

    // The host VM exit rsp.
    auto host_rsp = reinterpret_cast<std::uint64_t>(
        std::end(host_vm_launch_stack) -
        (2 * sizeof(arch::x86_64::context)));

    // Construct the guest context on the host stack.
    auto & local_guest_context =
        *::new (reinterpret_cast<void *>(host_rsp)) arch::x86_64::context;

    // Construct the host context on the host stack.
    auto & host_context = *::new (reinterpret_cast<void *>(
        host_rsp + sizeof(arch::x86_64::context))) arch::x86_64::context;

    // Write host rip.
    vmcs.host_rip(
        reinterpret_cast<std::uint64_t>(arch::x86_64::vm_exit_entry));

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
    arch::x86_64::capture_context(&host_context);

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
        reinterpret_cast<std::uint64_t>(arch::x86_64::vmx::vmlaunch);

    // Set rflags to host rflags, to leave interrupts disabled.
    guest_context.rflags = host_context.rflags;

    // Set return value of guest to success.
    guest_context.rax = 0;

    // Launch the VM.
    arch::x86_64::restore_context(&guest_context);
}

std::expected<void, zpp::error>
hypervisor::main(arch::x86_64::context & caller_context)
{
    // Fetch parameters.
    auto cpuid = caller_context.rdi;
    auto physical_to_virtual =
        reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(
            caller_context.rsi);

    // Save the interrupt flag rather than assuming it was set: the Linux
    // loader enters through an IPI handler, where interrupts are already
    // disabled and enabling them would be enabling interrupts inside an
    // interrupt handler.
    auto interrupts_were_enabled =
        (arch::x86_64::flags() & (1ull << 9)) != 0;

    arch::x86_64::disable_interrupts();

    // Guard to restore interrupts to how they were found.
    scope_exit restore_interrupts{[interrupts_were_enabled] {
        if (interrupts_were_enabled) {
            arch::x86_64::enable_interrupts();
        }
    }};

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
        if (auto result = initialize_module_physical_to_virtual();
            !result) {
            return result;
        }

        // Initialize host GDT.
        initialize_host_gdt();

        // Initialize host IDT, which needs the GDT above to exist.
        initialize_host_idt();
    }

    // Initialize intermediate GDT.
    initialize_intermediate_gdt();

    // Load intermediate GDT.
    load_intermediate_gdt();

    // Guard to restore GDT.
    scope_exit restore_gdt{[&] { load_os_gdt(); }};

    // Switch page tables.
    arch::x86_64::cr3(this->host_cr3);

    // Guard to restore cr3.
    scope_exit restore_cr3{[&] { arch::x86_64::cr3(this->guest_cr3); }};

    // The OS IDT lives in memory the host page table does not map, so with
    // the switch above done IDTR names pages that are no longer there.
    // Every exception would fault again while being delivered and escalate
    // to a triple fault, taking the machine down with no diagnosis. Load
    // the host IDT, which is inside the module and therefore mapped.
    load_host_idt();

    // Guard to restore the OS IDT.
    scope_exit restore_os_idt{[&] { load_os_idt(); }};

    // Arm the recovery point the host IDT handler unwinds to, using the
    // same trick as the VM exit path in vm_launch: capture_context returns
    // twice, and the second return is the exception. Atomic so the flag is
    // read from memory rather than from a register the unwind restored.
    std::atomic<bool> host_exception_occurred;
    host_exception_occurred = false;
    this->host_exception_recovery_flag = &host_exception_occurred;
    arch::x86_64::capture_context(&this->host_exception_recovery);

    // Arriving from an exception rather than from the capture. The details
    // are in host_exception and host_exception_cr2 for a debugger to read;
    // the caller only learns that a host exception happened.
    if (host_exception_occurred) {
        return std::unexpected(zpp::error{error::host_exception});
    }

    // Perform only on first CPU load.
    if (0 == cpuid) {
        // Initialize VMX MSRS.
        initialize_vmx_msrs();

        // Initialize MTRRS.
        initialize_mtrrs();

        // Initialize the EPT.
        initialize_ept();

        // Protect module.
        if (auto result = protect_module(); !result) {
            return result;
        }

        // Allow guest access to unprotected memory.
        unprotect_guest_memory();
    }

    // Initialize vmx.
    initialize_vmx();

    // Enter root mode.
    if (auto result = enter_root_mode(); !result) {
        return result;
    }

    // Guard to turn off vmx.
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    // Setup vmcs.
    setup_vmcs(caller_context);

    // Disarm the recovery point. It is only good while this frame is live,
    // and the launch below does not return - the guest resumes on the
    // caller's stack instead - so from here on a host exception has
    // nothing to unwind to and the handler stops the CPU rather than
    // jumping into a dead frame.
    this->host_exception_recovery_flag = nullptr;

    log("launching guest on virtual processor {}",
        this->next_virtual_processor);

    // Launch VM.
    vm_launch(caller_context, [&](auto & context) {
        using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
        auto & vmcs = this->vmcs;

        // Virtual processor id.
        auto vpid = vmcs.vpid();
        static_cast<void>(vpid);

        // The basic exit reason.
        basic_reason reason{};

        // Get the exit reason. No failure to handle: reading it cannot
        // fail while a VMCS is current, and it traps if it ever does -
        // which beats what used to happen here, a bare return that left
        // the guest un-resumed and said nothing about why.
        auto full_reason =
            arch::x86_64::vmx::exit_reason(vmcs.exit_reason());

        // A failed VM entry arrives here looking like an exit, so it has
        // to be separated out before anything treats it as one. Nothing
        // below applies to it: the guest did not run, the instruction
        // length field describes no instruction, and resuming would fail
        // the same way again.
        if (full_reason.entry_failure()) {
            on_vm_entry_failure(full_reason);
        }

        reason = full_reason.basic();

        // Get the guest RIP.
        context.rip = vmcs.guest_rip();

        // Whether the exit was caused by an instruction the guest should
        // be resumed past. Cleared by the handlers for which it is not.
        bool advance_rip = true;

        // What follows is the whole of what this VMM presents to its
        // guest, and every case in it is load bearing for booting
        // Windows. Each was found by Windows failing in a way that named
        // something else, so the reasoning is kept at each case rather
        // than left to be rediscovered:
        //
        // - cpuid hides VMX, or Hyper-V launches ahead of Windows and
        //   faults on its own vmxon.
        // - cpuid answers the entire hypervisor leaf range, not just the
        //   leaf holding the signature. Unanswered leaves fall through to
        //   whatever is underneath, which told Windows that Hyper-V was
        //   present.
        // - rdmsr and wrmsr fault rather than being skipped, so a guest
        //   is never handed a value it did not read.
        // - anything else stops the CPU instead of being resumed from,
        //   because resuming advances RIP past an instruction that never
        //   took effect.
        //
        // The shape of the mistake was the same every time: answering
        // part of an interface, or resuming as though an unhandled
        // instruction had worked. Both leave a guest that has been lied
        // to, and it fails later somewhere unrelated - the first of these
        // cost a long hunt through the boot chain for a boot
        // configuration problem that did not exist. When adding a case,
        // answer the whole of whatever it is, or fault.
        switch (reason) {
        case basic_reason::cpuid: {
            std::uint32_t cpuid_result[4]{};

            // Execute the cpuid instruction.
            arch::x86_64::cpuid(context.rax, context.rcx, cpuid_result);

            // The leaf, which is EAX. The high half of RAX is not part
            // of it and real CPUID ignores it.
            auto leaf = static_cast<std::uint32_t>(context.rax);

            // The range reserved for hypervisor use. Nothing physical
            // answers here, so whatever a guest reads is whatever the
            // layer above it chose to say.
            constexpr std::uint32_t hypervisor_leaf_first = 0x40000000;
            constexpr std::uint32_t hypervisor_leaf_last = 0x4fffffff;

            // If needs to set hypervisor present bit.
            if (1 == leaf) {
                // Set hypervisor present bit.
                cpuid_result[2] |= (1 << 31);

                // Hide VMX. We hold VMX root mode and do not support
                // nesting, so a guest hypervisor would #GP on its own
                // vmxon and take the boot down with it. Reporting no VMX
                // makes it stand down instead: Hyper-V, which launches
                // ahead of Windows whenever VBS is on, hands straight
                // off to the OS. Remove this once nesting exists.
                cpuid_result[2] &= ~(1u << 5);
            } else if ((leaf >= hypervisor_leaf_first) &&
                       (leaf <= hypervisor_leaf_last)) {
                // Answer the whole hypervisor range, not just the leaf
                // carrying the signature. Anything left unanswered falls
                // through to whatever is underneath, and underneath is
                // not nothing: a guest of another hypervisor sees that
                // one's leaves, and this test rig runs QEMU with
                // hv-passthrough, which exposes a full set of Hyper-V
                // enlightenments.
                //
                // Answering one leaf out of that range produces a guest
                // that believes contradictory things - the vendor below
                // said Zpp, the interface and feature leaves above said
                // Hyper-V - and Windows acts on the more specific claim.
                // It then uses the Hyper-V synthetic MSRs, which this
                // implements no more than it implements the interface.
                if (hypervisor_leaf_first == leaf) {
                    // The highest leaf answered here. Just this one:
                    // there is a signature to report and no interface
                    // behind it.
                    cpuid_result[0] = hypervisor_leaf_first;

                    // HyperVisor Name: ZppZppZppZpp.
                    cpuid_result[1] = 0x5a70705a;
                    cpuid_result[2] = 0x705a7070;
                    cpuid_result[3] = 0x70705a70;
                } else {
                    // No interface, no features, nothing to enlighten
                    // anyone about.
                    cpuid_result[0] = 0;
                    cpuid_result[1] = 0;
                    cpuid_result[2] = 0;
                    cpuid_result[3] = 0;
                }
            }

            // Place the cpuid result into the context.
            context.rax = cpuid_result[0];
            context.rbx = cpuid_result[1];
            context.rcx = cpuid_result[2];
            context.rdx = cpuid_result[3];
            break;
        }
        case basic_reason::xsetbv: {
            // Activate CR4 xsave bit.
            auto cr4 = arch::x86_64::cr4();
            if (!(cr4 & arch::x86_64::cr4_bits::os_xsave)) {
                arch::x86_64::cr4(cr4 | arch::x86_64::cr4_bits::os_xsave);
            }

            // Execute the xsetbv instruction.
            arch::x86_64::xsetbv(context.rcx,
                                 context.rax | (context.rdx << 32));
            break;
        }
        case basic_reason::rdmsr:
        case basic_reason::wrmsr: {
            // SDM 28.1.3 lists, among the reasons RDMSR causes a VM exit,
            // that "the MSR address is not in the ranges 00000000H -
            // 00001FFFH and C0000000H - C0001FFFH". Accesses inside those
            // ranges are governed by the bitmap, which is all zeroes, so
            // they never exit. Outside them the access exits
            // unconditionally and no bitmap can stop it.
            //
            // Nothing outside those ranges is a real MSR on this
            // architecture. What lives there is the synthetic MSR space a
            // paravirtual interface would implement - Windows reads
            // 0x40000022, the Hyper-V timer frequency, having been told by
            // CPUID that some hypervisor is present. This one implements
            // no such interface, so the honest answer is the one bare
            // hardware gives for an MSR that does not exist: a general
            // protection fault.
            //
            // Getting this wrong is expensive and quiet. Resuming past the
            // instruction instead leaves the guest believing it read a
            // value, and Windows fails a long way from here with
            // 0xc000000d, blaming its own boot configuration.
            inject_general_protection_fault();

            // The MSR index, which is the one thing needed to tell an
            // absent architectural MSR from a synthetic one a guest was
            // invited to ask for. Finding this out the first time took a
            // debugger and a breakpoint.
            log("general protection fault on {} of msr {}",
                (basic_reason::rdmsr == reason) ? "rdmsr" : "wrmsr",
                context.rcx);

            // The fault is reported at the faulting instruction, so RIP
            // stays where it is.
            advance_rip = false;
            break;
        }
        case basic_reason::invd: {
            // Execute the invd instruction.
            arch::x86_64::invd();
            break;
        }
        case basic_reason::init_signal: {
            // SDM 28.2: "INIT signals cause VM exits. A logical
            // processor performs none of the operations normally
            // associated with these events." So the hardware tells us and
            // does nothing else - this is the only thing standing between
            // an application processor and never waking again.
            // Deliberately not logged. This runs in the window between
            // the INIT exit and the resume, during which a start-up IPI
            // for this processor is discarded rather than queued, and
            // logging allocates and takes a lock. record_exit below
            // captures the exit anyway.
            emulate_init_signal(context);
            advance_rip = false;
            break;
        }
        case basic_reason::start_up_ipi: {
            // The vector is the low byte of the exit qualification.
            constexpr std::uint64_t sipi_vector_mask = 0xff;
            auto vector = vmcs.exit_qualification() & sipi_vector_mask;
            emulate_start_up_ipi(vector);
            advance_rip = false;
            break;
        }
        default: {
            // Everything reaching here exits unconditionally - there is
            // no VM execution control that turns it off - so arriving
            // means this VMM was asked something it does not implement.
            // Recorded and stopped on rather than resumed from, because
            // the resume below would advance RIP past an instruction that
            // never took effect.
            record_exit(full_reason);
            on_unhandled_exit(full_reason);
        }
        }

        // Update RIP, unless nothing was executed. For an INIT signal or
        // a start-up IPI the instruction length field holds nothing
        // meaningful, and both handlers have already put RIP where the
        // processor is meant to resume - adding to it would land the
        // guest a few bytes into its own entry point.
        if (advance_rip) {
            context.rip += vmcs.vm_exit_instruction_length();
            vmcs.guest_rip(context.rip);
        }

        // Record what is about to be resumed, now that the handlers have
        // had their say.
        record_exit(full_reason);

        // Resume the VM.
        context.rip =
            reinterpret_cast<std::uint64_t>(arch::x86_64::vmx::vmresume);

        // Restore VM.
        arch::x86_64::restore_context(&context);
    });

    return {};
}

void hypervisor::launch_on_cpu_private_stack(
    hypervisor & hypervisor, arch::x86_64::context & caller_context)
{
    // Invoke the main function.
    auto result = hypervisor.main(caller_context);

    // Use result as return value.
    caller_context.rax = result ? 0 : result.error().code();

    // On success the hypervisor stays resident and its globals must
    // outlive every guest, so only tear them down when it does not.
    if (!result) {
        zpp::crt::init::cleanup();
    }

    // Restore context to caller.
    arch::x86_64::restore_context(&caller_context);
}

void hypervisor::launch_on_cpu(arch::x86_64::context & caller_context)
{
    // Fetch the stack the hypervisor will launch with.
    auto & stack = this->stack[this->available_stack_index];

    // Compute the stack top.
    auto stack_top = stack + sizeof(stack) - sizeof(arch::x86_64::context);

    // Copy construct caller context into the new stack top.
    auto copied_caller_context =
        ::new (stack_top) arch::x86_64::context(caller_context);

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
    arch::x86_64::restore_context(&launch_context);
}

} // namespace zpp::hypervisor

/**
 * The handler every host IDT entry stub funnels into. Declared by
 * zpp/arch/x86_64/exception_entry.h, which the x64 layer uses without
 * knowing what implements it.
 */
extern "C" void
zpp_x86_64_exception(zpp::arch::x86_64::exception_frame * frame)
{
    zpp::hypervisor::hypervisor::instance().on_host_exception(*frame);
}
