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

# The firmware's linear framebuffer: the UEFI loader asks the graphics output
# protocol where the screen is and hands the answer over, and the hypervisor
# records it. Nothing draws - it is recorded so something *outside* the
# machine can read the screen, which on the passthrough rig is the only way
# to see one at all: the display is a passed-through GPU, so QEMU answers
# `screendump` with "There is no console to take a screendump from" and the
# monitor's `xp` over the reported base is the whole capability.
#
# On in debug and hardcoded 0 in release, following ZPP_DIAG rather than the
# nested switches: it changes nothing the guest observes, it is a diagnostic
# aid, and a release binary has no reason to query a protocol it will not use.
#
# Off leaves the hand-over structure all zero, which is what the resident side
# already reads on a machine that reports no graphics output protocol - so off
# is an existing state rather than a new one. The structure itself stays in
# loader/include/zpp/loader.h unconditionally: it is embedded by value in the
# loader/hypervisor ABI blocks, and an ABI whose shape follows a build switch
# is one where a loader and a hypervisor built with different switches
# disagree silently about every field after it.
option(ZPP_FRAMEBUFFER
    "Locate the firmware framebuffer and hand it over (debug builds only)"
    ON)

# A per-processor cache of VMCS fields, valid from one VM exit to the next
# VM entry. See vmcs.h, which carries the whole argument.
#
# OFF, and off is the decision rather than a convenience. It is worth a
# great deal - 30.7 VMCS reads an exit at ~3,529 cycles each, of which the
# caller census says 37.8% are repeats of four fields - but it turns a
# read that always talked to the processor into one that may talk to
# memory instead, and a cache that answers with a stale field does not
# fault, it answers. That is the worst failure shape in this tree.
#
# It also changes the launch path: with it on, `setup_vmcs` points the
# live GS base at this processor's row, because until the first VM exit GS
# still holds whatever the loader left. A first attempt read GS without
# doing that and the launch died with loader code 0x60e00 and zero exits
# on every processor.
#
# So it is asked for, measured against a run without it, and the pair
# `vmcs_cache_hits` / `vmcs_cache_misses` says whether it did anything.
option(ZPP_VMCS_CACHE
    "Cache VMCS fields per processor between a VM exit and the next entry"
    OFF)

# Virtual-interrupt delivery, offered to the guest hypervisor. See
# nested_vmx.h, which carries the measurement this exists for.
#
# OFF. It is the best-evidenced candidate for the deadlock that stops the
# nested boot - the guest pinned at DISPATCH_LEVEL with vector 0x2f
# undeliverable by the strictly-greater rule, and the level above
# asserting VINA for an interrupt it cannot itself deliver - but it hands
# the processor a job the level above is doing today, and a run with it on
# is not comparable with one without.
option(ZPP_NESTED_VID
    "Offer APIC-register virtualization and virtual-interrupt delivery"
    OFF)

# Clear the virtual interrupt notification flag on the entry that runs
# VTL1, so the secure kernel does its work instead of yielding. See
# nested_vmx.h, which carries the measurement and the argument.
#
# OFF, and it is a lie to the guest rather than a fix to a defect: nothing
# in this VMM is misbehaving, the rate is simply wrong. It exists because
# the rate cannot be fixed on this host.
option(ZPP_SUPPRESS_VINA
    "Clear the interrupt notification flag when entering VTL1"
    OFF)

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

option(ZPP_VIRTUALIZE_APIC
    "Take every external interrupt in the host and inject it into the guest, the way KVM does, instead of letting the guest's own interrupt controller deliver them natively. Off is this VMM's design; on exists to test whether that difference is what stalls the nested guest."
    OFF)

set(ZPP_LAZY_TICK "0" CACHE STRING
    "Withhold a clock interrupt arriving sooner than this many microseconds after the last delivered one. 0 is off. Aimed at the measured preemption where the guest is interrupted two instructions short of draining its deferred-call queue.")

option(ZPP_DISARM_APIC_WATCH
    "Drop the local APIC page watch once no more processors are going to start. The watch exists to catch a start-up IPI; in steady state it catches end-of-interrupt traffic instead, measured at 23.2% of all exits and every one of them self-inflicted. Off by default: a processor started after the watch is dropped would run unvirtualized."
    OFF)

option(ZPP_INTERCEPT_APIC
    "Watch the guest's local APIC - the page in xAPIC mode, the interrupt command MSR in x2APIC mode. Off only makes sense with one processor, since the watch is what stops a start-up IPI handing a processor over unvirtualized."
    ON)

option(ZPP_DELIVER_SELF_IPI
    "Deliver a second-level guest's self-directed interrupt when the guest hypervisor does not. Measured: the guest asks for vector 0x2f on every clock cycle and the level above delivers it six times, while arming a TPR-below-threshold notification that fires 33 times in 3.6 million exits. Delivered only when the guest's own virtual task priority permits, and only when the level above staged nothing itself. OFF by default."
    OFF)

option(ZPP_INTERCEPT_SELF_IPI
    "Withhold from the guest hypervisor a self-directed synthetic interrupt the requesting processor cannot currently take, and deliver it here at the first entry its own task priority admits it. Gates both halves together, which is the whole point: ZPP_DELIVER_SELF_IPI alone still reflected the write, so the level above kept the vector pending and kept asserting its trust-level notification regardless. Measured on the rig over one 257 second window: vector 0x2f requested 145,300 times and delivered ZERO, the virtual task priority never below 0x20 across 630,418 entries so every refusal was correct, and the notification asserted on 1.0032 of every HvCallVtlCall - which is what stops VTL1 retiring its secure call. Only self-directed commands, and only while undeliverable. OFF by default: the level above may keep bookkeeping that depends on seeing the write. See BACKLOG.md."
    OFF)

option(ZPP_FORCE_DISPATCH_ONCE
    "DIAGNOSTIC, ONE BOOT ONLY. Deliver the deferred-call vector 0x2f ONCE, ever, in deliberate violation of the architectural masking rule (SDM 12.8.4: strictly greater priority class), and only when the sampled virtual task priority is exactly 0x20. The stall is a three-link cycle and two links are closed by measurement: withholding the request halts the level above, and round-trip speed cannot reach a level-triggered notification. This tests the third - one delivery drains the deferred call, the vector stops being pending, the notification deasserts and the priority drops. Armed only after reading DpcRoutineActive=0, queue depth 1, and 0x2f>>4=2 out of the guest. The reflection is NOT withheld. OFF by default and it stays off whatever the outcome - it is an experiment, not a fix. See BACKLOG.md."
    OFF)

option(ZPP_EAGER_EPT_NEIGHBOURS
    "On an extended-page-table fault, also install the pages around the one that faulted, bounded to an aligned window. Measured on a verified build: ept-violation is 16.31 per trust-level round trip and 61.1% of all exits at 18.0 VMCS accesses each - 30% of all accesses - and one leaf is installed per fault. Sound because only mappings the guest already permits are installed, composed identically, and any change obliges an INVEPT which discards the root. Sized before building: taking faults to 4 is about 302 us of 3,450, under 9%, against the ~50% needed - it cannot reach the goal and is not expected to. OFF by default. See BACKLOG.md."
    OFF)

option(ZPP_PUBLISH_REFERENCE_TSC
    "Fill in the reference TSC page the guest hypervisor enables for its guest and then leaves invalid, The scale is COMPUTED - reference time is 100-nanosecond units, so the counter is 10 MHz by specification and scale = 10^7 * 2^64 / tsc_hz - wherever CPUID.15H enumerates the counter's frequency (SDM 22.7.3). QEMU zeroes that leaf, so on the rig the scale is still fitted to the answers the level above gave for the counter MSR, but over a baseline of at least 2^31 counts rather than the 5 ms that 32 reads span. Only the offset ever comes from the fit. Measured: without it the guest reads that MSR about fifteen times per clock tick, 26.3% of every exit on the machine, and the guest sits at 0xf0 on 39.3% of entries against 0.2% with the page. OFF by default, and the reason is a bug rather than a design objection: the target frame is resolved by walking the guest hypervisor's OWN extended page tables (l2_physical_to_l1 over eptp12) and then written by a separate call, with NOTHING pinning or re-validating the mapping in between - so a level that remaps guest frames as ordinary business, and is never asked, can reassign that frame between the check and the write. That is time-of-check-to-time-of-use writing to an address somebody else now owns: arbitrary memory corruption, not a boundary violation. Refusing to overwrite a non-zero sequence would be worth doing and does NOT fix this - the race is on the address, not the contents. See BACKLOG.md."
    OFF)

option(ZPP_NESTED_SHADOW_VMCS
    "Let the processor shadow a guest hypervisor's VMCS, so its vmreads and vmwrites do not exit. ON, with the field list cut to the measured hot set - measured at the wall, the handler holds 90.1% of the wall clock with the old 36 field list and 89.2% with the short one, so the guest gets a tenth more of the machine. Do NOT compare settings by cost per exit; the flag changes the exit count several fold and only handler share of wall clock survives that. Turn it OFF to read vmcs_field_use, which is blind to every field it shadows and is the only way to re-measure the hot set."
    ON)

set(ZPP_SLOW_EXITS "0" CACHE STRING
    "Burn this many extra cycles in every VM exit. A deliberate slowdown, to find out whether the nested stall moves with throughput or sits at a fixed point - making a machine faster cannot distinguish a throughput limit from a wall, making it slower can.")

set(ZPP_TICK_FLOOR "0" CACHE STRING
    "Least periodic tick period, in 100 ns units, this VMM will let\n     the second-level guest arm; 0 leaves every request untouched.\n     156250 is 15.625 ms, the 64 Hz tick Windows itself uses before\n     it re-arms at 1.74 ms. Only periods are floored, never one-shot\n     deadlines.")

set(ZPP_TIME_DILATION "1" CACHE STRING
    "Charge the second-level guest, and the hypervisor above it, only\n     1/N of the time this VMM spends in root operation, by moving the\n     time-stamp counter offset. 1 is off. Windows arms a periodic 1.74\n     ms tick - KeQuantumEndTimerIncrement, a literal in its own kernel -\n     and a tick costs this VMM about 2.46 ms, so the handler cannot\n     finish inside its own period. Unlike ZPP_STRETCH_GUEST_TIMER this\n     moves *every* clock in the virtual machine together, since all of\n     them derive from the time-stamp counter, so no two of the guest's\n     time sources can disagree. Only the interval between an exit and\n     the entry after it is scaled - the guest's own execution never is -\n     so the counter stays monotonic. See nested_vmx.h.")

set(ZPP_STRETCH_GUEST_TIMER "1" CACHE STRING
    "Multiply the second-level guest's synthetic timer *period* by this before its hypervisor sees it. 1 is off. A diagnostic, and the only one that can cross the threshold rather than retreat from it: the guest arms 1.74 ms and a tick costs 1.97 ms, so 'too slow to finish a tick inside a tick' and 'a handler that would never finish' look identical from outside. Eight gives the handler eight times the budget without needing the 2.3x of speed. Absolute one-shot deadlines are left alone - only values under one second are treated as periods. A guest run this way is being lied to about time, so never leave it on.")

option(ZPP_SHADOW_GUEST_STATE
    "Run the deferred guest-state copy in shadow: compute what it would produce, keep using the eager value, and record where they differ. Behaviour-neutral by construction - it reads and records and takes no decision - so it names the field and the moment instead of resetting the guest. A diagnostic, never a candidate build."
    OFF)

option(ZPP_EVMCS
    "Offer Hyper-V's enlightened VMCS, so the guest hypervisor states its VMCS in a shared structure instead of executing VMREAD and VMWRITE - 54% of a trust-level round trip measured on the rig. Off by default: the advertisement and the handling cannot be staged separately, so the first boot with this on exercises the whole mechanism at once."
    OFF)

option(ZPP_DEFER_GUEST_STATE
    "Defer the bulk copy of the second-level guest's state out of vmcs02 into vmcs12 until something reads it. Measured on the rig at 1.136x: save_l2_state 198,309 -> 52,792 cycles/call, wall clock per exit 462,843 -> 407,257. Four ordering conditions are found, fixed, and covered by tests/nested_exit's sequence cases; the fourth was flush_guest_vmcs12 copying the whole vmcs12 back on every VMPTRLD."
    ON)

option(ZPP_VTL_CAPTURE
    "Take the deep per-switch trust-level capture - the stack, the code window, the VP assist page, the shared and spin regions. OFF: it is the last candidate for 137.6 VMCS reads a vmcall, which is 19% of the machine. The cheap register census beside it stays either way. See nested_vmx.h."
    OFF)

option(ZPP_INIT_CLEARS_EFER
       "Clear IA32_EFER when applying start-up state, as SDM Table 12-1 requires after INIT"
       OFF)

option(ZPP_TRACK_LONG_MODE_SWITCH
       "Maintain the 'IA-32e mode guest' VM-entry control and the guest IA32_EFER.LMA field across a guest's own paging transitions, and own CR0.PG in the guest/host mask so those transitions cannot be silent. Nothing in this tree maintained either: setup_vmcs sets the control once for a boot processor already in long mode and apply_start_up clears it, so an application processor's stub that goes real -> protected -> long reaches VM entry with CR0.PG set, EFER.LME set and the control clear, which SDM 29.3.1.1 rejects as invalid guest state (reason 0x80000021). Mirrors KVM's vmx_set_cr0 / enter_lmode / vmx_set_efer."
       ON)

option(ZPP_TRAP_AP_FAULTS
       "Diagnostic. Intercept an application processor's first exception in the window opened by the write that enables paging, so the fault that kills it is recorded with its vector, error code and - for a page fault - its faulting linear address, instead of arriving as a bare triple fault. The capture disarms itself and resumes without advancing RIP or injecting anything, so the guest re-executes and the boot ends as it did before. Read hypervisor::ap_fault as a pair: armed 1 with occurred 0 means the trap was in place and caught nothing, which says the triple fault is not a guest exception at that instruction at all."
       OFF)

option(ZPP_HONEST_EXIT_LENGTH
       "Report a VM-exit instruction length to the guest hypervisor only for exit reasons where the SDM defines it"
       ON)

option(ZPP_L2_STARTUP_SPIN
       "Spin in root operation while a second-level processor waits for its start-up IPI (off: return immediately)"
       ON)

option(ZPP_STEP_AP_WATCHED_WRITES
       "Let processors other than the first execute their own watched-page writes by monitor trap step instead of emulating them"
       OFF)

option(ZPP_DROP_WATCH_ON_START_UP
       "Drop the local APIC page watch as soon as a start-up has been applied, rather than after a quiet period"
       OFF)

option(ZPP_WATCH_AP_PAGE_TABLE
       "Watch the page table entry mapping an application processor's descriptor table, and name whoever writes it"
       OFF)

option(ZPP_APPLY_QUEUED_START_UP
    "Apply a start-up IPI that arrived before its target reached its INIT exit, rather than only holding it. On is how the multicore boot got past its first wall; it also starts a processor twice, which hardware would not. Off is the experiment. See nested_vmx.h."
    ON)

option(ZPP_STEP_VTL
    "Step the trust-level loop with the monitor trap flag, recording every instruction each side executes with its bytes. The only instrument that can see inside a loop which takes no exits - and an exit per retired instruction, measured at 5.8% of all exits on the rig, which is enough to move the guest between regimes. Never leave it on for a boot being compared against another."
    OFF)

option(ZPP_PROFILE_L2
    "Sample a second-level guest's instruction pointer on the VMX-preemption timer. A profiler: it costs an exit per interval and is the only instrument that can see a guest spinning on memory."
    OFF)

option(ZPP_SAMPLE_L1
    "Sample the first-level guest's instruction pointer on the VMX-preemption timer, armed at VMCS setup. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_WINDOW_ON_TPR
    "Arm the guest hypervisor's interrupt window on a task-priority drop instead of on every interruptible moment. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_DELIVER_ON_DROP
    "Arm a TPR threshold where the guest hypervisor left none, and take nothing away - the interrupt window is never withheld. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_COUNT_DROPS
    "Account for the low-priority vector the guest asks for, from the ask to the delivery, and name the requests dropped without one. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_VMCS_CENSUS
    "Census every VMCS access by field and by caller. See cmake/hypervisor/CMakeLists.txt."
    ON)

option(ZPP_EVMCS_TO_KVM
    "Use Hyper-V's enlightened VMCS toward the layer below. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_EVMCS_MIXED
    "Enlighten only the second-level VMCS and keep offering VMCS shadowing. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_STALL_BREAKER
    "Refuse to deliver the same vector twice at the same guest instruction. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_INVEPT_ALL_PROCESSORS
    "Propagate a guest INVEPT to every processor, not only the one that executed it. The guest hypervisor revokes a page from its own guest with HvCallModifyVtlProtectionMask and then invalidates; our shadow of that table is per processor and only the executing one discards, so another processor can keep serving the permission that was just revoked. Only possible above one processor."
    OFF)

option(ZPP_TRACE_VTL
    "Record Hyper-V virtual trust level switches. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_WATCH_VTL_BLOCK
    "Watch the IUM context block for writes. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_TRACE_AP_ENTRY
    "Log the whole guest state at an application processor's first VM entry, at every application of the start-up state, and at any triple fault. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_ANNOUNCE_HYPERVISOR_BIT
    "Set CPUID leaf 1 ECX bit 31 without the hypervisor block. See cmake/hypervisor/CMakeLists.txt."
    OFF)

option(ZPP_CENSUS_EXITS
    "Fill the exit ring's qualification, activity state and CS selector, and the CPL census beside them. Three VMCS reads on every exit, which is 7.8 per round trip at the measured 2.61 exits per round trip, and every one of them is an exit to KVM on a host without VMCS shadowing. OFF: the ring still records the reason, the instruction pointer and the guest-physical address, and the terminal records keep their full detail. See nested_vmx.h."
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
        -DZPP_FRAMEBUFFER=${ZPP_FRAMEBUFFER}
        -DZPP_VMCS_CACHE=$<BOOL:${ZPP_VMCS_CACHE}>
        -DZPP_NESTED_VID=$<BOOL:${ZPP_NESTED_VID}>
        -DZPP_SUPPRESS_VINA=$<BOOL:${ZPP_SUPPRESS_VINA}>
        -DZPP_NESTED_VMX=${ZPP_NESTED_VMX}
        -DZPP_REQUEUE_INTERRUPTED_EVENTS=${ZPP_REQUEUE_INTERRUPTED_EVENTS}
        -DZPP_NESTED_TPR_SHADOW=${ZPP_NESTED_TPR_SHADOW}
        -DZPP_SLOW_EXITS=${ZPP_SLOW_EXITS}
        -DZPP_STRETCH_GUEST_TIMER=${ZPP_STRETCH_GUEST_TIMER}
        -DZPP_TIME_DILATION=${ZPP_TIME_DILATION}
        -DZPP_VTL_CAPTURE=$<BOOL:${ZPP_VTL_CAPTURE}>
        -DZPP_TICK_FLOOR=${ZPP_TICK_FLOOR}
        -DZPP_NESTED_SHADOW_VMCS=${ZPP_NESTED_SHADOW_VMCS}
        -DZPP_PUBLISH_REFERENCE_TSC=${ZPP_PUBLISH_REFERENCE_TSC}
        -DZPP_DELIVER_SELF_IPI=${ZPP_DELIVER_SELF_IPI}
        -DZPP_INTERCEPT_SELF_IPI=${ZPP_INTERCEPT_SELF_IPI}
        -DZPP_FORCE_DISPATCH_ONCE=${ZPP_FORCE_DISPATCH_ONCE}
        -DZPP_EAGER_EPT_NEIGHBOURS=${ZPP_EAGER_EPT_NEIGHBOURS}
        -DZPP_INTERCEPT_APIC=${ZPP_INTERCEPT_APIC}
        -DZPP_DISARM_APIC_WATCH=${ZPP_DISARM_APIC_WATCH}
        -DZPP_LAZY_TICK=${ZPP_LAZY_TICK}
        -DZPP_VIRTUALIZE_APIC=${ZPP_VIRTUALIZE_APIC}
        -DZPP_PROFILE_L2=${ZPP_PROFILE_L2}
        -DZPP_CENSUS_EXITS=${ZPP_CENSUS_EXITS}
        -DZPP_SAMPLE_L1=${ZPP_SAMPLE_L1}
        -DZPP_WINDOW_ON_TPR=${ZPP_WINDOW_ON_TPR}
        -DZPP_DELIVER_ON_DROP=${ZPP_DELIVER_ON_DROP}
        -DZPP_COUNT_DROPS=${ZPP_COUNT_DROPS}
        -DZPP_VMCS_CENSUS=${ZPP_VMCS_CENSUS}
        -DZPP_EVMCS_TO_KVM=${ZPP_EVMCS_TO_KVM}
        -DZPP_EVMCS_MIXED=${ZPP_EVMCS_MIXED}
        -DZPP_STALL_BREAKER=${ZPP_STALL_BREAKER}
        -DZPP_TRACE_VTL=${ZPP_TRACE_VTL}
        -DZPP_INVEPT_ALL_PROCESSORS=${ZPP_INVEPT_ALL_PROCESSORS}
        -DZPP_WATCH_VTL_BLOCK=${ZPP_WATCH_VTL_BLOCK}
        -DZPP_TRACE_AP_ENTRY=${ZPP_TRACE_AP_ENTRY}
        -DZPP_ANNOUNCE_HYPERVISOR_BIT=${ZPP_ANNOUNCE_HYPERVISOR_BIT}
        -DZPP_STEP_VTL=${ZPP_STEP_VTL}
        -DZPP_INIT_CLEARS_EFER=${ZPP_INIT_CLEARS_EFER}
        -DZPP_TRACK_LONG_MODE_SWITCH=${ZPP_TRACK_LONG_MODE_SWITCH}
        -DZPP_TRAP_AP_FAULTS=${ZPP_TRAP_AP_FAULTS}
        -DZPP_HONEST_EXIT_LENGTH=${ZPP_HONEST_EXIT_LENGTH}
        -DZPP_L2_STARTUP_SPIN=${ZPP_L2_STARTUP_SPIN}
        -DZPP_STEP_AP_WATCHED_WRITES=${ZPP_STEP_AP_WATCHED_WRITES}
        -DZPP_DROP_WATCH_ON_START_UP=${ZPP_DROP_WATCH_ON_START_UP}
        -DZPP_WATCH_AP_PAGE_TABLE=${ZPP_WATCH_AP_PAGE_TABLE}
        -DZPP_APPLY_QUEUED_START_UP=${ZPP_APPLY_QUEUED_START_UP}
        -DZPP_EVMCS=${ZPP_EVMCS}
        -DZPP_DEFER_GUEST_STATE=${ZPP_DEFER_GUEST_STATE}
        -DZPP_SHADOW_GUEST_STATE=${ZPP_SHADOW_GUEST_STATE}
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
        -DZPP_SLOW_EXITS=${ZPP_SLOW_EXITS}
        -DZPP_STRETCH_GUEST_TIMER=${ZPP_STRETCH_GUEST_TIMER}
        -DZPP_TICK_FLOOR=${ZPP_TICK_FLOOR}
        -DZPP_NESTED_SHADOW_VMCS=${ZPP_NESTED_SHADOW_VMCS}
        -DZPP_PUBLISH_REFERENCE_TSC=${ZPP_PUBLISH_REFERENCE_TSC}
        -DZPP_DELIVER_SELF_IPI=${ZPP_DELIVER_SELF_IPI}
        -DZPP_INTERCEPT_SELF_IPI=${ZPP_INTERCEPT_SELF_IPI}
        -DZPP_FORCE_DISPATCH_ONCE=${ZPP_FORCE_DISPATCH_ONCE}
        -DZPP_EAGER_EPT_NEIGHBOURS=${ZPP_EAGER_EPT_NEIGHBOURS}
        -DZPP_INTERCEPT_APIC=${ZPP_INTERCEPT_APIC}
        -DZPP_DISARM_APIC_WATCH=${ZPP_DISARM_APIC_WATCH}
        -DZPP_LAZY_TICK=${ZPP_LAZY_TICK}
        -DZPP_VIRTUALIZE_APIC=${ZPP_VIRTUALIZE_APIC}
        -DZPP_PROFILE_L2=${ZPP_PROFILE_L2}
        -DZPP_CENSUS_EXITS=${ZPP_CENSUS_EXITS}
        -DZPP_SAMPLE_L1=${ZPP_SAMPLE_L1}
        -DZPP_WINDOW_ON_TPR=${ZPP_WINDOW_ON_TPR}
        -DZPP_DELIVER_ON_DROP=${ZPP_DELIVER_ON_DROP}
        -DZPP_COUNT_DROPS=${ZPP_COUNT_DROPS}
        -DZPP_VMCS_CENSUS=${ZPP_VMCS_CENSUS}
        -DZPP_EVMCS_TO_KVM=${ZPP_EVMCS_TO_KVM}
        -DZPP_EVMCS_MIXED=${ZPP_EVMCS_MIXED}
        -DZPP_STALL_BREAKER=${ZPP_STALL_BREAKER}
        -DZPP_TRACE_VTL=${ZPP_TRACE_VTL}
        -DZPP_INVEPT_ALL_PROCESSORS=${ZPP_INVEPT_ALL_PROCESSORS}
        -DZPP_WATCH_VTL_BLOCK=${ZPP_WATCH_VTL_BLOCK}
        -DZPP_TRACE_AP_ENTRY=${ZPP_TRACE_AP_ENTRY}
        -DZPP_ANNOUNCE_HYPERVISOR_BIT=${ZPP_ANNOUNCE_HYPERVISOR_BIT}
        -DZPP_STEP_VTL=${ZPP_STEP_VTL}
        -DZPP_INIT_CLEARS_EFER=${ZPP_INIT_CLEARS_EFER}
        -DZPP_TRACK_LONG_MODE_SWITCH=${ZPP_TRACK_LONG_MODE_SWITCH}
        -DZPP_TRAP_AP_FAULTS=${ZPP_TRAP_AP_FAULTS}
        -DZPP_HONEST_EXIT_LENGTH=${ZPP_HONEST_EXIT_LENGTH}
        -DZPP_L2_STARTUP_SPIN=${ZPP_L2_STARTUP_SPIN}
        -DZPP_STEP_AP_WATCHED_WRITES=${ZPP_STEP_AP_WATCHED_WRITES}
        -DZPP_DROP_WATCH_ON_START_UP=${ZPP_DROP_WATCH_ON_START_UP}
        -DZPP_WATCH_AP_PAGE_TABLE=${ZPP_WATCH_AP_PAGE_TABLE}
        -DZPP_APPLY_QUEUED_START_UP=${ZPP_APPLY_QUEUED_START_UP}
        -DZPP_EVMCS=${ZPP_EVMCS}
        -DZPP_DEFER_GUEST_STATE=${ZPP_DEFER_GUEST_STATE}
        -DZPP_SHADOW_GUEST_STATE=${ZPP_SHADOW_GUEST_STATE}
        -DZPP_TRACE=${ZPP_TRACE}
        -DZPP_CHAINLOAD_ONLY=${ZPP_CHAINLOAD_ONLY}
        -DZPP_SEARCH_ALL_DEVICES=${ZPP_SEARCH_ALL_DEVICES}
        -DZPP_DIAG=${ZPP_DIAG}
        -DZPP_DIAG_RESERVE_WINDOW=${ZPP_DIAG_RESERVE_WINDOW}
        -DZPP_FRAMEBUFFER=${ZPP_FRAMEBUFFER}
        -DZPP_VMCS_CACHE=$<BOOL:${ZPP_VMCS_CACHE}>
        -DZPP_NESTED_VID=$<BOOL:${ZPP_NESTED_VID}>
        -DZPP_SUPPRESS_VINA=$<BOOL:${ZPP_SUPPRESS_VINA}>
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
