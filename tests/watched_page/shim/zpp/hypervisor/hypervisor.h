#pragma once
// A stand-in for the real hypervisor.h, carrying only what the watched
// page write path touches. Everything the extracted sources *define* is
// declared here with the same signature; everything they *call* that
// lives elsewhere in hypervisor.cpp is declared here and defined by the
// harness.
//
// A third shim rather than a widening of the other two, for the same
// reason nested_exit's exists: each harness links one set of real
// definitions, and a class carrying all three sets would need every
// member of every path. Everything else - asm.h, vmx/asm.h, diag/log.h -
// is shared, by putting this directory ahead of tests/nested_vmx/shim on
// the include path.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/instruction.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/vmx/ept.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/error.h"
#include "zpp/spin_lock.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
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
        out_of_ept_entries = 5,
    };

    static constexpr std::size_t max_cpus = 32;
    static constexpr std::size_t page_size = 0x1000;

    /**
     * The host page table, reduced to the one query the write path makes
     * of it: whether this VMM can reach the address at all. The harness
     * answers from the fake guest memory it owns, so a "not mapped" case
     * can be produced without unmapping anything.
     */
    struct page_table_stub
    {
        std::uint64_t virtual_to_physical(const void * address) const;
    };

    static hypervisor & instance();

    struct guest_write
    {
        std::uint64_t address{};
        std::uint64_t value{};
        std::uint8_t size{};
    };

    struct page_watch
    {
        using handler = void (*)(void * context,
                                 std::uint64_t page,
                                 const guest_write * write);

        enum class mode
        {
            notify,
            hold,
        };

        std::uint64_t page{};
        handler on_write{};
        void (*before_write)(void * context, std::uint64_t page){};

        using filter = std::optional<std::uint64_t> (*)(
            void * context, std::uint64_t page, const guest_write * write);

        filter filter_write{};

        void * context{};
        mode behaviour{mode::notify};
        bool armed{};
        std::atomic<bool> held{};
    };

    static constexpr bool emulate_watched_page_writes = true;

    // ------------------------------------------- the code under test
    std::expected<void, zpp::error> watch_guest_page_writes(
        std::uint64_t guest_physical,
        page_watch::handler on_write,
        void * context,
        page_watch::mode behaviour = page_watch::mode::notify,
        void (*before_write)(void * context, std::uint64_t page) = nullptr,
        page_watch::filter filter_write = nullptr);

    void unwatch_guest_page(std::uint64_t guest_physical);

    std::optional<std::uint64_t>
    read_guest_word(std::uint64_t guest_physical, std::uint8_t size);

    bool apply_guest_store(std::uint64_t guest_physical,
                           const arch::x86_64::memory_store & store);

    bool carry_out_guest_instruction(
        std::uint64_t guest_physical,
        const arch::x86_64::decoded_instruction & instruction,
        arch::x86_64::context & context,
        guest_write & performed,
        bool & changed_memory,
        std::optional<std::uint64_t> known_contents = {});

    bool on_ept_violation(std::size_t cpu,
                          arch::x86_64::context & context,
                          std::uint64_t guest_physical);

    bool on_monitor_trap_flag(std::size_t cpu);

    static void on_local_apic_write(void * context,
                                    std::uint64_t page,
                                    const guest_write * write);

    static std::optional<std::uint64_t> filter_local_apic_write(
        void * context, std::uint64_t page, const guest_write * write);

    // ------------------------------------------ defined by the harness
    std::optional<arch::x86_64::decoded_instruction>
    decode_guest_instruction(std::size_t cpu,
                             arch::x86_64::context & context);

    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    epte_for(std::uint64_t physical_address);

    void invalidate_ept();
    void monitor_trap_flag(bool value);
    std::optional<std::uint64_t>
    on_interrupt_command(std::uint64_t command);

    // ------------------------------------------------------------ state
    arch::x86_64::vmx::vmcs vmcs{};
    page_table_stub host_page_table{};

    static constexpr std::size_t watch_capacity = 8;
    page_watch watches[watch_capacity]{};

    bool stepping_watch[max_cpus]{};
    std::uint64_t stepping_page[max_cpus]{};
    std::uint64_t stepping_offset[max_cpus]{};

    struct watched_access
    {
        std::uint64_t page{};
        std::uint64_t offset{};
    };

    static constexpr std::size_t watched_access_capacity = 24;
    watched_access watched_accesses[watched_access_capacity]{};
    volatile std::uint64_t watched_access_count{};

    static constexpr std::size_t emulated_trace_capacity = 32;

    struct emulated_trace_entry
    {
        std::uint8_t code[8]{};
        std::uint64_t page{};
        std::uint64_t old_value{};
        std::uint64_t new_value{};
        std::uint32_t what{};
        std::uint32_t how{};
        std::uint32_t size{};
        std::uint32_t wrote{};
    };

    emulated_trace_entry emulated_trace[emulated_trace_capacity]{};
    volatile std::uint64_t emulated_trace_count{};
    std::uint8_t last_fetched_code[8]{};

    volatile std::uint64_t emulated_writes{};
    volatile std::uint64_t filtered_writes{};
    volatile std::uint64_t stepped_writes{};
    volatile std::uint64_t emulated_length_disagreement{};
    volatile std::uint64_t emulated_length_reported{};
    volatile std::uint64_t emulated_length_decoded{};
    volatile std::uint64_t access_offset_decoded{};
    volatile std::uint64_t access_offset_unknown{};
    volatile std::uint64_t physical_offset_present{};
    volatile std::uint64_t physical_offset_agreed{};
    volatile std::uint64_t physical_offset_disagreed{};
    volatile std::uint64_t apic_page_commands_filtered{};
    volatile std::uint64_t apic_writes_undecoded{};
    std::uint64_t module_access_count{};

    std::map<std::uint64_t, std::uint64_t> module_physical_to_virtual{};

    struct
    {
        alignas(page_size) std::uint8_t decoy_page[page_size]{};
    } unprotected_memory{};
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
