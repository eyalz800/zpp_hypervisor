#!/bin/sh
# Runs the guest-side coverage suite under Bochs' software emulated VT-x
# and grades it.
#
# What is being tested is what this VMM answers a guest with: the VM exit
# handler's cases, the extended page tables, and the store emulation. The
# only position those questions can be asked from is inside the guest -
# a debugger outside sees guest state only, and the VMCS fields that
# decide anything cannot be read without being on that processor with that
# VMCS current. So the tests are guest instructions in the loader, behind
# ZPP_GUEST_TESTS, and this script only starts them and reads the verdict.
#
# Bochs needs no hardware virtualization, which is the whole reason this
# can run in CI at all: the hosted runners are AMD and expose no vmx.
#
# Output, all under build/bochs:
#   serial.out          the raw run
#   results.xml         JUnit, for whatever consumes test results
#   results.json        the same thing as a summary
#   coverage.txt        which exit reasons were reached and which were not
#
# Exits non-zero on any failing case, on an unexpected pass, or on a run
# that produced no verdict at all.
#
# Never run clang-format over this file - it reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-smp/bin/bochs}"

# Bounded, always. A hang has to fail the job rather than spend the
# runner's six hours discovering it. The suite is a few hundred VM exits
# on top of a firmware boot, so this is generous by an order of magnitude
# and exists only as a backstop.
timeout_seconds="${TIMEOUT:-420}"

# One processor. Everything here runs on the boot processor, which is the
# only one the loader virtualizes, and the extra processors the smoke test
# needs only slow this down - Bochs simulates them round robin whether or
# not they do anything.
cpus="${CPUS:-1}"

[ -x "$bochs" ] || { echo "no bochs at $bochs" >&2; exit 1; }
[ -f "$work/esp.img" ] || {
    echo "run scripts/bochs/setup.sh first" >&2
    exit 1
}

# The medium has to carry a build that actually has the suite in it.
# Checked here rather than assumed, because the switch persists in a CMake
# cache and a run against a loader without it produces no ZPPTEST lines at
# all - which is indistinguishable from a boot that never got that far.
# That distinction is exactly what CLAUDE.md's account of
# ZPP_VERIFY_HYPERVISOR is about, in the other direction.
loader="$root/out/${CONFIG:-debug}/x86_64/zpp_loader.efi"
if [ -f "$loader" ]; then
    if ! LC_ALL=C grep -qa 'ZPPTEST BEGIN' "$loader"; then
        echo "FAIL: $loader was not built with ZPP_GUEST_TESTS=ON." >&2
        echo "  cmake --preset ${CONFIG:-debug} -DZPP_GUEST_TESTS=ON" >&2
        echo "  cmake --build --preset ${CONFIG:-debug}" >&2
        echo "  ./scripts/bochs/setup.sh ${CONFIG:-debug}" >&2
        exit 1
    fi
fi

# Unattended, so no gdb stub: Bochs halts at reset waiting for a
# connection that is never coming, and a Bochs built without the stub
# panics on the option being mentioned at all - hence deleted rather than
# disabled.
#
# Triple faults panic rather than reboot. A silent reset looks exactly
# like a slow boot from out here and costs the whole timeout to tell
# apart; the panic puts a register dump in bochs.log.
sed -e '/^gdbstub:/d' \
    -e 's/^info:.*/info: action=report/' \
    -e 's/reset_on_triple_fault=1/reset_on_triple_fault=0/' \
    -e "s/count=1/count=$cpus/" \
    "$root/scripts/bochs/bochsrc.txt" > "$work/bochsrc-coverage.txt"

cd "$work"
: > serial.out
# Left behind by a hard kill, and the next run dies with "image locked".
rm -f esp.img.lock

# stdin from /dev/null: the text config interface reads the console, and
# CI has no tty, so any prompt Bochs raises would block rather than fail.
"$bochs" -q -f bochsrc-coverage.txt < /dev/null > bochs.stdout 2>&1 &
bochs_pid=$!

# Polled rather than waited out. Bochs never exits on its own - the
# firmware drops into its shell once the loader returns and sits there -
# so a successful run would otherwise cost the whole timeout.
waited=0
while kill -0 "$bochs_pid" 2>/dev/null; do
    if grep -q 'ZPPTEST DONE' serial.out 2>/dev/null; then
        break
    fi
    if [ "$waited" -ge "$timeout_seconds" ]; then
        echo "timed out after ${timeout_seconds}s with no verdict" >&2
        break
    fi
    sleep 2
    waited=$((waited + 2))
done

kill "$bochs_pid" 2>/dev/null || true
sleep 1
kill -9 "$bochs_pid" 2>/dev/null || true
wait "$bochs_pid" 2>/dev/null || true
echo "bochs ran for ${waited}s"

# The serial log is CRLF and carries the firmware's own output around
# ours, so pull out the two line shapes this owns and work from those.
tr -d '\r' < serial.out > serial.txt
# Only the result lines. BEGIN and DONE share the prefix and are not
# cases; counting them made the totals disagree with the suite's own.
grep -E '^ZPPTEST [^ ]+ (PASS|FAIL|SKIP|XFAIL|XPASS) ' serial.txt \
    > cases.txt 2>/dev/null || : > cases.txt
grep '^ZPPCOVER ' serial.txt > cover.txt 2>/dev/null || : > cover.txt

if [ ! -s cases.txt ]; then
    echo "=== serial output ($(wc -c < serial.out) bytes) ===" >&2
    cat serial.out >&2 || true
    echo "=== bochs log (last 60 lines) ===" >&2
    tail -60 bochs.log 2>/dev/null || echo "  no bochs.log" >&2
    echo "FAIL: no ZPPTEST lines - the guest never reached the suite" >&2
    exit 1
fi

pass=$(grep -c ' PASS ' cases.txt || true)
fail=$(grep -c ' FAIL ' cases.txt || true)
skip=$(grep -c ' SKIP ' cases.txt || true)
xfail=$(grep -c ' XFAIL ' cases.txt || true)
xpass=$(grep -c ' XPASS ' cases.txt || true)

# JUnit, so anything that consumes test results can read this without
# knowing the serial format. Failures carry the whole line, which already
# has expected and actual in it.
{
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    printf '<testsuites><testsuite name="zpp.guest_tests"'
    printf ' tests="%s" failures="%s" skipped="%s">\n' \
        "$(wc -l < cases.txt | tr -d ' ')" "$fail" "$skip"
    while read -r _ name status detail; do
        printf '  <testcase classname="zpp.guest_tests" name="%s">\n' \
            "$name"
        case "$status" in
        FAIL)
            printf '    <failure message="%s">%s</failure>\n' \
                "$status" "$detail"
            ;;
        SKIP)
            printf '    <skipped message="%s"/>\n' "$detail"
            ;;
        XFAIL)
            # Not a failure to the consumer: a case written to the SDM or
            # to KVM that this VMM knowingly diverges from. Recorded as
            # skipped with its citation so it is visible without being
            # red.
            printf '    <skipped message="expected failure: %s"/>\n' \
                "$detail"
            ;;
        XPASS)
            printf '    <failure message="unexpected pass">%s</failure>\n' \
                "$detail"
            ;;
        esac
        printf '  </testcase>\n'
    done < cases.txt
    echo '</testsuite></testsuites>'
} > results.xml

{
    printf '{\n'
    printf '  "pass": %s,\n' "$pass"
    printf '  "fail": %s,\n' "$fail"
    printf '  "skip": %s,\n' "$skip"
    printf '  "expected_failures": %s,\n' "$xfail"
    printf '  "unexpected_passes": %s,\n' "$xpass"
    printf '  "seconds": %s,\n' "$waited"
    printf '  "cases": [\n'
    first=1
    while read -r _ name status detail; do
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    {"name": "%s", "status": "%s", "detail": "%s"}' \
            "$name" "$status" "$detail"
    done < cases.txt
    printf '\n  ]\n}\n'
} > results.json

# The coverage report. The list of reasons NOT reached is the point of it,
# so it is printed in full and never filtered - a reason that stopped being
# reachable is a regression that no individual case would report.
observed=$(grep -c ' observed$' cover.txt || true)
total=$(wc -l < cover.txt | tr -d ' ')
{
    echo "VM exit reasons reached by the guest coverage suite"
    echo "  reached : $observed"
    echo "  listed  : $total  (SDM Vol. 3D Appendix C)"
    echo
    echo "REACHED"
    grep ' observed$' cover.txt | sed 's/^ZPPCOVER /  /' || true
    echo
    echo "NOT REACHED - and why is a question for whoever reads this"
    grep ' absent$' cover.txt | sed 's/^ZPPCOVER /  /' || true
} > coverage.txt

echo "=== cases ==="
cat cases.txt
echo
echo "=== coverage ==="
cat coverage.txt
echo
echo "=== summary ==="
echo "pass=$pass fail=$fail skip=$skip xfail=$xfail xpass=$xpass"
echo "exit reasons reached: $observed of $total listed"

status=0

if [ "$fail" != "0" ]; then
    echo >&2
    echo "FAILING CASES" >&2
    grep ' FAIL ' cases.txt >&2 || true
    status=1
fi

if [ "$xpass" != "0" ]; then
    echo >&2
    echo "UNEXPECTED PASSES - a known divergence was fixed and the case" >&2
    echo "should be promoted out of expected-failure:" >&2
    grep ' XPASS ' cases.txt >&2 || true
    status=1
fi

# A run that stopped part way through is a failure even with no FAIL line,
# because the cases it never reached reported nothing at all.
if ! grep -q 'ZPPTEST DONE' serial.txt; then
    echo >&2
    echo "FAIL: the run produced no DONE line - it stopped part way" >&2
    echo "=== bochs log (last 40 lines) ===" >&2
    tail -40 bochs.log 2>/dev/null || echo "  no bochs.log" >&2
    status=1
fi

# The suite's own count against the harness' count. They are produced
# independently - one by counting as it goes, the other by grepping the
# serial log afterwards - so a disagreement means serial output was lost,
# which would otherwise look like cases that were never written.
reported=$(grep 'ZPPTEST DONE' serial.txt | tail -1 || true)
if [ -n "$reported" ]; then
    echo "$reported"
    # The space before `pass=` is what keeps this off `xpass=`. Without it
    # the greedy match lands on the last field and every run disagrees
    # with itself by exactly the whole pass count.
    want_pass=$(echo "$reported" | sed -n 's/.* pass=\([0-9]*\) .*/\1/p')
    if [ -n "$want_pass" ] && [ "$want_pass" != "$pass" ]; then
        echo "FAIL: suite counted $want_pass passes, log carries $pass" >&2
        echo "      - serial output was lost between them" >&2
        status=1
    fi
fi

exit "$status"
