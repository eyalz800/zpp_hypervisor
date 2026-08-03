#include "zpp/heap.h"
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" {
void * memcpy(void * dest, const void * src, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        *(static_cast<unsigned char *>(dest) + i) =
            *(static_cast<const unsigned char *>(src) + i);
    }

    return dest;
}

void * memmove(void * dest, const void * src, std::size_t count)
{
    auto * to = static_cast<unsigned char *>(dest);
    auto * from = static_cast<const unsigned char *>(src);

    // When the ranges overlap with the destination above the source, a
    // forward copy would clobber source bytes before reading them, so
    // copy backwards instead.
    if (to > from && to < from + count) {
        for (auto i = count; i--;) {
            to[i] = from[i];
        }

        return dest;
    }

    for (std::size_t i{}; i < count; ++i) {
        to[i] = from[i];
    }

    return dest;
}

void * memset(void * dest, int value, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        *(static_cast<unsigned char *>(dest) + i) =
            static_cast<unsigned char>(value);
    }

    return dest;
}

int memcmp(const void * dest, const void * src, std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        auto diff = *(static_cast<const unsigned char *>(dest) + i) -
                    *(static_cast<const unsigned char *>(src) + i);
        if (diff) {
            return diff;
        }
    }

    return 0;
}

size_t strlen(const char * string)
{
    for (std::size_t i{};; ++i) {
        if (!string[i]) {
            return i;
        }
    }
}

void __cxa_pure_virtual()
{
}
}

namespace
{
/**
 * Allocates from the global heap, trapping on failure. Exceptions are
 * unavailable, so returning null here would surface as a null dereference
 * far away from the exhausted allocation.
 */
void * allocate_or_trap(std::size_t size)
{
    // A zero sized allocation must still yield a unique pointer.
    auto * pointer = zpp::global_heap().allocate(size ? size : 1);
    if (!pointer) {
        __builtin_trap();
    }

    return pointer;
}

/**
 * Allocates with an alignment stricter than the heap guarantees, by
 * over-allocating and storing the unaligned pointer just below the
 * returned address so it can be recovered on release.
 */
void * allocate_aligned_or_trap(std::size_t size, std::size_t alignment)
{
    if (alignment <= zpp::heap::default_alignment) {
        return allocate_or_trap(size);
    }

    auto * base = static_cast<std::byte *>(
        allocate_or_trap(size + alignment + sizeof(void *)));

    auto aligned = (reinterpret_cast<std::uintptr_t>(base) +
                    sizeof(void *) + alignment - 1) &
                   ~(alignment - 1);

    reinterpret_cast<void **>(aligned)[-1] = base;
    return reinterpret_cast<void *>(aligned);
}

/**
 * Releases a pointer obtained from 'allocate_aligned_or_trap'.
 */
void deallocate_aligned(void * pointer, std::size_t alignment) noexcept
{
    if (!pointer) {
        return;
    }

    if (alignment <= zpp::heap::default_alignment) {
        zpp::global_heap().deallocate(pointer);
        return;
    }

    zpp::global_heap().deallocate(static_cast<void **>(pointer)[-1]);
}
} // namespace

void * operator new(std::size_t size)
{
    return allocate_or_trap(size);
}

void * operator new[](std::size_t size)
{
    return allocate_or_trap(size);
}

void * operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return zpp::global_heap().allocate(size ? size : 1);
}

void * operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return zpp::global_heap().allocate(size ? size : 1);
}

void * operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned_or_trap(size,
                                    static_cast<std::size_t>(alignment));
}

void * operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned_or_trap(size,
                                    static_cast<std::size_t>(alignment));
}

void * operator new(std::size_t size,
                    std::align_val_t alignment,
                    const std::nothrow_t &) noexcept
{
    return allocate_aligned_or_trap(size,
                                    static_cast<std::size_t>(alignment));
}

void * operator new[](std::size_t size,
                      std::align_val_t alignment,
                      const std::nothrow_t &) noexcept
{
    return allocate_aligned_or_trap(size,
                                    static_cast<std::size_t>(alignment));
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

void operator delete(void * ptr, const std::nothrow_t &) noexcept
{
    zpp::global_heap().deallocate(ptr);
}

void operator delete[](void * ptr, const std::nothrow_t &) noexcept
{
    zpp::global_heap().deallocate(ptr);
}

void operator delete(void * ptr, std::align_val_t alignment) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr, std::align_val_t alignment) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr,
                     std::size_t,
                     std::align_val_t alignment) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr,
                       std::size_t,
                       std::align_val_t alignment) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr,
                     std::align_val_t alignment,
                     const std::nothrow_t &) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr,
                       std::align_val_t alignment,
                       const std::nothrow_t &) noexcept
{
    deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}
