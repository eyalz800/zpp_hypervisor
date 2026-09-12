/**
 * The Hyper-V reference TSC page's arithmetic.
 *
 * `zpp/hypervisor/reference_tsc.h` depends on `<cstdint>` and `<climits>`
 * and on nothing else, so it compiles here exactly as the hypervisor
 * compiles it - the same argument tests/mtrr and tests/small_map make.
 * Pure and `constexpr`, so most of what follows is asserted at compile
 * time and the compile is itself the test; the runtime half exists so a
 * failure names the number that disagreed.
 *
 * **What it pins, and why the file exists.** The scale on that page used
 * to be *fitted* - a slope through two of the guest hypervisor's own
 * answers for the counter MSR - and validated by predicting a third
 * sample from inside the same baseline. That check proves the samples are
 * collinear. It cannot fail on a wrong slope, and a wrong slope was the
 * entire risk: a constant factor on the guest's clock makes a periodic
 * timer fire at the wrong rate, which is measured in `BACKLOG.md` as a
 * 574.7 Hz tick arriving 906 times a second and the boot livelocking on
 * it.
 *
 * The rate does not need fitting because it is defined - Hyper-V
 * reference time is counted in 100-nanosecond units, so
 * `scale = 10^7 * 2^64 / tsc_hz` - and the assertions below are that
 * closed form, its round trip, and the two ways of getting it wrong that
 * would look plausible:
 *
 *   - the shift. `>> 63` instead of `>> 64` is exactly a factor of two
 *     and produces a scale of an entirely ordinary magnitude.
 *   - the units. Hertz where kilohertz was meant, or the 100-nanosecond
 *     tick read as a nanosecond one, is a factor of a thousand or a
 *     hundred, and each also produces a number that looks like a scale.
 *
 * Every one of those is caught by `implied_frequency` reading something
 * other than 10,000,000, which is the whole point of that function: it is
 * a check that can fail on the thing it protects against.
 */
#include "zpp/hypervisor/reference_tsc.h"

#include <cstdint>
#include <print>
#include <string>

namespace ref = zpp::hypervisor::reference_tsc;

namespace
{
std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}", what);
}

void check_equal(std::uint64_t expected,
                 std::uint64_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println(
        "FAIL: {}\n  expected {}\n  actual   {}", what, expected, actual);
}

/**
 * A frequency the round trip is asserted over, and a name for the
 * failure message. The rig's measured counter is first because it is the
 * one number in this file that came off hardware.
 */
struct part
{
    std::uint64_t tsc_hz;
    const char * what;
};

constexpr part parts[] = {
    // 179,446,096,055 counts over a 90.08 second window read from
    // outside the machine, per BACKLOG.md. Also exactly 24 MHz times 83,
    // which is what CPUID.15H would report for this part.
    {1'992'000'000, "the rig's measured counter, 1.992 GHz"},
    // The part's marketed base frequency, which is NOT its counter -
    // recorded because CPUID.16H would answer this and be 10.7% low.
    {1'800'000'000, "the rig's base frequency, 1.800 GHz"},
    {1'000'000'000, "1 GHz"},
    {2'000'000'000, "2 GHz"},
    {2'500'000'000, "2.5 GHz"},
    {3'000'000'000, "3 GHz"},
    {3'600'000'000, "3.6 GHz"},
    {4'700'000'000, "4.7 GHz"},
    // The lowest frequency the fixed point can carry, one tick above the
    // reference rate itself.
    {10'000'001, "one hertz above the reference rate"},
};

} // namespace

// ==== The closed form, at compile time ==================================

// 2 GHz is the case that can be written down by hand and checked without
// trusting the implementation: 10^7 * 2^64 / 2*10^9 is 2^64 / 200, which
// is 0x0147AE147AE147AE with the remainder discarded.
static_assert(0x0147AE147AE147AEull == ref::scale_for(2'000'000'000));

// And the shift is 64, not 63 or 65. Stated as its own assertion because
// getting it wrong by one is a factor of two on the guest's clock and
// nothing about the resulting number looks wrong.
static_assert((static_cast<unsigned __int128>(1'992'000'000) *
               ref::scale_for(1'992'000'000)) >>
                  64 ==
              ref::implied_frequency(ref::scale_for(1'992'000'000),
                                     1'992'000'000));

// The definition, in one line: a page carrying this scale advances
// reference time at ten million ticks a second.
//
// **To within one tick, not exactly, and the one tick is real.** Both
// halves of the arithmetic truncate - `scale_for` discards the remainder
// of `10^7 * 2^64 / tsc_hz`, and the page's own `(tsc * scale) >> 64`
// discards again - so a second of counter lands just under the rate and
// floors to 9,999,999. Xen's `update_reference_tsc` and KVM's
// `compute_tsc_page_parameters` both truncate in the same places, so this
// is the arithmetic every implementation of this page performs and not a
// defect in this one. One part in ten million; the failure this file
// exists for was one part in two.
static_assert(1 >= (10'000'000 -
                    ref::implied_frequency(ref::scale_for(1'992'000'000),
                                           1'992'000'000)));

// Nothing is publishable from a frequency that was never enumerated, and
// nothing is publishable from one at or below the reference rate - the
// fixed point cannot carry a quotient of one.
static_assert(0 == ref::scale_for(0));
static_assert(0 == ref::scale_for(ref::frequency));
static_assert(0 == ref::scale_for(1'000'000));
static_assert(!ref::frequency_agrees(0, 1'992'000'000));
static_assert(!ref::frequency_agrees(ref::scale_for(1'992'000'000), 0));

// ==== The two plausible ways to get it wrong ============================

// A thousand-fold units slip - kilohertz where hertz was meant - is
// rejected, and by the frequency check rather than by looking wrong.
static_assert(!ref::frequency_agrees(ref::scale_for(1'992'000),
                                     1'992'000'000));

// The scale actually measured on the rig, from BACKLOG.md's
// "The reference TSC page is right, and the clock lead is closed", is
// accepted: 0x0148f2db8d6da21a against a 1.992 GHz counter.
static_assert(ref::frequency_agrees(0x0148f2db8d6da21aull, 1'992'000'000));

// And a scale 1.58 times too fast - the failure this whole file exists
// for - is not. `906 / 574.7` scaled onto the same counter.
static_assert(!ref::frequency_agrees(
    ref::scale_for(1'992'000'000ull * 100 / 158), 1'992'000'000));

// ==== CPUID leaf 0x15, per SDM 22.7.3 ==================================

// The rig's part: a 24 MHz crystal from Table 22-95 because ECX is zero,
// and a ratio of 83. 24e6 * 166 / 2 = 1,992,000,000, which is the number
// the wall clock measured.
static_assert(1'992'000'000 ==
              ref::tsc_frequency_from_leaf_15(2, 166, 0, 24'000'000));

// ECX wins over the fallback when the leaf does enumerate it.
static_assert(2'000'000'000 == ref::tsc_frequency_from_leaf_15(
                                   2, 160, 25'000'000, 24'000'000));

// **Zero when the leaf enumerates nothing, which is what QEMU returns.**
// `cpu_x86_cpuid` has no case for 0x15 and its default is "reserved
// values: zero", and `kvm_x86_build_cpuid` builds the guest's table from
// it. A zero here must not become a published scale, which is what the
// `scale_for(0)` assertion above is paired with.
static_assert(0 == ref::tsc_frequency_from_leaf_15(0, 0, 0, 24'000'000));
static_assert(0 == ref::tsc_frequency_from_leaf_15(2, 0, 0, 24'000'000));
static_assert(0 == ref::tsc_frequency_from_leaf_15(0, 166, 0, 24'000'000));

// The ratio is enumerated but the part is not in Table 22-95 and the leaf
// does not name the crystal. A guessed crystal is a guessed clock, so the
// answer is that there is no answer.
static_assert(0 == ref::tsc_frequency_from_leaf_15(2, 166, 0, 0));

// Table 22-95 itself, by display family and model.
static_assert(24'000'000 == ref::nominal_crystal_frequency(6, 0x8e));
static_assert(25'000'000 == ref::nominal_crystal_frequency(6, 0x55));
static_assert(19'200'000 == ref::nominal_crystal_frequency(6, 0x5c));
static_assert(0 == ref::nominal_crystal_frequency(6, 0x97));
static_assert(0 == ref::nominal_crystal_frequency(15, 0x8e));

int main()
{
    // The round trip, over a spread of real frequencies: put a counter
    // frequency in, get a scale, push one second of that counter through
    // the page's own arithmetic, and land on ten million.
    //
    for (auto [tsc_hz, what] : parts) {
        auto scale = ref::scale_for(tsc_hz);
        check(0 != scale, std::string{"a scale exists for "} + what);

        // Within one tick rather than equal, for the reason the
        // static assertion above gives at length: the page's own
        // arithmetic truncates twice and so does every other
        // implementation of it. One part in ten million.
        auto implied = ref::implied_frequency(scale, tsc_hz);
        check(1 >= (ref::frequency - implied),
              std::string{"the implied reference frequency for "} + what +
                  " is " + std::to_string(implied));

        check(ref::frequency_agrees(scale, tsc_hz),
              std::string{"the scale agrees with the definition for "} +
                  what);

        // The two failures that look plausible, asserted at every
        // frequency rather than at one: half the clock and twice it are
        // what a shift wrong by one produces, in either direction.
        check(!ref::frequency_agrees(scale / 2, tsc_hz),
              std::string{"half the scale is rejected for "} + what);
        // Doubling is only a meaningful perturbation while it is
        // representable. At a counter barely above the reference rate
        // the scale is already most of 2^64, so `scale * 2` wraps to a
        // small number - which is rejected, but for the wrong reason,
        // and a check that passes for the wrong reason is worth less
        // than no check. The wrap is the real statement there: a shift
        // wrong by one cannot even be expressed.
        if (scale <= (UINT64_MAX / 2)) {
            check(!ref::frequency_agrees(scale * 2, tsc_hz),
                  std::string{"twice the scale is rejected for "} + what);
        } else {
            check(scale * 2 < scale,
                  std::string{"twice the scale is unrepresentable for "} +
                      what);
        }
    }

    // The offset construction the publisher uses, checked for what it is
    // for: reference time must not step when the scale is replaced.
    {
        constexpr std::uint64_t tsc_hz = 1'992'000'000;
        constexpr std::uint64_t now = 0x1234'5678'9abcull;

        // A scale fitted 1.58 times fast, and the offset that anchored it
        // on some earlier answer.
        auto wrong = ref::scale_for(tsc_hz * 100 / 158);
        std::uint64_t wrong_offset = 0x4000'0000ull;
        auto before = ref::scaled_tsc(now, wrong) + wrong_offset;

        // Re-anchored onto the computed scale, the way
        // `publish_reference_tsc_page` does it on a revision.
        auto right = ref::scale_for(tsc_hz);
        auto right_offset = before - ref::scaled_tsc(now, right);
        auto after = ref::scaled_tsc(now, right) + right_offset;

        check_equal(before,
                    after,
                    "replacing the scale does not step reference time");

        // And a second later the two clocks have diverged, which is the
        // point of replacing it.
        auto later = now + tsc_hz;
        auto wrong_later = ref::scaled_tsc(later, wrong) + wrong_offset;
        auto right_later = ref::scaled_tsc(later, right) + right_offset;

        check(1 >= (ref::frequency - (right_later - after)),
              "the computed scale advances ten million a second");
        check(wrong_later - before > ref::frequency + 5'000'000,
              "the fitted scale advanced far more than ten million");
    }

    // **The regression, stated as the contrast it is.** The removed
    // check fitted a slope through two of the guest hypervisor's answers
    // and then validated it by predicting a third answer taken from
    // between them, to a tolerance of 1000 hundred-nanosecond units.
    //
    // Give it samples that lie exactly on a line whose slope is 1.58
    // times too fast - which is what a five-millisecond baseline with
    // 2.5 ms of noise on each end produces - and it accepts them,
    // because collinear samples always reproduce themselves. It has
    // nothing to disagree with. The frequency check has the definition
    // to disagree with, and does.
    {
        constexpr std::uint64_t tsc_hz = 1'992'000'000;

        // A counter running 1.58 times fast against this time-stamp
        // counter, sampled at three points on its line.
        constexpr std::uint64_t wrong_hz = 15'800'000;
        constexpr std::uint64_t t1 = 0x4000'0000'0000ull;
        constexpr std::uint64_t span = tsc_hz / 200; // 5 ms of counter
        constexpr std::uint64_t tm = t1 + (span / 2);
        constexpr std::uint64_t t2 = t1 + span;

        auto reference_at = [](std::uint64_t tsc) {
            return static_cast<std::uint64_t>(
                (static_cast<unsigned __int128>(tsc) * wrong_hz) / tsc_hz);
        };

        auto r1 = reference_at(t1);
        auto rm = reference_at(tm);
        auto r2 = reference_at(t2);

        auto fitted = ref::shifted_quotient(r2 - r1, t2 - t1);
        auto fit_anchor = r2 - ref::scaled_tsc(t2, fitted);

        // The removed check, reproduced here and nowhere else, so the
        // contrast is made against the real thing rather than against a
        // description of it.
        auto predicted = ref::scaled_tsc(tm, fitted) + fit_anchor;
        auto difference =
            (predicted > rm) ? (predicted - rm) : (rm - predicted);

        check(difference <= 1000,
              "the removed collinearity check ACCEPTS a slope 1.58 times "
              "fast (difference " +
                  std::to_string(difference) + ")");

        // And what it could not see.
        auto implied = ref::implied_frequency(fitted, tsc_hz);
        check(!ref::frequency_agrees(fitted, tsc_hz),
              "the frequency check rejects the same slope, at " +
                  std::to_string(implied) + " Hz");

        // The number is the diagnostic `rig-dump-state.py` prints, so
        // pin it: 15.8 MHz where ten million is the definition.
        check(implied > 15'000'000 && implied < 16'000'000,
              "the implied reference frequency names the error: " +
                  std::to_string(implied) + " Hz");

        // Computed from the same counter, the scale is right - which is
        // the whole repair in two lines.
        check(ref::frequency_agrees(ref::scale_for(tsc_hz), tsc_hz),
              "the computed scale agrees with the definition");
    }

    // `shifted_quotient` against a division the host can do, since the
    // hypervisor cannot: it is the loop that exists so no `__udivti3` is
    // referenced.
    for (auto [tsc_hz, what] : parts) {
        auto expected = static_cast<std::uint64_t>(
            (static_cast<unsigned __int128>(ref::frequency) << 64) /
            tsc_hz);
        check_equal(expected,
                    ref::shifted_quotient(ref::frequency, tsc_hz),
                    std::string{"shift-and-subtract matches a real "
                                "128-bit division for "} +
                        what);
    }

    std::println("{} checks, {} failures", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
