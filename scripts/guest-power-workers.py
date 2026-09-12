r"""Walk `PopIrpThreadList` and name the driver each power worker is inside.

**This is the instrument Microsoft's own 0x9F triage points at, and
nothing in this tree read it.** The `TRIAGE_9F_POWER` block that
`PopIrpWatchdogBugcheck` (RVA 0x5ca648) assembles contains exactly four
things: `PopIrpList`, **`PopIrpThreadList`**, `ExWorkerQueue` and
`IoWorkerQueue`. `guest-power-irps.py` covers the first. This covers the
second, which is the one that names a *culprit* rather than a symptom.

Why it matters here. Device power IRPs are **not** delivered
synchronously: `PoHandleIrp` (RVA 0x3c75f4) hands anything without
`DO_POWER_NOOP` to `PopDispatchQuerySetIrp` (0x3c7bf0), which queues the
IRP on `PopIrpWorkerList` and signals `PopIrpWorkerSemaphore`. The actual
`IRP_MJ_POWER` dispatch call happens on a `PopIrpWorker` thread (0x4e33e0).

`PopInitializeIrpWorkers` (0xc32954) creates **exactly two** of those, at
**base priority 13** (`PopCreatePowerThread` 0x492c58 ->
`KeSetActualBasePriorityThread(thread, 13)`), growing to at most 15 and
only when a worker observes the pool saturated *at the moment it
dequeues*. Two PASSIVE_LEVEL threads is the shape that starves first on a
slow machine - which is the standing hypothesis this script exists to
test.

Each worker registers a `_POP_IRP_WORKER_ENTRY` **on its own kernel
stack** while it runs:

    +0x00  Link (LIST_ENTRY)
    +0x10  Thread   (ETHREAD)
    +0x18  Irp      NON-ZERO => this worker is INSIDE a driver right now
    +0x20  Device   (DEVICE_OBJECT) -> +0x08 DriverObject -> +0x38 name
    +0x28  Static   1 = one of the two threads created at boot

So: **every node with a non-zero `Irp` and no idle worker left means the
pool is fully blocked, and `+0x20` names who is holding it.**

READER PROOFS - four, and the script says which failed
------------------------------------------------------
1. the list head must be well-formed (`head->Flink->Blink == head`), the
   same proof `guest-power-irps.py` uses on `PopIrpList`
2. every `Thread` must be a canonical kernel pointer
3. every `Device` must resolve to a `DRIVER_OBJECT` whose name is a
   readable `\Driver\...` UNICODE_STRING
4. the walk must terminate at the head, and a cycle or an over-long walk
   is reported as a FAILURE rather than as a result

**32-bit reads on this guest are NOT proven.** `guest-power-irps.py`
records `PopWatchdogSleepTimeout` reading 296,207,625 instead of 600 and
`PopIrpWorkerCount` reading 296,159,351, while pointer reads on
`PopIrpList` were good. So this script reads a **known constant** first -
`PopIrpWorkerSemaphore.Limit` must be `0x7FFFFFFF` - and refuses to print
any dword until that passes. Without that anchor a garbage worker count
looks exactly like a saturated pool.

usage: guest-power-workers.py <kernel_base_hex> <cr3_hex>
"""
import re
import socket
import sys
import time

RIG, PORT = '192.168.1.199', 4446

# RVAs from the disassembly of the shipped ntoskrnl. Section 26 (.data)
# unless noted; `guest-power-irps.py` records why the section matters.
POPIRPTHREADLIST = 0xf08790        # LIST_ENTRY head, Link at +0 of node
POPIRPWORKERLIST = 0xf0b380        # LIST_ENTRY of QUEUED IRPs (Irp+0xa8)
POPIRPWORKERSEM = 0xf0b3a0         # KSEMAPHORE
POPIRPWORKERCOUNT = 0xf08778
POPIRPWORKERINFLIGHT = 0xf0870c
POPIRPWORKERPENDING = 0xf0877c
POPINRUSHIRP = 0xf0b3c8
POPINRUSHIRPLIST = 0xf0bd50
POPPENDINGSETPOWERIRPS = 0xf0b3d0
POPCURRENTIRPSEQUENCEID = 0xf0b350

# _POP_IRP_WORKER_ENTRY, sizeof 48
W_THREAD, W_IRP, W_DEVICE, W_STATIC = 0x10, 0x18, 0x20, 0x28

# KSEMAPHORE: Header (DISPATCHER_HEADER, 0x18) then Limit.
SEM_TYPE, SEM_SIGNALSTATE, SEM_WAITLIST, SEM_LIMIT = 0x00, 0x04, 0x08, 0x18
SEM_LIMIT_EXPECTED = 0x7FFFFFFF
SEMAPHORE_OBJECT_TYPE = 5

WALK_LIMIT = 64


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


def _vals(cmd, width):
    d = monitor([cmd])
    out = []
    for l in _rows(d):
        body = l.split(':', 1)[1]
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{%d})' % width,
                                               body)]
    return out


def xp_q(p, n=1):
    return _vals(f'xp /{n}xg 0x{p:x}', 16)


def xp_w(p, n=1):
    return _vals(f'xp /{n}xw 0x{p:x}', 8)


def xp_b(p, n):
    return _vals(f'xp /{n}xb 0x{p:x}', 2)


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
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


def rw(va):
    p = v2p(va)
    if p is None:
        return None
    v = xp_w(p)
    return v[0] if v else None


def rbytes(va, n):
    p = v2p(va)
    if p is None:
        return None
    return xp_b(p, n)


def canonical(v):
    return v is not None and v != 0 and (v >> 47) in (0, 0x1ffff)


def unicode_string(va):
    """Read a UNICODE_STRING {len, max, pad, buffer} and its characters."""
    hdr = rq(va)
    buf = rq(va + 8)
    if hdr is None or not canonical(buf):
        return None
    ln = hdr & 0xffff
    if not ln or ln > 512:
        return None
    b = rbytes(buf, min(ln, 128))
    if not b:
        return None
    return ''.join(chr(b[i]) for i in range(0, len(b) - 1, 2)
                   if 32 <= b[i] < 127)


def drivername(devobj):
    """DEVICE_OBJECT +0x08 DriverObject -> +0x38 DriverName."""
    if not canonical(devobj):
        return '<null>'
    drv = rq(devobj + 0x08)
    if not canonical(drv):
        return f'device {devobj:#x} -> DriverObject unreadable'
    nm = unicode_string(drv + 0x38)
    return nm if nm else f'DRIVER_OBJECT {drv:#x} (name unreadable)'


print(f'kernel base 0x{BASE:x}  cr3 0x{CR3:x}')

# ---------------------------------------------------------------- anchor
# A garbage dword read looks exactly like a saturated pool, so prove the
# 32-bit path against a constant the kernel must hold before printing any
# count. `PopInitializeIrpWorkers+0xcc` writes Limit = 0x7FFFFFFF.
sem = BASE + POPIRPWORKERSEM
limit = rw(sem + SEM_LIMIT)
otype = rbytes(sem + SEM_TYPE, 1)
dword_ok = (limit == SEM_LIMIT_EXPECTED)
print(f'\n32-bit read anchor: PopIrpWorkerSemaphore.Limit = '
      f'{limit if limit is None else hex(limit)} '
      f'(must be 0x7fffffff) -> {"PASS" if dword_ok else "FAIL"}')
if otype:
    print(f'                    header Type = {otype[0]} '
          f'(must be {SEMAPHORE_OBJECT_TYPE} = SemaphoreObject) -> '
          f'{"PASS" if otype[0] == SEMAPHORE_OBJECT_TYPE else "FAIL"}')
if not dword_ok:
    print('  -> every dword below is SUPPRESSED. A wrong 32-bit read is '
          'indistinguishable from a saturated pool, and this tree has '
          'already quoted PopIrpWorkerCount as 296,159,351.')

if dword_ok:
    sig = rw(sem + SEM_SIGNALSTATE)
    wl = rq(sem + SEM_WAITLIST)
    print(f'\nPopIrpWorkerSemaphore.SignalState = {sig}'
          + ('   <- IRPs queued and UNCLAIMED' if sig else
             '   (nothing waiting to be claimed)'))
    print(f'PopIrpWorkerSemaphore.WaitListHead = '
          f'{wl and hex(wl)}'
          + ('   <- EMPTY: no worker is idle, all are busy or blocked'
             if wl == sem + SEM_WAITLIST else
             '   (a worker is idle and waiting for work)'))
    for name, rva in (('PopIrpWorkerCount', POPIRPWORKERCOUNT),
                      ('PopIrpWorkerInFlight', POPIRPWORKERINFLIGHT),
                      ('PopIrpWorkerPending', POPIRPWORKERPENDING),
                      ('PopPendingSetPowerDeviceIrps',
                       POPPENDINGSETPOWERIRPS)):
        print(f'{name} = {rw(BASE + rva)}')
    print(f'PopCurrentIrpSequenceID = {rw(BASE + POPCURRENTIRPSEQUENCEID)}'
          f'   <- sample twice: a moving value means power IRPs are still '
          f'being ISSUED')

inrush = rq(BASE + POPINRUSHIRP)
print(f'\nPopInrushIrp = {inrush and hex(inrush)}'
      + ('   <- an inrush IRP owns the global slot; every other inrush '
         'device is serialised behind it, with NO watchdog until it '
         'becomes PopInrushIrp' if inrush else '   (slot free)'))

# ------------------------------------------------------- the worker list
head = BASE + POPIRPTHREADLIST
flink = rq(head)
blink = rq(head + 8)
print(f'\nPopIrpThreadList head 0x{head:x}  Flink {flink and hex(flink)}'
      f'  Blink {blink and hex(blink)}')
if flink is None:
    print('  -> head UNREADABLE. Not "empty" - not read.')
    sys.exit(0)
if flink == head and blink == head:
    print('  -> reader proven: well-formed EMPTY list.')
    print('  NO power worker is inside a driver. If the guest is stalled, '
          'it is NOT stalled in an IRP_MJ_POWER dispatch, and the power '
          'worker pool is not the blocker.')
    sys.exit(0)
back = rq(flink + 8) if canonical(flink) else None
if back != head:
    print(f'  -> READER NOT PROVEN: head->Flink->Blink is '
          f'{back and hex(back)}, expected 0x{head:x}. Nothing below is '
          f'printed.')
    sys.exit(1)
print('  -> reader proven: head->Flink->Blink points back at head')

cur, n, seen, why = flink, 0, set(), 'ran off the end'
busy = 0
while cur and cur != head and n < WALK_LIMIT:
    if cur in seen:
        why = 'CYCLE - the list points back at itself'
        break
    seen.add(cur)
    n += 1
    thread = rq(cur + W_THREAD)
    irp = rq(cur + W_IRP)
    dev = rq(cur + W_DEVICE)
    static = rq(cur + W_STATIC)
    print(f'\n  [{n}] _POP_IRP_WORKER_ENTRY 0x{cur:x}'
          + ('   (on a boot worker\'s stack)' if static == 1 else ''))
    print(f'      Thread  {thread and hex(thread)}'
          + ('' if canonical(thread) else
             '   <- NOT a canonical kernel pointer, entry suspect'))
    if canonical(irp):
        busy += 1
        print(f'      Irp     {irp:#x}   <- THIS WORKER IS INSIDE A DRIVER')
        print(f'      Device  {dev and hex(dev)}  {drivername(dev)}')
        print(f'        ^ the driver holding a power worker RIGHT NOW')
    else:
        print(f'      Irp     <null>   (idle - not inside a driver)')
        print(f'      Device  {dev and hex(dev)}')
    nxt = rq(cur)
    if nxt is None:
        why = 'unreadable Flink'
        break
    if nxt == head:
        why = 'reached the head - complete'
        break
    cur = nxt
else:
    if n >= WALK_LIMIT:
        why = f'hit the {WALK_LIMIT}-entry walk limit'

print(f'\n{n} worker entr{"y" if n == 1 else "ies"}; walk ended because: '
      f'{why}')
print(f'{busy} of {n} are inside a driver.')

# The verdict, stated as a rule rather than a guess, so it cannot be
# fitted to the data after the fact.
print('\nHOW TO READ THIS:')
print('  every entry busy AND SignalState > 0  -> the pool is SATURATED: '
      'work is queued and no worker can take it. The drivers named above '
      'are holding it.')
print('  every entry busy AND SignalState == 0 -> workers are inside '
      'drivers but nothing is waiting; slow, not blocked.')
print('  some entry idle                        -> the pool is NOT the '
      'bottleneck, whatever else is wrong.')
print('  Sample twice: a Device that CHANGES is progress, a Device frozen '
      'across two reads is the blocker.')
