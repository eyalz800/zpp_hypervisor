#!/usr/bin/env python3
"""`dump_guest_threads`, and specifically its refusals.

**Every case here is a negative control.** The positive one - a well
formed thread list printing a correct table - is the least interesting
thing this reader does, because a reader that prints a plausible table is
exactly what this investigation keeps being misled by. Thirty
mislabelled or stale readings have been recorded in one session of it,
and not one of them looked wrong at the time.

So what is tested is that the four ways this reader can be aimed at
nothing all produce a *refusal* and not a table:

  - the walk never ran, so the array is zero-filled by construction and
    reads as "the process has no threads";
  - the refresh never ran, so every state is from whenever the walk
    happened to succeed and may be minutes stale;
  - `guest_windows.h`'s offsets belong to one Windows build, and against
    a different one every field holds a plausible number;
  - the list is bounded at `thread_walk_limit`, so a full one is
    evidence of truncation rather than of the process's size.

The first and third are the ones that matter. A zero-filled array and a
process with no threads are the same bytes, and only `guest_thread_list_walked`
separates them - this is the same shape as `clock_gap_buckets`, which
reported "96.3% of clock gaps meet the period" for a stream that had
stopped, because a histogram of intervals cannot record the interval it
is inside.

Hermetic on purpose: `gdb_offsets`, `gdb_values` and `Monitor` are all
replaced, so this runs on a machine that has only cloned the repository
and needs neither a cross build nor a rig. That is the same argument
`test_layout.py` makes for being source-level rather than DWARF-level.
"""
import importlib.util
import io
import os
import contextlib
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DUMP_STATE = os.path.join(ROOT, "scripts", "rig-dump-state.py")

# The layout the reader resolves from the ELF. Restated here as the
# *fake* the stub serves, never as an expectation about the C++ - the
# real reader asks gdb and must keep asking gdb, for the reason
# `gdb_lengths` records. A literal here that drifted from the header
# would make this test wrong and the reader right.
STRIDE = 40
CAPACITY = 16
FIELD = {"thread": 0, "start_address": 8, "state": 16,
         "wait_reason": 24, "wait_irql": 32}

LIST = 0x1000
SCALARS = {
    "guest_thread_list_count": 0x2000,
    "guest_thread_list_process": 0x2008,
    "guest_thread_list_walked": 0x2010,
    "guest_thread_refreshes": 0x2018,
    "guest_kernel_base": 0x2020,
    "guest_kernel_size": 0x2028,
}
INSTANCE = 0x40000000

KERNEL_THREAD = 0xffffb00112345000


def load():
    spec = importlib.util.spec_from_file_location("rds", DUMP_STATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Args:
    rig = "nowhere"
    port = "0"
    cpus = 1
    elf = "unused"
    guest_syms = os.path.join(ROOT, "no-such-directory")


def run_reader(threads, count=None, walked=1, refreshes=1,
               process=0xffffb00100000000, unanswered=()):
    """Run `dump_guest_threads` against a synthetic singleton.

    Returns its stdout. Nothing here touches a network, a rig or gdb.
    """
    module = load()

    words = {}
    for i, entry in enumerate(threads):
        at = INSTANCE + LIST + i * STRIDE
        for name, offset in FIELD.items():
            words[at + offset] = entry.get(name, 0)
    values = {
        "guest_thread_list_count":
            len(threads) if count is None else count,
        "guest_thread_list_process": process,
        "guest_thread_list_walked": walked,
        "guest_thread_refreshes": refreshes,
        "guest_kernel_base": 0,
        "guest_kernel_size": 0,
    }
    for name, offset in SCALARS.items():
        words[INSTANCE + offset] = values[name]

    offsets = dict(SCALARS)
    offsets["guest_thread_list"] = LIST
    for name, offset in FIELD.items():
        offsets[f"guest_thread_list[0].{name}"] = LIST + offset

    module.gdb_offsets = (
        lambda elf, members, optional=False, quiet=False:
        {m: offsets[m] for m in members if m in offsets})
    module.gdb_values = lambda elf, expressions: [STRIDE, CAPACITY]

    class FakeMonitor:
        def __init__(self, rig, port):
            self.unanswered = list(unanswered)

        def queue(self, address, count):
            pass

        def run(self):
            return words

    module.Monitor = FakeMonitor

    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        module.dump_guest_threads(Args(), "unused", INSTANCE)
    return out.getvalue()


def waiting(reason, thread=KERNEL_THREAD, irql=0):
    return {"thread": thread, "state": 5, "wait_reason": reason,
            "wait_irql": irql}


class WalkNeverRan(unittest.TestCase):
    """The control that matters most, because zero is a legal answer.

    `walk_guest_threads` stops for good once it finds a process with
    more than one thread, and until then it returns early on every exit.
    A zero-filled `guest_thread_list` is therefore the *normal* state
    before it fires, and it is byte-identical to a process that really
    has no threads. Nothing in the array says which.
    """

    def test_refuses_rather_than_printing_an_empty_table(self):
        out = run_reader([], walked=0)
        self.assertIn("THE WALK NEVER RAN", out)
        self.assertIn("NOT evidence about the guest", out)
        # And it must stop there. A table printed under this condition
        # is sixteen rows of zeroes that read as sixteen dead threads.
        self.assertNotIn("idx  thread", out)

    def test_a_walk_that_ran_and_found_nothing_says_so_differently(self):
        out = run_reader([], walked=1)
        self.assertNotIn("THE WALK NEVER RAN", out)
        self.assertIn("recorded nothing", out)


class OffsetsFromAnotherBuild(unittest.TestCase):
    """Wrong offsets do not fail. They answer.

    `guest_windows.h` says so in its own header comment: a different
    Windows build moves every one of these and nothing notices, because
    the probe follows a pointer read from the wrong place and records a
    plausible number. The legal ranges are the only thing standing
    between that and a confident wrong diagnosis.
    """

    def test_illegal_thread_state_is_named(self):
        out = run_reader([{"thread": KERNEL_THREAD, "state": 200,
                           "wait_reason": 0, "wait_irql": 0}])
        self.assertIn("OFFSETS SUSPECT", out)
        self.assertIn("not a legal _KTHREAD_STATE", out)
        self.assertIn("should not be quoted", out)

    def test_illegal_wait_reason_is_named(self):
        out = run_reader([waiting(99)])
        self.assertIn("OFFSETS SUSPECT", out)
        self.assertIn("not a legal _KWAIT_REASON", out)

    def test_a_thread_pointer_below_the_kernel_floor_is_named(self):
        out = run_reader([waiting(0, thread=0x1234)])
        self.assertIn("OFFSETS SUSPECT", out)
        self.assertIn("not a canonical kernel address", out)

    def test_wait_irql_above_high_level_is_named(self):
        out = run_reader([waiting(0, irql=200)])
        self.assertIn("OFFSETS SUSPECT", out)
        self.assertIn("above HIGH_LEVEL", out)

    def test_a_well_formed_list_is_not_flagged(self):
        """The other half of a control: it must not cry wolf.

        `rig-dump-state.py`'s own base check records what a guard that
        fires falsely costs - "the next time it fires truthfully nobody
        will believe it".
        """
        out = run_reader([waiting(4), waiting(0), waiting(15)])
        self.assertNotIn("OFFSETS SUSPECT", out)


class Staleness(unittest.TestCase):
    """A state read once and never refreshed is a fossil.

    `refresh_guest_threads` is what makes the table describe now rather
    than the moment the walk happened to succeed, and its own docstring
    says the walk caught `Phase1Initialization` still running - which
    says nothing about the stall that follows.
    """

    def test_zero_refreshes_is_announced(self):
        out = run_reader([waiting(0)], refreshes=0)
        self.assertIn("refreshes = 0", out)
        self.assertIn("not from now", out)

    def test_the_reader_says_how_to_check_it_is_live(self):
        # The instrument cannot report its own staleness from inside, so
        # it must hand over the command that can. Same argument as
        # `clock_gap_buckets`, which could not.
        out = run_reader([waiting(0)])
        self.assertIn("guest_thread_refreshes", out)
        self.assertIn("--delta", out)

    def test_the_refresh_counter_is_differenced_by_delta_mode(self):
        """And the command it hands over must actually work.

        A reader that prints "run this to check" for a member `--delta`
        refuses by name is worse than one that prints nothing:
        `clock_gap_buckets` was refused by name for exactly this long,
        and every figure quoted from it was boot-cumulative.
        """
        module = load()
        named = [n for n, _ in module.DELTA_GLOBAL_COUNTERS]
        self.assertIn("guest_thread_refreshes", named)
        self.assertIn("guest_thread_list_walked", named)


class Truncation(unittest.TestCase):
    """A full list is a bound, not a census."""

    def test_a_full_list_is_announced_as_truncated(self):
        out = run_reader([waiting(0)] * CAPACITY)
        self.assertIn("the list is FULL", out)
        self.assertIn("not all of them", out)

    def test_a_short_list_is_not(self):
        out = run_reader([waiting(0)] * 3)
        self.assertNotIn("the list is FULL", out)


class UnansweredReads(unittest.TestCase):
    """An unread word and a zero look identical afterwards."""

    def test_unanswered_reads_are_announced(self):
        out = run_reader([waiting(0)], unanswered=[(0x1234, 4)])
        self.assertIn("READER INCOMPLETE", out)
        self.assertIn("may be an unread word", out)


class TheDiagnosisItExistsToMake(unittest.TestCase):
    """Waiting for time, versus waiting for something to be signalled.

    These are the two branches this whole reading is for, they produce
    an identical exit profile - the idle loop, for ever - and they are
    opposite problems.
    """

    def test_a_runnable_thread_points_at_throughput(self):
        out = run_reader([{"thread": KERNEL_THREAD, "state": 2,
                           "wait_reason": 0, "wait_irql": 0},
                          waiting(4)])
        self.assertIn("AT LEAST ONE THREAD IS RUNNABLE", out)
        self.assertIn("points at throughput", out)

    def test_only_sleepers_does_not_claim_runnable(self):
        out = run_reader([waiting(4), waiting(4)])
        self.assertNotIn("AT LEAST ONE THREAD IS RUNNABLE", out)

    def test_a_timer_sleep_is_called_out_as_not_external(self):
        out = run_reader([waiting(4)])
        self.assertIn("DelayExecution", out)
        self.assertIn("NOT blocked on anything external", out)

    def test_a_dispatcher_wait_is_called_out_as_external(self):
        out = run_reader([waiting(0)])
        self.assertIn("Executive", out)
        self.assertIn("somebody else must signal", out)

    def test_a_paging_wait_is_called_out_as_disk_io(self):
        # `WrPageIn` and `WrPhysicalFault` are the two that say the
        # thread is stopped on the disk, which is the storage-stack
        # branch of this investigation.
        for reason in (2, 9, 39):
            out = run_reader([waiting(reason)])
            self.assertIn("THIS IS DISK I/O", out)

    def test_wait_reasons_are_only_read_from_waiting_threads(self):
        """`WaitReason` is stale for a thread that is not waiting.

        `guest_windows.h` records this for `WaitIrql` - it is the level
        at which a thread last called `KeWaitForSingleObject` - and the
        same is true of the reason beside it. A running thread's
        `WaitReason` is whatever it last waited for, and summarising it
        alongside real waits invents blocked threads.
        """
        out = run_reader([{"thread": KERNEL_THREAD, "state": 2,
                           "wait_reason": 2, "wait_irql": 0}])
        # It appears in the per-thread row, but must not be counted as a
        # wait in the summary below it.
        summary = out.split("running/standby")[1]
        self.assertNotIn("THIS IS DISK I/O", summary)

    def test_the_state_census_adds_up(self):
        out = run_reader([waiting(0), waiting(4),
                          {"thread": KERNEL_THREAD, "state": 2},
                          {"thread": KERNEL_THREAD, "state": 1}])
        self.assertIn("2 waiting, 1 running/standby, 1 ready, 0 other", out)


class EnumsMatchTheDebugSymbols(unittest.TestCase):
    """The tables are transcribed from the PDB, so check they are whole.

    Read with `llvm-pdbutil dump --types ntkrnlmp.pdb` on GUID
    C8A7F11B37FE28227B6B11412E3A0519 - the build `guest_windows.h`
    names. `llvm-pdbutil pretty` cannot be used on macOS: it needs DIA
    and says so.
    """

    def test_thread_states_are_the_ten_windows_defines(self):
        module = load()
        self.assertEqual(len(module.THREAD_STATE), 10)
        self.assertEqual(module.THREAD_STATE[0], "Initialized")
        self.assertEqual(module.THREAD_STATE[5], "Waiting")
        self.assertEqual(module.THREAD_STATE[7], "DeferredReady")

    def test_wait_reasons_run_to_maximum(self):
        module = load()
        self.assertEqual(module.WAIT_REASON[43], "MaximumWaitReason")
        self.assertEqual(sorted(module.WAIT_REASON), list(range(44)))
        self.assertEqual(module.WAIT_REASON[4], "DelayExecution")
        self.assertEqual(module.WAIT_REASON[0], "Executive")

    def test_every_annotated_reason_is_a_real_one(self):
        module = load()
        for reason in module.WAIT_MEANING:
            self.assertIn(reason, module.WAIT_REASON)


class OffsetsAgreeWithTheHypervisor(unittest.TestCase):
    """The Windows offsets the VMM compiles in, against this PDB.

    Not a check of the reader - a check that the numbers the *walk* used
    are the ones the debug symbols give, since everything downstream is
    void if they are not. Verified with `llvm-pdbutil dump --types` and
    transcribed here; `guest_windows.h` is the thing under test.
    """

    FROM_PDB = {
        "ZPP_WINDOWS_KTHREAD_STATE": 388,
        "ZPP_WINDOWS_KTHREAD_WAIT_IRQL": 390,
        "ZPP_WINDOWS_KTHREAD_WAIT_REASON": 643,
        "ZPP_WINDOWS_KTHREAD_PROCESS": 544,
        "ZPP_WINDOWS_ETHREAD_START_ADDRESS": 1248,
        "ZPP_WINDOWS_ETHREAD_THREAD_LIST_ENTRY": 1400,
        "ZPP_WINDOWS_EPROCESS_THREAD_LIST_HEAD": 880,
    }

    def test_guest_windows_matches_the_debug_symbols(self):
        import re
        path = os.path.join(ROOT, "hypervisor", "include", "zpp",
                            "hypervisor", "guest_windows.h")
        with open(path, encoding="utf-8") as handle:
            source = handle.read()
        for name, expected in self.FROM_PDB.items():
            found = re.search(rf"#define\s+{name}\s+(\d+)", source)
            self.assertIsNotNone(found, f"{name} is gone from {path}")
            self.assertEqual(
                int(found.group(1)), expected,
                f"{name} disagrees with ntkrnlmp.pdb "
                f"C8A7F11B37FE28227B6B11412E3A0519. Either the guest "
                f"build moved, in which case every thread reading is "
                f"void until these are re-read, or this is a typo.")


if __name__ == "__main__":
    unittest.main()
