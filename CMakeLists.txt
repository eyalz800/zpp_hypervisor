cmake_minimum_required(VERSION 3.25)
project(zpp_hypervisor_project LANGUAGES NONE)

include(ExternalProject)

set(ZPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
string(TOLOWER "${CMAKE_BUILD_TYPE}" ZPP_CONFIG_LOWER)
set(ZPP_OUTPUT_DIR "${ZPP_SOURCE_DIR}/out/${ZPP_CONFIG_LOWER}/x86_64")

set(FREESTANDING_TOOLCHAIN "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-freestanding.cmake")

# === Hypervisor (freestanding x86_64 PIE ELF) ===
ExternalProject_Add(hypervisor
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hypervisor"
    BINARY_DIR "${CMAKE_BINARY_DIR}/hypervisor"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${FREESTANDING_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
)

set(WINDOWS_MSVC_TOOLCHAIN "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-windows-msvc.cmake")

ExternalProject_Add(windows_loader
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows-loader"
    BINARY_DIR "${CMAKE_BINARY_DIR}/windows-loader"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${WINDOWS_MSVC_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DFETCHCONTENT_BASE_DIR=${CMAKE_BINARY_DIR}/_deps
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS hypervisor
)

ExternalProject_Add(uefi_loader
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uefi-loader"
    BINARY_DIR "${CMAKE_BINARY_DIR}/uefi-loader"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${WINDOWS_MSVC_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DFETCHCONTENT_BASE_DIR=${CMAKE_BINARY_DIR}/_deps
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS hypervisor
)

# === Linux Loader (freestanding x86_64 ELF relocatable object) ===
# Phase 1 (CMake): cross-compiles loader/src/*.cpp into a single zpp_loader.o.
# Phase 2 (Podman): kbuild compiles linux_loader/src/main.c + links .ko.
#   Run: cmake --build --preset <config> --target linux_loader_ko
ExternalProject_Add(linux_loader
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-loader"
    BINARY_DIR "${CMAKE_BINARY_DIR}/linux-loader"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${FREESTANDING_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DFETCHCONTENT_BASE_DIR=${CMAKE_BINARY_DIR}/_deps
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS hypervisor
)

# Optional .ko build via Podman kbuild (requires Podman to be installed).
# Usage: cmake --build --preset <config> --target linux_loader_ko
add_custom_target(linux_loader_ko
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-loader/kbuild-wrapper.sh"
        "${ZPP_SOURCE_DIR}"
        "${ZPP_OUTPUT_DIR}"
        "${ZPP_CONFIG_LOWER}"
    DEPENDS linux_loader
    COMMENT "Building linux kernel module (.ko) via Podman kbuild"
)
