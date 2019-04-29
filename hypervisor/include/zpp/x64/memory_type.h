#pragma once

namespace zpp::x64
{
/**
 * Memory Types.
 */
enum class memory_type
{
    uncachable = 0,
    write_combining = 1,
    write_through = 4,
    write_protected = 5,
    write_back = 6,
};

} // namespace zpp::x64
