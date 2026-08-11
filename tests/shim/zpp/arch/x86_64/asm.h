#pragma once
// The instruction wrappers, standing in for the real ones.
//
// This is one of the two headers in the tree that a hosted harness is
// allowed to replace, and the reason is not a convenience: the real
// zpp/arch/x86_64/asm.h is `__attribute__((naked))` x86-64 assembly, and
// the machine running these tests is an arm64 Mac. Nothing else under
// zpp/ is shimmed - the class under test, the VMCS, vmcs12, the log and
// the page table walkers are all the real headers.
#include "zpp/arch/x86_64/context.h"

#include <csetjmp>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * Where a resumed context lands, and what it was carrying.
 *
 * `restore_context` is `[[noreturn]]`, so a harness that wants to look
 * at what the code under test wrote just before it needs that call to
 * come back. `longjmp` is what brings it back, and it is sound here for
 * the reason it is sound anywhere: every object alive in the frames it
 * unwinds is scalar, so there is no destructor to skip.
 *
 * A harness that never reaches `restore_context` never sets
 * `g_resume_escape` and never longjmps, so this costs it nothing. This
 * used to be a second copy of this file under tests/resume_guest/shim
 * carrying only that difference.
 * @{
 */
inline std::jmp_buf g_resume_escape{};
inline context g_restored_context{};
inline std::uint64_t g_restore_count{};
/**
 * @}
 */

// The harness never takes the second-arrival path, so a plain no-op is
// enough: capture_context returns once and nested_entry_failed stays
// false.
inline void capture_context(context *)
{
}

[[noreturn]] inline void restore_context(context * from)
{
    g_restored_context = *from;
    g_restore_count += 1;
    std::longjmp(g_resume_escape, 1);
}

// Model-specific registers, declared rather than defined: a harness that
// executes code reading them supplies the answers it wants observed, and
// one that does not never links them in.
std::uint64_t rdmsr(std::uint32_t index);
void wrmsr(std::uint32_t index, std::uint64_t value);

/**
 * CPUID, answering zero for every leaf.
 *
 * The one caller reachable from a harness on this shim is
 * `zpp::diag::current_processor`, which folds whatever it gets modulo
 * the ring's processor count - so zero means "processor 0" and nothing
 * more. A harness that cares what a leaf says supplies its own, which is
 * what tests/ap_start_up/shim does.
 */
inline void cpuid(std::uint64_t, std::uint64_t, std::uint32_t (&out)[4])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
}

// The time stamp counter, which the local APIC filter samples alongside
// every timer arming. Monotonic rather than real: a harness cares that
// successive readings differ and increase, not what they mean.
inline std::uint64_t rdtsc()
{
    static std::uint64_t ticks{};
    return ticks += 1000;
}

} // namespace zpp::arch::x86_64
