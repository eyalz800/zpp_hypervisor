#pragma once
#include "zpp/spin_lock.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace zpp
{
class heap
{
public:
    static constexpr std::size_t default_alignment = 16;

    constexpr heap() = default;

    void init(std::span<std::byte> storage);
    void * allocate(std::size_t bytes);
    void deallocate(void * ptr);

    /**
     * Forces the lock open, whoever was holding it.
     *
     * For exactly one caller and one situation: a processor coming back
     * from a power transition that took every other processor away. This
     * lock is a byte in memory, memory survives S3, and an allocation is
     * reachable from inside a VM exit - so the transition can catch
     * another processor mid-allocation and leave the lock held by a
     * processor the platform has since reset. Nothing would ever release
     * it, and the first allocation after the resume would spin for good.
     *
     * Only meaningful while the caller is the only processor running,
     * which is what a resume entry point is. It is not a way out of a
     * deadlock anywhere else.
     *
     * It does not make the free list correct, and that is worth stating
     * rather than glossing: the power can land while a processor is
     * *inside* split_block or coalesce, in which case the list is left
     * half updated and forcing the lock open exposes it. There is no
     * better answer available - re-running init would reset the list and
     * orphan every live allocation, including the log's own nodes - so
     * this is an accepted residual risk, taken because the alternative is
     * certain: a lock held by a processor that no longer exists hangs the
     * resume every time, where a list caught mid-update is a window of a
     * few instructions.
     */
    void abandon_lock()
    {
        m_lock.unlock();
    }
    constexpr std::size_t capacity() const
    {
        return m_size;
    }

private:
    struct block_header
    {
        std::size_t size;
        bool free;
        block_header * next;
    };

    static constexpr std::size_t header_size =
        (sizeof(block_header) + default_alignment - 1) &
        ~(default_alignment - 1);

    block_header * find_free_block(std::size_t bytes);
    void split_block(block_header * block, std::size_t bytes);
    void coalesce();

    block_header * m_free_list{};
    std::size_t m_size{};
    spin_lock m_lock{};
    bool m_initialized{};
};

} // namespace zpp
