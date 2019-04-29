#include "zpp/hypervisor/state.h"
#include "zpp/x64/context.h"

namespace zpp::hypervisor
{
#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
extern "C" bool gdb_attached = false;
#endif

extern "C" void zpp_hypervisor_main(x64::context & caller_context)
{
#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
    while (!gdb_attached) {
        asm("pause");
    }
#endif

    // Create the state once.
    state::create_once();

    // Launch hypervisor on CPU, does not return.
    g_state.hypervisor.launch_on_cpu(caller_context);
}

extern "C" void __attribute__((naked)) _start()
{
    asm(R"!!(
        .intel_syntax noprefix
        sub rsp, 3a8h // Make space for capture context and align to 16 bytes.
        call zpp_x64_capture_context_into_stack // Capture the context.
        mov rax, rsp // Get a pointer to the context to change rip and rsp.
        mov r10, [rsp+3a8h] // Fetch return address from the stack into r10.
        mov [rax+80h], r10 // Set context->rip to the return address.
        lea r10, [rsp+3b0h] // Compute the stack pointer at function return.
        mov [rax+20h], r10 // Set context->rsp to the stack pointer after this call.
        mov rdi, rax // Send context pointer as the main function parameter. 
        sub rsp, 8h // Align stack to expected value.
        jmp zpp_hypervisor_main // Jump to main function.
    )!!");
}

} // namespace zpp::hypervisor
