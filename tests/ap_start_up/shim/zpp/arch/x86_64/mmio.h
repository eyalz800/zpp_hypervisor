#pragma once
// Shim for the memory-mapped register accessors, which exist here for one
// caller: `send_start_up_ipi`'s xAPIC branch, which writes the interrupt
// command register through the APIC page.
//
// **Recording rather than storing, and that is not a convenience.** The
// real accessors dereference the address, and the address is derived from
// IA32_APIC_BASE - which this harness answers out of a variable. Letting
// the store through would write to whatever number that variable holds.
// Recording it keeps the operand, which is the whole question the xAPIC
// half of 2685265 asks: the destination has to be written *before* the
// low half, because writing the low half is what sends the interrupt.
#include <atomic>
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * One recorded 32-bit register write. `order` counts from one across the
 * whole run, so two writes can be told apart by which came first.
 */
struct mmio_write32
{
    std::uint64_t address;
    std::uint32_t value;
    unsigned order;
};

inline std::atomic<unsigned> g_mmio_write_count{};
inline std::atomic<unsigned> g_mmio_order{};
inline mmio_write32 g_mmio_writes[64]{};

inline void mmio_reset()
{
    g_mmio_write_count.store(0);
    g_mmio_order.store(0);
    for (auto & write : g_mmio_writes) {
        write = mmio_write32{};
    }
}

inline void write32(volatile void * address, std::uint32_t value)
{
    auto index = g_mmio_write_count.fetch_add(1);
    if (index < (sizeof(g_mmio_writes) / sizeof(g_mmio_writes[0]))) {
        g_mmio_writes[index] = mmio_write32{
            reinterpret_cast<std::uint64_t>(const_cast<void *>(address)),
            value,
            g_mmio_order.fetch_add(1) + 1};
    }
}

inline std::uint32_t read32(const volatile void *)
{
    return 0;
}

/**
 * The two fences, copied from the real header rather than reduced.
 *
 * Nothing on the start-up path uses them; they are here because the real
 * hypervisor.h reaches zpp/nvme/admin_borrow.h, which does, and this
 * file shadows the real mmio.h for the whole translation unit. Copied
 * because they are not hardware in any sense this harness has to stand
 * in for - `std::atomic_thread_fence` is exactly what the real ones
 * call, on any architecture.
 * @{
 */
inline void order_stores()
{
    std::atomic_thread_fence(std::memory_order_release);
}

inline void order_loads()
{
    std::atomic_thread_fence(std::memory_order_acquire);
}
/**
 * @}
 */

} // namespace zpp::arch::x86_64
