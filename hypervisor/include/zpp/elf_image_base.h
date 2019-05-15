#pragma once
#include <cstdint>

namespace zpp
{
/**
 * Returns the current ELF image base.
 */
inline const unsigned char * elf_image_base()
{
    static constexpr auto page_size = 0x1000;

    alignas(page_size) static const struct unaligned_elf_magic
    {
        unsigned char pad{};
        unsigned char magic[4] = {0x7f, 'E', 'L', 'F'};
    } g_elf_magic;

    auto elf_base = reinterpret_cast<const unsigned char *>(
        reinterpret_cast<std::uintptr_t>(&g_elf_magic) & ~0xffful);

    while (std::memcmp(
        elf_base, g_elf_magic.magic, sizeof(g_elf_magic.magic))) {
        elf_base -= page_size;
    }

    return elf_base;
}

} // namespace zpp
