#pragma once
#include "zpp/arch/x86_64/context.h"
#include <cstdint>

namespace zpp::arch::x86_64
{
// The harness never takes the second-arrival path, so a plain no-op is
// enough: capture_context returns once and nested_entry_failed stays
// false.
inline void capture_context(context *)
{
}

[[noreturn]] inline void restore_context(context *)
{
    __builtin_trap();
}

// Model-specific registers, declared rather than defined: a harness that
// executes code reading them supplies the answers it wants observed, and
// one that does not never links them in.
std::uint64_t rdmsr(std::uint32_t index);
void wrmsr(std::uint32_t index, std::uint64_t value);

// The time stamp counter, which the local APIC filter samples alongside
// every timer arming. Monotonic rather than real: a harness cares that
// successive readings differ and increase, not what they mean.
inline std::uint64_t rdtsc()
{
    static std::uint64_t ticks{};
    return ticks += 1000;
}

} // namespace zpp::arch::x86_64
