#pragma once
#include <cstdint>

namespace zpp::arch::x86_64
{
/**
 * Reads a byte from an IO port.
 */
inline std::uint8_t __attribute__((naked)) in8(std::uint16_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov dx, di
        xor eax, eax
        in al, dx
        ret
    )!!");
}

/**
 * Reads a doubleword from an IO port.
 */
inline std::uint32_t __attribute__((naked)) in32(std::uint16_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov dx, di
        in eax, dx
        ret
    )!!");
}

/**
 * Writes a byte to an IO port.
 */
inline void __attribute__((naked)) out8(std::uint16_t, std::uint8_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov dx, di
        mov eax, esi
        out dx, al
        ret
    )!!");
}

/**
 * Writes a doubleword to an IO port.
 */
inline void __attribute__((naked)) out32(std::uint16_t, std::uint32_t)
{
    asm(R"!!(
        .intel_syntax noprefix
        mov dx, di
        mov eax, esi
        out dx, eax
        ret
    )!!");
}

/**
 * PCI configuration space through the two legacy IO ports.
 *
 * The mechanism rather than the memory mapped one, for the same reason
 * Linux's own early boot code uses it in arch/x86/pci/early.c: reaching
 * configuration space through MMCONFIG needs the base address out of the
 * ACPI MCFG table, which needs the tables parsed, while these two ports
 * are architectural and work with nothing set up at all. It reaches bus
 * zero on every x86 machine, which is where a host controller integrated
 * into the chipset lives.
 *
 * Byte access goes to the data port offset by the low two bits of the
 * register offset, which is how the same file does it - and it matters
 * rather than being a nicety, since the doubleword holding the command
 * register also holds the status register, whose bits are write-one-to-
 * clear. A read-modify-write of the whole doubleword would clear them.
 */
class pci_config
{
public:
    /**
     * The address port. A write selects the doubleword that the data port
     * then reads or writes.
     */
    static constexpr std::uint16_t address_port = 0xcf8;

    /**
     * The data port.
     */
    static constexpr std::uint16_t data_port = 0xcfc;

    /**
     * How many buses, devices and functions a scan has to cover.
     */
    static constexpr std::uint32_t max_buses = 256;
    static constexpr std::uint32_t max_devices = 32;
    static constexpr std::uint32_t max_functions = 8;

    /**
     * Identifies one configuration space.
     */
    struct address
    {
        std::uint32_t bus{};
        std::uint32_t device{};
        std::uint32_t function{};
    };

    /**
     * The value the address port takes to select a doubleword.
     */
    static constexpr std::uint32_t select(const address & target,
                                          std::uint32_t offset)
    {
        constexpr std::uint32_t enable = 0x80000000;
        return enable | (target.bus << 16) | (target.device << 11) |
               (target.function << 8) | (offset & 0xfc);
    }

    /**
     * Reads a doubleword. The offset is rounded down to a doubleword
     * boundary, which is the only thing this mechanism can address.
     */
    static std::uint32_t read32(const address & target,
                                std::uint32_t offset)
    {
        out32(address_port, select(target, offset));
        return in32(data_port);
    }

    /**
     * Writes a doubleword.
     */
    static void write32(const address & target,
                        std::uint32_t offset,
                        std::uint32_t value)
    {
        out32(address_port, select(target, offset));
        out32(data_port, value);
    }

    /**
     * Reads a byte.
     */
    static std::uint8_t read8(const address & target, std::uint32_t offset)
    {
        out32(address_port, select(target, offset));
        return in8(static_cast<std::uint16_t>(data_port + (offset & 0x3)));
    }

    /**
     * Writes a byte.
     */
    static void write8(const address & target,
                       std::uint32_t offset,
                       std::uint8_t value)
    {
        out32(address_port, select(target, offset));
        out8(static_cast<std::uint16_t>(data_port + (offset & 0x3)),
             value);
    }

    /**
     * Reads a word out of the doubleword that contains it.
     */
    static std::uint16_t read16(const address & target,
                                std::uint32_t offset)
    {
        auto value = read32(target, offset);
        return static_cast<std::uint16_t>(value >> ((offset & 0x2) * 8));
    }

    /**
     * Offsets of the registers this needs, from the configuration space
     * header every function has.
     */
    static constexpr std::uint32_t vendor_id_offset = 0x00;
    static constexpr std::uint32_t device_id_offset = 0x02;
    static constexpr std::uint32_t command_offset = 0x04;
    static constexpr std::uint32_t class_revision_offset = 0x08;
    static constexpr std::uint32_t base_address_0_offset = 0x10;

    /**
     * Command register bits.
     */
    static constexpr std::uint8_t command_memory_space = 1 << 1;
    static constexpr std::uint8_t command_bus_master = 1 << 2;

    /**
     * Base address register bits. A memory register whose type field is
     * this value is the low half of a sixty four bit pair.
     */
    static constexpr std::uint32_t base_address_memory_mask = ~0xfu;
    static constexpr std::uint32_t base_address_type_mask = 0x6;
    static constexpr std::uint32_t base_address_type_64_bit = 0x4;

    /**
     * The class, subclass and interface a USB extensible host controller
     * reports, in the layout the class and revision doubleword has once
     * its revision byte is shifted out.
     */
    static constexpr std::uint32_t class_xhci = 0x0c0330;
};

} // namespace zpp::arch::x86_64
