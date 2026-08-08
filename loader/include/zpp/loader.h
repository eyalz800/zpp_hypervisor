#pragma once

// Shared between the platform loaders and loader/src/main.cpp. Must stay
// valid C: linux_loader/src/main.c is compiled by kbuild with gcc, not by
// this project's clang.

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
extern "C" {
#elif defined(__KERNEL__)
// Kernel modules build with -nostdinc, so the C library headers do not
// exist. linux/types.h is where size_t and uintptr_t come from there.
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/**
 * How much memory below one megabyte to reserve for the hypervisor to
 * start a processor with.
 *
 * A macro rather than a constant because this header has to stay valid C.
 * The hypervisor's own requirement is stated by ap_start_up_pages in
 * zpp/arch/x86_64/ap_start_up.h, which the two builds do not share a view
 * of - so this has to be at least that, and the hypervisor checks what it
 * was given rather than assuming.
 */
#define ZPP_START_UP_MEMORY_SIZE (4 * 4096)

/**
 * The platform services zpp_load_elf needs from its caller.
 *
 * Grouped into a struct rather than passed positionally so that call sites
 * can name each one with a designated initializer.
 */
/**
 * Everything the hypervisor is handed at launch, other than which
 * processor it is running on.
 *
 * A structure rather than a widening argument list, because the argument
 * list had already been widened twice and every widening costs the same
 * four places: this header, the loader's call, and a naked calling
 * convention adapter in each of two loaders that has to be re-derived by
 * hand. Adding a field here costs none of them.
 *
 * Shared by every processor and read only, so one instance serves the
 * whole launch. It must outlive the launch on every platform - the
 * loaders keep it in static storage rather than on the stack, since the
 * hypervisor goes resident and the caller's frame does not.
 */
struct zpp_launch_parameters
{
    /**
     * Translates a physical address within the OS page tables to a
     * virtual one. Only called during initialization. May be null on
     * platforms that identity map, such as UEFI.
     */
    uintptr_t (*physical_to_virtual)(uintptr_t address);

    /**
     * Page aligned memory below one megabyte for starting processors, or
     * null where the platform could not supply it.
     */
    void * start_up_memory;

    /**
     * Where the loader placed the hypervisor image, or null if it cannot
     * say.
     *
     * Handed over rather than searched for. The resident side can find
     * its own base by scanning backwards for the ELF magic, and that
     * scan is only correct while no page between the true base and the
     * key it starts from begins with those four bytes - which is not a
     * property anything guarantees. Measured failing on one machine and
     * succeeding on another with the same build, landing about a
     * megabyte inside the image. The loader chose the address and has it
     * exactly, so it says so.
     */
    const void * module_base;

    /**
     * The diagnostic channel the loader established, or null.
     *
     * A `void *` because this header is included from C while the
     * structure behind it is C++ - the resident side casts it to
     * `zpp::nvme::channel_handover` and checks its magic before
     * believing any field of it. Null means the platform established
     * none, which the hypervisor treats as the channel being
     * unavailable rather than as an error.
     */
    const void * diagnostic_channel;

    /**
     * The ACPI sleep control register, as I/O port numbers, or zero when
     * the loader could not find it. The second is zero on the usual
     * platform, which uses one block.
     *
     * Only the loader can find these - they come out of the fixed ACPI
     * description table, and the hypervisor has no table walker. Without
     * them a suspend is invisible to it.
     * @{
     */
    uint16_t sleep_control_port;
    uint16_t sleep_control_port_secondary;
    /**
     * @}
     */

    /**
     * How wide that register is, in bytes, from the same table. Two on the
     * usual platform, and the specification allows four.
     *
     * Needed because a VMM that performs the guest's write itself has to
     * perform it at the register's own width. Writing four bytes to a two
     * byte register is not a wider version of the same access - it is a
     * write to two registers, the second of which is whatever the platform
     * put next to this one. Zero when the loader found no register.
     */
    uint8_t sleep_control_width;

    /**
     * The physical address of the firmware ACPI control structure, or zero
     * when the loader found none or found one that did not check out.
     *
     * That table holds the firmware waking vector, which is where the
     * platform jumps on an S3 resume and therefore the only place a VMM
     * with no component inside the guest can insert itself into one.
     */
    uint64_t sleep_facs_physical;

    /**
     * The local APIC id of every processor the platform reports, and how
     * many there are. Null and zero where the loader could not say.
     *
     * Wanted for one case that cannot be answered any other way: a guest
     * that starts its processors with a **broadcast** start-up IPI. The
     * command names no destination at all - it says "all excluding self"
     * - so a VMM that learns a processor exists by being told to start
     * it learns nothing from it, and the processors come up outside the
     * VMM entirely. With a guest hypervisor above, that is worse than
     * refusing: those processors then belong to nobody, its rendezvous
     * never completes, and it resets the machine.
     *
     * Every full hypervisor resolves the same shorthand against a roster
     * it already holds - KVM's kvm_apic_match_dest against kvm->vcpus,
     * for one - because it created every processor before the guest
     * asked for one. This VMM launches on the boot processor alone, so
     * the roster has to be handed to it.
     *
     * From the loader because only the loader has a platform to ask.
     * Under UEFI that is EFI_MP_SERVICES_PROTOCOL; a loader that runs
     * under an operating system already launches the hypervisor on every
     * processor, so it has no broadcast to resolve and may leave this
     * null.
     *
     * The array need not outlive the call: the hypervisor copies it.
     * @{
     */
    const uint32_t * processor_apic_ids;
    size_t number_of_processor_apic_ids;
    /**
     * @}
     */
};

struct zpp_loader_parameters
{
    /**
     * Allocates size bytes of readable, writable and executable memory.
     * Returns null on failure.
     */
    void * (*allocate_rwx)(size_t size);

    /**
     * Translates a physical address within the OS page tables to a virtual
     * one. Only called during the hypervisor's initialization phase. May
     * be null on platforms that identity map, such as UEFI.
     */
    uintptr_t (*physical_to_virtual)(uintptr_t address);

    /**
     * Runs function on the given CPU with the given context and waits for
     * it to finish. Returns zero on success.
     */
    int (*call_on_cpu)(size_t cpu,
                       int (*function)(void *),
                       void * context);

    /**
     * Returns the number of CPUs to launch the hypervisor on.
     */
    size_t (*number_of_cpus)(void);

    /**
     * Allocates size bytes of page aligned memory strictly below one
     * megabyte, and returns null on failure.
     *
     * That is not an arbitrary bound. A processor the hypervisor starts
     * itself is started with a start-up IPI, whose vector is the page
     * number of the entry point - eight bits of page number, so nothing
     * above one megabyte can be named at all.
     *
     * May itself be null, on platforms where the loader launches the
     * hypervisor on every processor and it therefore never has to start
     * one. The hypervisor treats that as the feature being unavailable
     * rather than as an error.
     */
    void * (*allocate_below_one_megabyte)(size_t size);

    /**
     * The diagnostic channel this loader established, or null. Copied
     * into the launch parameters rather than passed separately.
     */
    const void * diagnostic_channel;

    /**
     * The ACPI sleep control register, as found by this loader, or zero.
     * @{
     */
    uint16_t sleep_control_port;
    uint16_t sleep_control_port_secondary;
    uint8_t sleep_control_width;
    uint64_t sleep_facs_physical;
    /**
     * @}
     */

    /**
     * Every processor's local APIC id, as this loader's platform reports
     * them, or null. See zpp_launch_parameters for what needs it.
     * @{
     */
    const uint32_t * processor_apic_ids;
    size_t number_of_processor_apic_ids;
    /**
     * @}
     */

    /**
     * Adjusts the calling convention before entering the hypervisor. May
     * be null when the platform's convention already matches.
     */
    int (*adjust_launch_calling_convention)(
        int (*entry)(size_t cpu,
                     const struct zpp_launch_parameters * launch),
        size_t cpu,
        const struct zpp_launch_parameters * launch);
};

/**
 * Loads the embedded hypervisor ELF and launches it on every CPU.
 * Returns zero on success.
 */
int zpp_load_elf(const struct zpp_loader_parameters * parameters);

#ifdef __cplusplus
}
#endif
