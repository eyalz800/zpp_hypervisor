// The VM exit dispatch: everything this VMM tells its guest.
//
// One function, `hypervisor::on_vm_exit`, and it is the whole of the
// switch on the basic exit reason - the CPUID answers, the MSR reads and
// writes, the control register accesses, the VMX instructions that
// fault, the extended page-table violations, the I/O instructions, and
// the re-entry through `resume_guest` at the end.
//
// It was a lambda inside `hypervisor::main`, passed to `vm_launch`.
// Seventeen hundred lines of it, in a ten thousand line file, which
// meant no harness could reach any part of it: a test that wanted one
// case had to have the whole of hypervisor.cpp compile, and nothing can
// compile hypervisor.cpp. Every case in here was added because Windows
// failed in a way that pointed somewhere else entirely - CLAUDE.md's
// "What the guest is told" is the list - and until this move none of
// them could have a hosted test at all.
//
// **What made the move safe, stated because the next such move should be
// checked the same way.** The lambda captured `[&]`, which says nothing
// about what it actually uses. Changing that to `[this]` and compiling
// named the entire dependency on `main`'s scope: one variable, `cpuid`,
// the processor index `main` was launched with. That is now this
// function's first parameter, and nothing else in the body changed - not
// one line reflowed, not one name requalified.
//
// The parameter keeps the name `cpuid` for exactly that reason. It is a
// poor name for a processor index and it is the name the seventeen
// hundred lines already use, so renaming it here would have turned a
// verifiable motion into a diff nobody can check.
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
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/power.h"
#include "zpp/loader.h"
#include "zpp/nvme/command.h"
#include "zpp/scope_exit.h"
#include "zpp/spin_lock.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>

namespace zpp::hypervisor
{
void hypervisor::on_vm_exit(std::uint64_t cpuid,
                            arch::x86_64::context & context)
{
    using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
    auto & vmcs = this->vmcs;


    // One thousand accesses each, once, to price the instructions the
    // whole optimisation question turns on. See
    // `vmread_benchmark_cycles`, and the field choice below.
    if (!this->vmread_benchmark_done) {
        using bench_field = arch::x86_64::vmx::vmcs::field;
        this->vmread_benchmark_done = true;

        std::uint64_t sink{};

        auto price_read = [&](bench_field which) {
            auto before = arch::x86_64::rdtsc();
            for (int i = 0; i < 1000; ++i) {
                sink += vmcs.read(which);
            }
            return arch::x86_64::rdtsc() - before;
        };

        // The value is read first and written back unchanged, so this
        // prices the instruction without altering any state.
        auto price_write = [&](bench_field which) {
            auto value = vmcs.read(which);
            auto before = arch::x86_64::rdtsc();
            for (int i = 0; i < 1000; ++i) {
                vmcs.write(which, value);
            }
            return arch::x86_64::rdtsc() - before;
        };

        this->vmread_benchmark_cycles =
            price_read(bench_field::exit_reason);
        this->vmread_shadowed_cycles = price_read(bench_field::guest_rip);
        this->vmread_unshadowed_cycles =
            price_read(bench_field::guest_gdtr_base);
        this->vmwrite_shadowed_cycles =
            price_write(bench_field::guest_rsp);
        this->vmwrite_unshadowed_cycles =
            price_write(bench_field::guest_gdtr_limit);
        this->vmread_benchmark_sink = sink;

        log("vmcs price per 1000: exit_reason {} rip {} gdtr_base {}",
            this->vmread_benchmark_cycles,
            this->vmread_shadowed_cycles,
            this->vmread_unshadowed_cycles);
        log("vmcs price per 1000: write rsp {} write gdtr_limit {}",
            this->vmwrite_shadowed_cycles,
            this->vmwrite_unshadowed_cycles);
    }

    // A deliberate slowdown, off unless asked for. See ZPP_SLOW_EXITS.
    //
    // Fifteen per cent faster did not move the stall at all, and that is
    // consistent with two opposite things: a throughput limit that
    // fifteen per cent does not cross, and a wall that no speed crosses.
    // Making the machine faster cannot separate them - making it
    // *slower* can. If the point the guest stops at moves down with this
    // turned up, the limit is throughput; if it sits where it is, speed
    // was never the question.
#ifndef ZPP_SLOW_EXITS
#define ZPP_SLOW_EXITS 0
#endif
    if constexpr (0 != ZPP_SLOW_EXITS) {
        auto until = arch::x86_64::rdtsc() + ZPP_SLOW_EXITS;
        while (arch::x86_64::rdtsc() < until) {
            zpp::spin_hint();
        }
    }

    // The clock for `handler_cycles`. See its declaration: this is the
    // measurement that decides whether the fix is fewer instructions per
    // exit or fewer exits, and it is taken here because here is the
    // first instruction of this VMM's own code after the exit.
    if (cpuid < max_cpus) {
        auto now = arch::x86_64::rdtsc();
        this->handler_entry_tsc[cpuid] = now;
        if (0 == this->handler_first_tsc[cpuid]) {
            this->handler_first_tsc[cpuid] = now;
        }

        // Closes the span opened at the resume, and attributes it to
        // whichever level was entered. See `l2_run_cycles`: this is the
        // only measurement in the tree denominated in the guest's work
        // rather than this VMM's, and it is the quantity the timer
        // stretch moved.
        if (0 != this->level_run_tsc[cpuid]) {
            auto ran = now - this->level_run_tsc[cpuid];
            if (this->level_run_was_l2[cpuid]) {
                this->l2_run_cycles[cpuid] += ran;
            } else {
                this->l1_run_cycles[cpuid] += ran;
            }
        }
    }

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
                reserve_channel_queue_allocation();
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
    auto full_reason = arch::x86_64::vmx::exit_reason(vmcs.exit_reason());

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
    // Did this exit interrupt an event the processor was in the
    // middle of delivering? One VMREAD, before anything decides what
    // to do with the exit, because the answer is destroyed by the
    // next entry.
    if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus)) {
        auto cpu = slot - 1;
        constexpr std::uint64_t vectoring_valid = 1ull << 31;

        if (auto vectoring =
                vmcs.read(arch::x86_64::vmx::vmcs::field::
                              idt_vectoring_information_field);
            0 != (vectoring & vectoring_valid)) {
            if (this->running_l2[cpu]) {
                this->idt_vectoring_l2[cpu] =
                    this->idt_vectoring_l2[cpu] + 1;
            } else {
                this->idt_vectoring_l1[cpu] =
                    this->idt_vectoring_l1[cpu] + 1;
            }

            if (auto at = this->idt_vectoring_trace_count;
                at < idt_vectoring_trace_capacity) {
                this->idt_vectoring_trace[at] =
                    vectoring | (full_reason.value() << 32);
                this->idt_vectoring_trace_count = at + 1;
            }

            // Held for the entry that follows. The error code is
            // only meaningful when its own valid bit says so, and
            // the length only for the three software types - but
            // both are read here rather than at re-injection,
            // because the next entry destroys the fields they come
            // from.
            constexpr std::uint64_t vectoring_error_valid = 1ull << 11;

            this->pending_event[cpu] = vectoring;
            this->pending_event_l2[cpu] = this->running_l2[cpu];

            // Which second-level guest it was being delivered to.
            //
            // Held events are not always re-injected on the very
            // next entry - one for a second-level guest is deferred
            // while its hypervisor runs - and "the same guest" has
            // to mean something over that gap. The guest
            // hypervisor's current VMCS is what names it: a
            // VMPTRLD of another region is a different guest, and
            // an event held across that switch belongs to a guest
            // that is no longer running.
            this->pending_event_vmcs[cpu] = this->guest_current_vmcs[cpu];
            this->pending_event_error[cpu] =
                (0 != (vectoring & vectoring_error_valid))
                    ? vmcs.read(arch::x86_64::vmx::vmcs::field::
                                    idt_vectoring_error_code)
                    : 0;
            this->pending_event_length[cpu] =
                vmcs.vm_exit_instruction_length();
        }
    }

    if constexpr (nested_vmx::enabled) {
        if (auto slot = vmcs.vpid(); (0 != slot) && (slot <= max_cpus) &&
                                     this->running_l2[slot - 1]) {
            if (l2_exit_outcome::deferred !=
                on_l2_exit(slot - 1, full_reason, context, advance_rip)) {
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
    case basic_reason::external_interrupt: {
        // Only reachable with ZPP_VIRTUALIZE_APIC on, because nothing
        // else in this VMM sets external-interrupt exiting. See the
        // flag: this is the KVM shape - take every interrupt here and
        // put it back into the guest - against this VMM's own, where the
        // guest owns the controller and the interrupt never exits.
        //
        // The vector is in hand only because `acknowledge_interrupt_on_
        // exit` is set with the control (SDM 30.2); without it the field
        // is not valid and the interrupt would still be pending at the
        // controller, so dropping it here would lose it for ever.
        //
        // **Nothing retired to produce this exit**, and getting that
        // wrong is what broke the firmware the first time this switch
        // was turned on. SDM 30.2.5 lists the VM exits for which the
        // VM-exit instruction length is defined - fault-like exits due
        // to named instructions, software exceptions, task switches, a
        // few others - and ends "All VM exits other than those listed in
        // the above items leave this field undefined"
        // (`sdm.txt:204135`). An external-interrupt exit is not on that
        // list, so the field holds whatever the last instruction-caused
        // exit left in it, and the default `advance_rip` added it to the
        // guest's RIP. The guest resumed a few bytes into the middle of
        // an instruction it had not executed, *and* was handed the
        // interrupt, so its handler's return address was the corrupt
        // one. KVM's `handle_external_interrupt` (`vmx.c:5383`) does
        // nothing but count and return 1 - it never calls
        // `kvm_skip_emulated_instruction` - which is the same statement
        // in a codebase where advancing is opt-in rather than default.
        advance_rip = false;

        constexpr std::uint64_t interruption_valid = 1ull << 31;
        constexpr std::uint64_t interruption_vector = 0xff;

        auto information = vmcs.read(arch::x86_64::vmx::vmcs::field::
                                         vm_exit_interruption_information);

        if (auto slot = vmcs.vpid();
            (0 != slot) && (slot <= max_cpus) &&
            (0 != (information & interruption_valid))) {
            // Queued, not held in a single slot. The acknowledge has
            // already taken this vector out of the interrupt controller
            // and set its in-service bit there, so a vector this VMM
            // fails to deliver is not merely late: it is gone, and the
            // in-service bit nothing will now EOI blocks every interrupt
            // at or below its priority for the rest of the machine's
            // life. See `queue_external_interrupt`.
            auto vector = information & interruption_vector;

            this->external_interrupt_vector_counts[slot - 1][vector] += 1;

            queue_external_interrupt(slot - 1, vector);
        }
        break;
    }

    case basic_reason::interrupt_window: {
        // Armed by the resume path when it had a vector to deliver and
        // the guest could not take one. Nothing to do here: the window
        // is open by definition now, so the resume below will inject it
        // and disarm the control.
        //
        // A case is *required* even though it does nothing, because
        // `default:` stops the processor - see the note at the top of
        // this handler. An exit this VMM asked for and then did not
        // handle would be a halt of its own making.
        //
        // Nothing retired here either, and for the same reason as the
        // case above: SDM 30.2.5 does not list this exit, so the
        // instruction-length field is undefined. KVM's
        // `handle_interrupt_window` (`vmx.c:5653`) likewise clears the
        // control and returns without skipping anything. Unreachable
        // with ZPP_VIRTUALIZE_APIC off - nothing else here ever sets
        // interrupt-window exiting, and a guest hypervisor that sets it
        // in vmcs12 has the exit reflected before this switch is
        // reached - so this is not a change to that build's behaviour.
        advance_rip = false;
        break;
    }

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
            vmcs.vm_entry_interruption_information_field(valid | type_nmi |
                                                         2u);
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

        // Recorded before the answer is edited, so the pair below is
        // what the guest asked and what it was told, in order. Frozen
        // when full: the leaves that decide anything are asked during
        // start-up.
        auto trace_slot = this->cpuid_trace_count;
        this->cpuid_trace_count = trace_slot + 1;

        if ((leaf >= 0x40000000u) && (leaf <= 0x4fffffffu)) {
            this->cpuid_hypervisor_leaves_asked =
                this->cpuid_hypervisor_leaves_asked + 1;
        }

        scope_exit record_cpuid{[&] {
            if (trace_slot < cpuid_trace_capacity) {
                this->cpuid_trace[trace_slot] = cpuid_trace_entry{
                    .leaf = leaf,
                    .subleaf = static_cast<std::uint32_t>(context.rcx),
                    .ecx_answered = cpuid_result[2],
                    .eax_answered = cpuid_result[0],
                };
            }
        }};

        // The range reserved for hypervisor use. Nothing physical
        // answers here, so whatever a guest reads is whatever the
        // layer above it chose to say.
        constexpr std::uint32_t hypervisor_leaf_first = 0x40000000;
        constexpr std::uint32_t hypervisor_leaf_last = 0x4fffffff;

        // The block that begins at hypervisor_leaf_first, in the shape
        // the interface signature below commits this VMM to. Named
        // rather than written as offsets so the maximum leaf reported
        // at the base cannot drift from the set actually answered -
        // which is exactly what had happened.
        constexpr std::uint32_t interface_leaf = hypervisor_leaf_first + 1;
        constexpr std::uint32_t version_leaf = hypervisor_leaf_first + 2;
        constexpr std::uint32_t features_leaf = hypervisor_leaf_first + 3;
        constexpr std::uint32_t recommendations_leaf =
            hypervisor_leaf_first + 4;
        constexpr std::uint32_t limits_leaf = hypervisor_leaf_first + 5;

        // Where the enlightened VMCS version is reported. The guest
        // hypervisor reads it to decide whether the layer below speaks a
        // version it knows; KVM checks the same leaf against its own
        // KVM_EVMCS_VERSION at `.references/kvm/vmx.c:565-567`.
        constexpr std::uint32_t nested_features_leaf =
            hypervisor_leaf_first + 0xa;

        // The highest leaf of that block, which is what EAX at the
        // base means: KVM's own reader takes it that way -
        // kvm_get_hypervisor_cpuid in arch/x86/kvm/cpuid.c matches the
        // signature in EBX/ECX/EDX and then records `cpuid.limit =
        // entry->eax` - and KVM's documentation of its own signature
        // leaf says outright that "the value in eax corresponds to the
        // maximum cpuid function present in this leaf".
        //
        // It used to be the diagnostic leaf's number, which was wrong
        // in both directions at once: it under-reported the block
        // while more leaves were answered above it, and it named a
        // leaf that is not part of this block at all.
        // With the enlightenment offered the block reaches the nested
        // features leaf, and the maximum has to say so - a guest that
        // stops at `limits_leaf` never reads the version and never turns
        // the enlightenment on.
        constexpr std::uint32_t hypervisor_leaf_maximum =
            nested_vmx::evmcs_offered ? nested_features_leaf
            : nested_vmx::announce_hypervisor
                ? limits_leaf
                : hypervisor_leaf_first;

        // Reports a given processor's most recent exit, selected by
        // ecx. Inside the range this VMM already owns, so it costs no
        // new interface and nothing underneath can answer it instead.
        //
        // In its own block at 0x40000100 rather than at 0x40000001,
        // and moving it was a bug fix rather than tidying: 0x40000001
        // is where the interface signature goes, so on any build with
        // announce_hypervisor on the signature shadowed the diagnostic
        // entirely and every reading taken through it was the four
        // bytes "Hv#1" instead of a trace.
        //
        // 0x100 is the step because that is the granularity a vendor
        // block may begin on. KVM's own scan says so:
        // for_each_possible_cpuid_base_hypervisor in
        // arch/x86/include/asm/cpuid/api.h is
        // `for (function = 0x40000000; function < 0x40010000; function
        // += 0x100)`. And KVM does not assume its own block sits at
        // the first of those bases either - kvm_get_hypervisor_cpuid
        // in arch/x86/kvm/cpuid.c walks the same sequence looking for
        // the signature and takes whichever base carries it. So a
        // second vendor block at 0x40000100 is a layout the reference
        // implementation already expects to find rather than an
        // invention, and a guest scanning for the block at 0x40000000
        // never lands on this one.
        constexpr std::uint32_t diagnostic_leaf = 0x40000100;

        // A presence leaf was added at 0x40000101 and **removed again**,
        // for two independent reasons that both say the same thing about
        // claiming leaves inside this range.
        //
        // It did not work: a guest running under Hyper-V reads
        // "Microsoft Hv" at 0x40000101 as well as at 0x40000000, because
        // Hyper-V claims the whole range and answers every leaf in it -
        // exactly as this VMM does, for the reason stated below.
        //
        // And it broke the invariant that rule exists to keep. The guest
        // suite's `cpuid.range_answered_whole` asserts every leaf in
        // 0x40000000..0x4fffffff outside the advertised block answers
        // zero; a leaf answering its own signature is precisely the
        // "answered part of an interface" failure this file warns about
        // everywhere else. The suite caught it on the first run.
        //
        // `deep_presence_leaf` below replaces it and is strictly better:
        // outside the range, so it neither breaks the invariant nor gets
        // swallowed by a hypervisor above.

        // The same question asked from *two* levels up, which the leaf
        // above cannot answer.
        //
        // Measured on the rig: a guest running under Hyper-V sees
        // "Microsoft Hv" at 0x40000000 **and at 0x40000101**, because
        // Hyper-V claims the whole 0x40000000-0x4fffffff range and
        // answers every leaf in it - exactly as this VMM does, and for
        // the same reason. So no leaf inside that range can ever reach
        // a guest with another hypervisor above it.
        //
        // Outside the range is different: a hypervisor has no reason to
        // synthesise a leaf that is not its own, so it executes CPUID
        // and passes the processor's answer up - and the processor's
        // answer, under this VMM, is this one.
        //
        // 0x8fffffff is chosen because it is architecturally undefined.
        // It is above the maximum extended leaf on every processor this
        // runs on, so nothing reads it expecting a defined value, and
        // answering it takes nothing away from a guest. That is the
        // whole test for whether a leaf may be claimed: not "is it
        // free" but "does claiming it deprive a guest of an answer it
        // would otherwise have got".
        constexpr std::uint32_t deep_presence_leaf = 0x8fffffff;

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
            //
            // Kept clear, except where this VMM is deliberately
            // presenting the interface of whatever it runs under.
            //
            // Measured, and it is why the first attempt at that
            // presentation proved nothing: with this bit clear the
            // guest asked for **zero** leaves in the whole hypervisor
            // range - 0 of 49,860 CPUID leaves - so passing that range
            // through was invisible to it. Announcing a hypervisor is
            // what makes a guest go looking for one, and the two are
            // therefore one switch.
            //
            // It also plausibly explains the feature this is all for.
            // A guest that believes it is on bare metal applies bare
            // metal requirements to virtualization-based security,
            // Secure Boot among them, and this rig reports Secure Boot
            // unsupported. A guest that knows it is virtualized takes
            // the nested path instead. The reference this is being
            // compared against announces itself unconditionally.
            if constexpr (nested_vmx::pass_through_hypervisor_interface ||
                          nested_vmx::announce_hypervisor) {
                cpuid_result[2] |= (1u << 31);
            } else {
                cpuid_result[2] &= ~(1u << 31);
            }

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

            // Safer mode extensions, ECX[6], concealed.
            //
            // Nothing here implements SMX, and the pairing matters
            // rather than the bit on its own: IA32_FEATURE_CONTROL has
            // separate permissions for VMXON inside and outside SMX,
            // and the value handed to the guest below grants only
            // outside. A guest told SMX exists may take the inside-SMX
            // path and find it refused. Concealing the extension and
            // granting only the outside permission are one decision,
            // and a smaller bare-metal reference that carries this
            // exact guest makes both, saying "currently support VMX
            // outside SMX only" where it does.
            cpuid_result[2] &= ~(1u << 6);

            // What the guest was actually told, so bit 5 can be read
            // back rather than reasoned about. A guest that never
            // executes VMXON may simply never have been offered it.
            this->cpuid_leaf_1_ecx_reported = cpuid_result[2];

            // MONITOR/MWAIT, ECX[3], is deliberately left as the
            // hardware reports it. Both instructions execute in the
            // guest and neither is intercepted, so there is nothing
            // to conceal - and concealing it is what produced the
            // 0x139 bugcheck described where those controls are
            // declared. Leaf 5, which the same bit governs, is
            // likewise passed through untouched, so the guest sees
            // one consistent answer across both leaves.
        } else if (deep_presence_leaf == leaf) {
            // Answered outside the hypervisor range on purpose, so a
            // guest two levels up can see it. See the declaration.
            cpuid_result[0] = 1;
            cpuid_result[1] = 0x5a70705a;
            cpuid_result[2] = 0x705a7070;
            cpuid_result[3] = 0x70705a70;
        } else if ((leaf >= hypervisor_leaf_first) &&
                   (leaf <= hypervisor_leaf_last) &&
                   !nested_vmx::pass_through_hypervisor_interface) {
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
                // The highest leaf of *this* block, which is the set
                // answered below and nothing else.
                cpuid_result[0] = hypervisor_leaf_maximum;

                // HyperVisor Name: ZppZppZppZpp - except when the
                // enlightened VMCS is offered, where it must be
                // "Microsoft Hv".
                //
                // **Measured, and it is why the first attempt was
                // declined.** A guest hypervisor probing for the
                // interface matches the *vendor* here before it looks at
                // anything else - Xen's `hyperv_probe`,
                // `.references/xen/xen/arch/x86/guest/hyperv/hyperv.c:46`,
                // returns NULL unless ebx/ecx/edx spell "Micr", "osof",
                // "t Hv", and only then reads `Hv#1` at 0x40000001, the
                // features, and the hints leaf carrying the
                // enlightenment recommendation. With `ZppZppZppZpp` here
                // the probe stops at the first check: booted on the rig
                // with everything else in place, the guest hypervisor
                // never wrote HV_X64_MSR_VP_ASSIST_PAGE and all three
                // counters read zero.
                //
                // Claiming the name is not claiming the whole interface:
                // the same probe then requires the hypercall and
                // processor-index MSRs from the privilege mask below and
                // gives up if they are missing, so what a guest is
                // entitled to is still what 0x40000003 says. That is the
                // contract KVM works to as well.
                if constexpr (nested_vmx::evmcs_offered) {
                    cpuid_result[1] = 0x7263694d;
                    cpuid_result[2] = 0x666f736f;
                    cpuid_result[3] = 0x76482074;
                } else {
                    cpuid_result[1] = 0x5a70705a;
                    cpuid_result[2] = 0x705a7070;
                    cpuid_result[3] = 0x70705a70;
                }
            } else if (nested_vmx::announce_hypervisor &&
                       (interface_leaf == leaf)) {
                // The interface signature, which is what a guest
                // matches on rather than the vendor above. "Hv#1".
                //
                // Claimed with no features behind it, which the three
                // leaves below are what actually say. A guest that
                // recognises the interface and finds it offers nothing
                // keeps doing everything the way it would without one
                // - and that is the point, because the enlightenment
                // that changes how it starts its processors is the one
                // thing this VMM must not have taken away from it.
                cpuid_result[0] = 0x31237648;
                cpuid_result[1] = 0;
                cpuid_result[2] = 0;
                cpuid_result[3] = 0;
            } else if (nested_vmx::announce_hypervisor &&
                       ((version_leaf == leaf) ||
                        (features_leaf == leaf) ||
                        (recommendations_leaf == leaf))) {
                // Version, features and recommendations, all zero, and
                // written out here rather than reached by falling
                // through to the zeroes at the bottom. Falling through
                // gave the same bytes and said nothing about why, and
                // these three are the leaves that decide what a guest
                // is entitled to expect - so they are the last place a
                // silent answer belongs.
                //
                // Recommendations zero is what makes an unimplemented
                // hypercall legal. Nothing is recommended, so a guest
                // has been told to use no enlightenment, and a guest
                // that uses none never issues a hypercall this VMM
                // would have to answer.
                cpuid_result[0] = 0;
                cpuid_result[1] = 0;
                cpuid_result[2] = 0;
                cpuid_result[3] = 0;

                // The **privileges**, which are not zero and must not
                // be, and this is the correction to an earlier version
                // of this block that made them so.
                //
                // Zero here says "this interface is present and you are
                // entitled to none of it", which is this project's
                // recurring mistake exactly - announcing an interface
                // and answering part of it. Measured on the rig: the
                // guest hypervisor announced to, registered itself
                // through HV_X64_MSR_GUEST_OS_ID and then behaved as
                // though nothing were there.
                //
                // So it claims precisely the two privileges this VMM
                // backs and no others. Bit 5 is the hypercall MSRs -
                // the guest OS identity and the hypercall page, both
                // answered above - and bit 6 is the processor index
                // MSR, answered from `cpuid`. Everything else stays
                // clear because nothing else is implemented: the
                // synthetic interrupt controller, the synthetic timers
                // and the reference counter are all absent, and a
                // guest invited to use them would fault on the first
                // access.
                //
                // The pairing is the point. A privilege bit set here is
                // a promise that the matching MSRs answer, and the two
                // lists must be read together - the switch in the MSR
                // handler answers 0x40000000, 0x40000001 and
                // 0x40000002, which is bits 5 and 6 and nothing more.
                constexpr std::uint32_t privilege_hypercall_msrs = 1u << 5;
                constexpr std::uint32_t privilege_vp_index_msr = 1u << 6;

                if (features_leaf == leaf) {
                    cpuid_result[0] =
                        privilege_hypercall_msrs | privilege_vp_index_msr;
                }

                // The one recommendation this VMM makes, and it is a
                // promise in the same way the privileges above are: bit
                // 14 says "use an enlightened VMCS rather than VMREAD
                // and VMWRITE", and the guest hypervisor will then state
                // its VMCS through the structure in
                // `zpp/hypervisor/enlightened_vmcs.h` and stop executing
                // the instructions - which is 54% of a trust-level round
                // trip here.
                //
                // Set only with `evmcs_offered`, because a recommendation
                // whose handling is absent is the announce-an-interface
                // failure this file warns about everywhere.
                constexpr std::uint32_t recommend_enlightened_vmcs =
                    1u << 14;

                if (nested_vmx::evmcs_offered &&
                    (recommendations_leaf == leaf)) {
                    cpuid_result[0] = recommend_enlightened_vmcs;
                }
            } else if (nested_vmx::evmcs_offered &&
                       (nested_features_leaf == leaf)) {
                // The enlightened VMCS version, in the low half of eax.
                // One is the only version defined, and the only one the
                // structure transcribed here describes.
                constexpr std::uint32_t enlightened_vmcs_version = 1;

                cpuid_result[0] = enlightened_vmcs_version;
                cpuid_result[1] = 0;
                cpuid_result[2] = 0;
                cpuid_result[3] = 0;
            } else if (nested_vmx::announce_hypervisor &&
                       (limits_leaf == leaf)) {
                // Implementation limits, of which the only one this
                // VMM has an honest number for is how many processors
                // it can track - max_cpus, the dimension of every
                // per-processor array here. A guest reading zero would
                // be reading a claim that no processor is supported.
                cpuid_result[0] = static_cast<std::uint32_t>(max_cpus);
                cpuid_result[1] = 0;
                cpuid_result[2] = 0;
                cpuid_result[3] = 0;
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

                    cpuid_result[0] = static_cast<std::uint32_t>(count);
                    cpuid_result[1] =
                        count ? static_cast<std::uint32_t>(newest.reason)
                              : 0;
                    cpuid_result[2] = count ? static_cast<std::uint32_t>(
                                                  newest.qualification)
                                            : 0;

                    // Everything that says "this processor stopped and
                    // why", packed so one leaf answers the question.
                    cpuid_result[3] =
                        (this->unhandled_exit.occurred ? (1u << 0) : 0) |
                        (this->vm_entry_failure.occurred ? (1u << 1) : 0) |
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
                        (static_cast<std::uint32_t>(newest.activity_state &
                                                    0x3)
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

        // CPL, which XSETBV requires to be zero. SDM: "#GP(0) If the
        // current privilege level is not 0" - and CPL after a VM exit
        // is bits 6:5 of the guest's SS access rights, since the
        // segment registers themselves are the host's by then.
        //
        // Without this a ring-three thread in the guest could change
        // XCR0, which no real processor permits.
        constexpr std::uint64_t descriptor_privilege_shift = 5;
        constexpr std::uint64_t descriptor_privilege_mask = 3;

        auto cpl =
            (vmcs.guest_ss_access_rights() >> descriptor_privilege_shift) &
            descriptor_privilege_mask;

        if (0 != cpl) {
            inject_general_protection_fault();
            advance_rip = false;
            break;
        }

        // Masked to thirty-two bits each. SDM XSETBV: "the high-order
        // 32 bits of each of RAX and RDX are ignored" - so the value
        // is EDX:EAX, and ORing the whole of RAX in put whatever the
        // guest left in its high half into XCR0. Every MSR path in
        // this handler already masks; this one did not.
        constexpr std::uint64_t low = 0xffffffffull;

        arch::x86_64::xsetbv(context.rcx & low,
                             (context.rax & low) |
                                 ((context.rdx & low) << 32));
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
                (vmcs.read(arch::x86_64::vmx::vmcs::field::
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

        // The guest moving its local APIC, or changing its mode, or
        // switching it off.
        //
        // Performed as the guest wrote it: the local APIC underneath
        // is the guest's own and nothing here virtualizes it, so the
        // answer to "what should this register hold" is whatever the
        // guest said. What this VMM needs from the exit is not a veto,
        // it is the *notification* - both mechanisms it uses to see an
        // interrupt command are chosen from the mode this register
        // holds, and until now they were chosen once at launch and
        // never revisited.
        //
        // A write the guest would have faulted on faults here instead,
        // in the host, exactly as the timer registers above do and for
        // the same reason: the conditions are architectural and depend
        // on this same local APIC's current state, so a write that
        // faults for this side is one that would have faulted for the
        // guest. SDM 13.12.5 has the two that matter - a direct
        // transition from x2APIC mode to xAPIC mode "is not valid, and
        // the corresponding WRMSR to the IA32_APIC_BASE MSR causes a
        // general-protection exception", and so does any attempt to
        // reach EN=0 with EXTD=1.
        if (arch::x86_64::msr::ia32_apic_base ==
            static_cast<std::uint32_t>(context.rcx)) {
            arch::x86_64::wrmsr(arch::x86_64::msr::ia32_apic_base,
                                (context.rax & 0xffffffff) |
                                    (context.rdx << 32));

            // Re-derive both interceptions from what the register now
            // says, on this processor and across the machine. This is
            // the shape of KVM's kvm_lapic_set_base, which notices a
            // change in either of those two bits and calls
            // set_virtual_apic_mode rather than leaving what was armed
            // at launch in place.
            //
            // The index is bounds checked inside, so a VPID of zero -
            // which underflows here - records nothing and still
            // re-derives the two interceptions from every other
            // processor. Losing one processor's entry is a missed
            // observation; refusing to re-derive would be a missed
            // IPI.
            note_apic_mode(vmcs.vpid() - 1);
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
        // Whether the guest is looking at virtualization at all.
        //
        // `guest_vmxon_count` staying zero has two completely
        // different causes and they are indistinguishable from it: a
        // guest that never considered entering VMX operation, and one
        // that read the capabilities and declined. Counted here, at
        // the exit, so it does not matter which path below answers.
        if (auto index = static_cast<std::uint32_t>(context.rcx);
            (index >= 0x480) && (index <= 0x491)) {
            this->vmx_capability_reads = this->vmx_capability_reads + 1;
        } else if (0x3a == index) {
            this->feature_control_reads = this->feature_control_reads + 1;
        }

        // The read half of the same set. Answered before the fault
        // below for the same reason: with nested VMX compiled in the
        // bitmap is no longer all zeroes, so an in-range MSR can exit
        // because this VMM asked to see it rather than because it does
        // not exist.
        if ((basic_reason::rdmsr == reason) &&
            on_nested_vmx_msr_read(static_cast<std::uint32_t>(context.rcx),
                                   context)) {
            break;
        }

        // IA32_APIC_BASE, answered with the register's own contents.
        //
        // Nothing is edited out of it. The local APIC below is the
        // guest's, so the base it names, the mode it is in and the
        // bootstrap-processor flag are all facts about hardware the
        // guest owns - and a guest told a different base from the one
        // its own stores would reach is a guest that has been lied to
        // in the way `What the guest is told` in CLAUDE.md is about.
        //
        // Intercepted at all only so that the write half can be, since
        // the bitmap's read and write bits are independent but a guest
        // that is about to change modes reads this first. Cheap: an
        // operating system reads it during start-up and hardly ever
        // afterwards.
        if (arch::x86_64::msr::ia32_apic_base ==
            static_cast<std::uint32_t>(context.rcx)) {
            auto value =
                arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base);
            context.rax = value & 0xffffffff;
            context.rdx = value >> 32;
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
        // Forwarded rather than faulted, where this VMM has told the
        // guest an interface exists.
        //
        // A synthetic MSR is only answerable by whoever implements the
        // interface it belongs to. Claiming the interface and then
        // faulting its MSRs is the worst of both, and is precisely the
        // 0xc000000d recorded below - so the two are the same switch.
        // What makes forwarding sound here is that the interface being
        // claimed is not ours: it is the one underneath, which does
        // implement these, and passing the access straight down is the
        // whole of what "pass through" means.
        //
        // Bounded to the synthetic range on purpose. Everything below
        // it is architectural and must keep faulting when absent,
        // which is what the comment above this is about.
        if constexpr (nested_vmx::pass_through_hypervisor_interface) {
            constexpr std::uint32_t synthetic_first = 0x40000000;
            constexpr std::uint32_t synthetic_last = 0x4fffffff;

            if (auto index = static_cast<std::uint32_t>(context.rcx);
                (index >= synthetic_first) && (index <= synthetic_last)) {
                if (basic_reason::rdmsr == reason) {
                    auto value = arch::x86_64::rdmsr(index);
                    context.rax = value & 0xffffffff;
                    context.rdx = value >> 32;
                } else {
                    arch::x86_64::wrmsr(index,
                                        (context.rax & 0xffffffff) |
                                            (context.rdx << 32));
                }

                this->synthetic_msr_accesses =
                    this->synthetic_msr_accesses + 1;
                break;
            }
        }

        // The hypervisor interface's own MSRs, which exist only
        // because we said a hypervisor was here.
        //
        // **Announcing one and then faulting its MSRs killed the
        // guest outright.** Measured on the rig: with the present bit
        // set, the boot processor's last four exits were CPUID,
        // CPUID, RDMSR at guest RIP 0x1e086fd, and a triple fault at
        // the same RIP - and exactly one MSR index was ever faulted,
        // 0x40000001. So the guest reads the hypercall MSR whether or
        // not any feature bit invites it to, and a general protection
        // fault there is fatal rather than informative. That is this
        // project's recurring mistake stated exactly: answering part
        // of an interface. Announce the interface and these are part
        // of it.
        //
        // Three are answered and no more. Identity and the hypercall
        // page are what the measurement demanded; the processor index
        // is answered because it costs a register and a guest that
        // has a hypercall page will ask for it. Everything else -
        // reference counter, reference TSC, the frequency MSRs -
        // stays faulting, which is honest: nothing here backs them,
        // and no feature bit claims them.
        if constexpr (nested_vmx::announce_hypervisor) {
            constexpr std::uint32_t guest_os_id_msr = 0x40000000;
            constexpr std::uint32_t hypercall_msr = 0x40000001;
            constexpr std::uint32_t vp_index_msr = 0x40000002;

            // Where the guest hypervisor puts its own assist page, and
            // the reason the enlightenment can work at all: with an
            // enlightened VMCS it writes the structure's address into
            // that page rather than executing VMPTRLD, so this register
            // is how this VMM learns where to look. See
            // `hyperv::vp_assist_page`.
            //
            // Answered only with the enlightenment offered, because
            // outside it nothing reads the page and accepting the write
            // would be claiming a feature that does nothing - which is
            // the failure the privilege list above is careful to avoid.
            constexpr std::uint32_t vp_assist_msr = 0x40000073;

            auto index = static_cast<std::uint32_t>(context.rcx);
            auto answered = true;

            if (basic_reason::rdmsr == reason) {
                std::uint64_t value{};

                switch (index) {
                case guest_os_id_msr:
                    value = this->hyperv_guest_os_id;
                    break;
                case hypercall_msr:
                    value = this->hyperv_hypercall;
                    break;
                case vp_index_msr:
                    value = cpuid;
                    break;
                case vp_assist_msr:
                    if constexpr (!nested_vmx::evmcs_offered) {
                        answered = false;
                    } else {
                        value = (cpuid < max_cpus)
                                    ? this->hyperv_vp_assist[cpuid]
                                    : 0;
                    }
                    break;
                default:
                    answered = false;
                    break;
                }

                if (answered) {
                    context.rax = value & 0xffffffff;
                    context.rdx = value >> 32;
                }
            } else {
                auto value =
                    (context.rax & 0xffffffff) | (context.rdx << 32);

                switch (index) {
                case guest_os_id_msr:
                    this->hyperv_guest_os_id = value;

                    // Clearing the identity retires the hypercall
                    // page, so a guest cannot leave one enabled
                    // behind an identity it has withdrawn.
                    if (0 == value) {
                        this->hyperv_hypercall =
                            this->hyperv_hypercall & ~std::uint64_t{1};
                    }
                    break;

                case vp_assist_msr:
                    if constexpr (!nested_vmx::evmcs_offered) {
                        answered = false;
                    } else if (cpuid < max_cpus) {
                        this->hyperv_vp_assist[cpuid] = value;
                        this->hyperv_vp_assist_writes[cpuid] =
                            this->hyperv_vp_assist_writes[cpuid] + 1;
                    }
                    break;

                case hypercall_msr:
                    // Not installed before the guest has identified
                    // itself, which is the order the reference keeps
                    // - but the write is still accepted. Faulting it
                    // is what killed the guest, and the fault is not
                    // made better by having a reason.
                    if (0 == this->hyperv_guest_os_id) {
                        this->hypercall_page_early =
                            this->hypercall_page_early + 1;
                        break;
                    }

                    this->hyperv_hypercall = value;

                    if (0 != (value & 1)) {
                        // What the guest will call, and all it will
                        // ever get: `mov rax, 2` then `ret`, which is
                        // the invalid-hypercall-code status. A
                        // hypervisor that implements no hypercalls
                        // must fail them all cleanly rather than
                        // return success for a call it did not make.
                        //
                        // `xor edx, edx` then `mov eax, 2` then
                        // `ret`, which is the invalid hypercall code
                        // status returned in the pair the interface
                        // uses.
                        //
                        // These eight bytes assemble to themselves in
                        // both 32 and 64 bit code, so one page serves
                        // a caller in either mode. The obvious
                        // spelling - `mov rax, 2` and `ret` - does
                        // not: it is 64 bit only, and a 32 bit caller
                        // would execute its REX prefix as `dec eax`.
                        // Writing the mode independent form removes
                        // the question rather than answering it,
                        // which is worth more than the byte it costs.
                        constexpr std::uint8_t instructions[]{
                            0x31,
                            0xd2,
                            0xb8,
                            0x02,
                            0x00,
                            0x00,
                            0x00,
                            0xc3,
                        };

                        // With the enlightenment offered the page has to
                        // **trap** instead of answering locally, or the
                        // calls never reach this VMM and cannot be seen
                        // at all - which is why two boots showed the
                        // offer declined with nothing recorded about
                        // why. `vmcall` then `ret`: the exit handler
                        // gives the same refusal the bytes above give,
                        // and records the code on the way.
                        constexpr std::uint8_t trapping[]{
                            0x0f,
                            0x01,
                            0xc1,
                            0xc3,
                        };

                        // Through the mapping window, not through a
                        // store against the host page table.
                        //
                        // **This is what `hypercall_page_unwritable`
                        // counted 2 of.** `apply_guest_store` writes
                        // at the guest physical address as though it
                        // were a host virtual one, which works for
                        // the local APIC because that page is mapped
                        // here - and cannot work for a page of the
                        // guest's own memory, which this VMM
                        // deliberately never maps. The window is how
                        // guest memory is reached, and it takes the
                        // lock itself.
                        // Dynamic extent, because the two differ in
                        // length and a conditional over two fixed-extent
                        // spans has no common type.
                        std::span<const std::byte> page_bytes =
                            nested_vmx::evmcs_offered
                                ? std::span<const std::byte>(
                                      std::as_bytes(std::span{trapping}))
                                : std::span<const std::byte>(
                                      std::as_bytes(
                                          std::span{instructions}));

                        if (!write_guest_physical((value >> 12) << 12,
                                                  page_bytes)) {
                            this->hypercall_page_unwritable =
                                this->hypercall_page_unwritable + 1;
                        }
                    }
                    break;

                default:
                    answered = false;
                    break;
                }
            }

            if (answered) {
                this->synthetic_msr_accesses =
                    this->synthetic_msr_accesses + 1;
                break;
            }
        }

        inject_general_protection_fault();

        // The same index the log records below, in a form that can be
        // read out of a wedged guest through the emulator's monitor.
        if (auto slot = this->faulted_msr_count;
            slot < faulted_msr_capacity) {
            this->faulted_msrs[slot] = context.rcx;
            this->faulted_msr_count = slot + 1;
        }

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
    case basic_reason::task_switch: {
        // A task switch, which SDM 28.2 (.references/sdm.txt:200956)
        // makes unconditional: "Task switches are not allowed in VMX
        // non-root operation. Any attempt to effect a task switch in
        // VMX non-root operation causes a VM exit." No VM-execution
        // control turns it off, so any guest reaches it with a far
        // JMP or CALL through a TSS descriptor, an INT through a task
        // gate, or an IRET with RFLAGS.NT set.
        //
        // It had no case, so it reached `default:` and stopped the
        // processor - found by the same sweep as GETSEC and with a
        // lower bar to reach, since it needs no control register
        // write first. Not reachable from a long-mode guest, which is
        // why a Windows boot never found it: "hardware task switches
        // are not supported in IA-32e mode". But `unrestricted_guest`
        // is on and this VMM adopts application processors that start
        // in real mode, so "the guest is always in long mode" is not
        // a property this VMM has.
        //
        // #GP with the selector from the exit qualification, which is
        // the closest honest answer available. Emulating the switch
        // is what KVM does - `kvm_task_switch` reads and writes both
        // task-state segments - and is a large amount of code for a
        // mechanism no 64-bit operating system uses. Refusing it
        // instead reports a task switch that could not be performed,
        // which is what a guest gets from a malformed TSS descriptor
        // anyway, and it kills at most the guest rather than the
        // machine.
        //
        // SDM Table 28-5 puts the selector in bits 15:0 of the exit
        // qualification, and a #GP raised by a task switch carries
        // that selector as its error code.
        inject_general_protection_fault(vmcs.exit_qualification() &
                                        0xffff);
        advance_rip = false;

        log("cpu {} refused a task switch, selector {} rip {}",
            vmcs.vpid(),
            vmcs.exit_qualification() & 0xffff,
            vmcs.guest_rip());
        break;
    }
    case basic_reason::triple_fault: {
        // The guest has destroyed itself. SDM 28.2
        // (.references/sdm.txt:200927): a VM exit occurs "if the
        // logical processor encounters an exception while attempting
        // to call the double-fault handler". Unconditional, like the
        // task switch above.
        //
        // There is nothing to resume and nothing to inject: a guest
        // that faulted on its way into its own double-fault handler
        // has an interrupt descriptor table that cannot deliver, so
        // any exception put in would take the same path again. On
        // bare metal the processor resets the machine, which is not
        // something to do to somebody else's hardware because one
        // guest lost its IDT.
        //
        // So the processor stops - the same outcome `default:` gave.
        // What changes is *what it says*, and that is the whole point
        // of the case. `unhandled_exit` means "this VMM was asked
        // something it does not implement", and it is one of the two
        // records read out of a wedged machine. Reporting a guest's
        // own triple fault through it sends the next investigation
        // into this VMM's exit handler, looking for a missing case
        // that was never the problem.
        //
        // A second-level guest's triple fault does not reach here:
        // `l1_wants_l2_exit` reflects reason 2 unconditionally, as
        // KVM does in `nested_vmx_l1_wants_exit`
        // (.references/kvm/nested.c:6427), so a guest hypervisor is
        // told its own guest died and decides what to do about it.
        log("cpu {} guest triple faulted, rip {} cs {} - the guest "
            "took an exception calling its own double-fault "
            "handler, which is a guest failure and not an "
            "unimplemented exit",
            vmcs.vpid(),
            vmcs.guest_rip(),
            vmcs.guest_cs_selector());

        // Stopped through the same path, and the log line above is
        // what tells the two apart. A record of its own was
        // considered and not added: `unhandled_exit` is read by a
        // debugger attached to a processor that is already stopped,
        // and the log is what survives a restart and what CLAUDE.md
        // says to read first - so a second member would duplicate
        // the weaker half of the evidence.
        record_exit(full_reason, context);
        on_unhandled_exit(full_reason);
        break;
    }
    case basic_reason::getsec: {
        // Unreachable today, and a case anyway.
        //
        // GETSEC exits unconditionally in VMX non-root operation -
        // SDM 28.1.2 (.references/sdm.txt:200727): "An execution of
        // GETSEC in VMX non-root operation causes a VM exit if
        // CR4.SMXE[Bit 14] = 1 regardless of the value of CPL or
        // RAX". With that bit clear the instruction raises #UD in
        // hardware instead and no exit is taken at all, and the CR4
        // guest/host mask now keeps it clear for every guest - so on
        // a correctly built VMCS this case cannot execute.
        //
        // It exists because "cannot execute" was exactly the state of
        // affairs the day a guest could stop a processor with two
        // instructions. CPUID leaf 1 ECX[6] was cleared and CR4.SMXE
        // was not masked, so a guest that ignored the CPUID - or that
        // simply set the bit without asking - reached this exit, and
        // this exit reached `default:`, which does not resume.
        // CLAUDE.md states the rule the miss broke: nothing a guest
        // can execute may reach `default:`, and a case that faults is
        // always available and always better.
        //
        // #UD is the honest answer rather than an arbitrary one. It is
        // what a processor without SMX gives, it is what CPUID leaf 1
        // ECX[6] already told the guest to expect, and it is the same
        // answer the thirteen VMX instructions get from the case below
        // for the same reason (8412b76). Emulating GETSEC is not an
        // option worth weighing: its leaves enter an authenticated
        // code module and a measured launch environment, neither of
        // which survives a hypervisor underneath.
        //
        // RIP stays on the instruction, because a fault is reported at
        // the instruction that caused it.
        inject_invalid_opcode_exception();
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
            record_exit(full_reason, context);
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
        // Reachable because a guest/host mask is non-zero: a write
        // that would change a masked bit away from what the read
        // shadow says exits instead of landing in the register.
        // Today that is CR4.VMXE and CR0.NE.
        //
        // The guest is given what it asked for in the shadow, so a
        // read back agrees with its own write, while the real
        // register keeps VMXE - without which the next VM entry
        // fails, since a processor in root mode must have it set.
        constexpr std::uint64_t cr4_vmxe = 1ull << 13;
        constexpr std::uint64_t cr4_smxe = 1ull << 14;

        auto qualification = vmcs.exit_qualification();
        auto number = qualification & 0xf;
        auto access = (qualification >> 4) & 0x3;
        auto gpr = (qualification >> 8) & 0xf;

        // CR8, which reaches here only where the TPR shadow was not
        // handed to the processor and CR8 exiting was forced in its
        // place - `nested_vmx::tpr_shadow_offered`. Answered against the
        // guest hypervisor's own virtual-APIC page, which is where the
        // processor would have put it.
        //
        // Before the register test below rather than inside it, because
        // that test refuses everything but CR0 and CR4 and refusing means
        // stopping the processor.
        if (on_nested_cr8_access(
                vmcs.vpid() - 1, qualification, context, advance_rip)) {
            break;
        }

        // A MOV to CR0 or to CR4 can arrive here. Anything else
        // means a mask grew without this growing with it, and
        // guessing would resume the guest as though something had
        // worked.
        //
        // CR0 is accepted rather than refused because a guest writes
        // it - an application processor enables paging on its way up
        // - and CLAUDE.md's rule is that nothing a guest can execute
        // may reach an unhandled exit. This VMM owns no CR0 bit, so
        // the write is simply performed: the value goes to the guest
        // register and to the shadow, and the guest reads back what
        // it wrote.
        if (((0 != number) && (4 != number)) || (0 != access)) {
            record_exit(full_reason, context);
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

        // CR0, which this VMM owns no bit of, so the write is simply
        // performed.
        //
        // NE is forced on, and it is the architecture's requirement
        // rather than a policy: IA32_VMX_CR0_FIXED0 has it set, so a
        // guest CR0 without it fails VM entry. PE and PG are exempt
        // from the fixed bits because unrestricted guest is enabled,
        // which is what lets an application processor come up in real
        // mode and turn paging on here. apply_start_up states the same
        // rule from the other direction.
        //
        // The shadow gets what the guest wrote, unmodified, so a read
        // back agrees with the write even for the bit the register
        // keeps.
        if (0 == number) {
            vmcs.cr0_read_shadow(value);
            vmcs.guest_cr0(value | arch::x86_64::cr0_bits::numeric_error);
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
                   (cpu < max_cpus) && this->guest_in_vmx_operation[cpu] &&
                   (0 == (value & cr4_vmxe))) {
            inject_general_protection_fault();
            advance_rip = false;
            break;
        }

        // SMXE goes the other way from VMXE, and unconditionally:
        // it is refused in the shadow *and* in the register.
        //
        // CPUID leaf 1 ECX[6] is cleared for every build, so unlike
        // VMX there is no configuration in which the guest is
        // entitled to see this feature - and unlike VMXE nothing here
        // needs the bit set. Dropping it from both keeps the two
        // answers agreeing: the guest reads the CR4 it would have on
        // a processor without SMX, and GETSEC raises #UD there rather
        // than exiting to a handler.
        //
        // Silently rather than with a fault, which is the same choice
        // the CR0 case above makes: a write to a reserved or
        // unsupported CR4 bit is the guest's own to get wrong, and on
        // a processor without SMX it would #GP - but this VMM is not
        // emulating a processor without SMX, it is hiding one bit of
        // a processor that has it. Faulting would announce the
        // concealment.
        shadow &= ~cr4_smxe;

        vmcs.cr4_read_shadow(shadow);
        vmcs.guest_cr4((value | cr4_vmxe) & ~cr4_smxe);
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
            record_exit(full_reason, context);
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
            record_exit(full_reason, context);
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

        // And the profile, when the timer was armed for a second-level
        // guest. This is the only sample in the tree taken on a clock the
        // guest does not control - see `nested_vmx::profile_l2`.
        if constexpr (nested_vmx::profile_l2) {
            if (auto slot = vmcs.vpid(); (0 != slot) &&
                                         (slot <= max_cpus) &&
                                         this->running_l2[slot - 1]) {
                auto where = vmcs.guest_rip();
                record_profile_sample(where);
                record_profile_context(where, context);

                // And the stack, occasionally, from *here* rather than
                // from the thread sampler.
                //
                // The thread sampler runs on the second-level entry path,
                // which the guest reaches only by exiting - so its stack
                // is always the clock interrupt's, which is the one place
                // the guest is not stuck. This exit is on a clock the
                // guest does not control, so it lands wherever the guest
                // actually is, which at the stall is the configuration
                // read 35% of the time.
                //
                // Rate-limited hard: a scan is hundreds of guest reads
                // through two levels of translation and this fires six
                // thousand times a second.
                constexpr std::uint64_t stack_sample_period = 256;

                if (0 == (this->profile_samples % stack_sample_period)) {
                    sample_guest_stack(slot - 1);
                }
            }
        }

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
        // A hypercall, not a VMX instruction, whenever the
        // enlightenment is offered and the guest hypervisor has
        // installed a hypercall page.
        //
        // **The #UD below is right for the build it was written for and
        // wrong here.** With VMX hidden, VMCALL is an invalid opcode and
        // that is the architecturally correct answer. But a guest
        // hypervisor that has been told an interface exists and has
        // written HV_X64_MSR_HYPERCALL is making a hypercall, and
        // answering #UD to it is the "answered part of an interface"
        // failure this file records everywhere else.
        //
        // The answer is still a refusal - `HV_STATUS_INVALID_HYPERCALL_
        // CODE`, 2, exactly what the page returned when it answered
        // locally - because nothing is implemented and saying otherwise
        // is worse. What changes is that the call is now *seen*, and the
        // codes are recorded: this VMM offers the enlightened VMCS, the
        // guest hypervisor declines it, and which calls it makes before
        // declining is the question that decides what to implement next.
        if constexpr (nested_vmx::evmcs_offered) {
            constexpr std::uint64_t hypercall_page_enabled = 1;

            // **Only the guest hypervisor's own calls.** This case sees
            // exits from both levels, and a second-level guest's
            // hypercall belongs to the level above - it must be
            // reflected like every other second-level exit, not answered
            // here.
            //
            // Answering both is what made the trapping page reset-loop
            // while the local page, returning the *identical* status,
            // did not: every call Windows made to Hyper-V was
            // intercepted a layer too low and refused. Same answer,
            // different outcome, so the difference had to be structural.
            auto slot = vmcs.vpid();
            auto caller = ((0 != slot) && (slot <= max_cpus))
                              ? (slot - 1)
                              : max_cpus;
            auto from_guest_hypervisor =
                (caller < max_cpus) && !this->running_l2[caller];

            if (from_guest_hypervisor &&
                (0 != (this->hyperv_hypercall & hypercall_page_enabled))) {
                constexpr std::uint64_t invalid_hypercall_code = 2;

                auto code = context.rcx & 0xffff;

                this->hypercalls_seen = this->hypercalls_seen + 1;

                // Distinct codes with counts. Which, not how many.
                auto found = false;
                for (std::size_t i{}; i < hypercall_code_slots; ++i) {
                    if (this->hypercall_code_counts[i] &&
                        (this->hypercall_codes[i] == code)) {
                        this->hypercall_code_counts[i] =
                            this->hypercall_code_counts[i] + 1;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    for (std::size_t i{}; i < hypercall_code_slots; ++i) {
                        if (0 == this->hypercall_code_counts[i]) {
                            this->hypercall_codes[i] = code;
                            this->hypercall_code_counts[i] = 1;
                            break;
                        }
                    }
                }

                context.rax = invalid_hypercall_code;
                context.rdx = 0;
                break;
            }
        }

        if (on_vmx_instruction(full_reason, context)) {
            // Unless it was a VMLAUNCH or VMRESUME that settled where
            // RIP goes for itself, which is either of the two
            // outcomes that are not a VMfail: the second-level guest
            // is about to run and RIP is now its own, or its entry
            // failed after loading guest state and the guest
            // hypervisor has been put back at its own host RIP.
            // Advancing in either case moves a RIP somebody else owns.
            if constexpr (nested_vmx::enabled) {
                if (auto slot = vmcs.vpid();
                    (0 != slot) && (slot <= max_cpus) &&
                    this->nested_rip_settled[slot - 1]) {
                    this->nested_rip_settled[slot - 1] = false;
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
        if (0 == (vmcs.read(arch::x86_64::vmx::vmcs::field::
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
    // === Instructions a deployed build lets run ========================
    //
    // Ten exit reasons that arrive only in a ZPP_GUEST_TESTS build, where
    // `trap_the_quiet_instructions` in setup_vmcs requests the controls
    // that produce them. Their reason for existing is in that comment;
    // what matters here is that each of these **emulates the
    // instruction** rather than skipping it.
    //
    // That is not a nicety. Every case below breaks out to the resume
    // path, which advances RIP by the instruction's length - so a case
    // that did nothing would have the guest continue as though the
    // instruction had worked, which is the recurring failure this file
    // warns about everywhere else. A guest reading a timestamp that never
    // changes, or a random number that is always whatever was in the
    // register, is a guest that has been lied to.
    case basic_reason::hlt: {
        // Emulated as a no-op, so the guest polls instead of idling.
        //
        // This is what a VMM that intercepts HLT and wants the processor
        // to keep running does, and it is safe here for a reason that
        // would not hold generally: with the control off - every deployed
        // build - HLT is not intercepted at all and does what the guest
        // asked. A guest that genuinely needs to wait for an interrupt
        // therefore still waits; only the test build spins, and only
        // around the one HLT the coverage suite executes deliberately.
        break;
    }
    case basic_reason::pause: {
        // A hint with no architectural effect, so a no-op is the whole
        // of the emulation. SDM Vol. 2B, PAUSE: "improves the performance
        // of spin-wait loops ... the processor uses this hint" - nothing
        // observable follows from executing it.
        break;
    }
    case basic_reason::rdtsc: {
        // The host's timestamp counter, which is also the guest's: the
        // TSC offset field is zero and nothing here scales it, so there
        // is one clock and this hands it over unchanged.
        //
        // Both halves masked to thirty-two bits, because RDTSC writes
        // EAX and EDX - the upper halves of RAX and RDX are cleared, and
        // a guest reading a value with our high bits still in it would
        // see a clock that jumps.
        constexpr std::uint64_t low = 0xffffffffull;

        auto counter = arch::x86_64::rdtsc();
        context.rax = counter & low;
        context.rdx = (counter >> 32) & low;
        break;
    }
    case basic_reason::rdtscp: {
        // The same, plus IA32_TSC_AUX into ECX. SDM Vol. 2B, RDTSCP:
        // "reads the current value of the processor's time-stamp counter
        // into the EDX:EAX registers and also reads the value of the
        // IA32_TSC_AUX MSR into the ECX register".
        //
        // Reaching this needs two controls, which is why it is separate
        // from the case above rather than folded into it: "RDTSC exiting"
        // *and* the secondary "enable RDTSCP". The second is already on
        // in every build - without it the instruction raises #UD rather
        // than exiting - so the test build only adds the first.
        constexpr std::uint64_t low = 0xffffffffull;
        constexpr std::uint32_t ia32_tsc_aux = 0xc0000103;

        auto counter = arch::x86_64::rdtsc();
        context.rax = counter & low;
        context.rdx = (counter >> 32) & low;
        context.rcx = arch::x86_64::rdmsr(ia32_tsc_aux) & low;
        break;
    }
    case basic_reason::wbinvd: {
        // Executed, and that is the whole difference from INVD above.
        //
        // WBINVD writes modified cache lines back before invalidating
        // them, so carrying it out on a guest's behalf loses nothing -
        // where INVD discards them, which is why that case is a no-op and
        // says so at length. SDM Vol. 2B, WBINVD: "Writes back all
        // modified cache lines in the processor's internal cache to main
        // memory and invalidates the internal caches."
        arch::x86_64::wbinvd();
        break;
    }
    case basic_reason::rdpmc: {
        // Answered with a general-protection fault, which is what a
        // processor gives for a counter that does not exist.
        //
        // Nothing here virtualizes the performance counters, so no index
        // is valid, and #GP(0) is the architectural answer to an invalid
        // one - SDM Vol. 2B, RDPMC: "#GP(0) If the value in ECX specifies
        // a non-existent performance counter". KVM answers the same way:
        // `kvm_pmu_rdpmc` returning non-zero sends x86.c's emulator to
        // `kvm_inject_gp(vcpu, 0)`.
        //
        // A fault rather than a zero, deliberately. Handing back a
        // fabricated counter value is the "answer part of an interface"
        // mistake this file's header warns about: a guest would then
        // compute rates from a counter that never moves.
        inject_general_protection_fault();
        advance_rip = false;
        break;
    }
    case basic_reason::invlpg: {
        // Invalidated for the guest, on the guest's own tag.
        //
        // Executing `invlpg` here would invalidate a *host* linear
        // address, which is not what the guest asked and would leave its
        // stale translation in place. The guest's linear mappings are
        // tagged with its VPID - `enable_vpid` is on and setup_vmcs
        // writes one per processor - so the precise answer is INVVPID
        // type 0, individual-address, which SDM 31.4.3.1 defines as
        // invalidating "mappings for the linear address ... tagged with
        // the specified VPID".
        //
        // The linear address is the exit qualification, which SDM Table
        // 28-6 gives for this reason as the operand of the instruction.
        struct invvpid_descriptor
        {
            std::uint64_t vpid{};
            std::uint64_t linear_address{};
        };

        constexpr std::uint64_t invvpid_individual_address = 0;

        invvpid_descriptor descriptor{vmcs.vpid(),
                                      vmcs.exit_qualification()};

        if (arch::x86_64::vmx::invvpid(invvpid_individual_address,
                                       &descriptor)) {
            log("cpu {} invlpg: invvpid refused address {}",
                vmcs.vpid() - 1,
                vmcs.exit_qualification());
        }
        break;
    }
    case basic_reason::invpcid: {
        // **Reachable only as a side effect of INVLPG exiting, which is
        // the trap worth naming.** SDM Table 25-6 makes INVPCID exit when
        // "enable INVPCID" is 1 *and* "INVLPG exiting" is 1. The first is
        // requested by every build; the second is requested by the test
        // build alone - so turning INVLPG exiting on made a second exit
        // reason reachable, and without this case it would have reached
        // `default:` and stopped the processor.
        //
        // Emulated as a single-context invalidation of the guest's whole
        // VPID rather than by decoding the operand.
        //
        // That invalidates more than the guest asked for, and more is
        // always allowed: SDM 31.4.3.2 says an execution of INVVPID "may
        // invalidate mappings ... beyond those it is required to
        // invalidate", and the guidelines in 31.4.3.3 are about doing too
        // little rather than too much. What it buys is not needing the
        // memory operand at all - INVPCID's descriptor lives in guest
        // memory whose linear address only the VM-exit instruction
        // information field describes, and computing it is the piece the
        // descriptor-table instructions also want and none of them has
        // yet.
        //
        // The cost is a guest losing translations it had not asked to
        // lose, which costs it page walks and nothing else.
        struct invvpid_descriptor
        {
            std::uint64_t vpid{};
            std::uint64_t linear_address{};
        };

        constexpr std::uint64_t invvpid_single_context = 1;

        invvpid_descriptor descriptor{vmcs.vpid(), 0};

        if (arch::x86_64::vmx::invvpid(invvpid_single_context,
                                       &descriptor)) {
            log("cpu {} invpcid: invvpid refused vpid {}",
                vmcs.vpid() - 1,
                vmcs.vpid());
        }
        break;
    }
    case basic_reason::mov_debug_register: {
        // SDM Table 28-6, "Exit Qualification for MOV DR": bits 2:0 are
        // the debug register, bit 4 is the direction - 0 is MOV to DR, 1
        // is MOV from DR - and bits 11:8 are the general-purpose register.
        constexpr std::uint64_t debug_register_mask = 0x7;
        constexpr std::uint64_t direction_bit = 1ull << 4;
        constexpr std::uint64_t general_register_shift = 8;
        constexpr std::uint64_t general_register_mask = 0xf;

        auto qualification = vmcs.exit_qualification();
        auto number = qualification & debug_register_mask;
        auto reading = 0 != (qualification & direction_bit);
        auto general = static_cast<std::uint8_t>(
            (qualification >> general_register_shift) &
            general_register_mask);

        // The encoding order is not the context's layout, and
        // `register_of` is the only thing that knows the difference -
        // the same mapping the instruction decoder uses for a decoded
        // destination.
        auto & value = context.*arch::x86_64::register_of(general);

        // DR0 through DR3 and DR6 are *live in hardware* across a VM
        // exit: SDM 28.5.1 lists only DR7 among the registers a VM exit
        // changes, and the exit controls here save DR7 into the VMCS. So
        // the real registers still hold the guest's values and reading
        // them is reading the guest's, while DR7 has to come from the
        // field.
        //
        // DR4 and DR5 alias DR6 and DR7 when CR4.DE is clear and raise
        // #UD when it is set; the aliasing is what the masks below leave
        // in place, since the qualification only carries three bits.
        constexpr std::uint64_t debug_register_6 = 6;
        constexpr std::uint64_t debug_register_7 = 7;

        if (reading) {
            switch (number) {
            case debug_register_7:
                value =
                    vmcs.read(arch::x86_64::vmx::vmcs::field::guest_dr7);
                break;
            case debug_register_6:
                value = arch::x86_64::dr6();
                break;
            default:
                value = arch::x86_64::debug_register(
                    static_cast<std::uint8_t>(number));
                break;
            }
            break;
        }

        switch (number) {
        case debug_register_7:
            vmcs.write(arch::x86_64::vmx::vmcs::field::guest_dr7, value);
            break;
        case debug_register_6:
            arch::x86_64::dr6(value);
            break;
        default:
            arch::x86_64::debug_register(static_cast<std::uint8_t>(number),
                                         value);
            break;
        }
        break;
    }
    case basic_reason::rdrand:
    case basic_reason::rdseed: {
        // SDM Table 28-12, "Format of the VM-Exit Instruction-Information
        // Field (for RDRAND and RDSEED)": bits 6:3 are the destination
        // register and bits 12:11 the operand size - 0 is 16 bits, 1 is
        // 32 and 2 is 64.
        constexpr std::uint64_t destination_shift = 3;
        constexpr std::uint64_t destination_mask = 0xf;
        constexpr std::uint64_t operand_size_shift = 11;
        constexpr std::uint64_t operand_size_mask = 0x3;
        constexpr std::uint64_t operand_size_16 = 0;
        constexpr std::uint64_t operand_size_32 = 1;

        auto information = vmcs.read(arch::x86_64::vmx::vmcs::field::
                                         vm_exit_instruction_information);

        auto destination = static_cast<std::uint8_t>(
            (information >> destination_shift) & destination_mask);
        auto size =
            (information >> operand_size_shift) & operand_size_mask;

        // The host's own instruction, which is the only honest source: a
        // fabricated value handed to a guest asking for entropy is worse
        // than a refusal, and refusing is expressible - both instructions
        // report failure by clearing CF, which SDM Vol. 2B documents as
        // the "if the returned value is valid" flag.
        std::uint64_t random{};
        auto succeeded = (basic_reason::rdrand == reason)
                             ? arch::x86_64::rdrand(random)
                             : arch::x86_64::rdseed(random);

        auto & value = context.*arch::x86_64::register_of(destination);

        if (succeeded) {
            if (operand_size_16 == size) {
                // A 16-bit destination leaves the upper bits of the
                // register alone, unlike every wider form.
                constexpr std::uint64_t low_16 = 0xffffull;
                value = (value & ~low_16) | (random & low_16);
            } else if (operand_size_32 == size) {
                constexpr std::uint64_t low_32 = 0xffffffffull;
                value = random & low_32;
            } else {
                value = random;
            }
        } else {
            // SDM Vol. 2B, RDRAND: on failure "the returned value is 0"
            // and CF is cleared, so a guest that ignores the flag reads a
            // zero rather than a stale register.
            value = 0;
        }

        // The flags the instruction sets, written into the guest's saved
        // RFLAGS: CF says whether the value is valid, and SDM Vol. 2B
        // says OF, SF, ZF, AF and PF are cleared.
        constexpr std::uint64_t rflags_carry = 1ull << 0;
        constexpr std::uint64_t rflags_cleared_by_rdrand =
            (1ull << 2) | (1ull << 4) | (1ull << 6) | (1ull << 7) |
            (1ull << 11);

        auto flags = vmcs.guest_rflags() & ~rflags_cleared_by_rdrand;
        flags =
            succeeded ? (flags | rflags_carry) : (flags & ~rflags_carry);
        vmcs.guest_rflags(flags);
        break;
    }
    default: {
        // Everything reaching here exits unconditionally - there is
        // no VM execution control that turns it off - so arriving
        // means this VMM was asked something it does not implement.
        // Recorded and stopped on rather than resumed from, because
        // the resume below would advance RIP past an instruction that
        // never took effect.
        record_exit(full_reason, context);
        on_unhandled_exit(full_reason);
    }
    }

    resume_guest(context, full_reason, advance_rip);
}

} // namespace zpp::hypervisor
