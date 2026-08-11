#pragma once
// A stand-in for the real hypervisor.h, carrying only what the
// application-processor start-up and adoption path touches. Everything
// the extracted sources *define* is declared here with the same
// signature; everything they *call* that lives elsewhere in
// hypervisor.cpp is declared here and defined by the harness.
//
// A fourth shim rather than a widening of the other three, for the reason
// each of those gives: a harness links one set of real definitions, and a
// class carrying every set would need every member of every path.
// zpp/diag/log.h is shared with tests/nested_vmx/shim; asm.h and
// vmx/asm.h are this harness's own, because this one runs four threads
// standing in for four logical processors and their state has to be per
// thread.
#include "zpp/arch/x86_64/ap_start_up.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/arch/x86_64/segment_descriptor.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmx.h"
#include "zpp/error.h"
#include "zpp/scope_exit.h"
#include "zpp/spin_lock.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace zpp::hypervisor
{
/**
 * The log, reduced to a recorder the harness can inspect. Same shape as
 * the real one: constructed as a temporary at the call site.
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
    enum class error
    {
        success = 0,
        too_many_processors = 8,
        vmxon_failed = 30,
        vmclear_failed = 31,
        vmptrld_failed = 32,
    };

    static constexpr std::size_t max_cpus = 32;
    static constexpr std::size_t page_size = 0x1000;

    /**
     * What starting one processor needed. Same two outcomes as the real
     * header.
     */
    enum class start_up_result
    {
        adopted,
        needs_hardware,
    };

    /**
     * The hand-off states, copied from the real header because they are
     * declared inside the class there and this shim replaces the class.
     * The values are asserted against the real ones by the harness, which
     * reads them out of hypervisor.h, so a change to either fails here.
     */
    struct start_up_handoff_state
    {
        static constexpr std::uint64_t none = 0;
        static constexpr std::uint64_t software_wait = 1;
        static constexpr std::uint64_t hardware_wait = 2;
        static constexpr std::uint64_t delivered = 3;

        static constexpr std::uint64_t deliver(std::uint64_t vector)
        {
            return delivered + vector;
        }

        static constexpr bool is_delivered(std::uint64_t state)
        {
            return state >= delivered;
        }

        static constexpr std::uint64_t vector(std::uint64_t state)
        {
            return state - delivered;
        }
    };

    /**
     * The host page table, reduced to the one query these paths make of
     * it. The harness answers with the host address, which is the same
     * identity tests/watched_page relies on.
     */
    struct page_table_stub
    {
        std::uint64_t virtual_to_physical(const void * address) const
        {
            return reinterpret_cast<std::uint64_t>(address);
        }
    };

    // ------------------------------------------- the code under test
    std::optional<std::size_t> processor_slot(std::uint64_t apic_id);

    start_up_result start_up_processor(std::uint64_t destination,
                                       std::uint64_t vector);

    bool start_application_processor(std::size_t slot,
                                     std::uint64_t guest_vector);

    std::expected<void, zpp::error> enter_root_mode(std::size_t cpu);

    /**
     * The state an INIT leaves behind, plus the entry point a start-up
     * IPI names. Same default arguments as the real declaration - the
     * `first_launch` one is load bearing, so a call site that omits it
     * has to mean the same thing here as there.
     */
    void apply_start_up(arch::x86_64::context & context,
                        std::uint64_t vector,
                        const char * from = "?",
                        bool first_launch = false);

    /**
     * The other end of the hand-off mailbox: the INIT handler, which
     * publishes what this processor is waiting on and then waits on it.
     *
     * Compiled here rather than stood in for, because the ordering
     * *inside* it is the thing under test - f949649 is a reordering of
     * two writes against a wait, and no stand-in can be wrong in the same
     * way the original was.
     */
    void emulate_init_signal(arch::x86_64::context & context);

    /**
     * The intercepted write to the interrupt command register, which is
     * where a guest starting a processor is caught. Returns the command to
     * actually issue, or nothing when the write is swallowed.
     */
    std::optional<std::uint64_t>
    on_interrupt_command(std::uint64_t command);

    /**
     * Resolves a broadcast start-up IPI against the platform's roster.
     * Windows sends no other kind, so the guest phase of the firmware
     * -then-guest sequence goes through here.
     */
    bool start_up_broadcast(std::uint64_t vector);

    /**
     * One sender for both APIC modes. The harness records what it wrote
     * and in what order rather than letting it reach memory - see
     * shim/zpp/arch/x86_64/mmio.h.
     */
    void send_start_up_ipi(std::uint64_t apic, std::uint64_t vector);

    static bool x2apic_enabled();

    static std::uint64_t local_apic_id();

    // ------------------------------------------ defined by the harness
    std::uint32_t start_up_trampoline_stage() const;

    std::expected<void, zpp::error> enable_vmx_in_feature_control();

    // ------------------------------------------------------------ state
    arch::x86_64::vmx::vmcs vmcs{};
    page_table_stub host_page_table{};

    std::size_t number_of_known_processors = 1;
    std::uint64_t apic_id[max_cpus]{};

    bool processor_virtualized[max_cpus]{};
    bool started_by_trampoline[max_cpus]{};

    volatile std::uint64_t resume_activity_state[max_cpus]{};
    volatile std::uint64_t l2_activity_state[max_cpus]{};

    std::atomic<std::uint64_t> start_up_handoff[max_cpus]{};

    // Whether a start-up has already been applied to this processor.
    // `apply_start_up` reads it to ignore the *second* start-up IPI of an
    // INIT-SIPI-SIPI sequence, which would otherwise send a running
    // processor back to its entry point.
    bool started_by_start_up_ipi[max_cpus]{};
    std::atomic<bool> start_up_launched[max_cpus]{};

    // Written by `start_up_broadcast` and by `on_interrupt_command`, and
    // read by nothing - see the harness's note on c65f8f7, whose guard
    // this was for and which 95d9759 replaced with the activity state.
    bool started_by_guest_start_up_ipi[max_cpus]{};

    std::uint64_t guest_start_up_vector[max_cpus]{};
    std::uint64_t start_up_memory{};

    // The platform's roster, which is the only thing a broadcast - the
    // form that names no destination - can be resolved against.
    std::size_t number_of_platform_processors{};
    std::uint32_t platform_apic_id[max_cpus]{};

    // What the guest asked for, as counters, which is all the log has to
    // say about a command it decided not to act on.
    volatile std::uint64_t ipi_init_seen{};
    volatile std::uint64_t ipi_start_up_seen{};
    volatile std::uint64_t ipi_refused_shorthand{};
    volatile std::uint64_t ipi_refused_logical{};
    volatile std::uint64_t ipi_last_command{};

    std::uint64_t host_cr0{};
    std::uint64_t host_cr4{};

    alignas(page_size) arch::x86_64::vmx::vmx_vmcs vmx[max_cpus]{};
    alignas(page_size) arch::x86_64::vmx::vmx_vmcs vmx_vmcs[max_cpus]{};

    alignas(page_size) std::uint8_t start_up_stack[0x4000]{};

    spin_lock start_up_lock{};
};

/**
 * The hypervisor error category, so zpp::error can carry the codes above.
 */
inline const zpp::error_category & category(hypervisor::error)
{
    constexpr static auto error_category = zpp::make_error_category(
        "hypervisor",
        hypervisor::error::success,
        [](auto) -> std::string_view { return "hypervisor"; });
    return error_category;
}

} // namespace zpp::hypervisor
