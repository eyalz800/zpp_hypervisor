#!/bin/sh
# Asserts that nested VMX is not in a ZPP_NESTED_VMX=OFF release build, and -
# the part that matters more - that the OFF binary has not *changed*.
#
# Why this exists rather than trusting the switch: the L1 exit path is the code
# a working Windows boot depends on, and BACKLOG.md records two long hunts for
# a hang that turned out to be a change to something adjacent. Nested VMX adds
# a second exit path beside that one. "The switch is off" is a claim about the
# source; this is a claim about the bytes.
#
# Three checks, each failing for a different reason.
#
# 1. Every release compile was given ZPP_NESTED_VMX=0. Catches the trap
#    BACKLOG.md records: an -DZPP_NESTED_VMX=ON left in a CMake cache and
#    inherited silently by a later plain build.
#
# 2. No string that only nested code emits reaches the release binary. This is
#    the check that proves the *code* is gone rather than merely unreachable,
#    and it works because of how the two builds differ. Unlike the diagnostic
#    facility, nested VMX is compiled either way - `if constexpr (!enabled)
#    return false;` sits at the top of the entry points, so the objects define
#    every nested symbol in both configurations. It is --gc-sections at link
#    time that removes them from the release binary. So a symbol check on the
#    objects would be vacuous here, where the equivalent diag check is
#    conclusive: the evidence has to come from the linked artifact.
#    Measured: 3 such strings with the switch on, 0 with it off.
#
# 3. The release binary matches a recorded baseline. While nested VMX is being
#    built, the OFF binary should not change at all - so any difference is
#    either a deliberate change to shared code or an accident, and both want to
#    be noticed. Updating the baseline is allowed and expected; doing it
#    silently is not, which is why the failure message says so.
#
#    The baseline is keyed on the compiler's version string, because the hash
#    is a property of the toolchain as much as of the source. A version with no
#    recorded baseline is skipped with a warning rather than failed - a
#    developer on a different clang is not a regression.
#
#    Verified reproducible before being relied on: a clean rebuild of the OFF
#    release preset produced the identical hash. Release is stripped, which is
#    what makes that true - a debug build embeds absolute source paths and is
#    not comparable across checkouts.
#
# What this deliberately does *not* claim: that the OFF binary is byte-identical
# to one built from a tree where the nested sources do not exist. Nothing can
# check that without deleting them, and the baseline above is the achievable
# form of the same guarantee.
set -e

root="$(cd "$(dirname "$0")/../.." && pwd)"
elf="$root/out/release/x86_64/zpp_hypervisor"
build="$root/build/release"
baseline="$(dirname "$0")/nested-off-baseline"
strings="${STRINGS:-llvm-strings}"

[ -f "$elf" ] || { echo "missing $elf - build the release preset" >&2; exit 1; }

status=0

# 1. The switch, as the compiler actually received it.
found_databases=0
switch_ok=1
for database in "$build"/hypervisor/compile_commands.json; do
    [ -f "$database" ] || continue
    found_databases=$((found_databases + 1))
    if grep -q 'ZPP_NESTED_VMX=1' "$database"; then
        echo "FAIL: a release compile was given ZPP_NESTED_VMX=1" >&2
        status=1
        switch_ok=0
    elif ! grep -q 'ZPP_NESTED_VMX=0' "$database"; then
        echo "WARN: $database mentions no ZPP_NESTED_VMX at all" >&2
        switch_ok=0
    fi
done
if [ "$found_databases" = "0" ]; then
    echo "WARN: no release compile database found; skipped the switch check" >&2
elif [ "$switch_ok" = "1" ]; then
    echo "ok: every release compile was given ZPP_NESTED_VMX=0"
fi

# 2. Strings only nested code emits. Spelled as the format strings actually
#    passed to log(), so this breaks loudly if one is reworded - which is the
#    right failure, since the check would otherwise silently stop checking.
nested_strings='guest vmxon at
guest {} refused: error
nested vmx: reporting vmx to the guest'

# The promise above - "this breaks loudly if one is reworded" - was not kept,
# and the check had silently stopped checking. One of the three was `refused:
# no second level entry`, which no longer exists anywhere in the tree: the
# refusal log reads `cpu {} guest {} refused: error {}`. A string that is not
# in the source is not in either binary, so it passed unconditionally and the
# measured count of three had quietly become two. Found while adding the
# nested TPR shadow, by grepping the source for each of them. Re-measured
# after the replacement: 3 with the switch on, 0 with it off, which is what
# the header above claims and had stopped being true.
#
# So the promise is enforced rather than stated. Each sentinel must still be
# somewhere in the hypervisor sources; one that is not is a failure here,
# because the alternative is exactly what happened - a check that reports ok
# while testing nothing.
missing_sentinels=0
echo "$nested_strings" | while IFS= read -r text; do
    [ -n "$text" ] || continue
    if ! grep -rqF "$text" "$root/hypervisor/src"; then
        echo "FAIL: sentinel string is not in the source any more: $text" >&2
        echo "      it therefore tests nothing. Replace it with a string" >&2
        echo "      nested code actually emits." >&2
        exit 1
    fi
done || missing_sentinels=1

if [ "$missing_sentinels" = "1" ]; then
    status=1
else
    echo "ok: every nested sentinel string still exists in the source"
fi

found_strings=0
echo "$nested_strings" | while IFS= read -r text; do
    [ -n "$text" ] || continue
    if "$strings" "$elf" | grep -qF "$text"; then
        echo "FAIL: nested VMX string in the release binary: $text" >&2
        exit 1
    fi
done || found_strings=1

if [ "$found_strings" = "1" ]; then
    status=1
else
    echo "ok: no nested VMX string in the release binary"
fi

# 3. The recorded baseline for this compiler.
if ! command -v shasum > /dev/null 2>&1; then
    echo "WARN: no shasum; skipped the baseline check" >&2
elif [ ! -f "$baseline" ]; then
    echo "WARN: no $baseline; skipped the baseline check" >&2
else
    # From the compile database rather than the CMake cache. The toolchain
    # file sets CMAKE_CXX_COMPILER directly, so it is never cached as an
    # entry - the cache holds only the ar, ranlib and scan-deps it derives.
    # The database holds the command actually run, which is the thing whose
    # version the hash belongs to.
    compiler=$(sed -n 's/.*"command"[^"]*"\([^ ]*\).*/\1/p' \
        "$build/hypervisor/compile_commands.json" 2>/dev/null | head -1)

    if [ -z "$compiler" ] || [ ! -x "$compiler" ]; then
        echo "WARN: no hypervisor compiler recorded; skipped the baseline" >&2
    else
        version=$("$compiler" --version | head -1)
        actual=$(shasum -a 256 "$elf" | cut -d' ' -f1)
        expected=$(grep -F "$version" "$baseline" 2>/dev/null |
            head -1 | sed 's/.*  *//')

        if [ -z "$expected" ]; then
            echo "WARN: no baseline recorded for '$version'" >&2
            echo "      the OFF release hash is $actual" >&2
        elif [ "$expected" = "$actual" ]; then
            echo "ok: the OFF release binary matches its baseline"
        else
            echo "FAIL: the OFF release binary changed." >&2
            echo "      expected $expected" >&2
            echo "      actual   $actual" >&2
            echo "" >&2
            echo "      The switch-off binary is not supposed to move while" >&2
            echo "      nested VMX is being built. Either shared code was" >&2
            echo "      changed - in which case update $baseline and say" >&2
            echo "      what changed and why in the commit - or something" >&2
            echo "      leaked out of the nested path, which is the failure" >&2
            echo "      this check exists to catch." >&2
            status=1
        fi
    fi
fi

exit "$status"
