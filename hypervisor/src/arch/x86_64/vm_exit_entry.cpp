#include "zpp/arch/x86_64/vm_exit_entry.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/generic.h"

namespace zpp::arch::x86_64
{
extern "C" void zpp_vm_exit(arch::x86_64::context & host_context)
{
    arch::x86_64::restore_context(&host_context);
}

// Naked because a VM exit loads only the host-state fields, so the
// processor arrives still holding the guest's registers.
void __attribute__((naked)) vm_exit_entry()
{
    asm(R"!!(
        .intel_syntax noprefix
        call zpp_x86_64_capture_context_into_stack // Capture guest context.
        lea rdi, [rsp+0x3a0] // The host context vm_launch put above it.
        sub rsp, 0x8 // Align stack to expected value.
        jmp zpp_vm_exit // Jump to the vm exit function.
    )!!");
}

} // namespace zpp::arch::x86_64
