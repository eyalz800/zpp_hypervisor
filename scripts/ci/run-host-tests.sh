#!/bin/sh
# Runs every hosted test in the tree, in one command, and fails if any of
# them does.
#
# It used to do that itself: find each harness, run its build.sh, bound it
# with timeout(1), collect which ones failed and print a summary. All four
# of those are now ctest's, so this is a wrapper over configure, build and
# ctest and nothing else. What the old script existed for survives
# unchanged, and it is worth naming which property lives where now:
#
#   - a bounded run, because a harness that hangs must fail the job rather
#     than spend the runner's budget. Now the TIMEOUT test property set in
#     tests/CMakeLists.txt, overridable here with HOST_TEST_TIMEOUT.
#   - one subprocess per test, so one failing does not stop the rest. ctest
#     runs each test as its own process and always runs them all.
#   - a report naming *which* tests failed, which is the useful output of a
#     run like this and what stopping at the first hides. ctest's summary.
#
# The one thing that changed on purpose: check-invariants is reported as
# skipped rather than silently omitted when the cross build has not run,
# because this is also the script a developer runs before touching CMake.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
config="${1:-debug}"

cd "$root"

cmake --preset "$config"
cmake --build --preset "$config" --target zpp_tests

# HOST_TEST_TIMEOUT overrides the per-test bound compiled into the test
# properties, which is what the environment variable of the same name did
# before. Unset means the tree's own value applies.
# --no-tests=error, because "no tests ran" is ctest's default *success*.
#
# The suite is registered by tests/CMakeLists.txt, and that file can now
# decline to register the compiled harnesses - it does so on a host
# compiler with no C++23 <print>, rather than failing the configure of a
# cross build that never touches them. The `zpp_tests` build above is what
# turns that into a red job, and this is the second lock on the same door:
# if the registration ever falls away for a reason nobody predicted, a run
# with nothing in it must not report success.
if [ -n "${HOST_TEST_TIMEOUT:-}" ]; then
    exec ctest --preset "$config" --no-tests=error \
        --timeout "$HOST_TEST_TIMEOUT"
fi

exec ctest --preset "$config" --no-tests=error
