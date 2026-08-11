#pragma once
// The states a processor's start-up hand-off takes.
//
// A header of its own, and the reason is that this is one of the few
// things in the hypervisor a *test* has to know the exact numbers of.
// tests/ap_start_up asserts things like "a hand-off of vector zero is
// delivered and is not the same word as no hand-off at all", which is a
// claim about these values and not about the harness's idea of them - and
// the harness replaces `hypervisor.h` wholesale with a shim, so anything
// declared inside that class is somewhere a silent disagreement can live.
//
// It used to be a nested struct in the class and a second copy in the
// shim, compared textually by `sed` from tests/ap_start_up/build.sh. That
// worked, and it was the wrong shape twice over: a check that runs only
// when a shell script runs, over a copy that only exists because there
// was nowhere else to put it. There is one definition now, both classes
// alias it, and the invariants the comparison was protecting are
// static_asserts below - they cannot be skipped and they cost nothing.
#include <cstdint>

namespace zpp::hypervisor
{
/**
 * The states a processor's start-up hand-off takes, held in
 * `hypervisor::start_up_handoff`.
 *
 * There are two ways a start-up IPI can reach a processor coming out of
 * an INIT, and they are mutually exclusive: the architectural one, where
 * the processor parks in the wait-for-SIPI activity state and the
 * hardware delivers the IPI as a VM exit, and this VMM's own, where the
 * sending processor hands the vector over through memory because a layer
 * below would discard the hardware one.
 *
 * Which one is in use has to be one fact rather than two opinions. The
 * target chooses, publishes the choice here, and the sender obeys it -
 * and because the target can stop waiting at any moment, the sender's
 * hand-over is a compare-exchange out of `software_wait` rather than a
 * store. That is what makes it impossible for the sender to consume an
 * IPI the target is expecting from hardware, which is precisely how one
 * used to be lost.
 */
struct start_up_handoff_state
{
    /**
     * No hand-off in progress. Either this processor has never taken an
     * INIT exit, or its last start-up has already been applied. A sender
     * must let the hardware deliver.
     */
    static constexpr std::uint64_t none = 0;

    /**
     * The target is spinning in its INIT handler, in VMX root mode,
     * waiting for a vector to be handed to it. Only in this state may a
     * sender swallow the guest's write to the interrupt command register.
     */
    static constexpr std::uint64_t software_wait = 1;

    /**
     * The target is parked in the wait-for-SIPI activity state and is
     * waiting on hardware. A sender must issue the guest's start-up IPI,
     * because nothing this VMM does will wake it.
     */
    static constexpr std::uint64_t hardware_wait = 2;

    /**
     * A vector has been handed over. The state is this plus the vector,
     * so that vector zero is still distinguishable from no hand-off at
     * all.
     */
    static constexpr std::uint64_t delivered = 3;

    /**
     * The state that carries the given start-up vector.
     */
    static constexpr std::uint64_t deliver(std::uint64_t vector)
    {
        return delivered + vector;
    }

    /**
     * Whether the given state carries a vector.
     */
    static constexpr bool is_delivered(std::uint64_t state)
    {
        return state >= delivered;
    }

    /**
     * The vector such a state carries.
     */
    static constexpr std::uint64_t vector(std::uint64_t state)
    {
        return state - delivered;
    }
};

// The relationships between the four numbers, asserted here rather than
// left to a test to discover. Each of these is what makes some line of
// the start-up path correct, and none of them survives a value being
// changed on its own.
static_assert(start_up_handoff_state::none !=
                  start_up_handoff_state::software_wait,
              "a processor that has never taken an INIT must be "
              "distinguishable from one waiting in its INIT handler");
static_assert(start_up_handoff_state::software_wait !=
                  start_up_handoff_state::hardware_wait,
              "which of the two mechanisms is in use is the whole "
              "decision a sender makes");
static_assert(start_up_handoff_state::none !=
                  start_up_handoff_state::hardware_wait,
              "and a sender must not read a processor waiting on "
              "hardware as one that has never asked");

// is_delivered is a comparison rather than a set membership test, and
// that is only sound while all three named states sort below the first
// delivered one.
static_assert(start_up_handoff_state::none <
                  start_up_handoff_state::delivered,
              "is_delivered is `state >= delivered`");
static_assert(start_up_handoff_state::software_wait <
                  start_up_handoff_state::delivered,
              "is_delivered is `state >= delivered`");
static_assert(start_up_handoff_state::hardware_wait <
                  start_up_handoff_state::delivered,
              "is_delivered is `state >= delivered`");

// Vector zero is a real start-up vector - it starts a processor at
// physical address zero - so a hand-off carrying it must not be the word
// that means no hand-off at all. This is why `delivered` is an offset
// rather than the vector being stored bare.
static_assert(start_up_handoff_state::is_delivered(
                  start_up_handoff_state::deliver(0)),
              "vector zero must read back as a delivered hand-off");
static_assert(start_up_handoff_state::deliver(0) !=
                  start_up_handoff_state::none,
              "vector zero must not be the same word as no hand-off");
static_assert(0 == start_up_handoff_state::vector(
                       start_up_handoff_state::deliver(0)),
              "and it must come back out as the vector that went in");

// The whole 8-bit vector space has to fit above the three named states
// without wrapping, since the state is stored in one 64-bit word.
static_assert(start_up_handoff_state::deliver(0xff) >
                  start_up_handoff_state::delivered,
              "every one of the 256 vectors must be representable");

} // namespace zpp::hypervisor
