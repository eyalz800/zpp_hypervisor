#pragma once
#include <cstdint>

namespace zpp::x64::intel
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
 * The VM entry controls.
 */
namespace vm_entry_controls
{
enum type : std::uint64_t
{
    ia_32e_mode_guest = (1ull << 9),
};
} // namespace vm_entry_controls

} // namespace zpp::x64::intel