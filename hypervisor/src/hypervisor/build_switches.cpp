#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/hypervisor/nested_vmx.h"

namespace zpp::hypervisor
{
namespace
{
// Mirrors the constant in hypervisor.cpp, which is where the control is
// actually programmed. Spelled from the macro here because the constexpr
// bool it feeds is local to that translation unit.
#ifndef ZPP_SAMPLE_L1
#define ZPP_SAMPLE_L1 0
#endif
constexpr bool sample_l1_enabled = (0 != ZPP_SAMPLE_L1);

constexpr char digit(bool value)
{
    return value ? '1' : '0';
}

constexpr char digit(unsigned value)
{
    return static_cast<char>('0' + (value % 10));
}
} // namespace

/**
 * What the *compiler* saw, spelled into `.rodata` so that `strings` on the
 * built hypervisor - or on the loader that embeds it - answers "is this
 * switch actually in the binary" without a boot and without a debugger.
 *
 * **This exists because a CMake cache reading `ON` is not evidence.**
 * `ZPP_PUBLISH_REFERENCE_TSC:BOOL=ON` sat in both caches and in
 * `compile_commands.json` while the object file that consumed it was
 * stale, so `publish_reference_tsc_page` was linked in as a bare `ret`
 * and every measurement taken against that binary - two sessions of them -
 * was of a configuration nobody had built. The switch controlled the
 * single largest exit reason on the machine at the time.
 *
 * Built from the `constexpr bool`s the code branches on rather than from
 * the macros behind them, deliberately: a manifest assembled from `-D`
 * flags would have agreed with the cache and been just as wrong. These are
 * the same constants `if constexpr` is evaluated against, in a translation
 * unit that includes the same header, so a value here that disagrees with
 * behaviour means the *link* is inconsistent - which is the only failure
 * left once the compile is proven.
 *
 * `gnu::used` keeps the compiler from dropping it and `gnu::retain` marks
 * the section `SHF_GNU_RETAIN`, which is what survives `--gc-sections`;
 * `check-invariants.sh` builds with both.
 *
 * The one-second check, which is the whole point:
 *
 * ```sh
 * strings out/debug/x86_64/zpp_hypervisor | grep 'zpp switches'
 * ```
 */
extern "C" [[gnu::used, gnu::retain]] constinit const char
    zpp_build_switches[] = {
        'z', 'p', 'p', ' ', 's', 'w', 'i', 't', 'c', 'h', 'e', 's', ':',
        ' ', 'n', 'e', 's', 't', 'e', 'd', '=',
        digit(nested_vmx::enabled),
        ' ', 'e', 'v', 'm', 'c', 's', '=',
        digit(nested_vmx::evmcs_offered),
        ' ', 's', 'h', 'a', 'd', 'o', 'w', 'v', 'm', 'c', 's', '=',
        digit(nested_vmx::shadow_vmcs_enabled),
        ' ', 't', 'p', 'r', '=',
        digit(nested_vmx::tpr_shadow_offered),
        ' ', 'r', 'e', 'f', 't', 's', 'c', '=',
        digit(nested_vmx::publish_reference_tsc),
        ' ', 's', 'e', 'l', 'f', 'i', 'p', 'i', '=',
        digit(nested_vmx::deliver_self_ipi),
        ' ', 's', 'w', 'a', 'l', 'l', 'o', 'w', '=',
        digit(nested_vmx::intercept_self_ipi),
        ' ', 'f', 'o', 'r', 'c', 'e', 'd', 'p', 'c', '=',
        digit(nested_vmx::force_dispatch_once),
        ' ', 'e', 'a', 'g', 'e', 'r', 'e', 'p', 't', '=',
        digit(nested_vmx::eager_ept_neighbours),
        ' ', 'd', 'e', 'f', 'e', 'r', '=',
        digit(nested_vmx::defer_guest_state),
        ' ', 's', 'h', 'a', 'd', 'o', 'w', 'g', 's', '=',
        digit(nested_vmx::shadow_guest_state),
        ' ', 's', 't', 'e', 'p', 'v', 't', 'l', '=',
        digit(nested_vmx::step_vtl),
        ' ', 'q', 's', 't', 'a', 'r', 't', '=',
        digit(nested_vmx::apply_queued_start_up),
        ' ', 'p', 'r', 'o', 'f', 'i', 'l', 'e', '=',
        digit(nested_vmx::profile_l2),
        // Off leaves three fields of every exit-ring entry reading zero,
        // and zero is legal for all three - so this field is the only
        // thing that distinguishes "the guest was in state zero" from
        // "nobody paid to ask".
        ' ', 'c', 'e', 'n', 's', 'u', 's', '=',
        digit(nested_vmx::census_exits),
        // Off hands application processors to the guest unvirtualized,
        // so this is a correctness field and not a tuning one. It was
        // stale-OFF in `build/debug` for a session with nothing on the
        // deploy path able to say so.
        ' ', 'e', 'x', 'l', 'e', 'n', '=',
        digit(nested_vmx::honest_exit_length),
        ' ', 'l', '2', 's', 'p', 'i', 'n', '=',
        digit(nested_vmx::l2_startup_spin),
        ' ', 'a', 'p', 's', 't', 'e', 'p', '=',
        digit(nested_vmx::step_ap_watched_writes),
        ' ', 'd', 'r', 'o', 'p', 'w', '=',
        digit(nested_vmx::drop_watch_on_start_up),
        ' ', 'a', 'p', 't', 'w', '=',
        digit(nested_vmx::watch_ap_page_table),
        ' ', 'a', 'p', 'i', 'c', '=',
        digit(nested_vmx::intercept_apic),
        // Correctness again, not tuning: on, a processor that starts after
        // the watch is dropped runs outside this VMM.
        ' ', 'a', 'p', 'i', 'c', 'o', 'f', 'f', '=',
        digit(nested_vmx::disarm_apic_watch),
        // Microseconds, **five** digits. A manifest that cannot show the
        // value is no better than one that cannot show the switch, and
        // this project has already lost a session to a switch the
        // manifest could not show.
        //
        // It was four, and four was not enough. The leading digit was
        // `value / 1000 % 10`, so 10,000 printed `0000` - character for
        // character what *off* prints. A build configured with
        // -DZPP_LAZY_TICK=10000 was compiled, deployed to the rig and
        // read back as disabled; the switch was almost dismissed as
        // having no effect on that evidence. Ten milliseconds is not an
        // exotic value here either: the measured tick-handling cycle is
        // about 4 ms, so every useful setting is over 9,999.
        ' ', 'l', 'a', 'z', 'y', '=',
        digit(static_cast<unsigned>(
            nested_vmx::lazy_tick_microseconds / 10000)),
        digit(static_cast<unsigned>(
            nested_vmx::lazy_tick_microseconds / 1000)),
        digit(static_cast<unsigned>(
            nested_vmx::lazy_tick_microseconds / 100)),
        digit(static_cast<unsigned>(
            nested_vmx::lazy_tick_microseconds / 10)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_microseconds)),
        // Seven digits, because the site that consumes this refuses any
        // value at or above 10,000,000 as an absolute deadline rather
        // than a period, so every legal setting fits and none can be
        // truncated into a different legal one.
        //
        // **This is the switch that most needed printing and was the one
        // the manifest could not say.** It took the boot from stalling at
        // `VBoxSup.sys` to reaching ring 3, so which value was compiled
        // now decides what every measurement means - and it was invisible
        // to `strings` for exactly the reason the array exists. It stayed
        // invisible because it is a *value* and everything here was a
        // boolean, which is a gap in the manifest and not in the switch.
        ' ', 'f', 'l', 'o', 'o', 'r', '=',
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units /
                                    1000000)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units /
                                    100000)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units / 10000)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units / 1000)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units / 100)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units / 10)),
        digit(static_cast<unsigned>(nested_vmx::tick_floor_units)),
        // Two digits for both multipliers, where every switch above
        // needs one. They are free-form `CACHE STRING`s rather than
        // booleans - `BACKLOG.md` records the stretch run at 1, 2 and 8,
        // and nothing stops 16 - and at 16 a single digit prints `6`.
        // **A manifest that disagrees with the build is worse than no
        // manifest**, which is the whole argument for this array, so a
        // place where it could disagree is a place to widen.
        //
        // The comment used to sit between `stretch` and the switch after
        // it while applying only to `dilate` two lines further down,
        // which is how `stretch` kept one digit for a session.
        ' ', 's', 't', 'r', 'e', 't', 'c', 'h', '=',
        digit(static_cast<unsigned>(ZPP_STRETCH_GUEST_TIMER) / 10),
        digit(static_cast<unsigned>(ZPP_STRETCH_GUEST_TIMER)),
        // The first-level sampler. On, every processor pays an exit a
        // millisecond for ever, so a run that has it on is not comparable
        // with one that does not - which is exactly what a manifest field
        // is for.
        ' ', 's', 'a', 'm', 'p', 'l', '1', '=',
        digit(sample_l1_enabled),
        // The IUM block watch. On, a live kernel stack page loses
        // write permission and every write to it is an exit, so a run
        // with this on is not comparable with one without.
        // Whether the guest is told it is virtualized at all.
        ' ', 'h', 'v', 'b', 'i', 't', '=',
        digit(nested_vmx::announce_hypervisor_bit),
        // The trust-level trace. Observational, so it cannot change what
        // the guest sees - but it is the difference between a dump full
        // of switch records and one where every VTL field reads zero,
        // which looks exactly like a guest that never used them.
        // Whether this VMM talks to the layer below through a shared
        // page instead of VMREAD and VMWRITE. A correctness field, not a
        // tuning one: on, the second-level VMCS is not a VMCS at all, and
        // two runs that differ in it are not comparable.
        // Off leaves the VMCS field and caller tables reading zero,
        // which is indistinguishable from a VMM that made no accesses.
        ' ', 'c', 'e', 'n', 's', 'v', '=',
        digit(arch::x86_64::vmx::vmcs_census_enabled),
        ' ', 'e', 'v', 'm', 'k', '=',
        digit(nested_vmx::evmcs_to_kvm),
        ' ', 'e', 'v', 'm', 'i', 'x', '=',
        digit(nested_vmx::evmcs_mixed),
        ' ', 's', 't', 'a', 'l', 'l', '=',
        digit(nested_vmx::stall_breaker),
        // Correctness, not tuning: off, a permission the guest
        // hypervisor revoked can still be served by another
        // processor's shadow.
        ' ', 'i', 'n', 'v', 'a', 'l', 'l', '=',
        digit(nested_vmx::invept_all_processors),
        // Correctness, not tuning, and the reason it is here: on, the
        // local APIC page loses write permission partition-wide, so an
        // application processor's own ordinary bring-up writes - its
        // logical destination, its destination format, its spurious
        // vector - each become a VM exit, none of which can carry a
        // start-up IPI. Two runs differing in it are not comparable,
        // and it had every edit but this one for the whole time it
        // mattered.
        ' ', 'a', 'p', 'i', 'c', '=',
        digit(nested_vmx::intercept_apic),
        ' ', 'v', 't', 'l', 't', 'r', 'c', '=',
        digit(nested_vmx::trace_vtl),
        ' ', 'b', 'l', 'k', 'w', '=',
        digit(nested_vmx::watch_vtl_block),
        ' ', 'v', 't', 'l', 'c', 'a', 'p', '=',
        digit(nested_vmx::capture_vtl_deeply),
        ' ', 'd', 'i', 'l', 'a', 't', 'e', '=',
        digit(static_cast<unsigned>(nested_vmx::time_dilation / 10)),
        digit(static_cast<unsigned>(nested_vmx::time_dilation)),
        // The framebuffer record. Off, every framebuffer field in the
        // singleton reads zero - which is indistinguishable from a
        // machine whose firmware reported no graphics output protocol,
        // and from a loader that found one and failed to hand it over.
        // Three causes, one reading, so the manifest is what separates
        // "not built" from the other two.
        ' ', 'f', 'b', '=',
        digit(nested_vmx::framebuffer_recorded),
        // The VMCS field cache. A correctness field rather than a tuning
        // one: on, a read may be answered from memory instead of from
        // the processor, and the launch path points the live GS base at
        // this processor's row. Two runs that differ in it are not
        // comparable and one of them boots differently.
        ' ', 'v', 'c', 'a', 'c', 'h', 'e', '=',
        digit(arch::x86_64::vmx::vmcs_cache_enabled),
        // Virtual-interrupt delivery. A correctness field: on, the
        // processor decides when a pending vector may be delivered
        // instead of the level above polling for it, so two runs that
        // differ in it are not comparable at all.
        ' ', 'v', 'i', 'd', '=',
        digit(nested_vmx::virtual_interrupt_delivery_offered),
        // **This switch had three of its four edits and not this one.**
        // The option, the forward and the `constexpr bool` were all
        // there - `CMakeLists.txt` 261, 399 and 512,
        // `cmake/hypervisor/CMakeLists.txt` 115 and 298, and
        // `nested_vmx.h` 2220 - so a cache reading ON looked like
        // proof, and the manifest could not contradict it. That is
        // precisely the failure this array exists to stop, and it went
        // unnoticed on the switch aimed at the largest interrupt-window
        // storm on the machine.
        //
        // A correctness field rather than a tuning one: on, an
        // interrupt window the level above asked for is **withheld**
        // while the task priority blocks the dispatch class, and a TPR
        // threshold is armed in its place. The level above is therefore
        // woken at a different moment, so two runs that differ in this
        // are not comparable.
        ' ', 'w', 'i', 'n', 'd', 'o', 'w', 't', 'p', 'r', '=',
        digit(nested_vmx::window_on_tpr),
        // Clears the notification flag on a VTL1 entry, so the secure
        // kernel works instead of yielding. A deliberate lie to the
        // guest, so a run with it on describes a different machine.
        ' ', 'n', 'o', 'v', 'i', 'n', 'a', '=',
        digit(nested_vmx::suppress_vina),
        '\0'};

} // namespace zpp::hypervisor
