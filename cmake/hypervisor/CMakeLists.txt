cmake_minimum_required(VERSION 3.25)
project(zpp_hypervisor LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
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

# libc++ freestanding headers — LLVM_LIBCXX_INCLUDE and LLVM_CLANG_INCLUDE are
# set by the toolchain file.  MACOS_SDK_INCLUDE (also set by the toolchain) is
# the macOS SDK usr/include directory, which provides the C standard library
# declarations that libc++'s #include_next machinery expects.  These are placed
# after the libc++ headers so that libc++'s own <string.h> / <wchar.h> wrappers
# are found first and the macOS SDK headers satisfy the subsequent include_next.
target_include_directories(zpp_hypervisor SYSTEM PRIVATE
    "${LLVM_LIBCXX_INCLUDE}"
    "${MACOS_SDK_INCLUDE}"
)

# Freestanding compile flags (all languages)
target_compile_options(zpp_hypervisor PRIVATE
    -pedantic -Wall -Wextra -Werror
    -fPIE -nostdlib -ffreestanding -mno-red-zone
    -stdlib=libc++ -fno-rtti -fno-threadsafe-statics -fno-exceptions
    # Homebrew libc++ sets _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS=1 in
    # __config_site for macOS builds.  When targeting a bare-metal x86_64-elf
    # the deployment-target macros that the vendor block expects are absent,
    # which triggers a hard #error.  -D_LIBCPP_DISABLE_AVAILABILITY is the
    # documented escape hatch (see __configuration/availability.h).
    -D_LIBCPP_DISABLE_AVAILABILITY
    # libc++ v22 headers include transitive headers (locale, thread support, etc.)
    # that are macOS-specific and fail for an x86_64-linux-elf cross target.
    # Removing transitive includes makes each TU responsible for its own headers,
    # which is correct behaviour — the one missing include was added to small_map.h.
    -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES
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
