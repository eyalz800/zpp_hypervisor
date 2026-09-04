// Differential test harness for the watched-page write path.
//
// Compiles the real on_ept_violation, carry_out_guest_instruction,
// apply_guest_store, read_guest_word, on_monitor_trap_flag,
// watch_guest_page_writes, filter_local_apic_write and
// on_local_apic_write - cut out of hypervisor.cpp by build.sh - natively
// against a shim hypervisor, a shim VMCS and a real page of fake guest
// memory, and drives them the way an EPT violation on a watched page
// would.
//
// The fake memory is *real host memory*, deliberately. read_guest_word
// and apply_guest_store reach the page by casting the guest-physical
// address to a pointer, because the extended page tables make guest
// physical and host physical the same thing and this VMM's own mapping
// is the only writable one. So the watched page here is a 4 KB-aligned
// array and its address is its own page number - the same identity, and
// nothing about the code under test has to be relaxed to allow it.
//
// The expected column for the local APIC is not this harness's opinion.
// It was read out of KVM's apic_mmio_write and kvm_apic_send_ipi in
// .references/kvm/lapic.c and out of SDM 13.4 (the section CLAUDE.md and
// KVM both call 12.4/8.4.1), and every case where the two legitimately
// disagree carries the reason in a `diverge` entry - which is
// *asserted*, so a divergence that silently disappears fails the test
// just as a new one does.
#include "support/identity_page_table.h"
#include "zpp/hypervisor/hypervisor.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <print>
#include <string>
#include <vector>

using zpp::arch::x86_64::code_size;
using zpp::arch::x86_64::combine_with;
using zpp::arch::x86_64::memory_operation;
using field = zpp::arch::x86_64::vmx::vmcs::field;

// ---------------------------------------------------------------- memory
/**
 * The fake guest memory. Page 0 is the watched page and page 1 is what
 * follows it, which is what a straddling store would spill onto.
 */
alignas(0x1000) static std::uint8_t g_memory[4 * 0x1000];

static std::uint64_t base()
{
    return reinterpret_cast<std::uint64_t>(g_memory);
}

static std::uint64_t watched_page()
{
    return base() >> 12;
}

// ------------------------------------------------------ decoder feeding
static std::uint8_t g_code[15];
static code_size g_code_size = code_size::bits_64;
static bool g_decode_refuses = false;
static std::uint64_t g_decode_calls = 0;

static void set_code(std::initializer_list<std::uint8_t> bytes)
{
    std::memset(g_code, 0, sizeof(g_code));
    std::size_t at{};
    for (auto byte : bytes) {
        g_code[at++] = byte;
    }
}

// ------------------------------------------------- other shim behaviour
static zpp::arch::x86_64::vmx::epte g_epte{};
static bool g_epte_fails = false;
static std::uint64_t g_invalidations = 0;
static std::uint64_t g_monitor_trap_calls = 0;
static bool g_monitor_trap_armed = false;

static std::vector<std::uint64_t> g_commands;
static bool g_command_swallows = false;
static bool g_command_rewrites = false;
static std::uint64_t g_command_rewrite_to = 0;

/**
 * What `every_platform_processor_adopted` answers here.
 *
 * The real one lives in `local_apic.cpp`, which this harness does not
 * compile, and is tested for itself in `tests/local_apic`. What is under
 * test *here* is what `on_ept_violation` does with the answer, so the
 * answer is a knob.
 */
static bool g_roster_fully_adopted = false;

/**
 * What IA32_APIC_BASE reads, and how wide this processor's physical
 * addresses are - the two inputs to `local_apic_base`.
 *
 * Both are knobs because the pair is what `watch_local_apic`'s refusal
 * to arm on a relocated APIC page turns on, and a relocation above the
 * 36-bit fallback is the case that used to slip through it.
 */
static std::uint64_t g_apic_base_msr = 0;
static std::uint64_t g_physical_address_bits = 39;

// ------------------------------------------------------- shim defintions
namespace zpp::arch::x86_64
{
std::uint64_t rdmsr(std::uint32_t index)
{
    if (msr::ia32_apic_base == index) {
        return g_apic_base_msr;
    }
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

/**
 * The instruction fetch, reduced to the decode.
 *
 * The real one walks the guest's paging structures through a mapping
 * window to find the bytes at RIP, and then calls exactly this. The walk
 * is not what this harness is about, so the bytes are supplied directly
 * and the *real* arch::x86_64::decode still answers what they mean.
 */
std::optional<arch::x86_64::decoded_instruction>
hypervisor::decode_guest_instruction(std::size_t,
                                     arch::x86_64::context & context)
{
    ++g_decode_calls;

    for (std::size_t i{}; i < sizeof(this->last_fetched_code); ++i) {
        this->last_fetched_code[i] = g_code[i];
    }

    if (g_decode_refuses) {
        return {};
    }

    return arch::x86_64::decode(
        std::as_bytes(std::span{g_code}), context, g_code_size);
}

std::expected<arch::x86_64::vmx::epte *, zpp::error>
hypervisor::epte_for(std::uint64_t physical_address)
{
    // The table lookup is stubbed and the **bound is not**, which is the
    // same division this harness already makes for
    // `decode_guest_instruction`: the bytes are supplied and the real
    // decoder still says what they mean. `epte_for` indexes `epd` rather
    // than walking it, so the bound is the whole of its input validation
    // and it is the part a caller can be handed a guest's address for.
    if (!physical_address_within_ept(physical_address)) {
        return std::unexpected(
            zpp::error{error::physical_address_beyond_ept});
    }

    if (g_epte_fails) {
        return std::unexpected(zpp::error{error::out_of_ept_entries});
    }
    return &g_epte;
}

void hypervisor::invalidate_ept()
{
    ++g_invalidations;
}

void hypervisor::monitor_trap_flag(bool value)
{
    ++g_monitor_trap_calls;
    g_monitor_trap_armed = value;
}

std::optional<std::uint64_t>
hypervisor::on_interrupt_command(std::uint64_t command)
{
    g_commands.push_back(command);

    if (g_command_swallows) {
        return {};
    }

    if (g_command_rewrites) {
        return g_command_rewrite_to;
    }

    return command;
}

bool hypervisor::every_platform_processor_adopted()
{
    return g_roster_fully_adopted;
}

/**
 * The processor's physical-address width, which the real
 * `local_apic_base` masks IA32_APIC_BASE down to. The real one lives in
 * nested_ept.cpp and reads CPUID.80000008H:EAX[7:0]; this harness runs
 * on arm64 as often as not, so it is a knob.
 *
 * Thirty-nine by default rather than thirty-six, because thirty-six is
 * the fallback that made the four masks this replaced look correct -
 * a default of 36 would let the bug back in without a failing check.
 */
std::uint64_t hypervisor::physical_address_bits()
{
    return g_physical_address_bits;
}

} // namespace zpp::hypervisor

// ------------------------------------------------------------------- rig
using hypervisor_t = zpp::hypervisor::hypervisor;
using guest_write = hypervisor_t::guest_write;
using page_watch = hypervisor_t::page_watch;

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
        std::println("  FAIL {}", what);
        g_findings.push_back(what);
    }
}

/**
 * A place where this VMM's answer is *wrong*, or disagrees with KVM, and
 * is recorded rather than repaired here.
 *
 * The suite stays green on purpose. A permanently red harness is one
 * nobody runs, and repairing the behaviour is a change to the hypervisor
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
        std::println("  FAIL a recorded divergence no longer reproduces, "
                     "so the record is stale: {}",
                     what);
    } else {
        std::println("  DIVERGES {}", what);
    }
}

static std::string text(const char * format, ...)
    __attribute__((format(printf, 1, 2)));

static std::string text(const char * format, ...)
{
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    return buffer;
}

// --------------------------------------------------------- observations
static std::vector<guest_write> g_notified;
static std::vector<guest_write> g_filtered;
static std::vector<std::uint64_t> g_before_write;
static bool g_filter_refuses = false;
static bool g_filter_rewrites = false;
static std::uint64_t g_filter_rewrite_to = 0;

static void notify_handler(void *,
                           std::uint64_t,
                           const guest_write * write)
{
    g_notified.push_back(write ? *write : guest_write{});
}

static std::optional<std::uint64_t>
filter_handler(void *, std::uint64_t, const guest_write * write)
{
    g_filtered.push_back(write ? *write : guest_write{});

    if (g_filter_refuses) {
        return {};
    }

    if (g_filter_rewrites) {
        return g_filter_rewrite_to;
    }

    return write ? write->value : 0;
}

static void before_handler(void *, std::uint64_t page)
{
    g_before_write.push_back(page);
}

// -------------------------------------------------------------- fixture
static void write_field(field which, std::uint64_t value)
{
    // Straight into the shim's VMCS array, because the read-only fields
    // an exit reports - the qualification, the guest-linear address -
    // have getters and no setters on the real class, which is correct
    // of the real class and unhelpful here.
    zpp::arch::x86_64::vmx::g_vmcs[static_cast<std::uint64_t>(which)] =
        value;
}

/**
 * Models the stepped instruction retiring: a real single step leaves RIP
 * past the instruction, and `on_monitor_trap_flag` now uses that to tell
 * a step that ran from one whose instruction faulted instead - SDM
 * 26.5.2, sdm.txt:201495.
 */
// Returns where the guest now is, because that is what the exit path
// hands to `on_monitor_trap_flag`. It reads the field for itself no
// longer: `on_vm_exit` has already read `guest_rip` once for this exit
// and a VMREAD is an exit to the layer below on the target.
static std::uint64_t retire_stepped_instruction(std::uint64_t length = 3)
{
    auto rip = hv().vmcs.guest_rip() + length;
    write_field(field::guest_rip, rip);
    return rip;
}

static void reset()
{
    auto & self = hv();

    std::memset(g_memory, 0, sizeof(g_memory));
    std::memset(zpp::arch::x86_64::vmx::g_vmcs,
                0,
                sizeof(zpp::arch::x86_64::vmx::g_vmcs));

    for (auto & watch : self.watches) {
        watch.page = 0;
        watch.on_write = nullptr;
        watch.before_write = nullptr;
        watch.filter_write = nullptr;
        watch.context = nullptr;
        watch.behaviour = page_watch::mode::notify;
        watch.armed = false;
        watch.held.store(false);
    }

    for (std::size_t cpu{}; cpu < hypervisor_t::max_cpus; ++cpu) {
        self.stepping_watch[cpu] = false;
        self.stepping_page[cpu] = 0;
        self.stepping_offset[cpu] = 0;
    }

    self.emulated_writes = 0;
    self.filtered_writes = 0;
    self.stepped_writes = 0;
    self.emulated_length_disagreement = 0;
    self.emulated_length_reported = 0;
    self.emulated_length_decoded = 0;
    self.access_offset_decoded = 0;
    self.access_offset_unknown = 0;
    self.physical_offset_present = 0;
    self.physical_offset_agreed = 0;
    self.physical_offset_disagreed = 0;
    self.apic_page_commands_filtered = 0;
    self.apic_writes_undecoded = 0;
    self.emulated_trace_count = 0;
    self.watched_access_count = 0;
    self.module_access_count = 0;
    self.module_physical_to_virtual.clear();

    // The timer records, which persist across cases otherwise and would
    // make every arming count cumulative.
    for (std::size_t cpu{}; cpu < hypervisor_t::max_cpus; ++cpu) {
        self.timer_arm_count[cpu] = 0;
        self.timer_arm_recent_count[cpu] = 0;
        self.timer_lvt[cpu] = 0;
        self.timer_divide[cpu] = 0;
        for (std::size_t i{}; i < hypervisor_t::timer_arm_capacity; ++i) {
            self.timer_arm_value[cpu][i] = 0;
            self.timer_arm_tsc[cpu][i] = 0;
            self.timer_arm_recent_value[cpu][i] = 0;
            self.timer_arm_recent_tsc[cpu][i] = 0;
            self.timer_arm_recent_lvt[cpu][i] = 0;
            self.timer_arm_recent_divide[cpu][i] = 0;
        }
    }

    std::memset(self.last_fetched_code, 0, sizeof(self.last_fetched_code));

    g_notified.clear();
    g_filtered.clear();
    g_before_write.clear();
    g_commands.clear();
    g_filter_refuses = false;
    g_filter_rewrites = false;
    g_filter_rewrite_to = 0;
    g_command_swallows = false;
    g_command_rewrites = false;
    g_command_rewrite_to = 0;
    g_roster_fully_adopted = false;
    self.all_processors_started.store(false);
    g_decode_refuses = false;
    g_decode_calls = 0;
    g_code_size = code_size::bits_64;
    g_epte_fails = false;
    g_invalidations = 0;
    g_monitor_trap_calls = 0;
    g_monitor_trap_armed = false;
    g_epte = zpp::arch::x86_64::vmx::epte{};

    std::memset(g_code, 0, sizeof(g_code));
}

/**
 * Arms a watch the way the local APIC's own arming does, but with the
 * harness's recorders in place of the real handlers.
 */
static void arm(page_watch::handler on_write,
                page_watch::filter filter_write,
                void (*before_write)(void *, std::uint64_t) = nullptr)
{
    auto armed = hv().watch_guest_page_writes(base(),
                                              on_write,
                                              &hv(),
                                              page_watch::mode::notify,
                                              before_write,
                                              filter_write);
    check(armed.has_value(), "watch_guest_page_writes must succeed");
}

/**
 * One EPT violation, as the exit handler delivers it.
 *
 * `physical` is what the VMCS guest-physical address field reports, which
 * is the page-granular value on the machines this was measured on and a
 * full address elsewhere - which is exactly the question the offset
 * resolution answers.
 */
struct violation
{
    std::uint64_t qualification{};
    std::uint64_t linear{};
    std::uint64_t physical{};
    std::uint64_t rip = 0x400000;
    std::uint64_t reported_length{};
};

static constexpr std::uint64_t linear_valid = 1ull << 7;
static constexpr std::uint64_t operand_access = 1ull << 8;
static constexpr std::uint64_t data_write = 1ull << 1;

static bool fault(const violation & what,
                  zpp::arch::x86_64::context & registers)
{
    write_field(field::exit_qualification, what.qualification);
    write_field(field::guest_linear_address, what.linear);
    write_field(field::guest_physical_address, what.physical);
    write_field(field::guest_rip, what.rip);
    write_field(field::vm_exit_instruction_length, what.reported_length);

    // Into the context as well as the VMCS, because that is what the real
    // caller does. `on_vm_exit` reads `guest_rip` once at the top of
    // every exit and puts it in `context.rip`, and everything below -
    // including `on_ept_violation` - takes it from there rather than
    // spending another VMREAD. Setting only the field made the harness
    // drive a state the exit path cannot produce.
    registers.rip = what.rip;

    return hv().on_ept_violation(0, registers, what.physical);
}

static std::uint32_t at32(std::uint64_t offset)
{
    std::uint32_t value{};
    std::memcpy(&value, g_memory + offset, sizeof(value));
    return value;
}

static void put32(std::uint64_t offset, std::uint32_t value)
{
    std::memcpy(g_memory + offset, &value, sizeof(value));
}

// ------------------------------------------------------------- encodings
// Every form below addresses memory through `mod=00, rm=011`, which is
// `[rbx]` with no displacement - so the effective address the decoder
// computes is whatever the test put in rbx, and nothing here depends on
// the harness agreeing with the decoder about displacements.
static void mov_mem_reg32()
{
    set_code({0x89, 0x03}); // mov [rbx], eax
}

static void mov_mem_reg8()
{
    set_code({0x88, 0x03}); // mov [rbx], al
}

static void mov_mem_reg16()
{
    set_code({0x66, 0x89, 0x03}); // mov [rbx], ax
}

static void mov_mem_reg64()
{
    set_code({0x48, 0x89, 0x03}); // mov [rbx], rax
}

static void mov_mem_imm32(std::uint32_t value)
{
    set_code({0xc7,
              0x03,
              static_cast<std::uint8_t>(value),
              static_cast<std::uint8_t>(value >> 8),
              static_cast<std::uint8_t>(value >> 16),
              static_cast<std::uint8_t>(value >> 24)});
}

static void mov_reg_mem32()
{
    set_code({0x8b, 0x03}); // mov eax, [rbx]
}

static void or_mem_reg32()
{
    set_code({0x09, 0x03}); // or [rbx], eax
}

static void and_mem_reg32()
{
    set_code({0x21, 0x03}); // and [rbx], eax
}

static void add_mem_reg32()
{
    set_code({0x01, 0x03}); // add [rbx], eax
}

static void cmp_mem_imm()
{
    set_code({0x83, 0x3b, 0x12}); // cmp dword [rbx], 0x12
}

static void cmp_mem_reg32()
{
    set_code({0x39, 0x03}); // cmp [rbx], eax
}

static void test_mem_reg32()
{
    set_code({0x85, 0x03}); // test [rbx], eax
}

static void xchg_mem_reg32()
{
    set_code({0x87, 0x03}); // xchg [rbx], eax
}

static void lock_or_mem_reg32()
{
    set_code({0xf0, 0x09, 0x03}); // lock or [rbx], eax
}

static void bts_mem_imm(std::uint8_t bit)
{
    set_code({0x0f, 0xba, 0x2b, bit}); // bts dword [rbx], bit
}

static void bt_mem_imm(std::uint8_t bit)
{
    set_code({0x0f, 0xba, 0x23, bit}); // bt dword [rbx], bit
}

static void movzx_reg_mem8()
{
    set_code({0x0f, 0xb6, 0x03}); // movzx eax, byte [rbx]
}

static void movsx_reg_mem8()
{
    set_code({0x0f, 0xbe, 0x03}); // movsx eax, byte [rbx]
}

static zpp::arch::x86_64::decoded_instruction
decoded(const zpp::arch::x86_64::context & registers)
{
    auto answer = zpp::arch::x86_64::decode(
        std::as_bytes(std::span{g_code}), registers, g_code_size);
    if (!answer) {
        std::println("  FAIL the harness wrote bytes the decoder refuses");
        ++g_failures;
        return {};
    }
    return *answer;
}

// ===================================================== offset resolution
//
// on_ept_violation has three sources for the offset within the watched
// page, and which one it picks decides which device register a handler is
// told about. The rules, from hypervisor.cpp:3407-3460:
//
//   1. the guest-linear address, whenever qualification bit 7 is set,
//   2. otherwise the decoder's effective address,
//   3. otherwise the guest-physical address, which is reported at page
//      granularity so this is offset zero - and the access is then not
//      emulated at all, because address_known stays clear.
static void test_offset_resolution()
{
    std::println("\noffset resolution");

    // 1. The linear address answers it, and the page-granular physical
    //    address contributes only the page.
    {
        reset();
        arm(notify_handler, nullptr);
        put32(0x300, 0x11111111);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0xdeadbeef;
        mov_mem_reg32();

        auto ours = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = 0xffff'8000'0000'0300ull,
             .physical = base()},
            registers);

        check(ours, "a violation on a watched page is ours");
        check(1 == g_notified.size(),
              "the linear address reaches the handler as one write");
        if (1 == g_notified.size()) {
            check(g_notified[0].address == (base() + 0x300),
                  "offset comes from the guest-linear address");
        }
        check(0xdeadbeef == at32(0x300),
              "the store landed at the linear address's offset");
        check(0 == hv().access_offset_decoded,
              "the decoder is not credited when the exit answered");
    }

    // 2. Bit 7 clear, so the instruction's own addressing answers it.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0xdeadbeef;
        mov_mem_reg32();

        fault({.qualification = data_write, .physical = base()},
              registers);

        check(1 == g_notified.size(),
              "a decoded address is enough to emulate with bit 7 clear");
        if (1 == g_notified.size()) {
            check(g_notified[0].address == (base() + 0x300),
                  "offset comes from the decoder's effective address");
        }
        check(1 == hv().access_offset_decoded,
              "access_offset_decoded counts it");
        check(0 == hv().access_offset_unknown,
              "access_offset_unknown does not");
    }

    // 3. Neither source answers, so the access is stepped and the offset
    //    falls back to the guest-physical address's low bits.
    {
        reset();
        arm(notify_handler, nullptr);
        g_decode_refuses = true;

        zpp::arch::x86_64::context registers{};
        fault({.qualification = data_write, .physical = base()},
              registers);

        check(0 == g_notified.size(),
              "an unresolvable offset is not emulated");
        check(1 == hv().access_offset_unknown,
              "access_offset_unknown counts it");
        check(1 == hv().stepped_writes, "and it is stepped instead");
        check(g_monitor_trap_armed, "stepping arms the monitor trap flag");
        check(0 == hv().stepping_offset[0],
              "the fallback offset is the page-granular zero");
    }

    // The counters. physical_offset_present is taken before anything is
    // decided, so it describes every violation rather than a subset -
    // hypervisor.cpp:3413-3433.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        mov_mem_reg32();

        // Page-granular physical: nothing present, nothing compared.
        fault({.qualification = linear_valid | operand_access,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(0 == hv().physical_offset_present,
              "a page-aligned guest-physical address is not present");
        check(0 == hv().physical_offset_agreed, "and is not compared");
        check(0 == hv().physical_offset_disagreed, "either way");
    }

    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        mov_mem_reg32();

        // Present and agreeing with the linear address.
        fault({.qualification = linear_valid | operand_access,
               .linear = 0x1234'5300,
               .physical = base() + 0x300},
              registers);

        check(1 == hv().physical_offset_present,
              "a non-zero guest-physical offset is present");
        check(1 == hv().physical_offset_agreed,
              "and agrees with the linear address");
        check(0 == hv().physical_offset_disagreed, "so does not disagree");
    }

    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x310;
        registers.rax = 0x5a5a5a5a;
        mov_mem_reg32();

        // Present and disagreeing. The linear address wins, which is the
        // whole point of the ordering.
        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x1234'5300,
               .physical = base() + 0x310},
              registers);

        check(1 == hv().physical_offset_present, "present");
        check(0 == hv().physical_offset_agreed, "not agreed");
        check(1 == hv().physical_offset_disagreed, "disagreed");
        check(0x5a5a5a5a == at32(0x300),
              "the linear address wins over the guest-physical one");
        check(0 == at32(0x310),
              "so the guest-physical offset is not written");
    }

    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        mov_mem_reg32();

        // Present, but bit 7 clear, so there is nothing to compare it
        // against and neither of the pair moves.
        fault({.qualification = 0, .physical = base() + 0x300}, registers);

        check(1 == hv().physical_offset_present,
              "present is counted with bit 7 clear");
        check(0 == hv().physical_offset_agreed,
              "but agreed needs a linear address");
        check(0 == hv().physical_offset_disagreed,
              "and so does disagreed");
    }

    // The decoder's answer wins over a *present* guest-physical offset
    // when bit 7 is clear. That is the case the counters exist to judge:
    // if the hardware's low twelve bits are trustworthy, this is a
    // needless decode; if they are not, this is the only correct answer.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0xa5a5a5a5;
        mov_mem_reg32();

        fault({.qualification = data_write, .physical = base() + 0x310},
              registers);

        check(0xa5a5a5a5 == at32(0x300),
              "the decoder outranks a non-zero guest-physical offset");
        check(0 == at32(0x310), "which is therefore not written");
        check(1 == hv().access_offset_decoded, "counted as decoded");
    }

    // A refused decode with a *present* guest-physical offset. The
    // address keeps the offset - `address` starts as the whole
    // guest-physical address - but address_known stays clear, so the
    // access is refused emulation and stepped at that offset anyway.
    // hypervisor.cpp:3409-3460.
    {
        reset();
        arm(notify_handler, nullptr);
        g_decode_refuses = true;

        zpp::arch::x86_64::context registers{};
        fault({.qualification = data_write, .physical = base() + 0x300},
              registers);

        check(1 == hv().access_offset_unknown,
              "a refused decode is unknown even with a physical offset");
        check(1 == hv().stepped_writes, "so it is stepped");
        check(0x300 == hv().stepping_offset[0],
              "and the step still carries the guest-physical offset");
    }

    // Bit 7 set and bit 8 clear is the processor walking the guest's own
    // paging structures, which has no store to carry out. SDM Table 28-7.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0xcafe;
        mov_mem_reg32();

        fault({.qualification = linear_valid,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(0 == at32(0x300),
              "a paging-structure access is not emulated");
        check(1 == hv().stepped_writes, "it is stepped");
        check(0 == g_notified.size(), "and nothing is notified yet");
        check(0x300 == hv().stepping_offset[0],
              "the step still uses the linear address's offset");
    }

    // Bit 7 clear and a decoded address is treated as an operand access
    // outright, because a watched page is device memory and no guest puts
    // its paging structures there. hypervisor.cpp:3447-3460.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0xfeed;
        mov_mem_reg32();

        fault({.qualification = 0, .physical = base()}, registers);

        check(0xfeed == at32(0x300),
              "bit 8 is not required when bit 7 is clear");
        check(0 == hv().stepped_writes, "so nothing is stepped");
    }

    // An unwatched page is not ours, and neither is a watch on a
    // different page.
    {
        reset();
        arm(notify_handler, nullptr);

        zpp::arch::x86_64::context registers{};
        check(!fault({.physical = base() + 0x1000}, registers),
              "a violation on an unwatched page is refused");

        reset();
        check(!fault({.physical = base()}, registers),
              "so is one with no watch armed at all");
    }
}

// ==============================================
// carry_out_guest_instruction
static void test_carry_out()
{
    std::println("\ncarry_out_guest_instruction");

    auto run = [](zpp::arch::x86_64::context & registers,
                  std::uint64_t address,
                  guest_write & performed,
                  bool & changed,
                  std::optional<std::uint64_t> known = {}) {
        auto instruction = decoded(registers);
        return hv().carry_out_guest_instruction(
            address, instruction, registers, performed, changed, known);
    };

    // A plain store of every width.
    struct
    {
        void (*encode)();
        std::uint8_t size;
        std::uint64_t value;
        std::uint64_t expected;
    } stores[] = {
        {mov_mem_reg8, 1, 0xffull, 0xffull},
        {mov_mem_reg16, 2, 0xbeefull, 0xbeefull},
        {mov_mem_reg32, 4, 0xdeadbeefull, 0xdeadbeefull},
        {mov_mem_reg64, 8, 0x0123456789abcdefull, 0x0123456789abcdefull},
    };

    for (auto & one : stores) {
        reset();
        one.encode();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = one.value;

        std::memset(g_memory + 0x40, 0xcc, 16);

        guest_write performed{};
        auto changed = false;
        auto carried = run(registers, base() + 0x40, performed, changed);

        std::uint64_t held{};
        std::memcpy(&held, g_memory + 0x40, one.size);

        check(carried, text("a %u-byte store is carried out", one.size));
        check(changed, text("a %u-byte store changes memory", one.size));
        check(held == one.expected,
              text("a %u-byte store leaves its value", one.size));
        check(performed.size == one.size,
              text("a %u-byte store reports its width", one.size));
        check(performed.value == one.expected,
              text("a %u-byte store reports its value", one.size));
        check(performed.address == (base() + 0x40),
              text("a %u-byte store reports its address", one.size));
        check(0xcc == g_memory[0x40 + one.size],
              text("a %u-byte store writes no further", one.size));
    }

    // The immediate form, whose operand is in the instruction rather than
    // in a register - and whose length therefore includes it, which is
    // what the guest's RIP is advanced by.
    {
        reset();
        mov_mem_imm32(0x89abcdef);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "an immediate store is carried out");
        check(0x89abcdef == at32(0x40), "with the immediate's value");
        check(0x89abcdef == performed.value, "and reports it");
        check(6 == decoded(registers).length,
              "and measures its own length including the immediate");
    }

    // A store must not read what is there first: a device register that
    // reads back differently from what was written is exactly the case
    // this exists to observe. hypervisor.cpp:3159-3163.
    {
        reset();
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x12345678;
        put32(0x40, 0xffffffff);

        guest_write performed{};
        auto changed = false;

        // known_contents that could only be visible if the store looked
        // at the old value at all.
        check(run(registers, base() + 0x40, performed, changed, 0x99),
              "a store carries out with known_contents present");
        check(0x12345678 == at32(0x40),
              "a store ignores known_contents entirely");
        check(0x12345678 == performed.value,
              "and reports its own operand");
    }

    // A load leaves memory alone, fills the register and reports what was
    // there.
    {
        reset();
        mov_reg_mem32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0xffffffffffffffffull;
        put32(0x40, 0x0badf00d);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "a load is carried out");
        check(!changed, "a load does not change memory");
        check(0x0badf00d == at32(0x40), "so memory is untouched");
        check(0x0badf00d == registers.rax,
              "a 32-bit load zeroes the rest of the register");
        check(0x0badf00d == performed.value,
              "and reports what memory held");
        check(4 == performed.size, "at the access width");
    }

    // The widening moves, where the destination width is not the access
    // width. decoded_instruction::destination_size exists for this.
    {
        reset();
        movzx_reg_mem8();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0xffffffffffffffffull;
        g_memory[0x40] = 0x81;

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "movzx is carried out");
        check(0x81 == registers.rax,
              "movzx zero-extends into the whole "
              "register");
        check(!changed, "movzx changes no memory");
    }

    {
        reset();
        movsx_reg_mem8();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0;
        g_memory[0x40] = 0x81;

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "movsx is carried out");
        check(0xffffff81ull == registers.rax,
              "movsx sign-extends to the destination width");
    }

    // An examine changes nothing but the flags.
    {
        reset();
        cmp_mem_imm();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        put32(0x40, 0x12);
        write_field(field::guest_rflags, 0x2);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "cmp is carried out");
        check(!changed, "cmp changes no memory");
        check(0x12 == at32(0x40), "so memory is untouched");
        check(0x12 == performed.value, "and it reports what was there");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::zero),
              "cmp of equal operands sets the zero flag");
    }

    // CMP with a *register* operand, which used to be refused: the
    // register group carried ADD, OR, AND, SUB and XOR but not
    // 0x38/0x39, while the immediate form 0x83 /7 and TEST against a
    // register were both there. This asserted the refusal; it now
    // asserts the emulation, end to end, and the flags are the
    // interesting half - a compare that leaves RFLAGS alone sends the
    // guest down the other branch.
    {
        reset();
        cmp_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x12;
        put32(0x40, 0x12);
        write_field(field::guest_rflags,
                    0x2 | zpp::arch::x86_64::status_flag::carry);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "cmp against a register is carried out");
        check(!changed, "cmp changes no memory");
        check(0x12 == at32(0x40), "so memory is untouched");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::zero),
              "cmp of equal operands sets the zero flag");
        check(0 == (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "and clears the carry flag it was given");
    }

    // The same, with memory *below* the register, which is the case that
    // tells the two directions apart: 0x38/0x39 subtract the register
    // from memory, and getting it the other way round sets the carry and
    // sign flags backwards.
    {
        reset();
        cmp_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x20;
        put32(0x40, 0x10);
        write_field(field::guest_rflags, 0x2);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "cmp against a larger register is carried out");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "memory below the register borrows");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::sign),
              "and leaves a negative result");
        check(0 == (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::zero),
              "and no zero flag");
    }

    {
        reset();
        test_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x1;
        put32(0x40, 0x2);
        write_field(field::guest_rflags,
                    0x2 | zpp::arch::x86_64::status_flag::carry);

        guest_write performed{};
        auto changed = false;

        run(registers, base() + 0x40, performed, changed);

        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::zero),
              "test of disjoint bits sets the zero flag");
        check(0 == (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "and clears a stale carry, because RFLAGS is assigned "
              "rather than merged");
    }

    // A read-modify-write reports the *result*, not the operand.
    {
        reset();
        or_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x0000ff00;
        put32(0x40, 0x000000ff);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "or is carried out");
        check(changed, "or changes memory");
        check(0x0000ffff == at32(0x40), "with the combination");
        check(0x0000ffff == performed.value,
              "and reports the combination, not the operand");
    }

    {
        reset();
        and_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x0f0f0f0f;
        put32(0x40, 0xf0f0f0f0);
        write_field(field::guest_rflags, 0x2);

        guest_write performed{};
        auto changed = false;

        run(registers, base() + 0x40, performed, changed);

        check(0 == at32(0x40), "and of disjoint operands leaves zero");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::zero),
              "and sets the zero flag the guest branches on");
    }

    {
        reset();
        add_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 1;
        put32(0x40, 0xffffffff);

        guest_write performed{};
        auto changed = false;

        run(registers, base() + 0x40, performed, changed);

        check(0 == at32(0x40), "add wraps at the access width");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "and sets the carry flag");
    }

    // The bit operations, which are the forms the narrow decoder refused.
    {
        reset();
        bts_mem_imm(5);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        put32(0x40, 0);
        write_field(field::guest_rflags,
                    0x2 | zpp::arch::x86_64::status_flag::carry);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "bts is carried out");
        check(changed, "bts changes memory");
        check((1u << 5) == at32(0x40), "and sets the bit named");
        check(0 == (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "carry takes the bit as it was");
    }

    {
        reset();
        bt_mem_imm(5);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        put32(0x40, 1u << 5);
        write_field(field::guest_rflags, 0x2);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "bt is carried out");
        check(!changed, "bt changes no memory");
        check((1u << 5) == at32(0x40), "so memory is untouched");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "and carry takes the bit");
    }

    // An exchange writes the operand and takes the old value into the
    // register, and touches no flags.
    {
        reset();
        xchg_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0xaaaaaaaa;
        put32(0x40, 0xbbbbbbbb);
        write_field(field::guest_rflags,
                    0x2 | zpp::arch::x86_64::status_flag::carry);

        guest_write performed{};
        auto changed = false;

        check(run(registers, base() + 0x40, performed, changed),
              "xchg is carried out");
        check(changed, "xchg changes memory");
        check(0xaaaaaaaa == at32(0x40), "memory takes the operand");
        check(0xbbbbbbbb == registers.rax,
              "the register takes what memory held");
        check(0xaaaaaaaa == performed.value,
              "and the report is what memory now holds");
        check(0 != (hv().vmcs.guest_rflags() &
                    zpp::arch::x86_64::status_flag::carry),
              "xchg leaves the flags alone");
    }

    // known_contents is threaded through and the old value is *not* read
    // again. hypervisor.cpp:3166-3184. Proved by making the two differ:
    // a re-read would combine against what is in memory, and the result
    // shows which one was used.
    {
        reset();
        or_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0x00ff0000;
        put32(0x40, 0x0000ffff);

        guest_write performed{};
        auto changed = false;

        check(run(registers,
                  base() + 0x40,
                  performed,
                  changed,
                  0xff000000ull),
              "a combine with known_contents is carried out");
        check(0xffff0000 == at32(0x40),
              "the combination uses known_contents, not memory");
        check(0xffff0000 == performed.value, "and reports that");
    }

    // The same, proved from the other side: with no memory to read at
    // all, a form that needs the old contents and does not write still
    // succeeds when the caller supplied them.
    {
        reset();
        cmp_mem_imm();

        zpp::arch::x86_64::context registers{};
        registers.rbx = 0x1000;

        guest_write performed{};
        auto changed = false;

        check(run(registers, 0x1000, performed, changed, 0x1234ull),
              "known_contents removes the read entirely");
        check(0x1234 == performed.value, "and is what gets reported");

        check(!run(registers, 0x1000, performed, changed),
              "without it the unreachable read refuses");
    }

    // The flags after a store are left exactly alone, which is what MOV
    // does. hypervisor.cpp:3221-3224.
    {
        reset();
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        registers.rax = 0;
        write_field(field::guest_rflags,
                    0x2 | zpp::arch::x86_64::status_flag::carry |
                        zpp::arch::x86_64::status_flag::zero);

        guest_write performed{};
        auto changed = false;

        run(registers, base() + 0x40, performed, changed);

        check((0x2 | zpp::arch::x86_64::status_flag::carry |
               zpp::arch::x86_64::status_flag::zero) ==
                  hv().vmcs.guest_rflags(),
              "a store touches no flags");
    }

    // The emulated trace keeps everything except plain stores.
    // hypervisor.cpp:3242-3266.
    {
        reset();
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x40;
        guest_write performed{};
        auto changed = false;

        run(registers, base() + 0x40, performed, changed);
        check(0 == hv().emulated_trace_count,
              "a plain store is not traced");

        or_mem_reg32();
        run(registers, base() + 0x40, performed, changed);
        check(1 == hv().emulated_trace_count, "a combine is");
        check(1 == hv().emulated_trace[0].wrote, "and says it wrote");
        check(static_cast<std::uint32_t>(memory_operation::combine) ==
                  hv().emulated_trace[0].what,
              "and which operation it was");

        mov_reg_mem32();
        run(registers, base() + 0x40, performed, changed);
        check(2 == hv().emulated_trace_count, "so is a load");
        check(0 == hv().emulated_trace[1].wrote,
              "which says it did not "
              "write");
    }

    // An unreachable address refuses at each of the two places it can.
    {
        reset();
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = 0x1000;
        guest_write performed{};
        auto changed = false;

        check(!run(registers, 0x1000, performed, changed),
              "a store to an unmapped address refuses");

        or_mem_reg32();
        check(!run(registers, 0x1000, performed, changed),
              "and so does a combine, at the read");
    }

    // read_guest_word answers only the four architectural widths.
    {
        reset();
        put32(0x40, 0x11223344);

        check(0x44 == hv().read_guest_word(base() + 0x40, 1),
              "read_guest_word answers one byte");
        check(0x3344 == hv().read_guest_word(base() + 0x40, 2), "and two");
        check(0x11223344 == hv().read_guest_word(base() + 0x40, 4),
              "and four");
        check(hv().read_guest_word(base() + 0x40, 8).has_value(),
              "and eight");
        check(!hv().read_guest_word(base() + 0x40, 3).has_value(),
              "and refuses three");
        check(!hv().read_guest_word(base() + 0x40, 0).has_value(),
              "and zero");
        check(!hv().read_guest_word(0x1000, 4).has_value(),
              "and an address it cannot reach");
    }

    // Which decoder the exit path actually uses.
    //
    // hypervisor.h:3714-3754 documents a switch,
    // `decode_watched_page_fully = false`, and says "the decoder itself
    // is kept, tested, and unused by the exit path". Both halves are now
    // stale: nothing anywhere reads that constant, and on_ept_violation
    // calls decode_guest_instruction unconditionally, which calls the
    // *full* arch::x86_64::decode. The narrow decode_memory_store in
    // decoder.h is the one nothing but scripts/ci/decoder-test.cpp uses.
    //
    // Proved rather than asserted from the source: BTS is a form only the
    // full decoder answers, and it is emulated end to end below.
    {
        reset();
        arm(notify_handler, nullptr);
        bts_mem_imm(9);
        put32(0x300, 0);

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x300,
               .physical = base()},
              registers);

        check((1u << 9) == at32(0x300),
              "the exit path emulates BTS, so it is on the full decoder");
        check(!zpp::arch::x86_64::decode_memory_store(
                   std::as_bytes(std::span{g_code}), registers)
                   .has_value(),
              "which the narrow decode_memory_store refuses");
    }

    // apply_guest_store, the same.
    {
        reset();

        check(
            hv().apply_guest_store(base() + 0x40, {.value = 1, .size = 1}),
            "apply_guest_store writes one byte");
        check(!hv().apply_guest_store(base() + 0x40,
                                      {.value = 1, .size = 3}),
              "and refuses three");
        check(!hv().apply_guest_store(0x1000, {.value = 1, .size = 4}),
              "and an address it cannot reach");
    }
}

// ================================================ filter / notify
// contract
//
// The contract, from hypervisor.cpp:3473-3629:
//
//   - filter_write is consulted before the write, for every form that
//     writes memory, with the value that form would leave behind;
//   - on_write is called only when the filter did NOT run and memory
//     actually changed;
//   - a filter returning a different value rewrites a plain store, and
//     refuses emulation for any other form.
static void test_filter_notify()
{
    std::println("\nfilter and notify contract");

    struct form
    {
        const char * name;
        void (*encode)();
        bool writes;
        std::uint32_t before;
        std::uint32_t operand;
        std::uint32_t after;
    };

    const form forms[] = {
        {"store", mov_mem_reg32, true, 0x0000ffff, 0x12345678, 0x12345678},
        {"or", or_mem_reg32, true, 0x0000ffff, 0x00ff0000, 0x00ffffff},
        {"and", and_mem_reg32, true, 0x0000ffff, 0x000000ff, 0x000000ff},
        {"add", add_mem_reg32, true, 0x00000001, 0x00000002, 0x00000003},
        {"xchg", xchg_mem_reg32, true, 0x0000ffff, 0x12345678, 0x12345678},
        {"lock or",
         lock_or_mem_reg32,
         true,
         0x0000ff00,
         0x000000ff,
         0x0000ffff},
        {"load", mov_reg_mem32, false, 0x0000ffff, 0, 0x0000ffff},
        {"cmp", cmp_mem_imm, false, 0x0000ffff, 0x1234, 0x0000ffff},
        {"test", test_mem_reg32, false, 0x0000ffff, 0x1234, 0x0000ffff},
    };

    // With a filter armed: every form that writes memory is consulted,
    // and none of them reaches the notify.
    for (auto & one : forms) {
        reset();
        arm(notify_handler, filter_handler);
        put32(0x300, one.before);
        one.encode();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = one.operand;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(one.writes == (1 == g_filtered.size()),
              text("%s: the filter is consulted iff it writes memory",
                   one.name));
        check(0 == g_notified.size(),
              text("%s: the notify never runs beside a filter", one.name));
        check(one.after == at32(0x300),
              text("%s: memory holds what the form produces", one.name));

        if (one.writes && (1 == g_filtered.size())) {
            check(g_filtered[0].value == one.after,
                  text("%s: the filter sees the value it would leave "
                       "behind",
                       one.name));
            check(g_filtered[0].address == (base() + 0x300),
                  text("%s: at the faulting address", one.name));
            check(4 == g_filtered[0].size,
                  text("%s: at the access width", one.name));
        }
    }

    // With no filter armed: only the forms that changed memory notify.
    for (auto & one : forms) {
        reset();
        arm(notify_handler, nullptr);
        put32(0x300, one.before);
        one.encode();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = one.operand;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(one.writes == (1 == g_notified.size()),
              text("%s: the notify runs iff memory changed", one.name));

        if (one.writes && (1 == g_notified.size())) {
            check(g_notified[0].value == one.after,
                  text("%s: and is told what memory now holds", one.name));
        }
    }

    // A filter that refuses. The instruction still retires - the guest
    // would fault forever otherwise - but memory is left alone.
    for (auto & one : forms) {
        if (!one.writes) {
            continue;
        }

        reset();
        arm(notify_handler, filter_handler);
        g_filter_refuses = true;
        put32(0x300, one.before);
        one.encode();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = one.operand;

        auto rip = 0x400000ull;
        auto ours = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = 0x300,
             .physical = base(),
             .rip = rip},
            registers);

        auto instruction = decoded(registers);

        check(ours,
              text("%s: a refused write is still handled", one.name));
        check(one.before == at32(0x300),
              text("%s: a refused write leaves memory alone", one.name));
        check(0 == g_notified.size(),
              text("%s: and notifies nobody", one.name));
        check(1 == hv().filtered_writes,
              text("%s: filtered_writes counts it", one.name));
        check(1 == hv().emulated_writes,
              text("%s: emulated_writes counts it too", one.name));
        check(hv().vmcs.guest_rip() == (rip + instruction.length),
              text("%s: and the guest retires the instruction", one.name));

        // And the two accounts of where the guest is agree.
        //
        // `context.rip` is what `on_vm_exit` filled from the VMCS and
        // what everything on the exit path reads instead of asking the
        // processor again - so a retirement that moves the field and not
        // the context leaves the two differing by the length of one
        // instruction, which reads as a perfectly plausible address
        // everywhere it is then used. Nothing else in this file would
        // notice.
        check(registers.rip == hv().vmcs.guest_rip(),
              text("%s: and context.rip moves with it", one.name));
    }

    // A filter that rewrites a plain store: the store's operand is
    // replaced and the rewritten value is what lands.
    {
        reset();
        arm(notify_handler, filter_handler);
        g_filter_rewrites = true;
        g_filter_rewrite_to = 0xa5a5a5a5;
        put32(0x300, 0);
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0x12345678;

        auto ours = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = 0x300,
             .physical = base()},
            registers);

        check(ours, "a rewritten store is handled");
        check(0xa5a5a5a5 == at32(0x300),
              "a filter rewrites a plain store's value");
        check(0 == g_notified.size(), "and the notify still does not run");
        check(1 == hv().emulated_writes, "counted as an emulated write");
    }

    // A filter that rewrites anything else. The comment at
    // hypervisor.cpp:3548-3572 says such an access "takes the stepping
    // path instead" - it does not. `return false` there is the *caller's*
    // signal that nothing had the page watched, and hypervisor.cpp:9821
    // answers it with on_unhandled_exit, which stops the processor.
    {
        auto stopped = 0;
        auto stepped = 0;

        for (auto & one : forms) {
            if (!one.writes || (0 == std::strcmp("store", one.name)) ||
                (0 == std::strcmp("xchg", one.name))) {
                continue;
            }

            reset();
            arm(notify_handler, filter_handler);
            g_filter_rewrites = true;
            g_filter_rewrite_to = 0xa5a5a5a5;
            put32(0x300, one.before);
            one.encode();

            zpp::arch::x86_64::context registers{};
            registers.rbx = base() + 0x300;
            registers.rax = one.operand;

            auto ours =
                fault({.qualification =
                           linear_valid | operand_access | data_write,
                       .linear = 0x300,
                       .physical = base()},
                      registers);

            if (!ours) {
                ++stopped;
            }
            if (0 != hv().stepped_writes) {
                ++stepped;
            }

            check(one.before == at32(0x300),
                  text("%s: a refused rewrite leaves memory alone",
                       one.name));
        }

        check((0 == stopped) && (4 == stepped),
              text("all %d non-store forms whose value a filter rewrote "
                   "take the stepping path rather than stopping the "
                   "processor - refusing emulation is safe, stopping is "
                   "not",
                   stepped));
    }

    // xchg is the one non-store form whose rewrite *is* honoured, because
    // apply() returns the operand for it exactly as it does for a store -
    // so replacing store->operand replaces what lands. The guard is on
    // memory_operation::store alone, so it refuses this anyway.
    {
        reset();
        arm(notify_handler, filter_handler);
        g_filter_rewrites = true;
        g_filter_rewrite_to = 0xa5a5a5a5;
        put32(0x300, 0x1111);
        xchg_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0x2222;

        auto ours = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = 0x300,
             .physical = base()},
            registers);

        check(ours,
              "xchg: a filter's rewrite is honoured, because apply() "
              "returns the operand for an exchange exactly as for a "
              "store");
    }

    // before_write runs on every violation, whatever the form and
    // whatever the filter decides. hypervisor.cpp:3315-3319.
    {
        reset();
        arm(notify_handler, filter_handler, before_handler);
        g_filter_refuses = true;
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(1 == g_before_write.size(),
              "before_write runs even when the filter refuses");
        check(watched_page() == g_before_write[0],
              "and is given the page");
    }

    {
        reset();
        arm(notify_handler, nullptr, before_handler);
        g_decode_refuses = true;

        zpp::arch::x86_64::context registers{};
        fault({.qualification = data_write, .physical = base()},
              registers);

        check(1 == g_before_write.size(),
              "before_write runs on the stepping path too");
    }

    // The stepped path notifies from on_monitor_trap_flag, reading the
    // value back out of the page. hypervisor.cpp:3786-3799.
    {
        reset();
        arm(notify_handler, filter_handler);
        g_decode_refuses = true;

        zpp::arch::x86_64::context registers{};
        fault({.qualification = data_write, .physical = base() + 0x300},
              registers);

        check(0 == g_filtered.size(),
              "a stepped write never reaches the filter");

        // The guest's own instruction retires here.
        put32(0x300, 0x0c0ffee0);
        auto retired_at = retire_stepped_instruction();

        check(hv().on_monitor_trap_flag(0, retired_at),
              "the step completes");
        check(1 == g_notified.size(), "and the notify runs from the step");
        if (1 == g_notified.size()) {
            check(0x0c0ffee0 == g_notified[0].value,
                  "with the value read back out of the page");
            check((base() + 0x300) == g_notified[0].address,
                  "at the offset the violation resolved");
            check(4 == g_notified[0].size,
                  "and a width of four, which the exit never reported");
        }
        check(!g_monitor_trap_armed, "the trap flag is disarmed");
        check(1 == hv().watched_access_count, "and the access recorded");
    }

    // The qualification measured on the rig: 0x2b against the local APIC
    // page, which is bits 0, 1, 3 and 5 - a read-modify-write, with bit 7
    // clear so no linear address. hypervisor.cpp:3609-3619 records that
    // every one of these used to go past both hooks. They must reach the
    // filter now, by way of the decoder's address.
    {
        reset();
        arm(notify_handler, filter_handler);
        put32(0x300, 0x0000ff00);
        lock_or_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0x000000ff;

        auto ours =
            fault({.qualification = 0x2b, .physical = base()}, registers);

        check(ours, "qualification 0x2b on a watched page is handled");
        check(1 == g_filtered.size(),
              "a locked read-modify-write with bit 7 clear reaches the "
              "filter");
        check(0 == g_notified.size(), "and not the notify beside it");
        check(0x0000ffff == at32(0x300), "and the combination lands");
        check(1 == g_decode_calls,
              "one decode per violation, which is what resolved it");
    }

    // An MTF exit with no step in progress is refused.
    {
        reset();
        check(!hv().on_monitor_trap_flag(0, 0),
              "an unexpected monitor trap exit is refused");
    }

    // The decoded length wins over the VMCS's, and a disagreement stops
    // the processor rather than resuming at either address.
    // hypervisor.cpp:3636-3657, SDM 30.2.5.
    {
        reset();
        arm(notify_handler, nullptr);
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;

        auto ours = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = 0x300,
             .physical = base(),
             .rip = 0x400000,
             .reported_length = 7},
            registers);

        check(!ours, "a length disagreement stops the processor");
        check(1 == hv().emulated_length_disagreement, "and is counted");
        check(7 == hv().emulated_length_reported,
              "with what was reported");
        check(2 == hv().emulated_length_decoded, "and what was decoded");
        check(0x400000 == hv().vmcs.guest_rip(),
              "RIP is left where it was");
    }

    {
        reset();
        arm(notify_handler, nullptr);
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;

        check(fault({.qualification =
                         linear_valid | operand_access | data_write,
                     .linear = 0x300,
                     .physical = base(),
                     .rip = 0x400000,
                     .reported_length = 2},
                    registers),
              "an agreeing length is fine");
        check(0x400002 == hv().vmcs.guest_rip(),
              "and RIP advances by the decoded length");
        check(0x400002 == registers.rip,
              "and context.rip with it - see the refusal cases above for "
              "what a disagreement costs");
    }
}

// =========================================== the local APIC's own
// handlers
//
// KVM's apic_mmio_write (.references/kvm/lapic.c:2417) is the reference:
// it gates on width and alignment, then dispatches on `offset & 0xff0`,
// and only APIC_ICR - 0x300 - reaches kvm_apic_send_ipi (lapic.c:1515).
static constexpr std::uint64_t interrupt_command_low = 0x300;
static constexpr std::uint64_t interrupt_command_high = 0x310;

/**
 * What KVM composes from an xAPIC interrupt command register write, from
 * kvm_apic_send_ipi (lapic.c:1515-1537) and its caller
 * kvm_lapic_reg_write's APIC_ICR case (lapic.c:2323-2329), in the same
 * shape this VMM's on_interrupt_command takes: the low dword with the
 * destination in bits 63:32.
 *
 * Two things it does that this VMM does not, both visible below:
 *   - the busy bit is cleared before the command is composed
 *     (lapic.c:2326, `val &= ~APIC_ICR_BUSY`),
 *   - the destination is eight bits (lapic.c:1532,
 *     GET_XAPIC_DEST_FIELD, which is `((x) >> 24) & 0xff` in
 *     arch/x86/include/asm/apicdef.h - not in .references, so this
 *     harness proves only what the shift alone gives).
 */
static constexpr std::uint32_t apic_icr_busy = 1u << 12;

static std::uint64_t kvm_command(std::uint32_t low, std::uint32_t high)
{
    return (low & ~apic_icr_busy) |
           (static_cast<std::uint64_t>((high >> 24) & 0xff) << 32);
}

static void test_local_apic()
{
    std::println("\nlocal APIC filter and notify");

    auto filter =
        [](std::uint64_t offset, std::uint64_t value, std::uint8_t size) {
            guest_write write{
                .address = base() + offset,
                .value = value,
                .size = size,
            };
            return hypervisor_t::filter_local_apic_write(
                &hv(), watched_page(), &write);
        };

    // Only 0x300 is acted on. Every other 16-byte aligned register on the
    // page passes through unchanged and reaches no command handler.
    {
        reset();

        for (std::uint64_t offset = 0; offset < 0x1000; offset += 0x10) {
            g_commands.clear();
            std::memset(g_memory, 0, 0x1000);
            put32(interrupt_command_high, 0xab000000);

            // The filter decides; it never writes. Anything it changed
            // would be a write the guest did not make, applied before
            // the guest's own write lands.
            std::uint8_t before[0x1000]{};
            std::memcpy(before, g_memory, sizeof(before));

            auto answer = filter(offset, 0x000c4500, 4);

            auto acted = (interrupt_command_low == offset);

            check(acted == (1 == g_commands.size()),
                  text("apic offset 0x%llx is acted on iff it is the "
                       "interrupt command register",
                       static_cast<unsigned long long>(offset)));

            check(answer.has_value() && (0x000c4500 == *answer),
                  text("apic offset 0x%llx passes the value through",
                       static_cast<unsigned long long>(offset)));

            check(0 == std::memcmp(before, g_memory, sizeof(before)),
                  text("apic offset 0x%llx writes nothing itself",
                       static_cast<unsigned long long>(offset)));
        }
    }

    // The composition: low dword plus the destination out of 0x310's top
    // eight bits, matching kvm_apic_send_ipi.
    {
        const std::uint32_t lows[] = {
            0x000000ff,
            0x000c4500, // INIT, level assert
            0x000c4600, // start-up
            0x000846f0,
            0x000c0500,
            0x00000000,
            0x000c5500, // the same INIT with the busy bit set
            0x00001000, // the busy bit alone
        };
        const std::uint32_t highs[] = {
            0x00000000,
            0x01000000,
            0xff000000,
            0x0a5a0000,
            0xffffffff,
        };

        auto busy_mismatches = 0;
        auto other_mismatches = 0;

        for (auto low : lows) {
            for (auto high : highs) {
                reset();
                put32(interrupt_command_high, high);

                auto answer = filter(interrupt_command_low, low, 4);

                check(1 == g_commands.size(),
                      "the interrupt command register is acted on");
                check(answer.has_value() && (low == *answer),
                      "and the guest's own write goes out unchanged");

                if (1 == g_commands.size()) {
                    check(g_commands[0] ==
                              (low | (std::uint64_t{high >> 24} << 32)),
                          text("apic icr 0x%08x/0x%08x composes as "
                               "written",
                               low,
                               high));

                    auto matches_kvm =
                        (g_commands[0] == kvm_command(low, high));

                    if (!matches_kvm) {
                        if (0 != (low & apic_icr_busy)) {
                            ++busy_mismatches;
                        } else {
                            ++other_mismatches;
                        }
                        continue;
                    }

                    check(matches_kvm,
                          text("apic icr 0x%08x/0x%08x matches "
                               "kvm_apic_send_ipi",
                               low,
                               high));
                }
            }
        }

        // The destination is the only other thing that could differ, and
        // it does not: KVM's GET_XAPIC_DEST_FIELD masks the shifted dword
        // to eight bits and a `u32 >> 24` is already eight bits wide, so
        // every high half above - including 0xffffffff - composes
        // identically.
        check(0 == other_mismatches,
              "the destination composes exactly as kvm_apic_send_ipi's "
              "GET_XAPIC_DEST_FIELD does, for every value tried");

        diverge(10 == busy_mismatches,
                text("%d of the composed commands keep the "
                     "delivery-status bit that KVM clears before "
                     "composing (lapic.c:2326, `val &= ~APIC_ICR_BUSY`); "
                     "KVM's IPI decoder then asserts it is never set "
                     "(lapic.c:1520). The SDM makes bit 12 read only - "
                     "\"Delivery Status (Read Only)\", sdm.txt:170910 - "
                     "so a guest cannot mean anything by it. Harmless "
                     "today, because on_interrupt_command reads only the "
                     "vector, the delivery mode and the shorthand. "
                     "Smallest fix: mask 1u<<12 out of write->value at "
                     "hypervisor.cpp:5813",
                     busy_mismatches));
    }

    // The filter's two answers: a swallowed command suppresses the
    // guest's write; anything else lets it through unchanged.
    {
        reset();
        put32(interrupt_command_high, 0x03000000);
        g_command_swallows = true;

        auto answer = filter(interrupt_command_low, 0x000c4600, 4);

        check(!answer.has_value(),
              "a swallowed command suppresses the guest's write");
        check(1 == hv().apic_page_commands_filtered,
              "and is counted as filtered");
    }

    {
        reset();
        put32(interrupt_command_high, 0x03000000);
        g_command_rewrites = true;
        g_command_rewrite_to = 0x000c4500;

        auto answer = filter(interrupt_command_low, 0x000c4600, 4);

        diverge(answer.has_value() && (0x000c4600 == *answer),
                "filter_local_apic_write returns write->value whatever "
                "on_interrupt_command answers, so a handler that "
                "*modifies* a command is silently ignored on the "
                "emulated path - only a swallow is honoured. "
                "on_local_apic_write, the stepped path, does honour it "
                "(hypervisor.cpp:5942-5953). Unreachable today because "
                "on_interrupt_command returns its argument on every "
                "path. Smallest fix: return the handler's value at "
                "hypervisor.cpp:5826");
    }

    // A null write suppresses, which is the safe direction but is the
    // opposite of what on_local_apic_write does with one.
    {
        reset();
        check(!hypervisor_t::filter_local_apic_write(
                   &hv(), watched_page(), nullptr)
                   .has_value(),
              "a null write suppresses in the filter");
        check(0 == g_commands.size(), "and reaches no command handler");
    }

    // on_local_apic_write, the stepped path. It reads the command out of
    // the page rather than out of the write, because by the time it runs
    // the guest's own instruction has retired.
    {
        reset();
        put32(interrupt_command_low, 0x000c4600);
        put32(interrupt_command_high, 0x07000000);

        guest_write write{
            .address = base() + interrupt_command_low,
            .value = 0xdeadbeef, // deliberately not what the page holds
            .size = 4,
        };

        hypervisor_t::on_local_apic_write(&hv(), watched_page(), &write);

        check(1 == g_commands.size(), "the stepped path acts on 0x300");
        if (1 == g_commands.size()) {
            check(0x0000'0007'000c'4600ull == g_commands[0],
                  "and composes from the page, not from the write");
        }
    }

    {
        reset();
        for (std::uint64_t offset = 0; offset < 0x1000; offset += 0x10) {
            if (interrupt_command_low == offset) {
                continue;
            }

            g_commands.clear();
            put32(interrupt_command_low, 0x000c4600);

            guest_write write{
                .address = base() + offset,
                .value = 0x1234,
                .size = 4,
            };

            hypervisor_t::on_local_apic_write(
                &hv(), watched_page(), &write);

            check(0 == g_commands.size(),
                  text("the stepped path ignores apic offset 0x%llx",
                       static_cast<unsigned long long>(offset)));
        }
    }

    {
        reset();
        hypervisor_t::on_local_apic_write(&hv(), watched_page(), nullptr);
        check(1 == hv().apic_writes_undecoded,
              "a null write is counted rather than guessed at");
        check(0 == g_commands.size(), "and acts on nothing");
    }

    // A rewritten command is put back, high half first because writing
    // the low half is what sends it. hypervisor.cpp:5946-5953.
    {
        reset();
        put32(interrupt_command_low, 0x000c4600);
        put32(interrupt_command_high, 0x07000000);
        g_command_rewrites = true;
        g_command_rewrite_to = 0x0000'0002'000c'4500ull;

        guest_write write{
            .address = base() + interrupt_command_low,
            .value = 0x000c4600,
            .size = 4,
        };

        hypervisor_t::on_local_apic_write(&hv(), watched_page(), &write);

        check(0x000c4500 == at32(interrupt_command_low),
              "a rewritten command replaces the low half");
        check(0x02000000 == at32(interrupt_command_high),
              "and the destination, in bits 31:24");
    }

    {
        reset();
        put32(interrupt_command_low, 0x000c4600);
        put32(interrupt_command_high, 0x07000000);

        guest_write write{
            .address = base() + interrupt_command_low,
            .value = 0x000c4600,
            .size = 4,
        };

        hypervisor_t::on_local_apic_write(&hv(), watched_page(), &write);

        check(0x000c4600 == at32(interrupt_command_low),
              "an unchanged command is not written back, which would "
              "send it a second time");
        check(0x07000000 == at32(interrupt_command_high),
              "nor is its destination");
    }

    // The two paths compose the same command from the same state, which
    // is what makes one on_interrupt_command serve both.
    {
        for (auto low : {0x000c4500u, 0x000c4600u, 0x000000ffu}) {
            for (auto high : {0x00000000u, 0x05000000u, 0xff000000u}) {
                reset();
                put32(interrupt_command_high, high);
                put32(interrupt_command_low, low);

                guest_write write{
                    .address = base() + interrupt_command_low,
                    .value = low,
                    .size = 4,
                };

                hypervisor_t::on_local_apic_write(
                    &hv(), watched_page(), &write);
                auto stepped = g_commands;

                g_commands.clear();
                hypervisor_t::filter_local_apic_write(
                    &hv(), watched_page(), &write);
                auto emulated = g_commands;

                check((1 == stepped.size()) && (1 == emulated.size()) &&
                          (stepped[0] == emulated[0]),
                      text("the stepped and emulated paths compose "
                           "0x%08x/0x%08x identically",
                           low,
                           high));
            }
        }
    }
}

// ================================================== straddling and width
//
// SDM 13.4 (.references/sdm.txt:170419, the paragraph beginning "Table
// 13-1 shows how the APIC registers are mapped"): "All 32-bit registers
// should be accessed using 128-bit aligned 32-bit loads or stores... Any
// FP/MMX/SSE access to an APIC register, or any access that touches bytes
// 4 through 15 of an APIC register may cause undefined behavior".
//
// KVM enforces exactly that and drops anything else on the floor:
// apic_mmio_write, .references/kvm/lapic.c:2440,
//   `if (len != 4 || (offset & 0xf)) return 0;`
static void test_straddle_and_width()
{
    std::println("\nstraddling and access width");

    // A store crossing the end of the watched page is refused and
    // stepped, rather than applied whole at the faulting address.
    // hypervisor.cpp:3465-3471.
    struct
    {
        std::uint64_t offset;
        std::uint8_t size;
        void (*encode)();
        bool straddles;
    } cases[] = {
        {0xffc, 4, mov_mem_reg32, false},
        {0xffd, 4, mov_mem_reg32, true},
        {0xffe, 4, mov_mem_reg32, true},
        {0xfff, 4, mov_mem_reg32, true},
        {0xfff, 1, mov_mem_reg8, false},
        {0xffe, 2, mov_mem_reg16, false},
        {0xfff, 2, mov_mem_reg16, true},
        {0xff8, 8, mov_mem_reg64, false},
        {0xff9, 8, mov_mem_reg64, true},
    };

    for (auto & one : cases) {
        reset();
        arm(notify_handler, filter_handler);
        one.encode();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + one.offset;
        registers.rax = 0xffffffffffffffffull;

        std::memset(g_memory + 0x1000, 0xcc, 16);

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = one.offset,
               .physical = base()},
              registers);

        check(one.straddles == (1 == hv().stepped_writes),
              text("a %u-byte access at 0x%llx straddles: %d",
                   one.size,
                   static_cast<unsigned long long>(one.offset),
                   one.straddles ? 1 : 0));
        check(one.straddles == (0 == g_filtered.size()),
              text("a straddling access does not reach the filter "
                   "(0x%llx, %u bytes)",
                   static_cast<unsigned long long>(one.offset),
                   one.size));
        check(0xcc == g_memory[0x1000],
              text("and never spills onto the following page (0x%llx, %u "
                   "bytes)",
                   static_cast<unsigned long long>(one.offset),
                   one.size));
        check(one.straddles == g_monitor_trap_armed,
              text("a straddling access arms the trap flag (0x%llx, %u "
                   "bytes)",
                   static_cast<unsigned long long>(one.offset),
                   one.size));
    }

    // The step's own read-back is four bytes from the recorded offset,
    // unconditionally - hypervisor.cpp:3786-3792 - so for the straddling
    // offsets it just refused, it reads across the page boundary it was
    // protecting.
    {
        reset();
        arm(notify_handler, nullptr);
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0xffe;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0xffe,
               .physical = base()},
              registers);

        check(1 == hv().stepped_writes, "the straddling store is stepped");

        std::memset(g_memory + 0xffe, 0x11, 2);
        std::memset(g_memory + 0x1000, 0x22, 2);
        hv().on_monitor_trap_flag(0, retire_stepped_instruction());

        check(g_notified.empty(),
              "a step resolved to the last bytes of the page notifies "
              "nothing, rather than reading past the end of the very "
              "page the emulation refused to straddle");
    }

    // Now the widths and alignments, against a real filter and against
    // KVM. This VMM has no width or alignment gate anywhere on the path -
    // not in on_ept_violation, not in filter_local_apic_write - so an
    // access KVM drops is emulated here and, at 0x300, sent.
    {
        struct
        {
            std::uint64_t offset;
            std::uint8_t size;
            void (*encode)();
            const char * how;
        } accesses[] = {
            {0x300, 1, mov_mem_reg8, "a byte write to the ICR"},
            {0x300, 2, mov_mem_reg16, "a word write to the ICR"},
            {0x300, 8, mov_mem_reg64, "a quadword write to the ICR"},
            {0x304, 4, mov_mem_reg32, "a dword write four into the ICR"},
            {0x302, 4, mov_mem_reg32, "a misaligned dword over the ICR"},
            {0x2fe, 4, mov_mem_reg32, "a dword ending inside the ICR"},
        };

        for (auto & one : accesses) {
            reset();
            arm(&hypervisor_t::on_local_apic_write,
                &hypervisor_t::filter_local_apic_write);
            put32(interrupt_command_high, 0x03000000);
            one.encode();

            zpp::arch::x86_64::context registers{};
            registers.rbx = base() + one.offset;
            registers.rax = 0x000c4600000c4600ull;

            fault({.qualification =
                       linear_valid | operand_access | data_write,
                   .linear = one.offset,
                   .physical = base()},
                  registers);

            auto sent = g_commands.size();
            [[maybe_unused]] auto kvm_would_take =
                (4 == one.size) && (0 == (one.offset & 0xf));

            if (0x300 == one.offset) {
                check(0 == sent,
                      text("%s (%u bytes at 0x%llx) is not read as a "
                           "command - SDM 13.4 (sdm.txt:170419) requires "
                           "128-bit aligned 32-bit accesses and KVM's "
                           "apic_mmio_write drops anything else "
                           "(lapic.c:2440)",
                           one.how,
                           one.size,
                           static_cast<unsigned long long>(one.offset)));
            } else {
                check(0 == sent,
                      text("%s (%u bytes at 0x%llx) is applied to the "
                           "page but never read as a command, since it "
                           "is not a 16-byte aligned dword",
                           one.how,
                           one.size,
                           static_cast<unsigned long long>(one.offset)));
            }
        }
    }

    // The sharpest of them, spelled out on its own: a 4-byte store at
    // 0x2fe lands two of its bytes in the interrupt command register and
    // the filter is asked about offset 0x2fe, which it passes straight
    // through.
    {
        reset();
        arm(&hypervisor_t::on_local_apic_write,
            &hypervisor_t::filter_local_apic_write);
        put32(0x2fc, 0);
        put32(0x300, 0);
        mov_mem_reg32();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x2fe;
        registers.rax = 0x4600ffff;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x2fe,
               .physical = base()},
              registers);

        check(0 == g_commands.size(),
              "a dword at 0x2fe reaches no command handler");
        check(0x00004600 == at32(0x300),
              "yet two of its bytes land in the interrupt command "
              "register");
    }

    // And the mirror image, which is the one that actually sends: a
    // 1-byte write at 0x300 composes a command out of one byte.
    {
        reset();
        arm(&hypervisor_t::on_local_apic_write,
            &hypervisor_t::filter_local_apic_write);
        put32(interrupt_command_high, 0x02000000);
        put32(interrupt_command_low, 0x000c4600);
        mov_mem_reg8();

        zpp::arch::x86_64::context registers{};
        registers.rbx = base() + 0x300;
        registers.rax = 0x11;

        fault({.qualification = linear_valid | operand_access | data_write,
               .linear = 0x300,
               .physical = base()},
              registers);

        check(g_commands.empty(),
              "a byte write at 0x300 no longer reaches "
              "on_interrupt_command, so it cannot send a command the "
              "guest never wrote");
    }
}

// ---------------------------------------------------------------- main
/**
 * The local APIC timer, as `on_local_apic_write` records it.
 *
 * This is the register a guest hypervisor's own clock is built on, and
 * the records taken here are what a stopped machine is read through - so
 * the questions are not "did it store the value" but "did it store the
 * value *with the things that make it mean something*, and did it keep
 * the right ones".
 *
 * Three registers, and none of them says anything alone. SDM 13.5.4,
 * "APIC Timer": 0x320 is the LVT timer entry, carrying the mode - one
 * shot, periodic or TSC deadline - and the vector; 0x3e0 is the divide
 * configuration, the divisor the count is scaled by; 0x380 is the
 * initial count. A ring of bare counts recorded across a mode change is
 * two different quantities in one column, which is the mistake this
 * recording is shaped to avoid.
 */
/**
 * Dropping the local APIC page watch on a proof rather than on a guess.
 *
 * `on_ept_violation` opens by asking `every_platform_processor_adopted`,
 * and drops the watch when the answer is yes - unconditionally, not
 * behind `ZPP_DISARM_APIC_WATCH`, because a proof is not a heuristic.
 * The real predicate is `tests/local_apic`'s subject; here it is a knob,
 * and what is under test is the three things `on_ept_violation` has to
 * get right around it.
 *
 * The third is the one that killed three boots before it was fixed and
 * is the reason this is a test rather than a reading of the source: the
 * disarm lands on a fault it has just made unhandleable, because the
 * watch it clears is the only one for the page that faulted. Falling out
 * of the block into the loop answers `false`, and both callers stop the
 * processor for a `false`. It has to `return true` and let the guest
 * re-execute against an entry that now permits the write.
 */
static void test_roster_drops_the_watch()
{
    std::println("\n-- the watch is dropped once the roster is adopted");

    zpp::arch::x86_64::context registers{};
    registers.rbx = base() + 0x300;
    registers.rax = 0x000c4500;

    // Not adopted: nothing changes, and this is the negative control for
    // the whole section. Reverting the disarm leaves every check below
    // passing and only these failing, so they are what say the drop is
    // conditional at all.
    {
        reset();
        mov_mem_reg32();
        arm(notify_handler, nullptr);
        hv().watched_apic_page = base();

        auto handled = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = base() + 0x300,
             .physical = base(),
             .reported_length = 2},
            registers);

        check(handled, "an unadopted roster still handles the violation");
        check(base() == hv().watched_apic_page,
              "and leaves the watch armed");
        check(!hv().all_processors_started.load(),
              "and does not claim every processor has started");
        check(1 == g_notified.size(),
              "the write is emulated, which is what the watch is for");
    }

    // Adopted: the watch goes, the fault is answered, and the guest is
    // resumed to re-execute its own instruction.
    {
        reset();
        mov_mem_reg32();
        arm(notify_handler, nullptr);
        hv().watched_apic_page = base();
        g_roster_fully_adopted = true;

        auto handled = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = base() + 0x300,
             .physical = base(),
             .reported_length = 2},
            registers);

        check(handled,
              "the fault the disarm lands on is answered rather than "
              "fallen out of - `false` here stops the processor");
        check(
            hv().all_processors_started.load(),
            "an adopted roster settles that every processor has started");
        check(0 == hv().watched_apic_page, "and the watch is dropped");
        check(g_notified.empty(),
              "nothing is emulated on the way out - the guest re-executes "
              "its own write against an entry that now permits it");

        // And it cannot loop, which is the other half of `return true`
        // being safe. `watched_apic_page` is zero now, so the block is
        // not reached a second time and the page carries no watch.
        auto again = fault(
            {.qualification = linear_valid | operand_access | data_write,
             .linear = base() + 0x300,
             .physical = base(),
             .reported_length = 2},
            registers);

        check(!again,
              "a second fault on the same page finds nothing watching it, "
              "which is the unclaimed case the caller counts and resumes");
    }
}

static void test_apic_timer()
{
    std::println("\n-- the local apic timer, and what a count means");

    constexpr std::uint64_t lvt_timer = 0x320;
    constexpr std::uint64_t divide_configuration = 0x3e0;
    constexpr std::uint64_t timer_initial_count = 0x380;

    auto write_register = [&](std::uint64_t offset, std::uint64_t value) {
        guest_write write{
            .address = base() + offset,
            .value = value,
            .size = 4,
        };
        // `filter_local_apic_write`, not `on_local_apic_write`. The
        // timer records are taken on the *filter* path, which sees every
        // register on the page - the notify path returns early for any
        // offset that is not the interrupt command register's low half,
        // so it never sees a timer write at all. Getting that wrong is
        // how this section was first written, and every case failed with
        // nothing recorded.
        static_cast<void>(hypervisor_t::filter_local_apic_write(
            &hv(), watched_page(), &write));
    };

    // Which processor the writes below come from.
    //
    // Two mechanisms, set together, because the filter changed which of
    // them it asks and the two must not be allowed to drift apart. The
    // VPID is what `setup_vmcs` writes and what everything used to read;
    // GS is what `hypervisor::this_processor()` reads now, and on the
    // target the two agree because `setup_vmcs` writes the processor's
    // own `gs_data` row on the line beside `vpid(cpu + 1)`.
    //
    // The per-processor cases below are the whole reason the filter's
    // move to GS is testable at all: with one mechanism set and the other
    // stale, "each processor recorded its own arming" fails - which is
    // exactly the silent misattribution the change risks.
    auto on_processor = [](std::size_t cpu) {
        hv().vmcs.vpid(cpu + 1);
        zpp::arch::x86_64::g_gs_qword = cpu;
    };

    // The mode and the divisor are latched as they are written, so that
    // an arming can be recorded with the pair that was in force for it.
    {
        reset();
        on_processor(0);
        write_register(lvt_timer, 0x20005);
        write_register(divide_configuration, 0xb);

        check(0x20005 == hv().timer_lvt[0],
              "the LVT timer entry is latched - it carries the mode and "
              "the vector, without which a count is a bare number");
        check(0xb == hv().timer_divide[0],
              "and the divide configuration, which is what the count is "
              "scaled by");
    }

    // An arming records the count, a time stamp, and **the mode and
    // divisor as they stood at that moment**. Recording the current ones
    // instead would be correct until the guest changed mode, which the
    // rig measured it doing: periodic at vector 5 while bringing
    // processors up, then one-shot at vector 0xef.
    {
        reset();
        on_processor(0);
        write_register(lvt_timer, 0x20005);
        write_register(divide_configuration, 0xb);
        write_register(timer_initial_count, 0x1000);

        // Now change the mode, the way the guest does after start-up,
        // and arm again.
        write_register(lvt_timer, 0x000ef);
        write_register(divide_configuration, 0);
        write_register(timer_initial_count, 0x2000);

        check(2 == hv().timer_arm_recent_count[0],
              "two armings were recorded");
        check(0x1000 == hv().timer_arm_recent_value[0][0] &&
                  0x2000 == hv().timer_arm_recent_value[0][1],
              "with their own counts");
        check(0x20005 == hv().timer_arm_recent_lvt[0][0],
              "the first arming kept the periodic mode it was made in, "
              "not the one-shot mode set afterwards - a ring that "
              "recorded the current LVT would relabel every earlier "
              "arming the moment the guest switched");
        check(0x000ef == hv().timer_arm_recent_lvt[0][1],
              "and the second kept its own");
        check(0xb == hv().timer_arm_recent_divide[0][0] &&
                  0 == hv().timer_arm_recent_divide[0][1],
              "the divisors likewise travel with their arming");
    }

    // The two records answer different questions and must not be one
    // record. `timer_arm_*` keeps the **earliest** armings, because the
    // calibration is the first thing a guest does with this register and
    // a ring would throw it away. `timer_arm_recent_*` keeps the newest,
    // because a machine that stopped stopped at the end.
    {
        reset();
        on_processor(0);
        constexpr std::size_t capacity = hypervisor_t::timer_arm_capacity;

        for (std::size_t i{}; i < (capacity * 2); ++i) {
            write_register(timer_initial_count, 0x100 + i);
        }

        check(capacity == hv().timer_arm_count[0],
              "the first-armings array stops at capacity rather than "
              "wrapping");
        check(0x100 == hv().timer_arm_value[0][0],
              "and its first slot still holds the *first* arming of the "
              "boot - the one that decided the calibration, which a ring "
              "would have evicted long before anything went wrong");
        check((0x100 + capacity - 1) ==
                  hv().timer_arm_value[0][capacity - 1],
              "and its last slot holds the last arming it had room for");

        check((capacity * 2) == hv().timer_arm_recent_count[0],
              "the recent ring counts every arming, not only the ones it "
              "kept - the count is how a reader knows how far behind the "
              "ring is");
        check((0x100 + capacity) == hv().timer_arm_recent_value[0][0],
              "and it wrapped, so slot 0 now holds an arming from the "
              "second lap");
        check((0x100 + (capacity * 2) - 1) ==
                  hv().timer_arm_recent_value[0][(capacity * 2 - 1) %
                                                 capacity],
              "with the newest arming at (count - 1) mod capacity, which "
              "is where a reader has to look");
    }

    // A time stamp per arming, because the interval between two of them
    // is the whole measurement: it separates a guest that measured a
    // true interval and scaled it wrongly from one that measured an
    // interval this VMM had already stretched.
    {
        reset();
        on_processor(0);
        write_register(timer_initial_count, 0x1000);
        write_register(timer_initial_count, 0x2000);

        check(hv().timer_arm_tsc[0][0] != 0,
              "an arming carries a time stamp");
        check(hv().timer_arm_tsc[0][1] >= hv().timer_arm_tsc[0][0],
              "and the stamps do not go backwards, so an interval "
              "between two armings can be taken");
    }

    // The width and alignment gate applies to the timer registers too.
    //
    // SDM 13.4.1 makes a local APIC register a 4-byte access on a
    // 16-byte boundary, and `filter_local_apic_write` refuses to read
    // anything else as a register access. A narrow store composed into
    // an arming would put a fragment of a count into the record and the
    // reader would take it for a deadline.
    {
        reset();
        on_processor(0);
        for (std::uint8_t size : {1, 2, 8}) {
            guest_write write{
                .address = base() + timer_initial_count,
                .value = 0xdead,
                .size = size,
            };
            static_cast<void>(hypervisor_t::filter_local_apic_write(
                &hv(), watched_page(), &write));
        }

        check(0 == hv().timer_arm_count[0],
              "a store that is not four bytes wide is not recorded as an "
              "arming - SDM 13.4.1 makes it not a register access at all");
    }

    // And the records are per processor. A guest hypervisor arms one
    // timer per virtual processor it brings up, so a shared record would
    // interleave several clocks into one column and read as a single
    // clock behaving impossibly.
    {
        reset();
        on_processor(0);
        write_register(timer_initial_count, 0x1111);
        on_processor(1);
        write_register(timer_initial_count, 0x2222);
        on_processor(0);

        check(1 == hv().timer_arm_count[0] && 1 == hv().timer_arm_count[1],
              "each processor recorded its own arming");
        check(0x1111 == hv().timer_arm_value[0][0] &&
                  0x2222 == hv().timer_arm_value[1][0],
              "and kept its own count");
    }
}

/**
 * A watch may not be armed on an address past what `epd` describes.
 *
 * `epte_for` is indexed rather than walked - `epd[address >> 30]` into an
 * array of 512 rows - so an address at or above 512 GB reads a
 * page-directory entry that is some other member of the singleton, and
 * then this function *writes* through the returned pointer to clear write
 * permission. That is an out-of-bounds store, not a wrong answer.
 *
 * It is reachable from addresses the guest chooses. The IUM block watch
 * at `nested_entry.cpp:7837` takes a hypercall's RDX, walks the guest's
 * own page tables with `translate_guest_linear` and then the guest
 * hypervisor's extended page tables with `l2_physical_to_l1`, and every
 * frame number in both walks comes out of a table the guest owns - so a
 * 52-bit frame from either indexed `epd` thousands of rows past its end.
 *
 * KVM refuses the same class one level up rather than at the table:
 * `kvm_vcpu_is_illegal_gpa` rejects a guest-physical address above the
 * reported width before anything indexes on it. There is no equivalent
 * here because the bound is this VMM's own array rather than MAXPHYADDR,
 * and the array is the smaller of the two.
 *
 * The negative control is the point of the first two checks: the same
 * call one page *below* the limit must still succeed, or this would pass
 * for a `watch_guest_page_writes` that refuses everything.
 */
static void test_epte_bound()
{
    reset();

    check(hypervisor_t::physical_address_within_ept(
              hypervisor_t::ept_identity_limit - hypervisor_t::page_size),
          "the last page below the limit is nameable");
    check(!hypervisor_t::physical_address_within_ept(
              hypervisor_t::ept_identity_limit),
          "the first address at the limit is not nameable");
    check(!hypervisor_t::physical_address_within_ept(
              (1ull << 52) - hypervisor_t::page_size),
          "a 52-bit frame is not nameable");

    // Through the real `watch_guest_page_writes`, so the refusal is the
    // one a caller actually gets rather than the predicate on its own.
    auto beyond =
        hv().watch_guest_page_writes(hypervisor_t::ept_identity_limit,
                                     &hypervisor_t::on_local_apic_write,
                                     &hv(),
                                     page_watch::mode::notify);

    check(!beyond.has_value(), "arming a watch at the limit is refused");
    check(!beyond.has_value() &&
              (hypervisor_t::error::physical_address_beyond_ept ==
               static_cast<hypervisor_t::error>(beyond.error().code())),
          "and refused with physical_address_beyond_ept");

    auto far = hv().watch_guest_page_writes(
        (1ull << 52) - hypervisor_t::page_size,
        &hypervisor_t::on_local_apic_write,
        &hv(),
        page_watch::mode::notify);

    check(!far.has_value(), "so is one at a 52-bit frame");

    // The negative control. One page below the limit is an address
    // `epte_for` may index, so the arming must go through - otherwise
    // the two refusals above prove nothing about the bound.
    auto within = hv().watch_guest_page_writes(
        hypervisor_t::ept_identity_limit - hypervisor_t::page_size,
        &hypervisor_t::on_local_apic_write,
        &hv(),
        page_watch::mode::notify);

    check(within.has_value(),
          "a watch one page below the limit is still armed");

    reset();
}

/**
 * `watch_local_apic` must refuse to arm on an APIC that has moved, and
 * the mask it compares through is what decides whether it can tell.
 *
 * The refusal exists because `filter_local_apic_write` and
 * `on_local_apic_write` reach the page by dereferencing `page << 12` as
 * a **host virtual address**, and the host page table maps exactly one
 * local APIC page - the one read from IA32_APIC_BASE before any guest
 * ran. Arming on any other page is either a #PF in the exit handler or,
 * where the old page is still mapped, a watch on a page the APIC no
 * longer decodes - so every interrupt-command write goes unintercepted
 * and no start-up IPI is ever seen.
 *
 * The field is IA32_APIC_BASE[MAXAPICADDR-1:12] - SDM 13.4.4
 * ([[PAGE 3577]]), with MAXAPICADDR "normally CPUID.80000008H:EAX[7:0]
 * ... and 36 otherwise". Four hard-coded `0xffffff000` masks applied
 * the 36-bit fallback unconditionally, so on a processor reporting more
 * than 36 bits **both sides of the comparison were truncated** and a
 * relocation that moved only the bits above 36 compared equal. This is
 * that case, and it is reachable by a guest with one WRMSR - nothing in
 * this VMM validates the value it forwards to hardware.
 */
static void test_relocated_apic_is_refused()
{
    reset();

    constexpr std::uint64_t mapped = 0xfee00000;
    constexpr std::uint64_t enabled = 0x800;

    g_physical_address_bits = 39;
    hv().mapped_apic_page = mapped;

    // The negative control first, and it has to be first: if arming at
    // the mapped page did not work, the refusals below would prove
    // nothing about the mask.
    g_apic_base_msr = mapped | enabled;
    hv().watch_local_apic(true);
    check(mapped == hv().watched_apic_page,
          "the APIC page this VMM maps is armed - the control, without "
          "which a refusal proves nothing");

    // Relocated by one bit *above* the 36-bit fallback, and nothing
    // else changed. Truncated to 36 bits this is byte-for-byte the
    // control above, which is exactly why it used to be accepted.
    g_apic_base_msr = (1ull << 36) | mapped | enabled;
    hv().watch_local_apic(true);
    check(0 == hv().watched_apic_page,
          "an APIC relocated above the 36-bit fallback is refused - the "
          "bits that moved are inside IA32_APIC_BASE's field on any "
          "processor reporting more than 36 physical address bits");

    // And the same value on a processor that really does report 36,
    // where those bits are reserved and hardware would have faulted the
    // write. The mask then genuinely is `0xffffff000`, the base reads
    // back as the mapped page, and arming is correct.
    g_physical_address_bits = 36;
    hv().watch_local_apic(true);
    check(mapped == hv().watched_apic_page,
          "on a processor reporting 36 bits the same MSR value names "
          "the mapped page, because bit 36 is reserved there - so the "
          "mask follows MAXAPICADDR rather than refusing everything");

    g_physical_address_bits = 39;
    g_apic_base_msr = 0;
    hv().watch_local_apic(false);
    hv().mapped_apic_page = 0;
    reset();
}

int main()
{
    // The real host page table, filled with an identity mapping over the
    // guest's pages and over the hypervisor object - the decoy page an
    // EPT violation redirects to is a member of it.
    //
    // The two reads and the two writes `carry_out_guest_instruction`
    // makes are gated on this translation answering: a page the host
    // table does not map is refused, which is what stops this VMM
    // dereferencing an address the guest chose. Against the
    // `page_table_stub` this harness used to carry - a nested class of a
    // copy of the hypervisor, whose translation returned its argument -
    // that gate was answered by the test rather than by the code.
    zpp::tests::map_identity(
        hv().host_page_table, g_memory, sizeof(g_memory));
    zpp::tests::map_identity(hv().host_page_table, &hv(), sizeof(hv()));

    check(base() == hv().host_page_table.virtual_to_physical(g_memory),
          "the host page table translates the guest's page to itself");

    test_offset_resolution();
    test_carry_out();
    test_filter_notify();
    test_local_apic();
    test_straddle_and_width();
    test_roster_drops_the_watch();
    test_apic_timer();
    test_epte_bound();
    test_relocated_apic_is_refused();

    std::println("\n{} checks, {} failures", g_checks, g_failures);

    if (!g_findings.empty()) {
        std::println("\nfindings ({}):", g_findings.size());
        for (auto & finding : g_findings) {
            std::println("  - {}", finding);
        }
    }

    return (0 == g_failures) ? 0 : 1;
}
