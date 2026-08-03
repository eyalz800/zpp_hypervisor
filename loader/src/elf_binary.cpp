#include <cstddef>

namespace zpp
{
alignas(8) extern const unsigned char elf_binary[] = {
    #embed ZPP_ELF_BINARY_PATH
};

extern const std::size_t elf_binary_size = sizeof(elf_binary);
} // namespace zpp
