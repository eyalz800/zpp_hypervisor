# One pass, one connection, timestamped: cpu0 current thread + its
# NextProcessor, cpu1 current thread + NextThread. The pieces of the
# phase-1 argument read together rather than minutes apart.
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
def monitor(cmds):
    s=socket.create_connection((RIG,PORT),timeout=12); time.sleep(0.35)
    for c in cmds: s.sendall((c+'\n').encode()); time.sleep(0.25)
    time.sleep(1.0); s.setblocking(False); out=b''
    try:
        while True:
            b=s.recv(65536)
            if not b: break
            out+=b
    except Exception: pass
    s.close()
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',out.decode('utf-8','replace'))
def xp_q(p,n=1): return [int(x,16) for x in re.findall(r'0x([0-9a-f]{16})', monitor([f'xp /{n}xg 0x{p:x}']))]
CR3=int(sys.argv[1],16)&0x000ffffffffff000
KPB=int(sys.argv[2],16)   # KiProcessorBlock VA
_C={}
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
t0=time.time()
prcb=[rq(KPB), rq(KPB+8)]
out={}
for i,p in enumerate(prcb):
    if not p: continue
    cur=rq(p+0x08); nxt=rq(p+0x10); idle=rq(p+0x18)
    npx=rq(cur+536) if cur else None
    out[i]=(p,cur,nxt,idle,npx)
t1=time.time()
print(f'single pass, span {t1-t0:.1f}s')
for i,(p,cur,nxt,idle,npx) in out.items():
    tag='IDLE' if cur==idle else 'busy'
    print(f'  cpu {i} PRCB {p:#x}')
    print(f'    CurrentThread {cur:#x}  ({tag})')
    print(f'    NextThread    {nxt:#x}')
    print(f'    cur.NextProcessor(+536) {npx:#x}' if npx is not None else '    NextProcessor unreadable')
