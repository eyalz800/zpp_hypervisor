#pragma once
#include <atomic>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * Accesses to a device's memory mapped registers.
 *
 * Free functions over a volatile pointer rather than a struct of volatile
 * members, so that a register block can be described once as a plain
 * layout and every access to it says out loud that it is a device access.
 *
 * volatile is what makes each of these exactly one instruction that is
 * neither elided, duplicated, nor merged with its neighbour, which is the
 * whole requirement for a register. It is not a barrier against accesses
 * to ordinary memory, which is why the fence functions below exist
 * separately.
 */
inline std::uint32_t read32(const volatile void * address)
{
    return *static_cast<const volatile std::uint32_t *>(address);
}

inline void write32(volatile void * address, std::uint32_t value)
{
    *static_cast<volatile std::uint32_t *>(address) = value;
}

inline std::uint64_t read64(const volatile void * address)
{
    return *static_cast<const volatile std::uint64_t *>(address);
}

inline void write64(volatile void * address, std::uint64_t value)
{
    *static_cast<volatile std::uint64_t *>(address) = value;
}

/**
 * Orders every store before it against every store after it.
 *
 * The pairing this exists for is a descriptor built in ordinary write back
 * memory and then made live by one final store - either the store that
 * flips a descriptor's ownership bit or the store to a doorbell register.
 * Ordinary stores are not volatile, so nothing in the language stops the
 * compiler from sinking one past the volatile store that publishes them,
 * and a device reading the descriptor at that moment would read half of
 * it.
 *
 * On x86 this costs no instruction: stores retire in program order, so a
 * release fence is a compiler barrier and nothing more. That is exactly
 * what the kernel's own wmb() reduces to on this architecture, and it is
 * the barrier the driver this was ported from uses in the same two places.
 */
inline void order_stores()
{
    std::atomic_thread_fence(std::memory_order_release);
}

/**
 * Orders every load before it against every load after it.
 *
 * The pairing here is reading a descriptor's ownership bit and then
 * reading the rest of the descriptor: the second must not be hoisted above
 * the first, or the fields belong to the previous owner's contents.
 */
inline void order_loads()
{
    std::atomic_thread_fence(std::memory_order_acquire);
}

} // namespace zpp::arch::x86_64
