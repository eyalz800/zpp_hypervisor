"""Incomplete guest lists must fail even after printing plausible entries.

Run the real reader control flow with virtual-memory reads supplied by the
fixture. Monitor framing is covered separately in test_qemu_monitor.py.
"""
import ast
import contextlib
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
BASE = 0x10000000


def run_reader(name, words, names=None, dwords=None):
    filename = SCRIPTS / name
    tree = ast.parse(filename.read_text(), filename=str(filename))
    # Replace only the memory-access boundary; retain the list traversal,
    # anchor checks, printing, and exit-status logic from the real script.
    tree.body = [node for node in tree.body if not (
        isinstance(node, ast.FunctionDef) and node.name in {"rq", "rw", "rb", "rname"})]
    namespace = {
        "rq": words.get,
        "rw": (dwords or {}).get,
        "rb": lambda address: 2,
        "rname": (names or {}).get,
    }
    fake_monitor = types.ModuleType("qemu_monitor")
    fake_monitor.read_physical = lambda *args: (_ for _ in ()).throw(
        AssertionError("unexpected physical read"))
    output = io.StringIO()
    with patch.dict(sys.modules, {"qemu_monitor": fake_monitor}), \
            patch.object(sys, "argv", [name, hex(BASE), "0x1000"]), \
            contextlib.redirect_stdout(output):
        try:
            exec(compile(tree, str(filename), "exec"), namespace)
        except SystemExit as exc:
            return exc.code, output.getvalue()
    return 0, output.getvalue()


class ReaderCompletion(unittest.TestCase):
    def test_process_list_needs_both_anchor_and_completion(self):
        head, process = BASE + 0xf05c60, 0x20000000
        link = process + 472
        for next_link, name, success in (
            (head, "System", True),
            (head, "wrong", False),
            (None, "System", False),
            (link, "System", False),
        ):
            with self.subTest(next_link=next_link, name=name):
                status, output = run_reader("guest-processes.py", {
                    head: link, link: next_link,
                    process + 464: 4, process + 720: 0,
                }, {process + 824: name})
                self.assertEqual(status == 0, success)
                self.assertIn(name, output)

    def test_empty_power_list_is_a_successful_zero(self):
        head = BASE + 0xf0bd70
        status, output = run_reader("guest-power-irps.py", {head: head, head + 8: head})
        self.assertEqual(status, 0)
        self.assertIn("well-formed EMPTY list", output)

    def test_partial_or_cyclic_power_list_cannot_report_success(self):
        head, entry = BASE + 0xf0bd70, 0x20000000
        for next_link, state, success in (
            (head, 1, True),
            (0, 1, False),
            (entry, 1, False),
            (head, None, False),
        ):
            with self.subTest(next_link=next_link, state=state):
                status, output = run_reader("guest-power-irps.py", {
                    head: entry, head + 8: entry, entry: next_link,
                    entry + 8: head, entry + 0x10: 0x30000000,
                    entry + 0x18: 0, entry + 0x28: 0, entry + 0x30: 0,
                }, dwords={entry + 0xbc: 1, entry + 0x128: state})
                self.assertEqual(status == 0, success)
                if state == 1:
                    # This is exactly why grepping output alone is unsafe:
                    # even a failed walk may already have printed an entry.
                    self.assertIn("ENABLED (armed", output)

    def test_power_deadline_comes_from_each_timer(self):
        head, entry = BASE + 0xf0bd70, 0x20000000
        start, now = 1_000_000_000, 2_000_000_000
        for timeout, expected in ((120, "timer due in 20.0 s"),
                                  (300, "timer due in 200.0 s"),
                                  (60, "timer overdue by 40.0 s"),
                                  (None, "timer deadline unavailable")):
            with self.subTest(timeout=timeout):
                words = {
                    head: entry, head + 8: entry, entry: head,
                    entry + 8: head, entry + 0x10: 0x30000000,
                    entry + 0x18: 0, entry + 0x28: 0,
                    entry + 0x30: start,
                    entry + 0x50: None if timeout is None else start + timeout * 10_000_000,
                    0xfffff78000000008: now, 0xfffff780000003b0: 500_000_000,
                }
                status, output = run_reader("guest-power-irps.py", words,
                    dwords={entry + 0xbc: 1, entry + 0x128: 1})
                self.assertEqual(status, 0)
                self.assertIn(expected, output)
                self.assertNotIn("% to bugcheck", output)

    def test_disabled_power_timer_has_no_deadline(self):
        head, entry = BASE + 0xf0bd70, 0x20000000
        status, output = run_reader("guest-power-irps.py", {
            head: entry, head + 8: entry, entry: head, entry + 8: head,
            entry + 0x10: 0x30000000, entry + 0x18: 0, entry + 0x28: 0,
            entry + 0x30: 1_000_000_000, entry + 0x50: 2_000_000_000,
            0xfffff78000000008: 1_500_000_000, 0xfffff780000003b0: 0,
        }, dwords={entry + 0xbc: 1, entry + 0x128: 0})
        self.assertEqual(status, 0)
        self.assertNotIn("timer due in", output)
        self.assertNotIn("% to bugcheck", output)


if __name__ == "__main__":
    unittest.main()
