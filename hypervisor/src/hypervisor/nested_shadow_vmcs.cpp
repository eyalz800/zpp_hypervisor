#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include "zpp/scope_exit.h"
#include <cstdint>

/**
 * VMCS shadowing: letting the guest hypervisor read and write its own
 * VMCS without an exit.
 *
 * Separate from nested_vmx.cpp because this is the only part of the
 * nested state machine that executes VMX instructions. That file is
 * compiled natively by tests/nested_vmx as a differential harness, on a
 * machine with no VMX at all, and VMPTRLD in it would not assemble.
 * Everything here is stubbed in that harness's shim instead, which is
 * also what a processor that does not offer the control does.
 */
namespace zpp::hypervisor
{
using arch::x86_64::vmx::vmcs_field_encoding;

namespace
{
using field = arch::x86_64::vmx::vmcs::field;

/**
 * **Both lists are the measured hot set and deliberately nothing more.**
 *
 * A field left off is not a bug: the guest hypervisor's access to it
 * exits and this VMM emulates it, which is exactly what happens with the
 * whole feature switched off. So the lists trade a copy on every
 * second-level exit against an exit whenever that field is touched, and
 * the only way to place that trade is to count what the guest hypervisor
 * in front of you actually touches.
 *
 * Counted on the rig with the feature off, so `vmcs_field_use` could see
 * everything rather than only what it does not shadow - Hyper-V, one
 * processor, one run: 5,095,645 reads over 25 distinct fields and
 * 2,176,011 writes over 106. The distribution is not close:
 *
 *   read   interruptibility 18.4%, instruction length 17.8%,
 *          exit reason 17.8%, CS access rights 16.6%, RIP 16.6%,
 *          RFLAGS 8.1%, SS access rights 2.0%, DR7 1.6%,
 *          entry interruption 1.3%           -> 99.6% in nine fields
 *   write  RIP 38.8%, interruptibility 36.0%, TPR threshold 11.3%,
 *          primary controls 10.4%,
 *          entry interruption 3.5%           -> 99.98% in five fields
 *
 * Their union is the eleven fields below. Everything dropped was
 * measured in the tens: `guest_cr4` twenty reads and twelve writes,
 * `guest_cr0` seven and seven, `guest_cr3` four and seven,
 * `exit_qualification` 146 reads, `guest_physical_address` and
 * `guest_linear_address` and `idt_vectoring_information` 129 each - and
 * the whole of the segment-base and access-right block seven writes
 * apiece, which is a guest hypervisor building a VMCS once and never
 * touching it again.
 *
 * What the trim is worth: the lists were 10 read-only and 26
 * read-write, so 62 VMCS accesses per second-level exit, against 20 now.
 * Every one of those is a VMX instruction trapping to the layer below at
 * a measured 1.76 us. The exits it gives up are the 0.4% tail.
 *
 * **Re-measure before editing this.** The previous list was assembled
 * from KVM's `vmcs_shadow_fields.h` plus one measurement, and carried
 * fifteen fields this guest hypervisor touches single-digit times.
 */

/**
 * The fields the guest hypervisor may read without an exit.
 *
 * The exit-information fields are here and not in the writable list
 * because they are read-only to a guest hypervisor: it reads them on
 * every exit it takes, and VMWRITE to one of them faults unless the
 * processor reports "VMWRITE to any supported field", which this VMM does
 * not report. So they need copying in one direction only, at the point
 * this VMM writes them - the reflection.
 */
constexpr field shadow_read_only_fields[] = {
    field::exit_reason,
    field::vm_exit_instruction_length,
};

/**
 * The fields the guest hypervisor may both read and write without an
 * exit.
 *
 * Every one of these has to be copied *both* ways, and the ordering rule
 * that keeps that correct is in copy_shadow_to_vmcs12: the guest
 * hypervisor's silent writes are collected before this VMM overwrites the
 * region, never after.
 *
 * Guest state that a VM exit reports and a VM entry consumes - RIP, RSP,
 * RFLAGS, the control registers, interruptibility - is the bulk of it,
 * because that is what an exit handler reads on the way in and writes on
 * the way out. The entry-interruption fields are here for the same
 * reason: injecting an event is a write, and it happens on the path this
 * is trying to make free.
 */
using nested_vmx::shadow_read_write_fields;

/**
 * Clears a field's bit in a bitmap, meaning "answer this one from the
 * shadow region rather than exiting".
 *
 * The bitmaps are indexed by the encoding directly, and only encodings
 * below 0x8000 are indexable - SDM 26.2 sends anything above that to a VM
 * exit whatever the bitmap says. Every encoding this VMM shadows is far
 * below it, so a value that is not is a mistake rather than a case to
 * handle, and it is left set.
 */
[[maybe_unused]] constexpr void permit_field(std::uint8_t * bitmap,
                                             std::uint64_t encoding)
{
    if (encoding >= 0x8000) {
        return;
    }

    bitmap[encoding / 8] &=
        static_cast<std::uint8_t>(~(1u << (encoding % 8)));
}

/**
 * Whether a field is answered from the shadow region rather than by an
 * exit - the inverse of what `permit_field` leaves behind, since a clear
 * bit is the permission.
 */
[[maybe_unused]] constexpr bool field_shadowed(
    const std::uint8_t * bitmap, std::uint64_t encoding)
{
    if (encoding >= 0x8000) {
        return false;
    }

    return 0 ==
           (bitmap[encoding / 8] & static_cast<std::uint8_t>(
                                       1u << (encoding % 8)));
}

} // namespace

/**
 * Stands VMCS shadowing down when the guest hypervisor keeps exiting for
 * fields the shadow bitmaps permit.
 *
 * **The capability MSR is not evidence that the feature works.** This
 * VMM sets `vmcs_shadowing_enabled` from the allowed-1 half of
 * IA32_VMX_PROCBASED_CTLS2, and under KVM that bit is advertised
 * *unconditionally* - `nested_vmx_setup_ctls_msrs` sets
 * SECONDARY_EXEC_SHADOW_VMCS outside any test, under the comment "We can
 * emulate 'VMCS shadowing,' even if the hardware doesn't support it".
 * Whether it is then honoured depends on the `enable_shadow_vmcs` module
 * parameter, and with it clear `prepare_vmcs02` strips the control before
 * hardware ever sees it. Measured on the rig: `enable_shadow_vmcs` is
 * **N**.
 *
 * The cost of believing it is exact and large: `copy_vmcs12_to_shadow`
 * and `copy_shadow_to_vmcs12` maintain the region on every second-level
 * exit at twenty VMCS accesses, measured together at **75,970 cycles an
 * entry - 20% of the exit** - and the guest hypervisor's reads and writes
 * trap anyway, because the control was removed. Twenty accesses spent to
 * avoid exits that are not being avoided.
 *
 * So the test is the *effect*, not the capability: an exit for a field
 * the read or write bitmap permits is proof the control is not in force,
 * because that is precisely the exit it exists to prevent. On a processor
 * where shadowing works this never fires and nothing changes.
 *
 * The threshold is not a confidence interval - one such exit already
 * proves it. It is there because the control is armed per VMCS and a
 * handful of exits can precede the arming, and 64 is far above that while
 * being reached in milliseconds when the feature is dead.
 *
 * Standing down is always safe: it reverts to exiting for every field,
 * which is what the guest hypervisor is already experiencing, and it is
 * what this VMM did before shadowing existed. The control is cleared
 * before the flag, because `set_vmcs_shadowing` returns early once the
 * flag is false.
 */
void hypervisor::note_shadowing_ineffective(std::size_t cpu,
                                            std::uint64_t encoding,
                                            bool write)
{
    if constexpr (!nested_vmx::enabled) {
        static_cast<void>(cpu);
        static_cast<void>(encoding);
        static_cast<void>(write);
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        const auto * bitmap = write ? this->vmcs_shadow_write_bitmap
                                    : this->vmcs_shadow_read_bitmap;

        if (!field_shadowed(bitmap, encoding)) {
            return;
        }

        if (cpu < max_cpus) {
            this->shadowing_ineffective[cpu] =
                this->shadowing_ineffective[cpu] + 1;
        }

        constexpr std::uint64_t enough = 64;

        std::uint64_t seen{};
        for (auto count : this->shadowing_ineffective) {
            seen += count;
        }

        if (seen < enough) {
            return;
        }

        log("vmcs shadowing is offered but not in force - {} exits for "
            "shadowed fields; standing it down and saving the copies",
            seen);

        set_vmcs_shadowing(cpu, false);
        this->vmcs_shadowing_enabled = false;
    }
}

/**
 * Builds the VMREAD and VMWRITE bitmaps, once, and records whether the
 * processor offers the control at all.
 *
 * Called from every processor's VMCS setup and does its work on the
 * first: the contents do not depend on the processor, and the alternative
 * - a separate boot-processor-only initialisation step - is one more
 * ordering constraint for no gain.
 */
void hypervisor::initialize_vmcs_shadowing()
{
    if constexpr (!nested_vmx::enabled ||
                  !nested_vmx::shadow_vmcs_enabled) {
        // Left false, so set_vmcs_shadowing and both copies are no-ops
        // and the guest hypervisor exits for every field as it always
        // did. Said out loud, because a run that was meant to have this
        // on and did not must not be read as the feature failing.
        this->vmcs_shadowing_enabled = false;
        log("vmcs shadowing switched off in this build");
        return;
    } else {
        if (0 != this->vmcs_shadow_read_bitmap_physical) {
            return;
        }

        // The allowed-1 settings live in the high half of the capability
        // MSR. A processor that does not offer the control gets the
        // bitmaps written and the control never requested, which costs two
        // VMCS fields and changes nothing else.
        constexpr auto shadowing = arch::x86_64::vmx::
            vm_execution_controls::secondary::vmcs_shadowing;
        this->vmcs_shadowing_enabled =
            0 !=
            ((this->cached_vmx_msr(
                  arch::x86_64::vmx::msr::processor_based_contorls_2) >>
              32) &
             shadowing);

        // All ones is "exit for everything", which is what this VMM did
        // before there were bitmaps at all - so a field left out of the
        // lists below behaves exactly as it used to.
        for (auto & byte : this->vmcs_shadow_read_bitmap) {
            byte = 0xff;
        }
        for (auto & byte : this->vmcs_shadow_write_bitmap) {
            byte = 0xff;
        }

        for (auto entry : shadow_read_only_fields) {
            permit_field(this->vmcs_shadow_read_bitmap,
                         static_cast<std::uint64_t>(entry));
        }

        for (auto entry : shadow_read_write_fields) {
            permit_field(this->vmcs_shadow_read_bitmap,
                         static_cast<std::uint64_t>(entry));
            permit_field(this->vmcs_shadow_write_bitmap,
                         static_cast<std::uint64_t>(entry));
        }

        this->vmcs_shadow_read_bitmap_physical =
            this->host_page_table.virtual_to_physical(
                this->vmcs_shadow_read_bitmap);
        this->vmcs_shadow_write_bitmap_physical =
            this->host_page_table.virtual_to_physical(
                this->vmcs_shadow_write_bitmap);

        log("vmcs shadowing {}",
            this->vmcs_shadowing_enabled ? "available" : "not offered");
    }
}

/**
 * Turns the control on or off for this processor, together with the link
 * pointer it requires.
 *
 * The two move together and must: SDM 27.3.1.5 makes a VM entry fail if
 * the control is set and the link pointer does not name a valid shadow
 * region. So "no VMCS of the guest hypervisor's is current" is expressed
 * by clearing both, which is also what KVM's vmx_disable_shadow_vmcs
 * does.
 */
void hypervisor::set_vmcs_shadowing(std::size_t cpu, bool enabled)
{
    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        constexpr auto shadowing = arch::x86_64::vmx::
            vm_execution_controls::secondary::vmcs_shadowing;

        auto controls =
            this->vmcs.secondary_processor_based_vm_execution_controls();

        // No adjust_msr on either write below: the capability checked
        // above is the same test, made once. `vmcs_shadowing_enabled` is
        // assigned from the allowed-1 half of IA32_VMX_PROCBASED_CTLS2
        // and this function returns early when it is false.
        if (enabled) {
            this->vmcs.vmcs_link_pointer(this->shadow_vmcs_physical[cpu]);
            this->vmcs.secondary_processor_based_vm_execution_controls(
                controls | shadowing);
            copy_vmcs12_to_shadow(cpu);
        } else {
            // Same capability checked above, and SDM A.3.3 reserves the
            // allowed-0 half of the secondary controls to zero, so
            // clearing a bit there can violate neither half.
            this->vmcs.secondary_processor_based_vm_execution_controls(
                controls & ~static_cast<std::uint64_t>(shadowing));
            this->vmcs.vmcs_link_pointer(~std::uint64_t{});
        }
    }
}

/**
 * Publishes this VMM's cached vmcs12 into the shadow region, so the guest
 * hypervisor's next VMREAD of a shadowed field finds what this VMM would
 * have answered.
 *
 * Loading the region makes it current, which is the only way its fields
 * can be written - the format is not architecturally defined and must not
 * be written as memory. It is cleared again afterwards so its contents
 * reach memory rather than staying in whatever the processor caches, and
 * the VMCS that was current is put back. VMPTRST rather than a remembered
 * pointer because this is called from both the vmcs01 and the reflection
 * paths, and a wrong restore here would be a VM entry against the wrong
 * VMCS.
 */
void hypervisor::copy_vmcs12_to_shadow(std::size_t cpu)
{
    // Phase timing; see `phase_cycles`.
    auto copy_start = arch::x86_64::rdtsc();
    auto copy_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][4] +=
                arch::x86_64::rdtsc() - copy_start;
            this->phase_calls[cpu][4] += 1;
        }
    });

    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        // Adjacent intervals over the five things this does, because
        // four of them are region instructions and the fifth is the only
        // one anybody has ever thought about.
        //
        // `shadow_writes_skipped` says the field writes are mostly
        // elided already, so a call that still costs tens of thousands
        // of cycles is not costing them on fields - it is costing them
        // on the VMPTRST, the two VMPTRLDs and the VMCLEAR that bracket
        // them. Those four are **not** in `vmcs_reads_taken` or
        // `vmcs_writes_taken`, which count only `vmcs::read` and
        // `vmcs::write` - so the "110.6 VMCS accesses a round trip" this
        // project prices its estimates from has never included the most
        // expensive instructions on the path.
        //
        // Which of the four is the answer: KVM's `handle_vmptrst` writes
        // through a guest *linear* address and its `handle_vmclear`
        // releases the mapped page and writes the launch state back, so
        // neither is the cheap pointer move its name suggests. If they
        // are, the VMPTRST is removable outright - this VMM knows which
        // VMCS is current from `running_l2` and
        // `own_vmcs_region_physical`, and the comment above only says it
        // does not.
        auto mark = copy_start;
        auto stamp = [&](std::size_t slot) {
            if (cpu < max_cpus) {
                auto now = arch::x86_64::rdtsc();
                this->phase_cycles[cpu][slot] += now - mark;
                this->phase_calls[cpu][slot] += 1;
                mark = now;
            }
        };

        // The VMCS pointer is borrowed from here to the `vmptrld`
        // back at the end, and only the shadow region is touched in
        // between - so the fields cached for the VMCS that is current
        // now are still good when it becomes current again. Holding the
        // cache still across the borrow, and handing the row back on the
        // way out, is what stops these two functions wiping it six times
        // a round trip. See `vmcs_cache_suspended`.
        arch::x86_64::vmx::vmcs_cache_borrow borrow;

        std::uint64_t previous{};
        if (arch::x86_64::vmx::vmptrst(&previous)) {
            return;
        }

        stamp(40);

        if (arch::x86_64::vmx::vmptrld(&this->shadow_vmcs_physical[cpu])) {
            return;
        }

        stamp(41);

        auto & cached = this->guest_vmcs12[cpu];

        // Only what the region does not already hold. See
        // `shadow_cache`: each of these writes traps to the layer below,
        // and most of them write a value that is already there.
        std::size_t index{};
        auto put = [&](auto entry) {
            auto value = cached.read(
                vmcs_field_encoding(static_cast<std::uint64_t>(entry)));

            if (this->shadow_cache_valid[cpu] &&
                (index < shadow_cache_capacity) &&
                (this->shadow_cache[cpu][index] == value)) {
                this->shadow_writes_skipped[cpu] =
                    this->shadow_writes_skipped[cpu] + 1;
                ++index;
                return;
            }

            this->vmcs.write(entry, value);
            this->shadow_writes_done[cpu] =
                this->shadow_writes_done[cpu] + 1;
            if (index < shadow_cache_capacity) {
                this->shadow_cache[cpu][index] = value;
            }
            ++index;
        };

        for (auto entry : shadow_read_only_fields) {
            put(entry);
        }
        for (auto entry : shadow_read_write_fields) {
            put(entry);
        }
        this->shadow_cache_valid[cpu] = true;

        stamp(42);

        arch::x86_64::vmx::vmclear(&this->shadow_vmcs_physical[cpu]);

        stamp(43);

        arch::x86_64::vmx::vmptrld(&previous);

        stamp(44);

        this->vmcs_shadow_stores[cpu] = this->vmcs_shadow_stores[cpu] + 1;
    }
}

/**
 * Collects the writable fields back out of the shadow region, because the
 * guest hypervisor may have written any of them without this VMM seeing
 * it - which is the entire point of the feature.
 *
 * Must run before anything reads the cached vmcs12 after the guest
 * hypervisor has had a chance to run, and before any copy in the other
 * direction, which would otherwise discard those writes.
 */
void hypervisor::copy_shadow_to_vmcs12(std::size_t cpu)
{
    // Phase timing; see `phase_cycles`.
    auto copy_start = arch::x86_64::rdtsc();
    auto copy_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][5] +=
                arch::x86_64::rdtsc() - copy_start;
            this->phase_calls[cpu][5] += 1;
        }
    });

    if constexpr (!nested_vmx::enabled) {
        return;
    } else {
        if (!this->vmcs_shadowing_enabled) {
            return;
        }

        // The same five adjacent intervals as the copy out; see it for
        // why the four region instructions are the interesting part and
        // the nine field reads are not.
        auto mark = copy_start;
        auto stamp = [&](std::size_t slot) {
            if (cpu < max_cpus) {
                auto now = arch::x86_64::rdtsc();
                this->phase_cycles[cpu][slot] += now - mark;
                this->phase_calls[cpu][slot] += 1;
                mark = now;
            }
        };

        // The VMCS pointer is borrowed from here to the `vmptrld`
        // back at the end, and only the shadow region is touched in
        // between - so the fields cached for the VMCS that is current
        // now are still good when it becomes current again. Holding the
        // cache still across the borrow, and handing the row back on the
        // way out, is what stops these two functions wiping it six times
        // a round trip. See `vmcs_cache_suspended`.
        arch::x86_64::vmx::vmcs_cache_borrow borrow;

        std::uint64_t previous{};
        if (arch::x86_64::vmx::vmptrst(&previous)) {
            return;
        }

        stamp(45);

        if (arch::x86_64::vmx::vmptrld(&this->shadow_vmcs_physical[cpu])) {
            return;
        }

        stamp(46);

        auto & cached = this->guest_vmcs12[cpu];

        // The read-write fields sit after the read-only ones in the
        // cache, because that is the order the copy out writes them.
        // Updated with what was *found*, not what was last intended -
        // the guest hypervisor writes these without exiting, so what it
        // left behind is the region's truth.
        auto index = sizeof(shadow_read_only_fields) /
                     sizeof(shadow_read_only_fields[0]);

        for (auto entry : shadow_read_write_fields) {
            auto value = this->vmcs.read(entry);
            cached.write(
                vmcs_field_encoding(static_cast<std::uint64_t>(entry)),
                value);
            if (index < shadow_cache_capacity) {
                this->shadow_cache[cpu][index] = value;
            }
            ++index;
        }

        stamp(47);

        arch::x86_64::vmx::vmclear(&this->shadow_vmcs_physical[cpu]);

        stamp(48);

        arch::x86_64::vmx::vmptrld(&previous);

        stamp(49);

        this->vmcs_shadow_loads[cpu] = this->vmcs_shadow_loads[cpu] + 1;
    }
}
} // namespace zpp::hypervisor
