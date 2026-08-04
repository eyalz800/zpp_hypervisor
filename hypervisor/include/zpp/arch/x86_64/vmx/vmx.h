#pragma once
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * The VMX VMCS region structure.
 */
struct vmx_vmcs
{
    std::uint32_t revision_id{};
    std::uint32_t abort_indicator{};
    std::uint8_t data[0x1000 - (sizeof(std::uint32_t) * 2)]{};
};

/**
 * The primary execution controls.
 */
namespace vm_execution_controls::primary
{
enum type : std::uint64_t
{
    enable_msr_bitmaps = (1ull << 28),
    enable_secondary_controls = (1ull << 31),
};
} // namespace vm_execution_controls::primary

/**
 * The secondary execution controls.
 */
namespace vm_execution_controls::secondary
{
enum type : std::uint64_t
{
    enable_ept = (1ull << 1),
    enable_rdtscp = (1ull << 3),
    enable_vpid = (1ull << 5),
    // Without this a guest may not run with CR0.PE or CR0.PG clear, and
    // an application processor coming out of a start-up IPI does exactly
    // that: it begins in real mode.
    unrestricted_guest = (1ull << 7),
    enable_invpcid = (1ull << 12),
    enable_xsaves_xrstors = (1ull << 20),
    mode_based_execute_control = (1ull << 22),
};
} // namespace vm_execution_controls::secondary

/**
 * The VM exit controls.
 */
namespace vm_exit_controls
{
enum type : std::uint64_t
{
    host_address_space_size = (1ull << 9),
};
} // namespace vm_exit_controls

/**
 * Guest activity states, as held in the guest activity state field.
 *
 * wait_for_start_up_ipi is the one that matters here: it is the state an
 * application processor is left in after an INIT, and the only state from
 * which a start-up IPI will start it.
 */
namespace activity_state
{
enum type : std::uint64_t
{
    active = 0,
    hlt = 1,
    shutdown = 2,
    wait_for_start_up_ipi = 3,
};
} // namespace activity_state

/**
 * The VM entry interruption information field, which is how the VMM asks
 * the processor to deliver an event to the guest on the next entry.
 */
namespace vm_entry_interruption
{
enum type : std::uint64_t
{
    /**
     * The event type, in bits 10:8. Hardware exception is the kind a
     * faulting instruction would have raised on its own.
     */
    hardware_exception = (3ull << 8),

    /**
     * Set when the vector pushes an error code, which the processor takes
     * from the VM entry exception error code field.
     */
    deliver_error_code = (1ull << 11),

    /**
     * Set to make the field mean anything at all. Cleared by the processor
     * once the event has been delivered.
     */
    valid = (1ull << 31),
};
} // namespace vm_entry_interruption

/**
 * The VM entry controls.
 */
namespace vm_entry_controls
{
enum type : std::uint64_t
{
    ia_32e_mode_guest = (1ull << 9),
};
} // namespace vm_entry_controls

} // namespace zpp::arch::x86_64::vmx