#pragma once
#include <cstddef>
#include <cstdint>

namespace zpp::x64
{
/**
 * The number of vectors the exception entry table covers, which is every
 * vector an IDT can hold.
 */
constexpr std::size_t number_of_exception_vectors = 256;

/**
 * The distance in bytes between consecutive stubs in the exception entry
 * table. Every stub is padded to this, so the entry point for a vector is
 * a multiplication rather than a table lookup.
 */
constexpr std::size_t exception_entry_stride = 16;

/**
 * The state an exception entry stub leaves on the stack for the handler.
 * The vector and error code are pushed by the stub, the rest by the CPU
 * when it delivers the exception. Vectors that carry no error code get a
 * zero pushed in its place, so the frame always has the same shape.
 */
struct exception_frame
{
    std::uint64_t vector{};
    std::uint64_t error_code{};
    std::uint64_t rip{};
    std::uint64_t cs{};
    std::uint64_t rflags{};
    std::uint64_t rsp{};
    std::uint64_t ss{};
};

/**
 * The base of the exception entry table: one stub per vector, each
 * exception_entry_stride bytes apart. Declared as a function because that
 * is what it is made of, but it is never called - the symbol only names
 * the start of the table.
 */
extern "C" void zpp_x64_exception_entry_table();

/**
 * Returns the entry point for the given vector, which is the address an
 * IDT gate for that vector points at.
 */
inline std::uint64_t exception_entry(std::size_t vector)
{
    return reinterpret_cast<std::uint64_t>(
               &zpp_x64_exception_entry_table) +
           (vector * exception_entry_stride);
}

/**
 * The handler every entry stub funnels into, which never returns.
 * Implemented by the hypervisor.
 */
extern "C" [[noreturn]] void zpp_x64_exception(exception_frame * frame);

} // namespace zpp::x64
