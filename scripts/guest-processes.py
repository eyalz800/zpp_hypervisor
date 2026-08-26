#!/usr/bin/env python3
"""Enumerate the second-level guest's processes, live, without a rebuild.

    scripts/guest-processes.py <kernel-base> <windows-cr3>

Both arguments come from this VMM's own log line, which prints them
together:

    second-level guest kernel image at 0xfffff8018ca00000, cr3 0x1ae002

Why the physical walk rather than the monitor's own `x`: the monitor
translates through whichever processor is currently selected, and on
this rig that processor is usually inside the guest hypervisor's
address space rather than Windows'. `x` then answers "Cannot access
memory", which is a fact about the mapping and not about the guest.
`xp` plus a walk of Windows' own CR3 works whatever the processor is
doing, including on a guest frozen at `paused (shutdown)`.

The two offsets are read out of ntkrnlmp.pdb rather than guessed -
`llvm-pdbutil dump --types`, LF_MEMBER ActiveProcessLinks and
ImageFileName - and they are per build, so re-read them if the guest's
Windows changes.

What it is for: "did we reach the login screen" has no other answer on
this rig. The display is a passed-through GPU, so QEMU refuses a
screendump, and exit counters cannot tell a booted system from a
spinning one. A run reaching only System, Secure System, Registry and
smss.exe has stalled in Phase 1 however many exits it has taken.
"""

import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446

def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12); time.sleep(0.4)
    for c in cmds:
        s.sendall((c+'\n').encode()); time.sleep(0.3)
    time.sleep(1.2); s.setblocking(False); out=b''
    try:
        while True:
            b=s.recv(65536)
            if not b: break
            out+=b
    except Exception: pass
    s.close()
    d=out.decode('utf-8','replace')
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',d).replace('\x1b','')

def xp_q(phys, n=1):
    d = monitor([f'xp /{n}xg 0x{phys:x}'])
    return [int(x,16) for x in re.findall(r'0x([0-9a-f]{16})', d)]

def xp_b(phys, n):
    d = monitor([f'xp /{n}xb 0x{phys:x}'])
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})', d))

CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
def v2p(va):
    t = CR3
    for lvl, sh in ((0,39),(1,30),(2,21),(3,12)):
        e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
        if not e or not (e[0] & 1): return None
        if lvl < 3 and (e[0] & 0x80):
            mask = (1 << sh) - 1
            return (e[0] & 0x000fffffffe00000 & ~mask) | (va & mask) if sh==21 else \
                   (e[0] & 0x000fffffc0000000) | (va & mask)
        t = e[0] & 0x000ffffffffff000
    return t | (va & 0xfff)

base = int(sys.argv[1], 16)
head = base + 0xf05c60
LINKS, NAME = 472, 824
p = v2p(head)
if p is None: print('PsActiveProcessHead not mapped'); sys.exit(1)
first = xp_q(p)
if not first: print('read failed'); sys.exit(1)
cur, seen, names = first[0], set(), []
for _ in range(60):
    if not cur or cur in seen or cur == head: break
    seen.add(cur)
    np_ = v2p(cur - LINKS + NAME)
    if np_:
        n = xp_b(np_, 16).split(b'\x00')[0].decode('ascii','replace')
        if n: names.append(n)
    lp = v2p(cur)
    if lp is None: break
    nxt = xp_q(lp)
    if not nxt: break
    cur = nxt[0]
print(f'{len(names)} processes: ' + ', '.join(names))
