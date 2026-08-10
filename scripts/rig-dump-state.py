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


def name_reason(value):
    reason = value & 0xffff
    tag = EXIT_REASON.get(reason, str(reason))
    if value & (1 << 31):
        tag += "!ENTRY-FAIL"
    return tag


def monitor_reasons(monitor, instance, off, args, cpu=0, capacity=64):
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
               "l2_exit_trace_count", "l2_entries", "l2_activity_state",
               "running_l2", "events_requeued", "events_deferred",
               "pending_event", "unhandled_exit", "vm_entry_failure",
               "exit_reason_counts",
               "shadow_ept_builds", "shadow_ept_cache_hits",
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_leaves_filled",
               "vmcs_shadow_loads", "vmcs_shadow_stores",
               "vmcs_field_read_encoding", "vmcs_field_read_count",
               "vmcs_field_write_encoding", "vmcs_field_write_count",
               "vmcs_field_use_overflow"]
    off = gdb_offsets(args.elf, members)
    instance = base + gdb_symbol(
        args.elf, "zpp::hypervisor::hypervisor::instance()::instance")
    entry_size = 0x40
    ring = 32

    print(f"module base 0x{base:x}, singleton 0x{instance:x}")

    monitor = Monitor(args.rig, args.port)
    # The scalar per-processor arrays, one read each - they are contiguous.
    scalars = ["exit_trace_count", "l2_exit_trace_count", "l2_entries",
               "l2_activity_state", "events_requeued", "events_deferred",
               "pending_event", "shadow_ept_builds", "shadow_ept_cache_hits",
               "shadow_ept_evictions", "shadow_ept_resets",
               "shadow_ept_leaves_filled", "vmcs_shadow_loads",
               "vmcs_shadow_stores"]
    for name in scalars:
        monitor.queue(instance + off[name], args.cpus)
    monitor.queue(instance + off["running_l2"], (args.cpus + 7) // 8)
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

    print("\nvmcs fields the guest hypervisor uses")
    dump_field_use(args, instance, off)

    print("\ncpu 0 exit reasons")
    counts = monitor_reasons(monitor, instance, off, args)
    total = sum(counts.values()) or 1
    for reason, value in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {EXIT_REASON.get(reason, reason):<18} {value:>10}  "
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
            reason, qual, activity, cs, rip, phys, repeat, detail = (
                words.get(a + 8 * k, 0) for k in range(8))
            extra = f" phys=0x{phys:x}" if phys else ""
            extra += f" detail=0x{detail:x}" if detail else ""
            times = f" x{repeat}" if repeat > 1 else ""
            print(f"  [{i:6d}] {name_reason(reason):<16} "
                  f"qual=0x{qual:<12x} {ACTIVITY.get(activity, activity)} "
                  f"cs=0x{cs:04x} rip=0x{rip:x}{extra}{times}")

    if args.l2 is not None:
        cpu = args.l2
        l2ring = 256
        count = read("l2_exit_trace_count", cpu)
        show = min(args.l2_entries, count, l2ring)
        print(f"\n--- cpu {cpu}: last {show} second-level exits "
              f"(count {count}) ---")
        monitor2 = Monitor(args.rig, args.port)
        for i in range(count - show, count):
            slot = i % l2ring
            monitor2.queue(
                instance + off["l2_exit_trace"]
                + (cpu * l2ring + slot) * entry_size, entry_size // 8)
        w2 = monitor2.run()
        for i in range(count - show, count):
            slot = i % l2ring
            a = (instance + off["l2_exit_trace"]
                 + (cpu * l2ring + slot) * entry_size)
            reason, qual, activity, cs, rip, phys, repeat, detail = (
                w2.get(a + 8 * k, 0) for k in range(8))
            times = f" x{repeat}" if repeat > 1 else ""
            extra = f" phys=0x{phys:x}" if phys else ""
            extra += f" detail=0x{detail:x}" if detail else ""
            print(f"  [{i:6d}] {name_reason(reason):<16} "
                  f"qual=0x{qual:<12x} {ACTIVITY.get(activity, activity)} "
                  f"cs=0x{cs:04x} rip=0x{rip:x}{extra}{times}")


if __name__ == "__main__":
    main()
