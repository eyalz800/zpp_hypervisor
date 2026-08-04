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
     * The crash log region the platform has already reserved, at the one
     * fixed physical address the next boot will look for it at, or null
     * where the platform refused that address or does not do this at all.
     *
     * Data rather than a service, unlike the reservation above, because
     * the ordering is not this function's to choose. The region has to be
     * claimed at the very start of the platform loader: the previous
     * boot's log is read out of it and written to disk there, before
     * anything else has had a chance to fail, so that a boot which never
     * reaches zpp_load_elf still reports what the last one left behind. By
     * the time this struct is filled in, it is done.
     *
     * The address is not carried here either - it is a constant both ends
     * already agree on, zpp::crash_log::region_address. What crosses this
     * boundary is only whether this boot's loader succeeded in claiming
     * it, which is a question nothing else can answer: the region survives
     * restarts by design, so anything found inside it may have been left
     * by a boot whose memory map was not this one's.
     *
     * Null on the platforms where an operating system is already running
     * and has its own means of keeping a log across a restart.
     */
    void * crash_log_memory;

    /**
     * Adjusts the calling convention before entering the hypervisor. May
     * be null when the platform's convention already matches.
     */
    int (*adjust_launch_calling_convention)(
        int (*entry)(size_t cpu,
                     uintptr_t (*physical_to_virtual)(uintptr_t),
                     void * start_up_memory,
                     void * crash_log_memory),
        size_t cpu,
        uintptr_t (*physical_to_virtual)(uintptr_t),
        void * start_up_memory,
        void * crash_log_memory);
};

/**
 * Loads the embedded hypervisor ELF and launches it on every CPU.
 * Returns zero on success.
 */
int zpp_load_elf(const struct zpp_loader_parameters * parameters);

#ifdef __cplusplus
}
#endif
