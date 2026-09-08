#!/usr/bin/env python3
"""Read `l2_entry_vector` - what the second-level guest actually
vectored - and set it beside `l2_injected_vector`, what we staged.

**CLAUDE.md names this member specifically and says nothing in
`scripts/` has ever read it.** That is why it exists: `clock_gap_buckets`
counts *stagings out of vmcs12* - what the level above asked for - and
the member's own declaration in `hypervisor.h` records the case where
the two disagree completely, "vector 0xd1 injected 52,799 times ... and
a second-level guest that never vectored once".

So a staging rate is not a delivery rate, and every reading taken off
`clock_gap_buckets` in this tree has been a reading of the request side
only.

**What this script is and is not.**

It takes ONE sample and prints cumulative totals. That is a real
limitation and it is deliberate: the row is 256 entries of 32 bits per
processor, and differencing it would mean 512 quadword reads per sample
through a monitor whose own reader warns that batching corrupts lines
into plausible zeroes (`Monitor.CHUNK`, and the 26-reads-returned-78-
zero-words measurement beside it). Widening the read window is the one
thing the delta reader's note asks callers not to do casually.

**The comparison that survives being cumulative is cpu0 against cpu1**,
because both processors accumulated over the same boot, through the same
run, and are read in the same pass. That is the question this was built
for: on a stalled-A boot cpu0 is staged the clock vector at ~1.78x
cpu1's rate while doing zero fresh trust-level work, and nothing so far
has been able to say whether those extra stagings are *taken*.

Do not quote a single processor's number here as a rate. It is a boot
total, over a span this script does not measure, and CLAUDE.md has three
recorded instances of exactly that mistake.

**32 bits, not 64.** `l2_entry_vector` is
`volatile std::uint32_t[max_cpus][256]`, where its neighbour
`clock_gap_buckets` is 64-bit. The monitor reads quadwords, so each read
carries TWO vector counters - low half is the even index, high half the
odd one. Reading this member at the neighbour's stride produces a full,
plausible, entirely wrong histogram, which is the shape of mistake this
file is otherwise full of.
"""

import argparse
import subprocess
import importlib.util
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent


def load_reader():
    """Import `rig-dump-state.py` for its monitor and ELF helpers.

    The hyphen makes it not a module name, so this goes through
    importlib rather than `import`. Reusing its `Monitor`, `gdb_offsets`
    and `serial_module_base` matters more than the ugliness: those carry
    the batching limit, the retry logic and the unanswered-read tracking
    that a hand-rolled reader would silently omit.
    """
    spec = importlib.util.spec_from_file_location(
        "rig_dump_state", HERE / "rig-dump-state.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["rig_dump_state"] = module
    spec.loader.exec_module(module)
    return module


def unpack32(words, base_address, count):
    """Turn quadwords read at `base_address` into `count` 32-bit slots."""
    out = []
    for index in range(count):
        address = base_address + (index // 2) * 8
        word = words.get(address)
        if word is None:
            out.append(None)
            continue
        out.append((word >> 32) if (index % 2) else (word & 0xffffffff))
    return out


def main():
    ap = argparse.ArgumentParser()
    # **The user matters and its absence is invisible.** The first
    # version of this defaulted to a bare address, so every ssh returned
    # 255 Permission denied and every one of the 32 reads came back
    # unanswered - which read on screen as "the guest vectored nothing",
    # a finding, rather than "this script cannot log in". Match
    # `rig-dump-state.py`'s default exactly rather than retyping it.
    ap.add_argument("--rig", default="tc@192.168.1.199")
    ap.add_argument("--port", default="4446")
    ap.add_argument("--elf", default=".rig-deployed-hypervisor.elf")
    ap.add_argument("--base", default=None)
    ap.add_argument("--cpus", type=int, default=2)
    ap.add_argument("--top", type=int, default=12,
                    help="rows per processor; 0 prints every non-zero "
                         "vector, which is what a census should do")
    # **A targeted read, so a LONG window fits inside the arming budget.**
    # The full census is 128 quadword reads per member per processor, and
    # two of them plus a gap overran the 300 s watchdog on boots 287 and
    # 290. But the question only ever concerns a handful of vectors - the
    # xHCI's 0xa1/0x91/0x81 and a live control like 0xd1 - and reading
    # four slots costs four reads, not 512. That buys a window long
    # enough for zero to mean something: at the measured 0.51/s, 30 s
    # predicts ~15 deliveries and 180 s predicts ~92.
    ap.add_argument("--only", default=None,
                    help="comma-separated vectors (e.g. 0xa1,0x91,0x81,"
                         "0xd1) to read instead of the whole row")
    args = ap.parse_args()

    reader = load_reader()

    base = args.base
    if base is None:
        base = reader.serial_module_base(args.rig)
        if base is None:
            sys.exit("no module base on serial - did the loader run?")
    base = int(base, 16)

    members = ["l2_entry_vector", "l2_injected_vector"]
    off = reader.gdb_offsets(args.elf, members, optional=True, quiet=True)
    missing = [m for m in members if m not in off]
    if missing:
        sys.exit(f"absent from this ELF: {', '.join(missing)} - "
                 f"is --elf the binary that is actually running?")

    # The same resolution the main reader does, spelled the same way -
    # the singleton is a function-local static, so its symbol carries
    # the function in its name.
    instance = base + reader.gdb_symbol(
        args.elf, "zpp::hypervisor::hypervisor::instance()::instance")

    monitor = reader.Monitor(args.rig, args.port)

    # 256 entries of 32 bits = 128 quadwords per processor per member.
    #
    # **Queued in slices of SLICE, not as one 128-word command.** The
    # first version of this asked for `xp/128gx` in a single command and
    # all four reads came back unanswered - which the warning below
    # caught and printed, rather than letting four rows of zeroes be
    # read as "the guest never vectored anything". A wide command is not
    # the same risk as many commands in flight (`Monitor.CHUNK`), but it
    # is a risk, and this reader has no way to tell a long answer that
    # was truncated from one that was empty.
    SLICE = 16
    wanted = None
    if args.only:
        wanted = sorted({int(v, 16) for v in args.only.split(",") if v})
    rows = {}
    for member in members:
        for cpu in range(args.cpus):
            start = instance + off[member] + cpu * 256 * 4
            if wanted is None:
                for word in range(0, 128, SLICE):
                    monitor.queue(start + word * 8, SLICE)
            else:
                # Each quadword holds two 32-bit counters, so one read
                # covers the pair (2i, 2i+1). Deduplicated, since
                # neighbouring vectors share a word.
                for word in sorted({v // 2 for v in wanted}):
                    monitor.queue(start + word * 8, 1)
            rows[(member, cpu)] = start

    words = monitor.run()

    # A reader that cannot say it failed reports health for ever.
    unanswered = getattr(monitor, "unanswered", [])
    if unanswered:
        print(f"WARNING: {len(unanswered)} reads never came back. "
              f"Every zero below may be an unread word, not a count.")

    # **`l2_entry_vector` is compiled out unless census_exits is on.**
    # Its only increment sits inside `if constexpr
    # (nested_vmx::census_exits)` in `resume.cpp`, so on a census=0
    # build the whole row is zero however healthy the guest is - and
    # the first run of this script read exactly that, on a processor
    # visibly meeting its 574.7 Hz clock, which would have been written
    # up as "the guest never vectors an interrupt".
    #
    # The manifest is the authority, per CLAUDE.md, and reading the
    # whole field rather than a prefix of it is the other rule that
    # applies here.
    census = None
    try:
        strings = subprocess.run(
            ["strings", args.elf], capture_output=True, text=True).stdout
        for line in strings.split("\n"):
            if "zpp switches:" in line:
                for field in line.split():
                    if field.startswith("census="):
                        census = field.split("=", 1)[1]
    except OSError:
        pass

    print(f"module base {base:#x}   singleton {instance:#x}")
    if census == "0":
        print("NOTE: this binary is census=0, so l2_entry_vector is "
              "COMPILED OUT and will read zero regardless of the "
              "guest. Only l2_injected_vector below is live.")
    elif census is None:
        print("NOTE: could not read census= out of the manifest, so "
              "whether l2_entry_vector is even compiled in is unknown.")
    print("l2_entry_vector: vectors the SECOND-LEVEL GUEST actually took")
    print("l2_injected_vector: vectors WE staged for it")
    print("Both are BOOT TOTALS, not rates. The fair comparison is "
          "cpu0 against cpu1 in the same pass.\n")

    for member in members:
        for cpu in range(args.cpus):
            counts = unpack32(words, rows[(member, cpu)], 256)
            if wanted is None:
                live = [(v, c) for v, c in enumerate(counts) if c]
            else:
                # Print the asked-for vectors even when zero - a zero
                # that was READ is a datum, where an absent row is not.
                live = [(v, counts[v] or 0) for v in wanted]
            live.sort(key=lambda pair: -pair[1])
            total = sum(c for _, c in live)
            shown = live if args.top == 0 else live[:args.top]
            print(f"{member} cpu{cpu}: {total:,} total across "
                  f"{len(live)} distinct vectors")
            if not live:
                # Only meaningful if every read came back. The warning
                # above is what separates "the guest vectored nothing"
                # from "this script read nothing", and those printed
                # identically until the first run of it did exactly that.
                print(f"    (every entry zero - for {member} that is "
                      f"itself a finding IF the reads all answered; "
                      f"check the warning above before quoting it)")
            for vector, count in shown:
                share = 100.0 * count / total if total else 0.0
                print(f"    vector {vector:#04x}  {count:12,}  {share:5.1f}%")
            if args.top and len(live) > args.top:
                print(f"    ... {len(live) - args.top} colder vectors not "
                      f"shown; pass --top 0 for the whole census")
            print()


if __name__ == "__main__":
    main()
