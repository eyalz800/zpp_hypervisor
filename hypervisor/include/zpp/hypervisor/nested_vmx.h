#pragma once
#include "zpp/arch/x86_64/vmx/vmx.h"
#include <cstdint>

namespace zpp::hypervisor::nested_vmx
{
/**
 * Whether the guest is told VT-x exists.
 *
 * Off by default, and the default is the whole of the decision. With it
 * off, CPUID leaf 1 ECX[5] reports no VMX, CR4.VMXE reads back clear
 * through its shadow, and the capability MSRs are not intercepted - so a
 * guest hypervisor stands down before it executes anything, and every VMX
 * instruction it might still attempt takes the #UD the exit handler gives
 * it. That is a consistent machine: no VMX in CPUID, no VMXE in CR4, and
 * #UD on the instructions. It is the state this VMM boots Windows in
 * today, VBS included, because Hyper-V launches ahead of Windows whenever
 * VBS is on and hands straight off when it finds no VMX.
 *
 * With it on, the guest is told VMX exists and the machinery below
 * answers for it: VMXON through VMPTRST keep a shadow VMCS per processor,
 * the capability MSRs report a subset of the hardware's,
 * IA32_FEATURE_CONTROL is answered rather than passed through, and
 * VMLAUNCH builds a second real VMCS out of the shadow and runs the
 * second-level guest with it - with a shadow of the first level's
 * extended page tables composed against this VMM's own, and every exit
 * the second-level guest takes either answered here or reflected into the
 * shadow's exit-information fields.
 *
 * **Why it is still off:** none of it has executed. Not on hardware, not
 * under an emulator, not once - it is checked against the compiler in
 * four configurations and against the SDM and KVM by reading, and that is
 * all. BACKLOG.md's "Not run anywhere" section lists the experiments that
 * would change that, in the order of how much each proves. Until one of
 * them has been run, "the guest is told VMX exists" is a promise this VMM
 * has no evidence it keeps, and a broken promise about VMX is worse for a
 * guest than a clean absence: Hyper-V stands down gracefully when it
 * finds no VMX and does not when it finds a broken one.
 *
 * BACKLOG.md's coverage checklist has no `no` rows left, which measures
 * coverage against the SDM and KVM and measures nothing else. The one
 * restriction worth knowing without reading it: a VM-entry or VM-exit MSR
 * area may only name MSRs this VMM will read and write, because it has no
 * WRMSR that can fault and recover, and an area naming anything else
 * refuses the entry.
 *
 * Turning it on: -DZPP_NESTED_VMX=ON.
 */
/**
 * Whether to let the guest see the hypervisor interface of whatever this
 * VMM is itself running under, instead of answering the whole hypervisor
 * CPUID range ourselves.
 *
 * **This is what stops a guest hypervisor starting, and it is off because
 * turning it on is only half a change.**
 *
 * Measured: Hyper-V arms on this rig without this VMM in the way and does
 * not arm with it, while every *hardware* prerequisite it reports is
 * present. The difference is that the outer hypervisor exposes a full set
 * of Hyper-V enlightenments and we hide them - so the guest sees an
 * unrecognised hypervisor advertising nothing about nested virtualization,
 * and declines.
 *
 * The other half is the synthetic MSRs. They live at 0x40000000 and above,
 * outside both ranges the MSR bitmap covers, so every access exits
 * unconditionally and is answered with a general protection fault. That is
 * the honest answer for a VMM claiming no interface and a fatal one for a
 * VMM claiming one - it is the 0xc000000d this tree already paid for. So
 * this switch forwards those accesses to the hypervisor underneath, which
 * does implement them, and the two halves are deliberately the same
 * switch: neither is safe alone.
 *
 * Off by default because it hands the guest an interface this VMM does not
 * implement and cannot implement alone - it works only while something
 * underneath does. On bare metal there is nothing underneath, so this must
 * stay off there and the interface would have to be implemented instead.
 */
inline constexpr bool pass_through_hypervisor_interface = false;

/**
 * Whether to announce this VMM to the guest and answer the hypervisor
 * CPUID range ourselves, claiming no features at all.
 *
 * The bare-metal-honest alternative to the switch above, which forwards to
 * whatever is underneath and therefore cannot ever ship: on real hardware
 * there is nothing underneath to forward to.
 *
 * Three things were measured to arrive at this shape:
 *
 * - With no hypervisor announced, the guest asked for **zero** leaves in
 *   the hypervisor range out of 49,860 CPUID leaves in a boot. It does not
 *   go looking unless told.
 * - Announcing one and forwarding the range made it look (225 leaves) and
 *   use the enlightenments it found (9 synthetic MSR accesses) - and its
 *   application-processor adoption fell from all eight to none, because a
 *   guest that believes it is under a Hyper-V-compatible hypervisor starts
 *   processors through an enlightened hypercall rather than through the
 *   interrupt command register this VMM watches.
 * - The reference implementation compared against claims only five feature
 *   bits - reference counter, hypercall MSRs, VP index, reference TSC,
 *   frequency MSRs - and nothing whatever about starting processors. Its
 *   guest therefore keeps using INIT-SIPI-SIPI.
 *
 * So this claims *no* features. That is the smallest thing that makes the
 * guest aware it is virtualized, and awareness is what the interesting
 * question turns on: a guest that thinks it is on bare metal applies bare
 * metal requirements to virtualization-based security - Secure Boot among
 * them, which this rig cannot provide - while one that knows better does
 * not. Claiming nothing also means there is nothing to implement and no
 * synthetic MSR the guest has been invited to ask for, so the general
 * protection fault those still take remains the honest answer.
 *
 * Deliberately not "Microsoft Hv" in the vendor leaf. The interface
 * signature in the second leaf is what a guest matches on; the vendor
 * stays ours, which is what the reference does too.
 *
 * **On, and it took implementing the interface's MSRs to get there.**
 * Announcing used to stop the machine reaching an operating system at
 * all, and the sequence was measured rather than reasoned about: the
 * boot processor's last four exits were CPUID, CPUID, an RDMSR at guest
 * RIP 0x1e086fd, and a triple fault at that same RIP, with exactly one
 * MSR index ever faulted - 0x40000001, the hypercall MSR. Announcing an
 * interface publishes its signature, the guest reads that MSR whether or
 * not any feature bit invites it to, and a general protection fault
 * there is fatal.
 *
 * So the MSRs are answered now - identity, the hypercall page and the
 * processor index - and with them the same build reaches the Windows
 * kernel with two processors running and no MSR faulted at all, where it
 * had been sitting in this VMM's own halt loop after an unhandled triple
 * fault. The guest also starts asking: 688 hypervisor leaves in a boot,
 * against zero when nothing is announced.
 *
 * Two lessons are worth more than the fix. The switch was flipped in the
 * commit that added an instruction decoder, so the first failing boot was
 * blamed on the decoder and several boots went into bisecting a change
 * that was innocent - **a behavioural switch does not belong in a commit
 * that adds a mechanism.** And the state sampled from the emulator's
 * monitor showed CS 8 with RFLAGS 2, which is this VMM's own host
 * selector and the flags a VM exit loads, not the guest at all; reading
 * it as guest state cost a wrong conclusion.
 *
 * What is still not backed: no feature bit is claimed, so no hypercall,
 * reference counter, reference TSC or frequency MSR is offered, and
 * those still fault - which is the honest answer while nothing here
 * implements them. A guest hypervisor has not armed yet
 * (`guest_vmxon_count` zero), so the next question is what else
 * virtualization based security wants before it will.
 */
// **Tried on 2026-08-15 against the first baseline, and withdrawn.**
//
// The reasoning was sound and the measurement refuted it. The control
// run boots the same Windows, guest hypervisor and virtual secure mode
// to ring 3 under KVM alone, and KVM announces itself unconditionally
// while this VMM did not - so the one configuration that works told the
// guest hypervisor it was nested and the one that stalls told it it was
// on bare metal.
//
// Turned on, the announcement was taken **cleanly**: the guest
// hypervisor wrote its identity to HV_X64_MSR_GUEST_OS_ID -
// 0x1040a0000271b - made four synthetic accesses and faulted none,
// which is the failure that killed the guest the last time a hypervisor
// was announced without its MSRs answered. And the boot was unchanged:
// same livelock, ring 3 still zero.
//
// **Withdrawn because it is not the same claim the baseline makes, and
// the difference is the dangerous direction.** KVM presents its *own*
// signature and the rig passes no `hv-` flags at all - checked in the
// launcher, see CLAUDE.md - so under KVM the guest hypervisor sees a
// hypervisor that is not Microsoft's. This VMM announces `Hv#1`, the
// Microsoft interface, and then answers leaf 0x40000003 - the privilege
// mask - as **zero**, deliberately, because nothing is implemented. So
// a guest hypervisor is told Microsoft's interface is present and that
// it is entitled to none of it. That is this project's recurring
// mistake in its purest form: announcing an interface and answering
// part of it.
//
// What would make it right is one of two things, and both are real
// work rather than a switch: present a signature that is not `Hv#1`, or
// populate 0x40000003 with privileges this VMM can actually back.
// **Enlightened VMCS makes this the right thing to do rather than the
// wrong one**, which is why it is the switch below rather than a
// hard-coded false.
//
// The objection above is to announcing an interface and backing none of
// it. Enlightened VMCS is an interface this VMM *can* back: the guest
// hypervisor writes its VMCS into a shared structure instead of
// executing VMREAD and VMWRITE, and both directions of that copy are
// implemented in `nested_evmcs.cpp` and driven by `tests/nested_exit`
// against a hand-built structure.
//
// It is also the only candidate large enough to matter. Measured on the
// rig, the guest hypervisor's own VMX instructions are **54% of a
// trust-level round trip** - and a round trip is 6.64 ms against a 1.74
// ms tick, which is why Windows with virtualization-based security
// livelocks here and boots on KVM alone in five minutes. KVM implements
// enlightened VMCS; this VMM did not.
//
// Off by default because the advertisement and the handling cannot be
// staged separately - the guest hypervisor registers no assist page
// until it sees the advertisement - so the first boot with this on
// exercises the whole mechanism at once.
inline constexpr bool evmcs_offered =
#if defined(ZPP_EVMCS) && ZPP_EVMCS
    true;
#else
    false;
#endif

inline constexpr bool announce_hypervisor = evmcs_offered;

inline constexpr bool enabled =
#if defined(ZPP_NESTED_VMX) && ZPP_NESTED_VMX
    true;
#else
    false;
#endif

/**
 * Whether this VMM fills in the reference TSC page the guest hypervisor
 * enables for its guest and then leaves invalid.
 *
 * The measured chain this exists to break: Windows enables the page,
 * `0x40000021` written with the enable bit; the guest hypervisor never
 * writes it, so its sequence stays zero, which the interface defines as
 * "invalid, ask the counter MSR"; Windows therefore reads `0x40000020`
 * about fifteen times per clock tick; each costs a reflection and the
 * resume after it, about 42% of every exit on the machine; and
 * `Phase1Initialization` spins on `KeQueryPerformanceCounter` and never
 * leaves phase one of the boot.
 *
 * With a *valid* page the same query is arithmetic on RDTSC and takes no
 * exit at all: `reference = ((tsc * scale) >> 64) + offset`.
 *
 * **Nothing is invented.** The scale and offset are fitted to the guest
 * hypervisor's own answers - `reference_read_value` and
 * `reference_read_tsc` already record what came back from every reflected
 * read and the counter when it did - so what is published reproduces the
 * MSR rather than competing with it. A wrong fit would step the guest's
 * clock, so it is checked against a sample it was not derived from before
 * the sequence is made non-zero.
 *
 * Off by default: it writes into guest memory the level above believes it
 * owns.
 */
/**
 * Whether this VMM delivers a second-level guest's self-directed
 * interrupt when the guest hypervisor does not.
 *
 * Measured: the guest writes the synthetic interrupt command register
 * with `0x4002f` - a self-IPI of vector `0x2f`, the deferred-procedure
 * call - on every clock cycle, tens of thousands of times, and
 * `l2_injected_vector` shows the guest hypervisor delivering it **six**
 * times. It is not the guest masking it: `l2_entry_vtpr` has the guest at
 * task priority zero or `0x10` on about a tenth of entries, where vector
 * `0x2f` outranks it comfortably.
 *
 * What the guest hypervisor appears to be waiting for is a
 * TPR-below-threshold exit - it writes `tpr_threshold` on 11% of its
 * VMWRITEs - and that exit fires 33 times in 3.6 million. So it arms a
 * notification it never receives and holds an interrupt it never
 * delivers.
 *
 * With this on, the vector is delivered on the next entry where the
 * guest's own virtual task priority permits it, by the rule its local
 * APIC would use: priority class strictly greater, SDM 12.8.4. Only when
 * the guest hypervisor has staged nothing itself, so its own injections
 * always win.
 *
 * Off by default. It puts an interrupt into a guest that the level owning
 * that guest did not ask for, which is defensible only because the guest
 * itself did ask and can be shown not to be masking it.
 */
inline constexpr bool deliver_self_ipi =
#if defined(ZPP_DELIVER_SELF_IPI) && ZPP_DELIVER_SELF_IPI
    true;
#else
    false;
#endif

inline constexpr bool publish_reference_tsc =
#if defined(ZPP_PUBLISH_REFERENCE_TSC) && ZPP_PUBLISH_REFERENCE_TSC
    true;
#else
    false;
#endif

/**
 * Whether the guest hypervisor's own VMREADs and VMWRITEs are served from
 * a shadow VMCS region instead of exiting.
 *
 * On, and the measurement it is on for: VMREAD and VMWRITE were 65.7% of
 * every exit this VMM took while Hyper-V booted Windows on the rig -
 * 2,070,621 and 765,747 out of 4,287,565 - and shadowing took them to six
 * and 411, with the guest hypervisor's own guest running 2.5 times
 * faster.
 *
 * It was left `false` after a one-variable bare-metal boot and stayed
 * that way, with this comment still saying "On" - which is the drift the
 * switch's own design was meant to prevent, arriving by a route it did
 * not anticipate. The value is the thing that runs; a comment that
 * disagrees with it is worse than no comment, because it is read as the
 * configuration.
 *
 * Measured again on 2026-08-12 with it off, on one processor, Hyper-V
 * booting Windows: **15,522,446 VMREADs**, of which `exit_reason` and
 * `vm_exit_instruction_length` were 2,846,294 each - one apiece per
 * reflected exit, so the guest hypervisor was paying four exits and more
 * simply to *look at* every exit handed to it, and the second-level
 * guest advanced about 890 entries a second.
 *
 * A plain constant and **deliberately not a CMake option**. A build
 * switch that changes how the processor is programmed persists in a cache
 * and is inherited silently by every later build; that is exactly how
 * `ZPP_VERIFY_HYPERVISOR` cost a day, and the resulting hang was
 * indistinguishable from a hypervisor bug through seven bisected causes.
 * Flipping this one requires editing a file that appears in the diff.
 *
 * Off restores the previous behaviour exactly rather than approximately:
 * the control is never requested, the link pointer is never written, the
 * bitmaps are never installed, and every copy between the cached vmcs12
 * and the region becomes a no-op - so the guest hypervisor exits for
 * every field access, which is what the handlers have always done. That
 * matters most where there is no other channel: on bare metal a boot
 * either works or it does not, and this is the one variable to move
 * between two such boots.
 *
 * Note the *processor* can still refuse: the control is requested through
 * adjust_msr, and `hypervisor::vmcs_shadowing_enabled` records whether it
 * was granted. On is a request, not an assertion.
 */
#ifndef ZPP_NESTED_SHADOW_VMCS
#define ZPP_NESTED_SHADOW_VMCS 1
#endif

inline constexpr bool shadow_vmcs_enabled = (0 != ZPP_NESTED_SHADOW_VMCS);

/**
 * Whether a guest hypervisor's TPR shadow is handed to the processor, or
 * emulated here instead.
 *
 * On, which is what a processor does and what costs nothing: the guest's
 * `mov cr8` writes the task priority straight into the guest hypervisor's
 * virtual-APIC page with no exit at all, and the processor raises a
 * TPR-below-threshold exit when it crosses the threshold that hypervisor
 * set.
 *
 * Off exists to be the one variable between two boots. It forces CR8
 * load and store exiting instead, and answers both here against the same
 * page - reading and writing byte 0x80, and synthesising the
 * below-threshold exit the processor would have raised. That is strictly
 * slower, one exit per interrupt-priority change where there were none,
 * and it is not a fallback: the point is that a boot which behaves
 * identically either way has ruled the whole mechanism out, and one that
 * does not has localised the fault to it.
 *
 * This is worth having because three separate explanations of the stall
 * have been built out of the task priority register and all three were
 * wrong. `BACKLOG.md` records them. What none of them did was change the
 * mechanism and re-run.
 */
#ifndef ZPP_NESTED_TPR_SHADOW
#define ZPP_NESTED_TPR_SHADOW 1
#endif

inline constexpr bool tpr_shadow_offered = (0 != ZPP_NESTED_TPR_SHADOW);

/**
 * Whether to sample the second-level guest's instruction pointer on a
 * clock this VMM owns.
 *
 * Off by default: it is a profiler, it costs an exit every interval, and
 * it perturbs what it measures.
 *
 * On, it answers the one question the exit rings structurally cannot.
 * Every instrument here is driven by exits, and the guest this VMM is
 * chasing **spins on memory** - it takes no exit at all between clock
 * ticks, so every sample lands in the clock interrupt handler and the
 * handler is not the problem. An exit source the guest does not control
 * is the only way to see the rest, and the VMX-preemption timer is
 * exactly that: it is already this VMM's alone, already stripped from
 * the controls a guest hypervisor is given, and already claimed by
 * `l0_wants_l2_exit`, so arming it for a second-level guest adds a clock
 * and nothing else.
 */
#ifndef ZPP_PROFILE_L2
#define ZPP_PROFILE_L2 0
#endif

inline constexpr bool profile_l2 = (0 != ZPP_PROFILE_L2);

/**
 * Step the trust-level loop with the monitor trap flag. Off unless
 * asked for, and that is a correctness requirement rather than tidiness.
 *
 * It answered what nothing else could - the loop is a clock interrupt
 * whose handler makes a secure call that outlasts the next tick, and
 * three readings in `BACKLOG.md` were wrong until an instruction stream
 * contradicted them. But it is an exit per retired instruction, and
 * measured on the rig the traces are **5.8 per cent of every exit the
 * machine takes**: 28,218 monitor-trap exits against 482,582.
 *
 * That is enough to move the guest between regimes. The boots that
 * produced the livelock data reached the guest's 1.74 ms timer and sat
 * at CLOCK_LEVEL; the three boots taken with this on stayed in an
 * extended-page-table fill regime with the 15.6 ms tick and never got
 * there. Whether the instrument caused that or the boots simply varied
 * is not established - which is exactly why it must not be on by
 * default, because every comparison against an earlier boot is
 * otherwise between two different machines.
 *
 * `BACKLOG.md` already records this rule being learned once, on the
 * single-step-after-injection probe that fired 30,968 times where
 * sixteen were wanted: "Diagnostics that perturb what they measure
 * belong behind a switch or not at all."
 *
 * The counters that come with it - the round-trip halves, the priority
 * histograms, the read-back of what vmcs02 carried - are passive and
 * stay compiled in whatever this says.
 */
#ifndef ZPP_STEP_VTL
#define ZPP_STEP_VTL 0
#endif

inline constexpr bool step_vtl = (0 != ZPP_STEP_VTL);

/**
 * Watch writes to the second-level guest's VP assist page.
 *
 * **Off, because it wedged the guest.** Armed on the frame the two
 * translations agree on, the machine stopped at 90,226 exits with the
 * counters frozen across three reads, the monitor still reporting
 * `running`, and the hypervisor log clean - no unhandled exit, no
 * unwatched-page complaint. A processor taking no exits at all is a
 * spin on memory, and that page carries structures both trust levels
 * touch.
 *
 * The reason for looking was that a page resolved to the *wrong frame*
 * would have been this VMM's bug with a blast radius past lazy
 * end-of-interrupt. That is settled without the watch: the guest
 * hypervisor's own extended-page-table walk and this VMM's map both
 * resolve `0x117a1f000` to `0x117a1f000`.
 *
 * Anyone turning it back on should arm it late and release it after a
 * bounded number of exits rather than arming it for the life of the
 * boot.
 */
#ifndef ZPP_WATCH_VP_ASSIST
#define ZPP_WATCH_VP_ASSIST 0
#endif

inline constexpr bool watch_vp_assist_page = (0 != ZPP_WATCH_VP_ASSIST);

/**
 * Take the deep capture on each trust-level switch: the stack, the code
 * window, the VP assist page, the shared and spin regions.
 *
 * **Off, because it is the last candidate standing for 137.6 VMCS reads
 * a `vmcall`.** The register census before it is cheap and stays; this
 * is the part that walks sixty-four stack words, a 1,024 byte code
 * window, the VP assist page and the shared and spin regions, with the
 * guest page tables behind every one of them.
 *
 * It runs on one switch in sixty-four after the first 4,096 - 1.45% of
 * vmcalls - and an earlier entry retracted it as the cause on exactly
 * that ground. **That retraction proved it does not run unconditionally;
 * it did not prove it is cheap.** A sampled thing dominates if each
 * sample is large enough, and the sampling rate is the multiplier that
 * says how large: 137.6 reads averaged over 1.45% is about 9,500 reads
 * a capture.
 *
 * It has earned its place - it produced `HvlSwitchToVsmVtl1`,
 * `SkpReturnFromNormalMode`, the balanced call and return counts and the
 * protection-mask decode - so it is gated rather than deleted, and a
 * session that needs it again switches it on.
 *
 * If gating this does **not** move a vmcall's accesses, the capture is
 * not the residue and the mechanism behind those reads is still unfound.
 * That is the outcome this switch exists to make possible.
 */
#ifndef ZPP_VTL_CAPTURE
#define ZPP_VTL_CAPTURE 0
#endif

inline constexpr bool capture_vtl_deeply = (0 != ZPP_VTL_CAPTURE);

/**
 * Charge the guest only a fraction of the time this VMM spends in root
 * operation, by moving its time-stamp counter offset. 1 is off, and is
 * the default.
 *
 * **Why this exists, stated as arithmetic rather than as a hope.**
 * Windows arms a *periodic* 1.74 ms tick - 574.7 Hz - and that number
 * is `KeQuantumEndTimerIncrement`, a literal in `ntoskrnl.exe` behind
 * `KiVelocityFlags` bit 18, confirmed both by disassembly and by
 * reading the running guest's memory (`BACKLOG.md`, "574.7 Hz is
 * ntoskrnl's own short-thread-quantum literal"). It is what any Windows
 * on any Hyper-V arms, nothing this VMM says to it changes it, and one
 * tick costs this VMM about 2.46 ms. A handler that cannot finish
 * inside its own period runs for ever, and it does: measured over 191
 * seconds on the rig, zero extended-page-table violations, zero shadow
 * rebuilds, and 99.76% of second-level entries at one of eight
 * instruction pointers, every one of them in the clock path.
 *
 * **Why it is not the fourth version of a lie that failed three
 * times.** `ZPP_STRETCH_GUEST_TIMER`, `ZPP_DELIVER_SELF_IPI` and
 * `ZPP_TICK_FLOOR` each changed *one* thing the guest sees and left the
 * rest honest, and the stretch's own post mortem says why it died:
 * "stretching the period without slowing the reference counter leaves
 * the guest's two time sources disagreeing". Every clock inside this
 * virtual machine is a function of the time-stamp counter - the
 * reference counter through the page this VMM publishes, the
 * performance counter, system time, and the guest hypervisor's own
 * synthetic-timer deadlines - so moving the counter moves all of them
 * together and leaves nothing to disagree.
 *
 * **What is subtracted, and why monotonicity is a property rather than
 * a hope.** Only the interval between one VM exit and the entry that
 * follows it - time in which nothing inside the virtual machine ran at
 * all - and only `(1 - 1/n)` of it. The guest's own execution is never
 * scaled. So across an exit the counter advances by `root / n`, which
 * is never negative, and inside a run it advances at the true rate.
 * This is the "steal time" a paravirtual guest is told about, applied
 * to the clock instead of reported beside it.
 *
 * Hardware TSC scaling would be the direct instrument and this part does
 * not have it - no `tsc_scaling` in the VMX flags on this i7-8565U, and
 * no `shadow_vmcs` beside it. The offset field needs no capability.
 *
 * The guest's wall clock runs slow by construction, which is the whole
 * of the cost: it is a virtual machine being told it was suspended for
 * most of every millisecond, which is true.
 */
#ifndef ZPP_TIME_DILATION
#define ZPP_TIME_DILATION 1
#endif

inline constexpr std::uint64_t time_dilation = ZPP_TIME_DILATION;
inline constexpr bool dilate_time = (1 < time_dilation);

/**
 * Defer the bulk guest-state copy out of vmcs02 into vmcs12.
 *
 * **Off, after three boots and about 280 unclean resets of the rig's
 * real Windows installation.** Worth roughly 1.16x on paper - 44
 * VMREADs an exit that the processor has already made unnecessary by
 * saving the same values into vmcs02 - and every attempt to ship it has
 * reset the guest.
 *
 * Three conditions were found and fixed, the last one on a desk: defer
 * only when vmcs02 has been launched; only when its saved state belongs
 * to the guest being entered; and never across a VMCLEAR-and-reload of
 * the same vmcs12 address. `tests/nested_exit` catches all three, and
 * the third was found *by* the suite rather than by a boot. It still
 * reset the guest, so **a fourth condition exists and is not
 * characterised.**
 *
 * The switch exists rather than a revert because everything around it
 * is sound and was expensive to learn: the ordering suite, the shim's
 * VMCS regions, `set_guest_current_vmcs` owning its own invalidation.
 * Turning this on is the only thing that is not.
 *
 * **Do not turn it on for a rig boot without a new condition to test.**
 * One hypothesis per boot against that installation, with nothing to
 * reason from, is spending the user's hardware on guesses - and this
 * has already spent three.
 */
#ifndef ZPP_DEFER_GUEST_STATE
#define ZPP_DEFER_GUEST_STATE 0
#endif

inline constexpr bool defer_guest_state = (0 != ZPP_DEFER_GUEST_STATE);

/**
 * Run the deferred guest-state copy in **shadow**: compute what it
 * would produce, keep using what the eager path produces, and record
 * where the two differ.
 *
 * Three boots have asked "does my fix hold?" and could only answer by
 * surviving or not. This asks "where does my model of the guest diverge
 * from the guest?" and answers with a field and a moment. Item 1's
 * audit is the same technique and caught a hole on its first boot; this
 * is it applied before shipping rather than after.
 *
 * **Behaviour-neutral by construction**: the shadow path performs one
 * VMREAD of a field this VMM is about to write anyway and compares it
 * against vmcs12. It writes nothing, changes no VMCS state, and takes
 * no decision - so it cannot reset the guest, which is the whole point
 * of using it instead of another live attempt.
 *
 * It is slower, and that does not matter. It is a diagnostic and never
 * a candidate build.
 */
#ifndef ZPP_SHADOW_GUEST_STATE
#define ZPP_SHADOW_GUEST_STATE 0
#endif

inline constexpr bool shadow_guest_state =
    (0 != ZPP_SHADOW_GUEST_STATE);

/**
 * How long the timer runs before it forces an exit.
 *
 * The counter decrements once per time-stamp counter tick shifted right
 * by IA32_VMX_MISC bits 4:0, which is five on every processor this has
 * run on - so a unit is 32 ticks, and ten thousand units is about 160
 * microseconds at 2 GHz. That is roughly six thousand samples a second
 * against the fourteen hundred entries a second the guest already makes,
 * which is enough to see a spin and cheap enough that the guest still
 * runs.
 */
constexpr std::uint64_t profile_timer_value = 10000;

/**
 * The value the guest's current-VMCS pointer takes when there is none.
 *
 * All ones rather than zero, because zero names physical page zero. SDM
 * 33.3, VMXON, sets "current-VMCS pointer := FFFFFFFF_FFFFFFFFH" on
 * entering VMX operation, so this is the architecture's own sentinel and
 * not one invented here.
 */
constexpr std::uint64_t no_current_vmcs = ~std::uint64_t{};

/**
 * The VM-instruction error numbers, from SDM Table 33-1, "VM-Instruction
 * Error Numbers". Only the ones reachable from here are named; the table
 * has more, and every one of the rest belongs to the dual-monitor
 * treatment of system-management interrupts, which this does not
 * implement.
 */
enum class instruction_error : std::uint64_t
{
    vmcall_in_vmx_root_operation = 1,
    vmclear_invalid_address = 2,
    vmclear_with_vmxon_pointer = 3,
    vmlaunch_with_non_clear_vmcs = 4,
    vmresume_with_non_launched_vmcs = 5,
    entry_invalid_control_field = 7,
    entry_invalid_host_state_field = 8,
    vmptrld_invalid_address = 9,
    vmptrld_with_vmxon_pointer = 10,
    vmptrld_incorrect_revision_id = 11,
    unsupported_vmcs_component = 12,
    vmwrite_to_read_only_component = 13,
    vmxon_in_vmx_root_operation = 15,
    invalid_operand_to_invept_invvpid = 28,
};

/**
 * The pin-based controls a first-level hypervisor is permitted to set.
 *
 * A capability MSR reports allowed-0 settings in bits 31:0 and allowed-1
 * settings in bits 63:32 (SDM A.3.1), so this is the mask the hardware's
 * allowed-1 half is narrowed by. Narrowing is the point: every control
 * reported here is one whose effect would have to be honoured when a
 * second-level guest runs, and reporting one that is then ignored is the
 * half-answered interface this codebase warns about.
 *
 * External-interrupt and NMI exiting are the two a hypervisor cannot do
 * without, since without them it cannot own its own guest's interrupts.
 * The VMX-preemption timer is deliberately absent, and there is a second
 * reason beyond honesty: the test rig runs QEMU with hv-passthrough, which
 * filters the capability MSRs through enlightened VMCS version 1 and
 * reports the timer as not permitted anyway - so a build that asked for it
 * would fail its own VM entry on the machine it is developed on. Hence the
 * mask is applied *to* the hardware's value rather than replacing it.
 */
constexpr std::uint64_t supported_pin_based_controls =
    (1ull << 0) | // External-interrupt exiting.
    (1ull << 3) | // NMI exiting.
    (1ull << 5);  // Virtual NMIs.

/**
 * The primary processor-based controls a first-level hypervisor may set.
 *
 * The MSR bitmaps and the secondary controls are the two that matter: a
 * hypervisor without the bitmaps traps every MSR access its guest makes,
 * and one without the secondary controls has no EPT and no unrestricted
 * guest. The rest are the plain intercepts, each of which costs nothing
 * to honour because it only ever adds an exit.
 *
 * Absent on purpose: the CR3-target list, which needs four VMCS fields
 * nothing here consults - IA32_VMX_MISC reports a target count of zero to
 * say so.
 *
 * **The TPR shadow is here because a real guest hypervisor was measured
 * setting it.** BACKLOG.md records the capture, read out of Hyper-V's own
 * vmcs12 at the first `build_vmcs02` on the run where it engaged: pin
 * 0x0000001e, primary **0xa4206dfa** - bit 21 set - secondary 0x00000000,
 * exit 0x0003efff, entry 0x000013ff. It is the one control in that set
 * this VMM withheld, and withholding it is what produced seventeen
 * second-level entries followed by a boot loop: the guest hypervisor was
 * told it could have the control, set it, and had `build_vmcs02` remove it
 * with no compensating CR8-load or CR8-store exiting - so every `mov cr8`
 * its own guest executed reached the *physical* control register and the
 * virtual-APIC page it was told to expect was never written.
 *
 * Offering it is therefore only half of it. `build_vmcs02` honours it: it
 * validates the virtual-APIC address vmcs12 names, writes it and the TPR
 * threshold into vmcs02, and leaves the control set. What it does not
 * honour it refuses, or removes and replaces with CR8 load and store
 * exiting - see there.
 *
 * What is still absent is everything the TPR shadow is a prerequisite
 * *for*. SDM 29.2.1.1: "If the 'use TPR shadow' VM-execution control is
 * 0, the following VM-execution controls must also be 0: 'virtualize
 * x2APIC mode', 'APIC-register virtualization', 'virtual-interrupt
 * delivery', and 'IPI virtualization'." That pairing was once the argument
 * for doing this work alongside secondary bit 4; the capture above retires
 * it, because the guest hypervisor sets **no** secondary control at all.
 * The pairing is real and now runs the other way round: offering the TPR
 * shadow permits those controls to be offered later, and each still needs
 * its own state before it may be. None is offered today, which is why the
 * only user of the virtual-APIC page here is `mov cr8` (SDM 27.6.8 lists
 * the three: MOV CR8, the APIC-access page under "virtualize APIC
 * accesses", and the APIC MSRs under "virtualize x2APIC mode" - the latter
 * two need controls this VMM withholds).
 */
constexpr std::uint64_t supported_primary_controls =
    (1ull << 2) |  // Interrupt-window exiting.
    (1ull << 3) |  // Use TSC offsetting.
    (1ull << 7) |  // HLT exiting.
    (1ull << 9) |  // INVLPG exiting.
    (1ull << 10) | // MWAIT exiting.
    (1ull << 11) | // RDPMC exiting.
    (1ull << 12) | // RDTSC exiting.
    (1ull << 15) | // CR3-load exiting.
    (1ull << 16) | // CR3-store exiting.
    (1ull << 19) | // CR8-load exiting.
    (1ull << 20) | // CR8-store exiting.
                   //
                   // The pair above is what a guest hypervisor is given
                   // *instead* of the TPR shadow, and that is deliberate.
                   //
    (1ull
     << 21) | // Use TPR shadow - OFFERED, experiment in progress.
              //
              // **Re-offered because the stall that withdrew it is now
              // suspected to have been a different bug entirely.** The
              // symptom recorded both times it was withdrawn was seven
              // application processors left in the firmware's wait loop
              // and the boot processor spinning for them - which is the
              // *identical* symptom to a failure since traced to the
              // emulator being told to offer interrupt remapping that
              // the host kernel does not implement, and fixed by
              // turning that off. Every run that withdrew this bit was
              // made with the broken setting in place, so the stall was
              // never attributable to the bit on its own.
              //
              // What to read after this boot, in order: guest_vmxon_count
              // (never once read in a run with this bit offered), then
              // vmcs12_controls_captured and l2_entries. Zero vmxon means
              // the capability set is still being refused and the answer
              // is elsewhere; non-zero means a guest hypervisor entered
              // VMX operation here for the first time.
              //
              // Bit 21, use TPR shadow, was previously **not** offered,
              // and that is the one thing standing between this VMM and a
              // guest hypervisor.
              //
              // **This is the bit a guest hypervisor refuses on,
              // and that is now established from its own code
              // rather than inferred.** The guest's loader gathers
              // the capability MSRs and compares them against a
              // required table it carries as immediates; the
              // primary-controls requirement is 0xe7f9fffe. Against
              // the 0xffd9fffe this VMM reported, `required &
              // ~offered` is 0x00200000 - bit 21, and nothing else.
              // Every other comparison in that table passes. The
              // refusal is silent: it returns a status meaning
              // invalid device request and never opens the
              // hypervisor image at all, which is exactly the
              // measurement this tree had - the capability MSRs read
              // 28 times and VMXON executed zero times.
              //
              // It was offered once before and withdrawn, because
              // advertising it deterministically stalled the guest's
              // own application-processor start-up: seven processors
              // left in the firmware's wait loop, measured twice,
              // with `build_vmcs02` never running in either - so the
              // advertisement alone, not the honouring of it.
              //
              // Offering it again has now been tried, with all
              // three of the local-APIC defects below fixed, and
              // **the stall came back**: the guest bounced between
              // two addresses for seven minutes and never reached
              // the Windows kernel. So the APIC repairs were not
              // the whole of it, and the bit stays off until the
              // stall is understood rather than hoped away.
              //
              // What had changed since the first withdrawal is the
              // local APIC path it stalled in, and all of it was
              // defective at the time:
              // the interrupt command register's handler tested a
              // read-only delivery-status bit and so refused every
              // xAPIC command; a write that had to be stepped rather
              // than emulated reached the handler with no address,
              // and 149 of them were dropped on one boot; and the
              // APIC mode was not followed across a write of
              // IA32_APIC_BASE. A start-up sequence losing its
              // interrupts explains the stall exactly, and none of
              // those three defects existed to be ruled out when the
              // bit was withdrawn.
              //
              // The alternative remains recorded rather than
              // deleted, because it is what to fall back to if the
              // stall returns: SDM 27.6.8 makes CR8-load and
              // CR8-store exiting the architectural substitute, and
              // KVM's nested_get_vmcs12_pages makes the same
              // substitution when it cannot map the virtual-APIC
              // page. That path costs only reflected exits, which
              // `l1_wants_l2_exit` already answers - but it is not
              // what the guest hypervisor asks for, and it will not
              // launch without the bit.
    (1ull << 22) | // NMI-window exiting.
    (1ull << 23) | // MOV-DR exiting.
    (1ull << 24) | // Unconditional I/O exiting.
    (1ull << 25) | // Use I/O bitmaps.
    (1ull << 27) | // MONITOR trap flag.
    (1ull << 28) | // Use MSR bitmaps.
    (1ull << 29) | // MONITOR exiting.
    (1ull << 30) | // PAUSE exiting.
    (1ull << 31);  // Activate secondary controls.

/**
 * The secondary processor-based controls a first-level hypervisor may set.
 *
 * Extended page tables are the one that decides whether a real guest
 * hypervisor can run here at all: Hyper-V and every other modern one
 * require them. They are offered because the shadow exists -
 * `build_shadow_ept` composes the first level's tables with this VMM's own
 * and hands the result to VM entry as the EPT pointer, so the control is
 * honoured rather than merely reported.
 *
 * VPIDs are offered for the same reason Hyper-V wants them, and honoured
 * in the way SDM 31.4.1 makes sufficient rather than by allocating one:
 * the second-level guest runs under *this* VMM's VPID, and the transitions
 * either side of it invalidate it. A second-level guest's own mappings
 * cannot be confused with the first level's regardless, because they are
 * combined mappings associated with the shadow's EPT root address as well
 * as with the VPID, and the two roots differ.
 *
 * Unrestricted guest is offered because a guest hypervisor starting its
 * own processors starts them in real mode, and without it every one of
 * those entries would fail the guest-state checks.
 *
 * Absent on purpose: everything to do with the local APIC - virtualized
 * APIC accesses, x2APIC virtualization, APIC-register virtualization,
 * virtual-interrupt delivery - because each needs pages and state of its
 * own that nothing here maintains. Mode-based execute control is absent
 * too, and its absence is load bearing rather than incidental: bit 10 of a
 * guest hypervisor's own extended page-table entries means nothing it
 * chose, so composing it into the shadow would deny user-mode execute
 * across the whole second-level guest. `build_vmcs02` therefore also
 * clears the control it inherits from this VMM's own VMCS.
 */
constexpr std::uint64_t supported_secondary_controls =
    (1ull << 1) |  // Enable EPT.
    (1ull << 2) |  // Descriptor-table exiting.
    (1ull << 3) |  // Enable RDTSCP.
    (1ull << 5) |  // Enable VPID.
    (1ull << 6) |  // WBINVD exiting.
    (1ull << 7) |  // Unrestricted guest.
    (1ull << 10) | // PAUSE-loop exiting.
    (1ull << 11) | // RDRAND exiting.
    (1ull << 12) | // Enable INVPCID.

    // Enable VMFUNC, SDM Table 25-7 bit 13.
    //
    // Virtual secure mode switches trust level by switching the
    // extended-page-table pointer, and where the processor offers
    // VMFUNC it does that without a hypercall at all. Withheld, the
    // guest hypervisor falls back to the hypercall path - and that path
    // is the HvCallVtlCall/HvCallVtlReturn pair measured alternating
    // for ever with byte-identical state.
    //
    // Safe to offer because `build_vmcs02` forces vmcs02's VM-function
    // *controls* to zero, so every VMFUNC exits to this VMM and the
    // processor never loads a pointer itself. The list the guest
    // hypervisor publishes holds its own pointers; each is translated
    // through `shadow_ept_pointer_for` before anything reaches the
    // hardware.
    (1ull << 13) | (1ull << 16) | // RDSEED exiting.
    (1ull << 20) |                // Enable XSAVES/XRSTORS.

    // Mode-based execute control, SDM Table 25-7 bit 22.
    //
    // **This is how a secure kernel expresses code integrity through the
    // extended page tables**, by splitting execute permission in two -
    // executable in supervisor mode, executable in user mode - so a page
    // can be one and not the other. Without it there is no way to say
    // "this page may not be executed by the kernel", which is the whole
    // of what hypervisor-enforced code integrity does.
    //
    // Absent until now, and its absence explains the measurement nothing
    // else did: `HvCallModifyVtlProtectionMask` is issued repeatedly -
    // once with a repeat count of 510 - and `reflected_permission` is 0
    // of 448,441, so the protection change is accepted and never appears
    // in the tables this VMM shadows. A guest hypervisor that cannot
    // split execute cannot put that policy anywhere.
    //
    // The composition already handles it: `nested_ept.h` takes
    // `mode_based_execute_control` and reads `execute_user()`, and Table
    // 30-7's rule that bit 6 exists only with the control set is written
    // there. Only the advertisement was missing.
    //
    // Baseline for the comparison, measured 2026-08-15: the same guest
    // reaches ring 3 in about five minutes under KVM alone, which does
    // advertise this bit.
    (1ull << 22);

// Deliberately absent: **use TSC scaling**, SDM Table 25-7 bit 25.
//
// It was offered, run on the rig and withdrawn on 2026-08-13. The
// hypothesis it tested was that a guest hypervisor which cannot scale its
// guest's time-stamp counter will not vouch for one either - which would
// explain the reference TSC page Windows enables and Hyper-V leaves with
// a sequence of zero, and the 42% of all exits the resulting fallback to
// the reference counter MSR costs. See BACKLOG.md.
//
// Hyper-V did not take it. `control_secondary_requested` reads 0x1010ae
// with bit 25 clear, so the control was advertised and never asked for,
// the guest stalled at the same instruction as before, and the steady
// state was unchanged at 89.9% of the wall clock against 89.2%.
//
// So it is withdrawn rather than left standing, because a capability
// nothing has ever exercised is a claim this VMM cannot back with a
// measurement. `build_vmcs02` keeps the composition it grew for this -
// the multiplier composed and the offset scaled through it, rather than
// the two offsets added - since that is what the architecture requires
// the day anything does use it, and with the bit absent it reduces
// exactly to the sum it replaced.

/**
 * The extended-page-table and VPID capabilities reported to a first-level
 * hypervisor through IA32_VMX_EPT_VPID_CAP, from SDM A.10.
 *
 * Narrowed from the hardware's rather than replacing it, like every other
 * capability here, so a machine that cannot do one of these does not have
 * it promised on its behalf. Each bit below is one the shadow builder
 * honours:
 *
 * - bit 6, four-level page walks, because `shadow_ept_entry` descends
 *   exactly four and `build_shadow_ept` reads a PML4 at the top.
 * - bit 8, uncacheable, and bit 14, write-back, as the memory types an
 *   EPT pointer may name. Both are accepted because the shadow's own
 *   pointer type is this VMM's choice and the leaf types come from the
 *   memory-type range registers either way - which is the divergence
 *   BACKLOG.md records under E9.
 * - bits 16 and 17, 2 MB and 1 GB leaves, because the builder reads both.
 *   It installs neither as a 1 GB shadow leaf - it fans a 1 GB mapping
 *   out to 2 MB entries - but what the bits report is what a *guest
 *   hypervisor* may write, not what the shadow holds.
 * - bit 20 with bits 25 and 26, INVEPT and both of its types, because
 *   `on_guest_invept` answers them by discarding the shadow.
 * - bit 32 with bits 40 through 43, INVVPID and all four of its types,
 *   because `on_guest_invvpid` answers every one of them the same way,
 *   by invalidating this VMM's own VPID entirely. SDM 31.4.3.2 permits a
 *   processor to invalidate more than it was asked to, which is what
 *   makes one answer serve four types.
 *
 * Absent on purpose: bit 0, execute-only translations, which
 * `execute_only_translations_offered` also reports false and which decides
 * what `ept_permissions::normalised` may leave in an entry - the two must
 * agree. Bit 21, accessed and dirty flags, because the shadow never sets
 * them and a guest hypervisor reading them would find nothing ever
 * accessed, and because `build_vmcs02` refuses an EPT pointer asking for
 * them - the other half of withholding the bit. Bit 22, advanced VM-exit
 * information, because the reflected exit qualification does not carry
 * bits 11:9.
 *
 * Neither of the first two is what a real guest hypervisor was found to be
 * objecting to. BACKLOG.md records the measurement and the correction: the
 * run in which one engaged was made with this mask still applied, because
 * the diagnostic that widened the control MSRs never reached this one. So
 * they are honest gaps rather than the gate.
 */
constexpr std::uint64_t supported_ept_vpid_capabilities =
    (1ull << 6) |  // Four-level page walks.
    (1ull << 8) |  // Uncacheable EPT paging structures.
    (1ull << 14) | // Write-back EPT paging structures.
    (1ull << 16) | // 2 MB EPT leaves.
    (1ull << 17) | // 1 GB EPT leaves.
    (1ull << 20) | // INVEPT.
    (1ull << 25) | // INVEPT single-context.
    (1ull << 26) | // INVEPT all-context.
    (1ull << 32) | // INVVPID.
    (1ull << 40) | // INVVPID individual-address.
    (1ull << 41) | // INVVPID single-context.
    (1ull << 42) | // INVVPID all-context.
    (1ull << 43);  // INVVPID single-context retaining globals.

/**
 * The VM-exit controls a first-level hypervisor may set.
 *
 * "Host address-space size" is here because a 64-bit hypervisor cannot do
 * without it, and the MSR-area ones because saving and loading IA32_PAT
 * and IA32_EFER across a transition is a field copy and nothing more.
 * "Save VMX-preemption timer value" is absent to agree with the pin-based
 * mask, which does not offer the timer.
 */
constexpr std::uint64_t supported_exit_controls =
    (1ull << 2) |  // Save debug controls.
    (1ull << 9) |  // Host address-space size.
    (1ull << 15) | // Acknowledge interrupt on exit.
    (1ull << 18) | // Save IA32_PAT.
    (1ull << 19) | // Load IA32_PAT.
    (1ull << 20) | // Save IA32_EFER.
    (1ull << 21) | // Load IA32_EFER.
    (1ull << 23);  // Clear IA32_BNDCFGS.
                   //
                   // The other half of the entry control that loads it,
                   // and a guest hypervisor wants **both or neither**.
                   // Established from underneath rather than guessed:
                   // with the emulator's own capability set degraded to
                   // this VMM's and then restored one control at a time,
                   // the guest hypervisor launched only when the entry
                   // and exit halves were offered together. Entry alone
                   // was measured and was not enough.
                   //
                   // The same run cleared four other differences of
                   // suspicion, which is worth as much as the finding:
                   // virtualize x2APIC mode, execute-only translations,
                   // accessed and dirty flags, and the two instruction
                   // information bits were all absent while it launched,
                   // so none of them is required. That retires the
                   // theory that the local APIC path was implicated.
                   //
                   // Nothing is claimed that is not carried: the guest's
                   // IA32_BNDCFGS travels into the entered VMCS and back
                   // out with it, beside IA32_PAT and IA32_EFER.

/**
 * The VM-entry controls a first-level hypervisor may set.
 *
 * "Entry to SMM" and "deactivate dual-monitor treatment" are absent: SDM
 * Table 27-17 says both "must be 0 for any VM entry from outside SMM",
 * and every entry reachable here is from outside SMM.
 */
constexpr std::uint64_t supported_entry_controls =
    (1ull << 2) |  // Load debug controls.
    (1ull << 9) |  // IA-32e mode guest.
    (1ull << 14) | // Load IA32_PAT.
    (1ull << 15) | // Load IA32_EFER.
    (1ull << 16);  // Load IA32_BNDCFGS.
                   //
                   // **This one bit is what a guest hypervisor refuses
                   // on, and it was found by measurement rather than by
                   // reading.** With the emulator underneath answering
                   // the capability MSRs itself, its advertised set was
                   // degraded one group at a time towards this VMM's,
                   // and the guest hypervisor was booted at each step:
                   // it launched until this control was withdrawn, and
                   // stopped the moment it was. Four boots, no change to
                   // this tree, and the answer is a bit neither the
                   // capability comparison nor the guest loader's own
                   // required table pointed at - that table covers the
                   // *primary* controls, and this is an entry control,
                   // so nothing in it could ever have named this.
                   //
                   // Measured either side of the change:
                   //   underneath, offering it: 0x0001d3ff000011fb
                   //   this VMM, without it:    0x0000d3ff000011fb
                   //
                   // Honoured rather than merely claimed: the guest's
                   // IA32_BNDCFGS is carried into the entered VMCS and
                   // back out again, next to IA32_PAT and IA32_EFER,
                   // which are the two controls this sits beside and
                   // which already worked that way.

} // namespace zpp::hypervisor::nested_vmx
