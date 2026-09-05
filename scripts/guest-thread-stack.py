#!/usr/bin/env python3
"""Print the kernel stack of a guest thread that is waiting.

The last piece of guest state this project could not read. A wait
*reason* says a thread is blocked; only its stack says on what. Used to
decide whether the `lsass` thread stuck in `WrVirtualMemory` at barrier
2 is inside the secure kernel's VAD-fault wait
(`SkmiWaitForBlockedFault` -> `SkeWaitForAlertByThreadId`) or an
ordinary page fault - two different bugs that read identically as
WaitReason 18.

For a thread that is not running, `_KTHREAD.KernelStack` (offset 88) is
its saved stack pointer and `StackBase` (8) is the top. Scanning between
them for values that fall inside the kernel image gives the return
addresses; it is a scan rather than a frame walk, so it over-reports -
stale slots survive on a stack - which is why every hit is printed with
its offset and left for the reader to judge rather than presented as a
call chain.

usage: guest-thread-stack.py <kernel_base> <cr3> <process> [wait_reason]
       wait_reason defaults to 18 (WrVirtualMemory)
"""
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
LINKS, NAME, TLIST, TENTRY, WREASON = 472, 824, 48, 32, 643
KSTACK, SBASE = 88, 8

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
def xp_q(p,n=1): return [int(x,16) for x in re.findall(r'0x([0-9a-f]{16})', monitor([f'xp /{n}xg 0x{p:x}']))]
def xp_b(p,n):  return [int(x,16) for x in re.findall(r'0x([0-9a-f]{2})', monitor([f'xp /{n}xb 0x{p:x}']))]

BASE=int(sys.argv[1],16); CR3=int(sys.argv[2],16)&0x000ffffffffff000
WANT=sys.argv[3]; WR=int(sys.argv[4]) if len(sys.argv)>4 else 18
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

cur=rq(HEAD); seen=set()
while cur and cur!=HEAD and cur not in seen:
    seen.add(cur); ep=cur-LINKS
    if WANT.lower() in rname(ep+NAME).lower():
        th=rq(ep+TLIST); thead=ep+TLIST; tseen=set(); n=0
        while th and th!=thead and th not in tseen and n<40:
            tseen.add(th); kt=th-TENTRY; n+=1
            if rb(kt+WREASON)==WR:
                ksp=rq(kt+KSTACK); base=rq(kt+SBASE)
                print(f'thread {kt:#x} WaitReason {WR}')
                print(f'  KernelStack {ksp:#x}  StackBase {base:#x}')
                if not (ksp and base and base>ksp and base-ksp < 0x8000):
                    print('  stack pointers unusable'); break
                hits=0
                for off in range(0, min(base-ksp, 0x600), 8):
                    v=rq(ksp+off)
                    if v is None: continue
                    if BASE <= v < BASE+0x2000000:
                        print(f'    +{off:#05x}  {v:#x}  ntoskrnl+{v-BASE:#x}')
                        hits+=1
                        if hits>=24: break
                print(f'  {hits} kernel-image addresses on the stack')
                break
            th=rq(th)
        break
    cur=rq(cur)
