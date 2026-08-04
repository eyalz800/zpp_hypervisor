#pragma once
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

inline int __attribute__((naked)) vmptrld(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmptrld [rdi]
        jc vmptrld_fail
        mov eax, 0
        ret
    vmptrld_fail:
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

inline int __attribute__((naked)) vmclear(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        vmclear [rdi]
        jc vmclear_fail
        mov eax, 0
        ret
    vmclear_fail:
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

inline int __attribute__((naked)) invept(void *, void *)
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

inline int __attribute__((naked)) invvpid(void *, void *)
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

inline void __attribute__((naked)) vmresume()
{
    asm(R"!!(
        .intel_syntax noprefix
        vmresume
        // Same as vmlaunch above: reaching this means VM entry failed on
        // the controls or the host state, and there is no caller to
        // return to.
    1:  cli
        hlt
        jmp 1b
    )!!");
}

} // namespace zpp::arch::x86_64::vmx
