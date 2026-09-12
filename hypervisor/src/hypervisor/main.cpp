#include "zpp/arch/x86_64/context.h"
#include "zpp/crt.h"
#include "zpp/hypervisor/hypervisor.h"

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

extern "C" void zpp_hypervisor_main(arch::x86_64::context & caller_context)
{
#if ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
    while (!gdb_attached) {
        zpp::spin_hint();
    }
#endif

    // Brings the heap up and then walks the init arrays, in that order,
    // so a constructor is free to allocate. The call below constructs
    // the singleton on first use, so nothing may precede this line.
    crt::init::main();

    // Never returns: every path out of launch_on_cpu ends in
    // restore_context, which loads a context rather than returning. On
    // success that context is the captured one, entered as the guest.
    hypervisor::instance().launch_on_cpu(caller_context);
}

// RIP and RSP are rewritten to describe the state at the *exit* from
// this function rather than the entry to it, so launching the guest on
// the captured context lands it on the loader's next instruction and
// virtualization is invisible.
extern "C" void __attribute__((naked)) _start()
{
    asm(R"!!(
        .intel_syntax noprefix
        sub rsp, 0x3a8 // Make space for capture context and align to 16 bytes.
        call zpp_x86_64_capture_context_into_stack // Capture the context.
        mov rax, rsp // Get a pointer to the context to change rip and rsp.
        mov r10, [rsp+0x3a8] // The return address, just above the context.
        mov [rax+0x80], r10 // Set context->rip to the return address.
        lea r10, [rsp+0x3b0] // The stack pointer a ret would leave behind.
        mov [rax+0x20], r10 // Set context->rsp to the stack pointer after this call.
        mov rdi, rax // Send context pointer as the main function parameter.
        sub rsp, 0x8 // Reserve the slot a call would have pushed.
        jmp zpp_hypervisor_main // Jump to main function.
    )!!");
}

} // namespace zpp::hypervisor
