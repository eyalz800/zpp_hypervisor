#!/bin/sh
# Differential harness for the nested VM-exit reflection decision.
#
#   shim/            a stand-in hypervisor.h carrying only what
#                    nested_entry.cpp touches, so the REAL
#                    l0_wants_l2_exit and l1_wants_l2_exit compile
#                    natively on arm64 macOS.
#   ../nested_vmx/shim
#                    the asm.h, vmx/asm.h and diag/log.h shims, shared
#                    with the VMX-instruction harness. This directory
#                    comes first on the include path so its hypervisor.h
#                    wins; everything else falls through to that one.
#   harness.cpp      the fake guest memory, the fake VMCS and the tests.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e
cd "$(dirname "$0")"
SRC=${SRC:-"$(cd ../.. && pwd)/hypervisor"}
clang++ -std=c++2c -g -O0 -Wall -Wextra -fno-exceptions -fno-rtti \
    -DZPP_NESTED_VMX=1 \
    -I shim -I ../nested_vmx/shim -I "$SRC/include" -I . \
    harness.cpp "$SRC/src/hypervisor/nested_entry.cpp" -o nexit
exec ./nexit
