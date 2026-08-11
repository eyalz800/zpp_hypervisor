#!/bin/sh
# Runs every hosted test in the tree, in one command, and fails if any of
# them does.
#
# This exists because an audit of the whole history found that it did not.
# There are six harnesses under tests/ and several compile-only checks
# under scripts/ci/, together roughly eleven thousand lines of assertions
# - the decoder differential against LLVM, the nested-exit differential
# against KVM, the watched-page path, the AP start-up concurrency
# invariants, the nested VMX state machine, the ELF relocation - and
# .github/workflows/ci.yml invoked exactly two of the twelve check
# scripts and none of the harnesses. There was no ctest registration, no
# Makefile and no runner, so the rest ran only when somebody remembered
# to type a path.
#
# That is worse than not having them. check-nested-absent.sh exists
# specifically to make a silent byte-level regression loud, and a check
# nobody runs is a check that reports nothing while looking like
# coverage.
#
# Each entry is run in its own subshell so one failing does not stop the
# rest: the useful output of a run like this is *which* of them failed,
# and stopping at the first hides that.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -u

root="$(cd "$(dirname "$0")/../.." && pwd)"
config="${1:-debug}"

# Bounded, because a harness that hangs must fail the job rather than
# spend the runner's budget. Generous: the slowest of these is the
# decoder differential, which assembles and disassembles a corpus of tens
# of thousands of instructions.
per_test_timeout="${HOST_TEST_TIMEOUT:-900}"

# `timeout` is not on every macOS by default, and a developer machine is
# the other place this runs. Fall back to running without it rather than
# refusing.
if command -v timeout > /dev/null 2>&1; then
    limit="timeout $per_test_timeout"
elif command -v gtimeout > /dev/null 2>&1; then
    limit="gtimeout $per_test_timeout"
else
    limit=""
    echo "note: no timeout(1) found, running unbounded" >&2
fi

failed=""
passed=""
skipped=""

run() {
    name="$1"
    shift

    echo
    echo "=================================================="
    echo "== $name"
    echo "=================================================="

    if $limit "$@"; then
        passed="$passed $name"
    else
        status=$?
        echo "-- $name FAILED (exit $status)"
        failed="$failed $name"
    fi
}

# The harnesses. Each builds itself; see its own build.sh for what it
# compiles and why it needs no shim, or which functions it cuts out of
# hypervisor.cpp by name when it does.
for harness in \
    elf_relocate \
    local_apic \
    decoder \
    nested_exit \
    watched_page \
    ap_start_up \
    nested_vmx \
    mtrr \
    crt \
    page_table
do
    if [ ! -x "$root/tests/$harness/build.sh" ]; then
        echo "-- tests/$harness/build.sh missing or not executable" >&2
        failed="$failed tests/$harness"
        continue
    fi
    run "tests/$harness" "$root/tests/$harness/build.sh"
done

# The compile-only checks. Their assertions are static_asserts, so the
# compile *is* the run - there is nothing to execute and nothing to skip.
run "check-decoder" "$root/scripts/ci/check-decoder.sh"
run "check-instruction" "$root/scripts/ci/check-instruction.sh"
run "check-nested-ept" "$root/scripts/ci/check-nested-ept.sh"

# The source-level invariants, which need no build at all.
run "check-exit-handler" "$root/scripts/ci/check-exit-handler.sh"

# The harness shims against the class they stand in for. A member the
# real source uses and a shim lacks is a compile error and needs no help;
# a member present in both with a *different* declaration compiles and
# tests the wrong thing, which is what this catches.
run "check-shim-members" python3 "$root/scripts/ci/check-shim-members.py"

# The Python readers under scripts/, against the C++ they transcribe.
# There were no Python tests of any kind in this tree, and those scripts
# are the one place a wrong constant produces *plausible* output rather
# than an error - which is the failure mode that sends an investigation
# to the hypervisor instead of to the reader.
run "tests/python_layout" python3 -m unittest discover \
    -s "$root/tests/python_layout" -t "$root/tests/python_layout"

# The checks that grade a built binary. Skipped rather than failed when
# the binary is not there, because this script is also the one a
# developer runs before touching CMake.
if [ -f "$root/out/$config/x86_64/zpp_hypervisor" ]; then
    run "check-invariants" "$root/scripts/ci/check-invariants.sh" "$config"
else
    echo "-- check-invariants skipped: no out/$config/x86_64/zpp_hypervisor"
    skipped="$skipped check-invariants"
fi

# Two checks are deliberately not here, and the reasons are worth
# recording so they are not simply added later and found to flap.
#
# check-nested-absent.sh compares a sha256 of the release hypervisor
# against a committed baseline. That is exactly the right check for
# "nothing changed in the bytes", and it is a property of one toolchain
# on one machine: a different clang 22 build, a different host or a
# different set of prefix maps produces different bytes for identical
# source, so wiring it into a hosted runner would make it fail for
# reasons that say nothing. It belongs on the machine that owns the
# baseline.
#
# check-diag-absent.sh grades release *objects*, which means it needs the
# release preset built first. It is worth adding to whichever job builds
# release rather than to this script, which is meant to run without a
# build having happened.

echo
echo "=================================================="
echo "== summary"
echo "=================================================="
for name in $passed; do
    echo "  ok      $name"
done
for name in $skipped; do
    echo "  skipped $name"
done
for name in $failed; do
    echo "  FAILED  $name"
done

if [ -n "$failed" ]; then
    echo
    echo "host tests failed:$failed" >&2
    exit 1
fi

echo
echo "all host tests passed"
