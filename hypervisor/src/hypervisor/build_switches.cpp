#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/config.h"
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
        ' ', 'v', 't', 'l', '1', 'c', 'l', 'k', '=',
        digit(nested_vmx::hold_clock_in_vtl1),
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
        ' ', 'e', 'f', 'e', 'r', '0', '=',
        digit(nested_vmx::init_clears_efer),
        // Correctness, not tuning, and it reads beside `efer0=` because
        // the two are one mechanism: `efer0=1` makes the guest EFER
        // field authoritative, and this is the only thing that then
        // keeps "IA-32e mode guest" and EFER.LMA agreeing with CR0.PG
        // when the guest turns paging on. Off, an application processor
        // that reaches long mode on its own is refused entry with
        // reason 0x80000021 and halts, which looks from outside exactly
        // like a processor that never started.
        ' ', 'l', 'm', 's', 'w', 'i', 't', 'c', 'h', '=',
        digit(nested_vmx::track_long_mode_switch),
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
        // How long that gap stays in force. Zero is for ever, which is
        // what wedged the machine; a bounded window is the only shape
        // of it that has not been tried.
        ' ', 'l', 'a', 'z', 'y', 's', 'e', 'c', '=',
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_seconds / 100)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_seconds / 10)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_seconds)),
        ' ', 'p', 'o', 'l', 'l', 'l', '1', '=',
        digit(nested_vmx::poll_l1),
        ' ', 'l', 'z', 'm', 'a', 'x', '=',
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_max / 10)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_max)),
        ' ', 'a', 'f', 't', 'p', 'r', 'o', 't', '=',
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_after_protect / 10000)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_after_protect / 1000)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_after_protect / 100)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_after_protect / 10)),
        digit(static_cast<unsigned>(nested_vmx::lazy_tick_after_protect)),
        ' ', 'h', 'l', 't', 'p', 'o', 'l', 'l', '=',
        digit(nested_vmx::poll_on_halt),
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
        // The **opposite** of the field above, not a repair of it. On,
        // the interrupt window is never withheld and a TPR threshold is
        // added where the level above left none. A correctness field
        // all the same: the level above is woken at moments it was not
        // before, so two runs that differ in it are not comparable.
        //
        // Read it beside `windowtpr=`. Withholding the window took the
        // clock from 388,241 delivered vectors to 5,550 in one measured
        // run, so a boot where both read 1 - which a static assertion
        // now refuses to build - would be the bad half of that.
        //
        // It is in the manifest from the first commit that has it,
        // which is the whole lesson of the field above - that one had
        // three of its four edits, and the cache reading ON was taken
        // as proof for a switch nobody had built.
        ' ', 'd', 'r', 'o', 'p', '=',
        digit(nested_vmx::deliver_on_drop),
        // The accounting behind it. A *tuning* field rather than a
        // correctness one - it only counts - but it costs a guest read
        // and a VMREAD per second-level entry, so a run with it on is
        // slower than one without and the two should not be compared
        // for rate. Read it as "were the four drop counters even
        // compiled in": all four reading zero means this is 0, not
        // that nothing was dropped.
        ' ', 'd', 'r', 'o', 'p', 'c', 'n', 't', '=',
        digit(nested_vmx::count_dropped_requests),
        // Clears the notification flag on a VTL1 entry, so the secure
        // kernel works instead of yielding. A deliberate lie to the
        // guest, so a run with it on describes a different machine.
        ' ', 'n', 'o', 'v', 'i', 'n', 'a', '=',
        digit(nested_vmx::suppress_vina),
        // The application-processor state trace. Observational, so it
        // cannot change what the guest sees - but it is the difference
        // between a log that says which application of the start-up
        // state killed a processor and one where that question has no
        // answer at all, and a reader who finds no `zpp-state` lines
        // must be able to tell "the switch was off" from "no
        // application processor was ever entered". This field is the
        // first half of that; the `ap-entry instrument armed` line the
        // boot processor writes at its own launch is the second.
        ' ', 'a', 'p', 'e', 'n', 't', 'r', 'y', '=',
        digit(nested_vmx::trace_ap_entry),
        // The application-processor fault trap. Observational - it
        // disarms itself on the first capture and resumes without
        // advancing RIP or injecting anything, so the boot ends as it
        // did before - but it is the only thing that distinguishes
        // "`ap_fault` reads zero because nothing faulted" from
        // "`ap_fault` reads zero because this was never built". Both
        // readings are indistinguishable in a dump, and the record is
        // the whole output of the instrument.
        ' ', 'a', 'p', 'f', 'a', 'u', 'l', 't', '=',
        digit(nested_vmx::trap_ap_faults),
        // The application-processor liveness probe, for exactly the
        // reason the fault trap above is here: `ap_probe_sent` reading
        // zero means either that nothing probed or that this was never
        // built, and those are the same six zeroes in a dump. This is
        // the field that separates them, and the reader says so rather
        // than guessing.
        ' ', 'p', 'r', 'o', 'b', 'e', '=',
        digit(nested_vmx::probe_aps),
        // === The diagnostic channel ==============================
        //
        // **This whole class was invisible here, and it is the one
        // class that can make a real controller look dead to the
        // guest.** Every field above is a `nested_vmx::` constant or a
        // VMCS one; nothing said anything about `zpp::diag`, so the
        // instrument that exists to answer "what is actually compiled
        // in" could not answer it for the switches that reach a
        // passed-through disk.
        //
        // What hangs off `blocks=`, which is
        // `diag::policy_of(diag::sink::esp_blocks).present`:
        // `shadow_controller_registers` re-points the NVMe's BAR0
        // extended-page-table entry at a RAM shadow with **CC.EN and
        // CSTS.RDY forced to zero**, and `reserve_channel_queue_-
        // allocation` write-protects the doorbell page. Both are inert
        // while it reads 0. A boot where it reads 1 is a boot where the
        // guest's driver can be told its controller is disabled, and
        // "the disk is dead" is exactly the reading this project has
        // already taken twice off a wide `xp` and once off a doorbell.
        //
        // `diag=` is the facility itself, forced to 0 in release, and
        // `win=` is the loader's reservation of a tail of the EFI
        // system partition - which edits firmware tables the guest
        // reads and takes space from a real disk, so it is worth
        // reading before a boot rather than after one.
        //
        // Read from `zpp::diag`'s own `constexpr bool`s and from
        // `policy_of` itself, not from the `-D` macros behind them,
        // for the reason at the top of this file: a manifest assembled
        // from the flags would have agreed with the cache.
        ' ', 'd', 'i', 'a', 'g', '=',
        digit(diag::enabled),
        ' ', 'b', 'l', 'o', 'c', 'k', 's', '=',
        digit(diag::policy_of(diag::sink::esp_blocks).present),
        ' ', 'w', 'i', 'n', '=',
        digit(diag::reserve_controller_window),
        '\0'};

} // namespace zpp::hypervisor
