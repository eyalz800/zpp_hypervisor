#!/usr/bin/env python3
"""Name an address inside the guest's secure kernel.

Why this exists.  Every stall this investigation has reached ends in
securekernel, and this tree has only ever had `ntkrnlmp.pdb`, so the
frames that matter read as bare addresses.  One return address landing
in `HvcallpExtendedFastHypercall` - a wrapper every caller shares -
"names nothing", as `vtl_protect_last_stack`'s own comment puts it.
With symbols the same window reads:

    SkmiExpandPteRange+0x4fe
      SkmiProtectPageRange+0x124
        ShvlpInitiateFastHypercall+0x36
          HvcallpExtendedFastHypercall+0x51

The whole chain is recoverable from a running guest with no rebuild and
nothing perturbed, and the steps are not obvious, so they are here
rather than in a session that scrolls away.

  1. The secure kernel runs on its own address space.  `cr3` for it is
     printed beside every protection call in `rig-dump-state.py`
     (`last answer entered cr3 ...`); it has been 0x8800002.
  2. Walk that cr3 to the region holding a known stall address.  The
     page directory entry below the module reads NOT PRESENT, which is
     how the module's 2 MB region is identified without guessing.
  3. Scan that region's page table for the first present page and check
     it for `MZ`.  **The image does not start at the region base** - it
     was 0x21000 into it - so scanning is required, not arithmetic.
  4. Read the PE debug directory, take the type-2 CODEVIEW entry, and
     build the symbol-server key from the RSDS GUID and age.
  5. Download the PDB and map addresses through the section table.

Four traps, all of which produce plausible wrong answers:

  - `llvm-pdbutil dump --publics` prints `addr = SEGMENT:OFFSET` with
    the **offset in decimal**.  Read as hex every symbol lands
    somewhere believable and wrong.  CLAUDE.md records this costing a
    session for `ntkrnlmp.pdb`; it is the same trap here.
  - The segment indexes the PE section table, so an address is only an
    RVA after adding that section's virtual address.  For securekernel
    `.text` is section 1 at RVA 0x1000.
  - **The segment is decimal too, and zero-padded to four digits**, so
    it reads like hex.  `KiProcessorBlock` is `addr = 0027:15488`;
    `ntoskrnl.exe` has **36** sections, so `0x27 = 39` is out of range
    and the symbol looks unresolvable, while `27` decimal is section
    `ALMOSTRO` at RVA 0xfc5000 and `0xfc5000 + 15488 = 0xfc8c80` - the
    value `.references/hyperv/ntkrnlmp_symbols.csv` already had.  This
    script does the arithmetic; do not redo it by hand, which is what
    produced a "38 sections" miscount.
  - **A public symbol is not always a function entry.**  securekernel's
    publics include interior labels: `SkpReturnFromNormalModeRaxSet`
    (RVA 0xd9434) has no `.pdata` row of its own - it is
    `SkCallNormalMode+0x2c4`, inside the range 0xd9170-0xd9687.  So an
    offset can be exact and still not measure from a function start.
    Cross-check anything surprising against
    `.references/hyperv/sk_functions.csv` (`rva_start,rva_end,name`,
    built from real `.pdata`), which settles it in one lookup.

And one rule for disassembling afterwards, since the wrapper recipe in
CLAUDE.md hides it: `llvm-objdump --adjust-vma=N` shifts the printed
instruction addresses but **not** its `# 0x…` resolution of
RIP-relative operands, so

    true RVA = objdump's `#` comment + N

Checked on twenty references in one session - twelve call targets and
eight data loads - and all twenty landed on an exact symbol start once
N was added.  A wrong rule would not hit a symbol boundary even once.

Usage:
    scripts/guest-securekernel-syms.py --pdb securekernel.pdb \\
        --base 0xfffff80615221000 0xfffff806152ff701 ...

    # or give raw RVAs
    scripts/guest-securekernel-syms.py --pdb securekernel.pdb 0xde701
"""
import argparse
import bisect
import re
import subprocess
import sys


def publics(pdb):
    """Public symbols as (section, offset, name), offsets DECIMAL."""
    out = subprocess.run(["llvm-pdbutil", "dump", "--publics", pdb],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        sys.exit(f"llvm-pdbutil failed: {out.stderr.strip()[:200]}")
    syms, name = [], None
    for line in out.stdout.splitlines():
        m = re.search(r"S_PUB32 \[size = \d+\] `([^`]+)`", line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r"addr = (\d+):(\d+)", line)
        if m and name:
            syms.append((int(m.group(1)), int(m.group(2)), name))
            name = None
    return syms


def section_rvas(pdb):
    """Section index (1-based) -> virtual address, from the PDB."""
    out = subprocess.run(["llvm-pdbutil", "dump", "--section-headers", pdb],
                         capture_output=True, text=True, errors="replace")
    rvas, index = {}, 0
    for line in out.stdout.splitlines():
        m = re.search(r"SECTION HEADER #(\d+)", line)
        if m:
            index = int(m.group(1))
            continue
        m = re.match(r"\s*([0-9A-Fa-f]+) virtual address", line)
        if m and index:
            rvas[index] = int(m.group(1), 16)
            index = 0
    return rvas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pdb", required=True)
    ap.add_argument("--base", default=None,
                    help="module base; addresses are VAs when given")
    ap.add_argument("addresses", nargs="+")
    args = ap.parse_args()

    base = int(args.base, 16) if args.base else 0
    rvas = section_rvas(args.pdb)
    if not rvas:
        sys.exit("no section headers in the PDB - cannot turn "
                 "segment:offset into an RVA, and guessing would be wrong")

    # One sorted list per section, so a lookup cannot silently pick a
    # symbol from a different section.
    per_section = {}
    for section, offset, name in publics(args.pdb):
        per_section.setdefault(section, []).append((offset, name))
    for values in per_section.values():
        values.sort()

    for text in args.addresses:
        value = int(text, 16)
        rva = value - base
        hit = None
        for section, start in sorted(rvas.items()):
            values = per_section.get(section)
            if not values:
                continue
            offset = rva - start
            if offset < 0 or offset > values[-1][0] + (1 << 20):
                continue
            i = bisect.bisect_right([o for o, _ in values], offset) - 1
            if i >= 0:
                hit = (values[i][1], offset - values[i][0], section)
        if hit:
            print(f"  0x{value:x}  rva 0x{rva:x}  ->  "
                  f"{hit[0]}+0x{hit[1]:x}  (section {hit[2]})")
        else:
            print(f"  0x{value:x}  rva 0x{rva:x}  ->  no symbol below it")


if __name__ == "__main__":
    main()
