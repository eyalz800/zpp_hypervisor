// The extended-page-table rendezvous, against the real
// `wait_for_ept_acknowledgement` and `send_wake_nmi` out of
// ept_rendezvous.cpp.
//
// **Why this harness exists at all.** This is the only code in the tree
// whose behaviour differs between one processor and two. With one, the
// caller stamps its own high-water mark at the top and the loop finds
// nobody outstanding, so every line below - the probe, the patience, the
// give-up - is dead. It went untested for exactly that reason and for one
// more: it lived in hypervisor.cpp, which is eight thousand lines and
// reaches the whole VMM, so nothing could compile it. Splitting it into a
// translation unit is what makes these tests possible, the same move as
// b6f0bee and the start_up.cpp split.
//
// The defect the first test pins, and it is a two-processor-only defect:
//
//   `wake_requested[cpu]` is a de-duplication latch - "an interrupt is
//   already on its way to that processor, do not send another". It is
//   cleared by the target's own NMI *exit*, and NMI exiting governs
//   non-root operation only. A probe that lands while the target is
//   inside its own VM exit arrives at `on_host_exception`, which counts
//   it and returns without clearing the latch - deliberately, and
//   hypervisor.cpp:1341-1347 says under nesting that is "the ordinary
//   case, not an exotic one: a shadow-EPT rebuild is measured in hundreds
//   of microseconds".
//
//   The patience clock was started by the *send*. So the next call found
//   the same processor outstanding, the exchange returned true, no
//   interrupt went out, `probed` stayed zero, the give-up was never
//   reached, and an outstanding processor was carried round the entire
//   16,777,216-round budget in VMX root operation with the guest stopped
//   - once per extended-page-table change. Worse, the give-up is what
//   clears the latch, so missing it once means missing it for ever: the
//   only mechanism for taking a silent processor out of whatever it is
//   doing is switched off permanently by its own first use.
//
// Negative control, measured by putting `probed = budget` back inside the
// `!exchange` branch and re-running: 30 checks, **3 failures** -
// `latched.returned-true`, `latched.gave-up-once` and
// `latched.un-latched`. Exactly the three that describe the wedge: the
// wait comes back false having burned its whole budget, nobody was
// declared unresponsive, and the latch is still set so the next call
// cannot probe either.
//
// The other two checks in that test pass either way on purpose.
// `latched.probe-not-resent` and `latched.not-stamped` are guards on
// behaviour the fix must *not* change, and a guard that only passes with
// the fix in would be testing the fix rather than the invariant.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/hypervisor/hypervisor.h"
#include <atomic>
#include <cstdint>
#include <print>
#include <string>

// ------------------------------------------------------------- checking
static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string & what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::println("  FAIL  {}", what);
    } else {
        std::println("  ok    {}", what);
    }
}

namespace zpp::arch::x86_64
{
// IA32_APIC_BASE is the only MSR anything here reads: `send_wake_nmi`
// builds its two register addresses out of the frame. Answered out of the
// shim's variable rather than stubbed to zero, because zero would make
// the addresses the harness checks indistinguishable from the offsets.
std::uint64_t rdmsr(std::uint32_t index)
{
    return (msr::ia32_apic_base == index) ? g_apic_base.load() : 0;
}

void wrmsr(std::uint32_t, std::uint64_t)
{
}
} // namespace zpp::arch::x86_64

// ------------------------------------------------------------- helpers
/**
 * One hypervisor, reset between tests. Too big for a stack.
 */
static zpp::hypervisor::hypervisor g_vmm;

/**
 * The patience the function under test applies, as a literal.
 *
 * It is a `constexpr` local in `wait_for_ept_acknowledgement` and is not
 * reachable from here, so this is a copy and has to be kept equal to it.
 * A budget below it would never reach the give-up and every test here
 * would pass for the wrong reason, so the budget below is stated as this
 * plus a margin rather than as a number.
 */
static constexpr std::uint64_t probe_patience = 1u << 20;
static constexpr std::uint64_t budget = probe_patience + (1u << 16);

static void reset()
{
    g_vmm.~hypervisor();
    ::new (&g_vmm) zpp::hypervisor::hypervisor();

    // xAPIC at the conventional frame, so `send_wake_nmi`'s register
    // addresses are the real ones.
    zpp::arch::x86_64::g_apic_base.store(0xfee00000);
    zpp::arch::x86_64::mmio_reset();

    // This thread stands in for the boot processor. The function reads
    // its own slot out of the VMCS, and the shim's VMCS is thread_local,
    // so writing it here is writing this processor's.
    g_vmm.vmcs.vpid(1);
}

/**
 * Two processors, both launched, and the second one behind by one
 * generation - which is the state every caller of this function is in
 * immediately after changing an entry.
 */
static void two_processors_one_behind()
{
    g_vmm.start_up_launched[0] = true;
    g_vmm.start_up_launched[1] = true;
    g_vmm.apic_id[0] = 0;
    g_vmm.apic_id[1] = 1;

    g_vmm.ept_generation.store(7, std::memory_order_release);
    g_vmm.ept_generation_seen[0] = 0;
    g_vmm.ept_generation_seen[1] = 0;
}

namespace
{
// ------------------------- 1. a latched probe must not wedge the waiter
//
// The state is exactly what a probe taken in root mode leaves behind:
// `wake_requested` set, and nothing having cleared it. The wait must
// still finish, must still give up on the silent processor, and must
// leave the latch clear so the next call can send a real interrupt.
void test_a_latched_probe_still_times_out()
{
    std::println("\na latched probe still times out");

    reset();
    two_processors_one_behind();

    // The residue of a probe that landed while the target was inside its
    // own VM exit: `on_host_exception` counted it and returned, so this
    // was never cleared.
    g_vmm.wake_requested[1].store(true, std::memory_order_release);

    auto sent_before = g_vmm.wake_nmis_sent;
    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);

    // The whole point: it comes back, rather than carrying an
    // outstanding processor round the entire budget.
    check(answered, "latched.returned-true");

    check(1 == g_vmm.unresponsive_processors, "latched.gave-up-once");

    // And it un-latches, so the *next* call is able to probe again. Left
    // set, the one mechanism for waking a silent processor is off for the
    // rest of the boot.
    check(!g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "latched.un-latched");

    // No second interrupt while one is believed to be in flight, which is
    // what the latch is for and must survive the fix.
    check(sent_before == g_vmm.wake_nmis_sent, "latched.probe-not-resent");

    // The processor that was behind is *not* stamped up to date. Stamping
    // it here would cancel its own invalidation, which is the defect the
    // give-up's comment records.
    check(0 == g_vmm.ept_generation_seen[1], "latched.not-stamped");
}

// ------------------------------- 2. an unlatched processor is probed once
//
// The path that already worked, kept as a guard: the fix must not stop a
// genuine probe going out, and must not send two.
void test_an_unprobed_processor_is_probed_once()
{
    std::println("\nan unprobed processor is probed once");

    reset();
    two_processors_one_behind();

    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);

    check(answered, "probe.returned-true");
    check(1 == g_vmm.wake_nmis_sent, "probe.sent-exactly-one");
    check(1 == g_vmm.unresponsive_processors, "probe.gave-up-once");
    check(!g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "probe.un-latched");

    // The interrupt itself, as two writes to the local APIC page: the
    // destination at 0x310 before the command at 0x300, because writing
    // the low half is what sends it.
    auto writes = zpp::arch::x86_64::g_mmio_write_count.load();
    check(2 == writes, "probe.two-register-writes");

    if (2 == writes) {
        auto & high = zpp::arch::x86_64::g_mmio_writes[0];
        auto & low = zpp::arch::x86_64::g_mmio_writes[1];

        check(0xfee00310 == high.address, "probe.destination-first");
        check(0xfee00300 == low.address, "probe.command-second");
        check(high.order < low.order, "probe.in-that-order");

        // Delivery mode 100b, level assert, and no vector: an NMI
        // carries none.
        check(0x4400 == low.value, "probe.is-an-nmi");
        check((1u << 24) == high.value, "probe.addressed-to-apic-id-1");
    }
}

// ---------------------------- 3. a processor that answers is waited for
//
// The success path, and the reason the give-up must not be reached
// eagerly: a processor that stamps before the patience expires is not
// unresponsive and must not be counted as one.
void test_a_processor_that_answers_is_not_unresponsive()
{
    std::println("\na processor that answers is not unresponsive");

    reset();
    two_processors_one_behind();
    g_vmm.ept_generation_seen[1] = 7;

    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);

    check(answered, "answered.returned-true");
    check(0 == g_vmm.unresponsive_processors, "answered.not-counted");
    check(0 == g_vmm.wake_nmis_sent, "answered.no-probe-sent");
}

// ------------------------------ 4. one processor never waits for anybody
//
// Why this whole file was dead code until the guest was given a second
// processor. The caller stamps itself at the top - it has to, since
// changing an entry is what moved the generation - so with nobody else
// launched the first round finds nothing outstanding.
void test_one_processor_never_waits()
{
    std::println("\none processor never waits");

    reset();

    g_vmm.start_up_launched[0] = true;
    g_vmm.apic_id[0] = 0;
    g_vmm.ept_generation.store(7, std::memory_order_release);
    g_vmm.ept_generation_seen[0] = 0;

    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);

    check(answered, "single.returned-true");
    check(0 == g_vmm.wake_nmis_sent, "single.no-probe-sent");
    check(0 == g_vmm.unresponsive_processors,
          "single.nobody-unresponsive");

    // Stamped by the caller itself, which is the line that makes the
    // single-processor case trivial.
    check(7 == g_vmm.ept_generation_seen[0],
          "single.caller-stamped-itself");
}

// --------------------- 5. a processor that never launched is not waited
// on
//
// It holds no translation, so an invalidation it never performs was never
// needed - and waiting on it would wait for ever.
void test_an_unlaunched_processor_is_skipped()
{
    std::println("\nan unlaunched processor is skipped");

    reset();
    two_processors_one_behind();
    g_vmm.start_up_launched[1] = false;

    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);

    check(answered, "unlaunched.returned-true");
    check(0 == g_vmm.wake_nmis_sent, "unlaunched.no-probe-sent");
    check(0 == g_vmm.unresponsive_processors, "unlaunched.not-counted");

    // And it is recorded as not being part of the rendezvous, which is
    // what a reader of a wedged machine needs in order to tell "did not
    // answer" from "was never asked".
    check(1 == g_vmm.ack_launched_mask, "unlaunched.absent-from-the-mask");
}

// ------------------------------- 6. without a probe there is no give-up
//
// `probe` false is the passive wait, and it deliberately has no way to
// declare anybody unresponsive - so it burns its budget and answers
// false. Kept because the fix touches the branch that decides this.
void test_a_passive_wait_does_not_give_up()
{
    std::println("\na passive wait does not give up");

    reset();
    two_processors_one_behind();

    // A small budget, because this one is expected to spend all of it.
    auto answered = g_vmm.wait_for_ept_acknowledgement(1u << 10, false);

    check(!answered, "passive.returned-false");
    check(0 == g_vmm.wake_nmis_sent, "passive.no-probe-sent");
    check(0 == g_vmm.unresponsive_processors, "passive.nobody-declared");
    check(!g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "passive.latch-untouched");
}

// ------------------------ 7. the liveness probe reaches the right set
//
// `probe_application_processors` is the second driver of the same wake
// interrupt, and this is the shape of one round: every *other* launched
// processor, once each, counted where a reader can see it.
//
// Self is excluded because a processor driving this is by definition
// executing, so an interrupt it sent itself would come back through its
// own exit path and be recorded as evidence about somebody else.
void test_a_probe_round_reaches_every_other_launched_processor()
{
    std::println("\na probe round reaches every other launched processor");

    reset();
    two_processors_one_behind();

    g_vmm.probe_application_processors(0);

    check(1 == g_vmm.wake_nmis_sent, "round.sent-exactly-one");
    check(1 == g_vmm.ap_probe_sent[1], "round.counted-against-target");
    check(0 == g_vmm.ap_probe_sent[0], "round.did-not-probe-itself");
    check(g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "round.latched-the-target");

    // Addressed to the target's APIC id, not to its slot. The two are
    // equal in this fixture only because the fixture sets them equal, so
    // the check is on the register the interrupt actually carries.
    auto writes = zpp::arch::x86_64::g_mmio_write_count.load();
    check(2 == writes, "round.two-register-writes");
    if (2 == writes) {
        check((1u << 24) == zpp::arch::x86_64::g_mmio_writes[0].value,
              "round.addressed-to-apic-id-1");
        check(0x4400 == zpp::arch::x86_64::g_mmio_writes[1].value,
              "round.is-an-nmi");
    }
}

// ------------------- 8. a processor that never launched is not probed
//
// It has never been in non-root operation, so an interrupt sent to it
// measures the firmware rather than this VMM, and the silence it would
// answer with is not the silence this instrument reads.
void test_the_probe_skips_an_unlaunched_processor()
{
    std::println("\nthe probe skips an unlaunched processor");

    reset();
    two_processors_one_behind();
    g_vmm.start_up_launched[1] = false;

    g_vmm.probe_application_processors(0);

    check(0 == g_vmm.wake_nmis_sent, "unlaunched.no-probe-sent");
    check(0 == g_vmm.ap_probe_sent[1], "unlaunched.nothing-counted");
}

// ------- 9. an unanswered probe must not starve the rendezvous. THE ONE
//
// This is the check the whole design turns on, and it is the same defect
// `test_a_latched_probe_still_times_out` pins arriving from the other
// direction.
//
// The state this instrument exists to report - a processor in the
// wait-for-SIPI state, where an NMI is blocked outright and no exit
// occurs - is precisely the state in which nothing ever clears the
// latch. `send_wake_nmi` is called only when the exchange finds the latch
// clear, so a probe left latched disables *both* drivers for that
// processor for the rest of the boot: this one stops counting, and the
// extended-page-table rendezvous loses its only means of taking a silent
// processor out of whatever it is doing.
//
// So a round that finds its own previous probe outstanding clears the
// latch and sends nothing, and the round after that sends again. The
// counter therefore keeps rising against a processor that answers
// nothing, which is what makes "sent rises, neither answer moves"
// readable as a state rather than as a dead instrument.
void test_an_unanswered_probe_does_not_starve_the_rendezvous()
{
    std::println("\nan unanswered probe does not starve the rendezvous");

    reset();
    two_processors_one_behind();

    // Round one: a real interrupt, and the target answers nothing -
    // which is what the wait-for-SIPI state does.
    g_vmm.probe_application_processors(0);
    check(1 == g_vmm.wake_nmis_sent, "starve.round-one-sent");

    // Round two: finds its own probe outstanding. Sends nothing, and
    // un-latches.
    g_vmm.probe_application_processors(0);
    check(1 == g_vmm.wake_nmis_sent, "starve.round-two-sent-nothing");
    check(!g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "starve.round-two-un-latched");

    // Round three: able to send again. Without the un-latch above this
    // is the round that never happens, and the counter stops moving on
    // exactly the processor the instrument was aimed at.
    g_vmm.probe_application_processors(0);
    check(2 == g_vmm.wake_nmis_sent, "starve.round-three-sent-again");
    check(2 == g_vmm.ap_probe_sent[1], "starve.counted-both-sends");

    // And the rendezvous can still probe, which is the property that
    // must not be broken by adding a second driver to a shared latch.
    // It finds the latch set from round three, so its own give-up path
    // is what has to clear it - the case test 1 covers.
    auto answered = g_vmm.wait_for_ept_acknowledgement(budget, true);
    check(answered, "starve.rendezvous-still-returns");
    check(!g_vmm.wake_requested[1].load(std::memory_order_acquire),
          "starve.rendezvous-left-it-clear");
}

} // namespace

// ---------------------------------------------------------------- main
int main()
{
    test_a_latched_probe_still_times_out();
    test_an_unprobed_processor_is_probed_once();
    test_a_processor_that_answers_is_not_unresponsive();
    test_one_processor_never_waits();
    test_an_unlaunched_processor_is_skipped();
    test_a_passive_wait_does_not_give_up();
    test_a_probe_round_reaches_every_other_launched_processor();
    test_the_probe_skips_an_unlaunched_processor();
    test_an_unanswered_probe_does_not_starve_the_rendezvous();

    std::println("\n{} checks, {} failures", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
