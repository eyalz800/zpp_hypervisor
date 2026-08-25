// Differential test harness for the nested VMX state machine.
//
// Compiles the real vmcs12 shadow and the real VMX-instruction emulation
// (hypervisor/src/hypervisor/nested_vmx.cpp) natively against the real
// hypervisor class, a shim VMCS and a fake guest physical memory, and
// drives them the way a guest hypervisor would.
//
// The only stand-in headers on this harness's include path are
// tests/shim/zpp/arch/x86_64/asm.h and .../vmx/asm.h, which exist
// because a Mac cannot execute `vmread`.
#include "zpp/hypervisor/hypervisor.h"
#include <cstring>
#include <format>
#include <map>
#include <print>
#include <string>
#include <vector>

using zpp::arch::x86_64::vmx::vmcs12;
using zpp::arch::x86_64::vmx::vmcs_field_encoding;
using basic_reason = zpp::arch::x86_64::vmx::exit_reason::basic_reason;
using ie = zpp::hypervisor::nested_vmx::instruction_error;
namespace fields = zpp::arch::x86_64::vmx::vmcs_fields;

// ---------------------------------------------------------------- memory
static std::map<std::uint64_t, std::vector<std::byte>> g_pages;

static std::vector<std::byte> & page_of(std::uint64_t physical)
{
    auto base = physical & ~std::uint64_t(0xfff);
    auto it = g_pages.find(base);
    if (it == g_pages.end()) {
        it = g_pages.emplace(base, std::vector<std::byte>(4096)).first;
    }
    return it->second;
}

static bool g_page_present_only = false;

// --------------------------------------------------------- observations
/**
 * What this harness records about the calls the code under test makes
 * into the rest of the VMM, and the two answers it makes those calls
 * give back.
 *
 * Namespace scope, because these are the harness's counters and not the
 * hypervisor's. They used to be members of a stand-in
 * `zpp::hypervisor::hypervisor`; that class copy is gone and the real
 * one has no place for them, which is the right answer - a counter only
 * a test reads does not belong in a class the hypervisor ships.
 */
struct observations
{
    std::uint64_t gp_faults{};
    std::uint64_t ud_faults{};

    /**
     * The page faults the memory-form VMREAD path injects when it cannot
     * write the operand, with the address and error code it built. Kept
     * separately from `ud_faults` because the whole defect these
     * describe was answering that case with the *wrong* fault, and a
     * counter that merged them could not have caught it.
     */
    std::uint64_t pf_faults{};
    std::uint64_t last_pf_address{};
    std::uint64_t last_pf_error{};
    std::uint64_t ept_discards{};
    std::uint64_t ept_discards_for{};
    std::uint64_t last_discard_root{};
    std::uint64_t ept_refreshes_for{};
    std::uint64_t last_refresh_root{};
    std::uint64_t flushes{};
    std::uint64_t enter_or_park_l2_calls{};

    /**
     * What `build_vmcs02` and `enter_or_park_l2` answer, which the
     * suite drives rather than observes.
     */
    bool build_vmcs02_fails{};
    zpp::hypervisor::hypervisor::l2_entry_outcome
        enter_or_park_l2_outcome{};
};

static observations g_observed;

namespace zpp::hypervisor
{
hypervisor & hypervisor::instance()
{
    static hypervisor the;
    return the;
}

/**
 * The fixture processor's VMX capability MSRs.
 *
 * Split from `cached_vmx_msr`, which returns a reference: the real one
 * hands back a slot of the array `initialize_vmx_msrs` fills, and this
 * harness compiles that declaration rather than a copy of it.
 */
static std::uint64_t vmx_msr_fixture(std::size_t msr)
{
    // A generous but realistic host: every control allowed-1, allowed-0
    // minimal, so narrowing is the only thing that can remove a bit.
    switch (msr) {
    case 0x480:
        return 0x00da040000000000ull;
    case 0x481:
    case 0x48d:
        return 0x7f00000016ull;
    case 0x482:
    case 0x48e:
        return 0xfff9fffe0401e172ull;
    case 0x483:
    case 0x48f:
        return 0x07ffffff00036dffull;
    case 0x484:
    case 0x490:
        return 0x0003fff000011ffull;
    case 0x485:
        return 0x7004c1e7ull; // bit 29 set, cr3 targets 4, msr list 7
    case 0x48b:
        return 0xfffffffull << 32;
    case 0x48c:
        return 0xf0106734141ull;
    default:
        return 0;
    }
}

std::uint64_t & hypervisor::cached_vmx_msr(std::size_t msr)
{
    static std::map<std::size_t, std::uint64_t> answers;

    auto & slot = answers[msr];
    slot = vmx_msr_fixture(msr);
    return slot;
}

void hypervisor::inject_general_protection_fault(std::uint64_t)
{
    g_observed.gp_faults = g_observed.gp_faults + 1;
}

// The VMFUNC path in `on_l2_exit` refuses a function it cannot follow
// with #UD, which is what SDM 26.5.5 requires of a VM function that
// fails, so this harness needs the injector its sibling above already
// had. Counted rather than ignored: a refusal is the interesting
// outcome, since offering VMFUNC and then refusing every call would be
// the "announced and not answered" failure this tree keeps finding.
// The enlightened VMCS lives in nested_evmcs.cpp, which this harness does
// not compile - it has no guest memory to read the structure out of. The
// launch path calls the loader, so that is the one symbol that reaches
// here, and answering false is the un-armed case: the ordinary VMPTRLD
// path applies, which is what every case in this suite exercises.
bool hypervisor::load_enlightened_vmcs(std::size_t)
{
    return false;
}

void hypervisor::store_enlightened_vmcs(std::size_t)
{
}

void hypervisor::inject_invalid_opcode_exception()
{
    g_observed.ud_faults = g_observed.ud_faults + 1;
}

void hypervisor::inject_page_fault(std::uint64_t linear,
                                   std::uint64_t error_code)
{
    g_observed.pf_faults = g_observed.pf_faults + 1;
    g_observed.last_pf_address = linear;
    g_observed.last_pf_error = error_code;
}

std::expected<std::uint64_t, zpp::error>
hypervisor::guest_linear_to_physical(std::uint64_t linear)
{
    if (g_page_present_only &&
        !g_pages.count(linear & ~std::uint64_t(0xfff))) {
        return std::unexpected(
            zpp::error{error::guest_address_not_mapped});
    }
    return linear;
}

/**
 * The guest-thread probe, which `on_guest_vmlaunch` calls on every entry.
 *
 * Stood in rather than linked, because it lives in nested_entry.cpp and
 * this harness deliberately compiles nested_vmx.cpp alone. What it does
 * is read another operating system's structures through two levels of
 * translation, which this harness has neither of - and it is a
 * diagnostic, so doing nothing is a faithful stand-in.
 */
/**
 * The two interception points the deferred guest-state copy depends on,
 * which live in `nested_entry.cpp` and are not compiled here.
 *
 * Counted rather than ignored, because they *are* the safety argument:
 * a guest-state field the level above writes must be marked owed, and
 * one it reads must materialise before the value is produced. A stub
 * that silently did nothing would let those cases pass while the
 * property they assert was absent.
 */
std::size_t g_guest_state_marked_dirty{};
std::size_t g_guest_state_materialised{};
std::uint64_t g_guest_state_last_encoding{};

/**
 * The setter that owns "which vmcs12 is current", which lives in
 * `nested_entry.cpp`. Assigning the member here rather than stubbing it
 * away, because the suite's own cases read it back.
 */
void hypervisor::set_guest_current_vmcs(std::size_t cpu,
                                        std::uint64_t address)
{
    this->guest_current_vmcs[cpu] = address;
    this->guest_state_deferred[cpu] = false;
}

std::size_t g_guest_state_full_materialise{};

void hypervisor::materialise_l2_guest_state(std::size_t)
{
    ++g_guest_state_full_materialise;
}

void hypervisor::mark_l2_guest_state_dirty(std::size_t,
                                           std::uint64_t encoding)
{
    ++g_guest_state_marked_dirty;
    g_guest_state_last_encoding = encoding;
}

void hypervisor::materialise_l2_guest_state_for(std::size_t,
                                                std::uint64_t encoding)
{
    ++g_guest_state_materialised;
    g_guest_state_last_encoding = encoding;
}

void hypervisor::sample_guest_thread(std::size_t)
{
}

std::expected<void, zpp::error> hypervisor::read_guest_physical(
    std::uint64_t physical, std::span<std::byte> into)
{
    if (g_page_present_only &&
        !g_pages.count(physical & ~std::uint64_t(0xfff))) {
        return std::unexpected(
            zpp::error{error::guest_address_not_mapped});
    }
    std::size_t done{};
    while (done < into.size()) {
        auto at = physical + done;
        auto offset = at & 0xfff;
        auto count = 0x1000 - offset;
        if (count > (into.size() - done)) {
            count = into.size() - done;
        }
        std::memcpy(
            into.data() + done, page_of(at).data() + offset, count);
        done += count;
    }
    return {};
}

std::expected<void, zpp::error> hypervisor::write_guest_physical(
    std::uint64_t physical, std::span<const std::byte> from)
{
    if (g_page_present_only &&
        !g_pages.count(physical & ~std::uint64_t(0xfff))) {
        return std::unexpected(
            zpp::error{error::guest_address_not_mapped});
    }
    std::size_t done{};
    while (done < from.size()) {
        auto at = physical + done;
        auto offset = at & 0xfff;
        auto count = 0x1000 - offset;
        if (count > (from.size() - done)) {
            count = from.size() - done;
        }
        std::memcpy(
            page_of(at).data() + offset, from.data() + done, count);
        done += count;
    }
    return {};
}

/**
 * Publishing the reference TSC page needs the guest hypervisor's extended
 * tables and a write into its guest's memory, neither of which this
 * harness models - and with the switch off, which is how this suite
 * builds, the real one returns immediately anyway.
 */
void hypervisor::publish_reference_tsc_page(std::size_t)
{
}

void hypervisor::discard_shadow_ept(std::size_t)
{
    g_observed.ept_discards = g_observed.ept_discards + 1;
}

void hypervisor::discard_shadow_ept_for(std::size_t, std::uint64_t root)
{
    g_observed.ept_discards_for = g_observed.ept_discards_for + 1;
    g_observed.last_discard_root = root;
}

// The refresh replaced the discard on the single-context path. Modelled
// separately rather than folded into the counter above, because the two
// differ in what survives - a refresh keeps the mappings the guest
// hypervisor did not change - and a test that could not tell them apart
// would pass either way.
void hypervisor::refresh_shadow_ept_for(std::size_t, std::uint64_t root)
{
    g_observed.ept_refreshes_for = g_observed.ept_refreshes_for + 1;
    g_observed.last_refresh_root = root;
}

void hypervisor::nested_transition_flush(std::size_t)
{
    g_observed.flushes = g_observed.flushes + 1;
}

std::expected<void, zpp::error> hypervisor::build_vmcs02(std::size_t)
{
    if (g_observed.build_vmcs02_fails) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }
    return {};
}

hypervisor::l2_entry_outcome hypervisor::enter_or_park_l2(std::size_t)
{
    g_observed.enter_or_park_l2_calls =
        g_observed.enter_or_park_l2_calls + 1;
    return g_observed.enter_or_park_l2_outcome;
}

void hypervisor::reflect_l2_exit(std::size_t,
                                 arch::x86_64::vmx::exit_reason,
                                 std::uint64_t)
{
}

std::uint64_t hypervisor::own_vmcs_region_physical(std::size_t)
{
    return 0x1000;
}

} // namespace zpp::hypervisor

// ----------------------------------------------------------------- rig
using hypervisor_t = zpp::hypervisor::hypervisor;
namespace vmxmsr = zpp::arch::x86_64::vmx::msr;

static int g_failures = 0;
static int g_checks = 0;
static std::vector<std::string> g_findings;

static void check(bool ok, const std::string & what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::println("  FAIL {}", what);
        g_findings.push_back(what);
    }
}

static constexpr std::uint64_t rflags_cf = 1ull << 0;
static constexpr std::uint64_t rflags_zf = 1ull << 6;

static hypervisor_t & hv()
{
    return hypervisor_t::instance();
}

// Result of executing one emulated instruction.
enum class outcome
{
    ud,           // handler returned false: caller injects #UD
    gp,           // a general protection fault was injected
    succeed,      // VMsucceed
    fail_invalid, // VMfailInvalid
    fail_valid,   // VMfailValid, error in `error`
};

struct result
{
    outcome what{};
    std::uint64_t error{};
};

static const char * name(outcome o)
{
    switch (o) {
    case outcome::ud:
        return "#UD";
    case outcome::gp:
        return "#GP";
    case outcome::succeed:
        return "VMsucceed";
    case outcome::fail_invalid:
        return "VMfailInvalid";
    case outcome::fail_valid:
        return "VMfailValid";
    }
    return "?";
}

// Guest state that makes every #UD precondition pass.
static void arm_guest(std::size_t cpu)
{
    auto & v = hv().vmcs;
    v.vpid(cpu + 1);
    v.guest_cr0(1);                                  // PE
    v.guest_rflags(0x2);                             // no VM, no arith
    v.cr4_read_shadow(1ull << 13);                   // VMXE
    v.guest_cs_access_rights(0xa09b | (1ull << 13)); // L = 1
    v.guest_ss_access_rights(0xc093);                // DPL 0
    v.vm_entry_controls(1ull << 9);                  // IA-32e mode guest
    v.guest_fs_base(0);
    v.guest_gs_base(0);
}

// Instruction-information field for a memory operand with no base and no
// index, so the linear address is exactly the displacement.
static std::uint64_t memory_operand_information(std::uint64_t reg2 = 0,
                                                std::uint64_t reg1 = 0)
{
    return (2ull << 7)    // address size 64-bit
           | (1ull << 22) // index invalid
           | (1ull << 27) // base invalid
           | (0ull << 15) // segment ES (ignored in 64-bit)
           | ((reg1 & 0xf) << 3) | ((reg2 & 0xf) << 28);
}

static std::uint64_t register_operand_information(std::uint64_t reg1,
                                                  std::uint64_t reg2)
{
    return (1ull << 10) | ((reg1 & 0xf) << 3) | ((reg2 & 0xf) << 28) |
           (2ull << 7);
}

static result run(basic_reason reason,
                  zpp::arch::x86_64::context & regs,
                  std::uint64_t information,
                  std::uint64_t qualification = 0)
{
    auto & v = hv().vmcs;
    v.vm_exit_instruction_information(information);
    zpp::arch::x86_64::vmx::g_vmcs
        [zpp::arch::x86_64::vmx::vmcs_fields::exit_qualification] =
            qualification;
    v.guest_rflags((v.guest_rflags() & ~0x8d5ull) | 0x2);

    auto faults = g_observed.gp_faults;

    // The processor index, which `on_vmx_instruction` takes now instead
    // of reading the VPID for itself. Derived here from the same field it
    // used to read, so the harness drives it with exactly the number the
    // real caller does - `on_vm_exit`'s `cpuid`, which `setup_vmcs` wrote
    // the VPID from.
    auto handled =
        hv().on_vmx_instruction(v.vpid() - 1,
                                zpp::arch::x86_64::vmx::exit_reason(
                                    static_cast<std::uint64_t>(reason)),
                                regs);

    if (g_observed.gp_faults != faults) {
        return {outcome::gp, 0};
    }
    if (!handled) {
        return {outcome::ud, 0};
    }

    auto flags = v.guest_rflags();
    auto cpu = v.vpid() - 1;
    if (0 != (flags & rflags_cf)) {
        return {outcome::fail_invalid, 0};
    }
    if (0 != (flags & rflags_zf)) {
        return {outcome::fail_valid,
                hv().guest_vmcs12[cpu].read(fields::vm_instruction_error)};
    }
    return {outcome::succeed, 0};
}

static void expect(const char * what,
                   result got,
                   outcome want,
                   std::uint64_t want_error = 0)
{
    bool ok = got.what == want &&
              (want != outcome::fail_valid || got.error == want_error);
    ++g_checks;
    if (!ok) {
        ++g_failures;
        auto message = std::format("{}: got {}({}), wanted {}({})",
                                   what,
                                   name(got.what),
                                   got.error,
                                   name(want),
                                   want_error);
        std::println("  FAIL {}", message);
        g_findings.push_back(message);
    }
}

// Prepares a page as a VMXON/VMCS region with the right revision.
static void make_region(std::uint64_t physical, std::uint32_t revision)
{
    auto & page = page_of(physical);
    std::memset(page.data(), 0, page.size());
    std::memcpy(page.data(), &revision, sizeof(revision));
}

static result do_pointer_instruction(basic_reason reason,
                                     std::uint64_t operand_address,
                                     std::uint64_t pointer)
{
    // The operand is a pointer in memory at operand_address.
    auto & page = page_of(operand_address);
    std::memcpy(page.data() + (operand_address & 0xfff),
                &pointer,
                sizeof(pointer));

    zpp::arch::x86_64::context regs{};
    return run(
        reason, regs, memory_operand_information(), operand_address);
}

static constexpr std::uint64_t operand_slot = 0x40000;
static constexpr std::uint64_t vmxon_region = 0x50000;
static constexpr std::uint64_t vmcs_a = 0x51000;
static constexpr std::uint64_t vmcs_b = 0x52000;

static void reset_cpu(std::size_t cpu)
{
    hv().guest_in_vmx_operation[cpu] = false;
    hv().guest_vmxon_pointer[cpu] = 0;
    hv().guest_current_vmcs[cpu] =
        zpp::hypervisor::nested_vmx::no_current_vmcs;
    hv().guest_vmcs12[cpu].clear();
    hv().guest_feature_control[cpu] = 0x5;
    hv().running_l2[cpu] = false;
    hv().l2_entries[cpu] = 0;
    g_observed.build_vmcs02_fails = false;
    g_observed.enter_or_park_l2_outcome =
        hypervisor_t::l2_entry_outcome::entered;
    g_observed.enter_or_park_l2_calls = 0;
    arm_guest(cpu);
}

// The processor is named by the caller for the same reason every other
// step is, but vmxon needs no index: it acts on whichever processor
// `arm_guest` has already made current.
static void enter_vmx([[maybe_unused]] std::size_t cpu)
{
    make_region(vmxon_region, vmcs12::revision);
    auto r = do_pointer_instruction(
        basic_reason::vmxon, operand_slot, vmxon_region);
    if (r.what != outcome::succeed) {
        std::println("  setup: vmxon failed ({})", name(r.what));
    }
}

// --------------------------------------------------- 1. field round-trip
struct named_field
{
    const char * text;
    std::uint64_t encoding;
};

static const named_field g_fields[] = {
#include "fields.inc"
};

static result vmwrite_field(std::uint64_t encoding, std::uint64_t value)
{
    zpp::arch::x86_64::context regs{};
    regs.rcx = encoding;
    regs.rax = value;
    return run(basic_reason::vmwrite,
               regs,
               register_operand_information(0 /*rax*/, 1 /*rcx*/));
}

/**
 * The two interception points the deferred guest-state copy rests on.
 *
 * `save_l2_state` no longer copies 44 of `guest_state_fields` out of
 * vmcs02 on every exit, which is only sound while a field the level
 * above *writes* is remembered as owed, and a field it *reads* is
 * materialised before the value is produced. Both are one call on a
 * path this suite already drives, and a stub that did nothing would let
 * every other case here pass with the property absent - so they are
 * asserted directly.
 */
static void check_guest_state_interception();

static result vmread_result(std::uint64_t encoding, std::uint64_t & out)
{
    zpp::arch::x86_64::context regs{};
    regs.rcx = encoding;
    auto r = run(
        basic_reason::vmread, regs, register_operand_information(0, 1));
    out = regs.rax;
    return r;
}

static void test_field_round_trip()
{
    std::println("[1] VMWRITE/VMREAD round trip over vmcs_fields.h");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    constexpr std::uint64_t pattern = 0xfeedface12345678ull;

    for (auto & f : g_fields) {
        vmcs_field_encoding e(f.encoding);
        auto width = e.field_width();
        bool read_only = e.read_only();

        auto w = vmwrite_field(f.encoding, pattern);

        if (read_only) {
            expect((std::string("VMWRITE ") + f.text +
                    " (read-only component)")
                       .c_str(),
                   w,
                   outcome::fail_valid,
                   13);
            continue;
        }

        if (w.what != outcome::succeed) {
            auto message = std::format(
                "VMWRITE {} (encoding {:#x}, index {}) refused: "
                "{}({})",
                f.text,
                f.encoding,
                e.index(),
                name(w.what),
                w.error);
            ++g_checks;
            ++g_failures;
            std::println("  FAIL {}", message);
            g_findings.push_back(message);
            continue;
        }

        std::uint64_t got{};
        auto r = vmread_result(f.encoding, got);
        if (r.what != outcome::succeed) {
            check(false,
                  std::string("VMREAD ") + f.text +
                      " refused after a "
                      "successful VMWRITE");
            continue;
        }

        std::uint64_t want = pattern;
        switch (width) {
        case vmcs_field_encoding::width::bits_16:
            want &= 0xffff;
            break;
        case vmcs_field_encoding::width::bits_32:
            want &= 0xffffffff;
            break;
        default:
            break;
        }

        if (got != want) {
            auto message = std::format(
                "{} ({:#x}): wrote {:#x} read back {:#x}, wanted "
                "{:#x}",
                f.text,
                f.encoding,
                pattern,
                got,
                want);
            ++g_checks;
            ++g_failures;
            std::println("  FAIL {}", message);
            g_findings.push_back(message);
        } else {
            ++g_checks;
        }

        // The high access type of a 64-bit field.
        if (width == vmcs_field_encoding::width::bits_64) {
            std::uint64_t high{};
            auto hr = vmread_result(f.encoding | 1, high);
            if (hr.what != outcome::succeed) {
                check(false,
                      std::string("VMREAD high half of ") + f.text +
                          " refused");
            } else {
                check(high == (want >> 32),
                      std::string("high half of ") + f.text +
                          " read back wrong");
            }

            // Writing the high half must merge, not replace.
            auto hw = vmwrite_field(f.encoding | 1, 0xaabbccddull);
            check(hw.what == outcome::succeed,
                  std::string("VMWRITE high half of ") + f.text +
                      " refused");
            std::uint64_t merged{};
            vmread_result(f.encoding, merged);
            check(merged == ((0xaabbccddull << 32) | (want & 0xffffffff)),
                  std::string("high-half VMWRITE of ") + f.text +
                      " did not merge");
            vmwrite_field(f.encoding, want);
        }
    }
}

// ------------------------------------------------ 2. encoding validation
static void test_encoding_validation()
{
    std::println("[2] encoding validation and width/type decode");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    // Bit 12 reserved.
    expect("VMWRITE encoding with bit 12 set",
           vmwrite_field(0x1000 | 0x800, 1),
           outcome::fail_valid,
           12);

    // Beyond bit 14.
    expect("VMWRITE encoding with bit 15 set",
           vmwrite_field(0x8000, 1),
           outcome::fail_valid,
           12);

    // High access on a 16-bit field.
    expect("VMWRITE high access of a 16-bit field",
           vmwrite_field(fields::guest_es_selector | 1, 1),
           outcome::fail_valid,
           12);

    // High access on a natural-width field.
    expect("VMWRITE high access of a natural-width field",
           vmwrite_field(fields::guest_cr0 | 1, 1),
           outcome::fail_valid,
           12);

    // High access on a 32-bit field.
    expect("VMWRITE high access of a 32-bit field",
           vmwrite_field(fields::exception_bitmap | 1, 1),
           outcome::fail_valid,
           12);

    // Index at and beyond the capacity. index_capacity is 28, and
    // IA32_VMX_VMCS_ENUM reports 27, so index 28 must be refused and
    // index 27 accepted.
    auto in_range = (27ull << 1) | (0ull << 10) | (3ull << 13);
    auto out_of_range = (28ull << 1) | (0ull << 10) | (3ull << 13);
    expect("VMWRITE natural-width control index 27",
           vmwrite_field(in_range, 1),
           outcome::succeed);
    expect("VMWRITE natural-width control index 28",
           vmwrite_field(out_of_range, 1),
           outcome::fail_valid,
           12);

    // Real Appendix B encodings above the shadow's capacity. These are
    // fields a processor supports and a guest hypervisor may use.
    struct
    {
        const char * text;
        std::uint64_t encoding;
    } beyond[] = {
        {"ENCLV-exiting bitmap (00002038H)", 0x2038},
        {"HLAT pointer (00002040H)", 0x2040},
        {"tertiary processor-based controls (00002034H)", 0x2034},
        {"shared EPT pointer (0000203CH)", 0x203c},
        {"secondary VM-exit controls (00002044H)", 0x2044},
        {"guest IA32_RTIT_CTL (00002814H)", 0x2814},
        {"guest IA32_LBR_CTL (00002816H)", 0x2816},
        {"guest IA32_S_CET (00006828H)", 0x6828},
        {"guest SSP (0000682AH)", 0x682a},
        {"guest interrupt SSP table (0000682CH)", 0x682c},
        {"host IA32_S_CET (00006C18H)", 0x6c18},
        {"host SSP (00006C1AH)", 0x6c1a},
        {"host interrupt SSP table (00006C1CH)", 0x6c1c},
    };
    for (auto & b : beyond) {
        vmcs_field_encoding e(b.encoding);
        std::uint64_t unused{};
        auto w = vmwrite_field(b.encoding, 1);
        auto r = vmread_result(b.encoding, unused);
        std::println("      {:<46} index {:2} -> VMWRITE {}({}) VMREAD "
                     "{}({})",
                     b.text,
                     e.index(),
                     name(w.what),
                     w.error,
                     name(r.what),
                     r.error);
    }
}

// ---------------------------------------------------- 3. launch machine
static void test_launch_state_machine()
{
    std::println("[3] the launch state machine");
    std::size_t cpu = 0;
    reset_cpu(cpu);

    // Not in VMX operation: every instruction but VMXON is #UD.
    zpp::arch::x86_64::context regs{};
    expect("VMREAD outside VMX operation",
           run(basic_reason::vmread,
               regs,
               register_operand_information(0, 1)),
           outcome::ud);
    expect("VMLAUNCH outside VMX operation",
           run(basic_reason::vmlaunch, regs, 0),
           outcome::ud);
    expect("VMXOFF outside VMX operation",
           run(basic_reason::vmxoff, regs, 0),
           outcome::ud);

    // VMXON with no CR4.VMXE in the read shadow.
    hv().vmcs.cr4_read_shadow(0);
    expect("VMXON with CR4.VMXE clear in the read shadow",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::ud);
    hv().vmcs.cr4_read_shadow(1ull << 13);

    // CPL above zero.
    hv().vmcs.guest_ss_access_rights(0xc0f3);
    expect("VMXON at CPL 3",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::gp);
    hv().vmcs.guest_ss_access_rights(0xc093);

    // Virtual-8086 mode and real mode.
    hv().vmcs.guest_rflags(0x2 | (1ull << 17));
    expect("VMXON in virtual-8086 mode",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::ud);
    hv().vmcs.guest_rflags(0x2);
    hv().vmcs.guest_cr0(0);
    expect("VMXON with CR0.PE clear",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::ud);
    hv().vmcs.guest_cr0(1);

    // Compatibility mode: LMA set, CS.L clear (bit 13 of the access
    // rights). 0xa09b already carries L, so the D/B form is used.
    hv().vmcs.guest_cs_access_rights(0xc09b);
    expect("VMXON in compatibility mode",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::ud);
    hv().vmcs.guest_cs_access_rights(0xa09b | (1ull << 13));

    // VMXON with the feature control MSR unlocked.
    hv().guest_feature_control[cpu] = 0;
    expect("VMXON with IA32_FEATURE_CONTROL unlocked",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::gp);
    hv().guest_feature_control[cpu] = 0x5;

    // Misaligned region, and a register operand.
    expect("VMXON with a misaligned region",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region + 8),
           outcome::fail_invalid);
    {
        zpp::arch::x86_64::context r2{};
        expect("VMXON with a register operand",
               run(basic_reason::vmxon,
                   r2,
                   register_operand_information(0, 1)),
               outcome::ud);
    }

    // A region beyond the address limit vmcs_pointer_valid enforces.
    expect("VMXON with a region at 512 GB",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  512ull * 1024 * 1024 * 1024),
           outcome::fail_invalid);

    // Wrong revision.
    make_region(vmxon_region, vmcs12::revision ^ 1);
    expect("VMXON with the wrong revision identifier",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::fail_invalid);

    // Good.
    make_region(vmxon_region, vmcs12::revision);
    expect("VMXON",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::succeed);

    // Again. SDM 33.2 VMfail: with no current VMCS this is VMfailInvalid,
    // not VMfailValid(15) - the error number has nowhere to go.
    expect("VMXON in VMX root operation, no current VMCS",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::fail_invalid);

    // No current VMCS yet.
    expect("VMLAUNCH with no current VMCS",
           run(basic_reason::vmlaunch, regs, 0),
           outcome::fail_invalid);
    expect("VMRESUME with no current VMCS",
           run(basic_reason::vmresume, regs, 0),
           outcome::fail_invalid);
    expect("VMREAD with no current VMCS",
           run(basic_reason::vmread,
               regs,
               register_operand_information(0, 1)),
           outcome::fail_invalid);
    expect("VMWRITE with no current VMCS",
           vmwrite_field(fields::guest_rip, 1),
           outcome::fail_invalid);
    expect("VMCALL with no current VMCS",
           run(basic_reason::vmcall, regs, 0),
           outcome::fail_invalid);

    // VMPTRST with no current VMCS writes the all-ones sentinel.
    {
        zpp::arch::x86_64::context r2{};
        run(basic_reason::vmptrst,
            r2,
            memory_operand_information(),
            operand_slot);
        std::uint64_t stored{};
        std::memcpy(&stored,
                    page_of(operand_slot).data() + (operand_slot & 0xfff),
                    sizeof(stored));
        check(stored == ~std::uint64_t(0),
              "VMPTRST with no current VMCS did not store "
              "FFFFFFFFFFFFFFFFH");
    }

    // A good VMPTRLD, so that from here every VMfail carries an error.
    make_region(vmcs_a, vmcs12::revision);
    expect("VMPTRLD",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_a),
           outcome::succeed);
    check(hv().guest_current_vmcs[cpu] == vmcs_a,
          "VMPTRLD did not make the region current");

    // Now the error numbers, SDM Table 33-1.
    expect("VMXON in VMX root operation",
           do_pointer_instruction(
               basic_reason::vmxon, operand_slot, vmxon_region),
           outcome::fail_valid,
           15);
    expect("VMCALL in VMX root operation",
           run(basic_reason::vmcall, regs, 0),
           outcome::fail_valid,
           1);
    expect("VMPTRLD with the VMXON pointer",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmxon_region),
           outcome::fail_valid,
           10);
    expect("VMCLEAR with the VMXON pointer",
           do_pointer_instruction(
               basic_reason::vmclear, operand_slot, vmxon_region),
           outcome::fail_valid,
           3);
    expect("VMPTRLD with a misaligned address",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_b + 8),
           outcome::fail_valid,
           9);
    expect("VMCLEAR with a misaligned address",
           do_pointer_instruction(
               basic_reason::vmclear, operand_slot, vmcs_b + 8),
           outcome::fail_valid,
           2);

    // A region with the wrong revision.
    make_region(vmcs_b, 0xdeadbeef);
    expect("VMPTRLD with the wrong revision identifier",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_b),
           outcome::fail_valid,
           11);
    check(hv().guest_current_vmcs[cpu] == vmcs_a,
          "a refused VMPTRLD changed the current VMCS anyway");

    // VMPTRLD of the already-current VMCS must not reload it, which is
    // the regression the cache-over-region bug left behind.
    vmwrite_field(fields::guest_rip, 0x1234);
    expect("VMPTRLD of the already-current VMCS",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_a),
           outcome::succeed);
    std::uint64_t rip{};
    vmread_result(fields::guest_rip, rip);
    check(rip == 0x1234,
          "a redundant VMPTRLD discarded the cached shadow");

    // VMRESUME before any VMLAUNCH.
    expect("VMRESUME on a clear VMCS",
           run(basic_reason::vmresume, regs, 0),
           outcome::fail_valid,
           5);

    vmwrite_field(fields::pin_based_vm_execution_controls, 0x1e);

    // VMLAUNCH.
    auto launched = run(basic_reason::vmlaunch, regs, 0);
    expect("VMLAUNCH on a clear VMCS", launched, outcome::succeed);
    check(hv().running_l2[cpu],
          "VMLAUNCH did not mark the processor as running L2");
    check(hv().nested_rip_settled[cpu],
          "VMLAUNCH did not stop the caller advancing RIP");

    // SDM 33.3 VMLAUNCH sets the launch state as step 5 of VM entry.
    // This VMM sets it in reflect_l2_exit instead, which is what KVM
    // does too - recorded rather than asserted.
    std::println("      launch state after a successful VMLAUNCH: {}",
                 hv().guest_vmcs12[cpu].state() ==
                         vmcs12::launch_state::launched
                     ? "launched"
                     : "clear");

    check(1 == g_observed.enter_or_park_l2_calls,
          "VMLAUNCH did not ask whether the guest may be entered");

    // The guest-state decision is asked after the controls and the host
    // state, and neither of its two "not entered" answers may leave the
    // processor marked as running a second-level guest. Both are a
    // VMsucceed as far as the guest hypervisor's flags go: an entry that
    // fails on guest state is a VM exit, not a VMfail (SDM 29.8), and a
    // retry has not happened at all.
    for (auto outcome_case : {hypervisor_t::l2_entry_outcome::reflected,
                              hypervisor_t::l2_entry_outcome::retry}) {
        hv().running_l2[cpu] = false;
        hv().nested_rip_settled[cpu] = false;
        hv().guest_vmcs12[cpu].state(vmcs12::launch_state::clear);
        g_observed.enter_or_park_l2_outcome = outcome_case;

        expect("VMLAUNCH the guest-state decision declined",
               run(basic_reason::vmlaunch, regs, 0),
               outcome::succeed);
        check(!hv().running_l2[cpu],
              "a declined VM entry left the processor running L2");
        check(hv().nested_rip_settled[cpu],
              "a declined VM entry let the caller advance RIP");
    }

    g_observed.enter_or_park_l2_outcome =
        hypervisor_t::l2_entry_outcome::entered;

    // A refused build_vmcs02 must leave the launch state alone and tell
    // the guest hypervisor its entry did not happen.
    hv().running_l2[cpu] = false;
    g_observed.build_vmcs02_fails = true;
    expect("VMLAUNCH refused by build_vmcs02",
           run(basic_reason::vmlaunch, regs, 0),
           outcome::fail_valid,
           7);
    check(!hv().running_l2[cpu],
          "a refused VMLAUNCH left the processor marked as running L2");
    g_observed.build_vmcs02_fails = false;
}

// ------------------------------------------ 4. VMCLEAR / migration cycle
static void test_vmclear_and_migration()
{
    std::println("[4] VMCLEAR, and the migration cycle it exists for");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);

    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    vmwrite_field(fields::guest_rip, 0xcafe0000);
    vmwrite_field(fields::pin_based_vm_execution_controls, 0x1e);

    // VMCLEAR of the current VMCS: the fields must survive.
    expect("VMCLEAR of the current VMCS",
           do_pointer_instruction(
               basic_reason::vmclear, operand_slot, vmcs_a),
           outcome::succeed);
    check(hv().guest_current_vmcs[cpu] ==
              zpp::hypervisor::nested_vmx::no_current_vmcs,
          "VMCLEAR of the current VMCS left it current");

    // Reload and confirm the data came back.
    expect("VMPTRLD after VMCLEAR",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_a),
           outcome::succeed);
    std::uint64_t rip{};
    vmread_result(fields::guest_rip, rip);
    check(rip == 0xcafe0000,
          "VMCLEAR then VMPTRLD lost the fields the guest had written");

    // A VMWRITE, then VMCLEAR of a *different* VMCS, then VMPTRLD of this
    // one again - the migration sequence. The cache must reach the region.
    vmwrite_field(fields::guest_rip, 0xbeef0000);
    make_region(vmcs_b, vmcs12::revision);
    expect("VMCLEAR of a VMCS that is not current",
           do_pointer_instruction(
               basic_reason::vmclear, operand_slot, vmcs_b),
           outcome::succeed);
    expect("VMPTRLD of the other VMCS",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_b),
           outcome::succeed);
    expect("VMPTRLD back to the first",
           do_pointer_instruction(
               basic_reason::vmptrld, operand_slot, vmcs_a),
           outcome::succeed);
    vmread_result(fields::guest_rip, rip);
    check(
        rip == 0xbeef0000,
        "a VMWRITE made after the last VMCLEAR did not reach the region, "
        "so switching VMCS pointers loses it");

    // The launch state must survive the round trip through memory too.
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    zpp::arch::x86_64::context regs{};
    run(basic_reason::vmlaunch, regs, 0);
    hv().running_l2[cpu] = false;
    auto state_before = hv().guest_vmcs12[cpu].state();
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_b);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    check(hv().guest_vmcs12[cpu].state() == state_before,
          "the launch state did not survive a VMPTRLD away and back");

    // VMXOFF flushes.
    expect("VMXOFF", run(basic_reason::vmxoff, regs, 0), outcome::succeed);
    check(!hv().guest_in_vmx_operation[cpu],
          "VMXOFF left the processor in VMX operation");
    expect("VMREAD after VMXOFF",
           run(basic_reason::vmread,
               regs,
               register_operand_information(0, 1)),
           outcome::ud);
}

// --------------------------------------------- 5. VMREAD/VMWRITE memory
static void test_memory_operands()
{
    std::println("[5] the memory forms of VMREAD and VMWRITE");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    vmwrite_field(fields::guest_rip, 0x1122334455667788ull);

    // VMREAD to memory in 64-bit mode stores 8 bytes.
    {
        auto & page = page_of(operand_slot);
        std::memset(page.data() + (operand_slot & 0xfff), 0xcc, 16);
        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_rip;
        auto r = run(basic_reason::vmread,
                     regs,
                     memory_operand_information(1 /*reg2 = rcx*/),
                     operand_slot);
        expect("VMREAD to memory", r, outcome::succeed);
        std::uint64_t stored{};
        std::memcpy(
            &stored, page.data() + (operand_slot & 0xfff), sizeof(stored));
        check(stored == 0x1122334455667788ull,
              "VMREAD to memory stored the wrong value");
    }

    // A memory-form VMREAD whose operand page cannot be written must
    // take #PF, not #UD.
    //
    // This is a regression test for a defect measured on a
    // three-processor Windows boot: the second processor executed
    // `VMREAD GUEST_RIP` to a kernel stack address this VMM could not
    // write, was told the instruction did not exist, and four log
    // entries later the guest hypervisor executed `vmxoff` on every
    // processor and Windows bugchecked HYPERVISOR_ERROR. #UD is a lie
    // about VMREAD; SDM 27.11.2 delivers memory-operand faults as any
    // other access would, and a guest given #PF pages the target in and
    // re-executes.
    {
        auto absent = 0xdead0000ull;
        check(!g_pages.count(absent), "the test's absent page exists");

        auto faulted = g_observed.pf_faults;
        auto refused = g_observed.ud_faults;

        g_page_present_only = true;

        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_rip;
        auto r = run(basic_reason::vmread,
                     regs,
                     memory_operand_information(1 /*reg2 = rcx*/),
                     absent);

        g_page_present_only = false;

        // The handler still returns false, so RIP stays on the
        // instruction - `outcome::ud` names that return value, not the
        // fault that was delivered.
        check(outcome::ud == r.what,
              "VMREAD to an unwritable operand did not leave RIP put");
        check(g_observed.pf_faults == (faulted + 1),
              "VMREAD to an unwritable operand injected no page fault");
        check(g_observed.ud_faults == refused,
              "VMREAD to an unwritable operand injected #UD as well");
        check(g_observed.last_pf_address == absent,
              "the injected page fault named the wrong address");

        // Write, and not-present: there is no translation for this page,
        // so SDM 4.7 bit 0 stays clear. Bit 2 is clear because VMREAD
        // outside CPL 0 has already taken #GP.
        check(g_observed.last_pf_error == 0x2,
              "the injected page fault carried the wrong error code");
    }

    // VMWRITE from memory.
    {
        std::uint64_t source = 0x99aabbccddeeff00ull;
        auto & page = page_of(operand_slot);
        std::memcpy(
            page.data() + (operand_slot & 0xfff), &source, sizeof(source));
        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_rsp;
        auto r = run(basic_reason::vmwrite,
                     regs,
                     memory_operand_information(1 /*reg2 = rcx*/),
                     operand_slot);
        expect("VMWRITE from memory", r, outcome::succeed);
        std::uint64_t got{};
        vmread_result(fields::guest_rsp, got);
        check(got == source, "VMWRITE from memory wrote the wrong value");
    }

    // The encoding register is read as a full 64-bit value. A guest that
    // leaves rubbish in the high half of the register - which is legal,
    // the architecture only defines the low bits as the encoding - must
    // still reach the field.
    {
        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_rip;
        regs.rax = 0x5555;
        auto r = run(basic_reason::vmwrite,
                     regs,
                     register_operand_information(0, 1));
        expect(
            "VMWRITE with a clean encoding register", r, outcome::succeed);
    }

    // RSP as the operand register: it is not in the captured context.
    {
        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_rip;
        hv().vmcs.guest_rsp(0x7777000);
        auto r = run(basic_reason::vmwrite,
                     regs,
                     register_operand_information(4 /*rsp*/, 1 /*rcx*/));
        expect("VMWRITE from RSP", r, outcome::succeed);
        std::uint64_t got{};
        vmread_result(fields::guest_rip, got);
        check(got == 0x7777000,
              "VMWRITE naming RSP as the source did not read the VMCS");
    }
    {
        zpp::arch::x86_64::context regs{};
        regs.rcx = fields::guest_cr3;
        vmwrite_field(fields::guest_cr3, 0x123000);
        auto r = run(basic_reason::vmread,
                     regs,
                     register_operand_information(4 /*rsp*/, 1 /*rcx*/));
        expect("VMREAD into RSP", r, outcome::succeed);
        check(
            hv().vmcs.guest_rsp() == 0x123000,
            "VMREAD naming RSP as the destination did not write the VMCS");
    }
}

// ------------------------------------------------- 6. INVEPT and INVVPID
static void test_invalidation()
{
    std::println("[6] INVEPT and INVVPID operand checking");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    auto descriptor = [](std::uint64_t low, std::uint64_t high) {
        auto & page = page_of(operand_slot);
        std::memcpy(
            page.data() + (operand_slot & 0xfff), &low, sizeof(low));
        std::memcpy(
            page.data() + (operand_slot & 0xfff) + 8, &high, sizeof(high));
    };

    auto invalidate = [&](basic_reason reason, std::uint64_t type) {
        zpp::arch::x86_64::context regs{};
        regs.rcx = type;
        return run(
            reason, regs, memory_operand_information(1), operand_slot);
    };

    descriptor(0x1000, 0);
    expect("INVEPT type 0",
           invalidate(basic_reason::invept, 0),
           outcome::fail_valid,
           28);
    expect("INVEPT type 3",
           invalidate(basic_reason::invept, 3),
           outcome::fail_valid,
           28);
    // Discard, because refresh_shadow_on_invept is off in nested_vmx.cpp
    // - this harness compiles that file, so it follows the switch rather
    // than describing an intention. Flip both together, and the refresh
    // counters below are here so that flip is one line.
    auto before = g_observed.ept_discards_for;
    expect("INVEPT single-context",
           invalidate(basic_reason::invept, 1),
           outcome::succeed);
    check(g_observed.ept_discards_for == before + 1,
          "INVEPT single-context did not invalidate the named shadow");
    check(g_observed.last_discard_root == 0x1000,
          "INVEPT single-context named the wrong root");
    expect("INVEPT all-context",
           invalidate(basic_reason::invept, 2),
           outcome::succeed);

    descriptor(0, 0);
    expect("INVVPID single-context with VPID 0",
           invalidate(basic_reason::invvpid, 1),
           outcome::fail_valid,
           28);
    expect("INVVPID all-context with VPID 0",
           invalidate(basic_reason::invvpid, 2),
           outcome::succeed);
    expect("INVVPID type 4",
           invalidate(basic_reason::invvpid, 4),
           outcome::fail_valid,
           28);

    descriptor(1, 0x0000800000000000ull);
    expect("INVVPID individual-address, non-canonical",
           invalidate(basic_reason::invvpid, 0),
           outcome::fail_valid,
           28);
    descriptor(1, 0x00007fffffffffffull);
    expect("INVVPID individual-address, canonical",
           invalidate(basic_reason::invvpid, 0),
           outcome::succeed);
}

// ------------------------------------- 7. the capability MSRs and VMXON
static void test_capability_msrs()
{
    std::println("[7] the capability MSRs");
    std::size_t cpu = 0;
    reset_cpu(cpu);

    auto read = [&](std::uint32_t index) {
        zpp::arch::x86_64::context regs{};
        hv().on_nested_vmx_msr_read(cpu, index, regs);
        return (regs.rax & 0xffffffff) | (regs.rdx << 32);
    };

    auto basic = read(vmxmsr::basic);
    check((basic & 0x7fffffff) == vmcs12::revision,
          "IA32_VMX_BASIC does not report the shadow's revision");
    check(((basic >> 32) & 0x1fff) == 4096,
          "IA32_VMX_BASIC does not report a 4096-byte region");

    auto enumeration = read(vmxmsr::vmcs_enum);
    check(((enumeration >> 1) & 0x1ff) ==
              (vmcs_field_encoding::index_capacity - 1),
          "IA32_VMX_VMCS_ENUM disagrees with the shadow's index capacity");

    auto misc = read(vmxmsr::misc);
    check(
        0 == (misc & (1ull << 29)),
        "IA32_VMX_MISC reports that VMWRITE may modify read-only fields");
    check(0 == ((misc >> 16) & 0x1ff),
          "IA32_VMX_MISC reports a non-zero CR3-target count");

    // The activity-state bitmap, SDM A.6 bits 8:6. The set offered has to
    // be exactly the set enter_or_park_l2 accepts, and it is KVM's -
    // active, HLT and wait-for-SIPI, in nested_check_guest_non_reg_state
    // (.references/kvm/nested.c:3117-3119). Shutdown is the one withheld:
    // SDM 29.7.2 has it block external interrupts as well as start-up
    // IPIs, so nothing here could end it.
    check(0 != (misc & (1ull << 6)),
          "IA32_VMX_MISC withholds the HLT activity state, which is "
          "entered in hardware");
    check(0 == (misc & (1ull << 7)),
          "IA32_VMX_MISC offers the shutdown activity state, which "
          "nothing here can end");
    check(0 != (misc & (1ull << 8)),
          "IA32_VMX_MISC withholds the wait-for-SIPI activity state, "
          "which enter_or_park_l2 honours");

    // Every control the narrowing offers must be one within_capability
    // would accept; and allowed-0 must always be a subset of allowed-1.
    struct
    {
        const char * text;
        std::size_t msr;
    } controls[] = {
        {"pin based", vmxmsr::pin_based_controls},
        {"true pin based", vmxmsr::true_pin_based_controls},
        {"primary", vmxmsr::processor_based_contorls},
        {"true primary", vmxmsr::true_processor_based_controls},
        {"exit", vmxmsr::exit_controls},
        {"true exit", vmxmsr::true_exit_controls},
        {"entry", vmxmsr::entry_controls},
        {"true entry", vmxmsr::true_entry_controls},
    };
    for (auto & c : controls) {
        auto value = read(c.msr);
        auto allowed_0 = value & 0xffffffff;
        auto allowed_1 = value >> 32;
        check((allowed_0 & ~allowed_1) == 0,
              std::string(c.text) +
                  " controls: a control that must be 1 is not allowed to "
                  "be 1");
    }

    // A write to any of them is a fault.
    {
        zpp::arch::x86_64::context regs{};
        auto before = g_observed.gp_faults;
        hv().on_nested_vmx_msr_write(cpu, vmxmsr::basic, regs);
        check(g_observed.gp_faults == before + 1,
              "a WRMSR to IA32_VMX_BASIC was not a fault");
    }

    // IA32_FEATURE_CONTROL is write-once.
    reset_cpu(cpu);
    hv().guest_feature_control[cpu] = 0;
    {
        zpp::arch::x86_64::context regs{};
        regs.rax = 0x5;
        hv().on_nested_vmx_msr_write(cpu, 0x3a, regs);
        check(hv().guest_feature_control[cpu] == 0x5,
              "the first write to IA32_FEATURE_CONTROL was dropped");
        auto before = g_observed.gp_faults;
        regs.rax = 0x1;
        hv().on_nested_vmx_msr_write(cpu, 0x3a, regs);
        check(g_observed.gp_faults == before + 1,
              "a second write to a locked IA32_FEATURE_CONTROL was not a "
              "fault");
        check(
            hv().guest_feature_control[cpu] == 0x5,
            "a second write to a locked IA32_FEATURE_CONTROL changed it");
    }
}

// ------------------------------------ 8. per-CPU indexing under a live L2
static void test_cpu_indexing()
{
    std::println("[8] per-processor state indexing");
    // Slot 0 is unreachable: cpu = vpid - 1 and vpid 0 wraps.
    reset_cpu(0);
    hv().vmcs.vpid(0);
    zpp::arch::x86_64::context regs{};
    auto r = run(basic_reason::vmxon, regs, memory_operand_information());
    check(r.what == outcome::ud,
          "a processor whose VPID is 0 was not refused outright");
    hv().vmcs.vpid(1);
}

// ------------------------------- 9. exhaustive sweep of the encoding
// space
static void test_encoding_sweep()
{
    std::println("[9] exhaustive sweep of the 15-bit encoding space");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    // The architectural predicate, from SDM Table 27-22: bit 12 and bits
    // 31:15 are reserved and must be zero, and the access type must be
    // full for 16-bit, 32-bit and natural-width fields. Anything else is
    // a structurally valid encoding, whether or not the field exists.
    auto structurally_valid = [](std::uint64_t e) {
        if (0 != (e & (1ull << 12))) {
            return false;
        }
        if (0 != (e & ~0x7fffull)) {
            return false;
        }
        auto width = (e >> 13) & 3;
        if ((0 != (e & 1)) && (width != 1)) {
            return false;
        }
        return true;
    };

    std::size_t accepted{}, refused_by_capacity{}, wrongly_accepted{},
        wrongly_refused{};
    std::map<std::uint64_t, std::uint64_t> slot_of;
    std::size_t aliases{};

    for (std::uint64_t e = 0; e < 0x8000; ++e) {
        auto w = vmwrite_field(e, 0x1111222233334444ull);
        bool taken = (w.what == outcome::succeed);
        bool read_only = (((e >> 10) & 3) == 1);

        if (read_only && structurally_valid(e) &&
            (((e >> 1) & 0x1ff) < vmcs_field_encoding::index_capacity)) {
            if (w.what != outcome::fail_valid || w.error != 13) {
                ++wrongly_accepted;
                std::println("      encoding {:#06x}: read-only field not "
                             "refused with error 13 ({} {})",
                             e,
                             name(w.what),
                             w.error);
            }
            continue;
        }

        if (!structurally_valid(e)) {
            if (taken) {
                ++wrongly_accepted;
                std::println(
                    "      encoding {:#06x}: structurally invalid "
                    "but accepted",
                    e);
            }
            continue;
        }

        if (((e >> 1) & 0x1ff) >= vmcs_field_encoding::index_capacity) {
            if (taken) {
                ++wrongly_accepted;
            } else {
                ++refused_by_capacity;
            }
            continue;
        }

        if (!taken) {
            ++wrongly_refused;
            std::println("      encoding {:#06x}: structurally valid and "
                         "within capacity but refused ({} {})",
                         e,
                         name(w.what),
                         w.error);
            continue;
        }
        ++accepted;
    }

    // Injectivity: give every accepted, writable encoding a unique value,
    // then read them all back. Two encodings sharing a slot show up as a
    // value that is not its own.
    std::vector<std::uint64_t> writable;
    for (std::uint64_t e = 0; e < 0x8000; ++e) {
        if (!structurally_valid(e)) {
            continue;
        }
        if (((e >> 1) & 0x1ff) >= vmcs_field_encoding::index_capacity) {
            continue;
        }
        if (((e >> 10) & 3) == 1) {
            continue;
        }
        if (0 != (e & 1)) {
            continue; // the high access type shares the low one's slot
        }
        writable.push_back(e);
    }
    for (auto e : writable) {
        vmwrite_field(e, 0xc0de000000000000ull | e);
    }
    for (auto e : writable) {
        std::uint64_t got{};
        vmread_result(e, got);
        auto width = (e >> 13) & 3;
        std::uint64_t want = 0xc0de000000000000ull | e;
        if (0 == width) {
            want &= 0xffff;
        } else if (2 == width) {
            want &= 0xffffffff;
        }
        if (got != want) {
            ++aliases;
            if (aliases < 8) {
                std::println("      encoding {:#06x} aliases: read {:#x} "
                             "wanted {:#x}",
                             e,
                             got,
                             want);
            }
        }
    }

    std::println("      {} accepted, {} refused only by index_capacity, "
                 "{} wrongly accepted, {} wrongly refused, {} aliasing",
                 accepted,
                 refused_by_capacity,
                 wrongly_accepted,
                 wrongly_refused,
                 aliases);
    check(wrongly_accepted == 0,
          "encodings accepted that Table 27-22 "
          "reserves");
    check(wrongly_refused == 0, "structurally valid encodings refused");
    check(aliases == 0, "two encodings share one shadow slot");
}

// ------------------------------------------- 10. read-only field handling
static void test_read_only_fields()
{
    std::println("[10] read-only VM-exit information fields");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    std::uint64_t values[] = {fields::vm_instruction_error,
                              fields::exit_reason,
                              fields::vm_exit_interruption_information,
                              fields::idt_vectoring_information_field,
                              fields::vm_exit_instruction_length,
                              fields::exit_qualification,
                              fields::guest_linear_address,
                              fields::guest_physical_address,
                              fields::io_rcx};
    for (auto f : values) {
        expect("VMWRITE to a read-only field",
               vmwrite_field(f, 1),
               outcome::fail_valid,
               13);
        std::uint64_t got{};
        expect("VMREAD of a read-only field",
               vmread_result(f, got),
               outcome::succeed);
    }

    // A VMfail must leave its error number where VMREAD can find it.
    vmwrite_field(0x8000, 1); // unsupported component, error 12
    std::uint64_t error{};
    vmread_result(fields::vm_instruction_error, error);
    check(error == 12,
          "the VM-instruction error field does not report the last "
          "failure");
}

// --------------------------------- 11. operand size outside IA-32e mode
static void test_operand_size()
{
    std::println("[11] the 32-bit operand size outside IA-32e mode");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    vmwrite_field(fields::guest_rip, 0x1122334455667788ull);

    // Drop out of IA-32e mode. CS.L must then be clear, which is legal.
    hv().vmcs.vm_entry_controls(0);
    hv().vmcs.guest_cs_access_rights(0xc09b);

    auto & page = page_of(operand_slot);
    std::memset(page.data() + (operand_slot & 0xfff), 0xcc, 16);
    zpp::arch::x86_64::context regs{};
    regs.rcx = fields::guest_rip;
    auto r = run(basic_reason::vmread,
                 regs,
                 memory_operand_information(1) | (1ull << 7),
                 operand_slot);
    expect("VMREAD to memory outside IA-32e mode", r, outcome::succeed);
    std::uint64_t stored{};
    std::memcpy(
        &stored, page.data() + (operand_slot & 0xfff), sizeof(stored));
    check((stored & 0xffffffff) == 0x55667788ull,
          "VMREAD to memory outside IA-32e mode stored the wrong 32 bits");
    check((stored >> 32) == 0xccccccccull,
          "VMREAD to memory outside IA-32e mode stored more than 32 bits");

    hv().vmcs.vm_entry_controls(1ull << 9);
    hv().vmcs.guest_cs_access_rights(0xa09b | (1ull << 13));
}

// ------------------------------ 12. the effective address of the operand
static void test_effective_address()
{
    std::println("[12] the effective address of a memory operand");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    // base + index * scale + displacement, 64-bit addressing.
    zpp::arch::x86_64::context regs{};
    regs.rbx = 0x40000;                // base, encoding 3
    regs.rsi = 0x10;                   // index, encoding 6
    auto information = (2ull << 7)     // address size 64
                       | (3ull << 23)  // base rbx
                       | (6ull << 18)  // index rsi
                       | (2ull << 0)   // scale 4
                       | (1ull << 28); // reg2 = rcx
    regs.rcx = fields::guest_rip;
    vmwrite_field(fields::guest_rip, 0xfeedfeed);
    std::memset(page_of(0x40040).data(), 0, 4096);
    auto r = run(basic_reason::vmread, regs, information, 0);
    expect("VMREAD with base + index*4", r, outcome::succeed);
    std::uint64_t stored{};
    std::memcpy(&stored, page_of(0x40040).data() + 0x40, sizeof(stored));
    check(stored == 0xfeedfeed,
          "the effective address of base + index*4 was wrong");

    // FS-relative, which is the one segment base that participates in
    // 64-bit mode.
    hv().vmcs.guest_fs_base(0x40000);
    regs = zpp::arch::x86_64::context{};
    regs.rcx = fields::guest_rip;
    auto fs_information = (2ull << 7) | (1ull << 22) | (1ull << 27) |
                          (4ull << 15) | (1ull << 28);
    std::memset(page_of(0x40080).data(), 0, 4096);
    expect("VMREAD through FS",
           run(basic_reason::vmread, regs, fs_information, 0x80),
           outcome::succeed);
    std::memcpy(&stored, page_of(0x40080).data() + 0x80, sizeof(stored));
    check(stored == 0xfeedfeed, "the FS base was not added");
    hv().vmcs.guest_fs_base(0);

    // A negative displacement, sign extended from the exit qualification.
    // In 64-bit mode the qualification carries the whole 64-bit value.
    regs = zpp::arch::x86_64::context{};
    regs.rcx = fields::guest_rip;
    regs.rbx = 0x41000;
    auto negative =
        (2ull << 7) | (3ull << 23) | (1ull << 22) | (1ull << 28);
    std::memset(page_of(0x40000).data(), 0, 4096);
    expect(
        "VMREAD at base - 0x1000",
        run(basic_reason::vmread, regs, negative, ~std::uint64_t(0xfff)),
        outcome::succeed);
    std::memcpy(&stored, page_of(0x40000).data(), sizeof(stored));
    check(stored == 0xfeedfeed, "a negative displacement was not applied");
}

// ------- 13. what is advertised versus what is implemented
//
// Every bit set in a capability MSR is a promise. This project's recurring
// failure mode is stated in CLAUDE.md as "answering *part* of an
// interface", and a capability MSR is the largest interface here: a guest
// hypervisor reads it once, believes it for the life of the machine, and
// builds a VMCS and its own page tables on the strength of it.
//
// So this section pairs each promise with the thing that has to be true
// for it to be kept, in both directions. A bit offered whose behaviour is
// missing is the defect. A bit withheld is not a defect, but it is a gap
// in what a guest hypervisor may use, and pinning it here is what stops
// the mask and the implementation drifting apart silently - which is the
// only way this class of defect ever arrives.
static void test_advertised_versus_implemented()
{
    std::println("[13] what is advertised versus what is implemented");
    std::size_t cpu = 0;
    reset_cpu(cpu);

    auto read = [&](std::uint32_t index) {
        zpp::arch::x86_64::context regs{};
        hv().on_nested_vmx_msr_read(cpu, index, regs);
        return (regs.rax & 0xffffffff) | (regs.rdx << 32);
    };

    // The allowed-1 half is what a guest hypervisor may set.
    auto offered = [&](std::uint32_t index) { return read(index) >> 32; };

    // --- The local APIC, offered in none of its forms -----------------
    //
    // Three secondary controls turn parts of the local APIC into
    // something the processor maintains: bit 0 virtualize APIC accesses,
    // bit 8 APIC-register virtualization, bit 9 virtual-interrupt
    // delivery. Each needs pages and state of its own that nothing here
    // maintains, so none is offered - and `within_capability` in
    // build_vmcs02 is what makes "not offered" mean "cannot be set"
    // rather than "we hope nobody tries".
    //
    // Worth stating because it answers a question that would otherwise
    // need an experiment: a guest hypervisor cannot produce the
    // inconsistent vmcs02 of virtual-interrupt delivery with the TPR
    // shadow dropped, because it cannot set virtual-interrupt delivery
    // at all. That pair is a VM-entry consistency check on hardware;
    // here it is refused a step earlier, at the control itself, which is
    // the stronger place to refuse it.
    struct
    {
        std::uint64_t bit;
        const char * name;
    } apic_controls[]{
        {0, "virtualize APIC accesses"},
        {8, "APIC-register virtualization"},
        {9, "virtual-interrupt delivery"},
    };

    auto secondary_offered = offered(0x48b);
    for (auto & entry : apic_controls) {
        check(0 == (secondary_offered & (1ull << entry.bit)),
              std::string("secondary control bit ") +
                  std::to_string(entry.bit) + " (" + entry.name +
                  ") is offered, and nothing here maintains the state it "
                  "needs");
    }

    // --- Posted interrupts and the preemption timer -------------------
    //
    // Pin bit 7 processes posted interrupts, which needs a descriptor and
    // a notification vector this VMM has neither of. Pin bit 6 activates
    // the VMX-preemption timer, which this VMM arms for its own use - so
    // a guest hypervisor's copy of the bit would mean nothing, and the
    // exits it produced would belong to the wrong layer.
    //
    // Both are withheld here, and build_vmcs02 strips both from the pin
    // union regardless. The strip is the belt; this is the braces.
    auto pin_offered = offered(0x481);
    check(0 == (pin_offered & (1ull << 6)),
          "pin control bit 6 (activate VMX-preemption timer) is offered, "
          "but this VMM arms the timer for its own use - a guest "
          "hypervisor's exits would be taken by the wrong layer");
    check(0 == (pin_offered & (1ull << 7)),
          "pin control bit 7 (process posted interrupts) is offered, and "
          "there is no posted-interrupt descriptor behind it");

    // The TRUE variants have to agree with the originals. This VMM sets
    // IA32_VMX_BASIC bit 55, which tells a guest hypervisor the TRUE MSRs
    // exist - and a guest hypervisor then reads whichever of the pair it
    // prefers. A disagreement between them is a guest reading a different
    // machine depending on which MSR it asked, which is the worst kind of
    // inconsistency because it depends on the guest's own code path.
    check(offered(0x48d) == pin_offered,
          "IA32_VMX_TRUE_PINBASED_CTLS offers a different set from "
          "IA32_VMX_PINBASED_CTLS");
    check(offered(0x48e) == offered(0x482),
          "IA32_VMX_TRUE_PROCBASED_CTLS offers a different set from "
          "IA32_VMX_PROCBASED_CTLS");
    check(offered(0x48f) == offered(0x483),
          "IA32_VMX_TRUE_EXIT_CTLS offers a different set from "
          "IA32_VMX_EXIT_CTLS");
    check(offered(0x490) == offered(0x484),
          "IA32_VMX_TRUE_ENTRY_CTLS offers a different set from "
          "IA32_VMX_ENTRY_CTLS");

    // --- VMCS shadowing -----------------------------------------------
    //
    // Secondary bit 14, not offered, and that is the honest answer: this
    // VMM builds no second shadow region for a guest hypervisor's own
    // guest to have its VMCS accesses caught in.
    //
    // The measurement that argues for implementing it is recorded in
    // nested_vmx.h - VMREAD and VMWRITE were 65.7% of every exit taken
    // while Hyper-V booted Windows - and the switch that turns on this
    // VMM's *own* use of shadowing is `shadow_vmcs_enabled`, deliberately
    // not a CMake option. Neither changes what a guest hypervisor may
    // set, which is what this asserts and all it asserts.
    check(0 == (secondary_offered & (1ull << 14)),
          "secondary control bit 14 (VMCS shadowing) is offered to a "
          "guest hypervisor, and there is no second shadow region for "
          "its own guest's accesses");

    // --- Mode-based execute control, the asymmetry in the other
    //     direction ---------------------------------------------------
    //
    // Secondary bit 22 **is** offered now, and `build_vmcs02` takes it
    // from the guest hypervisor's own controls rather than stripping it.
    // This assertion used to say the opposite, and its own comment asked
    // for the reversal to be a deliberate act rather than a side effect.
    // This is that act, so the assertion moves with it.
    //
    // Why: splitting execute in two is how a secure kernel expresses
    // code integrity through the extended page tables. Withholding it
    // left `HvCallModifyVtlProtectionMask` issued repeatedly with
    // `reflected_permission` at 0 of 448,441 - a protection change
    // accepted and expressible nowhere - while the same guest reaches
    // ring 3 under KVM alone, which advertises the bit.
    //
    // The pairing is what matters and is what this checks: offering the
    // capability while `build_vmcs02` cleared it unconditionally would
    // be the worst of both, a guest hypervisor setting the control and
    // getting nothing. It is taken from *its* controls, so a guest
    // hypervisor that does not ask still gets the old behaviour exactly.
    // **Inverted 2026-08-22.** The reasoning above is wrong in two
    // places and is kept only so the reversal is legible.
    //
    // "The composition already handles it" is true and beside the point:
    // `compose_ept` carries `execute_user`, but the lambda in
    // `nested_entry.cpp` that decides what a fault *means* tests
    // `permissions.execute()` - bit 2 - and never `execute_user()`, bit
    // 10. With the control on, a user-mode fetch is governed by bit 10,
    // so a user-executable page is reflected to the guest hypervisor for
    // an access its own tables allow, and a supervisor-only page is
    // quietly satisfied. Both resume onto the identical fault for ever.
    //
    // "the same guest reaches ring 3 under KVM alone, which advertises
    // the bit" is **false**: KVM's allow-list in
    // `nested_vmx_setup_ctls_msrs` omits
    // `SECONDARY_EXEC_MODE_BASED_EPT_EXEC`, its only nested mention is a
    // consistency check, and its nested walker is three-bit. The
    // baseline never had the bit, so the comparison did not test what it
    // claimed.
    //
    // Offering a capability whose fault handling cannot read it is worse
    // than withholding it - that is the "worst of both" the paragraph
    // above warns about, arrived at from the other direction.
    check(0 == (secondary_offered & (1ull << 22)),
          "secondary control bit 22 (mode-based execute control) is "
          "offered while the fault disposition still tests only "
          "supervisor execute - a guest hypervisor that sets it then "
          "faults for ever on pages its own tables permit");

    // --- The EPT and VPID capabilities --------------------------------
    //
    // IA32_VMX_EPT_VPID_CAP is where a promise is cheapest to make and
    // most expensive to break, because a guest hypervisor acts on it when
    // it *builds its own page tables* - long before anything here could
    // refuse the result.
    auto ept_vpid = read(0x48c);

    // Bit 0, execute-only translations. Withheld, and paired with
    // `execute_only_translations_offered` in the real hypervisor.h: the
    // composition in nested_ept.h consults that constant when deciding
    // what `normalised` may leave in an entry, so reporting the bit
    // without honouring it - or honouring it without reporting it - puts
    // the capability MSR and the permission composition at odds.
    //
    // Only one half is checkable here, because this harness compiles
    // against a shim hypervisor.h and comparing the mask against the
    // *shim's* copy of the constant would test the shim. The pairing
    // itself is a source-level invariant and is asserted in
    // scripts/ci/check-exit-handler.sh, which reads both real headers.
    check(0 == (ept_vpid & (1ull << 0)),
          "IA32_VMX_EPT_VPID_CAP bit 0 (execute-only translations) is "
          "offered, and normalised may not leave an execute-only entry "
          "unless execute_only_translations_offered says so");

    // Bit 21, accessed and dirty flags. Withheld, and build_vmcs02
    // refuses an EPT pointer that asks for them. Both halves matter:
    // withholding the bit while accepting the pointer leaves a guest
    // hypervisor's page tracking silently never marking anything, which
    // is the failure that looks like a guest bug for as long as it takes
    // to find.
    check(0 == (ept_vpid & (1ull << 21)),
          "IA32_VMX_EPT_VPID_CAP bit 21 (accessed and dirty flags) is "
          "offered, and nothing in the shadow ever sets either flag");

    // Bit 22, advanced VM-exit information for EPT violations. Withheld
    // because the reflected exit qualification does not carry bits 11:9.
    check(0 == (ept_vpid & (1ull << 22)),
          "IA32_VMX_EPT_VPID_CAP bit 22 (advanced VM-exit information) "
          "is offered, and the reflected qualification does not carry "
          "bits 11:9");

    // Bit 6, four-level walks: offered, and the only walk length the
    // shadow builds. A promise that is kept, asserted so that a shadow
    // gaining five-level support without the bit is caught too.
    check(0 != (ept_vpid & (1ull << 6)),
          "IA32_VMX_EPT_VPID_CAP bit 6 (four-level page walks) is "
          "withheld, and it is the only walk length the shadow builds");

    // Bits 16 and 17, 2 MB and 1 GB leaves. Both offered, and this is the
    // one place where "advertised" and "implemented" deliberately mean
    // different things: the bits report what a *guest hypervisor* may
    // write in its own tables, not what the shadow holds. A 1 GB guest
    // mapping is fanned out to 2 MB shadow entries, which implements it
    // rather than breaking the promise.
    check(0 != (ept_vpid & (1ull << 16)),
          "IA32_VMX_EPT_VPID_CAP bit 16 (2 MB EPT leaves) is withheld");
    check(0 != (ept_vpid & (1ull << 17)),
          "IA32_VMX_EPT_VPID_CAP bit 17 (1 GB EPT leaves) is withheld");

    // --- Every advertised invalidation type must be answered ----------
    //
    // The capability MSR names which INVEPT and INVVPID types exist. A
    // type reported but refused is the exact shape of the recurring
    // defect: the guest hypervisor believes an invalidation happened, and
    // a stale translation is silent until it is wrong.
    //
    // Section 6 above drives each type; this asserts the *set* it drives
    // is the set the MSR advertises, so adding a capability bit without
    // adding its answer fails here.
    {
        struct
        {
            std::uint64_t cap_bit;
            std::uint64_t type;
            const char * name;
        } types[]{
            {25, 1, "INVEPT single-context"},
            {26, 2, "INVEPT all-context"},
            {40, 0, "INVVPID individual-address"},
            {41, 1, "INVVPID single-context"},
            {42, 2, "INVVPID all-context"},
            {43, 3, "INVVPID single-context retaining globals"},
        };

        auto invept_supported = 0 != (ept_vpid & (1ull << 20));
        auto invvpid_supported = 0 != (ept_vpid & (1ull << 32));

        check(invept_supported,
              "IA32_VMX_EPT_VPID_CAP bit 20 (INVEPT) is withheld while "
              "its type bits are offered");
        check(invvpid_supported,
              "IA32_VMX_EPT_VPID_CAP bit 32 (INVVPID) is withheld while "
              "its type bits are offered");

        reset_cpu(cpu);
        enter_vmx(cpu);
        make_region(vmcs_a, vmcs12::revision);
        do_pointer_instruction(
            basic_reason::vmptrld, operand_slot, vmcs_a);

        auto descriptor = [](std::uint64_t low, std::uint64_t high) {
            auto & page = page_of(operand_slot);
            std::memcpy(
                page.data() + (operand_slot & 0xfff), &low, sizeof(low));
            std::memcpy(page.data() + (operand_slot & 0xfff) + 8,
                        &high,
                        sizeof(high));
        };

        for (auto & entry : types) {
            if (0 == (ept_vpid & (1ull << entry.cap_bit))) {
                continue;
            }

            auto is_invept = entry.cap_bit < 32;
            // A descriptor each type will accept: a non-zero EPT root for
            // INVEPT, a non-zero VPID with a canonical address for
            // INVVPID. The point of the case is the *type* being
            // answered, so the operand is deliberately the easy one.
            descriptor(is_invept ? 0x1000 : 1, 0);

            zpp::arch::x86_64::context regs{};
            regs.rcx = entry.type;
            auto result = run(is_invept ? basic_reason::invept
                                        : basic_reason::invvpid,
                              regs,
                              memory_operand_information(1),
                              operand_slot);

            check(outcome::succeed == result.what,
                  std::string(entry.name) +
                      " is advertised in IA32_VMX_EPT_VPID_CAP and is "
                      "not answered - a guest hypervisor believes the "
                      "invalidation happened");
        }
    }

    // --- The narrowing is a subset, never a superset ------------------
    //
    // Nothing this VMM answers may offer a control the hardware
    // underneath does not, whatever the supported mask says. `narrow`
    // ANDs the allowed-1 half with the supported set and ORs the
    // hardware's allowed-0 half back in, so the result is bounded above
    // by hardware - and this checks that against the same
    // `cached_vmx_msr` the code reads, so the two cannot be satisfied by
    // the same mistake.
    struct
    {
        std::uint32_t msr;
        const char * name;
    } narrowed[]{
        {0x481, "pin based"},
        {0x482, "primary"},
        {0x48b, "secondary"},
        {0x483, "exit"},
        {0x484, "entry"},
    };

    for (auto & entry : narrowed) {
        auto hardware = hv().cached_vmx_msr(entry.msr) >> 32;
        auto reported = offered(entry.msr);
        check(0 == (reported & ~hardware),
              std::string(entry.name) +
                  " controls offer a bit the hardware underneath does "
                  "not allow - the narrowing has become a widening");
    }

    // The EPT and VPID capabilities are masked rather than narrowed - a
    // plain AND, so the subset property is the whole of it.
    check(0 == (ept_vpid & ~hv().cached_vmx_msr(0x48c)),
          "IA32_VMX_EPT_VPID_CAP reports a capability the hardware "
          "underneath does not have");

    // --- IA32_VMX_MISC: the MSR-list capacity is a promise too --------
    //
    // Bits 27:25 give (N+1)*512 as the number of entries a VM-entry or
    // VM-exit MSR area may hold. This VMM clears them, promising 512 -
    // and it processes those areas in software rather than handing their
    // addresses to the processor, so the number it promises is the number
    // its own loop has to be willing to walk.
    auto misc = read(0x485);
    check(0 == ((misc >> 25) & 0x7),
          "IA32_VMX_MISC offers an MSR-list capacity above 512 entries, "
          "and the areas are walked in software");

    // And the CR3-target count, which is zero for the same kind of
    // reason: there is no CR3-target list here, so promising one would
    // invite a guest hypervisor to write values nothing consults.
    check(0 == ((misc >> 16) & 0x1ff),
          "IA32_VMX_MISC offers a CR3-target list, and nothing here "
          "consults one");
}

int main()
{
    hv().vmcs.vpid(1);
    test_field_round_trip();
    test_encoding_validation();
    test_launch_state_machine();
    test_vmclear_and_migration();
    test_memory_operands();
    test_invalidation();
    test_capability_msrs();
    test_cpu_indexing();
    test_encoding_sweep();
    test_read_only_fields();
    test_operand_size();
    test_effective_address();
    test_advertised_versus_implemented();
    check_guest_state_interception();

    std::println("\n{} checks, {} failures", g_checks, g_failures);
    if (!g_findings.empty()) {
        std::println("\nfindings:");
        for (auto & f : g_findings) {
            std::println("  - {}", f);
        }
    }
    return g_failures != 0;
}

static void check_guest_state_interception()
{
    constexpr auto guest_es_selector = 0x0800ull;
    constexpr auto guest_cs_access_rights = 0x4816ull;

    // A write of a guest-state field must be remembered as owed.
    auto before = zpp::hypervisor::g_guest_state_marked_dirty;
    static_cast<void>(vmwrite_field(guest_es_selector, 0x1234));
    check(zpp::hypervisor::g_guest_state_marked_dirty == before + 1,
          "a vmwrite of a guest-state field marks it owed, or the next "
          "entry leaves the level above's value in vmcs12 and never "
          "writes it into vmcs02");
    check(zpp::hypervisor::g_guest_state_last_encoding == guest_es_selector,
          "and marks the field it actually wrote");

    // A read must materialise first.
    auto read_before = zpp::hypervisor::g_guest_state_materialised;
    std::uint64_t got{};
    static_cast<void>(vmread_result(guest_es_selector, got));
    check(zpp::hypervisor::g_guest_state_materialised == read_before + 1,
          "a vmread of a guest-state field materialises the deferred "
          "copy before producing a value - the repair, not an assertion");

    // The shadowed pair's exclusion is asserted in `nested_exit`, which
    // compiles the translation unit those predicates live in.
    static_cast<void>(guest_cs_access_rights);

    // **The flush is a bulk consumer of vmcs12's guest-state area, and
    // it is the one no field-name grep could find.**
    //
    // `flush_guest_vmcs12` writes the *entire* vmcs12 structure back to
    // the guest hypervisor's own VMCS page, and it runs on VMPTRLD,
    // VMCLEAR and VMXOFF - VMPTRLD about four times per trust-level
    // round trip. With the guest-state copy deferred that area is
    // stale, so the flush overwrites the level above's record of its
    // guest with values from before the guest ran.
    //
    // Reasoning about what Hyper-V reads could never have found this:
    // the reader is inside this VMM, and it reads by `memcpy` of the
    // whole structure rather than by field.
    auto flushes = zpp::hypervisor::g_guest_state_full_materialise;
    hv().guest_current_vmcs[0] = 0x9000;
    hv().flush_guest_vmcs12(0);

    check(zpp::hypervisor::g_guest_state_full_materialise == flushes + 1,
          "flushing vmcs12 to the guest's own page materialises the "
          "deferred guest state first - otherwise the flush writes a "
          "stale guest-state area over the level above's record, on "
          "every VMPTRLD");

    hv().guest_current_vmcs[0] = 0;
}
