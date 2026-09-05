#!/usr/bin/env python3
# Print one process's threads and what each is waiting on. Offsets from
# llvm-pdbutil --types: _KPROCESS.ThreadListHead 48 (EPROCESS+0x30),
# _KTHREAD.ThreadListEntry is at **760**, NOT 32. This PDB has THREE
# members named `ThreadListEntry`: offset 32 belongs to `_IRP`, 760 to
# `_KTHREAD`, 1400 to `_ETHREAD`. This script used 32 and therefore read
# WaitReason from `_ETHREAD+0x55B` - inside SchedulerApc - for every
# listing it ever printed. The values looked plausible (small integers
# that decode to real wait reasons) and were quoted as findings.
# Verified: `llvm-pdbutil dump --types` shows the 760 member sitting
# between `SuspendEvent` (736) and `MutantListHead` (776), which is the
# _KTHREAD neighbourhood; the 32 member sits between `AssociatedIrp` (24)
# and `IoStatus` (48), which is _IRP's.
# _KTHREAD.WaitReason 643 (unsigned char) - that one was always right.
#
# **And the offset being right was not enough: no WaitReason this script
# has ever printed came out of guest memory.** `xp_b` matched
# `0x([0-9a-f]{2})` against the WHOLE monitor response, and the monitor
# ECHOES the command it was sent - `xp /1xb 0x351f61dd0`. A physical
# address there is 9-12 hex digits, so the first match is the address's
# leading two digits and `rb()` returns that, every time, for every
# thread. Measured on a synthetic response: `0x35` comes back before
# `0x0d`. Threads in one pool region share a leading prefix, so the
# column read IDENTICAL for every thread and STABLE across boots, which
# is what a real wait state looks like. `{16}` in `xp_q` was safe only
# because an echoed address is never sixteen digits long - so the
# pointer walk was sound while every byte read was fabricated. Fixed by
# filtering to data rows first, the same `_rows` that
# `guest-bugcheck.py` carries.
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
LINKS, NAME, TLIST, TENTRY, WREASON = 472, 824, 48, 760, 643
# KWAIT_REASON, the ones that matter here
R = {0:'Executive',1:'FreePage',2:'PageIn',3:'PoolAllocation',
     4:'DelayExecution',5:'Suspended',6:'UserRequest',7:'WrExecutive',
     8:'WrFreePage',9:'WrPageIn',10:'WrPoolAllocation',11:'WrDelayExecution',
     12:'WrSuspended',13:'WrUserRequest',14:'WrEventPair',15:'WrQueue',
     16:'WrLpcReceive',17:'WrLpcReply',18:'WrVirtualMemory',19:'WrPageOut',
     20:'WrRendezvous',21:'WrKeyedEvent',22:'WrTerminated',23:'WrProcessInSwap',
     24:'WrCpuRateControl',25:'WrCalloutStack',26:'WrKernel',27:'WrResource',
     28:'WrPushLock',29:'WrMutex',30:'WrQuantumEnd',31:'WrDispatchInt',
     32:'WrPreempted',33:'WrYieldExecution',34:'WrFastMutex',35:'WrGuardedMutex',
     36:'WrRundown',37:'WrAlertByThreadId',38:'WrDeferredPreempt'}
def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12); time.sleep(0.35)
    for c in cmds:
        s.sendall((c+'\n').encode()); time.sleep(0.28)
    time.sleep(1.1); s.setblocking(False); out=b''
    try:
        while True:
            b=s.recv(65536)
            if not b: break
            out+=b
    except Exception: pass
    s.close()
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',out.decode('utf-8','replace'))
def _rows(d):
    # Data rows only. The echoed command is not one, and a physical
    # address inside it matches any width narrower than sixteen.
    return '\n'.join(l for l in d.splitlines()
                     if re.match(r'^[0-9a-f]{6,}: ', l.strip()))
def xp_q(p,n=1): return [int(x,16) for x in re.findall(r'0x([0-9a-f]{16})', _rows(monitor([f'xp /{n}xg 0x{p:x}'])))]
def xp_b(p,n):  return [int(x,16) for x in re.findall(r'0x([0-9a-f]{2})', _rows(monitor([f'xp /{n}xb 0x{p:x}'])))]
BASE=int(sys.argv[1],16); CR3=int(sys.argv[2],16)&0x000ffffffffff000; WANT=sys.argv[3]
HEAD=BASE+0xf05c60; _C={}
def v2p(va):
    t=CR3
    for lvl,sh in ((0,39),(1,30),(2,21),(3,12)):
        k=(t,(va>>sh)&0x1ff); e=_C.get(k)
        if e is None:
            r=xp_q(t+((va>>sh)&0x1ff)*8)
            if not r: return None
            e=r[0]; _C[k]=e
        if not (e&1): return None
        if lvl<3 and (e&0x80):
            m=(1<<sh)-1; return (e&~m&0x000fffffffffffff)|(va&m)
        t=e&0x000ffffffffff000
    return t|(va&0xfff)
def rq(va):
    p=v2p(va)
    if p is None: return None
    v=xp_q(p); return v[0] if v else None
def rb(va):
    p=v2p(va)
    if p is None: return None
    v=xp_b(p,1); return v[0] if v else None
def rname(va):
    p=v2p(va)
    if p is None: return ''
    return ''.join(chr(c) for c in xp_b(p,15) if 32<=c<127)
# **Anchor before walking.** `PsActiveProcessHead`'s first entry is
# always `System` on Windows, so reading anything else means the base or
# the head RVA is wrong and the walk below is of unrelated memory - a
# short garbage list that finds no match and prints nothing, which reads
# exactly like "that process is not running". `guest-processes.py` has
# carried this check all along and these two walkers did not.
_first=rq(HEAD)
_name=rname(_first-LINKS+NAME) if _first else ''
if not _name.lower().startswith('system'):
    sys.exit(f'ANCHOR FAILED: the first entry of PsActiveProcessHead '
             f'reads {_name!r}, not `System`. The kernel base or the '
             f'head RVA 0xf05c60 is wrong for this build, and every '
             f'name below would have come from unrelated memory.')
print('anchor: first entry of PsActiveProcessHead is `System`')
cur=_first; seen=set()
while cur and cur!=HEAD and cur not in seen:
    seen.add(cur); ep=cur-LINKS
    if WANT.lower() in rname(ep+NAME).lower():
        print(f'{rname(ep+NAME)} threads:')
        th=rq(ep+TLIST); tseen=set(); n=0
        thead=ep+TLIST
        while th and th!=thead and th not in tseen and n<40:
            tseen.add(th); kt=th-TENTRY
            wr=rb(kt+WREASON)
            print(f'  thread {kt:#x}  WaitReason {wr} = {R.get(wr,"?")}')
            th=rq(th); n+=1
        print(f'{n} threads listed')
        break
    cur=rq(cur)
