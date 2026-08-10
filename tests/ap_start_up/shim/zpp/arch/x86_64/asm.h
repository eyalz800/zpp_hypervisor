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
