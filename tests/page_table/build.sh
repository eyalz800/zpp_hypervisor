#!/bin/sh
# Builds and runs the page table harness: the host page table, the OS
# page table walker, and the paging-structure helpers underneath both.
#
# There is no shim directory and no extracted source.
# zpp/arch/x86_64/page_table.h depends on pte.h, virtual_address.h,
# <cstdint>, <type_traits>, <utility> and <iterator>, os_page_table.h on
# <cstdint> alone, and neither of the two .cpp files reaches an assembly
# wrapper or any hypervisor state - so they compile natively here exactly
# as the hypervisor compiles them. Same reason tests/elf_relocate,
# tests/mtrr and tests/decoder need no shim.
#
# The two translation units are the real ones out of
# hypervisor/src/arch/x86_64/, compiled rather than copied, so a change
# to either is a change to what runs here. If a future page_table starts
# calling invlpg or reading CR3 this build breaks, which is the right
# moment to add a shim rather than to weaken the test.
#
# What it pins, all three found by writing it and none fixed by it:
# a large-page translation shifted nine bits too far in both walkers, an
# unmapped address answered with its own page offset instead of being
# refused, and a protection key read one bit low. The harness asserts
# what the code does today and says so in each message, so a fix turns a
# named check red.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e
cd "$(dirname "$0")"
SRC=${SRC:-"$(cd ../.. && pwd)/hypervisor"}

: "${CXX:=clang++}"

"$CXX" -std=c++2c -g -O0 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -I "$SRC/include" \
    harness.cpp \
    "$SRC/src/arch/x86_64/page_table.cpp" \
    "$SRC/src/arch/x86_64/os_page_table.cpp" \
    -o pagetable

exec ./pagetable
