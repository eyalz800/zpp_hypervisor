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
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

// Threaded through work_on_cpu, which passes a single void argument.
struct call_on_cpu_arguments
{
    int (*function)(void *);
    void * context;
};

static size_t number_of_cpus(void)
{
    int result = num_online_cpus();
    if (result < 0) {
        return 0;
    }

    return result;
}

static long call_on_cpu_trampoline(void * argument)
{
    struct call_on_cpu_arguments * arguments = argument;
    return arguments->function(arguments->context);
}

static int call_on_cpu(size_t cpuid,
                       int (*function)(void *),
                       void * context)
{
    struct call_on_cpu_arguments arguments = {
        .function = function,
        .context = context,
    };

    // work_on_cpu runs this in a worker bound to the target CPU and waits
    // for it, so the hypervisor is genuinely entered on that CPU and
    // cannot be migrated off it. The previous approach called
    // sched_setaffinity, which only expressed a preference - the calling
    // task carried on running on whichever CPU it was already on until it
    // next scheduled - and relied on kallsyms_lookup_name, unexported
    // since 5.7.
    return (int)work_on_cpu(
        (int)cpuid, call_on_cpu_trampoline, &arguments);
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

    // Load the ELF.
    const struct zpp_loader_parameters parameters = {
        .allocate_rwx = &allocate_rwx,
        // phys_to_virt returns void *, which is pointer sized here.
        .physical_to_virtual = (uintptr_t (*)(uintptr_t))&phys_to_virt,
        .call_on_cpu = &call_on_cpu,
        .number_of_cpus = &number_of_cpus,
        // The kernel already uses the SysV convention the hypervisor
        // wants.
        .adjust_launch_calling_convention = NULL,
    };

    result = zpp_load_elf(&parameters);

    // If we failed, return an arbitrary failure.
    if (result) {
        return -EFAULT;
    }

    // Return an error so the driver gets unloaded.
    return -EPERM;
}

static void zpp_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(zpp_init);
module_exit(zpp_exit);
