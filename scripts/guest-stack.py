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
        return d


# **Select the data rows rather than subtract the echo.** The old code
# here found the command text and sliced past it, and returned the whole
# buffer *including the echo* whenever `find` missed - a redraw, a split
# echo - with no diagnosis. That is safe only while every regex below is
# `{16}`, because an echoed physical address is 9-12 hex digits: the
# first `xp /Nxw` added here would have read the address as the value.
# `guest-clock.py` shared this idiom and did have such a read.
def _rows(d):
    return '\n'.join(l for l in d.splitlines()
                     if re.match(r'^[0-9a-f]{6,}: ', l.strip()))


_M = Mon()


def xp_q(phys, n=1):
    d = _rows(_M.cmd(f'xp /{n}xg 0x{phys:x}'))
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
# ntoskrnl's SizeOfImage is 0x1450000, not 0x1000000. The old bound was
# short by 4.5 MB - harmless for code, which all sits under +0xc83000, but
# wrong for any "not in ntoskrnl" verdict drawn from it.
LO, HI = BASE, BASE + 0x1450000


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
seen, frames, others = set(), [], []
addr = bot
while addr < top:
    n = min(32, (top - addr) // 8)
    w = rq(addr, n)
    if not w:
        addr += n * 8
        continue
    for i, v in enumerate(w):
        if v in seen:
            continue
        if LO <= v < HI:
            seen.add(v)
            frames.append((addr + i * 8, v))
        elif v >= 0xffff800000000000:
            # **Canonical, and NOT ntoskrnl.** These were previously
            # discarded, which is why a driver's own frames read as
            # "unlabelled" - they were never printed at all. A third-party
            # DriverEntry that does not return leaves its frames here and
            # nowhere else, so dropping them threw away the only evidence
            # of where inside that driver the thread is. Collected
            # separately so the ntoskrnl list above is unchanged.
            seen.add(v)
            others.append((addr + i * 8, v))
    addr += n * 8

print(f'{len(frames)} distinct ntoskrnl words on the stack '
      f'(~SCAN, not an unwind - deepest first):')
for at, v in frames:
    print(f'  ~ 0x{at:x}  {nm(v)}')

# Anything canonical that is not ntoskrnl: drivers, the secure kernel, the
# hypercall page, and plain data that happens to look like a pointer. A
# scan cannot tell code from data, so these are candidates, not frames -
# but the driver's are in here and nowhere else.
if others:
    print(f'\n{len(others)} canonical NON-ntoskrnl words (candidates, not '
          f'frames - a scan cannot tell code from data):')
    for at, v in others[:60]:
        print(f'  ~ 0x{at:x}  0x{v:x}')
    if len(others) > 60:
        print(f'  ... and {len(others) - 60} more')
