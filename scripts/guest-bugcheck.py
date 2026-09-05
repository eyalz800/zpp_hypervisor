#!/usr/bin/env python3
r"""Read the guest's `KiBugCheckData` and name the driver that caused it.

There is no other way to see a stop code on this rig: the display is a
passed-through GPU and QEMU answers `screendump` with "There is no console
to take a screendump from". Windows writes the five words of
`KiBugCheckData` - the stop code and its four parameters - before anything
is displayed, so they are readable from outside with no debugger and
nothing perturbed.

usage: guest-bugcheck.py <kernel_base_hex> <cr3_hex>

Get both from a state dump: `kernel base 0x...` and the cr3 the second
level runs Windows under. Both move every boot (KASLR), so read them per
run and never carry one between builds.

WHY THE OFFSETS ARE WHAT THEY ARE
---------------------------------
`llvm-pdbutil dump --publics` prints `addr = SEGMENT:OFFSET` with the
**offset in decimal** and the segment indexing the PE section table.
Reading either as hex puts every symbol somewhere plausible and wrong.

    KiBugCheckData        segment 26 (.data, VA 0xE00000) offset 1190336
                          -> RVA 0xE00000 + 0x1229c0 = 0xf229c0

Cross-checked against `PsActiveProcessHead`, whose published offset
1072224 gives 0xE00000 + 0x105c60 = 0xf05c60 - the value
`guest-processes.py` already uses and which is known good. If one of the
two arithmetic paths were wrong, they would not both land on a known
value.

A wrong base or cr3 does not fail loudly, it prints plausible garbage, so
this refuses to be believed unless the stop code looks like a stop code.

BUGCHECK 0x9F, DRIVER_POWER_STATE_FAILURE
-----------------------------------------
With param1 == 3 the parameters are:

    param2  the DEVICE_OBJECT that blocked the IRP
    param3  a pointer to nt!TRIAGE_9F_POWER
    param4  the blocked IRP

`_DEVICE_OBJECT.DriverObject` is at +0x08 and `_DRIVER_OBJECT.DriverName`
(a UNICODE_STRING: Length u16, MaximumLength u16, Buffer at +0x08) is at
+0x38, so the name is two pointer hops from the parameter. That is how
`\Driver\IntcAudioBus` was identified rather than guessed.

**A stop code is a timeout, not an accusation.** 0x9F fires after a
watchdog interval, so a machine livelocked for that long produces it
whichever driver happened to be holding an IRP. Read the name as "who was
holding it", and settle cause-versus-consequence separately.
"""
import re, socket, sys, time

RIG, PORT = '192.168.1.199', 4446

# RVAs derived above; see the docstring for the decimal/segment trap.
KIBUGCHECKDATA_RVA = 0xf229c0

# `_DEVICE_OBJECT.DriverObject`, then `_DRIVER_OBJECT`'s DriverStart,
# DriverSize and the DriverName UNICODE_STRING. All from the PDB.
DEVOBJ_DRIVEROBJECT = 0x08
DRVOBJ_START, DRVOBJ_SIZE = 0x18, 0x20
DRVOBJ_NAME = 0x38          # UNICODE_STRING; Buffer is at +0x08 of it

# Bugchecks worth naming inline. The list is deliberately short - a code
# that is not here still prints its number, which is the thing that
# matters.
CODES = {
    0x9f: 'DRIVER_POWER_STATE_FAILURE',
    0x1ca: 'SYNTHETIC_WATCHDOG_TIMEOUT',
    0x133: 'DPC_WATCHDOG_VIOLATION',
    0xa: 'IRQL_NOT_LESS_OR_EQUAL',
    0x50: 'PAGE_FAULT_IN_NONPAGED_AREA',
    0x139: 'KERNEL_SECURITY_CHECK_FAILURE',
    0x1e: 'KMODE_EXCEPTION_NOT_HANDLED',
    0x7e: 'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED',
    0xef: 'CRITICAL_PROCESS_DIED',
    0x5c: 'HAL_INITIALIZATION_FAILED',
}

# `DRIVER_POWER_STATE_FAILURE`'s first parameter selects what the other
# three mean. Only the case actually seen here is decoded in full.
P1_9F = {
    0x1: 'a device object has been blocking an IRP for too long',
    0x2: 'the device object completed the IRP but did not call '
         'PoStartNextPowerIrp',
    0x3: 'a device object has been blocking an IRP for too long',
    0x4: 'the power IRP timed out',
    0x500: 'a thread is stuck in a device driver power routine',
}


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


def xp_b(phys, n):
    d = monitor([f'xp /{n}xb 0x{phys:x}'])
    out = []
    for l in _rows(d):
        out += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', l)]
    return out


BASE = int(sys.argv[1], 16)
CR3 = int(sys.argv[2], 16) & 0x000ffffffffff000
_E = {}


def v2p(va):
    """Walk the guest's own page tables. The extended page tables have
    measured identity for these pages, so a second-level physical address
    is an `xp` address directly."""
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


def runicode(struct_va):
    """Read a UNICODE_STRING at `struct_va`. Returns '' on any failure -
    an unreadable name is not an error, it is a paged-out buffer."""
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


va = BASE + KIBUGCHECKDATA_RVA
phys = v2p(va)
print(f'KiBugCheckData va {va:#x} -> phys '
      f'{"UNMAPPED" if phys is None else hex(phys)}')
if phys is None:
    print('READ FAILED - the walk did not resolve. This is a mapping '
          'fact, not evidence about the guest: on a frozen guest both '
          'processors are usually in the guest hypervisor address space. '
          'Check the base and cr3 came from THIS boot.')
    sys.exit(1)

vals = xp_q(phys, 5)
if len(vals) < 5:
    print(f'READ FAILED - got {len(vals)} of 5 words. Close any other '
          'monitor connection (`pkill -x nc`); the monitor takes one.')
    sys.exit(1)

code = vals[0]
print(f'  STOP CODE  {code:#x}  {CODES.get(code, "")}')
for i, v in enumerate(vals[1:], 1):
    print(f'  param {i}    {v:#x}')

# A guest that never bugchecked leaves this zero, and that is a real
# answer rather than a failure - say so, so it is not read as a broken
# reader.
if 0 == code:
    print('\n-> stop code is ZERO: this guest has NOT bugchecked. If the '
          'VM is stopped, it reset for another reason (1->2 CPUs is a '
          'hardware change Windows reboots for). Check the last exit in '
          "this VMM's own ring instead.")
    sys.exit(0)

if code not in CODES:
    print('\n-> stop code is not one this script names. It is still the '
          'value Windows wrote; look it up rather than doubting it.')

if 0x9f == code:
    p1 = vals[1]
    print(f'\nDRIVER_POWER_STATE_FAILURE, param1 {p1:#x}: '
          f'{P1_9F.get(p1, "see the reference for this subtype")}')
    if p1 in (0x3, 0x1):
        devobj = vals[2]
        print(f'  DEVICE_OBJECT {devobj:#x}   blocked IRP {vals[4]:#x}')
        drv = rq(devobj + DEVOBJ_DRIVEROBJECT)
        if not drv:
            print('  -> DriverObject unreadable')
            sys.exit(0)
        start = rq(drv + DRVOBJ_START)
        size = rq(drv + DRVOBJ_SIZE)
        name = runicode(drv + DRVOBJ_NAME)
        print(f'  -> DRIVER_OBJECT {drv:#x}')
        print(f'     DriverStart {start:#x}  DriverSize {size:#x}'
              if start is not None and size is not None else
              '     DriverStart/Size unreadable')
        if name:
            print(f'     DRIVER NAME: {name}')
            print('\n  NOTE: 0x9F is a WATCHDOG TIMEOUT. This names who '
                  'was holding an IRP when the timer expired, which is '
                  'not the same as what caused the stall - a machine '
                  'livelocked for the timeout produces this whichever '
                  'driver happened to hold one. Settle cause versus '
                  'consequence separately.')
        else:
            print('     DriverName unreadable (paged out) - use '
                  'DriverStart with scripts/guest-modules.py instead')
