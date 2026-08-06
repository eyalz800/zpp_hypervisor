#pragma once
#include <cstdint>

/**
 * Whether a device's DMA will land where we tell it to, and what may be
 * done about it when the answer is no.
 *
 * This header exists to be implemented twice. Remapping hardware turns
 * "give the controller a physical address" into a question about the
 * guest operating system's own translation tables, and there are two
 * defensible answers to it that differ in what they cost the guest
 * rather than in what they achieve:
 *
 * - Ask the guest to reserve a range for us, by describing it in the
 *   remapping tables the firmware hands over, and let the guest go on
 *   owning the hardware. The guest keeps per-device isolation and keeps
 *   reporting it. We widen exactly one device's reach by exactly our
 *   window and nothing else.
 *
 * - Take the remapping hardware ourselves and map memory as we choose.
 *   Correct without depending on anything the guest does, and it costs
 *   the guest the isolation it would otherwise have had.
 *
 * The first is the default because the second cannot be reconciled with
 * a guest that is required to keep protecting itself. The second is
 * kept because the first rests on the guest honouring a description
 * that no firmware has been observed to produce for a storage
 * controller, and a fallback that has already been written is worth
 * more than one that is only argued for.
 *
 * Neither is selected here. A strategy is chosen by the diagnostics
 * configuration, and both compile to nothing when the channel is off.
 */
namespace zpp::nvme
{
/**
 * A range of host physical memory a device is required to reach.
 *
 * Physical rather than a span, because nothing ever reads through this
 * - it is handed to hardware, and describing it as a pointer would
 * invite someone to dereference it from the wrong address space.
 */
struct dma_window
{
    std::uint64_t physical{};
    std::uint64_t length{};
};

/**
 * What a strategy concluded.
 *
 * Distinct values rather than a bool because the three are different
 * things to go and look at, and a channel that stays quiet has to be
 * able to say which of them it was.
 */
enum class reachability
{
    /**
     * The window is already reachable and nothing was done. Either no
     * remapping hardware is translating this device, or something has
     * already mapped the window for it.
     */
    reachable,

    /**
     * The window was not reachable and this strategy made it so. Worth
     * distinguishing from the above: it means state was changed, so it
     * is the case that has to survive the guest changing it back.
     */
    made_reachable,

    /**
     * The window is not reachable and this strategy will not make it
     * so. **The caller must not hand the controller any address.** A
     * refusal is the successful outcome of asking; it is not an error
     * to be retried past.
     */
    refused,
};

/**
 * Whether a conclusion permits handing hardware our addresses.
 */
constexpr bool may_submit(reachability result)
{
    return (reachability::reachable == result) ||
           (reachability::made_reachable == result);
}

/**
 * The interface both strategies present.
 *
 * Not a base class with virtual functions: there is no runtime
 * polymorphism here and a vtable in this tree costs a relocation in
 * `.data.rel.ro` for no benefit. A strategy is a type with these two
 * static members, selected at compile time, and this comment is the
 * contract rather than the compiler.
 *
 *     static reachability ensure_reachable(const dma_window & window);
 *     static const char * describe(reachability result);
 *
 * `ensure_reachable` must be safe to call repeatedly and from any
 * point after the guest is running. It must perform no DMA, and it
 * must be read-only in every path that ends in `refused` - the whole
 * reason this is asked rather than tried is that finding out by
 * submitting can bugcheck the guest.
 *
 * It is called again before each epoch rather than once, because every
 * mechanism that could invalidate the answer - a function level reset,
 * a power transition, a domain being torn down and rebuilt - happens
 * without announcement while the guest runs.
 */
} // namespace zpp::nvme
