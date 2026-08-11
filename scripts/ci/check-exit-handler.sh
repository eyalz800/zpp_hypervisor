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
vmlaunch vmptrld vmptrst vmresume vmxoff vmxon vmread vmwrite"

# Reasons that are known to have no case, with why.
#
# Each entry is a divergence from the rule above that has been looked at
# and left, and the reason travels with it. A reason that is missing and
# *not* named here fails this check; a reason named here that has since
# gained a case also fails it, so the list cannot outlive the gap.
known_gaps="getsec"

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

# The known gap, spelled out where anyone reading the output will see it.
cat <<'GAP'

  GETSEC, exit reason 11, has no case.
    SDM 28.1.2 lists it among the instructions that exit
    unconditionally, so on the face of it a guest can stop a processor
    with one instruction. What stands between the two is CR4.SMXE:
    GETSEC raises #UD with it clear, and this VMM conceals safer mode
    extensions from the guest by clearing CPUID leaf 1 ECX[6].
    But CR4.SMXE is *not* in the CR4 guest/host mask - only VMXE is -
    so on a processor that has SMX a guest can set the bit anyway,
    against a CPUID that says it does not exist, and then execute
    GETSEC. Concealing a feature in CPUID is not the same as making it
    unreachable.
    The fix is one case that injects #UD, exactly as the thirteen VMX
    instructions do, and it is not made here because this script's job
    is to report the invariant rather than to change the handler.

GAP

# The other half of the CR4/CPUID pair, which CLAUDE.md says are "keyed on
# the same constant so they cannot drift apart". Checked because the
# combination they exist to prevent - no VMX in CPUID, VMXE set in CR4 -
# exists on no real processor, and a guest that trusts CR4 then faults on
# its own VMXON. BACKLOG.md item 1.
echo "== CR4.VMXE and CPUID leaf 1 ECX[5] agree"

if grep -q 'cr4_guest_host_mask(cr4_vmxe)' "$handler"; then
    echo "  ok    CR4.VMXE is owned in the guest/host mask"
else
    echo "  FAIL  CR4.VMXE is not in the CR4 guest/host mask, so the" >&2
    echo "        read shadow answers for no bits and the guest reads" >&2
    echo "        the real register - which has VMXE set, because a" >&2
    echo "        processor in root mode must." >&2
    status=1
fi

if grep -q 'cr4_read_shadow(this->guest_cr4 & ~cr4_vmxe)' "$handler"; then
    echo "  ok    the read shadow starts with VMXE clear"
else
    echo "  FAIL  the CR4 read shadow does not start with VMXE clear" >&2
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

echo
if [ "$status" = "0" ]; then
    echo "exit handler invariants hold"
else
    echo "exit handler invariants BROKEN" >&2
fi

exit "$status"
