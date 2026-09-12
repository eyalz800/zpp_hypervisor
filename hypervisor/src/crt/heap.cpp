#include "zpp/heap.h"

// This file implements the heap type only. The global heap instance and
// its backing storage live in crt/crt.cpp, next to the code that brings
// them up.

namespace zpp
{
void heap::init(std::span<std::byte> storage)
{
    if (m_initialized) {
        return;
    }

    auto * initial = reinterpret_cast<block_header *>(storage.data());
    initial->size = storage.size() - header_size;
    initial->free = true;
    initial->next = nullptr;
    m_free_list = initial;
    m_size = storage.size();
    m_initialized = true;
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
    if (block->size >= bytes + header_size + default_alignment) {
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

    // Reject sizes that would wrap while rounding up, otherwise a huge
    // request would round down to a small successful allocation.
    if (bytes > (~std::size_t{} - default_alignment + 1)) {
        return nullptr;
    }

    bytes = (bytes + default_alignment - 1) & ~(default_alignment - 1);

    m_lock.lock();

    auto * block = find_free_block(bytes);
    if (!block) {
        coalesce();
        block = find_free_block(bytes);
    }

    if (!block) {
        m_lock.unlock();
        return nullptr;
    }

    split_block(block, bytes);
    block->free = false;

    m_lock.unlock();

    return reinterpret_cast<std::byte *>(block) + header_size;
}

void heap::deallocate(void * ptr)
{
    if (!ptr) {
        return;
    }

    auto * block = reinterpret_cast<block_header *>(
        static_cast<std::byte *>(ptr) - header_size);

    m_lock.lock();
    block->free = true;

    // Merge adjacent free blocks right away, so fragmentation does not
    // build up until an allocation has already failed.
    coalesce();
    m_lock.unlock();
}

} // namespace zpp
