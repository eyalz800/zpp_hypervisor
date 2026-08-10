// Concurrency harness for application-processor start-up and adoption.
//
// Compiles the real processor_slot, start_up_processor,
// start_application_processor and enter_root_mode - cut out of
// hypervisor.cpp by build.sh - natively against a shim hypervisor, and
// drives them from several host threads standing in for several logical
// processors.
//
// What it is and is not. Every other harness in tests/ is differential:
// it compares this VMM's answer against a reference. There is no
// reference for "did two processors collide", so this one asserts
// **invariants** instead, each of which is a sentence out of the SDM or
// out of this tree's own comments, and each of which a defect that has
// actually been in this tree violates. The rig costs ten minutes a boot
// and this costs a second, which is the whole argument for it.
//
// The four invariants:
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
//
// Recorded rather than performed: a start-up IPI has nowhere to go here,
// and what matters is which processor was aimed at and how often.
static std::atomic<int> g_start_up_ipis{};
static std::atomic<std::uint64_t> g_last_start_up_target{};

namespace zpp::hypervisor
{
void hypervisor::send_start_up_ipi(std::uint64_t apic, std::uint64_t)
{
    g_last_start_up_target.store(apic);
    g_start_up_ipis.fetch_add(1);
}

std::uint32_t hypervisor::start_up_trampoline_stage() const
{
    return 0;
}

std::expected<void, zpp::error> hypervisor::enable_vmx_in_feature_control()
{
    return {};
}
} // namespace zpp::hypervisor

namespace zpp::arch::x86_64
{
std::uint64_t rdmsr(std::uint32_t)
{
    return 0;
}

void wrmsr(std::uint32_t, std::uint64_t)
{
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
    g_start_up_ipis.store(0);
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
