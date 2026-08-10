// Differential test harness for the nested VMX state machine.
//
// Compiles the real vmcs12 shadow and the real VMX-instruction emulation
// (hypervisor/src/hypervisor/nested_vmx.cpp) natively against a shim
// hypervisor, a shim VMCS and a fake guest physical memory, and drives
// them the way a guest hypervisor would.
#include "zpp/hypervisor/hypervisor.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace zpp;
using namespace zpp::arch::x86_64;
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

namespace zpp::hypervisor
{
hypervisor & hypervisor::instance()
{
    static hypervisor the;
    return the;
}

std::uint64_t hypervisor::cached_vmx_msr(std::size_t msr)
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

void hypervisor::inject_general_protection_fault()
{
    this->gp_faults = this->gp_faults + 1;
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

std::expected<void, zpp::error>
hypervisor::read_guest_physical(std::uint64_t physical,
                                std::span<std::byte> into)
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
        std::memcpy(into.data() + done, page_of(at).data() + offset, count);
        done += count;
    }
    return {};
}

std::expected<void, zpp::error>
hypervisor::write_guest_physical(std::uint64_t physical,
                                 std::span<const std::byte> from)
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
        std::memcpy(page_of(at).data() + offset, from.data() + done, count);
        done += count;
    }
    return {};
}

void hypervisor::discard_shadow_ept(std::size_t)
{
    this->ept_discards = this->ept_discards + 1;
}

void hypervisor::discard_shadow_ept_for(std::size_t, std::uint64_t root)
{
    this->ept_discards_for = this->ept_discards_for + 1;
    this->last_discard_root = root;
}

void hypervisor::nested_transition_flush()
{
    this->flushes = this->flushes + 1;
}

std::expected<void, zpp::error> hypervisor::build_vmcs02(std::size_t)
{
    if (this->build_vmcs02_fails) {
        return std::unexpected(
            zpp::error{error::nested_control_unsupported});
    }
    return {};
}

hypervisor::l2_entry_outcome hypervisor::enter_or_park_l2(std::size_t)
{
    this->enter_or_park_l2_calls = this->enter_or_park_l2_calls + 1;
    return this->enter_or_park_l2_outcome;
}

void hypervisor::reflect_l2_exit(std::size_t,
                                 arch::x86_64::vmx::exit_reason,
                                 std::uint64_t)
{
}

std::uint64_t hypervisor::own_vmcs_region_physical()
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
        std::printf("  FAIL %s\n", what.c_str());
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
    ud,          // handler returned false: caller injects #UD
    gp,          // a general protection fault was injected
    succeed,     // VMsucceed
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
    v.guest_cr0(1);                                    // PE
    v.guest_rflags(0x2);                               // no VM, no arith
    v.cr4_read_shadow(1ull << 13);                     // VMXE
    v.guest_cs_access_rights(0xa09b | (1ull << 13));   // L = 1
    v.guest_ss_access_rights(0xc093);                  // DPL 0
    v.vm_entry_controls(1ull << 9);                    // IA-32e mode guest
    v.guest_fs_base(0);
    v.guest_gs_base(0);
}

// Instruction-information field for a memory operand with no base and no
// index, so the linear address is exactly the displacement.
static std::uint64_t memory_operand_information(std::uint64_t reg2 = 0,
                                                std::uint64_t reg1 = 0)
{
    return (2ull << 7)          // address size 64-bit
           | (1ull << 22)       // index invalid
           | (1ull << 27)       // base invalid
           | (0ull << 15)       // segment ES (ignored in 64-bit)
           | ((reg1 & 0xf) << 3) | ((reg2 & 0xf) << 28);
}

static std::uint64_t register_operand_information(std::uint64_t reg1,
                                                  std::uint64_t reg2)
{
    return (1ull << 10) | ((reg1 & 0xf) << 3) | ((reg2 & 0xf) << 28) |
           (2ull << 7);
}

static result run(basic_reason reason,
                  context & regs,
                  std::uint64_t information,
                  std::uint64_t qualification = 0)
{
    auto & v = hv().vmcs;
    v.vm_exit_instruction_information(information);
    zpp::arch::x86_64::vmx::g_vmcs[zpp::arch::x86_64::vmx::
        vmcs_fields::exit_qualification] = qualification;
    v.guest_rflags((v.guest_rflags() & ~0x8d5ull) | 0x2);

    auto faults = hv().gp_faults;
    auto handled = hv().on_vmx_instruction(
        zpp::arch::x86_64::vmx::exit_reason(
            static_cast<std::uint64_t>(reason)),
        regs);

    if (hv().gp_faults != faults) {
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
                hv().guest_vmcs12[cpu].read(
                    fields::vm_instruction_error)};
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
        char buffer[512];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s: got %s(%llu), wanted %s(%llu)",
                      what,
                      name(got.what),
                      (unsigned long long)got.error,
                      name(want),
                      (unsigned long long)want_error);
        std::printf("  FAIL %s\n", buffer);
        g_findings.push_back(buffer);
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

    context regs{};
    return run(reason, regs, memory_operand_information(), operand_address);
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
    hv().build_vmcs02_fails = false;
    hv().enter_or_park_l2_outcome =
        hypervisor_t::l2_entry_outcome::entered;
    hv().enter_or_park_l2_calls = 0;
    arm_guest(cpu);
}

static void enter_vmx(std::size_t cpu)
{
    make_region(vmxon_region, vmcs12::revision);
    auto r = do_pointer_instruction(basic_reason::vmxon,
                                    operand_slot,
                                    vmxon_region);
    if (r.what != outcome::succeed) {
        std::printf("  setup: vmxon failed (%s)\n", name(r.what));
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

static std::uint64_t vmread_field(std::size_t cpu, std::uint64_t encoding)
{
    context regs{};
    regs.rcx = encoding;              // reg2 = rcx (1)
    auto r = run(basic_reason::vmread,
                 regs,
                 register_operand_information(0 /*rax*/, 1 /*rcx*/));
    if (r.what != outcome::succeed) {
        return ~std::uint64_t(0);
    }
    (void)cpu;
    return regs.rax;
}

static result vmwrite_field(std::uint64_t encoding, std::uint64_t value)
{
    context regs{};
    regs.rcx = encoding;
    regs.rax = value;
    return run(basic_reason::vmwrite,
               regs,
               register_operand_information(0 /*rax*/, 1 /*rcx*/));
}

static result vmread_result(std::uint64_t encoding, std::uint64_t & out)
{
    context regs{};
    regs.rcx = encoding;
    auto r = run(basic_reason::vmread,
                 regs,
                 register_operand_information(0, 1));
    out = regs.rax;
    return r;
}

static void test_field_round_trip()
{
    std::printf("[1] VMWRITE/VMREAD round trip over vmcs_fields.h\n");
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
            char buffer[256];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "VMWRITE %s (encoding %#llx, index %llu) refused: "
                          "%s(%llu)",
                          f.text,
                          (unsigned long long)f.encoding,
                          (unsigned long long)e.index(),
                          name(w.what),
                          (unsigned long long)w.error);
            ++g_checks;
            ++g_failures;
            std::printf("  FAIL %s\n", buffer);
            g_findings.push_back(buffer);
            continue;
        }

        std::uint64_t got{};
        auto r = vmread_result(f.encoding, got);
        if (r.what != outcome::succeed) {
            check(false,
                  std::string("VMREAD ") + f.text + " refused after a "
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
            char buffer[256];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s (%#llx): wrote %#llx read back %#llx, wanted "
                          "%#llx",
                          f.text,
                          (unsigned long long)f.encoding,
                          (unsigned long long)pattern,
                          (unsigned long long)got,
                          (unsigned long long)want);
            ++g_checks;
            ++g_failures;
            std::printf("  FAIL %s\n", buffer);
            g_findings.push_back(buffer);
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
    std::printf("[2] encoding validation and width/type decode\n");
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
        std::printf("      %-46s index %2llu -> VMWRITE %s(%llu) VMREAD "
                    "%s(%llu)\n",
                    b.text,
                    (unsigned long long)e.index(),
                    name(w.what),
                    (unsigned long long)w.error,
                    name(r.what),
                    (unsigned long long)r.error);
    }
}

// ---------------------------------------------------- 3. launch machine
static void test_launch_state_machine()
{
    std::printf("[3] the launch state machine\n");
    std::size_t cpu = 0;
    reset_cpu(cpu);

    // Not in VMX operation: every instruction but VMXON is #UD.
    context regs{};
    expect("VMREAD outside VMX operation",
           run(basic_reason::vmread, regs, register_operand_information(0, 1)),
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
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::ud);
    hv().vmcs.cr4_read_shadow(1ull << 13);

    // CPL above zero.
    hv().vmcs.guest_ss_access_rights(0xc0f3);
    expect("VMXON at CPL 3",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::gp);
    hv().vmcs.guest_ss_access_rights(0xc093);

    // Virtual-8086 mode and real mode.
    hv().vmcs.guest_rflags(0x2 | (1ull << 17));
    expect("VMXON in virtual-8086 mode",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::ud);
    hv().vmcs.guest_rflags(0x2);
    hv().vmcs.guest_cr0(0);
    expect("VMXON with CR0.PE clear",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::ud);
    hv().vmcs.guest_cr0(1);

    // Compatibility mode: LMA set, CS.L clear (bit 13 of the access
    // rights). 0xa09b already carries L, so the D/B form is used.
    hv().vmcs.guest_cs_access_rights(0xc09b);
    expect("VMXON in compatibility mode",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::ud);
    hv().vmcs.guest_cs_access_rights(0xa09b | (1ull << 13));

    // VMXON with the feature control MSR unlocked.
    hv().guest_feature_control[cpu] = 0;
    expect("VMXON with IA32_FEATURE_CONTROL unlocked",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::gp);
    hv().guest_feature_control[cpu] = 0x5;

    // Misaligned region, and a register operand.
    expect("VMXON with a misaligned region",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region + 8),
           outcome::fail_invalid);
    {
        context r2{};
        expect("VMXON with a register operand",
               run(basic_reason::vmxon, r2,
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
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::fail_invalid);

    // Good.
    make_region(vmxon_region, vmcs12::revision);
    expect("VMXON",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::succeed);

    // Again. SDM 33.2 VMfail: with no current VMCS this is VMfailInvalid,
    // not VMfailValid(15) - the error number has nowhere to go.
    expect("VMXON in VMX root operation, no current VMCS",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::fail_invalid);

    // No current VMCS yet.
    expect("VMLAUNCH with no current VMCS",
           run(basic_reason::vmlaunch, regs, 0),
           outcome::fail_invalid);
    expect("VMRESUME with no current VMCS",
           run(basic_reason::vmresume, regs, 0),
           outcome::fail_invalid);
    expect("VMREAD with no current VMCS",
           run(basic_reason::vmread, regs, register_operand_information(0, 1)),
           outcome::fail_invalid);
    expect("VMWRITE with no current VMCS",
           vmwrite_field(fields::guest_rip, 1),
           outcome::fail_invalid);
    expect("VMCALL with no current VMCS",
           run(basic_reason::vmcall, regs, 0),
           outcome::fail_invalid);

    // VMPTRST with no current VMCS writes the all-ones sentinel.
    {
        context r2{};
        run(basic_reason::vmptrst, r2, memory_operand_information(),
            operand_slot);
        std::uint64_t stored{};
        std::memcpy(&stored,
                    page_of(operand_slot).data() + (operand_slot & 0xfff),
                    sizeof(stored));
        check(stored == ~std::uint64_t(0),
              "VMPTRST with no current VMCS did not store FFFFFFFFFFFFFFFFH");
    }

    // A good VMPTRLD, so that from here every VMfail carries an error.
    make_region(vmcs_a, vmcs12::revision);
    expect("VMPTRLD",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_a),
           outcome::succeed);
    check(hv().guest_current_vmcs[cpu] == vmcs_a,
          "VMPTRLD did not make the region current");

    // Now the error numbers, SDM Table 33-1.
    expect("VMXON in VMX root operation",
           do_pointer_instruction(basic_reason::vmxon,
                                  operand_slot,
                                  vmxon_region),
           outcome::fail_valid, 15);
    expect("VMCALL in VMX root operation",
           run(basic_reason::vmcall, regs, 0),
           outcome::fail_valid, 1);
    expect("VMPTRLD with the VMXON pointer",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmxon_region),
           outcome::fail_valid, 10);
    expect("VMCLEAR with the VMXON pointer",
           do_pointer_instruction(basic_reason::vmclear,
                                  operand_slot,
                                  vmxon_region),
           outcome::fail_valid, 3);
    expect("VMPTRLD with a misaligned address",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_b + 8),
           outcome::fail_valid, 9);
    expect("VMCLEAR with a misaligned address",
           do_pointer_instruction(basic_reason::vmclear,
                                  operand_slot,
                                  vmcs_b + 8),
           outcome::fail_valid, 2);

    // A region with the wrong revision.
    make_region(vmcs_b, 0xdeadbeef);
    expect("VMPTRLD with the wrong revision identifier",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_b),
           outcome::fail_valid, 11);
    check(hv().guest_current_vmcs[cpu] == vmcs_a,
          "a refused VMPTRLD changed the current VMCS anyway");

    // VMPTRLD of the already-current VMCS must not reload it, which is
    // the regression the cache-over-region bug left behind.
    vmwrite_field(fields::guest_rip, 0x1234);
    expect("VMPTRLD of the already-current VMCS",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_a),
           outcome::succeed);
    std::uint64_t rip{};
    vmread_result(fields::guest_rip, rip);
    check(rip == 0x1234,
          "a redundant VMPTRLD discarded the cached shadow");

    // VMRESUME before any VMLAUNCH.
    expect("VMRESUME on a clear VMCS",
           run(basic_reason::vmresume, regs, 0),
           outcome::fail_valid, 5);

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
    std::printf("      launch state after a successful VMLAUNCH: %s\n",
                hv().guest_vmcs12[cpu].state() ==
                        vmcs12::launch_state::launched
                    ? "launched"
                    : "clear");

    check(1 == hv().enter_or_park_l2_calls,
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
        hv().enter_or_park_l2_outcome = outcome_case;

        expect("VMLAUNCH the guest-state decision declined",
               run(basic_reason::vmlaunch, regs, 0),
               outcome::succeed);
        check(!hv().running_l2[cpu],
              "a declined VM entry left the processor running L2");
        check(hv().nested_rip_settled[cpu],
              "a declined VM entry let the caller advance RIP");
    }

    hv().enter_or_park_l2_outcome =
        hypervisor_t::l2_entry_outcome::entered;

    // A refused build_vmcs02 must leave the launch state alone and tell
    // the guest hypervisor its entry did not happen.
    hv().running_l2[cpu] = false;
    hv().build_vmcs02_fails = true;
    expect("VMLAUNCH refused by build_vmcs02",
           run(basic_reason::vmlaunch, regs, 0),
           outcome::fail_valid, 7);
    check(!hv().running_l2[cpu],
          "a refused VMLAUNCH left the processor marked as running L2");
    hv().build_vmcs02_fails = false;
}

// ------------------------------------------ 4. VMCLEAR / migration cycle
static void test_vmclear_and_migration()
{
    std::printf("[4] VMCLEAR, and the migration cycle it exists for\n");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);

    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    vmwrite_field(fields::guest_rip, 0xcafe0000);
    vmwrite_field(fields::pin_based_vm_execution_controls, 0x1e);

    // VMCLEAR of the current VMCS: the fields must survive.
    expect("VMCLEAR of the current VMCS",
           do_pointer_instruction(basic_reason::vmclear,
                                  operand_slot,
                                  vmcs_a),
           outcome::succeed);
    check(hv().guest_current_vmcs[cpu] ==
              zpp::hypervisor::nested_vmx::no_current_vmcs,
          "VMCLEAR of the current VMCS left it current");

    // Reload and confirm the data came back.
    expect("VMPTRLD after VMCLEAR",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_a),
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
           do_pointer_instruction(basic_reason::vmclear,
                                  operand_slot,
                                  vmcs_b),
           outcome::succeed);
    expect("VMPTRLD of the other VMCS",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_b),
           outcome::succeed);
    expect("VMPTRLD back to the first",
           do_pointer_instruction(basic_reason::vmptrld,
                                  operand_slot,
                                  vmcs_a),
           outcome::succeed);
    vmread_result(fields::guest_rip, rip);
    check(rip == 0xbeef0000,
          "a VMWRITE made after the last VMCLEAR did not reach the region, "
          "so switching VMCS pointers loses it");

    // The launch state must survive the round trip through memory too.
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);
    context regs{};
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
           run(basic_reason::vmread, regs, register_operand_information(0, 1)),
           outcome::ud);
}

// --------------------------------------------- 5. VMREAD/VMWRITE memory
static void test_memory_operands()
{
    std::printf("[5] the memory forms of VMREAD and VMWRITE\n");
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
        context regs{};
        regs.rcx = fields::guest_rip;
        auto r = run(basic_reason::vmread,
                     regs,
                     memory_operand_information(1 /*reg2 = rcx*/),
                     operand_slot);
        expect("VMREAD to memory", r, outcome::succeed);
        std::uint64_t stored{};
        std::memcpy(&stored,
                    page.data() + (operand_slot & 0xfff),
                    sizeof(stored));
        check(stored == 0x1122334455667788ull,
              "VMREAD to memory stored the wrong value");
    }

    // VMWRITE from memory.
    {
        std::uint64_t source = 0x99aabbccddeeff00ull;
        auto & page = page_of(operand_slot);
        std::memcpy(page.data() + (operand_slot & 0xfff),
                    &source,
                    sizeof(source));
        context regs{};
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
        context regs{};
        regs.rcx = fields::guest_rip;
        regs.rax = 0x5555;
        auto r = run(basic_reason::vmwrite,
                     regs,
                     register_operand_information(0, 1));
        expect("VMWRITE with a clean encoding register", r,
               outcome::succeed);
    }

    // RSP as the operand register: it is not in the captured context.
    {
        context regs{};
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
        context regs{};
        regs.rcx = fields::guest_cr3;
        vmwrite_field(fields::guest_cr3, 0x123000);
        auto r = run(basic_reason::vmread,
                     regs,
                     register_operand_information(4 /*rsp*/, 1 /*rcx*/));
        expect("VMREAD into RSP", r, outcome::succeed);
        check(hv().vmcs.guest_rsp() == 0x123000,
              "VMREAD naming RSP as the destination did not write the VMCS");
    }
}

// ------------------------------------------------- 6. INVEPT and INVVPID
static void test_invalidation()
{
    std::printf("[6] INVEPT and INVVPID operand checking\n");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    auto descriptor = [](std::uint64_t low, std::uint64_t high) {
        auto & page = page_of(operand_slot);
        std::memcpy(page.data() + (operand_slot & 0xfff), &low,
                    sizeof(low));
        std::memcpy(page.data() + (operand_slot & 0xfff) + 8, &high,
                    sizeof(high));
    };

    auto invalidate = [&](basic_reason reason, std::uint64_t type) {
        context regs{};
        regs.rcx = type;
        return run(reason, regs, memory_operand_information(1), operand_slot);
    };

    descriptor(0x1000, 0);
    expect("INVEPT type 0", invalidate(basic_reason::invept, 0),
           outcome::fail_valid, 28);
    expect("INVEPT type 3", invalidate(basic_reason::invept, 3),
           outcome::fail_valid, 28);
    auto before = hv().ept_discards_for;
    expect("INVEPT single-context", invalidate(basic_reason::invept, 1),
           outcome::succeed);
    check(hv().ept_discards_for == before + 1,
          "INVEPT single-context did not discard the named shadow");
    check(hv().last_discard_root == 0x1000,
          "INVEPT single-context named the wrong root");
    expect("INVEPT all-context", invalidate(basic_reason::invept, 2),
           outcome::succeed);

    descriptor(0, 0);
    expect("INVVPID single-context with VPID 0",
           invalidate(basic_reason::invvpid, 1),
           outcome::fail_valid, 28);
    expect("INVVPID all-context with VPID 0",
           invalidate(basic_reason::invvpid, 2),
           outcome::succeed);
    expect("INVVPID type 4", invalidate(basic_reason::invvpid, 4),
           outcome::fail_valid, 28);

    descriptor(1, 0x0000800000000000ull);
    expect("INVVPID individual-address, non-canonical",
           invalidate(basic_reason::invvpid, 0),
           outcome::fail_valid, 28);
    descriptor(1, 0x00007fffffffffffull);
    expect("INVVPID individual-address, canonical",
           invalidate(basic_reason::invvpid, 0),
           outcome::succeed);
}

// ------------------------------------- 7. the capability MSRs and VMXON
static void test_capability_msrs()
{
    std::printf("[7] the capability MSRs\n");
    std::size_t cpu = 0;
    reset_cpu(cpu);

    auto read = [&](std::uint32_t index) {
        context regs{};
        hv().on_nested_vmx_msr_read(index, regs);
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
    check(0 == (misc & (1ull << 29)),
          "IA32_VMX_MISC reports that VMWRITE may modify read-only fields");
    check(0 == ((misc >> 16) & 0x1ff),
          "IA32_VMX_MISC reports a non-zero CR3-target count");

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
        context regs{};
        auto before = hv().gp_faults;
        hv().on_nested_vmx_msr_write(vmxmsr::basic, regs);
        check(hv().gp_faults == before + 1,
              "a WRMSR to IA32_VMX_BASIC was not a fault");
    }

    // IA32_FEATURE_CONTROL is write-once.
    reset_cpu(cpu);
    hv().guest_feature_control[cpu] = 0;
    {
        context regs{};
        regs.rax = 0x5;
        hv().on_nested_vmx_msr_write(0x3a, regs);
        check(hv().guest_feature_control[cpu] == 0x5,
              "the first write to IA32_FEATURE_CONTROL was dropped");
        auto before = hv().gp_faults;
        regs.rax = 0x1;
        hv().on_nested_vmx_msr_write(0x3a, regs);
        check(hv().gp_faults == before + 1,
              "a second write to a locked IA32_FEATURE_CONTROL was not a "
              "fault");
        check(hv().guest_feature_control[cpu] == 0x5,
              "a second write to a locked IA32_FEATURE_CONTROL changed it");
    }
}

// ------------------------------------ 8. per-CPU indexing under a live L2
static void test_cpu_indexing()
{
    std::printf("[8] per-processor state indexing\n");
    // Slot 0 is unreachable: cpu = vpid - 1 and vpid 0 wraps.
    reset_cpu(0);
    hv().vmcs.vpid(0);
    context regs{};
    auto r = run(basic_reason::vmxon, regs, memory_operand_information());
    check(r.what == outcome::ud,
          "a processor whose VPID is 0 was not refused outright");
    hv().vmcs.vpid(1);
}

// ------------------------------- 9. exhaustive sweep of the encoding space
static void test_encoding_sweep()
{
    std::printf("[9] exhaustive sweep of the 15-bit encoding space\n");
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
                std::printf("      encoding %#06llx: read-only field not "
                            "refused with error 13 (%s %llu)\n",
                            (unsigned long long)e,
                            name(w.what),
                            (unsigned long long)w.error);
            }
            continue;
        }

        if (!structurally_valid(e)) {
            if (taken) {
                ++wrongly_accepted;
                std::printf("      encoding %#06llx: structurally invalid "
                            "but accepted\n",
                            (unsigned long long)e);
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
            std::printf("      encoding %#06llx: structurally valid and "
                        "within capacity but refused (%s %llu)\n",
                        (unsigned long long)e,
                        name(w.what),
                        (unsigned long long)w.error);
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
                std::printf("      encoding %#06llx aliases: read %#llx "
                            "wanted %#llx\n",
                            (unsigned long long)e,
                            (unsigned long long)got,
                            (unsigned long long)want);
            }
        }
    }

    std::printf("      %zu accepted, %zu refused only by index_capacity, "
                "%zu wrongly accepted, %zu wrongly refused, %zu aliasing\n",
                accepted,
                refused_by_capacity,
                wrongly_accepted,
                wrongly_refused,
                aliases);
    check(wrongly_accepted == 0, "encodings accepted that Table 27-22 "
                                 "reserves");
    check(wrongly_refused == 0, "structurally valid encodings refused");
    check(aliases == 0, "two encodings share one shadow slot");
}

// ------------------------------------------- 10. read-only field handling
static void test_read_only_fields()
{
    std::printf("[10] read-only VM-exit information fields\n");
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
    std::printf("[11] the 32-bit operand size outside IA-32e mode\n");
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
    context regs{};
    regs.rcx = fields::guest_rip;
    auto r = run(basic_reason::vmread,
                 regs,
                 memory_operand_information(1) | (1ull << 7),
                 operand_slot);
    expect("VMREAD to memory outside IA-32e mode", r, outcome::succeed);
    std::uint64_t stored{};
    std::memcpy(&stored, page.data() + (operand_slot & 0xfff),
                sizeof(stored));
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
    std::printf("[12] the effective address of a memory operand\n");
    std::size_t cpu = 0;
    reset_cpu(cpu);
    enter_vmx(cpu);
    make_region(vmcs_a, vmcs12::revision);
    do_pointer_instruction(basic_reason::vmptrld, operand_slot, vmcs_a);

    // base + index * scale + displacement, 64-bit addressing.
    context regs{};
    regs.rbx = 0x40000;    // base, encoding 3
    regs.rsi = 0x10;       // index, encoding 6
    auto information = (2ull << 7)          // address size 64
                       | (3ull << 23)       // base rbx
                       | (6ull << 18)       // index rsi
                       | (2ull << 0)        // scale 4
                       | (1ull << 28);      // reg2 = rcx
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
    regs = context{};
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
    regs = context{};
    regs.rcx = fields::guest_rip;
    regs.rbx = 0x41000;
    auto negative = (2ull << 7) | (3ull << 23) | (1ull << 22) |
                    (1ull << 28);
    std::memset(page_of(0x40000).data(), 0, 4096);
    expect("VMREAD at base - 0x1000",
           run(basic_reason::vmread, regs, negative,
               ~std::uint64_t(0xfff)),
           outcome::succeed);
    std::memcpy(&stored, page_of(0x40000).data(), sizeof(stored));
    check(stored == 0xfeedfeed,
          "a negative displacement was not applied");
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (!g_findings.empty()) {
        std::printf("\nfindings:\n");
        for (auto & f : g_findings) {
            std::printf("  - %s\n", f.c_str());
        }
    }
    return g_failures != 0;
}
