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
endif()

# ---- Include directories -----------------------------------------------
# Project headers first (non-system), then system includes.
target_include_directories(zpp_loader_efi PRIVATE
    "${ZPP_SOURCE_DIR}/hypervisor/include"
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
