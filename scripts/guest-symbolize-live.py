#!/usr/bin/env python3
"""Names Windows kernel addresses from the image as loaded in guest RAM.

The rig's ntoskrnl.exe is on a passed-through NVMe, so the host cannot
read the file while the guest runs.  The loaded image is reachable
though, and in a loaded image an RVA *is* the offset into the buffer -
which makes this simpler than parsing the on-disk file, not harder.

Exports alone name the wrong function by a mile (BACKLOG).  The exception
directory does not: every function with a stack frame has a
RUNTIME_FUNCTION giving its exact start and end.  Both are used - the
function bounds locate, the nearest preceding export names.
"""
import struct, sys, bisect

d = open(sys.argv[1], 'rb').read()
BASE = int(sys.argv[2], 16)

pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec, = struct.unpack_from('<H', d, pe + 6)
optsz, = struct.unpack_from('<H', d, pe + 20)
opt = pe + 24
magic, = struct.unpack_from('<H', d, opt)
ddir = opt + (112 if magic == 0x20b else 96)

def dirent(i):
    return struct.unpack_from('<II', d, ddir + 8 * i)

sections = []
sh = opt + optsz
for i in range(nsec):
    o = sh + 40 * i
    name = d[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va = struct.unpack_from('<II', d, o + 8)
    sections.append((va, vsz, name))

def section_of(rva):
    for va, vsz, name in sections:
        if va <= rva < va + max(vsz, 1):
            return name
    return '?'

# --- exports -------------------------------------------------------
exp_rva, exp_sz = dirent(0)
exports = []
if exp_rva:
    nfun, nname = struct.unpack_from('<II', d, exp_rva + 20)
    afun, anam, aord = struct.unpack_from('<III', d, exp_rva + 28)
    for i in range(nname):
        nrva, = struct.unpack_from('<I', d, anam + 4 * i)
        o, = struct.unpack_from('<H', d, aord + 2 * i)
        frva, = struct.unpack_from('<I', d, afun + 4 * o)
        end = d.index(b'\0', nrva)
        exports.append((frva, d[nrva:end].decode('latin1')))
exports.sort()
exp_rvas = [e[0] for e in exports]

# --- exception directory (function bounds) -------------------------
pd_rva, pd_sz = dirent(3)
funcs = []
for o in range(pd_rva, pd_rva + pd_sz, 12):
    start, end, unw = struct.unpack_from('<III', d, o)
    if start == 0 and end == 0:
        break
    funcs.append((start, end))
funcs.sort()
starts = [f[0] for f in funcs]

def name(addr):
    rva = addr - BASE
    if not (0 <= rva < len(d)):
        return f'0x{addr:x}  <outside the image>'
    out = f'ntoskrnl+0x{rva:<8x} [{section_of(rva):<8}]'
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and funcs[i][0] <= rva < funcs[i][1]:
        fs, fe = funcs[i]
        out += f'  fn 0x{fs:x}..0x{fe:x} (+0x{rva-fs:x})'
        j = bisect.bisect_right(exp_rvas, fs) - 1
        if j >= 0:
            out += f'  ~{exports[j][1]}+0x{fs-exp_rvas[j]:x}'
    else:
        j = bisect.bisect_right(exp_rvas, rva) - 1
        if j >= 0:
            out += f'  no RUNTIME_FUNCTION  ~{exports[j][1]}+0x{rva-exp_rvas[j]:x}'
    return out

print(f'image {len(d):,} bytes, {len(sections)} sections, '
      f'{len(exports):,} exports, {len(funcs):,} runtime functions')
for a in sys.argv[3:]:
    print(' ', name(int(a, 16)))
