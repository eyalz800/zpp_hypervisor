#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/diag/config.h"
#include "zpp/nvme/dma_reachability.h"

#include <cstdint>

/**
 * Asking the guest to reserve a window for the storage controller, by
 * describing it in the remapping tables the firmware hands over.
 *
 * The problem this exists for is stated in DIAGNOSTICS.md and again in
 * NVME-LOG.md: remapping hardware translates a *device's* DMA regardless
 * of which software programmed the device, so a translating domain that
 * does not contain our buffers stops the channel with every controller
 * register still reading healthy. `zpp/nvme/iommu_gate.h` measures
 * whether that is happening. This is one of the two answers to it being
 * true.
 *
 * The architecture has a mechanism for exactly this and it is not a
 * loophole: VT-d 5.20 3.16 describes reserved system memory regions as
 * ranges "allocated by BIOS at boot time and reported to OS as reserved
 * address ranges in the system memory map", for which "system software is
 * expected to setup identity mapping in second-stage translation (with
 * read and write privileges) ... for the specified devices", and for
 * which "system software is also responsible for ensuring that any input
 * addresses used for device accesses to OS-visible memory do not overlap
 * with the reserved system memory address ranges". Section 8.4 gives the
 * structure that reports one.
 *
 * The second of those sentences is what makes this different from writing
 * an entry into the guest's page tables. A reservation is not a mapping
 * that happens to exist; it is an *exclusion from allocation*, re-applied
 * every time a domain is built. A hand-installed page table entry is
 * undone by the next domain teardown - a function level reset, a D3
 * cycle, a driver reload - and each of those happens without
 * announcement. The declared hole survives all of them because the guest
 * rebuilds it from the same table each time.
 *
 * What it costs the guest is exactly one range in one device's address
 * space, declared, visible in its own reporting, and nothing else. That
 * is why it is the default of the two strategies in
 * `zpp/nvme/dma_reachability.h`.
 *
 * What it rests on is the guest honouring the description. Firmware has
 * not been observed producing a reserved region for a storage controller,
 * so this is the architecture's mechanism used somewhere its consumers
 * have historically been graphics and USB. RESERVED-REGION.md says what
 * was proven and what is still argued.
 *
 * Debug only, gated on the disk sink being compiled in, exactly as
 * `nvme_selftest.h` is and for the same reason: "the channel is compiled
 * in" and "make the channel's DMA reach" are the same decision.
 */
namespace zpp
{
struct reserved_region
{
    /**
     * Whether this build carries it. The disk sink's own policy row.
     */
    static constexpr bool enabled =
        diag::policy_of(diag::sink::esp_blocks).present;

    /**
     * The window that was reserved, or a zero length one if nothing was.
     *
     * The resident side needs this and cannot derive it: the address is
     * whatever the firmware's allocator returned. Constant initialized,
     * so it costs no `.init_array` entry.
     */
    static inline constinit nvme::dma_window window{};

    /**
     * The controller the region was scoped to, so the resident side asks
     * about the same device the region names. Meaningful only when
     * `window.length` is non-zero.
     * @{
     */
    static inline constinit std::uint8_t bus{};
    static inline constinit std::uint8_t device{};
    static inline constinit std::uint8_t function{};
    /**
     * @}
     */

    /**
     * The register base of the remapping unit whose scope the controller
     * was found under, so the resident side does not have to parse ACPI.
     * Zero when the controller was not found under one - which is itself
     * a refusal, since VT-d 5.20 8.4 requires the devices an RMRR names
     * to be "devices under the scope of one of the remapping hardware
     * units reported in DRHD".
     */
    static inline constinit std::uint64_t unit_registers{};

    /**
     * Runs the whole thing and reports through `zpp::trace`. Never fails
     * the boot: every step refuses rather than propagating, because a
     * diagnostic that can stop a machine booting is worse than no
     * diagnostic. Must be called before the boot manager is started and
     * after the firmware has published its tables.
     *
     * An inline wrapper around `if constexpr` so a build with the channel
     * off has no call and no code, in the shape `zpp::trace`,
     * `zpp::verify` and `zpp::nvme_selftest` already use.
     */
    static void install(EFI_SYSTEM_TABLE * system_table)
    {
        if constexpr (enabled) {
            execute(system_table);
        } else {
            static_cast<void>(system_table);
        }
    }

private:
    /**
     * The implementation, out of line so the header costs nothing.
     * Defined only when enabled - see reserved_region.cpp.
     */
    static void execute(EFI_SYSTEM_TABLE * system_table);
};

} // namespace zpp
