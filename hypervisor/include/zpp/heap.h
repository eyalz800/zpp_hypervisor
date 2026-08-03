#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace zpp
{
class heap
{
public:
    static constexpr std::size_t default_alignment = 16;

    void init(std::byte * storage, std::size_t storage_size);
    void * allocate(std::size_t bytes);
    void deallocate(void * ptr);
    constexpr std::size_t capacity() const { return m_size; }

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

    void lock();
    void unlock();

    block_header * m_free_list{};
    std::size_t m_size{};
    std::atomic_flag m_lock{};
    bool m_initialized{};
};

heap & global_heap();

template <typename T>
class allocator
{
public:
    using value_type = T;

    constexpr explicit allocator(heap & h) noexcept : m_heap(&h) {}

    template <typename U>
    constexpr allocator(const allocator<U> & other) noexcept : m_heap(other.m_heap) {}

    T * allocate(std::size_t n)
    {
        return static_cast<T *>(m_heap->allocate(n * sizeof(T)));
    }

    void deallocate(T * p, std::size_t) noexcept
    {
        m_heap->deallocate(p);
    }

    constexpr friend bool operator==(const allocator & a,
                                     const allocator & b) noexcept
    {
        return a.m_heap == b.m_heap;
    }

private:
    template <typename U>
    friend class allocator;
    heap * m_heap;
};

} // namespace zpp
