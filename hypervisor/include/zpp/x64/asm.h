#pragma once
#include "zpp/x64/context.h"
#include <cstdint>

namespace zpp::x64
{
inline std::uint64_t __attribute__((naked)) cr0()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, cr0
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) cr3()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, cr3
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) cr4()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, cr4
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) cr0(std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov cr0, rdi
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) cr3(std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov cr3, rdi
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) cr4(std::uint64_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov cr4, rdi
        ret
    )!!");
}

inline void __attribute__((naked)) sgdt(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        sgdt [rdi]
        ret
    )!!");
}

inline void __attribute__((naked)) sidt(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        sidt [rdi]
        ret
    )!!");
}

inline void __attribute__((naked)) lgdt(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        lgdt [rdi]
        ret
    )!!");
}

inline void __attribute__((naked)) lidt(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        lidt [rdi]
        ret
    )!!");
}

inline void __attribute__((naked)) sldt(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        sldt [rdi]
        ret
    )!!");
}

inline void __attribute__((naked)) str(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        str rax
        mov [rdi], ax 
        ret
    )!!");
}

inline void __attribute__((naked)) ltr(void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        ltr [rdi]
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) dr7()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, dr7
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) cs()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, cs
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) ds()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, ds
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) es()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, es
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) fs()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, fs
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) gs()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, gs
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked)) ss()
{
    asm(R"!!(
        .intel_syntax noprefix
        mov ax, ss
        ret
    )!!");
}

inline std::uint64_t __attribute__((naked)) rflags()
{
    asm(R"!!(
        .intel_syntax noprefix
        pushfq
        pop rax
        ret
    )!!");
}

inline std::uint16_t __attribute__((naked))
load_segment_limit(std::uint16_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        lsl ax, di
        ret
    )!!");
}

inline std::uint32_t __attribute__((naked))
load_access_rights(std::uint32_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        lar eax, edi
        ret
    )!!");
}

inline void __attribute__((naked)) disable_interrupts()
{
    asm(R"!!(
        .intel_syntax noprefix
        cli
        ret
    )!!");
}

inline void __attribute__((naked)) enable_interrupts()
{
    asm(R"!!(
        .intel_syntax noprefix
        sti
        ret
    )!!");
}

inline void __attribute__((naked))
cpuid(std::uint32_t, std::uint32_t, std::uint32_t *)
{
    asm(R"!!(
        .intel_syntax noprefix
        push rbx
        push rdx
        mov eax, edi
        mov ecx, esi
        cpuid
        pop rsi
        mov [rsi], eax
        mov [rsi+4], ebx
        mov [rsi+8], ecx
        mov [rsi+0ch], edx
        pop rbx
        ret
    )!!");
}

inline void __attribute__((naked)) capture_context(x64::context *)
{
    asm(R"!!(
        .intel_syntax noprefix
        // Return address is at rbp+10h.
        pushfq // rflags is at rbp+8h.
        push rbp // rbp is at rbp.
        mov rbp, rsp // Fix rbp.
        push rax // rbp-8h.
        push rcx // rbp-10h.
        mov rax, rdi // The context parameter.
        mov rcx, [rbp-8h] // Restore rax to rcx.
        mov [rax], rcx // context->rax.
        mov [rax+8h], rbx // context->rbx.
        mov rcx, [rbp-10h] // Restore rcx to rcx.
        mov [rax+10h], rcx // context->rcx.
        mov [rax+18h], rdx // context->rdx.
        lea rcx, [rbp+18h] // Restore rsp to rcx.
        mov [rax+20h], rcx // context->rsp.
        mov rcx, [rbp] // Restore rbp to rcx.
        mov [rax+28h], rcx // context->rbp.
        mov [rax+30h], rsi // context->rsi.
        mov [rax+38h], rdi // context->rdi.
        mov [rax+40h], r8 // context->r8.
        mov [rax+48h], r9 // context->r9.
        mov [rax+50h], r10 // context->r10.
        mov [rax+58h], r11 // context->r11.
        mov [rax+60h], r12 // context->r12.
        mov [rax+68h], r13 // context->r13.
        mov [rax+70h], r14 // context->r14.
        mov [rax+78h], r15 // context->r15.
        mov rcx, [rbp+10h] // Restore return address to rcx.
        mov [rax+80h], rcx // context->rip.
        mov rcx, [rbp+8h] // Restore rflags to rcx.
        mov [rax+88h], rcx // context->rflags.
        movdqa [rax+90h], xmm0 // context->xmm0.
        movdqa [rax+0a0h], xmm1 // context->xmm1.
        movdqa [rax+0b0h], xmm2 // context->xmm2.
        movdqa [rax+0c0h], xmm3 // context->xmm3.
        movdqa [rax+0d0h], xmm4 // context->xmm4.
        movdqa [rax+0e0h], xmm5 // context->xmm5.
        movdqa [rax+0f0h], xmm6 // context->xmm6.
        movdqa [rax+100h], xmm7 // context->xmm7.
        movdqa [rax+110h], xmm8 // context->xmm8.
        movdqa [rax+120h], xmm9 // context->xmm9.
        movdqa [rax+130h], xmm10 // context->xmm10.
        movdqa [rax+140h], xmm11 // context->xmm11.
        movdqa [rax+150h], xmm12 // context->xmm12.
        movdqa [rax+160h], xmm13 // context->xmm13.
        movdqa [rax+170h], xmm14 // context->xmm14.
        movdqa [rax+180h], xmm15 // context->xmm15.
        fxsave [rax+190h] // context->fxsave.
        stmxcsr [rax+390h] // context->mxcsr.
        mov [rax+394h], cs // context->cs.
        mov [rax+396h], ds // context->ds.
        mov [rax+398h], es // context->es.
        mov [rax+39ah], fs // context->fs.
        mov [rax+39ch], gs // context->gs.
        mov [rax+39eh], ss // context->ss.
        add rsp, 10h // Skip rcx and rax on the stack.
        pop rbp // Restore rbp.
        add rsp, 8h // Skip rflags on the stack.
        ret
    )!!");
}

inline void __attribute__((naked)) restore_context(const x64::context *)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, rdi // The context parameter.
        mov rbx, [rax+8h] // context->rbx.
        mov rdx, [rax+18h] // context->rdx.
        mov rbp, [rax+28h] // context->rbp.
        mov rsi, [rax+30h] // context->rsi.
        mov rdi, [rax+38h] // context->rdi.
        mov r8, [rax+40h] // context->r8.
        mov r9, [rax+48h] // context->r9.
        mov r10, [rax+50h] // context->r10.
        mov r11, [rax+58h] // context->r11.
        mov r12, [rax+60h] // context->r12.
        mov r13, [rax+68h] // context->r13.
        mov r14, [rax+70h] // context->r14.
        mov r15, [rax+78h] // context->r15.
        ldmxcsr [rax+390h] // context->mxcsr.
        fxrstor [rax+190h] // context->fxsave.
        movdqa xmm0, [rax+90h] // context->xmm0.
        movdqa xmm1, [rax+0a0h] // context->xmm1.
        movdqa xmm2, [rax+0b0h] // context->xmm2.
        movdqa xmm3, [rax+0c0h] // context->xmm3.
        movdqa xmm4, [rax+0d0h] // context->xmm4.
        movdqa xmm5, [rax+0e0h] // context->xmm5.
        movdqa xmm6, [rax+0f0h] // context->xmm6.
        movdqa xmm7, [rax+100h] // context->xmm7.
        movdqa xmm8, [rax+110h] // context->xmm8.
        movdqa xmm9, [rax+120h] // context->xmm9.
        movdqa xmm10, [rax+130h] // context->xmm10.
        movdqa xmm11, [rax+140h] // context->xmm11.
        movdqa xmm12, [rax+150h] // context->xmm12.
        movdqa xmm13, [rax+160h] // context->xmm13.
        movdqa xmm14, [rax+170h] // context->xmm14.
        movdqa xmm15, [rax+180h] // context->xmm15.
        mov cx, [rax+39eh] // Load context->ss into cx.
        sub rsp, 6h // Align stack for stack segment.
        push cx // Push context->ss.
        mov rcx, [rax+20h] // Load context->rsp into rcx.
        push rcx // Push context->rsp.
        mov rcx, [rax+88h] // Load context->rflags into rcx.
        push rcx // Push context->rflags.
        mov cx, [rax+394h] // Load context->cs into cx.
        sub rsp, 6h // Align stack for code segment.
        push cx // Push context->cs.
        mov rcx, [rax+80h] // Load context->rip into rcx.
        push rcx // Push context->rip as return address.
        mov rcx, [rax] // Load context->rax into rcx.
        push rcx // Push context->rax.
        mov rcx, [rax+10h] // Load context->rcx.
        pop rax // Pop context->rax.
        iretq // Pop context->ss, context->rsp, context->rflags, context->cs, context->rip.
    )!!");
}

} // namespace zpp::x64
