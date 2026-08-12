cmake_minimum_required(VERSION 3.28)
project(zpp_hypervisor_project LANGUAGES NONE)

include(ExternalProject)

set(ZPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
string(TOLOWER "${CMAKE_BUILD_TYPE}" ZPP_CONFIG_LOWER)
set(ZPP_OUTPUT_DIR "${ZPP_SOURCE_DIR}/out/${ZPP_CONFIG_LOWER}/x86_64")

# The hosted test suite is the one thing in this project built for the
# machine running the build rather than for the target, so it is a normal
# subdirectory rather than a fifth ExternalProject. That is what lets
# `ctest` see it: an ExternalProject's tests belong to a project of its
# own and the top level knows nothing about them.
#
# The compile database is exported for the same reason it is for each
# sub-build - tests/CMakeLists.txt copies it into tests/ so clangd resolves
# the shim directories, which no editor can guess.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

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

# Nested VMX: whether the guest is told VT-x exists.
#
# Off, and the default is the decision rather than a convenience. With it off
# the guest sees no VMX in CPUID, no VMXE in CR4 and takes a #UD on every VMX
# instruction - one consistent machine, and the one this VMM boots Windows in
# today, VBS included, because Hyper-V stands down when it finds no VMX.
#
# With it on the guest is told VMX exists and the instruction emulation answers
# for it, but VMLAUNCH is refused: there is no VMCS built from the shadow, no
# reflection of a second-level exit, and no shadow of the guest's extended page
# tables. A guest hypervisor therefore fails at launch instead of standing down
# cleanly, which is worse. hypervisor/include/zpp/hypervisor/nested_vmx.h
# carries the full list of what has to exist before this can default on.
# On by default. The whole objective of this tree is booting Hyper-V
# nested, so a build with it off cannot exercise the thing being worked
# on - and an afternoon was lost to exactly that: a clean reconfigure
# dropped the cached ON, the loader booted Windows with VMX concealed,
# Hyper-V stood down, and the result was briefly read as a milestone.
#
# Turning it off is still expressible and still useful - it is how you
# ask "is this failure nested at all" in one boot - but it has to be
# asked for now rather than arrived at by losing a cache entry.
# scripts/check-bootable.sh refuses a rig deploy of a loader built
# without it, so the bytes cannot reach the machine by accident.
option(ZPP_NESTED_VMX
    "Report VMX to the guest and emulate the VMX instructions"
    ON)

# The interrupted-event re-queue. On by default: without it an event whose
# delivery an exit interrupted is destroyed, which is what stopped Hyper-V's
# synthetic timer dead - see BACKLOG.md. Off is kept expressible because the
# path has a history of costing more than it bought, and because "does this
# change come from the re-queue" is then one boot rather than an argument.
option(ZPP_REQUEUE_INTERRUPTED_EVENTS
    "Put back an event whose delivery a VM exit interrupted"
    ON)

option(ZPP_NESTED_TPR_SHADOW
    "Hand a guest hypervisor's TPR shadow to the processor. Off forces CR8 exiting and emulates it against the same page, which is slower and exists to be the one variable between two boots."
    ON)

option(ZPP_PROFILE_L2
    "Sample a second-level guest's instruction pointer on the VMX-preemption timer. A profiler: it costs an exit per interval and is the only instrument that can see a guest spinning on memory."
    OFF)

# The loader's edit of the guest's DMAR table to declare a reserved
# window for the storage controller, plus the EFI system partition
# reservation and the restart that picks it up.
#
# Off, and no longer following ZPP_DIAG. Tying it to the facility put it
# in every build carrying any diagnostics - which is every rig build - so
# a boot that wanted a log ring also rewrote firmware tables and took a
# tail of a real partition on a real disk. It edits what the guest reads
# and it writes to the medium, so it is asked for rather than inherited.
option(ZPP_DIAG_RESERVE_WINDOW
    "Declare a reserved memory window for the storage controller"
    OFF)

# The guest-side coverage suite: uefi_loader/include/zpp/guest_tests.h, run
# from inside the guest once the hypervisor is live, reporting one line per
# case on serial. scripts/ci/bochs-exit-coverage.sh grades the result.
#
# Off, and off is the decision rather than a convenience. It is a test
# facility and has no place in a loader that boots anything: it installs an
# interrupt descriptor table of its own, deliberately faults thirteen
# instructions, writes the local APIC's task priority register and then
# stops instead of chainloading. None of that destroys state the way
# ZPP_VERIFY_HYPERVISOR does - every register it writes is put back and no
# other processor is touched - but a build carrying it never reaches an
# operating system, which is the failure mode CLAUDE.md records as costing
# a full day when it was a stale cache entry rather than a deliberate
# choice. scripts/check-bootable.sh refuses one.
#
# Declared here rather than beside the loader's other switches because the
# *hypervisor* reads it too: two VM-execution controls whose handler cases
# would otherwise never execute are turned on under it. See
# `trap_monitor_and_mwait` in hypervisor.cpp for why that is the only sound
# way to test them.
option(ZPP_GUEST_TESTS
    "Compile the guest-side VM-exit, EPT and emulation coverage suite"
    OFF)

# The module protection probe: one store into this module's own text, made
# from inside the hypervisor once it is running on its own page table.
#
# It exists because every step of the per-segment protection fails silently
# and in the safe-looking direction. The ELF permissions could be read
# wrong, the union could round the wrong way, CR0.WP could be clear, and in
# each case the page table reads correctly in a debugger and denies
# nothing. The only evidence that separates "protected" from "looks
# protected" is a fault, so this build makes one happen.
#
# Off, and off is the decision. The probe is not destructive - it reads the
# byte and writes the same byte back - but the launch ends in a verdict
# either way and the machine never reaches an operating system. That is the
# ZPP_GUEST_TESTS failure shape, and scripts/check-bootable.sh refuses it
# for the same reason.
#
# The passing result is error 0x60e03 printed by the loader: a page fault,
# error code 3, a present page written by a supervisor. Error 25 -
# module_protection_missing - is the failing one.
option(ZPP_TEST_MODULE_PROTECTION
    "Probe the module's own text with a store, to prove it is read-only"
    OFF)

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
        -DZPP_DIAG_RESERVE_WINDOW=${ZPP_DIAG_RESERVE_WINDOW}
        -DZPP_NESTED_VMX=${ZPP_NESTED_VMX}
        -DZPP_REQUEUE_INTERRUPTED_EVENTS=${ZPP_REQUEUE_INTERRUPTED_EVENTS}
        -DZPP_NESTED_TPR_SHADOW=${ZPP_NESTED_TPR_SHADOW}
        -DZPP_PROFILE_L2=${ZPP_PROFILE_L2}
        -DZPP_GUEST_TESTS=${ZPP_GUEST_TESTS}
        -DZPP_TEST_MODULE_PROTECTION=${ZPP_TEST_MODULE_PROTECTION}
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
option(ZPP_SEARCH_ALL_DEVICES
    "Look for the boot manager on every device, not only our own - the rig needs this"
    OFF)

option(ZPP_CHAINLOAD_ONLY
    "Chainload the boot manager without launching the hypervisor - a control run"
    OFF)

option(ZPP_TRACE
    "Compile the serial trace output in the loaders"
    ${ZPP_VERIFY_HYPERVISOR})

# The guest-side coverage suite: uefi_loader/include/zpp/guest_tests.h, run
# from inside the guest once the hypervisor is live, reporting one line per
# case on serial. scripts/ci/bochs-exit-coverage.sh grades the result.
#
# Off, and off is the decision rather than a convenience. It is a test
# facility and has no place in a loader that boots anything: it installs an
# interrupt descriptor table of its own, deliberately faults thirteen
# instructions, writes the local APIC's task priority register and then
# stops instead of chainloading. None of that destroys state the way
# ZPP_VERIFY_HYPERVISOR does - every register it writes is put back and no
# other processor is touched - but a build carrying it never reaches an
# operating system, which is the failure mode CLAUDE.md records as costing
# a full day when it was a stale cache entry rather than a deliberate
# choice. scripts/check-bootable.sh refuses one.

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
        -DZPP_GUEST_TESTS=${ZPP_GUEST_TESTS}
        -DZPP_NESTED_VMX=${ZPP_NESTED_VMX}
        -DZPP_REQUEUE_INTERRUPTED_EVENTS=${ZPP_REQUEUE_INTERRUPTED_EVENTS}
        -DZPP_NESTED_TPR_SHADOW=${ZPP_NESTED_TPR_SHADOW}
        -DZPP_PROFILE_L2=${ZPP_PROFILE_L2}
        -DZPP_TRACE=${ZPP_TRACE}
        -DZPP_CHAINLOAD_ONLY=${ZPP_CHAINLOAD_ONLY}
        -DZPP_SEARCH_ALL_DEVICES=${ZPP_SEARCH_ALL_DEVICES}
        -DZPP_DIAG=${ZPP_DIAG}
        -DZPP_DIAG_RESERVE_WINDOW=${ZPP_DIAG_RESERVE_WINDOW}
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

# === Hosted test suite (native, for the machine running the build) ===
add_subdirectory(tests)

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
