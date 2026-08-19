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
#include "zpp/diag/pump.h"
#include "zpp/diag/sinks/esp_blocks.h"
#include "zpp/hypervisor/hypervisor.h"

#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

/**
 * What this harness records about the two calls the resume path makes
 * into the rest of the VMM.
 *
 * Namespace scope, because they are the harness's counters and not the
 * hypervisor's. They were members of a stand-in class that is gone; the
 * real class has no place for them, which is right.
 *
 * `zpp::diag::pump::run` and `zpp::diag::esp_block_sink::ready` used to
 * be counted here too, through four stand-in diag headers. Those are
 * gone: both are defined inline in the real headers, which are on the
 * path now, and `ready()` answers false because nothing configured the
 * channel - the same false a boot with no diagnostic medium gets. The
 * count of pump runs was never asserted, so nothing is lost; the call
 * itself is still observed, through `controller_polls`, which the
 * resume path reaches on the same line.
 */
struct observations
{
    std::uint64_t record_exits{};
    std::uint64_t controller_polls{};
    std::uint64_t shadow_ept_checks{};
};

static observations g_observed;

namespace zpp::hypervisor
{
void hypervisor::record_exit(std::size_t,
                             arch::x86_64::vmx::exit_reason,
                             const arch::x86_64::context &)
{
    g_observed.record_exits += 1;
}

void hypervisor::arm_controller_poll(std::size_t, bool)
{
    g_observed.controller_polls += 1;
}

/**
 * The shadow extended page tables, which are not this harness's subject.
 *
 * `resume_guest` calls this on the way into a guest so that a permission
 * change made by the handler it is returning from cannot be entered
 * against a shadow composed from the old permissions -
 * `discard_stale_shadow_ept` says why that has to be the entry path and
 * not the exit path. What it does is nested_ept.cpp's, and
 * tests/shadow_ept drives it against a real pool; here it only has to
 * exist, and the count says the entry path really does reach it.
 */
void hypervisor::discard_stale_shadow_ept(std::size_t)
{
    g_observed.shadow_ept_checks += 1;
}

/**
 * A processor that permits every VM-execution control and requires none.
 *
 * `adjust_msr` computes `(value & allowed_1) | allowed_0` from the two
 * halves of a capability MSR, so allowed-1 all ones and allowed-0 zero
 * makes it the identity - and the cases that arm and disarm
 * interrupt-window exiting then read back exactly what the code under
 * test asked for. A real processor requires several primary controls to
 * be 1 even in the TRUE MSR, but none of those are the bit under test,
 * and a fixture that set them would only make the expected values here
 * harder to read for no question answered.
 */
std::uint64_t & hypervisor::cached_vmx_msr(std::size_t)
{
    static std::uint64_t permissive = 0xffffffff00000000ull;
    return permissive;
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
    std::println("FAIL: {}", what);
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
    std::println("FAIL: {}\n  expected 0x{:x}\n  actual   0x{:x}",
                 what,
                 expected,
                 actual);
}

std::vector<std::string> g_findings;

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

/**
 * The guest RFLAGS bits an entry check reads.
 *
 * Bit 1 is the one RFLAGS bit the architecture requires to be set - SDM
 * 29.3.1.4, "reserved bit 1 must be 1" (.references/sdm.txt:202579) - so
 * a fixture that wants IF clear still has to set it, or it is testing a
 * value no VM entry would accept for a different reason.
 */
namespace rflags
{
constexpr std::uint64_t always_one = 1ull << 1;
constexpr std::uint64_t interrupt_enable = 1ull << 9;
} // namespace rflags

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

    // The observations are namespace scope now rather than members of a
    // stand-in class, so a fresh machine has to clear them explicitly -
    // a new hypervisor object no longer does it by construction.
    g_observed = observations{};

    machine built{std::make_unique<zpp::hypervisor::hypervisor>()};

    auto & vmcs = built.state->vmcs;
    vmcs.vpid(cpu + 1);
    vmcs.guest_rip(guest_rip);
    vmcs.guest_cs_selector(guest_cs);

    // Interrupts enabled, which is the state an interrupted delivery of
    // an *external* interrupt was in: hardware only delivers one while
    // RFLAGS.IF is 1. The fixture used to leave RFLAGS at the zero the
    // VMCS is wiped to, which is IF clear - so every external-interrupt
    // case here was asking the resume path to re-queue into a state SDM
    // 29.3.1.4 refuses (.references/sdm.txt:202582), and passing. The
    // one case that wants IF clear now says so, below.
    vmcs.guest_rflags(rflags::always_one | rflags::interrupt_enable);
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
void resume(machine & built,
            bool advance_rip = true,
            std::uint64_t cpuid = cpu)
{
    if (0 == setjmp(zpp::arch::x86_64::g_resume_escape)) {
        built.state->resume_guest(
            cpuid,
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

// === The interrupt flag ================================================

/**
 * An external interrupt may only be put back into a guest whose
 * interrupts are enabled, and this VMM does not ask.
 *
 * Written from the SDM before reading `event_allowed_on_entry`, which is
 * why it is a finding rather than a confirmation. The entry checks live
 * in two sections and this VMM transcribed one of them. SDM 29.3.1.5,
 * "Checks on Guest Non-Register State", is the activity-state and
 * interruptibility rule the cases above are about. The requirement here
 * is one section earlier, in 29.3.1.4, "Checks on Guest RFLAGS"
 * (.references/sdm.txt:202582):
 *
 *   "The IF flag (RFLAGS[bit 9]) must be 1 if the valid bit (bit 31) in
 *    the injected-event identification field is 1 and the event type
 *    (bits 10:8) is external interrupt."
 *
 * KVM spells the whole predicate in one place, and it is both halves at
 * once - `__vmx_interrupt_blocked` (.references/kvm/vmx.c:5071):
 *
 *   return !(vmx_get_rflags(vcpu) & X86_EFLAGS_IF) ||
 *          (vmcs_read32(GUEST_INTERRUPTIBILITY_INFO) &
 *           (GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS));
 *
 * `event_allowed_on_entry` has the second disjunct and not the first, so
 * an external interrupt whose delivery an exit interrupted is put back
 * into a guest with IF clear - and the entry then fails, which produces
 * **no exit at all**. That is the silent failure the whole re-queue path
 * exists to prevent, arriving through the path itself.
 *
 * What is established here and what is not, kept separate on purpose.
 *
 * Established: the two `diverge` calls below. `resume_guest` writes the
 * event into the entry-interruption field with RFLAGS.IF clear, and
 * clears `pending_event`, so the event is gone either way. That is this
 * harness' own measurement and it needs no emulator.
 *
 * *Not* established here: that this is what a real boot hits. An earlier
 * revision of this comment claimed a Bochs run reproduced it, on the
 * strength of
 *
 *   VMENTER FAIL: VMCS guest interrupts blocked when injecting external
 *                 interrupt
 *
 * appearing in a guest-tests run at `bfb69c7`. Bochs' condition for that
 * message (`cpu/vmx.cc:2002`) is `(interruptibility & 3) != 0 ||
 * (rflags & IF) == 0`, and the first disjunct is one this VMM refuses -
 * but there is a second injection site in that run and it is the more
 * likely one: `uefi_loader/include/zpp/verify_nested.h:953` builds its
 * vmcs12 with `guest_rflags = 0x2` and then asks for an external
 * interrupt to be injected, which SDM 29.3.1.4 refuses on its own. The
 * two cannot be told apart from the emulator's log, so the claim is
 * withdrawn rather than kept. The defect above stands on the source and
 * on the checks below.
 *
 * The fix is one condition in `event_allowed_on_entry`, beside the
 * blocking-by-STI test it already makes, and it is deliberately not made
 * here: resume.cpp is not this harness' to edit.
 */
void an_external_interrupt_needs_the_interrupt_flag()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::external_interrupt |
                 clock_interrupt_vector;

    built.state->vmcs.guest_rflags(rflags::always_one);
    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(0,
                entry_field(built),
                "an external interrupt is not put back into a guest "
                "with interrupts disabled: SDM 29.3.1.4 "
                "(sdm.txt:202582) requires RFLAGS.IF to be 1 when the "
                "entry field injects one, and an entry that fails its "
                "guest-state checks produces no exit at all");

    check_equal(event,
                built.state->pending_event[cpu],
                "and it is held rather than cleared, so the interrupt "
                "goes back on an entry the guest can take - clearing it "
                "here is what destroyed the I/O completion Windows was "
                "blocked on");
}

/**
 * The same event, with interrupts enabled, still goes back.
 *
 * The pair matters: a fix that refuses every external interrupt would
 * satisfy the case above and undo the whole path, so what may not change
 * is asserted beside what must.
 */
void an_external_interrupt_goes_back_when_interrupts_are_enabled()
{
    auto built = make();
    auto event = original_event::valid |
                 original_event::external_interrupt |
                 clock_interrupt_vector;

    built.state->vmcs.guest_rflags(rflags::always_one |
                                   rflags::interrupt_enable);
    interrupt_the_delivery_of(built, event);
    resume(built);

    check_equal(event,
                entry_field(built),
                "an external interrupt is put back where RFLAGS.IF is 1");
    check_equal(0,
                built.state->pending_event[cpu],
                "and nothing is left held once it has gone out");
}

/**
 * An NMI is not subject to the interrupt flag, and a hardware exception
 * is not either.
 *
 * SDM 29.3.1.4 names one event type, "external interrupt", and 29.3.1.5
 * names two for the STI and MOV-SS blocking - external interrupt and
 * NMI. The asymmetry is the point: an NMI is not maskable by IF, so a
 * fix that reads RFLAGS for every type would hold back the one event a
 * guest with interrupts disabled must still be able to take.
 */
void the_interrupt_flag_binds_only_external_interrupts()
{
    struct
    {
        std::uint64_t event;
        const char * what;
    } const cases[]{
        {original_event::non_maskable_interrupt | 2,
         "an NMI, which RFLAGS.IF does not mask"},
        {original_event::hardware_exception | page_fault_vector,
         "a page fault, which no interruptibility rule mentions"},
        {original_event::software_exception | invalid_opcode_vector,
         "a software exception, likewise"},
    };

    for (const auto & one : cases) {
        auto built = make();
        auto event = original_event::valid | one.event;

        built.state->vmcs.guest_rflags(rflags::always_one);
        interrupt_the_delivery_of(built, event);
        resume(built);

        check_equal(event,
                    entry_field(built),
                    std::string{"with interrupts disabled, "} + one.what +
                        " still goes back: SDM 29.3.1.4 restricts only "
                        "the external-interrupt type");
    }
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
 * A slot outside the table is not an index.
 *
 * Every per-processor array here is `max_cpus` long and the slot is the
 * processor index plus one, so zero means "no slot" and anything past
 * the table is a write off the end of it.
 *
 * The slot used to be read out of the VMCS, so this drove it by writing
 * `vpid`. `resume_guest` now takes the processor index from its caller -
 * reading the field cost an exit to the layer below on every use, and
 * `on_vm_exit` already had the value - so the out-of-range slot arrives
 * as an argument instead, and that is what this passes. The property
 * under test is unchanged and so are the bounds checks it exercises:
 * `~0ull` makes the slot zero and `max_cpus` makes it one past the end.
 */
void a_slot_outside_the_table_is_left_alone()
{
    for (auto cpuid : {~0ull,
                       static_cast<unsigned long long>(
                           zpp::hypervisor::hypervisor::max_cpus)}) {
        auto built = make();

        interrupt_the_delivery_of(built,
                                  original_event::valid |
                                      original_event::external_interrupt |
                                      clock_interrupt_vector);
        resume(built, true, cpuid);

        check_equal(0,
                    entry_field(built),
                    "cpuid " + std::to_string(cpuid) +
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

// === The external-interrupt queue behind ZPP_VIRTUALIZE_APIC ===========
//
// `resume_guest` only calls into this with the switch on, and the switch
// is off here - so these drive `queue_external_interrupt` and
// `deliver_pending_external_interrupt` directly. That is deliberate and
// is why the two are separate functions: the alternative was a second
// build of this harness with `-DZPP_VIRTUALIZE_APIC=1`, which would
// double the test count for one code path.
//
// What is under test is the part that has no hardware in it: which vector
// is chosen, that none is lost, and when interrupt-window exiting is
// armed. What is *not* under test is the part that only a machine can
// answer - that the physical local APIC's in-service bit is cleared by
// the guest's own end-of-interrupt write. Nothing here models an APIC.

constexpr std::uint64_t primary_interrupt_window = 1ull << 2;

std::uint64_t interrupt_window(const machine & built)
{
    return built.state->vmcs
               .primary_processor_based_vm_execution_controls() &
           primary_interrupt_window;
}

/**
 * Highest vector first, and every queued vector eventually delivered.
 *
 * The order is the local APIC's own: SDM 12.8.4 makes a vector's
 * interrupt priority its value divided by sixteen, with the higher vector
 * winning inside a class, so descending vector order is what the hardware
 * would have produced had the interrupts never been taken from it.
 */
void queued_interrupts_go_out_highest_first()
{
    auto built = make();

    for (auto vector : {0x30ull, 0xf0ull, 0x51ull, 0x50ull}) {
        built.state->queue_external_interrupt(cpu, vector);
    }

    check_equal(4,
                built.state->external_interrupts_pending[cpu],
                "four vectors acknowledged, four queued");
    check_equal(4,
                built.state->external_interrupts_pending_high_water[cpu],
                "and the high-water mark records the depth reached");

    for (auto expected : {0xf0ull, 0x51ull, 0x50ull, 0x30ull}) {
        built.state->vmcs.write(
            zpp::arch::x86_64::vmx::vmcs::field::
                vm_entry_interruption_information_field,
            0);

        check(built.state->deliver_pending_external_interrupt(cpu),
              std::format("vector 0x{:x} is next", expected));
        check_equal(original_event::valid |
                        original_event::external_interrupt | expected,
                    entry_field(built),
                    "and it goes out as a valid external interrupt");
    }

    check_equal(0,
                built.state->external_interrupts_pending[cpu],
                "the queue empties");
    check_equal(4,
                built.state->external_interrupts_injected[cpu],
                "and every acknowledged vector reached the guest");
    check_equal(0,
                built.state->external_interrupts_dropped[cpu],
                "none of them dropped");
}

/**
 * A second vector arriving before the first is delivered is kept.
 *
 * This is the defect the single slot had, and it is not a lost interrupt
 * in the ordinary sense: "acknowledge interrupt on exit" has already
 * taken the vector out of the interrupt controller (SDM 30.2), so there
 * is nowhere for it to still be pending. Its in-service bit is set in the
 * physical local APIC and only the guest's handler would ever clear it.
 */
void a_second_vector_does_not_displace_the_first()
{
    auto built = make();
    built.state->queue_external_interrupt(cpu, 0x40);
    built.state->queue_external_interrupt(cpu, 0x41);

    check_equal(0,
                built.state->external_interrupts_dropped[cpu],
                "two different vectors are both representable");

    built.state->deliver_pending_external_interrupt(cpu);
    check_equal(original_event::valid |
                    original_event::external_interrupt | 0x41,
                entry_field(built),
                "the higher goes first");
    check_equal(1,
                built.state->external_interrupts_pending[cpu],
                "and the other is still owed");
}

/**
 * The same vector twice with nothing delivered between is the alarm.
 *
 * It should be unreachable on hardware, for the reason the counter's
 * comment gives, so what is pinned here is that it is *counted* rather
 * than silently coalesced.
 */
void the_same_vector_twice_is_counted_as_a_drop()
{
    auto built = make();
    built.state->queue_external_interrupt(cpu, 0x60);
    built.state->queue_external_interrupt(cpu, 0x60);

    check_equal(2,
                built.state->external_interrupts_taken[cpu],
                "both acknowledgements are counted as taken");
    check_equal(1,
                built.state->external_interrupts_dropped[cpu],
                "and the one a bitmap cannot represent is counted lost");
    check_equal(1,
                built.state->external_interrupts_pending[cpu],
                "one is queued, not two");
}

/**
 * A guest that cannot take an interrupt gets a window instead, and the
 * window is closed again the moment the queue empties.
 */
void a_masked_guest_gets_an_interrupt_window()
{
    auto built = make();
    built.state->vmcs.guest_rflags(rflags::always_one);
    built.state->queue_external_interrupt(cpu, 0x50);

    check(!built.state->deliver_pending_external_interrupt(cpu),
          "an interrupt is not injected into a guest with RFLAGS.IF "
          "clear - injection ignores the flag, so honouring it is this "
          "code's job (SDM 29.3.1.4)");
    check_equal(0, entry_field(built), "nothing is staged");
    check(0 != interrupt_window(built),
          "interrupt-window exiting is armed instead");
    check_equal(1,
                built.state->external_interrupts_deferred[cpu],
                "and the deferral is counted");

    built.state->vmcs.guest_rflags(rflags::always_one |
                                   rflags::interrupt_enable);
    check(built.state->deliver_pending_external_interrupt(cpu),
          "and it goes out once the guest enables interrupts");
    check_equal(0,
                interrupt_window(built),
                "with the window closed again on the way");

    check(!built.state->deliver_pending_external_interrupt(cpu),
          "an empty queue injects nothing");
    check_equal(0,
                interrupt_window(built),
                "and leaves the window closed, or the processor would "
                "exit at every instruction boundary for ever");
}

/**
 * An STI shadow defers too, and so does an event already staged.
 */
void a_shadow_or_a_staged_event_defers_the_interrupt()
{
    auto shadowed = make();
    shadowed.state->vmcs.guest_interruptibility_state(
        interruptibility::blocking_by_sti);
    shadowed.state->queue_external_interrupt(cpu, 0x50);

    check(!shadowed.state->deliver_pending_external_interrupt(cpu),
          "not into an STI shadow (SDM 29.3.1.5)");
    check(0 != interrupt_window(shadowed), "window armed");

    auto occupied = make();
    occupied.state->vmcs.write(
        zpp::arch::x86_64::vmx::vmcs::field::
            vm_entry_interruption_information_field,
        original_event::valid | original_event::hardware_exception | 14);
    occupied.state->queue_external_interrupt(cpu, 0x50);

    check(!occupied.state->deliver_pending_external_interrupt(cpu),
          "and not on top of an event the exit interrupted, which the "
          "guest was owed first");
    check_equal(original_event::valid |
                    original_event::hardware_exception | 14,
                entry_field(occupied),
                "the staged event is untouched");
}

/**
 * A halted processor takes the interrupt and is made active.
 *
 * KVM writes the activity state back for the same reason in
 * `vmx_clear_hlt` (.references/kvm/vmx.c:1817).
 */
void a_halted_processor_is_woken_by_the_interrupt()
{
    auto built = make();
    built.state->vmcs.guest_activity_state(activity::hlt);
    built.state->queue_external_interrupt(cpu, 0x50);

    check(built.state->deliver_pending_external_interrupt(cpu),
          "an external interrupt may be injected into a halted "
          "processor (SDM 29.3.1.5)");
    check_equal(activity::active,
                built.state->vmcs.guest_activity_state(),
                "and the activity state says the processor is running "
                "again");
    check_equal(1,
                built.state->external_interrupts_hlt_cleared[cpu],
                "counted, so a guest that halts a lot is visible");
}

/**
 * Nothing goes into a second-level guest.
 *
 * The interrupt was signalled to the physical processor, so it is the
 * first-level guest's - the guest hypervisor's - and vmcs02's
 * entry-interruption field would deliver it to the second-level guest's
 * interrupt descriptor table instead. One a guest hypervisor asked to see
 * never reaches here: `l1_wants_l2_exit` reflects that exit first.
 */
void a_second_level_guest_is_never_given_a_host_interrupt()
{
    if constexpr (!zpp::hypervisor::nested_vmx::enabled) {
        return;
    } else {
        auto built = make();
        built.state->running_l2[cpu] = true;
        built.state->queue_external_interrupt(cpu, 0x50);

        check(!built.state->deliver_pending_external_interrupt(cpu),
              "held while a second-level guest is about to run");
        check_equal(0, entry_field(built), "nothing staged into vmcs02");
        check_equal(0,
                    interrupt_window(built),
                    "and no control written, because the VMCS in hand is "
                    "the second-level guest's");
        check_equal(1,
                    built.state->external_interrupts_deferred_in_l2[cpu],
                    "counted, because nothing here bounds the wait");

        built.state->running_l2[cpu] = false;
        check(built.state->deliver_pending_external_interrupt(cpu),
              "and delivered once the first-level guest runs again");
    }
}

/**
 * The window a *guest hypervisor* armed survives an exit with nothing
 * queued.
 *
 * This is the other half of the case above, and it was the defect. The
 * not-found branch closes the interrupt window this VMM may have armed on
 * an earlier entry, and it used to sit *above* the second-level guard - so
 * an exit taken with vmcs02 current and an empty queue cleared the bit out
 * of vmcs02. That bit is never this VMM's: `build_vmcs02` composes the
 * primary controls as `(primary01 & ~(interrupt_window | nmi_window)) |
 * primary12`, so its only source is vmcs12.
 *
 * A guest hypervisor arms that window when it has an interrupt it cannot
 * yet deliver, and the exit is how it is told it may. Removing it under
 * the guest hypervisor is therefore not a lost optimisation, it is a
 * livelock: the measured shape was a guest hypervisor rewriting its
 * primary controls on 37% of its VMWRITEs and injecting on 0.1%, around
 * 940 exits a second, indefinitely.
 */
void a_second_level_guests_own_interrupt_window_is_left_alone()
{
    if constexpr (!zpp::hypervisor::nested_vmx::enabled) {
        return;
    } else {
        auto built = make();

        // What `build_vmcs02` leaves behind for a guest hypervisor that
        // asked for the window, and an empty queue.
        auto primary =
            built.state->vmcs
                .primary_processor_based_vm_execution_controls();
        built.state->vmcs.primary_processor_based_vm_execution_controls(
            primary | primary_interrupt_window);
        built.state->running_l2[cpu] = true;

        check(!built.state->deliver_pending_external_interrupt(cpu),
              "nothing to deliver, so nothing is delivered");
        check(0 != interrupt_window(built),
              "and the window the guest hypervisor armed is still there, "
              "because the VMCS in hand is the second-level guest's");
        check_equal(0,
                    built.state->external_interrupts_deferred_in_l2[cpu],
                    "with nothing deferred, since nothing was queued");

        // And the close still happens for the level it belongs to.
        built.state->running_l2[cpu] = false;
        check(!built.state->deliver_pending_external_interrupt(cpu),
              "still nothing to deliver at the first level");
        check_equal(0,
                    interrupt_window(built),
                    "and there the window is closed, because that one is "
                    "this VMM's own");
    }
}

/**
 * A slot outside the table is refused rather than written past.
 */
void the_queue_ignores_a_slot_it_does_not_have()
{
    auto built = make();
    built.state->queue_external_interrupt(
        zpp::hypervisor::hypervisor::max_cpus, 0x50);
    check(!built.state->deliver_pending_external_interrupt(
              zpp::hypervisor::hypervisor::max_cpus),
          "a processor slot the table does not have is left alone");
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
                g_observed.record_exits,
                "and the exit is recorded once, after the handlers have "
                "had their say");
    check_equal(1,
                zpp::arch::x86_64::g_restore_count,
                "and the guest is entered exactly once");
}

// === The clock this VMM hands the guest ================================

/**
 * The one property `ZPP_TIME_DILATION` rests on: the counter every
 * level reads goes forward, never back.
 *
 * It is arithmetic rather than a check in the code - `root - root / n`
 * can never exceed `root`, so what is left for the guest is `root / n`,
 * which is at worst zero - and this is what pins that arithmetic in
 * place. The failure it guards against is somebody making the
 * subtraction depend on something other than the interval just
 * measured, at which point a guest whose time-stamp counter steps
 * backwards bugchecks on a machine nobody can attach a debugger to.
 */
void the_dilated_counter_never_runs_backwards()
{
    auto built = make();
    constexpr std::uint64_t divisor = 8;

    // A first entry with no mark charges nothing and only sets one, so
    // the very first exit of a boot cannot be charged an interval that
    // began before the counter was read.
    built.state->apply_time_dilation(cpu, 1000);

    check_equal(0,
                built.state->dilation_offset[cpu],
                "the first entry moves the offset by nothing");
    check_equal(1000,
                built.state->dilation_mark[cpu],
                "and leaves the mark where root operation was entered");

    // Then a sequence of exits, each one a different length, with the
    // guest's own counter read at every entry.
    std::uint64_t marks[] = {1000, 1080, 1080 + 64, 1080 + 64 + 1000};
    auto previous = 1000 + built.state->dilation_offset[cpu];

    for (std::size_t i = 1; i < std::size(marks); ++i) {
        auto root = marks[i] - marks[i - 1];

        built.state->dilation_mark[cpu] = marks[i - 1];
        built.state->apply_time_dilation(cpu, marks[i]);

        auto seen = marks[i] + built.state->dilation_offset[cpu];

        check_equal(root / divisor,
                    seen - previous,
                    "the guest is charged a divisor's worth of the time "
                    "this VMM spent in root operation");
        previous = seen;
    }

    // And the two halves of the wall clock account for all of it.
    check_equal(marks[std::size(marks) - 1] - marks[0],
                built.state->dilation_hidden[cpu] +
                    built.state->dilation_charged[cpu],
                "hidden and charged sum to the whole interval");
}

/**
 * Reaching the same exit twice charges the guest once.
 *
 * `resume_guest` is called from the nested path as well as from the end
 * of `on_vm_exit`, so this is not hypothetical - and charging one span
 * twice would take the guest's counter backwards by exactly the amount
 * the test above proves it never goes.
 */
void one_exit_is_charged_once()
{
    auto built = make();

    built.state->dilation_mark[cpu] = 1000;
    built.state->apply_time_dilation(cpu, 1800);
    auto once = built.state->dilation_offset[cpu];

    built.state->apply_time_dilation(cpu, 1800);

    check_equal(once,
                built.state->dilation_offset[cpu],
                "the second call over the same instant charges nothing");
}

/**
 * Which VMCS the offset lands in, and what it carries there.
 *
 * vmcs02's field is the sum of both levels' offsets - the processor
 * applies one field, so the guest hypervisor's own offset for its guest
 * has to be inside it - while vmcs01's carries only this VMM's, there
 * being no level above. Getting this the wrong way round would leave a
 * second-level guest's counter jumping by the guest hypervisor's offset
 * every time it was resumed without a rebuild.
 */
void the_offset_composes_for_the_level_being_entered()
{
    using field = zpp::arch::x86_64::vmx::vmcs::field;

    auto built = make();
    built.state->tsc_offset_from_guest[cpu] = 0x5000;

    built.state->dilation_mark[cpu] = 1000;
    built.state->running_l2[cpu] = false;
    built.state->apply_time_dilation(cpu, 1800);

    auto ours = built.state->dilation_offset[cpu];

    check_equal(ours,
                built.state->vmcs.read(field::tsc_offset),
                "the guest hypervisor's own VMCS carries this VMM's "
                "offset alone");

    built.state->dilation_mark[cpu] = 1800;
    built.state->running_l2[cpu] = true;
    built.state->apply_time_dilation(cpu, 2600);

    check_equal(built.state->dilation_offset[cpu] + 0x5000,
                built.state->vmcs.read(field::tsc_offset),
                "and a second-level guest's carries both levels'");
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
    an_external_interrupt_needs_the_interrupt_flag();
    an_external_interrupt_goes_back_when_interrupts_are_enabled();
    the_interrupt_flag_binds_only_external_interrupts();
    an_nmi_put_back_clears_blocking_by_nmi();
    only_an_nmi_clears_blocking_by_nmi();
    nothing_is_put_back_without_a_pending_event();
    a_slot_outside_the_table_is_left_alone();
    rip_advances_only_when_asked();
    queued_interrupts_go_out_highest_first();
    a_second_vector_does_not_displace_the_first();
    the_same_vector_twice_is_counted_as_a_drop();
    a_masked_guest_gets_an_interrupt_window();
    a_shadow_or_a_staged_event_defers_the_interrupt();
    a_halted_processor_is_woken_by_the_interrupt();
    a_second_level_guest_is_never_given_a_host_interrupt();
    a_second_level_guests_own_interrupt_window_is_left_alone();
    the_queue_ignores_a_slot_it_does_not_have();
    the_entry_is_chosen_by_launch_state();
    the_resume_records_where_it_left_the_guest();
    the_dilated_counter_never_runs_backwards();
    one_exit_is_charged_once();
    the_offset_composes_for_the_level_being_entered();

    if (!g_findings.empty()) {
        std::println("\nfindings:");
        for (const auto & finding : g_findings) {
            std::println("  - {}", finding);
        }
    }

    std::println(
        "\nresume_guest: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
