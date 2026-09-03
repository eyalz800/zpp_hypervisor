#!/usr/bin/env python3
"""Walk the second-level guest's System threads and say what each waits on.

    scripts/guest-threads.py <kernel-base> <windows-cr3> [--stacks]

Both arguments come from this VMM's own log line, which prints them
together, and both move every boot (KASLR):

    second-level guest kernel image at 0xfffff8018ca00000, cr3 0x1ae002

Why this exists. `guest-processes.py` answers "how far did the boot
get" and, at the phase-1 barrier this tree keeps hitting, its answer is
always the same three names - System, Secure System, Registry. That is
the *premise*, not a finding. The question that follows it is which
thread is blocked and on what, and there was no reader for it: the
recipe lived in prose, was re-derived by hand several times, and the
offsets were re-guessed each time.

The physical walk rather than the monitor's own `x`, for the same
reason `guest-processes.py` gives: the monitor translates through
whichever processor is selected, and here that processor is usually
inside the guest hypervisor's address space, so `x` answers "Cannot
access memory" - a fact about the mapping, not about the guest.

**A switched-out thread's stack is the only record of its wait.** For a
thread in state 5 (Waiting) the scheduler has spilled its return
addresses onto the kernel stack, and _KTHREAD.KernelStack points at the
switch frame. Scanning upward from there and keeping the words that
look like kernel text recovers the call chain - approximately, because
this is a scan and not an unwind, so it can show stale frames. It is
labelled `~` for that reason. Do not report a scanned frame as a stack
without saying so.

Offsets are per Windows build. These came from ntkrnlmp.pdb
(`llvm-pdbutil dump --types`, LF_MEMBER) and are printed with the raw
values beside them, so a wrong one shows up as implausible output
rather than as a plausible wrong answer.
"""

import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446

# _EPROCESS / _ETHREAD / _KTHREAD member offsets, ntkrnlmp.pdb.
P_LINKS, P_NAME = 472, 824          # ActiveProcessLinks, ImageFileName
P_THREADS       = 880               # ThreadListHead
T_ENTRY         = 1400              # _ETHREAD.ThreadListEntry
T_START         = 1248              # _ETHREAD.StartAddress
K_STATE         = 388               # _KTHREAD.State
K_STACK         = 0x58              # _KTHREAD.KernelStack
K_WAITREASON    = 0x283             # _KTHREAD.WaitReason

STATE = {0: 'Initialized', 1: 'Ready', 2: 'Running', 3: 'Standby',
         4: 'Terminated', 5: 'Waiting', 6: 'Transition',
         7: 'DeferredReady'}

# _KWAIT_REASON, the subset that appears during boot.
REASON = {0: 'Executive', 1: 'FreePage', 2: 'PageIn', 3: 'PoolAllocation',
          4: 'DelayExecution', 5: 'Suspended', 6: 'UserRequest',
          7: 'WrExecutive', 8: 'WrFreePage', 9: 'WrPageIn',
          13: 'WrQueue', 15: 'WrEventPair', 16: 'WrVirtualMemory',
          22: 'WrPreempted', 27: 'WrKernel', 31: 'WrGuardedMutex'}


class Mon:
    """ONE persistent connection, and that is the whole difference.

    A reader that opens a socket per read pays about 1.6 s each, so a
    thread walk - hundreds of reads - outlasts the state it is
    describing and gets killed before printing anything. Measured: the
    per-read form produced no output at all inside fifteen minutes.

    The monitor also accepts exactly one connection, so this must be
    closed; a leaked socket makes every later reader report None for
    ever after.
    """

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
        # **Drop the echoed command line.** The monitor echoes what it
        # was sent, and the echo contains the address - so a byte-wide
        # reader whose pattern is `0x[0-9a-f]{2}` matches inside
        # `0x119800000` and returns the command back as data. That
        # produced a process list whose every name was garbage, and
        # cost a silent empty walk: the names simply never compared
        # equal to anything, so nothing printed and the exit status was
        # zero. A quoted reader must remove its own echo.
        i = d.find(c)
        if i >= 0:
            d = d[i + len(c):]
        return d


_M = Mon()


def monitor(cmds):
    return ''.join(_M.cmd(c) for c in cmds)


def xp_q(phys, n=1):
    """xp /Ngx - the address column has NO 0x prefix, so the 16-hex-digit
    match only ever picks up value columns."""
    d = monitor([f'xp /{n}xg 0x{phys:x}'])
    return [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', d)]


def xp_b(phys, n):
    d = monitor([f'xp /{n}xb 0x{phys:x}'])
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', d))


CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
_ENTRY = {}


def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _ENTRY.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
            if not e:
                return None
            e0 = e[0]
            _ENTRY[key] = e0
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
WANT_STACKS = '--stacks' in sys.argv
# ntoskrnl is well under 16 MB; anything outside that is another image.
TEXT_LO, TEXT_HI = BASE, BASE + 0x1000000


def name_of(va):
    return f'ntoskrnl+0x{va - BASE:x}' if TEXT_LO <= va < TEXT_HI \
        else f'0x{va:x}'


head = BASE + 0xf05c60                      # PsActiveProcessHead
first = rq(head)
if not first:
    print('PsActiveProcessHead not mapped - wrong base or wrong cr3')
    sys.exit(1)

cur = first[0]
proc = []
for _ in range(60):
    if not cur or cur == head:
        break
    eproc = cur - P_LINKS
    nm = ''
    p = v2p(eproc + P_NAME)
    if p:
        nm = xp_b(p, 16).split(b'\x00')[0].decode('ascii', 'replace')
    proc.append((eproc, nm))
    nxt = rq(cur)
    if not nxt:
        break
    cur = nxt[0]

print('processes: ' + ', '.join(f'{n or "<unnamed>"}' for _, n in proc))

if not any(n == 'System' for _, n in proc):
    print('no process named System - the names above are the evidence; '
          'if they look like garbage the reader is at fault, not the guest')
    sys.exit(1)

for eproc, nm in proc:
    if nm != 'System':
        continue
    print(f'{nm}  _EPROCESS 0x{eproc:x}')
    lh = eproc + P_THREADS
    e = rq(lh)
    if not e:
        print('  thread list not mapped')
        break
    t, n, blocked = e[0], 0, 0
    while t and t != lh and n < 400:
        eth = t - T_ENTRY
        st = rq(eth + K_STATE)
        sa = rq(eth + T_START)
        state = (st[0] & 0xff) if st else None
        start = sa[0] if sa else 0
        wrb = xp_b(v2p(eth + K_WAITREASON), 1) if v2p(eth + K_WAITREASON) else b''
        wr = wrb[0] if wrb else None
        n += 1
        label = STATE.get(state, f'?{state}')
        rs = REASON.get(wr, f'{wr}')
        if state == 5:
            blocked += 1
        print(f'  _ETHREAD 0x{eth:x}  {label:<12} {rs:<14} '
              f'start {name_of(start)}')
        if WANT_STACKS and state == 5:
            ks = rq(eth + K_STACK)
            if ks and ks[0]:
                sp = ks[0]
                got, seen = [], set()
                for off in range(0, 0x600, 0x100):
                    w = rq(sp + off, 32)
                    for v in w:
                        if TEXT_LO <= v < TEXT_HI and v not in seen:
                            seen.add(v)
                            got.append(v)
                    if len(got) > 24:
                        break
                print(f'      ~stack from KernelStack 0x{sp:x} '
                      f'(SCAN, not an unwind - may hold stale frames):')
                for v in got[:24]:
                    print(f'        ~ {name_of(v)}')
        nxt = rq(t)
        if not nxt:
            break
        t = nxt[0]
    print(f'  {n} threads, {blocked} Waiting')
    break
