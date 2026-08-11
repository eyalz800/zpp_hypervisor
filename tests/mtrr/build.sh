#!/bin/sh
# Builds and runs the MTRR memory-type derivation harness.
#
# There is no shim directory and no extracted source:
# zpp/arch/x86_64/mtrr.h depends on memory_type.h, msr.h, <cstddef>,
# <cstdint>, <iterator>, <optional> and <type_traits> and on nothing
# else, so the header under test compiles here exactly as the hypervisor
# compiles it. Same reason tests/elf_relocate and tests/decoder need no
# shim.
#
# Native, no emulator and no target - the derivation is pure and
# constexpr, so most of the assertions are made at compile time and the
# compile is itself the test. The runtime half exists so a failure prints
# which address disagreed rather than one static_assert message.
#
# What it pins: 63a5d17, where every range no variable MTRR covered got
# write-back including the MMIO hole above the top of DRAM, and 83012e7,
# where the variable-range array was sized 8 while real Intel client
# parts report VCNT of 10.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e
cd "$(dirname "$0")"
SRC=${SRC:-"$(cd ../.. && pwd)/hypervisor"}

: "${CXX:=clang++}"

"$CXX" -std=c++2c -g -O0 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -I "$SRC/include" harness.cpp -o mtrr

exec ./mtrr
