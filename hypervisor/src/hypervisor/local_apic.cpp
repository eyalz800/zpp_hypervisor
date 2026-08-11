#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/msr.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/scope_exit.h"
#include <cstdint>
#include <optional>

namespace zpp::hypervisor
{

void hypervisor::monitor_trap_flag(bool value)
{
    // Bit 27 of the primary processor based controls, SDM Table 25-6.
    constexpr std::uint64_t monitor_trap_flag_bit = 1ull << 27;

    auto controls =
        this->vmcs.primary_processor_based_vm_execution_controls();

    // Through `adjust_msr`, like every other control this VMM writes, and
    // that is a correctness requirement rather than tidiness.
    //
    // `5531fdc` fixed exactly this defect one control field over, for the
    // pin-based preemption timer, and recorded what it costs: "Setting a
    // control that is not permitted does not fail where it is written; it
    // fails the next VM entry, and under a nested hypervisor it can simply
    // never come back" - one exit recorded, and no second exit ever. There
    // is no fault, no exit and no record, because the failure happens on
    // the way *in*.
    //
    // The bit was reachable in that state: `setup_vmcs` composes the
    // primary controls through `adjust_msr`, which correctly drops the
    // monitor trap flag on a processor that does not permit it, and this
    // function then OR'd it straight back in - bypassing the only thing
    // that was checking. It fires on the stepping path, so every
    // straddling or non-plain-store write to a watched page took it,
    // the local APIC page included.
    //
    // Re-adjusting the whole field rather than testing the one bit,
    // because the read-modify-write can only be legal if what it starts
    // from is: `adjust_msr` forces the allowed-0 bits back on and masks
    // to allowed-1, so the result is a fixed point of the capability MSR
    // whatever it was given.
    auto requested = value ? (controls | monitor_trap_flag_bit)
                           : (controls & ~monitor_trap_flag_bit);

    auto permitted = arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(
            arch::x86_64::vmx::msr::true_processor_based_controls),
        requested);

    this->vmcs.primary_processor_based_vm_execution_controls(permitted);

    // Asked for and not granted, which is a degradation rather than a
    // fault and so has to be said out loud. The stepping path arms this
    // to be told when one instruction has retired; without it the trap
    // exit never arrives, `on_monitor_trap_flag` never runs, and the
    // watched page is left open for every processor - which is the
    // failure `build_vmcs02` describes at length for the same bit going
    // missing from vmcs02.
    if (value && (0 == (permitted & monitor_trap_flag_bit))) {
        log("the monitor trap flag is not permitted by this processor, "
            "so a stepped write cannot be closed");
    }
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

void hypervisor::note_apic_mode(std::size_t cpu)
{
    // IA32_APIC_BASE[EN] is bit 11 and [EXTD] is bit 10, and SDM 13.12.5.1
    // reads the pair as the four states below - the fourth of which,
    // EN=0 with EXTD=1, "is not valid and it is not possible to get into".
    constexpr std::uint64_t apic_base_enabled = 1ull << 11;
    constexpr std::uint64_t apic_base_extended = 1ull << 10;

    auto base = arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base);

    auto mode = apic_mode::disabled;
    if (0 != (base & apic_base_extended)) {
        mode = apic_mode::x2apic;
    } else if (0 != (base & apic_base_enabled)) {
        mode = apic_mode::xapic;
    }

    // Everything below is one processor's decision about machine-wide
    // state, so it is taken under a lock.
    //
    // The state is machine-wide twice over: `observed_apic_mode` is read
    // across every processor, and the MSR bitmap is a **single page**
    // that every processor's VMCS points at - `msr_bitmap` is one array
    // in this class, not one per processor. So `intercept_interrupt_
    // command` is a read-modify-write of a byte another processor may be
    // reading, writing, or having its own VMX operation consult, and it
    // was performed in root operation with nothing holding anything.
    //
    // The case that breaks without this is the one the survey below was
    // written for, and the comment on it named the hazard without
    // covering it: a guest switches its processors to x2APIC one at a
    // time. Two processors doing that at once each write their own slot
    // and then survey, and a processor whose survey ran before the other
    // processor's store became visible computes `any_x2apic` false and
    // *disarms* the interception the other one has just armed. Which of
    // the two unsynchronised writes to the shared byte lands last decides
    // the outcome, and the losing outcome is a processor in x2APIC mode
    // whose interrupt command register is not intercepted - so its
    // start-up IPIs are never seen and the processors it starts are never
    // adopted.
    //
    // The lock is not recursive and nothing under it takes another, which
    // is what makes it safe to hold across `watch_local_apic`: that path
    // reaches the extended page tables, and the page-watch code takes no
    // lock of its own.
    this->apic_mode_lock.lock();
    zpp::scope_exit unlock{[&] { this->apic_mode_lock.unlock(); }};

    if (cpu < max_cpus) {
        this->observed_apic_mode[cpu] = mode;
    }

    // Over every processor, not over the caller. A guest switches its
    // processors to x2APIC one at a time, so during that switch both
    // mechanisms are genuinely in use at once and disarming either would
    // lose interrupt commands from the processors that have not moved yet.
    auto any_x2apic = false;
    auto any_xapic = false;
    for (auto seen : this->observed_apic_mode) {
        any_x2apic = any_x2apic || (apic_mode::x2apic == seen);
        any_xapic = any_xapic || (apic_mode::xapic == seen);
    }

    // Armed only while some processor is in x2APIC mode, which is a
    // correctness matter and not only a cost. The register is an MSR only
    // in that mode - SDM 13.12.1 - so a guest in xAPIC mode writing it is
    // one that should take #GP, and an armed bit would instead exit and
    // have this VMM perform the write in the host, where the #GP has no
    // recovery point and stops the processor.
    intercept_interrupt_command(any_x2apic);

    // And the page, only while some processor still uses it. Reads the
    // base from the calling processor's own MSR, which is right because
    // the page is a single physical address that every processor's local
    // APIC answers at; a machine that gave each processor a different one
    // would need a watch per processor and gets none.
    watch_local_apic(any_xapic);

    // The shape is KVM's. kvm_lapic_set_base in arch/x86/kvm/lapic.c
    // notices any change in either of those two bits and calls
    // set_virtual_apic_mode, and vmx_set_virtual_apic_mode in
    // arch/x86/kvm/vmx/vmx.c then switches on the resulting mode to arm
    // the APIC-access mechanism for xAPIC or the x2APIC MSR bitmap for
    // x2APIC - the same two things, re-derived rather than left as they
    // were set at launch.
}

} // namespace zpp::hypervisor
