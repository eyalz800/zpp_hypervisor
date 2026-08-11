#!/bin/sh
# Source-level invariants on the VM exit handler.
#
# CLAUDE.md states the first one as a rule rather than as a list:
# **nothing a guest can execute may reach `default:`**. Until 8412b76
# every one of the thirteen VMX instructions did, and the consequence was
# that a guest instruction could stop a physical processor - the handler
# does not resume from an unhandled exit, because the resume path would
# advance RIP past an instruction that never took effect.
#
# A test for the thirteen instances is worth having and exists, in the
# guest coverage suite. This is a test for the *rule*, which is worth
# more: it says a new exit reason cannot appear in the architecture, or an
# old case be deleted, without somebody noticing.
#
# What "can execute" means here is SDM 28.1.2, "Instructions That Cause VM
# Exits Unconditionally" (.references/sdm.txt:200699), plus VMREAD and
# VMWRITE, which join that list whenever the "VMCS shadowing" control is
# 0. No VM-execution control turns any of them off, so a guest reaches
# every one of them with nothing but the instruction.
#
# Needs no build and no emulator: it reads the source.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -u

root="$(cd "$(dirname "$0")/../.." && pwd)"
handler="$root/hypervisor/src/hypervisor/hypervisor.cpp"

[ -f "$handler" ] || {
    echo "missing $handler" >&2
    exit 2
}

status=0

cases=$(grep -o 'case basic_reason::[a-z_0-9]*' "$handler" \
        | sed 's/case basic_reason:://' | sort -u)

has_case() {
    echo "$cases" | grep -qx "$1"
}

# SDM 28.1.2 names CPUID, GETSEC, INVD and XSETBV, and then "instructions
# introduced with VMX": INVEPT, INVVPID, SEAMCALL, TDCALL, VMCALL,
# VMCLEAR, VMLAUNCH, VMPTRLD, VMPTRST, VMRESUME, VMXOFF and VMXON.
#
# **Instructions are not the whole of it**, and stopping at them is how
# two more reasons sat uncased for as long as they did. SDM 28.2, "Other
# Causes of VM Exits" (.references/sdm.txt:200927 onwards), lists events
# that exit with no VM-execution control gating them at all, and four of
# them are reachable here:
#
#   task switch   (:200956) "Task switches are not allowed in VMX
#                 non-root operation. Any attempt to effect a task switch
#                 in VMX non-root operation causes a VM exit." A far JMP
#                 through a TSS descriptor is enough.
#   triple fault  (:200927) the guest faulted calling its own
#                 double-fault handler.
#   INIT signal   (:200945) and
#   start-up IPI  (:200950), which this VMM already answers because it
#                 emulates processor start-up.
#
# The rest of 28.2 is gated on something this VMM does not turn on: SMIs
# need the dual-monitor treatment, external interrupts need pin bit 0,
# and the preemption timer is armed only where a case exists for it.
#
# SEAMCALL and TDCALL are left out: they exist only on a processor with
# TDX, they have no basic exit reason in the enumeration this tree
# carries, and nothing here can reach them.
#
# VMREAD and VMWRITE are added: SDM 28.1.3 makes them exit whenever the
# "VMCS shadowing" VM-execution control is 0, and setup_vmcs leaves it 0
# unless the nested machinery is compiled in - and with it in, the
# instructions are answered rather than faulted, so a case is needed
# either way.
required="cpuid getsec invd xsetbv invept invvpid vmcall vmclear
vmlaunch vmptrld vmptrst vmresume vmxoff vmxon vmread vmwrite
task_switch triple_fault init_signal start_up_ipi"

# Reasons that are known to have no case, with why.
#
# Each entry is a divergence from the rule above that has been looked at
# and left, and the reason travels with it. A reason that is missing and
# *not* named here fails this check; a reason named here that has since
# gained a case also fails it, so the list cannot outlive the gap.
#
# Empty, and it should stay that way. GETSEC was the last entry: it is
# fixed rather than recorded now, and the XPASS branch below is what
# emptied the list - the case appeared, the script refused, and the entry
# had to go. That is the mechanism working, so do not add an entry here
# in preference to adding a case.
known_gaps=""

echo "== nothing a guest can execute may reach default:"

for reason in $required; do
    expected_gap=0
    for gap in $known_gaps; do
        [ "$reason" = "$gap" ] && expected_gap=1
    done

    if has_case "$reason"; then
        if [ "$expected_gap" = "1" ]; then
            echo "  XPASS $reason now has a case - delete it from" >&2
            echo "        known_gaps in this script, and from the" >&2
            echo "        report the gap is written up in." >&2
            status=1
        else
            echo "  ok    $reason"
        fi
    elif [ "$expected_gap" = "1" ]; then
        echo "  XFAIL $reason has no case - known gap, see below"
    else
        echo "  FAIL  $reason exits unconditionally in VMX non-root" >&2
        echo "        operation (SDM 28.1.2) and has no case, so a" >&2
        echo "        guest executing it reaches default: and stops" >&2
        echo "        this processor. Add a case that faults." >&2
        status=1
    fi
done

# The other half of the CR4/CPUID pair, which CLAUDE.md says are "keyed on
# the same constant so they cannot drift apart". Checked because the
# combination they exist to prevent - no VMX in CPUID, VMXE set in CR4 -
# exists on no real processor, and a guest that trusts CR4 then faults on
# its own VMXON. BACKLOG.md item 1.
echo "== CR4.VMXE and CPUID leaf 1 ECX[5] agree"

if grep -q 'cr4_guest_host_mask(cr4_vmxe | cr4_smxe)' "$handler"; then
    echo "  ok    CR4.VMXE is owned in the guest/host mask"
else
    echo "  FAIL  CR4.VMXE is not in the CR4 guest/host mask, so the" >&2
    echo "        read shadow answers for no bits and the guest reads" >&2
    echo "        the real register - which has VMXE set, because a" >&2
    echo "        processor in root mode must." >&2
    status=1
fi

if grep -q 'cr4_read_shadow(this->guest_cr4 & ~(cr4_vmxe | cr4_smxe))' \
    "$handler"; then
    echo "  ok    the read shadow starts with VMXE and SMXE clear"
else
    echo "  FAIL  the CR4 read shadow does not start with VMXE and" >&2
    echo "        SMXE clear" >&2
    status=1
fi

# === CR4.SMXE and CPUID leaf 1 ECX[6] agree ===========================
#
# The same pairing one bit over, and the one that was missing. `073bc83`
# concealed safer mode extensions in CPUID and left CR4.SMXE unmasked, so
# a guest could set the bit against a CPUID saying the feature does not
# exist - and then execute GETSEC, which SDM 28.1.2 makes exit
# unconditionally with that bit set, into a handler that had no case for
# it. Two instructions to stop a physical processor.
#
# Three things now have to hold together, which is why they are checked
# together rather than one per section: the bit is concealed in CPUID, it
# is refused in CR4, and GETSEC faults if it is ever reached anyway.
echo "== CR4.SMXE and CPUID leaf 1 ECX[6] agree"

if grep -q 'cpuid_result\[2\] &= ~(1u << 6)' "$handler"; then
    echo "  ok    CPUID leaf 1 ECX[6] conceals safer mode extensions"
else
    echo "  FAIL  safer mode extensions are no longer concealed in" >&2
    echo "        CPUID leaf 1. If the feature is now advertised, the" >&2
    echo "        CR4 mask and the GETSEC case have to move with it -" >&2
    echo "        and nothing here implements a measured launch." >&2
    status=1
fi

if grep -q 'vmcs.guest_cr4(this->host_cr4 & ~cr4_smxe)' "$handler"; then
    echo "  ok    SMXE is kept out of the real guest CR4"
else
    echo "  FAIL  the guest CR4 is no longer built with SMXE cleared." >&2
    echo "        Unlike VMXE, nothing here needs the bit set - and" >&2
    echo "        with it set GETSEC exits (SDM 28.1.2) instead of" >&2
    echo "        raising the #UD a processor without SMX would." >&2
    status=1
fi

if grep -q 'vmcs.guest_cr4((value | cr4_vmxe) & ~cr4_smxe)' "$handler"; then
    echo "  ok    and out of what a guest writes to CR4"
else
    echo "  FAIL  the MOV to CR4 handler no longer strips SMXE, so a" >&2
    echo "        guest that sets it reaches the register and can then" >&2
    echo "        exit on GETSEC." >&2
    status=1
fi

# The CPUID half has to be conditional on the same constant the nested
# machinery is, and the two lines are five apart in the source - so the
# window is generous rather than exact, which is the right trade for a
# grep: it catches the bit being cleared unconditionally or the guard
# being deleted, and does not care about reformatting.
if grep -A6 'if constexpr (!nested_vmx::enabled)' "$handler" \
    | grep -q 'cpuid_result\[2\] &= ~(1u << 5)'; then
    echo "  ok    CPUID leaf 1 ECX[5] is cleared under !nested_vmx::enabled"
else
    echo "  FAIL  the VMX bit in CPUID leaf 1 is not cleared under" >&2
    echo "        !nested_vmx::enabled. It and CR4.VMXE must be keyed" >&2
    echo "        on the same constant: a guest told there is no VMX" >&2
    echo "        and shown VMXE set faults on its own vmxon." >&2
    status=1
fi

# === The watched local APIC page must be one the host can address =====
#
# `filter_local_apic_write` and `on_local_apic_write` reach the watched
# page by dereferencing `page << 12` as a **host virtual address**, and
# the host page table maps exactly one local APIC page - read from
# IA32_APIC_BASE once, before any guest ran.
#
# A guest may relocate its local APIC; IA32_APIC_BASE[35:12] is writable
# and `note_apic_mode` follows the move. Before the guard below, the watch
# was then armed on a page the host does not map, and the guest's next
# interrupt-command store took an EPT violation into a filter that read an
# unmapped address - a #PF in the exit handler, where there is no recovery
# point, so the processor stops with nothing recorded anywhere.
#
# Checked at the source because no harness compiles `watch_local_apic`,
# and because the two halves live in different translation units: the
# record is written where the mapping is made in hypervisor.cpp, and read
# where the watch is armed in local_apic.cpp. A grep is what spans them.
echo "== the watched local APIC page is one the host page table maps"

apic="$root/hypervisor/src/hypervisor/local_apic.cpp"
setup="$root/hypervisor/src/hypervisor/hypervisor.cpp"

if grep -q 'this->mapped_apic_page = apic_base;' "$setup"; then
    echo "  ok    the mapped page is recorded where it is mapped"
else
    echo "  FAIL  nothing records which local APIC page the host page" >&2
    echo "        table maps, so the watch cannot tell whether the page" >&2
    echo "        it is about to arm on is addressable." >&2
    status=1
fi

if grep -q 'base != this->mapped_apic_page' "$apic"; then
    echo "  ok    and watch_local_apic refuses any other page"
else
    echo "  FAIL  watch_local_apic no longer refuses a local APIC page" >&2
    echo "        other than the one the host page table maps. A guest" >&2
    echo "        that relocates its APIC now arms the watch on an" >&2
    echo "        address the filter cannot dereference, and the next" >&2
    echo "        interrupt-command write stops the processor with" >&2
    echo "        nothing recorded." >&2
    status=1
fi

# The MSR ranges the bitmap can govern. Everything outside them exits
# unconditionally (SDM 28.1.3, .references/sdm.txt:200814), which is why
# an all-zeroes bitmap does not stop the accesses that cost an afternoon.
echo "== an unimplemented MSR faults rather than being resumed past"

if grep -q 'inject_general_protection_fault();' "$handler"; then
    echo "  ok    the MSR path injects #GP"
else
    echo "  FAIL  nothing injects a general protection fault" >&2
    status=1
fi

# RIP must stay on the faulting instruction. A #GP is reported at the
# instruction that caused it, and advancing past one is how a guest ends
# up believing it read a value it never got - which is the 0xc000000d
# that blamed Windows' own boot configuration data.
if grep -A6 'inject_general_protection_fault();' "$handler" \
    | grep -q 'advance_rip = false'; then
    echo "  ok    RIP is left on the faulting instruction"
else
    echo "  FAIL  a path injects #GP without clearing advance_rip" >&2
    status=1
fi

# === What is advertised versus what is implemented ====================
#
# A capability MSR bit is a promise, and this project's recurring failure
# mode is "answering part of an interface". Most of that surface is
# checked by tests/nested_vmx section 13, which reads the MSRs the
# emulation actually answers - but two of the pairings span headers the
# harness replaces with a shim, so they can only be checked here, against
# the real sources.
# The two controls the coverage suite turns on, and the case that answers
# them. Both are off in any build anyone deploys - a UEFI firmware parks
# its application processors in `monitor; mwait; jmp` and this VMM adopts
# them, which measured 1,190,000 exits per processor in under two minutes
# - so the handler case for them cannot execute in a deployed build.
#
# That is exactly why the case has to keep existing: turning the constant
# back on is one line, and a case that has never executed is not a case.
# ZPP_GUEST_TESTS turns the controls on so the coverage suite executes it.
# This asserts the three parts stay attached to each other.
echo "== monitor and mwait: the controls, the switch and the case"

if grep -q 'trap_monitor_and_mwait = ZPP_GUEST_TESTS' "$handler"; then
    echo "  ok    the controls are keyed on ZPP_GUEST_TESTS"
elif grep -q 'trap_monitor_and_mwait = false' "$handler"; then
    echo "  FAIL  trap_monitor_and_mwait is hardcoded false again, so" >&2
    echo "        the monitor/mwait case cannot execute in any build and" >&2
    echo "        the coverage suite cannot reach exit reasons 36 or 39." >&2
    status=1
else
    echo "  FAIL  trap_monitor_and_mwait is neither false nor keyed on" >&2
    echo "        ZPP_GUEST_TESTS - if it is now unconditionally true," >&2
    echo "        every idle guest processor pays two exits per loop." >&2
    status=1
fi

if has_case "monitor" && has_case "mwait"; then
    echo "  ok    the handler answers both"
else
    echo "  FAIL  the monitor/mwait case was removed while the controls" >&2
    echo "        can still be turned on - they would reach default:." >&2
    status=1
fi

echo "== a capability offered is a capability implemented"

nested="$root/hypervisor/include/zpp/hypervisor/nested_vmx.h"
header="$root/hypervisor/include/zpp/hypervisor/hypervisor.h"

# Execute-only translations: IA32_VMX_EPT_VPID_CAP bit 0, paired with
# `execute_only_translations_offered`. That constant decides what
# `ept_permissions::normalised` may leave in a shadow entry, so reporting
# the bit without honouring it - or honouring it without reporting it -
# puts the capability MSR and the permission composition at odds, and the
# guest hypervisor is the one that finds out.
#
# The mask is written one bit per line with the bit number in the
# expression, so "is bit 0 in it" is a grep for the line rather than an
# arithmetic evaluation.
if grep -A20 'supported_ept_vpid_capabilities' "$nested" \
    | grep -qE '^\s*\(1ull << 0\)'; then
    offers_execute_only=1
else
    offers_execute_only=0
fi

if grep -q 'execute_only_translations_offered = true' "$header"; then
    honours_execute_only=1
else
    honours_execute_only=0
fi

if [ "$offers_execute_only" = "$honours_execute_only" ]; then
    echo "  ok    execute-only translations: offered=$offers_execute_only"\
         "honoured=$honours_execute_only"
else
    echo "  FAIL  IA32_VMX_EPT_VPID_CAP bit 0 says execute-only" >&2
    echo "        translations are offered=$offers_execute_only while" >&2
    echo "        execute_only_translations_offered says" >&2
    echo "        honoured=$honours_execute_only. The capability MSR and" >&2
    echo "        ept_permissions::normalised have to move together: a" >&2
    echo "        guest hypervisor builds its own tables on the strength" >&2
    echo "        of that bit." >&2
    status=1
fi

# Accessed and dirty flags: bit 21 withheld, and build_vmcs02 must refuse
# an EPT pointer that asks for them. Withholding the bit while accepting
# the pointer leaves a guest hypervisor's page tracking silently never
# marking anything - the failure that looks like a guest bug for as long
# as it takes to find.
if grep -A20 'supported_ept_vpid_capabilities' "$nested" \
    | grep -qE '^\s*\(1ull << 21\)'; then
    echo "  FAIL  IA32_VMX_EPT_VPID_CAP bit 21 offers accessed and" >&2
    echo "        dirty flags, and nothing in the shadow sets either." >&2
    status=1
elif grep -q 'ept_cap_access_and_dirty' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp"; then
    echo "  ok    accessed and dirty flags withheld, and build_vmcs02"\
         "refuses an EPT pointer asking for them"
else
    echo "  FAIL  accessed and dirty flags are withheld in the" >&2
    echo "        capability MSR, but build_vmcs02 no longer refuses an" >&2
    echo "        EPT pointer that asks for them." >&2
    status=1
fi

# The VMX-preemption timer, which this VMM arms for its own use. It must
# be absent from what a guest hypervisor is offered *and* stripped from
# the pin union in build_vmcs02 - either alone is not enough, and the two
# live in different files.
if grep -A6 'supported_pin_based_controls' "$nested" \
    | grep -qE '^\s*\(1ull << 6\)'; then
    echo "  FAIL  pin control bit 6 offers the VMX-preemption timer to" >&2
    echo "        a guest hypervisor, and this VMM arms it for itself." >&2
    status=1
elif grep -q 'pin_preemption_timer' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp"; then
    echo "  ok    the VMX-preemption timer is withheld and stripped"\
         "from vmcs02"
else
    echo "  FAIL  the preemption timer is withheld from the capability" >&2
    echo "        MSR but build_vmcs02 no longer strips it from the pin" >&2
    echo "        union." >&2
    status=1
fi

# Posted interrupts, same shape: pin bit 7 withheld and stripped.
if grep -A6 'supported_pin_based_controls' "$nested" \
    | grep -qE '^\s*\(1ull << 7\)'; then
    echo "  FAIL  pin control bit 7 offers posted interrupts, and there" >&2
    echo "        is no posted-interrupt descriptor behind it." >&2
    status=1
elif grep -q 'pin_posted_interrupts' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp"; then
    echo "  ok    posted interrupts are withheld and stripped from vmcs02"
else
    echo "  FAIL  posted interrupts are withheld from the capability MSR" >&2
    echo "        but build_vmcs02 no longer strips the bit." >&2
    status=1
fi

# The MSR load and store areas. Their addresses must never reach the
# processor: it reads and *writes* those lists in root operation, where
# extended page tables do not apply, so a guest hypervisor could name this
# module's own pages as its VM-exit MSR-store area and have the processor
# write MSR values into them. Every other protection here is an EPT
# permission and none of them would apply.
if grep -q 'vm_entry_msr_load_count, 0' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp" &&
   grep -q 'vm_exit_msr_load_count, 0' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp" &&
   grep -q 'vm_exit_msr_store_count, 0' \
    "$root/hypervisor/src/hypervisor/nested_entry.cpp"; then
    echo "  ok    all three MSR area counts in vmcs02 are zero, so the"\
         "processor is given no list of its own"
else
    echo "  FAIL  build_vmcs02 no longer zeroes all three MSR area" >&2
    echo "        counts. The processor reads and writes those lists in" >&2
    echo "        root operation, where extended page tables do not" >&2
    echo "        apply - a guest hypervisor naming this module's own" >&2
    echo "        pages would have MSR values written into them." >&2
    status=1
fi

# === The window controls come from the guest hypervisor alone =========
#
# A window exit says "the guest can take an interrupt now". It is an
# answer to a question, and only whoever asked it can act on the answer -
# so vmcs02 takes the interrupt-window and NMI-window controls from
# vmcs12 and not from the union with this VMM's own.
#
# Both directions cost something, and the second is the expensive one:
#
# - Inheriting this VMM's bits would produce window exits with nothing to
#   do, on a path taken once per interrupt.
# - **Dropping the guest hypervisor's bits is worse.** A guest hypervisor
#   that has an interrupt to deliver to a second-level guest with
#   interrupts blocked sets interrupt-window exiting and waits for the
#   exit that says it may inject. If that bit never reaches vmcs02 the
#   exit never happens, the guest hypervisor never injects, and the
#   second-level guest never runs again - with nothing anywhere reporting
#   a fault. From outside it is a virtual processor that has stopped.
#
# KVM composes it the same way in `prepare_vmcs02_early`.
#
# Checked at the source because the composition is inside build_vmcs02,
# which the harnesses cannot reach: it makes vmcs02 current, and from
# that point nothing may fail.
echo "== the interrupt and NMI window controls come from vmcs12"

entry_source="$root/hypervisor/src/hypervisor/nested_entry.cpp"

if grep -q 'primary01 & ~(primary_interrupt_window | primary_nmi_window)' \
    "$entry_source"; then
    echo "  ok    this VMM's own window bits are masked out of the union"
else
    echo "  FAIL  vmcs02's primary controls no longer take the window" >&2
    echo "        controls from the guest hypervisor alone. If its bits" >&2
    echo "        are being dropped, a guest hypervisor waiting for an" >&2
    echo "        interrupt-window exit never gets one, never injects," >&2
    echo "        and its second-level guest never runs again - with no" >&2
    echo "        fault reported anywhere." >&2
    status=1
fi

# The guest hypervisor's half has to be ORed in **whole**. Masking the
# window bits out of it as well would compile, read as symmetry, and lose
# exactly the request this is about - so the line is matched on its own
# rather than by looking for the identifier anywhere nearby, which the
# masked form would also satisfy.
if grep -A2 'primary01 & ~(primary_interrupt_window | primary_nmi_window)' \
    "$entry_source" | grep -qE '^\s*primary12;\s*$'; then
    echo "  ok    and the guest hypervisor's are ORed in whole"
else
    echo "  FAIL  the guest hypervisor's primary controls are no longer" >&2
    echo "        ORed into vmcs02's, so its window request is lost." >&2
    status=1
fi

# And the bit has to be offered in the first place. A capability MSR that
# withholds interrupt-window exiting would make a guest hypervisor unable
# to ask the question at all, which fails the same way one step earlier.
if grep -A4 'supported_primary_controls' "$nested" \
    | grep -qE '^\s*\(1ull << 2\)'; then
    echo "  ok    interrupt-window exiting is offered to a guest"\
         "hypervisor"
else
    echo "  FAIL  primary control bit 2, interrupt-window exiting, is" >&2
    echo "        not offered. A guest hypervisor cannot then ask when" >&2
    echo "        it may inject into a second-level guest with" >&2
    echo "        interrupts blocked, and has no way to deliver one." >&2
    status=1
fi

# === The entry-interruption triple reaches vmcs02 ======================
#
# The three fields are how a guest hypervisor delivers anything at all to
# its own guest: the information field's valid bit, the exception error
# code and the instruction length. SDM 27.8.3 makes them a set - the valid
# bit decides whether the other two are read.
#
# If the information field never reaches vmcs02, every injection a guest
# hypervisor makes is silently dropped. That is not a subtle failure and
# it has no diagnostic: SDM 30.2 clears the valid bit on every VM exit, so
# by the time the guest hypervisor looks again its own record of the
# injection is gone too. A halted virtual processor is never woken and
# nothing anywhere reports a fault - which is the shape of the failure
# being chased on the rig.
#
# Checked here because the copy is inside build_vmcs02, past the vmptrld
# that makes vmcs02 current, and from that point nothing may fail - so no
# hosted harness can drive it. tests/nested_exit covers the decision that
# leads to it; this covers the copy itself.
echo "== the entry-interruption triple reaches vmcs02"

if grep -q 'vmcs.write(field::vm_entry_interruption_information_field, injection)' \
    "$entry_source"; then
    echo "  ok    the information field is copied from vmcs12"
else
    echo "  FAIL  the entry-interruption information field is no longer" >&2
    echo "        copied into vmcs02. Every injection a guest hypervisor" >&2
    echo "        makes is dropped, and SDM 30.2 clears its own record" >&2
    echo "        of it on the next exit, so it cannot find out." >&2
    status=1
fi

for field in vm_entry_exception_error_code vm_entry_instruction_length; do
    if grep -A6 'vmcs.write(field::vm_entry_interruption_information_field, injection)' \
        "$entry_source" | grep -q "shadow.read(field::$field)"; then
        echo "  ok    $field travels with it"
    else
        echo "  FAIL  $field is no longer copied beside the" >&2
        echo "        information field. SDM 27.8.3 makes the three a" >&2
        echo "        set: an exception injected without its error code" >&2
        echo "        pushes whatever vmcs02 last held." >&2
        status=1
    fi
done

# And the two are read only when the valid bit is set, which is the
# other half of 27.8.3. Reading them unconditionally is harmless; writing
# them unconditionally is not, because a stale error code left in vmcs02
# is what the next injected exception would push.
if grep -B2 'shadow.read(field::vm_entry_exception_error_code)' \
    "$entry_source" | grep -q 'injection & interruption_valid'; then
    echo "  ok    and only when the valid bit says they mean anything"
else
    echo "  FAIL  the error code and instruction length are copied" >&2
    echo "        without testing the valid bit - SDM 27.8.3 makes the" >&2
    echo "        information field decide whether the other two are" >&2
    echo "        read at all." >&2
    status=1
fi

echo
if [ "$status" = "0" ]; then
    echo "exit handler invariants hold"
else
    echo "exit handler invariants BROKEN" >&2
fi

exit "$status"
