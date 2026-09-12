#!/usr/bin/env python3
"""Turns `rig-dump-state.py`'s instruction trace into disassembly.

The trace is a list of guest instruction pointers with 32 bytes of code
for each distinct one, because there is no copy of `ntoskrnl.exe` or
`securekernel.exe` anywhere in this tree and an address on its own says
nothing.  This reassembles the two halves: the ordered addresses give the
path taken, the bytes give what each one is.

Only the first instruction of each block is kept.  Every address in the
trace is an instruction start by construction - the monitor trap flag
exits after a retired instruction - so decoding further into the block
would be decoding bytes the guest may never have executed as
instructions, which is exactly what makes a disassembly of a fixed window
unreliable.

`llvm-mc --disassemble` rather than `llvm-objdump`: objdump needs an
object file and has no raw-binary mode, and building one per address to
print one instruction is a lot of machinery for a decoder that already
exists.  What llvm-mc does not know is *where* the bytes came from, so
rip-relative operands are resolved here from the address and the
instruction's own length, which its `--show-encoding` output gives.

Usage:

    scripts/rig-dump-state.py --cpus 1 > dump.txt
    scripts/disassemble-trace.py dump.txt \\
        --base securekernel=0xfffff80278041000 \\
        --base ntoskrnl=0xfffff802e1200000

The bases come from the `caller` lines of the same dump, and naming them
is what makes an address comparable across boots - nothing else in the
guest is at a stable address.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys


def llvm_mc():
    for candidate in (os.environ.get("ZPP_LLVM_MC"), "llvm-mc",
                      "/opt/homebrew/opt/llvm/bin/llvm-mc",
                      "/usr/local/opt/llvm/bin/llvm-mc"):
        if not candidate:
            continue
        try:
            subprocess.run([candidate, "--version"], capture_output=True)
            return candidate
        except OSError:
            continue
    sys.exit("no llvm-mc on PATH; set ZPP_LLVM_MC")


def decode_one(args):
    """The first instruction of one block, as (text, length)."""
    tool, raw = args

    out = subprocess.run(
        [tool, "--disassemble", "--triple=x86_64", "--show-encoding"],
        input=" ".join(f"0x{b:02x}" for b in raw),
        capture_output=True, text=True).stdout

    for line in out.split("\n"):
        m = re.match(r"^\s*(.+?)\s*# encoding: \[(.*)\]\s*$", line)
        if not m:
            continue
        encoding = bytes(int(b, 16) for b in m.group(2).split(",") if b)
        return (m.group(1).replace("\t", " "), len(encoding))

    return None


def decode(tool, blocks):
    """One instruction per block, as (text, length), keyed by address.

    **One process per block, and batching them is not available.**
    llvm-mc decodes its input as one flat stream, does not restart at a
    newline, and skips bytes it cannot decode without saying where - so
    neither a separator nor an offset survives a block that ends
    mid-instruction. Measured: a block ending that way swallowed the
    next block's first bytes as an immediate and reported `movq
    %gs:1208684304, %rax` for what was a separator plus a `movq`, and
    padding the blocks apart instead left every offset after the first
    warning wrong.

    A block is a hundred bytes of input, so this is a few seconds of
    fork for a trace of two thousand steps, in a tool that is read once
    per boot.
    """
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        answers = pool.map(decode_one,
                           ((tool, raw) for raw in blocks.values()))

    return {address: answer
            for address, answer in zip(blocks, answers)
            if answer is not None}


def resolve_rip(text, address, length):
    """Absolute target of a rip-relative operand, appended as a note."""
    m = re.search(r"(-?0x[0-9a-f]+)\(%rip\)", text)
    if not m:
        return text
    target = address + length + int(m.group(1), 16)
    return f"{text}   # 0x{target:x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--base", action="append", default=[],
                    metavar="NAME=0xADDR",
                    help="name an image so addresses print module-relative")
    args = ap.parse_args()

    bases = []
    for entry in args.base:
        name, _, value = entry.partition("=")
        bases.append((name, int(value, 16)))
    bases.sort(key=lambda kv: -kv[1])

    def name_of(address):
        for name, base in bases:
            if address >= base:
                return f"{name}+0x{address - base:x}"
        return f"0x{address:016x}"

    tool = llvm_mc()

    section, order, code, titles = None, {}, {}, {}

    for line in open(args.dump):
        line = line.rstrip("\n")

        m = re.match(r"^--- instruction trace (.+?): (.*) ---$", line)
        if m:
            section = m.group(1)
            order[section] = []
            code[section] = {}
            titles[section] = m.group(2)
            continue

        if section is None:
            continue

        m = re.match(r"^    0x([0-9a-f]{16})  cr3 0x([0-9a-f]+)\s+"
                     r"([0-9a-f]+)(?:  x(\d+))?$", line)
        if m:
            address = int(m.group(1), 16)
            order[section].append((address,
                                   int(m.group(2), 16),
                                   int(m.group(4) or 1)))
            code[section][address] = bytes.fromhex(m.group(3))
            continue

        if line.startswith("---") or line.startswith("cpu "):
            section = None

    for name, steps in order.items():
        if not steps:
            continue

        blocks = {a: b for a, b in code[name].items() if any(b)}
        decoded = decode(tool, blocks)

        print(f"\n=== {name}: {titles[name]} ===")
        for address, cr3, repeats in steps:
            if address in decoded:
                text, length = decoded[address]
                shown = resolve_rip(text, address, length)
            else:
                shown = "<not captured>"
            times = f"   x{repeats}" if repeats > 1 else ""
            print(f"  {name_of(address):<26} cr3 {cr3:#010x}  "
                  f"{shown}{times}")


if __name__ == "__main__":
    main()
