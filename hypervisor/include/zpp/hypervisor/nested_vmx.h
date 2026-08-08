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
 * (`guest_vmxon_count` zero), so the next question is what else virtualization
 * based security wants before it will.
 */
inline constexpr bool announce_hypervisor = false;

inline constexpr bool enabled =
#if defined(ZPP_NESTED_VMX) && ZPP_NESTED_VMX
    true;
#else
    false;
#endif

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
    (1ull << 21) | // Use TPR shadow - OFFERED, experiment in progress.
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
                   // Bit 21, use TPR shadow, was previously **not** offered, and
                   // that is the one thing standing between this VMM and
                   // a guest hypervisor.
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
    (1ull << 16) | // RDSEED exiting.
    (1ull << 20);  // Enable XSAVES/XRSTORS.

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
