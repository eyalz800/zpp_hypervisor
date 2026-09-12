#!/usr/bin/env python3
"""Symbolize an MTF instruction trace, and prove the base by byte identity.

The rule this exists to enforce: a nearest-symbol lookup against the wrong
module answers with plausible fabricated names, and that cost this project a
day.  So every address is checked against the image's own bytes - the trace
records the instruction bytes, and the image says what is at that offset.  A
base that is wrong matches nothing, loudly.
"""
import re, sys, subprocess, bisect, collections

SEC_CACHE, SYM_CACHE = {}, {}


def sections(exe):
    """Read the PE section table directly.

    Was `llvm-readobj --sections` scraped with a regex, which parsed to an
    EMPTY list on these images - so every rva lookup returned None and every
    byte check reported "mismatch" against a check that had never run.  A
    verifier that fails open is worse than none, so this parses the header.
    """
    if exe in SEC_CACHE:
        return SEC_CACHE[exe]
    import struct
    data = open(exe, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[pe:pe + 4] == b"PE\0\0", exe
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    tab = pe + 24 + opt
    secs = []
    for i in range(nsec):
        e = tab + 40 * i
        vsz, va, rsz, ptr = struct.unpack_from("<IIII", data, e + 8)
        if rsz:
            secs.append((va, ptr, min(rsz, vsz if vsz else rsz)))
    SEC_CACHE[exe] = (secs, data)
    return SEC_CACHE[exe]


def bytes_at(exe, rva, n):
    secs, data = sections(exe)
    for v, p, sz in secs:
        if v <= rva < v + sz:
            o = p + rva - v
            return data[o:o + n]
    return None


def publics(pdb):
    if pdb in SYM_CACHE:
        return SYM_CACHE[pdb]
    def run(a):
        return subprocess.run(a, capture_output=True).stdout.decode(
            "utf-8", "replace")
    rv = []
    for blk in run(["llvm-pdbutil", "dump", "--section-headers",
                    pdb]).split("SECTION HEADER #")[1:]:
        v = re.search(r"([0-9A-Fa-f]+) virtual address", blk)
        if v:
            rv.append(int(v.group(1), 16))
    lines = run(["llvm-pdbutil", "dump", "--publics", pdb]).split("\n")
    syms = []
    for i, l in enumerate(lines):
        m = re.search(r"S_PUB32 \[size = \d+\] `([^`]+)`", l)
        if not m or i + 1 >= len(lines):
            continue
        a = re.search(r"addr = (\d{4}):(\d+)", lines[i + 1])
        if not a:
            continue
        # BOTH fields are decimal. llvm-pdbutil zero-pads the segment to
        # four digits, which reads like hex and is not: this parsed it
        # base 16 and the two agree only for segments 1-9.
        #
        # Measured on the rig's own ntkrnlmp.pdb (36 sections). Under the
        # hex reading, segment 0010 (PAGELK) resolved into 0016
        # (TRACESUP), 0024 (INIT) into 0036 (.reloc), and every segment
        # from 0026 up - .data, ALMOSTRO, PAGEDATA, INITDATA, CFGRO -
        # parsed above 36 and was dropped by the range test below, so
        # those symbols were silently absent rather than wrong.
        #
        # The check that settles it, and it needs no rig: every segment's
        # largest public offset must fit inside that section. Decimal fits
        # all 29 segments present; hex fails six outright. Independently,
        # PsLoadedModuleList is seg 0026 off 1004496, which is 0xef53d0
        # read decimal - the RVA scripts/guest-modules.py has hardcoded
        # and walks successfully - and unresolvable read hex.
        sg, off = int(a.group(1), 10), int(a.group(2))
        # Segment len(rv) + 1 with offset 0xffffffff is the absolute /
        # unmapped sentinel, not a section. It fails the test below.
        if 1 <= sg <= len(rv):
            syms.append((rv[sg - 1] + off, m.group(1)))
    syms.sort()
    SYM_CACHE[pdb] = (syms, [s[0] for s in syms])
    return SYM_CACHE[pdb]


def parse(path, header):
    txt = open(path, errors="replace").read()
    if header not in txt:
        return []
    part = txt.split(header)[1].split("--- instruction trace")[0]
    return [(int(m.group(1), 16), m.group(2)) for m in re.finditer(
        r"\s+0x([0-9a-f]{16})\s+cr3 \S+\s+(\S+)", part)]


def report(rows, mods, label):
    """mods: list of (name, base, size, exe, pdb)."""
    print(f"\n=== {label}: {len(rows)} steps ===")
    hit = collections.Counter()
    ok = collections.Counter()
    bad = collections.Counter()
    order, counts = [], collections.Counter()
    for addr, bs in rows:
        blob = bytes.fromhex(bs)
        for name, base, size, exe, pdb in mods:
            if not (base <= addr < base + size):
                continue
            hit[name] += 1
            got = bytes_at(exe, addr - base, len(blob))
            if got is not None and got == blob:
                ok[name] += 1
            else:
                bad[name] += 1
            syms, keys = publics(pdb)
            j = bisect.bisect_right(keys, addr - base) - 1
            f = syms[j][1] if j >= 0 else "?"
            counts[(name, f)] += 1
            if (name, f) not in order:
                order.append((name, f))
            break
    for name, _, _, _, _ in mods:
        if hit[name]:
            print(f"  {name:<16} {hit[name]:>5} steps   "
                  f"bytes match {ok[name]}, mismatch {bad[name]}")
    unk = len(rows) - sum(hit.values())
    if unk:
        print(f"  {'(unmapped)':<16} {unk:>5} steps")
    print("  order of first appearance:")
    for k in order[:30]:
        print(f"    {k[0]:<14} {k[1]:<44} {counts[k]:>5}")
    return counts


def derive_base(exe, rows):
    """Solve for the load base from the trace's own instruction bytes.

    Never assume a base from a known offset: deriving securekernel as
    `rows[0] - SkpReturnFromNormalMode` silently produced a wrong base the
    moment a trace started somewhere else, and every symbol printed under it
    was fabricated.  A unique byte run pins the base or nothing does.
    """
    secs, data = sections(exe)
    votes = collections.Counter()
    for addr, bs in rows:
        blob = bytes.fromhex(bs)
        i = data.find(blob)
        if i < 0 or data.find(blob, i + 1) >= 0:
            continue
        for v, p, sz in secs:
            if p <= i < p + sz:
                votes[addr - (v + i - p)] += 1
                break
    if not votes:
        return None, 0
    base, n = votes.most_common(1)[0]
    return (base, n) if n >= 8 else (None, n)


if __name__ == "__main__":
    path = sys.argv[1]
    allrows = []
    for hdr in ("--- instruction trace after HvCallVtlCall",
                "--- instruction trace after HvCallVtlReturn",
                "--- instruction trace free-running"):
        allrows += parse(path, hdr)
    SK, skn = derive_base("syms/securekernel.exe", allrows)
    NT, ntn = derive_base("syms/ntoskrnl.exe", allrows)
    NTSZ = 0x1450000
    mods = [m for m in
            [("securekernel", SK, 0x160000, "syms/securekernel.exe",
              "syms/securekernel.pdb"),
             ("ntoskrnl", NT, NTSZ, "syms/ntoskrnl.exe",
              "syms/ntkrnlmp.pdb")] if m[1]]
    print(f"securekernel base {SK and hex(SK)} ({skn} votes)   "
          f"ntoskrnl base {NT and hex(NT)} ({ntn} votes)")
    for hdr, lbl in [
        ("--- instruction trace after HvCallVtlCall", "after VtlCall"),
        ("--- instruction trace after HvCallVtlReturn", "after VtlReturn"),
        ("--- instruction trace free-running", "free-running"),
    ]:
        rows = parse(path, hdr)
        if rows:
            report(rows, mods, lbl)
