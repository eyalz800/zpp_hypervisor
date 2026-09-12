#pragma once
#include "zpp/heap.h"
#include <cstddef>
#include <span>

namespace zpp::crt
{
/**
 * Allocates from the global heap, trapping on failure. Exceptions are
 * unavailable, so returning null here would surface as a null dereference
 * far away from the exhausted allocation.
 *
 * Declared here rather than being file-local to crt.cpp because the
 * allocation operators that call it live in crt/operators.cpp. They are a
 * translation unit of their own so that a hosted test can compile the
 * rest of the C runtime without replacing the host's operator new - see
 * that file, and tests/crt, for what replacing it costs.
 * @{
 */
void * allocate_or_trap(std::size_t size);

/**
 * Allocates with an alignment stricter than the heap guarantees, by
 * over-allocating and storing the unaligned pointer just below the
 * returned address so it can be recovered on release.
 */
void * allocate_aligned_or_trap(std::size_t size, std::size_t alignment);

/**
 * Releases a pointer obtained from 'allocate_aligned_or_trap'.
 */
void deallocate_aligned(void * pointer, std::size_t alignment) noexcept;
/**
 * @}
 */

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

/**
 * The three linker-synthesized arrays, as ranges.
 *
 * The bounds are `extern void (*name[])()` symbols the linker fills in,
 * declared as arrays of unknown bound, and they exist only in a linked
 * image. That is exactly what a hosted test cannot reproduce: no portable
 * C++ places two separately declared arrays contiguously, and on Linux
 * the names are the linker's to define rather than ours.
 *
 * So the declarations, and only the declarations, live in
 * crt/init_array.cpp, and the walk above asks for them through these.
 * tests/crt supplies its own three and can therefore aim `main` and
 * `cleanup` at whatever it likes - which is the whole of what the test
 * needs, and it used to get it by deleting six lines out of crt.cpp with
 * awk at build time.
 *
 * Spans rather than a pointer pair, per the convention in CLAUDE.md.
 * Empty is the normal case for the preinit array and usually for the fini
 * array too: destructors of globals register through `__cxa_atexit` at
 * runtime, and `.fini_array` only receives functions marked with the
 * destructor attribute.
 * @{
 */
using array_entry = void (*)();

std::span<const array_entry> preinit_array();
std::span<const array_entry> init_array();
std::span<const array_entry> fini_array();
/**
 * @}
 */

} // namespace init
} // namespace zpp::crt
