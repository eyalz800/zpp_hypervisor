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
    asm volatile("cpuid"
                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(0x40000100u), "c"(0u));
    return exit_state{a, b, c, d};
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

void probe_in_16()
{
    std::uint16_t value{};
    asm volatile("inw %1, %0"
                 : "=a"(value)
                 : "d"(static_cast<std::uint16_t>(g_probe_value)));
    g_probe_value = value;
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

    void note_exit(std::uint32_t reason)
    {
        if (reason < (sizeof(observed_exit) / sizeof(observed_exit[0]))) {
            observed_exit[reason] = true;
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
    };

    auto probe = [&](void (*body)()) {
        auto before = read_exits_before();
        auto vector = zpp_guest_test_try(body);
        auto after = read_exits_after();

        probe_result result{};
        result.vector = vector;
        result.qualification = after.qualification;
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
        check_equal(state,
                    "cpuid.leaf1_vmx_hidden",
                    "leaf=1_ecx_bit5",
                    0,
                    (ecx >> 5) & 1);

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
        constexpr std::uint32_t sampled[]{
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
        check_equal(state,
                    "cr4.vmxe_still_clear_after_write",
                    "cr4_bit13",
                    0,
                    (after & cr4_vmxe) ? 1 : 0);

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

        const vmx_case cases[]{
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
        const msr_case faulting[]{
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
                 "guest_cr4_osxsave_clear",
                 exit_xsetbv,
                 0);
        }
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
        }
    }

    // === What was and was not reached ==================================
    //
    // Printed from what the run measured rather than from the list of
    // cases above, so a case whose instruction quietly stopped exiting
    // shows up as a gap instead of as a pass.
    {
        struct reason_name
        {
            std::uint32_t reason;
            const char * name;
        };

        // SDM Vol. 3D Appendix C, "VMX Basic Exit Reasons". Every reason
        // this VMM's handler has a case for, plus the ones it does not,
        // because the list of what is not covered is the point.
        constexpr reason_name names[]{
            {0, "exception_or_nmi"},
            {1, "external_interrupt"},
            {2, "triple_fault"},
            {3, "init_signal"},
            {4, "start_up_ipi"},
            {7, "interrupt_window"},
            {8, "nmi_window"},
            {9, "task_switch"},
            {10, "cpuid"},
            {11, "getsec"},
            {12, "hlt"},
            {13, "invd"},
            {14, "invlpg"},
            {15, "rdpmc"},
            {16, "rdtsc"},
            {17, "rsm"},
            {18, "vmcall"},
            {19, "vmclear"},
            {20, "vmlaunch"},
            {21, "vmptrld"},
            {22, "vmptrst"},
            {23, "vmread"},
            {24, "vmresume"},
            {25, "vmwrite"},
            {26, "vmxoff"},
            {27, "vmxon"},
            {28, "control_register_access"},
            {29, "mov_debug_register"},
            {30, "io_instruction"},
            {31, "rdmsr"},
            {32, "wrmsr"},
            {33, "entry_invalid_guest_state"},
            {34, "entry_failure_msr_loading"},
            {36, "mwait"},
            {37, "monitor_trap_flag"},
            {39, "monitor"},
            {40, "pause"},
            {43, "tpr_below_threshold"},
            {44, "apic_access"},
            {45, "virtualized_eoi"},
            {46, "gdtr_or_idtr"},
            {47, "ldtr_or_tr"},
            {48, "ept_violation"},
            {49, "ept_misconfiguration"},
            {50, "invept"},
            {51, "rdtscp"},
            {52, "vmx_preemption_timer"},
            {53, "invvpid"},
            {54, "wbinvd"},
            {55, "xsetbv"},
            {56, "apic_write"},
            {57, "rdrand"},
            {58, "invpcid"},
            {59, "vmfunc"},
            {60, "encls"},
            {61, "rdseed"},
            {62, "page_modification_log_full"},
            {63, "xsaves"},
            {64, "xrstors"},
            {66, "spp_related_event"},
        };

        for (const auto & entry : names) {
            char line[trace::line_capacity]{};
            auto at = trace::append_text(line, "ZPPCOVER ");
            at = trace::append_decimal(at, entry.reason);
            at = trace::append_text(at, " ");
            at = trace::append_text(at, entry.name);
            at = trace::append_text(at,
                                    state.observed_exit[entry.reason]
                                        ? " observed"
                                        : " absent");
            at = trace::append_text(at, "\r\n");
            *at = 0;
            trace::raw(line);
        }
    }

    // Firmware's own table back, before anything else in this image
    // runs. A fault taken after this point is the firmware's to report,
    // which is what it would have been without this suite.
    write_idtr(firmware_idtr);
    asm volatile("sti" : : : "memory");

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
