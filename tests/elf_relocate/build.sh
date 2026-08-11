#!/bin/sh
# Builds and runs the ELF relocation regression harness.
#
# There is no shim directory and no extracted source: zpp/elf_file.h is a
# freestanding header that depends on <algorithm>, <cstddef>, <cstdint>,
# <tuple>, <type_traits> and <variant> and on nothing else, so the header
# under test compiles here exactly as the loader compiles it. Same reason
# tests/decoder needs no shim.
#
# Native, no emulator, no target - and that is the point. The bug this
# pins made every relocated slot come out as the module base, and the
# cheapest possible way to catch it is to relocate an image in host memory
# and read the words back.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e
cd "$(dirname "$0")"
SRC=${SRC:-"$(cd ../.. && pwd)/hypervisor"}

: "${CXX:=clang++}"

"$CXX" -std=c++2c -g -O0 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -I "$SRC/include" harness.cpp -o elf_relocate

exec ./elf_relocate
