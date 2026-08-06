#include "zpp/reserved_region.h"

#include "zpp/trace.h"

#if ZPP_DIAG

extern "C" {
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/DmaRemappingReportingTable.h>
}

#include "zpp/arch/x86_64/pci.h"
#include "zpp/nvme/iommu_gate.h"

#include <cstddef>

namespace zpp
{
namespace
{
using arch::x86_64::pci_config;

/**
 * The class code an NVM Express controller reports, in the layout the
 * class and revision doubleword has once its revision byte is shifted
 * out. The same constant nvme_selftest.cpp uses, and for the same reason:
 * base class 01h mass storage, subclass 08h non-volatile memory,
 * programming interface 02h NVM Express.
 */
constexpr std::uint32_t class_nvme = 0x010802;

/**
 * How large a window to reserve.
 *
 * Sixteen kilobytes, not the module. The point of a reserved region is
 * that it is the smallest hole that does the job: it is a range the guest
 * is told to keep out of one device's address space, so every page of it
 * is protection the guest gives up. Four pages carries a submission
 * queue, a completion queue and a staging block with room over.
 */
constexpr std::size_t window_pages = 4;

/**
 * How deep a PCI hierarchy this will walk, and therefore the longest
 * device scope path it can build.
 *
 * VT-d 5.20 8.3.1 has no architectural limit - "a device in a N-deep
 * hierarchy is identified by N {PCI Device Number, PCI Function Number}
 * pairs" - so the bound is ours. Eight is far past any real topology and
 * keeps the scan bounded, which matters more: this runs before anything
 * else and a machine that hangs here shows a black screen.
 */
constexpr std::size_t max_path_depth = 8;

/**
 * Configuration space offsets beyond the ones pci_config already names.
 * @{
 */
constexpr std::uint32_t header_type_offset = 0x0e;
constexpr std::uint32_t secondary_bus_offset = 0x19;
constexpr std::uint8_t header_type_mask = 0x7f;
constexpr std::uint8_t header_type_bridge = 0x01;
constexpr std::uint8_t header_type_multi_function = 0x80;
/**
 * @}
 */

/**
 * The hierarchical path from a host bridge down to one function, in the
 * shape VT-d 5.20 8.3.1 asks a device scope entry to describe it: a Start
 * Bus Number and N {Device, Function} pairs, "where the first {Device,
 * Function} pair resides on the bus identified by the Start Bus Number
 * field" and "each subsequent pair resides on the bus directly behind the
 * bus of the device identified by the previous pair".
 *
 * Recorded as the path rather than as the requester id on purpose. A
 * requester id is what the device has *now*: 8.3.5 has system software
 * changing bus numbers on resource rebalancing, and the path is what
 * survives that, because resolving it re-reads each bridge's secondary
 * bus register.
 */
struct device_path
{
    std::uint8_t start_bus{};
    std::uint8_t depth{};
    EFI_ACPI_DMAR_PCI_PATH node[max_path_depth]{};

    /**
     * Where the walk found it this boot. Only for reporting and for the
     * resident side's own walk, which indexes tables by requester id.
     * @{
     */
    std::uint8_t bus{};
    std::uint8_t device{};
    std::uint8_t function{};
    /**
     * @}
     */
};

/**
 * Unaligned loads and stores over table bytes.
 *
 * ACPI tables are byte streams with no alignment guarantee worth relying
 * on - an XSDT's entry array starts at offset 36, so every second
 * quadword pointer in it is misaligned by four. Byte loops rather than a
 * cast, so there is no undefined behaviour to reason about and no
 * dependence on what the compiler does with a misaligned access.
 * @{
 */
std::uint16_t load16(const std::uint8_t * from)
{
    return static_cast<std::uint16_t>(from[0] |
                                      (std::uint16_t{from[1]} << 8));
}

std::uint32_t load32(const std::uint8_t * from)
{
    std::uint32_t value{};
    for (std::size_t i{}; i < 4; ++i) {
        value |= std::uint32_t{from[i]} << (i * 8);
    }
    return value;
}

std::uint64_t load64(const std::uint8_t * from)
{
    std::uint64_t value{};
    for (std::size_t i{}; i < 8; ++i) {
        value |= std::uint64_t{from[i]} << (i * 8);
    }
    return value;
}

void store32(std::uint8_t * to, std::uint32_t value)
{
    for (std::size_t i{}; i < 4; ++i) {
        to[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void store64(std::uint8_t * to, std::uint64_t value)
{
    for (std::size_t i{}; i < 8; ++i) {
        to[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void copy_bytes(std::uint8_t * to,
                const std::uint8_t * from,
                std::size_t count)
{
    for (std::size_t i{}; i < count; ++i) {
        to[i] = from[i];
    }
}
/**
 * @}
 */

/**
 * Offsets inside the common ACPI description table header, taken from
 * EDK2's EFI_ACPI_DESCRIPTION_HEADER in MdePkg/Include/IndustryStandard/
 * Acpi10.h rather than from recall. VT-d 5.20 8.1 gives the same four for
 * the DMAR table specifically: Signature at 0, Length at 4, Revision at
 * 8, and Checksum at 9 with "entire table must sum to zero".
 * @{
 */
constexpr std::size_t header_length_offset =
    offsetof(EFI_ACPI_DESCRIPTION_HEADER, Length);
constexpr std::size_t header_checksum_offset =
    offsetof(EFI_ACPI_DESCRIPTION_HEADER, Checksum);
constexpr std::size_t header_size = sizeof(EFI_ACPI_DESCRIPTION_HEADER);
/**
 * @}
 */

static_assert(header_length_offset == 4);
static_assert(header_checksum_offset == 9);
static_assert(header_size == 36);
static_assert(sizeof(EFI_ACPI_DMAR_HEADER) == 48,
              "VT-d 5.20 8.1 puts the first remapping structure at 48");
static_assert(sizeof(EFI_ACPI_DMAR_RMRR_HEADER) == 24,
              "VT-d 5.20 8.4: length is 24 + the device scope");
static_assert(sizeof(EFI_ACPI_DMAR_DEVICE_SCOPE_STRUCTURE_HEADER) == 6,
              "VT-d 5.20 8.3.1: length is 6 + 2N path bytes");

/**
 * Rewrites a table's checksum so the whole table sums to zero.
 */
void fix_checksum(std::uint8_t * table, std::size_t length)
{
    table[header_checksum_offset] = 0;
    std::uint8_t sum{};
    for (std::size_t i{}; i < length; ++i) {
        sum = static_cast<std::uint8_t>(sum + table[i]);
    }
    table[header_checksum_offset] = static_cast<std::uint8_t>(0u - sum);
}

/**
 * Whether a table's checksum is already right.
 */
bool checksum_valid(const std::uint8_t * table, std::size_t length)
{
    std::uint8_t sum{};
    for (std::size_t i{}; i < length; ++i) {
        sum = static_cast<std::uint8_t>(sum + table[i]);
    }
    return 0 == sum;
}

/**
 * Depth first walk of the PCI hierarchy for the first NVM Express
 * controller, recording the path to it as it goes.
 *
 * Bounded twice over - by `max_path_depth` and by requiring a bridge's
 * secondary bus number to be greater than the bus it was found on, which
 * is what stops a misprogrammed or emulated bridge that points at itself
 * from being walked forever.
 */
bool find_controller(std::uint8_t bus,
                     std::uint8_t depth,
                     device_path & found)
{
    if (depth >= max_path_depth) {
        return false;
    }

    for (std::uint32_t device{}; device < pci_config::max_devices;
         ++device) {
        auto functions = pci_config::max_functions;
        for (std::uint32_t function{}; function < functions; ++function) {
            pci_config::address at{bus, device, function};
            auto vendor =
                pci_config::read32(at, pci_config::vendor_id_offset);
            if ((0xffffffffu == vendor) || (0 == (vendor & 0xffff))) {
                if (0 == function) {
                    // Function zero absent means the device is absent.
                    break;
                }
                continue;
            }

            auto header = pci_config::read8(at, header_type_offset);
            if ((0 == function) &&
                (0 == (header & header_type_multi_function))) {
                functions = 1;
            }

            found.node[depth].Device = static_cast<std::uint8_t>(device);
            found.node[depth].Function =
                static_cast<std::uint8_t>(function);

            auto classes =
                pci_config::read32(at, pci_config::class_revision_offset);
            if (class_nvme == (classes >> 8)) {
                found.depth = static_cast<std::uint8_t>(depth + 1);
                found.bus = bus;
                found.device = static_cast<std::uint8_t>(device);
                found.function = static_cast<std::uint8_t>(function);
                return true;
            }

            if (header_type_bridge == (header & header_type_mask)) {
                auto secondary =
                    pci_config::read8(at, secondary_bus_offset);
                if (secondary > bus) {
                    if (find_controller(
                            secondary,
                            static_cast<std::uint8_t>(depth + 1),
                            found)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/**
 * Whether one device scope entry covers the path.
 *
 * A type 01h entry has to name the endpoint exactly. A type 02h entry
 * names a PCI-PCI bridge, and VT-d 5.20 8.3.1 defines the sub-hierarchy
 * it stands for as "the collection of PCI controllers that are downstream
 * to a specific PCI-PCI bridge" - so it covers the path when its own path
 * is a strict prefix of ours.
 */
bool scope_covers(const std::uint8_t * entry, const device_path & path)
{
    auto type = entry[0];
    auto length = entry[1];
    if (length < 6) {
        return false;
    }
    auto pairs = static_cast<std::size_t>(length - 6) / 2;
    if ((0 == pairs) || (pairs > path.depth)) {
        return false;
    }

    auto start_bus = entry[offsetof(
        EFI_ACPI_DMAR_DEVICE_SCOPE_STRUCTURE_HEADER, StartBusNumber)];
    if (start_bus != path.start_bus) {
        return false;
    }

    if (EFI_ACPI_DEVICE_SCOPE_ENTRY_TYPE_PCI_ENDPOINT == type) {
        if (pairs != path.depth) {
            return false;
        }
    } else if (EFI_ACPI_DEVICE_SCOPE_ENTRY_TYPE_PCI_BRIDGE == type) {
        if (pairs >= path.depth) {
            return false;
        }
    } else {
        return false;
    }

    const auto * bytes = entry + 6;
    for (std::size_t i{}; i < pairs; ++i) {
        if ((bytes[i * 2] != path.node[i].Device) ||
            (bytes[(i * 2) + 1] != path.node[i].Function)) {
            return false;
        }
    }
    return true;
}

/**
 * Walks a DMAR structure list, checking that every structure's length is
 * sane and that the list ends exactly at the table's end. A structure
 * whose length is zero would otherwise spin, and one that runs past the
 * end would read whatever follows the table.
 */
bool structures_parse(const std::uint8_t * dmar, std::uint32_t length)
{
    if (length < sizeof(EFI_ACPI_DMAR_HEADER)) {
        return false;
    }
    auto offset = std::size_t{sizeof(EFI_ACPI_DMAR_HEADER)};
    while (offset < length) {
        if ((offset + 4) > length) {
            return false;
        }
        auto size = load16(dmar + offset + 2);
        if ((size < 4) || ((offset + size) > length)) {
            return false;
        }
        offset += size;
    }
    return offset == length;
}

/**
 * Where a type 1 structure has to go.
 *
 * VT-d 5.20 8.2: "BIOS implementations must report these remapping
 * structure types in numerical order. i.e., All remapping structures of
 * type 0 (DRHD) enumerated before remapping structures of type 1 (RMRR),
 * and so forth." So an RMRR goes after the last DRHD or RMRR and before
 * the first ATSR, RHSA, ANDD, SATC or SIDP. Appending at the end of the
 * table is a specification violation whenever any of those is present,
 * and a parser that trusts the ordering to stop early would not see the
 * region at all.
 */
std::size_t rmrr_insertion_offset(const std::uint8_t * dmar,
                                  std::uint32_t length)
{
    auto offset = std::size_t{sizeof(EFI_ACPI_DMAR_HEADER)};
    while (offset < length) {
        auto type = load16(dmar + offset);
        if (type > EFI_ACPI_DMAR_TYPE_RMRR) {
            return offset;
        }
        offset += load16(dmar + offset + 2);
    }
    return length;
}

/**
 * Whether some remapping hardware unit already has the device in scope,
 * and which one.
 *
 * VT-d 5.20 8.4 requires it: the devices an RMRR names "must be devices
 * under the scope of one of the remapping hardware units reported in
 * DRHD". A region scoped to a device no unit covers is a region no unit
 * applies, which would leave the channel believing in a reservation that
 * does nothing.
 *
 * An explicit device scope entry wins over INCLUDE_PCI_ALL. 8.3 says a
 * unit with that flag "has under its scope all PCI compatible devices in
 * the specified Segment, **except devices reported under the scope of
 * other remapping hardware units for the same Segment**", so the catch-all
 * is only the answer when nothing else claimed the device.
 */
bool find_scoping_unit(const std::uint8_t * dmar,
                       std::uint32_t length,
                       const device_path & path,
                       std::uint64_t & registers,
                       bool & by_catch_all)
{
    std::uint64_t catch_all{};
    bool have_catch_all{};

    auto offset = std::size_t{sizeof(EFI_ACPI_DMAR_HEADER)};
    while (offset < length) {
        auto type = load16(dmar + offset);
        auto size = load16(dmar + offset + 2);
        if (EFI_ACPI_DMAR_TYPE_DRHD != type) {
            offset += size;
            continue;
        }

        auto flags =
            dmar[offset + offsetof(EFI_ACPI_DMAR_DRHD_HEADER, Flags)];
        auto segment =
            load16(dmar + offset +
                   offsetof(EFI_ACPI_DMAR_DRHD_HEADER, SegmentNumber));
        auto base = load64(
            dmar + offset +
            offsetof(EFI_ACPI_DMAR_DRHD_HEADER, RegisterBaseAddress));

        // Segment zero only. The configuration mechanism this loader uses
        // reaches segment zero and nothing else, so a device on another
        // one was never found in the first place.
        if (0 != segment) {
            offset += size;
            continue;
        }

        auto scope = offset + sizeof(EFI_ACPI_DMAR_DRHD_HEADER);
        while ((scope + 2) <= (offset + size)) {
            auto entry_length = dmar[scope + 1];
            if ((entry_length < 6) ||
                ((scope + entry_length) > (offset + size))) {
                break;
            }
            if (scope_covers(dmar + scope, path)) {
                registers = base;
                by_catch_all = false;
                return true;
            }
            scope += entry_length;
        }

        if (0 != (flags & EFI_ACPI_DMAR_DRHD_FLAGS_INCLUDE_PCI_ALL)) {
            catch_all = base;
            have_catch_all = true;
        }

        offset += size;
    }

    if (have_catch_all) {
        registers = catch_all;
        by_catch_all = true;
        return true;
    }
    return false;
}

/**
 * Whether an RMRR already names this device, so we do not add a second
 * one describing a different range. Firmware that already declares a
 * region for the controller has arranged something we do not understand,
 * and the right response is to report it and leave it alone.
 */
bool existing_region_for(const std::uint8_t * dmar,
                         std::uint32_t length,
                         const device_path & path)
{
    auto offset = std::size_t{sizeof(EFI_ACPI_DMAR_HEADER)};
    while (offset < length) {
        auto type = load16(dmar + offset);
        auto size = load16(dmar + offset + 2);
        if (EFI_ACPI_DMAR_TYPE_RMRR != type) {
            offset += size;
            continue;
        }
        auto scope = offset + sizeof(EFI_ACPI_DMAR_RMRR_HEADER);
        while ((scope + 2) <= (offset + size)) {
            auto entry_length = dmar[scope + 1];
            if ((entry_length < 6) ||
                ((scope + entry_length) > (offset + size))) {
                break;
            }
            if (scope_covers(dmar + scope, path)) {
                return true;
            }
            scope += entry_length;
        }
        offset += size;
    }
    return false;
}

/**
 * The ACPI 2.0 and later configuration table GUID, which is the one that
 * carries an XSDT. Compared in full rather than by its first doubleword:
 * this decides which memory gets rewritten.
 */
constexpr EFI_GUID acpi_table_guid = EFI_ACPI_TABLE_GUID;

bool same_guid(const EFI_GUID & left, const EFI_GUID & right)
{
    const auto * a = reinterpret_cast<const std::uint8_t *>(&left);
    const auto * b = reinterpret_cast<const std::uint8_t *>(&right);
    for (std::size_t i{}; i < sizeof(EFI_GUID); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/**
 * Repoints every entry of a root system description table that names
 * `from` at `to`, and fixes that table's checksum.
 *
 * `width` is 8 for an XSDT and 4 for an RSDT. Both are walked, because a
 * firmware that publishes an RSDT as well as an XSDT has two lists that
 * have to agree - and an operating system is free to read either.
 */
std::uint32_t repoint(std::uint8_t * table,
                      std::size_t width,
                      std::uint64_t from,
                      std::uint64_t to)
{
    auto length = load32(table + header_length_offset);
    if (length < header_size) {
        return 0;
    }

    std::uint32_t changed{};
    for (auto offset = header_size; (offset + width) <= length;
         offset += width) {
        auto value =
            (8 == width) ? load64(table + offset) : load32(table + offset);
        if (value != from) {
            continue;
        }
        if (8 == width) {
            store64(table + offset, to);
        } else {
            store32(table + offset, static_cast<std::uint32_t>(to));
        }
        ++changed;
    }

    if (changed) {
        fix_checksum(table, length);
    }
    return changed;
}

} // namespace

void reserved_region::execute(EFI_SYSTEM_TABLE * system_table)
{
    trace::line("rmrr: begin");

    if (!system_table || !system_table->BootServices) {
        trace::line("rmrr: no boot services");
        return;
    }
    auto * boot = system_table->BootServices;

    // ---- the root pointer --------------------------------------------
    const std::uint8_t * rsdp{};
    for (std::size_t i{}; i < system_table->NumberOfTableEntries; ++i) {
        auto & entry = system_table->ConfigurationTable[i];
        if (same_guid(entry.VendorGuid, acpi_table_guid)) {
            rsdp = static_cast<const std::uint8_t *>(entry.VendorTable);
            break;
        }
    }
    if (!rsdp) {
        trace::line("rmrr: no acpi 2.0 root pointer, refused");
        return;
    }

    const auto * pointer = reinterpret_cast<
        const EFI_ACPI_6_5_ROOT_SYSTEM_DESCRIPTION_POINTER *>(rsdp);
    auto xsdt_address = pointer->XsdtAddress;
    auto rsdt_address = std::uint64_t{pointer->RsdtAddress};
    trace::hex_line("rmrr: rsdp revision ", pointer->Revision);
    trace::hex_line("rmrr: xsdt ", xsdt_address);
    trace::hex_line("rmrr: rsdt ", rsdt_address);

    if (!xsdt_address) {
        trace::line("rmrr: no xsdt, refused");
        return;
    }
    auto * xsdt = reinterpret_cast<std::uint8_t *>(xsdt_address);

    // ---- the DMAR table ----------------------------------------------
    constexpr std::uint32_t dmar_signature = 0x52414d44; // 'DMAR'
    std::uint64_t dmar_address{};
    auto xsdt_length = load32(xsdt + header_length_offset);
    for (auto offset = header_size; (offset + 8) <= xsdt_length;
         offset += 8) {
        auto candidate = load64(xsdt + offset);
        if (!candidate) {
            continue;
        }
        if (dmar_signature ==
            load32(reinterpret_cast<const std::uint8_t *>(candidate))) {
            dmar_address = candidate;
            break;
        }
    }
    if (!dmar_address) {
        trace::line("rmrr: no DMAR table, so no remapping hardware is "
                    "described - nothing to reserve against");
        return;
    }

    auto * dmar = reinterpret_cast<std::uint8_t *>(dmar_address);
    auto dmar_length = load32(dmar + header_length_offset);
    trace::hex_line("rmrr: dmar at ", dmar_address);
    trace::hex_line("rmrr: dmar length ", dmar_length);
    if (!checksum_valid(dmar, dmar_length)) {
        trace::line("rmrr: dmar checksum wrong, refused");
        return;
    }
    if (!structures_parse(dmar, dmar_length)) {
        trace::line("rmrr: dmar structures do not parse, refused");
        return;
    }
    trace::hex_line("rmrr: dmar flags ",
                    dmar[offsetof(EFI_ACPI_DMAR_HEADER, Flags)]);

    // ---- the controller ----------------------------------------------
    device_path path{};
    if (!find_controller(0, 0, path)) {
        trace::line("rmrr: no nvme controller found, refused");
        return;
    }
    trace::hex_line("rmrr: controller bus ", path.bus);
    trace::hex_line("rmrr: controller device ", path.device);
    trace::hex_line("rmrr: controller function ", path.function);
    trace::hex_line("rmrr: path depth ", path.depth);
    for (std::size_t i{}; i < path.depth; ++i) {
        trace::hex_line("rmrr: path device ", path.node[i].Device);
        trace::hex_line("rmrr: path function ", path.node[i].Function);
    }

    std::uint64_t unit{};
    bool by_catch_all{};
    if (!find_scoping_unit(dmar, dmar_length, path, unit, by_catch_all)) {
        trace::line("rmrr: controller is under no remapping unit's "
                    "scope, refused - VT-d 8.4 requires it");
        return;
    }
    trace::hex_line(by_catch_all
                        ? "rmrr: scoped by INCLUDE_PCI_ALL unit at "
                        : "rmrr: scoped by explicit unit at ",
                    unit);

    if (existing_region_for(dmar, dmar_length, path)) {
        trace::line("rmrr: firmware already declares a region for this "
                    "controller, refused - leaving it alone");
        return;
    }

    // ---- the window ---------------------------------------------------
    // Reserved, which is what VT-d 5.20 8.4 requires of the range an RMRR
    // describes: "BIOS must report the RMRR reported memory addresses as
    // reserved (or as EFI runtime) in the system memory map returned
    // through methods such as INT15, EFI GetMemoryMap etc." The same
    // memory type allocate_rwx uses, for a related reason - it is the one
    // kind of memory no operating system may account for or reuse.
    EFI_PHYSICAL_ADDRESS window_address{};
    auto status = boot->AllocatePages(AllocateAnyPages,
                                      EfiReservedMemoryType,
                                      window_pages,
                                      &window_address);
    if (EFI_ERROR(status)) {
        trace::line("rmrr: window allocation failed, refused");
        return;
    }
    for (std::size_t i{}; i < (window_pages * EFI_PAGE_SIZE); ++i) {
        reinterpret_cast<std::uint8_t *>(window_address)[i] = 0;
    }
    trace::hex_line("rmrr: window base ", window_address);
    trace::hex_line("rmrr: window length ", window_pages * EFI_PAGE_SIZE);

    // ---- the structure ------------------------------------------------
    // Type 01h, PCI Endpoint Device, naming the controller itself rather
    // than type 02h naming the root port above it. Both were considered
    // and RESERVED-REGION.md gives the argument; the short form is that
    // 8.4 describes the device scope as identifying "devices requiring
    // access to the specified reserved memory region", a sub-hierarchy
    // entry puts the same hole in the address space of every device
    // behind the bridge rather than in one, and the usual reason to
    // prefer 02h - bus renumbering - does not apply, because 8.3.1
    // resolves a path through each bridge's secondary bus register rather
    // than by remembering a bus number.
    constexpr std::uint8_t scope_type =
        EFI_ACPI_DEVICE_SCOPE_ENTRY_TYPE_PCI_ENDPOINT;

    auto scope_length =
        sizeof(EFI_ACPI_DMAR_DEVICE_SCOPE_STRUCTURE_HEADER) +
        (std::size_t{path.depth} * 2);
    auto structure_length =
        sizeof(EFI_ACPI_DMAR_RMRR_HEADER) + scope_length;

    std::uint8_t
        structure[sizeof(EFI_ACPI_DMAR_RMRR_HEADER) +
                  sizeof(EFI_ACPI_DMAR_DEVICE_SCOPE_STRUCTURE_HEADER) +
                  (max_path_depth * 2)]{};

    auto limit = window_address + (window_pages * EFI_PAGE_SIZE) - 1;

    // VT-d 5.20 8.4: base 4 KB aligned, "value in this field must be
    // greater than the value in Reserved Memory Region Base Address
    // field", and "(Limit - Base + 1) must be an integer multiple of
    // 4KB". Asserted rather than assumed, since a region that fails any
    // of the three is one the guest may reject wholesale - taking the
    // firmware's own regions with it.
    if ((0 != (window_address & 0xfff)) || (limit <= window_address) ||
        (0 != ((limit - window_address + 1) & 0xfff))) {
        trace::line("rmrr: window fails the 8.4 shape, refused");
        return;
    }

    store32(structure,
            EFI_ACPI_DMAR_TYPE_RMRR |
                (static_cast<std::uint32_t>(structure_length) << 16));
    store64(structure + offsetof(EFI_ACPI_DMAR_RMRR_HEADER,
                                 ReservedMemoryRegionBaseAddress),
            window_address);
    store64(structure + offsetof(EFI_ACPI_DMAR_RMRR_HEADER,
                                 ReservedMemoryRegionLimitAddress),
            limit);

    auto * scope = structure + sizeof(EFI_ACPI_DMAR_RMRR_HEADER);
    scope[0] = scope_type;
    scope[1] = static_cast<std::uint8_t>(scope_length);
    scope[offsetof(EFI_ACPI_DMAR_DEVICE_SCOPE_STRUCTURE_HEADER,
                   StartBusNumber)] = path.start_bus;
    for (std::size_t i{}; i < path.depth; ++i) {
        scope[6 + (i * 2)] = path.node[i].Device;
        scope[7 + (i * 2)] = path.node[i].Function;
    }

    // ---- the rewritten table ------------------------------------------
    // A table cannot grow in place: whatever follows it in ACPI reclaim
    // memory is another table. So a new copy with room, and the lists
    // that name the old one repointed at it.
    //
    // Below four gigabytes on purpose. An RSDT entry is 32 bits wide, so
    // a table allocated above that could be reached from the XSDT and not
    // from the RSDT, and the two lists would disagree about where the
    // DMAR is.
    auto new_length = dmar_length + structure_length;
    auto new_pages = (new_length + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS copy_address = 0xffffffff;
    status = boot->AllocatePages(AllocateMaxAddress,
                                 EfiACPIReclaimMemory,
                                 new_pages,
                                 &copy_address);
    if (EFI_ERROR(status)) {
        trace::line("rmrr: table allocation failed, refused");
        return;
    }

    auto * copy = reinterpret_cast<std::uint8_t *>(copy_address);
    for (std::size_t i{}; i < (new_pages * EFI_PAGE_SIZE); ++i) {
        copy[i] = 0;
    }

    auto insert_at = rmrr_insertion_offset(dmar, dmar_length);
    trace::hex_line("rmrr: inserting at offset ", insert_at);

    copy_bytes(copy, dmar, insert_at);
    copy_bytes(copy + insert_at, structure, structure_length);
    copy_bytes(copy + insert_at + structure_length,
               dmar + insert_at,
               dmar_length - insert_at);

    store32(copy + header_length_offset, new_length);
    fix_checksum(copy, new_length);

    if (!structures_parse(copy, new_length)) {
        trace::line("rmrr: rewritten table does not parse, refused - "
                    "the firmware's own table is untouched");
        return;
    }

    // ---- publish it ----------------------------------------------------
    auto in_xsdt = repoint(xsdt, 8, dmar_address, copy_address);
    std::uint32_t in_rsdt{};
    if (rsdt_address) {
        in_rsdt = repoint(reinterpret_cast<std::uint8_t *>(rsdt_address),
                          4,
                          dmar_address,
                          copy_address);
    }
    trace::hex_line("rmrr: xsdt entries repointed ", in_xsdt);
    trace::hex_line("rmrr: rsdt entries repointed ", in_rsdt);

    if (0 == in_xsdt) {
        trace::line("rmrr: the xsdt does not name the DMAR we found, "
                    "refused - nothing was published");
        return;
    }

    window.physical = window_address;
    window.length = window_pages * EFI_PAGE_SIZE;
    bus = path.bus;
    device = path.device;
    function = path.function;
    unit_registers = unit;

    trace::hex_line("rmrr: new dmar at ", copy_address);
    trace::hex_line("rmrr: new dmar length ", new_length);
    trace::line("rmrr: VERDICT reserved region declared for the "
                "controller");

    // The other half of the strategy, run here read-only, against the
    // register block the DMAR named. Under boot services the address
    // space is identity mapped, so a physical address is a usable
    // pointer and the translation callback is the identity - which is
    // the one part of this the resident side has to supply for real.
    //
    // Not an idle report: it is the only thing that says the register
    // base parsed out of the DRHD is a register block at all rather than
    // an address that happened to decode. `install` is false, so nothing
    // can be written whatever it finds.
    nvme::iommu_gate::reach_request probe{
        .registers = reinterpret_cast<const volatile void *>(unit),
        .bus = path.bus,
        .device = path.device,
        .function = path.function,
        .physical_to_virtual =
            [](std::uint64_t physical) { return physical; },
        .window_physical = window.physical,
        .window_length = window.length,
        .install = false,
    };
    auto verdict = nvme::iommu_gate::reach(probe);
    trace::line("rmrr: gate says");
    trace::line(nvme::iommu_gate::describe(verdict));

    trace::line("rmrr: end");
}

} // namespace zpp

#endif
