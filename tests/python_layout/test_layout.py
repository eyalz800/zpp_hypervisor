#!/usr/bin/env python3
"""The diagnostic scripts' hardcoded layout constants, against the C++.

There were no Python tests of any kind in this tree, and the readers under
scripts/ are the one place a wrong number produces *plausible* output
rather than an error. That is the worst failure mode there is: a reader
that crashes sends you to the reader, and a reader that prints confident
nonsense sends you to the hypervisor.

The commit this file exists for is ecc4b70. `rig-dump-state.py` walked the
per-processor exit-reason histogram with a row stride of 64 words where
the array is 96 wide, so cpu 1 onwards read into the middle of its own
neighbour's row. It cancels exactly at cpu 0, which was the only processor
anyone had dumped, so it reported plausible numbers for as long as nobody
looked at a second processor.

The lesson recorded there - "a constant copied here is a constant that
does not move when the header does" - was only half applied. Several
capacities, one entry size, one member count and one member *order* are
still transcribed by hand, and each of them fails the same silent way.

The check is deliberately source-level rather than DWARF-level. Reading
the built ELF would be stronger and is the right end state, but it needs a
cross build to have happened, and this has to be runnable on a machine
that has only cloned the repository - which is exactly when a stale
constant is cheapest to notice.

Run with:  python3 -m unittest discover tests/python_layout
"""
import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "hypervisor", "include", "zpp", "hypervisor",
                      "hypervisor.h")
EXIT_REASON_HEADER = os.path.join(
    ROOT, "hypervisor", "include", "zpp", "arch", "x86_64", "vmx",
    "vmx_exit_reason.h")
DUMP_STATE = os.path.join(ROOT, "scripts", "rig-dump-state.py")
ZPP_GDB = os.path.join(ROOT, "scripts", "zpp.gdb")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def cxx_constant(source, name):
    """The value of a `static constexpr std::size_t <name> = <n>;`.

    Raises rather than returning None when the name is absent. That is the
    negative control this whole family of bugs needs: every one of them is
    "the lookup silently returned nothing and the caller carried on", so a
    lookup that cannot fail loudly is not a check.
    """
    match = re.search(
        r"static\s+constexpr\s+std::size_t\s+" + re.escape(name)
        + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;",
        source)
    if not match:
        raise AssertionError(
            "no `static constexpr std::size_t {}` in the header - it was "
            "renamed or removed, and every script that hardcodes its "
            "value is now silently wrong".format(name))
    return int(match.group(1), 0)


def cxx_member_words(source, struct_name):
    """The names of a diagnostic record's members, in declaration order.

    Handles the two shapes this header uses: a named `struct <name> {`,
    and an anonymous `struct { ... } <name>{};` - the records read by the
    scripts are written both ways, and a checker that only knew one of
    them would silently skip the other, which is the failure mode being
    guarded against.

    Only members declared `std::uint64_t` or `bool` are counted, which is
    all these records contain. Anything else raises rather than returning
    a short list.
    """
    match = re.search(
        r"struct\s+" + re.escape(struct_name) + r"\s*\{(.*?)\n(\s*)\};",
        source, re.S)
    if not match:
        # The anonymous form, anchored to the *nearest* preceding
        # `struct {`. Without the lookahead the non-greedy body starts at
        # the first anonymous struct in the whole header and swallows
        # every one between - which reported sixteen members for a record
        # that has six, and would have been a checker inventing its own
        # version of the bug it exists to catch.
        match = re.search(
            r"struct\s*\{((?:(?!struct\s*\{).)*?)\n\s*\}\s*"
            + re.escape(struct_name) + r"\s*\{\}\s*;",
            source, re.S)
    if not match:
        raise AssertionError(
            "no `struct {}` in the header, named or anonymous - it was "
            "renamed or removed, and every script that reads it by a "
            "fixed word count is now silently wrong".format(struct_name))

    body = match.group(1)
    # Strip comments before counting, so a member named in prose does not
    # count as a member.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)

    members = re.findall(
        r"\b(?:std::uint64_t|bool)\s+([a-z_0-9]+)\s*(?:\{[^}]*\}|=[^;]*)?\s*;",
        body)
    if not members:
        raise AssertionError(
            "struct {} parsed to no members - the parser is wrong, which "
            "is worse than the constant being wrong".format(struct_name))
    return members


class ExitTraceEntry(unittest.TestCase):
    """The record rig-dump-state.py unpacks as a positional 8-tuple."""

    def setUp(self):
        self.header = read(HEADER)
        self.script = read(DUMP_STATE)

    def test_entry_size_is_derived_and_not_copied(self):
        """The stride must come from the type, not from a literal.

        It used to be `entry_size = 0x40` here and this test compared it
        against the member count - which worked, and only because someone
        remembered to run it. A record gained a ninth field the day this
        was rewritten, and a stale stride does not fail: it reads the ring
        at the wrong pitch and prints plausible nonsense, which is the
        same failure mode `gdb_lengths` documents for the capacities.

        So the check is now the stronger one - that no literal is carried
        at all.
        """
        self.assertNotRegex(
            self.script, r"entry_size\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*$",
            "rig-dump-state.py carries a literal entry_size again. Derive "
            "it from sizeof(exit_trace[0][0]) instead - a copy of a "
            "struct's size does not fail when the struct changes, it "
            "reads every ring at the wrong stride.")
        self.assertIn(
            "sizeof(('zpp::hypervisor::hypervisor' *)0)", self.script,
            "rig-dump-state.py no longer asks the ELF for the record "
            "size")

    def test_unpacked_names_match_declaration_order(self):
        """The tuple encodes member *order*, which no size check sees.

        Swapping two uint64_t members changes nothing about the struct's
        size, so a `sizeof` check passes while every printed column is
        relabelled. 7a85e7a was the insertion case of this.
        """
        members = cxx_member_words(self.header, "exit_trace_entry")

        # The names the script unpacks into, in the order it unpacks them.
        match = re.search(
            r"reason, qual, activity, cs, rip, phys, repeat, value, "
            r"\\\n\s*detail, rip_owner",
            self.script)
        self.assertIsNotNone(
            match,
            "rig-dump-state.py no longer unpacks the exit trace as the "
            "tuple this test knows about - re-read it and update this")

        expected = ["reason", "qualification", "activity_state",
                    "cs_selector", "rip", "guest_physical", "repeated",
                    "detail_value", "detail", "rip_owner"]
        self.assertEqual(
            members, expected,
            "exit_trace_entry's members changed order or name. "
            "rig-dump-state.py unpacks them positionally as "
            "(reason, qual, activity, cs, rip, phys, repeat, value, "
            "detail, rip_owner), so "
            "every column it prints is now attributed to the wrong field "
            "- confidently, and with no error.")


class Capacities(unittest.TestCase):
    """The array capacities the scripts carry copies of."""

    def setUp(self):
        self.header = read(HEADER)

    def test_vmcs_field_use_capacity(self):
        """`dump_field_use(capacity=128)`.

        The same shape, the same file and the same kind of default
        argument as the bug ecc4b70 fixed: too small misses the tail of
        four contiguous arrays, too large reads into the next one and
        prints counts where encodings should be.
        """
        declared = cxx_constant(self.header, "vmcs_field_use_capacity")
        script = read(DUMP_STATE)
        match = re.search(r"def dump_field_use\([^)]*capacity=(\d+)",
                          script)
        self.assertIsNotNone(
            match, "dump_field_use no longer takes a capacity default")
        self.assertEqual(
            int(match.group(1)), declared,
            "dump_field_use's capacity default disagrees with "
            "vmcs_field_use_capacity in the header")

    def test_tick_account_ring_capacities(self):
        """`dump_tick_account`'s two ring depths, against the header.

        The tick account merges two rings on the one clock they share -
        the guest's synthetic-timer arms and the level above's own local
        APIC timer arms - and they are declared with *different*
        constants that happen to be equal.  A reader that assumed one
        constant for both would walk the second ring at the wrong pitch
        the moment either moved, and the failure is the one this file
        exists for: it would still print a plausible timeline, in the
        wrong order, from which somebody would conclude the level above
        expires the timer early.
        """
        script = read(DUMP_STATE)

        for name, variable in (("reference_sample_capacity",
                                "stimer_capacity"),
                               ("timer_arm_capacity", "apic_capacity")):
            declared = cxx_constant(self.header, name)
            match = re.search(r"^\s*" + variable + r" = (\d+)$", script,
                              re.M)
            self.assertIsNotNone(
                match,
                "dump_tick_account no longer sets " + variable)
            self.assertEqual(
                int(match.group(1)), declared,
                "dump_tick_account's {} disagrees with {} in the header, "
                "so one of the two rings it merges is read at the wrong "
                "pitch".format(variable, name))

    def test_phase_names_and_parents_cover_every_slot(self):
        """PHASE_NAMES and PHASE_PARENT, against `phase_count`.

        `phase_cycles` is indexed by position, so a name list shorter
        than the array silently stops printing the newest slots - which
        are exactly the ones somebody added because they suspected a
        cost. Longer, and it names slots that do not exist and prints
        them as zero, which reads as "nothing happens there".

        PHASE_PARENT is worse if it drifts, because it is what the tree
        printer subtracts children with: a slot that falls off the end
        is treated as cross-cutting, so its cycles stop being subtracted
        from its parent's `self` column and the parent's residue grows
        by exactly the amount that was just explained.
        """
        declared = cxx_constant(self.header, "phase_count")
        script = read(DUMP_STATE)

        for name in ("PHASE_NAMES", "PHASE_PARENT"):
            match = re.search(
                r"^" + name + r" = \[(.*?)^\]", script,
                re.S | re.M)
            self.assertIsNotNone(
                match, "rig-dump-state.py no longer defines " + name)

        names = re.search(r"^PHASE_NAMES = \[(.*?)\]$", script,
                          re.S | re.M).group(1)
        self.assertEqual(
            len(re.findall(r'"[^"]*"', names)), declared,
            "PHASE_NAMES has a different number of entries than "
            "phase_count, so the phase table names the wrong slots")

        parents = re.search(r"^PHASE_PARENT = \[(.*?)^\]", script,
                            re.S | re.M).group(1)
        self.assertEqual(
            len(re.findall(r"^\s*(?:-?\d+|PHASE_\w+),", parents,
                           re.M)), declared,
            "PHASE_PARENT has a different number of entries than "
            "phase_count, so some slot's nesting is unknown and its "
            "cycles are left out of its parent's residue")

    def test_exit_trace_capacity_in_gdb(self):
        """`set $cap = 32` in scripts/zpp.gdb.

        Literally the same constant class as the fixed bug, in a language
        that could read it for free -
        `sizeof($h->exit_trace[0]) / sizeof($h->exit_trace[0][0])`.
        Wrong, and `$slot = ($n - $count + $i) % $cap` prints the wrong
        slots in the wrong order, plausibly.
        """
        declared = cxx_constant(self.header, "exit_trace_capacity")
        script = read(ZPP_GDB)
        match = re.search(r"set \$cap = (\d+)", script)
        self.assertIsNotNone(
            match, "scripts/zpp.gdb no longer sets $cap")
        self.assertEqual(
            int(match.group(1)), declared,
            "zpp.gdb's $cap disagrees with exit_trace_capacity")


class QueuedRecordLengths(unittest.TestCase):
    """Records read as a fixed number of words."""

    def setUp(self):
        self.header = read(HEADER)
        self.script = read(DUMP_STATE)

    def _queued_words(self, member):
        match = re.search(
            r'monitor\.queue\(instance \+ off\["' + re.escape(member)
            + r'"\], (\d+)\)', self.script)
        self.assertIsNotNone(
            match,
            "rig-dump-state.py no longer queues {} with a literal word "
            "count".format(member))
        return int(match.group(1))

    def test_unhandled_exit_word_count(self):
        members = cxx_member_words(self.header, "unhandled_exit")
        self.assertEqual(
            self._queued_words("unhandled_exit"), len(members),
            "rig-dump-state.py reads a different number of words than "
            "the unhandled_exit record has members, so the tail of the "
            "record is missing or the read runs into what follows it")

    def test_vm_entry_failure_word_count(self):
        """Was queued as 6 words against a record of 18, and is not now.

        The decorator this used to carry said "turns red when the script
        is fixed, which is the moment to delete the decorator", and that
        is what happened: the reader now queues the whole record, so the
        four fields nobody could see - `cpu`, `virtual_processor`,
        `from_trampoline` and `start_up_vector` - are readable.

        Those four are exactly what a processor that fails entry after a
        start-up IPI writes, so the record was blind in the one case it
        exists for.
        """
        members = cxx_member_words(self.header, "vm_entry_failure")
        self.assertEqual(self._queued_words("vm_entry_failure"),
                         len(members))


class NegativeControl(unittest.TestCase):
    """The lookups have to fail loudly when the name moves.

    Every bug in this family is "the lookup silently returned nothing and
    the caller carried on". A checker with the same property checks
    nothing, so the checker's own failure mode is asserted here.
    """

    def test_missing_constant_raises(self):
        with self.assertRaises(AssertionError):
            cxx_constant("struct x {};", "no_such_capacity")

    def test_missing_struct_raises(self):
        with self.assertRaises(AssertionError):
            cxx_member_words("struct x {};", "no_such_record")

    def test_renamed_member_is_visible(self):
        """A rename must change the member list, not silently pass.

        Stated on a synthetic struct rather than the real one, so it
        checks the parser rather than the header.
        """
        source = (
            "    struct probe_record\n"
            "    {\n"
            "        std::uint64_t first{};\n"
            "        // std::uint64_t commented_out{};\n"
            "        std::uint64_t second{};\n"
            "    };\n")
        self.assertEqual(cxx_member_words(source, "probe_record"),
                         ["first", "second"])


class ExitReasonNames(unittest.TestCase):
    """The duplicated exit-reason name table.

    rig-dump-state.py's own docstring calls a duplicated name table worse
    than no names, because "it labels the wrong field confidently" - and
    then carries one. A *renamed* enumerator is the drift that matters; a
    new one degrades acceptably to a bare number, so only names that exist
    in both are compared.
    """

    def test_names_agree_where_both_have_them(self):
        enum_source = read(EXIT_REASON_HEADER)
        script = read(DUMP_STATE)

        declared = {}
        for name, value in re.findall(
                r"^\s*([a-z_0-9]+)\s*=\s*(\d+),\s*$", enum_source,
                re.M):
            declared.setdefault(int(value), name)

        # **Scoped to the table**, not to the whole file.  This used to
        # scan every line of the script for `<n>: "<name>"`, which is a
        # shape any small integer-keyed dictionary has: a ring-kind
        # legend added elsewhere in the file matched it, and the test
        # failed reporting that exit reason 3 had been renamed to
        # something it had nothing to do with.  The same class of defect
        # the whole file is about - an instrument aimed at more than the
        # thing it names cannot tell you it is reading the wrong dict.
        table = re.search(r"^EXIT_REASON = \{(.*?)^\}", script,
                          re.S | re.M)
        self.assertIsNotNone(
            table,
            "rig-dump-state.py no longer defines EXIT_REASON as a "
            "top-level dictionary - the shape this test knows about has "
            "changed")

        scripted = {
            int(value): name
            for value, name in re.findall(
                r"^\s*(\d+):\s*\"([a-z_0-9 /]+)\"", table.group(1),
                re.M)}

        self.assertTrue(
            scripted,
            "rig-dump-state.py's EXIT_REASON table did not parse - the "
            "shape this test knows about has changed")

        disagreements = []
        for value, name in sorted(scripted.items()):
            if value not in declared:
                continue
            # The script uses readable names, the enum uses identifiers.
            # Compare loosely: what matters is that they describe the same
            # thing, and a rename shows up as a word that is simply gone.
            expected = declared[value].replace("_", " ")
            actual = name.replace("_", " ")
            if expected.split()[0] not in actual and \
                    actual.split()[0] not in expected:
                disagreements.append(
                    "  {}: header says {!r}, script says {!r}".format(
                        value, declared[value], name))

        self.assertEqual(
            [], disagreements,
            "the exit reason names in rig-dump-state.py have drifted "
            "from vmx_exit_reason.h:\n" + "\n".join(disagreements))


class FramebufferMembers(unittest.TestCase):
    """The framebuffer members, against the two readers that name them.

    These are read by name through gdb rather than at a copied offset, so
    they cannot drift the way the histogram stride did.  They can still
    drift the *other* way: a member renamed in the header leaves the
    scripts asking for a name gdb cannot resolve, and `rig-dump-state.py`
    resolves them with `optional=True` - which was the right choice, since
    a deployed binary may predate them, and which means a rename makes the
    whole section **silently disappear** instead of failing.

    Silence from an instrument is exactly what this file exists to stop
    being mistaken for a measurement, so the two lists are compared here.
    """

    MEMBERS = ["framebuffer_base", "framebuffer_size", "framebuffer_width",
               "framebuffer_height", "framebuffer_stride",
               "framebuffer_format", "framebuffer_red_mask",
               "framebuffer_green_mask", "framebuffer_blue_mask",
               "framebuffer_reserved_mask"]

    def test_header_declares_every_member(self):
        source = read(HEADER)
        missing = [name for name in self.MEMBERS
                   if not re.search(
                       r"std::uint(?:32|64)_t\s+" + re.escape(name)
                       + r"\s*\{\}\s*;", source)]
        self.assertEqual(
            [], missing,
            "hypervisor.h no longer declares: " + ", ".join(missing))

    def test_both_readers_ask_for_the_same_names(self):
        for path in (DUMP_STATE,
                     os.path.join(ROOT, "scripts", "rig-screen.py")):
            source = read(path)
            missing = [name for name in self.MEMBERS
                       if '"{}"'.format(name) not in source]
            self.assertEqual(
                [], missing,
                "{} no longer asks for: {}".format(
                    os.path.basename(path), ", ".join(missing)))


class RecordExitIsCalledOncePerExit(unittest.TestCase):
    """`record_exit` on a path that also resumes counts the exit twice.

    `hypervisor.h` states the invariant beside `exit_reason_counts`:
    "Counted in record_exit, which runs exactly once per exit - the paths
    that record before stopping do so instead of reaching the resume,
    since on_unhandled_exit does not return."

    `resume_guest` calls it for every exit that reaches the resume, so an
    explicit call is only legal where the processor is about to stop.
    Every other explicit call in this tree is `record_exit(...)`
    immediately followed by `on_unhandled_exit(...)`, which is
    `[[noreturn]]`.

    One was not, and it is why this check exists: the EPT violation whose
    watch had already been dropped used to record and then `break` to the
    resume, so `exit_total[cpu]` and `exit_reason_counts[cpu][48]` were
    both inflated by `ept_violation_unclaimed[cpu]`.

    **The reader's own consistency check cannot catch that**, because it
    compares the histogram's sum against the total and the double count
    moves both. What it corrupts is the difference
    `exit_total - resumes_reached`, which is the one reading that says
    whether a processor is stopped *inside* this VMM - so the defect made
    the answer to "is it in our handler or in its guest" wrong by exactly
    the number of times a watch had been dropped under a fault in flight.

    Checked at the source rather than by running anything: the double
    count is invisible at run time by construction, which is what made it
    survive.
    """

    SOURCES = ["exit_dispatch.cpp", "nested_entry.cpp", "hypervisor.cpp",
               "resume.cpp", "watched_page.cpp", "nested_vmx.cpp"]

    def test_every_explicit_call_precedes_a_stop(self):
        offenders = []
        for name in self.SOURCES:
            path = os.path.join(ROOT, "hypervisor", "src", "hypervisor",
                                name)
            if not os.path.exists(path):
                continue
            lines = read(path).splitlines()
            for index, line in enumerate(lines):
                if not re.search(r"^\s*record_exit\(", line):
                    continue
                # The resume path's own call, which is the one that runs
                # for every exit. Identified by its file rather than by
                # its text, so renaming the arguments cannot smuggle a
                # second one in beside it.
                if name == "resume.cpp":
                    continue
                window = "\n".join(lines[index:index + 8])
                if "on_unhandled_exit(" not in window:
                    offenders.append("{}:{}".format(name, index + 1))

        self.assertEqual(
            [], offenders,
            "record_exit is called on a path that goes on to resume, so "
            "resume_guest records the same exit a second time: "
            + ", ".join(offenders))


class FrozenExitCountReadings(unittest.TestCase):
    """The three counters that say what a still `exits` column means.

    A processor whose exit count has stopped moving is in one of three
    states and the summary table cannot tell them apart: out in its own
    guest and not exiting, stopped inside this VMM's handler, or
    executing the level above's VMLAUNCH on every pass and being parked.
    Each wants a different investigation and all three look identical.

    `resumes_reached`, `l2_start_up_waits` and `ept_violation_unclaimed`
    separate them, and **none of the three had a reader anywhere in this
    tree** - not `rig-dump-state.py`, not `zpp.gdb` - while
    `l2_start_up_waits` in particular is the only thing that
    distinguishes "never attempted a second-level entry" from "attempts
    one every pass and is refused", both of which show `l2_entries` as
    zero.

    An instrument that exists and is not read is worth what an instrument
    that does not exist is worth, and this pins that they are read.
    """

    MEMBERS = ["resumes_reached", "l2_start_up_waits",
               "ept_violation_unclaimed"]

    def test_header_declares_every_member(self):
        source = read(HEADER)
        missing = [name for name in self.MEMBERS
                   if not re.search(
                       r"std::uint64_t\s+" + re.escape(name)
                       + r"\[max_cpus\]", source)]
        self.assertEqual(
            [], missing,
            "hypervisor.h no longer declares as a per-cpu array: "
            + ", ".join(missing))

    def test_the_reader_asks_for_every_member(self):
        source = read(DUMP_STATE)
        missing = [name for name in self.MEMBERS
                   if '"{}"'.format(name) not in source]
        self.assertEqual(
            [], missing,
            "rig-dump-state.py no longer reads: " + ", ".join(missing))

    def test_dispositions_are_printed_for_every_processor(self):
        # The row was read for every processor and printed for cpu 0
        # only, so `watched` - the entry that says whether a
        # *second-level* guest ever wrote a page this VMM watches - has
        # never been visible for an application processor. A claim about
        # it was nonetheless recorded in nested_vmx.h.
        source = read(DUMP_STATE)
        self.assertNotIn(
            "l2_ept_dispositions', 0 * 10", source,
            "rig-dump-state.py prints the second-level fault "
            "dispositions for cpu 0 only again")
        self.assertIn(
            "l2_ept_dispositions', cpu * 10", source,
            "rig-dump-state.py no longer indexes the disposition row by "
            "processor")

    def test_the_reader_prints_the_difference(self):
        # The subtraction itself, not the column heading. The heading
        # also appears in the comment that explains it, so asserting on
        # that would pass with the arithmetic deleted - which is the
        # failure mode this whole file exists to stop, and it was
        # measured here: dropping the print left the check green.
        source = read(DUMP_STATE)
        self.assertTrue(
            re.search(r"exits\s*-\s*reached", source),
            "rig-dump-state.py no longer prints `exit_total - "
            "resumes_reached`, which is the reading that separates a "
            "processor stopped inside this VMM from one spinning in its "
            "own guest")


if __name__ == "__main__":
    unittest.main()
