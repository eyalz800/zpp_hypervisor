#pragma once
#include <cstdint>

namespace zpp::x64::intel
{
/**
 * Represents the VMX exit reason.
 */
class exit_reason
{
public:
    /**
     * The basic reason enumeration.
     */
    enum class basic_reason : int;

    /**
     * Construct an empty exit reason, value is unspecified.
     */
    constexpr exit_reason() = default;

    /**
     * Constructs an exit reason whose integral value is specified.
     */
    constexpr exit_reason(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * Returns the basic exit reason.
     */
    constexpr basic_reason basic() const
    {
        return basic_reason((m_value >> 0) & 0xff);
    }

    /**
     * Converts the exit reason to integral representation.
     */
    constexpr operator std::uint64_t() const
    {
        return m_value;
    }

    /**
     * Converts the exit reason to integral representation.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    /**
     * The integral representation of the exit reason.
     */
    std::uint64_t m_value{};
};

/**
 * Basic exit reasons.
 */
enum class exit_reason::basic_reason : int
{
    exception_or_nmi = 0,
    external_interrupt = 1,
    triple_fault = 2,
    init_signal = 3,
    start_up_ipi = 4,
    io_system_management_interrupt = 5,
    io_smi = 5,
    other_system_management_interrupt = 6,
    other_smi = 6,
    interrupt_window = 7,
    nmi_window = 8,
    task_switch = 9,
    cpuid = 10,
    getsec = 11,
    hlt = 12,
    invd = 13,
    invlpg = 14,
    rdpmc = 15,
    rdtsc = 16,
    rsm = 17,
    vmcall = 18,
    vmclear = 19,
    vmlaunch = 20,
    vmptrld = 21,
    vmptrst = 22,
    vmread = 23,
    vmresume = 24,
    vmwrite = 25,
    vmxoff = 26,
    vmxon = 27,
    control_register_access = 28,
    mov_debug_register = 29,
    mov_dr = 29,
    io_instruction = 30,
    rdmsr = 31,
    wrmsr = 32,
    entry_invalid_guest_state = 33,
    entry_failure_msr_loading = 34,
    mwait = 36,
    monitor_trap_flag = 37,
    monitor = 39,
    pause = 40,
    entry_failure_machine_check_event = 41,
    tpr_below_threshold = 43,
    apic_access = 44,
    virtualized_eoi = 45,
    gdtr_or_idtr = 46,
    ldtr_or_tr = 47,
    ept_violation = 48,
    ept_misconfiguration = 49,
    invept = 50,
    rdtscp = 51,
    vmx_preemption_timer = 52,
    invvpid = 53,
    wbinvd = 54,
    xsetbv = 55,
    apic_write = 56,
    rdrand = 57,
    invpcid = 58,
    vmfunc = 59,
    encls = 60,
    rdseed = 61,
    page_modification_log_full = 62,
    xsaves = 63,
    xrstors = 64,
    spp_related_event = 66,
};

} // namespace zpp::x64::intel
