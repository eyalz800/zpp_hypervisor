r"""Read each processor's DPC state, to say WHY vector 0x2f is not taken.

The wedge has been narrowed to one question. Hyper-V has the DPC vector
`0x2f` latched in its IRR - measured, 6 of 6 samples, healthy and wedged
alike - and on a wedged guest it is sometimes the *highest* pending vector
with an empty ISR stack. The guest asks for it 165,736 times. Only 2.4%
are ever carried. So either it is being refused on priority, or it is
delivered and drains nothing.

This distinguishes those, and it distinguishes three sub-cases that were
previously conflated:

    NestingLevel  ActiveDpc   reading
    -----------------------------------------------------------------
    1 mostly      changing    DPCs requeued faster than they retire
    1 always      frozen      one DPC never returned
    0             NULL        KiRetireDpcList is never entered at all

The third is what the ask count already implies: all three per-tick
self-IPI sites are gated on `NestingLevel == 0`
(`KiRequestSoftwareInterrupt` sets `InterruptRequest` and returns without
an IPI when nesting), so 165,736 asks being *issued* means the processor
was outside `KiRetireDpcList` essentially every tick.

`KiDpcInterrupt` EOIs first and unconditionally, then two gates can skip
the drain entirely:

    Gate A  KPRCB.IdleHalt (+0x07)        set -> return without draining
    Gate B  DpcRequestSummary & 0xBF      clear -> skip KiRetireDpcList

Gate B's mask **excludes bit 6**, `DpcNormalPriorityAntiStarvation` - the
bit `KiUpdateRunTime`'s quantum-end path sets. So a `0x2f` raised only for
that reason is delivered and legitimately drains nothing, which would
explain deliveries that achieve no progress.

usage: guest-dpc-state.py <kernel_base_hex> <cr3_hex> [cpus]

OFFSETS - every one verified against the PDB, none assumed
----------------------------------------------------------
`_KPRCB` has TWO definitions in this PDB (the small one is the WDK
truncation). These come from the field list containing `NestingLevel`:

    IdleHalt              +0x0007    Gate A
    CurrentThread         +0x0008
    NextThread            +0x0010
    IdleThread            +0x0018
    NestingLevel          +0x0020
    Number                +0x0024    <- the reader proof
    DpcData               +0x3840    _KDPC_DATA[2], sizeof 48
      .DpcQueueDepth      +0x18  ->  +0x3858
      .DpcCount           +0x1c  ->  +0x385c
      .ActiveDpc          +0x20  ->  +0x3860
    MaximumDpcQueueDepth  +0x38a8
    QuantumEnd            +0x38b9
    DpcRoutineActive      +0x38ba
    DpcRequestSummary     +0x38bc    Gate B
    DpcWatchdogCount      +0x83ac

READER PROOF
------------
`KPRCB.Number` must equal the array index, for every processor. One field,
checked per entry, and it validates both `KiProcessorBlock` and the whole
`_KPRCB` layout at once. `Number` also exists at offset 2 of a different
structure in this PDB - the duplicate-member trap that has already cost
this tree four wrong readings - so it is taken from the field list that
also contains `NestingLevel`, never by name alone.

Sample twice, seconds apart: `DpcCount` and `DpcWatchdogCount` are only
meaningful as deltas.
"""
import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446

KIPROCESSORBLOCK_RVA = 0xfc8c80
KENUMBERPROCESSORS_RVA = 0xfc6ac0

P_IDLEHALT, P_CURTHREAD, P_NEXTTHREAD, P_IDLETHREAD = 0x07, 0x08, 0x10, 0x18
P_NESTING, P_NUMBER = 0x20, 0x24
P_DPCQUEUEDEPTH, P_DPCCOUNT, P_ACTIVEDPC = 0x3858, 0x385c, 0x3860
P_MAXDEPTH, P_QUANTUMEND = 0x38a8, 0x38b9
P_DPCROUTINEACTIVE, P_DPCREQSUMMARY = 0x38ba, 0x38bc
P_DPCWATCHDOG = 0x83ac

SUMMARY_BITS = {
    0: 'NormalProcessingActive', 1: 'NormalRequested',
    2: 'ThreadSignal', 3: 'TimerExpiration', 4: 'DpcPresent',
    5: 'LocalInterrupt', 6: 'PriorityAntiStarvation (EXCLUDED by 0xBF)',
    7: 'SwapToDpcDelegate', 16: 'DpcThreadActive', 17: 'DpcThreadRequested',
}


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


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
CPUS = int(sys.argv[3]) if len(sys.argv) > 3 else 2
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
    if v is None:
        return None
    return (v >> (8 * (va & 7))) & 0xff


def dword(va):
    v = rq(va & ~7)
    if v is None:
        return None
    return (v >> (32 * ((va >> 2) & 1))) & 0xffffffff


n = dword(BASE + KENUMBERPROCESSORS_RVA)
print(f'KeNumberProcessors = {n}')

for i in range(CPUS):
    prcb = rq(BASE + KIPROCESSORBLOCK_RVA + 8 * i)
    print(f'\ncpu {i}: KPRCB {prcb and hex(prcb)}')
    if not prcb:
        print('  unreadable')
        continue
    num = dword(prcb + P_NUMBER)
    if num != i:
        print(f'  -> READER NOT PROVEN: KPRCB.Number reads {num}, '
              f'expected {i}. Either KiProcessorBlock is wrong or the '
              f'_KPRCB layout is. Nothing below is printed.')
        continue
    print(f'  reader proven: KPRCB.Number == {i}')
    idlehalt = byte(prcb + P_IDLEHALT)
    nesting = byte(prcb + P_NESTING)
    depth = dword(prcb + P_DPCQUEUEDEPTH)
    count = dword(prcb + P_DPCCOUNT)
    active = rq(prcb + P_ACTIVEDPC)
    summ = dword(prcb + P_DPCREQSUMMARY)
    qend = byte(prcb + P_QUANTUMEND)
    ra = byte(prcb + P_DPCROUTINEACTIVE)
    wd = dword(prcb + P_DPCWATCHDOG)
    cur = rq(prcb + P_CURTHREAD)
    idle = rq(prcb + P_IDLETHREAD)
    nxt = rq(prcb + P_NEXTTHREAD)
    print(f'  IdleHalt (GateA)      {idlehalt}'
          + ('   <- set: KiDpcInterrupt EOIs and returns WITHOUT draining'
             if idlehalt else ''))
    print(f'  NestingLevel          {nesting}'
          + ('   <- 0: NOT inside KiRetireDpcList' if nesting == 0 else
             '   <- inside KiRetireDpcList'))
    print(f'  DpcQueueDepth[0]      {depth}')
    print(f'  DpcCount[0]           {count}   (delta only)')
    print(f'  ActiveDpc[0]          {active and hex(active)}'
          + ('   <- NULL: no DPC is executing' if not active else ''))
    print(f'  MaximumDpcQueueDepth  {dword(prcb + P_MAXDEPTH)}')
    print(f'  QuantumEnd            {qend}   DpcRoutineActive {ra}')
    print(f'  DpcWatchdogCount      {wd}   (delta only)')
    print(f'  CurrentThread {cur and hex(cur)}  IdleThread '
          f'{idle and hex(idle)}'
          + ('   <- RUNNING IDLE' if cur and cur == idle else ''))
    print(f'  NextThread    {nxt and hex(nxt)}')
    if summ is not None:
        names = [f'{b}:{SUMMARY_BITS.get(b, "?")}'
                 for b in range(32) if (summ >> b) & 1]
        print(f'  DpcRequestSummary     {summ:#010x}  '
              f'[{", ".join(names) if names else "none set"}]')
        print(f'    Gate B: summary & 0xBF = {summ & 0xbf:#x} -> '
              + ('KiRetireDpcList RUNS' if (summ & 0xbf) else
                 'KiRetireDpcList SKIPPED (only bit 6 or nothing set)'))
    # The three-way discriminator, stated so it is not re-derived.
    if nesting == 0 and not active:
        print('  => KiRetireDpcList is NOT entered. Not "requeued faster" '
              'and not "one DPC hangs" - the drain is never reached.')
    elif active:
        print('  => a DPC is executing; sample again - frozen ActiveDpc '
              'means it never returned.')
