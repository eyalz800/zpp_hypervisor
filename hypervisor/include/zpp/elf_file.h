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
     * Specifies whether the given ELF file is loaded or unloaded.
     */
    enum class state
    {
        unloaded,
        loaded,
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
     * Constructs an ELF file from an ELF file in memory.
     */
    elf_file(const void * file_data, state elf_state) :
        // The ELF file data.
        m_file_data(reinterpret_cast<const unsigned char *>(file_data)),

        // The ELF header is at the beginning of the ELF file data.
        m_header(reinterpret_cast<const elf_header *>(file_data)),

        // The ELF program headers.
        m_program_headers(reinterpret_cast<const elf_phdr *>(
            m_file_data + m_header->e_phoff)),

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

        // Find the dynamic program header according to the program header
        // type.
        m_dynamic_phdr(std::find_if(m_program_headers,
                                    m_program_headers + m_header->e_phnum,
                                    [](auto & program_header) {
                                        return elf_phdr::type::dynamic ==
                                               elf_phdr::type(
                                                   program_header.p_type);
                                    })),

        // Compute the dynamic segment address.
        m_dynamic(reinterpret_cast<const elf_dyn *>(
            (state::unloaded == elf_state)
                ? m_file_data + m_dynamic_phdr->p_offset
                : m_dynamic_phdr->p_vaddr +
                      (m_file_data - m_preferred_base))),

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
    }

    /**
     * Loads the ELF file into memory, using an allocation strategy
     * and a protection strategy to adjust the page protection accordingly.
     * The behavior is undefined if the ELF file is already loaded.
     * The function will load the ELF segments into memory and
     * perform relative relocations on it.
     * Returns the base address of the loaded ELF.
     */
    template <typename Allocate, typename Protect>
    void * load(Allocate && allocate, Protect && protect)
    {
        // Allocate enough memory.
        auto base = static_cast<unsigned char *>(allocate(m_memory_size));

        // If failed to allocate, return nullptr.
        if (!base) {
            return nullptr;
        }

        // The base difference, which is the new base minus the preferred
        // base, to be used when converting ELF file addresses to point at
        // the new relocated ELF.
        auto base_difference =
            reinterpret_cast<std::ptrdiff_t>(base - m_preferred_base);

        // Map ELF segments into memory.
        map_segments(base_difference);

        // The relocations and relocations size.
        auto [relocations, relocations_size] =
            get_relocations(base_difference);

        // Relocate the ELF.
        relocate(relocations, relocations_size, base_difference);

        // Protect ELF segments.
        protect_segments(std::forward<Protect>(protect), base_difference);

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
     * Returns the ELF dynamic program header.
     */
    const elf_phdr & dynamic_program_header() const
    {
        return *m_program_headers;
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
    /**
     * Iterates the loadable segments using the program headers
     * and maps them into memory, given the loaded ELF base difference.
     */
    void map_segments(std::ptrdiff_t base_difference)
    {
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
                        reinterpret_cast<unsigned char *>(
                            base_difference + program_header.p_vaddr));

            // Zero memory.
            std::fill_n(reinterpret_cast<unsigned char *>(
                            base_difference + program_header.p_vaddr +
                            program_header.p_filesz),
                        program_header.p_memsz - program_header.p_filesz,
                        0);
        }
    }

    /**
     * Returns the relocation table and its size, using
     * the given base difference and the dynamic segment.
     */
    auto get_relocations(std::ptrdiff_t base_difference)
        -> std::tuple<std::variant<const elf_rela *, const elf_rel *>,
                      std::size_t>
    {
        const elf_rel * rel{};
        std::size_t rel_size{};
        const elf_rela * rela{};
        std::size_t rela_size{};

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
                rel = reinterpret_cast<const elf_rel *>(
                    base_difference + dynamic_entry.d_ptr);
                break;
            case elf_dyn::tag::rel_size:
                rel_size = dynamic_entry.d_val;
                break;
            case elf_dyn::tag::rela:
                rela = reinterpret_cast<const elf_rela *>(
                    base_difference + dynamic_entry.d_ptr);
                break;
            case elf_dyn::tag::rela_size:
                rela_size = dynamic_entry.d_val;
                break;
            default:
                break;
            }
        }

        // The relocation table and size to be returned.
        std::variant<const elf_rela *, const elf_rel *> relocations;
        std::size_t relocations_size{};

        // If rela relocations exist, prefer them, else use rel
        // relocations.
        if (rela) {
            relocations = rela;
            relocations_size = rela_size;
        } else {
            relocations = rel;
            relocations_size = rel_size;
        }

        // Return the relocation table and size.
        return {relocations, relocations_size};
    }

    /**
     * Returns the relative relocation type.
     * The behavior is undefined if the ELF machine value
     * is not defined in the 'elf_machine' enumeration.
     */
    elf_relocation_type relative_relocation_value()
    {
        switch (elf_machine(m_header->e_machine)) {
        case elf_machine::aarch64:
            return elf_relocation_type::aarch64_relative;
        case elf_machine::x86_64:
            return elf_relocation_type::x86_64_relative;
        }
    }

    /**
     * Returns the relocation type given a rel/rela info.
     */
    static int parse_relocation_type(std::uintptr_t info)
    {
        return info & 0xffffffff;
    }

    /**
     * Relocates the ELF file. Does not resolve symbols.
     */
    template <typename Relocations>
    void relocate(Relocations && relocations,
                  std::size_t relocations_size,
                  std::ptrdiff_t base_difference)
    {
        // Define the relocation strategy.
        auto relocate = [&](auto relocations) {
            // The relocation type.
            using relocation_kind = std::remove_pointer_t<
                std::remove_cv_t<decltype(relocations)>>;

            // Get relocation types.
            auto relative_relocation = relative_relocation_value();

            // Iterate rela entries.
            for (std::size_t i{}; i < relocations_size; ++i) {
                auto & relocation = relocations[i];

                // If not relative, skip.
                if (parse_relocation_type(relocation.r_info) !=
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
                    target = base_difference + relocation.r_addend;
                } else {
                    // Perform the relocation by adding base to
                    // target.
                    target += base_difference;
                }
            }
        };

        // Perform the relocations.
        std::visit(relocate, relocations);
    }

    /**
     * Iterates the loadable segments and protects them using the
     * protection function.
     */
    template <typename Protect>
    void protect_segments(Protect && protect,
                          std::ptrdiff_t base_difference)
    {
        // Iterate the program headers and change memory protection.
        for (std::size_t i{}; i < m_header->e_phnum; ++i) {
            auto & program_header = m_program_headers[i];

            // If not loadable, skip.
            if (elf_phdr::type::load !=
                elf_phdr::type(program_header.p_type)) {
                continue;
            }

            // Protect the memory range.
            protect(reinterpret_cast<unsigned char *>(
                        base_difference + program_header.p_vaddr),
                    program_header.p_memsz,
                    memory_protection(program_header.p_flags));
        }
    }

private:
    /**
     * The ELF file data pointer. If the ELF is loaded
     * this is also served as the base address.
     */
    const unsigned char * m_file_data{};

    /**
     * The ELF header pointer.
     */
    const elf_header * m_header{};

    /**
     * Points to the program header table.
     */
    const elf_phdr * m_program_headers{};

    /**
     * The address of the loadable segment with the
     * lowest virtual address, aligned to page size.
     */
    std::uintptr_t m_preferred_base{};

    /**
     * The program header which points to the dynamic
     * segment.
     */
    const elf_phdr * m_dynamic_phdr{};

    /**
     * The dynamic segment.
     */
    const elf_dyn * m_dynamic{};

    /**
     * The last program header which points to a loadable
     * segment.
     */
    const elf_phdr * m_last_load_phdr{};

    /**
     * The size in memory that the ELF requires.
     */
    std::size_t m_memory_size{};
};

} // namespace zpp