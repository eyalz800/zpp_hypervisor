r"""Name the DPCs that are queued and never retire.

`guest-dpc-state.py` established the fact this exists to explain: on boot
185 both processors held a NON-EMPTY DPC queue and executed **zero** DPCs
across three samples spanning 150 s, with `DpcRequestSummary` nonzero and
Gate B open. `DpcCount` frozen at 2,431 / 798, `DpcQueueDepth` frozen at
2 / 1, `QuantumEnd` set on both.

That says the drain is stuck. It does not say *what* is stuck, and the
DeferredRoutine of a queued DPC names the subsystem waiting on it - which
is the difference between "the scheduler is unhappy" and "storage
completion has been parked for three minutes".

STRUCTURE - every offset from the PDB type stream, none assumed
---------------------------------------------------------------
`_KDPC`, sizeof 64, from the field list that carries `DpcListEntry` AND
`DeferredRoutine` together (there are three other `DeferredRoutine`
members in this PDB, all at offset 64 of unrelated structures - the
duplicate-member trap that has already cost this tree several wrong
readings):

    TargetInfoAsUlong  +0x00   Type +0x00, Importance +0x01, Number +0x02
    DpcListEntry       +0x08   SINGLE_LIST_ENTRY - 8 bytes, proven by
                               ProcessorHistory sitting at +0x10
    ProcessorHistory   +0x10
    DeferredRoutine    +0x18
    DeferredContext    +0x20
    SystemArgument1    +0x28
    SystemArgument2    +0x30
    DpcData            +0x38

`_KDPC_DATA`, sizeof 48, at `KPRCB+0x3840` (from `guest-dpc-state.py`,
whose offsets are PDB-verified):

    DpcList (KDPC_LIST)  +0x00   ListHead.Next +0x00, LastEntry +0x08
    DpcLock              +0x10
    DpcQueueDepth        +0x18
    DpcCount             +0x1c
    ActiveDpc            +0x20

The list is **singly linked**, so a walk cannot detect a cycle by
returning to the head. It is bounded and cycle-checked explicitly.

READER PROOF - three, and all three must pass
----------------------------------------------
1. `KPRCB.Number` (+0x24) == the array index, per processor. Same proof
   `guest-dpc-state.py` uses; it validates `KiProcessorBlock` and the
   `_KPRCB` layout at once.
2. **The number of entries walked must equal `DpcQueueDepth`.** This is
   the strong one and it is free: two independent fields, one a count and
   one a list, that can only agree if the layout is right. A walk that
   ends early or runs long is reported as a FAILURE, not as a result.
3. Every `DeferredRoutine` must be a canonical kernel address. A routine
   inside `ntoskrnl` is named; one outside it is reported as such, since
   a driver's DPC is exactly what would be interesting here.

`DpcData` (+0x38) of each entry should point back at the `_KDPC_DATA` it
is queued on. That is checked and printed as a fourth, weaker proof - it
can legitimately be NULL on an entry mid-insertion.

usage: guest-dpc-queue.py <kernel_base_hex> <cr3_hex> [cpus]
"""
import re
import socket
import sys
import time

RIG, PORT = '192.168.1.199', 4446

KIPROCESSORBLOCK_RVA = 0xfc8c80

P_NUMBER = 0x24
P_DPCDATA = 0x3840

# _KDPC_DATA, relative to P_DPCDATA
D_LISTHEAD, D_LASTENTRY = 0x00, 0x08
D_QUEUEDEPTH, D_COUNT, D_ACTIVEDPC = 0x18, 0x1c, 0x20
KDPC_DATA_SIZE = 0x30

# _KDPC, relative to the KDPC base (= DpcListEntry address - K_LISTENTRY)
K_TYPE, K_IMPORTANCE, K_NUMBER = 0x00, 0x01, 0x02
K_LISTENTRY, K_HISTORY = 0x08, 0x10
K_ROUTINE, K_CONTEXT = 0x18, 0x20
K_ARG1, K_ARG2, K_DPCDATA = 0x28, 0x30, 0x38

# A DPC queue longer than this is not a queue, it is a corrupted list.
WALK_LIMIT = 64

IMPORTANCE = {0: 'Low', 1: 'Medium', 2: 'High', 3: 'MediumHigh'}


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
    # Match ONLY the data columns. The monitor echoes the command back,
    # and a physical address in that echo is 9-12 hex digits - a regex of
    # `0x([0-9a-f]{8})` matched the echo and produced a whole session of
    # fabricated readings. Anchor on the row prefix instead.
    d = monitor([f'xp /{n}xg 0x{p:x}'])
    out = []
    for l in _rows(d):
        body = l.split(':', 1)[1]
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', body)]
    return out


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
CPUS = int(sys.argv[3]) if len(sys.argv) > 3 else 2
KSIZE = 0x1450000
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


def byte(va):
    v = rq(va & ~7)
    return None if v is None else (v >> (8 * (va & 7))) & 0xff


def dword(va):
    v = rq(va & ~7)
    return None if v is None else (v >> (32 * ((va >> 2) & 1))) & 0xffffffff


def canonical(v):
    return v is not None and (v >> 47) in (0, 0x1ffff) and v != 0


def where(v):
    if v is None:
        return '?'
    if BASE <= v < BASE + KSIZE:
        return f'ntoskrnl+0x{v - BASE:x}'
    return 'NOT in ntoskrnl  <- a driver DPC, or a wrong read'


print(f'kernel base 0x{BASE:x}  cr3 0x{CR3:x}')

for i in range(CPUS):
    prcb = rq(BASE + KIPROCESSORBLOCK_RVA + 8 * i)
    print(f'\n=== cpu {i}: KPRCB {prcb and hex(prcb)} ===')
    if not prcb:
        print('  unreadable')
        continue
    num = dword(prcb + P_NUMBER)
    if num != i:
        print(f'  READER NOT PROVEN: KPRCB.Number reads {num}, expected '
              f'{i}. Nothing below is printed.')
        continue
    print(f'  reader proven: KPRCB.Number == {i}')

    for which, label in ((0, 'normal'), (1, 'threaded')):
        dd = prcb + P_DPCDATA + which * KDPC_DATA_SIZE
        depth = dword(dd + D_QUEUEDEPTH)
        count = dword(dd + D_COUNT)
        active = rq(dd + D_ACTIVEDPC)
        head = rq(dd + D_LISTHEAD)
        last = rq(dd + D_LASTENTRY)
        print(f'\n  DpcData[{which}] ({label})  depth {depth}  '
              f'count {count}  ActiveDpc {active and hex(active)}')
        print(f'    ListHead.Next {head and hex(head)}   '
              f'LastEntry {last and hex(last)}')
        if not depth:
            if head:
                print('    depth 0 but ListHead.Next is NON-NULL '
                      '-> layout or read is wrong')
            continue

        walked, seen, p, why = [], set(), head, 'ran off the end'
        while p and len(walked) < WALK_LIMIT:
            if p in seen:
                why = 'CYCLE - the list points back at itself'
                break
            seen.add(p)
            k = p - K_LISTENTRY
            walked.append(k)
            nxt = rq(p)
            if nxt is None:
                why = 'unreadable next pointer'
                break
            if nxt == 0:
                why = 'reached the end of the list'
                break
            p = nxt
        else:
            if len(walked) >= WALK_LIMIT:
                why = f'hit the {WALK_LIMIT}-entry walk limit'

        # PROOF 2 - the count and the list must agree.
        ok = (len(walked) == depth)
        print(f'    walked {len(walked)} entr{"y" if len(walked) == 1 else "ies"}'
              f', ended because: {why}')
        print(f'    PROOF: walked == DpcQueueDepth?  '
              f'{len(walked)} vs {depth} -> {"PASS" if ok else "FAIL"}')
        if not ok:
            print('    -> the walk disagrees with the count, so the '
                  'routines below are NOT trustworthy. Reported anyway, '
                  'labelled, rather than silently dropped.')

        for n, k in enumerate(walked):
            routine = rq(k + K_ROUTINE)
            ctx = rq(k + K_CONTEXT)
            a1 = rq(k + K_ARG1)
            a2 = rq(k + K_ARG2)
            owner = rq(k + K_DPCDATA)
            imp = byte(k + K_IMPORTANCE)
            tgt = dword(k + K_NUMBER) if False else None
            print(f'    [{n}] _KDPC 0x{k:x}')
            print(f'         DeferredRoutine 0x{routine:x}  {where(routine)}'
                  if canonical(routine) else
                  f'         DeferredRoutine {routine and hex(routine)}  '
                  f'<- NOT canonical, this is not a routine')
            print(f'         DeferredContext {ctx and hex(ctx)}   '
                  f'Arg1 {a1 and hex(a1)}   Arg2 {a2 and hex(a2)}')
            print(f'         Importance {IMPORTANCE.get(imp, imp)}   '
                  f'DpcData {owner and hex(owner)}'
                  + ('  (points back at this queue: PASS)'
                     if owner == dd else
                     '  <- does NOT point back at this queue'))
            _ = tgt
