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

/**
 * What a 32-bit register read answers, indexed by the low twelve bits of
 * the address - so one page's worth of registers, which is what a local
 * APIC is.
 *
 * Answering out of a table rather than returning zero, because zero is a
 * *legal* answer for every one of these and therefore cannot be told
 * apart from "this harness does not model the register". The one read on
 * the start-up path is the local APIC version register, whose "Max LVT
 * Entry" field decides how many local vector table entries
 * `reset_local_apic_after_init` writes - and read as zero that field says
 * one entry, which is a silent under-test of six of the seven.
 */
inline std::uint32_t g_mmio_read_answers[0x1000 / 4]{};

inline void mmio_reset()
{
    g_mmio_write_count.store(0);
    g_mmio_order.store(0);
    for (auto & write : g_mmio_writes) {
        write = mmio_write32{};
    }
    for (auto & answer : g_mmio_read_answers) {
        answer = 0;
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

/**
 * How many times `address` was written, and the value of the last one.
 * Zero for an address nothing wrote - which is the assertion for every
 * register the local APIC reset must leave alone.
 * @{
 */
inline unsigned mmio_writes_of(std::uint64_t address)
{
    auto count = 0u;
    auto seen = g_mmio_write_count.load();
    for (auto slot = 0u;
         (slot < seen) &&
         (slot < (sizeof(g_mmio_writes) / sizeof(g_mmio_writes[0])));
         ++slot) {
        if (g_mmio_writes[slot].address == address) {
            ++count;
        }
    }
    return count;
}

inline std::uint32_t mmio_last_write_of(std::uint64_t address)
{
    auto value = std::uint32_t{};
    auto seen = g_mmio_write_count.load();
    for (auto slot = 0u;
         (slot < seen) &&
         (slot < (sizeof(g_mmio_writes) / sizeof(g_mmio_writes[0])));
         ++slot) {
        if (g_mmio_writes[slot].address == address) {
            value = g_mmio_writes[slot].value;
        }
    }
    return value;
}
/**
 * @}
 */

inline std::uint32_t read32(const volatile void * address)
{
    auto offset =
        reinterpret_cast<std::uint64_t>(const_cast<void *>(address)) &
        0xfff;
    return g_mmio_read_answers[offset / 4];
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
