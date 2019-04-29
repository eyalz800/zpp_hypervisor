namespace zpp::x64
{
extern "C" void __attribute__((naked)) zpp_x64_capture_context_into_stack()
{
    asm(R"!!(
        .intel_syntax noprefix
        // Start of context structure is at rbp+18h.
        // Return address is at rbp+10h.
        pushfq // rflags is at rbp+8h.
        push rbp // rbp is at rbp.
        mov rbp, rsp // Fix rbp.
        push rax // rbp-8h.
        push rcx // rbp-10h.
        lea rax, [rbp+18h] // The context parameter.
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

} // namespace zpp::x64
