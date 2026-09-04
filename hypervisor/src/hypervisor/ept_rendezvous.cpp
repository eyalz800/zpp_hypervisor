// The extended-page-table rendezvous: making every other processor drop
// the translations this one has just changed.
//
// Two functions, split out of hypervisor.cpp beside `invalidate_ept`,
// which is their only caller's caller. `wait_for_ept_acknowledgement`
// decides whether every launched processor has invalidated since a
// change, and `send_wake_nmi` is the one nudge it has for a processor
// that is not answering.
//
// **Why they are a translation unit rather than two more functions in a
// file nothing can compile.** This is the only code in the tree whose
// behaviour differs between one processor and two - with one, the caller
// stamps itself at the top and the loop finds nobody outstanding, so
// every path below is dead - and it had no test, because hypervisor.cpp
// is eight thousand lines and reaches the whole VMM. Same reason as
// b6f0bee and as the start_up.cpp split: a harness needs a translation
// unit it can compile against a small stand-in header. tests/ept
// _rendezvous compiles this one with host threads standing in for
// logical processors.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/spin_lock.h"
#include <cstdint>

namespace zpp::hypervisor
{
void hypervisor::send_wake_nmi(std::uint64_t apic)
{
    // Straight at the local APIC's command register, which the host page
    // table already maps because the interrupt command watch needs it.
    //
    // The destination half is written first because writing the low half
    // is what sends the interrupt - a destination written after it would
    // be written after the thing that used it.
    auto base = local_apic_base();

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

void hypervisor::probe_application_processors(std::size_t cpu)
{
    for (std::size_t other{}; other < max_cpus; ++other) {
        // Not this one. A processor driving this is by definition
        // executing, and an NMI it sent itself would be answered by its
        // own exit path as though it were evidence about somebody else.
        if (other == cpu) {
            continue;
        }

        // Only processors that were actually launched. One that never
        // was has no VMCS, has never been in non-root operation, and is
        // parked wherever the firmware left it - so an interrupt sent to
        // it measures the firmware, not this VMM. Same test the
        // rendezvous applies, and for a related reason.
        if (!this->start_up_launched[other].load(
                std::memory_order_acquire)) {
            continue;
        }

        // A probe still outstanding is one nobody answered, which is the
        // wait-for-SIPI reading this instrument exists to produce. Clear
        // the latch and send nothing this round.
        //
        // **The clearing is not tidiness, it is what keeps the
        // extended-page-table rendezvous working.** That function shares
        // this latch and calls `send_wake_nmi` only when the exchange
        // finds it clear, so a latch left set by an unanswered probe
        // would disable its only means of taking a silent processor out
        // of whatever it is doing - permanently, and for the processor
        // most likely to need it. `tests/ept_rendezvous` was written for
        // exactly that failure arriving from the other direction.
        if (this->wake_requested[other].exchange(
                true, std::memory_order_acq_rel)) {
            this->wake_requested[other].store(false,
                                              std::memory_order_release);
            continue;
        }

        // Counted before the interrupt goes out, so a send that faults
        // still leaves evidence that it was attempted. The counter is
        // this instrument's alone; the two that record answers are not.
        this->ap_probe_sent[other] = this->ap_probe_sent[other] + 1;

        send_wake_nmi(this->apic_id[other]);
    }
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
                // **The patience clock starts on finding a processor
                // outstanding, not on sending an interrupt to it.** The
                // two used to be the same line, and separating them is
                // the fix.
                //
                // `wake_requested` is a de-duplication latch: it stops a
                // second probe going out while one is still in flight.
                // It is cleared by the target's own NMI *exit*, and an
                // NMI only produces an exit in non-root operation. A
                // probe that lands while the target is inside its own VM
                // exit arrives at `on_host_exception` instead, which
                // counts it and returns - deliberately, and the comment
                // below already says so. So the latch stays set.
                //
                // With the clock started by the send, that left this
                // loop unable to finish. The next call finds the same
                // processor outstanding, the exchange returns true, no
                // interrupt goes out, `probed` stays zero, the give-up
                // below is never reached, and the `break` carries an
                // outstanding processor round the whole budget - 16.7
                // million spins in root operation, with the guest
                // stopped, on every extended-page-table change. And the
                // latch is cleared *by* the give-up, so missing it once
                // means missing it for ever: the one mechanism for
                // taking a silent processor out of whatever it is doing
                // is switched off permanently by its own first use.
                //
                // None of this is reachable with a single processor -
                // the caller stamps itself at the top, so the loop finds
                // nobody outstanding and returns on its first round.
                // That is why it survived: the two-processor
                // configuration is the only one that executes it.
                //
                // Starting the clock here bounds every call at
                // `probe_patience` rounds per outstanding processor
                // whether or not this call is the one that sent the
                // interrupt, and the give-up then un-latches the probe
                // so the next call can send a real one.
                if (probe) {
                    if (!this->wake_requested[cpu].exchange(
                            true, std::memory_order_acq_rel)) {
                        send_wake_nmi(this->apic_id[cpu]);
                    }

                    if (0 == probed) {
                        probed = budget;
                    }
                }

                if ((0 != probed) &&
                    ((probed - budget) > probe_patience)) {
                    this->wake_requested[cpu].store(
                        false, std::memory_order_release);

                    // **Give up without stamping.** This used to write
                    // `ept_generation_seen[cpu] = target` here, which is
                    // the very variable that processor's own exit path
                    // reads to decide whether to invalidate - so being
                    // declared unresponsive *cancelled its invalidation*
                    // and it resumed its guest against translations this
                    // one had just changed.
                    //
                    // The inference the stamp rested on - "silence
                    // outliving the probe means not running, and a
                    // processor that is not running holds no translation
                    // it can use" - has a hole. A processor inside its
                    // own VM exit takes the probe through the host IDT,
                    // where `on_host_exception` counts it and returns
                    // without stamping or clearing `wake_requested`; the
                    // comment there says so. Under nesting that is the
                    // ordinary case, not an exotic one: a shadow-EPT
                    // rebuild is measured in hundreds of microseconds.
                    //
                    // Leaving the mark alone costs nothing and is
                    // correct either way. A parked processor holds no
                    // translations, so an invalidation it never performs
                    // was never needed; a busy one performs it at its
                    // next exit, before it re-enters. KVM does not have
                    // the choice to make - `kvm_flush_remote_tlbs` waits
                    // for every vCPU to leave guest mode
                    // (`.references/kvm/mmu.c:2642`) and a pending flush
                    // is only ever cleared by the processor that owns it.
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

} // namespace zpp::hypervisor
