#include "zpp/x64/vm_exit_entry.h"
#include "zpp/x64/asm.h"
#include "zpp/x64/context.h"
#include "zpp/x64/generic.h"

namespace zpp::x64
{
extern "C" void zpp_vm_exit(x64::context & host_context)
{
    x64::restore_context(&host_context);
}

void __attribute__((naked)) vm_exit_entry()
{
    asm(R"!!(
        .intel_syntax noprefix
        call zpp_x64_capture_context_into_stack // Capture guest context.
        lea rdi, [rsp+0x3a0] // Move host context pointer to rdi.
        sub rsp, 0x8 // Align stack to expected value.
        jmp zpp_vm_exit // Jump to the vm exit function.
    )!!");
}

} // namespace zpp::x64
