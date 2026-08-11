#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/trace.h"
#include <cstddef>
#include <cstdint>

/**
 * Where a VM exit from the second-level guest comes back to.
 *
 * At namespace scope with C linkage, and both of those are forced. A VM
 * exit loads RSP and RIP from the host-state area of the VMCS that ran the
 * guest, so the landing point is entered with no arguments and with every
 * general purpose register still holding what the second-level guest left
 * there - which is the whole point of a VM exit, and is why a real
 * hypervisor's first instruction is a push. Nothing can be passed to it,
 * so what it needs has to be at a fixed address, and the assembly that
 * reaches it can only name an unmangled symbol.
 * @{
 */
extern "C" {
/**
 * `used` is not decoration. Two of these three are named only from inline
 * assembly, which the compiler does not see as a use - so without it the
 * definitions are never emitted and the link fails on symbols the assembly
 * is the only reader of.
 */
[[gnu::used]] inline std::uint64_t zpp_probe_resume_rsp{};
[[gnu::used]] inline std::uint64_t zpp_probe_resume_rip{};
[[gnu::used]] inline std::uint64_t zpp_probe_exit_taken{};
}
/**
 * @}
 */

/**
 * The guest hypervisor's host RIP: where the processor lands when the
 * second-level guest exits.
 *
 * Nothing here may touch the stack before RSP is loaded. The host-state
 * area names a page the probe allocated, so RSP is *valid* on arrival -
 * but it is not the stack the C++ that launched was using, and that one is
 * in zpp_probe_resume_rsp. The jump afterwards lands back inside the
 * launcher, which is a longjmp in everything but name.
 *
 * The flag is written before the jump because after it there is no here to
 * write it in.
 */
extern "C" inline void __attribute__((naked)) zpp_probe_l1_host()
{
    asm volatile(".intel_syntax noprefix\n\t"
                 "mov qword ptr [rip + zpp_probe_exit_taken], 1\n\t"
                 "mov rsp, qword ptr [rip + zpp_probe_resume_rsp]\n\t"
                 "jmp qword ptr [rip + zpp_probe_resume_rip]\n\t"
                 ".att_syntax prefix");
}

/**
 * The whole of the second-level guest: one CPUID, then a halt.
 *
 * CPUID is the instruction to choose. SDM 28.1.2 makes it exit
 * unconditionally in VMX non-root operation - there is no control that
 * turns it off - and the VMM reflects it unconditionally too, as KVM's
 * `nested_vmx_l1_wants_exit` does. So a correct run produces exactly one
 * exit, with reason 10, delivered to the guest hypervisor.
 *
 * The halt is a backstop rather than a step. Reaching it means the exit
 * did not happen, and a halted second-level guest is a hang the harness's
 * timeout catches rather than a wrong answer it reports.
 */
extern "C" inline void __attribute__((naked)) zpp_probe_l2_entry()
{
    asm volatile(".intel_syntax noprefix\n\t"
                 "cpuid\n\t"
                 "1: hlt\n\t"
                 "jmp 1b\n\t"
                 ".att_syntax prefix");
}

/**
 * How many times the injected interrupt's handler has run inside the
 * second-level guest.
 *
 * This is the whole of the measurement `launch`'s injection argument
 * exists for, and it is worth saying why it needs a counter rather than a
 * flag: an entry that injects delivers the event *before* the first
 * instruction at the guest's RIP, so a handler that ran means the
 * injection retired into the guest, and one that did not means it was
 * accepted by every check and then dropped. Those two are
 * indistinguishable from the guest hypervisor's side - SDM 30.2 clears
 * the valid bit of the entry-interruption field on every VM exit, so its
 * own record of the injection is gone by the time it looks - which is
 * exactly why nothing above this layer can measure it.
 */
extern "C" inline volatile std::uint64_t zpp_probe_l2_injections{};

/**
 * The interrupt handler the injected vector lands in, running as the
 * second-level guest.
 *
 * It records the arrival and returns, and returning is the important
 * half: IRET puts the guest back at its own RIP, which is the CPUID
 * above, so the run continues to the exit the harness was already
 * checking for. A handler that halted instead would prove the delivery
 * and lose everything after it.
 *
 * The second-level guest is entered with the *current* IDTR - `launch`
 * copies it out of this processor - so this has to be reachable through
 * whatever interrupt descriptor table is installed when the launch
 * happens. The caller installs one.
 *
 * No error code: the vector injected is an external interrupt, and SDM
 * 27.8.3 lists external interrupts among the event types that push none.
 */
extern "C" inline volatile std::uint64_t zpp_probe_l2_interrupted_rip{};

extern "C" inline void __attribute__((naked)) zpp_probe_l2_interrupt()
{
    asm volatile(".intel_syntax noprefix\n\t"
                 "inc qword ptr [rip + zpp_probe_l2_injections]\n\t"
                 "push rax\n\t"
                 "mov rax, [rsp + 8]\n\t"
                 "mov [rip + zpp_probe_l2_interrupted_rip], rax\n\t"
                 "pop rax\n\t"
                 "iretq\n\t"
                 ".att_syntax prefix");
}

namespace zpp
{
/**
 * A first-level hypervisor's worth of VMX, executed as the guest.
 *
 * **This is the only experiment in the tree that can establish anything
 * about nested VMX without a guest hypervisor and without hardware.**
 * BACKLOG.md's "Not run anywhere" section names it as the first of three
 * and says why: it exercises every VMX instruction the VMM emulates,
 * against the flag conventions the SDM specifies for each, from inside a
 * guest - which is the only place those instructions ever execute, since
 * the default build faults on all of them.
 *
 * It goes all the way to a second-level guest: `launch` below writes a
 * whole VMCS out of the state this processor is running with, enters a
 * guest that differs from its hypervisor in exactly one thing - RIP - and
 * checks that the exit that guest takes comes back here with the reason,
 * the guest RIP and the instruction length the architecture says it
 * should.
 *
 * Runs in the same build as `verify`, behind the same switch, and for the
 * same reason it is behind one: it leaves the processor in VMX operation
 * for the duration and puts CR4.VMXE on. Both are undone before it
 * returns, and a failure that skips the undoing is reported rather than
 * hidden - but a machine that is going to boot an operating system
 * afterwards should not be running this at all.
 */
struct verify_nested
{
    /**
     * Whether this build carries the probe. The same switch as `verify`,
     * because the two are the same experiment: one asks whether the VMM is
     * there, the other asks whether the VMX it offers answers.
     */
    static constexpr bool enabled = ZPP_VERIFY_HYPERVISOR;

    /**
     * The flags the VMX instructions answer in, SDM 33.2.
     *
     * VMsucceed leaves all six arithmetic flags clear, VMfailInvalid sets
     * carry, and VMfailValid sets zero - so carry and zero are the whole
     * of the answer and the other four only say that nothing else was
     * disturbed.
     * @{
     */
    static constexpr std::uint64_t rflags_carry = 1ull << 0;
    static constexpr std::uint64_t rflags_zero = 1ull << 6;
    /**
     * @}
     */

    /**
     * What one VMX instruction did.
     */
    enum class outcome
    {
        succeeded,
        failed_invalid,
        failed_valid,
    };

    /**
     * The flags an instruction left, turned into which of SDM 33.2's three
     * conventions it was.
     */
    static constexpr outcome outcome_of(std::uint64_t flags)
    {
        if (0 != (flags & rflags_carry)) {
            return outcome::failed_invalid;
        }

        if (0 != (flags & rflags_zero)) {
            return outcome::failed_valid;
        }

        return outcome::succeeded;
    }

    static constexpr const char * name_of(outcome result)
    {
        switch (result) {
        case outcome::succeeded:
            return "VMsucceed";
        case outcome::failed_invalid:
            return "VMfailInvalid";
        default:
            return "VMfailValid";
        }
    }

    /**
     * The instructions, each returning the flags it left rather than a
     * decoded answer, so the caller decides what the answer should have
     * been.
     *
     * Spelled with extended assembly and letting the compiler allocate,
     * for the reason `verify::query_cpuid` gives: this is compiled for the
     * Microsoft ABI, where the argument registers are not the ones the
     * System V spelling in the hypervisor's own asm.h assumes.
     * @{
     */
    static std::uint64_t vmxon(std::uint64_t region)
    {
        std::uint64_t flags{};

        asm volatile("vmxon %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "m"(region)
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmxoff()
    {
        std::uint64_t flags{};

        asm volatile("vmxoff\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     :
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmptrld(std::uint64_t region)
    {
        std::uint64_t flags{};

        asm volatile("vmptrld %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "m"(region)
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmclear(std::uint64_t region)
    {
        std::uint64_t flags{};

        asm volatile("vmclear %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "m"(region)
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmptrst(std::uint64_t & into)
    {
        std::uint64_t flags{};

        asm volatile("vmptrst %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags), "=m"(into)
                     :
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmwrite(std::uint64_t field, std::uint64_t value)
    {
        std::uint64_t flags{};

        asm volatile("vmwrite %2, %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "r"(field), "r"(value)
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t vmread(std::uint64_t field, std::uint64_t & into)
    {
        std::uint64_t flags{};
        std::uint64_t value{};

        asm volatile("vmread %2, %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags), "=r"(value)
                     : "r"(field)
                     : "cc", "memory");

        into = value;
        return flags;
    }

    static std::uint64_t invept(std::uint64_t type,
                                const void * descriptor)
    {
        std::uint64_t flags{};

        asm volatile("invept %2, %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "r"(type),
                       "m"(*static_cast<const char *>(descriptor))
                     : "cc", "memory");

        return flags;
    }

    static std::uint64_t invvpid(std::uint64_t type,
                                 const void * descriptor)
    {
        std::uint64_t flags{};

        asm volatile("invvpid %2, %1\n\t"
                     "pushfq\n\t"
                     "pop %0"
                     : "=r"(flags)
                     : "r"(type),
                       "m"(*static_cast<const char *>(descriptor))
                     : "cc", "memory");

        return flags;
    }
    /**
     * @}
     */

    /**
     * Reads the descriptor-table registers and the selectors, which a
     * guest-state area needs and which no MSR carries.
     * @{
     */
    struct descriptor_table
    {
        std::uint16_t limit{};
        std::uint64_t base{};
    } __attribute__((packed));

    static descriptor_table read_gdtr()
    {
        descriptor_table table{};
        asm volatile("sgdt %0" : "=m"(table));
        return table;
    }

    static descriptor_table read_idtr()
    {
        descriptor_table table{};
        asm volatile("sidt %0" : "=m"(table));
        return table;
    }

    static std::uint16_t read_tr()
    {
        std::uint16_t value{};
        asm volatile("str %0" : "=r"(value));
        return value;
    }

    static std::uint16_t read_cs()
    {
        std::uint16_t value{};
        asm volatile("mov %%cs, %0" : "=r"(value));
        return value;
    }

    static std::uint16_t read_ss()
    {
        std::uint16_t value{};
        asm volatile("mov %%ss, %0" : "=r"(value));
        return value;
    }

    static std::uint16_t read_ds()
    {
        std::uint16_t value{};
        asm volatile("mov %%ds, %0" : "=r"(value));
        return value;
    }

    static std::uint16_t read_es()
    {
        std::uint16_t value{};
        asm volatile("mov %%es, %0" : "=r"(value));
        return value;
    }

    static std::uint64_t read_cr0()
    {
        std::uint64_t value{};
        asm volatile("mov %%cr0, %0" : "=r"(value));
        return value;
    }

    static std::uint64_t read_cr3()
    {
        std::uint64_t value{};
        asm volatile("mov %%cr3, %0" : "=r"(value));
        return value;
    }
    /**
     * @}
     */

    /**
     * A control value the capability MSR will accept: everything the low
     * half insists on, and nothing the high half forbids. SDM A.3.1 -
     * "bits 31:0 indicate the allowed 0-settings ... bits 63:32 indicate
     * the allowed 1-settings".
     */
    static constexpr std::uint64_t adjust(std::uint64_t capability,
                                          std::uint64_t wanted)
    {
        return (wanted | (capability & 0xffffffff)) &
               ((capability >> 32) & 0xffffffff);
    }

    static std::uint64_t read_cr4()
    {
        std::uint64_t value{};
        asm volatile("mov %%cr4, %0" : "=r"(value));
        return value;
    }

    static void write_cr4(std::uint64_t value)
    {
        asm volatile("mov %0, %%cr4" : : "r"(value) : "memory");
    }

    static std::uint64_t read_msr(std::uint32_t index)
    {
        std::uint32_t low{};
        std::uint32_t high{};

        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));

        return (static_cast<std::uint64_t>(high) << 32) | low;
    }

    static void write_msr(std::uint32_t index, std::uint64_t value)
    {
        asm volatile("wrmsr"
                     :
                     : "a"(static_cast<std::uint32_t>(value)),
                       "d"(static_cast<std::uint32_t>(value >> 32)),
                       "c"(index)
                     : "memory");
    }

    /**
     * The MSRs and bits this needs, named where they are used rather than
     * pulled in from the hypervisor's headers - this is the *guest* side,
     * and it must not be able to accidentally agree with the VMM by
     * sharing a constant with it. A probe that reads the answer out of the
     * same header the answer was written from tests nothing.
     * @{
     */
    static constexpr std::uint32_t ia32_feature_control = 0x3a;
    static constexpr std::uint32_t ia32_vmx_basic = 0x480;
    static constexpr std::uint32_t ia32_vmx_misc = 0x485;
    static constexpr std::uint32_t ia32_vmx_vmcs_enum = 0x48a;
    static constexpr std::uint32_t ia32_vmx_procbased_ctls2 = 0x48b;
    static constexpr std::uint32_t ia32_vmx_ept_vpid_cap = 0x48c;

    static constexpr std::uint64_t feature_control_lock = 1ull << 0;
    static constexpr std::uint64_t feature_control_vmxon = 1ull << 2;

    static constexpr std::uint64_t cr4_vmxe = 1ull << 13;
    /**
     * @}
     */

    /**
     * A field of each width, so the width handling of VMREAD and VMWRITE
     * is exercised rather than assumed. SDM Appendix B for the encodings.
     * @{
     */
    static constexpr std::uint64_t field_vpid = 0x0000;       // 16-bit.
    static constexpr std::uint64_t field_tsc_offset = 0x2010; // 64-bit.
    static constexpr std::uint64_t field_exception_bitmap =
        0x4004;                                              // 32-bit.
    static constexpr std::uint64_t field_guest_rip = 0x681e; // natural.
    static constexpr std::uint64_t field_exit_reason =
        0x4402; // read-only.
    /**
     * @}
     */

    /**
     * The rest of the encodings a whole VMCS needs, from SDM Appendix B.
     *
     * Written out here rather than taken from the VMM's own
     * `vmcs_fields.h`, for the same reason the expectations are: this is
     * the guest side, and a probe that reaches into the thing it probes
     * for its constants cannot disagree with it.
     * @{
     */
    static constexpr std::uint64_t field_guest_es_selector = 0x0800;
    static constexpr std::uint64_t field_guest_cs_selector = 0x0802;
    static constexpr std::uint64_t field_guest_ss_selector = 0x0804;
    static constexpr std::uint64_t field_guest_ds_selector = 0x0806;
    static constexpr std::uint64_t field_guest_fs_selector = 0x0808;
    static constexpr std::uint64_t field_guest_gs_selector = 0x080a;
    static constexpr std::uint64_t field_guest_ldtr_selector = 0x080c;
    static constexpr std::uint64_t field_guest_tr_selector = 0x080e;
    static constexpr std::uint64_t field_host_es_selector = 0x0c00;
    static constexpr std::uint64_t field_host_cs_selector = 0x0c02;
    static constexpr std::uint64_t field_host_ss_selector = 0x0c04;
    static constexpr std::uint64_t field_host_ds_selector = 0x0c06;
    static constexpr std::uint64_t field_host_fs_selector = 0x0c08;
    static constexpr std::uint64_t field_host_gs_selector = 0x0c0a;
    static constexpr std::uint64_t field_host_tr_selector = 0x0c0c;
    static constexpr std::uint64_t field_ept_pointer = 0x201a;
    static constexpr std::uint64_t field_vmcs_link_pointer = 0x2800;
    static constexpr std::uint64_t field_guest_debugctl = 0x2802;
    static constexpr std::uint64_t field_guest_efer = 0x2806;
    static constexpr std::uint64_t field_pin_controls = 0x4000;
    static constexpr std::uint64_t field_primary_controls = 0x4002;
    static constexpr std::uint64_t field_cr3_target_count = 0x400a;
    static constexpr std::uint64_t field_exit_controls = 0x400c;
    static constexpr std::uint64_t field_exit_msr_store_count = 0x400e;
    static constexpr std::uint64_t field_exit_msr_load_count = 0x4010;
    static constexpr std::uint64_t field_entry_controls = 0x4012;
    static constexpr std::uint64_t field_entry_msr_load_count = 0x4014;
    static constexpr std::uint64_t field_entry_interruption_information =
        0x4016;
    static constexpr std::uint64_t field_secondary_controls = 0x401e;
    static constexpr std::uint64_t field_vm_instruction_error = 0x4400;
    static constexpr std::uint64_t field_exit_instruction_length = 0x440c;
    static constexpr std::uint64_t field_guest_es_limit = 0x4800;
    static constexpr std::uint64_t field_guest_cs_limit = 0x4802;
    static constexpr std::uint64_t field_guest_ss_limit = 0x4804;
    static constexpr std::uint64_t field_guest_ds_limit = 0x4806;
    static constexpr std::uint64_t field_guest_fs_limit = 0x4808;
    static constexpr std::uint64_t field_guest_gs_limit = 0x480a;
    static constexpr std::uint64_t field_guest_ldtr_limit = 0x480c;
    static constexpr std::uint64_t field_guest_tr_limit = 0x480e;
    static constexpr std::uint64_t field_guest_gdtr_limit = 0x4810;
    static constexpr std::uint64_t field_guest_idtr_limit = 0x4812;
    static constexpr std::uint64_t field_guest_es_access = 0x4814;
    static constexpr std::uint64_t field_guest_cs_access = 0x4816;
    static constexpr std::uint64_t field_guest_ss_access = 0x4818;
    static constexpr std::uint64_t field_guest_ds_access = 0x481a;
    static constexpr std::uint64_t field_guest_fs_access = 0x481c;
    static constexpr std::uint64_t field_guest_gs_access = 0x481e;
    static constexpr std::uint64_t field_guest_ldtr_access = 0x4820;
    static constexpr std::uint64_t field_guest_tr_access = 0x4822;
    static constexpr std::uint64_t field_guest_interruptibility = 0x4824;
    static constexpr std::uint64_t field_guest_activity_state = 0x4826;
    static constexpr std::uint64_t field_guest_sysenter_cs = 0x482a;
    static constexpr std::uint64_t field_host_sysenter_cs = 0x4c00;
    static constexpr std::uint64_t field_cr0_guest_host_mask = 0x6000;
    static constexpr std::uint64_t field_cr4_guest_host_mask = 0x6002;
    static constexpr std::uint64_t field_cr0_read_shadow = 0x6004;
    static constexpr std::uint64_t field_cr4_read_shadow = 0x6006;
    static constexpr std::uint64_t field_guest_cr0 = 0x6800;
    static constexpr std::uint64_t field_guest_cr3 = 0x6802;
    static constexpr std::uint64_t field_guest_cr4 = 0x6804;
    static constexpr std::uint64_t field_guest_es_base = 0x6806;
    static constexpr std::uint64_t field_guest_cs_base = 0x6808;
    static constexpr std::uint64_t field_guest_ss_base = 0x680a;
    static constexpr std::uint64_t field_guest_ds_base = 0x680c;
    static constexpr std::uint64_t field_guest_fs_base = 0x680e;
    static constexpr std::uint64_t field_guest_gs_base = 0x6810;
    static constexpr std::uint64_t field_guest_ldtr_base = 0x6812;
    static constexpr std::uint64_t field_guest_tr_base = 0x6814;
    static constexpr std::uint64_t field_guest_gdtr_base = 0x6816;
    static constexpr std::uint64_t field_guest_idtr_base = 0x6818;
    static constexpr std::uint64_t field_guest_dr7 = 0x681a;
    static constexpr std::uint64_t field_guest_rsp = 0x681c;
    static constexpr std::uint64_t field_guest_rflags = 0x6820;
    static constexpr std::uint64_t field_guest_pending_debug = 0x6822;
    static constexpr std::uint64_t field_guest_sysenter_esp = 0x6824;
    static constexpr std::uint64_t field_guest_sysenter_eip = 0x6826;
    static constexpr std::uint64_t field_host_cr0 = 0x6c00;
    static constexpr std::uint64_t field_host_cr3 = 0x6c02;
    static constexpr std::uint64_t field_host_cr4 = 0x6c04;
    static constexpr std::uint64_t field_host_fs_base = 0x6c06;
    static constexpr std::uint64_t field_host_gs_base = 0x6c08;
    static constexpr std::uint64_t field_host_tr_base = 0x6c0a;
    static constexpr std::uint64_t field_host_gdtr_base = 0x6c0c;
    static constexpr std::uint64_t field_host_idtr_base = 0x6c0e;
    static constexpr std::uint64_t field_host_sysenter_esp = 0x6c10;
    static constexpr std::uint64_t field_host_sysenter_eip = 0x6c12;
    static constexpr std::uint64_t field_host_rsp = 0x6c14;
    static constexpr std::uint64_t field_host_rip = 0x6c16;
    /**
     * @}
     */

    /**
     * The TRUE capability MSRs, which IA32_VMX_BASIC bit 55 says exist and
     * which the VMM reports set, and IA32_EFER, which a 64-bit guest-state
     * area has to carry.
     * @{
     */
    static constexpr std::uint32_t ia32_vmx_true_pinbased_ctls = 0x48d;
    static constexpr std::uint32_t ia32_vmx_true_procbased_ctls = 0x48e;
    static constexpr std::uint32_t ia32_vmx_true_exit_ctls = 0x48f;
    static constexpr std::uint32_t ia32_vmx_true_entry_ctls = 0x490;
    static constexpr std::uint32_t ia32_efer = 0xc0000080;
    /**
     * @}
     */

    /**
     * Writes a whole VMCS out of the state this processor is running with,
     * launches a second-level guest into one CPUID, and checks that the
     * exit comes back here with the reason and the state the architecture
     * says it should.
     *
     * The second-level guest runs with *this* processor's own paging, GDT
     * and control registers - the same CR3, the same flat segments - and
     * differs from its hypervisor in exactly one thing, RIP. That is the
     * smallest guest that can exist, and it is deliberate: anything it
     * gets wrong is the VMM's, because nothing about the guest itself is
     * novel.
     *
     * Extended page tables are not enabled in it. Without them the
     * second-level guest's physical addresses are the first level's, which
     * is what the VMM's own identity map already translates - so this
     * exercises the entry and the reflection without also depending on the
     * shadow page-table builder, and a failure has one place to be rather
     * than two. Turning them on is the next experiment.
     */
    template <typename Line, typename Say, typename Step>
    static bool launch(EFI_SYSTEM_TABLE * system_table,
                       std::uint64_t vmcs_region,
                       bool with_ept,
                       Line && line,
                       Say && say,
                       Step && step,
                       std::uint64_t injection = 0)
    {
        // A clear launch state each time, so both runs can use VMLAUNCH.
        // SDM 29.1: VMLAUNCH requires clear and VMRESUME requires
        // launched, and the first run leaves it launched - so without this
        // the second would be answered with error 4 and never enter.
        if ((outcome::succeeded != outcome_of(vmclear(vmcs_region))) ||
            (outcome::succeeded != outcome_of(vmptrld(vmcs_region)))) {
            line("zpp: nested FAIL could not re-clear the vmcs\r\n");
            return false;
        }

        // A stack for the guest hypervisor's own host state to land on.
        // Never actually used for anything - `zpp_probe_l1_host` moves off
        // it immediately - but VM exit loads RSP from the field whatever
        // the landing code does with it, so it has to name real memory.
        EFI_PHYSICAL_ADDRESS host_stack = 0xffffffff;
        auto status = system_table->BootServices->AllocatePages(
            AllocateMaxAddress, EfiBootServicesData, 1, &host_stack);

        if (EFI_ERROR(status)) {
            line("zpp: nested FAIL could not allocate a host stack\r\n");
            return false;
        }

        EFI_PHYSICAL_ADDRESS ept_pages = 0xffffffff;

        // Both allocations, given back on every path out - there are
        // four, and the second run of this needs the pages the first
        // returned.
        auto release = [&] {
            system_table->BootServices->FreePages(host_stack, 1);
            if (0xffffffff != ept_pages) {
                system_table->BootServices->FreePages(ept_pages, 2);
            }
        };

        auto gdtr = read_gdtr();
        auto idtr = read_idtr();

        auto cs = read_cs();
        auto ss = read_ss();
        auto ds = read_ds();
        auto es = read_es();
        auto tr = read_tr();

        auto cr0 = read_cr0();
        auto cr3 = read_cr3();
        auto cr4 = read_cr4();
        auto efer = read_msr(ia32_efer);

        auto ok = true;

        auto write = [&](std::uint64_t field, std::uint64_t value) {
            if (outcome::succeeded != outcome_of(vmwrite(field, value))) {
                ok = false;
            }
        };

        // The controls, each put through its capability MSR rather than
        // written as wanted - a control the MSR forbids fails the entry
        // with error 7, and a control it insists on and this leaves clear
        // fails it the same way. The TRUE MSRs, because IA32_VMX_BASIC
        // bit 55 says they exist.
        write(field_pin_controls,
              adjust(read_msr(ia32_vmx_true_pinbased_ctls), 0));

        // Extended page tables, when this run wants them: the secondary
        // controls have to be activated to hold the bit, and the bit needs
        // a real table under it.
        constexpr std::uint64_t primary_secondary_controls = 1ull << 31;
        constexpr std::uint64_t secondary_enable_ept = 1ull << 1;

        if (with_ept) {
            if (EFI_ERROR(system_table->BootServices->AllocatePages(
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    2,
                    &ept_pages))) {
                line("zpp: nested FAIL could not allocate ept12\r\n");
                system_table->BootServices->FreePages(host_stack, 1);
                return false;
            }

            // Four gigabytes, identity mapped, as one page-map level-4
            // entry over four one-gigabyte leaves. That is the whole of
            // what the second-level guest can reach, and it is enough: its
            // code, its stack and the page tables it walks are all inside
            // the firmware's own low memory.
            //
            // Entry layout from SDM Tables 31-1 through 31-5: bits 2:0 are
            // read, write and execute; bits 5:3 of a *leaf* are the memory
            // type, and are reserved in an entry that references another
            // table; bit 7 says a page-directory-pointer entry maps a
            // gigabyte rather than referencing a directory.
            constexpr std::uint64_t entry_read_write_execute = 0x7;
            constexpr std::uint64_t entry_large = 1ull << 7;
            constexpr std::uint64_t entry_write_back = 6ull << 3;
            constexpr std::uint64_t gigabyte = 1ull << 30;

            auto * pml4 = reinterpret_cast<std::uint64_t *>(ept_pages);
            auto * pdpt =
                reinterpret_cast<std::uint64_t *>(ept_pages + 0x1000);

            for (std::size_t i{}; i < 512; ++i) {
                pml4[i] = 0;
                pdpt[i] = 0;
            }

            pml4[0] = (ept_pages + 0x1000) | entry_read_write_execute;

            for (std::size_t i{}; i < 4; ++i) {
                pdpt[i] = (i * gigabyte) | entry_read_write_execute |
                          entry_large | entry_write_back;
            }

            // The pointer itself: bits 2:0 the memory type, bits 5:3 the
            // page-walk length minus one, and the address above. SDM
            // Table 25-9.
            constexpr std::uint64_t eptp_write_back = 6;
            constexpr std::uint64_t eptp_walk_length_4 = 3ull << 3;

            write(field_ept_pointer,
                  ept_pages | eptp_write_back | eptp_walk_length_4);
            write(field_secondary_controls,
                  adjust(read_msr(ia32_vmx_procbased_ctls2),
                         secondary_enable_ept));
        }

        write(field_primary_controls,
              adjust(read_msr(ia32_vmx_true_procbased_ctls),
                     with_ept ? primary_secondary_controls : 0));

        // Host address-space size is the one control this must set. The
        // guest hypervisor is 64-bit, and a VM exit that did not say so
        // would put it back in compatibility mode.
        constexpr std::uint64_t exit_host_address_space_size = 1ull << 9;
        constexpr std::uint64_t entry_ia32e_mode_guest = 1ull << 9;

        write(field_exit_controls,
              adjust(read_msr(ia32_vmx_true_exit_ctls),
                     exit_host_address_space_size));
        write(field_entry_controls,
              adjust(read_msr(ia32_vmx_true_entry_ctls),
                     entry_ia32e_mode_guest));

        write(field_exception_bitmap, 0);
        write(field_cr3_target_count, 0);
        write(field_entry_msr_load_count, 0);
        write(field_exit_msr_load_count, 0);
        write(field_exit_msr_store_count, 0);
        // The event the guest hypervisor asks the processor to deliver
        // to its guest on this entry, and normally none.
        //
        // SDM 29.4 (.references/sdm.txt:203155): "If the VM entry is
        // injecting, the logical processor is in the active state after
        // VM entry ... the contents of the activity-state field do not
        // determine the activity state after VM entry." The delivery
        // happens before the first instruction at the guest's RIP, so a
        // handler that runs is proof the injection retired - which is the
        // one thing about this path that cannot be established from
        // outside the guest.
        write(field_entry_interruption_information, injection);
        write(field_vmcs_link_pointer, ~std::uint64_t{});

        write(field_cr0_guest_host_mask, 0);
        write(field_cr4_guest_host_mask, 0);
        write(field_cr0_read_shadow, cr0);
        write(field_cr4_read_shadow, cr4);

        // The guest-state area: this processor, with a different RIP.
        //
        // The access rights are the architecture's encodings rather than
        // anything read back, because a descriptor's bytes are not what
        // the VMCS field holds - SDM Table 25-2 packs type, S, DPL, P, L,
        // D/B and G into bits 15:0 with bit 16 as "unusable". A 64-bit
        // code segment is 0xa09b, a flat data segment 0xc093, a busy
        // 64-bit task-state segment 0x008b, and an unusable segment is
        // bit 16 alone.
        constexpr std::uint64_t code_access = 0xa09b;
        constexpr std::uint64_t data_access = 0xc093;
        constexpr std::uint64_t task_access = 0x008b;
        constexpr std::uint64_t unusable_access = 0x10000;
        constexpr std::uint64_t flat_limit = 0xffffffff;

        write(field_guest_cs_selector, cs);
        write(field_guest_cs_base, 0);
        write(field_guest_cs_limit, flat_limit);
        write(field_guest_cs_access, code_access);

        struct
        {
            std::uint64_t selector_field;
            std::uint64_t base_field;
            std::uint64_t limit_field;
            std::uint64_t access_field;
            std::uint16_t selector;
        } data_segments[] = {
            {field_guest_ss_selector,
             field_guest_ss_base,
             field_guest_ss_limit,
             field_guest_ss_access,
             ss},
            {field_guest_ds_selector,
             field_guest_ds_base,
             field_guest_ds_limit,
             field_guest_ds_access,
             ds},
            {field_guest_es_selector,
             field_guest_es_base,
             field_guest_es_limit,
             field_guest_es_access,
             es},
            {field_guest_fs_selector,
             field_guest_fs_base,
             field_guest_fs_limit,
             field_guest_fs_access,
             0},
            {field_guest_gs_selector,
             field_guest_gs_base,
             field_guest_gs_limit,
             field_guest_gs_access,
             0},
        };

        for (const auto & one : data_segments) {
            write(one.selector_field, one.selector);
            write(one.base_field, 0);
            write(one.limit_field, flat_limit);
            write(one.access_field,
                  (0 == one.selector) ? unusable_access : data_access);
        }

        write(field_guest_tr_selector, tr);
        write(field_guest_tr_base, 0);
        write(field_guest_tr_limit, 0x67);
        write(field_guest_tr_access, task_access);

        write(field_guest_ldtr_selector, 0);
        write(field_guest_ldtr_base, 0);
        write(field_guest_ldtr_limit, 0);
        write(field_guest_ldtr_access, unusable_access);

        write(field_guest_gdtr_base, gdtr.base);
        write(field_guest_gdtr_limit, gdtr.limit);
        write(field_guest_idtr_base, idtr.base);
        write(field_guest_idtr_limit, idtr.limit);

        write(field_guest_cr0, cr0);
        write(field_guest_cr3, cr3);
        write(field_guest_cr4, cr4);
        write(field_guest_efer, efer);
        write(field_guest_dr7, 0x400);
        write(field_guest_debugctl, 0);
        write(field_guest_activity_state, 0);
        write(field_guest_interruptibility, 0);
        write(field_guest_pending_debug, 0);
        write(field_guest_sysenter_cs, 0);
        write(field_guest_sysenter_esp, 0);
        write(field_guest_sysenter_eip, 0);

        // Bit 1 is reserved and must be 1; everything else stays clear,
        // and interrupts stay off because there is nothing here to take
        // one.
        write(field_guest_rflags, 0x2);
        write(field_guest_rsp, host_stack + 0x800);
        write(field_guest_rip,
              reinterpret_cast<std::uint64_t>(&zpp_probe_l2_entry));

        // The guest hypervisor's own host state, which is where a VM exit
        // puts this processor back.
        write(field_host_cs_selector, cs);
        write(field_host_ss_selector, ss);
        write(field_host_ds_selector, ds);
        write(field_host_es_selector, es);
        write(field_host_fs_selector, 0);
        write(field_host_gs_selector, 0);
        write(field_host_tr_selector, tr);
        write(field_host_cr0, cr0);
        write(field_host_cr3, cr3);
        write(field_host_cr4, cr4);
        write(field_host_fs_base, 0);
        write(field_host_gs_base, 0);
        write(field_host_tr_base, 0);
        write(field_host_gdtr_base, gdtr.base);
        write(field_host_idtr_base, idtr.base);
        write(field_host_sysenter_cs, 0);
        write(field_host_sysenter_esp, 0);
        write(field_host_sysenter_eip, 0);
        write(field_host_rsp, host_stack + 0xf00);
        write(field_host_rip,
              reinterpret_cast<std::uint64_t>(&zpp_probe_l1_host));

        if (!ok) {
            line("zpp: nested FAIL a vmwrite building the vmcs failed"
                 "\r\n");
            release();
            return false;
        }

        line(with_ept ? "zpp: nested vmcs12 written with ept, launching"
                        "\r\n"
                      : "zpp: nested vmcs12 written, launching\r\n");

        // The launch, and the two ways back from it.
        //
        // A VMLAUNCH that the processor refuses returns to the instruction
        // after it, with the flags saying why. One that succeeds does not
        // return at all - the next thing this processor does in the guest
        // hypervisor's world is arrive at its host RIP, which is
        // `zpp_probe_l1_host`, which jumps back to the same label. So both
        // paths land on `1:` and the flag says which happened.
        //
        // Every register is clobbered because one of the two paths ran a
        // guest in between.
        zpp_probe_exit_taken = 0;

        std::uint64_t flags{};

        // AT&T syntax for this one block, where the two Intel-syntax
        // stubs above are Intel. Not a style lapse: a forward reference to
        // a numeric local label inside a RIP-relative bracket -
        // `lea rax, [rip + 1f]` - is not something the Intel-syntax parser
        // accepts, and the label is the whole trick here.
        asm volatile("lea 1f(%%rip), %%rax\n\t"
                     "mov %%rax, zpp_probe_resume_rip(%%rip)\n\t"
                     "mov %%rsp, zpp_probe_resume_rsp(%%rip)\n\t"
                     "vmlaunch\n\t"
                     "1:\n\t"
                     "pushfq\n\t"
                     "pop %0\n\t"
                     : "=r"(flags)
                     :
                     : "rax",
                       "rbx",
                       "rcx",
                       "rdx",
                       "rsi",
                       "rdi",
                       "r8",
                       "r9",
                       "r10",
                       "r11",
                       "r12",
                       "r13",
                       "r14",
                       "r15",
                       "cc",
                       "memory");

        if (0 == zpp_probe_exit_taken) {
            step("vmlaunch", outcome_of(flags), outcome::succeeded);

            std::uint64_t error{};
            vmread(field_vm_instruction_error, error);
            say("vm-instruction error", error);

            line("zpp: nested FAIL the second level never ran\r\n");
            release();
            return false;
        }

        line("zpp: nested the second level ran and exited\r\n");

        // What the guest hypervisor is told about the exit. The reason has
        // to be 10, CPUID, with bit 31 clear - a set bit 31 would mean the
        // entry failed after loading guest state, which is a different
        // answer and a wrong one here.
        std::uint64_t reason{};
        std::uint64_t exit_rip{};
        std::uint64_t length{};

        auto read_ok = outcome::succeeded ==
                       outcome_of(vmread(field_exit_reason, reason));
        read_ok &= outcome::succeeded ==
                   outcome_of(vmread(field_guest_rip, exit_rip));
        read_ok &=
            outcome::succeeded ==
            outcome_of(vmread(field_exit_instruction_length, length));

        say("exit reason", reason);
        say("guest rip at exit", exit_rip);
        say("exit instruction length", length);

        constexpr std::uint64_t exit_reason_cpuid = 10;

        auto entered =
            reinterpret_cast<std::uint64_t>(&zpp_probe_l2_entry);

        if (!read_ok) {
            line("zpp: nested FAIL could not read the exit fields\r\n");
            ok = false;
        }

        if (exit_reason_cpuid != reason) {
            line("zpp: nested FAIL exit reason was not cpuid\r\n");
            ok = false;
        }

        // RIP is saved at the *faulting* instruction, so it is where the
        // second-level guest started - the CPUID is the first instruction
        // there. SDM 30.3: the guest RIP saved is "the value that would
        // have been saved" for the instruction that caused the exit.
        if (entered != exit_rip) {
            line("zpp: nested FAIL guest rip is not where L2 started"
                 "\r\n");
            ok = false;
        }

        // CPUID is two bytes, and the exit-information field says so.
        if (2 != length) {
            line("zpp: nested FAIL instruction length is not two\r\n");
            ok = false;
        }

        release();
        return ok;
    }

    /**
     * Runs the probe and reports every step, returning whether all of it
     * answered as the SDM says it should.
     *
     * Everything it prints goes to the serial port and not to the
     * firmware console; see the reporter inside.
     */
    static bool present(EFI_SYSTEM_TABLE * system_table)
    {
        constexpr std::size_t line_capacity = trace::line_capacity;

        // Serial only, and deliberately not the firmware console.
        //
        // This prints upwards of forty lines, and the console is the slow
        // one: OVMF's ConOut goes to the video text buffer *and* to the
        // serial port, so a line written to both arrives twice on serial
        // and costs a screen scroll. Under an emulator that is the
        // difference between a probe that finishes inside the harness's
        // timeout and one that does not - measured, on the first run of
        // this, which was still printing when the harness killed it.
        //
        // The verdict goes to both, from the caller, because that is the
        // one line a person watching a screen needs.
        auto report = [&](const char * text) { trace::raw(text); };
        auto line = [&](const char * text) { report(text); };

        auto say = [&](const char * text, std::uint64_t value) {
            char buffer[line_capacity]{};
            auto end = trace::append_text(buffer, "zpp: nested ");
            end = trace::append_text(end, text);
            end = trace::append_text(end, " ");
            end = trace::append_hex(end, value, 16);
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report(buffer);
        };

        auto step =
            [&](const char * text, outcome got, outcome want) -> bool {
            char buffer[line_capacity]{};
            auto end = trace::append_text(buffer, "zpp: nested ");
            end = trace::append_text(end, text);
            end = trace::append_text(end, " -> ");
            end = trace::append_text(end, name_of(got));
            if (got != want) {
                end = trace::append_text(end, "  EXPECTED ");
                end = trace::append_text(end, name_of(want));
            }
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report(buffer);
            return got == want;
        };

        // Whether there is anything to probe at all. With the VMM's switch
        // off the guest is told there is no VMX, and the correct behaviour
        // here is to say so and pass - the probe is not a demand that the
        // feature be present, it is a check that it answers when it is.
        std::uint32_t leaf_1[4]{};
        asm volatile("cpuid"
                     : "=a"(leaf_1[0]),
                       "=b"(leaf_1[1]),
                       "=c"(leaf_1[2]),
                       "=d"(leaf_1[3])
                     : "a"(1u), "c"(0u));

        constexpr std::uint32_t cpuid_vmx = 1u << 5;

        if (0 == (leaf_1[2] & cpuid_vmx)) {
            line("zpp: nested vmx not offered by CPUID, nothing to probe"
                 "\r\n");
            return true;
        }

        line("zpp: nested vmx probe begins\r\n");

        // The capability MSRs, reported before anything is attempted,
        // because a refusal below is only interpretable against what the
        // machine said it could do.
        auto basic = read_msr(ia32_vmx_basic);
        say("IA32_VMX_BASIC", basic);
        say("IA32_VMX_MISC", read_msr(ia32_vmx_misc));
        say("IA32_VMX_VMCS_ENUM", read_msr(ia32_vmx_vmcs_enum));
        say("IA32_VMX_PROCBASED_CTLS2",
            read_msr(ia32_vmx_procbased_ctls2));
        say("IA32_VMX_EPT_VPID_CAP", read_msr(ia32_vmx_ept_vpid_cap));

        // IA32_FEATURE_CONTROL has to permit VMXON outside SMX, and the
        // guest's copy is write-once exactly as the real register is - so
        // this either finds it already set or sets it, and a second write
        // afterwards is expected to fault rather than to take.
        auto feature = read_msr(ia32_feature_control);
        say("IA32_FEATURE_CONTROL", feature);

        if (0 == (feature & feature_control_lock)) {
            write_msr(ia32_feature_control,
                      feature | feature_control_lock |
                          feature_control_vmxon);
            feature = read_msr(ia32_feature_control);
            say("IA32_FEATURE_CONTROL after write", feature);
        }

        if (0 == (feature & feature_control_vmxon)) {
            line("zpp: nested FAIL feature control forbids vmxon\r\n");
            return false;
        }

        // Two pages, both below four gigabytes because a VMCS pointer is
        // checked against the processor's physical-address width and this
        // is simpler than reading it.
        EFI_PHYSICAL_ADDRESS pages = 0xffffffff;
        auto status = system_table->BootServices->AllocatePages(
            AllocateMaxAddress, EfiBootServicesData, 2, &pages);

        if (EFI_ERROR(status)) {
            line("zpp: nested FAIL could not allocate probe pages\r\n");
            return false;
        }

        auto vmxon_region = static_cast<std::uint64_t>(pages);
        auto vmcs_region = vmxon_region + 0x1000;

        auto * vmxon_bytes =
            reinterpret_cast<std::uint8_t *>(vmxon_region);
        auto * vmcs_bytes = reinterpret_cast<std::uint8_t *>(vmcs_region);

        for (std::size_t i{}; i < 0x2000; ++i) {
            vmxon_bytes[i] = 0;
        }

        // The revision identifier the VMM reported, in the first dword of
        // both regions. VMPTRLD checks it there (SDM 33.3), and it is the
        // one field of a VMCS region whose layout the architecture fixes.
        auto revision = static_cast<std::uint32_t>(basic & 0x7fffffff);

        *reinterpret_cast<std::uint32_t *>(vmxon_bytes) = revision;
        *reinterpret_cast<std::uint32_t *>(vmcs_bytes) = revision;

        auto passed = true;

        // CR4.VMXE, which VMXON raises #UD without. The VMM answers CR4
        // reads through a shadow, so this is also a check that the bit
        // reads back as written - with the switch on, a guest that turns
        // VMX on is entitled to see that it did.
        auto cr4 = read_cr4();
        write_cr4(cr4 | cr4_vmxe);
        auto cr4_after = read_cr4();
        say("CR4 after setting VMXE", cr4_after);

        if (0 == (cr4_after & cr4_vmxe)) {
            line("zpp: nested FAIL CR4.VMXE did not read back set\r\n");
            passed = false;
        }

        passed &= step(
            "vmxon", outcome_of(vmxon(vmxon_region)), outcome::succeeded);

        // VMXON again, which SDM 33.3 answers with
        // "VMfail(VMXON executed in VMX root operation)" - and VMfail is
        // not VMfailValid. SDM 33.2 defines it as "IF VMCS pointer is
        // valid THEN VMfailValid(ErrorNumber); ELSE VMfailInvalid", and
        // VMXON leaves the current-VMCS pointer at all ones, so at this
        // point there is none and the answer is VMfailInvalid with no
        // error number anywhere.
        //
        // This probe expected VMfailValid and the VMM disagreed with it.
        // The SDM settled it in the VMM's favour, which is the whole
        // reason the expectations are written here rather than taken from
        // the VMM's own headers: a probe that shares its constants with
        // the thing it is probing cannot disagree with it.
        //
        // It is also the first answer that could only have come from state
        // the VMM is keeping, since a processor with no VMM under it would
        // have faulted on the first VMXON.
        passed &= step("vmxon again",
                       outcome_of(vmxon(vmxon_region)),
                       outcome::failed_invalid);

        // No current VMCS yet, so VMPTRST answers with the architecture's
        // own sentinel rather than with zero.
        std::uint64_t stored = 0;
        passed &= step("vmptrst with no current vmcs",
                       outcome_of(vmptrst(stored)),
                       outcome::succeeded);
        say("vmptrst gave", stored);

        if (~std::uint64_t{} != stored) {
            line("zpp: nested FAIL vmptrst did not give all ones\r\n");
            passed = false;
        }

        // VMREAD before there is a current VMCS is VMfailInvalid, which is
        // the one failure that carries no error number because there is
        // nowhere to record one.
        std::uint64_t read_value{};
        passed &= step("vmread with no current vmcs",
                       outcome_of(vmread(field_guest_rip, read_value)),
                       outcome::failed_invalid);

        passed &= step("vmclear",
                       outcome_of(vmclear(vmcs_region)),
                       outcome::succeeded);
        passed &= step("vmptrld",
                       outcome_of(vmptrld(vmcs_region)),
                       outcome::succeeded);

        passed &= step("vmptrst with a current vmcs",
                       outcome_of(vmptrst(stored)),
                       outcome::succeeded);
        say("vmptrst gave", stored);

        if (vmcs_region != stored) {
            line("zpp: nested FAIL vmptrst did not give the region\r\n");
            passed = false;
        }

        // One field of each width, written and read back. The widths are
        // the point: a shadow that stored every field as 64 bits and gave
        // it back whole would pass a natural-width check and fail these.
        struct
        {
            const char * name;
            std::uint64_t field;
            std::uint64_t written;
            std::uint64_t expected;
        } fields[] = {
            {"vpid (16-bit)", field_vpid, 0x1234abcd, 0xabcd},
            {"exception bitmap (32-bit)",
             field_exception_bitmap,
             0x1234567800000042ull,
             0x00000042},
            {"tsc offset (64-bit)",
             field_tsc_offset,
             0x0123456789abcdefull,
             0x0123456789abcdefull},
            {"guest rip (natural)",
             field_guest_rip,
             0xfedcba9876543210ull,
             0xfedcba9876543210ull},
        };

        for (const auto & one : fields) {
            char buffer[line_capacity]{};

            auto write_flags = outcome_of(vmwrite(one.field, one.written));
            auto end = trace::append_text(buffer, "vmwrite ");
            end = trace::append_text(end, one.name);
            *end = 0;
            passed &= step(buffer, write_flags, outcome::succeeded);

            std::uint64_t got{};
            auto read_flags = outcome_of(vmread(one.field, got));

            end = trace::append_text(buffer, "vmread ");
            end = trace::append_text(end, one.name);
            *end = 0;
            passed &= step(buffer, read_flags, outcome::succeeded);

            say(one.name, got);

            if (got != one.expected) {
                line("zpp: nested FAIL field did not read back as "
                     "written\r\n");
                passed = false;
            }
        }

        // A VM-exit information field is read-only unless IA32_VMX_MISC
        // bit 29 says otherwise, and the VMM reports that bit clear - so
        // this is VMfailValid with error 13.
        passed &= step("vmwrite to a read-only field",
                       outcome_of(vmwrite(field_exit_reason, 0)),
                       outcome::failed_valid);

        // An encoding with a reserved bit set, which Table 27-22 makes
        // structurally invalid rather than merely unimplemented.
        passed &= step("vmread of a reserved encoding",
                       outcome_of(vmread(1ull << 12, read_value)),
                       outcome::failed_valid);

        // The two invalidation instructions, with a type each reports
        // support for and a descriptor that is at least readable. Both are
        // expected to succeed now that the capability MSR offers them; on
        // a build where it does not, both answer VMfailValid instead,
        // which is what the unsupported-type path gives.
        alignas(16) std::uint8_t descriptor[16]{};
        auto ept_vpid = read_msr(ia32_vmx_ept_vpid_cap);

        constexpr std::uint64_t cap_invept = 1ull << 20;
        constexpr std::uint64_t cap_invept_all_context = 1ull << 26;
        constexpr std::uint64_t cap_invvpid = 1ull << 32;
        constexpr std::uint64_t cap_invvpid_all_context = 1ull << 42;

        if ((cap_invept | cap_invept_all_context) ==
            (ept_vpid & (cap_invept | cap_invept_all_context))) {
            passed &= step("invept all-context",
                           outcome_of(invept(2, descriptor)),
                           outcome::succeeded);
        }

        if ((cap_invvpid | cap_invvpid_all_context) ==
            (ept_vpid & (cap_invvpid | cap_invvpid_all_context))) {
            passed &= step("invvpid all-context",
                           outcome_of(invvpid(2, descriptor)),
                           outcome::succeeded);
        }

        // An invalid type, which SDM 33.3 answers with error 28 for both.
        passed &= step("invept with an unsupported type",
                       outcome_of(invept(7, descriptor)),
                       outcome::failed_valid);

        // And now the one that matters: a second-level guest, actually
        // run. Everything above exercises the instruction emulation, which
        // is a shadow VMCS and some flag conventions; this exercises the
        // VMCS the VMM builds out of that shadow, the entry into it, and
        // the reflection of the exit back here.
        // Twice: once with the guest hypervisor using no extended page
        // tables of its own, and once with them. The first exercises the
        // entry and the reflection alone; the second adds the shadow the
        // VMM composes out of the guest hypervisor's tables and its own,
        // which is the piece Hyper-V cannot do without.
        passed &=
            launch(system_table, vmcs_region, false, line, say, step);
        passed &= launch(system_table, vmcs_region, true, line, say, step);

        // And a third, which is the only one that answers a question no
        // layer above this can.
        //
        // Everything else about injection is checkable from outside: that
        // the entry decision is made, that vmcs12's triple is copied into
        // vmcs02, that a halted guest is entered rather than parked. All
        // of it is checked, hosted, in tests/nested_exit and
        // check-exit-handler.sh. **None of it establishes that the
        // injected event actually retires into the second-level guest**,
        // and the two outcomes are indistinguishable from the guest
        // hypervisor's side: SDM 30.2 clears the valid bit of the
        // entry-interruption field on every VM exit, so an injection that
        // was delivered and one that was silently dropped leave the same
        // vmcs12 behind.
        //
        // The only witness is the guest itself. An entry that injects
        // delivers the event *before* the first instruction at the
        // guest's RIP (SDM 29.4, .references/sdm.txt:203155), so the
        // second-level guest starts inside its interrupt handler rather
        // than at its entry point. That handler counts and returns, and
        // the run then continues to the CPUID exit the two launches above
        // already check - so the injection costs one extra exit-free step
        // and proves itself.
        //
        // Vector 0x20 as an external interrupt: type 0 in bits 10:8, and
        // SDM 27.8.3 puts no error code on that type, so the two fields
        // beside the information one stay untouched. Above the
        // architecturally defined exceptions, so nothing else claims it.
        constexpr std::uint64_t interruption_valid = 1ull << 31;
        constexpr std::uint64_t interruption_external = 0ull << 8;
        constexpr std::uint64_t injected_vector = 0x20;

        // The gate the injected vector lands in, installed here rather
        // than asked of the caller.
        //
        // The second-level guest runs with *this* processor's interrupt
        // descriptor table - `launch` copies the IDTR out of it, so
        // whatever is installed when the launch happens is what an
        // injected event is delivered through. Depending on the caller to
        // have put a handler there would make this probe silently useless
        // whenever it did not: the injection would land in the firmware's
        // own handler, which prints and hangs, and the run would time out
        // rather than report.
        //
        // One entry, saved and put back immediately afterwards. The
        // guest's own RFLAGS is 0x2, so interrupts are disabled inside it,
        // and injection ignores RFLAGS.IF in any case - so nothing else
        // can arrive on this vector while it is borrowed.
        struct gate
        {
            std::uint16_t offset_low;
            std::uint16_t selector;
            std::uint16_t attributes;
            std::uint16_t offset_middle;
            std::uint32_t offset_high;
            std::uint32_t reserved;
        };

        auto idtr_now = read_idtr();
        auto * gates = reinterpret_cast<gate *>(idtr_now.base);

        if (idtr_now.limit <
            (((injected_vector + 1) * sizeof(gate)) - 1)) {
            line("zpp: nested SKIP the idt is too small to borrow a "
                 "vector from\r\n");
            return passed;
        }

        // Interrupts off from here until the gate is put back.
        //
        // Not a precaution - a correction. The first run of this measured
        // one delivery and the entry had *failed*, which cannot both be
        // true: a refused entry runs no guest. What incremented the
        // counter was a real interrupt on this vector arriving in the
        // guest hypervisor's own world, through the gate this borrows,
        // while the probe was running with interrupts enabled. The
        // counter has to mean "the second-level guest took it" and
        // nothing else, so nothing else may reach the handler.
        asm volatile("cli" : : : "memory");

        auto saved = gates[injected_vector];

        auto handler =
            reinterpret_cast<std::uint64_t>(&zpp_probe_l2_interrupt);

        gate borrowed{};
        borrowed.offset_low = static_cast<std::uint16_t>(handler);
        borrowed.selector = read_cs();
        // Present, DPL 0, 64-bit interrupt gate, no interrupt stack
        // table: type 0xe in bits 11:8, P in bit 15.
        borrowed.attributes = 0x8e00;
        borrowed.offset_middle = static_cast<std::uint16_t>(handler >> 16);
        borrowed.offset_high = static_cast<std::uint32_t>(handler >> 32);
        gates[injected_vector] = borrowed;

        auto before = zpp_probe_l2_injections;

        passed &= launch(system_table,
                         vmcs_region,
                         false,
                         line,
                         say,
                         step,
                         interruption_valid | interruption_external |
                             injected_vector);

        auto delivered = zpp_probe_l2_injections - before;
        say("injections delivered to the second-level guest", delivered);

        // Where it fired, which is what tells a delivery into the
        // second-level guest from one into the guest hypervisor's own
        // world. The second-level guest's only RIP is its entry point.
        say("the rip the handler interrupted",
            zpp_probe_l2_interrupted_rip);
        say("the second-level guest's entry point",
            reinterpret_cast<std::uint64_t>(&zpp_probe_l2_entry));

        // One, not "at least one". More than one would mean the event was
        // re-delivered on a re-entry nobody asked for, which is its own
        // defect and would show as a guest taking an interrupt storm.
        gates[injected_vector] = saved;
        asm volatile("sti" : : : "memory");

        passed &= step("second-level guest took the injected interrupt",
                       (1 == delivered) ? outcome::succeeded
                                        : outcome::failed_valid,
                       outcome::succeeded);

        // And back out, leaving the processor as it was found. VMXOFF
        // first, because clearing CR4.VMXE while in VMX operation is a
        // fault the guest would deserve - SDM 26.8, "Once in VMX
        // operation, it is not possible to clear CR4.VMXE".
        passed &= step("vmclear before leaving",
                       outcome_of(vmclear(vmcs_region)),
                       outcome::succeeded);
        passed &= step("vmxoff", outcome_of(vmxoff()), outcome::succeeded);

        write_cr4(cr4);
        say("CR4 after clearing VMXE", read_cr4());

        system_table->BootServices->FreePages(pages, 2);

        line(passed ? "zpp: ZPP_NESTED_VMX_OK every step answered as the "
                      "SDM says\r\n"
                    : "zpp: ZPP_NESTED_VMX_FAILED at least one step did "
                      "not\r\n");

        return passed;
    }
};

} // namespace zpp
