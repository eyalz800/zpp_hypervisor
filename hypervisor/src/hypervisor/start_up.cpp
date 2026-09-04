// Starting and adopting a logical processor: the decision, not the
// scaffolding.
//
// The whole of what this VMM *decides* about the other processors on the
// machine: give one a slot, work out what a guest's write to the
// interrupt command register means, resolve a broadcast against the
// platform's roster, hand a start-up vector over in software or send a
// real start-up IPI, take a processor into VMX root mode, and apply the
// INIT and start-up states SDM Table 12-1 describes.
//
// Most of this was already contiguous in hypervisor.cpp - `processor_slot`
// through `apply_start_up` in file order - which is itself evidence that
// it is one subject. Three functions elsewhere come with it because they
// are part of the same decision and have no other user:
// `send_start_up_ipi`, `emulate_init_signal` and `local_apic_id`;
// `enter_root_mode` comes because starting a processor is one of its two
// callers and it is the step that makes a started processor ours.
//
// **What deliberately stayed behind, and why.** Three functions from the
// middle of that contiguous run are not here:
// `initialize_start_up_memory`, `start_up_trampoline_stage` and
// `start_up_on_this_processor`. They are the trampoline page and the
// entry point a woken processor lands on - building the machine an
// application processor wakes into, which is a different subject from
// deciding whether to wake one. The practical consequence is the reason
// it is worth saying: they reach the host GDT, the host IDT, the host
// CR3, the OS page table and `launch_on_cpu`, none of which anything else
// in this file touches, and taking them along would mean a harness had to
// model the whole launch to test a slot allocator.
//
// Why move anything at all: hypervisor.cpp is ten thousand lines and
// reaches the whole VMM, so nothing can compile it, and tests/ap_start_up
// and tests/local_apic therefore cut these bodies out with awk at build
// time. A translation unit a harness can compile against a small stand-in
// header is what makes that unnecessary. Same reason as b6f0bee.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/segment_descriptor.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/msr.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/scope_exit.h"
#include <cstdint>
#include <optional>
#include <utility>

namespace zpp::hypervisor
{
std::expected<void, zpp::error>
hypervisor::enter_root_mode(std::size_t cpu)
{
    // This processor's own regions, as locals, and that is a fix rather
    // than a tidy-up.
    //
    // They used to be two members `initialize_vmx` wrote and this
    // function read back. Everything else about a launch is indexed by
    // the slot; these two were a hand-off through shared state, and the
    // window between the write and the VMXON below is not protected by
    // anything on this processor - `start_up_lock` is held by the
    // *starter*, and it is released on its own timeout as well as on
    // success. A second processor entering `initialize_vmx` inside that
    // window overwrote both, and this one then executed VMPTRLD on the
    // other's VMCS. SDM 25.1 and the comment in `initialize_vmx`: a VMCS
    // may not be active on more than one logical processor.
    //
    // Derived here rather than passed in, so there is no way to call this
    // with a slot that disagrees with the regions - the same shape
    // `own_vmxon_region_physical` and `own_vmcs_region_physical` already
    // have for the sleep path. They cannot be reused: both read the slot
    // out of `vmcs.vpid()`, and no VMCS is current yet.
    //
    // Addressable because VMXON, VMCLEAR and VMPTRLD take the address of
    // a physical address rather than the address itself.
    if (cpu >= max_cpus) {
        return std::unexpected(zpp::error{error::too_many_processors});
    }

    auto vmx_physical =
        this->host_page_table.virtual_to_physical(&this->vmx[cpu]);
    auto vmcs_physical =
        this->host_page_table.virtual_to_physical(&this->vmx_vmcs[cpu]);

    // Into the state VMX requires, each behind a guard: every step below
    // can fail, and a failure has to leave the loader the machine it was
    // still running on.
    auto cr0 = arch::x86_64::cr0();
    auto cr4 = arch::x86_64::cr4();

    // Write protected, because from here until the guest is entered this
    // processor is running this VMM's own code on this VMM's own page
    // table, and that table denies writes to the module's text and
    // read-only data. Without CR0.WP those denials mean nothing at
    // privilege zero - see host_control_register_0.
    arch::x86_64::cr0(this->host_control_register_0());
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

    if (arch::x86_64::vmx::vmxon(&vmx_physical)) {
        return std::unexpected(zpp::error{error::vmxon_failed});
    }
    scope_exit turn_off_vmx{arch::x86_64::vmx::vmxoff};

    // VMCLEAR is the only thing that sets the launch state to clear, and
    // VMLAUNCH requires clear (SDM 27.1). The state lives in the region
    // itself and cannot be read back, so it has to be set here.
    if (arch::x86_64::vmx::vmclear(&vmcs_physical)) {
        return std::unexpected(zpp::error{error::vmclear_failed});
    }

    if (arch::x86_64::vmx::vmptrld(&vmcs_physical)) {
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

void hypervisor::trace_guest_state(std::size_t cpu, const char * where)
{
    // Compiled either way and reached only with the switch on. `return`
    // rather than wrapping the body, so the cost with it off is one
    // constant-folded branch and the body still has to compile - which is
    // what keeps a diagnostic from rotting while it is switched off.
    if constexpr (!nested_vmx::trace_ap_entry) {
        static_cast<void>(cpu);
        static_cast<void>(where);
        return;
    } else {
        auto & vmcs = this->vmcs;

        // One line per subject, because the ring truncates a long line
        // and a truncated field is indistinguishable from a zero one -
        // a trap this tree has already recorded once.
        //
        // The count first, and on every dump. A reader that finds a
        // triple-fault dump has to be told, in the same breath, whether
        // this instrument ever saw an application processor at all:
        // zero here is "no application processor was ever entered", which
        // is a different failure and not this one.
        log("zpp-state {} cpu {}: ap first entries traced {} - zero means "
            "no application processor was ever entered",
            where,
            cpu + 1,
            this->ap_entry_traces);

        log("zpp-state {} cpu {}: rip {} rsp {} rflags {}",
            where,
            cpu + 1,
            vmcs.guest_rip(),
            vmcs.guest_rsp(),
            vmcs.guest_rflags());

        // **The masks, because they decide whether we see anything at
        // all.** Measured: after this state is applied, cpu 1 takes
        // ZERO exits - the unconditional log at the top of
        // `on_vm_exit` catches none - while a hardware breakpoint
        // proves it executes `mov cr0` with PG|PE at trampoline offset
        // 0x216b. A CR0 write only exits if it changes a bit in the
        // guest/host mask, so if PG is absent from cpu 1's mask the
        // long-mode switch is invisible here and
        // `ia_32e_mode_guest` is never brought into agreement with
        // EFER.LMA. This prints the field rather than assuming it.
        log("zpp-state {} cpu {}: cr0 mask {} cr4 mask {}",
            where,
            cpu + 1,
            vmcs.cr0_guest_host_mask(),
            vmcs.cr4_guest_host_mask());

        log("zpp-state {} cpu {}: cr0 {} shadow {} cr3 {}",
            where,
            cpu + 1,
            vmcs.guest_cr0(),
            vmcs.cr0_read_shadow(),
            vmcs.guest_cr3());

        log("zpp-state {} cpu {}: cr4 {} shadow {} efer {} dr7 {}",
            where,
            cpu + 1,
            vmcs.guest_cr4(),
            vmcs.cr4_read_shadow(),
            vmcs.guest_ia32_efer(),
            vmcs.guest_dr7());

        // The three fields that decide whether the *next* entry can
        // happen at all, and the one that says which mode it will be in.
        // A guest state that looks perfect and an entry control that
        // disagrees with it is a VM-entry failure rather than anything
        // the segments below would explain.
        log("zpp-state {} cpu {}: activity {} interruptibility {} "
            "entry-controls {} pending-dbg {}",
            where,
            cpu + 1,
            vmcs.guest_activity_state(),
            vmcs.guest_interruptibility_state(),
            vmcs.vm_entry_controls(),
            vmcs.guest_pending_debug_exceptions());

        log("zpp-state {} cpu {}: cs {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_cs_selector(),
            vmcs.guest_cs_base(),
            vmcs.guest_cs_limit(),
            vmcs.guest_cs_access_rights());

        log("zpp-state {} cpu {}: ss {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_ss_selector(),
            vmcs.guest_ss_base(),
            vmcs.guest_ss_limit(),
            vmcs.guest_ss_access_rights());

        log("zpp-state {} cpu {}: ds {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_ds_selector(),
            vmcs.guest_ds_base(),
            vmcs.guest_ds_limit(),
            vmcs.guest_ds_access_rights());

        log("zpp-state {} cpu {}: es {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_es_selector(),
            vmcs.guest_es_base(),
            vmcs.guest_es_limit(),
            vmcs.guest_es_access_rights());

        log("zpp-state {} cpu {}: fs {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_fs_selector(),
            vmcs.guest_fs_base(),
            vmcs.guest_fs_limit(),
            vmcs.guest_fs_access_rights());

        log("zpp-state {} cpu {}: gs {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_gs_selector(),
            vmcs.guest_gs_base(),
            vmcs.guest_gs_limit(),
            vmcs.guest_gs_access_rights());

        log("zpp-state {} cpu {}: ldtr {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_ldtr_selector(),
            vmcs.guest_ldtr_base(),
            vmcs.guest_ldtr_limit(),
            vmcs.guest_ldtr_access_rights());

        log("zpp-state {} cpu {}: tr {} base {} limit {} ar {}",
            where,
            cpu + 1,
            vmcs.guest_tr_selector(),
            vmcs.guest_tr_base(),
            vmcs.guest_tr_limit(),
            vmcs.guest_tr_access_rights());

        log("zpp-state {} cpu {}: gdtr {}/{} idtr {}/{}",
            where,
            cpu + 1,
            vmcs.guest_gdtr_base(),
            vmcs.guest_gdtr_limit(),
            vmcs.guest_idtr_base(),
            vmcs.guest_idtr_limit());
    }
}

void hypervisor::reset_local_apic_after_init()
{
    // The xAPIC page offsets, which double as the x2APIC MSR numbers.
    // SDM Table 13-6 (`.references/sdm.txt:172145`) lists every x2APIC
    // MSR beside the MMIO offset of the same register, and the pairing
    // is `msr = 0x800 + (offset >> 4)` for all of them - "the MSR
    // address space is compressed", one MSR per 128-bit boundary.
    constexpr std::uint64_t version_register = 0x30;
    constexpr std::uint64_t task_priority = 0x80;
    constexpr std::uint64_t logical_destination = 0xd0;
    constexpr std::uint64_t destination_format = 0xe0;
    constexpr std::uint64_t spurious_vector = 0xf0;
    constexpr std::uint64_t error_status = 0x280;
    constexpr std::uint64_t lvt_cmci = 0x2f0;
    constexpr std::uint64_t lvt_timer = 0x320;
    constexpr std::uint64_t timer_initial_count = 0x380;
    constexpr std::uint64_t timer_divide = 0x3e0;
    constexpr std::uint32_t x2apic_msr_base = 0x800;

    // Which mode, asked of this processor now rather than assumed, and
    // an INIT is not allowed to change the answer underneath: SDM
    // 13.12.5 (`.references/sdm.txt:172339`) - "An INIT in this state
    // keeps the x2APIC in the x2APIC mode ... However, all the other
    // APIC registers are initialized as a result of the INIT transition"
    // - and the paragraph above it says an INIT taken in xAPIC mode
    // "places the APIC in the state with EN=1, EXTD=0".
    //
    // The two spellings are not interchangeable and picking the wrong
    // one is not a degradation: a WRMSR in 0x800-0x8ff while the APIC is
    // in xAPIC mode is #GP, in the host, where this VMM has no recovery
    // point. That is `2685265` one register over, and it is why
    // `send_start_up_ipi` branches on the same question.
    auto extended = x2apic_enabled();

    std::uint64_t base{};

    if (!extended) {
        // The same mask and the same guard as `watch_local_apic`. The
        // host page table maps exactly one local APIC page, read from
        // IA32_APIC_BASE before any guest ran, and a guest may relocate
        // its APIC by writing that MSR. Writing through an address this
        // table does not map is a #PF in root mode with nothing left to
        // unwind to, so a relocated APIC is left alone and said out loud
        // instead - the same trade `watch_local_apic` records at length.
        constexpr std::uint64_t base_mask = 0xffffff000ull;
        base = arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) &
               base_mask;

        if (base != this->mapped_apic_page) {
            log("cpu {} init: not resetting a relocated local apic at "
                "{}, this vmm maps {}",
                this->vmcs.vpid(),
                base,
                this->mapped_apic_page);
            return;
        }
    }

    auto read = [&](std::uint64_t offset) {
        if (extended) {
            return static_cast<std::uint32_t>(
                arch::x86_64::rdmsr(static_cast<std::uint32_t>(
                    x2apic_msr_base + (offset >> 4))));
        }
        return arch::x86_64::read32(
            reinterpret_cast<volatile std::uint8_t *>(base) + offset);
    };

    auto write = [&](std::uint64_t offset, std::uint32_t value) {
        if (extended) {
            arch::x86_64::wrmsr(static_cast<std::uint32_t>(
                                    x2apic_msr_base + (offset >> 4)),
                                value);
            return;
        }
        arch::x86_64::write32(
            reinterpret_cast<volatile std::uint8_t *>(base) + offset,
            value);
    };

    // Every local vector table entry masked. SDM 13.4.7.1
    // (`.references/sdm.txt:170707`): "The LVT register is reset to 0s
    // except for the mask bits; these are set to 1s", and the mask is
    // bit 16 - SDM 13.5.1, Figure 13-8, `.references/sdm.txt:170829`.
    // KVM writes the identical value over the identical set,
    // `.references/kvm/lapic.c:2753`.
    //
    // **How many there are is this processor's to say.** The version
    // register's "Max LVT Entry" field, bits 23:16, "shows the number of
    // LVT entries minus 1" - SDM 13.4.8, Figure 13-7,
    // `.references/sdm.txt:170761`. KVM asks the same question a
    // different way (`kvm_apic_calc_nr_lvt_entries`, lapic.c:579) and
    // for the same reason: an LVT this processor does not implement is a
    // reserved MSR in x2APIC mode, and a WRMSR to one is #GP.
    //
    // The offsets are not one run. Entries 0 to 5 are the timer,
    // thermal, performance, LINT0, LINT1 and error registers at 0x320
    // through 0x370; entry 6 is CMCI, which sits *below* them at 0x2f0.
    // SDM 13.5.1 lists all seven with their addresses.
    constexpr std::uint32_t lvt_masked = 1u << 16;
    constexpr std::uint32_t max_lvt_entry_shift = 16;
    constexpr std::uint32_t max_lvt_entry_mask = 0xff;
    constexpr std::uint32_t most_lvt_entries = 7;

    auto entries = ((read(version_register) >> max_lvt_entry_shift) &
                    max_lvt_entry_mask) +
                   1;

    if (entries > most_lvt_entries) {
        entries = most_lvt_entries;
    }

    for (std::uint32_t entry{}; entry < entries; ++entry) {
        write((most_lvt_entries - 1) == entry
                  ? lvt_cmci
                  : (lvt_timer + (0x10 * entry)),
              lvt_masked);
    }

    // The destination format register to all ones, which is flat model.
    // SDM 13.4.7.1: "The DFR register is reset to all 1s". KVM,
    // lapic.c:2761.
    //
    // **xAPIC only.** "The DFR, supported at offset 0E0H in xAPIC mode,
    // is not supported in x2APIC mode. There is no MSR with address
    // 80EH" - SDM 13.12.1.2, `.references/sdm.txt:172126` - and an
    // access to a reserved MSR in that range is a general-protection
    // exception.
    if (!extended) {
        write(destination_format, 0xffffffff);
    }

    // The spurious interrupt vector register to 0xff: vector 0xff with
    // bit 8, the APIC software enable, clear. SDM 13.4.7.1: "The
    // spurious-interrupt vector register is initialized to 000000FFH. By
    // setting bit 8 to 0, software disables the local APIC." KVM,
    // `apic_set_spiv(apic, 0xff)`, lapic.c:2762.
    //
    // This is the one write here with teeth, so what it does *not* stop
    // is worth stating. A software-disabled local APIC "will respond
    // normally to INIT, NMI, SMI, and SIPI messages" and "can still
    // issue IPIs" - SDM 13.4.7.2, `.references/sdm.txt:170723`. So the
    // start-up IPI this processor is about to be given still arrives,
    // the wake NMI `send_wake_nmi` uses for the extended-page-table
    // rendezvous still arrives, and a processor that has to send one
    // still can. A guest's own bring-up stub re-enables it, because on
    // real hardware after an INIT it has to.
    write(spurious_vector, 0xff);

    // The task priority to zero. SDM 13.4.7.1 lists TPR among the
    // registers "reset to all 0s"; KVM, lapic.c:2763.
    write(task_priority, 0);

    // The logical destination register to zero, **xAPIC only**: it is
    // read-only in x2APIC mode, where it is derived from the x2APIC ID -
    // SDM Table 13-6, "Read-only ... Read/write in xAPIC mode". KVM
    // makes the same exception on the same test, lapic.c:2764-2765.
    if (!extended) {
        write(logical_destination, 0);
    }

    // The error status register. Zero, and only zero: "WRMSR of a
    // non-zero value causes #GP(0)" in x2APIC mode - SDM Table 13-6 -
    // and the value is ignored in either mode anyway, because the write
    // is the operation: "this write clears any previously logged errors
    // and updates the ESR with any errors detected since the last write
    // to the ESR", SDM 13.5.3, `.references/sdm.txt:171022`. KVM,
    // lapic.c:2766.
    write(error_status, 0);

    // The timer's divide configuration and its initial count. SDM
    // 13.4.7.1 lists "the divide configuration register" and "timer
    // initial count and timer current count registers" among those reset
    // to zero; KVM, lapic.c:2773 and 2774. The second write is also what
    // stops a timer that is running: "a write of 0 to the initial-count
    // register effectively stops the local APIC timer, in both one-shot
    // and periodic mode" - SDM 13.5.4, `.references/sdm.txt:171052`.
    // Ordered after the LVT masking above, as KVM orders it.
    write(timer_divide, 0);
    write(timer_initial_count, 0);

    // ----------------------------------------------------------------
    // **What is deliberately not written, and why.**
    //
    // *The interrupt command register.* KVM zeroes it,
    // lapic.c:2768-2771, and KVM can, because `kvm_lapic_set_reg` writes
    // the register's *storage*. Here the register is a real one, and
    // "the act of writing to the low doubleword of the ICR causes the
    // IPI to be sent" - SDM 13.6.1, `.references/sdm.txt:171174`. A
    // write of zero is therefore a fixed-mode IPI with vector 0 to APIC
    // ID 0, which is an interrupt this VMM invented, not a clear. So the
    // ICR keeps whatever was left in it: nothing reads it back for
    // state, and its delivery-status bit is read-only.
    //
    // *IRR, ISR and TMR.* Read-only in both modes - SDM Table 13-6,
    // `.references/sdm.txt:172145`, marks all twenty-four of their
    // doublewords "Read-only" - so the reset KVM performs at
    // lapic.c:2775-2779 has **no equivalent that can be executed against
    // a passed-through local APIC at all**. This is the half of
    // `kvm_lapic_reset` that is missing here, and it cannot be closed by
    // writing anything.
    //
    // **The residual risk is a stale ISR bit.** The in-service register
    // records the vector a processor is handling and is cleared one bit
    // at a time by the end-of-interrupt its handler writes. An INIT this
    // VMM emulates destroys that handler, so the bit stays set, and the
    // processor priority it feeds then blocks every vector at or below
    // it for the rest of that processor's life - which presents from
    // outside as a processor that started and never checked in.
    //
    // The only instrument that would clear it is a blind
    // end-of-interrupt per set bit, and that is **rejected rather than
    // merely unimplemented**: the in-service register is readable, so it
    // could be done, but this local APIC belongs to a machine with a
    // passed-through NVMe on it, and "if the terminated interrupt was a
    // level-triggered interrupt, the local APIC also sends an
    // end-of-interrupt message to all I/O APICs" - SDM 13.8.5,
    // `.references/sdm.txt:171777`. A reset that can de-assert a live
    // device interrupt is worse than the stale bit it clears. If it ever
    // has to be done, it needs the ISR read first and exactly one EOI
    // per bit actually set, never a fixed count.
}

void hypervisor::apply_start_up(arch::x86_64::context & context,
                                std::uint64_t vector,
                                const char * from,
                                bool first_launch)
{
    auto & vmcs = this->vmcs;

    // Counted per processor. See the declaration: the guest hypervisor
    // is re-running its own processor bring-up thousands of times on an
    // application processor, and this says whether it is because
    // something here keeps starting it.
    if (auto here = this->vmcs.vpid(); (0 != here) && (here <= max_cpus)) {
        // Recorded **before** the increment, so slot 0 is the first
        // application rather than the second. `from` is a string
        // literal in this module, so the pointer stays valid and the
        // reader resolves it. See `start_up_from`: the log line that
        // used to carry this is unavailable, because the log ring reads
        // empty on a two-processor boot.
        if (auto slot = this->start_up_applied[here - 1];
            slot < start_up_from_slots) {
            // The first eight characters, packed little-endian, rather
            // than the pointer. The reader batches its reads up front,
            // so an address would need a second pass it does not have -
            // and a label it cannot fetch reads as absent, which is the
            // failure this member exists to avoid. Eight characters
            // separate every caller: "launch", "queued", "sipi exi".
            std::uint64_t packed{};

            for (std::size_t i = 0;
                 (i < 8) && (nullptr != from) && ('\0' != from[i]);
                 ++i) {
                packed |= static_cast<std::uint64_t>(
                              static_cast<unsigned char>(from[i]))
                          << (8 * i);
            }

            this->start_up_from[here - 1][slot] = packed;
        }

        this->start_up_applied[here - 1] =
            this->start_up_applied[here - 1] + 1;
    }

    using segment_descriptor = arch::x86_64::segment_descriptor;

    // A second start-up IPI for a processor already started is ignored.
    // INIT-SIPI-SIPI sends two, and the second would otherwise send a
    // processor that is already running back to its entry point - which
    // wedges it in a way indistinguishable from never having started.
    // Guarded here rather than relying on the hardware to discard the
    // second one, which it does not do reliably.
    if (auto cpu = vmcs.vpid() - 1; cpu < max_cpus) {
        // A processor being launched out of the trampoline is being
        // started for the first time, whatever this flag says. The guard
        // below exists to ignore the *second* start-up IPI of an
        // INIT-SIPI-SIPI sequence, and a launch is not one of those.
        //
        // Honouring it there is a contradiction rather than a
        // conservatism: declining leaves the VMCS holding what
        // setup_vmcs captured, which on this path is this VMM's own C
        // frame - an unusable CS and a RIP inside this module - and VM
        // entry rejects it. Measured as exactly one processor of eight
        // failing per boot, a different one each time, with
        // "cpu N start-up already applied, not applying again, asked by
        // launch" immediately before it.
        if (this->started_by_start_up_ipi[cpu] && !first_launch) {
            // Said out loud, because returning here leaves the guest
            // state as whoever built it last, and on a processor coming
            // out of the trampoline that state is this VMM's own C frame
            // - an unusable CS and a RIP inside this module, which VM
            // entry then rejects. Silently declining to apply start-up
            // state and silently failing to enter look identical from
            // outside, and one of them is this function's fault.
            log("cpu {} start-up already applied, not applying again, "
                "asked by {}",
                cpu,
                from);

            // Counted, because the increment at the top of this function
            // has already happened and counts an entry rather than an
            // application. Without this pair the two cannot be told
            // apart from a dump, and a two-processor boot reporting
            // "start-ups applied 2" against one start-up-IPI exit has no
            // consistent reading at all. See the declaration.
            this->start_up_declined[cpu] =
                this->start_up_declined[cpu] + 1;
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
    auto entry_controls =
        vmcs.vm_entry_controls() &
        ~arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest;

    // See `nested_vmx::init_clears_efer`. The paragraph above is right
    // that writing the field alone does nothing; the answer is to ask
    // for it to be loaded as well, which is what SDM Table 12-1's
    // "IA32_EFER 0H after INIT" requires of us here. Without it a
    // restarted processor keeps the host's LME and its own stub lands
    // in long mode when it enables paging.
    // **Both controls, not just the entry one.** Requesting the load
    // alone froze EFER at zero for every entry afterwards and wedged
    // the guest at 753 exits - the control persists, so a value
    // written once to fix one entry is then imposed for ever. With
    // the exit control paired to it the field carries the guest's own
    // EFER out on every exit and back in on every entry, so it is
    // authoritative rather than frozen, and writing zero here means
    // what INIT means.
    if constexpr (nested_vmx::init_clears_efer) {
        entry_controls |=
            arch::x86_64::vmx::vm_entry_controls::load_ia32_efer;

        vmcs.vm_exit_controls(
            vmcs.vm_exit_controls() |
            arch::x86_64::vmx::vm_exit_controls::save_ia32_efer);

        vmcs.guest_ia32_efer(0);
    }

    vmcs.vm_entry_controls(entry_controls);

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

    // DR0 through DR3, for exactly the same reason and out of the same
    // row of the same table. SDM Table 12-1 ([[PAGE 3522]]), INIT
    // column: "DR0, DR1, DR2, DR3   00000000H". None of the four is in
    // the guest-state area - SDM 25.4 lists DR7 and nothing else - so
    // VMX neither saves nor restores them, and an address the guest
    // armed before its INIT is still in the register afterwards unless
    // something writes it. Nothing did.
    //
    // KVM writes them on the same INIT path that zeroes the general
    // purpose registers a few lines above this: `kvm_vcpu_reset`
    // (.references/kvm/x86.c) does `memset(vcpu->arch.db, 0,
    // sizeof(vcpu->arch.db))` followed by `kvm_update_dr0123(vcpu)`.
    //
    // Guest-observable two ways, and the second is the one that bites. A
    // guest can read the stale address straight back with `mov rax, dr0`.
    // Worse, DR7 is written as 00000400H above - every breakpoint
    // disabled - so the four addresses sit there inert until the guest
    // enables DR7 for breakpoints of its own, and it then takes a #DB at
    // an address it never armed in this life. Neither can happen on real
    // hardware, because a real INIT clears the registers; here the INIT
    // is a VM exit, and SDM 28.2 ([[PAGE 4208]]) is explicit that such an
    // exit performs "none of the operations normally associated with
    // these events" and does "not modify register state".
    constexpr std::uint8_t address_debug_registers = 4;
    for (std::uint8_t index{}; index < address_debug_registers; ++index) {
        arch::x86_64::debug_register(index, 0);
    }

    // CR2, same table and missed for the same reason: the row reads
    // "CR2, CR3, CR4   00000000H", and of the three only CR3 and CR4 are
    // VMCS guest fields - both written above. CR2 is shared, and
    // `write_cr2`'s own comment already says why: "VMX neither saves nor
    // restores CR2 across a transition - SDM 25.4 and 25.5 list the host
    // and guest state areas, and CR2 is in neither".
    //
    // KVM zeroes it in the same function, `vcpu->arch.cr2 = 0`.
    arch::x86_64::write_cr2(0);

    // And the local APIC, for the same reason DR6 is written above: it is
    // not a VMCS field, it belongs to this processor, and SDM 13.4.7.3
    // (`.references/sdm.txt:170737`) says an INIT resets it - "the
    // processor responds by beginning the initialization process of the
    // processor core *and the local APIC*", to "the same as it is after a
    // power-up or hardware reset, except that the APIC ID and arbitration
    // ID registers are not affected". Neither of those two is touched
    // there.
    //
    // Nothing did it, and nothing else could: SDM 28.2
    // (`.references/sdm.txt:200947`) says of an INIT-signal VM exit that
    // "a logical processor performs none of the operations normally
    // associated with these events", so a processor the guest restarted
    // carried the previous occupant's LVTs, task priority, spurious
    // vector and timer straight through its own INIT.
    //
    // Here rather than in `emulate_init_signal`, with the rest of the
    // INIT state, and for the reason stated above it: this is where there
    // is time, and a processor in wait-for-SIPI takes no interrupts in
    // between anyway. It is after the duplicate guard on purpose - a
    // second start-up IPI to a processor that is already running is
    // declined above and must not reset a live APIC.
    if constexpr (nested_vmx::reset_apic_on_init) {
        reset_local_apic_after_init();
    }

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

    // **Which application of the start-up state this was.** The state
    // itself is not in doubt - `cr0 0x30 cr3 0x0 cr4 0x2000` is what the
    // writes above produce and nothing else in this tree produces it -
    // but *when* it was applied is, and a second application sends a
    // processor that is already running the operating system back to a
    // page that may no longer hold the trampoline the guest put there.
    // The caller's name, the vector and the count are what separate the
    // two, and they exist nowhere else.
    if constexpr (nested_vmx::trace_ap_entry) {
        if (auto cpu = vmcs.vpid() - 1; cpu < max_cpus) {
            log("start-up applied on cpu {} vector {} by {} "
                "first-launch {}, application {}",
                cpu + 1,
                vector,
                from,
                static_cast<std::uint64_t>(first_launch),
                this->start_up_applied[cpu]);
            trace_guest_state(cpu, "start-up-applied");
        }
    }

    // Adopted, so the watch that adopted it has done its work. See
    // `nested_vmx::drop_watch_on_start_up`: left armed it charges this
    // processor an exit for each of its own bring-up writes, and the
    // quiet-period fallback is about two minutes away at the rig's
    // clock. The drop itself happens on the next violation, in
    // `on_ept_violation`, which resumes without advancing RIP so the
    // guest re-executes against an entry that now permits it.
    if constexpr (nested_vmx::drop_watch_on_start_up) {
        if (!this->all_processors_started.load(
                std::memory_order_relaxed)) {
            this->all_processors_started.store(true,
                                               std::memory_order_relaxed);
            log("start-up applied on cpu {}, dropping the local apic "
                "page watch now rather than after the quiet period",
                vmcs.vpid());
        }
    }
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

    // Asked again, now that this is exclusive. The test that sent us here
    // is in `start_up_processor` and is made *outside* the lock, so
    // between it and this line another processor can have started this
    // one - two senders answering the same broadcast is enough, and
    // `processor_virtualized[slot]` is written by the target itself from
    // inside its own launch.
    //
    // Going on anyway is not merely wasted work, it is destructive, and
    // the worst of it is one line: `start_up_launched[slot] = false`
    // below. That flag is what `wait_for_ept_acknowledgement` uses to
    // decide which processors must answer an extended page table change -
    // a processor whose flag is clear is skipped, on the argument that it
    // holds no translation. Clearing it for a processor that is running
    // the guest removes it from every rendezvous from then on, silently
    // and permanently, because only `main` ever sets it again. The rest
    // follows: the shared trampoline area is rewritten under a processor
    // that may still be climbing it, and a start-up IPI goes out to one
    // that is executing.
    //
    // Answered as adopted rather than refused, because it is true: the
    // processor is up and virtualized, at the vector the first sender
    // recorded. That is the same answer `start_up_processor` gives a
    // duplicate start-up IPI aimed at a running processor, and SDM 29.7.2
    // says the hardware discards one too - "the active state blocks
    // start-up IPIs (SIPIs)".
    if (this->processor_virtualized[slot]) {
        log("cpu {} was started while this start-up ipi waited for the "
            "lock, vector {} not re-applied",
            slot,
            guest_vector);
        return true;
    }

    this->guest_start_up_vector[slot] = guest_vector;
    this->started_by_trampoline[slot] = true;

    // `start_up_launched[slot] = false` used to be here, and taking it
    // out is the rest of the fix the re-test above only half made.
    //
    // The re-test narrows the window and cannot close it, because the
    // target does not take this lock: it marks itself virtualized and
    // then launched from inside `main`, so a sender can read "not
    // virtualized", have the target mark itself in the gap, and then
    // clear a flag belonging to a processor that is running. Measured in
    // tests/ap_start_up as five rounds in four hundred with the clear
    // still present, and none without it.
    //
    // Nothing needs it cleared. The flag is written true by `main` on the
    // target and false only by `rewind_for_resume`, which runs on one
    // processor with nothing else alive - so a processor that has never
    // launched already reads false, and one that has reads true, which is
    // the answer the wait below wants anyway. Removing the write makes
    // the target the only writer of the "up" edge, which is what a flag
    // that means "this processor is running the guest" has to be.
    //
    // It matters because of who else reads it:
    // `wait_for_ept_acknowledgement` skips a processor whose flag is
    // clear, on the argument that it holds no translation. Clearing it
    // for a running processor takes that processor out of every extended
    // page table rendezvous from then on, silently and for good.

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

    // And the stage, which the memcpy above does not cover: it lives at
    // 0xa2 and assembly_owned ends at 0x80.
    //
    // That made the failure diagnostic lie in the one case it exists for.
    // "Zero means the trampoline never ran a single instruction" is only
    // true until some processor has run it - after that every later
    // failure reports the *previous* processor's stage, and reports it as
    // though it were its own. Read from a real boot as
    // "cpu 2 did not come up, trampoline stage 6" six times over, with
    // the 6 belonging to processor 1, which had succeeded.
    area.stage = static_cast<std::uint8_t>(
        arch::x86_64::ap_start_up_stage::not_started);

    area.argument = slot;

    // Per processor. This was one shared stack handed to every
    // application processor and to the boot processor's
    // resume-from-sleep slot, and its own comment named two cases it
    // did not cover - a slow target still on it when the sender's
    // bounded wait expires, and a target whose `main` fails returning
    // onto that frame. Neither is bounded when the processors are
    // adopted from the guest's own start-up IPIs rather than launched
    // one at a time by a loader.
    area.stack_top = reinterpret_cast<std::uint64_t>(
        std::end(this->start_up_stack[(slot < max_cpus) ? slot : 0]));

    // The vector is the trampoline page's page number, which is the whole
    // reason that page had to be below one megabyte.
    //
    // **Its value is logged and must be read rather than inferred.**
    // `initialize_start_up_memory` prints it on the line that assigns the
    // member - "start-up memory ready at {}, vector {}" - and on the rig
    // it is `0x9c`, because the UEFI loader takes the *highest* free page
    // below one megabyte (`AllocateMaxAddress`,
    // `uefi_loader/src/main.cpp:1065`). Reading this expression instead
    // of that line is how a guest's start-up IPI carrying vector `0x2`
    // was attributed to this VMM for a whole session; see BACKLOG.md,
    // "RETRACTED: vector 0x2 is not ours". A vector in a ring or an exit
    // record is ours only if it equals that logged number.
    //
    // Through send_start_up_ipi rather than straight to the x2APIC
    // command MSR, which is what this used to do. That MSR does not exist
    // while the APIC is in xAPIC mode and the write faults - see
    // send_start_up_ipi. A guest that writes its own command to the APIC
    // page is on a processor in exactly that mode, so this path could
    // never have started a processor there.
    send_start_up_ipi(this->apic_id[slot], this->start_up_memory >> 12);

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

    // `started_by_trampoline[slot]` is deliberately **left set**.
    //
    // Clearing it here was the "the trampoline timeout races the processor
    // it is timing out" item in BACKLOG.md, and this is the whole of that
    // race: giving up does not stop the target, it only stops waiting for
    // it. A processor that is merely slow arrives afterwards and reads
    // this flag in `main`, where it decides three things - whether to
    // capture the operating system's registers, whether to build an
    // intermediate GDT by reading the OS descriptor table, and whether to
    // put the OS page table back on the way out. All three are wrong for a
    // processor that came out of the trampoline, and the second one reads
    // through a page table that no longer maps what it names.
    //
    // So the flag describes how this processor arrived, which the timeout
    // does not change. Nothing reads it for a processor that never
    // arrives, and a later attempt sets it again at the top of this
    // function, so leaving it costs nothing and closes the window.
    return false;
}

hypervisor::start_up_result hypervisor::start_up_processor(
    std::uint64_t destination, std::uint64_t vector)
{
    auto slot = processor_slot(destination);
    if (!slot) {
        log("no room to track the processor with apic id {}", destination);
        return start_up_result::needs_hardware;
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
        // Only the *second* start-up IPI of one sequence is ignored, and
        // "one sequence" means since this processor's last INIT from the
        // sender that is asking now.
        //
        // `started_by_start_up_ipi` alone is not that. apply_start_up
        // sets it for every processor it applies a vector to, including
        // the seven the firmware started with its broadcast SIPI long
        // before the guest's operating system existed. So by the time
        // Windows starts its own processors, all seven look "already
        // started" and every one of their start-up IPIs is swallowed.
        //
        // Measured, and this is the whole of the remaining stall: slot 1
        // reports `by_sipi 0` while the log says "already started,
        // ignored" for it - both true, in that order, because the SIPI
        // was swallowed and the INIT that followed then cleared the flag.
        // Hyper-V does not re-send after that, so the processor never
        // receives the vector, never checks in, and gets INITed again.
        //
        // The INIT is what separates them. A processor that has taken one
        // since it was last started is *waiting* for a start-up IPI - SDM
        // 11.4.2 - and the next one is the first of its sequence, not the
        // second. `started_by_start_up_ipi` is cleared by
        // emulate_init_signal, so what is wanted is exactly the flag
        // itself; what was missing is that the firmware's own start-up
        // must not count as the guest's.
        // None of the above survives contact with a guest hypervisor,
        // and the reason is that both flags are a *proxy* for a fact the
        // processor already records.
        //
        // KVM makes exactly one test, in `kvm_apic_accept_events`
        // (lapic.c): "INITs are blocked while CPU is in specific states
        // ..., while SIPIs are dropped if the CPU isn't in wait-for-SIPI
        // (WFS)" - it delivers the vector when the target's mp_state is
        // KVM_MP_STATE_INIT_RECEIVED and drops it otherwise. There is no
        // "already started" flag anywhere in it, and its delivery path
        // (APIC_DM_STARTUP) records every SIPI unconditionally.
        //
        // Measured on the rig with Hyper-V running, which is what the
        // proxy cost: slots 2-7 had *both* flags set while their VMCS
        // activity state was 3 - wait-for-SIPI - so every start-up IPI
        // Hyper-V sent them was swallowed, and its boot processor sat
        // spinning at one RIP taking preemption-timer exits, waiting for
        // processors that were waiting for it. Slot 1 was the control:
        // flags 0 and 1, so this guard never even looked at it, and it
        // was parked in the same state at RIP 0x90e7. Two flags, one of
        // them cleared by INIT, could not describe it; the activity
        // state describes it exactly.
        //
        // So the architectural fact decides. It is written on every
        // resume from the VMCS itself, and SDM 11.4.2 says a processor
        // in wait-for-SIPI is waiting for precisely this message.
        constexpr std::uint64_t wait_for_sipi = 3;

        // Two records to ask, because there are two levels a virtual
        // processor can be waiting at and only one of them is in a VMCS
        // this VMM enters.
        //
        // `resume_activity_state` is the first-level guest's, read off
        // vmcs01 on the way out of the exit handler. `l2_activity_state`
        // is a second-level guest's, and it exists precisely because
        // wait-for-SIPI is never handed to hardware - see
        // `enter_or_park_l2`, which holds the processor in root operation
        // and is listening on the hand-off below at exactly this moment.
        // Asking only the first would drop every start-up IPI a guest
        // hypervisor's own guest sends, since the processor running it
        // reports its hypervisor's activity state, which is active.
        auto activity = (wait_for_sipi == this->l2_activity_state[*slot])
                            ? wait_for_sipi
                            : this->resume_activity_state[*slot];

        if (wait_for_sipi != activity) {
            // **Queued, not dropped.** Measured before this: on a
            // three-processor boot the log carried four of these and
            // *zero* hand-overs, so not one start-up IPI ever reached a
            // processor and both application processors stayed in the
            // firmware's park loop for the life of the boot.
            //
            // The activity record read above is written by the exit path
            // at the end of the handler, so between a target's INIT exit
            // and its publishing the hand-off it still says "active" -
            // for the whole software wait, which covers both of the
            // start-up IPIs SDM 11.4.4.1 step 15 has a guest send. The
            // reading is not wrong, it is early.
            //
            // Holding the vector is what the flag attempt in
            // `interrupt_command.cpp` could not do. A flag on the target
            // carries no order against the start-up IPI behind it and
            // could land after the vector had been accepted; a held
            // vector cannot be applied until the target reaches the
            // point where it is waiting, so the INIT is necessarily
            // first. That is the ordering KVM gets from
            // `apic->pending_events`.
            this->queued_start_up[*slot].store(
                queued_start_up_valid | vector, std::memory_order_release);

            log("guest start-up ipi for cpu {}, activity {} is not "
                "wait-for-sipi, queued vector {} and forwarded",
                *slot,
                activity,
                vector);

            // **Queued is not delivered, so the guest's write goes out.**
            //
            // This returned `adopted` until now, which swallows the
            // guest's store to the interrupt command register. That is a
            // claim that this VMM delivered the command, and queuing is
            // not delivering - it is a promise to deliver later, to a
            // processor that may never come and ask. The promise is kept
            // only if the target reaches `emulate_init_signal` and drains
            // the mailbox; every other outcome destroys the command.
            //
            // Measured, and this is the shape of it. Nesting on, two
            // processors, one variable: the guest hypervisor sent three
            // INIT/start-up pairs and the target recorded one INIT exit
            // and one start-up-IPI exit, so **two entire bring-up
            // attempts were consumed inside this interception** - and
            // this is the branch that consumes them, because a target
            // that is running reports `active` and lands here. The guest
            // then reset the machine, reproducibly, over five boots.
            //
            // Forwarding is correct, not a hedge, and the architecture
            // says so from both ends:
            //
            // - A start-up IPI aimed at a processor that is not waiting
            //   for one is discarded *by the target*, not by the sender.
            //   SDM 29.7.2: "The active state blocks start-up IPIs
            //   (SIPIs). SIPIs that arrive while a logical processor is
            //   in the active state and in VMX non-root operation are
            //   discarded and do not cause VM exits." So a forwarded
            //   command reaching a running processor costs nothing and
            //   produces nothing, which is exactly what the guest would
            //   have got on bare metal.
            // - A target that *is* parked in wait-for-SIPI in non-root
            //   operation takes a start-up-IPI VM exit instead (SDM
            //   28.2), which `emulate_start_up_ipi` handles. That is the
            //   architectural delivery path, and the mailbox is only
            //   ever a stand-in for it while the target is in root mode,
            //   where the layer below discards the IPI - see
            //   `emulate_init_signal`.
            //
            // The two cannot both start the processor. `apply_start_up`
            // refuses a second application while `started_by_start_up
            // _ipi` is set and this is not a launch, and only an INIT
            // clears that - which is the same guard that already makes
            // the conventional INIT-SIPI-SIPI sequence safe.
            //
            // What this gives up is the swallow, and the swallow was the
            // liability: with it, losing the queued vector loses the
            // processor, which is why `emulate_init_signal` had to be
            // argued into keeping a vector across an INIT and why
            // `discard_start_up_for_init` had to be written to take it
            // away again. Without it the mailbox is an optimisation
            // whose loss costs nothing, because the guest's own command
            // is still on the wire.
            return start_up_result::needs_hardware;
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
            return start_up_result::adopted;
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
        return start_up_result::needs_hardware;
    }

    // Never seen before, so this is the guest starting it for the first
    // time. Nothing has virtualized it and nothing can, from here - a
    // processor cannot be put into VMX operation by another one. So it is
    // started in this VMM's own trampoline instead, which brings it up
    // under the hypervisor and only then lets it run from the vector the
    // guest asked for.
    if (!this->start_up_memory) {
        log("no start-up memory, cpu {} will run unvirtualized", *slot);
        return start_up_result::needs_hardware;
    }

    if (start_application_processor(*slot, vector)) {
        return start_up_result::adopted;
    }

    // It did not come up. Letting a real start-up IPI reach it is the
    // least bad thing left: the processor is still waiting for one, so
    // the guest gets a processor it can use, unvirtualized. Losing it
    // outright would usually take the guest down with it.
    return start_up_result::needs_hardware;
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

void hypervisor::emulate_init_signal(arch::x86_64::context & context)
{
    auto & vmcs = this->vmcs;

    // Counted per processor, beside `start_up_applied`. The pair
    // distinguishes "something keeps sending this processor INIT" from
    // "the guest hypervisor's bring-up restarts itself".
    if (auto here = this->vmcs.vpid(); (0 != here) && (here <= max_cpus)) {
        this->init_emulated[here - 1] = this->init_emulated[here - 1] + 1;

        // **Deliberately not cleared here, and that is a reversal.**
        //
        // This used to discard any held vector at the top of the INIT, on
        // the argument that a processor being re-started must not inherit
        // a vector meant for its last life - which is what
        // `kvm_apic_accept_events` does. Measured, it discarded the
        // *live* one:
        //
        //     guest init ipi, command 0xc4500
        //     start-up ipi for cpu 1, activity 0 is not wait-for-sipi,
        //         queued vector 0x87
        //     ... (much later)
        //     cpu 1 init: found activity 0, waiting for the hardware
        //         start-up ipi
        //
        // The guest sends INIT then start-up IPI, and this VMM sees the
        // start-up IPI **before** the target reaches its own INIT exit,
        // because `interrupt_command.cpp` only forwards the INIT and the
        // target's activity record does not change until it faults in
        // here. So the vector is always queued before this point, never
        // after it, and clearing at the top threw away the only one that
        // was ever going to arrive.
        //
        // KVM can clear because its INIT and start-up IPI are both
        // latched in `apic->pending_events` and accepted together. Here
        // they arrive by different routes and the ordering is inverted,
        // so the queue must survive the INIT that precedes it.
        //
        // Staleness is bounded instead by consume-on-use: the slot holds
        // one vector, and the exchange below empties it when it is
        // applied.
    }

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

    // **The APIC mode is deliberately not part of this decision any
    // more, and removing it is the fix.**
    //
    // This used to read `x2apic_enabled() && nested`, on the stated
    // grounds that "the interrupt command register is an MSR, which it is
    // only in x2APIC mode; in xAPIC mode it is a location on the APIC
    // page and the MSR bitmap never sees it, so waiting would burn the
    // whole timeout before falling back for nothing".
    //
    // That premise was true when it was written and was falsified three
    // days later. `b3ca36c` added the conjunct on 2026-08-04; `540d8b6`
    // added the xAPIC interception on 2026-08-07, and it is live today:
    // `filter_local_apic_write` and `on_local_apic_write` both key on
    // offset 0x300 of the watched APIC page, read the two dwords back out
    // of the page and compose them into the same shape the x2APIC MSR
    // carries, expressly "so one decision function serves both". A
    // command written in xAPIC mode reaches `on_interrupt_command`, and
    // therefore reaches the hand-off, exactly as an MSR write does.
    //
    // What the stale conjunct cost, measured on the rig - an xAPIC
    // machine, `rdmsr 0x1b` on the target reading 0xfee00800 with EXTD
    // clear:
    //
    //     guest start-up ipi for cpu 1, activity 0 is not wait-for-sipi,
    //         queued vector 0x87
    //     guest start-up ipi for cpu 1, vector 0x2, to hardware
    //
    // The sender queued the vector and swallowed the guest's write - see
    // `start_up_processor`, which returns `adopted` for that - while the
    // target went down the hardware branch, which had no consumer for the
    // queue at all until `ZPP_APPLY_QUEUED_START_UP`. So the vector was
    // destroyed by the one processor that could have used it, and the
    // processor was later started at a vector from a different sequence.
    //
    // Nothing is lost on a machine that really cannot be handed a vector:
    // the wait is bounded and falls back to the architectural path, and
    // the queue is drained into the hand-off before the first spin, so
    // the case this rig is actually in - the vector already queued by the
    // time the target arrives - costs no iterations at all.

    // Worth waiting for when the hardware path cannot be trusted, which
    // is precisely when something is virtualizing *us*.
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
    //
    // **Two conjuncts, and the second is new.** A software hand-off
    // needs a sender that can make one, and a sender can only make one
    // if it sees the guest's write to the interrupt command register.
    // With neither the page watch nor the MSR bitmap bit armed, nothing
    // does - so the wait below is two million iterations of nothing,
    // taken in root mode where the layer underneath is discarding the
    // very IPI being waited for, followed by the fallback that was going
    // to happen anyway.
    //
    // It did not matter while the watch was armed for the whole of every
    // boot. It matters now that `every_platform_processor_adopted` can
    // drop it, which is the point of dropping it: once every processor
    // is adopted, an INIT and its start-up IPI are ordinary VM exits on
    // the target and the architectural path is the *only* one, so taking
    // the software wait would add tens of milliseconds of a processor
    // being dead to the guest, once per INIT, for nothing.
    //
    // Same defect class as the conjunct `5729ef9` removed from this
    // line, in the other direction. That one - `x2apic_enabled()` -
    // tested the APIC's mode, which stopped deciding anything the day
    // `540d8b6` taught the page watch to decode the same register. The
    // question was never which mode the APIC is in; it is whether this
    // VMM is looking at it.
    auto waited = nested && interrupt_command_intercepted();

    // Both facts about this processor are published *before* the wait
    // below, and that ordering is the whole of this fix.
    //
    // The sender does not look at the mailbox first. `start_up_processor`
    // gates on the activity state and only then compare-exchanges into
    // `start_up_handoff`, so a target whose activity record still says
    // "active" has its start-up IPI dropped - and returned as `adopted`,
    // which swallows the guest's write to the interrupt command register.
    // The vector is destroyed rather than delivered.
    //
    // That record is `resume_activity_state`, written by the exit path at
    // the *end* of the handler (see the tail of the exit handler, where it
    // is sampled off vmcs01). So for the entire length of the software
    // wait - up to two million iterations - it holds the value from the
    // exit before the INIT, which is `active`. The window is not a race of
    // instructions: it is the whole wait, and it covers both of the two
    // start-up IPIs SDM 11.4.4.1 step 15 has a guest send.
    //
    // `enter_or_park_l2` already had this right: it writes
    // `l2_activity_state[cpu]` before calling `wait_for_l2_start_up_ipi`,
    // which is why the second-level hand-off works and this one did not.
    // The asymmetry was the bug.
    //
    // Written to the VMCS as well as to the record, and it costs nothing
    // to do it here rather than after: the field is consumed at VM entry,
    // which is far away, and `apply_start_up` puts it back to `active` on
    // every path that delivers a vector.
    vmcs.guest_activity_state(
        arch::x86_64::vmx::activity_state::wait_for_start_up_ipi);
    this->resume_activity_state[cpu] =
        arch::x86_64::vmx::activity_state::wait_for_start_up_ipi;

    if (waited) {
        // Published before the first attempt, so a sender that arrives
        // during the wait finds this processor listening.
        handoff.store(start_up_handoff_state::software_wait);

        // And a sender that arrived *before* this processor was
        // listening. Taken after the hand-off is published so the two
        // cannot both deliver: a sender racing this exchange either wins
        // the compare-exchange below on `software_wait`, or finds the
        // queue already emptied here.
        //
        // The valid bit is tested rather than the whole word, so that a
        // slot holding anything other than a vector cannot be read as
        // vector zero - which would start this processor at physical
        // address zero, an address nobody chose.
        if (auto queued = this->queued_start_up[cpu].exchange(
                0, std::memory_order_acq_rel);
            0 != (queued & queued_start_up_valid)) {
            auto expected = start_up_handoff_state::software_wait;
            static_cast<void>(handoff.compare_exchange_strong(
                expected, start_up_handoff_state::deliver(queued & 0xff)));
        }

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
                               start_up_handoff_state::vector(state),
                               "init wait");
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
                           start_up_handoff_state::vector(expected),
                           "init race");
            return;
        }
    } else {
        // Waiting on hardware from the start, which is the case on bare
        // metal and under Bochs. Said out loud here rather than left
        // implied, because a sender that assumed otherwise is what used
        // to swallow the IPI this processor is now waiting for.
        handoff.store(start_up_handoff_state::hardware_wait);

        // **And a vector that arrived before this processor was ready
        // must be applied here, on this path too.** The consumption used
        // to live only in the software-wait branch above, and this was
        // the branch every processor took, because `waited` carried a
        // stale conjunct on the APIC mode - see the paragraph above it.
        // With that removed this branch is bare metal and Bochs only,
        // where a queued vector is rare rather than universal, and the
        // consumption stays because rare is not never.
        //
        // Measured, that is not hypothetical - it is the whole failure:
        //
        //     start-up ipi for cpu 1, activity 0 is not wait-for-sipi,
        //         queued vector 0x87        <- never applied
        //     start-up ipi for cpu 1, vector 0x2, to hardware
        //
        // The processor was started with the *later* vector instead, and
        // `0x2` is not where the operating system put its start-up code,
        // which is why it came up inside the firmware's parked loop and
        // never became a Windows processor.
        //
        // Applied directly rather than through the mailbox, because
        // nothing is going to come and collect it: on this path the
        // processor returns to wait for a hardware start-up IPI that has
        // already been sent and refused.
        //
        // **What made applying it dangerous is fixed elsewhere.** The
        // ring showed two `init` exits carrying `cs=0x8700` then
        // `cs=0x0200`, vector 0x87 applied and then 0x02 over it, and
        // the 0x02 came out of this slot: it was queued by the *second*
        // start-up IPI of a sequence this VMM had already satisfied, sat
        // in the mailbox with nothing to say which sequence it belonged
        // to, and was then applied to the next INIT. A vector cannot
        // carry that by itself, so the sender clears the slot when it
        // sees the INIT - `on_interrupt_command`, and the same rule
        // `kvm_apic_accept_events` applies when it takes one. What is
        // left here is a vector the guest sent *after* that INIT, which
        // is the one this processor is waiting for.
        if (!nested_vmx::apply_queued_start_up) {
            if (auto queued = this->queued_start_up[cpu].load(
                    std::memory_order_acquire);
                0 != (queued & queued_start_up_valid)) {
                log("cpu {} init: holding queued start-up vector {} "
                    "rather than applying it",
                    cpu,
                    queued & 0xff);
            }
        } else if (auto queued = this->queued_start_up[cpu].exchange(
                       0, std::memory_order_acq_rel);
                   0 != (queued & queued_start_up_valid)) {
            log("cpu {} init: applying start-up ipi vector {} that "
                "arrived before this processor was waiting",
                cpu,
                queued & 0xff);

            apply_start_up(context, queued & 0xff, "queued", false);
            return;
        }
    }

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

void hypervisor::send_start_up_ipi(std::uint64_t apic,
                                   std::uint64_t vector)
{
    // Delivery mode 110b is start-up, and its vector is the entry point's
    // page number rather than an interrupt vector.
    constexpr std::uint64_t delivery_mode_start_up = 0x6ull << 8;
    constexpr std::uint64_t level_assert = 1ull << 14;

    if (x2apic_enabled()) {
        constexpr std::uint64_t destination_shift = 32;

        arch::x86_64::wrmsr(arch::x86_64::msr::ia32_x2apic_icr,
                            vector | delivery_mode_start_up |
                                (apic << destination_shift));
        return;
    }

    // xAPIC, so the command goes through the APIC page, which the host
    // page table already maps because the interrupt command watch needs
    // it. The destination half is written first: writing the low half is
    // what sends the interrupt, so a destination written after it would
    // be written after the thing that used it.
    auto base = local_apic_base();

    constexpr std::uint64_t interrupt_command_low = 0x300;
    constexpr std::uint64_t interrupt_command_high = 0x310;

    auto * bytes = reinterpret_cast<volatile std::uint8_t *>(base);

    // Eight bits of destination, in 31:24. The x2APIC form above carries
    // thirty-two, which is the other reason these cannot share a line.
    arch::x86_64::write32(bytes + interrupt_command_high,
                          static_cast<std::uint32_t>(apic << 24));
    arch::x86_64::write32(
        bytes + interrupt_command_low,
        static_cast<std::uint32_t>(vector | delivery_mode_start_up |
                                   level_assert));
}

} // namespace zpp::hypervisor
