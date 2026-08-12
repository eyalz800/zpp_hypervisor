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
        m_file_data(reinterpret_cast<const unsigned char *>(file_data)),
        m_header(reinterpret_cast<const elf_header *>(file_data)),
        m_program_headers(reinterpret_cast<const elf_phdr *>(
            m_file_data + m_header->e_phoff)),

        // Where the linker laid the image out, and so the origin every
        // other address here is relative to. Rounded down because the
        // first load segment may begin part way into a page, and the
        // whole of that page has to be inside the allocation.
        m_preferred_base(
            std::find_if(m_program_headers,
                         m_program_headers + m_header->e_phnum,
                         [](auto & program_header) {
                             return elf_phdr::type::load ==
                                    elf_phdr::type(program_header.p_type);
                         })
                ->p_vaddr &
            ~0xfff),

        m_dynamic_phdr(std::find_if(m_program_headers,
                                    m_program_headers + m_header->e_phnum,
                                    [](auto & program_header) {
                                        return elf_phdr::type::dynamic ==
                                               elf_phdr::type(
                                                   program_header.p_type);
                                    })),

        // The whole reason `state` exists. A segment sits at p_offset
        // in a file and at p_vaddr in a loaded image, and those differ
        // - so reading a loaded image at p_offset lands inside some
        // other segment. The subtraction is the load bias.
        m_dynamic(reinterpret_cast<const elf_dyn *>(
            (state::unloaded == elf_state)
                ? m_file_data + m_dynamic_phdr->p_offset
                : m_dynamic_phdr->p_vaddr +
                      (m_file_data - m_preferred_base))),

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

        // One span covering every segment and the gaps between them,
        // since the image has to stay at fixed offsets from a single
        // base. p_memsz, not p_filesz: they differ by .bss, which here
        // is most of the module - the per-processor stacks alone are
        // megabytes.
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
        auto base = static_cast<unsigned char *>(allocate(m_memory_size));
        if (!base) {
            return nullptr;
        }

        // How far the image moved from where it was linked, which added
        // to any address out of the file gives where that thing now
        // lives - which is all a relative relocation is.
        auto base_difference =
            reinterpret_cast<std::ptrdiff_t>(base - m_preferred_base);

        // The only order that works: DT_REL/DT_RELA hold virtual
        // addresses that do not resolve until the segments are mapped,
        // and relocating writes into pages protection would close.
        map_segments(base_difference);

        auto [relocations, relocations_size] =
            get_relocations(base_difference);

        relocate(relocations, relocations_size, base_difference);

        protect_segments(std::forward<Protect>(protect), base_difference);

        return base;
    }

    /**
     * Applies the loadable segments' own permissions to an image that is
     * already in memory, through the same protection strategy `load`
     * takes. The behavior is undefined if the ELF file is not loaded.
     *
     * This exists because the two halves of `load` have different owners
     * here. The loaders place the image and relocate it, into one
     * readable, writable and executable region, because that is all a
     * platform allocator gives them and because relocating writes into
     * pages protection would close. The hypervisor builds a page table of
     * its own afterwards and is the only thing that can express per
     * segment permissions in it - by which time the load is long over, so
     * it needs the protection pass without the rest.
     */
    template <typename Protect>
    void protect(Protect && protect_callback)
    {
        // The load bias of an image already in memory: where it is, less
        // where it was linked. The same subtraction the constructor makes
        // to find the dynamic segment in a loaded image.
        protect_segments(
            std::forward<Protect>(protect_callback),
            reinterpret_cast<std::ptrdiff_t>(m_file_data) -
                static_cast<std::ptrdiff_t>(m_preferred_base));
    }

    /**
     * Returns the entry relative to file to be mapped in memory.
     */
    constexpr std::uintptr_t entry() const
    {
        return m_header->e_entry - m_preferred_base;
    }

    /**
     * Returns the file data.
     */
    constexpr const unsigned char * file_data() const
    {
        return m_file_data;
    }

    /**
     * Returns the ELF header.
     */
    constexpr const elf_header & header() const
    {
        return *m_header;
    }

    /**
     * Returns the ELF program headers.
     */
    constexpr const elf_phdr * program_headers() const
    {
        return m_program_headers;
    }

    /**
     * Returns the ELF dynamic program header.
     */
    constexpr const elf_phdr & dynamic_program_header() const
    {
        return *m_dynamic_phdr;
    }

    /**
     * Returns the ELF dynamic segment.
     */
    constexpr const elf_dyn * dynamic() const
    {
        return m_dynamic;
    }

    /**
     * Returns the ELF preferred base.
     */
    constexpr std::uintptr_t preferred_base() const
    {
        return m_preferred_base;
    }

    /**
     * Returns the size ELF requires in memory.
     */
    constexpr std::size_t memory_size() const
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
        for (std::size_t i{}; i < m_header->e_phnum; ++i) {
            auto & program_header = m_program_headers[i];

            if (elf_phdr::type::load !=
                elf_phdr::type(program_header.p_type)) {
                continue;
            }

            std::copy_n(m_file_data + program_header.p_offset,
                        program_header.p_filesz,
                        reinterpret_cast<unsigned char *>(
                            base_difference + program_header.p_vaddr));

            // The tail beyond p_filesz is .bss, and the allocate
            // callback promises nothing about the contents. Zeroing it
            // here is what makes a zero-initialized global read as
            // zero.
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

        // Unbounded because the dynamic segment carries no count -
        // DT_NULL is the only thing that ends it.
        for (std::size_t i{};; ++i) {
            auto & dynamic_entry = m_dynamic[i];
            auto tag = elf_dyn::tag(dynamic_entry.d_tag);

            if (elf_dyn::tag::null == tag) {
                break;
            }

            // Each table and its size are separate entries in no
            // guaranteed order, so all four are collected first and
            // paired up afterwards.
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

        // A variant rather than a pointer and a flag, so that relocate
        // dispatches on the type - the two forms hold their value in
        // different places, and getting that wrong is the bug
        // documented there.
        std::variant<const elf_rela *, const elf_rel *> relocations;
        std::size_t relocations_size{};

        // `llvm-readelf -d` on the hypervisor shows RELA and no REL, so
        // the REL branch has never been exercised by a boot.
        if (rela) {
            relocations = rela;
            relocations_size = rela_size;
        } else {
            relocations = rel;
            relocations_size = rel_size;
        }

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

        return {};
    }

    /**
     * Returns the relocation type given a rel/rela info.
     */
    static constexpr int parse_relocation_type(std::uintptr_t info)
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
        // One body instantiated for each alternative, so the two forms
        // cannot drift apart; the `if constexpr` below is the only
        // place they differ.
        auto relocate = [&](auto relocations) {
            // The relocation type. Strip the pointer first and the cv
            // second, never the other way around: the variant holds
            // 'const elf_rela *', whose top level is the pointer and not
            // const, so removing cv first is a no-op and leaves
            // 'const elf_rela' after the pointer goes. That compares
            // unequal to 'elf_rela' below, which silently sent every RELA
            // relocation down the REL branch - it added the base to a
            // target that holds zero in a RELA file, so every relocated
            // slot became the module base instead of base plus addend.
            // Nothing dereferenced one until .init_array gained an entry,
            // and the boot CPU then called the module base and executed
            // the ELF header.
            using relocation_kind = std::remove_cv_t<
                std::remove_pointer_t<decltype(relocations)>>;

            // The relative relocation number differs per architecture,
            // and it is the only kind handled here.
            auto relative_relocation = relative_relocation_value();

            auto relocations_count =
                relocations_size / sizeof(relocation_kind);

            for (std::size_t i{}; i < relocations_count; ++i) {
                auto & relocation = relocations[i];

                // Anything else needs a symbol resolved, and there is
                // nothing to resolve against - this module links with
                // no imports and llvm-nm -u on it must stay empty.
                if (parse_relocation_type(relocation.r_info) !=
                    relative_relocation) {
                    continue;
                }

                auto & target = *reinterpret_cast<std::uintptr_t *>(
                    base_difference + relocation.r_offset);

                // RELA states the value, REL accumulates it. A RELA
                // file leaves the target word zero, so treating one as
                // the other writes the bare base.
                if constexpr (std::is_same_v<relocation_kind, elf_rela>) {
                    target = base_difference + relocation.r_addend;
                } else {
                    target += base_difference;
                }
            }
        };

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
        // p_memsz rather than p_filesz, so a segment's .bss tail gets
        // the same protection as the rest of it. Every loader here
        // passes a callback that does nothing - they own one RWX region
        // and have nothing to say per segment - so the only caller that
        // acts on this is the hypervisor, through `protect` above, once
        // it is building its own page table.
        for (std::size_t i{}; i < m_header->e_phnum; ++i) {
            auto & program_header = m_program_headers[i];

            if (elf_phdr::type::load !=
                elf_phdr::type(program_header.p_type)) {
                continue;
            }

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
