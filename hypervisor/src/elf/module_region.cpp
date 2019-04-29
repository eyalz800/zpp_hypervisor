#include "zpp/elf/module_region.h"
#include <cstdint>
#include <cstring>

namespace zpp::elf
{
namespace
{
struct elf_header
{
    unsigned char e_ident[16];
    std::uint16_t e_type;
    std::uint16_t e_machine;
    std::uint32_t e_version;
    std::uintptr_t e_entry;
    std::size_t e_phoff;
    std::size_t e_shoff;
    std::uint32_t e_flags;
    std::uint16_t e_ehsize;
    std::uint16_t e_phentsize;
    std::uint16_t e_phnum;
    std::uint16_t e_shentsize;
    std::uint16_t e_shnum;
    std::uint16_t e_shstrndx;
};

struct elf_phdr
{
    enum class type
    {
        load = 1
    };
    std::uint32_t p_type;
    std::uint32_t p_flags;
    std::size_t p_offset;
    std::uintptr_t p_vaddr;
    std::uintptr_t p_paddr;
    std::size_t p_filesz;
    std::size_t p_memsz;
    std::size_t p_align;
};

alignas(0x1000) static const struct unaligned_elf_magic
{
    char pad{};
    char magic[4] = {0x7f, 'E', 'L', 'F'};
} g_elf_magic;
} // namespace

std::tuple<module_base, module_size> get_module_region()
{
    auto elf_base = static_cast<const char *>(
        reinterpret_cast<void *>(&get_module_region));

    elf_base = reinterpret_cast<const char *>(
        reinterpret_cast<std::uintptr_t>(elf_base) & 0xfffffffffffff000);

    while (std::memcmp(
        elf_base, g_elf_magic.magic, sizeof(g_elf_magic.magic))) {
        elf_base -= 0x1000;
    }

    auto header = reinterpret_cast<const elf_header *>(elf_base);
    auto program_header =
        reinterpret_cast<const elf_phdr *>(elf_base + header->e_phoff);
    auto end_address = elf_base;
    for (std::size_t i{}; i < header->e_phnum; ++i) {
        auto & phdr = program_header[i];
        if (elf_phdr::type::load != elf_phdr::type(phdr.p_type)) {
            continue;
        }

        if (end_address < elf_base + phdr.p_vaddr + phdr.p_memsz) {
            end_address = elf_base + phdr.p_vaddr + phdr.p_memsz;
        }
    }

    return {module_base(reinterpret_cast<std::uintptr_t>(elf_base)),
            module_size(end_address - elf_base)};
}

} // namespace zpp::elf