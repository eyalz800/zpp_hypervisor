#include <cstddef>

// Embed the hypervisor ELF binary using C++26 #embed.
// ZPP_ELF_BINARY_PATH is defined by CMake to point to the built hypervisor.
//
// Declared extern "C" with explicit external linkage so that loader/src/main.cpp
// (which declares them as extern "C" unsigned char[] / size_t) can link against
// these definitions.  Without explicit `extern`, `const` variables at namespace
// scope have internal linkage in C++.

extern "C" alignas(8) const unsigned char zpp_elf_binary[] = {
    #embed ZPP_ELF_BINARY_PATH
};

extern "C" const std::size_t zpp_elf_binary_size = sizeof(zpp_elf_binary);
