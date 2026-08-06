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
 * How a pixel is laid out in the framebuffer below.
 *
 * Named here rather than reusing the firmware's enumeration, because the
 * hypervisor is not a UEFI program and has no view of it - and because the
 * two formats this describes are the two that can be written to without
 * further information. The loader translates.
 */
enum zpp_framebuffer_format
{
    /**
     * There is no framebuffer, or nothing is known about it. Nothing may
     * be drawn.
     */
    zpp_framebuffer_format_none = 0,

    /**
     * Four bytes per pixel, byte zero blue, byte one green, byte two red,
     * byte three reserved.
     */
    zpp_framebuffer_format_blue_green_red_reserved = 1,

    /**
     * Four bytes per pixel, byte zero red, byte one green, byte two blue,
     * byte three reserved.
     */
    zpp_framebuffer_format_red_green_blue_reserved = 2,

    /**
     * A format that exists but says nothing about how wide a pixel is or
     * where its channels are - a bit mask, or a mode with no linear
     * framebuffer at all. Reported rather than dropped, so that a machine
     * which draws nothing can be told apart from one that was never asked
     * to.
     */
    zpp_framebuffer_format_unsupported = 3
};

/**
 * The linear framebuffer, as the loader found it.
 *
 * This is the hypervisor's only output channel once a guest is running: it
 * has no console, the serial port belongs to the guest, and the members it
 * records state in need a debugger attached to the very processor that
 * stopped. Handed over so that a processor which stops can say why on the
 * screen.
 *
 * All zero when the platform has no framebuffer to report, which is not an
 * error - it means the hypervisor keeps quiet.
 */
struct zpp_framebuffer
{
    /**
     * The physical address of the first pixel, or zero when there is none.
     */
    uint64_t base;

    /**
     * How many bytes the framebuffer occupies.
     */
    uint64_t size;

    /**
     * The visible width in pixels.
     */
    uint32_t width;

    /**
     * The visible height in pixels.
     */
    uint32_t height;

    /**
     * How many pixels one line of video memory holds, which is not the
     * width: real adapters pad the line, and using the width as the stride
     * skews the image by the difference on every row.
     */
    uint32_t pixels_per_scan_line;

    /**
     * One of the zpp_framebuffer_format values. A plain integer rather
     * than the enumeration, because an enumeration's underlying type is up
     * to the implementation and this structure crosses between two
     * compilers.
     */
    uint32_t format;
};

/**
 * The platform services zpp_load_elf needs from its caller.
 *
 * Grouped into a struct rather than passed positionally so that call sites
 * can name each one with a designated initializer.
 */
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
     * Adjusts the calling convention before entering the hypervisor. May
     * be null when the platform's convention already matches.
     */
    int (*adjust_launch_calling_convention)(
        int (*entry)(size_t cpu,
                     uintptr_t (*physical_to_virtual)(uintptr_t),
                     void * start_up_memory,
                     const struct zpp_framebuffer * framebuffer),
        size_t cpu,
        uintptr_t (*physical_to_virtual)(uintptr_t),
        void * start_up_memory,
        const struct zpp_framebuffer * framebuffer);

    /**
     * The screen the hypervisor may draw a diagnosis on, all zero on a
     * platform that has none.
     *
     * Read only while zpp_load_elf is running: the hypervisor copies what
     * it needs out of it during its launch, in the same window
     * physical_to_virtual above is callable in.
     *
     * Last, and by value, so that extending this description again is an
     * addition here and nothing more - a platform loader that says nothing
     * about a framebuffer still compiles and still reports none.
     */
    struct zpp_framebuffer framebuffer;
};

/**
 * Loads the embedded hypervisor ELF and launches it on every CPU.
 * Returns zero on success.
 */
int zpp_load_elf(const struct zpp_loader_parameters * parameters);

#ifdef __cplusplus
}
#endif
