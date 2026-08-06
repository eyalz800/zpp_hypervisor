#include "zpp/dmar_unlink.h"

#include "zpp/trace.h"

#if ZPP_DIAG

#include "zpp/arch/x86_64/mmio.h"

#include <cstdint>
#include <cstring>

namespace zpp
{
namespace
{
/**
 * The two configuration table identifiers a root system description
 * pointer is published under. Compared whole rather than by their first
 * doubleword: a configuration table is whatever the firmware put there,
 * and this one is about to be edited.
 * @{
 */
constexpr EFI_GUID acpi_20_table_guid = {
    0x8868e871,
    0xe4f1,
    0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};

constexpr EFI_GUID acpi_10_table_guid = {
    0xeb9d2d30,
    0x2d88,
    0x11d3,
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
/**
 * @}
 */

/**
 * Offsets inside the root system description pointer, ACPI 6.5 table
 * 5.5, "Root System Description Pointer (RSDP) Structure".
 *
 * The revision decides how much of it exists: a zero there is the ACPI
 * 1.0 structure, which is twenty bytes and has an `RSDT` address and no
 * `XSDT` address at all, and reading offset 24 of one would be reading
 * whatever follows it.
 * @{
 */
constexpr std::size_t rsdp_revision_offset = 15;
constexpr std::size_t rsdp_rsdt_address_offset = 16;
constexpr std::size_t rsdp_length_offset = 20;
constexpr std::size_t rsdp_xsdt_address_offset = 24;
constexpr std::size_t rsdp_v1_length = 20;
/**
 * @}
 */

/**
 * Offsets inside a system description table header, ACPI 6.5 table 5.4.
 * The header is 36 bytes and every table in the system starts with one,
 * so the entry arrays of both root tables begin at 36.
 * @{
 */
constexpr std::size_t table_length_offset = 4;
constexpr std::size_t table_checksum_offset = 9;
constexpr std::size_t table_header_size = 36;
/**
 * @}
 */

/**
 * Offsets inside the `DMAR` table.
 *
 * Layout from the DMA remapping reporting structure: the host address
 * width and the flags byte follow the standard header, ten reserved
 * bytes follow those, and the variable length list of remapping
 * structures begins at 48.
 * @{
 */
constexpr std::size_t dmar_host_address_width_offset = 36;
constexpr std::size_t dmar_flags_offset = 37;
constexpr std::size_t dmar_structures_offset = 48;
/**
 * @}
 */

/**
 * A remapping structure's common header, and the fields of the one type
 * this reads.
 *
 * Every remapping structure begins with a type and a length, so the list
 * is walked by length regardless of which types this build understands.
 * Type 0 is the hardware unit definition; its flags byte carries
 * INCLUDE_PCI_ALL in bit 0, the segment number is a halfword at 6, and
 * the register base address is a quadword at 8.
 * @{
 */
constexpr std::uint16_t structure_type_hardware_unit = 0;
constexpr std::size_t structure_type_offset = 0;
constexpr std::size_t structure_length_offset = 2;
constexpr std::size_t drhd_flags_offset = 4;
constexpr std::size_t drhd_segment_offset = 6;
constexpr std::size_t drhd_register_base_offset = 8;
constexpr std::size_t drhd_minimum_length = 16;
constexpr std::uint8_t drhd_include_pci_all = 1u << 0;
/**
 * @}
 */

/**
 * The three register block fields this half reads, and only reads.
 *
 * Duplicated from `zpp/nvme/iommu_gate.h` on purpose rather than shared:
 * that header is the read-only gate the other strategy is built around
 * and this one must not become a reason to change it. The offsets are
 * architectural and are the same two numbers either way.
 *
 * The status register's Translation Enable Status is bit 31 and its
 * Interrupt Remapping Enable Status is bit 25.
 * @{
 */
constexpr std::uint32_t register_version_offset = 0x00;
constexpr std::uint32_t register_global_status_offset = 0x1c;
constexpr std::uint32_t status_translation_enabled = 1u << 31;
constexpr std::uint32_t status_interrupt_remapping_enabled = 1u << 25;
/**
 * @}
 */

/**
 * What was recorded. File scope rather than a member, so the storage
 * exists only in a translation unit that is compiled only when the
 * channel is.
 * @{
 */
nvme::remapping_unit g_units[dmar_unlink::max_units]{};
std::size_t g_unit_count{};
/**
 * @}
 */

/**
 * Reads an unaligned field out of a table.
 *
 * Every root table entry array starts at offset 36, which is four modulo
 * eight, so an `XSDT` entry is a misaligned quadword by construction. A
 * pointer cast would be undefined and, on a build with no unaligned
 * access legalisation, wrong.
 */
template <typename Value>
Value read_field(const unsigned char * base, std::size_t offset)
{
    Value value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

/**
 * The byte that makes a table's bytes sum to zero.
 */
std::uint8_t checksum_of(const unsigned char * table, std::size_t length)
{
    std::uint8_t sum{};
    for (std::size_t i{}; i < length; ++i) {
        sum = static_cast<std::uint8_t>(sum + table[i]);
    }
    return sum;
}

/**
 * Whether a table's own checksum already holds.
 */
bool checksum_holds(const unsigned char * table, std::size_t length)
{
    return 0 == checksum_of(table, length);
}

/**
 * Rewrites the checksum byte so the table sums to zero again.
 *
 * Written after the length has been reduced, so the sum covers the table
 * as it now is. The checksum byte is zeroed first because it is itself
 * one of the bytes being summed.
 */
void fix_checksum(unsigned char * table, std::size_t length)
{
    table[table_checksum_offset] = 0;
    table[table_checksum_offset] =
        static_cast<std::uint8_t>(0x100 - checksum_of(table, length));
}

/**
 * Finds the root system description pointer among the configuration
 * tables, preferring the 2.0 one.
 *
 * The 1.0 identifier is accepted as well so that a machine publishing
 * only that one is reported as having no `XSDT` rather than as having no
 * ACPI - the two lead to different conclusions and only one of them is a
 * reason to stop.
 */
const unsigned char * find_root_pointer(EFI_SYSTEM_TABLE * system_table)
{
    const unsigned char * fallback{};
    for (std::size_t i{}; i < system_table->NumberOfTableEntries; ++i) {
        auto & entry = system_table->ConfigurationTable[i];
        if (!std::memcmp(&entry.VendorGuid,
                         &acpi_20_table_guid,
                         sizeof(EFI_GUID))) {
            return static_cast<const unsigned char *>(entry.VendorTable);
        }
        if (!std::memcmp(&entry.VendorGuid,
                         &acpi_10_table_guid,
                         sizeof(EFI_GUID))) {
            fallback =
                static_cast<const unsigned char *>(entry.VendorTable);
        }
    }
    return fallback;
}

/**
 * Finds a table by signature in one root table, and reports where its
 * pointer sits so the pointer can be removed.
 *
 * `entry_size` is four for an `RSDT` and eight for an `XSDT`, which is
 * the only difference between walking the two.
 */
struct located
{
    unsigned char * table{};
    std::size_t index{};
    bool found{};
};

located find_in_root(unsigned char * root,
                     std::size_t entry_size,
                     const char * signature)
{
    if (!root) {
        return {};
    }

    auto length = read_field<std::uint32_t>(root, table_length_offset);
    if (length <= table_header_size) {
        return {};
    }

    auto entries = (length - table_header_size) / entry_size;
    for (std::size_t i{}; i < entries; ++i) {
        auto offset = table_header_size + (i * entry_size);
        std::uint64_t address{};
        if (4 == entry_size) {
            address = read_field<std::uint32_t>(root, offset);
        } else {
            address = read_field<std::uint64_t>(root, offset);
        }
        if (!address) {
            continue;
        }

        auto * table = reinterpret_cast<unsigned char *>(
            static_cast<std::uintptr_t>(address));
        if (std::memcmp(table, signature, 4)) {
            continue;
        }
        return {.table = table, .index = i, .found = true};
    }

    return {};
}

/**
 * Removes one entry from a root table and repairs it.
 *
 * The entries after the removed one are moved down over it, the header's
 * length is reduced by one entry, and the checksum is recomputed over the
 * new length. The tail of the old array is left as it was, outside the
 * table's declared length, which nothing is allowed to read.
 *
 * Refuses if the table's checksum did not hold on the way in. A root
 * table that was already inconsistent is not one to start editing: the
 * repair would make it look correct and hide whatever was wrong.
 */
bool remove_entry(unsigned char * root,
                  std::size_t entry_size,
                  std::size_t index)
{
    auto length = read_field<std::uint32_t>(root, table_length_offset);
    if (!checksum_holds(root, length)) {
        return false;
    }

    auto offset = table_header_size + (index * entry_size);
    auto remaining = length - (offset + entry_size);
    std::memmove(root + offset, root + offset + entry_size, remaining);

    auto reduced = static_cast<std::uint32_t>(length - entry_size);
    std::memcpy(root + table_length_offset, &reduced, sizeof(reduced));
    fix_checksum(root, reduced);
    return true;
}

/**
 * Records every hardware unit the table describes.
 *
 * Walked by each structure's own length rather than by type, so a table
 * carrying reserved memory regions, address space allocations or
 * anything newer than this code is stepped over rather than
 * misinterpreted.
 */
void record_units(const unsigned char * dmar)
{
    g_unit_count = 0;

    auto length = read_field<std::uint32_t>(dmar, table_length_offset);
    if (length <= dmar_structures_offset) {
        return;
    }

    for (std::size_t offset = dmar_structures_offset; offset < length;) {
        auto type = read_field<std::uint16_t>(
            dmar, offset + structure_type_offset);
        auto size = read_field<std::uint16_t>(
            dmar, offset + structure_length_offset);

        // A zero or oversized length would either spin here or walk off
        // the end. Both mean the table is not one we can read.
        if ((size < 4) || ((offset + size) > length)) {
            trace::line("ZPP_TRACE dmar: malformed structure list");
            g_unit_count = 0;
            return;
        }

        if ((structure_type_hardware_unit == type) &&
            (size >= drhd_minimum_length)) {
            if (g_unit_count >= dmar_unlink::max_units) {
                trace::line("ZPP_TRACE dmar: more units than recorded");
                g_unit_count = 0;
                return;
            }

            auto flags =
                read_field<std::uint8_t>(dmar, offset + drhd_flags_offset);
            g_units[g_unit_count++] = {
                .registers = read_field<std::uint64_t>(
                    dmar, offset + drhd_register_base_offset),
                .segment = read_field<std::uint16_t>(
                    dmar, offset + drhd_segment_offset),
                .include_pci_all = 0 != (flags & drhd_include_pci_all),
            };
        }

        offset += size;
    }
}

/**
 * Whether every recorded unit is in the state this strategy requires
 * before it may take ownership.
 *
 * Two refusals, and they are the whole ordering argument made
 * enforceable:
 *
 * - **Translation already enabled.** Somebody has domains and live
 *   device addresses. Repointing that unit at an identity map would turn
 *   every address a device is already using into a raw physical address,
 *   and the next transfer would land wherever those bits happen to
 *   point. There is no safe transition out of a translating domain from
 *   underneath its owner, so a unit found translating is a unit this
 *   strategy does not touch - and if it does not touch it, the guest
 *   must keep being able to see it, so nothing is unlinked either.
 *
 * - **Interrupt remapping already enabled.** This one has no consistent
 *   answer other than refusal. Leaving it on hands the guest a machine
 *   whose interrupt remap table it cannot see, cannot rebuild and cannot
 *   extend, and the first interrupt it reprograms is misrouted. Turning
 *   it off here breaks whatever is currently using it, which at this
 *   point in the boot is the firmware itself. Both are worse than not
 *   taking the hardware.
 *
 * Read only, and it runs before any edit for that reason.
 */
bool units_are_ours_to_take()
{
    for (std::size_t i{}; i < g_unit_count; ++i) {
        auto base = reinterpret_cast<const volatile unsigned char *>(
            static_cast<std::uintptr_t>(g_units[i].registers));
        if (!g_units[i].registers) {
            trace::line("ZPP_TRACE dmar: unit with no register base");
            return false;
        }

        auto version =
            arch::x86_64::read32(base + register_version_offset);
        if ((0 == version) || (0xffffffffu == version)) {
            trace::hex_line("ZPP_TRACE dmar: unit absent at ",
                            g_units[i].registers);
            return false;
        }

        auto status =
            arch::x86_64::read32(base + register_global_status_offset);
        trace::hex_line("ZPP_TRACE dmar: unit status ", status);

        if (0 != (status & status_translation_enabled)) {
            trace::line("ZPP_TRACE dmar: translation already enabled");
            return false;
        }
        if (0 != (status & status_interrupt_remapping_enabled)) {
            trace::line("ZPP_TRACE dmar: interrupt remapping already on");
            return false;
        }
    }

    return 0 != g_unit_count;
}

/**
 * Says out loud what was done, on the channel that survives tracing being
 * switched off.
 *
 * `DIAGNOSTICS.md` requires that degrading a machine's DMA protection is
 * never silent, and this is where that requirement is met. The verdict is
 * a result rather than a diagnostic, so it goes through `raw` for the
 * reason `zpp/trace.h` gives about the self check's verdict.
 */
void verdict(const char * text)
{
    char buffer[trace::line_capacity]{};
    auto end = trace::append_text(buffer, "zpp: ZPP_OWNED_IOMMU ");
    end = trace::append_text(end, text);
    end = trace::append_text(end, "\r\n");
    *end = 0;
    trace::raw(buffer);
}

} // namespace

std::span<const nvme::remapping_unit> dmar_unlink::units()
{
    return {g_units, g_unit_count};
}

void dmar_unlink::execute(EFI_SYSTEM_TABLE * system_table)
{
    g_unit_count = 0;

    auto * rsdp = find_root_pointer(system_table);
    if (!rsdp) {
        verdict("no acpi root pointer, guest keeps its remapping");
        return;
    }

    // The 1.0 structure is twenty bytes and its checksum covers all of
    // them; the 2.0 one carries its own length and a second checksum over
    // the whole of it. Both are verified before anything here believes a
    // pointer read out of it.
    auto revision = rsdp[rsdp_revision_offset];
    if (!checksum_holds(rsdp, rsdp_v1_length)) {
        verdict("acpi root pointer checksum failed");
        return;
    }
    if (revision >= 2) {
        auto length = read_field<std::uint32_t>(rsdp, rsdp_length_offset);
        if ((length < rsdp_v1_length) || !checksum_holds(rsdp, length)) {
            verdict("acpi root pointer extended checksum failed");
            return;
        }
    }

    auto * xsdt =
        (revision >= 2)
            ? reinterpret_cast<unsigned char *>(
                  static_cast<std::uintptr_t>(read_field<std::uint64_t>(
                      rsdp, rsdp_xsdt_address_offset)))
            : nullptr;
    auto * rsdt =
        reinterpret_cast<unsigned char *>(static_cast<std::uintptr_t>(
            read_field<std::uint32_t>(rsdp, rsdp_rsdt_address_offset)));

    auto in_xsdt = find_in_root(xsdt, sizeof(std::uint64_t), "DMAR");
    auto in_rsdt = find_in_root(rsdt, sizeof(std::uint32_t), "DMAR");
    auto * dmar = in_xsdt.found ? in_xsdt.table : in_rsdt.table;
    if (!dmar) {
        verdict("no dmar, nothing is remapping, guest unchanged");
        return;
    }

    auto dmar_length =
        read_field<std::uint32_t>(dmar, table_length_offset);
    if ((dmar_length <= dmar_structures_offset) ||
        !checksum_holds(dmar, dmar_length)) {
        verdict("dmar checksum failed, guest keeps its remapping");
        return;
    }

    // Reported for the record rather than acted on. The host address
    // width bounds the guest physical addresses a unit can be given, and
    // the flags byte carries the platform's own opt-in to DMA remapping
    // - which is the bit `NVME-LOG.md` records as clear on the
    // development target, and is the reason that machine leaves its
    // storage controller in a pass-through domain.
    trace::hex_line(
        "ZPP_TRACE dmar: host address width ",
        read_field<std::uint8_t>(dmar, dmar_host_address_width_offset));
    trace::hex_line("ZPP_TRACE dmar: flags ",
                    read_field<std::uint8_t>(dmar, dmar_flags_offset));

    record_units(dmar);
    if (!g_unit_count) {
        verdict("dmar describes no usable unit, guest keeps it");
        return;
    }

    for (std::size_t i{}; i < g_unit_count; ++i) {
        trace::hex_line("ZPP_TRACE dmar: unit registers at ",
                        g_units[i].registers);
    }

    if (!units_are_ours_to_take()) {
        g_unit_count = 0;
        verdict("hardware already in use, refused, guest keeps it");
        return;
    }

    // Only now is anything written. Both root tables are edited when both
    // carry a pointer, because a guest is entitled to read either.
    bool unlinked{};
    if (in_xsdt.found &&
        remove_entry(xsdt, sizeof(std::uint64_t), in_xsdt.index)) {
        trace::line("ZPP_TRACE dmar: unlinked from xsdt");
        unlinked = true;
    }
    if (in_rsdt.found &&
        remove_entry(rsdt, sizeof(std::uint32_t), in_rsdt.index)) {
        trace::line("ZPP_TRACE dmar: unlinked from rsdt");
        unlinked = true;
    }

    if (!unlinked) {
        g_unit_count = 0;
        verdict("could not unlink dmar, guest keeps its remapping");
        return;
    }

    // The closure. Believing an edit is not the same as having checked
    // it, and the thing being edited is the table every operating system
    // in the world reads to find its hardware - a root table left
    // inconsistent does not fail here, it fails inside the guest's ACPI
    // parser with no reference to us.
    //
    // So both root tables are walked again through exactly the code a
    // reader would use: the signature must now be absent, and the
    // checksum must hold over the reduced length. Anything else is
    // reported as loudly as the success is.
    auto still_there =
        find_in_root(xsdt, sizeof(std::uint64_t), "DMAR").found ||
        find_in_root(rsdt, sizeof(std::uint32_t), "DMAR").found;
    auto consistent =
        (!xsdt || checksum_holds(xsdt,
                                 read_field<std::uint32_t>(
                                     xsdt, table_length_offset))) &&
        (!rsdt ||
         checksum_holds(
             rsdt, read_field<std::uint32_t>(rsdt, table_length_offset)));

    if (xsdt) {
        trace::hex_line(
            "ZPP_TRACE dmar: xsdt length now ",
            read_field<std::uint32_t>(xsdt, table_length_offset));
    }
    if (rsdt) {
        trace::hex_line(
            "ZPP_TRACE dmar: rsdt length now ",
            read_field<std::uint32_t>(rsdt, table_length_offset));
    }

    if (still_there || !consistent) {
        g_unit_count = 0;
        verdict("unlink did not verify, root tables may be inconsistent");
        return;
    }
    trace::line("ZPP_TRACE dmar: verified absent, checksums hold");

    // The cost, stated at the moment it is incurred, on the one channel
    // that is on in every build carrying this.
    verdict("dmar unlinked - the guest has no dma remapping and will "
            "report kernel dma protection off");
}

} // namespace zpp

#endif
