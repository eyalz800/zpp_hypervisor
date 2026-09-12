cmake_minimum_required(VERSION 3.28)
project(zpp_uefi_loader LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Suppress CMake's automatic MSVC CRT selection (-Xclang --dependent-lib=msvcrtd
# etc.) — UEFI applications provide their own minimal CRT (crt.cpp).
set(CMAKE_MSVC_RUNTIME_LIBRARY "" CACHE STRING "" FORCE)

# ZPP_SOURCE_DIR, ZPP_OUTPUT_DIR, and FETCHCONTENT_BASE_DIR are passed from
# the parent ExternalProject_Add invocation.
if(NOT DEFINED ZPP_SOURCE_DIR)
    message(FATAL_ERROR "ZPP_SOURCE_DIR must be set (path to the repository root)")
endif()
if(NOT DEFINED ZPP_OUTPUT_DIR)
    message(FATAL_ERROR "ZPP_OUTPUT_DIR must be set (path to the output directory)")
endif()

option(ZPP_VERIFY_HYPERVISOR
    "Compile the CPUID self check that asserts the hypervisor reached root mode"
    OFF)

option(ZPP_TRACE
    "Compile the serial trace output in the loaders"
    ${ZPP_VERIFY_HYPERVISOR})

option(ZPP_GUEST_TESTS
    "Compile the guest-side VM-exit, EPT and emulation coverage suite"
    OFF)

# Whether the hypervisor this loader embeds was built with the nested
# machinery. The loader does not act on it except in the coverage suite,
# which has to know: with nesting on the guest is deliberately told VMX
# exists, so the cases asserting it is hidden are asserting the *other*
# build's answer. Without this the suite tested one build's rule against
# the other's binary and failed for a reason that was not a defect.
# ON, matching the root project. See the comment there for why.
option(ZPP_DIAG_RESERVE_WINDOW
    "Declare a reserved memory window for the storage controller"
    OFF)
option(ZPP_NESTED_VMX
    "The embedded hypervisor reports VMX to its guest"
    ON)

option(ZPP_DIAG
    "Compile the diagnostic log, its ring and its sinks (debug builds only)"
    ON)

# ON, matching the root project. See the comment there for what it costs
# and why the hand-over structure is not behind it.
option(ZPP_FRAMEBUFFER
    "Locate the firmware framebuffer and hand it over (debug builds only)"
    ON)

# Everything the loader does except launching the hypervisor: locate the boot
# manager, connect the controllers it needs, forward the load options, chain
# to it. The point is a control run - the same discovery, the same rewritten
# ACPI tables, the same device path and the same guest, with only the thing
# under test removed - so a guest failure can be attributed rather than
# guessed at.
#
# Doing the control from the UEFI shell instead is not equivalent, twice over:
# the shell connects only what it needs to reach its own boot option, so a
# passed-through disk carries no file system handle at all; and it does none
# of the ACPI rewriting this loader does, so the guest is handed a different
# platform description. Reusing the loader removes both differences.
option(ZPP_CHAINLOAD_ONLY
    "Chainload the boot manager without launching the hypervisor"
    OFF)

# Whether to look for the boot manager on every device rather than only the
# one this loader was started from.
#
# Off is right for a bare metal boot: the machine chainloaded us through its
# own ESP and the boot manager is on that same ESP, so searching further can
# only find a worse answer - a recovery or vendor partition carrying its own
# copy without the configuration beside it.
#
# On is required whenever the loader and the boot manager are on different
# devices, which is the case in the passthrough rig: the loader is served off
# a virtual FAT drive and Windows is on the real disk. Firmware connects only
# what it needs to reach its own boot option, so that disk carries no file
# system handle until this loader connects every controller - which it only
# does when this is on.
option(ZPP_SEARCH_ALL_DEVICES
    "Look for the boot manager on every device, not only our own"
    OFF)

# Download dependencies (Windows SDK for ucrt/shared headers; EDK2 for UEFI headers)
include("${ZPP_SOURCE_DIR}/cmake/dependencies.cmake")
download_windows_sdk(WINDOWS_SDK_ROOT)
download_edk2(EDK2_ROOT)

# Internal version string used for paths inside the NuGet package layout.
set(WINDOWS_SDK_VER "10.0.28000.0")

# ---- Include directories -----------------------------------------------
# EDK2 MdePkg headers (Uefi.h, Protocol/*.h, Pi/*.h, etc.)
set(EDK2_INCLUDES
    "${EDK2_ROOT}/MdePkg/Include"
    "${EDK2_ROOT}/MdePkg/Include/X64"
)
# Windows SDK: only ucrt and shared (no um/km needed for UEFI)
set(SDK_INCLUDES
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/ucrt"
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/shared"
)

# ---- Source files -------------------------------------------------------
# Globbed recursively so new sources never have to be listed by hand.
# CONFIGURE_DEPENDS makes the generator re-check the glob on each build,
# so adding a file does not require re-running cmake manually.
file(GLOB_RECURSE LOADER_SOURCES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/uefi_loader/src/*.cpp"
    "${ZPP_SOURCE_DIR}/uefi_loader/src/*.S"
    "${ZPP_SOURCE_DIR}/loader/src/*.cpp"
)

# Module interface units are added through a CXX_MODULES file set rather
# than as plain sources, so CMake scans and builds them as C++ modules.
file(GLOB_RECURSE LOADER_MODULES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/uefi_loader/src/*.cppm"
    "${ZPP_SOURCE_DIR}/loader/src/*.cppm"
)

if(NOT LOADER_SOURCES AND NOT LOADER_MODULES)
    message(FATAL_ERROR "No UEFI loader source files found under ${ZPP_SOURCE_DIR}/uefi_loader/src/")
endif()

add_executable(zpp_loader_efi ${LOADER_SOURCES})

if(LOADER_MODULES)
    target_sources(zpp_loader_efi PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS "${ZPP_SOURCE_DIR}"
        FILES ${LOADER_MODULES}
    )
else()
    # With no module interface units there is nothing to scan, and the scan is
    # actively harmful here: clang-scan-deps preprocesses more eagerly than the
    # compiler and walks into the SDK's ucrt/corecrt.h, which includes a
    # vcruntime.h that the NuGet package does not ship. The compile itself
    # never takes that path. Re-enabled automatically by adding a .cppm.
    set_target_properties(zpp_loader_efi PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

# ---- Include directories -----------------------------------------------
# Project headers first (non-system), then system includes.
target_include_directories(zpp_loader_efi PRIVATE
    "${ZPP_SOURCE_DIR}/uefi_loader/include"
    "${ZPP_SOURCE_DIR}/loader/include"
    "${ZPP_SOURCE_DIR}/hypervisor/include"
    # Shared with the hypervisor: the diagnostic configuration, the format
    # spec, the record and the ring. Header only, constexpr, no allocation and
    # no platform headers, so it compiles under this ABI as readily as under
    # the freestanding one. This loader's own zpp/diag/sinks.h, in
    # uefi_loader/include, is found first and is what says which channels exist
    # while boot services do.
    "${ZPP_SOURCE_DIR}/diag/include"
)
target_include_directories(zpp_loader_efi SYSTEM PRIVATE
    # C++ standard headers — freestanding-style include chain:
    #  1. Windows-loader-specific config (forces Itanium ABI to avoid vcruntime
    #     header dependencies; disables threads/locale/filesystem)
    #  2. libc++ headers (cstddef, cstdint, algorithm, tuple, variant, string, etc.)
    #  3. Freestanding C stubs (string.h, wchar.h type decls for libc++ #include_next)
    #  4. Clang builtin C headers (stddef.h, stdint.h etc.)
    "${ZPP_SOURCE_DIR}/cmake/windows-loader-config"
    "${LLVM_LIBCXX_INCLUDE}"
    "${ZPP_SOURCE_DIR}/cmake/freestanding-libc"
    "${LLVM_CLANG_INCLUDE}"
    # EDK2 UEFI protocol and type headers
    ${EDK2_INCLUDES}
    # Windows SDK ucrt/shared headers
    ${SDK_INCLUDES}
)

# ---- Compile definitions -----------------------------------------------
target_compile_definitions(zpp_loader_efi PRIVATE
    _MT
    _WIN32
    _WIN64
    _AMD64_
    _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE
    _NO_CRT_STDIO_INLINE
    # Point #embed at the built hypervisor ELF binary.
    "ZPP_ELF_BINARY_PATH=\"${ZPP_OUTPUT_DIR}/zpp_hypervisor\""
    # Compiles the CPUID self check that asserts the hypervisor reached root
    # mode. Off by default: it is a test facility, whether run locally or in
    # CI, and has no place in a loader that is actually being deployed.
    # Forced off when the hypervisor is not launched, rather than merely
    # documented as incompatible. The check asserts that our signature is in
    # CPUID; with ZPP_CHAINLOAD_ONLY there is nothing to find, so it reports
    # a failure that is correct and useless, and it stops the boot before the
    # control run can produce the comparison it exists for. It cost a boot
    # exactly once, through a stale ON in a cache - the same way
    # BACKLOG.md records this option costing time before.
    "ZPP_VERIFY_HYPERVISOR=$<AND:$<BOOL:${ZPP_VERIFY_HYPERVISOR}>,$<NOT:$<BOOL:${ZPP_CHAINLOAD_ONLY}>>>"
    # The guest-side coverage suite. Forced off in the two configurations
    # where it could not mean anything, rather than documented as
    # incompatible - which is the lesson the line above already carries.
    #
    # With ZPP_CHAINLOAD_ONLY there is no hypervisor to ask, so every case
    # would fail correctly and uselessly. With ZPP_VERIFY_HYPERVISOR the
    # destructive check runs first and ends the boot, and worse, anything
    # this suite then reported would be attributed to it rather than to
    # the state verify::present had already left behind.
    "ZPP_GUEST_TESTS=$<AND:$<BOOL:${ZPP_GUEST_TESTS}>,$<NOT:$<BOOL:${ZPP_CHAINLOAD_ONLY}>>,$<NOT:$<BOOL:${ZPP_VERIFY_HYPERVISOR}>>>"
    # Always defined, never tested with #ifdef, matching the shape the
    # hypervisor's own build uses for the same switch.
    "ZPP_NESTED_VMX=$<BOOL:${ZPP_NESTED_VMX}>"
    # Serial tracing, controlled on its own so it can be enabled or disabled
    # independently of the check above. Defaults to the same value.
    "ZPP_TRACE=$<BOOL:${ZPP_TRACE}>"
    # The diagnostic facility, shared with the hypervisor and configured in one
    # place - diag/include/zpp/diag/config.h. Always defined, never tested with
    # #ifdef, and hardcoded to zero in release for the reason the option's own
    # comment gives.
    "ZPP_DIAG=$<IF:$<CONFIG:Release>,0,$<BOOL:${ZPP_DIAG}>>"
    "ZPP_DIAG_RESERVE_WINDOW=$<IF:$<CONFIG:Release>,0,$<BOOL:${ZPP_DIAG_RESERVE_WINDOW}>>"
    # The framebuffer query. Same shape as the diagnostic switch and
    # hardcoded 0 in release for the same reason: it is a diagnostic
    # capability, and a release loader has no reason to ask the firmware
    # where a screen it will never draw on lives.
    "ZPP_FRAMEBUFFER=$<IF:$<CONFIG:Release>,0,$<BOOL:${ZPP_FRAMEBUFFER}>>"
    # Skips the hypervisor launch so the same loader can produce a control
    # run. See the option's own comment for why the UEFI shell cannot.
    "ZPP_CHAINLOAD_ONLY=$<BOOL:${ZPP_CHAINLOAD_ONLY}>"
    # Search every device for the boot manager. Required by the passthrough
    # rig, wrong for a bare metal boot - see the option's own comment.
    "ZPP_SEARCH_ALL_DEVICES=$<BOOL:${ZPP_SEARCH_ALL_DEVICES}>"
    # libc++ availability annotations rely on OS version macros we don't have
    # in a freestanding / UEFI context.
    _LIBCPP_DISABLE_AVAILABILITY
)

# ---- Compile options ---------------------------------------------------
target_compile_options(zpp_loader_efi PRIVATE
    -nostdinc
    -pedantic -Wall -Wextra -Werror
    -ffreestanding -mno-red-zone
    -fno-exceptions -fno-rtti
    # Suppress the "extension" warning for #embed (it's standard in C++26 but
    # clang may still warn under -pedantic with certain targets).
    -Wno-c23-extensions
    # Undefine GCC-specific macros that MSVC/EDK2 headers check for.
    -U__GNUC__
    -U__gnu_linux__
    -U__GNUC_MINOR__
    -U__GNUC_PATCHLEVEL__
    -U__GNUC_STDC_INLINE__
    # Override libc++'s verbose abort to use a builtin trap rather than calling
    # std::__libcpp_verbose_abort (which lives in the libc++ shared library —
    # not available in a freestanding UEFI build).
    "-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()"
    # Assembly sources need the C preprocessor.
    $<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-fvisibility=hidden>
)

# ---- Link options ------------------------------------------------------
target_link_options(zpp_loader_efi PRIVATE
    -nostdlib
    -Wl,-subsystem:efi_application
    -Wl,-dynamicbase
    -Wl,-nodefaultlib
    -Wl,-dll
    -Wl,-entry:uefi_main
    -Wl,-ignore:4217
)

# ---- Output ------------------------------------------------------------
set_target_properties(zpp_loader_efi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ZPP_OUTPUT_DIR}"
    OUTPUT_NAME "zpp_loader"
    SUFFIX ".efi"
)
