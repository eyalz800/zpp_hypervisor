r"""Name the driver image the guest is loading RIGHT NOW.

At most one image load exists in the system at any instant: three nested
exclusive locks guarantee it - `PnpDeviceActionWorker` is the sole drainer
and holds `PpDevNodeLockTree` across the whole drain, `IopLoadDriver`
takes `IopDriverLoadResource` exclusive, and `MmLoadSystemImageEx` holds
`PsLoadedModuleResource` exclusive across mapping, imports, relocation,
the page copies *and* `VslCompleteSecureDriverLoad`.

`MmLoadSystemImageEx` appends the entry to `PsLoadedModuleList` **before**
any of the slow work - before imports, before the copy loop, before the
secure-driver-load call - and failed loads are removed again, so there are
no tombstones. Therefore:

    the image being copied right now is the TAIL of PsLoadedModuleList

which turns "which driver is the 79th" into one pointer read, with no
counting and no dependence on load order. That matters because the order
is *not* the boot-driver list here: the wedge path arrives through
`PipCallDriverAddDeviceQueryRoutine` - PnP device enumeration - whose
order follows hardware enumeration timing, not
`Control\ServiceGroupOrder\List`.

usage: guest-loading-driver.py <kernel_base_hex> <cr3_hex> [count]

OFFSETS, and why they are what they are
---------------------------------------
`PsLoadedModuleList` is PDB segment 26 (`.data`, VA 0xE00000) offset
1004496 **decimal** = 0xf53d0, so RVA **0xef53d0**. It IS the LIST_ENTRY,
not a pointer to one.

`_KLDR_DATA_TABLE_ENTRY` - **not** `_LDR_DATA_TABLE_ENTRY`, which is the
user-mode type and would read plausibly wrong:

    InLoadOrderLinks 0x00 · DllBase 0x30 · SizeOfImage 0x40
    FullDllName 0x48 · BaseDllName 0x58 · Flags 0x68

`_UNICODE_STRING`: Length 0x00 (BYTES), MaximumLength 0x02, Buffer 0x08.

READER PROOF
------------
The head's `Flink` is the first loaded image, which is always
`ntoskrnl.exe`, and its `DllBase` must equal the kernel base passed in.
Two independent facts agreeing on one read. A wrong base, a wrong cr3 or
the wrong structure type all fail it; a wedged guest does not.

This is the check this tree keeps needing. A previous instrument here read
`WaitReason` through `_IRP.ThreadListEntry` instead of `_KTHREAD`'s and
produced small integers that decoded to real wait reasons for four
sessions, because a wait-reason census has no value whose correct answer
is known in advance. This one does.
"""
import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446

PSLOADEDMODULELIST_RVA = 0xef53d0
E_DLLBASE, E_SIZEOFIMAGE = 0x30, 0x40
E_FULLNAME, E_BASENAME, E_FLAGS = 0x48, 0x58, 0x68

# Set by `MiCompleteSecureDriverLoad`. If the tail already carries it, that
# image finished and nothing is mid-load.
FLAG_SECURE_LOAD_COMPLETE = 0x2000


# **Filter to DATA ROWS before matching.** The monitor ECHOES the command
# it was sent, so the response contains the literal text `xp /1xw
# 0x351f61dd0`. A physical address there is 9-12 hex digits, so a regex of
# `0x([0-9a-f]{8})` matches the ECHO's first eight digits and returns the
# address as if it were the value. `{16}` happens to be safe because an
# echoed address is never that long - which is exactly why pointer reads
# looked fine while every dword and byte read was garbage.
#
# Measured: SizeOfImage printed 0x11c71c9d where the row-filtered read
# gives 0x12b000, and Flags printed SizeOfImage+2 - two independent fields
# cannot differ by 2, which is what gave it away. Ask whether a reading is
# POSSIBLE before asking whether it is believable.
def _rows(d):
    import re as _re
    return [l for l in d.splitlines()
            if _re.match(r'^[0-9a-f]{6,}: ', l.strip())]


def monitor(cmds):
    s = socket.create_connection((RIG, PORT), timeout=12)
    time.sleep(0.35)
    for c in cmds:
        s.sendall((c + '\n').encode())
        time.sleep(0.28)
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
    d = out.decode('utf-8', 'replace')
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', d).replace('\x1b', '')


def xp_q(phys, n=1):
    d = monitor([f'xp /{n}xg 0x{phys:x}'])
    out = []
    for l in _rows(d):
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{16})', l)]
    return out


def xp_w(phys, n=1):
    d = monitor([f'xp /{n}xw 0x{phys:x}'])
    out = []
    for l in _rows(d):
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', l)]
    return out


def xp_b(phys, n):
    d = monitor([f'xp /{n}xb 0x{phys:x}'])
    out = []
    for l in _rows(d):
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', l)]
    return out


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
COUNT = int(sys.argv[3]) if len(sys.argv) > 3 else 6
_E = {}


def v2p(va):
    t = CR3
    for lvl, sh in ((0, 39), (1, 30), (2, 21), (3, 12)):
        key = (t, (va >> sh) & 0x1ff)
        e0 = _E.get(key)
        if e0 is None:
            e = xp_q(t + ((va >> sh) & 0x1ff) * 8)
            if not e:
                return None
            e0 = e[0]
            _E[key] = e0
        if not (e0 & 1):
            return None
        # .data is very likely a 2 MB large page - stop on PDE bit 7 or the
        # walk descends into what is actually page contents.
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


def rname(struct_va):
    ln = rq(struct_va)
    buf = rq(struct_va + 0x08)
    if ln is None or not buf:
        return ''
    length = ln & 0xffff
    if not (0 < length <= 512):
        return ''
    p = v2p(buf)
    if p is None:
        return ''
    bs = xp_b(p, length)
    return ''.join(chr(bs[i]) for i in range(0, len(bs), 2)
                   if 32 <= bs[i] < 127)


head = BASE + PSLOADEDMODULELIST_RVA
flink = rq(head)
blink = rq(head + 8)
print(f'PsLoadedModuleList {head:#x}  Flink {flink and hex(flink)}  '
      f'Blink {blink and hex(blink)}')
if flink is None or blink is None:
    print('  -> READ FAILED. Close any other monitor connection '
          '(`pkill -x nc`); the monitor takes exactly one.')
    sys.exit(1)
if flink == head:
    print('  -> list is EMPTY, which cannot be true of a running guest. '
          'The base or cr3 is wrong.')
    sys.exit(1)

# ---- reader proof: first entry is ntoskrnl.exe at the base we were given
first_name = rname(flink + E_BASENAME)
first_base = rq(flink + E_DLLBASE)
ok = (first_name.lower().startswith('ntoskrnl')
      and first_base == BASE)
print(f'reader proof: first entry is {first_name!r} DllBase '
      f'{first_base and hex(first_base)}')
if not ok:
    print(f'  -> READER NOT PROVEN. Expected ntoskrnl.exe with DllBase '
          f'{BASE:#x}. Nothing below would be meaningful.')
    sys.exit(1)
print('  -> proven: name and DllBase both agree')

print(f'\nlast {COUNT} entries (newest last - the TAIL is the one loading '
      f'now):')
entries = []
cur = blink
for _ in range(COUNT):
    if cur is None or cur == head:
        break
    entries.append(cur)
    cur = rq(cur + 8)          # Blink, walking backwards
for e in reversed(entries):
    nm = rname(e + E_BASENAME) or '<unreadable>'
    db = rq(e + E_DLLBASE)
    sz = rw(e + E_SIZEOFIMAGE)
    fl = rw(e + E_FLAGS)
    tail = '   <<< TAIL' if e == blink else ''
    print(f'  {nm:28s} DllBase {db and hex(db):>18}  '
          f'size {sz and hex(sz):>9}  Flags {fl and hex(fl)}{tail}')

fl = rw(blink + E_FLAGS)
nm = rname(blink + E_BASENAME) or '<unreadable>'
print()
if fl is None:
    print('tail Flags unreadable')
elif fl & FLAG_SECURE_LOAD_COMPLETE:
    print(f'tail {nm} has Flags & 0x2000 SET - MiCompleteSecureDriverLoad '
          f'already ran for it, so NO image is mid-load. Whatever is stuck '
          f'is not the image-copy path.')
else:
    print(f'tail {nm} has Flags & 0x2000 CLEAR - it has NOT completed '
          f'MiCompleteSecureDriverLoad, so this is the image currently '
          f'being loaded. On a wedged guest this names the stuck driver.')
