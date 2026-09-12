#!/usr/bin/env python3
"""List the second-level guest's loaded kernel modules, live.

    scripts/guest-modules.py <kernel-base> <windows-cr3>

Both from this VMM's own log line, which prints them together:

    second-level guest kernel image at 0xfffff8077f200000, cr3 0x1ae002

Walks `PsLoadedModuleList` (RVA 0xef53d0) physically with `xp`, for the
same reason `guest-processes.py` does: the monitor's `x` translates
through whichever processor is selected, and that one is usually inside
the guest hypervisor's address space, where it answers "Cannot access
memory" - a fact about the mapping, not about the guest.

Offsets are from `llvm-pdbutil dump --types` on ntkrnlmp.pdb -
`_LDR_DATA_TABLE_ENTRY.InLoadOrderLinks` 0, `BaseDllName` 88 - and are
per build.

**It caches page-table entries.** Without that each name costs four
monitor round trips and the walk takes longer than a boot; kernel space
maps through very few tables, so the cache turns minutes into seconds.

What it is for: "has the storage stack started" and "did the driver
load" have no other answer here, and both were guessed at - wrongly -
for a long time before this existed.
"""
import re, socket, sys, time
RIG, PORT = '192.168.1.199', 4446
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
    d=out.decode('utf-8','replace')
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',d).replace('\x1b','')
# Data rows only. The monitor echoes the command, and a physical address
# in the echo is 9-12 hex digits, so anything narrower than `{16}` can
# match it. `xp_w`'s `{4}` was correct here **by arithmetic accident**:
# the artefact is the address's first four digits, so it is at least
# 0x1000 for any address of four or more digits and `rname`'s
# `32 <= w < 127` filter always dropped it. That is a property of the
# guest's RAM size, not of this code, so it is anchored properly now.
def _rows(d):
    return '\n'.join(l for l in d.splitlines()
                     if re.match(r'^[0-9a-f]{6,}: ', l.strip()))
def xp_q(phys, n=1):
    d = _rows(monitor([f'xp /{n}xg 0x{phys:x}']))
    return [int(x,16) for x in re.findall(r'0x([0-9a-f]{16})', d)]
def xp_w(phys, n):
    d = _rows(monitor([f'xp /{n}xh 0x{phys:x}']))
    return [int(x,16) for x in re.findall(r'0x([0-9a-f]{4})', d)]
CR3 = int(sys.argv[2],16) & 0x000ffffffffff000
_ENTRY = {}
def v2p(va):
    t = CR3
    for lvl, sh in ((0,39),(1,30),(2,21),(3,12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _ENTRY.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff)*8)
            if not e: return None
            e0 = e[0]; _ENTRY[key] = e0
        if not (e0 & 1): return None
        if lvl < 3 and (e0 & 0x80):
            mask=(1<<sh)-1; return (e0 & ~mask & 0x000fffffffffffff) | (va & mask)
        t = e0 & 0x000ffffffffff000
    return t | (va & 0xfff)
def rq(va):
    p=v2p(va)
    if p is None: return None
    v=xp_q(p); return v[0] if v else None
base=int(sys.argv[1],16); head=base+0xef53d0
def name_at(entry):
    # BaseDllName is a UNICODE_STRING at +88: Length(2) MaxLength(2) pad Buffer(8)
    ln = rq(entry+88)
    buf = rq(entry+88+8)
    if ln is None or not buf: return ''
    n=(ln & 0xffff)//2
    p=v2p(buf)
    if not p or not (0 < n <= 64): return ''
    return ''.join(chr(w) for w in xp_w(p, n) if 32 <= w < 127)
# Optional third argument: an address to place. Prints the module whose
# [DllBase, DllBase+SizeOfImage) contains it, which is how a bare pointer
# out of a KDPC, a stack frame or an exit trace becomes a driver name.
# Without it the walk behaves exactly as before.
#
# `DllBase` is at +0x30 and `SizeOfImage` at +0x40 in
# _LDR_DATA_TABLE_ENTRY, beside the `BaseDllName` at +88 this already
# reads. Costs two more reads per module, both through the same cached
# page tables.
want = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
found = None

# **READER PROOF, ported from `guest-loading-driver.py:40-45`.** The
# head's `Flink` is the first loaded image, which is always
# `ntoskrnl.exe`, and its `DllBase` must equal the kernel base passed in
# - two independent facts agreeing on one read. A wrong base, a wrong
# cr3 or the wrong structure type all fail it; a wedged guest does not.
# This file is the one that answers "did the driver load" and the one
# whose output gets quoted, and it was the one without the proof.
#
# (The offsets here are documented as `_LDR_DATA_TABLE_ENTRY`, which is
# the user-mode type its sibling explicitly names as the trap. The
# numbers agree with `_KLDR_DATA_TABLE_ENTRY` so nothing is broken, but
# the justification pointed at the wrong type - and this check is what
# settles it either way.)
_first=rq(head)
_dllbase=rq(_first+0x30) if _first else None
if _dllbase != base:
    sys.exit(f'READER PROOF FAILED: the first module\'s DllBase reads '
             f'{_dllbase if _dllbase is None else hex(_dllbase)}, and '
             f'the kernel base handed in is {base:#x}. They must be the '
             f'same address - the first entry of PsLoadedModuleList is '
             f'ntoskrnl.exe. The base, the cr3 or the head RVA 0xef53d0 '
             f'is wrong, and the list below would have been unrelated '
             f'memory.')
print(f'reader proven: first module DllBase == kernel base {base:#x}')

cur=rq(head); seen=set(); names=[]; why='ran out of iterations'
for _ in range(400):
    if cur is None: why='READ FAILED walking Flink - the walk is truncated, not the list'; break
    if cur==head: why='reached the list head - complete'; break
    if cur in seen: why='LOOP: Flink revisited an entry - corrupt or torn read'; break
    seen.add(cur)
    nm = name_at(cur)
    names.append(nm or '<name unreadable>')
    if want is None:
        print(' ', names[-1], flush=True)
    else:
        dll = rq(cur + 0x30)
        siz = rq(cur + 0x40)
        siz = (siz & 0xffffffff) if siz is not None else None
        if dll and siz:
            print(f'  {names[-1]:28s} {dll:#018x} + {siz:#x}', flush=True)
            if dll <= want < dll + siz:
                found = (names[-1], dll, siz)
    cur=rq(cur)

if want is not None:
    print()
    if found:
        nm, dll, siz = found
        print(f'{want:#x} is in {nm}  (base {dll:#x}, size {siz:#x}, '
              f'offset +{want - dll:#x})')
    else:
        # Say so rather than printing nothing: an address in no module is
        # a real answer (pool, a dynamically generated thunk, or a
        # truncated walk - check the completion line below before
        # believing the first two).
        print(f'{want:#x} is in NO module on this walk - check the walk '
              f'completed before reading that as "not a driver"')
print(f'{len(names)} modules; walk ended because: {why}')
# The forward walk cannot report its own truncation: a page that fails to
# translate ends it silently, and the result is indistinguishable from a
# list that really stopped there - the same result twice, in two runs
# sixty seconds apart, because the same page fails to translate both
# times. So the last entry is read a second way, from the head's Blink,
# which costs one round trip and does not depend on any of the entries in
# between.
#
# Read the two together:
#   Blink name == last forward name   -> the list really does end there
#   Blink name != last forward name   -> the forward walk was truncated,
#                                        and Blink names the real last
#                                        module loaded
blink=rq(head+8)
if blink is None:
    print('blink: READ FAILED - cannot cross-check')
else:
    bn=name_at(blink) or '<name unreadable>'
    last=names[-1] if names else None
    print(f'blink (last loaded module, read independently): {bn}')
    if last is not None:
        print('cross-check: AGREES - list genuinely ends here' if bn==last
              else f'cross-check: DISAGREES - forward walk stopped at '
                   f'{last!r} but the list ends at {bn!r}; the forward '
                   f'walk is truncated and {len(names)} is a floor')
for n in names: print(' ', n)
