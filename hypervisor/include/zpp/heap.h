#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace zpp
{
class heap
{
public:
    static constexpr std::size_t size = 20 * 1024 * 1024;
    static constexpr std::size_t alignment = 16;

    void init();
    void * allocate(std::size_t bytes);
    void deallocate(void * ptr);

private:
    struct block_header
    {
        std::size_t size;
        bool free;
        block_header * next;
    };

    static constexpr std::size_t header_size =
        (sizeof(block_header) + alignment - 1) & ~(alignment - 1);

    block_header * find_free_block(std::size_t bytes);
    void split_block(block_header * block, std::size_t bytes);
    void coalesce();

    void lock();
    void unlock();

    alignas(4096) std::byte m_storage[size]{};
    block_header * m_free_list{};
    std::atomic_flag m_lock{};
    bool m_initialized{};
};

heap & global_heap();

} // namespace zpp
