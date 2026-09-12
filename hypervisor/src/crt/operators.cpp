// The allocation operators.
//
// A translation unit of their own, and the reason is a test rather than
// tidiness. crt.cpp is a *replacement* C runtime - it defines memcpy and
// friends with C linkage, the whole __cxa_* ABI and __dso_handle - and
// tests/crt compiles it directly, renaming the C-linkage names with
// #define so the host's own remain available as an oracle. The twenty
// definitions below are the one thing a #define cannot rename, because
// `operator` is a keyword.
//
// Keeping them there is not an option either: linked into a hosted binary
// they route *every* allocation in the process through the 20 MB arena,
// including allocations the standard library's own initializers make
// before main, which is before zpp::crt::init::main() has brought the
// heap up. That is the trap CLAUDE.md documents - allocate before init
// and the heap returns null and this file calls __builtin_trap() - so the
// harness would die at start-up for a reason that says nothing about the
// hypervisor.
//
// The three helpers they call stay in crt.cpp and are declared in
// zpp/crt.h, so the over-alignment arithmetic - which is where the
// interesting logic is - is still compiled and tested there.
#include "zpp/crt.h"
#include <cstddef>
#include <new>

void * operator new(std::size_t size)
{
    return zpp::crt::allocate_or_trap(size);
}

void * operator new[](std::size_t size)
{
    return zpp::crt::allocate_or_trap(size);
}

void * operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return zpp::crt::heap().allocate(size ? size : 1);
}

void * operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return zpp::crt::heap().allocate(size ? size : 1);
}

void * operator new(std::size_t size, std::align_val_t alignment)
{
    return zpp::crt::allocate_aligned_or_trap(
        size, static_cast<std::size_t>(alignment));
}

void * operator new[](std::size_t size, std::align_val_t alignment)
{
    return zpp::crt::allocate_aligned_or_trap(
        size, static_cast<std::size_t>(alignment));
}

void * operator new(std::size_t size,
                    std::align_val_t alignment,
                    const std::nothrow_t &) noexcept
{
    return zpp::crt::allocate_aligned_or_trap(
        size, static_cast<std::size_t>(alignment));
}

void * operator new[](std::size_t size,
                      std::align_val_t alignment,
                      const std::nothrow_t &) noexcept
{
    return zpp::crt::allocate_aligned_or_trap(
        size, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete[](void * ptr) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete(void * ptr, std::size_t) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete[](void * ptr, std::size_t) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete(void * ptr, const std::nothrow_t &) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete[](void * ptr, const std::nothrow_t &) noexcept
{
    zpp::crt::heap().deallocate(ptr);
}

void operator delete(void * ptr, std::align_val_t alignment) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr, std::align_val_t alignment) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr,
                     std::size_t,
                     std::align_val_t alignment) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr,
                       std::size_t,
                       std::align_val_t alignment) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete(void * ptr,
                     std::align_val_t alignment,
                     const std::nothrow_t &) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}

void operator delete[](void * ptr,
                       std::align_val_t alignment,
                       const std::nothrow_t &) noexcept
{
    zpp::crt::deallocate_aligned(ptr, static_cast<std::size_t>(alignment));
}
