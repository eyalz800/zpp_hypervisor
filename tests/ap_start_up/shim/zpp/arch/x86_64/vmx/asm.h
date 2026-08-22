#pragma once
// Shim for the VMX instruction wrappers.
//
// The three that take a region pointer **record what they were given**,
// per thread, because that is the whole question this harness asks: a
// VMXON or VMPTRLD that names another processor's region is the defect,
// and the only way to see it is to keep the operand.
//
// Everything backing them is thread_local, standing in for per-processor
// state. One shared array would let one thread's VMWRITEs be read back by
// another, which no real machine does and which would hide exactly the
// kind of defect this exists to find.
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
// The whole encoding space vmcs_fields.h uses fits in 0x8000.
inline thread_local std::uint64_t g_vmcs[0x8000]{};

// What this thread's VMXON, VMCLEAR and VMPTRLD were last handed.
inline thread_local std::uint64_t g_vmxon_region{};
inline thread_local std::uint64_t g_vmclear_region{};
inline thread_local std::uint64_t g_vmptrld_region{};

inline int vmread(std::uint64_t field, void * out)
{
    if (field >= 0x8000) {
        return 1;
    }
    *static_cast<std::uint64_t *>(out) = g_vmcs[field];
    return 0;
}

inline int vmwrite(std::uint64_t field, std::uint64_t value)
{
    if (field >= 0x8000) {
        return 1;
    }
    g_vmcs[field] = value;
    return 0;
}

inline int vmxon(void * region)
{
    g_vmxon_region = *static_cast<std::uint64_t *>(region);
    return 0;
}

inline int vmxoff()
{
    return 0;
}

// Named `_raw` to match the real header, where `vmcs.h` wraps these to
// end the VMCS field cache's window first.
inline int vmclear_raw(void * region)
{
    g_vmclear_region = *static_cast<std::uint64_t *>(region);
    return 0;
}

// Named `_raw` to match the real header, where `vmcs.h` wraps these to
// end the VMCS field cache's window first.
inline int vmptrld_raw(void * region)
{
    g_vmptrld_region = *static_cast<std::uint64_t *>(region);
    return 0;
}

inline int invvpid(std::uint64_t, void *)
{
    return 0;
}

} // namespace zpp::arch::x86_64::vmx
