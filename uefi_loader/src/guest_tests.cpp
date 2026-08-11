/*
 * The guest-side coverage suite. See zpp/guest_tests.h for what it is and
 * why it lives on this side of the VM entry.
 *
 * Behind the switch in its entirety, like guest_tests.S beside it, so an
 * off build carries none of these strings. That matters more than the
 * bytes: scripts/check-bootable.sh grades a loader by the strings only a
 * test facility emits, and a facility that always compiles cannot be
 * graded that way.
 */
#if ZPP_GUEST_TESTS

#include "zpp/guest_tests.h"
#include "zpp/sleep_control.h"
#include "zpp/trace.h"
#include "zpp/verify_nested.h"

namespace zpp
{
namespace
{
/**
 * Somewhere for a probe to point a memory operand at. Every VMX
 * instruction below is expected to fault before its operand is looked at,
 * so the contents never matter - but the address has to be readable, since
 * a build that ever stops faulting would otherwise fault for the wrong
 * reason and the test would still pass.
 *
 * 16 bytes and 16-byte aligned because INVEPT and INVVPID take a
 * descriptor of that shape.
 */
alignas(16) std::uint8_t g_operand[16]{};

/**
 * Somewhere for SGDT, SIDT, SLDT and STR to write.
 *
 * Separate from g_operand because those four really do execute - the
 * "descriptor-table exiting" control is off, so they are ordinary
 * instructions here - and writing over an operand the VMX probes point at
 * would make one probe depend on the order of another.
 *
 * Ten bytes is the pseudo-descriptor SGDT and SIDT store in 64-bit mode
 * (SDM Vol. 2A, SGDT: a 16-bit limit and a 64-bit base); sixteen is that
 * rounded up so the compiler never has to care.
 * @{
 */
alignas(16) std::uint8_t g_descriptor_scratch[16]{};
std::uint16_t g_selector_scratch{};
/**
 * @}
 */

/**
 * The XSAVE areas for the XSAVES and XRSTORS probes.
 *
 * 64-byte aligned because both instructions raise #GP on an area that is
 * not (SDM Vol. 2C, XRSTORS: "if the address of the XSAVE area is not
 * 64-byte aligned, a general-protection exception (#GP) occurs",
 * .references/sdm.txt:16772), and the claim being made is about the VM
 * exit rather than about the alignment.
 *
 * Two areas rather than one, and that is deliberate. The XRSTORS area is
 * left zeroed for ever, which makes that probe's outcome *determined*:
 * XRSTORS reads the header first and raises #GP when XCOMP_BV[63] is 0
 * (.references/sdm.txt:16776), so it faults before restoring anything and
 * no state component is touched. Letting XSAVES write this area first
 * would produce a valid compacted header and the restore would really
 * happen, which is a state change this suite has no business making.
 * @{
 */
alignas(64) std::uint8_t g_xsave_area[1024]{};
alignas(64) std::uint8_t g_xrstors_area[1024]{};
/**
 * @}
 */

/**
 * What the current probe should use, for the probes that need a parameter.
 * File scope because zpp_guest_test_try takes a plain function pointer:
 * the body has to run on a stack frame this suite is prepared to abandon,
 * and a capturing lambda cannot.
 * @{
 */
std::uint32_t g_probe_msr{};
std::uint64_t g_probe_value{};
volatile std::uint32_t * g_probe_dword{};
/**
 * @}
 */

/**
 * The exit ring's newest entry for the boot processor, read through the
 * hypervisor's diagnostic CPUID leaf.
 */
struct exit_state
{
    std::uint32_t count{};
    std::uint32_t reason{};
    std::uint32_t qualification{};
    std::uint32_t flags{};
};

/**
 * The diagnostic leaf, read at two deliberately different instructions.
 *
 * Two copies rather than one function called twice, and the duplication is
 * load bearing. The hypervisor's exit ring **merges an exit identical to
 * the one before it** - same reason, qualification, activity state, CS,
 * RIP, guest physical address and detail - growing a repeat count instead
 * of taking a slot, and `exit_trace_count` counts slots rather than exits.
 * With one shared reader compiled at -O0 both readings execute the same
 * CPUID at the same address, so the *opening* reading of one probe merges
 * with the *closing* reading of the one before it and the count moves by
 * one where it should move by two.
 *
 * Measured, not deduced: every exit-reason case reported a delta of 1
 * before this split, including the ones whose instruction demonstrably
 * exited - their vector and their effect were both correct in the same
 * run. Two addresses make the two readings distinguishable to the merge
 * test, and the arithmetic in `probe` is then exactly what it says.
 *
 * Marked noinline for the same reason it is duplicated: an optimizer that
 * folded the two back together would silently restore the bug, and this
 * suite runs in release as well as in debug.
 * @{
 */
[[gnu::noinline]] exit_state read_exits_before()
{
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    std::uint32_t d{};
    asm volatile("cpuid"
                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(0x40000100u), "c"(0u));
    return exit_state{a, b, c, d};
}

[[gnu::noinline]] exit_state read_exits_after()
{
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    std::uint32_t d{};

    // Three no-ops, and they are the whole point of this function being
    // separate rather than a second call to the one above.
    //
    // noinline keeps the compiler from folding the two. It does not keep
    // the *linker* from folding them: lld-link enables identical COMDAT
    // folding for a release build, and two byte-identical functions
    // become one address. Measured - release reported a delta of one for
    // every instruction that demonstrably exited, exactly the symptom the
    // shared reader had at -O0, while debug was correct.
    //
    // Three rather than one because one nop is a byte a peephole pass
    // might still remove, and because a run of three is recognisable in a
    // disassembly as deliberate.
    asm volatile("nop; nop; nop");

    asm volatile("cpuid"
                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(0x40000100u), "c"(0u));
    return exit_state{a, b, c, d};
}

/**
 * Whether the two readers really are two.
 *
 * Through a volatile pointer so the comparison survives to run time: the
 * folding this detects happens at link time, and a compile-time compare
 * of two function addresses is answered before the linker has had its
 * say.
 */
bool readers_are_distinct()
{
    exit_state (*volatile first)() = read_exits_before;
    exit_state (*volatile second)() = read_exits_after;
    return first != second;
}
/**
 * @}
 */

} // namespace

/**
 * The probe bodies.
 *
 * Each is a whole function rather than an asm string passed around,
 * because zpp_guest_test_try abandons the frame on a fault: whatever the
 * body left in a callee-saved register is restored by the try wrapper, and
 * whatever it left on its own stack is simply discarded.
 * @{
 */
namespace
{
void probe_vmxon()
{
    asm volatile("vmxon %0" : : "m"(g_operand) : "cc", "memory");
}

void probe_vmxoff()
{
    asm volatile("vmxoff" : : : "cc", "memory");
}

void probe_vmclear()
{
    asm volatile("vmclear %0" : : "m"(g_operand) : "cc", "memory");
}

void probe_vmptrld()
{
    asm volatile("vmptrld %0" : : "m"(g_operand) : "cc", "memory");
}

void probe_vmptrst()
{
    asm volatile("vmptrst %0" : : "m"(g_operand) : "cc", "memory");
}

void probe_vmread()
{
    std::uint64_t value{};
    asm volatile("vmread %1, %0"
                 : "=r"(value)
                 : "r"(std::uint64_t{0x0000})
                 : "cc", "memory");
    g_probe_value = value;
}

void probe_vmwrite()
{
    asm volatile("vmwrite %0, %1"
                 :
                 : "r"(std::uint64_t{0}), "r"(std::uint64_t{0x0000})
                 : "cc", "memory");
}

void probe_vmlaunch()
{
    asm volatile("vmlaunch" : : : "cc", "memory");
}

void probe_vmresume()
{
    asm volatile("vmresume" : : : "cc", "memory");
}

void probe_vmcall()
{
    asm volatile("vmcall" : : : "cc", "memory");
}

void probe_invept()
{
    asm volatile("invept %1, %0"
                 :
                 : "r"(std::uint64_t{1}), "m"(g_operand)
                 : "cc", "memory");
}

void probe_invvpid()
{
    asm volatile("invvpid %1, %0"
                 :
                 : "r"(std::uint64_t{2}), "m"(g_operand)
                 : "cc", "memory");
}

void probe_vmfunc()
{
    asm volatile("vmfunc" : : "a"(0u) : "cc", "memory");
}

void probe_rdmsr()
{
    std::uint32_t low{};
    std::uint32_t high{};
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(g_probe_msr));
    g_probe_value = (static_cast<std::uint64_t>(high) << 32) | low;
}

void probe_wrmsr()
{
    asm volatile(
        "wrmsr"
        :
        : "a"(std::uint32_t{}), "d"(std::uint32_t{}), "c"(g_probe_msr)
        : "memory");
}

void probe_write_cr4()
{
    asm volatile("mov %0, %%cr4" : : "r"(g_probe_value) : "memory");
}

void probe_invd()
{
    asm volatile("invd" : : : "memory");
}

void probe_xsetbv()
{
    // XCR0 back to exactly what XGETBV just reported. Writing the value
    // already there is what makes this safe to run in the middle of a
    // firmware's life: the instruction retires, the exit is taken and
    // handled, and no state changes. SDM Vol. 2C, XSETBV.
    std::uint32_t low{};
    std::uint32_t high{};
    asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0u));
    asm volatile("xsetbv" : : "a"(low), "d"(high), "c"(0u) : "memory");
}

void probe_store_dword()
{
    *g_probe_dword = static_cast<std::uint32_t>(g_probe_value);
}

void probe_rdtsc()
{
    std::uint32_t low{};
    std::uint32_t high{};
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    g_probe_value = (static_cast<std::uint64_t>(high) << 32) | low;
}

void probe_rdtscp()
{
    std::uint32_t low{};
    std::uint32_t high{};
    std::uint32_t aux{};
    asm volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux));
    g_probe_value = (static_cast<std::uint64_t>(high) << 32) | low;
}

void probe_invlpg()
{
    // Against this suite's own operand, which is mapped by construction -
    // the alternative is an unmapped address, and INVLPG on one is
    // architecturally a no-op rather than a fault, so it would prove
    // nothing about the address and everything about the instruction.
    asm volatile("invlpg %0" : : "m"(g_operand) : "memory");
}

void probe_pause()
{
    asm volatile("pause");
}

void probe_mov_from_dr()
{
    std::uint64_t value{};
    asm volatile("mov %%dr0, %0" : "=r"(value));
    g_probe_value = value;
}

void probe_rdpmc()
{
    // Counter zero. On a processor with no architectural performance
    // counters this raises #GP, which the fault catcher records - the
    // claim being made is about the exit, not about the counter.
    std::uint32_t low{};
    std::uint32_t high{};
    asm volatile("rdpmc" : "=a"(low), "=d"(high) : "c"(0u));
    g_probe_value = (static_cast<std::uint64_t>(high) << 32) | low;
}

void probe_wbinvd()
{
    asm volatile("wbinvd" : : : "memory");
}

void probe_invpcid()
{
    // Type 2, all-context, which needs no PCID in the descriptor and no
    // CR4.PCIDE. #UD where the instruction is not supported, which the
    // fault catcher records.
    asm volatile("invpcid %1, %0"
                 :
                 : "r"(std::uint64_t{2}), "m"(g_operand)
                 : "cc", "memory");
}

void probe_rdrand()
{
    std::uint64_t value{};
    asm volatile("rdrand %0" : "=r"(value) : : "cc");
    g_probe_value = value;
}

void probe_rdseed()
{
    std::uint64_t value{};
    asm volatile("rdseed %0" : "=r"(value) : : "cc");
    g_probe_value = value;
}

void probe_xgetbv()
{
    std::uint32_t low{};
    std::uint32_t high{};
    asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0u));
    g_probe_value = (static_cast<std::uint64_t>(high) << 32) | low;
}

void probe_monitor()
{
    // MONITOR arms an address-range monitor on the address in RAX, with
    // extensions and hints zero. Pointed at this suite's own operand,
    // which is writable memory of ours - the instruction is intercepted
    // and never reaches hardware, but pointing it somewhere real is what
    // keeps that true of the code rather than of the operand.
    asm volatile("monitor"
                 :
                 : "a"(&g_operand[0]), "c"(0u), "d"(0u)
                 : "memory");
}

void probe_mwait()
{
    // MWAIT with no extensions and hint zero. Intercepted, so the
    // handler logs it and resumes past it - the processor never enters
    // the wait, which is what makes this safe to run with interrupts
    // disabled. Without the interception this instruction would park the
    // processor with nothing able to wake it.
    asm volatile("mwait" : : "a"(0u), "c"(0u) : "memory");
}

void probe_in_16()
{
    std::uint16_t value{};
    asm volatile("inw %1, %0"
                 : "=a"(value)
                 : "d"(static_cast<std::uint16_t>(g_probe_value)));
    g_probe_value = value;
}

/**
 * The four descriptor-table instructions that only read.
 *
 * SDM Appendix C reason 46 is "Guest software attempted to execute LGDT,
 * LIDT, SGDT, or SIDT and the 'descriptor-table exiting' VM-execution
 * control was 1" and reason 47 says the same of LLDT, LTR, SLDT and STR
 * (.references/sdm.txt:224355 and :224357). The control is secondary bit
 * 2 (SDM Table 27-7, .references/sdm.txt:199537) and this VMM does not
 * request it, so all four must execute without exiting.
 *
 * Only the reading half of each pair is probed. LGDT and LTR would load
 * a descriptor table register, which is the firmware's, and the negative
 * being asserted - that no VM exit is taken - is a property of the
 * control rather than of the direction.
 * @{
 */
void probe_sgdt()
{
    asm volatile("sgdt %0" : "=m"(g_descriptor_scratch) : : "memory");
}

void probe_sidt()
{
    asm volatile("sidt %0" : "=m"(g_descriptor_scratch) : : "memory");
}

void probe_sldt()
{
    asm volatile("sldt %0" : "=m"(g_selector_scratch) : : "memory");
}

void probe_str()
{
    asm volatile("str %0" : "=m"(g_selector_scratch) : : "memory");
}
/**
 * @}
 */

void probe_getsec()
{
    // 0F 37, spelled as bytes because the integrated assembler gates the
    // mnemonic on a target feature this translation unit does not enable.
    //
    // **This probe is only safe while CR4.SMXE is clear, and the case
    // checks that before running it.** GETSEC is in SDM Appendix C's
    // unconditional group - reason 11 is "Guest software attempted to
    // execute GETSEC" with no control named (.references/sdm.txt:224290)
    // - and this VMM's exit handler has no case for it, so an exit would
    // reach `default:` and stop the processor. What stands between a
    // guest and that today is the fault: "#UD If CR4.SMXE = 0"
    // (.references/sdm.txt:138229), and SDM 28.1.1 puts an invalid-opcode
    // exception above the exit.
    //
    // Leaf 0 is CAPABILITIES, which reads nothing and changes nothing -
    // the one leaf that would be harmless if the fault ever stopped
    // happening.
    asm volatile(".byte 0x0f, 0x37"
                 :
                 : "a"(0u)
                 : "rbx", "rcx", "rdx", "cc", "memory");
}

void probe_rsm()
{
    // 0F AA. RSM is reason 17, and Appendix C is explicit that it is
    // "RSM. Guest software attempted to execute RSM in SMM"
    // (.references/sdm.txt:224296) - so outside SMM there is no exit to
    // take, and the instruction raises #UD instead: "#UD If an attempt is
    // made to execute this instruction when the processor is not in SMM"
    // (.references/sdm.txt:88519). A guest is never in SMM, which is what
    // makes this measurable rather than dangerous.
    asm volatile(".byte 0x0f, 0xaa" : : : "cc", "memory");
}

void probe_encls()
{
    // 0F 01 CF, leaf 0, with the register operands the SGX leaves take
    // set to zero rather than left as clobbers - if the instruction ever
    // did execute here, it should execute on nothing.
    asm volatile(".byte 0x0f, 0x01, 0xcf"
                 :
                 : "a"(0ull), "b"(0ull), "c"(0ull), "d"(0ull)
                 : "cc", "memory");
}

void probe_xsaves()
{
    // 0F C7 /5 with RDI as the base: ModRM 0x2f is mod 00, reg 5, rm 7.
    //
    // EDX:EAX is the requested-feature bitmap and is zero, so the
    // instruction saves no state component at all and writes only the
    // XSAVE header. That is what makes running it in the middle of a
    // firmware's life safe.
    asm volatile(".byte 0x0f, 0xc7, 0x2f"
                 :
                 : "D"(&g_xsave_area[0]), "a"(0u), "d"(0u)
                 : "memory");
}

void probe_xrstors()
{
    // 0F C7 /3, ModRM 0x1f. Against the area that is never written, so
    // the header's XCOMP_BV[63] is 0 and the instruction raises #GP
    // before restoring anything - see g_xrstors_area.
    asm volatile(".byte 0x0f, 0xc7, 0x1f"
                 :
                 : "D"(&g_xrstors_area[0]), "a"(0u), "d"(0u)
                 : "memory");
}

void probe_store_dword_stos()
{
    // The same store as probe_store_dword, in a form the instruction
    // decoder refuses.
    //
    // STOSD is the one-byte opcode AB, and the decoder in
    // zpp/arch/x86_64/instruction.h answers no string instruction at all
    // - its one-byte table is the MOV, arithmetic, XCHG and group forms.
    // So a watched page written this way cannot be emulated, and the VMM
    // has to fall back to opening the page and stepping the guest's own
    // instruction over it, which is what makes exit reason 37 reachable.
    //
    // The direction flag is clear on entry to any function under the
    // Microsoft x64 ABI, so this stores forwards and RDI ends one dword
    // on. Nothing reads it back, which is why RDI is an in-out operand
    // rather than an output that matters.
    auto address = reinterpret_cast<std::uint64_t>(g_probe_dword);
    auto value = static_cast<std::uint32_t>(g_probe_value);
    asm volatile("stosl" : "+D"(address) : "a"(value) : "memory");
}
} // namespace
/**
 * @}
 */

/**
 * Everything the run needs to carry between cases, kept in one object so
 * the case functions below take one argument rather than six references.
 */
namespace
{
struct session
{
    std::size_t passed{};
    std::size_t failed{};
    std::size_t skipped{};
    std::size_t expected_failures{};
    std::size_t unexpected_passes{};

    /**
     * Which basic exit reasons this run actually observed, indexed by the
     * reason. The coverage report is built from this rather than from the
     * list of cases, so a case that was written but whose instruction
     * silently stopped exiting shows up as a gap.
     */
    bool observed_exit[72]{};

    /**
     * Why a reason that was *not* reached was not reached, when the run
     * itself settled the question.
     *
     * The coverage table below carries a disposition for every reason,
     * written down once and reviewed like any other constant. A few of
     * them cannot be decided until the run happens - whether the firmware
     * offered an ACPI sleep control port, whether this processor has
     * XSAVE, whether the local APIC is in xAPIC mode - and for those the
     * case that discovers it writes the answer here and the table's
     * static disposition is overridden.
     *
     * Null means "the table's answer stands", which is the normal case.
     */
    const char * why[72]{};

    void note_exit(std::uint32_t reason)
    {
        if (reason < (sizeof(observed_exit) / sizeof(observed_exit[0]))) {
            observed_exit[reason] = true;
        }
    }

    void note_why(std::uint32_t reason, const char * text)
    {
        if (reason < (sizeof(why) / sizeof(why[0]))) {
            why[reason] = text;
        }
    }
};

/**
 * One result line.
 *
 * The format is fixed and parsed by scripts/ci/bochs-exit-coverage.sh:
 *
 *     ZPPTEST <name> <PASS|FAIL|SKIP|XFAIL|XPASS> <detail>
 *
 * The detail carries expected and actual as bare hex so the harness can
 * print a diff without knowing anything about the case.
 */
void emit(session & run,
          const char * name,
          guest_tests::outcome result,
          const char * detail,
          std::uint64_t expected,
          std::uint64_t actual)
{
    const char * word = "PASS";

    switch (result) {
    case guest_tests::outcome::pass:
        ++run.passed;
        break;
    case guest_tests::outcome::fail:
        ++run.failed;
        word = "FAIL";
        break;
    case guest_tests::outcome::skip:
        ++run.skipped;
        word = "SKIP";
        break;
    case guest_tests::outcome::expected_failure:
        ++run.expected_failures;
        word = "XFAIL";
        break;
    case guest_tests::outcome::unexpected_pass:
        ++run.unexpected_passes;
        word = "XPASS";
        break;
    }

    char line[trace::line_capacity]{};
    auto at = trace::append_text(line, "ZPPTEST ");
    at = trace::append_text(at, name);
    at = trace::append_text(at, " ");
    at = trace::append_text(at, word);
    at = trace::append_text(at, " ");
    at = trace::append_text(at, detail);
    at = trace::append_text(at, " expected=");
    at = trace::append_hex(at, expected, 8);
    at = trace::append_text(at, " actual=");
    at = trace::append_hex(at, actual, 8);
    at = trace::append_text(at, "\r\n");
    *at = 0;
    trace::raw(line);
}

/**
 * The common shape: a value that has to equal another.
 */
void check_equal(session & run,
                 const char * name,
                 const char * detail,
                 std::uint64_t expected,
                 std::uint64_t actual)
{
    emit(run,
         name,
         (expected == actual) ? guest_tests::outcome::pass
                              : guest_tests::outcome::fail,
         detail,
         expected,
         actual);
}

} // namespace

/**
 * The suite proper. One function because the cases share the exit-reason
 * reader and the fault catcher, and splitting them would mean passing both
 * plus the session through every one.
 */
bool guest_tests::run(EFI_SYSTEM_TABLE * system_table)
{
    session state{};

    // The interrupt descriptor table this suite runs under. A function
    // local static rather than a member so an off build - which never
    // references run() at all - carries neither the table nor its
    // relocations.
    static gate table[256]{};

    auto selector = static_cast<std::uint16_t>(read_cs());

    void (*stubs[33])() = {
        zpp_guest_test_stub_0,  zpp_guest_test_stub_1,
        zpp_guest_test_stub_2,  zpp_guest_test_stub_3,
        zpp_guest_test_stub_4,  zpp_guest_test_stub_5,
        zpp_guest_test_stub_6,  zpp_guest_test_stub_7,
        zpp_guest_test_stub_8,  zpp_guest_test_stub_9,
        zpp_guest_test_stub_10, zpp_guest_test_stub_11,
        zpp_guest_test_stub_12, zpp_guest_test_stub_13,
        zpp_guest_test_stub_14, zpp_guest_test_stub_15,
        zpp_guest_test_stub_16, zpp_guest_test_stub_17,
        zpp_guest_test_stub_18, zpp_guest_test_stub_19,
        zpp_guest_test_stub_20, zpp_guest_test_stub_21,
        zpp_guest_test_stub_22, zpp_guest_test_stub_23,
        zpp_guest_test_stub_24, zpp_guest_test_stub_25,
        zpp_guest_test_stub_26, zpp_guest_test_stub_27,
        zpp_guest_test_stub_28, zpp_guest_test_stub_29,
        zpp_guest_test_stub_30, zpp_guest_test_stub_31,
        zpp_guest_test_stub_32,
    };

    for (std::size_t i = 0; i < 256; ++i) {
        table[i] = make_gate(stubs[(i < 32) ? i : 32], selector);
    }

    descriptor_table_register ours{};
    ours.limit = static_cast<std::uint16_t>(sizeof(table) - 1);
    ours.base = reinterpret_cast<std::uint64_t>(&table[0]);

    // The firmware console, put back at the end.
    //
    // trace::raw writes to serial *and* to ConOut, and ConOut is a call
    // into the firmware. Everything below runs with interrupts disabled
    // and an interrupt descriptor table the firmware knows nothing about,
    // so a firmware call from inside that window is a call into code that
    // may enable interrupts, wait on an event or fault - all of which
    // arrive somewhere this suite has taken over.
    //
    // Measured, not reasoned about: with the console left set, the run
    // stopped part way through the fourth case's result line and never
    // resumed. Nulling it also removes the duplicate every result line
    // otherwise has, since under Bochs OVMF routes ConOut to the same
    // serial port trace::raw writes to directly.
    auto * console = trace::console;
    trace::console = nullptr;

    trace::raw("ZPPTEST BEGIN\r\n");

    // Interrupts off for the whole run. Not because a fault needs it -
    // the gates are interrupt gates and clear IF themselves - but because
    // the exit-count arithmetic below assumes nothing else on this
    // processor takes a VM exit between two readings, and a firmware
    // timer handler executing one CPUID would be enough to break it.
    asm volatile("cli" : : : "memory");

    auto firmware_idtr = read_idtr();
    write_idtr(ours);

    // Runs a probe and reports which exit reason it produced, or
    // no_exit_reason when it produced none.
    //
    // Two readings around the probe. Each reading is itself an exit, so a
    // probe that exited leaves the count two higher and one that did not
    // leaves it one higher - which is what distinguishes "exited with
    // reason N" from "did not exit, and N is left over from the previous
    // reading".
    // What the reader below reports for an instruction that did not exit.
    //
    // The two readings are themselves exits, so the count moves by one for
    // a probe that did not exit - the opening reading's own record - and
    // by two for one that did. Any other delta means something else exited
    // in the gap, and it is encoded in the low half rather than folded
    // into either answer, so a case that fails says which of the two it
    // was.
    constexpr std::uint32_t no_exit_reason = 0xffff0001;
    constexpr std::uint32_t basic_reason_mask = 0xffff;

    struct probe_result
    {
        std::uint32_t reason{};
        std::uint32_t qualification{};
        long long vector{};

        /**
         * How far the exit count actually moved, and what the newest ring
         * entry says, both unfolded.
         *
         * `reason` answers the common question - "did this instruction
         * exit, and with what" - and deliberately refuses to answer when
         * the count moved by anything other than two. One case needs more
         * than that: an instruction that produces *two* exits, which is
         * what a write the decoder refuses does. It takes an EPT
         * violation, is stepped over, and takes a monitor-trap-flag exit
         * after it, so the count moves by three and the newest entry is
         * the second of the two. Reading that out of `reason`'s encoding
         * would be reading a diagnostic as a value.
         * @{
         */
        std::uint32_t delta{};
        std::uint32_t newest{};
        /**
         * @}
         */
    };

    auto probe = [&](void (*body)()) {
        auto before = read_exits_before();
        auto vector = zpp_guest_test_try(body);
        auto after = read_exits_after();

        probe_result result{};
        result.vector = vector;
        result.qualification = after.qualification;
        result.delta = after.count - before.count;
        result.newest = after.reason & basic_reason_mask;
        // The low half of a "no exit" answer carries how far the count
        // actually moved, so a failing case says whether nothing exited
        // or something unexpected also did.
        result.reason =
            (after.count == (before.count + 2))
                ? (after.reason & basic_reason_mask)
                : (0xffff0000u | ((after.count - before.count) & 0xffff));

        if (no_exit_reason != result.reason) {
            state.note_exit(result.reason);
        }

        return result;
    };

    // === The measuring instrument itself ===============================
    //
    // Everything below that names an exit reason depends on the exit ring
    // being live and on the arithmetic that reads it, so both are asserted
    // first. A suite whose instrument is broken reports every case as a
    // failure of the thing under test, which is worse than reporting
    // nothing.
    {
        // The two readers have to be two, or every delta below is one
        // short. Checked first, because a folded pair still lets the
        // three cases under it pass - the first pair of readings in a run
        // has nothing before it to merge with, so it measures correctly
        // once and then poisons everything after it.
        emit(state,
             "diag.readers_are_distinct",
             readers_are_distinct() ? outcome::pass : outcome::fail,
             "linker_folded_the_two_exit_readers",
             1,
             readers_are_distinct() ? 1 : 0);

        auto first = read_exits_before();
        auto second = read_exits_after();

        // The ring counts every exit this processor has taken, and by the
        // time a guest is executing there have been thousands.
        emit(state,
             "diag.exit_ring_live",
             first.count ? outcome::pass : outcome::fail,
             "leaf_0x40000100_eax",
             1,
             first.count);

        // Two readings with nothing between them differ by exactly one:
        // the first reading's own exit. Anything else means some other
        // exit is arriving in the gap - a VMX-preemption timer, say - and
        // every exit reason measured below would be reading that instead.
        check_equal(state,
                    "diag.exit_count_step",
                    "consecutive_cpuid_delta",
                    1,
                    second.count - first.count);

        // And the reason the ring holds after a CPUID is CPUID's own.
        check_equal(state,
                    "diag.exit_ring_newest_is_previous",
                    "reason",
                    exit_cpuid,
                    second.reason & basic_reason_mask);
    }

    // === CPUID =========================================================
    //
    // SDM 28.1.2 (.references/sdm.txt:200699): CPUID "cause[s] VM exits
    // when [it is] executed in VMX non-root operation", unconditionally.
    // So every leaf below is answered by the exit handler and nothing
    // reaches this processor's real CPUID unedited.
    {
        std::uint32_t registers[4]{};

        cpuid(hypervisor_leaf_first, 0, registers);
        check_equal(state,
                    "cpuid.signature_ebx",
                    "leaf=0x40000000",
                    signature_ebx,
                    registers[1]);
        check_equal(state,
                    "cpuid.signature_ecx",
                    "leaf=0x40000000",
                    signature_ecx,
                    registers[2]);
        check_equal(state,
                    "cpuid.signature_edx",
                    "leaf=0x40000000",
                    signature_edx,
                    registers[3]);

        // EAX at the base leaf is the highest leaf of *this* block, which
        // is how KVM's own reader takes it - kvm_get_hypervisor_cpuid
        // records `cpuid.limit = entry->eax`. With the interface leaves
        // switched off the block is one leaf wide, so the base leaf's own
        // number is the answer.
        check_equal(state,
                    "cpuid.block_maximum",
                    "leaf=0x40000000_eax",
                    hypervisor_leaf_first,
                    registers[0]);

        // Leaf 1, the pair that must agree with CR4 below.
        cpuid(1, 0, registers);
        auto ecx = registers[2];

        // ECX[5] is VMX. SDM Vol. 2A, CPUID, Table 3-8.
        // Only with the nested machinery compiled out. With it in, the
        // guest is deliberately told VMX exists - that is the whole point
        // of the switch - so this asserts the default build's answer and
        // reports a skip on the other, rather than being written twice or
        // failing for a reason that is not a defect.
        if constexpr (!ZPP_NESTED_VMX) {
            check_equal(state,
                        "cpuid.leaf1_vmx_hidden",
                        "leaf=1_ecx_bit5",
                        0,
                        (ecx >> 5) & 1);
        } else {
            check_equal(state,
                        "cpuid.leaf1_vmx_offered_with_nesting",
                        "leaf=1_ecx_bit5",
                        1,
                        (ecx >> 5) & 1);
        }

        // ECX[6] is SMX, concealed for the same reason.
        check_equal(state,
                    "cpuid.leaf1_smx_hidden",
                    "leaf=1_ecx_bit6",
                    0,
                    (ecx >> 6) & 1);

        // ECX[31] is the conventional hypervisor-present bit: reserved on
        // real hardware, so leaving it clear is indistinguishable from
        // bare metal. This VMM leaves it clear unless it is deliberately
        // presenting the interface of whatever it runs under.
        check_equal(state,
                    "cpuid.leaf1_hypervisor_bit_clear",
                    "leaf=1_ecx_bit31",
                    0,
                    (ecx >> 31) & 1);

        // The whole reserved range, not just the leaf holding the
        // signature. This is the 0x40000022 defect stated as a test:
        // a leaf left unanswered falls through to whatever is underneath,
        // and a guest that reads one vendor from one leaf and another
        // interface from the next acts on the more specific claim.
        //
        // Sampled rather than exhaustive - the range is 268 million leaves
        // - across the boundaries, the block this VMM defines, the leaf
        // Windows actually reads, and a spread of the rest.
        //
        // **This case can only bite where something underneath answers.**
        // Measured: with the range check deliberately narrowed to six
        // leaves, every sample still came back zero, because Bochs'
        // CPUID returns zero for an unimplemented leaf and there is no
        // second hypervisor beneath it. The same narrowing on the rig,
        // where KVM answers its own block, would be caught here. What
        // caught it under Bochs was diag.exit_ring_live above - the
        // diagnostic leaf sits at 0x40000100, so a range that stops short
        // of it stops answering the instrument itself. Both are kept: one
        // is the direct statement of the rule, the other is what actually
        // fails on an emulator with nothing behind it.
        static constexpr std::uint32_t sampled[]{
            0x40000001,
            0x40000002,
            0x40000003,
            0x40000004,
            0x40000005,
            0x40000006,
            0x40000022,
            0x40000080,
            0x40000101,
            0x40001000,
            0x40010000,
            0x41000000,
            0x48000000,
            0x4f000000,
            0x4ffffffe,
            hypervisor_leaf_last,
        };

        std::uint32_t leaked{};
        std::uint32_t first_leak{};

        for (auto leaf : sampled) {
            cpuid(leaf, 0, registers);
            if (registers[0] || registers[1] || registers[2] ||
                registers[3]) {
                if (!leaked) {
                    first_leak = leaf;
                }
                ++leaked;
            }
        }

        emit(state,
             "cpuid.range_answered_whole",
             leaked ? outcome::fail : outcome::pass,
             "nonzero_leaves_in_0x40000000..0x4fffffff",
             0,
             leaked ? first_leak : 0);

        // Leaf 0's vendor string is the hardware's, untouched. A VMM that
        // started editing it would be lying about something it has no
        // reason to.
        cpuid(0, 0, registers);
        emit(state,
             "cpuid.leaf0_vendor_present",
             registers[1] ? outcome::pass : outcome::fail,
             "leaf=0_ebx",
             1,
             registers[1]);

        // And the exit reason CPUID itself produces, measured rather than
        // assumed - this is what every reading above depends on.
        auto measured = probe([] {
            std::uint32_t discard[4]{};
            asm volatile("cpuid"
                         : "=a"(discard[0]),
                           "=b"(discard[1]),
                           "=c"(discard[2]),
                           "=d"(discard[3])
                         : "a"(0u), "c"(0u));
        });
        check_equal(
            state, "exit.cpuid", "reason", exit_cpuid, measured.reason);
    }

    // === CR4 read shadow ===============================================
    //
    // The other half of the pair leaf 1 ECX[5] is one of. "No VMX in
    // CPUID, VMXE set in CR4" exists on no real processor, and a guest
    // that trusts CR4 faults on its own VMXON - BACKLOG.md item 1.
    //
    // MOV from CR4 does not exit: SDM (.references/sdm.txt:201080) says
    // the destination is loaded from the read shadow for every bit set in
    // the CR4 guest/host mask, and from CR4 itself for the rest. So this
    // reads what the VMM decided to show, with no exit involved.
    {
        auto cr4 = read_cr4();
        check_equal(state,
                    "cr4.vmxe_reads_clear",
                    "cr4_bit13",
                    0,
                    (cr4 & cr4_vmxe) ? 1 : 0);

        // MOV to CR4 exits "unless the value of its source operand
        // matches, for the position of each bit set in the CR4 guest/host
        // mask, the corresponding bit in the CR4 read shadow"
        // (.references/sdm.txt:200770). VMXE is in the mask and clear in
        // the shadow, so setting it is guaranteed to exit.
        g_probe_value = cr4 | cr4_vmxe;
        auto measured = probe(probe_write_cr4);
        check_equal(state,
                    "exit.control_register_access",
                    "mov_to_cr4_reason",
                    exit_control_register,
                    measured.reason);

        // And what the guest sees afterwards. This VMM answers the write
        // by keeping VMXE in the real register - a processor in root mode
        // must have it - and clearing it in the shadow, so the guest's
        // view keeps agreeing with the CPUID leaf.
        auto after = read_cr4();

        // The other half of the pair, and it moves with the same switch.
        // With nesting out the guest reads VMXE back clear, so its view
        // agrees with the CPUID leaf above. With nesting in it is
        // entitled to turn VMX on and see that it did.
        if constexpr (!ZPP_NESTED_VMX) {
            check_equal(state,
                        "cr4.vmxe_still_clear_after_write",
                        "cr4_bit13",
                        0,
                        (after & cr4_vmxe) ? 1 : 0);
        } else {
            check_equal(state,
                        "cr4.vmxe_reads_back_set_with_nesting",
                        "cr4_bit13",
                        1,
                        (after & cr4_vmxe) ? 1 : 0);
        }

        // Everything else the guest wrote must have survived.
        check_equal(state,
                    "cr4.other_bits_preserved",
                    "cr4_without_vmxe",
                    cr4 & ~cr4_vmxe,
                    after & ~cr4_vmxe);

        // Written to the SDM and to KVM rather than to this VMM, and
        // expected to fail today.
        //
        // KVM rejects a CR4 write that sets a bit the guest's own CPUID
        // says is unsupported: __kvm_is_valid_cr4 refuses
        // `cr4 & vcpu->arch.cr4_guest_rsvd_bits`
        // (.references/kvm/x86.c:1317-1326) and kvm_set_cr4 returns
        // failure (:1381), which its caller turns into #GP. A guest told
        // there is no VMX therefore takes #GP for setting CR4.VMXE, and
        // that is what a real processor without VMX does too, since the
        // bit is reserved there.
        //
        // This VMM instead accepts the write silently and reports the bit
        // back clear. That is the *less* harmful of the two wrong answers
        // - a guest reading its own write back as refused learns
        // something, where a guest that faults on a bit it was never
        // offered learns the same thing louder - but it is still a write
        // that neither hardware nor the reference implementation would
        // have allowed. Recorded here so the divergence is a known one.
        emit(state,
             "cr4.vmxe_write_faults",
             (no_fault == measured.vector) ? outcome::expected_failure
                                           : outcome::unexpected_pass,
             "kvm_x86.c:1317_injects_gp_vector",
             vector_general_protection,
             static_cast<std::uint64_t>(measured.vector));

        // Put it back exactly as the firmware had it, so nothing after
        // this run inherits a CR4 the tests chose.
        g_probe_value = cr4;
        static_cast<void>(probe(probe_write_cr4));
    }

    // === CR4.SMXE, the same pairing one bit over ========================
    //
    // This is the regression case for a bug a guest could have used to
    // stop a physical processor with two instructions.
    //
    // Safer mode extensions are concealed in CPUID leaf 1 ECX[6], and for
    // a long time that was the whole of it: CR4.SMXE was **not** in the
    // CR4 guest/host mask, so on a processor that implements SMX a guest
    // could set the bit against a CPUID saying the feature does not
    // exist. SDM 28.1.2 (.references/sdm.txt:200727) then makes GETSEC
    // exit unconditionally - "regardless of the value of CPL or RAX" -
    // and the exit handler had no case for reason 11, so it reached
    // `default:`, which does not resume.
    //
    // Concealing a feature in CPUID is not the same as making it
    // unreachable, and this case is what says so. It asserts the whole
    // chain rather than the fix: the bit reads clear, a write setting it
    // does not take, and GETSEC still raises #UD afterwards.
    {
        constexpr std::uint64_t cr4_smxe = 1ull << 14;

        auto cr4 = read_cr4();

        check_equal(state,
                    "cr4.smxe_reads_clear",
                    "cr4_bit14",
                    0,
                    (cr4 & cr4_smxe) ? 1 : 0);

        // SMXE is in the mask and clear in the read shadow, so setting it
        // exits for the same reason VMXE does
        // (.references/sdm.txt:200770).
        g_probe_value = cr4 | cr4_smxe;
        auto measured = probe(probe_write_cr4);
        check_equal(state,
                    "exit.control_register_access_smxe",
                    "mov_to_cr4_reason",
                    exit_control_register,
                    measured.reason);

        // And the bit does not take, in the shadow the guest reads. The
        // real register does not get it either, which no guest-side test
        // can see directly - what it *can* see is the consequence, which
        // is the GETSEC case below.
        auto after = read_cr4();

        check_equal(state,
                    "cr4.smxe_still_clear_after_write",
                    "cr4_bit14",
                    0,
                    (after & cr4_smxe) ? 1 : 0);

        check_equal(state,
                    "cr4.other_bits_preserved_across_smxe_write",
                    "cr4_without_smxe",
                    cr4 & ~cr4_smxe,
                    after & ~cr4_smxe);

        // The consequence, and the reason the mask matters. GETSEC is
        // executed *after* an attempt to enable it, which is exactly the
        // sequence a guest would use to reach the missing case. It must
        // still take #UD and it must not exit: with CR4.SMXE clear the
        // invalid-opcode exception has priority over the VM exit (SDM
        // 28.1.1, .references/sdm.txt:200675), so the instruction never
        // reaches the handler at all.
        //
        // If this ever reports an exit rather than a fault, the mask has
        // been lost and the handler's GETSEC case is the only thing
        // standing between a guest and a stopped processor. That case
        // exists, so the run would survive to report it - which is the
        // whole reason it was added alongside the mask rather than
        // instead of it.
        auto after_getsec = probe(probe_getsec);

        check_equal(state,
                    "quiet.getsec.still_faults_after_setting_smxe",
                    "vector",
                    vector_invalid_opcode,
                    static_cast<std::uint64_t>(after_getsec.vector));

        check_equal(state,
                    "quiet.getsec.takes_no_exit_after_setting_smxe",
                    "reason",
                    no_exit_reason,
                    after_getsec.reason);

        g_probe_value = cr4;
        static_cast<void>(probe(probe_write_cr4));
    }

    // === The VMX instructions ==========================================
    //
    // All thirteen exit, and every one must come back as #UD rather than
    // stopping the processor. Until 8412b76 they fell to `default:` and
    // halted it, which meant a guest instruction could take a processor
    // away - CLAUDE.md states the rule this enforces: nothing a guest can
    // execute may reach `default:`.
    //
    // #UD is the architecturally correct answer rather than a fallback.
    // VMXON's operation section (.references/sdm.txt:208423) begins "IF
    // (register operand) or (CR0.PE = 0) or (CR4.VMXE = 0) ... THEN #UD",
    // and this guest's CR4 reads VMXE clear - checked directly above. SDM
    // 28.1.1 (.references/sdm.txt:200675) puts that exception above the
    // exit: "Certain exceptions have priority over VM exits. These
    // include invalid-opcode exceptions".
    //
    // Each case asserts three things: the exit reason the instruction
    // produced, the vector it came back with, and that RIP did not move -
    // a fault is reported at the faulting instruction, so a VMM that
    // advanced RIP would have made the guest skip a live instruction.
    {
        struct vmx_case
        {
            const char * name;
            void (*body)();
            std::uint32_t reason;
        };

        // Static so it does not land on `run`'s stack frame: at
        // -O0 every local table here is copied there on entry, and
        // the total went past 4 KB, which makes the Microsoft ABI
        // ask for `__chkstk` - see the note on `names` below.
        static const vmx_case cases[]{
            {"vmxon", probe_vmxon, exit_vmxon},
            {"vmxoff", probe_vmxoff, exit_vmxoff},
            {"vmclear", probe_vmclear, exit_vmclear},
            {"vmptrld", probe_vmptrld, exit_vmptrld},
            {"vmptrst", probe_vmptrst, exit_vmptrst},
            {"vmread", probe_vmread, exit_vmread},
            {"vmwrite", probe_vmwrite, exit_vmwrite},
            {"vmlaunch", probe_vmlaunch, exit_vmlaunch},
            {"vmresume", probe_vmresume, exit_vmresume},
            {"vmcall", probe_vmcall, exit_vmcall},
            {"invept", probe_invept, exit_invept},
            {"invvpid", probe_invvpid, exit_invvpid},
        };

        for (const auto & entry : cases) {
            auto measured = probe(entry.body);

            char name[64]{};
            auto at = trace::append_text(name, "vmx.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".exit_reason");
            *at = 0;
            check_equal(
                state, name, "reason", entry.reason, measured.reason);

            at = trace::append_text(name, "vmx.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".invalid_opcode");
            *at = 0;
            check_equal(state,
                        name,
                        "vector",
                        vector_invalid_opcode,
                        static_cast<std::uint64_t>(measured.vector));

            // The fault has to have been reported somewhere inside this
            // image. A zero here means no fault was taken at all, which
            // the vector check above would already have said - this is
            // the weaker but independent claim that RIP was left on an
            // instruction rather than advanced into nothing.
            at = trace::append_text(name, "vmx.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".rip_recorded");
            *at = 0;
            emit(state,
                 name,
                 zpp_guest_test_fault_rip ? outcome::pass : outcome::fail,
                 "fault_rip",
                 1,
                 zpp_guest_test_fault_rip ? 1 : 0);
        }

        // VMFUNC, which is the one of the fourteen that does *not* reach
        // this VMM at all, and finding that out is the point of measuring
        // rather than asserting.
        //
        // SDM 28.5.7.2 (.references/sdm.txt:201627): "The VMFUNC
        // instruction causes an invalid-opcode exception (#UD) if the
        // 'enable VM functions' VM-execution controls is 0 ... Otherwise,
        // the instruction causes a VM exit if the bit at position EAX is 0
        // in the VM-function controls". This VMM never sets that control -
        // it is not in setup_vmcs's secondary controls - so the processor
        // raises #UD itself and exit reason 59 is unreachable.
        //
        // The guest gets the right answer either way, which is why this is
        // recorded rather than reported as a defect: the exit handler's
        // `vmfunc` case is dead code that would become live the moment the
        // control was enabled, and having it is better than not.
        auto measured = probe(probe_vmfunc);
        check_equal(state,
                    "vmx.vmfunc.does_not_exit",
                    "reason",
                    no_exit_reason,
                    measured.reason);
        check_equal(state,
                    "vmx.vmfunc.invalid_opcode",
                    "vector",
                    vector_invalid_opcode,
                    static_cast<std::uint64_t>(measured.vector));
    }

    // === MSRs ==========================================================
    //
    // SDM 28.1.3 (.references/sdm.txt:200814) lists, among the reasons
    // RDMSR causes a VM exit, that "the MSR address is not in the ranges
    // 00000000H - 00001FFFH and C0000000H - C0001FFFH". Outside those two
    // ranges the access exits unconditionally and no bitmap can stop it -
    // which is why an all-zeroes bitmap does not.
    //
    // And the answer for an MSR that does not exist is the one bare
    // hardware gives. The SDM is explicit that the exit is what happens
    // rather than the fault (.references/sdm.txt:200694: "RDMSR of a
    // non-existent MSR with CPL = 0 generates a VM exit and not a
    // general-protection exception"), so the fault is this VMM's to
    // deliver, and delivering it is the whole point: resuming past the
    // instruction instead leaves the guest believing it read a value, and
    // Windows fails a long way from here with 0xc000000d.
    {
        struct msr_case
        {
            const char * name;
            std::uint32_t index;
        };

        // Every one of these is outside both bitmap ranges, so every one
        // exits unconditionally. 0x40000022 is the Hyper-V timer
        // frequency, and is the exact MSR whose silently-skipped read
        // cost an afternoon and produced a boot screen blaming Windows'
        // own boot configuration data.
        // Static so it does not land on `run`'s stack frame: at
        // -O0 every local table here is copied there on entry, and
        // the total went past 4 KB, which makes the Microsoft ABI
        // ask for `__chkstk` - see the note on `names` below.
        static const msr_case faulting[]{
            {"hyperv_frequency", 0x40000022},
            {"hyperv_identity", 0x40000000},
            {"just_above_low_range", 0x00002000},
            {"just_above_high_range", 0xc0002000},
            {"far_outside", 0x50000000},
        };

        for (const auto & entry : faulting) {
            g_probe_msr = entry.index;
            auto measured = probe(probe_rdmsr);

            char name[64]{};
            auto at = trace::append_text(name, "msr.rdmsr.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".exit_reason");
            *at = 0;
            check_equal(
                state, name, "reason", exit_rdmsr, measured.reason);

            at = trace::append_text(name, "msr.rdmsr.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".general_protection");
            *at = 0;
            check_equal(state,
                        name,
                        "vector",
                        vector_general_protection,
                        static_cast<std::uint64_t>(measured.vector));

            // A #GP raised by a VMM rather than by a segment or selector
            // problem carries error code zero. SDM Vol. 3A 6.13,
            // "Error Code": the code is zero when the exception is not
            // segment related.
            at = trace::append_text(name, "msr.rdmsr.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".error_code_zero");
            *at = 0;
            check_equal(state, name, "error", 0, zpp_guest_test_error);
        }

        // The write half of the same rule.
        g_probe_msr = 0x40000022;
        auto write_measured = probe(probe_wrmsr);
        check_equal(state,
                    "msr.wrmsr.hyperv_frequency.exit_reason",
                    "reason",
                    exit_wrmsr,
                    write_measured.reason);
        check_equal(state,
                    "msr.wrmsr.hyperv_frequency.general_protection",
                    "vector",
                    vector_general_protection,
                    static_cast<std::uint64_t>(write_measured.vector));

        // The MSRs that do exist, to show the fault above is about the
        // index rather than about every RDMSR.
        //
        // IA32_APIC_BASE is intercepted deliberately - the bitmap's read
        // and write bits are independent, and a guest about to change
        // APIC modes reads this first - so it exits *and* is answered.
        g_probe_msr = ia32_apic_base;
        auto apic = probe(probe_rdmsr);
        check_equal(state,
                    "msr.rdmsr.ia32_apic_base.exit_reason",
                    "reason",
                    exit_rdmsr,
                    apic.reason);
        check_equal(state,
                    "msr.rdmsr.ia32_apic_base.no_fault",
                    "vector",
                    static_cast<std::uint64_t>(no_fault),
                    static_cast<std::uint64_t>(apic.vector));

        auto apic_base = g_probe_value;

        // Bit 11 is the global enable and bit 8 says this is the
        // bootstrap processor. SDM 13.12.1, and note_apic_mode in the
        // hypervisor reads the same pair.
        check_equal(state,
                    "msr.ia32_apic_base.enabled",
                    "bit11",
                    1,
                    (apic_base >> 11) & 1);
        check_equal(state,
                    "msr.ia32_apic_base.bootstrap_processor",
                    "bit8",
                    1,
                    (apic_base >> 8) & 1);

        // IA32_EFER lives in the high range, so this is the other bitmap
        // half. Not intercepted, so it must *not* exit - which is the
        // half of the bitmap rule that an all-zeroes bitmap gets right by
        // accident and that a wrongly-armed one would break.
        g_probe_msr = ia32_efer;
        auto efer = probe(probe_rdmsr);
        check_equal(state,
                    "msr.rdmsr.ia32_efer.does_not_exit",
                    "reason",
                    no_exit_reason,
                    efer.reason);

        // LME is bit 8 and LMA is bit 10, and this loader is running in
        // long mode, so both must be set. That is what makes the read
        // above a real answer rather than a zero the VMM invented.
        check_equal(state,
                    "msr.ia32_efer.long_mode_active",
                    "bit10",
                    1,
                    (g_probe_value >> 10) & 1);

        // The VMX capability MSRs are inside the low range, so whether
        // they exit depends on the bitmap this build armed. What they
        // must not do is fault: they are architectural, and with VMX
        // hidden from CPUID a guest has no business reading them - but
        // the hardware underneath does implement them, so the honest
        // answer is whatever it says.
        g_probe_msr = ia32_vmx_basic;
        auto basic = probe(probe_rdmsr);
        emit(state,
             "msr.rdmsr.ia32_vmx_basic.no_fault",
             (no_fault == basic.vector) ? outcome::pass : outcome::fail,
             "vector",
             static_cast<std::uint64_t>(no_fault),
             static_cast<std::uint64_t>(basic.vector));
    }

    // === INVD and XSETBV ===============================================
    //
    // Both are in SDM 28.1.2's unconditional list
    // (.references/sdm.txt:200699), alongside CPUID and GETSEC.
    {
        auto measured = probe(probe_invd);
        check_equal(
            state, "exit.invd", "reason", exit_invd, measured.reason);
        check_equal(state,
                    "invd.does_not_fault",
                    "vector",
                    static_cast<std::uint64_t>(no_fault),
                    static_cast<std::uint64_t>(measured.vector));

        // XSETBV needs CR4.OSXSAVE in the *guest*, since without it the
        // instruction raises #UD before any exit is considered. The host
        // side of this VMM sets its own OSXSAVE before executing the
        // guest's request, which is a different register.
        //
        // Turned on here rather than skipped when the firmware left it
        // off. OSXSAVE is not in this VMM's CR4 guest/host mask, so the
        // write lands in the real register without exiting, and turning
        // it on is what makes exit reason 55 reachable at all - it was a
        // skip on every run before this. Restored below, so the firmware
        // gets back the CR4 it had.
        auto cr4_before_xsetbv = read_cr4();
        auto osxsave_supported = [] {
            std::uint32_t registers[4]{};
            cpuid(1, 0, registers);
            // Leaf 1 ECX bit 26 is XSAVE, which is what OSXSAVE enables
            // an operating system's use of. Without it CR4.OSXSAVE is a
            // reserved bit and setting it raises #GP.
            return 0 != (registers[2] & (1u << 26));
        }();

        if (osxsave_supported && !(cr4_before_xsetbv & cr4_os_xsave)) {
            write_cr4(cr4_before_xsetbv | cr4_os_xsave);
        }

        if (read_cr4() & cr4_os_xsave) {
            auto xsetbv = probe(probe_xsetbv);
            check_equal(state,
                        "exit.xsetbv",
                        "reason",
                        exit_xsetbv,
                        xsetbv.reason);
            check_equal(state,
                        "xsetbv.does_not_fault",
                        "vector",
                        static_cast<std::uint64_t>(no_fault),
                        static_cast<std::uint64_t>(xsetbv.vector));
        } else {
            emit(state,
                 "exit.xsetbv",
                 outcome::skip,
                 "no_xsave_in_cpuid_so_osxsave_is_reserved",
                 exit_xsetbv,
                 0);

            // The coverage table says this reason is covered by a case.
            // On a processor with no XSAVE the case cannot run, so the
            // table would be reporting a regression that is really an
            // absent feature. Corrected here rather than weakened there.
            state.note_why(exit_xsetbv,
                           "unreachable-here:no_xsave_in_cpuid_so_"
                           "cr4_osxsave_cannot_be_set");
        }

        // CR4 back exactly as the firmware had it.
        if (read_cr4() != cr4_before_xsetbv) {
            write_cr4(cr4_before_xsetbv);
        }
        check_equal(state,
                    "cr4.osxsave_restored",
                    "cr4",
                    cr4_before_xsetbv,
                    read_cr4());
    }

    // === Instructions that fault before they can exit ==================
    //
    // Three exit reasons whose instruction is in SDM Appendix C's
    // *unconditional* group - no VM-execution control turns them off -
    // and which this exit handler nevertheless has no case for. An exit
    // from any of them reaches `default:` and stops the processor, which
    // is the thing CLAUDE.md forbids a guest instruction from being able
    // to do.
    //
    // What stops it today is an exception with priority over the exit
    // (SDM 28.1.1, .references/sdm.txt:200675). Each case below measures
    // *that*, not the absence of the exit: the absence follows from the
    // fault, and it is the fault that would stop being true if the
    // conditions changed. So each asserts the vector as well as the
    // silence, and the conditions each one depends on are read from the
    // guest first.
    {
        struct faulting_case
        {
            const char * name;
            void (*body)();
            long long vector;
            const char * why;
        };

        // GETSEC is only probed while CR4.SMXE is clear, because that is
        // the whole of what makes it safe. With SMXE set the instruction
        // stops faulting, exits, and stops the processor - see
        // probe_getsec. The bit is read here rather than assumed, since
        // the firmware ran before this suite did.
        //
        // Note what this does *not* establish: CR4.SMXE is not in this
        // VMM's CR4 guest/host mask, so a guest on hardware that
        // implements SMX can set it for itself even though CPUID says
        // there is no SMX. That is a defect in the VMM rather than in the
        // test, and it is reported as one.
        constexpr std::uint64_t cr4_smxe = 1ull << 14;
        auto smx_enabled = 0 != (read_cr4() & cr4_smxe);

        // Static so it does not land on `run`'s stack frame: at
        // -O0 every local table here is copied there on entry, and
        // the total went past 4 KB, which makes the Microsoft ABI
        // ask for `__chkstk` - see the note on `names` below.
        static const faulting_case cases[]{
            {"rsm", probe_rsm, vector_invalid_opcode, "rsm_outside_smm"},
            {"encls",
             probe_encls,
             vector_invalid_opcode,
             "encls_without_sgx"},
        };

        for (const auto & entry : cases) {
            auto measured = probe(entry.body);

            char name[80]{};
            auto at = trace::append_text(name, "quiet.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".does_not_exit");
            *at = 0;
            emit(state,
                 name,
                 (no_exit_reason == measured.reason) ? outcome::pass
                                                     : outcome::fail,
                 entry.why,
                 no_exit_reason,
                 measured.reason);

            at = trace::append_text(name, "quiet.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".invalid_opcode");
            *at = 0;
            check_equal(state,
                        name,
                        "vector",
                        entry.vector,
                        static_cast<std::uint64_t>(measured.vector));
        }

        if (smx_enabled) {
            // Unreachable now, and left in place as an alarm.
            //
            // CR4.SMXE is in the guest/host mask and answered clear in
            // the read shadow, so a guest cannot see the bit set - not
            // from the firmware, which the VMM strips it from, and not
            // from its own write, which the handler refuses. Reaching
            // here means one of those two stopped being true, and the
            // disposition below fails the job rather than letting the
            // suite skip quietly past it.
            emit(state,
                 "quiet.getsec.does_not_exit",
                 outcome::skip,
                 "cr4_smxe_is_set_and_getsec_would_exit_to_default",
                 no_exit_reason,
                 1);
            // UNEXPLAINED rather than a reason, deliberately: the
            // suite genuinely does not know why the bit is set, and
            // saying anything else would be inventing one. The report
            // fails on it.
            state.note_why(exit_getsec, "UNEXPLAINED");
        } else {
            auto measured = probe(probe_getsec);
            emit(state,
                 "quiet.getsec.does_not_exit",
                 (no_exit_reason == measured.reason) ? outcome::pass
                                                     : outcome::fail,
                 "getsec_with_cr4_smxe_clear",
                 no_exit_reason,
                 measured.reason);
            check_equal(state,
                        "quiet.getsec.invalid_opcode",
                        "vector",
                        vector_invalid_opcode,
                        static_cast<std::uint64_t>(measured.vector));
        }
    }

    // === Instructions that must NOT exit ===============================
    //
    // The other half of exit-reason coverage, and the half a
    // reached-reason count cannot show.
    //
    // Every instruction below exits *conditionally*, on a VM-execution
    // control this VMM does not set - and for every one of them the exit
    // handler has no case, so turning the control on would send the
    // instruction to `default:` and stop the processor. CLAUDE.md states
    // that as a rule: nothing a guest can execute may reach `default:`.
    //
    // check-exit-handler.sh states the rule at the source. These state it
    // at the machine: the configuration really is what the source says,
    // on a processor really running under this VMM. A control turned on
    // by accident - forced through `adjust_msr` by a capability MSR, say,
    // which is exactly how 5531fdc's preemption timer arrived - fails
    // here cleanly instead of wedging a processor on a rig.
    //
    // Some of these fault instead on a processor that does not implement
    // them. A fault is not a failure of the claim: the claim is only that
    // no VM exit was taken.
    //
    // HLT is deliberately absent. HLT exiting is off, so the instruction
    // would do what a guest asked and halt - and these run with
    // interrupts disabled, so nothing would end it. The one instruction
    // whose negative cannot be asserted from inside the guest that would
    // be stopped by asserting it.
    {
        struct negative_case
        {
            const char * name;
            void (*body)();
            const char * control;
        };

        // Static so it does not land on `run`'s stack frame: at
        // -O0 every local table here is copied there on entry, and
        // the total went past 4 KB, which makes the Microsoft ABI
        // ask for `__chkstk` - see the note on `names` below.
        static const negative_case cases[]{
            {"rdtsc", probe_rdtsc, "primary_bit_12_rdtsc_exiting"},
            {"rdtscp", probe_rdtscp, "primary_bit_12_rdtsc_exiting"},
            {"invlpg", probe_invlpg, "primary_bit_9_invlpg_exiting"},
            {"pause", probe_pause, "primary_bit_30_pause_exiting"},
            {"mov_from_dr",
             probe_mov_from_dr,
             "primary_bit_23_mov_dr_exiting"},
            {"rdpmc", probe_rdpmc, "primary_bit_11_rdpmc_exiting"},
            {"wbinvd", probe_wbinvd, "secondary_bit_6_wbinvd_exiting"},
            {"invpcid", probe_invpcid, "secondary_bit_12_enable_invpcid"},
            {"rdrand", probe_rdrand, "secondary_bit_11_rdrand_exiting"},
            {"rdseed", probe_rdseed, "secondary_bit_16_rdseed_exiting"},
            {"xgetbv", probe_xgetbv, "no_control_xgetbv_never_exits"},

            // The four descriptor-table instructions, which are reasons
            // 46 and 47 and share one control - secondary bit 2, SDM
            // Table 27-7 (.references/sdm.txt:199537). Both halves are
            // probed because the two reasons are separate: SGDT and SIDT
            // produce 46, SLDT and STR produce 47, and a control turned
            // on would send both to `default:`.
            {"sgdt", probe_sgdt, "secondary_bit_2_descriptor_table"},
            {"sidt", probe_sidt, "secondary_bit_2_descriptor_table"},
            {"sldt", probe_sldt, "secondary_bit_2_descriptor_table"},
            {"str", probe_str, "secondary_bit_2_descriptor_table"},

            // XSAVES and XRSTORS, reasons 63 and 64. Their control -
            // "enable XSAVES/XRSTORS", secondary bit 20 - *is* requested
            // by setup_vmcs, so unlike everything else here the
            // instruction is enabled rather than disabled. What keeps it
            // from exiting is the second condition: an exit needs a bit
            // set in the logical AND of EDX:EAX, IA32_XSS and the
            // XSS-exiting bitmap (.references/sdm.txt:201349), and this
            // VMM never writes that bitmap, so it is zero.
            //
            // A fault here is not a failure of the claim - on a processor
            // without XSAVES, or with CR4.OSXSAVE clear, both raise #UD -
            // and the claim is only that no VM exit was taken.
            {"xsaves", probe_xsaves, "xss_exiting_bitmap_is_zero"},
            {"xrstors", probe_xrstors, "xss_exiting_bitmap_is_zero"},
        };

        for (const auto & entry : cases) {
            auto measured = probe(entry.body);

            char name[80]{};
            auto at = trace::append_text(name, "quiet.");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at, ".does_not_exit");
            *at = 0;

            emit(state,
                 name,
                 (no_exit_reason == measured.reason) ? outcome::pass
                                                     : outcome::fail,
                 entry.control,
                 no_exit_reason,
                 measured.reason);
        }
    }

    // === MONITOR and MWAIT =============================================
    //
    // The only two exit reasons in this suite that are reachable *because
    // the test build asks for them*. Both controls are off in a deployed
    // build, and the measurement that turned them off is in setup_vmcs:
    // a UEFI firmware parks its application processors in
    // `monitor; mwait; jmp`, and this VMM adopts those processors, so all
    // seven sat in that loop taking 1,190,000 exits each in under two
    // minutes.
    //
    // The handler's case for them therefore cannot execute in any build
    // anyone deploys - it is neither exercised nor removed. Turning the
    // controls on under ZPP_GUEST_TESTS makes it live for exactly as long
    // as this suite runs, which is the only sound way to test it: the
    // alternative is a case that has never once executed protecting a
    // path a guest can reach the moment somebody flips the constant back.
    //
    // SDM 28.1.3 lists both as conditional on their controls, so with the
    // controls off these two would join the negatives above - which is
    // what they were before this.
    {
        auto monitor = probe(probe_monitor);
        check_equal(
            state, "exit.monitor", "reason", exit_monitor, monitor.reason);
        check_equal(state,
                    "monitor.does_not_fault",
                    "vector",
                    static_cast<std::uint64_t>(no_fault),
                    static_cast<std::uint64_t>(monitor.vector));

        auto mwait = probe(probe_mwait);
        check_equal(
            state, "exit.mwait", "reason", exit_mwait, mwait.reason);
        check_equal(state,
                    "mwait.does_not_fault",
                    "vector",
                    static_cast<std::uint64_t>(no_fault),
                    static_cast<std::uint64_t>(mwait.vector));

        // And the thing that makes running MWAIT with interrupts disabled
        // survivable: the handler resumed *past* it rather than letting
        // the processor enter the wait. If it had not, nothing below this
        // line would run and the harness would report no DONE line.
        emit(state,
             "mwait.resumed_past",
             outcome::pass,
             "reaching_this_line_is_the_assertion",
             1,
             1);
    }

    // === EPT and the store emulation ===================================
    //
    // The local APIC page is the one page this VMM takes write permission
    // away from in the extended page tables, so a guest store to it takes
    // an EPT violation, the store is decoded out of the guest's
    // instruction stream and applied. That is the only path here that
    // exercises EPT, the instruction decoder and the write filter at
    // once, and it is reachable from a guest with nothing but a MOV.
    //
    // The task priority register is used rather than the interrupt
    // command register on purpose. SDM 13.4.1 says a local APIC register
    // must be accessed with an aligned 32-bit access and that "any access
    // that touches bytes 4 through 15 of an APIC register may cause
    // undefined behavior", so a narrow store to the command register is
    // undefined at the hardware, and the filter's own gate against
    // composing a command out of one is not observable from here without
    // sending an interrupt. Writing the task priority instead exercises
    // the same decode and apply path against a register whose value can
    // simply be read back.
    {
        auto apic_base = read_msr(ia32_apic_base);
        constexpr std::uint64_t apic_base_mask = 0xfffff000ull;
        constexpr std::uint64_t apic_enabled = 1ull << 11;
        constexpr std::uint64_t apic_extended = 1ull << 10;
        constexpr std::uint64_t task_priority = 0x80;

        if (!(apic_base & apic_enabled) || (apic_base & apic_extended)) {
            // x2APIC reaches the same registers through MSRs, so there is
            // no page to take a violation on. Reported rather than
            // silently omitted.
            emit(state,
                 "ept.local_apic_store",
                 outcome::skip,
                 "apic_not_in_xapic_mode",
                 exit_ept_violation,
                 apic_base);

            // Both reasons this block is the only source of. Neither is a
            // regression when there is no page to fault on, so the
            // coverage table's "a case covers this" is corrected to what
            // actually happened.
            state.note_why(exit_ept_violation,
                           "unreachable-here:apic_not_in_xapic_mode_so_"
                           "there_is_no_watched_page");
            state.note_why(exit_monitor_trap_flag,
                           "unreachable-here:apic_not_in_xapic_mode_so_"
                           "nothing_is_stepped");
        } else {
            auto * page = reinterpret_cast<volatile std::uint8_t *>(
                apic_base & apic_base_mask);
            auto * priority = reinterpret_cast<volatile std::uint32_t *>(
                page + task_priority);

            auto original = *priority;

            // A four-byte aligned store: the shape SDM 13.4.1 requires,
            // and the one the filter recognises as a register access.
            g_probe_dword = priority;
            g_probe_value = 0x20;
            auto aligned = probe(probe_store_dword);

            check_equal(state,
                        "exit.ept_violation",
                        "aligned_dword_store_reason",
                        exit_ept_violation,
                        aligned.reason);

            // The qualification's bit 1 is "write access". SDM Table
            // 28-7, "Exit Qualification for EPT Violations".
            check_equal(state,
                        "ept.qualification_write",
                        "bit1",
                        1,
                        (aligned.qualification >> 1) & 1);

            // And the store actually landed, which is what says the
            // decoder read the right instruction and apply_guest_store
            // wrote the right bytes rather than the exit merely
            // being taken and dropped.
            check_equal(state,
                        "ept.aligned_store_applied",
                        "task_priority",
                        0x20,
                        *priority & 0xff);

            // The same store in a form the decoder refuses, which is the
            // only way a guest can reach exit reason 37.
            //
            // The VMM has two answers to a write it has taken permission
            // away for. Where it can decode the instruction it carries
            // the write out itself and advances RIP - that is the case
            // above, and it costs one exit. Where it cannot, it opens the
            // page, sets the monitor trap flag, lets the guest's own
            // instruction run, and takes a second exit one instruction
            // later to close the page again. That second path is live in
            // every deployed build, its handler stops the processor if it
            // is ever entered without a step in progress, and until this
            // case nothing in the tree executed it.
            //
            // STOSD is the refusal: the decoder answers no string
            // instruction (see probe_store_dword_stos). So the same
            // dword, to the same register, takes a *different* route
            // through the VMM, and the two routes are told apart by how
            // far the exit count moved - two for the decoded store, three
            // for the stepped one.
            //
            // SDM Appendix C reason 37 is "A VM exit occurred due to the
            // 1-setting of the 'monitor trap flag' VM-execution control"
            // (.references/sdm.txt:224338), and the flag is set by
            // nothing here except that fallback.
            g_probe_dword = priority;
            g_probe_value = 0x30;
            auto stepped = probe(probe_store_dword_stos);

            // Three exits: this probe's opening reading, the EPT
            // violation the store took, and the monitor-trap-flag exit
            // after the step. A delta of two would mean the decoder
            // answered STOSD after all and the case is measuring the
            // wrong path.
            check_equal(state,
                        "ept.stos_store_is_stepped",
                        "exit_count_delta",
                        3,
                        stepped.delta);

            // And the newest of the three is the trap, not the
            // violation, which is what says the step completed rather
            // than the guest being left on the faulting instruction.
            emit(state,
                 "exit.monitor_trap_flag",
                 ((3 == stepped.delta) &&
                  (exit_monitor_trap_flag == stepped.newest))
                     ? outcome::pass
                     : outcome::fail,
                 "newest_reason_after_a_stepped_store",
                 exit_monitor_trap_flag,
                 stepped.newest);

            if ((3 == stepped.delta) &&
                (exit_monitor_trap_flag == stepped.newest)) {
                // Measured, so recorded. `probe` only records a reason
                // for a delta of two, deliberately - the encoding it
                // hands back for anything else is a diagnostic rather
                // than a reason - so this one is noted by the case that
                // knows what the three exits were.
                state.note_exit(exit_monitor_trap_flag);
            }

            // The step has to have retired the instruction, not just
            // taken the trap. on_monitor_trap_flag reports nothing to the
            // watch when RIP did not advance past the store, so a value
            // that did not land is the shape of that failure, and it is
            // the shape a guest would see as a device register that
            // silently ignored a write.
            check_equal(state,
                        "ept.stos_store_applied",
                        "task_priority",
                        0x30,
                        *priority & 0xff);

            // Nothing faulted. The page was opened and closed around one
            // instruction, and a guest that took a fault out of that
            // sequence would be a guest whose own store had been turned
            // into an exception by a VMM it cannot see.
            check_equal(state,
                        "ept.stos_store_does_not_fault",
                        "vector",
                        static_cast<std::uint64_t>(no_fault),
                        static_cast<std::uint64_t>(stepped.vector));

            // A one-byte store to the same register is *not* attempted,
            // and the reason is the emulator rather than the VMM.
            //
            // Bochs' local APIC model refuses any access that is not four
            // bytes wide - `>>PANIC<< APIC write with len=1 (should be
            // 4)` - and a panic ends the run, taking every case after it
            // with it. That is Bochs agreeing with SDM 13.4.1
            // ("all 32-bit registers should be accessed using 128-bit
            // aligned 32-bit loads or stores") more strictly than real
            // hardware does, which is a reasonable thing for a model to
            // do and leaves this untestable here.
            //
            // Not a gap in coverage of the narrow-store path, only in
            // coverage of it *end to end*: tests/watched_page drives
            // `carry_out_guest_instruction` at every width against the
            // real function, hosted, with no emulator to object.
            emit(state,
                 "ept.byte_store_applied",
                 outcome::skip,
                 "bochs_lapic_model_panics_on_len_1",
                 0x30,
                 0);

            // Put the register back before anything else runs.
            g_probe_dword = priority;
            g_probe_value = original;
            static_cast<void>(probe(probe_store_dword));

            check_equal(state,
                        "ept.task_priority_restored",
                        "task_priority",
                        original,
                        *priority);

            // A read of the same page must *not* take a violation: only
            // write permission is taken away, and a VMM that removed read
            // permission as well would exit on every poll a guest makes
            // of its own interrupt controller. Measured rather than
            // assumed, because "reads are free" is exactly the kind of
            // claim that stops being true when a page is re-armed with
            // the wrong mode.
            g_probe_dword = priority;
            auto read_only = probe([] { g_probe_value = *g_probe_dword; });
            check_equal(state,
                        "ept.read_does_not_exit",
                        "reason",
                        no_exit_reason,
                        read_only.reason);
        }
    }

    // === I/O ===========================================================
    //
    // The I/O bitmaps are enabled and exactly one port is armed in them:
    // the ACPI sleep control register, so that a guest asking the
    // platform to power down can be seen doing it. Reading that port is
    // harmless - the sleep only happens on a write with the enable bit -
    // and it is the only way to reach exit reason 30 from here.
    {
        auto & found = zpp::sleep_control_finder::found;

        if (found.usable()) {
            g_probe_value = found.pm1a_control_port;
            auto measured = probe(probe_in_16);
            check_equal(state,
                        "exit.io_instruction",
                        "in_from_pm1a_control",
                        exit_io_instruction,
                        measured.reason);
        } else {
            // Not a failure: Bochs' own firmware path may present no
            // FADT at all, and a port that was never armed cannot exit.
            // Reported so the coverage report can say why the reason is
            // missing rather than leaving it blank.
            emit(state,
                 "exit.io_instruction",
                 outcome::skip,
                 "no_acpi_sleep_control_port_found",
                 exit_io_instruction,
                 0);

            // Exactly one port is armed in the I/O bitmaps, and it is the
            // one the FADT names. Where there is no FADT there is no
            // armed port, and with "use I/O bitmaps" set and every bit
            // clear no I/O instruction exits at all - SDM Appendix C
            // reason 30 (.references/sdm.txt:224310) makes the bitmap the
            // only condition once unconditional I/O exiting is off.
            state.note_why(exit_io_instruction,
                           "unreachable-here:no_pm1a_control_port_in_the_"
                           "fadt_so_no_port_is_armed");
        }
    }

    // === Controls the processor does not even offer ====================
    //
    // Two exit reasons have no instruction behind them: 62, the
    // page-modification log filling up, and 66, an SPP miss or
    // misconfiguration. Nothing a guest executes can produce either, so
    // the only honest thing a guest-side suite can say about them is
    // whether the machinery exists at all - and that is readable, because
    // the VMX capability MSRs are architectural and this guest can read
    // them.
    //
    // IA32_VMX_PROCBASED_CTLS2's high half is the allowed-1 settings of
    // the secondary controls (SDM Appendix A.3.3). Bit 17 is "Enable
    // PML" and bit 23 is "Sub-page write permissions for EPT" (SDM Table
    // 27-7, .references/sdm.txt:199589 and :199603), so bits 49 and 55 of
    // the MSR say whether this processor allows them to be set at all.
    //
    // A processor that does not allow the control settles the question
    // outright. One that does leaves the weaker answer - that setup_vmcs
    // does not ask for it - and the table below says so rather than
    // claiming more than was measured.
    {
        constexpr std::uint32_t ia32_vmx_procbased_ctls2 = 0x48b;
        constexpr std::uint64_t allowed_enable_pml = 1ull << 49;
        constexpr std::uint64_t allowed_sub_page_write = 1ull << 55;

        g_probe_msr = ia32_vmx_procbased_ctls2;
        auto capability = probe(probe_rdmsr);

        if (no_fault != capability.vector) {
            // No secondary controls at all on this processor, which is a
            // stronger statement than either bit would have been: every
            // secondary control is unavailable, PML and SPP included.
            emit(state,
                 "vmx.procbased_ctls2.readable",
                 outcome::skip,
                 "no_secondary_controls_on_this_processor",
                 0,
                 static_cast<std::uint64_t>(capability.vector));

            state.note_why(exit_page_modification_log_full,
                           "unreachable-here:this_processor_has_no_"
                           "secondary_vm_execution_controls");
            state.note_why(exit_spp_related_event,
                           "unreachable-here:this_processor_has_no_"
                           "secondary_vm_execution_controls");
        } else {
            auto allowed = g_probe_value;

            emit(state,
                 "vmx.procbased_ctls2.readable",
                 allowed ? outcome::pass : outcome::fail,
                 "ia32_vmx_procbased_ctls2",
                 1,
                 allowed);

            if (!(allowed & allowed_enable_pml)) {
                state.note_why(exit_page_modification_log_full,
                               "unreachable-here:ia32_vmx_procbased_"
                               "ctls2_bit49_enable_pml_is_not_allowed");
            }

            if (!(allowed & allowed_sub_page_write)) {
                state.note_why(exit_spp_related_event,
                               "unreachable-here:ia32_vmx_procbased_"
                               "ctls2_bit55_sub_page_write_not_allowed");
            }
        }
    }

    // === Nested VMX, and the one thing only a guest can witness ======
    //
    // Run outside the window below, with the firmware's own interrupt
    // descriptor table back and interrupts enabled, because
    // `verify_nested::present` allocates pages through boot services -
    // and a firmware call made with a foreign IDT installed and
    // interrupts disabled is what stopped an earlier version of this
    // suite part way through its fourth result line.
    //
    // It installs its own gate for the vector it injects, so it does not
    // need this suite's table and does not care that it has been put
    // back.
    //
    // Compiled in unconditionally and self-gating: with ZPP_NESTED_VMX
    // off the guest is told there is no VMX, `present` says so and
    // returns true. So this reports a skip on the build CI runs by
    // default, and a real answer on the one built with nesting on.
    write_idtr(firmware_idtr);
    asm volatile("sti" : : : "memory");

    {
        std::uint32_t registers[4]{};
        cpuid(1, 0, registers);
        auto vmx_offered = 0 != (registers[2] & (1u << 5));

        if (!vmx_offered) {
            emit(state,
                 "nested.injection_retires",
                 outcome::skip,
                 "vmx_not_offered_build_with_ZPP_NESTED_VMX_ON",
                 1,
                 0);
        } else {
            auto before = ::zpp_probe_l2_injections;
            auto ok = zpp::verify_nested::present(system_table);
            auto delivered = ::zpp_probe_l2_injections - before;

            // **The measurement.** Everything else about injection is
            // checkable from outside the guest and is checked, hosted, in
            // tests/nested_exit and check-exit-handler.sh: the entry
            // decision, the triple copied from vmcs12 into vmcs02, a
            // halted guest entered rather than parked. None of it
            // establishes that the injected event *retires*, and the two
            // outcomes leave the same vmcs12 behind - SDM 30.2 clears the
            // valid bit on every exit, so a delivery and a silent drop
            // are indistinguishable to the layer that asked for it.
            //
            // The only witness is the second-level guest. An injecting
            // entry delivers before the first instruction at its RIP,
            // so a handler that ran is proof.
            // A count alone is not the measurement, and taking it for
            // one gave a wrong pass on the first run. The handler is
            // reached through the guest hypervisor's own interrupt
            // descriptor table - the second-level guest is entered with
            // this processor's IDTR - so anything delivered on that
            // vector in *either* world reaches it. The witness has to be
            // where it fired.
            //
            // The second-level guest has exactly one RIP: its entry
            // point. An injecting entry delivers before the first
            // instruction there, so a handler that interrupted that
            // address ran inside the guest and one that interrupted any
            // other address did not.
            auto entry_point =
                reinterpret_cast<std::uint64_t>(&zpp_probe_l2_entry);
            auto retired = (1 == delivered) &&
                           (::zpp_probe_l2_interrupted_rip == entry_point);

            emit(state,
                 "nested.injection_retires",
                 retired ? outcome::unexpected_pass
                         : outcome::expected_failure,
                 "injected_interrupt_reaches_the_second_level_guest",
                 entry_point,
                 ::zpp_probe_l2_interrupted_rip);

            // **Which layer refused it.**
            //
            // Reason 33 with bit 31 is what `enter_or_park_l2`'s
            // `refuse` synthesises *and* what a processor produces for a
            // guest state it rejects, so the reason reflected into
            // vmcs12 cannot tell them apart - and they want opposite
            // fixes. What separates them is the VMM's own exit ring,
            // sampled the instant the launch came back: a refusal made
            // in software leaves the VMLAUNCH itself as the last exit,
            // reason 20, because the VMM never entered. A refusal made
            // by the processor means the VMM's *own* VM entry failed,
            // and its last exit carries bit 31.
            //
            // Measured: 0x80000021. **The processor refused vmcs02.**
            // The VMM's reflection of it to the guest hypervisor is
            // therefore correct - the entry really did fail - and what
            // is wrong is the guest state built into vmcs02, which the
            // injection exposes rather than causes. All three `refuse`
            // sites are activity-state conditions and this vmcs12 says
            // active, so the code agrees with the measurement.
            constexpr std::uint32_t entry_failure_bit = 1u << 31;
            constexpr std::uint32_t vmlaunch_reason = 20;

            auto refused_by_hardware =
                0 != (::zpp_probe_ring_reason & entry_failure_bit);
            auto refused_by_us =
                vmlaunch_reason == (::zpp_probe_ring_reason & 0xffff);

            // The one place in this suite where an entry-failure reason
            // is *observed* rather than reasoned about. The exit ring
            // carries 0x80000021 - bit 31 plus basic reason 33 - and the
            // coverage table's out-of-scope note for 33 says a failed
            // entry means the guest does not run, which is exactly why
            // this reading had to come from the VMM's own ring rather
            // than from a probe. Recorded so a ZPP_NESTED_VMX build's
            // coverage report tells the truth about it; the default build
            // never gets here, so the note stands there.
            if (refused_by_hardware) {
                state.note_exit(exit_entry_invalid_guest_state);
                state.note_why(
                    exit_entry_invalid_guest_state,
                    "covered:nested.injection_refused_by_hardware");
            }

            emit(state,
                 "nested.injection_refused_by_hardware",
                 refused_by_hardware ? outcome::pass : outcome::fail,
                 "the_vmms_own_entry_of_vmcs02_failed",
                 entry_failure_bit,
                 ::zpp_probe_ring_reason);

            emit(state,
                 "nested.injection_not_refused_in_software",
                 refused_by_us ? outcome::fail : outcome::pass,
                 "enter_or_park_l2_did_not_refuse",
                 0,
                 refused_by_us ? vmlaunch_reason : 0);

            // The probe as a whole, which fails today for the same
            // reason: its third launch is the injecting one.
            emit(state,
                 "nested.probe_passed",
                 ok ? outcome::unexpected_pass : outcome::expected_failure,
                 "verify_nested_present_fails_on_the_injecting_launch",
                 1,
                 ok ? 1 : 0);
        }
    }

    // === Nothing else exits ============================================
    //
    // Three exit reasons this suite cannot produce on purpose, and the
    // reason it does not have to: none of them has a case in the exit
    // handler, so a single one of them would reach `default:`, stop this
    // processor, and there would be no verdict at all.
    //
    // Reaching this line is therefore the measurement, and it is a strong
    // one, because of *where* the line is. Interrupts have been enabled
    // since the nested probe above, which called into boot services -
    // firmware that services timer interrupts and waits on events. So the
    // run has spent real time with RFLAGS.IF set and interrupts arriving:
    //
    // - External-interrupt exiting would have produced reason 1 at the
    //   first tick. SDM Appendix C: "External interrupt. An external
    //   interrupt arrived and the 'external-interrupt exiting'
    //   VM-execution control was 1" (.references/sdm.txt:224276 area).
    // - Interrupt-window exiting produces reason 7 "before execution of
    //   any instruction if RFLAGS.IF = 1 and there is no blocking of
    //   events by STI or by MOV SS" (.references/sdm.txt:200976), which
    //   with interrupts enabled is nearly every instruction.
    // - NMI-window exiting produces reason 8 "before execution of any
    //   instruction if there is no virtual-NMI blocking"
    //   (.references/sdm.txt:200983), which does not even need interrupts
    //   enabled - it would have ended the run before its first case.
    //
    // A negative asserted by having survived is worth less than one
    // asserted by a measurement, and these are the two places in this
    // suite where that is the only assertion available: an exit whose
    // handler stops the processor cannot be probed for, because probing
    // for it is what stops the processor.
    {
        emit(state,
             "quiet.external_interrupt.control_off",
             outcome::pass,
             "reached_with_interrupts_enabled_and_reason_1_has_no_case",
             0,
             state.observed_exit[exit_external_interrupt] ? 1 : 0);

        emit(state,
             "quiet.interrupt_window.control_off",
             outcome::pass,
             "reached_with_interrupts_enabled_and_reason_7_has_no_case",
             0,
             state.observed_exit[exit_interrupt_window] ? 1 : 0);

        emit(state,
             "quiet.nmi_window.control_off",
             outcome::pass,
             "the_whole_run_completed_and_reason_8_has_no_case",
             0,
             state.observed_exit[exit_nmi_window] ? 1 : 0);
    }

    // === What was and was not reached ==================================
    //
    // Printed from what the run measured rather than from the list of
    // cases above, so a case whose instruction quietly stopped exiting
    // shows up as a gap instead of as a pass.
    //
    // Every reason carries a **disposition** beside it, and that is the
    // durable half of this report. A list of reasons that were not
    // reached is a question; a list of reasons that were not reached,
    // each with the recorded reason it could not be, is an answer that
    // stays answered. Three dispositions are allowed:
    //
    //   covered:<case>          a case in this suite reaches it, and the
    //                           case is named. Absent means a regression:
    //                           something that used to exit stopped, and
    //                           the harness fails the run.
    //   unreachable-here:<why>  it cannot be produced in this
    //                           environment, and the run measured the
    //                           thing that makes it so. The measurement
    //                           is named, not the conclusion.
    //   out-of-scope:<why>      it is reachable and this suite
    //                           deliberately does not reach it, with the
    //                           reasoning.
    //
    // and one that is not allowed:
    //
    //   UNEXPLAINED             nobody has said. The harness fails on it,
    //                           which is the whole mechanism: the list
    //                           cannot grow silently, because a new
    //                           reason with no disposition is a red run.
    //
    // A few dispositions cannot be decided until the run happens - see
    // session::why - and those are written by the case that discovers
    // them. The static text below is what stands otherwise.
    {
        struct reason_name
        {
            std::uint32_t reason;
            const char * name;
            const char * disposition;
        };

        // SDM Vol. 3D Appendix C, "VMX Basic Exit Reasons"
        // (.references/sdm.txt:224280 onwards). Every reason this VMM's
        // handler has a case for, plus the ones it does not, because the
        // list of what is not covered is the point.
        // Static, and that is a build constraint rather than a style
        // choice: at -O0 a non-static local array is materialised on the
        // stack every time the function is entered, and this one is
        // sixty entries of three pointers. Adding the dispositions took
        // `run`'s frame past 4 KB, which on the Microsoft ABI makes the
        // compiler emit a call to `__chkstk` - a stack probe helper that
        // comes from a C runtime this loader does not link. The link
        // fails with an undefined symbol rather than anything that points
        // at a frame size, so it is worth naming here.
        static constexpr reason_name names[]{
            // The only NMI source a single-processor guest has is an IPI
            // to itself, and this VMM emulates the interrupt command
            // register write from *root* operation - so the NMI it
            // produces is delivered to the host, not to the guest, and no
            // reason 0 is taken. Driving it down the stepping path
            // instead would deliver in non-root operation, but then the
            // NMI arrives asynchronously: guest_tests.S disarms its
            // recovery point on the way out of every probe and parks the
            // processor on a fault taken outside one, so an NMI landing a
            // few instructions late ends the run rather than reporting.
            {0,
             "exception_or_nmi",
             "out-of-scope:the_only_self_nmi_route_is_an_icr_write_this_"
             "vmm_emulates_in_root_operation"},
            {1,
             "external_interrupt",
             "unreachable-here:no_case_so_the_run_reaching_its_end_with_"
             "interrupts_enabled_is_the_measurement"},
            // Reachable, and reaching it ends everything: a triple fault
            // has no case either, so it stops the processor - which is
            // the right answer to a guest that has destroyed itself, and
            // the wrong thing for a suite that has cases left to report.
            {2,
             "triple_fault",
             "out-of-scope:reaching_it_ends_the_run_it_would_be_reported_"
             "in"},
            // An INIT is only observable by the processor receiving it,
            // and this suite has one processor. Sending it to itself is
            // the emulated reset of the guest that is running the suite.
            {3,
             "init_signal",
             "out-of-scope:an_init_to_this_processor_resets_the_guest_"
             "running_the_suite"},
            // SDM Appendix C reason 4 is a start-up IPI, which a
            // processor only accepts in the wait-for-SIPI activity state
            // (.references/sdm.txt:163861). A processor executing this
            // suite is by definition not in it.
            {4,
             "start_up_ipi",
             "unreachable-here:only_delivered_in_the_wait_for_sipi_state_"
             "which_a_running_processor_is_not_in"},
            {7,
             "interrupt_window",
             "unreachable-here:no_case_so_the_run_reaching_its_end_with_"
             "interrupts_enabled_is_the_measurement"},
            {8,
             "nmi_window",
             "unreachable-here:no_case_and_it_fires_every_instruction_so_"
             "the_run_completing_is_the_measurement"},
            // "Hardware task switches are not supported in IA-32e mode"
            // (.references/sdm.txt:153161), and this guest is in it -
            // msr.ia32_efer.long_mode_active measures LMA directly.
            // Leaving long mode to reach the reason is a guest this
            // suite is not.
            {9,
             "task_switch",
             "unreachable-here:no_hardware_task_switch_in_long_mode_and_"
             "msr.ia32_efer.long_mode_active_measures_it"},
            {10, "cpuid", "covered:exit.cpuid"},
            {11,
             "getsec",
             "unreachable-here:cr4_smxe_is_masked_and_answered_clear_so_"
             "quiet.getsec.invalid_opcode_measures_the_ud_instead"},
            // HLT exiting is off, so the instruction does what a guest
            // asked and halts. Asserting that from inside the guest means
            // executing it, and the only things that end a halt are an
            // interrupt - which this suite disables - or an NMI from
            // another processor, of which there is one.
            {12,
             "hlt",
             "out-of-scope:asserting_it_requires_halting_the_processor_"
             "the_assertion_runs_on"},
            {13, "invd", "covered:exit.invd"},
            {14,
             "invlpg",
             "unreachable-here:quiet.invlpg.does_not_exit_measures_"
             "invlpg_exiting_off"},
            {15,
             "rdpmc",
             "unreachable-here:quiet.rdpmc.does_not_exit_measures_rdpmc_"
             "exiting_off"},
            {16,
             "rdtsc",
             "unreachable-here:quiet.rdtsc.does_not_exit_measures_rdtsc_"
             "exiting_off"},
            {17,
             "rsm",
             "unreachable-here:quiet.rsm.invalid_opcode_measures_the_ud_"
             "rsm_takes_outside_smm"},
            {18, "vmcall", "covered:vmx.vmcall.exit_reason"},
            {19, "vmclear", "covered:vmx.vmclear.exit_reason"},
            {20, "vmlaunch", "covered:vmx.vmlaunch.exit_reason"},
            {21, "vmptrld", "covered:vmx.vmptrld.exit_reason"},
            {22, "vmptrst", "covered:vmx.vmptrst.exit_reason"},
            {23, "vmread", "covered:vmx.vmread.exit_reason"},
            {24, "vmresume", "covered:vmx.vmresume.exit_reason"},
            {25, "vmwrite", "covered:vmx.vmwrite.exit_reason"},
            {26, "vmxoff", "covered:vmx.vmxoff.exit_reason"},
            {27, "vmxon", "covered:vmx.vmxon.exit_reason"},
            {28,
             "control_register_access",
             "covered:exit.control_register_access"},
            {29,
             "mov_debug_register",
             "unreachable-here:quiet.mov_from_dr.does_not_exit_measures_"
             "mov_dr_exiting_off"},
            {30, "io_instruction", "covered:exit.io_instruction"},
            {31, "rdmsr", "covered:msr.rdmsr.hyperv_frequency"},
            {32, "wrmsr", "covered:msr.wrmsr.hyperv_frequency"},
            // A VM entry that fails leaves the guest not running, so
            // there is nothing inside it to report the failure. The one
            // place this suite sees reason 33 at all is the nested probe,
            // which reads the VMM's own ring after a launch it made on
            // the guest's behalf - and that is a ZPP_NESTED_VMX build.
            {33,
             "entry_invalid_guest_state",
             "out-of-scope:a_failed_entry_means_the_guest_does_not_run_"
             "see_nested.injection_refused_by_hardware"},
            {34,
             "entry_failure_msr_loading",
             "out-of-scope:the_vm_entry_msr_load_count_is_zero_and_a_"
             "failed_entry_stops_the_processor"},
            {36, "mwait", "covered:exit.mwait"},
            {37, "monitor_trap_flag", "covered:exit.monitor_trap_flag"},
            {39, "monitor", "covered:exit.monitor"},
            {40,
             "pause",
             "unreachable-here:quiet.pause.does_not_exit_measures_pause_"
             "exiting_off"},
            // Reason 43 needs "use TPR shadow", and 44 needs "virtualize
            // APIC accesses". The second is measured rather than argued:
            // a store to the local APIC page took reason 48, and SDM
            // Appendix C says that with the control set the same access
            // would have taken reason 44 instead
            // (.references/sdm.txt:224345).
            {43,
             "tpr_below_threshold",
             "unreachable-here:use_tpr_shadow_off_and_the_apic_page_took_"
             "reason_48_not_44"},
            {44,
             "apic_access",
             "unreachable-here:exit.ept_violation_measured_reason_48_on_"
             "the_apic_page_so_it_is_not_virtualized"},
            // Virtualized EOI is performed by virtual-interrupt delivery,
            // and SDM 29.2.1.1 requires "external-interrupt exiting" to
            // be 1 whenever that control is 1
            // (.references/sdm.txt:202136). The run measured external
            // -interrupt exiting off, so this follows from it.
            {45,
             "virtualized_eoi",
             "unreachable-here:virtual_interrupt_delivery_requires_"
             "external_interrupt_exiting_which_is_off"},
            {46,
             "gdtr_or_idtr",
             "unreachable-here:quiet.sgdt_and_quiet.sidt_measure_"
             "descriptor_table_exiting_off"},
            {47,
             "ldtr_or_tr",
             "unreachable-here:quiet.sldt_and_quiet.str_measure_"
             "descriptor_table_exiting_off"},
            {48, "ept_violation", "covered:exit.ept_violation"},
            // Only this VMM writes an EPT entry, so a guest has no way to
            // construct a misconfigured one. Reaching this reason would
            // mean the tables this VMM built are wrong, which is a defect
            // rather than a case - and it has no handler, so it stops the
            // processor, which is the correct answer to that defect.
            {49,
             "ept_misconfiguration",
             "out-of-scope:only_this_vmm_writes_ept_entries_so_a_guest_"
             "cannot_construct_one"},
            {50, "invept", "covered:vmx.invept.exit_reason"},
            {51,
             "rdtscp",
             "unreachable-here:quiet.rdtscp.does_not_exit_measures_rdtsc_"
             "exiting_off"},
            // Armed by this VMM alone, for its own log polling, and never
            // by anything a guest executes. diag.exit_count_step measures
            // that none arrives: two consecutive readings differ by
            // exactly one, which a free-running timer would break.
            {52,
             "vmx_preemption_timer",
             "unreachable-here:armed_by_this_vmm_only_and_diag.exit_"
             "count_step_measures_none_arriving"},
            {53, "invvpid", "covered:vmx.invvpid.exit_reason"},
            {54,
             "wbinvd",
             "unreachable-here:quiet.wbinvd.does_not_exit_measures_"
             "wbinvd_exiting_off"},
            {55, "xsetbv", "covered:exit.xsetbv"},
            // APIC-register virtualization and virtualize-x2APIC mode are
            // the two controls that produce reason 56, and SDM 29.2.1.1
            // requires both to be 0 when "use TPR shadow" is 0
            // (.references/sdm.txt:202132).
            {56,
             "apic_write",
             "unreachable-here:apic_register_virtualization_requires_use_"
             "tpr_shadow_which_is_off"},
            {57,
             "rdrand",
             "unreachable-here:quiet.rdrand.does_not_exit_measures_"
             "rdrand_exiting_off"},
            {58,
             "invpcid",
             "unreachable-here:quiet.invpcid.does_not_exit_measures_"
             "invlpg_exiting_off"},
            {59,
             "vmfunc",
             "unreachable-here:vmx.vmfunc.does_not_exit_measures_enable_"
             "vm_functions_off_so_the_processor_faults"},
            {60,
             "encls",
             "unreachable-here:quiet.encls.invalid_opcode_measures_that_"
             "the_instruction_cannot_execute_here"},
            {61,
             "rdseed",
             "unreachable-here:quiet.rdseed.does_not_exit_measures_"
             "rdseed_exiting_off"},
            // No instruction produces these two, so the only guest-side
            // statement available is whether the processor offers the
            // control at all - which the run reads out of
            // IA32_VMX_PROCBASED_CTLS2 and writes into session::why when
            // it does not. What stands otherwise is weaker and says so.
            {62,
             "page_modification_log_full",
             "unreachable-here:enable_pml_not_requested_and_reason_62_"
             "has_no_case_so_the_run_would_have_stopped"},
            {63,
             "xsaves",
             "unreachable-here:quiet.xsaves.does_not_exit_measures_the_"
             "xss_exiting_bitmap_being_zero"},
            {64,
             "xrstors",
             "unreachable-here:quiet.xrstors.does_not_exit_measures_the_"
             "xss_exiting_bitmap_being_zero"},
            {66,
             "spp_related_event",
             "unreachable-here:sub_page_write_permissions_not_requested_"
             "and_no_ept_entry_asks_for_them"},
        };

        for (const auto & entry : names) {
            char line[trace::line_capacity]{};
            auto at = trace::append_text(line, "ZPPCOVER ");
            at = trace::append_decimal(at, entry.reason);
            at = trace::append_text(at, " ");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at,
                                    state.observed_exit[entry.reason]
                                        ? " observed "
                                        : " absent ");

            // What the run discovered wins over what the table assumed.
            // Only a handful of reasons ever have one - see session::why
            // - and every one of them is a case that could not run rather
            // than a claim being softened after the fact.
            auto discovered = state.why[entry.reason];
            at = trace::append_text(
                at, discovered ? discovered : entry.disposition);

            at = trace::append_text(at, "\r\n");
            *at = 0;
            trace::raw(line);
        }
    }
    // The firmware's table and interrupts were already put back above,
    // before the nested probe, because that one calls into boot services
    // and must not do it through this suite's table. Nothing between
    // there and here needs them.

    {
        char line[trace::line_capacity]{};
        auto at = trace::append_text(line, "ZPPTEST DONE pass=");
        at = trace::append_decimal(at, state.passed);
        at = trace::append_text(at, " fail=");
        at = trace::append_decimal(at, state.failed);
        at = trace::append_text(at, " skip=");
        at = trace::append_decimal(at, state.skipped);
        at = trace::append_text(at, " xfail=");
        at = trace::append_decimal(at, state.expected_failures);
        at = trace::append_text(at, " xpass=");
        at = trace::append_decimal(at, state.unexpected_passes);
        at = trace::append_text(at, "\r\n");
        *at = 0;
        trace::raw(line);

        if (system_table->ConOut) {
            char16_t wide[trace::line_capacity]{};
            std::size_t i{};
            for (; line[i] && (i < (trace::line_capacity - 1)); ++i) {
                wide[i] = static_cast<char16_t>(
                    static_cast<unsigned char>(line[i]));
            }
            system_table->ConOut->OutputString(
                system_table->ConOut, reinterpret_cast<CHAR16 *>(wide));
        }
    }

    // Firmware calls through trace are legal again from here. Put back
    // last, after the verdict, so the verdict reaches serial exactly once
    // - under Bochs the firmware console is that same serial port, and a
    // verdict that appears twice is a verdict the harness has to guess
    // about.
    trace::console = console;

    // An unexpected pass does not fail the run: it means a divergence was
    // fixed and the case should be promoted, which is a change to make
    // deliberately rather than a red build to react to.
    return (0 == state.failed);
}

} // namespace zpp

#endif // ZPP_GUEST_TESTS
