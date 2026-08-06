#include "zpp/nvme_selftest.h"

#include "zpp/trace.h"

#if ZPP_DIAG

#include "zpp/arch/x86_64/mmio.h"
#include "zpp/arch/x86_64/pci.h"
#include "zpp/nvme/admin_borrow.h"
#include "zpp/nvme/command.h"
#include "zpp/nvme/iommu_gate.h"
#include "zpp/nvme/log_writer.h"
#include "zpp/nvme/registers.h"

namespace zpp
{
namespace
{
using namespace zpp::nvme;
using arch::x86_64::pci_config;

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
 * Finds the first NVM Express controller on bus zero.
 */
bool find_controller(pci_config::address & found)
{
    for (std::uint32_t device{}; device < pci_config::max_devices;
         ++device) {
        for (std::uint32_t function{};
             function < pci_config::max_functions;
             ++function) {
            pci_config::address at{0, device, function};
            auto vendor =
                pci_config::read32(at, pci_config::vendor_id_offset);
            if ((0xffffffffu == vendor) || (0 == (vendor & 0xffff))) {
                continue;
            }

            auto classes =
                pci_config::read32(at, pci_config::class_revision_offset);
            if (class_nvme == (classes >> 8)) {
                found = at;
                return true;
            }
        }
    }
    return false;
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
bool validate_bar(const pci_config::address & at, std::uint64_t & base)
{
    auto low = pci_config::read32(at, pci_config::base_address_0_offset);
    auto high =
        pci_config::read32(at, pci_config::base_address_0_offset + 4);

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

    auto command = pci_config::read16(at, pci_config::command_offset);
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

void nvme_selftest::execute()
{
    trace::line("selftest: begin");

    pci_config::address at{};
    if (!find_controller(at)) {
        trace::line("selftest: no nvme controller on bus zero");
        return;
    }
    trace::hex_line("selftest: controller at device ", at.device);
    trace::hex_line("selftest: controller at function ", at.function);

    std::uint64_t base{};
    if (!validate_bar(at, base)) {
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
    auto verdict = iommu_gate::verdict_for(nullptr, 0, 0, 0, nullptr);
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
    auto result = admin_borrow::run(
        bar, stride, where, saved, payload, 2, payload_status, 1u << 24);

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

    trace::line("selftest: end");
}

} // namespace zpp

#endif
