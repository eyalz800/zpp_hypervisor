#pragma once
// Filling the real host page table, for harnesses that compile the real
// hypervisor class.
//
// `zpp::arch::x86_64::page_table` is built *from* another page table:
// every level it writes needs the physical address of the level below,
// and it asks whoever it is being built from for that. On the machine
// the hypervisor runs on, the source is the operating system's own
// table. Here it is this - the identity, because a hosted harness has
// one address space and no distinction between virtual and physical to
// model.
//
// This is not a stand-in for a `zpp/` header. Nothing is replaced: the
// page table, its walker and `virtual_to_physical` are the real ones out
// of arch/x86_64/page_table.cpp, and this only answers the question the
// real code asks of whatever table it is handed. Three harnesses used to
// answer it instead with a `page_table_stub` nested in a copy of the
// hypervisor class, whose `virtual_to_physical` returned its argument -
// which made every assertion about a physical address an assertion about
// a virtual one.
#include "zpp/arch/x86_64/page_table.h"

#include <cstddef>
#include <cstdint>

namespace zpp::tests
{
/**
 * A page table in which every address translates to itself.
 */
struct identity_page_table
{
    std::uint64_t virtual_to_physical(std::uint64_t value) const
    {
        return value;
    }

    std::uint64_t virtual_to_physical(const void * value) const
    {
        return reinterpret_cast<std::uint64_t>(value);
    }
};

/**
 * Maps `size` bytes at `base` into `table`, readable and writable, with
 * each page translating to itself.
 *
 * `map_self` first, and it is not optional: `map_from` resolves the
 * addresses of the levels below through the table being built, so those
 * translations have to exist before the first ordinary mapping is
 * added. That is exactly what the comment on `page_table::map_page`
 * says, and skipping it produces a table that answers zero for
 * everything - which is what a stub returning its argument was hiding.
 */
inline void map_identity(zpp::arch::x86_64::page_table & table,
                         const void * base,
                         std::size_t size)
{
    identity_page_table identity;

    table.map_self(identity);
    table.map_from(base,
                   size,
                   zpp::arch::x86_64::page_table::protection::read |
                       zpp::arch::x86_64::page_table::protection::write,
                   identity);
}

} // namespace zpp::tests
