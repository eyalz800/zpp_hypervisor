#include "zpp/diag/config.h"

#if ZPP_DIAG

#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/msr.h"

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
 * The x2APIC id through `rdmsr`, which config.h names as the
 * zero-dependency interim answer and costs about a hundred cycles. The
 * cheaper answer it points at - a per-processor block reached through the
 * host GS base - is not available: this VMM does not set one up, and
 * inventing one to save ninety cycles on a debug path would be the wrong
 * trade.
 *
 * `IA32_X2APIC_APICID` is readable regardless of whether the guest has
 * enabled x2APIC for itself, because this runs in root mode on a
 * processor whose own APIC the hypervisor has not switched out of x2APIC
 * mode. That is worth stating because the same MSR read from the *guest's*
 * perspective is exactly the case hypervisor.cpp already handles
 * specially.
 *
 * The caller reduces this modulo the ring's processor count, so a machine
 * with sparse or large APIC ids folds rather than reads out of bounds. It
 * can therefore collide - two processors sharing a ring - which costs
 * interleaved lines and no correctness, since every record carries its
 * own timestamp.
 */
std::size_t current_processor()
{
    constexpr std::uint32_t x2apic_id = 0x802;
    return static_cast<std::size_t>(arch::x86_64::rdmsr(x2apic_id));
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
