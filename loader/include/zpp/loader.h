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
 * Where the firmware's own graphics output is, and how it is laid out.
 *
 * Only the loader can answer this. The framebuffer's address comes out of
 * EFI_GRAPHICS_OUTPUT_PROTOCOL, which is a boot services protocol - it
 * stops existing at ExitBootServices, long before the resident side has
 * anything to ask it, and the resident side has no protocol database to
 * ask anyway.
 *
 * Why it is worth handing over at all: on a rig whose display adapter is
 * passed through, the emulator has no console of its own and answers
 * `screendump` with "There is no console to take a screendump from". The
 * pixels are still there - they are in the *guest's* physical address
 * space, inside the adapter's framebuffer bar, which the monitor's `xp`
 * reads. What is missing is only the address and the layout, and this is
 * that. See scripts/rig-screen.py, which is the reader.
 *
 * This describes the **firmware's** linear framebuffer, which is what the
 * boot graphics - the vendor logo, the spinner, and a bugcheck screen
 * raised before the display driver loads - are drawn into. Once the
 * operating system's own display driver takes over it may program the
 * adapter differently and this stops describing what is on screen. That
 * is a real limit and it is the *right* one for the window that matters:
 * a guest that hangs in Phase1Initialization never gets that far.
 *
 * By value inside `zpp_launch_parameters` rather than behind a pointer,
 * deliberately. The resident side copies the launch block whole and then
 * stops being able to follow any pointer out of it - it switches to the
 * host page table, which maps this module and very little else - so a
 * pointer here would need its own second copy, exactly like
 * `diagnostic_channel` does. Ten scalars are cheaper than that.
 *
 * All zero means the loader found no graphics output protocol, which is
 * not an error: it must never stop a boot.
 */
struct zpp_framebuffer_info
{
    /**
     * Physical address of the first pixel, and how many bytes the
     * firmware says the whole framebuffer occupies.
     *
     * A physical address in the address space the loader is running in,
     * which under UEFI is identity mapped - so it is directly what `xp`
     * wants. Zero means there is no framebuffer.
     * @{
     */
    uint64_t base;
    uint64_t size;
    /**
     * @}
     */

    /**
     * The visible extent, in pixels.
     * @{
     */
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    /**
     * @}
     */

    /**
     * How many pixels one scan line occupies, which is **not** the same
     * as the horizontal resolution and is the field a reader gets wrong.
     * Firmware routinely pads a scan line out to a convenient alignment,
     * so an image walked at the visible width shears diagonally.
     */
    uint32_t pixels_per_scan_line;

    /**
     * The pixel format, as EFI_GRAPHICS_PIXEL_FORMAT numbers them:
     * 0 red-green-blue-reserved, 1 blue-green-red-reserved, 2 bit mask,
     * 3 blt only. Every one of the first three is four bytes per pixel.
     *
     * Format 3 has **no** linear framebuffer at all - the firmware only
     * offers the Blt() service - and `base` is meaningless there. A
     * reader must check this rather than assume, which is why the number
     * is carried rather than normalised away.
     */
    uint32_t pixel_format;

    /**
     * The channel masks, meaningful only when `pixel_format` is 2. Zero
     * otherwise, since the other formats define their byte order.
     * @{
     */
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
    /**
     * @}
     */
};

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

    /**
     * The firmware's linear framebuffer, or all zero where the loader
     * found none. See `zpp_framebuffer_info`.
     */
    struct zpp_framebuffer_info framebuffer;
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
     * The firmware's linear framebuffer, as this loader's platform
     * describes it, or all zero. Only the UEFI loader can fill this in -
     * a loader running under an operating system has no graphics output
     * protocol to ask, and by then the display driver owns the adapter
     * anyway.
     */
    struct zpp_framebuffer_info framebuffer;

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
