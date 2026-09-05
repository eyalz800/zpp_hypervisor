r"""Read hvix64's private software vAPIC block from outside.

Tests one question: **is Hyper-V's lazy-EOI grant being denied, and if so
by what?** `HvlEndSystemInterrupt` issues the explicit synthetic-EOI
`wrmsr` only when hvix64 withheld the grant, and that wrmsr costs a
reflection - measured at 23.5% of all exits on a wedged guest.

usage: guest-hv-vapic.py <hvix64_cr3> <hvix64_gs_base> [cpu]

Both anchors come from `rig-dump-state.py`:

    guest hypervisor's own index 0 from gs base 0x...  - agrees with cpu 0
      guest hypervisor's own cr3 0x...

THE ROUTE, and a correction to the one written down
---------------------------------------------------
The documented route was `GS -> *(GS+0) = LP block -> *(LP+0x2c770) = VP`,
proved by `*(LP+0x2c778) == cpu`. **That hop does not hold on this build.**
`*(GS+0)` is a self-pointer here and `*(LP+0x2c778)` reads `0xffffffff`,
so the walk refuses there.

What works is the independent cached pointer the same notes mention as a
cross-check: **`VP = *(GS+0x358)`**. On the machine this was written
against, `GS+0x368` holds the same pointer, so two slots agree before the
walk even starts.

    VP    = *(GS + 0x358)
    VTL0  = *(VP + 0x148)          VTL1 = *(VP + 0x150)
    A0    = VTL0 + 0x80

PROOFS - each hop, and the walk stops rather than guessing
----------------------------------------------------------
    *(GS + 8)      low dword == cpu       the processor index
    *(VP + 8)      low dword == cpu       hvix64's own assertion
    *(VTL0)        == VP                  back-pointer
    *(VTL0 + 0x14) byte      == 0         the VTL number

Corroboration, not gated on: `*(VP+0x18)` reads `0xfee00300`, the APIC ICR
address, which is what a per-VP APIC context should carry.

FIELDS
------
    A0+0x5e0  byte      ISR stack depth
    A0+0x5e8  dword     highest pending vector
    A0+0x5ec  dword     SECOND-highest pending vector
    A0+0x522  byte      ApicLazyEoiGranted
    A0+0x590  256 bits  IRR;  vector 0x2f is word 0 bit 47

`0x5e8`/`0x5ec` are written together as ONE qword by the IRR scan, which
is why they are read as one here.

HOW TO READ IT
--------------
Healthy baseline, measured on a progressing boot:

    depth 1, highest 0xd1, SECOND 0x0, granted 1, 0x2f not in IRR

i.e. the clock is in service, nothing else is pending, and the grant is
made - so no explicit EOI wrmsr is needed.

The hypothesis under test predicts, on a WEDGED boot:

    SECOND == 0x2f  and  granted == 0

because one of the grant filters is "another interrupt is queued", and a
chronically pending DPC vector would deny the grant on every interrupt.

    SECOND == 0x2f, granted 0   -> confirmed, and the denier is named
    SECOND == 0,    granted 1   -> refuted; the explicit EOIs have another
                                   cause
    granted 1 while the guest still writes EOI -> the grant is made and the
                                   guest does not see it: a VP-assist
                                   coherency failure, a different repair

Sample repeatedly. A single read of a periodic system is a constant
generator - this tree's own rule.
"""
import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446


def _rows(d):
    return [l for l in d.splitlines()
            if re.match(r'^[0-9a-f]{6,}: ', l.strip())]


def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12)
    time.sleep(0.35)
    for c in cmds:
        s.sendall((c + '\n').encode())
        time.sleep(0.30)
    time.sleep(1.2)
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


def xp_q(p, n=1):
    d = monitor([f'xp /{n}xg 0x{p:x}'])
    out = []
    for l in _rows(d):
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', l)]
    return out


CR3 = int(sys.argv[1], 16) & 0x000ffffffffff000
GS = int(sys.argv[2], 16)
CPU = int(sys.argv[3]) if len(sys.argv) > 3 else 0
_E = {}


def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        k = (t, (va >> sh) & 0x1ff)
        e0 = _E.get(k)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
            if not e:
                return None
            e0 = e[0]
            _E[k] = e0
        if not (e0 & 1):
            return None
        if lvl < 3 and (e0 & 0x80):
            m = (1 << sh) - 1
            return (e0 & ~m & 0x000fffffffffffff) | (va & m)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)


def rq(va):
    p = v2p(va)
    if p is None:
        return None
    v = xp_q(p)
    return v[0] if v else None


def canon(x):
    return x is not None and x >= 0xffff800000000000


def fail(msg):
    print(f'  -> WALK NOT PROVEN: {msg}')
    print('     Nothing below would be meaningful, so it is not printed. '
          'A wrong anchor fails here; a wedged guest does not.')
    sys.exit(1)


print(f'hvix64 cr3 {CR3:#x}  gs {GS:#x}  expecting cpu {CPU}')

v = rq(GS + 8)
if v is None or (v & 0xffffffff) != CPU:
    fail(f'*(GS+8) low dword is {v and (v & 0xffffffff)}, expected {CPU}')
print(f'hop0  *(GS+8) = {CPU}  proven')

VP = rq(GS + 0x358)
if not canon(VP):
    fail(f'*(GS+0x358) = {VP and hex(VP)} is not canonical')
alt = rq(GS + 0x368)
# `GS+0x358` is the CURRENTLY DISPATCHED VP, not necessarily the VP of
# the processor whose GS page this is - it was observed pointing at index
# 0 and then at index 1 across two reads seconds apart. So the index is
# DERIVED here and reported, not asserted against the cpu argument. The
# proof that survives is the one that cannot be satisfied by chance: a
# small plausible index, plus the VTL0 back-pointer equalling VP.
c = rq(VP + 8)
if c is None or (c & 0xffffffff) > 63:
    fail(f'*(VP+8) low dword is {c and (c & 0xffffffff)}, not a '
         f'plausible processor index')
VP_INDEX = c & 0xffffffff
icr = rq(VP + 0x18)
print(f'hop1  VP = *(GS+0x358) = {VP:#x}  proven - this is the '
      f'CURRENT VP, index {VP_INDEX}'
      + ('' if VP_INDEX == CPU else f' (NOT cpu {CPU} - the pointer '
         f'follows dispatch, so read the index, not the argument)'))
print(f'      *(GS+0x368) {"agrees" if alt == VP else "DIFFERS"}   '
      f'*(VP+0x18) = {icr and hex(icr)} '
      f'{"(APIC ICR address - corroborates)" if icr == 0xfee00300 else ""}')

VTL0 = rq(VP + 0x148)
VTL1 = rq(VP + 0x150)
if not canon(VTL0):
    fail(f'VTL0 = {VTL0 and hex(VTL0)} is not canonical')
back = rq(VTL0)
n0 = rq(VTL0 + 0x10)
if back != VP:
    fail(f'*(VTL0) = {back and hex(back)}, expected VP {VP:#x}')
if n0 is None or ((n0 >> 32) & 0xff) != 0:
    fail(f'VTL number byte is {n0 is not None and ((n0 >> 32) & 0xff)}, '
         f'expected 0')
print(f'hop2  VTL0 = {VTL0:#x}  VTL1 = {VTL1 and hex(VTL1)}  proven')

A0 = VTL0 + 0x80
print(f'\nA0 = {A0:#x}')
depth = rq(A0 + 0x5e0)
pend = rq(A0 + 0x5e8)
lazy = rq(A0 + 0x520)
irr0 = rq(A0 + 0x590)
if None in (depth, pend, lazy, irr0):
    print('  one or more fields unreadable')
    sys.exit(1)
second = (pend >> 32) & 0xffffffff
granted = (lazy >> 16) & 0xff
print(f'  ISR stack depth          {depth & 0xff}')
print(f'  highest pending vector   0x{pend & 0xffffffff:x}')
print(f'  SECOND pending (0x5ec)   0x{second:x}')
print(f'  ApicLazyEoiGranted       {granted}')
print(f'  IRR word0                {irr0:#018x}   '
      f'vector 0x2f pending = {(irr0 >> 47) & 1}')

print()
if second == 0x2f and 0 == granted:
    print('-> CONFIRMED: the DPC vector 0x2f is the second pending vector '
          'and the lazy-EOI grant is withheld. The chronically pending '
          '0x2f denies the grant on every interrupt, which is what forces '
          'the explicit synthetic EOI wrmsr.')
elif 0 == second and granted:
    print('-> REFUTED for this sample: nothing else is pending and the '
          'grant IS made, so an explicit EOI here would have another '
          'cause. This is the healthy shape.')
elif granted:
    print('-> the grant IS made. If the guest still writes the explicit '
          'EOI, hvix64 wrote the grant somewhere the guest is not reading '
          '- a VP-assist coherency failure, which is a different repair.')
else:
    print(f'-> grant withheld with second pending vector 0x{second:x}. '
          f'Mechanism holds, actor differs - check that vector\'s class '
          f'against the priority it is asked at.')
