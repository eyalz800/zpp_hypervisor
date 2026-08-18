// The way back into a guest.
//
// Four functions, and they are here rather than in hypervisor.cpp for the
// reason b6f0bee moved the local APIC path out: hypervisor.cpp reaches
// the whole VMM and nothing can compile it, so the only way to test
// anything in it was to cut the bodies out with awk at build time. A
// translation unit a harness can compile against a small stand-in header
// is what makes that extraction unnecessary - see tests/resume_guest,
// which compiles this file.
//
// Why these four and no others. `resume_guest` is the last thing every
// exit path calls, and `event_allowed_on_entry` is the predicate it asks
// before re-queueing an interrupted event. The other two are the whole of
// the external-interrupt queue behind `ZPP_VIRTUALIZE_APIC`:
// `queue_external_interrupt` is called from the exit handler and
// `deliver_pending_external_interrupt` from `resume_guest`, and they are
// here so a harness can drive the decision with the switch off - what the
// switch removes is the two call sites, not these bodies.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/diag/log.h"
#include "zpp/diag/pump.h"
#include "zpp/diag/sinks.h"
#include "zpp/diag/sinks/esp_blocks.h"
#include "zpp/hypervisor/hypervisor.h"
#include <bit>
#include <cstdint>
#include <utility>

namespace zpp::hypervisor
{
bool hypervisor::event_allowed_on_entry(std::uint64_t event) const
{
    // SDM 29.3.1.5, the activity-state and interruptibility-state checks
    // on VM entry (.references/sdm.txt:202612-202628). Transcribed as a
    // predicate rather than open-coded at the one call site, because it
    // is a property of the architecture and the next thing that wants to
    // inject has to ask the same question.
    //
    // Answering "no" is not an error. It means the guest is in a state
    // the event may not be delivered into *yet* - a processor waiting
    // for its start-up IPI, or one inside an STI shadow - and the caller
    // holds the event for a later entry.
    namespace activity = arch::x86_64::vmx::activity_state;

    constexpr std::uint64_t type_shift = 8;
    constexpr std::uint64_t type_mask = 7;
    constexpr std::uint64_t type_external_interrupt = 0;
    constexpr std::uint64_t type_nmi = 2;
    constexpr std::uint64_t type_hardware_exception = 3;
    constexpr std::uint64_t type_other_event = 7;
    constexpr std::uint64_t vector_mask = 0xff;
    constexpr std::uint64_t vector_debug = 1;
    constexpr std::uint64_t vector_machine_check = 18;
    constexpr std::uint64_t vector_pending_mtf = 0;
    constexpr std::uint64_t blocking_by_sti_or_mov_ss = 0x3;

    auto type = (event >> type_shift) & type_mask;
    auto vector = event & vector_mask;
    auto activity_state = this->vmcs.guest_activity_state();

    switch (activity_state) {
    case activity::active:
        // "Active. Any event is allowed."
        break;

    case activity::hlt:
        // "HLT. The only events allowed are ... external interrupt or
        // NMI ... hardware exception and vector 1 or 18 ... other event
        // and vector 0."
        if ((type_external_interrupt != type) && (type_nmi != type) &&
            !((type_hardware_exception == type) &&
              ((vector_debug == vector) ||
               (vector_machine_check == vector))) &&
            !((type_other_event == type) &&
              (vector_pending_mtf == vector))) {
            return false;
        }
        break;

    case activity::shutdown:
        // "Shutdown. Only NMIs and machine-check exceptions are
        // allowed."
        if ((type_nmi != type) && !((type_hardware_exception == type) &&
                                    (vector_machine_check == vector))) {
            return false;
        }
        break;

    case activity::wait_for_start_up_ipi:
        // "Wait-for-SIPI. No events are allowed."
        //
        // The caller drops rather than holds on this one, and the
        // distinction is not arbitrary: the only thing that parks a
        // processor in wait-for-SIPI is an INIT, and an INIT destroys
        // what was pending. KVM's `kvm_vcpu_reset` clears the exception
        // and interrupt queues on the same event. Holding here would
        // deliver, into a processor that has just been reset, an event
        // its previous life was owed.
        return false;

    default:
        // A state the architecture does not define. Refusing is the safe
        // answer: the alternative is an entry the processor rejects,
        // which produces no exit and stops the processor silently.
        return false;
    }

    // "Bit 0 (blocking by STI) and bit 1 (blocking by MOV-SS) must both
    // be 0 if the valid bit ... is 1 and the event type ... has value 0,
    // indicating external interrupt, or value 2, indicating
    // non-maskable interrupt."
    //
    // Note the type restriction, which is easy to lose: an exception may
    // be injected into an STI shadow, and only these two may not.
    if ((type_external_interrupt == type) || (type_nmi == type)) {
        if (0 != (this->vmcs.read(arch::x86_64::vmx::vmcs::field::
                                      guest_interruptibility_state) &
                  blocking_by_sti_or_mov_ss)) {
            return false;
        }
    }

    // And the check from the *other* section, which this predicate was
    // written without and which is the one that mattered.
    //
    // SDM 29.3.1.4 (`sdm.txt:202582`): "The IF flag (RFLAGS[bit 9]) must
    // be 1 if the valid bit (bit 31) in the injected-event
    // identification field is 1 and the event type (bits 10:8) is
    // external interrupt."
    //
    // Only external interrupts, and that is the difference from the pair
    // above: an NMI is delivered to a guest with interrupts disabled,
    // because that is what makes it non-maskable. Transcribing 29.3.1.5
    // and stopping there produced a predicate that looked complete and
    // let exactly one case through - the common one.
    //
    // What it cost is worth recording, because the failure is silent by
    // construction. A device interrupt whose delivery an exit
    // interrupted is re-queued into a guest that has since disabled
    // interrupts; the entry fails its guest-state checks; a failed entry
    // produces **no exit at all**; and `pending_event` has already been
    // cleared, so nothing retries it. The event is destroyed - which is
    // precisely what this whole path exists to prevent, arriving through
    // the path itself. On the rig it presents as Windows blocked for
    // ever on an I/O completion that never arrives, with the timer still
    // ticking at 97 Hz so the machine looks alive.
    //
    // KVM keeps both halves in one predicate, `__vmx_interrupt_blocked`
    // (.references/kvm/vmx.c:5071), which is the shape this should have
    // had.
    if (type_external_interrupt == type) {
        constexpr std::uint64_t rflags_interrupt_enable = 1ull << 9;

        if (0 == (this->vmcs.guest_rflags() & rflags_interrupt_enable)) {
            return false;
        }
    }

    return true;
}

void hypervisor::queue_external_interrupt(std::size_t cpu,
                                          std::uint64_t vector)
{
    if (cpu >= max_cpus) {
        return;
    }

    // Masked here rather than trusted from the caller, because the one
    // thing this function must not do is write outside the bitmap. The
    // exit-interruption field's vector is bits 7:0 (SDM 27.9.2), so the
    // mask is a no-op on the real path and a guard on any other.
    vector = vector & 0xff;

    auto & word = this->pending_external_vectors[cpu][vector / 64];
    auto bit = std::uint64_t{1} << (vector % 64);

    this->external_interrupts_taken[cpu] =
        this->external_interrupts_taken[cpu] + 1;

    if (0 != (word & bit)) {
        // The same vector acknowledged twice with nothing delivered in
        // between. A bitmap cannot represent two, so this is a real
        // loss - and it should be unreachable: the first acknowledge set
        // the in-service bit for that vector in the physical local APIC,
        // and SDM 12.8.4 has the APIC deliver only an interrupt of
        // *higher* priority while an in-service bit is set, which the
        // same vector is not. Counted rather than hidden because if it
        // ever fires, the model this whole path rests on is wrong.
        this->external_interrupts_dropped[cpu] =
            this->external_interrupts_dropped[cpu] + 1;
        return;
    }

    word = word | bit;

    auto pending = this->external_interrupts_pending[cpu] + 1;
    this->external_interrupts_pending[cpu] = pending;

    if (pending > this->external_interrupts_pending_high_water[cpu]) {
        this->external_interrupts_pending_high_water[cpu] = pending;
    }
}

bool hypervisor::deliver_pending_external_interrupt(std::size_t cpu)
{
    if (cpu >= max_cpus) {
        return false;
    }

    auto & vmcs = this->vmcs;

    constexpr std::uint64_t primary_interrupt_window = 1ull << 2;
    constexpr std::uint64_t valid = 1ull << 31;
    constexpr std::uint64_t type_external_interrupt = 0ull << 8;

    // Highest vector first. The local APIC's own delivery order is by
    // interrupt priority, which SDM 12.8.4 defines as the vector divided
    // by sixteen, with the higher vector winning inside a class - so
    // scanning down from the top word reproduces the order the hardware
    // would have used had these never been taken away from it.
    auto found = false;
    std::uint64_t vector = 0;

    for (auto index = external_vector_words; index-- > 0;) {
        if (auto word = this->pending_external_vectors[cpu][index];
            0 != word) {
            vector = (index * 64) + (std::bit_width(word) - 1);
            found = true;
            break;
        }
    }

    // Never into a second-level guest. The interrupt was signalled to
    // the *physical* processor, so in the two-level world it belongs to
    // the first-level guest - the guest hypervisor - and putting it
    // through vmcs02's entry-interruption field would deliver it to the
    // second-level guest's interrupt descriptor table instead. A guest
    // hypervisor that wanted it asked for external-interrupt exiting in
    // vmcs12, and that exit is reflected long before this runs; see
    // `l1_wants_l2_exit`.
    //
    // Held rather than dropped, and nothing here shortens the wait -
    // which is the known weakness of this path and why the counter
    // exists. KVM does force the exit, through `vmx_check_nested_events`
    // and `nested_vmx_vmexit`; that is not implemented here.
    //
    // And - this is the half that was missing - never *touching a
    // second-level guest's controls* either. This guard used to sit
    // below the not-found branch, so an exit taken
    // with vmcs02 current and nothing queued fell into that branch and
    // cleared the interrupt-window bit out of vmcs02. That bit is not
    // this VMM's: `build_vmcs02` composes the primary controls as
    // `(primary01 & ~(interrupt_window | nmi_window)) | primary12`, so an
    // interrupt-window bit in vmcs02 is one the *guest hypervisor* asked
    // for, and clearing it means its interrupt-window exit never arrives.
    //
    // Measured as a livelock: a guest hypervisor rewriting its primary
    // controls on 37% of its VMWRITEs and injecting on 0.1% of them, at
    // around 940 exits a second, indefinitely - arming a window over and
    // over that was removed under it every time. The rule was already
    // written down here, in the paragraph above, and obeyed by the
    // delivery path and by nothing else.
    if constexpr (nested_vmx::enabled) {
        if (this->running_l2[cpu]) {
            if (found) {
                this->external_interrupts_deferred_in_l2[cpu] =
                    this->external_interrupts_deferred_in_l2[cpu] + 1;
            }
            return false;
        }
    }

    // Nothing queued: make sure the window this may have armed on an
    // earlier entry is closed again, or the processor exits on every
    // instruction boundary the guest can take an interrupt at, for ever.
    // Only ever reached with vmcs01 current, by the guard above.
    if (!found) {
        auto primary =
            vmcs.primary_processor_based_vm_execution_controls();

        if (0 != (primary & primary_interrupt_window)) {
            vmcs.primary_processor_based_vm_execution_controls(
                arch::x86_64::vmx::adjust_msr(
                    this->cached_vmx_msr(
                        arch::x86_64::vmx::msr::
                            true_processor_based_controls),
                    primary & ~primary_interrupt_window));
        }
        return false;
    }

    auto event = valid | type_external_interrupt | vector;

    // Not on top of something else. The re-queue above may already have
    // staged the event whose delivery this exit interrupted, and that
    // one is owed to the guest from before this interrupt existed.
    auto staged = vmcs.read(arch::x86_64::vmx::vmcs::field::
                                vm_entry_interruption_information_field);

    auto deliverable =
        (0 == (staged & valid)) && event_allowed_on_entry(event);

    auto primary = vmcs.primary_processor_based_vm_execution_controls();
    auto wanted = deliverable ? (primary & ~primary_interrupt_window)
                              : (primary | primary_interrupt_window);

    if (wanted != primary) {
        vmcs.primary_processor_based_vm_execution_controls(
            arch::x86_64::vmx::adjust_msr(
                this->cached_vmx_msr(
                    arch::x86_64::vmx::msr::true_processor_based_controls),
                wanted));
    }

    if (!deliverable) {
        this->external_interrupts_deferred[cpu] =
            this->external_interrupts_deferred[cpu] + 1;
        return false;
    }

    vmcs.write(arch::x86_64::vmx::vmcs::field::
                   vm_entry_interruption_information_field,
               event);

    // A halted processor is put back into the active state, because the
    // interrupt is what ends the halt. SDM 29.3.1.5 permits injecting an
    // external interrupt while the activity state is HLT - that is what
    // `event_allowed_on_entry` just agreed to - and SDM 29.7.2
    // (`sdm.txt:203155`) says the entry is active afterwards regardless:
    // "If the VM entry is injecting, the logical processor is in the
    // active state after VM entry ... the contents of the activity-state
    // field do not determine the activity state after VM entry."
    //
    // So this write changes nothing about *this* entry. It is here so
    // that the field agrees with what the processor is about to do, for
    // the next exit and for anything reading the VMCS in between -
    // `resume_activity_state` is one such reader. KVM writes it for the
    // same reason in `vmx_clear_hlt` (`vmx.c:1817`). The HLT instruction
    // is not re-executed: RIP is already past it, which is how the
    // activity state came to be HLT at all.
    if (arch::x86_64::vmx::activity_state::hlt ==
        vmcs.guest_activity_state()) {
        vmcs.guest_activity_state(
            arch::x86_64::vmx::activity_state::active);
        this->external_interrupts_hlt_cleared[cpu] =
            this->external_interrupts_hlt_cleared[cpu] + 1;
    }

    this->pending_external_vectors[cpu][vector / 64] =
        this->pending_external_vectors[cpu][vector / 64] &
        ~(std::uint64_t{1} << (vector % 64));

    if (0 != this->external_interrupts_pending[cpu]) {
        this->external_interrupts_pending[cpu] =
            this->external_interrupts_pending[cpu] - 1;
    }

    this->external_interrupts_injected[cpu] =
        this->external_interrupts_injected[cpu] + 1;

    return true;
}

void hypervisor::apply_time_dilation(std::size_t cpu, std::uint64_t now)
{
    if constexpr (!nested_vmx::dilate_time) {
        // Not merely a cost: without the control set in vmcs01 the
        // field means nothing to the processor, and writing it would be
        // a VMWRITE to say so.
        (void)cpu;
        (void)now;
        return;
    } else {
        // Only the interval since the exit, and only the part of it the
        // guest is not to be charged for. `mark` is set at the top of
        // `on_vm_exit` and set here to `now`, so the span cannot be
        // counted twice if this is reached twice for one exit - which
        // the nested path does, since it calls `resume_guest` itself.
        //
        // Integer division truncates, so `root - root / n` is at least
        // `root * (n - 1) / n` and never exceeds `root`. The guest's
        // counter therefore advances by `root / n >= 0` across the
        // exit, which is the monotonicity property stated at
        // `ZPP_TIME_DILATION` - it is arithmetic here, not a check.
        auto mark = this->dilation_mark[cpu];

        if ((0 != mark) && (now > mark)) {
            auto root = now - mark;
            auto hidden = root - (root / nested_vmx::time_dilation);

            this->dilation_offset[cpu] =
                this->dilation_offset[cpu] - hidden;
            this->dilation_hidden[cpu] =
                this->dilation_hidden[cpu] + hidden;
            this->dilation_charged[cpu] =
                this->dilation_charged[cpu] + (root - hidden);
        }

        this->dilation_mark[cpu] = now;

        // Whichever VMCS the next instruction enters. vmcs02's field
        // carries both levels' offsets - `build_vmcs02` composes them
        // and leaves the guest hypervisor's half here - and vmcs01's
        // carries only this VMM's, since there is no level above it.
        this->vmcs.write(arch::x86_64::vmx::vmcs::field::tsc_offset,
                         this->running_l2[cpu]
                             ? (this->dilation_offset[cpu] +
                                this->tsc_offset_from_guest[cpu])
                             : this->dilation_offset[cpu]);
    }
}

void hypervisor::resume_guest(std::uint64_t cpuid,
                              arch::x86_64::context & context,
                              arch::x86_64::vmx::exit_reason full_reason,
                              bool advance_rip)
{
    auto & vmcs = this->vmcs;

    // Put back the event whose delivery the exit interrupted.
    //
    // The processor clears the entry-interruption field as it begins a
    // delivery, so an exit taken *during* one leaves nothing to resume
    // from: the only record is the interrupted-event field, and the next
    // entry overwrites that too. An event not put back here is destroyed
    // silently, and the guest that injected it cannot tell the difference
    // from one that arrived.
    //
    // Measured on the rig before this existed: nine events destroyed in a
    // single boot, every one of them interrupted by an EPT violation
    // against the second-level guest's lazily built shadow table - two
    // inter-processor interrupts at vector 0x2f, five clock interrupts at
    // 0xd1, and three page faults. The two at 0x2f are how a halted
    // virtual processor is woken, which is why the machine stopped with
    // every processor halted and nothing pending.
    //
    // KVM does the same thing in `vmx_complete_interrupts`
    // (.references/kvm/vmx.c:7488), which re-queues the vector, the error
    // code (:7142-7146) and the software-event length (:7139, :7149).
    //
    // Not done where the exit is reflected: there the interrupted event
    // is copied into the guest hypervisor's own VMCS and becomes its
    // business, and putting it back here as well would deliver it twice.
    //
    // SWITCHED OFF, and the switch is the whole point of this constant.
    // `git bisect` over seven rig boots, good `1975400`, bad `02c747e` -
    // the commit that added this - named it as the first commit at which
    // the guest hypervisor stops bringing up its application processors.
    // The verdict each boot was whether any of `cpu 0x1` .. `cpu 0x7`
    // reaches `guest vmxon` within about three minutes: seven of them do
    // at `1975400` and none does at `02c747e`, reproducibly, on the same
    // launcher and the same Windows installation.
    //
    // Two defects are visible by inspection and either could be it, so
    // neither is claimed as the cause without a measurement that
    // separates them:
    //
    // - the write below is unconditional, so it overwrites an
    //   entry-interruption field the exit handler had already staged for
    //   this entry - an injected fault, say - rather than yielding to it;
    // - `pending_event[cpu]` is cleared only when it is re-injected, so
    //   an event deferred because it belonged to the other level is held
    //   indefinitely and then delivered into some later unrelated entry.
    //
    // BOTH DEFECTS ARE FIXED AND IT IS BACK ON. See the two branches
    // below marked "defect 1" and "defect 2".
    //
    // What settled it was not inspection. The monitor trap flag armed on
    // every entry that injects `0xd1` - the vector the root partition's
    // `SINT3` carries - exits after one retired instruction, and on all
    // eight processors the first landing was an EPT violation with the
    // instruction pointer **unmoved**. Delivery reads the interrupt
    // descriptor table and pushes five words on the guest's stack before
    // reaching the handler, and both go through the extended page
    // tables, so it faults there against the lazily built shadow. With
    // this off the event was then destroyed, the handler never ran, the
    // end-of-message register was never written, and Hyper-V dropped the
    // timer and halted every processor behind it. BACKLOG.md carries the
    // whole chain.
    //
    // That is the same failure the paragraph above measured from the
    // other end - "five clock interrupts at 0xd1" - found again months
    // later by a different measurement.
    // Expressible as a build option, `-DZPP_REQUEUE_INTERRUPTED_EVENTS`,
    // for the same reason the launcher makes the processor count one:
    // "did this come from the re-queue" then costs one boot instead of an
    // argument. It defaulted to `false` for a long time and the comment
    // above says why; it is `true` now and BACKLOG.md says why.
    constexpr bool requeue_interrupted_events =
        (0 != ZPP_REQUEUE_INTERRUPTED_EVENTS);

    if constexpr (!requeue_interrupted_events) {
        (void)0;
    } else if (auto slot = (cpuid + 1);
               (0 != slot) && (slot <= max_cpus)) {
        auto cpu = slot - 1;
        constexpr std::uint64_t injection_valid = 1ull << 31;

        auto staged =
            vmcs.read(arch::x86_64::vmx::vmcs::field::
                          vm_entry_interruption_information_field);

        // Defect 1: yield to an event this exit's own handler staged.
        //
        // The write below used to be unconditional, so a re-queue
        // silently replaced a fault the handler had just decided to
        // inject - the general protection fault for a locked MSR, say.
        // The exit's own event wins: it is the processor's account of
        // what the guest just did, and delivering it is the whole reason
        // the exit was handled that way. The interrupted one stays held
        // and goes in on a later entry, which is what being held is for.
        //
        // The same test is already spelled twice in this file, at the
        // invalid-opcode injection and the MSR fault, both reading the
        // field's valid bit for exactly this reason.
        if (this->pending_event[cpu] &&
            (0 != (staged & injection_valid))) {
            this->events_yielded[cpu] = this->events_yielded[cpu] + 1;

            // Only into the guest it was being delivered to. A guest
            // hypervisor's VMLAUNCH is an exit like any other, so an exit
            // that interrupted a delivery to it can be followed straight
            // away by an entry into *its* guest - and putting the event
            // back there would hand one level's interrupt to the other.
            // Held instead until that guest runs again.
        } else if (this->pending_event[cpu] &&
                   (this->pending_event_l2[cpu] !=
                    this->running_l2[cpu])) {
            this->events_deferred[cpu] = this->events_deferred[cpu] + 1;

            // Defect 2: the guest it belonged to is gone.
            //
            // Being held across an entry into the other level is normal
            // and the branch above is what does it. Being held across a
            // VMPTRLD of a different region is not: that is a different
            // second-level guest, and the event is one its predecessor
            // was owed. Delivering it there is delivering a vector into
            // a guest that never had it pending - the worst shape of
            // this bug, because it arrives long after the exit that
            // produced it and nothing connects the two.
            //
            // Discarded rather than held for ever, and counted, because
            // an event whose guest is gone has no later entry to wait
            // for and a silent leak here would look exactly like the
            // destruction this whole path exists to prevent.
        } else if (this->pending_event[cpu] &&
                   this->pending_event_l2[cpu] &&
                   (this->pending_event_vmcs[cpu] !=
                    this->guest_current_vmcs[cpu])) {
            this->pending_event[cpu] = 0;
            this->events_discarded[cpu] = this->events_discarded[cpu] + 1;
            // Defect 3, which neither of the two named on the switch
            // covered and which is what the first re-run of this found:
            // **the entry state has to allow the event at all.**
            //
            // A re-queue writes the entry-interruption field after the
            // rest of the entry has been decided, so unlike an injection
            // the handler chose, nothing above it has checked the state
            // it is landing in. SDM 29.3.1.5 (`sdm.txt:202612-202621`)
            // makes three of those a consistency check, and a failed
            // check is a VM entry failure - which produces **no exit at
            // all** and is exactly as silent as the destruction this
            // path exists to prevent.
            //
            // - "Wait-for-SIPI. No events are allowed." This is the one
            //   that matters: an application processor waiting for its
            //   start-up IPI is precisely a processor in that state, and
            //   re-queueing into it fails every entry. Turning this path
            //   on without the check reproduced the original regression
            //   exactly - one processor of eight reaching `guest vmxon`,
            //   which is the same verdict `git bisect` recorded against
            //   `02c747e` over seven boots.
            // - "Shutdown. Only NMIs and machine-check exceptions are
            //   allowed."
            // - "HLT. The only events allowed are ... external interrupt
            //   or NMI ... hardware exception and vector 1 or 18 ...
            //   other event and vector 0."
            //
            // And SDM 29.3.1.5 again (`sdm.txt:202627`): blocking by STI
            // and by MOV SS must both be 0 when the injected event is an
            // external interrupt or an NMI.
            //
            // Held rather than forced in every case. Clearing the
            // interruptibility bits would also satisfy the processor -
            // SDM 27.3.1.5 (`sdm.txt:203115`) says an injecting entry
            // leaves no such blocking regardless, and KVM's
            // `vmx_clear_interrupt_shadow` does exactly that - but that
            // is guest state this VMM did not write, and holding costs
            // only a later entry. An STI shadow lasts one instruction.
            //
            // Held, not dropped - with one exception, below. The state
            // that refuses the event is the state the guest is in now,
            // and the reason to keep the event is that the guest will
            // leave it.
            //
            // Wait-for-SIPI is the exception, and it is dropped rather
            // than held. The only thing that parks a processor there is
            // an INIT, and an INIT **destroys what was pending** - KVM's
            // `kvm_vcpu_reset` clears the exception and interrupt queues
            // on the same event. Holding would deliver, into a processor
            // that has just been reset, an event its previous life was
            // owed.
        } else if (this->pending_event[cpu] &&
                   !event_allowed_on_entry(this->pending_event[cpu])) {
            this->events_refused_by_state[cpu] =
                this->events_refused_by_state[cpu] + 1;

            if (arch::x86_64::vmx::activity_state::wait_for_start_up_ipi ==
                vmcs.guest_activity_state()) {
                this->pending_event[cpu] = 0;
                this->events_discarded[cpu] =
                    this->events_discarded[cpu] + 1;
            }
        } else if (auto event = this->pending_event[cpu]; 0 != event) {
            constexpr std::uint64_t error_valid = 1ull << 11;
            constexpr std::uint64_t type_mask = 7ull << 8;
            constexpr std::uint64_t type_software_interrupt = 4ull << 8;
            constexpr std::uint64_t type_privileged_software = 5ull << 8;
            constexpr std::uint64_t type_software_exception = 6ull << 8;
            constexpr std::uint64_t type_nmi = 2ull << 8;

            if (0 != (event & error_valid)) {
                vmcs.write(arch::x86_64::vmx::vmcs::field::
                               vm_entry_exception_error_code,
                           this->pending_event_error[cpu]);
            }

            // A software event resumes at the instruction after the one
            // that raised it, so the processor has to be told how long
            // that instruction was. A hardware one ignores the field.
            if (auto type = event & type_mask;
                (type_software_interrupt == type) ||
                (type_privileged_software == type) ||
                (type_software_exception == type)) {
                vmcs.write(arch::x86_64::vmx::vmcs::field::
                               vm_entry_instruction_length,
                           this->pending_event_length[cpu]);
            }

            // An NMI put back has to be put back into a state that
            // accepts it. SDM 29.3.1.5: blocking by NMI must be 0 when
            // the injected event is an NMI and the "virtual NMIs"
            // VM-execution control is 1. This VMM does not ask for that
            // control, but `build_vmcs02` merges it from a guest
            // hypervisor that does - so the entry it would refuse is a
            // second-level one, and refusing it produces no exit at all.
            //
            // Cleared rather than held, unlike the STI shadow above, and
            // for a reason that does not apply there: the blocking is
            // the architecture's record that an NMI is *in progress*,
            // and the NMI being put back is that same one. Waiting for
            // it to clear is waiting for the IRET of a handler that
            // never ran. KVM clears it unconditionally on this path,
            // `vmx.c:7130`.
            if (type_nmi == (event & type_mask)) {
                vmcs.guest_interruptibility_state(
                    vmcs.guest_interruptibility_state() &
                    ~arch::x86_64::vmx::interruptibility_state::
                        blocking_by_nmi);
            }

            // Masked, not copied. The interrupted-event field this was
            // read from carries bit 13, "nested exception", which SDM
            // 29.2.1.3 makes legal in the *entry* field only on a
            // processor enumerating FRED. Copying the word verbatim
            // hands the processor a reserved bit and the entry is
            // refused - silently, like every other failed check here.
            vmcs.write(arch::x86_64::vmx::vmcs::field::
                           vm_entry_interruption_information_field,
                       event & arch::x86_64::vmx::vm_entry_interruption::
                                   defined_bits);

            this->pending_event[cpu] = 0;
            this->events_requeued[cpu] = this->events_requeued[cpu] + 1;
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
        auto cpu = (cpuid + 1);
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
    record_exit(full_reason, context);

    // Counted here, at the last point before control leaves this
    // handler, so a frozen exit count can be read two ways round.
    if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
        this->resumes_reached[slot - 1] =
            this->resumes_reached[slot - 1] + 1;

        // The activity state only from this VMM's own VMCS, because it is
        // consumed rather than merely read: start_up_processor decides
        // whether to deliver a guest's start-up IPI by it, and a
        // second-level guest's activity state answers a different
        // question about a different virtual processor. Left at vmcs01's
        // last value while one runs, which is the first-level guest's
        // state and is what the guest hypervisor's own processor is
        // actually in. A second-level guest waiting for a start-up IPI is
        // held by enter_or_park_l2 and recorded in `l2_activity_state`,
        // which is the record start_up_processor asks alongside this one.
        //
        // RIP and CS are not gated, deliberately: nothing consumes them,
        // they exist to be read from a debugger, and the address a
        // second-level guest is at is the more useful of the two answers.
        auto in_l2 = false;
        if constexpr (nested_vmx::enabled) {
            in_l2 = this->running_l2[slot - 1];
        }

        if (!in_l2) {
            this->resume_activity_state[slot - 1] =
                vmcs.guest_activity_state();
        }

        this->resume_guest_rip[slot - 1] = vmcs.guest_rip();
        this->resume_guest_cs[slot - 1] = vmcs.guest_cs_selector();
    }

    // Whether this processor has been out of VMX operation and back
    // since the last entry, which only the sleep quiesce does. Its
    // return leaves the launch state clear, and VMRESUME requires
    // launched (SDM 27.1) - so that one case has to leave through
    // VMLAUNCH instead. Consumed here, so the next exit resumes.
    auto relaunch = false;
    if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
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
        if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus) &&
                                     this->running_l2[slot - 1]) {
            entry = this->vmcs02_launched[slot - 1]
                        ? arch::x86_64::vmx::nested_vmresume
                        : arch::x86_64::vmx::nested_vmlaunch;

            // What the entry actually carries, read here because here
            // is the last instant it can still change. See
            // `l2_entry_vector`: the guest hypervisor is injecting and
            // the second-level guest is not vectoring, and a successful
            // entry holding a valid injection has no third option.
            //
            // A vmread on the entry path is not free, and it is worth
            // it: every other counter in this investigation observes
            // where an event was *put*, and none of them observes
            // whether it was still there.
            constexpr std::uint64_t injection_valid = 1ull << 31;
            constexpr std::uint64_t vector_mask = 0xff;

            auto carried =
                vmcs.read(arch::x86_64::vmx::vmcs::field::
                              vm_entry_interruption_information_field);

            auto cpu = slot - 1;

            // What the guest resumes with. See `l2_resume_rip`: the
            // guest hypervisor has answered by now, the instruction
            // pointer is past the RDMSR, and the value it produced is
            // in RAX and RDX.
            //
            // Monotonicity is checked against the previous sample from
            // the *same* instruction pointer, because that is what
            // makes two samples comparable - a different caller is
            // reading a different thing and proves nothing about this
            // one.
            auto guest_rip =
                vmcs.read(arch::x86_64::vmx::vmcs::field::guest_rip);
            auto value = ((context.rdx & 0xffffffff) << 32) |
                         (context.rax & 0xffffffff);

            auto slot_index =
                this->l2_resume_count[cpu] % l2_resume_sample_capacity;
            this->l2_resume_rip[cpu][slot_index] = guest_rip;
            this->l2_resume_value[cpu][slot_index] = value;
            this->l2_resume_count[cpu] = this->l2_resume_count[cpu] + 1;

            auto previous_index = (this->l2_resume_count[cpu] +
                                   l2_resume_sample_capacity - 2) %
                                  l2_resume_sample_capacity;

            if ((this->l2_resume_count[cpu] > 1) &&
                (this->l2_resume_rip[cpu][previous_index] == guest_rip) &&
                (value < this->reference_count_previous[cpu])) {
                this->reference_count_backwards[cpu] =
                    this->reference_count_backwards[cpu] + 1;
            }
            this->reference_count_previous[cpu] = value;

            if (0 != (carried & injection_valid)) {
                auto vector = carried & vector_mask;
                this->l2_entry_vector[cpu][vector] =
                    this->l2_entry_vector[cpu][vector] + 1;
            } else {
                this->l2_entries_carrying_nothing[cpu] =
                    this->l2_entries_carrying_nothing[cpu] + 1;
            }
        }
    }

    // Close the span opened at the top of `on_vm_exit`. Here rather
    // than anywhere earlier because everything this VMM does for an exit
    // has now been done, and the next instruction is the entry itself.
    if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
        auto cpu = slot - 1;
        auto now = arch::x86_64::rdtsc();
        if (0 != this->handler_entry_tsc[cpu]) {
            this->handler_cycles[cpu] =
                this->handler_cycles[cpu] +
                (now - this->handler_entry_tsc[cpu]);
            this->handler_exits[cpu] = this->handler_exits[cpu] + 1;
        }
        this->handler_last_tsc[cpu] = now;

        // Opens the span the *guest* runs in, closed at the top of
        // `on_vm_exit`. `running_l2` is already set for the entry about
        // to happen, so it names which level this span belongs to.
        this->level_run_tsc[cpu] = now;
        this->level_run_was_l2[cpu] = this->running_l2[cpu];

        // And the clock the guest itself will read. Here for the same
        // reason the span above is here - the next instruction is the
        // entry, so this is the last moment root operation owns - and
        // it needs `running_l2` for the same reason too, to know which
        // VMCS the offset is going into.
        apply_time_dilation(cpu, now);
    }

    // Put back an external interrupt this VMM took on the guest's
    // behalf. Only with ZPP_VIRTUALIZE_APIC; otherwise nothing ever
    // queues one and this is a call that is not compiled at all.
    //
    // The decision is `deliver_pending_external_interrupt`'s and not
    // spelled out here, so that `tests/resume_guest` can drive it
    // without the switch: what the switch removes is this call.
#ifndef ZPP_VIRTUALIZE_APIC
#define ZPP_VIRTUALIZE_APIC 0
#endif
    if constexpr (0 != ZPP_VIRTUALIZE_APIC) {
        if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
            deliver_pending_external_interrupt(slot - 1);
        }
    }

    // The mirror of the launch: the guest's registers are put back
    // and the last thing executed in host mode is the resume itself.
    context.rip = reinterpret_cast<std::uint64_t>(entry);
    arch::x86_64::restore_context(&context);
    std::unreachable();
}
} // namespace zpp::hypervisor
