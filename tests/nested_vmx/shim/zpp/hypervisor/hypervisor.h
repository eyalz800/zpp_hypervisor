#pragma once
// A stand-in for the real hypervisor.h, carrying only what
// nested_vmx.cpp touches. Everything nested_vmx.cpp *defines* is declared
// here with the same signature; everything it *calls* that lives
// elsewhere is declared here and defined by the harness.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/vmx/msr.h"
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
        nested_host_state_unsupported = 22,
        nested_control_unsupported = 23,
    };

    static constexpr std::size_t max_cpus = 32;
    static constexpr std::size_t page_size = 0x1000;
    static constexpr std::size_t capability_answer_capacity = 48;

    struct capability_answer
    {
        std::uint32_t msr{};
        std::uint64_t value{};
    };

    /**
     * Same three outcomes as the real header. The decision itself lives in
     * nested_entry.cpp, which this binary does not compile, so the harness
     * answers it - see enter_or_park_l2_outcome below.
     */
    enum class l2_entry_outcome
    {
        entered,
        reflected,
        retry,
    };

    static hypervisor & instance();

    // Defined in nested_vmx.cpp - the code under test.
    void intercept_msr(std::uint32_t index, bool read, bool write);
    std::uint64_t nested_vmx_capability_msr(std::size_t msr);
    bool on_nested_vmx_msr_read(std::uint32_t index,
                                arch::x86_64::context & context);
    bool on_nested_vmx_msr_write(std::uint32_t index,
                                 arch::x86_64::context & context);
    bool on_vmx_instruction(arch::x86_64::vmx::exit_reason reason,
                            arch::x86_64::context & context);
    void vmx_succeed();
    void vmx_fail_invalid();
    void vmx_fail_valid(std::size_t cpu,
                        nested_vmx::instruction_error error);
    void vmx_fail(std::size_t cpu, nested_vmx::instruction_error error);
    std::expected<std::uint64_t, zpp::error>
    vmx_operand_linear_address(const arch::x86_64::context & context);
    std::uint64_t guest_segment_base(std::uint64_t segment);
    std::uint64_t guest_register(const arch::x86_64::context & context,
                                 std::uint64_t encoding);
    void set_guest_register(arch::x86_64::context & context,
                            std::uint64_t encoding,
                            std::uint64_t value);
    std::expected<void, zpp::error>
    read_guest_linear(std::uint64_t linear, std::span<std::byte> into);
    std::expected<void, zpp::error>
    write_guest_linear(std::uint64_t linear,
                       std::span<const std::byte> from);
    std::expected<std::uint64_t, zpp::error>
    read_guest_vmcs_pointer(const arch::x86_64::context & context);
    bool vmcs_pointer_valid(std::uint64_t pointer);
    bool on_guest_vmxon(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_vmxoff(std::size_t cpu);
    void flush_guest_vmcs12(std::size_t cpu);
    bool on_guest_vmclear(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmptrld(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmptrst(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmread(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_vmwrite(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_invept(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_invvpid(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmlaunch(
        std::size_t cpu,
        arch::x86_64::vmx::exit_reason::basic_reason reason);
    void on_nested_entry_failure(arch::x86_64::context * recovery);

    // Defined by the harness.
    std::uint64_t cached_vmx_msr(std::size_t msr);
    void inject_general_protection_fault();
    std::expected<std::uint64_t, zpp::error>
    guest_linear_to_physical(std::uint64_t linear);
    std::expected<void, zpp::error>
    read_guest_physical(std::uint64_t physical, std::span<std::byte> into);
    std::expected<void, zpp::error>
    write_guest_physical(std::uint64_t physical,
                         std::span<const std::byte> from);
    void discard_shadow_ept(std::size_t cpu);
    void discard_shadow_ept_for(std::size_t cpu, std::uint64_t root);
    void nested_transition_flush();
    std::expected<void, zpp::error> build_vmcs02(std::size_t cpu);
    l2_entry_outcome enter_or_park_l2(std::size_t cpu);
    void reflect_l2_exit(std::size_t cpu,
                         arch::x86_64::vmx::exit_reason reason,
                         std::uint64_t qualification);
    std::uint64_t own_vmcs_region_physical();

    // State.
    arch::x86_64::vmx::vmcs vmcs{};

    alignas(page_size) std::uint8_t msr_bitmap[page_size]{};

    volatile std::uint64_t nested_vmfail_count[max_cpus]{};
    volatile std::uint64_t nested_last_vmfail[max_cpus]{};
    volatile std::uint64_t nested_capability_reads{};
    volatile std::uint64_t nested_capability_last_msr{};
    capability_answer capability_answers[capability_answer_capacity]{};

    bool guest_in_vmx_operation[max_cpus]{};
    volatile std::uint64_t guest_vmxon_count[max_cpus]{};
    volatile std::uint64_t guest_vmxoff_count[max_cpus]{};
    std::uint64_t guest_vmxon_pointer[max_cpus]{};
    std::uint64_t guest_current_vmcs[max_cpus]{};
    arch::x86_64::vmx::vmcs12 guest_vmcs12[max_cpus]{};
    bool running_l2[max_cpus]{};
    bool l2_entry_logged[max_cpus]{};
    std::uint64_t l2_entries[max_cpus]{};
    arch::x86_64::context nested_entry_recovery[max_cpus]{};
    std::atomic<bool> nested_entry_failed[max_cpus]{};
    std::uint64_t nested_entry_error[max_cpus]{};
    bool nested_rip_settled[max_cpus]{};
    bool nested_msr_load_failed[max_cpus]{};
    std::uint64_t nested_msr_failure_entry[max_cpus]{};
    std::uint64_t guest_feature_control[max_cpus]{};

    // Harness-only observation.
    std::uint64_t gp_faults{};
    std::uint64_t flushes{};
    std::uint64_t ept_discards{};
    std::uint64_t ept_discards_for{};
    std::uint64_t last_discard_root{};
    bool build_vmcs02_fails{};
    l2_entry_outcome enter_or_park_l2_outcome{l2_entry_outcome::entered};
    std::uint64_t enter_or_park_l2_calls{};
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
