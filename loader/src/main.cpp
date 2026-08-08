#include "zpp/loader.h"

#include "zpp/elf_file.h"
#include <cstdint>
#include <utility>

namespace zpp
{
// The hypervisor ELF, embedded by elf_binary.cpp. It lives on this side of
// the ABI rather than in each platform loader because linux_loader is
// built by kbuild with gcc, which cannot do #embed.
extern const unsigned char elf_binary[];
extern const std::size_t elf_binary_size;
} // namespace zpp

extern "C" int
zpp_load_elf(const struct zpp_loader_parameters * parameters)
{
    using namespace zpp;

    // A caller that supplies neither a way to allocate nor a CPU count
    // cannot be served.
    if (!parameters || !parameters->allocate_rwx ||
        !parameters->call_on_cpu || !parameters->number_of_cpus) {
        return -1;
    }

    // The protect callback does nothing on purpose: every loader here
    // allocates one readable, writable and executable region, so there
    // are no per-segment permissions to apply afterwards.
    elf_file elf(elf_binary, elf_file::state::unloaded);
    auto base = elf.load(
        parameters->allocate_rwx,
        [](const void *, std::size_t, elf_file::memory_protection) {});
    if (!base) {
        return -1;
    }

    // The ELF is position independent, so its entry is an offset from
    // wherever it was just loaded rather than an address.
    auto entry_point_address =
        reinterpret_cast<std::uintptr_t>(base) + elf.entry();

    auto entry = reinterpret_cast<int (*)(
        std::size_t cpuid, const zpp_launch_parameters * launch)>(
        entry_point_address);

    auto cpus = parameters->number_of_cpus();

    // Zero would run the loop below no times and still report success,
    // which is a hypervisor that was never launched being reported as one
    // that was. Treated as the platform failing to answer.
    if (!cpus) {
        return -1;
    }

    // Memory below one megabyte, for the hypervisor to start processors
    // the loader is not launching it on. Reserved here, once, rather than
    // per processor: there is only ever one processor being started at a
    // time, so one reservation is reused for all of them.
    //
    // A platform that cannot supply it, or a reservation that fails, is
    // not an error - it means the hypervisor cannot start a processor
    // itself, which only matters on platforms that need it to.
    void * start_up_memory{};
    if (parameters->allocate_below_one_megabyte) {
        start_up_memory = parameters->allocate_below_one_megabyte(
            ZPP_START_UP_MEMORY_SIZE);
    }

    // Static rather than automatic, because the hypervisor goes resident
    // and this frame does not - a pointer into it would dangle the
    // moment the loader returned. Filled once and shared by every
    // processor, since none of it varies between them.
    static zpp_launch_parameters handover{};
    handover.physical_to_virtual = parameters->physical_to_virtual;
    handover.start_up_memory = start_up_memory;
    handover.module_base = base;
    handover.diagnostic_channel = parameters->diagnostic_channel;
    handover.sleep_control_port = parameters->sleep_control_port;
    handover.sleep_control_port_secondary =
        parameters->sleep_control_port_secondary;
    handover.sleep_control_width = parameters->sleep_control_width;
    handover.sleep_facs_physical = parameters->sleep_facs_physical;

    for (std::size_t i{}; i < cpus; ++i) {
        // Rebuilt per processor because it captures i, which is the one
        // thing the hypervisor is told that differs between them.
        auto launch = [&] {
            if (parameters->adjust_launch_calling_convention) {
                return parameters->adjust_launch_calling_convention(
                    entry, i, &handover);
            }
            return entry(i, &handover);
        };

        // A capturing lambda has no function pointer conversion, so the
        // capture travels as call_on_cpu's void context and is recovered
        // here. This is what keeps the callback boundary C compatible,
        // which linux_loader's C caller requires.
        auto erased_launch = [](void * context) {
            auto & local_launch =
                *static_cast<decltype(launch) *>(context);
            return local_launch();
        };

        auto result = parameters->call_on_cpu(
            i,
            static_cast<int (*)(void *)>(erased_launch),
            std::addressof(launch));

        // If failed, hand the hypervisor's own error code back rather than
        // flattening it to -1. Its codes are positive, so they stay
        // distinguishable from the -1 this function returns for failures
        // of its own, and the caller gets to say which step went wrong.
        if (result) {
            return result;
        }
    }

    return 0;
}
