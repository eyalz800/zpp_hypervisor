#!/usr/bin/env python3
"""The harness shims' member declarations, against the real hypervisor.h.

Five harnesses under tests/ replace `zpp::hypervisor::hypervisor` with a
stand-in carrying only the members the code they cut out actually
touches. That is what makes them possible - the real class is megabytes
of per-processor arrays - and it is also a copy, which is somewhere for a
silent disagreement to live.

Two kinds of drift, and only one of them announces itself:

  - A member the real source uses and the shim lacks is a **compile
    error**, loud and immediate. That is the drift that has bitten three
    times, and it needs no help from this script.

  - A member present in both with a *different declaration* compiles
    perfectly and tests the wrong thing. `std::uint64_t x[max_cpus]`
    against a real `x[max_cpus][32]` is one array index away from
    reporting a neighbour's value, and every assertion about it passes.

This checks the second. It is the same argument as the `start_up_handoff
_state` constant comparison in tests/ap_start_up/build.sh, generalised:
where a test carries a copy of something the hypervisor owns, the copy
has to be checked against the original or the test is about the copy.

Members that exist only in a shim are reported rather than failed - every
harness adds its own instrumentation, and there is no way to tell that
from a typo except by reading. Printing them is what makes a typo
visible: a misspelled member silently becomes "harness-only" and appears
in a list that is otherwise short and familiar.

Run from scripts/ci/run-host-tests.sh. Needs no build.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL = os.path.join(ROOT, "hypervisor", "include", "zpp", "hypervisor",
                    "hypervisor.h")

# A data member: a type, a name, optional array bounds, optional
# initialiser, ending at the semicolon. Deliberately narrow - it matches
# the shapes these headers actually use and nothing else, so anything it
# cannot parse is skipped rather than guessed at.
MEMBER = re.compile(
    r"^(?P<type>(?:volatile\s+)?"
    r"(?:std::(?:uint\d+_t|size_t|atomic<[^>]+>)|bool|epte|"
    r"arch::[\w:]+|zpp::[\w:]+|[A-Za-z_]\w*)"
    r"(?:\s*\*)?)\s+"
    r"(?P<name>[a-z_]\w*)"
    r"(?P<bounds>(?:\s*\[[^\]]*\])*)"
    r"\s*(?:\{[^;]*\}|=[^;]*)?;$")

CONSTANT = re.compile(
    r"^static\s+constexpr\s+(?P<type>[\w:]+)\s+(?P<name>[a-z_]\w*)"
    r"\s*=\s*(?P<value>[^;]+);$")


def declarations(path):
    """Every member and constant a header declares, normalised.

    Line oriented and continuation joining, because clang-format wraps a
    long array bound onto the next line - and matching on the wrapped
    form would make this a comparison of formatting rather than of
    meaning.

    An earlier version split the whole file on semicolons and then tried
    to find where each declaration began by trimming back from the last
    brace. It parsed 47 members out of a header with hundreds, which the
    self-check below caught - and the reason it is a hard failure rather
    than a warning is that a checker comparing nothing reports success.
    """
    with open(path, encoding="utf-8") as handle:
        text = handle.read()

    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)

    members = {}
    constants = {}

    pending = ""
    depth = 0
    class_depth = None

    for raw in text.split("\n"):
        line = raw.strip()
        if not line:
            continue

        # Where the class body begins. Absolute depth will not do: the
        # real header nests the class inside two namespaces and a shim
        # inside one, so "depth 1" means different things in the two
        # files - which is what made the first attempt at this parse
        # nothing at all.
        if class_depth is None and re.match(
                r"^(class|struct)\s+hypervisor\b", line):
            class_depth = depth

        # Members of a *nested* struct are skipped. Without this a
        # harness-local `struct attempt { std::uint64_t vector; }` is
        # compared against the hypervisor's own top-level `vector` - a
        # different thing with the same name - and the result is a false
        # report, which is the failure mode a checker can least afford.
        at_class_level = (class_depth is not None and
                          depth == class_depth + 1)
        depth += line.count("{") - line.count("}")

        if not pending and not at_class_level:
            continue

        # A declaration begins at a line that starts one; anything else
        # resets, so a brace or a label cannot be carried into the next
        # statement.
        if not pending:
            if not re.match(r"^(static\s+constexpr\s+|volatile\s+|"
                            r"std::|bool\s|epte\s|arch::|zpp::|"
                            r"[A-Za-z_]\w*\s+[a-z_])", line):
                continue
            pending = line
        else:
            pending += " " + line

        if ";" not in pending:
            continue

        statement = " ".join(pending[:pending.index(";") + 1].split())
        pending = ""

        match = CONSTANT.match(statement)
        if match:
            constants[match.group("name")] = " ".join(
                match.group("value").split())
            continue

        match = MEMBER.match(statement)
        if match:
            bounds = match.group("bounds").replace(" ", "")
            members[match.group("name")] = (
                " ".join(match.group("type").split()), bounds)

    return members, constants


def main():
    if not os.path.isfile(REAL):
        print("missing {}".format(REAL), file=sys.stderr)
        return 2

    real_members, real_constants = declarations(REAL)

    if len(real_members) < 100:
        # The real header has hundreds of members. A handful means the
        # parser stopped understanding it, and a checker that silently
        # compares nothing is worse than none - which is the whole
        # lesson this script exists to apply.
        print("FAIL: parsed only {} members out of the real header - the "
              "parser no longer understands it, so nothing was actually "
              "compared".format(len(real_members)), file=sys.stderr)
        return 1

    print("== harness shims against hypervisor.h")
    print("   real header: {} members, {} constants".format(
        len(real_members), len(real_constants)))

    status = 0
    shims = sorted(
        os.path.join(ROOT, "tests", name, "shim", "zpp", "hypervisor",
                     "hypervisor.h")
        for name in os.listdir(os.path.join(ROOT, "tests"))
        if os.path.isfile(
            os.path.join(ROOT, "tests", name, "shim", "zpp", "hypervisor",
                         "hypervisor.h")))

    if not shims:
        print("FAIL: found no shims to check", file=sys.stderr)
        return 1

    for shim in shims:
        harness = os.path.relpath(shim, ROOT).split(os.sep)[1]
        members, constants = declarations(shim)

        shared = sorted(set(members) & set(real_members))
        only_here = sorted(set(members) - set(real_members))

        disagreements = []
        for name in shared:
            shim_type, shim_bounds = members[name]
            real_type, real_bounds = real_members[name]

            # The bounds always have to agree. A shim array of the wrong
            # rank or extent is the failure this exists for: it compiles,
            # and every index into it lands somewhere plausible.
            # A bound the hypervisor writes as a conditional -
            # `[nested_vmx::enabled?max_cpus:1]` - has two legitimate
            # values, and a harness that always compiles with the feature
            # on is right to carry the taken branch. Accepted only if the
            # shim's bound is one of the two, so a third value is still a
            # disagreement.
            settled = shim_bounds == real_bounds
            if not settled and "?" in real_bounds:
                for branch in re.findall(r"\?([^:\]]+):([^\]]+)",
                                         real_bounds):
                    for taken in branch:
                        if shim_bounds == re.sub(
                                r"[\w:]+\?[^\]]+", taken, real_bounds):
                            settled = True

            if not settled:
                disagreements.append(
                    "    {} has bounds {} in the shim and {} in the "
                    "hypervisor".format(name,
                                        shim_bounds or "(none)",
                                        real_bounds or "(none)"))
                continue

            # The element type has to agree only where it is a
            # fundamental one. A shim standing a class in for another -
            # `page_table_stub` for `arch::x86_64::page_table` - is the
            # whole point of a shim, and comparing those spellings would
            # report every harness as broken. An integer width is a
            # different matter: `std::uint64_t` where the hypervisor has
            # `std::uint32_t` is a stride, and a stride is what the
            # bounds check above is protecting.
            if not (shim_type.startswith("std::uint") or
                    shim_type.startswith("bool") or
                    real_type.startswith("std::uint") or
                    real_type.startswith("bool")):
                continue

            if shim_type != real_type:
                disagreements.append(
                    "    {} is {} in the shim and {} in the "
                    "hypervisor".format(name, shim_type, real_type))

        for name in sorted(set(constants) & set(real_constants)):
            if constants[name] != real_constants[name]:
                disagreements.append(
                    "    {} = {} in the shim, {} in the "
                    "hypervisor".format(name, constants[name],
                                        real_constants[name]))

        print("   {}: {} shared, {} harness-only".format(
            harness, len(shared), len(only_here)))

        if only_here:
            print("     harness-only: {}".format(" ".join(only_here)))

        if disagreements:
            print("  FAIL {} declares members the hypervisor declares "
                  "differently:".format(harness), file=sys.stderr)
            for entry in disagreements:
                print(entry, file=sys.stderr)
            print("     A shim member of the wrong shape compiles and "
                  "tests the wrong thing - one array index away from "
                  "reading a neighbour, with every assertion about it "
                  "passing.", file=sys.stderr)
            status = 1

    if 0 == status:
        print("   every shared declaration agrees")

    return status


if __name__ == "__main__":
    sys.exit(main())
