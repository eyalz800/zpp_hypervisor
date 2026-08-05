cmake_minimum_required(VERSION 3.28)
project(zpp_hypervisor LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ZPP_SOURCE_DIR and ZPP_OUTPUT_DIR are passed in via -D from the parent build.
if(NOT DEFINED ZPP_SOURCE_DIR)
    message(FATAL_ERROR "ZPP_SOURCE_DIR must be set (path to the repository root)")
endif()
if(NOT DEFINED ZPP_OUTPUT_DIR)
    message(FATAL_ERROR "ZPP_OUTPUT_DIR must be set (path to the output directory)")
endif()

# Source files — all .cpp/.cppm files under hypervisor/src/ recursively.
# CONFIGURE_DEPENDS makes the generator re-check the glob on each build,
# so adding a file does not require re-running cmake manually.
file(GLOB_RECURSE HYPERVISOR_SOURCES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/hypervisor/src/*.cpp"
    "${ZPP_SOURCE_DIR}/hypervisor/src/*.S"
)

# Module interface units are added through a CXX_MODULES file set rather
# than as plain sources, so CMake scans and builds them as C++ modules.
file(GLOB_RECURSE HYPERVISOR_MODULES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/hypervisor/src/*.cppm"
)

if(NOT HYPERVISOR_SOURCES AND NOT HYPERVISOR_MODULES)
    message(FATAL_ERROR "No hypervisor source files found under ${ZPP_SOURCE_DIR}/hypervisor/src/")
endif()

add_executable(zpp_hypervisor ${HYPERVISOR_SOURCES})

if(HYPERVISOR_MODULES)
    target_sources(zpp_hypervisor PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS "${ZPP_SOURCE_DIR}/hypervisor/src"
        FILES ${HYPERVISOR_MODULES}
    )
endif()

# Project include directories. loader/include is here for zpp/loader.h,
# which declares what the loader hands the entry point - the framebuffer
# description among it. Shared rather than duplicated: it is one ABI, and a
# second declaration of it would be free to drift.
target_include_directories(zpp_hypervisor PRIVATE
    "${ZPP_SOURCE_DIR}/hypervisor/include"
    "${ZPP_SOURCE_DIR}/loader/include"
)

# Freestanding include order (critical for #include_next chains):
# 1. Config override (__config_site for freestanding: no threads, no locale, etc.)
# 2. libc++ headers (cstdint, type_traits, etc. — use #include_next for C headers)
# 3. Freestanding C stubs (string.h, wchar.h — satisfy libc++'s #include_next)
# 4. Clang builtins (stddef.h, stdint.h — satisfy the stubs' #includes)
target_include_directories(zpp_hypervisor SYSTEM PRIVATE
    "${ZPP_SOURCE_DIR}/cmake/freestanding-config"
    "${LLVM_LIBCXX_INCLUDE}"
    "${ZPP_SOURCE_DIR}/cmake/freestanding-libc"
    "${LLVM_CLANG_INCLUDE}"
)

# Freestanding compile flags. -stdlib is scoped to C++ rather than applied
# to everything: the assembler has no use for it, and an ignored argument
# is an error under -Werror.
target_compile_options(zpp_hypervisor PRIVATE
    -pedantic -Wall -Wextra -Werror
    -fPIE -nostdlib -ffreestanding -mno-red-zone
    $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
    -fno-rtti -fno-threadsafe-statics -fno-exceptions
    -D_LIBCPP_DISABLE_AVAILABILITY
    "-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()"
    $<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>
)

# Config-specific compile flags
target_compile_options(zpp_hypervisor PRIVATE
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=$<BOOL:${ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER}>>
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-fvisibility=hidden>
)

# Link flags
target_link_options(zpp_hypervisor PRIVATE
    -nostdlib -fPIE -pie
    -Wl,--no-undefined
    $<$<CONFIG:Release>:-Wl,--strip-all>
    $<$<CONFIG:Release>:-Wl,--gc-sections>
)

# Output to ZPP_OUTPUT_DIR (e.g. out/debug/x86_64/)
set_target_properties(zpp_hypervisor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ZPP_OUTPUT_DIR}"
    OUTPUT_NAME "zpp_hypervisor"
)
