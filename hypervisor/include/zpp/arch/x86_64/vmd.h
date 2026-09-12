#pragma once
#include <cstdint>

#include "zpp/arch/x86_64/pci.h"
#include "zpp/arch/x86_64/pci_ecam.h"

namespace zpp::arch::x86_64
{
/**
 * Devices hidden behind Intel Volume Management Device.
 *
 * VMD is a root complex integrated endpoint that takes a set of PCI
 * Express root ports out of the host's configuration space entirely and
 * republishes them in a domain of its own. The consequence for anything
 * that enumerates by walking configuration space is total: a brute force
 * walk of all 256 buses through the two legacy ports finds **nothing**
 * behind it, so a machine that boots from an NVMe drive reports no NVMe
 * controller at all. That is the failure this exists for.
 *
 * Everything below is read only. It finds what is there and refuses when
 * it cannot prove what it found; it programs nothing, and in particular
 * it does not assign bus numbers or windows, because on a machine where
 * this matters the firmware has already done so and is booting from the
 * result.
 *
 * Three properties of the hardware, all taken from Linux's
 * drivers/pci/controller/vmd.c at v6.12, which is the reference
 * implementation:
 *
 * - The children's configuration space is an ECAM shaped window at the
 *   endpoint's BAR0 - VMD_CFGBAR is 0 there, and vmd_cfg_addr indexes it
 *   with PCIE_ECAM_OFFSET. So one megabyte per bus, and `pci_ecam` reads
 *   it unchanged.
 * - The window's first bus is not necessarily bus zero.
 *   vmd_get_bus_number_start reads a capability word at 0x40 and, if its
 *   low bit is set, a configuration word at 0x44 whose bits 9:8 select a
 *   start of 0, 128 or 224. A fourth encoding exists and that function
 *   fails on it; so does this.
 * - Children's BARs live inside the endpoint's own MEMBAR1 (BAR2) and
 *   MEMBAR2 (BAR4) and are real host physical addresses, so a child's
 *   register block is used exactly as any other device's would be. No
 *   translation is applied here and none is needed.
 *
 * **A child's requester id is not its own.** DMA and MSI from anything
 * behind VMD are sourced with the endpoint's requester id, which is what
 * pci_real_dma_dev in arch/x86/pci/common.c returns for a device on a VMD
 * bus - the VMD's own pci_dev rather than the device's. Anything that
 * scopes a DMAR device scope entry, or reasons about which IOMMU domain a
 * device is in, must therefore name the endpoint. `domain::endpoint` is
 * that address and it is deliberately kept separate from the child's own
 * bus, device and function, which are only meaningful inside the VMD
 * domain and mean something else entirely outside it.
 *
 * Matched by class and vendor rather than by a device identifier list.
 * vmd.c carries such a list, but it needs one for a different reason: its
 * entries select per-silicon feature flags - shadow membars, bus
 * restrictions, MSI remap bypass - and none of those features are used
 * here. A list is a liability for a plain read only enumeration, since it
 * is wrong for every part that ships after it was written, and the
 * evidence that a device is a VMD is available directly: an Intel RAID
 * bus controller whose BAR0 is a memory window that decodes as
 * configuration space, holding a child of the class being looked for. A
 * device that fails any of that is refused, so a mismatch costs a scan
 * and nothing else.
 */
class vmd
{
public:
    /**
     * The class code a VMD endpoint reports, in the layout the class and
     * revision doubleword has once its revision byte is shifted out: base
     * class 01h mass storage, subclass 04h RAID bus controller,
     * programming interface 00h.
     */
    static constexpr std::uint32_t endpoint_class = 0x010400;

    /**
     * Whose silicon it has to be. The class alone is shared with every
     * RAID controller ever made.
     */
    static constexpr std::uint16_t endpoint_vendor = 0x8086;

    /**
     * The two vendor specific registers vmd_get_bus_number_start reads,
     * and the fields it takes out of them.
     * @{
     */
    static constexpr std::uint32_t capability_offset = 0x40;
    static constexpr std::uint32_t configuration_offset = 0x44;
    static constexpr std::uint16_t bus_restrict_capable = 0x1;
    static constexpr std::uint32_t bus_restrict_shift = 8;
    static constexpr std::uint32_t bus_restrict_mask = 0x3;
    /**
     * @}
     */

    /**
     * How many buses of the window to scan.
     *
     * The window is thirty two megabytes on the parts this was written
     * for, which is thirty two buses, and its true size can only be
     * learned by writing all ones into the BAR and reading back what
     * sticks. That is a write to a live device the firmware is booting
     * from, so it is not done: the size is bounded from above instead,
     * from two facts that cost nothing. A BAR is naturally aligned, so
     * the lowest set bit of its base is an upper bound on its size; and
     * vmd_probe refuses a CFGBAR smaller than one megabyte, so one bus is
     * the floor. The scan takes the smaller of that bound and this cap.
     *
     * Scanning past the end of the window would be reads of addresses
     * belonging to something else, and a read is not free of consequence
     * on a device register.
     */
    static constexpr std::uint32_t max_child_buses = 32;

    /**
     * One VMD endpoint and the configuration space it publishes.
     */
    struct domain
    {
        /**
         * The endpoint's own bus, device and function, in the host's
         * configuration space. This is the requester id every child's DMA
         * and every child's message signalled interrupt carries.
         */
        pci_config::address endpoint{};

        /**
         * The children's configuration space.
         */
        pci_ecam config{};

        /**
         * The bus number the window's first bus is called inside the
         * domain, and how many buses of it are scanned.
         * @{
         */
        std::uint32_t bus_start{};
        std::uint32_t bus_count{};
        /**
         * @}
         */

        constexpr bool valid() const
        {
            return config.valid() && (0 != bus_count);
        }

        /**
         * Turns a bus number inside the domain into the window relative
         * address `pci_ecam` indexes by.
         */
        constexpr pci_config::address
        window_address(const pci_config::address & child) const
        {
            return pci_config::address{
                child.bus - bus_start, child.device, child.function};
        }

        /**
         * Reads a child's configuration space by its domain address.
         * @{
         */
        std::uint32_t read32(const pci_config::address & child,
                             std::uint32_t offset) const
        {
            return config.read32(window_address(child), offset);
        }

        std::uint16_t read16(const pci_config::address & child,
                             std::uint32_t offset) const
        {
            return config.read16(window_address(child), offset);
        }

        std::uint8_t read8(const pci_config::address & child,
                           std::uint32_t offset) const
        {
            return config.read8(window_address(child), offset);
        }
        /**
         * @}
         */
    };

    /**
     * Whether a function even looks like a VMD endpoint. Two register
     * reads, so that the full decode below is only paid for by something
     * that could be one.
     */
    static bool describes(const pci_config::address & at)
    {
        auto vendor = pci_config::read32(at, pci_config::vendor_id_offset);
        if (endpoint_vendor != (vendor & 0xffff)) {
            return false;
        }
        auto classes =
            pci_config::read32(at, pci_config::class_revision_offset);
        return endpoint_class == (classes >> 8);
    }

    /**
     * Everything that has to hold before the window at BAR0 may be read
     * as configuration space. Each refusal is a thing that could not be
     * validated rather than a thing known to be wrong, which is the
     * difference that matters: a machine with no VMD, and a machine with
     * a VMD this code does not understand, both end up refusing here
     * rather than interpreting whatever the window happens to hold.
     */
    static bool decode(const pci_config::address & at, domain & found)
    {
        if (!describes(at)) {
            return false;
        }

        auto low =
            pci_config::read32(at, pci_config::base_address_0_offset);
        auto high =
            pci_config::read32(at, pci_config::base_address_0_offset + 4);

        // CFGBAR is a 64 bit memory BAR. An IO BAR, or a 32 bit one, is
        // not the thing being looked for and is not read as if it were.
        if (0 != (low & 1)) {
            return false;
        }
        if (pci_config::base_address_type_64_bit !=
            (low & pci_config::base_address_type_mask)) {
            return false;
        }

        auto base = (std::uint64_t{high} << 32) |
                    (low & pci_config::base_address_memory_mask);

        // An unassigned window is not an address, and reading one would
        // be reading the low megabyte of physical memory.
        if (0 == base) {
            return false;
        }

        // Nothing decodes if the endpoint is not decoding. Read rather
        // than set: the firmware owns this device and is booting from
        // what is behind it.
        auto command = pci_config::read16(at, pci_config::command_offset);
        if (0 == (command & pci_config::command_memory_space)) {
            return false;
        }

        std::uint32_t bus_start{};
        if (!bus_start_of(at, bus_start)) {
            return false;
        }

        // The alignment bound described on max_child_buses. The lowest
        // set bit of the base is at least the window's size, so dividing
        // it by the bytes one bus takes never overstates the bus count.
        auto alignment = base & (~base + 1);
        auto bound = alignment / pci_ecam::bytes_per_bus;
        if (0 == bound) {
            // Not even one bus wide, so it is not a configuration window
            // whatever else it is.
            return false;
        }

        auto count = (bound < max_child_buses)
                         ? static_cast<std::uint32_t>(bound)
                         : max_child_buses;
        if ((bus_start + count) > pci_config::max_buses) {
            count = pci_config::max_buses - bus_start;
        }
        if (0 == count) {
            return false;
        }

        found.endpoint = at;
        found.config = pci_ecam{base, count * pci_ecam::bytes_per_bus};
        found.bus_start = bus_start;
        found.bus_count = count;
        return found.valid() && first_bus_decodes(found);
    }

    /**
     * The first bus of the window has to answer, or the window is not
     * configuration space and every conclusion drawn from it would be
     * drawn from whatever memory is there.
     *
     * A bus that decodes has at least one function whose vendor
     * identifier is neither all ones nor zero. On a VMD that is a root
     * port, since the ports are what the endpoint republishes.
     */
    static bool first_bus_decodes(const domain & of)
    {
        for (std::uint32_t device{}; device < pci_config::max_devices;
             ++device) {
            pci_config::address at{of.bus_start, device, 0};
            auto vendor = of.read32(at, pci_config::vendor_id_offset);
            if ((0xffffffffu != vendor) && (0 != (vendor & 0xffff))) {
                return true;
            }
        }
        return false;
    }

    /**
     * Where the window's buses are numbered from.
     *
     * vmd_get_bus_number_start, with its default of zero when the part
     * does not report the capability at all, and its refusal of the
     * fourth encoding.
     */
    static bool bus_start_of(const pci_config::address & at,
                             std::uint32_t & start)
    {
        start = 0;

        auto capability = pci_config::read16(at, capability_offset);
        if (0 == (capability & bus_restrict_capable)) {
            return true;
        }

        auto configuration = pci_config::read16(at, configuration_offset);
        switch ((configuration >> bus_restrict_shift) &
                bus_restrict_mask) {
        case 0:
            start = 0;
            return true;
        case 1:
            start = 128;
            return true;
        case 2:
            start = 224;
            return true;
        default:
            // vmd.c calls this "Unknown Bus Offset Setting" and gives up.
            return false;
        }
    }

    /**
     * The first child of a given class anywhere in the domain, by the
     * same brute force walk the host side uses and for the same reasons:
     * it needs no bridge register to be read or trusted, it reaches a
     * device behind any depth of bridging inside the domain, and an
     * absent bus costs one read per slot.
     */
    static bool find_child(const domain & of,
                           std::uint32_t class_code,
                           pci_config::address & child)
    {
        for (std::uint32_t index{}; index < of.bus_count; ++index) {
            auto bus = of.bus_start + index;
            for (std::uint32_t device{}; device < pci_config::max_devices;
                 ++device) {
                auto functions = pci_config::max_functions;

                for (std::uint32_t function{}; function < functions;
                     ++function) {
                    pci_config::address at{bus, device, function};
                    auto vendor =
                        of.read32(at, pci_config::vendor_id_offset);
                    if ((0xffffffffu == vendor) ||
                        (0 == (vendor & 0xffff))) {
                        if (0 == function) {
                            break;
                        }
                        continue;
                    }

                    if (0 == function) {
                        auto header =
                            of.read8(at, pci_config::header_type_offset);
                        if (0 ==
                            (header & pci_config::multi_function_bit)) {
                            functions = 1;
                        }
                    }

                    auto classes =
                        of.read32(at, pci_config::class_revision_offset);
                    if (class_code == (classes >> 8)) {
                        child = at;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /**
     * The whole search: every VMD endpoint in the host's configuration
     * space, and the first child of the wanted class behind any of them.
     *
     * All buses rather than bus zero. A VMD is root complex integrated
     * and sits at 00:0e.0 on client parts, but a multiple socket machine
     * has one per root complex and those root buses are not zero. The
     * walk costs one read per absent slot, which is what the host side
     * already pays.
     */
    static bool find(std::uint32_t class_code,
                     domain & found,
                     pci_config::address & child)
    {
        for (std::uint32_t bus{}; bus < pci_config::max_buses; ++bus) {
            for (std::uint32_t device{}; device < pci_config::max_devices;
                 ++device) {
                auto functions = pci_config::max_functions;

                for (std::uint32_t function{}; function < functions;
                     ++function) {
                    pci_config::address at{bus, device, function};
                    auto vendor = pci_config::read32(
                        at, pci_config::vendor_id_offset);
                    if ((0xffffffffu == vendor) ||
                        (0 == (vendor & 0xffff))) {
                        if (0 == function) {
                            break;
                        }
                        continue;
                    }

                    if (0 == function) {
                        auto header = pci_config::read8(
                            at, pci_config::header_type_offset);
                        if (0 ==
                            (header & pci_config::multi_function_bit)) {
                            functions = 1;
                        }
                    }

                    domain candidate{};
                    if (!decode(at, candidate)) {
                        continue;
                    }
                    if (find_child(candidate, class_code, child)) {
                        found = candidate;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /**
     * Whether this function is a VMD endpoint with a child of the given
     * class behind it - and therefore the requester id that child's DMA
     * carries.
     *
     * Exists for callers that already walk configuration space for their
     * own reasons and need to know, at one function, whether it stands
     * for a device they are looking for. The reserved region work is the
     * one that needs it: what it has to name in a DMAR device scope entry
     * is the path to *this* function, never the path to the NVMe
     * controller, because the controller's own bus number does not exist
     * outside the VMD domain.
     */
    static bool hosts_class(const pci_config::address & at,
                            std::uint32_t class_code)
    {
        domain candidate{};
        if (!decode(at, candidate)) {
            return false;
        }
        pci_config::address child{};
        return find_child(candidate, class_code, child);
    }
};

} // namespace zpp::arch::x86_64
