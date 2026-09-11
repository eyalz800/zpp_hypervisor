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

/**
 * How many times each encoding has been fetched out of a region, ever.
 *
 * A field this VMM decides not to read is invisible in the *value* it
 * hands over whenever the value it would have written is the same - the
 * instruction-length gate is exactly that case, since `honest_exit_length`
 * already zeroed the field it had just read. A test that only compares
 * vmcs12 therefore cannot tell "the read was removed" from "the read was
 * kept", which is the whole property those gates exist for.
 *
 * `vmcs_reads_taken` counts logical reads and would answer, but it counts
 * every field together, so a case would have to assume nothing else on
 * the path changed with the exit reason. This counts per encoding and
 * assumes nothing.
 *
 * Note it is below the field cache: a read answered from the cache does
 * not reach here. Nothing in the exit-information block reads a field
 * twice, so the two agree there, and a case that ever needs them not to
 * should say so.
 */
inline std::uint64_t g_vmread_field_count[0x8000]{};

inline int vmread(std::uint64_t field, void * out)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    g_vmread_field_count[field] = g_vmread_field_count[field] + 1;
    *static_cast<std::uint64_t *>(out) = g_vmcs_loaded[field];
    return 0;
}

/**
 * How many times each encoding has been written into a region, ever.
 *
 * The write side of `g_vmread_field_count`, and needed for the same
 * reason it was: an elided write leaves vmcs02 holding the value the
 * write would have put there, so the *value* is identical either way and
 * a case comparing only vmcs02 cannot tell an elision from a write. That
 * is the whole property `hot_state_saved` exists for.
 *
 * `vmcs_writes_taken` counts every field together and `hot_state_writes_
 * skipped` counts the family together, so neither can say *which* field
 * was skipped - and a gate aimed at CR0 that silently also skipped
 * IA32_EFER would pass both. This assumes nothing.
 */
inline std::uint64_t g_vmwrite_field_count[0x8000]{};

// SDM 27.4.1 permits processors to clear reserved segment-access bits.
// KVM handle_vmwrite uses 0x1f0ff. Fixtures can select either behavior.
inline std::uint32_t g_vmwrite_access_rights_mask{0xffffffffu};

inline int vmwrite(std::uint64_t field, std::uint64_t value)
{
    if (!g_vmcs_valid || (field >= 0x8000)) {
        return 1;
    }
    g_vmwrite_field_count[field] = g_vmwrite_field_count[field] + 1;
    if (field >= 0x4814 && field <= 0x4822 && (field & 1) == 0) {
        value &= g_vmwrite_access_rights_mask;
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
// Named `_raw` to match the real header, where `vmcs.h` wraps these to
// end the VMCS field cache's window first.
inline int vmptrld_raw(void * pointer)
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
/**
 * How many times `vmptrst` has been executed, ever.
 *
 * The claim `current_vmcs_region_physical` rests on is that **nothing on
 * the shadow-copy path executes one**, and a claim about an instruction
 * not being executed cannot be checked by looking at the result - the
 * result is the same either way. So it is counted, and
 * `tests/nested_exit` asserts the count does not move across a copy in
 * each direction. Put the VMPTRST back and that assertion fails; nothing
 * else in the suite does.
 */
inline std::uint64_t g_vmptrst_calls{};

/**
 * SDM 33.3: "Stores the current-VMCS pointer into a specified memory
 * address."
 *
 * **This used to return 0 and write nothing**, which is not what the
 * instruction does and made the harness unable to tell a correct restore
 * from a restore of address zero. It has to be faithful here or the
 * negative control above proves the wrong thing: with a lying VMPTRST the
 * old code fails for the shim's reasons rather than its own.
 */
inline int vmptrst(void * pointer)
{
    g_vmptrst_calls = g_vmptrst_calls + 1;

    for (std::size_t i{}; i < g_vmcs_regions; ++i) {
        if (g_vmcs_loaded == g_vmcs_region(i)) {
            *static_cast<std::uint64_t *>(pointer) = g_vmcs_address[i];
            return 0;
        }
    }

    return 1;
}
// Named `_raw` to match the real header, where `vmcs.h` wraps these to
// end the VMCS field cache's window first.
inline int vmclear_raw(void *)
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

constexpr std::uint64_t nested_entry_slot_field = 0x0000;
constexpr std::size_t nested_entry_recovery_slots = 32;
constexpr std::size_t nested_entry_recovery_rsp_offset = 0x20;

extern "C" {
inline arch::x86_64::context *
    zpp_vmx_nested_entry_recovery[nested_entry_recovery_slots]{};
}

extern "C" void zpp_vmx_entry_failed(std::uint64_t flags);
extern "C" void
zpp_vmx_nested_entry_failure(arch::x86_64::context * recovery);

} // namespace zpp::arch::x86_64::vmx
