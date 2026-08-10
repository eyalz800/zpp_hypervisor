// Differential test harness for the nested VM-exit reflection decision.
//
// Compiles the real hypervisor/src/hypervisor/nested_entry.cpp natively
// against a shim hypervisor, a shim VMCS and a fake guest physical memory,
// and drives `l0_wants_l2_exit` and `l1_wants_l2_exit` the way a VM exit
// taken by a second-level guest would.
//
// The expected column is not this harness's opinion. Every entry in the
// table below was read out of KVM's `nested_vmx_l1_wants_exit` and
// `nested_vmx_l0_wants_exit` in .references/kvm/nested.c, case by case,
// and every case where the two implementations legitimately disagree
// carries the reason in `divergence` - which is *asserted*, so a
// divergence that silently disappears fails the test just as a new one
// does.
#include "zpp/hypervisor/hypervisor.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace zpp;
using namespace zpp::arch::x86_64;
using zpp::arch::x86_64::vmx::vmcs12;
using basic_reason = zpp::arch::x86_64::vmx::exit_reason::basic_reason;
namespace fields = zpp::arch::x86_64::vmx::vmcs_fields;
using field = zpp::arch::x86_64::vmx::vmcs::field;

// ---------------------------------------------------------------- memory
static std::map<std::uint64_t, std::vector<std::byte>> g_pages;
static std::map<std::uint64_t, bool> g_unreadable;

static std::vector<std::byte> & page_of(std::uint64_t physical)
{
    auto base = physical & ~std::uint64_t(0xfff);
    auto it = g_pages.find(base);
    if (it == g_pages.end()) {
        it = g_pages.emplace(base, std::vector<std::byte>(4096)).first;
    }
    return it->second;
}

// ------------------------------------------------------- shim definitions
namespace zpp::arch::x86_64
{
std::uint64_t rdmsr(std::uint32_t)
{
    return 0;
}

void wrmsr(std::uint32_t, std::uint64_t)
{
}
} // namespace zpp::arch::x86_64

namespace zpp::hypervisor
{
hypervisor & hypervisor::instance()
{
    static hypervisor the;
    return the;
}

std::uint64_t hypervisor::cached_vmx_msr(std::size_t msr)
{
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
        return 0x7004c1e7ull;
    case 0x48b:
        return 0xfffffffull << 32;
    case 0x48c:
        return 0xf0106734141ull;
    default:
        return 0;
    }
}

std::uint64_t hypervisor::nested_vmx_capability_msr(std::size_t msr)
{
    return cached_vmx_msr(msr);
}

std::uint64_t hypervisor::physical_address_bits()
{
    return 39;
}

std::uint64_t hypervisor::guest_register(const arch::x86_64::context & c,
                                         std::uint64_t encoding)
{
    constexpr std::uint64_t encoded_rsp = 4;
    if (encoded_rsp == encoding) {
        return this->vmcs.guest_rsp();
    }
    if (encoding >= 16) {
        return 0;
    }
    return c.*arch::x86_64::detail::encoded_registers[encoding];
}

std::expected<void, zpp::error> hypervisor::read_guest_physical(
    std::uint64_t physical, std::span<std::byte> into)
{
    if (g_unreadable.count(physical & ~std::uint64_t(0xfff))) {
        return std::unexpected(
            zpp::error{error::guest_memory_unreachable});
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
    if (g_unreadable.count(physical & ~std::uint64_t(0xfff))) {
        return std::unexpected(
            zpp::error{error::guest_memory_unreachable});
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

std::expected<std::uint64_t, zpp::error>
hypervisor::shadow_ept_pointer_for(std::size_t, std::uint64_t)
{
    return 0x1000ull | (3ull << 3) | 6;
}

std::expected<void, zpp::error>
hypervisor::fill_shadow_leaf(std::size_t,
                             std::uint64_t,
                             const arch::x86_64::vmx::ept_walk_result &,
                             std::uint64_t)
{
    return {};
}

arch::x86_64::vmx::ept_walk_result
hypervisor::host_ept_lookup(std::uint64_t physical_address)
{
    arch::x86_64::vmx::ept_walk_result result;
    result.status = arch::x86_64::vmx::ept_walk_status::mapped;
    result.physical_address = physical_address;
    result.permissions = arch::x86_64::vmx::ept_permissions::all();
    return result;
}

void hypervisor::record_exit(arch::x86_64::vmx::exit_reason)
{
}

/**
 * The start-up IPI hand-off, reduced to whatever the test armed. The real
 * one spins in VMX root operation on a mailbox another processor writes,
 * which is not something one process can have.
 */
} // namespace zpp::hypervisor

std::optional<std::uint64_t> g_start_up_vector{};

namespace zpp::hypervisor
{
std::optional<std::uint64_t>
hypervisor::wait_for_l2_start_up_ipi(std::size_t)
{
    return g_start_up_vector;
}

void hypervisor::on_unhandled_exit(arch::x86_64::vmx::exit_reason)
{
    __builtin_trap();
}

std::uint64_t hypervisor::own_vmcs_region_physical()
{
    return 0x1000;
}

bool hypervisor::on_ept_violation(std::size_t,
                                  arch::x86_64::context &,
                                  std::uint64_t)
{
    return true;
}

void hypervisor::invalidate_ept_locally()
{
}

} // namespace zpp::hypervisor

// ------------------------------------------------------------------- rig
using hypervisor_t = zpp::hypervisor::hypervisor;

static int g_failures = 0;
static int g_checks = 0;
static std::vector<std::string> g_findings;

static hypervisor_t & hv()
{
    return hypervisor_t::instance();
}

static void check(bool ok, const std::string & what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what.c_str());
        g_findings.push_back(what);
    }
}

/**
 * A place where this VMM's answer is *wrong* and is recorded rather than
 * repaired here.
 *
 * The suite stays green on purpose. A permanently red harness is one
 * nobody runs, and repairing the decision is a change to the hypervisor
 * rather than to its tests. So the *current* answer is asserted, which
 * means a fix flips this check and forces whoever makes it to come back
 * and delete the entry - and the reason and the citation are printed on
 * every run in the meantime.
 */
static void diverge(bool current_answer_holds, const std::string & what)
{
    ++g_checks;
    g_findings.push_back(what);
    if (!current_answer_holds) {
        ++g_failures;
        std::printf("  FAIL a recorded divergence no longer reproduces, "
                    "so the record is stale: %s\n",
                    what.c_str());
    } else {
        std::printf("  DIVERGES %s\n", what.c_str());
    }
}

static std::string text(const char * format, ...)
    __attribute__((format(printf, 1, 2)));

static std::string text(const char * format, ...)
{
    char buffer[512];
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    __builtin_va_end(arguments);
    return buffer;
}

/**
 * The pin, primary and secondary control bits this harness names, from SDM
 * Tables 25-5, 25-6 and 25-7. Spelled again here rather than shared with
 * nested_entry.cpp's anonymous namespace, so a change there that renumbers
 * one is a test failure rather than a silent agreement.
 * @{
 */
static constexpr std::uint64_t pin_external_interrupt = 1ull << 0;
static constexpr std::uint64_t primary_interrupt_window = 1ull << 2;
static constexpr std::uint64_t primary_hlt_exiting = 1ull << 7;
static constexpr std::uint64_t primary_invlpg_exiting = 1ull << 9;
static constexpr std::uint64_t primary_mwait_exiting = 1ull << 10;
static constexpr std::uint64_t primary_rdpmc_exiting = 1ull << 11;
static constexpr std::uint64_t primary_rdtsc_exiting = 1ull << 12;
static constexpr std::uint64_t primary_cr3_load_exiting = 1ull << 15;
static constexpr std::uint64_t primary_cr3_store_exiting = 1ull << 16;
static constexpr std::uint64_t primary_cr8_load_exiting = 1ull << 19;
static constexpr std::uint64_t primary_cr8_store_exiting = 1ull << 20;
static constexpr std::uint64_t primary_tpr_shadow = 1ull << 21;
static constexpr std::uint64_t primary_nmi_window = 1ull << 22;
static constexpr std::uint64_t primary_mov_dr_exiting = 1ull << 23;
static constexpr std::uint64_t primary_unconditional_io = 1ull << 24;
static constexpr std::uint64_t primary_io_bitmaps = 1ull << 25;
static constexpr std::uint64_t primary_monitor_trap_flag = 1ull << 27;
static constexpr std::uint64_t primary_msr_bitmaps = 1ull << 28;
static constexpr std::uint64_t primary_monitor_exiting = 1ull << 29;
static constexpr std::uint64_t primary_pause_exiting = 1ull << 30;
static constexpr std::uint64_t primary_secondary_controls = 1ull << 31;
static constexpr std::uint64_t secondary_descriptor_table = 1ull << 2;
static constexpr std::uint64_t secondary_wbinvd_exiting = 1ull << 6;
static constexpr std::uint64_t secondary_pause_loop_exiting = 1ull << 10;
static constexpr std::uint64_t secondary_rdrand_exiting = 1ull << 11;
static constexpr std::uint64_t secondary_enable_invpcid = 1ull << 12;
static constexpr std::uint64_t secondary_rdseed_exiting = 1ull << 16;
static constexpr std::uint64_t secondary_enable_xsaves = 1ull << 20;
/**
 * @}
 */

// Guest physical addresses the fixture hands to vmcs12 for its bitmaps.
static constexpr std::uint64_t l1_msr_bitmap = 0x40000;
static constexpr std::uint64_t l1_io_bitmap_a = 0x50000;
static constexpr std::uint64_t l1_io_bitmap_b = 0x60000;

static constexpr std::size_t cpu = 0;

/**
 * A neutral exit: nothing this VMM keeps for itself, so `l0_wants_l2_exit`
 * answers no for every reason and `l1_wants_l2_exit` is what decides.
 *
 * The MSR index and the I/O port are ones this VMM's own bitmaps do not
 * name, and both bitmaps are cleared, so the two reasons whose L0 answer
 * is bitmap driven answer no.
 */
static void reset(context & registers)
{
    std::memset(zpp::arch::x86_64::vmx::g_vmcs,
                0,
                sizeof(zpp::arch::x86_64::vmx::g_vmcs));
    hv().guest_vmcs12[cpu].clear();
    std::memset(hv().msr_bitmap, 0, sizeof(hv().msr_bitmap));
    std::memset(hv().io_bitmap_a, 0, sizeof(hv().io_bitmap_a));
    std::memset(hv().io_bitmap_b, 0, sizeof(hv().io_bitmap_b));
    hv().stepping_watch[cpu] = false;
    g_pages.clear();
    g_unreadable.clear();

    registers = context{};
    registers.rcx = 0x1234; // An MSR neither side's bitmap names.

    auto & shadow = hv().guest_vmcs12[cpu];
    shadow.write(fields::msr_bitmap, l1_msr_bitmap);
    shadow.write(fields::io_bitmap_a, l1_io_bitmap_a);
    shadow.write(fields::io_bitmap_b, l1_io_bitmap_b);

    // Touch each so the fake memory has a zeroed page there.
    page_of(l1_msr_bitmap);
    page_of(l1_io_bitmap_a);
    page_of(l1_io_bitmap_b);
}

static void controls(std::uint64_t pin,
                     std::uint64_t primary,
                     std::uint64_t secondary)
{
    auto & shadow = hv().guest_vmcs12[cpu];
    if (0 != secondary) {
        primary |= primary_secondary_controls;
    }
    shadow.write(fields::pin_based_vm_execution_controls, pin);
    shadow.write(fields::primary_processor_based_vm_execution_controls,
                 primary);
    shadow.write(fields::secondary_processor_based_vm_execution_controls,
                 secondary);
}

static bool l1_wants(unsigned reason, const context & registers)
{
    return hv().l1_wants_l2_exit(
        cpu, zpp::arch::x86_64::vmx::exit_reason(reason), registers);
}

static bool l0_wants(unsigned reason, const context & registers)
{
    return hv().l0_wants_l2_exit(
        cpu, zpp::arch::x86_64::vmx::exit_reason(reason), registers);
}

/**
 * What `on_l2_exit` does with an exit, in its own order: an exit this VMM
 * must have is one no reflection may take away, and only then does the
 * guest hypervisor's configuration decide.
 */
enum class decision
{
    kept,      // answered here, the guest hypervisor never sees it
    reflected, // handed to the guest hypervisor
};

static decision decide(unsigned reason, const context & registers)
{
    if (l0_wants(reason, registers)) {
        return decision::kept;
    }
    if (!l1_wants(reason, registers)) {
        return decision::kept;
    }
    return decision::reflected;
}

// ------------------------------------------------- the exit reason table
/**
 * How a reason is turned on and off in vmcs12.
 */
enum class knob
{
    always,    // no control: the exit is always the guest's
    never,     // no control: never the guest's
    pin,       // pin control bit `a`
    primary,   // primary control bit `a`
    secondary, // secondary control bit `a`
    either,    // primary bit `a` or secondary bit `b`
    both,      // primary bit `a` and secondary bit `b`
    exception, // the exception bitmap, vector `a`
    msr,       // "use MSR bitmaps" clear means every access exits
    io_uncond, // unconditional I/O exiting, with no bitmaps
    cr3_load,  // MOV to CR3 against CR3-load exiting
};

struct reason_case
{
    unsigned reason;
    const char * name;
    knob how;
    std::uint64_t a;
    std::uint64_t b;
    // What zpp answers with the knob off and on.
    bool zpp_off;
    bool zpp_on;
    // What KVM's nested_vmx_l1_wants_exit answers for the same inputs.
    bool kvm_off;
    bool kvm_on;
    // What each answers in l0, under the neutral fixture above.
    bool l0_zpp;
    bool l0_kvm;
    // Non-null exactly when a column above differs.
    const char * divergence;
};

/**
 * Every basic exit reason 0 through 69.
 *
 * The KVM columns are `nested_vmx_l1_wants_exit` and
 * `nested_vmx_l0_wants_exit` read case by case; a reason KVM does not name
 * takes its `default: return true` for L1 and `return false` for L0.
 */
static const reason_case g_reasons[] = {
    {0,
     "exception or NMI",
     knob::exception,
     13,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {1,
     "external interrupt",
     knob::pin,
     pin_external_interrupt,
     0,
     false,
     true,
     false,
     true,
     false,
     true,
     "L0: KVM always takes external interrupts because its host has "
     "handlers to run; this VMM never sets external-interrupt exiting, so "
     "the control can only be the guest hypervisor's."},
    {2,
     "triple fault",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {3,
     "INIT signal",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {4,
     "start-up IPI",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {5,
     "I/O SMI",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {6,
     "other SMI",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {7,
     "interrupt window",
     knob::primary,
     primary_interrupt_window,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {8,
     "NMI window",
     knob::primary,
     primary_nmi_window,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {9,
     "task switch",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {10,
     "CPUID",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {11,
     "GETSEC",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {12,
     "HLT",
     knob::primary,
     primary_hlt_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {13,
     "INVD",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {14,
     "INVLPG",
     knob::primary,
     primary_invlpg_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {15,
     "RDPMC",
     knob::primary,
     primary_rdpmc_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {16,
     "RDTSC",
     knob::primary,
     primary_rdtsc_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {17,
     "RSM",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {18,
     "VMCALL",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {19,
     "VMCLEAR",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {20,
     "VMLAUNCH",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {21,
     "VMPTRLD",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {22,
     "VMPTRST",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {23,
     "VMREAD",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {24,
     "VMRESUME",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {25,
     "VMWRITE",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {26,
     "VMXOFF",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {27,
     "VMXON",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {28,
     "control register access",
     knob::cr3_load,
     0,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {29,
     "MOV DR",
     knob::primary,
     primary_mov_dr_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {30,
     "I/O instruction",
     knob::io_uncond,
     0,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {31,
     "RDMSR",
     knob::msr,
     0,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {32,
     "WRMSR",
     knob::msr,
     0,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {33,
     "invalid guest state",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {34,
     "MSR loading failure",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {35,
     "reserved (35)",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {36,
     "MWAIT",
     knob::primary,
     primary_mwait_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {37,
     "monitor trap flag",
     knob::primary,
     primary_monitor_trap_flag,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {38,
     "reserved (38)",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {39,
     "MONITOR",
     knob::primary,
     primary_monitor_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {40,
     "PAUSE",
     knob::either,
     primary_pause_exiting,
     secondary_pause_loop_exiting,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {41,
     "machine check during VM entry",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     true,
     "L0: KVM takes a machine check during VM entry for itself; this VMM "
     "reflects it. Unreachable in practice - nothing here enables "
     "machine-check architecture handling - but the direction differs."},
    {42,
     "reserved (42)",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {43,
     "TPR below threshold",
     knob::primary,
     primary_tpr_shadow,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {44,
     "APIC access",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {45,
     "virtualized EOI",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {46,
     "GDTR or IDTR",
     knob::secondary,
     secondary_descriptor_table,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {47,
     "LDTR or TR",
     knob::secondary,
     secondary_descriptor_table,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {48,
     "EPT violation",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     true,
     true,
     nullptr},
    {49,
     "EPT misconfiguration",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     true,
     true,
     nullptr},
    {50,
     "INVEPT",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {51,
     "RDTSCP",
     knob::primary,
     primary_rdtsc_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {52,
     "VMX preemption timer",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     true,
     true,
     nullptr},
    {53,
     "INVVPID",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {54,
     "WBINVD",
     knob::secondary,
     secondary_wbinvd_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {55,
     "XSETBV",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {56,
     "APIC write",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {57,
     "RDRAND",
     knob::secondary,
     secondary_rdrand_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {58,
     "INVPCID",
     knob::both,
     primary_invlpg_exiting,
     secondary_enable_invpcid,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {59,
     "VMFUNC",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     true,
     "L0: KVM emulates VM functions itself. This VMM does not offer the "
     "'enable VM functions' control, so VMFUNC takes #UD in a "
     "second-level guest and the exit cannot occur."},
    {60,
     "ENCLS",
     knob::always,
     0,
     0,
     true,
     true,
     false,
     false,
     false,
     false,
     "L1: KVM consults vmcs12's ENCLS-exiting bitmap and answers no when "
     "SGX is absent; this VMM reflects. It does not offer 'enable ENCLS "
     "exiting', so the exit cannot occur."},
    {61,
     "RDSEED",
     knob::secondary,
     secondary_rdseed_exiting,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {62,
     "page modification log full",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     true,
     "L0: KVM never enables page-modification logging in vmcs02 and takes "
     "the exit itself. This VMM does not offer the control either, so the "
     "exit cannot occur."},
    {63,
     "XSAVES",
     knob::secondary,
     secondary_enable_xsaves,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {64,
     "XRSTORS",
     knob::secondary,
     secondary_enable_xsaves,
     0,
     false,
     true,
     false,
     true,
     false,
     false,
     nullptr},
    {65,
     "reserved (65)",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {66,
     "SPP related event",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
    {67,
     "UMWAIT",
     knob::always,
     0,
     0,
     true,
     true,
     false,
     false,
     false,
     false,
     "L1: KVM gates on the secondary 'enable user wait and pause' "
     "control; this VMM reflects. It does not offer that control, so a "
     "second-level guest's UMWAIT takes #UD and the exit cannot occur."},
    {68,
     "TPAUSE",
     knob::always,
     0,
     0,
     true,
     true,
     false,
     false,
     false,
     false,
     "L1: KVM gates on the secondary 'enable user wait and pause' "
     "control; this VMM reflects. It does not offer that control, so a "
     "second-level guest's TPAUSE takes #UD and the exit cannot occur."},
    {69,
     "LOADIWKEY",
     knob::always,
     0,
     0,
     true,
     true,
     true,
     true,
     false,
     false,
     nullptr},
};

static void arm(const reason_case & entry, bool on, context & registers)
{
    auto & shadow = hv().guest_vmcs12[cpu];

    switch (entry.how) {
    case knob::always:
    case knob::never:
        break;
    case knob::pin:
        controls(on ? entry.a : 0, 0, 0);
        break;
    case knob::primary:
        controls(0, on ? entry.a : 0, 0);
        break;
    case knob::secondary:
        controls(0, 0, on ? entry.a : 0);
        break;
    case knob::either:
        controls(0, on ? entry.a : 0, 0);
        break;
    case knob::both:
        controls(0, entry.a, on ? entry.b : 0);
        break;
    case knob::exception:
        shadow.write(fields::exception_bitmap, on ? (1ull << entry.a) : 0);
        hv().vmcs.write(field::vm_exit_interruption_information,
                        (1ull << 31) | (3ull << 8) | entry.a);
        break;
    case knob::msr:
        // "Use MSR bitmaps" clear is what makes every MSR access the guest
        // hypervisor's; set with a zeroed bitmap is what makes it none of
        // them. SDM 28.1.3, RDMSR: "the 'use MSR bitmaps' VM-execution
        // control is 0" is the first of the four exit conditions.
        controls(0, on ? 0 : primary_msr_bitmaps, 0);
        break;
    case knob::io_uncond:
        controls(0, on ? primary_unconditional_io : primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification, (0x1234ull << 16) | 0);
        break;
    case knob::cr3_load:
        controls(0, on ? primary_cr3_load_exiting : 0, 0);
        // MOV to CR3 from RAX. SDM Table 28-3.
        hv().vmcs.write(field::exit_qualification, 3ull | (0ull << 4));
        registers.rax = 0x1000;
        break;
    }
}

static void test_reason_table()
{
    std::printf("exit reason table, 0 through 69\n");

    for (auto & entry : g_reasons) {
        for (auto on : {false, true}) {
            context registers{};
            reset(registers);
            arm(entry, on, registers);

            auto want_zpp = on ? entry.zpp_on : entry.zpp_off;
            auto want_kvm = on ? entry.kvm_on : entry.kvm_off;
            auto got = l1_wants(entry.reason, registers);

            check(got == want_zpp,
                  text("reason %u (%s) knob %s: l1_wants = %s, wanted %s",
                       entry.reason,
                       entry.name,
                       on ? "on" : "off",
                       got ? "true" : "false",
                       want_zpp ? "true" : "false"));

            // The differential half: agreeing with KVM unless the entry
            // says why it does not.
            if (nullptr == entry.divergence) {
                check(want_zpp == want_kvm,
                      text("reason %u (%s) knob %s: differs from KVM with "
                           "no divergence recorded",
                           entry.reason,
                           entry.name,
                           on ? "on" : "off"));
            }

            auto got_l0 = l0_wants(entry.reason, registers);
            check(got_l0 == entry.l0_zpp,
                  text("reason %u (%s) knob %s: l0_wants = %s, wanted %s",
                       entry.reason,
                       entry.name,
                       on ? "on" : "off",
                       got_l0 ? "true" : "false",
                       entry.l0_zpp ? "true" : "false"));
        }

        // A recorded divergence that has gone away is a stale comment, and
        // a stale comment about this decision is worse than none.
        if (nullptr != entry.divergence) {
            auto same_l1 = (entry.zpp_off == entry.kvm_off) &&
                           (entry.zpp_on == entry.kvm_on);
            auto same_l0 = entry.l0_zpp == entry.l0_kvm;
            check(!(same_l1 && same_l0),
                  text("reason %u (%s): a divergence is recorded but the "
                       "two columns agree",
                       entry.reason,
                       entry.name));
        }
    }

    // Cross-talk: every *other* control set, and the gate's own clear.
    //
    // This is what catches a gate named with the wrong bit number, which
    // the two-setting test above cannot: a case reading bit 8 instead of
    // bit 7 passes both directions as long as the fixture only ever sets
    // the bit it is about to test.
    for (auto & entry : g_reasons) {
        std::uint64_t pin{}, primary{}, secondary{};

        switch (entry.how) {
        case knob::pin:
            pin = ~entry.a;
            primary = ~std::uint64_t{};
            secondary = ~std::uint64_t{};
            break;
        case knob::primary:
            pin = ~std::uint64_t{};
            primary = ~entry.a;
            secondary = ~std::uint64_t{};
            break;
        case knob::secondary:
            pin = ~std::uint64_t{};
            primary = ~std::uint64_t{};
            secondary = ~entry.a;
            break;
        case knob::either:
            pin = ~std::uint64_t{};
            primary = ~entry.a;
            secondary = ~entry.b;
            break;
        case knob::both:
            // Both are needed, so clearing either one is enough.
            pin = ~std::uint64_t{};
            primary = ~entry.a;
            secondary = ~std::uint64_t{};
            break;
        default:
            continue;
        }

        context registers{};
        reset(registers);
        controls(pin, primary, secondary);
        check(!l1_wants(entry.reason, registers),
              text("reason %u (%s): every control but its own gate set "
                   "must not reflect",
                   entry.reason,
                   entry.name));
    }

    // The either/both knobs deserve their own combinations rather than the
    // one the table drives.
    {
        context registers{};
        reset(registers);
        controls(0, 0, secondary_pause_loop_exiting);
        check(l1_wants(40, registers),
              "PAUSE: PAUSE-loop exiting alone must reflect");

        reset(registers);
        controls(0, primary_pause_exiting, secondary_pause_loop_exiting);
        check(l1_wants(40, registers),
              "PAUSE: both controls must reflect");

        reset(registers);
        controls(0, 0, 0);
        check(!l1_wants(40, registers),
              "PAUSE: neither control must not reflect");
    }

    {
        context registers{};
        reset(registers);
        controls(0, primary_invlpg_exiting, 0);
        check(!l1_wants(58, registers),
              "INVPCID: INVLPG exiting alone must not reflect");

        reset(registers);
        controls(0, 0, secondary_enable_invpcid);
        check(!l1_wants(58, registers),
              "INVPCID: enable INVPCID alone must not reflect");

        reset(registers);
        controls(0, primary_invlpg_exiting, secondary_enable_invpcid);
        check(l1_wants(58, registers),
              "INVPCID: both controls must reflect");
    }

    // Activating the secondary controls is what makes them readable at
    // all: SDM 27.6.2 says VMX non-root operation functions as if they
    // were zero when primary bit 31 is clear, and l1_wants_l2_exit only
    // reads them under that bit.
    {
        context registers{};
        reset(registers);
        auto & shadow = hv().guest_vmcs12[cpu];
        shadow.write(fields::primary_processor_based_vm_execution_controls,
                     0);
        shadow.write(
            fields::secondary_processor_based_vm_execution_controls,
            secondary_wbinvd_exiting);
        check(!l1_wants(54, registers),
              "WBINVD: a secondary control without primary bit 31 must be "
              "ignored");
    }
}

// ------------------------------------------------- the MSR bitmap suite
static void set_bit(std::uint64_t base, std::size_t bit)
{
    auto at = base + (bit / 8);
    page_of(at)[at & 0xfff] |= std::byte(1u << (bit % 8));
}

static void test_msr_bitmap()
{
    std::printf("MSR bitmap consultation\n");

    struct quadrant
    {
        const char * name;
        unsigned reason; // 31 read, 32 write
        std::uint32_t index;
        std::size_t offset; // byte offset of the quadrant in the page
        std::size_t bit;
    };

    // SDM 27.6.9: four 1024-byte bitmaps - low read, high read, low write,
    // high write. KVM's nested_vmx_exit_handled_msr picks the same four.
    static const quadrant quadrants[] = {
        {"low read", 31, 0x10, 0x000, 0x10},
        {"high read", 31, 0xc0000080, 0x400, 0x80},
        {"low write", 32, 0x10, 0x800, 0x10},
        {"high write", 32, 0xc0000080, 0xc00, 0x80},
    };

    for (auto & entry : quadrants) {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = entry.index;

        check(!l1_wants(entry.reason, registers),
              text("MSR %s: a clear bit must not reflect", entry.name));

        set_bit(l1_msr_bitmap + entry.offset, entry.bit);
        check(l1_wants(entry.reason, registers),
              text("MSR %s: a set bit must reflect", entry.name));

        // The opposite quadrant must not answer for this one.
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = entry.index;
        auto other = (31 == entry.reason) ? entry.offset + 0x800
                                          : entry.offset - 0x800;
        set_bit(l1_msr_bitmap + other, entry.bit);
        check(!l1_wants(entry.reason, registers),
              text("MSR %s: the other direction's quadrant must not "
                   "answer",
                   entry.name));
    }

    // The two range boundaries, exactly.
    struct boundary
    {
        std::uint32_t index;
        bool in_range;
        const char * name;
    };

    static const boundary boundaries[] = {
        {0x0000, true, "0x0"},
        {0x1fff, true, "0x1fff"},
        {0x2000, false, "0x2000"},
        {0xbfffffff, false, "0xbfffffff"},
        {0xc0000000, true, "0xc0000000"},
        {0xc0001fff, true, "0xc0001fff"},
        {0xc0002000, false, "0xc0002000"},
        {0xffffffff, false, "0xffffffff"},
    };

    for (auto & entry : boundaries) {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = entry.index;

        // SDM 28.1.3, RDMSR: an address outside both ranges causes a VM
        // exit whatever the bitmap says, so it is the guest hypervisor's.
        check(l1_wants(31, registers) == !entry.in_range,
              text("MSR %s: out of range must reflect unconditionally",
                   entry.name));
    }

    // No bitmap at all means every access exits, which is what the guest
    // hypervisor asked for. SDM 28.1.3, RDMSR, first condition.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        registers.rcx = 0x10;
        check(l1_wants(31, registers),
              "MSR: 'use MSR bitmaps' clear must reflect every access");
        check(l1_wants(32, registers),
              "MSR: 'use MSR bitmaps' clear must reflect every write");
    }

    // A bitmap this VMM cannot read is treated as intercepting, which is
    // KVM's answer too - `kvm_vcpu_read_guest` failing returns true.
    {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = 0x10;
        g_unreadable[l1_msr_bitmap] = true;
        check(l1_wants(31, registers),
              "MSR: an unreadable bitmap must reflect");
    }
}

// -------------------------------------------------- the CR access suite
static std::uint64_t cr_qualification(std::uint64_t number,
                                      std::uint64_t access,
                                      std::uint64_t gpr = 0,
                                      std::uint64_t lmsw_source = 0)
{
    // SDM Table 28-3: bits 3:0 the register, bits 5:4 the access type,
    // bits 11:8 the general purpose register, bits 31:16 the LMSW source.
    return (number & 0xf) | ((access & 0x3) << 4) | ((gpr & 0xf) << 8) |
           ((lmsw_source & 0xffff) << 16);
}

static constexpr std::uint64_t access_move_to = 0;
static constexpr std::uint64_t access_move_from = 1;
static constexpr std::uint64_t access_clts = 2;
static constexpr std::uint64_t access_lmsw = 3;

static void test_cr_access()
{
    std::printf("control register access\n");

    auto & shadow = hv().guest_vmcs12[cpu];

    struct write_case
    {
        std::uint64_t number;
        std::uint64_t mask;
        std::uint64_t read_shadow;
        std::uint64_t written;
        bool want;
        const char * why;
    };

    // SDM 28.1.3: "MOV to CR0 ... causes a VM exit unless the value of its
    // source operand matches, for the position of each bit set in the CR0
    // guest/host mask, the corresponding bit in the CR0 read shadow", and
    // the same sentence for CR4. KVM: nested_vmx_exit_handled_cr cases 0
    // and 4.
    static const write_case writes[] = {
        {0, 0, 0, ~0ull, false, "an empty mask can never exit"},
        {4, 0, 0, ~0ull, false, "an empty mask can never exit (CR4)"},
        {0,
         1ull << 5,
         0,
         1ull << 5,
         true,
         "an owned bit set that the "
         "shadow shows clear"},
        {0,
         1ull << 5,
         1ull << 5,
         1ull << 5,
         false,
         "an owned bit that matches the shadow"},
        {0,
         1ull << 5,
         1ull << 5,
         0,
         true,
         "an owned bit cleared that the shadow shows set"},
        {0, 1ull << 5, 0, 1ull << 6, false, "a change outside the mask"},
        {4,
         1ull << 13,
         1ull << 13,
         1ull << 13,
         false,
         "CR4.VMXE owned and matching the shadow must not exit"},
        {4,
         1ull << 13,
         1ull << 13,
         0,
         true,
         "CR4.VMXE cleared against a shadow that shows it set"},
        {4,
         (1ull << 13) | (1ull << 5),
         1ull << 13,
         (1ull << 13) | (1ull << 5),
         true,
         "a second owned bit changing while VMXE matches"},
    };

    for (auto & entry : writes) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write((0 == entry.number) ? fields::cr0_guest_host_mask
                                         : fields::cr4_guest_host_mask,
                     entry.mask);
        shadow.write((0 == entry.number) ? fields::cr0_read_shadow
                                         : fields::cr4_read_shadow,
                     entry.read_shadow);
        // Register 3 is RBX, chosen so a harness that forgot to decode
        // bits 11:8 reads zero and fails.
        registers.rbx = entry.written;
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(entry.number, access_move_to, 3));

        check(l1_wants(28, registers) == entry.want,
              text("MOV to CR%llu: %s",
                   (unsigned long long)entry.number,
                   entry.why));
    }

    // The read shadow is the operand, not vmcs12's guest CR0/CR4. The two
    // are meant to differ - that is what owning a bit is for - and this is
    // the case the fix in this session was about.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, 1ull << 5);
        shadow.write(fields::cr0_read_shadow, 1ull << 5);
        shadow.write(fields::guest_cr0, 0); // Deliberately disagreeing.
        registers.rbx = 1ull << 5;
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_move_to, 3));
        check(!l1_wants(28, registers),
              "MOV to CR0: the read shadow decides, not vmcs12's guest "
              "CR0");
    }

    // The general purpose register really is read out of bits 11:8, and
    // RSP is the one that is not in the captured context.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, 1ull << 5);
        shadow.write(fields::cr0_read_shadow, 0);
        hv().vmcs.guest_rsp(1ull << 5);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_move_to, 4));
        check(l1_wants(28, registers),
              "MOV to CR0 from RSP: the value comes from the VMCS");
    }

    // Every general purpose register encoding, so that bits 11:8 really
    // are what names the source operand.
    for (std::uint64_t gpr = 0; gpr < 16; ++gpr) {
        context registers{};
        reset(registers);
        registers.rcx = 0; // The fixture's MSR index is not wanted here.
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, 1ull << 5);
        shadow.write(fields::cr0_read_shadow, 0);

        // Only the named register carries the owned bit.
        if (4 == gpr) {
            hv().vmcs.guest_rsp(1ull << 5);
        } else {
            registers.*arch::x86_64::detail::encoded_registers[gpr] = 1ull
                                                                      << 5;
        }

        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_move_to, gpr));
        check(l1_wants(28, registers),
              text("MOV to CR0 from register %llu must reflect",
                   (unsigned long long)gpr));

        // And a neighbour holding the same value must not answer for it.
        context others{};
        reset(others);
        others.rcx = 0;
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, 1ull << 5);
        shadow.write(fields::cr0_read_shadow, 0);
        for (std::uint64_t other = 0; other < 16; ++other) {
            if ((other == gpr) || (4 == other)) {
                continue;
            }
            others.*arch::x86_64::detail::encoded_registers[other] = 1ull
                                                                     << 5;
        }
        if (4 != gpr) {
            hv().vmcs.guest_rsp(1ull << 5);
        }
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_move_to, gpr));
        check(!l1_wants(28, others),
              text("MOV to CR0 from register %llu must read only that "
                   "register",
                   (unsigned long long)gpr));
    }

    // The two cases the architecture cannot produce, recorded rather than
    // asserted blind. SDM 28.1.3 lists no exit for MOV from CR0 or MOV
    // from CR4 - the read shadow answers those without leaving the guest -
    // and a control-register access exit only ever names CR0, CR3, CR4 or
    // CR8. zpp's `default: return true` and KVM's fall-through `return
    // false` therefore differ on inputs a processor never delivers.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_move_from, 0));
        check(l1_wants(28, registers),
              "MOV from CR0: zpp reflects where KVM would not - the "
              "architecture delivers no such exit, so neither answer is "
              "reachable");

        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(4, access_move_from, 0));
        check(l1_wants(28, registers),
              "MOV from CR4: same, and same reason");

        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(2, access_move_to, 0));
        check(l1_wants(28, registers),
              "MOV to CR2: same - CR2 is not one of the four a "
              "control-register access exit can name");
    }

    // CR3 and CR8, both directions, both settings.
    struct gated_case
    {
        std::uint64_t number;
        std::uint64_t access;
        std::uint64_t control;
        const char * name;
    };

    static const gated_case gated[] = {
        {3, access_move_to, primary_cr3_load_exiting, "MOV to CR3"},
        {3, access_move_from, primary_cr3_store_exiting, "MOV from CR3"},
        {8, access_move_to, primary_cr8_load_exiting, "MOV to CR8"},
        {8, access_move_from, primary_cr8_store_exiting, "MOV from CR8"},
    };

    for (auto & entry : gated) {
        for (auto on : {false, true}) {
            context registers{};
            reset(registers);
            controls(0, on ? entry.control : 0, 0);
            hv().vmcs.write(
                field::exit_qualification,
                cr_qualification(entry.number, entry.access, 0));
            check(l1_wants(28, registers) == on,
                  text("%s with its control %s",
                       entry.name,
                       on ? "set" : "clear"));
        }
    }

    // CLTS. SDM 28.1.3: "The CLTS instruction causes a VM exit if the bits
    // in position 3 (corresponding to CR0.TS) are set in both the CR0
    // guest/host mask and the CR0 read shadow." KVM: nested.c case 2.
    struct clts_case
    {
        std::uint64_t mask;
        std::uint64_t read_shadow;
        bool want;
        const char * why;
    };

    static const clts_case clts[] = {
        {0, 0, false, "neither"},
        {1ull << 3,
         0,
         false,
         "mask alone - SDM 28.3 says CLTS completes without changing "
         "CR0.TS, so there is no exit"},
        {0, 1ull << 3, false, "read shadow alone"},
        {1ull << 3, 1ull << 3, true, "both"},
        {~0ull,
         ~(1ull << 3),
         false,
         "every bit owned but TS clear in the shadow"},
    };

    for (auto & entry : clts) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, entry.mask);
        shadow.write(fields::cr0_read_shadow, entry.read_shadow);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_clts));
        check(l1_wants(28, registers) == entry.want,
              text("CLTS: %s", entry.why));
    }

    // LMSW. SDM 28.1.3 splits it in two because "LMSW never clears bit 0
    // of CR0 (CR0.PE)". KVM builds the same two-part test.
    struct lmsw_case
    {
        std::uint64_t mask;
        std::uint64_t read_shadow;
        std::uint64_t source;
        bool want;
        const char * why;
    };

    static const lmsw_case lmsws[] = {
        {0, 0, 0xf, false, "an empty mask"},
        {0x1,
         0,
         0x1,
         true,
         "PE owned, set in the source, clear in the "
         "shadow"},
        {0x1, 0x1, 0x1, false, "PE owned and already set in the shadow"},
        {0x1,
         0x1,
         0x0,
         false,
         "PE owned, clear in the source - LMSW cannot clear it, so it is "
         "not a change"},
        {0x1, 0x0, 0x0, false, "PE owned and nothing written"},
        {0x2, 0x0, 0x2, true, "MP owned and differing"},
        {0x2, 0x2, 0x2, false, "MP owned and agreeing"},
        {0x2, 0x2, 0x0, true, "MP owned, cleared by the source"},
        {0x8, 0x0, 0x8, true, "TS owned and differing"},
        {0xe,
         0x0,
         0x1,
         false,
         "only PE in the source, and PE is not owned"},
        {0xf, 0x0, 0x1, true, "PE owned and set with the shadow clear"},
        {0xe, 0xe, 0xe, false, "bits 3:1 owned and all agreeing"},
        {0x10,
         0x0,
         0x10,
         false,
         "bit 4 is outside the low four bits LMSW writes"},
    };

    for (auto & entry : lmsws) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, entry.mask);
        shadow.write(fields::cr0_read_shadow, entry.read_shadow);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_lmsw, 0, entry.source));
        check(l1_wants(28, registers) == entry.want,
              text("LMSW: %s", entry.why));
    }

    // The source data lives in bits 31:16, and the bits above the low four
    // must not reach the comparison. KVM masks the source with 0x0f.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::cr0_guest_host_mask, 0xe);
        shadow.write(fields::cr0_read_shadow, 0);
        hv().vmcs.write(field::exit_qualification,
                        cr_qualification(0, access_lmsw, 0, 0xfff0));
        check(!l1_wants(28, registers),
              "LMSW: source bits above 3:0 must not cause an exit");
    }
}

// ------------------------------------------------- the exception suite
static void test_exceptions()
{
    std::printf("exception bitmap and page-fault filtering\n");

    auto & shadow = hv().guest_vmcs12[cpu];

    // Every vector, both settings of its bit. SDM 28.2: "its vector (in
    // the range 0-31) is used to select a bit in the exception bitmap. If
    // the bit is 1, a VM exit occurs."
    for (unsigned vector = 0; vector < 32; ++vector) {
        if (14 == vector) {
            continue; // Page faults are the special case below.
        }

        for (auto on : {false, true}) {
            context registers{};
            reset(registers);
            controls(0, 0, 0);
            shadow.write(fields::exception_bitmap,
                         on ? (1ull << vector) : 0);
            hv().vmcs.write(field::vm_exit_interruption_information,
                            (1ull << 31) | (3ull << 8) | vector);
            check(l1_wants(0, registers) == on,
                  text("exception vector %u with its bit %s",
                       vector,
                       on ? "set" : "clear"));
        }
    }

    // A vector whose bit is set must not answer for a different vector.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::exception_bitmap, 1ull << 13);
        hv().vmcs.write(field::vm_exit_interruption_information,
                        (1ull << 31) | (3ull << 8) | 6);
        check(!l1_wants(0, registers),
              "exception: the bitmap is indexed by the exit's own vector");
    }

    // Page faults. SDM 28.2: the processor checks PFEC & PFEC_MASK ==
    // PFEC_MATCH; "if there is equality, the specification of bit 14 in
    // the exception bitmap is followed ... if there is inequality, the
    // meaning of that bit is reversed."
    struct page_fault_case
    {
        bool in_bitmap;
        std::uint64_t mask;
        std::uint64_t match;
        std::uint64_t error_code;
        bool want;
        const char * why;
    };

    static const page_fault_case faults[] = {
        {true,
         0,
         0,
         0,
         true,
         "exit on all page faults (SDM 28.2's own "
         "example: bit set, mask 0, match 0)"},
        {true, 0, 0, 0x7, true, "same, with a non-zero error code"},
        {true,
         0,
         0xffffffff,
         0,
         false,
         "exit on no page faults (SDM 28.2's second example)"},
        {false, 0, 0, 0, false, "bit clear, equality: no exit"},
        {false,
         0,
         0xffffffff,
         0,
         true,
         "bit clear, inequality: the meaning of the bit is reversed"},
        {true, 0x1, 0x1, 0x1, true, "present-bit match, bit set"},
        {true, 0x1, 0x1, 0x0, false, "present-bit mismatch, bit set"},
        {false, 0x1, 0x1, 0x1, false, "present-bit match, bit clear"},
        {false, 0x1, 0x1, 0x0, true, "present-bit mismatch, bit clear"},
        {true, 0xffffffff, 0x2, 0x2, true, "a full mask matching exactly"},
        {true, 0xffffffff, 0x2, 0x3, false, "a full mask not matching"},
    };

    for (auto & entry : faults) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        shadow.write(fields::exception_bitmap,
                     entry.in_bitmap ? (1ull << 14) : 0);
        shadow.write(fields::page_fault_error_code_mask, entry.mask);
        shadow.write(fields::page_fault_error_code_match, entry.match);
        hv().vmcs.write(field::vm_exit_interruption_information,
                        (1ull << 31) | (3ull << 8) | 14);
        hv().vmcs.write(field::vm_exit_interruption_error_code,
                        entry.error_code);
        check(l1_wants(0, registers) == entry.want,
              text("page fault: %s", entry.why));
    }

    // An NMI is this VMM's whatever the guest hypervisor asked, because it
    // sets NMI exiting in its own pin controls. KVM's
    // nested_vmx_l0_wants_exit answers the same. Interruption type 2 is
    // NMI, SDM Table 25-19.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::vm_exit_interruption_information,
                        (1ull << 31) | (2ull << 8) | 2);
        check(l0_wants(0, registers), "NMI: l0 must keep it");
        check(decision::kept == decide(0, registers),
              "NMI: the composed decision must keep it");

        // Every interruption type, so that only type 2 is an NMI. SDM
        // Table 25-19 gives bits 10:8 as the type.
        for (std::uint64_t type = 0; type < 8; ++type) {
            reset(registers);
            controls(0, 0, 0);
            hv().vmcs.write(field::vm_exit_interruption_information,
                            (1ull << 31) | (type << 8) | 2);
            check(l0_wants(0, registers) == (2 == type),
                  text("interruption type %llu is %san NMI",
                       (unsigned long long)type,
                       (2 == type) ? "" : "not "));
        }
    }

    // What zpp does *not* claim for itself here, and KVM does. All four
    // are conditional on state this VMM has no equivalent of - a host
    // asynchronous page fault, a hardware breakpoint the debugger owns, an
    // alignment check the host injected, or a virtualization exception -
    // so the divergence is structural rather than a defect. Recorded so
    // that adding any of those features comes back through here.
    for (auto vector : {1u, 3u, 14u, 17u, 20u}) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::vm_exit_interruption_information,
                        (1ull << 31) | (3ull << 8) | vector);
        check(!l0_wants(0, registers),
              text("vector %u is not this VMM's: it has no debugger, no "
                   "host page faults and no #VE",
                   vector));
    }
}

// -------------------------------------------------------- the I/O suite
static void test_io()
{
    std::printf("I/O instructions\n");

    auto io_qualification = [](std::uint64_t port, std::uint64_t size) {
        // SDM Table 28-5: bits 2:0 the size minus one, bits 31:16 the
        // port.
        return ((size - 1) & 0x7) | (port << 16);
    };

    // Neither control: the guest hypervisor asked for nothing.
    {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x60, 1));
        check(!l1_wants(30, registers),
              "I/O: neither control set must not reflect");
    }

    // Unconditional I/O exiting alone.
    {
        context registers{};
        reset(registers);
        controls(0, primary_unconditional_io, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x60, 1));
        check(l1_wants(30, registers),
              "I/O: unconditional I/O exiting must reflect");
    }

    // The bitmaps, in both halves of the port space.
    struct bitmap_case
    {
        std::uint64_t port;
        std::uint64_t page;
        const char * name;
    };

    static const bitmap_case bitmaps[] = {
        {0x0000, l1_io_bitmap_a, "port 0x0000, bitmap A"},
        {0x0060, l1_io_bitmap_a, "port 0x0060, bitmap A"},
        {0x7fff, l1_io_bitmap_a, "port 0x7fff, the last of bitmap A"},
        {0x8000, l1_io_bitmap_b, "port 0x8000, the first of bitmap B"},
        {0xcf8, l1_io_bitmap_a, "port 0x0cf8, bitmap A"},
        {0xffff, l1_io_bitmap_b, "port 0xffff, the last of bitmap B"},
    };

    for (auto & entry : bitmaps) {
        context registers{};
        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(entry.port, 1));
        check(!l1_wants(30, registers),
              text("I/O %s: a clear bit must not reflect", entry.name));

        set_bit(entry.page, entry.port & 0x7fff);
        check(l1_wants(30, registers),
              text("I/O %s: a set bit must reflect", entry.name));
    }

    // A wide access whose first port is clear but whose later ports are
    // not. KVM's nested_vmx_check_io_bitmaps walks the same range.
    {
        for (unsigned offset = 0; offset < 4; ++offset) {
            context registers{};
            reset(registers);
            controls(0, primary_io_bitmaps, 0);
            hv().vmcs.write(field::exit_qualification,
                            io_qualification(0x60, 4));
            set_bit(l1_io_bitmap_a, 0x60 + offset);
            check(l1_wants(30, registers),
                  text("I/O: a 4-byte access at 0x60 must reflect when "
                       "only port 0x%x is intercepted",
                       0x60 + offset));
        }

        context registers{};
        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x60, 4));
        set_bit(l1_io_bitmap_a, 0x64);
        check(!l1_wants(30, registers),
              "I/O: a 4-byte access at 0x60 must not reflect for port "
              "0x64");
    }

    // A range straddling a bitmap byte boundary: ports 0x3f8 through
    // 0x3fb sit in byte 0x7f, ports 0x3fe and 0x3ff in byte 0x7f too, so
    // the boundary worth testing is 0x3f7 through 0x3fa - bytes 0x7e and
    // 0x7f.
    {
        for (unsigned offset = 0; offset < 4; ++offset) {
            context registers{};
            reset(registers);
            controls(0, primary_io_bitmaps, 0);
            hv().vmcs.write(field::exit_qualification,
                            io_qualification(0x3f7, 4));
            set_bit(l1_io_bitmap_a, 0x3f7 + offset);
            check(l1_wants(30, registers),
                  text("I/O: an access straddling a byte boundary must "
                       "reflect for port 0x%x",
                       0x3f7 + offset));
        }
    }

    // A range straddling the A/B bitmap boundary at 0x8000.
    {
        for (unsigned offset = 0; offset < 4; ++offset) {
            context registers{};
            reset(registers);
            controls(0, primary_io_bitmaps, 0);
            hv().vmcs.write(field::exit_qualification,
                            io_qualification(0x7ffe, 4));
            auto port = 0x7ffe + offset;
            set_bit(port < 0x8000 ? l1_io_bitmap_a : l1_io_bitmap_b,
                    port & 0x7fff);
            check(l1_wants(30, registers),
                  text("I/O: an access straddling bitmap A and B must "
                       "reflect for port 0x%x",
                       port));
        }
    }

    // A bitmap this VMM cannot read is treated as intercepting.
    {
        context registers{};
        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x60, 1));
        g_unreadable[l1_io_bitmap_a] = true;
        check(l1_wants(30, registers),
              "I/O: an unreadable bitmap must reflect");

        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x8000, 1));
        g_unreadable[l1_io_bitmap_b] = true;
        check(l1_wants(30, registers),
              "I/O: an unreadable bitmap B must reflect");
    }

    // ---- the two findings ----

    // FINDING 1. Both controls set. SDM Table 25-6, bit 25: "If the I/O
    // bitmaps are used, the setting of the 'unconditional I/O exiting'
    // control is ignored." SDM 28.1.3 repeats it. KVM's
    // nested_vmx_exit_handled_io tests the bitmaps first and only falls
    // back on the unconditional control when they are off; this VMM tests
    // the unconditional control first and reflects everything.
    //
    // Both controls are offered to a guest hypervisor - nested_vmx.h lines
    // for bits 24 and 25 - so this is reachable.
    {
        context registers{};
        reset(registers);
        controls(0, primary_unconditional_io | primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0x60, 1));
        check(!l1_wants(30, registers),
              "with both 'unconditional I/O exiting' and 'use I/O "
              "bitmaps' set, the bitmaps decide - SDM 28.1.3 "
              "(sdm.txt:200725), \"the 'unconditional I/O exiting' "
              "VM-execution control is ignored if the 'use I/O bitmaps' "
              "VM-execution control is 1\", matching "
              "nested_vmx_exit_handled_io");
    }

    // FINDING 2. An access that wraps the 16-bit port space. SDM 28.1.3:
    // "If an I/O operation 'wraps around' the 16-bit I/O-port space
    // (accesses ports FFFFH and 0000H), the I/O instruction causes a VM
    // exit." KVM returns true as soon as the port reaches 0x10000; this
    // VMM breaks out of the loop and answers with whatever the ports below
    // said.
    {
        context registers{};
        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification,
                        io_qualification(0xffff, 4));
        check(l1_wants(30, registers),
              "an I/O access that wraps the 16-bit port space exits - "
              "SDM 28.1.3 (sdm.txt:200724), \"If an I/O operation "
              "'wraps around' the 16-bit I/O-port space (accesses ports "
              "FFFFH and 0000H), the I/O instruction causes a VM "
              "exit\"");
    }
}

// ------------------------------------------ the L0-before-L1 precedence
static void test_l0_precedence()
{
    std::printf("what this VMM keeps before the guest hypervisor is "
                "asked\n");

    auto own_msr = [](std::uint32_t index, bool write) {
        std::size_t base = write ? 0x800 : 0x000;
        std::uint32_t bit = index;
        if (index >= 0xc0000000) {
            base = write ? 0xc00 : 0x400;
            bit = index - 0xc0000000;
        }
        hv().msr_bitmap[base + (bit / 8)] |= 1u << (bit % 8);
    };

    // The three cases an MSR access can be in.
    {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = 0x1b; // IA32_APIC_BASE.
        own_msr(0x1b, false);
        check(!l0_wants(31, registers),
              "MSR: this VMM's own bitmap no longer makes the exit its "
              "own - that decision belongs to the guest hypervisor");
        check(decision::kept == decide(31, registers),
              "MSR: an access only this VMM asked for still reaches the "
              "ordinary handler, by way of neither side wanting it");
    }

    {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = 0x1b;
        set_bit(l1_msr_bitmap + 0x000, 0x1b);
        check(!l0_wants(31, registers),
              "MSR: an access only the guest hypervisor asked for is not "
              "this VMM's");
        check(decision::reflected == decide(31, registers),
              "MSR: an access only the guest hypervisor asked for is "
              "reflected");
    }

    // Both bitmaps naming the same MSR, which was a defect and is the
    // regression test for its fix.
    //
    // KVM's nested_vmx_l0_wants_exit does not name EXIT_REASON_MSR_READ or
    // EXIT_REASON_MSR_WRITE at all, so nested_vmx_l1_wants_exit decides
    // and the exit is reflected; L0's own interest is served when L1 then
    // performs the access itself and exits from L1. This VMM used to ask
    // its own bitmap first and keep the exit, so the guest hypervisor
    // never learned its guest had touched the register.
    //
    // It was reachable: hypervisor.cpp intercepts IA32_APIC_BASE
    // unconditionally, and IA32_FEATURE_CONTROL plus the whole VMX
    // capability range when nested VMX is on - which is the exact set a
    // guest hypervisor presenting VMX to its own guest also intercepts.
    {
        context registers{};
        reset(registers);
        controls(0, primary_msr_bitmaps, 0);
        registers.rcx = 0x1b;
        own_msr(0x1b, false);
        set_bit(l1_msr_bitmap + 0x000, 0x1b);

        check(decision::reflected == decide(31, registers),
              "an MSR both this VMM and the guest hypervisor intercept "
              "is reflected, not kept - KVM's nested_vmx_l0_wants_exit "
              "names neither EXIT_REASON_MSR_READ nor "
              "EXIT_REASON_MSR_WRITE, so the guest hypervisor decides "
              "and this VMM's own interest is served when it performs "
              "the access itself");
    }

    // The same shape for I/O ports.
    {
        context registers{};
        reset(registers);
        controls(0, primary_io_bitmaps, 0);
        hv().vmcs.write(field::exit_qualification, (0x604ull << 16) | 1);
        hv().io_bitmap_a[0x604 / 8] |= 1u << (0x604 % 8);
        set_bit(l1_io_bitmap_a, 0x604);

        check(decision::reflected == decide(30, registers),
              "an I/O port both this VMM and the guest hypervisor "
              "intercept is reflected, not kept - the same shape as the "
              "MSR case above and the same reason");
    }

    // The monitor trap flag: this VMM's only while it is stepping.
    {
        context registers{};
        reset(registers);
        controls(0, primary_monitor_trap_flag, 0);
        check(!l0_wants(37, registers),
              "MTF: not this VMM's when it is not stepping");
        check(decision::reflected == decide(37, registers),
              "MTF: the guest hypervisor's control decides");

        hv().stepping_watch[cpu] = true;
        check(l0_wants(37, registers),
              "MTF: this VMM's while it is stepping a watched write");
        check(decision::kept == decide(37, registers),
              "MTF: a step in progress outranks the guest hypervisor's "
              "control");
        hv().stepping_watch[cpu] = false;
    }

    // The two exits that are always this VMM's whatever vmcs12 says.
    for (auto reason : {48u, 49u, 52u}) {
        context registers{};
        reset(registers);
        controls(~0ull, ~0ull, ~0ull);
        check(l0_wants(reason, registers),
              text("reason %u must always be this VMM's", reason));
    }

    // Nothing else is, whatever vmcs12 says.
    for (unsigned reason = 0; reason <= 69; ++reason) {
        if ((0 == reason) || (30 == reason) || (31 == reason) ||
            (32 == reason) || (37 == reason) || (48 == reason) ||
            (49 == reason) || (52 == reason)) {
            continue;
        }

        context registers{};
        reset(registers);
        controls(~0ull, ~0ull, ~0ull);
        check(!l0_wants(reason, registers),
              text("reason %u must not be claimed by l0", reason));
    }
}

// ---------------------------------------------------------------- main
// -------------------------------------------- 7. the activity state
/**
 * What a VM entry may establish in the second-level guest's activity
 * state, which is `enter_or_park_l2`'s decision.
 *
 * The expected column is KVM's. `nested_check_guest_non_reg_state`
 * (.references/kvm/nested.c:3117-3119) accepts active, HLT and
 * wait-for-SIPI and nothing else, and a rejection there becomes
 * EXIT_REASON_INVALID_STATE with ENTRY_FAIL_DEFAULT
 * (nested.c:3568-3572), which is SDM 29.8's reason 33 with bit 31 set and
 * a zero qualification.
 *
 * The one place this VMM answers differently from KVM is HLT, and it is
 * asserted rather than left implied - see the divergence note below.
 */
static void test_activity_state()
{
    std::printf("the activity state a second-level VM entry may "
                "establish\n");

    using entry_outcome = zpp::hypervisor::hypervisor::l2_entry_outcome;
    namespace activity = zpp::arch::x86_64::vmx::activity_state;

    constexpr std::uint64_t entry_failure_bit = 1ull << 31;
    constexpr std::uint64_t invalid_guest_state = 33;
    constexpr std::uint64_t sentinel_rip = 0xfeedfacecafe0000ull;

    auto & shadow = hv().guest_vmcs12[cpu];

    auto arm = [&](std::uint64_t state) {
        context registers{};
        reset(registers);
        controls(0, 0, 0);
        hv().running_l2[cpu] = false;
        hv().l2_activity_state[cpu] = activity::active;
        g_start_up_vector.reset();
        shadow.write(fields::guest_activity_state, state);
        shadow.write(fields::guest_rip, sentinel_rip);
        shadow.write(fields::exit_reason, 0);
        shadow.write(fields::exit_qualification, 0);
    };

    auto refused = [&](std::uint64_t state, const char * what) {
        arm(state);
        check(entry_outcome::reflected == hv().enter_or_park_l2(cpu),
              text("activity state %s must not be entered", what));
        check((entry_failure_bit | invalid_guest_state) ==
                  shadow.read(fields::exit_reason),
              text("activity state %s must fail the entry with reason 33 "
                   "and bit 31",
                   what));
        check(0 == shadow.read(fields::exit_qualification),
              text("activity state %s must fail with a zero "
                   "qualification",
                   what));

        // SDM 29.8: "The guest-state area is not modified." A refusal
        // that saved vmcs02 over it would move the guest hypervisor's own
        // guest, which is what save_l2_state does when the second-level
        // guest never ran.
        check(sentinel_rip == shadow.read(fields::guest_rip),
              text("activity state %s modified vmcs12's guest state on a "
                   "VM-entry failure",
                   what));
    };

    // Active, which every implementation supports (SDM A.6).
    arm(activity::active);
    check(entry_outcome::entered == hv().enter_or_park_l2(cpu),
          "the active state must be entered");
    check(activity::active == hv().vmcs.read(field::guest_activity_state),
          "the active state must reach vmcs02");

    // HLT, entered in hardware. KVM does not: `kvm_emulate_halt_noskip`
    // blocks the vCPU thread instead (nested.c:3766-3779). This VMM has no
    // scheduler and root operation cannot observe or deliver the physical
    // interrupt that ends a halt, and SDM 29.7.2's own list says nothing
    // is lost by letting the processor sit there - the active state and
    // the HLT state block exactly the same events, start-up IPIs.
    arm(activity::hlt);
    check(entry_outcome::entered == hv().enter_or_park_l2(cpu),
          "the HLT state must be entered");
    check(activity::hlt == hv().vmcs.read(field::guest_activity_state),
          "the HLT state must reach vmcs02");
    diverge(true,
            "guest activity state 1 (HLT): KVM emulates the halt and "
            "keeps hardware active; this VMM enters hardware in the HLT "
            "state, which SDM 29.7.2 blocks exactly what the active "
            "state does. It has no scheduler to block a virtual "
            "processor on, and forcing active would make a halted guest "
            "execute instructions.");

    // Shutdown, which this VMM does not report in IA32_VMX_MISC and KVM
    // does not accept either.
    refused(activity::shutdown, "2 (shutdown)");
    refused(4, "4 (out of range)");

    // Wait-for-SIPI is held rather than entered, because SDM 29.7.2 has
    // it block external interrupts, NMIs, INIT and SMIs.
    arm(activity::wait_for_start_up_ipi);
    check(entry_outcome::retry == hv().enter_or_park_l2(cpu),
          "wait-for-SIPI with no IPI to give must not be entered");
    check(activity::wait_for_start_up_ipi == hv().l2_activity_state[cpu],
          "wait-for-SIPI must be recorded where start_up_processor reads "
          "it");
    check(0 != hv().l2_start_up_waits[cpu],
          "a held second-level guest must be counted, or a parked "
          "processor cannot be told from a frozen one");
    check(activity::wait_for_start_up_ipi !=
              hv().vmcs.read(field::guest_activity_state),
          "wait-for-SIPI reached vmcs02, which nothing can then end");

    // And when one arrives, the guest hypervisor gets the exit hardware
    // would have given it. KVM synthesises the identical one from
    // mp_state, in `vmx_check_nested_events` (nested.c:4240-4243).
    arm(activity::wait_for_start_up_ipi);
    g_start_up_vector = 0x8a;
    check(entry_outcome::reflected == hv().enter_or_park_l2(cpu),
          "a start-up IPI for a parked second-level guest must reflect");
    check(static_cast<std::uint64_t>(basic_reason::start_up_ipi) ==
              shadow.read(fields::exit_reason),
          "a start-up IPI must reflect as exit reason 4");
    check(0x8a == shadow.read(fields::exit_qualification),
          "a start-up IPI must carry its vector in the qualification");
    check(activity::wait_for_start_up_ipi ==
              shadow.read(fields::guest_activity_state),
          "a start-up IPI exit must leave vmcs12 in wait-for-SIPI");

    // And the software record goes back to describing this processor, so
    // that start_up_processor stops offering it vectors through a mailbox
    // nothing is spinning on.
    check(activity::active == hv().l2_activity_state[cpu],
          "a reflected start-up IPI left the processor recorded as still "
          "parked");
    g_start_up_vector.reset();

    // The two checks the processor no longer makes, because the state it
    // would have made them about is not the one vmcs02 is entered with.
    // SDM 29.3.1.5, "Checks on Guest Non-Register State".
    arm(activity::wait_for_start_up_ipi);
    // Blocking by STI.
    shadow.write(fields::guest_interruptibility_state, 1);
    check(entry_outcome::reflected == hv().enter_or_park_l2(cpu),
          "wait-for-SIPI with blocking by STI must fail the entry");
    check((entry_failure_bit | invalid_guest_state) ==
              shadow.read(fields::exit_reason),
          "wait-for-SIPI with blocking by STI must give reason 33");

    arm(activity::wait_for_start_up_ipi);
    shadow.write(fields::vm_entry_interruption_information_field,
                 1ull << 31);
    check(entry_outcome::reflected == hv().enter_or_park_l2(cpu),
          "wait-for-SIPI with an event to inject must fail the entry");
    check((entry_failure_bit | invalid_guest_state) ==
              shadow.read(fields::exit_reason),
          "wait-for-SIPI with an event to inject must give reason 33");
}

int main()
{
    test_reason_table();
    test_msr_bitmap();
    test_cr_access();
    test_exceptions();
    test_io();
    test_l0_precedence();
    test_activity_state();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);

    // The differences the table records, which are all of the form "KVM
    // handles a feature this VMM does not offer". Each is asserted above,
    // so one that stops reproducing fails the run; printing them is what
    // makes the run say what it settled rather than only what it broke.
    std::printf("\ndifferences from KVM that are not defects:\n");
    for (auto & entry : g_reasons) {
        if (nullptr != entry.divergence) {
            std::printf("  - reason %u (%s): %s\n",
                        entry.reason,
                        entry.name,
                        entry.divergence);
        }
    }

    if (!g_findings.empty()) {
        std::printf("\nfindings:\n");
        for (auto & finding : g_findings) {
            std::printf("  - %s\n", finding.c_str());
        }
    }

    return (0 == g_failures) ? 0 : 1;
}
