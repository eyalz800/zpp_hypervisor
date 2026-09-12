#!/usr/bin/env python3
"""Every narrow `xp` parse in `scripts/`, against the monitor's echo.

**The defect this exists for produced numbers, not errors.** The QEMU
monitor echoes the command it was sent before printing the answer, so a
response looks like

    (qemu) xp /1xb 0x351f61dd0
    0000000351f61dd0: 0x0d

A physical address on this rig is 9-12 hex digits, so `re.findall` of
`0x([0-9a-f]{2})` over the whole response returns `['35', '0d']` - the
address's leading two digits FIRST, and every caller that took `[0]`
got the address. `{16}` was safe only because an echoed address is
never sixteen digits long, which is why the pointer walks and page-table
reads looked fine while every byte and word read was fabricated.

What that cost, measured rather than argued: `guest-threads.py` printed
a `WaitReason` column that was the physical address's prefix. Threads in
one pool region share that prefix, so the column read *identical for
every thread* and *stable across boots* - which is what a real wait
state looks like, and it was quoted as one. `guest-thread-stack.py` used
the same byte as a *selector*, so it dumped the stack of whichever
thread's address happened to begin with the requested wait reason, and
printed nothing when none did, which reads exactly like "no thread is
waiting on that".

Two checks, and the source-level one is the one that lasts:

  - `test_rows_rejects_the_echo` fixes the *behaviour* of the filter
    against a real-shaped response, including the negative control - a
    response with no data row at all must yield nothing, so a caller's
    `v[0] if v else None` can report failure. The old code could not
    fail: it always had the echo to return.

  - `test_every_narrow_parse_is_row_filtered` fixes the *rule*. Any
    `findall` in `scripts/` asking for fewer than sixteen hex digits
    must be applied to filtered rows. This is what stops the fix from
    being undone one script at a time, which is how it spread: the
    four-line filter existed and was correct in the four scripts where
    the bug had been caught, and was never propagated to the five that
    used the same idiom.

Hermetic: reads the scripts as text and runs one regex. No rig, no
cross build, no network.
"""
import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPTS = os.path.join(ROOT, "scripts")

# A response of the shape the monitor really sends, echo included. The
# address is deliberately nine digits - the shortest that makes the echo
# match a `{8}` parse - and its leading digits are deliberately
# printable-ASCII and plausible as a wait reason, because that is what
# made the fabricated values believable.
ECHOED = ("(qemu) xp /1xb 0x351f61dd0\n"
          "0000000351f61dd0: 0x0d\n"
          "(qemu) ")

ROW_RE = re.compile(r"^[0-9a-f]{6,}: ")


def rows(d):
    """The filter under test, spelled as the scripts spell it."""
    return "\n".join(l for l in d.splitlines()
                     if ROW_RE.match(l.strip()))


class MonitorEcho(unittest.TestCase):
    def test_rows_rejects_the_echo(self):
        # The negative control first: unfiltered, the echo wins.
        raw = re.findall(r"0x([0-9a-f]{2})", ECHOED)
        self.assertEqual(raw[0], "35",
                         "the premise of this test is wrong: an "
                         "unfiltered narrow parse must return the "
                         "echoed address first")
        # Filtered, only the datum survives.
        got = re.findall(r"0x([0-9a-f]{2})", rows(ECHOED))
        self.assertEqual(got, ["0d"])

    def test_rows_rejects_the_echo_at_every_width(self):
        for width, cmd, row, want in (
                (2, "xp /1xb", "0000000351f61dd0: 0x0d", ["0d"]),
                (4, "xp /1xh", "0000000351f61dd0: 0x000d", ["000d"]),
                (8, "xp /2xw",
                 "0000007011108014: 0x00460001 0x00000000",
                 ["00460001", "00000000"]),
                (16, "xp /1xg",
                 "00000001a2b3c4d5: 0x0000000000000123",
                 ["0000000000000123"])):
            d = f"(qemu) {cmd} 0x351f61dd0\n{row}\n(qemu) "
            got = re.findall(r"0x([0-9a-f]{%d})" % width, rows(d))
            self.assertEqual(got, want, f"width {width}")

    def test_no_rows_yields_nothing_so_a_caller_can_refuse(self):
        # "Cannot access memory" is a real monitor answer. An instrument
        # that returns a number here cannot report its own failure, and
        # that is the whole family this file belongs to.
        d = ("(qemu) xp /1xb 0x351f61dd0\n"
             "Cannot access memory\n"
             "(qemu) ")
        self.assertEqual(rows(d), "")
        self.assertEqual(re.findall(r"0x([0-9a-f]{2})", rows(d)), [])

    def test_every_narrow_parse_is_row_filtered(self):
        """No `findall` under 16 digits may see an unfiltered response.

        The check is textual and scoped to the enclosing function,
        because the two safe spellings in this tree differ. Some
        scripts filter once - `d = _rows(monitor(...))` - and match
        against `d`; others iterate `for l in _rows(d)` and match
        against `l`. Both are correct and neither is recognisable from
        the `findall` line alone, so the rule is: a function containing
        a narrow parse must also contain the filter.

        A function that matched a *wider-than-the-address* pattern
        would be safe too, but nothing here does that and permitting it
        would let the rule be argued around.
        """
        narrow = re.compile(
            r"re\.findall\(\s*r?['\"]0x\(\[0-9a-f\]\{(\d+)\}\)['\"]")
        offenders = []
        for name in sorted(os.listdir(SCRIPTS)):
            if not name.endswith(".py"):
                continue
            with open(os.path.join(SCRIPTS, name), encoding="utf-8") as f:
                lines = f.read().splitlines()
            # Enclosing top-level `def` for each line, by indentation.
            owner, start = {}, None
            for i, line in enumerate(lines):
                if re.match(r"^(async )?def \w+", line):
                    start = i
                elif line and not line[0].isspace() \
                        and not line.lstrip().startswith("#"):
                    start = None
                owner[i] = start
            for i, line in enumerate(lines):
                m = narrow.search(line)
                if not m or int(m.group(1)) >= 16:
                    continue
                s = owner[i]
                if s is None:
                    body = "\n".join(lines)
                else:
                    end = s + 1
                    while end < len(lines) and (
                            not lines[end]
                            or lines[end][0].isspace()):
                        end += 1
                    body = "\n".join(lines[s:end])
                if "_rows(" in body:
                    continue
                offenders.append(f"{name}:{i + 1}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "a narrow xp parse sits in a function that never filters "
            "to data rows - it will match the ECHOED command's address "
            "instead of the data:\n  " + "\n  ".join(offenders))


if __name__ == "__main__":
    unittest.main()
