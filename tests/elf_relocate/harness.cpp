/*
 * Regression harness for zpp::elf_file's relocation.
 *
 * The loader's relocation code had never been checked against memory,
 * only against the source, and the reason it could not be is written in
 * CLAUDE.md: a constinit-only codebase never dereferences a relocated
 * slot, so a loader that wrote the wrong value into every one of them was
 * silent until `.init_array` gained its first entry. The boot processor
 * then called the module base, executed the ELF header as code and took a
 * #UD at base + 0x40.
 *
 * The bug was one trait spelling:
 *
 *     std::remove_pointer_t<std::remove_cv_t<decltype(relocations)>>
 *
 * The variant holds `const elf_rela *`. `remove_cv_t` strips *top level*
 * cv and the top level there is the pointer, which is not const - so it
 * did nothing, and stripping the pointer afterwards left `const
 * elf_rela`. That is not `elf_rela`, so the `if constexpr` chose the REL
 * branch for a RELA file: `*target += base` against a word a RELA file
 * leaves at zero, which makes every relocated slot come out as exactly
 * the module base. `sizeof` is the same either way, so the stride, the
 * entry count and every `r_offset` were right. Only the value was wrong.
 *
 * So the assertion this harness exists for is the one nothing else can
 * make: **read the result out of memory** and compare it against
 * `base + r_addend`, not merely against "something changed".
 *
 * Hosted, native, no emulator and no target. elf_file.h is a header that
 * depends on <algorithm>, <cstddef>, <cstdint>, <tuple>, <type_traits>
 * and <variant> and on nothing else, so it compiles here exactly as the
 * hypervisor compiles it - the same reason tests/decoder needs no shim.
 *
 * The images are built by hand rather than assembled, because the point
 * is to control the relocation table exactly: an assembler will not
 * produce a RELA entry whose addend is zero next to one whose addend is
 * not, nor a table whose declared size does not divide evenly.
 */
#include "zpp/elf_file.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
using elf_file = zpp::elf_file;

std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
}

void check_equal(std::uintptr_t expected,
                 std::uintptr_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n  expected 0x%llx\n  actual   0x%llx\n",
                what.c_str(),
                static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(actual));
}

/**
 * Where a hand-built image is linked. Page aligned, because the
 * constructor rounds the preferred base down to a page and a base that
 * was not aligned would make every offset below differ from every offset
 * in the file by an amount the test would have to model.
 */
constexpr std::uintptr_t preferred_base = 0x400000;

/**
 * The relative relocation number for x86-64, R_X86_64_RELATIVE. Spelled
 * out rather than taken from elf_file::enumerations, so a change to that
 * enumeration is caught here instead of being agreed with.
 */
constexpr std::uintptr_t r_x86_64_relative = 8;

/**
 * A relocation type that needs a symbol, which this loader must skip
 * rather than guess at. R_X86_64_64.
 */
constexpr std::uintptr_t r_x86_64_64 = 1;

/**
 * One hand-built image: an ELF header, two program headers, a dynamic
 * array, a relocation table and the words the relocations point at.
 *
 * The layout is fixed and the offsets are computed once here, so a test
 * below can name a target word by index and know where it landed.
 */
struct image
{
    /**
     * Number of relocation entries and of target words. One target per
     * entry, in the same order.
     */
    static constexpr std::size_t entry_count = 4;

    std::vector<unsigned char> bytes;

    std::size_t phdr_offset{};
    std::size_t dynamic_offset{};
    std::size_t table_offset{};
    std::size_t targets_offset{};

    /**
     * The size the dynamic entry declares for the relocation table.
     * Separate from the real table so a test can declare a size that
     * covers fewer entries than exist - which is what tells a byte count
     * from an entry count.
     */
    std::size_t declared_table_size{};

    template <typename T>
    T * at(std::size_t offset)
    {
        return reinterpret_cast<T *>(bytes.data() + offset);
    }

    std::uintptr_t target_address(std::size_t index) const
    {
        return preferred_base + targets_offset +
               (index * sizeof(std::uintptr_t));
    }
};

/**
 * Builds an image whose relocation table is RELA or REL.
 *
 * `addends` supplies one addend per entry, as a *virtual* address in the
 * image's own address space - which is what a linker writes. `types`
 * supplies the relocation type of each entry.
 */
image build(bool use_rela,
            const std::uintptr_t (&addends)[image::entry_count],
            const std::uintptr_t (&types)[image::entry_count],
            const std::uintptr_t (&initial_targets)[image::entry_count])
{
    using elf_header = elf_file::elf_header;
    using elf_phdr = elf_file::elf_phdr;
    using elf_dyn = elf_file::elf_dyn;
    using elf_rel = elf_file::elf_rel;
    using elf_rela = elf_file::elf_rela;

    auto entry_size = use_rela ? sizeof(elf_rela) : sizeof(elf_rel);

    image result{};
    result.phdr_offset = sizeof(elf_header);
    result.dynamic_offset = result.phdr_offset + (2 * sizeof(elf_phdr));
    // Six dynamic entries is room for a table pointer, a table size and
    // the terminating DT_NULL, with slack.
    result.table_offset = result.dynamic_offset + (6 * sizeof(elf_dyn));
    result.targets_offset =
        result.table_offset + (image::entry_count * entry_size);

    auto total = result.targets_offset +
                 (image::entry_count * sizeof(std::uintptr_t));
    result.bytes.assign(total, 0);

    auto & header = *result.at<elf_header>(0);
    std::memcpy(header.e_ident,
                "\x7f"
                "ELF\x02\x01\x01",
                7);
    header.e_type = 3;     // ET_DYN.
    header.e_machine = 62; // EM_X86_64.
    header.e_version = 1;
    header.e_entry = preferred_base;
    header.e_phoff = result.phdr_offset;
    header.e_ehsize = sizeof(elf_header);
    header.e_phentsize = sizeof(elf_phdr);
    header.e_phnum = 2;

    // One PT_LOAD covering the whole image, so p_offset and p_vaddr
    // differ by exactly the preferred base and every offset here doubles
    // as an address.
    auto * program_headers = result.at<elf_phdr>(result.phdr_offset);
    program_headers[0] = {};
    program_headers[0].p_type =
        static_cast<std::uint32_t>(elf_phdr::type::load);
    program_headers[0].p_flags = 6; // read | write.
    program_headers[0].p_offset = 0;
    program_headers[0].p_vaddr = preferred_base;
    program_headers[0].p_paddr = preferred_base;
    program_headers[0].p_filesz = total;
    program_headers[0].p_memsz = total;
    program_headers[0].p_align = 0x1000;

    program_headers[1] = {};
    program_headers[1].p_type =
        static_cast<std::uint32_t>(elf_phdr::type::dynamic);
    program_headers[1].p_flags = 6;
    program_headers[1].p_offset = result.dynamic_offset;
    program_headers[1].p_vaddr = preferred_base + result.dynamic_offset;
    program_headers[1].p_paddr = program_headers[1].p_vaddr;
    program_headers[1].p_filesz = 6 * sizeof(elf_dyn);
    program_headers[1].p_memsz = program_headers[1].p_filesz;
    program_headers[1].p_align = 8;

    result.declared_table_size = image::entry_count * entry_size;

    auto * dynamic = result.at<elf_dyn>(result.dynamic_offset);
    dynamic[0].d_tag = static_cast<std::uintptr_t>(
        use_rela ? elf_dyn::tag::rela : elf_dyn::tag::rel);
    dynamic[0].d_ptr = preferred_base + result.table_offset;
    dynamic[1].d_tag = static_cast<std::uintptr_t>(
        use_rela ? elf_dyn::tag::rela_size : elf_dyn::tag::rel_size);
    dynamic[1].d_val = result.declared_table_size;
    dynamic[2].d_tag = static_cast<std::uintptr_t>(elf_dyn::tag::null);

    for (std::size_t i{}; i < image::entry_count; ++i) {
        if (use_rela) {
            auto & entry = result.at<elf_rela>(result.table_offset)[i];
            entry.r_offset = result.target_address(i);
            entry.r_info = types[i];
            entry.r_addend = addends[i];
        } else {
            auto & entry = result.at<elf_rel>(result.table_offset)[i];
            entry.r_offset = result.target_address(i);
            entry.r_info = types[i];
        }

        result.at<std::uintptr_t>(result.targets_offset)[i] =
            initial_targets[i];
    }

    return result;
}

/**
 * Loads an image into a fresh allocation and returns the base.
 *
 * The allocation is deliberately not at the preferred base - the whole
 * question is what happens when the image moves - and it is a vector kept
 * alive by the caller.
 */
unsigned char * load(image & source, std::vector<unsigned char> & memory)
{
    elf_file file(source.bytes.data(), elf_file::state::unloaded);

    // Sized by the loader itself, so a change to how it computes the
    // image size is followed rather than duplicated here.
    auto base = static_cast<unsigned char *>(file.load(
        [&](std::size_t size) -> void * {
            memory.assign(size, 0xcd);
            return memory.data();
        },
        [](void *, std::size_t, elf_file::memory_protection) {}));

    return base;
}

/**
 * The trait spelling itself, stated both ways round.
 *
 * The wrong one is asserted to be wrong rather than left out, because
 * the whole failure was that it *looked* right: it compiles, it has the
 * correct sizeof, and it indexes the table correctly. The only thing
 * that distinguishes it is this comparison.
 */
void trait_spelling()
{
    using rela_pointer = const elf_file::elf_rela *;

    using correct = std::remove_cv_t<std::remove_pointer_t<rela_pointer>>;
    using wrong = std::remove_pointer_t<std::remove_cv_t<rela_pointer>>;

    check(std::is_same_v<correct, elf_file::elf_rela>,
          "pointer stripped first, cv second, names elf_rela");
    check(!std::is_same_v<wrong, elf_file::elf_rela>,
          "cv stripped first names const elf_rela, which is NOT elf_rela "
          "- this is the comparison the bug turned false");
    check(std::is_same_v<wrong, const elf_file::elf_rela>,
          "and it is const elf_rela specifically");

    // Same sizeof, which is why the stride, the count and every r_offset
    // stayed correct while the written value did not.
    check(sizeof(correct) == sizeof(wrong),
          "both spellings have the same size, so nothing about the walk "
          "changes - only the branch taken");
}

/**
 * RELA: the target is *stated*, not accumulated.
 */
void rela_states_the_value()
{
    // Four entries: two ordinary relative relocations with different
    // addends, one whose addend is zero, and one that needs a symbol.
    //
    // The zero addend matters: it is the only case where the buggy
    // answer and the correct answer coincide, so a test written with
    // only zero addends would have passed throughout.
    const std::uintptr_t addends[image::entry_count]{
        preferred_base + 0x11,
        preferred_base + 0x2200,
        preferred_base + 0,
        preferred_base + 0x33,
    };
    const std::uintptr_t types[image::entry_count]{
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_64,
    };
    // A RELA file leaves the target word at zero. That is exactly what
    // made the bug produce the bare base: `*target += base` on a zero.
    const std::uintptr_t initial[image::entry_count]{0, 0, 0, 0};

    auto source = build(true, addends, types, initial);
    std::vector<unsigned char> memory;
    auto * base = load(source, memory);
    check(nullptr != base, "rela image loaded");
    if (!base) {
        return;
    }

    auto difference =
        reinterpret_cast<std::uintptr_t>(base) - preferred_base;
    auto * targets =
        reinterpret_cast<std::uintptr_t *>(base + source.targets_offset);

    for (std::size_t i{}; i < 3; ++i) {
        check_equal(difference + addends[i],
                    targets[i],
                    "rela entry " + std::to_string(i) +
                        " is base plus addend");
    }

    // And the same claim stated the way the bug failed it.
    //
    // The REL branch does `*target += base_difference` on a word a RELA
    // file leaves at zero, so every relocated slot comes out as exactly
    // the base difference - the same value for all of them, whatever
    // their addend. The hypervisor is linked at zero, so there the base
    // difference *is* the module base, which is why the symptom recorded
    // in CLAUDE.md is a fault at a tiny offset from it.
    //
    // Entry 2's addend is zero, so it legitimately equals the difference
    // and is excluded; entries 0 and 1 must not.
    check(targets[0] != difference,
          "rela entry 0 is not the bare base difference - that value is "
          "the signature of the REL branch running on a RELA table");
    check(targets[1] != difference,
          "rela entry 1 is not the bare base difference");
    check(targets[0] != targets[1],
          "two rela entries with different addends do not land on the "
          "same address");

    // The entry that needs a symbol is left exactly as the file had it.
    // There is nothing to resolve against: this module links with no
    // imports, and llvm-nm -u on it must stay empty.
    check_equal(
        0,
        targets[3],
        "a relocation that needs a symbol is skipped, not guessed");
}

/**
 * REL: the target is accumulated, and the file carries the addend in the
 * word itself.
 *
 * Never exercised by a boot - `llvm-readelf -d` on the hypervisor shows
 * RELA and no REL - which is precisely why it is worth pinning: the
 * branch that was wrongly taken for years is the one nothing runs.
 */
void rel_accumulates_the_value()
{
    const std::uintptr_t addends[image::entry_count]{0, 0, 0, 0};
    const std::uintptr_t types[image::entry_count]{
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_64,
    };
    const std::uintptr_t initial[image::entry_count]{
        preferred_base + 0x11,
        preferred_base + 0x2200,
        preferred_base + 0,
        preferred_base + 0x33,
    };

    auto source = build(false, addends, types, initial);
    std::vector<unsigned char> memory;
    auto * base = load(source, memory);
    check(nullptr != base, "rel image loaded");
    if (!base) {
        return;
    }

    auto difference =
        reinterpret_cast<std::uintptr_t>(base) - preferred_base;
    auto * targets =
        reinterpret_cast<std::uintptr_t *>(base + source.targets_offset);

    for (std::size_t i{}; i < 3; ++i) {
        check_equal(difference + initial[i],
                    targets[i],
                    "rel entry " + std::to_string(i) +
                        " is its own contents plus the base difference");
    }

    check_equal(preferred_base + 0x33,
                targets[3],
                "a rel relocation that needs a symbol is skipped");
}

/**
 * The table's declared size is a **byte count**, not an entry count.
 *
 * Reading it as a count walked far past the end of the table and applied
 * a relocation for any garbage whose r_info low half happened to match.
 * Stated here from the other direction, which is the one a test can
 * assert: a size covering two entries must relocate two and leave the
 * rest alone.
 */
void size_is_bytes_not_entries()
{
    const std::uintptr_t addends[image::entry_count]{
        preferred_base + 0x11,
        preferred_base + 0x22,
        preferred_base + 0x33,
        preferred_base + 0x44,
    };
    const std::uintptr_t types[image::entry_count]{
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_relative,
        r_x86_64_relative,
    };
    const std::uintptr_t initial[image::entry_count]{0, 0, 0, 0};

    auto source = build(true, addends, types, initial);

    // Declare half the table. Two entries of four.
    using elf_dyn = elf_file::elf_dyn;
    auto * dynamic = source.at<elf_dyn>(source.dynamic_offset);
    dynamic[1].d_val = 2 * sizeof(elf_file::elf_rela);

    std::vector<unsigned char> memory;
    auto * base = load(source, memory);
    check(nullptr != base, "truncated-table image loaded");
    if (!base) {
        return;
    }

    auto difference =
        reinterpret_cast<std::uintptr_t>(base) - preferred_base;
    auto * targets =
        reinterpret_cast<std::uintptr_t *>(base + source.targets_offset);

    check_equal(difference + addends[0], targets[0], "entry 0 applied");
    check_equal(difference + addends[1], targets[1], "entry 1 applied");
    check_equal(0, targets[2], "entry 2 is past the declared size");
    check_equal(0, targets[3], "entry 3 is past the declared size");
}

/**
 * The dynamic segment is found by type, not by position.
 *
 * `dynamic_program_header` once returned the *first* program header
 * regardless of what it was, which happens to work only when PT_DYNAMIC
 * is first. Put it second here - which is where a real linker puts it,
 * after PT_LOAD - and a loader with that bug reads a PT_LOAD's contents
 * as a dynamic array.
 *
 * This is already the layout every other case above uses, so the
 * assertion is that they worked at all; stated separately so the reason
 * they are laid out that way survives someone reordering them.
 */
void dynamic_segment_found_by_type()
{
    const std::uintptr_t addends[image::entry_count]{
        preferred_base + 0x77, 0, 0, 0};
    const std::uintptr_t types[image::entry_count]{
        r_x86_64_relative, 0, 0, 0};
    const std::uintptr_t initial[image::entry_count]{0, 0, 0, 0};

    auto source = build(true, addends, types, initial);

    using elf_phdr = elf_file::elf_phdr;
    auto * program_headers = source.at<elf_phdr>(source.phdr_offset);
    check(elf_phdr::type::load ==
              elf_phdr::type(program_headers[0].p_type),
          "the first program header is PT_LOAD, so PT_DYNAMIC is not "
          "reachable by taking the first one");

    std::vector<unsigned char> memory;
    auto * base = load(source, memory);
    check(nullptr != base, "image with PT_DYNAMIC second loaded");
    if (!base) {
        return;
    }

    auto difference =
        reinterpret_cast<std::uintptr_t>(base) - preferred_base;
    auto * targets =
        reinterpret_cast<std::uintptr_t *>(base + source.targets_offset);
    check_equal(difference + addends[0],
                targets[0],
                "the relocation table named by the second program header "
                "was found and applied");
}

/**
 * .bss is zeroed.
 *
 * The allocate callback promises nothing about the contents - this
 * harness deliberately fills the allocation with 0xcd - so a global that
 * the file does not carry bytes for reads as zero only because
 * map_segments fills the tail.
 */
void bss_tail_is_zeroed()
{
    const std::uintptr_t addends[image::entry_count]{0, 0, 0, 0};
    const std::uintptr_t types[image::entry_count]{0, 0, 0, 0};
    const std::uintptr_t initial[image::entry_count]{0, 0, 0, 0};

    auto source = build(true, addends, types, initial);

    using elf_phdr = elf_file::elf_phdr;
    auto * program_headers = source.at<elf_phdr>(source.phdr_offset);
    auto file_size = program_headers[0].p_filesz;
    program_headers[0].p_memsz = file_size + 0x100;

    std::vector<unsigned char> memory;
    auto * base = load(source, memory);
    check(nullptr != base, "image with a bss tail loaded");
    if (!base) {
        return;
    }

    auto zeroed = true;
    for (std::size_t i{}; i < 0x100; ++i) {
        if (0 != base[file_size + i]) {
            zeroed = false;
        }
    }
    check(zeroed, "the bytes past p_filesz are zero, not the allocator's");
}

} // namespace

int main()
{
    trait_spelling();
    rela_states_the_value();
    rel_accumulates_the_value();
    size_is_bytes_not_entries();
    dynamic_segment_found_by_type();
    bss_tail_is_zeroed();

    std::printf(
        "elf_relocate: %zu checks, %zu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
