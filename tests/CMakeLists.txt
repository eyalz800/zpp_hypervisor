# The hosted test suite.
#
# Every harness here compiles natively for the machine running the build -
# no emulator, no target, no cross toolchain - which is what makes this the
# cheapest coverage in the tree and the part most likely to catch a
# mistake. The hypervisor sources it compiles are the real ones; where a
# translation unit reaches too much of the VMM to compile whole, a shim
# directory earlier on the include path supplies a stand-in header and the
# real source is compiled against it.
#
# Why this is CMake rather than the per-directory build.sh scripts it
# replaces, in the order the difference is felt:
#
#   - clangd. A shell script produces no compile_commands.json, so every
#     harness and every shim was edited with no index at all. The copy at
#     the bottom of this file fixes that for the whole directory.
#   - ctest. `add_test` gives per-test timeouts, one subprocess per test and
#     a report naming *which* tests failed, which is the whole of what
#     scripts/ci/run-host-tests.sh was written to provide by hand.
#   - one target per harness, so a single one can be built and run while it
#     is being worked on, rather than the suite being all or nothing.
#
# The root project is LANGUAGES NONE, because everything else it builds is
# an ExternalProject with a toolchain of its own. The host compiler is
# enabled here and nowhere else.
enable_language(CXX)

# The dialect and the warnings, identical for every harness. -Werror
# included: five of the build.sh scripts this replaces left it out, which
# meant a warning in a harness was invisible in exactly the files nobody
# rebuilds by hand.
set(ZPP_TEST_COMPILE_OPTIONS
    -std=c++2c
    -Wall
    -Wextra
    -Werror
    -fno-exceptions
    -fno-rtti)

# <print> is what these harnesses report through, so the standard library
# behind the host compiler has to have it. Probed rather than assumed,
# because the two libraries disagree about when it arrived: libc++ has it
# from 18, libstdc++ only from 14, and a clang on a distribution whose
# default g++ is 13 picks up that 13's headers and fails on the include
# alone.
#
# So: try it bare, and if that fails try again asking for libc++, which
# CI already installs beside the clang it names. Asking for libc++ is not
# a workaround either - the hypervisor sources these harnesses compile are
# built against libc++ headers in the cross build, so a harness using it
# is instantiating the same templates the target does.
#
# There is no fallback to std::printf, and there will not be one: a suite
# that reports through a different mechanism depending on the machine is a
# suite whose output cannot be compared across machines, which is the
# property the whole conversion was verified by.
#
# **Where the failure is raised matters, and getting that wrong broke
# three CI jobs that do not compile a single harness.** This was a
# `FATAL_ERROR`, which stops *configure* - and the root project always
# does `add_subdirectory(tests)`, so `cmake --preset debug` failed
# outright on any machine whose default compiler is older than this
# needs. That is exactly the runner the `build debug`, `build release`
# and `linux .ko` jobs use: they name clang only in the toolchain files
# their ExternalProjects use, so CMake probes the distribution's `c++`,
# which is GNU 13.3.0, which has no <print>. Every harness here is
# EXCLUDE_FROM_ALL and none of those jobs builds one. The suite had made
# itself a dependency of builds that never use it.
#
# So the requirement is unchanged and only its timing moved: a compiler
# that cannot build the suite now fails when the suite is *asked for*,
# not when a project containing it is configured. `zpp_tests` becomes a
# target that fails with the reason, and `zpp_add_host_test` registers
# nothing - so there is no silently-skipped harness and no ctest entry
# that would pass by not existing. scripts/ci/run-host-tests.sh builds
# `zpp_tests` before it runs ctest, so the host-tests job still goes red
# rather than green-with-nothing-run.
#
# The checks below that need no C++ compiler at all - check-nested-ept,
# check-exit-handler, check-shims, python_layout, check-invariants - stay
# registered either way. They were never what the probe was about.
include(CheckCXXSourceCompiles)

set(ZPP_PRINT_PROGRAM "#include <print>\nint main() { std::println(\"{}\", 1); }\n")

set(CMAKE_REQUIRED_FLAGS "-std=c++2c")
check_cxx_source_compiles("${ZPP_PRINT_PROGRAM}" ZPP_HAVE_PRINT)

if(NOT ZPP_HAVE_PRINT)
    set(CMAKE_REQUIRED_FLAGS "-std=c++2c -stdlib=libc++")
    check_cxx_source_compiles("${ZPP_PRINT_PROGRAM}" ZPP_HAVE_PRINT_LIBCXX)
    if(ZPP_HAVE_PRINT_LIBCXX)
        list(APPEND ZPP_TEST_COMPILE_OPTIONS -stdlib=libc++)
        set(ZPP_TEST_LINK_OPTIONS -stdlib=libc++)
    endif()
endif()

unset(CMAKE_REQUIRED_FLAGS)

if(ZPP_HAVE_PRINT OR ZPP_HAVE_PRINT_LIBCXX)
    set(ZPP_HOST_HARNESSES_BUILDABLE TRUE)
else()
    set(ZPP_HOST_HARNESSES_BUILDABLE FALSE)
    message(WARNING
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} has no "
        "<print>, with or without -stdlib=libc++, so the compiled "
        "harnesses under tests/ cannot be built by this configuration. "
        "Configuring anyway, because the cross builds do not use them. "
        "Building the `zpp_tests` target will fail with this reason. "
        "Install libc++ for this clang, or point CXX at a newer "
        "compiler.")
endif()

set(ZPP_HYPERVISOR_INCLUDE "${ZPP_SOURCE_DIR}/hypervisor/include")
set(ZPP_DIAG_INCLUDE "${ZPP_SOURCE_DIR}/diag/include")

# zpp/diag/config.h reads ZPP_DIAG directly rather than defaulting it, so
# every harness that includes a hypervisor source has to answer. Off,
# which is what a release build answers: the diagnostic channel's sinks
# write to an NVMe controller this suite does not have, so on would need
# them stood in for - and standing our own headers in for our own headers
# is what this suite is getting rid of. The calls under test compile away
# to nothing either way, which is exactly what the shim
# `zpp::diag::log` these harnesses used to carry did.
set(ZPP_TEST_DIAG 0)
set(ZPP_HYPERVISOR_SRC "${ZPP_SOURCE_DIR}/hypervisor/src")
set(ZPP_CI_DIR "${ZPP_SOURCE_DIR}/scripts/ci")

# Bounded, because a harness that hangs must fail the run rather than spend
# a CI budget. Generous, because the slowest of these assembles and
# disassembles a corpus of tens of thousands of instructions. Carried over
# unchanged from scripts/ci/run-host-tests.sh, which had it for the same
# reason.
set(ZPP_HOST_TEST_TIMEOUT_SECONDS 900 CACHE STRING
    "Seconds any one hosted test may run before ctest fails it")

# Builds every harness and runs none of them, which is what a check of
# "does the suite still compile" wants and what CI's host-tests job builds
# before handing over to ctest.
if(ZPP_HOST_HARNESSES_BUILDABLE)
    add_custom_target(zpp_tests
        COMMENT "Building every hosted test harness")
else()
    # The same target, and it fails. Not absent: a missing target makes
    # `cmake --build --target zpp_tests` fail with "no rule to make
    # target", which says nothing about why, and it would make anyone
    # reading the job think the suite had been removed rather than that
    # this compiler cannot build it.
    #
    # The message says "the C++23 print header" rather than spelling it
    # in angle brackets, and that is not fussiness: `cmake -E echo` runs
    # its arguments through a shell, which reads the brackets as a
    # redirection and turns the explanation into
    # "/bin/sh: print: No such file or directory".
    add_custom_target(zpp_tests
        COMMAND ${CMAKE_COMMAND} -E echo
            "the hosted harnesses report through std::println, and the C++23 print header is not available with ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, with or without -stdlib=libc++"
        COMMAND ${CMAKE_COMMAND} -E echo
            "install libc++ for this clang, or point CXX at a newer compiler, and re-run cmake"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "The hosted harnesses cannot be built by this compiler")
endif()

# Declares one harness: an executable of the same name, a ctest entry that
# runs it, and a dependency edge from zpp_tests.
#
#   SOURCES   the harness and whatever real hypervisor sources it compiles
#   INCLUDES  directories ahead of hypervisor/include, shims first
#   DEFINES   compile definitions, e.g. ZPP_NESTED_VMX=1
#   TEST_NAME the name ctest reports, when it differs from the target's
#   DIAG      ZPP_DIAG for this harness, when ZPP_TEST_DIAG will not do
function(zpp_add_host_test name)
    # Registers nothing where the compiler cannot build a harness, so
    # there is no ctest entry that would pass by not running. The loud
    # part is the `zpp_tests` target above, which fails; this half only
    # makes sure the failure cannot be mistaken for a pass.
    if(NOT ZPP_HOST_HARNESSES_BUILDABLE)
        return()
    endif()

    cmake_parse_arguments(HARNESS
        ""
        "TEST_NAME;DIAG"
        "SOURCES;INCLUDES;DEFINES"
        ${ARGN})

    if(NOT HARNESS_TEST_NAME)
        set(HARNESS_TEST_NAME "tests/${name}")
    endif()

    if(NOT DEFINED HARNESS_DIAG)
        set(HARNESS_DIAG ${ZPP_TEST_DIAG})
    endif()

    # EXCLUDE_FROM_ALL, so `cmake --build --preset <config>` still builds
    # exactly the four cross targets it always did. This suite is native
    # and the rest of the tree is not, and a harness that fails to compile
    # must not be able to stop a deploy - build `zpp_tests` for it, which
    # is what scripts/ci/run-host-tests.sh and ci.yml both do. It stays in
    # compile_commands.json either way, which is what clangd needs.
    add_executable(${name} EXCLUDE_FROM_ALL ${HARNESS_SOURCES})
    target_compile_options(${name} PRIVATE ${ZPP_TEST_COMPILE_OPTIONS})
    if(ZPP_TEST_LINK_OPTIONS)
        target_link_options(${name} PRIVATE ${ZPP_TEST_LINK_OPTIONS})
    endif()
    # diag/include last, and unconditionally: the hypervisor sources
    # include zpp/diag/log.h, whose real header is header-only and
    # compiles natively, so there is nothing here for a harness to
    # choose between.
    target_include_directories(${name} PRIVATE
        ${HARNESS_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ZPP_HYPERVISOR_INCLUDE}"
        "${ZPP_DIAG_INCLUDE}")
    target_compile_definitions(${name} PRIVATE
        ${HARNESS_DEFINES}
        ZPP_DIAG=${HARNESS_DIAG})

    add_dependencies(zpp_tests ${name})

    add_test(NAME "${HARNESS_TEST_NAME}" COMMAND ${name})
    set_tests_properties("${HARNESS_TEST_NAME}" PROPERTIES
        TIMEOUT ${ZPP_HOST_TEST_TIMEOUT_SECONDS})
endfunction()

# === Harnesses that compile a header or a whole source file ==============

# The ELF relocation regression. zpp/elf_file.h is freestanding and
# depends on nothing but the standard library, so the header under test
# compiles here exactly as the loader compiles it.
#
# What it pins: every relocated slot coming out as the module base, which
# was silent until .init_array became the first slot anything
# dereferenced. Native, no emulator and no target, and that is the point -
# the cheapest possible way to catch it is to relocate an image in host
# memory and read the words back.
zpp_add_host_test(elf_relocate
    SOURCES elf_relocate/harness.cpp)

# zpp::small_map, the fixed-capacity sorted map the hypervisor keeps
# `module_physical_to_virtual` in. A header depending on nothing but the
# standard library, so it compiles here exactly as the hypervisor
# compiles it.
#
# What it pins: `end()` on an *empty* map computed
# `std::addressof(value(m_size - 1)) + 1`, and `m_size - 1` wraps on an
# unsigned zero - so it formed a pointer one element before the storage
# array and walked back. Same pointer, undefined behaviour, and it
# survived every run of this suite on three platforms. Found by
# UndefinedBehaviorSanitizer over tests/watched_page rather than by a
# failure, which is the argument for this file existing at all: the
# container had no test of any kind.
zpp_add_host_test(small_map
    SOURCES small_map/harness.cpp)

# The MTRR memory-type derivation. zpp/arch/x86_64/mtrr.h depends on
# memory_type.h, msr.h and the standard library and on nothing else, so
# the header under test compiles here exactly as the hypervisor compiles
# it. Pure and constexpr, so most of the assertions are made at compile
# time and the compile is itself the test; the runtime half exists so a
# failure names the address that disagreed.
#
# What it pins: 63a5d17, where every range no variable MTRR covered got
# write-back including the MMIO hole above the top of DRAM, and 83012e7,
# where the variable-range array was sized 8 while real Intel client
# parts report a VCNT of 10.
zpp_add_host_test(mtrr
    SOURCES mtrr/harness.cpp)

# The host page table, the OS page table walker and the paging-structure
# helpers underneath both. The two translation units are the real ones,
# compiled rather than copied, so a change to either is a change to what
# runs here. If a future page_table starts calling invlpg or reading CR3
# this build breaks, which is the right moment to add a shim rather than
# to weaken the test.
#
# What it pins, all three found by writing it and none fixed by it: a
# large-page translation shifted nine bits too far in both walkers, an
# unmapped address answered with its own page offset instead of being
# refused, and a protection key read one bit low. The harness asserts what
# the code does today and says so in each message, so a fix turns a named
# check red.
zpp_add_host_test(page_table
    SOURCES
        page_table/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/os_page_table.cpp")

# The instruction decoder, differentially against LLVM's own assembler and
# disassembler. instruction.h and context.h are freestanding headers that
# depend on nothing but the standard library, so the header under test is
# compiled here exactly as the hypervisor compiles it.
#
# The corpus is generated, assembled and parsed by the harness itself
# rather than by the build, so every entry keeps its expectations in the
# same object that carries its bytes. An earlier round of this comparison
# was run and thrown away, and its result - ten thousand accepted
# instructions with no length mismatch - had to be taken on trust
# afterwards because nothing was left to re-run. That is why this one is
# committed.
zpp_add_host_test(decoder
    SOURCES decoder/harness.cpp)

# The nested VM-exit reflection decision: the real l0_wants_l2_exit and
# l1_wants_l2_exit, compiled against the real hypervisor class.
#
# The first harness to drop its copy of that class, and the shape the
# rest follow. Three things replaced 338 lines of stand-in:
#
#   - ZPP_HOSTED_TESTS, which widens the class's one `private:` to
#     `public:` and is defined here and nowhere else. The comment on it
#     in hypervisor.h carries the reasoning and the layout measurement.
#   - the real sources for what the stand-in used to fake. Its
#     `copy_vmcs12_to_shadow` was an empty body; nested_shadow_vmcs.cpp
#     is the real one, and returns early here because shadowing is off.
#     Its `page_table_stub::virtual_to_physical` returned its argument;
#     page_table.cpp is the real walker.
#   - support/host_heap.cpp plus crt/heap.cpp, so the real
#     `zpp::hypervisor::log` allocates its lines from a real arena
#     instead of the stand-in's `log` that discarded them.
zpp_add_host_test(nested_exit
    SOURCES
        nested_exit/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_entry.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_shadow_vmcs.cpp"
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_exit"
    DEFINES ZPP_NESTED_VMX=1 ZPP_HOSTED_TESTS=1)

# The nested extended page table rules, exhaustively.
#
# zpp/arch/x86_64/vmx/nested_ept.h depends on ept.h and memory_type.h and
# on nothing else, so the header under test compiles here exactly as the
# hypervisor compiles it - no shim, no stand-in, and no hypervisor object,
# because every function in it is pure and reaches memory only through a
# callback.
#
# Why it earns a harness of its own, beside check-nested-ept.sh rather
# than inside it: that script asserts a handful of static_asserts over the
# same header and cannot call anything, because the interesting functions
# are templates taking a reader. Everything nested rests on these five
# functions and until now the only thing that ever called them was a
# second-level guest fault on real hardware - where a wrong answer is not
# a crash but a shadow entry granting slightly the wrong thing, and the
# symptom is a guest livelocked on one address a million exits later.
#
# The permission algebra is swept exhaustively rather than sampled: there
# are sixteen permission sets and two settings of execute-only support, so
# the whole input space is 512 pairs and sampling it would be a choice
# with no argument behind it.
zpp_add_host_test(nested_ept
    SOURCES nested_ept/harness.cpp)

# The nested VMX state machine: the real nested_vmx.cpp and the real
# vmcs12.h against the real hypervisor class, with only the two asm.h
# stand-ins on the path.
#
# nested_vmx/fields.inc is every encoding in vmcs_fields.h, and this is
# the only record of how it was made. Regenerate it after editing that
# header with:
#
#   python3 - <<'EOF'
#   import re
#   src = open(SRC + '/zpp/arch/x86_64/vmx/vmcs_fields.h').read()
#   body = src.split('{',2)[2]
#   print('\n'.join('    {"%s", %s},' % m.groups()
#         for m in re.finditer(r'^\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+),',
#                              body, re.M)))
#   EOF
zpp_add_host_test(nested_vmx
    SOURCES
        nested_vmx/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_vmx.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_shadow_vmcs.cpp"
        # initialize_vmcs_shadowing resolves the shadow region's physical
        # address, and only on the path taken when shadowing is on - so
        # while it was off this link succeeded without the walker and the
        # gap was invisible. Turning the switch on is what found it.
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx"
    DEFINES ZPP_NESTED_VMX=1 ZPP_HOSTED_TESTS=1)

# Application-processor start-up and adoption, with host threads standing
# in for logical processors: the real slot allocator, the real interrupt
# command decode and the real INIT and start-up state, out of start_up.cpp
# and local_apic.cpp.
zpp_add_host_test(ap_start_up
    SOURCES
        ap_start_up/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/start_up.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/interrupt_command.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic.cpp"
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/ap_start_up/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/ap_start_up"
    DEFINES ZPP_HOSTED_TESTS=1)

# The interrupt command register's decode path: the real processor_slot,
# on_interrupt_command and start_up_broadcast out of interrupt_command.cpp
# and the real note_apic_mode and intercept_interrupt_command out of
# local_apic.cpp, against the real hypervisor class, a fake roster and
# fake hardware.
zpp_add_host_test(local_apic
    SOURCES
        local_apic/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/interrupt_command.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/local_apic"
    DEFINES ZPP_HOSTED_TESTS=1)

# The watched-page write path: the real watch_guest_page_writes,
# on_ept_violation, carry_out_guest_instruction and on_monitor_trap_flag
# out of watched_page.cpp, and the real filter_local_apic_write and
# on_local_apic_write out of local_apic_write.cpp, against the real
# hypervisor class.
zpp_add_host_test(watched_page
    SOURCES
        watched_page/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/watched_page.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic_write.cpp"
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/watched_page"
    DEFINES ZPP_HOSTED_TESTS=1)

# Guest memory: the real read_guest_physical, write_guest_physical and
# guest_linear_to_physical out of guest_memory.cpp, against the real
# hypervisor class.
#
# One stand-in, `map_window_at`, defined by the harness over an array of
# host memory. It cannot run here for the same reason the two asm.h
# headers cannot: it writes host paging structures and issues `invlpg`.
# Everything else is the real code - the bounds arithmetic, the
# page-at-a-time loop, and the whole four-level walk.
#
# Why it earns a harness: everything nested reads a guest hypervisor's
# memory through these - vmcs12, the VMXON region, the guest's own
# extended page tables - and a wrong answer is not a crash but a
# *plausible* address belonging to the wrong page.
zpp_add_host_test(guest_memory
    SOURCES
        guest_memory/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/guest_memory.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/guest_memory"
    DEFINES ZPP_HOSTED_TESTS=1)

# The interrupted-event re-queue on the way back to a guest: the real
# resume_guest and event_allowed_on_entry out of resume.cpp, against the
# real hypervisor class. The shim's restore_context comes back instead
# of trapping, because resume_guest is [[noreturn]] and everything worth
# reading is written just before it.
#
# DIAG=1, alone in this file. The resume path's diagnostic half - the
# pump and the block sink's readiness - is under test here, and with
# ZPP_DIAG=0 it would compile away and this harness would pin nothing
# about it. Four stand-in headers used to supply that half; the real
# zpp/diag/config.h, log.h, pump.h, sinks.h and sinks/esp_blocks.h are
# on the path instead, so what answers `esp_block_sink::ready()` is the
# sink itself.
zpp_add_host_test(resume_guest
    SOURCES
        resume_guest/harness.cpp
        support/host_heap.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/resume.cpp"
        "${ZPP_HYPERVISOR_SRC}/diag/platform.cpp"
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/resume_guest"
    DEFINES
        ZPP_NESTED_VMX=1
        ZPP_HOSTED_TESTS=1
        # The path under test is the one behind this switch, so it is on
        # here whatever the cross build defaults it to. It used to be
        # `#define`d at the top of the stand-in hypervisor.h, which put a
        # build option inside a header - the reason it moved.
        ZPP_REQUEUE_INTERRUPTED_EVENTS=1
    DIAG 1)

# === Compile-only checks ==================================================
#
# Their assertions are static_asserts, so the compile *is* the run and the
# executable only says so on standard output. They were shell scripts under
# scripts/ci/ that did nothing a target does not: run the host compiler
# over one file with one include directory.
zpp_add_host_test(decoder_test
    TEST_NAME check-decoder
    SOURCES "${ZPP_CI_DIR}/decoder-test.cpp")

zpp_add_host_test(instruction_test
    TEST_NAME check-instruction
    SOURCES "${ZPP_CI_DIR}/instruction-test.cpp")

# === The decoder's differential needs LLVM's own tools ====================
#
# Homebrew keeps its LLVM off the default PATH, and the toolchain this tree
# cross-compiles with is exactly that one - so ask CMake to find it in the
# brew prefix rather than writing the prefix down, which would be an
# absolute path that is wrong on any other machine.
if(NOT DEFINED ZPP_BREW_LLVM_PREFIX)
    find_program(ZPP_BREW brew)
    set(ZPP_BREW_LLVM_PREFIX "")
    if(ZPP_BREW)
        execute_process(
            COMMAND "${ZPP_BREW}" --prefix llvm
            OUTPUT_VARIABLE ZPP_BREW_LLVM_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    set(ZPP_BREW_LLVM_PREFIX "${ZPP_BREW_LLVM_PREFIX}" CACHE INTERNAL
        "Homebrew's LLVM prefix, empty where there is no brew")
endif()

find_program(ZPP_LLVM_MC llvm-mc HINTS "${ZPP_BREW_LLVM_PREFIX}/bin")
find_program(ZPP_LLVM_OBJDUMP llvm-objdump
    HINTS "${ZPP_BREW_LLVM_PREFIX}/bin")

if(NOT ZPP_HOST_HARNESSES_BUILDABLE)
    # There is no tests/decoder to give an environment to.
elseif(ZPP_LLVM_MC AND ZPP_LLVM_OBJDUMP)
    set_tests_properties("tests/decoder" PROPERTIES
        ENVIRONMENT
            "LLVM_MC=${ZPP_LLVM_MC};LLVM_OBJDUMP=${ZPP_LLVM_OBJDUMP}")
else()
    message(WARNING
        "llvm-mc or llvm-objdump not found - tests/decoder will look for "
        "them on PATH and fail if they are not there")
endif()

# The freestanding C runtime: the heap, the string and memory primitives,
# scope_exit, the allocator and the container aliases, zpp::error, and the
# init and fini array machinery.
#
# The only harness that *includes* a hypervisor source rather than linking
# it, and the reason is in harness.cpp: crt.cpp is a replacement C runtime
# whose memcpy, __cxa_* and __dso_handle would preempt the host's if they
# were linked in, so the harness renames them with #define and includes the
# file. hypervisor/src/crt is on the include path for that one line.
zpp_add_host_test(crt
    SOURCES
        crt/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/crt/heap.cpp"
    INCLUDES "${ZPP_HYPERVISOR_SRC}/crt")

# === Checks that are not a compile =======================================

# The nested EPT tests. Two tiers: static_asserts over nested_ept.h, and
# the shadow table pool and second-level fault decision, whose bodies are
# cut out of nested_ept.cpp and nested_entry.cpp by name and by anchor.
# Still a script because of the second tier - the anchors cut *fragments*
# from inside one function, which no include path arrangement can do.
add_test(NAME check-nested-ept COMMAND "${ZPP_CI_DIR}/check-nested-ept.sh")

# The source-level invariants on the VM exit handler. It reads the source
# and needs no build at all, which is why it is a script and stays one.
add_test(NAME check-exit-handler
    COMMAND "${ZPP_CI_DIR}/check-exit-handler.sh")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # What this suite is allowed to stand in for. Replaces
    # check-shim-members.py, which compared six copies of the hypervisor
    # class against the real header - there are no copies left, so the
    # compiler makes that comparison now. This guards the rule those
    # copies broke instead: shim the hardware, not the code under test.
    add_test(NAME check-shims
        COMMAND "${Python3_EXECUTABLE}" "${ZPP_CI_DIR}/check-shims.py")

    # The Python readers under scripts/, against the C++ they transcribe.
    # Those scripts are the one place a wrong constant produces *plausible*
    # output rather than an error, which is the failure mode that sends an
    # investigation to the hypervisor instead of to the reader.
    add_test(NAME tests/python_layout
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/python_layout"
            -t "${CMAKE_CURRENT_SOURCE_DIR}/python_layout")
else()
    message(WARNING
        "no Python 3 interpreter - check-shims and "
        "tests/python_layout will not run")
endif()

# The ELF invariants, which grade a built hypervisor rather than compiling
# anything. Reported as skipped rather than failed when the binary is not
# there, because this suite is also what a developer runs before touching
# the cross build.
add_test(NAME check-invariants
    COMMAND "${ZPP_CI_DIR}/check-invariants.sh" "${ZPP_CONFIG_LOWER}")
set_tests_properties(check-invariants PROPERTIES SKIP_RETURN_CODE 77)

foreach(_test IN ITEMS check-nested-ept check-exit-handler check-invariants)
    set_tests_properties(${_test} PROPERTIES
        TIMEOUT ${ZPP_HOST_TEST_TIMEOUT_SECONDS})
endforeach()

# Copy the compile database into tests/ after any harness builds, the same
# way the root CMakeLists.txt does it for each sub-build. This is the whole
# reason clangd works in this directory: the shims are on the include path
# ahead of hypervisor/include, and no editor can guess that.
if(ZPP_HOST_HARNESSES_BUILDABLE)
    add_custom_command(TARGET elf_relocate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
        COMMENT "Copying compile_commands.json to tests/")
endif()
