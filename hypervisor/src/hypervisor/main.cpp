#include "zpp/crt.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/x64/context.h"

#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
// Only the debugger wait loop below spins, and it is compiled out by
// default.
#include "zpp/spin_lock.h"
#endif

namespace zpp::hypervisor
{
#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
extern "C" bool gdb_attached = false;
#endif

extern "C" void zpp_hypervisor_main(x64::context & caller_context)
{
#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
    while (!gdb_attached) {
        zpp::spin_hint();
    }
#endif

    // Initialize the C runtime before anything can use a global.
    crt::init::main();

    // Launch hypervisor on CPU, does not return.
    hypervisor::instance().launch_on_cpu(caller_context);
}

extern "C" void __attribute__((naked)) _start()
{
    asm(R"!!(
        .intel_syntax noprefix
        sub rsp, 0x3a8 // Make space for capture context and align to 16 bytes.
        call zpp_x64_capture_context_into_stack // Capture the context.
        mov rax, rsp // Get a pointer to the context to change rip and rsp.
        mov r10, [rsp+0x3a8] // Fetch return address from the stack into r10.
        mov [rax+0x80], r10 // Set context->rip to the return address.
        lea r10, [rsp+0x3b0] // Compute the stack pointer at function return.
        mov [rax+0x20], r10 // Set context->rsp to the stack pointer after this call.
        mov rdi, rax // Send context pointer as the main function parameter.
        sub rsp, 0x8 // Align stack to expected value.
        jmp zpp_hypervisor_main // Jump to main function.
    )!!");
}

} // namespace zpp::hypervisor
