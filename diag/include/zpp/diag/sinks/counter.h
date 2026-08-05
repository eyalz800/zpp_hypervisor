#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/ring.h"

#include <atomic>
#include <cstdint>

namespace zpp::diag
{
/**
 * The simplest sink there is: it counts what it is given.
 *
 * Sounds like a placeholder and is not. Once the guest is running there is
 * no console, the serial port belongs to the guest and the development
 * target has none anyway - so the first question about any new logging
 * path is whether records are reaching it at all, and this answers that
 * from a debugger with three reads and no device. It is also the sink to
 * turn on when the question is whether the *pump* is running: `delivered`
 * on its cursor only moves if something drained the ring.
 *
 * It is written as a class template on its own channel id purely to
 * demonstrate the idiom every sink with storage has to use: a static data
 * member of a class template is emitted only if it is odr-used, and the
 * only use is inside `write`, which is only instantiated if the fold in
 * pump.h reaches it. So a disabled counter is not a counter nobody
 * increments - there is no counter. The alternative, `static inline` in a
 * plain struct, is emitted whether or not anything touches it, which is
 * what exit_trace does today in every build.
 */
template <sink Which>
struct counter_for
{
    static constexpr sink id = Which;

    /**
     * How many records were accepted, and how many payload bytes they
     * carried between them.
     * @{
     */
    static inline std::atomic<std::uint64_t> accepted{};
    static inline std::atomic<std::uint64_t> bytes{};
    /**
     * @}
     */

    /**
     * The last record's event id and time stamp, so a debugger can tell a
     * live stream from a stalled one without decoding anything.
     * @{
     */
    static inline std::atomic<std::uint32_t> last_event{};
    static inline std::atomic<std::uint64_t> last_when{};
    /**
     * @}
     */

    /**
     * Always. There is no device to be absent.
     */
    static bool ready()
    {
        return true;
    }

    /**
     * Cannot fail, cannot wait, touches nothing outside these four words.
     */
    static bool write(const record & entry)
    {
        accepted.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(entry.length, std::memory_order_relaxed);
        last_event.store(entry.event, std::memory_order_relaxed);
        last_when.store(entry.when, std::memory_order_relaxed);
        return true;
    }
};

using counter_sink = counter_for<sink::counter>;

} // namespace zpp::diag
