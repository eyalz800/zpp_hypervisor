#pragma once
#include <cstdint>

namespace zpp::x64::intel
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
        ret
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
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) rdmsr(std::uint32_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ecx, edi
        rdmsr
        shl rdx, 32
        or rax, rdx
        ret
    )!!");
}

inline void __attribute__((naked)) wrmsr(std::uint32_t, std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ecx, edi
        mov eax, esi
        shr rsi, 32
        mov edx, esi
        wrmsr
        ret
    )!!");
}

inline void __attribute__((naked)) xsetbv(std::uint32_t, std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ecx, edi
        mov eax, esi
        shr rsi, 32
        mov edx, esi
        xsetbv
        ret
    )!!");
}

inline void __attribute__((naked)) invd()
{
    asm(R"!!(
        .intel_syntax noprefix
        invd
        ret
    )!!");
}

} // namespace zpp::x64::intel
