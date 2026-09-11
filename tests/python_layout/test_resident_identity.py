"""Resident reports must retain the module identity and read full switches."""
import ast
import contextlib
import importlib.util
import io
from pathlib import Path
import types
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/rig-dump-state.py"
spec = importlib.util.spec_from_file_location("resident_identity_dump", SCRIPT)
dump = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dump)


class MemoryMonitor:
    def __init__(self, memory):
        self.memory = memory
        self.pending = []

    def queue(self, address, count):
        self.pending.extend(range(address, address + count * 8, 8))

    def run(self):
        result = {a: self.memory[a] for a in self.pending if a in self.memory}
        self.pending.clear()
        return result


class ResidentIdentity(unittest.TestCase):
    def test_diagnostic_rows_do_not_relocate_later_global_reads(self):
        tree = ast.parse(SCRIPT.read_text())
        main = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                    and n.name == "main")
        for member in ("l1_vmcall_count", "vtl1_resume_rip"):
            with self.subTest(member=member):
                block = next(n for n in main.body if isinstance(n, ast.If)
                             and ast.unparse(n.test) == f"'{member}' in off")
                off = {name: i * 0x1000 for i, name in enumerate((
                    "l1_vmcall_count", "l1_vmcall_rcx", "l1_vmcall_rdx",
                    "l1_vmcall_rax", "l1_vmcall_rip", "l1_vmcall_codes",
                    "l1_vmcall_code_counts", "l1_vmcall_code_other",
                    "vtl1_resume_rip", "vtl1_resume_count"))}
                namespace = dict(base=0x100000, instance=0x200000, off=off,
                                 args=types.SimpleNamespace(cpus=2), words={},
                                 monitor=MemoryMonitor({}),
                                 read=lambda *args: 1)
                with contextlib.redirect_stdout(io.StringIO()):
                    exec(compile(ast.Module(body=[block], type_ignores=[]),
                                 str(SCRIPT), "exec"), namespace)
                symbols = {s: 0x1234 for s, _ in
                           dump.VMCS_GLOBAL_COUNTERS + dump.VMCS_GLOBAL_STATE}
                with patch.object(dump, "gdb_symbols", return_value=symbols):
                    addresses, _ = dump.vmcs_globals_resolve(
                        "fixture.elf", namespace["base"])
                self.assertTrue(addresses)
                self.assertEqual(set(addresses.values()), {0x101234})

    def read_manifest(self, data, missing=None):
        data = data.ljust(4096, b"\0")
        memory = {0x102000 + i: int.from_bytes(data[i:i+8], "little")
                  for i in range(0, len(data), 8) if i != missing}
        args = types.SimpleNamespace(elf="fixture.elf", rig="fixture", port=0)
        with patch.object(dump, "gdb_symbol", return_value=0x2000), \
                patch.object(dump, "Monitor", return_value=MemoryMonitor(memory)):
            return dump.read_build_manifest(args, 0x100000)

    def test_switches_after_the_first_256_bytes_are_read(self):
        data = b"zpp switches: " + b"padding=0 " * 35 + b"vcache=1 uevmcs=1\0"
        address, raw = self.read_manifest(data)
        self.assertEqual(address, 0x102000)
        self.assertIn(data, raw)
        self.assertEqual(dump.manifest_field("vcache"), "1")
        self.assertEqual(dump.manifest_field("uevmcs"), "1")

    def test_missing_word_cannot_be_a_fabricated_terminator(self):
        data = b"zpp switches: " + b"padding=0 " * 35 + b"vcache=1\0"
        with self.assertRaises(RuntimeError):
            self.read_manifest(data, missing=24)
        self.assertIsNone(dump.BUILD_MANIFEST)

    def test_unterminated_manifest_is_rejected(self):
        with self.assertRaises(RuntimeError):
            self.read_manifest(b"zpp switches: ".ljust(4096, b"x"))
        self.assertIsNone(dump.BUILD_MANIFEST)

    def test_wrong_base_does_not_retain_a_previous_manifest(self):
        dump.BUILD_MANIFEST = "zpp switches: vcache=1"
        self.read_manifest(b"wrong base\0")
        self.assertIsNone(dump.BUILD_MANIFEST)


if __name__ == "__main__":
    unittest.main()
