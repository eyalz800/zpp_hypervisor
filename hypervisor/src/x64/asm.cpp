namespace zpp::x64
{
extern "C" void __attribute__((naked)) zpp_x64_capture_context_into_stack()
{
    asm(R"!!(
        .intel_syntax noprefix
        // Start of context structure is at rbp+0x18.
        // Return address is at rbp+0x10.
        pushfq // rflags is at rbp+0x8.
        push rbp // rbp is at rbp.
        mov rbp, rsp // Fix rbp.
        push rax // rbp-0x8.
        push rcx // rbp-0x10.
        lea rax, [rbp+0x18] // The context parameter.
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

} // namespace zpp::x64
