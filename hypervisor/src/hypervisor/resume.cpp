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
bool hypervisor::event_allowed_on_entry(
    std::uint64_t event, std::uint64_t activity_state) const
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

    // The state comes from the caller. It was a VMREAD here, and
    // `resume_guest` asks this twice on the re-queue path and then reads
    // the same field again itself - three reads of one value that
    // nothing between them writes, at 1.4-1.8 microseconds each on a
    // host with no VMCS shadowing.
    auto type = (event >> type_shift) & type_mask;
    auto vector = event & vector_mask;

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
        (0 == (staged & valid)) &&
        event_allowed_on_entry(event, vmcs.guest_activity_state());

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

void hypervisor::emit_disk_telemetry(std::size_t cpu)
{
    if constexpr (!diag::enabled) {
        return;
    }

    if (cpu >= max_cpus) {
        return;
    }

    auto now = arch::x86_64::rdtsc();

    if ((now - this->telemetry_last_tsc[cpu]) < telemetry_period_cycles) {
        return;
    }

    this->telemetry_last_tsc[cpu] = now;

    // Named `tlm` and fixed in field order so the reader can be a regular
    // expression rather than a parser. The sequence is first because a
    // gap in it is the only thing that distinguishes a lost block from a
    // guest that stopped moving.
    //
    // The values are plain loads of counters this VMM already maintains -
    // no VMREAD, nothing that costs an exit - which matters because
    // zpp/diag/log.h warns that arguments are evaluated even when the
    // facility is off.
    constexpr std::size_t dispatch_vector = 0x2f;
    constexpr std::size_t clock_vector = 0xd1;
    constexpr std::size_t ept_violation = 48;

    diag::log<diag::severity::info>(
        "tlm seq {} 2f {} p00 {} p10 {} ept {} vtl {} clk {}",
        this->telemetry_sequence[cpu]++,
        this->l2_injected_vector[cpu][dispatch_vector],
        this->l2_entry_vtpr[cpu][0x00],
        this->l2_entry_vtpr[cpu][0x10],
        this->exit_reason_counts[cpu][ept_violation],
        this->vtl_switches[cpu][0],
        this->l2_injected_vector[cpu][clock_vector]);
}

void hypervisor::note_pending_vector(std::size_t cpu, std::uint64_t staged)
{
    if constexpr (!nested_vmx::count_dropped_requests) {
        (void)cpu;
        (void)staged;
        return;
    } else {
        if (cpu >= max_cpus) {
            return;
        }

        constexpr std::uint64_t injection_valid = 1ull << 31;
        constexpr std::uint64_t vector_mask = 0xff;
        constexpr std::uint64_t priority_class = 4;
        constexpr std::uint64_t interrupt_enable = 1ull << 9;
        constexpr std::uint64_t blocking_sti_or_mov_ss = 0x3;
        constexpr std::uint64_t virtual_task_priority = 0x80;
        constexpr std::uint64_t primary_tpr_shadow = 1ull << 21;
        constexpr std::uint64_t primary_interrupt_window = 1ull << 2;

        using field = arch::x86_64::vmx::vmcs::field;

        auto & vmcs = this->vmcs;

        // Proof of life. Without it "dropped 0" cannot be told from a
        // binary built with the switch off, and this project has read
        // an all-zero counter as a fact about the machine three times.
        this->pending_vector_instrument_entries[cpu] += 1;

        auto vector = std::uint64_t{this->pending_vector_now[cpu]};

        if (0 == vector) {
            return;
        }

        this->pending_vector_entries_pending[cpu] += 1;

        if ((0 != (staged & injection_valid)) &&
            (vector == (staged & vector_mask))) {
            // Delivered, and retired only here. An entry that carries
            // it is the only evidence it arrived; every other point on
            // the path is a claim about where it was put.
            this->pending_vector_now[cpu] = 0;
            this->pending_vector_drop_marked[cpu] = false;
            this->pending_vector_delivered[cpu] += 1;
            return;
        }

        // Not delivered. Three reasons, and only one is a fault -
        // which is why this is four counters rather than one.
        std::uint8_t vtpr{};
        auto page = this->nested_virtual_apic_address[cpu];

        auto read = (0 != page) &&
                    read_guest_physical(
                        page + virtual_task_priority,
                        std::as_writable_bytes(std::span(&vtpr, 1)));

        if (!read) {
            // No page, no priority, no verdict. Counted apart so an
            // unreadable virtual-APIC page cannot masquerade as either
            // a drop or a legitimate block.
            this->pending_vector_unreadable[cpu] += 1;
            return;
        }

        auto blocking = vmcs.read(field::guest_interruptibility_state);

        auto interruptible =
            (0 != (vmcs.guest_rflags() & interrupt_enable)) &&
            (0 == (blocking & blocking_sti_or_mov_ss));

        // SDM 12.8.4: admitted only where the vector's priority class
        // is **strictly greater** than the task priority's.
        auto admitted = (vector >> priority_class) >
                        (std::uint64_t{vtpr} >> priority_class);

        if (!admitted || !interruptible) {
            // Correct behaviour. The guest is at a priority that
            // refuses the vector, or has interrupts off, and nothing
            // is owed. Keeping this apart from the fault below is the
            // whole reason a single "not delivered" counter would have
            // been useless.
            this->pending_vector_blocked[cpu] += 1;
            return;
        }

        // Anything that could still produce the exit at which the
        // level above would stage it. The window is its own mechanism;
        // the threshold is this VMM's, and `window_threshold_armed` is
        // the only place that is ever set.
        auto primary =
            vmcs.primary_processor_based_vm_execution_controls();

        auto armed = (0 != (primary & primary_interrupt_window)) ||
                     ((0 != (primary & primary_tpr_shadow)) &&
                      this->window_threshold_armed[cpu]);

        if (armed) {
            // Counted, so the populations sum. This was the one branch
            // that returned silently, and its size could only be got
            // by subtracting the other four from
            // `instrument_entries` - which is how a residual ends up
            // attributed to whichever population somebody is arguing
            // for.
            this->pending_vector_window_already_armed[cpu] += 1;
            return;
        }

        // **The fault.** The guest can take it, it is not staged, and
        // nothing in vmcs02 will cause an exit at which it could be.
        // The request is not deferred - it is lost until some
        // unrelated exit happens to be reflected.
        //
        // Counted twice on purpose, per request and per entry: one
        // request abandoned for a million entries and a million
        // requests each abandoned once are different faults and a
        // single counter cannot tell them apart.
        this->pending_vector_drop_moments[cpu] += 1;

        if (!this->pending_vector_drop_marked[cpu]) {
            this->pending_vector_drop_marked[cpu] = true;
            this->pending_vector_dropped[cpu] += 1;
        }
    }
}

void hypervisor::resume_guest(std::uint64_t cpuid,
                              arch::x86_64::context & context,
                              arch::x86_64::vmx::exit_reason full_reason,
                              bool advance_rip)
{
    auto & vmcs = this->vmcs;

    // Closes the dispatch. Everything between the mark taken in
    // `on_vm_exit` and here is whatever handled the exit - the
    // reflection, the vmcs02 build, the extended page-table fault, or
    // one of the ordinary cases in the switch - and every older phase
    // slot nests inside this one. See `phase_mark`.
    //
    // This function is `[[noreturn]]` and has exactly two call sites,
    // both in `on_vm_exit`, which is what makes the boundary sound: no
    // exit reaches the guest without passing through here once.
    mark_phase(cpuid, 26);

    // Every resume, and what it is resuming to. See `resume_count`:
    // this is the counterpart to the launch-path trace, which fires
    // once per processor and cannot report a missing ordinary resume.
    if (cpuid < max_cpus) {
        this->resume_count[cpuid] = this->resume_count[cpuid] + 1;
        this->last_resume_rip[cpuid] = vmcs.guest_rip();

        // **Which VMCS is current, asked on the processor itself.**
        // Only for the handful of resumes after a start-up, and only on
        // application processors, so the boot processor's half-million
        // exits pay nothing.
        if (0 != this->vmptrst_owed[cpuid]) {
            this->vmptrst_owed[cpuid] = this->vmptrst_owed[cpuid] - 1;

            alignas(16) std::uint64_t current{};

            if (arch::x86_64::vmx::vmptrst(&current)) {
                log("cpu {} vmptrst FAILED at resume rip {}",
                    cpuid,
                    vmcs.guest_rip());
            } else {
                // The mask **at entry**, not at start-up time. Every
                // reading of it so far was taken in `apply_start_up`,
                // and everything between there and here - the nested
                // entry path included - is unexamined. It is a VMCS
                // control field, and if it is rewritten on the way in
                // then the CR0 writes that "must trap" simply do not
                // have to.
                // **Re-write the mask, then read it back.**
                //
                // cpu 1's own CR0 writes DID exit earlier in the boot -
                // two `cr-access` exits at firmware RIPs `0x7ef5a28f`
                // and `0x7f39f05a` - and stop exiting after the second
                // INIT-SIPI. So the mask works on this processor and
                // then ceases to, which is a much sharper statement
                // than "it never worked".
                //
                // This machine is nested: what this VMM writes is a
                // vmcs12 that the layer below merges into the vmcs02 it
                // actually runs. If that merge is stale for a processor
                // it has just handled an INIT-SIPI for, re-writing the
                // field here marks it dirty again and the merge must
                // redo it. Same value, so on correct hardware this is a
                // no-op and costs one VMWRITE on six entries per
                // processor per boot.
                //
                // Diagnostic and candidate fix at once: if the CR0
                // writes start exiting after this, the mask was not
                // reaching the hardware and that is the bug.
                vmcs.cr0_guest_host_mask(vmcs.cr0_guest_host_mask());

                log("cpu {} vmptrst {} at resume rip {} cr0mask {} "
                    "entryctl {}",
                    cpuid,
                    current,
                    vmcs.guest_rip(),
                    vmcs.cr0_guest_host_mask(),
                    vmcs.vm_entry_controls());
            }
        }
    }

    // The activity state, read at most once and only where something
    // asks for it.
    //
    // Three separate readers wanted it - `event_allowed_on_entry`, the
    // wait-for-SIPI test beside it and `resume_activity_state` at the end
    // - and nothing between them writes the field. The census over our
    // own reads put `guest_activity_state` at 7.9 per round trip and this
    // function is where most of them were.
    //
    // Memoised rather than read up front, because the common exit reaches
    // none of the three and would then pay for a field it never looks at.
    // `deliver_pending_external_interrupt` does write the field - it
    // clears HLT when it injects - and runs after the last reader here.
    std::uint64_t activity_state{};
    auto activity_state_read = false;
    auto activity_now = [&] {
        if (!activity_state_read) {
            activity_state = vmcs.guest_activity_state();
            activity_state_read = true;
        }
        return activity_state;
    };

    // And the entry-interruption field, which this function reads,
    // writes, and then used to read back.
    //
    // The read-back is the cheapest class of redundant access there is:
    // the value is whatever the handlers left, unless the re-queue below
    // replaced it, and this frame is the only thing that could have.
    std::uint64_t entry_event{};
    auto entry_event_read = false;
    auto entry_event_now = [&] {
        if (!entry_event_read) {
            using field = arch::x86_64::vmx::vmcs::field;
            entry_event = vmcs.read(
                field::vm_entry_interruption_information_field);
            entry_event_read = true;
        }
        return entry_event;
    };

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

        auto staged = entry_event_now();

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
                   !event_allowed_on_entry(this->pending_event[cpu],
                                           activity_now())) {
            this->events_refused_by_state[cpu] =
                this->events_refused_by_state[cpu] + 1;

            if (arch::x86_64::vmx::activity_state::wait_for_start_up_ipi ==
                activity_now()) {
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
            // Kept, because the read-back at the entry census below
            // wants exactly this and the processor has no other account
            // of it that we did not just supply.
            entry_event = event & arch::x86_64::vmx::
                                      vm_entry_interruption::defined_bits;
            entry_event_read = true;

            vmcs.write(arch::x86_64::vmx::vmcs::field::
                           vm_entry_interruption_information_field,
                       entry_event);

            this->pending_event[cpu] = 0;
            this->events_requeued[cpu] = this->events_requeued[cpu] + 1;
        }
    }

    // The event decision, closed. Everything above is the re-queue: the
    // interruptibility state, the error code and length writes, and the
    // entry-interruption write itself.
    mark_phase(cpuid, 27);

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
    // Before the pump, so a record produced now leaves on this pass
    // rather than waiting for the next.
    emit_disk_telemetry(cpuid);

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
    // The lazy tick's wake-up for the **first** level.
    //
    // `build_vmcs02` arms this timer for the second-level guest, and
    // that is only half of it. Measured 2026-08-28: with a gap set and
    // no timer here, the machine froze with `exit_total` unchanged
    // across two minutes, `info status` running, the vCPU thread
    // burning 100% of a core in system time, and `RIP` identical over
    // three samples at an address in the **first** level - the same
    // page as the exit ring's `l1-rip`. Hyper-V was spinning in its own
    // code, taking no exits, and the vmcs02 timer cannot fire there
    // because that VMCS is not current while the first level runs.
    //
    // So the gap starves the level above as well as the guest, and the
    // wake-up has to cover whichever level is running. This call is the
    // existing one; it skips while `running_l2` is set, which is
    // exactly right now that vmcs02 carries its own, and it checks the
    // capability MSR before setting the control - arming one the outer
    // hypervisor does not offer wedged this rig once already.
    if constexpr ((nested_vmx::poll_l1 ||
                   (0 != nested_vmx::lazy_tick_microseconds)) &&
                  !diag::policy_of(diag::sink::esp_blocks).present) {
        arm_controller_poll(cpuid, true);
    }

    if constexpr (diag::policy_of(diag::sink::esp_blocks).present) {
        arm_controller_poll(cpuid, diag::esp_block_sink::ready());
    }

    // The liveness probe, driven by whichever processor is still
    // exiting.
    //
    // It has to hang off an exit path because there is no other clock
    // here - and that is not the limitation it looks like. A processor
    // that has stopped exiting cannot drive anything, which is the whole
    // premise; a processor that is still exiting is exactly the one in a
    // position to ask about the others. With every processor silent
    // nothing probes anything, and nothing could have.
    if constexpr (nested_vmx::probe_aps) {
        if (auto cpu = (cpuid + 1); (0 != cpu) && (cpu <= max_cpus)) {
            auto & seen = this->probe_exits_seen[cpu - 1];
            if (0 == (++seen % nested_vmx::probe_ap_exits)) {
                probe_application_processors(cpu - 1);
            }
        }
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
    //
    // The value is kept, because two things below want it and both used
    // to read it back out of the VMCS. `guest_rip` was 18.6 reads per
    // round trip in the census over our own reads, the largest single
    // field, and every read of it is an exit to the layer below at
    // 1.4-1.8 microseconds on a host with no VMCS shadowing.
    //
    // Two branches rather than one, and the difference matters. Where
    // RIP was advanced the value was just written, so reading it back
    // asks the processor a question this frame answered. Where it was
    // not, `context.rip` is **not** a substitute: a reflected exit
    // reaches here with vmcs01 current and its guest RIP holding the
    // guest hypervisor's host entry point, while `context.rip` still
    // holds the second-level guest's - see `resume_guest_rip`, which is
    // documented as recording the former.
    std::uint64_t resume_rip{};
    if (advance_rip) {
        auto rip_before = context.rip;
        auto advanced_by = vmcs.vm_exit_instruction_length();

        context.rip += advanced_by;
        vmcs.guest_rip(context.rip);
        resume_rip = context.rip;

        // The largest of the three arithmetic writers, and the only one
        // on every exit. Recorded only while a second-level guest is
        // running, because that is the case where the field being moved
        // is vmcs02's and `save_l2_state` copies it into vmcs12 at the
        // next reflection. See `low_rip_source`.
        //
        // `context.rip` came out of the VMCS at the top of the handler,
        // so `rip_before` is what the processor saved, and
        // `rip_before + advanced_by == resume_rip` is the sum being
        // alleged.
        if constexpr (nested_vmx::enabled) {
            if ((context.rip < low_rip_threshold) && (cpuid < max_cpus) &&
                this->running_l2[cpuid]) {
                note_low_guest_rip(static_cast<std::size_t>(cpuid),
                                   low_rip_source::advanced_on_resume,
                                   rip_before,
                                   context.rip,
                                   advanced_by,
                                   full_reason.value(),
                                   vmcs.guest_cs_base());
            }
        }
    } else {
        resume_rip = vmcs.guest_rip();
    }

    // The diagnostic trickle and the RIP advance, closed. The advance is
    // one VMWRITE and the pump folds away when the channel is off, so a
    // large number here is the pump and nothing else.
    mark_phase(cpuid, 28);

    // Record what is about to be resumed, now that the handlers have
    // had their say.
    record_exit(cpuid, full_reason, context);

    // `record_exit` alone, because it runs on every single exit and
    // reads up to five VMCS fields doing it - and it is the one thing on
    // this path that exists purely to be read from outside.
    mark_phase(cpuid, 29);

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
        // RIP and CS are not gated, deliberately: they exist to be read
        // from a debugger, and the address a second-level guest is at is
        // the more useful of the two answers.
        //
        // **"Nothing consumes them" is what this comment used to say, and
        // it is false.** `tests/resume_guest` asserts the segment - "and
        // in which segment", expecting `0x28` - so gating the CS read
        // behind a diagnostic switch fails the suite. It was gated on
        // exactly that reasoning, for the 0.8% of wall the read costs at
        // 95 million resumes a run, and the test caught it in one build.
        //
        // Left ungated. A saving that small is not worth weakening a test
        // for, and the comment is corrected so the next person does not
        // repeat the trade.
        auto in_l2 = false;
        if constexpr (nested_vmx::enabled) {
            in_l2 = this->running_l2[slot - 1];
        }

        if (!in_l2) {
            this->resume_activity_state[slot - 1] = activity_now();
        }

        this->resume_guest_rip[slot - 1] = resume_rip;
        this->resume_guest_cs[slot - 1] = vmcs.guest_cs_selector();
    }

    // Whether this processor is about to enter the *second-level* guest
    // rather than its own VMCS. Asked first, because both of the marks
    // below say something about **vmcs01's** launch state and neither may
    // be spent on an entry that does not touch vmcs01.
    auto entering_l2 = false;

    if constexpr (nested_vmx::enabled) {
        if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
            entering_l2 = this->running_l2[slot - 1];
        }
    }

    // Whether this processor has been out of VMX operation and back
    // since the last entry, which only the sleep quiesce does. Its
    // return leaves the launch state clear, and VMRESUME requires
    // launched (SDM 27.1) - so that one case has to leave through
    // VMLAUNCH instead. Consumed here, so the next exit resumes.
    //
    // **Not consumed on a second-level entry, and that is the same
    // defect the enlightened mark below carries a paragraph about.** The
    // flag means "vmcs01 must be launched, not resumed". A second-level
    // entry picks its instruction from vmcs02's launch state a few lines
    // down and ignores `relaunch` entirely - so reading and clearing it
    // here threw it away, and the *next* vmcs01 entry then executed
    // VMRESUME on a VMCS that `enter_root_mode`'s VMCLEAR had left
    // non-launched. That is `vm_instruction_error` 5 out of the plain
    // `vmresume` stub, which produces no VM exit at all and parks the
    // processor.
    //
    // Reachable rather than theoretical: the flag is set by
    // `quiesce_and_sleep`'s caller, which is the I/O exit for the sleep
    // control port, and `on_l2_exit` answers an I/O exit that neither
    // level intercepts with `deferred` - so that handler can run with
    // `running_l2` set. Held instead of dropped, because the requirement
    // is still true and the next entry into vmcs01 is where it applies.
    auto relaunch = false;
    if (auto slot = (cpuid + 1);
        (0 != slot) && (slot <= max_cpus) && !entering_l2) {
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
    // **After an enlightened entry, this VMM's own VMCS must be launched
    // rather than resumed.** The layer below sets its current VMCS
    // pointer to invalid whenever the enlightened pointer changes
    // (`nested_vmx_handle_enlightened_vmptrld`), so the launch state it
    // held for vmcs01 does not survive a second-level entry, and a
    // VMRESUME then fails with `vm_instruction_error` 5, "VMRESUME with
    // non-launched VMCS".
    //
    // **That error was read as the second-level entry's for four
    // attempts.** It is not: the nested stubs report through
    // `zpp_vmx_nested_entry_failure`, and `record_entry_failure` - where
    // the 5 was read - is only called from the plain `vmresume` stub,
    // which is this entry. Three fixes were aimed at the wrong VM entry
    // because two reporters were assumed to be one.
    //
    // `entering_l2` is computed above, beside the sleep relaunch, which
    // needs it for the same reason and had the same defect.

    if constexpr (nested_vmx::evmcs_to_kvm) {
        // **Not on a second-level entry.** The mark says "this VMM's own
        // VMCS must be launched, not resumed"; a second-level entry goes
        // through the nested stubs and picks its instruction from
        // vmcs02's own launch state, so consuming the mark there throws
        // it away and the vmcs01 entry that follows resumes a VMCS the
        // layer below considers unlaunched.
        //
        // Measured, and it is why the counters read as a contradiction:
        // `evmcs_mark_set` 1 and `evmcs_mark_seen` 1 - the mark was made
        // and consumed - beside `vm_instruction_error` **5**, "VMRESUME
        // with non-launched VMCS". Both were true. The resume that saw
        // the mark was the one entering the second level, which ignored
        // it, and the entry that needed it never saw one.
        if (cpuid < max_cpus && !entering_l2) {
            // **Two counters that can disagree.** `set` is incremented
            // where the mark is made, `seen` where it is consumed. If
            // they diverge the mark is being lost between them; if `seen`
            // stays zero while `set` climbs, this path is not the one
            // taken. Either answer is a fact; the single "it did not
            // work" that preceded them was not.
            if (this->evmcs_entered_since_own[cpuid]) {
                relaunch = true;
                this->evmcs_entered_since_own[cpuid] = false;
                this->evmcs_mark_seen[cpuid] += 1;
            } else {
                this->evmcs_mark_absent[cpuid] += 1;
            }
        }
    }

    auto entry = relaunch ? arch::x86_64::vmx::vmlaunch
                          : arch::x86_64::vmx::vmresume;

    if constexpr (nested_vmx::enabled) {
        if (auto slot = (cpuid + 1);
            (0 != slot) && (slot <= max_cpus) && entering_l2) {
            entry = this->vmcs02_launched[slot - 1]
                        ? arch::x86_64::vmx::nested_vmresume
                        : arch::x86_64::vmx::nested_vmlaunch;

            // **An enlightened VMCS is always launched, never resumed.**
            // The launch state lives in `vmcs12->launch_state`, and the
            // enlightened layout has no field for it - so it cannot
            // survive the copy the layer below makes out of the page on
            // every entry, and that layer asks for a VMLAUNCH each time.
            //
            // Measured rather than reasoned: mixed mode reached exactly
            // one second-level entry, four times, and the failure is
            // `vm_instruction_error` **5**, "VMRESUME with non-launched
            // VMCS". Clearing the launch state at the release was not
            // enough, because `on_l2_exit` sets it again at the top of
            // handling the very next exit.
            if constexpr (nested_vmx::evmcs_to_kvm) {
                if (this->evmcs_active[slot - 1]) {
                    entry = arch::x86_64::vmx::nested_vmlaunch;
                }
            }

            // **Which stub was chosen, recorded beside the failure.**
            // Three fixes have been aimed at `vm_instruction_error` 5,
            // "VMRESUME with non-launched VMCS", from reading this
            // function, and all three missed - a VMRESUME executes though
            // this is the only site that selects the instruction. That is
            // a mechanism inferred rather than measured, which is the
            // shape everything expensive in this session had.
            //
            // So the next run says which it is: 1 is launch, 2 is resume.
            // If it reads 1 while the error stays 5, the choice is right
            // and something else executes the entry; if it reads 2, the
            // condition above is not doing what it appears to.
            this->entry_stub_chosen[slot - 1] =
                (entry == arch::x86_64::vmx::nested_vmlaunch) ? 1 : 2;

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
            //
            // **Now behind `census_exits`, where it always belonged.**
            // Measured at **6,104 cycles a call** in the phase tree's
            // `resume: entry census` slot - 3.6% of a 170,454-cycle exit
            // - and it was gated on `nested_vmx::enabled` alone, so a
            // build with every diagnostic switched off still paid a
            // VMREAD, four ring writes and a modulo on *every*
            // second-level entry.
            //
            // The question it was added to answer is answered: entries
            // carry what they were given, and `l2_entry_vector` and
            // `l2_entries_carrying_nothing` recorded it. Keeping the
            // machinery and paying for it on every entry for ever is a
            // different decision from having made the measurement, and
            // only the first one was ever taken deliberately.
            //
            // The general rule this is the worked example of: **a
            // diagnostic on the hot path belongs behind a switch from
            // the day it is written**, because the day it stops being
            // read is never marked.
            if constexpr (nested_vmx::census_exits) {
            constexpr std::uint64_t injection_valid = 1ull << 31;
            constexpr std::uint64_t vector_mask = 0xff;

            auto carried = entry_event_now();

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
            // The same value `resume_rip` above holds: this block runs
            // with vmcs02 current, nothing between the two writes the
            // field, and both want what the guest is about to resume at.
            auto guest_rip = resume_rip;
            auto value = ((context.rdx & 0xffffffff) << 32) |
                         (context.rax & 0xffffffff);

            // Capture the HvCallAttachDevice (0x82) HV_STATUS (secure-dma
            // §18): hvix64 handled the VTL1 VMCALL in L1 and resumes VTL1
            // here at pending_rip+3 (VMCALL is 3 bytes) with RAX = the status.
            // Only that return resumes at pending_rip+3, so no into-VTL1 flag
            // is needed.
            if ((cpu < max_cpus) && this->attach_pending[cpu] &&
                (resume_rip == this->attach_pending_rip[cpu] + 3)) {
                this->attach_status[cpu] =
                    static_cast<std::uint16_t>(context.rax & 0xffff);
                this->attach_captured[cpu] += 1;
                this->attach_pending[cpu] = 0;
            }

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

            // The drop account, taken at the last instant before the
            // entry so it describes what the guest will actually run
            // under. See `nested_vmx::count_dropped_requests`.
            //
            // **Here rather than in `record_l2_entry_event`**, which is
            // where `l2_given_vector` is counted, because that runs
            // only from `enter_or_park_l2` - the path the *level
            // above's* VMLAUNCH and VMRESUME take. An exit this VMM
            // handles itself is resumed straight through here without
            // it, and the entry that follows a tpr-below-threshold exit
            // is exactly one of those. An instrument that could not see
            // that entry could not see the defect it exists to name.
            if constexpr (nested_vmx::count_dropped_requests) {
                note_pending_vector(slot - 1, entry_event_now());
            }
        }
    }

    // Close the span opened at the top of `on_vm_exit`. Here rather
    // than anywhere earlier because everything this VMM does for an exit
    // has now been done, and the next instruction is the entry itself.
    // The last adjacent interval: the entry census above, from the end
    // of `record_exit` to here. Taken before the span below rather than
    // beside it so the two share one RDTSC - `mark_phase` leaves the
    // instant it read in `phase_mark`, and that is the instant the
    // handler's span closes at.
    mark_phase(cpuid, 30);

    if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
        auto cpu = slot - 1;
        auto now = this->phase_mark[cpu];
        if (0 != this->handler_entry_tsc[cpu]) {
            auto span = now - this->handler_entry_tsc[cpu];

            this->handler_cycles[cpu] =
                this->handler_cycles[cpu] + span;
            this->handler_exits[cpu] = this->handler_exits[cpu] + 1;

            // And the same span against the reason that caused it. See
            // `handler_reason_cycles`: the phase table covers the
            // reflection path, only a third of exits take it, and every
            // optimisation aimed at those phases has left the total
            // where it was.
            //
            // Not per processor, deliberately - one processor runs the
            // guest being chased, and a second dimension here would cost
            // 64 cache lines to say so.
            if (auto slot = static_cast<std::size_t>(full_reason.basic());
                slot < handler_reason_slots) {
                this->handler_reason_cycles[slot] =
                    this->handler_reason_cycles[slot] + span;
                this->handler_reason_exits[slot] =
                    this->handler_reason_exits[slot] + 1;

                if (this->handler_was_l2[cpu]) {
                    this->handler_reason_from_l2[slot] =
                        this->handler_reason_from_l2[slot] + 1;
                }

                // And the accesses over the same span. See
                // `handler_reason_reads` - this is what separates a
                // vmcall's excess being hardware from its being
                // software, and the two answers need opposite work.
                this->handler_reason_reads[slot] =
                    this->handler_reason_reads[slot] +
                    (arch::x86_64::vmx::vmcs_reads_taken -
                     this->handler_entry_reads[cpu]);
                this->handler_reason_writes[slot] =
                    this->handler_reason_writes[slot] +
                    (arch::x86_64::vmx::vmcs_writes_taken -
                     this->handler_entry_writes[cpu]);
            }
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

    // Drop any composed shadow this processor holds that was built from
    // an older generation of this VMM's own extended page tables.
    //
    // **Here, on the entry path, rather than beside the hardware
    // catch-up on the exit path, and the difference is a window rather
    // than a preference.** The exit-path catch-up in `on_vm_exit` runs on
    // the way *out* of the guest, so a permission change made after it -
    // by this processor's own handler, or by another processor - is not
    // acted on until the *next* exit. One entry then runs against a
    // shadow composed from the old permissions. That is the whole of the
    // bug: the watched-page step path opens a page, resumes, and the leaf
    // composed while it was open outlives the close.
    //
    // Last thing before the entry for the same reason `apply_time
    // _dilation` is: this is the last moment root operation owns, and a
    // handler above may have changed a watch.
    //
    // Costs one load and one compare when nothing has moved - see
    // `shadow_ept_generation_applied` - and only exists at all with
    // nested VMX, since without it there is no composed shadow to be
    // stale.
    if constexpr (nested_vmx::enabled) {
        if (auto slot = (cpuid + 1); (0 != slot) && (slot <= max_cpus)) {
            discard_stale_shadow_ept(slot - 1);
        }
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
    //
    // **RSP is put back too, and that is a fix rather than a
    // tidy-up.** `restore_context` ends in `iretq`, which pops
    // `context.rsp` into RSP - so this field is not guest state on
    // this path, it is the *host* stack the entry instruction runs
    // on. The exit stub wrote it: `zpp_x86_64_capture_context_into
    // _stack` stores `lea rcx, [rbp+0x18]` into `context->rsp`, which
    // is the address of the context structure itself, and
    // `exit_dispatch.cpp`'s decoder case already records that ("
    // `context.rsp` holds the address of the context structure").
    //
    // `apply_start_up` overwrites it with zero. It has to on the
    // launch path - `vm_launch` seeds `vmcs.guest_rsp` from this
    // field - and its comment says "on the VM exit path both are
    // overwritten again before the resume". **Only `rip` is.**
    // Verified on the artifact rather than by reading: in
    // `out/debug/x86_64/zpp_hypervisor`, `apply_start_up` carries
    // `movq $0x0, 0x20(%rax)` at `0x8a1a5`, and `resume_guest` has
    // exactly one store into the context before `restore_context`,
    // `movq %rcx, 0x80(%rax)` at `0x888a2` - offset 0x80 is `rip`,
    // and nothing writes offset 0x20.
    //
    // What that costs, and it is only visible when something else has
    // already gone wrong: a refused VM entry sets RFLAGS.ZF and
    // passes control to the next instruction rather than taking a VM
    // exit (SDM 29.1 and 29.2, `.references/sdm.txt:202031` and the
    // paragraph closing 29.2), and the next instruction is the
    // `vmresume` stub's `pushfq; pop rdi; call zpp_vmx_entry_failed`.
    // With RSP zero that `pushfq` writes to linear address -8, every
    // host IDT gate is built with `interrupt_stack_table(0)`, and the
    // fault has no stack to be delivered on either - so the reporter
    // that exists precisely to name a refused entry takes the
    // processor down instead of writing the error number.
    // `record_entry_failure`'s own comment asserts the opposite
    // ("`context.rsp` holds the address of the context inside this
    // module, not a guest stack"), and on the one path that applies
    // application-processor start-up state that was false.
    //
    // Restored here rather than in `apply_start_up` on purpose: this
    // is the only place that decides what the entry runs on, it is
    // reached by every exit, and it re-establishes the invariant
    // whoever scribbled on the field. The `vmlaunch` stub survives
    // the same window only because its reporter is stack-free by
    // construction - it stores the instruction error RIP-relatively -
    // which is an asymmetry worth keeping in mind rather than
    // relying on.
    context.rip = reinterpret_cast<std::uint64_t>(entry);
    context.rsp = reinterpret_cast<std::uint64_t>(&context);
    arch::x86_64::restore_context(&context);
    std::unreachable();
}
} // namespace zpp::hypervisor
