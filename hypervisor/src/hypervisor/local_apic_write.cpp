// Watching the local APIC page.
//
// Three functions, split out of local_apic.cpp beside it:
// `watch_local_apic` takes write permission away from the page and
// registers the two below with the watched-page machinery,
// `filter_local_apic_write` decides what a write means and whether the
// guest's own store should still go out, and `on_local_apic_write` acts
// on it.
//
// The line the split follows is the page against the processor. What
// stays in local_apic.cpp is what is true of the APIC itself and of this
// processor's interception of it - `x2apic_enabled`, `note_apic_mode`,
// `intercept_interrupt_command` and `monitor_trap_flag`, which are read
// and written through model specific registers and VMCS controls. What
// comes here is everything that goes through the *page*: arming the
// watch on it, and the two handlers that fire when it is written.
//
// The practical evidence for that being a real seam rather than a
// convenient one is that tests/watched_page compiles this file whole and
// needs nothing from the half left behind, where before the split it cut
// two bodies out of local_apic.cpp by name.
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <cstdint>
#include <optional>

namespace zpp::hypervisor
{
std::optional<std::uint64_t> hypervisor::filter_local_apic_write(
    void * context, std::uint64_t page, const guest_write * write)
{
    auto & self = *static_cast<hypervisor *>(context);

    if (!write) {
        return {};
    }

    // Writing the low half is what sends the command. Every other
    // register on the page - the end of interrupt, the task priority, the
    // destination half at 0x310 - is left exactly alone.
    constexpr std::uint64_t interrupt_command_low = 0x300;
    constexpr std::uint64_t interrupt_command_high = 0x310;
    constexpr std::uint64_t page_offset_mask = page_size - 1;

    // Only a 4-byte access on a 16-byte boundary is a register access at
    // all, and anything else is passed through without being read as a
    // command.
    //
    // SDM 13.4.1 (.references/sdm.txt:170419): "Registers are 32 bits,
    // 64 bits, or 256 bits in width; all are aligned on 128-bit
    // boundaries. All 32-bit registers should be accessed using 128-bit
    // aligned 32-bit loads or stores ... any access that touches bytes 4
    // through 15 of an APIC register may cause undefined behavior".
    // KVM's apic_mmio_write (lapic.c) opens with exactly this test -
    // `if (len != 4 || (offset & 0xf)) return 0;` - and drops the write.
    //
    // Without it the command register was composed from whatever a
    // narrow or misaligned access happened to carry. `mov byte [apic +
    // 0x300], 0x11` composed 0x0000000200000011 and **sent an interrupt
    // the guest never wrote** - vector from the one byte, delivery mode,
    // level and shorthand all zero, destination from a register the
    // guest was not writing. A four-byte write at offset 0x2fe put two
    // of its bytes into the command register while the filter was asked
    // about offset 0x2fe and saw nothing.
    //
    // The write itself is still performed: this page is the processor's
    // real local APIC, so what a narrow store does to it is the
    // hardware's business and the same thing would happen without a VMM
    // in the way. What is refused is *interpreting* it as a command.
    constexpr std::uint64_t register_size = 4;
    constexpr std::uint64_t register_alignment_mask = 0xf;

    if ((register_size != write->size) ||
        (0 != (write->address & register_alignment_mask))) {
        return write->value;
    }

    if (interrupt_command_low != (write->address & page_offset_mask)) {
        auto offset = write->address & page_offset_mask;

        // Which register, for the writes this hook deliberately passes
        // through. A wedged guest hypervisor writing the page twice every
        // few seconds is doing something with it, and "not the interrupt
        // command register" is all this used to be able to say. Collapsed
        // by the log the same way as everything else, so a register
        // written in a loop with one value costs one line.
        //
        // Three registers are excluded, and "twice every few seconds" is
        // exactly the assumption that turned out to be wrong about them.
        // Measured on the rig: an idle Hyper-V writes the timer's initial
        // count and then the end of interrupt once per tick, at about ten
        // thousand a second on the boot processor alone - 1.25 million
        // exits in one run, the whole of that processor's exit ring, two
        // alternating RIPs. Every one took the global log lock and
        // allocated a line whose only content was a counter value that
        // never repeats, so the deduplication could not collapse them
        // either. The dominant cost of an interrupt on this machine was
        // this diagnostic.
        //
        // They are excluded rather than the log being made cheaper
        // because none of the three says anything: the end of interrupt
        // carries no value, the task priority is not consulted here, and
        // the initial count is a deadline whose *rate* is the interesting
        // thing and is better read from the exit ring. Everything a guest
        // writes once - the LVTs, the destination format, the spurious
        // vector - still logs, which is what this was added to see.
        constexpr std::uint64_t end_of_interrupt = 0xb0;
        constexpr std::uint64_t task_priority = 0x80;
        constexpr std::uint64_t timer_initial_count = 0x380;

        if ((end_of_interrupt != offset) && (task_priority != offset) &&
            (timer_initial_count != offset)) {
            log("guest apic write, register {}, value {}",
                offset,
                write->value);
        }

        // The two registers that decide what an initial count *means*,
        // remembered so the count can be recorded with them rather than
        // on its own. A count says nothing by itself: 0x320 carries the
        // mask bit, the mode - one-shot, periodic or TSC deadline - and
        // the vector, and 0x3e0 carries the divisor the count is scaled
        // by. Measured on the rig, the guest hypervisor uses all of it:
        // it starts periodic at vector 5 and then switches to one-shot at
        // vector 0xef with divide-by-one, arming and disarming once per
        // processor it brings up. A ring of bare counts recorded across
        // that switch would be two different quantities in one column.
        constexpr std::uint64_t lvt_timer = 0x320;
        constexpr std::uint64_t divide_configuration = 0x3e0;

        if (auto cpu = self.vmcs.vpid() - 1; cpu < max_cpus) {
            if (lvt_timer == offset) {
                self.timer_lvt[cpu] = write->value;
            } else if (divide_configuration == offset) {
                self.timer_divide[cpu] = write->value;
            }
        }

        // The initial count is excluded from the log above and recorded
        // here instead, in a fixed per-processor array that keeps the
        // *earliest* writes rather than the latest.
        //
        // Both halves of that matter. It cannot go in the log because it
        // is the hottest write on the page and no two values repeat, so
        // the deduplication that makes the log affordable does nothing
        // for it and a boot's worth would evict everything else. And it
        // must keep the earliest writes because the calibration is the
        // first thing a guest does with this register - a ring would hold
        // the millionth arming and throw away the one that decided it.
        //
        // The time stamp is what makes the values mean anything. A
        // calibration is an arm, a wait and a read back, so the real time
        // between two armings separates a guest that measured a true
        // interval and scaled it wrongly from one that measured an
        // interval this VMM had already stretched. Measured on the rig:
        // this guest arms to about 2.38e9 where the same guest with
        // nothing underneath arms to 1,961,755.
        if (timer_initial_count == offset) {
            if (auto cpu = self.vmcs.vpid() - 1; cpu < max_cpus) {
                if (auto slot = self.timer_arm_count[cpu];
                    slot < timer_arm_capacity) {
                    self.timer_arm_value[cpu][slot] = write->value;
                    self.timer_arm_tsc[cpu][slot] = arch::x86_64::rdtsc();
                    self.timer_arm_count[cpu] = slot + 1;
                }

                // And the newest, in a ring, which is a different
                // question from the one the array above answers.
                //
                // That one holds the calibration and must not be evicted
                // by it. This one holds the last deadline the guest
                // hypervisor programmed before the machine stopped, and
                // the stop is at the end - so the array above is
                // guaranteed to have been full for minutes by the time
                // anything interesting happens, and reading it after a
                // freeze shows the first thirty-two armings of a boot
                // whose last thirty-two are the ones being asked about.
                //
                // Recorded with the mode and divisor as they stood, since
                // a count is meaningless without them.
                auto slot =
                    self.timer_arm_recent_count[cpu] % timer_arm_capacity;
                self.timer_arm_recent_value[cpu][slot] = write->value;
                self.timer_arm_recent_tsc[cpu][slot] =
                    arch::x86_64::rdtsc();
                self.timer_arm_recent_lvt[cpu][slot] = self.timer_lvt[cpu];
                self.timer_arm_recent_divide[cpu][slot] =
                    self.timer_divide[cpu];
                self.timer_arm_recent_count[cpu] =
                    self.timer_arm_recent_count[cpu] + 1;
            }
        }

        return write->value;
    }

    // The destination half, which the guest wrote first - it has to, for
    // the same reason this hook exists - so it is already in the page and
    // is read from there rather than remembered.
    //
    // Composed into the x2APIC shape, destination in bits 63:32, so the
    // one decision function serves both forms. The xAPIC destination is
    // eight bits in 31:24 of that dword.
    auto * bytes = reinterpret_cast<volatile std::uint8_t *>(page << 12);
    auto high = arch::x86_64::read32(bytes + interrupt_command_high);

    auto command = (write->value & 0xffffffffull) |
                   (static_cast<std::uint64_t>(high >> 24) << 32);

    self.apic_page_commands_filtered =
        self.apic_page_commands_filtered + 1;

    // Nothing means this VMM answered the command itself and the guest's
    // write must not go out. A value means it did not, and the guest's
    // own command goes to the hardware unchanged.
    if (!self.on_interrupt_command(command)) {
        return {};
    }

    return write->value;
}

void hypervisor::on_local_apic_write(void * context,
                                     std::uint64_t page,
                                     const hypervisor::guest_write * write)
{
    auto & self = *static_cast<hypervisor *>(context);

    // Bring-up looks over, so stop paying for it. See
    // `nested_vmx::disarm_apic_watch` for what this is worth and what it
    // risks; the short version is 23.2% of every exit against a processor
    // that would run unvirtualized if one ever started after this.
    //
    // Checked here rather than on a timer because there is no timer, and
    // this handler runs on exactly the traffic the watch is being kept
    // alive for. Disarming does not skip the rest of this call: the store
    // has already been applied through this VMM's own mapping, so a write
    // that turns out to be an interrupt command is still acted on below,
    // and only the *next* one goes unseen.
    if constexpr (nested_vmx::disarm_apic_watch) {
        if (!self.all_processors_started.load(std::memory_order_relaxed) &&
            (0 != self.ipi_start_up_seen) &&
            (0 != self.last_start_up_ipi_tsc) &&
            ((arch::x86_64::rdtsc() - self.last_start_up_ipi_tsc) >
             nested_vmx::apic_watch_quiet_ticks)) {
            self.all_processors_started.store(true,
                                              std::memory_order_relaxed);
            log("no start-up ipi for {} ticks after {} of them, dropping "
                "the local apic page watch",
                nested_vmx::apic_watch_quiet_ticks,
                self.ipi_start_up_seen);
            self.watch_local_apic(false);
        }
    }

    // The write has already happened - the watch steps over it before
    // saying so - which is why the command can simply be read back out
    // of the page rather than decoded from the instruction.
    //
    // The xAPIC form is two dwords rather than one quadword: the low
    // half at 0x300, and the destination in the top eight bits of the
    // dword at 0x310. Composed here into the same shape the x2APIC path
    // produces, so one decision function serves both.
    constexpr std::uint64_t interrupt_command_low = 0x300;
    constexpr std::uint64_t interrupt_command_high = 0x310;

    auto * bytes = reinterpret_cast<volatile std::uint8_t *>(page << 12);
    auto low = arch::x86_64::read32(bytes + interrupt_command_low);
    auto high = arch::x86_64::read32(bytes + interrupt_command_high);

    // Which register was written, taken from the faulting address rather
    // than guessed from the register's contents.
    //
    // **This gate used to test delivery status, and that made the whole
    // path dead.** Bit 12 of the interrupt command register is Delivery
    // Status and it is *read only*: SDM 12.6.1 describes it as "Delivery
    // Status (Read Only)", with 0 meaning either no activity for this
    // source *or* that the previous interrupt from it "was delivered to
    // the processor core and accepted". Software cannot set it, so it was
    // never a marker for "software wrote a command".
    //
    // And by the time this handler runs the send has completed: the write
    // faults, the processor exits, the store is emulated, and only then is
    // the watch's handler called - hundreds of cycles after a delivery
    // that takes a handful. So the bit reads 0, the test refused every
    // write, and `on_interrupt_command` was never reached for an xAPIC
    // command. Measured: zero application processors adopted, seven left
    // in the firmware's own wait loop, and not one line in the log -
    // which the comment a few lines below correctly warns is the
    // indistinguishable case.
    //
    // KVM makes the same point from the other side: it clears the busy bit
    // on every guest write to this register, and its IPI decoder opens by
    // asserting the bit is never set by then - see `kvm_apic_send_ipi` and
    // the write path around it in arch/x86/kvm/lapic.c.
    //
    // Keying on the offset is exact. Writing the low half is what sends
    // the command, so this fires once per command and never on an end of
    // interrupt, which is the traffic the old test was trying to exclude.
    // Stepped rather than emulated, so which register was written is not
    // known - but the command is, because it is read from the page above
    // rather than from the instruction.
    //
    // **Returning here threw away every command that arrived this way,
    // and that was the wedge.** Measured on the rig with a hypervisor
    // announced to the guest: 178 writes to this page emulated and 149
    // stepped, with the decoder refusing *none* of them - so the stepped
    // ones are not a decoder gap, they are accesses the exit
    // qualification did not describe as an operand access, and no decoder
    // would have recovered them. The guest's start-up IPIs were among
    // them, no application processor ever started, and the firmware sat
    // in its own wait loop for them for ever: EDK2's MpInitLib blocks in
    // WaitApWakeup on a per-processor semaphore. The guest was stopped in
    // firmware, RIP unchanged across samples minutes apart, having never
    // reached the operating system.
    //
    // So the value is used and the offset is replaced by a weaker test
    // that this path can actually make: the interrupt command register
    // holding something other than what it last held. Any other register
    // on the page leaves it alone, so unchanged means "not this one".
    if (!write) {
        // Nothing at all is known about the access, which should no
        // longer happen: both the emulated and the stepped path now
        // report an address. Counted rather than guessed at, because
        // acting on the wrong register sends an interrupt nobody asked
        // for.
        self.apic_writes_undecoded = self.apic_writes_undecoded + 1;
        return;
    }

    // The destination alone sends nothing, so there is nothing to decide
    // until the low half follows it.
    auto offset = write->address & (page_size - 1);
    if (interrupt_command_low != offset) {
        return;
    }

    auto command = std::uint64_t{low} | (std::uint64_t{high >> 24} << 32);

    if (auto issue = self.on_interrupt_command(command)) {
        // Only where the handler changed it.
        //
        // **The write has already gone out.** The watch calls this after
        // the store has taken effect - emulated through this VMM's own
        // mapping of the page, which is the real interrupt command
        // register - so the command the guest wrote has already been
        // issued by the time this runs. Re-writing an unchanged value
        // sends it a second time, and two INITs or two start-up IPIs per
        // command is not a subtle fault: it was measured as a triple
        // fault, exit reason 2, after 179 emulated writes to this page.
        //
        // That did not happen while these writes were stepped rather than
        // emulated, because a stepped write arrives here undecoded and is
        // refused above - so the second send is new, and belongs to the
        // emulation.
        //
        // What this does not fix, and is recorded rather than hidden: a
        // handler that decides to *swallow* a command is too late, because
        // the send already happened. Swallowing needs the decision made
        // before the store is applied - `page_watch::before_write` is the
        // hook for it - and the x2APIC path next door is already the right
        // shape, deciding before it executes the write.
        if (*issue == command) {
            return;
        }

        // Put back what the handler decided, in the form this interface
        // takes. The high half is written first, because writing the low
        // half is what sends it.
        arch::x86_64::write32(
            bytes + interrupt_command_high,
            static_cast<std::uint32_t>((*issue >> 32) << 24));
        arch::x86_64::write32(bytes + interrupt_command_low,
                              static_cast<std::uint32_t>(*issue));
    }
}
void hypervisor::watch_local_apic(bool watch)
{
    // Where the page is, from the guest's own view of it. The base is
    // not architecturally fixed - IA32_APIC_BASE can relocate it - so it
    // is read rather than assumed to be 0xfee00000.
    constexpr std::uint64_t base_mask = 0xffffff000ull;
    auto base =
        arch::x86_64::rdmsr(arch::x86_64::msr::ia32_apic_base) & base_mask;

    if (!watch) {
        if (this->watched_apic_page) {
            unwatch_guest_page(this->watched_apic_page);
            this->watched_apic_page = 0;
        }
        return;
    }

    if (this->watched_apic_page == base) {
        return;
    }
    if (this->watched_apic_page) {
        unwatch_guest_page(this->watched_apic_page);
    }

    // A page this VMM's own table does not map is one the watch must not
    // be armed on, and refusing is the whole of the answer.
    //
    // `filter_local_apic_write` and `on_local_apic_write` both reach the
    // page by dereferencing `page << 12` as a **host virtual address**,
    // and the host page table maps exactly one local APIC page - read
    // from IA32_APIC_BASE once, before any guest ran. Relocating the
    // local APIC is a guest's to do: IA32_APIC_BASE[35:12] is writable
    // and `note_apic_mode` follows the move, so before this the watch
    // was armed on the new page and the guest's next interrupt-command
    // store took an EPT violation into a filter that read an unmapped
    // address. That is a #PF in the exit handler, where there is no
    // recovery point left to unwind to, so the processor simply stops -
    // the failure the comment at that dereference already describes, and
    // one that leaves nothing behind to say what happened.
    //
    // Mapping the new page instead was the obvious repair and does not
    // work: `map_from` walks the loader's OS page table through a
    // callback that stops resolving once a processor has switched to this
    // table, which is why the controller register pages beside it are
    // mapped before the switch rather than when they are first used.
    //
    // So the choice is between losing sight of the interrupt command
    // register and stopping the processor. Losing sight of it costs the
    // start-up IPI interception, which is real - but it is a degradation
    // the log names, where the alternative is a processor that vanishes
    // with nothing recorded anywhere.
    if (base != this->mapped_apic_page) {
        this->watched_apic_page = 0;
        log("refusing to watch a relocated local apic page at {}, this "
            "vmm maps {}",
            base,
            this->mapped_apic_page);
        return;
    }

    if (auto armed = watch_guest_page_writes(
            base,
            &hypervisor::on_local_apic_write,
            this,
            page_watch::mode::notify,
            nullptr,
            &hypervisor::filter_local_apic_write)) {
        this->watched_apic_page = base;
        log("watching the local apic page at {}", base);
    } else {
        // Refused rather than left half armed. Missing an IPI is bad;
        // believing one is being watched when it is not is worse.
        this->watched_apic_page = 0;
        log("could not watch the local apic page at {}", base);
    }
}

} // namespace zpp::hypervisor
