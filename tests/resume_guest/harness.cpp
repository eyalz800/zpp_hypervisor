/*
 * The interrupted-event re-queue, exercised against the real
 * `hypervisor::resume_guest`.
 *
 * An exit taken *during* event delivery is the one part of the injection
 * chain that had no test at all, and it is not a corner: `1975400`
 * measured nine events destroyed in a single boot on the rig - two
 * interprocessor interrupts at vector 0x2f, five clock interrupts at
 * 0xd1, three page faults - and the two at 0x2f are how a halted virtual
 * processor is woken. The processor clears the entry-interruption field
 * as it begins a delivery, so an exit taken in the middle of one leaves
 * nothing to resume from: the only record is the original-event
 * identification field, and the next entry overwrites that too.
 *
 * Putting the event back was written once, measured to stop the guest
 * hypervisor bringing up its application processors - `git bisect` over
 * seven rig boots, good `1975400`, bad `02c747e` - and switched off
 * behind `requeue_interrupted_events` with two defects named in the
 * comment and unfixed. Reading SDM 29.3.1.5 against the code found two
 * more. This harness is a case per defect, so that switching it back on
 * is a decision with evidence under it rather than a second seven boots.
 *
 * Every constant below is spelled out from the SDM tables rather than
 * taken from the hypervisor's own headers, for the reason
 * tests/local_apic gives for doing the same: a probe that reads the
 * expected answer out of the source the answer was written from tests
 * nothing. The tables are:
 *
 * - Table 27-21, "Format of the Original-Event Identification Field",
 *   .references/sdm.txt:200374 - the field an exit during delivery
 *   leaves behind, and the encoding an entry field takes back.
 * - Table 27-18, the injected-event identification field, and the checks
 *   on it in SDM 29.2.1.3, .references/sdm.txt:202225.
 * - SDM 29.3.1.5, "Checks on Guest Non-Register State",
 *   .references/sdm.txt:202589 - which events each activity state
 *   permits, and the blocking-by-NMI rule.
 *
 * KVM's equivalent is `__vmx_complete_interrupts`
 * (.references/kvm/vmx.c:7105) for what a re-queue carries, and
 * `kvm_check_and_inject_events` (.references/kvm/x86.c:10269) for what
 * happens when it collides with an event the exit handler raised.
 *
 * **This harness fails on purpose until the re-queue is fixed.** It is
 * pushed first so the fix lands against tests that already fail, which
 * is the only order in which a fix to this path is checkable: the
 * alternative is a second seven-boot bisect. What fails and why is at the
 * end of this comment.
 *
 * Two of the cases turn on *which* second-level guest an event was
 * captured under, and `guest_current_vmcs[cpu]` is what stands for that
 * identity here. It is necessary and not quite sufficient, and the gap is
 * worth knowing before anything relies on it: a guest hypervisor may
 * VMCLEAR a region and VMPTRLD a different one at the same physical
 * address, at which point the address matches and the guest is not the
 * same. What closes the gap is the operation rather than the address -
 * VMCLEAR is the architecture's "this VMCS is no longer a guest" (SDM
 * 27.11.1 sets its launch state to clear), so an event dropped whenever
 * the region it was captured under is cleared, or whenever a different
 * one is loaded, is sound with no extra state at all. The address alone
 * is enough for every case below because none of them reuses one.
 *
 * The cases that fail against `requeue_interrupted_events = false`, which
 * is every case about the re-queue itself, and the four defects behind
 * them:
 *
 * 1. the write is unconditional, so it overwrites what the handler
 *    staged - `a_staged_event_is_not_overwritten`;
 * 2. `pending_event[cpu]` is cleared only when re-injected, so a
 *    deferred event outlives the guest it was captured under -
 *    `a_stale_event_is_not_delivered_into_a_different_guest`;
 * 3. the activity state is never asked, and an event put back into a
 *    processor parked by an INIT fails the VM entry outright -
 *    `wait_for_sipi_permits_no_event` and the two beside it. This is the
 *    one that stops application processors coming up, which is what
 *    `scripts/rig-check-vmxon.sh` measures;
 * 4. blocking by NMI is left set under a re-queued NMI, which fails the
 *    entry wherever the guest hypervisor asked for virtual NMIs -
 *    `an_nmi_put_back_clears_blocking_by_nmi`.
 */
#include "zpp/hypervisor/hypervisor.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace zpp::diag
{
namespace
{
std::uint64_t g_pump_runs{};
} // namespace

void pump::run()
{
    g_pump_runs += 1;
}

bool esp_block_sink::ready()
{
    return true;
}

} // namespace zpp::diag

namespace zpp::hypervisor
{
void hypervisor::record_exit(arch::x86_64::vmx::exit_reason,
                             const arch::x86_64::context &)
{
    this->record_exit_count += 1;
}

void hypervisor::arm_controller_poll(bool)
{
    this->controller_poll_count += 1;
}

} // namespace zpp::hypervisor

namespace
{
std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
}

void check_equal(std::uint64_t expected,
                 std::uint64_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n  expected 0x%llx\n  actual   0x%llx\n",
                what.c_str(),
                static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(actual));
}

/*
 * The original-event identification field, SDM Table 27-21. Values 1 and
 * 7 of the type are "not used" and so have no name here.
 */
namespace original_event
{
constexpr std::uint64_t external_interrupt = 0ull << 8;
constexpr std::uint64_t non_maskable_interrupt = 2ull << 8;
constexpr std::uint64_t hardware_exception = 3ull << 8;
constexpr std::uint64_t software_interrupt = 4ull << 8;
constexpr std::uint64_t privileged_software_exception = 5ull << 8;
constexpr std::uint64_t software_exception = 6ull << 8;
constexpr std::uint64_t error_code_valid = 1ull << 11;

/**
 * Bit 13, "Exception nested on FRED event delivery". Architectural on the
 * way out and permitted on the way in only where IA32_VMX_BASIC[58]
 * reports FRED transitions, which is what makes copying the word whole a
 * defect rather than a shortcut.
 */
constexpr std::uint64_t nested_exception = 1ull << 13;
constexpr std::uint64_t valid = 1ull << 31;
} // namespace original_event

/**
 * The guest activity states, SDM 27.4.2.
 */
namespace activity
{
constexpr std::uint64_t active = 0;
constexpr std::uint64_t hlt = 1;
constexpr std::uint64_t shutdown = 2;
constexpr std::uint64_t wait_for_start_up_ipi = 3;
} // namespace activity

/**
 * The guest interruptibility-state field, SDM Table 27-3.
 */
namespace interruptibility
{
constexpr std::uint64_t blocking_by_sti = 1ull << 0;
constexpr std::uint64_t blocking_by_nmi = 1ull << 3;
} // namespace interruptibility

/*
 * The vectors the cases below name. Each is here because the
 * architecture treats it differently from its neighbours, not because it
 * is a plausible number.
 */
constexpr std::uint64_t debug_exception_vector = 1;
constexpr std::uint64_t invalid_opcode_vector = 6;
constexpr std::uint64_t page_fault_vector = 14;
constexpr std::uint64_t machine_check_vector = 18;

/**
 * The clock interrupt Hyper-V injects into its guest on this rig, and one
 * of the vectors `1975400` measured being destroyed.
 */
constexpr std::uint64_t clock_interrupt_vector = 0xd1;

/**
 * The interprocessor interrupt that wakes a halted virtual processor,
 * which is why a destroyed one leaves the machine with every processor
 * halted and nothing pending.
 */
constexpr std::uint64_t wake_interrupt_vector = 0x2f;

/**
 * A guest whose exit is being resumed from, at a known instruction.
 *
 * The instruction length is the one the exit reported, which the resume
 * path adds to RIP when it is told to; the values are arbitrary but
 * distinct, so a wrong one is visible as itself rather than as zero.
 */
constexpr std::uint64_t guest_rip = 0x1234'5000;
constexpr std::uint64_t guest_cs = 0x28;
constexpr std::uint64_t exit_instruction_length = 3;

/**
 * The error code and instruction length a captured event carried, which
 * the re-queue has to put where the architecture reads them from.
 */
constexpr std::uint64_t captured_error_code = 0xbeef;
constexpr std::uint64_t captured_instruction_length = 2;

/**
 * A processor slot. One rather than zero, because the VPID *is* the slot
 * plus one - `setup_vmcs` writes `vpid(cpu + 1)` - and a harness that
 * used zero would be testing the "no slot" path by accident.
 */
constexpr std::uint64_t cpu = 0;

/*
 * The activity-state rule on its own, checked at compile time against
 * SDM 29.3.1.5 before anything asks whether the resume path applies it.
 *
 * `vm_entry_interruption::allowed_in` is a pure function of the two
 * fields, so it can be settled here rather than through a resume - and
 * settling it here is what keeps the cases further down about the resume
 * path rather than about the rule. The expectations are transcribed from
 * the enumeration in that section, quoted case by case where each is
 * used below.
 */
namespace
{
constexpr bool allowed_in(std::uint64_t state, std::uint64_t event)
{
    return zpp::arch::x86_64::vmx::vm_entry_interruption::allowed_in(
        state, event);
}

// "Active. Any event is allowed."
static_assert(allowed_in(activity::active,
                         original_event::valid |
                             original_event::software_exception |
                             invalid_opcode_vector));

// "HLT. The only events allowed are ... external interrupt or
// non-maskable interrupt (NMI) ... hardware exception and vector 1
// (debug exception) or vector 18 (machine-check exception)."
static_assert(allowed_in(activity::hlt,
                         original_event::valid |
                             original_event::external_interrupt |
                             wake_interrupt_vector));
static_assert(allowed_in(activity::hlt,
                         original_event::valid |
                             original_event::non_maskable_interrupt | 2));
static_assert(allowed_in(activity::hlt,
                         original_event::valid |
                             original_event::hardware_exception |
                             debug_exception_vector));
static_assert(allowed_in(activity::hlt,
                         original_event::valid |
                             original_event::hardware_exception |
                             machine_check_vector));
static_assert(!allowed_in(activity::hlt,
                          original_event::valid |
                              original_event::hardware_exception |
                              page_fault_vector));
static_assert(!allowed_in(activity::hlt,
                          original_event::valid |
                              original_event::software_exception |
                              invalid_opcode_vector));

// "Shutdown. Only NMIs and machine-check exceptions are allowed."
static_assert(allowed_in(activity::shutdown,
                         original_event::valid |
                             original_event::non_maskable_interrupt | 2));
static_assert(allowed_in(activity::shutdown,
                         original_event::valid |
                             original_event::hardware_exception |
                             machine_check_vector));
static_assert(!allowed_in(activity::shutdown,
                          original_event::valid |
                              original_event::external_interrupt |
                              clock_interrupt_vector));

// "Wait-for-SIPI. No events are allowed."
static_assert(!allowed_in(activity::wait_for_start_up_ipi,
                          original_event::valid |
                              original_event::external_interrupt |
                              clock_interrupt_vector));
static_assert(!allowed_in(activity::wait_for_start_up_ipi,
                          original_event::valid |
                              original_event::non_maskable_interrupt | 2));

// The rule is about an event, so no event passes every state.
static_assert(allowed_in(activity::wait_for_start_up_ipi, 0));
} // namespace

struct machine
{
    std::unique_ptr<zpp::hypervisor::hypervisor> state;
    zpp::arch::x86_64::context context{};
};

/**
 * A machine with one processor, its guest stopped at a known place.
 */
machine make()
{
    std::memset(&zpp::arch::x86_64::vmx::g_vmcs,
                0,
                sizeof(zpp::arch::x86_64::vmx::g_vmcs));
    zpp::arch::x86_64::vmx::g_vmcs_valid = true;
    zpp::arch::x86_64::g_restore_count = 0;

    machine built{std::make_unique<zpp::hypervisor::hypervisor>()};

    auto & vmcs = built.state->vmcs;
    vmcs.vpid(cpu + 1);
    vmcs.guest_rip(guest_rip);
    vmcs.guest_cs_selector(guest_cs);
    vmcs.write(
        zpp::arch::x86_64::vmx::vmcs::field::vm_exit_instruction_length,
        exit_instruction_length);

    built.context.rip = guest_rip;
    return built;
}

/**
 * Arm the state an exit taken during event delivery leaves behind.
 */
void interrupt_the_delivery_of(machine & built,
                               std::uint64_t event,
                               bool belongs_to_l2 = false)
{
    built.state->pending_event[cpu] = event;
    built.state->pending_event_l2[cpu] = belongs_to_l2;
    built.state->pending_event_error[cpu] = captured_error_code;
    built.state->pending_event_length[cpu] = captured_instruction_length;

    // Which second-level guest it was captured under, exactly as the
    // capture side records it. That code is inside the exit-dispatch
    // lambda and cannot be cut out by name, so this stands in for it -
    // and it has to record the *current* guest, or every case here would
    // look like an event whose guest had been torn down.
    built.state->pending_event_vmcs[cpu] =
        built.state->guest_current_vmcs[cpu];
}

/**
 * Run the real resume path and come back from it.
 *
 * `resume_guest` is [[noreturn]] and ends in `restore_context`, which the
 * shim turns into a `longjmp` back to here - so what it wrote is
 * readable afterwards, and the context it was about to restore is in
 * `g_restored_context`.
 */
void resume(machine & built, bool advance_rip = true)
{
    if (0 == setjmp(zpp::arch::x86_64::g_resume_escape)) {
        built.state->resume_guest(
            built.context,
            zpp::arch::x86_64::vmx::exit_reason(static_cast<std::uint64_t>(
                zpp::arch::x86_64::vmx::exit_reason::basic_reason::
                    ept_violation)),
            advance_rip);
    }
}

std::uint64_t entry_field(const machine & built)
{
    return built.state->vmcs.vm_entry_interruption_information_field();
}

// === What a re-queue carries ===========================================

/**
 * The plain case, and the one the rig destroyed nine of.
 *
 * KVM's `__vmx_complete_interrupts` re-queues the vector and the type
 * (.references/kvm/vmx.c:7125-7156); everything else about the event is
 * carried by those two.
 */
void an_external_interrupt_is_put_back()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::external_interrupt |
                 clock_interrupt_vector;

    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(event,
                entry_field(built),
                "the interrupted interrupt is staged for the next entry");
    check_equal(
        1, built.state->events_requeued[cpu], "and is counted as one");
    check_equal(0,
                built.state->pending_event[cpu],
                "and is no longer pending, so the entry after this one "
                "does not deliver it a second time");
}

/**
 * An error code is carried only when the event says it has one.
 *
 * SDM 29.2.1.3 makes the deliver-error-code bit part of what a valid
 * entry field must get right, and KVM reads the error code out of the
 * exit only for `VECTORING_INFO_DELIVER_CODE_MASK`
 * (.references/kvm/vmx.c:7142).
 */
void a_hardware_exception_carries_its_error_code()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::hardware_exception |
                 original_event::error_code_valid | page_fault_vector;

    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(event, entry_field(built), "the page fault is staged");
    check_equal(captured_error_code,
                built.state->vmcs.vm_entry_exception_error_code(),
                "with the error code the exit captured");
}

void an_event_without_an_error_code_leaves_the_field_alone()
{
    auto built = make();
    constexpr std::uint64_t left_behind = 0x1111;

    built.state->vmcs.vm_entry_exception_error_code(left_behind);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector);
    resume(built);

    check_equal(left_behind,
                built.state->vmcs.vm_entry_exception_error_code(),
                "an interrupt does not touch the error code field, "
                "which the processor ignores for it anyway");
}

/**
 * All three software types need the instruction length, and this is
 * where KVM and the SDM disagree.
 *
 * SDM 29.2.1.3: "The VM-entry instruction-length field is in the range
 * 0-15 if the event type is software interrupt, software exception, or
 * privileged software exception". KVM reads it for the first two and
 * drops the third at its `default:` (.references/kvm/vmx.c:7138-7156).
 * The SDM is the one to follow: the field decides the RIP pushed on the
 * stack, so getting it wrong for type 5 returns the guest to the middle
 * of its own instruction.
 */
void the_three_software_types_carry_their_length()
{
    struct
    {
        std::uint64_t type;
        const char * what;
    } constexpr cases[]{
        {original_event::software_interrupt, "a software interrupt"},
        {original_event::privileged_software_exception,
         "a privileged software exception"},
        {original_event::software_exception, "a software exception"},
    };

    for (auto & one : cases) {
        auto built = make();
        interrupt_the_delivery_of(built,
                                  original_event::valid | one.type |
                                      invalid_opcode_vector);
        resume(built);

        check_equal(captured_instruction_length,
                    built.state->vmcs.vm_entry_instruction_length(),
                    std::string(one.what) +
                        " carries the length of the instruction that "
                        "raised it");
    }
}

void a_hardware_event_leaves_the_length_alone()
{
    struct
    {
        std::uint64_t type;
        std::uint64_t vector;
        const char * what;
    } constexpr cases[]{
        {original_event::external_interrupt,
         clock_interrupt_vector,
         "an external interrupt"},
        {original_event::non_maskable_interrupt, 2, "an NMI"},
        {original_event::hardware_exception,
         page_fault_vector,
         "a hardware exception"},
    };

    for (auto & one : cases) {
        auto built = make();
        constexpr std::uint64_t left_behind = 0xf;

        built.state->vmcs.vm_entry_instruction_length(left_behind);
        interrupt_the_delivery_of(
            built, original_event::valid | one.type | one.vector);
        resume(built);

        check_equal(left_behind,
                    built.state->vmcs.vm_entry_instruction_length(),
                    std::string(one.what) +
                        " does not write the instruction length, which "
                        "the processor ignores for it");
    }
}

/**
 * The word is not copied whole.
 *
 * SDM 29.2.1.3 requires bits 30:14 and 12 of the entry field to be 0, and
 * permits bit 13 only where IA32_VMX_BASIC[58] reports FRED transitions -
 * while SDM Table 27-21 defines bit 13 on the way *out*. A processor that
 * sets it and a VMM that copies the word therefore builds an entry field
 * that fails the entry. KVM never meets the question because it rebuilds
 * the event from its vector and its type.
 */
void undefined_bits_are_not_copied_into_the_entry_field()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::hardware_exception |
                 original_event::nested_exception | page_fault_vector;

    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(original_event::valid |
                    original_event::hardware_exception | page_fault_vector,
                entry_field(built),
                "the nested-exception bit is not carried into the entry "
                "field, where it is only legal on a FRED processor");
}

// === The collision with an event the handler raised ====================

/**
 * The first defect the switch was turned off for.
 *
 * The write used to be unconditional, so it overwrote whatever the exit
 * handler had staged - and the handler stages an event exactly when it
 * has decided the guest must see a fault. KVM: "Don't re-inject an NMI or
 * interrupt if there is a pending exception. This collision arises if an
 * exception occurred while vectoring the injected event, KVM intercepted
 * said exception, and KVM ultimately determined the fault belongs to the
 * guest and queues the exception for injection back into the guest"
 * (.references/kvm/x86.c:10287).
 */
void a_staged_event_is_not_overwritten()
{
    auto built = make();
    auto staged = original_event::valid |
                  original_event::hardware_exception |
                  invalid_opcode_vector;

    built.state->vmcs.vm_entry_interruption_information_field(staged);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector);
    resume(built);

    check_equal(staged,
                entry_field(built),
                "the fault the handler staged is what the guest sees, "
                "not the interrupt whose delivery the exit interrupted");
    check_equal(0,
                built.state->events_requeued[cpu],
                "and nothing is counted as re-queued");
    check(0 != built.state->pending_event[cpu],
          "and the interrupt is not lost either - KVM leaves it queued "
          "and delivers it once the exception has gone out "
          "(.references/kvm/x86.c:10345, `can_inject`), which is the "
          "only choice here: this VMM has no virtual APIC to re-derive "
          "an external interrupt from");
}

/**
 * The displaced event goes out at the entry after it.
 *
 * Holding it is only correct if something eventually delivers it. The
 * pair with the case above is the whole of the requirement: the staged
 * fault first, then the interrupt whose delivery it interrupted.
 */
void a_displaced_event_goes_out_at_the_next_entry()
{
    auto built = make();

    built.state->vmcs.vm_entry_interruption_information_field(
        original_event::valid | original_event::hardware_exception |
        invalid_opcode_vector);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  wake_interrupt_vector);
    resume(built);

    // The entry after it: the staged fault has been delivered, so the
    // processor has cleared the field, and nothing new was captured.
    built.state->vmcs.vm_entry_interruption_information_field(0);
    resume(built);

    check_equal(original_event::valid |
                    original_event::external_interrupt |
                    wake_interrupt_vector,
                entry_field(built),
                "the interrupt that was displaced is delivered at the "
                "next entry to the same guest");
    check_equal(1, built.state->events_requeued[cpu], "and counted once");
    check_equal(0,
                built.state->pending_event[cpu],
                "and is no longer pending, so a third entry does not "
                "deliver it a second time");
}

/**
 * An event belonging to the other level waits for the level it belongs
 * to.
 *
 * A guest hypervisor's VMLAUNCH is an exit like any other, so an exit
 * that interrupted a delivery to one level can be followed by an entry
 * into the other - and putting the event back there hands one level's
 * interrupt to the other.
 */
void an_event_for_the_other_level_waits_for_it()
{
    auto built = make();
    constexpr std::uint64_t second_level_guest = 0x4000'0000;

    built.state->guest_current_vmcs[cpu] = second_level_guest;
    built.state->running_l2[cpu] = false;
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector,
                              true);
    resume(built);

    check_equal(0,
                entry_field(built),
                "an event captured for the second-level guest is not "
                "delivered into the first");
    check_equal(1,
                built.state->events_deferred[cpu],
                "and the deferral is counted");

    // That guest runs again, and it is the same one.
    built.state->running_l2[cpu] = true;
    resume(built);

    check_equal(original_event::valid |
                    original_event::external_interrupt |
                    clock_interrupt_vector,
                entry_field(built),
                "and is delivered when that guest runs again");
}

/**
 * The second defect the switch was turned off for.
 *
 * `pending_event[cpu]` is cleared only on the path that re-injects it, so
 * an event deferred because it belonged to the other level is held
 * indefinitely and then delivered into some later unrelated entry. The
 * bound has to be the guest it was captured under: an interrupt destined
 * for a virtual processor that no longer exists is not an interrupt for
 * whichever one now occupies the slot.
 *
 * `guest_current_vmcs[cpu]` is what identifies that guest here, and it is
 * necessary rather than sufficient - see the note in the harness header.
 */
void a_stale_event_is_not_delivered_into_a_different_guest()
{
    auto built = make();
    constexpr std::uint64_t first_guest = 0x4000'0000;
    constexpr std::uint64_t second_guest = 0x5000'0000;

    built.state->guest_current_vmcs[cpu] = first_guest;
    built.state->running_l2[cpu] = false;
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector,
                              true);
    resume(built);

    // The guest hypervisor tears that guest down and starts another one.
    built.state->guest_current_vmcs[cpu] = second_guest;
    built.state->running_l2[cpu] = true;
    resume(built);

    check_equal(0,
                entry_field(built),
                "an event captured under one second-level guest is not "
                "delivered into the next one");
    check_equal(0,
                built.state->pending_event[cpu],
                "and does not stay pending for the one after that");
}

// === What the activity state permits ===================================

/**
 * The defect that fits what the bisect measured.
 *
 * `emulate_init` parks a processor in wait-for-SIPI and clears the
 * entry-interruption field on purpose, citing the entry check that
 * forbids the two together. The re-queue then wrote it straight back.
 * SDM 29.3.1.5: "Wait-for-SIPI. No events are allowed." The entry fails,
 * `on_vm_entry_failure` stops the processor, and the processor it stops
 * is one the guest was in the middle of starting.
 */
void wait_for_sipi_permits_no_event()
{
    auto built = make();

    built.state->vmcs.guest_activity_state(
        activity::wait_for_start_up_ipi);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector);
    resume(built);

    check_equal(0,
                entry_field(built),
                "a processor parked in wait-for-SIPI is left with no "
                "event, which is the only state VM entry accepts");
    check_equal(0,
                built.state->pending_event[cpu],
                "and the event is dropped rather than held: the only "
                "thing that parks a processor there is an INIT, and an "
                "INIT destroys what was pending - `kvm_vcpu_reset` "
                "clears the exception and interrupt queues");
}

/**
 * "Shutdown. Only NMIs and machine-check exceptions are allowed."
 */
void shutdown_permits_only_nmi_and_machine_check()
{
    struct
    {
        std::uint64_t event;
        bool allowed;
        const char * what;
    } constexpr cases[]{
        {original_event::non_maskable_interrupt | 2, true, "an NMI"},
        {original_event::hardware_exception | machine_check_vector,
         true,
         "a machine-check exception"},
        {original_event::external_interrupt | clock_interrupt_vector,
         false,
         "an external interrupt"},
        {original_event::hardware_exception | page_fault_vector,
         false,
         "a page fault"},
    };

    for (auto & one : cases) {
        auto built = make();

        built.state->vmcs.guest_activity_state(activity::shutdown);
        interrupt_the_delivery_of(built,
                                  original_event::valid | one.event);
        resume(built);

        check_equal(one.allowed ? (original_event::valid | one.event) : 0,
                    entry_field(built),
                    std::string(one.what) +
                        (one.allowed ? " is allowed" : " is refused") +
                        " into a processor in shutdown");
    }
}

/**
 * "HLT. The only events allowed are the following: those with event type
 * external interrupt or non-maskable interrupt (NMI); those with event
 * type hardware exception and vector 1 (debug exception) or vector 18
 * (machine-check exception); those with event type other event and vector
 * 0 (pending MTF VM exit)."
 *
 * The last of those has no case: type 7 is "not used" in the
 * original-event field, so it cannot arrive here.
 */
void hlt_permits_what_a_halted_processor_can_take()
{
    struct
    {
        std::uint64_t event;
        bool allowed;
        const char * what;
    } constexpr cases[]{
        {original_event::external_interrupt | wake_interrupt_vector,
         true,
         "the interrupt that wakes it"},
        {original_event::non_maskable_interrupt | 2, true, "an NMI"},
        {original_event::hardware_exception | debug_exception_vector,
         true,
         "a debug exception"},
        {original_event::hardware_exception | machine_check_vector,
         true,
         "a machine-check exception"},
        {original_event::hardware_exception | page_fault_vector,
         false,
         "a page fault"},
        {original_event::software_exception | invalid_opcode_vector,
         false,
         "a software exception"},
    };

    for (auto & one : cases) {
        auto built = make();

        built.state->vmcs.guest_activity_state(activity::hlt);
        interrupt_the_delivery_of(built,
                                  original_event::valid | one.event);
        resume(built);

        check_equal(one.allowed ? (original_event::valid | one.event) : 0,
                    entry_field(built),
                    std::string(one.what) +
                        (one.allowed ? " is allowed" : " is refused") +
                        " into a halted processor");
    }
}

/**
 * An active processor takes anything, which is what makes the three
 * cases above a restriction rather than a filter.
 */
void an_active_processor_takes_any_event()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::software_exception |
                 invalid_opcode_vector;

    built.state->vmcs.guest_activity_state(activity::active);
    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(event,
                entry_field(built),
                "SDM 29.3.1.5: \"Active. Any event is allowed.\"");
}

// === Blocking by NMI ===================================================

/**
 * An NMI put back has to have its own blocking undone first.
 *
 * A delivery that started left blocking by NMI in effect, and SDM
 * 29.3.1.5 refuses the entry that puts the NMI back while it is still
 * set: "Bit 3 (blocking by NMI) must be 0 if the 'virtual NMIs'
 * VM-execution control is 1, the valid bit (bit 31) in the injected-event
 * identification field is 1, and the event type (bits 10:8) in that field
 * has value 2 (indicating NMI)." A guest hypervisor asks for virtual NMIs
 * and `build_vmcs02` merges the request, so the control this VMM does not
 * set for itself is set underneath a second-level guest.
 *
 * KVM clears it on the same path and without asking whether the control
 * is on: `vmx_set_nmi_mask(vcpu, false)`, "Clear bit 'block by NMI'
 * before VM entry if a NMI delivery faulted" (.references/kvm/vmx.c:7130).
 */
void an_nmi_put_back_clears_blocking_by_nmi()
{
    auto built = make();

    built.state->vmcs.guest_interruptibility_state(
        interruptibility::blocking_by_nmi);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::non_maskable_interrupt |
                                  2);
    resume(built);

    check_equal(0,
                built.state->vmcs.guest_interruptibility_state() &
                    interruptibility::blocking_by_nmi,
                "blocking by NMI is cleared, so the entry that puts the "
                "NMI back is not refused");
}

void only_an_nmi_clears_blocking_by_nmi()
{
    auto built = make();

    built.state->vmcs.guest_interruptibility_state(
        interruptibility::blocking_by_nmi |
        interruptibility::blocking_by_sti);
    interrupt_the_delivery_of(built,
                              original_event::valid |
                                  original_event::external_interrupt |
                                  clock_interrupt_vector);
    resume(built);

    check_equal(interruptibility::blocking_by_nmi |
                    interruptibility::blocking_by_sti,
                built.state->vmcs.guest_interruptibility_state(),
                "an interrupt leaves the interruptibility state alone - "
                "the rule is about the NMI being put back, not about "
                "the field");
}

// === The rest of the resume path =======================================

void nothing_is_put_back_without_a_pending_event()
{
    auto built = make();
    resume(built);

    check_equal(0,
                entry_field(built),
                "an exit that interrupted nothing stages nothing");
    check_equal(0, built.state->events_requeued[cpu], "and counts none");
    check_equal(0, built.state->events_deferred[cpu], "and defers none");
}

/**
 * The failure as it was measured, end to end.
 *
 * Measured on the rig with the monitor trap flag armed on every entry
 * that injects vector 0xd1: on all eight processors the first landing
 * after the injecting entry is an EPT violation with an RIP delta of
 * zero. The instruction never retired. The processor reads the interrupt
 * descriptor table and pushes five words on the guest's stack before it
 * reaches the handler, and both of those go through the extended page
 * tables - so the delivery is what faulted, the injection is cancelled,
 * and `events_requeued` reads zero on every processor.
 *
 * The vector is the same one `1975400` measured being destroyed, found
 * from the opposite end by a different measurement. `b2ce7d0` records the
 * chain.
 *
 * This is the case the whole harness exists for, so it asserts the exact
 * word rather than a property of it.
 */
void the_clock_interrupt_the_rig_destroys_is_put_back()
{
    auto built = make();
    constexpr std::uint64_t measured_event = 0x8000'00d1;

    static_assert(measured_event == (original_event::valid |
                                     original_event::external_interrupt |
                                     clock_interrupt_vector),
                  "valid, type 0 external interrupt, vector 0xd1 - and "
                  "no error code, which SDM Table 27-21 permits only "
                  "for hardware exceptions");

    built.state->running_l2[cpu] = true;
    built.state->guest_current_vmcs[cpu] = 0x4000'0000;
    interrupt_the_delivery_of(built, measured_event, true);
    resume(built);

    check_equal(measured_event,
                entry_field(built),
                "the clock interrupt whose delivery the EPT violation "
                "interrupted is put back exactly as it was recorded");
    check_equal(0,
                built.state->vmcs.vm_entry_exception_error_code(),
                "with no error code, because an external interrupt has "
                "none to carry");
    check_equal(1,
                built.state->events_requeued[cpu],
                "and events_requeued stops reading zero, which is what "
                "the rig measured it reading on all eight processors");
}

/**
 * A VPID outside the table is not an index.
 *
 * Every per-processor array here is `max_cpus` long and the VPID is the
 * slot plus one, so zero means "no slot" and anything past the table is a
 * write off the end of it.
 */
void a_slot_outside_the_table_is_left_alone()
{
    for (auto vpid : {0ull,
                      static_cast<unsigned long long>(
                          zpp::hypervisor::hypervisor::max_cpus + 1)}) {
        auto built = make();

        built.state->vmcs.vpid(vpid);
        interrupt_the_delivery_of(built,
                                  original_event::valid |
                                      original_event::external_interrupt |
                                      clock_interrupt_vector);
        resume(built);

        check_equal(0,
                    entry_field(built),
                    "vpid " + std::to_string(vpid) +
                        " names no slot, so nothing is put back");
        check_equal(0,
                    built.state->resumes_reached[cpu],
                    "and nothing is counted against slot zero");
    }
}

/**
 * RIP moves only when the exit says an instruction retired.
 *
 * An INIT signal or a start-up IPI leaves the instruction-length field
 * holding nothing meaningful, and both handlers have already put RIP
 * where the processor is meant to resume - adding to it would land the
 * guest a few bytes into its own entry point.
 */
void rip_advances_only_when_asked()
{
    auto advanced = make();
    resume(advanced, true);

    check_equal(guest_rip + exit_instruction_length,
                advanced.state->vmcs.guest_rip(),
                "an instruction the guest executed is resumed past");
    check_equal(guest_rip + exit_instruction_length,
                advanced.state->resume_guest_rip[cpu],
                "and the record a debugger reads says the same");

    auto held = make();
    resume(held, false);

    check_equal(guest_rip,
                held.state->vmcs.guest_rip(),
                "an INIT or a start-up IPI resumes where its handler "
                "left the guest");
}

/**
 * Which entry the processor leaves through, which is three questions
 * rather than one.
 */
void the_entry_is_chosen_by_launch_state()
{
    auto ordinary = make();
    resume(ordinary);
    check_equal(
        reinterpret_cast<std::uint64_t>(&zpp::arch::x86_64::vmx::vmresume),
        zpp::arch::x86_64::g_restored_context.rip,
        "an ordinary resume goes back through VMRESUME");

    auto after_sleep = make();
    after_sleep.state->relaunch_after_sleep[cpu] = true;
    resume(after_sleep);
    check_equal(
        reinterpret_cast<std::uint64_t>(&zpp::arch::x86_64::vmx::vmlaunch),
        zpp::arch::x86_64::g_restored_context.rip,
        "a processor that has been out of VMX operation and back "
        "leaves through VMLAUNCH, because VMRESUME requires "
        "launched (SDM 27.1)");
    check(!after_sleep.state->relaunch_after_sleep[cpu],
          "and the flag is consumed, so the entry after it resumes");

    if constexpr (zpp::hypervisor::nested_vmx::enabled) {
        auto into_l2 = make();
        into_l2.state->running_l2[cpu] = true;
        into_l2.state->vmcs02_launched[cpu] = false;
        resume(into_l2);
        check_equal(reinterpret_cast<std::uint64_t>(
                        &zpp::arch::x86_64::vmx::nested_vmlaunch),
                    zpp::arch::x86_64::g_restored_context.rip,
                    "a second-level guest that has never run is entered "
                    "with VMLAUNCH");

        auto back_to_l2 = make();
        back_to_l2.state->running_l2[cpu] = true;
        back_to_l2.state->vmcs02_launched[cpu] = true;
        resume(back_to_l2);
        check_equal(reinterpret_cast<std::uint64_t>(
                        &zpp::arch::x86_64::vmx::nested_vmresume),
                    zpp::arch::x86_64::g_restored_context.rip,
                    "and one that has, with VMRESUME");
    }
}

/**
 * What the resume records for a debugger, which is the only channel there
 * is once a guest is running.
 */
void the_resume_records_where_it_left_the_guest()
{
    auto built = make();

    built.state->vmcs.guest_activity_state(activity::hlt);
    resume(built);

    check_equal(1,
                built.state->resumes_reached[cpu],
                "the resume is counted at the last point before control "
                "leaves the handler");
    check_equal(activity::hlt,
                built.state->resume_activity_state[cpu],
                "with the activity state this VMM's own VMCS holds");
    check_equal(guest_rip + exit_instruction_length,
                built.state->resume_guest_rip[cpu],
                "and where the guest is about to resume");
    check_equal(guest_cs,
                built.state->resume_guest_cs[cpu],
                "and in which segment");
    check_equal(1,
                built.state->record_exit_count,
                "and the exit is recorded once, after the handlers have "
                "had their say");
    check_equal(1,
                zpp::arch::x86_64::g_restore_count,
                "and the guest is entered exactly once");
}

} // namespace

int main()
{
    an_external_interrupt_is_put_back();
    a_hardware_exception_carries_its_error_code();
    an_event_without_an_error_code_leaves_the_field_alone();
    the_three_software_types_carry_their_length();
    a_hardware_event_leaves_the_length_alone();
    undefined_bits_are_not_copied_into_the_entry_field();
    a_staged_event_is_not_overwritten();
    a_displaced_event_goes_out_at_the_next_entry();
    an_event_for_the_other_level_waits_for_it();
    a_stale_event_is_not_delivered_into_a_different_guest();
    the_clock_interrupt_the_rig_destroys_is_put_back();
    wait_for_sipi_permits_no_event();
    shutdown_permits_only_nmi_and_machine_check();
    hlt_permits_what_a_halted_processor_can_take();
    an_active_processor_takes_any_event();
    an_nmi_put_back_clears_blocking_by_nmi();
    only_an_nmi_clears_blocking_by_nmi();
    nothing_is_put_back_without_a_pending_event();
    a_slot_outside_the_table_is_left_alone();
    rip_advances_only_when_asked();
    the_entry_is_chosen_by_launch_state();
    the_resume_records_where_it_left_the_guest();

    std::printf(
        "resume_guest: %zu checks, %zu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
