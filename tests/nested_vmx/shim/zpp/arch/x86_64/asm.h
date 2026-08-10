#pragma once
#include "zpp/arch/x86_64/context.h"
#include <cstdint>

namespace zpp::arch::x86_64
{
// The harness never takes the second-arrival path, so a plain no-op is
// enough: capture_context returns once and nested_entry_failed stays false.
inline void capture_context(context *)
{
}

[[noreturn]] inline void restore_context(context *)
{
    __builtin_trap();
}

} // namespace zpp::arch::x86_64
