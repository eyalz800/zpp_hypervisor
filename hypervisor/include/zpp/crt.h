#pragma once
#include "zpp/heap.h"

namespace zpp
{
/**
 * The heap that backs operator new and zpp::allocator.
 *
 * Declared here rather than in heap.h because the CRT owns the instance
 * and its backing storage - heap.h only describes the type. This is a
 * plain accessor: zpp::crt::init::run() brings the heap up before any
 * global constructor can allocate, so there is no initialization check on
 * the allocation path. Allocating before that point traps.
 */
heap & global_heap();

} // namespace zpp

namespace zpp::crt
{
/**
 * Startup and teardown of the C runtime.
 */
namespace init
{
/**
 * Initializes the C runtime: prepares the global heap, then runs the
 * preinit and init arrays, that is the constructors of objects with static
 * storage duration whose initializers are not constant, plus anything
 * marked with the constructor attribute.
 *
 * Only the first call has an effect, so every CPU may call this freely.
 * The heap is ready before the first constructor runs, so a constructor is
 * free to allocate. Allocating before this point traps.
 */
void run();

/**
 * Tears the C runtime down: runs destructors registered through
 * __cxa_atexit, followed by the fini array, both in reverse order of
 * registration.
 *
 * Only the first call has an effect. Call this only when the module is not
 * staying resident - once the hypervisor is live its globals must outlive
 * every guest, so the successful path deliberately never runs destructors.
 */
void teardown();

} // namespace init
} // namespace zpp::crt
