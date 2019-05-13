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
        mov [rsi+0xc], edx
        pop rbx
        ret
    )!!");
}

inline void __attribute__((naked)) capture_context(x64::context *)
{
    asm(R"!!(
        .intel_syntax noprefix
        // Return address is at rbp+0x10.
        pushfq // rflags is at rbp+0x8.
        push rbp // rbp is at rbp.
        mov rbp, rsp // Fix rbp.
        push rax // rbp-0x8.
        push rcx // rbp-0x10.
        mov rax, rdi // The context parameter.
        mov rcx, [rbp-0x8] // Restore rax to rcx.
        mov [rax], rcx // context->rax.
        mov [rax+0x8], rbx // context->rbx.
        mov rcx, [rbp-0x10] // Restore rcx to rcx.
        mov [rax+0x10], rcx // context->rcx.
        mov [rax+0x18], rdx // context->rdx.
        lea rcx, [rbp+0x18] // Restore rsp to rcx.
        mov [rax+0x20], rcx // context->rsp.
        mov rcx, [rbp] // Restore rbp to rcx.
        mov [rax+0x28], rcx // context->rbp.
        mov [rax+0x30], rsi // context->rsi.
        mov [rax+0x38], rdi // context->rdi.
        mov [rax+0x40], r8 // context->r8.
        mov [rax+0x48], r9 // context->r9.
        mov [rax+0x50], r10 // context->r10.
        mov [rax+0x58], r11 // context->r11.
        mov [rax+0x60], r12 // context->r12.
        mov [rax+0x68], r13 // context->r13.
        mov [rax+0x70], r14 // context->r14.
        mov [rax+0x78], r15 // context->r15.
        mov rcx, [rbp+0x10] // Restore return address to rcx.
        mov [rax+0x80], rcx // context->rip.
        mov rcx, [rbp+0x8] // Restore rflags to rcx.
        mov [rax+0x88], rcx // context->rflags.
        movdqa [rax+0x90], xmm0 // context->xmm0.
        movdqa [rax+0xa0], xmm1 // context->xmm1.
        movdqa [rax+0xb0], xmm2 // context->xmm2.
        movdqa [rax+0xc0], xmm3 // context->xmm3.
        movdqa [rax+0xd0], xmm4 // context->xmm4.
        movdqa [rax+0xe0], xmm5 // context->xmm5.
        movdqa [rax+0xf0], xmm6 // context->xmm6.
        movdqa [rax+0x100], xmm7 // context->xmm7.
        movdqa [rax+0x110], xmm8 // context->xmm8.
        movdqa [rax+0x120], xmm9 // context->xmm9.
        movdqa [rax+0x130], xmm10 // context->xmm10.
        movdqa [rax+0x140], xmm11 // context->xmm11.
        movdqa [rax+0x150], xmm12 // context->xmm12.
        movdqa [rax+0x160], xmm13 // context->xmm13.
        movdqa [rax+0x170], xmm14 // context->xmm14.
        movdqa [rax+0x180], xmm15 // context->xmm15.
        fxsave [rax+0x190] // context->fxsave.
        stmxcsr [rax+0x390] // context->mxcsr.
        mov [rax+0x394], cs // context->cs.
        mov [rax+0x396], ds // context->ds.
        mov [rax+0x398], es // context->es.
        mov [rax+0x39a], fs // context->fs.
        mov [rax+0x39c], gs // context->gs.
        mov [rax+0x39e], ss // context->ss.
        add rsp, 0x10 // Skip rcx and rax on the stack.
        pop rbp // Restore rbp.
        add rsp, 0x8 // Skip rflags on the stack.
        ret
    )!!");
}

inline void __attribute__((naked)) restore_context(const x64::context *)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rax, rdi // The context parameter.
        mov rbx, [rax+0x8] // context->rbx.
        mov rdx, [rax+0x18] // context->rdx.
        mov rbp, [rax+0x28] // context->rbp.
        mov rsi, [rax+0x30] // context->rsi.
        mov rdi, [rax+0x38] // context->rdi.
        mov r8, [rax+0x40] // context->r8.
        mov r9, [rax+0x48] // context->r9.
        mov r10, [rax+0x50] // context->r10.
        mov r11, [rax+0x58] // context->r11.
        mov r12, [rax+0x60] // context->r12.
        mov r13, [rax+0x68] // context->r13.
        mov r14, [rax+0x70] // context->r14.
        mov r15, [rax+0x78] // context->r15.
        ldmxcsr [rax+0x390] // context->mxcsr.
        fxrstor [rax+0x190] // context->fxsave.
        movdqa xmm0, [rax+0x90] // context->xmm0.
        movdqa xmm1, [rax+0xa0] // context->xmm1.
        movdqa xmm2, [rax+0xb0] // context->xmm2.
        movdqa xmm3, [rax+0xc0] // context->xmm3.
        movdqa xmm4, [rax+0xd0] // context->xmm4.
        movdqa xmm5, [rax+0xe0] // context->xmm5.
        movdqa xmm6, [rax+0xf0] // context->xmm6.
        movdqa xmm7, [rax+0x100] // context->xmm7.
        movdqa xmm8, [rax+0x110] // context->xmm8.
        movdqa xmm9, [rax+0x120] // context->xmm9.
        movdqa xmm10, [rax+0x130] // context->xmm10.
        movdqa xmm11, [rax+0x140] // context->xmm11.
        movdqa xmm12, [rax+0x150] // context->xmm12.
        movdqa xmm13, [rax+0x160] // context->xmm13.
        movdqa xmm14, [rax+0x170] // context->xmm14.
        movdqa xmm15, [rax+0x180] // context->xmm15.
        mov cx, [rax+0x39e] // Load context->ss into cx.
        sub rsp, 0x6 // Align stack for stack segment.
        push cx // Push context->ss.
        mov rcx, [rax+0x20] // Load context->rsp into rcx.
        push rcx // Push context->rsp.
        mov rcx, [rax+0x88] // Load context->rflags into rcx.
        push rcx // Push context->rflags.
        mov cx, [rax+0x394] // Load context->cs into cx.
        sub rsp, 0x6 // Align stack for code segment.
        push cx // Push context->cs.
        mov rcx, [rax+0x80] // Load context->rip into rcx.
        push rcx // Push context->rip as return address.
        mov rcx, [rax] // Load context->rax into rcx.
        push rcx // Push context->rax.
        mov rcx, [rax+0x10] // Load context->rcx.
        pop rax // Pop context->rax.
        iretq // Pop context->ss, context->rsp, context->rflags, context->cs, context->rip.
    )!!");
}

} // namespace zpp::x64
