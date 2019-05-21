extern "C" {
#include <Uefi.h>
}
extern "C" {
#include <Pi/PiMultiPhase.h>
}
extern "C" {
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePathUtilities.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/MpService.h>
}
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * The boot services.
 */
static EFI_BOOT_SERVICES * g_boot_services{};

/**
 * The MP services.
 */
static EFI_MP_SERVICES_PROTOCOL * g_mp_services{};

/**
 * EFI Guids.
 * @{
 */
static EFI_GUID g_efi_block_io_protocol_guid = {
    0x964E5B21,
    0x6459,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static EFI_GUID g_efi_loaded_image_protocol_guid = {
    0x5B1B31A1,
    0x9562,
    0x11D2,
    {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static EFI_GUID g_efi_device_path_protocol_guid = {
    0x09576E91,
    0x6D3F,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static EFI_GUID g_efi_device_path_utilities_protocol_guid = {
    0x0379BE4E,
    0xD706,
    0x437D,
    {0xB0, 0x37, 0xED, 0xB8, 0x2F, 0xB7, 0x72, 0xA4}};

static EFI_GUID g_efi_mp_service_protocol_guid = {
    0x3fdda605,
    0xa76e,
    0x4f46,
    {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}};
/**
 * @}
 */

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
    EFI_PHYSICAL_ADDRESS physical_address{};

    // Allocate pages just enough for 'size' bytes.
    auto status = g_boot_services->AllocatePages(
        AllocateAnyPages,
        EfiRuntimeServicesCode,
        (size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE,
        &physical_address);

    // If not success, return nullptr;
    if (EFI_ERROR(status)) {
        return nullptr;
    }

    // Return the result address.
    return reinterpret_cast<void *>(physical_address);
}

static std::size_t number_of_cpus()
{
    std::size_t cpu_count{};
    std::size_t enabled_cpu_count{};

    // Get the number of processors.
    auto status = g_mp_services->GetNumberOfProcessors(
        g_mp_services, &cpu_count, &enabled_cpu_count);
    if (EFI_ERROR(status)) {
        return 0;
    }

    return cpu_count;
}

static int call_on_cpu(std::size_t cpuid,
                       int (*function)(void *),
                       void * context)
{
    int result = -1;

    // If this is the main CPU, just call the user function.
    if (0 == cpuid) {
        return function(context);
    }

    // The event we will wait for to join the new started AP.
    EFI_EVENT join_event{};
    std::size_t event_index{};

    // The launch function.
    auto launch = [&] {
        // Call the user function and save the result.
        result = function(context);

        // Signal the event.
        g_boot_services->SignalEvent(join_event);
    };

    // Erased launch function.
    auto erased_launch = [](void * parameter) {
        auto & local_launch = *static_cast<decltype(launch) *>(parameter);
        return local_launch();
    };

    // Create the join event.
    auto status =
        g_boot_services->CreateEvent(0, 0, nullptr, nullptr, &join_event);
    if (EFI_ERROR(status)) {
        return result;
    }

    // Startup the relevant CPU.
    status = g_mp_services->StartupThisAP(
        g_mp_services,
        static_cast<void (*)(void *)>(erased_launch),
        cpuid,
        nullptr,
        0,
        &launch,
        nullptr);
    if (EFI_ERROR(status)) {
        goto close_event;
    }

    // Wait for the join event.
    status = g_boot_services->WaitForEvent(1, &join_event, &event_index);
    if (EFI_ERROR(status)) {
        goto close_event;
    }

    // Result was changed by the other core.
close_event:
    // Close the event, and return the result.
    g_boot_services->CloseEvent(join_event);
    return result;
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

static EFI_DEVICE_PATH * file_device_path(EFI_HANDLE device,
                                          const char16_t * file_name)
{
    EFI_STATUS status{};

    // Locate the device path utilities protocol.
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL * device_path_utilities{};
    status = g_boot_services->LocateProtocol(
        &g_efi_device_path_utilities_protocol_guid,
        nullptr,
        reinterpret_cast<void **>(&device_path_utilities));
    if (EFI_ERROR(status)) {
        return nullptr;
    }

    // Compute the file name size.
    std::size_t file_name_size =
        std::char_traits<char16_t>::length(file_name) * sizeof(char16_t);

    // Compute the file path device path size.
    std::size_t file_path_device_path_size = file_name_size +
                                             SIZE_OF_FILEPATH_DEVICE_PATH +
                                             sizeof(EFI_DEVICE_PATH);

    // Allocate memory for the file path device path.
    FILEPATH_DEVICE_PATH * file_path_device_path{};
    status = g_boot_services->AllocatePool(
        EfiBootServicesData,
        file_path_device_path_size,
        reinterpret_cast<void **>(&file_path_device_path));
    if (EFI_ERROR(status)) {
        return nullptr;
    }

    // Zero the file path device path.
    std::memset(file_path_device_path,
                0,
                file_name_size + SIZE_OF_FILEPATH_DEVICE_PATH +
                    sizeof(EFI_DEVICE_PATH));

    // Initialize file path device path.
    file_path_device_path->Header.Type = MEDIA_DEVICE_PATH;
    file_path_device_path->Header.SubType = MEDIA_FILEPATH_DP;
    file_path_device_path->Header.Length[0] =
        ((file_name_size + SIZE_OF_FILEPATH_DEVICE_PATH) & 0xff);
    file_path_device_path->Header.Length[1] =
        ((file_name_size + SIZE_OF_FILEPATH_DEVICE_PATH) >> 8);
    std::memcpy(
        file_path_device_path->PathName, file_name, file_name_size);

    // Compute the device path end.
    auto * end_of_device_path = reinterpret_cast<EFI_DEVICE_PATH *>(
        reinterpret_cast<std::uintptr_t>(&file_path_device_path->Header) +
        file_path_device_path->Header.Length[0] +
        (file_path_device_path->Header.Length[1] << 8));
    end_of_device_path->Type = END_DEVICE_PATH_TYPE;
    end_of_device_path->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end_of_device_path->Length[0] = sizeof(EFI_DEVICE_PATH);
    end_of_device_path->Length[1] = 0;

    // Fetch the device path.
    auto device_path =
        reinterpret_cast<EFI_DEVICE_PATH *>(file_path_device_path);

    // If device was not specified, return the device path as is.
    if (!device) {
        return device_path;
    }

    // Convert device handle to path.
    EFI_DEVICE_PATH device_path_from_handle{};
    status = g_boot_services->HandleProtocol(
        device,
        &g_efi_device_path_protocol_guid,
        reinterpret_cast<void **>(&device_path_from_handle));
    if (EFI_ERROR(status)) {
        device_path = nullptr;
        goto free_file_path_device_path;
    }

    // Build the full path from device and path.
    device_path = device_path_utilities->AppendDevicePath(
        &device_path_from_handle, device_path);

free_file_path_device_path:
    // Free file path device path.
    g_boot_services->FreePool(file_path_device_path);

    return device_path;
}

extern "C" EFI_STATUS EFIAPI uefi_main(EFI_HANDLE image_handle,
                                       EFI_SYSTEM_TABLE * system_table)
{
    EFI_STATUS status{};

    // Copy the boot services.
    g_boot_services = system_table->BootServices;

    // Load the MP Services.
    status = g_boot_services->LocateProtocol(
        &g_efi_mp_service_protocol_guid,
        nullptr,
        reinterpret_cast<void **>(&g_mp_services));
    if (EFI_ERROR(status)) {
        return EFI_LOAD_ERROR;
    }

    // Load the ELF.
    auto result = zpp_load_elf(
        allocate_rwx, nullptr, call_on_cpu, number_of_cpus, invoke_entry);

    // If we failed, return an arbitrary failure.
    if (result) {
        return EFI_LOAD_ERROR;
    }

    // Continue to the OS.

    // Locate file system handles.
    EFI_HANDLE * file_system_handles{};
    std::size_t number_of_file_system_handles{};
    status =
        g_boot_services->LocateHandleBuffer(ByProtocol,
                                            &g_efi_block_io_protocol_guid,
                                            nullptr,
                                            &number_of_file_system_handles,
                                            &file_system_handles);
    if (EFI_ERROR(status)) {
        return EFI_LOAD_ERROR;
    }

    // Iterate all file systems.
    for (std::size_t i{}; i < number_of_file_system_handles; ++i) {
        // Find the block IO from the handle.
        EFI_BLOCK_IO * block_io{};
        status = g_boot_services->HandleProtocol(
            file_system_handles[i],
            &g_efi_block_io_protocol_guid,
            reinterpret_cast<void **>(&block_io));
        if (EFI_ERROR(status)) {
            continue;
        }

        // Get the full path to 'bootx64.efi' inside the specified file
        // system.
        auto file_path = file_device_path(file_system_handles[i],
                                          u"\\EFI\\BOOT\\bootx64.efi");
        if (!file_path) {
            return EFI_LOAD_ERROR;
        }

        // Load the image from the specified path.
        EFI_HANDLE current_image_handle{};
        status = g_boot_services->LoadImage(false,
                                            image_handle,
                                            file_path,
                                            nullptr,
                                            0,
                                            &current_image_handle);

        // Free the file path.
        g_boot_services->FreePool(file_path);

        // If failed, continue to another file system.
        if (EFI_ERROR(status)) {
            continue;
        }

        // Get loaded image info.
        EFI_LOADED_IMAGE_PROTOCOL * image_info{};
        status = g_boot_services->HandleProtocol(
            current_image_handle,
            &g_efi_loaded_image_protocol_guid,
            reinterpret_cast<void **>(&image_info));

        // If we had an error, or the image is not an EFI loader code,
        // continue.
        if (EFI_ERROR(status) ||
            image_info->ImageCodeType != EfiLoaderCode) {
            continue;
        }

        // Start the image.
        status = g_boot_services->StartImage(
            current_image_handle, nullptr, nullptr);

        // Return the start image status.
        return status;
    }

    // Return success anyway, no image was found is considered ok.
    return EFI_SUCCESS;
}
