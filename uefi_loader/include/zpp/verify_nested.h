#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/trace.h"
#include <cstddef>
#include <cstdint>

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
 * What it is not: a test of VM entry. That needs a second-level guest,
 * which needs its own state and its own stack, and is the next experiment
 * rather than this one. This stops at the point where a guest hypervisor
 * would have written its controls.
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
