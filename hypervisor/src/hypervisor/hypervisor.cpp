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

/**
 * A loadable segment's permissions in the page table's spelling.
 *
 * A translation and deliberately not a cast: both are three flags in one
 * integer and the two orders disagree - the ELF form is execute 1, write
 * 2, read 4 (elf_file::memory_protection, from p_flags), the page table's
 * is read 1, write 2, execute 4. Only write happens to line up, so a cast
 * would silently swap read for execute and produce a writable, executable
 * .text with a read-only page table beside it.
 */
constexpr arch::x86_64::page_table::protection
host_protection(elf_file::memory_protection protection)
{
    // Read is unconditional. A page that is mapped at all is readable on
    // this architecture - there is no bit for it - and every loadable
    // segment carries PF_R in practice.
    auto result = arch::x86_64::page_table::protection::read;

    if (protection & elf_file::memory_protection::write) {
        result = result | arch::x86_64::page_table::protection::write;
    }

    if (protection & elf_file::memory_protection::execute) {
        result = result | arch::x86_64::page_table::protection::execute;
    }

    return result;
}
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

    // Map the module, readable and nothing else.
    //
    // This is the floor, not the answer. What each part of the image may
    // do is decided immediately below from its own program headers; what
    // is left at this level is the padding between segments, which holds
    // no code and no data and therefore needs neither writing nor
    // executing.
    //
    // It used to be read, write and execute over the whole range, on the
    // grounds that this VMM executes from its image and writes to its own
    // data and both live in it. Both halves are true and neither needs
    // the whole range: text is executed and never written, data is
    // written and never executed. A range that is both is a single stray
    // store away from being the thing that rewrites this VMM's own code,
    // and the image already says which is which.
    this->host_page_table.map_from(
        this->module_base,
        this->module_size,
        arch::x86_64::page_table::protection::read,
        this->os_page_table);

    // What each loadable segment asks for, on top of that floor.
    //
    // The ELF the loader placed here is the only authority on its own
    // layout, and it is still readable: this is the same image
    // initialize_module_region measured, addressed through the OS page
    // table this processor is still running on.
    //
    // Permissions are added rather than assigned, and each segment is
    // rounded outward to whole pages - see page_table::add_protection for
    // why that direction and not the other. The consequence to know is
    // that a page two segments share ends up with the union of what they
    // asked for; today nothing here shares one, which the program headers
    // show and which the log line below records per boot.
    elf_file(this->module_base, elf_file::state::loaded)
        .protect([this](const void * address,
                        std::size_t size,
                        elf_file::memory_protection protection) {
            log("module segment {} size {} protection {}",
                reinterpret_cast<std::uint64_t>(address),
                size,
                static_cast<std::uint64_t>(protection));

            this->host_page_table.add_protection(
                address, size, host_protection(protection));
        });

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

    // Which page that was, so `watch_local_apic` can refuse to arm on any
    // other. It is the only local APIC page this table maps, and the
    // watch's filter reaches it by dereferencing a host virtual address.
    this->mapped_apic_page = apic_base;

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

void hypervisor::initialize_intermediate_gdt(std::size_t cpu)
{
    // Out of unprotected_memory, because protect_module makes the rest
    // of this module not-present to the guest and the guest goes on
    // running with these as its own descriptor tables.
    // By slot, like everything else per processor. See setup_vmcs on why
    // the shared counter is not an identity.
    auto & intermediate_gdt =
        this->unprotected_memory.intermediate_gdt[cpu];

    auto & guest_tss = this->unprotected_memory.guest_tss[cpu];

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
        this->unprotected_memory.intermediate_gdt[cpu];

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

void hypervisor::load_intermediate_gdt(std::size_t cpu)
{
    arch::x86_64::gdt_layout lgdt_layout{};
    lgdt_layout.base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory.intermediate_gdt[cpu]);
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
    // What a phase boundary costs, measured rather than assumed.
    //
    // **`mark_phase`'s comment calls it "a handful of cycles against the
    // ~390,000 an exit costs here", and that was never measured.** It
    // matters because every per-exit figure in `BACKLOG.md` comes from
    // the phase tree, and the tree takes forty-odd boundaries an exit -
    // so if a boundary is thousands of cycles rather than tens, the
    // profiler is a large fraction of what it profiles and the
    // decomposition built on it describes an instrumented build.
    //
    // The suspicion is concrete: gating the diagnostic inside the
    // `resume: entry census` span left that span at 6,089 cycles a call
    // against 6,104, so its cost is not its contents.
    //
    // Measured here, once, on the boot processor before any guest runs:
    // a thousand calls inside one RDTSC pair. Slot 0 absorbs the
    // thousand marks and is the only distortion - against the 1.68
    // million calls a real boot puts through it, that is noise.
    {
        constexpr std::uint64_t rounds = 1000;
        auto before = arch::x86_64::rdtsc();
        for (std::uint64_t i{}; i < rounds; ++i) {
            mark_phase(0, 0);
        }
        auto after = arch::x86_64::rdtsc();

        log("phase boundary costs {} cycles, over {} calls - compare the "
            "phase tree's per-slot figures, which carry one of these each",
            (after - before) / rounds,
            rounds);
    }

    log("mtrr cap {}, def type {}, enabled {}, fixed in use {}, "
        "default {}, variable {}",
        mtrrs.capabilities.value(),
        mtrrs.default_type.value(),
        mtrrs.default_type.enabled(),
        mtrrs.fixed_ranges_in_use(),
        mtrrs.default_type.type(),
        mtrrs.variable_count);

    // And each variable range in full, because the summary above says how
    // many there are and nothing about what they cover.
    //
    // What that costs: eight lines once, at start-up, in a ring of four
    // thousand. What its absence cost: the coverage of these ranges had to
    // be recovered by reading EPT entries back out of physical memory by
    // hand, and the memory type in an EPT entry is the *derived* answer -
    // so a wrong range and a wrong derivation from a right range are
    // indistinguishable from it. These are the input, and the input is the
    // half that was never readable.
    //
    // The valid bit is the interesting one and is the reason `size` alone
    // will not do. SDM 14.11.2.3, "Variable Range MTRRs", gives bit 11 of
    // the mask register as the "V (valid) flag - Enables the register pair
    // when set; disables register pair when clear", so an invalid pair
    // still has a base and a mask in it,
    // decodes to a plausible range, and covers nothing at all. Firmware
    // leaves the unused pairs behind rather than zeroing them, so a run
    // with three ranges in use out of ten looks like ten unless this says
    // otherwise.
    for (std::size_t i{}; i < mtrrs.variable_count; ++i) {
        const auto & range = mtrrs.variable[i];

        log("mtrr {} base {} size {} type {} valid {}",
            i,
            range.physical_base,
            range.size,
            range.type,
            range.valid);
    }
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
    //
    // All-context, not single-context, and the descriptor above is
    // therefore ignored - SDM 31.4.3.1 says an all-context invalidation
    // takes no EPTP from the operand.
    //
    // Single-context was wrong for most of the callers. The descriptor it
    // builds names `epml4_physical`, which is *this VMM's own* extended
    // page table root, but two of the callers change a **shadow** table
    // whose root is a different address entirely - the one recycled in
    // shadow_ept_pointer_for, and the leaf installed on a second-level
    // guest's fault. Naming one context and changing another invalidates
    // nothing that moved and leaves the stale translation cached, which
    // is precisely the failure this cannot afford: a guest hypervisor
    // whose flush appears to succeed and does not take effect retries it,
    // and the boot measured on the rig ends in an escalating storm of
    // flush hypercalls and inter-processor interrupts.
    //
    // INVVPID cannot cover for it. SDM 31.4.3.2: "The INVVPID instruction
    // is not required to invalidate any guest-physical mappings", and
    // KVM says the same where it relies on it - nested.c:1209-1213, "EPT
    // is a special snowflake, as guest-physical mappings aren't flushed
    // on VPID invalidations".
    //
    // The header's own description of this design already assumed a
    // global invalidation; there simply was not one. All-context is what
    // it claimed and costs nothing here, because every context this
    // processor holds belongs to this VMM.
    constexpr std::uint64_t all_context = 2;

    if (0 != arch::x86_64::vmx::invept(all_context, &operand)) {
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

void hypervisor::arm_controller_poll(std::size_t cpu, bool armed)
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
    //
    // From the caller rather than from `vmcs.vpid()`, which this used to
    // read. `resume_guest` calls this once on every exit and already has
    // the index; the VPID it would have read is `cpu + 1` for the life of
    // the processor, and reading it is a VMREAD - an exit to the layer
    // below at 1.4-1.8 microseconds on a host with no VMCS shadowing.
    if (cpu >= max_cpus) {
        return;
    }

    // And only against this VMM's own VMCS, because that is the only one
    // the control means anything in.
    //
    // Called unconditionally from resume_guest, which runs with vmcs02
    // current whenever a second-level guest is about to be resumed - so
    // without this it read and wrote *vmcs02's* pin-based controls.
    // build_vmcs02 strips the preemption timer from vmcs02 on every entry
    // (it is this VMM's alone and the capability MSRs do not offer it to
    // a guest hypervisor), so the arm was discarded at the next entry
    // while `controller_poll_armed` recorded it as done - and the early
    // return above then never re-armed it on vmcs01. The processor's own
    // millisecond poll died there and stayed dead.
    //
    // Skipped rather than redirected: vmcs01's control is untouched while
    // a second-level guest runs, so `controller_poll_armed` still
    // describes it truthfully and the next exit that returns to the guest
    // hypervisor re-evaluates.
    if constexpr (nested_vmx::enabled) {
        if (this->running_l2[cpu]) {
            return;
        }
    }

    auto & armed_here = this->controller_poll_armed[cpu];

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
        if (!this->timer_refusal_reported[cpu]) {
            this->timer_refusal_reported[cpu] = true;
            diag::log<diag::severity::warning>(
                "preemption timer not permitted, allowed-1 {}",
                allowed_one);
            log("preemption timer falling back to the guest's timer");
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

        // No adjust_msr here because the capability checked above already
        // made it: a processor that does not offer the timer took the
        // guest-timer fallback and returned before reaching this.
        this->vmcs.pin_based_vm_execution_controls(
            controls | arch::x86_64::vmx::vm_execution_controls::pin::
                           activate_preemption_timer);
    } else {
        // Same capability checked above, and clearing could not violate
        // it either way: SDM A.3.1 does not reserve pin bit 6 to 1.
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
    this->intercept_msr(
        arch::x86_64::msr::ia32_tsc_deadline, false, armed);
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
        auto slot = first_page + i;
        auto at = mapping_window + (slot * page_size);
        auto frame = page + (i * page_size);

        // The leaf entry, found once per window page and written
        // directly thereafter. See `window_entry`: everything
        // `map_page` does above the leaf recomputes a constant, and
        // that recomputation was 14.0% of this VMM's whole time.
        //
        // The first mapping of a page still goes the long way, because
        // that is what establishes the levels above it - and it is what
        // makes the entry this then caches the one the processor will
        // consult.
        if (auto entry = this->window_entry[slot]) {
            entry->page_number(frame >> 12);
            this->window_entry_fast = this->window_entry_fast + 1;
        } else {
            this->host_page_table.map_page(
                at,
                frame,
                arch::x86_64::page_table::protection::read |
                    arch::x86_64::page_table::protection::write);

            // Whichever entry terminates the walk - which for a page
            // this VMM has just mapped with `map_page` is the four
            // kilobyte leaf, since `map_page_from` points the directory
            // entry at a page table rather than leaving a large page.
            // Checked rather than assumed: caching a large-page entry
            // here would repoint a whole two megabytes on every call.
            auto & leaf = this->host_page_table.page_table_entry(at);

            if (!leaf.large()) {
                this->window_entry[slot] = &leaf;
            }

            this->window_entry_slow = this->window_entry_slow + 1;
        }

        // The processor has a translation cached for this address from
        // whoever used the window last, pointing at their page. Without
        // this a read through it answers with their bytes, which is the
        // entire failure mode a shared window has.
        //
        // **Not elidable by remembering what this slot last held.** That
        // was built and measured: it fired on 11.5% of 11.4 million
        // calls and moved this phase's cost by nothing, because a
        // four-level walk asks one slot for four *different* table
        // frames in a row, so the immediately preceding use of a slot is
        // essentially never the page the next one wants. See BACKLOG.
        // That measurement still stands, and it is now also the reason
        // the *walk* had to go instead: the hit rate cannot be improved,
        // so the miss had to be made cheap.
        arch::x86_64::invlpg(reinterpret_cast<const void *>(at));
    }

    return reinterpret_cast<std::uint8_t *>(mapping_window +
                                            (first_page * page_size)) +
           (physical_address - page);
}

std::uint64_t hypervisor::borrow_guest_admin_queue(
    const nvme::submission_entry * payload,
    std::uint32_t payload_count,
    std::uint16_t * payload_status,
    std::uint32_t * payload_result)
{
    // The whole body behind `if constexpr`, because naming a static
    // member of the sink's class template odr-uses it and would carry the
    // channel into a build that switched it off - which
    // scripts/ci/check-diag-absent.sh fails, and did over this function.
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset) {
        static_cast<void>(payload);
        static_cast<void>(payload_count);
        static_cast<void>(payload_status);
        static_cast<void>(payload_result);
        return 0xff;
    } else {
        // Nothing to borrow against if the channel was never up.
        if (!this->channel_bar) {
            return 0xfb;
        }

        // The doorbell page has to be watched in holding mode already.
        // This does not arm it: arming is what the enable does, and it
        // stays armed until the queue pair exists, because the same watch
        // is what records the guest's Create commands. Arming it here
        // instead would leave the window between the enable and the first
        // borrow unobserved.
        if (!this->channel_doorbell_watched) {
            return 0xf5;
        }

        // One borrow at a time, across every processor.
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
        // serialised is not harmless: the second borrow runs after the
        // guest's driver is live. That is precisely the unexcluded borrow
        // that was once measured spending its whole budget and timing
        // out, with the guest's admin queue desynchronised behind it.
        //
        // An exchange rather than a test and a set, because the two
        // observers are what create the race in the first place.
        if (this->channel_rebuild_running.exchange(
                true, std::memory_order_acquire)) {
            this->channel_rebuild_reentered =
                this->channel_rebuild_reentered + 1;
            return 0xfa;
        }

        scope_exit release_rebuild{[this] {
            this->channel_rebuild_running.store(false,
                                                std::memory_order_release);
        }};

        auto * bar = this->channel_bar;

        // Wait for the controller. At the enable it is the one the guest
        // has just started, and the guest is about to spend that same
        // wait polling this register itself; later on it is already up
        // and this reads once.
        auto budget = std::uint64_t{1} << 24;
        for (;;) {
            auto status = nvme::controller_status{arch::x86_64::read32(
                static_cast<volatile std::uint8_t *>(bar) +
                nvme::offset_of(nvme::register_offset::status))};
            if (status.fatal_status()) {
                return 0xf0;
            }
            if (status.ready()) {
                break;
            }
            if (0 == budget--) {
                return 0xf1;
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
            return 0xf2;
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
            return 0xf3;
        }

        where.submission = submission;
        where.completion = completion;

        this->rebuild_submission_depth = where.submission_depth;
        this->rebuild_completion_depth = where.completion_depth;
        this->rebuild_submission_base = submission_base;
        this->rebuild_completion_base = completion_base;
        this->rebuild_stride = this->channel_doorbell_stride;

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
        // Translated, not cast: these take guest physical addresses and
        // `bar` is a host virtual pointer. Casting is right only while
        // that mapping is the identity, which is not something this VMM
        // establishes or checks.
        auto doorbell_page =
            this->host_page_table.virtual_to_physical(
                const_cast<const void *>(bar)) +
            nvme::offset_of(nvme::register_offset::doorbell_base);

        // Acknowledged first, held second, and the order is not
        // cosmetic.
        //
        // A processor still holding a translation cached before the page
        // was armed would write straight through the protection, so the
        // borrow cannot begin until every running processor has said it
        // has picked the change up. Not getting that answer means not
        // borrowing - an unexcluded borrow desynchronises the guest's
        // admin queue, and no channel is better than that.
        //
        // The other order deadlocks. A processor held at a faulting
        // instruction takes no further exits, so if it stamped its high
        // water mark before the arming moved the generation it can never
        // stamp again and the wait for it never ends. Measured exactly
        // that way, twice: acknowledgement refused on every attempt.
        //
        // **Probing.** The wake NMI is sent, which this caller refused to
        // do for as long as taking one in root mode halted the processor
        // that took it - that is what took the development machine off
        // the network, and it is fixed: on_host_exception returns for
        // vector 2 and the entry stub saves, restores and irets. Probing
        // is not optional here, because a processor the guest has halted
        // executes nothing, reaches no exit path and never stamps -
        // measured, one processor three generations behind and staying
        // there. Waiting passively for it waits for ever, which is why
        // the passive form of this wait timed out rather than succeeded.
        if (!wait_for_ept_acknowledgement(std::uint64_t{1} << 24, true)) {
            return 0xf7;
        }

        if (!hold_guest_page(doorbell_page)) {
            return 0xf6;
        }

        scope_exit unhold{[&] { release_guest_page(doorbell_page); }};

        // Only now is the queue anyone's to read, and only now is it
        // worth waiting for it to fall still.
        //
        // What has to be true is narrower than "nothing is happening",
        // and it is worth being exact because the wide version cannot be
        // established at all. A completion arriving mid borrow is
        // handled - it is recognised as the guest's by its command
        // identifier, held, and replayed into the slot it would have
        // occupied. What is *not* handled is a submission the controller
        // has not fetched yet: the borrow writes its own commands over
        // the queue from the submission tail on, so an entry still
        // waiting there is destroyed.
        //
        // SQHD answers exactly that question. Every completion carries
        // the controller's submission head at the time it was posted, so
        // SQHD equal to the tail the guest last published means the
        // controller has taken everything the guest wrote. The caller
        // supplies that tail, because it is the value the guest's own
        // doorbell write carried and cannot be read back from the device
        // - PCIe Transport 1.0c 3.1.2.1, a doorbell read returns a vendor
        // specific value.
        //
        // Then a stillness check on top, which is cheap and catches the
        // case SQHD cannot: a completion posted between the locate and
        // the first submission.
        constexpr std::uint32_t stable_rounds = 64;
        auto settle = std::uint64_t{1} << 22;
        std::uint32_t stable{};

        nvme::admin_borrow::locate(where);
        auto last_completion_tail = where.completion_tail;
        auto last_completion_phase = where.completion_phase;
        auto last_submission_tail = where.submission_tail;

        while (stable < stable_rounds) {
            if (0 == settle--) {
                this->channel_quiesce_submission_tail =
                    where.submission_tail;
                this->channel_quiesce_expected_tail =
                    this->channel_expected_admin_tail;
                return 0xf8;
            }

            nvme::admin_borrow::locate(where);

            if ((where.completion_tail == last_completion_tail) &&
                (where.completion_phase == last_completion_phase) &&
                (where.submission_tail == last_submission_tail) &&
                (where.submission_tail ==
                 this->channel_expected_admin_tail)) {
                ++stable;
                continue;
            }

            last_completion_tail = where.completion_tail;
            last_completion_phase = where.completion_phase;
            last_submission_tail = where.submission_tail;
            stable = 0;
        }

        nvme::admin_borrow::snapshot saved{
            this->channel_snapshot_submission,
            this->channel_snapshot_completion};

        auto started = arch::x86_64::rdtsc();

        auto result =
            nvme::admin_borrow::run(bar,
                                    this->channel_doorbell_stride,
                                    where,
                                    saved,
                                    payload,
                                    payload_count,
                                    payload_status,
                                    payload_result,
                                    std::uint64_t{1} << 24);

        this->channel_rebuild_result = static_cast<std::uint64_t>(result);
        this->rebuild_issued = nvme::admin_borrow::last_issued;
        this->rebuild_reaped = nvme::admin_borrow::last_reaped;
        this->rebuild_total = nvme::admin_borrow::last_total;
        this->channel_rebuild_ticks = arch::x86_64::rdtsc() - started;

        if (nvme::borrow_result::ok != result) {
            // A borrow that stops halfway has left the queue
            // desynchronised and cannot be retried into it. Not
            // recoverable from here; recorded so the medium says which
            // step it was.
            return 0xd0 | static_cast<std::uint64_t>(result);
        }

        return 0;
    }
}

void hypervisor::reserve_channel_queue_allocation()
{
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset) {
        return;
    } else {
        if (!this->channel_bar) {
            return;
        }

        // Once per epoch. A controller level reset clears the allocation
        // and this is what re-establishes it; asking twice inside one
        // epoch would be refused anyway, since 5.2.30.1.5 freezes the
        // allocation at the first Set Features completed after the reset.
        if (0 != this->channel_reserve_result) {
            return;
        }

        ++this->channel_rebuilds;

        // Watch the doorbell page before borrowing, not after.
        //
        // It is two things at once and both are needed from this instant:
        // the exclusion the borrow rests on, and the only way to see what
        // the guest's driver submits. The guest's own Set Features and its
        // Create commands follow within microseconds of this exit, and a
        // watch armed after the borrow would miss whichever of them
        // arrived first.
        //
        // Holding mode rather than notify, and it costs nothing to leave
        // it there: a watch in holding mode with nobody holding behaves
        // exactly as a notifying one.
        //
        // Translated, not cast, for the same reason as everywhere else
        // this address is formed.
        auto doorbell_page =
            this->host_page_table.virtual_to_physical(
                const_cast<const void *>(this->channel_bar)) +
            nvme::offset_of(nvme::register_offset::doorbell_base);

        if (auto armed =
                watch_guest_page_writes(doorbell_page,
                                        &hypervisor::on_doorbell_write,
                                        this,
                                        page_watch::mode::hold);
            !armed) {
            this->channel_reserve_result = 0xf5;
            return;
        }

        this->channel_doorbell_watched = true;

        // Nothing of the guest's has been submitted yet: the controller
        // has only just been enabled, and the processor that would submit
        // the first command is this one, held inside this exit.
        this->channel_expected_admin_tail = 0;
        this->admin_observed_head = 0;

        // One command, and it creates nothing.
        //
        // That is the whole correction. NVMe Base 5.2.30.1.5 constrains
        // Set Features and nothing else - a Create I/O queue may be
        // issued at any point after an allocation exists - so the
        // sequence that breaks the guest is reserving and creating in one
        // borrow and letting the guest's own Set Features arrive after
        // our queues exist. It is aborted with Command Sequence Error,
        // and Linux turns that into zero I/O queues and no block device
        // (`nvme_set_queue_count` sets *count = 0 on any error status).
        nvme::submission_entry payload[1]{
            nvme::set_features_maximum_number_of_queues()};

        std::uint16_t status[1]{0xffff};
        std::uint32_t granted[1]{};

        auto refusal =
            borrow_guest_admin_queue(payload, 1, status, granted);

        // Another processor got here first and is inside the borrow now.
        //
        // Recorded as nothing rather than as a refusal, and that is the
        // point: the one that did claim it is about to write the real
        // answer, and a loser writing 0xfa afterwards would replace a
        // successful reservation with a failure that did not happen. Two
        // processors reach here for one transition because the enable is
        // observed twice - from the emulated write and from the poll.
        if (0xfa == refusal) {
            return;
        }

        // Whatever happens from here, the doorbell page comes off again
        // unless something is still going to need it.
        //
        // Only a reservation that took, with the creation switched on,
        // does: that is the one case where a later Create of the guest's
        // still has to be seen. A reservation that was refused will never
        // create anything, and the control build creates nothing by
        // definition - and leaving the watch armed for either would trap
        // the guest's entire disk traffic for the rest of the boot, which
        // is a cost the control run must not be paying while it is
        // supposed to be measuring the borrow.
        //
        // Declared after the reentry check above so it cannot fire on
        // that path: another processor is inside the borrow there and is
        // relying on the hold.
        auto keep_watching = false;
        scope_exit stop_watching{[&] {
            if (!keep_watching) {
                stop_watching_channel_doorbells();
            }
        }};

        this->channel_reserve_status = status[0];
        this->channel_reserve_allocation = granted[0];

        if (0 != refusal) {
            this->channel_reserve_result = refusal;
            return;
        }

        if (0 != status[0]) {
            this->channel_reserve_result = 0xe0;
            return;
        }

        this->channel_allocated_submission_queues =
            nvme::number_of_submission_queues(granted[0]);
        this->channel_allocated_completion_queues =
            nvme::number_of_completion_queues(granted[0]);

        this->channel_reserve_result = 1;

        // Two things still need the watch: the guest's own Set Features,
        // whose answer is edited from its doorbell, and its Create
        // commands. Either switch on its own is a reason to keep it, and
        // the reduction takes the watch off again itself once the answer
        // has been edited when the creation is not going to want it.
        if constexpr (diag::create_channel_queue_after_guest ||
                      diag::reduce_guest_queue_grant) {
            keep_watching = true;
        }

        diag::log<diag::severity::info>(
            "reserved {} submission and {} completion queues",
            this->channel_allocated_submission_queues,
            this->channel_allocated_completion_queues);
    }
}

void hypervisor::create_channel_queue()
{
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset ||
                  !diag::create_channel_queue_after_guest) {
        return;
    } else {
        // The identifiers, chosen from what the guest did rather than
        // from what it was told.
        //
        // One above the higher of two observations of the guest, in each
        // space separately: the highest identifier it has actually
        // created, and the number it asked the controller for. The second
        // is not "what it was told" - it is the guest's own request, read
        // off its own submission queue - and it is the margin that
        // survives a driver which creates its whole request in an order
        // this has not seen, or creates the rest of it after this fires.
        // Measured on the rig the two agree: it asks for sixteen and
        // eight and creates exactly one to sixteen and one to eight.
        //
        // The request is clamped by what the guest was *told* before it
        // is used as a margin, and without that clamp the reduction below
        // buys nothing: with diag::reduce_guest_queue_grant on the guest
        // asks for sixteen submission queues, is told fifteen, creates
        // fifteen - and a margin of sixteen would still choose
        // seventeen, which is outside the allocation and refused with
        // Invalid Queue Identifier. See queue_limit; with that switch off
        // the granted count is zero and this is the request, unchanged.
        auto highest = [](std::uint64_t created, std::uint64_t requested) {
            return (created > requested) ? created : requested;
        };

        auto submission_id =
            highest(this->channel_guest_highest_submission_queue,
                    queue_limit(
                        this->channel_guest_requested_submission_queues,
                        this->channel_guest_granted_submission_queues)) +
            1;
        auto completion_id =
            highest(this->channel_guest_highest_completion_queue,
                    queue_limit(
                        this->channel_guest_requested_completion_queues,
                        this->channel_guest_granted_completion_queues)) +
            1;

        // Inside the allocation, or nothing is created at all.
        //
        // This is the case that was measured as status 0x4101 - DNR,
        // command specific, Invalid Queue Identifier - and the guest
        // booted precisely because nothing had been created behind its
        // back. Refusing here reaches the same outcome without spending a
        // borrow on it, and says so in a counter rather than in a status
        // nobody can see.
        if ((submission_id > this->channel_allocated_submission_queues) ||
            (completion_id > this->channel_allocated_completion_queues) ||
            (submission_id > 0xffffu) || (completion_id > 0xffffu)) {
            this->channel_create_result = 0xe2;
            stop_watching_channel_doorbells();
            return;
        }

        // Both doorbells have to land inside the two pages of the
        // controller's registers this VMM can reach. With a stride of
        // zero and identifiers in the tens they are a few hundred bytes
        // into the doorbell page, but the stride is the controller's to
        // choose and the identifier is now the guest's, so the product is
        // checked rather than assumed.
        auto stride = this->channel_doorbell_stride;
        auto submission_doorbell = nvme::submission_queue_doorbell_offset(
            static_cast<std::uint32_t>(submission_id), stride);
        auto completion_doorbell = nvme::completion_queue_doorbell_offset(
            static_cast<std::uint32_t>(completion_id), stride);

        constexpr std::uint32_t reachable = 2 * page_size;
        if ((submission_doorbell + sizeof(std::uint32_t) > reachable) ||
            (completion_doorbell + sizeof(std::uint32_t) > reachable)) {
            this->channel_create_result = 0xe3;
            stop_watching_channel_doorbells();
            return;
        }

        auto started = arch::x86_64::rdtsc();

        // Ours to create: the same storage the loader used, because the
        // storage outlives every reset - it is reserved memory - and only
        // the controller's idea of the queues was lost. The identifiers
        // are not the loader's: it hardcodes four, which is inside both
        // of the ranges this guest uses.
        //
        // Interrupts deliberately disabled on the completion queue. The
        // guest never has to see anything of ours, and a queue that
        // signals nothing cannot be the thing that makes it.
        nvme::submission_entry payload[2]{};
        payload[0] = nvme::create_io_completion_queue(
            static_cast<std::uint16_t>(completion_id),
            diag::esp_block_sink::queue_entries,
            this->channel_completion_physical,
            false,
            0);
        payload[1] = nvme::create_io_submission_queue(
            static_cast<std::uint16_t>(submission_id),
            diag::esp_block_sink::queue_entries,
            this->channel_submission_physical,
            static_cast<std::uint16_t>(completion_id),
            nvme::queue_priority::medium);

        std::uint16_t status[2]{0xffff, 0xffff};

        auto refusal =
            borrow_guest_admin_queue(payload, 2, status, nullptr);

        // Somebody else is inside a borrow. Left untried rather than
        // recorded as refused, so that the next doorbell ring tries
        // again - the trigger is a condition rather than an edge, so it
        // is still true when the guest submits its next command.
        if (0xfa == refusal) {
            return;
        }

        this->channel_create_ticks = arch::x86_64::rdtsc() - started;
        this->channel_create_status =
            (std::uint64_t{status[1]} << 16) | status[0];

        if (0 != refusal) {
            this->channel_create_result = refusal;
            stop_watching_channel_doorbells();
            return;
        }

        if ((0 != status[0]) || (0 != status[1])) {
            this->channel_create_result = 0xe1;
            stop_watching_channel_doorbells();
            return;
        }

        this->channel_created_submission_id =
            static_cast<std::uint16_t>(submission_id);
        this->channel_created_completion_id =
            static_cast<std::uint16_t>(completion_id);

        // The queues exist again, empty, so the channel is told where they
        // are and that they start from nothing.
        diag::esp_block_sink::adopt_rebuilt_queue(
            static_cast<volatile std::uint8_t *>(this->channel_bar),
            stride,
            this->channel_created_submission_id,
            this->channel_created_completion_id,
            this->channel_namespace);

        this->channel_create_result = 1;

        diag::log<diag::severity::info>(
            "channel queue pair created, submission {} completion {}",
            this->channel_created_submission_id,
            this->channel_created_completion_id);

        // Nothing left to watch for. The doorbell page carries every I/O
        // queue's doorbell as well as the admin one whenever the stride
        // is zero, which it is on real hardware, so leaving it armed is an
        // exit per disk command for the rest of the boot.
        stop_watching_channel_doorbells();
    }
}

void hypervisor::patch_guest_queue_grant()
{
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset ||
                  !diag::reduce_guest_queue_grant) {
        return;
    } else {
        // Once per epoch. The guest issues this feature once, during
        // initialisation - 5.2.30.1.5 says it may only be issued before
        // any I/O queue exists - and a second answer would arrive after
        // ours had already decided what the guest believes.
        if (0 != this->channel_grant_result) {
            return;
        }

        if (!this->channel_bar) {
            this->channel_grant_result = 0xfb;
            return;
        }

        // No reservation, no lie worth telling. Without our own Set
        // Features having gone first the controller's allocation is
        // whatever the guest asked for, so reducing the answer would take
        // a queue away from the guest and leave nothing valid above it -
        // the identifier we would then choose is outside the allocation
        // and Create returns Invalid Queue Identifier, which is measured
        // and recorded in NVME-LOG.md as status 0x4101.
        if (1 != this->channel_reserve_result) {
            this->channel_grant_result = 0xf9;
            return;
        }

        auto started = arch::x86_64::rdtsc();

        auto * bar =
            static_cast<volatile std::uint8_t *>(this->channel_bar);

        // Where the guest put its admin completion queue and how deep it
        // made it, read from the controller rather than remembered: the
        // guest chooses both afresh on every reset.
        auto attributes =
            nvme::admin_queue_attributes{arch::x86_64::read32(
                bar + nvme::offset_of(
                          nvme::register_offset::admin_queue_attributes))};
        auto completion_base = arch::x86_64::read64(
            bar + nvme::offset_of(
                      nvme::register_offset::admin_completion_queue_base));

        auto depth = attributes.completion_queue_size();

        // One page reaches the whole queue at the deepest this will look
        // at - 256 entries of 16 bytes - and ACQ's low twelve bits are
        // reserved, so a base that is not page aligned is a register this
        // does not understand rather than a queue in an unusual place.
        if ((0 == depth) || (depth > nvme::admin_borrow::max_depth) ||
            (0 == completion_base) ||
            (0 != (completion_base & (page_size - 1)))) {
            this->channel_grant_result = 0xf2;
            return;
        }

        // **The doorbell page is deliberately not held for this**, and
        // the reservation's borrow does hold it, so the difference is
        // worth stating. A hold buys exclusion from other processors
        // *writing* doorbells, and this writes none and submits nothing -
        // it reads a queue and changes four bytes of one entry the
        // controller has finished with. What a hold would not buy is the
        // only exposure there is, which is another processor's interrupt
        // service routine *reading* that entry first; see
        // diag::reduce_guest_queue_grant. So it would cost an
        // acknowledgement wait and a wake NMI broadcast at a new point in
        // the boot and close nothing.
        this->mapping_window_lock.lock();
        scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

        // Again, now that this is the only processor in here.
        //
        // The check above was made before the lock, so two processors
        // both taking a doorbell violation could both have passed it and
        // queued up here. Without this the second one patches a
        // completion the first has already reduced, finds the counts no
        // longer equal to the allocation, and replaces a successful
        // result with 0xe8 - a refusal that did not happen.
        if (0 != this->channel_grant_result) {
            return;
        }

        // The same window page the borrow points at the guest's
        // completion queue. Reusing it is safe because both hold the
        // window's lock for the whole of what they read through it, and
        // neither is reachable from inside the other.
        constexpr std::size_t completion_window_page = 4;

        auto * completion = static_cast<nvme::completion_entry *>(
            map_window_at(completion_window_page, completion_base, 1));
        if (!completion) {
            this->channel_grant_result = 0xf3;
            return;
        }

        // Where the controller will post next, worked out from the phase
        // bits alone. No doorbell has to have been observed for this, and
        // no software head pointer of the guest's has to be guessed at -
        // see admin_borrow::locate.
        nvme::admin_borrow::queues where{};
        where.completion = completion;
        where.completion_depth = depth;
        nvme::admin_borrow::locate(where);

        auto slot = where.completion_tail;
        auto phase = where.completion_phase;

        auto ours = [&](const nvme::completion_entry & entry) {
            return (entry.command_id() ==
                    this->channel_grant_command_id) &&
                   (0 == entry.submission_queue_id());
        };

        // **It may already be there, and this is a race rather than a
        // possibility.** The watch applies the guest's store before
        // calling its handler - page_watch::handler, "called after the
        // write has taken effect" - so the doorbell has rung by the time
        // anything here runs, and a Set Features is two DMA round trips
        // for the controller against a few microseconds of reading the
        // guest's submission queue here. Either can win.
        //
        // So look at what is already posted before waiting for anything.
        // Newest first, over the slots this lap has written - which are
        // exactly [0, tail), since the host is required to zero a
        // completion queue before enabling it and only the controller
        // writes it afterwards. Newest first is what makes a command
        // identifier the guest has reused earlier in the epoch harmless:
        // the most recent occurrence is the one being answered now.
        //
        // A controller that had wrapped would leave older entries above
        // the tail carrying the previous lap's phase, and this does not
        // look at them. It cannot have: the queue is 256 entries deep,
        // was zeroed at the enable a few commands ago, and this runs
        // during the driver's initialisation. If it somehow had, nothing
        // is found and the wait below times out, which costs the channel
        // and nothing else.
        auto already_posted = false;

        for (std::uint32_t back{1}; back <= where.completion_tail;
             ++back) {
            auto & entry = completion[where.completion_tail - back];
            if (entry.phase() != phase) {
                continue;
            }
            arch::x86_64::order_loads();
            if (ours(entry)) {
                slot = where.completion_tail - back;
                already_posted = true;
                break;
            }
        }

        // The controller writes the phase tag last, which is what makes
        // polling it safe: the same rule Linux's nvme_cqe_pending relies
        // on, testing `(status & 1) == cq_phase` and then issuing
        // dma_rmb() before reading the rest of the entry. order_loads is
        // that barrier, and admin_borrow::run does the same thing in the
        // same order.
        auto budget = std::uint64_t{1} << 24;
        std::uint32_t scanned{};

        while (!already_posted) {
            auto & entry = completion[slot];

            if (entry.phase() != phase) {
                if (0 == budget--) {
                    this->channel_grant_scanned = scanned;
                    this->channel_grant_ticks =
                        arch::x86_64::rdtsc() - started;
                    this->channel_grant_result = 0xf4;
                    return;
                }
                zpp::spin_hint();
                continue;
            }

            arch::x86_64::order_loads();

            if (ours(entry)) {
                break;
            }

            // Somebody else's, which at this moment means another admin
            // command of the guest's - an Asynchronous Event Request
            // completing, or an Identify issued in the same burst. It is
            // stepped past and left completely alone: this reads the
            // queue and writes four bytes of one entry, and consumes
            // nothing.
            ++scanned;
            if (scanned > nvme::admin_borrow::max_foreign) {
                this->channel_grant_scanned = scanned;
                this->channel_grant_ticks =
                    arch::x86_64::rdtsc() - started;
                this->channel_grant_result = 0xf5;
                return;
            }

            slot = (slot + 1) % depth;
            if (0 == slot) {
                phase = !phase;
            }
            budget = std::uint64_t{1} << 24;
        }

        auto & entry = completion[slot];

        this->channel_grant_scanned = scanned;
        this->channel_grant_already_posted = already_posted ? 1 : 0;
        this->channel_grant_reported = entry.command_specific;
        this->channel_grant_ticks = arch::x86_64::rdtsc() - started;

        // A refused Set Features has no allocation in it to edit, and the
        // guest has an error to handle that is none of our business.
        if (0 != entry.status()) {
            this->channel_grant_result = 0xe6;
            return;
        }

        auto granted_submission =
            nvme::number_of_submission_queues(entry.command_specific);
        auto granted_completion =
            nvme::number_of_completion_queues(entry.command_specific);

        // The entry has to be the answer to a Number of Queues, and this
        // is what proves it rather than assumes it.
        //
        // 5.2.30.1.5 freezes the allocation at the first Set Features
        // completed after a controller level reset, and ours was that
        // one - so the guest's answer must be exactly what our
        // reservation was granted. Two things ride on the check. It is
        // the discriminator that makes matching on a command identifier
        // safe, since an entry that carried the same identifier for some
        // other command would have to hold this exact pair of counts in
        // DW0 by coincidence. And a disagreement falsifies the ordering
        // argument the whole reservation rests on, which is worth
        // recording loudly rather than papering over by editing whatever
        // was found.
        if ((granted_submission !=
             this->channel_allocated_submission_queues) ||
            (granted_completion !=
             this->channel_allocated_completion_queues)) {
            this->channel_grant_result = 0xe8;
            return;
        }

        // Reduce a half only where the guest's own request leaves nothing
        // above it. Measured on the rig that is the submission half
        // alone: it asks for sixteen submission queues against an
        // allocation of sixteen, and eight completion queues against an
        // allocation of sixteen, so the completion half already has eight
        // spare identifiers and is left exactly as the controller wrote
        // it.
        auto present_submission = granted_submission;
        auto present_completion = granted_completion;
        auto wanted = false;
        auto too_small = false;

        if (this->channel_guest_requested_submission_queues >=
            granted_submission) {
            wanted = true;
            if (granted_submission >= 2) {
                present_submission = granted_submission - 1;
            } else {
                too_small = true;
            }
        }

        if (this->channel_guest_requested_completion_queues >=
            granted_completion) {
            wanted = true;
            if (granted_completion >= 2) {
                present_completion = granted_completion - 1;
            } else {
                too_small = true;
            }
        }

        if (!wanted) {
            // Nothing to do, and that is a success rather than a
            // refusal: there is already an identifier above what the
            // guest can reach in both spaces.
            this->channel_guest_granted_submission_queues =
                granted_submission;
            this->channel_guest_granted_completion_queues =
                granted_completion;
            this->channel_grant_presented = entry.command_specific;
            this->channel_grant_result = 2;
            return;
        }

        // A controller with one queue is a controller whose only queue
        // belongs to the guest. No channel is better than a boot disk
        // with nowhere to submit.
        if (too_small) {
            this->channel_grant_result = 0xe7;
            return;
        }

        // Both halves zero's based, the same encoding the request uses -
        // see nvme::number_of_submission_queues.
        auto presented = ((present_submission - 1) & 0xffffu) |
                         (((present_completion - 1) & 0xffffu) << 16);

        entry.command_specific = presented;
        arch::x86_64::order_stores();

        this->channel_guest_granted_submission_queues = present_submission;
        this->channel_guest_granted_completion_queues = present_completion;
        this->channel_grant_presented = presented;
        this->channel_grant_result = 1;

        diag::log<diag::severity::info>(
            "grant {} shown to the guest as {}",
            static_cast<std::uint64_t>(this->channel_grant_reported),
            static_cast<std::uint64_t>(presented));
    }
}

void hypervisor::stop_watching_channel_doorbells()
{
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset) {
        return;
    } else {
        // Left armed where the observation build asked for it: that
        // switch exists to price a permanently trapped doorbell page, and
        // removing the trap under it would measure nothing.
        if (diag::observe_controller_admin || !this->channel_bar ||
            !this->channel_doorbell_watched) {
            return;
        }

        auto doorbell_page =
            this->host_page_table.virtual_to_physical(
                const_cast<const void *>(this->channel_bar)) +
            nvme::offset_of(nvme::register_offset::doorbell_base);

        unwatch_guest_page(doorbell_page);
        this->channel_doorbell_watched = false;
    }
}

void hypervisor::forget_channel_queue_observations()
{
    if constexpr (!diag::policy_of(diag::sink::esp_blocks).present ||
                  !diag::rebuild_channel_after_reset) {
        return;
    } else {
        // A controller level reset clears the Number of Queues allocation
        // and deletes every I/O queue - 5.2.30.1.5 - so every number here
        // describes a controller that no longer exists. Keeping any of it
        // would let the next epoch reason from the last one's
        // identifiers.
        this->channel_reserve_result = 0;
        this->channel_reserve_status = 0;
        this->channel_reserve_allocation = 0;
        this->channel_allocated_submission_queues = 0;
        this->channel_allocated_completion_queues = 0;

        this->channel_guest_requested_submission_queues = 0;
        this->channel_guest_requested_completion_queues = 0;
        this->channel_guest_highest_submission_queue = 0;
        this->channel_guest_highest_completion_queue = 0;
        this->channel_guest_created_submission_queues = 0;
        this->channel_guest_created_completion_queues = 0;
        this->channel_guest_deleted_queues = 0;

        // What the guest was told describes the allocation the reset has
        // just cleared, and the command identifier describes a command
        // that can never complete now: the controller discards everything
        // outstanding at a controller level reset.
        this->channel_grant_result = 0;
        this->channel_grant_reported = 0;
        this->channel_grant_presented = 0;
        this->channel_grant_scanned = 0;
        this->channel_grant_already_posted = 0;
        this->channel_grant_ticks = 0;
        this->channel_guest_granted_submission_queues = 0;
        this->channel_guest_granted_completion_queues = 0;
        this->channel_grant_command_id = 0;
        this->channel_grant_pending = false;

        this->channel_create_result = 0;
        this->channel_create_status = 0;
        this->channel_created_submission_id = 0;
        this->channel_created_completion_id = 0;

        this->channel_expected_admin_tail = 0;
        this->admin_observed_head = 0;

        stop_watching_channel_doorbells();
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
        // One identifier in both spaces here, and correctly so: the
        // excursion runs with the controller reset and nothing of the
        // guest's on it, so identifier space is empty and the pair is
        // this VMM's to name.
        diag::esp_block_sink::adopt_rebuilt_queue(
            bar,
            this->channel_doorbell_stride,
            this->channel_queue_id,
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

std::optional<std::uint64_t>
hypervisor::translate_guest_linear(std::size_t cpu, std::uint64_t linear)
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
        {39, 0}, // no 512 GB pages exist
        {30, 1ull << 30},
        {21, 1ull << 21},
        {12, 0}, // the last level always terminates
    };

    // Which processor's second-level guest this is, if any, is now the
    // caller's to say. Every table read below goes through the guest
    // hypervisor's extended page tables when one is running, because the
    // addresses in that guest's page tables are physical in *its*
    // hypervisor's address space and not in this VMM's - see
    // `l2_physical_to_l1`, which is where the whole of that reasoning
    // lives.
    //
    // It was `this->vmcs.vpid() - 1`, which is a VMREAD of a field
    // `setup_vmcs` set to `cpu + 1` and nothing ever writes again. Every
    // caller in nested_entry.cpp already had `cpu` as a parameter.

    for (std::size_t level{}; level < 4; ++level) {
        auto reachable = l2_physical_to_l1(cpu, table);
        if (!reachable) {
            return {};
        }

        auto * entries = static_cast<const std::uint64_t *>(
            map_window_at(transfer_window_first_page, *reachable, 1));
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

arch::x86_64::code_size hypervisor::guest_code_size()
{
    // SDM Table 27-2, "Format of Access Rights": bit 13 is "L - 64-bit
    // mode active (for CS only)" and bit 14 is "D/B - Default operation
    // size (0 = 16-bit segment; 1 = 32-bit segment)".
    constexpr std::uint64_t long_mode_code = 1ull << 13;
    constexpr std::uint64_t default_operation_size = 1ull << 14;

    auto rights = this->vmcs.guest_cs_access_rights();

    if (0 != (rights & long_mode_code)) {
        return arch::x86_64::code_size::bits_64;
    }

    // Read from the segment and not from the paging mode. A guest in
    // protected mode with paging on can still be executing a 16-bit code
    // segment, and a processor this VMM has just started out of a start-up
    // IPI is in real mode with these very bits clear.
    return (0 != (rights & default_operation_size))
               ? arch::x86_64::code_size::bits_32
               : arch::x86_64::code_size::bits_16;
}

std::optional<arch::x86_64::decoded_instruction>
hypervisor::decode_guest_instruction(std::size_t cpu,
                                     arch::x86_64::context & context)
{
    // The instruction is at the guest's RIP, which is a linear address
    // in the guest's own address space, so it takes the guest's page
    // tables to find - not this VMM's.
    //
    // From `context`, which `on_vm_exit` filled from the VMCS at the top
    // of this exit. This is called from `on_ept_violation`, which is
    // 23% of exits on the rig, and reading the field again is an exit to
    // the layer below.
    auto rip = context.rip;

    // Serialised, because the window is now one shared pair of pages -
    // and taken before the walk, which reaches through it too.
    this->mapping_window_lock.lock();
    scope_exit release{[&] { this->mapping_window_lock.unlock(); }};

    auto physical = this->translate_guest_linear(cpu, rip);
    if (!physical) {
        return {};
    }

    // Both translations are done before either page is mapped, because
    // the walk and the instruction fetch share a window page and the walk
    // must not be re-pointed under itself.
    constexpr std::uint64_t page_mask =
        ~static_cast<std::uint64_t>(page_size - 1);
    auto tail_linear = (rip & page_mask) + page_size;
    auto tail_physical = this->translate_guest_linear(cpu, tail_linear);

    // Both translations end in the address space of whichever guest is
    // running, so both need the same last step before anything maps them.
    // `translate_guest_linear` walks *through* the guest hypervisor's
    // extended page tables and hands back an address still inside its
    // guest - see `l2_physical_to_l1` - and mapping that directly is how
    // the decoder came to read unrelated memory and answer with a
    // plausible instruction.
    auto reachable = this->l2_physical_to_l1(cpu, *physical);
    if (!reachable) {
        return {};
    }

    std::optional<std::uint64_t> tail_reachable;
    if (tail_physical) {
        if (auto translated =
                this->l2_physical_to_l1(cpu, *tail_physical)) {
            tail_reachable = *translated;
        }
    }

    // Fifteen bytes is the architectural maximum length of an
    // instruction, and it may straddle a page boundary, which is why the
    // window is two pages. They need not be contiguous in guest physical
    // memory, so the second is mapped from its own translation rather
    // than assumed to follow the first.
    constexpr std::size_t longest_instruction = 15;

    auto first_page = instruction_window_first_page(cpu);
    auto * bytes = static_cast<const std::uint8_t *>(
        map_window_at(first_page, *reachable, 1));
    if (!bytes) {
        return {};
    }

    std::uint8_t code[longest_instruction]{};

    auto offset = *reachable & (page_size - 1);
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
        if (tail_reachable) {
            if (auto * tail = static_cast<const std::uint8_t *>(
                    map_window_at(first_page + 1, *tail_reachable, 1))) {
                __builtin_memcpy(
                    code + in_first, tail, longest_instruction - in_first);
            }
        }
    }

    // The guest's own code segment decides what these bytes mean, and the
    // decoder refuses anything that is not 32- or 64-bit code rather than
    // reading it as long mode and reporting a length that is short. A
    // refusal here costs the observation and nothing else: the caller
    // falls back to stepping the guest's own instruction.
    //
    // Reachable, and not only through real mode. A real-mode guest is
    // already excluded upstream, because translate_guest_linear walks
    // 4-level paging only - but a *compatibility-mode* code segment under
    // that same paging has L clear and D/B either way, and its
    // instructions are fetched here perfectly happily.
    auto store = arch::x86_64::decode(
        std::as_bytes(std::span{code}), context, guest_code_size());

    // Kept for the trace, so a failing emulation can be identified by its
    // opcode rather than by inference.
    for (std::size_t i{}; i < sizeof(this->last_fetched_code); ++i) {
        this->last_fetched_code[i] = code[i];
    }

    // What was refused, so the forms can be named rather than counted.
    //
    // The bytes are already here - the fetch above did the work - and a
    // count has already proved insufficient once: it said forty-five per
    // cent of a guest's local APIC writes were refused and could not say
    // which instructions they were, which is what decides whether covering
    // them is a morning's work or a decoder rewrite.
    if (!store) {
        record_refused_instruction(code, refusal::not_decoded);
        return store;
    }

    // A decode that cannot be true of the instruction that faulted.
    //
    // A watch clears the write permission and nothing else, so reads of
    // the page stay permitted and a pure read of one cannot fault. The
    // caller has already required bit 8 of the exit qualification, so this
    // is not a paging-structure access either. So the access was the
    // instruction's own operand and it was a write, and an instruction
    // that leaves memory alone cannot have produced it.
    //
    // SDM, "Exit Qualification for EPT Violations": bit 1 is "Set if the
    // access causing the EPT violation was a data write." A read-modify-
    // write sets bit 0 as well, which is why the test is on bit 1 alone
    // rather than on the pair.
    //
    // Nothing here should ever be true. If it is, the decoder was handed
    // bytes that are not the faulting instruction - a wrong answer from
    // translate_guest_linear, or a window pointed at the wrong page - and
    // the paths that *do* write have been writing fabricated values at
    // fabricated lengths all along, silently. So it is counted, the bytes
    // are kept, and the decode is refused: carrying it out would advance
    // the guest past its own write by a length measured from unrelated
    // bytes, and the write would simply be lost. Stepping loses only the
    // observation.
    constexpr std::uint64_t qualification_data_write = 1ull << 1;
    auto wrote =
        0 != (this->vmcs.exit_qualification() & qualification_data_write);
    auto leaves_memory_alone =
        (arch::x86_64::memory_operation::load == store->what) ||
        (arch::x86_64::memory_operation::examine == store->what);

    if (wrote && leaves_memory_alone) {
        this->impossible_decodes = this->impossible_decodes + 1;
        record_refused_instruction(code, refusal::impossible_operation);
        log("impossible decode at rip {}, page {}",
            rip,
            this->vmcs.guest_physical_address() >> 12);
        return {};
    }

    return store;
}

void hypervisor::record_refused_instruction(const std::uint8_t * code,
                                            refusal why)
{
    auto slot = this->refused_instruction_count;
    this->refused_instruction_count = slot + 1;

    if (slot >= refused_instruction_capacity) {
        // Frozen when full rather than wrapping, because the interesting
        // ones arrive during start-up.
        return;
    }

    auto & recorded = this->refused_instructions[slot];
    for (std::size_t i{}; i < refused_instruction_bytes; ++i) {
        recorded.code[i] = code[i];
    }
    recorded.page = this->vmcs.guest_physical_address() >> 12;
    recorded.why = why;
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

void hypervisor::initialize_vmx(std::size_t cpu)
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    // One pair per virtual processor: a VMCS may not be active on more
    // than one logical processor.
    //
    // Indexed by this processor's own slot rather than by a shared
    // counter. The counter was claimed here and advanced later, inside
    // vm_launch, and between those two the starter has already been
    // released to bring up the next processor - so which entry this
    // picked depended on another processor's timing.
    auto & vmx = this->vmx[cpu];
    auto & vmx_vmcs = this->vmx_vmcs[cpu];

    // Its low bits are the VMCS revision identifier, which VMPTRLD
    // checks against the first dword of the region.
    const auto & basic_msr = this->cached_vmx_msr(vmx_msr::basic);

    // The processor reaches all of these by physical address, through no
    // page table of ours.
    //
    // The VMXON and VMCS regions used to be published here too, in two
    // shared members that `enter_root_mode` read back. That is the one
    // piece of this launch that is *not* per processor, and it was
    // handed between two functions through class state - so a second
    // processor reaching this line between another's write and its
    // VMXON put its own regions there, and the first one entered VMX
    // operation on somebody else's. One VMCS on two logical processors
    // is exactly what the comment above says must not happen.
    //
    // Nothing published it that did not immediately consume it, so the
    // members are gone rather than made per processor:
    // `enter_root_mode` derives both from its own slot, which is the
    // same thing `own_vmxon_region_physical` and
    // `own_vmcs_region_physical` already do for the sleep path. The rest
    // below stays, because every processor computes the same value for
    // it - one extended page table root, one MSR bitmap, one pair of I/O
    // bitmaps, shared by every VMCS by design.
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

std::optional<std::uint64_t>
hypervisor::wait_for_l2_start_up_ipi(std::size_t cpu)
{
    if (cpu >= max_cpus) {
        return std::nullopt;
    }

    auto & handoff = this->start_up_handoff[cpu];

    // Publish that this processor is listening, unless a vector is
    // already sitting there.
    //
    // A compare-exchange rather than a store, for the reason
    // emulate_init_signal gives at the other end of the same mailbox: a
    // sender may be handing a vector over at this instant, and a store
    // would drop it while the sender had already swallowed the guest's
    // write that would have produced another.
    auto state = handoff.load();
    for (;;) {
        if (start_up_handoff_state::is_delivered(state)) {
            handoff.store(start_up_handoff_state::none);
            return start_up_handoff_state::vector(state);
        }

        if (handoff.compare_exchange_strong(
                state, start_up_handoff_state::software_wait)) {
            break;
        }
    }

    // Bounded, and the bound is not a fallback - there is nothing to fall
    // back to. It is how often this processor returns to its exit
    // handler while it waits: the diagnostic channel is pumped there, the
    // poll is re-armed there, and a processor that never gets there is
    // indistinguishable from the wedged one this whole path exists to
    // prevent. `enter_or_park_l2` comes straight back on the guest
    // hypervisor's next VMLAUNCH.
    constexpr std::uint32_t start_up_wait_attempts = 200000;

    for (std::uint32_t attempt{}; attempt < start_up_wait_attempts;
         ++attempt) {
        if (auto delivered = handoff.load();
            start_up_handoff_state::is_delivered(delivered)) {
            handoff.store(start_up_handoff_state::none);
            return start_up_handoff_state::vector(delivered);
        }

        zpp::spin_hint();
    }

    // Left published on purpose. The mailbox holds a value rather than an
    // edge, so a vector deposited during the microseconds this processor
    // spends in its exit handler is still there when it comes back - and
    // reverting to hardware_wait would open a window in which a sender
    // issued a real start-up IPI to a processor sitting in root mode,
    // where it is discarded. What ends the publication is leaving this
    // state at all, which `enter_or_park_l2` does by entering or by
    // reflecting.
    return std::nullopt;
}

void hypervisor::emulate_start_up_ipi(arch::x86_64::context & context,
                                      std::uint64_t vector)
{
    // Logged because this is one of three ways into apply_start_up and
    // the only one with no line of its own, which left "who called it"
    // unanswerable from a log - and the answer is the whole question when
    // apply_start_up declines to apply anything.
    log("cpu {} start-up ipi exit, vector {}", this->vmcs.vpid(), vector);

    // Reached whenever this processor's INIT left it waiting on hardware,
    // which is every INIT on bare metal and under Bochs. Under a layer
    // that discards the IPI while this VMM is in root mode the INIT
    // handler waits for the vector in root mode instead and has already
    // applied it by now, so this exit never arrives there.
    apply_start_up(context, vector, "sipi exit");
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

std::uint64_t hypervisor::own_vmxon_region_physical(std::size_t cpu)
{
    if (cpu >= max_cpus) {
        return 0;
    }
    return this->host_page_table.virtual_to_physical(&this->vmx[cpu]);
}

std::uint64_t hypervisor::own_vmcs_region_physical(std::size_t cpu)
{
    // A slot, counting from zero. These read `vmcs.vpid()` - which counts
    // from one - and subtracted one from it, and the subtraction has
    // moved to the caller along with the read. `reflect_l2_exit` calls
    // this on every reflection, so it was a VMREAD on the hottest path
    // there is to ask a question whose answer is a parameter two frames
    // up.
    if (cpu >= max_cpus) {
        return 0;
    }
    return this->host_page_table.virtual_to_physical(&this->vmx_vmcs[cpu]);
}

std::expected<void, zpp::error> hypervisor::quiesce_and_sleep(
    std::uint16_t port, std::uint32_t value, std::uint8_t bytes)
{
    // Which regions this processor is actually using.
    //
    // Still derived from the VPID here, and deliberately: this is the
    // sleep path and it runs once per suspend, so threading an index
    // down through `on_io_instruction` and `on_sleep_request` would be
    // churn against two calls that never repeat. The read moved out of
    // the two accessors and up to here, which turns two VMREADs into
    // one - and everything on an exit path passes its own index instead.
    // `on_sleep_request` above still reads the field twice for the same
    // reason.
    auto slot = this->vmcs.vpid();
    auto vmxon_region = own_vmxon_region_physical(slot - 1);
    auto vmcs_region = own_vmcs_region_physical(slot - 1);
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
    // across the whole of borrow_guest_admin_queue, start_up_lock across a
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

            // And so is the allocation, and every identifier that was
            // chosen against it. A controller level reset clears the
            // Number of Queues feature and deletes every I/O queue -
            // NVMe Base 5.2.30.1.5 - so the next epoch has to observe
            // the guest again from nothing.
            self.forget_channel_queue_observations();
        } else if (!was && now) {
            // Coming back up. Handled here when the write is caught, and
            // by poll_for_controller_return when it is not - see there
            // for why catching it cannot be relied on.
            //
            // Reserving, not creating. This is the only moment the
            // reservation is legal - after the reset that cleared the
            // allocation and before any I/O queue exists - and creating
            // here is what boot looped the guest, because the guest's own
            // Set Features then arrived after our queues existed.
            self.reserve_channel_queue_allocation();
        }
    }
}

void hypervisor::on_doorbell_write(void * context,
                                   std::uint64_t page,
                                   const hypervisor::guest_write * write)
{
    static_cast<void>(page);

    // Armed for two different reasons and this serves both. The
    // observation build wants every command the guest submits, recorded
    // and counted; the rebuild wants only the Number of Queues request
    // and the Create identifiers, and wants to know when the guest has
    // finished creating so that it can create its own above them.
    if constexpr (!diag::observe_controller_admin &&
                  !diag::rebuild_channel_after_reset) {
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

        auto tail = static_cast<std::uint32_t>(write->value);

        {
            // Everything between what was looked at last and what the
            // guest has just published, read through the window.
            //
            // The lock is released before anything below it runs: a
            // borrow takes the same lock and it is not recursive, so a
            // create triggered from here has to happen outside this
            // block.
            self.mapping_window_lock.lock();
            scope_exit release{[&] { self.mapping_window_lock.unlock(); }};

            self.observe_guest_admin_submissions(tail);
        }

        // The guest's Number of Queues answer, edited before it reads it.
        //
        // Here rather than inside the observation because the window's
        // lock is not recursive and the poll below takes it, and because
        // the wait has to happen after the doorbell has rung: the
        // controller does not fetch the command until it has, and the
        // watch applies the guest's store before calling this.
        if constexpr (diag::rebuild_channel_after_reset &&
                      diag::reduce_guest_queue_grant) {
            if (self.channel_grant_pending) {
                self.channel_grant_pending = false;
                self.patch_guest_queue_grant();

                // With the creation off there is nothing left for the
                // watch to see, and leaving it armed is an exit per disk
                // command for the rest of the boot - the stride is zero
                // on this controller, so every I/O doorbell is on this
                // page. This is the control run for the reduction on its
                // own: the guest is told fifteen, creates fifteen, and
                // nothing is created behind it.
                if constexpr (!diag::create_channel_queue_after_guest) {
                    self.stop_watching_channel_doorbells();
                }
            }
        }

        // Whether the guest has finished creating its own queues.
        //
        // There is no signal for that, so this is the closest thing the
        // guest itself provides: it has now created at least as many
        // queues as it asked the controller for, in both spaces. Measured
        // on the rig that is exactly right - it asks for sixteen
        // submission and eight completion queues and creates precisely
        // those - and it is also what makes the identifier chosen below
        // safe if it is wrong in the generous direction, since the
        // identifier is one above the higher of what was created and what
        // was requested.
        //
        // A guest that creates *more* than it asked for would defeat
        // this, and no driver does: both clamp their creates to the
        // smaller of their own request and the allocation.
        //
        // Clamped by what it was told, not only by what it asked for.
        // With diag::reduce_guest_queue_grant on, the guest is told one
        // less than the controller granted in whichever space had no
        // spare, and it then creates one less than it asked for - so
        // waiting for its request to be met would wait for ever. With
        // that switch off the granted count is zero, queue_limit answers
        // the request, and this is exactly what it was.
        if constexpr (diag::rebuild_channel_after_reset &&
                      diag::create_channel_queue_after_guest) {
            auto due =
                (1 == self.channel_reserve_result) &&
                (0 == self.channel_create_result) &&
                (0 == self.channel_guest_deleted_queues) &&
                (0 != self.channel_guest_requested_submission_queues) &&
                (0 != self.channel_guest_requested_completion_queues) &&
                (self.channel_guest_created_submission_queues >=
                 queue_limit(
                     self.channel_guest_requested_submission_queues,
                     self.channel_guest_granted_submission_queues)) &&
                (self.channel_guest_created_completion_queues >=
                 queue_limit(
                     self.channel_guest_requested_completion_queues,
                     self.channel_guest_granted_completion_queues));

            if (due) {
                self.create_channel_queue();
            }
        }
    }
}

void hypervisor::observe_guest_admin_submissions(std::uint32_t tail)
{
    if constexpr (!diag::observe_controller_admin &&
                  !diag::rebuild_channel_after_reset) {
        static_cast<void>(tail);
        return;
    } else {
        auto * bar =
            static_cast<volatile std::uint8_t *>(this->channel_bar);

        // Where the guest put its admin submission queue, and how big it
        // said it was. Read from the controller rather than remembered,
        // because the driver may have moved it since - this runs from the
        // driver's own initialisation, which is when it sets them.
        auto queue_base = arch::x86_64::read64(
            bar + nvme::offset_of(
                      nvme::register_offset::admin_submission_queue_base));
        auto attributes = arch::x86_64::read32(
            bar + nvme::offset_of(
                      nvme::register_offset::admin_queue_attributes));

        auto entries =
            (attributes & 0xfff) + 1; // ASQS is a zero's based count
        if (!queue_base || !entries) {
            return;
        }

        if (tail >= entries) {
            return;
        }

        constexpr std::uint32_t command_size = 64;

        // Wrapping is why this is a loop rather than a subtraction.
        for (auto at = this->admin_observed_head; at != tail;
             at = (at + 1) % entries) {
            auto offset = static_cast<std::uint64_t>(at) * command_size;
            auto * command =
                static_cast<const std::uint32_t *>(this->map_window_at(
                    transfer_window_first_page, queue_base + offset, 1));
            if (!command) {
                break;
            }

            // What the rebuild needs, which is three commands out of the
            // whole admin command set.
            //
            // Read here rather than off the recorded ring, because the
            // ring is the observation build's and freezes when full,
            // while these have to be right whatever else the guest has
            // submitted.
            if constexpr (diag::rebuild_channel_after_reset) {
                auto opcode = static_cast<std::uint8_t>(command[0] & 0xff);
                auto queue_id = command[10] & 0xffffu;

                switch (static_cast<nvme::admin_opcode>(opcode)) {
                case nvme::admin_opcode::set_features:
                    // Only the Number of Queues feature. CDW10 bits 7:0
                    // are the feature identifier; the guest issues Set
                    // Features constantly for power management once it
                    // has settled, and those say nothing about queues.
                    if (static_cast<std::uint8_t>(command[10] & 0xff) ==
                        static_cast<std::uint8_t>(
                            nvme::feature_identifier::number_of_queues)) {
                        // The request, not a grant: this is the guest's
                        // own submission, so it is what the guest wants
                        // rather than what the controller answered.
                        this->channel_guest_requested_submission_queues =
                            nvme::number_of_submission_queues(command[11]);
                        this->channel_guest_requested_completion_queues =
                            nvme::number_of_completion_queues(command[11]);

                        // Which command's completion has to be edited.
                        // CDW0 bits 31:16 are the identifier the guest
                        // chose, and it is the only way to recognise
                        // that completion among whatever else the
                        // controller posts in the same window.
                        if constexpr (diag::reduce_guest_queue_grant) {
                            this->channel_grant_command_id =
                                static_cast<std::uint16_t>(command[0] >>
                                                           16);
                            this->channel_grant_pending = true;
                        }
                    }
                    break;

                case nvme::admin_opcode::create_io_completion_queue:
                    this->channel_guest_created_completion_queues =
                        this->channel_guest_created_completion_queues + 1;
                    if (queue_id >
                        this->channel_guest_highest_completion_queue) {
                        this->channel_guest_highest_completion_queue =
                            queue_id;
                    }
                    break;

                case nvme::admin_opcode::create_io_submission_queue:
                    this->channel_guest_created_submission_queues =
                        this->channel_guest_created_submission_queues + 1;
                    if (queue_id >
                        this->channel_guest_highest_submission_queue) {
                        this->channel_guest_highest_submission_queue =
                            queue_id;
                    }
                    break;

                case nvme::admin_opcode::delete_io_completion_queue:
                case nvme::admin_opcode::delete_io_submission_queue:
                    // A driver deleting queues is taking the controller
                    // apart, and creating one behind it at that point is
                    // the worst available moment. Nothing more is created
                    // until the next enable.
                    this->channel_guest_deleted_queues =
                        this->channel_guest_deleted_queues + 1;
                    break;

                default:
                    break;
                }
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
            if constexpr (diag::observe_controller_admin) {
                if (this->admin_observation_count >=
                    admin_observation_capacity) {
                    this->admin_observation_count =
                        this->admin_observation_count + 1;
                    continue;
                }

                auto slot = this->admin_observation_count;

                this->admin_observations[slot] = admin_observation{
                    .command = command[0],
                    .dword_10 = command[10],
                    .dword_11 = command[11],
                    .namespace_id = command[1],
                };

                this->admin_observation_count =
                    this->admin_observation_count + 1;
            }
        }

        this->admin_observed_head = tail;

        // What a borrow compares SQHD against. The doorbell cannot be
        // read back, so this is the only record of where the guest's
        // submission tail is.
        this->channel_expected_admin_tail = tail;
    }
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
    //
    // The blob's own page is the one place in this VMM's address space
    // that is writable and executable at the same time, and it has to be:
    // the code fetched from it writes its own progress marker and its own
    // data area, both of which live in that same page because a start-up
    // vector names a page and the blob may not span two. What can be
    // taken away is the executable half of the three pages behind it,
    // which are the temporary page table - walked by the processor
    // through physical addresses and never fetched from.
    this->host_page_table.map_from(
        memory,
        page_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write |
            arch::x86_64::page_table::protection::execute,
        this->os_page_table);

    this->host_page_table.map_from(
        memory + page_size,
        (arch::x86_64::ap_start_up_pages - 1) * page_size,
        arch::x86_64::page_table::protection::read |
            arch::x86_64::page_table::protection::write,
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

    // With write protection, unlike the value the guest's CR0 field gets:
    // a processor climbing this trampoline lands directly in this VMM's
    // C++, on the host page table, and never passes through a VM exit
    // that would have loaded it - so this is the only place it can be
    // given.
    area.host_cr0 = this->host_control_register_0();
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

void hypervisor::record_exit(std::size_t cpu,
                             arch::x86_64::vmx::exit_reason reason,
                             const arch::x86_64::context & context)
{
    auto & vmcs = this->vmcs;

    // The index comes from the caller now. It was `vmcs.vpid() - 1`, and
    // this function runs once on every exit, so that was one VMREAD per
    // exit - 1.4-1.8 microseconds under a host with no VMCS shadowing -
    // to recover a number `on_vm_exit` was handed as its first parameter.
    // Guarded anyway: an out of range index here would corrupt whatever
    // follows the ring.
    if (cpu >= max_cpus) {
        return;
    }

    auto & count = this->exit_trace_count[cpu];

    exit_trace_entry recorded{};
    recorded.reason = reason.value();
    recorded.rip = vmcs.guest_rip();
    recorded.repeated = 1;

    // Three fields, three VMCS reads, on every exit - and behind a switch
    // for that reason. See `nested_vmx::census_exits`: the census over
    // our own reads put `exit_qualification` at 6.1 per round trip,
    // `guest_activity_state` at 7.9 and `guest_cs_selector` at 9.1, and
    // this is a reader of all three. A VMREAD is an exit to the layer
    // below at 1.4-1.8 microseconds, because nothing under this VMM
    // offers VMCS shadowing.
    //
    // Off, they read zero, and **zero is a legal value for all three** -
    // so nothing in the ring says the switch was off. The build manifest
    // does, `census=`, and `check-bootable.sh` prints it on every deploy.
    //
    // The instruction pointer above is not gated, because `on_vm_exit`
    // has already read it for this exit; this is a second read only
    // because a reflection makes vmcs01 current and its guest RIP is a
    // different quantity from `context.rip` - which is the whole of what
    // `rip_owner` below is about.
    if constexpr (nested_vmx::census_exits) {
        recorded.qualification = vmcs.exit_qualification();
        recorded.activity_state = vmcs.guest_activity_state();
        recorded.cs_selector = vmcs.guest_cs_selector();
    }

    // The privilege level of the *first* level guest, on every exit it
    // takes. Free: the selector was read one line above for the ring.
    //
    // This exists because sampling cannot answer the question it kept
    // being asked. `l2_cpl_seen` is a census over every second-level
    // entry and settles the nested case beyond argument; with nested VMX
    // off there is no second level and no such counter, and the fallback
    // was hundreds of `info registers` samples - which reported CPL 0
    // every time and was read as "ring 3 was never reached". A booted,
    // idle Windows is at CPL 0 in its idle loop essentially all of the
    // time, so that reading was equally consistent with a machine at the
    // desktop and one livelocked in kernel code.
    //
    // **Read `cpl_seen[3]` as a proof of existence, not as a
    // distribution.** One exit taken at ring 3 proves the guest reached
    // user mode; a zero is weaker, because the exits this VMM takes are
    // biased towards kernel work and there are few of them when nested
    // VMX is off. That asymmetry is the point - the question is whether
    // user mode is reached at all.
    if constexpr (nested_vmx::census_exits) {
        this->cpl_seen[cpu][recorded.cs_selector & 3] += 1;
    }

    // Whose instruction pointer that is. `running_l2` is cleared by
    // `reflect_l2_exit` on its way out, so a processor that was running a
    // second-level guest and is not now is one whose exit was reflected -
    // and the address just read is the guest hypervisor's resume site
    // rather than its guest's. See the field for what reading it the
    // other way cost.
    // Whose instruction pointer that is, which the three cases in
    // `rip_owner` spell out. `running_l2` being set here means vmcs02 is
    // current and the address is the second-level guest's; the reflection
    // flag, set on the way out of `reflect_l2_exit`, means vmcs01 is
    // current again and it is the guest hypervisor's resume site.
    if (this->running_l2[cpu]) {
        recorded.rip_owner =
            static_cast<std::uint64_t>(rip_owner::second_level);
    } else if (0 != this->exit_reflected[cpu]) {
        recorded.rip_owner =
            static_cast<std::uint64_t>(rip_owner::first_level);
    } else {
        recorded.rip_owner = static_cast<std::uint64_t>(rip_owner::guest);
    }

    this->exit_reflected[cpu] = 0;

    // Only the two reasons that report one, so nothing else pays a VMREAD
    // on a path taken by every exit.
    constexpr std::uint64_t ept_violation = 48;
    constexpr std::uint64_t ept_misconfiguration = 49;
    constexpr std::uint64_t basic_reason_mask = 0xffff;

    auto basic = recorded.reason & basic_reason_mask;

    if ((ept_violation == basic) || (ept_misconfiguration == basic)) {
        recorded.guest_physical = vmcs.guest_physical_address();
    }

    // What the guest asked for, where the reason alone does not say it.
    //
    // Out of the context rather than the VMCS, so this reads no field and
    // costs a compare and a move on the three reasons it applies to and
    // nothing at all on the rest. See exit_trace_entry::detail for what
    // each packing means and why VMCALL needs two registers.
    constexpr std::uint64_t vmcall = 18;
    constexpr std::uint64_t rdmsr = 31;
    constexpr std::uint64_t wrmsr = 32;
    constexpr std::uint64_t low_half_mask = 0xffffffff;
    constexpr std::uint64_t high_half_shift = 32;

    if ((rdmsr == basic) || (wrmsr == basic)) {
        recorded.detail = context.rcx & low_half_mask;

        // EDX:EAX either way - the answer on a read, the value supplied
        // on a write. See `exit_trace_entry::detail_value`.
        recorded.detail_value =
            ((context.rdx & low_half_mask) << high_half_shift) |
            (context.rax & low_half_mask);
    } else if (vmcall == basic) {
        recorded.detail =
            ((context.rax & low_half_mask) << high_half_shift) |
            (context.rcx & low_half_mask);
    }

    // Counted before the ring is touched, because this is the count that
    // has to be right whether or not the entry merges with the one before
    // it. The bound is the array's rather than the architecture's: a
    // reason past the end of Table C-1 is left uncounted rather than
    // folded onto a neighbour, since a wrong non-zero count is worse than
    // a missing one and the ring below records it either way.
    if (basic < exit_reason_capacity) {
        this->exit_reason_counts[cpu][basic] =
            this->exit_reason_counts[cpu][basic] + 1;
    }

    ++count;

    // A repeat grows the entry already there rather than taking a slot,
    // which is the only thing that keeps this ring worth reading.
    //
    // A guest spinning on a lock takes the same exit at the same RIP about
    // a thousand times a second - measured on the rig, all eight
    // processors inside Hyper-V taking VMX-preemption timer exits at one
    // unchanging RIP - so without this every one of the thirty-two slots
    // holds that line and the sequence that led into the spin, which is
    // the part worth having, has been evicted by the time anyone looks.
    //
    // Compared on everything except the repeat count, so two genuinely
    // different exits never merge. exit_trace_count counts slots written
    // rather than exits taken, because that is what the ring index needs;
    // exit_total counts exits, and the two disagree deliberately - one
    // says what happened, the other how much of it.
    ++this->exit_total[cpu];

    if (count > 1) {
        auto & previous =
            this->exit_trace[cpu][(count - 2) % exit_trace_capacity];

        if ((previous.reason == recorded.reason) &&
            (previous.qualification == recorded.qualification) &&
            (previous.activity_state == recorded.activity_state) &&
            (previous.cs_selector == recorded.cs_selector) &&
            (previous.rip == recorded.rip) &&
            (previous.guest_physical == recorded.guest_physical) &&
            (previous.detail == recorded.detail) &&
            (previous.detail_value == recorded.detail_value)) {
            ++previous.repeated;
            --count;
            return;
        }
    }

    this->exit_trace[cpu][(count - 1) % exit_trace_capacity] = recorded;
}

void hypervisor::inject_general_protection_fault(std::uint64_t error_code)
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

    // Zero by default, which is what a general protection fault that is
    // not a segment violation pushes. The refused task switch is the one
    // caller that passes something: a #GP raised by a task switch carries
    // the selector it could not switch to.
    this->vmcs.vm_entry_exception_error_code(error_code);
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

    // Who this was, which the record could not say before. The VPID is
    // this VMM's virtual processor number and the slot is its index into
    // the per processor arrays; both are recorded because they are
    // maintained separately and agree only by construction.
    record.virtual_processor = vmcs.vpid();

    auto cpu = (0 != record.virtual_processor)
                   ? (record.virtual_processor - 1)
                   : 0;
    record.cpu = cpu;

    if (cpu < max_cpus) {
        record.from_trampoline = this->started_by_trampoline[cpu] ? 1 : 0;
        record.start_up_vector = this->guest_start_up_vector[cpu];
    }

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
    log("stopping, vm entry failed, cpu {} vp {} from trampoline {} "
        "start-up vector {} cs {} rights {} reason {} instruction error "
        "{} "
        "activity {} rip {} cr0 {} cr4 {} rflags {}",
        record.cpu,
        record.virtual_processor,
        record.from_trampoline,
        record.start_up_vector,
        record.guest_cs_selector,
        record.guest_cs_access_rights,
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

void hypervisor::setup_vmcs(std::size_t cpu,
                            arch::x86_64::context & guest_context)
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    auto & vmcs = this->vmcs;

    // Zero the VMX abort indicator, as the SDM recommends for any VMCS
    // this VMM uses. A VMX abort is a failure during a VM *exit*: it puts
    // the processor into a shutdown state that only RESET leaves, and it
    // is not a VM entry failure, so nothing else here would notice one.
    // The indicator names the cause - but only if it was known to be zero
    // beforehand, which is what this is for.
    this->vmx_vmcs[cpu].abort_indicator = 0;

    // All ones is the "no linked VMCS" value. Any other value is taken
    // as the address of a shadow VMCS and checked as one on VM entry,
    // SDM 29.3.1.5 - zero would name physical page zero.
    vmcs.vmcs_link_pointer(0xffffffffffffffffull);

    // Must be non-zero with VPID enabled (SDM 29.2.1.1), and it doubles
    // as this VMM's processor index - hence counting from one.
    //
    // Derived from the slot rather than taken from a shared counter, so
    // that `vmcs.vpid() - 1` **is** the slot by construction. It was
    // equal by accident before: the counter was read here and advanced
    // later, inside vm_launch, and the starter was released in between -
    // so the identity a processor answered with depended on when the
    // processors around it happened to run.
    //
    // Everything on the exit path answers "which processor am I" with
    // this field: record_exit, emulate_init_signal, apply_start_up. When
    // it disagreed with the slot, the consequences were silent and
    // various - one processor's exits credited to another's ring, a
    // start-up flag cleared at the wrong index, and a slot reporting
    // `virtualized 1, by_sipi 0, exits 0` while its processor was plainly
    // running.
    vmcs.vpid(cpu + 1);

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
    if (cpu < max_cpus) {
        this->guest_in_vmx_operation[cpu] = false;
        this->guest_vmxon_pointer[cpu] = 0;
        this->guest_current_vmcs[cpu] = nested_vmx::no_current_vmcs;
        // Constructed rather than passed through.
        //
        // Passing the hardware register on hands the guest whatever else
        // the platform happened to set - the inside-SMX permission, the
        // SGX bits - none of which this VMM backs. What it must say is
        // exactly two things: locked, so the guest does not try to write
        // it, and VMXON permitted outside SMX, which is the only mode
        // offered. That pairs with concealing the extension in CPUID.
        //
        // And it says nothing about VMX at all where nesting is compiled
        // out. Granting VMXON while CPUID reports no VMX and CR4.VMXE
        // reads back clear is the same inconsistency BACKLOG.md item 1
        // records one register over, and a guest that trusts this register
        // over CPUID faults on its own vmxon.
        constexpr std::uint64_t feature_control_lock = 1ull << 0;
        constexpr std::uint64_t feature_control_vmxon_outside_smx = 1ull
                                                                    << 2;

        this->guest_feature_control[cpu] =
            feature_control_lock |
            (nested_vmx::enabled ? feature_control_vmxon_outside_smx
                                 : std::uint64_t{});

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

            if (arch::x86_64::vmx::vmclear(&this->vmcs02_physical[cpu])) {
                log("cpu {} could not clear its second level vmcs", cpu);
            }

            // The region's contents are now nothing, so neither elision
            // that believes something about them may keep believing it.
            forget_vmcs02_contents(cpu);

            // The shadow region, which the guest hypervisor's own VMREADs
            // and VMWRITEs are served from once a VMCS of its own is
            // current. Bit 31 of the revision identifier is what makes it
            // a *shadow* VMCS rather than an ordinary one - SDM 26.11.5,
            // "VMCS Types: Ordinary and Shadow" - and a region without it
            // is rejected by the VM-entry check on the link pointer.
            auto & shadow = this->shadow_vmcs[cpu];

            constexpr std::uint32_t shadow_indicator = 1u << 31;
            shadow.revision_id =
                static_cast<std::uint32_t>(
                    this->cached_vmx_msr(vmx_msr::basic) & 0xffffffff) |
                shadow_indicator;
            shadow.abort_indicator = 0;

            this->shadow_vmcs_physical[cpu] =
                this->host_page_table.virtual_to_physical(&shadow);

            if (arch::x86_64::vmx::vmclear(
                    &this->shadow_vmcs_physical[cpu])) {
                log("cpu {} could not clear its shadow vmcs", cpu);
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

    // The VMREAD and VMWRITE bitmaps are shared for the same reason, and
    // are written here on every processor while their contents are built
    // once. The control that consults them stays *off* until the guest
    // hypervisor makes a VMCS of its own current, because VM entry
    // requires a valid shadow region behind the link pointer whenever it
    // is on - see set_vmcs_shadowing.
    if constexpr (nested_vmx::enabled) {
        initialize_vmcs_shadowing();

        // Only when the feature is on. With it off these fields are never
        // consulted - the control that reads them is never set - but
        // leaving them unwritten is what makes "off" mean the VMCS is
        // programmed exactly as it was before shadowing existed, which is
        // the property a bare-metal bisection depends on.
        if constexpr (nested_vmx::shadow_vmcs_enabled) {
            vmcs.vmread_bitmap_address(
                this->vmcs_shadow_read_bitmap_physical);
            vmcs.vmwrite_bitmap_address(
                this->vmcs_shadow_write_bitmap_physical);
        }
    }

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
                    mode_based_execute_control |
                // The secondary half of the guest-test intercepts. See
                // `trap_the_quiet_instructions` below, which is declared
                // after this field is written and is therefore spelled
                // out again rather than shared - the alternative was to
                // move the whole argument above the secondary controls,
                // which would put it a hundred lines from the primary
                // controls it is mostly about.
                (ZPP_GUEST_TESTS
                     ? (arch::x86_64::vmx::vm_execution_controls::
                            secondary::wbinvd_exiting |
                        arch::x86_64::vmx::vm_execution_controls::
                            secondary::rdrand_exiting |
                        arch::x86_64::vmx::vm_execution_controls::
                            secondary::rdseed_exiting)
                     : std::uint64_t{})));

    // Nothing requested: external interrupts and NMIs stay the guest's,
    // which owns the interrupt controller.
    // NMI exiting, so that a non-maskable interrupt this VMM sends takes
    // the target processor out of whatever it is doing - including a halt,
    // which nothing else available here can do. The cost is that a genuine
    // NMI from the guest's world arrives here too and has to be handed
    // back; see the exception_or_nmi case in the exit handler.
#ifndef ZPP_VIRTUALIZE_APIC
#define ZPP_VIRTUALIZE_APIC 0
#endif
    constexpr bool virtualize_apic = (0 != ZPP_VIRTUALIZE_APIC);

    // Sampling the *first*-level guest, which is otherwise unobservable
    // once it stops exiting.
    //
    // **The application processors burn 100% of a host core each and take
    // no exits at all**, measured from the host's own scheduler accounting
    // (`/proc/<pid>/task/*/schedstat`) with `signal_exits` proving nothing
    // kicked them. Every instrument in this tree is driven by an exit, so
    // a first-level guest that spins without exiting is invisible to all
    // of them - and reading its instruction pointer through the monitor is
    // not passive, because QEMU must signal the processor out of guest
    // mode to do it, which perturbs exactly the state being asked about.
    //
    // `arm_controller_poll` already arms this timer and cannot help here:
    // it runs from the exit path, and these processors do not reach one.
    // Arming it at VMCS setup is what breaks that chicken-and-egg.
    //
    // Nothing else is needed - the exit ring already records the guest RIP
    // of every exit, so a periodic exit *is* the sample.
    //
    // Off by default: it charges every processor an exit per period for
    // ever, and this VMM's whole difficulty is exits.
#ifndef ZPP_SAMPLE_L1
#define ZPP_SAMPLE_L1 0
#endif
    constexpr bool sample_l1 = (0 != ZPP_SAMPLE_L1);

    vmcs.pin_based_vm_execution_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_pin_based_controls),
        arch::x86_64::vmx::vm_execution_controls::pin::nmi_exiting |
            (sample_l1 ? arch::x86_64::vmx::vm_execution_controls::pin::
                             activate_preemption_timer
                       : 0) |
            (virtualize_apic ? arch::x86_64::vmx::vm_execution_controls::
                                   pin::external_interrupt_exiting
                             : 0)));

    if constexpr (sample_l1) {
        // The timer counts the time-stamp counter shifted right by
        // IA32_VMX_MISC[4:0], so the same interval is a different number
        // on every machine. A millisecond is far longer than anything
        // being chased and still gives a thousand samples a second.
        //
        // Written unconditionally on every entry is not needed here: the
        // exit controls leave "save VMX-preemption timer value" clear, so
        // this one write keeps producing exits at the same interval -
        // SDM 26.6.4, and `build_vmcs02` relies on the same property.
        auto divisor = this->cached_vmx_msr(vmx_msr::misc) & 0x1f;
        auto ticks = (1000ull * 1800) >> divisor;
        vmcs.vmx_preemption_timer_value(ticks ? ticks : 1);
    }

    // Trapping MONITOR and MWAIT, which is **off**, and the reason is a
    // measurement rather than a preference.
    //
    // Neither is emulated: the handler logs the first one per processor
    // and resumes. So the only thing the two controls bought was that
    // first line - and they were charging two VM exits for every
    // iteration of every idle loop in the guest, for ever.
    //
    // What that costs is not marginal. A UEFI firmware parks its
    // application processors in `monitor; mwait; jmp`, and this VMM now
    // adopts those processors, so all seven of them sat in that loop
    // taking exits. Measured on the rig: exit reasons 36 and 39
    // alternating in every processor's exit ring, 1,190,000 exits each in
    // under two minutes, against 1,000,000 on the boot processor that was
    // actually trying to boot Windows - and a guest hypervisor above
    // managing 11 second-level entries in 25 seconds while that went on.
    //
    // Not trapping them also lets MWAIT do what the guest asked: idle the
    // processor. A processor idling that way is still reachable, because
    // the one thing this VMM needs to wake it with is an NMI and an NMI
    // ends MWAIT - see send_wake_nmi, which exists for the harder case of
    // a halted processor.
    //
    // Turn it on to ask questions about a guest's idle path - whether it
    // reaches MWAIT, on which processor, and whether its monitor arms -
    // and turn it off again afterwards.
    //
    // On under ZPP_GUEST_TESTS, and that is the only sound way to test
    // the two cases. The exit handler has had a `monitor`/`mwait` case
    // since before the controls were turned off, and with them off it is
    // code that cannot execute - so it is neither exercised by any test
    // nor removed. Turning the controls on in the test build makes the
    // two reasons reachable and the case live, and costs a deployed
    // build nothing, because a hypervisor compiled with this switch is
    // only ever embedded in a loader check-bootable.sh refuses.
    //
    // It does not make the measurement above wrong: those exits are still
    // two per idle-loop iteration, and the coverage suite executes each
    // instruction a handful of times rather than parking in one.
    constexpr bool trap_monitor_and_mwait = ZPP_GUEST_TESTS;

    auto monitor_controls =
        trap_monitor_and_mwait
            ? (arch::x86_64::vmx::vm_execution_controls::primary::
                   mwait_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   monitor_exiting)
            : std::uint64_t{};

    // The rest of the family, on the same switch and for the same reason
    // spelled out above MONITOR and MWAIT.
    //
    // Six primary controls and three secondary ones, each intercepting an
    // instruction a deployed build lets run. Every one of their exit
    // reasons was recorded in the coverage suite's table as
    // "unreachable-here: the control is off", which is true and is not a
    // closed question - the control being off is a decision this file
    // makes, not a property of the environment. And each was worse than
    // untested: an exit reason with no case reaches `default:`, which
    // stops the processor, so the handler cases for these were code that
    // could not run and could not be removed.
    //
    // Turning them on here makes ten exit reasons reachable - 12, 14, 15,
    // 16, 29, 40, 51, 54, 57 and 61 - and makes their handler cases live.
    // Each of those cases emulates the instruction rather than skipping
    // it; see exit_dispatch.cpp, where the cost of resuming as though an
    // unhandled instruction had succeeded is the recurring lesson.
    //
    // Costs a deployed build nothing: a hypervisor compiled with this
    // switch is only ever embedded in a loader check-bootable.sh refuses.
    // The cost to the *test* build is real and bounded - PAUSE exiting in
    // particular turns every spin loop into a stream of exits - which is
    // why this is one switch with the suite rather than a default.
    constexpr bool trap_the_quiet_instructions = ZPP_GUEST_TESTS;

    auto quiet_primary_controls =
        trap_the_quiet_instructions
            ? (arch::x86_64::vmx::vm_execution_controls::primary::
                   hlt_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   invlpg_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   rdpmc_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   rdtsc_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   mov_dr_exiting |
               arch::x86_64::vmx::vm_execution_controls::primary::
                   pause_exiting)
            : std::uint64_t{};

    // The one control here that changes a value rather than
    // intercepting an instruction. Requested only where this build
    // dilates time, so a default build asks the processor for exactly
    // what it always did - and the field it enables is zero until
    // `apply_time_dilation` moves it, so even here nothing changes
    // until the first entry.
    auto dilation_controls =
        nested_vmx::dilate_time
            ? arch::x86_64::vmx::vm_execution_controls::primary::
                  use_tsc_offsetting
            : std::uint64_t{};

    vmcs.primary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_processor_based_controls),
            arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_secondary_controls |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_msr_bitmaps |
                arch::x86_64::vmx::vm_execution_controls::primary::
                    enable_io_bitmaps |
                monitor_controls | quiet_primary_controls |
                dilation_controls));

    // The host runs in 64-bit mode after an exit, and DR7 and
    // IA32_DEBUGCTL are saved on the way out so the guest gets back what
    // it had rather than what the host was using.
    vmcs.vm_exit_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_exit_controls),
        arch::x86_64::vmx::vm_exit_controls::host_address_space_size |
            arch::x86_64::vmx::vm_exit_controls::save_debug_controls |
            // Without this the exit reports no vector and leaves the
            // interrupt pending at the controller, so there is nothing
            // to inject. SDM 30.2.
            (virtualize_apic ? arch::x86_64::vmx::vm_exit_controls::
                                   acknowledge_interrupt_on_exit
                             : 0)));

    // The mirror on entry. apply_start_up clears ia_32e_mode_guest
    // again, since it has to agree with CR0.PG or entry fails.
    vmcs.vm_entry_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_entry_controls),
        arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest |
            arch::x86_64::vmx::vm_entry_controls::load_debug_controls));

    // The selectors below are resolved against the intermediate GDT
    // rather than the OS one, which is not mapped here any more.
    auto intermediate_gdt_base = reinterpret_cast<std::uint64_t>(
        this->unprotected_memory.intermediate_gdt[cpu]);

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

    // Its own row, and its own index written into it, so that host code
    // can name the processor it is on with a single load through GS
    // instead of a VMREAD of the VPID. See `gs_data`. Bounded because
    // `cpu` indexes a fixed array and this is the only writer; a
    // processor outside the range gets the shared behaviour it had
    // before rather than a write past the end.
    if (cpu < max_cpus) {
        auto * row = this->gs_data[cpu];
        *reinterpret_cast<std::uint64_t *>(row +
                                           host_gs_processor_index) = cpu;
        vmcs.host_gs_base(reinterpret_cast<std::uint64_t>(row));
    } else {
        vmcs.host_gs_base(
            reinterpret_cast<std::uint64_t>(this->gs_data[0]));
    }

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

    // NE is owned by this VMM, and it is the bit a guest cannot be
    // allowed to reach.
    //
    // IA32_VMX_CR0_FIXED0 requires CR0.NE set in VMX operation, and
    // unrestricted guest exempts only PE and PG from the fixed bits. SDM
    // 28.1.3, "MOV to CR0": an execution that does not cause a VM exit
    // "leaves unmodified any bit in CR0 corresponding to a bit set in the
    // CR0 guest/host mask", and with unrestricted guest set it "causes a
    // general-protection exception if it attempts to set any bit in CR0
    // other than bit 0 (PE) or bit 31 (PG) to a value not supported in VMX
    // operation".
    //
    // So with this mask at zero - which it was - a guest writing a CR0
    // with NE clear takes a #GP that no processor outside VMX would have
    // given it. That is not hypothetical: Hyper-V's application processor
    // trampoline does exactly that. Disassembled off the rig at guest
    // physical 0x2000, the whole of its transition to protected mode is
    //
    //     mov  eax, 1
    //     mov  cr0, eax
    //     mov  ax, 0x20
    //     mov  ds, ax
    //     jmp  far [edi+0x74]
    //
    // - a literal 1, so PE set and every other bit cleared, NE among
    // them. The processor took a #GP with the real mode interrupt vector
    // table still loaded, vectored through a zero entry, and executed
    // zero-filled memory from there: measured, the processor's guest RIP
    // crawled from 0x53af to 0x6409 over four minutes with the same CS.
    // It therefore never reached its `mov dword [ds:0x98], 1` alive flag,
    // so Hyper-V INIT-ed it, gave up, and its boot processor waited for an
    // application processor that could not start.
    //
    // With NE in the mask the bit is simply left unmodified and no fault
    // occurs, whether or not the write exits. KVM host-owns it the same
    // way: `vmx_l1_guest_owned_cr0_bits` (vmx-internal.h:636) is a small
    // allow list and `vmcs_writel(CR0_GUEST_HOST_MASK, ~...owned_bits)`
    // (vmx.c:4808) makes everything else the host's, while
    // `KVM_VM_CR0_ALWAYS_ON_UNRESTRICTED_GUEST` (vmx.c:151) is
    // X86_CR0_NE exactly.
    //
    // The mask is written all the same when it is zero, and that is not
    // tidiness either. A VMCS
    // field that has never been written has no defined value, and
    // build_vmcs02 composes the second-level mask as
    // `cr0_mask01 | cr0_mask12` - so leaving this one unwritten ORs
    // whatever the region happened to hold into a mask that decides which
    // of a second-level guest's CR0 writes exit. Bits nobody owns then
    // produce exits neither this VMM nor the guest hypervisor will claim:
    // the reflection test finds the guest hypervisor does not own them,
    // the handler here accepts only CR4, and the processor stops.
    // Measured, on a Windows application processor coming up under
    // Hyper-V: `unhandled exit reason 0x1c qualification 0xe00`, which is
    // MOV to CR0 from R14.
    vmcs.cr0_guest_host_mask(arch::x86_64::cr0_bits::numeric_error);
    vmcs.cr0_read_shadow(this->guest_cr0);
    vmcs.guest_cr0(this->host_cr0);

    // The host half takes write protection with it and the guest half
    // above deliberately does not. Every VM exit lands on the host page
    // table, where this module's text and read-only data are mapped
    // without the write flag, and that flag is only consulted for a
    // supervisor write while CR0.WP is set.
    vmcs.host_cr0(this->host_control_register_0());

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
    //
    // SMXE is here for the same reason and with the opposite sign, and it
    // is the half that was missing. `073bc83` concealed safer mode
    // extensions from the guest by clearing CPUID leaf 1 ECX[6], and
    // stopped there - so on a processor that implements SMX the guest
    // could set CR4.SMXE against a CPUID saying the feature does not
    // exist. Concealing a feature in CPUID is not the same as making it
    // unreachable.
    //
    // What it cost: SDM 28.1.2 (.references/sdm.txt:200727) says "An
    // execution of GETSEC in VMX non-root operation causes a VM exit if
    // CR4.SMXE[Bit 14] = 1 regardless of the value of CPL or RAX", and
    // the exit handler had no case for reason 11 - so the exit reached
    // `default:` and stopped the processor. Two instructions, one of them
    // a MOV, and a guest could halt a physical CPU.
    //
    // The bit is therefore masked, answered clear in the read shadow, and
    // - unlike VMXE - kept out of the *real* register as well, since
    // nothing here needs it. With CR4.SMXE clear GETSEC raises #UD in
    // hardware and takes no exit at all, which is both the architecture's
    // answer and the cheapest one. The case added beside this is what
    // catches the day that stops being true.
    constexpr std::uint64_t cr4_vmxe = 1ull << 13;
    constexpr std::uint64_t cr4_smxe = 1ull << 14;
    vmcs.cr4_guest_host_mask(cr4_vmxe | cr4_smxe);
    vmcs.cr4_read_shadow(this->guest_cr4 & ~(cr4_vmxe | cr4_smxe));
    vmcs.guest_cr4(this->host_cr4 & ~cr4_smxe);
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

    // The stack every VM exit on this processor runs on.
    //
    // Thirty two kilobytes rather than the 0x1500 it was, and the number
    // is no longer a guess. Two contexts sit at the top, 1856 bytes of the
    // total, and what has to fit below them is the deepest exit handler
    // path - which is now the eager shadow extended page-table build, and
    // that alone wants sixteen kilobytes: `build_shadow_ept` descends four
    // levels with a whole 4096-byte table read into a local at each. On
    // the old size the first shadow build would have run off the bottom of
    // this array and into whatever `vm_launch`'s frame had below it,
    // silently, and only with nested VMX switched on - which is the worst
    // shape a bug can have here.
    //
    // Costing nothing is what makes the margin the right answer rather
    // than a tight fit: this is a local of a function that never returns,
    // on the 512 KB per-processor stack `launch_on_cpu` already reserved,
    // so the whole of it comes out of memory that was allocated and
    // unused.
    alignas(0x10) unsigned char host_vm_launch_stack[0x8000]{};

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

        // The firmware's framebuffer, recorded and never read from
        // here - see the members' comment. Inside this guard with the
        // rest, because a processor this VMM started carries no launch
        // block and would otherwise zero what the boot processor found.
        this->framebuffer_base = launch.framebuffer.base;
        this->framebuffer_size = launch.framebuffer.size;
        this->framebuffer_width = launch.framebuffer.horizontal_resolution;
        this->framebuffer_height = launch.framebuffer.vertical_resolution;
        this->framebuffer_stride = launch.framebuffer.pixels_per_scan_line;
        this->framebuffer_format = launch.framebuffer.pixel_format;
        this->framebuffer_red_mask = launch.framebuffer.red_mask;
        this->framebuffer_green_mask = launch.framebuffer.green_mask;
        this->framebuffer_blue_mask = launch.framebuffer.blue_mask;
        this->framebuffer_reserved_mask = launch.framebuffer.reserved_mask;

        // In the log too, not only in the members. The members need a
        // reader that knows the singleton's offsets; the log is text and
        // comes out of a wedged guest through the same path everything
        // else does. Both, because the two are read in different
        // situations and neither subsumes the other.
        log("framebuffer at {} size {}, {}x{} stride {} format {}",
            this->framebuffer_base,
            this->framebuffer_size,
            this->framebuffer_width,
            this->framebuffer_height,
            this->framebuffer_stride,
            this->framebuffer_format);

        // Where this module was put. Inside the guard with the rest,
        // because a processor this VMM started has no launch block and
        // would otherwise write a null over the answer the boot
        // processor already found - which is exactly how the sleep
        // control port was lost once.
        this->handed_over_module_base = launch.module_base;

        // Copied, not pointed at. The array is the loader's and this
        // module outlives it - the same reason every other field here is
        // copied rather than referenced.
        this->number_of_platform_processors = 0;
        if (launch.processor_apic_ids) {
            auto count = launch.number_of_processor_apic_ids;
            if (count > max_cpus) {
                count = max_cpus;
            }

            for (std::size_t i{}; i < count; ++i) {
                this->platform_apic_id[i] = launch.processor_apic_ids[i];
            }

            this->number_of_platform_processors = count;
        }
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

        // Start intercepting IA32_APIC_BASE, before any other processor is
        // launched. The MSR bitmap is shared by every VMCS, so this is
        // done once.
        //
        // Both halves, and reads as well as writes. It is inside the low
        // range the bitmap covers, so with the bitmap left at zero it did
        // not exit at all: a guest write reached the physical register,
        // moved the local APIC's mode or its page out from under this VMM,
        // and nothing here noticed. Whatever was armed at launch then
        // stayed armed and was describing a machine that no longer
        // existed.
        //
        // The read is intercepted too, even though the value handed back
        // is the register's own. Reading it is what a guest does
        // immediately before writing it, and a read that exits is the only
        // cheap evidence of a guest that is about to change modes.
        intercept_msr(arch::x86_64::msr::ia32_apic_base, true, true);
        // The interrupt command register's own interception is no longer
        // armed unconditionally here. It is derived from the mode instead,
        // which is what note_apic_mode does - and it cannot run yet,
        // because arming the other half of the same decision edits an
        // extended page table entry and there is no table to edit until
        // initialize_ept.

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
        initialize_intermediate_gdt(cpuid);
        load_intermediate_gdt(cpuid);
    }

    // Guard to restore GDT, on the processors that had one to replace.
    scope_exit restore_gdt{[&] {
        if (!from_trampoline) {
            load_os_gdt();
        }
    }};

    // The host page table sets the execute disable bit - on its own
    // pages, on the local APIC page, and since this module is protected
    // per segment on everything that is not text. That bit is *reserved*
    // while IA32_EFER.NXE is clear (SDM 5.5.4,
    // .references/sdm.txt:157079), and a reserved bit set makes the entry
    // fault on any access through it, not merely on an instruction fetch.
    //
    // So this is not a check that a hardening feature is available, it is
    // a check that the next instruction has somewhere to be fetched from.
    // Checked rather than assumed, and checked here rather than at the
    // one place that could have set it: the boot processor inherits EFER
    // from whatever launched it, and a processor this VMM started set the
    // bit itself in ap_start_up.S - so there is no single owner to ask,
    // only the register.
    //
    // Not set here either. Writing EFER on this path would leave the bit
    // set in the guest as well, since no VM-entry control loads EFER and
    // the register simply carries over - which silently turns the guest's
    // own reserved bit into a meaningful one. Refusing to launch is the
    // honest answer; BACKLOG.md records what separating the two would
    // take.
    constexpr std::uint64_t execute_disable_enable = 1ull << 11;
    if (!(arch::x86_64::rdmsr(
              arch::x86_64::msr::ia32_extended_feature_enable) &
          execute_disable_enable)) {
        return std::unexpected(
            zpp::error{error::execute_disable_not_enabled});
    }

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

    // And write protection, in the same breath as the table it makes mean
    // something. The host page table denies writes to this module's text
    // and read-only data, and SDM 5.6.1 only consults those denials for a
    // supervisor write while CR0.WP is set.
    //
    // Here as well as in host_control_register_0, because that value does
    // not exist yet: host_cr0 is derived in initialize_vmx, which runs
    // several hundred lines below this. Between the two the processor is
    // already on the host page table, running this VMM's own code, with
    // whatever CR0 the platform left behind - and what it leaves is not
    // ours to assume. OVMF sets the bit; the probe build's page fault
    // (0x60e03) was taken under it, so on that firmware this line changes
    // nothing and on one without it this line is the whole protection.
    //
    // The bit is set rather than the register written, since everything
    // else in CR0 belongs to whatever launched this and is restored
    // below on the same terms CR3 is.
    auto entry_cr0 = arch::x86_64::cr0();
    arch::x86_64::cr0(entry_cr0 | arch::x86_64::cr0_bits::write_protect);

    scope_exit restore_cr0{[&] {
        if (!from_trampoline) {
            arch::x86_64::cr0(entry_cr0);
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

#if ZPP_TEST_MODULE_PROTECTION
    // Prove the module protection rather than believe it.
    //
    // A tightening nobody has watched fault is a tightening that may not
    // have been applied: every step of it - the ELF permissions, the
    // union, CR0.WP, EFER.NXE - fails silently and in the safe-looking
    // direction, leaving a page table that reads correctly in a debugger
    // and denies nothing.
    //
    // So this stores a byte of .text back over itself. The byte is read
    // first and written back unchanged, so the probe destroys nothing
    // whichever way it goes - it is the *store* that is being tested, not
    // what it would have written.
    //
    // Only the fall-through needs code. If the store faults, the host IDT
    // unwinds to the recovery point armed just above, `host_exception`
    // holds the frame, and the block immediately below already turns that
    // into the error code the loader prints: 0x60e03 - vector 14, error
    // code 3, which is a present page written by a supervisor. That is
    // the expected result of this build and the whole verdict.
    //
    // Reaching the line after the store is the failure. It means a write
    // to this module's own code was allowed, and the launch is refused
    // with its own code rather than continuing, because a machine that
    // boots is exactly how this would be missed.
    //
    // Boot processor only: the answer is a property of one page table
    // that every processor shares, and a second opinion costs a boot.
    if (first_launch && !host_exception_occurred) {
        log("module protection probe: storing to text");

        auto probe = reinterpret_cast<volatile unsigned char *>(
            const_cast<unsigned char *>(
                arch::x86_64::zpp_ap_start_up_begin));

        auto value = *probe;
        *probe = value;

        log("module protection probe: the store to text was allowed");
        return std::unexpected(
            zpp::error{error::module_protection_missing});
    }
#endif

    // Arriving from an exception rather than from the capture. The details
    // are in host_exception and host_exception_cr2 for a debugger to read;
    // the caller only learns that a host exception happened.
    if (host_exception_occurred) {
        // The vector travels in the code, because on the target there is
        // nowhere else for it to go.
        //
        // host_exception and host_exception_cr2 hold the whole frame and
        // a debugger can read them - on the rig. On bare metal there is
        // no debugger, no serial port and no resident channel yet, and
        // the loader's printed code is the only thing that leaves the
        // machine. "code 6" says a fault happened in this VMM's own
        // setup and nothing about which, and that is one boot spent to
        // learn almost nothing.
        //
        // So the code becomes 0x60000 | vector << 8 | error code, which
        // the loader prints as sixteen hex digits: 0x60d00 is a general
        // protection fault, 0x60e00 a page fault, 0x60600 an invalid
        // opcode. The low byte carries the exception's own error code,
        // which for a #GP names the selector when it has one.
        //
        // Still `host_exception` in spirit and still positive, so it
        // stays distinguishable from the loader's own -1.
        constexpr std::uint64_t host_exception_tag = 0x60000;
        return std::unexpected(zpp::error{static_cast<error>(
            host_exception_tag |
            ((this->host_exception.vector & 0xff) << 8) |
            (this->host_exception.error_code & 0xff))});
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

                        // Holding mode rather than notify, so that this
                        // and the rebuild ask for the same thing on the
                        // same page: re-arming replaces rather than
                        // duplicating, and two callers disagreeing about
                        // the mode would leave whichever armed last in
                        // charge. A hold nobody holds behaves exactly
                        // like a notify.
                        if (auto watching = watch_guest_page_writes(
                                doorbell_page,
                                &hypervisor::on_doorbell_write,
                                this,
                                page_watch::mode::hold);
                            !watching) {
                            log("could not watch the doorbell page");
                        } else {
                            this->channel_doorbell_watched = true;
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

    initialize_vmx(cpuid);

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

    if (auto result = enter_root_mode(cpuid); !result) {
        return result;
    }

    // The guard enter_root_mode released, taken up again here: a failure
    // between now and the launch still has to leave VMX operation.
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    // Interception of the interrupt command, in whichever of its two forms
    // the machine's local APICs are currently using: an MSR write in
    // x2APIC mode, which the bitmap catches, or a store to the APIC page
    // in xAPIC mode, which only a page watch can.
    //
    // Past initialize_ept and unprotect_guest_memory, because arming a
    // watch edits an extended page table entry: before the first there is
    // no table to edit, and between the two the entry is written and then
    // overwritten and the watch is silently lost. And past enter_root_mode
    // as well, which the arming used to precede - INVEPT is defined only
    // in VMX root operation and invalidate_ept_locally correctly does
    // nothing outside it, so a watch armed before this point was one whose
    // invalidation never happened.
    //
    // On *every* launch and not only the boot processor's, which is the
    // smaller half of the fix. The larger half is that this now also runs
    // from the IA32_APIC_BASE write handler, so a mode change during the
    // boot is followed rather than missed. It used to run exactly once,
    // and the mode was therefore whatever it happened to be at launch for
    // the rest of the boot - so a guest that moved to x2APIC afterwards,
    // which is what an operating system does within milliseconds of
    // starting, left both halves of this describing a machine that had
    // stopped existing.
    note_apic_mode(cpuid);

    setup_vmcs(cpuid, caller_context);

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
        apply_start_up(caller_context,
                       this->guest_start_up_vector[cpuid],
                       "launch",
                       true);

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

    // This processor's own VPID, read from its own VMCS, rather than the
    // counter it was taken from.
    //
    // The two are equal here only because the increment happens later,
    // inside vm_launch, and nothing else has claimed a number in between
    // - which is a property of the *other* processors' timing, not of
    // this one's state. A shared mutable counter is not an identity and
    // reading one as though it were is what makes a misattributed
    // processor impossible to see.
    //
    // This sentence used to end "everything else on the exit path already
    // answers 'which processor am I' with vmcs.vpid()", and it no longer
    // does: the exit path threads `cpuid` instead, because a VMREAD is an
    // exit to the layer below whenever this VMM is itself a guest. The
    // field is still the truth about *this* line, which runs once per
    // processor at launch and is the one place the counter and the VMCS
    // can be compared at all.
    log("launching guest on virtual processor {}", vmcs.vpid());

    // The exit dispatch itself is `on_vm_exit`, in exit_dispatch.cpp.
    //
    // It was this lambda's body, seventeen hundred lines of it, and the
    // capture list is why the move was safe: changing `[&]` to `[this]`
    // named `cpuid` as the only thing it took from `main`'s scope, so it
    // is the only thing that had to become a parameter. Nothing else in
    // the body changed.
    vm_launch(caller_context, [this, cpuid](auto & context) {
        on_vm_exit(cpuid, context);
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
