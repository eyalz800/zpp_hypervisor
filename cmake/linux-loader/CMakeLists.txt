cmake_minimum_required(VERSION 3.28)
project(zpp_linux_loader LANGUAGES CXX ASM)

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

file(MAKE_DIRECTORY "${ZPP_OUTPUT_DIR}")

# --- Shared loader object (C++ freestanding) ---
# Compiles loader/src/elf_binary.cpp (embeds hypervisor ELF via #embed)
# and loader/src/main.cpp (implements zpp_load_elf) into relocatable objects.
#
# Note: linux_loader/src/main.c (kernel module glue) requires Linux kernel
# headers and must be compiled inside the Podman container via kbuild.
# See cmake/linux-loader/kbuild-wrapper.sh for the .ko build step.

# Globbed recursively so new sources never have to be listed by hand.
# CONFIGURE_DEPENDS makes the generator re-check the glob on each build,
# so adding a file does not require re-running cmake manually.
# Note: linux_loader/src/main.c is compiled by kbuild, not here.
file(GLOB_RECURSE LOADER_SOURCES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/loader/src/*.cpp"
)

# Module interface units are added through a CXX_MODULES file set rather
# than as plain sources, so CMake scans and builds them as C++ modules.
file(GLOB_RECURSE LOADER_MODULES CONFIGURE_DEPENDS
    "${ZPP_SOURCE_DIR}/loader/src/*.cppm"
)

if(NOT LOADER_SOURCES AND NOT LOADER_MODULES)
    message(FATAL_ERROR "No loader source files found under ${ZPP_SOURCE_DIR}/loader/src/")
endif()

add_library(zpp_loader_obj OBJECT ${LOADER_SOURCES})

if(LOADER_MODULES)
    target_sources(zpp_loader_obj PRIVATE
        FILE_SET CXX_MODULES
        BASE_DIRS "${ZPP_SOURCE_DIR}"
        FILES ${LOADER_MODULES}
    )
endif()

# Project headers
target_include_directories(zpp_loader_obj PRIVATE
    "${ZPP_SOURCE_DIR}/loader/include"
    "${ZPP_SOURCE_DIR}/hypervisor/include"
)

# Freestanding include order (same as hypervisor):
#   1. Config override (__config_site: no threads, no locale, etc.)
#   2. libc++ headers (cstdint, type_traits, etc.)
#   3. Freestanding C stubs (satisfy libc++'s #include_next)
#   4. Clang builtins (stddef.h, stdint.h, etc.)
target_include_directories(zpp_loader_obj SYSTEM PRIVATE
    "${ZPP_SOURCE_DIR}/cmake/freestanding-config"
    "${LLVM_LIBCXX_INCLUDE}"
    "${ZPP_SOURCE_DIR}/cmake/freestanding-libc"
    "${LLVM_CLANG_INCLUDE}"
)

# Freestanding compile flags (matches hypervisor, minus -fPIE / extra sections)
target_compile_options(zpp_loader_obj PRIVATE
    -pedantic -Wall -Wextra -Werror
    -nostdlib -ffreestanding -mno-red-zone
    -stdlib=libc++ -fno-rtti -fno-threadsafe-statics -fno-exceptions
    -D_LIBCPP_DISABLE_AVAILABILITY
    -Wno-deprecated-declarations
    -Wno-c23-extensions
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-fvisibility=hidden>
)

target_compile_definitions(zpp_loader_obj PRIVATE
    # Tell elf_binary.cpp where to find the built hypervisor ELF.
    "ZPP_ELF_BINARY_PATH=\"${ZPP_OUTPUT_DIR}/zpp_hypervisor\""
)

# --- Link into a single relocatable object ---
# Combines all OBJECT library artifacts into one zpp_loader.o via partial link.
# This is what gets bundled into the .ko by kbuild.
#
# We invoke ld.lld directly (not via the clang driver) to avoid the driver
# appending EXE_LINKER_FLAGS like --hash-style=gnu / --eh-frame-hdr, which
# ld.lld rejects in -r (partial-link) mode.
add_custom_command(
    OUTPUT "${ZPP_OUTPUT_DIR}/zpp_loader.o"
    COMMAND "${CMAKE_LINKER}"
        -m elf_x86_64
        --relocatable
        $<TARGET_OBJECTS:zpp_loader_obj>
        -o "${ZPP_OUTPUT_DIR}/zpp_loader.o"
    DEPENDS zpp_loader_obj
    COMMAND_EXPAND_LISTS
    COMMENT "Partial-linking linux loader relocatable object -> zpp_loader.o"
)

add_custom_target(zpp_loader_o ALL
    DEPENDS "${ZPP_OUTPUT_DIR}/zpp_loader.o"
)
