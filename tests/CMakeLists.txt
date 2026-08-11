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

set(ZPP_HYPERVISOR_INCLUDE "${ZPP_SOURCE_DIR}/hypervisor/include")
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
add_custom_target(zpp_tests
    COMMENT "Building every hosted test harness")

# Declares one harness: an executable of the same name, a ctest entry that
# runs it, and a dependency edge from zpp_tests.
#
#   SOURCES   the harness and whatever real hypervisor sources it compiles
#   INCLUDES  directories ahead of hypervisor/include, shims first
#   DEFINES   compile definitions, e.g. ZPP_NESTED_VMX=1
#   TEST_NAME the name ctest reports, when it differs from the target's
function(zpp_add_host_test name)
    cmake_parse_arguments(HARNESS
        ""
        "TEST_NAME"
        "SOURCES;INCLUDES;DEFINES"
        ${ARGN})

    if(NOT HARNESS_TEST_NAME)
        set(HARNESS_TEST_NAME "tests/${name}")
    endif()

    # EXCLUDE_FROM_ALL, so `cmake --build --preset <config>` still builds
    # exactly the four cross targets it always did. This suite is native
    # and the rest of the tree is not, and a harness that fails to compile
    # must not be able to stop a deploy - build `zpp_tests` for it, which
    # is what scripts/ci/run-host-tests.sh and ci.yml both do. It stays in
    # compile_commands.json either way, which is what clangd needs.
    add_executable(${name} EXCLUDE_FROM_ALL ${HARNESS_SOURCES})
    target_compile_options(${name} PRIVATE ${ZPP_TEST_COMPILE_OPTIONS})
    target_include_directories(${name} PRIVATE
        ${HARNESS_INCLUDES}
        "${ZPP_HYPERVISOR_INCLUDE}")
    if(HARNESS_DEFINES)
        target_compile_definitions(${name} PRIVATE ${HARNESS_DEFINES})
    endif()

    add_dependencies(zpp_tests ${name})

    add_test(NAME "${HARNESS_TEST_NAME}" COMMAND ${name})
    set_tests_properties("${HARNESS_TEST_NAME}" PROPERTIES
        TIMEOUT ${ZPP_HOST_TEST_TIMEOUT_SECONDS})
endfunction()

# === Harnesses that compile a header or a whole source file ==============

# The ELF relocation regression. zpp/elf_file.h is freestanding and depends
# on nothing but the standard library, so the header under test compiles
# here exactly as the loader compiles it. What it pins: every relocated
# slot coming out as the module base, which was silent until .init_array
# became the first slot anything dereferenced.
zpp_add_host_test(elf_relocate
    SOURCES elf_relocate/harness.cpp)

# The MTRR memory-type derivation. Pure and constexpr, so most of the
# assertions are made at compile time and the compile is itself the test;
# the runtime half exists so a failure names the address that disagreed.
zpp_add_host_test(mtrr
    SOURCES mtrr/harness.cpp)

# The host page table, the OS page table walker and the paging-structure
# helpers underneath both. The two translation units are the real ones,
# compiled rather than copied, so a change to either is a change to what
# runs here.
zpp_add_host_test(page_table
    SOURCES
        page_table/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/page_table.cpp"
        "${ZPP_HYPERVISOR_SRC}/arch/x86_64/os_page_table.cpp")

# The instruction decoder, differentially against LLVM's own assembler and
# disassembler. The corpus is generated, assembled and parsed by the
# harness itself, so every entry keeps its expectations in the same object
# that carries its bytes.
zpp_add_host_test(decoder
    SOURCES decoder/harness.cpp)

# The nested VM-exit reflection decision: the real l0_wants_l2_exit and
# l1_wants_l2_exit, compiled against a stand-in hypervisor.h.
zpp_add_host_test(nested_exit
    SOURCES
        nested_exit/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_entry.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_exit/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_exit"
    DEFINES ZPP_NESTED_VMX=1)

# The nested VMX state machine: the real nested_vmx.cpp and the real
# vmcs12.h against stand-ins for hypervisor.h, the log and the two asm.h
# headers.
zpp_add_host_test(nested_vmx
    SOURCES
        nested_vmx/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/nested_vmx.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx"
    DEFINES ZPP_NESTED_VMX=1)

# Application-processor start-up and adoption, with host threads standing
# in for logical processors: the real slot allocator, the real interrupt
# command decode and the real INIT and start-up state, out of start_up.cpp
# and local_apic.cpp.
zpp_add_host_test(ap_start_up
    SOURCES
        ap_start_up/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/start_up.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/interrupt_command.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/ap_start_up/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/ap_start_up")

# The interrupt command register's decode path: the real processor_slot,
# on_interrupt_command and start_up_broadcast out of interrupt_command.cpp
# and the real note_apic_mode and intercept_interrupt_command out of
# local_apic.cpp, against a fake roster and fake hardware.
zpp_add_host_test(local_apic
    SOURCES
        local_apic/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/interrupt_command.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/local_apic/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/local_apic")

# The watched-page write path: the real watch_guest_page_writes,
# on_ept_violation, carry_out_guest_instruction and on_monitor_trap_flag
# out of watched_page.cpp, and the real filter_local_apic_write and
# on_local_apic_write out of local_apic_write.cpp.
zpp_add_host_test(watched_page
    SOURCES
        watched_page/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/watched_page.cpp"
        "${ZPP_HYPERVISOR_SRC}/hypervisor/local_apic_write.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/watched_page/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/watched_page")

# The interrupted-event re-queue on the way back to a guest: the real
# resume_guest and event_allowed_on_entry out of resume.cpp, against a
# stand-in carrying the per-processor event state and the VMCS. The shim's
# restore_context comes back instead of trapping, because resume_guest is
# [[noreturn]] and everything worth reading is written just before it.
zpp_add_host_test(resume_guest
    SOURCES
        resume_guest/harness.cpp
        "${ZPP_HYPERVISOR_SRC}/hypervisor/resume.cpp"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/resume_guest/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/nested_vmx/shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/resume_guest"
    DEFINES ZPP_NESTED_VMX=1)

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

if(ZPP_LLVM_MC AND ZPP_LLVM_OBJDUMP)
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
    # The harness shims against the class they stand in for. A member the
    # real source uses and a shim lacks is a compile error and needs no
    # help; a member present in both with a *different* declaration
    # compiles and tests the wrong thing, which is what this catches - it
    # has already caught a uint64_t that a shim spelled uint32_t.
    add_test(NAME check-shim-members
        COMMAND "${Python3_EXECUTABLE}" "${ZPP_CI_DIR}/check-shim-members.py")

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
        "no Python 3 interpreter - check-shim-members and "
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
add_custom_command(TARGET elf_relocate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
    COMMENT "Copying compile_commands.json to tests/")
