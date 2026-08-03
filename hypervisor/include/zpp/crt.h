#pragma once
#include "zpp/heap.h"

namespace zpp::crt
{
/**
 * The heap that backs operator new and zpp::allocator.
 *
 * Belongs to the CRT rather than to heap.h because the CRT owns the
 * instance and its backing storage - heap.h only describes the type. This
 * is a plain accessor: zpp::crt::init::main() brings the heap up before
 * any global constructor can allocate, so there is no initialization check
 * on the allocation path. Allocating before that point traps.
 *
 * The return type is written qualified because within this namespace the
 * name 'heap' resolves to this function, not to the type.
 */
zpp::heap & heap();

/**
 * Startup and cleanup of the C runtime.
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
void main();

/**
 * Cleans the C runtime up: runs destructors registered through
 * __cxa_atexit, followed by the fini array, both in reverse order of
 * registration.
 *
 * Only the first call has an effect. Call this only when the module is not
 * staying resident - once the hypervisor is live its globals must outlive
 * every guest, so the successful path deliberately never runs destructors.
 */
void cleanup();

} // namespace init
} // namespace zpp::crt
