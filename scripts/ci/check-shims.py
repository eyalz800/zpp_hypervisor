#!/usr/bin/env python3
"""What the hosted test suite is allowed to stand in for.

Replaces check-shim-members.py, which compared six hand-written copies
of `zpp::hypervisor::hypervisor` against the real header and reported
where a member's declaration disagreed. There are no copies left, so
there is nothing left for it to compare - every harness under tests/
compiles the class the hypervisor is built from, and every declaration
in it is therefore checked by the compiler rather than by a script.

What the old checker caught was a `std::uint64_t` a shim spelled
`std::uint32_t`. That cannot recur: there is one declaration now. What
it could *not* catch was an enumerator - tests/nested_vmx carried
`error::nested_control_unsupported` against the hypervisor's
`nested_controls_unsupported`, a different constant with a similar name,
and every assertion about it passed. That cannot recur either, and for
the same reason.

So the drift check is retired and this takes its place, guarding the
rule the copies violated rather than the copies themselves:

  **Shim the hardware, not the code under test.**

A stand-in header is legitimate when the real one cannot be compiled or
executed on the machine running the tests - `zpp/arch/x86_64/asm.h` is
naked x86-64 assembly and these tests run on arm64. It is not
legitimate for anything the harness merely finds inconvenient: a
stand-in for the class under test is a second definition of the thing
being tested, and a test against it is a test of the copy.

Everything under a directory named `shim` has to appear below with a
reason. Adding a file without adding it here fails, which is the point:
the decision to stand something in gets made deliberately and gets
written down.

Registered as the `check-shims` test in tests/CMakeLists.txt. Needs no
build.
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TESTS = os.path.join(ROOT, "tests")

# Path relative to tests/, and why the real header cannot be used.
#
# Every entry here is a header that cannot compile or cannot execute on
# the machine running this suite. Nothing else belongs in it.
ALLOWED = {
    "shim/zpp/arch/x86_64/asm.h":
        "naked x86-64 assembly; the development host is arm64. Also "
        "supplies a restore_context that returns, so a [[noreturn]] "
        "resume can be looked at afterwards",
    "shim/zpp/arch/x86_64/vmx/asm.h":
        "the VMX instructions; no VMX on the development host, and the "
        "in-memory VMCS it keeps is what lets the real vmcs.h run",
    "ap_start_up/shim/zpp/arch/x86_64/asm.h":
        "as above, and thread_local rather than global: that harness "
        "runs four host threads as four logical processors, and one "
        "shared control register would make the harness the thing that "
        "races",
    "ap_start_up/shim/zpp/arch/x86_64/vmx/asm.h":
        "as above, thread_local for the same reason - one shared VMCS "
        "would let one thread read back another's VMWRITEs",
    "ap_start_up/shim/zpp/arch/x86_64/mmio.h":
        "records a register write instead of performing it, keeping its "
        "address and its position in a sequence. Pointing the harness's "
        "IA32_APIC_BASE at a real buffer and using the real accessors "
        "was considered and does not work: the question that shim "
        "exists to ask is whether the *high* half of the interrupt "
        "command is written before the low half - writing the low half "
        "is what sends the interrupt - and two volatile stores into a "
        "buffer leave no record of which came first",
}

# Files that must never be stood in for, with what it cost when they
# were. Reported before the general rule below, because the message is
# the useful part.
FORBIDDEN = {
    "zpp/hypervisor/hypervisor.h":
        "the class under test. Six copies of it existed, 1463 lines "
        "against a 6246-line original, and they drifted: an enumerator "
        "renamed in one of them made every assertion about that error "
        "an assertion about a constant the hypervisor does not have",
    "zpp/diag/log.h":
        "the diagnostic log. It compiles away to nothing under "
        "ZPP_DIAG=0 and works for real under ZPP_DIAG=1, so a harness "
        "has a real answer either way",
    "zpp/arch/x86_64/page_table.h":
        "the page table. tests/support/identity_page_table.h supplies "
        "the source table the real one is built from, which is all it "
        "needs from outside",
}


def main():
    if not os.path.isdir(TESTS):
        print("missing {}".format(TESTS), file=sys.stderr)
        return 2

    found = []
    for directory, _, files in os.walk(TESTS):
        if "shim" not in os.path.relpath(directory, TESTS).split(os.sep):
            continue
        for name in files:
            found.append(os.path.relpath(
                os.path.join(directory, name), TESTS))

    print("== stand-in headers under tests/")

    status = 0
    for path in sorted(found):
        # The header it stands in for, which is the path below the shim
        # directory - that is what an include resolves to.
        parts = path.split(os.sep)
        stands_in_for = os.sep.join(parts[parts.index("shim") + 1:])

        if stands_in_for in FORBIDDEN:
            print("  FAIL {} stands in for {}, which may not be stood "
                  "in for: {}".format(path, stands_in_for,
                                      FORBIDDEN[stands_in_for]),
                  file=sys.stderr)
            status = 1
            continue

        if path not in ALLOWED:
            print("  FAIL {} is a stand-in for one of this tree's own "
                  "headers and is not listed in {}. Shim the hardware, "
                  "not the code under test: if the real header cannot "
                  "compile or cannot execute here, add it there with "
                  "the reason; otherwise use the real "
                  "one.".format(path, os.path.relpath(__file__, ROOT)),
                  file=sys.stderr)
            status = 1
            continue

        print("   {}\n     {}".format(path, ALLOWED[path]))

    missing = sorted(set(ALLOWED) - set(found))
    for path in missing:
        print("  FAIL {} is listed as an allowed stand-in and does not "
              "exist. A list naming files that are gone stops being "
              "read.".format(path), file=sys.stderr)
        status = 1

    if 0 == status:
        print("   {} stand-in headers, every one of them hardware "
              "this machine cannot run".format(len(found)))

    return status


if __name__ == "__main__":
    sys.exit(main())
