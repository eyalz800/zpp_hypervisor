#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/ring.h"
#include "zpp/diag/sink.h"
#include "zpp/diag/sinks.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace zpp::diag
{
/**
 * What moves records out of the ring and into the sinks.
 *
 * There is no thread and no scheduler in this program, so this has to be
 * called from somewhere that runs anyway. There are exactly three such
 * places, and all three are used for different reasons:
 *
 * - **The VM exit handler**, bounded to a few records per exit. This is
 * the only periodic execution the resident hypervisor has, and a guest
 *   produces exits constantly - CPUID, MSR accesses, EPT violations, and
 * on an idle Windows the intercepted HLT and MWAIT. Bounded, because the
 *   guest is waiting: a loop that drains the ring would put a device's
 *   latency into a VM exit.
 * - **The halt paths**, unbounded - `drain()`. A processor stopping inside
 *   on_unhandled_exit or on_vm_entry_failure has nothing left to be late
 *   for, and this is the case that matters most: it is what puts the log
 * on the screen before the machine has to be powered off. It is also the
 *   answer to "what if the pump never ran" - a failure always ends here.
 * - **The loader**, before it hands over and after it comes back, where
 *   boot services still exist and can write a file.
 *
 * If a guest that takes no exits at all ever becomes a real case, the VMX
 * preemption timer is the tool: it forces an exit after a set number of
 * ticks, which is a timer for a program that has none. Not built - the
 * exits are there today and an unmeasured timer would be a second thing to
 * debug.
 *
 * What the pump never does: wait for a sink, retry a sink indefinitely, or
 * hold anything across a VM entry.
 */
struct pump
{
    /**
     * How many records one pass offers a sink, per processor, when called
     * from a VM exit. Small on purpose: the point is a steady trickle that
     * keeps up with a guest, not a flush.
     */
    static constexpr std::size_t exit_budget = 4;

    /**
     * Offers up to `budget` records per processor to every compiled sink.
     * Called from the exit path.
     */
    static void run(std::size_t budget = exit_budget)
    {
        if constexpr (!enabled) {
            static_cast<void>(budget);
            return;
        } else {
            offer_each(sinks{}, budget);
        }
    }

    /**
     * Empties the ring into every sink. Called from the halt paths only.
     * Bounded by the ring's own size, which is the largest amount of work
     * this can ever be.
     */
    static void drain()
    {
        run(ring.records);
    }

    /**
     * Releases every sink's gate, whoever was holding it.
     *
     * For a processor coming back from a power transition, and the failure
     * this avoids is quiet rather than loud - which is what makes it worth
     * having. A gate never spins: a processor that finds one held leaves
     * its records in the ring and moves on. So a gate left held by a
     * processor the platform reset does not hang anything; it makes the
     * channel silent, for the rest of the boot, on the one path whose
     * whole purpose is to report what happened. A resume that worked and a
     * resume that hung would look the same from outside.
     *
     * Safe only while the caller is the only processor running. Unlike the
     * heap's, this one leaves nothing half updated: a gate guards the
     * pump's cursor advance, and a cursor that was mid-advance simply
     * re-offers or skips a record.
     */
    static void abandon_gates()
    {
        if constexpr (enabled) {
            release_each(sinks{});
        }
    }

private:
    template <typename... Sinks>
    static void release_each(sink_list<Sinks...>)
    {
        (release_one<Sinks>(), ...);
    }

    template <typename Sink>
    static void release_one()
    {
        if constexpr (policy_of(Sink::id).present) {
            gate<Sink::id>::leave();
        }
    }

    /**
     * The fold. One `if constexpr` per sink, so a disabled sink is not a
     * branch that is never taken - it is not there.
     *
     * A compile-time list rather than an array of function pointers, and
     * the reasons are all things this tree already cares about: a pointer
     * table is storage that has to be initialised, every entry in it is a
     * relocation in .data.rel.ro - the section whose relocations were
     * silently wrong for months, per CLAUDE.md - and an indirect call
     * cannot be inlined or eliminated. The fold has none of those, and a
     * disabled sink leaves nothing at all rather than a null slot. It
     * costs code size at each call, which is why there is exactly one
     * call: here.
     */
    template <typename... Sinks>
    static void offer_each(sink_list<Sinks...>, std::size_t budget)
    {
        (offer_one<Sinks>(budget), ...);
    }

    template <typename Sink>
    static void offer_one(std::size_t budget)
    {
        constexpr auto rules = policy_of(Sink::id);

        if constexpr (!rules.present) {
            static_cast<void>(budget);
            return;
        } else {
            auto & position = reader<Sink::id>::position;
            if (position.dead) {
                return;
            }

            // A channel is one device even where the ring is per
            // processor. Whichever processor gets in does the work; the
            // other one leaves the records in the ring and moves on. No
            // waiting, so no processor is ever held up by a sink.
            if (!gate<Sink::id>::enter()) {
                return;
            }

            if (!Sink::ready()) {
                gate<Sink::id>::leave();
                return;
            }

            for (std::size_t processor{}; processor < ring.processors;
                 ++processor) {
                for (std::size_t taken{}; taken < budget; ++taken) {
                    // A sink that cannot take a record right now.
                    //
                    // Optional, and detected rather than required, like
                    // flush_if_due below. Asked *before* the cursor
                    // advances, which is the whole point: the record stays
                    // in the ring and is offered again next pass, rather
                    // than being handed over and dropped. The ring is the
                    // buffer for exactly this, and when it does run out
                    // its own `lost` counter says so - which reaches the
                    // reader in band, unlike a counter of a sink's own.
                    //
                    // The return of write() cannot express this: it means
                    // refused, and four refusals in a row take the sink
                    // off the pump for the rest of the boot.
                    if constexpr (requires { Sink::accepting(); }) {
                        if (!Sink::accepting()) {
                            break;
                        }
                    }

                    record entry{};
                    if (!ring_reader::next(position, processor, entry)) {
                        break;
                    }

                    // Reader side filtering. The ring retains the union of
                    // what every enabled sink asked for, and each sink
                    // takes its own share out of it. A record this sink
                    // does not want costs it a cursor advance and nothing
                    // else - it is skipped, not lost.
                    if (rules.shape != entry.shape) {
                        continue;
                    }
                    if (!at_least(entry.level, rules.floor)) {
                        continue;
                    }
                    if (0 == (rules.categories & only(entry.which))) {
                        continue;
                    }

                    if (Sink::write(entry)) {
                        ++position.delivered;
                        position.failures = 0;
                        continue;
                    }

                    // Refused. Counted, and the record is not offered
                    // again: a channel that cannot keep up must not be
                    // able to stall the stream behind it.
                    ++position.refused;
                    ++position.failures;
                    if ((0 != rules.failures_tolerated) &&
                        (position.failures > rules.failures_tolerated)) {
                        // Wedged. Stop offering it anything for the rest
                        // of the boot rather than paying for its failure
                        // out of every VM exit from now on.
                        position.dead = true;
                        gate<Sink::id>::leave();
                        return;
                    }
                }
            }

            // Give the sink a chance to act on time having passed
            // rather than on a record having arrived.
            //
            // Optional, and detected rather than required, so a sink
            // that has nothing to do with time does not have to say so.
            // The disk sink uses it to bound how long a partly filled
            // block may wait: a block is written when it fills, and a
            // guest that has gone quiet would otherwise leave the last
            // one unwritten indefinitely - which is precisely the case
            // the log exists for.
            if constexpr (requires { Sink::flush_if_due(); }) {
                Sink::flush_if_due();
            }

            gate<Sink::id>::leave();
        }
    }
};

/**
 * Enabling a channel in config.h with nothing in this program to drive it
 * is a build failure rather than a channel that silently never runs. This
 * is what makes the configuration the single place: the table says what is
 * on, and the program has to be able to honour it.
 *
 * Written as a fold over the enumerators, so a new sink needs no new
 * assertion.
 */
template <std::uint8_t... Index>
constexpr bool every_enabled_sink_implemented(
    std::integer_sequence<std::uint8_t, Index...>)
{
    return ((!policy_of(static_cast<sink>(Index)).present ||
             implemented<static_cast<sink>(Index)>(sinks{})) &&
            ...);
}

static_assert(
    every_enabled_sink_implemented(
        std::make_integer_sequence<
            std::uint8_t,
            static_cast<std::uint8_t>(sink::count)>{}),
    "a sink is enabled in zpp/diag/config.h with no implementation in "
    "this program's zpp/diag/sinks.h");

} // namespace zpp::diag
