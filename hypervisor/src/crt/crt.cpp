#include "zpp/crt.h"
#include "zpp/heap.h"
#include "zpp/spin_lock.h"
#include <cstddef>
#include <cstdint>
#include <new>

// The compiler passes the address of this symbol to __cxa_atexit. It is
// normally supplied by crtbegin, which a -nostdlib link never pulls in.
extern "C" constinit void * __dso_handle = &__dso_handle;

namespace
{
// Backing storage for the global heap. Owned by the CRT rather than by the
// hypervisor, which is what lets the heap be usable before any hypervisor
// object exists, and so lets constructors of objects with static storage
// duration allocate.
//
// Deliberately not part of heap.h: the size is a property of this global
// heap, not of the heap type.
constexpr std::size_t global_heap_size = 20 * 1024 * 1024; // 20 MB.
constexpr std::size_t heap_storage_alignment = 0x1000;

alignas(heap_storage_alignment) constinit std::byte
    g_heap_storage[global_heap_size]{};

constinit zpp::heap g_heap{};

struct registered_destructor
{
    void (*function)(void *);
    void * argument;
};

// Fixed capacity on purpose: registering a destructor happens from inside
// a constructor and must not itself allocate, and the table has to stay
// usable while the heap is being torn down.
constexpr std::size_t max_registered_destructors = 2048;

constinit registered_destructor
    g_registered_destructors[max_registered_destructors]{};
constinit std::size_t g_registered_destructor_count{};

constinit bool g_initialized{};
constinit bool g_torn_down{};

// Guard support for function local statics. The build passes
// -fno-threadsafe-statics, so the compiler does not currently emit calls
// to these - they exist so the ABI is complete if that flag is ever
// dropped. Note they need no threads or mutexes: the lock is a spin on a
// byte of the guard object, the same technique as the heap lock.
//
// The Itanium ABI gives each guard variable eight bytes. Byte zero
// records that initialization completed; byte one is used here as the
// spin lock, matching what the hosted runtimes do.
constexpr std::size_t guard_done_byte = 0;
constexpr std::size_t guard_lock_byte = 1;

unsigned char * guard_bytes(std::int64_t * guard_object)
{
    return reinterpret_cast<unsigned char *>(guard_object);
}
} // namespace

extern "C" {
/**
 * A word at a time, because a byte at a time was measured at **4.6
 * cycles a byte** on the rig and this is the hottest code in the tree.
 *
 * How that was found: `merge_nested_bitmaps` reads the guest
 * hypervisor's three bitmap pages every VM entry, and the read of one
 * four-kilobyte page cost 20,005 cycles of which the mapping was 976.
 * Nineteen thousand cycles to move 4,096 bytes is a byte loop, and it
 * was one. Nothing about the copy was uncached and nothing about the
 * mapping was slow - the loop was just doing one byte per iteration.
 *
 * It is not one copy. `flush_guest_vmcs12` copies a whole vmcs12 on
 * every VMPTRLD, four times a trust-level round trip; `merge_page`
 * copies three pages an entry; every guest-memory read in the
 * extended-page-table fault path lands here. So this multiplies out
 * across the whole exit handler rather than one phase of it.
 *
 * Unaligned 64-bit accesses, which x86-64 permits, so no head
 * alignment step. Word-wise access through a cast is the shape already
 * used next door in `merge_page`'s union, which reads both sides as
 * `std::uint64_t *`.
 *
 * Deliberately not `rep movsb`: it would be the faster answer on a part
 * with fast-short-rep, and it is inline assembly, which this tree
 * confines to `zpp/arch/x86_64/`. A CRT that is architecture-neutral is
 * worth more here than the last factor of two, and the measured gain
 * below is from the word loop alone.
 */
void * memcpy(void * dest, const void * src, std::size_t count)
{
    auto * to = static_cast<unsigned char *>(dest);
    const auto * from = static_cast<const unsigned char *>(src);

    constexpr auto word = sizeof(std::uint64_t);

    std::size_t i{};
    for (; (count - i) >= word; i += word) {
        auto * out = reinterpret_cast<std::uint64_t *>(to + i);
        const auto * in =
            reinterpret_cast<const std::uint64_t *>(from + i);
        *out = *in;
    }

    for (; i < count; ++i) {
        to[i] = from[i];
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
    //
    // Left a byte at a time. Overlapping moves are rare here and a
    // word-wise backward copy has a second overlap case of its own to
    // get right; the measured cost is all in `memcpy`, which cannot
    // overlap by contract.
    if (to > from && to < from + count) {
        for (auto i = count; i--;) {
            to[i] = from[i];
        }

        return dest;
    }

    // Non-overlapping, so the fast path above applies.
    return memcpy(dest, src, count);
}

void * memset(void * dest, int value, std::size_t count)
{
    auto * to = static_cast<unsigned char *>(dest);
    auto byte = static_cast<unsigned char>(value);

    constexpr auto word = sizeof(std::uint64_t);

    // 0x0101...01 times the byte, which is the byte in all eight lanes.
    auto filled = static_cast<std::uint64_t>(byte) * 0x0101010101010101ull;

    std::size_t i{};
    for (; (count - i) >= word; i += word) {
        *reinterpret_cast<std::uint64_t *>(to + i) = filled;
    }

    for (; i < count; ++i) {
        to[i] = byte;
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

int __cxa_atexit(void (*function)(void *), void * argument, void *)
{
    if (g_registered_destructor_count >= max_registered_destructors) {
        // Dropping a destructor would silently leak whatever it releases,
        // so fail loudly instead.
        __builtin_trap();
    }

    g_registered_destructors[g_registered_destructor_count++] = {function,
                                                                 argument};
    return 0;
}

void __cxa_finalize(void *)
{
    // Nothing to do - destructors are run by zpp::crt::init::cleanup so
    // that their ordering relative to the fini array stays explicit.
}

int __cxa_guard_acquire(std::int64_t * guard_object)
{
    auto * guard = guard_bytes(guard_object);

    if (__atomic_load_n(&guard[guard_done_byte], __ATOMIC_ACQUIRE)) {
        return 0;
    }

    while (__atomic_exchange_n(&guard[guard_lock_byte],
                               static_cast<unsigned char>(1),
                               __ATOMIC_ACQUIRE)) {
        zpp::spin_hint();
    }

    // Another CPU may have finished initializing while we waited.
    if (__atomic_load_n(&guard[guard_done_byte], __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&guard[guard_lock_byte],
                         static_cast<unsigned char>(0),
                         __ATOMIC_RELEASE);
        return 0;
    }

    return 1;
}

void __cxa_guard_release(std::int64_t * guard_object)
{
    auto * guard = guard_bytes(guard_object);

    __atomic_store_n(&guard[guard_done_byte],
                     static_cast<unsigned char>(1),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&guard[guard_lock_byte],
                     static_cast<unsigned char>(0),
                     __ATOMIC_RELEASE);
}

void __cxa_guard_abort(std::int64_t * guard_object)
{
    auto * guard = guard_bytes(guard_object);

    __atomic_store_n(&guard[guard_lock_byte],
                     static_cast<unsigned char>(0),
                     __ATOMIC_RELEASE);
}
}

namespace zpp::crt
{
/**
 * Allocates from the global heap, trapping on failure. Exceptions are
 * unavailable, so returning null here would surface as a null dereference
 * far away from the exhausted allocation.
 *
 * These three are declared in zpp/crt.h rather than being file-local,
 * because the allocation operators that call them are a translation unit
 * of their own. See crt/operators.cpp for why.
 */
void * allocate_or_trap(std::size_t size)
{
    // A zero sized allocation must still yield a unique pointer.
    auto * pointer = zpp::crt::heap().allocate(size ? size : 1);
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
        zpp::crt::heap().deallocate(pointer);
        return;
    }

    zpp::crt::heap().deallocate(static_cast<void **>(pointer)[-1]);
}

zpp::heap & heap()
{
    // Deliberately just an accessor - no initialization check on the
    // allocation path. zpp::crt::init::main() brings the heap up once,
    // before any global constructor can allocate.
    return g_heap;
}
} // namespace zpp::crt

namespace zpp::crt::init
{
void main()
{
    if (g_initialized) {
        return;
    }
    g_initialized = true;

    // The heap comes up before any constructor, since a constructor is
    // allowed to allocate.
    g_heap.init(g_heap_storage);

    for (auto entry : preinit_array()) {
        entry();
    }

    for (auto entry : init_array()) {
        entry();
    }
}

void cleanup()
{
    if (g_torn_down || !g_initialized) {
        return;
    }
    g_torn_down = true;

    // Reverse order of registration, so objects created later are
    // destroyed before the ones they may depend on.
    while (g_registered_destructor_count) {
        auto & entry =
            g_registered_destructors[--g_registered_destructor_count];
        entry.function(entry.argument);
    }

    auto finalizers = fini_array();
    for (auto entry = finalizers.rbegin(); entry != finalizers.rend();
         ++entry) {
        (*entry)();
    }
}

} // namespace zpp::crt::init
