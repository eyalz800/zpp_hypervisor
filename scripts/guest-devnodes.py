#!/usr/bin/env python3
"""Walk the second-level guest's PnP device tree, live, and name what
never started.

    scripts/guest-devnodes.py <kernel-base> <windows-cr3>

Both from this VMM's own log line, which prints them together:

    second-level guest kernel image at 0xfffff8077f200000, cr3 0x1ae002

What this exists for. `guest-modules.py` answers "did the driver load",
and it has been answering it: 78 modules, ending `CLASSPNP.SYS`, with
`disk.sys` and `stornvme`'s neighbours behind it. **That is a different
question from "did the device bind", and the two have opposite
remedies.** A driver can be resident, matched to a device, and parked
for ever waiting for an interrupt that never arrives - and the module
list looks perfect the whole time.

The device tree is where that shows. Every device Windows knows about
has a `_DEVICE_NODE`, and the node carries the stage it reached
(`State`), why it stopped (`Problem`, `ProblemStatus`), and - the field
that matters most here - `StateHistory`, which distinguishes "never got
there" from "got there and fell back". A device that never bound sits at
`DeviceNodeStartPending` for ever, and nothing else in this rig's
instrument set says so.

Offsets and enumerators are from `llvm-pdbutil dump --types` on
ntkrnlmp.pdb, GUID C8A7F11B37FE28227B6B11412E3A0519 - the build
`hypervisor/include/zpp/hypervisor/guest_windows.h` names, and the same
one `symbolize-trace.py` resolves against. **This PDB is not
publics-only**: it carries full `LF_FIELDLIST` records, and
`_DEVICE_NODE` has 73 members and `sizeof` 904. Note `llvm-pdbutil
pretty` cannot read them on macOS - it needs DIA and says so - which is
what makes the type stream look absent. Use `dump --types`.

Traps this file pays for, all of them already charged to this project
once:

  - **`xp` prints its address column with NO `0x` prefix.**
  - **The monitor takes ONE connection.** A leaked socket makes every
    later reader report `None` for ever after.
  - **A structure is not a page.** `_DEVICE_NODE` is 904 bytes and an
    instance path is routinely longer than a monitor line; either can
    straddle a page boundary, and one `v2p` covers only the first. Every
    read here is split at the boundary for that reason. `guest-modules.py`
    caps a name at 64 characters and would silently render a
    `PCI\VEN_...` path as the empty string.
  - **`Problem == 0` is not health.** It is also the value for a node
    never processed, and for one whose problem was cleared on a retry.
    It is printed only beside `State`, never alone - the two-field census
    this tree's own notes insist on.
"""
import re
import socket
import sys
import time

RIG, PORT = "192.168.1.199", 4446

# `_DEVICE_NODE`, verified member by member against the type stream.
SIBLING, CHILD, PARENT, LAST_CHILD = 0, 8, 16, 24
PHYSICAL_DEVICE_OBJECT = 32
INSTANCE_PATH, SERVICE_NAME = 40, 56
PENDING_IRP = 72
STATE, PREVIOUS_STATE = 300, 304
STATE_HISTORY, STATE_HISTORY_ENTRY = 308, 388
COMPLETION_STATUS = 392
FLAGS, USER_FLAGS, PROBLEM, PROBLEM_STATUS = 396, 400, 404, 408
RESOURCE_LIST, RESOURCE_LIST_TRANSLATED = 416, 424
DEVICE_NODE_SIZE = 904

# `_DEVICE_OBJECT` and `_DRIVER_OBJECT`, same source.
DO_DRIVER_OBJECT, DO_ATTACHED_DEVICE, DO_CURRENT_IRP = 8, 24, 32
DRV_DRIVER_NAME = 56

# `IopRootDeviceNode`, `PnPBootDriversLoaded` and
# `PnPBootDriversInitialized` are 0x20 and 0x22 apart in `.data`, so all
# three land in one page and cost one translation between them.
IOP_ROOT_DEVICE_NODE = 0xF8BA58
PNP_BOOT_DRIVERS_LOADED = 0xF8BA78
PNP_BOOT_DRIVERS_INITIALIZED = 0xF8BA7A

# `_PNP_DEVNODE_STATE` (LF_ENUM 0x232D). **The base is 768, not 0** - a
# state printed as a small integer is a state read from the wrong offset.
DEVNODE_STATE = {
    768: "Unspecified", 769: "Uninitialized", 770: "UninitializedPending",
    771: "InitializedPending", 772: "Initialized", 773: "DriversAdded",
    774: "ResourcesAssigned", 775: "StartPending", 776: "StartCompletion",
    777: "StartPostWork", 778: "Started", 779: "QueryStopped",
    780: "Stopped", 781: "RestartCompletion", 782: "EnumeratePending",
    783: "EnumerateCompletion", 784: "AwaitingQueuedDeletion",
    785: "AwaitingQueuedRemoval", 786: "QueryRemoved",
    787: "RemovePendingCloses", 788: "Removed", 789: "DeletePendingCloses",
    790: "Deleted", 791: "MaxDeviceNodeState",
}

STARTED = 778

# What each stopping point means, so the reading does not need the
# sequence re-derived. Every one of these is a stage `stornvme` must pass
# to own the disk.
STATE_MEANING = {
    772: "enumerated, but PnP matched no driver to it yet",
    773: "driver matched - resources NOT yet assigned",
    774: "resources assigned - IRP_MN_START_DEVICE not yet issued",
    775: "START ISSUED AND NEVER COMPLETED - the driver is inside "
         "StartDevice and has not returned",
    776: "start completing",
    777: "start post-work",
}

# `Problem` values. **These are from general knowledge, NOT from this
# PDB** - the field is typed `unsigned long` and no `_CM_PROB` enum is in
# the type stream. So they are printed with the raw number always beside
# them, and a name here must be checked before it is quoted anywhere.
PROBLEM_NAME = {
    0: "none recorded (NOT proof of health - see the header)",
    1: "CM_PROB_NOT_CONFIGURED?", 3: "CM_PROB_OUT_OF_MEMORY?",
    10: "CM_PROB_FAILED_START?", 12: "CM_PROB_NO_VALID_LOG_CONFIG?",
    13: "CM_PROB_FAILED_FILTER?", 14: "CM_PROB_NEED_RESTART?",
    18: "CM_PROB_REINSTALL?", 19: "CM_PROB_REGISTRY?",
    22: "CM_PROB_DISABLED?", 24: "CM_PROB_DEVICE_NOT_THERE?",
    28: "CM_PROB_FAILED_INSTALL?", 31: "CM_PROB_FAILED_ADD?",
    35: "CM_PROB_UNSUPPORTED_DEVICE?", 36: "CM_PROB_IRQ_TRANSLATION_FAILED?",
    51: "CM_PROB_WAITING_ON_DEPENDENCY?", 52: "CM_PROB_UNSIGNED_DRIVER?",
}


def monitor(commands):
    sock = socket.create_connection((RIG, PORT), timeout=12)
    time.sleep(0.35)
    for command in commands:
        sock.sendall((command + "\n").encode())
        time.sleep(0.28)
    time.sleep(1.1)
    sock.setblocking(False)
    out = b""
    try:
        while True:
            block = sock.recv(65536)
            if not block:
                break
            out += block
    except Exception:
        pass
    sock.close()
    text = out.decode("utf-8", "replace")
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text).replace("\x1b", "")


def xp_bytes(physical, count):
    """`count` bytes at a physical address, as a bytes object.

    Byte granularity on purpose. The fields wanted here are a mixture of
    quadwords, longs, shorts and single `UCHAR` flags, and slicing one
    buffer is the only way to read them without a separate round trip and
    a separate chance to mis-scale each.
    """
    text = monitor([f"xp /{count}xb 0x{physical:x}"])
    got = bytearray()
    for line in text.splitlines():
        head = re.match(r"^\s*([0-9a-f]{4,16}):\s+(.*)$", line)
        if not head:
            continue
        for value in re.findall(r"0x([0-9a-f]{2})\b", head.group(2)):
            got.append(int(value, 16))
    return bytes(got[:count])


CR3 = 0
_ENTRY = {}


def xp_q(physical, count=1):
    text = monitor([f"xp /{count}xg 0x{physical:x}"])
    return [int(v, 16) for v in re.findall(r"0x([0-9a-f]{16})", text)]


def v2p(va):
    """Four-level walk, with the page-table entries cached.

    Without the cache every field costs four round trips and the walk
    takes longer than a boot. Kernel space maps through very few tables,
    so this turns minutes into seconds - the same argument
    `guest-modules.py` makes.
    """
    table = CR3
    for level, shift in ((0, 39), (1, 30), (2, 21), (3, 12)):
        index = (va >> shift) & 0x1FF
        key = (table, index)
        entry = _ENTRY.get(key)
        if entry is None:
            got = xp_q(table + index * 8)
            if not got:
                return None
            entry = got[0]
            _ENTRY[key] = entry
        if not (entry & 1):
            return None
        if level < 3 and (entry & 0x80):
            mask = (1 << shift) - 1
            return (entry & ~mask & 0x000FFFFFFFFFFFFF) | (va & mask)
        table = entry & 0x000FFFFFFFFFF000
    return table | (va & 0xFFF)


def read(va, count):
    """`count` bytes at a virtual address, split at every page boundary.

    **This is the part a simpler reader gets wrong.** One `v2p` answers
    for one page; a `_DEVICE_NODE` is 904 bytes and an instance path is
    often longer than a page's remainder, so a single translation covers
    the head and the tail comes from whatever happens to follow it
    physically. That does not fail - it returns plausible bytes.

    Returns None if any page in the range fails to translate, rather
    than a short buffer: a truncated structure read as a whole one is
    the same class of error.
    """
    out = b""
    while count:
        physical = v2p(va)
        if physical is None:
            return None
        chunk = min(count, 0x1000 - (va & 0xFFF))
        got = xp_bytes(physical, chunk)
        if len(got) != chunk:
            return None
        out += got
        va += chunk
        count -= chunk
    return out


def u64(buf, at):
    return int.from_bytes(buf[at:at + 8], "little")


def u32(buf, at):
    return int.from_bytes(buf[at:at + 4], "little")


def s32(buf, at):
    return int.from_bytes(buf[at:at + 4], "little", signed=True)


def unicode_string(buf, at):
    """A `_UNICODE_STRING` embedded at `at`: Length, MaximumLength, Buffer.

    Embedded, not pointed to - 16 bytes, `Buffer` at +8. The length is in
    *bytes*, so the character count is half it.
    """
    length = int.from_bytes(buf[at:at + 2], "little")
    buffer = u64(buf, at + 8)
    if not buffer or not length:
        return ""
    # A generous but finite bound. `guest-modules.py` uses 64 and a PCI
    # instance path exceeds it, which renders as the empty string and
    # reads as "this node has no name".
    if length > 1024:
        return f"<length {length} implausible>"
    raw = read(buffer, length)
    if raw is None:
        return "<unreadable>"
    text = raw.decode("utf-16-le", "replace")
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in text)


def driver_name(device_object):
    """`DEVICE_OBJECT -> DriverObject -> DriverName`, e.g. `\\Driver\\pci`."""
    if not device_object:
        return ""
    buf = read(device_object, 64)
    if buf is None:
        return "<unreadable>"
    driver = u64(buf, DO_DRIVER_OBJECT)
    if not driver:
        return ""
    head = read(driver, 72)
    if head is None:
        return "<unreadable>"
    return unicode_string(head, DRV_DRIVER_NAME)


def driver_stack(pdo):
    """Every driver attached above the physical device object.

    The PDO belongs to the bus driver - `\\Driver\\pci` - and each
    `AttachedDevice` above it is a function or filter driver. For an NVMe
    disk a healthy stack is pci, then storport/stornvme, then partmgr and
    disk. A stack that is only the bus driver means nothing ever attached,
    which is a different failure from one that attached and hung.
    """
    names, device, seen = [], pdo, set()
    for _ in range(16):
        if not device or device in seen:
            break
        seen.add(device)
        name = driver_name(device)
        if name:
            names.append(name)
        buf = read(device, 32)
        if buf is None:
            break
        device = u64(buf, DO_ATTACHED_DEVICE)
    return names


def describe(node, buf):
    state = u32(buf, STATE)
    problem = u32(buf, PROBLEM)
    return {
        "node": node,
        "instance": unicode_string(buf, INSTANCE_PATH),
        "service": unicode_string(buf, SERVICE_NAME),
        "state": state,
        "previous": u32(buf, PREVIOUS_STATE),
        "problem": problem,
        "problem_status": s32(buf, PROBLEM_STATUS) & 0xFFFFFFFF,
        "completion": s32(buf, COMPLETION_STATUS) & 0xFFFFFFFF,
        "flags": u32(buf, FLAGS),
        "pdo": u64(buf, PHYSICAL_DEVICE_OBJECT),
        "pending_irp": u64(buf, PENDING_IRP),
        "resources": u64(buf, RESOURCE_LIST),
        "resources_translated": u64(buf, RESOURCE_LIST_TRANSLATED),
        "history": [u32(buf, STATE_HISTORY + 4 * i) for i in range(20)],
        "history_entry": u32(buf, STATE_HISTORY_ENTRY),
    }


def state_name(value):
    return DEVNODE_STATE.get(value, f"?{value}")


def main():
    global CR3
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    base = int(sys.argv[1], 16)
    CR3 = int(sys.argv[2], 16) & 0x000FFFFFFFFFF000

    # One read for the root pointer and both boot-driver flags - they are
    # 0x20 and 0x22 past it, so this is a single page and a single
    # translation.
    head = read(base + IOP_ROOT_DEVICE_NODE, 0x30)
    if head is None:
        sys.exit("could not translate IopRootDeviceNode - is the kernel "
                 "base right, and is this Windows' CR3 rather than the "
                 "guest hypervisor's? (see guest-walk.py's header)")
    root = u64(head, 0)
    loaded = head[PNP_BOOT_DRIVERS_LOADED - IOP_ROOT_DEVICE_NODE]
    initialized = head[PNP_BOOT_DRIVERS_INITIALIZED - IOP_ROOT_DEVICE_NODE]

    print(f"IopRootDeviceNode  0x{root:x}")
    if root < 0xFFFF800000000000:
        print("  NOT A CANONICAL KERNEL ADDRESS. The base or the CR3 is "
              "wrong, and every byte below would be fiction. Stopping.")
        return
    print(f"PnPBootDriversLoaded       {loaded}")
    print(f"PnPBootDriversInitialized  {initialized}")
    if not initialized:
        print("  -> Windows is still INSIDE boot-driver initialization. "
              "The failure is a DriverEntry that has not returned, not a "
              "device that has not started.")
    else:
        print("  -> boot-driver initialization returned; anything stuck "
              "below is a device that did not start.")

    # Depth first, with a visited set and a hard bound. A tree read out of
    # another operating system's memory is not something to trust to
    # terminate, and a torn read turns a tree into a cycle.
    nodes, seen, stack, truncated = [], set(), [(root, 0)], False
    while stack:
        node, depth = stack.pop()
        if node in seen or node < 0xFFFF800000000000:
            continue
        if len(nodes) >= 512:
            truncated = True
            break
        seen.add(node)
        buf = read(node, DEVICE_NODE_SIZE)
        if buf is None:
            print(f"  [node 0x{node:x} unreadable - subtree skipped]")
            continue
        info = describe(node, buf)
        info["depth"] = depth
        nodes.append(info)
        sibling, child = u64(buf, SIBLING), u64(buf, CHILD)
        if sibling:
            stack.append((sibling, depth))
        if child:
            stack.append((child, depth + 1))

    print(f"\n{len(nodes)} device nodes"
          + ("  (BOUND HIT - the tree is larger than this)" if truncated
             else ""))
    print("  state                 problem  service       instance")
    for info in nodes:
        indent = "  " + "  " * info["depth"]
        mark = " " if info["state"] == STARTED else "*"
        print(f"{mark}{indent}{state_name(info['state']):<20} "
              f"{info['problem']:>7}  {info['service']:<13} "
              f"{info['instance']}")

    stuck = [n for n in nodes if n["state"] != STARTED]
    print(f"\n{len(stuck)} of {len(nodes)} node(s) are not "
          f"DeviceNodeStarted")
    if not stuck:
        print("  Every device started. The wedge is ABOVE the device "
              "tree - storage bound, and the failure is elsewhere. "
              "`guest-processes.py` is the next reading.")
        return

    for info in stuck:
        print(f"\n  node 0x{info['node']:x}  {info['instance'] or '<no path>'}")
        print(f"    service            {info['service'] or '<none>'}")
        print(f"    state              {state_name(info['state'])}"
              f"  ({info['state']})")
        meaning = STATE_MEANING.get(info["state"])
        if meaning:
            print(f"                       {meaning}")
        print(f"    previous state     {state_name(info['previous'])}")
        print(f"    problem            {info['problem']}  "
              f"{PROBLEM_NAME.get(info['problem'], '')}")
        print(f"    problem status     0x{info['problem_status']:08x}")
        print(f"    completion status  0x{info['completion']:08x}")
        print(f"    pending IRP        0x{info['pending_irp']:x}"
              + ("   <- an IRP is outstanding on this node"
                 if info["pending_irp"] else ""))
        # The field that separates "never got there" from "got there and
        # fell back", and no other field can.
        history = [state_name(v) for v in info["history"] if v]
        print(f"    state history      {' -> '.join(history) or '<empty>'}")
        # Resources are the step before START. Translated NULL with the
        # node past DriversAdded means PnP never assigned any, which is a
        # different failure from a driver that got resources and hung.
        print(f"    resource list      0x{info['resources']:x}  "
              f"translated 0x{info['resources_translated']:x}")
        if info["state"] >= 774 and not info["resources_translated"]:
            print("                       NO TRANSLATED RESOURCES past "
                  "ResourcesAssigned - the arbiters produced nothing")
        names = driver_stack(info["pdo"])
        print(f"    driver stack       {' | '.join(names) or '<none>'}")
        if len(names) <= 1:
            print("                       ONLY THE BUS DRIVER - nothing "
                  "ever attached above the PDO")

    print("\nHow to read this, in one line each:")
    print("  StartPending + a pending IRP + a full driver stack")
    print("      -> the driver is inside StartDevice waiting on the "
          "device. For NVMe that is Identify not completing, which is an "
          "interrupt that never arrived.")
    print("  DriversAdded/ResourcesAssigned + no translated resources")
    print("      -> PnP never gave it resources; the failure is in "
          "arbitration or config space, not in the driver.")
    print("  Initialized + no service name")
    print("      -> no driver was matched at all.")
    print("\nAnd the standing caution: `problem 0` above means NOTHING "
          "was recorded, not that nothing is wrong. Read it only beside "
          "the state.")


if __name__ == "__main__":
    main()
