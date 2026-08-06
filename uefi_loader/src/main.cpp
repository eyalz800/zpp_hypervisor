extern "C" {
#include <Uefi.h>
}
extern "C" {
#include <Pi/PiMultiPhase.h>
}
extern "C" {
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DevicePathUtilities.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/MpService.h>
#include <Protocol/SimpleFileSystem.h>
}
#include "zpp/loader.h"
#include "zpp/trace.h"
#include "zpp/verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string>

/**
 * The boot services.
 */
static EFI_BOOT_SERVICES * g_boot_services{};

/**
 * The runtime services, needed for the variable services that hold the
 * firmware's boot options.
 */
static EFI_RUNTIME_SERVICES * g_runtime_services{};

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

static EFI_GUID g_efi_simple_file_system_protocol_guid = {
    0x964E5B22,
    0x6459,
    0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static EFI_GUID g_efi_global_variable_guid = {
    0x8BE4DF61,
    0x93CA,
    0x11D2,
    {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};

static EFI_GUID g_efi_device_path_to_text_protocol_guid = {
    0x8B843E20,
    0x8132,
    0x4852,
    {0x90, 0xCC, 0x55, 0x1A, 0x4E, 0x4A, 0x7F, 0x1C}};

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

/**
 * Whether this build carries the hypervisor self check. The build system
 * always defines the macro, to 0 or 1, so this needs no preprocessor
 * fallback - and having it as a constant rather than a macro is what lets
 * everything below be ordinary code under `if constexpr`.
 */
// Unqualified, so the many call sites below stay readable.
using zpp::trace;
using zpp::verify;

/**
 * Traces a device path in the same text form the firmware's own boot
 * messages use, so what this loader hands to LoadImage can be compared
 * against the boot option the firmware would have used.
 *
 * Worth having permanently: a device path is the one parameter of the
 * chainload that cannot be checked by reading the code, because it is
 * assembled at runtime out of whatever the firmware enumerated.
 */
static void trace_device_path(const char * prefix, EFI_DEVICE_PATH * path)
{
    if constexpr (!trace::enabled) {
        static_cast<void>(prefix);
        static_cast<void>(path);
        return;
    }

    EFI_DEVICE_PATH_TO_TEXT_PROTOCOL * to_text{};
    if (EFI_ERROR(g_boot_services->LocateProtocol(
            &g_efi_device_path_to_text_protocol_guid,
            nullptr,
            reinterpret_cast<void **>(&to_text)))) {
        trace::line("ZPP_TRACE no device path to text protocol");
        return;
    }

    auto text = to_text->ConvertDevicePathToText(path, false, false);
    if (!text) {
        trace::line("ZPP_TRACE device path to text failed");
        return;
    }

    // Device path text is UTF-16 and the trace channel takes bytes. Every
    // character a device path uses is ASCII, so fold rather than encode,
    // and mark anything unexpected instead of dropping it silently. Sized
    // well under trace::line_capacity, since line() adds its own location
    // stamp to whatever it is given.
    char buffer[112]{};
    auto end = trace::append_text(buffer, prefix);
    auto limit = std::end(buffer) - 1;
    for (auto character = text; *character && (end < limit); ++character) {
        *end++ = (*character < 0x80) ? static_cast<char>(*character) : '?';
    }
    *end = 0;
    trace::line(buffer);

    g_boot_services->FreePool(text);
}

/**
 * Traces a UTF-16 string on the byte oriented trace channel, folding to
 * ASCII the same way trace_device_path does.
 */
static void trace_utf16(const char * prefix, const char16_t * text)
{
    if constexpr (!trace::enabled) {
        static_cast<void>(prefix);
        static_cast<void>(text);
        return;
    }

    char buffer[112]{};
    auto end = trace::append_text(buffer, prefix);
    auto limit = std::end(buffer) - 1;
    for (auto character = text; *character && (end < limit); ++character) {
        *end++ = (*character < 0x80) ? static_cast<char>(*character) : '?';
    }
    *end = 0;
    trace::line(buffer);
}

/**
 * The load options a boot manager has to be started with, borrowed from
 * the firmware's own boot option for it.
 *
 * Chainloading a boot manager is not only loading the file: the firmware's
 * boot option can carry optional data that it passes on as the started
 * image's load options. For Windows Boot Manager that data names the BCD
 * object the boot manager is to use for itself, as
 * BCDOBJECT={9dea862c-5cdd-4e70-acc1-f32b344d4795}.
 *
 * Forwarded verbatim rather than synthesized, because it is the platform's
 * own description of how that boot manager is meant to be started, and
 * nothing here needs to understand it in order to hand it back.
 *
 * Note that an option the firmware generated itself by scanning a file
 * system carries none of this - its description is just the file name -
 * so an empty result is the common case rather than a problem.
 */
struct boot_option_load_options
{
    void * data{};
    std::uint32_t size{};
};

/**
 * Returns just the file name out of a path.
 */
static const char16_t * path_file_name(const char16_t * path)
{
    auto name = path;
    for (auto character = path; *character; ++character) {
        if ((u'\\' == *character) || (u'/' == *character)) {
            name = character + 1;
        }
    }
    return name;
}

/**
 * Whether a UTF-16 path ends with the given file name, ignoring case.
 * Firmware and Windows disagree on the case of these paths, so a case
 * sensitive compare would never match.
 */
static bool path_ends_with(const char16_t * path, const char16_t * suffix)
{
    auto lower = [](char16_t value) -> char16_t {
        return ((value >= u'A') && (value <= u'Z'))
                   ? static_cast<char16_t>(value - u'A' + u'a')
                   : value;
    };

    auto path_length = std::char_traits<char16_t>::length(path);
    auto suffix_length = std::char_traits<char16_t>::length(suffix);
    if (path_length < suffix_length) {
        return false;
    }

    auto tail = path + (path_length - suffix_length);
    for (std::size_t i{}; i < suffix_length; ++i) {
        if (lower(tail[i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

/**
 * Reads one Boot#### option and, if it names the given boot manager,
 * returns the load options it carries.
 *
 * The allocation holding them is deliberately not freed on success: the
 * returned pointer points into it, and it has to stay live for as long as
 * the image that gets started, which never returns.
 */
static boot_option_load_options
read_boot_option(std::uint16_t number, const char16_t * boot_manager)
{
    // An option has no fixed layout, so it is parsed by walking it:
    //   UINT32 Attributes
    //   UINT16 FilePathListLength
    //   CHAR16 Description[]              (null terminated)
    //   EFI_DEVICE_PATH FilePathList[]    (FilePathListLength bytes)
    //   UINT8 OptionalData[]              (whatever is left)
    constexpr std::size_t attributes_size = sizeof(std::uint32_t);
    constexpr std::size_t file_path_list_length_size =
        sizeof(std::uint16_t);
    constexpr std::size_t description_offset =
        attributes_size + file_path_list_length_size;

    // Boot#### where #### is the option number in upper case hex.
    char16_t name[]{u"Boot0000"};
    for (std::size_t nibble{}; nibble < 4; ++nibble) {
        auto value = (number >> ((3 - nibble) * 4)) & 0xf;
        name[4 + nibble] = static_cast<char16_t>(
            value < 10 ? (u'0' + value) : (u'A' + value - 10));
    }

    // Options carry a description and a device path, so the size is not
    // known in advance. Ask for it, then allocate.
    std::size_t option_size{};
    if (EFI_BUFFER_TOO_SMALL !=
        g_runtime_services->GetVariable(reinterpret_cast<CHAR16 *>(name),
                                        &g_efi_global_variable_guid,
                                        nullptr,
                                        &option_size,
                                        nullptr)) {
        return {};
    }

    void * option{};
    if (EFI_ERROR(g_boot_services->AllocatePool(
            EfiLoaderData, option_size, &option))) {
        return {};
    }

    auto bytes = static_cast<unsigned char *>(option);
    auto keep = false;
    boot_option_load_options result{};

    if (!EFI_ERROR(g_runtime_services->GetVariable(
            reinterpret_cast<CHAR16 *>(name),
            &g_efi_global_variable_guid,
            nullptr,
            &option_size,
            option)) &&
        (option_size > description_offset)) {
        std::uint16_t file_path_list_length{};
        std::memcpy(&file_path_list_length,
                    bytes + attributes_size,
                    sizeof(file_path_list_length));

        // Walk the description to its terminator to find where the device
        // path begins.
        auto description =
            reinterpret_cast<const char16_t *>(bytes + description_offset);
        auto description_length =
            std::char_traits<char16_t>::length(description);
        auto device_path_offset =
            description_offset +
            ((description_length + 1) * sizeof(char16_t));
        auto optional_data_offset =
            device_path_offset + file_path_list_length;

        // A truncated or inconsistent option is skipped rather than
        // trusted - these are attacker reachable NVRAM contents.
        if ((device_path_offset < option_size) &&
            (optional_data_offset <= option_size)) {
            trace_utf16("ZPP_TRACE boot option ", description);

            // Find the file path node naming the boot manager. It is not
            // necessarily the last node, so walk to the end. The walk is
            // bounded by the option's own declared device path length
            // rather than by the end marker, so a malformed path cannot
            // walk off the allocation.
            auto matches = false;
            std::size_t node_offset{};
            while ((node_offset + sizeof(EFI_DEVICE_PATH)) <=
                   file_path_list_length) {
                auto node = reinterpret_cast<EFI_DEVICE_PATH *>(
                    bytes + device_path_offset + node_offset);

                // The node length is a two byte field rather than an
                // aligned integer, so it is assembled by hand.
                std::size_t node_length =
                    node->Length[0] |
                    (static_cast<std::size_t>(node->Length[1]) << 8);
                if ((node_length < sizeof(EFI_DEVICE_PATH)) ||
                    ((node_offset + node_length) >
                     file_path_list_length)) {
                    break;
                }
                if (END_DEVICE_PATH_TYPE == node->Type) {
                    break;
                }

                if ((MEDIA_DEVICE_PATH == node->Type) &&
                    (MEDIA_FILEPATH_DP == node->SubType)) {
                    auto file_name = reinterpret_cast<const char16_t *>(
                        bytes + device_path_offset + node_offset +
                        sizeof(EFI_DEVICE_PATH));
                    trace_utf16("ZPP_TRACE   file path ", file_name);
                    if (path_ends_with(file_name,
                                       path_file_name(boot_manager))) {
                        matches = true;
                        break;
                    }
                }

                node_offset += node_length;
            }

            if (matches && (optional_data_offset < option_size)) {
                result.data = bytes + optional_data_offset;
                result.size = static_cast<std::uint32_t>(
                    option_size - optional_data_offset);
                keep = true;
                trace::hex_line("ZPP_TRACE matched, load options bytes ",
                                result.size);
            }
        }
    }

    if (!keep) {
        g_boot_services->FreePool(option);
        return {};
    }

    return result;
}

/**
 * Finds the firmware's own boot option for the named boot manager and
 * returns the load options it would have started it with.
 *
 * Tries the options named in BootOrder first, so a machine with more than
 * one boot manager is chained to in the firmware's own order of
 * preference, then sweeps the low option numbers. BootOrder is not a
 * complete list: firmware regenerates and reorders it as devices come and
 * go, so an option can be present in NVRAM while absent from BootOrder -
 * which is exactly the state the Windows option was found in here.
 */
static boot_option_load_options
find_boot_option_load_options(const char16_t * boot_manager)
{
    if (!g_runtime_services) {
        return {};
    }

    std::uint16_t boot_order[128]{};
    std::size_t boot_order_size = sizeof(boot_order);
    if (EFI_ERROR(g_runtime_services->GetVariable(
            reinterpret_cast<CHAR16 *>(
                const_cast<char16_t *>(u"BootOrder")),
            &g_efi_global_variable_guid,
            nullptr,
            &boot_order_size,
            boot_order))) {
        trace::line("ZPP_TRACE no BootOrder variable");
        boot_order_size = 0;
    }

    for (std::size_t i{}; i < (boot_order_size / sizeof(std::uint16_t));
         ++i) {
        if (auto result = read_boot_option(boot_order[i], boot_manager);
            result.data) {
            return result;
        }
    }

    // Not in BootOrder, so sweep the low option numbers. Every option
    // number is possible in principle, but each probe is a variable store
    // lookup and sweeping all of them costs a noticeable part of the boot,
    // so this stops where real firmware and real installers stop.
    constexpr std::uint32_t highest_swept_option = 0xff;
    trace::line("ZPP_TRACE not in BootOrder, sweeping low options");
    for (std::uint32_t number{}; number <= highest_swept_option;
         ++number) {
        if (auto result = read_boot_option(
                static_cast<std::uint16_t>(number), boot_manager);
            result.data) {
            return result;
        }
    }

    return {};
}

/**
 * Whether a file exists on the file system of the given device handle.
 *
 * Used to tell a real boot partition from one that merely happens to
 * carry a copy of a boot manager. A boot manager needs the rest of its
 * installation beside it, so the presence of the executable alone is not
 * evidence that starting it will work.
 */
static bool file_exists(EFI_HANDLE device, const char16_t * path)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL * file_system{};
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            device,
            &g_efi_simple_file_system_protocol_guid,
            reinterpret_cast<void **>(&file_system)))) {
        return false;
    }

    EFI_FILE_PROTOCOL * volume{};
    if (EFI_ERROR(file_system->OpenVolume(file_system, &volume))) {
        return false;
    }

    EFI_FILE_PROTOCOL * file{};
    auto status = volume->Open(
        volume,
        &file,
        reinterpret_cast<CHAR16 *>(const_cast<char16_t *>(path)),
        EFI_FILE_MODE_READ,
        0);
    if (!EFI_ERROR(status)) {
        file->Close(file);
    }
    volume->Close(volume);

    return !EFI_ERROR(status);
}

/**
 * Writes everything traced so far to a file on the given device.
 *
 * This is the only diagnosis channel that survives on the development
 * target: it has no serial port, no debugger, and the screen belongs to
 * whatever gets booted next. Without this, a bare metal attempt that fails
 * leaves a blank screen and no reason.
 *
 * Called before handing control away and on the paths that give up, so the
 * log describes the attempt either way. Failure to write is ignored - it
 * is a diagnostic, and refusing to boot because the log could not be
 * saved would be worse than booting without one.
 */
/**
 * Traces which boot option started this loader, and what the firmware
 * thinks each of its processors is doing.
 *
 * Both exist because of a failure that looked intermittent for hours and
 * was not. Launched from the firmware's boot order the chainload works;
 * launched by the setup menu's boot override the same build stops dead
 * after the chainload line. Nothing in the log distinguished the two, so
 * every hypothesis was tested against a baseline that was moving for a
 * reason nobody had recorded.
 *
 * BootCurrent is the firmware's own statement of which option it is
 * running. An absent one is itself the answer: the firmware is starting
 * this image outside the boot order, which is what a boot override is.
 *
 * The processor states test the obvious suspicion about why that would
 * matter - that the setup environment has woken the application
 * processors, leaving them somewhere other than the wait-for-SIPI state
 * this VMM's whole adoption path assumes them to be parked in. MP
 * services is the only thing here that can see them, it is already
 * located, and asking costs nothing. Reading only: waking or borrowing
 * one is exactly what was removed from this loader.
 */
static void trace_launch_context()
{
    if constexpr (!trace::enabled) {
        return;
    }

    if (std::uint16_t current{}; g_runtime_services) {
        auto name = u"BootCurrent";
        std::size_t size = sizeof(current);
        if (EFI_ERROR(g_runtime_services->GetVariable(
                reinterpret_cast<CHAR16 *>(const_cast<char16_t *>(name)),
                &g_efi_global_variable_guid,
                nullptr,
                &size,
                &current))) {
            trace::line("ZPP_TRACE no BootCurrent, started outside the "
                        "boot order");
        } else {
            trace::hex_line("ZPP_TRACE started as boot option ", current);
        }
    }

    if (!g_mp_services) {
        return;
    }

    std::size_t total{};
    std::size_t enabled{};
    if (EFI_ERROR(g_mp_services->GetNumberOfProcessors(
            g_mp_services, &total, &enabled))) {
        trace::line("ZPP_TRACE processor count unavailable");
        return;
    }

    char buffer[trace::line_capacity]{};
    auto end = trace::append_text(buffer, "processors ");
    end = trace::append_decimal(end, total);
    end = trace::append_text(end, ", enabled ");
    end = trace::append_decimal(end, enabled);
    *end = 0;
    trace::line(buffer);

    // The status flags say whether the firmware considers a processor the
    // boot processor, enabled, and healthy. They do not name an activity
    // state - nothing outside the processor itself can - so a difference
    // between two launches is evidence rather than a diagnosis.
    for (std::size_t i{}; i < total; ++i) {
        EFI_PROCESSOR_INFORMATION information{};
        if (EFI_ERROR(g_mp_services->GetProcessorInfo(
                g_mp_services, i, &information))) {
            continue;
        }

        char line[trace::line_capacity]{};
        auto at = trace::append_text(line, "cpu ");
        at = trace::append_decimal(at, i);
        at = trace::append_text(at, " apic ");
        at = trace::append_hex(
            at, static_cast<std::uint64_t>(information.ProcessorId), 4);
        at = trace::append_text(at, " flags ");
        at = trace::append_hex(at, information.StatusFlag, 8);
        at = trace::append_text(at, information.StatusFlag & 0x1
                                        ? " bsp"
                                        : " application");
        at = trace::append_text(
            at, information.StatusFlag & 0x2 ? " enabled" : " disabled");
        at = trace::append_text(
            at, information.StatusFlag & 0x4 ? " healthy" : " unhealthy");
        *at = 0;
        trace::line(line);
    }
}

/**
 * The vendor GUID the trace variable lives under. Anything but the global
 * one, so nothing here can collide with a firmware variable.
 */
static EFI_GUID g_zpp_variable_guid = {
    0x7a1c9e42,
    0x3b8d,
    0x4f16,
    {0x9c, 0x5e, 0x11, 0x2d, 0x7f, 0x63, 0xa8, 0x04}};

/**
 * Saves the trace log into a non-volatile firmware variable.
 *
 * The file on the EFI system partition is the better copy - larger, and
 * readable without knowing anything about variables - but it needs a file
 * system, a directory, an open, a write and a flush, so it is written at
 * exactly one point in the boot. Every other way out of this loader
 * returned without writing anything at all, which is how a boot that
 * failed before the chainload came to leave no evidence whatsoever.
 *
 * A variable has neither problem. It needs no file system, so it can be
 * written from any failure path, and it survives a firmware that refuses
 * to give us memory or a disk we cannot reach.
 *
 * Runtime access deliberately included: that is what puts it under
 * efivarfs, so the log can be read from an operating system on the same
 * machine rather than only from here.
 *
 * Deliberately not called on a boot that succeeds without incident -
 * variable storage is finite and wears, so this is for the paths worth
 * recording.
 */
static void write_trace_variable()
{
    if constexpr (!trace::enabled) {
        return;
    }

    if (!g_runtime_services) {
        return;
    }

    auto log = trace::log();
    if (log.empty()) {
        return;
    }

    auto name = u"ZppTrace";
    g_runtime_services->SetVariable(
        reinterpret_cast<CHAR16 *>(const_cast<char16_t *>(name)),
        &g_zpp_variable_guid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        log.size(),
        const_cast<char *>(log.data()));
}

static void write_trace_log(EFI_HANDLE device)
{
    if constexpr (!trace::enabled) {
        static_cast<void>(device);
        return;
    }

    if (!device || trace::log().empty()) {
        return;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL * file_system{};
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            device,
            &g_efi_simple_file_system_protocol_guid,
            reinterpret_cast<void **>(&file_system)))) {
        return;
    }

    EFI_FILE_PROTOCOL * volume{};
    if (EFI_ERROR(file_system->OpenVolume(file_system, &volume))) {
        return;
    }

    // The directory has to exist before a file can be created in it, and
    // on a freshly prepared EFI system partition it does not. Opening it
    // as a directory with CREATE makes one if needed and finds the
    // existing one otherwise.
    EFI_FILE_PROTOCOL * directory{};
    auto directory_path = u"\\EFI\\zpp";
    if (!EFI_ERROR(
            volume->Open(volume,
                         &directory,
                         reinterpret_cast<CHAR16 *>(
                             const_cast<char16_t *>(directory_path)),
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                             EFI_FILE_MODE_CREATE,
                         EFI_FILE_DIRECTORY))) {
        directory->Close(directory);
    }

    EFI_FILE_PROTOCOL * file{};
    auto file_path = u"\\EFI\\zpp\\zpp_trace.log";
    if (EFI_ERROR(volume->Open(
            volume,
            &file,
            reinterpret_cast<CHAR16 *>(const_cast<char16_t *>(file_path)),
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                EFI_FILE_MODE_CREATE,
            0))) {
        volume->Close(volume);
        return;
    }

    // Append, so the file keeps every boot rather than only the last one.
    // Comparing two boots is the whole reason this file is read - a boot
    // that works against one that does not - and overwriting threw away
    // one half of every comparison.
    //
    // What has to be avoided is not the appending, it is appending
    // *unmarked*. Rewinding to offset zero and writing did not shorten the
    // file, and with no truncating open here a shorter log left the tail
    // of a longer one behind, reading as one boot because nothing said
    // where this boot's log stopped and an older one resumed. It was read
    // that way: a log appeared to have chainloaded twice and sampled its
    // processor states twice, and the only clue was a line beginning
    // mid-word - "9] ZPP_TRACE boot option zpp", the tail of an earlier
    // "[main.cpp:179]". So each section gets a banner, and every boot's
    // log begins with its own entry line underneath it.
    //
    // This function runs more than once in a boot - before handing over,
    // and again if the hand-over returns - so the offset this boot's
    // section starts at is remembered and rewritten from, rather than
    // appended to twice. Within a boot the log only ever grows, so
    // rewriting from that offset never leaves anything stale behind.
    static std::uint64_t section_offset = ~std::uint64_t{};
    static bool section_started = false;

    if (!section_started) {
        // Seeking to the maximum position is how the end of a file is
        // asked for here.
        file->SetPosition(file, ~std::uint64_t{});
        std::uint64_t end_position{};
        if (EFI_ERROR(file->GetPosition(file, &end_position))) {
            end_position = 0;
        }

        // Keep it from growing without limit. Starting over loses history,
        // which is the cost of not filling the partition; at a few
        // kilobytes a boot this is many tens of boots.
        constexpr std::uint64_t largest_kept = 512 * 1024;
        if (end_position > largest_kept) {
            file->Delete(file);
            file = nullptr;
            if (EFI_ERROR(volume->Open(
                    volume,
                    &file,
                    reinterpret_cast<CHAR16 *>(
                        const_cast<char16_t *>(file_path)),
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                        EFI_FILE_MODE_CREATE,
                    0))) {
                volume->Close(volume);
                return;
            }
            end_position = 0;
        }

        auto banner = "\r\n===== zpp boot =====\r\n";
        std::size_t banner_size = std::char_traits<char>::length(banner);
        file->SetPosition(file, end_position);
        file->Write(file, &banner_size, const_cast<char *>(banner));

        if (EFI_ERROR(file->GetPosition(file, &section_offset))) {
            section_offset = end_position;
        }
        section_started = true;
    }

    file->SetPosition(file, section_offset);

    auto log = trace::log();
    std::size_t size = log.size();
    file->Write(file, &size, const_cast<char *>(log.data()));

    // Flush before closing: the firmware's FAT driver caches, and a
    // machine that is about to hand over to an OS - or to hang - may never
    // get another chance to write this out.
    file->Flush(file);
    file->Close(file);
    volume->Close(volume);
}

static void * allocate_rwx(std::size_t size)
{
    trace::line("ZPP_TRACE allocate_rwx enter");
    EFI_PHYSICAL_ADDRESS physical_address{};

    // Reserved, not runtime services code, and the difference decides
    // whether this machine can resume from hibernation.
    //
    // Microsoft's UEFI firmware requirements state, for the S4 transition:
    // "firmware runtime memory must be consistent across S4 sleep state
    // transitions, in both size and location", where runtime memory is
    // whatever the memory map reports with EFI_MEMORY_RUNTIME - which
    // EfiRuntimeServicesCode carries. A hibernation image captured without
    // this loader present, resumed with it present, therefore sees runtime
    // memory that differs in both size and location from the image.
    //
    // The same document excludes AddressRangeReserved from both that rule
    // and the matching one for operating system physical memory, so
    // reserved memory is the one kind an image neither contains nor
    // expects. That is exactly what this allocation wants to be: memory no
    // operating system may account for, reuse, or restore over.
    //
    // Runtime services code was also the wrong description on its own
    // terms. It means code the firmware calls through the runtime services
    // after ExitBootServices, at a virtual address the operating system
    // assigns with SetVirtualAddressMap. Nothing calls into this module
    // that way, and having a virtual mapping made for it is not wanted.
    //
    // What matters for a resume is compounded by the module being hidden:
    // protect_module clears every EPT permission on these pages, so a
    // restore writing over them does not merely corrupt the module, it
    // takes an EPT violation on a processor whose only response is to stop.
    auto status = g_boot_services->AllocatePages(
        AllocateAnyPages,
        EfiReservedMemoryType,
        (size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE,
        &physical_address);

    // If not success, return nullptr;
    if (EFI_ERROR(status)) {
        return nullptr;
    }

    trace::hex_line("ZPP_TRACE allocate_rwx done at ", physical_address);

    // Return the result address.
    return reinterpret_cast<void *>(physical_address);
}

static void * allocate_below_one_megabyte(std::size_t size)
{
    // One below a megabyte, not the megabyte itself: this is an inclusive
    // upper bound on the last byte allocated, so naming the boundary would
    // permit a page starting at it - and a page number of 0x100 does not
    // fit in the eight bits a start-up IPI carries.
    constexpr EFI_PHYSICAL_ADDRESS highest_usable_address = 0x100000 - 1;

    EFI_PHYSICAL_ADDRESS physical_address = highest_usable_address;

    // Reserved rather than loader owned, because this has to outlive the
    // loader by the whole life of the machine. A processor may be started
    // long after the operating system has taken over, and when it is, it
    // begins executing here - so this must be memory no operating system
    // believes it may reuse.
    auto status = g_boot_services->AllocatePages(
        AllocateMaxAddress,
        EfiReservedMemoryType,
        (size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE,
        &physical_address);
    if (EFI_ERROR(status)) {
        trace::line("ZPP_TRACE no memory below one megabyte");
        return nullptr;
    }

    trace::hex_line("ZPP_TRACE start up memory at ", physical_address);
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
    // Under Bochs the firmware publishes
    // no ACPI tables at all, so there is no FADT to read the timer block
    // from - but the PIIX4 power management function is there and OVMF's
    // own AcpiTimerLib finds it the same way this does, straight out of
    // PCI config space at 00:01.3 offset 0x40. Without this the loader
    // reports a single cpu and never touches MP services, so the AP path
    // under test is never exercised.
    auto piix4_pm_timer_advancing = [&] {
        constexpr std::uint16_t config_address_port = 0xcf8;
        constexpr std::uint16_t config_data_port = 0xcfc;
        constexpr std::uint32_t piix4_pm_function =
            0x80000000u | (0u << 16) | (1u << 11) | (3u << 8);
        constexpr std::uint32_t pmba_offset = 0x40;
        constexpr std::uint16_t pm_timer_offset = 8;
        constexpr std::uint32_t piix4_pm_identity = 0x71138086;

        auto read_config = [&](std::uint32_t offset) {
            asm volatile("outl %0, %1" ::"a"(piix4_pm_function | offset),
                         "Nd"(config_address_port));
            std::uint32_t value{};
            asm volatile("inl %1, %0"
                         : "=a"(value)
                         : "Nd"(config_data_port));
            return value;
        };

        // Only ever touch a function that identifies itself as the PIIX4
        // power management one. Without this the probe would read offset
        // 0x40 of whatever happens to sit at 00:01.3 and then hammer a
        // port derived from it, which on a modern chipset is not a timer.
        auto identity = read_config(0);
        if (piix4_pm_identity != identity) {
            return false;
        }

        auto pmba = read_config(pmba_offset);
        auto base = static_cast<std::uint16_t>(pmba & 0xfffcu);
        if (!base || (0xfffcu == base)) {
            return false;
        }

        auto port = static_cast<std::uint16_t>(base + pm_timer_offset);
        auto first = read_port(port);
        if (0xffffffffu == first) {
            return false;
        }
        for (std::uint32_t attempt{}; attempt < 1000000u; ++attempt) {
            if (read_port(port) != first) {
                trace::line("ZPP_TRACE piix4 pm timer advances");
                return true;
            }
        }
        return false;
    };

    if (!rsdp) {
        trace::line("ZPP_TRACE acpi probe: no rsdp");
        return piix4_pm_timer_advancing();
    }

    // XSDT address lives at offset 24 of the root pointer.
    std::uint64_t xsdt_address{};
    std::memcpy(&xsdt_address, rsdp + 24, sizeof(xsdt_address));
    if (!xsdt_address) {
        trace::line("ZPP_TRACE acpi probe: no xsdt");
        return false;
    }

    auto * xsdt = reinterpret_cast<const unsigned char *>(xsdt_address);
    std::uint32_t xsdt_length{};
    std::memcpy(&xsdt_length, xsdt + 4, sizeof(xsdt_length));
    if (xsdt_length <= 36) {
        trace::line("ZPP_TRACE acpi probe: short xsdt");
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

    trace::line("ZPP_TRACE acpi probe: no facp");
    return false;
}

static std::size_t number_of_cpus()
{
    // The boot processor alone, deliberately, and this is the whole of the
    // decision that used to be made by asking MP services.
    //
    // An application processor is not idle here - it is parked by the
    // firmware, waiting to be woken by the INIT-SIPI-SIPI its operating
    // system will eventually send. Borrowing it to run vmxon leaves it in
    // VMX root mode when that sequence arrives, and that is the one state
    // in which a start-up IPI is architecturally allowed to go missing:
    // SDM 28.2 has a start-up IPI discarded rather than queued unless the
    // target is already in the wait-for-SIPI activity state, and a layer
    // virtualizing this machine will hold the IPI back for as long as this
    // VMM is in root mode, by design.
    //
    // So the loader does not touch them. Each one is left exactly as the
    // firmware parked it, receives its operating system's INIT-SIPI-SIPI
    // through the ordinary hardware path that has always worked, and is
    // brought under the hypervisor by the hypervisor itself - which sees
    // that sequence, because the processor sending it is a guest.
    //
    // This also removes the loader's dependence on MP services being
    // usable at all, which was never a comfortable thing to require: the
    // firmware's own AP wakeup blocked indefinitely once a processor had
    // been used and given back.
    return 1;
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

    // The launch function.
    auto launch = [&] {
        // Call the user function and save the result.
        result = function(context);

        // Signalled here as well as by MP services, which signals it when
        // this procedure returns. Redundant on purpose: a double signal is
        // harmless, and this way the wait still ends if the firmware ever
        // fails to signal it.
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

    // Startup the relevant CPU, and wait for it under our own deadline.
    //
    // The non-blocking form, with the deadline enforced here rather than
    // by the firmware. Passing a timeout to the blocking form was tried
    // first and does not work: this firmware waits indefinitely
    // regardless, so a processor that never runs the function hung the
    // loader forever, which reports nothing and leaves the firmware on
    // screen looking wedged.
    //
    // Polling CheckEvent with a Stall between tries is the way to keep the
    // deadline ours. The event is signalled by MP services when the
    // procedure returns, so nothing in the launch function has to.
    //
    // Generous, because this is not a latency budget. Starting a processor
    // takes an INIT, ten milliseconds, a start-up IPI, two hundred
    // microseconds and another; anything still absent after seconds is not
    // late, it is not coming.
    constexpr std::size_t start_up_poll_microseconds = 1000;
    constexpr std::size_t start_up_polls = 5000;
    status = g_mp_services->StartupThisAP(
        g_mp_services,
        static_cast<void (*)(void *)>(erased_launch),
        cpuid,
        join_event,
        0,
        &launch,
        nullptr);
    if (EFI_ERROR(status)) {
        goto close_event;
    }

    // Wait for the join event, bounded.
    status = EFI_TIMEOUT;
    for (std::size_t poll{}; poll < start_up_polls; ++poll) {
        if (!EFI_ERROR(g_boot_services->CheckEvent(join_event))) {
            status = EFI_SUCCESS;
            break;
        }
        g_boot_services->Stall(start_up_poll_microseconds);
    }
    if (EFI_ERROR(status)) {
        // Left as the failure the caller reports. result keeps whatever
        // the function managed to store, which for a processor that never
        // ran is the -1 it started as.
        goto close_event;
    }

    // Result was changed by the other core.
close_event:
    // Close the event, and return the result.
    g_boot_services->CloseEvent(join_event);
    return result;
}

static int __attribute__((naked)) invoke_entry(
    int (*)(std::size_t, std::uintptr_t (*)(std::uintptr_t), void *),
    std::size_t,
    std::uintptr_t (*)(std::uintptr_t),
    void *)
{
    asm(R"!!(
        .intel_syntax noprefix
        push rdi // Save rdi before use as it is non-volatile.
        push rsi // Save rsi before use as it is non-volatile.
        mov rdi, rdx // Forward first parameter to function.
        mov rsi, r8 // Forward second parameter to function.
        mov rdx, r9 // Forward third parameter, after rdx has been read.
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
        trace::line("ZPP_TRACE no device path utilities");
        return nullptr;
    }

    // Compute the file name size. The terminating null is part of the
    // node: a FILEPATH_DEVICE_PATH holds a null terminated string, and
    // leaving it out both truncates the name and understates the node
    // length by two bytes, which is enough for AppendDevicePath to
    // reject the whole path.
    std::size_t file_name_size =
        (std::char_traits<char16_t>::length(file_name) + 1) *
        sizeof(char16_t);

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

    // Convert device handle to path. HandleProtocol hands back a pointer
    // to the protocol, so this has to be a pointer - taking the address
    // of an EFI_DEVICE_PATH and passing that on made AppendDevicePath
    // read a device path whose first bytes were the pointer itself, and
    // it answered null. Nothing caught it because the chainload only
    // runs on a real machine.
    EFI_DEVICE_PATH * device_path_from_handle{};
    status = g_boot_services->HandleProtocol(
        device,
        &g_efi_device_path_protocol_guid,
        reinterpret_cast<void **>(&device_path_from_handle));
    if (EFI_ERROR(status)) {
        trace::line("ZPP_TRACE handle has no device path");
        device_path = nullptr;
        goto free_file_path_device_path;
    }

    // Build the full path from device and path.
    device_path = device_path_utilities->AppendDevicePath(
        device_path_from_handle, device_path);
    if (!device_path) {
        trace::line("ZPP_TRACE append device path failed");
    }

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

    // Point the trace channel at the screen before anything can fail, so
    // that a failure has somewhere to appear. On a machine with no serial
    // port this is the only channel that reports as it goes - the disk
    // copy is written once, at the chainload, and says nothing about a
    // hang before it.
    trace::console = system_table->ConOut;
    g_runtime_services = system_table->RuntimeServices;

    trace::line("ZPP_TRACE entry");

    // Establish whether timed waits work before touching MP services,
    // since a dead timer makes them hang rather than return an error.
    g_timed_waits_usable = acpi_timer_advancing(system_table);
    trace::line(g_timed_waits_usable
                    ? "ZPP_TRACE timed waits usable"
                    : "ZPP_TRACE timed waits unusable, single cpu");

    // Load the MP Services.
    status = g_boot_services->LocateProtocol(
        &g_efi_mp_service_protocol_guid,
        nullptr,
        reinterpret_cast<void **>(&g_mp_services));
    if (EFI_ERROR(status)) {
        trace::line("ZPP_HYPERVISOR_FAILED no EFI_MP_SERVICES_PROTOCOL");
        write_trace_variable();
        return EFI_LOAD_ERROR;
    }

    trace::line("ZPP_TRACE mp services located");

    // Before this loader has done anything, so it describes the machine
    // the firmware handed over rather than the one we made.
    trace_launch_context();

    // Load the ELF.
    const zpp_loader_parameters parameters{
        .allocate_rwx = allocate_rwx,
        .physical_to_virtual = nullptr,
        .call_on_cpu = call_on_cpu,
        .number_of_cpus = number_of_cpus,
        // The one platform that needs this. Only the boot processor is
        // launched from here, so every other one is started by the
        // hypervisor, and a processor being started begins in real mode
        // below one megabyte.
        .allocate_below_one_megabyte = allocate_below_one_megabyte,
        .adjust_launch_calling_convention = invoke_entry,
    };

    trace::line("ZPP_TRACE loading");

    auto result = zpp_load_elf(&parameters);

    trace::line("ZPP_TRACE loaded");

    // If we failed, return an arbitrary failure.
    if (result) {
        // The code is the hypervisor's own error enumeration, so print it
        // - it is the only thing that says which step failed, and a
        // debugger is not always available where this runs.
        // raw, not line: this is the verdict rather than a diagnostic, so
        // it must survive tracing being switched off.
        char buffer[trace::line_capacity]{};
        auto end = trace::append_text(
            buffer,
            "zpp: ZPP_HYPERVISOR_FAILED zpp_load_elf failed, code ");
        end =
            trace::append_hex(end, static_cast<std::uint64_t>(result), 16);
        end = trace::append_text(end, "\r\n");
        *end = 0;
        trace::raw(buffer);
        write_trace_variable();
        return EFI_LOAD_ERROR;
    }

    // Ask the hypervisor to identify itself now that it should be live.
    // Built only for automated testing, so a normal loader does not carry
    // it - which is what the constant is for, since everything under here
    // is discarded when it is false and the chainload below becomes the
    // only path out.
    if constexpr (verify::enabled) {
        if (!verify::present(system_table, parameters)) {
            write_trace_variable();
            return EFI_LOAD_ERROR;
        }
    }

    // Continue to the OS.

    // The device this loader came from, so the search below can skip it.
    // On a real machine the removable media fallback is whatever boot
    // manager is installed - Limine on the development target - which is
    // the thing that chainloaded us, so chaining back to it would loop.
    // Under test our own image is the only thing on the medium, and
    // skipping it is what keeps the chainload from running at all.
    EFI_HANDLE our_device{};
    if (EFI_LOADED_IMAGE_PROTOCOL * our_image{};
        !EFI_ERROR(g_boot_services->HandleProtocol(
            image_handle,
            &g_efi_loaded_image_protocol_guid,
            reinterpret_cast<void **>(&our_image)))) {
        our_device = our_image->DeviceHandle;
    }

    // The boot managers to chain to, in order of preference. Windows is
    // named explicitly rather than relying on the removable media
    // fallback, because on a machine that has any boot manager
    // installed that fallback is the boot manager, not the OS.
    struct boot_manager
    {
        /**
         * The boot manager to load.
         */
        const char16_t * path;

        /**
         * Whether this entry must not be looked for on the device this
         * loader was itself loaded from.
         *
         * Only the removable media fallback needs it. On a machine that
         * has any boot manager installed, that path *is* that boot
         * manager - Limine on the development target - and it is the thing
         * that chainloaded us, so chaining back to it would loop forever.
         *
         * A boot manager named explicitly needs the opposite. Booted from
         * a Limine entry on bare metal, this loader sits in /EFI/zpp on
         * the very same EFI system partition as Windows, so refusing to
         * look at our own device would refuse to look at the only place
         * Windows is.
         */
        bool avoid_our_own_device;

        /**
         * A file that has to sit beside it for starting it to be able to
         * work, or null when there is nothing to require.
         *
         * This is what separates the boot partition from any other
         * partition carrying a copy of the same executable. A recovery or
         * vendor partition can hold its own bootmgfw.efi, and firmware
         * that generates boot options by scanning file systems will
         * happily create an option for it, so agreeing with the
         * firmware's own option is not evidence of having found the right
         * one either. Windows Boot Manager reads its configuration from
         * the BCD beside it, and without one it stops with 0xc000000d.
         */
        const char16_t * companion;
    };

    // Whether to chain only to Windows Boot Manager, refusing the
    // removable media fallback below it.
    //
    // On by default because that fallback is a hazard on the machine this
    // is booted from by hand. It names \EFI\BOOT\bootx64.efi, and on an
    // ESP with a boot manager of its own installed there, that file is the
    // boot manager which chainloaded this loader - so taking the fallback
    // starts the thing that started us, and the machine loops with no menu
    // to escape through. Requiring the BCD beside bootmgfw.efi already
    // makes the fallback unreachable whenever Windows is present, so this
    // costs nothing there and removes the loop everywhere else.
    //
    // Turn it off to get the previous behaviour, which is what a machine
    // that boots something other than Windows needs.
    static constexpr bool chain_to_windows_only = true;

    // Whether to look for the boot manager only on the device this loader
    // was itself loaded from, skipping the enumeration of everything else.
    //
    // On by default, and it is the difference between working and hanging
    // on a machine booted by hand. Enumerating means driving every
    // controller the firmware has not bothered with yet, and on real
    // hardware that is a long walk through drivers this loader has no
    // business starting - a black screen with no output was the result.
    // The passed-through disk in the emulated rig needed it, because
    // nothing had booted from it and it carried no file system handle at
    // all; a machine that just chainloaded us through its own ESP does
    // not, because the boot manager is on that same ESP.
    static constexpr bool chain_to_our_own_device_only = true;

    // The boot managers to chain to, in order of preference. Windows is
    // named explicitly rather than relying on the removable media
    // fallback, because on a machine that has any boot manager
    // installed that fallback is the boot manager, not the OS.
    static constexpr boot_manager boot_managers[]{
        {u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
         false,
         u"\\EFI\\Microsoft\\Boot\\BCD"},
        {u"\\EFI\\BOOT\\bootx64.efi", true, nullptr},
    };

    // How many of them to actually consider. Windows Boot Manager is
    // first, so restricting the count to one is what drops the fallback.
    static constexpr std::size_t considered_boot_managers =
        chain_to_windows_only ? 1 : std::size(boot_managers);

    // Drive every controller before looking for a boot manager. Firmware
    // connects only as much as it needs to reach the boot option it was
    // told to start, so a disk nothing has booted from yet carries no
    // block io handle at all - which is exactly the state the passed
    // through NVMe holding Windows was found in, enumerated as a PCI
    // device and invisible as a file system.
    EFI_HANDLE * all_handles{};
    std::size_t number_of_all_handles{};
    if (!chain_to_our_own_device_only &&
        !EFI_ERROR(
            g_boot_services->LocateHandleBuffer(AllHandles,
                                                nullptr,
                                                nullptr,
                                                &number_of_all_handles,
                                                &all_handles))) {
        for (std::size_t i{}; i < number_of_all_handles; ++i) {
            // Failure is normal and uninteresting: most handles are not
            // controllers, and the ones that are may already be driven.
            g_boot_services->ConnectController(
                all_handles[i], nullptr, nullptr, true);
        }
        g_boot_services->FreePool(all_handles);
        trace::hex_line("ZPP_TRACE connected controllers",
                        number_of_all_handles);
    }

    // Locate file system handles.
    EFI_HANDLE * file_system_handles{};
    std::size_t number_of_file_system_handles{};

    // Not pool memory, so it is never freed - and nothing here frees the
    // enumerated buffer either, so the two cases stay interchangeable.
    EFI_HANDLE our_device_only[]{our_device};

    if constexpr (chain_to_our_own_device_only) {
        if (!our_device) {
            trace::line("ZPP_TRACE no device for this image");
            return EFI_LOAD_ERROR;
        }
        file_system_handles = our_device_only;
        number_of_file_system_handles = 1;
    } else {
        status = g_boot_services->LocateHandleBuffer(
            ByProtocol,
            &g_efi_block_io_protocol_guid,
            nullptr,
            &number_of_file_system_handles,
            &file_system_handles);
        if (EFI_ERROR(status)) {
            trace::line("ZPP_TRACE no block io handles");
            return EFI_LOAD_ERROR;
        }
    }

    trace::hex_line("ZPP_TRACE block io handles",
                    number_of_file_system_handles);

    // Iterate the boot managers, and every file system for each one, so
    // that a preferred boot manager anywhere wins over a fallback on
    // whichever device happens to enumerate first.
    for (auto [boot_manager, avoid_our_own_device, companion] :
         std::span{boot_managers}.first(considered_boot_managers)) {
        for (std::size_t i{}; i < number_of_file_system_handles; ++i) {
            // Skip the device this loader came from, for the entries
            // that would loop back into it.
            if (avoid_our_own_device &&
                (file_system_handles[i] == our_device)) {
                continue;
            }

            // Find the block IO from the handle.
            EFI_BLOCK_IO * block_io{};
            status = g_boot_services->HandleProtocol(
                file_system_handles[i],
                &g_efi_block_io_protocol_guid,
                reinterpret_cast<void **>(&block_io));
            if (EFI_ERROR(status)) {
                continue;
            }

            // Require the rest of the installation to be present before
            // treating this file system as the one to boot. Reported
            // either way, since which partitions carry what is the first
            // thing worth knowing when a chainload goes wrong.
            if (companion) {
                auto has_boot_manager =
                    file_exists(file_system_handles[i], boot_manager);
                auto has_companion =
                    file_exists(file_system_handles[i], companion);
                if (has_boot_manager || has_companion) {
                    trace::hex_line(
                        has_boot_manager
                            ? (has_companion
                                   ? "ZPP_TRACE fs has boot manager and "
                                     "companion, index "
                                   : "ZPP_TRACE fs has boot manager but "
                                     "no "
                                     "companion, index ")
                            : "ZPP_TRACE fs has companion only, index ",
                        i);
                }
                if (!has_boot_manager || !has_companion) {
                    continue;
                }
            }

            // Get the full path to the boot manager inside the
            // specified file system.
            auto file_path =
                file_device_path(file_system_handles[i], boot_manager);
            // One handle that cannot produce a path is not a reason to
            // abandon the search - the next one may well be the OS.
            if (!file_path) {
                trace::line("ZPP_TRACE file device path failed");
                continue;
            }

            trace_device_path("ZPP_TRACE candidate ", file_path);

            // Load the image from the specified path.
            EFI_HANDLE current_image_handle{};
            status = g_boot_services->LoadImage(false,
                                                image_handle,
                                                file_path,
                                                nullptr,
                                                0,
                                                &current_image_handle);

            // Say which candidate the firmware accepted. Several devices
            // can carry a file at the same path, so knowing one was found
            // is not the same as knowing the right one was.
            if (!EFI_ERROR(status)) {
                trace_device_path("ZPP_TRACE loaded from ", file_path);
            }

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

            // If we had an error, or the image is not an EFI loader
            // code, continue.
            if (EFI_ERROR(status) ||
                image_info->ImageCodeType != EfiLoaderCode) {
                continue;
            }

            // What the started image will treat as its own device and
            // path. Windows Boot Manager finds its BCD, its OS loader and
            // its resources relative to these rather than relative to the
            // path it was asked for, so the right file reached through the
            // wrong device handle fails exactly like a missing boot
            // configuration - which is what 0xc000000d reports.
            if (EFI_DEVICE_PATH * image_device_path{};
                !EFI_ERROR(g_boot_services->HandleProtocol(
                    image_info->DeviceHandle,
                    &g_efi_device_path_protocol_guid,
                    reinterpret_cast<void **>(&image_device_path)))) {
                trace_device_path("ZPP_TRACE image device ",
                                  image_device_path);
            } else {
                trace::line("ZPP_TRACE image has no device handle path");
            }
            trace_device_path("ZPP_TRACE image file path ",
                              image_info->FilePath);

            // Hand over the load options the firmware's own boot option
            // for this boot manager carries, if it has any. LoadImage
            // takes no such parameter - the firmware's own boot path sets
            // them on the loaded image between LoadImage and StartImage,
            // and so does this.
            if (auto load_options =
                    find_boot_option_load_options(boot_manager);
                load_options.data) {
                image_info->LoadOptions = load_options.data;
                image_info->LoadOptionsSize = load_options.size;
            } else {
                trace::line("ZPP_TRACE no load options for boot manager");
            }

            // Again, immediately before handing over: the comparison
            // against the same lines above is what would show this loader
            // having changed a processor's state, and the state at the
            // handoff is the one the boot manager actually inherits.
            trace_launch_context();

            // Name what is being started, on the line that says it is
            // being started. The candidate paths traced above are traced
            // for every file system considered, so the last one printed
            // is not necessarily the one chosen - and this is the only
            // line that is reached exactly once, for the winner.
            trace_device_path("ZPP_TRACE chainloading ",
                              image_info->FilePath);

            // Save the log before handing over, because a boot manager
            // that boots successfully never comes back and would take
            // the log with it.
            write_trace_log(our_device);

            // Start the image.
            status = g_boot_services->StartImage(
                current_image_handle, nullptr, nullptr);

            // Reaching here means the boot manager returned instead of
            // booting. That is a different failure from hanging inside
            // it, and the two used to be indistinguishable: the log was
            // written above and never again, so a successful boot, a
            // hang and a refused StartImage all left a log ending at the
            // line above. Record the status and write the log a second
            // time, so the next occurrence says which one happened.
            //
            // The screen is no help either way. On this path control
            // returns to the firmware with our own output still the last
            // thing on it, which looks exactly like a hang.
            trace::hex_line("ZPP_TRACE start image returned ", status);
            write_trace_log(our_device);
            write_trace_variable();

            // Return the start image status.
            return status;
        }
    }

    trace::line("ZPP_TRACE no boot manager found");
    write_trace_log(our_device);
    write_trace_variable();

    // Return success anyway, no image was found is considered ok.
    return EFI_SUCCESS;
}
