#pragma once
#include "zpp/arch/x86_64/mmio.h"

#include <cstdint>

/**
 * Whether this controller's DMA will reach the addresses we give it.
 *
 * VT-d translates a device's DMA regardless of which software programmed
 * the device, so a translating domain that does not contain our buffers
 * stops the channel with every controller register still reading healthy.
 *
 * **`verdict_for` is read only, and that is the whole point.** Arming the
 * channel to find out whether its DMA lands is not a benign experiment:
 * DMA outside a reserved region after ExitBootServices can raise bugcheck
 * 0xE6 subcode 0x26 with no Driver Verifier involved. The question is
 * answered by reading the remapping hardware's own tables before a single
 * descriptor is handed over.
 *
 * `reach` below goes one step further and will, under conditions it
 * refuses to relax, install a single not-present leaf entry. That is
 * still not a licence to widen the guest's exposure by guesswork: it is
 * only safe because a reserved memory region injected into the DMAR table
 * before the guest booted made the guest reserve that address range, so
 * nothing of the guest's can be there. Every path that does not end in
 * exactly that situation refuses, and every refusal has its own name so
 * the channel can say which one it was. See RESERVED-REGION.md.
 *
 * Register offsets, bit positions and the table walk are taken from
 * Linux's own driver rather than from recall:
 * drivers/iommu/intel/iommu.h for the offsets and the translation type
 * constants, iommu_context_addr() in drivers/iommu/intel/iommu.c for the
 * root and context walk, and dmar_fault_dump_ptes() plus pgtable_walk()
 * in the same file for the scalable-mode and paging-structure walk.
 * Quoted at each use below. Field layouts are additionally checked
 * against the Intel Virtualization Technology for Directed I/O
 * Architecture Specification, revision 5.20 (order number D51397-019),
 * cited by section and table number - that revision and no other, since
 * an unverified citation is worse than none.
 */
namespace zpp::nvme
{
/**
 * What the remapping hardware says about one device.
 */
enum class translation_verdict
{
    /**
     * Translation is off entirely, or the device sits in a pass-through
     * domain. Our physical addresses are what the controller will use.
     * @{
     */
    disabled,
    pass_through,
    /**
     * @}
     */

    /**
     * Translated through second level page tables that are not ours.
     * Refused: our addresses are meaningless to the device.
     */
    translated,

    /**
     * The root or context entry is not present, so DMA from the device
     * is blocked outright. Cannot be the state of a disk the guest is
     * running from, but it is not our place to assume that.
     */
    blocked,

    /**
     * Scalable mode. Refused rather than guessed at, and not as a hedge:
     * iommu_context_addr() does `devfn *= 2` in that mode because
     * scalable context entries are 32 bytes rather than 16, and the
     * pass-through decision moves into the PASID table entry - so bits
     * 3:2 of the context entry no longer mean what we would be reading.
     */
    scalable_mode,

    /**
     * No remapping hardware was described to us at all.
     */
    no_hardware,

    /**
     * Caching Mode is set, so this is an emulated unit and installing a
     * mapping would need an invalidation issued through a queue the
     * guest owns. VT-d 4.0 6.1.
     */
    emulated,

    /**
     * The unit requires an explicit write buffer flush through the
     * global command register. VT-d 4.0 6.8.
     */
    needs_write_buffer_flush,

    /**
     * Translation Table Mode is 11b, in which hardware aborts every
     * DMA request. VT-d 4.0 3.4.4. Transient during the enable
     * sequence, so this means ask again rather than give up.
     */
    abort_dma,
};

/**
 * Whether a verdict permits handing the controller our addresses.
 */
constexpr bool safe_for_dma(translation_verdict verdict)
{
    return (translation_verdict::disabled == verdict) ||
           (translation_verdict::pass_through == verdict) ||
           (translation_verdict::no_hardware == verdict);
}

/**
 * The full answer for one address range rather than for one device.
 *
 * `translation_verdict` above answers "is this device translated", which
 * was the right question while nothing was allowed to be done about it.
 * It is the wrong question now: `translated` is not one situation but
 * several, and they differ in whether the range we care about is already
 * in the domain, is absent from an otherwise complete set of tables, or
 * is absent because the tables themselves are not there.
 *
 * One enumerator per cause, deliberately, and more of them than is
 * comfortable. A channel that stays quiet has to be able to say *which*
 * refusal it was, and collapsing two causes into one name is how a
 * machine that could have worked gets written off as one that could not.
 */
enum class reach_verdict
{
    /**
     * Already reachable, nothing was written.
     *
     * `no_translation` is translation switched off or no unit at all;
     * `pass_through` is a domain that does not translate this device;
     * `identity_mapped` is a leaf that already maps our own physical
     * address with read and write - which is what a guest that honoured
     * the reserved region produced by itself, and is the outcome to hope
     * for.
     * @{
     */
    no_translation,
    pass_through,
    identity_mapped,
    /**
     * @}
     */

    /**
     * The leaf was not present, the whole path above it was, and this
     * installed it. The only value that means state was changed.
     */
    installed,

    /**
     * The leaf was not present and installing was not asked for. The
     * read-only probe's way of saying "this would have worked".
     */
    would_install,

    /**
     * Refusals, one per cause. Every one of these leaves the tables
     * exactly as they were found.
     * @{
     */

    /** No unit, no register block, or a version register reading as
     * nothing. */
    no_hardware,

    /** The caller's window is not a whole number of 4 KB pages, is
     * empty, or is longer than this walker will consider. */
    bad_window,

    /** The window lies above what the unit's page tables can address -
     * either beyond CAP.MGAW or beyond the domain's own AGAW. */
    window_out_of_range,

    /** CAP.CM is set. Installing would need an invalidation issued
     * through a queue whose tail register the guest owns. */
    caching_mode,

    /** CAP.RWBF is set. Installing would need a write buffer flush
     * through a shared one-shot register the guest also owns. */
    write_buffer_flush,

    /** Translation Table Mode 11b, in which hardware aborts every DMA
     * request. Transient during the enable sequence, so this one means
     * ask again rather than give up. */
    abort_dma,

    /** Translation Table Mode 10b, which is reserved. */
    reserved_mode,

    /** RTADDR.SSIRWE is set, so permission - and with it the only
     * available test for whether an entry is present at all - lives in
     * bits 62:61 rather than bits 1:0. Refused rather than implemented,
     * because getting it wrong is the one mistake this whole header
     * exists to avoid. */
    io_read_write_permissions,

    /** A table's physical address did not translate to anything this
     * can read. */
    unmapped_table,

    /** The root entry for this bus is not present, so DMA from every
     * device on it is blocked. */
    root_not_present,

    /** The context entry for this device is not present, so its DMA is
     * blocked outright. */
    context_not_present,

    /** A field this walker does not implement - a reserved translation
     * type, or an address width outside the three the architecture
     * defines. */
    unsupported_configuration,

    /** Scalable mode, and the PASID directory entry for the device's
     * RID_PASID is not present. */
    pasid_directory_not_present,

    /** Scalable mode, and the PASID table entry is not present. */
    pasid_entry_not_present,

    /** Scalable mode with first-stage-only translation. There are no
     * second-stage tables to install into, and the first-stage ones
     * are a page table of the guest's own process address space. */
    first_stage_only,

    /** Scalable mode with nested translation. The second stage is
     * reachable but a first stage translates before it, so an identity
     * second-stage entry does not make our physical address usable. */
    nested_translation,

    /** An intermediate paging level is missing. **Refused rather than
     * allocated**: a table of ours hung off the guest's tree is a page
     * the guest will walk and free into its own allocator the moment
     * the domain is torn down. */
    intermediate_missing,

    /** The leaf is present and maps something that is not our window.
     * Never modified - only 0 to present is ever written. */
    mapped_elsewhere,

    /** The leaf is present, maps our window, and does not carry write
     * permission. Upgrading it would be modifying a present entry, and
     * VT-d 5.20 6.5.3.3 Table 28's heading note requires invalidation
     * for exactly that. */
    mapped_read_only,

    /** A large page covers our window and maps something else. Splitting
     * it would be modifying a present entry, twice over. */
    covered_by_superpage,
    /**
     * @}
     */
};

/**
 * Whether a verdict means the window is reachable now.
 */
constexpr bool reachable_now(reach_verdict verdict)
{
    return (reach_verdict::no_translation == verdict) ||
           (reach_verdict::pass_through == verdict) ||
           (reach_verdict::identity_mapped == verdict);
}

/**
 * Whether a verdict means this call made it reachable.
 */
constexpr bool made_reachable_now(reach_verdict verdict)
{
    return reach_verdict::installed == verdict;
}

/**
 * The remapping hardware unit's register block, and the walk from it to
 * one device's context entry.
 *
 * A class template on nothing but its own presence flag, so that a build
 * with the channel off emits none of it - the idiom counter.h documents.
 */
class iommu_gate
{
public:
    /**
     * Offsets within a DRHD register block.
     *
     * Linux drivers/iommu/intel/iommu.h:
     *     #define DMAR_VER_REG    0x0
     *     #define DMAR_CAP_REG    0x8
     *     #define DMAR_ECAP_REG   0x10
     *     #define DMAR_GSTS_REG   0x1c
     *     #define DMAR_RTADDR_REG 0x20
     * @{
     */
    static constexpr std::uint32_t version_offset = 0x00;
    static constexpr std::uint32_t capability_offset = 0x08;
    static constexpr std::uint32_t extended_capability_offset = 0x10;
    static constexpr std::uint32_t global_status_offset = 0x1c;
    static constexpr std::uint32_t root_address_offset = 0x20;
    /**
     * @}
     */

    /**
     * Translation Enable Status, in the global status register.
     *
     *     #define DMA_GSTS_TES (((u32)1) << 31)
     */
    static constexpr std::uint32_t translation_enabled = 1u << 31;

    /**
     * Translation Table Mode, in the root table address register.
     *
     * Bits 11:10, not bit 10 alone. Linux's own DMA_RTADDR_SMT tests the
     * single bit, which is enough to answer "is this scalable mode" and
     * is not enough to answer anything else - and the value this code
     * has to notice most is 11b, abort-DMA, in which VT-d 4.0 3.4.4 has
     * hardware block *all* translation requests. That is a real state
     * during the recommended enable sequence, so seeing it means the
     * answer is "ask again later" rather than "no translation".
     * @{
     */
    static constexpr std::uint64_t translation_mode_shift = 10;
    static constexpr std::uint64_t translation_mode_mask = 0x3;
    static constexpr std::uint64_t mode_legacy = 0;
    static constexpr std::uint64_t mode_scalable = 1;
    static constexpr std::uint64_t mode_abort_dma = 3;
    /**
     * @}
     */

    /**
     * Second Stage I/O Read/Write Enable, root table address register
     * bit 7.
     *
     * VT-d 5.20 11.4.5, added in this revision: "0: Hardware uses bit 0
     * (R) and bit 1 (W) in second-stage paging entries to calculate
     * effective permissions. 1: Hardware uses bit 61 (IR) and bit 62
     * (IW)".
     *
     * This is not one more permission model to support, it is a change
     * to the only test this header has for whether an entry is present.
     * There is no P bit in a second-stage entry: `dma_pte_present()`
     * reads `(pte->val & 3) != 0`, and with SSIRWE set that reads a
     * *live* entry as empty - so the install path would overwrite a
     * present mapping of the guest's, which is the single outcome every
     * other rule here is written to prevent. Refused, and named.
     *
     * Read in both modes even though VT-d 5.20 7.2 makes it a
     * programming error in legacy mode (fault RTA.1.4, "the SSIRWE field
     * is set when the TTM field is programmed to legacy mode"): a unit
     * in that state is misprogrammed, and a misprogrammed unit is not
     * one to start writing tables for. Hardware without Second Stage
     * I/O Read/Write Support reports the bit as Reserved(0), so this
     * costs a machine nothing that has not deliberately turned it on.
     */
    static constexpr std::uint64_t io_read_write_enable_bit = 1ull << 7;

    /**
     * Caching Mode, capability register bit 7.
     *
     * Set means not-present entries are cached, so installing a mapping
     * needs an explicit invalidation - and the only channel for one is a
     * queue whose tail register the guest's operating system owns.
     *
     * It also means this is not real hardware. VT-d 4.0 6.1: "Hardware
     * implementations of this architecture must support operation
     * corresponding to CM=0. Operation corresponding to CM=1 may be
     * supported by software implementations (emulation)."
     */
    static constexpr std::uint64_t caching_mode_bit = 1ull << 7;

    /**
     * Required Write-Buffer Flushing, capability register bit 4.
     *
     * Set means VT-d 4.0 6.8 requires an explicit write buffer flush
     * through the global command register after modifying a not-present
     * entry. That register is a shared one-shot whose other fields would
     * have to be reconstructed from status, so a unit reporting this is
     * refused rather than driven. Clear on every modern part.
     */
    static constexpr std::uint64_t write_buffer_flushing_bit = 1ull << 4;

    /**
     * Translation types in a context entry.
     *
     *     #define CONTEXT_TT_MULTI_LEVEL  0
     *     #define CONTEXT_TT_DEV_IOTLB    1
     *     #define CONTEXT_TT_PASS_THROUGH 2
     *
     * The field is bits 3:2, which context_set_translation_type() pins
     * down beyond doubt:
     *
     *     context->lo &= (((u64)-1) << 4) | 3;
     *     context->lo |= (value & 3) << 2;
     * @{
     */
    static constexpr std::uint64_t translation_type_shift = 2;
    static constexpr std::uint64_t translation_type_mask = 0x3;
    static constexpr std::uint64_t type_pass_through = 2;
    /**
     * @}
     */

    /**
     * Asks one remapping unit about one device.
     *
     * `registers` is the DRHD register block, already mapped uncacheable
     * by the caller. `physical_to_virtual` is how a table's physical
     * address becomes something this can read - the tables belong to the
     * guest's operating system and are ordinary memory.
     *
     * Every read here is a read. Nothing is written, nothing is
     * invalidated, and no state is left behind.
     */
    static translation_verdict
    verdict_for(const volatile void * registers,
                std::uint8_t bus,
                std::uint8_t device,
                std::uint8_t function,
                std::uint64_t (*physical_to_virtual)(std::uint64_t))
    {
        if (!registers || !physical_to_virtual) {
            return translation_verdict::no_hardware;
        }

        auto at = [registers](std::uint32_t offset) {
            return static_cast<const volatile std::uint8_t *>(registers) +
                   offset;
        };

        // A unit whose version register reads as nothing is not there.
        // Same reasoning as the CAP check on the controller's own BAR:
        // an unassigned or unmapped window reads 0 or all ones, and
        // either would otherwise be interpreted as "translation off".
        auto version = arch::x86_64::read32(at(version_offset));
        if ((0 == version) || (0xffffffffu == version)) {
            return translation_verdict::no_hardware;
        }

        auto status = arch::x86_64::read32(at(global_status_offset));
        if (0 == (status & translation_enabled)) {
            return translation_verdict::disabled;
        }

        // Checked before anything is believed about the tables, because
        // both of these change what installing a mapping would cost and
        // neither is visible further down.
        auto capability = arch::x86_64::read64(at(capability_offset));
        if (0 != (capability & caching_mode_bit)) {
            return translation_verdict::emulated;
        }
        if (0 != (capability & write_buffer_flushing_bit)) {
            return translation_verdict::needs_write_buffer_flush;
        }

        auto root_address = arch::x86_64::read64(at(root_address_offset));
        auto mode = (root_address >> translation_mode_shift) &
                    translation_mode_mask;
        if (mode_abort_dma == mode) {
            return translation_verdict::abort_dma;
        }
        if (mode_scalable == mode) {
            return translation_verdict::scalable_mode;
        }
        if (mode_legacy != mode) {
            // 10b is reserved. Something is either wrong or newer than
            // this code, and both mean the walk below would be reading
            // the wrong shape of table.
            return translation_verdict::no_hardware;
        }

        // The root table is indexed by bus number with 16 byte entries,
        // and the low quadword's bit 0 is present with bits 63:12 the
        // context table pointer:
        //
        //     static phys_addr_t root_entry_lctp(struct root_entry *re)
        //     {
        //         if (!(re->lo & 1))
        //             return 0;
        //         return re->lo & VTD_PAGE_MASK;
        //     }
        constexpr std::uint64_t page_mask = ~0xfffull;
        auto root_table = root_address & page_mask;
        auto root_entry = reinterpret_cast<const volatile std::uint64_t *>(
            physical_to_virtual(root_table + (std::uint64_t{bus} * 16)));
        if (!root_entry) {
            return translation_verdict::no_hardware;
        }

        auto root_low = root_entry[0];
        if (0 == (root_low & 1)) {
            return translation_verdict::blocked;
        }

        // In legacy mode the context entry is indexed by devfn directly,
        // 16 bytes each - iommu_context_addr() only scales the index
        // when scalable mode is in use, which was refused above.
        auto devfn = static_cast<std::uint64_t>((device << 3) | function);
        auto context_table = root_low & page_mask;
        auto context_entry =
            reinterpret_cast<const volatile std::uint64_t *>(
                physical_to_virtual(context_table + (devfn * 16)));
        if (!context_entry) {
            return translation_verdict::no_hardware;
        }

        auto context_low = context_entry[0];
        if (0 == (context_low & 1)) {
            return translation_verdict::blocked;
        }

        auto type = (context_low >> translation_type_shift) &
                    translation_type_mask;
        if (type_pass_through == type) {
            return translation_verdict::pass_through;
        }

        return translation_verdict::translated;
    }

    /**
     * Extended capability register fields this needs.
     *
     * VT-d 5.20 11.4.3:
     * - bit 0, C, Page-walk Coherency. Clear means "hardware accesses to
     *   remapping structures are non-coherent", so a modified entry has
     *   to be pushed out of the processor's caches before the unit's
     *   page walker can see it.
     * - bit 7, SC, Snoop Control. Clear means hardware "does not support
     *   1-setting of the SNP field in the second-stage page-table
     *   entries", and the field is then reserved - so SNP is set only
     *   when this says it may be.
     * - bit 49, RPS, RID-PASID Support. Clear means hardware "uses the
     *   value of 0 for RID_PASID" regardless of the field's contents, so
     *   the field is read only when this is set.
     * @{
     */
    static constexpr std::uint64_t page_walk_coherency_bit = 1ull << 0;
    static constexpr std::uint64_t snoop_control_bit = 1ull << 7;
    static constexpr std::uint64_t rid_pasid_support_bit = 1ull << 49;
    /**
     * @}
     */

    /**
     * Second-stage paging entry bits, VT-d 5.20 Table 46 and Table 47.
     *
     * There is no present bit: an entry is not present when both R and W
     * are clear, which is what Linux's dma_pte_present() tests:
     *
     *     return (pte->val & 3) != 0;
     *
     * PS at bit 7 turns a non-leaf entry into a large page mapping, SNP
     * at bit 11 asks for the access to snoop processor caches, and the
     * address occupies bits (HAW-1):12.
     * @{
     */
    static constexpr std::uint64_t entry_read = 1ull << 0;
    static constexpr std::uint64_t entry_write = 1ull << 1;
    static constexpr std::uint64_t entry_present =
        entry_read | entry_write;
    static constexpr std::uint64_t entry_large_page = 1ull << 7;
    static constexpr std::uint64_t entry_snoop = 1ull << 11;
    /**
     * @}
     */

    /**
     * The address a second-stage paging entry holds: bits 51:12, and
     * not one bit more.
     *
     * Deliberately narrower than the `~0xfff` used for the root,
     * context and PASID structure pointers, and the difference is not
     * pedantry. Those describe their address as "bits 63:HAW are
     * reserved (0)", so the high bits are guaranteed zero and masking
     * off the low twelve is enough. A second-stage entry does not:
     * VT-d 5.20 Tables 41-47 give it "(HAW-1):12 ADDR", "51:HAW R:
     * Reserved (0)", "60:52 IGN: Ignored" and "63 IGN: Ignored" - nine
     * bits plus one that hardware ignores and *software may therefore
     * use*.
     *
     * These are the guest operating system's tables, not ours. Taking
     * an ignored bit for the address turns a leaf that maps exactly our
     * window into one that appears to map somewhere else, and the
     * `mapped_elsewhere` refusal that follows is a machine written off
     * over a flag some other software stored in a bit the architecture
     * set aside for it. Linux's `VTD_PAGE_MASK` is the wide one, which
     * is correct for Linux because Linux wrote every entry it reads.
     */
    static constexpr std::uint64_t entry_address_mask = 0x000ffffffffff000;

    /**
     * Nine bits of index per paging level, as on the processor side -
     * VT-d 5.20 9.8 says the entries "are bitwise compatible with the
     * Intel 64 processor's EPT paging entry format".
     *
     *     #define LEVEL_STRIDE (9)
     *     #define LEVEL_MASK   (((u64)1 << LEVEL_STRIDE) - 1)
     * @{
     */
    static constexpr std::uint64_t level_stride = 9;
    static constexpr std::uint64_t level_mask = (1ull << level_stride) - 1;
    /**
     * @}
     */

    /**
     * The largest window this will consider, in 4 KB pages.
     *
     * Not a resource limit - it is the bound that keeps a single call
     * cheap enough to run from the guard read before every doorbell,
     * which is the whole reason the answer can be trusted across a
     * function level reset or a domain rebuild that happened since the
     * last one. Sixteen pages is four times the window this was written
     * for, so the bound is not tight.
     */
    static constexpr std::uint64_t max_window_pages = 16;

    /**
     * Everything one question needs.
     *
     * A struct rather than nine parameters because two of them are
     * addresses of different address spaces and one is a permission to
     * write, and a positional argument list makes all three easy to get
     * wrong at a call site.
     */
    struct reach_request
    {
        /**
         * The DRHD register block, already mapped uncacheable.
         */
        const volatile void * registers{};

        /**
         * The device, as the remapping tables index it.
         * @{
         */
        std::uint8_t bus{};
        std::uint8_t device{};
        std::uint8_t function{};
        /**
         * @}
         */

        /**
         * How a table's physical address becomes something readable.
         * The tables belong to the guest's operating system and are
         * ordinary memory.
         */
        std::uint64_t (*physical_to_virtual)(std::uint64_t){};

        /**
         * The host physical range the device must reach, 4 KB aligned
         * with a length that is a whole number of 4 KB pages.
         * @{
         */
        std::uint64_t window_physical{};
        std::uint64_t window_length{};
        /**
         * @}
         */

        /**
         * Whether a not-present leaf may be installed. False makes this
         * a pure read, which is what the loader-side proof and any
         * first look at an unfamiliar machine should use.
         */
        bool install{};
    };

    /**
     * Answers `reach_request`, and acts on the answer when asked to.
     *
     * Safe to call repeatedly. Performs no DMA, issues no invalidation,
     * and writes nothing at all on any path that ends in a refusal.
     */
    static reach_verdict reach(const reach_request & request)
    {
        if (!request.registers || !request.physical_to_virtual) {
            return reach_verdict::no_hardware;
        }

        auto pages = window_pages(request);
        if (0 == pages) {
            return reach_verdict::bad_window;
        }

        auto at = [&request](std::uint32_t offset) {
            return static_cast<const volatile std::uint8_t *>(
                       request.registers) +
                   offset;
        };

        // Same reasoning as verdict_for: an unassigned or unmapped
        // window reads 0 or all ones, and either would be interpreted
        // as translation being off.
        auto version = arch::x86_64::read32(at(version_offset));
        if ((0 == version) || (0xffffffffu == version)) {
            return reach_verdict::no_hardware;
        }

        auto status = arch::x86_64::read32(at(global_status_offset));
        if (0 == (status & translation_enabled)) {
            return reach_verdict::no_translation;
        }

        auto capability = arch::x86_64::read64(at(capability_offset));
        if (0 != (capability & caching_mode_bit)) {
            return reach_verdict::caching_mode;
        }
        if (0 != (capability & write_buffer_flushing_bit)) {
            return reach_verdict::write_buffer_flush;
        }

        // CAP.MGAW, bits 21:16, is "the maximum guest physical address
        // width supported by second-stage translation", computed as
        // N+1. VT-d 5.20 11.4.2 has hardware block untranslated
        // requests to addresses above 2^(X+1)-1, so a window above it
        // cannot be reached however the tables are written.
        auto guest_address_width = ((capability >> 16) & 0x3f) + 1;
        auto last = request.window_physical + request.window_length - 1;
        if ((guest_address_width < 64) &&
            (last >= (1ull << guest_address_width))) {
            return reach_verdict::window_out_of_range;
        }

        // And separately, whatever MGAW says, above bit 51 there is no
        // address field left to write it into - Tables 41 to 47 stop
        // ADDR at 51 and call everything above it ignored. MGAW is 57
        // on shipping parts, so this is not implied by the check above.
        if (0 != (last & ~entry_address_mask & ~0xfffull)) {
            return reach_verdict::window_out_of_range;
        }

        auto extended =
            arch::x86_64::read64(at(extended_capability_offset));

        auto root_address = arch::x86_64::read64(at(root_address_offset));
        auto mode = (root_address >> translation_mode_shift) &
                    translation_mode_mask;
        if (mode_abort_dma == mode) {
            return reach_verdict::abort_dma;
        }
        if ((mode_legacy != mode) && (mode_scalable != mode)) {
            return reach_verdict::reserved_mode;
        }
        if (0 != (root_address & io_read_write_enable_bit)) {
            return reach_verdict::io_read_write_permissions;
        }

        constexpr std::uint64_t page_mask = ~0xfffull;
        auto devfn = static_cast<std::uint64_t>((request.device << 3) |
                                                request.function);

        // The second-stage page table root and its level, wherever the
        // mode in use happens to keep them. `why` carries the answer
        // whenever there is no walk to do - which includes pass-through,
        // where there is nothing to do because the window is already
        // reachable.
        std::uint64_t table{};
        std::uint32_t levels{};
        auto why = reach_verdict::no_hardware;
        auto located = (mode_legacy == mode)
                           ? locate_legacy(request,
                                           root_address & page_mask,
                                           devfn,
                                           table,
                                           levels,
                                           why)
                           : locate_scalable(request,
                                             extended,
                                             root_address & page_mask,
                                             devfn,
                                             table,
                                             levels,
                                             why);
        if (!located) {
            return why;
        }

        // An AGAW of L levels covers 9*L + 12 address bits - 39, 48 or
        // 57 - so a window above that has no entry to live in even
        // though the hardware could address it.
        auto covered = (level_stride * levels) + 12;
        if (last >= (1ull << covered)) {
            return reach_verdict::window_out_of_range;
        }

        auto result = reach_verdict::identity_mapped;
        for (std::uint64_t index{}; index < pages; ++index) {
            auto page = request.window_physical + (index * 0x1000);
            auto one = reach_page(request, extended, table, levels, page);
            if (reachable_now(one)) {
                continue;
            }
            if ((reach_verdict::installed == one) ||
                (reach_verdict::would_install == one)) {
                // An install anywhere in the window outranks the pages
                // that were already there, since it is the answer that
                // says state changed.
                result = one;
                continue;
            }
            return one;
        }
        return result;
    }

    /**
     * A short name for the verdict, for the line this channel writes
     * before it writes anything else. A machine where the channel cannot
     * work has to say so immediately rather than going quiet, which is
     * the third of DIAGNOSTICS.md's three rules applied to the
     * precondition rather than to the channel.
     */
    static constexpr const char * describe(translation_verdict verdict)
    {
        switch (verdict) {
        case translation_verdict::disabled:
            return "translation off";
        case translation_verdict::pass_through:
            return "pass-through";
        case translation_verdict::translated:
            return "translated - refused";
        case translation_verdict::blocked:
            return "blocked - refused";
        case translation_verdict::scalable_mode:
            return "scalable mode - refused";
        case translation_verdict::emulated:
            return "caching mode set, emulated - refused";
        case translation_verdict::needs_write_buffer_flush:
            return "write buffer flushing required - refused";
        case translation_verdict::abort_dma:
            return "abort dma mode - refused";
        case translation_verdict::no_hardware:
            return "no remapping hardware";
        }
        return "unknown";
    }

    /**
     * A short name for a reachability verdict. Same rule as above: a
     * refusal that cannot be named is a machine written off for a reason
     * nobody can look up.
     */
    static constexpr const char * describe(reach_verdict verdict)
    {
        switch (verdict) {
        case reach_verdict::no_translation:
            return "translation off - reachable";
        case reach_verdict::pass_through:
            return "pass-through - reachable";
        case reach_verdict::identity_mapped:
            return "already identity mapped - reachable";
        case reach_verdict::installed:
            return "leaf installed - made reachable";
        case reach_verdict::would_install:
            return "leaf absent, path present - would install";
        case reach_verdict::no_hardware:
            return "no remapping hardware";
        case reach_verdict::bad_window:
            return "window not whole 4 KB pages - refused";
        case reach_verdict::window_out_of_range:
            return "window above addressable range - refused";
        case reach_verdict::caching_mode:
            return "caching mode set, emulated - refused";
        case reach_verdict::write_buffer_flush:
            return "write buffer flushing required - refused";
        case reach_verdict::abort_dma:
            return "abort dma mode - ask again";
        case reach_verdict::reserved_mode:
            return "reserved translation table mode - refused";
        case reach_verdict::io_read_write_permissions:
            return "second stage io read/write permissions - refused";
        case reach_verdict::unmapped_table:
            return "a table did not translate - refused";
        case reach_verdict::root_not_present:
            return "root entry not present - refused";
        case reach_verdict::context_not_present:
            return "context entry not present - refused";
        case reach_verdict::unsupported_configuration:
            return "unsupported field value - refused";
        case reach_verdict::pasid_directory_not_present:
            return "pasid directory entry not present - refused";
        case reach_verdict::pasid_entry_not_present:
            return "pasid table entry not present - refused";
        case reach_verdict::first_stage_only:
            return "first stage only translation - refused";
        case reach_verdict::nested_translation:
            return "nested translation - refused";
        case reach_verdict::intermediate_missing:
            return "intermediate level missing - refused";
        case reach_verdict::mapped_elsewhere:
            return "leaf maps another address - refused";
        case reach_verdict::mapped_read_only:
            return "leaf lacks write permission - refused";
        case reach_verdict::covered_by_superpage:
            return "covered by a large page - refused";
        }
        return "unknown";
    }

private:
    /**
     * How many 4 KB pages the request covers, or zero if it does not
     * describe a whole number of them.
     *
     * VT-d 5.20 8.4 requires exactly this of a reserved memory region -
     * "the base address of each RMRR region must be 4KB aligned and the
     * size must be an integer multiple of 4KB" - so a window that fails
     * here could not have been declared to the guest either, and the
     * two checks are the same check in two places.
     */
    static constexpr std::uint64_t
    window_pages(const reach_request & request)
    {
        if ((0 == request.window_length) ||
            (0 != (request.window_physical & 0xfff)) ||
            (0 != (request.window_length & 0xfff))) {
            return 0;
        }
        auto pages = request.window_length / 0x1000;
        if (pages > max_window_pages) {
            return 0;
        }
        // A window that wraps the address space is not a window.
        if ((request.window_physical + request.window_length) <
            request.window_physical) {
            return 0;
        }
        return pages;
    }

    /**
     * Reads one quadword of a table whose address is physical.
     *
     * Returns false when the translation callback cannot produce a
     * mapping, which is a refusal rather than a zero: a table read as
     * zero would look like a not-present entry and send the caller down
     * an install path with no idea what is really there.
     */
    static bool read_table(const reach_request & request,
                           std::uint64_t physical,
                           std::uint64_t & value)
    {
        auto mapped = request.physical_to_virtual(physical);
        if (!mapped) {
            return false;
        }
        value = *reinterpret_cast<const volatile std::uint64_t *>(mapped);
        return true;
    }

    /**
     * Turns an AW field into a number of paging levels.
     *
     * VT-d 5.20 Table 40 and Table 48: 001b is a 39-bit AGAW with three
     * levels, 010b a 48-bit AGAW with four, 011b a 57-bit AGAW with
     * five, and everything else is reserved. Linux says the same thing
     * arithmetically, in agaw_to_level():
     *
     *     return agaw + 2;
     */
    static constexpr std::uint32_t levels_of(std::uint64_t address_width)
    {
        if ((address_width < 1) || (address_width > 3)) {
            return 0;
        }
        return static_cast<std::uint32_t>(address_width + 2);
    }

    /**
     * The legacy, TTM 00b, path from the root table to the second-stage
     * page tables.
     *
     * iommu_context_addr() scales the context index only in scalable
     * mode, so here the context entry is at `devfn` directly with 16
     * bytes each, and the low quadword's bits 63:12 are SSPTPTR with
     * bits 3:2 the translation type - VT-d 5.20 9.3, and
     * context_set_translation_type() in Linux pins the shift:
     *
     *     context->lo |= (value & 3) << 2;
     *
     * The address width is bits 66:64, which is bits 2:0 of the high
     * quadword. dmar_fault_dump_ptes() reads it the same way:
     *
     *     level = agaw_to_level(ctx_entry->hi & 7);
     */
    static bool locate_legacy(const reach_request & request,
                              std::uint64_t root_table,
                              std::uint64_t devfn,
                              std::uint64_t & table,
                              std::uint32_t & levels,
                              reach_verdict & why)
    {
        constexpr std::uint64_t page_mask = ~0xfffull;

        std::uint64_t root_low{};
        if (!read_table(request,
                        root_table + (std::uint64_t{request.bus} * 16),
                        root_low)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (root_low & 1)) {
            why = reach_verdict::root_not_present;
            return false;
        }

        auto context = (root_low & page_mask) + (devfn * 16);
        std::uint64_t context_low{};
        std::uint64_t context_high{};
        if (!read_table(request, context, context_low) ||
            !read_table(request, context + 8, context_high)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (context_low & 1)) {
            why = reach_verdict::context_not_present;
            return false;
        }

        auto type = (context_low >> translation_type_shift) &
                    translation_type_mask;
        if (type_pass_through == type) {
            why = reach_verdict::pass_through;
            return false;
        }
        // 00b and 01b both translate through SSPTPTR; 11b is reserved.
        if (type > type_pass_through) {
            why = reach_verdict::unsupported_configuration;
            return false;
        }

        levels = levels_of(context_high & 0x7);
        if (0 == levels) {
            why = reach_verdict::unsupported_configuration;
            return false;
        }
        table = context_low & page_mask;
        return true;
    }

    /**
     * The scalable, TTM 01b, path: root, context, PASID directory, PASID
     * table, and only then the second-stage tables.
     *
     * The shape is from dmar_fault_dump_ptes(); every field position is
     * checked against the specification because the Linux code reaches
     * most of them through accessors rather than by number.
     *
     * - The scalable root entry is 16 bytes with two halves. VT-d 5.20
     *   9.2: LP at bit 0 with LCTP at 63:12, UP at bit 64 with UCTP at
     *   127:76. Which half applies is the same test Linux makes:
     *
     *         if (devfn >= 0x80) { devfn -= 0x80; entry = &root->hi; }
     *         devfn *= 2;
     *
     *   `devfn *= 2` is the 32 byte context entry, and subtracting 0x80
     *   first is why the index below is `devfn & 0x7f`.
     * - The scalable context entry is 32 bytes. VT-d 5.20 9.4: P at bit
     *   0, PASIDDIRPTR at 63:12, PDTS at 11:9, RID_PASID at 83:64 -
     *   which is bits 19:0 of the second quadword.
     * - The PASID directory entry is 8 bytes, indexed by `pasid >> 6`
     *   (Linux PASID_PDE_SHIFT), P at bit 0 and the table pointer at
     *   63:12. VT-d 5.20 9.5.
     * - The PASID table entry is 64 bytes, indexed by `pasid & 0x3f`
     *   (Linux PASID_PTE_MASK). VT-d 5.20 9.6: P at bit 0, AW at 4:2,
     *   PGTT at 8:6, SSPTPTR at (HAW-1):12, all in the first quadword.
     *   Linux reads the last two the same way:
     *
     *         return (u16)((READ_ONCE(pte->val[0]) >> 6) & 0x7);
     *         level = agaw_to_level((pte->val[0] >> 2) & 0x7);
     *
     * RID_PASID is read rather than assumed zero, but only when ECAP.RPS
     * says the field exists - VT-d 5.20 11.4.3 says hardware without it
     * "uses the value of 0 for RID_PASID" whatever is written there, so
     * reading it unconditionally would follow a field hardware ignores.
     */
    static bool locate_scalable(const reach_request & request,
                                std::uint64_t extended,
                                std::uint64_t root_table,
                                std::uint64_t devfn,
                                std::uint64_t & table,
                                std::uint32_t & levels,
                                reach_verdict & why)
    {
        constexpr std::uint64_t page_mask = ~0xfffull;

        auto root = root_table + (std::uint64_t{request.bus} * 16);
        std::uint64_t half{};
        if (!read_table(request, root + ((devfn >= 0x80) ? 8 : 0), half)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (half & 1)) {
            why = reach_verdict::root_not_present;
            return false;
        }

        auto context = (half & page_mask) + ((devfn & 0x7f) * 32);
        std::uint64_t context_0{};
        std::uint64_t context_1{};
        if (!read_table(request, context, context_0) ||
            !read_table(request, context + 8, context_1)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (context_0 & 1)) {
            why = reach_verdict::context_not_present;
            return false;
        }

        std::uint64_t pasid{};
        if (0 != (extended & rid_pasid_support_bit)) {
            pasid = context_1 & 0xfffff;
        }

        // PDTS gives 2^(X+7) directory entries. An index past the end is
        // memory that is not the directory, and reading it would be
        // reading whatever the guest keeps after it.
        auto directory_entries = 1ull << (((context_0 >> 9) & 0x7) + 7);
        auto directory_index = pasid >> 6;
        if (directory_index >= directory_entries) {
            why = reach_verdict::unsupported_configuration;
            return false;
        }

        std::uint64_t directory_entry{};
        if (!read_table(request,
                        (context_0 & page_mask) + (directory_index * 8),
                        directory_entry)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (directory_entry & 1)) {
            why = reach_verdict::pasid_directory_not_present;
            return false;
        }

        std::uint64_t entry{};
        if (!read_table(request,
                        (directory_entry & page_mask) +
                            ((pasid & 0x3f) * 64),
                        entry)) {
            why = reach_verdict::unmapped_table;
            return false;
        }
        if (0 == (entry & 1)) {
            why = reach_verdict::pasid_entry_not_present;
            return false;
        }

        switch ((entry >> 6) & 0x7) {
        case 1:
            why = reach_verdict::first_stage_only;
            return false;
        case 2:
            break;
        case 3:
            why = reach_verdict::nested_translation;
            return false;
        case 4:
            why = reach_verdict::pass_through;
            return false;
        default:
            why = reach_verdict::unsupported_configuration;
            return false;
        }

        levels = levels_of((entry >> 2) & 0x7);
        if (0 == levels) {
            why = reach_verdict::unsupported_configuration;
            return false;
        }
        table = entry & page_mask;
        return true;
    }

    /**
     * Walks the second-stage tables for one 4 KB page, and installs the
     * leaf if that is the only thing missing.
     *
     * pgtable_walk() in Linux is the same descent, and the index is
     * pfn_level_offset():
     *
     *     return (pfn >> level_to_offset_bits(level)) & LEVEL_MASK;
     *     // level_to_offset_bits(level) == (level - 1) * LEVEL_STRIDE
     */
    static reach_verdict reach_page(const reach_request & request,
                                    std::uint64_t extended,
                                    std::uint64_t table,
                                    std::uint32_t levels,
                                    std::uint64_t page)
    {
        auto frame = page >> 12;

        auto parent = table;
        for (auto level = levels; level > 1; --level) {
            auto index =
                (frame >> ((level - 1) * level_stride)) & level_mask;
            std::uint64_t value{};
            if (!read_table(request, parent + (index * 8), value)) {
                return reach_verdict::unmapped_table;
            }
            if (0 == (value & entry_present)) {
                // Deliberately not allocated. A table of ours hung off
                // the guest's tree becomes the guest's to free.
                return reach_verdict::intermediate_missing;
            }
            if (0 != (value & entry_large_page)) {
                // Bit 7 is PS only two levels down. VT-d 5.20 Table 43
                // is the 1 GByte page at level 3 and Table 45 the
                // 2 MByte page at level 2; at level 4 and level 5 the
                // same bit is "7 R: Reserved (0)" - Tables 42 and 41.
                // Believing it there would invent a 512 GByte or
                // 256 TByte mapping out of a bit that should not have
                // been set, and then compare our window against its
                // imaginary base. A unit whose tables say that is not
                // one to write into.
                if (level > 3) {
                    return reach_verdict::unsupported_configuration;
                }

                // A large page already maps this. Whether it happens to
                // map our own address or not, it is a present entry and
                // present entries are never touched.
                auto base = value & entry_address_mask;
                auto size = 1ull << (((level - 1) * level_stride) + 12);
                auto offset = page - (page & ~(size - 1));
                if ((base + offset) != page) {
                    return reach_verdict::covered_by_superpage;
                }
                if (entry_present != (value & entry_present)) {
                    return reach_verdict::mapped_read_only;
                }
                return reach_verdict::identity_mapped;
            }
            parent = value & entry_address_mask;
        }

        auto slot = parent + ((frame & level_mask) * 8);
        std::uint64_t leaf{};
        if (!read_table(request, slot, leaf)) {
            return reach_verdict::unmapped_table;
        }

        if (0 != (leaf & entry_present)) {
            if ((leaf & entry_address_mask) != page) {
                return reach_verdict::mapped_elsewhere;
            }
            if (entry_present != (leaf & entry_present)) {
                return reach_verdict::mapped_read_only;
            }
            return reach_verdict::identity_mapped;
        }

        if (!request.install) {
            return reach_verdict::would_install;
        }

        return install_leaf(request, extended, slot, page);
    }

    /**
     * Writes one not-present leaf into a present one.
     *
     * The only write in this header, and every constraint on it is load
     * bearing:
     *
     * - It is only ever reached with the existing entry at zero. A
     *   present entry is never modified and nothing is ever unmapped.
     *   Leaving the mapping in place forever is the correct outcome;
     *   removing it is the direction that needs an invalidation.
     * - Read and write, and nothing else. VT-d 5.20 Table 47 puts R at
     *   bit 0 and W at bit 1, and bit 2 is IGN in every second-stage
     *   entry type - Tables 41 to 47. There is no execute permission to
     *   grant or withhold here, and Linux's dma_pte bits agree: READ,
     *   WRITE, LARGE_PAGE and SNP, with no EXEC.
     * - SNP at bit 11 only when ECAP.SC allows it. VT-d 5.20 Table 47:
     *   the field "is treated as reserved(0) by hardware implementations
     *   not supporting Snoop Control", and a reserved bit set in a
     *   present entry is a fault, not an ignored hint.
     * - CLFLUSH when ECAP.C is clear, because the unit's page walker
     *   then does not snoop and would read the stale line out of memory.
     *   Linux does the same thing behind __iommu_flush_cache().
     * - **No invalidation is issued, and that is the point.** VT-d 5.20
     *   6.5.3.3, the note heading Table 28: "Invalidations described in
     *   the table are required when the entry being changed is present
     *   or when Caching Mode (CM) is reported as 1." Neither holds here:
     *   the entry was not present and CAP.CM was refused above.
     *   6.5.3.4's ordering rules concern adding permissions with host
     *   permission tables enabled, which is a scalable-mode feature this
     *   does not touch. Linux agrees in code -
     *   cache_tag_flush_range_np() in drivers/iommu/intel/cache.c takes
     *   the `!cap_caching_mode(iommu->cap)` branch, flushes the write
     *   buffer and `continue`s, queuing no descriptor at all.
     *
     *   The reason this matters so much more than a paragraph of
     *   specification: the only channel for an invalidation is a queue
     *   whose tail register the guest's operating system owns. Appending
     *   to it is the step that has to be avoided, not merely optimised
     *   away.
     */
    static reach_verdict install_leaf(const reach_request & request,
                                      std::uint64_t extended,
                                      std::uint64_t slot,
                                      std::uint64_t page)
    {
        auto mapped = request.physical_to_virtual(slot);
        if (!mapped) {
            return reach_verdict::unmapped_table;
        }

        auto value = (page & entry_address_mask) | entry_present;
        if (0 != (extended & snoop_control_bit)) {
            value |= entry_snoop;
        }

        // Every other field is left at zero deliberately. Bit 2 is IGN
        // in all of Tables 41 to 47, so there is no execute permission
        // to grant or withhold. EMT at 5:3 and IPAT at 6 are "ignored
        // by hardware when Extended Memory Type Enable (EMTE) field is
        // Clear ... or when Translation Table Mode is set to legacy
        // mode", and a zero EMT under a domain that did enable EMTE
        // means uncacheable for this one page - slower for a device
        // access, and still coherent, so it is a cost rather than a
        // correctness question and not worth a refusal.
        auto * entry = reinterpret_cast<volatile std::uint64_t *>(mapped);
        *entry = value;

        if (0 == (extended & page_walk_coherency_bit)) {
            // A builtin rather than inline assembly, since generic code
            // outside the architecture layer does not write assembly and
            // there is no standard spelling of a cache line flush to
            // prefer over it.
            __builtin_ia32_clflush(reinterpret_cast<const void *>(mapped));
        }

        // Not order_stores(): that is a release fence, which on this
        // architecture is a compiler barrier and nothing more, and a
        // CLFLUSH is not ordered by one. This has to be the instruction.
        __builtin_ia32_sfence();

        return reach_verdict::installed;
    }
};

} // namespace zpp::nvme
