#include <cstddef>
#include <cstdint>
#include <ntddk.h>
#include <ntstatus.h>
#include <wdm.h>

extern "C" int
zpp_load_elf(void * (*allocate_rwx)(std::size_t),
             std::uintptr_t (*physical_to_virtual)(std::uintptr_t),
             int (*call_on_cpu)(std::size_t, int (*)(void *), void *),
             std::size_t (*number_of_cpus)(void),
             int (*adjust_launch_calling_convention)(
                 int (*)(std::size_t, std::uintptr_t (*)(std::uintptr_t)),
                 std::size_t,
                 std::uintptr_t (*)(std::uintptr_t)));

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

static int __attribute__((naked))
invoke_entry(int (*)(std::size_t, std::uintptr_t (*)(std::uintptr_t)),
             std::size_t,
             std::uintptr_t (*)(std::uintptr_t))
{
    asm(R"!!(
        .intel_syntax noprefix
        push rdi // Save rdi before use as it is non-volatile.
        push rsi // Save rsi before use as it is non-volatile.
        mov rdi, rdx // Forward first parameter to function.
        mov rsi, r8 // Forward second parameter to function.
        sub rsp, 0x8 // Align stack to 16 bytes.
        call rcx // Call the function pointer.
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
    auto result = zpp_load_elf(allocate_rwx,
                               invoke_physical_to_virtual,
                               call_on_cpu,
                               number_of_cpus,
                               invoke_entry);

    // If we failed, return an arbitrary failure.
    if (result) {
        return STATUS_INTERNAL_ERROR;
    }

    // Success, return a failure so the OS unloads us.
    return STATUS_INSUFFICIENT_POWER;
}
