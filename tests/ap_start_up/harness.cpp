// Concurrency harness for application-processor start-up and adoption.
//
// Compiles the real processor_slot, start_up_processor,
// start_application_processor, enter_root_mode, apply_start_up,
// emulate_init_signal, start_up_broadcast, send_start_up_ipi,
// x2apic_enabled and local_apic_id - cut out of hypervisor.cpp by
// build.sh - natively against a shim hypervisor, and drives them from
// several host threads standing in for several logical processors.
//
// Both ends of the start-up hand-off are compiled here, and that is the
// point of the second half of this file: the sender's side alone can only
// be asked what it does with a published state, never whether the target
// publishes it in time. f949649 was a reordering of two writes against a
// wait, so nothing that stands in for the target can be wrong in the way
// the original was.
//
// What it is and is not. Every other harness in tests/ is differential:
// it compares this VMM's answer against a reference. There is no
// reference for "did two processors collide", so this one asserts
// **invariants** instead, each of which is a sentence out of the SDM or
// out of this tree's own comments, and each of which a defect that has
// actually been in this tree violates. The rig costs ten minutes a boot
// and this costs a second, which is the whole argument for it.
//
// The invariants:
//
//   1. No two logical processors may hold the same VMCS region. SDM 25.1
//      and initialize_vmx's own comment. Violated for as long as the
//      VMXON and VMCS region addresses were passed from initialize_vmx to
//      enter_root_mode through two shared members - a second processor
//      arriving inside that window sent the first one's VMPTRLD at its
//      region.
//   2. A slot names one processor. processor_slot hands out the index
//      every per-processor array is addressed by, including the VPID, so
//      two identifiers sharing a slot is invariant 1 by another route.
//   3. A processor that is running the guest must not have
//      start_up_launched cleared underneath it. That flag is what
//      wait_for_ept_acknowledgement uses to decide who must answer an
//      extended page table change; clearing it for a running processor
//      removes it from every rendezvous, silently and for good.
//   4. A start-up IPI is delivered to a target that says it is waiting
//      for one through the software hand-off, and dropped for one that
//      does not say so. That is the whole contract between
//      emulate_init_signal and start_up_processor, and it is why the
//      target has to publish its activity state *before* it waits.
//   5. The hand-off is consumed exactly once. A sender may deliver only
//      *out of* software_wait and the target leaves that state by the
//      same indivisible operation, so the two cannot both succeed and
//      cannot both fail. This is what 439abb5's flag did not have: a flag
//      carries no ordering against the start-up IPI that follows it.
//   6. A processor that says it is listening is listening by the time it
//      can be seen to be. The activity record and the mailbox are both
//      published before the wait, not after it - f949649.
//   7. The firmware's start-up is not the guest's. The decision about a
//      duplicate start-up IPI is the target's activity state and never a
//      flag, because the firmware's own broadcast sets every flag a guard
//      might have used - c65f8f7, then 95d9759.
//   8. A start-up IPI goes out in whichever local APIC mode this
//      processor is actually in. The x2APIC interrupt command MSR does
//      not exist in xAPIC mode and writing it faults in the host, where
//      there is no recovery point - 2685265.
//   9. An INIT is forwarded and nothing more. It changes no
//      per-processor state here, which is the property 439abb5's flag
//      broke - and it broke it for an INIT level de-assert too, which
//      starts nothing at all.
//
// The threads are not a stand-in for a processor in every respect - a
// host thread can be descheduled and a logical processor cannot - but
// they interleave the same shared memory the same way, which is the part
// the defects live in.
//
// How much each check is worth, measured rather than claimed. Backing the
// fix out and re-running gives:
//
//   invariant 2 - 6,149 collisions over 400 rounds without the lock in
//                 processor_slot, 0 with it.
//   invariant 3 - 5 rounds in 400 with `start_up_launched[slot] = false`
//                 still in start_application_processor, 0 without it.
//                 That number also found the hole in the first attempt at
//                 the fix: re-testing processor_virtualized under the lock
//                 narrows the window and does not close it, because the
//                 target marks itself without taking that lock.
//   invariant 1 - a **regression pin**, not a reproduction. Putting the
//                 two shared members back inside enter_root_mode does not
//                 fail here, because the compiler forwards the store to
//                 the load when both are in one function; in the tree they
//                 were in two, hundreds of lines apart. Said out loud
//                 rather than left as an apparent pass.
//   invariant 4 - a contract pin. It states what the sender does with each
//                 published state, which is what the target's ordering has
//                 to be written against.
//
// The later ones were measured the same way, by putting each reverted
// defect back into hypervisor.cpp and re-running. All four go red, and
// what they say when they do is the reason each is worth its lines:
//
//   f949649 - moving `resume_activity_state[cpu] = wait_for_start_up_ipi`
//             back below the wait: 7 checks, and the one that names it is
//             "no round lost it (16 did)". Sixteen of twenty-four rounds
//             swallowed the guest's write with nothing left to start the
//             processor.
//   439abb5 - the sender's hand-over as a plain store rather than a
//             compare-exchange: 13 checks. Eight rounds lost the vector,
//             and every "left exactly as it was" case failed - which is
//             the difference between a store and an exchange stated as a
//             value rather than as a shape.
//   c65f8f7 - the duplicate guard reading `started_by_start_up_ipi`
//             instead of the activity state: 4 checks, and the count in
//             the message is the original measurement - 0 of the guest's
//             7 start-up IPIs sent, all swallowed as duplicates of the
//             firmware's own broadcast.
//   2685265 - `send_start_up_ipi` writing the x2APIC command MSR
//             unconditionally: 8 checks. On the rig that write is a #GP
//             in the host with no recovery point.
//
// And 439abb5's own mechanism, put back in the smallest shape it had -
// `on_interrupt_command` flagging the destination of an INIT: 2 checks,
// one of them for the level de-assert, which is the form that flagged a
// processor for an INIT the guest had not sent.
#include "support/identity_page_table.h"
// By name, because the real hypervisor.h does not include it and the
// stand-in this harness used to compile against did. Everything below
// that reads a control register, an APIC base or a CPUID leaf reads it
// out of tests/ap_start_up/shim/zpp/arch/x86_64/asm.h.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/hypervisor/hypervisor.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using zpp::hypervisor::hypervisor;

// ------------------------------------------------------------- checking
static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string & what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    } else {
        std::printf("  ok    %s\n", what.c_str());
    }
}

// -------------------------------------------------- the fake trampoline
//
// start_application_processor copies the assembler's own working area out
// of the blob and writes the per-processor fields into the page. Both are
// real memory here, so nothing about the code under test is relaxed.
extern "C" const unsigned char zpp_ap_start_up_begin[0x1000] = {};
extern "C" const unsigned char zpp_ap_start_up_end[0] = {};

alignas(0x1000) static unsigned char g_trampoline
    [zpp::arch::x86_64::ap_start_up_pages * 0x1000];

// ------------------------------------------- what the harness supplies
namespace zpp::hypervisor
{
std::uint32_t hypervisor::start_up_trampoline_stage() const
{
    return 0;
}

std::expected<void, zpp::error> hypervisor::enable_vmx_in_feature_control()
{
    return {};
}

/**
 * Reached only by `note_apic_mode`, which this harness compiles out of
 * local_apic.cpp and asks nothing of. The real one lives in
 * local_apic_write.cpp, which is not compiled here.
 */
void hypervisor::watch_local_apic(bool)
{
}

/**
 * The cached VMX capability MSRs, reached only by `monitor_trap_flag`,
 * which this harness compiles and asks nothing of. One slot, shared by
 * every index: nothing here reads it back.
 */
std::uint64_t & hypervisor::cached_vmx_msr(std::size_t)
{
    static std::uint64_t unused{};
    return unused;
}
} // namespace zpp::hypervisor

namespace zpp::arch::x86_64
{
// IA32_APIC_BASE is the one MSR anything here reads, and two functions
// under test branch on it: `x2apic_enabled` on bit 10, and the xAPIC half
// of `send_start_up_ipi` on the frame. Answered out of a variable so both
// modes can be driven, rather than stubbed to zero - a stub to zero is
// the xAPIC answer and would have left the x2APIC branch untested, which
// is the branch 2685265 is about.
std::uint64_t rdmsr(std::uint32_t index)
{
    return (msr::ia32_apic_base == index) ? g_apic_base.load() : 0;
}

// Recorded rather than performed, and the count is the point: writing the
// x2APIC interrupt command MSR while the APIC is in xAPIC mode takes a
// #GP in the host, where this VMM has no recovery point.
void wrmsr(std::uint32_t index, std::uint64_t value)
{
    if (msr::ia32_x2apic_icr == index) {
        g_x2apic_icr_last.store(value);
        g_x2apic_icr_writes.fetch_add(1);
    }
}
} // namespace zpp::arch::x86_64

// ------------------------------------------------------------- helpers
/**
 * A fresh hypervisor for each test. Big enough that one on the stack is
 * unwise, so it lives here and is reset between tests.
 */
static hypervisor g_vmm;

static void reset()
{
    g_vmm.~hypervisor();
    ::new (&g_vmm) hypervisor();
    g_vmm.start_up_memory = reinterpret_cast<std::uint64_t>(g_trampoline);

    // The real host page table, over the two per-processor regions
    // `enter_root_mode` translates: `vmxon` and `vmptrld` take the
    // address of a physical address, and what this harness asserts is
    // that each processor named *its own*. Re-mapped here rather than
    // once in main, because the reconstruction above wipes the table
    // along with the rest of the object.
    zpp::tests::map_identity(g_vmm.host_page_table,
                             {{&g_vmm.vmx, sizeof(g_vmm.vmx)},
                              {&g_vmm.vmx_vmcs, sizeof(g_vmm.vmx_vmcs)}});

    // xAPIC at the conventional frame, and nothing under us. That is bare
    // metal, which is what every test that says nothing about the mode
    // wants to be running on.
    zpp::arch::x86_64::g_apic_base.store(0xfee00000);
    zpp::arch::x86_64::g_leaf_1_ecx = 0;
    zpp::arch::x86_64::g_initial_apic_id = 0;
    zpp::arch::x86_64::g_x2apic_icr_writes.store(0);
    zpp::arch::x86_64::g_x2apic_icr_last.store(0);
    zpp::arch::x86_64::mmio_reset();
}

/**
 * Runs `work(index)` on `count` threads and waits for all of them,
 * released together so the interleaving is as tight as the host will
 * make it.
 */
template <typename Work>
static void together(std::size_t count, Work work)
{
    std::atomic<bool> go{false};
    std::atomic<std::size_t> ready{};
    std::vector<std::thread> threads;

    for (std::size_t index{}; index < count; ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1);
            while (!go.load()) {
            }
            work(index);
        });
    }

    while (ready.load() < count) {
    }
    go.store(true);

    for (auto & thread : threads) {
        thread.join();
    }
}

// ------------------------------------- 1. one VMCS, one processor
//
// The defect this pins: `initialize_vmx(cpu)` used to publish this
// processor's VMXON and VMCS region addresses into two shared members,
// and `enter_root_mode()` read them back several hundred lines later.
// Between those two the starter's `start_up_lock` is the only thing
// holding anybody off, and it is released on its own timeout as well as
// on success - so a second processor entering `initialize_vmx` inside the
// window sent the first one's VMPTRLD at the second one's region.
//
// SDM 25.1, and initialize_vmx's own comment: "a VMCS may not be active
// on more than one logical processor."
static void test_one_vmcs_per_processor()
{
    std::printf("\none VMCS per processor\n");

    constexpr std::size_t processors = 8;
    constexpr int rounds = 200;

    std::uint64_t vmxon[processors]{};
    std::uint64_t vmptrld[processors]{};
    auto disagreements = 0;

    for (auto round = 0; round < rounds; ++round) {
        reset();

        together(processors, [&](std::size_t cpu) {
            auto entered = g_vmm.enter_root_mode(cpu);
            if (!entered) {
                return;
            }
            vmxon[cpu] = zpp::arch::x86_64::vmx::g_vmxon_region;
            vmptrld[cpu] = zpp::arch::x86_64::vmx::g_vmptrld_region;
        });

        for (std::size_t cpu{}; cpu < processors; ++cpu) {
            auto own_vmxon =
                reinterpret_cast<std::uint64_t>(&g_vmm.vmx[cpu]);
            auto own_vmcs =
                reinterpret_cast<std::uint64_t>(&g_vmm.vmx_vmcs[cpu]);
            if ((vmxon[cpu] != own_vmxon) || (vmptrld[cpu] != own_vmcs)) {
                ++disagreements;
            }
        }
    }

    check(0 == disagreements,
          "every processor entered VMX operation on its own VMXON and "
          "VMCS region, over " +
              std::to_string(rounds) + " rounds of " +
              std::to_string(processors) + " at once (" +
              std::to_string(disagreements) + " disagreed)");

    // And the other half of the same sentence, stated separately because
    // a defect could satisfy one and not the other: no two processors may
    // name the same region even if each is wrong in the same way.
    auto shared = 0;
    for (std::size_t a{}; a < processors; ++a) {
        for (auto b = a + 1; b < processors; ++b) {
            if (vmptrld[a] == vmptrld[b]) {
                ++shared;
            }
        }
    }

    check(0 == shared,
          "no two processors named the same VMCS region (" +
              std::to_string(shared) + " pairs did)");

    // Out of range is refused rather than indexed, since the slot now
    // reaches an array directly.
    check(!g_vmm.enter_root_mode(hypervisor::max_cpus),
          "a slot past max_cpus is refused rather than indexed");
}

// ------------------------------------------- 2. a slot names one
// processor
//
// processor_slot scans the table of known local APIC identifiers and
// appends on a miss, and every processor reaches it from its own exit
// handler. Unsynchronised, two processors asking about two different
// unknown identifiers both read the same count, both take that slot, and
// the second overwrites the first's entry.
static void test_slot_allocation()
{
    std::printf("\na slot names one processor\n");

    constexpr std::size_t askers = 8;
    constexpr int rounds = 400;

    auto collisions = 0;
    auto lost = 0;

    for (auto round = 0; round < rounds; ++round) {
        reset();

        // Slot zero is the boot processor's, which exists before anything
        // else does - number_of_known_processors counts from one.
        g_vmm.apic_id[0] = 0;

        std::size_t taken[askers]{};
        std::atomic<bool> refused{false};

        together(askers, [&](std::size_t index) {
            auto identifier = static_cast<std::uint64_t>(index + 1);
            if (auto slot = g_vmm.processor_slot(identifier)) {
                taken[index] = *slot;
            } else {
                refused.store(true);
            }
        });

        if (refused.load()) {
            ++lost;
            continue;
        }

        for (std::size_t a{}; a < askers; ++a) {
            // The identifier it asked about has to be the one the table
            // now holds at the slot it was given.
            if (g_vmm.apic_id[taken[a]] !=
                static_cast<std::uint64_t>(a + 1)) {
                ++collisions;
            }
            for (auto b = a + 1; b < askers; ++b) {
                if (taken[a] == taken[b]) {
                    ++collisions;
                }
            }
        }
    }

    check(0 == lost, "no asker was refused a slot");
    check(0 == collisions,
          "no two local APIC identifiers were given the same slot, over " +
              std::to_string(rounds) + " rounds of " +
              std::to_string(askers) + " at once (" +
              std::to_string(collisions) + " did)");

    // The same identifier twice is the same slot, which is what makes the
    // scan a lookup rather than an allocator.
    reset();
    auto first = g_vmm.processor_slot(9);
    auto again = g_vmm.processor_slot(9);
    check(first && again && (*first == *again),
          "the same identifier resolves to the same slot");
}

// --------------------------- 3. a running processor stays launched
//
// start_up_processor tests processor_virtualized[slot] outside the lock,
// so two senders answering the same broadcast both fall through to
// start_application_processor. The second one's first act was
// `start_up_launched[slot] = false` - for a processor that is running the
// guest, and that flag is what wait_for_ept_acknowledgement uses to
// decide who must answer an extended page table change.
static void test_running_processor_stays_launched()
{
    std::printf("\na running processor stays launched\n");

    constexpr int rounds = 400;
    auto cleared = 0;

    for (auto round = 0; round < rounds; ++round) {
        reset();
        g_vmm.apic_id[1] = 1;
        g_vmm.number_of_known_processors = 2;

        std::atomic<bool> observed_clear{false};

        // Two senders racing to start slot 1, and the target itself,
        // which comes up and marks itself virtualized and launched
        // exactly as main does - processor_virtualized first, then
        // start_up_launched.
        together(3, [&](std::size_t who) {
            if (2 == who) {
                g_vmm.processor_virtualized[1] = true;
                g_vmm.start_up_launched[1].store(true);

                // From here it is running the guest. Nothing may take
                // that mark away.
                for (auto watch = 0; watch < 20000; ++watch) {
                    if (!g_vmm.start_up_launched[1].load()) {
                        observed_clear.store(true);
                        return;
                    }
                }
                return;
            }

            // A sender's own view, taken outside the lock exactly as
            // start_up_processor takes it.
            if (!g_vmm.processor_virtualized[1]) {
                static_cast<void>(g_vmm.start_application_processor(1, 2));
            }
        });

        if (observed_clear.load() || !g_vmm.start_up_launched[1].load()) {
            ++cleared;
        }
    }

    check(0 == cleared,
          "start_up_launched was never cleared for a processor that had "
          "already marked itself virtualized, over " +
              std::to_string(rounds) + " rounds (" +
              std::to_string(cleared) + " were)");
}

// ------------------------- 4. the hand-off is obeyed, not guessed
//
// The contract between emulate_init_signal and start_up_processor. The
// target publishes what it is waiting on; the sender obeys it. The bug
// this pins is that the sender gates on the *activity record* before it
// looks at the mailbox, and the target used to write that record only
// after its wait - so for the whole length of the wait it read `active`
// and every start-up IPI aimed at the target was dropped, and dropped is
// returned as adopted, which swallows the guest's write.
static void test_handoff_is_obeyed()
{
    std::printf("\nthe hand-off is obeyed, not guessed\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::uint64_t wait_for_sipi = 3;
    constexpr std::uint64_t active = 0;

    // A target that has published both facts is handed the vector.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = wait_for_sipi;
    g_vmm.start_up_handoff[1].store(handoff::software_wait);

    auto answered = g_vmm.start_up_processor(7, 0x42);
    check(hypervisor::start_up_result::adopted == answered,
          "a target listening on the software hand-off is adopted");
    check(handoff::is_delivered(g_vmm.start_up_handoff[1].load()) &&
              (0x42 == handoff::vector(g_vmm.start_up_handoff[1].load())),
          "and the vector it asked for is in the mailbox");

    // The same, published by a second-level guest's park instead, which
    // records it in l2_activity_state.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = active;
    g_vmm.l2_activity_state[1] = wait_for_sipi;
    g_vmm.start_up_handoff[1].store(handoff::software_wait);

    answered = g_vmm.start_up_processor(7, 0x30);
    check(hypervisor::start_up_result::adopted == answered,
          "a second-level guest parked in wait-for-SIPI is adopted too");
    check(0x30 == handoff::vector(g_vmm.start_up_handoff[1].load()),
          "and gets its own vector");

    // A target waiting on hardware is *not* swallowed. This is the case
    // that used to lose a start-up IPI outright, and it is why the
    // hand-over is a compare-exchange rather than a store.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = wait_for_sipi;
    g_vmm.start_up_handoff[1].store(handoff::hardware_wait);

    answered = g_vmm.start_up_processor(7, 0x20);
    check(hypervisor::start_up_result::needs_hardware == answered,
          "a target waiting on hardware gets a real start-up IPI, not a "
          "swallowed one");
    check(handoff::hardware_wait == g_vmm.start_up_handoff[1].load(),
          "and its mailbox is left alone");

    // A running processor's duplicate start-up IPI is dropped. SDM 29.7.2:
    // "the active state blocks start-up IPIs (SIPIs)".
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = active;
    g_vmm.start_up_handoff[1].store(handoff::none);

    answered = g_vmm.start_up_processor(7, 0x10);
    check(hypervisor::start_up_result::adopted == answered,
          "a duplicate start-up IPI to a running processor is dropped");
    check(handoff::none == g_vmm.start_up_handoff[1].load(),
          "and nothing is written into its mailbox");

    // **The defect.** A target that is listening on the software hand-off
    // but whose activity record still says `active` has its start-up IPI
    // dropped - the sender never reaches the mailbox. That combination is
    // exactly what emulate_init_signal used to produce for the whole
    // length of its wait, and the fix is that it no longer can: it writes
    // the activity record before the wait, not after it.
    //
    // Asserted as the sender's behaviour rather than as a bug, because
    // the sender is right. A record is the only thing it can trust, and a
    // mailbox left published by enter_or_park_l2's `entered` path would
    // otherwise be read as a listener that is not there.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = active;
    g_vmm.l2_activity_state[1] = active;
    g_vmm.start_up_handoff[1].store(handoff::software_wait);

    answered = g_vmm.start_up_processor(7, 0x08);
    check(hypervisor::start_up_result::adopted == answered,
          "a listener whose activity record says active is dropped - so "
          "a target must publish that record before it waits");
    check(handoff::software_wait == g_vmm.start_up_handoff[1].load(),
          "and its mailbox is untouched, which is what makes the drop "
          "silent and the ordering load bearing");
}

// --------------------------- 5. the hand-off state machine itself
//
// Four states in one word, three of them named and the fourth carrying a
// vector. The one property that is not obvious from the declaration is
// the reason `delivered` is 3 rather than 1: a hand-off of vector zero
// has to stay distinguishable from no hand-off at all, and it would not
// be if the state were the vector.
static void test_handoff_state_machine()
{
    std::printf("\nthe hand-off state machine\n");

    using handoff = hypervisor::start_up_handoff_state;

    check((handoff::none != handoff::software_wait) &&
              (handoff::none != handoff::hardware_wait) &&
              (handoff::software_wait != handoff::hardware_wait),
          "the three non-delivered states are distinct");

    check(!handoff::is_delivered(handoff::none) &&
              !handoff::is_delivered(handoff::software_wait) &&
              !handoff::is_delivered(handoff::hardware_wait),
          "none of them carries a vector - is_delivered is false for all "
          "three, which is what lets one word answer both questions");

    check((handoff::none < handoff::delivered) &&
              (handoff::software_wait < handoff::delivered) &&
              (handoff::hardware_wait < handoff::delivered),
          "and all three sort below the first delivered state, which is "
          "the whole of why is_delivered can be a comparison");

    // Every vector, not a sample. A start-up IPI's vector is eight bits
    // (SDM 11.6.1, the interrupt command register's vector field), so the
    // whole domain is 256 values and there is no reason to check fewer.
    auto round_tripped = 0;
    auto collided = 0;
    auto not_delivered = 0;
    for (std::uint64_t vector{}; vector < 256; ++vector) {
        auto state = handoff::deliver(vector);

        if (!handoff::is_delivered(state)) {
            ++not_delivered;
        }
        if (handoff::vector(state) != vector) {
            ++round_tripped;
        }
        if ((handoff::none == state) ||
            (handoff::software_wait == state) ||
            (handoff::hardware_wait == state)) {
            ++collided;
        }
    }

    check(0 == not_delivered,
          "is_delivered is true for a hand-off of every one of the 256 "
          "vectors (" +
              std::to_string(not_delivered) + " were not)");
    check(0 == round_tripped,
          "and vector() gives back exactly what deliver() was given, for "
          "all 256 (" +
              std::to_string(round_tripped) + " did not)");
    check(0 == collided,
          "and no delivered state collides with one of the three named "
          "ones (" +
              std::to_string(collided) + " did)");

    // Said separately because it is the case the comment in the header
    // exists for, and the one an "is the mailbox non-zero" test would get
    // wrong: a guest may legitimately start a processor at vector zero.
    check(handoff::is_delivered(handoff::deliver(0)) &&
              (0 == handoff::vector(handoff::deliver(0))) &&
              (handoff::none != handoff::deliver(0)),
          "a hand-off of vector zero is delivered, reads back as zero, "
          "and is not the same word as no hand-off at all");
}

// ------------------- 6. the hand-off is exchanged, never stored
//
// **439abb5.** The reverted change flagged every processor a guest INIT
// named, so the target could apply the INIT to its own VMCS at its next
// exit. The reasoning was sound - KVM's `vmx_apic_init_signal_blocked` is
// `nested.vmxon && !is_guest_mode`, so the layer below blocks an INIT for
// every instant this VMM is in root mode - and the mechanism was still
// wrong, because **a flag carries no ordering against the start-up IPI
// that follows it**. Measured, from a boot that ended in `guest vmxoff`:
//
//     guest start-up ipi for cpu 1, vector 2, to hardware
//     cpu 2 start-up ipi exit, vector 2
//     cpu 1 applying a guest init the layer below did not deliver
//     guest start-up ipi for cpu 1, vector 2, to hardware
//
// The flagged INIT landed *after* the processor had accepted its start-up
// IPI, and put it back in wait-for-SIPI for ever.
//
// What replaced it is not a better flag, it is an exchange: a sender may
// only deliver *out of* `software_wait`, and the target leaves that state
// by the same indivisible operation. So the two cannot both succeed and
// cannot both fail, which is the ordering a flag does not have. Asserted
// here as the value property - a store would satisfy none of it.
static void test_handoff_is_exchanged_not_stored()
{
    std::printf("\nthe hand-off is exchanged, never stored\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::uint64_t wait_for_sipi = 3;
    constexpr std::uint64_t active = 0;

    // Every state a target can be in that is *not* the software wait, with
    // the activity gate deliberately open so that the mailbox is the only
    // thing left deciding. A store would overwrite all three.
    struct
    {
        const char * name;
        std::uint64_t state;
    } states[]{
        {"no hand-off at all", handoff::none},
        {"waiting on hardware", handoff::hardware_wait},
        {"already holding a delivered vector", handoff::deliver(0x11)},
    };

    for (auto & entry : states) {
        reset();
        g_vmm.apic_id[1] = 7;
        g_vmm.number_of_known_processors = 2;
        g_vmm.processor_virtualized[1] = true;
        g_vmm.resume_activity_state[1] = wait_for_sipi;
        g_vmm.start_up_handoff[1].store(entry.state);

        auto answered = g_vmm.start_up_processor(7, 0x33);

        check(hypervisor::start_up_result::needs_hardware == answered,
              std::string("a target ") + entry.name +
                  " is not handed a vector - the guest's own write goes "
                  "out instead");
        check(entry.state == g_vmm.start_up_handoff[1].load(),
              std::string("and its mailbox is left exactly as it was, "
                          "which a plain store would not have done (") +
                  entry.name + ")");
    }

    // The ordering the flag lacked, in the one shape that matters: the
    // target has *left* the software wait. `emulate_init_signal` does
    // that with a compare-exchange to hardware_wait on its own timeout,
    // so from this instant only hardware can move it - and a sender that
    // swallowed the guest's write here would leave nothing to start it
    // with. A flag saying "this target has an INIT pending" would still
    // be set, which is precisely why it was the wrong mechanism.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = wait_for_sipi;
    g_vmm.start_up_handoff[1].store(handoff::software_wait);

    auto expected = handoff::software_wait;
    check(g_vmm.start_up_handoff[1].compare_exchange_strong(
              expected, handoff::hardware_wait),
          "a target can leave the software wait, and leaving it is an "
          "exchange out of software_wait rather than a store");

    check(hypervisor::start_up_result::needs_hardware ==
              g_vmm.start_up_processor(7, 0x44),
          "and a sender arriving one instant later must issue the real "
          "start-up IPI - exactly one of the two happens, which is what a "
          "flag could not have given");

    // And the 439abb5 sequence itself, in the order the log recorded it.
    // A processor that has *accepted* its start-up IPI is running. Nothing
    // arriving afterwards may put it back in wait-for-SIPI, and the reason
    // it cannot is that apply_start_up clears the mailbox on the way out -
    // so there is no vector left for a late arrival to find and no state
    // left for it to be delivered into.
    reset();
    g_vmm.apic_id[1] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.vmcs.vpid(2);
    g_vmm.start_up_handoff[1].store(handoff::deliver(0x2));

    auto context = zpp::arch::x86_64::context{};
    g_vmm.apply_start_up(context, 0x2, "sipi exit");
    g_vmm.processor_virtualized[1] = true;
    g_vmm.resume_activity_state[1] = active;

    check(handoff::none == g_vmm.start_up_handoff[1].load(),
          "a processor that has accepted its start-up IPI holds no "
          "hand-off - apply_start_up clears the mailbox however the "
          "vector arrived");
    check(hypervisor::start_up_result::adopted ==
              g_vmm.start_up_processor(7, 0x2),
          "and the INIT-SIPI pair's second half, arriving late, is "
          "dropped rather than re-applied - 439abb5's flag arrived here "
          "instead and sent a running processor back to its entry point");
    check(handoff::none == g_vmm.start_up_handoff[1].load(),
          "leaving the mailbox empty, so nothing is waiting to be found "
          "by the next INIT either");
}

// ------------------------------------------ 5. the state an INIT leaves
//
// `apply_start_up` is where a processor stops being whatever it was and
// becomes a processor that has just been reset. It is reached three ways
// - out of an INIT exit, out of a start-up IPI exit, and out of a launch
// - and it writes twenty-odd VMCS fields plus the whole general-purpose
// register file. Nothing tested any of it.
//
// The reference is SDM Table 12-1, "IA-32 and Intel 64 Processor States
// Following Power-up, Reset, or INIT", INIT column. CLAUDE.md's "Checking
// architectural claims" section is about this exact table: three
// positions were held on its CR0 row in one afternoon, two of them wrong,
// and only footnote 2 settled it. That footnote is a case below.
//
// Two defects this pins, both of which cost a processor per boot:
//
//   1efec42  The duplicate-start-up guard declined for a *launch* as well
//            as for a start-up IPI, leaving the VMCS holding setup_vmcs's
//            capture of this VMM's own C frame - an unusable CS and a RIP
//            inside this module - which VM entry rejects. Measured as
//            exactly one processor of eight failing per boot, a different
//            one each time.
//
//   c65f8f7  The guard tested a flag that apply_start_up sets for every
//            vector it applies, including the firmware's own broadcast
//            long before an operating system exists - so every start-up
//            IPI a guest later sent was swallowed as a duplicate.
//            Covered here as the positive half: the guard has to fire for
//            a second SIPI and must not fire for the first.

namespace
{
namespace fields = zpp::arch::x86_64::vmx::vmcs_fields;

std::uint64_t vmcs_field(std::uint64_t encoding)
{
    return zpp::arch::x86_64::vmx::g_vmcs[encoding];
}

/**
 * A processor that has been running a 64-bit guest, so that every field
 * apply_start_up is supposed to reset is holding something it must not
 * leave behind.
 *
 * Poisoned rather than zeroed on purpose: a test that starts from zero
 * cannot tell "written to zero" from "never written", and half of what
 * this function does is write zeroes.
 */
void poison_vmcs(std::size_t cpu)
{
    auto & vmcs = g_vmm.vmcs;
    vmcs.vpid(cpu + 1);

    vmcs.guest_cr0(0x80050033);
    vmcs.cr0_read_shadow(0x80050033);
    vmcs.guest_cr3(0x1a2b3000);
    vmcs.guest_cr4(0x372ef8);
    vmcs.cr4_read_shadow(0x372ef8);
    vmcs.guest_rflags(0x246);
    vmcs.guest_rsp(0xdeadbeefc0de);
    vmcs.guest_rip(0xfffff80012345678);
    vmcs.guest_dr7(0xdead);
    vmcs.guest_pending_debug_exceptions(0xff);
    vmcs.guest_activity_state(
        zpp::arch::x86_64::vmx::activity_state::wait_for_start_up_ipi);

    vmcs.guest_cs_selector(0x10);
    vmcs.guest_cs_base(0xcafe0000);
    vmcs.guest_cs_limit(0xdeadbeef);
    vmcs.guest_cs_access_rights(0xa09b);

    vmcs.guest_ss_selector(0x18);
    vmcs.guest_ss_base(0x11110000);
    vmcs.guest_ss_limit(0xdeadbeef);
    vmcs.guest_ss_access_rights(0xc093);

    vmcs.guest_ds_selector(0x18);
    vmcs.guest_ds_base(0x22220000);
    vmcs.guest_es_selector(0x18);
    vmcs.guest_es_base(0x33330000);
    vmcs.guest_fs_selector(0x18);
    vmcs.guest_fs_base(0x44440000);
    vmcs.guest_gs_selector(0x18);
    vmcs.guest_gs_base(0x55550000);
    vmcs.guest_ldtr_selector(0x40);
    vmcs.guest_ldtr_base(0x66660000);
    vmcs.guest_tr_selector(0x48);
    vmcs.guest_tr_base(0x77770000);

    vmcs.guest_gdtr_base(0x88880000);
    vmcs.guest_gdtr_limit(0x57);
    vmcs.guest_idtr_base(0x99990000);
    vmcs.guest_idtr_limit(0xfff);

    // IA-32e mode guest, which apply_start_up has to clear: with CR0.PG
    // going to zero the control and the paging state have to agree or VM
    // entry fails its own consistency checks.
    vmcs.vm_entry_controls(
        zpp::arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest |
        zpp::arch::x86_64::vmx::vm_entry_controls::load_debug_controls);
}

/**
 * A register file with nothing zero in it, for the same reason.
 */
zpp::arch::x86_64::context poisoned_context()
{
    zpp::arch::x86_64::context context{};
    context.rax = 0x1111111111111111;
    context.rbx = 0x2222222222222222;
    context.rcx = 0x3333333333333333;
    context.rdx = 0x4444444444444444;
    context.rsi = 0x5555555555555555;
    context.rdi = 0x6666666666666666;
    context.rbp = 0x7777777777777777;
    context.rsp = 0x8888888888888888;
    context.rip = 0x9999999999999999;
    context.r8 = 0xa;
    context.r9 = 0xb;
    context.r10 = 0xc;
    context.r11 = 0xd;
    context.r12 = 0xe;
    context.r13 = 0xf;
    context.r14 = 0x10;
    context.r15 = 0x11;
    return context;
}

void test_apply_start_up_state()
{
    std::printf("\n-- the state an INIT leaves behind\n");
    reset();

    constexpr std::size_t cpu = 0;
    constexpr std::uint64_t vector = 0x8;

    poison_vmcs(cpu);
    auto context = poisoned_context();
    g_vmm.apply_start_up(context, vector, "test");

    auto & vmcs = g_vmm.vmcs;

    // --- CR0, and the footnote that settles it --------------------
    //
    // SDM Table 12-1 gives 60000010H in the CR0 row's INIT column, which
    // taken literally sets CD and NW and disables this processor's caches
    // for the rest of its life. Footnote 2 on that very row qualifies it:
    // "The CD and NW flags are unchanged, bit 4 is set to 1, all other
    // bits are cleared." The column is the power-up value, where CD and
    // NW happen to be set. KVM writes X86_CR0_ET with CD and NW preserved
    // and a comment saying the SDM contradicts itself; the footnote says
    // KVM is right.
    //
    // The read shadow is what a guest sees, so the architectural value
    // goes there. The real field additionally carries CR0.NE, which
    // IA32_VMX_CR0_FIXED0 requires in VMX operation - unrestricted guest
    // exempts only PE and PG - so a literally architectural CR0 would
    // fail VM entry.
    constexpr std::uint64_t cr0_extension_type = 1ull << 4;
    check(cr0_extension_type == vmcs.cr0_read_shadow(),
          "CR0's read shadow is ET alone - SDM Table 12-1 footnote 2, not "
          "the 60000010H in the column, which is the power-up value and "
          "would leave this processor's caches disabled for ever");
    check(0 != (vmcs.guest_cr0() & (1ull << 5)),
          "the real CR0 keeps NE, which IA32_VMX_CR0_FIXED0 requires in "
          "VMX operation");
    check(cr0_extension_type == (vmcs.guest_cr0() & ~(1ull << 5)),
          "and carries nothing else");

    // CD and NW preserved. The poisoned CR0 has neither set, so the
    // interesting direction is the other one - set them and check they
    // survive.
    reset();
    poison_vmcs(cpu);
    vmcs.guest_cr0(vmcs.guest_cr0() | (1ull << 30) | (1ull << 29));
    context = poisoned_context();
    g_vmm.apply_start_up(context, vector, "test");
    check(0 != (vmcs.cr0_read_shadow() & (1ull << 30)),
          "CR0.CD is preserved across an INIT - footnote 2 again, and "
          "clearing it is the mistake that costs the caches");
    check(0 != (vmcs.cr0_read_shadow() & (1ull << 29)),
          "CR0.NW is preserved across an INIT");

    reset();
    poison_vmcs(cpu);
    context = poisoned_context();
    g_vmm.apply_start_up(context, vector, "test");

    // --- CR3, CR4 and the entry control ---------------------------
    check(0 == vmcs.guest_cr3(), "CR3 is zero after an INIT");
    check(0 == vmcs.cr4_read_shadow(),
          "CR4's read shadow is zero after an INIT");
    check(0 != (vmcs.guest_cr4() & (1ull << 13)),
          "the real CR4 keeps VMXE, which IA32_VMX_CR4_FIXED0 requires");
    check(0 == (vmcs.guest_cr4() & ~(1ull << 13)),
          "and carries nothing else");

    check(
        0 ==
            (vmcs.vm_entry_controls() &
             zpp::arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest),
        "the IA-32e mode guest entry control is cleared - long mode is "
        "gone with CR0.PG and VM entry checks that the two agree");
    check(0 != (vmcs.vm_entry_controls() &
                zpp::arch::x86_64::vmx::vm_entry_controls::
                    load_debug_controls),
          "and every other entry control is left alone - clearing the "
          "whole field would take load-debug-controls with it, and DR7 "
          "and IA32_DEBUGCTL are written a few lines above on the "
          "strength of it being set");

    // --- RFLAGS, RSP, DR6, DR7 ------------------------------------
    check(0x2 == vmcs.guest_rflags(),
          "RFLAGS is 00000002H after an INIT - SDM Table 12-1");
    check(0 == vmcs.guest_rsp(), "RSP is zero after an INIT");
    check(0x400 == vmcs.guest_dr7(),
          "DR7 is 00000400H after an INIT - SDM Table 12-1");
    check(0xffff0ff0 == zpp::arch::x86_64::g_dr6,
          "DR6 is FFFF0FF0H after an INIT, written to the real register "
          "because the guest and host share it - it is not a VMCS guest "
          "field");
    check(0 == vmcs.guest_pending_debug_exceptions(),
          "the pending debug exceptions field is cleared");

    // --- CS:IP, which is the whole point of the vector ------------
    //
    // The start-up vector is a page number, so the segment base is the
    // vector scaled by a page and the selector is that base shifted down
    // by the four bits real mode already implies. KVM's
    // kvm_vcpu_deliver_sipi_vector sets the same three fields and nothing
    // else.
    check((vector << 8) == vmcs.guest_cs_selector(),
          "CS's selector is the vector scaled to a real-mode selector");
    check((vector << 12) == vmcs.guest_cs_base(),
          "CS's base is the vector scaled by a page");
    check(0xffff == vmcs.guest_cs_limit(),
          "CS's limit is 0000FFFFH - SDM Table 12-1's real-mode segment");
    check(0 == vmcs.guest_rip(),
          "RIP is zero, so execution begins at the base");

    // --- Every other segment is a real-mode segment at zero -------
    struct
    {
        const char * name;
        std::uint64_t selector;
        std::uint64_t base;
        std::uint64_t limit;
    } segments[]{
        {"SS",
         fields::guest_ss_selector,
         fields::guest_ss_base,
         fields::guest_ss_limit},
        {"DS",
         fields::guest_ds_selector,
         fields::guest_ds_base,
         fields::guest_ds_limit},
        {"ES",
         fields::guest_es_selector,
         fields::guest_es_base,
         fields::guest_es_limit},
        {"FS",
         fields::guest_fs_selector,
         fields::guest_fs_base,
         fields::guest_fs_limit},
        {"GS",
         fields::guest_gs_selector,
         fields::guest_gs_base,
         fields::guest_gs_limit},
        {"LDTR",
         fields::guest_ldtr_selector,
         fields::guest_ldtr_base,
         fields::guest_ldtr_limit},
        {"TR",
         fields::guest_tr_selector,
         fields::guest_tr_base,
         fields::guest_tr_limit},
    };

    for (auto & segment : segments) {
        check(0 == vmcs_field(segment.selector),
              std::string(segment.name) + "'s selector is zero");
        check(0 == vmcs_field(segment.base),
              std::string(segment.name) + "'s base is zero");
        check(0xffff == vmcs_field(segment.limit),
              std::string(segment.name) +
                  "'s limit is 0000FFFFH - a real-mode segment");
    }

    // The descriptor tables: base zero, limit FFFFH.
    check(0 == vmcs.guest_gdtr_base(), "the GDTR's base is zero");
    check(0xffff == vmcs.guest_gdtr_limit(),
          "the GDTR's limit is 0000FFFFH");
    check(0 == vmcs.guest_idtr_base(), "the IDTR's base is zero");
    check(0xffff == vmcs.guest_idtr_limit(),
          "the IDTR's limit is 0000FFFFH");

    // --- The access rights, built from a descriptor rather than
    //     written as a number --------------------------------------
    //
    // Checked against the descriptor the code builds rather than against
    // a literal, because the point of building them that way is that the
    // VMX encoding and the descriptor encoding cannot drift apart. What
    // this asserts is the part a literal could not: that CS is a code
    // segment and the data segments are data segments, that all of them
    // are present and 16-bit, and that none is marked unusable - VM entry
    // rejects an unusable CS, which is exactly what the 1efec42 defect
    // left behind.
    constexpr std::uint64_t access_rights_unusable = 1ull << 16;
    constexpr std::uint64_t access_rights_present = 1ull << 7;
    constexpr std::uint64_t access_rights_default_size = 1ull << 14;
    constexpr std::uint64_t access_rights_granularity = 1ull << 15;
    constexpr std::uint64_t access_rights_long_mode = 1ull << 13;

    struct
    {
        const char * name;
        std::uint64_t field;
        std::uint64_t type;
        bool system;
    } rights[]{
        {"CS", fields::guest_cs_access_rights, 0xb, false},
        {"SS", fields::guest_ss_access_rights, 0x3, false},
        {"DS", fields::guest_ds_access_rights, 0x3, false},
        {"ES", fields::guest_es_access_rights, 0x3, false},
        {"FS", fields::guest_fs_access_rights, 0x3, false},
        {"GS", fields::guest_gs_access_rights, 0x3, false},
        {"LDTR", fields::guest_ldtr_access_rights, 0x2, true},
        {"TR", fields::guest_tr_access_rights, 0xb, true},
    };

    for (auto & entry : rights) {
        auto value = vmcs_field(entry.field);
        check(0 == (value & access_rights_unusable),
              std::string(entry.name) +
                  " is not marked unusable - VM entry rejects an unusable "
                  "CS, which is what a declined apply_start_up used to "
                  "leave behind");
        check(0 != (value & access_rights_present),
              std::string(entry.name) + " is present");
        check(entry.type == (value & 0xf),
              std::string(entry.name) + " has the right descriptor type");
        check(entry.system == (0 == (value & (1ull << 4))),
              std::string(entry.name) +
                  " agrees with the SDM about being a system descriptor");
        check(0 == (value & access_rights_default_size),
              std::string(entry.name) +
                  " is 16-bit - the default operation size bit is clear");
        check(0 == (value & access_rights_granularity),
              std::string(entry.name) +
                  " is byte granular, so its limit means bytes");
        check(0 == (value & access_rights_long_mode),
              std::string(entry.name) + " is not a 64-bit code segment");
    }

    // --- The general-purpose registers ----------------------------
    //
    // Architecturally defined after an INIT too, and not in the VMCS -
    // they live in the context this VMM saved on the way in. SDM Table
    // 12-1: EAX zero, EDX the family, model and stepping, the rest zero.
    //
    // RIP and RSP are load bearing on the launch path specifically:
    // vm_launch takes them from this context rather than from the VMCS,
    // so leaving either holding where this VMM happened to be would start
    // the guest there instead of at its entry point.
    check(0 == context.rax, "RAX is zero after an INIT");
    check(0 == context.rbx, "RBX is zero after an INIT");
    check(0 == context.rcx, "RCX is zero after an INIT");
    check(zpp::arch::x86_64::identification_leaf_1_eax == context.rdx,
          "RDX holds CPUID leaf 1's EAX - the family, model and stepping "
          "- which is the one register SDM Table 12-1 does not zero");
    check(0 == context.rsi && 0 == context.rdi && 0 == context.rbp,
          "RSI, RDI and RBP are zero after an INIT");
    check(0 == context.rip,
          "RIP in the context is zero - vm_launch takes the guest's RIP "
          "from here rather than from the VMCS");
    check(0 == context.rsp,
          "RSP in the context is zero, for the same reason");
    check(0 == context.r8 && 0 == context.r9 && 0 == context.r10 &&
              0 == context.r11 && 0 == context.r12 && 0 == context.r13 &&
              0 == context.r14 && 0 == context.r15,
          "R8 through R15 are zero after an INIT");

    // --- Runnable again -------------------------------------------
    check(zpp::arch::x86_64::vmx::activity_state::active ==
              vmcs.guest_activity_state(),
          "the activity state is active - a processor that has been "
          "started is running, and leaving it in wait-for-SIPI is a "
          "processor that never starts");

    // --- The hand-off is over, however it arrived -----------------
    check(hypervisor::start_up_handoff_state::none ==
              g_vmm.start_up_handoff[cpu].load(),
          "the software hand-off mailbox is cleared - a delivered vector "
          "left behind would let the next INIT find a start-up nobody "
          "sent this time");
}

/**
 * The vector is a page number, and every value of it has to land.
 */
void test_apply_start_up_vectors()
{
    std::printf("\n-- the start-up vector scales to a real-mode CS\n");

    for (std::uint64_t vector : {std::uint64_t{0},
                                 std::uint64_t{1},
                                 std::uint64_t{0x8},
                                 std::uint64_t{0x7f},
                                 std::uint64_t{0x80},
                                 std::uint64_t{0xff}}) {
        reset();
        poison_vmcs(0);
        auto context = poisoned_context();
        g_vmm.apply_start_up(context, vector, "test");

        check((vector << 12) == g_vmm.vmcs.guest_cs_base(),
              "vector " + std::to_string(vector) +
                  " scales to a base a page apart from its neighbour");
        check((vector << 8) == g_vmm.vmcs.guest_cs_selector(),
              "vector " + std::to_string(vector) +
                  " scales to the matching selector");
    }
}

/**
 * The duplicate guard, and the one exemption that makes it correct.
 */
void test_apply_start_up_duplicate_guard()
{
    std::printf("\n-- the duplicate start-up guard\n");

    // The first start-up IPI of an INIT-SIPI-SIPI applies.
    reset();
    poison_vmcs(0);
    auto context = poisoned_context();
    g_vmm.apply_start_up(context, 0x8, "first sipi");
    check((0x8ull << 12) == g_vmm.vmcs.guest_cs_base(),
          "the first start-up IPI is applied");
    check(g_vmm.started_by_start_up_ipi[0],
          "and the processor is flagged as started");

    // The second is ignored. INIT-SIPI-SIPI sends two, and applying the
    // second sends a processor that is already running back to its entry
    // point - which wedges it in a way indistinguishable from never
    // having started. Guarded here rather than relying on the hardware to
    // discard it, which it does not do reliably.
    g_vmm.vmcs.guest_cs_base(0xdeadb000);
    g_vmm.apply_start_up(context, 0x9, "second sipi");
    check(0xdeadb000 == g_vmm.vmcs.guest_cs_base(),
          "the second start-up IPI of an INIT-SIPI-SIPI is ignored - "
          "applying it would send a running processor back to its entry "
          "point");

    // **1efec42.** A processor being launched out of the trampoline is
    // being started for the first time whatever the flag says, and
    // honouring the guard there is a contradiction rather than a
    // conservatism: declining leaves the VMCS holding setup_vmcs's
    // capture of this VMM's own C frame - an unusable CS and a RIP inside
    // this module - and VM entry rejects it. Measured as exactly one
    // processor of eight failing per boot, a different one each time.
    g_vmm.vmcs.guest_cs_base(0xdeadb000);
    g_vmm.apply_start_up(context, 0xa, "launch", true);
    check((0xaull << 12) == g_vmm.vmcs.guest_cs_base(),
          "a launch applies the start-up state even with the flag set - "
          "1efec42, and declining leaves an unusable CS that VM entry "
          "rejects");

    // And the guard is per processor, not global. A slot is not a label:
    // two processors sharing one would be invariant 2 by another route.
    reset();
    poison_vmcs(0);
    context = poisoned_context();
    g_vmm.apply_start_up(context, 0x8, "cpu 0 sipi");
    check(g_vmm.started_by_start_up_ipi[0], "cpu 0 is flagged");
    check(!g_vmm.started_by_start_up_ipi[1],
          "cpu 1 is not flagged by cpu 0's start-up - the guard is per "
          "processor");

    g_vmm.vmcs.vpid(2);
    g_vmm.vmcs.guest_cs_base(0xdeadb000);
    g_vmm.apply_start_up(context, 0xb, "cpu 1 sipi");
    check((0xbull << 12) == g_vmm.vmcs.guest_cs_base(),
          "cpu 1's first start-up IPI applies even though cpu 0 has "
          "already had one");
}

// ------------- 7. the INIT handler publishes before it waits
//
// **f949649.** `start_up_processor` decides what to do with a guest's
// start-up IPI by reading the activity record and only then exchanging
// into the mailbox. The gate comes first, so a target whose record does
// not yet say wait-for-SIPI has its IPI **dropped** - and dropped is
// returned as `adopted`, which swallows the guest's write to the
// interrupt command register. The vector is destroyed, not delivered.
//
// The record used to be written after the wait, by the exit path on the
// way out of the handler. So for the entire length of the software wait -
// up to two million iterations, covering both of the two start-up IPIs
// SDM 11.4.4.1 step 15 has a guest send - it still said `active`, and the
// software hand-off was unreachable from the sender.
//
// This is the half the existing hand-off test could not state, because it
// drives the sender alone: what has to be true is that the *target* has
// published both facts before it is observable as waiting at all. Two
// host threads, one running the real INIT handler and one standing in for
// the sender's exit handler.

/**
 * Waits for `predicate`, bounded. Returns whether it came true - a
 * harness that spun for ever on a defect would be reporting nothing.
 */
template <typename Predicate>
static bool eventually(Predicate predicate)
{
    for (auto attempt = 0; attempt < 200000000; ++attempt) {
        if (predicate()) {
            return true;
        }
    }
    return false;
}

/**
 * Runs the real `emulate_init_signal` on its own thread, as the target's
 * exit handler would, and records what it left behind. The VMCS shim is
 * per thread - deliberately, so that one thread's VMWRITEs cannot be read
 * back by another - so everything the caller needs has to be sampled here
 * rather than afterwards.
 */
struct init_target
{
    std::size_t cpu{};
    std::atomic<bool> returned{false};
    std::atomic<std::uint64_t> cs_base{};
    std::atomic<std::uint64_t> activity_left{};

    void run()
    {
        g_vmm.vmcs.vpid(this->cpu + 1);

        // A processor that has been running the guest, so that what
        // apply_start_up writes is distinguishable from what was there.
        g_vmm.vmcs.guest_cs_base(0xdeadb000);
        g_vmm.vmcs.guest_activity_state(
            zpp::arch::x86_64::vmx::activity_state::active);

        zpp::arch::x86_64::context context{};
        g_vmm.emulate_init_signal(context);

        this->cs_base.store(g_vmm.vmcs.guest_cs_base());
        this->activity_left.store(g_vmm.vmcs.guest_activity_state());
        this->returned.store(true);
    }
};

/**
 * Under a layer that discards a start-up IPI while this VMM is in root
 * mode, which is the only situation the software wait exists for: x2APIC,
 * because only then is the interrupt command register an MSR this VMM can
 * see, and something virtualizing us.
 */
static void nested_and_x2apic()
{
    zpp::arch::x86_64::g_apic_base.store(0xfee00000 | (1ull << 10));
    zpp::arch::x86_64::g_leaf_1_ecx =
        zpp::arch::x86_64::hypervisor_present_bit;
}

static void test_init_publishes_before_it_waits()
{
    std::printf("\nthe INIT handler publishes before it waits\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::uint64_t wait_for_sipi = 3;
    constexpr std::size_t cpu = 1;

    reset();
    nested_and_x2apic();
    g_vmm.apic_id[cpu] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[cpu] = true;

    // The record the sender gates on, holding the value it holds for real:
    // whatever the exit before the INIT left there, which is `active`.
    g_vmm.resume_activity_state[cpu] = 0;

    init_target target{cpu};
    std::thread thread{[&] { target.run(); }};

    auto listening = eventually([&] {
        return handoff::software_wait ==
               g_vmm.start_up_handoff[cpu].load();
    });
    check(listening,
          "the target publishes that it is listening on the software "
          "hand-off");

    // The whole of f949649, in one line: at the instant the target is
    // observable as listening, the record a sender gates on already says
    // wait-for-SIPI. Sampled while the target is still inside its wait,
    // which is the only time it proves anything.
    check(wait_for_sipi == g_vmm.resume_activity_state[cpu],
          "and by then the activity record already says wait-for-SIPI - "
          "published *before* the wait, so a sender arriving during it "
          "passes the gate instead of dropping the vector");
    check(!target.returned.load(),
          "and the target has not returned, so that was read mid-wait "
          "rather than after the handler finished");

    // Now the sender, from its own exit handler, exactly as
    // on_interrupt_command reaches it.
    auto answered = g_vmm.start_up_processor(7, 0x30);
    check(hypervisor::start_up_result::adopted == answered,
          "a start-up IPI arriving mid-wait is adopted, and adopted here "
          "means delivered rather than dropped");

    thread.join();

    check(g_vmm.started_by_start_up_ipi[cpu],
          "the target applied it - which is what makes the adoption above "
          "a delivery. Reported adopted without applying is the swallowed "
          "write f949649 is about");
    check((0x30ull << 12) == target.cs_base.load(),
          "at the vector the guest asked for");
    check(zpp::arch::x86_64::vmx::activity_state::active ==
              target.activity_left.load(),
          "and the target is runnable again, not left parked in "
          "wait-for-SIPI");
    check(handoff::none == g_vmm.start_up_handoff[cpu].load(),
          "with the mailbox cleared behind it");
}

/**
 * The same, repeated, because once is a demonstration and the sender and
 * the target are on two processors: whichever order they interleave in,
 * the vector is consumed exactly once - never twice, never not at all.
 */
static void test_handoff_race_is_exactly_once()
{
    std::printf("\nthe hand-off consumes a vector exactly once\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::size_t cpu = 1;
    constexpr int rounds = 24;

    auto delivered = 0;
    auto to_hardware = 0;
    auto both = 0;
    auto neither = 0;

    for (auto round = 0; round < rounds; ++round) {
        reset();
        nested_and_x2apic();
        g_vmm.apic_id[cpu] = 7;
        g_vmm.number_of_known_processors = 2;
        g_vmm.processor_virtualized[cpu] = true;
        g_vmm.resume_activity_state[cpu] = 0;

        init_target target{cpu};
        std::thread thread{[&] { target.run(); }};

        // Only once the record is published, because the sender gating on
        // a record that is not there yet is a window this harness cannot
        // close and the guest's own 210 microseconds covers - see the
        // BACKLOG section this file is cited from.
        eventually([&] {
            return handoff::software_wait ==
                       g_vmm.start_up_handoff[cpu].load() ||
                   target.returned.load();
        });

        // Three arrivals, so that both sides of the target's timeout are
        // reached rather than hoped for: inside the wait, somewhere in
        // it, and after it has given up. The exact instant of the two
        // exchanges colliding is not reproducible from host threads -
        // what is reproducible is that every ordering either side of it
        // obeys the same invariant.
        switch (round % 3) {
        case 0:
            break;
        case 1:
            for (auto spin = 0; spin < 900000; ++spin) {
                zpp::spin_hint();
            }
            break;
        default:
            eventually([&] { return target.returned.load(); });
            break;
        }

        auto answered = g_vmm.start_up_processor(7, 0x30);
        thread.join();

        auto applied = g_vmm.started_by_start_up_ipi[cpu];
        auto hardware =
            (hypervisor::start_up_result::needs_hardware == answered);

        if (applied && hardware) {
            ++both;
        } else if (!applied && !hardware) {
            ++neither;
        } else if (applied) {
            ++delivered;
        } else {
            ++to_hardware;
        }
    }

    check(0 == both,
          "no round both handed the vector over and issued a real "
          "start-up IPI for it (" +
              std::to_string(both) + " did)");
    check(0 == neither,
          "and no round lost it - a swallowed write with nothing left to "
          "start the processor is the failure this exists to catch (" +
              std::to_string(neither) + " did)");
    check((0 != delivered) && (0 != to_hardware),
          "and both outcomes were actually reached over " +
              std::to_string(rounds) + " rounds (" +
              std::to_string(delivered) + " handed over, " +
              std::to_string(to_hardware) +
              " to hardware) - an invariant "
              "only one branch ever reaches is not being tested");
}

/**
 * Which hand-off the INIT handler chooses, and the fact that it says so
 * either way. Waiting in software is worth it in exactly one situation -
 * a layer below that discards the IPI while this VMM is in root mode - so
 * it is taken there and nowhere else, and every other case has to publish
 * `hardware_wait` rather than leave the mailbox at whatever it held.
 */
static void test_init_chooses_and_publishes_a_handoff()
{
    std::printf("\nthe INIT handler chooses a hand-off and says which\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::uint64_t wait_for_sipi = 3;
    constexpr std::size_t cpu = 1;

    struct
    {
        const char * name;
        bool x2apic;
        bool nested;
    } cases[]{
        {"bare metal", false, false},
        {"bare metal in x2APIC mode", true, false},
        {"under a layer, but in xAPIC mode", false, true},
    };

    for (auto & entry : cases) {
        reset();
        if (entry.x2apic) {
            zpp::arch::x86_64::g_apic_base.store(0xfee00000 |
                                                 (1ull << 10));
        }
        if (entry.nested) {
            zpp::arch::x86_64::g_leaf_1_ecx =
                zpp::arch::x86_64::hypervisor_present_bit;
        }

        init_target target{cpu};
        target.run();

        check(handoff::hardware_wait == g_vmm.start_up_handoff[cpu].load(),
              std::string(entry.name) +
                  " waits on hardware, and says so - a sender that assumed"
                  " otherwise is what used to swallow the IPI");
        check(wait_for_sipi == g_vmm.resume_activity_state[cpu],
              std::string(entry.name) +
                  " publishes wait-for-SIPI in the record a sender gates "
                  "on");
        check(wait_for_sipi == target.activity_left.load(),
              std::string(entry.name) +
                  " parks the VMCS in wait-for-SIPI, which is what makes "
                  "the hardware delivery a VM exit - SDM 28.2");
        check(!g_vmm.started_by_start_up_ipi[cpu],
              std::string(entry.name) +
                  " clears the started flag, because an INIT is what makes"
                  " the next start-up IPI the first of its sequence");
    }

    // And the software wait's own timeout, which is the branch the
    // exchange at the end of it exists for. Nobody sends, so it runs to
    // its bound and falls back - bounded because a processor spinning for
    // ever on an INIT whose start-up IPI never comes would take the
    // machine down with no diagnosis.
    reset();
    nested_and_x2apic();
    g_vmm.apic_id[cpu] = 7;
    g_vmm.number_of_known_processors = 2;
    g_vmm.processor_virtualized[cpu] = true;

    init_target target{cpu};
    target.run();

    check(handoff::hardware_wait == g_vmm.start_up_handoff[cpu].load(),
          "a software wait nobody answered gives up and falls back to the "
          "hardware path, which is correct on real hardware and no worse "
          "than what came before anywhere else");
    check(!g_vmm.started_by_start_up_ipi[cpu], "having applied nothing");

    g_vmm.resume_activity_state[cpu] = wait_for_sipi;
    check(hypervisor::start_up_result::needs_hardware ==
              g_vmm.start_up_processor(7, 0x30),
          "and a sender arriving after the fall-back issues the guest's "
          "own start-up IPI rather than swallowing it");
}

// -------------- 8. the firmware's start-up is not the guest's
//
// **c65f8f7, then 95d9759.** The duplicate-start-up guard used to test a
// flag that `apply_start_up` sets for *every* vector it applies -
// including the seven processors the firmware brings up with its own
// broadcast start-up IPI, long before an operating system exists. So by
// the time Windows started its processors, all seven were already flagged
// and every one of their start-up IPIs was swallowed as a duplicate.
// c65f8f7 added a second flag; 95d9759 measured slots 2-7 with *both*
// flags set while their VMCS activity state was 3, wait-for-SIPI, and
// replaced the pair with the architectural fact.
//
// KVM makes exactly one test, in `kvm_apic_accept_events`: SIPIs are
// dropped if the target is not in wait-for-SIPI. There is no "already
// started" flag anywhere in it.
static void test_firmware_start_up_is_not_the_guest_s()
{
    std::printf("\nthe firmware's start-up is not the guest's\n");

    using handoff = hypervisor::start_up_handoff_state;
    constexpr std::uint64_t wait_for_sipi = 3;
    constexpr std::uint64_t active = 0;
    constexpr std::size_t processors = 8;

    // x2APIC, which is the mode a guest whose interrupt command register
    // this VMM can see at all is in, and one MSR write per real start-up
    // IPI - which is how the sends are counted below.
    auto firmware_phase = [&] {
        reset();
        zpp::arch::x86_64::g_apic_base.store(0xfee00000 | (1ull << 10));

        g_vmm.number_of_platform_processors = processors;
        g_vmm.number_of_known_processors = processors;
        for (std::size_t slot{}; slot < processors; ++slot) {
            g_vmm.apic_id[slot] = slot;
            g_vmm.platform_apic_id[slot] = slot;
        }

        // The firmware's own broadcast start-up IPI, applied by each
        // target exactly as a SIPI exit would - which is what sets the
        // flag the guard used to trust.
        for (std::size_t slot = 1; slot < processors; ++slot) {
            g_vmm.vmcs.vpid(slot + 1);
            auto context = zpp::arch::x86_64::context{};
            g_vmm.apply_start_up(context, 0x99, "firmware sipi");
            g_vmm.processor_virtualized[slot] = true;
        }
    };

    firmware_phase();

    auto flagged = 0;
    for (std::size_t slot = 1; slot < processors; ++slot) {
        flagged += g_vmm.started_by_start_up_ipi[slot] ? 1 : 0;
    }
    check((processors - 1) == static_cast<std::size_t>(flagged),
          "the firmware's broadcast leaves every application processor "
          "flagged as started - which is the state c65f8f7 found, and the "
          "reason a flag cannot answer the guest's question");

    // The guest's phase. Windows INITs them, which is what puts them back
    // in wait-for-SIPI, and then broadcasts a start-up IPI - the only
    // form it sends.
    for (std::size_t slot = 1; slot < processors; ++slot) {
        g_vmm.resume_activity_state[slot] = wait_for_sipi;
        g_vmm.start_up_handoff[slot].store(handoff::hardware_wait);
    }

    zpp::arch::x86_64::g_x2apic_icr_writes.store(0);
    check(g_vmm.start_up_broadcast(0x8),
          "the guest's broadcast start-up IPI is resolved against the "
          "platform roster rather than passed through");
    check(
        (processors - 1) == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
        "and every one of the seven gets a start-up IPI - not one is "
        "swallowed as a duplicate of the firmware's, which is the whole "
        "of c65f8f7 (" +
            std::to_string(zpp::arch::x86_64::g_x2apic_icr_writes.load()) +
            " were sent)");

    // And the decision is the activity state, not a flag: the same
    // processors, the same flags, the only difference being what they say
    // they are doing.
    firmware_phase();
    for (std::size_t slot = 1; slot < processors; ++slot) {
        g_vmm.resume_activity_state[slot] = active;
        g_vmm.start_up_handoff[slot].store(handoff::hardware_wait);
    }

    zpp::arch::x86_64::g_x2apic_icr_writes.store(0);
    check(g_vmm.start_up_broadcast(0x8),
          "a broadcast to processors that are all running is still "
          "resolved");
    check(0 == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
          "and every one of those start-up IPIs is dropped - identical "
          "flags, opposite answers, so the decision is the activity state "
          "and not the flag 95d9759 removed");

    // The third answer, so that all three of the guard's outcomes are
    // covered by the same setup: a target that is parked in the software
    // wait is handed its vector and gets no hardware IPI at all.
    firmware_phase();
    for (std::size_t slot = 1; slot < processors; ++slot) {
        g_vmm.resume_activity_state[slot] = wait_for_sipi;
        g_vmm.start_up_handoff[slot].store(handoff::software_wait);
    }

    zpp::arch::x86_64::g_x2apic_icr_writes.store(0);
    static_cast<void>(g_vmm.start_up_broadcast(0x8));

    auto handed = 0;
    for (std::size_t slot = 1; slot < processors; ++slot) {
        auto state = g_vmm.start_up_handoff[slot].load();
        handed += (handoff::is_delivered(state) &&
                   (0x8 == handoff::vector(state)))
                      ? 1
                      : 0;
    }
    check((processors - 1) == static_cast<std::size_t>(handed),
          "a broadcast to processors listening on the software hand-off "
          "hands each of them the vector (" +
              std::to_string(handed) + " of 7)");
    check(0 == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
          "and issues no hardware start-up IPI for any of them, because "
          "each is in root mode where hardware could not deliver one");
}

// ------------------- 9. one sender, branching on the APIC mode
//
// **2685265.** Both places that start a processor wrote the x2APIC
// interrupt command MSR unconditionally. That MSR does not exist while
// the local APIC is in xAPIC mode, so the write takes a general
// protection fault - measured as host exception vector 13 with the
// faulting RIP inside `wrmsr`, and the boot processor halted in
// `on_host_exception` with nothing in the VM exit records to explain it,
// because the fault was in this VMM rather than in anything the guest
// did.
//
// The two forms are not one line with a flag: x2APIC is a single 64-bit
// write carrying a 32-bit destination, xAPIC is two 32-bit writes with an
// eight-bit destination in 31:24, and the high half must go first because
// writing the low half is what sends the interrupt.
static void test_start_up_ipi_follows_the_apic_mode()
{
    std::printf("\none sender, branching on the APIC mode\n");

    constexpr std::uint64_t delivery_mode_start_up = 0x6ull << 8;
    constexpr std::uint64_t level_assert = 1ull << 14;
    constexpr std::uint64_t apic_page = 0xfee00000;

    // x2APIC: one MSR write, and the destination is thirty-two bits wide,
    // which is the other reason these cannot share a line.
    reset();
    zpp::arch::x86_64::g_apic_base.store(apic_page | (1ull << 10));
    g_vmm.send_start_up_ipi(0x101, 0x8);

    check(1 == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
          "in x2APIC mode the command is one write to the interrupt "
          "command MSR");
    check(0 == zpp::arch::x86_64::g_mmio_write_count.load(),
          "and nothing goes through the APIC page, which in that mode is "
          "not where the register is");
    check((0x8ull | delivery_mode_start_up | (0x101ull << 32)) ==
              zpp::arch::x86_64::g_x2apic_icr_last.load(),
          "carrying delivery mode 110b and all thirty-two bits of the "
          "destination");

    // xAPIC: two writes through the page, and *no* MSR write. The count
    // being zero is the assertion - one is the #GP that halted the boot
    // processor.
    reset();
    zpp::arch::x86_64::g_apic_base.store(apic_page);
    g_vmm.send_start_up_ipi(0x1f, 0x8);

    check(0 == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
          "in xAPIC mode the interrupt command MSR is not written at all "
          "- it does not exist there, and writing it takes a #GP in the "
          "host with no recovery point");
    check(2 == zpp::arch::x86_64::g_mmio_write_count.load(),
          "the command goes through the APIC page instead, as two 32-bit "
          "writes");

    auto & first = zpp::arch::x86_64::g_mmio_writes[0];
    auto & second = zpp::arch::x86_64::g_mmio_writes[1];

    check((apic_page + 0x310) == first.address,
          "the high half first - writing the low half is what sends the "
          "interrupt, so a destination written after it would be written "
          "after the thing that used it");
    check((0x1fu << 24) == first.value,
          "with eight bits of destination in 31:24");
    check((apic_page + 0x300) == second.address,
          "and the low half second");
    check(static_cast<std::uint32_t>(0x8 | delivery_mode_start_up |
                                     level_assert) == second.value,
          "carrying the vector, delivery mode 110b and level assert");
    check(first.order < second.order,
          "in that order, which is the half of this a two-field check "
          "cannot see");

    // The frame is read from IA32_APIC_BASE rather than assumed, so a
    // firmware that put the APIC somewhere else is still answered.
    reset();
    zpp::arch::x86_64::g_apic_base.store(0xfed00000 | 0x800);
    g_vmm.send_start_up_ipi(0x2, 0x8);
    check((0xfed00000 + 0x310) ==
              zpp::arch::x86_64::g_mmio_writes[0].address,
          "and the page comes from IA32_APIC_BASE's frame, masked, rather "
          "than from a constant");
}

// ---------------- 10. an INIT is forwarded and nothing more
//
// The other half of **439abb5**, and the half the hand-off tests above
// cannot reach: what the *sender's* exit handler does with an INIT. The
// reverted change flagged every processor a guest INIT named, and its
// second measured fault was that it flagged them for an INIT **level
// de-assert** as well - command 0x100008500, level clear and trigger mode
// level - which architecturally starts nothing at all. So it could invent
// an INIT, which the comment it added explicitly denied it could.
//
// Nothing here can prove a queue would be correct. What it pins is the
// property the flag broke: an INIT is passed through as the guest wrote
// it and changes no per-processor state, so re-introducing a flag has to
// fail these rather than pass them silently.
static void test_init_is_forwarded_and_nothing_more()
{
    std::printf("\nan INIT is forwarded and nothing more\n");

    using handoff = hypervisor::start_up_handoff_state;

    // Delivery mode 101b is INIT. Bit 14 is level assert and bit 15 is
    // trigger mode; the destination is in 63:32, physical mode.
    constexpr std::uint64_t init_assert = 0x100004500;
    constexpr std::uint64_t init_level_de_assert = 0x100008500;

    struct
    {
        const char * name;
        std::uint64_t command;
    } inits[]{
        {"an INIT", init_assert},
        {"an INIT level de-assert, which starts nothing at all",
         init_level_de_assert},
    };

    for (auto & entry : inits) {
        reset();
        zpp::arch::x86_64::g_apic_base.store(0xfee00000 | (1ull << 10));
        g_vmm.apic_id[1] = 1;
        g_vmm.number_of_known_processors = 2;
        g_vmm.processor_virtualized[1] = true;
        g_vmm.resume_activity_state[1] = 0;

        auto issue = g_vmm.on_interrupt_command(entry.command);

        check(issue && (entry.command == *issue),
              std::string(entry.name) +
                  " is passed through as the guest wrote it - it is what "
                  "leaves the target waiting for a start-up IPI, and "
                  "there is nothing here that improves on it");
        check(handoff::none == g_vmm.start_up_handoff[1].load(),
              std::string(entry.name) +
                  " hands nothing to the target's mailbox");
        check(!g_vmm.started_by_start_up_ipi[1] &&
                  !g_vmm.started_by_guest_start_up_ipi[1],
              std::string(entry.name) +
                  " marks the target as neither started nor asked for - a "
                  "flag set here is 439abb5, and for the de-assert it is "
                  "an INIT the guest never sent");
        check(0 == zpp::arch::x86_64::g_x2apic_icr_writes.load(),
              std::string(entry.name) +
                  " issues no start-up IPI of this VMM's own");
    }

    // And a start-up IPI in logical destination mode, which is the other
    // way a command can name something that is not a processor - c6349a4.
    // The sharp end of that one is not the refusal, it is that asking
    // `processor_slot` about a bitmask *allocates a slot for it*, spending
    // an entry of a fixed table on a processor that does not exist.
    reset();
    zpp::arch::x86_64::g_apic_base.store(0xfee00000 | (1ull << 10));
    g_vmm.apic_id[0] = 0;
    g_vmm.number_of_known_processors = 1;

    constexpr std::uint64_t logical_start_up_ipi = 0x100000e08;
    auto issue = g_vmm.on_interrupt_command(logical_start_up_ipi);

    check(issue && (logical_start_up_ipi == *issue),
          "a start-up IPI in logical destination mode is passed through "
          "rather than mis-resolved - SDM 13.6.1, bit 11, where the "
          "destination field is a bitmask and not an identifier");
    check(1 == g_vmm.number_of_known_processors,
          "and no slot is allocated for it, which is the part that costs: "
          "a slot spent on a processor that does not exist hands the rest "
          "of the decision an index naming the wrong one");
}

} // namespace

// ---------------------------------------------------------------- main
int main()
{
    // The blob and the trampoline page have to be big enough for the area
    // start_application_processor writes into, or the code under test
    // would be running past the end of the harness's memory rather than
    // being tested.
    static_assert(sizeof(g_trampoline) >=
                  zpp::arch::x86_64::ap_start_up_area_offset +
                      sizeof(zpp::arch::x86_64::ap_start_up_area));
    static_assert(sizeof(zpp_ap_start_up_begin) >=
                  zpp::arch::x86_64::ap_start_up_area_offset +
                      sizeof(zpp::arch::x86_64::ap_start_up_area));

    test_one_vmcs_per_processor();
    test_slot_allocation();
    test_running_processor_stays_launched();
    test_handoff_is_obeyed();
    test_handoff_state_machine();
    test_handoff_is_exchanged_not_stored();
    test_apply_start_up_state();
    test_apply_start_up_vectors();
    test_apply_start_up_duplicate_guard();
    test_init_publishes_before_it_waits();
    test_handoff_race_is_exactly_once();
    test_init_chooses_and_publishes_a_handoff();
    test_firmware_start_up_is_not_the_guest_s();
    test_start_up_ipi_follows_the_apic_mode();
    test_init_is_forwarded_and_nothing_more();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
