#!/usr/bin/env python3
"""Sample KUSER_SHARED_DATA's clocks twice and say whether they advance.

    scripts/guest-clock.py <windows-cr3> [seconds]

Answers, in one command and without a rebuild, the question every
"guest is stuck" theory in this tree eventually turns on: **is the
guest's own notion of time moving, and at what rate against the wall?**

Windows charges thread quantum in clock ticks and satisfies timed waits
against these fields, so a clock that has stopped explains both a spin
that never completes and a scheduler that never preempts - and it looks
exactly like a hang caused by something else entirely.

KUSER_SHARED_DATA is at a fixed kernel address on x64, 0xFFFFF78000000000,
and is readable through Windows' own page tables with the monitor's `xp`,
so this works on a frozen guest and perturbs nothing.

The three fields are KSYSTEM_TIME - LowPart, High1Time, High2Time - and
are written without a lock, so each is read as a `high1, low, high2`
triple and retried while the halves disagree. Reading LowPart alone
would wrap every 429 seconds at 100 ns resolution and silently report
time going backwards.
"""

import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446
KUSD = 0xFFFFF78000000000

FIELDS = {
    'InterruptTime': 0x008,     # KSYSTEM_TIME, 100ns since boot
    'SystemTime':    0x014,     # KSYSTEM_TIME, 100ns since 1601
    'TimeZoneBias':  0x020,
}
TICKCOUNT = 0x320               # KSYSTEM_TIME, ticks


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
        i = d.find(c)
        return d[i + len(c):] if i >= 0 else d


_M = Mon()


def xp_q(phys, n=1):
    d = _M.cmd(f'xp /{n}xg 0x{phys:x}')
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', d)]


def xp_w(phys, n=1):
    d = _M.cmd(f'xp /{n}xw 0x{phys:x}')
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', d)]


CR3 = int(sys.argv[1], 16) & 0x000ffffffffff000
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
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


base_phys = v2p(KUSD)
if base_phys is None:
    print('KUSER_SHARED_DATA not mapped - wrong cr3?')
    sys.exit(1)
print(f'KUSER_SHARED_DATA 0x{KUSD:x} -> phys 0x{base_phys:x}')


def ksystem_time(off):
    """high1, low, high2 - retry while the halves disagree."""
    for _ in range(8):
        w = xp_w(base_phys + off, 3)
        if len(w) < 3:
            continue
        low, high1, high2 = w[0], w[1], w[2]
        if high1 == high2:
            return (high1 << 32) | low
    return None


def snap():
    return {
        'InterruptTime': ksystem_time(0x008),
        'SystemTime': ksystem_time(0x014),
        'TickCount': ksystem_time(TICKCOUNT),
    }


t0 = time.time()
a = snap()
time.sleep(SECS)
b = snap()
wall = time.time() - t0

print(f'\nmeasured wall span {wall:.3f} s\n')
print(f'{"field":16} {"first":>20} {"second":>20} {"delta":>16}  rate')
for k in a:
    if a[k] is None or b[k] is None:
        print(f'{k:16} {"unreadable":>20}')
        continue
    d = b[k] - a[k]
    if k == 'TickCount':
        rate = f'{d / wall:.1f} ticks/s'
    else:
        rate = f'{d / 1e7 / wall:.4f} s per wall second'
    print(f'{k:16} {a[k]:>20,} {b[k]:>20,} {d:>16,}  {rate}')

print('\nInterruptTime and SystemTime are in 100ns units, so a healthy')
print('guest shows ~1.0000 s per wall second. A rate of 0 is a stopped')
print('clock; a rate far from 1 is a guest whose timed waits and thread')
print('quanta are being charged at the wrong speed.')
