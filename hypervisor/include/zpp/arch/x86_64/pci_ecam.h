#pragma once
#include <cstdint>

#include "zpp/arch/x86_64/pci.h"

namespace zpp::arch::x86_64
{
/**
 * Configuration space reached through a memory mapped window rather than
 * through the two legacy IO ports.
 *
 * Its own header rather than another member of `pci_config`, and the
 * reason is that the two are not the same kind of thing. `pci_config` is
 * stateless: the ports are architectural, there is exactly one of them,
 * and every accessor can be static. A window has to be told where it is
 * and how large it is, so it is an object - and there can be more than
 * one of them at a time, which is the entire point of the VMD case that
 * motivated this. Folding a constructor and two members into `pci_config`
 * would have made every existing static caller pass state it does not
 * have.
 *
 * The layout is the one PCI Express defines for enhanced configuration
 * access and which Linux spells out in include/linux/pci-ecam.h as
 * PCIE_ECAM_OFFSET: bus number at bit 20, device and function packed into
 * one byte at bit 12, register offset in the low twelve bits. One
 * megabyte per bus, four kilobytes per function.
 *
 * Two hazards this deliberately encodes, both from the VMD case in
 * Linux's drivers/pci/controller/vmd.c:
 *
 * - **Writes are posted.** vmd_pci_write's comment says the hardware
 *   "converts non-posted config writes to posted memory writes", so a
 *   write returns before the register has changed. Every write below
 *   reads the same location back to force the completion, which is what
 *   that function does.
 * - **Access may need serializing.** vmd_pci_read's comment says the "CPU
 *   may deadlock if config space is not serialized on some versions of
 *   this hardware", and Linux takes a spinlock across every access. No
 *   lock is taken here, because the one caller runs in the UEFI loader
 *   before any application processor has been started and before any
 *   guest exists, so there is exactly one accessor. Anything that reaches
 *   a window from more than one processor has to wrap it in a
 *   `zpp::spin_lock` of its own - this class cannot do it, since the lock
 *   has to cover the whole read-modify-write, not one access.
 *
 * The base is a **mapped** address, not a physical one. Under boot
 * services those are the same, since the address space is identity
 * mapped; the resident side has to map the window itself and hand the
 * mapping in.
 */
class pci_ecam
{
public:
    /**
     * The shifts and masks of the enhanced configuration address, as
     * above.
     * @{
     */
    static constexpr std::uint32_t bus_shift = 20;
    static constexpr std::uint32_t function_shift = 12;
    static constexpr std::uint32_t register_mask = 0xfff;
    /**
     * @}
     */

    static constexpr std::uint64_t bytes_per_bus = std::uint64_t{1}
                                                   << bus_shift;
    static constexpr std::uint64_t bytes_per_function = std::uint64_t{1}
                                                        << function_shift;

    constexpr pci_ecam() = default;

    constexpr pci_ecam(std::uint64_t mapped_base, std::uint64_t length) :
        m_base(mapped_base), m_length(length)
    {
    }

    /**
     * A window is usable when it has an address at all and covers at
     * least the one bus that makes it a window.
     */
    constexpr bool valid() const
    {
        return (0 != m_base) && (m_length >= bytes_per_bus);
    }

    constexpr std::uint64_t base() const
    {
        return m_base;
    }

    constexpr std::uint64_t length() const
    {
        return m_length;
    }

    /**
     * How many buses the window covers.
     */
    constexpr std::uint32_t buses() const
    {
        return static_cast<std::uint32_t>(m_length / bytes_per_bus);
    }

    /**
     * Where one function's register lands in the window.
     *
     * The bus here is the **index into the window**, not a bus number
     * from anywhere else. A window that starts at a bus number other than
     * zero - which is the normal VMD case - has to subtract its own start
     * before calling this, and `vmd` does.
     */
    static constexpr std::uint64_t
    offset_of(const pci_config::address & target, std::uint32_t offset)
    {
        auto packed =
            ((target.device & 0x1f) << 3) | (target.function & 0x7);
        return (std::uint64_t{target.bus & 0xff} << bus_shift) |
               (std::uint64_t{packed} << function_shift) |
               (offset & register_mask);
    }

    /**
     * Reads, in the three widths a configuration header is described in.
     *
     * An access the window does not cover reads as all ones, which is
     * what an absent function reads as through either mechanism - so
     * every caller's existing "is anything there" test already refuses
     * it, and no caller has to learn a second way of saying nothing.
     * @{
     */
    std::uint8_t read8(const pci_config::address & target,
                       std::uint32_t offset) const
    {
        const auto * at = locate(target, offset, sizeof(std::uint8_t));
        if (!at) {
            return 0xff;
        }
        return *at;
    }

    std::uint16_t read16(const pci_config::address & target,
                         std::uint32_t offset) const
    {
        const auto * at = locate(target, offset, sizeof(std::uint16_t));
        if (!at) {
            return 0xffff;
        }
        return *reinterpret_cast<const volatile std::uint16_t *>(at);
    }

    std::uint32_t read32(const pci_config::address & target,
                         std::uint32_t offset) const
    {
        const auto * at = locate(target, offset, sizeof(std::uint32_t));
        if (!at) {
            return 0xffffffff;
        }
        return *reinterpret_cast<const volatile std::uint32_t *>(at);
    }
    /**
     * @}
     */

    /**
     * Writes, each followed by a read back of the same location for the
     * posted write reason above. The read's value is discarded on
     * purpose: it is the completion that is wanted, not the data.
     * @{
     */
    void write8(const pci_config::address & target,
                std::uint32_t offset,
                std::uint8_t value) const
    {
        auto * at = locate(target, offset, sizeof(std::uint8_t));
        if (!at) {
            return;
        }
        *at = value;
        static_cast<void>(*at);
    }

    void write16(const pci_config::address & target,
                 std::uint32_t offset,
                 std::uint16_t value) const
    {
        auto * at = locate(target, offset, sizeof(std::uint16_t));
        if (!at) {
            return;
        }
        auto * word = reinterpret_cast<volatile std::uint16_t *>(at);
        *word = value;
        static_cast<void>(*word);
    }

    void write32(const pci_config::address & target,
                 std::uint32_t offset,
                 std::uint32_t value) const
    {
        auto * at = locate(target, offset, sizeof(std::uint32_t));
        if (!at) {
            return;
        }
        auto * doubleword = reinterpret_cast<volatile std::uint32_t *>(at);
        *doubleword = value;
        static_cast<void>(*doubleword);
    }
    /**
     * @}
     */

private:
    /**
     * The mapped address of a register, or null when the access is not
     * wholly inside the window.
     *
     * Checked against the window's length rather than against a bus
     * count, so a window shorter than the caller believes refuses the
     * accesses past its end instead of reading whatever the firmware put
     * after it.
     */
    volatile std::uint8_t * locate(const pci_config::address & target,
                                   std::uint32_t offset,
                                   std::uint64_t width) const
    {
        if (!valid()) {
            return nullptr;
        }
        auto where = offset_of(target, offset);
        if ((where + width) > m_length) {
            return nullptr;
        }
        return reinterpret_cast<volatile std::uint8_t *>(m_base + where);
    }

    std::uint64_t m_base{};
    std::uint64_t m_length{};
};

} // namespace zpp::arch::x86_64
