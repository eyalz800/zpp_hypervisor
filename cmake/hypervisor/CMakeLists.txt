option(ZPP_DIAG_RESERVE_WINDOW
    "Declare a reserved memory window for the storage controller"
    OFF)
option(ZPP_REQUEUE_INTERRUPTED_EVENTS
    "Put back an event whose delivery a VM exit interrupted"
    ON)

option(ZPP_NESTED_TPR_SHADOW
    "Hand a guest hypervisor's TPR shadow to the processor. Off forces CR8 exiting and emulates it against the same page, which is slower and exists to be the one variable between two boots."
    ON)

option(ZPP_NESTED_SHADOW_VMCS
    "Let the processor shadow a guest hypervisor's VMCS, so its vmreads and vmwrites do not exit. Off makes every one of them exit and be recorded, which is the only way to see what it actually asks for."
    ON)

option(ZPP_PROFILE_L2
    "Sample a second-level guest's instruction pointer on the VMX-preemption timer. A profiler: it costs an exit per interval and is the only instrument that can see a guest spinning on memory."
    OFF)
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

# Project include directories. hypervisor/include comes first so that this
# program's own zpp/diag/sinks.h - the list of channels the resident side can
# drive - is what pump.h finds, while everything else about the diagnostic
# facility comes from the shared diag/include.
target_include_directories(zpp_hypervisor PRIVATE
    "${ZPP_SOURCE_DIR}/hypervisor/include"
    "${ZPP_SOURCE_DIR}/diag/include"
    # The launch parameter structure the loaders hand over. Shared rather
    # than duplicated, so the two sides of that boundary cannot drift -
    # the header is C compatible precisely so both can include it.
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
    # Build the module out of relative paths, so the same source produces
    # the same bytes wherever it is checked out.
    #
    # It does not, otherwise. `std::source_location` is how the log records
    # where a line came from, and its file name is ordinary data in
    # .rodata - stripping removes debug information and leaves that
    # untouched - so the absolute build path ends up inside the binary.
    # Measured: a release build carried
    # ".../zpp_hypervisor/hypervisor/src/hypervisor/hypervisor.cpp", and
    # the same tree built in a git worktree carried the worktree's path
    # instead, so the two binaries differed for no reason but their
    # location. That made check-nested-absent.sh's baseline hash
    # unreproducible across checkouts, which is what exposed it.
    #
    # Both forms are given because they cover different things: the
    # -ffile-prefix-map form covers __FILE__ and source_location, and the
    # -fdebug-prefix-map form covers what lands in debug information for
    # the debug build.
    "-ffile-prefix-map=${ZPP_SOURCE_DIR}="
    "-fdebug-prefix-map=${ZPP_SOURCE_DIR}="
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
    # The diagnostic facility follows the tree's existing pattern: an always
    # defined 0/1 macro, turned into a constexpr bool by the header, never an
    # #ifdef. Release hardcodes 0 rather than honouring the option, so no cache
    # entry and no command line can put diagnostics into a release binary.
    $<$<CONFIG:Debug>:-DZPP_DIAG=$<BOOL:${ZPP_DIAG}>>
    $<$<CONFIG:Release>:-DZPP_DIAG=0>
    $<$<CONFIG:Debug>:-DZPP_DIAG_RESERVE_WINDOW=$<BOOL:${ZPP_DIAG_RESERVE_WINDOW}>>
    $<$<CONFIG:Release>:-DZPP_DIAG_RESERVE_WINDOW=0>
    # Nested VMX. Same shape as the diagnostic switch - an always defined 0/1
    # macro turned into a constexpr bool by nested_vmx.h - and defined for both
    # configurations rather than debug only, because unlike diagnostics this is
    # a change to what the *guest* is told and a release build has to be able
    # to carry it once it works. It defaults off in both; see nested_vmx.h for
    # what is still missing.
    -DZPP_NESTED_VMX=$<BOOL:${ZPP_NESTED_VMX}>
    -DZPP_REQUEUE_INTERRUPTED_EVENTS=$<BOOL:${ZPP_REQUEUE_INTERRUPTED_EVENTS}>
    -DZPP_NESTED_TPR_SHADOW=$<BOOL:${ZPP_NESTED_TPR_SHADOW}>
    -DZPP_NESTED_SHADOW_VMCS=$<BOOL:${ZPP_NESTED_SHADOW_VMCS}>
    -DZPP_PROFILE_L2=$<BOOL:${ZPP_PROFILE_L2}>
    # The guest coverage suite, which lives in the loader but turns on two
    # VM-execution controls here. Same always-defined 0/1 shape as the two
    # above. Off by default and off in every build that is not a test one;
    # scripts/check-bootable.sh refuses a loader carrying the suite, and a
    # hypervisor built with this is only ever embedded in such a loader.
    -DZPP_GUEST_TESTS=$<BOOL:${ZPP_GUEST_TESTS}>
    # The module protection probe. Debug only and hardcoded off in release,
    # for the same reason the diagnostic switch is: it ends the launch in a
    # verdict instead of a guest, and no cache entry or command line should
    # be able to put that in a release binary.
    $<$<CONFIG:Debug>:-DZPP_TEST_MODULE_PROTECTION=$<BOOL:${ZPP_TEST_MODULE_PROTECTION}>>
    $<$<CONFIG:Release>:-DZPP_TEST_MODULE_PROTECTION=0>
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
