#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/diag/config.h"
#include "zpp/nvme/owned_iommu_strategy.h"

#include <cstddef>
#include <span>

/**
 * Taking the remapping hardware away from the guest, at the only moment
 * it can be taken.
 *
 * **Read the cost before reading the code.** Unlinking `DMAR` removes the
 * guest's DMA remapping wholesale. Windows will report Kernel DMA
 * Protection as off, every device it would have isolated becomes able to
 * reach all of memory as far as the guest's own configuration goes, and
 * nothing the guest does can put that back - it cannot configure hardware
 * it cannot see. `OWNED-IOMMU.md` states the cost at length and states
 * why the project keeps this as the fallback rather than the default.
 *
 * What replaces the guest's protection is our own: the resident side
 * programs every unit itself with an identity map that excludes the
 * hypervisor module, so the machine is not left unprotected, it is left
 * protected by us and coarsely. Coarse is the honest word. There is one
 * domain and every device is in it.
 *
 * The half done here is the half that has to happen while ACPI is still
 * ours to edit:
 *
 * - Record where the remapping hardware is, from the table, before the
 *   table stops being reachable. Nothing on the resident side parses
 *   ACPI, which is the division of labour `NVME-LOG.md` already sets for
 *   this hardware.
 * - Remove the `DMAR` pointer from the `XSDT`, and from the `RSDT` when
 *   the root pointer's revision has one, fixing both checksums. A guest
 *   that never sees a `DMAR` never programs a unit and never enables
 *   interrupt remapping, which is what makes our own programming safe to
 *   leave in place.
 *
 * The table body itself is left where it is. Only the pointers to it are
 * removed, so nothing is freed and no length outside the two root tables
 * changes.
 */
namespace zpp
{
struct dmar_unlink
{
    /**
     * Whether this build carries it. The disk sink's own policy row, in
     * the shape `zpp/nvme_selftest.h` uses: the channel this exists to
     * make reachable is the same channel that decides the build carries
     * any of it.
     */
    static constexpr bool enabled =
        diag::policy_of(diag::sink::esp_blocks).present;

    /**
     * Most remapping units this records. Real platforms report one or
     * two; the development target reports one, a single `DRHD` at
     * `fed91000` with `INCLUDE_PCI_ALL`. A fixed bound rather than an
     * allocation, because this runs before anything else the loader does
     * and must not depend on the pool being in any particular state.
     */
    static constexpr std::size_t max_units = 8;

    /**
     * Records the remapping hardware and unlinks the table.
     *
     * Never fails the boot. Every outcome is traced and a refusal leaves
     * the tables exactly as the firmware wrote them - which means the
     * guest keeps its own remapping and the resident side finds no units
     * to adopt, so the two halves stay consistent with each other.
     *
     * An inline wrapper around `if constexpr` so a build with the channel
     * off has no call and no code.
     */
    static void run(EFI_SYSTEM_TABLE * system_table)
    {
        if constexpr (enabled) {
            execute(system_table);
        } else {
            static_cast<void>(system_table);
        }
    }

    /**
     * What was recorded, in the order the table listed it. Empty until
     * `run` has been called, and empty afterwards if the table was
     * absent or was refused - so a caller that finds it empty must not
     * assume the guest has been deprived of anything.
     */
    static std::span<const nvme::remapping_unit> units();

private:
    /**
     * The implementation, out of line so the header costs nothing.
     * Defined only when enabled - see dmar_unlink.cpp.
     */
    static void execute(EFI_SYSTEM_TABLE * system_table);
};

} // namespace zpp
