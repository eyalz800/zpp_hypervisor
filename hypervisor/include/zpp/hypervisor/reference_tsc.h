#pragma once

#include <climits>
#include <cstdint>

/**
 * The arithmetic of the Hyper-V reference TSC page, on its own.
 *
 * Separated from `publish_reference_tsc_page` for one reason: the whole
 * of what can be got wrong here is arithmetic, and arithmetic is the one
 * part of this VMM a hosted test can execute. The publishing side needs
 * a guest, extended page tables and a second-level VMCS; this header
 * needs `<cstdint>`, so `tests/reference_tsc` compiles it exactly as the
 * hypervisor compiles it.
 *
 * **The reference counter's rate is defined, not measured.** Hyper-V
 * partition reference time is counted in 100-nanosecond units, so the
 * counter advances at exactly 10 MHz by specification. Three independent
 * confirmations, because the code that used to live here *fitted* the
 * rate from samples and a fitted constant is a constant nobody checked:
 *
 * - The TLFS text, quoted verbatim in Xen's
 *   `xen/arch/x86/include/asm/guest/hyperv.h` above `hv_scale_tsc`:
 *   "ReferenceTime = ((VirtualTsc * TscScale) >> 64) + TscOffset. The
 *   multiplication is a 64 bit multiplication, which results in a 128
 *   bit number which is then shifted 64 times to the right to obtain the
 *   high 64 bits."
 * - Xen's own implementation, `xen/arch/x86/hvm/viridian/time.c`
 *   `update_reference_tsc`: `p->tsc_scale = ((10000UL << 32) /
 *   d->arch.tsc_khz) << 32;` beside the comment "Windows uses a 100ns
 *   tick, so we need a scale which is cpu ticks per 100ns shifted left by
 *   64". That expression is `10^7 * 2^64 / tsc_hz` with the division
 *   split to stay inside 64 bits, and `hyperv-tlfs.h` beside it defines
 *   `HV_CLOCK_HZ (NSEC_PER_SEC/100)`.
 * - KVM's `arch/x86/kvm/hyperv.c`: `compute_tsc_page_parameters` divides
 *   by 100 to convert nanoseconds to reference units, and
 *   `kvm_hv_get_time_ref_counter` reads the page back as
 *   `mul_u64_u64_shr(tsc, hv->tsc_ref.tsc_scale, 64) +
 *   hv->tsc_ref.tsc_offset`.
 *
 * All three agree on the shift, which is the thing worth being sure of:
 * getting it wrong by one is a factor of two in the guest's clock and
 * looks like a plausible number rather than a broken one.
 */
namespace zpp::hypervisor::reference_tsc
{
/**
 * Reference ticks per second. 100-nanosecond units, so 10^7 - by the
 * specification and not by observation. This is the number the whole
 * file exists to stop being fitted.
 */
inline constexpr std::uint64_t frequency = 10'000'000;

/**
 * `floor((numerator << 64) / denominator)`, for `numerator <
 * denominator` so the result fits.
 *
 * Shift and subtract rather than a 128-bit division, because there is no
 * runtime library here to supply `__udivti3` and a link failure at this
 * depth is a bad way to find that out. Sixty-four iterations, once.
 */
constexpr std::uint64_t shifted_quotient(std::uint64_t numerator,
                                         std::uint64_t denominator)
{
    if ((0 == denominator) || (numerator >= denominator)) {
        return 0;
    }

    std::uint64_t quotient{};
    std::uint64_t remainder = numerator;

    for (int i{}; i < 64; ++i) {
        auto carry = remainder >> 63;
        remainder <<= 1;
        quotient <<= 1;

        if ((0 != carry) || (remainder >= denominator)) {
            remainder -= denominator;
            quotient |= 1;
        }
    }

    return quotient;
}

/**
 * `((tsc * scale) >> 64)`, the reference TSC page's own arithmetic - the
 * high half of a 64x64 multiply, per the TLFS text quoted above.
 */
constexpr std::uint64_t scaled_tsc(std::uint64_t tsc, std::uint64_t scale)
{
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(tsc) * scale) >> 64);
}

/**
 * The scale a page must carry so that reference time advances at exactly
 * `frequency`, given a time-stamp counter of `tsc_hz`.
 *
 *     scale = 10^7 * 2^64 / tsc_hz
 *
 * Zero when `tsc_hz` is not a usable frequency. Anything at or below
 * `frequency` would make the quotient one or more and overflow the
 * fixed point, and there is no such processor - the guard exists so an
 * unenumerated zero cannot be published as a scale.
 */
constexpr std::uint64_t scale_for(std::uint64_t tsc_hz)
{
    return shifted_quotient(frequency, tsc_hz);
}

/**
 * **The check the fitted version could not make.** How fast a page
 * carrying `scale` makes reference time advance, in hertz, given the
 * real `tsc_hz`: one second of time-stamp counter pushed through the
 * page's own arithmetic.
 *
 * If this is not ~10,000,000 the scale is wrong, and a guest reading
 * that page believes a different second from the one it is living in.
 * The old fit validated itself by predicting a sample from *inside its
 * own baseline*, which proves the samples are collinear and cannot fail
 * on a wrong slope - and a wrong slope was the entire risk.
 */
constexpr std::uint64_t implied_frequency(std::uint64_t scale,
                                          std::uint64_t tsc_hz)
{
    return scaled_tsc(tsc_hz, scale);
}

/**
 * How far `implied_frequency` may sit from `frequency` before the scale
 * is called wrong: one part in a thousand.
 *
 * Chosen against what is being guarded rather than against what is
 * achievable. The failure this exists for was measured as a clock 1.58
 * times fast; the honest fits recorded in `BACKLOG.md` came out at
 * 9,998,562 Hz and 10,000,215 Hz, both inside 0.03%. So a thousandth
 * separates every fit that has ever been right from the one that was
 * wrong, with two orders of magnitude to spare on each side.
 */
inline constexpr std::uint64_t frequency_tolerance = frequency / 1000;

/** Whether a scale is close enough to right to be published. */
constexpr bool frequency_agrees(std::uint64_t scale, std::uint64_t tsc_hz)
{
    if ((0 == scale) || (0 == tsc_hz)) {
        return false;
    }

    auto implied = implied_frequency(scale, tsc_hz);
    auto difference = (implied > frequency) ? (implied - frequency)
                                            : (frequency - implied);

    return difference <= frequency_tolerance;
}

/**
 * The nominal core crystal clock frequency for a processor that
 * enumerates `CPUID.15H:EBX/EAX` but leaves `ECX` zero, from SDM Table
 * 22-95 "Nominal Core Crystal Clock Frequency". Zero when the table does
 * not name the part, which is the honest answer: a guessed crystal is a
 * guessed clock.
 *
 * `family` and `model` are the *display* family and model - CPUID.01H
 * EAX with the extended fields already folded in - because that is what
 * the table's `06_55H` style signatures mean.
 */
constexpr std::uint64_t nominal_crystal_frequency(std::uint32_t family,
                                                  std::uint32_t model)
{
    if (6 != family) {
        return 0;
    }

    switch (model) {
    // Intel Xeon Scalable, CPUID signature 06_55H.
    case 0x55:
        return 25'000'000;

    // "6th and 7th generation Intel Core processors and Intel Xeon W
    // Processor Family", 24 MHz. Skylake client (4E, 5E), Skylake-X
    // client W (55 is above), Kaby Lake and its refreshes (8E, 9E) -
    // and 8E is the rig's own part.
    case 0x4e:
    case 0x5e:
    case 0x8e:
    case 0x9e:
        return 24'000'000;

    // Goldmont, CPUID signature 06_5CH.
    case 0x5c:
        return 19'200'000;

    default:
        return 0;
    }
}

/**
 * The nominal time-stamp counter frequency from `CPUID.15H`, per SDM
 * 22.7.3 "Determining the Processor Base Frequency":
 *
 *     Nominal TSC frequency =
 *         ( CPUID.15H:ECX[31:0] * CPUID.15H:EBX[31:0] )
 *         / CPUID.15H:EAX[31:0]
 *
 * and, where the ratio is enumerated but `ECX` is not, Table 22-95 for
 * the crystal - which is `crystal_fallback` here.
 *
 * Zero when the leaf enumerates nothing, and **zero is the answer under
 * QEMU**: `cpu_x86_cpuid` in `target/i386/cpu.c` has no case for 0x15 or
 * 0x16 and its `default:` returns "reserved values: zero", and
 * `kvm_x86_build_cpuid` in `target/i386/kvm/kvm.c` builds the guest's
 * CPUID table from that function. KVM itself would have passed the leaf
 * through - `__do_cpuid_func` in `arch/x86/kvm/cpuid.c` has no case for
 * it either, so it keeps the host values `do_host_cpuid` read - but
 * QEMU decides what KVM is given.
 *
 * **CPUID.16H is deliberately not a fallback.** It reports the processor
 * base frequency, which is not the time-stamp counter's: the rig's part
 * is marketed at 1.80 GHz and its counter was measured at 1.9920 GHz -
 * 179,446,096,055 counts over a 90.08 second window read from outside,
 * recorded in `BACKLOG.md`. Leaf 0x15's own answer for that part,
 * 24 MHz crystal times a ratio of 83, is 1,992,000,000 - which agrees
 * with the measurement to four significant figures where leaf 0x16
 * would be 10.7% low. `MSR_PLATFORM_INFO` (0xCE) is out for the same
 * reason and for a second one: KVM resets it to `MSR_PLATFORM_INFO_
 * CPUID_FAULT` alone in `kvm_vcpu_reset`, so its ratio field reads zero
 * to any guest whose userspace has not written it.
 */
constexpr std::uint64_t
tsc_frequency_from_leaf_15(std::uint32_t eax,
                           std::uint32_t ebx,
                           std::uint32_t ecx,
                           std::uint64_t crystal_fallback)
{
    // "If 0, the TSC/core crystal clock ratio is not enumerated."
    if ((0 == eax) || (0 == ebx)) {
        return 0;
    }

    std::uint64_t crystal = (0 != ecx) ? ecx : crystal_fallback;
    if (0 == crystal) {
        return 0;
    }

    // The multiply is widened because nothing in the leaf's definition
    // bounds EBX, and then narrowed *before* the division on purpose: a
    // 128-bit divide compiles to a call to `__udivti3`, and there is no
    // runtime library here to supply it. That is not hypothetical - it
    // is a link failure this very function produced when it was first
    // written, which is the same trap `shifted_quotient` above exists to
    // avoid and the reason it is a loop.
    auto product = static_cast<unsigned __int128>(crystal) * ebx;
    constexpr auto limit = static_cast<unsigned __int128>(UINT64_MAX);

    if (product > limit) {
        return 0;
    }

    return static_cast<std::uint64_t>(product) / eax;
}

} // namespace zpp::hypervisor::reference_tsc
