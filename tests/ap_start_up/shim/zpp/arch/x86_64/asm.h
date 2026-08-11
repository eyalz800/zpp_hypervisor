#pragma once
// Shim for the instruction wrappers the start-up path uses. Its own
// rather than tests/nested_vmx/shim's, because enter_root_mode reads and
// writes the control registers and this harness has to run four of these
// at once on host threads - so they are thread_local, standing in for
// four logical processors' worth of state.
#include "zpp/arch/x86_64/context.h"
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
 * CPUID, which apply_start_up reads leaf 1 from for the value SDM Table
 * 12-1 puts in EDX after an INIT: the family, model and stepping.
 *
 * A fixed answer rather than the host's. This harness runs on arm64 as
 * often as not, and even on an x86 host the point is that the *guest's*
 * RDX ends up holding leaf 1's EAX - not that this machine's stepping is
 * whatever it is.
 */
inline constexpr std::uint32_t identification_leaf_1_eax = 0x000806ec;

inline void cpuid(std::uint64_t leaf,
                  std::uint64_t,
                  std::uint32_t (&out)[4])
{
    out[0] = (1 == leaf) ? identification_leaf_1_eax : 0;
    out[1] = 0;
    out[2] = 0;
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

} // namespace zpp::arch::x86_64
