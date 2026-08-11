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
# ZPP_RESTART is the other way this loop can end, and recognising it is
# worth more than the three lines it costs.
#
# With the diagnostic facility compiled in, the loader establishes the
# ESP reservation and then warm-resets the machine so the channel is
# live on the next boot. That is right on a real machine and fatal here:
# nothing after that point runs, so the suite never starts, and the only
# symptom is serial going quiet after the firmware's own "starting
# Boot0001". Waiting the full timeout out and reporting "the guest never
# reached the suite" is true and useless - it reads as a hypervisor hang
# and sends the reader to the VMM.
#
# Cost four runs and most of a session to identify from the outside.
# The loader announces it with `raw` now, which survives ZPP_TRACE being
# off, so this can stop at once and name the flag.
while kill -0 "$bochs_pid" 2>/dev/null; do
    if grep -q 'ZPPTEST DONE' serial.out 2>/dev/null; then
        break
    fi
    if grep -q 'ZPP_RESTART' serial.out 2>/dev/null; then
        echo "restarted after establishing the ESP reservation" >&2
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

if grep -q 'ZPP_RESTART' serial.txt 2>/dev/null; then
    echo >&2
    echo "FAIL: the loader established the ESP reservation and restarted" >&2
    echo "the machine, so it never reached the coverage suite. That" >&2
    echo "happens whenever the diagnostic facility is compiled in:" >&2
    echo "zpp::diag::restart_after_reservation follows ZPP_DIAG, which" >&2
    echo "defaults ON for a debug build." >&2
    echo >&2
    echo "Rebuild the medium without it, which is what ci.yml does:" >&2
    echo "  cmake --preset ${CONFIG:-debug} -DZPP_GUEST_TESTS=ON -DZPP_DIAG=OFF" >&2
    echo "  cmake --build --preset ${CONFIG:-debug}" >&2
    echo "  ./scripts/bochs/setup.sh ${CONFIG:-debug}" >&2
    grep 'ZPP_RESTART' serial.txt | sed 's/^/  /' >&2
    exit 1
fi

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

# The coverage report.
#
# Every line the suite emits carries four fields:
#
#   ZPPCOVER <reason> <name> <observed|absent> <disposition>
#
# and the disposition is the point of the whole thing. This report used to
# print the reasons that were not reached under a heading saying why was a
# question for whoever read it, and the answer was nobody: the list sat at
# 39 of 60 for as long as it existed, and a reason joining it was
# indistinguishable from a reason that had always been there.
#
# So the suite now records a disposition per reason and this grades it:
#
#   covered:<case>          a case reaches it. Absent is a REGRESSION.
#   plan:<what_would_reach_it>
#                           not reached, and the work that would reach it
#                           is named. Open work, not a closed question.
#   unreachable-here:<why>  cannot be produced here, and the run measured
#                           the thing that makes it so. Observed is a
#                           CONTRADICTION - the record is wrong.
#   out-of-scope:<why>      reachable, deliberately not reached.
#   UNEXPLAINED             nobody has said. Fails the job.
#
# `plan:` exists because the other two non-covered dispositions read as
# closed and are not. "Unreachable in this environment" is a reason to
# change the environment; "out of scope because this VMM does not request
# the control" is reachable the moment a test fixture requests it. A
# disposition that explains is not coverage, and the report below counts
# the planned ones separately so the remaining work has a number.
#
# The last is the durable part. A new reason with no disposition, or one
# whose disposition was deleted, turns the job red instead of quietly
# lengthening a list nobody reads.
observed=$(grep -c ' observed ' cover.txt || true)
total=$(wc -l < cover.txt | tr -d ' ')

# The three gradings, each as a file so they can be counted and printed
# without re-running the greps.
grep ' absent .*UNEXPLAINED' cover.txt > unexplained.txt 2>/dev/null \
    || : > unexplained.txt
grep ' absent covered:' cover.txt > regressed.txt 2>/dev/null \
    || : > regressed.txt
grep -E ' observed (unreachable-here|out-of-scope|plan):' cover.txt \
    > contradicted.txt 2>/dev/null || : > contradicted.txt
planned=$(grep -c ' absent plan:' cover.txt || true)

{
    echo "VM exit reasons reached by the guest coverage suite"
    echo "  reached : $observed"
    echo "  listed  : $total  (SDM Vol. 3D Appendix C)"
    echo
    echo "REACHED"
    grep ' observed ' cover.txt | sed 's/^ZPPCOVER /  /' || true
    echo
    echo "NOT REACHED - with a plan to reach it"
    grep ' absent plan:' cover.txt | sed 's/^ZPPCOVER /  /' || true
    echo
    echo "NOT REACHED - with the recorded reason it was not"
    grep ' absent ' cover.txt | grep -v ' absent plan:' \
        | sed 's/^ZPPCOVER /  /' || true
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
echo "exit reasons with a plan to reach them: $planned"

status=0

# The floor. Coverage may go up and may not go down.
#
# The disposition gate above catches a reason that loses its *reason*; it
# says nothing about a reason that loses its *case*, because rewriting
# `covered:exit.foo` to `unreachable-here:...` satisfies every check here
# while the coverage number falls. That is the failure mode this exists
# for, and it is not hypothetical: the list sat at 39 of 60 for as long as
# it existed precisely because nothing measured the number.
#
# Kept in a file rather than in this script so that raising it is a diff
# that says what it raised - the same argument nested_vmx.h makes for a
# constant over a CMake option.
floor_file="$root/scripts/ci/exit-coverage-floor.txt"
floor=$(sed -n 's/^floor=\([0-9]*\).*/\1/p' "$floor_file" 2>/dev/null)

rows=$(sed -n 's/^rows=\([0-9]*\).*/\1/p' "$floor_file" 2>/dev/null)

# The denominator, graded exactly.
#
# The floor below grades how many reasons were reached and nothing
# grades how many there are to reach, which leaves one way to improve
# the report without improving anything: delete a row. A reason carrying
# `plan:` costs nothing to remove, no check here notices - the
# disposition gate only reads rows that exist, and `observed` does not
# move - and the printed fraction goes from 22 of 79 to 22 of 78, which
# reads as progress.
#
# The same edit is how a real reason gets lost, and that had already
# happened: nineteen of Appendix C's reasons had no row at all and the
# report called the remaining sixty "the 60 reasons in Appendix C". See
# the comment on `rows=` in the floor file.
if [ -z "$rows" ]; then
    echo >&2
    echo "FAIL: no rows= in $floor_file - the coverage denominator" >&2
    echo "cannot be graded if nothing records what it should be." >&2
    status=1
elif [ "$total" != "$rows" ]; then
    echo >&2
    echo "COVERAGE TABLE ROW COUNT CHANGED - the run reported $total" >&2
    echo "rows and $floor_file records $rows. A row that vanished is a" >&2
    echo "reason nothing grades any more, and deleting one raises the" >&2
    echo "printed fraction without reaching anything. Add the row back," >&2
    echo "or record the new count in the same commit that changes the" >&2
    echo "table:" >&2
    echo "  $floor_file" >&2
    status=1
fi

if [ -z "$floor" ]; then
    echo >&2
    echo "FAIL: no floor in $floor_file - coverage cannot regress if" >&2
    echo "nothing records what it was." >&2
    status=1
elif [ "$observed" -lt "$floor" ]; then
    echo >&2
    echo "COVERAGE REGRESSED - $observed exit reasons were reached and" >&2
    echo "the recorded floor is $floor. A case that stopped reaching its" >&2
    echo "reason is a case that stopped testing anything, and the" >&2
    echo "individual case can still pass while it happens. Restore it," >&2
    echo "or lower the floor in a commit that says why:" >&2
    echo "  $floor_file" >&2
    status=1
elif [ "$observed" -gt "$floor" ]; then
    # Not a failure - but it is the moment the floor is cheapest to
    # raise, and the only moment anybody is looking.
    echo
    echo "COVERAGE ROSE - $observed reached against a floor of $floor."
    echo "Raise it in $floor_file so the new reasons cannot be lost."
fi

if [ "$fail" != "0" ]; then
    echo >&2
    echo "FAILING CASES" >&2
    grep ' FAIL ' cases.txt >&2 || true
    status=1
fi

if [ -s unexplained.txt ]; then
    echo >&2
    echo "UNEXPLAINED EXIT REASONS - a reason was not reached and" >&2
    echo "nothing in the suite says why. Give it a disposition in the" >&2
    echo "coverage table in uefi_loader/src/guest_tests.cpp: a case that" >&2
    echo "reaches it, a measurement showing it cannot be reached here," >&2
    echo "or the reasoning for leaving it out of scope." >&2
    sed 's/^ZPPCOVER /  /' unexplained.txt >&2
    status=1
fi

if [ -s regressed.txt ]; then
    echo >&2
    echo "REASONS THAT STOPPED BEING REACHED - a case in the suite is" >&2
    echo "written to produce each of these and none of them arrived, so" >&2
    echo "either the case stopped running or the instruction stopped" >&2
    echo "exiting. Both are regressions no individual case reports:" >&2
    sed 's/^ZPPCOVER /  /' regressed.txt >&2
    status=1
fi

if [ -s contradicted.txt ]; then
    echo >&2
    echo "REASONS RECORDED UNREACHABLE THAT WERE REACHED - the record" >&2
    echo "is wrong, which is worse than no record: it is what the next" >&2
    echo "person reads instead of checking. Promote it to a covered" >&2
    echo "case, or correct the reasoning:" >&2
    sed 's/^ZPPCOVER /  /' contradicted.txt >&2
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
