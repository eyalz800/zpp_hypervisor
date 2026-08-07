#include "zpp/nvme_selftest.h"

#include "zpp/trace.h"

#if ZPP_DIAG

#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/pci.h"
#include "zpp/arch/x86_64/vmd.h"
#include "zpp/nvme/admin_borrow.h"
#include "zpp/nvme/command.h"
#include "zpp/nvme/iommu_gate.h"
#include "zpp/nvme/log_writer.h"
#include "zpp/nvme/registers.h"

#include <cstring>

namespace zpp
{
namespace
{
using namespace zpp::nvme;
using arch::x86_64::pci_config;
using arch::x86_64::vmd;

/**
 * The class code an NVM Express controller reports, in the layout the
 * class and revision doubleword has once its revision byte is shifted
 * out: base class 01h mass storage, subclass 08h non-volatile memory,
 * programming interface 02h NVM Express.
 */
constexpr std::uint32_t class_nvme = 0x010802;

/**
 * Which queue identifier the test claims.
 *
 * The firmware's NvmExpressDxe creates exactly one I/O queue pair and
 * numbers it 1, so 4 is free with room either side. On a real controller
 * this is what the Set Features completion patch has to manufacture; here
 * it is free because the emulated controller allocates its configured
 * maximum regardless of what is asked, so nothing has to be reserved.
 */
constexpr std::uint16_t test_queue_id = 4;

/**
 * Snapshots of the guest's admin queues, taken before the borrow and
 * compared byte for byte afterwards. That comparison is the whole
 * measurement.
 *
 * File scope rather than local, and not for taste: together these are
 * twenty kilobytes, and a stack frame over one page makes the MSVC ABI
 * emit a call to __chkstk, which does not exist in a freestanding loader.
 * The resolve-once paths in this loader are all written this way.
 * @{
 */
submission_entry g_before_submission[admin_borrow::max_depth]{};
completion_entry g_before_completion[admin_borrow::max_depth]{};
submission_entry g_scratch_submission[admin_borrow::max_depth]{};
completion_entry g_scratch_completion[admin_borrow::max_depth]{};
/**
 * @}
 */

using test_queues = queue_pair<64>;

static_assert(nvme_selftest::queue_storage_bytes ==
                  test_queues::storage_bytes,
              "the storage the loader allocates and the storage the "
              "queue pair lays out have to be the same size");

/**
 * Under UEFI the address space is identity mapped, so a physical address
 * is already a usable pointer and the reverse is equally trivial. Stated
 * as a function rather than assumed at each use, because the resident
 * side has to supply a real translation here and the difference should be
 * visible.
 */
std::uint64_t identity_of(const void * address)
{
    return reinterpret_cast<std::uint64_t>(address);
}

/**
 * Finds the first NVM Express controller on any bus of the host's own
 * configuration space.
 *
 * Every bus, not bus zero. This scanned bus zero alone, on the
 * reasoning `pci.h` still gives for the two configuration ports - that
 * bus zero "is where a host controller integrated into the chipset
 * lives". That is true of the xHCI, which sits at 00:14.0 on every
 * Intel client platform. It is not true of NVMe, which is an ordinary
 * endpoint behind a PCI Express root port: on the development target
 * the root port is 00:1d.4 and the controller is at **02:00.0**, so
 * the scan found nothing and reported no controller on a machine that
 * had just booted from one.
 *
 * The reason that went unnoticed is worth keeping. It was tested under
 * QEMU, where the emulated controller lands at 00:03.0 - on bus zero -
 * so the test passed for a reason that does not hold on any real
 * machine. A pass on emulated hardware says nothing about topology
 * unless the topology was the thing being emulated.
 *
 * A brute force walk rather than a recursive bridge enumeration: it
 * needs no bridge configuration to be read or trusted, it finds a
 * controller behind any depth of bridging, and it cannot be confused
 * by a bridge whose subordinate bus numbers firmware has not yet
 * programmed. Absent buses read back all ones and cost only the read.
 * Functions beyond zero are probed only when the header type says the
 * device has them, which keeps the common case to one read per slot.
 */
bool find_on_host(pci_config::address & found)
{
    for (std::uint32_t bus{}; bus < pci_config::max_buses; ++bus) {
        for (std::uint32_t device{}; device < pci_config::max_devices;
             ++device) {
            auto functions = pci_config::max_functions;

            for (std::uint32_t function{}; function < functions;
                 ++function) {
                pci_config::address at{bus, device, function};
                auto vendor =
                    pci_config::read32(at, pci_config::vendor_id_offset);
                if ((0xffffffffu == vendor) || (0 == (vendor & 0xffff))) {
                    // Function zero absent means the slot is empty, so
                    // there is nothing to probe the other seven for.
                    if (0 == function) {
                        break;
                    }
                    continue;
                }

                if (0 == function) {
                    auto header = static_cast<std::uint8_t>(
                        pci_config::read32(
                            at, pci_config::header_type_offset & ~0x3u) >>
                        (8 * (pci_config::header_type_offset & 0x3)));
                    if (0 == (header & pci_config::multi_function_bit)) {
                        functions = 1;
                    }
                }

                auto classes = pci_config::read32(
                    at, pci_config::class_revision_offset);
                if (class_nvme == (classes >> 8)) {
                    found = at;
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * Where a controller was found, and by which of the two mechanisms.
 *
 * The requester id is a separate field rather than being read off the
 * address, and that separation is the point of this type. A controller
 * behind VMD sources every DMA and every message signalled interrupt with
 * the VMD endpoint's requester id, not its own - see the comment on
 * `zpp::arch::x86_64::vmd`, and pci_real_dma_dev in Linux's
 * arch/x86/pci/common.c, which returns the VMD's pci_dev for any device
 * on a VMD bus. Its own bus number is a number inside the VMD domain and
 * means something else entirely in the host's configuration space, so
 * anything that hands a bus, device and function to remapping hardware -
 * a DMAR device scope entry, a context table lookup, an IOMMU domain -
 * has to be handed `requester` and never `at`.
 */
struct location
{
    /**
     * The controller's own address. Inside the VMD domain when
     * `behind_vmd`, in the host's configuration space otherwise.
     */
    pci_config::address at{};

    /**
     * The address whose requester id its transactions carry, which is
     * the same address when it is not behind anything.
     */
    pci_config::address requester{};

    bool behind_vmd{};

    /**
     * Only meaningful when `behind_vmd`. Holds the window `at` is read
     * through.
     */
    vmd::domain domain{};
};

/**
 * The ordinary mechanism first, then VMD.
 *
 * That order and not the other, because the ordinary scan is what is
 * right on every machine without VMD - which is most of them - and
 * because a VMD decode is only ever reached on a machine where the
 * ordinary scan already came up empty. There is no machine where both
 * find something and the answers differ: VMD takes its root ports out of
 * the host's configuration space, so a controller behind one is not
 * findable by the first scan at all. That is the whole failure being
 * fixed - a machine that had just booted from an NVMe drive reported no
 * NVM Express controller on any bus, because there genuinely was none
 * where the scan was looking.
 */
bool find_controller(location & found)
{
    if (find_on_host(found.at)) {
        found.requester = found.at;
        found.behind_vmd = false;
        return true;
    }

    if (vmd::find(class_nvme, found.domain, found.at)) {
        found.requester = found.domain.endpoint;
        found.behind_vmd = true;
        return true;
    }

    return false;
}

/**
 * One read of the controller's configuration space, whichever mechanism
 * reaches it.
 *
 * Everything below reads configuration space through this rather than
 * through `pci_config` directly, so that the validation chain is the same
 * chain in both cases rather than a second copy of it that could drift.
 */
std::uint32_t read_config(const location & where, std::uint32_t offset)
{
    if (where.behind_vmd) {
        return where.domain.read32(where.at, offset);
    }
    return pci_config::read32(where.at, offset);
}

/**
 * The word inside that doubleword, in the shape pci_config::read16 gives
 * it - the command register shares its doubleword with the status
 * register, so it cannot be read as a doubleword and used as one.
 */
std::uint16_t read_config16(const location & where, std::uint32_t offset)
{
    auto value = read_config(where, offset);
    return static_cast<std::uint16_t>(value >> ((offset & 0x2) * 8));
}

/**
 * The BAR validation chain from NVME-LOG.md, in order, refusing rather
 * than interpreting.
 *
 * The failure this exists for was found on the parallel xHCI work: a
 * controller whose BAR is unassigned reads as a 64 bit memory window with
 * every address bit clear, and sizing code that only checks which bits
 * stick accepts a base of zero. For NVMe that would put "doorbells" into
 * the real mode interrupt vector table.
 */
bool validate_bar(const location & where, std::uint64_t & base)
{
    auto low = read_config(where, pci_config::base_address_0_offset);
    auto high = read_config(where, pci_config::base_address_0_offset + 4);

    if (0 != (low & 1)) {
        trace::line("selftest: bar0 is io space, refused");
        return false;
    }
    if (pci_config::base_address_type_64_bit !=
        (low & pci_config::base_address_type_mask)) {
        trace::line("selftest: bar0 is not a 64 bit window, refused");
        return false;
    }

    auto candidate = (static_cast<std::uint64_t>(high) << 32) |
                     (low & pci_config::base_address_memory_mask);

    // An unassigned window is not an address.
    if (0 == candidate) {
        trace::line("selftest: bar0 unassigned, refused");
        return false;
    }
    if (0 != (candidate & 0xfff)) {
        trace::hex_line("selftest: bar0 not page aligned, refused ",
                        candidate);
        return false;
    }

    auto command = read_config16(where, pci_config::command_offset);
    if (0 == (command & pci_config::command_memory_space)) {
        trace::line("selftest: memory space disabled, refused - the "
                    "guest owns this and we do not enable it");
        return false;
    }
    if (0 == (command & pci_config::command_bus_master)) {
        trace::line("selftest: bus master disabled, refused");
        return false;
    }

    base = candidate;
    return true;
}

/**
 * The last of the chain, and the one that catches an address that looks
 * plausible and is not: read CAP back through the mapping and refuse on
 * zero or all ones, then check what a real controller must say.
 */
bool validate_capabilities(volatile void * bar)
{
    auto raw = arch::x86_64::read64(bar);
    if ((0 == raw) || (0xffffffffffffffffull == raw)) {
        trace::hex_line("selftest: CAP reads as nothing, refused ", raw);
        return false;
    }

    controller_capabilities capabilities{raw};
    if (0 == capabilities.maximum_queue_entries_field()) {
        trace::line("selftest: CAP.MQES is zero, refused");
        return false;
    }
    if (capabilities.doorbell_stride() > 0xf) {
        trace::line("selftest: CAP.DSTRD out of range, refused");
        return false;
    }
    return true;
}

void compare_and_report(std::uint32_t submission_depth,
                        std::uint32_t completion_depth)
{
    std::uint32_t submission_differences{};
    for (std::uint32_t i{}; i < submission_depth; ++i) {
        const auto * a = reinterpret_cast<const std::uint8_t *>(
            &g_before_submission[i]);
        const auto * b = reinterpret_cast<const std::uint8_t *>(
            &g_scratch_submission[i]);
        for (std::size_t byte{}; byte < sizeof(submission_entry); ++byte) {
            if (a[byte] != b[byte]) {
                ++submission_differences;
                break;
            }
        }
    }

    std::uint32_t completion_differences{};
    for (std::uint32_t i{}; i < completion_depth; ++i) {
        const auto * a = reinterpret_cast<const std::uint8_t *>(
            &g_before_completion[i]);
        const auto * b = reinterpret_cast<const std::uint8_t *>(
            &g_scratch_completion[i]);
        for (std::size_t byte{}; byte < sizeof(completion_entry); ++byte) {
            if (a[byte] != b[byte]) {
                ++completion_differences;
                break;
            }
        }
    }

    trace::hex_line("selftest: admin sq entries differing ",
                    submission_differences);
    trace::hex_line("selftest: admin cq entries differing ",
                    completion_differences);

    if ((0 == submission_differences) && (0 == completion_differences)) {
        trace::line("selftest: VERDICT borrow restored the admin queue "
                    "byte for byte");
    } else {
        trace::line("selftest: VERDICT FAILED - the admin queue was not "
                    "restored");
    }
}

} // namespace

void nvme_selftest::execute(const nvme::log_target & destination,
                            void * queue_storage)
{
    trace::line("selftest: begin");

    location controller{};
    if (!find_controller(controller)) {
        trace::line("selftest: no nvme controller on any bus, and none "
                    "behind any volume management device either");
        return;
    }

    if (controller.behind_vmd) {
        trace::line("selftest: found behind a volume management device");
        trace::hex_line("selftest: vmd endpoint bus ",
                        controller.domain.endpoint.bus);
        trace::hex_line("selftest: vmd endpoint device ",
                        controller.domain.endpoint.device);
        trace::hex_line("selftest: vmd endpoint function ",
                        controller.domain.endpoint.function);
        trace::hex_line("selftest: vmd cfgbar ",
                        controller.domain.config.base());
        trace::hex_line("selftest: vmd first child bus ",
                        controller.domain.bus_start);
        trace::hex_line("selftest: vmd child buses scanned ",
                        controller.domain.bus_count);
    } else {
        trace::line("selftest: found by the ordinary configuration space "
                    "scan");
    }

    trace::hex_line("selftest: controller at bus ", controller.at.bus);
    trace::hex_line("selftest: controller at device ",
                    controller.at.device);
    trace::hex_line("selftest: controller at function ",
                    controller.at.function);

    // Separately, and loudly, because it is the one that is not obvious
    // and the one remapping hardware is programmed with. Equal to the
    // controller's own address on an ordinary machine; the VMD
    // endpoint's on a machine controller the controller is behind one.
    trace::hex_line("selftest: dma requester bus ",
                    controller.requester.bus);
    trace::hex_line("selftest: dma requester device ",
                    controller.requester.device);
    trace::hex_line("selftest: dma requester function ",
                    controller.requester.function);

    std::uint64_t base{};
    if (!validate_bar(controller, base)) {
        return;
    }
    trace::hex_line("selftest: bar0 ", base);

    // Identity mapped while boot services are alive, so the physical
    // address is the mapping. The resident side maps it explicitly.
    auto * bar = reinterpret_cast<volatile void *>(base);
    if (!validate_capabilities(bar)) {
        return;
    }

    auto * bytes = static_cast<volatile std::uint8_t *>(bar);
    controller_capabilities capabilities{arch::x86_64::read64(bar)};
    auto stride = capabilities.doorbell_stride();

    auto configuration = controller_configuration{arch::x86_64::read32(
        bytes + offset_of(register_offset::configuration))};
    auto status = controller_status{
        arch::x86_64::read32(bytes + offset_of(register_offset::status))};
    auto attributes = admin_queue_attributes{arch::x86_64::read32(
        bytes + offset_of(register_offset::admin_queue_attributes))};
    auto submission_base = arch::x86_64::read64(
        bytes + offset_of(register_offset::admin_submission_queue_base));
    auto completion_base = arch::x86_64::read64(
        bytes + offset_of(register_offset::admin_completion_queue_base));

    trace::hex_line("selftest: CAP.MQES ",
                    capabilities.maximum_queue_entries());
    trace::hex_line("selftest: CAP.DSTRD ", stride);
    trace::hex_line("selftest: CC.EN ", configuration.enable() ? 1 : 0);
    trace::hex_line("selftest: CSTS.RDY ", status.ready() ? 1 : 0);
    trace::hex_line("selftest: AQA.ASQS entries ",
                    attributes.submission_queue_size());
    trace::hex_line("selftest: AQA.ACQS entries ",
                    attributes.completion_queue_size());
    trace::hex_line("selftest: ASQ ", submission_base);
    trace::hex_line("selftest: ACQ ", completion_base);

    if (!configuration.enable() || !status.ready()) {
        trace::line("selftest: controller not enabled and ready, so "
                    "there is no live driver to borrow from");
        return;
    }
    if (status.fatal_status()) {
        trace::line("selftest: CSTS.CFS set, refusing to borrow");
        return;
    }

    // The remapping hardware gate. The loader does not parse the ACPI
    // DMAR table yet, so this is exercised with no register block and
    // reports that rather than pretending to a verdict it did not reach.
    // Stated loudly because a channel whose precondition was never
    // actually checked is exactly what NVME-LOG.md forbids.
    //
    // The requester id rather than the controller's own address, for the
    // reason `location` gives: a context table is indexed by the id the
    // transaction actually carries. It changes nothing today, since the
    // register block is null and the gate reports that, but the wrong
    // value here would be a wrong lookup the moment one is parsed.
    auto verdict = iommu_gate::verdict_for(
        nullptr,
        static_cast<std::uint8_t>(controller.requester.bus),
        static_cast<std::uint8_t>(controller.requester.device),
        static_cast<std::uint8_t>(controller.requester.function),
        nullptr);
    trace::line("selftest: iommu gate (no DMAR parsed yet)");
    trace::line(iommu_gate::describe(verdict));

    admin_borrow::queues where{};
    where.submission =
        reinterpret_cast<submission_entry *>(submission_base);
    where.completion =
        reinterpret_cast<completion_entry *>(completion_base);
    where.submission_depth = attributes.submission_queue_size();
    where.completion_depth = attributes.completion_queue_size();

    if ((0 == where.submission_depth) ||
        (where.submission_depth > admin_borrow::max_depth) ||
        (0 == where.completion_depth) ||
        (where.completion_depth > admin_borrow::max_depth)) {
        trace::line("selftest: admin queue depth outside the bound");
        return;
    }

    // Snapshot first, because everything after this is the measurement.
    for (std::uint32_t i{}; i < where.submission_depth; ++i) {
        g_before_submission[i] = where.submission[i];
    }
    for (std::uint32_t i{}; i < where.completion_depth; ++i) {
        g_before_completion[i] = where.completion[i];
    }

    admin_borrow::locate(where);
    trace::hex_line("selftest: derived cq tail ", where.completion_tail);
    trace::hex_line("selftest: derived cq phase ",
                    where.completion_phase ? 1 : 0);
    trace::hex_line("selftest: derived sq tail ", where.submission_tail);

    auto lap = admin_borrow::length(where.submission_depth,
                                    where.completion_depth);
    trace::hex_line("selftest: lap length ", lap);

    // The storage the queues live in, before anything touches them.
    //
    // Supplied rather than owned, and permanently allocated rather than
    // part of this image: the resident side is handed the same address
    // and points its own queue_pair at it, so there is exactly one
    // submission queue and it is the one the controller is told about.
    // See the comment on queue_pair's pointers for what having two of
    // them cost.
    if (!test_queues::bind_storage(queue_storage)) {
        trace::line("selftest: no queue storage, refusing");
        return;
    }
    channel.queue_storage = queue_storage;
    trace::hex_line("selftest: queue storage at ",
                    reinterpret_cast<std::uint64_t>(queue_storage));

    // Our own queues have to be zeroed before Create I/O Completion
    // Queue: the host owns the initial phase bits, and a stale entry
    // from anything else would read as a completion that never happened.
    for (std::uint32_t i{}; i < 64; ++i) {
        test_queues::completions[i] = completion_entry{};
        test_queues::submissions[i] = submission_entry{};
    }

    submission_entry payload[2]{};
    payload[0] =
        create_io_completion_queue(test_queue_id,
                                   64,
                                   identity_of(test_queues::completions),
                                   false,
                                   0);
    payload[1] =
        create_io_submission_queue(test_queue_id,
                                   64,
                                   identity_of(test_queues::submissions),
                                   test_queue_id,
                                   queue_priority::medium);

    std::uint16_t payload_status[2]{0xffff, 0xffff};

    admin_borrow::snapshot saved{g_scratch_submission,
                                 g_scratch_completion};

    trace::line("selftest: borrowing the firmware's admin queue");

    // Timed, because how long this takes decides an architecture.
    //
    // The borrow needs exclusive use of the admin queue for its whole
    // duration, and the resident version of it will have to exclude a
    // live guest driver for exactly that long. Whether that exclusion
    // can be a doorbell trap that parks one processor, or has to be
    // something heavier, depends entirely on whether this is hundreds of
    // microseconds or milliseconds - and that is a property of the
    // controller, not something to be reasoned about.
    //
    // Measured here rather than resident because it is the same code
    // against the same controller, and here it costs nothing and risks
    // nothing.
    auto borrow_started = arch::x86_64::rdtsc();
    auto result = admin_borrow::run(
        bar, stride, where, saved, payload, 2, payload_status, 1u << 24);
    auto borrow_ticks = arch::x86_64::rdtsc() - borrow_started;

    trace::hex_line("selftest: borrow took tsc ticks ", borrow_ticks);
    trace::hex_line("selftest: lap length was ", lap);

    // Cost against a *deep* queue, which is the number that decides the
    // resident design and cannot be read off the one above.
    //
    // The firmware's admin queue is shallow, so one lap here is four
    // commands; a guest's is 256 deep, so one lap there is 512. Scaling
    // the single measurement by 128 assumes the whole of it is marginal
    // cost, which is exactly the assumption worth not making - a borrow
    // has a fixed part, the snapshot and the restore, that does not grow
    // with the lap.
    //
    // Any whole number of laps is a legal borrow, so timing a long one
    // separates the two: the slope is what a command costs and the
    // intercept is what a borrow costs. Done only when the channel is
    // being brought up, and against the firmware's own queue, so it
    // measures the controller rather than a guest.
    for (std::uint32_t laps : {std::uint32_t{8}, std::uint32_t{64}}) {
        admin_borrow::snapshot again{g_scratch_submission,
                                     g_scratch_completion};
        auto started = arch::x86_64::rdtsc();
        auto measured = admin_borrow::run(bar,
                                          stride,
                                          where,
                                          again,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1u << 24,
                                          laps);
        auto ticks = arch::x86_64::rdtsc() - started;
        if (borrow_result::ok != measured) {
            trace::hex_line("selftest: timing borrow refused at laps ",
                            laps);
            break;
        }
        trace::hex_line("selftest: laps ", laps);
        trace::hex_line("selftest:   commands ", lap * laps);
        trace::hex_line("selftest:   tsc ticks ", ticks);
    }

    switch (result) {
    case borrow_result::ok:
        trace::line("selftest: borrow returned ok");
        break;
    case borrow_result::controller_not_ready:
        trace::line("selftest: borrow refused - controller not ready");
        return;
    case borrow_result::queue_too_deep:
        trace::line("selftest: borrow refused - queue too deep");
        return;
    case borrow_result::too_many_foreign_completions:
        trace::line("selftest: borrow refused - too many foreign");
        return;
    case borrow_result::timed_out:
        trace::line("selftest: borrow TIMED OUT - the admin queue is "
                    "left desynchronised and the controller needs a "
                    "reset");
        return;
    }

    trace::hex_line("selftest: create cq status ", payload_status[0]);
    trace::hex_line("selftest: create sq status ", payload_status[1]);

    // Read the guest's queues back out of memory rather than trusting
    // what the borrow says it put there.
    for (std::uint32_t i{}; i < where.submission_depth; ++i) {
        g_scratch_submission[i] = where.submission[i];
    }
    for (std::uint32_t i{}; i < where.completion_depth; ++i) {
        g_scratch_completion[i] = where.completion[i];
    }
    compare_and_report(where.submission_depth, where.completion_depth);

    if ((0 != payload_status[0]) || (0 != payload_status[1])) {
        trace::line("selftest: queue creation failed, so there is no "
                    "private pair to write through");
        return;
    }
    trace::line("selftest: private queue pair created");

    // Bind the private pair and prove a command of ours completes on our
    // own completion queue, with the guest's never involved.
    test_queues::bound = test_queues::binding{
        .submission_doorbell = bytes + submission_queue_doorbell_offset(
                                           test_queue_id, stride),
        .completion_doorbell = bytes + completion_queue_doorbell_offset(
                                           test_queue_id, stride),
        .status_register = bytes + offset_of(register_offset::status),
        .configuration_register =
            bytes + offset_of(register_offset::configuration),
        .submission_id = test_queue_id,
        .completion_id = test_queue_id,
        .namespace_id = 1,
        .epoch = 1,
    };

    if (!test_queues::controller_still_ours(1)) {
        trace::line("selftest: guard read refused the bound queues");
        return;
    }
    trace::line("selftest: guard read accepts the bound queues");

    // Everything the resident side needs to keep using this queue, in a
    // structure it can read. Device register addresses and identifiers
    // only: nothing here points at memory the firmware reclaims, and
    // nothing points back into this loader.
    //
    // The target comes from the reservation, which is the only thing in
    // a position to say where a write may land - it is what signed the
    // region, and a write to a block carrying no signature of ours is
    // refused. Copied in whole, so the hand-over is self contained and
    // does not point at the reservation's own storage.
    channel.target = destination;
    channel.submission_doorbell = test_queues::bound.submission_doorbell;
    channel.completion_doorbell = test_queues::bound.completion_doorbell;
    channel.status_register = test_queues::bound.status_register;
    channel.configuration_register =
        test_queues::bound.configuration_register;
    channel.submission_id = test_queues::bound.submission_id;
    channel.completion_id = test_queues::bound.completion_id;
    channel.namespace_id = test_queues::bound.namespace_id;

    // Written last, so a partially filled structure never reads as
    // usable.
    // Where the queues stand after everything above, including the
    // proof write below - so this is refreshed once more at the end.
    channel.submission_tail = test_queues::tail_position();
    channel.completion_head = test_queues::head_position();
    channel.completion_phase = test_queues::phase_position() ? 1u : 0u;

    channel.magic = nvme::channel_handover::valid_magic;
    trace::line("selftest: channel handover prepared");

    // The end of the chain: put bytes on the disk through the private
    // queue, using the same submit path the resident side will use.
    //
    // Everything before this proves the queue exists. Only this proves
    // the queue *moves data*, and it proves it the way the channel will
    // actually be read - by something outside the machine searching the
    // raw device for a marker, with no file system involved.
    //
    // Routed through submit() rather than through a hand built command
    // on purpose. submit() writes the block, reads it back and checks
    // the signature it finds against the one it intended, so a pass here
    // exercises the guard as well as the write.
    //
    // Into the reserved region, and nowhere else. This used to write to
    // a hard coded logical block, which was harmless against an emulated
    // disk that nothing else owned and is wrong twice over on a real
    // one. That block belongs to whatever is installed there - on this
    // development machine LBA 120000 is forty two megabytes into the EFI
    // system partition, among boot files. And it could never have
    // succeeded anyway: submit() reads the destination first and refuses
    // unless it finds our magic, our file id and the right block index,
    // none of which anything had ever written there, so the proof could
    // only ever have reported refused_signature.
    //
    // The reserved region is the one place both of those come out right,
    // because the reservation signs every block of it as it establishes
    // it. So the guard passing here is not a formality - it is the
    // reservation and the writer agreeing about the same blocks.
    auto & target = destination;

    if (!target.usable()) {
        // Not a failure of the queue. Everything above this point has
        // already been proved; there is simply nowhere legal to write,
        // so the write is skipped rather than aimed somewhere else.
        trace::line("selftest: no reserved region, proof write skipped");
        return;
    }

    trace::hex_line("selftest: proof write to lba ",
                    target.extents[0].first_lba);

    std::memset(test_queues::staging, 0, nvme::block_size);

    // The signature the guard just insisted on, put back *complete* -
    // every field the reservation stamped, not merely the ones the guard
    // happens to read today.
    //
    // Writing a partial one is a way to corrupt the guard rather than
    // the data. The check reads the block that is already there, so it
    // passes on the reservation's stamp and then this write replaces it;
    // anything left out is gone, and the next write to this block is
    // refused because the block no longer proves it is ours. An earlier
    // version of this omitted both GUIDs and would have done exactly
    // that - silently, since the failure lands one write later.
    //
    // So the rule is: a block this code writes must come out of it
    // carrying everything it carried going in.
    auto header =
        reinterpret_cast<block_signature *>(test_queues::staging);
    header->signature_magic = block_signature::magic;
    header->file_id = target.file_id;
    header->block_index = 0;
    std::memcpy(
        header->disk_guid, target.disk_guid, sizeof(header->disk_guid));
    std::memcpy(header->partition_guid,
                target.partition_guid,
                sizeof(header->partition_guid));

    static constexpr char marker[] = "ZPP_DISK_CHANNEL_PROOF_V1";
    for (std::size_t i{}; i < (sizeof(marker) - 1); ++i) {
        test_queues::staging[sizeof(block_signature) + i] =
            static_cast<std::uint8_t>(marker[i]);
    }

    auto written = test_queues::submit(target, 0, 1, identity_of, 1000000);
    if (write_result::ok != written) {
        trace::hex_line("selftest: proof write did not land, result ",
                        static_cast<std::uint64_t>(written));
        trace::hex_line("selftest: submitted ", test_queues::submitted);
        trace::hex_line("selftest: completed ", test_queues::completed);
        trace::hex_line("selftest: failed ", test_queues::failed);
        trace::hex_line("selftest: refused_guard ",
                        test_queues::refused_guard);
        trace::hex_line("selftest: refused_signature ",
                        test_queues::refused_signature);
        return;
    }

    trace::hex_line("selftest: proof written and verified at lba ",
                    target.extents[0].first_lba);
    // Settle the queues before naming where they stand.
    //
    // Whoever inherits them starts with its own counters at zero, which
    // means "nothing outstanding", so a completion left sitting here
    // becomes one it attributes to its own first command - and then
    // believes a write landed that it never issued. The proof write's
    // completion is exactly that: submit returns without waiting for it.
    if (!test_queues::drain(1000000)) {
        trace::line("selftest: queues would not settle, channel refused");
        channel.magic = 0;
        return;
    }

    // The proof write moved the queues on, so the hand-over's idea of
    // where they stand is now stale by exactly one command. Refreshed
    // here rather than before the write, because this is the position
    // the resident side actually inherits.
    channel.submission_tail = test_queues::tail_position();
    channel.completion_head = test_queues::head_position();
    channel.completion_phase = test_queues::phase_position() ? 1u : 0u;
    trace::hex_line("selftest: handing over submission tail ",
                    channel.submission_tail);
    trace::hex_line("selftest: handing over completion head ",
                    channel.completion_head);
    trace::hex_line("selftest: handing over completion phase ",
                    channel.completion_phase);

    trace::line(
        "selftest: VERDICT the private queue moves data to the disk");

    trace::line("selftest: end");
}

} // namespace zpp

#endif
