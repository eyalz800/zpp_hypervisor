#!/usr/bin/env python3
"""Executable proof of the one claim the NVMe log channel rests on.

NVME-LOG.md argues that a hypervisor can borrow the guest's admin queue,
issue commands of its own, and hand the queue back with the guest unable
to tell.  The argument is arithmetic, and arithmetic is worth executing
rather than believing: this models both sides of the admin queue exactly
as NVMe 2.0 defines them and asserts the property directly.

    controller  posts at slot T with phase bit P, T += 1, and inverts P
                each time T wraps to 0.  It never reads the completion
                queue - 2.0 section 3.3.1.2, the tail "is only used
                internally by the controller and is not visible to the
                host" - which is what makes restoring the memory legal.
    guest       an entry at its head is new iff its phase bit equals the
                phase it expects; on consuming it the head advances and
                the expected phase inverts on wrap.  Both are software
                variables inside the guest's driver.  We cannot write
                them, and that is the whole difficulty.

Three things are asserted, and the second is the one that would be got
wrong by inspection:

 1. A lap of L commands leaves the guest in sync iff L % (2 * D) == 0.
 2. **One lap is not enough.**  L == D returns the tail but leaves the
    phase inverted, so the guest sees its next real completion as stale
    and hangs.  The counter-example battery below pins that down.
 3. A foreign completion arriving mid-lap - an Asynchronous Event
    Request, or Modern Standby's Set Features - is survivable, because
    the requirement is only that the guest consume exactly as many
    entries as the controller posted.  Replaying the foreign entries
    into the restored slots does that.

Run it: python3 scripts/nvme-lap-model.py
"""

import math
import sys


class Controller:
    """The completion queue as the controller drives it."""

    def __init__(self, depth, phase=1):
        self.depth = depth
        self.tail = 0
        self.phase = phase
        # (command id, phase bit) per slot. The host is required to zero
        # the queue before enabling, so a phase bit of 0 reads as stale.
        self.memory = [(None, 0)] * depth
        self.posted = 0

    def post(self, command_id):
        self.memory[self.tail] = (command_id, self.phase)
        self.tail += 1
        self.posted += 1
        if self.tail == self.depth:
            self.tail = 0
            self.phase ^= 1


class Guest:
    """The completion queue as the guest's driver believes it to be."""

    def __init__(self, depth, phase=1):
        self.depth = depth
        self.head = 0
        self.phase = phase

    def drain(self, memory):
        seen = []
        while memory[self.head][1] == self.phase:
            seen.append(memory[self.head][0])
            self.head += 1
            if self.head == self.depth:
                self.head = 0
                self.phase ^= 1
        return seen


def borrow(depth, settled, lap, foreign, follow=1000):
    """One borrow, start to finish. Returns (ok, explanation)."""
    controller = Controller(depth)
    guest = Guest(depth)

    # Ordinary life first, so the borrow does not always start at slot 0.
    for i in range(settled):
        controller.post(("guest", i))
        if guest.drain(controller.memory) != [("guest", i)]:
            return False, "the model itself diverged before the borrow"

    # The hypervisor takes over. The guest is stopped at a VM exit and
    # the admin interrupt vector is masked, so nothing of ours is ever
    # visible to it - which is the property the restore has to preserve.
    snapshot = list(controller.memory)
    resume_head, resume_phase = guest.head, guest.phase

    strays = []
    for issued in range(lap):
        controller.post(("hypervisor", issued))
        # A guest command completing in the middle of our window.
        if foreign and issued == lap // 2:
            controller.post(("stray", len(strays)))
            strays.append(("stray", len(strays)))

    # Hand it back. The memory goes back byte for byte; the doorbells go
    # back to values that, after a whole number of laps, the last write
    # of the lap already carries.
    controller.memory = list(snapshot)

    # Anything of the guest's that completed inside the window still has
    # to reach it, in order, at the slots it would have occupied.
    slot, phase = resume_head, resume_phase
    for stray in strays:
        controller.memory[slot] = (stray, phase)
        slot += 1
        if slot == depth:
            slot, phase = 0, phase ^ 1

    if guest.drain(controller.memory) != strays:
        return False, "the guest saw something that was not its own"

    # And it must track the controller from here on, indefinitely.
    for i in range(follow):
        controller.post(("after", i))
        if guest.drain(controller.memory) != [("after", i)]:
            return (False,
                    f"desynced after {i}: guest head {guest.head} "
                    f"phase {guest.phase}, controller tail "
                    f"{controller.tail} phase {controller.phase}")

    return True, (f"in sync (head {guest.head} == tail {controller.tail}, "
                  f"phase {guest.phase} == {controller.phase})")


def lap_length(submission_depth, completion_depth):
    """How many commands one borrow must issue.

    The submission queue only needs its tail returned, so a multiple of
    its depth. The completion queue needs the tail *and* the phase, and
    the phase only comes back on an even number of wraps - hence the two.
    """
    return math.lcm(submission_depth, 2 * completion_depth)


def main():
    failures = 0

    print("A borrow of lcm(sq, 2*cq) commands, over the queue depths that")
    print("real drivers use - Linux 32, Windows stornvme 256.\n")
    print(f"{'depth':>6} {'settled':>8} {'lap':>6} {'stray':>6}  verdict")

    for depth in (2, 8, 32, 64, 128, 256):
        lap = lap_length(depth, depth)
        for settled in (0, 1, depth - 1, depth, depth + 3, 2 * depth + 1):
            for foreign in (False, True):
                ok, why = borrow(depth, settled, lap, foreign)
                if not ok:
                    failures += 1
                print(f"{depth:>6} {settled:>8} {lap:>6} {int(foreign):>6}  "
                      f"{'OK ' if ok else 'BAD'} {why}")

    print("\nAnd the counter-examples. Every lap that is not a whole")
    print("number of *pairs* of laps must desync - especially L == D,")
    print("which returns the tail and inverts the phase.\n")

    depth = 32
    for lap in (1, 2, depth - 1, depth, depth + 1, 2 * depth - 1,
                2 * depth + 1, 3 * depth):
        ok, _ = borrow(depth, 3, lap, False)
        expected = (lap % (2 * depth)) == 0
        agrees = (ok == expected)
        if not agrees:
            failures += 1
        note = "in sync" if ok else "desync"
        print(f"  depth {depth} lap {lap:>3} -> {note:<8} "
              f"{'as predicted' if agrees else 'PREDICTION WRONG'}")

    print("\nLap length for the depths that matter:")
    for depth in (32, 64, 128, 256):
        print(f"  admin queue depth {depth:>4} -> "
              f"{lap_length(depth, depth):>4} commands per borrow")
    print("  (Linux uses 32, Windows stornvme 256, so a Windows borrow")
    print("   costs 512 admin commands - the dominant cost of an epoch.)")

    if failures:
        print(f"\n{failures} FAILURES")
        return 1
    print("\nevery assertion held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
