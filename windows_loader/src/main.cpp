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
    // Pinning this thread is how the hypervisor is entered on a chosen
    // processor here: vmxon and vmlaunch act on whichever processor
    // executes them, so the launch is worthless unless it runs on the one
    // the caller named. Restored afterwards because the thread belongs to
    // the operating system, not to this driver.
    KAFFINITY previous{};
    previous = KeSetSystemAffinityThreadEx(1ull << cpuid);

    int result = function(context);

    KeRevertToUserAffinityThreadEx(previous);

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
invoke_entry(int (*)(std::size_t, const zpp_launch_parameters *),
             std::size_t,
             const zpp_launch_parameters *)
{
    // Two arguments now rather than three, because everything else the
    // hypervisor is handed moved into the structure the second one
    // points at. That is the point of the structure: this adapter is
    // hand written assembly in two loaders, and it no longer has to
    // change when the hypervisor needs to be told something new.
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

    // Everything the platform has to supply. Designated initializers
    // throughout, so a field added to the structure shows up here as a
    // name rather than as a shifted position.
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
        .diagnostic_channel = nullptr,
        .sleep_control_port = 0,
        .sleep_control_port_secondary = 0,
        .sleep_control_width = 0,
        .sleep_facs_physical = 0,
        // Nor this, for the same reason. A guest's broadcast start-up IPI
        // names processors this loader has already launched the
        // hypervisor on, so every one of them is known by the time one
        // arrives and there is nothing left for a roster to answer.
        .processor_apic_ids = nullptr,
        .number_of_processor_apic_ids = 0,
        .adjust_launch_calling_convention = invoke_entry,
    };

    auto result = zpp_load_elf(&parameters);

    // Flattened, because a driver entry point's return value has to be an
    // NTSTATUS and the hypervisor's own error codes are not. The UEFI
    // loader is the one that prints them unflattened.
    if (result) {
        return STATUS_INTERNAL_ERROR;
    }

    // A failure on success, deliberately: the hypervisor is resident in
    // its own allocation and no longer needs this driver, so failing the
    // load is how the driver gets unloaded again. The two failures are
    // distinct codes so that the outcome is still readable from outside.
    return STATUS_INSUFFICIENT_POWER;
}
