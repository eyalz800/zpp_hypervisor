cmake_minimum_required(VERSION 3.28)
project(zpp_windows_loader LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Suppress CMake's automatic MSVC CRT selection (-Xclang --dependent-lib=msvcrtd
# etc.) — kernel drivers provide their own minimal CRT (crt.cpp).
set(CMAKE_MSVC_RUNTIME_LIBRARY "" CACHE STRING "" FORCE)

# ZPP_SOURCE_DIR, ZPP_OUTPUT_DIR, and FETCHCONTENT_BASE_DIR are passed from
# the parent ExternalProject_Add invocation.
if(NOT DEFINED ZPP_SOURCE_DIR)
    message(FATAL_ERROR "ZPP_SOURCE_DIR must be set (path to the repository root)")
endif()
if(NOT DEFINED ZPP_OUTPUT_DIR)
    message(FATAL_ERROR "ZPP_OUTPUT_DIR must be set (path to the output directory)")
endif()

# Download dependencies (WDK/SDK NuGet packages)
include("${ZPP_SOURCE_DIR}/cmake/dependencies.cmake")
download_windows_sdk(WINDOWS_SDK_ROOT)
download_windows_wdk(WINDOWS_WDK_ROOT)
download_windows_sdk_libs(WINDOWS_SDK_LIBS_ROOT)

# Internal version string used for paths inside the NuGet package layout.
# The package version is 10.0.28000.2526 but the internal directory structure
# uses 10.0.28000.0.
set(WINDOWS_SDK_VER "10.0.28000.0")

# ---- Include directories -----------------------------------------------
# Order matches the original zpp_toolchain.mk (WDK km first, then SDK).
set(WDK_INCLUDES
    "${WINDOWS_WDK_ROOT}/Include/${WINDOWS_SDK_VER}/km"
    # km/crt provides excpt.h (needed by wdm.h) and CRT headers for kernel mode.
    # Must come before SDK ucrt so that new.h / stddef.h resolve to the km
    # versions, avoiding the user-mode vcruntime.h dependency chain.
    "${WINDOWS_WDK_ROOT}/Include/${WINDOWS_SDK_VER}/km/crt"
)
set(SDK_INCLUDES
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/ucrt"
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/um"
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/shared"
    "${WINDOWS_SDK_ROOT}/Include/${WINDOWS_SDK_VER}/winrt"
)

# ---- Library directories -----------------------------------------------
set(SDK_LIBS_DIR
    "${WINDOWS_SDK_LIBS_ROOT}/um/x64"
    "${WINDOWS_SDK_ROOT}/Lib/${WINDOWS_SDK_VER}/ucrt/x64"
    "${WINDOWS_SDK_ROOT}/Lib/${WINDOWS_SDK_VER}/um/x64"
)
set(WDK_LIBS_DIR
    "${WINDOWS_WDK_ROOT}/Lib/${WINDOWS_SDK_VER}/km/x64"
)

# ---- Source files -------------------------------------------------------
# Globbed recursively so new sources never have to be listed by hand.
# CONFIGURE_DEPENDS makes the generator re-check the glob on each build,
# so adding a file does not require re-running cmake manually.
file(GLOB_RECURSE LOADER_SOURCES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/windows_loader/src/*.cpp"
    "${ZPP_SOURCE_DIR}/windows_loader/src/*.S"
    "${ZPP_SOURCE_DIR}/loader/src/*.cpp"
)

# Module interface units are added through a CXX_MODULES file set rather
# than as plain sources, so CMake scans and builds them as C++ modules.
file(GLOB_RECURSE LOADER_MODULES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/windows_loader/src/*.cppm"
    "${ZPP_SOURCE_DIR}/loader/src/*.cppm"
)

if(NOT LOADER_SOURCES AND NOT LOADER_MODULES)
    message(FATAL_ERROR "No Windows loader source files found under ${ZPP_SOURCE_DIR}/windows_loader/src/")
endif()

add_executable(zpp_loader_sys ${LOADER_SOURCES})

if(LOADER_MODULES)
    target_sources(zpp_loader_sys PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS "${ZPP_SOURCE_DIR}"
        FILES ${LOADER_MODULES}
    )
endif()

# ---- Include directories -----------------------------------------------
# Project headers first (non-system), then WDK/SDK as system includes.
target_include_directories(zpp_loader_sys PRIVATE
    "${ZPP_SOURCE_DIR}/loader/include"
    "${ZPP_SOURCE_DIR}/windows_loader/include"
    "${ZPP_SOURCE_DIR}/hypervisor/include"
)
target_include_directories(zpp_loader_sys SYSTEM PRIVATE
    # C++ standard headers — freestanding-style include chain:
    #  1. Windows-loader-specific config (forces Itanium ABI to avoid vcruntime
    #     header dependencies; disables threads/locale/filesystem)
    #  2. libc++ headers (cstddef, cstdint, algorithm, tuple, variant, etc.)
    #  3. Freestanding C stubs (string.h, wchar.h type decls for libc++ #include_next)
    #  4. Clang builtin C headers (stddef.h, stdint.h etc.)
    "${ZPP_SOURCE_DIR}/cmake/windows-loader-config"
    "${LLVM_LIBCXX_INCLUDE}"
    "${ZPP_SOURCE_DIR}/cmake/freestanding-libc"
    "${LLVM_CLANG_INCLUDE}"
    # Windows-specific kernel/user headers from NuGet packages
    ${WDK_INCLUDES}
    ${SDK_INCLUDES}
)

# ---- Compile definitions -----------------------------------------------
target_compile_definitions(zpp_loader_sys PRIVATE
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
    # in a freestanding / driver context.
    _LIBCPP_DISABLE_AVAILABILITY
    # Prevent guiddef.h from defining _SYS_GUID_OPERATOR_EQ_, which causes
    # devpropdef.h to emit an inline operator that calls _wcsicmp — a function
    # not declared in our freestanding-libc wchar.h stub.
    _NO_SYS_GUID_OPERATOR_EQ_
)

# ---- Compile options ---------------------------------------------------
target_compile_options(zpp_loader_sys PRIVATE
    -nostdinc
    -pedantic -Wall -Wextra -Werror
    -ffreestanding -mno-red-zone
    -fno-exceptions -fno-rtti
    # Suppress the "extension" warning for #embed (it's standard in C++26 but
    # clang may still warn under -pedantic with certain targets).
    -Wno-c23-extensions
    # ExAllocatePool is deprecated in newer WDK; suppress since the source
    # uses ExAllocatePool intentionally and we cannot modify it here.
    -Wno-deprecated-declarations
    # Undefine GCC-specific macros that MSVC headers check for.
    -U__GNUC__
    -U__gnu_linux__
    -U__GNUC_MINOR__
    -U__GNUC_PATCHLEVEL__
    -U__GNUC_STDC_INLINE__
    # Override libc++'s verbose abort to use a builtin trap rather than calling
    # std::__libcpp_verbose_abort (which lives in the libc++ shared library —
    # not available in a freestanding kernel driver build).
    "-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()"
    # Assembly sources need the C preprocessor.
    $<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-fvisibility=hidden>
)

# ---- Link options ------------------------------------------------------
target_link_options(zpp_loader_sys PRIVATE
    -nostdlib
    -Wl,-subsystem:native
    -Wl,-driver:wdm
    -Wl,-dynamicbase
    -Wl,-nodefaultlib
    -Wl,-dll
    "LINKER:ntoskrnl.lib"
    -Wl,-entry:driver_entry
    -Wl,-ignore:4217
)

# Add WDK/SDK library search paths so lld-link can find ntoskrnl.lib etc.
foreach(_dir ${SDK_LIBS_DIR} ${WDK_LIBS_DIR})
    target_link_options(zpp_loader_sys PRIVATE "-L${_dir}")
endforeach()

# ---- Output ------------------------------------------------------------
set_target_properties(zpp_loader_sys PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ZPP_OUTPUT_DIR}"
    OUTPUT_NAME "zpp_loader"
    SUFFIX ".sys"
)
