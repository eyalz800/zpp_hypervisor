#pragma once
extern "C" {
#include <Uefi.h>
}

#include "zpp/diag/config.h"

#include <cstdint>

/**
 * Where the guest writes when it puts the machine to sleep.
 *
 * A suspend takes this hypervisor away. Root mode does not survive S3,
 * and nothing restores it, so the machine resumes running the guest on
 * bare hardware with the guest none the wiser - BACKLOG item 7. Before
 * any of that can be fixed it has to be *seen*, and seeing it means
 * knowing which I/O port the guest writes to enter a sleep state.
 *
 * That port is only discoverable from ACPI, and only the loader can
 * discover it: the hypervisor has no table walker and, once resident,
 * nothing it could trust to point at one. So the loader reads it out of
 * the fixed ACPI description table and hands the address over, the same
 * way it hands over everything else the resident side cannot find for
 * itself.
 *
 * Reading only. Nothing here changes a table or arms anything - the
 * interception belongs on the other side of the hand-over.
 */
namespace zpp
{
/**
 * The sleep control registers, as the resident side needs them.
 *
 * Two blocks because ACPI allows the register to be split across two,
 * with the second optional and zero when absent. Both have to be
 * watched: a write to either can carry the enable bit.
 */
struct sleep_control
{
    /**
     * Set once every field below has been established, and checked
     * before any of them is believed. A zeroed structure reads as "not
     * found" rather than as port zero.
     */
    static constexpr std::uint32_t valid_magic = 0x504c5321; // 'SLP!'

    std::uint32_t magic{};

    /**
     * The two control blocks, as I/O port numbers. The second is zero
     * when the platform does not use one.
     */
    std::uint16_t pm1a_control_port{};
    std::uint16_t pm1b_control_port{};

    /**
     * How wide the register is, in bytes, from the table rather than
     * assumed - the specification allows two or four.
     */
    std::uint8_t control_width{};

    constexpr bool usable() const
    {
        return (valid_magic == magic) && (0 != pm1a_control_port) &&
               (0 != control_width);
    }
};

/**
 * Finds the sleep control registers, or leaves the result unusable.
 *
 * Compiles to nothing unless the diagnostic facility is present, since
 * the only consumer of the answer is behind the same switch.
 */
struct sleep_control_finder
{
    static constexpr bool enabled = diag::enabled;

    /**
     * What was found. Static so the address handed across the launch
     * boundary outlives this loader's frames.
     */
    static inline sleep_control found{};

    static void run(EFI_SYSTEM_TABLE * system_table)
    {
        if constexpr (enabled) {
            execute(system_table);
        } else {
            static_cast<void>(system_table);
        }
    }

private:
    /**
     * Defined only when enabled - see sleep_control.cpp.
     */
    static void execute(EFI_SYSTEM_TABLE * system_table);
};

} // namespace zpp
