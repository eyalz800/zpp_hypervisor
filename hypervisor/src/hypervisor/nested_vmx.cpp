#include "zpp/hypervisor/nested_vmx.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/hypervisor/hypervisor.h"
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
    auto narrow = [](std::uint64_t value, std::uint64_t supported) {
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
        return narrow(hardware, nested_vmx::supported_pin_based_controls);

    case vmx_msr::processor_based_contorls:
    case vmx_msr::true_processor_based_controls:
        return narrow(hardware, nested_vmx::supported_primary_controls);

    case vmx_msr::processor_based_contorls_2:
        // The secondary controls MSR is not a pair of halves like the
        // others: SDM A.3.3 gives it allowed-1 settings in bits 63:32 and
        // reserves bits 31:0 to zero. narrow handles that unchanged,
        // because an allowed-0 half of zero leaves nothing to put back.
        return narrow(hardware, nested_vmx::supported_secondary_controls);

    case vmx_msr::exit_controls:
    case vmx_msr::true_exit_controls:
        return narrow(hardware, nested_vmx::supported_exit_controls);

    case vmx_msr::entry_controls:
    case vmx_msr::true_entry_controls:
        return narrow(hardware, nested_vmx::supported_entry_controls);

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
        constexpr std::uint64_t vmwrite_to_exit_information = 1ull << 29;
        constexpr std::uint64_t cr3_target_count = 0x1ffull << 16;

        return hardware &
               ~(vmwrite_to_exit_information | cr3_target_count);
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
        // Zero: no extended page tables and no VPIDs for a first-level
        // hypervisor. It agrees with the secondary controls, which offer
        // neither, and it is the single fact that decides whether a real
        // guest hypervisor can run here - see
        // nested_vmx::supported_secondary_controls.
        return 0;

    case vmx_msr::vm_functions:
        // Zero: no VM functions. The secondary control that enables
        // VMFUNC is not offered either, and VMFUNC itself raises #UD with
        // that control clear.
        return 0;

    default:
        // Every index in the range is named above. Reaching here means
        // the range grew without this growing with it, so the hardware's
        // value is the least wrong answer - it is at least self
        // consistent - and the log says it happened.
        return hardware;
    }
}

bool hypervisor::on_nested_vmx_msr_read(std::uint32_t index,
                                        arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        return false;
    }

    auto cpu = this->vmcs.vpid() - 1;
    if (cpu >= max_cpus) {
        return false;
    }

    std::uint64_t value{};

    if (arch::x86_64::msr::ia32_feature_control == index) {
        value = this->guest_feature_control[cpu];
    } else if ((index >= arch::x86_64::vmx::msr::begin) &&
               (index < arch::x86_64::vmx::msr::end)) {
        value = nested_vmx_capability_msr(index);
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

bool hypervisor::on_nested_vmx_msr_write(std::uint32_t index,
                                         arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        return false;
    }

    auto cpu = this->vmcs.vpid() - 1;
    if (cpu >= max_cpus) {
        return false;
    }

    auto value = (context.rax & 0xffffffff) | (context.rdx << 32);

    if (arch::x86_64::msr::ia32_feature_control == index) {
        // Write-once, like the real register: SDM 26.5.1 describes the
        // lock bit as making the MSR read-only until the next reset. A
        // write after the lock is set is a general protection fault
        // rather than a silent no-op, because a hypervisor that finds its
        // write ignored has no way to tell.
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

bool hypervisor::on_vmx_instruction(arch::x86_64::vmx::exit_reason reason,
                                    arch::x86_64::context & context)
{
    if constexpr (!nested_vmx::enabled) {
        // The guest was never told VMX exists, so the instruction is not
        // one it can legitimately have executed. The caller delivers the
        // #UD a processor without VMX would have.
        return false;
    }

    auto & vmcs = this->vmcs;

    auto cpu = vmcs.vpid() - 1;
    if (cpu >= max_cpus) {
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
        inject_general_protection_fault();
        return false;
    }

    switch (basic) {
    case basic_reason::vmxon:
        return on_guest_vmxon(cpu, context);
    case basic_reason::vmxoff:
        return on_guest_vmxoff(cpu, context);
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
        return on_guest_vmlaunch(cpu, context, basic);
    case basic_reason::invept:
    case basic_reason::invvpid:
        // Both are refused with the error the architecture reserves for a
        // bad operand, error 28, because the capability that makes either
        // meaningful is not reported: IA32_VMX_EPT_VPID_CAP reads as zero,
        // and SDM 33.3 makes the supported types of each the contents of
        // that MSR. With no type supported, every type is invalid.
        //
        // Refusing rather than succeeding is the point. An INVEPT that
        // reports success has told a first-level hypervisor its extended
        // page tables were invalidated, and nothing here has any.
        vmx_fail(cpu,
                 context,
                 instruction_error::invalid_operand_to_invept_invvpid);
        return true;
    case basic_reason::vmcall:
        // VMCALL from a guest that has done VMXON is, from its own point
        // of view, VMCALL in VMX root operation - there is no VMM above
        // it that it knows of - and SDM Table 33-1 error 1 is exactly
        // that case. This VMM implements no hypercall interface, which
        // the hypervisor CPUID range already says by answering zero for
        // the interface and feature leaves.
        vmx_fail(
            cpu, context, instruction_error::vmcall_in_vmx_root_operation);
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

void hypervisor::vmx_succeed(arch::x86_64::context & context)
{
    // SDM 33.2, VMsucceed: every arithmetic flag cleared.
    static_cast<void>(context);
    this->vmcs.guest_rflags(this->vmcs.guest_rflags() &
                            ~rflags_arithmetic);
}

void hypervisor::vmx_fail_invalid(arch::x86_64::context & context)
{
    // SDM 33.2, VMfailInvalid: carry set, the rest cleared. This is the
    // failure that carries no error number, because there is no current
    // VMCS to record one in.
    static_cast<void>(context);
    this->vmcs.guest_rflags(
        (this->vmcs.guest_rflags() & ~rflags_arithmetic) | rflags_carry);
}

void hypervisor::vmx_fail_valid(std::size_t cpu,
                                arch::x86_64::context & context,
                                instruction_error error)
{
    // SDM 33.2, VMfailValid: zero set, the rest cleared, and the error
    // number written to the VM-instruction error field of the current
    // VMCS - which is the shadow, not this VMM's own.
    static_cast<void>(context);
    this->vmcs.guest_rflags(
        (this->vmcs.guest_rflags() & ~rflags_arithmetic) | rflags_zero);

    this->guest_vmcs12[cpu].write(
        arch::x86_64::vmx::vmcs::field::vm_instruction_error,
        static_cast<std::uint64_t>(error));
}

void hypervisor::vmx_fail(std::size_t cpu,
                          arch::x86_64::context & context,
                          instruction_error error)
{
    // SDM 33.2, VMfail: "IF VMCS pointer is valid THEN
    // VMfailValid(ErrorNumber); ELSE VMfailInvalid". The error number has
    // nowhere to go without a current VMCS.
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        vmx_fail_invalid(context);
        return;
    }

    vmx_fail_valid(cpu, context, error);
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
        vmx_fail(
            cpu, context, instruction_error::vmxon_in_vmx_root_operation);
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
        vmx_fail_invalid(context);
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
        vmx_fail_invalid(context);
        return true;
    }

    if (vmcs12::revision != revision) {
        vmx_fail_invalid(context);
        return true;
    }

    this->guest_in_vmx_operation[cpu] = true;
    this->guest_vmxon_pointer[cpu] = *pointer;
    this->guest_current_vmcs[cpu] = no_current_vmcs;
    this->guest_vmcs12[cpu].clear();

    log("cpu {} guest vmxon at {}", cpu, *pointer);

    vmx_succeed(context);
    return true;
}

bool hypervisor::on_guest_vmxoff(std::size_t cpu,
                                 arch::x86_64::context & context)
{
    // Anything still current has to reach its own region before the
    // pointer to it is forgotten. SDM 27.11.1 asks for the same thing of
    // real software: "software should execute VMCLEAR for that VMCS
    // before executing the VMXOFF instruction", and the reason given is
    // that a VMCS active when a processor leaves VMX operation "may be
    // corrupted".
    flush_guest_vmcs12(cpu);

    this->guest_in_vmx_operation[cpu] = false;
    this->guest_vmxon_pointer[cpu] = 0;
    this->guest_current_vmcs[cpu] = no_current_vmcs;

    log("cpu {} guest vmxoff", cpu);

    vmx_succeed(context);
    return true;
}

void hypervisor::flush_guest_vmcs12(std::size_t cpu)
{
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        return;
    }

    auto & shadow = this->guest_vmcs12[cpu];

    // Failure is deliberately not reported. There is nothing a caller
    // could do about it - the instruction that provoked the flush has
    // already been decided - and the address was validated when it became
    // current, so a failure here means the guest unmapped its own VMCS.
    static_cast<void>(write_guest_physical(
        this->guest_current_vmcs[cpu],
        std::span(reinterpret_cast<const std::byte *>(&shadow),
                  sizeof(shadow))));
}

bool hypervisor::on_guest_vmclear(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto pointer = read_guest_vmcs_pointer(context);
    if (!pointer) {
        return false;
    }

    if (!vmcs_pointer_valid(*pointer)) {
        vmx_fail(cpu, context, instruction_error::vmclear_invalid_address);
        return true;
    }

    if (*pointer == this->guest_vmxon_pointer[cpu]) {
        vmx_fail(
            cpu, context, instruction_error::vmclear_with_vmxon_pointer);
        return true;
    }

    if (*pointer == this->guest_current_vmcs[cpu]) {
        // The current one: clear the cache and write the cleared state
        // out, then stop it being current. SDM 33.3, VMCLEAR: "IF addr =
        // current-VMCS pointer THEN current-VMCS pointer :=
        // FFFFFFFF_FFFFFFFFH".
        this->guest_vmcs12[cpu].clear();
        flush_guest_vmcs12(cpu);
        this->guest_current_vmcs[cpu] = no_current_vmcs;

        vmx_succeed(context);
        return true;
    }

    // Not current, so its region is the only copy of it. VMCLEAR's whole
    // effect on such a VMCS is to set its launch state to clear, which is
    // why the launch state lives in the region rather than beside the
    // cache: a VMCLEAR of a VMCS this processor has never loaded still has
    // to reach it.
    vmcs12 cleared;
    cleared.clear();

    static_cast<void>(write_guest_physical(
        *pointer,
        std::span(reinterpret_cast<const std::byte *>(&cleared),
                  sizeof(cleared))));

    vmx_succeed(context);
    return true;
}

bool hypervisor::on_guest_vmptrld(std::size_t cpu,
                                  arch::x86_64::context & context)
{
    auto pointer = read_guest_vmcs_pointer(context);
    if (!pointer) {
        return false;
    }

    if (!vmcs_pointer_valid(*pointer)) {
        vmx_fail(cpu, context, instruction_error::vmptrld_invalid_address);
        return true;
    }

    if (*pointer == this->guest_vmxon_pointer[cpu]) {
        vmx_fail(
            cpu, context, instruction_error::vmptrld_with_vmxon_pointer);
        return true;
    }

    // Already current, which the architecture makes a plain success: SDM
    // 33.3, VMPTRLD, does not special-case it, so it re-reads. Skipping
    // the reload would lose whatever a VMCLEAR from another processor put
    // there.
    vmcs12 loaded;
    auto read = read_guest_physical(
        *pointer,
        std::span(reinterpret_cast<std::byte *>(&loaded), sizeof(loaded)));
    if (!read) {
        vmx_fail(cpu, context, instruction_error::vmptrld_invalid_address);
        return true;
    }

    if (vmcs12::revision != loaded.revision_id()) {
        vmx_fail(cpu,
                 context,
                 instruction_error::vmptrld_incorrect_revision_id);
        return true;
    }

    // Only now, once the new one is known good, is the old one written
    // back. Doing it earlier would flush over a region that turned out to
    // be the same one.
    if (*pointer != this->guest_current_vmcs[cpu]) {
        flush_guest_vmcs12(cpu);
    }

    this->guest_vmcs12[cpu] = loaded;
    this->guest_current_vmcs[cpu] = *pointer;

    vmx_succeed(context);
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

    vmx_succeed(context);
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
        vmx_fail_invalid(context);
        return true;
    }

    if (!encoding.valid()) {
        vmx_fail(
            cpu, context, instruction_error::unsupported_vmcs_component);
        return true;
    }

    auto value = this->guest_vmcs12[cpu].read(encoding);

    if (operand.is_register) {
        set_guest_register(context, operand.register_1, value);
        vmx_succeed(context);
        return true;
    }

    // The memory form. The operand's size is decided by the mode, not by
    // the field: SDM 27.11.2 makes the effective operand size "always 32
    // bits outside IA-32e mode ... and 64 bits in 64-bit mode".
    auto linear = vmx_operand_linear_address(context);
    if (!linear) {
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
    if (!written) {
        return false;
    }

    vmx_succeed(context);
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
        vmx_fail_invalid(context);
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

        auto read = read_guest_linear(
            *linear,
            std::span(reinterpret_cast<std::byte *>(&value), size));
        if (!read) {
            return false;
        }
    }

    if (!encoding.valid()) {
        vmx_fail(
            cpu, context, instruction_error::unsupported_vmcs_component);
        return true;
    }

    if (encoding.read_only()) {
        vmx_fail(cpu,
                 context,
                 instruction_error::vmwrite_to_read_only_component);
        return true;
    }

    this->guest_vmcs12[cpu].write(encoding, value);

    vmx_succeed(context);
    return true;
}

bool hypervisor::on_guest_vmlaunch(std::size_t cpu,
                                   arch::x86_64::context & context,
                                   basic_reason reason)
{
    // The launch-state checks first, because they are the ones the
    // architecture puts before any consistency check and the ones a
    // first-level hypervisor's own error handling is written around. SDM
    // 33.3, VMLAUNCH/VMRESUME.
    if (no_current_vmcs == this->guest_current_vmcs[cpu]) {
        vmx_fail_invalid(context);
        return true;
    }

    auto & shadow = this->guest_vmcs12[cpu];

    if ((basic_reason::vmlaunch == reason) &&
        (vmcs12::launch_state::clear != shadow.state())) {
        vmx_fail(
            cpu, context, instruction_error::vmlaunch_with_non_clear_vmcs);
        return true;
    }

    if ((basic_reason::vmresume == reason) &&
        (vmcs12::launch_state::launched != shadow.state())) {
        vmx_fail(cpu,
                 context,
                 instruction_error::vmresume_with_non_launched_vmcs);
        return true;
    }

    // And here is the limit of what this implements.
    //
    // Running the second-level guest needs three things that do not
    // exist: a real VMCS built by merging the shadow's guest state and
    // controls with this VMM's own host state, a decision for every exit
    // that second-level guest takes about whether it is reflected into
    // the shadow or handled here, and a shadow of the first level's
    // extended page tables combined with this VMM's. The third is what
    // makes the other two worth having, and it is the largest of them.
    //
    // So the entry is refused, with the error number the architecture
    // gives for controls a processor will not accept. It is honest in the
    // sense that matters: no first-level hypervisor is told its guest is
    // running when it is not, and RIP lands on the instruction after the
    // VMLAUNCH, which is where SDM 33.3 says a failure on the controls
    // puts it - "failure to pass checks on the VMX controls or on the
    // host-state area passes control to the instruction following the
    // VMLAUNCH or VMRESUME instruction".
    //
    // It is not honest in the sense of naming the real reason, and there
    // is no error number that does. Error 7 is the closest: the controls
    // this VMM can honour genuinely do not include the ones any real
    // guest hypervisor needs, because IA32_VMX_EPT_VPID_CAP reads as zero
    // and every one of them requires extended page tables. A first-level
    // hypervisor that consulted the capability MSRs before writing its
    // controls will have found that out already.
    log("cpu {} guest {} refused: no second level entry",
        cpu,
        (basic_reason::vmlaunch == reason) ? "vmlaunch" : "vmresume");

    vmx_fail(cpu, context, instruction_error::entry_invalid_control_field);
    return true;
}

} // namespace zpp::hypervisor
