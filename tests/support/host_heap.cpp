// The global heap, for harnesses that compile the real hypervisor class.
//
// `zpp::hypervisor::log` writes into a `zpp::list<zpp::string>`, and
// `zpp::allocator` routes every container allocation through
// `zpp::crt::heap()`. A harness that compiles the real class therefore
// needs that accessor to exist - and until this file, the way it was
// made to exist was a stand-in `zpp/hypervisor/hypervisor.h` carrying a
// `log` that discarded its arguments.
//
// This is not a stub of `crt.cpp`, and the difference is the point:
// `hypervisor/src/crt/heap.cpp` is compiled in beside it, so the
// allocator under test is the real arena allocator and the log lines a
// harness produces are produced the way the hypervisor produces them.
// Only the *storage* is local, and only because `crt.cpp` cannot be
// linked into a hosted binary at all: it defines `operator new`, which
// would take over the host standard library's own allocations, and it
// defines the linker-synthesized init-array bounds, which a host link
// does not have. That is the same reason tests/crt gives for including
// crt.cpp rather than linking it.
#include "zpp/crt.h"
#include <cstddef>
#include <span>

namespace zpp::crt
{
/**
 * The arena's size.
 *
 * Not the hypervisor's twenty megabytes: nothing here is resident for a
 * boot, and a harness that exhausts this has produced tens of thousands
 * of log lines, which is a fact about the harness worth failing over
 * rather than absorbing.
 */
static constexpr std::size_t host_arena_size = 4 * 1024 * 1024;

zpp::heap & heap()
{
    static std::byte storage[host_arena_size];
    static zpp::heap the_heap;

    // A second static rather than an initializer on the first: zpp::heap
    // is not copyable, so it cannot be returned out of a lambda into its
    // own definition.
    static const bool initialized = [] {
        the_heap.init(std::span{storage});
        return true;
    }();
    static_cast<void>(initialized);

    return the_heap;
}

} // namespace zpp::crt
