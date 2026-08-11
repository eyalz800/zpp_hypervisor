// The linker-synthesized initializer and finalizer arrays.
//
// Six declarations and three accessors, and they are a translation unit
// of their own for the same kind of reason crt/operators.cpp is: they are
// the part of the C runtime a hosted test cannot reproduce.
//
// The bounds are arrays of unknown bound whose addresses the linker
// fills in, and the loops in crt.cpp walk from start to end by increment,
// so a hosted stand-in would have to place two separately declared arrays
// contiguously. No portable C++ does that, and on Linux these names are
// the linker's to define rather than ours. tests/crt therefore supplies
// its own three accessors and does not compile this file - where before
// it deleted these six lines out of crt.cpp with awk at build time and
// declared the symbols as plain pointers instead.
//
// One latent trap, recorded in CLAUDE.md and unchanged by the move:
// `__preinit_array_start` and `__preinit_array_end` are both link-time
// address 0, which PC-relative addressing turns into *the module base* at
// runtime rather than 0. They are equal, so the loop is a no-op and this
// is harmless today - but it would walk from the module base if they ever
// differed.
#include "zpp/crt.h"
#include <span>

extern "C" {
extern void (*__preinit_array_start[])();
extern void (*__preinit_array_end[])();
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
extern void (*__fini_array_start[])();
extern void (*__fini_array_end[])();
}

namespace zpp::crt::init
{
namespace
{
/**
 * The two bounds as a range. When a section is absent lld gives the two
 * symbols the same value, so the span is empty and the corresponding walk
 * simply does nothing.
 */
std::span<const array_entry> between(void (**first)(), void (**last)())
{
    return {reinterpret_cast<const array_entry *>(first),
            static_cast<std::size_t>(last - first)};
}

} // namespace

std::span<const array_entry> preinit_array()
{
    return between(__preinit_array_start, __preinit_array_end);
}

std::span<const array_entry> init_array()
{
    return between(__init_array_start, __init_array_end);
}

std::span<const array_entry> fini_array()
{
    return between(__fini_array_start, __fini_array_end);
}

} // namespace zpp::crt::init
