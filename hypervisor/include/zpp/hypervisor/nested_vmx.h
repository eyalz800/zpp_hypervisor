#pragma once
#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
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

/**
 * **This coupling is why the enlightenment has never been negotiated,
 * and it makes `ZPP_EVMCS=ON` two changes rather than one.**
 *
 * Offering the enlightened VMCS also announces a Hyper-V-compatible
 * interface, signature `Hv#1`, because eVMCS is advertised through the
 * nested-features leaf *inside* the `0x40000000` block - so the two
 * cannot be separated. Measured either side of the switch: **187 CPUID
 * entries in the hypervisor range with it on, zero on every other
 * boot.**
 *
 * What the guest does with that: probes the block, then `vmon`,
 * `vmptrld`, ninety-nine **real** `vmwrite`s, **one** `vmlaunch`,
 * `vmclear`, `vmoff`. It stands down. `hyperv_vp_assist_writes`,
 * `evmcs_reads`, `evmcs_writes` and `evmcs_recommended` all read **0**,
 * and so do `nested_vmfail_count` and `nested_entry_error` - nothing is
 * refused, because nothing is attempted a second time. Ninety-nine real
 * VMWRITEs is itself the proof the enlightened path was not taken.
 *
 * So the eVMCS defects found by reading the code - the over-imported
 * MSR-area fields, the VMfail an enlightened guest cannot see, the
 * unguarded VMCLEAR, the conflated pointer - are all on a path this
 * machine never reaches. Fixing them changes nothing until Windows' own
 * hypervisor agrees to run under an announced one, and here it declines.
 * That is a question about what the whole block claims. `BACKLOG.md` has
 * the census.
 */
/**
 * Set CPUID leaf 1 ECX bit 31 - "you are virtualized" - **without**
 * advertising a hypervisor interface block at `0x40000000`.
 *
 * **These are two different claims and the coupling above conflates
 * them.** The bit says only that a hypervisor is present. The block says
 * *which* hypervisor and what it offers, and it is the block that makes
 * Windows' own hypervisor stand down: with `ZPP_EVMCS=ON` it probes the
 * block, executes `vmon`, `vmptrld`, ninety-nine real `vmwrite`s, one
 * `vmlaunch`, then `vmclear` and `vmoff`. It declines to run under an
 * announced peer.
 *
 * The bit alone has never been tried, and it is the half that decides a
 * different question: **which virtualization-based-security path the
 * guest takes.** The `cpuid` handler's own comment states the stake -
 * "a guest that believes it is on bare metal applies bare metal
 * requirements to virtualization-based security, Secure Boot among them,
 * and this rig reports Secure Boot unsupported. A guest that knows it is
 * virtualized takes the nested path instead."
 *
 * Currently the bit is **clear**, because `announce_hypervisor` follows
 * `evmcs_offered` and eVMCS is off - so Hyper-V believes it is on bare
 * metal and applies bare-metal VBS requirements on a machine that cannot
 * meet them. That is a candidate explanation for a VBS state machine
 * that initialises and then will not advance.
 *
 * Off by default because it is a claim about the machine and an untested
 * one. On, expect either the guest to take a different VBS path - which
 * is the point - or to stand down as it does for the full block, which
 * would show as `vmoff` and settle the question the other way.
 */
#ifndef ZPP_ANNOUNCE_HYPERVISOR_BIT
#define ZPP_ANNOUNCE_HYPERVISOR_BIT 0
#endif

inline constexpr bool announce_hypervisor_bit =
    (0 != ZPP_ANNOUNCE_HYPERVISOR_BIT);

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

/**
 * Whether a self-directed synthetic interrupt the requesting processor
 * cannot currently take is **withheld from the guest hypervisor** and
 * delivered here instead, at the first entry the guest's own task
 * priority admits it.
 *
 * `deliver_self_ipi` above is the second half of this and was never
 * enough on its own. Measured with it alone: one delivery and 2,005
 * holds - the priority rule was right and the holds were correct - and
 * the guest was no better off, because the write was **also** reflected,
 * so the guest hypervisor queued the same vector as pending and behaved
 * exactly as before. The two halves have never been run together and
 * each alone does nothing.
 *
 * What the reflection costs, measured on the rig over one 257 second
 * window on a settled guest:
 *
 * - vector `0x2f` requested 145,300 times through this register and
 *   delivered **zero**;
 * - the virtual task priority never once below `0x20` across 630,418
 *   second-level entries, at either trust level, so the vector was
 *   refused correctly every time - `0x2f` is class 2 and the rule is
 *   strictly greater, SDM 12.8.4;
 * - and the guest hypervisor asserted its virtual interrupt
 *   notification into VTL1 on **1.0032 of every `HvCallVtlCall`**,
 *   because the vector it will not deliver is nevertheless pending.
 *
 * VTL1's `ShvlVinaHandler` answers that notification by returning to
 * VTL0 with secure-call state `4`, `ntoskrnl`'s `VslpEnterIumSecureMode`
 * has no case for `4` and re-enters, and the secure call never retires -
 * which is the whole of the stall. The state byte is byte 1 of RBX,
 * carried across the trust-level boundary by `HvlSwitchToVsmVtl1`, and
 * it read `4` on 25,659 consecutive returns with every register's
 * change counter at zero.
 *
 * So the lever is the reflection, and it is the only one left on this
 * side: stop telling the guest hypervisor about a self-directed
 * interrupt the requesting processor cannot take, and deliver it here
 * when it can.
 *
 * **This misrepresents nothing.** The four interventions before it all
 * lied about *time* and Windows checked its clocks against each other
 * and bugchecked. Here the interrupt is genuinely undeliverable at the
 * instant it is requested, by the guest's own task priority, and it is
 * delivered at the first instant it is deliverable, by the rule the
 * processor itself would apply. Nothing observes a value it could not
 * have observed on hardware.
 *
 * **The honest risk, which is why this is off by default**: the guest
 * hypervisor may keep bookkeeping of its own that depends on seeing the
 * write - a pending-interrupt count, a synthetic message, an
 * end-of-message pairing - and swallowing it could desynchronise
 * something invisible from here. Only self-directed commands are
 * swallowed, and only while undeliverable, so a command that outranks
 * the priority still reflects and the level above still sees it.
 *
 * **It was tried on the rig and it is refuted. Do not turn it on.**
 *
 * Two boots, one variable, same tree, both deployed hash-verified and
 * both module bases read per run:
 *
 * | | `swallow=1` | `swallow=0` control |
 * |---|---|---|
 * | `l2_entries` | 38,974 then **frozen 6+ minutes** | 571,550 -> 1,052,033, climbing |
 * | `vtl_switches` | 8,611, frozen | 51,524 -> 80,027, climbing |
 * | last exit | **`hlt` at the L1 rip** | still running |
 *
 * The mechanism is in that last exit. The guest hypervisor took a
 * trust-level call, two more hypercalls, and then **halted** - and
 * nothing woke it, so this VMM stopped taking exits entirely at 204,225.
 * The machine was not paused, not faulted, and no host exception,
 * unhandled exit or entry failure was recorded.
 *
 * **The level above uses the pending interrupt as its own wake
 * condition.** It cannot deliver `0x2f` and it does not idle while it is
 * outstanding; withhold the write and it has nothing left to wait for.
 * So the interrupt being permanently pending is not only the thing that
 * keeps the notification asserted - it is also the thing that keeps the
 * level above running at all, and the two cannot be separated from here.
 *
 * Note what it is **not**: `l2_self_ipi_pending` was `0` at the freeze
 * and the single swallowed request had been delivered normally, held
 * 2,691 entries and then injected. The delivery half worked exactly as
 * designed. The harm is not a vector held for ever.
 *
 * **And the harm is probably not the withholding either - this comment
 * over-claimed and `force_dispatch_once` caught it.** That switch
 * withholds *nothing*, reflects every write, and froze the machine the
 * same way: `hlt` at the L1 rip, `vmptrld` four times, then no exit at
 * all. The one thing both boots share, and the control does not, is that
 * **the delivery path injected a vector the level above did not stage**.
 * `hlt` appears zero times in a known-good boot of 17 million
 * second-level entries. Read the two comments together: the wake-condition
 * story is a hypothesis with one supporting boot and one boot against
 * it, and the injection story fits both.
 *
 * The prediction this was built to test - the notification rate falling
 * from 1.00 per call - was never reached, because the guest never got as
 * far as the secure-call loop. That is a failure to test rather than a
 * test that passed, and it is recorded as one.
 *
 * What would have to change before this is worth another boot: something
 * that keeps the level above awake while still withholding the vector -
 * which means understanding what it waits on, and that is above this VMM
 * and not visible from it.
 */
inline constexpr bool intercept_self_ipi =
#if defined(ZPP_INTERCEPT_SELF_IPI) && ZPP_INTERCEPT_SELF_IPI
    true;
#else
    false;
#endif

/**
 * Whether the held-self-IPI delivery path runs at all.
 *
 * Both switches need it, and they differ only in whether the write is
 * also reflected - so the delivery site is keyed on this rather than on
 * either one, and adding a third way to hold a vector does not have to
 * find that site again.
 */
/**
 * Whether this VMM delivers the deferred-call vector **once, ever**, in
 * deliberate violation of the architectural masking rule, as a
 * diagnostic.
 *
 * **This is an architectural violation and nothing here pretends
 * otherwise.** The guest masked the vector; SDM 12.8.4 admits an
 * interrupt only when its priority class is strictly greater than the
 * task priority's, and vector `0x2f` is class 2 against a task priority
 * of `0x20`, which is also class 2. `deliver_self_ipi` computes that
 * rule correctly and holds - measured, 2,005 holds against one delivery.
 * This relaxes it exactly once.
 *
 * The reason it is worth one boot: the stall is a three-link cycle and
 * two links are closed by measurement.
 *
 * ```
 * IRQL 2        -> 0x2f masked, pending for ever
 * 0x2f pending  -> the trust-level notification asserted on 100% of entries
 * notification  -> the secure call never retires -> IRQL never drops
 * ```
 *
 * Link two was tested by `intercept_self_ipi` and is closed - withhold
 * the request and the level above halts, because it uses the pending
 * interrupt as its own wake condition. Link three was tested against the
 * round-trip cost and is closed - the notification is a level on a
 * permanently pending interrupt, so no speed reaches it.
 *
 * **That last statement is about escape and must not be read as being
 * about entry.** Sampling the transition from boot later showed the
 * guest running for 53 seconds at 0.2 clock ticks per trust-level round
 * trip and entering the stall, in one sample, when it re-armed its own
 * timer from 64 Hz to 574 Hz and the figure went to 3.6. Speed cannot
 * get the guest *out*; it is exactly what decides whether it goes *in*.
 * See `BACKLOG.md`. **Link one has
 * never been touched**, and one delivery is all the cycle needs: the
 * deferred call drains, `0x2f` stops being pending, the notification
 * deasserts, the secure service retires, and the priority drops - after
 * which the cycle cannot re-form, because the guest is no longer pinned
 * above the dispatcher.
 *
 * Three things make it less reckless than it sounds, and **all three
 * were read out of the guest on the boot this was armed for** rather
 * than assumed:
 *
 * - `0x2f` is the deferred-call dispatch and its handler runs *at*
 *   DISPATCH_LEVEL - `0x2f >> 4` is 2 - which is exactly where the guest
 *   already is. It is entered at the priority it was written for.
 * - Windows masks it at IRQL >= 2 to stop the dispatcher being
 *   re-entered, and `KPRCB.DpcRoutineActive` read **0**, twice, 45
 *   seconds apart - there is no dispatcher to re-enter. It cannot change
 *   under us either: the guest's registers are byte-identical across
 *   every switch, so it is executing nothing that could set it.
 * - `DpcData[0].QueueDepth` read **1 -> 1** with a maximum ever of 4, so
 *   one delivery drains one deferred call rather than releasing a storm.
 *
 * **The reflection is deliberately left alone.** `intercept_self_ipi`
 * proved the level above needs to see the write, so this switch adds an
 * injection and withholds nothing - the two are independent and must
 * stay that way.
 *
 * Gated as hard as it can be: once ever per processor, and only when the
 * sampled virtual task priority is exactly `0x20`. Not `0xd0`, where the
 * guest is inside the clock interrupt and a dispatch interrupt would be
 * genuinely wrong; and not `0x40`, which is the other trust level. The
 * interruptibility test above is **not** relaxed - `RFLAGS.IF` clear or
 * an `STI`/`MOV SS` shadow still refuses, because those are the guest
 * protecting a critical section rather than setting a priority.
 *
 * Off by default, and it stays off whatever the outcome: it is an
 * experiment, not a fix.
 *
 * **It was booted. It did not test what it was built to test, and the
 * instrument is unsound as written.**
 *
 * The gate worked exactly as specified - `l2_forced_dispatch` read `1`,
 * and the log carries `forced dispatch vector 0x2f at task priority
 * 0x20`. But **the whole boot contained only two self-directed requests**
 * (`vectors the guest asked for (2)`), where the stall produces 1.27
 * million. So the single shot was spent during early boot, thousands of
 * seconds before `VslpEnterIumSecureMode` is ever reached. "Once ever, at
 * priority `0x20`" is not selective enough: early boot is full of
 * moments at `0x20`.
 *
 * The guest then froze at 54,604 second-level entries - and it froze
 * **exactly as the `intercept_self_ipi` boot did**: last exit `hlt` at
 * the L1 rip, then `vmptrld` four times, then no exit at all. No
 * bugcheck: `KiBugCheckData` read `0` in all five words, taken before
 * the guest was killed.
 *
 * **Which corrects what `intercept_self_ipi`'s comment claims.** That
 * one blamed the freeze on withholding the write - the level above
 * losing its wake condition. The simpler explanation is the one thing
 * both boots share and the control does not: **the delivery path
 * injected a vector the level above did not stage.** `hlt` appears
 * **zero times** in a known-good boot of 17 million second-level
 * entries and **once** in each experimental boot, as the last exit.
 * Injecting into a nested guest whose virtual interrupt controller is
 * owned by the level above is not a complete operation - the
 * acknowledgement protocol belongs to that level - and this is the
 * failure `CLAUDE.md` describes as answering part of an interface.
 *
 * So link one is not closed and not open: it is **untested**, and
 * testing it needs an instrument that does not exist - one armed only
 * once the stall is confirmed present (the notification rate at 1.00
 * over thousands of switches), and an injection that the level above
 * can account for. The second of those is the hard one and it may not
 * be reachable from underneath at all.
 */
/**
 * Whether an extended-page-table fault installs the pages *around* the
 * one that faulted, as well as the one that did.
 *
 * The one lever nobody had tried: every other change in this file made an
 * exit cheaper and none made there be fewer. Measured on a verified
 * build, clean-phase window: `ept-violation` is **16.31 a trust-level
 * round trip and 61.1% of all exits**, at 18.0 VMCS accesses each - **293
 * of the 987 accesses a round trip, 30%**. One leaf is installed per
 * fault, so the count is the whole cost.
 *
 * **Soundness, and it is short**: only mappings the guest's own tables
 * already permit are installed, composed by the same `compose_ept` the
 * faulting page goes through, at the same page size. A change the guest
 * makes to those tables obliges it to INVEPT, and `on_guest_invept`
 * discards the whole root - so an eagerly installed entry cannot outlive
 * the permission it was composed from. That is the argument
 * `replay_shadow_recall` already rests on, and it was verified there.
 *
 * **The honest risk is not correctness, it is waste.** A neighbour walk
 * costs four guest-physical reads, about 11.5 microseconds; a fault
 * avoided is worth about 24.5. So a window whose pages are never touched
 * is a net loss, and whether it pays depends entirely on the guest's
 * locality. `shadow_ept_neighbours_filled` counts what was installed and
 * the fault rate says what it bought - if faults a round trip do not
 * fall, this is costing and not saving.
 *
 * **Sized before building, so it is not discovered afterwards**: taking
 * 16.31 faults to 4 saves ~222 accesses a round trip, about 302
 * microseconds of 3,450 - **under 9%**, against the ~50% the round trip
 * needs to fall below the guest's own tick. This cannot reach the goal on
 * this machine and is not expected to. It is worth doing because it is
 * the last untested line, and a line that is asserted rather than tested
 * is not closed.
 *
 * **Built, booted, and it is a net loss. Leave it off.**
 *
 * | | baseline | eager |
 * |---|---|---|
 * | faults per round trip | 16.31 | **12.92** (-21%) |
 * | exits per round trip | 26.7 | 23.3 |
 * | clean round trip | 3.45 ms | **4.07 ms** (+18%) |
 * | settled ticks/RT | 5.01 | **6.15** |
 *
 * **The mechanism worked and the result went backwards.** Faults did
 * fall - by 21%, not to 4 - and the round trip got *worse*, so the guest
 * stalls sooner rather than later.
 *
 * Attributed rather than guessed, from the phase decomposition:
 *
 * ```
 * on_l2_ept_fault        377.9 -> 735.1 us/RT  (+357)  calls 16.4 -> 12.6
 *   so per fault           23.0 -> 58.3 us     - 2.5x more expensive
 * map_window repoints    282.7 -> 580.1 /RT    (+297)
 * everything else        unchanged to within 17 us
 * ```
 *
 * Each fault now walks seven neighbours, and each walk repoints the
 * mapping window about four times. That is +357 microseconds a round trip
 * to save about 3.4 faults worth ~83 - **a four-to-one loss.**
 *
 * And the reason it cannot be tuned into profit is structural: **1.1
 * million neighbours were installed against 209,344 faulting leaves, 5.3
 * per fault**, while `on_guest_invept` discards the whole root **0.74
 * times a round trip**. An eagerly installed leaf has less than two round
 * trips to be used before it is thrown away, and most are not. A smaller
 * window installs fewer useless leaves and saves proportionally fewer
 * faults; a larger one is worse. The INVEPT rate is the ceiling, and it
 * is the guest's.
 *
 * That is the same INVEPT behaviour that closed the shadow-refresh lever,
 * arriving from the other side.
 */
inline constexpr bool eager_ept_neighbours =
#if defined(ZPP_EAGER_EPT_NEIGHBOURS) && ZPP_EAGER_EPT_NEIGHBOURS
    true;
#else
    false;
#endif

/**
 * Pages installed per fault, including the faulting one. Aligned, so the
 * set is the same whichever page of it faults first.
 */
inline constexpr std::uint64_t eager_ept_window = 8;

inline constexpr bool force_dispatch_once =
#if defined(ZPP_FORCE_DISPATCH_ONCE) && ZPP_FORCE_DISPATCH_ONCE
    true;
#else
    false;
#endif

inline constexpr bool self_ipi_delivery =
    deliver_self_ipi || intercept_self_ipi || force_dispatch_once;

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
 * Whether the exit ring records the three fields that cost a VMCS read.
 *
 * `record_exit` runs once on every exit and read four fields for the
 * ring: the exit qualification, the guest activity state, the guest CS
 * selector and the instruction pointer. The census over our own reads
 * put those four at 6.1, 7.9, 9.1 and 18.6 per round trip respectively -
 * and the ring is one reader of each. A VMREAD is an exit to the layer
 * below at 1.4-1.8 microseconds here, because the host this VMM runs
 * under offers no VMCS shadowing: `enable_shadow_vmcs` reads `N`, and
 * 1,000 reads of a shadow-listed field timed at 2,687 cycles against
 * 2,801 for a field that is not listed - identical.
 *
 * Off by default, and what that costs is bounded on purpose:
 *
 * - The ring keeps the **reason**, the **instruction pointer**, the
 *   **guest-physical address** on the two extended-page-table reasons,
 *   and the register detail for RDMSR, WRMSR and VMCALL. None of those
 *   costs a read that the exit had not already taken.
 * - `qualification`, `activity_state` and `cs_selector` read **zero**,
 *   and zero is a legal value for all three. Nothing in the ring can
 *   tell you the switch was off; the build manifest can, and
 *   `check-bootable.sh` prints it on every deploy. That is the trade.
 * - `cpl_seen` is empty, so the "did the guest ever reach ring 3"
 *   question needs this on.
 * - **The terminal records are untouched.** `unhandled_exit` and
 *   `vm_entry_failure` still capture everything, because they run once
 *   and then the processor stops - they are the records a failure is
 *   actually read from, and they cost nothing per exit.
 *
 * On, when the question is what the guest was doing rather than how much
 * it cost.
 */
#ifndef ZPP_CENSUS_EXITS
#define ZPP_CENSUS_EXITS 0
#endif

inline constexpr bool census_exits = (0 != ZPP_CENSUS_EXITS);

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
 * Apply a start-up IPI that arrived before its target reached its INIT
 * exit, rather than only holding it.
 *
 * On, which is how the multicore boot got past its first wall. It is
 * also what starts a processor twice - the exit ring shows two `init`
 * exits carrying `cs=0x8700` then `cs=0x0200`, vector 0x87 applied and
 * then vector 0x02 over it - and hardware ignores a start-up IPI to a
 * processor that is already running, where this does not.
 *
 * Off is the experiment that says whether the queued application is load
 * bearing or whether the hardware start-up IPI behind it would have
 * started the processor anyway.
 */
#ifndef ZPP_APPLY_QUEUED_START_UP
#define ZPP_APPLY_QUEUED_START_UP 1
#endif

#ifndef ZPP_HONEST_EXIT_LENGTH
#define ZPP_HONEST_EXIT_LENGTH 1
#endif

// Whether a reflected exit carries an instruction length only where
// SDM 30.2.5 defines one.
//
// Measured, one boot, two processors: of 91,841 reflections, **3,193
// were for a reason the SDM leaves the field undefined, and all 3,193
// reported a non-zero length** - reason 0x7, an interrupt window, with
// length 7, and reason 0x30, an EPT violation, with length 5. Neither
// exit is caused by an instruction, so the number is whatever a
// previous exit left in vmcs02, and a guest hypervisor that advances a
// RIP by it lands at an address nothing chose.
//
// The worst case is `start_up_ipi`, where the second-level guest never
// executed at all and the field is a previous exit's length outright.
//
// Off restores the old behaviour - forward the field whatever the
// reason - so the two are comparable. Hardware leaves the field
// undefined rather than zero for these reasons, so zeroing it is not
// strictly what a processor does; it is the one value that cannot be
// mistaken for a length, and a guest that uses it is then wrong in a
// way that shows rather than one that drifts.
inline constexpr bool honest_exit_length =
    (0 != ZPP_HONEST_EXIT_LENGTH);

#ifndef ZPP_L2_STARTUP_SPIN
#define ZPP_L2_STARTUP_SPIN 1
#endif

// Whether `wait_for_l2_start_up_ipi` spins before giving up its pass.
//
// **The spin can prevent its own release.** Two things end that park
// and only one is an IPI. The other - and the one Hyper-V uses, since
// it virtualises its own guest's local APIC and never writes ours - is
// vmcs12's activity state going back to active, which
// `enter_or_park_l2` re-reads on every pass. That write is made by the
// *guest hypervisor*, which cannot run while this processor spins in
// root operation, so every iteration is time the release cannot
// happen.
//
// Measured at 200,000 iterations, two processors: the application
// processor took about six exits a second - one per pass - while its
// host thread burned more system time than the boot processor's. That
// second half is L0's pause-loop exiting: a PAUSE in non-root trips
// `ple_window` into `kvm_vcpu_on_spin`, which is host kernel work
// charged to stime and invisible to this VMM's own exit count.
//
// Nothing is lost by not waiting: the mailbox holds a value rather
// than an edge and stays published, so a vector deposited meanwhile is
// found by the compare-exchange on the next pass. Only hand-off
// latency changes, by one VMLAUNCH round trip.
inline constexpr bool l2_startup_spin = (0 != ZPP_L2_STARTUP_SPIN);

#ifndef ZPP_STEP_AP_WATCHED_WRITES
#define ZPP_STEP_AP_WATCHED_WRITES 0
#endif

// Settles whether the write *emulation* is what kills a processor
// other than the first, in one comparison rather than field by field.
// Every field checks out - thirteen writes decoded, every applied
// offset equal to the decoded one and all of them an ordinary
// bring-up set, no instruction-length disagreement, none refused,
// none stepped, and the host in xAPIC mode so the store reaches the
// device - and the processor still dies with the watch armed and
// lives with it dropped.
//
// On, those processors take the monitor-trap fallback instead: the
// page is opened, the guest executes its own instruction, and the
// watch is restored. Survival then means the emulation was at fault;
// death means it was not, and neither answer needs another field.
inline constexpr bool step_ap_watched_writes =
    (0 != ZPP_STEP_AP_WATCHED_WRITES);

#ifndef ZPP_DROP_WATCH_ON_START_UP
#define ZPP_DROP_WATCH_ON_START_UP 0
#endif

// The local APIC page watch is how an application processor is adopted
// at all - measured, with ZPP_INTERCEPT_APIC=OFF the processor takes
// zero exits and never runs. But it is only needed *until* a processor
// has been adopted: after that its INIT arrives as a plain exit, which
// the exit ring shows directly.
//
// Left armed, it costs the newly started processor an exit for each of
// its own ordinary bring-up writes - logical destination, destination
// format, spurious vector - none of which can carry a start-up IPI. Its
// descriptor table's mapping is then torn down four exits after it is
// installed, while still in use, by the guest and not by this VMM.
//
// So drop it when a start-up is applied rather than after
// `apic_watch_quiet_ticks`, which is about two minutes at the rig's
// clock and therefore always still armed at that moment.
inline constexpr bool drop_watch_on_start_up =
    (0 != ZPP_DROP_WATCH_ON_START_UP);

#ifndef ZPP_WATCH_AP_PAGE_TABLE
#define ZPP_WATCH_AP_PAGE_TABLE 0
#endif

// Names whoever zeroes the page table entry that maps an application
// processor's descriptor table. The entry is installed, four exits pass,
// and it is cleared while the table is still in use - and a whole-handler
// bracket proves this VMM never writes it, so the writer is the guest.
// Which processor of the guest is the question this answers, because
// "the processor unmapped its own live descriptor table" and "the other
// one unmapped it underneath" want opposite fixes.
//
// Off by default: it takes write permission from a live page table page,
// so a run with it on is not comparable with one without.
inline constexpr bool watch_ap_page_table =
    (0 != ZPP_WATCH_AP_PAGE_TABLE);

inline constexpr bool apply_queued_start_up =
    (0 != ZPP_APPLY_QUEUED_START_UP);

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
 * Whether the firmware's linear framebuffer is located by the loader and
 * recorded here.
 *
 * Nothing in this VMM ever draws. The record exists so that something
 * *outside* the machine can read the screen, which on the passthrough rig
 * is the only way to see one: the display is a passed-through GPU, so
 * QEMU answers `screendump` with "There is no console to take a
 * screendump from", and the monitor's `xp` over the base recorded here is
 * the whole capability. `scripts/rig-screen.py` is the reader.
 *
 * Off, every field stays zero - which is exactly what a machine reporting
 * no graphics output protocol produces, so off is a state the reader
 * already handles rather than a new one.
 *
 * Living in this header rather than beside the graphics code is a
 * deliberate compromise: the header is where every `constexpr bool` the
 * build manifest is assembled from lives, and a switch the manifest
 * cannot show is a switch this project has already lost a session to.
 */
#ifndef ZPP_FRAMEBUFFER
#define ZPP_FRAMEBUFFER 1
#endif

inline constexpr bool framebuffer_recorded = (0 != ZPP_FRAMEBUFFER);

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
 * Whether the guest's own local APIC is watched - the page in xAPIC
 * mode, the interrupt command MSR in x2APIC mode.
 *
 * Not a nested-VMX switch by subject, and here anyway, because this is
 * the header the build manifest is assembled from and **a switch the
 * manifest cannot see is a switch that costs a session**. This one
 * proved it: `build/debug` held `ZPP_INTERCEPT_APIC:BOOL=OFF` from an
 * experiment recorded in `BACKLOG.md` - "Not the local APIC watch" -
 * while release and nested held ON, and every deploy inherited the
 * stale entry silently. Nothing on the path to the rig could show it,
 * because `zpp_build_switches` had no field for it.
 *
 * What that costs when it is off is not a slower guest, it is a wrong
 * one: the watch is what catches a start-up IPI and replaces it with
 * one naming this VMM's own trampoline. Off, the IPI reaches hardware
 * and the processor it starts runs **outside** this VMM. Measured on
 * an eight-processor guest with the stale cache: CPU 1 parked in
 * `hvix64.exe+0x248146` with zero exits and zero second-level entries
 * against this VMM, every `ipi_*` counter zero, `watched_apic_page`
 * zero, and the APIC page still an unsplit read-write-execute 2 MB
 * extended-page-table entry.
 *
 * So the experiment it exists for is only valid with one processor,
 * which is what `BACKLOG.md` says and what the eight-processor run
 * violated.
 */
#ifndef ZPP_INTERCEPT_APIC
#define ZPP_INTERCEPT_APIC 1
#endif

inline constexpr bool intercept_apic = (0 != ZPP_INTERCEPT_APIC);

/**
 * Whether to drop the local APIC page watch once no more processors are
 * going to start.
 *
 * `hypervisor::all_processors_started` was declared for exactly this and
 * says so - "after which the interception is switched off - inter-processor
 * interrupts are hot on a running system and there is no reason to keep
 * paying for them once no more processors are going to start" - and was
 * then never set and never read anywhere in `hypervisor/src`. This is that
 * mechanism, wired.
 *
 * What it is worth, measured on the settled eight-processor guest: the
 * write-protection on `0xfee00000` costs 1,586 extended-page-table
 * violations a second, 23.2% of every exit CPU 0 takes. None of them are
 * nested - `l2_ept_dispositions` reports `watched` zero and the faulting
 * instruction pointers are inside hvix64 - so this is the guest
 * hypervisor's own end-of-interrupt traffic, paid for a start-up IPI that
 * stopped coming once the seventh application processor was up.
 *
 * The round trip costs 402 microseconds against a 1.74 ms tick and the
 * guest manages 4.65 of them per tick, so it overruns by about 7.5%. This
 * is the cheapest lever that is anywhere near that margin.
 *
 * **Off by default, and the reason is not cost.** The watch is what turns a
 * start-up IPI into one naming this VMM's own trampoline. A processor
 * started after the watch is dropped runs *outside* this VMM - which is
 * precisely the failure `BACKLOG.md` records under "The switch below leaked
 * out of its experiment", where seven of eight processors were lost that
 * way. Dropping it is safe only if no further processor starts, and nothing
 * here can prove that; the quiescence delay below is a heuristic, not a
 * proof.
 *
 * What would let this be on by default: an APIC-access page with
 * APIC-register virtualization, where SDM 32.4.3.2 has INIT and SIPI always
 * take the trap-like APIC-write exit while ordinary traffic stops exiting.
 * Then the interception gets cheaper *and* stronger and no heuristic is
 * needed.
 *
 * **It also used to stop the guest dead on the very first fault after the
 * drop, and that is fixed.** The disarm runs at the top of
 * `on_ept_violation` and then fell through into the loop over `watches`,
 * which - the watch having just been dropped - found nothing, so
 * `on_ept_violation` answered `false` and both of its callers stop the
 * processor for it. Three runs died that way, always at local APIC offset
 * `0x380`. It now returns and lets the guest re-execute its own
 * instruction. `BACKLOG.md` carries the record and the wrong diagnosis it
 * was first given. Nothing about the cost has been re-measured since, so
 * this is still an unanswered experiment rather than a rejected one.
 */
#ifndef ZPP_DISARM_APIC_WATCH
#define ZPP_DISARM_APIC_WATCH 0
#endif

inline constexpr bool disarm_apic_watch = (0 != ZPP_DISARM_APIC_WATCH);

/**
 * Withhold a clock interrupt that arrives sooner than this many
 * microseconds after the last one that was delivered. Zero is off.
 *
 * **What it is for.** Measured on the rig, the second-level guest
 * returns from its clock handler to one instruction, finds the next
 * clock already pending, and takes it before that instruction retires -
 * 680,862 times, always the same address, two instructions short of the
 * `call` that would drain its deferred-procedure-call queue. It has
 * therefore never run one. Giving it a gap is the only intervention
 * left that addresses the mechanism rather than its cost.
 *
 * **Why this is not the four time lies that already failed.**
 * `ZPP_STRETCH_GUEST_TIMER`, `ZPP_TICK_FLOOR` and `ZPP_TIME_DILATION`
 * all alter what the guest is told the *time* is, and Windows checks its
 * clocks against each other - `HalpWatchdogCheckPreResetNMI`, bugcheck
 * `0x1CA`. This alters *delivery* instead. The reference counter stays
 * truthful and a masked interrupt is something real hardware produces
 * routinely.
 *
 * **Why it is expected to fail, recorded before running it.** KVM's
 * equivalent - the lazy lost-ticks policy at `hyperv.c:812-830` - drops
 * a periodic expiry at the **source**, before the message is committed,
 * and declines to re-arm a timer whose message the guest has not
 * consumed. This drops at the **sink**: the level above has already
 * written its message and set the synthetic interrupt source, so it will
 * believe the interrupt was injected and will not re-stage it, and the
 * guest will never acknowledge a tick it never took. Both earlier
 * experiments that perturbed this same field ended with the level above
 * halting. **A failure here is expected and is still worth one boot,
 * because the alternative is to stop having any mechanism to test.**
 *
 * The failure signature is known and takes ninety seconds: second-level
 * entries frozen with a last exit of `hlt` at the first-level
 * instruction pointer. Success is `l2_injected_vector[0x2f]` climbing
 * and `leaves-filled` moving.
 */
#ifndef ZPP_LAZY_TICK
#define ZPP_LAZY_TICK 0
#endif

inline constexpr std::uint64_t lazy_tick_microseconds = ZPP_LAZY_TICK;

/**
 * A floor under the second-level guest's periodic synthetic timer, in
 * 100 ns units. Zero is off. See the site in `nested_entry.cpp` for what
 * it does and what it costs; this is here rather than beside that site
 * so `build_switches.cpp` can print it.
 *
 * **It has to be printable.** It is the switch that took this boot from
 * stalling at `VBoxSup.sys` to reaching ring 3, so every measurement now
 * depends on which value was compiled - and the manifest could not say.
 * That is the exact shape of the `ZPP_PUBLISH_REFERENCE_TSC` failure the
 * manifest exists to prevent, and it survived in the one switch that
 * mattered most because the switch is a *value* and the manifest only
 * had digits for booleans.
 */
#ifndef ZPP_TICK_FLOOR
#define ZPP_TICK_FLOOR 0
#endif

inline constexpr std::uint64_t tick_floor_units = ZPP_TICK_FLOOR;

/**
 * Time-stamp ticks in a microsecond on the part this runs on, measured
 * at the wall as 1,992,000,000 Hz and agreeing with CPUID.15H's 24 MHz
 * crystal times 83. Not derived from a leaf that reads zero here.
 */
inline constexpr std::uint64_t ticks_per_microsecond = 1992;

/**
 * How long after the last start-up IPI the watch is considered to have
 * done its job, in time-stamp counter ticks.
 *
 * Two hundred and forty thousand million, which is about two minutes on the
 * 2 GHz part this runs on. Deliberately not derived from a measured
 * frequency: a wrong frequency would silently make the delay zero and drop
 * the watch during bring-up.
 *
 * **Five seconds was tried first and wedged the guest outright.** The gaps
 * between this guest's start-up IPIs are far longer than five seconds -
 * measured armed, the count goes 1 INIT and 2 start-up IPIs early, then
 * reaches 15 and 16 by ninety seconds, then never moves again. So a
 * five-second silence fired after the *first* application processor and the
 * remaining six were started unwatched. What that produced is worth
 * recording, because it is not the failure that was expected: all eight
 * processors still reached this VMM and took about 107 exits each, but the
 * seven application processors never entered the second level at all, and
 * CPU 0 stopped dead - 28,736 exits and 2,741 shadow leaves, both unchanged
 * across ninety seconds. Not a livelock. A stop.
 *
 * Two minutes clears the ninety-second bring-up with margin. It is still a
 * heuristic and can still be wrong on a slower boot, which is the whole
 * reason the switch is off by default.
 */
inline constexpr std::uint64_t apic_watch_quiet_ticks = 240'000'000'000ull;

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
/**
 * Offer virtual-interrupt delivery to the guest hypervisor.
 *
 * **Off, and it is the best-evidenced remaining candidate for the boot
 * this tree cannot finish.** The measured deadlock is: the guest sits at
 * DISPATCH_LEVEL inside its deferred-procedure-call dispatcher; one of
 * those calls enters VTL1; vector `0x2f` is pending and *undeliverable*,
 * because delivery needs a priority class strictly greater than the task
 * priority's and both are class 2; the guest hypervisor sees an interrupt
 * pending for VTL0 and asserts VINA; `ShvlVinaHandler` yields having done
 * nothing; the dispatcher retries unchanged.
 *
 * With virtual-interrupt delivery the processor evaluates the request
 * against the task priority itself and delivers the instant it falls
 * below class 2 - no exit, and no involvement from the level above. The
 * fifth link, a hypervisor asserting VINA for an interrupt it cannot
 * itself deliver, does not arise.
 *
 * **Nothing is being refused today**, which is why this is a feature and
 * not a fix to an existing hole: `vmcs12_secondary_asked` reads with bits
 * 0, 4, 8 and 9 clear and `vmcs12_pin_asked` is `0x3f`. The guest
 * hypervisor read the capability MSRs, found these absent, and adapted to
 * the polling path - which is the interrupt-window storm measured at 36
 * exits for every one vector injected.
 *
 * Bits 8 and 9 together, as KVM offers them
 * (`.references/kvm/nested.c:7051-7066`). SDM 25.6.2 requires only that
 * "use TPR shadow" be 1 for either, and that is already honoured for
 * every vmcs12 - `tpr_shadow_refused` and `tpr_shadow_absent` both read
 * zero. Bit 0, virtualize-APIC-accesses, is deliberately **not** offered:
 * it needs an APIC-access address this VMM never writes, and offering a
 * control whose backing field is absent is the exact failure this project
 * keeps making.
 *
 * What has to work for it to be sound, all of it in `build_vmcs02` and
 * `reflect_l2_exit`: the guest interrupt status carried into vmcs02 and
 * back out to vmcs12, the four EOI-exit bitmaps passed through, and the
 * two exits it makes possible - `virtualized_eoi` and `apic_write` -
 * reflected rather than reaching `default:` and stopping the processor.
 */
#ifndef ZPP_NESTED_VID
#define ZPP_NESTED_VID 0
#endif

/**
 * Clear the virtual interrupt notification flag on the entry that runs
 * VTL1, so the secure kernel does its work instead of yielding.
 *
 * **This is a lie, and it is the narrowest one that addresses the
 * measured deadlock.** The secure kernel spins in `ShvlVinaHandler`,
 * which loops until VINA is clear. VINA is set because vector `0x2f` is
 * pending for VTL0 and deliverable - measured, 95.6% of returns whose
 * call was made at priority class 0, against 6.2% at class 2 where the
 * guest's own priority blocks that vector. The level above is right to
 * set it. The guest is right to call in. Nothing is broken.
 *
 * What is wrong is only the *rate*: for VINA to be clear, VTL0 must
 * re-enter VTL1 inside the 1.74 ms before the clock re-requests `0x2f`,
 * and its half of the round trip is 11,357 us across 95.6 exits - about
 * seven ticks wide - because every synthetic MSR write in the clock path
 * costs two exits and every exit costs 58 exits into KVM underneath us.
 * On a host with VMCS shadowing that half would fit and this flag would
 * be unnecessary.
 *
 * So the flag makes the outcome that faster hardware would produce: the
 * secure kernel runs its 1,456 us and finishes instead of yielding
 * having done nothing. The cost is that VTL0's deferred procedure call
 * waits that long, which Windows tolerates by design - deferred calls
 * have no deadline.
 *
 * **What would make it wrong.** VINA is VTL0's signal that it wants the
 * processor back; suppressing it indefinitely would starve VTL0. This
 * clears it only on the entry that runs VTL1, so the level above is free
 * to set it again the moment VTL1 returns, and every other use of the
 * flag is untouched. If the secure kernel has a second reason to consult
 * it that this has not seen, this will find it - which is why it is a
 * switch and not a change.
 */
#ifndef ZPP_SUPPRESS_VINA
#define ZPP_SUPPRESS_VINA 0
#endif

inline constexpr bool suppress_vina = (0 != ZPP_SUPPRESS_VINA);

inline constexpr bool virtual_interrupt_delivery_offered =
    (0 != ZPP_NESTED_VID);

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

    // APIC-register virtualization and virtual-interrupt delivery, SDM
    // Table 25-7 bits 8 and 9. See `virtual_interrupt_delivery_offered`
    // for why, and for why bit 0 is not offered beside them.
    (virtual_interrupt_delivery_offered ? ((1ull << 8) | (1ull << 9))
                                        : 0ull) |
    (1ull << 20);                 // Enable XSAVES/XRSTORS.

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
    // **WITHDRAWN 2026-08-22, and the justification above is wrong in
    // two places.**
    //
    // "The composition already handles it" is true and irrelevant:
    // `compose_ept` carries `execute_user` correctly, but **nothing that
    // decides what a fault MEANS ever reads it.** The disposition in
    // `nested_entry.cpp` is made by one lambda that tests
    // `permissions.execute()` - bit 2, supervisor execute - and never
    // `execute_user()`, bit 10. With this control on, a user-mode fetch
    // is governed by bit 10, so both directions are wrong: a user-mode
    // fetch of a user-executable page is reflected to the guest
    // hypervisor for an access its own tables permit, and a user-mode
    // fetch of a supervisor-only page is quietly satisfied. Either way
    // the guest resumes onto the identical fault for ever.
    //
    // "KVM alone... does advertise this bit" is **false**. KVM masks
    // `secondary_ctls_high` to an allow-list that omits
    // `SECONDARY_EXEC_MODE_BASED_EPT_EXEC`
    // (`.references/kvm/nested.c`, `nested_vmx_setup_ctls_msrs`); its only
    // mention of the control in the nested path is a consistency check at
    // `nested.c:890`. Its nested walker is three-bit
    // (`paging_tmpl.h:179-186`). So the baseline this was measured
    // against never had the bit, and the comparison did not test what it
    // said it tested.
    //
    // **And the instruments that should have caught it are blind by
    // construction.** `shadow_ept_leaves_that_did_not_help` re-uses the
    // same `permits()` lambda, so it reads zero while this happens; and
    // both leaf-permission histograms in `nested_ept.cpp` mask
    // `permissions.bits() & 7` while `bits()` places `execute_user` at
    // **bit 10**, so the "eptp12 alone against composed" table cannot see
    // this bit at all. A whole session's worth of "the composition is
    // exact" readings were taken through that mask.
    //
    // Re-advertising needs three things first: `permits()` and the
    // reflected qualification taught about bit 10 and the faulting
    // privilege level, the qualification's bit 6 stopped being hard-coded
    // false at both call sites, and the histograms widened past `& 7`.
    // 0;

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

/**
 * The fields the guest hypervisor may read and write against the hardware
 * shadow region **without exiting**.
 *
 * Lives here, rather than beside its use in `nested_shadow_vmcs.cpp`,
 * because a second list has to be kept disjoint from it and the two were
 * in different translation units with nothing tying them together.
 * `guest_state_deferrable` excludes exactly the entries this list and
 * `guest_state_fields` share - by hand, and correctly today. Sharing the
 * definition lets that be a `static_assert` instead of a comment.
 *
 * **Why the two must not overlap.** A deferred guest-state field is
 * written to vmcs02 lazily and materialised when somebody reads it. A
 * shadowed field is read by the guest hypervisor straight out of the
 * shadow region with no exit - so there is no read to materialise on, and
 * a deferred value would be handed over stale, invisibly, with no fault
 * and no counter moving.
 *
 * `field::guest_cr3` is the live hazard: it is in `guest_state_fields`,
 * it is deferrable, and **KVM shadows `GUEST_CR3`**
 * (`.references/kvm/vmcs_shadow_fields.h:65`). Adding it here - the
 * obvious optimisation, and the one KVM sanctions - would make a guest
 * hypervisor read a stale second-level CR3. The assert is what stops
 * that being found the hard way.
 */
inline constexpr arch::x86_64::vmx::vmcs_fields::vmcs_field
    shadow_read_write_fields[] = {
        // Measured, and it was not on KVM's list. With everything else
        // here shadowed, Hyper-V's remaining VMREAD traffic was 45,866
        // reads of which 45,866 were DR7 and six were anything else -
        // one per exit it handles, from its own exit path. KVM's
        // vmcs_shadow_fields.h does not shadow it, which is the whole
        // argument for measuring the guest in front of you rather than
        // copying another VMM's list.
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_dr7,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_rip,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_rflags,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_interruptibility_state,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::
            vm_entry_interruption_information_field,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::
            primary_processor_based_vm_execution_controls,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::tpr_threshold,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_cs_access_rights,
        arch::x86_64::vmx::vmcs_fields::vmcs_field::guest_ss_access_rights,
};

/**
 * Watch the IUM context block for writes, and log who makes them.
 *
 * **The value being unchanged and the value being un-writable look
 * identical from outside.** The second-level guest loops on a state byte
 * that never moves, and polling it - twelve reads over two minutes, all
 * `0x0000000100000400` - cannot distinguish "nothing writes it" from "a
 * write is attempted and lost". Those want opposite fixes, and only a
 * memory breakpoint separates them.
 *
 * The block's address is not known until the guest is deep in boot and
 * changes every run with KASLR, so it cannot be baked in. It is
 * discovered instead: `capture_vtl_switch` already holds `rdx` at the
 * `HvCallVtlCall`, which is the block, and translates second-level
 * addresses for its code window - so the watch is armed from there, once,
 * on the page that address lands in.
 *
 * Watching in *this* VMM's extended page tables reaches a second-level
 * write because the shadow composes our permissions with the guest
 * hypervisor's - measured bucket-for-bucket in `install_shadow_leaf` -
 * so clearing write here removes it from the composition too.
 *
 * Off by default: it costs an EPT violation on every write to a live
 * kernel stack page, which is not a page a deployed build should be
 * faulting on.
 */
#ifndef ZPP_WATCH_VTL_BLOCK
#define ZPP_WATCH_VTL_BLOCK 0
#endif

#ifndef ZPP_TRACE_VTL
#define ZPP_TRACE_VTL 0
#endif

/**
 * Record Hyper-V's virtual trust level switches - the `HvCallVtlCall` and
 * `HvCallVtlReturn` hypercalls, the register state either side of each,
 * and the guest stack above the caller.
 *
 * **Off by default, and observational only.** This VMM is not specific to
 * any guest hypervisor, and virtual trust levels are one guest
 * hypervisor's interface; nothing here may be on the path of a guest that
 * has never heard of them. Everything behind this flag reads guest state
 * and writes only this VMM's own members - it decides nothing, injects
 * nothing and answers no hypercall, so a build with it off and a build
 * with it on present the *same* machine to the guest and differ only in
 * what this VMM knows about it.
 *
 * It is not free, which is the other reason it is a flag: several VMCS
 * reads and a guest page walk on every trust-level switch.
 */
#ifndef ZPP_EVMCS_TO_KVM
#define ZPP_EVMCS_TO_KVM 0
#endif

/**
 * Use Hyper-V's enlightened VMCS toward the layer *below* this VMM.
 *
 * Every VMREAD and VMWRITE executed here is an exit to that layer when it
 * will not give us a shadow VMCS - measured at about 4,600 cycles against
 * some thirty-six accesses an exit, which is essentially the whole cost of
 * an exit, and that cost is what pins the guest at its highest interrupt
 * priority. The enlightened VMCS replaces those instructions with loads
 * and stores to a page shared with the layer below, so the cost is not
 * reduced, it stops being paid.
 *
 * **Off by default, detected at run time, and never advertised upward.**
 * This is a contract with whatever is underneath, and this VMM is not
 * specific to anything underneath - so it is used only where the layer
 * below says it offers it, and the guest hypervisor above is told nothing:
 * the whole hypervisor CPUID range is answered here rather than forwarded,
 * so it goes on believing it is on bare metal.
 *
 * **It cannot be combined with offering VMCS shadowing to the guest.** The
 * enlightened layout has no home for the VMREAD and VMWRITE bitmap
 * pointers; KVM refuses the same combination.
 */
inline constexpr bool evmcs_to_kvm = (0 != ZPP_EVMCS_TO_KVM);

#ifndef ZPP_EVMCS_MIXED
#define ZPP_EVMCS_MIXED 0
#endif

/**
 * Enlighten **only** the second-level VMCS, and keep offering the guest
 * hypervisor VMCS shadowing.
 *
 * The two savings look mutually exclusive and are not. Shadowing needs
 * the VMREAD and VMWRITE bitmap pointers, which the enlightened layout
 * has no home for - but only *vmcs01* carries those, because shadowing
 * is a control on the VMCS that runs the guest hypervisor. vmcs02 can
 * live in an enlightened page while vmcs01 stays a real region.
 *
 * **The measurement that says it is worth it**, one processor, 431 s
 * each, against the same guest:
 *
 * | | shadowing only | enlightened only |
 * |---|---|---|
 * | this VMM's duty | 0.771 | 0.371 |
 * | guest hypervisor | 15.4% | 55.4% |
 * | Windows | 7.6% | 7.6% |
 *
 * Each frees one side's VMCS accesses and pays the other's, and the
 * guest gets 7.6% of the machine either way - which is what pins it at
 * `CLOCK_LEVEL` and stops the boot. Mixed mode is the only configuration
 * in which neither side pays.
 *
 * **Off by default**: five earlier attempts each reached exactly one
 * second-level entry. What they were missing is recorded on
 * `evmcs_own_flushed` - the layer below discards its cached vmcs01
 * un-flushed on every enlightened entry, so the state loss outlives any
 * fix aimed at the launch state alone.
 */
#ifndef ZPP_STALL_BREAKER
#define ZPP_STALL_BREAKER 0
#endif

/**
 * Refuse to deliver the same vector twice at the same guest instruction.
 *
 * **The measurement this exists for.** Two histograms of the guest's
 * instruction pointer - one sampled where the entry stages an event, one
 * where it stages nothing - disagree completely. The control is flat
 * across 192 addresses with no peak above 3.3%; **84% of injections land
 * on two instructions**, and the first of them is the instruction
 * immediately after a `sti`:
 *
 * ```
 *         movl  $2, %ecx
 *         movq  %rcx, %cr8      ; lower to DISPATCH_LEVEL
 *         sti
 * +0x12:  movq  -87(%rbp), %rcx   <- 45.6% of all injections
 * ```
 *
 * The guest enables interrupts, takes one, runs the handler, returns to
 * the same instruction and takes another - for ever, never retiring it.
 * The second address is the first instruction of the `HvCallVtlReturn`
 * stub, which is the same shape at the other point the guest becomes
 * deliverable-to.
 *
 * **Why withholding loses nothing here.** The source is level-asserted:
 * the timer message sits unconsumed in the guest's own message page -
 * read directly, slot 3 holds `0x80000010`, `HvMessageTimerExpired` -
 * so the level above re-asserts as soon as it is allowed to. Declining
 * one delivery defers it, it does not drop it. `suppress_vina` already
 * establishes the mechanism: clear the valid bit in vmcs02 before entry.
 *
 * **The rule is forward progress, not a rate.** Deliver when the guest's
 * instruction pointer has moved since the last delivery of that vector,
 * withhold while it has not. That guarantees at least one retired
 * instruction between two deliveries of the same vector and cannot slow
 * a guest that is making progress, because a guest making progress never
 * meets the condition.
 *
 * **Not a lie about time**, which is what separates it from the four
 * interventions in `BACKLOG.md` that failed. Those changed the period,
 * floored it, multiplied it or dilated every clock together, and Windows
 * cross-checks its clocks. This changes nothing the guest can observe
 * about time; it changes only whether an interrupt is delivered at an
 * instruction boundary the guest has already been interrupted at.
 *
 * Capped, because a guest legitimately spinning on one instruction - a
 * string operation, a lock - must not be starved of interrupts for ever.
 */
inline constexpr bool stall_breaker = (0 != ZPP_STALL_BREAKER);

/** Consecutive withholds at one instruction before one is forced. */
inline constexpr std::uint64_t stall_breaker_limit = 256;

inline constexpr bool evmcs_mixed =
    evmcs_to_kvm && (0 != ZPP_EVMCS_MIXED);

#ifndef ZPP_WINDOW_ON_TPR
#define ZPP_WINDOW_ON_TPR 0
#endif

/**
 * Arm the guest hypervisor's interrupt window on a task-priority drop
 * rather than on every interruptible moment.
 *
 * Interrupt-window exiting fires whenever the guest could take *an*
 * interrupt - RFLAGS.IF set, no blocking - and says nothing about the
 * task priority. A hypervisor holding a vector the guest's priority
 * blocks therefore gets woken constantly and can deliver nothing, which
 * is what this VMM measures: **3,055,183 window requests** with
 * `int_window_stale` at zero, so the requests are genuine, while the
 * guest sits at a priority that blocks the dispatch vector 75% of the
 * time.
 *
 * Worse than wasteful: the wakeup it gets is uncorrelated with the event
 * it needs, so the moment the priority *does* drop is only noticed if an
 * interruptible moment happens to coincide.
 *
 * On, the window is withheld while the priority blocks the vector and the
 * TPR threshold is armed instead, so the processor reports the drop
 * itself and the window is given at exactly that moment.
 *
 * **Off by default, because it is a behaviour change and not an
 * optimisation.** A vector whose priority the guest would have admitted
 * is delayed until the next drop, and only the level above knows which
 * vector it holds.
 */
inline constexpr bool window_on_tpr = (0 != ZPP_WINDOW_ON_TPR);

#ifndef ZPP_DELIVER_ON_DROP
#define ZPP_DELIVER_ON_DROP 0
#endif

/**
 * Arm a TPR threshold where the level above left none, and **take
 * nothing away**.
 *
 * ### What this switch is not, and the two measurements that decided it
 *
 * It is not `window_on_tpr` repaired. The first version of it was, and
 * it was measured on the rig and was worse. One processor, one boot,
 * against the same binary with the switch off:
 *
 *     switch                0x2f     0xd1      all vectors   guest asks
 *     both off              9,627    388,241   404,029       411,669
 *     first deliver_on_drop    11      5,550     5,938            12
 *
 * The dispatch vector did not move and **the clock collapsed with it**,
 * 388,241 to 5,550. So the harm is not specific to the vector being
 * chased: withholding the interrupt window stops delivery of
 * everything. The guest then does almost no work, which is why it asks
 * only twelve times - the ask count is an effect of the intervention,
 * not a measurement of the guest. Synthetic interrupt-command writes
 * fell from 1,452,927 to 12 for the same reason.
 *
 * That is now measured twice, since `window_on_tpr` did the same thing
 * for the same reason, and the conclusion is the one both runs support:
 * **in this VMM the interrupt window is the primary delivery mechanism
 * for every vector.** Its 1,500,914 exits are wasteful and they are
 * load bearing. `window withheld 460,323` was the harmful line, and no
 * repair to what happens *after* the withholding can pay for it.
 *
 * ### Why the TPR threshold delivers nothing, which is a separate defect
 *
 * The level above arms a threshold - it writes `tpr_threshold` on 11%
 * of its VMWRITEs - and the exit fires 33 times in 3.6 million. The
 * architecture says why, and it is not subtle. SDM 32.1.2, TPR
 * virtualization (`.references/sdm.txt:206566`):
 *
 *     IF "virtual-interrupt delivery" is 0
 *     THEN
 *       IF VTPR[7:4] < TPR threshold
 *       THEN cause VM exit due to TPR below threshold;
 *
 * **A threshold of zero can never fire**, because nothing is less than
 * zero. `l2_tpr_threshold_seen` histograms exactly this value and is
 * the check: all of it in bucket 0 settles the question outright.
 *
 * And the exit has only one trigger available in this configuration.
 * SDM 32.1.2 lists three operations that perform TPR virtualization -
 * MOV to CR8, a write to offset 080H on the APIC-access page, and
 * WRMSR with ECX = 808H - of which the second needs "virtualize APIC
 * accesses" (SDM 32.4) and the third needs "virtualize x2APIC mode"
 * (SDM 32.5, `.references/sdm.txt:207200`), neither of which this VMM
 * offers. VM entry is a fourth trigger and needs the first of those
 * too (SDM 29.7.7). So MOV to CR8 is the whole of it, and only where
 * the level above did not also ask for CR8-load exiting - SDM 32.3
 * treats specially only a MOV to CR8 "that does not fault or cause a
 * VM exit" (`.references/sdm.txt:206830`).
 *
 * ### What is on
 *
 * `primary` is untouched, so every entry carries exactly the controls
 * the default build would have given it, and `window_deferred_count`
 * must read zero. Added on top:
 *
 * - a threshold at the dispatch class on entries where the level above
 *   is holding something (it is asking for an interrupt window) that
 *   the guest's priority blocks, **and** it left the threshold at zero,
 *   **and** it set the TPR shadow. An impossible condition becomes a
 *   possible one; no exit that was happening stops happening.
 * - at that exit, the threshold is written back down (left standing it
 *   is a VM-entry consistency check, SDM 29.2.1.1,
 *   `.references/sdm.txt:202124`) and the interrupt window is ensured
 *   live in vmcs02 before the resume. Normally it is already there and
 *   `window_already_armed_at_drop` counts that; the write exists for
 *   the case where the level above cleared its request in between and
 *   would otherwise be woken by nothing at all.
 *
 * The class is the vector's own, which is the quantity KVM writes in
 * `vmx_update_cr8_intercept` (`.references/kvm/vmx.c:6728`,
 * `tpr_threshold = (irr == -1 || tpr < irr) ? 0 : irr` over an `irr`
 * already shifted down by four in `update_cr8_intercept`,
 * `.references/kvm/x86.c:10193`). The refusal to touch a threshold the
 * level above owns is KVM's too, at `.references/kvm/vmx.c:6723`.
 *
 * ### What would say it is working, and what would say it is not
 *
 * Working: `0x2f` rises from 9,627 while `0xd1` stays near 388,241 and
 * the total carried stays near 404,029, and `pending_vector_dropped`
 * falls. Not working, and the thing to check first: total carried
 * falling at all. This switch may not cost a single delivery, and if it
 * does, it is doing the thing both previous attempts did.
 *
 * **Off by default**, so an A/B against it is one variable. Independent
 * of `window_on_tpr` rather than a modifier of it; both on is refused
 * by a static assertion, since they drive the same state and one of
 * them withholds.
 */
inline constexpr bool deliver_on_drop = (0 != ZPP_DELIVER_ON_DROP);

static_assert(!(window_on_tpr && deliver_on_drop),
              "ZPP_WINDOW_ON_TPR and ZPP_DELIVER_ON_DROP drive the same "
              "per-processor state and must not both be on");

#ifndef ZPP_COUNT_DROPS
#define ZPP_COUNT_DROPS 0
#endif

/**
 * Account for the low-priority vector the guest asks for, from the ask
 * to the delivery, with a name for the failure that has no other one.
 *
 * Four numbers per processor, on one basis, so they can be subtracted
 * from each other:
 *
 * - **asked** - writes of the synthetic interrupt command register that
 *   name this processor and a vector below the dispatch class.
 * - **pending at entry** - entries into the second-level guest made
 *   while one of those is still outstanding.
 * - **delivered** - entries whose entry-interruption field carries it.
 * - **dropped** - entries made while it was outstanding, at a priority
 *   that admits it, with nothing staged **and** nothing armed that
 *   could cause an exit at which the level above might stage it: no
 *   interrupt window in vmcs02 and no TPR threshold of this VMM's.
 *
 * The last is the number this work exists to take to zero, and it is
 * the only one of the four that is a fault rather than a rate. Every
 * other counter in this tree observes where an event was *put*;
 * `dropped` observes a moment at which the machine had every reason to
 * deliver one and no mechanism left to.
 *
 * `asked` deliberately exceeds `delivered` by a large factor even when
 * nothing is wrong: a request already in the level above's interrupt
 * request register coalesces with the new one, which is what
 * `coalesced` counts, so the two are not meant to be equal.
 *
 * **Off by default and safe to turn on in both arms of an A/B**, since
 * it only counts. It is not free - one guest-memory read and one
 * VMREAD per second-level entry - which is why it is a switch from the
 * day it was written rather than from the day somebody notices, this
 * tree having already paid for that lesson once in `census_exits`.
 */
inline constexpr bool count_dropped_requests = (0 != ZPP_COUNT_DROPS);

inline constexpr bool trace_vtl = (0 != ZPP_TRACE_VTL);

/**
 * Whether a guest INVEPT discards the shadow on **every** processor.
 *
 * The shadow of the guest hypervisor's extended page tables is kept per
 * processor, and `on_guest_invept` discards only the slot belonging to
 * the processor that executed the instruction. INVEPT is not a
 * broadcast, so that is what the instruction itself does - but the guest
 * hypervisor is entitled to assume its own shootdown IPI carries the
 * rest, and our shadow is invisible to that IPI.
 *
 * Why it matters here: VTL1 revokes a page from VTL0 with
 * `HvCallModifyVtlProtectionMask` - measured at 39,323 calls on one boot
 * - and then invalidates. If another processor's shadow still grants
 * what was revoked, VTL0 can touch a page the secure kernel believes it
 * cannot, which is exactly the class of thing a secure kernel checks and
 * fail-fasts on. **Impossible with one processor**, which is the shape of
 * the failure being chased.
 *
 * Off by default because it is not free: propagation is a bump of the
 * global generation, so every processor discards every shadow root at its
 * next entry, and a rebuild is measured at about 374 microseconds.
 */
#ifndef ZPP_INVEPT_ALL_PROCESSORS
#define ZPP_INVEPT_ALL_PROCESSORS 0
#endif

inline constexpr bool invept_all_processors =
    (0 != ZPP_INVEPT_ALL_PROCESSORS);

inline constexpr bool watch_vtl_block = (0 != ZPP_WATCH_VTL_BLOCK);

#ifndef ZPP_TRACE_AP_ENTRY
#define ZPP_TRACE_AP_ENTRY 0
#endif

/**
 * Whether the **whole** guest state is written to the log at the moments
 * an application processor's life turns over: its first VM entry, every
 * application of the INIT-plus-start-up state, and any triple fault.
 *
 * Observational only - nothing here changes a VMCS field, an exit
 * decision or a control - so a run with it on is comparable with one
 * without, unlike most of the switches above. What it costs is log
 * lines, about a dozen per event, and the events are rare: one per
 * processor per start.
 *
 * ### Why it exists
 *
 * The two-processor failure this was written for is a triple fault on
 * the application processor with
 *
 *     cr0 0x30 cr3 0x0 cr4 0x2000 efer 0x0 rsp 0x5 ss 0x0
 *     rip 0x10000 cs 0x178
 *
 * The first line is, character for character, what `apply_start_up`
 * writes - `extension_type | numeric_error`, a zero CR3 and CR4 holding
 * nothing but VMXE - and nothing else in this tree writes that triple.
 * The second line cannot come from `apply_start_up` at all: it writes
 * `vector << 8` into CS, whose low byte is therefore always zero, and
 * `0x178` is not of that shape. So the state was applied and *then* the
 * guest ran, and the one thing the log could not say is **which
 * application it was** - the first, out of `main`, or a later one out of
 * an INIT and a start-up IPI, at a vector that may no longer hold the
 * trampoline the guest put there.
 *
 * That question is what the trace answers, and it answers it without a
 * debugger: the log survives the freeze, and `-no-reboot -no-shutdown`
 * leaves the guest frozen rather than reset.
 *
 * **Self-falsifying.** The boot processor prints `ap-entry instrument
 * armed` at its own launch, so the switch being compiled in is visible
 * even when no application processor is ever started, and every state
 * dump carries the count of application-processor first entries - a
 * count of zero at a triple fault says outright that no application
 * processor was ever entered, which is a different failure from the one
 * being chased and must not be read as it.
 *
 * Off by default: it is a diagnostic, and a diagnostic left on writes
 * into a ring that other investigations need.
 */
inline constexpr bool trace_ap_entry = (0 != ZPP_TRACE_AP_ENTRY);

} // namespace zpp::hypervisor::nested_vmx
