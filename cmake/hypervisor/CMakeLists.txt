cmake_minimum_required(VERSION 3.25)
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

# Source files — all .cpp files under hypervisor/src/ recursively
file(GLOB_RECURSE HYPERVISOR_SOURCES
    "${ZPP_SOURCE_DIR}/hypervisor/src/*.cpp"
)

if(NOT HYPERVISOR_SOURCES)
    message(FATAL_ERROR "No hypervisor source files found under ${ZPP_SOURCE_DIR}/hypervisor/src/")
endif()

add_executable(zpp_hypervisor ${HYPERVISOR_SOURCES})

# Project include directories
target_include_directories(zpp_hypervisor PRIVATE
    "${ZPP_SOURCE_DIR}/hypervisor/include"
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

# Freestanding compile flags
target_compile_options(zpp_hypervisor PRIVATE
    -pedantic -Wall -Wextra -Werror
    -fPIE -nostdlib -ffreestanding -mno-red-zone
    -stdlib=libc++ -fno-rtti -fno-threadsafe-statics -fno-exceptions
    -D_LIBCPP_DISABLE_AVAILABILITY
    "-D_LIBCPP_VERBOSE_ABORT(...)=__builtin_trap()"
    $<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>
)

# Config-specific compile flags
target_compile_options(zpp_hypervisor PRIVATE
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=0>
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
