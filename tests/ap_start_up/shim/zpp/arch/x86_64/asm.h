#pragma once
// Shim for the instruction wrappers the start-up path uses. Its own
// rather than tests/nested_vmx/shim's, because enter_root_mode reads and
// writes the control registers and this harness has to run four of these
// at once on host threads - so they are thread_local, standing in for
// four logical processors' worth of state.
#include "zpp/arch/x86_64/context.h"
#include <atomic>
#include <cstdint>

namespace zpp::arch::x86_64
{
inline thread_local std::uint64_t g_cr0{};
inline thread_local std::uint64_t g_cr4{};

inline std::uint64_t cr0()
{
    return g_cr0;
}

inline std::uint64_t cr0(std::uint64_t value)
{
    g_cr0 = value;
    return value;
}

inline std::uint64_t cr4()
{
    return g_cr4;
}

inline std::uint64_t cr4(std::uint64_t value)
{
    g_cr4 = value;
    return value;
}

/**
 * DR6, which is not a VMCS guest field - the guest and host share the
 * register - so apply_start_up writes the architectural after-INIT value
 * to the real one. Recorded per thread so the harness can read it back.
 */
inline thread_local std::uint64_t g_dr6{};

inline std::uint64_t dr6()
{
    return g_dr6;
}

inline void dr6(std::uint64_t value)
{
    g_dr6 = value;
}

/**
 * DR0 through DR3, shared between guest and host exactly as DR6 is - SDM
 * 25.4 puts DR7 in the guest-state area and none of these four - so
 * apply_start_up writes SDM Table 12-1's after-INIT zero to the real
 * registers. Seeded non-zero by the harness so a missing write is a
 * failing check rather than a value that happened to be zero already.
 */
inline thread_local std::uint64_t g_debug_registers[4]{};

inline std::uint64_t debug_register(std::uint8_t index)
{
    return g_debug_registers[index & 3];
}

inline void debug_register(std::uint8_t index, std::uint64_t value)
{
    g_debug_registers[index & 3] = value;
}

/**
 * CR2, shared for the same reason - `write_cr2`'s own comment in the real
 * header says VMX neither saves nor restores it, so the after-INIT zero
 * of SDM Table 12-1's "CR2, CR3, CR4" row has to be written by hand.
 */
inline thread_local std::uint64_t g_cr2{};

inline std::uint64_t cr2()
{
    return g_cr2;
}

inline void write_cr2(std::uint64_t value)
{
    g_cr2 = value;
}

/**
 * CPUID, which apply_start_up reads leaf 1 from for the value SDM Table
 * 12-1 puts in EDX after an INIT: the family, model and stepping.
 *
 * A fixed answer rather than the host's. This harness runs on arm64 as
 * often as not, and even on an x86 host the point is that the *guest's*
 * RDX ends up holding leaf 1's EAX - not that this machine's stepping is
 * whatever it is.
 */
inline constexpr std::uint32_t identification_leaf_1_eax = 0x000806ec;

/**
 * Leaf 1's ECX bit 31, which is reserved on real hardware and is the
 * conventional way for a hypervisor to announce itself - SDM Vol. 2A,
 * "CPUID.01H:ECX Feature Information".
 *
 * Settable because `emulate_init_signal` decides which of the two
 * hand-offs to wait on by asking whether something is virtualizing *us*.
 * Both answers are a real configuration this VMM runs in: clear is bare
 * metal and Bochs, set is the rig, and the two take different paths
 * through the whole INIT handler.
 */
inline constexpr std::uint32_t hypervisor_present_bit = (1u << 31);
inline std::uint32_t g_leaf_1_ecx{};

/**
 * The initial APIC id leaf 1 reports in EBX bits 31:24, which is what
 * `local_apic_id` falls back to when neither topology leaf is answered -
 * this shim reports leaf 0 as zero, so that is the path it takes.
 */
inline std::uint32_t g_initial_apic_id{};

inline void cpuid(std::uint64_t leaf,
                  std::uint64_t,
                  std::uint32_t (&out)[4])
{
    out[0] = (1 == leaf) ? identification_leaf_1_eax : 0;
    out[1] = (1 == leaf) ? (g_initial_apic_id << 24) : 0;
    out[2] = (1 == leaf) ? g_leaf_1_ecx : 0;
    out[3] = 0;
}

inline void capture_context(context *)
{
}

[[noreturn]] inline void restore_context(context *)
{
    __builtin_trap();
}

std::uint64_t rdmsr(std::uint32_t index);
void wrmsr(std::uint32_t index, std::uint64_t value);

/**
 * IA32_APIC_BASE, as this harness answers it. Bit 10 is EXTD, which is
 * what `x2apic_enabled` reads, and the frame is what the xAPIC branch of
 * `send_start_up_ipi` builds its register addresses out of.
 */
inline std::atomic<std::uint64_t> g_apic_base{};

/**
 * The last write to the x2APIC interrupt command MSR, and how many there
 * have been. That MSR does not exist in xAPIC mode, so "how many" is the
 * question 2685265 turns on - an unconditional write to it faulted in the
 * host, with no recovery point, and halted the boot processor.
 */
inline std::atomic<unsigned> g_x2apic_icr_writes{};
inline std::atomic<std::uint64_t> g_x2apic_icr_last{};

// The time stamp counter. `interrupt_command.cpp` stamps every INIT and
// start-up IPI with it, so that a long enough silence can stand in for
// "no more processors are going to start" - see
// `hypervisor::last_start_up_ipi_tsc`. Monotonic rather than real, as in
// the shared shim beside this one: a harness cares that successive
// readings increase, not what they mean.
inline std::uint64_t rdtsc()
{
    static std::uint64_t ticks{};
    return ticks += 1000;
}

/**
 * A quadword through GS - `hypervisor::this_processor()` on the target,
 * where the VMCS's `host_gs_base` makes the base per processor with no
 * lookup of ours.
 *
 * `thread_local` rather than global for the reason the control registers
 * above are: this harness runs four host threads as four logical
 * processors, and a shared answer here would make every one of them
 * processor zero - which is exactly the failure the check in
 * `on_vm_exit` exists to catch on the target.
 */
inline thread_local std::uint64_t g_gs_qword{};

inline std::uint64_t gs_qword(std::uint64_t)
{
    return g_gs_qword;
}

} // namespace zpp::arch::x86_64
