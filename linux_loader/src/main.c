#include "zpp/loader.h"

#include <asm/io.h>
#include <asm/pgtable.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

// Threaded through smp_call_function_single, which passes a single void
// argument and cannot return a value of its own.
struct call_on_cpu_arguments
{
    int (*function)(void *);
    void * context;
    int result;
};

static size_t number_of_cpus(void)
{
    int result = num_online_cpus();
    if (result < 0) {
        return 0;
    }

    return result;
}

static void call_on_cpu_trampoline(void * argument)
{
    struct call_on_cpu_arguments * arguments = argument;
    arguments->result = arguments->function(arguments->context);
}

static int call_on_cpu(size_t cpuid,
                       int (*function)(void *),
                       void * context)
{
    struct call_on_cpu_arguments arguments = {
        .function = function,
        .context = context,
        .result = -1,
    };

    // A plain EXPORT_SYMBOL, unlike work_on_cpu and set_cpus_allowed_ptr,
    // so it is usable from this MIT licensed module. Sends an IPI and,
    // with wait set, blocks until the target CPU has finished, which is
    // what launch_on_cpu requires of its caller.
    //
    // The function therefore runs in interrupt context with interrupts
    // already disabled. It must not sleep or allocate, and note that the
    // hypervisor's failure path currently enables interrupts
    // unconditionally rather than restoring them, which is wrong in this
    // context.
    if (smp_call_function_single(
            (int)cpuid, call_on_cpu_trampoline, &arguments, 1)) {
        return -1;
    }

    return arguments.result;
}

static void * allocate_rwx(size_t size)
{
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    struct page ** pages;
    void * address;
    size_t index;

    // __vmalloc lost its pgprot parameter in 5.8, and vmalloc_exec was
    // removed with it, so executable memory can no longer be asked for
    // directly.
    // __vmalloc_node_range and module_alloc can do it but are not exported
    // to modules. vmap is exported and still takes a protection, so
    // allocate the pages and map them executable explicitly.
    pages = kmalloc_array(page_count, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        return NULL;
    }

    for (index = 0; index < page_count; ++index) {
        pages[index] = alloc_page(GFP_KERNEL);
        if (!pages[index]) {
            while (index--) {
                __free_page(pages[index]);
            }
            kfree(pages);
            return NULL;
        }
    }

    address = vmap(pages, page_count, VM_MAP, PAGE_KERNEL_EXEC);
    if (!address) {
        for (index = 0; index < page_count; ++index) {
            __free_page(pages[index]);
        }
        kfree(pages);
        return NULL;
    }

    // vmap does not retain the array itself, only the mappings, so the
    // temporary array goes now. The pages are deliberately never released:
    // this module reports failure so that it gets unloaded, while the
    // hypervisor it just launched stays resident in this allocation.
    kfree(pages);

    return address;
}

static int zpp_init(void)
{
    int result = 0;

    // Everything the platform has to supply. Designated initializers
    // throughout, so a field added to the structure shows up here as a
    // name rather than as a shifted position - which matters more here
    // than elsewhere, since this file is compiled by a different
    // compiler from the header's other consumers.
    const struct zpp_loader_parameters parameters = {
        .allocate_rwx = &allocate_rwx,
        // phys_to_virt returns void *, which is pointer sized here.
        .physical_to_virtual = (uintptr_t (*)(uintptr_t))&phys_to_virt,
        .call_on_cpu = &call_on_cpu,
        .number_of_cpus = &number_of_cpus,
        // Not supplied, because nothing here needs it: the hypervisor is
        // launched on every processor from this loader, all of them
        // already running under the kernel, so it never has to start one
        // itself.
        .allocate_below_one_megabyte = NULL,
        /* No channel here: this loader has no boot services to resolve a
         * file with, and the disk is already owned by a running kernel. */
        .diagnostic_channel = NULL,
        .sleep_control_port = 0,
        .sleep_control_port_secondary = 0,
        .sleep_control_width = 0,
        .sleep_facs_physical = 0,
        /* Nor this, for the same reason: every processor is already
         * running the hypervisor by the time a guest broadcasts a
         * start-up IPI, so there is nothing for a roster to answer. */
        .processor_apic_ids = NULL,
        .number_of_processor_apic_ids = 0,
        /* System V already, so no adaptation is needed. */
        .adjust_launch_calling_convention = NULL,
    };

    result = zpp_load_elf(&parameters);

    // Flattened, because module_init's return value has to be a negative
    // errno and the hypervisor's own error codes are not. The UEFI loader
    // is the one that prints them unflattened.
    if (result) {
        return -EFAULT;
    }

    // An error on success, deliberately: the hypervisor is resident in
    // the vmap allocation that allocate_rwx deliberately never frees, so
    // failing init is how this module gets unloaded again without taking
    // the hypervisor with it. A distinct errno from the failure above so
    // the outcome is still readable from dmesg.
    return -EPERM;
}

static void zpp_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(zpp_init);
module_exit(zpp_exit);
