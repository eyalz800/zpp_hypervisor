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
#include "zpp/diag/log.h"
#include "zpp/diag/pump.h"
#include "zpp/diag/sinks.h"
#include "zpp/diag/sinks/esp_blocks.h"
#include "zpp/elf_file.h"
#include "zpp/elf_image_base.h"
#include "zpp/error.h"
#include "zpp/loader.h"
#include "zpp/nvme/command.h"
#include "zpp/scope_exit.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
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
    // Everything this function reads is read for the same reason: the
    // processor is about to become a guest, and it has to come back up
    // running exactly what it was running before. There is no other
    // record of that state anywhere - it is only in the registers, and
    // the VM entry is what overwrites them - so it is captured here,
    // once, on each processor, and copied into the guest half of the
    // VMCS later. The names say "guest" already, because that is what
    // this state becomes.
    this->guest_cr0 = arch::x86_64::cr0();
    this->guest_cr3 = arch::x86_64::cr3();
    this->guest_cr4 = arch::x86_64::cr4();
    this->guest_dr7 = arch::x86_64::dr7();

    this->ia32_debug_control =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_debug_control);

    // The segment bases that are not in the segment registers. In long
    // mode FS and GS carry a full sixty four bit base that a selector
    // cannot express, so it lives in these model specific registers
    // instead - and an operating system keeps real things there, GS
    // being where per processor state is usually reached from. Losing
    // either one across the entry would not be subtle.
    this->ia32_fs_base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_fs_base);
    this->ia32_gs_base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_gs_base);

    // The descriptor tables, both of which are read through the store
    // instruction's own layout - a limit and a base packed together -
    // and split into the two fields the VMCS wants them in.
    arch::x86_64::gdt_layout sgdt_layout{};
    arch::x86_64::sgdt(sgdt_layout.data());
    this->gdtr.limit = sgdt_layout.limit;
    this->gdtr.base = sgdt_layout.base;

    arch::x86_64::idt_layout sidt_layout{};
    arch::x86_64::sidt(sidt_layout.data());
    this->idtr.limit = sidt_layout.limit;
    this->idtr.base = sidt_layout.base;

    // The two selectors that are not general segment registers. The task
    // register in particular is needed twice over: to give back, and
    // because entering root mode needs a task state segment of its own,
    // so this VMM has to know whether the one it ends up loading is the
    // one that was already there - see initialize_intermediate_gdt.
    arch::x86_64::sldt(&this->guest_ldtr);
    arch::x86_64::str(&this->os_tr);
}

void hypervisor::initialize_module_region()
{
    // Where this module begins and how far it runs.
    //
    // Both are worked out from the image itself rather than supplied,
    // because nothing outside is in a position to say: the loader chose
    // the address, but this is position independent code and the only
    // authority on its own extent is its own program headers.
    //
    // The base is found by searching for it - take the address of
    // something known to be inside the module, round down to a page, and
    // walk backwards a page at a time until the ELF magic appears, which
    // it does exactly once, at the header. See elf_image_base.h, where
    // the pad byte in front of the search key is deliberate: it leaves
    // the key itself unaligned so the search cannot stop on it.
    //
    // The size is the *memory* size, not the file size. They differ by
    // .bss, which is most of this module - the per-processor stacks
    // alone are megabytes - and using the smaller of the two would leave
    // the VMM's own stacks outside every range derived from here: the
    // host mapping just below, and the protection that hides the module
    // from the guest.
    this->module_base = elf_image_base();
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
    // Map the host page table into itself.
    //
    // A page table is walked by the processor through physical
    // addresses, so it does not need to be mapped for paging to work.
    // It needs to be mapped for *this VMM* to keep editing it: every
    // change after this point - a watch armed, a page protected - is a
    // store through a virtual address, and once this processor is on
    // this table the only virtual addresses that exist are the ones it
    // maps. A table that does not map itself is one that can never be
    // changed again.
    //
    // Done through the OS table because that is the one still in force
    // here, and it is what can still turn the table's own address into
    // the physical address to install.
    this->host_page_table.map_self(this->os_page_table);

    // Map the module.
    //
    // Writable and executable as well as readable, because this is the
    // VMM's own image: it executes from here, and it writes to its own
    // data - which lives in the same mapped region, the module being
    // mapped as one range rather than per section.
    this->host_page_table.map_from(
        this->module_base,
        this->module_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write |
            arch::x86_64::page_table::protection::execute,
        this->os_page_table);

    // Map the local APIC page.
    //
    // The xAPIC form of the interrupt command is a store to a page
    // rather than a write to a model specific register, so the handler
    // that watches it reads the command back out of that page - through
    // its physical address, used directly as a host virtual one. That
    // resolves only if this table maps it, and by default this table
    // maps itself and the module and nothing else at all. Without this
    // the read faults: #PF, error code zero, CR2 at the command
    // register, taken in the exit handler where there is no recovery
    // point left to unwind to, so the processor simply stops.
    //
    // Mapped here rather than where it is armed, because arming happens
    // after this processor has already switched to this table, and
    // map_from walks the OS table through the loader's callback - which
    // is one of the outside addresses that stops resolving at that
    // switch.
    //
    // Unconditional, and only one page. A guest in x2APIC mode never
    // takes the path that reads it, but establishing the mapping costs a
    // single entry and removes the ordering question entirely.
    constexpr std::uint64_t apic_base_mask = 0xffffff000ull;
    auto apic_base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) &
        apic_base_mask;
    this->host_page_table.map_from(
        apic_base,
        page_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write,
        this->os_page_table);

    // Compose the value that will be loaded into CR3.
    //
    // The top of it is the physical address of this table's top level,
    // which is what the register actually selects. The low twelve bits
    // are not part of that address and are carried over from whatever
    // the OS was already running with, because what they mean depends on
    // a mode this VMM does not control: with CR4.PCIDE clear they are
    // the page level cache attributes PWT and PCD, and with it set the
    // whole field is the process context identifier. Copying them keeps
    // this table on the same terms as the one it replaces instead of
    // silently choosing zero for both readings.
    this->host_cr3 = this->host_page_table.virtual_to_physical(
                         &this->host_page_table.head()) |
                     (this->guest_cr3 & 0xfff);
}

std::expected<void, zpp::error>
hypervisor::initialize_module_physical_to_virtual()
{
    // Build the reverse of the module's own mapping, once, up front.
    //
    // Going virtual to physical is a page table walk and can be done at
    // any time. Going the other way cannot: nothing in the hardware
    // answers "which virtual address is this physical page at", and the
    // walk that would answer it is a search. So the answers are worked
    // out here, while walking is still cheap and legal, and looked up
    // afterwards.
    //
    // Afterwards is the reason for the up front part. The lookups happen
    // from inside exits, where a page table walk would mean touching the
    // guest's tables from the host, and where allocating is not an
    // option - which is also why this is a fixed capacity that can be
    // exceeded and is refused rather than grown.
    auto number_of_pages = this->module_size / page_size;

    if (number_of_pages >= this->module_physical_to_virtual.capacity()) {
        return std::unexpected(
            zpp::error{error::physical_to_virtual_capacity_error});
    }

    auto module_base = reinterpret_cast<std::uintptr_t>(this->module_base);

    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto address = module_base + (page_size * i);

        // Through the host table rather than the OS one, because that is
        // the mapping these addresses will be reached through once this
        // processor has switched, and it is the physical page behind
        // *that* which the reverse lookup has to name.
        auto physical_address =
            this->host_page_table.virtual_to_physical(address);

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
    // Out of unprotected_memory, because protect_module makes the rest
    // of this module not-present to the guest and the guest goes on
    // running with these as its own descriptor tables.
    auto & intermediate_gdt =
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1];

    auto & guest_tss = this->unprotected_memory
                           .guest_tss[this->next_virtual_processor - 1];

    // Copied rather than pointed at, because the host page table does
    // not map the OS table and main switches onto it a few lines after
    // calling here.
    std::memcpy(intermediate_gdt,
                reinterpret_cast<const char *>(this->gdtr.base),
                this->gdtr.limit + 1);

    // A present task segment is all the guest needs, so it keeps the
    // table it was found with. The branch below exists because VM entry
    // rejects an unusable guest TR - SDM 29.3.1.2 - and UEFI leaves TR
    // null.
    if (auto task_state_segment =
            arch::x86_64::segment_descriptor::from_memory(
                reinterpret_cast<std::uint64_t>(intermediate_gdt),
                this->os_tr);
        task_state_segment.present()) {
        this->guest_gdt_pointer =
            reinterpret_cast<std::uint64_t *>(this->gdtr.base);
        this->guest_gdt_limit = this->gdtr.limit;
        this->intermediate_gdt_limit = this->gdtr.limit;
        this->guest_tr = this->os_tr;
        return;
    }

    // The guest is pointed at the extended copy, since appending the new
    // descriptor to the firmware's own table would write past its limit.
    this->guest_gdt_pointer =
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1];

    // Sixteen bytes, not eight: in IA-32e mode a TSS descriptor occupies
    // the space of two entries, SDM 3.5.2.
    this->intermediate_gdt_limit =
        this->gdtr.limit + (2 * sizeof(std::uint64_t));
    this->guest_gdt_limit = this->intermediate_gdt_limit;

    // One slot past the last descriptor the OS had, so nothing the guest
    // already refers to is overwritten.
    auto tr_index = (this->gdtr.limit + 1) / sizeof(std::uint64_t);

    // Available rather than busy because LTR faults on anything else and
    // marks it busy itself; byte granular because SDM 29.3.1.2 requires
    // G clear for a TR limit whose low twelve bits are not all ones.
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

    // Two slots for the sixteen byte form. The selector still scales by
    // eight - a table is indexed in eight byte units however many of
    // them a descriptor spans.
    intermediate_gdt[tr_index] = task_state_segment.basic_value();
    intermediate_gdt[tr_index + 1] = task_state_segment.extended_value();
    this->guest_tr = tr_index << 3;
}

void hypervisor::load_intermediate_gdt()
{
    arch::x86_64::gdt_layout lgdt_layout{};
    lgdt_layout.base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1]);
    lgdt_layout.limit = this->intermediate_gdt_limit;
    arch::x86_64::lgdt(lgdt_layout.data());

    // Only for a synthesized segment. The OS's own is already marked
    // busy by the OS's LTR, and LTR faults on a busy descriptor.
    if (this->guest_tr != this->os_tr) {
        arch::x86_64::ltr(&this->guest_tr);
    }
}

void hypervisor::load_os_gdt()
{
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
    auto & mtrrs = this->mtrrs;

    // First, because the reads below are gated on it: whether the
    // fixed-range registers exist, and how many variable ones there are.
    mtrrs.capabilities = arch::x86_64::mtrr_capabilities(
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_mtrr_capability));

    // Read IA32_MTRR_DEF_TYPE, which carries three things nothing here
    // used to see: the memory type every range no MTRR covers takes, the
    // fixed-range enable, and the global MTRR enable. Firmware normally
    // sets the default type to UC, so assuming write-back for an uncovered
    // range - which is what this did before - marked the whole MMIO hole
    // above the top of DRAM cacheable.
    mtrrs.default_type = arch::x86_64::mtrr_default_type(
        arch::x86_64::rdmsr(arch::x86_64::msr::mtrr::default_type));

    // Read the fixed-range registers, but only where the processor says
    // they exist: an MSR a processor does not implement raises #GP on the
    // read, and there is no handler for one here.
    if (mtrrs.capabilities.fixed_range_registers_supported()) {
        for (std::size_t i{}; i < mtrrs.fixed_range_count; ++i) {
            mtrrs.fixed[i] = arch::x86_64::rdmsr(
                arch::x86_64::mtrr_fixed_ranges[i].msr);
        }
    }

    // The MTRR variable count, bounded by the array it fills. The bound is
    // defensive rather than expected: the array is sized to the field's
    // architectural maximum, so a processor cannot report more than fits.
    // It stays because the alternative, if that ever stopped being true,
    // is silently writing over whatever follows the array - which is
    // exactly what happened when this was sized at 8 and the processor
    // said 10.
    mtrrs.variable_count = std::min<std::size_t>(
        std::size(mtrrs.variable),
        mtrrs.capabilities.variable_range_register_count());

    // Base and mask alternate in MSR space - 0x200, 0x201, then the next
    // pair two higher - which is where the stride of two comes from.
    for (std::size_t i{}; i < mtrrs.variable_count; ++i) {
        auto mtrr_base =
            arch::x86_64::mtrr_variable_base(arch::x86_64::rdmsr(
                arch::x86_64::msr::mtrr::physbase_0 + i * 2));

        auto mtrr_mask =
            arch::x86_64::mtrr_variable_mask(arch::x86_64::rdmsr(
                arch::x86_64::msr::mtrr::physmask_0 + i * 2));

        mtrrs.variable[i] = arch::x86_64::make_mtrr(mtrr_base, mtrr_mask);
    }

    // The state every EPT memory type below is derived from, recorded
    // because there is no other way to see it once the guest is running -
    // and because the type this VMM hands the guest is the only one it
    // gets. Reading it back off a failed boot is how a wrong default type
    // or a missed fixed range would be identified.
    log("mtrr cap {}, def type {}, enabled {}, fixed in use {}, "
        "default {}, variable {}",
        mtrrs.capabilities.value(),
        mtrrs.default_type.value(),
        mtrrs.default_type.enabled(),
        mtrrs.fixed_ranges_in_use(),
        mtrrs.default_type.type(),
        mtrrs.variable_count);
}

std::expected<void, zpp::error> hypervisor::initialize_ept()
{
    // One entry only. It covers the 512 GB the identity map below
    // describes; above that the entries stay not-present, so an access
    // faults rather than resolving to unrelated memory.
    this->epml4->read(true);
    this->epml4->write(true);
    this->epml4->execute(true);
    this->epml4->execute_user(true);
    this->epml4->page_number(
        this->host_page_table.virtual_to_physical(&this->epdpt) >> 12);

    // A template for the loop below. Both execute bits are needed
    // because mode-based execute control is on, which makes bit 10 a
    // separate user-mode execute permission - SDM Table 31-4.
    arch::x86_64::vmx::epte rwx_pdpte;
    rwx_pdpte.read(true);
    rwx_pdpte.write(true);
    rwx_pdpte.execute(true);
    rwx_pdpte.execute_user(true);

    // One page directory per gigabyte, all built up front: nothing fills
    // an EPT entry on demand, so the map must be complete before entry.
    for (std::size_t i{}; i < std::size(this->epdpt); ++i) {
        this->epdpt[i] = rwx_pdpte;
        this->epdpt[i].page_number(
            this->host_page_table.virtual_to_physical(this->epd[i]) >> 12);
    }

    // The same one level down, with the large bit set so the entry maps
    // two megabytes directly instead of naming a table.
    arch::x86_64::vmx::epte rwx_pde;
    rwx_pde.read(true);
    rwx_pde.write(true);
    rwx_pde.execute(true);
    rwx_pde.execute_user(true);
    rwx_pde.large(true);

    // Fill a temporary RWX 4 KB epte, for the regions a large page cannot
    // describe.
    arch::x86_64::vmx::epte rwx_pte;
    rwx_pte.read(true);
    rwx_pte.write(true);
    rwx_pte.execute(true);
    rwx_pte.execute_user(true);

    // How many 4 KB entries one page table holds, which is also how many a
    // large page covers.
    constexpr std::size_t entries_per_table = 512;

    // Large pages throughout, split to 4 KB only where the MTRRs
    // disagree within a region: mapping all 512 GB at 4 KB would want
    // 262144 tables and the pool holds 1024.
    std::size_t large_page_number{};
    for (std::size_t i{}; i < std::size(this->epd); ++i) {
        for (std::size_t j{}; j < std::size(*this->epd); ++j) {
            // An identity map, so this is the host physical address too,
            // which is what the MTRR lookup below answers about.
            auto physical_address = (large_page_number << 21);

            auto & epde = this->epd[i][j];
            epde = rwx_pde;
            epde.large_page_number(large_page_number);

            ++large_page_number;

            // The memory type the MTRRs give this whole 2 MB region, when
            // they give it one. Uncovered ranges come out as the default
            // type from IA32_MTRR_DEF_TYPE, which is what firmware sets to
            // UC and what makes MMIO above the top of DRAM uncacheable.
            if (auto type = this->mtrrs.uniform_type_of(physical_address,
                                                        large_page_size)) {
                epde.type(*type);
                continue;
            }

            // The region straddles a change of memory type, so no single
            // type describes it. Split it into 4 KB entries and give each
            // its own.
            //
            // The alternative to splitting is one conservative type for
            // the whole region, and the only safe conservative type is
            // uncacheable - anything cacheable over a device range is the
            // defect this derivation exists to fix. That would make the
            // 2 MB containing the legacy 0xa0000 aperture uncacheable, and
            // the first 2 MB of physical memory is real DRAM that a guest
            // runs code out of. Splitting costs one 4 KB table per mixed
            // region out of a pool that is statically allocated either
            // way, so it costs nothing that is not already spent.
            //
            // How many mixed regions there can be is bounded, so the pool
            // cannot be exhausted by this in practice. Each valid variable
            // MTRR contributes at most two boundaries, each boundary makes
            // at most one region mixed, and every fixed range lives inside
            // the first 2 MB - so at most 2 * VCNT + 1 regions. VCNT is at
            // most 255, giving 511, and protect_module takes at most one
            // more per 2 MB of a module capped at max_module_size, which
            // is
            // 51. That is 562 of the 1024 tables the pool holds. VCNT is
            // 10 on the machine this was written for, where one region is
            // mixed: the first, holding the legacy 0xa0000 aperture.
            if (this->next_ept_table >= std::size(this->ept)) {
                return std::unexpected(
                    zpp::error{error::out_of_ept_entries});
            }

            auto & ept = this->ept[this->next_ept_table++];
            for (std::size_t k{}; k < entries_per_table; ++k) {
                auto & epte = ept[k];
                epte = rwx_pte;
                epte.page_number((physical_address >> 12) + k);
                epte.type(this->mtrrs.type_of(physical_address +
                                              (k * page_size)));
            }

            // Point the epde at the table. Clearing the memory type is
            // required rather than tidy: in an entry that references a
            // page table the field is not ignored. SDM Vol. 3C
            // Table 31-6, "Format of an EPT Page-Directory Entry (PDE)
            // that References an EPT Page Table", bits "6:3 Reserved (must
            // be 0)" - and a reserved value set here is an EPT
            // misconfiguration, not a wrong memory type.
            epde.large(false);
            epde.type({});
            epde.page_number(
                this->host_page_table.virtual_to_physical(ept) >> 12);
        }
    }

    // Last, so that a build which failed part way through leaves the
    // flag clear and epte_for keeps refusing rather than handing out
    // pointers into tables that were never finished.
    this->ept_initialized = true;

    log("ept built, {} mixed regions split to 4 kb", this->next_ept_table);
    return {};
}

/**
 * Flushes cached translations after an EPT entry has been changed.
 *
 * Every modification below this line happens **after launch**, which is a
 * situation this VMM did not previously have: the EPT was built once and
 * never touched, so `invept` had no call sites at all and BACKLOG.md item
 * 4 recorded it as harmless for exactly that reason. Arming a page watch
 * changed that. A permission written into an entry that hardware still
 * has cached in its combined mappings is a permission that does not take
 * effect, so a protected page would go on being written without faulting.
 *
 * Single context rather than global: only this EPTP's translations are
 * stale, and SDM 30.3 gives type 1 as the single-context form. The
 * descriptor is the EPTP followed by a reserved zero quadword.
 *
 * **This invalidates on the calling processor only.** INVEPT is not a
 * broadcast, so another processor may still hold the old translation and
 * miss a watch that this one has just armed. That is the same limitation
 * the watch already documents from the other direction, it needs a
 * rendezvous to fix - BACKLOG.md item 10 - and it is safe in the
 * direction that matters here: a stale *permissive* entry means a missed
 * observation, never a wrong one.
 */
void hypervisor::invalidate_ept()
{
    // Announce the change before making it locally, so a processor that
    // reads the counter after this point cannot conclude it is up to
    // date when it is not.
    this->ept_generation.fetch_add(1, std::memory_order_release);
    invalidate_ept_locally();
}

void hypervisor::invalidate_ept_locally()
{
    // INVEPT is only defined in VMX root operation. Outside it the
    // instruction is not merely ineffective, it raises #UD - which is
    // how this was found: a fault at a small offset from the module
    // base, from the setup path, long before vmxon.
    //
    // Some of the callers below genuinely do run before root mode is
    // entered: the module is protected and the local APIC page is armed
    // while the extended page tables are still being built. Those need
    // no invalidation at all, because the processor is not yet in VMX
    // operation and therefore holds no cached translation for an EPTP
    // it has never loaded. So the correct behaviour there is to do
    // nothing, not to fault.
    //
    // CR4.VMXE answers "am I in VMX operation" without any new state to
    // keep in step. The bit is required to be set to execute vmxon and
    // may not be cleared while in VMX operation, so clear means
    // certainly outside it. Nothing else in this tree writes the bit -
    // the guest's view of it is a read shadow, not the register.
    constexpr std::uint64_t cr4_vmxe = 1ull << 13;
    if (!(arch::x86_64::cr4() & cr4_vmxe)) {
        return;
    }

    arch::x86_64::vmx::ept_pointer eptp;
    eptp.memory_type(arch::x86_64::memory_type::write_back);
    eptp.page_walk_length(4);
    eptp.page_number(this->epml4_physical >> 12);

    struct alignas(16) descriptor
    {
        std::uint64_t eptp;
        std::uint64_t reserved;
    } operand{eptp.value(), 0};

    constexpr std::uint64_t single_context = 1;
    auto type = single_context;

    if (0 != arch::x86_64::vmx::invept(&type, &operand)) {
        // Nothing useful to do with a failure here - the caller has
        // already changed the entry - but it must not pass silently,
        // because the symptom is a watch that never fires.
        log("invept failed after an ept change");
    }
}

std::expected<arch::x86_64::vmx::epte *, zpp::error>
hypervisor::epte_for(std::uint64_t physical_address)
{
    // Before initialize_ept there is nothing here to hand out, and
    // handing out a pointer into unbuilt tables would let a caller
    // "protect" a page by writing into memory the processor will never
    // read. Refused rather than allowed to look like it worked.
    if (!this->ept_initialized) {
        return std::unexpected(zpp::error{error::ept_not_initialized});
    }

    auto ept_count = std::size(this->ept);
    auto & host_page_table = this->host_page_table;

    // Indexed rather than walked, which initialize_ept's complete
    // identity map is what makes possible: bits 38:30 pick the page
    // directory and bits 29:21 the entry in it.
    auto & epde = this->epd[physical_address >> 30]
                           [(physical_address >> 21) & 0x1ff];

    // Already split, so the entry exists and only has to be found.
    if (!epde.large()) {
        // An EPT entry names its table by physical address, and the
        // module is not identity mapped in the host page table - hence
        // the reverse map rather than a dereference.
        auto ept_physical_address = epde.page_number() << 12;

        auto ept = reinterpret_cast<arch::x86_64::vmx::epte *>(
            this->module_physical_to_virtual.find(ept_physical_address)
                ->second);

        return &ept[(physical_address >> 12) & 0x1ff];
    }

    // Convert large epde into ept table. The pool index is a member
    // rather than a local because initialize_ept draws from the same
    // pool for any 2 MB region whose MTRR coverage is not of one type,
    // and it has already run by the time this does.
    //
    // Checked before the table is used rather than after. The old form
    // incremented first and compared the incremented value, which
    // reported exhaustion on the last table in the pool even though it
    // had just been filled and installed successfully. It never wrote
    // out of bounds, so this is a change of where the boundary is by
    // one, not a fix - but it has to be a check before use now,
    // because initialize_ept has already consumed part of the pool and
    // the index no longer starts at zero.
    if (this->next_ept_table >= ept_count) {
        return std::unexpected(zpp::error{error::out_of_ept_entries});
    }

    auto & ept = this->ept[this->next_ept_table++];
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

    // Point the epde at the table. The memory type must be cleared for
    // the reason given at the same three lines in initialize_ept: those
    // bits are reserved in an entry that references a page table.
    epde.large(false);
    epde.type({});
    epde.page_number(host_page_table.virtual_to_physical(ept) >> 12);

    return &ept[(physical_address >> 12) & 0x1ff];
}

std::expected<void, zpp::error> hypervisor::protect_module()
{
    auto number_of_pages = this->module_size / page_size;
    auto & host_page_table = this->host_page_table;

    // A page at a time, because epte_for splits any 2 MB entry still
    // covering one - protecting a whole large page would take two
    // megabytes of the guest's memory with it.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto address = this->module_base + (i * page_size);

        auto physical_address =
            host_page_table.virtual_to_physical(address);

        auto entry = epte_for(physical_address);
        if (!entry) {
            return std::unexpected(entry.error());
        }

        // All four bits: mode-based execute control makes the two
        // execute permissions separate, so clearing only the supervisor
        // one would leave the module fetchable from user mode.
        auto & epte = **entry;
        epte.read(false);
        epte.write(false);
        epte.execute(false);
        epte.execute_user(false);
    }

    return {};
}

void hypervisor::send_wake_nmi(std::uint64_t apic)
{
    // Straight at the local APIC's command register, which the host page
    // table already maps because the interrupt command watch needs it.
    //
    // The destination half is written first because writing the low half
    // is what sends the interrupt - a destination written after it would
    // be written after the thing that used it.
    constexpr std::uint64_t base_mask = 0xffffff000ull;
    auto base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) & base_mask;

    constexpr std::uint64_t interrupt_command_low = 0x300;
    constexpr std::uint64_t interrupt_command_high = 0x310;

    // Delivery mode 100b is NMI, and an NMI carries no vector.
    constexpr std::uint32_t delivery_mode_nmi = 0x4u << 8;
    constexpr std::uint32_t level_assert = 1u << 14;

    auto * bytes = reinterpret_cast<volatile std::uint8_t *>(base);

    arch::x86_64::write32(bytes + interrupt_command_high,
                          static_cast<std::uint32_t>(apic << 24));
    arch::x86_64::write32(bytes + interrupt_command_low,
                          delivery_mode_nmi | level_assert);

    ++this->wake_nmis_sent;
}

bool hypervisor::wait_for_ept_acknowledgement(std::uint64_t budget)
{
    // This processor is up to date by construction and has to say so.
    //
    // Whoever calls this has just changed an entry, and changing one goes
    // through invalidate_ept, which invalidates locally *and* moves the
    // generation on. So the caller's own high water mark is stale the
    // instant it makes the change, and waiting for it would wait for
    // itself - which is what the first version of this did, and it
    // reported no acknowledgement on every attempt.
    if (auto cpu = this->vmcs.vpid(); (0 != cpu) && (cpu <= max_cpus)) {
        this->ept_generation_seen[cpu - 1] =
            this->ept_generation.load(std::memory_order_acquire);
    }

    // The target is captured once and compared with "at least", not
    // "equal to".
    //
    // Equality cannot be satisfied here. The local APIC page is watched
    // and is written constantly by the guest, and every one of those
    // writes moves the generation twice - once to let the write through
    // and once to protect the page again - so a processor that answers
    // is stale again microseconds later. Demanding equality is a
    // livelock, and re-reading the target each round makes it a worse
    // one: the goalpost moves faster than anybody can reach it. Measured
    // as acknowledgement refused on every attempt, with nothing wrong.
    //
    // What is actually needed is weaker and monotone: every processor
    // has invalidated at least once *since the change this call is
    // about*. High water marks only ever rise, so once a processor has
    // passed the captured value it has passed it for good.
    auto target = this->ept_generation.load(std::memory_order_acquire);
    this->ack_target = target;
    this->ack_launched_mask = 0;
    for (std::size_t cpu{}; cpu < max_cpus; ++cpu) {
        if (this->start_up_launched[cpu].load(std::memory_order_acquire)) {
            this->ack_launched_mask |= (std::uint64_t{1} << cpu);
        }
    }

    // How long a processor is given to answer the interrupt before it is
    // read as not executing. Generous: an interrupt takes microseconds to
    // arrive and this is spins, not time.
    constexpr std::uint64_t probe_patience = 1u << 20;
    std::uint64_t probed{};

    while (budget--) {
        auto outstanding = false;

        for (std::size_t cpu{}; cpu < max_cpus; ++cpu) {
            // Only processors that are actually running a guest can
            // acknowledge. One that never launched holds no translation
            // to be stale, so waiting on it would wait forever.
            if (!this->start_up_launched[cpu].load(
                    std::memory_order_acquire)) {
                continue;
            }
            if (this->ept_generation_seen[cpu] < target) {
                outstanding = true;
                this->ack_outstanding_cpu = cpu;
                this->ack_outstanding_seen =
                    this->ept_generation_seen[cpu];

                // Take it out of whatever it is doing. Waiting alone is
                // not enough, and the reason was measured rather than
                // guessed: a processor the guest has halted executes
                // nothing, reaches no exit path, and never stamps.
                // The interrupt is the probe as well as the nudge.
                //
                // A processor that is executing guest code must answer a
                // non-maskable interrupt: "NMI exiting" is set, so it
                // takes one out of anything it is doing, including a
                // halt. A processor that does not answer one is not
                // executing - the SDM's own note on the control says an
                // NMI is neither delivered nor causes an exit while a
                // logical processor is in the wait-for-SIPI state, and
                // this VMM puts processors there itself when the guest
                // sends an INIT.
                //
                // So a silence that outlives the probe is read as "not
                // running", and a processor that is not running holds no
                // translation it can use. That is an inference rather
                // than a fact reported by the hardware, and it is the one
                // soft spot left in this: the alternative is waiting
                // forever for a processor the guest has parked, which is
                // what the first version did.
                if (!this->wake_requested[cpu].exchange(
                        true, std::memory_order_acq_rel)) {
                    send_wake_nmi(this->apic_id[cpu]);
                    probed = budget;
                }

                if ((0 != probed) &&
                    ((probed - budget) > probe_patience)) {
                    this->wake_requested[cpu].store(
                        false, std::memory_order_release);
                    this->ept_generation_seen[cpu] = target;
                    ++this->unresponsive_processors;
                    outstanding = false;
                    continue;
                }
                break;
            }
        }

        if (!outstanding) {
            return true;
        }

        spin_hint();
    }

    return false;
}

void hypervisor::arm_controller_poll(bool armed)
{
    if (armed == this->controller_poll_armed) {
        return;
    }
    this->controller_poll_armed = armed;

    auto controls = this->vmcs.pin_based_vm_execution_controls();

    if (armed) {
        // The unit the timer counts in is the time stamp counter shifted
        // right by IA32_VMX_MISC[4:0], so the same wall clock interval is
        // a different number on every machine and has to be computed.
        auto divisor =
            this->cached_vmx_msr(arch::x86_64::vmx::msr::misc) & 0x1f;
        auto ticks = (controller_poll_microseconds * 1800) >> divisor;

        this->vmcs.vmx_preemption_timer_value(ticks ? ticks : 1);
        this->vmcs.pin_based_vm_execution_controls(
            controls | arch::x86_64::vmx::vm_execution_controls::pin::
                           activate_preemption_timer);
    } else {
        this->vmcs.pin_based_vm_execution_controls(
            controls & ~arch::x86_64::vmx::vm_execution_controls::pin::
                           activate_preemption_timer);
    }
}

void * hypervisor::map_window(std::uint64_t physical_address,
                              std::size_t pages)
{
    return map_window_at(0, physical_address, pages);
}

void * hypervisor::map_window_at(std::size_t first_page,
                                 std::uint64_t physical_address,
                                 std::size_t pages)
{
    auto page = physical_address & ~(page_size - 1);
    auto span = pages + ((physical_address - page) ? 1 : 0);

    if ((first_page + span) > mapping_window_pages) {
        return nullptr;
    }

    for (std::size_t i{}; i < span; ++i) {
        auto at = mapping_window + ((first_page + i) * page_size);

        this->host_page_table.map_page(
            at,
            page + (i * page_size),
            arch::x86_64::page_table::protection::read |
                arch::x86_64::page_table::protection::write);

        // The processor has a translation cached for this address from
        // whoever used the window last, pointing at their page. Without
        // this a read through it answers with their bytes, which is the
        // entire failure mode a shared window has.
        arch::x86_64::invlpg(reinterpret_cast<const void *>(at));
    }

    return reinterpret_cast<std::uint8_t *>(mapping_window +
                                            (first_page * page_size)) +
           (physical_address - page);
}

void hypervisor::rebuild_channel_queue()
{
    // The whole body behind `if constexpr`, because naming a static
    // member of the sink's class template odr-uses it and would carry the
    // channel into a build that switched it off - which
    // scripts/ci/check-diag-absent.sh fails, and did over this function.
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset) {
        return;
    } else {
        // Nothing to rebuild if the channel was never up.
        if (!this->channel_bar) {
            return;
        }

        ++this->channel_rebuilds;
        this->channel_rebuild_result = 0xff;

        auto * bar = this->channel_bar;
        auto started = arch::x86_64::rdtsc();

        // Wait for the controller the guest has just enabled. It is
        // allowed CAP.TO half-seconds to answer, and the guest is about to
        // spend that same wait polling this register itself.
        auto capabilities =
            nvme::controller_capabilities{arch::x86_64::read64(
                static_cast<volatile std::uint8_t *>(bar) +
                nvme::offset_of(nvme::register_offset::capabilities))};

        auto budget = std::uint64_t{1} << 26;
        for (;;) {
            auto status = nvme::controller_status{arch::x86_64::read32(
                static_cast<volatile std::uint8_t *>(bar) +
                nvme::offset_of(nvme::register_offset::status))};
            if (status.fatal_status()) {
                this->channel_rebuild_result = 0xf0;
                return;
            }
            if (status.ready()) {
                break;
            }
            if (0 == budget--) {
                this->channel_rebuild_result = 0xf1;
                return;
            }
        }

        // Where the guest put its admin queue, and how deep it made it.
        // Read now rather than remembered, because the guest chooses these
        // afresh on every reset and may not choose the same thing twice.
        auto attributes =
            nvme::admin_queue_attributes{arch::x86_64::read32(
                static_cast<volatile std::uint8_t *>(bar) +
                nvme::offset_of(
                    nvme::register_offset::admin_queue_attributes))};

        auto submission_base = arch::x86_64::read64(
            static_cast<volatile std::uint8_t *>(bar) +
            nvme::offset_of(
                nvme::register_offset::admin_submission_queue_base));
        auto completion_base = arch::x86_64::read64(
            static_cast<volatile std::uint8_t *>(bar) +
            nvme::offset_of(
                nvme::register_offset::admin_completion_queue_base));

        nvme::admin_borrow::queues where{};
        where.submission_depth = attributes.submission_queue_size();
        where.completion_depth = attributes.completion_queue_size();

        if ((0 == where.submission_depth) ||
            (where.submission_depth > nvme::admin_borrow::max_depth) ||
            (0 == where.completion_depth) ||
            (where.completion_depth > nvme::admin_borrow::max_depth)) {
            this->channel_rebuild_result = 0xf2;
            return;
        }

        // The guest's queues are at addresses this VMM does not choose, so
        // they are reached through the window rather than mapped. Two
        // runs, at different window pages, because the borrow needs both
        // live at once.
        this->mapping_window_lock.lock();
        scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

        constexpr std::size_t submission_pages = 4;
        auto * submission =
            static_cast<nvme::submission_entry *>(map_window_at(
                0, submission_base & ~0xfffull, submission_pages));
        auto * completion =
            static_cast<nvme::completion_entry *>(map_window_at(
                submission_pages, completion_base & ~0xfffull, 1));

        if ((nullptr == submission) || (nullptr == completion)) {
            this->channel_rebuild_result = 0xf3;
            return;
        }

        where.submission = submission;
        where.completion = completion;

        this->rebuild_submission_depth = where.submission_depth;
        this->rebuild_completion_depth = where.completion_depth;
        this->rebuild_submission_base = submission_base;
        this->rebuild_completion_base = completion_base;
        this->rebuild_stride = this->channel_doorbell_stride;

        nvme::admin_borrow::locate(where);

        // Exclude the guest from the admin queue for the length of the
        // borrow, and make sure the exclusion is actually in force before
        // relying on it.
        //
        // The doorbell page is watched in hold mode, so a processor that
        // rings any doorbell faults and is held at the faulting
        // instruction - its write has not taken effect, so the queue is
        // unchanged for as long as the hold lasts. Our own submissions are
        // unaffected: they are stores from root mode, and extended page
        // tables do not apply there.
        //
        // Armed only for the borrow. Left armed it would trap every
        // doorbell the guest ever rings, which is its entire disk traffic.
        auto doorbell_page =
            reinterpret_cast<std::uint64_t>(bar) +
            nvme::offset_of(nvme::register_offset::doorbell_base);

        if (auto armed =
                watch_guest_page_writes(doorbell_page,
                                        &hypervisor::on_doorbell_write,
                                        this,
                                        page_watch::mode::hold);
            !armed) {
            this->channel_rebuild_result = 0xf5;
            return;
        }

        scope_exit unwatch{[&] { unwatch_guest_page(doorbell_page); }};

        // Acknowledged first, held second, and the order is not
        // cosmetic.
        //
        // A processor still holding a translation cached before the
        // arming above would write straight through the protection, so
        // the borrow cannot begin until every running processor has said
        // it has picked the change up. Not getting that answer means not
        // borrowing - an unexcluded borrow desynchronises the guest's
        // admin queue, and no channel is better than that.
        //
        // The other order deadlocks. A processor held at a faulting
        // instruction takes no further exits, so if it stamped its high
        // water mark before the arming moved the generation it can never
        // stamp again and the wait for it never ends. Measured exactly
        // that way, twice: acknowledgement refused on every attempt.
        //
        // Waiting before holding costs nothing. A processor that rings
        // the doorbell in the gap takes the ordinary trapped write and
        // proceeds, and whatever it submitted is part of the queue state
        // the borrow reads when it starts.
        if (!wait_for_ept_acknowledgement(std::uint64_t{1} << 24)) {
            this->channel_rebuild_result = 0xf7;
            return;
        }

        if (!hold_guest_page(doorbell_page)) {
            this->channel_rebuild_result = 0xf6;
            return;
        }

        scope_exit unhold{[&] { release_guest_page(doorbell_page); }};

        // Ours to create: the same identifiers and the same storage the
        // loader used, because the storage outlives every reset - it is
        // reserved memory - and only the controller's idea of the queues
        // was lost.
        nvme::submission_entry payload[2]{};
        payload[0] = nvme::create_io_completion_queue(
            this->channel_queue_id,
            64,
            this->channel_completion_physical,
            false,
            0);
        payload[1] = nvme::create_io_submission_queue(
            this->channel_queue_id,
            64,
            this->channel_submission_physical,
            this->channel_queue_id,
            nvme::queue_priority::medium);

        std::uint16_t payload_status[2]{0xffff, 0xffff};

        nvme::admin_borrow::snapshot saved{
            this->channel_snapshot_submission,
            this->channel_snapshot_completion};

        auto result =
            nvme::admin_borrow::run(bar,
                                    this->channel_doorbell_stride,
                                    where,
                                    saved,
                                    payload,
                                    2,
                                    payload_status,
                                    std::uint64_t{1} << 26);

        this->channel_rebuild_result = static_cast<std::uint64_t>(result);
        this->rebuild_issued = nvme::admin_borrow::last_issued;
        this->rebuild_reaped = nvme::admin_borrow::last_reaped;
        this->rebuild_total = nvme::admin_borrow::last_total;
        this->channel_rebuild_ticks = arch::x86_64::rdtsc() - started;

        if (nvme::borrow_result::ok != result) {
            return;
        }
        if ((0 != payload_status[0]) || (0 != payload_status[1])) {
            this->channel_rebuild_result = 0xf4;
            return;
        }

        // The queues exist again, empty, so the channel is told where they
        // are and that they start from nothing.
        diag::esp_block_sink::adopt_rebuilt_queue(
            static_cast<volatile std::uint8_t *>(bar),
            this->channel_doorbell_stride,
            this->channel_queue_id,
            this->channel_namespace);

        static_cast<void>(capabilities);
    }
}

std::expected<void, zpp::error> hypervisor::protect_region(
    std::uint64_t physical_address, std::uint64_t size)
{
    // The same treatment the module gets, for memory that is ours but
    // does not live inside it.
    //
    // The queue storage is the case this exists for. It used to be an
    // array inside the module and was therefore covered by
    // protect_module for free; it is now a separate allocation the
    // loader makes, and being outside the module means being visible to
    // the guest unless something says otherwise.
    //
    // Leaving it visible would be the worst hole in this channel. The
    // submission queue holds raw NVMe commands - opcodes, logical block
    // addresses, data pointers - and the controller executes whatever is
    // in it when the doorbell rings. A guest able to write there does
    // not merely corrupt the log, it dictates disk commands. EPT does
    // not affect DMA, so the controller still reads the memory the guest
    // can no longer touch, which is exactly the asymmetry wanted.
    auto number_of_pages = (size + page_size - 1) / page_size;

    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto entry = epte_for(physical_address + (i * page_size));
        if (!entry) {
            return std::unexpected(entry.error());
        }

        auto & epte = **entry;
        epte.read(false);
        epte.write(false);
        epte.execute(false);
        epte.execute_user(false);
    }

    invalidate_ept();
    return {};
}

std::expected<void, zpp::error>
hypervisor::watch_guest_page_writes(std::uint64_t guest_physical,
                                    page_watch::handler on_write,
                                    void * context,
                                    page_watch::mode behaviour)
{
    auto page = guest_physical >> 12;

    // Re-arming the same page replaces the handler rather than taking a
    // second slot, so a caller that cannot easily tell whether it has
    // already armed does not silently exhaust the table.
    page_watch * free_slot{};
    for (auto & watch : this->watches) {
        if (watch.armed && (watch.page == page)) {
            watch.on_write = on_write;
            watch.context = context;
            watch.behaviour = behaviour;
            return {};
        }
        if (!watch.armed && !free_slot) {
            free_slot = &watch;
        }
    }

    if (!free_slot) {
        return std::unexpected(zpp::error{error::out_of_ept_entries});
    }

    // The EPT is an identity map, so the guest physical address is also
    // the host physical one. Taken through the same call the module
    // protection uses, so a 2 MB entry covering a device register page
    // is split here rather than silently protecting two megabytes of
    // unrelated memory.
    auto entry = epte_for(page << 12);
    if (!entry) {
        return std::unexpected(entry.error());
    }

    // Reads stay permitted. A driver polls status registers far more
    // often than it writes commands, and every permitted read is a VM
    // exit that does not happen.
    (*entry)->write(false);
    invalidate_ept();

    free_slot->page = page;
    free_slot->on_write = on_write;
    free_slot->context = context;
    free_slot->behaviour = behaviour;
    free_slot->held.store(false, std::memory_order_relaxed);
    free_slot->armed = true;

    log("watching writes to guest page {}", page);
    return {};
}

void hypervisor::unwatch_guest_page(std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }

        // The entry exists already - the page was split when the watch
        // was armed - so this cannot fail and nothing here has to cope
        // with it failing.
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(true);
            invalidate_ept();
        }

        watch.page = {};
        watch.on_write = {};
        watch.context = {};
        watch.behaviour = page_watch::mode::notify;
        watch.held.store(false, std::memory_order_relaxed);
        watch.armed = false;
        log("stopped watching guest page {}", page);
        return;
    }
}

bool hypervisor::hold_guest_page(std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }
        if (page_watch::mode::hold != watch.behaviour) {
            // Refused rather than silently doing nothing, so that a
            // caller cannot go on believing it has exclusion it was
            // never given.
            return false;
        }
        watch.held.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

void hypervisor::release_guest_page(std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (watch.armed && (watch.page == page)) {
            watch.held.store(false, std::memory_order_release);
            return;
        }
    }
}

void hypervisor::monitor_trap_flag(bool value)
{
    // Bit 27 of the primary processor based controls, SDM Table 25-6.
    constexpr std::uint64_t monitor_trap_flag_bit = 1ull << 27;

    auto controls =
        this->vmcs.primary_processor_based_vm_execution_controls();
    this->vmcs.primary_processor_based_vm_execution_controls(
        value ? (controls | monitor_trap_flag_bit)
              : (controls & ~monitor_trap_flag_bit));
}

bool hypervisor::on_ept_violation(std::size_t cpu)
{
    auto guest_physical = this->vmcs.guest_physical_address();
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }

        // A held page stops the writer here, at the faulting
        // instruction, until whoever is holding it lets go.
        //
        // This is the whole of the exclusion. The write has not taken
        // effect when the violation is delivered, so the page is
        // unchanged for as long as the spin lasts, and a borrower can
        // work on a structure the guest owns without the guest being
        // able to touch it. No cooperation is required and no processor
        // that is not writing this page is delayed at all.
        //
        // Spinning inside the exit is deliberate. The alternative -
        // resuming and re-faulting - burns exits for the same wait and
        // gives the guest a window between the resume and the next
        // fault, which is the window this exists to close.
        while (watch.held.load(std::memory_order_acquire)) {
            zpp::spin_hint();
        }

        // Let the guest's own instruction do the write, then look at
        // what it did. The alternative is to decode and emulate it, and
        // an EPT violation reports neither the data nor the operand size
        // - SDM Table 30-7 - so that road starts with an x86 decoder.
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(true);
            invalidate_ept();
        }

        this->stepping_watch[cpu] = true;
        this->stepping_page[cpu] = page;
        monitor_trap_flag(true);
        return true;
    }

    // Not a watched page. It may still be one of ours: protect_module
    // makes every page of this module not-present to the guest, so any
    // access to them arrives here.
    //
    // Stopping the processor for that would be a poor trade. A guest is
    // entitled to walk physical memory - a memory manager building its
    // own map does exactly that - and killing it for reading an address
    // it has no idea is special turns a curiosity into a bug check.
    //
    // So the page is redirected, permanently, to a page of zeroes. The
    // guest is resumed **without** advancing past its own instruction,
    // which then re-executes and completes against the decoy. That needs
    // no decoder, cannot disagree with what the instruction meant, and
    // costs one exit per module page ever touched rather than one per
    // access - a guest scanning memory pays once per page and then runs
    // at full speed through a region that tells it nothing.
    if (this->module_physical_to_virtual.find(page << 12) !=
        this->module_physical_to_virtual.end()) {
        ++this->module_access_count;

        diag::log<diag::severity::warning>(
            "guest touched module memory at {} from rip {}, {} pages so "
            "far",
            guest_physical,
            this->vmcs.guest_rip(),
            this->module_access_count);
        log("guest touched module memory at {} from rip {}",
            guest_physical,
            this->vmcs.guest_rip());

        if (auto entry = epte_for(page << 12)) {
            (*entry)->page_number(
                this->host_page_table.virtual_to_physical(
                    this->unprotected_memory.decoy_page) >>
                12);

            // Executable as well as readable, deliberately. If the guest
            // jumps into this it should run zeroes and reach whatever
            // conclusion that leads to, rather than fault here forever
            // on a page it is allowed to touch.
            (*entry)->read(true);
            (*entry)->write(true);
            (*entry)->execute(true);
            (*entry)->execute_user(true);
            invalidate_ept();
        }

        return true;
    }

    return false;
}

bool hypervisor::on_monitor_trap_flag(std::size_t cpu)
{
    if (!this->stepping_watch[cpu]) {
        return false;
    }

    auto page = this->stepping_page[cpu];
    this->stepping_watch[cpu] = false;
    this->stepping_page[cpu] = {};
    monitor_trap_flag(false);

    // Close the page again before the handler runs, so that a handler
    // which arms or disarms watches cannot observe a half open state.
    if (auto entry = epte_for(page << 12)) {
        (*entry)->write(false);
        invalidate_ept();
    }

    for (auto & watch : this->watches) {
        if (watch.armed && (watch.page == page) && watch.on_write) {
            watch.on_write(watch.context, page);
            break;
        }
    }

    return true;
}

void hypervisor::unprotect_guest_memory()
{
    auto number_of_pages = sizeof(this->unprotected_memory) / page_size;

    // Undoes protect_module over the part of the module the guest is
    // meant to reach, so it has to run after it. Walked by hand rather
    // than through epte_for for the same reason: protect_module has
    // already split these pages, so the 4 KB entry is known to exist.
    for (std::size_t i{}; i < number_of_pages; ++i) {
        auto address =
            reinterpret_cast<unsigned char *>(&this->unprotected_memory) +
            (i * page_size);

        auto physical_address =
            this->host_page_table.virtual_to_physical(address);

        auto & epde = this->epd[physical_address >> 30]
                               [(physical_address >> 21) & 0x1ff];

        auto ept_physical_address = epde.page_number() << 12;

        auto ept = reinterpret_cast<arch::x86_64::vmx::epte *>(
            this->module_physical_to_virtual.find(ept_physical_address)
                ->second);

        // The exact inverse of what protect_module wrote.
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

    // One pair per virtual processor: a VMCS may not be active on more
    // than one logical processor.
    auto & vmx = this->vmx[this->next_virtual_processor - 1];
    auto & vmx_vmcs = this->vmx_vmcs[this->next_virtual_processor - 1];

    // Its low bits are the VMCS revision identifier, which VMPTRLD
    // checks against the first dword of the region.
    const auto & basic_msr = this->cached_vmx_msr(vmx_msr::basic);

    // The processor reaches all of these by physical address, through no
    // page table of ours.
    this->vmx_physical = this->host_page_table.virtual_to_physical(&vmx);
    this->vmcs_physical =
        this->host_page_table.virtual_to_physical(&vmx_vmcs);
    this->epml4_physical =
        this->host_page_table.virtual_to_physical(&this->epml4);
    this->msr_bitmap_physical =
        this->host_page_table.virtual_to_physical(&this->msr_bitmap);
    this->io_bitmap_a_physical =
        this->host_page_table.virtual_to_physical(&this->io_bitmap_a);
    this->io_bitmap_b_physical =
        this->host_page_table.virtual_to_physical(&this->io_bitmap_b);

    vmx.revision_id = basic_msr & 0xffffffff;
    vmx_vmcs.revision_id = basic_msr & 0xffffffff;

    // Started from what this processor was already running with, so the
    // host keeps the paging mode and feature set it was found in.
    this->host_cr0 = this->guest_cr0;
    this->host_cr4 = this->guest_cr4;

    // A bit set in FIXED0 must be 1 and a bit clear in FIXED1 must be 0
    // in VMX operation (SDM A.7), hence the OR and the AND. vmxon faults
    // on a CR0 that does not satisfy them.
    this->host_cr0 &=
        this->cached_vmx_msr(vmx_msr::cr0_fixed_1) & 0xffffffff;
    this->host_cr0 |=
        this->cached_vmx_msr(vmx_msr::cr0_fixed_0) & 0xffffffff;

    // The same for CR4, SDM A.8. VMXE is the bit this turns on in
    // practice, and vmxon cannot execute without it.
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

    // This handler must do as little as possible, and that is not a style
    // preference - it is the difference between working and not.
    //
    // A start-up IPI arriving while this processor is not yet in the
    // wait-for-SIPI activity state is discarded rather than queued (SDM
    // 28.2). Under nested virtualization the layer below discards it for
    // the whole time this VMM is in VMX root mode, deliberately - KVM's
    // vmx_apic_init_signal_blocked() is `nested.vmxon && !is_guest_mode`,
    // and the maintainers' position is that it is software's job not to be
    // in root mode when a SIPI arrives. So every instruction between the
    // INIT exit and the resume is a chance to lose the IPI that was
    // supposed to start this processor, and each VMCS write can itself be
    // an exit to the layer below.
    //
    // Windows allows about 210 microseconds between the INIT and the first
    // start-up IPI; the second follows 200 microseconds later. That is the
    // entire budget. So the architectural reset that used to be here now
    // lives in emulate_start_up_ipi, which may take as long as it likes -
    // by the time it runs, the IPI has already been received. The rule to
    // keep is blunt: do not add code to this function.
    //
    // The one read added to that budget on purpose, because the whole
    // adoption design rests on an assumption about it that nothing else
    // records: the activity state this processor was in when the INIT
    // arrived. Everything below sets that field, and record_exit samples
    // it after the fact, so without this the state we *found* is the one
    // thing about an INIT that is unrecoverable afterwards. It answers
    // whether the guest INITed a processor that was running, halted, or
    // already parked waiting for a start-up IPI - and a hang whose cause
    // is the last of those looks like nothing else in the log.
    auto activity_state_found = vmcs.guest_activity_state();

    // What remains is the activity state and the two things that state
    // requires to be clear. Wait-for-SIPI does not permit a pending event:
    // real hardware fails VM entry on a valid VM-entry interruption
    // information field combined with this activity state, even though the
    // nested implementations do not check it.
    vmcs.vm_entry_interruption_information_field(0);
    vmcs.vm_entry_exception_error_code(0);
    vmcs.guest_interruptibility_state(0);

    // There are two ways the start-up IPI that follows this INIT can
    // arrive, and the rest of this function is about choosing one of them
    // and saying so.
    //
    // The architectural way is the wait-for-SIPI activity state: park the
    // processor across a VM entry and let the hardware deliver the IPI as
    // a VM exit. SDM 28.2 is what makes that work and also what makes it
    // fragile - "If a logical processor is not in the wait-for-SIPI
    // activity state when a SIPI arrives, no VM exit occurs and the SIPI
    // is discarded" - so it requires trusting whatever is below this VMM
    // to deliver it. Under nested virtualization that trust is misplaced
    // by design: the IPI is discarded for as long as this VMM is in root
    // mode. So the other way is for the sender to hand the vector over
    // through memory, from its intercepted write to the interrupt command
    // register, and for this processor to wait for it here in root mode
    // where nothing can be lost.
    //
    // The choice is published in start_up_handoff before either wait
    // begins, because the sender reads it to decide whether it may
    // swallow the guest's write. Both sides guessing independently is how
    // a start-up IPI was lost: this processor would park in wait-for-SIPI
    // while the sender, seeing only that the processor was virtualized,
    // handed the vector to a mailbox nobody was reading any more and
    // swallowed the write that would have woken it.
    //
    // Bounded, because a processor spinning forever on an INIT whose
    // start-up IPI never comes is worse than one that gives up: it would
    // take the machine down with no diagnosis. On timeout fall back to the
    // architectural path, which is correct on real hardware and no worse
    // than what came before anywhere else.
    auto cpu = vmcs.vpid() - 1;
    if (cpu >= max_cpus) {
        return;
    }

    this->started_by_start_up_ipi[cpu] = false;

    // Only worth waiting for if the interrupt command register is an MSR,
    // which it is only in x2APIC mode. In xAPIC mode it is a location on
    // the APIC page and the MSR bitmap never sees it, so waiting would
    // burn the whole timeout before falling back for nothing.
    auto x2apic = x2apic_enabled();

    // And only worth waiting for when the hardware path cannot be
    // trusted, which is precisely when something is virtualizing *us*.
    //
    // The architectural wait-for-SIPI path below is correct, cheaper and
    // better tested: it is what real hardware implements and what the
    // Bochs CI exercises on four processors. It fails in exactly one
    // situation - a layer below that discards the start-up IPI while this
    // VMM is in VMX root mode - so the software path is taken in exactly
    // that situation and nowhere else. Bare metal and Bochs never spin.
    //
    // The test is our own CPUID rather than the guest's: this executes in
    // root mode, so it reports what is underneath this VMM. On bare metal
    // the bit is clear. Note the exit handler clears this same bit out of
    // the guest's view of leaf 1, so the two must not be confused - the
    // guest is told there is no hypervisor under it, while we ask whether
    // there is one under us.
    //
    // SDM Vol. 2A, CPUID, "CPUID.01H:ECX Feature Information": bit 31 is
    // reserved and always returns 0 on real hardware, which is why it is
    // the conventional way for a hypervisor to announce itself.
    constexpr std::uint32_t hypervisor_present_bit = (1u << 31);
    std::uint32_t identification[4]{};
    arch::x86_64::cpuid(1, 0, identification);
    auto nested = 0 != (identification[2] & hypervisor_present_bit);

    auto & handoff = this->start_up_handoff[cpu];

    // Whether the software wait was taken at all, which is the one thing
    // worth saying about this on the way out: it is what a sender's
    // decision has to have agreed with.
    auto waited = x2apic && nested;

    if (waited) {
        // Published before the first attempt, so a sender that arrives
        // during the wait finds this processor listening.
        handoff.store(start_up_handoff_state::software_wait);

        // Bounded so a processor cannot spin forever on an INIT whose
        // start-up IPI never arrives. The senders in practice follow
        // within tens of microseconds to ten milliseconds, so this is
        // generous.
        constexpr std::uint32_t start_up_wait_attempts = 2000000;
        for (std::uint32_t attempt{}; attempt < start_up_wait_attempts;
             ++attempt) {
            if (auto state = handoff.load();
                start_up_handoff_state::is_delivered(state)) {
                apply_start_up(context,
                               start_up_handoff_state::vector(state));
                return;
            }
            zpp::spin_hint();
        }

        // Giving up, which is the moment the two sides could disagree.
        // A compare-exchange rather than a store because a sender may be
        // handing a vector over at exactly this instant: either this wins
        // and the sender then sees a processor that is no longer
        // listening and issues the IPI to hardware, or the sender wins
        // and the vector is applied here instead. Exactly one of the two
        // happens, which is the whole point - a plain store here would
        // discard a vector that had already been swallowed on the
        // sender's side, and nothing would ever start this processor.
        //
        // The state the exchange found is checked rather than assumed to
        // carry a vector. Nothing else writes it while this processor is
        // listening, so anything else is impossible - and starting a
        // processor at a vector computed from an impossible value would
        // send it to an address nobody chose, which is a worse way to
        // fail than falling through to the architectural wait.
        auto expected = start_up_handoff_state::software_wait;
        if (!handoff.compare_exchange_strong(
                expected, start_up_handoff_state::hardware_wait) &&
            start_up_handoff_state::is_delivered(expected)) {
            apply_start_up(context,
                           start_up_handoff_state::vector(expected));
            return;
        }
    } else {
        // Waiting on hardware from the start, which is the case on bare
        // metal and under Bochs. Said out loud here rather than left
        // implied, because a sender that assumed otherwise is what used
        // to swallow the IPI this processor is now waiting for.
        handoff.store(start_up_handoff_state::hardware_wait);
    }

    vmcs.guest_activity_state(
        arch::x86_64::vmx::activity_state::wait_for_start_up_ipi);

    // One log line, and the placement is deliberate: the activity state is
    // written first, so nothing about the diagnostic delays the write the
    // resume depends on. A processor that is never started again is left
    // with this as its last word, next to a recorded exit of INIT with
    // activity state 3 - and the sender's own line, in
    // on_interrupt_command, says which mechanism it then used. The two
    // together are what identifies a swallowed start-up IPI without
    // repeating the investigation: this line says which hand-off this
    // processor is waiting on, that one says which the sender used, and
    // they have to agree.
    log("cpu {} init: found activity {}, waiting for the hardware "
        "start-up ipi, software wait {}",
        vmcs.vpid(),
        activity_state_found,
        waited);
}

void hypervisor::emulate_start_up_ipi(arch::x86_64::context & context,
                                      std::uint64_t vector)
{
    // Reached whenever this processor's INIT left it waiting on hardware,
    // which is every INIT on bare metal and under Bochs. Under a layer
    // that discards the IPI while this VMM is in root mode the INIT
    // handler waits for the vector in root mode instead and has already
    // applied it by now, so this exit never arrives there.
    apply_start_up(context, vector);
}

std::uint64_t hypervisor::local_apic_id()
{
    // Read out of CPUID rather than out of the local APIC, and that is the
    // whole point of this function.
    //
    // The APIC's own identifier register is only an MSR in x2APIC mode,
    // and this VMM launches before any of that is settled: the firmware is
    // in xAPIC mode, so reading the MSR raises #GP. CPUID answers in every
    // mode and needs no APIC state at all.
    //
    // Not because an INIT returns the APIC to xAPIC mode - it does not,
    // and a comment here used to say so. SDM 13.12.5: "An INIT in this
    // state keeps the x2APIC in the x2APIC mode. The state of the local
    // APIC ID register is preserved (all 32 bits)." Only a reset does
    // that. The consequence to keep is that emulating an INIT must leave
    // IA32_APIC_BASE alone, which apply_start_up does - hardware preserves
    // it and nothing here should be "fixed" to clear it.
    //
    // Getting this wrong was silent. The identifier used to be recorded
    // only when x2APIC happened to be enabled, which on this firmware is
    // never, so the whole table stayed zero and a start-up IPI's
    // destination matched the boot processor's slot by accident.
    constexpr std::uint32_t extended_topology_leaf = 0x1f;
    constexpr std::uint32_t topology_leaf = 0x0b;

    std::uint32_t registers[4]{};

    // The highest leaf this processor answers, so an unsupported one is
    // not asked for - CPUID returns whatever the highest leaf holds
    // instead of failing, which would silently be somebody else's data.
    arch::x86_64::cpuid(0, 0, registers);
    auto highest_leaf = registers[0];

    // SDM Vol. 2A, CPUID: leaf 1FH EDX and leaf 0BH EDX both give "x2APIC
    // ID the current logical processor". The full thirty-two bits, which
    // is what an x2APIC interrupt command register carries as its
    // destination. Leaf 1FH is the newer of the two and is reported as
    // unsupported by a zero in EBX.
    if (highest_leaf >= extended_topology_leaf) {
        arch::x86_64::cpuid(extended_topology_leaf, 0, registers);
        if (registers[1]) {
            return registers[3];
        }
    }

    if (highest_leaf >= topology_leaf) {
        arch::x86_64::cpuid(topology_leaf, 0, registers);
        if (registers[1]) {
            return registers[3];
        }
    }

    // Neither topology leaf, so the initial APIC id out of leaf 1. Eight
    // bits, which is all a processor without x2APIC has.
    constexpr std::uint32_t initial_apic_id_shift = 24;
    arch::x86_64::cpuid(1, 0, registers);
    return registers[1] >> initial_apic_id_shift;
}

bool hypervisor::x2apic_enabled()
{
    // IA32_APIC_BASE.EXTD. With it clear the local APIC is in xAPIC mode,
    // its registers live on the APIC page rather than in MSR space, and
    // touching an x2APIC MSR raises #GP. SDM 13.12.1.
    constexpr std::uint64_t apic_base_x2apic_enabled = (1ull << 10);
    return 0 != (arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) &
                 apic_base_x2apic_enabled);
}

void hypervisor::intercept_interrupt_command(bool intercept)
{
    // The MSR bitmap is four 1024 byte bitmaps: reads of the low range,
    // reads of the high range, writes of the low range, writes of the high
    // range. The interrupt command register is in the low range.
    constexpr std::size_t write_low_range = 0x800;

    auto bit = arch::x86_64::msr::ia32_x2apic_icr;
    auto & byte = this->msr_bitmap[write_low_range + (bit / 8)];
    auto mask = static_cast<std::uint8_t>(1u << (bit % 8));

    if (intercept) {
        byte |= mask;
    } else {
        byte &= static_cast<std::uint8_t>(~mask);
    }
}

void hypervisor::intercept_io_port(std::uint16_t port, bool intercept)
{
    // One bit per port, the first bitmap covering 0x0000 to 0x7fff and
    // the second the rest.
    auto & bitmap =
        (port < 0x8000) ? this->io_bitmap_a : this->io_bitmap_b;
    auto index = static_cast<std::size_t>(port & 0x7fff);
    auto & byte = bitmap[index / 8];
    auto mask = static_cast<std::uint8_t>(1u << (index % 8));

    if (intercept) {
        byte |= mask;
    } else {
        byte &= static_cast<std::uint8_t>(~mask);
    }
}

bool hypervisor::on_io_instruction()
{
    // SDM Table 28-5. The port is in bits 31:16 for the forms that carry
    // one, which is every form this VMM asks to see; direction is bit 3,
    // with one meaning in.
    auto qualification = this->vmcs.exit_qualification();
    auto port = static_cast<std::uint16_t>((qualification >> 16) & 0xffff);
    auto reading = 0 != (qualification & (1ull << 3));

    if ((0 == this->sleep_control_port) ||
        ((port != this->sleep_control_port) &&
         (port != this->sleep_control_port_secondary))) {
        return false;
    }

    // Only the write matters: reading the register tells the guest what
    // it already wrote and enters nothing.
    if (!reading) {
        log("guest is entering a sleep state through port {}", port);
        diag::log<diag::severity::warning>("sleep entered through port {}",
                                           port);

        // Everything this VMM is dies here and nothing brings it back:
        // root mode does not survive S3, and there is no resume path.
        // Said out loud, in the channel that survives, rather than
        // discovered later by noticing the machine is unvirtualized.
        diag::pump::drain();
    }

    // Passed through rather than emulated, by releasing the port and
    // resuming *without* advancing past the instruction: the guest
    // re-executes its own OUT, which now reaches hardware. That needs no
    // decoder and cannot disagree with what the instruction meant.
    //
    // Releasing it also means this is seen once. A sleep is not a thing
    // worth trapping twice, and re-arming would have to happen on a
    // resume path that does not exist yet.
    intercept_io_port(port, false);
    return true;
}

void hypervisor::watch_local_apic(bool watch)
{
    // Where the page is, from the guest's own view of it. The base is
    // not architecturally fixed - IA32_APIC_BASE can relocate it - so it
    // is read rather than assumed to be 0xfee00000.
    constexpr std::uint64_t base_mask = 0xffffff000ull;
    auto base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) & base_mask;

    if (!watch) {
        if (this->watched_apic_page) {
            unwatch_guest_page(this->watched_apic_page);
            this->watched_apic_page = 0;
        }
        return;
    }

    if (this->watched_apic_page == base) {
        return;
    }
    if (this->watched_apic_page) {
        unwatch_guest_page(this->watched_apic_page);
    }

    if (auto armed = watch_guest_page_writes(
            base, &hypervisor::on_local_apic_write, this)) {
        this->watched_apic_page = base;
        log("watching the local apic page at {}", base);
    } else {
        // Refused rather than left half armed. Missing an IPI is bad;
        // believing one is being watched when it is not is worse.
        this->watched_apic_page = 0;
        log("could not watch the local apic page at {}", base);
    }
}

void hypervisor::on_controller_register_write(void * context,
                                              std::uint64_t page)
{
    static_cast<void>(context);
    static_cast<void>(page);

    // Everything about what this means belongs to the channel, so it is
    // asked rather than told. All this side knows is that the guest
    // touched a page it was watching.
    //
    // Behind `if constexpr` rather than a plain call, because naming a
    // static member of a class template odr-uses it and would put the
    // channel's storage and code into a build that has the channel
    // switched off. scripts/ci/check-diag-absent.sh fails the release
    // build over exactly that, and did over this line.
    if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
        auto & self = *static_cast<hypervisor *>(context);
        if (!self.channel_bar) {
            return;
        }

        // The write has already been stepped over, so this is the value
        // the guest just wrote.
        auto configuration =
            nvme::controller_configuration{arch::x86_64::read32(
                static_cast<volatile std::uint8_t *>(self.channel_bar) +
                nvme::offset_of(nvme::register_offset::configuration))};

        ++self.channel_register_writes;
        self.channel_last_configuration = configuration.value();

        auto now = configuration.enable();
        auto was = self.channel_controller_enabled;
        self.channel_controller_enabled = now;

        if (was && !now) {
            // Going down. The queues are gone with it.
            diag::esp_block_sink::note_controller_write();
        } else if (!was && now) {
            // Coming back up. Handled here when the write is caught, and
            // by poll_for_controller_return when it is not - see there
            // for why catching it cannot be relied on.
            self.rebuild_channel_queue();
        }
    }
}

void hypervisor::on_doorbell_write(void * context, std::uint64_t page)
{
    static_cast<void>(context);
    static_cast<void>(page);
}

void hypervisor::on_local_apic_write(void * context, std::uint64_t page)
{
    auto & self = *static_cast<hypervisor *>(context);

    // The write has already happened - the watch steps over it before
    // saying so - which is why the command can simply be read back out
    // of the page rather than decoded from the instruction.
    //
    // The xAPIC form is two dwords rather than one quadword: the low
    // half at 0x300, and the destination in the top eight bits of the
    // dword at 0x310. Composed here into the same shape the x2APIC path
    // produces, so one decision function serves both.
    constexpr std::uint64_t interrupt_command_low = 0x300;
    constexpr std::uint64_t interrupt_command_high = 0x310;
    constexpr std::uint64_t delivery_status_pending = 1ull << 12;

    auto * bytes = reinterpret_cast<volatile std::uint8_t *>(page << 12);
    auto low = arch::x86_64::read32(bytes + interrupt_command_low);
    auto high = arch::x86_64::read32(bytes + interrupt_command_high);

    // Every other register in this page is written far more often than
    // the command is - the end of interrupt one on every interrupt - so
    // a write that left no command pending was not a command at all.
    if (0 == (low & delivery_status_pending)) {
        return;
    }

    auto command = std::uint64_t{low} | (std::uint64_t{high >> 24} << 32);

    if (auto issue = self.on_interrupt_command(command)) {
        // Put back what the handler decided, in the form this interface
        // takes. The high half is written first, because writing the low
        // half is what sends it.
        arch::x86_64::write32(
            bytes + interrupt_command_high,
            static_cast<std::uint32_t>((*issue >> 32) << 24));
        arch::x86_64::write32(bytes + interrupt_command_low,
                              static_cast<std::uint32_t>(*issue));
    }
}

std::optional<std::size_t>
hypervisor::processor_slot(std::uint64_t apic_id)
{
    for (std::size_t slot{}; slot < this->number_of_known_processors;
         ++slot) {
        if (this->apic_id[slot] == apic_id) {
            return slot;
        }
    }

    if (this->number_of_known_processors >= max_cpus) {
        return {};
    }

    auto slot = this->number_of_known_processors++;
    this->apic_id[slot] = apic_id;
    return slot;
}

std::optional<std::uint64_t>
hypervisor::on_interrupt_command(std::uint64_t command)
{
    // The interrupt command register, x2APIC form: vector in the low
    // eight bits, delivery mode in bits 10:8, and the destination APIC id
    // in the upper half rather than in a second register.
    constexpr std::uint64_t vector_mask = 0xff;
    constexpr std::uint64_t delivery_mode_shift = 8;
    constexpr std::uint64_t delivery_mode_mask = 0x7;
    constexpr std::uint64_t delivery_mode_start_up = 6;
    constexpr std::uint64_t destination_shift = 32;

    // The destination shorthand, in bits 19:18. Zero means the destination
    // field names the target; anything else is a broadcast and the
    // destination field means nothing at all.
    constexpr std::uint64_t shorthand_shift = 18;
    constexpr std::uint64_t shorthand_mask = 0x3;
    constexpr std::uint64_t shorthand_none = 0;

    // INIT, which is logged and otherwise left alone. Logged because it is
    // the first half of the only sequence that starts a processor, and
    // because a hang in that sequence is otherwise invisible: the log then
    // shows what the guest sent, to which destination, and in what order,
    // next to what each target did about it. Cheap - a guest sends INIT
    // only to start or reset a processor, never on a hot path, unlike
    // every other delivery mode reaching this function.
    //
    // Their absence says something too, and it is the first thing to check
    // when a processor never starts: only the x2APIC interrupt command
    // register is an MSR, so a guest still in xAPIC mode writes its
    // command to the APIC page and reaches none of this. No line here at
    // all means the sequence was never seen, not that it was never sent.
    constexpr std::uint64_t delivery_mode_init = 5;

    auto delivery_mode =
        (command >> delivery_mode_shift) & delivery_mode_mask;
    if (delivery_mode_init == delivery_mode) {
        log("guest init ipi, command {}", command);
        return command;
    }

    if (delivery_mode_start_up != delivery_mode) {
        // Everything else goes out as the guest wrote it. That includes
        // the INIT handled above, and it especially includes INIT: it is
        // what leaves the target waiting for a start-up IPI, which is the
        // state the rest of this needs it in. There is nothing here that
        // improves on any of them.
        return command;
    }

    // A broadcast start-up IPI cannot be redirected one processor at a
    // time, and this VMM has no other way to enumerate what it would be
    // broadcasting to - it learns a processor exists by being told to
    // start it. So it is passed through, which hands those processors to
    // the guest unvirtualized, and said out loud rather than left to be
    // discovered.
    //
    // Decoded rather than ignored, which is what used to happen. Reading
    // bits 63:32 of a shorthand command yields whatever the guest left
    // there - zero, in practice - and matching that against the table of
    // known processors credited the boot processor with a start-up IPI
    // nobody had sent it.
    auto shorthand = (command >> shorthand_shift) & shorthand_mask;
    if (shorthand_none != shorthand) {
        log("broadcast start-up ipi, shorthand {}, not adopted",
            shorthand);
        return command;
    }

    auto destination = command >> destination_shift;
    auto vector = command & vector_mask;

    auto slot = processor_slot(destination);
    if (!slot) {
        log("no room to track the processor with apic id {}", destination);
        return command;
    }

    if (this->processor_virtualized[*slot]) {
        // Already started since its last INIT. A guest sends two start-up
        // IPIs and the second must not be acted on: sending a processor
        // that is already running back to its entry point wedges it in a
        // way indistinguishable from never having started. Swallowed
        // rather than passed on, which is also what the hardware would do
        // with it - SDM 29.7.2: "The active state blocks start-up IPIs
        // (SIPIs). SIPIs that arrive while a logical processor is in the
        // active state and in VMX non-root operation are discarded and do
        // not cause VM exits."
        if (this->started_by_start_up_ipi[*slot]) {
            log("guest start-up ipi for cpu {}, already started, ignored",
                *slot);
            return {};
        }

        // Under the hypervisor and out of an INIT, so the target chose
        // which hand-off it is waiting on and published it. Follow that
        // choice rather than assuming one.
        //
        // A compare-exchange, not a store, and that is the whole fix: the
        // target leaves the software wait on its own timeout, and a store
        // would put the vector into a mailbox nobody reads again while
        // swallowing the write that would have woken it. Then nothing
        // starts that processor and the guest waits for it forever. Bare
        // metal never takes the software wait at all, so before this the
        // swallow was unconditional and the loss certain the moment a
        // guest re-started a processor this VMM had already adopted.
        auto expected = start_up_handoff_state::software_wait;
        if (this->start_up_handoff[*slot].compare_exchange_strong(
                expected, start_up_handoff_state::deliver(vector))) {
            log("guest start-up ipi for cpu {}, vector {}, handed over",
                *slot,
                vector);
            return {};
        }

        // Not listening, so the hardware path is the only one that can
        // start it and the guest's own write has to go out. This keeps the
        // processor virtualized rather than handing it over: it is in VMX
        // non-root operation parked in the wait-for-SIPI activity state,
        // so SDM 28.2 turns the delivery into a VM exit on it rather than
        // letting it execute the guest's real-mode entry point directly.
        //
        // Correct on bare metal, with one window left that is the
        // architecture's rather than ours: between the target publishing
        // hardware_wait and its VM entry actually reaching that activity
        // state it is still in root mode, and an IPI arriving in that
        // instant is discarded. That window is what the second start-up
        // IPI of the conventional sequence covers - SDM Table 11-1 sends
        // two, 200 microseconds apart, and Vol. 3A's description of
        // delivery mode 110 says outright that a SIPI is not retried by
        // hardware and that reissuing it is software's job.
        //
        // Not futile under a layer either, which is the other half of why
        // this is the right thing to issue. KVM drops a pending start-up
        // IPI only while this VMM is in root mode - lapic.c's
        // kvm_apic_accept_events() clears KVM_APIC_SIPI when
        // kvm_apic_init_sipi_allowed() is false - and that is precisely
        // the window the software hand-off above covers. Once this VMM has
        // entered with an activity state of wait-for-SIPI, KVM's
        // nested_vmx_enter_non_root_mode() records
        // KVM_MP_STATE_INIT_RECEIVED for it, and vmx_check_nested_events()
        // then delivers the IPI as an EXIT_REASON_SIPI_SIGNAL exit. So the
        // two states this VMM publishes line up exactly with the two
        // KVM distinguishes, and each is issued the mechanism that works.
        //
        // The pair of log lines says which was used rather than leaving it
        // to be deduced.
        log("guest start-up ipi for cpu {}, vector {}, to hardware, "
            "target hand-off {}",
            *slot,
            vector,
            expected);
        return command;
    }

    // Never seen before, so this is the guest starting it for the first
    // time. Nothing has virtualized it and nothing can, from here - a
    // processor cannot be put into VMX operation by another one. So it is
    // started in this VMM's own trampoline instead, which brings it up
    // under the hypervisor and only then lets it run from the vector the
    // guest asked for.
    if (!this->start_up_memory) {
        log("no start-up memory, cpu {} will run unvirtualized", *slot);
        return command;
    }

    if (start_application_processor(*slot, vector)) {
        return {};
    }

    // It did not come up. Passing the guest's own start-up IPI through is
    // the least bad thing left: the processor is still waiting for one, so
    // the guest gets a processor it can use, unvirtualized. Losing it
    // outright would usually take the guest down with it.
    return command;
}

void hypervisor::initialize_start_up_memory(std::uint64_t memory)
{
    // How much of the blob there is to copy. Its own page has to hold it,
    // because the pages after it are the temporary page table.
    auto blob_size =
        static_cast<std::size_t>(arch::x86_64::zpp_ap_start_up_end -
                                 arch::x86_64::zpp_ap_start_up_begin);
    if (blob_size > page_size) {
        log("the start-up trampoline is {} bytes, too big for its page",
            blob_size);
        return;
    }

    // A start-up IPI names its entry point by page number, in eight bits,
    // so anything not page aligned or not below one megabyte cannot be
    // started from at all. Checked rather than assumed: this address comes
    // from the loader, and a platform that got it wrong would otherwise
    // send processors to an address nobody chose.
    constexpr std::uint64_t highest_start_up_page = 0xff;
    if (memory & (page_size - 1)) {
        log("start-up memory at {} is not page aligned", memory);
        return;
    }
    if ((memory >> 12) > highest_start_up_page) {
        log("start-up memory at {} is not below one megabyte", memory);
        return;
    }

    // Mapped into the host page table, because the trampoline is still
    // executing out of it at the moment it loads the host page table root
    // - the instruction after that load is fetched through this mapping.
    this->host_page_table.map_from(
        memory,
        arch::x86_64::ap_start_up_pages * page_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write |
            arch::x86_64::page_table::protection::execute,
        this->os_page_table);

    std::memcpy(reinterpret_cast<void *>(memory),
                arch::x86_64::zpp_ap_start_up_begin,
                blob_size);

    // The temporary page table, identity mapping the first gigabyte with
    // large pages. Only the trampoline's own few pages are ever touched
    // through it, but a gigabyte costs one page directory and removes the
    // question of what is reachable while it is live.
    constexpr std::uint64_t present_writable = 0x3;
    constexpr std::uint64_t large_page = 0x80;
    constexpr std::size_t entries_per_table =
        page_size / sizeof(std::uint64_t);

    auto level4 = memory + page_size;
    auto level3 = memory + (2 * page_size);
    auto level2 = memory + (3 * page_size);

    auto table = [](std::uint64_t address) {
        return reinterpret_cast<std::uint64_t *>(address);
    };

    for (std::size_t entry{}; entry < entries_per_table; ++entry) {
        table(level4)[entry] = 0;
        table(level3)[entry] = 0;
        table(level2)[entry] =
            (entry * large_page_size) | present_writable | large_page;
    }

    table(level4)[0] = level3 | present_writable;
    table(level3)[0] = level2 | present_writable;

    // Everything the trampoline cannot work out for itself. The stack and
    // the argument are per processor and filled in as each one is started.
    auto & area = *reinterpret_cast<arch::x86_64::ap_start_up_area *>(
        memory + arch::x86_64::ap_start_up_area_offset);
    area.host_cr3 = this->host_cr3;
    area.host_cr0 = this->host_cr0;
    area.host_cr4 = this->host_cr4;
    area.temporary_cr3 = level4;
    area.entry = reinterpret_cast<std::uint64_t>(zpp_ap_start_up_main);

    // The host descriptor tables, so the processor is running on them
    // before it reaches any of this VMM's C++ rather than after its first
    // VM exit. Without them a fault during the climb had no handler to
    // reach and escalated to a triple fault, which on a virtual machine
    // resets it - the failure this was written to stop being invisible.
    //
    // The GDT limit is the whole table, which is also what a VM exit
    // loads, so a fault before the first exit and one after it see the
    // same descriptors.
    area.host_gdtr.base = reinterpret_cast<std::uint64_t>(this->host_gdt);
    area.host_gdtr.limit = sizeof(this->host_gdt) - 1;
    area.host_idtr.base = this->host_idtr.base;
    area.host_idtr.limit =
        static_cast<std::uint16_t>(this->host_idtr.limit);
    area.host_cs = this->host_cs;

    this->start_up_memory = memory;
    log("start-up memory ready at {}, vector {}", memory, memory >> 12);
}

std::uint32_t hypervisor::start_up_trampoline_stage() const
{
    // Zero when there is no trampoline at all, which is a different answer
    // from a trampoline that never ran and has to stay distinguishable.
    if (!this->start_up_memory) {
        return 0;
    }

    using arch::x86_64::ap_start_up_area;
    auto & area = *reinterpret_cast<const ap_start_up_area *>(
        this->start_up_memory + arch::x86_64::ap_start_up_area_offset);
    return area.stage;
}

bool hypervisor::start_application_processor(std::size_t slot,
                                             std::uint64_t guest_vector)
{
    if (!this->start_up_memory || (slot >= max_cpus)) {
        return false;
    }

    // One processor at a time, which is what makes a single trampoline,
    // and the single stack below, enough. It also serializes the shared
    // state a launch walks through - the stack index, the virtual
    // processor counter and the VMX region pointers - none of which is
    // correct for two processors at once.
    this->start_up_lock.lock();
    scope_exit unlock{[&] { this->start_up_lock.unlock(); }};

    this->guest_start_up_vector[slot] = guest_vector;
    this->started_by_trampoline[slot] = true;
    this->start_up_launched[slot] = false;

    auto & area = *reinterpret_cast<arch::x86_64::ap_start_up_area *>(
        this->start_up_memory + arch::x86_64::ap_start_up_area_offset);

    // Put the trampoline's own working area back the way the assembler
    // left it, before every start and not just the first.
    //
    // The trampoline relocates three addresses by adding the page's base
    // to them in place, which is correct exactly once. A second processor
    // started from the same blob would add its base to values that already
    // hold one, producing a descriptor table base pointing at nothing -
    // and it faults on the far jump that follows, before it has any
    // interrupt descriptor table, so the machine resets rather than
    // reporting.
    //
    // Measured: with three processors to start, the firmware bootlooped
    // and printed no per-processor result at all, because the check starts
    // every target before polling any of them.
    std::memcpy(
        area.assembly_owned,
        arch::x86_64::zpp_ap_start_up_begin +
            arch::x86_64::ap_start_up_area_offset +
            offsetof(arch::x86_64::ap_start_up_area, assembly_owned),
        sizeof(area.assembly_owned));

    area.argument = slot;
    area.stack_top =
        reinterpret_cast<std::uint64_t>(std::end(this->start_up_stack));

    // The vector is the trampoline page's page number, which is the whole
    // reason that page had to be below one megabyte.
    constexpr std::uint64_t delivery_mode_start_up = (6ull << 8);
    constexpr std::uint64_t destination_shift = 32;
    arch::x86_64::wrmsr(arch::x86_64::msr::ia32_x2apic_icr,
                        (this->start_up_memory >> 12) |
                            delivery_mode_start_up |
                            (this->apic_id[slot] << destination_shift));

    // Bounded, so that a processor which never arrives costs a delay
    // rather than the machine. Everything it has to do between the IPI and
    // reporting in is a few thousand instructions, so this is generous by
    // orders of magnitude.
    constexpr std::uint32_t launch_wait_attempts = 2000000;
    for (std::uint32_t attempt{}; attempt < launch_wait_attempts;
         ++attempt) {
        if (this->start_up_launched[slot]) {
            log("cpu {} came up on the trampoline after {} attempts, "
                "guest vector {}",
                slot,
                attempt,
                guest_vector);
            return true;
        }
        zpp::spin_hint();
    }

    // The stage is what makes this diagnosable, and it separates the two
    // failures that look identical from here. Zero means the trampoline
    // never ran a single instruction, so the start-up IPI above was never
    // acted on, and the question is then what state the target was in
    // rather than anything about this VMM's own code. That is the whole
    // assumption this path rests on and never checks: SDM 11.4.2 has an
    // application processor "enter a wait-for-SIPI state" on any INIT
    // after the MP protocol has completed, and the sequence in Table 11-1
    // starts one from there. A target somewhere else is not startable this
    // way and nothing here can tell that it is, because an activity state
    // can only be read on the processor holding it.
    //
    // Anything but zero means it did run and died on the climb, at a stage
    // that says where.
    log("cpu {} did not come up after its start-up ipi, apic id {}, "
        "trampoline stage {}",
        slot,
        this->apic_id[slot],
        start_up_trampoline_stage());
    this->started_by_trampoline[slot] = false;
    return false;
}

void hypervisor::start_up_on_this_processor(std::uint64_t slot)
{
    // Twice returning, the same trick the launch and VM exit paths here
    // use: the first arrival goes on to launch, and a second one is that
    // launch having failed and come back. There is nowhere on this
    // processor to return to - it was started by an IPI, not called - so
    // the only thing left to do with it is stop it.
    std::atomic<bool> launch_returned;
    launch_returned = false;

    arch::x86_64::context context{};
    arch::x86_64::capture_context(&context);

    if (launch_returned) {
        log("cpu {} failed to launch", slot);
        for (;;) {
            arch::x86_64::halt();
        }
    }
    launch_returned = true;

    // What main reads out of the context: which processor this is, and no
    // physical to virtual translation, which only the boot processor's
    // once per boot setup ever calls.
    context.rdi = slot;
    context.rsi = 0;
    context.rdx = 0;

    launch_on_cpu(context);
}

void hypervisor::apply_start_up(arch::x86_64::context & context,
                                std::uint64_t vector)
{
    auto & vmcs = this->vmcs;

    using segment_descriptor = arch::x86_64::segment_descriptor;

    // A second start-up IPI for a processor already started is ignored.
    // INIT-SIPI-SIPI sends two, and the second would otherwise send a
    // processor that is already running back to its entry point - which
    // wedges it in a way indistinguishable from never having started.
    // Guarded here rather than relying on the hardware to discard the
    // second one, which it does not do reliably.
    if (auto cpu = vmcs.vpid() - 1; cpu < max_cpus) {
        if (this->started_by_start_up_ipi[cpu]) {
            return;
        }
        this->started_by_start_up_ipi[cpu] = true;

        // The hand-off is over, however it arrived. Cleared here because
        // this is the one place both paths end up, and leaving a delivered
        // vector behind would let the next INIT find a start-up nobody
        // sent this time.
        this->start_up_handoff[cpu].store(start_up_handoff_state::none);
    }

    // Everything below is the state an INIT leaves behind, applied here
    // rather than in the INIT handler because here there is time. SDM
    // Table 12-1, "IA-32 and Intel 64 Processor States Following Power-up,
    // Reset, or INIT", INIT column.
    constexpr std::uint64_t rflags_after_init = 0x2;
    constexpr std::uint64_t dr6_after_init = 0xffff0ff0;
    constexpr std::uint64_t dr7_after_init = 0x400;
    constexpr std::uint64_t cr4_after_init = 0;
    constexpr std::uint64_t real_mode_segment_limit = 0xffff;
    constexpr std::uint64_t descriptor_table_limit_after_init = 0xffff;

    // SDM Table 12-1 gives 0x60000010 in the CR0 row's INIT column, but
    // footnote 2 on that row qualifies it: "The CD and NW flags are
    // unchanged, bit 4 is set to 1, all other bits are cleared." The
    // 0x60000010 is the power-up value, where CD and NW happen to be set -
    // taking it literally for an INIT would disable this processor's
    // caches for the rest of its life.
    constexpr std::uint64_t preserved_across_init =
        arch::x86_64::cr0_bits::cache_disable |
        arch::x86_64::cr0_bits::not_write_through;
    auto cr0_after_init = arch::x86_64::cr0_bits::extension_type |
                          (vmcs.guest_cr0() & preserved_across_init);

    // The two bits VMX will not let a guest clear: CR0.NE and CR4.VMXE are
    // required to be set by IA32_VMX_CR0_FIXED0 and IA32_VMX_CR4_FIXED0
    // (SDM A.7 and A.8), and unrestricted guest exempts only PE and PG -
    // so a literally architectural CR0 and CR4 would fail VM entry. The
    // architectural values go into the read shadows, which is where a
    // guest looks once those bits are owned by the host.
    constexpr std::uint64_t cr0_never_clear =
        arch::x86_64::cr0_bits::numeric_error;
    constexpr std::uint64_t cr4_never_clear =
        arch::x86_64::cr4_bits::vmx_enable;

    // A real mode segment: sixteen bit, byte granular, limit 0xffff. The
    // access rights the VMCS wants are the descriptor's, so they are built
    // out of a descriptor rather than written as a number.
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

    vmcs.guest_cr0(cr0_after_init | cr0_never_clear);
    vmcs.cr0_read_shadow(cr0_after_init);
    vmcs.guest_cr3(0);
    vmcs.guest_cr4(cr4_after_init | cr4_never_clear);
    vmcs.cr4_read_shadow(cr4_after_init);

    // Long mode is gone with CR0.PG, and the entry control has to agree or
    // VM entry fails its consistency checks.
    //
    // The guest IA32_EFER field is deliberately not written. Without the
    // "load IA32_EFER" VM-entry control - which this VMCS does not set -
    // the field is ignored, and VM entry instead loads EFER.LMA from the
    // control cleared below and leaves LME alone when CR0.PG is being
    // loaded as zero, which it is here. Writing the field would look like
    // it cleared EFER when it does nothing at all.
    vmcs.vm_entry_controls(
        vmcs.vm_entry_controls() &
        ~arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);

    vmcs.guest_rflags(rflags_after_init);
    vmcs.guest_rsp(0);
    vmcs.guest_dr7(dr7_after_init);

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

    vmcs.guest_pending_debug_exceptions(0);

    // The general purpose registers are architecturally defined after an
    // INIT too, and they are not in the VMCS - they live in the context
    // this VMM saved on the way in and restores on the way out. Same
    // table: EAX zero, EDX the family, model and stepping, the rest zero.
    std::uint32_t identification[4]{};
    arch::x86_64::cpuid(1, 0, identification);

    context.rax = 0;
    context.rbx = 0;
    context.rcx = 0;
    context.rdx = identification[0];

    // Zeroed for the same reason as the rest, and load bearing on the
    // launch path: vm_launch takes the guest's RIP and RSP from this
    // context rather than from the VMCS, so leaving either holding where
    // this VMM happened to be would start the guest there instead of at
    // its entry point. On the VM exit path both are overwritten again
    // before the resume, so this costs nothing there.
    context.rip = 0;
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

    // DR6 is not a VMCS guest field - the guest and host share the
    // register - so the architectural value has to be written to the real
    // one while running on this processor.
    arch::x86_64::dr6(dr6_after_init);

    // SDM 12.1: during an INIT "the TLBs and BTB are invalidated as with a
    // hardware reset", and the same paragraph describes INIT as the method
    // for "switching from protected to real-address mode" - exactly the
    // transition just made. With VPID enabled the processor tags its
    // cached translations, so they survive it and must be invalidated by
    // hand. Single-context, so other processors are left alone.
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

    // And now where the start-up IPI says to begin. Shifts that turn the
    // vector into a segment: the vector is a page number, so the segment
    // is the vector scaled by a page, and the selector is that base
    // shifted down by the four bits real mode already implies. Matches
    // KVM's kvm_vcpu_deliver_sipi_vector(), which sets the same three
    // fields and nothing else.
    constexpr std::uint64_t vector_to_selector_shift = 8;
    constexpr std::uint64_t vector_to_base_shift = 12;

    vmcs.guest_cs_selector(vector << vector_to_selector_shift);
    vmcs.guest_cs_base(vector << vector_to_base_shift);
    vmcs.guest_cs_limit(real_mode_segment_limit);
    vmcs.guest_cs_access_rights(code_access_rights);
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

    // Say it into the log as well as into the record.
    //
    // The record above says what the state *was* and only for the last
    // exit of its kind; the log says what happened, in order, across
    // processors. A boot failure is a sequence, and the record keeps
    // only its final frame.
    //
    // Then empty the ring, because this processor is about to stop and
    // anything still buffered dies with it. The trickle in the exit path
    // is sized for a running guest; here there is no guest left to
    // delay, so the whole ring goes.
    diag::log<diag::severity::error>(
        "unhandled exit {} qualification {} rip {} cs {}",
        record.reason,
        record.qualification,
        record.guest_rip,
        record.guest_cs_selector);
    diag::pump::drain();

    // Everything above went into members, which only a debugger attached
    // to this CPU can read - and this CPU is about to stop, so on bare
    // metal nothing ever reads them. The log is what survives a restart,
    // so the record goes into it too rather than only into the members.
    log("stopping, unhandled exit reason {} qualification {} rip {} "
        "cs {} linear {}",
        reason.value(),
        record.qualification,
        record.guest_rip,
        record.guest_cs_selector,
        record.guest_linear_address);

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

    // Same reasoning as the unhandled exit path: the record keeps the
    // last frame, the log keeps the sequence, and this processor is
    // about to stop so nothing buffered survives unless it goes now.
    diag::log<diag::severity::error>(
        "vm entry failure {} rip {}", record.reason, record.guest_rip);
    diag::pump::drain();

    // Into the log as well as the members, for the reason given in
    // on_unhandled_exit: the members need a debugger on a CPU that is
    // about to stop, and the log outlives the restart.
    log("stopping, vm entry failed, reason {} instruction error {} "
        "activity {} rip {} cr0 {} cr4 {} rflags {}",
        reason.value(),
        record.instruction_error,
        record.activity_state,
        record.guest_rip,
        record.guest_cr0,
        record.guest_cr4,
        record.guest_rflags);

    // Stop. This CPU is not going to run a guest again, and pretending
    // otherwise is what made this failure invisible before.
    for (;;) {
        arch::x86_64::disable_interrupts();
        arch::x86_64::halt();
    }
}

std::expected<void, zpp::error> hypervisor::enter_root_mode()
{
    // Into the state VMX requires, each behind a guard: every step below
    // can fail, and a failure has to leave the loader the machine it was
    // still running on.
    auto cr0 = arch::x86_64::cr0();
    auto cr4 = arch::x86_64::cr4();

    arch::x86_64::cr0(this->host_cr0);
    scope_exit restore_cr0{[&] { arch::x86_64::cr0(cr0); }};

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

    if (arch::x86_64::vmx::vmxon(&this->vmx_physical)) {
        return std::unexpected(zpp::error{error::vmxon_failed});
    }
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    // VMCLEAR is the only thing that sets the launch state to clear, and
    // VMLAUNCH requires clear (SDM 27.1). The state lives in the region
    // itself and cannot be read back, so it has to be set here.
    if (arch::x86_64::vmx::vmclear(&this->vmcs_physical)) {
        return std::unexpected(zpp::error{error::vmclear_failed});
    }

    if (arch::x86_64::vmx::vmptrld(&this->vmcs_physical)) {
        return std::unexpected(zpp::error{error::vmptrld_failed});
    }

    // Released, not run: from here the processor stays in VMX operation
    // with these registers, which is what this function is for. main
    // takes over the undoing.
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

    // All ones is the "no linked VMCS" value. Any other value is taken
    // as the address of a shadow VMCS and checked as one on VM entry,
    // SDM 29.3.1.5 - zero would name physical page zero.
    vmcs.vmcs_link_pointer(0xffffffffffffffffull);

    // Must be non-zero with VPID enabled (SDM 29.2.1.1), and it doubles
    // as this VMM's processor index - hence counting from one.
    vmcs.vpid(this->next_virtual_processor);

    arch::x86_64::vmx::ept_pointer eptp;
    eptp.memory_type(arch::x86_64::memory_type::write_back);
    eptp.page_walk_length(4);
    eptp.page_number(this->epml4_physical >> 12);
    vmcs.ept_pointer(eptp);

    // All three bitmaps are shared by every VMCS, which is what lets
    // intercept_interrupt_command and intercept_io_port be called once
    // on the boot processor and take effect everywhere.
    vmcs.msr_bitmap(this->msr_bitmap_physical);
    vmcs.write(arch::x86_64::vmx::vmcs::field::io_bitmap_a,
               this->io_bitmap_a_physical);
    vmcs.write(arch::x86_64::vmx::vmcs::field::io_bitmap_b,
               this->io_bitmap_b_physical);

    // What each control field below spells out is a request, not the
    // value written: adjust_msr forces the bits the capability MSR
    // requires, and a field that disagrees with it fails VM entry.
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

    // Nothing requested: external interrupts and NMIs stay the guest's,
    // which owns the interrupt controller.
    // NMI exiting, so that a non-maskable interrupt this VMM sends takes
    // the target processor out of whatever it is doing - including a halt,
    // which nothing else available here can do. The cost is that a genuine
    // NMI from the guest's world arrives here too and has to be handed
    // back; see the exception_or_nmi case in the exit handler.
    vmcs.pin_based_vm_execution_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_pin_based_controls),
        arch::x86_64::vmx::vm_execution_controls::pin::nmi_exiting));

    vmcs.primary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_processor_based_controls),
            arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_secondary_controls |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_msr_bitmaps |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_io_bitmaps |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    mwait_exiting |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    monitor_exiting));

    // The host runs in 64-bit mode after an exit, and DR7 and
    // IA32_DEBUGCTL are saved on the way out so the guest gets back what
    // it had rather than what the host was using.
    vmcs.vm_exit_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_exit_controls),
        arch::x86_64::vmx::vm_exit_controls::host_address_space_size |
            arch::x86_64::vmx::vm_exit_controls::save_debug_controls));

    // The mirror on entry. apply_start_up clears ia_32e_mode_guest
    // again, since it has to agree with CR0.PG or entry fails.
    vmcs.vm_entry_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_entry_controls),
        arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest |
            arch::x86_64::vmx::vm_entry_controls::load_debug_controls));

    // The selectors below are resolved against the intermediate GDT
    // rather than the OS one, which is not mapped here any more.
    auto intermediate_gdt_base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory
            .intermediate_gdt[this->next_virtual_processor - 1]);

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
    // From the MSR, not the descriptor. In long mode the FS base does
    // not live in the descriptor at all - it lives in IA32_FS_BASE, and
    // a descriptor's base field is thirty two bits, so taking it from
    // there truncates any base above four gigabytes to whatever the low
    // half happens to be.
    //
    // GS a few lines below has always been read from its MSR. FS was
    // not, and the asymmetry is the tell: the same mistake was found
    // once and fixed in one of the two places.
    //
    // It has not bitten because 64 bit Windows keeps its per-processor
    // block in GS and leaves FS largely unused. A guest that does use it
    // would resume with a base silently different from the one it set.
    vmcs.guest_fs_base(this->ia32_fs_base);
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

    // The guest keeps its own tables; the host gets ones inside the
    // module, which is all the host page table maps. A host fault
    // delivered through an unmapped table would escalate.
    vmcs.guest_gdtr_limit(this->guest_gdt_limit);
    vmcs.guest_gdtr_base(
        reinterpret_cast<std::uint64_t>(this->guest_gdt_pointer));
    vmcs.host_gdtr_base(reinterpret_cast<std::uint64_t>(this->host_gdt));

    vmcs.guest_idtr_limit(this->idtr.limit);
    vmcs.guest_idtr_base(this->idtr.base);
    vmcs.host_idtr_base(reinterpret_cast<std::uintptr_t>(this->host_idt));

    // The CR0 shadow does nothing: CR0's guest/host mask is never set,
    // so the guest reads the real register. Same dead shadow the CR4
    // block below describes - setting a mask here would be needed first.
    vmcs.cr0_read_shadow(this->guest_cr0);
    vmcs.guest_cr0(this->host_cr0);
    vmcs.host_cr0(this->host_cr0);

    // The guest keeps the OS page table and the host runs on its own.
    // CR3-load exiting is not set, so guest writes to CR3 are never seen
    // here - which is what EPT is for.
    vmcs.guest_cr3(this->guest_cr3);
    vmcs.host_cr3(this->host_cr3);

    // Load CR4.
    //
    // The mask is what makes the read shadow mean anything: a shadow
    // only answers for the bits set here, and with the mask left at its
    // default of zero - which it was - every bit came from the real
    // register and the shadow above was dead code.
    //
    // VMXE is the bit that matters. CPUID leaf 1 reports no VMX, because
    // otherwise the guest's own hypervisor launches ahead of ours and
    // faults on its own vmxon. A guest that then reads CR4 and finds
    // VMXE set has been told two contradictory things, and the
    // combination exists on no real processor. Which one it believes is
    // its choice, and one of the choices ends in a #GP it cannot
    // explain.
    //
    // So: the guest sees VMXE clear, and any attempt to write the bit
    // exits to us rather than reaching the register we need it in.
    constexpr std::uint64_t cr4_vmxe = 1ull << 13;
    vmcs.cr4_guest_host_mask(cr4_vmxe);
    vmcs.cr4_read_shadow(this->guest_cr4 & ~cr4_vmxe);
    vmcs.guest_cr4(this->host_cr4);
    vmcs.host_cr4(this->host_cr4);

    // These take effect only because "load debug controls" is set in the
    // entry controls above; without it VM entry ignores both fields.
    vmcs.guest_ia32_debugctl(this->ia32_debug_control);
    vmcs.guest_dr7(this->guest_dr7);

    vmcs.guest_rflags(guest_context.rflags);
}

template <typename VmmCode>
void hypervisor::vm_launch(arch::x86_64::context & guest_context,
                           VmmCode && vmm_code)
{
    auto & vmcs = this->vmcs;

    // Allocate a small stack for the host VM exit.
    alignas(0x10) unsigned char host_vm_launch_stack[0x1500]{};

    // Two contexts below the top, because the top is what they occupy:
    // vm_exit_entry captures the guest's registers at host_rsp, and the
    // host's own captured context sits immediately above it.
    auto host_rsp = reinterpret_cast<std::uint64_t>(
        std::end(host_vm_launch_stack) -
        (2 * sizeof(arch::x86_64::context)));

    // Named so the exit path has something typed to hand to vmm_code;
    // what fills it is the exit stub, not this.
    auto & local_guest_context =
        *::new (reinterpret_cast<void *>(host_rsp)) arch::x86_64::context;

    auto & host_context = *::new (reinterpret_cast<void *>(
        host_rsp + sizeof(arch::x86_64::context))) arch::x86_64::context;

    // A VM exit arrives with no usable register state, so it lands in a
    // naked stub rather than in C++.
    vmcs.host_rip(
        reinterpret_cast<std::uint64_t>(arch::x86_64::vm_exit_entry));
    vmcs.host_rsp(host_rsp);

    // The caller's own captured RIP and RSP, so the guest is the loader
    // continuing from the call that got here.
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

    arch::x86_64::capture_context(&host_context);

    // The second arrival here is a VM exit rather than the fall through
    // from the capture - the same twice-returning trick main uses.
    if (vm_exit_flag) {
        // Call VMM code, which never returns.
        vmm_code(local_guest_context);

        // Error: vmm_code returned.
        return;
    }

    // The capture took the caller's selectors, which mean something else
    // in the host GDT. Overwritten with what VM exit itself loads.
    host_context.cs = this->host_cs;
    host_context.ds = 0;
    host_context.es = 0;
    host_context.fs = 0;
    host_context.gs = 0;
    host_context.ss = 0;

    // Everything indexed by next_virtual_processor - 1 above belongs to
    // this processor now, so the next one to launch takes the next slot.
    ++this->next_virtual_processor;

    // The next time we arrive after the capture context is due to VM
    // exit.
    vm_exit_flag = true;

    guest_context.rip =
        reinterpret_cast<std::uint64_t>(arch::x86_64::vmx::vmlaunch);

    // Set rflags to host rflags, to leave interrupts disabled.
    guest_context.rflags = host_context.rflags;

    // The value the loader will see: VM entry does not load the general
    // purpose registers, so RAX carries into the guest, which resumes at
    // the loader's return address.
    guest_context.rax = 0;

    // restore_context is the launch - RIP was pointed at vmlaunch above.
    arch::x86_64::restore_context(&guest_context);
}

std::expected<void, zpp::error>
hypervisor::main(arch::x86_64::context & caller_context)
{
    // Fetch parameters. The second argument is a pointer to everything
    // the loader hands over, so this grows by reading another field
    // rather than by another register and another adapter.
    auto cpuid = caller_context.rdi;

    // Everything the loader hands over is copied in, never referred to
    // where it lies. Nothing outside this module is mapped into the
    // hypervisor, so a pointer that leads out of it is only good for as
    // long as somebody else's page table is in force - and this
    // processor switches to the host page table part way through this
    // function. That table maps the module and very little else, so
    // every one of the loader's addresses stops resolving at that point.
    //
    // Reading a field after it faults: #PF, error code zero, CR2 in the
    // loader's pool, from a page directory pointer entry that was never
    // filled in. That is not hypothetical - it is what the version of
    // this code that kept the pointer and read through it actually did.
    //
    // Copying the whole block rather than each field in turn is what
    // makes that structural: a field added later is safe by
    // construction, instead of being safe only if whoever added it
    // noticed this. A zeroed copy also stands in for an absent block, so
    // nothing below needs to null check.
    zpp_launch_parameters launch{};
    auto launch_given = false;
    if (auto given = reinterpret_cast<const zpp_launch_parameters *>(
            caller_context.rsi)) {
        launch = *given;
        launch_given = true;
    }

    auto physical_to_virtual = launch.physical_to_virtual;
    auto start_up_memory =
        reinterpret_cast<std::uint64_t>(launch.start_up_memory);

    // Only when there was a block to read them out of.
    //
    // This function runs again on every processor this VMM starts, and
    // such a processor has no launch block - start_up_on_this_processor
    // sets rsi to zero deliberately, because only the boot processor's
    // once per boot setup needs anything out of it. The zeroed stand-in
    // above is the right answer for reading a *local*, and the wrong one
    // for writing shared state: it would overwrite what the boot
    // processor established with nothing.
    //
    // That is not hypothetical. These two are read from inside the I/O
    // exit handler, and the interception they belong to stays armed in
    // the bitmap once set. Zeroing them left the port still trapping and
    // no longer recognised, so the guest's next access to it became an
    // unhandled exit and stopped the processor - with the rest of the
    // guest's processors spinning behind it, which is what a boot that
    // hangs with the disk channel enabled looked like.
    if (launch_given) {
        this->sleep_control_port = launch.sleep_control_port;
        this->sleep_control_port_secondary =
            launch.sleep_control_port_secondary;
    }

    // The block copied above still holds one pointer that leads out of
    // this module, so it gets the same treatment one level down. Keeping
    // it would move the fault described above one dereference later,
    // into the hand-over's own fields, where it would be no easier to
    // read.
    nvme::channel_handover diagnostic_channel{};
    bool diagnostic_channel_given = false;
    if (launch.diagnostic_channel) {
        diagnostic_channel = *static_cast<const nvme::channel_handover *>(
            launch.diagnostic_channel);
        diagnostic_channel_given = true;
    }

    // Whether this processor was started by this VMM rather than launched
    // by the loader, which changes three things below: there is no state
    // of its own worth capturing, there is no OS descriptor table to build
    // an intermediate copy of, and its guest begins at the vector a
    // start-up IPI named rather than where a caller was.
    auto from_trampoline =
        (cpuid < max_cpus) && this->started_by_trampoline[cpuid];

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

    // The only way to read the OS page table from here: its levels are
    // named by physical address and this module maps none of them.
    this->physical_to_virtual = physical_to_virtual;

    // Initialize special registers.
    //
    // Not on a processor this VMM started. What it holds is what an INIT
    // and this VMM's own trampoline left behind - the host page table root
    // in CR3, a descriptor table belonging to the trampoline - and none of
    // it describes an operating system. Capturing it would overwrite the
    // boot processor's record of the guest with values that are not the
    // guest's, and every one of these is shared rather than per processor.
    if (!from_trampoline) {
        initialize_registers();
    }

    // Record this processor's local APIC id, so that an intercepted
    // interrupt command register write naming it as the destination can be
    // matched back to an index here.
    //
    // From CPUID, unconditionally. Reading the APIC's own identifier
    // register instead does not work here and cannot be made to: it is an
    // MSR only in x2APIC mode, and this firmware is in xAPIC mode when the
    // hypervisor launches, so an unconditional read raised #GP and took
    // the boot down. Guarding that read on x2APIC being enabled stopped
    // the fault but recorded nothing at all - the table stayed zero, and a
    // start-up IPI's destination then matched the boot processor's slot by
    // accident. Guests enable x2APIC after this VMM is already resident,
    // so there is no ordering in which the MSR would have worked.
    //
    // SDM 13.12.1, "Detecting and Enabling x2APIC Mode": "The local APIC
    // registers can be accessed via the MSR interface only when the local
    // APIC has been switched to the x2APIC mode."
    if (cpuid < max_cpus) {
        this->apic_id[cpuid] = local_apic_id();
    }

    // Perform only on first CPU load.
    if (0 == cpuid) {
        // The order is forced: the host page table covers the module
        // region and is built by reading the OS one, and the reverse
        // translation is read back off the host table.
        initialize_module_region();
        initialize_os_page_table();
        initialize_host_page_table();

        // The storage controller's registers, if a channel was handed
        // over.
        //
        // The channel is driven from inside VM exits: the writer reads
        // the controller's status before every write and rings a
        // doorbell after it, and both are memory mapped registers in the
        // controller's BAR. Nothing maps that BAR into the host page
        // table - it maps the module, itself, and the local APIC page -
        // so the first status read faults. Measured, not predicted: #PF
        // with CR2 at BAR0 + 0x1c, which is CSTS, taken in the exit
        // handler where there is no recovery point, so the processor
        // halts and the guest's other processors spin behind it.
        //
        // Here rather than where the sink is configured, for the same
        // reason the APIC page is mapped here: arming happens after this
        // processor has switched to this table, and map_from walks the
        // OS table through the loader's callback, which is one of the
        // addresses that stops resolving at that switch.
        //
        // Page granularity and one page per register address. The four
        // usually share one page - doorbells sit just past the register
        // block - so this is normally a single mapping done four times,
        // and map_from is idempotent.
        if (diagnostic_channel_given) {
            const volatile void * const registers[] = {
                diagnostic_channel.status_register,
                diagnostic_channel.configuration_register,
                diagnostic_channel.submission_doorbell,
                diagnostic_channel.completion_doorbell,
            };

            for (auto address : registers) {
                if (!address) {
                    continue;
                }
                auto page = reinterpret_cast<std::uint64_t>(address) &
                            ~(page_size - 1);
                this->host_page_table.map_from(
                    page,
                    page_size,
                    arch::x86_64::page_table::protection::read |
                        arch::x86_64::page_table::protection::write,
                    this->os_page_table);
                log("mapped controller register page {}", page);
            }

            // And the queue memory, which is ordinary RAM rather than
            // device registers but is just as unreachable: the loader
            // allocated it, so it is outside the module and outside
            // everything this table maps. The writer stores commands
            // into it on the way to ringing a doorbell, so without this
            // the fault simply moves from the status read to the store.
            if (diagnostic_channel.queue_storage) {
                auto base = reinterpret_cast<std::uint64_t>(
                    diagnostic_channel.queue_storage);
                for (std::uint64_t offset{};
                     offset < nvme::queue_pair<64>::storage_bytes;
                     offset += page_size) {
                    this->host_page_table.map_from(
                        base + offset,
                        page_size,
                        arch::x86_64::page_table::protection::read |
                            arch::x86_64::page_table::protection::write,
                        this->os_page_table);
                }
                log("mapped queue storage at {}", base);
            }
        }

        if (auto result = initialize_module_physical_to_virtual();
            !result) {
            return result;
        }

        initialize_host_gdt();

        // Initialize host IDT, which needs the GDT above to exist.
        initialize_host_idt();

        // Start intercepting the interrupt command register, before any
        // other processor is launched. The MSR bitmap is shared by every
        // VMCS, so this is done once.
        intercept_interrupt_command(true);

        // A suspend takes this VMM away and nothing brings it back, so
        // the least that can be done is notice. Armed only if the loader
        // found the register; the guest keeps every other port.
        if (this->sleep_control_port) {
            intercept_io_port(this->sleep_control_port, true);
            if (this->sleep_control_port_secondary) {
                intercept_io_port(this->sleep_control_port_secondary,
                                  true);
            }
            log("watching sleep control port {}",
                this->sleep_control_port);
        }
    }

    // Initialize and load the intermediate GDT, which is a copy of the
    // OS one taken while the OS page tables are still live.
    //
    // Skipped on a processor this VMM started, for the reason it cannot be
    // done there: the copy is read from the OS descriptor table, which the
    // host page table does not map, and this processor is already running
    // on the host page table. It does not need one either - the
    // intermediate GDT exists to stay valid across the page table switch
    // below, and this processor has no switch to make.
    if (!from_trampoline) {
        initialize_intermediate_gdt();
        load_intermediate_gdt();
    }

    // Guard to restore GDT, on the processors that had one to replace.
    scope_exit restore_gdt{[&] {
        if (!from_trampoline) {
            load_os_gdt();
        }
    }};

    // Switch page tables. Already the case on a processor this VMM
    // started - its trampoline loaded the host page table to get here -
    // and writing the same value again is harmless.
    arch::x86_64::cr3(this->host_cr3);

    // Guard to restore cr3, on the processors that had one to replace. A
    // processor this VMM started has never been on the OS page table and
    // has no business being put there.
    scope_exit restore_cr3{[&] {
        if (!from_trampoline) {
            arch::x86_64::cr3(this->guest_cr3);
        }
    }};

    // The OS IDT lives in memory the host page table does not map, so with
    // the switch above done IDTR names pages that are no longer there.
    // Every exception would fault again while being delivered and escalate
    // to a triple fault, taking the machine down with no diagnosis. Load
    // the host IDT, which is inside the module and therefore mapped.
    load_host_idt();

    // Guard to restore the OS IDT, on the processors that had one. The OS
    // IDT is not mapped in the host page table, so loading it on a
    // processor this VMM started would arm an IDTR pointing at nothing.
    scope_exit restore_os_idt{[&] {
        if (!from_trampoline) {
            load_os_idt();
        }
    }};

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
        // Ordered too: the EPT derives its memory types from the MTRRs,
        // the protection edits the EPT's entries, and unprotecting
        // punches a hole back in that protection.
        initialize_vmx_msrs();
        initialize_mtrrs();

        if (auto result = initialize_ept(); !result) {
            return result;
        }

        if (auto result = protect_module(); !result) {
            return result;
        }

        // And the queue storage, which protect_module covered for free
        // while it was an array inside the module and does not cover now
        // that the loader allocates it separately. A guest able to write
        // the submission queue dictates what the controller executes, so
        // this is not tidiness.
        //
        // Here and not beside the mapping further up, for the reason the
        // local APIC watch is down here too: editing an extended page
        // table entry before initialize_ept has built the tables edits
        // nothing, and doing it between that and this point would be
        // undone by initialize_ept itself. This is the first place the
        // tables are final.
        if (diagnostic_channel_given && diagnostic_channel.queue_storage) {
            auto storage = reinterpret_cast<std::uint64_t>(
                diagnostic_channel.queue_storage);
            if (auto hidden = protect_region(
                    storage, nvme::queue_pair<64>::storage_bytes);
                !hidden) {
                log("could not hide the queue storage");
            }
        }

        // Prove the temporary mapping window before anything depends on
        // it.
        //
        // It is the primitive that will let the borrow reach the guest's
        // admin queue, whose address this VMM does not choose, and it is
        // exactly the kind of thing that appears to work while returning
        // the previous caller's page. So it is pointed at a page whose
        // first bytes are known - the module's own base, which begins
        // with the ELF magic - and the answer is checked rather than
        // assumed.
        {
            this->mapping_window_lock.lock();
            scope_exit release{
                [&] { this->mapping_window_lock.unlock(); }};
            auto module_physical =
                this->host_page_table.virtual_to_physical(
                    this->module_base);
            auto * through = static_cast<const unsigned char *>(
                map_window(module_physical));

            auto correct = (0x7f == through[0]) && ('E' == through[1]) &&
                           ('L' == through[2]) && ('F' == through[3]);
            this->mapping_window_verified = correct ? 1 : 2;
            if (correct) {
                log("mapping window reads the module base correctly");
            } else {
                log("mapping window is wrong");
            }
        }

        unprotect_guest_memory();

        // Interception of the interrupt command for a guest that is not
        // in x2APIC mode, where the command is a store to a page rather
        // than an MSR write and the bitmap set up above cannot see it.
        // Armed only while that is actually the mode, because the page
        // is hot.
        //
        // Here, and not beside the MSR bitmap it complements, because
        // arming a watch edits an extended page table entry. Done
        // before initialize_ept() there is no table to edit; done
        // between it and unprotect_guest_memory() the entry is written
        // and then overwritten, and the watch is silently lost. This is
        // the first point at which the tables are final.
        watch_local_apic(!x2apic_enabled());

        // Take the diagnostic channel the loader established, if it
        // established one.
        //
        // Here rather than earlier because the sink needs a way to turn
        // one of its own buffers into the address a controller will use,
        // and that is the host page table - which only exists by this
        // point. Before it, the identity the loader was relying on has
        // stopped being true and nothing has replaced it yet.
        //
        // Everything about the channel is checked on the other side of
        // this call: a null pointer, a wrong magic, an unresolved
        // target, a missing doorbell. A hand-over that does not check
        // out leaves the sink not ready, and a sink that is not ready is
        // never offered a record.
        // The runtime condition inside a compile time one. Naming a
        // static member of a class template odr-uses it, so the plain
        // form put the channel's queues, staging buffer and code into
        // builds that have it switched off - which is what
        // scripts/ci/check-diag-absent.sh exists to catch.
        if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
            if (diagnostic_channel_given) {
                auto & handover = diagnostic_channel;

                // A captureless lambda, which converts to the plain
                // function pointer the sink takes. Local to its one use
                // rather than a member, since nothing else needs it and
                // this VMM is a singleton anyway.
                //
                // The host page table is the only correct answer once
                // resident: the identity the loader relied on stops being
                // true the moment CR3 changes.
                auto physical_of = [](const void * address) {
                    return instance().host_page_table.virtual_to_physical(
                        address);
                };

                if (diag::esp_block_sink::configure(handover,
                                                    physical_of)) {
                    // Watch the page the configuration register lives on,
                    // so a guest reset of the controller is noticed rather
                    // than inferred - it cannot be inferred, because a
                    // completed reset leaves the status register looking
                    // exactly as it did before.
                    //
                    // This page and not the doorbell page. With a stride
                    // of zero the doorbells begin at the next page, and
                    // they are written on every command the guest issues;
                    // watching them would trap the guest's entire disk
                    // traffic. The registers on this page are touched
                    // while a driver sets itself up and almost never
                    // afterwards.
                    auto register_page =
                        reinterpret_cast<std::uint64_t>(
                            handover.configuration_register) &
                        ~(page_size - 1);
                    if (auto armed = watch_guest_page_writes(
                            register_page,
                            &hypervisor::on_controller_register_write,
                            this);
                        !armed) {
                        log("could not watch the controller register "
                            "page");
                    }

                    // Everything the rebuild will need after a reset has
                    // taken the binding away, captured while it is still
                    // there. The loader that supplied it is gone by then
                    // and the sink will have forgotten it, which is the
                    // point of forgetting.
                    auto * registers =
                        static_cast<volatile std::uint8_t *>(
                            const_cast<void *>(
                                handover.configuration_register));
                    this->channel_bar =
                        registers -
                        nvme::offset_of(
                            nvme::register_offset::configuration);
                    this->channel_queue_id = handover.submission_id;
                    this->channel_namespace = handover.namespace_id;
                    this->channel_controller_enabled = true;

                    auto capabilities =
                        nvme::controller_capabilities{arch::x86_64::read64(
                            static_cast<volatile std::uint8_t *>(
                                this->channel_bar) +
                            nvme::offset_of(
                                nvme::register_offset::capabilities))};
                    this->channel_doorbell_stride =
                        capabilities.doorbell_stride();

                    this->channel_submission_physical = physical_of(
                        diag::esp_block_sink::queues::submissions);
                    this->channel_completion_physical = physical_of(
                        diag::esp_block_sink::queues::completions);

                    log("disk channel live, namespace {}",
                        handover.target.namespace_id);
                    diag::log<diag::severity::info>(
                        "disk channel live on namespace {}",
                        handover.target.namespace_id);
                } else {
                    log("disk channel refused the loader's hand-over");
                }
            }
        }
    }

    initialize_vmx();

    // Lay out the memory a processor this VMM starts begins executing in.
    //
    // Here rather than with the rest of the once per boot setup above,
    // because it needs the host control registers, and initialize_vmx is
    // what works those out. Failure is not fatal and does not return an
    // error: it means processors cannot be started by this VMM, which
    // matters only where the loader is not launching it on them.
    if ((0 == cpuid) && start_up_memory) {
        initialize_start_up_memory(start_up_memory);
    }

    if (auto result = enter_root_mode(); !result) {
        return result;
    }

    // The guard enter_root_mode released, taken up again here: a failure
    // between now and the launch still has to leave VMX operation.
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    setup_vmcs(caller_context);

    // On a processor this VMM started, replace the guest state just built
    // with the state a processor holds after an INIT followed by a
    // start-up IPI, at the vector the guest asked for.
    //
    // This is the point of the whole exercise. The guest asked for this
    // processor to begin in real mode at its own trampoline; it gets
    // exactly that, and it is virtualized before the first of those
    // instructions runs. Everything setup_vmcs just wrote into the guest
    // fields described the boot processor's firmware state and is
    // overwritten here - it was only ever a starting point.
    if (from_trampoline && (cpuid < max_cpus)) {
        apply_start_up(caller_context, this->guest_start_up_vector[cpuid]);
    }

    // Recorded before the launch rather than after it, because the launch
    // does not return. From here this processor either runs the guest or
    // has stopped, and both are states the processor that started it needs
    // to be able to tell apart from still waiting.
    if (cpuid < max_cpus) {
        this->processor_virtualized[cpuid] = true;
        this->start_up_launched[cpuid] = true;
    }

    // Disarm the recovery point. It is only good while this frame is live,
    // and the launch below does not return - the guest resumes on the
    // caller's stack instead - so from here on a host exception has
    // nothing to unwind to and the handler stops the CPU rather than
    // jumping into a dead frame.
    this->host_exception_recovery_flag = nullptr;

    log("launching guest on virtual processor {}",
        this->next_virtual_processor);

    vm_launch(caller_context, [&](auto & context) {
        using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
        auto & vmcs = this->vmcs;

        // Notice a controller that has come back, if the write that
        // brought it back was not caught.
        //
        // It very often is not, and the reason is structural rather than
        // a bug to fix here. A watch lets the guest's write land by
        // making the page writable, stepping one instruction with the
        // monitor trap flag, and protecting it again - and for that
        // window the page is writable for every processor, not just the
        // one being stepped. A driver resetting a controller writes CC
        // twice in quick succession, disable then enable, and the second
        // write goes through the window left open by the first.
        //
        // Measured exactly so: one trapped write carrying 0x00460000,
        // the controller afterwards reading 0x00460001, and the
        // transition never seen.
        //
        // So the state is polled rather than the edge caught. It costs
        // one memory mapped read on exits where the channel is down and
        // nothing at all once it is back, and it cannot miss - a
        // controller that is enabled stays enabled to be found. Detection
        // lands within microseconds of the enable, which is still inside
        // the CSTS.RDY wait the driver is required to tolerate, so the
        // borrow is still free.
        if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
            if (this->channel_bar && !this->channel_controller_enabled) {
                auto configuration =
                    nvme::controller_configuration{arch::x86_64::read32(
                        static_cast<volatile std::uint8_t *>(
                            this->channel_bar) +
                        nvme::offset_of(
                            nvme::register_offset::configuration))};
                if (configuration.enable()) {
                    this->channel_controller_enabled = true;
                    rebuild_channel_queue();
                }
            }
        }

        // Catch this processor up with any extended page table change
        // another one has made.
        //
        // INVEPT is not a broadcast, so an entry changed elsewhere is
        // not changed here until this happens - see ept_generation. Done
        // on the way out of the guest rather than on the way in, because
        // the handlers below are what act on watches and they have to see
        // an armed one as armed.
        if (auto cpu = vmcs.vpid(); (0 != cpu) && (cpu <= max_cpus)) {
            auto generation =
                this->ept_generation.load(std::memory_order_acquire);
            if (this->ept_generation_seen[cpu - 1] != generation) {
                this->ept_generation_seen[cpu - 1] = generation;
                invalidate_ept_locally();
            }
        }

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

        // Out of the VMCS, because what the exit stub's capture left in
        // this field is its own return address, not the guest's RIP.
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
        case basic_reason::exception_or_nmi: {
            // Either a wake this VMM sent, or the guest's own.
            //
            // The stamp that a wake exists to collect has already
            // happened - the catch-up at the top of this handler runs
            // before any of these cases - so a wake needs nothing done
            // to it beyond being consumed.
            //
            // Anything else is the guest's, and is handed straight back.
            // Swallowing an NMI would lose a watchdog or a machine check
            // the guest was relying on, and the guest cannot tell that a
            // hypervisor took it.
            auto information = vmcs.vm_exit_interruption_information();
            constexpr std::uint32_t type_nmi = 2u << 8;
            constexpr std::uint32_t type_mask = 7u << 8;
            constexpr std::uint32_t valid = 1u << 31;

            auto is_nmi = ((information & type_mask) == type_nmi);
            auto cpu = vmcs.vpid();

            if (is_nmi && (0 != cpu) && (cpu <= max_cpus) &&
                this->wake_requested[cpu - 1].exchange(
                    false, std::memory_order_acq_rel)) {
                break;
            }

            if (is_nmi) {
                ++this->guest_nmis_reinjected;
                vmcs.vm_entry_interruption_information_field(
                    valid | type_nmi | 2u);
            }
            break;
        }

        case basic_reason::cpuid: {
            std::uint32_t cpuid_result[4]{};

            // The real instruction, whose answer is then edited - so
            // every bit this VMM has no opinion on is the hardware's.
            arch::x86_64::cpuid(context.rax, context.rcx, cpuid_result);

            // The leaf, which is EAX. The high half of RAX is not part
            // of it and real CPUID ignores it.
            auto leaf = static_cast<std::uint32_t>(context.rax);

            // The range reserved for hypervisor use. Nothing physical
            // answers here, so whatever a guest reads is whatever the
            // layer above it chose to say.
            constexpr std::uint32_t hypervisor_leaf_first = 0x40000000;
            constexpr std::uint32_t hypervisor_leaf_last = 0x4fffffff;

            // Reports a given processor's most recent exit, selected by
            // ecx. Inside the range this VMM already owns, so it costs no
            // new interface and nothing underneath can answer it instead.
            constexpr std::uint32_t diagnostic_leaf = 0x40000001;

            // Leaf 1, the feature bits, where two of them are cleared
            // and a third is deliberately left alone.
            if (1 == leaf) {
                // Do not tell the guest it is virtualized. The nesting
                // check in launch_on_this_processor already describes this
                // bit as cleared here, and it has to be: that check asks
                // whether a hypervisor is under *us*, so announcing
                // ourselves to our own guest would make an adopted
                // application processor read its own VMM's answer and
                // conclude it was nested.
                //
                // Setting it also costs the guest its processor power
                // management, which is how this was found. Windows builds
                // an idle state for every ACPI FFH C-state the platform
                // reports, and FFH means MWAIT; a guest that knows it is
                // virtualized is told the host owns idle states, so its
                // platform layer registers an idle state block with the
                // handler at offset 0x50 left null.
                // PpmInstallNewIdleStates copies that block field for
                // field with no validation, and PpmIdleExecuteTransition
                // then calls the copy at 0x270 without a null check,
                // unlike the pointer beside it at 0x268. Kernel control
                // flow guard catches the call through null:
                // KERNEL_SECURITY_CHECK_FAILURE, 0x139, parameter 1 =
                // 0x0a, FAST_FAIL_GUARD_ICALL_CHECK_FAILURE.
                //
                // Read out of three crash dumps rather than reasoned
                // about, which is the only reason it was found. They
                // agreed to the register modulo the kernel's load address:
                // KiIdleLoop -> PoIdle -> PpmIdleExecuteTransition ->
                // _guard_dispatch_icall, target register zero, and
                // parameter 4 zero because _guard_icall_bugcheck passes
                // the rejected target through.
                //
                // Only real firmware reaches it, which is why no test rig
                // caught it: that idle state exists because the platform
                // reports ACPI FFH C-states, and an emulated platform
                // reports none.
                //
                // SDM Vol. 2A, CPUID, "CPUID.01H:ECX Feature
                // Information": bit 31 is reserved and returns 0 on real
                // hardware, which is why it is the conventional way to
                // announce a hypervisor - and why leaving it clear is
                // indistinguishable from bare metal.
                cpuid_result[2] &= ~(1u << 31);

                // Hide VMX. We hold VMX root mode and do not support
                // nesting, so a guest hypervisor would #GP on its own
                // vmxon and take the boot down with it. Reporting no VMX
                // makes it stand down instead: Hyper-V, which launches
                // ahead of Windows whenever VBS is on, hands straight
                // off to the OS. Remove this once nesting exists.
                cpuid_result[2] &= ~(1u << 5);

                // MONITOR/MWAIT, ECX[3], is deliberately left as the
                // hardware reports it. Both instructions execute in the
                // guest and neither is intercepted, so there is nothing
                // to conceal - and concealing it is what produced the
                // 0x139 bugcheck described where those controls are
                // declared. Leaf 5, which the same bit governs, is
                // likewise passed through untouched, so the guest sees
                // one consistent answer across both leaves.
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
                    // The highest leaf answered here, which now includes
                    // the diagnostic leaf below.
                    cpuid_result[0] = diagnostic_leaf;

                    // HyperVisor Name: ZppZppZppZpp.
                    cpuid_result[1] = 0x5a70705a;
                    cpuid_result[2] = 0x705a7070;
                    cpuid_result[3] = 0x70705a70;
                } else if (diagnostic_leaf == leaf) {
                    // Reports another processor's most recent exit.
                    //
                    // This exists because there is otherwise no way to ask
                    // a stopped processor anything. The records live in
                    // members and were readable only from a debugger, and
                    // a debugger is the one tool that cannot be used here:
                    // QEMU's KVM_GET_MP_STATE calls
                    // kvm_apic_accept_events(), which discards a pending
                    // start-up IPI, so attaching gdb can *cause* the
                    // failure being investigated.
                    //
                    // Answering for an arbitrary processor rather than the
                    // caller is the whole point: the processor with
                    // something to say is by definition not executing.
                    //
                    // Deliberately flat scalars rather than a pointer to a
                    // structure. A pointer would make the loader depend on
                    // this class's layout across two separate builds with
                    // different ABIs, and that coupling would break
                    // silently.
                    auto cpu = context.rcx & 0xff;
                    if (cpu < max_cpus) {
                        auto count = this->exit_trace_count[cpu];
                        auto & newest =
                            this->exit_trace[cpu][(count - 1) %
                                                  exit_trace_capacity];

                        cpuid_result[0] =
                            static_cast<std::uint32_t>(count);
                        cpuid_result[1] =
                            count
                                ? static_cast<std::uint32_t>(newest.reason)
                                : 0;
                        cpuid_result[2] = count
                                              ? static_cast<std::uint32_t>(
                                                    newest.qualification)
                                              : 0;

                        // Everything that says "this processor stopped and
                        // why", packed so one leaf answers the question.
                        cpuid_result[3] =
                            (this->unhandled_exit.occurred ? (1u << 0)
                                                           : 0) |
                            (this->vm_entry_failure.occurred ? (1u << 1)
                                                             : 0) |
                            (this->started_by_start_up_ipi[cpu] ? (1u << 2)
                                                                : 0) |
                            // How far the start-up trampoline got, in bits
                            // 11:8. The only account of a failure before
                            // the host descriptor tables are live, and
                            // reported here because a processor that never
                            // arrived cannot report anything itself.
                            (start_up_trampoline_stage() << 8) |
                            // What its launch failed with, in bits 19:16,
                            // for the same reason.
                            static_cast<std::uint32_t>(
                                (this->launch_error[cpu] & 0xf) << 16) |
                            // Which start-up hand-off it is waiting on, in
                            // bits 23:20. A processor that never restarted
                            // is by definition not able to say so itself,
                            // and this is the difference between one
                            // parked waiting on hardware and one still
                            // listening in software - which is the whole
                            // question when a start-up IPI goes missing.
                            static_cast<std::uint32_t>(
                                (this->start_up_handoff[cpu].load() & 0xf)
                                << 20) |
                            (static_cast<std::uint32_t>(
                                 newest.activity_state & 0x3)
                             << 3);
                    }
                } else {
                    // No interface, no features, nothing to enlighten
                    // anyone about.
                    cpuid_result[0] = 0;
                    cpuid_result[1] = 0;
                    cpuid_result[2] = 0;
                    cpuid_result[3] = 0;
                }
            }

            context.rax = cpuid_result[0];
            context.rbx = cpuid_result[1];
            context.rcx = cpuid_result[2];
            context.rdx = cpuid_result[3];
            break;
        }
        case basic_reason::xsetbv: {
            // This is the host's CR4 - VM exit replaced the guest's -
            // and XSETBV raises #UD with OSXSAVE clear (SDM 13.3), so
            // without this the instruction below faults in the host.
            auto cr4 = arch::x86_64::cr4();
            if (!(cr4 & arch::x86_64::cr4_bits::os_xsave)) {
                arch::x86_64::cr4(cr4 | arch::x86_64::cr4_bits::os_xsave);
            }

            arch::x86_64::xsetbv(context.rcx,
                                 context.rax | (context.rdx << 32));
            break;
        }
        case basic_reason::wrmsr:
            // The one MSR this VMM asks to see. It is inside the range the
            // bitmap covers, so it exits only because the bitmap says so.
            if (arch::x86_64::msr::ia32_x2apic_icr ==
                static_cast<std::uint32_t>(context.rcx)) {
                auto command =
                    (context.rax & 0xffffffff) | (context.rdx << 32);

                // What actually goes out is the handler's decision, not
                // the guest's. Most commands are passed through unchanged,
                // a start-up IPI is either replaced with one naming this
                // VMM's own trampoline or swallowed entirely, and there is
                // no correct default here - issuing the guest's own
                // start-up IPI is precisely what hands a processor over
                // unvirtualized.
                if (auto issue = on_interrupt_command(command)) {
                    arch::x86_64::wrmsr(arch::x86_64::msr::ia32_x2apic_icr,
                                        *issue);
                }
                break;
            }
            [[fallthrough]];
        case basic_reason::rdmsr: {
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
            // Deliberately not executed, and not passed through either.
            //
            // INVD discards every modified line in the cache hierarchy
            // without writing any of it back. Running it here on behalf of
            // a guest throws away whatever the host had dirty at that
            // moment as well - this VMM's own log, its page tables, its
            // EPT - and whatever the guest had dirty, and anything a
            // device had not yet observed. There is no way to scope it to
            // the caller, because there is one cache hierarchy.
            //
            // SDM Vol. 2A, INVD: "Data held in internal caches is not
            // written back to main memory ... Use this instruction with
            // care. Data cached internally and not written back to main
            // memory will be lost", and it goes on to say software should
            // use WBINVD instead.
            //
            // So the choice is between losing data and not invalidating.
            // Nothing here relies on a guest's INVD having any effect -
            // the EPT is built once before any guest runs and is never
            // touched again - so treating it as a no-op costs nothing this
            // VMM depends on, while executing it can corrupt both sides of
            // the boundary. RIP advances below as for any completed
            // instruction. KVM makes the same call
            // (kvm_emulate_invd: "Treat an INVD instruction as a NOP").
            //
            // This becomes a real decision the day EPT memory types are
            // re-derived at runtime from guest MTRR writes, because that
            // needs a deliberate cache and TLB sequence of its own. A
            // no-op is the right answer until then, not forever.
            break;
        }
        case basic_reason::monitor:
        case basic_reason::mwait: {
            // Both are emulated as no-ops: the exit is taken purely to
            // stop them executing, and RIP advances below as it would for
            // any completed instruction. A guest that cannot wait polls
            // instead, which is wasteful and correct.
            //
            // What must not go with this is hiding the feature from CPUID.
            // Leaf 1 ECX[3] stays as the hardware reports it, because
            // clearing it is what produced a 0x139 bugcheck on real
            // firmware - the measurement is with those controls, in
            // vmx.h. A guest told the monitor exists and refused the wait
            // loses nothing but power; a guest told it does not exist
            // builds an idle path with a hole in it.
            //
            // Logged once per processor, never per execution. Idle loops
            // run these continuously, so a line each would push everything
            // else out of a 512 line log - which is why this was silent
            // before, and why being silent cost so much. Every question
            // about the idle path so far has been argued rather than
            // measured: whether the guest reaches MWAIT at all, on which
            // processor, from where, and whether its monitor arms under
            // this VMM's EPT. One line per processor answers all four and
            // costs nothing after the first.
            //
            // The armed bit is the interesting half. SDM 28.2.1, Table
            // 28-3: for MWAIT the exit qualification "is set either to 0
            // (if address-range monitoring hardware is not armed) or to 1
            // (if ... armed)". A monitor that never arms means MONITOR is
            // not doing what the guest thinks, and the SDM makes an
            // unarmed MWAIT a no-op rather than a wait - so this
            // distinguishes a guest that is idling from one that is
            // spinning. KVM logs once for the same reason
            // (kvm_emulate_monitor_mwait).
            if (auto cpu = this->vmcs.vpid() - 1;
                (cpu < max_cpus) && !this->monitor_logged[cpu]) {
                this->monitor_logged[cpu] = true;
                log("cpu {} {} rip {} cs {} armed {}",
                    cpu,
                    (basic_reason::mwait == reason) ? "mwait" : "monitor",
                    this->vmcs.guest_rip(),
                    this->vmcs.guest_cs_selector(),
                    this->vmcs.exit_qualification());
            }
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

            // Everything that could stop the resume that follows, read
            // before anything is touched. This is the exit the processor
            // dies on - it is delivered, this handler runs, its writes
            // read back correctly, and then nothing executes - so these
            // are the fields nobody has looked at yet at the moment that
            // matters. A pending event here would be delivered by the very
            // next VM entry, through the descriptor tables the reset below
            // is about to zero.
            log("sipi cpu {} entry_intr {} idt_vectoring {}",
                vmcs.vpid(),
                vmcs.read(arch::x86_64::vmx::vmcs::field::
                              vm_entry_interruption_information_field),
                vmcs.read(arch::x86_64::vmx::vmcs::field::
                              idt_vectoring_information_field));
            log("sipi cpu {} interruptibility {} pending_dbg {}",
                vmcs.vpid(),
                vmcs.guest_interruptibility_state(),
                vmcs.guest_pending_debug_exceptions());
            emulate_start_up_ipi(context, vector);
            advance_rip = false;
            break;
        }
        case basic_reason::io_instruction: {
            // Reachable only for a port deliberately armed in the I/O
            // bitmaps, which today is the ACPI sleep control register
            // and nothing else.
            if (!on_io_instruction()) {
                record_exit(full_reason);
                on_unhandled_exit(full_reason);
                break;
            }
            // Deliberately not advanced. The handler released the port,
            // so re-executing the guest's own instruction is what
            // performs it.
            advance_rip = false;
            break;
        }
        case basic_reason::control_register_access: {
            // Reachable only because CR4's guest/host mask is non-zero:
            // a write to a masked bit exits instead of landing in the
            // register. Today that is VMXE and nothing else.
            //
            // The guest is given what it asked for in the shadow, so a
            // read back agrees with its own write, while the real
            // register keeps VMXE - without which the next VM entry
            // fails, since a processor in root mode must have it set.
            constexpr std::uint64_t cr4_vmxe = 1ull << 13;

            auto qualification = vmcs.exit_qualification();
            auto number = qualification & 0xf;
            auto access = (qualification >> 4) & 0x3;
            auto gpr = (qualification >> 8) & 0xf;

            // Only a MOV to CR4 can arrive here. Anything else means the
            // mask grew without this growing with it, and guessing at
            // it would resume the guest as though something had worked.
            if ((4 != number) || (0 != access)) {
                record_exit(full_reason);
                on_unhandled_exit(full_reason);
                break;
            }

            // The encoded register number is the architectural one,
            // which is not the order this context happens to store them
            // in - so it is spelled out rather than indexed.
            std::uint64_t value{};
            switch (gpr) {
            case 0:
                value = context.rax;
                break;
            case 1:
                value = context.rcx;
                break;
            case 2:
                value = context.rdx;
                break;
            case 3:
                value = context.rbx;
                break;
            case 4:
                value = context.rsp;
                break;
            case 5:
                value = context.rbp;
                break;
            case 6:
                value = context.rsi;
                break;
            case 7:
                value = context.rdi;
                break;
            case 8:
                value = context.r8;
                break;
            case 9:
                value = context.r9;
                break;
            case 10:
                value = context.r10;
                break;
            case 11:
                value = context.r11;
                break;
            case 12:
                value = context.r12;
                break;
            case 13:
                value = context.r13;
                break;
            case 14:
                value = context.r14;
                break;
            default:
                value = context.r15;
                break;
            }

            vmcs.cr4_read_shadow(value & ~cr4_vmxe);
            vmcs.guest_cr4(value | cr4_vmxe);
            break;
        }
        case basic_reason::ept_violation: {
            // A watched page was touched. RIP stays where it is: the
            // guest's instruction has not run yet, and the whole point
            // is to let it run for itself rather than emulate it.
            if (!on_ept_violation(cpuid)) {
                // Nothing had that page watched, so the protection was
                // put there by something that is not going to handle the
                // fault - which is a bug here rather than a guest error,
                // and resuming would fault identically forever.
                record_exit(full_reason);
                on_unhandled_exit(full_reason);
            }
            advance_rip = false;
            break;
        }
        case basic_reason::monitor_trap_flag: {
            // One guest instruction has retired since the page was
            // opened. Close it again and tell whoever was watching.
            if (!on_monitor_trap_flag(cpuid)) {
                // The flag is only ever armed by the watch above, so an
                // MTF exit with no step in progress means someone else
                // set it and there is no correct way to continue.
                record_exit(full_reason);
                on_unhandled_exit(full_reason);
            }
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

        // Move a few records out of the ring on the way back to the
        // guest.
        //
        // Here rather than on a timer, because this is the only place
        // that is guaranteed to run while a guest is alive and is
        // already a context where taking microseconds is normal. The
        // budget is four records, so this is a trickle that keeps up
        // with a guest rather than a flush - a flush belongs in the halt
        // paths, where there is no guest left to delay.
        //
        // It compiles to nothing when the facility is off: pump::run
        // is `if constexpr (!enabled) return;` and every sink behind it
        // folds away with it.
        diag::pump::run();

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

        // The mirror of the launch: the guest's registers are put back
        // and the last thing executed in host mode is the resume itself.
        context.rip =
            reinterpret_cast<std::uint64_t>(arch::x86_64::vmx::vmresume);
        arch::x86_64::restore_context(&context);
    });

    return {};
}

void hypervisor::launch_on_cpu_private_stack(
    hypervisor & hypervisor, arch::x86_64::context & caller_context)
{
    auto result = hypervisor.main(caller_context);

    // Record the failure where a processor that is still running can
    // read it.
    //
    // A processor this VMM started cannot report its own: it arrived on
    // an interrupt rather than a call, so there is nowhere to return an
    // error to and nothing to do afterwards but halt. The diagnostic
    // CPUID leaf folds the low nibble of this into what it reports, and
    // until now nothing ever assigned it - so that nibble was always
    // zero, which is worse than absent because it is read as an answer.
    //
    // The processor number is the one main was given, which is the same
    // index the leaf reads back with.
    if (!result) {
        if (auto cpu = caller_context.rdi; cpu < max_cpus) {
            hypervisor.launch_error[cpu] = result.error().code();
        }
    }

    // How it reaches the loader: _start captured this context with its
    // RIP set to the return address, so restoring it below makes the
    // module look like a function that returned this value.
    caller_context.rax = result ? 0 : result.error().code();

    // On success the hypervisor stays resident and its globals must
    // outlive every guest, so only tear them down when it does not.
    if (!result) {
        zpp::crt::init::cleanup();
    }

    arch::x86_64::restore_context(&caller_context);
}

void hypervisor::launch_on_cpu(arch::x86_64::context & caller_context)
{
    // Refuse rather than run off the end of the stack array.
    //
    // This index is incremented once per processor launched and was not
    // bounded. On a machine with more logical processors than max_cpus
    // the next one indexed past the array and took a 512 KB stack with
    // it, laying it over whatever member followed - silently, and only
    // on the machines with the most processors, which are the least
    // likely to be the ones being debugged.
    //
    // Every other per-processor array in this class is already indexed
    // under a `< max_cpus` test. This one was not, and it is the one
    // that writes half a megabyte.
    if (this->available_stack_index >= max_cpus) {
        caller_context.rax = zpp::error{error::too_many_processors}.code();
        arch::x86_64::restore_context(&caller_context);
        return;
    }

    // A stack inside the module, because main switches onto the host
    // page table, which does not map the caller's.
    auto & stack = this->stack[this->available_stack_index];

    // The context is copied onto it for the same reason: main reads it
    // after the switch, by when the original is unreachable.
    auto stack_top = stack + sizeof(stack) - sizeof(arch::x86_64::context);

    auto copied_caller_context =
        ::new (stack_top) arch::x86_64::context(caller_context);

    // The call is assembled by hand out of the caller's own context,
    // because it has to land on a different stack. RDI and RSI are the
    // System V argument registers, and the callee is static, so they are
    // its two parameters rather than a this pointer and one.
    auto & launch_context = caller_context;

    launch_context.rip =
        reinterpret_cast<std::uint64_t>(launch_on_cpu_private_stack);
    launch_context.rsp = reinterpret_cast<std::uint64_t>(stack_top);

    // Simulate call instruction for proper stack alignment.
    launch_context.rsp -= sizeof(std::uint64_t);

    launch_context.rdi = reinterpret_cast<std::uint64_t>(this);
    launch_context.rsi =
        reinterpret_cast<std::uint64_t>(copied_caller_context);

    ++this->available_stack_index;

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

/**
 * Where a processor started by this VMM arrives, declared by
 * zpp/arch/x86_64/ap_start_up.h and reached from the trampoline there.
 */
extern "C" void zpp_ap_start_up_main(std::uint64_t processor)
{
    zpp::hypervisor::hypervisor::instance().start_up_on_this_processor(
        processor);
}
