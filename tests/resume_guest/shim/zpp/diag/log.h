#pragma once
// The log, reduced to a severity and a call that discards. The resume
// path's own lines are a heartbeat and say nothing about the decision
// under test, which is why this one discards where tests/local_apic's
// records.
#include "zpp/diag/config.h"

namespace zpp::diag
{
enum class severity
{
    trace,
    debug,
    info,
    warning,
    error,
};

template <severity Level = severity::info, typename... Types>
inline void log(const char *, Types &&...)
{
}

} // namespace zpp::diag
