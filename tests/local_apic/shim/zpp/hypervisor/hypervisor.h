#pragma once
// A stand-in for the real hypervisor.h, carrying only what the interrupt
// command register's decode path touches. Everything the extracted
// sources *define* is declared here with the same signature; everything
// they *call* that lives elsewhere in hypervisor.cpp is declared here and
// defined by the harness.
//
// A fifth shim rather than a widening of one of the four, for the reason
// each of those gives: a harness links one set of real definitions, and a
// class carrying every set would need every member of every path. This
// one is deliberately the smallest of them - the three functions under
// test decode a 64-bit register and consult two tables, and the point of
// the harness is that nothing else is in the way.
//
// zpp/diag/log.h, asm.h and vmx/asm.h come from tests/nested_vmx/shim,
// which is on the include path after this directory.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/error.h"
#include "zpp/scope_exit.h"
#include "zpp/spin_lock.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace zpp::hypervisor
{
/**
 * The log, reduced to a recorder the harness can inspect.
 *
 * Not discarded: what the decode *says* is half of what it does. A
 * start-up IPI refused for a logical destination has to be visible in
 * some channel or the processors it silently hands to the guest are
 * unaccounted for, and that channel is this one.
 */
struct log_record
{
    const char * format{};
};

std::size_t log_count();
const char * log_at(std::size_t index);
void log_reset();
void log_line(const char * format);

template <typename... Types>
struct log
{
    log(const char * format, Types &&...)
    {
        log_line(format);
    }
};

template <typename... Types>
log(const char *, Types &&...) -> log<Types...>;

class hypervisor
{
public:
    static constexpr std::size_t max_cpus = 32;
    static constexpr std::size_t page_size = 0x1000;

    /**
     * Which of the two mechanisms a processor's local APIC is using, and
     * the fourth value that says nothing has looked yet.
     */
    enum class apic_mode : std::uint8_t
    {
        unknown,
        disabled,
        xapic,
        x2apic,
    };

    /**
     * What a start-up attempt did. Same two values and the same meaning
     * as the real one: `adopted` means the guest's own write must not go
     * out, `needs_hardware` means it must.
     */
    enum class start_up_result
    {
        adopted,
        needs_hardware,
    };

    // === Under test: cut verbatim out of hypervisor.cpp ================
    std::optional<std::size_t> processor_slot(std::uint64_t apic_id);
    std::optional<std::uint64_t>
    on_interrupt_command(std::uint64_t command);
    bool start_up_broadcast(std::uint64_t vector);
    void note_apic_mode(std::size_t cpu);
    void intercept_interrupt_command(bool intercept);

    // === Supplied by the harness =======================================
    //
    // Each of these reaches hardware in the real VMM. Here they record
    // what they were asked for, which is what makes the decode's
    // decisions observable: "adopted" and "passed through" differ only in
    // whether a start-up IPI reached a processor.
    std::uint64_t local_apic_id();
    start_up_result start_up_processor(std::uint64_t destination,
                                       std::uint64_t vector);
    void send_start_up_ipi(std::uint64_t apic, std::uint64_t vector);

    /**
     * Deliberately *not* cut out of local_apic.cpp, though it lives
     * beside the two that are.
     *
     * It reaches the extended page tables through
     * `watch_guest_page_writes`, whose signature drags in the whole
     * `page_watch` vocabulary and a `guest_write`, and none of that is
     * what `note_apic_mode` is being asked about here: the question is
     * which of the two mechanisms it arms for a given roster of
     * processors, and a recorder answers that exactly. What
     * `watch_local_apic` decides on its own - refusing a relocated page
     * - is a separate harness's to make.
     */
    void watch_local_apic(bool watch);

    // === State the decode reads and writes =============================
    volatile std::uint64_t ipi_init_seen{};
    volatile std::uint64_t ipi_start_up_seen{};
    volatile std::uint64_t ipi_refused_shorthand{};
    volatile std::uint64_t ipi_refused_logical{};
    volatile std::uint64_t ipi_last_command{};

    bool started_by_guest_start_up_ipi[max_cpus]{};
    std::uint64_t apic_id[max_cpus]{};
    std::size_t number_of_known_processors = 1;

    std::uint32_t platform_apic_id[max_cpus]{};
    std::size_t number_of_platform_processors{};

    zpp::spin_lock start_up_lock{};

    alignas(page_size) std::uint8_t msr_bitmap[page_size]{};
    apic_mode observed_apic_mode[max_cpus]{};
    zpp::spin_lock apic_mode_lock{};

    // === Harness observation ===========================================
    //
    // Not in the real class. Everything below records what the functions
    // under test asked the machine to do.
    struct attempt
    {
        std::uint64_t destination{};
        std::uint64_t vector{};
    };

    attempt start_up_attempts[64]{};
    std::size_t start_up_attempt_count{};

    attempt hardware_ipis[64]{};
    std::size_t hardware_ipi_count{};

    /**
     * What `note_apic_mode` asked of the page watch, which is half of
     * what it decides - the other half is the bit in the MSR bitmap.
     */
    std::uint64_t watch_calls{};
    bool watch_last{};

    std::uint64_t self_apic_id{};

    /**
     * What start_up_processor should answer, per destination. Anything
     * not named here answers `adopted`.
     */
    std::uint64_t needs_hardware_for[64]{};
    std::size_t needs_hardware_count{};
};

/**
 * The hypervisor error category, so zpp::error can carry the codes above.
 */
inline const zpp::error_category & category(hypervisor::start_up_result)
{
    constexpr static auto error_category = zpp::make_error_category(
        "hypervisor",
        hypervisor::start_up_result::adopted,
        [](auto) -> std::string_view { return "hypervisor"; });
    return error_category;
}

} // namespace zpp::hypervisor
