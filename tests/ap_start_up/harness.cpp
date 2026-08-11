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
    test_apply_start_up_state();
    test_apply_start_up_vectors();
    test_apply_start_up_duplicate_guard();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
