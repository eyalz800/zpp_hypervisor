#!/usr/bin/env python3
"""`guest-devnodes.py`: the offsets, and the reads that straddle a page.

Two things are tested and they are the two that fail silently.

**The offsets**, against the values read out of `ntkrnlmp.pdb` GUID
C8A7F11B37FE28227B6B11412E3A0519 with `llvm-pdbutil dump --types`. A
`_DEVICE_NODE` field read at the wrong offset does not fail - `State` is
an `int` and `Problem` is an `unsigned long`, so the wrong offset yields
a number, and the number names a device stage that the device is not in.
The `_PNP_DEVNODE_STATE` base of **768** is the sharpest of these: read
against a zero base every state is `?nnn`, and read from the wrong offset
a plausible member of the enum comes back.

**The page split.** `_DEVICE_NODE` is 904 bytes and an instance path is
routinely longer than what remains of the page it starts in. One `v2p`
answers for one page, so a reader that translates once and reads the
whole structure gets the head from the right place and the tail from
whatever follows it physically - which is not an error, it is data. This
is the same shape as every instrument this project has had to withdraw:
`guest-modules.py`'s 64-character name cap renders a `PCI\\VEN_...` path
as the empty string, which reads as "this node has no name".

Hermetic: `v2p` and `xp_bytes` are replaced by a synthetic address space
with a deliberately *discontiguous* physical mapping, so a reader that
ignores the boundary cannot accidentally pass.
"""
import importlib.util
import os
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "guest-devnodes.py")


def load():
    spec = importlib.util.spec_from_file_location("devnodes", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Read with `llvm-pdbutil dump --types ntkrnlmp.pdb`, LF_FIELDLIST 0x2342
# of LF_STRUCTURE 0x2343 `_DEVICE_NODE`, sizeof 904, 73 members.
DEVICE_NODE_FROM_PDB = {
    "SIBLING": 0, "CHILD": 8, "PARENT": 16, "LAST_CHILD": 24,
    "PHYSICAL_DEVICE_OBJECT": 32, "INSTANCE_PATH": 40, "SERVICE_NAME": 56,
    "PENDING_IRP": 72, "STATE": 300, "PREVIOUS_STATE": 304,
    "STATE_HISTORY": 308, "STATE_HISTORY_ENTRY": 388,
    "COMPLETION_STATUS": 392, "FLAGS": 396, "USER_FLAGS": 400,
    "PROBLEM": 404, "PROBLEM_STATUS": 408, "RESOURCE_LIST": 416,
    "RESOURCE_LIST_TRANSLATED": 424, "DEVICE_NODE_SIZE": 904,
}

# LF_FIELDLIST 0x11BE of `_DEVICE_OBJECT`, and 0x16A6 of `_DRIVER_OBJECT`.
OBJECTS_FROM_PDB = {
    "DO_DRIVER_OBJECT": 8, "DO_ATTACHED_DEVICE": 24, "DO_CURRENT_IRP": 32,
    "DRV_DRIVER_NAME": 56,
}


class OffsetsMatchTheDebugSymbols(unittest.TestCase):
    def test_device_node(self):
        module = load()
        for name, expected in DEVICE_NODE_FROM_PDB.items():
            self.assertEqual(
                getattr(module, name), expected,
                f"{name} disagrees with _DEVICE_NODE in ntkrnlmp.pdb")

    def test_device_and_driver_objects(self):
        module = load()
        for name, expected in OBJECTS_FROM_PDB.items():
            self.assertEqual(getattr(module, name), expected, name)

    def test_the_symbol_rvas(self):
        """Verified against the PDB with `symbolize-trace.py`'s `publics`.

        `IopRootDeviceNode` resolved to exactly 0xf8ba58 at offset 0, and
        the two boot flags 0x20 and 0x22 past it - which is what lets one
        read cover all three.
        """
        module = load()
        self.assertEqual(module.IOP_ROOT_DEVICE_NODE, 0xF8BA58)
        self.assertEqual(module.PNP_BOOT_DRIVERS_LOADED, 0xF8BA78)
        self.assertEqual(module.PNP_BOOT_DRIVERS_INITIALIZED, 0xF8BA7A)
        # The single-read claim, checked rather than asserted in prose.
        span = (module.PNP_BOOT_DRIVERS_INITIALIZED
                - module.IOP_ROOT_DEVICE_NODE + 2)
        self.assertLessEqual(span, 0x30)
        self.assertEqual(module.IOP_ROOT_DEVICE_NODE & ~0xFFF,
                         module.PNP_BOOT_DRIVERS_INITIALIZED & ~0xFFF,
                         "the flags left the root pointer's page, so the "
                         "one-translation read is no longer valid")


class DevnodeStateEnum(unittest.TestCase):
    """`_PNP_DEVNODE_STATE`, LF_ENUM 0x232D. The base is 768, not 0."""

    def test_the_base_is_768(self):
        module = load()
        self.assertEqual(min(module.DEVNODE_STATE), 768)
        self.assertEqual(module.DEVNODE_STATE[768], "Unspecified")

    def test_the_enum_is_contiguous_to_its_maximum(self):
        module = load()
        self.assertEqual(sorted(module.DEVNODE_STATE),
                         list(range(768, 792)))
        self.assertEqual(module.DEVNODE_STATE[791], "MaxDeviceNodeState")

    def test_started_is_778(self):
        module = load()
        self.assertEqual(module.STARTED, 778)
        self.assertEqual(module.DEVNODE_STATE[module.STARTED], "Started")

    def test_start_pending_is_the_one_the_hypothesis_names(self):
        module = load()
        self.assertEqual(module.DEVNODE_STATE[775], "StartPending")
        self.assertIn(775, module.STATE_MEANING)
        self.assertIn("NEVER COMPLETED", module.STATE_MEANING[775])

    def test_every_annotated_state_is_a_real_one(self):
        module = load()
        for state in module.STATE_MEANING:
            self.assertIn(state, module.DEVNODE_STATE)

    def test_problem_zero_is_not_called_healthy(self):
        """The two-field census, enforced.

        `Problem == 0` is also the value for a node never processed and
        for one whose problem was cleared on retry. A reader that labels
        it "none" without saying so would be the fourth instrument in
        this tree to report health for a reason other than health.
        """
        module = load()
        self.assertIn("NOT proof of health", module.PROBLEM_NAME[0])


class PagedReads(unittest.TestCase):
    """The read that straddles a page boundary.

    The synthetic address space below maps two consecutive virtual pages
    to two *non*-consecutive physical pages, which is the ordinary case
    and the one a single translation gets wrong.
    """

    def setUp(self):
        self.module = load()
        # va 0x1000 -> pa 0x900000, va 0x2000 -> pa 0x100000. Deliberately
        # backwards, so a reader that runs off the end of the first page
        # reads bytes that exist and are wrong.
        self.mapping = {0x1000: 0x900000, 0x2000: 0x100000}
        self.physical = {}
        for index in range(0x1000):
            self.physical[0x900000 + index] = index & 0xFF
            self.physical[0x100000 + index] = (0xFF - (index & 0xFF)) & 0xFF

        def v2p(va):
            page = self.mapping.get(va & ~0xFFF)
            return None if page is None else page | (va & 0xFFF)

        def xp_bytes(physical, count):
            return bytes(self.physical.get(physical + i, 0)
                         for i in range(count))

        self.module.v2p = v2p
        self.module.xp_bytes = xp_bytes

    def test_a_read_inside_one_page_is_unchanged(self):
        got = self.module.read(0x1010, 16)
        self.assertEqual(got, bytes(range(0x10, 0x20)))

    def test_a_read_across_the_boundary_follows_the_second_page(self):
        # Four bytes before the boundary and four after it. The second
        # page's bytes descend, so a reader that ignored the boundary
        # would return an ascending run and pass a weaker test.
        got = self.module.read(0x1FFC, 8)
        self.assertEqual(got[:4], bytes([0xFC, 0xFD, 0xFE, 0xFF]))
        self.assertEqual(got[4:], bytes([0xFF, 0xFE, 0xFD, 0xFC]))

    def test_a_whole_device_node_spanning_two_pages(self):
        """904 bytes started near the end of a page. The real case."""
        got = self.module.read(0x1F00, self.module.DEVICE_NODE_SIZE)
        self.assertEqual(len(got), self.module.DEVICE_NODE_SIZE)
        self.assertEqual(got[0], 0x00)          # va 0x1f00 -> 0x900f00
        self.assertEqual(got[0xFF], 0xFF)       # last byte of page one
        self.assertEqual(got[0x100], 0xFF)      # first byte of page two

    def test_an_untranslatable_tail_returns_none_not_a_short_buffer(self):
        """A truncated structure read as a whole one is the same error.

        Returning what was readable would put zeroes in `State` and
        `Problem`, and zero is a legal value for both.
        """
        got = self.module.read(0x2F00, self.module.DEVICE_NODE_SIZE)
        self.assertIsNone(got)

    def test_an_untranslatable_head_returns_none(self):
        self.assertIsNone(self.module.read(0x9000, 8))


class UnicodeStrings(unittest.TestCase):
    """`_UNICODE_STRING` is embedded, 16 bytes, Buffer at +8, Length in
    BYTES - so the character count is half of it."""

    def setUp(self):
        self.module = load()
        self.backing = {}

        def read(va, count):
            if va not in self.backing:
                return None
            return self.backing[va][:count]

        self.module.read = read

    def place(self, va, text):
        self.backing[va] = text.encode("utf-16-le")

    def field(self, length, buffer):
        return (length.to_bytes(2, "little") + length.to_bytes(2, "little")
                + b"\0\0\0\0" + buffer.to_bytes(8, "little"))

    def test_a_disk_instance_path_survives_intact(self):
        """The string `guest-modules.py`'s 64-character cap destroys.

        70 characters. That cap is `0 < n <= 64` on the *character*
        count (`guest-modules.py:78`), and a name failing it returns the
        empty string rather than an error - so the node reads as one with
        no instance path. The bare `PCI\\VEN_...` form is 62 and squeaks
        under; the disk node below, which is the one this investigation
        is actually looking for, does not.
        """
        path = ("SCSI\\Disk&Ven_NVMe&Prod_SAMSUNG_MZVLB512HBJQ-000L7\\"
                "5&1ec5b4c2&0&000000")
        self.assertGreater(len(path), 64)
        self.place(0x5000, path)
        buf = self.field(len(path) * 2, 0x5000)
        self.assertEqual(self.module.unicode_string(buf, 0), path)

    def test_length_is_bytes_not_characters(self):
        self.place(0x5000, "stornvme")
        buf = self.field(16, 0x5000)
        self.assertEqual(self.module.unicode_string(buf, 0), "stornvme")

    def test_a_null_buffer_is_empty_not_unreadable(self):
        self.assertEqual(self.module.unicode_string(self.field(8, 0), 0), "")

    def test_a_zero_length_is_empty(self):
        self.assertEqual(
            self.module.unicode_string(self.field(0, 0x5000), 0), "")

    def test_an_implausible_length_is_refused_rather_than_read(self):
        """A wrong offset yields a huge Length. Reading it would issue a
        megabyte of monitor traffic and return noise."""
        got = self.module.unicode_string(self.field(0xFFFE, 0x5000), 0)
        self.assertIn("implausible", got)

    def test_an_unreadable_buffer_says_so_rather_than_reading_empty(self):
        got = self.module.unicode_string(self.field(16, 0xDEAD000), 0)
        self.assertEqual(got, "<unreadable>")


class ScalarDecoding(unittest.TestCase):
    def test_signed_and_unsigned_longs_differ_where_it_matters(self):
        module = load()
        # An NTSTATUS is negative as a long and must print as 0xc0000001,
        # not as a huge positive decimal.
        buf = (0xC0000001).to_bytes(4, "little")
        self.assertEqual(module.u32(buf, 0), 0xC0000001)
        self.assertEqual(module.s32(buf, 0), -0x3FFFFFFF)

    def test_state_name_reports_an_unknown_value_rather_than_guessing(self):
        module = load()
        self.assertEqual(module.state_name(4242), "?4242")
        # And a state read against a zero base must NOT resolve.
        self.assertEqual(module.state_name(10), "?10")


if __name__ == "__main__":
    unittest.main()
