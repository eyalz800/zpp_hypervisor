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
#include "zpp/loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

// False when the firmware's timed waits cannot be trusted, which makes MP
// services unusable - see acpi_timer_advancing.
static bool g_timed_waits_usable = true;

#if ZPP_CI_VERIFY_HYPERVISOR
/**
 * Writes a string straight to the first serial port, bypassing UEFI
 * console services. OVMF does not necessarily route ConOut to serial, and
 * with no video device there may be nowhere else for it to go, so this is
 * the channel that can be relied on to reach Bochs' serial capture.
 */
static void serial_write(const char * text)
{
    constexpr std::uint16_t port = 0x3f8;
    constexpr std::uint16_t line_status = port + 5;
    constexpr std::uint8_t transmitter_empty = 1u << 5;

    for (auto * character = text; *character; ++character) {
        // Wait for the transmit holding register to drain. Without this
        // only the first sixteen characters ever appear - that is the
        // 16550's FIFO depth, and anything written past a full FIFO is
        // dropped. Bounded, so a machine with no working UART cannot wedge
        // the loader here.
        for (std::uint32_t attempt{}; attempt < 100000u; ++attempt) {
            std::uint8_t status{};
            asm volatile("inb %1, %0" : "=a"(status) : "Nd"(line_status));
            if (status & transmitter_empty) {
                break;
            }
        }

        asm volatile("outb %0, %1"
                     :
                     : "a"(static_cast<std::uint8_t>(*character)),
                       "Nd"(port));
    }
}
#endif

#if ZPP_CI_VERIFY_HYPERVISOR
/**
 * Writes a value as hex over serial. The hypervisor is loaded at a runtime
 * chosen address, so this is how a debugger is told where to place its
 * symbols.
 */
static void serial_write_hex(std::uint64_t value)
{
    char text[19]{};
    text[0] = '0';
    text[1] = 'x';
    for (std::size_t i{}; i < 16; ++i) {
        auto nibble =
            static_cast<std::uint8_t>((value >> ((15 - i) * 4)) & 0xf);
        text[2 + i] = static_cast<char>(
            nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10)));
    }
    serial_write(text);
}
#endif

static void * allocate_rwx(std::size_t size)
{
#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE allocate_rwx enter\r\n");
#endif
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

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE allocate_rwx done at ");
    serial_write_hex(physical_address);
    serial_write("\r\n");
#endif

    // Return the result address.
    return reinterpret_cast<void *>(physical_address);
}

/**
 * Returns true when the ACPI power management timer is present and
 * advancing.
 *
 * This matters because UEFI's microsecond delay is built on that timer,
 * and MP services uses timed waits to enumerate and start application
 * processors. On a machine where the timer never advances, those waits do
 * not fail - they spin forever, so GetNumberOfProcessors simply never
 * returns and the loader hangs with no diagnostic. Bochs is exactly such a
 * machine: it provides no PIIX4 power management function, so the firmware
 * ends up polling a port that always reads back all ones.
 *
 * The address comes from the FADT rather than from a fixed chipset
 * location, so this stays correct on real hardware regardless of chipset.
 */
static bool acpi_timer_advancing(EFI_SYSTEM_TABLE * system_table)
{
    constexpr std::uint64_t acpi_20_guid_data1 = 0x8868e871;
    constexpr std::size_t fadt_pm_timer_block_offset = 76;

    auto read_port = [](std::uint16_t port) {
        std::uint32_t value{};
        asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    };

    // Locate the ACPI 2.0 root pointer among the configuration tables.
    const unsigned char * rsdp{};
    for (std::size_t i{}; i < system_table->NumberOfTableEntries; ++i) {
        auto & entry = system_table->ConfigurationTable[i];
        if (entry.VendorGuid.Data1 == acpi_20_guid_data1) {
            rsdp = static_cast<const unsigned char *>(entry.VendorTable);
            break;
        }
    }
    if (!rsdp) {
        return false;
    }

    // XSDT address lives at offset 24 of the root pointer.
    std::uint64_t xsdt_address{};
    std::memcpy(&xsdt_address, rsdp + 24, sizeof(xsdt_address));
    if (!xsdt_address) {
        return false;
    }

    auto * xsdt = reinterpret_cast<const unsigned char *>(xsdt_address);
    std::uint32_t xsdt_length{};
    std::memcpy(&xsdt_length, xsdt + 4, sizeof(xsdt_length));
    if (xsdt_length <= 36) {
        return false;
    }

    // Walk the XSDT entries looking for the fixed ACPI description table.
    auto entries = (xsdt_length - 36) / sizeof(std::uint64_t);
    for (std::size_t i{}; i < entries; ++i) {
        std::uint64_t table_address{};
        std::memcpy(&table_address,
                    xsdt + 36 + (i * sizeof(std::uint64_t)),
                    sizeof(table_address));
        if (!table_address) {
            continue;
        }

        auto * table =
            reinterpret_cast<const unsigned char *>(table_address);
        if (std::memcmp(table, "FACP", 4)) {
            continue;
        }

        std::uint32_t timer_block{};
        std::memcpy(&timer_block,
                    table + fadt_pm_timer_block_offset,
                    sizeof(timer_block));
        if (!timer_block || (0xffffffffu == timer_block)) {
            return false;
        }

        // Present is not the same as working, so require it to actually
        // move.
        auto port = static_cast<std::uint16_t>(timer_block);
        auto first = read_port(port);
        if (0xffffffffu == first) {
            return false;
        }
        for (std::uint32_t attempt{}; attempt < 1000000u; ++attempt) {
            if (read_port(port) != first) {
                return true;
            }
        }
        return false;
    }

    return false;
}

static std::size_t number_of_cpus()
{
    // Without a working timer, MP services would hang rather than fail, so
    // report the boot processor alone and let the hypervisor come up on
    // it.
    if (!g_timed_waits_usable) {
        return 1;
    }

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE number_of_cpus enter\r\n");
#endif
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

    // Any other CPU needs MP services, which cannot be used without
    // working timed waits. Refuse rather than hang.
    if (!g_timed_waits_usable) {
        return -1;
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

#if ZPP_CI_VERIFY_HYPERVISOR
/**
 * cpuid and port output written with register constraints rather than
 * reused from zpp/x64/asm.h. Those are naked functions that read their
 * arguments from the System V registers, while this loader is built for
 * the Microsoft ABI, where the third argument arrives in r8 rather than
 * rdx - reusing them here would store the results through the second
 * argument's value instead of a pointer. Letting the compiler allocate
 * registers avoids the question.
 * @{
 */
static void query_cpuid(std::uint32_t leaf, std::uint32_t (&out)[4])
{
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    std::uint32_t d{};

    asm volatile("cpuid"
                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(leaf), "c"(0u));

    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

/**
 * @}
 */

/**
 * Asks the hypervisor to identify itself, from whichever CPU this runs on.
 *
 * The vmexit handler answers CPUID leaf 0x40000000 with the signature
 * ZppZppZppZpp and sets the hypervisor present bit in leaf 1. Neither can
 * happen unless vmxon, the VMCS setup, vmlaunch, the exit handler and
 * vmresume all worked on this CPU, so one check covers the whole path end
 * to end.
 */
static int verify_hypervisor_on_cpu(void *)
{
    std::uint32_t registers[4]{};

    // Leaf 1, bit 31 of ecx: a hypervisor is present.
    query_cpuid(1, registers);
    if (!(registers[2] & (1u << 31))) {
        return -1;
    }

    // Leaf 0x40000000: the vendor signature, in ebx, ecx then edx.
    query_cpuid(1u << 30, registers);
    if ((registers[1] != 0x5a70705a) || (registers[2] != 0x705a7070) ||
        (registers[3] != 0x70705a70)) {
        return -2;
    }

    return 0;
}

/**
 * Runs the check on every CPU, since the hypervisor is launched per CPU
 * and a failure on one is just as bad as a failure on all. Reports over
 * serial, and also through the UEFI console when one is present.
 */
static bool verify_hypervisor_present(EFI_SYSTEM_TABLE * system_table)
{
    // wchar_t rather than CHAR16 in the parameter: a L"" literal is
    // wchar_t here, and EDK2's CHAR16 is unsigned short, so they need a
    // cast between them even though both are 16 bit on this target.
    auto report = [&](const char * ascii, const wchar_t * wide) {
        serial_write(ascii);
        if (system_table->ConOut) {
            system_table->ConOut->OutputString(
                system_table->ConOut,
                reinterpret_cast<CHAR16 *>(const_cast<wchar_t *>(wide)));
        }
    };

    auto cpus = number_of_cpus();
    if (!cpus) {
        report("zpp: ZPP_HYPERVISOR_FAILED no cpus\r\n",
               L"zpp: ZPP_HYPERVISOR_FAILED no cpus\r\n");
        return false;
    }

    for (std::size_t i{}; i < cpus; ++i) {
        if (call_on_cpu(i, verify_hypervisor_on_cpu, nullptr)) {
            report("zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n",
                   L"zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n");
            return false;
        }
    }

    report("zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n",
           L"zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n");
    return true;
}
#endif

extern "C" EFI_STATUS EFIAPI uefi_main(EFI_HANDLE image_handle,
                                       EFI_SYSTEM_TABLE * system_table)
{
    EFI_STATUS status{};

    // Copy the boot services.
    g_boot_services = system_table->BootServices;

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE entry\r\n");
#endif

    // Establish whether timed waits work before touching MP services,
    // since a dead timer makes them hang rather than return an error.
    g_timed_waits_usable = acpi_timer_advancing(system_table);
#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write(
        g_timed_waits_usable
            ? "zpp: ZPP_TRACE timed waits usable\r\n"
            : "zpp: ZPP_TRACE timed waits unusable, single cpu\r\n");
#endif

    // Load the MP Services.
    status = g_boot_services->LocateProtocol(
        &g_efi_mp_service_protocol_guid,
        nullptr,
        reinterpret_cast<void **>(&g_mp_services));
    if (EFI_ERROR(status)) {
#if ZPP_CI_VERIFY_HYPERVISOR
        serial_write(
            "zpp: ZPP_HYPERVISOR_FAILED no EFI_MP_SERVICES_PROTOCOL\r\n");
#endif
        return EFI_LOAD_ERROR;
    }

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE mp services located\r\n");
#endif

    // Load the ELF.
    const zpp_loader_parameters parameters{
        .allocate_rwx = allocate_rwx,
        .physical_to_virtual = nullptr,
        .call_on_cpu = call_on_cpu,
        .number_of_cpus = number_of_cpus,
        .adjust_launch_calling_convention = invoke_entry,
    };

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE loading\r\n");
#endif

    auto result = zpp_load_elf(&parameters);

#if ZPP_CI_VERIFY_HYPERVISOR
    serial_write("zpp: ZPP_TRACE loaded\r\n");
#endif

    // If we failed, return an arbitrary failure.
    if (result) {
#if ZPP_CI_VERIFY_HYPERVISOR
        // The code is the hypervisor's own error enumeration, so print it
        // - it is the only thing that says which step failed, and a
        // debugger is not always available where this runs.
        serial_write(
            "zpp: ZPP_HYPERVISOR_FAILED zpp_load_elf failed, code ");
        serial_write_hex(static_cast<std::uint64_t>(result));
        serial_write("\r\n");
#endif
        return EFI_LOAD_ERROR;
    }

#if ZPP_CI_VERIFY_HYPERVISOR
    // Ask the hypervisor to identify itself now that it should be live.
    // Built only for automated testing, so a normal loader does not carry
    // it.
    if (!verify_hypervisor_present(system_table)) {
        return EFI_LOAD_ERROR;
    }

    // Stop here rather than continuing to the OS. Under test the boot
    // medium holds only this loader, so there is nothing to chain to and
    // that path could only fail - and a failure there would discard a
    // result that has already been established, making the test look like
    // a hypervisor problem when it is not.
    return EFI_SUCCESS;
#endif

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
