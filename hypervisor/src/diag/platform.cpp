#include "zpp/diag/config.h"

#if ZPP_DIAG

#include "zpp/arch/x86_64/asm.h"

/**
 * The two things the diagnostic facility needs from whoever hosts it.
 *
 * Declared in zpp/diag/config.h and deliberately left undefined there, so
 * that the loader and the hypervisor can answer them differently. This is
 * the hypervisor's answer.
 *
 * Both are called once per recorded line, from inside a VM exit, so
 * neither may take a lock, allocate, or read the VMCS.
 */
namespace zpp::diag
{
/**
 * Which processor is executing.
 *
 * Through `cpuid`, and **not** through `rdmsr` of `IA32_X2APIC_APICID`.
 * That is what this used to do, on the stated reasoning that the MSR is
 * readable whatever the guest has done to its own APIC. The reasoning
 * was wrong, and wrong in a way that took the whole VMM down: SDM 13.12.2
 * says accessing anything in the MSR range 0800H-08FFH while the local
 * APIC is not in x2APIC mode raises a general-protection exception. The
 * mode is a property of the machine, not of the guest, and this
 * hypervisor already knows the machine can be in xAPIC mode - it is
 * exactly the case `watch_local_apic` arms for. So the first line ever
 * recorded on such a machine took a #GP inside the diagnostic facility,
 * which is a poor way for a diagnostic facility to behave.
 *
 * Leaf 0BH answers the same question with no mode dependency at all: the
 * SDM's own table says EDX[31:0] is "the x2APIC ID of the current logical
 * processor" and is "always valid". Leaf 1's EBX[31:24] is the fallback
 * for a processor too old to have 0BH, which is the eight bit initial
 * APIC id - narrower, and enough, since the caller folds the answer
 * anyway.
 *
 * Costs more than the MSR read did. That is the correct trade for an
 * instruction that cannot fault.
 *
 * The caller reduces this modulo the ring's processor count, so a machine
 * with sparse or large APIC ids folds rather than reads out of bounds. It
 * can therefore collide - two processors sharing a ring - which costs
 * interleaved lines and no correctness, since every record carries its
 * own timestamp.
 */
std::size_t current_processor()
{
    std::uint32_t registers[4]{};

    // The highest leaf this processor answers, before asking for one.
    // Asking for a leaf above the maximum does not fault, it returns
    // some other leaf's contents, which would be an apic id made of
    // whatever happened to be there.
    arch::x86_64::cpuid(0, 0, registers);
    auto highest_leaf = registers[0];

    constexpr std::uint32_t extended_topology_leaf = 0x0b;
    if (highest_leaf >= extended_topology_leaf) {
        arch::x86_64::cpuid(extended_topology_leaf, 0, registers);
        return static_cast<std::size_t>(registers[3]);
    }

    constexpr std::uint32_t feature_leaf = 1;
    arch::x86_64::cpuid(feature_leaf, 0, registers);
    return static_cast<std::size_t>(registers[1] >> 24);
}

/**
 * A monotonic tick.
 *
 * The time stamp counter, which config.h names as the intended source:
 * invariant across cores on anything this runs on, and shared by nothing,
 * so the write path has nothing to contend on. It is only ever used to put
 * the per-processor rings back into one order when they are read, so its
 * absolute value and its frequency do not matter.
 */
std::uint64_t timestamp()
{
    return arch::x86_64::rdtsc();
}

} // namespace zpp::diag

#endif
