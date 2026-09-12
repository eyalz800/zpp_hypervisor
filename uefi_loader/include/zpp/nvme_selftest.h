#pragma once
#include "zpp/diag/config.h"
#include "zpp/nvme/log_format.h"

/**
 * Proving the admin queue borrow against a real driver, at boot.
 *
 * NVME-LOG.md argues that the guest's admin queue can be borrowed and
 * handed back with the guest unable to tell, and
 * scripts/nvme-lap-model.py asserts the arithmetic. Neither is a
 * measurement against a controller, and the difference matters: the model
 * knows what the specification says a controller does, not what one does.
 *
 * This runs the whole sequence from the loader, while boot services are
 * still alive, and the driver it borrows from is **the firmware's own
 * NvmExpressDxe** - an independent implementation that has already
 * initialised the controller, negotiated whatever it negotiates, created
 * its own I/O queue, and which the loader then goes on to use for the
 * rest of the boot. If the borrow is wrong, the firmware's driver breaks
 * and the boot fails visibly rather than subtly.
 *
 * The loader rather than the resident hypervisor, for three reasons that
 * all point the same way:
 *
 * - The sequence needs no VMX. It is memory mapped register accesses and
 *   ordinary memory, so it does not have to wait for a VM to be running,
 *   and it can therefore be exercised under an emulator whose processor
 *   does not implement VMX at all - which is the situation, since QEMU
 *   models NVMe fully and its TCG models no VT-x.
 * - Boot services mean there is a serial trace to report on, and a file
 *   system to prove the firmware's driver still works afterwards.
 * - Under UEFI the address space is identity mapped, so physical and
 *   virtual are the same and no page table work is needed.
 *
 * Gated on the disk sink being compiled in, which is one word in
 * zpp/diag/config.h. Deliberately not its own build option: config.h says
 * per-sink build switches do not exist, and "the channel is compiled in"
 * and "prove the channel comes up" are the same decision.
 */
namespace zpp
{
struct nvme_selftest
{
    /**
     * Whether this build carries it. The disk sink's own policy row, so
     * enabling the channel is what enables its proof.
     */
    static constexpr bool enabled =
        diag::policy_of(diag::sink::esp_blocks).present;

    /**
     * Runs everything and reports through zpp::trace. Never fails the
     * boot: every step is bounded and a refusal is traced rather than
     * propagated, because a diagnostic that can stop a machine booting is
     * worse than no diagnostic.
     *
     * An inline wrapper around `if constexpr` so a build with the channel
     * off has no call and no code, in the shape zpp::trace and
     * zpp::verify already use.
     */
    /**
     * The channel this run established, for the loader to hand across to
     * the resident side. Unusable until every step below has passed, so
     * a failed or absent self test hands over nothing rather than
     * something half built.
     *
     * It lives here because this is where the queue pair is created and
     * proved. Creating a second one somewhere else, to avoid the word
     * "selftest" appearing on the path that matters, would mean two
     * pieces of code that have to agree about a controller and only one
     * of them tested.
     */
    static inline nvme::channel_handover channel{};

    /**
     * How much permanently allocated, page aligned storage the queue
     * pair needs handed to it.
     *
     * Stated here rather than reached for out of queue_pair, so this
     * header stays light - the caller that allocates the region has no
     * other reason to know what a submission queue is. The two are tied
     * together by a static_assert in nvme_selftest.cpp, so they cannot
     * drift apart silently.
     */
    static constexpr std::size_t queue_storage_bytes = 24576;

    /**
     * Runs against a destination somebody else resolved.
     *
     * Taken as a parameter rather than reached for, because the only
     * place a write can legally land is a region that has already been
     * signed - see the proof write in nvme_selftest.cpp. That makes the
     * reservation a precondition of this rather than a neighbour of it,
     * and a parameter is how a precondition is spelled.
     *
     * A destination that is not usable is not an error. It means the
     * reservation refused, and the run goes ahead and proves everything
     * up to the write, then stops short of it and hands over nothing.
     */
    static void run(const nvme::log_target & destination,
                    void * queue_storage)
    {
        if constexpr (enabled) {
            execute(destination, queue_storage);
        } else {
            static_cast<void>(destination);
            static_cast<void>(queue_storage);
        }
    }

private:
    /**
     * The implementation, out of line so that the header costs nothing.
     * Defined only when enabled - see nvme_selftest.cpp.
     */
    static void execute(const nvme::log_target & destination,
                        void * queue_storage);
};

} // namespace zpp
