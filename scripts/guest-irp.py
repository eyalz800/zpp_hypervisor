r"""Read a blocked IRP's current stack location: who owes the completion.

**Why.** `e664ee5` eliminated the VTL livelock as the cause of the 300 s
power-watchdog stall, on a boot that was healthy by every other measure
and died anyway. What is left is the IRP itself: `guest-power-irps.py`
names the IRP and its device but not what the IRP is *waiting on*.

`_IO_STACK_LOCATION`, PDB-verified from ntkrnlmp.pdb's field list:

    MajorFunction       0      MinorFunction   1
    Control             3      DeviceObject   40
    CompletionRoutine  56

**The offset of `Tail.Overlay.CurrentStackLocation` inside `_IRP` is NOT
taken on faith.** `llvm-pdbutil` reports `CurrentStackLocation` at
offset 64 of an anonymous inner struct, not of `_IRP`, and adding
nesting offsets by hand is exactly how this tree has produced plausible
wrong answers before. So the pointer is found by **scanning** the IRP's
first 0x100 bytes for a candidate and then **proving** it:

  - it must point inside or just past the IRP (stack locations are
    allocated contiguously after the header), and
  - the `MajorFunction` it lands on must be **0x16, IRP_MJ_POWER**.

A power IRP whose stack location does not say IRP_MJ_POWER is not a
stack location. Nothing prints unless that check passes, so a wrong
offset produces a refusal instead of a driver name - which matters here
because this investigation has already named four innocent drivers.

usage: guest-irp.py <cr3_hex> <irp_va_hex> [device_va_hex]
"""
import re
import socket
import sys
import time

RIG, PORT = '192.168.1.199', 4446
IRP_MJ_POWER = 0x16
SL_MAJOR, SL_MINOR, SL_CONTROL = 0, 1, 3
SL_DEVICE, SL_COMPLETION = 40, 56


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
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out.decode('utf-8', 'replace'))


def _vals(cmd, width):
    out = []
    for l in monitor([cmd]).splitlines():
        if not re.match(r'^[0-9a-f]{6,}: ', l.strip()):
            continue
        out += [int(x, 16)
                for x in re.findall(r'0x([0-9a-f]{%d})' % width,
                                    l.split(':', 1)[1])]
    return out


CR3 = int(sys.argv[1], 16) & 0x000ffffffffff000
IRP = int(sys.argv[2], 16)
cache = {}


def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        k = (t, (va >> sh) & 0x1ff)
        v = cache.get(k)
        if v is None:
            got = _vals(f'xp /1xg 0x{t + ((va >> sh) & 0x1ff) * 8:x}', 16)
            if not got:
                return None
            v = got[0]
            cache[k] = v
        if not (v & 1):
            return None
        if lvl < 3 and (v & 0x80):
            m = (1 << sh) - 1
            return (v & ~m & 0x000fffffffffffff) | (va & m)
        t = v & 0x000ffffffffff000
    return t | (va & 0xfff)


def qwords(va, n):
    p = v2p(va)
    if p is None:
        return []
    return _vals(f'xp /{n}xg 0x{p:x}', 16)


def bytes_at(va, n):
    p = v2p(va)
    if p is None:
        return []
    return _vals(f'xp /{n}xb 0x{p:x}', 2)


print(f'IRP 0x{IRP:x}  cr3 0x{CR3:x}')
hdr = qwords(IRP, 32)
if not hdr:
    print('  IRP unreadable - wrong cr3, or the page is not mapped. '
          'This is a read failure, not an empty IRP.')
    sys.exit(1)

# Scan for a plausible CurrentStackLocation and PROVE it, rather than
# trusting a hand-computed nesting offset.
found = None
for i, v in enumerate(hdr):
    if not (IRP <= v < IRP + 0x400):
        continue
    b = bytes_at(v, 4)
    if len(b) >= 2 and b[SL_MAJOR] == IRP_MJ_POWER:
        found = (i * 8, v, b)
        break

if not found:
    print('  no candidate stack location proved out: no pointer in the '
          f'IRP header lands on a MajorFunction of 0x{IRP_MJ_POWER:02x} '
          '(IRP_MJ_POWER).')
    print('  Refusing to report a driver. A power IRP whose stack '
          'location does not say IRP_MJ_POWER is not a stack location.')
    sys.exit(1)

off, sl, b = found
print(f'  PROOF: IRP+0x{off:x} -> 0x{sl:x}, MajorFunction '
      f'0x{b[SL_MAJOR]:02x} = IRP_MJ_POWER -> PASS')
print(f'  MinorFunction 0x{b[SL_MINOR]:02x}   Control 0x{b[SL_CONTROL]:02x}')
dev = qwords(sl + SL_DEVICE, 1)
comp = qwords(sl + SL_COMPLETION, 1)
print(f'  stack DeviceObject      {dev and hex(dev[0])}')
print(f'  stack CompletionRoutine {comp and hex(comp[0])}')
print('\n  Control bits: 0x80 INVOKE_ON_SUCCESS  0x40 INVOKE_ON_ERROR')
print('                0x20 INVOKE_ON_CANCEL   0x01 PENDING_RETURNED')
