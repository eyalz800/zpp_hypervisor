#include "zpp/crt/init.h"
#include <cstddef>
#include <cstdint>

// Bounds of the initializer and finalizer arrays, synthesized by the
// linker. When a section is absent lld gives the two symbols the same
// value, so the corresponding loop simply does nothing.
extern "C" {
extern void (*__preinit_array_start[])();
extern void (*__preinit_array_end[])();
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
extern void (*__fini_array_start[])();
extern void (*__fini_array_end[])();
}

// The compiler passes the address of this symbol to __cxa_atexit. It is
// normally supplied by crtbegin, which a -nostdlib link never pulls in.
extern "C" constinit void * __dso_handle = &__dso_handle;

namespace
{
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

constinit bool g_constructed{};
constinit bool g_destroyed{};

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
    // Nothing to do - destructors are run by zpp::crt::fini so
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
        asm volatile("pause");
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
} // extern "C"

namespace zpp::crt
{
void init()
{
    if (g_constructed) {
        return;
    }
    g_constructed = true;

    for (auto * entry = __preinit_array_start;
         entry != __preinit_array_end;
         ++entry) {
        (*entry)();
    }

    for (auto * entry = __init_array_start; entry != __init_array_end;
         ++entry) {
        (*entry)();
    }
}

void fini()
{
    if (g_destroyed || !g_constructed) {
        return;
    }
    g_destroyed = true;

    // Reverse order of registration, so objects created later are
    // destroyed before the ones they may depend on.
    while (g_registered_destructor_count) {
        auto & entry =
            g_registered_destructors[--g_registered_destructor_count];
        entry.function(entry.argument);
    }

    for (auto * entry = __fini_array_end; entry != __fini_array_start;) {
        (*--entry)();
    }
}

} // namespace zpp::crt
