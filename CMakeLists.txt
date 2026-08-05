cmake_minimum_required(VERSION 3.28)
project(zpp_hypervisor_project LANGUAGES NONE)

include(ExternalProject)

set(ZPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
string(TOLOWER "${CMAKE_BUILD_TYPE}" ZPP_CONFIG_LOWER)
set(ZPP_OUTPUT_DIR "${ZPP_SOURCE_DIR}/out/${ZPP_CONFIG_LOWER}/x86_64")

set(FREESTANDING_TOOLCHAIN "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-freestanding.cmake")

# Debugging only: makes the hypervisor spin at its entry point until a
# debugger releases it with `set var zpp::hypervisor::gdb_attached = 1`.
#
# The alternative is racing the guest with a gdb attach, which does not work
# once the thing being chased kills the guest: symbols cannot be loaded until
# the loader has mapped the module, and by then the failure has happened. This
# stops the module before anything runs, with the mapping already in place.
option(ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER
    "Spin at the hypervisor entry point until a debugger releases it"
    OFF)

# The diagnostic facility - the log, its retention ring and every sink that
# reads out of it. See diag/include/zpp/diag/config.h, which is where what is
# recorded and where it goes is decided; this is only the master switch.
#
# On by default, and forced off in release by the sub-builds regardless of what
# is set here - deliberately, so a stale cache entry cannot put diagnostics
# into a release binary. BACKLOG.md records what that trap costs when the
# switch is merely defaulted: -DZPP_VERIFY_HYPERVISOR=ON persisting in a CMake
# cache and being inherited silently by a later plain build.
option(ZPP_DIAG
    "Compile the diagnostic log, its ring and its sinks (debug builds only)"
    ON)

# === Hypervisor (freestanding x86_64 PIE ELF) ===
ExternalProject_Add(hypervisor
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hypervisor"
    BINARY_DIR "${CMAKE_BINARY_DIR}/hypervisor"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${FREESTANDING_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=${ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER}
        -DZPP_DIAG=${ZPP_DIAG}
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
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DFETCHCONTENT_BASE_DIR=${CMAKE_BINARY_DIR}/_deps
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS hypervisor
)

# Testing only: makes the UEFI loader ask the hypervisor to identify itself
# through CPUID leaf 0x40000000 once it has launched. Not for anything
# deployed, but equally useful locally as in CI - scripts/ci/bochs-smoke.sh
# runs the same check on a developer machine.
option(ZPP_VERIFY_HYPERVISOR
    "Compile the CPUID self check that asserts the hypervisor reached root mode"
    OFF)

# Serial tracing. Separate from the self check so it can be turned on while
# chasing something without also compiling the check, and off while running the
# check. Defaults to whatever the self check is set to, since the two are
# usually wanted together, and tracing has no place in a deployed loader
# either.
option(ZPP_TRACE
    "Compile the serial trace output in the loaders"
    ${ZPP_VERIFY_HYPERVISOR})

ExternalProject_Add(uefi_loader
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uefi-loader"
    BINARY_DIR "${CMAKE_BINARY_DIR}/uefi-loader"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${WINDOWS_MSVC_TOOLCHAIN}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
        -DFETCHCONTENT_BASE_DIR=${CMAKE_BINARY_DIR}/_deps
        -DZPP_VERIFY_HYPERVISOR=${ZPP_VERIFY_HYPERVISOR}
        -DZPP_TRACE=${ZPP_TRACE}
        -DZPP_DIAG=${ZPP_DIAG}
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
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DZPP_SOURCE_DIR=${ZPP_SOURCE_DIR}
        -DZPP_OUTPUT_DIR=${ZPP_OUTPUT_DIR}
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

# Copy each sub-build's compile_commands.json into the corresponding source
# directory after build so clangd picks up the correct flags automatically.
# Copies survive a build directory clean.
set(_COMPDB_COPIES
    "hypervisor:hypervisor"
    "windows-loader:windows_loader"
    "uefi-loader:uefi_loader"
    "linux-loader:linux_loader"
    "linux-loader:loader"
)
foreach(_pair ${_COMPDB_COPIES})
    string(REPLACE ":" ";" _parts "${_pair}")
    list(GET _parts 0 _build)
    list(GET _parts 1 _src)
    list(GET _parts 0 _target)
    string(REPLACE "-" "_" _target "${_target}")
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/${_build}/compile_commands.json"
            "${ZPP_SOURCE_DIR}/${_src}/compile_commands.json"
        COMMENT "Copying compile_commands.json to ${_src}/"
    )
endforeach()
