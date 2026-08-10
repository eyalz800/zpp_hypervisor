#pragma once
#include <cstdint>

namespace zpp::diag
{
enum class severity
{
    debug,
    info,
    warning,
    error,
};

template <severity S = severity::info, typename... Types>
inline void log(const char *, Types &&...)
{
}

} // namespace zpp::diag
