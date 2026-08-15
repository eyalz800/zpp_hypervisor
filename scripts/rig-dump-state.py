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
PHASE_NAMES = ["save_l2_state", "reflect_l2_exit", "build_vmcs02",
               "shadow_ept_pointer_for", "copy_vmcs12_to_shadow",
               "copy_shadow_to_vmcs12", "vmptrld->vmcs02",
               "vmptrld->vmcs01", "merge_nested_bitmaps",
               "on_l2_ept_fault", "  of which guest read",
               "    of which map_window", "load_l1_host_state",
               "exit information"]

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


def gdb_offsets(elf, members):
    """Ask the ELF where each member lives inside the singleton."""
    args = []
    for m in members:
        args += ["-ex", f"print/x (long)&(('zpp::hypervisor::hypervisor' *)0)->{m}"]
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf] + args,
                         capture_output=True, text=True).stdout
    values = re.findall(r"^\$\d+ = (0x[0-9a-f]+)$", out, re.M)
    if len(values) != len(members):
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

    def queue(self, address, words):
        self.pending.append((address, words))

    def run(self):
        script = "".join(f"xp/{n}gx 0x{a:x}\n" for a, n in self.pending)
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
        self.pending = []
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
               "vtl_step_code_rip", "vtl_step_code",
               "vtl_step_code_count"]
    off = gdb_offsets(elf, members)

    kinds, capacity, code_slots, code_size = gdb_values(elf, [
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_count / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_rip[0] / 8",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_code[0] / "
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_code[0][0]",
        "sizeof(('zpp::hypervisor::hypervisor' *)0)->vtl_step_code[0][0]"])

    reader = Monitor(args.rig, args.port)
    for member in ("vtl_step_count", "vtl_step_other",
                   "vtl_step_other_reason", "vtl_step_code_count"):
        reader.queue(instance + off[member], kinds)
    reader.queue(instance + off["vtl_step_rip"], kinds * capacity)
    reader.queue(instance + off["vtl_step_cr3"], kinds * capacity)
    reader.queue(instance + off["vtl_step_code_rip"], kinds * code_slots)
    reader.queue(instance + off["vtl_step_code"],
                 kinds * code_slots * code_size // 8)
    got = reader.run()

    def word(member, index):
        return got.get(instance + off[member] + 8 * index, 0)

    if not any(word("vtl_step_count", k) for k in range(kinds)):
        return

    armed = ["after HvCallVtlCall (expected VTL1)",
             "after HvCallVtlReturn (expected VTL0)"]

    for k in range(kinds):
        count = word("vtl_step_count", k)
        if not count:
            continue

        print(f"\n--- instruction trace {armed[k]}: {count} steps, "
              f"{word('vtl_step_other', k)} other exits "
              f"(first reason 0x{word('vtl_step_other_reason', k):x}) ---")

        # Runs rather than lines: a loop of three addresses spun a
        # thousand times is three lines and a count, and the count is
        # the finding.
        run_rip, run_cr3, run_len = None, None, 0
        for i in range(count):
            rip = word("vtl_step_rip", k * capacity + i)
            cr3 = word("vtl_step_cr3", k * capacity + i)
            if (rip, cr3) == (run_rip, run_cr3):
                run_len += 1
                continue
            if run_rip is not None:
                print(f"    0x{run_rip:016x}  cr3 0x{run_cr3:x}"
                      + (f"  x{run_len}" if run_len > 1 else ""))
            run_rip, run_cr3, run_len = rip, cr3, 1
        if run_rip is not None:
            print(f"    0x{run_rip:016x}  cr3 0x{run_cr3:x}"
                  + (f"  x{run_len}" if run_len > 1 else ""))

        print(f"  {word('vtl_step_code_count', k)} distinct addresses:")
        for i in range(word("vtl_step_code_count", k)):
            rip = word("vtl_step_code_rip", k * code_slots + i)
            base = (k * code_slots + i) * code_size // 8
            raw = b"".join(word("vtl_step_code", base + j)
                           .to_bytes(8, "little")
                           for j in range(code_size // 8))
            print(f"    0x{rip:016x}  {raw.hex()}")


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

    members = ["exit_trace", "exit_trace_count", "l2_exit_trace",
               "l2_exit_trace_count", "l2_working_trace",
               "l2_working_trace_count", "l2_entries", "l2_activity_state",
               "running_l2", "events_requeued", "events_deferred",
               "pending_event", "unhandled_exit", "vm_entry_failure",
               "exit_reason_counts",
               "shadow_ept_builds", "shadow_ept_cache_hits",
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_leaves_filled",
               "vmcs_shadow_loads", "vmcs_shadow_stores",
               "vmcs_field_read_encoding", "vmcs_field_read_count",
               "vmcs_field_write_encoding", "vmcs_field_write_count",
               "vmcs_field_use_overflow",
               "external_interrupt_vector_counts",
               "l2_injected_vector",
               "phase_cycles", "phase_calls",
               "guest_state_writes_skipped", "guest_state_writes_done",
               "control_writes_skipped", "control_writes_done"]
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
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_leaves_filled", "vmcs_shadow_loads",
               "vmcs_shadow_stores",
               "guest_state_writes_skipped", "guest_state_writes_done",
               "control_writes_skipped", "control_writes_done"]
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
    monitor.queue(instance + off["running_l2"], (scalar_cpus + 7) // 8)
    monitor.queue(instance + off["unhandled_exit"], 6)
    monitor.queue(instance + off["vm_entry_failure"], 6)
    for cpu in range(args.cpus):
        monitor.queue(instance + off["exit_trace"] + cpu * ring * entry_size,
                      ring * entry_size // 8)
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

    print("\ncpu  shadow-builds  cache-hits  evictions  resets  leaves-filled")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('shadow_ept_builds', cpu):-13d}  "
              f"{read('shadow_ept_cache_hits', cpu):-10d}  "
              f"{read('shadow_ept_evictions', cpu):-9d}  "
              f"{read('shadow_ept_resets', cpu):-6d}  "
              f"{read('shadow_ept_leaves_filled', cpu):-13d}")

    print("\ncpu  shadow-loads  shadow-stores")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('vmcs_shadow_loads', cpu):-12d}  "
              f"{read('vmcs_shadow_stores', cpu):-13d}")

    # Where the nested round trip's time actually goes. Cycles a call is
    # the number to compare against the price of one VMCS access, since
    # on this rig every one of them traps to the layer below - a phase
    # is, to a first approximation, a count of accesses in disguise.
    print("\ncpu  phase                    calls        cycles  "
          "cycles/call")
    for cpu in range(args.cpus):
        for index, name in enumerate(PHASE_NAMES[:phase_count]):
            calls = words.get(instance + off["phase_calls"]
                              + (cpu * phase_count + index) * 8, 0)
            cycles = words.get(instance + off["phase_cycles"]
                               + (cpu * phase_count + index) * 8, 0)
            if not calls:
                continue
            print(f"{cpu:3d}  {name:<20} {calls:10d}  {cycles:12d}  "
                  f"{cycles // calls:11d}")

    dump_entry_rips(args, args.elf, instance)
    dump_vtl(args, args.elf, instance)
    dump_vtl_steps(args, args.elf, instance)

    print("\ncpu  guest-state skipped/done   control skipped/done")
    for cpu in range(args.cpus):
        print(f"{cpu:3d}  {read('guest_state_writes_skipped', cpu):11d}/"
              f"{read('guest_state_writes_done', cpu):-11d}  "
              f"{read('control_writes_skipped', cpu):11d}/"
              f"{read('control_writes_done', cpu):-11d}")

    print("\nvmcs fields the guest hypervisor uses")
    dump_field_use(args, instance, off)

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
                 "vectors injected into the second level")):
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
