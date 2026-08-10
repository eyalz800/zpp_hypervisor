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
     * The event type, in bits 10:8. Hardware exception is the kind a
     * faulting instruction would have raised on its own.
     */
    hardware_exception = (3ull << 8),

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
};
} // namespace vm_entry_interruption

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