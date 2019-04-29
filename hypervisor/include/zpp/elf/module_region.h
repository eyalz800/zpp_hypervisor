#pragma once
#include <cstdint>
#include <tuple>

namespace zpp::elf
{
/**
 * The module base type.
 */
enum module_base : std::uintptr_t
{
};

/**
 * The module size type.
 */
enum module_size : std::size_t
{
};

/**
 * Returns the module base and size of the current module.
 */
std::tuple<module_base, module_size> get_module_region();

} // namespace zpp::elf
