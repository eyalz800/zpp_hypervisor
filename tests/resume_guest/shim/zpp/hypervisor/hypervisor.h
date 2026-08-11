#pragma once
// A stand-in for the real hypervisor.h, carrying only what
// `hypervisor::resume_guest` touches. Everything the extracted source
// *defines* is declared here with the same signature; everything it
// *calls* that lives elsewhere in hypervisor.cpp is declared here and
// defined by the harness.
//
// A sixth shim rather than a widening of one of the five, for the reason
// each of those gives: a harness links one set of real definitions, and a
// class carrying every set would need every member of every path. What
// this one carries is the per-processor event state and the VMCS, because
// that is the whole of what the re-queue reads and writes.
//
// zpp/arch/x86_64/vmx/asm.h comes from tests/nested_vmx/shim, which is on
// the include path after this directory. zpp/arch/x86_64/asm.h and
// zpp/diag/log.h are shimmed *here* instead of shared: this harness needs
// a `restore_context` that comes back rather than one that traps, and a
// diagnostic side with the pump and the sink policy the resume path asks
// about.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmx.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/diag/log.h"
#include "zpp/error.h"
#include "zpp/hypervisor/nested_vmx.h"

#include <cstddef>
#include <cstdint>

namespace zpp::hypervisor
{
/**
 * The log, reduced to nothing. The resume path's own lines are a
 * heartbeat and say nothing about the decision under test, which is why
 * this one discards where tests/local_apic's records.
 */
template <typename... Types>
struct log
{
    log(const char *, Types &&...)
    {
    }
};

template <typename... Types>
log(const char *, Types &&...) -> log<Types...>;

class hypervisor
{
public:
    static constexpr std::size_t max_cpus = 32;

    // === Under test: cut verbatim out of hypervisor.cpp ================
    [[noreturn]] void resume_guest(arch::x86_64::context & context,
                                   arch::x86_64::vmx::exit_reason reason,
                                   bool advance_rip);

    // === Supplied by the harness =======================================
    void record_exit(arch::x86_64::vmx::exit_reason reason,
                     const arch::x86_64::context & context);
    void arm_controller_poll(bool armed);

    // === State the re-queue reads and writes ===========================
    arch::x86_64::vmx::vmcs vmcs{};

    std::uint64_t pending_event[max_cpus]{};
    std::uint64_t pending_event_error[max_cpus]{};
    std::uint64_t pending_event_length[max_cpus]{};
    std::uint64_t events_requeued[max_cpus]{};
    bool pending_event_l2[max_cpus]{};
    std::uint64_t events_deferred[max_cpus]{};

    /**
     * Which VMCS the guest hypervisor has current, which is the only
     * thing on hand that says *which* second-level guest is running - see
     * the note in harness.cpp on why an address alone is not an identity.
     */
    std::uint64_t guest_current_vmcs[max_cpus]{};

    bool running_l2[max_cpus]{};
    bool vmcs02_launched[max_cpus]{};
    bool relaunch_after_sleep[max_cpus]{};

    std::uint64_t heartbeat_exits_seen[max_cpus]{};
    volatile std::uint64_t resumes_reached[max_cpus]{};
    volatile std::uint64_t resume_activity_state[max_cpus]{};
    volatile std::uint64_t resume_guest_rip[max_cpus]{};
    volatile std::uint64_t resume_guest_cs[max_cpus]{};

    // === Harness observation ===========================================
    //
    // Not in the real class. `record_exit` and `arm_controller_poll`
    // reach state and hardware in the real VMM; here they only count, so
    // that "the resume path ran to the end" is observable separately from
    // what it wrote.
    std::uint64_t record_exit_count{};
    std::uint64_t controller_poll_count{};
};

} // namespace zpp::hypervisor
