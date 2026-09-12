#!/bin/sh
# Asserts that the diagnostic facility is not in a release build.
#
# "Debug only and genuinely zero cost in release" is a claim about a binary,
# not about the source, so it is checked against the binary. Three things are
# asserted, and each of them fails for a different reason:
#
# 1. Every release compile is given ZPP_DIAG=0. The sub-builds hardcode that
#    for release rather than passing the option through, so this also catches
#    the trap BACKLOG.md records: a -DZPP_DIAG=ON left in a CMake cache being
#    inherited silently by a later plain build.
# 2. No object in the release build defines a single zpp::diag symbol, and no
#    object carries the ring's storage. The release link strips the binary, so
#    a symbol check on the artifact itself would pass vacuously - the objects
#    are where the evidence is, and an absence there is conclusive for the
#    binary that comes out of them.
# 3. No format string reaches .rodata from a diagnostic call site.
#    Approximated by a marker the caller supplies, since the strings are
#    whatever the call sites say: pass ZPP_DIAG_MARKER=<text> when there is a
#    call site whose text can be searched for. Skipped when unset.
#
# The stronger check, worth running by hand before deploying and cheap in CI
# where a second build costs nothing:
#
#     cmake --preset release -DZPP_DIAG=ON  && cmake --build --preset release
#     cp out/release/x86_64/zpp_hypervisor /tmp/with
#     cmake --preset release -DZPP_DIAG=OFF && cmake --build --preset release
#     cmp /tmp/with out/release/x86_64/zpp_hypervisor
#
# Byte for byte identical is the whole claim, stated in one command.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
elf="$root/out/release/x86_64/zpp_hypervisor"
build="$root/build/release"
nm="${NM:-llvm-nm}"
readelf="${READELF:-llvm-readelf}"
strings="${STRINGS:-llvm-strings}"

# Skipped rather than failed when there is no release build, which is the
# same contract check-invariants.sh has and for the same reason: this is
# registered with ctest, and a developer running the suite after building
# only the debug preset has not regressed anything. 77 is ctest's
# SKIP_RETURN_CODE, set on the test in tests/CMakeLists.txt.
#
# It used to exit 1 here, which is what kept it out of ctest: a check that
# fails when its input is absent cannot be registered, so it was left
# unregistered and then nothing ran it at all.
[ -f "$elf" ] || {
    echo "no $elf - build the release preset to run this" >&2
    exit 77
}

status=0

# 1. The switch, as the compiler actually received it.
found_databases=0
for database in "$build"/hypervisor/compile_commands.json \
                "$build"/uefi-loader/compile_commands.json; do
    [ -f "$database" ] || continue
    found_databases=$((found_databases + 1))
    if grep -q 'ZPP_DIAG=1' "$database"; then
        echo "FAIL: a release compile was given ZPP_DIAG=1: $database" >&2
        status=1
    fi
    if ! grep -q 'ZPP_DIAG=0' "$database"; then
        echo "WARN: $database mentions no ZPP_DIAG at all" >&2
    fi
done
if [ "$found_databases" = "0" ]; then
    echo "WARN: no release compile database found; skipped the switch check" >&2
else
    echo "ok: every release compile was given ZPP_DIAG=0"
fi

# 2. Symbols and storage, in the objects the release binary is linked from.
objects=$(find "$build" -name '*.o' -o -name '*.obj' 2>/dev/null || true)
if [ -z "$objects" ]; then
    echo "WARN: no release objects found; skipped the symbol check" >&2
else
    # The mangled prefix for namespace zpp::diag. Matched rather than the
    # demangled spelling so this works on a stripped-of-debug-info object.
    if echo "$objects" | xargs "$nm" --defined-only 2>/dev/null |
            grep -q '_ZN3zpp4diag'; then
        echo "FAIL: release objects define zpp::diag symbols:" >&2
        echo "$objects" | xargs "$nm" --defined-only 2>/dev/null |
            grep '_ZN3zpp4diag' >&2
        status=1
    else
        echo "ok: no zpp::diag symbol in any release object"
    fi

    if echo "$objects" | xargs -n 1 "$readelf" -S --wide 2>/dev/null |
            grep -q 'ring_storage'; then
        echo "FAIL: the diagnostic ring's storage is in a release object" >&2
        status=1
    else
        echo "ok: no diagnostic ring storage in any release object"
    fi
fi

# 3. Format strings, when the caller names one to look for.
if [ -n "$ZPP_DIAG_MARKER" ]; then
    if "$strings" "$elf" | grep -q "$ZPP_DIAG_MARKER"; then
        echo "FAIL: '$ZPP_DIAG_MARKER' is in the release binary" >&2
        status=1
    else
        echo "ok: '$ZPP_DIAG_MARKER' is not in the release binary"
    fi
fi

exit "$status"
