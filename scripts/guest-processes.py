#!/usr/bin/env python3
"""Walk the guest's `PsActiveProcessHead` and print each process name.

Answers "what is Windows actually running", which on this rig has no
other answer: the display is a passed-through GPU and `screendump`
returns "There is no console to take a screendump from", so the only way
to tell a machine sitting at the logon UI from one still starting
services is to read its process list.

Offsets are taken from the PDB rather than guessed - `--types` gives
`_EPROCESS.ActiveProcessLinks` at 472 and `ImageFileName` at 824, and
`--publics` gives `PsActiveProcessHead` as segment 26 (.data, VA
0xE00000) offset 1072224 decimal. Pass a different --links/--name if the
guest build changes; a wrong offset prints plausible garbage, so the
walk cross-checks that the first entry is `System`.

usage: guest-processes.py <kernel_base_hex> <cr3_hex> [--head-rva 0xf05c60]
"""
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
LINKS, NAME = 472, 824

def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12); time.sleep(0.35)
    for c in cmds:
        s.sendall((c + '\n').encode()); time.sleep(0.28)
    time.sleep(1.1); s.setblocking(False); out = b''
    try:
        while True:
            b = s.recv(65536)
            if not b: break
            out += b
    except Exception: pass
    s.close()
    d = out.decode('utf-8', 'replace')
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', d).replace('\x1b', '')

def xp_q(phys, n=1):
    d = monitor([f'xp /{n}xg 0x{phys:x}'])
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', d)]

def xp_b(phys, n):
    d = monitor([f'xp /{n}xb 0x{phys:x}'])
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', d)]

BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
HEAD_RVA = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xf05c60
_E = {}

def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _E.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
            if not e: return None
            e0 = e[0]; _E[key] = e0
        if not (e0 & 1): return None
        if lvl < 3 and (e0 & 0x80):
            m = (1 << sh) - 1
            return (e0 & ~m & 0x000fffffffffffff) | (va & m)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)

def rq(va):
    p = v2p(va)
    if p is None: return None
    v = xp_q(p)
    return v[0] if v else None

def rname(va):
    p = v2p(va)
    if p is None: return ''
    bs = xp_b(p, 15)
    return ''.join(chr(c) for c in bs if 32 <= c < 127)

head = BASE + HEAD_RVA
cur = rq(head)
seen, names, why = set(), [], 'ran out of iterations'
for _ in range(400):
    if cur is None: why = 'READ FAILED - the walk is truncated, not the list'; break
    if cur == head: why = 'reached the list head - complete'; break
    if cur in seen: why = 'LOOP - corrupt or torn read'; break
    seen.add(cur)
    eproc = cur - LINKS
    nm = rname(eproc + NAME) or '<unreadable>'
    names.append(nm)
    print(f'  {nm}', flush=True)
    cur = rq(cur)

print(f'{len(names)} processes; walk ended because: {why}')
# A wrong head or offset does not fail loudly, it prints garbage that
# looks like a short process list. `System` is always the first entry of
# this list on Windows, so its absence means the offsets are wrong and
# nothing above should be believed.
if names and names[0] == 'System':
    print('cross-check: first entry is `System` - offsets are right')
else:
    print(f'cross-check FAILED: first entry is {names[:1]}, expected '
          f'`System`. Offsets or head RVA are wrong - do not believe '
          f'the list above.')
