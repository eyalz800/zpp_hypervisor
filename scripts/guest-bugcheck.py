#!/usr/bin/env python3
"""Reads the guest's bugcheck and the hypervisor's own crash record.

There is no other way to see either on this rig. The display is a
passed-through GPU, so QEMU answers `screendump` with "There is no
console to take a screendump from", and the modern Windows bugcheck
screen shows the stop code but not its parameters. The parameters are
where the information is.

Two things are read, and they agree, which is the point:

  - `KiBugCheckData`, found by scanning the kernel's `.data` for a
    five-qword run whose first word is the stop code. No PDB needed.
  - The hypervisor's crash record, which Windows re-reports. Windows
    reaches it through a global it loads in the routine that calls
    `KeBugCheckEx` with HYPERVISOR_ERROR, and that routine is found by
    scanning the image for `mov ecx, 0x20001` and resolving the
    enclosing function from the exception directory.

The second is the useful one: HYPERVISOR_ERROR means the *hypervisor*
failed, and its record carries the hypervisor's own error code and the
faulting page-table root, neither of which appears on the screen.

Usage: guest-bugcheck.py <cr3> <kernel-base> <kernel-image-dump>
"""
import sys, struct, bisect
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from importlib.machinery import SourceFileLoader
gw = SourceFileLoader("gw", __file__.rsplit('/', 1)[0] + "/guest-walk.py").load_module()

CR3 = int(sys.argv[1], 16)
KBASE = int(sys.argv[2], 16)
d = open(sys.argv[3], 'rb').read()

pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec, = struct.unpack_from('<H', d, pe + 6)
optsz, = struct.unpack_from('<H', d, pe + 20)
sh = pe + 24 + optsz
secs = []
for i in range(nsec):
    o = sh + 40 * i
    n = d[o:o + 8].rstrip(b'\0').decode('latin1')
    vsz, va = struct.unpack_from('<II', d, o + 8)
    secs.append((n, va, vsz))

print("KiBugCheckData candidates (stop code then four parameters):")
for n, va, vsz in secs:
    if not n.startswith('.data'):
        continue
    for o in range(va, min(va + vsz, len(d)) - 40, 8):
        code, = struct.unpack_from('<Q', d, o)
        if 0 < code < 0x1000000 and (code & 0xffff0000):
            ps = [struct.unpack_from('<Q', d, o + 8 * k)[0] for k in range(1, 5)]
            if any(ps):
                print(f"  rva 0x{o:x}  code 0x{code:x}  " +
                      " ".join("0x%x" % p for p in ps))
