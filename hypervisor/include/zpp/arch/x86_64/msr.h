#pragma once
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * MSR addresses.
 */
namespace msr
{
enum type : std::size_t
{
    ia32_extended_feature_enable = 0xc0000080,
    ia32_feature_control = 0x3a,
    ia32_mtrr_capability = 0xfe,
    ia32_debug_control = 0x1d9,
    ia32_fs_base = 0xC0000100,
    ia32_gs_base = 0xC0000101,
};
} // namespace msr

/**
 * MTRR msr addresses.
 */
namespace msr::mtrr
{
enum type : std::size_t
{
    physbase_0 = 0x200,
    physmask_0,
    physbase_1,
    physmask_1,
    physbase_2,
    physmask_2,
    physbase_3,
    physmask_3,
    physbase_4,
    physmask_4,
    physbase_5,
    physmask_5,
    physbase_6,
    physmask_6,
    physbase_7,
    physmask_7,
};
} // namespace msr::mtrr

} // namespace zpp::arch::x86_64