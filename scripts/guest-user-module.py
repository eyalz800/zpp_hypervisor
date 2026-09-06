r"""Place a user-mode address in a module, by walking a process's own PEB.

**This exists because the tree has named two innocent drivers this
session.** `VBoxSup.sys` came from a stack that had already returned;
`\Driver\IntcAudioBus` came from a watchdog that names whichever driver
happens to hold an IRP when a flat 600-second timer expires. Both looked
like findings. A third name - the module holding the user-mode tight loop
at `0x7ffe0eb132e5` - is not going into the record without the walk that
proves it.

The census that found that address samples the whole guest and records no
process, so "services.exe is spinning" is a hypothesis. This turns an
address into a module name, which is the first thing that could make it a
fact.

WHY A PROCESS'S OWN ADDRESS SPACE IS NEEDED
-------------------------------------------
The kernel's `PsLoadedModuleList` lists drivers, not user DLLs.
User modules live in the PEB, which is mapped only in that process's
address space - so the walk uses `_KPROCESS.DirectoryTableBase`
(`+0x28`, PDB-verified) as cr3, not the system cr3 every other script
here passes.

OFFSETS
-------
PDB-verified against `ntkrnlmp.pdb`:

    _EPROCESS.UniqueProcessId   464 = 0x1d0
    _EPROCESS.Peb               736 = 0x2e0
    _KPROCESS.DirectoryTableBase 40 = 0x28

Standard x64 user-mode layout, NOT from this PDB and labelled as such -
they are the same offsets `guest-loading-driver.py` already uses for the
kernel's `_KLDR_DATA_TABLE_ENTRY`, which is the same shape:

    _PEB.Ldr                              0x18
    _PEB_LDR_DATA.InMemoryOrderModuleList 0x20
    entry (from InMemoryOrderLinks, so subtract 0x10):
      DllBase 0x30   SizeOfImage 0x40   BaseDllName 0x58

READER PROOF - and nothing prints unless it passes
--------------------------------------------------
The first module on `InMemoryOrderModuleList` is the executable itself.
So walking `services.exe` must yield `services.exe` as entry one. If it
does not, the PEB, the cr3 or the offsets are wrong and every name below
would be invented. The walk refuses rather than reporting.

usage: guest-user-module.py <kernel_base_hex> <system_cr3_hex> <process> [addr...]
"""
import re
import socket
import sys
import time

RIG, PORT = '192.168.1.199', 4446

PSACTIVEPROCESSHEAD = 0xf05c60
E_LINKS, E_NAME, E_PID, E_PEB = 472, 824, 0x1d0, 0x2e0
K_DIRBASE = 0x28

PEB_LDR = 0x18
LDR_INMEMORY = 0x20
M_DLLBASE, M_SIZE, M_NAME = 0x30, 0x40, 0x58
WALK_LIMIT = 256


def _rows(d):
    return [l for l in d.splitlines()
            if re.match(r'^[0-9a-f]{6,}: ', l.strip())]


def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12)
    time.sleep(0.35)
    for c in cmds:
        s.sendall((c + '\n').encode())
        time.sleep(0.30)
    time.sleep(1.1)
    s.setblocking(False)
    out = b''
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    except Exception:
        pass
    s.close()
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '',
                  out.decode('utf-8', 'replace'))


def _vals(cmd, width):
    out = []
    for l in _rows(monitor([cmd])):
        body = l.split(':', 1)[1]
        out += [int(x, 16)
                for x in re.findall(r'0x([0-9a-f]{%d})' % width, body)]
    return out


def xp_q(p, n=1):
    return _vals(f'xp /{n}xg 0x{p:x}', 16)


def xp_b(p, n):
    return _vals(f'xp /{n}xb 0x{p:x}', 2)


BASE = int(sys.argv[1], 16)
SYSCR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
WANT = sys.argv[3]
QUERY = [int(a, 16) for a in sys.argv[4:]]


class Space:
    """One address space. The cache is per-space - sharing it across two
    different cr3 values is how a walk silently reads the wrong tables."""

    def __init__(self, cr3):
        self.cr3 = cr3 & 0x000ffffffffff000
        self.e = {}

    def v2p(self, va):
        t = self.cr3
        for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
            k = (t, (va >> sh) & 0x1ff)
            v = self.e.get(k)
            if v is None:
                got = xp_q(t + ((va >> sh) & 0x1ff) * 8)
                if not got:
                    return None
                v = got[0]
                self.e[k] = v
            if not (v & 1):
                return None
            if lvl < 3 and (v & 0x80):
                m = (1 << sh) - 1
                return (v & ~m & 0x000fffffffffffff) | (va & m)
            t = v & 0x000ffffffffff000
        return t | (va & 0xfff)

    def q(self, va):
        p = self.v2p(va)
        if p is None:
            return None
        got = xp_q(p)
        return got[0] if got else None

    def raw(self, va, n):
        p = self.v2p(va)
        return None if p is None else xp_b(p, n)

    def wstr(self, va):
        hdr = self.q(va)
        buf = self.q(va + 8)
        if hdr is None or not buf:
            return None
        ln = hdr & 0xffff
        if not ln or ln > 512:
            return None
        b = self.raw(buf, min(ln, 96))
        if not b:
            return None
        return ''.join(chr(b[i]) for i in range(0, len(b) - 1, 2)
                       if 32 <= b[i] < 127)

    def astr(self, va, n=16):
        b = self.raw(va, n)
        if not b:
            return ''
        return ''.join(chr(c) for c in b if 32 <= c < 127)


sysspace = Space(SYSCR3)
print(f'kernel base 0x{BASE:x}  system cr3 0x{SYSCR3:x}')

head = BASE + PSACTIVEPROCESSHEAD
cur = sysspace.q(head)
first = True
target = None
while cur and cur != head:
    ep = cur - E_LINKS
    name = sysspace.astr(ep + E_NAME)
    if first:
        if name != 'System':
            print(f'  READER NOT PROVEN: first process is {name!r}, '
                  f'expected System. Nothing below is printed.')
            sys.exit(1)
        print('  anchor: first entry of PsActiveProcessHead is `System`')
        first = False
    if WANT.lower() in name.lower():
        target = (ep, name)
        break
    cur = sysspace.q(cur)

if not target:
    print(f'  {WANT!r} not found in the process list')
    sys.exit(1)

ep, name = target
peb = sysspace.q(ep + E_PEB)
dirbase = sysspace.q(ep + K_DIRBASE)
print(f'\n{name}  _EPROCESS 0x{ep:x}  PEB {peb and hex(peb)}  '
      f'DirectoryTableBase {dirbase and hex(dirbase)}')
if not peb or not dirbase:
    print('  no PEB or no DirectoryTableBase - the process has exited or '
          'is a system process with no user address space.')
    sys.exit(1)

# The process's OWN space. A separate Space, deliberately - see the class.
proc = Space(dirbase)
ldr = proc.q(peb + PEB_LDR)
if not ldr:
    print('  PEB unreadable in the process address space. Either '
          'DirectoryTableBase is wrong or the PEB is paged out; both are '
          'read failures, not "no modules".')
    sys.exit(1)

lhead = ldr + LDR_INMEMORY
cur = proc.q(lhead)
mods, n, seen = [], 0, set()
while cur and cur != lhead and n < WALK_LIMIT and cur not in seen:
    seen.add(cur)
    e = cur - 0x10                      # InMemoryOrderLinks is at +0x10
    b = proc.q(e + M_DLLBASE)
    sz = proc.q(e + M_SIZE)
    nm = proc.wstr(e + M_NAME)
    if b:
        mods.append((b, (sz or 0) & 0xffffffff, nm or '<unnamed>'))
    n += 1
    cur = proc.q(cur)

if not mods:
    print('  no modules walked')
    sys.exit(1)

# PROOF: the first module on InMemoryOrderModuleList is the image itself.
head_name = mods[0][2].lower()
want_exe = name.lower().rstrip('\x00')
ok = want_exe.split('.')[0] in head_name
print(f'  PROOF: first module is {mods[0][2]!r}, expected {name!r} -> '
      f'{"PASS" if ok else "FAIL"}')
if not ok:
    print('  -> the PEB, the cr3 or the offsets are wrong. Every name '
          'below would be invented. Refusing.')
    sys.exit(1)

print(f'\n{len(mods)} modules:')
for b, sz, nm in mods[:40]:
    print(f'  0x{b:012x} + 0x{sz:<8x}  {nm}')

for a in QUERY:
    hit = [(b, sz, nm) for b, sz, nm in mods if b <= a < b + sz]
    if hit:
        b, sz, nm = hit[0]
        print(f'\n  0x{a:012x}  ->  **{nm}** + 0x{a - b:x}  '
              f'(base 0x{b:x}, size 0x{sz:x})')
    else:
        print(f'\n  0x{a:012x}  ->  in NO module of this process. '
              f'Either the address belongs to another process, or it is '
              f'private memory with no module behind it.')
