#pragma once
// Shim for the architectural instruction wrappers, and it exists as a
// second copy rather than a share of tests/nested_vmx/shim's for one
// reason: `resume_guest` is [[noreturn]] and ends in `restore_context`,
// so a harness that wants to look at what it wrote needs that call to
// come back to it. The shared shim traps there, which is right for a
// harness that never reaches it and useless for this one.
//
// The way back is `longjmp`, which is sound here for the same reason it
// is sound anywhere: every object alive in the frames it unwinds is
// scalar, so there is no destructor to skip. The context is copied out
// first, because it names the entry the resume chose and that is one of
// the things under test.
#include "zpp/arch/x86_64/context.h"

#include <csetjmp>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * Where the resume path lands, and what it was carrying.
 */
inline std::jmp_buf g_resume_escape{};
inline context g_restored_context{};
inline std::uint64_t g_restore_count{};

// The harness never takes the second-arrival path, so a plain no-op is
// enough.
inline void capture_context(context *)
{
}

[[noreturn]] inline void restore_context(context * from)
{
    g_restored_context = *from;
    g_restore_count += 1;
    std::longjmp(g_resume_escape, 1);
}

// Model-specific registers, declared rather than defined: nothing on this
// path reads one, and a definition would only invite a test to depend on
// a value no processor supplied.
std::uint64_t rdmsr(std::uint32_t index);
void wrmsr(std::uint32_t index, std::uint64_t value);

// The time stamp counter. Monotonic rather than real: a harness cares
// that successive readings differ and increase, not what they mean.
inline std::uint64_t rdtsc()
{
    static std::uint64_t ticks{};
    return ticks += 1000;
}

} // namespace zpp::arch::x86_64
