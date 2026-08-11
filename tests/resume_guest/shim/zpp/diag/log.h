#pragma once
// Shim for the diagnostic side, carrying the four things the resume path
// asks of it: the severity a heartbeat is written at, the pump it turns
// once per resume, whether the block sink is present, and whether that
// sink is ready for another controller poll.
//
// A second copy rather than a share of tests/nested_vmx/shim's, which has
// the log and nothing else. Both switches are on here deliberately: with
// them off the resume path's diagnostic half is a discarded statement and
// this harness would pin nothing about it, and the two counters it moves
// are the cheapest evidence that a resume ran all the way to the entry.
#include <cstdint>

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

/**
 * The sinks, of which the resume path names one.
 */
enum class sink
{
    esp_blocks,
};

struct policy
{
    bool present{};
};

constexpr policy policy_of(sink)
{
    return {.present = true};
}

inline constexpr bool enabled = true;

template <severity Level = severity::info, typename... Types>
inline void log(const char *, Types &&...)
{
}

/**
 * The trickle out of the ring, counted so a test can say the resume path
 * reached the end rather than only that it wrote the right field.
 */
struct pump
{
    static void run();
};

/**
 * Whether the block sink wants the controller polled again.
 */
struct esp_block_sink
{
    static bool ready();
};

} // namespace zpp::diag
