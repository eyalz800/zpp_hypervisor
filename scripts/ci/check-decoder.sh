#!/bin/sh
# Compiles the decoder's own tests, which are static_asserts, so the
# compile *is* the test run - there is nothing to execute and nothing to
# skip. Hosted rather than freestanding on purpose: the decoder is pure,
# takes a register file and some bytes, and needs no target to check.
set -e

here=$(dirname "$0")
root=$here/../..

: "${CXX:=clang++}"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

"$CXX" -std=c++2c -Wall -Wextra -Werror \
    -I "$root/hypervisor/include" \
    "$here/decoder-test.cpp" \
    -o "$out/decoder-test"

"$out/decoder-test"
