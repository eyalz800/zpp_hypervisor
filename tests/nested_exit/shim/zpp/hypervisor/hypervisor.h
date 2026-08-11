#pragma once
// A stand-in for the real hypervisor.h, carrying only what
// nested_entry.cpp touches. Everything nested_entry.cpp *defines* is
// declared here with the same signature; everything it *calls* that lives
// elsewhere is declared here and defined by the harness.
//
// Deliberately a second shim rather than a widening of
// tests/nested_vmx/shim's: that one exists so nested_vmx.cpp links against
// a hypervisor with no build_vmcs02, and this one needs the real
// build_vmcs02 out of nested_entry.cpp. The two would collide in one
// binary. Everything else - asm.h, vmx/asm.h, diag/log.h - is shared, by
// putting this directory ahead of that one on the include path.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/arch/x86_64/vmx/msr.h"
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmcs12.h"
#include "zpp/arch/x86_64/vmx/vmx.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/error.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace zpp::hypervisor
{
/**
 * The log, reduced to a recorder the harness can inspect.
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
        guest_address_not_mapped = 16,
        guest_memory_unreachable = 17,
        vmptrld_failed = 18,
        nested_host_state_unsupported = 22,
        nested_controls_unsupported = 23,
        nested_msr_area_unsupported = 24,
    };

    static constexpr std::size_t max_cpus = 32;
    static constexpr std::size_t page_size = 0x1000;
    static constexpr bool execute_only_translations_offered = false;

    /**
     * What the exit handler does next with an exit a second-level guest
     * took. Same three outcomes as the real header.
     */
    enum class l2_exit_outcome
    {
        reflected,
        handled,
        deferred,
    };

    /**
     * Same three outcomes as the real header.
     */
    enum class l2_entry_outcome
    {
        entered,
        reflected,
        retry,
    };

    /**
     * The host page table, reduced to the one query merge_nested_bitmaps
     * makes of it. The harness maps virtual to physical identically.
     */
    struct page_table_stub
    {
        std::uint64_t virtual_to_physical(const void * address) const
        {
            return reinterpret_cast<std::uint64_t>(address);
        }
    };

    static hypervisor & instance();

    // Defined in nested_entry.cpp - the code under test.
    std::expected<void, zpp::error> check_nested_msr_area(
        std::uint64_t address, std::uint64_t count, bool loading);
    std::expected<void, zpp::error> load_nested_msrs(std::size_t cpu,
                                                     std::uint64_t address,
                                                     std::uint64_t count);
    std::expected<void, zpp::error>
    store_nested_msrs(std::uint64_t address, std::uint64_t count);
    bool own_msr_intercepted(std::uint32_t index, bool write) const;
    bool own_io_port_intercepted(std::uint16_t port) const;
    std::expected<void, zpp::error> merge_nested_bitmaps(std::size_t cpu);
    std::expected<void, zpp::error> build_vmcs02(std::size_t cpu);
    l2_entry_outcome enter_or_park_l2(std::size_t cpu);
    void nested_transition_flush();
    bool l0_wants_l2_exit(std::size_t cpu,
                          arch::x86_64::vmx::exit_reason reason,
                          const arch::x86_64::context & context);
    bool l1_wants_l2_exit(std::size_t cpu,
                          arch::x86_64::vmx::exit_reason reason,
                          const arch::x86_64::context & context);
    void save_l2_state(std::size_t cpu);
    void load_l1_host_state(std::size_t cpu);
    void reflect_l2_exit(std::size_t cpu,
                         arch::x86_64::vmx::exit_reason reason,
                         std::uint64_t qualification);
    l2_exit_outcome on_l2_ept_fault(std::size_t cpu,
                                    arch::x86_64::vmx::exit_reason reason,
                                    arch::x86_64::context & context,
                                    bool & advance_rip);
    l2_exit_outcome on_l2_exit(std::size_t cpu,
                               arch::x86_64::vmx::exit_reason reason,
                               arch::x86_64::context & context,
                               bool & advance_rip);

    // Defined by the harness.
    std::optional<std::uint64_t> wait_for_l2_start_up_ipi(std::size_t cpu);
    std::uint64_t cached_vmx_msr(std::size_t msr);
    std::uint64_t nested_vmx_capability_msr(std::size_t msr);
    std::uint64_t physical_address_bits();
    std::uint64_t guest_register(const arch::x86_64::context & context,
                                 std::uint64_t encoding);
    std::expected<void, zpp::error>
    read_guest_physical(std::uint64_t physical, std::span<std::byte> into);
    std::expected<void, zpp::error> write_guest_physical(
        std::uint64_t physical, std::span<const std::byte> from);
    std::expected<std::uint64_t, zpp::error>
    shadow_ept_pointer_for(std::size_t cpu, std::uint64_t eptp12);
    std::expected<void, zpp::error>
    fill_shadow_leaf(std::size_t cpu,
                     std::uint64_t guest_physical,
                     const arch::x86_64::vmx::ept_walk_result & guest,
                     std::uint64_t shift);
    arch::x86_64::vmx::ept_walk_result
    host_ept_lookup(std::uint64_t physical_address);
    void record_exit(arch::x86_64::vmx::exit_reason reason,
                     const arch::x86_64::context & context);
    std::uint64_t own_vmcs_region_physical();
    bool on_ept_violation(std::size_t cpu,
                          arch::x86_64::context & context,
                          std::uint64_t guest_physical);
    void invalidate_ept_locally();
    [[noreturn]] void
    on_unhandled_exit(arch::x86_64::vmx::exit_reason reason);

    // State.
    arch::x86_64::vmx::vmcs vmcs{};
    page_table_stub host_page_table{};

    alignas(page_size) std::uint8_t msr_bitmap[page_size]{};
    alignas(page_size) std::uint8_t io_bitmap_a[page_size]{};
    alignas(page_size) std::uint8_t io_bitmap_b[page_size]{};
    alignas(page_size) std::uint8_t
        nested_msr_bitmap[max_cpus][page_size]{};
    alignas(page_size) std::uint8_t
        nested_io_bitmap[max_cpus][2 * page_size]{};
    std::uint64_t nested_msr_bitmap_physical[max_cpus]{};
    std::uint64_t nested_io_bitmap_physical[max_cpus]{};

    arch::x86_64::vmx::vmcs12 guest_vmcs12[max_cpus]{};
    bool running_l2[max_cpus]{};

    // Instrumentation the real source writes to. Present here so the
    // harness compiles the real reflection path unmodified.
    static constexpr std::size_t exit_trace_capacity = 32;
    struct exit_trace_entry
    {
        std::uint64_t reason;
        std::uint64_t qualification;
        std::uint64_t activity_state;
        std::uint64_t cs_selector;
        std::uint64_t rip;
        std::uint64_t guest_physical;
        std::uint64_t repeated;
        std::uint64_t detail;
    };
    static constexpr std::size_t l2_exit_trace_capacity = 256;
    exit_trace_entry l2_exit_trace[max_cpus][l2_exit_trace_capacity]{};
    std::uint64_t l2_exit_trace_count[max_cpus]{};
    std::uint64_t l2_exit_detail[max_cpus]{};
    std::uint64_t pending_event[max_cpus]{};

    // The reference-counter and synthetic-timer sampling the diagnostic
    // side of nested_entry.cpp writes into. Nothing here reads them; they
    // exist so the reflection path compiles against the same source the
    // hypervisor does.
    //
    // Added after run-host-tests.sh was written and this harness turned
    // out not to build any more: the members arrived with the diagnostic
    // commits and nothing was running the harness to notice. That is the
    // whole reason the runner exists, and this is its first catch.
    static constexpr std::size_t reference_sample_capacity = 32;
    std::uint64_t reference_read_value[max_cpus]
                                      [reference_sample_capacity]{};
    std::uint64_t reference_read_tsc[max_cpus]
                                    [reference_sample_capacity]{};
    std::uint64_t reference_read_count[max_cpus]{};
    std::uint64_t stimer_arm_value[max_cpus][reference_sample_capacity]{};
    std::uint64_t stimer_arm_tsc[max_cpus][reference_sample_capacity]{};
    std::uint64_t stimer_arm_kind[max_cpus][reference_sample_capacity]{};
    std::uint64_t stimer_arm_count[max_cpus]{};
    bool reference_read_pending[max_cpus]{};

    // The synthetic-MSR histogram nested_entry.cpp writes into. Added
    // after a rebase onto a develop that had grown it - the second time
    // run-host-tests.sh has caught this shim drifting, which is the
    // whole argument for the runner existing.
    static constexpr std::size_t synthetic_msr_capacity = 256;
    std::uint64_t synthetic_msr_reads[max_cpus][synthetic_msr_capacity]{};
    std::uint64_t synthetic_msr_writes[max_cpus][synthetic_msr_capacity]{};
    std::uint64_t synthetic_msr_last_value[max_cpus]
                                          [synthetic_msr_capacity]{};
    std::uint64_t synthetic_msr_last_write_tsc[max_cpus]
                                              [synthetic_msr_capacity]{};

    // VMCS shadowing does nothing here: this harness has no processor and
    // no shadow region, so publishing to one is a no-op that keeps the
    // reflection path compiling unchanged.
    void copy_vmcs12_to_shadow(std::size_t)
    {
    }
    void copy_shadow_to_vmcs12(std::size_t)
    {
    }
    bool vmcs02_launched[max_cpus]{};
    std::uint64_t vmcs02_physical[max_cpus]{};
    std::uint64_t l2_entries[max_cpus]{};
    std::uint64_t l2_exits_reflected[max_cpus]{};
    std::uint64_t l2_exits_handled[max_cpus]{};
    std::uint64_t shadow_ept_leaves_filled[max_cpus]{};
    bool stepping_watch[max_cpus]{};
    arch::x86_64::context nested_entry_recovery[max_cpus]{};
    std::atomic<bool> nested_entry_failed[max_cpus]{};
    bool nested_msr_load_failed[max_cpus]{};
    std::uint64_t nested_msr_failure_entry[max_cpus]{};
    volatile std::uint64_t l2_activity_state[max_cpus]{};
    volatile std::uint64_t l2_start_up_waits[max_cpus]{};

    volatile std::uint64_t vmcs12_controls_captured{};
    volatile std::uint64_t vmcs12_pin_controls{};
    volatile std::uint64_t vmcs12_primary_controls{};
    volatile std::uint64_t vmcs12_secondary_controls{};
    volatile std::uint64_t vmcs12_exit_controls{};
    volatile std::uint64_t vmcs12_entry_controls{};

    std::uint64_t epml4_physical{};

    // Harness-only observation.
    std::uint64_t reflections{};
    arch::x86_64::vmx::exit_reason last_reflected_reason{};
    std::uint64_t last_reflected_qualification{};
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
