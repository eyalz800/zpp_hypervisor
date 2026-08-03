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
