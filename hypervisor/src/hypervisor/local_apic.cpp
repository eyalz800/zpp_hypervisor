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

    // Recorded beside the write rather than read back out of the bitmap
    // by anybody who wants to know. See the member: the bit's position
    // is this function's business, and a second decoder of it elsewhere
    // is a second thing to get wrong.
    this->interrupt_command_bitmap_armed = intercept;

    // Every processor's merged bitmap was built from the page just
    // edited, and `nested_bitmap_is_ours` says some of them need not be
    // rebuilt. That is true only until this runs.
    forget_nested_bitmaps();
}

bool hypervisor::interrupt_command_intercepted()
{
    // Either mechanism is enough, and they are genuinely both in use at
    // once while a guest moves its processors to x2APIC one at a time -
    // which is the case `note_apic_mode` surveys every processor for.
    return (0 != this->watched_apic_page) ||
           this->interrupt_command_bitmap_armed;
}

bool hypervisor::every_platform_processor_adopted()
{
    // No roster, nothing to prove against. See the declaration - this is
    // the safe direction, and it is the answer on any loader that hands
    // over no processor list.
    if (0 == this->number_of_platform_processors) {
        return false;
    }

    // Deliberately over `max_cpus` rather than over
    // `number_of_known_processors`, because the two tables are filled in
    // by different writers. `main` writes `apic_id[cpuid]` for the
    // processor it is running on and does not touch the count;
    // `processor_slot` appends and does. A scan bounded by the count
    // would miss a processor that recorded itself in a slot beyond it.
    //
    // `apic_id` zero-initializes and zero is a real identifier - the
    // boot processor's, usually - so a slot is only allowed to answer
    // for a roster entry when it is *virtualized*. An empty slot is
    // never virtualized, so it can never satisfy the roster by holding a
    // default-constructed identifier.
    for (std::size_t i{}; i < this->number_of_platform_processors; ++i) {
        auto found = false;

        for (std::size_t slot{}; slot < max_cpus; ++slot) {
            if (!this->processor_virtualized[slot]) {
                continue;
            }
            if (this->apic_id[slot] == this->platform_apic_id[i]) {
                found = true;
                break;
            }
        }

        if (!found) {
            return false;
        }
    }

    return true;
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
    // Both interceptions are expressible off, as one variable.
    //
    // They are the only thing this VMM does to the guest's own interrupt
    // controller, and the nested stall is a guest that stops switching
    // trust levels and then waits for ever - so "is the watch itself the
    // deadlock" is a question worth one boot rather than an argument.
    //
    // **Switching it off is safe only with one processor**, which is how
    // that experiment is meant to be run. The interception exists to
    // catch a start-up IPI and replace it with one naming this VMM's own
    // trampoline; with no application processors there is no start-up
    // IPI to catch and nothing to hand over unvirtualized. With more
    // than one processor, off hands them to the guest and they are lost
    // - see `on_interrupt_command`.
    //
    // **On is not the multiprocessor answer either, and the asymmetry
    // between the two modes is why.** In x2APIC mode this is already a
    // narrow interception: one bit in the MSR bitmap, for the one
    // register that can carry a start-up IPI, and nothing else on the
    // local APIC costs an exit. In xAPIC mode the register is a location
    // on a page and the only tool for a page is the extended page
    // tables, whose granularity is the page - so `watch_local_apic` has
    // to take write permission away from the whole local APIC, and every
    // end-of-interrupt, task-priority write and timer arming the guest
    // performs becomes a VM exit. Measured on a settled eight-processor
    // guest, 1,586 violations a second, 23.2% of every exit the boot
    // processor took; measured on a two-processor guest that then reset,
    // 57.9%. None of that traffic can carry a start-up IPI.
    //
    // **And the cost is worse with more processors for a reason that is
    // not the obvious one.** Each processor's own writes fault on that
    // processor, so a second processor does not add faults to the first.
    // What a second processor adds is *inter-processor interrupts*, and
    // every one of them is two writes to this page by the sender - the
    // destination at 0x310 and then the command at 0x300 - so every
    // reschedule, every translation-lookaside-buffer shootdown and every
    // deferred-call IPI the guest sends costs the *sending* processor
    // two extended-page-table violations plus a full decode of
    // `on_interrupt_command` on the second of them. A single-processor
    // guest sends essentially none. That is the one term in this cost
    // that scales with the processor count on a single processor's own
    // exit ledger, and it is the first thing to check against a
    // one-processor control before anything subtler is proposed.
    //
    // Two things follow, and only the first is done here. The watch is
    // needed only until every processor is adopted, which is now proved
    // rather than guessed - see `every_platform_processor_adopted`, and
    // `on_ept_violation`, which drops it on the fault that proves it.
    // And the mechanism itself is wrong for the job. The architectural
    // tool is the APIC-access page - "virtualize APIC accesses",
    // secondary control bit 0 - which reports the page offset in the
    // exit qualification instead of requiring the faulting instruction
    // to be decoded. SDM 25.9.4's exit-qualification table, at
    // `.references/sdm.txt:203804`: "bits 11:0 of the exit qualification
    // are set to the page offset". That alone would retire three
    // instruments this tree records as unreliable - the decode of the
    // faulting instruction, the monitor-trap step for the accesses it
    // cannot decode, and the guest-linear-address fallback that
    // `on_ept_violation` needs because *every* violation on the rig
    // arrives with qualification bit 7 clear.
    //
    // **What it would not do is make the traffic cheaper, and the claim
    // beside `nested_vmx::disarm_apic_watch` that it would is wrong.**
    // That claim reads "SDM 32.4.3.2 has INIT and SIPI always take the
    // trap-like APIC-write exit while ordinary traffic stops exiting".
    // The first half is right and the second is not. Read at
    // `.references/sdm.txt:206998`, APIC-write emulation is by page
    // offset:
    //
    // - 300H, the interrupt command register's low half: with both
    //   "virtual-interrupt delivery" and "IPI virtualization" clear, the
    //   processor "causes an APIC-write VM exit" unconditionally. This
    //   is the half that is true, and it is the half this VMM wants.
    // - 310H, its high half: "No other virtualization or VM exit
    //   occurs." Free.
    // - 080H, the task priority: TPR virtualization, no exit.
    // - 0B0H, the end of interrupt: EOI virtualization *only* with
    //   virtual-interrupt delivery set; otherwise an APIC-write VM exit.
    // - "Any other page offset. The processor causes an APIC-write VM
    //   exit." Which includes 380H, the timer's initial count.
    //
    // The two registers `filter_local_apic_write` measured as the
    // hottest on this machine are 0B0H and 380H, at about ten thousand
    // writes a second on the boot processor alone. One of them still
    // exits under any configuration and the other needs virtual-interrupt
    // delivery, which is a much larger commitment than an APIC-access
    // page. So this is the right interception on fidelity grounds and it
    // is not the answer to the 57.9%; the answer to that is not being
    // armed once there is nothing left to catch.
    //
    // Either way it is a project rather than an edit: every one of these
    // mechanisms services the access out of a virtual-APIC page, so this
    // VMM would have to maintain one against the real local APIC.
    // Spelled in nested_vmx.h beside every other build switch, so that
    // `zpp switches:` on the built binary reports it. A cache reading
    // OFF is not evidence, and for a whole session this one was not
    // visible anywhere else - see the declaration.
    constexpr bool intercept_apic = nested_vmx::intercept_apic;

    intercept_interrupt_command(intercept_apic && any_x2apic);

    // And the page, only while some processor still uses it. Reads the
    // base from the calling processor's own MSR, which is right because
    // the page is a single physical address that every processor's local
    // APIC answers at; a machine that gave each processor a different one
    // would need a watch per processor and gets none.
    watch_local_apic(intercept_apic && any_xapic);

    // The shape is KVM's. kvm_lapic_set_base in arch/x86/kvm/lapic.c
    // notices any change in either of those two bits and calls
    // set_virtual_apic_mode, and vmx_set_virtual_apic_mode in
    // arch/x86/kvm/vmx/vmx.c then switches on the resulting mode to arm
    // the APIC-access mechanism for xAPIC or the x2APIC MSR bitmap for
    // x2APIC - the same two things, re-derived rather than left as they
    // were set at launch.
}

} // namespace zpp::hypervisor
