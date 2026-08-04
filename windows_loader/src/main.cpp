#include "zpp/loader.h"
#include <cstddef>
#include <cstdint>
#include <ntddk.h>
#include <ntstatus.h>
#include <wdm.h>

static void * allocate_rwx(std::size_t size)
{
    return ExAllocatePool(NonPagedPoolExecute, size);
}

static std::size_t number_of_cpus()
{
    KAFFINITY affinity{};
    return KeQueryActiveProcessorCount(&affinity);
}

static int call_on_cpu(std::size_t cpuid,
                       int (*function)(void *),
                       void * context)
{
    // The previous affinity.
    KAFFINITY previous{};

    // Set new affinity to only given cpuid.
    previous = KeSetSystemAffinityThreadEx(1ull << cpuid);

    // Call user function.
    int result = function(context);

    // Restore previous affinity.
    KeRevertToUserAffinityThreadEx(previous);

    // Return the result.
    return result;
}

extern "C" std::uintptr_t
zpp_windows_loader_physical_to_virtual(std::uintptr_t value)
{
    PHYSICAL_ADDRESS physical_address{};
    physical_address.QuadPart = value;
    return reinterpret_cast<std::uintptr_t>(
        MmGetVirtualForPhysical(physical_address));
}

static std::uintptr_t __attribute__((naked))
invoke_physical_to_virtual(std::uintptr_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov rcx, rdi // Forward the address parameter.
        sub rsp, 0x28 // Make enough room for shadow space and align.
        call zpp_windows_loader_physical_to_virtual // Invoke function.
        add rsp, 0x28 // Restore stack.
        ret // Return.
    )!!");
}

static int __attribute__((naked)) invoke_entry(
    int (*)(
        std::size_t, std::uintptr_t (*)(std::uintptr_t), void *, void *),
    std::size_t,
    std::uintptr_t (*)(std::uintptr_t),
    void *,
    void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        push rdi // Save rdi before use as it is non-volatile.
        push rsi // Save rsi before use as it is non-volatile.
        mov r10, rcx // Keep the function pointer, rcx is a parameter now.
        mov rdi, rdx // Forward first parameter to function.
        mov rsi, r8 // Forward second parameter to function.
        mov rdx, r9 // Forward third parameter, after rdx has been read.
        mov rcx, [rsp+0x38] // Fourth parameter, the fifth argument here:
        // eight bytes of return address plus thirty two of register spill
        // area, past the two pushes above.
        sub rsp, 0x8 // Align stack to 16 bytes.
        call r10 // Call the function pointer.
        add rsp, 0x8 // Restore stack.
        pop rsi // Restore rsi.
        pop rdi // Restore rdi.
        ret // Return.
    )!!");
}

extern "C" NTAPI NTSTATUS driver_entry(PDRIVER_OBJECT driver_object,
                                       PUNICODE_STRING)
{
    driver_object->DriverUnload = [](PDRIVER_OBJECT) {};

    // Load the ELF.
    const zpp_loader_parameters parameters{
        .allocate_rwx = allocate_rwx,
        .physical_to_virtual = invoke_physical_to_virtual,
        .call_on_cpu = call_on_cpu,
        .number_of_cpus = number_of_cpus,
        // Not supplied, because nothing here needs it: the hypervisor is
        // launched on every processor from this loader, all of them
        // already running under the operating system, so it never has to
        // start one itself.
        .allocate_below_one_megabyte = nullptr,
        // None either. This runs with an operating system already up,
        // which has its own means of keeping a log across a restart, and
        // claiming a fixed physical address behind its back is not one of
        // them.
        .crash_log_memory = nullptr,
        .adjust_launch_calling_convention = invoke_entry,
    };

    auto result = zpp_load_elf(&parameters);

    // If we failed, return an arbitrary failure.
    if (result) {
        return STATUS_INTERNAL_ERROR;
    }

    // Success, return a failure so the OS unloads us.
    return STATUS_INSUFFICIENT_POWER;
}
