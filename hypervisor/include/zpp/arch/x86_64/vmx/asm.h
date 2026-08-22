#pragma once
#include "zpp/arch/x86_64/context.h"
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
inline int __attribute__((naked)) vmxon(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmxon [rdi]
        jc vmxon_fail
        mov eax, 0
        ret
    vmxon_fail:
        mov eax, 1
        ret
    )!!");
}

/**
 * Why a failed launch was silent, and now is not.
 *
 * The park below is correct and has to stay - there is no caller to
 * return to - but for a long time it carried no information, so every
 * launch failure looked identical from outside: a processor stopped at a
 * named symbol, with no exits, and nothing to say why. That is precisely
 * the shape that gets read as "hung inside vmlaunch" when what happened
 * is "VM entry failed its checks".
 *
 * The VMCS is still current on the failing path, so the reason is one
 * VMREAD away - SDM 31.4 puts it in `vm_instruction_error`, field
 * `0x4400`. It is written here, before the halt, where a state dump can
 * find it.
 *
 * The registers clobbered are the guest's, and that is acceptable: this
 * path never resumes a guest, and an error code that names the failed
 * check is worth more than register state nothing will use.
 * @{
 */
// `used` and `retain` for the same reason `zpp_build_switches` carries
// them: the only reference is from inside the assembly below, which is
// not an odr-use, so without these the definitions are never emitted and
// the link fails on the symbol the asm names.
extern "C" {
[[gnu::used, gnu::retain]] inline constinit std::uint64_t
    zpp_launch_instruction_error{};
[[gnu::used, gnu::retain]] inline constinit std::uint64_t
    zpp_launch_failed{};
}
/** @} */

inline void __attribute__((naked)) vmlaunch()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmlaunch
        // Only reached when VM entry failed the checks on the VM execution
        // controls or the host state, which leaves the flags set and
        // execution right here. There is no caller to return to: this is
        // entered by loading it into RIP with restore_context, so RSP is
        // the guest stack and a ret would jump wherever that happens to
        // point. Park instead, so a debugger finds the CPU stopped at a
        // named symbol rather than somewhere unrecoverable.
        //
        // Say why first. The VMCS is still current, so the instruction
        // error is one VMREAD away, and without it every failure here
        // looks the same from outside.
        mov rax, 0x4400
        vmread rcx, rax
        mov qword ptr [rip + zpp_launch_instruction_error], rcx
        mov qword ptr [rip + zpp_launch_failed], 1
    1:  cli
        hlt
        jmp 1b
    )!!");
}

inline int __attribute__((naked)) vmxoff()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmxoff
        jc vmxoff_fail
        mov eax, 0
        ret
    vmxoff_fail:
        mov eax, 1
        ret
    )!!");
}

/**
 * The bare instruction. **Call `vmptrld` from `vmcs.h` instead**, which
 * ends the VMCS field cache's window first - a value read through a cache
 * filled under a different current VMCS does not fault, it answers.
 *
 * Named `_raw` so a translation unit that includes only this header and
 * reaches for `vmptrld` fails to compile rather than silently skipping
 * that. The rename exists for exactly that reason.
 */
inline int __attribute__((naked)) vmptrld_raw(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmptrld [rdi]
        jc vmptrld_raw_fail
        mov eax, 0
        ret
    vmptrld_raw_fail:
        mov eax, 1
        ret
    )!!");
}

inline int __attribute__((naked)) vmptrst(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmptrst [rdi]
        jc vmptrst_fail
        mov eax, 0
        ret
    vmptrst_fail:
        mov eax, 1
        ret
    )!!");
}

/**
 * The bare instruction. **Call `vmclear` from `vmcs.h` instead**, which
 * ends the VMCS field cache's window first - a value read through a cache
 * filled under a different current VMCS does not fault, it answers.
 *
 * Named `_raw` so a translation unit that includes only this header and
 * reaches for `vmclear` fails to compile rather than silently skipping
 * that. The rename exists for exactly that reason.
 */
inline int __attribute__((naked)) vmclear_raw(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmclear [rdi]
        jc vmclear_raw_fail
        mov eax, 0
        ret
    vmclear_raw_fail:
        mov eax, 1
        ret
    )!!");
}

inline int __attribute__((naked)) vmread(std::uint64_t, void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmread [rsi], rdi
        jc vmread_fail
        mov eax, 0
        ret
    vmread_fail:
        mov eax, 1
        ret
    )!!");
}

inline int __attribute__((naked)) vmwrite(std::uint64_t, std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmwrite rdi, rsi
        jc vmwrite_fail
        mov eax, 0
        ret
    vmwrite_fail:
        mov eax, 1
        ret
    )!!");
}

/**
 * The invalidation type each of these takes is a *register* operand rather
 * than a pointer to one. SDM 33.3, INVEPT, opens its operation section
 * with "INVEPT_TYPE := value of register operand", and its Op/En row makes
 * that operand ModRM:reg - which is the first argument here. INVVPID says
 * the same of INVVPID_TYPE.
 *
 * Spelled as an integer rather than as void *, because it was spelled as
 * void * and one of the two call sites then passed the address of a
 * variable holding the type. A stack address is not 1 or 2, so
 * "IF ... processor does not support INVEPT_TYPE THEN VMfail(Invalid
 * operand to INVEPT/INVVPID)" applied to every execution: the instruction
 * failed every time and nothing was ever invalidated. It is silent by
 * nature - the only consequence of a skipped invalidation is a stale
 * translation - so the type of the parameter is what has to stop it.
 * @{
 */
inline int __attribute__((naked)) invept(std::uint64_t, void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        invept rdi, [rsi]
        jc invept_fail
        mov eax, 0
        ret
    invept_fail:
        mov eax, 1
        ret
    )!!");
}

inline int __attribute__((naked)) invvpid(std::uint64_t, void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        invvpid rdi, [rsi]
        jc invvpid_fail
        mov eax, 0
        ret
    invvpid_fail:
        mov eax, 1
        ret
    )!!");
}
/**
 * @}
 */

/**
 * Called when a VM entry fails, with RFLAGS as it was left by the failing
 * instruction.
 *
 * A failing vmresume produces no VM exit, so the ordinary exit path never
 * sees it and the VMCS's own error field is the only thing that says why.
 * Halting without reading it discards the answer, which cost a long
 * afternoon of guessing.
 */
extern "C" void zpp_vmx_entry_failed(std::uint64_t flags);

inline void __attribute__((naked)) vmresume()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmresume
        // Failed. Hand the flags over - carry means there was no current
        // VMCS, zero means the error field has been set - and let the
        // reporter read the field while this VMCS is still current.
        pushfq
        pop rdi
        call zpp_vmx_entry_failed
        // Same as vmlaunch above: reaching this means VM entry failed on
        // the controls or the host state, and there is no caller to
        // return to.
    1:  cli
        hlt
        jmp 1b
    )!!");
}

/**
 * Where the two entries below park the address of the context to unwind
 * to when a VM entry into a second-level guest fails.
 *
 * A VMCS field rather than a register or a global, and the choice is
 * forced. On a failed VM entry no VM exit happens, so nothing has been
 * reloaded: every general purpose register still holds what the entry was
 * about to give the guest, RSP names a guest stack the host page table
 * does not map, and the only per-processor thing still addressable is the
 * VMCS that was current. So the recovery point has to come out of the
 * VMCS.
 *
 * The CR3-target values are the fields with nothing in them. SDM 25.6.7
 * makes the list consulted only "if the CR3-target count is n, ... the
 * first n CR3-target values", and this VMM reports a CR3-target count of
 * zero through IA32_VMX_MISC and writes zero into the field - so no
 * processor ever reads them, and no guest hypervisor is offered the list.
 * They are natural width, which a pointer needs, where the other spare
 * field of the right shape is 32 bits.
 *
 * Named here and spelled literally in the two stubs below, because inline
 * assembly cannot see a constant expression. Whoever changes one changes
 * both; nested_vmx.cpp asserts this is the encoding of CR3-target value 0.
 */
constexpr std::uint64_t nested_entry_recovery_field = 0x6008;

/**
 * Where the recovery context's stack pointer sits inside it, which the
 * stubs below reach by offset because they cannot call anything until
 * they have it.
 */
constexpr std::size_t nested_entry_recovery_rsp_offset = 0x20;

static_assert(nested_entry_recovery_rsp_offset ==
                  __builtin_offsetof(arch::x86_64::context, rsp),
              "The nested entry recovery stub indexes context::rsp by "
              "hand and the offset has moved.");

/**
 * Called when a VM entry into a second-level guest fails, on the stack the
 * recovery context named, with that context as its argument.
 *
 * Does not return: it puts the processor back where the entry was decided
 * from, which is inside the VM exit handler that decided it, so that the
 * guest hypervisor can be told its VM entry failed rather than the
 * processor stopping. A guest hypervisor's VMLAUNCH is a guest
 * instruction, and no guest instruction may halt a processor.
 */
extern "C" void
zpp_vmx_nested_entry_failure(arch::x86_64::context * recovery);

/**
 * VM entry into a second-level guest, launching and resuming.
 *
 * Separate from vmlaunch and vmresume above because the failure paths
 * differ in kind rather than in detail. A failed entry on this VMM's own
 * VMCS is a bug here and stops the processor; a failed entry on a VMCS
 * built out of a guest hypervisor's is something a guest asked for, and
 * has to come back with an answer.
 *
 * The recovery reads the context pointer out of the VMCS, moves onto the
 * host stack that context recorded, and calls. Nothing before that may
 * touch memory through RSP: it still names the guest's stack, which the
 * host page table does not map. VMREAD into a register touches none.
 * @{
 */
inline void __attribute__((naked)) nested_vmlaunch()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmlaunch
        mov eax, 0x6008
        vmread rdi, rax
        mov rsp, [rdi + 0x20]
        call zpp_vmx_nested_entry_failure
    1:  cli
        hlt
        jmp 1b
    )!!");
}

inline void __attribute__((naked)) nested_vmresume()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmresume
        mov eax, 0x6008
        vmread rdi, rax
        mov rsp, [rdi + 0x20]
        call zpp_vmx_nested_entry_failure
    1:  cli
        hlt
        jmp 1b
    )!!");
}
/**
 * @}
 */

} // namespace zpp::arch::x86_64::vmx
