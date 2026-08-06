#include "zpp/sleep_control.h"

#include "zpp/trace.h"

#if ZPP_DIAG

extern "C" {
#include <Guid/Acpi.h>
}

namespace zpp
{
namespace
{
/**
 * Offsets inside a table header, shared by every ACPI table.
 * @{
 */
constexpr std::size_t header_length_offset = 4;
constexpr std::size_t header_size = 36;
/**
 * @}
 */

/**
 * Offsets inside the fixed ACPI description table, from ACPI 6.5 table
 * 5.9. Only the four that say where the sleep control register is.
 *
 * The extended forms exist because the original fields are 32 bit I/O
 * port numbers and later revisions wanted a general address structure.
 * Where both are present the extended one wins, which is what the
 * specification requires and also what every operating system does.
 * @{
 */
constexpr std::size_t pm1a_control_block_offset = 64;
constexpr std::size_t pm1b_control_block_offset = 68;
constexpr std::size_t pm1_control_length_offset = 89;
constexpr std::size_t extended_pm1a_control_offset = 172;
constexpr std::size_t extended_pm1b_control_offset = 184;
/**
 * @}
 */

/**
 * A generic address structure is twelve bytes: address space, bit
 * width, bit offset, access size, then a sixty four bit address. Only
 * the space and the address are needed here.
 * @{
 */
constexpr std::size_t address_structure_size = 12;
constexpr std::size_t address_structure_address_offset = 4;
constexpr std::uint8_t address_space_io = 1;
/**
 * @}
 */

std::uint32_t load32(const std::uint8_t * from)
{
    return std::uint32_t{from[0]} | (std::uint32_t{from[1]} << 8) |
           (std::uint32_t{from[2]} << 16) | (std::uint32_t{from[3]} << 24);
}

std::uint64_t load64(const std::uint8_t * from)
{
    return std::uint64_t{load32(from)} |
           (std::uint64_t{load32(from + 4)} << 32);
}

/**
 * The address out of a generic address structure, but only when it
 * describes an I/O port. Anything else - memory mapped, embedded
 * controller - is not something an I/O bitmap can watch, so it is
 * refused rather than truncated into a port number.
 */
std::uint16_t io_port_from(const std::uint8_t * structure)
{
    if (address_space_io != structure[0]) {
        return 0;
    }
    auto address = load64(structure + address_structure_address_offset);
    if (address > 0xffff) {
        return 0;
    }
    return static_cast<std::uint16_t>(address);
}

} // namespace

void sleep_control_finder::execute(EFI_SYSTEM_TABLE * system_table)
{
    trace::line("sleep: begin");

    // ---- the root pointer --------------------------------------------
    const void * rsdp{};
    for (std::size_t i{}; i < system_table->NumberOfTableEntries; ++i) {
        auto & entry = system_table->ConfigurationTable[i];
        EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
        if (0 ==
            __builtin_memcmp(&entry.VendorGuid, &acpi20, sizeof(acpi20))) {
            rsdp = entry.VendorTable;
            break;
        }
    }
    if (!rsdp) {
        trace::line("sleep: no acpi 2.0 table, refused");
        return;
    }

    // The extended pointer, at offset 24, which is the only one worth
    // using: a table above four gigabytes cannot be named by the older
    // thirty two bit field at all.
    auto * pointer = static_cast<const std::uint8_t *>(rsdp);
    auto xsdt_address = load64(pointer + 24);
    if (!xsdt_address) {
        trace::line("sleep: no xsdt, refused");
        return;
    }
    auto * xsdt = reinterpret_cast<const std::uint8_t *>(xsdt_address);

    // ---- the fixed description table ---------------------------------
    constexpr std::uint32_t fadt_signature = 0x50434146; // 'FACP'
    const std::uint8_t * fadt{};
    auto xsdt_length = load32(xsdt + header_length_offset);
    for (auto offset = header_size; (offset + 8) <= xsdt_length;
         offset += 8) {
        auto candidate = load64(xsdt + offset);
        if (!candidate) {
            continue;
        }
        auto * table = reinterpret_cast<const std::uint8_t *>(candidate);
        if (fadt_signature == load32(table)) {
            fadt = table;
            break;
        }
    }
    if (!fadt) {
        trace::line("sleep: no FACP table, refused");
        return;
    }

    auto fadt_length = load32(fadt + header_length_offset);
    trace::hex_line("sleep: facp length ", fadt_length);

    // ---- the control registers ---------------------------------------
    //
    // The extended forms first, and only if the table is long enough to
    // contain them - a shorter revision simply does not have the field,
    // and reading past its length would be reading whatever follows it
    // in memory.
    std::uint16_t pm1a{};
    std::uint16_t pm1b{};

    if (fadt_length >=
        (extended_pm1a_control_offset + address_structure_size)) {
        pm1a = io_port_from(fadt + extended_pm1a_control_offset);
    }
    if (fadt_length >=
        (extended_pm1b_control_offset + address_structure_size)) {
        pm1b = io_port_from(fadt + extended_pm1b_control_offset);
    }

    // Fall back to the original fields where the extended ones are
    // absent or describe something an I/O bitmap cannot watch.
    if (!pm1a && (fadt_length >= (pm1a_control_block_offset + 4))) {
        auto value = load32(fadt + pm1a_control_block_offset);
        pm1a = (value <= 0xffff) ? static_cast<std::uint16_t>(value) : 0;
    }
    if (!pm1b && (fadt_length >= (pm1b_control_block_offset + 4))) {
        auto value = load32(fadt + pm1b_control_block_offset);
        pm1b = (value <= 0xffff) ? static_cast<std::uint16_t>(value) : 0;
    }

    std::uint8_t width{};
    if (fadt_length > pm1_control_length_offset) {
        width = fadt[pm1_control_length_offset];
    }

    trace::hex_line("sleep: pm1a control port ", pm1a);
    trace::hex_line("sleep: pm1b control port ", pm1b);
    trace::hex_line("sleep: control width ", width);

    if (!pm1a || !width) {
        trace::line("sleep: no usable control register, refused");
        return;
    }

    // Written last, so a partially filled structure is never usable.
    found.pm1a_control_port = pm1a;
    found.pm1b_control_port = pm1b;
    found.control_width = width;
    found.magic = sleep_control::valid_magic;

    trace::line("sleep: VERDICT sleep control register located");
}

} // namespace zpp

#endif
