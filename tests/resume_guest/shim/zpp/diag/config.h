#pragma once
// The diagnostic configuration, reduced to the three things the resume
// path asks of it: whether the facility exists at all, which sinks there
// are, and what the policy for one of them says.
//
// Split across the same four headers the real facility uses -
// zpp/diag/config.h, log.h, pump.h and sinks/esp_blocks.h - rather than
// gathered into one, because hypervisor/src/hypervisor/resume.cpp is
// compiled here as itself and includes each of them by name. A shim that
// answered a different set of file names would work only for as long as
// nothing under test included a header directly.
//
// Both switches are on deliberately: with them off the resume path's
// diagnostic half is a discarded statement and this harness would pin
// nothing about it, and the two counters it moves are the cheapest
// evidence that a resume ran all the way to the entry.
#include <cstdint>

namespace zpp::diag
{
inline constexpr bool enabled = true;

/**
 * The sinks, of which the resume path names one.
 */
enum class sink : std::uint8_t
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

} // namespace zpp::diag
