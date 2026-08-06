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
 * **This is read only, and that is the whole point.** Arming the channel
 * to find out whether its DMA lands is not a benign experiment: DMA
 * outside a reserved region after ExitBootServices can raise bugcheck
 * 0xE6 subcode 0x26 with no Driver Verifier involved. The question is
 * answered by reading the remapping hardware's own tables before a single
 * descriptor is handed over.
 *
 * Nothing here modifies the IOMMU. No DMAR unlinking, no RMRR injection,
 * no page table patching - DIAGNOSTICS.md lists those as options and
 * recommends measuring first, which is what this is.
 *
 * Register offsets, bit positions and the table walk are taken from
 * Linux's own driver rather than from recall:
 * drivers/iommu/intel/iommu.h for the offsets and the translation type
 * constants, and iommu_context_addr() in drivers/iommu/intel/iommu.c for
 * the walk. Quoted at each use below.
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
     * Scalable mode, in the root table address register.
     *
     *     #define DMA_RTADDR_SMT (((u64)1) << 10)
     */
    static constexpr std::uint64_t scalable_mode_bit = 1ull << 10;

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

        auto root_address = arch::x86_64::read64(at(root_address_offset));
        if (0 != (root_address & scalable_mode_bit)) {
            return translation_verdict::scalable_mode;
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
        case translation_verdict::no_hardware:
            return "no remapping hardware";
        }
        return "unknown";
    }
};

} // namespace zpp::nvme
