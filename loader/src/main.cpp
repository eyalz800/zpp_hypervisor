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

    // Invoke the elf_loader.
    elf_file elf(elf_binary, elf_file::state::unloaded);
    auto base = elf.load(
        parameters->allocate_rwx,
        [](const void *, std::size_t, elf_file::memory_protection) {});
    if (!base) {
        return -1;
    }

    // The entry point address.
    auto entry_point_address =
        reinterpret_cast<std::uintptr_t>(base) + elf.entry();

    // Convert ELF entry to function pointer.
    auto entry = reinterpret_cast<int (*)(
        std::size_t cpuid,
        std::uintptr_t (*physical_to_virtual)(std::uintptr_t),
        void * start_up_memory,
        const struct zpp_framebuffer * framebuffer)>(entry_point_address);

    // Call entry point on all cpus.
    auto cpus = parameters->number_of_cpus();

    // If failed, return failure.
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

    for (std::size_t i{}; i < cpus; ++i) {
        // The launch function.
        auto launch = [&] {
            if (parameters->adjust_launch_calling_convention) {
                return parameters->adjust_launch_calling_convention(
                    entry,
                    i,
                    parameters->physical_to_virtual,
                    start_up_memory,
                    &parameters->framebuffer);
            }
            return entry(i,
                         parameters->physical_to_virtual,
                         start_up_memory,
                         &parameters->framebuffer);
        };

        // The erased launch function.
        auto erased_launch = [](void * context) {
            auto & local_launch =
                *static_cast<decltype(launch) *>(context);
            return local_launch();
        };

        // Call on specified CPU.
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

    // Return success.
    return 0;
}
