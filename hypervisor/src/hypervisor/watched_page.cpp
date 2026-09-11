// The watched-page write path.
//
// What this VMM does when a guest writes a page it has taken write
// permission away from: arm and disarm the watch, take the resulting EPT
// violation, decode the instruction that caused it, carry the store out
// against a page the guest cannot write, and retire the instruction
// through the monitor trap flag when it could not be carried out.
//
// Why these seven and not some other cut. They are the whole of that path
// and nothing else in the VMM calls into the middle of it - checked by
// grep across every other translation unit, which finds callers only of
// the two ends, `watch_guest_page_writes`/`unwatch_guest_page` and the two
// exit handlers. `read_guest_word` and `apply_guest_store` come with them
// because emulating the store is their only user.
//
// Not folded into guest_memory.cpp beside it, which was tried first. That
// file is about copying guest physical memory through the mapping window -
// a different subject, with its own anonymous-namespace helpers and its
// own lock discipline - and putting the two together would mean any
// harness compiling this path also had to satisfy `read_guest_physical`,
// `write_guest_physical` and `guest_linear_to_physical`, none of which it
// touches.
//
// Why move anything at all: hypervisor.cpp is ten thousand lines and
// reaches the whole VMM, so nothing can compile it, and tests/watched_page
// therefore cut these bodies out with awk at build time. A translation
// unit a harness can compile against a small stand-in header is what makes
// that unnecessary. Same reason as b6f0bee.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <cstdint>
#include <optional>
#include <utility>

namespace zpp::hypervisor
{
std::optional<std::uint64_t> hypervisor::read_guest_word(
    std::uint64_t guest_physical, std::uint8_t size)
{
    // Straight through the host page table, for the same reason the write
    // below goes that way: the extended page tables establish an identity
    // between guest physical and host physical, and our own mapping of the
    // page is the only one that is not protected.
    auto * at =
        reinterpret_cast<const volatile std::uint8_t *>(guest_physical);

    if (!this->host_page_table.virtual_to_physical(
            reinterpret_cast<const void *>(guest_physical))) {
        return {};
    }

    switch (size) {
    case 1:
        return arch::x86_64::read8(at);
    case 2:
        return arch::x86_64::read16(at);
    case 4:
        return arch::x86_64::read32(at);
    case 8:
        return arch::x86_64::read64(at);
    default:
        return {};
    }
}

bool hypervisor::carry_out_guest_instruction(
    std::uint64_t guest_physical,
    const arch::x86_64::decoded_instruction & instruction,
    arch::x86_64::context & context,
    guest_write & performed,
    bool & changed_memory,
    std::optional<std::uint64_t> known_contents)
{
    using arch::x86_64::memory_operation;

    // A store replaces the contents outright, so the old value is not
    // needed - and must not be demanded, since a device register that
    // reads differently from what was written is exactly the case this
    // exists to observe.
    auto needs_old = (memory_operation::store != instruction.what);

    std::uint64_t old{};
    if (needs_old) {
        // The caller may already have read it, to show the filter what
        // this instruction was going to leave behind. Reading again is
        // not free and not even idempotent: these are device registers,
        // and on the local APIC page a second read is a second access to
        // hardware whose answer may differ from the first - so the two
        // halves of one instruction would combine against different
        // contents. Threaded through rather than re-read.
        if (known_contents) {
            old = *known_contents;
        } else {
            auto read = read_guest_word(guest_physical, instruction.size);
            if (!read) {
                return false;
            }

            old = *read;
        }
    }

    auto replacement = arch::x86_64::apply(instruction, old);

    // Written back only where the instruction actually changes memory. An
    // examine that wrote its own value back would turn a read of a device
    // register into a write of it, which on a register with side effects
    // is a different instruction from the one the guest executed.
    auto writes_memory = (memory_operation::load != instruction.what) &&
                         (memory_operation::examine != instruction.what);

    if (writes_memory) {
        auto stored = arch::x86_64::memory_store{
            .value = replacement,
            .size = instruction.size,
        };

        if (!apply_guest_store(guest_physical, stored)) {
            return false;
        }
    }

    if (instruction.writes_register) {
        auto & slot =
            context.*arch::x86_64::register_of(instruction.destination);
        slot = arch::x86_64::result_for_register(instruction, old, slot);
    }

    // The flags, which are not optional.
    //
    // Every operation here except the moves and the exchange sets them,
    // and the guest branches on them immediately - `and [mem], eax` is
    // followed by a `jz`. Carrying out the arithmetic and leaving RFLAGS
    // alone makes the guest take the other branch, and that is not a
    // subtle corruption: it was measured as a triple fault, exit reason 2,
    // after 179 emulated instructions. The decoder this replaces never
    // needed it because MOV sets no flags.
    if (arch::x86_64::memory_operation::store != instruction.what) {
        this->vmcs.guest_rflags(arch::x86_64::flags_after(
            instruction, this->vmcs.guest_rflags(), old, replacement));
    }

    // What the watch is told. The value reported is what the memory now
    // holds, which for a combine is the combination rather than the
    // operand - a handler looking for "did the guest clear the enable
    // bit" wants the result, not the mask.
    performed = guest_write{
        .address = guest_physical,
        .value = writes_memory ? replacement : old,
        .size = instruction.size,
    };

    changed_memory = writes_memory;

    // Recorded newest-last so the few before a failure can be read out.
    // Everything the emulation decided is here: what it thought the
    // instruction was, the width, what memory held and what it now holds.
    //
    // Only the operations the narrow decoder could not answer are kept.
    // A plain store is not new - it was emulated before this decoder
    // existed and on a build that booted - so recording those fills the
    // ring with traffic that is known good and hides the ones that are
    // not. Freezes when full, because the first of a new kind is what
    // matters.
    if ((memory_operation::store != instruction.what) &&
        (this->emulated_trace_count < emulated_trace_capacity)) {
        auto slot = this->emulated_trace_count;
        auto & recorded = this->emulated_trace[slot];

        for (std::size_t i{}; i < sizeof(recorded.code); ++i) {
            recorded.code[i] = this->last_fetched_code[i];
        }

        recorded.page = guest_physical >> 12;
        recorded.old_value = old;
        recorded.new_value = replacement;
        recorded.what = static_cast<std::uint32_t>(instruction.what);
        recorded.how = static_cast<std::uint32_t>(instruction.how);
        recorded.size = instruction.size;
        recorded.wrote = writes_memory ? 1u : 0u;

        this->emulated_trace_count = this->emulated_trace_count + 1;
    }

    return true;
}

bool hypervisor::apply_guest_store(
    std::uint64_t guest_physical, const arch::x86_64::memory_store & store)
{
    // Straight through the host page table, which is the only mapping of
    // this page that is writable - the guest's own is not, which is the
    // whole point. The identity between guest and host physical that the
    // EPT establishes is what makes the address usable here directly.
    auto * at = static_cast<volatile std::uint8_t *>(
        reinterpret_cast<void *>(guest_physical));

    if (!this->host_page_table.virtual_to_physical(
            reinterpret_cast<const void *>(guest_physical))) {
        return false;
    }

    switch (store.size) {
    case 1:
        arch::x86_64::write8(at, static_cast<std::uint8_t>(store.value));
        return true;
    case 2:
        arch::x86_64::write16(at, static_cast<std::uint16_t>(store.value));
        return true;
    case 4:
        arch::x86_64::write32(at, static_cast<std::uint32_t>(store.value));
        return true;
    case 8:
        arch::x86_64::write64(at, store.value);
        return true;
    default:
        return false;
    }
}

bool hypervisor::on_ept_violation(std::size_t cpu,
                                  arch::x86_64::context & context,
                                  std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    // The synthetic nested VT-d unit. Its register page is full-trapped so
    // hvix64 sees a synthetic CAP/ECAP, and every access is emulated here
    // rather than reaching QEMU's real unit. Handled before the local-APIC
    // adoption logic below, which is about a different page. See
    // `nested_vmx::nested_vtd`.
    if constexpr (nested_vmx::nested_vtd) {
        if ((0 != this->dmar_register_page) &&
            (page == this->dmar_register_page)) {
            return dmar_mmio(cpu, context, guest_physical);
        }
    }

    // Every violation an application processor takes, with the
    // qualification decoded. It takes four on the local APIC page and
    // dies, and its emulated and stepped counts are both zero - so
    // neither the decode path nor the monitor-trap fallback ran, and a
    // third branch is handling them. SDM Table 28-7: bit 0 read, bit 1
    // write, bit 2 instruction fetch, bit 3 readable, bit 4 writable,
    // bit 5 executable. The exit ring's qualification is not filled
    // (census=0), so it cannot answer this.
    if ((0 != cpu) && (cpu < max_cpus)) {
        // With the *host's* own IA32_APIC_BASE, read in root mode on
        // this processor. `apply_guest_store` writes 0xfee00xxx as a
        // host virtual address, which is the host's xAPIC MMIO window -
        // and bit 10 set means x2APIC, where that window is
        // architecturally unavailable and the store silently does
        // nothing. A native write by the guest goes through EPT
        // instead, so this is one of the few things that can differ
        // between emulating these writes and not.
        constexpr std::uint32_t ia32_apic_base = 0x1b;
        log("cpu {} ept violation on page {}, qualification {}, "
            "watched apic page {}, host apic base {}",
            cpu,
            page,
            this->vmcs.exit_qualification(),
            this->watched_apic_page,
            arch::x86_64::rdmsr(ia32_apic_base));
    }

    // Carried out here because this is the one place it is safe: the
    // decision is taken in `filter_local_apic_write`, which runs from
    // *inside* the loop below and cannot drop a watch without clearing the
    // entry that loop holds a reference to. Acting on it at the top of the
    // next fault costs one more fault and no reasoning about iterator
    // lifetime. See `nested_vmx::disarm_apic_watch`.
    //
    // **And the fault it lands on is that one more fault, so it must be
    // returned from rather than fallen out of.** The disarm clears the
    // only watch on the local APIC page, so the loop below then finds
    // nothing for the page that faulted, `on_ept_violation` answers false,
    // and both callers read false as "the protection was put there by
    // something that is not going to handle the fault" and stop the
    // processor. That is what `ZPP_DISARM_APIC_WATCH=ON` measured three
    // times: bring-up completed, and the first EPT violation after the
    // quiet period killed the guest with `unhandled_exit` reason 0x30,
    // qualification 0x2b, at local APIC offset 0x380 - the timer's initial
    // count, which an idle Hyper-V writes every tick. `BACKLOG.md` records
    // the run and the wrong diagnosis it was first given.
    //
    // Resuming without advancing RIP is the whole answer, and it is the
    // same answer the module-decoy branch at the bottom of this function
    // gives: the guest re-executes its own instruction against an entry
    // that now permits it. It cannot loop, because the disarm happens once
    // - `watched_apic_page` is zero afterwards, so this block is not
    // reached again.
    // **The proof, which runs whatever the switches say.**
    //
    // `all_processors_started` used to have two ways to become true and
    // both were guesses - a two-minute silence, and "somebody was given a
    // start-up vector". This is the third and it is a proof: every
    // identifier on the firmware's own roster has a slot that is running
    // under this hypervisor, so there is no processor left for a start-up
    // IPI to hand over unvirtualized. See
    // `every_platform_processor_adopted`.
    //
    // **"Which is the only thing the watch exists to prevent" used to
    // close that sentence, and it is false.** Grep says the watch has
    // three consumers, not one, and the retirement silences all three:
    //
    // - `interrupt_command_intercepted` (`local_apic.cpp:109`) is
    //   `watched_apic_page != 0 || interrupt_command_bitmap_armed`, and
    //   it is the second conjunct of `waited` in `emulate_init_signal`
    //   (`start_up.cpp:1333`). On an xAPIC machine the bitmap bit is
    //   never armed, so retiring the watch forces **every INIT after it**
    //   onto the hardware hand-off - the one the layer below discards
    //   while this VMM is in root mode, which is the whole reason the
    //   software hand-off exists.
    // - `discard_start_up_for_init` (`interrupt_command.cpp:103`) only
    //   runs from `on_interrupt_command`, so the mailbox stops being
    //   drained by an INIT once nothing sees the INIT.
    // - `start_up_processor` stops being consulted at all, so nothing
    //   records what vector the guest asked for.
    //
    // Measured, 2026-08-27, and the third of those cost a session: after
    // the retirement the ring carries `cpu 2 start-up ipi exit, vector
    // 0x2` with **no `guest ipi command` line anywhere near it**, and the
    // vector was then attributed to this VMM's own trampoline - which is
    // at page `0x9c` on that rig, logged eighteen records earlier. See
    // BACKLOG.md, "RETRACTED: vector 0x2 is not ours".
    //
    // This is a correction to the comment and not to the behaviour:
    // whether the retirement is right is a separate question, and it is
    // not settled - the measured cost of leaving the watch armed is real,
    // and nothing yet shows the retirement losing a start-up IPI. What is
    // settled is that the sentence above named one consumer out of three.
    //
    // Tested here rather than at the moment a processor marks itself
    // virtualized, because here is where it is *worth* anything: this is
    // the fault the watch charges for, so the proof is evaluated exactly
    // when the cost is being paid, and only while the watch is armed. It
    // is a scan of at most `max_cpus` per roster entry against two arrays
    // that are already in cache on this path.
    if ((0 != this->watched_apic_page) &&
        !this->all_processors_started.load(std::memory_order_relaxed) &&
        every_platform_processor_adopted()) {
        this->all_processors_started.store(true,
                                           std::memory_order_relaxed);
        log("every processor on the platform roster of {} is adopted, so "
            "the local apic page watch can no longer catch a start-up ipi "
            "for a processor this vmm does not own",
            this->number_of_platform_processors);
    }

    // Carried out on any of the three, and the return is what makes it
    // safe - see the paragraph above about `unhandled_exit` reason 0x30.
    if constexpr (!nested_vmx::keep_apic_watch) {
        if (this->all_processors_started.load(std::memory_order_relaxed) &&
            (0 != this->watched_apic_page)) {
            watch_local_apic(false);
            return true;
        }
    }

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }

        // Last chance to use the controller the guest is about to take
        // away. See page_watch::before_write.
        if (watch.before_write) {
            watch.before_write(watch.context, page);
        }

        // A held page stops the writer here, at the faulting
        // instruction, until whoever is holding it lets go.
        //
        // This is the whole of the exclusion. The write has not taken
        // effect when the violation is delivered, so the page is
        // unchanged for as long as the spin lasts, and a borrower can
        // work on a structure the guest owns without the guest being
        // able to touch it. No cooperation is required and no processor
        // that is not writing this page is delayed at all.
        //
        // Spinning inside the exit is deliberate. The alternative -
        // resuming and re-faulting - burns exits for the same wait and
        // gives the guest a window between the resume and the next
        // fault, which is the window this exists to close.
        while (watch.held.load(std::memory_order_acquire)) {
            zpp::spin_hint();
        }

        // Emulate the write where the instruction can be decoded, and
        // only step over it where it cannot.
        //
        // Stepping over means opening the page, letting one instruction
        // retire and closing it again, and for that window the page is
        // writable *for every processor*. That is not a theoretical
        // hole: a driver writes CC twice in succession, disable then
        // enable, and the second write goes through the window the first
        // opened. Measured - one trapped write of 0x00460000, a
        // controller afterwards reading 0x00460001, and no transition
        // seen. Losing that transition is losing the channel, because a
        // reset is the one event the sink has to notice.
        //
        // Emulating closes the window by never opening it. The page
        // stays unwritable, every write faults, and each is applied by
        // this VMM with the value it decoded - so a second write cannot
        // slip past a first, and the handler is told what was written
        // rather than having to read the register back and race the
        // guest for it.
        // Only a violation caused by the instruction's own operand may
        // be emulated. Bit 8 of the exit qualification clear means the
        // access was to a paging-structure entry - the processor walking
        // the guest's tables - and there is no store in the instruction
        // to carry out for that. SDM Table 28-7.
        constexpr std::uint64_t qualification_linear_address_valid = 1ull
                                                                     << 7;
        constexpr std::uint64_t qualification_operand_access = 1ull << 8;

        auto qualification = this->vmcs.exit_qualification();
        auto reports_linear =
            (0 != (qualification & qualification_linear_address_valid));

        // Decoded up front, because both paths below want it: emulating
        // needs the operation, and *addressing* needs it whenever the
        // exit did not report a linear address.
        auto instruction = decode_guest_instruction(cpu, context);

        // `context.rip` and not a VMREAD of `guest_rip`. `on_vm_exit`
        // reads the field once at the top of every exit and puts it
        // there, and nothing between that point and here writes it - so
        // this asked the processor a question the frame above already
        // answered, at 1.4-1.8 microseconds a time on a host with no
        // VMCS shadowing. It ran on every extended-page-table violation
        // against a watched page, which on this rig is 1,586 a second.
        auto decoded_address =
            instruction ? arch::x86_64::effective_address(
                              *instruction, context, context.rip)
                        : std::nullopt;

        // Where in the page the access landed, from three sources,
        // strongest first.
        //
        // **The guest-physical address is not one of them, despite being
        // the obvious one.** The processor reports it at *page*
        // granularity for an EPT violation, so its low twelve bits are
        // not the offset the instruction named. Taking them anyway made
        // every access look like an access to offset 0 - a reserved local
        // APIC register - and the interrupt-command handler, which acts
        // only on a write to the command register, could never be reached
        // no matter what the guest wrote.
        //
        // Measured, which is the only reason it was found: twenty-four
        // consecutive accesses recorded as page 0xfee00 offset 0, while
        // the exit ring showed the writes arriving and the handler showed
        // not one call. A guest hypervisor was running with its
        // processors never starting, waiting for start-up interrupts this
        // VMM had received and thrown the register number away from.
        //
        // The guest-linear address the same exit reports would answer it,
        // and does whenever qualification bit 7 is set. It is not always:
        // on the machine where the guest hypervisor runs, *every one* of
        // those violations arrived with bit 7 clear. So the instruction's
        // own addressing bytes are the third source and the only one that
        // does not depend on what the hardware volunteered.
        constexpr std::uint64_t page_offset_mask = page_size - 1;

        auto page_base = guest_physical & ~page_offset_mask;
        auto address = guest_physical;
        auto address_known = reports_linear;

        // Whether the processor - or, on the rig, KVM - answers this
        // question itself. Counted before anything is decided, so the
        // three counters describe every violation rather than the subset
        // that took some branch. See the members for why this is in
        // doubt.
        if (auto physical_offset = guest_physical & page_offset_mask;
            0 != physical_offset) {
            this->physical_offset_present =
                this->physical_offset_present + 1;

            if (reports_linear) {
                if (physical_offset == (this->vmcs.guest_linear_address() &
                                        page_offset_mask)) {
                    this->physical_offset_agreed =
                        this->physical_offset_agreed + 1;
                } else {
                    this->physical_offset_disagreed =
                        this->physical_offset_disagreed + 1;
                }
            }
        }

        if (reports_linear) {
            address = page_base | (this->vmcs.guest_linear_address() &
                                   page_offset_mask);
        } else if (decoded_address) {
            address = page_base | (*decoded_address & page_offset_mask);
            address_known = true;

            this->access_offset_decoded = this->access_offset_decoded + 1;
        } else {
            this->access_offset_unknown = this->access_offset_unknown + 1;
        }

        // The offset this write is about to be applied at, beside the
        // one the exit itself reported. With bit 7 of the qualification
        // clear there is no linear address, so the offset comes from
        // decoding the instruction - while the guest physical address
        // carries the true one. If these disagree the right value is
        // being written to the wrong local APIC register, which is
        // precisely the difference between emulating these writes and
        // letting them go native.
        if ((0 != cpu) && (cpu < max_cpus)) {
            log("cpu {} apic write offset: applied {} physical {} "
                "decoded {} known {}",
                cpu,
                address & page_offset_mask,
                guest_physical & page_offset_mask,
                decoded_address ? (*decoded_address & page_offset_mask)
                                : 0xffffull,
                static_cast<std::uint64_t>(address_known));
        }

        // Whether the fault was the instruction's own operand rather than
        // the processor walking the guest's paging structures.
        //
        // Bit 8 answers it outright, but only when bit 7 says the exit
        // has a linear address at all. Without it the question is settled
        // instead by *which page* faulted: these are watched pages, and a
        // watched page is a device's registers - the local APIC, a
        // controller's configuration space. No guest puts its paging
        // structures in uncacheable device memory, so an access to one of
        // them is never a table walk.
        auto operand_access =
            reports_linear
                ? (0 != (qualification & qualification_operand_access))
                : address_known;

        // See `nested_vmx::step_ap_watched_writes`: with it on, every
        // processor but the first falls through to the monitor-trap
        // step below and executes its own instruction.
        auto may_emulate =
            emulate_watched_page_writes &&
            (!nested_vmx::step_ap_watched_writes || (0 == cpu));

        if (auto store = (may_emulate && operand_access)
                             ? instruction
                             : std::nullopt) {
            // A store that crosses the end of the watched page would be
            // applied whole at the faulting address, writing bytes onto
            // the page that follows. Refused rather than split: nothing
            // a driver does to a register straddles the page, so the
            // fallback costs nothing and a wrong split would be silent.
            auto offset_in_page = address & page_offset_mask;
            auto straddles = (offset_in_page + store->size) > page_size;

            // The watch gets to refuse the write before it happens.
            //
            // This is where a start-up IPI is caught. Writing the local
            // APIC's command register *is* the send, so a VMM that
            // redirects one has to decide here - after the write there is
            // nothing left to redirect. See page_watch::filter.
            //
            // Every form that writes memory is consulted, not only a
            // plain store. Restricting it to stores was the other half of
            // the bug fixed alongside this: a read-modify-write reached
            // the notify instead, *after* the send, and the notify's
            // handler adopts a start-up IPI by starting the processor
            // itself - so the hardware had the guest's command and this
            // VMM sent a second one. Two start-up IPIs per command is not
            // subtle; the same shape was measured as a triple fault, exit
            // reason 2, after 179 emulated writes to this page.
            //
            // The value a non-store form will leave behind is not in the
            // instruction, so it is computed the way
            // carry_out_guest_instruction computes it - read what is
            // there, apply the operation. A register that reads
            // differently from what was written makes this the wrong
            // question for a *store*, which is why a store still uses its
            // operand and never reads first.
            auto filter_consulted = false;
            auto refused_rewrite = false;

            auto writes_memory =
                (arch::x86_64::memory_operation::load != store->what) &&
                (arch::x86_64::memory_operation::examine != store->what);

            std::optional<std::uint64_t> known_contents;
            std::optional<std::uint64_t> intended_value;
            if (arch::x86_64::memory_operation::store == store->what) {
                intended_value = store->operand;
            } else if (writes_memory) {
                known_contents = read_guest_word(address, store->size);
                if (known_contents) {
                    intended_value =
                        arch::x86_64::apply(*store, *known_contents);
                }
            }

            if (watch.filter_write && !straddles && intended_value) {
                filter_consulted = true;
                guest_write intended{
                    .address = address,
                    .value = *intended_value,
                    .size = store->size,
                };

                auto allowed =
                    watch.filter_write(watch.context, page, &intended);

                if (!allowed) {
                    // Refused. The guest still retires the instruction -
                    // it must, or it re-executes and faults for ever -
                    // but memory and the device are left alone.
                    this->emulated_writes = this->emulated_writes + 1;
                    if (cpu < max_cpus) {
                        this->emulated_writes_by_cpu[cpu] =
                            this->emulated_writes_by_cpu[cpu] + 1;
                    }
                    this->filtered_writes = this->filtered_writes + 1;

                    // Named on any processor but the first. A refused
                    // write is one this VMM swallows: the guest's
                    // store never happens and it is resumed as though
                    // it had. That is right for a start-up IPI being
                    // redirected and wrong for anything else, and the
                    // per-processor emulated count includes refusals,
                    // so a swallowed write is invisible in it.
                    if ((0 != cpu) && (cpu < max_cpus)) {
                        log("cpu {} refused apic write: offset {} "
                            "value {} size {} rip {}",
                            cpu,
                            address & page_offset_mask,
                            *intended_value,
                            static_cast<std::uint64_t>(store->size),
                            context.rip);
                    }

                    // SDM 30.2.5 (.references/sdm.txt:204101-204136)
                    // leaves this field undefined for an ordinary EPT
                    // operand fault. A nonzero value can be stale; it
                    // does not prove that the decoder is wrong. KVM's
                    // handle_ept_violation (vmx.c) uses instruction
                    // emulation for MMIO, not this field. Keep the raw
                    // comparison for diagnosis, but retire the decoded
                    // instruction even when its write is filtered out.
                    auto reported =
                        this->vmcs.vm_exit_instruction_length();
                    if ((0 != reported) && (reported != store->length)) {
                        this->emulated_length_disagreement =
                            this->emulated_length_disagreement + 1;
                        this->emulated_length_reported = reported;
                        this->emulated_length_decoded = store->length;
                        log("emulated write length disagreement: the "
                            "undefined VMCS field {} decoded {} at rip "
                            "{} - using decoded length",
                            reported,
                            store->length,
                            context.rip);
                    }

                    auto length = store->length;

                    // Advanced from `context.rip` rather than from a
                    // read back, and **`context.rip` is advanced with
                    // it**. The second half is not tidiness: everything
                    // downstream that reports "where the guest is" -
                    // `record_exit`'s ring, `resume_guest_rip` - reads
                    // the field, so leaving `context.rip` behind here
                    // would make the two disagree by the length of one
                    // instruction and the disagreement would be silent.
                    context.rip = context.rip + length;
                    this->vmcs.guest_rip(context.rip);
                    note_low_emulated_rip(cpu,
                                          context.rip - length,
                                          context.rip,
                                          length);
                    return true;
                }

                // Allowed, possibly with a different value.
                //
                // A plain store carries the value in its operand, so
                // replacing it is the whole of the rewrite. Every other
                // form derives what it writes from what is already there,
                // and there is nowhere to put a value the operation would
                // not have produced - so a filter that rewrites one is
                // refused emulation rather than half honoured, and the
                // access takes the stepping path instead.
                //
                // Unreachable today, and deliberately not papered over:
                // the only filter here is the local APIC's, and
                // on_interrupt_command returns the command it was given
                // on every path, so the rewrite never fires. It is the
                // next filter that would find this out.
                if (*allowed != *intended_value) {
                    // A store and an exchange both leave their operand
                    // in memory - `apply` returns it unchanged for
                    // either - so replacing the operand honours the
                    // rewrite exactly. Every other form derives what it
                    // writes from what is already there and has nowhere
                    // to put a value the operation would not have
                    // produced.
                    if ((arch::x86_64::memory_operation::store ==
                         store->what) ||
                        (arch::x86_64::memory_operation::exchange ==
                         store->what)) {
                        auto replaced = *store;
                        replaced.operand = *allowed;
                        store = replaced;
                    } else {
                        // Refused *emulation*, which is not the same as
                        // refusing the access. This used to `return
                        // false`, and the comment above claiming it took
                        // the stepping path was simply wrong: false is
                        // how on_ept_violation says "nothing had this
                        // page watched", and the caller treats that as a
                        // bug here and stops the processor. Letting the
                        // condition below fail is what actually reaches
                        // the stepping path.
                        refused_rewrite = true;
                    }
                }
            }

            guest_write written{};

            auto changed_memory = false;

            if (!straddles && !refused_rewrite &&
                carry_out_guest_instruction(address,
                                            *store,
                                            context,
                                            written,
                                            changed_memory,
                                            known_contents)) {
                // Only an access that actually changed memory is reported.
                //
                // The decoder now answers loads and examinations as well
                // as writes, which is the point of it - but a handler
                // watching a device register is watching for *writes*, and
                // handing it a read would be a different event wearing the
                // same shape. On the local APIC page that is not a
                // subtlety: the interrupt command register's handler acts
                // on a write to it, so reporting a read of it sends an
                // interrupt the guest never asked for. Measured as a guest
                // that never left early boot.
                // Not when the filter answered it. The filter is the
                // handler for an emulated write - it already saw the
                // value, before the write rather after - and calling
                // both would act on one command twice.
                //
                // Whether the filter *ran*, not whether one exists, and
                // that is the whole of this condition. The filter is
                // consulted only for a plain store, so testing for its
                // existence silently dropped every other form that writes
                // memory: a locked read-modify-write against a watched
                // page changed the page and told nobody.
                //
                // That is not a corner. SDM Table 30-7 says an EPT
                // violation from a read-modify-write sets bit 1 and may
                // also set bit 0, and the boot processor's ring on the
                // rig is full of qualification 0x2b against 0xfee00000 -
                // both bits, so a read-modify-write to the local APIC
                // page. Every one of those went past
                // filter_local_apic_write and on_local_apic_write alike,
                // so on_interrupt_command never saw it: no start-up IPI
                // adoption, no broadcast resolution against the roster,
                // and no log line. The write itself still reached the
                // register, which is why this was invisible.
                //
                // KVM does not key APIC emulation on the instruction form
                // at all - apic_mmio_write (lapic.c) gates on width and
                // alignment only, and x86.c:8068 degrades a locked
                // exchange against the APIC page to a plain write so it
                // lands in the same dispatcher.
                if (changed_memory && watch.on_write &&
                    !filter_consulted) {
                    watch.on_write(watch.context, page, &written);
                }

                this->emulated_writes = this->emulated_writes + 1;
                if (cpu < max_cpus) {
                    this->emulated_writes_by_cpu[cpu] =
                        this->emulated_writes_by_cpu[cpu] + 1;
                }

                // Retire the instruction that was actually decoded and
                // emulated. SDM 30.2.5 leaves VM-exit instruction length
                // undefined here, even when it is nonzero. KVM's
                // handle_ept_violation likewise routes MMIO through the
                // instruction emulator. A stale field must neither
                // skip following instructions nor replay this write.
                auto reported = this->vmcs.vm_exit_instruction_length();
                if ((0 != reported) && (reported != store->length)) {
                    this->emulated_length_disagreement =
                        this->emulated_length_disagreement + 1;
                    this->emulated_length_reported = reported;
                    this->emulated_length_decoded = store->length;

                    // A raw comparison, not proof of a bad decode.
                    if ((0 != cpu) && (cpu < max_cpus)) {
                        log("cpu {} emulated length disagreement at "
                            "offset {}: reported {} decoded {} rip {}",
                            cpu,
                            address & page_offset_mask,
                            reported,
                            store->length,
                            context.rip);
                    }
                }

                auto advance = store->length;

                context.rip = context.rip + advance;
                this->vmcs.guest_rip(context.rip);
                note_low_emulated_rip(
                    cpu, context.rip - advance, context.rip, advance);
                return true;
            }
        }

        // Not a form the decoder handles, so fall back to letting the
        // guest's own instruction do the write. This keeps the window
        // described above, and is why the decoder refusing is a
        // correctness question rather than only a performance one.
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(true);
            invalidate_ept();
        }

        this->stepped_writes = this->stepped_writes + 1;
        if (cpu < max_cpus) {
            this->stepped_writes_by_cpu[cpu] =
                this->stepped_writes_by_cpu[cpu] + 1;
        }

        this->stepping_watch[cpu] = true;
        this->stepping_page[cpu] = page;

        // The address resolved above, whichever of the three sources
        // answered it. Reaching here means the instruction could not be
        // carried out, not that the address is unknown.
        this->stepping_offset[cpu] = address & page_offset_mask;

        // Where the guest is now, so the trap exit can tell whether the
        // instruction retired. See stepping_rip.
        this->stepping_rip[cpu] = context.rip;
        monitor_trap_flag(true);
        return true;
    }

    // Not a watched page. It may still be one of ours: protect_module
    // makes every page of this module not-present to the guest, so any
    // access to them arrives here.
    //
    // Stopping the processor for that would be a poor trade. A guest is
    // entitled to walk physical memory - a memory manager building its
    // own map does exactly that - and killing it for reading an address
    // it has no idea is special turns a curiosity into a bug check.
    //
    // So the page is redirected, permanently, to a page of zeroes. The
    // guest is resumed **without** advancing past its own instruction,
    // which then re-executes and completes against the decoy. That needs
    // no decoder, cannot disagree with what the instruction meant, and
    // costs one exit per module page ever touched rather than one per
    // access - a guest scanning memory pays once per page and then runs
    // at full speed through a region that tells it nothing.
    if (this->module_physical_to_virtual.find(page << 12) !=
        this->module_physical_to_virtual.end()) {
        ++this->module_access_count;

        diag::log<diag::severity::warning>(
            "guest touched module memory at {} from rip {}, {} pages so "
            "far",
            guest_physical,
            context.rip,
            this->module_access_count);
        log("guest touched module memory at {} from rip {}",
            guest_physical,
            context.rip);

        if (auto entry = epte_for(page << 12)) {
            (*entry)->page_number(
                this->host_page_table.virtual_to_physical(
                    this->unprotected_memory.decoy_page) >>
                12);

            // Executable as well as readable, deliberately. If the guest
            // jumps into this it should run zeroes and reach whatever
            // conclusion that leads to, rather than fault here forever
            // on a page it is allowed to touch.
            (*entry)->read(true);
            (*entry)->write(true);
            (*entry)->execute(true);
            (*entry)->execute_user(true);
            invalidate_ept();
        }

        return true;
    }

    return false;
}

bool hypervisor::on_monitor_trap_flag(std::size_t cpu,
                                      std::uint64_t rip)
{
    if (!this->stepping_watch[cpu]) {
        return false;
    }

    auto page = this->stepping_page[cpu];
    auto offset = this->stepping_offset[cpu];
    auto armed_at = this->stepping_rip[cpu];
    this->stepping_watch[cpu] = false;
    this->stepping_page[cpu] = {};
    this->stepping_offset[cpu] = {};
    this->stepping_rip[cpu] = {};
    monitor_trap_flag(false);

    // Close the page again before the handler runs, so that a handler
    // which arms or disarms watches cannot observe a half open state.
    //
    // **Unless another processor is still stepping the same page.** The
    // window this closes is partition-wide - the EPT entry is shared by
    // every processor - while the state above it is per processor. Two
    // processors stepping writes to one page therefore used to close it
    // under each other: cpu 0 opens the page and arms the monitor trap
    // flag, cpu 1 closes it after its own step, cpu 0's instruction
    // faults again instead of retiring, and neither makes progress.
    //
    // Measured with the local APIC page watch kept armed past adoption
    // (`ZPP_KEEP_APIC_WATCH`), which is the configuration that reaches
    // it: the local APIC page is the one page both processors write
    // constantly. zpp's counters were identical 100 s apart, and from
    // underneath - KVM's own per-VM statistics - `nested_run` was
    // frozen while `exits` climbed at about 600 a second, which is host
    // timer preemption of two spinning threads rather than exits either
    // processor takes.
    //
    // Checked against the other processors' own state rather than a new
    // counter, because that state already says exactly this and a
    // counter would have to be kept in step with it. The read races
    // with those processors, and both outcomes are safe: seeing a step
    // that has just ended leaves the page open one step longer, which
    // is a missed observation and the direction this file already
    // treats as safe; missing a step that has just begun closes the
    // page under it, which is the behaviour before this change.
    auto still_stepping = false;
    for (std::size_t other{}; other < max_cpus; ++other) {
        if ((other != cpu) && this->stepping_watch[other] &&
            (this->stepping_page[other] == page)) {
            still_stepping = true;
            break;
        }
    }

    if (!still_stepping) {
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(false);
            invalidate_ept();
        }
    }

    // Whether the instruction actually ran.
    //
    // A monitor-trap-flag exit does not say that it did. SDM 26.5.2
    // (.references/sdm.txt:201495): "If the instruction causes a fault,
    // an MTF VM exit is pending on the instruction boundary following
    // delivery of the fault (or any nested exception)", and the case
    // above it is the same for a pending event delivered before the
    // instruction can execute. In both the guest's write has not
    // happened and RIP is inside a handler, not past the instruction.
    //
    // An instruction that retired left RIP between one and fifteen bytes
    // on - fifteen being the architectural maximum length - and it
    // cannot have branched, because the only forms stepped here are the
    // ones that faulted writing a watched page. Anything else means a
    // handler was entered instead.
    //
    // Reporting nothing is the safe answer, because the alternative is
    // reporting a write that never happened: on the local APIC page that
    // hands on_interrupt_command a stale command register, which can
    // adopt a start-up IPI nobody sent. Counted, so that writes being
    // missed this way is visible rather than silent.
    //
    // Also bounded to the page. The read below is four bytes, and an
    // access resolved to the last three bytes of the page would read
    // past the end of the very page the emulation refused to straddle.
    constexpr std::uint64_t maximum_instruction_length = 15;
    constexpr std::uint64_t page_offset_mask = page_size - 1;

    // `rip` is the caller's `context.rip`, which `on_vm_exit` filled
    // from the VMCS at the top of this exit - after the stepped
    // instruction retired, which is the whole point of the reading.
    // Passed rather than read here because this function has no context
    // to take it from and the read is one exit to the layer below per
    // stepped instruction, which pairs one-for-one with the violations
    // above.
    auto advanced = rip - armed_at;
    auto retired =
        (0 != advanced) && (advanced <= maximum_instruction_length);
    auto within_page = offset <= ((page_offset_mask + 1) - 4);

    if (!retired || !within_page) {
        this->stepped_not_retired = this->stepped_not_retired + 1;
        return true;
    }

    // What the step accomplished, in the shape an emulated write arrives
    // in. The step has already run, so the page holds what the guest
    // wrote and the value can simply be read back; the address comes from
    // the exit, which reported it before the step was armed.
    //
    // **Passing nullptr here is what wedged the guest.** A handler given
    // a page and no register cannot act, so every command that arrived
    // this way was dropped - measured as 149 dropped against 178
    // emulated, with the decoder refusing none of them, so these are not
    // a decoder gap and no amount of decoder coverage would have reached
    // them. The guest's start-up IPIs were among the dropped, no
    // application processor started, and the firmware waited for them for
    // ever in EDK2's MpInitLib. The guest never left firmware.
    //
    // Four bytes because every register on the pages watched here is a
    // dword, and the width is not reported by the exit.
    if (auto slot = this->watched_access_count;
        slot < watched_access_capacity) {
        this->watched_accesses[slot] = watched_access{
            .page = page,
            .offset = offset,
        };

        this->watched_access_count = slot + 1;
    }

    auto stepped = guest_write{
        .address = (page << 12) | offset,
        .value =
            arch::x86_64::read32(reinterpret_cast<volatile std::uint8_t *>(
                (page << 12) | offset)),
        .size = 4,
    };

    for (auto & watch : this->watches) {
        if (watch.armed && (watch.page == page) && watch.on_write) {
            watch.on_write(watch.context, page, &stepped);
            break;
        }
    }

    return true;
}

void hypervisor::note_page_table_write(
    const guest_write * write)
{
    // One line per distinct entry cleared, and only for a write that
    // makes an entry not-present - which is the event the whole
    // descriptor-table thread has been reading the aftermath of.
    if ((nullptr == write) || (0 != (write->value & 1))) {
        return;
    }

    log("page table entry cleared: address {} value {} by rip {}",
        write->address,
        write->value,
        this->vmcs.guest_rip());
}

std::expected<void, zpp::error> hypervisor::watch_guest_page_writes(
    std::uint64_t guest_physical,
    page_watch::handler on_write,
    void * context,
    page_watch::mode behaviour,
    void (*before_write)(void *, std::uint64_t),
    page_watch::filter filter_write)
{
    auto page = guest_physical >> 12;

    // Re-arming the same page replaces the handler rather than taking a
    // second slot, so a caller that cannot easily tell whether it has
    // already armed does not silently exhaust the table.
    page_watch * free_slot{};
    for (auto & watch : this->watches) {
        if (watch.armed && (watch.page == page)) {
            watch.on_write = on_write;
            watch.context = context;
            watch.before_write = before_write;
            watch.filter_write = filter_write;
            watch.behaviour = behaviour;
            return {};
        }
        if (!watch.armed && !free_slot) {
            free_slot = &watch;
        }
    }

    if (!free_slot) {
        return std::unexpected(zpp::error{error::out_of_ept_entries});
    }

    // The EPT is an identity map, so the guest physical address is also
    // the host physical one. Taken through the same call the module
    // protection uses, so a 2 MB entry covering a device register page
    // is split here rather than silently protecting two megabytes of
    // unrelated memory.
    auto entry = epte_for(page << 12);
    if (!entry) {
        return std::unexpected(entry.error());
    }

    // Reads stay permitted. A driver polls status registers far more
    // often than it writes commands, and every permitted read is a VM
    // exit that does not happen.
    //
    // `invalidate_ept` is what tells the composed shadows about this, and
    // it is not only a hardware flush: it bumps `ept_generation`, and
    // every processor drops the shadow leaves it composed from the old
    // permission before its next entry. See `discard_stale_shadow_ept`.
    // Arming is the direction that fails *silently* without it - a leaf
    // already granting write goes on granting it and the watch never
    // fires.
    (*entry)->write(false);
    invalidate_ept();

    free_slot->page = page;
    free_slot->on_write = on_write;
    free_slot->context = context;
    free_slot->before_write = before_write;
    free_slot->filter_write = filter_write;
    free_slot->behaviour = behaviour;
    free_slot->held.store(false, std::memory_order_relaxed);
    free_slot->armed = true;

    log("watching writes to guest page {}", page);
    return {};
}

void hypervisor::unwatch_guest_page(std::uint64_t guest_physical)
{
    auto page = guest_physical >> 12;

    for (auto & watch : this->watches) {
        if (!watch.armed || (watch.page != page)) {
            continue;
        }

        // The entry exists already - the page was split when the watch
        // was armed - so this cannot fail and nothing here has to cope
        // with it failing.
        //
        // And `invalidate_ept` here reaches the composed shadows through
        // `ept_generation`, not only the processor's own translations.
        // Without that a shadow leaf composed while the page was
        // write-protected goes on refusing the write, into a handler with
        // no watch left to answer it. See `discard_stale_shadow_ept`.
        if (auto entry = epte_for(page << 12)) {
            (*entry)->write(true);
            invalidate_ept();
        }

        watch.page = {};
        watch.on_write = {};
        watch.context = {};
        watch.behaviour = page_watch::mode::notify;
        watch.held.store(false, std::memory_order_relaxed);
        watch.armed = false;
        log("stopped watching guest page {}", page);
        return;
    }
}

} // namespace zpp::hypervisor
