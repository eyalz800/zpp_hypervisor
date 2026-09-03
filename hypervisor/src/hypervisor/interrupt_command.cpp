// Reading a guest's write to the interrupt command register.
//
// Three functions, split from start_up.cpp beside it: `processor_slot`
// maps a local APIC identifier to this VMM's own slot for it,
// `on_interrupt_command` decodes what the guest asked for - the delivery
// mode, the shorthand, the destination - and `start_up_broadcast`
// resolves a broadcast against the platform's roster rather than passing
// it through.
//
// The line the split follows is decode against act. Everything here reads
// a command and works out who it is for; everything in start_up.cpp does
// something to a processor as a result - sends the IPI, hands the vector
// over, applies the INIT and start-up states, takes the processor into
// root mode. The dependency runs one way, from here into there, and never
// back.
//
// The seam is not invented for the split: tests/local_apic was written
// around exactly it, and describes itself as the harness for "the
// interrupt command register's decode path". Before this it cut these
// three bodies out of hypervisor.cpp by name; it compiles this file now,
// and needs nothing from start_up.cpp at all.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/scope_exit.h"
#include <cstdint>
#include <optional>

namespace zpp::hypervisor
{
std::optional<std::size_t>
hypervisor::processor_slot(std::uint64_t apic_id)
{
    // Under the same lock that guards a launch, because this is the other
    // half of the same resource: it hands out the index every per
    // processor array is addressed by, including the VMXON and VMCS
    // regions and the VPID.
    //
    // Unsynchronised it is a scan followed by an append, and every
    // processor reaches it from its own exit handler - `on_interrupt
    // _command` calls it for any start-up IPI with a physical
    // destination. Two processors asking about two *different* unknown
    // identifiers both read the same `number_of_known_processors`, both
    // take that slot, and the second overwrites the first's `apic_id`
    // entry. The result is two physical processors sharing one slot, and
    // a slot is not a label: `setup_vmcs` writes `vpid(cpu + 1)`, and
    // `initialize_vmx`'s own comment says why that is fatal - "a VMCS may
    // not be active on more than one logical processor".
    //
    // It cannot happen with one application processor, because there is
    // one identifier to allocate and the scan finds it. It becomes
    // possible with two, which is where the processor-count bisect in
    // BACKLOG.md puts the boundary.
    //
    // `start_up_lock` rather than a lock of its own, so that nothing new
    // has to be forced open after an S3 resume - see
    // `resume_from_sleep_on_this_processor`. No caller holds it already:
    // `on_interrupt_command`, `start_up_broadcast` and
    // `start_up_processor` each call this before `start_application
    // _processor` takes it, never inside. It is not recursive, so that
    // has to stay true.
    this->start_up_lock.lock();
    scope_exit unlock{[&] { this->start_up_lock.unlock(); }};

    for (std::size_t slot{}; slot < this->number_of_known_processors;
         ++slot) {
        if (this->apic_id[slot] == apic_id) {
            return slot;
        }
    }

    if (this->number_of_known_processors >= max_cpus) {
        return {};
    }

    auto slot = this->number_of_known_processors++;
    this->apic_id[slot] = apic_id;
    return slot;
}

std::optional<std::size_t>
hypervisor::known_processor_slot(std::uint64_t apic_id)
{
    // The scan half of `processor_slot`, under the same lock and without
    // the append. See the declaration for why the allocation must not
    // happen on this caller's behalf.
    this->start_up_lock.lock();
    scope_exit unlock{[&] { this->start_up_lock.unlock(); }};

    for (std::size_t slot{}; slot < this->number_of_known_processors;
         ++slot) {
        if (this->apic_id[slot] == apic_id) {
            return slot;
        }
    }

    return {};
}

void hypervisor::discard_start_up_for_init(std::uint64_t command)
{
    constexpr std::uint64_t level_assert = 1ull << 14;
    constexpr std::uint64_t trigger_mode_level = 1ull << 15;
    constexpr std::uint64_t destination_logical = 1ull << 11;
    constexpr std::uint64_t shorthand_shift = 18;
    constexpr std::uint64_t shorthand_mask = 0x3;
    constexpr std::uint64_t shorthand_none = 0;
    constexpr std::uint64_t shorthand_self = 1;
    constexpr std::uint64_t destination_shift = 32;

    // INIT level de-assert starts nothing and supersedes nothing. SDM
    // Figure 13-12, "101 (INIT Level De-assert)": "for this delivery mode
    // the level flag must be set to 0 and trigger mode flag to 1". The
    // command the rig recorded for it is 0x100008500, and `439abb5` was
    // reverted in part because the flag it added was being set for
    // exactly this.
    //
    // The pair, not the level bit alone, because that is the condition
    // KVM applies and it is the wider one: `.references/kvm/lapic.c`'s
    // `APIC_DM_INIT` case is `if (!trig_mode || level)`, so everything
    // except level-triggered-and-clear is an INIT. Testing the level bit
    // on its own would silently ignore an edge-triggered command with
    // the level bit clear, which this is not entitled to do.
    if ((0 != (command & trigger_mode_level)) &&
        (0 == (command & level_assert))) {
        return;
    }

    // A logical destination is an eight-bit message destination address
    // matched against each APIC's own LDR and DFR, not an identifier, so
    // there is nothing here that can resolve it - the start-up path
    // refuses these for the same reason a few lines below, and refusing
    // both keeps the pair consistent. A vector left held for such a
    // target is the pre-existing behaviour, not a new hole.
    if (0 != (command & destination_logical)) {
        return;
    }

    auto discard = [this](std::size_t slot) {
        if (auto queued = this->queued_start_up[slot].exchange(
                0, std::memory_order_acq_rel);
            0 != (queued & queued_start_up_valid)) {
            log("guest init ipi for cpu {} discards the start-up vector "
                "{} queued before it",
                slot,
                queued & 0xff);
        }
    };

    auto shorthand = (command >> shorthand_shift) & shorthand_mask;

    if (shorthand_none == shorthand) {
        if (auto slot =
                known_processor_slot(command >> destination_shift)) {
            discard(*slot);
        }
        return;
    }

    // "Self" reaches one processor and it is the sender, which is not a
    // processor anybody is waiting to start.
    if (shorthand_self == shorthand) {
        return;
    }

    // The two broadcasts. Resolved against the slots this VMM already
    // tracks rather than against the platform roster, because the roster
    // is a list of identifiers and what has to be cleared is a slot -
    // and a processor with no slot has no mailbox to clear.
    //
    // Read before the lock is taken, because `local_apic_id` is a CPUID
    // and the loop below holds `start_up_lock` - the same lock
    // `processor_slot` appends the table under, so the count and the
    // identifiers cannot move while they are being walked. Nothing
    // inside the loop takes it, which it must not: it is not recursive.
    auto self = local_apic_id();

    this->start_up_lock.lock();
    scope_exit unlock{[&] { this->start_up_lock.unlock(); }};

    for (std::size_t slot{}; slot < this->number_of_known_processors;
         ++slot) {
        if (this->apic_id[slot] == self) {
            continue;
        }
        discard(slot);
    }
}

std::optional<std::uint64_t>
hypervisor::on_interrupt_command(std::uint64_t command)
{
    // The interrupt command register, x2APIC form: vector in the low
    // eight bits, delivery mode in bits 10:8, and the destination APIC id
    // in the upper half rather than in a second register.
    constexpr std::uint64_t vector_mask = 0xff;
    constexpr std::uint64_t delivery_mode_shift = 8;
    constexpr std::uint64_t delivery_mode_mask = 0x7;
    constexpr std::uint64_t delivery_mode_start_up = 6;
    constexpr std::uint64_t destination_shift = 32;

    // The destination shorthand, in bits 19:18. Zero means the destination
    // field names the target; anything else is a broadcast and the
    // destination field means nothing at all.
    constexpr std::uint64_t shorthand_shift = 18;
    constexpr std::uint64_t shorthand_mask = 0x3;
    constexpr std::uint64_t shorthand_none = 0;

    // The destination mode, bit 11. SDM 13.6.1, Figure 13-12 and the table
    // beside it: "Destination Mode - Selects either physical (0) or
    // logical (1) destination mode". A physical destination field is an
    // APIC id; a logical one is an eight-bit message destination address,
    // which SDM 13.6.2.2 says a receiving APIC compares against its own
    // LDR and DFR. It is a bitmask under either model - flat or cluster -
    // and not an identifier at all.
    constexpr std::uint64_t destination_logical = 1ull << 11;

    // INIT, which is logged and otherwise left alone. Logged because it is
    // the first half of the only sequence that starts a processor, and
    // because a hang in that sequence is otherwise invisible: the log then
    // shows what the guest sent, to which destination, and in what order,
    // next to what each target did about it. Cheap - a guest sends INIT
    // only to start or reset a processor, never on a hot path, unlike
    // every other delivery mode reaching this function.
    //
    // Their absence says something too, and it is the first thing to check
    // when a processor never starts: only the x2APIC interrupt command
    // register is an MSR, so a guest still in xAPIC mode writes its
    // command to the APIC page and reaches none of this. No line here at
    // all means the sequence was never seen, not that it was never sent.
    constexpr std::uint64_t delivery_mode_init = 5;

    auto delivery_mode =
        (command >> delivery_mode_shift) & delivery_mode_mask;
    this->ipi_last_command = command;

    // Every command, not only the two that start a processor.
    //
    // A guest hypervisor that has stopped making progress and pokes the
    // interrupt command register every few seconds is sending *something*,
    // and which delivery mode and destination it is decides whether the
    // answer is here or above. Measured on the rig: two writes to
    // 0xfee00000 followed by about 2500 VMX-preemption timer exits at one
    // unchanging RIP, repeating for as long as the guest was left up, with
    // nothing in this log to say what was being sent.
    //
    // Affordable only because the log collapses a repeat into a [times=N]
    // on the line already there - a guest sending the same IPI in a loop
    // costs one line, not one per send. Without that this would be the
    // hottest logger in the tree.
    log("guest ipi command {}, delivery mode {}", command, delivery_mode);

    if (delivery_mode_init == delivery_mode) {
        this->ipi_init_seen = this->ipi_init_seen + 1;
        // Bring-up is still going. See `last_start_up_ipi_tsc`.
        this->last_start_up_ipi_tsc = arch::x86_64::rdtsc();
        log("guest init ipi, command {}", command);

        // Forwarded and nothing more, deliberately.
        //
        // This used to also flag every target so the target could apply
        // the INIT to itself at its next exit, on the argument that KVM's
        // `vmx_apic_init_signal_blocked` - `nested.vmxon &&
        // !is_guest_mode` - blocks an INIT for every instant a processor
        // is inside this VMM's code, and that `kvm_apic_accept_events`
        // then *clears* the start-up IPI rather than deferring it.
        //
        // That reasoning is still right and the mechanism was still
        // wrong, because a flag carries no ordering against the start-up
        // IPI that follows. Measured on the rig, from the log ring of a
        // boot that ended in `guest vmxoff`:
        //
        //     guest start-up ipi for cpu 1, vector 2, to hardware
        //     sipi cpu 2 entry_intr 0 idt_vectoring 0
        //     cpu 2 start-up ipi exit, vector 2
        //     cpu 1 applying a guest init the layer below did not deliver
        //     guest start-up ipi for cpu 1, vector 2, to hardware
        //
        // The flagged INIT landed *after* the processor had accepted its
        // start-up IPI and put it back in wait-for-SIPI, so the guest saw
        // no processor start, sent the pair again, and looped until the
        // guest hypervisor gave up and executed vmxoff.
        //
        // A second measured fault in it: the flag was set for INIT level
        // de-assert too - command 0x100008500, level clear and trigger
        // mode level - which architecturally starts nothing at all. So it
        // could invent an INIT, which the comment here used to deny.
        //
        // Restoring this needs a queue with the start-up IPI in it, in
        // the order the guest wrote them, which is what KVM's
        // `apic->pending_events` is. A flag is not that.
        //
        // **The queue exists now, and this is the half of KVM's ordering
        // it was missing.** `queued_start_up` holds one vector per
        // processor and nothing in it says which INIT-SIPI-SIPI sequence
        // that vector belongs to. `emulate_init_signal` cannot supply
        // that - it records, from a measurement, that clearing the slot
        // at the top of an INIT threw away the live vector, because this
        // VMM sees the start-up IPI before the target reaches its INIT
        // exit. So the *sender* clears it, which puts the two writes in
        // the order the guest made them on the one processor that sees
        // both.
        //
        // Measured, and this is the failure it closes. Vector 0x87 is
        // Hyper-V's trampoline; the second start-up IPI of the same
        // sequence arrives after the target is already running and is
        // queued because its activity record reads active:
        //
        //     guest start-up ipi for cpu 1, vector 0x2, to hardware
        //     guest start-up ipi for cpu 1, activity 0x0 is not
        //         wait-for-sipi, queued vector 0x2
        //     guest init ipi, command 0x10000c500
        //
        // The 0x2 then sat in the mailbox until the *next* INIT drained
        // it, and started the processor at 0x2000 where the guest
        // hypervisor has nothing. Nothing is injected here and no INIT is
        // applied to anybody: a vector the guest has superseded is
        // dropped, which is what hardware does with it.
        discard_start_up_for_init(command);

        return command;
    }

    if (delivery_mode_start_up != delivery_mode) {
        // Everything else goes out as the guest wrote it. That includes
        // the INIT handled above, and it especially includes INIT: it is
        // what leaves the target waiting for a start-up IPI, which is the
        // state the rest of this needs it in. There is nothing here that
        // improves on any of them.
        return command;
    }

    // Decoded rather than ignored, which is what used to happen. Reading
    // bits 63:32 of a shorthand command yields whatever the guest left
    // there - zero, in practice - and matching that against the table of
    // known processors credited the boot processor with a start-up IPI
    // nobody had sent it.
    this->ipi_start_up_seen = this->ipi_start_up_seen + 1;
    // Bring-up is still going. See `last_start_up_ipi_tsc`.
    this->last_start_up_ipi_tsc = arch::x86_64::rdtsc();

    // A broadcast names no destination, so there is nothing to look up.
    // It is resolved against the platform's roster instead and every
    // target run through the same path a targeted command takes.
    //
    // It used to be passed through, on the stated grounds that this VMM
    // could not enumerate what it would be broadcasting to. That was
    // true and the conclusion was still wrong: **this is how Windows
    // starts its processors**, so the case being punted on was the only
    // one that ever happens. Measured on the rig - one broadcast INIT
    // and two broadcast start-up IPIs, shorthand 3, all refused, and not
    // one processor adopted across an entire boot.
    //
    // Passing it through is worse than refusing when a guest hypervisor
    // is above: the processors do start, outside this VMM, so they
    // belong to neither layer. The guest hypervisor's rendezvous never
    // completes and it resets the machine, which is what it did.
    auto shorthand = (command >> shorthand_shift) & shorthand_mask;
    if (shorthand_none != shorthand) {
        auto vector = command & vector_mask;

        if (start_up_broadcast(vector)) {
            return {};
        }

        this->ipi_refused_shorthand = this->ipi_refused_shorthand + 1;
        log("broadcast start-up ipi, shorthand {}, vector {}, no roster "
            "to resolve it against",
            shorthand,
            vector);
        return command;
    }

    // A logical destination is a bitmask, so it is not something
    // processor_slot can be asked about - and asking it anyway is worse
    // than not answering. It compares the value against the table of known
    // APIC ids, fails to match any, and then *allocates a slot for it*,
    // spending an entry on a processor that does not exist and giving the
    // rest of this function a slot index that names the wrong one. With a
    // flat model and a single target the two even coincide often enough to
    // look as though it works: logical id 0x01 is also physical APIC id 1.
    //
    // Passed through rather than adopted, which is the same answer the
    // broadcast case above gets and for the same reason - this VMM has no
    // way to resolve a set of targets into the one processor it would have
    // to start in its own trampoline. That hands those processors to the
    // guest unvirtualized, and is said out loud rather than left to be
    // discovered.
    //
    // Resolving it properly would mean tracking each processor's LDR and
    // DFR and reproducing the match, which is what KVM does:
    // kvm_apic_match_dest in arch/x86/kvm/lapic.c branches on the
    // destination mode before comparing anything, and
    // kvm_apic_match_logical_addr then compares the address against LDR
    // under whichever of flat or cluster DFR selects. Both registers are
    // written through the APIC page or the x2APIC MSRs, so this VMM sees
    // neither today.
    if (0 != (command & destination_logical)) {
        // x2APIC makes the match arithmetic rather than bookkeeping.
        // SDM 12.12.3: in x2APIC mode the logical destination register
        // is read-only and derived from the x2APIC id - bits 31:16 are
        // the cluster, `id >> 4`, and bits 15:0 carry a single bit,
        // `1 << (id & 0xf)`. There is no DFR and nothing is written, so
        // the LDR this VMM "sees neither" of does not need to be seen:
        // it is computable from an id already known. This path is the
        // x2APIC form - `destination_shift` is 32, the whole upper
        // dword - so the decode applies to every command reaching it.
        //
        // Only a destination naming exactly ONE processor is taken. The
        // trampoline starts one processor, and choosing one of a set
        // would be a guess dressed as a resolution.
        if constexpr (nested_vmx::adopt_logical_start_up) {
            auto logical = command >> destination_shift;
            auto cluster = logical >> 16;
            auto members = logical & 0xffff;

            if ((0 != members) && (0 == (members & (members - 1)))) {
                std::uint64_t index = 0;

                for (auto walk = members; 0 == (walk & 1); walk >>= 1) {
                    index = index + 1;
                }

                auto resolved = (cluster << 4) | index;
                auto vector = command & vector_mask;

                this->ipi_logical_resolved =
                    this->ipi_logical_resolved + 1;

                log("start-up ipi logical destination {} resolved to "
                    "apic id {}, vector {}",
                    logical,
                    resolved,
                    vector);

                // The same two steps the physical path takes below, for
                // the same reasons - the guest has started that
                // processor whether or not this VMM adopts it.
                if (auto slot = processor_slot(resolved)) {
                    this->started_by_guest_start_up_ipi[*slot] = true;
                }

                return (start_up_result::adopted ==
                        start_up_processor(resolved, vector))
                           ? std::optional<std::uint64_t>{}
                           : std::optional<std::uint64_t>{command};
            }
        }

        this->ipi_refused_logical = this->ipi_refused_logical + 1;
        log("start-up ipi in logical destination mode, command {}, "
            "not adopted",
            command);
        return command;
    }

    auto destination = command >> destination_shift;
    auto vector = command & vector_mask;

    // Recorded before the attempt, because it is a fact about the guest
    // having asked rather than about the attempt succeeding: a start-up
    // IPI this VMM passes to hardware still means the guest has started
    // that processor, and its duplicate must still be ignored.
    if (auto slot = processor_slot(destination)) {
        this->started_by_guest_start_up_ipi[*slot] = true;
    }

    return (start_up_result::adopted ==
            start_up_processor(destination, vector))
               ? std::optional<std::uint64_t>{}
               : std::optional<std::uint64_t>{command};
}

bool hypervisor::start_up_broadcast(std::uint64_t vector)
{
    // Nothing to resolve against. Answered by the caller passing the
    // guest's own command through, which is what this did before the
    // roster existed - and is still the only thing left when a loader
    // could not supply one.
    if (0 == this->number_of_platform_processors) {
        return false;
    }

    auto self = local_apic_id();

    // "All excluding self", the only broadcast a start-up IPI may use.
    // The other two forms include the sender, which SDM 11.6.1 does not
    // allow for INIT or start-up delivery modes - both references agree,
    // and the guest sends the legal one.
    for (std::size_t i{}; i < this->number_of_platform_processors; ++i) {
        auto destination = this->platform_apic_id[i];
        if (destination == self) {
            continue;
        }

        if (auto slot = processor_slot(destination)) {
            this->started_by_guest_start_up_ipi[*slot] = true;
        }

        if (start_up_result::needs_hardware ==
            start_up_processor(destination, vector)) {
            // The guest's broadcast cannot be forwarded for one target
            // and swallowed for another, so this target gets its own
            // command. Identical in effect to the broadcast the guest
            // wrote, restricted to the processor that still needs it.
            log("broadcast start-up ipi, apic id {} needs hardware, "
                "vector {}",
                destination,
                vector);

            send_start_up_ipi(destination, vector);
        }
    }

    return true;
}

} // namespace zpp::hypervisor
