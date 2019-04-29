#include "zpp/elf_file.h"
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace zpp
{
extern "C" unsigned char zpp_elf_binary[];
extern "C" std::size_t zpp_elf_binary_size;

extern "C" {
int zpp_cpp_init(std::uintptr_t (*lookup_symbol)(const char *),
                 void * (*allocate_rwx)(std::size_t),
                 std::uintptr_t (*physical_to_virtual)(std::uintptr_t),
                 int (*call_on_cpu)(std::size_t, int (*)(void *), void *),
                 std::size_t (*number_of_cpus)())
{
    static_cast<void>(lookup_symbol);

    // Invoke the elf_loader.
    elf_file elf(zpp_elf_binary, zpp_elf_binary_size);
    auto base = elf.load(
        allocate_rwx,
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
        std::uintptr_t(*physical_to_virtual)(std::uintptr_t))>(
        entry_point_address);

    // Call entry point on all cpus.
    auto cpus = number_of_cpus();

    // If failed, return failure.
    if (!cpus) {
        return -1;
    }

    for (std::size_t i{}; i < cpus; ++i) {
        // The launch function.
        auto launch = [&] { return entry(i, physical_to_virtual); };

        // The erased launch function.
        auto erased_launch = [](void * context) {
            auto & local_launch =
                *static_cast<decltype(launch) *>(context);
            return local_launch();
        };

        // Call on specified cpu.
        auto result =
            call_on_cpu(i,
                        static_cast<int (*)(void *)>(erased_launch),
                        std::addressof(launch));

        // If failed, return failure.
        if (result) {
            return -1;
        }
    }

    // Return an error so the OS unloads our module.
    return -1;
}

void zpp_cpp_exit()
{
}
}
} // namespace zpp