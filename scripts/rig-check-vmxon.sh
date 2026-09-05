#!/bin/sh
# Assert that every processor got as far as running a guest hypervisor.
#
# Three things have to be true of a healthy Hyper-V boot, and each has
# been false at some point in this tree's history:
#
#   1. every processor executes VMXON,
#   2. each one does so on a *distinct* VMXON region,
#   3. every processor that reached VMXON also reached a VM entry.
#
# The second is not pedantry. Two processors sharing a VMXON region is
# what a per-processor identifier taken from a shared counter produces -
# see c98c0f8, which derived a VPID from a slot for exactly this reason -
# and it reads as a working boot until something needs the two to differ.
#
# The third separates "the guest hypervisor never started" from "it
# started and stopped", which look identical from the outside and have
# nothing in common as failures.
#
# This exists because the boot investigation kept re-establishing the
# same fact by hand out of the log ring, and a fact re-established by
# hand is one nobody notices the day it stops being true.
#
# Reads the resident log ring through the monitor - no debugger, no
# processor inside the module, and nothing perturbed. Safe to run against
# a live guest, and against a wedged one, which is the point.
#
# Usage: scripts/rig-check-vmxon.sh [expected-cpu-count]
#        Default 8, the rig's topology.

set -e

EXPECTED=${1:-8}
LOG=${ZPP_LOG:-/tmp/zpp.log}

here=$(dirname "$0")

"$here/rig-dump-log-physical.sh" >/dev/null 2>&1 || {
    echo "could not read the log ring - is the guest up?" >&2
    exit 1
}

# One line per processor, and the region it used. `sort -u` on the pair
# rather than on the whole line so a repeat of the same processor on the
# same region does not hide a second processor on it.
vmxon=$(grep -a 'guest vmxon at' "$LOG" \
        | sed 's/.*cpu \(0x[0-9a-f]*\) guest vmxon at \(0x[0-9a-f]*\).*/\1 \2/' \
        | sort -u)

cpus=$(printf '%s\n' "$vmxon" | grep -c . || true)
regions=$(printf '%s\n' "$vmxon" | awk '{print $2}' | sort -u | grep -c . || true)
# The same treatment as `vmxon` above, and it did not have it: this was
# `grep -ac`, a count of matching LINES, printed and tested as a count
# of PROCESSORS. Two ways for that to be wrong in opposite directions.
# The log ring collapses a line identical to the one before it into a
# `[times=N]` marker rather than taking a slot, so two processors
# emitting textually identical entry lines count as one; and one
# processor entering twice with anything logged in between counts as
# two. The line carries its own processor - `nested_vmx.cpp:2912` logs
# "cpu {} entering the second level" - so extract it and `sort -u`.
entries=$(grep -a 'entering the second level' "$LOG" \
          | sed 's/.*cpu \(0x[0-9a-f]*\) entering the second level.*/\1/' \
          | sort -u | grep -c . || true)

echo "vmxon      : $cpus of $EXPECTED processors"
echo "regions    : $regions distinct"
echo "vm entries : $entries processors reached one"

status=0

if [ "$cpus" != "$EXPECTED" ]; then
    echo "FAIL: $cpus processors executed vmxon, expected $EXPECTED" >&2
    echo "      a processor that never reaches vmxon never runs a guest" >&2
    echo "      hypervisor, and Hyper-V will not start on it." >&2
    status=1
fi

if [ "$regions" != "$cpus" ]; then
    echo "FAIL: $cpus processors share $regions vmxon regions" >&2
    echo "      two processors on one region is the signature of a" >&2
    echo "      per-processor value taken from shared state." >&2
    printf '%s\n' "$vmxon" >&2
    status=1
fi

if [ "$entries" != "$EXPECTED" ]; then
    echo "FAIL: $entries processors reached a vm entry, expected $EXPECTED" >&2
    echo "      vmxon without an entry means the guest hypervisor came up" >&2
    echo "      and then declined to run anything - a different failure" >&2
    echo "      from never coming up, and they look the same from here." >&2
    status=1
fi

[ "$status" = 0 ] && echo "OK: every processor reached vmxon and a vm entry"

exit "$status"
