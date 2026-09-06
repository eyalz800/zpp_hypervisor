r"""Put a process's creation and a power watchdog's arming on ONE clock.

**Why this exists.** Boots 209 and 212 both failed to order two events -
`WerFault.exe` appearing and a power watchdog arming - because the
process poller's gap (2.2 minutes on boot 212) was wider than the
interval between them. Polling faster is the wrong fix: the events are
seconds apart and the walk itself takes minutes.

Both events already carry their own timestamp, so neither needs to be
caught live:

    _EPROCESS.CreateTime   system time, 100ns since 1601 UTC
    _POP_IRP_DATA.WatchdogStart  unbiased interrupt time, 100ns since boot

They are on **different clocks with different epochs**, which is exactly
the trap `CLAUDE.md` records for the SynIC message slot - a
`delivery_time - expiration_time` of 7.2078 s that was read as a latency
and was two clocks with different anchors. Subtracting them raw is
meaningless. The conversion is one subtraction and it is done here:

    boot_in_system_time = SystemTime_now - unbiased_interrupt_time_now
    create_in_interrupt_time = CreateTime - boot_in_system_time

after which both are seconds-since-boot and directly comparable.

**Sanity checks, and nothing prints without them.** A converted creation
time must be >= 0 (nothing is created before boot) and <= now (nothing is
created in the future). Either failing means the epoch conversion is
wrong, and a wrong conversion produces a plausible number - which is the
whole reason the SynIC figure survived twenty-five minutes of being
quoted.

Offsets: `_EPROCESS.CreateTime` = 504, taken from ntkrnlmp.pdb's field
list - the SAME list that carries `UniqueProcessId` at 464 and
`ActiveProcessLinks` at 472, the two this tree's readers already use and
whose reader proof passes. `llvm-pdbutil` reports a second `CreateTime`
at 1216 in a different structure; picking by offset alone would have
taken it.

usage: guest-when-created.py <kernel_base_hex> <cr3_hex> <process-substr>
"""
import re
import socket
import sys
import time

RIG, PORT = '192.168.1.199', 4446
PSACTIVEPROCESSHEAD = 0xf05c60
E_LINKS, E_NAME, E_CREATE = 472, 824, 504
KUSD_INTERRUPTTIME = 0xFFFFF78000000008
KUSD_INTERRUPTTIMEBIAS = 0xFFFFF780000003B0
KUSD_SYSTEMTIME = 0xFFFFF78000000014


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


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
WANT = sys.argv[3]
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


def q(va):
    p = v2p(va)
    if p is None:
        return None
    got = _vals(f'xp /1xg 0x{p:x}', 16)
    return got[0] if got else None


def astr(va, n=16):
    p = v2p(va)
    if p is None:
        return ''
    b = _vals(f'xp /{n}xb 0x{p:x}', 2)
    return ''.join(chr(c) for c in b if 32 <= c < 127)


now_it = q(KUSD_INTERRUPTTIME)
bias = q(KUSD_INTERRUPTTIMEBIAS)
sys_now = q(KUSD_SYSTEMTIME)
if None in (now_it, bias, sys_now):
    print('KUSER_SHARED_DATA unreadable - no conversion is possible.')
    sys.exit(1)
now_unbiased = now_it - bias
boot_in_systime = sys_now - now_unbiased
print(f'unbiased interrupt time now  {now_unbiased:,} = '
      f'{now_unbiased / 1e7:,.1f} s since boot')
print(f'system time now              {sys_now:,}')
print(f'-> boot, in system time      {boot_in_systime:,}')

head = BASE + PSACTIVEPROCESSHEAD
cur = q(head)
first, found = True, False
while cur and cur != head:
    ep = cur - E_LINKS
    name = astr(ep + E_NAME)
    if first:
        if name != 'System':
            print(f'  READER NOT PROVEN: first process is {name!r}, '
                  f'expected System. Nothing below is printed.')
            sys.exit(1)
        print('  reader proven: first entry of PsActiveProcessHead is `System`')
        first = False
    if WANT.lower() in name.lower():
        ct = q(ep + E_CREATE)
        if ct:
            conv = ct - boot_in_systime
            ok = 0 <= conv <= now_unbiased
            print(f'\n{name}  _EPROCESS 0x{ep:x}')
            print(f'  CreateTime (system time)     {ct:,}')
            print(f'  -> since boot                {conv:,} (100ns) = '
                  f'{conv / 1e7:,.1f} s')
            print(f'  -> that is {(now_unbiased - conv) / 1e7:,.1f} s ago')
            if not ok:
                print('  *** CONVERSION FAILED the sanity check: a creation '
                      'time must be >= 0 and <= now. The epochs do not line '
                      'up; do NOT compare this against WatchdogStart. ***')
            else:
                print('  conversion sane (0 <= create <= now); this IS '
                      'comparable with WatchdogStart directly.')
            found = True
    cur = q(cur)
if not found:
    print(f'  {WANT!r} not found in the process list')
