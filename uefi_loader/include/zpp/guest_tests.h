#pragma once
extern "C" {
#include <Uefi.h>
}
#include "zpp/trace.h"

#include <cstddef>
#include <cstdint>

/**
 * Symbols from uefi_loader/src/guest_tests.S. Declared unconditionally
 * because a declaration costs nothing; the definitions are behind the same
 * switch this header is, and nothing references them unless the switch is
 * on, so an off build emits no reference and needs no definition.
 * @{
 */
extern "C" {
long long zpp_guest_test_try(void (*body)());
extern std::uint64_t zpp_guest_test_vector;
extern std::uint64_t zpp_guest_test_error;
extern std::uint64_t zpp_guest_test_fault_rip;
extern std::uint64_t zpp_guest_test_recovery_rip;
extern std::uint64_t zpp_guest_test_recovery_rsp;

void zpp_guest_test_stub_0();
void zpp_guest_test_stub_1();
void zpp_guest_test_stub_2();
void zpp_guest_test_stub_3();
void zpp_guest_test_stub_4();
void zpp_guest_test_stub_5();
void zpp_guest_test_stub_6();
void zpp_guest_test_stub_7();
void zpp_guest_test_stub_8();
void zpp_guest_test_stub_9();
void zpp_guest_test_stub_10();
void zpp_guest_test_stub_11();
void zpp_guest_test_stub_12();
void zpp_guest_test_stub_13();
void zpp_guest_test_stub_14();
void zpp_guest_test_stub_15();
void zpp_guest_test_stub_16();
void zpp_guest_test_stub_17();
void zpp_guest_test_stub_18();
void zpp_guest_test_stub_19();
void zpp_guest_test_stub_20();
void zpp_guest_test_stub_21();
void zpp_guest_test_stub_22();
void zpp_guest_test_stub_23();
void zpp_guest_test_stub_24();
void zpp_guest_test_stub_25();
void zpp_guest_test_stub_26();
void zpp_guest_test_stub_27();
void zpp_guest_test_stub_28();
void zpp_guest_test_stub_29();
void zpp_guest_test_stub_30();
void zpp_guest_test_stub_31();
void zpp_guest_test_stub_32();
}
/**
 * @}
 */

namespace zpp
{
/**
 * End to end coverage of what this VMM presents to its guest, run from
 * inside that guest.
 *
 * The loader launches the hypervisor on this processor and then *is* the
 * guest, which is the only position from which the questions in
 * CLAUDE.md's `What the guest is told` can be asked at all: a debugger
 * outside sees guest state only, and the VMCS fields that decide anything
 * cannot be read without being on that processor with that VMCS current.
 * So the tests are guest instructions, and the answers are what the guest
 * gets back.
 *
 * Three mechanisms make that into something a harness can grade.
 *
 * **Faults are caught rather than fatal.** An interrupt descriptor table
 * of this suite's own is installed for the duration - see guest_tests.S -
 * so `vmxon` taking #UD produces a result instead of EDK2's exception
 * handler printing a register dump and hanging.
 *
 * **The exit reason each instruction produced is read back.** CPUID leaf
 * 0x40000100 reports the newest entry of the hypervisor's own per
 * processor exit ring, and it is answered *inside* the CPUID exit handler,
 * before that CPUID is itself recorded - so what it reports is the exit
 * immediately before it. Taking the exit count twice around an
 * instruction says whether the instruction exited at all: two more exits
 * means the instruction's own plus the first reading's, one more means the
 * instruction did not exit. That turns "does this cause exit reason N"
 * from a claim into a measurement, and it is what the coverage report is
 * built from.
 *
 * **Nothing is shared with the VMM.** Every constant below is spelled out
 * here rather than included from hypervisor/include, for the reason
 * verify_nested.h gives for doing the same: a probe that reads the
 * expected answer out of the header the answer was written from tests
 * nothing.
 *
 * Not destructive, deliberately, and that distinction is the whole reason
 * this is not part of verify::present. That check takes every application
 * processor away from the firmware and leaves the boot processor's APIC in
 * x2APIC mode, which is why CLAUDE.md says never to boot one. This suite
 * touches no other processor, leaves the APIC in the mode it found it,
 * restores the IDT and every register it wrote, and could safely chainload
 * afterwards. It still does not, because a test build should end in a
 * verdict rather than in an operating system - but that is a choice here
 * and a requirement there.
 */
struct guest_tests
{
    /**
     * Whether this build carries the suite. Off unless asked for, and
     * forced off by the build system whenever ZPP_VERIFY_HYPERVISOR or
     * ZPP_CHAINLOAD_ONLY is on - the first because its destruction would
     * be attributed to these tests, the second because there would be no
     * hypervisor for them to ask anything.
     */
    static constexpr bool enabled = ZPP_GUEST_TESTS;

    /**
     * Runs every case and reports each on serial. Returns whether the run
     * had no unexpected failure.
     */
    static bool run(EFI_SYSTEM_TABLE * system_table);

    /**
     * A case's outcome.
     *
     * `expected_failure` is for a case written to the SDM or to KVM that
     * this VMM does not satisfy. It is reported, counted and printed with
     * its citation, and it does not fail the run - the alternative was to
     * leave the divergence untested, which is how a known defect becomes
     * an unknown one. `unexpected_pass` is the same case passing, which
     * means the divergence was fixed and the entry should be promoted.
     */
    enum class outcome
    {
        pass,
        fail,
        skip,
        expected_failure,
        unexpected_pass,
    };

private:
    /**
     * The vector reported when nothing faulted. The assembly writes -1,
     * which is not any architectural vector, so it cannot be confused
     * with #DE at vector 0.
     */
    static constexpr long long no_fault = -1;

    /**
     * Vectors the cases below name. SDM Vol. 3A Table 6-1.
     * @{
     */
    static constexpr long long vector_invalid_opcode = 6;
    static constexpr long long vector_general_protection = 13;
    /**
     * @}
     */

    /**
     * The hypervisor CPUID range, and the two leaves inside it this suite
     * knows the shape of. Architecturally this whole range is
     * unimplemented on real hardware and returns whatever the layer above
     * chooses, which is exactly why answering only part of it is a defect.
     * @{
     */
    static constexpr std::uint32_t hypervisor_leaf_first = 0x40000000;
    static constexpr std::uint32_t hypervisor_leaf_last = 0x4fffffff;
    static constexpr std::uint32_t diagnostic_leaf = 0x40000100;
    /**
     * @}
     */

    /**
     * The signature this VMM answers the base leaf with, as the three
     * registers spell it: "ZppZppZppZpp".
     * @{
     */
    static constexpr std::uint32_t signature_ebx = 0x5a70705a;
    static constexpr std::uint32_t signature_ecx = 0x705a7070;
    static constexpr std::uint32_t signature_edx = 0x70705a70;
    /**
     * @}
     */

    /**
     * Basic exit reasons, SDM Vol. 3D Appendix C, "VMX Basic Exit
     * Reasons". Only the ones a case below expects to see.
     * @{
     */
    static constexpr std::uint32_t exit_cpuid = 10;
    static constexpr std::uint32_t exit_invd = 13;
    static constexpr std::uint32_t exit_vmcall = 18;
    static constexpr std::uint32_t exit_vmclear = 19;
    static constexpr std::uint32_t exit_vmlaunch = 20;
    static constexpr std::uint32_t exit_vmptrld = 21;
    static constexpr std::uint32_t exit_vmptrst = 22;
    static constexpr std::uint32_t exit_vmread = 23;
    static constexpr std::uint32_t exit_vmresume = 24;
    static constexpr std::uint32_t exit_vmwrite = 25;
    static constexpr std::uint32_t exit_vmxoff = 26;
    static constexpr std::uint32_t exit_vmxon = 27;
    static constexpr std::uint32_t exit_control_register = 28;
    static constexpr std::uint32_t exit_io_instruction = 30;
    static constexpr std::uint32_t exit_rdmsr = 31;
    static constexpr std::uint32_t exit_wrmsr = 32;
    static constexpr std::uint32_t exit_mwait = 36;
    static constexpr std::uint32_t exit_monitor = 39;
    static constexpr std::uint32_t exit_ept_violation = 48;
    static constexpr std::uint32_t exit_invept = 50;
    static constexpr std::uint32_t exit_invvpid = 53;
    static constexpr std::uint32_t exit_xsetbv = 55;
    static constexpr std::uint32_t exit_vmfunc = 59;
    /**
     * @}
     */

    /**
     * MSR indices, named here rather than shared with the VMM.
     * @{
     */
    static constexpr std::uint32_t ia32_apic_base = 0x1b;
    static constexpr std::uint32_t ia32_efer = 0xc0000080;
    static constexpr std::uint32_t ia32_vmx_basic = 0x480;
    /**
     * @}
     */

    /**
     * Control register bits the cases name.
     * @{
     */
    static constexpr std::uint64_t cr4_vmxe = 1ull << 13;
    static constexpr std::uint64_t cr4_os_xsave = 1ull << 18;
    /**
     * @}
     */

    /**
     * Instruction wrappers.
     *
     * Written with register constraints here rather than reused from
     * zpp/arch/x86_64/asm.h for the reason verify.h gives: everything
     * there is a naked function built for the freestanding ELF target and
     * the ABI this loader is compiled with is not that one. Inline
     * assembly outside zpp/arch/x86_64 is the loader's established
     * exception to the convention in CLAUDE.md, which is about the
     * hypervisor's own sources.
     * @{
     */
    static void cpuid(std::uint32_t leaf,
                      std::uint32_t subleaf,
                      std::uint32_t (&out)[4])
    {
        asm volatile(
            "cpuid"
            : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
            : "a"(leaf), "c"(subleaf));
    }

    static std::uint64_t read_msr(std::uint32_t index)
    {
        std::uint32_t low{};
        std::uint32_t high{};
        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
        return (static_cast<std::uint64_t>(high) << 32) | low;
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

    static std::uint64_t read_cs()
    {
        std::uint64_t value{};
        asm volatile("mov %%cs, %0" : "=r"(value));
        return value;
    }

    static std::uint8_t read_port_8(std::uint16_t port)
    {
        std::uint8_t value{};
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }

    static std::uint16_t read_port_16(std::uint16_t port)
    {
        std::uint16_t value{};
        asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }
    /**
     * @}
     */

    /**
     * The descriptor table register's memory form, which SIDT writes and
     * LIDT reads: a 16-bit limit followed by a 64-bit base.
     */
    struct descriptor_table_register
    {
        std::uint16_t limit{};
        std::uint64_t base{};
    } __attribute__((packed));

    static descriptor_table_register read_idtr()
    {
        descriptor_table_register value{};
        asm volatile("sidt %0" : "=m"(value));
        return value;
    }

    static void write_idtr(const descriptor_table_register & value)
    {
        asm volatile("lidt %0" : : "m"(value));
    }

    /**
     * A 64-bit interrupt gate. SDM Vol. 3A Figure 6-8, "64-Bit IDT Gate
     * Descriptors".
     */
    struct gate
    {
        std::uint16_t offset_low{};
        std::uint16_t selector{};
        std::uint16_t attributes{};
        std::uint16_t offset_middle{};
        std::uint32_t offset_high{};
        std::uint32_t reserved{};
    };

    /**
     * Present, DPL 0, 64-bit interrupt gate, no interrupt stack table.
     * Type 0xe in bits 11:8, P in bit 15.
     */
    static constexpr std::uint16_t interrupt_gate_attributes = 0x8e00;

    static gate make_gate(void (*handler)(), std::uint16_t selector)
    {
        auto address = reinterpret_cast<std::uint64_t>(handler);

        gate result{};
        result.offset_low = static_cast<std::uint16_t>(address);
        result.selector = selector;
        result.attributes = interrupt_gate_attributes;
        result.offset_middle = static_cast<std::uint16_t>(address >> 16);
        result.offset_high = static_cast<std::uint32_t>(address >> 32);
        return result;
    }
};

} // namespace zpp
