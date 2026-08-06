#pragma once
#include "zpp/nvme/dma_reachability.h"
#include "zpp/nvme/iommu_gate.h"

/**
 * The reachability strategy that asks the guest for a hole rather than
 * taking one.
 *
 * The mechanism is in two halves that do not share a line of code, and
 * only the second half is here.
 *
 * The first half runs in the loader, before anything is chainloaded. It
 * allocates a small window as `EfiReservedMemoryType` and injects a
 * Reserved Memory Region Reporting structure describing it, scoped to the
 * storage controller, into a rewritten copy of the DMAR table. VT-d 5.20
 * 3.16 says what that buys: "for legacy compatibility, system software is
 * expected to setup identity mapping in second-stage translation (with
 * read and write privileges) for these reserved address ranges, for the
 * specified devices", and "the system software is also responsible for
 * ensuring that any input addresses used for device accesses to
 * OS-visible memory do not overlap with the reserved system memory
 * address ranges". That second sentence is the one that matters: the
 * range is not merely mapped, it is *excluded from allocation*, so no
 * driver of the guest's can ever be handed it. See RESERVED-REGION.md and
 * uefi_loader/include/zpp/reserved_region.h.
 *
 * This half runs resident, once the guest owns the remapping hardware,
 * and answers whether the first half worked. Three outcomes:
 *
 * - The guest reserved the range *and* identity mapped it, which is what
 *   3.16 asks of it. Nothing to do.
 * - The guest reserved the range and did not map it. The leaf is
 *   installed. Safe only because of the reservation, which is why this
 *   strategy and the injection are one thing described in two files
 *   rather than two independent features.
 * - Anything else. Refused, and named.
 *
 * The refusal is the important case rather than the failure case. Firmware
 * has not been observed to declare a reserved region for a storage
 * controller, so a machine where the guest simply ignores an injected one
 * is entirely plausible - and the whole reason `dma_reachability.h` is a
 * contract with two implementations behind it.
 */
namespace zpp::nvme
{
struct reserved_region_strategy
{
    /**
     * What the resident side has to be told, since none of it can be
     * discovered from here.
     *
     * The register block comes from the loader's own DMAR parse - no
     * ACPI parsing lands on the resident side, which is the division of
     * labour NVME-LOG.md already sets for the disk sink. The translation
     * callback is how a guest-owned table's physical address becomes
     * readable.
     */
    struct hardware
    {
        const volatile void * registers{};
        std::uint8_t bus{};
        std::uint8_t device{};
        std::uint8_t function{};
        std::uint64_t (*physical_to_virtual)(std::uint64_t){};
    };

    /**
     * Where the remapping hardware and the device are.
     *
     * Namespace-scope-equivalent storage with constant initialization, so
     * it costs no `.init_array` entry and there is no guard byte for two
     * processors to race on - the shape CLAUDE.md asks for in anything
     * reachable from a VM exit. Written once, from the boot processor,
     * before the guest is running.
     */
    static inline constinit hardware unit{};

    /**
     * The last verdict `ensure_reachable` reached, in full detail.
     *
     * `reachability` has three values on purpose and that is the right
     * granularity for a caller deciding whether to submit. It is the
     * wrong granularity for a person asked why a machine went quiet, so
     * the cause is kept beside it.
     */
    static inline constinit reach_verdict cause{
        reach_verdict::no_hardware};

    /**
     * Points this at a unit and a device.
     */
    static constexpr void configure(const hardware & where)
    {
        unit = where;
    }

    /**
     * The contract, as `dma_reachability.h` states it.
     *
     * Read-only on every path that ends in `refused`, and the one path
     * that writes writes a single quadword that was zero. Issues no
     * invalidation; see `iommu_gate::install_leaf` for why none is
     * required and why it would be worse than merely unnecessary.
     *
     * Cheap enough to be the guard read before every doorbell, which is
     * what makes it a defence rather than a boot-time assumption: it is
     * re-walked from the root table address register each time, so a
     * relocated root table, a rewritten context entry after a function
     * level reset, a deleted domain and a change of translation table
     * mode all turn into counted refusals instead of stray DMA.
     */
    static reachability ensure_reachable(const dma_window & window)
    {
        iommu_gate::reach_request request{
            .registers = unit.registers,
            .bus = unit.bus,
            .device = unit.device,
            .function = unit.function,
            .physical_to_virtual = unit.physical_to_virtual,
            .window_physical = window.physical,
            .window_length = window.length,
            .install = true,
        };

        cause = iommu_gate::reach(request);

        if (reachable_now(cause)) {
            return reachability::reachable;
        }
        if (made_reachable_now(cause)) {
            return reachability::made_reachable;
        }
        return reachability::refused;
    }

    /**
     * The same question without the write, for a first look at an
     * unfamiliar machine and for the loader-side proof. Never returns
     * `made_reachable`, since nothing was made anything.
     */
    static reachability probe(const dma_window & window)
    {
        iommu_gate::reach_request request{
            .registers = unit.registers,
            .bus = unit.bus,
            .device = unit.device,
            .function = unit.function,
            .physical_to_virtual = unit.physical_to_virtual,
            .window_physical = window.physical,
            .window_length = window.length,
            .install = false,
        };

        cause = iommu_gate::reach(request);
        return reachable_now(cause) ? reachability::reachable
                                    : reachability::refused;
    }

    /**
     * A short name for the conclusion.
     *
     * Deliberately coarse - it says what the caller may do. `explain()`
     * below says why, and a channel reporting one without the other is
     * reporting half of an answer.
     */
    static constexpr const char * describe(reachability result)
    {
        switch (result) {
        case reachability::reachable:
            return "reserved region: reachable";
        case reachability::made_reachable:
            return "reserved region: mapping installed";
        case reachability::refused:
            return "reserved region: refused";
        }
        return "reserved region: unknown";
    }

    /**
     * Why the last conclusion was what it was.
     */
    static constexpr const char * explain()
    {
        return iommu_gate::describe(cause);
    }
};

} // namespace zpp::nvme
