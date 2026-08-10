#!/bin/sh
# Differential harness for the instruction decoder.
#
#   harness.cpp      generates a corpus of instructions as assembly text,
#                    assembles it with llvm-mc, disassembles it with
#                    llvm-objdump, and compares what
#                    zpp/arch/x86_64/instruction.h decodes out of the
#                    resulting bytes against what LLVM says they are.
#
# There is no shim directory and no extracted source: instruction.h and
# context.h are freestanding headers that depend on nothing but <cstdint>,
# <optional> and <span>, so the header under test is compiled here exactly
# as the hypervisor compiles it.
#
# The corpus is generated, assembled and parsed by the harness itself
# rather than by this script, so every entry keeps its expectations in the
# same object that carries its bytes. This script only finds the tools and
# hands their names over in the environment.
#
# An earlier round of this comparison was run and thrown away, and its
# result - ten thousand accepted instructions with no length mismatch -
# had to be taken on trust afterwards because nothing was left to re-run.
# That is why this one is committed.
#
# Never run clang-format over this file. It reflows a shell script as C++
# and destroys it, exactly as CLAUDE.md records for Markdown.
set -e
cd "$(dirname "$0")"
SRC=${SRC:-"$(cd ../.. && pwd)/hypervisor"}

LLVM_MC=${LLVM_MC:-llvm-mc}
LLVM_OBJDUMP=${LLVM_OBJDUMP:-llvm-objdump}

# Homebrew keeps its LLVM off the default PATH, and the toolchain this
# tree cross-compiles with is exactly that one - so ask brew where it is
# rather than writing the prefix down, which would be an absolute path
# that is wrong on any other machine.
if ! command -v "$LLVM_MC" >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
        PREFIX=$(brew --prefix llvm 2>/dev/null || true)
        if [ -n "$PREFIX" ] && [ -x "$PREFIX/bin/llvm-mc" ]; then
            LLVM_MC="$PREFIX/bin/llvm-mc"
            LLVM_OBJDUMP="$PREFIX/bin/llvm-objdump"
        fi
    fi
fi

if ! command -v "$LLVM_MC" >/dev/null 2>&1; then
    echo "llvm-mc not found - set LLVM_MC and LLVM_OBJDUMP" >&2
    exit 1
fi

clang++ -std=c++2c -g -O0 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -I "$SRC/include" harness.cpp -o decoder

LLVM_MC="$LLVM_MC" LLVM_OBJDUMP="$LLVM_OBJDUMP" exec ./decoder
