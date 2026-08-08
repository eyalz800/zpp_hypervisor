#!/bin/sh
# Compiles the nested EPT walker's own tests, which are static_asserts, so the
# compile *is* the test run - the same arrangement as check-decoder.sh beside
# it, and for the same reason: walk_ept is pure. It reaches memory only through
# a callable it is handed, so a table can be an array in the test file and no
# target is needed.
#
# Worth having beyond the usual reasons: this is the only verification of any
# part of nested VMX that does not need hardware. Everything else in it - the
# vmcs02, the reflection, the shadow tables - can currently only be read.
#
# The tests were checked for teeth rather than assumed to have them: breaking
# the permission normalisation fails four assertions, and taking the leaf's
# permissions instead of accumulating across levels fails one.
set -e

here=$(dirname "$0")
root=$here/../..

: "${CXX:=clang++}"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

"$CXX" -std=c++2c -Wall -Wextra -Werror \
    -I "$root/hypervisor/include" \
    "$here/nested-ept-test.cpp" \
    -o "$out/nested-ept-test"

"$out/nested-ept-test"
