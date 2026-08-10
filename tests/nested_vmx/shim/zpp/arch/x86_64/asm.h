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

} // namespace zpp::arch::x86_64
