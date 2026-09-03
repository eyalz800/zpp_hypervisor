#!/usr/bin/env python3
"""Scan one guest thread's kernel stack and print the frames on it.

    scripts/guest-stack.py <kernel-base> <windows-cr3> <ETHREAD>

Complements `guest-threads.py`, which says *which* thread is blocked or
running; this says *what it is doing*. Point it at an _ETHREAD from that
listing.

**A running thread's live RSP is in the processor, not in the
_KTHREAD**, so for one of those there is nothing to follow - the only
recoverable evidence is the stack region itself. This walks
StackLimit..StackBase and keeps every word that lands in ntoskrnl's
text. That recovers the call chain approximately and in address order,
which is enough to name a loop, and it is emphatically **not an
unwind**: a scan cannot tell a live return address from a dead one left
by an earlier deeper call. Frames are printed with `~` for that reason
and must be reported that way.

For a switched-out thread (state 5, Waiting) `KernelStack` points at the
switch frame and the top of the scan is trustworthy; for a running one
(state 2) treat the whole thing as a candidate set.
"""

import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446

K_INITIAL_STACK = 0x28              # _KTHREAD.InitialStack
K_STACK_LIMIT   = 0x30              # _KTHREAD.StackLimit
K_KERNEL_STACK  = 0x58              # _KTHREAD.KernelStack
K_STATE         = 388               # _KTHREAD.State


class Mon:
    def __init__(self):
        self.s = socket.create_connection((RIG, PORT), timeout=15)
        self.s.settimeout(10)
        time.sleep(0.4)
        try:
            self.s.recv(1 << 20)
        except Exception:
            pass

    def cmd(self, c):
        self.s.sendall((c + '\n').encode())
        buf, t = b'', time.time()
        while time.time() - t < 10:
            try:
                d = self.s.recv(1 << 20)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            if buf.rstrip().endswith(b'(qemu)'):
                break
        d = re.sub(rb'\x1b\[[0-9;]*[A-Za-z]|[\x08\x0d]', b'',
                   buf).decode('utf-8', 'replace')
        i = d.find(c)                # drop the monitor's echo of the command
        return d[i + len(c):] if i >= 0 else d


_M = Mon()


def xp_q(phys, n=1):
    d = _M.cmd(f'xp /{n}xg 0x{phys:x}')
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', d)]


CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
_E = {}


def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _E.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
            if not e:
                return None
            e0 = e[0]
            _E[key] = e0
        if not (e0 & 1):
            return None
        if lvl < 3 and (e0 & 0x80):
            mask = (1 << sh) - 1
            return (e0 & ~mask & 0x000fffffffffffff) | (va & mask)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)


def rq(va, n=1):
    p = v2p(va)
    return xp_q(p, n) if p is not None else []


BASE = int(sys.argv[1], 16)
ETH = int(sys.argv[3], 16)
LO, HI = BASE, BASE + 0x1000000


def nm(v):
    return f'ntoskrnl+0x{v - BASE:x}' if LO <= v < HI else f'0x{v:x}'


st = rq(ETH + K_STATE)
init = rq(ETH + K_INITIAL_STACK)
lim = rq(ETH + K_STACK_LIMIT)
ks = rq(ETH + K_KERNEL_STACK)
state = (st[0] & 0xff) if st else None
print(f'_ETHREAD 0x{ETH:x}  state {state}  '
      f'InitialStack 0x{(init or [0])[0]:x}  StackLimit 0x{(lim or [0])[0]:x}  '
      f'KernelStack 0x{(ks or [0])[0]:x}')

top = (init or [0])[0]
bot = (lim or [0])[0]
if not (top and bot and bot < top and top - bot < (1 << 20)):
    print('stack bounds implausible - not scanning')
    sys.exit(1)

# Scan low (deepest/most recent) to high (outermost), 32 qwords a read.
seen, frames = set(), []
addr = bot
while addr < top:
    n = min(32, (top - addr) // 8)
    w = rq(addr, n)
    if not w:
        addr += n * 8
        continue
    for i, v in enumerate(w):
        if LO <= v < HI and v not in seen:
            seen.add(v)
            frames.append((addr + i * 8, v))
    addr += n * 8

print(f'{len(frames)} distinct ntoskrnl words on the stack '
      f'(~SCAN, not an unwind - deepest first):')
for at, v in frames:
    print(f'  ~ 0x{at:x}  {nm(v)}')
