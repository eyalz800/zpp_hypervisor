#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <variant>

namespace zpp
{
/**
 * Represents an ELF file.
 */
class elf_file
{
public:
    /**
     * Defines regular enumerations.
     */
    struct enumerations
    {
        enum memory_protection : int
        {
            execute = 1,
            write = 2,
            read = 4,
        };

        enum elf_relocation_type : int
        {
            aarch64_relative = 1027,
            x86_64_relative = 8,
        };
    };

    /**
     * Represents the memory protection of loadable segments.
     */
    using memory_protection = enumerations::memory_protection;

    /**
     * Represents the ELF relocation type.
     */
    using elf_relocation_type = enumerations::elf_relocation_type;

    /**
     * The elf machine according to the ELF spec.
     */
    enum class elf_machine
    {
        x86_64 = 62,
        aarch64 = 183,
    };

    /**
     * The ELF header according to the ELF spec.
     */
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

    /**
     * The ELF program header according to the ELF spec.
     */
    struct elf_phdr
    {
        enum class type
        {
            load = 1,
            dynamic = 2,
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

    /**
     * The ELF relocation type according to the ELF spec.
     */
    struct elf_rel
    {
        std::uintptr_t r_offset;
        std::uintptr_t r_info;
    };

    /**
     * The ELF relocation by addend type according to the ELF spec.
     */
    struct elf_rela
    {
        std::uintptr_t r_offset;
        std::uintptr_t r_info;
        std::uintptr_t r_addend;
    };

    /**
     * The ELF dynamic entry according to the ELF spec.
     */
    struct elf_dyn
    {
        enum class tag
        {
            null = 0,
            rela = 7,
            rela_size = 8,
            rel = 17,
            rel_size = 18,
        };

        std::uintptr_t d_tag;
        union
        {
            std::uintptr_t d_val;
            std::uintptr_t d_ptr;
        };
    };

    /**
     * Constructs an empty ELF file.
     */
    elf_file() = default;

    /**
     * Constructs an ELF file from an unloaded ELF file in memory.
     */
    elf_file(const void * file_data, std::size_t size) :
        // The ELF file data.
        m_file_data(reinterpret_cast<const unsigned char *>(file_data)),

        // The ELF file data size.
        m_file_size(size),

        // The ELF header is at the beginning of the ELF file data.
        m_header(reinterpret_cast<const elf_header *>(file_data)),

        // The ELF program headers.
        m_program_headers(reinterpret_cast<const elf_phdr *>(
            m_file_data + m_header->e_phoff)),

        // Find the dynamic segment according to the program header type.
        m_dynamic(reinterpret_cast<const elf_dyn *>(
            m_file_data +
            std::find_if(m_program_headers,
                         m_program_headers + m_header->e_phnum,
                         [](auto & program_header) {
                             return elf_phdr::type::dynamic ==
                                    elf_phdr::type(program_header.p_type);
                         })
                ->p_offset)),

        // The preferred base is the first load segment virtual address
        // rounded down to page boundary.
        m_preferred_base(
            std::find_if(m_program_headers,
                         m_program_headers + m_header->e_phnum,
                         [](auto & program_header) {
                             return elf_phdr::type::load ==
                                    elf_phdr::type(program_header.p_type);
                         })
                ->p_vaddr &
            ~0xfff),

        // Finding the last load segment program header.
        m_last_load_phdr(
            std::find_if(std::reverse_iterator(m_program_headers +
                                               m_header->e_phnum),
                         std::reverse_iterator(m_program_headers),
                         [](auto & program_header) {
                             return elf_phdr::type::load ==
                                    elf_phdr::type(program_header.p_type);
                         })
                .base() -
            1),

        // Compute the memory size according to last and first load program
        // headers.
        m_memory_size(((m_last_load_phdr->p_vaddr +
                        m_last_load_phdr->p_memsz + 0xfff) &
                       ~0xfff) -
                      m_preferred_base)
    {
        // Parse the dynamic segment.
        for (std::size_t i{};; ++i) {
            auto & dynamic_entry = m_dynamic[i];
            auto tag = elf_dyn::tag(dynamic_entry.d_tag);

            // If the end is reached.
            if (elf_dyn::tag::null == tag) {
                break;
            }

            // Parse dynamic entry.
            switch (tag) {
            case elf_dyn::tag::rel:
                if (std::get_if<const elf_rela *>(&m_relocations)) {
                    break;
                }
                m_relocations = reinterpret_cast<const elf_rel *>(
                    m_file_data + dynamic_entry.d_ptr);
                break;
            case elf_dyn::tag::rel_size:
                if (m_relocations_size) {
                    break;
                }
                m_relocations_size = dynamic_entry.d_val;
                break;
            case elf_dyn::tag::rela:
                m_relocations = reinterpret_cast<const elf_rela *>(
                    m_file_data + dynamic_entry.d_ptr);
                break;
            case elf_dyn::tag::rela_size:
                m_relocations_size = dynamic_entry.d_val;
                break;
            default:
                break;
            }
        }
    }

    template <typename Allocate, typename Protect>
    void * load(Allocate && allocate, Protect && protect)
    {
        // Allocate enough memory.
        auto base = static_cast<unsigned char *>(allocate(m_memory_size));

        // If failed to allocate, return nullptr.
        if (!base) {
            return nullptr;
        }

        // The base difference.
        auto base_difference = base - m_preferred_base;

        // Iterate the program headers and load them.
        for (std::size_t i{}; i < m_header->e_phnum; ++i) {
            auto & program_header = m_program_headers[i];

            // If not loadable, skip.
            if (elf_phdr::type::load !=
                elf_phdr::type(program_header.p_type)) {
                continue;
            }

            // Load the segment.
            std::copy_n(m_file_data + program_header.p_offset,
                        program_header.p_filesz,
                        base_difference + program_header.p_vaddr);

            // Zero memory.
            std::fill_n(base_difference + program_header.p_vaddr +
                            program_header.p_filesz,
                        program_header.p_memsz - program_header.p_filesz,
                        0);
        }

        // Get relocation types.
        std::uint32_t relative_relocation{};
        switch (elf_machine(m_header->e_machine)) {
        case elf_machine::aarch64:
            relative_relocation = elf_relocation_type::aarch64_relative;
            break;
        case elf_machine::x86_64:
            relative_relocation = elf_relocation_type::x86_64_relative;
            break;
        default:
            return nullptr;
        }

        // Returns relocation type.
        auto relocation_type = [](auto value) {
            return value & 0xffffffff;
        };

        // Define how relocations are to be done .
        auto relocate = [&](auto relocations) {
            // The relocation type.
            using relocation_kind = std::remove_pointer_t<
                std::remove_cv_t<decltype(relocations)>>;

            // Iterate rela entries.
            for (std::size_t i{}; i < m_relocations_size; ++i) {
                auto & relocation = relocations[i];

                // If not relative, skip.
                if (relocation_type(relocation.r_info) !=
                    relative_relocation) {
                    continue;
                }

                // Get relocation target.
                auto & target = *reinterpret_cast<std::uintptr_t *>(
                    base_difference + relocation.r_offset);

                // If rela, use addend, else use target.
                if constexpr (std::is_same_v<relocation_kind, elf_rela>) {
                    // Perform the relocation by assigning base
                    // plus addend.
                    target =
                        reinterpret_cast<std::uintptr_t>(base_difference) +
                        relocation.r_addend;
                } else {
                    // Perform the relocation by adding base to
                    // target.
                    target +=
                        reinterpret_cast<std::uintptr_t>(base_difference);
                }
            }
        };

        // Perform the relocations.
        std::visit(relocate, m_relocations);

        // Iterate the program headers and change memory protection.
        for (std::size_t i{}; i < m_header->e_phnum; ++i) {
            auto & program_header = m_program_headers[i];

            // If not loadable, skip.
            if (elf_phdr::type::load !=
                elf_phdr::type(program_header.p_type)) {
                continue;
            }

            // Protect the memory range.
            protect(base_difference + program_header.p_vaddr,
                    program_header.p_memsz,
                    memory_protection(program_header.p_flags));
        }

        // Return the loaded ELF base.
        return base;
    }

    /**
     * Returns the entry relative to file to be mapped in memory.
     */
    std::uintptr_t entry() const
    {
        return m_header->e_entry - m_preferred_base;
    }

    /**
     * Returns the file data.
     */
    const unsigned char * file_data() const
    {
        return m_file_data;
    }

    /**
     * Returns the file size.
     */
    std::size_t file_size() const
    {
        return m_file_size;
    }

    /**
     * Returns the ELF header.
     */
    const elf_header & header() const
    {
        return *m_header;
    }

    /**
     * Returns the ELF program headers.
     */
    const elf_phdr * program_headers() const
    {
        return m_program_headers;
    }

    /**
     * Returns the ELF dynamic segment.
     */
    const elf_dyn * dynamic() const
    {
        return m_dynamic;
    }

    /**
     * Returns the ELF preferred base.
     */
    std::uintptr_t preferred_base() const
    {
        return m_preferred_base;
    }

    /**
     * Returns the size ELF requires in memory.
     */
    std::size_t memory_size() const
    {
        return m_memory_size;
    }

private:
    const unsigned char * m_file_data{};
    std::size_t m_file_size{};
    const elf_header * m_header{};
    const elf_phdr * m_program_headers{};
    const elf_dyn * m_dynamic{};
    std::uintptr_t m_preferred_base{};
    const elf_phdr * m_last_load_phdr{};
    std::size_t m_memory_size{};
    std::variant<const elf_rel *, const elf_rela *> m_relocations{};
    std::size_t m_relocations_size{};
};

} // namespace zpp