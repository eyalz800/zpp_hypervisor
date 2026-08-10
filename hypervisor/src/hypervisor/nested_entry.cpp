#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/memory_type.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/arch/x86_64/vmx/ept_pointer.h"
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include "zpp/hypervisor/hypervisor.h"
#include "zpp/hypervisor/nested_vmx.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>

namespace zpp::hypervisor
{
namespace
{
using arch::x86_64::vmx::vmcs12;
using arch::x86_64::vmx::vmcs_field_encoding;
using basic_reason = arch::x86_64::vmx::exit_reason::basic_reason;
using field = arch::x86_64::vmx::vmcs::field;

/**
 * The recovery-context field the entry stubs read is CR3-target value 0.
 * asm.h spells the encoding literally, because inline assembly cannot see
 * a constant expression; this is what keeps the two spellings honest.
 */
static_assert(arch::x86_64::vmx::nested_entry_recovery_field ==
                  field::cr3_target_value_0,
              "The nested entry stubs and the VMCS field disagree.");

/**
 * The two fields the TPR shadow is carried in, checked against what a
 * shadow VMCS has storage for.
 *
 * Offering primary bit 21 is a promise a guest hypervisor can *write*
 * these, and `vmcs_field_encoding::valid` is what decides that - a field
 * whose index is past `index_capacity` is answered with error 12 and the
 * control becomes unusable. Both are 64-bit and 32-bit control fields at
 * indices 9 and 14, comfortably inside it, but the pairing is checked here
 * rather than worked out by hand again: advertising a control whose fields
 * cannot be written would be the same half-answered interface withholding
 * it was.
 * @{
 */
static_assert(vmcs_field_encoding(
                  static_cast<std::uint64_t>(field::virtual_apic_address))
                  .valid(),
              "A guest hypervisor could not write the virtual-APIC "
              "address the TPR shadow needs.");

static_assert(
    vmcs_field_encoding(static_cast<std::uint64_t>(field::tpr_threshold))
        .valid(),
    "A guest hypervisor could not write the TPR threshold.");
/**
 * @}
 */

/**
 * The pin-based controls this code names, from SDM Table 25-5.
 * @{
 */
constexpr std::uint64_t pin_external_interrupt = 1ull << 0;
constexpr std::uint64_t pin_preemption_timer = 1ull << 6;
constexpr std::uint64_t pin_posted_interrupts = 1ull << 7;
/**
 * @}
 */

/**
 * The primary processor-based controls this code names, from SDM Table
 * 25-6.
 * @{
 */
constexpr std::uint64_t primary_interrupt_window = 1ull << 2;
constexpr std::uint64_t primary_tsc_offsetting = 1ull << 3;
constexpr std::uint64_t primary_hlt_exiting = 1ull << 7;
constexpr std::uint64_t primary_invlpg_exiting = 1ull << 9;
constexpr std::uint64_t primary_mwait_exiting = 1ull << 10;
constexpr std::uint64_t primary_rdpmc_exiting = 1ull << 11;
constexpr std::uint64_t primary_rdtsc_exiting = 1ull << 12;
constexpr std::uint64_t primary_cr3_load_exiting = 1ull << 15;
constexpr std::uint64_t primary_cr3_store_exiting = 1ull << 16;
constexpr std::uint64_t primary_cr8_load_exiting = 1ull << 19;
constexpr std::uint64_t primary_cr8_store_exiting = 1ull << 20;
constexpr std::uint64_t primary_tpr_shadow = 1ull << 21;
constexpr std::uint64_t primary_nmi_window = 1ull << 22;
constexpr std::uint64_t primary_mov_dr_exiting = 1ull << 23;
constexpr std::uint64_t primary_unconditional_io = 1ull << 24;
constexpr std::uint64_t primary_io_bitmaps = 1ull << 25;
constexpr std::uint64_t primary_monitor_trap_flag = 1ull << 27;
constexpr std::uint64_t primary_msr_bitmaps = 1ull << 28;
constexpr std::uint64_t primary_monitor_exiting = 1ull << 29;
constexpr std::uint64_t primary_pause_exiting = 1ull << 30;
constexpr std::uint64_t primary_secondary_controls = 1ull << 31;
/**
 * @}
 */

/**
 * The secondary processor-based controls this code names, from SDM Table
 * 25-7.
 * @{
 */
constexpr std::uint64_t secondary_enable_ept = 1ull << 1;
constexpr std::uint64_t secondary_descriptor_table_exiting = 1ull << 2;
constexpr std::uint64_t secondary_enable_vpid = 1ull << 5;
constexpr std::uint64_t secondary_wbinvd_exiting = 1ull << 6;
constexpr std::uint64_t secondary_unrestricted_guest = 1ull << 7;
constexpr std::uint64_t secondary_pause_loop_exiting = 1ull << 10;
constexpr std::uint64_t secondary_rdrand_exiting = 1ull << 11;
constexpr std::uint64_t secondary_enable_invpcid = 1ull << 12;
constexpr std::uint64_t secondary_rdseed_exiting = 1ull << 16;
constexpr std::uint64_t secondary_enable_xsaves = 1ull << 20;
constexpr std::uint64_t secondary_mode_based_execute = 1ull << 22;
/**
 * @}
 */

/**
 * The VM-exit controls this code names, from SDM Table 25-13.
 * @{
 */
constexpr std::uint64_t exit_save_debug_controls = 1ull << 2;
constexpr std::uint64_t exit_host_address_space_size = 1ull << 9;
constexpr std::uint64_t exit_save_ia32_pat = 1ull << 18;
constexpr std::uint64_t exit_load_ia32_pat = 1ull << 19;
constexpr std::uint64_t exit_save_ia32_efer = 1ull << 20;
constexpr std::uint64_t exit_load_ia32_efer = 1ull << 21;
/**
 * @}
 */

/**
 * The VM-entry controls this code names, from SDM Table 25-16.
 * @{
 */
constexpr std::uint64_t entry_ia32e_mode_guest = 1ull << 9;
/**
 * @}
 */

/**
 * CR4.VMXE, which a guest of any level must have set in the real register
 * because IA32_VMX_CR4_FIXED0 requires it in VMX operation, and must not
 * see set unless it put it there itself.
 */
constexpr std::uint64_t cr4_vmxe = 1ull << 13;

/**
 * The valid bit of an interruption-information field, SDM Table 25-19.
 */
constexpr std::uint64_t interruption_valid = 1ull << 31;

/**
 * The interruption type field of one, bits 10:8, and the value that means
 * a non-maskable interrupt. SDM Table 25-19.
 * @{
 */
constexpr std::uint64_t interruption_type_shift = 8;
constexpr std::uint64_t interruption_type_mask = 0x7;
constexpr std::uint64_t interruption_type_nmi = 2;
constexpr std::uint64_t interruption_vector_mask = 0xff;
/**
 * @}
 */

/**
 * One entry of a VM-entry or VM-exit MSR area, SDM 27.7.2, Figure 27-1:
 * bits 31:0 the MSR index, bits 63:32 reserved and required to be zero,
 * bits 127:64 the value.
 */
struct msr_area_entry
{
    std::uint32_t index{};
    std::uint32_t reserved{};
    std::uint64_t value{};
};

static_assert(sizeof(msr_area_entry) == 16);

/**
 * How many entries an MSR area may name.
 *
 * SDM A.6 puts the limit in IA32_VMX_MISC bits 27:25, as "(N+1)*512" - so
 * zero there is 512, which is what `nested_vmx_capability_msr` narrows the
 * field to. Reported rather than assumed, so a guest hypervisor building a
 * longer list is told rather than surprised.
 */
constexpr std::uint64_t msr_area_capacity = 512;

/**
 * Whether an MSR area may name this index at all.
 *
 * The three rules SDM 29.4 and SDM 30.4 share, and KVM's
 * `nested_vmx_msr_check_common` applies:
 *
 * - bits 31:8 equal to 000008H, which is the x2APIC register range. A
 *   local APIC in x2APIC mode is reached through those, and this VMM
 *   intercepts the interrupt command register among them.
 * - the microcode update registers, IA32_BIOS_UPDT_TRIG and
 *   IA32_BIOS_SIGN_ID.
 * - the reserved dword, which must be zero.
 */
constexpr bool msr_area_index_allowed(const msr_area_entry & entry)
{
    constexpr std::uint32_t x2apic_page = 0x8;
    constexpr std::uint32_t bios_update_trigger = 0x79;
    constexpr std::uint32_t bios_signature = 0x8b;

    if (0 != entry.reserved) {
        return false;
    }

    if (x2apic_page == (entry.index >> 8)) {
        return false;
    }

    return (bios_update_trigger != entry.index) &&
           (bios_signature != entry.index);
}

/**
 * Whether this VMM will read or write the named MSR on a guest
 * hypervisor's behalf.
 *
 * **A list rather than a rule, and the direction is the safe one.** SDM
 * 29.4's last failure condition is "an attempt to write bits 127:64 to the
 * MSR indexed by bits 31:0 of the entry would cause a general-protection
 * exception if executed via WRMSR with CPL = 0", and SDM 30.4 says the
 * same of RDMSR. Answering that question in general needs a WRMSR that can
 * fault and recover, and this VMM has no such thing: the host exception
 * recovery point is a single shared context that `main` disarms before the
 * guest ever runs, so a #GP in root operation stops the processor. A guest
 * hypervisor's VM entry is a guest instruction, and no guest instruction
 * may do that.
 *
 * So an index outside this list fails the entry rather than being
 * attempted, which is an outcome the architecture already has a name for -
 * the same failure a processor reports for an MSR it will not load. A
 * guest hypervisor is told; nothing is silently skipped.
 *
 * The list is what a hypervisor actually puts in these areas: the two
 * memory-typing and paging-mode MSRs, the system-call set, the two base
 * registers a context switch needs, the speculation controls, and the time
 * stamp counter.
 *
 * Removing the restriction needs a fault-tolerant WRMSR, which needs a
 * per-processor host exception recovery point. BACKLOG.md records that as
 * the change, since it touches the exception path every processor shares.
 */
constexpr bool msr_area_index_handled(std::uint32_t index)
{
    switch (index) {
    case 0x10:       // IA32_TIME_STAMP_COUNTER.
    case 0x48:       // IA32_SPEC_CTRL.
    case 0x49:       // IA32_PRED_CMD.
    case 0x10b:      // IA32_FLUSH_CMD.
    case 0x174:      // IA32_SYSENTER_CS.
    case 0x175:      // IA32_SYSENTER_ESP.
    case 0x176:      // IA32_SYSENTER_EIP.
    case 0x1d9:      // IA32_DEBUGCTL.
    case 0x277:      // IA32_PAT.
    case 0xd90:      // IA32_BNDCFGS.
    case 0xda0:      // IA32_XSS.
    case 0xc0000080: // IA32_EFER.
    case 0xc0000081: // IA32_STAR.
    case 0xc0000082: // IA32_LSTAR.
    case 0xc0000083: // IA32_CSTAR.
    case 0xc0000084: // IA32_FMASK.
    case 0xc0000102: // IA32_KERNEL_GS_BASE.
    case 0xc0000103: // IA32_TSC_AUX.
        return true;
    default:
        return false;
    }
}

/**
 * Whether writing this value to this MSR would fault, for the two in the
 * list above whose *value* can fault rather than only their index.
 *
 * IA32_EFER is the one the SDM calls out itself, in the footnote to SDM
 * 29.4: "If CR0.PG = 1, WRMSR to the IA32_EFER MSR causes a
 * general-protection exception if it would modify the LME bit". Root
 * operation always has CR0.PG = 1 here, so a guest hypervisor asking to
 * change LME is asking for a fault. Its reserved bits fault too - SCE,
 * LME, LMA and NXE are the whole of it.
 *
 * IA32_PAT faults on a byte that is not one of the six defined memory
 * types, SDM Table 13-11: 0, 1, 4, 5, 6 and 7, with 2 and 3 reserved.
 */
inline bool msr_area_value_writable(std::uint32_t index,
                                    std::uint64_t value)
{
    constexpr std::uint32_t ia32_efer = 0xc0000080;
    constexpr std::uint32_t ia32_pat = 0x277;

    if (ia32_efer == index) {
        constexpr std::uint64_t efer_defined =
            (1ull << 0) | (1ull << 8) | (1ull << 10) | (1ull << 11);
        constexpr std::uint64_t efer_lme = 1ull << 8;

        if (0 != (value & ~efer_defined)) {
            return false;
        }

        auto current = arch::x86_64::rdmsr(
            arch::x86_64::msr::ia32_extended_feature_enable);

        return (value & efer_lme) == (current & efer_lme);
    }

    if (ia32_pat == index) {
        for (auto byte = 0; byte < 8; ++byte) {
            auto type = (value >> (byte * 8)) & 0xff;

            if ((2 == type) || (3 == type) || (type > 7)) {
                return false;
            }
        }
    }

    return true;
}

/**
 * The host-state area, which vmcs02 takes unchanged from the VMCS that
 * runs the guest hypervisor.
 *
 * It is this VMM's rather than the guest hypervisor's because the VM exit
 * a second-level guest takes comes *here*: the processor knows one host
 * and it is this one. The guest hypervisor's own host state is loaded into
 * the guest-state area of vmcs01 instead, and only when an exit is
 * actually reflected to it. KVM says the same of the exit controls in
 * `prepare_vmcs02_early`: "L2->L1 exit controls are emulated - the
 * hardware exit is to L0 so we should use its exit controls".
 *
 * IA32_PAT, IA32_EFER and IA32_PERF_GLOBAL_CTRL are absent because the
 * exit controls copied alongside do not load them, so the processor never
 * reads those fields. They are host state that does not apply.
 */
constexpr field host_state_fields[] = {
    field::host_es_selector,
    field::host_cs_selector,
    field::host_ss_selector,
    field::host_ds_selector,
    field::host_fs_selector,
    field::host_gs_selector,
    field::host_tr_selector,
    field::host_ia32_sysenter_cs,
    field::host_cr0,
    field::host_cr3,
    field::host_cr4,
    field::host_fs_base,
    field::host_gs_base,
    field::host_tr_base,
    field::host_gdtr_base,
    field::host_idtr_base,
    field::host_ia32_sysenter_esp,
    field::host_ia32_sysenter_eip,
    field::host_rsp,
    field::host_rip,
};

/**
 * The guest-state fields carried in both directions between the guest
 * hypervisor's VMCS and the one that runs its guest.
 *
 * One list rather than two, because the two directions must agree: a field
 * loaded on the way in and not saved on the way out is a field the
 * second-level guest's changes to are silently discarded, which is exactly
 * the class of bug that shows up somewhere unrelated. SDM 27.4 defines the
 * guest-state area and SDM 30.3 defines what a VM exit saves back into it;
 * everything unconditional in the second list is here.
 *
 * What is deliberately absent, and handled by name instead: CR0, CR4 and
 * their read shadows, because the masks make them a computation rather
 * than a copy; RIP, RSP and RFLAGS, which are saved back but loaded from
 * the guest hypervisor's own values; DR7, IA32_PAT and IA32_EFER, whose
 * save-back is conditional on the guest hypervisor's exit controls; the
 * interruptibility state, which the entry-event path also writes; and the
 * activity state, which is not a copy in either direction because vmcs02
 * is not entered in every state vmcs12 may name - see `enter_or_park_l2`.
 */
constexpr field guest_state_fields[] = {
    field::guest_es_selector,
    field::guest_cs_selector,
    field::guest_ss_selector,
    field::guest_ds_selector,
    field::guest_fs_selector,
    field::guest_gs_selector,
    field::guest_ldtr_selector,
    field::guest_tr_selector,
    field::guest_es_limit,
    field::guest_cs_limit,
    field::guest_ss_limit,
    field::guest_ds_limit,
    field::guest_fs_limit,
    field::guest_gs_limit,
    field::guest_ldtr_limit,
    field::guest_tr_limit,
    field::guest_gdtr_limit,
    field::guest_idtr_limit,
    field::guest_es_access_rights,
    field::guest_cs_access_rights,
    field::guest_ss_access_rights,
    field::guest_ds_access_rights,
    field::guest_fs_access_rights,
    field::guest_gs_access_rights,
    field::guest_ldtr_access_rights,
    field::guest_tr_access_rights,
    field::guest_es_base,
    field::guest_cs_base,
    field::guest_ss_base,
    field::guest_ds_base,
    field::guest_fs_base,
    field::guest_gs_base,
    field::guest_ldtr_base,
    field::guest_tr_base,
    field::guest_gdtr_base,
    field::guest_idtr_base,
    field::guest_cr3,
    field::guest_pending_debug_exceptions,
    field::guest_ia32_sysenter_esp,
    field::guest_ia32_sysenter_eip,
    field::guest_ia32_sysenter_cs,
    field::guest_ia32_debugctl,
    field::guest_pdpte_0,
    field::guest_pdpte_1,
    field::guest_pdpte_2,
    field::guest_pdpte_3,
};

/**
 * The effective CR0 and CR4 a second-level guest reads, which is what its
 * read shadow under this VMM has to answer with.
 *
 * A guest reads `(shadow & mask) | (register & ~mask)`. Under the guest
 * hypervisor alone that is vmcs12's own three fields; under this VMM the
 * mask is wider - it includes the bits this VMM owns - so the shadow has
 * to carry the whole answer rather than half of it. Writing the effective
 * value into the shadow does that for every bit the wider mask covers,
 * and the bits it does not cover are outside vmcs12's mask too, where the
 * real register already agrees. KVM computes the same value in
 * `nested_read_cr0` and `nested_read_cr4`.
 * @{
 */
constexpr std::uint64_t effective_control_register(std::uint64_t value,
                                                   std::uint64_t shadow,
                                                   std::uint64_t mask)
{
    return (value & ~mask) | (shadow & mask);
}
/**
 * @}
 */

} // namespace

std::expected<void, zpp::error> hypervisor::check_nested_msr_area(
    std::uint64_t address, std::uint64_t count, bool loading)
{
    if (0 == count) {
        return {};
    }

    // SDM 29.2.1.1 checks the address itself as a control: 16-byte aligned
    // and the last byte within the processor's physical-address width.
    // That part is the architecture's and applies whether or not the
    // contents are looked at.
    auto bytes = count * sizeof(msr_area_entry);
    auto limit = 1ull << physical_address_bits();

    if ((0 != (address & 0xf)) || (count > msr_area_capacity) ||
        (address >= limit) || (bytes > (limit - address))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // The contents, which is where this diverges from a processor and does
    // so deliberately. A processor checks each entry as it processes it -
    // at VM entry for the entry area, and at VM *exit* for the two exit
    // areas, where a failure is a VMX abort and a VMX abort is a shutdown.
    // There is no shutdown available here that does not take the whole
    // machine with it, so all three are checked up front instead, and a
    // guest hypervisor is refused before its guest runs rather than after.
    //
    // What that costs is precision about which failure it was; what it
    // buys is that the exit path cannot fail on anything but memory that
    // stopped being readable, which is the one case left for the abort
    // below.
    for (std::uint64_t i{}; i < count; ++i) {
        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index) ||
            (loading &&
             !msr_area_value_writable(entry.index, entry.value))) {
            // Which MSR, because the caller only gets an error code.
            //
            // The list this checks against is closed, the areas are guest
            // memory a guest hypervisor rewrites with no exit, and the
            // check is stateless and runs on every entry - so the first
            // index it does not know refuses that virtual processor's
            // every subsequent entry, permanently and silently. Without
            // this line the refusal names no MSR and the next boot
            // answers nothing; with it, the fix is to add whatever it
            // names to msr_area_index_handled.
            log("msr area at {} ({}) refuses index {}, value {}",
                address,
                loading ? "load" : "store",
                entry.index,
                entry.value);

            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }
    }

    return {};
}

std::expected<void, zpp::error> hypervisor::load_nested_msrs(
    std::size_t cpu, std::uint64_t address, std::uint64_t count)
{
    for (std::uint64_t i{}; i < count; ++i) {
        // SDM 29.8: the exit qualification of an MSR-loading failure is
        // "the number of the entry that caused the problem (1 for the
        // first entry, 2 for the second, etc.)".
        this->nested_msr_failure_entry[cpu] = i + 1;

        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        // Re-checked rather than trusted, because the area is guest memory
        // and nothing stops a guest hypervisor rewriting it between the
        // check and here. The check exists to give a good answer early;
        // this exists so that a bad one cannot reach WRMSR.
        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index) ||
            !msr_area_value_writable(entry.index, entry.value)) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        arch::x86_64::wrmsr(entry.index, entry.value);
    }

    return {};
}

std::expected<void, zpp::error>
hypervisor::store_nested_msrs(std::uint64_t address, std::uint64_t count)
{
    for (std::uint64_t i{}; i < count; ++i) {
        msr_area_entry entry{};

        auto read = read_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<std::byte *>(&entry),
                      sizeof(entry)));
        if (!read) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }

        if (!msr_area_index_allowed(entry) ||
            !msr_area_index_handled(entry.index)) {
            return std::unexpected(
                zpp::error{error::nested_msr_area_unsupported});
        }

        entry.value = arch::x86_64::rdmsr(entry.index);

        auto written = write_guest_physical(
            address + (i * sizeof(entry)),
            std::span(reinterpret_cast<const std::byte *>(&entry),
                      sizeof(entry)));
        if (!written) {
            return std::unexpected(
                zpp::error{error::guest_memory_unreachable});
        }
    }

    return {};
}

bool hypervisor::own_msr_intercepted(std::uint32_t index, bool write) const
{
    // The same four 1024-byte bitmaps intercept_msr writes, read back.
    // SDM 27.6.9, "MSR-Bitmap Address".
    std::size_t base{};
    std::uint32_t bit{};

    if (index < 0x2000) {
        base = write ? 0x800 : 0x000;
        bit = index;
    } else if ((index >= 0xc0000000) && (index < 0xc0002000)) {
        base = write ? 0xc00 : 0x400;
        bit = index - 0xc0000000;
    } else {
        // Outside both ranges no bitmap is consulted and the access exits
        // unconditionally (SDM 28.1.3), so this VMM's bitmap did not ask
        // for it and the answer here is no.
        //
        // That matters more than it looks. This VMM answers an
        // out-of-range MSR with a general protection fault, which is what
        // bare hardware gives - but for a *second-level* guest the machine
        // is the guest hypervisor, and the synthetic MSR ranges a
        // hypervisor presents to its guest live exactly there: Hyper-V's
        // are at 40000000H upwards. Claiming those would have this VMM
        // fault an interface the guest hypervisor implements.
        return false;
    }

    return 0 != (this->msr_bitmap[base + (bit / 8)] & (1u << (bit % 8)));
}

bool hypervisor::own_io_port_intercepted(std::uint16_t port) const
{
    // Bitmap A covers ports 0000H-7FFFH and bitmap B 8000H-FFFFH. SDM
    // 27.6.4, "I/O-Bitmap Addresses".
    const auto & bitmap =
        (port < 0x8000) ? this->io_bitmap_a : this->io_bitmap_b;
    auto bit = static_cast<std::size_t>(port & 0x7fff);

    return 0 != (bitmap[bit / 8] & (1u << (bit % 8)));
}

std::expected<void, zpp::error>
hypervisor::merge_nested_bitmaps(std::size_t cpu)
{
    auto & shadow = this->guest_vmcs12[cpu];

    auto msr_source = shadow.read(field::msr_bitmap);
    auto io_a_source = shadow.read(field::io_bitmap_a);
    auto io_b_source = shadow.read(field::io_bitmap_b);

    // Every VM entry, with no cache on the bitmap addresses.
    //
    // A cache on the addresses was written first and is wrong, which is
    // worth recording because it looks obviously right: the addresses are
    // VMCS fields and a guest hypervisor changes them rarely, so caching
    // on them seems free. But the *contents* live in guest memory the
    // guest hypervisor writes directly, with no VMWRITE and no exit - a
    // hypervisor that stops intercepting an MSR clears a bit in the page
    // it already named. Cached on the address, this VMM would have gone on
    // trapping what the guest hypervisor stopped asking for and, worse,
    // gone on *not* trapping what it started asking for. KVM re-merges on
    // every entry too, in `nested_vmx_prepare_msr_bitmap`.
    //
    // What it costs is one guest page read per area the guest hypervisor
    // actually uses, per entry - four kilobytes for a hypervisor that uses
    // MSR bitmaps and no I/O bitmaps, which is the usual shape. Caching it
    // correctly needs a way to notice a write to those pages, and this VMM
    // has one: `watch_guest_page_writes`. That is the optimisation, and
    // the measurement that would justify it is the entry rate of a real
    // guest hypervisor, which nothing has yet run.

    // The guest hypervisor's page is read straight into the destination
    // and this VMM's own is then or'd on top, rather than the other way
    // round with a scratch page in between. There is nowhere to put a
    // scratch page: the host stack a VM exit runs on is under four
    // kilobytes, so a page-sized local would run off the end of it, and a
    // per-processor scratch member would cost what it saves.
    //
    // Reading into the destination means a failed read leaves a
    // half-merged bitmap behind. Harmless, because a failure refuses the
    // VM entry and the next attempt merges again from scratch - so the
    // half-merged state is never entered with.
    //
    // The merge itself is the union. A bit set on either side is an exit,
    // which is what makes it safe: every exit that happens is one of the
    // two asked for, and whichever asked for it knows how to answer it.
    auto merge_page =
        [&](std::uint64_t from,
            const void * ours,
            std::uint8_t * into,
            bool read_theirs) -> std::expected<void, zpp::error> {
        if (read_theirs) {
            auto read = read_guest_physical(
                from,
                std::span(reinterpret_cast<std::byte *>(into), page_size));
            if (!read) {
                return std::unexpected(read.error());
            }
        } else {
            std::memset(into, 0, page_size);
        }

        auto mine = static_cast<const std::uint8_t *>(ours);
        for (std::size_t i{}; i < page_size; ++i) {
            into[i] = static_cast<std::uint8_t>(into[i] | mine[i]);
        }

        return {};
    };

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);

    auto their_msr_bitmap = 0 != (primary12 & primary_msr_bitmaps);
    auto their_io_bitmaps = 0 != (primary12 & primary_io_bitmaps);

    if (auto merged = merge_page(msr_source,
                                 this->msr_bitmap,
                                 this->nested_msr_bitmap[cpu],
                                 their_msr_bitmap);
        !merged) {
        return merged;
    }

    if (auto merged = merge_page(io_a_source,
                                 this->io_bitmap_a,
                                 this->nested_io_bitmap[cpu],
                                 their_io_bitmaps);
        !merged) {
        return merged;
    }

    if (auto merged = merge_page(io_b_source,
                                 this->io_bitmap_b,
                                 this->nested_io_bitmap[cpu] + page_size,
                                 their_io_bitmaps);
        !merged) {
        return merged;
    }

    this->nested_msr_bitmap_physical[cpu] =
        this->host_page_table.virtual_to_physical(
            this->nested_msr_bitmap[cpu]);
    this->nested_io_bitmap_physical[cpu] =
        this->host_page_table.virtual_to_physical(
            this->nested_io_bitmap[cpu]);

    return {};
}

std::expected<void, zpp::error> hypervisor::build_vmcs02(std::size_t cpu)
{
    namespace vmx_msr = arch::x86_64::vmx::msr;

    if constexpr (!nested_vmx::enabled) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    auto pin12 = shadow.read(field::pin_based_vm_execution_controls);
    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};
    auto exit12 = shadow.read(field::vm_exit_controls);
    auto entry12 = shadow.read(field::vm_entry_controls);

    // What the guest hypervisor actually asked for, recorded once.
    //
    // Only the first entry's values are kept: a hypervisor that runs
    // many second-level guests would otherwise leave the last one's
    // controls here, and the question this answers is what it needs at
    // all, which the first launch already settles.
    if (0 == this->vmcs12_controls_captured) {
        this->vmcs12_pin_controls = pin12;
        this->vmcs12_primary_controls = primary12;
        this->vmcs12_secondary_controls = shadow.read(
            field::secondary_processor_based_vm_execution_controls);
        this->vmcs12_exit_controls = exit12;
        this->vmcs12_entry_controls = entry12;
        this->vmcs12_controls_captured = 1;
    }

    // Every control the guest hypervisor set has to be one the capability
    // MSRs told it it could set, and every control they said must be 1 has
    // to be 1. SDM 29.2.1.1 makes that the first check on the VM-execution
    // controls, and adjust_msr applied to the narrowed MSR is exactly it:
    // a value that already satisfies both halves is its own fixed point.
    //
    // Checked against the *narrowed* MSRs rather than the hardware's,
    // which is the point of narrowing them. Without this a guest
    // hypervisor could set a control this VMM never offered - virtualized
    // APIC accesses, say - and the union below would hand it to the
    // processor with nothing here maintaining the state it needs.
    auto within_capability = [&](std::size_t msr, std::uint64_t value) {
        return value == arch::x86_64::vmx::adjust_msr(
                            nested_vmx_capability_msr(msr), value);
    };

    if (!within_capability(vmx_msr::true_pin_based_controls, pin12) ||
        !within_capability(vmx_msr::true_processor_based_controls,
                           primary12) ||
        !within_capability(vmx_msr::true_exit_controls, exit12) ||
        !within_capability(vmx_msr::true_entry_controls, entry12) ||
        ((0 != (primary12 & primary_secondary_controls)) &&
         !within_capability(vmx_msr::processor_based_contorls_2,
                            secondary12))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // The two consistency rules SDM 29.2.1.1 states about NMIs, which are
    // the only control combinations reachable here that a union of the two
    // sides cannot repair. Both are also KVM's
    // `nested_vmx_check_nmi_controls`.
    constexpr std::uint64_t pin_nmi_exiting = 1ull << 3;
    constexpr std::uint64_t pin_virtual_nmis = 1ull << 5;

    if (((0 == (pin12 & pin_nmi_exiting)) &&
         (0 != (pin12 & pin_virtual_nmis))) ||
        ((0 == (pin12 & pin_virtual_nmis)) &&
         (0 != (primary12 & primary_nmi_window)))) {
        return std::unexpected(
            zpp::error{error::nested_controls_unsupported});
    }

    // All three MSR areas are checked here, up front, rather than each
    // where a processor would look at it. See check_nested_msr_area for
    // why: a failure in either exit area is a VMX abort, and a VMX abort
    // is a shutdown there is no way to perform on one guest's behalf.
    auto entry_load_address =
        shadow.read(field::vm_entry_msr_load_address);
    auto entry_load_count = shadow.read(field::vm_entry_msr_load_count);
    auto exit_load_address = shadow.read(field::vm_exit_msr_load_address);
    auto exit_load_count = shadow.read(field::vm_exit_msr_load_count);
    auto exit_store_address =
        shadow.read(field::vm_exit_msr_store_address);
    auto exit_store_count = shadow.read(field::vm_exit_msr_store_count);

    if (auto checked = check_nested_msr_area(
            entry_load_address, entry_load_count, true);
        !checked) {
        return checked;
    }

    if (auto checked = check_nested_msr_area(
            exit_load_address, exit_load_count, true);
        !checked) {
        return checked;
    }

    if (auto checked = check_nested_msr_area(
            exit_store_address, exit_store_count, false);
        !checked) {
        return checked;
    }

    // A 64-bit host is the only shape this can put back, since the exit
    // comes here and the guest hypervisor is resumed in whatever mode its
    // own host-state area describes. SDM 29.2.2 makes this an error 8
    // condition on its own terms - the host-state checks are where the
    // address-space size and the CS selector are validated together.
    if (0 == (exit12 & exit_host_address_space_size)) {
        return std::unexpected(
            zpp::error{error::nested_host_state_unsupported});
    }

    // The second level of address translation, which is either a shadow
    // composed out of the guest hypervisor's tables or - when it uses
    // none - this VMM's own.
    //
    // The second case is not a shortcut. Without extended page tables of
    // its own a guest hypervisor shadow-pages instead, so its guest's
    // physical addresses *are* its own, and its own are what this VMM's
    // identity map already translates with the module and every watched
    // page removed. So the protections are still in force, which is the
    // whole reason BACKLOG.md rejects handing the guest hypervisor's EPT
    // pointer straight to the processor.
    std::uint64_t eptp02{};

    if (0 != (secondary12 & secondary_enable_ept)) {
        auto eptp12 = shadow.read(field::ept_pointer);

        // The checks SDM 29.2.1.1 puts on the pointer, in the same order
        // as KVM's `nested_vmx_check_eptp`: a memory type this VMM
        // reports, a page-walk length it reports, the accessed-and-dirty
        // bit only where that capability is reported, and no bit set
        // above what the processor can address. Bits 5:3 hold the walk
        // length minus one, bit 6 enables accessed and dirty flags, and
        // bits 11:7 are reserved.
        constexpr std::uint64_t eptp_memory_type_mask = 0x7;
        constexpr std::uint64_t eptp_walk_length_mask = 0x38;
        constexpr std::uint64_t eptp_walk_length_4 = 3ull << 3;
        constexpr std::uint64_t eptp_access_and_dirty = 0x40;
        constexpr std::uint64_t eptp_reserved = 0xf80;

        // IA32_VMX_EPT_VPID_CAP bit 21, from SDM A.10. The shadow never
        // sets accessed or dirty in an entry it writes, so a guest
        // hypervisor asking for them here would poll its own tables and
        // find nothing ever touched. The capability MSR withholds the
        // bit; this is the other half of withholding it, and without it
        // the pointer is accepted and the promise quietly broken. KVM
        // pairs the two the same way, in `nested_vmx_check_eptp`'s "AD,
        // if set, should be supported".
        constexpr std::uint64_t ept_cap_access_and_dirty = 1ull << 21;

        auto memory_type = eptp12 & eptp_memory_type_mask;
        auto is_uncachable =
            memory_type == static_cast<std::uint64_t>(
                               arch::x86_64::memory_type::uncachable);
        auto is_write_back =
            memory_type == static_cast<std::uint64_t>(
                               arch::x86_64::memory_type::write_back);

        auto address_mask =
            ((1ull << physical_address_bits()) - 1) & ~0xfffull;

        auto access_and_dirty_offered =
            0 != (nested_vmx_capability_msr(vmx_msr::vpid_ept_capability) &
                  ept_cap_access_and_dirty);

        if ((!is_uncachable && !is_write_back) ||
            (eptp_walk_length_4 != (eptp12 & eptp_walk_length_mask)) ||
            (!access_and_dirty_offered &&
             (0 != (eptp12 & eptp_access_and_dirty))) ||
            (0 != (eptp12 & eptp_reserved)) ||
            (0 != (eptp12 & ~(address_mask | 0xfffull)))) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }

        auto shadow_pointer = shadow_ept_pointer_for(cpu, eptp12);
        if (!shadow_pointer) {
            return std::unexpected(shadow_pointer.error());
        }

        eptp02 = *shadow_pointer;
    } else {
        arch::x86_64::vmx::ept_pointer pointer;
        pointer.memory_type(arch::x86_64::memory_type::write_back);
        pointer.page_walk_length(4);
        pointer.page_number(this->epml4_physical >> 12);
        eptp02 = pointer;
    }

    // The TPR shadow, which a real guest hypervisor was measured setting -
    // primary 0xa4206dfa, bit 21 - and which withholding cost seventeen
    // second-level entries and then a boot loop. nested_vmx.h carries the
    // whole capture. Three outcomes here: honoured, replaced, or refused.
    auto tpr_shadow12 = 0 != (primary12 & primary_tpr_shadow);
    auto honour_tpr_shadow = false;
    std::uint64_t virtual_apic12{};
    std::uint64_t tpr_threshold12{};

    if (tpr_shadow12) {
        virtual_apic12 = shadow.read(field::virtual_apic_address);
        tpr_threshold12 = shadow.read(field::tpr_threshold);

        // SDM 29.2.1.1 puts one check on the threshold that always
        // applies here: "If the 'use TPR shadow' VM-execution control is 1
        // and the 'virtual-interrupt delivery' VM-execution control is 0,
        // bits 31:4 of the TPR threshold VM-execution control field must
        // be 0." Virtual-interrupt delivery is not offered, so the second
        // half of that condition is always true.
        //
        // The same paragraph has a second rule about the threshold - that
        // its bits 3:0 "should not be greater than the value of bits 7:4
        // of VTPR" - which is deliberately not checked. It says *should*
        // rather than *must*, it would cost a guest page read on every
        // entry, and KVM does not check it either.
        if (0 != (tpr_threshold12 & ~0xfull)) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }

        // The address, which is the part that has to be got right. SDM
        // 29.2.1.1 lists the virtual-APIC address among the fields whose
        // "bits 11:0 must be 0, and the fields are also subject to the
        // checks on physical-address width described above in Section
        // 28.2.1". KVM checks exactly those two, in
        // `nested_vmx_check_tpr_shadow_controls` through
        // `page_address_valid`.
        //
        // Two checks are added to them, and both are this VMM's rather
        // than the architecture's.
        //
        // Zero is refused. A processor would accept it - page zero is 4 KB
        // aligned and within any width - but a guest hypervisor that set
        // the control and never wrote the field leaves exactly zero
        // behind, since a shadow VMCS starts zeroed, and page zero holds
        // the real-mode interrupt vector table. Accepting it would have
        // the processor write VTPR over the guest's own IVT.
        //
        // Then the one that matters. **The processor reads and writes this
        // page in root operation, where extended page tables do not
        // apply** - so none of this VMM's protections cover those
        // accesses. The module's own pages, the log queue storage and
        // every watched page are hidden from the guest by extended
        // page-table permissions and by nothing else, so a guest
        // hypervisor naming one of them here would have the processor
        // write into it on its behalf. That is the same hazard the MSR
        // areas carry, and the reason their addresses are never handed to
        // the processor at all; this one has to be, because the whole
        // point of the control is that the processor uses the page.
        //
        // host_ept_lookup answers it directly and without allocating: it
        // is this VMM's own translation for the address with the
        // permissions accumulated down the walk. The test is therefore the
        // right one rather than a list that can drift - may the guest
        // itself read and write this page? If it may not, the processor
        // may not either. The extended page tables are an identity map of
        // the first 512 GB (see initialize_ept), so the L1-physical
        // address vmcs12 names is also the host-physical address vmcs02
        // gets, and a lookup that returns anything but `mapped` is already
        // an address outside that map.
        auto address_limit = 1ull << physical_address_bits();
        auto ours = host_ept_lookup(virtual_apic12);

        auto usable =
            (0 != virtual_apic12) && (0 == (virtual_apic12 & 0xfff)) &&
            (virtual_apic12 < address_limit) &&
            (arch::x86_64::vmx::ept_walk_status::mapped == ours.status) &&
            ours.permissions.read() && ours.permissions.write();

        // A page this VMM will not let the processor touch does not have
        // to refuse the entry, and refusing it is the worse of the two
        // answers where the guest hypervisor also asked for CR8-load and
        // CR8-store exiting. With both of those set the processor never
        // consults the virtual-APIC page at all - SDM 27.6.8 makes MOV CR8
        // the only operation that reads it here, since the other two need
        // controls this VMM withholds, and CR8 exiting means MOV CR8
        // never completes - so the control can be dropped with nothing
        // lost, and the exits it would have replaced go to the guest
        // hypervisor, which asked for them. That is KVM's fallback in
        // `nested_get_vmcs12_pages`, under the comment "the processor will
        // never use the TPR shadow, simply clear the bit from the
        // execution control"; the same function calls failing the entry
        // for any other configuration "_not_ what the processor does but
        // it's basically the only possibility we have", which is the case
        // below.
        //
        // Both controls, not either: a guest hypervisor that intercepts
        // the load and not the store still expects `mov rax, cr8` to read
        // VTPR out of the page.
        constexpr std::uint64_t cr8_exiting =
            primary_cr8_load_exiting | primary_cr8_store_exiting;

        if (usable) {
            honour_tpr_shadow = true;
        } else if (cr8_exiting != (primary12 & cr8_exiting)) {
            return std::unexpected(
                zpp::error{error::nested_controls_unsupported});
        }
    }

    if (auto merged = merge_nested_bitmaps(cpu); !merged) {
        return merged;
    }

    // Everything that is this VMM's, read out of the VMCS that runs the
    // guest hypervisor before that one stops being current.
    std::uint64_t host_values[std::size(host_state_fields)]{};
    for (std::size_t i{}; i < std::size(host_state_fields); ++i) {
        host_values[i] = vmcs.read(host_state_fields[i]);
    }

    auto pin01 = vmcs.pin_based_vm_execution_controls();
    auto primary01 = vmcs.primary_processor_based_vm_execution_controls();
    auto secondary01 =
        vmcs.secondary_processor_based_vm_execution_controls();
    auto exit01 = vmcs.vm_exit_controls();
    auto exception_bitmap01 = vmcs.read(field::exception_bitmap);
    auto cr0_mask01 = vmcs.read(field::cr0_guest_host_mask);
    auto cr4_mask01 = vmcs.read(field::cr4_guest_host_mask);
    auto vpid01 = vmcs.vpid();

    // From here nothing may fail: vmcs02 is about to become current, and a
    // caller that answered VMfail with it current would resume the guest
    // hypervisor on the wrong VMCS.
    if (arch::x86_64::vmx::vmptrld(&this->vmcs02_physical[cpu])) {
        return std::unexpected(zpp::error{error::vmptrld_failed});
    }

    for (std::size_t i{}; i < std::size(host_state_fields); ++i) {
        vmcs.write(host_state_fields[i], host_values[i]);
    }

    // Pin-based controls: the union, less the preemption timer, which is
    // this VMM's alone. The capability MSRs do not offer it to a guest
    // hypervisor, and this VMM arms it to drive its own log - so a guest
    // hypervisor's copy of the bit means nothing and its exits are not
    // reflected.
    vmcs.pin_based_vm_execution_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_pin_based_controls),
        (pin01 | pin12) &
            ~(pin_preemption_timer | pin_posted_interrupts)));

    // Primary controls: the union, with the two window controls taken from
    // the guest hypervisor alone. A window exit says "the guest can take
    // an interrupt now", which is an answer to a question only whoever
    // asked it can act on - so inheriting this VMM's would produce exits
    // with nothing to do, and inheriting the guest hypervisor's produces
    // exits it is waiting for. KVM does the same in
    // `prepare_vmcs02_early`.
    auto primary =
        (primary01 & ~(primary_interrupt_window | primary_nmi_window)) |
        primary12;

    // A step in progress survives the rebuild.
    //
    // The monitor trap flag is armed on whichever VMCS is current, and
    // with no second-level extended page tables a watched-page violation
    // taken by a second-level guest is handled with **vmcs02** current -
    // so that is where the bit lands. It is not in `primary01`, and a
    // guest hypervisor has no reason to set it in `primary12`, so this
    // recomputation drops it. Anything reflected to the guest hypervisor
    // before the step completes rebuilds these controls, and then:
    //
    // - the trap exit never arrives, so `on_monitor_trap_flag` never
    //   runs and never closes the page;
    // - `stepping_watch[cpu]` stays set for ever, so `l0_wants_l2_exit`
    //   claims every later trap exit;
    // - and the watched page - the local APIC's - is left **writable for
    //   every processor**, because that is what opening it did. No
    //   further violation is taken on it and nothing re-closes it, so
    //   this VMM stops seeing local APIC writes entirely: every
    //   interrupt command, INIT and start-up IPI after that point is
    //   invisible, with nothing recorded anywhere.
    //
    // KVM does not lose it either: `vmx_update_emulated_instruction`
    // records `nested.mtf_pending` and `vmx_check_nested_events`
    // delivers it as a monitor-trap-flag VM exit, so it survives round
    // trips through L1 and is even part of migration state.
    if (this->stepping_watch[cpu]) {
        primary |= primary_monitor_trap_flag;
    }

    // The TPR shadow, decided above. Three branches, and the difference
    // between them is which of them is allowed to leave the second-level
    // guest's `mov cr8` reaching the physical control register.
    if (honour_tpr_shadow) {
        // Honoured. The control stays set - it is already in `primary`
        // from the union - and the two fields behind it are written from
        // vmcs12: the page the processor will virtualize VTPR in, and the
        // threshold below which it exits. KVM copies the same pair, in
        // `nested_get_vmcs12_pages` for the address and
        // `prepare_vmcs02_early` for the threshold.
        //
        // The address is written unchanged because the extended page
        // tables are an identity map, so vmcs12's L1-physical address is
        // the host-physical one this field takes. Nothing pins the page:
        // there is no paging here and no memory hot-unplug, and the
        // validation above is what stands in for KVM's kvm_vcpu_map.
        vmcs.virtual_apic_address(virtual_apic12);
        vmcs.tpr_threshold(tpr_threshold12);
    } else if (tpr_shadow12) {
        // Asked for and not honoured, which the branch above only reaches
        // for a virtual-APIC page this VMM refuses to let the processor
        // touch *and* a guest hypervisor that intercepts both CR8
        // accesses. Removing the control alone would be the bug this
        // whole change exists to fix, so the intercepts it relies on are
        // forced rather than assumed - redundant today, since they came
        // out of vmcs12 through the union above, and load bearing the
        // moment anything narrows that union. KVM forces the same pair in
        // `prepare_vmcs02_early`: "else exec_control |=
        // CPU_BASED_CR8_LOAD_EXITING | CPU_BASED_CR8_STORE_EXITING".
        //
        // Both exits reflect to the guest hypervisor, because
        // `l1_wants_l2_exit` answers CR8 accesses against exactly these
        // two controls and this branch requires both of them set.
        primary &= ~primary_tpr_shadow;
        primary |= primary_cr8_load_exiting | primary_cr8_store_exiting;
    } else {
        // Never asked for, so the bit can only be here from this VMM's
        // own controls - which do not set it today, making this a guard
        // rather than a case. It is removed and *nothing* is forced in its
        // place, which is the opposite of the branch above and is
        // deliberate: a guest hypervisor that did not ask for the TPR
        // shadow does not believe its guest's CR8 is virtualized, so the
        // architecture's answer is that the second-level guest owns the
        // physical register, exactly as it would on bare hardware.
        // Forcing CR8 exiting here would manufacture exits neither side
        // asked for, which `l1_wants_l2_exit` would decline and the
        // ordinary control-register handler would stop the processor on -
        // it answers MOV to CR4 and nothing else.
        primary &= ~primary_tpr_shadow;
    }

    // The bitmaps, whose controls follow the merge rather than either
    // side. "Use MSR bitmaps" clear means *every* MSR access exits, which
    // is what a guest hypervisor that set no bitmap asked for - so the
    // control is the guest hypervisor's, and the bitmap behind it is the
    // union.
    if (0 != (primary12 & primary_msr_bitmaps)) {
        primary |= primary_msr_bitmaps;
        vmcs.msr_bitmap(this->nested_msr_bitmap_physical[cpu]);
    } else {
        primary &= ~primary_msr_bitmaps;
    }

    // I/O the same way, with one asymmetry: this VMM always uses bitmaps
    // and has ports of its own in them, so a guest hypervisor that uses
    // neither still gets bitmap-driven exits - its own guest's I/O reaches
    // it only for the ports this VMM watches, which is exactly right,
    // since it asked for none.
    if (0 != (primary12 & primary_unconditional_io)) {
        primary |= primary_unconditional_io;
        primary &= ~primary_io_bitmaps;
    } else {
        primary &= ~primary_unconditional_io;
        primary |= primary_io_bitmaps;
        vmcs.write(field::io_bitmap_a,
                   this->nested_io_bitmap_physical[cpu]);
        vmcs.write(field::io_bitmap_b,
                   this->nested_io_bitmap_physical[cpu] + page_size);
    }

    // Extended page tables are always in use for a second-level guest, so
    // the secondary controls are always activated whatever the guest
    // hypervisor asked.
    primary |= primary_secondary_controls;

    vmcs.primary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::true_processor_based_controls),
            primary));

    // Secondary controls: the union, with three corrections.
    //
    // Unrestricted guest comes from the guest hypervisor alone. With it
    // on, a guest may run with CR0.PE clear, and the guest-state checks
    // SDM 29.3.1 applies are relaxed accordingly - so inheriting this
    // VMM's copy would accept a second-level guest state its own
    // hypervisor's VMCS says is invalid. KVM clears it for the same
    // reason.
    //
    // Mode-based execute control is cleared, and this is the one that is
    // not obvious. The capability MSRs do not offer it, so bit 10 of the
    // guest hypervisor's own extended page-table entries means nothing it
    // chose - it has no reason ever to set it. The shadow builder
    // intersects that bit with this VMM's, so composing it would leave
    // every shadow leaf denying user-mode execute, and the second-level
    // guest would fault on the first instruction it ran in user mode. With
    // the control clear the processor ignores bit 10 entirely and bit 2
    // governs both modes, which is what both levels meant.
    //
    // Extended page tables and VPIDs are always on, because the pointer
    // written below is always a real one and the VPID always non-zero.
    auto secondary =
        (secondary01 | secondary12) &
        ~(secondary_mode_based_execute | secondary_unrestricted_guest);

    secondary |= secondary12 & secondary_unrestricted_guest;
    secondary |= secondary_enable_ept | secondary_enable_vpid;

    vmcs.secondary_processor_based_vm_execution_controls(
        arch::x86_64::vmx::adjust_msr(
            this->cached_vmx_msr(vmx_msr::processor_based_contorls_2),
            secondary));

    // Exit controls are this VMM's, unchanged. The exit comes here.
    vmcs.vm_exit_controls(exit01);

    // Entry controls are the guest hypervisor's, unchanged. They describe
    // what VM entry loads into *its* guest, which is a decision it owns
    // completely - including "IA-32e mode guest", which has to agree with
    // the CR0 and CR4 it wrote beside them or the entry fails its own
    // consistency check and is reflected as such.
    vmcs.vm_entry_controls(arch::x86_64::vmx::adjust_msr(
        this->cached_vmx_msr(vmx_msr::true_entry_controls), entry12));

    vmcs.ept_pointer(eptp02);
    vmcs.vpid(vpid01);

    // All ones is "no linked VMCS". VMCS shadowing is not offered, so the
    // guest hypervisor's own link pointer is not consulted.
    vmcs.vmcs_link_pointer(~std::uint64_t{});

    // Where a refused entry unwinds to. See asm.h: the stubs read this
    // field because on a refusal nothing has been reloaded and there is no
    // other per-processor thing left addressable.
    vmcs.write(field::cr3_target_value_0,
               reinterpret_cast<std::uint64_t>(
                   &this->nested_entry_recovery[cpu]));
    vmcs.write(field::cr3_target_count, 0);

    // The exception bitmap is the bitwise or of what the guest hypervisor
    // wants to trap and what this VMM does, which is the merge KVM
    // describes on `vmx_update_exception_bitmap`. The page-fault
    // error-code mask and match come from the guest hypervisor unchanged,
    // because this VMM traps no page faults of its own - if it ever does,
    // both must go to zero so that every page fault exits and the
    // filtering moves into the reflect decision.
    vmcs.write(field::exception_bitmap,
               exception_bitmap01 | shadow.read(field::exception_bitmap));
    vmcs.write(field::page_fault_error_code_mask,
               shadow.read(field::page_fault_error_code_mask));
    vmcs.write(field::page_fault_error_code_match,
               shadow.read(field::page_fault_error_code_match));

    // The control-register masks are the union too, and the read shadows
    // then have to carry the whole answer rather than half of it - see
    // effective_control_register.
    auto cr0_mask12 = shadow.read(field::cr0_guest_host_mask);
    auto cr4_mask12 = shadow.read(field::cr4_guest_host_mask);
    auto cr0_12 = shadow.read(field::guest_cr0);
    auto cr4_12 = shadow.read(field::guest_cr4);

    vmcs.write(field::cr0_guest_host_mask, cr0_mask01 | cr0_mask12);
    vmcs.write(field::cr4_guest_host_mask, cr4_mask01 | cr4_mask12);

    vmcs.cr0_read_shadow(effective_control_register(
        cr0_12, shadow.read(field::cr0_read_shadow), cr0_mask12));
    vmcs.cr4_read_shadow(effective_control_register(
        cr4_12, shadow.read(field::cr4_read_shadow), cr4_mask12));

    // VMXE is forced into the real register for the same reason it is for
    // the guest hypervisor: IA32_VMX_CR4_FIXED0 requires it in VMX
    // operation, so a guest-state area without it fails VM entry. The read
    // shadow above answers for the bit, so nothing sees it.
    vmcs.guest_cr0(cr0_12);
    vmcs.guest_cr4(cr4_12 | cr4_vmxe);

    for (auto guest_field : guest_state_fields) {
        vmcs.write(guest_field, shadow.read(guest_field));
    }

    // The activity state is decided, never copied.
    //
    // Active until `enter_or_park_l2` says otherwise, so that vmcs02 is
    // never left holding a state nothing here chose. That matters because
    // this function runs on every entry attempt while the decision below
    // it may end in the entry not happening at all: a leftover
    // wait-for-SIPI here would be entered by any later resume that skipped
    // the decision, and a processor entered in that state blocks external
    // interrupts, NMIs, INIT and SMIs (SDM 29.7.2) with nothing in vmcs02
    // able to end it - not even this VMM's preemption timer, which
    // build_vmcs02 strips.
    vmcs.write(field::guest_activity_state,
               arch::x86_64::vmx::activity_state::active);

    vmcs.guest_rip(shadow.read(field::guest_rip));
    vmcs.guest_rsp(shadow.read(field::guest_rsp));
    vmcs.guest_rflags(shadow.read(field::guest_rflags));
    vmcs.guest_dr7(shadow.read(field::guest_dr7));
    vmcs.write(field::guest_ia32_pat, shadow.read(field::guest_ia32_pat));
    vmcs.write(field::guest_ia32_efer,
               shadow.read(field::guest_ia32_efer));

    // Carried because the entry control that loads it is now offered.
    //
    // A guest hypervisor that sets "load IA32_BNDCFGS" expects the value
    // in its own VMCS to be the one its guest runs with, so leaving this
    // field at whatever the entered VMCS happened to hold would honour
    // the control's presence and not its meaning. Copied unconditionally
    // rather than under the control bit, exactly as IA32_PAT and
    // IA32_EFER above are: the processor ignores the field when the
    // control is clear, so there is nothing to gate and a gate would
    // only be another thing to get wrong.
    vmcs.write(field::guest_ia32_bndcfgs,
               shadow.read(field::guest_ia32_bndcfgs));
    vmcs.write(field::guest_interruptibility_state,
               shadow.read(field::guest_interruptibility_state));

    // The time stamp counter offset composes across levels: what this VMM
    // applies to the guest hypervisor, plus what the guest hypervisor
    // applies to its own guest. KVM's `kvm_calc_nested_tsc_offset` is the
    // same sum. This VMM applies none today, so the sum is the guest
    // hypervisor's, and writing it as a sum is what keeps it correct if
    // that changes.
    auto tsc_offset01 = (0 != (primary01 & primary_tsc_offsetting))
                            ? vmcs.read(field::tsc_offset)
                            : std::uint64_t{};
    auto tsc_offset12 = (0 != (primary12 & primary_tsc_offsetting))
                            ? shadow.read(field::tsc_offset)
                            : std::uint64_t{};
    vmcs.write(field::tsc_offset, tsc_offset01 + tsc_offset12);

    // The event the guest hypervisor asked to inject, taken from its VMCS
    // on the entry that starts its guest running. SDM 27.8.3 makes the
    // three fields a set: the information field's valid bit decides
    // whether the other two are read at all.
    auto injection =
        shadow.read(field::vm_entry_interruption_information_field);
    vmcs.write(field::vm_entry_interruption_information_field, injection);

    if (0 != (injection & interruption_valid)) {
        vmcs.write(field::vm_entry_exception_error_code,
                   shadow.read(field::vm_entry_exception_error_code));
        vmcs.write(field::vm_entry_instruction_length,
                   shadow.read(field::vm_entry_instruction_length));
    }

    // The processor is given no MSR areas of its own. The three the guest
    // hypervisor named are processed here instead, in software, and
    // handing its addresses to the processor is what must not happen: the
    // processor reads and *writes* those lists in root operation, where
    // extended page tables do not apply - so a guest hypervisor could name
    // this module's own physical pages as its VM-exit MSR-store area and
    // have the processor write MSR values into them. Every other
    // protection this VMM has is an extended page-table permission, and
    // none of them would apply.
    vmcs.write(field::vm_entry_msr_load_count, 0);
    vmcs.write(field::vm_exit_msr_load_count, 0);
    vmcs.write(field::vm_exit_msr_store_count, 0);

    // SDM 29, step 4: the MSR loads are the last thing a VM entry does
    // before the launch state changes. Done here, at the end, for the same
    // reason - and after the guest state is in vmcs02, so that a failure
    // leaves the same thing behind a processor's would.
    this->nested_msr_load_failed[cpu] = false;

    if (auto loaded =
            load_nested_msrs(cpu, entry_load_address, entry_load_count);
        !loaded) {
        this->nested_msr_load_failed[cpu] = true;
        return std::unexpected(
            zpp::error{error::nested_msr_area_unsupported});
    }

    // The transition itself. A guest hypervisor without VPIDs of its own
    // expects VM entry to flush, and a second-level guest shares this
    // VMM's VPID - so the flush has to be performed rather than left to
    // hardware, which will not do it for a non-zero VPID.
    nested_transition_flush();

    return {};
}

void hypervisor::nested_transition_flush()
{
    // Single-context, on this VMM's own VPID, which SDM 31.4.3.1 makes
    // invalidate "linear mappings and combined mappings associated with
    // that VPID ... for all PCIDs and, for combined mappings, all
    // EPTRTAs". That covers both levels at once: the guest hypervisor's
    // mappings and its guest's differ only in the extended page-table
    // root, and this invalidates every root.
    //
    // Over-invalidation rather than precision, deliberately. The
    // alternative is a VPID of its own for the second level, which buys
    // back the mappings this throws away and costs a second identifier per
    // processor plus the bookkeeping to keep it in step with the guest
    // hypervisor's own. The measurement that would justify it is the exit
    // rate of a real guest hypervisor, which nothing has yet run.
    constexpr std::uint64_t single_context = 1;

    struct alignas(0x10) invvpid_descriptor
    {
        std::uint64_t vpid{};
        std::uint64_t linear_address{};
    };

    invvpid_descriptor descriptor{this->vmcs.vpid(), 0};

    if (arch::x86_64::vmx::invvpid(single_context, &descriptor)) {
        log("invvpid failed on a nested transition, cpu {}",
            this->vmcs.vpid());
    }
}

// The captured context is no longer read - the two cases that needed a
// guest register, the MSR number in ECX and the I/O port, are gone. It
// stays in the signature because this and `l1_wants_l2_exit` are a pair
// called from one place with one set of arguments, and because the next
// case to need it will.
bool hypervisor::l0_wants_l2_exit(std::size_t cpu,
                                  arch::x86_64::vmx::exit_reason reason,
                                  const arch::x86_64::context &)
{
    auto & vmcs = this->vmcs;

    // The question this answers is narrow on purpose: not "can this VMM
    // handle it" but "must it", whatever the guest hypervisor asked. KVM
    // splits the decision the same way in `nested_vmx_l0_wants_exit`, and
    // asks it first, because an exit this VMM needs is one no reflection
    // may take away.
    switch (reason.basic()) {
    case basic_reason::exception_or_nmi: {
        // A non-maskable interrupt is this VMM's whoever is running: NMI
        // exiting is set in its own pin controls and the handler hands the
        // interrupt back to the guest's world. Everything else in this
        // exit is an exception, and exceptions belong to whoever put the
        // vector in the exception bitmap.
        auto information =
            vmcs.read(field::vm_exit_interruption_information);
        auto type = (information >> interruption_type_shift) &
                    interruption_type_mask;

        return interruption_type_nmi == type;
    }

    case basic_reason::ept_violation:
    case basic_reason::ept_misconfiguration:
        // Always, and the composition decides afterwards whose fault it
        // was. The processor walked the *shadow*, which is neither side's
        // table, so nothing about the exit as delivered describes what the
        // guest hypervisor's tables say - that has to be worked out here.
        // KVM reaches the same conclusion in `nested_vmx_l0_wants_exit`.
        return true;

    case basic_reason::vmx_preemption_timer:
        // This VMM's clock. The capability MSRs do not offer the timer, so
        // a guest hypervisor cannot have armed it.
        return true;

    case basic_reason::monitor_trap_flag:
        // Only while this VMM is stepping a watched write. A guest
        // hypervisor may set the flag itself - it is in the primary
        // controls it is offered - and then the exit is its own.
        return this->stepping_watch[cpu];

        // MSR accesses and I/O are deliberately absent, and that is a
        // change from what this used to do.
        //
        // They used to be claimed here whenever *this VMM's* bitmap named
        // them, which took them away from a guest hypervisor that had
        // asked for them too. The ones this VMM arms are exactly the set a
        // guest hypervisor presenting VMX to its own guest also arms:
        // IA32_APIC_BASE unconditionally, and IA32_FEATURE_CONTROL with
        // the whole VMX capability range when nested VMX is on. So a
        // second-level guest touching one of those was answered here and
        // its own hypervisor never learned it had - which is the shape of
        // a guest hypervisor that stops making progress with nothing
        // faulting.
        //
        // KVM names neither in `nested_vmx_l0_wants_exit`; the decision is
        // `nested_vmx_l1_wants_exit`'s and the exit is reflected. L0's own
        // interest is served a moment later, when the guest hypervisor
        // performs the access itself and exits from *its* context - which
        // is the right order, because the machine the second-level guest
        // sees is the guest hypervisor's, not this one's.
        //
        // Nothing else is needed to keep this VMM's own interest:
        // `on_l2_exit` already routes an exit neither side asked for to
        // the ordinary handler, so an access only this VMM wanted still
        // lands there.
        //
        // Both were found by tests/nested_exit, which exercises this
        // decision against KVM's for every exit reason.

    default:
        return false;
    }
}

bool hypervisor::l1_wants_l2_exit(std::size_t cpu,
                                  arch::x86_64::vmx::exit_reason reason,
                                  const arch::x86_64::context & context)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};
    auto pin12 = shadow.read(field::pin_based_vm_execution_controls);

    auto primary_set = [&](std::uint64_t control) {
        return 0 != (primary12 & control);
    };

    auto secondary_set = [&](std::uint64_t control) {
        return 0 != (secondary12 & control);
    };

    switch (reason.basic()) {
    case basic_reason::exception_or_nmi: {
        // Filtered through the guest hypervisor's own exception bitmap,
        // with the page-fault error-code mask and match applied to vector
        // 14. SDM 27.6.3: a page fault exits if
        // "(error_code & mask) == match" agrees with the bitmap bit.
        auto information =
            vmcs.read(field::vm_exit_interruption_information);
        auto vector = information & interruption_vector_mask;
        auto bitmap = shadow.read(field::exception_bitmap);

        constexpr std::uint64_t page_fault_vector = 14;

        if (page_fault_vector == vector) {
            auto error_code =
                vmcs.read(field::vm_exit_interruption_error_code);
            auto mask = shadow.read(field::page_fault_error_code_mask);
            auto match = shadow.read(field::page_fault_error_code_match);
            auto in_bitmap = 0 != (bitmap & (1ull << page_fault_vector));

            return in_bitmap == ((error_code & mask) == match);
        }

        return 0 != (bitmap & (1ull << vector));
    }

    case basic_reason::external_interrupt:
        // Unlike KVM, which always takes this for itself because its host
        // has interrupt handlers to run, this VMM never sets
        // external-interrupt exiting - interrupts are the guest's, which
        // owns the interrupt controller. So the control can only be set
        // because the guest hypervisor asked, and the exit can only be
        // its own.
        return 0 != (pin12 & pin_external_interrupt);

    case basic_reason::interrupt_window:
        return primary_set(primary_interrupt_window);
    case basic_reason::nmi_window:
        return primary_set(primary_nmi_window);
    case basic_reason::hlt:
        return primary_set(primary_hlt_exiting);
    case basic_reason::invlpg:
        return primary_set(primary_invlpg_exiting);
    case basic_reason::rdpmc:
        return primary_set(primary_rdpmc_exiting);
    case basic_reason::rdtsc:
    case basic_reason::rdtscp:
        return primary_set(primary_rdtsc_exiting);
    case basic_reason::mov_debug_register:
        return primary_set(primary_mov_dr_exiting);
    case basic_reason::mwait:
        return primary_set(primary_mwait_exiting);
    case basic_reason::monitor:
        return primary_set(primary_monitor_exiting);
    case basic_reason::monitor_trap_flag:
        return primary_set(primary_monitor_trap_flag);
    case basic_reason::pause:
        return primary_set(primary_pause_exiting) ||
               secondary_set(secondary_pause_loop_exiting);
    case basic_reason::rdrand:
        return secondary_set(secondary_rdrand_exiting);
    case basic_reason::rdseed:
        return secondary_set(secondary_rdseed_exiting);
    case basic_reason::wbinvd:
        return secondary_set(secondary_wbinvd_exiting);
    case basic_reason::gdtr_or_idtr:
    case basic_reason::ldtr_or_tr:
        return secondary_set(secondary_descriptor_table_exiting);
    case basic_reason::xsaves:
    case basic_reason::xrstors:
        return secondary_set(secondary_enable_xsaves);
    case basic_reason::invpcid:
        return secondary_set(secondary_enable_invpcid) &&
               primary_set(primary_invlpg_exiting);
    case basic_reason::tpr_below_threshold:
        // Correct without qualification now that the control is honoured,
        // and it was correct before only because the exit could not
        // happen. The exit exists only where vmcs02 has the TPR shadow set
        // (SDM 27.6.8: the threshold "exists only on processors that
        // support the 1-setting of the 'use TPR shadow' VM-execution
        // control"), `build_vmcs02` sets it in vmcs02 only where vmcs12
        // set it, and this VMM never sets it for itself - so the exit is
        // always the guest hypervisor's, never shared. `l0_wants_l2_exit`
        // therefore does not name it.
        return primary_set(primary_tpr_shadow);

    case basic_reason::control_register_access: {
        // SDM Table 28-3, "Exit Qualification for Control-Register
        // Accesses": bits 3:0 the register number, bits 5:4 the access
        // type, bits 11:8 the general purpose register. Same shape as
        // KVM's `nested_vmx_exit_handled_cr`.
        auto qualification = vmcs.exit_qualification();
        auto number = qualification & 0xf;
        auto access = (qualification >> 4) & 0x3;

        constexpr std::uint64_t access_move_to = 0;
        constexpr std::uint64_t access_move_from = 1;
        constexpr std::uint64_t access_clts = 2;
        constexpr std::uint64_t access_lmsw = 3;

        auto cr0_mask = shadow.read(field::cr0_guest_host_mask);
        auto cr4_mask = shadow.read(field::cr4_guest_host_mask);

        switch (access) {
        case access_move_to:
            switch (number) {
            case 0:
            case 4: {
                // A write exits to the guest hypervisor only if it changes
                // a bit the guest hypervisor owns, which is what SDM
                // 26.1.3 makes the guest/host mask mean - so the decision
                // needs the value being written, not just the mask.
                //
                // Comparing rather than testing the mask matters here, and
                // for CR4 specifically. This VMM masks VMXE for itself, so
                // *every* CR4 write by a second-level guest exits whether
                // or not the guest hypervisor asked. Reflecting all of
                // them because its mask happens to be non-empty would hand
                // it exits for bits it does not own, and a hypervisor that
                // trusts the architecture will act on the ones it sees.
                //
                // The register the value is in is named by bits 11:8, and
                // it is a *second-level* register - so it is in the
                // captured context, not in the VMCS. guest_register knows
                // that RSP is the exception.
                auto gpr = (qualification >> 8) & 0xf;
                auto written = guest_register(context, gpr);

                auto mask = (0 == number) ? cr0_mask : cr4_mask;

                // Against the *read shadow*, which is what the guest
                // hypervisor decided its guest should believe the
                // register holds - not against the register itself.
                //
                // SDM 28.1.3: "MOV to CR0 ... causes a VM exit unless the
                // value of its source operand matches, for the position
                // of each bit set in the CR0 guest/host mask, the
                // corresponding bit in the CR0 read shadow", and the same
                // sentence for CR4. KVM's nested_vmx_exit_handled_cr
                // spells it `vmcs12->cr0_guest_host_mask & (val ^
                // vmcs12->cr0_read_shadow)`.
                //
                // This used to compare against vmcs02's guest CR0, which
                // build_vmcs02 writes from vmcs12's *guest* field. The two
                // are different fields and are meant to differ - a guest
                // hypervisor owns a bit precisely so it can show its guest
                // something other than what the register holds. Wherever
                // they differed the decision was made on the wrong
                // operand, in both directions: an exit reflected that was
                // never wanted, and - the one that costs a boot - an exit
                // swallowed that the guest hypervisor was waiting for.
                auto shadow_value =
                    (0 == number) ? shadow.read(field::cr0_read_shadow)
                                  : shadow.read(field::cr4_read_shadow);

                return 0 != ((written ^ shadow_value) & mask);
            }
            case 3:
                return primary_set(primary_cr3_load_exiting);
            case 8:
                return primary_set(primary_cr8_load_exiting);
            default:
                return true;
            }
        case access_move_from:
            switch (number) {
            case 3:
                return primary_set(primary_cr3_store_exiting);
            case 8:
                return primary_set(primary_cr8_store_exiting);
            default:
                return true;
            }
        case access_clts: {
            // CLTS clears CR0.TS, so it exits to the guest hypervisor only
            // if that bit is one it owns *and* one it is showing as set.
            //
            // SDM 28.1.3: "The CLTS instruction causes a VM exit if the
            // bits in position 3 (corresponding to CR0.TS) are set in both
            // the CR0 guest/host mask and the CR0 read shadow." 28.3
            // gives the other half: mask set and shadow clear means CLTS
            // "completes but does not change the contents of CR0.TS" - no
            // exit. Testing the mask alone reflected that case too. KVM:
            // nested.c case 2, `(mask & X86_CR0_TS) && (read_shadow &
            // X86_CR0_TS)`.
            constexpr std::uint64_t task_switched = 1ull << 3;
            auto shadow_value = shadow.read(field::cr0_read_shadow);

            return (0 != (cr0_mask & task_switched)) &&
                   (0 != (shadow_value & task_switched));
        }
        case access_lmsw:
        default: {
            // LMSW writes CR0's low four bits, and whether that exits
            // depends on the value it would write - which is in the
            // qualification, not in a register.
            //
            // SDM 28.1.3 splits it in two, because "LMSW never clears bit
            // 0 of CR0 (CR0.PE)":
            //
            //   - PE exits only if the bit is set in both the mask and the
            //     source operand while clear in the read shadow. A source
            //     with PE clear cannot clear it, so it is not a change.
            //   - bits 3:1 exit if the mask owns the bit and the source
            //     and the read shadow disagree about it.
            //
            // SDM Table 28-3 puts the source data in bits 31:16 of the
            // exit qualification. KVM builds the same two-part test in
            // nested_vmx_exit_handled_cr's LMSW case.
            //
            // This used to return "any of the low four bits is owned",
            // which reflects every LMSW a second-level guest executes
            // whatever it writes.
            constexpr std::uint64_t protection_enable = 1ull << 0;
            constexpr std::uint64_t lmsw_upper_bits = 0xeull;

            auto source = (qualification >> 16) & 0xffff;
            auto shadow_value = shadow.read(field::cr0_read_shadow);

            if ((0 != (cr0_mask & protection_enable)) &&
                (0 != (source & protection_enable)) &&
                (0 == (shadow_value & protection_enable))) {
                return true;
            }

            return 0 !=
                   (cr0_mask & lmsw_upper_bits & (source ^ shadow_value));
        }
        }
    }

    case basic_reason::io_instruction: {
        // SDM Table 28-5: bits 2:0 the size, bit 3 the direction, bit 4
        // string, bit 5 REP, bit 6 operand encoding, bits 31:16 the port.
        // The bitmaps win when both are set. SDM 28.1.3
        // (.references/sdm.txt:200725) puts it in parentheses: "the
        // 'unconditional I/O exiting' VM-execution control is ignored if
        // the 'use I/O bitmaps' VM-execution control is 1". Testing
        // unconditional first reflected every I/O instruction to a guest
        // hypervisor that had set both - including the ports it had
        // explicitly cleared in its own bitmap - and both controls are
        // offered, so that is a configuration it can reach.
        if (!primary_set(primary_io_bitmaps)) {
            return primary_set(primary_unconditional_io);
        }

        auto qualification = vmcs.exit_qualification();
        auto port = static_cast<std::uint32_t>(qualification >> 16);
        auto size = static_cast<std::uint32_t>((qualification & 0x7) + 1);

        // Every byte of the access is checked, because a wide access whose
        // first port is not intercepted may still touch one that is. KVM's
        // `nested_vmx_exit_handled_io` walks the same range.
        for (std::uint32_t i{}; i < size; ++i) {
            auto at = port + i;

            // A wrapping access exits, it does not stop being checked.
            // SDM 28.1.3 (.references/sdm.txt:200724): "If an I/O
            // operation 'wraps around' the 16-bit I/O-port space
            // (accesses ports FFFFH and 0000H), the I/O instruction
            // causes a VM exit." Breaking out of the loop answered
            // "not intercepted" for a four-byte access at port 0xffff
            // against an empty bitmap; KVM returns true the moment the
            // port reaches 0x10000.
            if (at > 0xffff) {
                return true;
            }

            auto base = (at < 0x8000) ? shadow.read(field::io_bitmap_a)
                                      : shadow.read(field::io_bitmap_b);
            auto bit = at & 0x7fff;

            std::uint8_t byte{};
            auto read = read_guest_physical(
                base + (bit / 8),
                std::span(reinterpret_cast<std::byte *>(&byte), 1));

            // A bitmap this VMM cannot read is treated as intercepting.
            // The guest hypervisor named the page; if it is unreadable
            // that is its problem to see, and reflecting shows it.
            if (!read || (0 != (byte & (1u << (bit % 8))))) {
                return true;
            }
        }

        return false;
    }

    case basic_reason::rdmsr:
    case basic_reason::wrmsr: {
        if (!primary_set(primary_msr_bitmaps)) {
            // No bitmap means every MSR access exits, which is what the
            // guest hypervisor asked for.
            return true;
        }

        auto index = static_cast<std::uint32_t>(context.rcx);
        auto write = basic_reason::wrmsr == reason.basic();

        std::size_t base{};
        std::uint32_t bit{};

        if (index < 0x2000) {
            base = write ? 0x800 : 0x000;
            bit = index;
        } else if ((index >= 0xc0000000) && (index < 0xc0002000)) {
            base = write ? 0xc00 : 0x400;
            bit = index - 0xc0000000;
        } else {
            // Outside both ranges the bitmap is not consulted and the
            // access exits unconditionally, so it is the guest
            // hypervisor's. SDM 28.1.3.
            return true;
        }

        std::uint8_t byte{};
        auto read = read_guest_physical(
            shadow.read(field::msr_bitmap) + base + (bit / 8),
            std::span(reinterpret_cast<std::byte *>(&byte), 1));

        return !read || (0 != (byte & (1u << (bit % 8))));
    }

    default:
        // Everything else is the guest hypervisor's, which is KVM's
        // default in `nested_vmx_l1_wants_exit` and is the safe direction:
        // the exits that reach here unconditionally - triple fault, task
        // switch, CPUID, INVD, XSETBV, every VMX instruction, invalid
        // guest state - are all ones it must see, and reflecting one it
        // did not expect is an error it can report where absorbing one it
        // was waiting for is a hang.
        //
        // The VMX instructions being reflected is what makes three levels
        // of nesting work: a guest hypervisor emulating them for its own
        // guest gets them, exactly as this VMM gets them from the level
        // above.
        return true;
    }
}

void hypervisor::save_l2_state(std::size_t cpu)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Everything unconditional first, in the same order it was loaded, so
    // that the two lists cannot drift apart.
    for (auto guest_field : guest_state_fields) {
        shadow.write(guest_field, vmcs.read(guest_field));
    }

    shadow.write(field::guest_rip, vmcs.guest_rip());
    shadow.write(field::guest_rsp, vmcs.guest_rsp());
    shadow.write(field::guest_rflags, vmcs.guest_rflags());
    shadow.write(field::guest_interruptibility_state,
                 vmcs.read(field::guest_interruptibility_state));

    // The activity state is reconstructed rather than read back, for the
    // states this VMM holds outside the VMCS.
    //
    // A second-level guest that ran is described by the field, and SDM
    // 30.3 saves it there; one that never ran is described only by
    // `l2_activity_state`, because `enter_or_park_l2` held it in root
    // operation instead of entering. `running_l2` is exactly that
    // distinction: it is set by the entry and cleared below.
    //
    // KVM composes the same field the same way and from the same kind of
    // record - `sync_vmcs02_to_vmcs12` writes HLT or wait-for-SIPI out of
    // `mp_state` and active otherwise
    // (.references/kvm/nested.c:4539-4544) - never out of hardware. It
    // can afford to: the only values it ever writes into the real field
    // are active, at reset and to undo a HLT hardware entered on its own
    // (.references/kvm/vmx.c:4922 and 1827, which are the only writes in
    // the tree).
    shadow.write(field::guest_activity_state,
                 this->running_l2[cpu]
                     ? vmcs.read(field::guest_activity_state)
                     : this->l2_activity_state[cpu]);

    // The control registers, put back through the same masks they were
    // built with. What the second-level guest owns is the real register;
    // what its hypervisor owns is what it last wrote into vmcs12, and
    // saving the real value over that would tell it its own masked bits
    // had changed underneath it. KVM's `vmcs12_guest_cr0` and
    // `vmcs12_guest_cr4` compose the same two halves.
    auto cr0_mask12 = shadow.read(field::cr0_guest_host_mask);
    auto cr4_mask12 = shadow.read(field::cr4_guest_host_mask);

    shadow.write(field::guest_cr0,
                 (vmcs.guest_cr0() & ~cr0_mask12) |
                     (shadow.read(field::guest_cr0) & cr0_mask12));

    // VMXE is removed on the way back for the same reason it was forced in
    // on the way out: the bit is this VMM's, and a guest hypervisor that
    // never set it in its own guest's CR4 must not find it there.
    shadow.write(field::guest_cr4,
                 ((vmcs.guest_cr4() & ~cr4_vmxe) & ~cr4_mask12) |
                     (shadow.read(field::guest_cr4) & cr4_mask12));

    // "IA-32e mode guest" is a guest state bit wearing a control's
    // clothing, and SDM 30.3 has a VM exit update it. KVM says the same
    // in `sync_vmcs02_to_vmcs12`.
    shadow.write(
        field::vm_entry_controls,
        (shadow.read(field::vm_entry_controls) & ~entry_ia32e_mode_guest) |
            (vmcs.vm_entry_controls() & entry_ia32e_mode_guest));

    // The three saved conditionally, on the guest hypervisor's own exit
    // controls. SDM 30.4, "Saving MSRs", and SDM 30.3 for DR7.
    auto exit12 = shadow.read(field::vm_exit_controls);

    if (0 != (exit12 & exit_save_debug_controls)) {
        shadow.write(field::guest_dr7, vmcs.guest_dr7());
        shadow.write(field::guest_ia32_debugctl,
                     vmcs.read(field::guest_ia32_debugctl));
    }

    if (0 != (exit12 & exit_save_ia32_pat)) {
        shadow.write(field::guest_ia32_pat,
                     vmcs.read(field::guest_ia32_pat));
    }

    if (0 != (exit12 & exit_save_ia32_efer)) {
        shadow.write(field::guest_ia32_efer,
                     vmcs.read(field::guest_ia32_efer));
    }

    // The other half of carrying it in. Unconditional for the same
    // reason the load above is: there is no "save IA32_BNDCFGS" exit
    // control to test - the processor always writes the field on exit -
    // so a guest hypervisor reading it back expects what its guest left
    // there, and anything else silently loses the guest's bounds
    // configuration across every exit.
    shadow.write(field::guest_ia32_bndcfgs,
                 vmcs.read(field::guest_ia32_bndcfgs));
}

void hypervisor::load_l1_host_state(std::size_t cpu)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    auto exit12 = shadow.read(field::vm_exit_controls);

    auto host_cr0_12 = shadow.read(field::host_cr0);
    auto host_cr4_12 = shadow.read(field::host_cr4);

    vmcs.guest_cr0(host_cr0_12);
    vmcs.guest_cr3(shadow.read(field::host_cr3));
    vmcs.guest_cr4(host_cr4_12 | cr4_vmxe);

    // The read shadows have to follow, or the guest hypervisor reads back
    // the state of its own guest. CR4's is the one that matters: VMXE is
    // in this VMM's mask and forced into the register above, so without
    // this the guest hypervisor would read a CR4 it never wrote.
    vmcs.cr0_read_shadow(host_cr0_12);
    vmcs.cr4_read_shadow(host_cr4_12);

    vmcs.guest_rip(shadow.read(field::host_rip));
    vmcs.guest_rsp(shadow.read(field::host_rsp));

    // SDM 30.5.4: "RFLAGS is cleared, except bit 1, which is always set".
    constexpr std::uint64_t rflags_reserved_one = 1ull << 1;
    vmcs.guest_rflags(rflags_reserved_one);

    // SDM 30.5.4 again: no blocking by STI or MOV SS, and the activity
    // state is active. A hypervisor arriving at its own exit handler is
    // running.
    vmcs.write(field::guest_interruptibility_state, 0);
    vmcs.write(field::guest_activity_state,
               arch::x86_64::vmx::activity_state::active);
    vmcs.write(field::guest_pending_debug_exceptions, 0);

    // SDM 30.5.3, "Loading Host Segment and Descriptor-Table Registers".
    // The selectors come from the host-state area; everything else about
    // each segment is fixed by the architecture rather than stored, which
    // is why these are constants rather than copies.
    //
    // Access-rights encoding, SDM Table 25-2: bits 3:0 type, bit 4 S, bits
    // 6:5 DPL, bit 7 P, bit 13 L, bit 14 D/B, bit 15 G, bit 16 unusable.
    constexpr std::uint64_t code_access_long = 0xa09b;
    constexpr std::uint64_t code_access_legacy = 0xc09b;
    constexpr std::uint64_t data_access = 0xc093;
    constexpr std::uint64_t task_access = 0x008b;
    constexpr std::uint64_t unusable_access = 0x10000;
    constexpr std::uint64_t flat_limit = 0xffffffff;
    constexpr std::uint64_t task_limit = 0x67;
    constexpr std::uint64_t descriptor_table_limit = 0xffff;

    auto in_ia32e_mode = 0 != (exit12 & exit_host_address_space_size);

    vmcs.guest_cs_selector(shadow.read(field::host_cs_selector));
    vmcs.guest_cs_base(0);
    vmcs.guest_cs_limit(flat_limit);
    vmcs.guest_cs_access_rights(in_ia32e_mode ? code_access_long
                                              : code_access_legacy);

    // The five data segments, all with the same fixed shape. FS and GS
    // are the two exceptions and only in the base, which the host-state
    // area has to carry because a long-mode descriptor cannot.
    struct data_segment
    {
        field selector;
        field vmcs_selector;
        field vmcs_base;
        field vmcs_limit;
        field vmcs_access;
        std::uint64_t base;
    };

    const data_segment data_segments[] = {
        {field::host_ss_selector,
         field::guest_ss_selector,
         field::guest_ss_base,
         field::guest_ss_limit,
         field::guest_ss_access_rights,
         0},
        {field::host_ds_selector,
         field::guest_ds_selector,
         field::guest_ds_base,
         field::guest_ds_limit,
         field::guest_ds_access_rights,
         0},
        {field::host_es_selector,
         field::guest_es_selector,
         field::guest_es_base,
         field::guest_es_limit,
         field::guest_es_access_rights,
         0},
        {field::host_fs_selector,
         field::guest_fs_selector,
         field::guest_fs_base,
         field::guest_fs_limit,
         field::guest_fs_access_rights,
         shadow.read(field::host_fs_base)},
        {field::host_gs_selector,
         field::guest_gs_selector,
         field::guest_gs_base,
         field::guest_gs_limit,
         field::guest_gs_access_rights,
         shadow.read(field::host_gs_base)},
    };

    for (const auto & segment : data_segments) {
        vmcs.write(segment.vmcs_selector, shadow.read(segment.selector));
        vmcs.write(segment.vmcs_base, segment.base);
        vmcs.write(segment.vmcs_limit, flat_limit);
        vmcs.write(segment.vmcs_access, data_access);
    }

    vmcs.guest_tr_selector(shadow.read(field::host_tr_selector));
    vmcs.guest_tr_base(shadow.read(field::host_tr_base));
    vmcs.guest_tr_limit(task_limit);
    vmcs.guest_tr_access_rights(task_access);

    // LDTR is unusable after a VM exit, whatever it was.
    vmcs.guest_ldtr_selector(0);
    vmcs.guest_ldtr_base(0);
    vmcs.guest_ldtr_limit(0);
    vmcs.guest_ldtr_access_rights(unusable_access);

    vmcs.guest_gdtr_base(shadow.read(field::host_gdtr_base));
    vmcs.guest_gdtr_limit(descriptor_table_limit);
    vmcs.guest_idtr_base(shadow.read(field::host_idtr_base));
    vmcs.guest_idtr_limit(descriptor_table_limit);

    vmcs.write(field::guest_ia32_sysenter_cs,
               shadow.read(field::host_ia32_sysenter_cs));
    vmcs.write(field::guest_ia32_sysenter_esp,
               shadow.read(field::host_ia32_sysenter_esp));
    vmcs.write(field::guest_ia32_sysenter_eip,
               shadow.read(field::host_ia32_sysenter_eip));

    // SDM 30.5.4: DR7 is set to 400H and IA32_DEBUGCTL to 0.
    constexpr std::uint64_t dr7_after_exit = 0x400;
    vmcs.guest_dr7(dr7_after_exit);
    vmcs.write(field::guest_ia32_debugctl, 0);

    // IA32_PAT and IA32_EFER are written to the *registers* rather than to
    // the guest-state area, and that is not a shortcut - it is the only
    // thing that works here. This VMM's own VM-entry controls do not load
    // either, so a value put in the field would never be read; the
    // architecture's "load IA32_PAT on VM exit" means the register, and
    // the register is what the guest hypervisor will be running with.
    //
    // The alternative - adding the two load controls to this VMM's own
    // VMCS - was rejected because it changes the VMCS that runs the
    // ordinary guest, which is the code path a working Windows boot
    // depends on, for the sake of a path that only exists with nested VMX
    // switched on.
    if (0 != (exit12 & exit_load_ia32_pat)) {
        arch::x86_64::wrmsr(arch::x86_64::msr::ia32_pat,
                            shadow.read(field::host_ia32_pat));
    }

    if (0 != (exit12 & exit_load_ia32_efer)) {
        arch::x86_64::wrmsr(
            arch::x86_64::msr::ia32_extended_feature_enable,
            shadow.read(field::host_ia32_efer));
    } else {
        // LMA and LME are not part of that control's remit. SDM 30.5,
        // ".references/sdm.txt:204822": "The LMA and LME bits in the
        // IA32_EFER MSR are each loaded with the setting of the 'host
        // address-space size' VM-exit control" - unconditionally, on
        // every VM exit, whether or not the whole MSR is being loaded.
        //
        // Today this is masked by an accident: vmcs02 inherits this
        // VMM's own exit controls, which do carry host address-space
        // size, so the hardware exit has already put both bits back to
        // one before this runs. It breaks for a 32-bit guest hypervisor
        // - one whose vmcs12 clears the control - which would be
        // resumed in long mode with a host state that says otherwise.
        //
        // KVM writes the same three ways round in load_vmcs12_host_state:
        // the load control if set, otherwise LMA|LME from the
        // host-address-space-size bit, otherwise cleared.
        constexpr std::uint64_t efer_lme = 1ull << 8;
        constexpr std::uint64_t efer_lma = 1ull << 10;

        auto efer = arch::x86_64::rdmsr(
            arch::x86_64::msr::ia32_extended_feature_enable);

        if (0 != (exit12 & exit_host_address_space_size)) {
            efer |= (efer_lme | efer_lma);
        } else {
            efer &= ~(efer_lme | efer_lma);
        }

        arch::x86_64::wrmsr(
            arch::x86_64::msr::ia32_extended_feature_enable, efer);
    }
}

hypervisor::l2_entry_outcome hypervisor::enter_or_park_l2(std::size_t cpu)
{
    namespace activity = arch::x86_64::vmx::activity_state;

    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // SDM 29.8 gives 33 for "VM-entry failure due to invalid guest state"
    // and sets bit 31 of the exit reason to say a VM entry failed. The
    // qualification is zero: 29.8 lists the four non-zero values and none
    // of them is the activity state, so "In most cases, the exit
    // qualification is cleared to 0" applies. KVM writes the same pair -
    // EXIT_REASON_INVALID_STATE with ENTRY_FAIL_DEFAULT, which is 0
    // (.references/kvm/nested.c:3568-3572).
    constexpr std::uint64_t entry_failure = 1ull << 31;
    auto refuse = [&](const char * why, std::uint64_t what) {
        log("cpu {} second level entry refused: {} ({})", cpu, why, what);
        reflect_l2_exit(cpu,
                        entry_failure |
                            static_cast<std::uint64_t>(
                                basic_reason::entry_invalid_guest_state),
                        0);

        // After the reflection, never before it: save_l2_state writes
        // this into vmcs12, and the record has to describe what the
        // processor is doing *now*, which is running the guest
        // hypervisor's own code.
        this->l2_activity_state[cpu] = activity::active;
        return l2_entry_outcome::reflected;
    };

    auto activity12 = shadow.read(field::guest_activity_state);

    // The first check of SDM 29.3.1.5, "Checks on Guest Non-Register
    // State": the field must name an activity state the implementation
    // supports, and IA32_VMX_MISC is where software is told which those
    // are. This VMM reports HLT and wait-for-SIPI and not shutdown - see
    // nested_vmx_capability_msr - so those three plus active are the
    // whole of what may be accepted. KVM's set is identical, in
    // `nested_check_guest_non_reg_state`
    // (.references/kvm/nested.c:3117-3119).
    if ((activity::active != activity12) &&
        (activity::hlt != activity12) &&
        (activity::wait_for_start_up_ipi != activity12)) {
        return refuse("activity state not supported", activity12);
    }

    this->l2_activity_state[cpu] = activity12;

    if (activity::wait_for_start_up_ipi != activity12) {
        // Active and HLT are entered in hardware, and HLT deliberately so.
        //
        // Forcing HLT to active would be worse than wrong, it would be
        // loud: a halted second-level guest would start executing
        // instructions at whatever RIP its hypervisor left in vmcs12.
        // KVM can force it because it has somewhere to put the vCPU -
        // `kvm_emulate_halt_noskip` blocks the thread until an event
        // arrives (.references/kvm/nested.c:3766-3779). This VMM has no
        // scheduler and nothing to block on: the event that ends a halt
        // is a physical interrupt or NMI, its host runs with interrupts
        // disabled and sets no "external-interrupt exiting", so root
        // operation can neither observe one nor deliver it.
        //
        // Nothing is lost by letting the processor sit there, and SDM
        // 29.7.2's own list is why: the active state and the HLT state
        // block exactly the same events - start-up IPIs and nothing else.
        // A processor in HLT still takes external interrupts and NMIs,
        // which is precisely how its hypervisor gets it back. That is not
        // true of the other two inactive states, which is why only they
        // are refused or held.
        vmcs.write(field::guest_activity_state, activity12);
        return l2_entry_outcome::entered;
    }

    // Wait-for-SIPI, which is the one this VMM must not hand to hardware.
    //
    // SDM 29.7.2: it "blocks external interrupts, non-maskable interrupts
    // (NMIs), INIT signals, and system-management interrupts (SMIs). Such
    // events do not cause VM exits if they arrive while a logical
    // processor is in the wait-for-SIPI state and in VMX non-root
    // operation" (.references/sdm.txt:203152). Only a start-up IPI ends
    // it. So a processor entered in it is gone as far as this VMM is
    // concerned - and unlike bare metal there is no guarantee it will
    // ever be sent one, because this VMM intercepts the guest's write to
    // the interrupt command register and may answer it itself.
    //
    // Two checks first, and they exist *because* the state is not handed
    // to hardware: the processor would have made them, and forcing the
    // field means it no longer does. Both are SDM 29.3.1.5 - "The
    // activity-state field must indicate the active state if the
    // interruptibility-state field indicates blocking by either MOV-SS or
    // by STI", and, for an entry that is injecting, "Wait-for-SIPI. No
    // events are allowed."
    constexpr std::uint64_t blocking_by_sti_or_mov_ss = 0x3;

    if (0 != (shadow.read(field::guest_interruptibility_state) &
              blocking_by_sti_or_mov_ss)) {
        return refuse("wait-for-sipi with blocking by sti or mov ss",
                      shadow.read(field::guest_interruptibility_state));
    }

    if (0 != (shadow.read(field::vm_entry_interruption_information_field) &
              interruption_valid)) {
        return refuse(
            "wait-for-sipi with an event to inject",
            shadow.read(field::vm_entry_interruption_information_field));
    }

    // Waited for in root operation instead, on the hand-off this VMM
    // already uses for its own processors coming out of an INIT. Reusing
    // it is the point: a sender that sees a target listening there
    // swallows the guest's write and hands the vector over, which is the
    // only delivery that works while the target is in root mode.
    if (auto vector = wait_for_l2_start_up_ipi(cpu)) {
        // What the hardware would have given the guest hypervisor: SDM
        // 28.2 makes a start-up IPI arriving in the wait-for-SIPI state a
        // VM exit, with the vector in the exit qualification. KVM
        // synthesises the identical exit from its own record of the same
        // state - `vmx_check_nested_events` reflects
        // EXIT_REASON_SIPI_SIGNAL with `apic->sipi_vector & 0xFF` when
        // mp_state is KVM_MP_STATE_INIT_RECEIVED
        // (.references/kvm/nested.c:4240-4243).
        //
        // vmcs12's activity state stays at wait-for-SIPI across it, which
        // save_l2_state does out of l2_activity_state above - and is what
        // KVM saves too, since the exit does not clear mp_state.
        log("cpu {} second level start-up ipi, vector {}", cpu, *vector);

        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::start_up_ipi),
            *vector);

        // And the record goes back to describing this processor rather
        // than the guest it was holding, again after the reflection so
        // that vmcs12 keeps the wait-for-SIPI above. It is read by
        // start_up_processor, which must not hand a second vector to a
        // mailbox nobody is spinning on any more - the processor is
        // running the guest hypervisor's exit handler now.
        this->l2_activity_state[cpu] = activity::active;
        return l2_entry_outcome::reflected;
    }

    // Nothing yet. Back to the guest hypervisor with its own VMCS current
    // and RIP still on the VMLAUNCH, so it executes it again and this
    // decision is taken afresh.
    //
    // Not a spin that goes nowhere. It is what keeps this processor
    // reachable: every pass runs the exit handler, so the diagnostic
    // channel is fed, the poll is re-armed and the log records that this
    // processor is parked rather than lost. The alternative - waiting
    // here for ever - is the same darkness the hardware state produces,
    // only in root mode.
    auto region = own_vmcs_region_physical();
    if ((0 == region) || arch::x86_64::vmx::vmptrld(&region)) {
        // Same reasoning as reflect_l2_exit: without its own VMCS there is
        // no guest hypervisor left to go back to.
        __builtin_trap();
    }

    // Counted rather than logged. A log line per pass would be thousands
    // a second and would evict everything else in the ring; a count says
    // the same thing and says it about every pass. See the declaration
    // for what reading it settles.
    this->l2_start_up_waits[cpu] = this->l2_start_up_waits[cpu] + 1;

    return l2_entry_outcome::retry;
}

void hypervisor::reflect_l2_exit(std::size_t cpu,
                                 arch::x86_64::vmx::exit_reason reason,
                                 std::uint64_t qualification)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // The guest state first, while the VMCS that ran the second-level
    // guest is still current - unless this is a VM-entry failure, which
    // saves none of it.
    //
    // SDM 29.8 lists what an entry failure does *not* do, and the third
    // item is "The guest-state area is not modified." KVM's is the same
    // shape: `nested_vmx_enter_non_root_mode`'s `vmentry_fail_vmexit`
    // label calls `load_vmcs12_host_state` and writes the exit reason,
    // and reaches no `sync_vmcs02_to_vmcs12` on that path
    // (.references/kvm/nested.c:3640-3651).
    //
    // Not a tidy-up. It is what makes an entry failure reflectable from
    // a point where the second-level guest never ran and vmcs02 may not
    // even be the current VMCS: save_l2_state reads whichever VMCS *is*
    // current, so on that path it would copy the guest hypervisor's own
    // registers into its guest's state area.
    if (!reason.entry_failure()) {
        save_l2_state(cpu);
    }

    shadow.write(field::exit_reason, reason.value());
    shadow.write(field::exit_qualification, qualification);

    if (!reason.entry_failure()) {
        // SDM 33.3, VMLAUNCH: the launch state becomes launched once an
        // entry has completed, and an entry that failed after loading
        // guest state never reached that step. KVM writes the same
        // condition in `prepare_vmcs12`.
        shadow.state(vmcs12::launch_state::launched);

        // SDM 30.2: the rest of the exit-information fields are written
        // only for an ordinary exit. On an entry failure the architecture
        // updates the reason and the qualification and leaves the others
        // alone, so writing them would fabricate an account of an
        // instruction that never ran.
        shadow.write(field::guest_linear_address,
                     vmcs.read(field::guest_linear_address));
        shadow.write(field::guest_physical_address,
                     vmcs.read(field::guest_physical_address));
        shadow.write(field::vm_exit_interruption_information,
                     vmcs.read(field::vm_exit_interruption_information));
        shadow.write(field::vm_exit_interruption_error_code,
                     vmcs.read(field::vm_exit_interruption_error_code));
        shadow.write(field::vm_exit_instruction_length,
                     vmcs.read(field::vm_exit_instruction_length));
        shadow.write(field::vm_exit_instruction_information,
                     vmcs.read(field::vm_exit_instruction_information));

        // SDM 30.2.4, "Information for VM Exits During Event Delivery".
        // Copied from the hardware's own report rather than reconstructed,
        // because everything the second-level guest was in the middle of
        // delivering was put there by an entry this VMM built out of
        // vmcs12 - so the processor's account is the guest hypervisor's
        // account. The rule that a double or triple fault is never
        // reported as occurring during delivery is the processor's to
        // apply, and it applied it.
        shadow.write(field::idt_vectoring_information_field,
                     vmcs.read(field::idt_vectoring_information_field));
        shadow.write(field::idt_vectoring_error_code,
                     vmcs.read(field::idt_vectoring_error_code));

        // SDM 30.2: the valid bit of the VM-entry interruption-information
        // field is cleared on every VM exit. Emulated rather than read
        // back, because the field being read back is vmcs02's and the one
        // the guest hypervisor will next look at is vmcs12's.
        shadow.write(
            field::vm_entry_interruption_information_field,
            shadow.read(field::vm_entry_interruption_information_field) &
                ~interruption_valid);
    }

    // SDM 30.4: the VM-exit MSR-store area is processed after the guest
    // state is saved and before host state is loaded, so it reads the
    // values the second-level guest was running with.
    //
    // Skipped on an entry failure for the same reason the guest state is,
    // and from the same list: SDM 29.8's "No MSRs are saved into the
    // VM-exit MSR-store area." The MSR-*load* area below is not on that
    // list - 29.8 step 4 performs it - so only this half moves.
    auto aborted = false;

    if (!reason.entry_failure()) {
        if (auto stored = store_nested_msrs(
                shadow.read(field::vm_exit_msr_store_address),
                shadow.read(field::vm_exit_msr_store_count));
            !stored) {
            aborted = true;
            log("cpu {} could not store the guest hypervisor's exit "
                "msrs: {}",
                cpu,
                stored.error().code());
        }
    }

    // Back onto the VMCS that runs the guest hypervisor.
    auto region = own_vmcs_region_physical();
    if ((0 == region) || arch::x86_64::vmx::vmptrld(&region)) {
        // Not recoverable: without its own VMCS there is no guest
        // hypervisor to return to and nothing to resume. Same reasoning as
        // vmcs::write.
        __builtin_trap();
    }

    this->running_l2[cpu] = false;

    // Anything queued for the second-level guest is dropped here, which
    // vmcs02's own field holding it makes automatic: the next entry
    // rewrites it from vmcs12, and vmcs12's valid bit was just cleared.
    load_l1_host_state(cpu);

    // SDM 30.6: and the VM-exit MSR-load area after host state, which is
    // why this is here rather than beside the store above.
    if (auto loaded =
            load_nested_msrs(cpu,
                             shadow.read(field::vm_exit_msr_load_address),
                             shadow.read(field::vm_exit_msr_load_count));
        !loaded) {
        aborted = true;
        log("cpu {} could not load the guest hypervisor's exit msrs: {}",
            cpu,
            loaded.error().code());
    }

    nested_transition_flush();

    this->l2_exits_reflected[cpu] = this->l2_exits_reflected[cpu] + 1;

    // A failure in either exit area is a VMX abort, SDM 30.4 and SDM 30.6.
    // A real abort is a shutdown, which is not something to do to a whole
    // machine because one guest's hypervisor named a page that stopped
    // being readable - so this takes KVM's answer in `nested_vmx_abort`
    // instead and kills the *guest hypervisor's* virtual machine, by
    // giving it the triple fault its own guest would have taken. That is
    // reachable only through unreadable guest memory: every other reason
    // an entry in one of those areas can fail was checked before the
    // second-level guest ever ran.
    if (aborted) {
        this->guest_vmcs12[cpu].write(
            field::exit_reason,
            static_cast<std::uint64_t>(basic_reason::triple_fault));
        this->guest_vmcs12[cpu].write(field::exit_qualification, 0);
    }
}

hypervisor::l2_exit_outcome
hypervisor::on_l2_ept_fault(std::size_t cpu,
                            arch::x86_64::vmx::exit_reason reason,
                            arch::x86_64::context & context,
                            bool & advance_rip)
{
    auto & vmcs = this->vmcs;
    auto & shadow = this->guest_vmcs12[cpu];

    // Nothing retired: the faulting access has not happened yet, and the
    // whole point of every outcome below is to let it happen or to hand
    // the fault to whoever can explain it.
    advance_rip = false;

    auto guest_physical = vmcs.guest_physical_address();
    auto qualification = vmcs.exit_qualification();

    auto primary12 =
        shadow.read(field::primary_processor_based_vm_execution_controls);
    auto secondary12 =
        (0 != (primary12 & primary_secondary_controls))
            ? shadow.read(
                  field::secondary_processor_based_vm_execution_controls)
            : std::uint64_t{};

    // Without extended page tables of its own the guest hypervisor's guest
    // runs on this VMM's, so the address that faulted is one of this VMM's
    // own guest-physical addresses and the ordinary handler is the right
    // one. That is the same code path the guest hypervisor's own faults
    // take, which is what makes it correct rather than convenient.
    if (0 == (secondary12 & secondary_enable_ept)) {
        return l2_exit_outcome::deferred;
    }

    auto eptp12 = shadow.read(field::ept_pointer);

    // Walk the guest hypervisor's tables for the address the processor
    // reported, which is a second-level guest-physical one. The processor
    // walked the *shadow*, so its account of which permissions were
    // missing describes neither side's table on its own.
    auto guest_walk = arch::x86_64::vmx::walk_ept(
        eptp12 & (((1ull << 52) - 1) & ~0xfffull),
        guest_physical,
        physical_address_bits(),
        execute_only_translations_offered,
        [&](std::uint64_t at) -> std::optional<arch::x86_64::vmx::epte> {
            std::uint64_t value{};
            auto read = read_guest_physical(
                at,
                std::span(reinterpret_cast<std::byte *>(&value),
                          sizeof(value)));
            if (!read) {
                return std::nullopt;
            }
            return arch::x86_64::vmx::epte(value);
        });

    auto composition = arch::x86_64::vmx::compose_ept(
        guest_walk,
        host_ept_lookup(guest_walk.physical_address),
        execute_only_translations_offered);

    switch (composition.outcome) {
    case arch::x86_64::vmx::ept_compose_outcome::reflect_violation:
        // The guest hypervisor's own tables do not map it. Its guest would
        // have taken this exact fault on bare metal, so it gets it - with
        // a qualification synthesised from the walk of *its* tables rather
        // than forwarded from the shadow's, which is the whole of
        // reflected_ept_violation_qualification's reason for existing.
        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::ept_violation),
            arch::x86_64::vmx::reflected_ept_violation_qualification(
                qualification, guest_walk, false));
        return l2_exit_outcome::reflected;

    case arch::x86_64::vmx::ept_compose_outcome::reflect_misconfiguration:
        // Its tables hold a value the processor rejects. Reflected for the
        // same reason, and with no qualification, because SDM 30.2.1 does
        // not list EPT misconfiguration among the exits that save one.
        reflect_l2_exit(
            cpu,
            static_cast<std::uint64_t>(basic_reason::ept_misconfiguration),
            0);
        return l2_exit_outcome::reflected;

    case arch::x86_64::vmx::ept_compose_outcome::composed: {
        // "Composed" means the intersection is non-empty, NOT that it
        // permits what faulted, and the difference is a watched page.
        //
        // A watch clears write and keeps read and execute - see the
        // `write(false)` in the watch setup - so a second-level guest
        // writing a watched page composes to a perfectly valid read and
        // execute mapping. `compose_ept` reports `host_denied` only when
        // the intersection is *empty*, which that is not, so the write
        // arrived here, a read-only leaf was installed, the write was
        // resumed, and it faulted again on the leaf just installed.
        //
        // Measured, and it is what ends a Windows boot under Hyper-V:
        // the last eight exits before the guest gave up were all exit
        // reason 48 at one unchanging RIP with qualification 0x1aa - a
        // write, to a page reported readable and executable but not
        // writable - and 411,333 shadow leaves had been installed for
        // 88,281 second-level entries. One leaf per fault, no progress.
        // The watched pages are the local APIC page and the disk
        // controller's registers, which a guest writes constantly.
        //
        // So the access decides, not the intersection - and then which
        // side of the composition lacked it decides who answers.
        auto access_read = 0 != (qualification & (1ull << 0));
        auto access_write = 0 != (qualification & (1ull << 1));
        auto access_fetch = 0 != (qualification & (1ull << 2));

        auto permits =
            [&](const arch::x86_64::vmx::ept_permissions & permissions) {
                return (!access_read || permissions.read()) &&
                       (!access_write || permissions.write()) &&
                       (!access_fetch || permissions.execute());
            };

        // Which table refused it decides who answers, and the two are not
        // interchangeable.
        //
        // The guest hypervisor's tables mapping the address is not the
        // same as their permitting the access. Removing write from a page
        // it has mapped is how a hypervisor watches one - it is what this
        // VMM does to the local APIC page, and what Hyper-V does to its
        // guest's - and the intersection with ours is then still
        // non-empty, so it arrives here as `composed` rather than as a
        // walk failure. Attributing that to this VMM hands its guest's
        // trap to the wrong level: the watched-page machinery below would
        // emulate an access the guest hypervisor was waiting to be told
        // about, and a page only *it* watches would find nothing here
        // watching it and stop the processor.
        //
        // So the guest hypervisor's own permissions are tested first. Its
        // guest would have taken this fault on bare metal, which is the
        // same test the walk-failure case above applies, and the
        // qualification is synthesised the same way - bits 3, 4 and 5 come
        // from the walk of *its* tables, which is exactly what it needs to
        // see to know which permission it removed.
        if (!permits(guest_walk.permissions)) {
            reflect_l2_exit(
                cpu,
                static_cast<std::uint64_t>(basic_reason::ept_violation),
                arch::x86_64::vmx::reflected_ept_violation_qualification(
                    qualification, guest_walk, false));
            return l2_exit_outcome::reflected;
        }

        // Left over: the guest hypervisor permits it and the composition
        // does not, so the permission missing is this VMM's own.
        if (!permits(composition.permissions)) {
            if (!on_ept_violation(
                    cpu, context, guest_walk.physical_address)) {
                log("cpu {} second level {} to {} at first level {}, "
                    "which nothing here watches",
                    cpu,
                    access_write ? "write" : "access",
                    guest_physical,
                    guest_walk.physical_address);
                record_exit(reason);
                on_unhandled_exit(reason);
            }

            return l2_exit_outcome::handled;
        }

        // Both levels permit it, so the shadow is behind and this is not
        // a fault at all - it is a mapping that has to be put there.
        //
        // Rebuilding when the source or the generation moved is not
        // enough, and the reason is architectural rather than a bug in
        // the rebuild: a guest hypervisor that changes an EPT entry from
        // not-present to present **is not required to invalidate
        // anything**. SDM 31.4.3.3, "Guidelines for Use of the INVEPT
        // Instruction", requires invalidation when an entry is made
        // *more restrictive* and lists making one less restrictive among
        // the cases where software may skip it. So a shadow built from a
        // walk never learns about a page mapped after it was built,
        // nothing moves the source or the generation, and the access is
        // resumed to fault identically for ever.
        //
        // Measured on the rig, and it is what a Windows guest does within
        // seconds of Hyper-V launching: exit reason 48 filling the boot
        // processor's whole exit ring, qualification 0x184 - an
        // instruction fetch, linear address valid - at one unchanging
        // guest RIP, 1,062,627 exits, while the second level managed a
        // hundred entries in total. The comment that used to be here
        // called a loop the way a genuine bug would show itself. This is
        // that loop, and the bug is the assumption above it.
        //
        // So the faulting mapping is installed rather than the whole
        // shadow rebuilt. One leaf, not eleven thousand regions - the
        // rebuild is still there for what it is for, which is the source
        // or the generation actually moving.
        auto pointer = shadow_ept_pointer_for(cpu, eptp12);
        if (!pointer) {
            log("cpu {} shadow ept rebuild failed after an l2 fault at "
                "{}: error {}",
                cpu,
                guest_physical,
                pointer.error().code());
            record_exit(reason);
            on_unhandled_exit(reason);
            return l2_exit_outcome::handled;
        }

        vmcs.ept_pointer(*pointer);

        auto page =
            guest_physical & ~((1ull << composition.page_shift) - 1);

        if (auto installed = fill_shadow_leaf(
                cpu, page, guest_walk, composition.page_shift);
            !installed) {
            log("cpu {} could not install a shadow leaf for {}: error {}",
                cpu,
                page,
                installed.error().code());
            record_exit(reason);
            on_unhandled_exit(reason);
            return l2_exit_outcome::handled;
        }

        // The shadow's entry for this address has just changed from
        // permitting nothing to permitting something, and this processor
        // may hold the old one. Locally only: no other processor can have
        // cached a translation through a shadow that is this one's alone.
        invalidate_ept_locally();

        this->shadow_ept_leaves_filled[cpu] =
            this->shadow_ept_leaves_filled[cpu] + 1;

        return l2_exit_outcome::handled;
    }

    case arch::x86_64::vmx::ept_compose_outcome::host_denied:
    default:
        // This VMM's own tables refused it, so the guest hypervisor's are
        // innocent and must not be told otherwise - it would go looking
        // for a bug in tables that permit the access.
        //
        // Which leaves the watched-page machinery, keyed on this VMM's own
        // guest-physical addresses. The walk of the guest hypervisor's
        // tables just produced one, so it is handed over rather than taken
        // from the VMCS: the field the processor wrote holds a
        // second-level address, which no watch is keyed on.
        //
        // Opening a watched page changes this VMM's own tables and so
        // bumps the generation the shadow was built against. Nothing is
        // done about that here, because nothing needs to be: the resumed
        // access faults again, the composition then says `composed`, and
        // the branch above rebuilds. It costs a rebuild per stepped write
        // and is paid only by a guest hypervisor whose guest touches a
        // page this VMM watches - which is the disk channel's queue, owned
        // by the first-level guest's own operating system.
        //
        // A page nothing watches stops the processor, exactly as the
        // first-level guest's own access to the module does. BACKLOG.md
        // records that as a limit rather than a design.
        if (!on_ept_violation(cpu, context, guest_walk.physical_address)) {
            log("cpu {} second level touched {} at first level {}, which "
                "nothing here watches",
                cpu,
                guest_physical,
                guest_walk.physical_address);
            record_exit(reason);
            on_unhandled_exit(reason);
        }

        return l2_exit_outcome::handled;
    }
}

hypervisor::l2_exit_outcome
hypervisor::on_l2_exit(std::size_t cpu,
                       arch::x86_64::vmx::exit_reason reason,
                       arch::x86_64::context & context,
                       bool & advance_rip)
{
    // SDM 29, step 5: the launch state becomes launched only after the
    // guest-state checks and the MSR loads have passed, so an entry
    // failure leaves it where it was and the next attempt has to be a
    // VMLAUNCH again.
    if (!reason.entry_failure()) {
        this->vmcs02_launched[cpu] = true;
    } else {
        // The processor accepted the controls and the host state, loaded
        // the guest state the guest hypervisor wrote, and then found it
        // inconsistent. That is its guest's state and its own account to
        // receive: SDM 29.8 delivers it as a VM exit with bit 31 of the
        // reason set, and the guest hypervisor's own error handling is
        // written around exactly that.
        log("cpu {} second level entry failed after loading guest state, "
            "reason {} qualification {}",
            cpu,
            reason.value(),
            this->vmcs.exit_qualification());

        reflect_l2_exit(cpu, reason, this->vmcs.exit_qualification());
        advance_rip = false;
        return l2_exit_outcome::reflected;
    }

    // Extended page-table faults are decided by composing the two levels
    // rather than by the table below, because which level refused the
    // access is not something the exit itself says.
    if ((basic_reason::ept_violation == reason.basic()) ||
        (basic_reason::ept_misconfiguration == reason.basic())) {
        auto outcome = on_l2_ept_fault(cpu, reason, context, advance_rip);
        if (l2_exit_outcome::deferred != outcome) {
            this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        }
        return outcome;
    }

    // Two questions in order, which is KVM's shape in
    // `nested_vmx_reflect_vmexit` and is the order that matters: an exit
    // this VMM must have is one no reflection may take away, and only
    // after that does the guest hypervisor's own configuration decide.
    if (l0_wants_l2_exit(cpu, reason, context)) {
        this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        return l2_exit_outcome::deferred;
    }

    if (!l1_wants_l2_exit(cpu, reason, context)) {
        // Neither side asked for it, which can only happen where this
        // VMM's own controls are wider than the union it built - the
        // MONITOR and MWAIT intercepts are the two. The ordinary handler
        // answers them the same way it does for any guest.
        this->l2_exits_handled[cpu] = this->l2_exits_handled[cpu] + 1;
        return l2_exit_outcome::deferred;
    }

    reflect_l2_exit(cpu, reason, this->vmcs.exit_qualification());
    advance_rip = false;
    return l2_exit_outcome::reflected;
}

} // namespace zpp::hypervisor
