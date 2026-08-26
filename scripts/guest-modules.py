#!/usr/bin/env python3
"""List the second-level guest's loaded kernel modules, live.

    scripts/guest-modules.py <kernel-base> <windows-cr3>

Both from this VMM's own log line, which prints them together:

    second-level guest kernel image at 0xfffff8077f200000, cr3 0x1ae002

Walks `PsLoadedModuleList` (RVA 0xef53d0) physically with `xp`, for the
same reason `guest-processes.py` does: the monitor's `x` translates
through whichever processor is selected, and that one is usually inside
the guest hypervisor's address space, where it answers "Cannot access
memory" - a fact about the mapping, not about the guest.

Offsets are from `llvm-pdbutil dump --types` on ntkrnlmp.pdb -
`_LDR_DATA_TABLE_ENTRY.InLoadOrderLinks` 0, `BaseDllName` 88 - and are
per build.

**It caches page-table entries.** Without that each name costs four
monitor round trips and the walk takes longer than a boot; kernel space
maps through very few tables, so the cache turns minutes into seconds.

What it is for: "has the storage stack started" and "did the driver
load" have no other answer here, and both were guessed at - wrongly -
for a long time before this existed.
"""
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12); time.sleep(0.35)
    for c in cmds:
        s.sendall((c+'\n').encode()); time.sleep(0.28)
    time.sleep(1.1); s.setblocking(False); out=b''
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
def xp_w(phys, n):
    d = monitor([f'xp /{n}xh 0x{phys:x}'])
    return [int(x,16) for x in re.findall(r'0x([0-9a-f]{4})', d)]
CR3 = int(sys.argv[2],16) & 0x000ffffffffff000
_ENTRY = {}
def v2p(va):
    t = CR3
    for lvl, sh in ((0,39),(1,30),(2,21),(3,12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _ENTRY.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff)*8)
            if not e: return None
            e0 = e[0]; _ENTRY[key] = e0
        if not (e0 & 1): return None
        if lvl < 3 and (e0 & 0x80):
            mask=(1<<sh)-1; return (e0 & ~mask & 0x000fffffffffffff) | (va & mask)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)
def rq(va):
    p=v2p(va)
    if p is None: return None
    v=xp_q(p); return v[0] if v else None
base=int(sys.argv[1],16); head=base+0xef53d0
cur=rq(head); seen=set(); names=[]
for _ in range(200):
    if not cur or cur==head or cur in seen: break
    seen.add(cur)
    # BaseDllName is a UNICODE_STRING at +88: Length(2) MaxLength(2) pad Buffer(8)
    ln = rq(cur+88)
    buf = rq(cur+88+8)
    nm=''
    if ln is not None and buf:
        n=(ln & 0xffff)//2
        p=v2p(buf)
        if p and 0 < n <= 64:
            ws=xp_w(p, n)
            nm=''.join(chr(w) for w in ws if 32 <= w < 127)
    if nm: names.append(nm); print(' ', nm, flush=True)
    nx=rq(cur)
    if not nx: break
    cur=nx
print(f'{len(names)} modules')
for n in names: print(' ', n)
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12); time.sleep(0.35)
    for c in cmds:
        s.sendall((c+'\n').encode()); time.sleep(0.28)
    time.sleep(1.1); s.setblocking(False); out=b''
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
def xp_w(phys, n):
    d = monitor([f'xp /{n}xh 0x{phys:x}'])
    return [int(x,16) for x in re.findall(r'0x([0-9a-f]{4})', d)]
CR3 = int(sys.argv[2],16) & 0x000ffffffffff000
_ENTRY = {}
def v2p(va):
    t = CR3
    for lvl, sh in ((0,39),(1,30),(2,21),(3,12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _ENTRY.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff)*8)
            if not e: return None
            e0 = e[0]; _ENTRY[key] = e0
        if not (e0 & 1): return None
        if lvl < 3 and (e0 & 0x80):
            mask=(1<<sh)-1; return (e0 & ~mask & 0x000fffffffffffff) | (va & mask)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)
def rq(va):
    p=v2p(va)
    if p is None: return None
    v=xp_q(p); return v[0] if v else None
base=int(sys.argv[1],16); head=base+0xef53d0
cur=rq(head); seen=set(); names=[]
for _ in range(200):
    if not cur or cur==head or cur in seen: break
    seen.add(cur)
    # BaseDllName is a UNICODE_STRING at +88: Length(2) MaxLength(2) pad Buffer(8)
    ln = rq(cur+88)
    buf = rq(cur+88+8)
    nm=''
    if ln is not None and buf:
        n=(ln & 0xffff)//2
        p=v2p(buf)
        if p and 0 < n <= 64:
            ws=xp_w(p, n)
            nm=''.join(chr(w) for w in ws if 32 <= w < 127)
    if nm: names.append(nm); print(' ', nm, flush=True)
    nx=rq(cur)
    if not nx: break
    cur=nx
print(f'{len(names)} modules')
for n in names: print(' ', n)
