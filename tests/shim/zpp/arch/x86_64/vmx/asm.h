#pragma once
// Shim for the VMX instruction wrappers, so the real vmcs.h class works
// natively against an in-memory VMCS rather than a processor.
#include "zpp/arch/x86_64/context.h"
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * One backing region per VMCS, selected by `vmptrld`.
 *
 * **This used to be a single array and `vmptrld` used to be a no-op**,
 * which made vmcs01 and vmcs02 the same object here. Every ordering
 * property that depends on *which* VMCS is current was therefore
 * untestable, and a change resting on one - the deferred guest-state
 * copy - passed nine unit cases and then reset the guest on the rig,
 * twice, for about 250 unclean resets of a real Windows installation.
 *
 * A suite that cannot tell two VMCS regions apart cannot check anything
 * about switching between them, and that is worth more machinery than
 * it costs: this is sixteen regions keyed by the physical address the
 * real code passes, and nothing else changes.
 */
inline constexpr std::size_t g_vmcs_regions = 8;

/** Region zero, which every existing case means when it reaches for
 * `g_vmcs` directly - it is the region loaded until something calls
 * `vmptrld` with a second address. */
inline std::uint64_t g_vmcs[0x8000]{};
inline std::uint64_t g_vmcs_other[g_vmcs_regions - 1][0x8000]{};
inline std::uint64_t g_vmcs_address[g_vmcs_regions]{};
inline std::uint64_t * g_vmcs_loaded = g_vmcs;
inline bool g_vmcs_valid = true;

inline std::uint64_t * g_vmcs_region(std::size_t index)
{
    return (0 == index) ? g_vmcs : g_vmcs_other[index - 1];
}

inline int vmread(std::uint64_t field, void * out)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    *static_cast<std::uint64_t *>(out) = g_vmcs_loaded[field];
    return 0;
}

inline int vmwrite(std::uint64_t field, std::uint64_t value)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    g_vmcs_loaded[field] = value;
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
inline int vmptrld(void * pointer)
{
    // The real code passes the address *of* the physical address, which
    // is what the instruction takes - SDM 33.3, VMPTRLD, "the operand
    // is the address of a 64-bit field containing the address of the
    // VMCS".
    auto address = *static_cast<std::uint64_t *>(pointer);

    for (std::size_t i{}; i < g_vmcs_regions; ++i) {
        if ((0 != g_vmcs_address[i]) && (g_vmcs_address[i] == address)) {
            g_vmcs_loaded = g_vmcs_region(i);
            return 0;
        }
    }

    // First sight of this VMCS: take a free slot. A region starts
    // zeroed, which is what a freshly allocated VMCS looks like and is
    // exactly the state the first failure launched a guest with.
    for (std::size_t i{}; i < g_vmcs_regions; ++i) {
        if (0 == g_vmcs_address[i]) {
            g_vmcs_address[i] = address;
            g_vmcs_loaded = g_vmcs_region(i);
            return 0;
        }
    }

    return 1;
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
