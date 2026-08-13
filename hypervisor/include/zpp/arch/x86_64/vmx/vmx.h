#pragma once
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * The VMX VMCS region structure.
 */
struct vmx_vmcs
{
    std::uint32_t revision_id{};
    std::uint32_t abort_indicator{};
    std::uint8_t data[0x1000 - (sizeof(std::uint32_t) * 2)]{};
};

/**
 * The primary execution controls.
 */
namespace vm_execution_controls::primary
{
enum type : std::uint64_t
{
    // MONITOR and MWAIT are both intercepted and emulated as no-ops, so a
    // guest that would wait polls instead. That costs power and nothing
    // else, because SDM 29.3.3, "Clearing Address-Range Monitoring", and
    // SDM 30.5.6 clear address-range monitoring on every VM entry and
    // every VM exit - a monitor armed inside a guest cannot survive long
    // enough to be waited on, so there is no behaviour here to preserve.
    //
    // What must not accompany this is hiding the feature from CPUID, and
    // that pairing is the subtle part. Leaf 1 ECX[3] is left exactly as
    // the hardware reports it. Clearing it looks like the honest answer -
    // refuse the wait, so stop advertising the wait - and it bugchecks
    // Windows on real firmware. Windows builds an idle state for every
    // ACPI FFH C-state the firmware reports, and FFH means MWAIT, so with
    // the monitor hidden its platform layer installs no handler for that
    // state. PpmIdleExecuteTransition calls the handler anyway: the
    // pointer beside it is null-checked and this one is not. Kernel CFG
    // catches the indirect call through null and reports
    // KERNEL_SECURITY_CHECK_FAILURE, 0x139, parameter 1 = 0x0a,
    // FAST_FAIL_GUARD_ICALL_CHECK_FAILURE.
    //
    // Measured from the crash dump rather than reasoned about, which is
    // the only reason it was found: stack KiIdleLoop -> PoIdle ->
    // PpmIdleExecuteTransition -> _guard_dispatch_icall, target register
    // zero, and parameter 4 zero because _guard_icall_bugcheck passes the
    // rejected target through. Two dumps agreed to the byte modulo the
    // kernel's load address. Only real firmware reaches it - an emulator
    // reporting no FFH C-states never builds that idle state - so every
    // test rig missed it and the machine bugchecked the moment it first
    // went idle.
    //
    // So the two halves answer different questions and both answers are
    // deliberate: the guest is told the monitor exists, and is quietly
    // refused the wait. Advertising a feature and then declining it is
    // usually this codebase's cardinal sin; here declining it costs a poll
    // and concealing it costs a null function pointer in the guest's
    // kernel, which is worse.
    //
    // SDM Table 25-6, "Definitions of Primary Processor-Based
    // VM-Execution Controls", bits 10 and 29.
    mwait_exiting = (1ull << 10),

    /**
     * The controls that intercept an instruction which would otherwise
     * simply execute in the guest. SDM Table 25-6, "Definitions of
     * Primary Processor-Based VM-Execution Controls", by bit.
     *
     * **None of these is requested by a deployed build, and each is
     * requested by a ZPP_GUEST_TESTS one.** Their exit reasons are
     * otherwise unreachable from inside a guest, which left the handler
     * cases for them either absent or unexecutable - and an exit reason
     * with no case reaches `default:` and stops the processor, so
     * "unreachable" and "would stop the machine if it ever happened" were
     * the same sentence. Turning each on in the test build makes the
     * reason reachable and its case live; see `setup_vmcs`, where the
     * same argument is made at length for MONITOR and MWAIT above.
     * @{
     */
    hlt_exiting = (1ull << 7),
    invlpg_exiting = (1ull << 9),
    rdpmc_exiting = (1ull << 11),
    rdtsc_exiting = (1ull << 12),
    mov_dr_exiting = (1ull << 23),
    pause_exiting = (1ull << 30),
    /**
     * @}
     */

    // SDM Table 25-6 bit 25. With it set, an I/O instruction exits only
    // when its port's bit is set in one of the two bitmaps below - so
    // this is how a single port is watched without paying for every
    // other one. Bit 24, unconditional I/O exiting, would trap them all.
    enable_io_bitmaps = (1ull << 25),
    enable_msr_bitmaps = (1ull << 28),
    monitor_exiting = (1ull << 29),
    enable_secondary_controls = (1ull << 31),
};
} // namespace vm_execution_controls::primary

/**
 * The secondary execution controls.
 */
namespace vm_execution_controls::pin
{
enum type : std::uint64_t
{
    /**
     * SDM Table 27-5, bit 6: "If this control is 1, the VMX-preemption
     * timer counts down in VMX non-root operation... A VM exit occurs
     * when the timer counts down to zero."
     *
     * The only way this VMM has of making itself look at something on a
     * schedule. A guest that is running is usually taking exits, so
     * polling on the exit path is normally enough - but a guest spinning
     * on a memory mapped read of a passed through device takes none at
     * all, and that is exactly the window where the storage controller's
     * state has to be noticed. This manufactures the exits.
     *
     * Armed only while something is waiting to be noticed, because it
     * costs the guest an exit every time it fires.
     */
    /**
     * SDM Table 27-5, bit 3: "If this control is 1, NMIs cause VM exits.
     * Otherwise, they are delivered using vector 2."
     *
     * Wanted for one reason: an NMI is the only thing that reliably
     * takes a processor out of whatever it is doing, including a halt.
     * Nothing else this VMM can send does - an ordinary interrupt is
     * delivered straight into the guest's handler while
     * "external-interrupt exiting" is clear, and a halted processor
     * reaches no exit path of its own at all.
     *
     * The price is that a genuine NMI from the guest's own world now
     * arrives here instead, and has to be handed back rather than
     * swallowed. See on_nmi.
     */
    /**
     * SDM Table 27-5, bit 0: an external interrupt causes a VM exit
     * instead of being delivered through the guest's own interrupt
     * descriptor table.
     *
     * Off by default and deliberately: the interrupts are the guest's
     * and the guest owns the interrupt controller, so letting them
     * arrive natively costs no exit at all. `ZPP_VIRTUALIZE_APIC` turns
     * it on, which is the one architectural difference between this VMM
     * and KVM that a measurement has ever pointed at.
     *
     * It is *not* the same thing KVM uses the control for, and the
     * comment here used to say it was. KVM sets it because the
     * interrupts belong to its host: it runs the host's own IDT handler
     * for the vector in `handle_external_interrupt_irqoff`, and its
     * guests are given a different interrupt entirely, synthesized by
     * the virtual local APIC in `lapic.c`. Here the guest owns the
     * physical APIC, so the same acknowledged vector is what goes back
     * in - see `deliver_pending_external_interrupt`, and the BACKLOG
     * entry on why that is sound and where it is not.
     */
    external_interrupt_exiting = (1ull << 0),
    nmi_exiting = (1ull << 3),

    activate_preemption_timer = (1ull << 6),
};

} // namespace vm_execution_controls::pin

namespace vm_execution_controls::secondary
{
enum type : std::uint64_t
{
    enable_ept = (1ull << 1),
    enable_rdtscp = (1ull << 3),
    enable_vpid = (1ull << 5),
    // Without this a guest may not run with CR0.PE or CR0.PG clear, and
    // an application processor coming out of a start-up IPI does exactly
    // that: it begins in real mode.
    unrestricted_guest = (1ull << 7),
    enable_invpcid = (1ull << 12),
    // Lets a guest hypervisor read and write the fields named by the
    // VMREAD and VMWRITE bitmaps against a shadow region in memory,
    // without an exit. SDM 26.2 and 27.3.
    vmcs_shadowing = (1ull << 14),
    enable_xsaves_xrstors = (1ull << 20),
    mode_based_execute_control = (1ull << 22),

    /**
     * The secondary half of the same family as the primary controls
     * above: instructions a deployed build lets run and a
     * ZPP_GUEST_TESTS build intercepts so their exit reasons are
     * reachable. SDM Table 25-7, "Definitions of Secondary Processor-
     * Based VM-Execution Controls", by bit.
     * @{
     */
    wbinvd_exiting = (1ull << 6),
    rdrand_exiting = (1ull << 11),
    rdseed_exiting = (1ull << 16),
    /**
     * @}
     */
};
} // namespace vm_execution_controls::secondary

/**
 * The VM exit controls.
 */
namespace vm_exit_controls
{
enum type : std::uint64_t
{
    // SDM Table 25-13 bit 2. Saves the guest's DR7 and IA32_DEBUGCTL
    // into the VMCS on exit. Without it they are not saved, and since
    // SDM 28.5.1 sets DR7 to 400H on every exit regardless, whatever the
    // guest had in it is simply gone.
    save_debug_controls = (1ull << 2),
    host_address_space_size = (1ull << 9),
    /**
     * SDM 30.2: without this an external-interrupt VM exit leaves the
     * interrupt pending at the controller and reports no vector; with it
     * the controller is acknowledged and the vector lands in the
     * exit-interruption-information field. Only the processor can take a
     * vector from the controller, so injecting one requires this.
     */
    acknowledge_interrupt_on_exit = (1ull << 15),
};
} // namespace vm_exit_controls

/**
 * Guest activity states, as held in the guest activity state field.
 * SDM 27.4.2, "Guest Non-Register State". Which of these a processor
 * supports is reported by IA32_VMX_MISC - see SDM A.6 - and writing an
 * unsupported one fails VM entry.
 *
 * wait_for_start_up_ipi is the one that matters here: it is the state an
 * application processor is left in after an INIT, and the only state from
 * which a start-up IPI will start it.
 */
namespace activity_state
{
enum type : std::uint64_t
{
    active = 0,
    hlt = 1,
    shutdown = 2,
    wait_for_start_up_ipi = 3,
};
} // namespace activity_state

/**
 * The VM entry interruption information field, which is how the VMM asks
 * the processor to deliver an event to the guest on the next entry.
 */
namespace vm_entry_interruption
{
enum type : std::uint64_t
{
    /**
     * The vector, in bits 7:0.
     */
    vector_mask = 0xffull,

    /**
     * The event type, in bits 10:8, and each of the values it takes.
     * SDM Table 27-18, and the same encoding the original-event
     * identification field uses on the way out - SDM Table 27-21, which
     * is why an interrupted event can be put back through this field at
     * all.
     *
     * Values 1 and 7 are deliberately absent. SDM 29.2.1.3 makes 1
     * reserved on every processor and 7 reserved on any that supports
     * neither the monitor trap flag nor FRED, and SDM Table 27-21 marks
     * both "not used" on the way out - so an event coming back through
     * here can never legitimately carry one.
     */
    type_mask = (7ull << 8),
    external_interrupt = (0ull << 8),
    non_maskable_interrupt = (2ull << 8),
    hardware_exception = (3ull << 8),
    software_interrupt = (4ull << 8),
    privileged_software_exception = (5ull << 8),
    software_exception = (6ull << 8),

    /**
     * Set when the vector pushes an error code, which the processor takes
     * from the VM entry exception error code field.
     */
    deliver_error_code = (1ull << 11),

    /**
     * Set to make the field mean anything at all. Cleared by the processor
     * once the event has been delivered.
     */
    valid = (1ull << 31),

    /**
     * Every bit the architecture defines in this field, for a value
     * copied from somewhere else.
     *
     * SDM 29.2.1.3 requires bits 30:14 and 12 to be 0 and permits bit 13
     * only where IA32_VMX_BASIC[58] says FRED transitions exist, so a
     * word carrying anything outside this mask fails VM entry rather than
     * delivering anything. It exists because the one value ever copied
     * into this field comes from the original-event identification field,
     * whose bit 13 the architecture *does* define - see SDM Table 27-21 -
     * and whose undefined bits are only guaranteed zero on the
     * processors of today. KVM never faces the question because it
     * rebuilds the event out of its vector and type rather than copying
     * the word (`__vmx_complete_interrupts`,
     * .references/kvm/vmx.c:7105); masking is the same answer at one
     * instruction.
     */
    defined_bits = vector_mask | type_mask | deliver_error_code | valid,
};

/**
 * Whether an event may be injected into a guest in a given activity
 * state, which VM entry checks and fails rather than ignores.
 *
 * SDM 29.3.1.5, "Checks on Guest Non-Register State": "the event to be
 * delivered (as defined by event type and vector) must not be one that
 * would normally be blocked while a logical processor is in the activity
 * state corresponding to the contents of the activity-state field", and
 * then enumerates them state by state. The three that are not `active`
 * are the ones that matter here: a processor parked by an emulated INIT
 * is in wait-for-SIPI, which permits nothing at all.
 *
 * Written against the field rather than against a vector and a type
 * separately, because that is the shape the value has wherever it comes
 * from - the entry field on the way in and the original-event field on
 * the way out use the same encoding.
 */
constexpr bool allowed_in(std::uint64_t activity, std::uint64_t event)
{
    // The debug exception and the machine-check exception, which are the
    // two hardware exceptions a halted processor is still able to take.
    constexpr std::uint64_t debug_exception_vector = 1;
    constexpr std::uint64_t machine_check_vector = 18;

    if (0 == (event & valid)) {
        return true;
    }

    auto event_type = event & type_mask;
    auto event_vector = event & vector_mask;

    switch (activity) {
    case activity_state::active:
        // "Active. Any event is allowed."
        return true;

    case activity_state::hlt:
        // "HLT. The only events allowed are the following: those with
        // event type external interrupt or non-maskable interrupt (NMI);
        // those with event type hardware exception and vector 1 (debug
        // exception) or vector 18 (machine-check exception); those with
        // event type other event and vector 0 (pending MTF VM exit)."
        //
        // The last of those is absent deliberately: nothing in this VMM
        // injects a pending monitor trap flag exit, and type 7 cannot
        // arrive through the original-event field at all.
        return (external_interrupt == event_type) ||
               (non_maskable_interrupt == event_type) ||
               ((hardware_exception == event_type) &&
                ((debug_exception_vector == event_vector) ||
                 (machine_check_vector == event_vector)));

    case activity_state::shutdown:
        // "Shutdown. Only NMIs and machine-check exceptions are allowed."
        return (non_maskable_interrupt == event_type) ||
               ((hardware_exception == event_type) &&
                (machine_check_vector == event_vector));

    case activity_state::wait_for_start_up_ipi:
        // "Wait-for-SIPI. No events are allowed."
        return false;

    default:
        // An activity state this architecture does not define. SDM
        // 29.3.1.5 fails the entry on the state itself, so whatever is
        // answered here the entry is already lost; refusing keeps this
        // side from being the reason.
        return false;
    }
}
} // namespace vm_entry_interruption

/**
 * The guest interruptibility-state field, SDM Table 27-3.
 */
namespace interruptibility_state
{
enum type : std::uint64_t
{
    blocking_by_sti = (1ull << 0),
    blocking_by_mov_ss = (1ull << 1),
    blocking_by_smi = (1ull << 2),

    /**
     * Blocking by NMI, which VM entry refuses to see set alongside an
     * injected NMI whenever the "virtual NMIs" VM-execution control is 1.
     * SDM 29.3.1.5: "Bit 3 (blocking by NMI) must be 0 if the 'virtual
     * NMIs' VM-execution control is 1, the valid bit (bit 31) in the
     * injected-event identification field is 1, and the event type (bits
     * 10:8) in that field has value 2 (indicating NMI)."
     */
    blocking_by_nmi = (1ull << 3),
};
} // namespace interruptibility_state

/**
 * The VM entry controls.
 */
namespace vm_entry_controls
{
enum type : std::uint64_t
{
    // SDM Table 25-9 bit 2. Loads the guest's DR7 and IA32_DEBUGCTL from
    // the VMCS on entry. Without it those two fields are written and
    // never read - the guest resumes with whatever the last exit left in
    // the registers, which for DR7 is always 400H.
    load_debug_controls = (1ull << 2),
    ia_32e_mode_guest = (1ull << 9),
};
} // namespace vm_entry_controls

} // namespace zpp::arch::x86_64::vmx