#include <linux/cpumask.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/types.h>

typedef long (*sched_getaffinity_t)(pid_t pid, struct cpumask * mask);
typedef long (*sched_setaffinity_t)(pid_t pid,
                                    const struct cpumask * new_mask);

static struct driver_state
{
    cpumask_t previous_mask;
    sched_getaffinity_t sched_getaffinity;
    sched_setaffinity_t sched_setaffinity;
} g_state;

int zpp_cpp_init(uintptr_t (*kallsyms_lookup_name)(const char *),
                 void * (*allocate_rwx)(size_t),
                 void * (*physical_to_virtual)(unsigned long long),
                 int (*call_on_cpu)(size_t, int (*)(void *), void *),
                 size_t (*number_of_cpus)(void));

void zpp_cpp_exit(void);

static size_t number_of_cpus(void)
{
    int result = num_online_cpus();
    if (result < 0) {
        return 0;
    }

    return result;
}

static int call_on_cpu(size_t cpuid,
                       int (*function)(void *),
                       void * context)
{
    int result = -1;

    // Save previous affinity.
    if (g_state.sched_getaffinity(current->pid, &g_state.previous_mask)) {
        return -1;
    }

    // Set new affinity to only given cpuid.
    if (g_state.sched_setaffinity(current->pid, cpumask_of(cpuid))) {
        return -1;
    }

    // Call user function.
    result = function(context);

    // Restore previous affinity.
    if (g_state.sched_setaffinity(current->pid, &g_state.previous_mask)) {
        return -1;
    }
    return result;
}

static void * allocate_rwx(size_t size)
{
    return __vmalloc(size, GFP_KERNEL, PAGE_KERNEL_EXEC);
}

static int zpp_init(void)
{
    g_state.sched_getaffinity =
        (sched_getaffinity_t)kallsyms_lookup_name("sched_getaffinity");
    g_state.sched_setaffinity =
        (sched_setaffinity_t)kallsyms_lookup_name("sched_setaffinity");

    return zpp_cpp_init(&kallsyms_lookup_name,
                        &allocate_rwx,
                        &phys_to_virt,
                        &call_on_cpu,
                        &number_of_cpus);
}

static void zpp_exit(void)
{
    zpp_cpp_exit();
}

MODULE_LICENSE("GPL");
module_init(zpp_init);
module_exit(zpp_exit);