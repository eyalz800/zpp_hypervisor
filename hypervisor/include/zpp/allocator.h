#pragma once
#include "zpp/crt.h"
#include <cstddef>

namespace zpp
{
/**
 * A standard library compatible allocator backed by a zpp::heap.
 *
 * Defaults to the global heap owned by the CRT. That default constructor
 * is deliberately not constexpr, which is what stops a container from
 * being given static storage duration without an init array entry - see
 * the constant initialization notes in CLAUDE.md.
 */
template <typename T>
class allocator
{
public:
    using value_type = T;

    allocator() noexcept : m_heap(&crt::heap())
    {
    }

    constexpr explicit allocator(heap & h) noexcept : m_heap(&h)
    {
    }

    template <typename U>
    constexpr allocator(const allocator<U> & other) noexcept :
        m_heap(other.m_heap)
    {
    }

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
