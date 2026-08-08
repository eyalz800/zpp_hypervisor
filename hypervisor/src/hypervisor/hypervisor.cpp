#include "zpp/hypervisor/hypervisor.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/exception_entry.h"
#include "zpp/arch/x86_64/generic.h"
#include "zpp/arch/x86_64/interrupt_gate.h"
#include "zpp/arch/x86_64/page_table.h"
#include "zpp/arch/x86_64/pci.h"
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
#include "zpp/hypervisor/power.h"
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

namespace
{
/**
 * What initialize_module_region worked out, recorded where a debugger
 * can read it without doubt.
 *
 * Outside the singleton deliberately. That object is thirty megabytes,
 * and these members sit twenty-one megabytes into it, so reading them
 * means trusting both the object's address and a large offset. On one
 * machine those reads return page-address-like values while the sink's
 * own statics a little earlier in .bss read correctly, and there is no
 * way to tell a wrong address from wrong contents by staring at either.
 * A namespace scope global lands at a small .bss offset of its own and
 * removes the question.
 *
 * volatile because nothing here reads them.
 */
volatile std::uint64_t g_module_base_seen{};
volatile std::uint64_t g_module_size_seen{};
volatile std::uint64_t g_module_base_handed_over{};
} // namespace

void hypervisor::initialize_module_region()
{
    // Where this module begins and how far it runs.
    //
    // The base comes from the loader, which chose it; only the size is
    // worked out from the image, which is the only authority on its own
    // extent.
    //
    // Searching for the base is the fallback, not the plan, and the
    // comment here used to claim the ELF magic "appears exactly once, at
    // the header". It does not. The scan walks backwards a page at a
    // time from a key inside the module and stops at the first page
    // beginning with those four bytes, which is correct only while
    // nothing in between happens to. A displaced base would displace
    // module_size, the range protect_module hides from the guest, and the
    // physical map behind the decoy redirect, so it is not a cosmetic
    // error.
    //
    // A reading that appeared to show it going wrong was withdrawn - it
    // came from a machine where the hypervisor was not resident, so the
    // memory read was not ours. The argument for handing the base over
    // stands on its own: the loader knows the address exactly, and a scan
    // that can be wrong is a poor way to learn something the caller
    // already has.
    //
    // The pad byte in front of the search key in elf_image_base.h is
    // still deliberate: it leaves the key itself unaligned so the search
    // cannot stop on it. That was never the failure; a foreign page was.
    //
    // The size is the *memory* size, not the file size. They differ by
    // .bss, which is most of this module - the per-processor stacks
    // alone are megabytes - and using the smaller of the two would leave
    // the VMM's own stacks outside every range derived from here: the
    // host mapping just below, and the protection that hides the module
    // from the guest.
    this->module_base = this->handed_over_module_base
                            ? static_cast<const unsigned char *>(
                                  this->handed_over_module_base)
                            : elf_image_base();
    this->module_size =
        elf_file(this->module_base, elf_file::state::loaded).memory_size();

    g_module_base_handed_over =
        reinterpret_cast<std::uint64_t>(this->handed_over_module_base);
    g_module_base_seen =
        reinterpret_cast<std::uint64_t>(this->module_base);
    g_module_size_seen = this->module_size;
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
    // The non-maskable interrupt is answered first, and by returning.
    //
    // It is the one vector that is not caused by the instruction it
    // interrupts, so there is nothing to recover from and nothing to
    // diagnose - resuming is simply correct. Every other vector here is a
    // fault in this VMM, where resuming would re-execute the faulting
    // instruction and fault again.
    //
    // This is why the wake probe in wait_for_ept_acknowledgement was
    // unusable. NMI exiting governs non-root operation only, so an NMI
    // sent to a processor that is inside its own VM exit arrives *here*,
    // and this handler used to halt any vector with no recovery point -
    // for ever, with no record. One probe could take out several
    // processors at once, and it took the development machine off the
    // network. Every caller that might reach a spinning processor was
    // then made to wait passively instead, which is why those waits time
    // out rather than succeed.
    //
    // It is also a defect in its own right, independent of anything this
    // VMM does deliberately: a thermal, watchdog or performance
    // monitoring NMI arriving while any processor happened to be in root
    // mode halted it silently.
    //
    // Deliberately before the recovery point is consulted. An NMI is not
    // a fault, so unwinding to a recovery point armed for one would
    // discard work that had not failed.
    constexpr std::uint64_t non_maskable_interrupt = 2;

    if (non_maskable_interrupt == frame.vector) {
        this->host_nmi_count = this->host_nmi_count + 1;
        this->host_nmi_rip = frame.rip;
        return;
    }

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

    // The type by value, because it is a register operand: SDM 33.3,
    // INVEPT, "INVEPT_TYPE := value of register operand". This passed the
    // *address* of a variable holding it, which put a stack address in
    // that register - not a supported type - so every invalidation this
    // function ever performed failed with "invalid operand to
    // INVEPT/INVVPID" and left the mapping cached. The signature now takes
    // an integer so the mistake cannot be spelled.
    constexpr std::uint64_t single_context = 1;

    if (0 != arch::x86_64::vmx::invept(single_context, &operand)) {
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

bool hypervisor::wait_for_ept_acknowledgement(std::uint64_t budget,
                                              bool probe)
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
                if (probe && !this->wake_requested[cpu].exchange(
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
    // Per processor, because the control it guards is per VMCS.
    //
    // This was one shared flag, and a shared flag in front of
    // per-processor state gets the answer wrong twice over: the first
    // processor to arm set the flag and its own pin-based controls, and
    // every other processor then matched the flag and returned without
    // arming, so at most one processor ever had a clock. Disarming was
    // the mirror - the first to disarm cleared the flag and its own
    // control, leaving every other processor's set with nothing able to
    // turn it off again.
    auto slot = vmcs.vpid();
    if ((0 == slot) || (slot > max_cpus)) {
        return;
    }

    auto & armed_here = this->controller_poll_armed[slot - 1];

    if (armed == armed_here) {
        return;
    }

    // Only if the processor allows this control to be set.
    //
    // A pin based control may be set to 1 only where the corresponding
    // allowed-1 bit is set in the high half of IA32_VMX_PINBASED_CTLS, or
    // of IA32_VMX_TRUE_PINBASED_CTLS where the basic capability MSR says
    // the true controls exist - SDM Appendix A.3.1. Setting one that is
    // not permitted does not fail loudly; it fails the *next VM entry*,
    // and under a nested hypervisor it can simply not come back.
    //
    // Measured, and it is why this check exists: on the rig that passes a
    // real NVMe through, arming the timer left the machine wedged in
    // vmresume with exactly one exit recorded - reason 0xa, a plain
    // CPUID - and no second exit ever. The same build on an emulated
    // controller ran fine, so it read as a device problem for several
    // rounds. It is not: it is a control the outer hypervisor does not
    // offer, set without asking.
    constexpr std::uint64_t true_controls_available = 1ull << 55;

    auto capability_msr =
        (this->cached_vmx_msr(arch::x86_64::vmx::msr::basic) &
         true_controls_available)
            ? arch::x86_64::vmx::msr::true_pin_based_controls
            : arch::x86_64::vmx::msr::pin_based_controls;

    auto allowed_one = this->cached_vmx_msr(capability_msr) >> 32;

    if (armed && !(allowed_one & arch::x86_64::vmx::vm_execution_controls::
                                     pin::activate_preemption_timer)) {
        // Said once per processor, not once per exit.
        //
        // arm_controller_poll runs from the exit path, so a plain log here
        // was two heap-allocating calls into shared state on *every* exit
        // of every processor - thousands of them - which is both absurd
        // and a fine way to corrupt something. The flag is per processor
        // and is never cleared, because a control the processor does not
        // offer will not start being offered later.
        if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus)) {
            if (!this->timer_refusal_reported[slot - 1]) {
                this->timer_refusal_reported[slot - 1] = true;
                diag::log<diag::severity::warning>(
                    "preemption timer not permitted, allowed-1 {}",
                    allowed_one);
                log("preemption timer falling back to the guest's timer");
            }
        }

        // Borrow the guest's own timer instead of asking for one.
        //
        // Measured on the rig, and this is what the fallback exists for:
        // with the timer refused the channel wrote four heartbeats -
        // three, seven, eleven and fifteen exits - across four and a half
        // seconds of a four minute run, and then nothing. Not a broken
        // writer. A guest that has settled stops exiting, and every
        // record this side keeps is written from an exit.
        //
        // A guest that is merely stuck is still taking timer interrupts,
        // and its handler arms the next one before it returns. Trapping
        // that write costs an exit the guest was going to cause anyway,
        // needs no control the processor can refuse, and ticks at
        // whatever rate the guest has chosen rather than one this side
        // has to pick.
        //
        // What it does not cover: a guest whose local APIC is in xAPIC
        // mode programs the same timer through the APIC page, which is a
        // memory write and not an MSR. If that turns up, the answer is
        // to fault the APIC page rather than to widen this.
        this->arm_guest_timer_poll(armed);
        armed_here = armed;
        return;
    }

    armed_here = armed;

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

void hypervisor::arm_guest_timer_poll(bool armed)
{
    // Writes only. A read of either register tells this side nothing it
    // does not already have, and reads are the half a guest does far more
    // often - Windows reads the deadline register to work out how long is
    // left, and trapping that would multiply the cost for nothing.
    this->intercept_msr(arch::x86_64::msr::ia32_tsc_deadline, false, armed);
    this->intercept_msr(
        arch::x86_64::msr::ia32_x2apic_init_count, false, armed);
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

        // One rebuild at a time, across every processor.
        //
        // The enable is observed in two places - the emulated write to the
        // configuration register, and the poll on this VMM's own exits -
        // and `channel_controller_enabled` that guards both is a plain
        // bool written from every processor's exit path. So two
        // processors can both decide the controller has just come up and
        // both start a borrow.
        //
        // They would serialise on mapping_window_lock rather than run
        // together, which is what has kept this from being seen, but
        // serialised is not harmless: the second borrow finds the queue
        // already created and gets Invalid Queue Identifier, and unlike
        // the first it runs *after* the guest's driver is live. That is
        // precisely the unexcluded borrow that was once measured spending
        // its whole budget and timing out, with the guest's admin queue
        // desynchronised behind it.
        //
        // An exchange rather than a test and a set, because the two
        // observers are what create the race in the first place.
        if (this->channel_rebuild_running.exchange(
                true, std::memory_order_acquire)) {
            this->channel_rebuild_reentered =
                this->channel_rebuild_reentered + 1;
            return;
        }

        scope_exit release_rebuild{[this] {
            this->channel_rebuild_running.store(
                false, std::memory_order_release);
        }};

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

        auto budget = std::uint64_t{1} << 24;
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
        // Translated, not cast: these take guest physical addresses and
        // `bar` is a host virtual pointer. Casting is right only while
        // that mapping is the identity, which is not something this VMM
        // establishes or checks.
        auto doorbell_page =
            this->host_page_table.virtual_to_physical(
                const_cast<const void *>(bar)) +
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
        // Passively. The default sends a wake NMI to a processor that has
        // not answered, and the rule on the declaration is that no caller
        // a processor might be spinning inside may do that: such a
        // processor is in root mode, NMI exiting governs non-root
        // operation only, so the NMI arrives at the host IDT where
        // on_host_exception finds no recovery point and halts it for
        // ever. That took the development machine off the network once.
        //
        // This path was relying on nobody being in that position yet -
        // the application processors are still unlaunched during the
        // guest's storage initialisation - which is a property of when it
        // happens rather than of what it does, and is not the rule. The
        // excursion path next door already passes false.
        if (!wait_for_ept_acknowledgement(std::uint64_t{1} << 24, false)) {
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
                                    std::uint64_t{1} << 24);

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

std::expected<void, zpp::error> hypervisor::watch_guest_page_writes(
    std::uint64_t guest_physical,
    page_watch::handler on_write,
    void * context,
    page_watch::mode behaviour,
    void (*before_write)(void *, std::uint64_t))
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
            watch.before_write = before_write;
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
    free_slot->before_write = before_write;
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

namespace
{
/**
 * Waits for CSTS.RDY to reach a value, bounded.
 *
 * The controller is allowed CAP.TO half-seconds to answer, and this runs
 * inside a VM exit, so the bound is a spin count rather than a clock: the
 * point is that it cannot become a hang, not that it matches the
 * architectural allowance exactly.
 */
bool wait_for_ready(volatile std::uint8_t * bar, bool wanted)
{
    // Sized to what the wait actually costs, which is not what a spin
    // count usually implies: every iteration is an uncached MMIO read
    // across PCIe, so an iteration is closer to a microsecond than to a
    // nanosecond. This budget is therefore a few hundred milliseconds,
    // not a few hundred million of them.
    //
    // It was 1 << 26, which is over a minute of held processor - and a
    // processor that spins rather than halts is a host CPU that never
    // yields, which is how the development machine stopped answering its
    // network rather than merely wedging its guest. Every spin in a VM
    // exit has to be bounded by the thing it is waiting for: a controller
    // enable measured in the low milliseconds here, against a driver
    // unbind and rebind cycle of 178 ms that includes far more than this.
    constexpr std::uint64_t budget = 1ull << 18;

    auto at =
        bar + zpp::nvme::offset_of(zpp::nvme::register_offset::status);

    for (auto spun = budget; spun; --spun) {
        auto status =
            zpp::nvme::controller_status{zpp::arch::x86_64::read32(at)};

        if (status.ready() == wanted) {
            return true;
        }
    }

    return false;
}

} // namespace

std::expected<void, zpp::error> hypervisor::run_reset_excursion()
{
    using zpp::nvme::offset_of;
    using zpp::nvme::register_offset;

    // The whole body is behind the compile time condition, not only the
    // calls into the sink. Naming a static member of a class template
    // odr-uses it, so a plain reference would put the channel's queues
    // and code into a build that has it switched off - which
    // scripts/ci/check-diag-absent.sh fails, and did fail over exactly
    // this function.
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present) {
        return std::unexpected(
            zpp::error{error::controller_not_available});
    } else {
        if (!this->channel_bar) {
            return std::unexpected(
                zpp::error{error::controller_not_available});
        }

        auto * bar =
            static_cast<volatile std::uint8_t *>(this->channel_bar);

        // The guest has just cleared CC.EN and is waiting for the
        // controller to follow. Let it, then take the controller while it
        // is nobody's.
        if (!wait_for_ready(bar, false)) {
            return std::unexpected(
                zpp::error{error::controller_never_settled});
        }

        // What the guest programmed, so it can be put back exactly. These
        // are readable and are only writable while CC.EN is clear, which
        // is why the whole excursion has to live inside this window.
        auto guest_attributes = arch::x86_64::read32(
            bar + offset_of(register_offset::admin_queue_attributes));
        auto guest_submission = arch::x86_64::read64(
            bar + offset_of(register_offset::admin_submission_queue_base));
        auto guest_completion = arch::x86_64::read64(
            bar + offset_of(register_offset::admin_completion_queue_base));

        // Put it all back however this returns. The guest's
        // re-initialisation reads these, and leaving ours behind would
        // point its admin queue at memory it does not own.
        scope_exit restore{[&] {
            arch::x86_64::write32(
                bar + offset_of(register_offset::configuration), 0);
            wait_for_ready(bar, false);

            arch::x86_64::write32(
                bar + offset_of(register_offset::admin_queue_attributes),
                guest_attributes);
            arch::x86_64::write64(
                bar + offset_of(
                          register_offset::admin_submission_queue_base),
                guest_submission);
            arch::x86_64::write64(
                bar + offset_of(
                          register_offset::admin_completion_queue_base),
                guest_completion);
        }};

        // Our own admin queue, in storage nothing else can reach. Four
        // entries: the excursion issues two commands and the minimum a
        // controller must accept is two.
        constexpr std::uint32_t admin_entries = 4;

        using queues = diag::esp_block_sink::queues;

        auto admin_submission_physical =
            this->host_page_table.virtual_to_physical(
                queues::admin_submissions);
        auto admin_completion_physical =
            this->host_page_table.virtual_to_physical(
                queues::admin_completions);

        __builtin_memset(queues::admin_submissions,
                         0,
                         admin_entries * sizeof(nvme::submission_entry));
        __builtin_memset(queues::admin_completions,
                         0,
                         admin_entries * sizeof(nvme::completion_entry));

        // AQA carries both depths zero's based, ASQS in bits 11:0 and ACQS
        // in bits 27:16.
        arch::x86_64::write32(
            bar + offset_of(register_offset::admin_queue_attributes),
            (admin_entries - 1) | ((admin_entries - 1) << 16));
        arch::x86_64::write64(
            bar + offset_of(register_offset::admin_submission_queue_base),
            admin_submission_physical);
        arch::x86_64::write64(
            bar + offset_of(register_offset::admin_completion_queue_base),
            admin_completion_physical);

        // Enable, with the entry sizes the queues actually use: a
        // submission entry is 64 bytes and a completion entry 16, both
        // expressed as a power of two, and the NVM command set at the
        // smallest page size.
        constexpr std::uint32_t enable = 1;
        constexpr std::uint32_t submission_entry_size = 6u << 16;
        constexpr std::uint32_t completion_entry_size = 4u << 20;

        arch::x86_64::write32(
            bar + offset_of(register_offset::configuration),
            enable | submission_entry_size | completion_entry_size);

        if (!wait_for_ready(bar, true)) {
            return std::unexpected(
                zpp::error{error::controller_never_settled});
        }

        // One command at a time on a queue nobody else uses, so the whole
        // of the submission side is a slot, a doorbell and a wait.
        std::uint32_t admin_tail{};
        std::uint32_t admin_head{};
        auto admin_phase = true;

        auto run_admin = [&](const nvme::submission_entry & command) {
            queues::admin_submissions[admin_tail] = command;
            admin_tail = (admin_tail + 1) % admin_entries;

            arch::x86_64::order_stores();
            arch::x86_64::write32(
                bar + nvme::submission_queue_doorbell_offset(
                          0, this->channel_doorbell_stride),
                admin_tail);

            constexpr std::uint64_t budget = 1ull << 26;
            for (auto spun = budget; spun; --spun) {
                auto & entry = queues::admin_completions[admin_head];
                if (entry.phase() != admin_phase) {
                    continue;
                }

                arch::x86_64::order_loads();
                auto status = entry.status();

                admin_head = (admin_head + 1) % admin_entries;
                if (0 == admin_head) {
                    admin_phase = !admin_phase;
                }

                arch::x86_64::write32(
                    bar + nvme::completion_queue_doorbell_offset(
                              0, this->channel_doorbell_stride),
                    admin_head);

                return status;
            }

            return std::uint16_t{0xffff};
        };

        // The private pair. Destroyed with the controller at the end of
        // this function, which is why its identifier cannot collide with
        // anything the guest later creates.
        auto completion_physical =
            this->host_page_table.virtual_to_physical(queues::completions);
        auto submission_physical =
            this->host_page_table.virtual_to_physical(queues::submissions);

        if (0 != run_admin(nvme::create_io_completion_queue(
                     this->channel_queue_id,
                     diag::esp_block_sink::queue_entries,
                     completion_physical,
                     false,
                     0))) {
            return std::unexpected(zpp::error{error::excursion_refused});
        }

        if (0 != run_admin(nvme::create_io_submission_queue(
                     this->channel_queue_id,
                     diag::esp_block_sink::queue_entries,
                     submission_physical,
                     this->channel_queue_id,
                     nvme::queue_priority::medium))) {
            return std::unexpected(zpp::error{error::excursion_refused});
        }

        // The queue is ours again, at position zero, and everything staged
        // can go out through it.
        diag::esp_block_sink::adopt_rebuilt_queue(
            bar,
            this->channel_doorbell_stride,
            this->channel_queue_id,
            this->channel_namespace);
        diag::esp_block_sink::flush_pending();

        return {};

    } // if constexpr
}

std::expected<void, zpp::error>
hypervisor::shadow_controller_registers(bool armed)
{
    if (!this->channel_bar) {
        return std::unexpected(
            zpp::error{error::controller_not_available});
    }

    auto bar = const_cast<const void *>(this->channel_bar);
    auto bar_physical = this->host_page_table.virtual_to_physical(bar);
    auto bar_page = bar_physical & ~(page_size - 1);

    auto entry = epte_for(bar_page);
    if (!entry) {
        return std::unexpected(entry.error());
    }

    if (armed) {
        // Fill the shadow from the real registers first, so everything
        // the guest is not being lied to about - CAP, VS, the version and
        // the reserved space - reads back exactly as it did.
        auto * from = static_cast<const volatile std::uint8_t *>(
            reinterpret_cast<const volatile void *>(bar_page));
        auto * to = this->unprotected_memory.register_shadow;

        for (std::size_t i{}; i < page_size; i += sizeof(std::uint32_t)) {
            auto value = arch::x86_64::read32(from + i);
            __builtin_memcpy(to + i, &value, sizeof(value));
        }

        // Worth being explicit about what this freezes. The page is the
        // controller's own registers and nothing else - the doorbells
        // start at 0x1000, the next page, and a PCI memory BAR is not
        // shared with another device - so nothing unrelated is affected.
        // But every register on it stops changing for the window, not
        // only the two below: a controller that went fatal would have
        // CSTS.CFS hidden, a shutdown in progress would have CSTS.SHST
        // frozen, and a guest write to the interrupt mask is held rather
        // than lost. That is acceptable for a bounded window and would
        // not be for a permanent one.
        //
        // And then the two the guest must not see change. It has just
        // cleared CC.EN and is waiting for CSTS.RDY to follow; both stay
        // that way for as long as the controller is ours, whatever the
        // hardware is actually doing.
        constexpr std::size_t configuration = 0x14;
        constexpr std::size_t status = 0x1c;

        auto disabled = nvme::controller_configuration{
            *reinterpret_cast<const std::uint32_t *>(to + configuration)};
        auto stopped = nvme::controller_status{
            *reinterpret_cast<const std::uint32_t *>(to + status)};

        auto configuration_value = disabled.value() & ~std::uint32_t{1};
        auto status_value = stopped.value() & ~std::uint32_t{1};

        __builtin_memcpy(to + configuration,
                         &configuration_value,
                         sizeof(std::uint32_t));
        __builtin_memcpy(
            to + status, &status_value, sizeof(std::uint32_t));

        auto shadow_physical =
            this->host_page_table.virtual_to_physical(to);

        (*entry)->page_number(shadow_physical >> 12);
        (*entry)->read(true);
        (*entry)->write(false);
    } else {
        (*entry)->page_number(bar_page >> 12);
        (*entry)->read(true);
        (*entry)->write(false);
    }

    invalidate_ept();

    // A processor still holding the old translation would read the real
    // register, which is the whole thing this exists to prevent - so the
    // change is not merely announced, it is waited for.
    // Passive: no wake NMI. A processor faulting on this page spins inside
    // its own VM exit, in root mode, where an NMI reaches the host IDT and
    // halts it - see wait_for_ept_acknowledgement. Giving up is the safe
    // answer and the caller treats it as a refusal.
    if (!wait_for_ept_acknowledgement(std::uint64_t{1} << 24, false)) {
        return std::unexpected(
            zpp::error{error::acknowledgement_timed_out});
    }

    return {};
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

std::optional<std::uint64_t>
hypervisor::translate_guest_linear(std::uint64_t linear)
{
    // The guest's own four level page table, walked at exit time from the
    // CR3 the VMCS holds now.
    //
    // Not os_page_table, which is built once from the CR3 the launch saw
    // and is therefore only right while the guest is still on the
    // firmware's identity map. Once an operating system is on its own
    // tables that map answers with a kernel linear address used as a
    // physical one, and the two failure modes are both silent: either the
    // window maps a page number above the physical address width and the
    // copy faults in root mode with no recovery point armed, or unrelated
    // bytes decode into a plausible instruction and a fabricated value is
    // written to a device register.
    //
    // The caller holds mapping_window_lock. This walks through the same
    // window page the instruction fetch uses, which is safe only because
    // the walk finishes and yields a number before that fetch re-points
    // it - the lock is not recursive and cannot be taken here.
    if (0 == (this->vmcs.vm_entry_controls() &
              arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest)) {
        // Only long mode is walked. A guest in 32-bit paging has a
        // different table shape entirely, and answering with a
        // long-mode walk of it would be worse than refusing.
        return {};
    }

    // Bits 11:0 of CR3 are flags and a process context identifier, not
    // address. SDM 5.5, "4-Level Paging".
    constexpr std::uint64_t address_mask = 0x000ffffffffff000ull;
    constexpr std::uint64_t present = 1ull << 0;
    constexpr std::uint64_t large_page = 1ull << 7;

    auto table = this->vmcs.guest_cr3() & address_mask;

    // From the outermost level inwards, with the size of the page each
    // level would terminate at. A terminating entry is one with the page
    // size bit set, which is only meaningful on the middle two levels.
    constexpr struct
    {
        std::uint32_t shift;
        std::uint64_t page_size;
    } levels[] = {
        {39, 0},                    // no 512 GB pages exist
        {30, 1ull << 30},
        {21, 1ull << 21},
        {12, 0},                    // the last level always terminates
    };

    for (std::size_t level{}; level < 4; ++level) {
        auto * entries = static_cast<const std::uint64_t *>(
            map_window_at(transfer_window_first_page, table, 1));
        if (!entries) {
            return {};
        }

        auto index = (linear >> levels[level].shift) & 0x1ff;
        auto entry = entries[index];

        if (!(entry & present)) {
            return {};
        }

        if (3 == level) {
            return (entry & address_mask) | (linear & (page_size - 1));
        }

        if ((0 != levels[level].page_size) && (entry & large_page)) {
            auto offset = linear & (levels[level].page_size - 1);
            return (entry & address_mask &
                    ~(levels[level].page_size - 1)) |
                   offset;
        }

        table = entry & address_mask;
    }

    return {};
}

std::optional<arch::x86_64::memory_store> hypervisor::decode_guest_store(
    std::size_t cpu, arch::x86_64::context & context)
{
    // The instruction is at the guest's RIP, which is a linear address
    // in the guest's own address space, so it takes the guest's page
    // tables to find - not this VMM's.
    auto rip = this->vmcs.guest_rip();

    // Serialised, because the window is now one shared pair of pages -
    // and taken before the walk, which reaches through it too.
    this->mapping_window_lock.lock();
    scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

    auto physical = this->translate_guest_linear(rip);
    if (!physical) {
        return {};
    }

    // Both translations are done before either page is mapped, because
    // the walk and the instruction fetch share a window page and the walk
    // must not be re-pointed under itself.
    constexpr std::uint64_t page_mask =
        ~static_cast<std::uint64_t>(page_size - 1);
    auto tail_linear = (rip & page_mask) + page_size;
    auto tail_physical = this->translate_guest_linear(tail_linear);

    // Fifteen bytes is the architectural maximum length of an
    // instruction, and it may straddle a page boundary, which is why the
    // window is two pages. They need not be contiguous in guest physical
    // memory, so the second is mapped from its own translation rather
    // than assumed to follow the first.
    constexpr std::size_t longest_instruction = 15;

    auto first_page = instruction_window_first_page(cpu);
    auto * bytes = static_cast<const std::uint8_t *>(
        map_window_at(first_page, *physical, 1));
    if (!bytes) {
        return {};
    }

    std::uint8_t code[longest_instruction]{};

    auto offset = *physical & (page_size - 1);
    auto in_first = page_size - offset;
    if (in_first > longest_instruction) {
        in_first = longest_instruction;
    }

    __builtin_memcpy(code, bytes, in_first);

    if (in_first < longest_instruction) {
        // The tail lives on the next linear page, which was translated
        // separately above - the guest is free to have mapped it
        // anywhere, or not at all, and a decoder that read past the end
        // of the first page would be reading whatever physically follows
        // it.
        if (tail_physical) {
            if (auto * tail = static_cast<const std::uint8_t *>(
                    map_window_at(first_page + 1, *tail_physical, 1))) {
                __builtin_memcpy(
                    code + in_first, tail, longest_instruction - in_first);
            }
        }
    }

    return arch::x86_64::decode_memory_store(
        std::as_bytes(std::span{code}), context);
}

bool hypervisor::apply_guest_store(
    std::uint64_t guest_physical, const arch::x86_64::memory_store & store)
{
    // Straight through the host page table, which is the only mapping of
    // this page that is writable - the guest's own is not, which is the
    // whole point. The identity between guest and host physical that the
    // EPT establishes is what makes the address usable here directly.
    auto * at = static_cast<volatile std::uint8_t *>(
        reinterpret_cast<void *>(guest_physical));

    if (!this->host_page_table.virtual_to_physical(
            reinterpret_cast<const void *>(guest_physical))) {
        return false;
    }

    switch (store.size) {
    case 1:
        arch::x86_64::write8(at, static_cast<std::uint8_t>(store.value));
        return true;
    case 2:
        arch::x86_64::write16(at, static_cast<std::uint16_t>(store.value));
        return true;
    case 4:
        arch::x86_64::write32(at, static_cast<std::uint32_t>(store.value));
        return true;
    case 8:
        arch::x86_64::write64(at, store.value);
        return true;
    default:
        return false;
    }
}

bool hypervisor::on_ept_violation(std::size_t cpu,
                                  arch::x86_64::context & context,
                                  std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }

        // Last chance to use the controller the guest is about to take
        // away. See page_watch::before_write.
        if (watch.before_write) {
            watch.before_write(watch.context, page);
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

        // Emulate the write where the instruction can be decoded, and
        // only step over it where it cannot.
        //
        // Stepping over means opening the page, letting one instruction
        // retire and closing it again, and for that window the page is
        // writable *for every processor*. That is not a theoretical
        // hole: a driver writes CC twice in succession, disable then
        // enable, and the second write goes through the window the first
        // opened. Measured - one trapped write of 0x00460000, a
        // controller afterwards reading 0x00460001, and no transition
        // seen. Losing that transition is losing the channel, because a
        // reset is the one event the sink has to notice.
        //
        // Emulating closes the window by never opening it. The page
        // stays unwritable, every write faults, and each is applied by
        // this VMM with the value it decoded - so a second write cannot
        // slip past a first, and the handler is told what was written
        // rather than having to read the register back and race the
        // guest for it.
        // Only a violation caused by the instruction's own operand may
        // be emulated. Bit 8 of the exit qualification clear means the
        // access was to a paging-structure entry - the processor walking
        // the guest's tables - and there is no store in the instruction
        // to carry out for that. SDM Table 28-7.
        constexpr std::uint64_t qualification_linear_address_valid =
            1ull << 7;
        constexpr std::uint64_t qualification_operand_access = 1ull << 8;

        auto qualification = this->vmcs.exit_qualification();
        auto operand_access =
            (0 != (qualification & qualification_linear_address_valid)) &&
            (0 != (qualification & qualification_operand_access));

        if (auto store = (emulate_watched_page_writes && operand_access)
                             ? decode_guest_store(cpu, context)
                             : std::nullopt) {
            // A store that crosses the end of the watched page would be
            // applied whole at the faulting address, writing bytes onto
            // the page that follows. Refused rather than split: nothing
            // a driver does to a register straddles the page, so the
            // fallback costs nothing and a wrong split would be silent.
            auto offset_in_page = guest_physical & (page_size - 1);
            auto straddles =
                (offset_in_page + store->size) > page_size;

            if (!straddles && apply_guest_store(guest_physical, *store)) {
                if (watch.on_write) {
                    guest_write written{
                        .address = guest_physical,
                        .value = store->value,
                        .size = store->size,
                    };

                    watch.on_write(watch.context, page, &written);
                }

                this->emulated_writes = this->emulated_writes + 1;

                // The instruction has been carried out, so the guest
                // resumes after it rather than on it - by the length the
                // decoder measured, not the one the VMCS reports.
                //
                // SDM 30.2.5 leaves the VM-exit instruction length field
                // *undefined* for an EPT violation that was not
                // encountered during event delivery, and KVM agrees by
                // construction: handle_ept_violation never reads it, and
                // skip_emulated_instruction warns that it is not always
                // set. Advancing by an undefined value resumes the guest
                // somewhere inside its own instruction stream.
                //
                // Where the processor did supply a length and the two
                // disagree, the decoder has misread the instruction, and
                // the value already written to the device register makes
                // that unsafe to paper over. Recorded and the processor
                // stopped, rather than resumed at either address.
                auto reported = this->vmcs.vm_exit_instruction_length();
                if ((0 != reported) && (reported != store->length)) {
                    this->emulated_length_disagreement =
                        this->emulated_length_disagreement + 1;
                    this->emulated_length_reported = reported;
                    this->emulated_length_decoded = store->length;
                    return false;
                }

                this->vmcs.guest_rip(this->vmcs.guest_rip() +
                                     store->length);
                return true;
            }
        }

        // Not a form the decoder handles, so fall back to letting the
        // guest's own instruction do the write. This keeps the window
        // described above, and is why the decoder refusing is a
        // correctness question rather than only a performance one.
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(true);
            invalidate_ept();
        }

        this->stepped_writes = this->stepped_writes + 1;

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
            watch.on_write(watch.context, page, nullptr);
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
    //
    // The masks are used at their full width. They used to be truncated
    // to thirty two bits, which is wrong in the direction that silently
    // clears state: FIXED1 is an *allowed-1* mask, so `& 0xffffffff`
    // turns every bit above 31 into "must be 0" and the AND then clears
    // it from the control register. CR0 has nothing up there, but CR4
    // does - bit 32 is FRED - and host_cr4 is what guest_cr4 is derived
    // from, so the truncation would take the bit away from the guest as
    // well. Latent on this machine and wrong by construction.
    this->host_cr0 &= this->cached_vmx_msr(vmx_msr::cr0_fixed_1);
    this->host_cr0 |= this->cached_vmx_msr(vmx_msr::cr0_fixed_0);

    // The same for CR4, SDM A.8. VMXE is the bit this turns on in
    // practice, and vmxon cannot execute without it.
    this->host_cr4 &= this->cached_vmx_msr(vmx_msr::cr4_fixed_1);
    this->host_cr4 |= this->cached_vmx_msr(vmx_msr::cr4_fixed_0);
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

bool hypervisor::on_io_instruction(arch::x86_64::context & context,
                                   bool & re_execute)
{
    // SDM Table 30-5, "Exit Qualification for I/O Instructions". The port
    // is in bits 31:16 for the forms that carry one, which is every form
    // this VMM asks to see; direction is bit 3, with one meaning in; bits
    // 2:0 give the size of the access, and bit 4 says whether it was a
    // string instruction.
    auto qualification = this->vmcs.exit_qualification();
    auto port = static_cast<std::uint16_t>((qualification >> 16) & 0xffff);
    auto reading = 0 != (qualification & (1ull << 3));
    auto string_form = 0 != (qualification & (1ull << 4));
    auto size = qualification & 7;

    if ((0 == this->sleep_control_port) ||
        ((port != this->sleep_control_port) &&
         (port != this->sleep_control_port_secondary))) {
        return false;
    }

    // Only the write matters: reading the register tells the guest what
    // it already wrote and enters nothing.
    if (reading) {
        intercept_io_port(port, false);
        return true;
    }

    // The value the guest is writing, with no instruction decoder
    // involved.
    //
    // For a non-string OUT there is nowhere else it can come from: the
    // architecture defines OUT's source as AL, AX or EAX and nothing
    // else, and the exit qualification says which of the three by giving
    // the size of the access. The port comes out of the same field
    // whether it was named by DX or by an immediate, so the operand
    // encoding bit does not have to be looked at either.
    //
    // A string form - OUTS - would be reading from memory instead, and
    // there is no register to take it from. Windows does not use one to
    // write this register and no firmware does either, so rather than
    // guess, that case falls back to letting the guest execute its own
    // instruction, which is what this whole function used to do.
    if (string_form) {
        log("sleep control written by a string instruction, passed "
            "through");
        intercept_io_port(port, false);
        return true;
    }

    // Both the value and how wide the access was, from the same field.
    //
    // The width is carried on rather than re-derived from the table,
    // because the path that performs the write itself has to perform the
    // *guest's* instruction and not a corrected version of it. Table 30-5
    // defines only 0, 1 and 3 here and says "other values not used", so
    // anything else is reported as unknown rather than guessed at - the
    // register's own width from the fixed table is the fallback there, and
    // is the better answer for a value the architecture does not define.
    std::uint32_t value{};
    std::uint8_t bytes{};
    switch (size) {
    case 0:
        value = static_cast<std::uint8_t>(context.rax);
        bytes = 1;
        break;
    case 1:
        value = static_cast<std::uint16_t>(context.rax);
        bytes = 2;
        break;
    case 3:
        value = static_cast<std::uint32_t>(context.rax);
        bytes = 4;
        break;
    default:
        value = static_cast<std::uint32_t>(context.rax);
        bytes = 0;
        break;
    }

    re_execute = on_sleep_request(port, value, bytes);
    return true;
}

bool hypervisor::on_sleep_request(std::uint16_t port,
                                  std::uint32_t value,
                                  std::uint8_t bytes)
{
    // Not every write to this register enters anything. It also carries
    // SCI_EN, BM_RLD and GBL_RLS, and an operating system writes it while
    // running normally - so the register is watched, and only SLP_EN is
    // acted on.
    if (!power::asks_for_sleep(value)) {
        log("sleep control written {} with no enable, passed through",
            value);

        // Left armed. This was not the write worth seeing, so releasing
        // the port here would spend the one interception on a write that
        // entered nothing.
        return true;
    }

    this->sleep_request.port = port;
    this->sleep_request.value = value;
    this->sleep_request.sleep_type = power::sleep_type(value);
    this->sleep_request.processor = this->vmcs.vpid();
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::seen);
    this->sleep_request.occurred = 1;

    log("guest is entering sleep type {} through port {}",
        this->sleep_request.sleep_type,
        port);
    diag::log<diag::severity::warning>(
        "sleep type {} entered through port {}",
        this->sleep_request.sleep_type,
        port);

    // Everything staged in the channel, on the way out.
    //
    // Two calls and not one, because they do different things and the
    // second used to be missing. drain() empties the retention ring into
    // the sink; flush_pending() writes the sink's partially filled block
    // to the medium and waits for the controller to acknowledge it.
    // Without the flush, everything logged since the last full block -
    // which on a quiet machine is everything interesting about the
    // suspend - stayed in the staging buffer and went away with the
    // power.
    diag::pump::drain();
    if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
        diag::esp_block_sink::flush_pending();
    }
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::channel_flushed);

    // Where the platform will come back to, if it comes back at all.
    //
    // Read before anything else is decided, because it is the fact
    // everything else about a resume depends on and the only one that can
    // be established without risking a machine that does not wake. See
    // power::observe_waking_vector.
    if constexpr (power::observe_waking_vector) {
        observe_guest_waking_vector();
    }

    // Passed through rather than emulated, by releasing the port and
    // resuming *without* advancing past the instruction: the guest
    // re-executes its own OUT, which now reaches hardware. That needs no
    // decoder and cannot disagree with what the instruction meant.
    //
    // Releasing it also means this is seen once, which is the right
    // answer only while there is no resume path: a sleep is not worth
    // trapping twice, and re-arming has to happen on a resume.
    //
    // The cost of the pass-through is that this processor is still in VMX
    // operation with a VMCS current when the platform removes power, and
    // SDM 27.11.1 says a VMCS active on a logical processor that leaves
    // VMX operation may be corrupted - naming "removing power from the
    // processor (e.g., as part of a transition to the S3 and S4 power
    // states)" as one of the ways. It is harmless today only because
    // nothing ever reads those regions again. It stops being harmless the
    // moment a resume path does, which is what the quiesce below is for.
    if constexpr (!power::quiesce_on_sleep) {
        intercept_io_port(port, false);
        return true;
    } else {
        auto cpu = this->vmcs.vpid();

        // Point the platform's resume at this VMM, while there is still a
        // VMCS current and guest memory can still be reached through the
        // window - both of which the quiesce below takes away.
        //
        // A refusal here is not a failure of the suspend. It means the
        // table or the guest's own vector is not something that can be
        // resumed into, and the machine then does exactly what it did
        // before this existed: it sleeps, and it comes back
        // unvirtualized. Said out loud rather than silently skipped,
        // because "the resume did not happen" and "the resume was never
        // armed" are the two things a run has to be able to tell apart.
        if constexpr (power::resume_from_waking_vector) {
            if (auto armed = arm_resume_from_sleep(); !armed) {
                log("resume not armed, error {}; the machine will come "
                    "back unvirtualized",
                    armed.error().code());
                diag::log<diag::severity::warning>(
                    "resume not armed, error {}", armed.error().code());
            }
        }

        if (auto quiesced = quiesce_and_sleep(port, value, bytes);
            !quiesced) {
            // The write did not sleep the machine and this processor
            // could not be put back into VMX operation, so there is
            // nothing to resume into. Said out loud and then stopped,
            // rather than resumed into a VMCS that is not current - a
            // VMRESUME with no current VMCS produces no VM exit and no
            // recovery point, which is the failure that is impossible to
            // read afterwards.
            log("could not re-enter vmx operation after a sleep that did "
                "not happen, error {}",
                quiesced.error().code());
            diag::log<diag::severity::error>(
                "stranded outside vmx operation after a failed sleep");
            diag::pump::drain();
            if constexpr (diag::policy_of(diag::sink::esp_blocks)
                              .present) {
                diag::esp_block_sink::flush_pending();
            }
            for (;;) {
                arch::x86_64::disable_interrupts();
                arch::x86_64::halt();
            }
        }

        // Back in VMX operation with the launch state clear, which is
        // what the exit handler's tail has to know: VMRESUME requires
        // launched and VMCLEAR is the only thing that sets clear (SDM
        // 27.1), so leaving this guest has to be a VMLAUNCH.
        if ((0 != cpu) && (cpu <= max_cpus)) {
            this->relaunch_after_sleep[cpu - 1] = true;
        }
        return false;
    }
}

std::expected<hypervisor::waking_vector_record, zpp::error>
hypervisor::read_facs()
{
    if (0 == this->sleep_facs_physical) {
        return std::unexpected(zpp::error{error::no_usable_facs});
    }

    // Read as bytes and taken apart here, rather than reached through a
    // pointer into the mapping window.
    //
    // read_guest_physical is what carries the two things a hand-rolled
    // window mapping does not: a refusal for an address past what the
    // extended page tables describe, and the transfer window page rather
    // than page zero, which the diagnostic channel's queues use. It also
    // takes the window lock per page and releases it, so nothing is left
    // holding a pointer into a shared mapping.
    std::byte bytes[power::facs_minimum_length]{};
    if (auto read = read_guest_physical(this->sleep_facs_physical,
                                        std::span{bytes});
        !read) {
        return std::unexpected(read.error());
    }

    auto load32 = [&bytes](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, bytes + at, sizeof(value));
        return value;
    };
    auto load64 = [&bytes](std::size_t at) {
        std::uint64_t value{};
        std::memcpy(&value, bytes + at, sizeof(value));
        return value;
    };

    // Re-checked here and not only in the loader. The loader read this
    // table before the guest ever ran; between then and now the guest has
    // owned that memory and could have put anything there, and the whole
    // point of reading it is to decide whether to write to it later.
    waking_vector_record found{};
    if (power::facs_signature != load32(0)) {
        log("facs signature is {} rather than a facs, refused", load32(0));
        return std::unexpected(zpp::error{error::no_usable_facs});
    }

    found.length = load32(power::facs_length_offset);

    // The length and not the specification's minimum, because the table
    // says how long it is and that is the only bound that can be trusted
    // against a structure the guest has owned since the loader saw it.
    //
    // The loader's own check is weaker on purpose - it only demands room
    // for the thirty two bit field, which is all it reads - so it passes
    // tables this refuses. That is the right way round: it reads, and this
    // is what decides whether to write.
    found.usable = (found.length >= power::facs_minimum_length);
    if (!found.usable) {
        log("facs declares {} bytes, too short for both waking vectors",
            found.length);
        return found;
    }

    found.vector = load32(power::facs_waking_vector_offset);
    found.extended = load64(power::facs_extended_waking_vector_offset);
    return found;
}

bool hypervisor::observe_guest_waking_vector()
{
    auto found = read_facs();
    if (!found) {
        log("no waking vector to read, error {}", found.error().code());
        return false;
    }

    if (!found->usable) {
        return false;
    }

    this->guest_waking_vector = found->vector;
    this->guest_extended_waking_vector = found->extended;

    log("guest waking vector {} and extended {}",
        found->vector,
        found->extended);
    diag::log<diag::severity::warning>(
        "guest waking vector {} extended {}",
        found->vector,
        found->extended);

    // Both are reported because which one the guest used decides whether
    // this approach works at all. The thirty two bit field is entered in
    // real mode, which is the state a trampoline of ours could serve and
    // is what apply_start_up already builds. The extended field is entered
    // in long mode through a different protocol, and a guest that set it
    // and left the other zero cannot be resumed into by anything written
    // here yet.
    return 0 != found->vector;
}

std::uint64_t hypervisor::own_vmxon_region_physical()
{
    auto cpu = this->vmcs.vpid();
    if ((0 == cpu) || (cpu > max_cpus)) {
        return 0;
    }
    return this->host_page_table.virtual_to_physical(&this->vmx[cpu - 1]);
}

std::uint64_t hypervisor::own_vmcs_region_physical()
{
    auto cpu = this->vmcs.vpid();
    if ((0 == cpu) || (cpu > max_cpus)) {
        return 0;
    }
    return this->host_page_table.virtual_to_physical(
        &this->vmx_vmcs[cpu - 1]);
}

std::expected<void, zpp::error> hypervisor::quiesce_and_sleep(
    std::uint16_t port, std::uint32_t value, std::uint8_t bytes)
{
    // Which regions this processor is actually using, derived from the
    // VPID. vmx_physical and vmcs_physical cannot answer this - see
    // own_vmcs_region_physical.
    auto vmxon_region = own_vmxon_region_physical();
    auto vmcs_region = own_vmcs_region_physical();
    if ((0 == vmxon_region) || (0 == vmcs_region)) {
        return std::unexpected(zpp::error{error::no_region_for_processor});
    }

    // This processor only, and that is a limit rather than an oversight.
    //
    // The other processors are in the guest, and the only way to make one
    // execute VMCLEAR is to make it take a VM exit. It cannot be done for
    // a processor the guest has parked in wait-for-SIPI: SDM 28.2 says of
    // NMIs that "if a logical processor is in the wait-for-SIPI state,
    // NMIs are blocked. The NMI is not delivered and no VM exit occurs",
    // and SDM 29.7.2 repeats it for external interrupts, INIT and SMI.
    // From here there is no way to tell which state each processor is in,
    // and sending a wake NMI to one that may be in root mode has already
    // taken this machine down once - see the comment on
    // wait_for_ept_acknowledgement.
    //
    // So a rendezvous would work sometimes and hang or wedge otherwise,
    // which is the worst of the three outcomes. The consequence is
    // accepted instead and pushed onto the resume: every VMCS other than
    // this one may be corrupted per SDM 27.11.1, so a resume path must
    // rebuild all of them from scratch rather than trust any to have
    // survived. That is no harder than reusing them, and it is correct
    // whatever state the guest left its processors in.
    auto others = this->next_virtual_processor - 1;
    if (others > 1) {
        log("quiescing this processor only; {} others may be left with a "
            "corrupted vmcs",
            others - 1);
    }

    if (arch::x86_64::vmx::vmclear(&vmcs_region)) {
        return std::unexpected(zpp::error{error::vmclear_failed});
    }
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::vmcs_cleared);

    // Checked rather than discarded, unlike the scope guards that use this
    // on the failure paths out of main. There the processor is on its way
    // back to the loader whatever happens; here everything after it
    // depends on having actually left VMX operation, and a VMXOFF that
    // failed with the carry flag means this processor is still in it - so
    // the OUT below would run from root mode with a VMCS this function has
    // already cleared.
    if (arch::x86_64::vmx::vmxoff()) {
        return std::unexpected(zpp::error{error::vmxoff_failed});
    }
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::left_vmx_operation);

    // Everything written above has to be in memory rather than in a cache
    // when the power goes, and the guest's own flush has already happened
    // - it comes before the write that sleeps, because that write does not
    // return. So the writes this function has just made are on the wrong
    // side of it, and VMCLEAR's copy of the VMCS data to the region in
    // memory is among them.
    //
    // WBINVD and not INVD, for the reason invd() carries: INVD discards
    // modified lines without writing them back, which would throw away
    // exactly what this is here to preserve.
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::write_issued);
    arch::x86_64::wbinvd();

    // The guest's own write, at the width the guest's own instruction
    // used.
    //
    // Performed here rather than handed back because handing it back
    // requires a VM entry, and this processor has just left VMX operation
    // - there is nothing to enter. The whole point of the pass-through
    // path is that it "cannot disagree with what the guest meant", and
    // this path gives that up the moment it chooses a width for itself:
    // OUT's source is architecturally AL, AX or EAX and the exit
    // qualification says which (SDM Table 30-5, bits 2:0), so reproducing
    // the instruction is a matter of using that and nothing else.
    //
    // The register's width from the fixed ACPI description table was the
    // first answer here and is now only the fallback, for the undefined
    // encodings Table 30-5 leaves open. It was rejected as the primary
    // because it is wrong in the direction that writes bits the guest did
    // not: a two byte guest access issued as a four byte write puts zeroes
    // into bits 31:16 of whatever the platform decoded there, which is not
    // something the guest asked for. The other direction - a four byte
    // guest access issued as two - only drops bits that are reserved in
    // PM1_CNT, so it was the harmless half of a change that had a harmful
    // half.
    switch (bytes ? bytes : this->sleep_control_width) {
    case 1:
        arch::x86_64::out8(port, static_cast<std::uint8_t>(value));
        break;
    case 4:
        arch::x86_64::out32(port, value);
        break;
    default:
        arch::x86_64::out16(port, static_cast<std::uint16_t>(value));
        break;
    }

    // Only reached when the platform did not sleep. That is a normal
    // outcome, not a failure: a sleep can be refused, and a write to the
    // secondary control block does not sleep on its own.
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::write_returned);

    // Back into VMX operation with this processor's own regions.
    //
    // IA32_FEATURE_CONTROL has not been touched by anything - no reset has
    // happened on this path - but enable_vmx_in_feature_control is called
    // anyway rather than assumed, because it is the function that knows
    // what VMXON requires and it is cheap.
    if (auto allowed = enable_vmx_in_feature_control(); !allowed) {
        return allowed;
    }

    if (arch::x86_64::vmx::vmxon(&vmxon_region)) {
        return std::unexpected(zpp::error{error::vmxon_failed});
    }

    // VMCLEAR before VMPTRLD, and not only to set the launch state: this
    // region was active on a processor that left VMX operation, which is
    // the case SDM 27.11.1 says may corrupt it. The VMCLEAR above happened
    // first and is what makes that not so here; this one is the ordinary
    // requirement that VMLAUNCH needs a clear launch state.
    if (arch::x86_64::vmx::vmclear(&vmcs_region)) {
        return std::unexpected(zpp::error{error::vmclear_failed});
    }

    if (arch::x86_64::vmx::vmptrld(&vmcs_region)) {
        return std::unexpected(zpp::error{error::vmptrld_failed});
    }

    // The VMCS still holds every field it held before, because VMCLEAR
    // copies the data to the region rather than erasing it (SDM 27.11.1)
    // and nothing between then and now wrote to the region. So the guest
    // can be entered exactly where it was, and the port interception is
    // left armed - the guest is very likely about to try again.
    this->sleep_request.stage =
        static_cast<std::uint64_t>(power_stage::re_established);
    log("sleep write returned; back in vmx operation");
    return {};
}

std::expected<void, zpp::error> hypervisor::arm_resume_from_sleep()
{
    // Somewhere to come back to. The trampoline page is the only code this
    // VMM has that runs outside long mode, and without it there is no
    // resume to arm - a waking vector pointing into long mode code would
    // be entered in real mode and execute the first sixteen bits of it.
    if (0 == this->start_up_memory) {
        return std::unexpected(zpp::error{error::no_waking_vector});
    }

    auto found = read_facs();
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!found->usable) {
        return std::unexpected(zpp::error{error::no_usable_facs});
    }

    // Where the guest expects to continue, and there is no substitute for
    // it. Entering the guest anywhere else is the lie that *What the guest
    // is told* in CLAUDE.md is about, and a guest that left no real mode
    // vector cannot be served by this at all - the extended field is
    // entered through a different protocol.
    if (0 == found->vector) {
        return std::unexpected(zpp::error{error::no_waking_vector});
    }

    // And below one megabyte, because that is all the protocol can
    // express. The far pointer the firmware builds has a sixteen bit
    // segment, so a vector at or above 2^20 is truncated on the way into
    // it rather than refused - EDK2's AsmTransferControl shifts it right
    // by four and moves the result into BX, keeping sixteen bits. A guest
    // that left one up there is not resumable through this field by any
    // firmware, so this is refusing something already broken rather than
    // being strict.
    constexpr std::uint64_t highest_real_mode_vector = 0x100000;
    if (found->vector >= highest_real_mode_vector) {
        log("guest waking vector {} is not addressable in real mode",
            found->vector);
        return std::unexpected(zpp::error{error::no_waking_vector});
    }

    this->guest_waking_vector = found->vector;
    this->guest_extended_waking_vector = found->extended;

    // The trampoline, pointed at the resume rather than at an application
    // processor's launch.
    //
    // assembly_owned is put back first, for the reason
    // start_application_processor puts it back before every start: the
    // trampoline relocates the descriptor table pointer and the far
    // pointers by adding the page's base to them in place, which is
    // correct exactly once. A resume that ran on a blob some processor had
    // already climbed would add the base to values that already hold one
    // and fault on the far jump, before it has any interrupt descriptor
    // table - so the machine resets rather than reporting, which from the
    // outside is indistinguishable from firmware that never reached the
    // vector at all.
    auto & area = *reinterpret_cast<arch::x86_64::ap_start_up_area *>(
        this->start_up_memory + arch::x86_64::ap_start_up_area_offset);

    std::memcpy(
        area.assembly_owned,
        arch::x86_64::zpp_ap_start_up_begin +
            arch::x86_64::ap_start_up_area_offset +
            offsetof(arch::x86_64::ap_start_up_area, assembly_owned),
        sizeof(area.assembly_owned));

    // Slot zero, because the processor that comes back through the waking
    // vector is the boot processor and it takes its own slot again -
    // rewind_for_resume is what makes that slot free.
    area.entry =
        reinterpret_cast<std::uint64_t>(zpp_resume_from_sleep_main);
    area.argument = 0;
    area.stack_top =
        reinterpret_cast<std::uint64_t>(std::end(this->start_up_stack));

    // And the table, last: until this write the platform still resumes
    // into the guest, which is the outcome every failure above leaves in
    // place.
    //
    // The extended field is zeroed to force the real mode protocol. EDK2's
    // S3Resume.c takes the sixteen bit vector when XFirmwareWakingVector
    // is zero and a protected- or long mode path otherwise, so leaving a
    // non-zero extended field there would send the platform down a path
    // that ignores the vector written below.
    //
    // Both fields in one write, so there is no window in which the
    // platform would find our real mode vector next to the guest's
    // extended one and prefer the second. The span runs from the thirty
    // two bit vector to the end of the extended one, which puts the global
    // lock and the flags in the middle of it - and those belong to the
    // guest and the firmware between them, so they are read back and
    // written out unchanged rather than zeroed along with the field this
    // is here to clear.
    //
    // Offsets against the FACS structure EDK2 declares in
    // MdePkg/Include/IndustryStandard/Acpi65.h: FirmwareWakingVector at
    // 0x0c, GlobalLock at 0x10, Flags at 0x14, XFirmwareWakingVector at
    // 0x18.
    constexpr auto span =
        power::facs_minimum_length - power::facs_waking_vector_offset;
    constexpr auto between = power::facs_extended_waking_vector_offset -
                             power::facs_waking_vector_offset -
                             sizeof(std::uint32_t);

    std::byte replacement[span]{};

    if (auto read = read_guest_physical(
            this->sleep_facs_physical + power::facs_waking_vector_offset +
                sizeof(std::uint32_t),
            std::span{replacement + sizeof(std::uint32_t), between});
        !read) {
        return std::unexpected(read.error());
    }

    // Our trampoline's page, as the thirty two bit field. It is below one
    // megabyte by construction - initialize_start_up_memory refuses
    // anything else, because a start-up IPI vector is a page number in
    // eight bits - so the narrowing cannot lose anything.
    //
    // Copied as bytes rather than assigned through a pointer, so the
    // low-to-high order the table wants is the one memcpy gives on this
    // architecture and nothing depends on how a struct would be laid out.
    auto ours = static_cast<std::uint32_t>(this->start_up_memory);
    std::memcpy(replacement, &ours, sizeof(ours));

    // The extended field stays as the array's zero initialisation, which
    // is the point of writing this span at all.

    if (auto written = write_guest_physical(
            this->sleep_facs_physical + power::facs_waking_vector_offset,
            std::span{replacement});
        !written) {
        return std::unexpected(written.error());
    }

    this->resume_request.stage =
        static_cast<std::uint64_t>(resume_stage::armed);
    this->resume_request.guest_vector = this->guest_waking_vector;
    this->resume_request.occurred = 1;

    log("resume armed: waking vector {} taken over, guest vector {}",
        this->start_up_memory,
        this->guest_waking_vector);
    diag::log<diag::severity::warning>(
        "resume armed at {}, guest vector {}",
        this->start_up_memory,
        this->guest_waking_vector);
    return {};
}

void hypervisor::disarm_resume_from_sleep()
{
    // The trampoline first, because it is the half that cannot fail and
    // the half a start-up IPI needs: the guest is about to send
    // INIT-SIPI-SIPI to every other processor, and each of those goes
    // through start_application_processor, which puts assembly_owned back
    // but not entry. Leaving entry pointing here would send every
    // application processor into the resume path instead of its own
    // launch.
    if (0 != this->start_up_memory) {
        auto & area = *reinterpret_cast<arch::x86_64::ap_start_up_area *>(
            this->start_up_memory + arch::x86_64::ap_start_up_area_offset);
        area.entry = reinterpret_cast<std::uint64_t>(zpp_ap_start_up_main);
    }

    // Recorded here rather than at the end, because the trampoline is the
    // half that decides whether anything else can run and the FACS write
    // below is allowed to be skipped.
    this->resume_request.stage =
        static_cast<std::uint64_t>(resume_stage::disarmed);

    // And the guest's own table, put back exactly as it was read.
    //
    // Not required for anything here to work - the guest rewrites both
    // fields on its way into every suspend - and done anyway, because the
    // alternative is leaving a table the guest owns naming a trampoline of
    // ours. If this VMM then failed to come back on some later resume, the
    // guest would be running on bare hardware with its own resume pointing
    // at a page nothing maintains.
    //
    // Guarded on the address being non-zero, and that guard is doing real
    // work rather than restating the obvious: this is the one place in the
    // power path that *writes* to an address the loader supplied, and a
    // zero one would put the guest's saved vector at physical 0x0c.
    // Reaching here at all means the arm succeeded, so the address is set
    // - but a write reached only through a chain of shoulds is worth one
    // test.
    if (0 == this->sleep_facs_physical) {
        return;
    }

    constexpr auto span =
        power::facs_minimum_length - power::facs_waking_vector_offset;
    std::byte replacement[span]{};
    if (auto read = read_guest_physical(
            this->sleep_facs_physical + power::facs_waking_vector_offset,
            std::span{replacement});
        read) {
        // Narrowed explicitly rather than by copying four bytes out of the
        // front of a sixty four bit member, which would be reading a width
        // off an endianness. Both members hold what read_facs put in them,
        // so the thirty two bit one cannot lose anything.
        auto theirs =
            static_cast<std::uint32_t>(this->guest_waking_vector);
        auto extended = this->guest_extended_waking_vector;

        std::memcpy(replacement, &theirs, sizeof(theirs));
        std::memcpy(replacement +
                        (power::facs_extended_waking_vector_offset -
                         power::facs_waking_vector_offset),
                    &extended,
                    sizeof(extended));
        static_cast<void>(write_guest_physical(
            this->sleep_facs_physical + power::facs_waking_vector_offset,
            std::span{replacement}));
    }
}

void hypervisor::rewind_for_resume()
{
    // Which launches have happened, and after an S3 the answer is none.
    //
    // These two are what hand out slots, and every array below is indexed
    // by one. Rewinding them is what lets the resuming boot processor take
    // slot zero again and the guest's own start-up IPIs take one, two and
    // upwards after it - and not rewinding them is how max_cpus is reached
    // after a few suspends, with each suspend leaking as many slots as the
    // machine has processors.
    this->available_stack_index = 0;
    this->next_virtual_processor = 1;

    for (std::size_t cpu{}; cpu < max_cpus; ++cpu) {
        // Nothing is running. start_up_launched is the one that matters
        // most: wait_for_ept_acknowledgement waits on every processor it
        // marks launched, and a processor the platform has reset answers
        // nothing - so a stale mark here turns the first extended page
        // table change after a resume into a full-budget spin per
        // processor.
        this->start_up_launched[cpu] = false;
        this->processor_virtualized[cpu] = false;
        this->started_by_trampoline[cpu] = false;
        this->wake_requested[cpu] = false;
        this->relaunch_after_sleep[cpu] = false;
        this->controller_poll_armed[cpu] = false;

        // The start-up bookkeeping, or the guest's re-sent INIT-SIPI-SIPI
        // is refused. apply_start_up returns early for a processor whose
        // started_by_start_up_ipi is already set - which every application
        // processor's is, from before the suspend - and that early return
        // is indistinguishable from a processor that never started.
        this->started_by_start_up_ipi[cpu] = false;
        this->start_up_handoff[cpu].store(start_up_handoff_state::none);
        this->guest_start_up_vector[cpu] = 0;

        // A watch that was mid-step when the power went. The extended page
        // tables survive and so does whatever protection the step left,
        // but the processor that was stepping does not - so the step is
        // abandoned rather than waited for.
        this->stepping_watch[cpu] = false;
        this->stepping_page[cpu] = 0;

        // The nested VMX view each processor had of itself. setup_vmcs
        // seeds these per processor as it launches, and a stale
        // "in VMX operation" would make the first guest VMX instruction
        // after a resume answer against a shadow the guest has forgotten.
        this->guest_in_vmx_operation[cpu] = false;
        this->guest_vmxon_pointer[cpu] = 0;
        this->guest_current_vmcs[cpu] = nested_vmx::no_current_vmcs;

        // Caught up by construction: nothing has a cached translation,
        // because nothing has run. Left low it would name every processor
        // as outstanding.
        this->ept_generation_seen[cpu] =
            this->ept_generation.load(std::memory_order_acquire);
    }

    // The boot processor's own slot, and where its guest begins.
    //
    // started_by_trampoline is what tells main to skip the state capture,
    // the intermediate GDT and the page table switch - all of which are
    // wrong here for the same reason they are wrong for an application
    // processor: this processor arrived out of a reset by way of the
    // trampoline, so what it holds is not an operating system's.
    // The page the waking vector is in, and not the vector itself: this
    // array holds start-up IPI vectors, which are page numbers, and
    // apply_start_up reads it as one. apply_waking_vector is what puts the
    // remaining four bits back afterwards.
    this->started_by_trampoline[0] = true;
    this->guest_start_up_vector[0] = this->guest_waking_vector >> 12;

    // The controller has been through a power cycle, whatever it was doing
    // before. Marked down so the poll on the exit path notices it coming
    // back and rebuilds the queue, rather than submitting into queues the
    // controller no longer has.
    this->channel_controller_enabled = false;

    // Armed again rather than assumed to still be armed. The bitmap is in
    // module memory and survives, so this is usually a no-op - but the
    // pass-through path releases the port, and a machine that took that
    // path on an earlier suspend would come back watching nothing.
    if (this->sleep_control_port) {
        intercept_io_port(this->sleep_control_port, true);
        if (this->sleep_control_port_secondary) {
            intercept_io_port(this->sleep_control_port_secondary, true);
        }
    }

    // What makes main skip the once-per-boot setup. Set last, so that
    // everything above has happened before any of it can be acted on.
    this->resuming_from_sleep = true;

    this->resume_request.stage =
        static_cast<std::uint64_t>(resume_stage::rewound);
}

void hypervisor::apply_waking_vector()
{
    auto & vmcs = this->vmcs;
    auto vector = this->guest_waking_vector;

    // The firmware's own decoding, and nothing more than it. EDK2's
    // AsmTransferControl builds a far pointer whose segment is the vector
    // shifted right by four and whose offset is its low four bits, then
    // far-jumps through it in real mode - so the linear address the guest
    // begins at is the vector, reached as a segment plus an offset rather
    // than as a page.
    constexpr std::uint64_t real_mode_segment_limit = 0xffff;
    auto selector = (vector >> 4) & 0xffff;

    vmcs.guest_cs_selector(selector);
    vmcs.guest_cs_base(selector << 4);
    vmcs.guest_cs_limit(real_mode_segment_limit);
    vmcs.guest_rip(vector & 0xf);

    // The access rights apply_start_up already wrote are left alone: they
    // describe a sixteen bit read-executable segment, which is what this
    // is too. Only where it begins differs.
    log("guest entered at waking vector {}, cs {} rip {}",
        vector,
        selector,
        vector & 0xf);
}

void hypervisor::resume_from_sleep_on_this_processor(std::uint64_t slot)
{
    this->resume_request.occurred = 1;
    this->resume_request.count = this->resume_request.count + 1;
    this->resume_request.stage =
        static_cast<std::uint64_t>(resume_stage::entered);

    // Every lock, forced open, and this is the first thing done rather
    // than part of the rewind below - because the rewind's own first act
    // would otherwise be to wait on one of them.
    //
    // A spin lock's state is a byte in memory, and S3 preserves memory.
    // Both of these are taken and released inside a VM exit, so the write
    // that slept the machine could land while another processor held one:
    // mapping_window_lock is held across every guest memory access and
    // across the whole of rebuild_channel_queue, start_up_lock across a
    // processor's entire launch. That processor no longer exists - the
    // platform reset it - so nothing will ever release what it held, and
    // the first use of that lock after a resume spins for good. It would
    // look exactly like a resume that never arrived.
    //
    // Safe to force, and only here: this processor is the only one
    // running, by the same argument the resume rests on throughout - the
    // platform brings the boot processor up alone and the guest has not
    // yet sent an INIT-SIPI-SIPI to anything. The same for the two outside
    // this class, and the log's has to come before the first log line
    // rather than after it. append holds its lock across a push_back,
    // which allocates, so a transition caught in there leaves both held -
    // see the note on each abandon_lock.
    this->mapping_window_lock.unlock();
    this->start_up_lock.unlock();
    log_storage::abandon_lock();
    crt::heap().abandon_lock();

    // And the diagnostic channel's gates, whose failure is the quiet one:
    // a gate left held makes the channel silent rather than stuck, on the
    // path whose only job is to say whether this worked.
    diag::pump::abandon_gates();

    log("resumed from sleep on slot {}, guest vector {}",
        slot,
        this->guest_waking_vector);

    disarm_resume_from_sleep();
    rewind_for_resume();

    // Twice returning, exactly as start_up_on_this_processor does it and
    // for the same reason: this processor arrived on a waking vector
    // rather than a call, so there is nowhere to return to and nothing
    // left to do with it if the launch comes back.
    std::atomic<bool> launch_returned;
    launch_returned = false;

    arch::x86_64::context context{};
    arch::x86_64::capture_context(&context);

    if (launch_returned) {
        log("resume failed to launch on slot {}", slot);
        diag::log<diag::severity::error>("resume failed to launch");
        diag::pump::drain();
        if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
            diag::esp_block_sink::flush_pending();
        }
        for (;;) {
            arch::x86_64::disable_interrupts();
            arch::x86_64::halt();
        }
    }
    launch_returned = true;

    // What main reads out of the context, the same three fields
    // start_up_on_this_processor sets: which processor this is, and no
    // physical to virtual translation, since the once per boot setup that
    // would use one is not going to run.
    context.rdi = slot;
    context.rsi = 0;
    context.rdx = 0;

    this->resume_request.stage =
        static_cast<std::uint64_t>(resume_stage::launched);

    launch_on_cpu(context);
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

void hypervisor::on_controller_register_before_write(void * context,
                                                     std::uint64_t page)
{
    static_cast<void>(context);
    static_cast<void>(page);

    // Behind `if constexpr` for the usual reason: naming a static member
    // of a class template odr-uses it, and check-diag-absent.sh fails a
    // release build that carries the channel.
    if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
        diag::esp_block_sink::flush_pending();
    }
}

void hypervisor::on_controller_register_write(
    void * context,
    std::uint64_t page,
    const hypervisor::guest_write * write)
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

        // The value the guest wrote, taken from the decoded
        // instruction where there is one.
        //
        // Reading the register back instead is what this used to do, and
        // it loses transitions: the read happens after the write, so a
        // driver that writes CC twice in quick succession is observed
        // only at whatever the register holds by the time this looks.
        // Measured - a trapped write of 0x00460000 followed by a
        // controller reading 0x00460001, and the disable never seen.
        // The decoded value is what the instruction meant, and cannot
        // have been overtaken.
        //
        // Only a write that lands on CC itself counts. The page carries
        // other registers, and a store to one of them says nothing about
        // whether the controller is being turned off.
        constexpr auto configuration_offset =
            nvme::offset_of(nvme::register_offset::configuration);

        auto * bar =
            static_cast<volatile std::uint8_t *>(self.channel_bar);

        std::uint32_t configuration_value{};

        if (write) {
            auto bar_physical = self.host_page_table.virtual_to_physical(
                const_cast<const void *>(self.channel_bar));

            if (write->address != (bar_physical + configuration_offset)) {
                return;
            }

            configuration_value = static_cast<std::uint32_t>(write->value);
        } else {
            // Stepped over rather than emulated, so the register is the
            // only place the value can be had - with the race above.
            configuration_value =
                arch::x86_64::read32(bar + configuration_offset);
        }

        auto configuration =
            nvme::controller_configuration{configuration_value};

        ++self.channel_register_writes;
        self.channel_last_configuration = configuration.value();

        // Every value, in order, for the observation build.
        //
        // The last value alone cannot answer the question this is for.
        // Whether the guest's low power entry sets the shutdown
        // notification without ever clearing the enable bit is a question
        // about the *sequence*, and this VMM acts only on the enable bit
        // - so a shutdown that leaves it set is invisible to everything
        // else here.
        if constexpr (diag::observe_controller_admin) {
            auto slot = self.configuration_trace_count %
                        configuration_trace_capacity;
            self.configuration_trace[slot] = configuration.value();
            self.configuration_trace_count =
                self.configuration_trace_count + 1;
        }

        auto now = configuration.enable();
        auto was = self.channel_controller_enabled;
        self.channel_controller_enabled = now;

        if (was && !now) {
            // Going down. Take the controller while it is nobody's and
            // write out what is staged, then let go.
            //
            // The guest's view of the register page is redirected first,
            // so a driver polling CSTS on another processor keeps seeing
            // the controller it has just disabled rather than the one this
            // is briefly enabling. Everything is put back however the
            // excursion ends.
            if constexpr (diag::excursion_at_controller_reset) {
                if (auto shadowed =
                        self.shadow_controller_registers(true)) {
                    // Not discarded. Pointing the entry back at the
                    // real registers is the one step here that must not
                    // fail quietly: a guest left reading the frozen
                    // shadow would never see its controller return, which
                    // is far worse than the excursion not happening.
                    //
                    // A failure here is "not confirmed" rather than
                    // "still shadowed" - the entry is written before the
                    // acknowledgement is waited for, so what failed is
                    // the proof that every processor has picked it up,
                    // not the change itself. Counted so a reader can see
                    // it happened at all.
                    scope_exit unshadow{[&] {
                        if (!self.shadow_controller_registers(false)) {
                            self.unshadow_unconfirmed =
                                self.unshadow_unconfirmed + 1;
                        }
                    }};

                    // Translated, not cast. hold_guest_page takes a guest
                    // physical address and channel_bar is a host virtual
                    // pointer; casting one to the other is right only for
                    // as long as that mapping happens to be the identity,
                    // which is not a property this VMM establishes or
                    // checks. The same value is translated properly a few
                    // lines above to compare against the faulting
                    // address, so the two spellings sat next to each
                    // other disagreeing.
                    auto bar_page =
                        self.host_page_table.virtual_to_physical(
                            const_cast<const void *>(self.channel_bar));

                    auto held = self.hold_guest_page(bar_page);
                    scope_exit unhold{[&] {
                        if (held) {
                            self.release_guest_page(bar_page);
                        }
                    }};

                    if (auto ran = self.run_reset_excursion(); !ran) {
                        self.excursions_refused =
                            self.excursions_refused + 1;
                        self.excursion_error = ran.error().code();
                    } else {
                        self.excursions_completed =
                            self.excursions_completed + 1;
                    }
                } else {
                    self.excursions_refused = self.excursions_refused + 1;
                    self.excursion_error = shadowed.error().code();
                }
            }

            // The queues are gone with it either way: the excursion hands
            // the controller back disabled, as the guest asked.
            diag::esp_block_sink::note_controller_write(
                configuration_value);
        } else if (!was && now) {
            // Coming back up. Handled here when the write is caught, and
            // by poll_for_controller_return when it is not - see there
            // for why catching it cannot be relied on.
            self.rebuild_channel_queue();
        }
    }
}

void hypervisor::on_doorbell_write(void * context,
                                   std::uint64_t page,
                                   const hypervisor::guest_write * write)
{
    static_cast<void>(page);

    if constexpr (!diag::observe_controller_admin) {
        static_cast<void>(context);
        static_cast<void>(write);
        return;
    } else {
        auto & self = *static_cast<hypervisor *>(context);

        self.channel_doorbell_writes = self.channel_doorbell_writes + 1;

        if (!write || !self.channel_bar) {
            return;
        }

        // Only the admin submission queue's doorbell, which is the first
        // on the page. Every other doorbell on it belongs to an I/O queue
        // and carries the guest's ordinary disk traffic, which is
        // counted above and otherwise ignored.
        if (0 != (write->address & (page_size - 1))) {
            return;
        }

        auto * bar = static_cast<volatile std::uint8_t *>(self.channel_bar);

        // Where the guest put its admin submission queue, and how big it
        // said it was. Read from the controller rather than remembered,
        // because the driver may have moved it since - this runs from the
        // driver's own initialisation, which is when it sets them.
        auto queue_base = arch::x86_64::read64(
            bar + nvme::offset_of(
                      nvme::register_offset::admin_submission_queue_base));
        auto attributes = arch::x86_64::read32(
            bar +
            nvme::offset_of(nvme::register_offset::admin_queue_attributes));

        auto entries =
            (attributes & 0xfff) + 1; // ASQS is a zero's based count
        if (!queue_base || !entries) {
            return;
        }

        auto tail = static_cast<std::uint32_t>(write->value);
        if (tail >= entries) {
            return;
        }

        constexpr std::uint32_t command_size = 64;

        // Everything between what was looked at last and what the guest
        // has just published. Wrapping is why this is a loop rather than
        // a subtraction.
        self.mapping_window_lock.lock();
        scope_exit release{[&] { self.mapping_window_lock.unlock(); }};

        for (auto at = self.admin_observed_head; at != tail;
             at = (at + 1) % entries) {
            auto offset = static_cast<std::uint64_t>(at) * command_size;
            auto * command = static_cast<const std::uint32_t *>(
                self.map_window_at(transfer_window_first_page,
                                   queue_base + offset,
                                   1));
            if (!command) {
                break;
            }

            // Freezes when full rather than wrapping, which is the
            // opposite of every other ring here and deliberate.
            //
            // The commands this exists to see all arrive in one burst
            // while the driver initialises: Identify, Set Features
            // (Number of Queues), and the Create I/O Queue pair for each
            // processor. Everything after that is steady state, and
            // steady state is *chatty* - measured, it is Set Features on
            // the power management feature alternating between two power
            // states, for ever. A wrapping ring fills with that and
            // throws away the only part anyone wanted. First N, not last
            // N.
            if (self.admin_observation_count >=
                admin_observation_capacity) {
                self.admin_observation_count =
                    self.admin_observation_count + 1;
                continue;
            }

            auto slot = self.admin_observation_count;

            self.admin_observations[slot] = admin_observation{
                .command = command[0],
                .dword_10 = command[10],
                .dword_11 = command[11],
                .namespace_id = command[1],
            };

            self.admin_observation_count =
                self.admin_observation_count + 1;
        }

        self.admin_observed_head = tail;
    }
}

void hypervisor::on_local_apic_write(void * context,
                                     std::uint64_t page,
                                     const hypervisor::guest_write * write)
{
    // The decoded value is not used here: this handler cares that the
    // interrupt command register was written, and reads the whole
    // command out of the page, which is two registers wide in xAPIC
    // mode and so wider than any single store.
    static_cast<void>(write);

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
    if (arch::x86_64::vmx::invvpid(invvpid_single_context, &descriptor)) {
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

void hypervisor::inject_invalid_opcode_exception()
{
    constexpr std::uint64_t invalid_opcode_vector = 6;

    // A fault, like the general protection fault above, so the guest
    // resumes at the instruction rather than past it - callers must not
    // advance RIP.
    //
    // deliver_error_code is deliberately absent. SDM 29.2.1.3 lists the
    // vectors for which the bit must be 1 - #DF, #TS, #NP, #SS, #GP, #PF
    // and #AC - and requires it to be 0 for vectors in the ranges 0-7, 9,
    // 15, 16 and 18-31. Vector 6 is in the first of those, so setting it
    // would fail VM entry rather than deliver anything.
    this->vmcs.vm_entry_interruption_information_field(
        invalid_opcode_vector |
        arch::x86_64::vmx::vm_entry_interruption::hardware_exception |
        arch::x86_64::vmx::vm_entry_interruption::valid);
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

extern "C" void zpp_vmx_entry_failed(std::uint64_t flags)
{
    auto & self = zpp::hypervisor::hypervisor::instance();

    self.record_entry_failure(flags);
}

void hypervisor::record_entry_failure(std::uint64_t flags)
{
    this->entry_failures_seen = this->entry_failures_seen + 1;

    // vpid is readable only if a VMCS is current, which carry says it is
    // not - so fall back to a slot of zero rather than reading garbage.
    constexpr std::uint64_t carry = 1;
    auto slot = (flags & carry) ? std::uint64_t{} : this->vmcs.vpid();

    if ((0 != slot) && (slot <= max_cpus)) {
        this->entry_failure_flags[slot - 1] = flags;
        this->entry_failure_error[slot - 1] =
            this->vmcs.vm_instruction_error();
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

    // The nested VMX state for this processor, seeded before it runs a
    // single guest instruction.
    //
    // The current-VMCS pointer starts at the architecture's own sentinel
    // rather than at zero, because zero is a legal physical page and
    // everything that asks "is there a current VMCS" compares against it.
    // Zero-initialized storage would therefore claim page zero is current.
    //
    // IA32_FEATURE_CONTROL starts as the hardware's, which is the value
    // the guest would have found had this VMM not been here: firmware sets
    // and locks it long before any operating system runs, and where it did
    // not, enable_vmx_in_feature_control did during launch. Copying it
    // rather than fabricating one keeps a guest that is refused VMX by its
    // own firmware refused here too - that decision is the platform
    // owner's, not this VMM's.
    if (auto cpu = this->next_virtual_processor - 1; cpu < max_cpus) {
        this->guest_in_vmx_operation[cpu] = false;
        this->guest_vmxon_pointer[cpu] = 0;
        this->guest_current_vmcs[cpu] = nested_vmx::no_current_vmcs;
        this->guest_feature_control[cpu] =
            arch::x86_64::rdmsr(arch::x86_64::msr::ia32_feature_control);

        // And the second-level VMCS, which this is the only place that
        // prepares. VMCLEAR is what puts the launch state where VMLAUNCH
        // needs to find it, and it is executed exactly once per processor
        // because every entry afterwards rewrites every field - so there
        // is never cached state in it worth writing back, which is the
        // other thing VMCLEAR does and the reason not to repeat it.
        //
        // It leaves this VMM's own VMCS current: SDM 27.1 has VMCLEAR make
        // the *named* VMCS inactive and not current, and this names the
        // other one.
        if constexpr (nested_vmx::enabled) {
            auto & region = this->vmcs02[cpu];

            region.revision_id = static_cast<std::uint32_t>(
                this->cached_vmx_msr(vmx_msr::basic) & 0xffffffff);
            region.abort_indicator = 0;

            this->vmcs02_physical[cpu] =
                this->host_page_table.virtual_to_physical(&region);

            this->running_l2[cpu] = false;
            this->vmcs02_launched[cpu] = false;
            this->nested_bitmaps_merged[cpu] = false;

            if (arch::x86_64::vmx::vmclear(&this->vmcs02_physical[cpu])) {
                log("cpu {} could not clear its second level vmcs", cpu);
            }
        }
    }

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
        this->sleep_control_width = launch.sleep_control_width;
        this->sleep_facs_physical = launch.sleep_facs_physical;

        // Where this module was put. Inside the guard with the rest,
        // because a processor this VMM started has no launch block and
        // would otherwise write a null over the answer the boot
        // processor already found - which is exactly how the sleep
        // control port was lost once.
        this->handed_over_module_base = launch.module_base;
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

    // Whether the once-per-boot setup below has to run, which is not the
    // same question as "is this the boot processor".
    //
    // It is that on a first boot, and it is not on an S3 resume: the boot
    // processor comes back through the trampoline with slot zero, and
    // everything the once-per-boot setup builds describes physical memory
    // that S3 preserved and did not move - the host page table, the host
    // GDT and IDT, the extended page tables, the module's own protection.
    // Building it again would at best repeat work; at worst
    // initialize_os_page_table would read a page table that is no longer
    // any operating system's, because the resuming processor is already on
    // the host page table and the guest's CR3 belongs to a guest that has
    // not run yet.
    //
    // What does run again is everything per-processor: initialize_vmx,
    // enter_root_mode, setup_vmcs. Those are what a power transition
    // actually destroyed - IA32_FEATURE_CONTROL is zero again (SDM 26.7),
    // CR4.VMXE is clear, and no VMCS is current.
    auto first_launch = (0 == cpuid) && !this->resuming_from_sleep;

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

    // Perform only on first CPU load. Not on a resume - see first_launch.
    if (first_launch) {
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

        // The MSRs nested VMX answers, armed here for the same reason and
        // in the same place: one shared bitmap, set up before a second
        // processor exists.
        //
        // Both blocks are inside the range the bitmap covers, so without
        // this they reach hardware and the guest reads the real
        // capabilities of the processor this VMM is already using - which
        // is not a subset of anything, and IA32_FEATURE_CONTROL of which
        // is locked by enable_vmx_in_feature_control before any guest
        // runs.
        if constexpr (nested_vmx::enabled) {
            intercept_msr(
                arch::x86_64::msr::ia32_feature_control, true, true);

            for (auto msr = arch::x86_64::vmx::msr::begin;
                 msr < arch::x86_64::vmx::msr::end;
                 ++msr) {
                intercept_msr(static_cast<std::uint32_t>(msr), true, true);
            }

            log("nested vmx: reporting vmx to the guest");
        }

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

    // Perform only on first CPU load. Not on a resume - see first_launch.
    if (first_launch) {
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
                    // Translated, not cast. The hand-over carries host
                    // virtual pointers and a page watch is keyed by guest
                    // physical address; the two agree only while the BAR
                    // mapping is the identity, which nothing here
                    // establishes. on_controller_register_write already
                    // translates the same pointer to compare against the
                    // faulting address, so leaving this a cast made the
                    // watch and its own handler disagree about what they
                    // were watching.
                    auto register_page =
                        this->host_page_table.virtual_to_physical(
                            const_cast<const void *>(
                                handover.configuration_register)) &
                        ~(page_size - 1);
                    if (auto armed = watch_guest_page_writes(
                            register_page,
                            &hypervisor::on_controller_register_write,
                            this,
                            page_watch::mode::notify,
                            &hypervisor::
                                on_controller_register_before_write);
                        !armed) {
                        log("could not watch the controller register "
                            "page");
                    }

                    // The doorbell page, for the observation build only.
                    //
                    // Left armed rather than armed around a borrow, which
                    // is the opposite of what the rebuild does with the
                    // same page and for the opposite reason: the rebuild
                    // wants the guest excluded for a moment, this wants
                    // to see everything it submits. With a doorbell
                    // stride of zero that is every command on every
                    // queue, which is exactly the cost this is meant to
                    // price.
                    if constexpr (diag::observe_controller_admin) {
                        auto doorbell_page =
                            register_page +
                            nvme::offset_of(
                                nvme::register_offset::doorbell_base);

                        if (auto watching = watch_guest_page_writes(
                                doorbell_page,
                                &hypervisor::on_doorbell_write,
                                this,
                                page_watch::mode::notify);
                            !watching) {
                            log("could not watch the doorbell page");
                        } else {
                            log("observing the controller's admin queue");
                        }
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
    // Not on a resume either, and this one for a sharper reason than the
    // two blocks above: the trampoline page is the code this processor is
    // coming back through, and initialize_start_up_memory rewrites it -
    // the blob, the temporary page table, and `entry`. Doing that here
    // would overwrite the resume's own arrangement with the application
    // processor one, which is exactly what disarm_resume_from_sleep has
    // already done deliberately and in the right order.
    if (first_launch && start_up_memory) {
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

        // And, on a resume, at the exact address rather than at the page.
        //
        // apply_start_up takes a start-up IPI vector, which is a page
        // number, so it can only ever begin a guest at a page boundary. A
        // firmware waking vector is a full byte address and the ACPI real
        // mode protocol does not require it to be aligned at all - EDK2's
        // AsmTransferControl far-jumps to (vector >> 4):(vector & 0xf), so
        // the low four bits land in IP rather than in the segment. Applied
        // as a correction rather than by generalising apply_start_up,
        // because for a page aligned vector it writes the same three
        // values apply_start_up just did, which makes it self-checking.
        //
        // The slot test is load bearing rather than tidiness.
        // resuming_from_sleep stays set for the rest of the boot - it has
        // to, being what keeps the once-per-boot setup from running again
        // - and every application processor the guest starts *after* a
        // resume comes through this same branch. Without the test each of
        // them would have its entry point moved to the boot processor's
        // waking vector instead of the one its own start-up IPI named.
        if (this->resuming_from_sleep && (0 == cpuid)) {
            apply_waking_vector();
        }
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

        reason = full_reason.basic();

        // Out of the VMCS, because what the exit stub's capture left in
        // this field is its own return address, not the guest's RIP.
        context.rip = vmcs.guest_rip();

        // Whether the exit was caused by an instruction the guest should
        // be resumed past. Cleared by the handlers for which it is not.
        bool advance_rip = true;

        // A second-level guest's exit is decided before anything else
        // looks at it, because "whose exit is this" is a different
        // question from "what does it mean" and has to be asked first.
        //
        // Three answers. Reflected: the guest hypervisor's own VMCS is
        // current again and nothing below applies. Handled: answered
        // completely by the composition of the two levels of extended page
        // tables, which is the one thing only that path knows. Deferred:
        // the cases below answer it with the second-level VMCS current,
        // which is what keeps one piece of code answering an intercept
        // whichever guest ran into it.
        //
        // Nothing a second-level guest does reaches `default:` below,
        // which would stop the processor: an exit neither this VMM nor the
        // guest hypervisor asked for cannot happen, since the controls
        // that produced it are the union of the two, and everything not
        // named in the decision is reflected.
        if constexpr (nested_vmx::enabled) {
            if (auto slot = vmcs.vpid(); (0 != slot) &&
                                         (slot <= max_cpus) &&
                                         this->running_l2[slot - 1]) {
                if (l2_exit_outcome::deferred !=
                    on_l2_exit(
                        slot - 1, full_reason, context, advance_rip)) {
                    resume_guest(context, full_reason, advance_rip);
                }
            }
        }

        // A failed VM entry arrives here looking like an exit, so it has
        // to be separated out before anything treats it as one. Nothing
        // below applies to it: the guest did not run, the instruction
        // length field describes no instruction, and resuming would fail
        // the same way again.
        //
        // After the nested decision rather than before it, because an
        // entry into a *second-level* guest that failed is one the guest
        // hypervisor asked for and has to be told about, where this one is
        // a bug here with nothing left to do but stop.
        if (full_reason.entry_failure()) {
            on_vm_entry_failure(full_reason);
        }

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
        // - the VMX instructions fault with #UD, because the guest has
        //   been told VMX is absent and that is what a processor without
        //   it does. They used to reach `default:` and halt, which let a
        //   guest instruction stop a processor.
        // - anything else stops the CPU instead of being resumed from,
        //   because resuming advances RIP past an instruction that never
        //   took effect. Nothing a *guest* can execute should reach it:
        //   a case that faults is always available, and is better.
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

            // Nothing retired to produce this exit. The instruction
            // length field is defined only for exits due to instruction
            // execution and for software interrupts and exceptions - SDM
            // 30.2.3 - so for an event-caused exit it holds whatever the
            // last instruction-caused exit left there. Advancing by it
            // resumes the guest mid-instruction, and this VMM sends
            // itself NMIs, so every wake that lands on a running
            // processor would do it.
            advance_rip = false;

            if (is_nmi && (0 != cpu) && (cpu <= max_cpus) &&
                this->wake_requested[cpu - 1].exchange(
                    false, std::memory_order_acq_rel)) {
                break;
            }

            if (is_nmi) {
                // An NMI may only be injected at an instruction boundary
                // that is not inside an interrupt shadow. SDM 29.3.1.5
                // makes it a hard VM-entry check: blocking by STI and
                // blocking by MOV SS must both be clear when the
                // injected event type is NMI or external interrupt. The
                // interruptibility state is saved as it was before the
                // exit - SDM 30.3.4 - and an NMI recognised at a `sti;
                // hlt` boundary, which is how an idle Windows processor
                // waits, saves it with blocking by STI set.
                //
                // So the shadow is cleared before injecting. The
                // alternative KVM takes is to refuse the injection and
                // open an NMI window instead (vmx_nmi_blocked and
                // vmx_nmi_allowed); clearing is correct here because the
                // instruction the shadow belonged to has already retired
                // - the exit was taken after it - so the boundary it
                // described no longer exists.
                //
                // Getting this wrong does not lose the NMI, it fails the
                // next VM entry, and a failed entry produces no VM exit -
                // so it stops the processor with nothing recorded.
                constexpr std::uint64_t blocking_by_sti = 1ull << 0;
                constexpr std::uint64_t blocking_by_mov_ss = 1ull << 1;

                vmcs.guest_interruptibility_state(
                    vmcs.guest_interruptibility_state() &
                    ~(blocking_by_sti | blocking_by_mov_ss));

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

                // Hide VMX, unless the nested machinery is compiled in.
                //
                // Hidden is the default and the reason is not that the
                // instructions cannot be answered - they are, in
                // nested_vmx.cpp - but that VMLAUNCH cannot be. A guest
                // hypervisor told VMX exists gets as far as a fully
                // written VMCS and is then refused, and for Hyper-V, which
                // launches ahead of Windows whenever VBS is on, that is a
                // failure at launch rather than the clean stand-down it
                // performs when it finds no VMX at all.
                //
                // Bit 5 of leaf 1 ECX is the VMX bit, and it is the one
                // half of a pair: the other is CR4.VMXE, which the read
                // shadow answers for. The two must agree, because the
                // combination "no VMX in CPUID, VMXE set in CR4" exists on
                // no real processor and a guest that trusts CR4 over CPUID
                // then faults on its own vmxon - which is exactly the
                // defect BACKLOG.md records as item 1. Both are keyed on
                // the same constant so they cannot drift apart.
                if constexpr (!nested_vmx::enabled) {
                    cpuid_result[2] &= ~(1u << 5);
                }

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
            // The MSRs nested VMX owns, which are only armed in the bitmap
            // when it is compiled in. Taken before the interrupt command
            // register below because the two sets do not overlap and the
            // order costs nothing; taken before the fault below because
            // this is precisely the case where an in-range MSR access
            // exits for a reason other than being unimplemented.
            if (on_nested_vmx_msr_write(
                    static_cast<std::uint32_t>(context.rcx), context)) {
                // A locked IA32_FEATURE_CONTROL or a capability MSR
                // answers with a general protection fault, and a fault is
                // reported at the faulting instruction.
                constexpr std::uint64_t injection_valid = 1ull << 31;
                if (0 !=
                    (vmcs.read(
                         arch::x86_64::vmx::vmcs::field::
                             vm_entry_interruption_information_field) &
                     injection_valid)) {
                    advance_rip = false;
                }
                break;
            }

            // The guest arming its own next timer interrupt, which this
            // VMM asked to see only because the preemption timer was
            // refused. The exit *is* the point: the record draining below
            // runs for every exit regardless of reason, so a guest that
            // has settled still produces one of these per tick and the
            // channel keeps moving.
            //
            // The write is then performed exactly as the guest wrote it.
            // The local APIC underneath is the guest's own - nothing here
            // virtualizes it - and no time stamp counter offset is
            // programmed, so the value is already in the time base the
            // hardware expects and needs no adjustment.
            //
            // A write the guest would have faulted on faults here instead,
            // in the host. The conditions are architectural and depend on
            // the timer's mode in the same local APIC, so a write that
            // faults for this side is one that would have faulted for the
            // guest - the difference is only where it lands. Worth
            // knowing about; not worth guarding against by second
            // guessing the guest's own APIC state.
            if (auto index = static_cast<std::uint32_t>(context.rcx);
                (arch::x86_64::msr::ia32_tsc_deadline == index) ||
                (arch::x86_64::msr::ia32_x2apic_init_count == index)) {
                arch::x86_64::wrmsr(
                    index, (context.rax & 0xffffffff) | (context.rdx << 32));
                break;
            }

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
            // The read half of the same set. Answered before the fault
            // below for the same reason: with nested VMX compiled in the
            // bitmap is no longer all zeroes, so an in-range MSR can exit
            // because this VMM asked to see it rather than because it does
            // not exist.
            if ((basic_reason::rdmsr == reason) &&
                on_nested_vmx_msr_read(
                    static_cast<std::uint32_t>(context.rcx), context)) {
                break;
            }

            // SDM 28.1.3 lists, among the reasons RDMSR causes a VM exit,
            // that "the MSR address is not in the ranges 00000000H -
            // 00001FFFH and C0000000H - C0001FFFH". Accesses inside those
            // ranges are governed by the bitmap, which is all zeroes
            // unless nested VMX armed something in it, so they otherwise
            // never exit. Outside them the access exits unconditionally
            // and no bitmap can stop it.
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
            bool re_execute = true;
            if (!on_io_instruction(context, re_execute)) {
                record_exit(full_reason);
                on_unhandled_exit(full_reason);
                break;
            }

            // RIP is left alone whenever the guest still has to run its
            // own instruction: the handler released the port, so
            // re-executing it is what performs the access. It advances
            // only when this VMM performed the access itself, because
            // then the instruction has had its effect and resuming at it
            // would repeat it.
            advance_rip = !re_execute;
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

            // What the guest is allowed to see in the bit it just wrote.
            //
            // With nesting off it always reads back clear, so the guest's
            // view agrees with the CPUID leaf that told it there is no
            // VMX. With nesting on it reads back what the guest asked for,
            // because the guest is entitled to turn VMX on and see that it
            // did. The real register keeps the bit either way: a processor
            // in root mode must have it set, and IA32_VMX_CR4_FIXED0 says
            // so.
            //
            // One refusal on top of that, and it is the guest's own rule
            // rather than this VMM's: SDM 26.8 says "Once in VMX
            // operation, it is not possible to clear CR4.VMXE". A guest
            // that has executed VMXON and then tries to clear the bit gets
            // the general protection fault hardware would have given it,
            // with RIP left on the instruction.
            auto shadow = value;

            if constexpr (!nested_vmx::enabled) {
                shadow &= ~cr4_vmxe;
            } else if (auto cpu = vmcs.vpid() - 1;
                       (cpu < max_cpus) &&
                       this->guest_in_vmx_operation[cpu] &&
                       (0 == (value & cr4_vmxe))) {
                inject_general_protection_fault();
                advance_rip = false;
                break;
            }

            vmcs.cr4_read_shadow(shadow);
            vmcs.guest_cr4(value | cr4_vmxe);
            break;
        }
        case basic_reason::ept_violation: {
            // A watched page was touched. RIP stays where it is: the
            // guest's instruction has not run yet, and the whole point
            // is to let it run for itself rather than emulate it.
            if (!on_ept_violation(
                    cpuid, context, vmcs.guest_physical_address())) {
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
        case basic_reason::vmx_preemption_timer: {
            // The clock this side runs the log on. Nothing to do but
            // come back: the record-draining and flushing below is the
            // whole reason the timer was armed, and it runs for every
            // exit regardless of reason.
            //
            // The timer is not re-armed here. With "save VMX-preemption
            // timer value" clear in the exit controls, VM entry reloads
            // the counter from the VMCS field every time (SDM 26.6.4),
            // so the field written by arm_controller_poll keeps
            // producing exits at the same interval until the control is
            // turned off again.
            //
            // Nothing retired to produce this exit, so RIP stays where
            // it is. Advancing it here would have the guest skip a live
            // instruction once per tick, which is the recurring mistake
            // this file warns about.
            advance_rip = false;
            break;
        }
        case basic_reason::vmxon:
        case basic_reason::vmxoff:
        case basic_reason::vmclear:
        case basic_reason::vmptrld:
        case basic_reason::vmptrst:
        case basic_reason::vmread:
        case basic_reason::vmwrite:
        case basic_reason::vmlaunch:
        case basic_reason::vmresume:
        case basic_reason::invept:
        case basic_reason::invvpid:
        case basic_reason::vmfunc:
        case basic_reason::vmcall: {
            // Every instruction VMX added, and one exit reason each. They
            // all exit unconditionally in VMX non-root operation - SDM
            // 28.1.2 lists INVEPT, INVVPID, VMCALL, VMCLEAR, VMLAUNCH,
            // VMPTRLD, VMPTRST, VMRESUME, VMXOFF and VMXON among the
            // instructions that "cause VM exits when they are executed in
            // VMX non-root operation", and VMREAD and VMWRITE join them
            // whenever "VMCS shadowing" is 0, which it is here. So there
            // is no control that turns any of this off, and until this
            // block existed every one of them fell to `default:` and
            // halted the processor. A guest instruction must never be able
            // to stop a processor, which is what this fixes.
            //
            // The answer is an invalid opcode exception, and it is the
            // *architecturally correct* one rather than a fallback,
            // because of what the guest has already been told:
            //
            // - CPUID leaf 1 ECX[5] reports no VMX, in the cpuid case
            //   above.
            // - CR4's guest/host mask selects VMXE and the read shadow has
            //   it clear, so the guest's own view of CR4 is VMXE = 0 - see
            //   setup_vmcs and the control_register_access case.
            //
            // On a processor in that state VMXON raises #UD: its operation
            // section (SDM Vol. 3C, VMXON) begins "IF (register operand)
            // or (CR0.PE = 0) or (CR4.VMXE = 0) ... THEN #UD". And SDM
            // 28.1.1 puts that exception above the exit: "Certain
            // exceptions have priority over VM exits. These include
            // invalid-opcode exceptions". The exit only happens at all
            // because the *real* CR4 this VMM runs the guest with has VMXE
            // set, which it must - a processor in root mode cannot clear
            // it.
            //
            // The rest follow from the same fact. VMREAD, VMWRITE,
            // VMLAUNCH and VMRESUME raise #UD when "not in VMX operation",
            // and a guest that cannot execute VMXON never enters it.
            // VMFUNC raises #UD when the "enable VM functions"
            // VM-execution control is 0, which it is. VMCALL is the same
            // case as the others: outside VMX operation it is not a
            // recognised instruction, and this VMM implements no hypercall
            // interface for it to reach - consistent with the hypervisor
            // CPUID range answering zero for interface and feature leaves.
            //
            // With nesting compiled in, the instruction is answered
            // instead: the emulation sets RFLAGS to VMsucceed or to one of
            // the two failures and the guest is resumed past it, exactly
            // as it would be past any instruction that retired. A VMX
            // instruction that *fails* still retires - its failure is in
            // the flags - so this is the one place in this handler where
            // advancing past an instruction that did not do what the guest
            // asked is correct.
            if (on_vmx_instruction(full_reason, context)) {
                // Unless the instruction was a VMLAUNCH or VMRESUME that
                // took, in which case this processor is now about to enter
                // the second-level guest and RIP belongs to it. The guest
                // hypervisor's own RIP stays on its VMLAUNCH until an exit
                // is reflected and vmcs12's host RIP replaces it;
                // advancing here would write into vmcs02 instead and move
                // the second-level guest.
                if constexpr (nested_vmx::enabled) {
                    if (auto slot = vmcs.vpid();
                        (0 != slot) && (slot <= max_cpus) &&
                        this->running_l2[slot - 1]) {
                        advance_rip = false;
                    }
                }
                break;
            }

            // A fault, so RIP stays at the instruction. Advancing it would
            // be the mistake this file warns about twice over: the guest
            // would skip a live instruction *and* believe it had worked.
            //
            // The emulation may already have injected a general protection
            // fault of its own, for a privileged instruction attempted
            // from user mode or a write to a locked MSR. Injecting again
            // would overwrite it, so it only happens where nothing has
            // been injected - which the interruption information field
            // says, since it is cleared on every exit that did not inject.
            constexpr std::uint64_t injection_valid = 1ull << 31;
            if (0 ==
                (vmcs.read(arch::x86_64::vmx::vmcs::field::
                               vm_entry_interruption_information_field) &
                 injection_valid)) {
                inject_invalid_opcode_exception();
            }

            advance_rip = false;

            // Said once per processor, with a count kept for the rest.
            // Whether a guest hypervisor tried once and stood down or is
            // retrying for ever is the whole question when VBS is on, and
            // one line plus a counter answers it without an idle-loop's
            // worth of noise. The count is readable from the exit ring's
            // neighbourhood in a debugger; the line survives a restart.
            if (auto cpu = vmcs.vpid() - 1; cpu < max_cpus) {
                this->vmx_instructions_refused[cpu] =
                    this->vmx_instructions_refused[cpu] + 1;

                if (!this->vmx_instruction_logged[cpu]) {
                    this->vmx_instruction_logged[cpu] = true;
                    log("cpu {} vmx instruction, exit reason {}, rip {} "
                        "cs {}: faulted",
                        cpu,
                        static_cast<std::uint64_t>(full_reason.value()),
                        vmcs.guest_rip(),
                        vmcs.guest_cs_selector());
                }
            }
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

        resume_guest(context, full_reason, advance_rip);
    });

    return {};
}

void hypervisor::resume_guest(arch::x86_64::context & context,
                              arch::x86_64::vmx::exit_reason full_reason,
                              bool advance_rip)
{
    auto & vmcs = this->vmcs;

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

    // Keep the timer running while the channel is live, because
    // otherwise nothing happens at all.
    //
    // Measured, and it is the finding that decides how a continuous
    // log has to work: a steadily running Windows takes about
    // fifteen hundred exits on its busiest processor and a hundred
    // and sixty on the others - not millions. This VMM intercepts
    // very little, which is the point of it, and the consequence is
    // that the write path is reached almost never. A log driven by
    // guest exits is a log that stops the moment the guest settles.
    //
    // The preemption timer manufactures the exits instead, at an
    // interval this side chooses. That is a real cost - an exit the
    // guest would not otherwise have taken - so it is only armed
    // while there is a channel to feed.
    if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
        arm_controller_poll(diag::esp_block_sink::ready());
    }

    // A heartbeat, so the channel has something to carry.
    //
    // Without it the log is silent whenever nothing goes wrong, which
    // is most of the time - and a silent channel is
    // indistinguishable from a broken one to whoever is reading the
    // disk from another machine. That distinction is the whole point
    // of the channel, so it emits a line periodically whether or not
    // anything happened, and the line carries the two things worth
    // knowing about a guest that is merely alive: which processor
    // this is and how many exits it has taken.
    //
    // Counted rather than timed, because a count is free and reading
    // the time stamp counter on every exit is not. The interval is
    // large enough that the cost is nothing and small enough that a
    // reader sees movement within a second on any busy guest.
    if constexpr (diag::enabled) {
        // A proof of life, deliberately slow.
        //
        // This record exists only so an idle channel can be told
        // apart from a dead one, and it is the one record this side
        // manufactures rather than observes. That makes its rate a
        // direct tax on the region: the sink flushes a partly filled
        // block once staged_deadline_ticks passes, so a heartbeat
        // faster than a block fills turns every 128-byte record into
        // a 4096-byte write.
        //
        // Measured at one per eight ticks: 152 blocks a second, one
        // record in each, wrapping the 64 MB region every seven
        // minutes and writing 620 KB/s to the medium for nothing.
        // At one per thousand ticks it is 4 KB/s and the region holds
        // about four and a half hours.
        //
        // The freshness the deadline buys is not lost by slowing this
        // down, because it applies to real records too: anything the
        // guest actually causes still reaches the medium within
        // staged_deadline_ticks of being written. Only the synthetic
        // traffic is throttled, and an idle guest now writes nothing
        // at all - which is the correct behaviour, not a regression.
        constexpr std::uint64_t heartbeat_exits = 1000;
        auto cpu = vmcs.vpid();
        if ((0 != cpu) && (cpu <= max_cpus)) {
            auto & seen = this->heartbeat_exits_seen[cpu - 1];
            if (0 == (++seen % heartbeat_exits)) {
                diag::log<diag::severity::trace>(
                    "cpu {} alive, {} exits", cpu - 1, seen);
            }
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

    // Counted here, at the last point before control leaves this
    // handler, so a frozen exit count can be read two ways round.
    if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus)) {
        this->resumes_reached[slot - 1] =
            this->resumes_reached[slot - 1] + 1;
        this->resume_activity_state[slot - 1] =
            vmcs.guest_activity_state();
        this->resume_guest_rip[slot - 1] = vmcs.guest_rip();
        this->resume_guest_cs[slot - 1] = vmcs.guest_cs_selector();
    }

    // Whether this processor has been out of VMX operation and back
    // since the last entry, which only the sleep quiesce does. Its
    // return leaves the launch state clear, and VMRESUME requires
    // launched (SDM 27.1) - so that one case has to leave through
    // VMLAUNCH instead. Consumed here, so the next exit resumes.
    auto relaunch = false;
    if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus)) {
        relaunch = this->relaunch_after_sleep[slot - 1];
        this->relaunch_after_sleep[slot - 1] = false;
    }

    // Which entry this processor leaves through, which is three
    // questions rather than one.
    //
    // A processor running a second-level guest goes back to it through
    // the nested pair, whose failure path recovers instead of halting -
    // a guest hypervisor's VMLAUNCH is a guest instruction and no guest
    // instruction may stop a processor. Which of the two depends on
    // vmcs02's own launch state, which SDM 29 step 5 makes "launched"
    // only after an entry has passed every check.
    //
    // Otherwise it is the ordinary pair, and VMLAUNCH only where this
    // processor has been out of VMX operation and back since its last
    // entry - see relaunch above.
    auto entry = relaunch ? arch::x86_64::vmx::vmlaunch
                          : arch::x86_64::vmx::vmresume;

    if constexpr (nested_vmx::enabled) {
        if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus) &&
                                     this->running_l2[slot - 1]) {
            entry = this->vmcs02_launched[slot - 1]
                        ? arch::x86_64::vmx::nested_vmresume
                        : arch::x86_64::vmx::nested_vmlaunch;
        }
    }

    // The mirror of the launch: the guest's registers are put back
    // and the last thing executed in host mode is the resume itself.
    context.rip = reinterpret_cast<std::uint64_t>(entry);
    arch::x86_64::restore_context(&context);
    std::unreachable();
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

/**
 * Where the boot processor arrives when the platform resumes from S3
 * through a waking vector this VMM took over, declared by
 * zpp/arch/x86_64/ap_start_up.h and reached from the same trampoline.
 *
 * A fourth caller of instance(), and the note on it in CLAUDE.md is the
 * thing to check: construction must already have returned. It has, and by
 * a wider margin than either of the other two - this is reachable only
 * after a suspend, which is after a guest has been running, which is after
 * the boot processor left main.
 */
extern "C" void zpp_resume_from_sleep_main(std::uint64_t processor)
{
    zpp::hypervisor::hypervisor::instance()
        .resume_from_sleep_on_this_processor(processor);
}
