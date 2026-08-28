option(ZPP_DIAG_RESERVE_WINDOW
    "Declare a reserved memory window for the storage controller"
    OFF)
option(ZPP_REQUEUE_INTERRUPTED_EVENTS
    "Put back an event whose delivery a VM exit interrupted"
    ON)

option(ZPP_NESTED_TPR_SHADOW
    "Hand a guest hypervisor's TPR shadow to the processor. Off forces CR8 exiting and emulates it against the same page, which is slower and exists to be the one variable between two boots."
    ON)

option(ZPP_VIRTUALIZE_APIC
    "Take every external interrupt in the host and inject it into the guest, the way KVM does, instead of letting the guest's own interrupt controller deliver them natively. Off is this VMM's design; on exists to test whether that difference is what stalls the nested guest."
    OFF)

set(ZPP_LAZY_TICK "0" CACHE STRING
    "Withhold a clock interrupt arriving sooner than this many microseconds after the last delivered one.")

option(ZPP_DISARM_APIC_WATCH
    "Drop the local APIC page watch once no more processors are going to start."
    OFF)

option(ZPP_INTERCEPT_APIC
    "Watch the guest's local APIC. Off only makes sense with one processor."
    ON)

option(ZPP_HOLD_SELF_IPI_IN_VTL1
    "Swallow a self-directed undeliverable interrupt-command write only while VTL1 is running."
    OFF)

option(ZPP_KEEP_TICK_WHILE_VTL1_OWES
    "Do not withhold a clock tick while VTL1 has an unconsumed synthetic message."
    OFF)

option(ZPP_POLL_ON_HALT
    "Intercept HLT in the first level so a halt becomes an exit and the guest hypervisor polls instead of idling."
    OFF)

set(ZPP_LAZY_TICK_AFTER_PROTECT "0" CACHE STRING
    "Withhold nothing until the protection-mask call count reaches this; 0 withholds from the start.")

set(ZPP_LAZY_TICK_MAX "0" CACHE STRING
    "Most ticks ZPP_LAZY_TICK will ever withhold across the whole run; 0 means no limit.")

option(ZPP_POLL_L1
    "Arm the 1 ms preemption-timer poll on vmcs01 while the first level runs, independent of the lazy tick."
    OFF)

set(ZPP_LAZY_TICK_SECONDS "0" CACHE STRING
    "Seconds after the first withheld tick that ZPP_LAZY_TICK stays in force; 0 means for ever.")

option(ZPP_HOLD_CLOCK_IN_VTL1
    "Hold the clock interrupt while the secure kernel (VTL1) is current and deliver it on the first entry after VTL0 resumes. Bounded by one trust-level turn rather than a fixed gap."
    OFF)

option(ZPP_DELIVER_SELF_IPI
    "Deliver a second-level guest's self-directed interrupt when the guest hypervisor does not. See the root CMakeLists."
    OFF)

option(ZPP_INTERCEPT_SELF_IPI
    "Withhold a self-directed synthetic interrupt the requesting processor cannot take from the guest hypervisor, and deliver it here when it can. See the root CMakeLists."
    OFF)

option(ZPP_FORCE_DISPATCH_ONCE
    "DIAGNOSTIC. Deliver the deferred-call vector once, against the masking rule. See the root CMakeLists."
    OFF)

option(ZPP_EAGER_EPT_NEIGHBOURS
    "Install neighbouring pages on an EPT fault. See the root CMakeLists."
    OFF)

option(ZPP_PUBLISH_REFERENCE_TSC
    "Fill in the reference TSC page the guest hypervisor leaves invalid, with a scale computed from CPUID.15H where that enumerates and fitted over a long baseline where it does not. See the root CMakeLists."
    OFF)

option(ZPP_NESTED_SHADOW_VMCS
    "Let the processor shadow a guest hypervisor's VMCS, so its vmreads and vmwrites do not exit. ON: 88.0% -> 83.2% of wall clock in the handler, but only with the trimmed field list. See the root CMakeLists."
    ON)

set(ZPP_SLOW_EXITS "0" CACHE STRING
    "Extra cycles burned in every VM exit, to test whether the stall moves with throughput")

set(ZPP_TICK_FLOOR "0" CACHE STRING
    "Least periodic tick period, in 100 ns units, this VMM will let\n     the second-level guest arm; 0 leaves every request untouched.\n     156250 is 15.625 ms, the 64 Hz tick Windows itself uses before\n     it re-arms at 1.74 ms. Only periods are floored, never one-shot\n     deadlines.")

set(ZPP_TIME_DILATION "1" CACHE STRING
    "Charge the guest only 1/N of the time this VMM spends in root operation, by moving the time-stamp counter offset. 1 is off. See the root CMakeLists and nested_vmx.h.")

set(ZPP_STRETCH_GUEST_TIMER "1" CACHE STRING
    "Multiply the second-level guest's synthetic timer period by this before its hypervisor sees it. 1 is off. Diagnostic only - see the top-level CMakeLists for what it separates.")

option(ZPP_VTL_CAPTURE
    "Take the deep per-switch trust-level capture. OFF. See the root CMakeLists and nested_vmx.h."
    OFF)

option(ZPP_INIT_CLEARS_EFER
       "Clear IA32_EFER on the start-up path, per SDM Table 12-1"
       OFF)

option(ZPP_TRACK_LONG_MODE_SWITCH
       "Maintain 'IA-32e mode guest' and guest IA32_EFER.LMA across a guest paging transition, and own CR0.PG in the mask. See the root CMakeLists and nested_vmx.h."
       ON)

option(ZPP_TRAP_AP_FAULTS
       "Diagnostic. Record an application processor's first exception after it enables paging instead of letting it arrive as a bare triple fault. See the root CMakeLists and nested_vmx.h."
       OFF)

option(ZPP_PROBE_APS
       "Diagnostic. Probe silent application processors with a non-maskable interrupt and record what answered. See the root CMakeLists and nested_vmx.h."
       OFF)

option(ZPP_HONEST_EXIT_LENGTH
       "Report an exit instruction length only where the SDM defines it"
       ON)

option(ZPP_L2_STARTUP_SPIN
       "Spin while a second-level processor waits for its start-up IPI"
       ON)

option(ZPP_STEP_AP_WATCHED_WRITES
       "Step, rather than emulate, watched-page writes on processors other than the first"
       OFF)

option(ZPP_DROP_WATCH_ON_START_UP
       "Drop the local APIC page watch as soon as a start-up has been applied"
       OFF)

option(ZPP_WATCH_AP_PAGE_TABLE
       "Watch the page table entry mapping an application processor's descriptor table"
       OFF)

option(ZPP_APPLY_QUEUED_START_UP
    "Apply a start-up IPI that arrived before its target was waiting."
    ON)

option(ZPP_STEP_VTL
    "Step the trust-level loop with the monitor trap flag."
    OFF)

option(ZPP_EVMCS
    "Offer Hyper-V's enlightened VMCS, so the guest hypervisor states its VMCS in a shared structure instead of executing VMREAD and VMWRITE - 54% of a trust-level round trip measured on the rig. Off by default: the advertisement and the handling cannot be staged separately, so the first boot with this on exercises the whole mechanism at once."
    OFF)

option(ZPP_DEFER_GUEST_STATE
    "Defer the bulk copy of the second-level guest's state."
    ON)

option(ZPP_SHADOW_GUEST_STATE
    "Run the deferred guest-state copy in shadow."
    OFF)

option(ZPP_PROFILE_L2
    "Sample a second-level guest's instruction pointer on the VMX-preemption timer. A profiler: it costs an exit per interval and is the only instrument that can see a guest spinning on memory."
    OFF)

option(ZPP_SAMPLE_L1
    "Sample the FIRST-level guest's instruction pointer on the VMX-preemption timer, armed at VMCS setup rather than from the exit path. The application processors burn a whole host core each and take no exits, so every exit-driven instrument here is blind to them, and reading their state through the monitor is not passive - QEMU must signal them out of guest mode to do it. Costs one exit per period per processor, for ever."
    OFF)

option(ZPP_WINDOW_ON_TPR
    "Arm the guest hypervisor's interrupt window on a task-priority drop rather than on every interruptible moment. Interrupt-window exiting fires on RFLAGS.IF and ignores the task priority, so a hypervisor holding an interrupt the guest's priority blocks polls forever - measured at 3,055,183 requests. This defers the window until the priority actually drops, using the TPR threshold as the trigger. Off by default: it delays an interrupt whose priority the guest would have admitted, which is a behaviour change and not merely an optimisation."
    OFF)

option(ZPP_DELIVER_ON_DROP
    "Arm a TPR threshold where the guest hypervisor left none, and take NOTHING away. Withholding the interrupt window has now been measured twice and is wrong both times: it took vector 0x2f from 9,627 to 11 and the CLOCK from 388,241 to 5,550 in the same run, so it does not merely fail to help the vector being chased, it stops delivery of everything. In this VMM the interrupt window is the primary delivery mechanism for every vector and its 1,500,914 exits are load bearing. On, `primary` is untouched - every entry carries the controls the default build would give it, and window_deferred_count must read zero - and a threshold at the dispatch class is ADDED on entries where the level above is holding a vector the priority blocks and left the threshold at zero. A threshold of zero can never fire (SDM 32.1.2: IF VTPR[7:4] < TPR threshold), which is why its own notification never arrives; l2_tpr_threshold_seen is the check. At the drop the threshold is written back down and the window is ensured live in vmcs02 before the resume. Off by default so an A/B is one variable; a static assertion refuses it together with ZPP_WINDOW_ON_TPR."
    OFF)

option(ZPP_COUNT_DROPS
    "Account for the low-priority vector the guest asks for, from the ask to the delivery: asked, coalesced, pending at entry, delivered, and DROPPED - entries made while it was outstanding, at a priority that admits it, with nothing staged and nothing armed that could cause an exit at which the level above might stage it. The last is the number this work exists to take to zero and is the only counter in this tree that names a moment rather than an event. Only counts, so it is safe on in both arms of an A/B; not free - one guest-memory read and one VMREAD per second-level entry - which is why it is a switch."
    OFF)

option(ZPP_VMCS_CENSUS
    "Census every VMCS access by field and by caller. Free when an access is an exit to the layer below at about 4,600 cycles; the dominant cost once the enlightened VMCS makes an access a load or a store, measured at 1,606 cycles an access against roughly ten for the access itself. Off makes the field and caller tables read zero."
    ON)

option(ZPP_EVMCS_TO_KVM
    "Use Hyper-V's enlightened VMCS toward the layer *below* this VMM, replacing VMREAD and VMWRITE on the second-level VMCS with loads and stores to a page shared with it. Off by default and detected at run time: it is a contract with whatever is underneath, and this VMM is not specific to any. It is never advertised upward - the guest hypervisor keeps being told it is on bare metal - and it cannot be combined with offering VMCS shadowing to the guest, which the enlightened layout has no room for."
    OFF)

option(ZPP_EVMCS_MIXED
    "Enlighten only the second-level VMCS and keep offering the guest hypervisor VMCS shadowing. The two look mutually exclusive and are not: shadowing needs the VMREAD and VMWRITE bitmap pointers, which only vmcs01 carries, so vmcs02 can live in an enlightened page while vmcs01 stays a real region. Measured, each of the two configurations alone frees one side's VMCS accesses and pays the other's, and the second-level guest gets 7.6% of the machine either way - which is what stops the boot. Off by default: it needs an extra VMCLEAR on each side of the switch, one to release the enlightened pointer and one to make the layer below flush its cached vmcs01 instead of discarding it."
    OFF)

option(ZPP_STALL_BREAKER
    "Refuse to deliver the same vector twice at the same guest instruction, so the guest retires at least one between deliveries. Measured: two histograms of the guest's instruction pointer, one where the entry stages an event and one where it stages nothing, disagree completely - the control is flat across 192 addresses while 84% of injections land on two instructions, the first being the instruction immediately after a sti. The guest enables interrupts, takes one, returns to the same instruction and takes another, for ever. Withholding defers rather than drops, because the source is level-asserted: the timer message sits unconsumed in the guest's own message page. Off by default."
    OFF)

option(ZPP_INVEPT_ALL_PROCESSORS "propagate guest INVEPT to every processor" OFF)

option(ZPP_TRACE_VTL
    "Record Hyper-V virtual trust level switches - the VTL call and return hypercalls, the registers either side of them, and the guest stack above the caller. Purely observational: it reads guest state and writes only this VMM's own members, and folds to nothing when off. Off by default because it is specific to one guest hypervisor's interface, and this VMM is not specific to any guest. Costs several VMCS reads and a guest page walk per trust-level switch."
    OFF)

option(ZPP_WATCH_VTL_BLOCK
    "Watch the IUM context block for writes and record who makes them. The second-level guest loops on a state byte that never changes, and polling cannot distinguish nothing-writes-it from a-write-is-lost - those want opposite fixes. Costs an EPT violation per write to a live kernel stack page."
    OFF)

option(ZPP_TRACE_AP_ENTRY
    "Log the whole guest state - every segment selector, base, limit and access-rights field, the control registers and their read shadows, IA32_EFER, RFLAGS, RIP, RSP, both descriptor tables, and the activity, interruptibility and entry-control fields - at an application processor's first VM entry, at every application of the INIT-plus-start-up state, and at any triple fault. Observational: it writes log lines and changes no VMCS field, so an A/B is still one variable. Off by default because the ring is shared with every other investigation. Self falsifying: the boot processor prints `ap-entry instrument armed` at its own launch and every dump carries the count of application-processor first entries, so `zero` at a triple fault says no application processor was ever entered rather than leaving that indistinguishable from silence. See nested_vmx.h."
    OFF)

option(ZPP_ANNOUNCE_HYPERVISOR_BIT
    "Set CPUID leaf 1 ECX bit 31 - you are virtualized - without advertising a hypervisor interface block. The bit and the block are different claims: the block makes Windows' hypervisor stand down, the bit decides which VBS path it takes. See nested_vmx.h."
    OFF)

option(ZPP_CENSUS_EXITS
    "Fill the exit ring's qualification, activity state and CS selector, and the CPL census beside them. Three VMCS reads on every exit, which is 7.8 per round trip at the measured 2.61 exits per round trip, and every one of them is an exit to KVM on a host without VMCS shadowing. OFF: the ring still records the reason, the instruction pointer and the guest-physical address, and the terminal records keep their full detail. See nested_vmx.h."
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
    # The framebuffer the loader handed over. Recorded only; nothing here
    # draws. Off, the fields stay zero, which is what a machine reporting
    # no graphics output protocol already produces.
    $<$<CONFIG:Debug>:-DZPP_FRAMEBUFFER=$<BOOL:${ZPP_FRAMEBUFFER}>>
    $<$<CONFIG:Release>:-DZPP_FRAMEBUFFER=0>
    # The VMCS field cache. Defined for both configurations rather than
    # debug only: unlike a diagnostic this changes what the hot path does,
    # so a release build has to be able to ask for it.
    -DZPP_VMCS_CACHE=$<BOOL:${ZPP_VMCS_CACHE}>
    -DZPP_NESTED_VID=$<BOOL:${ZPP_NESTED_VID}>
    -DZPP_SUPPRESS_VINA=$<BOOL:${ZPP_SUPPRESS_VINA}>
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
    -DZPP_PUBLISH_REFERENCE_TSC=$<BOOL:${ZPP_PUBLISH_REFERENCE_TSC}>
    -DZPP_DELIVER_SELF_IPI=$<BOOL:${ZPP_DELIVER_SELF_IPI}>
    -DZPP_HOLD_CLOCK_IN_VTL1=$<BOOL:${ZPP_HOLD_CLOCK_IN_VTL1}>
    -DZPP_LAZY_TICK_SECONDS=${ZPP_LAZY_TICK_SECONDS}
    -DZPP_POLL_L1=$<BOOL:${ZPP_POLL_L1}>
    -DZPP_LAZY_TICK_MAX=${ZPP_LAZY_TICK_MAX}
    -DZPP_LAZY_TICK_AFTER_PROTECT=${ZPP_LAZY_TICK_AFTER_PROTECT}
    -DZPP_POLL_ON_HALT=$<BOOL:${ZPP_POLL_ON_HALT}>
    -DZPP_KEEP_TICK_WHILE_VTL1_OWES=$<BOOL:${ZPP_KEEP_TICK_WHILE_VTL1_OWES}>
    -DZPP_HOLD_SELF_IPI_IN_VTL1=$<BOOL:${ZPP_HOLD_SELF_IPI_IN_VTL1}>
    -DZPP_INTERCEPT_SELF_IPI=$<BOOL:${ZPP_INTERCEPT_SELF_IPI}>
    -DZPP_FORCE_DISPATCH_ONCE=$<BOOL:${ZPP_FORCE_DISPATCH_ONCE}>
    -DZPP_EAGER_EPT_NEIGHBOURS=$<BOOL:${ZPP_EAGER_EPT_NEIGHBOURS}>
    -DZPP_INTERCEPT_APIC=$<BOOL:${ZPP_INTERCEPT_APIC}>
    -DZPP_DISARM_APIC_WATCH=$<BOOL:${ZPP_DISARM_APIC_WATCH}>
    -DZPP_LAZY_TICK=${ZPP_LAZY_TICK}
    -DZPP_VIRTUALIZE_APIC=$<BOOL:${ZPP_VIRTUALIZE_APIC}>
    -DZPP_PROFILE_L2=$<BOOL:${ZPP_PROFILE_L2}>
    -DZPP_CENSUS_EXITS=$<BOOL:${ZPP_CENSUS_EXITS}>
    -DZPP_SAMPLE_L1=$<BOOL:${ZPP_SAMPLE_L1}>
    -DZPP_WINDOW_ON_TPR=$<BOOL:${ZPP_WINDOW_ON_TPR}>
    -DZPP_DELIVER_ON_DROP=$<BOOL:${ZPP_DELIVER_ON_DROP}>
    -DZPP_COUNT_DROPS=$<BOOL:${ZPP_COUNT_DROPS}>
    -DZPP_VMCS_CENSUS=$<BOOL:${ZPP_VMCS_CENSUS}>
    -DZPP_EVMCS_TO_KVM=$<BOOL:${ZPP_EVMCS_TO_KVM}>
    -DZPP_EVMCS_MIXED=$<BOOL:${ZPP_EVMCS_MIXED}>
    -DZPP_STALL_BREAKER=$<BOOL:${ZPP_STALL_BREAKER}>
    -DZPP_TRACE_VTL=$<BOOL:${ZPP_TRACE_VTL}>
    -DZPP_INVEPT_ALL_PROCESSORS=$<BOOL:${ZPP_INVEPT_ALL_PROCESSORS}>
    -DZPP_WATCH_VTL_BLOCK=$<BOOL:${ZPP_WATCH_VTL_BLOCK}>
    -DZPP_TRACE_AP_ENTRY=$<BOOL:${ZPP_TRACE_AP_ENTRY}>
    -DZPP_ANNOUNCE_HYPERVISOR_BIT=$<BOOL:${ZPP_ANNOUNCE_HYPERVISOR_BIT}>
    # **Three switches added in one session never reached the compiler
    # because they were declared in the top-level CMakeLists, forwarded
    # to this sub-build's cache, and then not added to this list.** The
    # cache read `ON`, the option existed, `cmake --build` succeeded,
    # and the code compiled out - so two rig boots measured a change
    # that was not in the binary, and a diagnostic reported zero
    # because it was never built. Adding an option is three edits here,
    # not two, and the third is the only one the compiler sees.
    -DZPP_STEP_VTL=$<BOOL:${ZPP_STEP_VTL}>
    -DZPP_INIT_CLEARS_EFER=$<BOOL:${ZPP_INIT_CLEARS_EFER}>
    -DZPP_TRACK_LONG_MODE_SWITCH=$<BOOL:${ZPP_TRACK_LONG_MODE_SWITCH}>
    -DZPP_TRAP_AP_FAULTS=$<BOOL:${ZPP_TRAP_AP_FAULTS}>
    -DZPP_PROBE_APS=$<BOOL:${ZPP_PROBE_APS}>
    -DZPP_HONEST_EXIT_LENGTH=$<BOOL:${ZPP_HONEST_EXIT_LENGTH}>
    -DZPP_L2_STARTUP_SPIN=$<BOOL:${ZPP_L2_STARTUP_SPIN}>
    -DZPP_STEP_AP_WATCHED_WRITES=$<BOOL:${ZPP_STEP_AP_WATCHED_WRITES}>
    -DZPP_DROP_WATCH_ON_START_UP=$<BOOL:${ZPP_DROP_WATCH_ON_START_UP}>
    -DZPP_WATCH_AP_PAGE_TABLE=$<BOOL:${ZPP_WATCH_AP_PAGE_TABLE}>
    -DZPP_APPLY_QUEUED_START_UP=$<BOOL:${ZPP_APPLY_QUEUED_START_UP}>
    -DZPP_EVMCS=$<BOOL:${ZPP_EVMCS}>
    -DZPP_DEFER_GUEST_STATE=$<BOOL:${ZPP_DEFER_GUEST_STATE}>
    -DZPP_SHADOW_GUEST_STATE=$<BOOL:${ZPP_SHADOW_GUEST_STATE}>
    -DZPP_SLOW_EXITS=${ZPP_SLOW_EXITS}
    -DZPP_STRETCH_GUEST_TIMER=${ZPP_STRETCH_GUEST_TIMER}
    -DZPP_TIME_DILATION=${ZPP_TIME_DILATION}
    -DZPP_VTL_CAPTURE=$<BOOL:${ZPP_VTL_CAPTURE}>
    -DZPP_TICK_FLOOR=${ZPP_TICK_FLOOR}
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
