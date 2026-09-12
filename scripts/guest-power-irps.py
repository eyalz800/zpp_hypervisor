r"""Walk the guest's `PopIrpList` and age every power IRP in flight.

Records outstanding power requests, their devices and watchdog ages for
comparison with debugger captures. This list alone does not identify the
cause of a delay.

DEADLINES AND DEVICE OWNERSHIP
-----------------------------
Read each armed WatchdogTimer.DueTime in the interrupt-time clock domain.
Do not assume every request has the same timeout: on September 12, 2026,
the captured audio resume request had a 120-second timer, while the live
sleep timeout was 300 seconds. The earlier fixed 300-second display hid
its expiry. A due time is not a prediction of the bugcheck's execution;
DPC dispatch and completion races can delay or trigger that path.

Bugcheck parameter 2 names the PDO's enumerator. CurrentDevice identifies
the current request holder. Neither alone establishes why it stalled.

COMPARING READS
---------------
Track IRP identities, devices, watchdog state, age and completions. Several
aging requests can share a dependency; a single aging request can also be
waiting on another component. Neither pattern establishes a culprit or
proves that the delay is upstream. Correlate it with the affected thread's
call path and the event it is waiting for.

usage: guest-power-irps.py <kernel_base_hex> <cr3_hex>
"""
import sys
from qemu_monitor import read_physical


# ntoskrnl RVAs, from the agent's disassembly of the shipped image.
POPIRPLIST = 0xf0bd70              # LIST_ENTRY head; Link is at +0 of the
                                   # struct, so each entry address IS the
                                   # _POP_IRP_DATA address
# RVAs below are resolved from the PDB (segment 26 = .data at VA
# 0xE00000, offset in DECIMAL), not from a disassembly. That distinction
# is not pedantry: `PopIrpList` agreed between the two, but
# `PnpEnumerationInProgress` was 0x10 off and `PopWatchdogSleepTimeout`
# lives in segment 27 entirely - a different section base - so both read
# garbage and would have been quoted as findings.
#   PopIrpList               26:1097072 -> 0xE00000+0x10bd70 = 0xf0bd70
#   PnpEnumerationInProgress 26:1614240 -> 0xE00000+0x18a190 = 0xf8a190
#   PopIrpWorkerCount        26:1083256 -> 0xE00000+0x108778 = 0xf08778
# Section 27 begins at 0xfc5000 in the matched image. The live timeout
# globals can differ by transition; the per-request timer is used below.
PNPENUMERATIONINPROGRESS = 0xf8a190
POPIRPWORKERCOUNT = 0xf08778

# _POP_IRP_DATA, sizeof 0x138, offsets from the PDB type record.
D_IRP, D_PDO, D_CURRENTDEVICE = 0x10, 0x18, 0x28
D_WATCHDOGSTART = 0x30
# _POP_IRP_DATA.WatchdogTimer (+0x38), _KTIMER.DueTime (+0x18).
# Both offsets are from the matching PDB; DueTime uses biased interrupt time.
D_WATCHDOGDUE = 0x50
D_MINORFUNCTION, D_POWERSTATETYPE = 0xb8, 0xbc
D_WATCHDOGSTATE = 0x128

# KUSER_SHARED_DATA is at a fixed kernel VA on x64.
KUSD_INTERRUPTTIME = 0xFFFFF78000000008
KUSD_INTERRUPTTIMEBIAS = 0xFFFFF780000003B0

WATCHDOG_STATE = {0: 'Disabled', 1: 'ENABLED (armed, running)',
                  2: 'Completed'}
# Standard WDK constants - NOT from this PDB. Flagged because the tree
# requires inference to be labelled.
MINOR = {0: 'IRP_MN_WAIT_WAKE', 1: 'IRP_MN_POWER_SEQUENCE',
         2: 'IRP_MN_SET_POWER', 3: 'IRP_MN_QUERY_POWER'}


def xp_q(phys, n=1):
    return read_physical(phys, n, 8)


def xp_b(phys, n):
    return read_physical(phys, n, 1)


def xp_w(phys, n=1):
    return read_physical(phys, n, 4)


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
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


def rb(va):
    p = v2p(va)
    if p is None:
        return None
    v = xp_b(p, 1)
    return v[0] if v else None


def drivername(devobj):
    """DEVICE_OBJECT -> DriverObject(+0x08) -> DriverName(+0x38)."""
    if not devobj:
        return '<null>'
    drv = rq(devobj + 0x08)
    if not drv:
        return '<driver object unreadable>'
    ln = rq(drv + 0x38)
    buf = rq(drv + 0x40)
    if ln is None or not buf:
        return f'<name unreadable, DRIVER_OBJECT {drv:#x}>'
    length = ln & 0xffff
    if not (0 < length <= 512):
        return f'<name length {length}, DRIVER_OBJECT {drv:#x}>'
    p = v2p(buf)
    if p is None:
        return f'<name paged out, DRIVER_OBJECT {drv:#x}>'
    bs = xp_b(p, length)
    return ''.join(chr(bs[i]) for i in range(0, len(bs), 2)
                   if 32 <= bs[i] < 127) or f'<empty, {drv:#x}>'


# ---- reader proof, before anything is believed -------------------------
#
# The obvious proof - `PopWatchdogSleepTimeout` must read 600 - was tried
# first and REFUSED on a guest whose base was already known good
# (`guest-processes.py` passed its `System` cross-check with the same base
# and cr3). It read 296,207,625. So that RVA, taken from a disassembly of
# `.references/hyperv/ntoskrnl.exe`, does not hold that constant on the
# running build: either the reference image is a different build, or the
# value does not survive into the live image at that offset. Either way it
# is the wrong thing to gate on, and gating on it would have made a
# working reader look broken.
#
# What is used instead is structural and needs no constant: a LIST_ENTRY
# is only valid if its neighbour points back at it. An empty list has
# Flink == Blink == head; a populated one must satisfy
# `head->Flink->Blink == head`. A wrong address fails this; a wedged
# guest does not.
head = BASE + POPIRPLIST
flink = rq(head)
blink = rq(head + 8)
print(f'PopIrpList head {head:#x}  Flink {flink and hex(flink)}  '
      f'Blink {blink and hex(blink)}')
if flink is None or blink is None:
    print('  -> READ FAILED. Close any other monitor connection '
          '(`pkill -x nc`); the monitor takes exactly one.')
    sys.exit(1)
if flink == head and blink == head:
    print('  -> reader proven: well-formed EMPTY list')
else:
    back = rq(flink + 8)
    if back != head:
        print(f'  -> READER NOT PROVEN: head->Flink->Blink is '
              f'{back and hex(back)}, expected {head:#x}. This is not a '
              f'LIST_ENTRY, so the base or the RVA is wrong. Nothing '
              f'below would be meaningful.')
        sys.exit(1)
    print('  -> reader proven: head->Flink->Blink points back at head')

now_it = rq(KUSD_INTERRUPTTIME)
bias = rq(KUSD_INTERRUPTTIMEBIAS)
if now_it is None or bias is None:
    print('KUSER_SHARED_DATA unreadable - ages cannot be computed')
    now_unbiased = None
else:
    now_unbiased = now_it - bias
    print(f'guest interrupt time (unbiased) {now_unbiased:,} '
          f'= {now_unbiased / 1e7:,.1f} s since boot')

# `PnpEnumerationInProgress` and `PopIrpWorkerCount` are deliberately NOT
# printed. Both were tried, both read implausible values (17 for a
# BOOLEAN; 296,159,351 for a worker count, identical across two samples
# 90 s apart) with PDB-resolved RVAs on a base already proven good. An
# instrument that cannot say why it disagrees is exactly the kind this
# tree keeps getting caught by, so they are omitted rather than shown
# with a caveat somebody would later quote without it. `PopIrpList`
# stands on its own: its RVA agrees between the PDB and the
# disassembly, and the walk proves itself structurally.

head = BASE + POPIRPLIST
cur = rq(head)
if cur is None:
    print('PopIrpList head unreadable')
    sys.exit(1)
if cur == head:
    print('\nPopIrpList is EMPTY - no power IRP is in flight. '
          'A wedge here has no power IRP outstanding, so 0x9F cannot '
          'fire from this sample; a later IRP may still arm one.')
    sys.exit(0)

print(f'\nPopIrpList at {head:#x}:')
n = 0
seen = set()
previous = head
while cur and cur != head and n < 64:
    if cur in seen or rq(cur + 8) != previous:
        sys.exit('READ FAILED: cyclic or changing PopIrpList; no armed count is valid')
    seen.add(cur)
    n += 1
    irp = rq(cur + D_IRP)
    pdo = rq(cur + D_PDO)
    curdev = rq(cur + D_CURRENTDEVICE)
    start = rq(cur + D_WATCHDOGSTART)
    minor = rb(cur + D_MINORFUNCTION)
    pstype = rw(cur + D_POWERSTATETYPE)
    state = rw(cur + D_WATCHDOGSTATE)
    if None in (irp, pdo, curdev, start, minor, pstype, state) or state not in WATCHDOG_STATE:
        sys.exit('READ FAILED: incomplete power IRP; no armed count is valid')
    print(f'\n  [{n}] _POP_IRP_DATA {cur:#x}')
    print(f'      Irp {irp:#x}' if irp else '      Irp <null>')
    print(f'      WatchdogState {state} '
          f'({WATCHDOG_STATE.get(state, "?")})')
    print(f'      MinorFunction {minor} ({MINOR.get(minor, "?")}, '
          f'WDK constant not from this PDB)   '
          f'PowerStateType {pstype} '
          f'({"Device" if pstype == 1 else "System"})')
    # The two device objects, printed together and labelled, because
    # confusing them is what produced a mis-accusation once already.
    print(f'      Pdo           {pdo:#x}  {drivername(pdo)}'
          if pdo else '      Pdo           <null>')
    print(f'        ^ the ENUMERATOR (parent bus). 0x9F prints this as '
          f'P2. NOT the accused.')
    print(f'      CurrentDevice {curdev:#x}  {drivername(curdev)}'
          if curdev else '      CurrentDevice <null>')
    print(f'        ^ the driver actually HOLDING the IRP.')
    # **`WatchdogStart == 0` is NOT an age of `now`. It means the watchdog
    # was never armed, and printing `now - 0` as an age invents a number.**
    #
    # `PopAllocateIrp` memsets the whole 0x138-byte `_POP_IRP_DATA` to
    # zero, and `WatchdogStart` (+0x30) is written in exactly one place -
    # `PopEnableIrpWatchdog+0x127`, immediately after `WatchdogState = 1`.
    # So an IRP that was never armed carries zero for its entire life.
    #
    # An `IRP_MN_WAIT_WAKE` (MinorFunction 0) is never armed **by
    # design**: `PopRequestPowerIrp+0xe6` sends minor 0 straight to
    # `IofCallDriverSpecifyReturn` without going through
    # `PopQueueQuerySetIrp`, so it never reaches the watchdog, and
    # `PoDeviceAcquireIrp` only writes `CurrentDevice` for minors 2 and 3.
    # Such an IRP sits on `PopIrpList` with `CurrentDevice` NULL and
    # `WatchdogStart` 0 **for the machine's whole uptime, healthily**,
    # completing only when the device signals wake.
    #
    # This printer previously showed four such entries as "age 2,881.5 s,
    # 480.3% to bugcheck", which read as four IRPs stalled for 48 minutes
    # and was quoted as exactly that. It was `now - 0`. The tell was there
    # and was missed: four entries agreeing to the last 100 ns are not
    # four events, they are one subtraction from zero.
    if start is not None and now_unbiased is not None:
        if not start:
            print(f'      WatchdogStart 0 - the watchdog was NEVER ARMED, '
                  f'so this entry HAS NO AGE and no deadline.')
            print(f'        (an IRP_MN_WAIT_WAKE is never armed by design '
                  f'and lives here for the whole uptime - check '
                  f'MinorFunction above before reading anything into it)')
        else:
            age = now_unbiased - start
            print(f'      WatchdogStart {start:,}')
            # **A negative age is IMPOSSIBLE, not merely surprising.** A
            # watchdog cannot start in the future. Measured on boot 296:
            # `age -248,010,314 (100ns) = -24.8 s of 300 s (-8.3% to
            # bugcheck)` - printed with the same confidence as a real
            # figure, and it would have been quoted.
            #
            # The guard above rejects `WatchdogStart == 0`; this rejects
            # the other unphysical case. It happens because `start` and
            # `now_unbiased` come from different places and are not
            # sampled atomically, so a read that straddles an update can
            # invert them. Refuse rather than print: CLAUDE.md's rule is
            # to ask whether a reading is *possible* before asking
            # whether it is believable.
            if age < 0:
                print(f'      age would be {age:,} (100ns) = '
                      f'{age / 1e7:,.1f} s - **NEGATIVE, THEREFORE '
                      f'IMPOSSIBLE**. A watchdog cannot start in the '
                      f'future, so `now` and WatchdogStart were not read '
                      f'consistently. NO AGE REPORTED; re-read before '
                      f'concluding anything about this entry.')
            else:
                print(f'      age {age:,} (100ns) = {age / 1e7:,.1f} s')
    if state == 1:
        due = rq(cur + D_WATCHDOGDUE)
        timer_now = rq(KUSD_INTERRUPTTIME)
        # Reject an observed rearm/completion across this non-atomic read.
        # Do not fall back to a timeout from another request or boot.
        if (due is None or due <= 0 or timer_now is None or
                rq(cur + D_WATCHDOGSTART) != start or
                rq(cur + D_WATCHDOGDUE) != due or
                rw(cur + D_WATCHDOGSTATE) != state):
            print('      timer deadline unavailable (unreadable or changing)')
        else:
            remaining = (due - timer_now) / 1e7
            timing = (f'due in {remaining:,.1f} s' if remaining >= 0 else
                      f'overdue by {-remaining:,.1f} s')
            print(f'      timer {timing} (sampled DueTime {due:,}; '
                  'not a bugcheck countdown)')
    previous, cur = cur, rq(cur)

if cur != head or rq(head + 8) != previous:
    sys.exit('READ FAILED: incomplete or changing PopIrpList; no armed count is valid')

print(f'\n{n} power IRP(s) in flight.')
print('\nCompare IRP identities, watchdog states, ages and completions across reads.')
print('Aging requests identify outstanding work, not its cause; use the affected '
      "thread's call path and wait dependency to investigate the delay.")
