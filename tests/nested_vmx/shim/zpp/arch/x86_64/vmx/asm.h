#pragma once
// Shim for the VMX instruction wrappers, so the real vmcs.h class works
// natively against an in-memory VMCS rather than a processor.
#include "zpp/arch/x86_64/context.h"
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
// The whole encoding space vmcs_fields.h uses fits in 0x8000.
inline std::uint64_t g_vmcs[0x8000]{};
inline bool g_vmcs_valid = true;

inline int vmread(std::uint64_t field, void * out)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    *static_cast<std::uint64_t *>(out) = g_vmcs[field];
    return 0;
}

inline int vmwrite(std::uint64_t field, std::uint64_t value)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    g_vmcs[field] = value;
    return 0;
}

inline int vmxon(void *)
{
    return 0;
}
inline int vmxoff()
{
    return 0;
}
inline int vmptrld(void *)
{
    return 0;
}
inline int vmptrst(void *)
{
    return 0;
}
inline int vmclear(void *)
{
    return 0;
}
inline int invept(std::uint64_t, void *)
{
    return 0;
}
inline int invvpid(std::uint64_t, void *)
{
    return 0;
}
inline void vmlaunch()
{
}
inline void vmresume()
{
}
inline void nested_vmlaunch()
{
}
inline void nested_vmresume()
{
}

constexpr std::uint64_t nested_entry_recovery_field = 0x6008;
constexpr std::size_t nested_entry_recovery_rsp_offset = 0x20;

extern "C" void zpp_vmx_entry_failed(std::uint64_t flags);
extern "C" void
zpp_vmx_nested_entry_failure(arch::x86_64::context * recovery);

} // namespace zpp::arch::x86_64::vmx
