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
import hashlib
import os
import re
import sys
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

    def test_interrupted_context_ring_shape(self):
        """`interrupted_contexts`' depth and width, against the header.

        Both are transcribed on one line of `rig-dump-state.py`, and both
        fail silently rather than loudly. The width is the worse of the
        two: the ring is walked with a stride of `fields * 8`, so a
        member added to `interrupted_context` and not counted here shifts
        every entry after the first by one word and prints a register
        file made of its neighbours' halves - plausible addresses, all
        wrong. The depth failing is milder and still bad: too small
        silently drops the oldest samples, and the whole point of the
        table is whether the values *repeat across the ring*, so a short
        read makes a moving register look like a stuck one and the
        verdict line says "a retry" about a walk that is progressing.

        Checked by member *count* rather than by size, because every
        member is a quadword by construction and a count is what the
        reader actually uses.
        """
        script = read(DUMP_STATE)

        members = cxx_member_words(self.header, "interrupted_context")
        capacity = cxx_constant(self.header,
                                "interrupted_context_capacity")

        match = re.search(
            r"^\s*interrupted_context_fields, "
            r"interrupted_context_capacity = (\d+), (\d+)$",
            script, re.M)
        self.assertIsNotNone(
            match,
            "rig-dump-state.py no longer sets the interrupted-context "
            "ring shape on one line - the reader and this check have "
            "drifted apart")
        self.assertEqual(
            int(match.group(1)), len(members),
            "the reader walks interrupted_contexts {} words at a time "
            "and the record has {} members ({}) - every entry after the "
            "first would be read at the wrong offset".format(
                match.group(1), len(members), ", ".join(members)))
        self.assertEqual(
            int(match.group(2)), capacity,
            "the reader's interrupted_context_capacity disagrees with "
            "the header's, so the ring is read short or long and the "
            "'same in every sample' verdict is drawn from the wrong "
            "window")

    def test_interrupted_context_verdict_partitions_by_rip(self):
        """The verdict, against the ring shape that actually occurs.

        The defect this pins: the verdict took `len(set(...))` over every
        row at once, so two interleaved contexts - each internally
        byte-identical - came out as "2 distinct - moving" for every
        register. That is the recorded boot in `BACKLOG.md` ("The guest
        is repeating identical work"), sixteen samples over 713,480
        second-level entries, and the reader printed the opposite of what
        the data said.

        Three cases, and the second is the negative control. Without it
        this test passes against a function that says "a retry" about
        everything, which is the same fail-open shape the rest of this
        file exists for.
        """
        import importlib.util

        spec = importlib.util.spec_from_file_location(
            "rig_dump_state", DUMP_STATE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        verdicts = module.interrupted_context_verdicts

        def row(rip, value):
            # occurred, rip, rsp, rcx, rdx, r8, rsi, rdi, irql, frame
            return [1, rip, value, value, value, 0, value, value, 0,
                    value]

        # 1. The measured shape: two contexts, each frozen, alternating.
        #    Every recorded field differs between the two contexts, so
        #    the replaced verdict reports all six as "moving" here and
        #    cannot pass this by accident - which it does when the two
        #    contexts happen to agree on a field.
        first, second = 0xfffff802b5cb3692, 0xfffff80200001000
        frozen = []
        for _ in range(8):
            frozen.append(row(first, 0x0100001f80000000))
            frozen.append(row(second, 0x12))
        lines = verdicts(frozen)
        self.assertEqual(
            [], [line for line in lines if "moving" in line],
            "two interleaved but individually frozen contexts must show "
            "no movement - counting across both is the bug this "
            "replaces:\n" + "\n".join(lines))
        self.assertEqual(
            2, sum("EVERY field identical" in line for line in lines),
            "each frozen context must be verdicted a retry on its "
            "own:\n" + "\n".join(lines))
        self.assertTrue(
            any("interleaved contexts" in line for line in lines),
            "the number of contexts must be stated separately from "
            "movement, or it gets read as movement again")

        # 2. THE NEGATIVE CONTROL. One context whose registers genuinely
        #    advance must NOT be called a retry, or the verdict is a
        #    constant and says nothing about any guest.
        walking = [row(first, 0x1000 + i) for i in range(8)]
        lines = verdicts(walking)
        self.assertEqual(
            [], [line for line in lines if "a retry" in line],
            "a context whose registers advance every sample was "
            "reported as a retry:\n" + "\n".join(lines))
        self.assertTrue(
            all(("moving: rcx(8)" in line) and ("rsi(8)" in line)
                for line in lines if str(first) or True),
            "a moving context must name the fields that moved and how "
            "many values they took:\n" + "\n".join(lines))

        # 3. A single frozen context, which is the unambiguous case and
        #    the one the old verdict got right - so it must not regress.
        lines = verdicts(frozen[0::2])
        self.assertEqual(
            1, sum("EVERY field identical" in line for line in lines),
            "one frozen context must still read as a retry:\n"
            + "\n".join(lines))

        # And the reader must actually call it rather than keep a second
        # copy of the logic inline.
        self.assertIn(
            "interrupted_context_verdicts(rows", read(DUMP_STATE),
            "the printer no longer calls the tested function, so this "
            "test checks code the rig never runs")

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


class ApLivenessProbeReadings(unittest.TestCase):
    """The six members that say what a silent application processor is
    doing, and the two lists that both have to name them.

    **The failure this pins is a reader that resolves a member and never
    fetches it.** `gdb_offsets` is the only place a name is checked
    against the ELF and the `scalars` loop is the only place one is
    actually read, so a name in `members` alone resolves to a real offset
    whose word is never queued, and `read()` returns `None` for the rest
    of the boot - which prints as a plausible zero rather than as an
    error. The reverse, a name in `scalars` alone, is a `KeyError` and is
    loud. The script's own comment at the `members` list records that
    this has already cost a run, and `FrozenExitCountReadings` above
    cannot catch it: its check is `'"name"' in source`, which one list
    satisfies on its own.

    So this asserts membership of each list separately.

    The members themselves are the liveness probe: four states - a
    processor executing guest code with no reason to exit, one halted,
    one shut down, one stopped inside this VMM - are indistinguishable in
    every other instrument in this tree, and are separated only by where
    a non-maskable interrupt is answered.
    """

    MEMBERS = ["ap_probe_sent", "ap_wake_exit", "ap_wake_root",
               "ap_probe_activity", "ap_probe_rip", "ap_probe_cs",
               # The last hypercall per processor. Same class, same
               # failure: written by the VMM since it was added and in
               # neither reader list, so the one reading that separates
               # "no longer called" from "called once and never
               # returned" did not exist. Both lists, or it reads as a
               # plausible zero again.
               "last_hypercall_code", "last_hypercall_rcx",
               "last_hypercall_rdx", "last_hypercall_r8",
               "last_hypercall_tsc", "last_hypercall_count"]

    @staticmethod
    def _list_named(source, name, terminator):
        """The text of one bracketed list in `rig-dump-state.py`.

        Sliced by its terminating statement rather than by bracket
        matching, because the lists carry comments containing brackets.
        """
        start = source.index("    {} = [".format(name))
        end = source.index(terminator, start)
        return source[start:end]

    def test_header_declares_every_member(self):
        # `volatile` is optional here and it is not decoration: the
        # liveness probe's members are `volatile` because they are
        # written from one path and read from another with nothing
        # between them, and `last_hypercall_*` are not. What this test
        # is for is the *shape* - a per-cpu array of quadwords, which is
        # what both reader lists assume when they queue `scalar_cpus`
        # words. A member that stopped being `[max_cpus]` would be read
        # into its neighbour and print a plausible number, which is the
        # failure this whole file exists for.
        source = read(HEADER)
        missing = [name for name in self.MEMBERS
                   if not re.search(
                       r"(?:volatile\s+)?std::uint64_t\s+"
                       + re.escape(name) + r"\[max_cpus\]", source)]
        self.assertEqual(
            [], missing,
            "hypervisor.h no longer declares as a per-cpu array: "
            + ", ".join(missing))

    def test_every_member_is_in_the_offsets_list(self):
        members = self._list_named(
            read(DUMP_STATE), "members",
            "off = gdb_offsets(args.elf, members)")
        missing = [name for name in self.MEMBERS
                   if '"{}"'.format(name) not in members]
        self.assertEqual(
            [], missing,
            "rig-dump-state.py's `members` list no longer names, so "
            "`gdb_offsets` never resolves an offset for: "
            + ", ".join(missing))

    def test_every_member_is_in_the_queue_list(self):
        scalars = self._list_named(
            read(DUMP_STATE), "scalars", "for name in scalars:")
        missing = [name for name in self.MEMBERS
                   if '"{}"'.format(name) not in scalars]
        self.assertEqual(
            [], missing,
            "rig-dump-state.py's `scalars` list no longer names, so the "
            "offset resolves and the word is never fetched and reads as "
            "a plausible zero: " + ", ".join(missing))

    def test_the_reader_reports_a_verdict_per_processor(self):
        # The counters alone are not the instrument - the reading is
        # which of the four states they mean, and a table of six numbers
        # with no verdict is one the next reader has to re-derive.
        source = read(DUMP_STATE)
        for reading in ("halted",
                        "shut down after a triple fault",
                        "wait-for-SIPI",
                        "inside this VMM"):
            self.assertIn(
                reading, source,
                "rig-dump-state.py no longer names the `{}` reading of "
                "the liveness probe".format(reading))

    def test_the_reader_distinguishes_unbuilt_from_unanswered(self):
        # A switched-off instrument and a processor that answers nothing
        # are the same six zeroes. The manifest separates them, and the
        # reader has to say so rather than leaving it to be guessed -
        # the same trap `apfault=` already records.
        source = read(DUMP_STATE)
        self.assertIn(
            "probe=0", source,
            "rig-dump-state.py no longer tells a build without the "
            "liveness probe apart from a processor that answered none")

    def test_the_switch_is_in_the_build_manifest(self):
        # Four edits make a switch, and this is the fourth. A cache
        # reading ON is not evidence; the binary saying so is.
        source = read(os.path.join(
            ROOT, "hypervisor", "src", "hypervisor", "build_switches.cpp"))
        self.assertIn(
            "'p', 'r', 'o', 'b', 'e', '='", source,
            "build_switches.cpp no longer reports `probe=`, so a binary "
            "cannot be asked whether the liveness probe is compiled in")
        self.assertIn(
            "digit(nested_vmx::probe_aps)", source,
            "build_switches.cpp no longer reports the liveness probe "
            "from the constant the code branches on")


class Code0RingIsRotated(unittest.TestCase):
    """The ring that revived a retracted lead, and the check that stops it.

    `vtl_code0_ring` is a circular buffer of eight with the newest entry
    at `(count - 1) % 8`. `1e22213` (2026-08-22) retracted the
    "four-frame lead" - `0x11aac9`..`0x11aacc` - precisely because the
    reader printed slots 0..7 in raw order, so frames in the MIDDLE of
    the window were read as the last ones the walk made.

    **The printer was never fixed.** Five days later `0c20f16` read the
    same unrotated buffer and reported "the walk stops at two known
    frames", reviving the lead its own file had killed. That is the
    failure this class exists to make impossible: the retraction lived
    in prose, and prose does not run.

    `hypervisor.h` states the convention beside `vtl_code0_wide` -
    "newest at `(count - 1) % 32`. That ordering is not decoration:
    reading the narrow ring as though slot 0 were oldest is exactly what
    produced the four-frame lead".
    """

    # The code-0 count from the dump in `0c20f16`. Kept as the literal
    # from that run so the negative control below is a measurement of
    # the real failure rather than of an invented one.
    REVIVING_COUNT = 21162

    def test_the_rotation_puts_the_newest_entry_last(self):
        for count in (0, 1, 7, 8, 9, self.REVIVING_COUNT):
            order = [(count + n) % 8 for n in range(8)]
            self.assertEqual(
                (count - 1) % 8, order[-1],
                "the reader's rotation does not print the newest entry "
                "last for count {}".format(count))
            self.assertEqual(
                sorted(order), list(range(8)),
                "the rotation does not visit every slot exactly once "
                "for count {}".format(count))

    def test_the_unrotated_reader_fails_this_check(self):
        """NEGATIVE CONTROL - measured against the run that misled.

        A check that cannot fail is not a check. This asserts that the
        OLD expression - `for sl in range(8)` - is actually caught, and
        it is: at the count `0c20f16` dumped, the newest entry is slot
        1 while raw order prints slot 7 last. The two frames reported as
        "where the walk stops" were slots 1 and 2, six positions from
        the end of the window.
        """
        raw = list(range(8))
        newest = (self.REVIVING_COUNT - 1) % 8
        self.assertEqual(1, newest)
        self.assertEqual(7, raw[-1])
        self.assertNotEqual(
            newest, raw[-1],
            "the unrotated reader would pass this check, so the check "
            "has no power to detect the defect it was written for")

    def test_the_reader_rotates_the_ring(self):
        source = read(DUMP_STATE)
        self.assertTrue(
            "sl = (c0 + n) % 8" in source,
            "rig-dump-state.py no longer rotates `vtl_code0_ring`, so "
            "it prints slots in raw order under a 'last ... seen' "
            "label - the exact defect 1e22213 retracted a lead for")
        self.assertFalse(
            "for sl in range(8):" in source,
            "rig-dump-state.py walks the code-0 ring in raw slot order "
            "again")

    def test_the_retracted_span_verdict_is_not_reinstated(self):
        # `83818da` measured the walk as ~900 runs of ~8 pages and
        # recorded that the verdict "assumes the wrong shape and should
        # not be believed". `min`/`max` are the extremes of what was
        # asked for, so nothing can fall short of a bound it set by
        # reaching it.
        source = read(DUMP_STATE)
        self.assertFalse(
            "SHORT OF THE SPAN" in source,
            "rig-dump-state.py prints the span verdict retracted in "
            "83818da - it reads a density as a completion fraction")
        self.assertIn(
            "NOT a completion", source,
            "rig-dump-state.py no longer says what the span figure is "
            "not, so the next reader re-derives the retracted reading")


class WalkShapeInstrument(unittest.TestCase):
    """The counters that separate one long run from nine hundred short.

    Every member here is written by the VMM and has to be in both reader
    lists or it resolves an offset, is never fetched, and prints as a
    plausible zero - the failure `FrozenExitCountReadings` and
    `LivenessProbeMembers` above both exist for.
    """

    MEMBERS = ["vtl_code0_run_current", "vtl_code0_run_longest",
               "vtl_code0_same", "vtl_code0_back", "vtl_code0_skip",
               "vtl_code0_epoch_tsc", "vtl_code0_epoch_pfn",
               "vtl_code0_epoch_code0", "vtl_code0_epoch_calls",
               "vtl_code0_epoch_count", "vtl_code0_epoch_last",
               "vtl_code0_word_value", "vtl_code0_word_count",
               "vtl_code0_word_other", "vtl_call_block_below_floor",
               "vtl_call_block_untranslated",
               "vtl_call_block_unreadable"]

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

    def test_every_member_is_in_the_offsets_list(self):
        source = read(DUMP_STATE)
        start = source.index("    members = [")
        members = source[start:source.index(
            "off = gdb_offsets(args.elf, members)", start)]
        missing = [name for name in self.MEMBERS
                   if '"{}"'.format(name) not in members]
        self.assertEqual(
            [], missing,
            "rig-dump-state.py's `members` list no longer names, so "
            "`gdb_offsets` never resolves an offset for: "
            + ", ".join(missing))

    def test_every_member_is_queued(self):
        # Named in the offsets list and never queued is the silent half:
        # the offset resolves, the word is never fetched, and it reads
        # as zero. Each name has to appear at least twice.
        source = read(DUMP_STATE)
        missing = [name for name in self.MEMBERS
                   if source.count('"{}"'.format(name)) < 2]
        self.assertEqual(
            [], missing,
            "rig-dump-state.py names these in one list only, so they "
            "resolve an offset and are never fetched: "
            + ", ".join(missing))

    def test_the_epoch_ring_capacity_agrees_with_the_header(self):
        # The reader queues a fixed 64 words per processor. A capacity
        # changed in the header and not here reads into its neighbour -
        # ecc4b70's bug exactly, and the reason this file exists.
        self.assertEqual(
            64, cxx_constant(read(HEADER), "vtl_code0_epoch_slots"),
            "hypervisor.h's `vtl_code0_epoch_slots` no longer matches "
            "the 64 words per processor rig-dump-state.py queues")
        self.assertEqual(
            16, cxx_constant(read(HEADER), "vtl_code0_word_slots"),
            "hypervisor.h's `vtl_code0_word_slots` no longer matches "
            "the 16 words per processor rig-dump-state.py queues")
        source = read(DUMP_STATE)
        self.assertIn("scalar_cpus * 64", source)
        self.assertIn("scalar_cpus * 16", source)

    def test_the_reader_checks_its_own_partition(self):
        # The identity `consecutive + same + back + skip == calls - 1`
        # is the only self-check in this family of counters. Twelve
        # instruments in this investigation measured the wrong thing and
        # every one was caught by a second reading disagreeing.
        source = read(DUMP_STATE)
        self.assertIn(
            "partition ", source,
            "rig-dump-state.py no longer checks that the step "
            "partition adds up to the request count")
        self.assertIn(
            "LOWER BOUND", source,
            "rig-dump-state.py no longer says that a census with "
            "missed calls is a lower bound")

    def test_the_partition_identity_holds_by_construction(self):
        # The C++ increments exactly one of the four per transition.
        source = read(os.path.join(
            ROOT, "hypervisor", "src", "hypervisor", "nested_entry.cpp"))
        start = source.index("vtl_code0_run_current[cpu]")
        window = source[start - 2000:start + 2000]
        # The declarations wrap at 75 columns, so `+= 1` can sit on the
        # next line. Matching the literal would make this a formatting
        # test rather than a partition test.
        for name in ("vtl_code0_consecutive", "vtl_code0_same",
                     "vtl_code0_back", "vtl_code0_skip"):
            self.assertTrue(
                re.search(re.escape(name) + r"\[cpu\]\s*\+= 1", window),
                "nested_entry.cpp no longer increments {} on the step "
                "partition, so the reader's identity cannot "
                "hold".format(name))


class SecureCallBlockIsDecodedNotGuessed(unittest.TestCase):
    """The secure call header, and the two guesses it retires.

    `hypervisor.h` carried two competing readings of the quadword at
    `block+0x00` - "byte 0 is a subcode and `0x01010002` means a PFN
    request", and "byte 0 is the operation and bytes 2-3 are a count".
    Both are wrong, and `ntoskrnl.exe` says so directly:
    `VslpEnterIumSecureMode` writes `block+0x00 = (BYTE)arg1` and
    `block+0x02 = (WORD)arg2`, and every caller memsets the block to
    zero first.

    So bytes 2-3 are a **secure service number**, and the two readings
    above are two ways of splitting a field that is not there.

    The consequence that matters: `0x0101` is `VslSetPlaceholderPages`
    (caller `MiUpdateSlabPagePlaceholderState`) and `0x00f4` is
    `VslCopyProtectedPage` (caller `MiCopyPage`). Those are different
    walks. Every "the walk finished" reading in this investigation was
    taken through the `0x01010002` filter, which selects the first and
    is blind to the second - and the second is the one the failing
    stack names.
    """

    # From the census in the dump this was decoded against.
    POPULATION = {
        0x00f40002: 10172,
        0x01010002: 7207,
        0x00f30002: 2926,
        0x00000000: 365,
        0x00d30002: 87,
    }

    @staticmethod
    def decode(word):
        return (word & 0xff, (word >> 8) & 0xff, (word >> 16) & 0xffff)

    def test_the_decode_explains_every_member_of_the_population(self):
        for word in self.POPULATION:
            klass, reason, service = self.decode(word)
            self.assertIn(
                klass, (0, 2),
                "call class {} for 0x{:08x} is outside the range "
                "`VslpEnterIumSecureMode` accepts".format(klass, word))
            self.assertEqual(
                0, reason,
                "byte 1 of 0x{:08x} is not zero, but every caller "
                "memsets the block before the call".format(word))
            self.assertLess(service, 0x120)

    def test_the_filter_selects_the_placeholder_walk_not_the_image_walk(self):
        """NEGATIVE CONTROL - the filter's blind spot, measured.

        A check that cannot fail is not a check. This one asserts that
        the `0x01010002` filter really does miss the majority of the
        traffic, using the counts from the run that was read as "the
        walk finished".
        """
        selected = self.POPULATION[0x01010002]
        missed = sum(n for w, n in self.POPULATION.items()
                     if w != 0x01010002)
        self.assertGreater(
            missed, selected,
            "the population no longer shows the filter missing more "
            "than it selects, so this control has lost its power")
        self.assertEqual(
            0x00f4, self.decode(0x00f40002)[2],
            "the largest missed population is no longer service 0x0f4")

    def test_the_header_records_the_decode(self):
        source = read(HEADER)
        self.assertIn(
            "vtl_service_calls", source,
            "hypervisor.h no longer censuses the secure service number, "
            "so the request word is back to being guessed")
        self.assertIn(
            "VslCopyProtectedPage", source,
            "hypervisor.h no longer names the service behind the 48% of "
            "traffic nothing decoded")
        self.assertIn(
            "vtl_copy_calls", source,
            "hypervisor.h no longer tracks the image validation walk "
            "separately from the placeholder walk")

    def test_the_service_census_cannot_saturate_silently(self):
        source = read(HEADER)
        slots = cxx_constant(source, "vtl_service_slots")
        self.assertGreater(
            slots, 0x116,
            "the service table is narrower than the highest service "
            "observed in ntoskrnl.exe's call sites (0x116), so real "
            "services would land in the overflow counter")
        self.assertIn(
            "vtl_service_other", source,
            "the service census has no overflow counter, which is the "
            "defect `l1_vmcall_code_other` was added to fix once")

    def test_the_per_cpu_hypercall_census_has_an_overflow_counter(self):
        source = read(HEADER)
        self.assertIn(
            "l2_hypercall_cpu_other", source,
            "the per-processor hypercall census has no overflow "
            "counter, so a saturated table reads exactly like a quiet "
            "one - which is what `l2_hypercall_code_counts` does today")
        self.assertIn(
            "l2_hypercall_epoch_delta", source,
            "nothing differences the hypercall census over an epoch, so "
            "'what is still being called' cannot be answered from one "
            "dump")


class EpochLengthIsMeasuredNotAssumed(unittest.TestCase):
    """An epoch is 2^34 ticks only while hypercalls keep arriving.

    `vtl_code0_epoch_*` samples on the **hypercall** path when
    `since >= vtl_code0_epoch_ticks`. When hypercalls stop, no sample
    is taken, so the epoch stretches - and a delta read as "per epoch"
    then understates a silence by exactly the stretch.

    The run this was written against had adjacent samples at
    372,630,200,629 and 529,099,445,053: a gap of 9.1 thresholds. Read
    as one epoch that is "50 calls per epoch"; read honestly it is a
    70-second stretch in which no second-level hypercall arrived at all
    on that processor.
    """

    THRESHOLD = 1 << 34
    SAMPLES = (372630200629, 529099445053)

    def test_the_observed_gap_is_many_thresholds(self):
        gap = self.SAMPLES[1] - self.SAMPLES[0]
        self.assertGreater(
            gap / float(self.THRESHOLD), 9.0,
            "the gap this check was written against is no longer many "
            "thresholds wide, so it no longer demonstrates the stretch")
        silence = gap - self.THRESHOLD
        self.assertGreater(
            silence / 2e9, 60.0,
            "the implied silence is under a minute, so the reading "
            "'calls keep arriving' would not be misleading")

    def test_a_nominal_epoch_overstates_the_rate(self):
        """NEGATIVE CONTROL - the wrong divisor, and by how much."""
        gap = self.SAMPLES[1] - self.SAMPLES[0]
        delta = 392
        honest = delta * 2e9 / gap
        nominal = delta * 2e9 / float(self.THRESHOLD)
        self.assertGreater(
            nominal / honest, 9.0,
            "dividing by the nominal threshold no longer overstates "
            "the rate, so this control has lost its power")

    def test_the_reader_divides_by_the_measured_gap(self):
        source = read(DUMP_STATE)
        self.assertIn(
            "threshold", source,
            "rig-dump-state.py no longer reports the epoch gap in "
            "units of the sampling threshold, so a stretched epoch "
            "reads as a normal one")
        self.assertIn(
            "l2_hypercall_epoch_span", source,
            "rig-dump-state.py no longer reads the measured epoch "
            "span, so any rate it prints uses an assumed divisor")


class ServiceZeroIsReEntriesNotFlushEntireTb(unittest.TestCase):
    """A stuck secure call is counted once, and lands on service 0.

    `VslpEnterIumSecureMode` re-enters VTL1 through `0038df53`, which
    writes `movb $0x0,(%rbx)` and `movw %r8w,0x2(%rbx)` - call class 0
    and service number **0** - before jumping back to the call site.
    Every entry reason that does not return to the caller funnels into
    it, including reason 4, which is not in the case list at all
    (`0038e0d9 cmpb $0x5,%cl; jne 0038df53`).

    Two consequences the existing census cannot show:

    - a call that never returns is counted **once** by service number,
      so "0x0003 VslFinishStartSecureProcessor, called exactly once" is
      equally consistent with "returned immediately" and "has been
      stuck since the moment it was issued";
    - every re-entry is added to service `0x0000`, whose name in the
      reader is `VslFlushEntireTb`.

    The arithmetic below settles the second from numbers that were
    **already measured**, with no new boot: the `code 0` word
    population recorded 365 blocks at `0x00000000` - class 0, reason 0,
    service 0 - and the reason histogram recorded 1,158 at reason 4,
    and 365 + 1,158 is the whole of the 1,520 counted at service
    `0x0000`. `VslFlushEntireTb` issues class **3**
    (`0058a251 xorl %edx,%edx`, `0058a25b movb $0x3,%cl`), so its own
    word would be `0x00000003` and it is nowhere in that population.
    """

    SERVICE_ZERO = 1520
    REASON_FOUR = 1158
    CLASS0_REASON0_WORD = 365

    def test_re_entries_account_for_the_whole_of_service_zero(self):
        accounted = self.CLASS0_REASON0_WORD + self.REASON_FOUR
        self.assertLess(
            abs(accounted - self.SERVICE_ZERO), 0.02 * self.SERVICE_ZERO,
            "re-entries no longer account for service 0x0000 within 2%, "
            "so the arithmetic this decode rests on has changed")

    def test_naming_service_zero_flushentiretb_mislabels_nearly_all(self):
        """NEGATIVE CONTROL - the size of the existing mislabel.

        A check that cannot fail is not a check. This one measures how
        much of the population the current name misattributes: if the
        residue left for `VslFlushEntireTb` were large, naming it that
        would be defensible and this control would have no power.
        """
        residue = self.SERVICE_ZERO - (self.CLASS0_REASON0_WORD
                                       + self.REASON_FOUR)
        self.assertLess(
            abs(residue), 0.05 * self.SERVICE_ZERO,
            "the residue left for VslFlushEntireTb is now large enough "
            "that naming service 0x0000 after it is defensible, so this "
            "control has lost its power")

    def test_the_class_field_is_what_separates_them(self):
        """The one field that tells the two populations apart.

        `VslFlushEntireTb` passes class 3; a re-entry carries class 0.
        Without the class the two are the same service number and no
        amount of counting separates them - which is the
        `census two fields, not one` rule applied to this census.
        """
        source = read(HEADER)
        self.assertIn(
            "vtl_class0_with_service", source,
            "the header no longer carries the check that class 0 never "
            "accompanies a service number, so the class-0 marker is "
            "trusted without anything able to contradict it")
        self.assertIn(
            "0058a25b", source,
            "the header no longer cites the instruction that shows "
            "VslFlushEntireTb issuing call class 3, so 'class separates "
            "them' is back to being asserted rather than looked up")

    def test_reason_four_falls_off_the_end_of_the_dispatch(self):
        source = read(HEADER)
        for site in ("0038df01", "0038e0d9", "0038df53"):
            self.assertIn(
                site, source,
                "the header no longer cites {} , so the claim that "
                "reason 4 is unhandled and re-enters silently is not "
                "checkable".format(site))

    def test_the_collection_site_partitions_every_block(self):
        source = read(os.path.join(
            ROOT, "hypervisor", "src", "hypervisor", "nested_entry.cpp"))
        for name in ("vtl_fresh_calls", "vtl_reentries"):
            self.assertTrue(
                re.search(re.escape(name) + r"\[cpu\]\s*\+= 1", source),
                "nested_entry.cpp no longer increments {}, so the "
                "reader's fresh + re-entry == blocks identity cannot "
                "hold".format(name))

    def test_the_ring_is_printed_newest_last_not_in_slot_order(self):
        """NEGATIVE CONTROL - slot order against chronological order.

        A circular ring printed in raw slot order has been read as a
        sequence twice in this investigation. This asserts the two
        orders genuinely differ for a wrapped ring, so the reader's
        `range(rc - n, rc)` is doing work rather than agreeing with the
        naive loop by accident.
        """
        source = read(HEADER)
        slots = cxx_constant(source, "vtl_reentry_ring_slots")
        count = 1158
        chronological = [k % slots for k in range(count - slots, count)]
        self.assertNotEqual(
            chronological, list(range(slots)),
            "slot order and chronological order agree for this ring, so "
            "this control cannot catch the bug it exists for")
        self.assertEqual(
            sorted(chronological), list(range(slots)),
            "the chronological walk no longer visits every slot exactly "
            "once")
        dump = read(DUMP_STATE)
        self.assertIn(
            "range(rc - n, rc)", dump,
            "rig-dump-state.py no longer walks the re-entry ring from "
            "its oldest slot, so it prints a circular buffer as though "
            "it were a list")

    def test_the_vtl1_duration_histogram_reaches_a_whole_second(self):
        """A saturating top bucket answers every question with itself.

        At 24 buckets everything from 2^23 ticks up - 4.2 ms at
        1.992 GHz - was one column, and the round trip being chased
        takes about a second (2^31 ticks). The histogram would have
        reported "4.2 ms or more" and that is not an answer.
        """
        header = read(HEADER)
        buckets = cxx_constant(header, "vtl1_duration_buckets")
        ghz = 1.992e9
        self.assertGreater(
            (1 << (buckets - 1)) / ghz, 1.0,
            "the top bucket saturates below one second, so the "
            "one-per-second round trip cannot be distinguished from a "
            "fast return")
        dump = read(DUMP_STATE)
        self.assertEqual(
            buckets, int(re.search(
                r"VTL1_DURATION_BUCKETS = (\d+)", dump).group(1)),
            "rig-dump-state.py's stride disagrees with the header's "
            "width, so the histogram is walked into its neighbour")

    def test_a_stride_of_twenty_four_would_now_be_wrong(self):
        """NEGATIVE CONTROL - measured, in words of overrun.

        If the two widths ever agreed at 24 again this check would be
        vacuous, so it asserts the width really did move and by how
        much a stale stride would overrun.
        """
        buckets = cxx_constant(read(HEADER), "vtl1_duration_buckets")
        self.assertGreater(
            buckets, 24,
            "the width is back at 24, so this control has no power")
        overrun = 2 * (buckets - 24)
        self.assertGreaterEqual(
            overrun, 8,
            "a stale stride of 24 would overrun by fewer than eight "
            "words, which is small enough to look like plausible data")

    def test_the_reader_charges_re_entries_to_a_call(self):
        dump = read(DUMP_STATE)
        for name in ("vtl_reentry_service", "vtl_reentry_by_reason",
                     "vtl_reentry_block_same", "vtl_reentry_orphan"):
            self.assertIn(
                name, dump,
                "rig-dump-state.py no longer reads {}, so a stuck call "
                "is still invisible to every reader in this "
                "tree".format(name))


def load_dump_state():
    """`rig-dump-state.py` as a module, despite the hyphen in its name."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("rds", DUMP_STATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TrustLevelRoundTripIsAMeanNotARate(unittest.TestCase):
    """The halves are a whole-boot mean, and the reader said "costs".

    `mark_vtl_half` (`nested_entry.cpp:8543`) accumulates from the first
    switch of the boot and never resets, so `vtl_half_cycles / count` is
    a mean over every round trip that ever happened. The reader printed
    it under "what one trust-level round trip costs", which reads as the
    present tense, and `0392123` quoted it as the current cost.

    The numbers below are the measured ones from that dump.
    """

    GHZ = 1992000000.0

    def halves(self, us0, us1, count):
        return [(count, int(us0 * 1992.0) * count, 0),
                (count, int(us1 * 1992.0) * count, 0)]

    def test_the_measured_dump_disagrees_by_fourteen_times(self):
        """MEASURED: 0392123's own two fields, divided against each other.

        1,531 us + 11,358 us over 22,324 round trips is 77.6 a second.
        The `HvCallVtlCall` epoch in the same dump is +47 over 8.6 s,
        which is 5.5 a second. Both were printed; neither was divided.
        """
        module = load_dump_state()
        lines = module.vtl_round_trip_verdict(
            self.halves(1531.0, 11358.0, 22324), self.GHZ,
            47, int(8.6 * self.GHZ))
        text = "\n".join(lines)
        self.assertIn(
            "DISAGREE", text,
            "the reader accepts a whole-boot mean as the current cost, "
            "which is exactly how 90.8 exits per round trip was quoted "
            "for a phase that had ended")
        self.assertIn("14.2x", text, "the measured ratio moved")
        self.assertIn(
            "BOOT-WIDE MEANS", text,
            "the mean is still printed as though it were a rate")

    def test_agreeing_rates_stay_silent(self):
        """NEGATIVE CONTROL - measured, and it must NOT fire.

        A check that fires on every input is not a check. Here the
        halves are made to imply the rate the epoch reports, and the
        verdict has to say so and say nothing else - otherwise the
        DISAGREE above is an artefact of the instrument rather than a
        fact about the dump.

        12,889 us a round trip is 77.6/s; the epoch is given +776 over
        10 s to match it.
        """
        module = load_dump_state()
        lines = module.vtl_round_trip_verdict(
            self.halves(1531.0, 11358.0, 22324), self.GHZ,
            776, int(10.0 * self.GHZ))
        text = "\n".join(lines)
        self.assertIn(
            "AGREE", text,
            "the verdict cannot stay quiet on agreeing rates, so its "
            "DISAGREE carries no information")
        self.assertNotIn("DISAGREE", text)

    def test_a_missing_epoch_never_claims_agreement(self):
        """The third outcome, which is not the other two.

        No epoch is not agreement. A reader that printed nothing here
        would leave the mean looking checked when it had not been.
        """
        module = load_dump_state()
        text = "\n".join(module.vtl_round_trip_verdict(
            self.halves(1531.0, 11358.0, 22324), self.GHZ, 0, 0))
        self.assertNotIn("AGREE", text)
        self.assertIn("cannot say", text)

    def test_no_halves_prints_nothing(self):
        module = load_dump_state()
        self.assertEqual(
            [], module.vtl_round_trip_verdict(
                [(0, 0, 0), (0, 0, 0)], self.GHZ, 47, 1000))

    def test_the_reader_still_calls_the_verdict(self):
        """The wiring, not the arithmetic.

        The helper being correct is worth nothing if `dump_priority`
        stops calling it, and that is a one-line deletion away.
        """
        dump = read(DUMP_STATE)
        self.assertIn(
            "vtl_round_trip_verdict(", dump)
        self.assertGreaterEqual(
            dump.count("vtl_round_trip_verdict"), 3,
            "the verdict is defined but no longer called from the "
            "half-printing site")
        for name in ("l2_hypercall_epoch_delta", "l2_hypercall_epoch_span",
                     "l2_hypercall_cpu_codes"):
            self.assertIn(
                name, dump,
                "the second field the halves are checked against is no "
                "longer read, so the check is vacuous")


def header_dimensions(name):
    """A member's declared array dimensions, as a list of bound strings.

    `[]` for a plain scalar, `["max_cpus"]` for a per-processor row.
    Raises when the member is not declared at all, which is the negative
    control this family needs: every silent failure in this tree began
    with a lookup that returned nothing and a caller that carried on.
    """
    source = read(HEADER)
    match = re.search(
        r"^[ \t]*(?:static\s+)?(?:constinit\s+)?(?:volatile\s+)?"
        r"(?:std::uint(?:8|16|32|64)_t|std::size_t|bool)\s+"
        + re.escape(name) + r"((?:\s*\[[^\]]*\])*)\s*(?:\{|=|;)",
        source, re.M)
    if not match:
        raise AssertionError(
            "no declaration of `{}` in hypervisor.h - the reader "
            "differences a member the header does not have".format(name))
    return [b.strip() for b in re.findall(r"\[([^\]]*)\]", match.group(1))]


class TheClockGapHistogramCannotReportItsOwnAbsence(unittest.TestCase):
    """`clock_gap_buckets`, and the three ways its label was wrong.

    The reading it produced - "96.3% of gaps in the bucket holding the
    guest's 1.74 ms period, so the period is met" - was quoted in
    CLAUDE.md as a statement about a guest that was making no progress
    at all. Three separate faults, each of which this class pins:

    1.  **It counts stagings, not arrivals.** The increment is in
        `build_vmcs02` on the value copied out of *vmcs12*
        (`nested_entry.cpp:3251`), which is what the level above asked
        for. `l2_entry_vector` exists because that disagrees with what
        the entry carries, and `hypervisor.h` records the rig showing
        `0xd1` "injected 52,799 times ... and a second-level guest that
        never vectored once".
    2.  **It counts one hardcoded vector.** Nothing checked that the
        guest programmed that vector into the interrupt source its timer
        posts to, and the value it did program has been recorded all
        along at `synthetic_msr_last_value[cpu][0x93]`.
    3.  **It survives the event stream ending.** A histogram of
        intervals cannot record the interval it is inside, so a clock
        that stops leaves the distribution frozen and still reading
        96.3% for ever. A stall that *ends* contributes one count in one
        bucket, which rounds away.

    Fault 3 is the one with no fix but measurement, so the reader now
    states its own coverage and `--delta` differences the buckets.
    """

    def test_the_reader_and_the_header_agree_on_the_counted_vector(self):
        """A constant copied into the reader that does not move.

        The same failure `gdb_lengths` exists for, one array over: the
        header owns `clock_gap_vector` and the reader prints a verdict
        against it.
        """
        module = load_dump_state()
        match = re.search(
            r"static\s+constexpr\s+std::uint64_t\s+clock_gap_vector"
            r"\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", read(HEADER))
        self.assertIsNotNone(
            match, "hypervisor.h no longer declares clock_gap_vector, so "
                   "the reader's verdict is against nothing")
        self.assertEqual(int(match.group(1), 16 if
                             match.group(1).startswith("0x") else 10),
                         module.CLOCK_GAP_VECTOR)

    def test_the_sint3_slot_is_read_so_the_vector_can_be_checked(self):
        """0x93 is in the slice, and its last value is read as state."""
        module = load_dump_state()
        self.assertIn(0x93, [s for s, _ in module.DELTA_SYNTHETIC_SLOTS])
        self.assertIn(0x93, module.DELTA_SYNTHETIC_STATE_SLOTS)

    def test_a_disagreeing_sint3_vector_is_called_out(self):
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0x93): 0x00a5}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("vector 0xa5", text)
        self.assertIn("DISAGREES", text)

    def test_an_agreeing_sint3_vector_is_not_called_out(self):
        """The negative control for the check above."""
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0x93):
                 module.CLOCK_GAP_VECTOR}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("AGREES with clock_gap_vector", text)
        self.assertNotIn("DISAGREES", text)

    def test_an_unwritten_sint3_is_unchecked_and_says_so(self):
        """Silence is not agreement.

        A processor on which no `wrmsr 0x40000093` was seen must not
        read as "the vector is confirmed" - it is the case where nothing
        checked it, which is what the whole class is about.
        """
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0xb1): 17400}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("UNCHECKED", text)
        self.assertNotIn("AGREES", text)

    def test_direct_mode_is_decoded_from_bit_twelve(self):
        """`0x30008` is message mode to SINT3, and that is the whole
        question the number was being asked.

        Bit layout from Linux's `union hv_stimer_config`, which is what
        `.references/kvm/hyperv.c` indexes at lines 233, 696-706 and
        812-854. `0x30008` therefore reads: not enabled, not periodic,
        auto-enable set, **direct_mode clear**, sintx 3.
        """
        module = load_dump_state()
        bits = module.stimer_config_decode(0x30008)
        self.assertEqual(0, bits["direct_mode"])
        self.assertEqual(3, bits["sintx"])
        self.assertEqual(1, bits["auto_enable"])
        self.assertEqual(0, bits["periodic"])
        self.assertEqual(0, bits["enable"])
        # And the positive control, so a decoder that returns zero for
        # everything cannot pass the assertion above.
        self.assertEqual(1, module.stimer_config_decode(
            0x30008 | (1 << 12))["direct_mode"])

    def test_message_mode_says_the_two_rates_must_agree(self):
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0xb0): 0x30008}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("message mode to SINT3", text)
        self.assertNotIn("DIRECT MODE", text)

    def test_direct_mode_retires_every_reading_of_the_message_page(self):
        """The negative control for the line above."""
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0xb0):
                 0x30008 | (1 << 12)}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("DIRECT MODE", text)
        self.assertIn("NO message", text)

    def test_the_two_payload_fields_are_not_declared_a_latency(self):
        """`delivery_time - expiration_time` needs one clock, not two.

        A stable 7.2078 s was read as a delivery latency. For a one-shot
        arm the expiry is the deadline the *guest* computed, and this
        VMM publishes its own reference-TSC page into the address the
        guest named (`publish_reference_tsc_page`), so the two fields
        need not share an epoch - and a latency that is constant to four
        decimals is the one thing a latency is not.
        """
        module = load_dump_state()
        after = {("state", "synthetic_msr_last_value", 0, 0x83): 0x1000 | 1}
        text = "\n".join(module.delta_synic_lines(after, 1))
        self.assertIn("only a latency if", text)
        self.assertIn("epoch", text)

    def test_the_buckets_are_differenced_rather_than_refused(self):
        """The positive control: it is out of the refusal list *and* in
        the histogram list. Removing it from one alone leaves a reader
        that neither reports it nor says it declined to."""
        module = load_dump_state()
        named = [n for n, _ in module.DELTA_PER_CPU_HISTOGRAMS]
        self.assertIn("clock_gap_buckets", named)
        for _what, names, _why in module.DELTA_REFUSALS:
            self.assertNotIn("clock_gap_buckets", names)

    def test_a_histogram_that_covers_the_run_is_not_complained_about(self):
        """The negative control for the coverage check.

        Without this, a check that always complains passes the test
        below and tells the next reader nothing.
        """
        module = load_dump_state()
        hz = module.TSC_HZ
        # 574 gaps of about 1.74 ms each is one second, measured against
        # a one second run.
        lines = "\n".join(module.clock_gap_coverage_lines(
            [(21, 574)], hz, int(574 * 1.5 * (1 << 21))))
        self.assertIn("accounts for the run", lines)
        self.assertNotIn("DOES NOT COVER", lines)

    def test_a_histogram_that_covers_a_tenth_of_the_run_says_so(self):
        """MEASURED shape: 241,551 gaps at about 1.6 ms is 420 s of
        clock, and the guest whose reference counter read 4,900 s was
        reported from it as meeting its period."""
        module = load_dump_state()
        hz = module.TSC_HZ
        covered_ticks = int(241551 * 1.5 * (1 << 21))
        lines = "\n".join(module.clock_gap_coverage_lines(
            [(21, 232690), (22, 4139), (21, 4722)], hz, covered_ticks * 10))
        self.assertIn("DOES NOT COVER", lines)
        self.assertIn("cannot", lines)

    def test_coverage_above_the_run_is_an_error_not_a_number(self):
        """Gaps between successive events cannot outlast the run.

        The case that produces it is the one this tree keeps hitting: a
        reader pointed at a rebuilt ELF against a deployed older binary,
        which reads plausible garbage rather than failing.
        """
        module = load_dump_state()
        lines = "\n".join(module.clock_gap_coverage_lines(
            [(21, 1000)], module.TSC_HZ, 1 << 21))
        self.assertIn("IMPOSSIBLE", lines)
        self.assertNotIn("accounts for the run", lines)

    def test_no_span_produces_no_verdict(self):
        """A denominator that was not read is not a small denominator."""
        module = load_dump_state()
        self.assertEqual([], module.clock_gap_coverage_lines(
            [(21, 100)], module.TSC_HZ, 0))
        self.assertEqual([], module.clock_gap_coverage_lines(
            [], module.TSC_HZ, 1 << 40))


class DeltaModeDifferencesOnlyWhatIsMonotonic(unittest.TestCase):
    """What `--delta` will subtract, and what it refuses to.

    A ring slot, a last-value field and a current-state field all look
    exactly like a counter once they are eight bytes in a dump, and
    subtracting any of them produces a number with a plausible magnitude
    and no meaning. The reader's classification is the only thing
    standing between those and a rate, so it is checked here rather than
    trusted.
    """

    def test_every_per_cpu_counter_is_declared_per_cpu(self):
        """The `hypercalls_seen` shape, caught by construction.

        `hypercalls_seen` is a bare `std::uint64_t` and the cumulative
        reader queues it at the processor count - harmless only because
        it happens to read index 0. A member differenced at the wrong
        width reads its *neighbour* and reports it under this one's
        name, and this file already records that costing a run.
        """
        module = load_dump_state()
        per_cpu = ([n for n, _ in module.DELTA_PER_CPU_COUNTERS]
                   + [n for n, _ in module.DELTA_PER_CPU_CYCLES]
                   + [module.DELTA_CLOCK, module.DELTA_FINGERPRINT])
        for name in per_cpu:
            self.assertEqual(
                ["max_cpus"], header_dimensions(name),
                "{} is differenced once per processor but is not "
                "declared [max_cpus]".format(name))

    def test_every_global_counter_is_declared_as_one_word(self):
        module = load_dump_state()
        for name, _ in module.DELTA_GLOBAL_COUNTERS:
            self.assertEqual(
                [], header_dimensions(name),
                "{} is read as a single word but the header declares it "
                "as an array - reading index 0 of it is one processor's "
                "share reported as the whole".format(name))

    def test_no_ring_or_state_member_is_differenced(self):
        """The refusal, as a list this test can fail on.

        Each of these is a member the cumulative reader already prints,
        each is eight bytes wide, and each would subtract without
        complaint.
        """
        module = load_dump_state()
        differenced = set(
            [n for n, _ in module.DELTA_PER_CPU_COUNTERS]
            + [n for n, _ in module.DELTA_GLOBAL_COUNTERS]
            + [n for n, _ in module.DELTA_PER_CPU_CYCLES])
        forbidden = [
            # Ring buffers: the same slot holds two unrelated records.
            "exit_trace", "l2_exit_trace", "l2_working_trace",
            "cpuid_trace", "vtl_code0_ring", "vtl_reentry_ring",
            "guest_stack_trace", "guest_interrupted_trace",
            # Last-value fields: a difference of two addresses.
            "nested_last_vmfail", "ipi_last_command",
            "last_hypercall_code", "last_hypercall_rcx",
            "last_hypercall_rdx", "last_hypercall_r8",
            "last_hypercall_tsc", "vtl_protect_last_rip",
            "vtl_protect_last_cr3", "vtl_copy_last_pfn",
            "ap_probe_rip", "ap_probe_cs", "profile_code_physical",
            # Current-state fields.
            "l2_activity_state", "pending_event", "running_l2",
            "shadow_ept_current_slot", "resume_activity_state",
            "processor_virtualized", "l1_own_cr3", "l2_exit_cr3",
            "host_page_table", "guest_kernel_base", "vtl_block_page",
            "watched_apic_page", "ap_probe_activity",
            # Min/max accumulators: monotonic and not counts.
            "vtl_code0_min_pfn", "vtl_code0_max_pfn",
            "vtl_copy_min_pfn", "vtl_copy_max_pfn",
            "vtl_code0_run_longest",
            # State histograms.
            "cpl_seen", "guest_leaf_permissions",
            "shadow_leaf_permissions", "vtl_protect_host_perms",
            "vtl_protect_guest_perms",
        ]
        for name in forbidden:
            self.assertNotIn(
                name, differenced,
                "{} is not a monotonic event count and must not be "
                "subtracted".format(name))

    def test_the_clock_is_never_rated_as_a_counter(self):
        """`handler_last_tsc` is the span, not a quantity per second.

        It is a last value. Its difference is the window; its difference
        divided by the window is 1.0 and means nothing.
        """
        module = load_dump_state()
        rated = set([n for n, _ in module.DELTA_PER_CPU_COUNTERS]
                    + [n for n, _ in module.DELTA_GLOBAL_COUNTERS])
        self.assertNotIn(module.DELTA_CLOCK, rated)
        self.assertNotIn(module.DELTA_FINGERPRINT, rated)

    def test_the_refusals_are_printed_with_their_reasons(self):
        """Silence from an instrument is not a measurement.

        A member left out and a member that never existed look the same
        in the output, so the report says what it declined and why.
        """
        module = load_dump_state()
        self.assertTrue(module.DELTA_REFUSALS)
        for what, names, why in module.DELTA_REFUSALS:
            self.assertTrue(what and names and why)
        text = "\n".join(module.delta_report(
            {}, {}, [], ({}, {}, []), [],
            module.delta_span(100, 200, 1.0),
            ({"base": 1, "first_tsc": (2,)},
             {"base": 1, "first_tsc": (2,)}),
            1, 1.0, (0.1, 0.1)))
        self.assertIn("REFUSED to difference", text)
        for what, _names, _why in module.DELTA_REFUSALS:
            self.assertIn(what, text)


class TheExitHistogramCannotSayWhoseExitItWas(unittest.TestCase):
    """`exit_reason_counts` mixes both levels, and the split that does
    not was read cumulatively only.

    `record_exit` runs once per exit at the top of the handler, for the
    guest hypervisor and for its guest alike, and counts into one row -
    so a `wrmsr` bucket is the sum of two populations with opposite
    consequences.  A second-level `wrmsr` is reflected and answered with
    a `VMRESUME` that comes straight back, so it is one round trip
    costing two exits; a first-level one is a single exit.  Halving the
    first halves two counts.

    `handler_reason_exits` and `handler_reason_from_l2` are the members
    that separate them, they have been resident all along, and
    `dump_handler_by_reason` printed them **cumulatively**.  Its own
    output quotes `vmresume at 38% of exits and vmptrld at 8%` - a mean
    over a configuration that no longer exists, since VMCS shadowing was
    not in force when it was taken.  That is the same shape as the 96.3%
    this file already has a class for.

    The measured window these numbers come from: 31.2 s, one processor,
    nesting on, shadowing in force, `exit_total` 8,189.92/s and
    `l2_entries` 4,088.98/s - which is 2.00 exits per second-level
    entry, and that ratio is the whole reason a split is worth having.
    """

    SECONDS = 31.2
    ENTRIES = 127_576                       # 4,088.98/s x 31.2 s
    # 255,525 total, which is 8,189.92/s over the same span.
    WINDOW = {
        24: (127_576, 0),                   # vmresume: the level above
        32: (90_000, 90_000),               # wrmsr:    all second level
        31: (37_200, 37_200),               # rdmsr:    all second level
        18: (168, 168),                     # vmcall
        12: (208, 208),                     # hlt
        21: (373, 0),                       # vmptrld:  the level above
    }

    def samples(self, window=None, slots=64, base=1_000_000):
        """`(before, after)` for one window, with a non-zero baseline.

        The baseline matters: a member that reads zero in both samples
        and a member that was never read are different facts, and a test
        starting from zero cannot tell them apart either.
        """
        window = self.WINDOW if window is None else window
        before, after = {}, {}
        for reason in range(slots):
            total, l2 = window.get(reason, (0, 0))
            before[("handler_reason_exits", reason)] = base
            before[("handler_reason_from_l2", reason)] = base // 2
            after[("handler_reason_exits", reason)] = base + total
            after[("handler_reason_from_l2", reason)] = base // 2 + l2
        return before, after

    def split(self, before, after, slots=64, entries=None):
        module = load_dump_state()
        return "\n".join(module.delta_level_split_lines(
            before, after, slots, self.SECONDS,
            self.ENTRIES if entries is None else entries))

    def test_the_split_members_are_one_row_indexed_by_reason(self):
        """Not `[max_cpus]`, and the reader must not read them as such.

        The stride bug this file already records - a 96-entry row read
        at a stride of 64 - cancelled for processor 0 and printed a
        coherent histogram of instructions the guest never executes for
        every other.  These two are global rows, so a reader that
        queued them per processor would read the *next member* and print
        it under this name.
        """
        module = load_dump_state()
        for name, _ in module.DELTA_GLOBAL_HISTOGRAMS:
            self.assertEqual(
                ["handler_reason_slots"], header_dimensions(name),
                "{} is read as one global row indexed by exit reason "
                "but the header does not declare it that way".format(
                    name))

    def test_the_split_is_not_also_differenced_as_a_counter(self):
        """One member, one classification.

        In `DELTA_PER_CPU_COUNTERS` it would be read at index `cpu` -
        i.e. reason 0 and reason 1 reported as two processors' exits.
        In `DELTA_GLOBAL_COUNTERS` it would be read as one word, i.e.
        reason 0 reported as the whole.
        """
        module = load_dump_state()
        elsewhere = set([n for n, _ in module.DELTA_PER_CPU_COUNTERS]
                        + [n for n, _ in module.DELTA_GLOBAL_COUNTERS]
                        + [n for n, _ in module.DELTA_PER_CPU_CYCLES]
                        + [n for n, _ in module.DELTA_PER_CPU_HISTOGRAMS])
        for name, _ in module.DELTA_GLOBAL_HISTOGRAMS:
            self.assertNotIn(name, elsewhere)

    def test_the_idle_and_reference_counts_are_rated_per_processor(self):
        """`hlt_reflect_count` is the one number that separates the two
        remaining accounts of the stall.

        A thread blocked in a wait puts the processor on the idle
        thread, which halts, and the guest hypervisor sets HLT exiting
        so that halt is an exit counted here.  A thread spinning does
        not halt.  Every other counter in this file reads the same
        either way.
        """
        module = load_dump_state()
        rated = [n for n, _ in module.DELTA_PER_CPU_COUNTERS]
        for name in ("hlt_reflect_count", "reference_read_count"):
            self.assertIn(name, rated)
            self.assertEqual(["max_cpus"], header_dimensions(name))

    def test_the_split_names_both_levels_and_the_round_trip(self):
        """The positive control, on the measured steady state."""
        text = self.split(*self.samples())
        self.assertIn("by LEVEL IN THIS WINDOW", text)
        self.assertIn("255,525", text)                 # the whole window
        self.assertIn("127,576", text)                 # entries, and L2
        # wrmsr is entirely second level, so its L1 column is zero and
        # its L2 column is the bucket.
        self.assertRegex(text, r"wrmsr\s+90,000\s+0\s+90,000")
        # vmresume is entirely the level above's.
        self.assertRegex(text, r"vmresume\s+127,576\s+127,576\s+0")
        self.assertIn("the round trip, as an identity", text)
        # And the residue, which is the number the decomposition is for.
        self.assertRegex(text, r"everything else\s+373")
        self.assertIn("AGREES with vmlaunch+vmresume", text)

    def test_a_subset_larger_than_its_superset_is_refused(self):
        """**The negative control.**

        `handler_reason_from_l2` is incremented inside the `if` that
        increments `handler_reason_exits`, over one span, so it cannot
        exceed it.  If it does, the two were read at different strides
        or from different binaries.

        Without this the reader prints an L1 column of `-5,000` and a
        second-level share of 105%, and both read as findings: "the
        level above took a negative number of exits" is not a sentence
        anyone would write, but a negative in a column is easy to skim
        past, and this tree has skimmed past worse.
        """
        before, after = self.samples()
        after[("handler_reason_from_l2", 32)] += 5_000   # 95,000 of 90,000
        text = self.split(before, after)
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("SUBSET exceeds its superset", text)
        self.assertIn("wrmsr", text)
        # And nothing is rated: no table, no identity, no percentage.
        self.assertNotIn("by LEVEL IN THIS WINDOW", text)
        self.assertNotIn("the round trip", text)
        self.assertNotIn("-5,000", text)

    def test_a_bucket_that_went_backwards_is_refused(self):
        """The other negative control, and the one this mode already
        applies everywhere else."""
        before, after = self.samples()
        after[("handler_reason_exits", 24)] = \
            before[("handler_reason_exits", 24)] - 1
        text = self.split(before, after)
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("BACKWARDS", text)
        self.assertIn("handler_reason_exits[reason 24]", text)
        self.assertNotIn("by LEVEL IN THIS WINDOW", text)

    def test_a_window_in_which_nothing_moved_says_so(self):
        """A table of zeroes is not a distribution.

        This is the failure mode the 96.3% had: a cumulative histogram
        keeps reporting its shape for ever after the event stream ends.
        A windowed one reads all zeroes, and printing them as a table
        with percentages would restore exactly the property that made
        the cumulative reading wrong.
        """
        before, after = self.samples(window={})
        text = self.split(before, after)
        self.assertIn("every reason unchanged in this window", text)
        self.assertNotIn("%", text)

    def test_a_member_that_was_not_read_is_not_read_as_zero(self):
        """An unanswered read and a counter reading zero look identical
        afterwards.  This mode's oldest rule, applied here."""
        before, after = self.samples()
        del after[("handler_reason_from_l2", 32)]
        text = self.split(before, after)
        self.assertIn("NOT READ", text)
        self.assertIn("unknown rather than zero", text)
        self.assertNotIn("by LEVEL IN THIS WINDOW", text)

    def test_the_entry_count_disagreeing_is_named_not_absorbed(self):
        """`hypervisor.h` states VMLAUNCH plus VMRESUME equals
        `l2_entries` when no entry is refused, and that equality is what
        makes the residue meaningful.  A reader that formed the residue
        without checking it would attribute a refused entry to
        'everything else'."""
        text = self.split(*self.samples(), entries=self.ENTRIES - 9)
        self.assertIn("DISAGREES with vmlaunch+vmresume", text)
        self.assertIn("nested_entry_refusals", text)

    def test_the_reasons_above_the_table_are_declared_missing(self):
        """`handler_reason_slots` is 64 and `exit_reason_capacity` is
        96, so the two totals may legitimately differ.  Said out loud,
        because a difference nobody expects gets explained by inventing
        a mechanism."""
        text = self.split(*self.samples())
        self.assertIn("reasons >= 64 are outside this table", text)


class DeltaModeReportsTheImpossibleAsAnError(unittest.TestCase):
    """A monotonic counter that decreased is not a small negative rate.

    This is the measured case. Differencing two dumps by hand during the
    session that produced this mode gave **+427 halves and -11,989
    cycles** for one column (recorded in 8c7dac9). A monotonic
    accumulator cannot go backwards, so that scrape was wrong rather
    than the guest surprising - and the figure it implied, about 4 us,
    was within one step of being written down.

    Four things produce it and all four have happened on this rig: a
    torn read, a wrapped field, a reader pointed at a different binary
    from the one running, and a guest that reset between the samples.
    """

    # The scrape, as it was measured.
    HALVES_BEFORE, HALVES_AFTER = 22_324, 22_751          # +427
    CYCLES_BEFORE, CYCLES_AFTER = 253_320_000, 253_308_011  # -11,989

    def test_the_recorded_hand_scrape_is_refused_not_rated(self):
        module = load_dump_state()
        before = {("vtl_half_count", 0): self.HALVES_BEFORE,
                  ("vtl_half_cycles", 0): self.CYCLES_BEFORE}
        after = {("vtl_half_count", 0): self.HALVES_AFTER,
                 ("vtl_half_cycles", 0): self.CYCLES_AFTER}
        rows, impossible, unread = module.delta_rows(
            before, after,
            [(("vtl_half_count", 0), "halves"),
             (("vtl_half_cycles", 0), "cycles")])
        self.assertEqual([], unread)
        self.assertEqual(1, len(rows))
        self.assertEqual(("vtl_half_count", 0), rows[0][0])
        self.assertEqual(427, rows[0][4])
        self.assertEqual(1, len(impossible))
        self.assertEqual(("vtl_half_cycles", 0), impossible[0][0])
        self.assertEqual(-11_989, impossible[0][4])

    def test_the_error_names_the_member_and_forbids_the_rest(self):
        module = load_dump_state()
        _rows, impossible, _unread = module.delta_rows(
            {("exit_total", 1): 1_000_000},
            {("exit_total", 1): 999_571},
            [(("exit_total", 1), "exits taken")])
        text = "\n".join(module.delta_impossible_lines(impossible))
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("exit_total[cpu 1]", text)
        self.assertIn("-429", text)
        self.assertIn("Do not read", text)
        # And it is NOT presented as a rate. A negative per-second
        # figure is the shape the hand scrape nearly produced.
        self.assertNotIn("per second", text)

    def test_a_counter_that_advanced_is_rated(self):
        """The control that says the detector is not always firing.

        A check that fails on everything catches nothing, and this file
        already records an instrument that "confirmed" the hypothesis
        under test because it was aimed at the wrong field.
        """
        module = load_dump_state()
        rows, impossible, unread = module.delta_rows(
            {("exit_total", 0): 1_000_000},
            {("exit_total", 0): 1_106_380},
            [(("exit_total", 0), "exits taken")])
        self.assertEqual([], impossible)
        self.assertEqual([], unread)
        self.assertEqual(106_380, rows[0][4])
        self.assertEqual([], module.delta_impossible_lines(impossible))

    def test_a_reading_that_never_came_back_is_not_zero(self):
        """An unanswered read and a counter at zero look identical.

        The monitor drops reads - `Monitor.unanswered` exists for it -
        and `words.get(addr, 0)` is how 26 reads once returned 78 words
        of plausible zeroes. A missing sample must be "not read", never
        a delta of zero.
        """
        module = load_dump_state()
        rows, impossible, unread = module.delta_rows(
            {("shadow_ept_builds", 0): 4_242},
            {},
            [(("shadow_ept_builds", 0), "builds")])
        self.assertEqual([], rows)
        self.assertEqual([], impossible)
        self.assertEqual(1, len(unread))
        self.assertEqual(("shadow_ept_builds", 0), unread[0][0])


class DeltaSpanIsMeasuredNeverNominal(unittest.TestCase):
    """`--delta 20` does not mean the span was twenty seconds.

    Each sample takes a measurable time, so the interval the counters
    accumulated over is the *midpoint to midpoint* distance and not the
    sleep. A nominal epoch is already one of the mislabelled readings
    this tree records, in `EpochLengthIsMeasuredNotAssumed` above.
    """

    ASKED = 20.0
    MEASURED = 20.9
    DELTA = 106_380

    def report(self, measured=None, ticks=(0, 0)):
        module = load_dump_state()
        measured = self.MEASURED if measured is None else measured
        span = module.delta_span(ticks[0], ticks[1], measured)
        return module, "\n".join(module.delta_report(
            {("exit_total", 0): 1_000_000},
            {("exit_total", 0): 1_000_000 + self.DELTA},
            [(("exit_total", 0), "exits taken")],
            ({}, {}, []), [], span,
            ({"base": 0x6720f000, "first_tsc": (7,)},
             {"base": 0x6720f000, "first_tsc": (7,)}),
            1, self.ASKED, (1.5, 1.5)))

    def test_the_rate_divides_by_the_measured_span(self):
        _module, text = self.report()
        measured_rate = self.DELTA / self.MEASURED       # 5,089.95
        nominal_rate = self.DELTA / self.ASKED           # 5,319.00
        self.assertIn("{:,.2f}".format(measured_rate), text)
        self.assertNotIn("{:,.2f}".format(nominal_rate), text)

    def test_the_nominal_span_would_overstate_the_rate(self):
        """The size of the error, so it is not dismissed as rounding."""
        self.assertAlmostEqual(
            4.5, 100.0 * (self.MEASURED - self.ASKED) / self.ASKED,
            places=1)
        _module, text = self.report()
        self.assertIn("nominal", text)
        self.assertIn("4.5%", text)

    def test_a_span_that_is_not_positive_rates_nothing(self):
        """A broken measurement, not a quiet guest.

        The two are opposite conclusions and a zero denominator is how
        they get confused - or a ZeroDivisionError, which at least is
        loud.
        """
        _module, text = self.report(measured=0.0)
        self.assertIn("not positive", text)
        self.assertNotIn("per second", text)

    def test_the_frequency_is_measured_when_the_ticks_allow(self):
        module = load_dump_state()
        ticks = int(2_000_000_000 * 20.9)
        _t, _s, hz, source, complaint = module.delta_span(0, ticks, 20.9)
        self.assertAlmostEqual(2_000_000_000, hz, delta=1.0)
        self.assertIn("MEASURED", source)
        self.assertEqual("", complaint)

    def test_the_fallback_constant_says_it_is_a_fallback(self):
        module = load_dump_state()
        _t, _s, hz, source, _c = module.delta_span(None, None, 20.9)
        self.assertEqual(float(module.TSC_HZ), hz)
        self.assertIn("FALLBACK", source)

    def test_a_frequency_far_from_the_measured_constant_complains(self):
        """Two clocks, and the disagreement is the finding.

        A tick span implying 4 GHz on a 1.992 GHz part means one of the
        two clocks is not measuring what its label says, and that is
        worth an error rather than a silently doubled microsecond.
        """
        module = load_dump_state()
        _t, _s, _hz, _source, complaint = module.delta_span(
            0, int(4_000_000_000 * 20.9), 20.9)
        self.assertIn("clocks", complaint)

    def test_the_delta_path_uses_one_frequency_constant(self):
        """1992.0 in six places and 2e9 in four is a 0.4% disagreement.

        It is invisible in any single reading and shifts every derived
        microsecond. The delta path picks the measured one - 1.992 GHz,
        fitted at the wall over a 90.08 s window per BACKLOG.md - and
        the round 2 GHz appears nowhere in it.
        """
        import inspect
        module = load_dump_state()
        self.assertEqual(1_992_000_000, module.TSC_HZ)
        for function in (module.delta_span, module.delta_report,
                         module.delta_rows, module.delta_main,
                         module.delta_sample):
            source = inspect.getsource(function)
            self.assertNotIn(
                "2e9", source,
                "{} carries the unmeasured 2 GHz constant".format(
                    function.__name__))
            self.assertNotIn("1992.0", source)


class DeltaModeCatchesAGuestThatResetBetweenSamples(unittest.TestCase):
    """Two machines' counters subtracted is not a measurement.

    This tree has a recorded run whose counters read 8,687 then 8,258
    then 8,014 across one poll, because the guest reset mid-poll and
    every reading afterwards was of a different machine. The negative
    delta check catches some of those; the fingerprint catches them
    before any arithmetic happens, and catches the case where the new
    boot has already climbed past the old one's totals - which the
    negative check cannot see at all.
    """

    def fingerprints(self, base_a, tsc_a, base_b, tsc_b):
        return ({"base": base_a, "first_tsc": tsc_a},
                {"base": base_b, "first_tsc": tsc_b})

    def test_a_changed_module_base_rates_nothing(self):
        module = load_dump_state()
        lines, same = module.delta_fingerprint_lines(*self.fingerprints(
            0x6720f000, (7,), 0x67210000, (7,)))
        self.assertFalse(same)
        self.assertIn("CHANGED", "\n".join(lines))

    def test_a_changed_first_handler_tsc_rates_nothing(self):
        """The second field, because one field cannot disagree with itself.

        A reload at the same address moves `handler_first_tsc` and not
        the base; a reload at a different address moves the base. One
        check would miss one of them.
        """
        module = load_dump_state()
        _lines, same = module.delta_fingerprint_lines(*self.fingerprints(
            0x6720f000, (7,), 0x6720f000, (9,)))
        self.assertFalse(same)

    def test_the_same_boot_is_rated(self):
        """The control. A check that refuses everything measures nothing."""
        module = load_dump_state()
        lines, same = module.delta_fingerprint_lines(*self.fingerprints(
            0x6720f000, (7, 7), 0x6720f000, (7, 7)))
        self.assertTrue(same)
        self.assertIn("unchanged", "\n".join(lines))
        self.assertNotIn("CHANGED", "\n".join(lines))

    def test_a_fingerprint_that_was_not_read_is_not_agreement(self):
        """Absence is not evidence of sameness.

        The same trap as the missing reading above: `None == None` is
        True in Python and would have read as "the same boot".
        """
        module = load_dump_state()
        _lines, same = module.delta_fingerprint_lines(*self.fingerprints(
            None, (7,), None, (7,)))
        self.assertFalse(same)

    def test_a_reset_stops_the_report_before_any_rate(self):
        module = load_dump_state()
        text = "\n".join(module.delta_report(
            {("exit_total", 0): 1_000_000},
            {("exit_total", 0): 12},
            [(("exit_total", 0), "exits taken")],
            ({}, {}, []), [], module.delta_span(0, 100, 1.0),
            self.fingerprints(0x6720f000, (7,), 0x67210000, (7,)),
            1, 1.0, (0.1, 0.1)))
        self.assertIn("NOT from the same boot", text)
        self.assertNotIn("per second", text)


# ---------------------------------------------------------------------
# The reader, end to end, on a synthetic machine
# ---------------------------------------------------------------------
#
# `rig-dump-state.py` shells out for exactly five things - member
# offsets, array lengths, type-derived expressions, the singleton's
# address, and physical memory over the monitor - and every one of them
# is `subprocess.run`. One stub over that name puts the whole reader on
# a machine this test controls, which is what makes "the default output
# did not change" and "the delta mode catches a counter that went
# backwards" checkable without hardware.

FAKE_BASE = 0x67000000
FAKE_SINGLETON = 0x200000


class FakeResult:
    def __init__(self, out):
        self.stdout = out
        self.stderr = ""
        self.returncode = 0


class FakeRig:
    """Answers every subprocess the reader runs.

    Deliberately deterministic and never zero by default: a fixture that
    answers zero everywhere lets a reader that reads the wrong address
    pass, which is the failure being guarded against.
    """

    # The header's own constants, so a dimension the reader derives from
    # the type comes back the size it really is. A fixture answering
    # every sizeof with one number indexes tables out of range, which is
    # the fixture lying rather than the reader.
    EXPRESSIONS = [
        (r"vtl_differed\[0\]\[0\]\s*/\s*8", 20),
        (r"vtl_differed\[0\] /", 3),
        (r"vtl_stack\[0\]\s*/\s*8", 64),
        (r"vtl_image_name\[0\]", 96),
        (r"vtl_code\[0\]", 1024),
        (r"vtl_assist\[0\]\[0\]", 512),
        (r"vtl_step_count\s*/\s*8", 3),
        (r"vtl_step_rip\[0\]\s*/\s*8", 2048),
        (r"vtl_step_code\[0\]\[0\]", 16),
    ]
    LENGTHS = {"exit_reason_counts": 96, "exit_trace": 64,
               "l2_exit_trace": 64, "l2_working_trace": 64,
               "phase_cycles": 16, "l2_ept_dispositions": 10}

    # The members declared `[n]` rather than `[max_cpus][n]`, and the
    # length each really has. Separate from LENGTHS because gdb answers
    # a different question for each shape - see `_answer`.
    FLAT_LENGTHS = {"handler_reason_exits": 64,
                    "handler_reason_from_l2": 64,
                    "handler_reason_cycles": 64,
                    "handler_reason_reads": 64,
                    "handler_reason_writes": 64}
    MAX_CPUS = 8

    def __init__(self):
        self.offsets = {}
        self.cell = {}
        self.base = FAKE_BASE
        self.singleton = FAKE_SINGLETON
        self.connections = 0
        self.open_now = 0

    def offset_for(self, name):
        """A member's offset, from its NAME rather than from the order
        it was asked for.

        It used to be `0x10000 + 0x2000 * len(self.offsets)`, which made
        every address depend on how many members had been requested
        before it - so adding one name to the reader's offset list moved
        every member after it, changed every pseudo-random word derived
        from an address, and produced a 400-line diff in a dump that no
        reader change had touched.  **That is a fixture that cannot tell
        "the output changed" from "the fixture moved"**, and the whole
        point of the byte-for-byte check below is to tell those apart.

        Hashed into a space large enough that a collision is unlikely,
        and then *checked*, because an unlikely collision that aliases
        two members is exactly the silent wrong answer this file exists
        to prevent.
        """
        if name not in self.offsets:
            digest = hashlib.sha1(name.encode()).hexdigest()[:8]
            offset = 0x10000 + 0x2000 * (int(digest, 16) % (1 << 20))
            clash = [n for n, o in self.offsets.items() if o == offset]
            if clash:
                raise AssertionError(
                    "FakeRig offset collision: {} and {} hash to the "
                    "same address, so two members alias".format(
                        name, clash[0]))
            self.offsets[name] = offset
        return self.offsets[name]

    def address(self, name, index=0):
        return (FAKE_BASE + FAKE_SINGLETON + self.offset_for(name)
                + 8 * index)

    def put(self, name, index, value):
        self.cell[self.address(name, index)] = value

    def word(self, address):
        if address in self.cell:
            return self.cell[address]
        return (((address >> 3) * 2654435761) % 251) + 1

    def run(self, argv, **kwargs):
        if argv and argv[0] == "x86_64-elf-gdb":
            return self._gdb(argv)
        return self._ssh(argv, kwargs.get("input", ""))

    def _gdb(self, argv):
        """gdb, including the questions it REFUSES to answer.

        An expression gdb cannot evaluate prints its error on stderr and
        produces no `$N = ...` line at all, and the reader's length
        helpers count `$N` lines - so a refusal is what makes one of
        them `sys.exit`. This fixture used to answer *every* length
        question with a plausible small number, which meant the reader
        passed here and failed on the rig. Measured against real
        `x86_64-elf-gdb` on a stand-in object: the nested question put
        to a flat array answers `cannot subscript something of type
        'unsigned long'` and nothing else.
        """
        out, n = [], 0
        for i, a in enumerate(argv):
            if a != "-ex":
                continue
            answer = self._answer(argv[i + 1])
            if answer is None:
                continue
            n += 1
            out.append("${} = {}".format(n, answer))
        return FakeResult("\n".join(out) + "\n")

    def _answer(self, expression):
        """One expression's answer, or None when gdb would refuse it."""
        member = re.search(r"->([A-Za-z0-9_]+)$", expression)
        if expression.startswith("print/x (long)&") and member:
            return "0x{:x}".format(self.offset_for(member.group(1)))
        if expression.startswith("print/x &'"):
            return "0x{:x}".format(self.singleton)
        for pattern, value in self.EXPRESSIONS:
            if re.search(pattern, expression):
                return value

        # `sizeof(m[0]) / sizeof(m[0][0])` - the nested question. A flat
        # member has no `m[0][0]`, so gdb refuses and answers nothing.
        row = re.search(r"->([A-Za-z0-9_]+)\[0\] / sizeof", expression)
        if row:
            if row.group(1) in self.FLAT_LENGTHS:
                return None
            return self.LENGTHS.get(row.group(1), 8)

        # `sizeof(m) / sizeof(m[0])` - the flat question. Put to a
        # `[max_cpus][n]` member it answers `max_cpus`, which is a
        # plausible small number and the reason the two questions are
        # two functions rather than one with a fallback.
        flat = re.search(r"->([A-Za-z0-9_]+) / sizeof", expression)
        if flat:
            return self.FLAT_LENGTHS.get(flat.group(1), self.MAX_CPUS)
        return 8

    def _ssh(self, argv, script):
        if "allocate_rwx" in " ".join(argv):
            return FakeResult(
                "allocate_rwx done at 0x{:x}\n".format(self.base))
        self.connections += 1
        self.open_now += 1
        lines = []
        for line in script.split("\n"):
            m = re.match(r"xp/(\d+)gx 0x([0-9a-f]+)$", line.strip())
            if not m:
                continue
            count, address = int(m.group(1)), int(m.group(2), 16)
            for i in range(0, count, 4):
                row = [self.word(address + 8 * (i + k))
                       for k in range(min(4, count - i))]
                lines.append("{:016x}: ".format(address + 8 * i)
                             + " ".join("0x{:016x}".format(v)
                                        for v in row))
        self.open_now -= 1
        return FakeResult("\n".join(lines) + "\n")


def run_reader(rig, argv, clock=None, on_sleep=None):
    """`main()` against a FakeRig, with stdout captured."""
    import contextlib
    import io
    import types
    module = load_dump_state()
    module.subprocess = types.SimpleNamespace(run=rig.run)
    if clock is not None:
        ticks = iter(clock)
        module.time = types.SimpleNamespace(
            monotonic=lambda: next(ticks),
            sleep=lambda n: on_sleep(n) if on_sleep else None)
    saved = sys.argv
    out = io.StringIO()
    try:
        sys.argv = ["rig-dump-state.py"] + argv
        with contextlib.redirect_stdout(out):
            module.main()
    finally:
        sys.argv = saved
    return out.getvalue()


class DeltaModeLeavesTheDefaultDumpAlone(unittest.TestCase):
    """Adding a mode must not move the output every recipe already reads.

    CLAUDE.md and BACKLOG.md quote this dump's headings by name. The
    check is byte-for-byte against a run of the same reader with the
    flag absent, on a machine whose every word is fixed.
    """

    DEFAULT = ["--elf", "/dev/null", "--cpus", "2"]

    def test_the_default_dump_is_deterministic(self):
        """The premise. Without this the comparison below proves nothing."""
        first = run_reader(FakeRig(), self.DEFAULT)
        second = run_reader(FakeRig(), self.DEFAULT)
        self.assertEqual(first, second)
        self.assertGreater(len(first.splitlines()), 1000)

    def test_the_default_dump_carries_no_delta_output(self):
        text = run_reader(FakeRig(), self.DEFAULT)
        for marker in ("DELTA over a MEASURED", "REFUSED to difference",
                       "IMPOSSIBLE: a monotonic counter",
                       "fingerprint module base"):
            self.assertNotIn(marker, text)

    def test_the_default_dump_still_prints_its_own_headings(self):
        text = run_reader(FakeRig(), self.DEFAULT)
        for heading in ("module base 0x", "l2-activity",
                        "resumes-reached", "exit reasons (total"):
            self.assertIn(heading, text)

    def test_delta_mode_replaces_the_dump_rather_than_joining_it(self):
        """A total and a rate under adjacent headings is the confusion
        this mode exists to end, so the two reports never print together.
        """
        text = run_reader(FakeRig(), self.DEFAULT + ["--delta", "20"],
                          clock=[0.0, 1.5, 20.9, 22.4])
        self.assertIn("DELTA over a MEASURED", text)
        self.assertNotIn("l2-activity", text)
        self.assertNotIn("exit reasons (total", text)


class DeltaModeHoldsNoMonitorConnectionAcrossTheWait(unittest.TestCase):
    """The monitor takes exactly ONE connection.

    A socket left open makes `rig-dump-state.py` fail with no diagnosis,
    and a poller that leaks one reports every field as None for ever
    after. A mode that sleeps between two samples is the obvious place
    to leak one, so it is measured rather than assumed.
    """

    def test_no_connection_is_open_during_the_sleep(self):
        rig = FakeRig()
        seen = {}

        def on_sleep(_seconds):
            seen["open"] = rig.open_now
            seen["before"] = rig.connections

        run_reader(rig, ["--elf", "/dev/null", "--cpus", "2",
                         "--delta", "20"],
                   clock=[0.0, 1.5, 20.9, 22.4], on_sleep=on_sleep)
        self.assertEqual(0, seen["open"],
                         "a monitor connection was open across the wait")
        # And both samples really did connect - a mode that read nothing
        # would also hold nothing.
        self.assertGreater(seen["before"], 0)
        self.assertGreater(rig.connections, seen["before"])

    def test_the_second_sample_is_far_smaller_than_the_full_dump(self):
        """Why the delta path builds its own queue.

        The cumulative dump issues 144 connections for 50,605 words on
        this fixture, and every word of it widens the read window - the
        window being this measurement's own error bar. Measured here so
        a member added to the delta lists cannot quietly restore the
        full cost.
        """
        full = FakeRig()
        run_reader(full, ["--elf", "/dev/null", "--cpus", "2"])
        delta = FakeRig()
        run_reader(delta, ["--elf", "/dev/null", "--cpus", "2",
                           "--delta", "20"],
                   clock=[0.0, 1.5, 20.9, 22.4])
        # Two samples, and still a fraction of one cumulative dump.
        self.assertLess(delta.connections, full.connections // 2)


class DeltaModeEndToEndCatchesTheBackwardsCounter(unittest.TestCase):
    """The whole path, from the monitor to the verdict.

    The unit tests above check the arithmetic. This checks that the
    arithmetic is reached: a classification that is right and never
    called is worth nothing, and this file already records a verdict
    helper that was one deletion away from exactly that.
    """

    ARGV = ["--elf", "/dev/null", "--cpus", "1", "--delta", "20"]
    CLOCK = [0.0, 1.5, 20.9, 22.4]

    def seed(self, rig, values):
        for name, value in values.items():
            rig.put(name, 0, value)

    def run_two(self, before, after):
        rig = FakeRig()
        self.seed(rig, before)
        return run_reader(rig, self.ARGV, clock=self.CLOCK,
                          on_sleep=lambda _n: self.seed(rig, after))

    def test_a_counter_that_advanced_is_reported_as_a_rate(self):
        text = self.run_two(
            {"handler_first_tsc": 7, "handler_last_tsc": 0,
             "exit_total": 1_000_000},
            {"handler_first_tsc": 7,
             "handler_last_tsc": int(1_992_000_000 * 20.9),
             "exit_total": 1_106_380})
        self.assertIn("exit_total", text)
        self.assertIn("5,089.95", text)          # 106,380 / 20.9
        self.assertNotIn("IMPOSSIBLE", text)

    def test_a_counter_that_went_backwards_is_reported_as_an_error(self):
        text = self.run_two(
            {"handler_first_tsc": 7, "handler_last_tsc": 0,
             "exit_total": 1_000_000},
            {"handler_first_tsc": 7,
             "handler_last_tsc": int(1_992_000_000 * 20.9),
             "exit_total": 999_571})
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("exit_total[cpu 0]", text)
        self.assertIn("-429", text)

    def test_a_boot_that_changed_stops_the_report(self):
        text = self.run_two(
            {"handler_first_tsc": 7, "handler_last_tsc": 0,
             "exit_total": 1_000_000},
            {"handler_first_tsc": 0x76adf1,
             "handler_last_tsc": int(1_992_000_000 * 20.9),
             "exit_total": 1_106_380})
        self.assertIn("NOT from the same boot", text)
        self.assertNotIn("5,089.95", text)


class AFlatArrayCannotBeAskedTheNestedQuestion(unittest.TestCase):
    """`gdb_lengths` on `x[64]`, and the section it silently deleted.

    `gdb_lengths` asks `sizeof(m[0]) / sizeof(m[0][0])`, which is right
    for `x[max_cpus][n]` and is not a question at all for a flat `x[n]`.
    **Measured with real `x86_64-elf-gdb` on a stand-in object holding
    `unsigned long handler_reason_exits[64]` and `unsigned long
    phase_cycles[8][52]`:**

        nested form, flat member   -> cannot subscript something of
                                      type `unsigned long'   (no $1)
        nested form, nested member -> $1 = 52
        flat form,   flat member   -> $1 = 64
        flat form,   nested member -> $1 = 8      <- max_cpus

    The caller wrapped that refusal in `except SystemExit` and printed
    `handler_reason_exits is absent from this ELF`. It is not absent -
    it has been resident since it was written - so `--delta` has never
    printed its exits-by-level split, and the note in its place named a
    cause that was not true.

    Two rules fall out and both are pinned below: the two shapes get two
    functions, because the flat question put to a nested member answers
    `max_cpus` rather than failing; and the fixture has to model the
    refusal, because a fixture that answers every question passes a
    reader that dies on the rig.
    """

    def test_the_two_helpers_ask_two_different_questions(self):
        source = read(DUMP_STATE)
        flat = re.search(r"def gdb_flat_lengths.*?return dict", source,
                         re.S)
        nested = re.search(r"def gdb_lengths.*?return dict", source,
                           re.S)
        self.assertIsNotNone(flat, "gdb_flat_lengths is gone, so a flat "
                                   "row is being asked the nested "
                                   "question again")
        self.assertIn("->{m} / sizeof", flat.group(0))
        self.assertNotIn("->{m}[0] / sizeof", flat.group(0))
        self.assertIn("->{m}[0] / sizeof", nested.group(0))

    def test_the_flat_members_are_resolved_with_the_flat_helper(self):
        """The wiring, not just the helper.

        A correct helper that nothing calls is worth nothing, and this
        file already records a verdict helper one deletion away from
        exactly that.
        """
        source = read(DUMP_STATE)
        call = re.search(
            r"reason_slots = (gdb_\w+)\(", source)
        self.assertIsNotNone(call)
        self.assertEqual("gdb_flat_lengths", call.group(1))
        phase = re.search(r"phase_slots = (gdb_\w+)\(", source)
        self.assertIsNotNone(phase)
        self.assertEqual("gdb_lengths", phase.group(1),
                         "phase_cycles is [max_cpus][n]; the flat "
                         "question would answer max_cpus and walk every "
                         "processor's row at the wrong stride")

    def test_the_fixture_refuses_what_gdb_refuses(self):
        """The fixture's own negative control.

        Without this the reader passes here and fails on the rig, which
        is what happened: the fixture answered the impossible question
        with 8 and the section looked healthy in every test.
        """
        rig = FakeRig()
        nested = ("print (int)(sizeof(('zpp::hypervisor::hypervisor' "
                  "*)0)->handler_reason_exits[0] / sizeof(('zpp::"
                  "hypervisor::hypervisor' *)0)->handler_reason_exits"
                  "[0][0])")
        self.assertIsNone(rig._answer(nested))
        flat = ("print (int)(sizeof(('zpp::hypervisor::hypervisor' *)0)"
                "->handler_reason_exits / sizeof(('zpp::hypervisor::"
                "hypervisor' *)0)->handler_reason_exits[0])")
        self.assertEqual(64, rig._answer(flat))

    def test_the_flat_question_on_a_nested_member_answers_max_cpus(self):
        """Why the two are two functions and not one with a fallback.

        This is the dangerous half: it does not fail, it answers a
        plausible small number.
        """
        rig = FakeRig()
        flat = ("print (int)(sizeof(('zpp::hypervisor::hypervisor' *)0)"
                "->phase_cycles / sizeof(('zpp::hypervisor::hypervisor'"
                " *)0)->phase_cycles[0])")
        self.assertEqual(rig.MAX_CPUS, rig._answer(flat))
        self.assertNotEqual(rig.LENGTHS["phase_cycles"],
                            rig._answer(flat))

    def test_the_split_appears_now_and_did_not_before(self):
        """End to end, and the negative control is the old expression.

        The old call is reconstructed here rather than described, so
        this fails if the fix is reverted **and** fails if the fixture
        stops modelling the refusal.
        """
        text = run_reader(FakeRig(),
                          ["--elf", "/dev/null", "--cpus", "1",
                           "--delta", "20"],
                          clock=[0.0, 1.5, 20.9, 22.4])
        self.assertNotIn("handler_reason_exits is absent from this ELF",
                         text)
        self.assertIn("exits by level", text)


class PerHandlerCyclesAreWindowedNotBootWide(unittest.TestCase):
    """`handler_reason_cycles`, the last boot-cumulative instrument.

    `dump_handler_by_reason` divides it by `handler_cycles` since the
    first exit of the boot and prints the quotient under a present-tense
    heading. Its own docstring quotes `vmresume at 38% of exits and
    vmptrld at 8%` - a mean taken before VMCS shadowing was in force,
    which is a configuration that no longer exists, and with shadowing
    on the guest hypervisor's VMREADs and VMWRITEs stop exiting at all.

    So the file already carries a stale figure of exactly the kind
    `--delta` exists to retire, and every test below is about the two
    numbers being different: what the split is over the boot, and what
    it is over the window.
    """

    ROWS = ("handler_reason_cycles", "handler_reason_reads",
            "handler_reason_writes", "handler_reason_exits",
            "handler_reason_from_l2")
    SLOTS = 64

    def samples(self, boot, window):
        """`(before, after)` from a boot total and a window's share.

        `after` is the boot total and `before` is it minus the window,
        which is the only arrangement that is monotonic - building them
        the other way round produces a negative delta and tests the
        impossibility path by accident.
        """
        before, after = {}, {}
        for reason in range(self.SLOTS):
            for name in self.ROWS:
                total = boot.get(reason, {}).get(name, 0)
                moved = window.get(reason, {}).get(name, 0)
                after[(name, reason)] = total
                before[(name, reason)] = total - moved
        return before, after

    def report(self, boot, window, handler=None, exits=None,
               seconds=20.0):
        module = load_dump_state()
        before, after = self.samples(boot, window)
        if handler is None:
            handler = int(sum(w.get("handler_reason_cycles", 0)
                              for w in window.values()) / 0.8)
        if exits is None:
            exits = sum(w.get("handler_reason_exits", 0)
                        for w in window.values())
        return "\n".join(module.delta_handler_reason_lines(
            before, after, self.SLOTS, seconds, handler, exits))

    # --- what is differenced, and what is refused -------------------

    def test_the_cost_rows_are_differenced_and_not_refused(self):
        """The positive control. Out of the refusal list AND in a read
        list - removing it from one alone leaves a reader that neither
        reports the member nor says it declined to."""
        module = load_dump_state()
        named = [n for n, _ in module.DELTA_HANDLER_REASON_COSTS]
        self.assertIn("handler_reason_cycles", named)
        self.assertIn("handler_reason_reads", named)
        self.assertIn("handler_reason_writes", named)
        for _what, names, _why in module.DELTA_REFUSALS:
            for name in named:
                self.assertNotIn(name, names)

    def test_the_open_end_of_the_same_bracket_is_refused(self):
        """The negative control, and the trap next to the thing added.

        `phase_mark` and `handler_entry_tsc` sit beside the members this
        change differences, are the same width, are per processor, and
        **climb**, because they are RDTSC values. Their delta looks
        exactly like a cycle count and is a distance between two
        unrelated instants.
        """
        module = load_dump_state()
        differenced = set(
            [n for n, _ in module.DELTA_PER_CPU_COUNTERS]
            + [n for n, _ in module.DELTA_GLOBAL_COUNTERS]
            + [n for n, _ in module.DELTA_PER_CPU_CYCLES]
            + [n for n, _ in module.DELTA_HANDLER_REASON_COSTS]
            + [n for n, _ in module.DELTA_GLOBAL_HISTOGRAMS])
        refused = "\n".join(names for _w, names, _y
                            in module.DELTA_REFUSALS)
        for name in ("phase_mark", "handler_entry_tsc",
                     "handler_entry_reads", "handler_entry_writes",
                     "handler_was_l2", "reason_bucket"):
            self.assertNotIn(name, differenced,
                             "{} is the open end of a bracket and must "
                             "not be subtracted".format(name))
            self.assertIn(name, refused,
                          "{} is neither differenced nor named as "
                          "refused, so the report is silent about "
                          "it".format(name))

    def test_the_shapes_are_what_the_header_declares(self):
        """A flat row read per processor reports one CPU's share as the
        whole, and a per-processor row read flat reads its neighbour."""
        module = load_dump_state()
        for name, _ in (module.DELTA_HANDLER_REASON_COSTS
                        + module.DELTA_GLOBAL_HISTOGRAMS):
            self.assertEqual(
                ["handler_reason_slots"], header_dimensions(name),
                "{} is read as one global row".format(name))
        for name in ("phase_cycles", "phase_calls"):
            self.assertEqual(["max_cpus", "phase_count"],
                             header_dimensions(name))

    # --- the arithmetic --------------------------------------------

    def test_a_reason_whose_share_moved_is_called_out_in_words(self):
        """The verdict this section exists for.

        Shape taken from the figure the cumulative reader printed:
        `vmresume` at 38% of the handler over the boot. Here the window
        holds 58% of it, which is a twenty-point drift and is the
        difference between "the reflection path is the cost" and "it is
        not".
        """
        boot = {24: {"handler_reason_cycles": 3_800_000,
                     "handler_reason_exits": 38_000},
                32: {"handler_reason_cycles": 6_200_000,
                     "handler_reason_exits": 62_000}}
        window = {24: {"handler_reason_cycles": 580_000,
                       "handler_reason_exits": 5_800},
                  32: {"handler_reason_cycles": 420_000,
                       "handler_reason_exits": 4_200}}
        text = self.report(boot, window)
        self.assertIn("DISAGREE", text)
        self.assertIn("vmresume", text)
        self.assertIn("38.0% since boot", text)
        self.assertIn("58.0%", text)
        # Signed against the boot figure. `vmresume` ROSE and `wrmsr`
        # FELL by the same twenty points, and printing both as `+20.0`
        # is the mislabelling this whole file is written against.
        self.assertIn("(+20.0 points)", text)
        self.assertIn("(-20.0 points)", text)

    def test_a_split_that_did_not_move_its_shares_says_it_agrees(self):
        """**The negative control for the check above.**

        A verdict that only ever fires cannot be told apart from one
        that is not wired up, and the AGREE case has to be as loud as
        the DISAGREE case - otherwise the reader learns nothing from
        silence. Same totals as above, window scaled exactly.
        """
        boot = {24: {"handler_reason_cycles": 3_800_000,
                     "handler_reason_exits": 38_000},
                32: {"handler_reason_cycles": 6_200_000,
                     "handler_reason_exits": 62_000}}
        window = {24: {"handler_reason_cycles": 380_000,
                       "handler_reason_exits": 3_800},
                  32: {"handler_reason_cycles": 620_000,
                       "handler_reason_exits": 6_200}}
        text = self.report(boot, window)
        self.assertIn("AGREES", text)
        self.assertNotIn("DISAGREE", text)

    def test_the_drift_threshold_is_the_one_the_module_declares(self):
        """A threshold in the test and a threshold in the reader are two
        constants, and this tree records what happens when a copy stops
        moving with its original."""
        module = load_dump_state()
        boot = {24: {"handler_reason_cycles": 500,
                     "handler_reason_exits": 5},
                32: {"handler_reason_cycles": 500,
                     "handler_reason_exits": 5}}
        # Just under the declared drift: 50% -> 50% + half of it.
        edge = module.DELTA_SHARE_DRIFT_POINTS
        self.assertGreater(edge, 0.0)
        window = {24: {"handler_reason_cycles": 100,
                       "handler_reason_exits": 1},
                  32: {"handler_reason_cycles": 100,
                       "handler_reason_exits": 1}}
        self.assertIn("AGREES", self.report(boot, window))

    def test_the_printed_cost_is_the_window_not_the_boot(self):
        """The whole point, stated as an assertion.

        Boot-wide this reason costs 100 cycles an exit; in the window it
        costs 10,000. The cumulative reader prints 100 under a
        present-tense heading, and 100 must not appear here.
        """
        boot = {24: {"handler_reason_cycles": 1_000_000,
                     "handler_reason_exits": 10_000}}
        window = {24: {"handler_reason_cycles": 100_000,
                       "handler_reason_exits": 10}}
        text = self.report(boot, window)
        self.assertIn("10,000", text)
        self.assertIn("IN THIS WINDOW", text)

    def test_a_backwards_cost_row_is_an_error_not_a_number(self):
        module = load_dump_state()
        before, after = self.samples(
            {24: {"handler_reason_cycles": 1_000,
                  "handler_reason_exits": 10}},
            {24: {"handler_reason_cycles": 100,
                  "handler_reason_exits": 1}})
        after[("handler_reason_cycles", 24)] = 500
        text = "\n".join(module.delta_handler_reason_lines(
            before, after, self.SLOTS, 20.0, 10_000, 10))
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("handler_reason_cycles[reason 24]", text)
        self.assertNotIn("cyc/exit", text)

    def test_a_split_larger_than_what_it_splits_is_impossible(self):
        """`handler_reason_cycles` and `handler_cycles` are closed from
        the same pair of reads in `resume_guest`, so the first cannot
        exceed the second - and a reader that prints it anyway reports
        a coverage above 100%, which reads as a finding."""
        boot = {24: {"handler_reason_cycles": 1_000_000,
                     "handler_reason_exits": 1_000}}
        window = {24: {"handler_reason_cycles": 500_000,
                       "handler_reason_exits": 500}}
        text = self.report(boot, window, handler=10_000, exits=500)
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("LARGER than what it splits", text)

    def test_one_exit_straddling_a_boundary_is_not_an_impossibility(self):
        """The negative control for the check above.

        At most one exit can be open at each of the two sample
        boundaries, so a split a whisker over its denominator is the
        expected case and refusing it would make this section useless on
        every real reading.
        """
        boot = {24: {"handler_reason_cycles": 1_000_000,
                     "handler_reason_exits": 1_000}}
        window = {24: {"handler_reason_cycles": 500_000,
                       "handler_reason_exits": 500}}
        text = self.report(boot, window, handler=499_000, exits=500)
        self.assertNotIn("IMPOSSIBLE", text)
        self.assertIn("covers 100.2%", text)

    def test_an_unread_row_is_unknown_and_not_zero(self):
        """An unanswered read and a handler that took no time produce
        the same missing row, and only one is a fact about the guest."""
        module = load_dump_state()
        before, after = self.samples(
            {24: {"handler_reason_cycles": 1_000,
                  "handler_reason_exits": 10}},
            {24: {"handler_reason_cycles": 100,
                  "handler_reason_exits": 1}})
        del after[("handler_reason_reads", 7)]
        text = "\n".join(module.delta_handler_reason_lines(
            before, after, self.SLOTS, 20.0, 1_000, 10))
        self.assertIn("NOT READ", text)
        self.assertIn("handler_reason_reads", text)
        self.assertIn("unknown rather than empty", text)

    def test_a_window_with_no_exits_is_not_a_table_of_zeroes(self):
        boot = {24: {"handler_reason_cycles": 1_000_000,
                     "handler_reason_exits": 10_000}}
        text = self.report(boot, {}, handler=0, exits=0)
        self.assertIn("every reason unchanged in this window", text)
        self.assertNotIn("cyc/exit", text)

    def test_an_unread_denominator_leaves_coverage_unknown(self):
        """A split with no denominator is not a fraction."""
        module = load_dump_state()
        before, after = self.samples(
            {24: {"handler_reason_cycles": 1_000_000,
                  "handler_reason_exits": 10_000}},
            {24: {"handler_reason_cycles": 1_000,
                  "handler_reason_exits": 10}})
        text = "\n".join(module.delta_handler_reason_lines(
            before, after, self.SLOTS, 20.0, None, None))
        self.assertIn("handler_cycles was NOT READ", text)
        self.assertIn("handler_exits was NOT READ", text)
        self.assertNotIn("covers", text)


class TheWindowedPhaseTreeReplacesTheBootWideOne(unittest.TestCase):
    """`phase_cycles`, and the reading that cost this session most.

    The cumulative tree printed `11,358 us and 90.8 exits` for one
    trust-level round trip under a present-tense heading, describing a
    phase that had ended - and the same dump implied 77.6 round trips a
    second against a measured 5.45. Every column of it is a boot-wide
    mean, and a boot has phases.

    The windowed tree is the same shape on purpose, so the two can be
    read against each other, with the heading saying which is which.
    """

    SLOTS = 52

    def samples(self, cpu, boot, window):
        before, after = {}, {}
        for slot in range(self.SLOTS):
            for name in ("phase_cycles", "phase_calls"):
                total = boot.get(slot, {}).get(name, 0)
                moved = window.get(slot, {}).get(name, 0)
                after[(name, (cpu, slot))] = total
                before[(name, (cpu, slot))] = total - moved
        return before, after

    def test_the_tree_is_opt_in_and_absence_is_announced(self):
        """A section silently absent and one that measured nothing look
        identical afterwards, which is this whole file's subject."""
        text = run_reader(FakeRig(),
                          ["--elf", "/dev/null", "--cpus", "1",
                           "--delta", "20"],
                          clock=[0.0, 1.5, 20.9, 22.4])
        self.assertIn("the phase tree: NOT SAMPLED in this window", text)
        self.assertIn("--delta-phases", text)
        self.assertNotIn("phase tree IN THIS WINDOW", text)

    def test_the_flag_reaches_the_reads(self):
        """The positive control for the line above: with the flag the
        section is produced, so 'NOT SAMPLED' is a choice and not a
        section that does not exist."""
        text = run_reader(FakeRig(),
                          ["--elf", "/dev/null", "--cpus", "1",
                           "--delta", "20", "--delta-phases"],
                          clock=[0.0, 1.5, 20.9, 22.4])
        self.assertNotIn("the phase tree: NOT SAMPLED", text)
        self.assertIn("cpu 0 phase tree", text)

    def test_a_phase_that_did_not_run_is_not_the_boot_s_cost(self):
        """The reading the cumulative tree gets exactly backwards.

        A phase with a large boot total and nothing in the window is a
        phase that has **stopped**; the cumulative tree prints its
        boot-wide cost either way, and that is the number that was
        quoted in the present tense.
        """
        module = load_dump_state()
        before, after = self.samples(
            0, {26: {"phase_cycles": 900_000_000,
                     "phase_calls": 3_000}}, {})
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, 0, 0,
            3_000, 900_000_000))
        self.assertIn("no phase was entered in this window", text)
        self.assertNotIn("900,000,000", text)
        self.assertNotIn("300,000", text)

    def test_the_nesting_is_taken_from_the_parent_table(self):
        """A container and its child must not be added to each other.

        Slot 1 `reflect_l2_exit` is inside slot 26 `exit: dispatch`, so
        26's `self` column is its own cycles minus 1's - and a flat sum
        of the table would count 1 twice.
        """
        module = load_dump_state()
        before, after = self.samples(
            0,
            {26: {"phase_cycles": 1_000_000, "phase_calls": 100},
             1: {"phase_cycles": 600_000, "phase_calls": 100}},
            {26: {"phase_cycles": 1_000_000, "phase_calls": 100},
             1: {"phase_cycles": 600_000, "phase_calls": 100}})
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, 100, 1_000_000,
            100, 1_000_000))
        self.assertIn("exit: dispatch", text)
        self.assertIn("reflect_l2_exit", text)
        # 26 is 10,000 cyc/RT and its self is 4,000 once 1 is removed.
        self.assertRegex(text, r"exit: dispatch\s+.*10,000\s+4,000")

    def test_a_backwards_phase_accumulator_is_an_error(self):
        module = load_dump_state()
        before, after = self.samples(
            0, {26: {"phase_cycles": 1_000, "phase_calls": 10}},
            {26: {"phase_cycles": 100, "phase_calls": 1}})
        after[("phase_cycles", (0, 26))] = 500
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, 10, 1_000, 10, 1_000))
        self.assertIn("IMPOSSIBLE", text)
        self.assertIn("phase_cycles[cpu 0][26]", text)
        self.assertNotIn("cyc/RT", text)

    def test_an_unread_denominator_prints_no_tree(self):
        """`round_trips` divides every column. A denominator that was
        not read is not a small denominator."""
        module = load_dump_state()
        before, after = self.samples(
            0, {26: {"phase_cycles": 1_000, "phase_calls": 10}},
            {26: {"phase_cycles": 100, "phase_calls": 1}})
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, None, 1_000, 10, 1_000))
        self.assertIn("NOT PRINTED", text)
        self.assertIn("no denominator", text)

    def test_a_round_trip_that_got_dearer_is_called_out(self):
        """The phase tree's own verdict.

        Boot-wide the handler costs 100,000 cycles a round trip; in this
        window it costs 300,000. Both are correct and only one is the
        present tense.
        """
        module = load_dump_state()
        before, after = self.samples(
            0, {26: {"phase_cycles": 1_000_000, "phase_calls": 100}},
            {26: {"phase_cycles": 300_000, "phase_calls": 10}})
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, 10, 3_000_000,
            1_000, 100_000_000))
        self.assertIn("DISAGREE", text)
        self.assertIn("3.00x", text)

    def test_a_round_trip_that_did_not_move_says_so(self):
        """**The negative control for the verdict above.**"""
        module = load_dump_state()
        before, after = self.samples(
            0, {26: {"phase_cycles": 1_000_000, "phase_calls": 100}},
            {26: {"phase_cycles": 300_000, "phase_calls": 10}})
        text = "\n".join(module.delta_phase_lines(
            before, after, 0, self.SLOTS, 20.0, 10, 1_000_000,
            1_000, 100_000_000))
        self.assertIn("agrees with the boot-wide mean", text)
        self.assertNotIn("DISAGREE", text)


class TheDeltaReadWindowIsMeasuredWhenItGrows(unittest.TestCase):
    """Every member added widens the read window, and the read window is
    this measurement's own error bar.

    So the cost of adding the per-handler accounting is measured rather
    than asserted to be small, and the widest part of it is behind a
    flag. The unit here is monitor round trips - `Monitor.CHUNK` is 6
    commands per ssh connection - because that, not the word count, is
    what the wall clock follows.
    """

    ARGV = ["--elf", "/dev/null", "--delta", "20"]
    CLOCK = [0.0, 1.5, 20.9, 22.4]

    def connections(self, cpus, phases):
        rig = FakeRig()
        argv = self.ARGV + ["--cpus", str(cpus)]
        if phases:
            argv = argv + ["--delta-phases"]
        run_reader(rig, argv, clock=self.CLOCK)
        return rig.connections

    def test_the_phase_tree_is_the_part_that_scales_with_processors(self):
        """Two rows of phase_count per processor, so its cost grows with
        the guest and the by-reason rows' cost does not - which is why
        one is opt-in and the other is not."""
        one = self.connections(1, True) - self.connections(1, False)
        eight = self.connections(8, True) - self.connections(8, False)
        self.assertGreater(eight, one)

    def test_the_delta_sample_is_still_a_fraction_of_the_full_dump(self):
        """The bound that stops a member added here quietly restoring
        the cumulative dump's cost."""
        full = FakeRig()
        run_reader(full, ["--elf", "/dev/null", "--cpus", "2"])
        delta = FakeRig()
        run_reader(delta, ["--elf", "/dev/null", "--cpus", "2",
                           "--delta", "20", "--delta-phases"],
                   clock=self.CLOCK)
        self.assertLess(delta.connections, full.connections // 2)


if __name__ == "__main__":
    unittest.main()
