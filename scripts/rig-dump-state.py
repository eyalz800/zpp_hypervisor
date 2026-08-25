#!/usr/bin/env python3
"""Dump the resident hypervisor's per-processor state through the emulator's
monitor, reading *physical* memory.

Why this exists rather than a gdb session: the module clears every
extended-page-table permission on its own pages, so from guest context those
addresses read as `Cannot access memory`, and a settled guest has no
processor in root operation to read them from.  `xp` reads physical memory,
which bypasses both the extended page tables and guest paging, and the module
base printed on serial *is* a physical address.

Why it asks the ELF for every offset rather than carrying a table: member
offsets move whenever a member is added, and stale offsets do not fail - they
return plausible zeroes.  That happened twice, and each time the zeroes were
read as "the processor did nothing" when they were "you read the wrong
address".
"""
import argparse
import os
import re
import subprocess
import sys

SSH = ["ssh", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=15"]

# Only the reasons this guest actually produces are named.  An unnamed one
# prints as its number, which is better than a wrong name.
EXIT_REASON = {
    0: "exception", 1: "ext-int", 2: "triple-fault", 3: "init", 4: "sipi",
    7: "int-window", 8: "nmi-window", 9: "task-switch", 10: "cpuid",
    12: "hlt", 13: "invd", 14: "invlpg", 18: "vmcall", 19: "vmclear",
    20: "vmlaunch", 21: "vmptrld", 22: "vmptrst", 23: "vmread",
    24: "vmresume", 25: "vmwrite", 26: "vmoff", 27: "vmon",
    28: "cr-access", 29: "dr-access", 30: "io", 31: "rdmsr", 32: "wrmsr",
    33: "entry-fail-state", 34: "entry-fail-msr", 36: "mwait",
    37: "monitor-trap", 39: "monitor", 40: "pause", 41: "entry-fail-mce",
    43: "tpr-below", 44: "apic-access", 45: "virt-eoi", 46: "gdtr-idtr",
    47: "ldtr-tr", 48: "ept-violation", 49: "ept-misconfig",
    50: "invept", 51: "rdtscp", 52: "preempt-timer", 53: "invvpid",
    54: "wbinvd", 55: "xsetbv", 56: "apic-write", 57: "rdrand",
    58: "invpcid", 59: "vmfunc", 60: "encls", 61: "rdseed",
    62: "pml-full", 63: "xsaves", 64: "xrstors",
}

ACTIVITY = {0: "active", 1: "hlt", 2: "shutdown", 3: "wait-sipi"}

# The order the hypervisor writes them in - phase_cycles is indexed by
# position, not by name, so this list is the only thing that says which
# is which. Keep it beside the indices in the sources that fill them.
#
# The indentation used to be in the *name*, which is how the table came
# to be summed: a reader looking at a flat column of "cycles/call" has
# nothing telling it that `save_l2_state` is inside `reflect_l2_exit`
# and that `copy_shadow_to_vmcs12` is called four times a round trip
# where `build_vmcs02` is called once. The nesting is now data, in
# PHASE_PARENT below, and the printer derives the indentation from it.
PHASE_NAMES = ["save_l2_state", "reflect_l2_exit", "build_vmcs02",
               "shadow_ept_pointer_for", "copy_vmcs12_to_shadow",
               "copy_shadow_to_vmcs12", "vmptrld->vmcs02",
               "vmptrld->vmcs01", "merge_nested_bitmaps",
               "on_l2_ept_fault", "merge: guest page read",
               "guest read: map_window", "load_l1_host_state",
               "exit information", "build: before vmptrld",
               "build: after vmptrld",
               "vmptrld: read region", "vmptrld: flush old",
               "vmptrld: assign", "vmptrld: shadow publish",
               "vmptrld: whole call",
               "materialise: vmptrld in", "materialise: field loop",
               "materialise: vmptrld out", "materialise: whole",
               "exit: prologue", "exit: dispatch", "resume: events",
               "resume: diag and rip", "resume: record_exit",
               "resume: entry census", "reflect: exit ring",
               "reflect: msr store", "reflect: msr load + invvpid",
               "reflect: evmcs store", "enter_or_park_l2",
               "on_guest_vmlaunch", "guest write: map_window",
               "guest read: whole call", "guest write: whole call",
               "copy out: vmptrst", "copy out: vmptrld shadow",
               "copy out: field writes", "copy out: vmclear",
               "copy out: vmptrld back",
               "copy in: vmptrst", "copy in: vmptrld shadow",
               "copy in: field reads", "copy in: vmclear",
               "copy in: vmptrld back",
               "(spare 50)", "(spare 51)"]

# Which slot each one is nested inside. TOP is a top-level interval of
# the adjacent split over a whole exit; CROSS is a phase with more than
# one caller, so it belongs to no single parent and is excluded from the
# residue arithmetic rather than being charged to whichever caller was
# guessed at.
#
# **This table is what makes the phase table readable, and its absence
# is what made it misleading.** Slots 0 to 24 were each added where
# somebody suspected a cost, so several of them nest two and three deep,
# and a naive sum of their cycles/call column came to about half the
# round trip - which was then reported as "the other half is
# unattributed". Some of that half was double counting.
PHASE_TOP = -1
PHASE_CROSS = -2

PHASE_PARENT = [
    1,            # 0  save_l2_state
    26,           # 1  reflect_l2_exit
    36,           # 2  build_vmcs02
    PHASE_CROSS,  # 3  shadow_ept_pointer_for - build, ept fault, vmfunc
    1,            # 4  copy_vmcs12_to_shadow
    PHASE_CROSS,  # 5  copy_shadow_to_vmcs12 - vmlaunch and the flush
    2,            # 6  vmptrld->vmcs02
    1,            # 7  vmptrld->vmcs01
    14,           # 8  merge_nested_bitmaps
    26,           # 9  on_l2_ept_fault
    8,            # 10 merge: guest page read
    38,           # 11 guest read: map_window
    1,            # 12 load_l1_host_state
    1,            # 13 exit information
    2,            # 14 build: before vmptrld
    2,            # 15 build: after vmptrld
    20,           # 16 vmptrld: read region
    20,           # 17 vmptrld: flush old
    20,           # 18 vmptrld: assign
    20,           # 19 vmptrld: shadow publish
    26,           # 20 vmptrld: whole call
    24,           # 21 materialise: vmptrld in
    24,           # 22 materialise: field loop
    24,           # 23 materialise: vmptrld out
    17,           # 24 materialise: whole
    PHASE_TOP,    # 25 exit: prologue
    PHASE_TOP,    # 26 exit: dispatch
    PHASE_TOP,    # 27 resume: events
    PHASE_TOP,    # 28 resume: diag and rip
    PHASE_TOP,    # 29 resume: record_exit
    PHASE_TOP,    # 30 resume: entry census
    1,            # 31 reflect: exit ring
    1,            # 32 reflect: msr store
    1,            # 33 reflect: msr load + invvpid
    1,            # 34 reflect: evmcs store
    36,           # 35 enter_or_park_l2
    26,           # 36 on_guest_vmlaunch
    39,           # 37 guest write: map_window
    PHASE_CROSS,  # 38 guest read: whole call
    PHASE_CROSS,  # 39 guest write: whole call
    4,            # 40 copy out: vmptrst
    4,            # 41 copy out: vmptrld shadow
    4,            # 42 copy out: field writes
    4,            # 43 copy out: vmclear
    4,            # 44 copy out: vmptrld back
    5,            # 45 copy in: vmptrst
    5,            # 46 copy in: vmptrld shadow
    5,            # 47 copy in: field reads
    5,            # 48 copy in: vmclear
    5,            # 49 copy in: vmptrld back
    PHASE_CROSS,  # 50 spare
    PHASE_CROSS,  # 51 spare
]

# Whose instruction pointer a record holds - see exit_trace_entry's
# rip_owner. An address attributed to the wrong guest reads as a
# perfectly ordinary address, so it is marked rather than left implicit,
# and the unmarked case is the ordinary one.
RIP_OWNER = {0: "", 1: " [l2-rip]", 2: " [l1-rip]"}

# The slot order `capture_vtl_switch` writes, and the two hypercalls it
# is armed for.  Slot 4 is the VMCS's guest RSP, not the exit context's.
VTL_SLOTS = ["rax", "rbx", "rcx", "rdx", "rsp", "rbp", "rsi", "rdi",
             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
             "rip", "cr3", "rflags", "eptp"]
VTL_KINDS = ["HvCallVtlCall 0x11", "HvCallVtlReturn 0x12",
             "STIMER0 periodic arm"]


def gdb_offsets(elf, members, optional=False, quiet=False):
    """Ask the ELF where each member lives inside the singleton."""
    args = []
    for m in members:
        args += ["-ex", f"print/x (long)&(('zpp::hypervisor::hypervisor' *)0)->{m}"]
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf] + args,
                         capture_output=True, text=True).stdout
    values = re.findall(r"^\$\d+ = (0x[0-9a-f]+)$", out, re.M)
    if len(values) != len(members):
        if optional:
            # One member per call, so a miss can be attributed. Used for
            # members a *deployed* binary may predate: the reader is
            # pointed at whichever ELF is running, and a dump of an older
            # one must lose that section rather than the whole dump.
            found = {}
            for m in members:
                one = gdb_offsets(elf, [m], optional=False, quiet=True)
                if one:
                    found.update(one)
            return found
        if quiet:
            return {}
        sys.exit(f"could not read all offsets from {elf}: got {values}")
    return dict(zip(members, (int(v, 16) for v in values)))


def gdb_lengths(elf, members):
    """Ask the ELF how long each per-processor row is, in entries.

    The same argument as `gdb_offsets`, for the same reason and after the
    same failure: a capacity carried here is a second copy of a constant
    that lives in the header, and when the header moved this did not.
    `exit_reason_capacity` went to 96 while this said 64, and the effect
    was invisible for cpu 0 - whose row starts at offset zero, so a wrong
    stride cancels - and wrong for every other processor.  It did not
    fail; it reported `rdrand`, `encls` and `xsaves` exits for a guest
    that executes none of them, which reads as a bizarre finding rather
    than as a bug in the reader.

    `sizeof(row) / sizeof(row[0])` cannot drift the same way, because
    both halves come from the type being read.
    """
    args = []
    for m in members:
        args += ["-ex",
                 f"print (int)(sizeof(('zpp::hypervisor::hypervisor' *)0)"
                 f"->{m}[0] / sizeof(('zpp::hypervisor::hypervisor' *)0)"
                 f"->{m}[0][0])"]
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf] + args,
                         capture_output=True, text=True).stdout
    values = re.findall(r"^\$\d+ = (\d+)$", out, re.M)
    if len(values) != len(members):
        sys.exit(f"could not read all lengths from {elf}: got {values}")
    return dict(zip(members, (int(v) for v in values)))


def gdb_values(elf, expressions):
    """Evaluate integer expressions against the ELF's own types.

    Same argument as `gdb_lengths`, one step more general: the trust-level
    capture is a three-dimensional array and its inner two bounds are not
    `sizeof(row)/sizeof(row[0])`.  Deriving them here rather than copying
    the constants keeps the failure mode at "gdb could not answer" instead
    of "the reader walked the array at the wrong stride".
    """
    args = []
    for expression in expressions:
        args += ["-ex", f"print (int)({expression})"]
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf] + args,
                         capture_output=True, text=True).stdout
    values = re.findall(r"^\$\d+ = (\d+)$", out, re.M)
    if len(values) != len(expressions):
        sys.exit(f"could not evaluate against {elf}: got {values}")
    return [int(v) for v in values]


def gdb_symbol(elf, symbol):
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf,
                          "-ex", f"print/x &'{symbol}'"],
                         capture_output=True, text=True).stdout
    m = re.search(r"(0x[0-9a-f]+)", out)
    if not m:
        sys.exit(f"could not find {symbol} in {elf}")
    return int(m.group(1), 16)


class Monitor:
    """Batched physical-memory reads over the QEMU monitor.

    One round trip per word is far too slow for a ring of hundreds of
    entries, so every read wanted is queued and issued in one connection.
    """

    def __init__(self, rig, port):
        self.rig, self.port = rig, port
        self.pending = []
        # Reads that never came back after retries. Non-empty means some
        # number printed above is a zero that was never read.
        self.unanswered = []

    def queue(self, address, words):
        self.pending.append((address, words))

    # Batch size, and it is not a performance knob.
    #
    # The monitor echoes each character of a command back with redraws,
    # and with many commands in flight that echo interleaves with the
    # output *within a line*. A corrupted line still matches the address
    # pattern, so it parses - into the **wrong key**. The reader then
    # returns the right *number* of words, none of them at an address
    # anyone asked for, and `words.get(addr, 0)` turns every one of those
    # misses into a plausible zero.
    #
    # Measured: 26 reads in one session returned 78 words, all zero,
    # while the same three-word read alone returned the right values.
    # That is what made the log ring print 37 empty lines.
    CHUNK = 6

    def _issue(self, batch):
        script = "".join(f"xp/{n}gx 0x{a:x}\n" for a, n in batch)
        proc = subprocess.run(
            SSH + [self.rig, f"cat | nc -w 30 127.0.0.1 {self.port}"],
            input=script, capture_output=True, text=True, errors="replace")
        words = {}
        for line in proc.stdout.replace("\r", "").split("\n"):
            line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line)
            m = re.match(r"^([0-9a-f]{8,16}):((?:\s+0x[0-9a-f]+)+)\s*$", line)
            if not m:
                continue
            address = int(m.group(1), 16)
            for i, word in enumerate(m.group(2).split()):
                words[address + 8 * i] = int(word, 16)
        return words

    def run(self):
        pending, self.pending = self.pending, []
        words = {}

        # Chunked, and then *checked*: a read whose address did not come
        # back is retried alone rather than left to read as zero. Without
        # the check this is the same failure shape as `reader proven` -
        # an answer that looks like data and is not.
        for start in range(0, len(pending), self.CHUNK):
            batch = pending[start:start + self.CHUNK]
            words.update(self._issue(batch))

            for address, count in batch:
                wanted = [address + 8 * i for i in range(count)]
                if all(w in words for w in wanted):
                    continue
                for _ in range(2):
                    words.update(self._issue([(address, count)]))
                    if all(w in words for w in wanted):
                        break
                else:
                    self.unanswered.append((address, count))

        return words


def dump_log(monitor, elf, base, limit):
    """The hypervisor's own log, oldest line first.

    The counters say what the state *is*; this says what happened, in
    order, which is usually the question. Walked by hand for the same
    reason `scripts/zpp.gdb` walks it by hand - the hypervisor is built
    against libc++ headers only, so there are no pretty printers and a
    list of strings is raw nodes and unions.

    Read over the monitor rather than gdb deliberately. gdb resolves
    through the *current* processor's page tables, and once the guest is
    running our module is not mapped in its CR3 - so every read answers
    "Cannot access memory" unless a processor happens to be inside our
    code. `xp` reads physical memory and ignores paging, and since the
    module is identity mapped the pointers stored in it are already
    physical addresses.

    Layout, matching zpp.gdb: a node is {__prev_, __next_, value} so the
    string starts sixteen bytes in, and libc++'s string keeps its
    long/short flag in the low bit of the first byte - long keeps a
    pointer sixteen bytes in, short keeps the characters one byte in.
    """
    head = gdb_symbol(elf, "zpp::hypervisor::log_storage::m_lines") + base

    # Walk the node chain first, one round trip per batch rather than
    # per node: the list is singly followed here, so each step needs the
    # previous answer, but the string bodies can all be fetched together.
    nodes, seen, node = [], set(), None
    monitor.queue(head + 8, 1)
    node = monitor.run().get(head + 8, 0)
    while node and node != head and len(nodes) < limit and node not in seen:
        seen.add(node)
        nodes.append(node)
        monitor.queue(node + 8, 1)
        node = monitor.run().get(node + 8, 0)

    if not nodes:
        print("\nlog ring: empty")
        return

    # The string headers, all at once.
    for n in nodes:
        monitor.queue(n + 16, 3)
    words = monitor.run()

    long_ones = []
    lines = []
    for i, n in enumerate(nodes):
        first = words.get(n + 16, 0)
        if first & 1:
            long_ones.append((i, words.get(n + 32, 0), first))
            lines.append(None)
        else:
            # Short: length in the top bits of the first byte's slot,
            # characters immediately after it.
            length = (first >> 1) & 0x7f
            raw = b""
            for w in (words.get(n + 16, 0), words.get(n + 24, 0),
                      words.get(n + 32, 0)):
                raw += w.to_bytes(8, "little")
            lines.append(raw[1:1 + length].decode("ascii", "replace"))

    # And the bodies of the long ones, also all at once.
    if long_ones:
        for _, pointer, _ in long_ones:
            if pointer:
                monitor.queue(pointer, 24)
        body = monitor.run()
        for index, pointer, _ in long_ones:
            raw = b""
            for k in range(24):
                raw += body.get(pointer + 8 * k, 0).to_bytes(8, "little")
            lines[index] = raw.split(b"\0")[0].decode("ascii", "replace")

    print(f"\nlog ring ({len(lines)} lines, oldest first)")
    for i, text in enumerate(lines):
        print(f"  [{i:4}] {text}")

    # An empty line is either an empty line or a read that never came
    # back, and those must not look alike - that is exactly what made
    # this ring print 37 blanks and read as "the log is empty".
    if monitor.unanswered:
        print(f"  WARNING: {len(monitor.unanswered)} reads never answered "
              f"after retries - blank lines above may be unread rather "
              f"than empty")


def name_reason(value):
    reason = value & 0xffff
    tag = EXIT_REASON.get(reason, str(reason))
    if value & (1 << 31):
        tag += "!ENTRY-FAIL"
    return tag


def monitor_vector_counts(monitor, instance, off, cpu, member):
    """Which interrupt vectors were acknowledged, per processor.

    The totals beside this cannot answer the question it exists for: a
    guest parked with its clock ticking is either not being handed a
    device's interrupt or never asked the device for anything, and
    `external_interrupts_taken` counts a timer tick and a completion the
    same.  One or two vectors here means only the clock is arriving.

    Read as one block of 1024 bytes rather than 256 words, because the
    counters are 32 bit - two per quadword, low half first.
    """
    base = instance + off[member] + cpu * 1024
    monitor.queue(base, 128)
    words = monitor.run()
    counts = {}
    for i in range(128):
        word = words.get(base + 8 * i, 0)
        for half in range(2):
            value = (word >> (32 * half)) & 0xffffffff
            if value:
                counts[2 * i + half] = value
    return counts


def monitor_reasons(monitor, instance, off, args, cpu, capacity):
    """The whole-run histogram, which the 32-entry ring cannot give.

    The ring answers "what was it doing when it stopped"; this answers
    "where does the time go", and those turned out to be different
    questions - the ring showed a plausible cycle while the histogram
    showed most exits were somewhere the ring never sampled.
    """
    base = instance + off["exit_reason_counts"] + cpu * capacity * 8
    monitor.queue(base, capacity)
    words = monitor.run()
    return {i: words.get(base + 8 * i, 0)
            for i in range(capacity) if words.get(base + 8 * i, 0)}


def dump_field_use(args, instance, off, capacity=128):
    """The VMCS fields the guest hypervisor reads and writes.

    This is what decides which fields VMCS shadowing should cover: a
    shadowed field costs a copy in each direction at every second-level
    exit, so a list longer than what the guest hypervisor touches makes
    the fix slower than the problem.
    """
    monitor = Monitor(args.rig, args.port)
    for name in ("vmcs_field_read_encoding", "vmcs_field_read_count",
                 "vmcs_field_write_encoding", "vmcs_field_write_count"):
        monitor.queue(instance + off[name], capacity)
    monitor.queue(instance + off["vmcs_field_use_overflow"], 1)
    words = monitor.run()

    def table(kind):
        rows = []
        for i in range(capacity):
            count = words.get(
                instance + off[f"vmcs_field_{kind}_count"] + 8 * i, 0)
            if not count:
                continue
            rows.append((count, words.get(
                instance + off[f"vmcs_field_{kind}_encoding"] + 8 * i, 0)))
        rows.sort(reverse=True)
        return rows

    for kind in ("read", "write"):
        rows = table(kind)
        total = sum(count for count, _ in rows) or 1
        print(f"  --- vm{kind} ({total} total, {len(rows)} distinct) ---")
        for count, encoding in rows:
            print(f"    0x{encoding:04x} {VMCS_FIELD.get(encoding, ''):<44} "
                  f"{count:>10}  {100.0 * count / total:5.1f}%")

    overflow = words.get(instance + off["vmcs_field_use_overflow"], 0)
    if overflow:
        print(f"  table full, {overflow} uses not recorded")


def dump_own_field_use(args, elf, base):
    """The VMCS fields **this VMM** reads and writes, by name.

    Different question from `dump_field_use` above, and the two are worth
    keeping apart: that one counts what the *guest hypervisor* asks for
    through VMREAD and VMWRITE exits, which is what a shadowing list
    would have to cover.  This one counts the accesses this VMM executes
    itself, which is what has to be *removed* - on a host without VMCS
    shadowing every one of them is an exit to the layer below at 1.4-1.8
    microseconds, and the round trip spends about half its time here.

    Namespace-scope globals rather than members of the singleton, so they
    resolve against the module base and not against `instance`.

    These tables had no reader at all for the whole of their existence -
    they were added, and then the number they answer was estimated twice
    from cycles divided by a price, and both estimates informed a wrong
    decision.  That is what this function is for.
    """
    slots = 512
    tables = {}
    for kind in ("read", "write"):
        try:
            tables[kind] = (
                base + gdb_symbol(
                    elf, f"zpp::arch::x86_64::vmx::vmcs_{kind}_field"),
                base + gdb_symbol(
                    elf, f"zpp::arch::x86_64::vmx::vmcs_{kind}_hits"),
                base + gdb_symbol(
                    elf, f"zpp::arch::x86_64::vmx::vmcs_{kind}_overflow"))
        except SystemExit as failure:
            print(f"\n[our own vmcs accesses: {failure}]")
            return

    monitor = Monitor(args.rig, args.port)
    for fields, hits, overflow in tables.values():
        monitor.queue(fields, slots)
        monitor.queue(hits, slots)
        monitor.queue(overflow, 1)
    words = monitor.run()

    print("\nvmcs fields this vmm accesses itself")
    for kind, (fields, hits, overflow) in tables.items():
        rows = []
        for i in range(slots):
            count = words.get(hits + 8 * i, 0)
            if count:
                rows.append((count, words.get(fields + 8 * i, 0)))
        rows.sort(reverse=True)
        total = sum(count for count, _ in rows) or 1
        print(f"  --- our {kind}s ({total:,} total, {len(rows)} distinct) ---")
        for count, encoding in rows[:32]:
            print(f"    0x{encoding:04x} "
                  f"{VMCS_FIELD.get(encoding, ''):<44} {count:>12,}  "
                  f"{100.0 * count / total:5.1f}%")
        lost = words.get(overflow, 0)
        if lost:
            # Only reachable if an encoding with an index above 31 turns
            # up - see `vmcs_use_slot`.  Measured zero over every
            # encoding in vmcs_fields.h, so a non-zero here means the
            # table's assumption has stopped holding and the counts above
            # may be two fields added together.
            print(f"    SLOT COLLISION: {lost:,} {kind}s not recorded - the "
                  f"counts above are not trustworthy")


def load_field_names():
    """Field encoding to name, straight out of the header.

    Read rather than duplicated, because a name table that drifts from the
    enum is worse than no names: it labels the wrong field confidently.
    """
    path = "hypervisor/include/zpp/arch/x86_64/vmx/vmcs_fields.h"
    names = {}
    try:
        with open(path) as handle:
            for line in handle:
                m = re.match(r"\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+),", line)
                if m:
                    names.setdefault(int(m.group(2), 16), m.group(1))
    except OSError:
        pass
    return names


VMCS_FIELD = load_field_names()


def dump_entry_rips(args, elf, instance):
    """The distinct instruction pointers the guest is entered at.

    Two of them, alternating, with the counts equal, is a guest that
    never executes anything: it is being resumed at the VMCALL it exited
    on.  Many of them is a guest that is executing and looping in its own
    software.  Nothing else distinguishes those.
    """
    off = gdb_offsets(elf, ["l2_entry_rip", "l2_entry_rip_count",
                            "l2_entry_rip_other"])
    slots = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->l2_entry_rip[0] "
        "/ sizeof(('zpp::hypervisor::hypervisor' *)0)"
        "->l2_entry_rip[0][0]"])[0]

    reader = Monitor(args.rig, args.port)
    for member in ("l2_entry_rip", "l2_entry_rip_count"):
        reader.queue(instance + off[member], args.cpus * slots)
    reader.queue(instance + off["l2_entry_rip_other"], args.cpus)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    for cpu in range(args.cpus):
        rows = [(word("l2_entry_rip_count", cpu * slots + i),
                 word("l2_entry_rip", cpu * slots + i))
                for i in range(slots)]
        rows = [r for r in rows if r[0]]
        if not rows:
            continue
        total = sum(c for c, _ in rows) or 1
        print(f"\ncpu {cpu} second-level entry rips "
              f"({total:,} entries, {len(rows)} distinct, "
              f"{word('l2_entry_rip_other', cpu):,} beyond the table)")
        for count, rip in sorted(rows, reverse=True):
            print(f"  0x{rip:016x}  {count:>10}  "
                  f"{100.0 * count / total:5.1f}%")


def dump_priority(args, elf, instance):
    """What priority the guest runs at, and what it is told to run at.

    The whole boot turns on this pair.  A software interrupt is
    delivered only when its class exceeds the virtual task priority's,
    so a guest that never drops below `0x20` never runs a deferred
    procedure call however often one is requested - and deferred
    procedure calls are where the boot's remaining work is.

    `l2_entry_vtpr` is sampled on the page the entry is about to use,
    which is the only page that is the right one; the threshold
    histogram is what the guest hypervisor armed beside it.
    """
    members = ["l2_entry_vtpr", "l2_tpr_threshold_seen", "l2_cpl_seen",
               "l2_tpr_would_fire", "l2_tpr_armed_above",
               "clock_gap_buckets", "l2_entry_ppr", "l2_given_vector",
               "l2_low_priority_no_event", "interrupt_request_vtpr_seen",
               "interrupt_request_vector", "vtl_half_cycles",
               "vtl_half_exits", "vtl_half_count"]
    off = gdb_offsets(elf, members)
    vtpr_slots, threshold_slots, cpl_slots = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->l2_entry_vtpr[0] / 4",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)"
        "->l2_tpr_threshold_seen[0] / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->l2_cpl_seen[0] / 8"])

    reader = Monitor(args.rig, args.port)
    # 32 bit counters, so two to a quadword and the reader unpacks.
    reader.queue(instance + off["l2_entry_vtpr"],
                 args.cpus * vtpr_slots // 2)
    reader.queue(instance + off["l2_tpr_threshold_seen"],
                 args.cpus * threshold_slots)
    reader.queue(instance + off["l2_cpl_seen"], args.cpus * cpl_slots)
    for member in ("l2_tpr_would_fire", "l2_tpr_armed_above"):
        reader.queue(instance + off[member], args.cpus)
    gap_slots = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)"
        "->clock_gap_buckets[0] / 8"])[0]
    reader.queue(instance + off["clock_gap_buckets"],
                 args.cpus * gap_slots)
    # 32 bit counters, two to a quadword, same as l2_entry_vtpr.
    for member in ("l2_entry_ppr", "l2_given_vector"):
        reader.queue(instance + off[member], args.cpus * 256 // 2)
    for member in ("interrupt_request_vtpr_seen",
                   "interrupt_request_vector"):
        reader.queue(instance + off[member], args.cpus * 256)
    reader.queue(instance + off["l2_low_priority_no_event"], args.cpus)
    for member in ("vtl_half_cycles", "vtl_half_exits", "vtl_half_count"):
        reader.queue(instance + off[member], args.cpus * 2)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    for cpu in range(args.cpus):
        rows = []
        for i in range(vtpr_slots):
            pair = word("l2_entry_vtpr", (cpu * vtpr_slots + i) // 2)
            count = (pair >> (32 * (i % 2))) & 0xffffffff
            if count:
                rows.append((count, i))
        if not rows:
            continue

        total = sum(c for c, _ in rows) or 1
        print(f"\ncpu {cpu} virtual task priority at second-level entry "
              f"({total:,} entries)")
        for count, vtpr in sorted(rows, reverse=True):
            print(f"  0x{vtpr:02x}  {count:>10}  "
                  f"{100.0 * count / total:5.1f}%")

        print(f"  owed by SDM 27.6.7 {word('l2_tpr_would_fire', cpu):,}, "
              f"armed while already at or above "
              f"{word('l2_tpr_armed_above', cpu):,}")

        cpl = [word("l2_cpl_seen", cpu * cpl_slots + i)
               for i in range(cpl_slots)]
        print("  cpl seen: " + ", ".join(f"{i}={v:,}"
                                         for i, v in enumerate(cpl) if v))

        # How long the guest gets between clock interrupts. The period
        # it programmed is 1.74 ms; a distribution far below that is a
        # backlog of expirations being drained rather than a timer.
        gaps = [(i, word("clock_gap_buckets", cpu * gap_slots + i))
                for i in range(gap_slots)]
        gaps = [(i, v) for i, v in gaps if v]
        if gaps:
            total = sum(v for _, v in gaps)
            # 1.992 GHz, measured rather than assumed: BACKLOG.md
            # records the TSC advancing 179,446,096,055 counts over a
            # 90.08 second wall-clock window, and the fitted
            # reference_scale agreeing to four significant figures. The
            # part's marketed 1.80 GHz base frequency is *not* its TSC
            # frequency, and this label previously used 2.6 GHz, which
            # understated every period by 31%.
            print(f"  time-stamp counter between clock interrupts "
                  f"({total:,} gaps, TSC 1.992 GHz measured)")
            for i, v in gaps:
                low = 1 << i
                # Counts divided by MHz are MICROSECONDS. This
                # printed "ms" while dividing by 1992, which is the
                # third unit slip in this reader in one session - after
                # the 2.6 GHz constant and the unlabelled dead field.
                # 2^21 counts is 1.05 ms, and the label said 1052 ms.
                print(f"    2^{i:<2} ({low / 1992000.0:8.2f} - "
                      f"{2.0 * low / 1992000.0:.2f} ms)  {v:>10}  "
                      f"{100.0 * v / total:5.1f}%")

        # PPR, not TPR, is what an arriving interrupt's class must
        # exceed - SDM 12.8.3.1 - so this is the reading that says
        # whether the DISPATCH_LEVEL request could ever be granted.
        def packed(member, index):
            pair = word(member, (cpu * 256 + index) // 2)
            return (pair >> (32 * (index % 2))) & 0xffffffff

        # The PPR heading says what it is, because it has already been
        # misread twice in one session. SDM 32.1.1: the processor
        # maintains VPPR only under "virtual-interrupt delivery", which
        # is not offered here and which the layer below does not permit
        # this VMM either - so a constant 0x00 is the field being dead,
        # not the guest being at PASSIVE, and reported the other way it
        # says the exact opposite of what VTPR beside it says.
        for member, what in (("l2_entry_ppr",
                              "processor priority at entry "
                              "[NOT MAINTAINED - expect 0x00, see "
                              "SDM 32.1.1; use the task priority above]"),
                             ("l2_given_vector",
                              "vectors vmcs02 actually carried")):
            rows = [(packed(member, i), i) for i in range(256)]
            rows = [r for r in rows if r[0]]
            if not rows:
                continue
            total = sum(c for c, _ in rows) or 1
            print(f"\n  {what} ({total:,})")
            for count, value in sorted(rows, reverse=True)[:8]:
                print(f"    0x{value:02x}  {count:>10}  "
                      f"{100.0 * count / total:5.1f}%")

        # Cumulative, and measured to be almost entirely early-boot
        # residue: over a steady-state window this does not move at
        # Read it as a delta between two dumps or not at all.
        #
        # **This used to say "the settled guest never goes below
        # DISPATCH, so this is early-boot residue". That was wrong, and
        # wrong in the direction that hides a live fault.** Measured on a
        # settled guest: 81,895 -> 83,747 across sixty seconds, **30.9 a
        # second and climbing**. The guest does go below DISPATCH, tens
        # of times a second, and on every one of those entries the level
        # above staged no event while the deferred-call vector it had
        # asked for was outstanding.
        #
        # The counter undercounts by construction - the site tests the
        # task priority, which is a lower bound on the processor
        # priority - so every entry counted is one the interrupt
        # certainly could have been delivered on.
        # **This is NOT evidence of a fault, and it used to say it was.**
        # Read the site before believing the label: `on_l2_entry_event`
        # increments this whenever an entry carries no event and the task
        # priority is below the dispatch class. It does **not** check that
        # a deferred call was outstanding. So a guest with nothing pending
        # at a low priority - an ordinary, healthy moment - counts here,
        # and the growth rate this used to call "a live fault" is
        # indistinguishable from a machine with nothing to do.
        #
        # Left in because the quantity is still worth watching; the claim
        # attached to it was not.
        print(f"\n  entries carrying no event while the task priority was "
              f"below the dispatch class: "
              f"{word('l2_low_priority_no_event', cpu):,} "
              f"(cumulative - read as a delta. NOT a fault by itself: the "
              f"site does not check that anything was pending)")

        for member, what in (
                ("interrupt_request_vector",
                 "vectors the guest asked for"),
                ("interrupt_request_vtpr_seen",
                 "task priority when it asked")):
            rows = [(word(member, cpu * 256 + i), i) for i in range(256)]
            rows = [r for r in rows if r[0]]
            if not rows:
                continue
            total = sum(c for c, _ in rows) or 1
            print(f"\n  {what} ({total:,})")
            for count, value in sorted(rows, reverse=True)[:8]:
                print(f"    0x{value:02x}  {count:>10}  "
                      f"{100.0 * count / total:5.1f}%")

        # And what a round trip costs, split into its two halves.
        halves = ["HvCallVtlCall -> HvCallVtlReturn (secure kernel)",
                  "HvCallVtlReturn -> HvCallVtlCall (ordinary kernel)"]
        if any(word("vtl_half_count", cpu * 2 + h) for h in range(2)):
            print("\n  what one trust-level round trip costs")
            for h in range(2):
                n = word("vtl_half_count", cpu * 2 + h)
                if not n:
                    continue
                cycles = word("vtl_half_cycles", cpu * 2 + h)
                exits = word("vtl_half_exits", cpu * 2 + h)
                print(f"    {halves[h]}")
                print(f"      {n:,} halves, {cycles // n:,} cycles "
                      f"({cycles / n / 1992.0:.1f} us at 1.992 GHz), "
                      f"{exits / n:.1f} exits")


def dump_synthetic_msrs(args, elf, instance):
    """Which synthetic MSRs the second-level guest writes, and how often.

    This is the question the exit budget turns on and it had never been
    asked. 2.69 synthetic-MSR writes per clock tick is a lot for a tick,
    and "the MSR bitmap cannot filter them" - which is true, they lie
    outside both ranges SDM 26.6.9 allows - says nothing about whether
    there should be 2.69 of them.
    """
    members = ["l2_synthetic_msr_writes", "l2_synthetic_msr_reads",
               "l2_int_window_armed", "l2_int_window_clear"]
    off = gdb_offsets(elf, members)

    reader = Monitor(args.rig, args.port)
    # **32 bit counters, two to a quadword.** Read as 64 bit they come
    # back as `0x3_00000002` - two adjacent slots welded together, which
    # printed as 12,884,901,890 writes of one MSR and looked like a
    # finding rather than a unit error. Same shape as `l2_entry_vtpr`
    # above, which is why that one already unpacks.
    for member in ("l2_synthetic_msr_writes", "l2_synthetic_msr_reads"):
        reader.queue(instance + off[member], args.cpus * 256 // 2)
    for member in ("l2_int_window_armed", "l2_int_window_clear"):
        reader.queue(instance + off[member], args.cpus)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    # Named where the name is established; the rest print as addresses.
    known = {0x70: "EOI", 0x71: "ICR", 0x72: "TPR",
             0x73: "VP_ASSIST_PAGE", 0x84: "EOM",
             0x20: "TIME_REF_COUNT", 0x21: "REFERENCE_TSC",
             0xb0: "STIMER0_CONFIG", 0xb1: "STIMER0_COUNT"}

    for cpu in range(args.cpus):
        def packed(member, index):
            pair = word(member, (cpu * 256 + index) // 2)
            return (pair >> (32 * (index % 2))) & 0xffffffff

        for member, what in (("l2_synthetic_msr_writes", "written"),
                             ("l2_synthetic_msr_reads", "read")):
            rows = [(packed(member, i), i) for i in range(256)]
            rows = [r for r in rows if r[0]]
            if not rows:
                continue
            total = sum(c for c, _ in rows)
            print(f"\ncpu {cpu} synthetic MSRs {what} ({total:,})")
            for count, slot in sorted(rows, reverse=True):
                name = known.get(slot, "")
                print(f"  0x400000{slot:02x}  {count:>10}  "
                      f"{100.0 * count / total:5.1f}%  {name}")

        vp = gdb_offsets(elf, [
            "vp_assist_l2_physical", "vp_assist_via_ept12",
            "vp_assist_via_identity", "vp_assist_paths_agree",
            "vp_assist_watch_armed", "vp_assist_writes",
            "vp_assist_write_page"])
        vr = Monitor(args.rig, args.port)
        for m in vp:
            vr.queue(instance + vp[m], 1)
        vg = vr.run()

        def v(m):
            return vg.get(instance + vp[m], 0)

        if v("vp_assist_l2_physical"):
            print(f"\ncpu {cpu} VP assist page")
            print(f"  L2 physical (what Windows wrote) "
                  f"0x{v('vp_assist_l2_physical'):x}")
            print(f"  via the guest hypervisor's own EPT "
                  f"0x{v('vp_assist_via_ept12'):x}")
            print(f"  via this VMM's identity map        "
                  f"0x{v('vp_assist_via_identity'):x}")
            print(f"  the two paths agree: "
                  f"{'YES' if v('vp_assist_paths_agree') else 'NO'}")
            print(f"  write-watch armed: "
                  f"0x{v('vp_assist_watch_armed'):x}")
            # An unarmed watch reports "no writes" exactly like a watch
            # that saw none. Never print the conclusion without the
            # premise - the first run of this said "nothing writes it,
            # on the watch's word" when no watch had been armed.
            if 1 != v("vp_assist_watch_armed"):
                print(f"  WRITES SEEN: {v('vp_assist_writes'):,}"
                      f"   <- MEANINGLESS, no watch is armed")
            else:
                print(f"  WRITES SEEN: {v('vp_assist_writes'):,}"
                      + ("   <- the guest hypervisor does write it"
                         if v("vp_assist_writes")
                         else "   <- nothing writes it, and a watch was "
                              "armed the whole time"))

        armed = word("l2_int_window_armed", cpu)
        clear = word("l2_int_window_clear", cpu)
        if armed or clear:
            total = armed + clear
            print(f"\ncpu {cpu} interrupt-window exiting, as vmcs12 asked "
                  f"for it at entry")
            print(f"  armed {armed:,} of {total:,} entries "
                  f"({100.0 * armed / max(total, 1):.1f}%)")


def dump_phase_tree(cpu, phase_count, cell, round_trips, handler):
    """The phase table as the tree it is, with a column that sums.

    Three things this prints that the flat table could not, and each of
    them is a wrong conclusion this file has already recorded:

    - **cycles per round trip**, so siblings are additive. The old table
      printed cycles per *call*, and the denominators differ by more
      than an order of magnitude across the rows - so the sum of that
      column is not a quantity.
    - **the nesting**, from PHASE_PARENT, so a container and its
      children are never added to each other.
    - **the residue**, twice: `handler_cycles` minus the top-level
      intervals, which says whether the split covers the handler at all,
      and each container minus its own children, which is where the cost
      is when everything named inside a large phase is small.

    Slots 25 to 30 are adjacent intervals over the whole of an exit and
    sum to `handler_cycles` by construction. Everything older nests
    inside one of them, so a top-level residue much above the round-off
    means an exit is leaving the handler somewhere this does not know
    about - a halt, or a path that never reaches `resume_guest`.
    """
    calls = [cell("phase_calls", i) for i in range(phase_count)]
    cycles = [cell("phase_cycles", i) for i in range(phase_count)]

    if not any(calls):
        return

    # A phase's own cycles, less everything charged to a child of it.
    # Cross-cutting slots are not anybody's child, so they never subtract
    # from a container - which is deliberate: charging
    # `copy_shadow_to_vmcs12` to whichever caller happened to be guessed
    # at is exactly the error this column exists to avoid.
    children = [[] for _ in range(phase_count)]
    for index in range(phase_count):
        parent = PHASE_PARENT[index] if index < len(PHASE_PARENT) \
            else PHASE_CROSS
        if 0 <= parent < phase_count:
            children[parent].append(index)

    rt = round_trips or 1
    total = handler or 1

    print(f"\ncpu {cpu} phase tree "
          f"({round_trips:,} round trips, "
          f"{handler // max(round_trips, 1):,} handler cycles a round "
          f"trip)")
    print("     phase                                     calls  calls/RT"
          "     cyc/call      cyc/RT   self/RT   %vmm")

    def row(index, depth):
        if not calls[index]:
            return
        name = (("  " * depth) + PHASE_NAMES[index])[:36]
        own = cycles[index] - sum(cycles[c] for c in children[index])
        print(f"  {index:3d}  {name:<36} {calls[index]:>12,} "
              f"{calls[index] / rt:>8.2f} "
              f"{cycles[index] // calls[index]:>12,} "
              f"{cycles[index] / rt:>11,.0f} "
              f"{own / rt:>9,.0f} "
              f"{100.0 * cycles[index] / total:>6.1f}")
        for child in children[index]:
            row(child, depth + 1)

    top = [i for i in range(phase_count)
           if (i < len(PHASE_PARENT)) and (PHASE_PARENT[i] == PHASE_TOP)]

    for index in top:
        row(index, 0)

    covered = sum(cycles[i] for i in top)
    for label, value in (
            ("--- the six adjacent intervals", covered),
            ("--- handler_cycles", handler),
            ("--- outside the split", handler - covered)):
        print(f"       {label:<36} {'':>12} {'':>8} {'':>12} "
              f"{value / rt:>11,.0f} {'':>9} "
              f"{100.0 * value / total:>6.1f}")

    cross = [i for i in range(phase_count)
             if (i < len(PHASE_PARENT))
             and (PHASE_PARENT[i] == PHASE_CROSS) and calls[i]]
    if cross:
        # Named, because the alternative is that somebody reads a `self`
        # column as smaller than it is. A cross-cutting phase is inside
        # one of the rows above - `copy_shadow_to_vmcs12` is inside
        # `on_guest_vmlaunch` on one call and inside `vmptrld: flush
        # old` on another - and because it is not anybody's child it is
        # never subtracted from either. So those two `self` figures are
        # upper bounds by exactly this much, and adding these rows to
        # the tree above double counts them.
        print("       cross-cutting - each of these is already inside one "
              "of the rows above,\n       and is NOT subtracted from that "
              "row's self, because it has more than\n       one caller. "
              "Do not add them to the tree.")
        for index in cross:
            row(index, 1)


def dump_regions(args, elf, instance):
    """Where an exit's cycles are, by region rather than by counting.

    `handler_cycles` over `handler_exits` is the time from the first
    instruction this VMM controls on an exit to the last before it
    resumes. Subtracting that from the wall clock between successive
    exits gives everything else - the transition, the level above, and
    its guest.

    Both counters have been running since they were written and nothing
    has ever printed them, which is the fourth in this file found that
    way. The cost model they can settle - a VMCS read priced at ~2,984
    cycles against a phase of 198,309 for 60 of them that did not move
    when 44 were removed - is refuted without them.
    """
    members = ["handler_cycles", "handler_exits", "handler_first_tsc",
               "handler_last_tsc", "guest_state_reads_skipped",
               "guest_state_reads_done", "dilation_hidden",
               "dilation_charged", "dilation_offset"]
    off = gdb_offsets(elf, members)

    # Added later than the rest, so a dump of a binary that predates them
    # loses these two lines and nothing else.
    later = gdb_offsets(elf, ["window_entry_fast", "window_entry_slow"],
                        optional=True)
    off.update(later)

    reader = Monitor(args.rig, args.port)
    for member in off:
        reader.queue(instance + off[member], args.cpus)
    got = reader.run()

    def word(member, cpu):
        return got.get(instance + off[member] + 8 * cpu, 0)

    for cpu in range(args.cpus):
        exits = word("handler_exits", cpu)
        if not exits:
            continue

        inside = word("handler_cycles", cpu)
        span = word("handler_last_tsc", cpu) - word("handler_first_tsc", cpu)

        print(f"\ncpu {cpu} where an exit's cycles are")
        print(f"  exits handled            {exits:,}")
        print(f"  inside this VMM          {inside // exits:,} cycles/exit")
        if span > 0:
            print(f"  wall clock per exit      {span // exits:,} cycles/exit")
            share = 100.0 * inside / span
            print(f"  share inside this VMM    {share:.1f}%")
            print(f"  everything else          {(span - inside) // exits:,} "
                  f"cycles/exit  (transition, the level above, its guest)")

        skipped = word("guest_state_reads_skipped", cpu)
        done = word("guest_state_reads_done", cpu)
        if skipped or done:
            print(f"  guest-state reads: {done:,} done, {skipped:,} skipped"
                  f"  ({skipped / max(exits, 1):.1f} skipped per exit)")

        # What the guest's own clock was told about all of that. Both
        # halves, because the dilation actually achieved is not the one
        # asked for - the guest's own execution is never scaled, so the
        # ratio depends on how much of the machine the guest was getting,
        # and the asked-for figure alone would say nothing about that.
        # Which path the mapping window took. Printed for cpu 0 only,
        # since the counters are not per processor - a cache nobody has
        # watched hit is one that may not be hitting, which is the whole
        # reason these exist.
        if (0 == cpu) and ("window_entry_fast" in off):
            fast = got.get(instance + off["window_entry_fast"], 0)
            slow = got.get(instance + off["window_entry_slow"], 0)
            if fast or slow:
                total = fast + slow
                print(f"  mapping window: {fast:,} repoints through the "
                      f"cached leaf, {slow:,} through the full walk "
                      f"({100.0 * fast / max(total, 1):.2f}% fast, "
                      f"{total / max(exits, 1):.1f} per exit)")

        hidden = word("dilation_hidden", cpu)
        charged = word("dilation_charged", cpu)
        if hidden or charged:
            offset = word("dilation_offset", cpu)
            print(f"  time dilation: {hidden:,} cycles hidden, "
                  f"{charged:,} charged, offset -0x{(-offset) & (2**64-1):x}")
            if span > 0:
                print(f"    the guest's clock runs at "
                      f"{100.0 * (span - hidden) / span:.1f}% of the wall")
                print(f"    so its {17400} unit tick is "
                      f"{1.74 * span / max(span - hidden, 1):.2f} ms of it")


def dump_handler_by_reason(args, elf, instance):
    """Where the handler's time goes, by the reason that caused the exit.

    The phase table covers the reflection path and only about a third of
    exits take it, so roughly sixty per cent of the handler has never
    been attributed to anything.  Four optimisations aimed at the phases
    have each removed real work and left the total where it was; this is
    the split that says whether they were aimed at the wrong third.

    The sum of this must equal `handler_cycles`, and the line at the end
    says so - a split that does not add up is measuring a different span
    from the one it is being compared against.
    """
    members = ["handler_reason_cycles", "handler_reason_exits",
               "handler_cycles", "handler_exits", "handler_reason_from_l2",
               "handler_reason_reads", "handler_reason_writes"]
    off = gdb_offsets(elf, members, optional=True)
    if len(off) != len(members):
        print("\n[handler by reason: not in this binary]")
        return

    slots = 64
    reader = Monitor(args.rig, args.port)
    for member in ("handler_reason_cycles", "handler_reason_exits",
                   "handler_reason_from_l2", "handler_reason_reads",
                   "handler_reason_writes"):
        reader.queue(instance + off[member], slots)
    reader.queue(instance + off["handler_cycles"], 1)
    reader.queue(instance + off["handler_exits"], 1)
    got = reader.run()

    def row(member, i):
        return got.get(instance + off[member] + 8 * i, 0)

    total_cycles = got.get(instance + off["handler_cycles"], 0)
    total_exits = got.get(instance + off["handler_exits"], 0) or 1

    rows = []
    split_cycles = 0
    split_exits = 0
    for i in range(slots):
        cycles = row("handler_reason_cycles", i)
        exits = row("handler_reason_exits", i)
        if not exits:
            continue
        split_cycles += cycles
        split_exits += exits
        rows.append((cycles, exits, i))

    print(f"\ncpu 0 where the handler's time goes, by exit reason "
          f"({total_cycles / total_exits:,.0f} cycles/exit overall)")
    for cycles, exits, i in sorted(rows, reverse=True):
        # Whose exit it was. A second-level exit is reflected and comes
        # back as the guest hypervisor's VMRESUME, so the two are one
        # round trip rather than two costs.
        l2 = row("handler_reason_from_l2", i)
        whose = "L2" if l2 == exits else ("L1" if l2 == 0 else f"{l2}/{exits}")

        # Accesses beside cycles, over the same span. This is what says
        # whether a reason's cost is VMCS traffic or software: cycles
        # over accesses near the ~3,100 measured price means hardware,
        # far above it means the path is doing something that touches
        # nothing.
        rd = row("handler_reason_reads", i)
        wr = row("handler_reason_writes", i)
        access = rd + wr
        each = (f"{cycles / access:>8,.0f}" if access else f"{'-':>8}")
        print(f"  {EXIT_REASON.get(i, i):<14} {exits:>9,} "
              f"{cycles // max(exits, 1):>9,}cyc "
              f"{100.0 * cycles / max(total_cycles, 1):>5.1f}% "
              f"{rd / max(exits, 1):>7.1f}rd {wr / max(exits, 1):>7.1f}wr "
              f"{each}/acc  {whose}")

    print(f"  --- split covers {100.0 * split_cycles / max(total_cycles, 1):.1f}% "
          f"of the cycles and {100.0 * split_exits / total_exits:.1f}% "
          f"of the exits")

    # The one comparison the whole hypothesis turns on, printed rather
    # than left to be computed by hand from two rows.
    def per(reason_name):
        for i, name in EXIT_REASON.items():
            if name != reason_name:
                continue
            exits = row("handler_reason_exits", i) or 1
            return ((row("handler_reason_reads", i) +
                     row("handler_reason_writes", i)) / exits,
                    row("handler_reason_cycles", i) / exits)
        return None

    call, msr = per("vmcall"), per("wrmsr")
    if call and msr and msr[0]:
        print(f"  --- vmcall takes {call[0] / msr[0]:.2f}x the VMCS accesses "
              f"of a wrmsr and costs {call[1] / max(msr[1], 1):.2f}x the "
              f"cycles; near-equal ratios mean the excess is hardware, a "
              f"cost ratio far above the access ratio means it is software")


VMCS02_SPLIT = ["controls read and validated",
                "the three MSR areas checked",
                "ept pointer and TPR shadow decided",
                "bitmaps merged, own controls in hand",
                "the VMPTRLD itself",
                "host state once, then every control",
                "every guest-state field",
                "the event to inject, transition flush"]


def dump_vmcs02_split(args, elf, instance):
    """`build_vmcs02` split into adjacent intervals, with its coverage.

    Adjacent rather than nested, so the slots sum to the span between the
    first mark and the last by construction - a cost cannot fall between
    two of them.  What they can miss is a call that returned early, and
    the coverage line against `phase_cycles[2]` is what says so.
    """
    members = ["vmcs02_split_cycles", "vmcs02_split_calls",
               "vmcs02_split_reads", "vmcs02_split_writes",
               "phase_cycles", "phase_calls"]
    off = gdb_offsets(elf, members, optional=True)
    if len(off) != len(members):
        print("\n[build_vmcs02 split: not in this binary]")
        return

    slots = len(VMCS02_SPLIT)
    reader = Monitor(args.rig, args.port)
    reader.queue(instance + off["vmcs02_split_cycles"], slots)
    reader.queue(instance + off["vmcs02_split_reads"], slots)
    reader.queue(instance + off["vmcs02_split_writes"], slots)
    reader.queue(instance + off["vmcs02_split_calls"], 1)
    # phase 2 of processor 0, which is build_vmcs02's own bracket.
    reader.queue(instance + off["phase_cycles"] + 8 * 2, 1)
    reader.queue(instance + off["phase_calls"] + 8 * 2, 1)
    got = reader.run()

    split = [got.get(instance + off["vmcs02_split_cycles"] + 8 * i, 0)
             for i in range(slots)]
    reads = [got.get(instance + off["vmcs02_split_reads"] + 8 * i, 0)
             for i in range(slots)]
    writes = [got.get(instance + off["vmcs02_split_writes"] + 8 * i, 0)
              for i in range(slots)]
    calls = got.get(instance + off["vmcs02_split_calls"], 0)
    whole = got.get(instance + off["phase_cycles"] + 8 * 2, 0)
    whole_calls = got.get(instance + off["phase_calls"] + 8 * 2, 0) or 1

    total = sum(split) or 1
    print(f"\ncpu 0 build_vmcs02, split ({whole // whole_calls:,} cycles a "
          f"call over {whole_calls:,} calls)")
    print(f"  {'slot':<40} {'cyc/call':>9} {'share':>6} "
          f"{'rd/call':>8} {'wr/call':>8} {'cyc/access':>11}")
    for i, cycles in sorted(enumerate(split), key=lambda kv: -kv[1]):
        access = reads[i] + writes[i]
        # The number the whole cost model turns on. A slot whose cycles
        # over its accesses lands near the launch-time price vindicates
        # that price in the settled state; one that lands far from it
        # tells us the marginal price for the first time.
        # Blank unless the slot really takes accesses. A slot with one
        # access in three hundred thousand calls divides to a number in
        # the hundreds of millions, which reads as a finding and is an
        # artefact of the denominator.
        each = (f"{cycles / access:>11,.0f}"
                if access and (access / whole_calls) >= 0.01
                else f"{'-':>11}")
        print(f"  {VMCS02_SPLIT[i]:<40} {cycles // whole_calls:>9,} "
              f"{100.0 * cycles / total:>5.1f}% "
              f"{reads[i] / whole_calls:>8.2f} {writes[i] / whole_calls:>8.2f}"
              f" {each}")

    all_access = sum(reads) + sum(writes)

    # Over the slots that *take* accesses only. Dividing the whole
    # phase's cycles by the whole phase's accesses charges the software
    # slots to the hardware price and answers a question nobody asked.
    hot = [i for i in range(slots)
           if (reads[i] + writes[i]) / whole_calls >= 0.01]
    hot_cycles = sum(split[i] for i in hot)
    hot_access = sum(reads[i] + writes[i] for i in hot)
    cold_cycles = total - hot_cycles

    print(f"  --- coverage {100.0 * total / max(whole, 1):.1f}% of the "
          f"phase's cycles, {100.0 * calls / whole_calls:.1f}% of its calls "
          f"reached the end")
    print(f"  --- {all_access / whole_calls:.1f} VMCS accesses a call; over "
          f"the slots that take them, {hot_cycles / max(hot_access, 1):,.0f} "
          f"cycles each (launch-time price list says ~3,100 a read, ~2,200 "
          f"a write)")
    print(f"  --- {100.0 * hot_cycles / max(total, 1):.1f}% of the phase is "
          f"slots that touch the VMCS, {100.0 * cold_cycles / max(total, 1):.1f}%"
          f" is software that touches nothing")


REFLECT_SLOTS = ["save_l2_state", "reflect_l2_exit", "exit information"]


def dump_reflect_buckets(args, elf, instance):
    """A `vmcall` reflection against a `wrmsr` one, phase by phase.

    Every reflection on this machine costs about 51 VMCS accesses except
    `vmcall`, which costs 230.4, and both take the same path - so the
    extra accesses are in a phase they share.  These three are the
    phases an L2 exit takes; they do not bracket the whole handler, so
    the residue against `handler_reason_*` for the same reason is printed
    as coverage rather than left implied.
    """
    members = ["bucket_phase_cycles", "bucket_phase_reads",
               "bucket_phase_writes", "bucket_calls",
               "handler_reason_cycles", "handler_reason_reads",
               "handler_reason_writes", "handler_reason_exits"]
    off = gdb_offsets(elf, members, optional=True)
    if len(off) != len(members):
        print("\n[reflection buckets: not in this binary]")
        return

    slots = len(REFLECT_SLOTS)
    reader = Monitor(args.rig, args.port)
    for member in ("bucket_phase_cycles", "bucket_phase_reads",
                   "bucket_phase_writes"):
        reader.queue(instance + off[member], 2 * slots)
    reader.queue(instance + off["bucket_calls"], 2)
    for member in ("handler_reason_cycles", "handler_reason_reads",
                   "handler_reason_writes", "handler_reason_exits"):
        reader.queue(instance + off[member], 64)
    got = reader.run()

    def cell(member, bucket, slot):
        return got.get(instance + off[member] + 8 * (bucket * slots + slot), 0)

    def whole(member, reason):
        for i, name in EXIT_REASON.items():
            if name == reason:
                return got.get(instance + off[member] + 8 * i, 0)
        return 0

    print("\ncpu 0 a vmcall reflection against a wrmsr one, by phase")
    for bucket, reason in ((0, "vmcall"), (1, "wrmsr")):
        exits = whole("handler_reason_exits", reason) or 1
        wc = whole("handler_reason_cycles", reason)
        wr = whole("handler_reason_reads", reason)
        ww = whole("handler_reason_writes", reason)
        print(f"  {reason} ({exits:,} exits, {wc // exits:,} cyc, "
              f"{(wr + ww) / exits:.1f} accesses an exit)")
        sc = sr = sw = 0
        for slot in range(slots):
            c = cell("bucket_phase_cycles", bucket, slot)
            r = cell("bucket_phase_reads", bucket, slot)
            w = cell("bucket_phase_writes", bucket, slot)
            sc += c
            sr += r
            sw += w
            print(f"    {REFLECT_SLOTS[slot]:<18} {c / exits:>10,.0f} cyc "
                  f"{r / exits:>7.1f}rd {w / exits:>7.1f}wr")
        print(f"    {'residue':<18} {(wc - sc) / exits:>10,.0f} cyc "
              f"{(wr - sr) / exits:>7.1f}rd {(ww - sw) / exits:>7.1f}wr")
        print(f"    --- the three phases hold "
              f"{100.0 * sc / max(wc, 1):.1f}% of the cycles and "
              f"{100.0 * (sr + sw) / max(wr + ww, 1):.1f}% of the accesses")


def dump_profile(args, elf, instance):
    """Where the second-level guest is, sampled on a clock it cannot see.

    The *distribution* is the reading, not the top entries.  A table that
    fills and overflows with a low maximum is a guest executing widely; a
    table with a few slots holding most of the samples is a spin, and the
    addresses name it.  So `profile_overflow` and the maximum are printed
    before the list rather than after it.
    """
    members = ["profile_rip", "profile_hits", "profile_samples",
               "profile_overflow"]
    off = gdb_offsets(elf, members, optional=True)
    if len(off) != len(members):
        print("\n[l2 profile: not in this binary]")
        return

    slots = 64
    reader = Monitor(args.rig, args.port)
    reader.queue(instance + off["profile_rip"], slots)
    reader.queue(instance + off["profile_hits"], slots)
    reader.queue(instance + off["profile_samples"], 1)
    reader.queue(instance + off["profile_overflow"], 1)
    got = reader.run()

    rows = []
    for i in range(slots):
        rip = got.get(instance + off["profile_rip"] + 8 * i, 0)
        hits = got.get(instance + off["profile_hits"] + 8 * i, 0)
        if hits:
            rows.append((hits, rip))

    samples = got.get(instance + off["profile_samples"], 0)
    overflow = got.get(instance + off["profile_overflow"], 0)

    if not samples:
        print("\n[l2 profile: no samples - is ZPP_PROFILE_L2 on?]")
        return

    rows.sort(reverse=True)
    top = rows[0][0] if rows else 0
    covered = sum(h for h, _ in rows)

    # `profile_overflow` counts *flushes*, not rejected samples:
    # `record_profile_sample` empties the whole table when it fills, so
    # the hits below are only those since the last flush. Reporting them
    # as a share of all samples would divide a partial fill by the whole
    # run and read as "the table holds 1.4%", which says nothing.
    print(f"\ncpu 0 second-level profile: {samples:,} samples, "
          f"{len(rows)} slots filled since the last flush, "
          f"{overflow:,} flushes")
    print(f"  the shape: {overflow:,} flushes means the table filled with "
          f"{slots} distinct addresses that many times - about "
          f"{overflow * slots:,} distinct-address fills over {samples:,} "
          f"samples")
    print(f"  top slot {top:,} hits of the {covered:,} since the last flush")
    print("  many flushes with a low maximum is a guest executing widely; "
          "a spin fills the table once and then never flushes again")
    for hits, rip in rows[:24]:
        print(f"    0x{rip:016x}  {hits:>8,}  "
              f"{100.0 * hits / max(samples, 1):5.1f}%")


def dump_guest_state_shadow(args, elf, instance):
    """Where the deferred guest-state copy's model differs from vmcs02.

    Shadow mode computes what the deferral would leave in vmcs02 and
    compares it against what the eager path is about to write. A
    divergence is a field the deferral would have got wrong - which is
    the failure three boots could only report as a reset.
    """
    members = ["shadow_divergences", "shadow_divergence_by_field",
               "shadow_divergence_field", "shadow_divergence_in_vmcs02",
               "shadow_divergence_in_vmcs12", "shadow_divergence_owner",
               "shadow_divergence_dirty", "shadow_divergence_entries",
               "guest_state_defers"]
    off = gdb_offsets(elf, members)
    slots = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)"
        "->shadow_divergence_field / 8"])[0]

    reader = Monitor(args.rig, args.port)
    for member in ("shadow_divergences", "guest_state_defers"):
        reader.queue(instance + off[member], args.cpus)
    reader.queue(instance + off["shadow_divergence_by_field"], 48)
    for member in ("shadow_divergence_field", "shadow_divergence_in_vmcs02",
                   "shadow_divergence_in_vmcs12", "shadow_divergence_owner",
                   "shadow_divergence_dirty", "shadow_divergence_entries"):
        reader.queue(instance + off[member], slots)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    total = sum(word("shadow_divergences", c) for c in range(args.cpus))
    defers = sum(word("guest_state_defers", c) for c in range(args.cpus))

    print(f"\nguest-state shadow: {total:,} divergences over "
          f"{defers:,} deferrable exits")

    if not total:
        print("  none - the deferral's model matched vmcs02 every time, "
              "so the fourth condition is not a stale field value")
        return

    rows = [(word("shadow_divergence_by_field", i), i) for i in range(48)]
    rows = [r for r in rows if r[0]]
    print("  by field index into guest_state_fields:")
    for count, index in sorted(rows, reverse=True):
        print(f"    slot {index:>2}  {count:>10}")

    print("  first few, in full:")
    for i in range(min(slots, total)):
        if not word("shadow_divergence_field", i):
            continue
        print(f"    field 0x{word('shadow_divergence_field', i):04x}  "
              f"vmcs02 0x{word('shadow_divergence_in_vmcs02', i):x}  "
              f"vmcs12 0x{word('shadow_divergence_in_vmcs12', i):x}  "
              f"owner 0x{word('shadow_divergence_owner', i):x}  "
              f"dirty 0x{word('shadow_divergence_dirty', i):x}  "
              f"at entry {word('shadow_divergence_entries', i):,}")


def dump_l1_host_audit(args, elf, instance):
    """Which of `load_l1_host_state`'s writes the processor undoes.

    The audit has been running since it was written and **nothing has
    ever read it**, which is its own lesson: a counter nobody prints is
    a measurement nobody has.

    It exists because the obvious optimisation here is unsound. Those
    writes go into vmcs01's *guest* fields, and SDM 30.3.2 has every VM
    exit save the guest hypervisor's own state over them - so a cache of
    what this VMM last wrote describes something the processor has since
    overwritten, and eliding against it would skip a write that is owed.
    That is what killed `ZPP_LAZY_GUEST_STATE`.

    What *is* sound is dropping a write the processor demonstrably never
    undoes, and that is a measurement. A slot at zero across a whole
    boot is one whose write can go; a slot that is not is one that never
    could have.
    """
    members = ["l1_host_field", "l1_host_changed", "l1_host_count",
               "l1_host_audits", "l1_host_samples", "l1_host_elided",
               "l1_host_diverged"]
    off = gdb_offsets(elf, members)
    slots = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->l1_host_field[0] / 8"
    ])[0]

    reader = Monitor(args.rig, args.port)
    for member in ("l1_host_field", "l1_host_changed"):
        reader.queue(instance + off[member], args.cpus * slots)
    reader.queue(instance + off["l1_host_samples"], args.cpus * slots)
    for member in ("l1_host_count", "l1_host_audits", "l1_host_elided",
                   "l1_host_diverged"):
        reader.queue(instance + off[member], args.cpus)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    for cpu in range(args.cpus):
        used = word("l1_host_count", cpu)
        if not used:
            continue

        audits = word("l1_host_audits", cpu)
        rows = [(i, word("l1_host_field", cpu * slots + i),
                 word("l1_host_changed", cpu * slots + i))
                for i in range(min(used, slots))]
        stable = [r for r in rows if 0 == r[2]]

        elided = word("l1_host_elided", cpu)
        diverged = word("l1_host_diverged", cpu)

        print(f"\ncpu {cpu} load_l1_host_state audit "
              f"({used} fields written, {audits:,} samples)")
        print(f"  {len(stable)} of {len(rows)} slots never observed "
              f"changed - those writes are the elidable set")
        print(f"  writes elided: {elided:,}")
        print(f"  DIVERGED AFTER ELISION: {diverged:,}"
              + ("   <- the safety property failed, read the log"
                 if diverged else "   (the safety property holds)"))
        for index, encoding, changed in rows:
            if changed:
                print(f"    slot {index:>2}  field 0x{encoding:04x}  "
                      f"changed {changed:>8}  <- the processor undoes "
                      f"this one")


def dump_reference_tsc(args, elf, instance):
    """What clock the guest was handed, in hertz.

    **This is the reading that would have caught a wrong scale, and there
    was no way to take it.**  The reference TSC page carries a fixed
    point multiplier and the guest computes
    `((rdtsc * scale) >> 64) + offset`; nothing about a scale of
    `0x0148f2db8d6da21a` says whether it is right, and three sessions
    quoted one at each other without anybody being able to say.

    Reference time is counted in 100-nanosecond units, so the counter
    advances at exactly 10,000,000 ticks a second - by the Hyper-V
    specification, not by observation.  `implied` below is one second of
    time-stamp counter pushed through the page's own arithmetic, so it
    **must read about 10,000,000**.  Anything else and the guest is
    living in a different second from the one it is being told about,
    which sets the rate of every timer it programs.

    Two columns, not one, because a single-field instrument cannot tell
    you it is aimed at the wrong field: `computed` is the scale derived
    from the counter frequency, `fitted` is the slope through the guest
    hypervisor's own answers for the counter MSR.  They should agree.
    Where they do not, `baseline` says whether the fit could possibly
    have been right - it is the time-stamp counter its two ends span, and
    each end carries the reflection cost, about 2.5 ms, as error.
    """
    members = ["reference_scale", "reference_offset", "reference_published",
               "reference_fit_error", "reference_tsc_frequency",
               "reference_fitted_scale", "reference_implied_hz",
               "reference_fit_implied_hz", "reference_baseline_tsc",
               "reference_publishes", "reference_read_count",
               "l2_reference_tsc_written"]
    off = gdb_offsets(elf, members)

    reader = Monitor(args.rig, args.port)
    for member in members:
        reader.queue(instance + off[member], args.cpus)
    got = reader.run()

    def word(member, cpu):
        return got.get(instance + off[member] + 8 * cpu, 0)

    # The counter frequency the hypervisor found for itself, where it
    # found one.  Zero is the expected answer on this rig and is not a
    # failure to read: QEMU's `cpu_x86_cpuid` has no case for CPUID leaf
    # 0x15 and its default returns zero, and `kvm_x86_build_cpuid` builds
    # the guest's table from that function.  The fallback below is the
    # tree's own wall-clock measurement - 179,446,096,055 counts over a
    # 90.08 second window - and it is labelled as a fallback wherever it
    # is used, because a diagnostic that silently substitutes a constant
    # for a reading is how the last three unit slips happened.
    measured = 1_992_000_000

    printed = False
    for cpu in range(args.cpus):
        enabled = word("l2_reference_tsc_written", cpu)
        if not enabled and not word("reference_read_count", cpu):
            continue

        if not printed:
            print("\nreference TSC page: the guest's clock, in hertz")
            printed = True

        tsc_hz = word("reference_tsc_frequency", cpu)
        source = "CPUID.15H"
        if not tsc_hz:
            tsc_hz = measured
            source = "FALLBACK, measured at the wall; CPUID.15H read zero"

        published = word("reference_published", cpu)
        scale = word("reference_scale", cpu)
        fitted = word("reference_fitted_scale", cpu)
        baseline = word("reference_baseline_tsc", cpu)

        # Computed here rather than trusted from the member, so a stale
        # deployed binary that does not have the member still gets a
        # reading - and so the two can disagree, which is the only way a
        # reader catches itself.
        def implied(value):
            return (value * tsc_hz) >> 64

        print(f"\n  cpu {cpu}  page 0x{enabled & ~0xfff:x} "
              f"{'enabled' if enabled & 1 else 'DISABLED'}, "
              f"published {word('reference_publishes', cpu)} time(s), "
              f"{word('reference_read_count', cpu):,} counter reads")
        print(f"    time-stamp counter {tsc_hz:,} Hz  ({source})")

        for what, value in (("published", scale), ("fitted", fitted)):
            if not value:
                print(f"    {what:<9} scale -                    "
                      f"        -")
                continue
            hz = implied(value)
            # 0.1%, which is `reference_tsc::frequency_tolerance`.  The
            # honest fits recorded in BACKLOG.md came out at 9,998,562
            # and 10,000,215 Hz; the failure was 15.8 million.
            verdict = ("ok" if abs(hz - 10_000_000) <= 10_000
                       else f"WRONG by {hz / 10_000_000.0:.3f}x")
            print(f"    {what:<9} scale 0x{value:016x}  "
                  f"implies {hz:>12,} Hz  {verdict}")

        if baseline:
            print(f"    fit baseline {baseline:,} counts "
                  f"({1000.0 * baseline / tsc_hz:.1f} ms)")
        # The removed check's own verdict, in hundred-nanosecond units,
        # kept beside the frequency it could not see.  A zero here next
        # to a WRONG above is the whole story: collinear samples always
        # reproduce themselves, whatever their slope.
        print(f"    offset 0x{word('reference_offset', cpu):x}, "
              f"collinearity error {word('reference_fit_error', cpu):,} "
              f"x100ns")

        if not published:
            print("    NOT PUBLISHED - the guest is still reading the "
                  "counter MSR")

    if not printed:
        print("\nreference TSC page: never enabled by the guest")


def dump_tick_account(args, elf, instance):
    """**What the level above believes elapsed time to be, against what
    it is.**

    The whole investigation turns on one ratio and nothing could state
    it.  The second-level guest arms Hyper-V synthetic timer 0
    *periodically* with 17,400 hundred-nanosecond units - 1.74 ms,
    574.7 Hz - and the clock vector arrives at about 1,080/s, which is
    0.926 ms.  That was only ever obtained by dividing two *rates*
    sampled from two different counters over a window, and a ratio
    between two quantities that were never measured together is exactly
    the mistake this file has already retired twice ("the ratio near
    1213 was two different quantities").

    `asked` and `given` below are measured on **one** clock, per arm:
    the hypervisor timestamps the guest's write of `STIMER0_COUNT` and
    the clock vector that answers it, and sums both.  `given / asked` is
    the factor, directly.

    Three things make it able to fail rather than merely print:

    - The two arm counts are separate.  Every arm is counted in `asked`;
      only an arm that a clock vector answered is counted in `given`.
      They disagreeing means the vector is not the answer to the arm,
      which is the one assumption the ratio rests on - and `unanswered`
      is the same failure seen from the other side.
    - The timeline below interleaves the guest's arms with the level
      above's *own* local APIC timer armings, which is the clock it
      actually schedules the expiry on.  A count against the real time
      to the next arming gives that timer's rate, and the rate against
      the count gives the interval the level above **intended**.  Three
      numbers, and they separate "the level above converted 1.74 ms into
      0.926 ms" from "it asked for 1.74 ms and the timer fired early".
    - Nothing here substitutes a constant for a reading without saying
      so.  The time-stamp counter frequency falls back to the tree's own
      wall-clock measurement and is labelled when it does.

    **If the APIC half of the timeline is empty, that is a finding and
    not a broken reader.**  `timer_arm_recent_*` is filled from the write
    watch on the local APIC *page*, so it sees an xAPIC-mode timer and
    nothing else.  A level above using x2APIC (`IA32_X2APIC_INIT_COUNT`)
    or TSC-deadline mode programs an MSR instead, and this VMM traps
    those two only when `arm_guest_timer_poll` is armed - which happens
    only where the processor refuses the VMX-preemption timer.  Empty
    here therefore means "go and arm that", not "it programs no timer".
    """
    members = ["stimer_asked_units", "stimer_asked_arms",
               "stimer_given_cycles", "stimer_given_arms",
               "stimer_unanswered", "stimer_arm_pending_tsc",
               "l2_stimer_config", "reference_tsc_frequency"]
    rings = ["stimer_arm_value", "stimer_arm_tsc", "stimer_arm_kind",
             "stimer_arm_count", "timer_arm_recent_value",
             "timer_arm_recent_tsc", "timer_arm_recent_lvt",
             "timer_arm_recent_divide", "timer_arm_recent_count"]
    off = gdb_offsets(elf, members + rings)

    # Both rings are 32 deep.  `reference_sample_capacity` and
    # `timer_arm_capacity` are separate constants in the header that
    # happen to be equal; tests/python_layout asserts both against it,
    # so a change there fails a test rather than misreading a ring.
    stimer_capacity = 32
    apic_capacity = 32

    reader = Monitor(args.rig, args.port)
    for member in members:
        reader.queue(instance + off[member], args.cpus)
    for member in ("stimer_arm_count", "timer_arm_recent_count"):
        reader.queue(instance + off[member], args.cpus)
    for member in ("stimer_arm_value", "stimer_arm_tsc",
                   "stimer_arm_kind"):
        reader.queue(instance + off[member], args.cpus * stimer_capacity)
    for member in ("timer_arm_recent_value", "timer_arm_recent_tsc",
                   "timer_arm_recent_lvt", "timer_arm_recent_divide"):
        reader.queue(instance + off[member], args.cpus * apic_capacity)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    measured = 1_992_000_000

    printed = False
    for cpu in range(args.cpus):
        asked_arms = word("stimer_asked_arms", cpu)
        given_arms = word("stimer_given_arms", cpu)
        stimer_n = word("stimer_arm_count", cpu)
        apic_n = word("timer_arm_recent_count", cpu)

        if not (asked_arms or given_arms or stimer_n or apic_n):
            continue

        if not printed:
            print("\nthe tick account: asked against given")
            printed = True

        tsc_hz = word("reference_tsc_frequency", cpu)
        source = "CPUID.15H"
        if not tsc_hz:
            tsc_hz = measured
            source = "FALLBACK, measured at the wall; CPUID.15H read zero"

        config = word("l2_stimer_config", cpu)
        print(f"\n  cpu {cpu}  STIMER0_CONFIG 0x{config:x} "
              f"({'periodic' if config & 2 else 'one-shot'}, "
              f"{'enabled' if config & 1 else 'DISABLED'}), "
              f"time-stamp counter {tsc_hz:,} Hz ({source})")

        if not asked_arms:
            print("    no periodic arm recorded - the guest has not "
                  "programmed a period yet, or STIMER0_CONFIG's periodic "
                  "bit was never seen")
            continue

        # 100 ns units in, 100 ns units out, so the two sides are the
        # same quantity before they are divided.
        asked_units = word("stimer_asked_units", cpu) / asked_arms
        print(f"    asked  {asked_units:>12,.1f} x100ns per arm "
              f"({asked_units / 10_000.0:.3f} ms, "
              f"{10_000_000.0 / max(asked_units, 1):.1f} Hz)  "
              f"over {asked_arms:,} arms")

        if not given_arms:
            print("    given  -  NOT ONE ARM WAS ANSWERED by the clock "
                  "vector.  The ratio cannot be formed, and that is the "
                  "finding: the vector is not the answer to the arm.")
        else:
            cycles = word("stimer_given_cycles", cpu) / given_arms
            given_units = cycles * 10_000_000.0 / tsc_hz
            print(f"    given  {given_units:>12,.1f} x100ns per arm "
                  f"({given_units / 10_000.0:.3f} ms, "
                  f"{10_000_000.0 / max(given_units, 1):.1f} Hz)  "
                  f"over {given_arms:,} answered")

            ratio = asked_units / max(given_units, 1e-9)
            verdict = ("ok" if abs(ratio - 1.0) <= 0.05
                       else f"EARLY by {ratio:.3f}x")
            if ratio < 0.95:
                verdict = f"LATE by {1.0 / ratio:.3f}x"
            print(f"    ratio  {ratio:>12.3f}x  {verdict}")

        # The assumption the ratio rests on, stated rather than assumed.
        missing = asked_arms - given_arms
        unanswered = word("stimer_unanswered", cpu)
        note = "one vector per arm" if missing <= 1 else "DISAGREE"
        print(f"    arms asked {asked_arms:,}, answered {given_arms:,}, "
              f"displaced before an answer {unanswered:,}  ({note})")
        if word("stimer_arm_pending_tsc", cpu):
            print("    one arm is outstanding, which is normal - it is "
                  "the arm the guest is currently waiting on")

        # ------------------------------------------- the timeline
        #
        # Two rings merged on the one clock they share.  Kinds 1, 2 and 3
        # are the guest's count write, its config write and the clock
        # vector; 'apic' rows are the level above arming its own timer,
        # which is what it schedules the expiry on.
        rows = []
        kinds = {1: "STIMER0_COUNT", 2: "STIMER0_CONFIG",
                 3: "clock vector injected"}
        for i in range(min(stimer_n, stimer_capacity)):
            slot = (stimer_n - 1 - i) % stimer_capacity
            tsc = word("stimer_arm_tsc", cpu * stimer_capacity + slot)
            if not tsc:
                continue
            kind = word("stimer_arm_kind", cpu * stimer_capacity + slot)
            value = word("stimer_arm_value", cpu * stimer_capacity + slot)
            rows.append((tsc, kinds.get(kind, f"kind {kind}"), value, None))

        for i in range(min(apic_n, apic_capacity)):
            slot = (apic_n - 1 - i) % apic_capacity
            tsc = word("timer_arm_recent_tsc", cpu * apic_capacity + slot)
            if not tsc:
                continue
            value = word("timer_arm_recent_value",
                         cpu * apic_capacity + slot)
            lvt = word("timer_arm_recent_lvt", cpu * apic_capacity + slot)
            divide = word("timer_arm_recent_divide",
                          cpu * apic_capacity + slot)
            rows.append((tsc, "apic initial count", value, (lvt, divide)))

        if not rows:
            continue

        rows.sort()

        # ------------------- what the level above intended to wait for
        #
        # It schedules a synthetic timer's expiry on its own local APIC
        # timer - measured, not assumed: the watch on the APIC page sees
        # it write the initial count once per tick, one-shot at vector
        # 0xef with divide-by-one.  So its arming, converted at that
        # timer's rate, is the interval it *meant* to wait, and that is
        # the third number the other two cannot supply.
        #
        # **Two rates, deliberately, because they can disagree.** The
        # nominal is 1.0 GHz - this tree's own earlier measurement on
        # this rig, recorded in BACKLOG.md where 2,382,592,343 counts
        # were observed against a 4.747e9-cycle gap, which is where the
        # "ratio near 1213" was retired.  It is not far-fetched
        # arithmetic either: KVM converts a count to a wall-clock
        # deadline in `tmict_to_ns` as `tmict * apic_bus_cycle_ns *
        # divide_count`, so the rate is a nanosecond-domain constant and
        # not anything derived from this processor.
        #
        # The fitted rate beside it is what these armings actually did:
        # a count, against the real time until the next arming.  It is
        # only the timer's rate if each one-shot arming ran to expiry -
        # so a fit far above the nominal does not mean a fast timer, it
        # means the level above re-armed early, and it is *that* which
        # says the verdict below cannot be trusted.  A single-rate
        # instrument could not tell the two apart.
        apic = [r for r in rows if r[1] == "apic initial count"]
        nominal = 1.0e9
        if len(apic) >= 2:
            fits = []
            for a, b in zip(apic, apic[1:]):
                gap = b[0] - a[0]
                if gap > 0 and a[2]:
                    fits.append(a[2] * tsc_hz / gap)
            if fits:
                fits.sort()
                rate = fits[len(fits) // 2]
                agrees = abs(rate - nominal) <= 0.1 * nominal
                print(f"\n    the level above's own APIC timer")
                print(f"      nominal {nominal / 1e6:,.1f} MHz "
                      f"(measured on this rig, BACKLOG.md)")
                print(f"      fitted  {rate / 1e6:,.1f} MHz "
                      f"(median of {len(fits)} armings)  "
                      f"{'agrees' if agrees else 'DISAGREES - the level '
                         'above is re-arming before expiry, so the '
                         'intended interval below is not what it waited'}")

                last = apic[-1][2]
                if last:
                    intended = last / nominal * 10_000_000.0
                    print(f"      its last arming of {last:,} counts is "
                          f"{intended:,.1f} x100ns "
                          f"({intended / 10_000.0:.3f} ms) at the "
                          f"nominal rate - what it INTENDED to wait")

                    # The discriminator, spelled out rather than left to
                    # the reader, because getting it the wrong way round
                    # sends the next session to the wrong layer.
                    to_asked = abs(intended - asked_units)
                    to_given = (abs(intended - given_units)
                                if given_arms else None)
                    if to_given is not None and to_given < to_asked:
                        print("      -> intended matches GIVEN, not "
                              "asked: the level above converted the "
                              "guest's period into a shorter wait. The "
                              "fault is its notion of elapsed time, and "
                              "the reference-page fit above says whether "
                              "its counter is fast too.")
                    elif to_given is not None:
                        print("      -> intended matches ASKED: the "
                              "level above wanted the right interval and "
                              "the timer fired early underneath it. The "
                              "fault is below it - this VMM or KVM - not "
                              "in its clock.")

        first = rows[0][0]
        print("\n    timeline, oldest first (us from the first row)")
        for tsc, what, value, extra in rows:
            when = (tsc - first) * 1e6 / tsc_hz
            tail = ""
            if extra is not None:
                lvt, divide = extra
                mode = {0: "one-shot", 1: "periodic",
                        2: "tsc-deadline"}.get((lvt >> 17) & 3, "?")
                tail = (f"  lvt 0x{lvt:x} ({mode}, vector 0x{lvt & 0xff:x}"
                        f"{', masked' if lvt & 0x10000 else ''}), "
                        f"divide 0x{divide:x}")
            print(f"      {when:10.1f} us  {what:<22} {value:>14,}{tail}")


def dump_vtl(args, elf, instance):
    """The trust-level switch loop: whether it advances, and who calls it.

    `changed` is against the previous switch of the same kind rather than
    against the first, so it reads as "how often this register moved
    while the loop ran".  All zero is a livelock; whichever rows are not
    zero say what the loop carries.
    """
    members = ["vtl_switches", "vtl_differed", "vtl_first", "vtl_latest",
               "vtl_stack", "vtl_rip", "vtl_rsp", "vtl_cr3",
               "vtl_image_base", "vtl_caller_base", "vtl_caller_address",
               "vtl_image_name", "vtl_caller_name", "vtl_captured",
               "vtl_code", "vtl_code_base", "vtl_assist",
               "l2_vp_assist", "l2_vp_assist_eptp", "vtl_assist_read",
               "vtl_assist_error", "vtl_assist_first"]
    off = gdb_offsets(elf, members)

    kind = "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_differed[0][0]"
    slots, kinds, stack_words, name_size = gdb_values(elf, [
        f"{kind} / 8",
        f"sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_differed[0] "
        f"/ {kind}",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_stack[0] / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_image_name[0]"])

    reader = Monitor(args.rig, args.port)
    reader.queue(instance + off["vtl_switches"], args.cpus * kinds)
    for cpu in range(args.cpus):
        for k in range(kinds):
            for member in ("vtl_differed", "vtl_first", "vtl_latest"):
                reader.queue(instance + off[member]
                             + ((cpu * kinds + k) * slots) * 8, slots)
    for member in ("vtl_rip", "vtl_rsp", "vtl_cr3", "vtl_image_base",
                   "vtl_caller_base", "vtl_caller_address",
                   "vtl_captured"):
        reader.queue(instance + off[member], kinds)
    reader.queue(instance + off["vtl_stack"], kinds * stack_words)
    code_size = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_code[0]"])[0]
    reader.queue(instance + off["vtl_code"], kinds * code_size // 8)
    reader.queue(instance + off["vtl_code_base"], kinds)
    assist_size = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_assist[0][0]"])[0]
    reader.queue(instance + off["vtl_assist"],
                 kinds * 2 * assist_size // 8)
    reader.queue(instance + off["l2_vp_assist"], 2)
    reader.queue(instance + off["l2_vp_assist_eptp"], 2)
    for member in ("vtl_assist_read", "vtl_assist_error",
                   "vtl_assist_first"):
        reader.queue(instance + off[member], kinds * 2)
    for member in ("vtl_image_name", "vtl_caller_name"):
        reader.queue(instance + off[member], kinds * name_size // 8)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    def text(member, k):
        raw = b"".join(word(member, k * name_size // 8 + i)
                       .to_bytes(8, "little")
                       for i in range(name_size // 8))
        return raw.split(b"\0")[0].decode("ascii", "replace")

    if not any(word("vtl_switches", i) for i in range(args.cpus * kinds)):
        return

    for cpu in range(args.cpus):
        for k in range(kinds):
            count = word("vtl_switches", cpu * kinds + k)
            if not count:
                continue
            print(f"\n--- cpu {cpu}: {VTL_KINDS[k]}, {count:,} switches ---")
            print("     register       changed  first                "
                  "latest")
            for s, name in enumerate(VTL_SLOTS[:slots]):
                index = (cpu * kinds + k) * slots + s
                print(f"     {name:<8} {word('vtl_differed', index):>12}  "
                      f"0x{word('vtl_first', index):<16x}   "
                      f"0x{word('vtl_latest', index):x}")

    for k in range(kinds):
        if not word("vtl_captured", k):
            continue
        print(f"\n--- {VTL_KINDS[k]} call site ---")
        print(f"  rip 0x{word('vtl_rip', k):x} "
              f"rsp 0x{word('vtl_rsp', k):x} "
              f"cr3 0x{word('vtl_cr3', k):x}")
        print(f"  image  0x{word('vtl_image_base', k):x} "
              f"{text('vtl_image_name', k)!r}")
        print(f"  caller 0x{word('vtl_caller_base', k):x} "
              f"{text('vtl_caller_name', k)!r} "
              f"at 0x{word('vtl_caller_address', k):x}")
        for i in range(stack_words):
            value = word("vtl_stack", k * stack_words + i)
            if value:
                print(f"    +0x{i * 8:03x}  0x{value:x}")

        # The loop body, as bytes.  Disassembled outside rather than
        # here: there is no x86 decoder in this script and adding one to
        # print a dozen instructions would be a second decoder to keep
        # right.  llvm-objdump takes it from the hex directly.
        raw = b"".join(word("vtl_code", k * code_size // 8 + i)
                       .to_bytes(8, "little")
                       for i in range(code_size // 8))
        if any(raw):
            print(f"  code at 0x{word('vtl_code_base', k):x}:")
            print("    " + raw.hex())

        # The page the trust levels talk through.  Printed as the
        # quadwords that are non-zero, because most of it is reserved
        # and a full hex dump of two 512 byte pages per side buries the
        # handful of fields that carry anything.
        for level in range(2):
            msr = word("l2_vp_assist", level)
            base = ((k * 2) + level) * assist_size // 8
            live = [(i * 8, word("vtl_assist", base + i))
                    for i in range(assist_size // 8)
                    if word("vtl_assist", base + i)]
            if not (msr or live
                    or word("vtl_assist_error", k * 2 + level)):
                continue
            print(f"  vp assist level {level}: msr 0x{msr:x} "
                  f"eptp 0x{word('l2_vp_assist_eptp', level):x} "
                  f"read {word('vtl_assist_read', k * 2 + level)} bytes "
                  f"err 0x{word('vtl_assist_error', k * 2 + level):x} "
                  f"first 0x{word('vtl_assist_first', k * 2 + level):x}")
            for at, value in live:
                mark = "  <- vtl control" if 0x100 <= at < 0x140 else ""
                print(f"    +0x{at:03x}  0x{value:016x}{mark}")


def dump_vtl_steps(args, elf, instance):
    """The instruction trace of each side of the trust-level loop.

    Printed as the ordered addresses with their repeat counts collapsed,
    plus the distinct addresses with their bytes, because the loop is
    expected to be short and a thousand lines of the same three
    addresses buries what they are.

    The bytes are not disassembled here for the reason `dump_vtl` gives
    about the call site's window: there is no x86 decoder in this script
    and llvm-objdump takes the hex directly.
    """
    members = ["vtl_step_rip", "vtl_step_cr3", "vtl_step_count",
               "vtl_step_other", "vtl_step_other_reason",
               "vtl_step_code", "vtl_step_at"]
    off = gdb_offsets(elf, members)

    kinds, capacity, code_size = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_count / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_rip[0] / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_code[0][0]"])

    reader = Monitor(args.rig, args.port)
    for member in ("vtl_step_count", "vtl_step_other",
                   "vtl_step_other_reason", "vtl_step_at"):
        reader.queue(instance + off[member], kinds)
    reader.queue(instance + off["vtl_step_rip"], kinds * capacity)
    reader.queue(instance + off["vtl_step_cr3"], kinds * capacity)
    reader.queue(instance + off["vtl_step_code"],
                 kinds * capacity * code_size // 8)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    if not any(word("vtl_step_count", k) for k in range(kinds)):
        return

    armed = ["after HvCallVtlCall (expected VTL1)",
             "after HvCallVtlReturn (expected VTL0)",
             "free-running, on an ordinary second-level entry"]

    for k in range(kinds):
        count = word("vtl_step_count", k)
        if not count:
            continue

        # The arming count against the switch total is what says
        # whether this is a trace of the loop *now* or of the boot -
        # the same two hypercalls carry both.
        print(f"\n--- instruction trace {armed[k]}: {count} steps, "
              f"armed at switch {word('vtl_step_at', k):,}, "
              f"{word('vtl_step_other', k)} other exits "
              f"(first reason 0x{word('vtl_step_other_reason', k):x}) ---")

        # The bytes go on the step's own line rather than in a table
        # beside it, so `disassemble-trace.py` needs no join and a
        # reader with neither script can still see what ran.
        #
        # Runs rather than lines: a loop of three addresses spun a
        # thousand times is three lines and a count, and the count is
        # the finding.
        def step(i):
            base = (k * capacity + i) * code_size // 8
            raw = b"".join(word("vtl_step_code", base + j)
                           .to_bytes(8, "little")
                           for j in range(code_size // 8))
            return (word("vtl_step_rip", k * capacity + i),
                    word("vtl_step_cr3", k * capacity + i),
                    raw)

        def show(entry, repeats):
            rip, cr3, raw = entry
            print(f"    0x{rip:016x}  cr3 0x{cr3:<9x} {raw.hex()}"
                  + (f"  x{repeats}" if repeats > 1 else ""))

        previous, repeats = None, 0
        for i in range(count):
            entry = step(i)
            if entry == previous:
                repeats += 1
                continue
            if previous is not None:
                show(previous, repeats)
            previous, repeats = entry, 1
        if previous is not None:
            show(previous, repeats)


# EFI_GRAPHICS_PIXEL_FORMAT, by number.  Format 3 has no linear
# framebuffer at all - the firmware offers only Blt() - so its base is
# not an address anything can read, and saying so is the whole reason
# the number is carried rather than normalised away by the loader.
PIXEL_FORMAT = {0: "RGBX (red first)", 1: "BGRX (blue first)",
                2: "bit mask", 3: "blt only - NO linear framebuffer"}


def dump_framebuffer(args, elf, instance):
    """Where the firmware's linear framebuffer is, as the loader found it.

    Nothing in the VMM reads these members; they exist to be read from
    out here.  On a rig whose display adapter is passed through the
    emulator has no console and answers `screendump` with "There is no
    console to take a screendump from", so this is what makes the boot
    spinner and a pre-driver bugcheck screen observable at all - see
    `scripts/rig-screen.py`, which takes these six numbers.

    Optional offsets, because a *deployed* binary may predate the
    members: a dump of an older one has to lose this section rather than
    the whole dump.
    """
    members = ["framebuffer_base", "framebuffer_size", "framebuffer_width",
               "framebuffer_height", "framebuffer_stride",
               "framebuffer_format", "framebuffer_red_mask",
               "framebuffer_green_mask", "framebuffer_blue_mask",
               "framebuffer_reserved_mask"]
    off = gdb_offsets(elf, members, optional=True)
    if "framebuffer_base" not in off:
        return

    # The monitor reads 8-byte words and six of these members are 32 bits,
    # so two of them share a word.  Reading each at its own **aligned**
    # address and picking the half by `offset & 4` is what keeps that from
    # being a layout assumption: it holds however the compiler chooses to
    # pack them, and it fails loudly (a missing offset) rather than
    # quietly if a member is ever removed.
    reader = Monitor(args.rig, args.port)
    for name in members:
        reader.queue(instance + (off[name] & ~7), 1)
    words = reader.run()

    def value(name, bits):
        word = words.get(instance + (off[name] & ~7))
        if word is None:
            return None
        if 64 == bits:
            return word
        return (word >> (32 if (off[name] & 4) else 0)) & 0xffffffff

    base = value("framebuffer_base", 64)
    if base is None:
        print("\nframebuffer: NOT READ")
        return

    size = value("framebuffer_size", 64)
    width = value("framebuffer_width", 32)
    height = value("framebuffer_height", 32)
    stride = value("framebuffer_stride", 32)
    fmt = value("framebuffer_format", 32)

    print("\nframebuffer (the firmware's, from the graphics output "
          "protocol)")
    if not base:
        # Not an error and not a bug.  The loader records zeros when the
        # firmware offers no graphics output, and it must never fail a
        # boot over one.
        print("  none - the loader found no graphics output protocol")
        return

    print(f"  base   0x{base:x}  size 0x{size:x} ({size // 1024:,} KiB)")
    print(f"  {width} x {height}, stride {stride} pixels, "
          f"format {fmt} ({PIXEL_FORMAT.get(fmt, '?')})")
    if 2 == fmt:
        print(f"  masks  red 0x{value('framebuffer_red_mask', 32):08x} "
              f"green 0x{value('framebuffer_green_mask', 32):08x} "
              f"blue 0x{value('framebuffer_blue_mask', 32):08x} "
              f"reserved "
              f"0x{value('framebuffer_reserved_mask', 32):08x}")

    # The arithmetic worth stating rather than leaving to be redone: a
    # stride that is not the width is the field a reader gets wrong, and
    # an image walked at the visible width shears diagonally.
    if stride and width and stride != width:
        print(f"  NOTE stride {stride} != width {width} - walk rows at "
              f"the stride")
    if size and stride and height and size < stride * height * 4:
        print(f"  NOTE size 0x{size:x} is smaller than "
              f"stride*height*4 = 0x{stride * height * 4:x}")

    print(f"  read it with: scripts/rig-screen.py --base 0x{base:x} "
          f"--width {width} --height {height} --stride {stride} "
          f"--format {fmt}")


def main():
    ap = argparse.ArgumentParser()
    # The archived copy first, because it is the binary the guest is
    # running; out/ is whatever was built most recently, which during an
    # investigation is routinely a different shape. See deploy-to-rig.sh.
    ap.add_argument("--elf",
                    default=(".rig-deployed-hypervisor.elf"
                             if os.path.exists(".rig-deployed-hypervisor.elf")
                             else "out/debug/x86_64/zpp_hypervisor"))
    ap.add_argument("--rig", default="tc@192.168.1.199")
    ap.add_argument("--port", default="4446")
    ap.add_argument("--cpus", type=int, default=8)
    ap.add_argument("--base", default=None,
                    help="module base; read from serial when omitted")
    ap.add_argument("--l2", type=int, default=None,
                    help="also dump this processor's second-level ring")
    ap.add_argument("--log", type=int, nargs="?", const=4096, default=None,
                    metavar="N",
                    help="also dump the hypervisor's log ring, oldest "
                         "first (default all 4096 lines)")
    ap.add_argument("--l2-entries", type=int, default=24,
                    help="how many second-level entries to show")
    args = ap.parse_args()

    base = args.base
    if base is None:
        out = subprocess.run(
            SSH + [args.rig,
                   'grep -ah "allocate_rwx done at" /home/tc/zpp/serial.out '
                   '2>/dev/null | tail -1'],
            capture_output=True, text=True).stdout.strip()
        m = re.search(r"done at (0x[0-9a-f]+)", out)
        if not m:
            sys.exit("no module base on serial - did the loader run?")
        base = m.group(1)
    base = int(base, 16)

    members = ["cpl_seen", "guest_leaf_permissions",
               "vtl_protect_rcx", "vtl_protect_rdx",
               "vtl_protect_rax", "vtl_protect_count",
               "vtl_call_rcx",
               "shadow_leaf_permissions", "exit_trace", "exit_trace_count", "l2_exit_trace",
               "l2_exit_trace_count", "l2_working_trace",
               "l2_working_trace_count", "l2_entries", "l2_activity_state",
               "running_l2", "events_requeued", "events_deferred",
               "pending_event", "unhandled_exit", "vm_entry_failure",
               "exit_reason_counts",
               "shadow_ept_builds", "shadow_ept_cache_hits",
               "shadow_ept_rebuild_new_root", "shadow_ept_rebuild_stale",
               "shadow_ept_generation_discards",
               "l2_invept_single_context", "l2_invept_all_context",
               "shadow_ept_replayed", "hyperv_vp_assist_writes", "hypercalls_seen",
               "host_exception", "host_exception_cr2",
               "cpuid_trace", "cpuid_trace_count", "host_page_table",
               "cpuid_hypervisor_leaves_asked",
               "hypercall_codes", "hypercall_code_counts",
               "msr_write_codes", "msr_write_counts",
               "msr_write_last_value",
               "l2_msr_write_codes", "l2_msr_write_counts",
               "l2_msr_write_last_value", "l2_msr_write_reflected",
               "msr_write_uncounted", "msr_write_uncounted_code",
               "evmcs_reads", "evmcs_writes", "evmcs_recommended",
               "hot_state_writes_skipped", "hot_state_writes_done",
               "l2_run_cycles", "l1_run_cycles", "handler_cycles",
               "handler_first_tsc", "handler_last_tsc",
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_reclaims", "guest_nmis_reinjected",
               "pending_event_lost", "pending_event_lost_first",
               "pending_event_lost_last", "pending_event_lost_reason",
               "l2_simp_msr", "l2_siefp_msr",
               "shadow_ept_leaves_filled",
               # How each second-level fault was answered. Without this
               # the only visible fact is that faults arrive, and a fault
               # that installs nothing looks exactly like one that
               # installs something - which is the case that livelocks.
               # `leaves_filled` frozen while the fault count climbs says
               # some branch other than `installed` is taking them, and
               # only this array says which.
               "l2_ept_dispositions",
               "shadow_ept_leaves_that_did_not_help",
               "shadow_ept_recall_root", "shadow_ept_current_slot",
               "vtl_call_rdx", "vtl_call_block", "vtl_call_block_read",
               "vtl_call_block_physical", "vtl_call_vtpr",
               "vtl_call_gap_buckets", "vtl1_entry_vector",
               "vina_gs_base", "vina_block", "vina_flags", "vina_read",
               "vina_set_count", "vina_clear_count", "vina_block_physical",
               "vina_at_call_set", "vina_at_call_clear",
               "vina_at_call_unread", "vtl1_duration", "vtl_call_request",
               "vtl_block_changes", "vtl_code0_param_changes", "vtl_code0_ring",
               "vtl_code0_count", "vtl_code0_min_pfn", "vtl_code0_max_pfn",
               "vtl_code0_consecutive", "vtl_code0_pfn_calls",
               "vtl_protect_failures", "vtl_protect_last_failure",
               "vtl_protect_reps_short", "vtl_protect_reps_asked",
               "vtl_protect_reps_done", "vtl_protect_last_rip",
               "vtl_protect_last_cr3", "vtl_protect_last_caller",
               "vtl_protect_last_rsp", "vtl_protect_last_stack",
               "vtl_protect_after_stack", "vtl_protect_after_read",
               "vtl_protect_early_before", "vtl_protect_early_after",
               "vtl_protect_last_r15", "vtl_protect_answer_to_caller",
               "vtl_protect_answer_to_other",
               "vtl_protect_answer_last_cr3", "vtl_protect_answer_rip",
               "vtl_protect_answer_rip_count",
               "vtl_protect_answer_rip_other", "vtl_protect_step_rip",
               "vtl_protect_step_count", "vtl_protect_step_other",
               "vtl_protect_next_reason", "vtl_protect_next_last",
               "vtl_protect_next_code", "vtl_protect_next_code_last",
               "vtl_protect_pfn_status", "vtl_protect_pfn_perms",
               "vtl_protect_pfn_probed", "vtl_protect_pfn_abnormal",
               "vtl_protect_pfn_perm_seen", "vtl_protect_readonly_pfn",
               "vtl_protect_readonly_count", "vtl_protect_host_perms",
               "vtl_protect_host_status", "vtl_protect_thread",
               "vtl_protect_thread_flags", "vtl_protect_thread_read",
               "vtl_protect_thread_locked", "vtl_protect_thread_clear",
               "vmcs_shadow_loads", "vmcs_shadow_stores",
               "vmcs_field_read_encoding", "vmcs_field_read_count",
               "vmcs_field_write_encoding", "vmcs_field_write_count",
               "vmcs_field_use_overflow",
               "external_interrupt_vector_counts",
               "l2_injected_vector", "l2_external_vector",
               "phase_cycles", "phase_calls",
               "guest_state_writes_skipped", "guest_state_writes_done",
               "control_writes_skipped", "control_writes_done",
               # The refusal itself. `scripts/zpp.gdb` has printed these
               # for a dozen sessions and this reader never did, so the
               # monitor path - the one that works on a wedged guest -
               # could not see the number that names a failed entry.
               "nested_vmfail_count", "nested_last_vmfail",
               # Whether GS is telling the truth about which processor it
               # is on. Two scalars, not per-processor arrays, so they are
               # read with their own queue below rather than with the
               # per-processor run.
               "gs_processor_index_disagreements",
               "gs_processor_index_checked",
               # The synthetic timer's arm-to-fire interval. This decides
               # whether the guest's clock handler can finish inside its
               # own period, and nothing else in this reader shows it.
               "stimer_given_cycles", "stimer_given_arms",
               "stimer_arm_count", "stimer_arm_value", "stimer_arm_tsc",
               "stimer_arm_kind",
               # Where the second-level guest's hot instruction lives, so
               # the bytes can be read. See `profile_code_physical`.
               "profile_code_physical", "profile_code_virtual",
               # The call stacks. Sampled for sessions and printed by
               # nothing, which is why "what is the guest waiting on" has
               # been answered from exit histograms every time.
               "guest_stack_trace", "guest_stack_count",
               "guest_stack_pointer", "guest_stack_rip",
               "guest_kernel_base", "guest_kernel_size", "l2_exit_cr3",
               "l1_own_cr3",
               "synthetic_msr_writes", "synthetic_msr_last_value",
               "interrupted_rip", "interrupted_hits",
               "interrupted_samples", "interrupted_overflow",
               "stall_withheld_total", "stall_forced_total",
               "window_deferred_count", "window_granted_on_drop",
               "stall_restaged_total", "stall_restage_blocked",
               "quiet_rip", "quiet_hits",
               "quiet_samples", "quiet_overflow",
               "guest_interrupted_trace", "guest_interrupted_count",
               "guest_interrupted_rsp", "guest_interrupted_rip",
               # Which thread the guest is running. Sampled for sessions
               # and printed by nothing, and it is the only progress
               # metric here that a livelock cannot fake.
               "guest_thread_samples", "guest_thread_sample_count",
               # Which hypercalls each level makes. Recorded for sessions
               # and printed by nothing, and the second-level one names
               # what Windows is asking Hyper-V to do.
               "hypercall_codes", "hypercall_code_counts",
               "msr_write_codes", "msr_write_counts",
               "msr_write_last_value",
               "l2_msr_write_codes", "l2_msr_write_counts",
               "l2_msr_write_last_value", "l2_msr_write_reflected",
               "msr_write_uncounted", "msr_write_uncounted_code",
               "l2_hypercall_codes", "l2_hypercall_code_counts",
               # What the guest hypervisor asked vmcs02 for against what
               # it was given. A bit it asked for and did not get changes
               # how its guest's APIC behaves.
               "control_secondary_requested", "control_secondary_granted",
               # The IUM block memory breakpoint. See
               # nested_vmx::watch_vtl_block.
               "capability_answers", "nested_capability_reads",
               # VMFUNC, which is how a guest hypervisor switches extended
               # page tables. A refusal injects #UD into its guest.
               "l2_vmfunc_calls", "l2_vmfunc_refused",
               "vtl_block_page", "vtl_block_writes",
               "vtl_block_writer_rip", "vtl_block_write_address",
               "vtl_block_write_value"]
    off = gdb_offsets(args.elf, members)
    instance = base + gdb_symbol(
        args.elf, "zpp::hypervisor::hypervisor::instance()::instance")
    # Derived from the type rather than carried here, for the reason
    # gdb_lengths gives at length: a constant copied out of the header
    # does not fail when the header changes, it reads the ring at the
    # wrong stride and reports plausible nonsense. The record gained a
    # field the day this comment was written.
    entry_size = int(subprocess.run(
        ["x86_64-elf-gdb", "-q", "-batch", args.elf, "-ex",
         "print (int)sizeof(('zpp::hypervisor::hypervisor' *)0)"
         "->exit_trace[0][0]"],
        capture_output=True, text=True).stdout.split("=")[-1].strip())
    lengths = gdb_lengths(args.elf, ["exit_trace", "l2_exit_trace",
                                     "l2_working_trace",
                                     "exit_reason_counts"])
    ring = lengths["exit_trace"]
    l2ring = lengths["l2_exit_trace"]
    working_ring = lengths["l2_working_trace"]
    reason_capacity = lengths["exit_reason_counts"]

    # A processor named by --l2 must have its scalars read even when it is
    # outside --cpus, or `l2_exit_trace_count` comes back as None and the
    # dump dies in arithmetic rather than saying what it wanted.
    scalar_cpus = max(args.cpus, 0 if args.l2 is None else args.l2 + 1)

    print(f"module base 0x{base:x}, singleton 0x{instance:x}")

    monitor = Monitor(args.rig, args.port)
    # The scalar per-processor arrays, one read each - they are contiguous.
    scalars = ["exit_trace_count", "l2_exit_trace_count",
               "l2_working_trace_count", "l2_entries",
               "l2_activity_state", "events_requeued", "events_deferred",
               "pending_event", "shadow_ept_builds", "shadow_ept_cache_hits",
               "shadow_ept_rebuild_new_root", "shadow_ept_rebuild_stale",
               "shadow_ept_generation_discards",
               "l2_invept_single_context", "l2_invept_all_context",
               "shadow_ept_replayed", "hyperv_vp_assist_writes", "hypercalls_seen",
               "host_exception", "host_exception_cr2",
               "cpuid_trace", "cpuid_trace_count", "host_page_table",
               "cpuid_hypervisor_leaves_asked",
               "evmcs_reads", "evmcs_writes", "evmcs_recommended",
               "hot_state_writes_skipped", "hot_state_writes_done",
               "l2_run_cycles", "l1_run_cycles", "handler_cycles",
               "handler_first_tsc", "handler_last_tsc",
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_leaves_filled",
               "l2_ept_dispositions",
               "shadow_ept_leaves_that_did_not_help",
               "shadow_ept_recall_root", "shadow_ept_current_slot",
               "vtl_call_rdx", "vtl_call_block", "vtl_call_block_read",
               "vtl_call_block_physical", "vtl_call_vtpr",
               "vtl_call_gap_buckets", "vtl1_entry_vector",
               "vina_gs_base", "vina_block", "vina_flags", "vina_read",
               "vina_set_count", "vina_clear_count", "vina_block_physical",
               "vina_at_call_set", "vina_at_call_clear",
               "vina_at_call_unread", "vtl1_duration", "vtl_call_request",
               "vtl_block_changes", "vtl_code0_param_changes", "vtl_code0_ring",
               "vtl_code0_count", "vtl_code0_min_pfn", "vtl_code0_max_pfn",
               "vtl_code0_consecutive", "vtl_code0_pfn_calls",
               "vtl_protect_failures", "vtl_protect_last_failure",
               "vtl_protect_reps_short", "vtl_protect_reps_asked",
               "vtl_protect_reps_done", "vtl_protect_last_rip",
               "vtl_protect_last_cr3", "vtl_protect_last_caller",
               "vtl_protect_last_rsp", "vtl_protect_last_stack",
               "vtl_protect_after_stack", "vtl_protect_after_read",
               "vtl_protect_early_before", "vtl_protect_early_after",
               "vtl_protect_last_r15", "vtl_protect_answer_to_caller",
               "vtl_protect_answer_to_other",
               "vtl_protect_answer_last_cr3", "vtl_protect_answer_rip",
               "vtl_protect_answer_rip_count",
               "vtl_protect_answer_rip_other", "vtl_protect_step_rip",
               "vtl_protect_step_count", "vtl_protect_step_other",
               "vtl_protect_next_reason", "vtl_protect_next_last",
               "vtl_protect_next_code", "vtl_protect_next_code_last",
               "vtl_protect_pfn_status", "vtl_protect_pfn_perms",
               "vtl_protect_pfn_probed", "vtl_protect_pfn_abnormal",
               "vtl_protect_pfn_perm_seen", "vtl_protect_readonly_pfn",
               "vtl_protect_readonly_count", "vtl_protect_host_perms",
               "vtl_protect_host_status", "vtl_protect_thread",
               "vtl_protect_thread_flags", "vtl_protect_thread_read",
               "vtl_protect_thread_locked", "vtl_protect_thread_clear",
               "vmcs_shadow_loads",
               "vmcs_shadow_stores",
               "guest_state_writes_skipped", "guest_state_writes_done",
               "control_writes_skipped", "control_writes_done",
               # The refusal itself. `scripts/zpp.gdb` has printed these
               # for a dozen sessions and this reader never did, so the
               # monitor path - the one that works on a wedged guest -
               # could not see the number that names a failed entry.
               "nested_vmfail_count", "nested_last_vmfail"]
    for name in scalars:
        monitor.queue(instance + off[name], scalar_cpus)
    # The phase rows are [cpu][phase_count], so each processor's row has
    # to be queued separately rather than as one run of scalars.
    phase_count = gdb_lengths(args.elf, ["phase_cycles"])["phase_cycles"]
    for cpu in range(args.cpus):
        monitor.queue(instance + off["phase_cycles"]
                      + cpu * phase_count * 8, phase_count)
        monitor.queue(instance + off["phase_calls"]
                      + cpu * phase_count * 8, phase_count)
    # Arrays that are not per-processor have to be queued with **their
    # own length**, not with the processor count. Queued as scalars they
    # fetch eight words and the rest resolves against whatever the next
    # queue covers - which is how sixteen hypercall slots read back as
    # eight populated ones with the same values from a different binary
    # and fresh guest memory. The same failure gdb_lengths was written
    # for: a capacity carried in the reader is a second copy of a
    # constant that lives in the header.
    # 512 entries of four 32-bit words - two words each - queued at its
    # own length for the reason the entry above records.
    cpuid_trace_words = 512 * 2
    monitor.queue(instance + off["cpuid_trace"], cpuid_trace_words)

    # Resolved for its offset is not the same as fetched. This was
    # registered and not queued, so it read as absent, the loop below saw
    # zero entries and printed nothing - and silence from an instrument
    # is exactly what this file keeps warning is not a measurement.
    # Seven words - vector, error code, rip, cs, rflags, rsp, ss - queued
    # at its own length. The rig notes say to read this first for a
    # failure before the guest gets going, and it was never read.
    monitor.queue(instance + off["host_exception"], 7)
    monitor.queue(instance + off["host_exception_cr2"], 1)

    # Queued at their own length, which is the difference between a
    # reading and a plausible lie. A member resolved for its offset and
    # not queued reads as absent; one queued short reads as zero past the
    # end. Both look like data. cpl_seen is [max_cpus][4] and the two
    # permission histograms are [max_cpus][8], so each needs the whole
    # array, not one word.
    monitor.queue(instance + off["cpl_seen"], scalar_cpus * 4)
    for name in ("vtl_protect_rcx", "vtl_protect_rdx", "vtl_protect_rax"):
        monitor.queue(instance + off[name], scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_count"], scalar_cpus)
    for _n in ("vtl_protect_failures", "vtl_protect_last_failure",
               "vtl_protect_reps_short", "vtl_protect_reps_asked",
               "vtl_protect_reps_done", "vtl_protect_last_rip",
               "vtl_protect_last_cr3", "vtl_protect_last_caller",
               "vtl_protect_last_rsp"):
        monitor.queue(instance + off[_n], scalar_cpus)
    monitor.queue(instance + off["vtl_protect_last_stack"],
                  scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_after_stack"],
                  scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_after_read"], scalar_cpus)
    monitor.queue(instance + off["vtl_protect_early_before"], scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_early_after"], scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_last_r15"], scalar_cpus)
    for _n in ("vtl_protect_answer_to_caller",
               "vtl_protect_answer_to_other",
               "vtl_protect_answer_last_cr3",
               "vtl_protect_answer_rip_other"):
        monitor.queue(instance + off[_n], scalar_cpus)
    for _n in ("vtl_protect_answer_rip", "vtl_protect_answer_rip_count",
               "vtl_protect_step_rip", "vtl_protect_step_count"):
        monitor.queue(instance + off[_n], scalar_cpus * 8)
    monitor.queue(instance + off["vtl_protect_step_other"], scalar_cpus)
    monitor.queue(instance + off["vtl_protect_next_reason"], scalar_cpus * 72)
    monitor.queue(instance + off["vtl_protect_next_last"], scalar_cpus)
    monitor.queue(instance + off["vtl_protect_next_code"], scalar_cpus * 32)
    monitor.queue(instance + off["vtl_protect_next_code_last"], scalar_cpus)
    for _n in ("vtl_protect_pfn_status", "vtl_protect_pfn_perms",
               "vtl_protect_pfn_probed", "vtl_protect_pfn_abnormal"):
        monitor.queue(instance + off[_n], scalar_cpus)
    monitor.queue(instance + off["vtl_protect_pfn_perm_seen"],
                  scalar_cpus * 8)
    monitor.queue(instance + off["vtl_protect_readonly_pfn"],
                  scalar_cpus * 8)
    monitor.queue(instance + off["vtl_protect_readonly_count"], scalar_cpus)
    for _n in ("vtl_protect_host_perms", "vtl_protect_host_status"):
        monitor.queue(instance + off[_n], scalar_cpus * 8)
    for _n in ("vtl_protect_thread", "vtl_protect_thread_flags",
               "vtl_protect_thread_read", "vtl_protect_thread_locked",
               "vtl_protect_thread_clear"):
        monitor.queue(instance + off[_n], scalar_cpus)
    for _n in ():
        monitor.queue(instance + off[_n], scalar_cpus)
    for _n in ():
        monitor.queue(instance + off[_n], scalar_cpus)
    monitor.queue(instance + off["vtl_call_rcx"], scalar_cpus)
    monitor.queue(instance + off["guest_leaf_permissions"], scalar_cpus * 8)
    monitor.queue(instance + off["shadow_leaf_permissions"],
                  scalar_cpus * 8)

    # The guest hypervisor's EPT roots this processor holds shadows for.
    # Four slots. **VSM gives each trust level its own extended page
    # tables** - that is the mechanism HvCallModifyVtlProtectionMask acts
    # through, and it is how VTL0 is denied the pages VTL1 owns. So a
    # single distinct root across every slot would mean the two levels are
    # sharing a view they must not share, and securekernel refusing to
    # proceed would be correct rather than mysterious.
    monitor.queue(instance + off["shadow_ept_recall_root"],
                  scalar_cpus * 4)
    monitor.queue(instance + off["shadow_ept_current_slot"], scalar_cpus)
    monitor.queue(instance + off["guest_nmis_reinjected"], 1)
    for _m in ("pending_event_lost", "pending_event_lost_first",
               "pending_event_lost_last", "pending_event_lost_reason"):
        if _m in off:
            monitor.queue(instance + off[_m], scalar_cpus)
    monitor.queue(instance + off["l2_simp_msr"], scalar_cpus)
    monitor.queue(instance + off["l2_siefp_msr"], scalar_cpus)

    # The IUM secure-call block. See hypervisor.h `vtl_call_block`.
    monitor.queue(instance + off["vtl_call_rdx"], scalar_cpus)
    monitor.queue(instance + off["vtl_call_block"], scalar_cpus * 4)
    monitor.queue(instance + off["vtl_call_block_read"], scalar_cpus)
    monitor.queue(instance + off["vtl_call_block_physical"], scalar_cpus)
    monitor.queue(instance + off["vtl_call_vtpr"], scalar_cpus * 16)
    monitor.queue(instance + off["vtl_call_gap_buckets"], scalar_cpus * 40)
    monitor.queue(instance + off["vtl1_entry_vector"], scalar_cpus * 257)
    monitor.queue(instance + off["vtl1_duration"], scalar_cpus * 2 * 24)
    monitor.queue(instance + off["vtl_call_request"], scalar_cpus * 256)
    monitor.queue(instance + off["vtl_block_changes"], scalar_cpus)
    monitor.queue(instance + off["vtl_code0_param_changes"], scalar_cpus)
    monitor.queue(instance + off["vtl_code0_count"], scalar_cpus)
    for _n in ("vtl_code0_min_pfn", "vtl_code0_max_pfn",
               "vtl_code0_consecutive", "vtl_code0_pfn_calls"):
        monitor.queue(instance + off[_n], scalar_cpus)
    monitor.queue(instance + off["vtl_code0_ring"], scalar_cpus * 8 * 3)
    for _n in ("vina_gs_base", "vina_block", "vina_flags", "vina_read",
               "vina_set_count", "vina_clear_count",
               "vina_block_physical", "vina_at_call_set",
               "vina_at_call_clear", "vina_at_call_unread"):
        monitor.queue(instance + off[_n], scalar_cpus)

    # Ten dispositions per processor - `none` through `pointer_failed`.
    monitor.queue(instance + off["l2_ept_dispositions"], scalar_cpus * 10)
    monitor.queue(instance + off["shadow_ept_leaves_that_did_not_help"],
                  scalar_cpus)

    monitor.queue(instance + off["cpuid_trace_count"], 1)
    monitor.queue(instance + off["cpuid_hypervisor_leaves_asked"], 1)
    monitor.queue(instance + off["host_page_table"], 1)

    hypercall_slots = 16
    for _name in ("msr_write_codes", "msr_write_counts",
                  "msr_write_last_value", "l2_msr_write_codes",
                  "l2_msr_write_counts", "l2_msr_write_last_value",
                  "l2_msr_write_reflected"):
        if _name in off:
            monitor.queue(instance + off[_name], 96)

    for _name in ("msr_write_uncounted", "msr_write_uncounted_code"):
        if _name in off:
            monitor.queue(instance + off[_name], 1)

    monitor.queue(instance + off["hypercall_codes"], hypercall_slots)
    monitor.queue(instance + off["hypercall_code_counts"], hypercall_slots)

    monitor.queue(instance + off["running_l2"], (scalar_cpus + 7) // 8)
    monitor.queue(instance + off["unhandled_exit"], 6)
    monitor.queue(instance + off["vm_entry_failure"], 6)
    for cpu in range(args.cpus):
        monitor.queue(instance + off["exit_trace"] + cpu * ring * entry_size,
                      ring * entry_size // 8)
    # The synthetic timer, which is what decides whether the guest's clock
    # handler can finish inside its own period. Queued here rather than in
    # a section of its own because every read has to precede monitor.run().
    stimer_ring = 32
    for name in ("stimer_given_cycles", "stimer_given_arms",
                 "stimer_arm_count"):
        if name in off:
            monitor.queue(instance + off[name], args.cpus)
    for name in ("stimer_arm_value", "stimer_arm_tsc", "stimer_arm_kind"):
        if name in off:
            monitor.queue(instance + off[name], args.cpus * stimer_ring)
    # Two scalars, not per-processor arrays - the profiler is boot
    # processor only, which is why these are queued with a count of one.
    for name in ("profile_code_physical", "profile_code_virtual",
                 "guest_stack_count", "guest_stack_pointer",
                 "guest_stack_rip", "guest_kernel_base", "guest_kernel_size", "l2_exit_cr3",
               "interrupted_rip", "interrupted_hits",
               "interrupted_samples", "interrupted_overflow",
               "quiet_rip", "quiet_hits",
               "quiet_samples", "quiet_overflow",
                 "guest_interrupted_count", "guest_interrupted_rsp",
                 "guest_interrupted_rip"):
        if name in off:
            monitor.queue(instance + off[name], 1)
    stack_capacity = 48
    for name in ("guest_stack_trace", "guest_interrupted_trace"):
        if name in off:
            monitor.queue(instance + off[name], stack_capacity)
    # guest_thread_sample is eight 64-bit fields; 32 of them per processor.
    thread_fields, thread_capacity = 8, 32
    for name in ("control_secondary_requested", "control_secondary_granted"):
        if name in off:
            monitor.queue(instance + off[name], args.cpus)
    for name in ("l2_vmfunc_calls", "l2_vmfunc_refused"):
        if name in off:
            monitor.queue(instance + off[name], args.cpus)
    if "capability_answers" in off:
        monitor.queue(instance + off["capability_answers"], 48 * 2)
        monitor.queue(instance + off["nested_capability_reads"], 1)
    for name in ("vtl_block_page", "vtl_block_writes", "vtl_block_writer_rip",
                 "vtl_block_write_address", "vtl_block_write_value"):
        if name in off:
            monitor.queue(instance + off[name], 1)
    for name in ("hypercall_codes", "hypercall_code_counts",
                 "l2_hypercall_codes", "l2_hypercall_code_counts"):
        if name in off:
            monitor.queue(instance + off[name], 16)
    if "guest_thread_samples" in off:
        monitor.queue(instance + off["guest_thread_samples"],
                      args.cpus * thread_capacity * thread_fields)
        monitor.queue(instance + off["guest_thread_sample_count"], args.cpus)

    words = monitor.run()

    def read(name, index=0):
        return words.get(instance + off[name] + 8 * index)

    print("\ncpu  exits      l2-entries  l2-exits  requeued  deferred  "
          "pending  l2-activity")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('exit_trace_count', cpu):-10d}  "
              f"{read('l2_entries', cpu):-10d}  "
              f"{read('l2_exit_trace_count', cpu):-8d}  "
              f"{read('events_requeued', cpu):-8d}  "
              f"{read('events_deferred', cpu):-8d}  "
              f"0x{read('pending_event', cpu):-6x}  "
              f"{ACTIVITY.get(read('l2_activity_state', cpu), '?')}")

    # What the guest asked its synthetic timer for, and what it was given.
    #
    # The pair is the point. `stimer_given_cycles / stimer_given_arms` is
    # the measured arm-to-fire interval; the guest's own constant
    # `KeQuantumEndTimerIncrement` is 17,400 units of 100 ns, so 1.74 ms is
    # what it believes it asked for. **A handler that costs more than the
    # interval it is given can never return**, and the guest then never
    # lowers IRQL far enough to take the DPC interrupt it keeps requesting -
    # which is exactly the 0x2f-at-task-priority-0xd0 census below.
    #
    # Read both, never one: the interval alone cannot distinguish "the
    # guest asked for a short period" from "the guest asked for 1.74 ms and
    # the level above expired it early", and those need different fixes.
    # The `kind` column is what separates them - 1 is the guest writing a
    # count, 3 is the clock vector actually going in.
    # Secondary controls: what the guest hypervisor asked vmcs02 for
    # against what it was granted.
    #
    # **A bit asked for and not granted changes how the second-level
    # guest's APIC behaves**, and the three APIC-virtualization controls
    # are exactly the ones that decide where its INIT and start-up IPIs
    # go. If those are missing, a processor the guest tries to start
    # never hears about it.
    SECONDARY = {
        0: "virtualize_apic_accesses", 1: "enable_ept",
        3: "enable_rdtscp", 5: "enable_vpid", 7: "unrestricted_guest",
        8: "apic_register_virtualization", 9: "virtual_interrupt_delivery",
        12: "enable_invpcid", 14: "vmcs_shadowing",
        18: "conceal_vmx_from_pt", 20: "enable_xsaves",
        22: "mode_based_execute_control",
    }
    if "control_secondary_requested" in off:
        for cpu in range(args.cpus):
            asked = read("control_secondary_requested", cpu) or 0
            got = read("control_secondary_granted", cpu) or 0
            if not asked and not got:
                continue
            missing = asked & ~got
            print(f"\ncpu {cpu} vmcs02 secondary controls: "
                  f"asked 0x{asked:x}, granted 0x{got:x}"
                  f"{'  <- ALL GRANTED' if not missing else ''}")
            if missing:
                for bit in range(64):
                    if missing & (1 << bit):
                        print(f"    NOT GRANTED bit {bit}  "
                              f"{SECONDARY.get(bit, '')}")
            for bit in (0, 8, 9):
                state = "yes" if got & (1 << bit) else "no"
                print(f"    {SECONDARY[bit]:<30} {state}")

    # VMFUNC: extended-page-table pointer switching, which is how a
    # guest hypervisor moves its guest between page-table sets.
    #
    # **A refusal is not silent - it injects #UD** - so refused > 0 means
    # the guest hypervisor asked for a switch and got an invalid-opcode
    # fault instead. Printed because a counter that only matters when
    # non-zero is exactly the kind that goes unread until it is too late.
    if "l2_vmfunc_calls" in off:
        calls = sum((read("l2_vmfunc_calls", c) or 0)
                    for c in range(args.cpus))
        refused = sum((read("l2_vmfunc_refused", c) or 0)
                      for c in range(args.cpus))
        if calls or refused:
            note = "  <- REFUSED, #UD injected" if refused else ""
            print(f"\ncpu* VMFUNC: {calls:,} calls, {refused:,} refused{note}")

    # What this VMM told the guest hypervisor about VMX, which is a set
    # of values we genuinely originate - unlike the hypercall answers,
    # which Hyper-V produces and we only carry.
    #
    # A capability narrowed away here is a thing the guest hypervisor
    # will not attempt. That is the point of narrowing - do not promise
    # what the shadow builder cannot honour - but it also means this list
    # is the complete set of ways this VMM can make Hyper-V behave
    # differently from how it would on the metal.
    VMX_MSRS = {
        0x480: "IA32_VMX_BASIC",          0x481: "PINBASED_CTLS",
        0x482: "PROCBASED_CTLS",          0x483: "EXIT_CTLS",
        0x484: "ENTRY_CTLS",              0x485: "MISC",
        0x486: "CR0_FIXED0",              0x487: "CR0_FIXED1",
        0x488: "CR4_FIXED0",              0x489: "CR4_FIXED1",
        0x48a: "VMCS_ENUM",               0x48b: "PROCBASED_CTLS2",
        0x48c: "EPT_VPID_CAP",            0x48d: "TRUE_PINBASED_CTLS",
        0x48e: "TRUE_PROCBASED_CTLS",     0x48f: "TRUE_EXIT_CTLS",
        0x490: "TRUE_ENTRY_CTLS",         0x491: "VMFUNC",
    }
    if "capability_answers" in off:
        n = read("nested_capability_reads") or 0
        if n:
            print(f"\ncpu 0 VMX capabilities answered to the guest "
                  f"hypervisor ({n} reads)")
            seen = {}
            for i in range(min(n, 48)):
                a = instance + off["capability_answers"] + i * 16
                msr = words.get(a, 0)
                val = words.get(a + 8, 0)
                seen[msr] = val
            for msr in sorted(seen):
                print(f"    0x{msr:03x}  {VMX_MSRS.get(msr,''):<22} "
                      f"0x{seen[msr]:016x}")

    # The IUM context block memory breakpoint.
    #
    # **A write that is attempted and lost, and a write that never
    # happens, leave the same bytes in memory.** The second-level guest
    # loops on a state byte that polling shows unchanged; only a watch
    # says which of those is true. `writes 0` with the page armed means
    # nothing writes it - the state is stale by omission, not by loss.
    if "vtl_block_page" in off:
        page = read("vtl_block_page") or 0
        if page:
            n = read("vtl_block_writes") or 0
            print(f"\ncpu 0 IUM block watch: page 0x{page:x}, "
                  f"{n:,} writes seen")
            if n:
                print(f"    last write: address 0x{read('vtl_block_write_address') or 0:x}"
                      f"  value 0x{read('vtl_block_write_value') or 0:x}"
                      f"  from rip 0x{read('vtl_block_writer_rip') or 0:x}")
            else:
                print("    NOTHING writes this page - the state is stale by "
                      "omission, not by a lost write")

    # Which hypercalls each level is making, by code.
    #
    # `HvCallStartVirtualProcessor` and friends are how Windows asks the
    # hypervisor above it to bring up a virtual processor, and nothing in
    # this reader has ever shown them. A code that repeats without the
    # guest moving on is a request that is not completing.
    MSR_NAMES = {
        0x0000001b: "IA32_APIC_BASE",
        0x0000006e0: "IA32_TSC_DEADLINE",
        0x00000830: "X2APIC_ICR",
        0x0000080b: "X2APIC_EOI",
        0x00000838: "X2APIC_INIT_COUNT",
        0x00000808: "X2APIC_TPR",
        0x00000832: "X2APIC_LVT_TIMER",
        0x0000083f: "X2APIC_SELF_IPI",
        0x40000070: "HV_EOI",
        0x40000071: "HV_ICR",
        0x40000072: "HV_TPR",
        0x40000020: "HV_TIME_REF_COUNT",
        0x40000082: "HV_SIEFP",
        0x40000083: "HV_SIMP",
        0x40000084: "HV_EOM",
        0x40000073: "HV_VP_ASSIST_PAGE",
        0x40000080: "HV_SCONTROL",
        0x4000008d: "HV_EOM",
        0x400000b0: "HV_STIMER0_CONFIG",
        0x400000b1: "HV_STIMER0_COUNT",
        0x400000b2: "HV_STIMER1_CONFIG",
        0x400000b3: "HV_STIMER1_COUNT",
        0xc0000080: "IA32_EFER",
        0xc0000101: "GS_BASE",
        0xc0000102: "KERNEL_GS_BASE",
    }
    HV_CALLS = {
        # Corrected against Linux's include/asm-generic/hyperv-tlfs.h.
        # The previous table put SendSyntheticClusterIpi here and
        # Get/SetVpRegisters at 0x005b/0x005c, and both were wrong -
        # 0x0008 is the call a guest makes when it has been spinning too
        # long, which is exactly the signal a livelock investigation
        # wants, and it was being printed under another name.
        0x0008: "HvCallNotifyLongSpinWait",
        0x000b: "HvCallSendSyntheticClusterIpi",
        0x0046: "HvCallGetPartitionId",
        0x0048: "HvCallDepositMemory",
        0x004e: "HvCallCreateVp",
        0x0050: "HvCallGetVpRegisters",
        0x0051: "HvCallSetVpRegisters",
        0x005c: "HvCallPostMessage",
        0x005d: "HvCallSignalEvent",
        0x0099: "HvCallStartVirtualProcessor",
        0x009a: "HvCallGetVpIndexFromApicId",
        0x00af: "HvCallFlushGuestPhysicalAddressSpace",
        0x00b0: "HvCallFlushGuestPhysicalAddressList",
        0x000c: "HvCallModifyVtlProtectionMask",
        0x000d: "HvCallEnablePartitionVtl",
        0x000f: "HvCallEnableVpVtl",
        0x0011: "HvCallVtlCall",
        0x0012: "HvCallVtlReturn",
        0x0013: "HvCallFlushVirtualAddressSpaceEx",
        0x0014: "HvCallFlushVirtualAddressListEx",
        0x0015: "HvCallSendSyntheticClusterIpiEx",
    }
    for label, codes, counts in (
            ("first level (Hyper-V)", "hypercall_codes",
             "hypercall_code_counts"),
            ("SECOND level (Windows)", "l2_hypercall_codes",
             "l2_hypercall_code_counts")):
        if codes not in off:
            continue
        rows = []
        for i in range(16):
            c = words.get(instance + off[codes] + 8 * i, 0)
            n = words.get(instance + off[counts] + 8 * i, 0)
            if n:
                rows.append((n, c))
        if not rows:
            continue
        print(f"\ncpu 0 hypercalls from the {label}")
        for n, c in sorted(rows, reverse=True):
            print(f"    0x{c:04x}  {n:>12,}  "
                  f"{HV_CALLS.get(c, '')}")

    # Which MSRs the wrmsr exits actually are. An exit reason is not an
    # instrument - see `msr_write_codes`. The value is printed beside the
    # count so a deadline being advanced can be told from one rewritten
    # unchanged, which the count alone cannot do.
    for label, pfx in (("every exit, at the trace record", "msr_write"),
                       ("SECOND level, at the reflect decision",
                        "l2_msr_write")):
        if pfx + "_codes" not in off:
            continue
        rows = []
        for i in range(96):
            c = words.get(instance + off[pfx + "_codes"] + 8 * i, 0)
            n = words.get(instance + off[pfx + "_counts"] + 8 * i, 0)
            v = words.get(instance + off[pfx + "_last_value"] + 8 * i, 0)
            r = words.get(instance + off.get(pfx + "_reflected", 0)
                          + 8 * i, 0) if pfx + "_reflected" in off else 0
            if n:
                rows.append((n, c, v, r))
        if not rows:
            print(f"\ncpu 0 wrmsr by MSR ({label}): none censused")
            continue
        total = sum(r[0] for r in rows)
        print(f"\ncpu 0 wrmsr by MSR ({label}, {total:,} censused)")
        for n, c, v, r in sorted(rows, reverse=True)[:12]:
            up = f"  up {r:>10,}" if (pfx + "_reflected") in off else ""
            print(f"    0x{c:08x}  {n:>12,}  {100.0*n/total:5.1f}%{up}  "
                  f"last 0x{v:016x}  {MSR_NAMES.get(c, '')}")
        if len(rows) > 12:
            print(f"    ... and {len(rows) - 12} more MSRs")
        if pfx == "msr_write" and "msr_write_uncounted" in off:
            lost = words.get(instance + off["msr_write_uncounted"], 0)
            code = words.get(instance + off["msr_write_uncounted_code"], 0)
            if lost:
                print(f"    NO SLOT: {lost:,} writes uncounted, "
                      f"one of them MSR 0x{code:08x} - the table "
                      f"saturated and this census is incomplete")

    # Where the guest was when an interrupt landed on it - the only
    # unbiased sample of the guest's own code in this tool. See
    # `interrupted_rip`.
    if "interrupted_rip" in off:
        kbase0 = read("guest_kernel_base") or 0
        ksize0 = read("guest_kernel_size") or 0
        CAP = 2048
        for _p in ("interrupted", "quiet"):
            if _p + "_rip" not in off:
                continue
            for _n in (_p + "_rip", _p + "_hits"):
                monitor.queue(instance + off[_n], CAP)
            for _n in (_p + "_samples", _p + "_overflow"):
                monitor.queue(instance + off[_n], 1)
        words.update(monitor.run())
        for _p, _what in (("interrupted",
                           "when an interrupt landed on it"),
                          ("quiet",
                           "on an entry staging nothing (the control)")):
            if _p + "_rip" not in off:
                continue
            rows = []
            for i in range(CAP):
                r = words.get(instance + off[_p + "_rip"] + 8 * i, 0)
                h = words.get(instance + off[_p + "_hits"] + 8 * i, 0)
                if h:
                    rows.append((h, r))
            tot = words.get(instance + off[_p + "_samples"], 0)
            lost = words.get(instance + off[_p + "_overflow"], 0)
            if not rows:
                continue
            print(f"\ncpu 0 where the guest was {_what} "
                  f"({tot:,} samples, {len(rows)} distinct)")
            for h, r in sorted(rows, reverse=True)[:14]:
                rel = ""
                if kbase0 and kbase0 <= r < kbase0 + (ksize0 or 0):
                    rel = f"  ntoskrnl+0x{r - kbase0:x}"
                print(f"  0x{r:016x}  {h:>10}  "
                      f"{100.0 * h / (tot or 1):5.1f}%{rel}")
            if lost:
                print(f"  contention: {lost:,} colliding samples decayed a "
                      f"resident entry (a rate, not lost hot addresses)")

    # The interface's own crash report. HV_X64_MSR_CRASH_P0..P4 are
    # 0x40000100-0x40000104 and the control is 0x40000105; the guest
    # writes them when it reports a fatal error, and Windows shows
    # HYPERVISOR_ERROR (0x20001) on the screen at the same moment. This
    # is the only place those parameters survive.
    if "synthetic_msr_last_value" in off and "synthetic_msr_writes" in off:
        CAPS = 320
        for _c in range(args.cpus):
            for i in range(0x100, 0x106):
                monitor.queue(instance + off["synthetic_msr_writes"]
                              + (_c * CAPS + i) * 8, 1)
                monitor.queue(instance + off["synthetic_msr_last_value"]
                              + (_c * CAPS + i) * 8, 1)
        words.update(monitor.run())
        for _c in range(args.cpus):
            rows = []
            for i in range(0x100, 0x106):
                n = words.get(instance + off["synthetic_msr_writes"]
                              + (_c * CAPS + i) * 8, 0)
                v = words.get(instance + off["synthetic_msr_last_value"]
                              + (_c * CAPS + i) * 8, 0)
                if n:
                    rows.append((i, n, v))
            if rows:
                print(f"\ncpu {_c} HYPERVISOR CRASH REGISTERS - the "
                      f"interface reported a fatal error")
                for i, n, v in rows:
                    nm = {0x100: "CRASH_P0", 0x101: "CRASH_P1",
                          0x102: "CRASH_P2", 0x103: "CRASH_P3",
                          0x104: "CRASH_P4", 0x105: "CRASH_CTL"}[i]
                    print(f"    0x{0x40000000 + i:08x} {nm:<9} "
                          f"writes {n:>6}  last 0x{v:016x}")

    # Which call sites take the VMCS reads. The field census says *what*
    # is read; this says *who* reads it, which is the only one of the two
    # that can be acted on. Empty unless ZPP_VMCS_CENSUS was on.
    # The kernel image bounds, used by both the thread and stack sections
    # below to turn an address into an offset that survives KASLR.
    kbase = read("guest_kernel_base") or 0
    ksize = read("guest_kernel_size") or 0

    # Which thread the guest is running, and whether it is the idle one.
    #
    # **This is the only progress metric here that a livelock cannot
    # fake.** `leaves-filled` counts new *mappings*, so a guest working
    # hard over a resident set reads as frozen; exits/s and l2-entries/s
    # rise when the guest is given room and say nothing about whether the
    # work is getting anywhere. A changing thread pointer is scheduling,
    # and scheduling is progress. One unchanging thread over minutes is
    # not.
    #
    # `thread == idle_thread` is the case worth calling out separately: a
    # guest that is idle is not stuck, it is waiting, and those want
    # opposite work.
    if "guest_thread_samples" in off:
        stride = thread_fields * 8
        for cpu in range(args.cpus):
            n = read("guest_thread_sample_count", cpu) or 0
            if not n:
                continue
            print(f"\ncpu {cpu} second-level threads "
                  f"({n} samples, newest last)")
            seen = []
            for slot in range(max(0, n - 6), n):
                i = slot % thread_capacity
                a = (instance + off["guest_thread_samples"] +
                     (cpu * thread_capacity + i) * stride)
                f = [words.get(a + 8 * k, 0) for k in range(thread_fields)]
                gs, prcb, thread, idle, start, state, why, irql = f
                tag = " IDLE" if thread and thread == idle else ""
                seen.append(thread)
                where = (f"ntoskrnl+0x{start - kbase:x}"
                         if kbase and kbase <= start < kbase + ksize
                         else f"0x{start:x}")
                print(f"    thread 0x{thread:x}{tag}  start {where}  "
                      f"state {state}  wait {why}/irql {irql}")
            distinct = len(set(x for x in seen if x))
            print(f"    -> {distinct} distinct thread(s) in the last "
                  f"{len(seen)} samples"
                  f"{'  <- ONE THREAD, not scheduling' if distinct == 1 else ''}")

    # The call stacks, which say what the guest is *doing* rather than
    # where it is.
    #
    # These have been sampled for several sessions and printed by nothing,
    # so every attempt at "what is the second-level guest waiting on" has
    # been answered from exit histograms and instruction pointers instead -
    # and those say where it is, never what called it there.
    #
    # Offsets from the kernel base rather than raw addresses, because the
    # base moves every boot (KASLR) and an offset is comparable across
    # runs and against a PDB. Values outside the image are printed raw:
    # they are stack data that survived the scan's filter, not frames.
    for label, tr, cnt, rsp, rip in (
            ("where it is now", "guest_stack_trace", "guest_stack_count",
             "guest_stack_pointer", "guest_stack_rip"),
            ("what it interrupted", "guest_interrupted_trace",
             "guest_interrupted_count", "guest_interrupted_rsp",
             "guest_interrupted_rip")):
        if tr not in off:
            continue
        n = read(cnt) or 0
        if not n:
            continue
        print(f"\ncpu 0 second-level call stack - {label} "
              f"({n} frames, rsp 0x{read(rsp) or 0:x}, "
              f"rip 0x{read(rip) or 0:x})")
        if kbase:
            print(f"    kernel base 0x{kbase:x} size 0x{ksize:x} "
                  f"- offsets below are into it")
        for i in range(min(n, stack_capacity)):
            frame = words.get(instance + off[tr] + 8 * i, 0)
            if kbase and kbase <= frame < kbase + ksize:
                print(f"    ntoskrnl+0x{frame - kbase:x}")
            else:
                print(f"    0x{frame:x}")

    # The instruction the second level is sitting on, read as bytes.
    #
    # `xp` is a physical read and the hypervisor has already resolved this
    # address through the *second* level's page tables, which is the part
    # nothing outside could do. Sixteen bytes is enough to tell a `vmcall`
    # from a `pause` loop from a `hlt`, which is the whole question.
    code_phys = read("profile_code_physical") or 0
    code_virt = read("profile_code_virtual") or 0
    if code_phys:
        raw = monitor.read_bytes(code_phys, 16) if hasattr(
            monitor, "read_bytes") else None
        print(f"\ncpu 0 second-level hot instruction: "
              f"virtual 0x{code_virt:x} -> physical 0x{code_phys:x}")
        if raw:
            print(f"    bytes {raw.hex()}")
        else:
            print(f"    read it with:  xp /16xb 0x{code_phys:x}")

    if "stimer_given_arms" in off:
        print("\ncpu  stimer arm->fire, measured against the guest's own "
              "1.74 ms constant")
        for cpu in range(args.cpus):
            arms = read("stimer_given_arms", cpu) or 0
            cycles = read("stimer_given_cycles", cpu) or 0
            if not arms:
                continue
            per = cycles / arms
            micro = per / 1992.0
            print(f"{cpu:3d}  {arms:,} arms, {per:,.0f} cycles "
                  f"({micro:,.1f} us at 1.992 GHz), "
                  f"{1e6 / micro if micro else 0:,.1f} Hz "
                  f"-> {micro / 1740.0:.2f}x the 1.74 ms it asked for")

        # The ring, newest last, so an interval can be differenced by hand
        # rather than trusted from the average above. Kind 1 and kind 3
        # alternating is one arm and one delivery per tick.
        # NOT named `base`. That is the module base, it is live for the
        # rest of this function, and shadowing it here pointed the
        # manifest check at 0x14daf40 + 0x2020 and made it report
        # BASE SUSPECT on a base that was provably correct.
        ring_base = off.get("stimer_arm_value")
        if ring_base is not None:
            count = read("stimer_arm_count", 0) or 0
            print(f"\ncpu 0 last synthetic timer events "
                  f"({count:,} total, newest last)")
            for slot in range(max(0, count - 8), count):
                i = slot % stimer_ring
                value = words.get(instance + off["stimer_arm_value"] +
                                  8 * i)
                tsc = words.get(instance + off["stimer_arm_tsc"] + 8 * i)
                kind = words.get(instance + off["stimer_arm_kind"] + 8 * i)
                name = {1: "COUNT written", 2: "CONFIG written",
                        3: "clock vector injected"}.get(kind, f"kind {kind}")
                print(f"    {name:<24} value 0x{value or 0:x} "
                      f"tsc 0x{tsc or 0:x}")

    # A census over every exit, not a sample. One entry at ring 3 proves
    # the guest reached user mode; hundreds of `info registers` samples
    # reading CPL 0 prove only that nothing user-mode was scheduled at
    # those instants, which on an idle machine is unremarkable.
    print("\ncpu  cpl0        cpl1     cpl2     cpl3 (first level, every exit)")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('cpl_seen', cpu * 4):-10d}  "
              f"{read('cpl_seen', cpu * 4 + 1):-7d}  "
              f"{read('cpl_seen', cpu * 4 + 2):-7d}  "
              f"{read('cpl_seen', cpu * 4 + 3):-7d}")

    # The pair that decides whether HvCallModifyVtlProtectionMask is
    # expressed through the extended page tables at all. Neither column
    # means anything alone: a composed side stuck at 7 is only a bug if
    # the guest side was ever something else.
    #
    # bit 0 read, bit 1 write, bit 2 execute - so 7 is read-write-execute
    # and anything less is a permission the level above withheld.
    print("\nept leaf permissions, as bits rwx (guest = eptp12 alone, "
          "composed = installed)")
    for cpu in range(args.cpus):
        guest = [read('guest_leaf_permissions', cpu * 8 + i)
                 for i in range(8)]
        composed = [read('shadow_leaf_permissions', cpu * 8 + i)
                    for i in range(8)]
        if not any(guest or []) and not any(composed or []):
            continue
        for i in range(8):
            g, c = guest[i] or 0, composed[i] or 0
            if g or c:
                print(f"  cpu {cpu}  {i:03b}  guest {g:>12,}  "
                      f"composed {c:>12,}")

    # HvCallModifyVtlProtectionMask, decoded rather than censused raw.
    # RCX is structured: bits 15:0 call code, bit 16 fast, bits 43:32 rep
    # count, bits 59:48 rep start. Printed in full for a handful of calls
    # before any aggregate, because an aggregate over a misdecoded field
    # is exactly the failure this replaces.
    def decode(rcx):
        return (rcx & 0xffff, (rcx >> 16) & 1,
                (rcx >> 32) & 0xfff, (rcx >> 48) & 0xfff)

    for cpu in range(args.cpus):
        total = read('vtl_protect_count', cpu) or 0
        if not total:
            continue

        # The decode's control first. HvCallVtlCall carries no reps, so
        # a nonzero rep count here means the shifts are wrong and
        # nothing below counts.
        control = read('vtl_call_rcx', cpu) or 0
        c_code, c_fast, c_reps, c_start = decode(control)
        verdict = ("DECODE OK" if (c_reps == 0 and c_start == 0)
                   else "DECODE WRONG - rep fields nonzero on a non-rep call")
        print(f"\ncpu {cpu} decode check: HvCallVtlCall rcx=0x{control:016x} "
              f"code=0x{c_code:x} fast={c_fast} reps={c_reps} "
              f"start={c_start}  <- {verdict}")

        f = read("vtl_protect_failures", cpu) or 0
        sh = read("vtl_protect_reps_short", cpu) or 0
        ra = read("vtl_protect_reps_asked", cpu) or 0
        rd = read("vtl_protect_reps_done", cpu) or 0
        print(f"cpu {cpu} ModifyVtlProtectionMask census over ALL calls "
              f"(the ring below is only the last few):")
        print(f"    non-zero statuses {f:,}"
              + (f"   last rax 0x{read('vtl_protect_last_failure', cpu):x}"
                 if f else "   <- never failed"))
        print(f"    answers short of the reps asked: {sh:,}")
        print(f"    reps asked {ra:,}  reps done {rd:,}"
              + ("   <- SHORTFALL" if rd < ra else "   <- all completed"))
        print(f"    the LAST call - the final act of the work item "
              f"that stops:")
        print(f"      rip 0x{read('vtl_protect_last_rip', cpu):x}  "
              f"cr3 0x{read('vtl_protect_last_cr3', cpu):x}")
        print(f"      rsp 0x{read('vtl_protect_last_rsp', cpu):x}")
        print("      stack window:")
        for w in range(32):
            v = read('vtl_protect_last_stack', (cpu * 32) + w) or 0
            if v:
                print(f"        +0x{w * 8:02x}  0x{v:016x}")
        nr = read('vtl_protect_after_read', cpu) or 0
        print(f"      AFTER the answer landed ({nr} words readable) - "
              f"differences from the window above:")
        for w in range(32):
            b4 = read('vtl_protect_last_stack', (cpu * 32) + w) or 0
            af = read('vtl_protect_after_stack', (cpu * 32) + w) or 0
            if b4 != af:
                print(f"        +0x{w * 8:02x}  0x{b4:016x} -> 0x{af:016x}")
        tc = read('vtl_protect_answer_to_caller', cpu) or 0
        to = read('vtl_protect_answer_to_other', cpu) or 0
        print(f"      the answer was delivered to the CALLING level "
              f"{tc:,} times, to the OTHER level {to:,} times"
              + ("   <- answers land in the wrong trust level"
                 if to > tc else ""))
        print("      where the answer resumes the guest, over all calls:")
        for k in range(8):
            c = read('vtl_protect_answer_rip_count', (cpu * 8) + k) or 0
            if c:
                print(f"        0x{read('vtl_protect_answer_rip', (cpu * 8) + k):x}"
                      f"  {c:9,d}")
        oth = read('vtl_protect_answer_rip_other', cpu) or 0
        if oth:
            print(f"        (beyond eight distinct) {oth:,}")
        pr = read('vtl_protect_pfn_probed', cpu) or 0
        ab = read('vtl_protect_pfn_abnormal', cpu) or 0
        if pr:
            print(f"      shadow lookup of the walked frames: {pr:,} probed, "
                  f"{ab:,} not normally mapped"
                  + ("   <- ALL NORMAL, not an EPT difference" if not ab
                     else "   <- a difference on our side"))
            NAMES = {0: "---", 1: "r--", 2: "-w-", 3: "rw-", 4: "--x",
                     5: "r-x", 6: "-wx", 7: "rwx"}
            for k in range(8):
                c = read('vtl_protect_pfn_perm_seen', (cpu * 8) + k) or 0
                if c:
                    print(f"          perms {NAMES[k]}  {c:9,d}")
            rc = read('vtl_protect_readonly_count', cpu) or 0
            if rc:
                pfns = [read('vtl_protect_readonly_pfn', (cpu * 8) + k) or 0
                        for k in range(min(rc, 8))]
                print(f"          the read-only frames ({rc}):")
                for k, pf in enumerate(pfns):
                    hp = read('vtl_protect_host_perms', (cpu * 8) + k) or 0
                    hs = read('vtl_protect_host_status', (cpu * 8) + k) or 0
                    print(f"            0x{pf:x}  shadow r--   "
                          f"our own tables: status {hs} perms "
                          f"{NAMES.get(hp & 7, '?')}"
                          + ("   <- OURS drops write too"
                             if (hp & 2) == 0 else
                             "   <- ours GRANT write; the shadow does not"))
            print(f"        last frame status {read('vtl_protect_pfn_status', cpu)} "
                  f"perms 0x{read('vtl_protect_pfn_perms', cpu):x}")
        print("      the first exit after a protection answer, by reason:")
        for k in range(72):
            c = read('vtl_protect_next_reason', (cpu * 72) + k) or 0
            if c:
                print(f"        {EXIT_REASON.get(k, hex(k)):<16s} {c:9,d}")
        HV = {0x0c: "ModifyVtlProtectionMask", 0x11: "VtlCall",
              0x12: "VtlReturn", 0x0d: "EnablePartitionVtl",
              0x0f: "EnableVpVtl"}
        for k in range(32):
            c = read('vtl_protect_next_code', (cpu * 32) + k) or 0
            if c:
                print(f"          code 0x{k:02x} {HV.get(k, ''):<24s} "
                      f"{c:9,d}")
        lc = read('vtl_protect_next_code_last', cpu) or 0
        print(f"          the LAST answer was followed by code 0x{lc:02x} "
              f"{HV.get(lc, '')}")
        print(f"        last one was: "
              f"{EXIT_REASON.get(read('vtl_protect_next_last', cpu), '?')}")
        print("      and where the NEXT instruction lands:")
        for k in range(8):
            c = read('vtl_protect_step_count', (cpu * 8) + k) or 0
            if c:
                print(f"        0x{read('vtl_protect_step_rip', (cpu * 8) + k):x}"
                      f"  {c:9,d}")
        so = read('vtl_protect_step_other', cpu) or 0
        if so:
            print(f"        (beyond eight distinct) {so:,}")
        print(f"      last answer entered cr3 "
              f"0x{read('vtl_protect_answer_last_cr3', cpu):x}, "
              f"call was from cr3 "
              f"0x{read('vtl_protect_last_cr3', cpu):x}")
        if read('vtl_protect_thread_read', cpu):
            fl = read('vtl_protect_thread_flags', cpu) or 0
            lk = read('vtl_protect_thread_locked', cpu) or 0
            cl = read('vtl_protect_thread_clear', cpu) or 0
            print(f"      secure-kernel thread 0x{read('vtl_protect_thread', cpu):x}"
                  f"  [+0xac] = 0x{fl:08x}  bit4 = {(fl >> 4) & 1}")
            print(f"        over all calls: bit4 SET {lk:,}, clear {cl:,}"
                  + ("   <- the in-use lock is held at the call"
                     if lk > cl else ""))
        print(f"      r15 at the last call (loop remaining) = "
              f"{read('vtl_protect_last_r15', cpu):,}"
              + ("   <- zero: the walk finished"
                 if not read('vtl_protect_last_r15', cpu) else
                 "   <- non-zero: it had work left"))
        diffs = [(w,
                  read('vtl_protect_early_before', (cpu * 32) + w) or 0,
                  read('vtl_protect_early_after', (cpu * 32) + w) or 0)
                 for w in range(32)]
        diffs = [d for d in diffs if d[1] != d[2]]
        print(f"      EARLY call (#100) - what the answer wrote "
              f"({len(diffs)} words changed):")
        for w, b4, af in diffs:
            print(f"        +0x{w * 8:02x}  0x{b4:016x} -> 0x{af:016x}")
        if not diffs:
            print("        nothing changed there either <- 0x40(%rsp) is "
                  "probably NOT the output area, and the reading is wrong")
        print(f"cpu {cpu} HvCallModifyVtlProtectionMask: {total:,} calls")
        cap = 32
        order = ([(total - cap + i) % cap for i in range(cap)]
                 if total >= cap else list(range(min(total, cap))))
        starts = []
        for i in order[-14:]:
            rcx = read('vtl_protect_rcx', cpu * cap + i) or 0
            rdx = read('vtl_protect_rdx', cpu * cap + i) or 0
            rax = read('vtl_protect_rax', cpu * cap + i) or 0
            code, fast, reps, start = decode(rcx)
            done = (rax >> 32) & 0xfff
            status = rax & 0xffff
            self_id = "SELF" if rdx == 0xffffffffffffffff else f"0x{rdx:x}"
            print(f"    code=0x{code:03x} fast={fast} reps={reps:5d} "
                  f"start={start:5d} | answer status=0x{status:04x} "
                  f"done={done:5d} | partition={self_id}")
        for i in order:
            rcx = read('vtl_protect_rcx', cpu * cap + i)
            if rcx:
                starts.append(decode(rcx)[3])
        if starts:
            print(f"  rep start over the last {len(starts)}: "
                  f"{len(set(starts))} distinct, "
                  f"min {min(starts)} max {max(starts)}")

    # **Hyper-V asked to see NMIs from its guest** - its captured pin
    # controls are 0x1e, and bit 3 is NMI exiting - but `l0_wants_l2_exit`
    # claims every NMI unconditionally and nothing in the tree reflects
    # one to the level above. Both KVM (`vmx_check_nested_events`) and the
    # architecture say it should be reflected when vmcs12 asked. This
    # counter says whether it ever fires.
    for _c in range(min(args.cpus, 1)):
        _simp = read('l2_simp_msr', _c) or 0
        _sief = read('l2_siefp_msr', _c) or 0
        if _simp or _sief:
            print(f"\nsynthetic interrupt controller pages, as the guest "
                  f"named them:")
            print(f"  SIMP  0x{_simp:016x}  enabled {_simp & 1}  "
                  f"gpa 0x{_simp & ~0xfff:x}")
            print(f"  SIEFP 0x{_sief:016x}  enabled {_sief & 1}  "
                  f"gpa 0x{_sief & ~0xfff:x}")
            print("  read the message slots with xp at the SIMP gpa: "
                  "sixteen 256-byte slots, a non-zero type word at slot "
                  "base means a message the guest has not consumed")
    print(f"\nNMIs re-injected into a guest rather than reflected: "
          f"{words.get(instance + off['guest_nmis_reinjected'], 0):,}")

    # A held event destroyed by reflect_l2_exit. Non-zero means the
    # fifth thing that can happen to an interrupted event is happening,
    # and an interrupt the second-level guest was owed is simply gone.
    # See `pending_event_lost` in hypervisor.h for why the path exists.
    if "pending_event_lost" in off:
        print("\ncpu  events destroyed by reflect_l2_exit  "
              "first          last           at reason")
        for cpu in range(args.cpus):
            n = read("pending_event_lost", cpu)
            if 0 == n:
                continue
            first = read("pending_event_lost_first", cpu)
            last = read("pending_event_lost_last", cpu)
            why = read("pending_event_lost_reason", cpu)
            print(f"{cpu:3d}  {n:-36,d}  0x{first:08x}     "
                  f"0x{last:08x}     {name_reason(why & 0xffff)}"
                  f" (0x{why:x})")
            print(f"     first vector 0x{first & 0xff:02x}, "
                  f"last vector 0x{last & 0xff:02x}")
        if all(0 == read("pending_event_lost", c)
               for c in range(args.cpus)):
            print("\nno held event was destroyed by reflect_l2_exit "
                  "(pending_event_lost = 0 on every cpu)")
    print("\ncpu  shadow-builds  cache-hits  evictions  resets  reclaims  leaves-filled")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('shadow_ept_builds', cpu):-13d}  "
              f"{read('shadow_ept_cache_hits', cpu):-10d}  "
              f"{read('shadow_ept_evictions', cpu):-9d}  "
              f"{read('shadow_ept_resets', cpu):-6d}  "
              f"{(read('shadow_ept_reclaims', cpu) or 0):-8d}  "
              f"{read('shadow_ept_leaves_filled', cpu):-13d}")

    # **How each fault was answered.** `leaves-filled` above counts only
    # the `installed` branch, so it going nowhere while the fault count
    # climbs means some other branch is taking every fault - and a fault
    # answered without installing anything resumes the guest onto the
    # identical fault. That is a livelock, and it is invisible in every
    # other counter here: exits climb, cache hits climb, nothing errors.
    #
    # Read the *rate*, never the total. Reflections are legitimate and a
    # booting guest makes plenty; what is not legitimate is a steady
    # state where the same disposition keeps rising and `installed` does
    # not move at all.
    # The roots themselves. See the queue above for why one distinct value
    # would be a finding rather than a detail.
    # **Every processor, not just the boot one.** This read was
    # `0 * 4 + i` and printed "cpu 0" - so an application processor's
    # roots could not be seen at all, and the absence of a section for it
    # read exactly like a processor that holds none. That matters
    # directly: the failure being chased is an application processor that
    # never performs a trust-level transition, and whether it holds one
    # root, two, or none is the first thing to compare against the boot
    # processor, which does switch.
    for cpu in range(args.cpus):
        roots = [read('shadow_ept_recall_root', cpu * 4 + i) or 0
                 for i in range(4)]
        current = read('shadow_ept_current_slot', cpu)
        distinct = sorted({r for r in roots if r})

        if not distinct and not current:
            print(f"\ncpu {cpu} shadow EPT roots held: none")
            continue

        print(f"\ncpu {cpu} shadow EPT roots held (slot "
              f"{current} current)")
        for i, r in enumerate(roots):
            print(f"  slot {i}  0x{r:012x}" +
                  ("  <- current" if i == current else ""))
        print(f"  {len(distinct)} distinct non-zero root(s)")
        if len(distinct) == 1:
            print("  ONE ROOT <- both trust levels would be sharing an "
                  "extended page table, which VSM requires them not to")

    # **What fraction of wall time this VMM occupies.** Every account of
    # the cost of this hang so far has been a rate multiplied by an
    # estimated per-exit cost; this is the quantity those were estimating,
    # measured directly. handler_cycles is time inside the exit handler,
    # and the two time stamps bracket the window it accumulated over, so
    # the ratio is the duty cycle - and it needs no assumption about how
    # much an exit costs or how many there were.
    #
    # A guest starved by exit handling shows a duty near 1. A guest that
    # is stuck for some other reason shows a small one, and then the cost
    # arithmetic is a red herring however convincing it looks.
    for cpu in range(min(args.cpus, 1)):
        cycles = read('handler_cycles', cpu) or 0
        first = read('handler_first_tsc', cpu) or 0
        last = read('handler_last_tsc', cpu) or 0
        span = last - first
        if span > 0:
            duty = cycles / span
            print(f"\ncpu {cpu} share of wall time spent in the exit "
                  f"handler")
            print(f"  handler cycles  {cycles:>20,}")
            print(f"  elapsed cycles  {span:>20,}"
                  f"   ({span / 1.992e9:,.1f} s at 1.992 GHz)")
            print(f"  duty            {duty:>20.3f}"
                  + ("   <- starved: the handler owns the processor"
                     if duty > 0.85 else
                     "   <- NOT starved: the guest has time it is not using"))

        # And **whose** the remaining time is. The duty cycle above says
        # how much is not this VMM's; it does not say whether what is left
        # reaches Windows at all. `l1_run_cycles` is the guest hypervisor
        # executing and `l2_run_cycles` is its guest - and "Windows has a
        # fifth of the machine" and "Hyper-V has a fifth of the machine
        # and Windows has almost none" are opposite diagnoses that the
        # duty cycle alone cannot tell apart.
        l1 = read('l1_run_cycles', cpu) or 0
        l2 = read('l2_run_cycles', cpu) or 0
        if span > 0 and (l1 or l2):
            print(f"  of which:")
            print(f"    guest hypervisor (L1)  {l1:>18,}"
                  f"   {100.0 * l1 / span:5.1f}% of wall")
            print(f"    Windows          (L2)  {l2:>18,}"
                  f"   {100.0 * l2 / span:5.1f}% of wall")
            if (l1 + l2) > 0:
                print(f"    Windows' share of non-VMM time: "
                      f"{100.0 * l2 / (l1 + l2):.1f}%")

    # **The secure kernel's own answer.** Byte 1 of the block is the
    # request it is making and the 32-bit word at offset 8 is the status
    # it returned - the slot VslpEnterIumSecureMode itself writes
    # 0xC000001C and 0xC0000030 into on its error paths. A trust-level
    # call that takes zero exits and declines has a reason, and this is
    # the field that carries it.
    if read('vtl_call_block_read', 0):
        blk = [read('vtl_call_block', 0 * 4 + i) or 0 for i in range(4)]
        state = (blk[0] >> 8) & 0xff
        status = blk[1] & 0xffffffff
        print(f"\ncpu 0 IUM secure-call block at "
              f"0x{read('vtl_call_rdx', 0):x}"
              f"  ->  guest-physical "
              f"0x{read('vtl_call_block_physical', 0):x}")
        print(f"  read the same physical from the monitor and compare: "
              f"a disagreement means the two trust levels' extended page "
              f"tables alias it to different host pages")
        for i, q in enumerate(blk):
            print(f"  +0x{i * 8:02x}  0x{q:016x}")
        print(f"  request byte  = {state} (0x{state:02x})")
        MEANING = {0: "secure memory manager (SkmiMapViewOfImage etc)",
                   2: "WPP tracing", 4: "VINA notification",
                   5: "process/thread teardown"}
        req = [read('vtl_call_request', 0 * 256 + i) or 0
               for i in range(256)]
        rtot = sum(req)
        if rtot:
            print(f"  every request byte ever seen ({rtot:,} calls):")
            for i, c in enumerate(req):
                if c:
                    print(f"    code {i:3d}  {c:9,d}  "
                          f"{100.0 * c / rtot:5.1f}%   "
                          f"{MEANING.get(i, '')}")
            print(f"  the block's first quadword changed "
                  f"{read('vtl_block_changes', 0):,} times")
            c0 = read('vtl_code0_count', 0) or 0
            ch = read('vtl_code0_param_changes', 0) or 0
            if c0:
                print(f"  code-0 requests: {c0:,}, parameters differed "
                      f"from the previous one {ch:,} times "
                      f"({100.0 * ch / c0:.1f}%)"
                      + ("   <- one request repeated" if ch * 20 < c0 else
                         "   <- distinct requests"))
                lo = read('vtl_code0_min_pfn', 0) or 0
                hi = read('vtl_code0_max_pfn', 0) or 0
                nc = read('vtl_code0_pfn_calls', 0) or 0
                cons = read('vtl_code0_consecutive', 0) or 0
                if nc:
                    span = hi - lo + 1
                    print(f"  page-walk span: 0x{lo:x}..0x{hi:x} "
                          f"= {span:,} pages ({span * 4096 / 1048576:.1f} MB)"
                          f", {nc:,} page requests, {cons:,} consecutive")
                    print("    " + ("COVERED THE SPAN - the walk finished a "
                                    "region, so the fault is in what should "
                                    "happen next" if nc >= span * 0.9 else
                                    "SHORT OF THE SPAN - it stopped inside"))
                print("  the last code-0 blocks seen:")
                for sl in range(8):
                    w = [read('vtl_code0_ring', (sl * 3) + k) or 0
                         for k in range(3)]
                    if any(w):
                        print(f"    +0x00 0x{w[0]:016x}  "
                              f"+0x08 0x{w[1]:016x}  +0x10 0x{w[2]:016x}")
        signed = status - (1 << 32) if status & 0x80000000 else status
        # The priority each trust-level call is made at. Class 13
        # masks both the clock vector 0xd1 and the deferred-call vector
        # 0x2f, so a call made there with either pending holds VINA
        # asserted - and VINA is what the instruction trace shows
        # preempting the secure kernel after it selects a thread.
        vt = [read('vtl_call_vtpr', 0 * 16 + i) or 0 for i in range(16)]
        vtotal = sum(vt)
        if vtotal:
            print("  task priority at the trust-level call:")
            for i, c in enumerate(vt):
                if c:
                    note = ("  <- masks the clock AND the deferred call"
                            if i == 13 else
                            ("  <- admits both" if i < 2 else ""))
                    print(f"    class {i:2d} (0x{i << 4:02x})  {c:9,d}  "
                          f"{100.0 * c / vtotal:5.1f}%{note}")
        # The latency distribution. A rate cannot separate "delayed a
        # little every iteration" from "delayed enormously on a few",
        # and those want opposite fixes.
        gaps = [read('vtl_call_gap_buckets', 0 * 40 + i) or 0
                for i in range(40)]
        gtotal = sum(gaps)
        if gtotal:
            print("  gaps between consecutive trust-level calls "
                  f"({gtotal:,} of them):")
            run = 0
            for i, c in enumerate(gaps):
                if not c:
                    continue
                run += c
                lo = (1 << i) / 1.992e9
                print(f"    2^{i:<2d} {lo * 1e6:12,.1f} us  {c:9,d}  "
                      f"{100.0 * c / gtotal:5.1f}%  "
                      f"(cumulative {100.0 * run / gtotal:5.1f}%)")
        # What the entry that runs VTL1 carries. VINA reaches the
        # secure kernel as an injected interrupt, and injections go
        # through vmcs02, so this is direct evidence rather than
        # inference from a priority.
        ev = [read('vtl1_entry_vector', 0 * 257 + i) or 0
              for i in range(257)]
        etotal = sum(ev)
        if etotal:
            print(f"  what the entry running VTL1 carried "
                  f"({etotal:,} entries):")
            for i, c in enumerate(ev[:256]):
                if c:
                    print(f"    vector 0x{i:02x}   {c:9,d}  "
                          f"{100.0 * c / etotal:5.1f}%")
            if ev[256]:
                print(f"    no event      {ev[256]:9,d}  "
                      f"{100.0 * ev[256] / etotal:5.1f}%"
                      "  <- nothing injected; whatever ends its turn "
                      "is not an injected interrupt")
        # The bit ShvlVinaHandler tests. See hypervisor.h `vina_flags`.
        if read('vina_read', 0):
            fl = read('vina_flags', 0) or 0
            print(f"  VINA flag chain: gs 0x{read('vina_gs_base', 0):x}"
                  f" -> block 0x{read('vina_block', 0):x}")
            print(f"    dword at +4 = 0x{(fl >> 32) & 0xffffffff:08x}"
                  f"   bit 0 = {(fl >> 32) & 1}"
                  + ("  <- SET: the secure kernel yields"
                     if (fl >> 32) & 1 else "  <- clear"))
            print(f"    block physical = "
                  f"0x{read('vina_block_physical', 0):x}")
            print(f"    at the RETURN (after KiVinaInterrupt cleared "
                  f"it - the aftermath, not the decision):")
            print(f"      set {read('vina_set_count', 0):,}  "
                  f"clear {read('vina_clear_count', 0):,}")
        cs = read('vina_at_call_set', 0) or 0
        cc = read('vina_at_call_clear', 0) or 0
        cu = read('vina_at_call_unread', 0) or 0
        if cs or cc or cu:
            tot = cs + cc
            print(f"    at the CALL, before VTL1 runs - THE DECISION:")
            print(f"      set {cs:,}  clear {cc:,}  unread {cu:,}"
                  + (f"   -> SET on {100.0 * cs / tot:.1f}% of entries"
                     if tot else ""))
        else:
            print("  VINA flag chain: NOT READ  <- walk failed, the "
                  "values above are meaningless")
        # How long VTL1 ran, split by the VINA flag. Aggregated over
        # every entry rather than read off one or two traces.
        dur = [[read('vtl1_duration', (v * 24) + i) or 0
                for i in range(24)] for v in range(2)]
        if sum(dur[0]) or sum(dur[1]):
            print("  how long VTL1 ran (us), by VINA flag at its return:")
            print("      bucket        us     VINA clear     VINA set")
            for i in range(24):
                if not (dur[0][i] or dur[1][i]):
                    continue
                print(f"      2^{i:<2d} {(1 << i) / 1.992e3:9,.1f}  "
                      f"{dur[0][i]:12,d} {dur[1][i]:12,d}")
        print(f"  STATUS        = 0x{status:08x}"
              + ("  <- an NTSTATUS error" if signed < 0 else
                 "  (success or not an error)"))
    else:
        print("\ncpu 0 IUM secure-call block: NOT READ"
              "  <- rdx unmapped or not yet captured, field is meaningless")

    DISPOSITIONS = ("none", "without-ept", "reflected-walk",
                    "reflected-misconfig", "reflected-permission",
                    "watched", "unwatched", "installed",
                    "install-failed", "pointer-failed")
    print("\ncpu 0 how each second-level fault was answered")
    total = sum(read('l2_ept_dispositions', 0 * 10 + i) or 0
                for i in range(len(DISPOSITIONS)))
    for i, name in enumerate(DISPOSITIONS):
        count = read('l2_ept_dispositions', 0 * 10 + i) or 0
        if not count:
            continue
        share = 100.0 * count / total if total else 0.0
        print(f"  {name:<22s} {count:12,d}  {share:5.1f}%")
    # **Which page.** `guest_physical` is recorded for every EPT violation
    # unconditionally - it is not behind ZPP_CENSUS_EXITS, unlike the
    # qualification - so the address is in the ring on every build and
    # nothing here has ever read it.
    #
    # The reason this matters: a fault whose disposition is not
    # `installed` resumes the guest onto the same access. One address
    # repeating across the whole ring is that livelock; a spread of
    # addresses is a guest touching memory, which is normal.
    pages = {}
    reasons = {}
    for slot in range(ring):
        a = instance + off["exit_trace"] + (0 * ring + slot) * entry_size
        reason = words.get(a)
        if reason is None:
            continue
        basic = reason & 0xffff
        reasons[basic] = reasons.get(basic, 0) + 1
        if basic not in (48, 49):
            continue
        physical = words.get(a + 8 * 5)
        if physical is None:
            continue
        page = physical & ~0xfff
        pages[page] = pages.get(page, 0) + 1
    if pages:
        ordered = sorted(pages.items(), key=lambda kv: -kv[1])
        seen = sum(pages.values())
        print(f"\ncpu 0 ept-violation pages in the exit ring "
              f"({seen} of {ring} slots, {len(pages)} distinct)")
        for page, count in ordered[:12]:
            print(f"  0x{page:012x}  {count:6d}  "
                  f"{100.0 * count / seen:5.1f}%")
        if len(pages) == 1:
            print("  ONE PAGE across the whole ring <- a livelock, not "
                  "a guest touching memory")

    unhelpful = read('shadow_ept_leaves_that_did_not_help', 0) or 0
    print(f"  {'leaves that did not help':<22s} {unhelpful:12,d}"
          f"  <- must be zero" if unhelpful else
          f"  leaves that did not help: 0")

    # Why a rebuild happened, which the total above cannot say. A stale
    # generation is this VMM's own tables moving under a root it had
    # already shadowed - `invalidate_ept` bumps one global counter, so a
    # permission change on a single page discards every shadow root and
    # each is refilled a fault at a time.
    # Did the guest hypervisor accept the enlightened VMCS offer? A
    # switch is not on until a counter says the code ran.
    # Which hypercalls the guest hypervisor makes before declining the
    # enlightenment. Not per cpu - the codes are what matter.
    # **Prove the reader before believing anything it says.** The first
    # quadword of the host page table is a present PML4 entry and ends
    # 023; if that does not read back, no other number in this dump is
    # evidence. Four readings were believed this session that were not
    # measurements, and this is the check that would have caught them.
    vec = read('host_exception', 0)
    if vec is not None:
        print(f"\nhost exception: vector {vec} error 0x"
              f"{read('host_exception', 1):x} rip 0x{read('host_exception', 2):x} "
              f"cs 0x{read('host_exception', 3):x} cr2 0x"
              f"{read('host_exception_cr2', 0):x}")

    # The refusal, by number, before anything else. `l2_entries` at zero
    # with `exits` climbing is a second level that never started, and
    # this is the field that says why - SDM 31.4 lists the VM-instruction
    # error codes.
    for cpu in range(args.cpus):
        fails = read('nested_vmfail_count', cpu)
        if fails:
            print(f"cpu {cpu} nested VMfail: {fails:,} times, "
                  f"last error {read('nested_last_vmfail', cpu)}")

    proof = read('host_page_table', 0)
    if proof is None:
        print("\nREADER UNPROVEN: host_page_table did not read back")
    elif 0x023 == (proof & 0xfff):
        print(f"\nreader proven: host_page_table[0] = 0x{proof:x}")
    else:
        print(f"\nREADER SUSPECT: host_page_table[0] = 0x{proof:x}, "
              f"expected a present entry ending 023")

    # And a second proof that is sensitive to the BASE, which the one
    # above is not: it checks twelve bits, so a module base that has
    # moved still lands on some present entry and passes. Measured - a
    # dump taken at a stale base printed `reader proven` and then a phase
    # table of zeroes and an entry count of four billion.
    #
    # The build manifest is a fixed string at a fixed offset from the
    # base, so reading it back is a direct test of the base itself.
    try:
        manifest_va = base + gdb_symbol(args.elf, "zpp_build_switches")
        mon = Monitor(args.rig, args.port)
        # The whole string, not the first two words. The prefix is all the
        # base check needs; the rest says what was compiled in, and one
        # switch below changes what the numbers above *mean*.
        mon.queue(manifest_va, 32)
        got = mon.run()
        raw = b"".join(got.get(manifest_va + 8 * i, 0).to_bytes(8, "little")
                       for i in range(32))
        if raw.startswith(b"zpp switches:"):
            print("base proven: zpp_build_switches reads back at the base")
            manifest = raw.split(b"\0")[0].decode("ascii", "replace")
            print(f"  {manifest}")
            # `census=0` leaves the exit ring's qualification, activity
            # state and CS selector reading zero and `cpl_seen` empty -
            # and zero is a legal value for all three, so nothing in the
            # data says so. Said here, where it is read.
            if b"census=0" in raw:
                print("  NOTE census=0: the exit ring's qualification, "
                      "activity state and CS selector are NOT filled, and "
                      "the cpl columns below are empty by construction - "
                      "build with -DZPP_CENSUS_EXITS=ON to ask")
        # A read that failed and a base that is wrong produce different
        # bytes, and saying so is the whole value of this check.
        #
        # **It cried wolf, and that is why this distinction exists.** The
        # payload came back all `0xff`, the guard blamed the base, and the
        # base was provably right: the module base matched the two
        # `allocate_rwx` lines on serial, `reader proven` passed, and
        # reading `base + 0x2020` by hand from the monitor returned
        # `zpp switches: nested=1 ...` exactly as it should. All-`0xff` is
        # what the monitor returns when a read does not land - most often
        # because something else already holds the one connection it
        # allows - and it is not evidence about the base at all.
        #
        # A guard that fires falsely is worse than no guard, because the
        # next time it fires truthfully nobody will believe it. That is
        # exactly the failure this file's own notes warn about with stale
        # caches and proxy metrics.
        elif not raw.strip(b"\xff") or not raw.strip(b"\x00"):
            print(f"manifest unread at 0x{manifest_va:x}: all "
                  f"0x{raw[0]:02x} bytes. **This is a failed read, not a "
                  f"wrong base** - check nothing else is holding the "
                  f"monitor connection. Compare `reader proven` above: if "
                  f"that passed, the base is fine.")
        else:
            print(f"BASE SUSPECT: {raw!r} at 0x{manifest_va:x} is not the "
                  f"manifest - the module base is probably wrong, and "
                  f"every number above it is fiction")
    except Exception as failure:
        print(f"base unproven: {failure}")

    # Straight after the two proofs, because it is the section most
    # likely to be the only one wanted: it is what a separate reader
    # needs before it can look at the screen at all, and it costs one
    # monitor connection.
    dump_framebuffer(args, args.elf, instance)

    # Which hypervisor-range CPUID leaves the guest actually asks for.
    # The question the exit trace cannot answer: it records that a cpuid
    # happened, not which leaf.
    asked = read('cpuid_hypervisor_leaves_asked', 0)
    count = read('cpuid_trace_count', 0) or 0
    if count:
        print(f"\ncpuid leaves recorded ({count} entries, "
              f"{asked} in the hypervisor range)")
        leaves = {}
        for i in range(min(count, 512)):
            word = read('cpuid_trace', i * 2)
            if word is None:
                continue
            leaf = word & 0xffffffff
            leaves[leaf] = leaves.get(leaf, 0) + 1
        for leaf in sorted(leaves):
            print(f"  0x{leaf:08x}  {leaves[leaf]}")

    seen = read('hypercalls_seen', 0)
    if seen:
        print(f"\nhypercalls seen: {seen}")
        print("  code    count   (which calls the guest hypervisor makes)")
        for i in range(16):
            count = read('hypercall_code_counts', i)
            if not count:
                continue
            print(f"  0x{read('hypercall_codes', i):04x}  {count:-6d}")

    print("\ncpu  rec  vp-assist-writes  evmcs-reads  evmcs-writes")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('evmcs_recommended', cpu):-4d}  "
              f"{read('hyperv_vp_assist_writes', cpu):-16d}  "
              f"{read('evmcs_reads', cpu):-11d}  "
              f"{read('evmcs_writes', cpu):-12d}")

    print("\ncpu  replayed-leaves  faulted-leaves")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('shadow_ept_replayed', cpu):-15d}  "
              f"{read('shadow_ept_leaves_filled', cpu):-14d}")

    # `generation-discards` is slots dropped on the entry path because
    # this VMM's own tables moved under them - see
    # `discard_stale_shadow_ept`. It is zero on a boot that arms every
    # watch before launch and never changes one, which is every boot so
    # far; non-zero is the only evidence that a runtime permission change
    # reached the composed shadows rather than being left to be noticed.
    print("\ncpu  rebuild-new-root  rebuild-stale-generation  "
          "generation-discards")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('shadow_ept_rebuild_new_root', cpu):-16d}  "
              f"{read('shadow_ept_rebuild_stale', cpu):-24d}  "
              f"{read('shadow_ept_generation_discards', cpu):-19d}")

    # Which invept type arrives decides whether the all-context discard
    # is costing anything - single-context already releases only the
    # slot naming that root. Printed beside the rebuild split because
    # the two are read together or not at all.
    print("\ncpu  invept-single-context  invept-all-context")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('l2_invept_single_context', cpu):-21d}  "
              f"{read('l2_invept_all_context', cpu):-18d}")

    # The five the processor saves into vmcs02 on exit, written back
    # only when they differ from what it saved. Zero skipped means the
    # elision did not compile in - a switch is not on until a counter
    # says the code ran.
    # The only figures denominated in the guest's work rather than
    # ours. Four changes worth 1.9x of handler cycles moved no
    # guest-facing indicator, so this is the quantity that matters.
    print("\ncpu  l2-run%  l1-run%  vmm%   (share of wall clock)")
    for cpu in range(args.cpus):
        first = read('handler_first_tsc', cpu) or 0
        last = read('handler_last_tsc', cpu) or 0
        span = last - first
        if span <= 0:
            continue
        l2 = read('l2_run_cycles', cpu) or 0
        l1 = read('l1_run_cycles', cpu) or 0
        vmm = read('handler_cycles', cpu) or 0
        print(f"{cpu:3d}  {100.0*l2/span:6.2f}  {100.0*l1/span:7.2f}  "
              f"{100.0*vmm/span:5.2f}")

    print("\ncpu  hot-state skipped/done")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('hot_state_writes_skipped', cpu):-12d}/"
              f"{read('hot_state_writes_done', cpu):-10d}")

    print("\ncpu  shadow-loads  shadow-stores")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('vmcs_shadow_loads', cpu):-12d}  "
              f"{read('vmcs_shadow_stores', cpu):-13d}")

    # Where the nested round trip's time actually goes.
    #
    # **Read the `cyc/RT` column, not `cyc/call`, and never sum
    # `cyc/call`.** They have different denominators: `build_vmcs02`
    # runs once a round trip, `copy_shadow_to_vmcs12` about four times,
    # and `guest read: map_window` about twenty. A column of cycles a
    # call is a column of prices for different quantities, and summing
    # it is how this project came to believe half the round trip was
    # unattributed when part of that half was one cost counted twice.
    #
    # `cyc/RT` is cycles divided by second-level entries, so it is
    # additive across siblings, and `self` is a phase minus its own
    # children - which is where a cost hides when a container is large
    # and everything named inside it is small.
    for cpu in range(args.cpus):
        dump_phase_tree(cpu, phase_count,
                        lambda member, index:
                        words.get(instance + off[member]
                                  + (cpu * phase_count + index) * 8, 0),
                        read("l2_entries", cpu) or 0,
                        read("handler_cycles", cpu) or 0)

    # Each section separately, because `gdb_offsets` exits the process
    # when a member is missing and the reader routinely runs ahead of
    # the deployed binary - a member renamed in the tree but not yet on
    # the rig killed every section after it, silently, and the dump just
    # looked short. One section failing must not cost the others.
    for section in (dump_entry_rips, dump_priority,
                    dump_synthetic_msrs, dump_reference_tsc,
                    dump_tick_account, dump_l1_host_audit,
                    dump_guest_state_shadow, dump_regions,
                    dump_vtl, dump_vtl_steps):
        try:
            section(args, args.elf, instance)
        except SystemExit as failure:
            print(f"\n[{section.__name__} skipped: {failure}]")

    print("\ncpu  guest-state skipped/done   control skipped/done")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('guest_state_writes_skipped', cpu):11d}/"
              f"{read('guest_state_writes_done', cpu):-11d}  "
              f"{read('control_writes_skipped', cpu):11d}/"
              f"{read('control_writes_done', cpu):-11d}")

    print("\nvmcs fields the guest hypervisor uses")
    dump_field_use(args, instance, off)

    # Is `hypervisor::this_processor()` right?  The watched-page
    # callbacks index per-processor state by it because their signature
    # cannot carry an index, and a wrong index there is silent - one
    # processor's timer armings land in another's row and every number
    # still reads plausibly.  `on_vm_exit` compares it against the index
    # it was handed on every exit, so this is a measurement rather than
    # an argument.
    gs = Monitor(args.rig, args.port)
    gs.queue(instance + off["gs_processor_index_disagreements"], 1)
    gs.queue(instance + off["gs_processor_index_checked"], 1)
    got = gs.run()
    bad = got.get(instance + off["gs_processor_index_disagreements"], 0)
    seen = got.get(instance + off["gs_processor_index_checked"], 0)
    if bad:
        print(f"\nGS PROCESSOR INDEX WRONG: {bad:,} of {seen:,} exits "
              f"disagreed with the index the exit path was handed - "
              f"anything indexed by this_processor() is misattributed")
    else:
        print(f"\ngs processor index agreed on all {seen:,} exits checked")

    try:
        dump_own_field_use(args, args.elf, base)
    except SystemExit as failure:
        print(f"\n[dump_own_field_use skipped: {failure}]")

    dump_handler_by_reason(args, args.elf, instance)
    dump_vmcs02_split(args, args.elf, instance)
    dump_reflect_buckets(args, args.elf, instance)
    dump_profile(args, args.elf, instance)

    # Every processor, not only the boot processor.  The application
    # processors are where "did this one participate at all" is decided,
    # and a per-processor histogram answers it in one line each - cpu 0
    # busy and the rest holding a few thousand cpuid exits is a different
    # machine from all eight holding the same shape.
    for cpu in range(args.cpus):
        counts = monitor_reasons(monitor, instance, off, args, cpu,
                                 reason_capacity)
        total = sum(counts.values()) or 1
        print(f"\ncpu {cpu} exit reasons (total {total:,})")
        for reason, value in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"  {EXIT_REASON.get(reason, reason):<18} {value:>10}  "
                  f"{100.0 * value / total:5.1f}%")

    for cpu in range(args.cpus):
        for member, what in (
                ("external_interrupt_vector_counts",
                 "interrupt vectors acknowledged"),
                ("l2_injected_vector",
                 "vectors injected into the second level"),
                # What the *processor* reported for each external
                # interrupt reflected upward. This is the one census
                # that separates "device interrupts never arrive" from
                # "they arrive and are mis-dispatched" - see the
                # declaration. A run whose vectors are all timer and
                # inter-processor and none belongs to a device says the
                # devices are silent, which is a different fault.
                ("l2_external_vector",
                 "external interrupt vectors reflected upward")):
            vectors = monitor_vector_counts(monitor, instance, off, cpu,
                                            member)
            if not vectors:
                continue
            total = sum(vectors.values())
            print(f"\ncpu {cpu} {what} "
                  f"({total:,} over {len(vectors)} distinct)")
            for vector, value in sorted(vectors.items(),
                                        key=lambda kv: -kv[1]):
                print(f"  0x{vector:02x}  {value:>10}  "
                      f"{100.0 * value / total:5.1f}%")

    for cpu in range(args.cpus):
        count = read("exit_trace_count", cpu)
        if not count:
            continue
        print(f"\n--- cpu {cpu}: last exits (count {count}) ---")
        start = max(0, count - ring)
        for i in range(start, count):
            slot = i % ring
            a = instance + off["exit_trace"] + (cpu * ring + slot) * entry_size
            reason, qual, activity, cs, rip, phys, repeat, value, \
                detail, rip_owner = (words.get(a + 8 * k, 0)
                                     for k in range(10))
            extra = f" phys=0x{phys:x}" if phys else ""
            extra += f" detail=0x{detail:x}" if detail else ""
            extra += f" value=0x{value:x}" if value else ""
            extra += RIP_OWNER.get(rip_owner, "")
            times = f" x{repeat}" if repeat > 1 else ""
            print(f"  [{i:6d}] {name_reason(reason):<16} "
                  f"qual=0x{qual:<12x} {ACTIVITY.get(activity, activity)} "
                  f"cs=0x{cs:04x} rip=0x{rip:x}{extra}{times}")

    def dump_ring(cpu, member, capacity, counter, title):
        """One second-level ring, newest `--l2-entries` records last.

        Both rings hold the same record type and differ only in what
        reaches them, so they print through the same code - which also
        means the working ring cannot drift into a second, subtly
        different reader.
        """
        count = read(counter, cpu)
        show = min(args.l2_entries, count, capacity)
        print(f"\n--- cpu {cpu}: last {show} {title} (count {count}) ---")

        reader = Monitor(args.rig, args.port)
        for i in range(count - show, count):
            slot = i % capacity
            reader.queue(
                instance + off[member]
                + (cpu * capacity + slot) * entry_size, entry_size // 8)
        got = reader.run()

        for i in range(count - show, count):
            slot = i % capacity
            a = (instance + off[member]
                 + (cpu * capacity + slot) * entry_size)
            reason, qual, activity, cs, rip, phys, repeat, value, \
                detail, rip_owner = (got.get(a + 8 * k, 0)
                                     for k in range(10))
            times = f" x{repeat}" if repeat > 1 else ""
            extra = f" phys=0x{phys:x}" if phys else ""
            extra += f" detail=0x{detail:x}" if detail else ""
            extra += f" value=0x{value:x}" if value else ""
            extra += RIP_OWNER.get(rip_owner, "")
            print(f"  [{i:6d}] {name_reason(reason):<16} "
                  f"qual=0x{qual:<12x} {ACTIVITY.get(activity, activity)} "
                  f"cs=0x{cs:04x} rip=0x{rip:x}{extra}{times}")

    if args.l2 is not None:
        dump_ring(args.l2, "l2_exit_trace", l2ring, "l2_exit_trace_count",
                  "second-level exits")

        # The same ring with the idle loop removed, and the one worth
        # reading first.
        #
        # A blocked guest spins its reference-counter poll, its
        # end-of-interrupt and its timer re-arm at about a hundred exits a
        # second, so the ring above holds two or three seconds of that and
        # nothing else - whatever the guest last *did* was evicted long
        # before anybody attached. This one drops exactly those and keeps
        # 4096 of the rest, which is minutes of work rather than seconds
        # of waiting.
        #
        # Its count against the other's is also the measurement that says
        # which failure this is: both climbing is a guest making progress,
        # the working count frozen while the other climbs is a guest that
        # has stopped working and is only waiting.
        dump_ring(args.l2, "l2_working_trace", working_ring,
                  "l2_working_trace_count", "working second-level exits")

    if args.log is not None:
        dump_log(monitor, args.elf, base, args.log)


if __name__ == "__main__":
    main()
