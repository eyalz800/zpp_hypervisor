#include "zpp/heap.h"
#include <cstring>
#include <new>

namespace zpp
{
static heap g_heap;

heap & global_heap()
{
    return g_heap;
}

void heap::init()
{
    if (m_initialized) {
        return;
    }

    auto * initial = reinterpret_cast<block_header *>(m_storage);
    initial->size = size - header_size;
    initial->free = true;
    initial->next = nullptr;
    m_free_list = initial;
    m_initialized = true;
}

void heap::lock()
{
    while (m_lock.test_and_set(std::memory_order_acquire)) {
        // Spin with pause hint for x86.
        asm volatile("pause");
    }
}

void heap::unlock()
{
    m_lock.clear(std::memory_order_release);
}

heap::block_header * heap::find_free_block(std::size_t bytes)
{
    auto * current = m_free_list;
    while (current) {
        if (current->free && current->size >= bytes) {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

void heap::split_block(block_header * block, std::size_t bytes)
{
    if (block->size >= bytes + header_size + alignment) {
        auto * new_block = reinterpret_cast<block_header *>(
            reinterpret_cast<std::byte *>(block) + header_size + bytes);
        new_block->size = block->size - bytes - header_size;
        new_block->free = true;
        new_block->next = block->next;
        block->size = bytes;
        block->next = new_block;
    }
}

void heap::coalesce()
{
    auto * current = m_free_list;
    while (current && current->next) {
        if (current->free && current->next->free) {
            current->size += header_size + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

void * heap::allocate(std::size_t bytes)
{
    if (!bytes) {
        return nullptr;
    }

    bytes = (bytes + alignment - 1) & ~(alignment - 1);

    lock();

    auto * block = find_free_block(bytes);
    if (!block) {
        coalesce();
        block = find_free_block(bytes);
    }

    if (!block) {
        unlock();
        return nullptr;
    }

    split_block(block, bytes);
    block->free = false;

    unlock();

    return reinterpret_cast<std::byte *>(block) + header_size;
}

void heap::deallocate(void * ptr)
{
    if (!ptr) {
        return;
    }

    auto * block = reinterpret_cast<block_header *>(
        static_cast<std::byte *>(ptr) - header_size);

    lock();
    block->free = true;
    unlock();
}

} // namespace zpp

void * operator new(std::size_t size)
{
    return zpp::global_heap().allocate(size);
}

void * operator new[](std::size_t size)
{
    return zpp::global_heap().allocate(size);
}

void operator delete(void * ptr) noexcept
{
    zpp::global_heap().deallocate(ptr);
}

void operator delete[](void * ptr) noexcept
{
    zpp::global_heap().deallocate(ptr);
}

void operator delete(void * ptr, std::size_t) noexcept
{
    zpp::global_heap().deallocate(ptr);
}

void operator delete[](void * ptr, std::size_t) noexcept
{
    zpp::global_heap().deallocate(ptr);
}
