#include "zpp/hypervisor/nested_vmx.h"
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/diag/log.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/scope_exit.h"
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace zpp::hypervisor
{
namespace
{
using arch::x86_64::vmx::vmcs12;
using arch::x86_64::vmx::vmcs_field_encoding;
using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
using instruction_error = nested_vmx::instruction_error;

using nested_vmx::no_current_vmcs;

/**
 * The arithmetic flags SDM 33.2 has the VMX instructions set and clear.
 * @{
 */
constexpr std::uint64_t rflags_carry = 1ull << 0;
constexpr std::uint64_t rflags_parity = 1ull << 2;
constexpr std::uint64_t rflags_adjust = 1ull << 4;
constexpr std::uint64_t rflags_zero = 1ull << 6;
constexpr std::uint64_t rflags_sign = 1ull << 7;
constexpr std::uint64_t rflags_overflow = 1ull << 11;

constexpr std::uint64_t rflags_arithmetic = rflags_carry | rflags_parity |
                                            rflags_adjust | rflags_zero |
                                            rflags_sign | rflags_overflow;
/**
 * @}
 */

/**
 * RFLAGS.VM, virtual-8086 mode, which every VMX instruction raises #UD in.
 */
constexpr std::uint64_t rflags_virtual_8086 = 1ull << 17;

/**
 * CR4.VMXE, which is the bit the guest's read shadow answers for.
 */
constexpr std::uint64_t cr4_vmxe = 1ull << 13;

/**
 * How many times a VMX instruction's memory operand access is retried
 * before the fault is delivered. The refusal it exists for is a
 * page-table entry caught mid-composition, so it clears within a few
 * instructions or not at all - a small bound answers it, and an
 * unbounded one would spin inside an exit handler.
 */
constexpr std::size_t operand_retries = 8;

/**
 * IA32_FEATURE_CONTROL's lock bit and its permission for VMXON outside
 * SMX, which VMXON raises #GP without. SDM 33.3, VMXON: "#GP(0) ... if
 * ... (bit 0 (lock bit) of IA32_FEATURE_CONTROL MSR is clear) or (outside
 * SMX operation and bit 2 of IA32_FEATURE_CONTROL MSR is clear)".
 * @{
 */
constexpr std::uint64_t feature_control_lock = 1ull << 0;
constexpr std::uint64_t feature_control_vmxon_outside_smx = 1ull << 2;
/**
 * @}
 */

/**
 * Which of the two VM-exit information formats an instruction's exit uses
 * for its operand, and whether it has a memory operand at all.
 *
 * SDM Table 30-14 covers VMCLEAR, VMPTRLD, VMPTRST and VMXON, whose only
 * operand is m64. SDM Table 30-15 covers VMREAD and VMWRITE, which add
 * Reg1 and Reg2 and a memory-or-register bit. The addressing fields
 * shared by both sit at the same bit positions in each, which is what
 * lets one decoder serve them.
 */
struct memory_operand
{
    std::uint64_t scaling{};
    std::uint64_t address_size{};
    std::uint64_t segment{};
    std::uint64_t index_register{};
    bool index_valid{};
    std::uint64_t base_register{};
    bool base_valid{};
    bool is_register{};
    std::uint64_t register_1{};
    std::uint64_t register_2{};
};

/**
 * Takes the instruction-information field apart, per SDM Table 30-15.
 * Table 30-14's fields are the same bits with Reg1, Reg2 and bit 10
 * undefined, and bit 10 is documented there as "Cleared to 0" - so a
 * caller for one of those instructions may read is_register and find it
 * false.
 */
constexpr memory_operand decode_operand(std::uint64_t information)
{
    memory_operand operand;

    operand.scaling = information & 0x3;
    operand.register_1 = (information >> 3) & 0xf;
    operand.address_size = (information >> 7) & 0x7;
    operand.is_register = 0 != (information & (1ull << 10));
    operand.segment = (information >> 15) & 0x7;
    operand.index_register = (information >> 18) & 0xf;
    operand.index_valid = 0 == (information & (1ull << 22));
    operand.base_register = (information >> 23) & 0xf;
    operand.base_valid = 0 == (information & (1ull << 27));
    operand.register_2 = (information >> 28) & 0xf;

    return operand;
}

/**
 * Whether the exit reason belongs to an instruction that requires the
 * guest to already be in VMX operation.
 *
 * VMXON is the one that does not - it is how a processor gets there - and
 * its own precondition is CR4.VMXE instead. Everything else raises #UD
 * "if not in VMX operation", which every operation section in SDM 33.3
 * states as its first condition.
 */
constexpr bool requires_vmx_operation(basic_reason reason)
{
    return basic_reason::vmxon != reason;
}

} // namespace

void hypervisor::intercept_msr(std::uint32_t index, bool read, bool write)
{
    // Four 1024-byte bitmaps in order: reads of 00000000H-00001FFFH, reads
    // of C0000000H-C0001FFFH, writes of the low range, writes of the high
    // range. SDM 27.6.9, "MSR-Bitmap Address".
    constexpr std::size_t read_low = 0x000;
    constexpr std::size_t read_high = 0x400;
    constexpr std::size_t write_low = 0x800;
    constexpr std::size_t write_high = 0xc00;

    // Outside both ranges the bitmap is not consulted at all: SDM 28.1.3
    // makes such an access exit unconditionally. Arming a bit for one
    // would write into whichever range the arithmetic landed in, so it is
    // refused instead - the exit happens regardless, which is what the
    // rdmsr case in the exit handler relies on.
    std::size_t low_base{};
    std::uint32_t bit{};

    if (index < 0x2000) {
        low_base = read_low;
        bit = index;
    } else if ((index >= 0xc0000000) && (index < 0xc0002000)) {
        low_base = read_high;
        bit = index - 0xc0000000;
    } else {
        return;
    }

    auto high_base = (read_low == low_base) ? write_low : write_high;

    auto apply = [&](std::size_t base, bool set) {
        auto & byte = this->msr_bitmap[base + (bit / 8)];
        auto mask = static_cast<std::uint8_t>(1u << (bit % 8));

        if (set) {
            byte |= mask;
        } else {
            byte &= static_cast<std::uint8_t>(~mask);
        }
    };

    apply(low_base, read);
    apply(high_base, write);
}

std::uint64_t hypervisor::nested_vmx_capability_msr(std::size_t msr)
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    // The hardware's own value, cached at launch. Every answer below is
    // this narrowed, never replaced - see the declaration for why that
    // matters on a machine whose capability MSRs are themselves filtered.
    auto hardware = this->cached_vmx_msr(msr);

    // Narrows a control MSR: the low half says which controls must be 1
    // and the high half which may be 1 (SDM A.3.1). A control this VMM
    // cannot honour is removed from the may-be-1 half, and anything the
    // hardware insists on is put back, since a control that must be 1
    // must also be permitted to be 1.
    //
    // Below it, a diagnostic that answers one question and must never
    // ship on.
    //
    // A guest hypervisor that reads the capability MSRs and then declines
    // has been told something it will not accept, and the narrowing below
    // is the only difference between what this VMM reports and what the
    // processor underneath offers. Reporting the hardware's set unchanged
    // says whether the capability set is the reason at all - which is
    // worth knowing *before* implementing support for a capability that
    // turns out not to be the one being objected to.
    //
    // It is a lie while it is on. Every bit this normally withholds is
    // withheld because nothing here honours it, so a guest hypervisor
    // that takes one up on the offer gets a VMM that does not do what it
    // just promised. Diagnostic only, on a machine that can be rebooted.
    //
    // **A mask rather than a switch, and that is the whole point.** It was
    // a single bool, and a run with it on widened five capability MSRs at
    // once - so the result said "the capability set is the reason" and
    // could not say which capability, which is the question worth an
    // answer. One group per boot narrows it to one MSR in three boots,
    // which is what a target costing a reboot per variable is worth
    // spending. Set this to one `unnarrow_` value at a time.
    [[maybe_unused]] constexpr std::uint64_t unnarrow_nothing = 0;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_pin_based = 1ull
                                                                  << 0;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_primary = 1ull << 1;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_secondary = 1ull
                                                                  << 2;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_exits = 1ull << 3;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_entries = 1ull << 4;
    [[maybe_unused]] constexpr std::uint64_t unnarrow_ept_vpid = 1ull << 5;

    constexpr std::uint64_t report_hardware_capabilities_unnarrowed =
        unnarrow_nothing;

    // Whether this group is one of the ones being reported unnarrowed.
    // Written so `narrow` and the extended-page-table capabilities below
    // ask the same question, because the previous shape put the check
    // inside `narrow` alone - and IA32_VMX_EPT_VPID_CAP is not narrowed
    // through `narrow`, so it stayed narrowed through the whole
    // experiment. Two of the three capabilities that run concluded were
    // "the work" were never actually offered to the guest hypervisor that
    // engaged.
    auto reporting = [](std::uint64_t group) {
        return 0 != (report_hardware_capabilities_unnarrowed & group);
    };

    auto narrow = [&](std::uint64_t value,
                      std::uint64_t supported,
                      std::uint64_t group) {
        if (reporting(group)) {
            return value;
        }

        auto allowed_0 = value & 0xffffffff;
        auto allowed_1 = (value >> 32) & 0xffffffff;

        allowed_1 = (allowed_1 & supported) | allowed_0;

        return allowed_0 | (allowed_1 << 32);
    };

    switch (msr) {
    case vmx_msr::basic: {
        // Bits 30:0 are the revision identifier, and the one reported is
        // the shadow's rather than the hardware's: a region prepared for
        // one must not be accepted by the other. Bits 44:32 are the
        // number of bytes to allocate for a VMXON region or a VMCS, which
        // is the whole page the shadow occupies. Bits 53:50 are the
        // memory type to use for it, write back. Bit 55 says the four
        // TRUE capability MSRs exist, which they do below. SDM A.1.
        //
        // Bit 48 stays clear: A.1 says it "is always 0 for processors that
        // support Intel 64 architecture". Bits 49, 54, 56 and 58 stay
        // clear because none of what they report is implemented - the
        // dual-monitor treatment of SMIs, instruction information for INS
        // and OUTS, unrestricted event injection, and nested-exception
        // support.
        constexpr std::uint64_t vmcs_size = 4096;
        constexpr std::uint64_t write_back = 6;
        constexpr std::uint64_t true_controls = 1ull << 55;

        return vmcs12::revision | (vmcs_size << 32) | (write_back << 50) |
               true_controls;
    }

    case vmx_msr::pin_based_controls:
    case vmx_msr::true_pin_based_controls:
        return narrow(hardware,
                      nested_vmx::supported_pin_based_controls,
                      unnarrow_pin_based);

    case vmx_msr::processor_based_contorls:
    case vmx_msr::true_processor_based_controls:
        return narrow(hardware,
                      nested_vmx::supported_primary_controls,
                      unnarrow_primary);

    case vmx_msr::processor_based_contorls_2:
        // The secondary controls MSR is not a pair of halves like the
        // others: SDM A.3.3 gives it allowed-1 settings in bits 63:32 and
        // reserves bits 31:0 to zero. narrow handles that unchanged,
        // because an allowed-0 half of zero leaves nothing to put back.
        return narrow(hardware,
                      nested_vmx::supported_secondary_controls,
                      unnarrow_secondary);

    case vmx_msr::exit_controls:
    case vmx_msr::true_exit_controls:
        return narrow(
            hardware, nested_vmx::supported_exit_controls, unnarrow_exits);

    case vmx_msr::entry_controls:
    case vmx_msr::true_entry_controls:
        return narrow(hardware,
                      nested_vmx::supported_entry_controls,
                      unnarrow_entries);

    case vmx_msr::misc: {
        // Two changes to the hardware's value, both of them removals.
        //
        // Bit 29 is cleared, which is the statement that VMWRITE cannot
        // modify a VM-exit information field (SDM A.6). The shadow
        // enforces it, in vmcs_field_encoding::read_only.
        //
        // The CR3-target count in bits 24:16 is cleared to zero. The
        // CR3-target list is four VMCS fields nothing here consults, and
        // reporting a non-zero count would invite a first-level
        // hypervisor to fill them in and expect MOV to CR3 to stop
        // exiting for those values.
        // And the MSR-list capacity in bits 27:25, which SDM A.6 makes
        // "(N+1)*512" - so zero is 512 entries, which is what
        // `check_nested_msr_area` enforces. Narrowed rather than passed
        // through because the areas are processed in software here, one
        // guest memory read per entry, and a list a processor would
        // happily walk is time this VMM spends inside a VM exit.
        // And bit 7, the shutdown activity state. SDM A.6 makes bits 8:6
        // the bitmap of supported activity states - 6 for HLT, 7 for
        // shutdown, 8 for wait-for-SIPI - and "If an activity state is
        // not supported, the implementation causes a VM entry to fail if
        // it attempts to establish that activity state", which
        // `enter_or_park_l2` is what does here.
        //
        // Three states are kept and the reasons differ:
        //
        // - HLT, bit 6, because it is entered in hardware and costs
        //   nothing to honour. SDM 29.7.2 has the active state and the
        //   HLT state block the same events - start-up IPIs - so a
        //   second-level guest in it still takes the external interrupts
        //   and NMIs its hypervisor gets it back with.
        // - Wait-for-SIPI, bit 8, because it is honoured in software:
        //   `enter_or_park_l2` holds the processor in VMX root operation
        //   and gives its hypervisor the start-up IPI exit when one
        //   arrives. Withdrawing it instead would refuse an entry every
        //   hypervisor makes - parking a virtual processor it has not
        //   started yet is the ordinary use of the state.
        // - Shutdown, bit 7, is withdrawn, and it is the one of the three
        //   that could not be honoured either way. SDM 29.7.2 has it
        //   block external interrupts as well as start-up IPIs, so a
        //   processor entered in it takes no exits at all and nothing
        //   here ends it; and there is no software stand-in, because the
        //   architectural way out is an NMI or a reset, neither of which
        //   this VMM manufactures. KVM accepts exactly the same three,
        //   in `nested_check_guest_non_reg_state`
        //   (.references/kvm/nested.c:3117-3119).
        //
        // What would have to change to offer it: something that ends a
        // shutdown - INIT emulation reaching a second-level guest - and a
        // way to hold the processor meanwhile, which is what
        // `enter_or_park_l2` does for wait-for-SIPI.
        constexpr std::uint64_t vmwrite_to_exit_information = 1ull << 29;
        constexpr std::uint64_t cr3_target_count = 0x1ffull << 16;
        constexpr std::uint64_t msr_list_capacity = 0x7ull << 25;
        constexpr std::uint64_t shutdown_activity_state = 1ull << 7;

        return hardware &
               ~(vmwrite_to_exit_information | cr3_target_count |
                 msr_list_capacity | shutdown_activity_state);
    }

    case vmx_msr::cr0_fixed_0:
    case vmx_msr::cr0_fixed_1:
    case vmx_msr::cr4_fixed_0:
    case vmx_msr::cr4_fixed_1:
        // Passed through unchanged. These say which bits of CR0 and CR4
        // are fixed in VMX operation (SDM A.7 and A.8), and they are
        // facts about the processor the guest is really running on - the
        // same processor a first-level hypervisor's own guest would run
        // on. Narrowing them would describe a machine that does not
        // exist.
        return hardware;

    case vmx_msr::vmcs_enum:
        // "Bits 9:1 contain the highest index value used for any VMCS
        // encoding" (SDM A.9). This is where the shadow's index capacity
        // stops being an implementation detail and becomes something the
        // guest is told, which is what makes refusing a higher index an
        // answer rather than a surprise.
        return (vmcs_field_encoding::index_capacity - 1) << 1;

    case vmx_msr::vpid_ept_capability:
        // Narrowed to what the shadow builder and the two invalidation
        // instructions actually honour, and narrowed *from* the hardware's
        // rather than replacing it, so a machine that cannot do one of
        // these does not have it promised on its behalf.
        //
        // Not a pair of halves like the control MSRs - SDM A.10 makes
        // every bit a plain capability - so it is masked rather than run
        // through `narrow`, which would put an allowed-0 half back that
        // does not exist here.
        //
        // See nested_vmx::supported_ept_vpid_capabilities for why each bit
        // is in the list and why the four that are absent are absent.
        //
        // The diagnostic reaches this one too, which it did not when it
        // was a switch inside `narrow` - and that omission is why the run
        // that made a guest hypervisor engage says nothing about the two
        // capabilities in this MSR.
        if (reporting(unnarrow_ept_vpid)) {
            return hardware;
        }

        return hardware & nested_vmx::supported_ept_vpid_capabilities;

    case vmx_msr::vm_functions:
        // VM-function 0, extended-page-table pointer switching, and
        // nothing else.
        //
        // This used to answer zero, on the grounds that the secondary
        // control enabling VMFUNC was not offered either - a consistent
        // pair, and the wrong one. Virtual secure mode switches trust
        // level by switching the pointer, and where the processor
        // offers VMFUNC it does that with no hypercall at all; withheld,
        // the guest hypervisor falls back to the hypercall path, which
        // is the pair measured alternating for ever on the rig.
        //
        // Backed rather than claimed: `on_l2_exit` answers the exit,
        // reading the guest hypervisor's own list and translating each
        // entry through `shadow_ept_pointer_for` before the hardware
        // sees it, and `build_vmcs02` forces vmcs02's VM-function
        // controls to zero so the processor can never take the switch
        // itself. The layer below grants it - IA32_VMX_VMFUNC reads 1
        // on this rig - so this is a capability held and passed on.
        return 1;

    default:
        // Every index in the range is named above. Reaching here means
        // the range grew without this growing with it, so the hardware's
        // value is the least wrong answer - it is at least self
        // consistent - and the log says it happened.
        return hardware;
    }
}

bool hypervisor::on_nested_vmx_msr_read(std::size_t cpu,
                                        std::uint32_t index,
                                        arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        return false;
    }

    if (cpu >= max_cpus) {
        return false;
    }

    std::uint64_t value{};

    if (arch::x86_64::msr::ia32_feature_control == index) {
        value = this->guest_feature_control[cpu];
    } else if ((index >= arch::x86_64::vmx::msr::begin) &&
               (index < arch::x86_64::vmx::msr::end)) {
        value = nested_vmx_capability_msr(index);

        // Counted, because a guest that reads these and then does not
        // enter VMX operation has looked and declined - which points at a
        // capability this VMM does not advertise, and is a different
        // problem from a guest that never looked.
        if (this->nested_capability_reads < capability_answer_capacity) {
            this->capability_answers[this->nested_capability_reads] =
                capability_answer{.msr = index, .value = value};
        }

        this->nested_capability_reads = this->nested_capability_reads + 1;
        this->nested_capability_last_msr = index;
    } else {
        return false;
    }

    // RDMSR answers in EDX:EAX, and the high halves of RAX and RDX are
    // cleared - SDM Vol. 2B, RDMSR: "RDX:RAX := MSR[ECX]", with the note
    // that in 64-bit mode the high 32 bits of each are cleared.
    context.rax = value & 0xffffffff;
    context.rdx = value >> 32;

    return true;
}

bool hypervisor::on_nested_vmx_msr_write(std::size_t cpu,
                                         std::uint32_t index,
                                         arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        return false;
    }

    if (cpu >= max_cpus) {
        return false;
    }

    auto value = (context.rax & 0xffffffff) | (context.rdx << 32);

    if (arch::x86_64::msr::ia32_feature_control == index) {
        // Write-once, like the real register. SDM Table 7-1, "Layout of
        // IA32_FEATURE_CONTROL", on bit 0: "If the lock bit is set, WRMSR
        // to the IA32_FEATURE_CONTROL MSR will cause a general-protection
        // exception. Once the lock bit is set, the MSR cannot be modified
        // until a power-on reset."
        //
        // So a write after the lock is a fault rather than a silent
        // no-op, which is also the only honest answer: a hypervisor whose
        // write was ignored has no way to find out.
        if (0 !=
            (this->guest_feature_control[cpu] & feature_control_lock)) {
            inject_general_protection_fault();
            return true;
        }

        this->guest_feature_control[cpu] = value;
        return true;
    }

    if ((index >= arch::x86_64::vmx::msr::begin) &&
        (index < arch::x86_64::vmx::msr::end)) {
        // The capability MSRs are read-only. SDM Appendix A calls them
        // "VMX capability reporting" throughout and gives no write
        // behaviour for any of them, and a write to a read-only MSR is a
        // general protection fault.
        inject_general_protection_fault();
        return true;
    }

    return false;
}

bool hypervisor::on_vmx_instruction(std::size_t cpu,
                                    arch::x86_64::vmx::exit_reason reason,
                                    arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        // The guest was never told VMX exists, so the instruction is not
        // one it can legitimately have executed. The caller delivers the
        // #UD a processor without VMX would have.
        return false;
    }

    auto & vmcs = this->vmcs;

    // The index comes from `on_vm_exit`'s own parameter. These three
    // entry points each opened with `vmcs.vpid() - 1`, and a guest
    // hypervisor's VMREAD and VMWRITE are the two hottest instructions it
    // executes - so the answer to "which processor am I" was itself a
    // VMREAD, on the path whose whole cost is VMREADs.
    if (cpu >= max_cpus) {
        // Recorded rather than silently refused. The caller turns this
        // into a #UD, and a guest hypervisor answers a #UD on a VMX
        // instruction by bugchecking, since it believes nothing is above
        // it - so this branch, taken once, ends the machine. It had no
        // counter at all.
        //
        // The VPID is read here and only here. This path is by
        // construction not hot, and the whole point of the comment above
        // is that the index and the VPID are two spellings of one
        // identity; if they ever disagree, this is where it shows.
        this->index_out_of_range += 1;
        this->index_out_of_range_last = cpu;
        this->index_out_of_range_vpid = vmcs.vpid();
        return false;
    }

    auto basic = reason.basic();

    // The three #UD conditions every operation section in SDM 33.3 opens
    // with, other than the per-instruction one below: real-address mode,
    // virtual-8086 mode, and compatibility mode. Written as
    // "(CR0.PE = 0) or (RFLAGS.VM = 1) or (IA32_EFER.LMA = 1 and
    // CS.L = 0)".
    //
    // Checked here rather than left to hardware because hardware cannot
    // check them for us: the exit has already happened by the time this
    // runs, and 28.1.1's priority rule only tells us which the processor
    // *would* have chosen. In practice the processor has already applied
    // them - it faults instead of exiting - so these are belt and braces
    // against a state this VMM forced rather than the guest chose.
    constexpr std::uint64_t cr0_pe = 1ull << 0;
    constexpr std::uint64_t cs_long_mode = 1ull << 13;

    auto in_ia32e_mode =
        0 != (vmcs.vm_entry_controls() &
              arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);
    auto cs_64_bit = 0 != (vmcs.guest_cs_access_rights() & cs_long_mode);

    if ((0 == (vmcs.guest_cr0() & cr0_pe)) ||
        (0 != (vmcs.guest_rflags() & rflags_virtual_8086)) ||
        (in_ia32e_mode && !cs_64_bit)) {
        // Counted, with all four inputs, because the caller turns this
        // into a #UD and the level above answers a #UD by bugchecking.
        // KVM does not make this check at all - see the member's
        // declaration - so a disagreement here is this VMM's alone.
        if (cpu < max_cpus) {
            this->vmx_refusal_mode[cpu] += 1;
        }
        this->vmx_refusal_cr0 = vmcs.guest_cr0();
        this->vmx_refusal_rflags = vmcs.guest_rflags();
        this->vmx_refusal_entry_controls = vmcs.vm_entry_controls();
        this->vmx_refusal_cs_rights = vmcs.guest_cs_access_rights();
        return false;
    }

    // The per-instruction precondition. VMXON needs CR4.VMXE, and it is
    // the guest's *view* of CR4 that decides - the read shadow - because
    // the real register must keep VMXE for this VMM's own sake. KVM checks
    // the same bit for the same reason, noting in handle_vmxon that it
    // "must force CR4.VMXE=1 to enter the guest and so cannot rely on
    // hardware to perform the check, which has higher priority than
    // VM-Exit".
    //
    // Everything else needs the guest to already be in VMX operation.
    if (requires_vmx_operation(basic)) {
        if (!this->guest_in_vmx_operation[cpu]) {
            return false;
        }
    } else if (0 == (vmcs.cr4_read_shadow() & cr4_vmxe)) {
        if (cpu < max_cpus) {
            this->vmx_refusal_vmxe[cpu] += 1;
        }
        return false;
    }

    // CPL, which is the DPL in the stack segment's access rights. Above
    // zero it is a general protection fault for every one of these, and
    // SDM 28.1.1 puts "faults based on privilege level" above VM exits -
    // so this is unreachable on hardware that applied that rule, and is
    // here because acting on a privileged instruction from user mode is
    // the worse failure of the two.
    constexpr std::uint64_t access_rights_dpl_shift = 5;
    auto cpl =
        (vmcs.guest_ss_access_rights() >> access_rights_dpl_shift) & 0x3;
    if (0 != cpl) {
        if (cpu < max_cpus) {
            this->vmx_refusal_cpl[cpu] += 1;
        }
        this->vmx_refusal_ss_rights = vmcs.guest_ss_access_rights();
        inject_general_protection_fault();
        return false;
    }

    switch (basic) {
    case basic_reason::vmxon:
        return on_guest_vmxon(cpu, context);
    case basic_reason::vmxoff:
        return on_guest_vmxoff(cpu);
    case basic_reason::vmclear:
        return on_guest_vmclear(cpu, context);
    case basic_reason::vmptrld:
        return on_guest_vmptrld(cpu, context);
    case basic_reason::vmptrst:
        return on_guest_vmptrst(cpu, context);
    case basic_reason::vmread:
        return on_guest_vmread(cpu, context);
    case basic_reason::vmwrite:
        return on_guest_vmwrite(cpu, context);
    case basic_reason::vmlaunch:
    case basic_reason::vmresume:
        return on_guest_vmlaunch(cpu, basic, context);
    case basic_reason::invept:
        return on_guest_invept(cpu, context);
    case basic_reason::invvpid:
        return on_guest_invvpid(cpu, context);
    case basic_reason::vmcall:
        // VMCALL from a guest that has done VMXON is, from its own point
        // of view, VMCALL in VMX root operation - there is no VMM above
        // it that it knows of - and SDM Table 33-1 error 1 is exactly
        // that case. This VMM implements no hypercall interface, which
        // the hypervisor CPUID range already says by answering zero for
        // the interface and feature leaves.
        vmx_fail(cpu, instruction_error::vmcall_in_vmx_root_operation);
        return true;
    case basic_reason::vmfunc:
        // No VM function is supported: IA32_VMX_VMFUNC reads as zero and
        // the secondary control that enables the instruction is not
        // offered. SDM 33.3, VMFUNC, raises #UD when that control is 0,
        // which is what the caller does with a false return.
        return false;
    default:
        return false;
    }
}

void hypervisor::vmx_succeed()
{
    // SDM 33.2, VMsucceed: every arithmetic flag cleared.
    //
    // In the VMCS rather than in the captured context, and that is not
    // interchangeable: the context holds the *host's* flags, since it is
    // what restore_context puts back before executing the resume. The
    // guest's RFLAGS is loaded from this field on VM entry and from
    // nowhere else.
    this->vmcs.guest_rflags(this->vmcs.guest_rflags() &
                            ~rflags_arithmetic);
}

void hypervisor::vmx_fail_invalid()
{
    // SDM 33.2, VMfailInvalid: carry set, the rest cleared. This is the
    // failure that carries no error number, because there is no current
    // VMCS to record one in.
    this->vmcs.guest_rflags(
        (this->vmcs.guest_rflags() & ~rflags_arithmetic) | rflags_carry);

    // Counted, because until this existed "no VM entry is being refused"
    // was not a measurement - it was the absence of one. A guest
    // hypervisor being told the same error thousands of times a second
    // and a guest hypervisor not executing the instruction at all
    // produced identical evidence, and they have entirely different
    // causes.
    if (auto slot = this->vmcs.vpid(); (0 != slot) && (slot <= max_cpus)) {
        this->nested_vmfail_count[slot - 1] =
            this->nested_vmfail_count[slot - 1] + 1;
        this->nested_last_vmfail[slot - 1] = 0;
    }
}

void hypervisor::vmx_fail_valid(std::size_t cpu, instruction_error error)
{
    // SDM 33.2, VMfailValid: zero set, the rest cleared, and the error
    // number written to the VM-instruction error field of the current
    // VMCS - which is the shadow, not this VMM's own.
    this->vmcs.guest_rflags(
        (this->vmcs.guest_rflags() & ~rflags_arithmetic) | rflags_zero);

    this->guest_vmcs12[cpu].write(
        arch::x86_64::vmx::vmcs::field::vm_instruction_error,
        static_cast<std::uint64_t>(error));

    // As vmx_fail_invalid: the error number is kept so a wedged guest
    // can be asked what it was last told, rather than inferred from a
    // log line that only one of the refusal paths emits.
    if (cpu < max_cpus) {
        this->nested_vmfail_count[cpu] =
            this->nested_vmfail_count[cpu] + 1;
        this->nested_last_vmfail[cpu] = static_cast<std::uint64_t>(error);
    }
}

void hypervisor::vmx_fail(std::size_t cpu, instruction_error error)
{
    // SDM 33.2, VMfail: "IF VMCS pointer is valid THEN
    // VMfailValid(ErrorNumber); ELSE VMfailInvalid". The error number has
    // nowhere to go without a current VMCS.
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        vmx_fail_invalid();
        return;
    }

    vmx_fail_valid(cpu, error);
}

std::expected<std::uint64_t, zpp::error>
hypervisor::vmx_operand_linear_address(
    const arch::x86_64::context & context)
{
    auto operand =
        decode_operand(this->vmcs.vm_exit_instruction_information());

    // A register operand where the instruction has none. SDM 33.3, VMXON,
    // raises #UD for exactly this: "IF (register operand) ... THEN #UD".
    if (operand.is_register) {
        return std::unexpected(
            zpp::error{error::guest_address_not_mapped});
    }

    // The exit qualification holds the displacement and nothing else, as
    // SDM 30.2.1 says for these instructions, sign extended to the
    // instruction's address size. Everything else that makes up the
    // effective address is in the instruction-information field.
    auto offset = this->vmcs.exit_qualification();

    constexpr std::uint64_t address_size_16 = 0;
    constexpr std::uint64_t address_size_32 = 1;

    if (address_size_32 == operand.address_size) {
        offset = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(offset)));
    } else if (address_size_16 == operand.address_size) {
        offset = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int16_t>(offset)));
    }

    if (operand.base_valid) {
        offset += guest_register(context, operand.base_register);
    }

    if (operand.index_valid) {
        offset += guest_register(context, operand.index_register)
                  << operand.scaling;
    }

    // The effective address is truncated to the address size before the
    // segment base is added, which is why this is not done afterwards.
    if (address_size_32 == operand.address_size) {
        offset &= 0xffffffff;
    } else if (address_size_16 == operand.address_size) {
        offset &= 0xffff;
    }

    // In 64-bit mode only FS and GS have a base that participates; the
    // others are treated as zero based. Outside it every segment's base is
    // added and the result truncated to 32 bits.
    constexpr std::uint64_t segment_fs = 4;
    constexpr std::uint64_t segment_gs = 5;

    auto in_ia32e_mode =
        0 != (this->vmcs.vm_entry_controls() &
              arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);

    if (in_ia32e_mode) {
        if (segment_fs == operand.segment) {
            return this->vmcs.guest_fs_base() + offset;
        }

        if (segment_gs == operand.segment) {
            return this->vmcs.guest_gs_base() + offset;
        }

        return offset;
    }

    return (guest_segment_base(operand.segment) + offset) & 0xffffffff;
}

std::uint64_t hypervisor::guest_segment_base(std::uint64_t segment)
{
    // The encoding SDM Table 30-15 gives for the segment-register field:
    // 0 ES, 1 CS, 2 SS, 3 DS, 4 FS, 5 GS.
    switch (segment) {
    case 0:
        return this->vmcs.guest_es_base();
    case 1:
        return this->vmcs.guest_cs_base();
    case 2:
        return this->vmcs.guest_ss_base();
    case 3:
        return this->vmcs.guest_ds_base();
    case 4:
        return this->vmcs.guest_fs_base();
    default:
        return this->vmcs.guest_gs_base();
    }
}

std::uint64_t hypervisor::guest_register(
    const arch::x86_64::context & context, std::uint64_t encoding)
{
    // RSP is not in the captured context. The exit stub stores the address
    // of the context structure there, deliberately, because
    // restore_context iretqs onto it - so reading context.rsp would give a
    // hypervisor stack address. BACKLOG.md records this as one of the
    // three defects that keep the write emulator switched off; here it is
    // simply read from the VMCS instead.
    constexpr std::uint64_t encoded_rsp = 4;
    if (encoded_rsp == encoding) {
        return this->vmcs.guest_rsp();
    }

    if (encoding >= std::size(arch::x86_64::detail::encoded_registers)) {
        return 0;
    }

    return context.*arch::x86_64::detail::encoded_registers[encoding];
}

void hypervisor::set_guest_register(arch::x86_64::context & context,
                                    std::uint64_t encoding,
                                    std::uint64_t value)
{
    constexpr std::uint64_t encoded_rsp = 4;
    if (encoded_rsp == encoding) {
        this->vmcs.guest_rsp(value);
        return;
    }

    if (encoding >= std::size(arch::x86_64::detail::encoded_registers)) {
        return;
    }

    context.*arch::x86_64::detail::encoded_registers[encoding] = value;
}

std::expected<void, zpp::error> hypervisor::read_guest_linear(
    std::uint64_t linear, std::span<std::byte> into)
{
    // A page at a time, because a translation is only good for the page it
    // resolved and an eight-byte operand may straddle two.
    std::size_t done{};
    while (done < into.size()) {
        auto at = linear + done;
        auto offset = at & (page_size - 1);
        auto count = page_size - offset;
        if (count > (into.size() - done)) {
            count = into.size() - done;
        }

        auto physical = guest_linear_to_physical(at);
        if (!physical) {
            return std::unexpected(physical.error());
        }

        auto read =
            read_guest_physical(*physical, into.subspan(done, count));
        if (!read) {
            return read;
        }

        done += count;
    }

    return {};
}

std::expected<void, zpp::error> hypervisor::write_guest_linear(
    std::uint64_t linear, std::span<const std::byte> from)
{
    std::size_t done{};
    while (done < from.size()) {
        auto at = linear + done;
        auto offset = at & (page_size - 1);
        auto count = page_size - offset;
        if (count > (from.size() - done)) {
            count = from.size() - done;
        }

        auto physical = guest_linear_to_physical(at);
        if (!physical) {
            return std::unexpected(physical.error());
        }

        auto written =
            write_guest_physical(*physical, from.subspan(done, count));
        if (!written) {
            return written;
        }

        done += count;
    }

    return {};
}

std::expected<std::uint64_t, zpp::error>
hypervisor::read_guest_vmcs_pointer(const arch::x86_64::context & context)
{
    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
        return std::unexpected(linear.error());
    }

    std::uint64_t pointer{};
    auto read = read_guest_linear(
        *linear,
        std::span(reinterpret_cast<std::byte *>(&pointer),
                  sizeof(pointer)));
    if (!read) {
        return std::unexpected(read.error());
    }

    return pointer;
}

bool hypervisor::vmcs_pointer_valid(std::uint64_t pointer)
{
    // "IF addr is not 4KB-aligned or addr sets any bits beyond the
    // physical-address width THEN VMfailInvalid" - SDM 33.3, VMXON, and
    // the same words in VMPTRLD and VMCLEAR.
    //
    // The width used is the one the extended page tables actually
    // describe rather than the processor's MAXPHYADDR, because a pointer
    // past it is one this VMM cannot reach: read_guest_physical refuses
    // it, and refusing here turns that into the architectural answer
    // instead of an internal error.
    constexpr std::uint64_t limit = 512ull * 1024 * 1024 * 1024;

    return (0 == (pointer & (page_size - 1))) && (pointer < limit) &&
           (0 != pointer);
}

bool hypervisor::on_guest_vmxon(std::size_t cpu,
                                arch::x86_64::context & context)
{
    // Already in VMX operation, which is a failure and not a fault. SDM
    // 33.3, VMXON: "ELSE VMfail('VMXON executed in VMX root operation')".
    if (this->guest_in_vmx_operation[cpu]) {
        vmx_fail(cpu, instruction_error::vmxon_in_vmx_root_operation);
        return true;
    }

    // The feature control gate, which is a general protection fault rather
    // than a VM failure. Checked against the guest's own copy of the
    // register, since the hardware one was locked by this VMM's launch.
    constexpr auto needed =
        feature_control_lock | feature_control_vmxon_outside_smx;

    if (needed != (this->guest_feature_control[cpu] & needed)) {
        inject_general_protection_fault();
        return false;
    }

    auto pointer = read_guest_vmcs_pointer(context);
    if (!pointer) {
        // The operand could not be fetched: either it was a register,
        // which is #UD, or its address does not translate, which would
        // have been a page fault. #UD is the answer for the first and the
        // caller's #UD is what a guest sees for the second - a wrong
        // exception, but a fault either way rather than a resume past an
        // instruction that did nothing.
        return false;
    }

    if (!vmcs_pointer_valid(*pointer)) {
        vmx_fail_invalid();
        return true;
    }

    // The first dword of the region must be the revision identifier this
    // VMM reports in IA32_VMX_BASIC, with bit 31 clear. SDM 33.3, VMXON:
    // "IF rev[30:0] != VMCS revision identifier supported by processor OR
    // rev[31] = 1 THEN VMfailInvalid".
    std::uint32_t revision{};
    auto read = read_guest_physical(
        *pointer,
        std::span(reinterpret_cast<std::byte *>(&revision),
                  sizeof(revision)));
    if (!read) {
        vmx_fail_invalid();
        return true;
    }

    if (vmcs12::revision != revision) {
        vmx_fail_invalid();
        return true;
    }

    this->guest_in_vmx_operation[cpu] = true;
    this->guest_vmxon_pointer[cpu] = *pointer;
    set_guest_current_vmcs(cpu, no_current_vmcs);

    this->guest_vmcs12[cpu].clear();

    this->guest_vmxon_count[cpu] = this->guest_vmxon_count[cpu] + 1;

    log("cpu {} guest vmxon at {}", cpu, *pointer);

    // Also to the retained channel, so the sequence reaches the medium
    // rather than only a debugger's view of memory. A guest hypervisor
    // entering VMX operation is the single most interesting thing this
    // VMM can report, and it was previously visible only to whoever
    // thought to walk the log list by hand.
    diag::log<diag::severity::warning>(
        "guest entered vmx operation on cpu {}", cpu);

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_vmxoff(std::size_t cpu)
{
    // Anything still current has to reach its own region before the
    // pointer to it is forgotten. SDM 27.11.1 asks for the same thing of
    // real software: "software should execute VMCLEAR for that VMCS
    // before executing the VMXOFF instruction", and the reason given is
    // that a VMCS active when a processor leaves VMX operation "may be
    // corrupted".
    flush_guest_vmcs12(cpu);

    // No VMCS of the guest hypervisor's is current any more, and the
    // control may not stay on without one - VM entry checks the link
    // pointer whenever it is set.
    set_vmcs_shadowing(cpu, false);

    this->guest_in_vmx_operation[cpu] = false;
    this->guest_vmxon_pointer[cpu] = 0;
    set_guest_current_vmcs(cpu, no_current_vmcs);


    this->guest_vmxoff_count[cpu] = this->guest_vmxoff_count[cpu] + 1;

    log("cpu {} guest vmxoff", cpu);

    // The other half of the pair. Without this a guest hypervisor that
    // starts and then stands down is indistinguishable from one that never
    // started.
    diag::log<diag::severity::warning>(
        "guest left vmx operation on cpu {}", cpu);

    vmx_succeed();
    return true;
}

void hypervisor::flush_guest_vmcs12(std::size_t cpu)
{
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        return;
    }

    // **And the deferred guest state, or this writes over it.**
    //
    // The line below copies the *entire* vmcs12 structure into the
    // guest hypervisor's own VMCS page. With the bulk guest-state copy
    // deferred that area holds what it held before its guest last ran,
    // so the flush replaces the level above's record of its guest with
    // a stale one - on every VMPTRLD, which is about four per
    // trust-level round trip.
    //
    // This is the consumer no field-name search could find: it reads
    // vmcs12 by copying the whole structure, and it lives *inside this
    // VMM* rather than being something the guest hypervisor does. Three
    // ordering conditions were found and fixed before it, and it
    // survived all three because it shares their trigger without being
    // the same question - VMCLEAR and VMPTRLD invalidate the deferral,
    // and invalidating is not flushing.
    materialise_l2_guest_state(cpu);

    // What reaches the guest's own region has to include the writes it
    // made into the shadow without exiting, or a VMCS it reads back is
    // missing everything it wrote since the last VM entry.
    copy_shadow_to_vmcs12(cpu);

    auto & shadow = this->guest_vmcs12[cpu];

    // Failure is deliberately not reported. There is nothing a caller
    // could do about it - the instruction that provoked the flush has
    // already been decided - and the address was validated when it became
    // current, so a failure here means the guest unmapped its own VMCS.
    //
    // **Not reported is not the same as not counted.** This write is the
    // only thing that persists a vmcs12, and the region is the only copy
    // of one between the processor that had it and the processor that
    // asks for it next - so a failure here is indistinguishable, from
    // every later reader, from a vmcs12 whose RIP was always zero. See
    // `note_vmcs12_flush`.
    auto written = write_guest_physical(
        this->guest_current_vmcs[cpu],
        std::span(reinterpret_cast<const std::byte *>(&shadow),
                  sizeof(shadow)));

    note_vmcs12_flush(
        cpu,
        this->guest_current_vmcs[cpu],
        shadow.read(arch::x86_64::vmx::vmcs::field::guest_rip),
        written.has_value());
}

bool hypervisor::on_guest_vmclear(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto pointer = read_guest_vmcs_pointer(context);
    if (!pointer) {
        // The VMCS-pointer operand could not be read. Counted as a
        // guest-read failure, because that is what it is - the caller
        // answers it with #UD, which tells the level above the
        // instruction does not exist when the truth is that this VMM
        // could not fetch its operand.
        note_vmx_operand_failure(cpu,
                                 true,
                                 this->vmcs.guest_rip(),
                                 0,
                                 static_cast<std::uint64_t>(
                                     pointer.error().code()));
        return false;
    }

    if (!vmcs_pointer_valid(*pointer)) {
        vmx_fail(cpu, instruction_error::vmclear_invalid_address);
        return true;
    }

    if (*pointer == this->guest_vmxon_pointer[cpu]) {
        vmx_fail(cpu, instruction_error::vmclear_with_vmxon_pointer);
        return true;
    }

    // VMCLEAR does not discard a VMCS, it *saves* one. SDM 33.3's
    // operation is three steps - "ensure that data for VMCS referenced by
    // the operand is in memory; initialize implementation-specific data
    // in VMCS region; launch state of VMCS referenced by the operand :=
    // clear" - and the description says it "initializes parts of the VMCS
    // region (for example, it sets the launch state of that VMCS to
    // clear)". The data fields survive, which is the entire premise of
    // SDM 27.1's advice to VMCLEAR a VMCS before using it "on another
    // logical processor": the region has to still be a VMCS afterwards.
    //
    // Both branches here used to zero all 3584 bytes of field storage.
    // That is not a small divergence: the standard way to move a virtual
    // processor between logical processors is VMCLEAR on the old one,
    // VMPTRLD and VMLAUNCH on the new one, and against a wiped region the
    // VMLAUNCH cannot succeed - `within_capability` rejects all-zero pin
    // controls, because adjust_msr returns the allowed-0 bits and those
    // are never zero - so that virtual processor could never be entered
    // again, on any processor, for the life of the boot.
    //
    // KVM's handle_vmclear is the shape to match: it flushes the cached
    // vmcs12 to guest memory whole and then writes *four bytes* of zero
    // at offsetof(struct vmcs12, launch_state).
    if (*pointer == this->guest_current_vmcs[cpu]) {
        // The current one. The cache is this processor's copy and may be
        // ahead of the region, so the launch state is set in the cache
        // and the whole thing written out - which is both of the first
        // two steps at once, and keeps every field the guest wrote.
        this->guest_vmcs12[cpu].state(vmcs12::launch_state::clear);
        flush_guest_vmcs12(cpu);
        set_vmcs_shadowing(cpu, false);
        set_guest_current_vmcs(cpu, no_current_vmcs);


        vmx_succeed();
        return true;
    }

    // Not current, so its region is the only copy of it and there is
    // nothing to flush. Only the launch state is written, which is why it
    // lives in the region rather than beside the cache: a VMCLEAR of a
    // VMCS this processor has never loaded still has to reach it.
    auto cleared_state =
        static_cast<std::uint32_t>(vmcs12::launch_state::clear);

    static_cast<void>(write_guest_physical(
        *pointer + vmcs12::launch_state_offset,
        std::span(reinterpret_cast<const std::byte *>(&cleared_state),
                  sizeof(cleared_state))));

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_vmptrld(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto pointer = read_guest_vmcs_pointer(context);
    if (!pointer) {
        // The VMCS-pointer operand could not be read. Counted as a
        // guest-read failure, because that is what it is - the caller
        // answers it with #UD, which tells the level above the
        // instruction does not exist when the truth is that this VMM
        // could not fetch its operand.
        note_vmx_operand_failure(cpu,
                                 true,
                                 this->vmcs.guest_rip(),
                                 0,
                                 static_cast<std::uint64_t>(
                                     pointer.error().code()));
        return false;
    }

    if (!vmcs_pointer_valid(*pointer)) {
        vmx_fail(cpu, instruction_error::vmptrld_invalid_address);
        return true;
    }

    if (*pointer == this->guest_vmxon_pointer[cpu]) {
        vmx_fail(cpu, instruction_error::vmptrld_with_vmxon_pointer);
        return true;
    }

    // Already current, which is a success that changes nothing.
    //
    // This used to re-read the region, on the reasoning that SDM 33.3
    // does not special-case it. SDM 27.1 does, by name:
    // ".references/sdm.txt:199016" - "The figure does not illustrate
    // operations that do not modify the VMCS state relative to these
    // parameters (e.g., **execution of VMPTRLD X when X is already
    // current**)." VMPTRLD's own effect on VMCS data is the pointer
    // assignment and nothing else; a processor keeps the data in
    // implementation-specific storage and only guarantees it is in
    // memory after VMCLEAR, which is why SDM 27.1 also says software
    // "should never access or modify the VMCS data of an active VMCS
    // using ordinary memory operations".
    //
    // Re-reading was not harmless here, because in this VMM the *cache*
    // is the authoritative copy and the region is stale: nothing flushes
    // on VMWRITE, and reflect_l2_exit does not flush either, so from a
    // guest hypervisor's first VMWRITE the region holds a revision
    // identifier and zeroes. A redundant VMPTRLD therefore replaced the
    // whole live vmcs12 with that - launch state back to clear, every
    // control zero, and the second-level guest state save_l2_state had
    // accumulated gone. The next VMRESUME then fails
    // vmresume_with_non_launched_vmcs, and that virtual processor can
    // never be entered again: even a VMCLEAR and VMLAUNCH would enter it
    // at RIP zero.
    //
    // KVM guards exactly this - handle_vmptrld wraps the release and the
    // read of cached_vmcs12 in `if (vmx->nested.current_vmptr != vmptr)`,
    // so a redundant VMPTRLD is a no-op.
    //
    // The worry the old comment recorded - losing what a VMCLEAR from
    // another processor put in the region - is a VMCLEAR of a VMCS
    // active on this one, which the architecture does not permit and
    // KVM does not defend against either.
    if (*pointer == this->guest_current_vmcs[cpu]) {
        vmx_succeed();
        return true;
    }

    // **Is this VMCS current on another processor right now?** It must
    // not be - the architecture requires a VMCLEAR before a VMCS moves
    // between logical processors - and in this VMM it would be fatal
    // rather than merely undefined, because `guest_vmcs12` is indexed by
    // processor while a VMCS is identified by its physical address. The
    // other processor's writes live in *its* shadow, and this one would
    // read them from the region, which only `flush_guest_vmcs12` ever
    // fills. Everything that processor wrote and had not flushed is
    // silently lost.
    //
    // Only possible above one processor, which is the shape of the
    // failure being chased, so it is worth a check rather than an
    // assumption. Once per processor.
    for (std::size_t other{}; other < max_cpus; ++other) {
        if ((other == cpu) ||
            (*pointer != this->guest_current_vmcs[other])) {
            continue;
        }

        if (!this->vmcs12_shared_logged[cpu]) {
            this->vmcs12_shared_logged[cpu] = true;
            log("cpu {} vmptrld of {} which is current on cpu {}",
                cpu,
                *pointer,
                other);
        }
        break;
    }

    // **Retaining a vmcs12 per pointer was proposed here, measured, and
    // is not worth doing.** The reasoning is kept because the conclusion
    // reversed once it was bracketed rather than computed.
    //
    // The guest alternates between **exactly two** VMCSs - censused live,
    // `guest_current_vmcs` took `0x117a18000` and `0x117a1b000` and
    // nothing else over forty samples, VTL0's and VTL1's, mirroring the
    // two extended-page-table roots. So this reads back a structure this
    // VMM held moments earlier, twice a trust-level round trip, and it is
    // the one cache here that does not retain where `shadow_ept_slots`
    // keeps four slots for two roots and never evicts.
    //
    // **And the architecture sanctions retaining it.** SDM 27.1: a
    // logical processor "may maintain a number of VMCSs that are active"
    // and "may optimize VMX operation by maintaining the state of an
    // active VMCS in memory, on the processor, or both"; 27.11.1 forbids
    // software touching an active VMCS's region with ordinary memory
    // operations, because the format "is implementation-specific" and the
    // processor "may maintain some VMCS data of an active VMCS on the
    // processor and not in the VMCS region". Only VMCLEAR makes a VMCS
    // inactive, so only VMCLEAR would have to drop a slot.
    //
    // **What killed it is the cost.** The four intervals below decompose
    // this function at 97.6% coverage: the region read is 4.9 us a call
    // and the assignment 2.3, together **15.8 us a round trip** - while
    // `flush_guest_vmcs12` is **81%** of the whole. Retention avoids the
    // read and the assignment and nothing else, because the flush's cost
    // is `materialise_l2_guest_state` capturing the deferred guest state,
    // which has to happen before any switch or the deferral is lost.
    //
    // The estimate this replaced said 204 us, from reading the code and
    // multiplying twelve kilobytes by a guess. `BACKLOG.md` records it as
    // the third attribution the instrument had to correct.
    // Adjacent intervals over what a non-redundant VMPTRLD actually
    // does, plus the whole call, so the residue can be attributed
    // instead of guessed. The 204 microseconds a round trip this costs
    // beyond its VMCS accesses was measured; *which* of these four it is
    // was not, and a change built on the guess would be the fifth
    // correct-but-invisible one in this file.
    auto whole_start = arch::x86_64::rdtsc();
    auto whole_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][20] +=
                arch::x86_64::rdtsc() - whole_start;
            this->phase_calls[cpu][20] += 1;
        }
    });

    auto mark = [&](std::size_t slot, std::uint64_t since) {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][slot] += arch::x86_64::rdtsc() - since;
            this->phase_calls[cpu][slot] += 1;
        }
    };

    auto read_start = arch::x86_64::rdtsc();
    vmcs12 loaded;
    auto read = read_guest_physical(
        *pointer,
        std::span(reinterpret_cast<std::byte *>(&loaded), sizeof(loaded)));
    mark(16, read_start);
    if (!read) {
        vmx_fail(cpu, instruction_error::vmptrld_invalid_address);
        return true;
    }

    if (vmcs12::revision != loaded.revision_id()) {
        vmx_fail(cpu, instruction_error::vmptrld_incorrect_revision_id);
        return true;
    }

    // Only now, once the new one is known good, is the old one written
    // back. The pointers differ by the early return above, so there is no
    // longer a case where this flushes over the region just read.
    auto flush_start = arch::x86_64::rdtsc();
    flush_guest_vmcs12(cpu);
    mark(17, flush_start);

    auto assign_start = arch::x86_64::rdtsc();

    // A whole region read out of guest memory, which replaces every
    // field including the instruction pointer. See `low_rip_source`:
    // this is the fourth and last writer of vmcs12's RIP, and it is the
    // one that can carry a value neither level wrote in this VMM's
    // lifetime - the region is whatever `flush_guest_vmcs12` left there.
    auto rip12 = loaded.read(arch::x86_64::vmx::vmcs::field::guest_rip);
    auto rip12_before =
        this->guest_vmcs12[cpu].read(arch::x86_64::vmx::vmcs::field::guest_rip);

    if (rip12 < low_rip_threshold) {
        note_low_guest_rip(cpu,
                           low_rip_source::loaded_by_vmptrld,
                           rip12_before,
                           rip12,
                           0,
                           0,
                           *pointer);
    }

    // And the region's own history, which `low_rip_source` cannot carry:
    // whether *this* VMM ever managed to write this region, and which
    // processor did it last. A low RIP loaded out of a region no flush
    // ever succeeded on is a vmcs12 that migrated between processors
    // through a page nobody filled; a low RIP loaded out of a region
    // whose last successful flush persisted a high one is memory
    // disagreeing with what was written. Those want opposite fixes and
    // are indistinguishable without the second field. See
    // `note_vmcs12_load`.
    note_vmcs12_load(cpu, *pointer, rip12, rip12_before);

    this->guest_vmcs12[cpu] = loaded;
    set_guest_current_vmcs(cpu, *pointer);
    mark(18, assign_start);


    // A VMCS of the guest hypervisor's is now current, which is the
    // condition VMCS shadowing exists for and the condition the link
    // pointer has to be valid under. Publishes the new contents too.
    auto publish_start = arch::x86_64::rdtsc();
    set_vmcs_shadowing(cpu, true);
    mark(19, publish_start);

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_vmptrst(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    // "64-bit destination operand := current-VMCS pointer" - SDM 33.3,
    // VMPTRST - which for no current VMCS is the all-ones sentinel, so
    // there is nothing to special case.
    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
        // Counted, because the caller turns this false into a #UD and
        // a guest hypervisor bugchecks on that. See
        // `note_vmx_operand_failure`.
        note_vmx_operand_failure(cpu,
                                 false,
                                 this->vmcs.guest_rip(),
                                 0,
                                 static_cast<std::uint64_t>(
                                     linear.error().code()));
        return false;
    }

    auto pointer = this->guest_current_vmcs[cpu];

    auto written = write_guest_linear(
        *linear,
        std::span(reinterpret_cast<const std::byte *>(&pointer),
                  sizeof(pointer)));
    if (!written) {
        return false;
    }

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_vmread(std::size_t cpu,
                                 arch::x86_64::context & context)
{
    auto information = this->vmcs.vm_exit_instruction_information();
    auto operand = decode_operand(information);

    // The encoding is in the register the ModRM reg field names, which
    // SDM Table 30-15 reports as Reg2. KVM reads the same field the same
    // way in handle_vmread.
    auto encoding =
        vmcs_field_encoding(guest_register(context, operand.register_2));

    // No current VMCS means there is nowhere to read from and no error
    // field to record why. SDM 33.3, VMREAD: "IF (in VMX root operation
    // AND current-VMCS pointer is not valid) ... THEN VMfailInvalid".
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        vmx_fail_invalid();
        return true;
    }

    if (!encoding.valid()) {
        vmx_fail(cpu, instruction_error::unsupported_vmcs_component);
        return true;
    }

    record_vmcs_field_use(false, encoding.value());

    // Reaching here for a field the read bitmap permits means shadowing
    // is not in force, whatever the capability MSR says.
    note_shadowing_ineffective(cpu, encoding.value(), false);

    // The one interception point a deferred guest-state field can be
    // asked for, and it **repairs rather than asserts**: if the bulk
    // copy was skipped and this read wants one of those fields, the
    // copy happens here, now, before the value is produced. See
    // `guest_state_deferred`.
    //
    // Measured before it was relied on: of the sixteen fields the guest
    // hypervisor ever reads, none is in the deferred set, so this is
    // expected never to fire - and `guest_state_materialises` says so
    // rather than leaving it assumed. A non-zero count is not a fault,
    // it is this path doing its job.
    materialise_l2_guest_state_for(cpu, encoding.value());

    auto value = this->guest_vmcs12[cpu].read(encoding);

    // **The read side, which every instrument before this one was blind
    // to.** `low_rip_source` counts the writers of vmcs12's RIP and
    // established that the guest hypervisor stored `2` itself; it cannot
    // say what it read to get there, and `0 + 2` is a two-byte
    // instruction length added to a zero. This is the only point at
    // which a value leaves this VMM for the guest hypervisor's
    // destination operand, so it is the only place the question can be
    // asked. See `note_served_guest_rip` for why it records the shadow
    // region beside the value rather than the cache, which is the same
    // storage this line reads and so cannot disagree with it.
    if (static_cast<std::uint64_t>(
            arch::x86_64::vmx::vmcs::field::guest_rip) ==
        encoding.value()) {
        note_served_guest_rip(cpu, value);
    }

    if (operand.is_register) {
        set_guest_register(context, operand.register_1, value);
        vmx_succeed();
        return true;
    }

    // The memory form. The operand's size is decided by the mode, not by
    // the field: SDM 27.11.2 makes the effective operand size "always 32
    // bits outside IA-32e mode ... and 64 bits in 64-bit mode".
    // Both failures below return false, and the caller answers a false
    // with `#UD`. **That is the wrong fault for a memory access that did
    // not work**, and it is not a theoretical complaint: on a
    // multi-processor boot exactly one of these fires, on the second
    // processor, and the hypervisor log ends two entries later with the
    // secure kernel dead in `HvlSkCrashdumpCallbackRoutine`. An
    // invalid-opcode tells the guest hypervisor the instruction does not
    // exist, which is a lie about VMREAD - the one thing this file says
    // never to do.
    //
    // Which of the two fires is not yet known, and the fix differs by
    // which: an operand address that will not decode is a different
    // defect from a guest page this VMM cannot reach. So this logs
    // rather than guesses, and **deliberately does not change the
    // injected exception yet** - changing behaviour and instrumenting it
    // in the same build would leave neither measured.
    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
        this->vmread_memory_form_failures[cpu] =
            this->vmread_memory_form_failures[cpu] + 1;
        log("cpu {} vmread memory form: operand address would not "
            "decode, field {}, rip {}",
            cpu,
            static_cast<std::uint64_t>(encoding.value()),
            context.rip);
        return false;
    }

    auto in_ia32e_mode =
        0 != (this->vmcs.vm_entry_controls() &
              arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);
    auto size =
        in_ia32e_mode ? sizeof(std::uint64_t) : sizeof(std::uint32_t);

    auto written = write_guest_linear(
        *linear,
        std::span(reinterpret_cast<const std::byte *>(&value), size));

    // **Retried, because the refusal is transient and permanent damage
    // is what it otherwise causes.** Measured on a three-processor boot:
    // the walk refuses at level 0 - the PML4 - reading entry `0x2`, a
    // value with the write bit set and the present bit clear, which is
    // what a page-table entry looks like while software is part-way
    // through composing it. A second walk of the *same* table in the
    // same handler then succeeds.
    //
    // A real processor would not see this at all: it resolves the
    // operand through its own TLB, which holds the translation from
    // before the edit. This VMM walks the tables in memory instead, so
    // it sees an intermediate state hardware hides.
    //
    // Giving up leaves the guest hypervisor's VMCS **half updated** -
    // SDM 30.3 has a failed memory operand mean the VMWRITE does not
    // happen, so vmcs12 keeps its old field while its owner believes it
    // stored a new one. That is what put a stale 64-bit `GUEST_RIP`
    // beside a freshly cleared IA-32e-mode-guest control and failed
    // every three-processor entry with reason 0x80000021.
    for (std::size_t attempt{}; (!written) && (attempt < operand_retries);
         ++attempt) {
        this->operand_retry_count[cpu] =
            this->operand_retry_count[cpu] + 1;
        written = write_guest_linear(
            *linear,
            std::span(reinterpret_cast<const std::byte *>(&value), size));
    }

    if (!written) {
        this->vmread_memory_form_failures[cpu] =
            this->vmread_memory_form_failures[cpu] + 1;
        // Both halves - see the matching note on the VMWRITE path.
        auto translated = guest_linear_to_physical(*linear);

        log("cpu {} vmread mem form: at {} field {} rip {} "
            "phys {} err {} l2 {}",
            cpu,
            *linear,
            static_cast<std::uint64_t>(encoding.value()),
            context.rip,
            translated ? *translated : std::uint64_t{},
            static_cast<std::uint64_t>(written.error().code()),
            static_cast<std::uint64_t>(this->running_l2[cpu]));

        log("cpu {} walk refused at level {} entry {} table {} linear {}",
            cpu,
            this->walk_refusal_level[cpu],
            this->walk_refusal_entry[cpu],
            this->walk_refusal_table[cpu],
            this->walk_refusal_linear[cpu]);

        // **This is the branch that fires, and it is now measured
        // rather than guessed.** The comment above used to say the fix
        // differed by which of the two failures happened and so
        // deliberately changed nothing. A three-processor boot answered
        // it: the log ends with this line - never the decode one - on
        // the second processor, writing `GUEST_RIP` (field 0x681e) to a
        // kernel stack address, and four entries later the guest
        // hypervisor executes `vmxoff` on every processor and Windows
        // bugchecks HYPERVISOR_ERROR.
        //
        // So the operand decoded and the guest page was not writable
        // through this VMM's walk. SDM 27.11.2 says faults from the
        // memory operand are delivered as they would be by any other
        // access, which means #PF - and `#UD` says instead that VMREAD
        // does not exist, a lie about an instruction the guest
        // hypervisor is in the middle of using. A guest told #PF pages
        // the target in and re-executes; a guest told #UD has nowhere
        // to go.
        //
        // The error code is built rather than assumed, since the two
        // cases differ: SDM 4.7 bit 0 is set only when a translation
        // existed and refused the access, and clear when there was
        // none. Bit 1 is set because this is a write. Bit 2 is clear
        // because VMREAD outside CPL 0 has already taken #GP long
        // before here, so there is no user-mode case to encode.
        constexpr std::uint64_t fault_present = 1ull << 0;
        constexpr std::uint64_t fault_write = 1ull << 1;

        auto error_code = fault_write;
        if (translated) {
            error_code |= fault_present;
        }

        inject_page_fault(*linear, error_code);

        // False still, so the caller keeps RIP on the instruction. It
        // will not overwrite this injection: it re-injects only when
        // the interruption-information field says nothing is pending.
        return false;
    }

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_vmwrite(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto information = this->vmcs.vm_exit_instruction_information();
    auto operand = decode_operand(information);

    auto encoding =
        vmcs_field_encoding(guest_register(context, operand.register_2));

    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        vmx_fail_invalid();
        return true;
    }

    // The value comes from Reg1 or from memory. SDM 33.3, VMWRITE, orders
    // the checks so that "any faults resulting from accessing a memory
    // source operand occur after determining ... that the relevant VMCS
    // pointer is valid but before determining if the destination VMCS
    // field is supported" - hence the fetch sits between the two.
    std::uint64_t value{};

    if (operand.is_register) {
        value = guest_register(context, operand.register_1);
    } else {
        auto linear = vmx_operand_linear_address(context);
        if (!linear) {
            return false;
        }

        auto in_ia32e_mode =
            0 != (this->vmcs.vm_entry_controls() &
                  arch::x86_64::vmx::vm_entry_controls::ia_32e_mode_guest);
        auto size =
            in_ia32e_mode ? sizeof(std::uint64_t) : sizeof(std::uint32_t);

        // Captured before the access, so it can be compared with the
        // same field read after it. The walk that failed and the walk
        // that then succeeded took the same argument, so something they
        // both read changed between them, and CR3 is the first
        // candidate: it comes through the VMCS cache.
        auto cr3_before = this->vmcs.guest_cr3();

        auto read = read_guest_linear(
            *linear,
            std::span(reinterpret_cast<std::byte *>(&value), size));

        // Retried for the reason the VMREAD path above spells out.
        for (std::size_t attempt{};
             (!read) && (attempt < operand_retries);
             ++attempt) {
            this->operand_retry_count[cpu] =
                this->operand_retry_count[cpu] + 1;
            read = read_guest_linear(
                *linear,
                std::span(reinterpret_cast<std::byte *>(&value), size));
        }

        if (!read) {
            // Both halves, because one of them cannot tell you it is the
            // wrong half. `translated` says the page walk produced a
            // physical address, so a failure with it set means this VMM
            // could not *reach* a page the guest does have, and a
            // failure with it clear means the walk itself refused. The
            // fault delivered is right either way; which defect to chase
            // is not the same question, and a single field would have
            // answered neither.
            auto translated = guest_linear_to_physical(*linear);

            log("cpu {} vmwrite mem form: at {} rip {} phys {} err {} "
                "cr3 {} l2 {}",
                cpu,
                *linear,
                context.rip,
                translated ? *translated : std::uint64_t{},
                static_cast<std::uint64_t>(read.error().code()),
                cr3_before,
                static_cast<std::uint64_t>(this->running_l2[cpu]));

            log("cpu {} walk refused at level {} entry {} table {} "
                "linear {}",
                cpu,
                this->walk_refusal_level[cpu],
                this->walk_refusal_entry[cpu],
                this->walk_refusal_table[cpu],
                this->walk_refusal_linear[cpu]);

            // The same defect as the memory-form VMREAD above, mirrored,
            // and found the same way: with that one fixed the boot got
            // one instruction further and died here instead, exit reason
            // 0x19 where it had been 0x17. The SDM sentence quoted above
            // this block is the one that governs - faults from accessing
            // the memory source operand happen, and they are the faults
            // the access would raise, not #UD.
            //
            // Bit 1 clear because this is a read; otherwise the error
            // code is built exactly as the VMREAD path builds it.
            constexpr std::uint64_t fault_present = 1ull << 0;

            std::uint64_t error_code{};
            if (translated) {
                error_code |= fault_present;
            }

            inject_page_fault(*linear, error_code);
            return false;
        }
    }

    if (!encoding.valid()) {
        vmx_fail(cpu, instruction_error::unsupported_vmcs_component);
        return true;
    }

    if (encoding.read_only()) {
        vmx_fail(cpu, instruction_error::vmwrite_to_read_only_component);
        return true;
    }

    record_vmcs_field_use(true, encoding.value());

    // Reaching here for a field the write bitmap permits means shadowing
    // is not in force, whatever the capability MSR says.
    note_shadowing_ineffective(cpu, encoding.value(), true);

    // And the other half: a guest-state field the level above writes is
    // **its** value, owed to vmcs02 on the next entry and not to be
    // overwritten by a later materialisation. See `guest_state_dirty`.
    mark_l2_guest_state_dirty(cpu, encoding.value());

    // The guest hypervisor's own store of a low instruction pointer,
    // which is the one writer that is not this VMM's doing. See
    // `low_rip_source`: an application processor being started really
    // is written this way, so this fires legitimately - and a boot where
    // it is the *only* source that fires says the address came from
    // above.
    if ((static_cast<std::uint64_t>(
             arch::x86_64::vmx::vmcs::field::guest_rip) ==
         encoding.value()) &&
        (value < low_rip_threshold)) {
        note_low_guest_rip(
            cpu,
            low_rip_source::written_by_guest,
            this->guest_vmcs12[cpu].read(encoding),
            value,
            0,
            0,
            this->guest_vmcs12[cpu].read(
                arch::x86_64::vmx::vmcs::field::guest_cs_base));
    }

    this->guest_vmcs12[cpu].write(encoding, value);

    vmx_succeed();
    return true;
}

/**
 * Records one use of a VMCS field by the guest hypervisor, for deciding
 * which fields VMCS shadowing should cover.
 *
 * Linear, and that is deliberate: the table is scanned on the hottest
 * path in the VMM, so the entries that matter have to be the ones found
 * first. They are, because a table appended to in order of first use puts
 * the fields a guest hypervisor touches every exit at the front - the
 * scan for a hot field ends in a handful of comparisons, and only a field
 * seen once pays for the whole walk.
 */
void hypervisor::record_vmcs_field_use(bool write, std::uint64_t encoding)
{
    auto encodings = write ? this->vmcs_field_write_encoding
                           : this->vmcs_field_read_encoding;
    auto counts =
        write ? this->vmcs_field_write_count : this->vmcs_field_read_count;

    // Encoding zero is a real one - VPID - so an empty slot cannot be
    // spelled as a zero encoding. It is spelled as a zero *count*, which
    // no used slot ever has.
    for (std::size_t slot{}; slot < vmcs_field_use_capacity; ++slot) {
        if (0 == counts[slot]) {
            encodings[slot] = encoding;
            counts[slot] = 1;
            return;
        }

        if (encoding == encodings[slot]) {
            counts[slot] = counts[slot] + 1;
            return;
        }
    }

    this->vmcs_field_use_overflow = this->vmcs_field_use_overflow + 1;
}

bool hypervisor::on_guest_invept(std::size_t cpu,
                                 arch::x86_64::context & context)
{
    // The type is a register operand and the descriptor a memory one, the
    // same way round as the instruction this VMM executes itself. SDM
    // 33.3, INVEPT: "INVEPT_TYPE := value of register operand", with the
    // register named by Reg2 of the instruction-information field.
    auto operand =
        decode_operand(this->vmcs.vm_exit_instruction_information());
    auto type = guest_register(context, operand.register_2);

    constexpr std::uint64_t single_context = 1;
    constexpr std::uint64_t all_context = 2;

    // The supported types are the ones IA32_VMX_EPT_VPID_CAP reports, as
    // the same operation section says, and this VMM reports both.
    if ((single_context != type) && (all_context != type)) {
        vmx_fail(cpu,
                 instruction_error::invalid_operand_to_invept_invvpid);
        return true;
    }

    // This is the announcement the translation cache waits for: the
    // guest hypervisor edited its extended page tables, so anything
    // remembered from walking them may now name the wrong page. Emptied
    // for both types rather than only for all-context - a single-context
    // invalidation names an EPTP, and matching it against the cache's
    // would save a flush the guest asks for a few thousand times a boot
    // while adding a way to keep a stale entry. See
    // `l2_translate_cache_tag`: a stale hit does not fault, it answers.
    forget_l2_translations(cpu);

    // The descriptor is read even for the all-context type, because the
    // memory operand is still decoded and a bad address is still a fault.
    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
        // Counted, because the caller turns this false into a #UD and
        // a guest hypervisor bugchecks on that. See
        // `note_vmx_operand_failure`.
        note_vmx_operand_failure(cpu,
                                 false,
                                 this->vmcs.guest_rip(),
                                 0,
                                 static_cast<std::uint64_t>(
                                     linear.error().code()));
        return false;
    }

    struct alignas(0x10) descriptor
    {
        std::uint64_t eptp{};
        std::uint64_t reserved{};
    } operand_value{};

    auto read = read_guest_linear(
        *linear,
        std::span(reinterpret_cast<std::byte *>(&operand_value),
                  sizeof(operand_value)));
    if (!read) {
        // Counted for the same reason, and separately: this half is a
        // memory access that did not work, which is not "no such
        // instruction". See `note_vmx_operand_failure`.
        note_vmx_operand_failure(cpu,
                                 true,
                                 this->vmcs.guest_rip(),
                                 *linear,
                                 static_cast<std::uint64_t>(
                                     read.error().code()));
        return false;
    }

    // The single-context type discards only the shadow built from the
    // pointer it names, which is what it asks for and is now possible:
    // this processor keeps a shadow per guest EPT pointer, so the others
    // can be left alone.
    //
    // It used to discard all of them, on the grounds that
    // over-invalidation is the safe direction - SDM 31.4.3.2 lets a
    // processor invalidate any cached mapping at any time - and that
    // keeping one per pointer bought nothing while there was only one.
    // Both halves were true and the second has expired. What it cost,
    // measured after the shadows became per pointer: 2,820 rebuilds
    // against 12,056 cache hits, each rebuild a walk of eleven to
    // twenty-four thousand regions, on a guest hypervisor that issues
    // this instruction constantly.
    //
    // Discarding rather than rebuilding, because the next VM entry needs
    // the shadow and nothing between now and then reads it.
    // Refreshing rather than discarding **works and is wrong**, and both
    // halves are measured, so it is switched off here rather than
    // deleted.
    //
    // The idea: either way every mapping composed from these tables has
    // to be composed again, since the descriptor names the pointer and
    // not what changed - so do the walks in root operation, four memory
    // reads each, instead of making the guest fault each one back. On the
    // rig discarding cost 464,815 EPT violations against 15,896 INVEPTs,
    // 58% of every exit this VMM took, for a guest hypervisor changing
    // one page per call.
    //
    // It delivered exactly that. Shadow leaves filled per second-level
    // exit went from 4.5 to 0.01 - 109 fills across 9,455 exits, against
    // 444,292 across 99,345 before - and shadow rebuilds went to two.
    //
    // And then Hyper-V executed VMXOFF after those 9,455 exits and the
    // machine reset, eight times in one boot where a working build loads
    // the loader twice. A guest hypervisor that stands down has been told
    // something it cannot reconcile, so the refresh is installing a
    // mapping that should have faulted - it recomposes from the guest
    // hypervisor's tables and this VMM's, and one of those answers is
    // conditional on state the composition does not capture. The fault
    // path knows that and deliberately leaves such entries absent, which
    // is the note install_shadow_leaf already carries: "the table holds
    // only what is unconditionally true".
    //
    // **Settled on 2026-08-13, and the answer is a hypercall.** Run again
    // with the newer rings, the machine reset in a loop - six module
    // loads in eighty seconds - and the second-level ring caught what
    // preceded every one of them, which nothing had before:
    //
    //   vmcall rcx=0x1000c from the second-level guest, reflected, and
    //   then vmcall rcx=0x1000c from the guest hypervisor, alternating
    //   until the reset. Every entry identical.
    //
    // `0x000c` with a repeat count of one is
    // `HvCallModifyVtlProtectionMask`
    // - the second virtual trust level changing what the first may do to
    // a page. So the state "the composition does not capture" has a name:
    // it is a protection change in flight.
    //
    // Which gives the ordering that makes eager refresh unsound. It is
    // only safe if a guest hypervisor always modifies its tables and
    // *then* invalidates. Hyper-V does not: it invalidates around a VTL
    // protection change, so a refresh driven by the INVEPT reinstalls
    // from tables that are about to change, the entry is present, no
    // fault ever occurs to pick the change up, and the level asking for
    // the protection asks again for ever.
    //
    // The lazy path is not merely the conservative choice here, it is the
    // correct one, and the cost it pays - about twenty-seven refaults per
    // INVEPT, 38 to 53 per cent of all exits - is the price of not
    // needing to know when the guest hypervisor writes its own tables.
    // Making that cheap means watching those pages, which
    // `watch_guest_page_writes` could do; it does not mean this switch.
    constexpr bool refresh_shadow_on_invept = false;

    // Counted before the branch, so a type that reaches neither arm
    // cannot be mistaken for one that did. See the declarations: which
    // type arrives decides whether the all-context discard is costing
    // anything at all, and no run has ever recorded it.
    //
    // **Recorded now, and the answer is that it costs nothing.** One
    // clean-phase window: `single-context` 4,104, `all-context` **0**.
    // In the settled stall both are zero, because a guest touching no
    // new memory invalidates nothing. So `discard_shadow_ept` - the
    // whole-processor discard that looked like the expensive arm - never
    // executes on this workload, and the shadow cost is entirely the
    // single-context arm.
    //
    // Which also settles a lead that was set aside three times: the
    // rebuilds are **not** the two trust levels evicting each other.
    // Censused live, the four slots held `0x101b1a000` (VTL0) and
    // `0x101b1d000` (VTL1) **simultaneously**, with two slots spare and
    // 19 of 96 tables used - `evictions`, `reclaims`,
    // `refresh_overflows` and `rebuild-stale-generation` all zero.
    // Retention already works. What drives the 4,104 rebuilds is this
    // branch, one for one with the 4,162 single-context INVEPTs in the
    // same window, at about 374 microseconds each - 272 of the 273
    // microseconds `shadow_ept_pointer_for` costs per trust-level round
    // trip.
    if (cpu < max_cpus) {
        if (single_context == type) {
            this->l2_invept_single_context[cpu] =
                this->l2_invept_single_context[cpu] + 1;
        } else {
            this->l2_invept_all_context[cpu] =
                this->l2_invept_all_context[cpu] + 1;
        }
    }

    if (single_context == type) {
        if constexpr (refresh_shadow_on_invept) {
            refresh_shadow_ept_for(cpu,
                                   operand_value.eptp &
                                       (((1ull << 52) - 1) & ~0xfffull));
        } else {
            discard_shadow_ept_for(cpu,
                                   operand_value.eptp &
                                       (((1ull << 52) - 1) & ~0xfffull));
        }
    } else {
        discard_shadow_ept(cpu);
    }

    // Propagated to every processor, when asked for.
    //
    // The discards above touch this processor's slots only. INVEPT is
    // not a broadcast and the instruction's own semantics are therefore
    // satisfied - but our *shadow* of the guest hypervisor's tables is
    // per processor and invisible to the shootdown IPI the guest
    // hypervisor sends to cover the rest. So a permission it has just
    // revoked can still be served by another processor's shadow.
    //
    // The generation is the propagation mechanism this VMM already has
    // for its own edits: bumping it makes every processor discard every
    // stale shadow root at its next entry, through
    // `discard_stale_shadow_ept`. Reused rather than duplicated.
    //
    // See `nested_vmx::invept_all_processors` for why this is the shape
    // of the multiprocessor failure and what it costs.
    if constexpr (nested_vmx::invept_all_processors) {
        this->ept_generation.fetch_add(1, std::memory_order_release);
    }

    vmx_succeed();
    return true;
}

bool hypervisor::on_guest_invvpid(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto operand =
        decode_operand(this->vmcs.vm_exit_instruction_information());
    auto type = guest_register(context, operand.register_2);

    // SDM 33.3, INVVPID, gives four types: 0 individual-address, 1
    // single-context, 2 all-context, 3 single-context retaining globals.
    // All four are reported, so all four are accepted.
    constexpr std::uint64_t highest_type = 3;

    if (type > highest_type) {
        vmx_fail(cpu,
                 instruction_error::invalid_operand_to_invept_invvpid);
        return true;
    }

    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
        // Counted, because the caller turns this false into a #UD and
        // a guest hypervisor bugchecks on that. See
        // `note_vmx_operand_failure`.
        note_vmx_operand_failure(cpu,
                                 false,
                                 this->vmcs.guest_rip(),
                                 0,
                                 static_cast<std::uint64_t>(
                                     linear.error().code()));
        return false;
    }

    struct alignas(0x10) descriptor
    {
        std::uint64_t vpid{};
        std::uint64_t linear_address{};
    } operand_value{};

    auto read = read_guest_linear(
        *linear,
        std::span(reinterpret_cast<std::byte *>(&operand_value),
                  sizeof(operand_value)));
    if (!read) {
        // Counted for the same reason, and separately: this half is a
        // memory access that did not work, which is not "no such
        // instruction". See `note_vmx_operand_failure`.
        note_vmx_operand_failure(cpu,
                                 true,
                                 this->vmcs.guest_rip(),
                                 *linear,
                                 static_cast<std::uint64_t>(
                                     read.error().code()));
        return false;
    }

    // SDM 33.3, INVVPID: "VMfail(Invalid operand to INVEPT/INVVPID)" if
    // the descriptor's VPID is 0000H for any type but all-context.
    constexpr std::uint64_t individual_address = 0;
    constexpr std::uint64_t all_context = 2;

    if ((all_context != type) && (0 == (operand_value.vpid & 0xffff))) {
        vmx_fail(cpu,
                 instruction_error::invalid_operand_to_invept_invvpid);
        return true;
    }

    // And the same section refuses a non-canonical linear address for the
    // individual-address type.
    if (individual_address == type) {
        auto address = operand_value.linear_address;
        auto sign_extended = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(address << 16) >> 16);

        if (address != sign_extended) {
            vmx_fail(cpu,
                     instruction_error::invalid_operand_to_invept_invvpid);
            return true;
        }
    }

    // The VPID in the descriptor is the guest hypervisor's own numbering
    // and names nothing this processor has ever tagged a mapping with: a
    // second-level guest runs under *this* VMM's VPID, which is what
    // `build_vmcs02` writes. So every type is answered the same way, by
    // invalidating that one - which SDM 31.4.3.1 makes cover every PCID
    // and, for combined mappings, every extended page-table root.
    //
    // Over-invalidation again, and permitted for the same reason as
    // above. What it costs is the second-level guest's translations on
    // every INVVPID the guest hypervisor executes, which is the price of
    // not allocating a VPID per second-level guest. BACKLOG.md records the
    // measurement that would justify allocating one.
    nested_transition_flush(cpu);

    vmx_succeed();
    return true;
}

void hypervisor::record_l2_entry_lowest(std::size_t cpu,
                                        std::uint64_t rip)
{
    if (cpu >= max_cpus) {
        return;
    }

    using field = arch::x86_64::vmx::vmcs::field;

    auto & record = this->l2_entry_lowest[cpu];
    auto & shadow = this->guest_vmcs12[cpu];

    record.entries = this->l2_entries[cpu];
    record.rip = rip;

    // vmcs02's, which is what the processor is about to act on. Read
    // out of the VMCS rather than remembered from where it was written,
    // for the reason `record_l2_entry_event` gives one screen up: every
    // other account of a field is a claim about the field.
    record.cs_selector = this->vmcs.guest_cs_selector();
    record.cs_base = this->vmcs.guest_cs_base();
    record.cs_limit = this->vmcs.guest_cs_limit();
    record.cs_access_rights = this->vmcs.guest_cs_access_rights();
    record.cr0 = this->vmcs.guest_cr0();
    record.efer = this->vmcs.guest_ia32_efer();
    record.rflags = this->vmcs.guest_rflags();

    // And vmcs12's, which is a subscript into module memory rather than
    // a VMREAD. The pair is the instrument: equal means vmcs02 carries
    // what the level above asked for, and that reads as clearly as the
    // alternative.
    record.cs_selector12 = shadow.read(field::guest_cs_selector);
    record.cs_base12 = shadow.read(field::guest_cs_base);
    record.cr0_12 = shadow.read(field::guest_cr0);

    constexpr std::uint64_t selector_to_base_shift = 4;

    record.base_is_selector_times_16 =
        (record.cs_base == (record.cs_selector << selector_to_base_shift))
            ? 1
            : 0;

    // Last, so a reader that finds it set finds the rest filled in.
    record.occurred = 1;

    log("cpu {} lowest second-level entry rip {}, cs {} base {}, cr0 {}",
        cpu,
        rip,
        record.cs_selector,
        record.cs_base,
        record.cr0);
    log("cpu {} lowest entry: vmcs12 cs {} base {} cr0 {}, rflags {}",
        cpu,
        record.cs_selector12,
        record.cs_base12,
        record.cr0_12,
        record.rflags);
}

bool hypervisor::on_guest_vmlaunch(std::size_t cpu,
                                   basic_reason reason,
                                   arch::x86_64::context & context)
{
    // Phase timing; see `phase_cycles`. This is the whole of the entry
    // half of a round trip - the exit the guest hypervisor's own
    // VMRESUME takes - and until slot 36 the only parts of it in the
    // table were `build_vmcs02` and the shadow collection. What it
    // covers besides those is the enlightenment probe, the launch-state
    // checks, `capture_context`, and `enter_or_park_l2`.
    auto entry_start = arch::x86_64::rdtsc();
    auto entry_stop = zpp::scope_exit([&] {
        if (cpu < max_cpus) {
            this->phase_cycles[cpu][36] +=
                arch::x86_64::rdtsc() - entry_start;
            this->phase_calls[cpu][36] += 1;
        }
    });

    // The enlightenment first, because it decides whether there *is* a
    // current VMCS by the architecture's reckoning. A guest hypervisor
    // using an enlightened VM entry never executes VMPTRLD - it names
    // the structure through its assist page - so the check below would
    // refuse a perfectly well formed launch. See
    // `load_enlightened_vmcs`, which sets `guest_current_vmcs` from the
    // structure's address so nothing downstream needs to know.
    auto enlightened = load_enlightened_vmcs(cpu);

    // The launch-state checks first, because they are the ones the
    // architecture puts before any consistency check and the ones a
    // first-level hypervisor's own error handling is written around. SDM
    // 33.3, VMLAUNCH/VMRESUME.
    if (!enlightened && (no_current_vmcs == this->guest_current_vmcs[cpu])) {
        note_entry_refusal(cpu, entry_refusal::no_current_vmcs);
        vmx_fail_invalid();
        return true;
    }

    // Everything below reads the cached vmcs12, and the guest hypervisor
    // has had a chance to write the shadowed fields since this VMM last
    // looked - silently, which is the point. So they are collected first.
    // This is the same place KVM does it, in nested_vmx_run.
    //
    // Skipped when the structure was just read: with an enlightened VM
    // entry the guest hypervisor states everything there, and the
    // hardware shadow region it would otherwise have written is not what
    // it used.
    if (!enlightened) {
        copy_shadow_to_vmcs12(cpu);
    }

    auto & shadow = this->guest_vmcs12[cpu];

    if ((basic_reason::vmlaunch == reason) &&
        (vmcs12::launch_state::clear != shadow.state())) {
        note_entry_refusal(cpu, entry_refusal::launch_not_clear);
        vmx_fail(cpu, instruction_error::vmlaunch_with_non_clear_vmcs);
        return true;
    }

    if ((basic_reason::vmresume == reason) &&
        (vmcs12::launch_state::launched != shadow.state())) {
        note_entry_refusal(cpu, entry_refusal::resume_not_launched);
        vmx_fail(cpu, instruction_error::vmresume_with_non_launched_vmcs);
        return true;
    }

    // Where a VM entry the processor refuses comes back to.
    //
    // It has to be captured here and not at the entry, because there is
    // no "here" at the entry: it is executed from a naked stub with the
    // guest's registers already loaded, by restore_context, out of the
    // tail of the exit handler. This is the last point that is still
    // ordinary C++ on the host stack.
    //
    // The flag is in memory rather than in a variable for the same reason
    // vm_launch's is: the second arrival restores every register to what
    // the first left, so only memory can tell the two apart.
    this->nested_entry_failed[cpu].store(false, std::memory_order_relaxed);
    this->nested_entry_error[cpu] = 0;

    arch::x86_64::capture_context(&this->nested_entry_recovery[cpu]);

    // Published *after* the capture, so the slot the entry stubs index is
    // null until the context behind it is real - and they test it. This
    // is where the recovery point's address lives now; it used to be
    // written into CR3-target value 0 of vmcs02, a field the layer below
    // does not model, so every stub that read it back got the
    // second-level guest's register instead. See `nested_entry_slot_field`
    // in `zpp/arch/x86_64/vmx/asm.h`.
    //
    // Idempotent and unconditional rather than once: it is one store, and
    // a "once" flag is one more thing that can be wrong on the path that
    // has no other way of reporting anything.
    if (cpu < arch::x86_64::vmx::nested_entry_recovery_slots) {
        arch::x86_64::vmx::zpp_vmx_nested_entry_recovery[cpu] =
            &this->nested_entry_recovery[cpu];
    }

    if (this->nested_entry_failed[cpu].load(std::memory_order_relaxed)) {
        // Arrived from zpp_vmx_nested_entry_failure, which has already put
        // this VMM's own VMCS back and recorded what the processor said.
        //
        // The error number is the hardware's rather than one invented
        // here, and that is the honest answer: vmcs02's controls come from
        // the guest hypervisor's own and its host state from this VMM's,
        // so a refusal is a refusal of something one of the two asked for.
        // SDM Table 33-1 gives 7 for the controls and 8 for the host-state
        // area, which is exactly the distinction the field carries.
        auto refusal = this->nested_entry_error[cpu];

        log("cpu {} second level entry refused by the processor, "
            "vm-instruction error {}",
            cpu,
            refusal);

        note_entry_refusal(cpu, entry_refusal::control_or_host_state);
        vmx_fail(cpu,
                 (0 != refusal)
                     ? static_cast<instruction_error>(refusal)
                     : instruction_error::entry_invalid_control_field);
        return true;
    }

    if (auto built = build_vmcs02(cpu); !built) {
        // The MSR-load area is the one failure that happens *after* the
        // switch, because SDM 29 step 4 puts the loads after the guest
        // state - and it is the one the architecture answers with an exit
        // rather than with VMfail: SDM 29.4 says the processor "responds
        // to such failures by loading state from the host-state area, as
        // it would for a VM exit". So the guest hypervisor is given that
        // exit, with the reason and the qualification SDM 29.8 defines.
        if (this->nested_msr_load_failed[cpu]) {
            this->nested_msr_load_failed[cpu] = false;

            constexpr std::uint64_t entry_failure = 1ull << 31;

            log("cpu {} second level entry failed loading msr entry {}",
                cpu,
                this->nested_msr_failure_entry[cpu]);

            reflect_l2_exit(
                cpu,
                entry_failure |
                    static_cast<std::uint64_t>(
                        basic_reason::entry_failure_msr_loading),
                this->nested_msr_failure_entry[cpu]);

            this->nested_rip_settled[cpu] = true;
            return true;
        }

        // Everything else fails before the switch, so nothing has moved
        // and the guest hypervisor is simply told its VM entry did not
        // happen, with RIP on the instruction after the VMLAUNCH - which
        // is where SDM 33.3 puts a failure on the controls or on the
        // host-state area.
        log("cpu {} guest {} refused: error {}",
            cpu,
            (basic_reason::vmlaunch == reason) ? "vmlaunch" : "vmresume",
            built.error().code());

        note_entry_refusal(cpu, entry_refusal::control_or_host_state);
        vmx_fail(
            cpu,
            (zpp::error{error::nested_host_state_unsupported}.code() ==
             built.error().code())
                ? instruction_error::entry_invalid_host_state_field
                : instruction_error::entry_invalid_control_field);
        return true;
    }

    // The guest-state area is checked here rather than in build_vmcs02,
    // because SDM 29.3 puts those checks after the VM-execution controls
    // and the host-state area - which is exactly what build_vmcs02 has
    // just passed. The answer is not always "enter": a guest-state area
    // this VMM cannot honour is refused with a VM-entry failure, and a
    // second-level guest waiting for a start-up IPI is held without being
    // entered at all.
    //
    // Either way RIP is settled by the callee and must not be advanced -
    // a reflection has already put the guest hypervisor at its own host
    // RIP, and a retry leaves it on the VMLAUNCH deliberately.
    switch (enter_or_park_l2(cpu)) {
    case l2_entry_outcome::entered:
        break;

    case l2_entry_outcome::reflected:
    case l2_entry_outcome::retry:
        this->nested_rip_settled[cpu] = true;
        return true;
    }

    // From here vmcs02 is current and the tail of the exit handler enters
    // it rather than resuming the guest hypervisor. Its RIP must not be
    // advanced: it still names the VMLAUNCH, which is where it stays until
    // an exit is reflected and vmcs12's host RIP replaces it - and
    // advancing now would write into vmcs02 and move the second-level
    // guest instead.
    this->running_l2[cpu] = true;
    this->nested_rip_settled[cpu] = true;
    this->l2_entries[cpu] = this->l2_entries[cpu] + 1;

    // Which instruction pointer the second-level guest is *entered* at,
    // as a small table of distinct values.
    //
    // This separates the two explanations of the livelock, and nothing
    // else recorded here does. If the guest loops in its own software it
    // is entered just past the VMCALL and goes round a branch further
    // out. If the guest hypervisor resumes it **at** the VMCALL, never
    // having advanced the instruction pointer past it, then it is
    // entered at the hypercall stub itself and executes nothing at all -
    // which is exactly why its registers and its whole sixty-four word
    // stack are byte-identical across thousands of switches, a
    // coincidence a software loop would have to work at.
    //
    // Read from the VMCS rather than from vmcs12, and read here, because
    // this is the point the comment above already establishes: vmcs02 is
    // current and its guest state is loaded, so this is the address the
    // processor is about to execute rather than a value that still has
    // to survive a merge.
    //
    // Distinct values with counts, not a ring: a ring of a hundred
    // thousand identical entries answers nothing and "how many different
    // ones are there" is the whole question.
    if (cpu < max_cpus) {
        auto rip = this->vmcs.guest_rip();

        // And whether that is the address vmcs12 asked for.
        //
        // **The one question the table below cannot answer.** It records
        // which addresses the second-level guest is entered at, and every
        // address in it reads as an ordinary number whether the level
        // above chose it or this VMM composed it. Read against vmcs12 the
        // pair either agrees or it does not, and a disagreement is proof
        // - not evidence - that vmcs02 was entered somewhere nobody asked
        // for. See `l2_entry_rip_agreed`.
        //
        // Free on the path that agrees: `rip` is already in hand for the
        // table, and `vmcs12::read` is a subscript into module memory
        // rather than a VMREAD. The three VMCS reads in the record below
        // are paid once per processor and only when it fires.
        auto rip12 = this->guest_vmcs12[cpu].read(
            arch::x86_64::vmx::vmcs::field::guest_rip);

        if (rip == rip12) {
            this->l2_entry_rip_agreed[cpu] =
                this->l2_entry_rip_agreed[cpu] + 1;
        } else {
            this->l2_entry_rip_differed[cpu] =
                this->l2_entry_rip_differed[cpu] + 1;

            if (0 == this->l2_entry_rip_mismatch[cpu].occurred) {
                auto & record = this->l2_entry_rip_mismatch[cpu];

                record.entries = this->l2_entries[cpu];
                record.rip02 = rip;
                record.rip12 = rip12;
                record.activity12 = this->guest_vmcs12[cpu].read(
                    arch::x86_64::vmx::vmcs::field::guest_activity_state);
                record.cs_selector = this->vmcs.guest_cs_selector();
                record.cs_base = this->vmcs.guest_cs_base();
                record.hot_state_rip = this->hot_state_saved[cpu][0];
                record.hot_state_valid_then =
                    this->hot_state_valid[cpu] ? 1 : 0;
                record.vmcs12_address = this->guest_current_vmcs[cpu];

                // Last, so a reader that finds it set finds the rest
                // filled in.
                record.occurred = 1;

                log("cpu {} second level entered at {} while vmcs12 "
                    "asked for {}, activity {}",
                    cpu,
                    rip,
                    rip12,
                    record.activity12);
                log("cpu {} entered-at mismatch: cs {} base {}, hot rip "
                    "{} valid {}, vmcs12 {}",
                    cpu,
                    record.cs_selector,
                    record.cs_base,
                    record.hot_state_rip,
                    record.hot_state_valid_then,
                    record.vmcs12_address);
            }
        }

        // The smallest address ever entered at, and the flag that makes
        // a zero in it readable. See `l2_entry_rip_lowest`: without the
        // flag, zero-initialised storage says "entered at zero" and
        // "never entered" with the same word.
        auto new_lowest = (0 == this->l2_entry_rip_lowest_seen[cpu]) ||
                          (rip < this->l2_entry_rip_lowest[cpu]);

        if (new_lowest) {
            this->l2_entry_rip_lowest_seen[cpu] = 1;
            this->l2_entry_rip_lowest[cpu] = rip;

            // And the segment that address is an offset into, because
            // the address alone cannot be read. See
            // `l2_entry_lowest_record`: RIP 0 is what a start-up IPI
            // asks for, and whether that is a processor beginning
            // normally or one about to execute the bottom of memory is
            // decided entirely by CS base - which this records from
            // vmcs02, beside what vmcs12 asked for, so the two can
            // disagree.
            //
            // Seven VMCS reads, paid only when the minimum falls. It
            // falls a handful of times in a boot and never rises, so
            // this is not on the entry path in any measurable sense.
            record_l2_entry_lowest(cpu, rip);
        }

        // Cleared every so often, so the table describes a *recent*
        // window rather than the first eight addresses of the boot.
        //
        // Without this it answers nothing at all, and it answered
        // nothing in a way that looked like an answer: the eight slots
        // filled with early-boot addresses within the first moments, and
        // every entry after that fell to the overflow counter - which
        // then read 682,716 and was briefly taken for "the guest is
        // entered at hundreds of thousands of addresses" when it means
        // only "at more than the eight this table happened to catch
        // first". A full fixed table cannot tell two distinct values
        // from a million.
        constexpr std::uint64_t epoch = 1u << 16;

        if (0 == (this->l2_entries[cpu] % epoch)) {
            for (std::size_t i{}; i < l2_entry_rip_slots; ++i) {
                this->l2_entry_rip[cpu][i] = 0;
                this->l2_entry_rip_count[cpu][i] = 0;
            }

            this->l2_entry_rip_other[cpu] = 0;
        }

        for (std::size_t i{}; i < l2_entry_rip_slots; ++i) {
            if (this->l2_entry_rip[cpu][i] == rip) {
                this->l2_entry_rip_count[cpu][i] =
                    this->l2_entry_rip_count[cpu][i] + 1;
                break;
            }

            if (0 == this->l2_entry_rip_count[cpu][i]) {
                this->l2_entry_rip[cpu][i] = rip;
                this->l2_entry_rip_count[cpu][i] = 1;
                break;
            }

            // Full and none matched: entered at more than eight
            // addresses, which is itself the answer.
            if ((l2_entry_rip_slots - 1) == i) {
                this->l2_entry_rip_other[cpu] =
                    this->l2_entry_rip_other[cpu] + 1;
            }
        }
    }

    // Which thread this guest is running, once every few thousand
    // entries. Here rather than on the exit path because the guest state
    // is loaded and vmcs02 is current, which is what the walk needs.
    sample_guest_thread(cpu);

    // The answer to a reference-counter read this VMM reflected, which is
    // readable here and nowhere else. Hyper-V has just loaded its guest's
    // general purpose registers into the physical ones - that is what a
    // VMM does before VMRESUME, and what KVM does in `__vmx_vcpu_run` -
    // so RAX and RDX hold the value about to be given to the second-level
    // guest.
    //
    // Taken after the entry is committed rather than before, so a
    // VMRESUME that is refused does not record a value the second-level
    // guest never saw. The flag is cleared either way: an owed read that
    // is never collected must not attach itself to some later unrelated
    // entry, which is the same failure the requeue switch in the exit
    // handler was withdrawn for.
    if (cpu < max_cpus) {
        if (this->reference_read_pending[cpu]) {
            this->reference_read_pending[cpu] = false;

            auto slot = this->reference_read_count[cpu] %
                        reference_sample_capacity;
            this->reference_read_value[cpu][slot] =
                (context.rax & 0xffffffff) | (context.rdx << 32);
            // The counter **the guest will read**, not the one this
            // VMM reads. They are the same number until
            // `ZPP_TIME_DILATION` moves the offset, and after that they
            // are not - and the page fitted from these pairs is
            // evaluated by the guest against its own counter, so a fit
            // against the host's would drift by exactly the offset and
            // put the guest's two clocks into the disagreement that
            // whole switch exists to avoid.
            //
            // It also makes the fit *exact* rather than merely current:
            // the guest hypervisor derives the reference counter from
            // the same dilated counter, so `reference = a * guest_tsc +
            // b` holds with constant `a` and `b` for the life of the
            // boot, where against the host's counter the relation bends
            // as the two levels' share of the machine changes.
            // Anchored on the guest's counter, not this VMM's - the
            // baseline this feeds is compared against the reference
            // counter the guest derives from its own `rdtsc`. See
            // `l2_time_stamp_counter`.
            auto when = l2_time_stamp_counter(cpu);
            this->reference_read_tsc[cpu][slot] = when;
            this->reference_read_count[cpu] =
                this->reference_read_count[cpu] + 1;

            // The far end of the baseline, kept outside the ring because
            // the ring is 32 entries and the guest reads the counter
            // fifteen times a clock tick - so opposite ends of it span
            // about 5 ms, and each end carries the reflection cost as
            // error. The member existed for this and nothing had ever
            // written it; `publish_reference_tsc_page` fell back to the
            // ring's oldest entry and fitted a slope to five
            // milliseconds. See `reference_minimum_baseline`.
            if (0 == this->reference_first_tsc[cpu]) {
                this->reference_first_tsc[cpu] = when;
                this->reference_first_value[cpu] =
                    this->reference_read_value[cpu][slot];
            }
        }

        // The answer to a reflected `HvCallModifyVtlProtectionMask`,
        // readable here for the same reason and at the same instant as
        // the reference-counter answer above: the guest hypervisor has
        // loaded its guest's registers and RAX holds what it is about to
        // be told. Bits 15:0 are the status, bits 43:32 the reps
        // completed.
        //
        // Cleared either way, so an answer that is never collected
        // cannot attach itself to a later unrelated entry.
        if (this->vtl_protect_answer_pending[cpu]) {
            this->vtl_protect_answer_pending[cpu] = false;

            auto slot = this->vtl_protect_answer_slot[cpu];
            if (slot < vtl_protect_capacity) {
                this->vtl_protect_rax[cpu][slot] = context.rax;
            }

            // And the guest's own stack, now that the answer has
            // landed - but performed in `nested_entry.cpp`, which is
            // where `translate_guest_linear` and `read_guest_memory`
            // live. The host test harness links this translation unit
            // and not that one, so referencing them here breaks the
            // build of the tests rather than of the hypervisor.
            if (cpu < max_cpus) {
                this->vtl_protect_after_pending[cpu] = 1;

                // And which trust level is being entered with it. See
                // `vtl_protect_answer_to_caller`: the call was made by
                // VTL1, so its answer belongs to VTL1.
                auto entering = this->vmcs.read(
                    arch::x86_64::vmx::vmcs::field::guest_cr3);

                this->vtl_protect_answer_last_cr3[cpu] = entering;

                // Where it resumes, counted rather than sampled. See
                // `vtl_protect_answer_rip`.
                auto resume_at = this->vmcs.guest_rip();
                bool placed{};

                for (std::size_t k{}; k < 8; ++k) {
                    if (0 == this->vtl_protect_answer_rip_count[cpu][k]) {
                        this->vtl_protect_answer_rip[cpu][k] = resume_at;
                        this->vtl_protect_answer_rip_count[cpu][k] = 1;
                        placed = true;
                        break;
                    }

                    if (resume_at == this->vtl_protect_answer_rip[cpu][k]) {
                        this->vtl_protect_answer_rip_count[cpu][k] += 1;
                        placed = true;
                        break;
                    }
                }

                if (!placed) {
                    this->vtl_protect_answer_rip_other[cpu] += 1;
                }

                // **Deliberately not armed.** Stepping one instruction
                // after a protection answer wedges the guest at two
                // protection calls instead of 39,275, with and without a
                // one-shot arming discipline - so the monitor trap flag
                // cannot be set on this path at all, and the counters
                // below stay inert. See BACKLOG.md: the answer arrives
                // during the guest hypervisor's own resume, and trapping
                // there is not something it survives.

                if (entering == this->vtl_protect_last_cr3[cpu]) {
                    this->vtl_protect_answer_to_caller[cpu] += 1;
                } else {
                    this->vtl_protect_answer_to_other[cpu] += 1;
                }

                // And trace what the secure kernel executes on **this**
                // entry - the one carrying the answer. The pinned trace
                // was armed on the following `HvCallVtlCall`, which is a
                // different event; this is a plain resume into VTL1 and
                // what runs on it has never been seen.
                //
                // `build_vmcs02` arms the monitor trap flag from
                // `vtl_step_active`, and it runs after this, so setting
                // the flag here traces this entry.
                if constexpr (nested_vmx::step_vtl) {
                    if ((this->vtl_protect_count[cpu] >= 39250) &&
                        (0 == this->vtl_step_pin_taken[cpu]) &&
                        (0 == this->vtl_step_active[cpu])) {
                        this->vtl_step_pin_taken[cpu] = 1;

                        this->vtl_step_count[0] = 0;
                        this->vtl_step_other[0] = 0;
                        this->vtl_step_other_reason[0] = 0;
                        this->vtl_step_at[0] = this->vtl_switches[cpu][0];
                        this->vtl_step_active[cpu] = 1;
                    }
                }
            }

            // And the census. See `vtl_protect_status_seen`: the ring
            // above holds sixteen of thirty-nine thousand calls, so a
            // single failure - the exact shape that would stop the walk -
            // is invisible in it.
            if (cpu < max_cpus) {
                auto status = context.rax & 0xffff;
                auto done = (context.rax >> 32) & 0xfff;

                this->vtl_protect_status_seen[cpu][status & 0xf] += 1;

                if (0 != status) {
                    this->vtl_protect_failures[cpu] += 1;
                    this->vtl_protect_last_failure[cpu] = context.rax;
                }

                auto asked = this->vtl_protect_reps_pending[cpu];

                this->vtl_protect_reps_asked[cpu] += asked;
                this->vtl_protect_reps_done[cpu] += done;

                if ((0 != asked) && (done < asked)) {
                    this->vtl_protect_reps_short[cpu] += 1;
                }
            }
        }

        publish_reference_tsc_page(cpu);
    }

    if (!this->l2_entry_logged[cpu]) {
        this->l2_entry_logged[cpu] = true;
        log("cpu {} entering the second level, rip {} cr3 {}",
            cpu,
            shadow.read(arch::x86_64::vmx::vmcs::field::guest_rip),
            shadow.read(arch::x86_64::vmx::vmcs::field::guest_cr3));
    }

    return true;
}

void hypervisor::on_nested_entry_failure(arch::x86_64::context * recovery)
{
    // Nothing was switched: a refused VM entry is not a VM exit, so vmcs02
    // is still current and its VM-instruction error field is the only
    // account of why. Read it before anything makes another VMCS current.
    // The VPID is read here and nowhere else on this path, because this
    // is one of the two places in the tree that genuinely cannot be told
    // which processor it is on: it is reached from the entry-failure
    // stub, which is entered from assembly with a recovery context and
    // nothing else. Read once and passed on, so `own_vmcs_region_
    // physical` below no longer reads it a second time.
    auto slot = this->vmcs.vpid();
    auto refusal = this->vmcs.read(
        arch::x86_64::vmx::vmcs::field::vm_instruction_error);

    // Back onto this VMM's own VMCS, so everything after the unwind is
    // talking about the guest hypervisor again.
    //
    // A failure here is not recoverable and traps, for the reason
    // vmcs::write gives: the region is this VMM's own and was current a
    // few instructions ago, so a refusal means the state this code
    // believes it is in is not the state the processor is in.
    auto region = own_vmcs_region_physical(slot - 1);
    if ((0 == region) || arch::x86_64::vmx::vmptrld(&region, slot - 1)) {
        __builtin_trap();
    }

    if ((0 != slot) && (slot <= max_cpus)) {
        auto cpu = slot - 1;
        this->running_l2[cpu] = false;
        this->nested_entry_refusals[cpu] =
            this->nested_entry_refusals[cpu] + 1;
        this->nested_entry_error[cpu] = refusal;
        this->nested_entry_failed[cpu].store(true,
                                             std::memory_order_relaxed);
    }

    arch::x86_64::restore_context(recovery);
    std::unreachable();
}

} // namespace zpp::hypervisor

/**
 * The failure stub's landing point, which exists only to reach the
 * singleton. Declared by zpp/arch/x86_64/vmx/asm.h, which the VMX layer
 * uses without knowing what implements it - the same arrangement the host
 * exception entry stubs use.
 */
extern "C" void
zpp_vmx_nested_entry_failure(zpp::arch::x86_64::context * recovery)
{
    zpp::hypervisor::hypervisor::instance().on_nested_entry_failure(
        recovery);
}
