#pragma once
#include <cstdint>

namespace zpp::arch::x86_64
{
/*
 * The four port accessors below are written with operand constraints
 * rather than as `naked` functions, unlike everything in `asm.h`. That
 * is deliberate and it is not a style preference.
 *
 * A naked function has to name the register its argument arrived in, so
 * it bakes in one calling convention. `asm.h` gets away with that
 * because only the hypervisor includes it, and the hypervisor is the
 * freestanding ELF target - System V, first argument in `rdi`. This
 * header is also included by the UEFI loader, which is built
 * `--target=x86_64-pc-windows-msvc`, where the first argument arrives in
 * `rcx` instead. The System V spelling read `di` there, so every
 * configuration space access went to whatever port the low half of an
 * unrelated register happened to hold, and the NVMe controller the
 * firmware had just booted us from was reported absent.
 *
 * Letting the compiler place the operands is the fix that cannot rot:
 * it is correct under both conventions and needs no per-target spelling.
 */

/**
 * Reads a byte from an IO port.
 */
inline std::uint8_t in8(std::uint16_t port)
{
    std::uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/**
 * Reads a doubleword from an IO port.
 */
inline std::uint32_t in32(std::uint16_t port)
{
    std::uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/**
 * Writes a byte to an IO port.
 */
inline void out8(std::uint16_t port, std::uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Writes a word to an IO port.
 *
 * Not needed by anything in this header - configuration space is reached a
 * byte or a doubleword at a time - but it belongs beside the other two
 * rather than in a second copy of the same reasoning somewhere else. Its
 * caller is the ACPI sleep control register, which the fixed ACPI
 * description table almost always reports as two bytes wide, and which has
 * to be written at the width the table gives.
 */
inline void out16(std::uint16_t port, std::uint16_t value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Writes a doubleword to an IO port.
 */
inline void out32(std::uint16_t port, std::uint32_t value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
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

    /**
     * The header type byte. Bit 7 set means the device has functions
     * beyond function zero, which is what makes it worth probing them.
     */
    static constexpr std::uint32_t header_type_offset = 0x0e;
    static constexpr std::uint8_t multi_function_bit = 0x80;

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
