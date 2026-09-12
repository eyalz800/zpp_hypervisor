#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace zpp::diag
{
/**
 * What a sink is.
 *
 * A struct with three static members and no state of its own:
 *
 *     struct my_sink
 *     {
 *         static constexpr sink id = sink::my_channel;
 *         static bool ready();
 *         static bool write(const record & entry);
 *     };
 *
 * `ready` says whether the channel can be used at all in this boot - a
 * framebuffer the firmware never reported, a UART that answered no probe.
 * `write` takes one record and returns whether it took it; false is a drop
 * and is counted, never retried indefinitely, and never waited on.
 *
 * Deliberately not a concept, not a base class, not an interface. This is
 * the whole contract, it is checked by the fold in pump.h simply by
 * calling it, and a sink that does not match fails to compile there. An
 * abstraction layer between the pump and the sinks would be somewhere a
 * record could be lost without anyone counting it.
 *
 * Three rules a sink implementation has to keep, and they are the hard
 * constraints of this environment rather than style:
 *
 * - `write` must not block. Every wait in it is bounded by a count, the
 * way zpp/trace.h's UART drain loop already is, and a wait that expires is
 * a refusal rather than a retry. A guest is executing while this runs.
 * - `write` must not take any lock another processor could hold across a
 *   VM exit. The gate below is the only permitted exclusion and it never
 *   waits.
 * - `write` must not fault. Whatever memory it touches - a device
 *   aperture, a framebuffer - is mapped into the host page table before
 *   `ready` first answers true, which is what screen::initialize already
 *   does on feat/screen-halt-output.
 */

/**
 * The list of sinks this program has. Each program provides its own
 * zpp/diag/sinks.h defining `sinks`, because a sink type can only exist
 * where its channel does: boot services live in the loader, the resident
 * side has none. Same include spelling in both, different file, no
 * platform conditionals anywhere.
 */
template <typename... Sinks>
struct sink_list
{
    static constexpr std::size_t size = sizeof...(Sinks);
};

/**
 * Whether the list contains an implementation of the given channel.
 *
 * Used for the one static_assert that makes the configuration
 * self-enforcing: enabling a sink in config.h with nothing to drive it is
 * a build failure rather than silence.
 * @{
 */
template <sink Which, typename... Sinks>
constexpr bool implemented(sink_list<Sinks...>)
{
    return ((Sinks::id == Which) || ... || false);
}
/**
 * @}
 */

/**
 * Non-blocking exclusion for a channel that is not per processor.
 *
 * A UART, a screen and a USB endpoint are single devices, and two
 * processors taking VM exits at once may both reach the pump. This is how
 * they are kept apart, and the important part is what it does *not* do: it
 * never spins, never waits and never retries. A processor that finds the
 * gate held leaves the records where they are - in the ring, with the
 * cursor unmoved - and the next pump on either processor picks them up.
 *
 * zpp::spin_lock is not usable here and the reason is worth stating: it
 * spins, and the processor holding it may be one that has stopped inside
 * an unhandled exit and is never going to release it. It is also not
 * recursive, so a host exception taken inside a sink would deadlock a
 * processor against itself. A single test_and_set with no retry has
 * neither failure mode.
 *
 * A template on the channel, so the flag exists only for sinks that are
 * compiled in.
 */
template <sink Which>
struct gate
{
    static inline std::atomic_flag held{};

    static bool enter()
    {
        return !held.test_and_set(std::memory_order_acquire);
    }

    static void leave()
    {
        held.clear(std::memory_order_release);
    }
};

/**
 * Where a sink is up to, and what it has missed.
 *
 * One cursor per channel, in a template so that it disappears with the
 * channel. Read by a debugger as much as by the pump: `lost`, `refused`
 * and `dead` between them say why a channel is quiet, which is otherwise
 * indistinguishable from nothing having happened.
 */
template <sink Which>
struct reader
{
    static inline cursor position{};
};

} // namespace zpp::diag
