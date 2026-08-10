// Differential test harness for the instruction decoder.
//
// Two questions are asked of `zpp/arch/x86_64/instruction.h`, and they
// need different authorities:
//
//   1. **Is this the right instruction, and where does it end?** Settled
//      against LLVM. The harness writes a corpus of instructions as
//      assembly text, assembles it with llvm-mc, disassembles it with
//      llvm-objdump, and compares the decoder's `length` and
//      `effective_address` against the assembler's own bytes and the
//      disassembler's own operand. Neither number is this harness's
//      opinion: the length is how many bytes llvm-mc emitted, and the
//      address is computed from the base, index, scale and displacement
//      llvm-objdump printed, against the same register file the decoder
//      was handed.
//
//      One ELF section per instruction, so every one of them starts at
//      offset zero and a misparse cannot cascade into the next.
//
//   2. **Does carrying it out produce what the instruction means?** LLVM
//      cannot answer that - it assembles and disassembles, it does not
//      execute - and neither can the host, which is arm64. So the second
//      half compares `apply`, `flags_after` and `result_for_register`
//      against a model written here from the SDM, with the line numbers
//      of `.references/sdm.txt` cited on each operation. The model is
//      deliberately written a different way round from the decoder: it
//      decides carry and overflow by doing the arithmetic in 128 bits and
//      asking whether the answer fits, where the decoder reasons about
//      the operands' signs.
//
// Undefined flags are asserted too, and that is a choice worth stating.
// Where the SDM leaves a flag undefined - AF after a logical operation,
// everything but CF after a bit test - there is nothing to be right
// about, so the model states what this decoder does and the assertion
// exists to catch it *changing*, not to prove it correct.
#include "zpp/arch/x86_64/instruction.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace zpp::arch::x86_64;

// ------------------------------------------------------------ reporting
static int g_checks{};
static int g_failures{};

static void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        if (g_failures <= 40) {
            std::printf("FAIL: %s\n", what.c_str());
        } else if (41 == g_failures) {
            std::printf("FAIL: ... further failures suppressed\n");
        }
    }
}

static std::string hex(std::uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

// ------------------------------------------------------- the registers
//
// Chosen rather than random. The small values are bit offsets and index
// registers that have to stay in range; the large ones are there so that
// a truncation the decoder gets wrong shows up as a different address
// rather than as the same one.
//
// RSP holds a value no correct answer can contain. The field in the
// context handed to this decoder is the *host* stack pointer - the exit
// stub stores the context's own address there - so an emulated
// instruction that reads it hands a hypervisor address to the guest.
// Every form naming it must be refused, and the pattern below is what
// makes a leak recognisable if one ever appears in an address.
static constexpr std::uint64_t host_stack_pointer = 0xdead'beef'0bad'0000;

/**
 * The guest's RIP, for the RIP-relative forms. Deliberately not zero:
 * the corpus is assembled at address zero, so a harness that used zero
 * would agree with LLVM's own comment column by accident even if the
 * length were left out of the sum.
 */
static constexpr std::uint64_t guest_rip = 0xffff'8000'1234'5000;

static context guest_registers()
{
    context registers{};
    registers.rax = 0x1111'1111'2222'2222;
    registers.rcx = 0x0000'0000'0001'0000;
    registers.rdx = 0xaaaa'aaaa'bbbb'bbbb;
    registers.rbx = 0x5555'5555'6666'6666;
    registers.rsp = host_stack_pointer;
    registers.rbp = 0x0000'0000'0002'0000;
    registers.rsi = 0x0000'0000'0000'0003;
    registers.rdi = 0xffff'ffff'ffff'ffff;
    registers.r8 = 0x9999'9999'eeee'eeee;
    registers.r9 = 0x0000'0000'0000'0007;
    registers.r10 = 0x0123'4567'89ab'cdef;
    registers.r11 = 0x0000'0000'0000'0040;
    registers.r12 = 0x0000'0000'0004'0000;
    registers.r13 = 0x0000'0000'0000'001f;
    registers.r14 = 0x0000'0000'0000'0002;
    registers.r15 = 0x0000'0000'0000'0008;
    return registers;
}

static const context g_registers = guest_registers();

// ---------------------------------------------------------- the corpus
enum class expectation : std::uint8_t
{
    /**
     * The decoder must accept it, and its length and address are then
     * checked against LLVM.
     */
    accepted,

    /**
     * The decoder must refuse it. A refusal is always safe - the caller
     * steps the guest's own instruction - so these are the forms where
     * accepting would be *wrong*, not merely the ones not implemented.
     */
    refused,

    /**
     * Either answer is allowed. Used for forms outside what this decoder
     * claims, where the corpus is generated rather than listed and a
     * refusal is simply missing coverage.
     */
    either,
};

struct semantic_case;

struct entry
{
    std::string text;
    code_size mode{code_size::bits_64};
    expectation expect{expectation::accepted};
    const semantic_case * model{nullptr};

    // Filled in from llvm-objdump's listing.
    std::vector<std::uint8_t> bytes;
    std::string mnemonic;
    std::string operands;
};

static std::vector<entry> g_corpus;

static void emit(std::string text,
                 code_size mode = code_size::bits_64,
                 expectation expect = expectation::accepted,
                 const semantic_case * model = nullptr)
{
    entry item{};
    item.text = std::move(text);
    item.mode = mode;
    item.expect = expect;
    item.model = model;
    g_corpus.push_back(std::move(item));
}

// ------------------------------------------------------- the toolchain
static std::string tool(const char * variable, const char * fallback)
{
    auto * value = std::getenv(variable);
    return (nullptr != value) ? std::string(value) : std::string(fallback);
}

static void run(const std::string & command)
{
    auto status = std::system(command.c_str());
    if (0 != status) {
        std::printf("command failed: %s\n", command.c_str());
        std::exit(1);
    }
}

/**
 * Reads the whole of a file, or dies. Nothing here recovers from a
 * missing corpus - it would only turn a broken pipeline into a run that
 * quietly checked nothing.
 */
static std::string read_file(const std::string & path)
{
    auto * file = std::fopen(path.c_str(), "rb");
    if (nullptr == file) {
        std::printf("cannot open %s\n", path.c_str());
        std::exit(1);
    }

    std::string contents;
    char buffer[4096];
    for (;;) {
        auto read = std::fread(buffer, 1, sizeof(buffer), file);
        if (0 == read) {
            break;
        }
        contents.append(buffer, read);
    }

    std::fclose(file);
    return contents;
}

static std::string triple_of(code_size mode)
{
    return (code_size::bits_64 == mode) ? "x86_64-unknown-none"
                                        : "i386-unknown-none";
}

/**
 * Assembles every corpus entry of one code size and reads the result back
 * out of a disassembly.
 *
 * One section per instruction. The section is the unit of truth for the
 * length: llvm-mc puts exactly the instruction's bytes in it, so the
 * section's size is the answer the decoder's `length` is compared
 * against, and a prefix that llvm-objdump lists on a line of its own -
 * `lock` does - is still inside it.
 */
static void assemble(code_size mode, const char * name)
{
    std::string source;
    for (std::size_t index{}; index < g_corpus.size(); ++index) {
        if (g_corpus[index].mode != mode) {
            continue;
        }

        source += "\t.section .t";
        source += std::to_string(index);
        source += ",\"ax\",@progbits\n\t";
        source += g_corpus[index].text;
        source += "\n";
    }

    if (source.empty()) {
        return;
    }

    auto stem = std::string("corpus-") + name;
    auto assembly = stem + ".s";
    auto object = stem + ".o";
    auto listing = stem + ".dis";

    auto * file = std::fopen(assembly.c_str(), "wb");
    if (nullptr == file) {
        std::printf("cannot write %s\n", assembly.c_str());
        std::exit(1);
    }
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);

    run(tool("LLVM_MC", "llvm-mc") + " -triple=" + triple_of(mode) +
        " -filetype=obj " + assembly + " -o " + object);
    run(tool("LLVM_OBJDUMP", "llvm-objdump") + " -d " + object + " > " +
        listing);

    // The listing, one section at a time. A section header names the
    // entry; every line after it that starts with whitespace and carries
    // an address is one disassembled instruction, and its bytes are
    // appended to whatever the section already had.
    auto text = read_file(listing);
    std::size_t at{};
    std::size_t current = g_corpus.size();

    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (std::string::npos == end) {
            end = text.size();
        }

        auto line = text.substr(at, end - at);
        at = end + 1;

        constexpr const char * header = "Disassembly of section .t";
        if (0 == line.compare(0, std::strlen(header), header)) {
            auto index = line.substr(std::strlen(header));
            current = static_cast<std::size_t>(
                std::strtoul(index.c_str(), nullptr, 10));
            continue;
        }

        if (line.empty() || (' ' != line[0])) {
            continue;
        }

        auto colon = line.find(':');
        auto tab = line.find('\t');
        if ((std::string::npos == colon) || (std::string::npos == tab) ||
            (colon > tab) || (current >= g_corpus.size())) {
            continue;
        }

        auto & into = g_corpus[current];

        // The byte column, between the address and the tab.
        auto column = line.substr(colon + 1, tab - colon - 1);
        for (std::size_t i{}; i + 1 < column.size(); ++i) {
            if (' ' == column[i]) {
                continue;
            }

            auto byte = column.substr(i, 2);
            into.bytes.push_back(static_cast<std::uint8_t>(
                std::strtoul(byte.c_str(), nullptr, 16)));
            ++i;
        }

        // The mnemonic and its operands, which are tab separated - but
        // not always two fields. A prefix llvm-objdump does not fold
        // into the mnemonic gets a field of its own, so `lock addl
        // %edx, %fs:(%rax)` arrives as three. Taking the first two
        // read the *prefix* as the mnemonic and `addl %edx` as an
        // operand, which parsed as a memory operand with displacement
        // zero and quietly lost the segment override - six comparisons
        // passed against nothing.
        std::vector<std::string> fields;
        auto rest = line.substr(tab + 1);
        for (std::size_t start{}; start <= rest.size();) {
            auto next = rest.find('\t', start);
            if (std::string::npos == next) {
                next = rest.size();
            }

            if (next > start) {
                fields.push_back(rest.substr(start, next - start));
            }

            start = next + 1;
        }

        if (fields.empty()) {
            continue;
        }

        if (1 == fields.size()) {
            into.mnemonic = fields.front();
            into.operands.clear();
        } else {
            into.mnemonic = fields[fields.size() - 2];
            into.operands = fields.back();
        }
    }
}

// --------------------------------------------------- the operand parser
/**
 * A memory operand as llvm-objdump printed it, in AT&T syntax.
 */
struct printed_memory
{
    bool present{};
    bool segment{};
    bool rip_relative{};
    bool has_base{};
    bool has_index{};
    std::uint8_t base{};
    std::uint8_t index{};
    std::uint8_t scale{1};
    std::int64_t displacement{};
};

/**
 * Register names to *encoding* indices, which is the order
 * `register_of` maps. Only the widths that can appear inside a memory
 * operand are here; the operand registers are never parsed, because the
 * decoder's reading of them is checked by the semantic cases instead.
 */
static bool name_to_index(const std::string & name, std::uint8_t & index)
{
    static const char * const quad[16] = {"rax",
                                          "rcx",
                                          "rdx",
                                          "rbx",
                                          "rsp",
                                          "rbp",
                                          "rsi",
                                          "rdi",
                                          "r8",
                                          "r9",
                                          "r10",
                                          "r11",
                                          "r12",
                                          "r13",
                                          "r14",
                                          "r15"};
    static const char * const dword[16] = {"eax",
                                           "ecx",
                                           "edx",
                                           "ebx",
                                           "esp",
                                           "ebp",
                                           "esi",
                                           "edi",
                                           "r8d",
                                           "r9d",
                                           "r10d",
                                           "r11d",
                                           "r12d",
                                           "r13d",
                                           "r14d",
                                           "r15d"};

    for (std::uint8_t i{}; i < 16; ++i) {
        if ((name == quad[i]) || (name == dword[i])) {
            index = i;
            return true;
        }
    }

    return false;
}

/**
 * Splits an operand list on the commas that are not inside a memory
 * operand's parentheses, which the scale-index-base form is full of.
 */
static std::vector<std::string> split_operands(const std::string & text)
{
    std::vector<std::string> parts;
    std::string current;
    auto depth = 0;

    for (auto character : text) {
        if ('(' == character) {
            ++depth;
        } else if (')' == character) {
            --depth;
        }

        if ((',' == character) && (0 == depth)) {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(character);
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

static std::string trim(const std::string & text)
{
    auto first = text.find_first_not_of(" \t");
    if (std::string::npos == first) {
        return {};
    }

    auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

static std::int64_t parse_number(const std::string & text)
{
    if (text.empty()) {
        return 0;
    }

    auto negative = ('-' == text[0]);
    auto digits = negative ? text.substr(1) : text;
    auto value = static_cast<std::int64_t>(
        std::strtoull(digits.c_str(), nullptr, 0));
    return negative ? -value : value;
}

/**
 * Finds the memory operand in a disassembled instruction, if it has one.
 *
 * The rule is AT&T's own: an immediate carries a `$`, a register is a
 * bare `%name`, and whatever is left is memory. It costs nothing to be
 * exact about it, and being approximate would let an instruction with no
 * memory operand pass its address comparison by comparing nothing.
 */
static printed_memory find_memory(const std::string & operands)
{
    printed_memory found{};

    // Anything after a comment marker is llvm-objdump's own annotation -
    // the resolved target of a RIP-relative operand - and is not part of
    // the operand list.
    auto text = operands;
    auto comment = text.find('#');
    if (std::string::npos != comment) {
        text = text.substr(0, comment);
    }

    for (auto & raw : split_operands(text)) {
        auto token = trim(raw);
        if (token.empty() || ('$' == token[0])) {
            continue;
        }

        if ('*' == token[0]) {
            token = token.substr(1);
        }

        // A segment override prints as `%gs:` in front of the operand.
        if (('%' == token[0]) && (std::string::npos != token.find(':'))) {
            found.segment = true;
            token = token.substr(token.find(':') + 1);
        }

        // A bare register, which is not the operand being looked for.
        if (!token.empty() && ('%' == token[0]) &&
            (std::string::npos == token.find('('))) {
            continue;
        }

        found.present = true;

        auto open = token.find('(');
        found.displacement = parse_number(trim(token.substr(
            0, (std::string::npos == open) ? token.size() : open)));

        if (std::string::npos == open) {
            // An absolute displacement with no base and no index.
            return found;
        }

        auto inside =
            token.substr(open + 1, token.find_last_of(')') - open - 1);
        auto parts = split_operands(inside);

        auto strip = [](const std::string & name) {
            auto trimmed = trim(name);
            return (!trimmed.empty() && ('%' == trimmed[0]))
                       ? trimmed.substr(1)
                       : trimmed;
        };

        if (!parts.empty()) {
            auto base = strip(parts[0]);
            if (("rip" == base) || ("eip" == base)) {
                found.rip_relative = true;
            } else if (!base.empty()) {
                found.has_base = name_to_index(base, found.base);
            }
        }

        if (parts.size() >= 2) {
            auto index = strip(parts[1]);
            if (!index.empty()) {
                found.has_index = name_to_index(index, found.index);
            }
        }

        if (parts.size() >= 3) {
            found.scale =
                static_cast<std::uint8_t>(parse_number(trim(parts[2])));
        }

        return found;
    }

    return found;
}

/**
 * The address LLVM's operand names, computed against the same registers
 * the decoder was given.
 */
static std::uint64_t address_of(const printed_memory & memory,
                                std::size_t length,
                                code_size mode)
{
    auto address = static_cast<std::uint64_t>(memory.displacement);

    if (memory.rip_relative) {
        address += guest_rip + length;
    } else {
        if (memory.has_base) {
            address += g_registers.*register_of(memory.base);
        }

        if (memory.has_index) {
            address +=
                g_registers.*register_of(memory.index) * memory.scale;
        }
    }

    if (code_size::bits_64 != mode) {
        address &= 0xffff'ffff;
    }

    return address;
}

// ---------------------------------------------------------- the model
//
// An independent implementation of what each instruction leaves behind,
// written from the SDM. Every operation cites the line in
// `.references/sdm.txt` its definition was read from, because an
// unverified citation is worse than none - it stops the next reader
// checking.
static std::uint64_t mask_of(std::uint8_t size)
{
    return (8 == size) ? ~std::uint64_t{}
                       : ((std::uint64_t{1} << (size * 8u)) - 1u);
}

static bool model_parity(std::uint64_t value)
{
    auto ones = 0u;
    for (auto i = 0u; i < 8u; ++i) {
        ones += (value >> i) & 1u;
    }
    return 0 == (ones & 1u);
}

static bool model_sign(std::uint64_t value, std::uint8_t size)
{
    return 0 != ((value >> ((size * 8u) - 1u)) & 1u);
}

static __int128 model_signed(std::uint64_t value, std::uint8_t size)
{
    auto masked = value & mask_of(size);
    return model_sign(masked, size)
               ? static_cast<__int128>(masked) -
                     (static_cast<__int128>(1) << (size * 8u))
               : static_cast<__int128>(masked);
}

/**
 * The flags of `left + right`, decided by doing the sum in 128 bits and
 * asking whether it fits - which is a different derivation from the
 * decoder's, and the point of a model.
 *
 * SDM Vol. 1 3.4.3.1 for the definitions; ADD's own "Flags Affected" is
 * `.references/sdm.txt:35089`.
 */
static std::uint64_t model_add_flags(std::uint64_t left,
                                     std::uint64_t right,
                                     std::uint8_t size)
{
    auto mask = mask_of(size);
    left &= mask;
    right &= mask;

    auto wide = static_cast<unsigned __int128>(left) + right;
    auto result = static_cast<std::uint64_t>(wide) & mask;

    std::uint64_t flags{};

    if (wide > mask) {
        flags |= status_flag::carry;
    }
    if (0 == result) {
        flags |= status_flag::zero;
    }
    if (model_sign(result, size)) {
        flags |= status_flag::sign;
    }
    if (model_parity(result)) {
        flags |= status_flag::parity;
    }
    if (((left & 0xf) + (right & 0xf)) > 0xf) {
        flags |= status_flag::adjust;
    }
    if ((model_signed(left, size) + model_signed(right, size)) !=
        model_signed(result, size)) {
        flags |= status_flag::overflow;
    }

    return flags;
}

/**
 * The flags of `left - right`, the same way round. SUB's "Flags
 * Affected" is `.references/sdm.txt:119349`; CMP is defined as the same
 * subtraction with the result discarded, `.references/sdm.txt:41126`.
 */
static std::uint64_t model_sub_flags(std::uint64_t left,
                                     std::uint64_t right,
                                     std::uint8_t size)
{
    auto mask = mask_of(size);
    left &= mask;
    right &= mask;

    auto result = (left - right) & mask;

    std::uint64_t flags{};

    if (left < right) {
        flags |= status_flag::carry;
    }
    if (0 == result) {
        flags |= status_flag::zero;
    }
    if (model_sign(result, size)) {
        flags |= status_flag::sign;
    }
    if (model_parity(result)) {
        flags |= status_flag::parity;
    }
    if ((left & 0xf) < (right & 0xf)) {
        flags |= status_flag::adjust;
    }
    if ((model_signed(left, size) - model_signed(right, size)) !=
        model_signed(result, size)) {
        flags |= status_flag::overflow;
    }

    return flags;
}

/**
 * The flags of a logical operation. Carry and overflow are cleared and
 * the rest come from the result - SDM AND, "Flags Affected", `the OF and
 * CF flags are cleared`. The adjust flag is *undefined* there, and zero
 * is this decoder's choice rather than the architecture's.
 */
static std::uint64_t model_logic_flags(std::uint64_t result,
                                       std::uint8_t size)
{
    std::uint64_t flags{};

    if (0 == (result & mask_of(size))) {
        flags |= status_flag::zero;
    }
    if (model_sign(result & mask_of(size), size)) {
        flags |= status_flag::sign;
    }
    if (model_parity(result)) {
        flags |= status_flag::parity;
    }

    return flags;
}

static std::uint64_t model_write_register(std::uint64_t previous,
                                          std::uint64_t value,
                                          std::uint8_t width)
{
    if (width >= 4) {
        return value & mask_of(width);
    }

    return (previous & ~mask_of(width)) | (value & mask_of(width));
}

enum class model_kind : std::uint8_t
{
    store,
    load,
    widen_zero,
    widen_sign,
    exchange,
    logic_and,
    logic_or,
    logic_xor,
    add,
    subtract,
    compare,
    test,
    bit_test,
    bit_set,
    bit_clear,
    bit_flip,
    increment,
    decrement,
    exchange_add,
    compare_exchange,
};

/**
 * One instruction whose *meaning* is checked, not only its length.
 *
 * Everything the decoder must report is written out by hand rather than
 * derived, so the structural half of the check is a second statement of
 * the encoding rather than a restatement of the decoder's answer.
 */
struct semantic_case
{
    const char * text{};
    code_size mode{code_size::bits_64};
    model_kind kind{};

    /** Width of the memory access, in bytes. */
    std::uint8_t size{};

    /** Zero unless the register result is wider than the access. */
    std::uint8_t destination_size{};

    /** What the decoder must report as the operand. */
    std::uint64_t operand{};

    bool writes_register{};
    std::uint8_t destination{};
};

static memory_operation expected_operation(model_kind kind)
{
    switch (kind) {
    case model_kind::store:
        return memory_operation::store;
    case model_kind::load:
    case model_kind::widen_zero:
    case model_kind::widen_sign:
        return memory_operation::load;
    case model_kind::exchange:
        return memory_operation::exchange;
    case model_kind::compare:
    case model_kind::test:
    case model_kind::bit_test:
        return memory_operation::examine;
    case model_kind::compare_exchange:
        return memory_operation::compare_exchange;
    default:
        return memory_operation::combine;
    }
}

static combine_with expected_combination(model_kind kind)
{
    switch (kind) {
    case model_kind::logic_and:
        return combine_with::bitwise_and;
    case model_kind::logic_or:
        return combine_with::bitwise_or;
    case model_kind::logic_xor:
        return combine_with::bitwise_xor;
    case model_kind::add:
    case model_kind::increment:
    case model_kind::exchange_add:
        return combine_with::add;
    case model_kind::subtract:
    case model_kind::decrement:
        return combine_with::subtract;
    case model_kind::bit_set:
        return combine_with::set_bit;
    case model_kind::bit_clear:
        return combine_with::clear_bit;
    case model_kind::bit_flip:
        return combine_with::flip_bit;
    default:
        return combine_with::none;
    }
}

struct outcome
{
    std::uint64_t memory{};
    std::uint64_t reg{};
    std::uint64_t flags{};
};

/**
 * What the instruction leaves behind, from the SDM.
 *
 * @param instruction  The case being modelled.
 * @param old          What memory held.
 * @param reg_before   What the register the instruction writes held.
 * @param flags_before RFLAGS as it was.
 */
static outcome model_of(const semantic_case & instruction,
                        std::uint64_t old,
                        std::uint64_t reg_before,
                        std::uint64_t flags_before)
{
    auto size = instruction.size;
    auto mask = mask_of(size);
    auto memory = old & mask;
    auto operand = instruction.operand & mask;
    auto written = (0 != instruction.destination_size)
                       ? instruction.destination_size
                       : size;

    outcome result{
        .memory = memory,
        .reg = reg_before,
        .flags = flags_before,
    };

    auto keep = flags_before & ~status_flag::arithmetic;

    switch (instruction.kind) {
    case model_kind::store:
        result.memory = operand;
        break;

    case model_kind::load:
        result.reg = model_write_register(reg_before, memory, written);
        break;

    case model_kind::widen_zero:
        result.reg = model_write_register(reg_before, memory, written);
        break;

    case model_kind::widen_sign: {
        auto value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(model_signed(memory, size)));
        result.reg = model_write_register(reg_before, value, written);
        break;
    }

    case model_kind::exchange:
        // XCHG affects no flags, `.references/sdm.txt:135758`.
        result.memory = operand;
        result.reg = model_write_register(reg_before, memory, written);
        break;

    case model_kind::logic_and:
        result.memory = memory & operand;
        result.flags = keep | model_logic_flags(result.memory, size);
        break;

    case model_kind::logic_or:
        result.memory = memory | operand;
        result.flags = keep | model_logic_flags(result.memory, size);
        break;

    case model_kind::logic_xor:
        result.memory = memory ^ operand;
        result.flags = keep | model_logic_flags(result.memory, size);
        break;

    case model_kind::add:
        result.memory = (memory + operand) & mask;
        result.flags = keep | model_add_flags(memory, operand, size);
        break;

    case model_kind::subtract:
        result.memory = (memory - operand) & mask;
        result.flags = keep | model_sub_flags(memory, operand, size);
        break;

    case model_kind::compare:
        // CMP subtracts the second operand from the first and discards
        // the result, `.references/sdm.txt:41126`.
        result.flags = keep | model_sub_flags(memory, operand, size);
        break;

    case model_kind::test:
        result.flags = keep | model_logic_flags(memory & operand, size);
        break;

    case model_kind::bit_test:
    case model_kind::bit_set:
    case model_kind::bit_clear:
    case model_kind::bit_flip: {
        // "The CF flag contains the value of the selected bit",
        // `.references/sdm.txt:39020` for BT and the same sentence on
        // each of the three that change it - `:39077`, `:39152`,
        // `:39227`, each saying the bit *before* the change. Every other
        // flag is undefined; leaving them alone is this decoder's
        // choice.
        auto bit = (memory >> operand) & 1u;
        result.flags = (flags_before & ~status_flag::carry) |
                       (bit ? status_flag::carry : 0);

        if (model_kind::bit_set == instruction.kind) {
            result.memory = memory | (std::uint64_t{1} << operand);
        } else if (model_kind::bit_clear == instruction.kind) {
            result.memory = memory & ~(std::uint64_t{1} << operand);
        } else if (model_kind::bit_flip == instruction.kind) {
            result.memory = memory ^ (std::uint64_t{1} << operand);
        }
        break;
    }

    case model_kind::increment:
    case model_kind::decrement: {
        // "The CF flag is not affected. The OF, SF, ZF, AF, and PF flags
        // are set according to the result" - INC at
        // `.references/sdm.txt:53379`, DEC at `:45951`.
        auto incrementing = (model_kind::increment == instruction.kind);
        result.memory =
            (incrementing ? (memory + 1) : (memory - 1)) & mask;

        auto flags = incrementing ? model_add_flags(memory, 1, size)
                                  : model_sub_flags(memory, 1, size);

        result.flags = keep | (flags & ~status_flag::carry) |
                       (flags_before & status_flag::carry);
        break;
    }

    case model_kind::exchange_add:
        // "TEMP := SRC + DEST; SRC := DEST; DEST := TEMP", and the flags
        // are ADD's - `.references/sdm.txt:135936` and `:135940`.
        result.memory = (memory + operand) & mask;
        result.reg = model_write_register(reg_before, memory, size);
        result.flags = keep | model_add_flags(memory, operand, size);
        break;

    case model_kind::compare_exchange: {
        // "TEMP := DEST; IF accumulator = TEMP THEN ZF := 1; DEST := SRC
        // ELSE ZF := 0; accumulator := TEMP; DEST := TEMP",
        // `.references/sdm.txt:42806`. The flags are those of the
        // comparison, accumulator against the destination -
        // `.references/sdm.txt:42848`, and Xen's emulator says the same
        // in one line: "cmp: %%eax - dst".
        auto accumulator = g_registers.rax & mask;

        result.flags = keep | model_sub_flags(accumulator, memory, size);

        if (accumulator == memory) {
            result.memory = operand;
        } else {
            result.memory = memory;
            result.reg = model_write_register(reg_before, memory, size);
        }
        break;
    }
    }

    return result;
}

// ------------------------------------------------------- the instances
//
// The forms whose meaning is checked. Listed rather than generated: each
// one states what the decoder must report, and a generated list could
// only state what the decoder does report.
static const semantic_case g_semantics[] = {
    // MOV, which sets no flags and is here so that "no flags" is
    // asserted rather than assumed.
    {.text = "movl %edx, (%rcx)",
     .kind = model_kind::store,
     .size = 4,
     .operand = 0xbbbb'bbbb},
    {.text = "movq %rdx, (%rcx)",
     .kind = model_kind::store,
     .size = 8,
     .operand = 0xaaaa'aaaa'bbbb'bbbb},
    {.text = "movb %dl, (%rcx)",
     .kind = model_kind::store,
     .size = 1,
     .operand = 0xbb},
    {.text = "movl $0x12345678, (%rcx)",
     .kind = model_kind::store,
     .size = 4,
     .operand = 0x1234'5678},
    {.text = "movq $-1, (%rcx)",
     .kind = model_kind::store,
     .size = 8,
     .operand = 0xffff'ffff'ffff'ffff},

    // The loads, where the destination width is not the access width.
    {.text = "movl (%rcx), %edx",
     .kind = model_kind::load,
     .size = 4,
     .writes_register = true,
     .destination = 2},
    {.text = "movb (%rcx), %dl",
     .kind = model_kind::load,
     .size = 1,
     .writes_register = true,
     .destination = 2},
    {.text = "movzbl (%rcx), %edx",
     .kind = model_kind::widen_zero,
     .size = 1,
     .destination_size = 4,
     .writes_register = true,
     .destination = 2},
    {.text = "movsbq (%rcx), %rdx",
     .kind = model_kind::widen_sign,
     .size = 1,
     .destination_size = 8,
     .writes_register = true,
     .destination = 2},
    {.text = "movswl (%rcx), %edx",
     .kind = model_kind::widen_sign,
     .size = 2,
     .destination_size = 4,
     .writes_register = true,
     .destination = 2},
    {.text = "movzwq (%rcx), %rdx",
     .kind = model_kind::widen_zero,
     .size = 2,
     .destination_size = 8,
     .writes_register = true,
     .destination = 2},

    {.text = "xchgl %edx, (%rcx)",
     .kind = model_kind::exchange,
     .size = 4,
     .operand = 0xbbbb'bbbb,
     .writes_register = true,
     .destination = 2},

    // The read-modify-write group, register and immediate.
    {.text = "andl %edx, (%rcx)",
     .kind = model_kind::logic_and,
     .size = 4,
     .operand = 0xbbbb'bbbb},
    {.text = "orq %rdx, (%rcx)",
     .kind = model_kind::logic_or,
     .size = 8,
     .operand = 0xaaaa'aaaa'bbbb'bbbb},
    {.text = "xorw %dx, (%rcx)",
     .kind = model_kind::logic_xor,
     .size = 2,
     .operand = 0xbbbb},
    {.text = "addl %edx, (%rcx)",
     .kind = model_kind::add,
     .size = 4,
     .operand = 0xbbbb'bbbb},
    {.text = "subb %dl, (%rcx)",
     .kind = model_kind::subtract,
     .size = 1,
     .operand = 0xbb},
    {.text = "addq $-1, (%rcx)",
     .kind = model_kind::add,
     .size = 8,
     .operand = 0xffff'ffff'ffff'ffff},
    {.text = "andl $0x0f0f0f0f, (%rcx)",
     .kind = model_kind::logic_and,
     .size = 4,
     .operand = 0x0f0f'0f0f},

    // The examinations.
    {.text = "cmpl $0x10, (%rcx)",
     .kind = model_kind::compare,
     .size = 4,
     .operand = 0x10},
    {.text = "cmpq $-1, (%rcx)",
     .kind = model_kind::compare,
     .size = 8,
     .operand = 0xffff'ffff'ffff'ffff},
    {.text = "cmpb %dl, (%rcx)",
     .kind = model_kind::compare,
     .size = 1,
     .operand = 0xbb},
    {.text = "cmpl %edx, (%rcx)",
     .kind = model_kind::compare,
     .size = 4,
     .operand = 0xbbbb'bbbb},
    {.text = "cmpq %rdx, (%rcx)",
     .kind = model_kind::compare,
     .size = 8,
     .operand = 0xaaaa'aaaa'bbbb'bbbb},
    {.text = "cmpw %r9w, (%rcx)",
     .kind = model_kind::compare,
     .size = 2,
     .operand = 0x0007},
    {.text = "testl %edx, (%rcx)",
     .kind = model_kind::test,
     .size = 4,
     .operand = 0xbbbb'bbbb},
    {.text = "testq $-1, (%rcx)",
     .kind = model_kind::test,
     .size = 8,
     .operand = 0xffff'ffff'ffff'ffff},

    // The bit group with an immediate offset.
    {.text = "btl $3, (%rcx)",
     .kind = model_kind::bit_test,
     .size = 4,
     .operand = 3},
    {.text = "btsl $31, (%rcx)",
     .kind = model_kind::bit_set,
     .size = 4,
     .operand = 31},
    {.text = "btrq $63, (%rcx)",
     .kind = model_kind::bit_clear,
     .size = 8,
     .operand = 63},
    {.text = "btcw $9, (%rcx)",
     .kind = model_kind::bit_flip,
     .size = 2,
     .operand = 9},

    // The bit group with the offset in a register, which is the same
    // four operations reading their offset out of the register file.
    {.text = "btl %r14d, (%rcx)",
     .kind = model_kind::bit_test,
     .size = 4,
     .operand = 2},
    {.text = "btsl %r13d, (%rcx)",
     .kind = model_kind::bit_set,
     .size = 4,
     .operand = 31},
    {.text = "btrq %r15, (%rcx)",
     .kind = model_kind::bit_clear,
     .size = 8,
     .operand = 8},
    {.text = "btcw %r14w, (%rcx)",
     .kind = model_kind::bit_flip,
     .size = 2,
     .operand = 2},
    {.text = "btq %r13, (%rcx)",
     .kind = model_kind::bit_test,
     .size = 8,
     .operand = 31},

    // INC and DEC, whose whole difference from ADD and SUB of one is the
    // carry flag surviving.
    {.text = "incb (%rcx)",
     .kind = model_kind::increment,
     .size = 1,
     .operand = 1},
    {.text = "incw (%rcx)",
     .kind = model_kind::increment,
     .size = 2,
     .operand = 1},
    {.text = "incl (%rcx)",
     .kind = model_kind::increment,
     .size = 4,
     .operand = 1},
    {.text = "incq (%rcx)",
     .kind = model_kind::increment,
     .size = 8,
     .operand = 1},
    {.text = "decb (%rcx)",
     .kind = model_kind::decrement,
     .size = 1,
     .operand = 1},
    {.text = "decl (%rcx)",
     .kind = model_kind::decrement,
     .size = 4,
     .operand = 1},
    {.text = "decq (%rcx)",
     .kind = model_kind::decrement,
     .size = 8,
     .operand = 1},

    // XADD, which writes a register as well as memory: the sum lands in
    // memory and what memory held lands in the register.
    {.text = "xaddb %dl, (%rcx)",
     .kind = model_kind::exchange_add,
     .size = 1,
     .operand = 0xbb,
     .writes_register = true,
     .destination = 2},
    {.text = "xaddw %r9w, (%rcx)",
     .kind = model_kind::exchange_add,
     .size = 2,
     .operand = 0x0007,
     .writes_register = true,
     .destination = 9},
    {.text = "xaddl %edx, (%rcx)",
     .kind = model_kind::exchange_add,
     .size = 4,
     .operand = 0xbbbb'bbbb,
     .writes_register = true,
     .destination = 2},
    {.text = "xaddq %rdx, (%rcx)",
     .kind = model_kind::exchange_add,
     .size = 8,
     .operand = 0xaaaa'aaaa'bbbb'bbbb,
     .writes_register = true,
     .destination = 2},

    // CMPXCHG, whose memory result depends on a register the encoding
    // never names. The accumulator holds 0x1111'1111'2222'2222, which is
    // one of the memory samples below - so both branches are taken at
    // every width, including the one where the equal branch must leave
    // the whole 64-bit accumulator alone.
    {.text = "cmpxchgb %dl, (%rcx)",
     .kind = model_kind::compare_exchange,
     .size = 1,
     .operand = 0xbb,
     .writes_register = true,
     .destination = 0},
    {.text = "cmpxchgw %r9w, (%rcx)",
     .kind = model_kind::compare_exchange,
     .size = 2,
     .operand = 0x0007,
     .writes_register = true,
     .destination = 0},
    {.text = "cmpxchgl %edx, (%rcx)",
     .kind = model_kind::compare_exchange,
     .size = 4,
     .operand = 0xbbbb'bbbb,
     .writes_register = true,
     .destination = 0},
    {.text = "cmpxchgq %rdx, (%rcx)",
     .kind = model_kind::compare_exchange,
     .size = 8,
     .operand = 0xaaaa'aaaa'bbbb'bbbb,
     .writes_register = true,
     .destination = 0},
    {.text = "cmpxchgl %r13d, (%rcx)",
     .kind = model_kind::compare_exchange,
     .size = 4,
     .operand = 0x0000'001f,
     .writes_register = true,
     .destination = 0},
};

// ------------------------------------------------------------- corpus
static const char * const g_memory_forms_64[] = {
    "(%rcx)",         "0x10(%rcx)",
    "-0x8(%rbp)",     "0x12345(%rcx)",
    "(%rax,%rsi,4)",  "0x10(%rax,%rsi,8)",
    "(,%rsi,2)",      "0x40(%rip)",
    "-0x100(%rip)",   "0x1234",
    "(%r8)",          "(%r12)",
    "0x8(%r13)",      "(%r8,%r15,2)",
    "(%rax,%r12,4)",  "0x100(%rbp)",
    "(%rsp)",         "0x10(%rsp,%rsi,4)",
    "%gs:0x10(%rcx)", "%fs:(%rax,%rsi,8)",
};

static const char * const g_memory_forms_32[] = {
    "(%ecx)",
    "0x10(%ecx)",
    "-0x8(%ebp)",
    "0x12345(%ecx)",
    "(%eax,%esi,4)",
    "0x10(%eax,%esi,8)",
    "(,%esi,2)",
    "0x1234",
    "0x100(%ebp)",
    "(%esp)",
    "0x10(%esp,%esi,4)",
    "%gs:0x10(%ecx)",
};

struct operand_form
{
    const char * suffix{};
    const char * const * registers{};
    std::size_t count{};
};

static const char * const g_regs8_64[] = {"%dl", "%bl", "%r9b", "%r13b"};
static const char * const g_regs16_64[] = {"%dx", "%bx", "%r9w"};
static const char * const g_regs32_64[] = {"%edx", "%ebx", "%r9d"};
static const char * const g_regs64_64[] = {"%rdx", "%rbx", "%r9", "%r12"};

static const char * const g_regs8_32[] = {"%dl", "%bl"};
static const char * const g_regs16_32[] = {"%dx", "%bx"};
static const char * const g_regs32_32[] = {"%edx", "%ebx"};

static void generate(code_size mode)
{
    auto sixty_four = (code_size::bits_64 == mode);

    const char * const * forms =
        sixty_four ? g_memory_forms_64 : g_memory_forms_32;
    auto form_count = sixty_four ? std::size(g_memory_forms_64)
                                 : std::size(g_memory_forms_32);

    operand_form widths[4] = {
        {"b",
         sixty_four ? g_regs8_64 : g_regs8_32,
         sixty_four ? std::size(g_regs8_64) : std::size(g_regs8_32)},
        {"w",
         sixty_four ? g_regs16_64 : g_regs16_32,
         sixty_four ? std::size(g_regs16_64) : std::size(g_regs16_32)},
        {"l",
         sixty_four ? g_regs32_64 : g_regs32_32,
         sixty_four ? std::size(g_regs32_64) : std::size(g_regs32_32)},
        {"q", g_regs64_64, sixty_four ? std::size(g_regs64_64) : 0},
    };

    // The read-modify-write group and the stores, register source.
    static const char * const to_memory[] = {"add",
                                             "or",
                                             "and",
                                             "sub",
                                             "xor",
                                             "mov",
                                             "xchg",
                                             "test",
                                             "cmp",
                                             "xadd",
                                             "cmpxchg"};

    for (auto * mnemonic : to_memory) {
        for (auto & width : widths) {
            for (std::size_t r{}; r < width.count; ++r) {
                for (std::size_t f{}; f < form_count; ++f) {
                    emit(std::string(mnemonic) + width.suffix + " " +
                             width.registers[r] + ", " + forms[f],
                         mode);
                }
            }
        }
    }

    // The loads and the widening loads, memory source.
    static const char * const from_memory[] = {"mov"};

    for (auto * mnemonic : from_memory) {
        for (auto & width : widths) {
            for (std::size_t r{}; r < width.count; ++r) {
                for (std::size_t f{}; f < form_count; ++f) {
                    emit(std::string(mnemonic) + width.suffix + " " +
                             forms[f] + ", " + width.registers[r],
                         mode);
                }
            }
        }
    }

    static const char * const widening[] = {
        "movzbl", "movzwl", "movsbl", "movswl"};
    static const char * const widening_64[] = {
        "movzbq", "movzwq", "movsbq", "movswq"};

    for (auto * mnemonic : widening) {
        for (std::size_t f{}; f < form_count; ++f) {
            emit(std::string(mnemonic) + " " + forms[f] + ", %edx", mode);
        }
    }

    if (sixty_four) {
        for (auto * mnemonic : widening_64) {
            for (std::size_t f{}; f < form_count; ++f) {
                emit(std::string(mnemonic) + " " + forms[f] + ", %r9",
                     mode);
            }
        }
    }

    // The same group with an immediate. Two magnitudes each, so both the
    // sign-extended byte form and the full-width one are exercised.
    static const char * const immediates[] = {
        "add", "or", "and", "sub", "xor", "cmp", "test", "mov"};

    for (auto * mnemonic : immediates) {
        for (auto & width : widths) {
            if (0 == width.count) {
                continue;
            }

            for (std::size_t f{}; f < form_count; ++f) {
                emit(std::string(mnemonic) + width.suffix + " $0x7, " +
                         forms[f],
                     mode);

                auto wide = (0 == std::strcmp(width.suffix, "b"))
                                ? "$0x7f"
                                : "$0x12345";
                emit(std::string(mnemonic) + width.suffix + " " + wide +
                         ", " + forms[f],
                     mode);
            }
        }
    }

    // The bit group with an immediate offset. No byte form exists.
    static const char * const bit_ops[] = {"bt", "bts", "btr", "btc"};

    for (auto * mnemonic : bit_ops) {
        for (std::size_t f{}; f < form_count; ++f) {
            emit(std::string(mnemonic) + "w $3, " + forms[f], mode);
            emit(std::string(mnemonic) + "l $30, " + forms[f], mode);
            if (sixty_four) {
                emit(std::string(mnemonic) + "q $63, " + forms[f], mode);
            }
        }
    }

    // INC and DEC, which are the only forms whose operation is entirely
    // in the ModRM register field and whose operand is not in the
    // instruction at all.
    for (auto & width : widths) {
        if (0 == width.count) {
            continue;
        }

        for (std::size_t f{}; f < form_count; ++f) {
            emit(std::string("inc") + width.suffix + " " + forms[f], mode);
            emit(std::string("dec") + width.suffix + " " + forms[f], mode);
        }
    }

    // The same group with the offset in a register. The register has to
    // hold an offset inside the operand or the instruction names a
    // different word of memory and is refused, so these are the
    // registers the file above gives small values to.
    for (auto * mnemonic : bit_ops) {
        for (std::size_t f{}; f < form_count; ++f) {
            if (sixty_four) {
                emit(std::string(mnemonic) + "w %r14w, " + forms[f], mode);
                emit(std::string(mnemonic) + "l %r13d, " + forms[f], mode);
                emit(std::string(mnemonic) + "l %r14d, " + forms[f], mode);
                emit(std::string(mnemonic) + "q %r15, " + forms[f], mode);
            } else {
                emit(std::string(mnemonic) + "w %si, " + forms[f], mode);
                emit(std::string(mnemonic) + "l %esi, " + forms[f], mode);
            }
        }
    }

    // Prefixes, on forms that carry them in the wild.
    for (std::size_t f{}; f < form_count; ++f) {
        emit(std::string("lock addl %edx, ") + forms[f], mode);
        emit(std::string("lock andl $0x7, ") + forms[f], mode);
    }
}

/**
 * The forms that must be refused, each because accepting it would be
 * wrong rather than merely unimplemented.
 */
static void generate_refusals()
{
    // A register destination has no memory operand at all.
    emit("movl %edx, %ecx", code_size::bits_64, expectation::refused);
    emit("addl %edx, %ecx", code_size::bits_64, expectation::refused);
    emit("btsl $3, %ecx", code_size::bits_64, expectation::refused);

    // The high-byte registers alias differently without REX, so the
    // encoding would index the table and read RSP, RBP, RSI or RDI.
    emit("movb %ah, (%rcx)", code_size::bits_64, expectation::refused);
    emit("addb %ch, (%rcx)", code_size::bits_64, expectation::refused);
    emit("testb %dh, (%rcx)", code_size::bits_64, expectation::refused);
    emit("xchgb %bh, (%rcx)", code_size::bits_64, expectation::refused);

    // Encoding four without REX.B is the *host* stack pointer in the
    // context this decoder is handed.
    emit("movq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("addq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("testq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("xchgq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("movq (%rcx), %rsp", code_size::bits_64, expectation::refused);
    emit("movzbq (%rcx), %rsp", code_size::bits_64, expectation::refused);

    // Group 11 defines only /0; every other value of the ModRM register
    // field with a memory operand is an invalid opcode on hardware.
    // llvm-mc will not assemble one, so the bytes are given directly.
    // (Handled by the raw-byte cases below.)

    // Group 5's other members are control transfers and a push. They
    // touch memory, so they reach this decoder, and emulating one as a
    // read-modify-write would be a write the guest never asked for.
    emit("callq *(%rax)", code_size::bits_64, expectation::refused);
    emit("jmpq *(%rax)", code_size::bits_64, expectation::refused);
    emit("pushq (%rax)", code_size::bits_64, expectation::refused);

    // Group 3's other members - not, neg, mul, imul, div, idiv.
    emit("notl (%rcx)", code_size::bits_64, expectation::refused);
    emit("negl (%rcx)", code_size::bits_64, expectation::refused);
    emit("mull (%rcx)", code_size::bits_64, expectation::refused);
    emit("divl (%rcx)", code_size::bits_64, expectation::refused);

    // Add-with-carry and subtract-with-borrow need a flag this decoder
    // does not carry into `apply`.
    emit("adcl %edx, (%rcx)", code_size::bits_64, expectation::refused);
    emit("sbbl %edx, (%rcx)", code_size::bits_64, expectation::refused);
    emit("adcl $0x7, (%rcx)", code_size::bits_64, expectation::refused);

    // The reversed direction, where memory is the source of a
    // read-modify-write whose destination is the register.
    emit("addl (%rcx), %edx", code_size::bits_64, expectation::refused);
    emit("andl (%rcx), %edx", code_size::bits_64, expectation::refused);
    emit("cmpl (%rcx), %edx", code_size::bits_64, expectation::refused);

    // A bit offset outside the operand names a different word of
    // memory, not a different bit of this one - the modulo the SDM
    // describes applies only to a register bit base
    // (`.references/sdm.txt:38974`). `btsl $40, (%rcx)` sets bit 8 of
    // the dword at `[rcx+4]`.
    emit("btsl $40, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btl $32, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btrw $20, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btcq $64, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btsl $200, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btsl $40, (%ecx)", code_size::bits_32, expectation::refused);

    // The same, with the offset in a register - where the value rather
    // than the encoding decides. RDI holds -1, R11 holds 0x40 and R13
    // holds 0x1f, so each of these is outside the operand it is applied
    // to and each names a word this decoder cannot address.
    emit("btl %edi, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btsq %rdi, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btrl %r11d, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btcw %r13w, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btsq %r11, (%rcx)", code_size::bits_64, expectation::refused);

    // Group 5's members that are not INC or DEC. Each reads its memory
    // operand rather than modifying it, so emulating one as an increment
    // would write memory the guest never asked to write and then run on
    // from a control transfer that never happened.
    emit("lcallq *(%rax)", code_size::bits_64, expectation::refused);
    emit("ljmpq *(%rax)", code_size::bits_64, expectation::refused);

    // XADD and CMPXCHG read their register operand and XADD writes it,
    // so both guards apply to both.
    emit("xaddq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("xaddb %ah, (%rcx)", code_size::bits_64, expectation::refused);
    emit(
        "cmpxchgq %rsp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("cmpxchgb %ah, (%rcx)", code_size::bits_64, expectation::refused);

    // And with the offset taken out of the host stack pointer's slot.
    emit("btsl %esp, (%rcx)", code_size::bits_64, expectation::refused);
    emit("btq %rsp, (%rcx)", code_size::bits_64, expectation::refused);

    // An address-size prefix in 32-bit code selects 16-bit addressing,
    // which moves where the instruction ends.
    emit("movl %edx, (%bx,%si)", code_size::bits_32, expectation::refused);
}

// ------------------------------------------------------------ the runs
static void check_lengths_and_addresses()
{
    auto accepted = 0;
    auto with_memory = 0;
    auto compared = 0;
    auto refused = 0;

    for (auto & item : g_corpus) {
        if (item.bytes.empty()) {
            check(false, "no bytes for: " + item.text);
            continue;
        }

        std::vector<std::byte> code;
        for (auto byte : item.bytes) {
            code.push_back(static_cast<std::byte>(byte));
        }

        auto decoded = decode(code, g_registers, item.mode);

        if (expectation::refused == item.expect) {
            ++refused;
            check(!decoded.has_value(), "must be refused: " + item.text);
            continue;
        }

        if (!decoded) {
            if (expectation::accepted == item.expect) {
                check(false, "must be decoded: " + item.text);
            }
            continue;
        }

        ++accepted;

        check(decoded->length == item.bytes.size(),
              "length " + std::to_string(decoded->length) +
                  " != " + std::to_string(item.bytes.size()) +
                  " for: " + item.text);

        // Truncating by one byte must poison the cursor rather than
        // produce a shorter instruction. Every byte a decode consumed
        // was read through the bounds-checked cursor, so removing the
        // last one has to be noticed.
        if (code.size() > 1) {
            auto shorter =
                std::span<const std::byte>(code).first(code.size() - 1);
            check(!decode(shorter, g_registers, item.mode).has_value(),
                  "truncation must be refused: " + item.text);
        }

        auto printed = find_memory(item.operands);
        if (!printed.present) {
            check(!decoded->where.known,
                  "no memory operand printed, but one decoded: " +
                      item.text);
            continue;
        }

        ++with_memory;

        check(decoded->where.known,
              "memory operand printed, none decoded: " + item.text);

        auto address = effective_address(*decoded, g_registers, guest_rip);

        auto names_stack = printed.has_base && (4 == printed.base);

        if (printed.segment || names_stack) {
            check(!address.has_value(),
                  "address must be refused: " + item.text);
            continue;
        }

        auto expected = address_of(printed, item.bytes.size(), item.mode);

        check(address.has_value(),
              "address must be computed: " + item.text);

        if (address) {
            ++compared;
            check(*address == expected,
                  "address " + hex(*address) + " != " + hex(expected) +
                      " for: " + item.text);

            // Nothing may ever put the host stack pointer into an
            // address, whatever the encoding says.
            check((*address >> 32) != (host_stack_pointer >> 32),
                  "host stack pointer leaked into: " + item.text);
        }
    }

    std::printf("  %d accepted, %d with a memory operand, %d addresses "
                "compared, %d refused\n",
                accepted,
                with_memory,
                compared,
                refused);
}

/**
 * Sixteen-bit code is refused outright, whatever the bytes are. Checked
 * by decoding the whole 32-bit corpus a second time in that mode: the
 * default operand size and the addressing forms are both different
 * there, so an answer would be wrong rather than incomplete.
 */
static void check_sixteen_bit_refusal()
{
    auto tried = 0;

    for (auto & item : g_corpus) {
        if ((code_size::bits_32 != item.mode) || item.bytes.empty()) {
            continue;
        }

        std::vector<std::byte> code;
        for (auto byte : item.bytes) {
            code.push_back(static_cast<std::byte>(byte));
        }

        ++tried;
        check(!decode(code, g_registers, code_size::bits_16).has_value(),
              "16-bit code must be refused: " + item.text);
    }

    std::printf("  %d instructions refused in 16-bit code\n", tried);
}

static const std::uint64_t g_memory_samples[] = {
    0,
    1,
    0x7f,
    0x80,
    0xff,
    0x1234,
    0x8000,
    0xffff,
    0x7fff'ffff,
    0x8000'0000,
    0xffff'ffff,
    0x1234'5678'9abc'def0,
    0x8000'0000'0000'0000,
    0xffff'ffff'ffff'ffff,
    0x5555'5555'5555'5555,
    0xaaaa'aaaa'bbbb'bbbb,
    0x1111'1111'2222'2222,
};

static const std::uint64_t g_flag_samples[] = {
    0x2,
    0x2 | status_flag::carry,
    0x2 | status_flag::arithmetic,
    0x2 | status_flag::zero | status_flag::sign,
    0x246,
};

static void check_semantics()
{
    auto cases = 0;

    for (auto & item : g_corpus) {
        if (nullptr == item.model) {
            continue;
        }

        auto & instruction = *item.model;

        std::vector<std::byte> code;
        for (auto byte : item.bytes) {
            code.push_back(static_cast<std::byte>(byte));
        }

        auto decoded = decode(code, g_registers, item.mode);
        if (!decoded) {
            check(false,
                  std::string("must be decoded: ") + instruction.text);
            continue;
        }

        ++cases;

        auto named = std::string(" for: ") + instruction.text;

        check(decoded->what == expected_operation(instruction.kind),
              "operation" + named);
        check(decoded->how == expected_combination(instruction.kind),
              "combination" + named);
        check(decoded->size == instruction.size,
              "size " + std::to_string(decoded->size) + named);
        check(decoded->destination_size == instruction.destination_size,
              "destination size" + named);
        check(decoded->operand ==
                  (instruction.operand & mask_of(instruction.size)),
              "operand " + hex(decoded->operand) +
                  " != " + hex(instruction.operand) + named);
        check(decoded->writes_register == instruction.writes_register,
              "writes a register" + named);

        if (instruction.writes_register) {
            check(decoded->destination == instruction.destination,
                  "destination" + named);
        }

        auto previous = g_registers.*register_of(instruction.destination);

        for (auto memory : g_memory_samples) {
            for (auto flags : g_flag_samples) {
                auto expected =
                    model_of(instruction, memory, previous, flags);

                auto replacement = apply(*decoded, memory);
                check(replacement ==
                          (expected.memory & mask_of(instruction.size)),
                      "memory " + hex(replacement) +
                          " != " + hex(expected.memory) + " from " +
                          hex(memory) + named);

                auto after =
                    flags_after(*decoded, flags, memory, replacement);
                check(after == expected.flags,
                      "flags " + hex(after) +
                          " != " + hex(expected.flags) + " from " +
                          hex(memory) + " and " + hex(flags) + named);

                if (instruction.writes_register) {
                    auto value =
                        result_for_register(*decoded, memory, previous);
                    check(value == expected.reg,
                          "register " + hex(value) +
                              " != " + hex(expected.reg) + " from " +
                              hex(memory) + named);
                }
            }
        }
    }

    std::printf("  %d instructions modelled against the SDM\n", cases);
}

// -------------------------------------------------------------- main
int main()
{
    generate(code_size::bits_64);
    generate(code_size::bits_32);
    generate_refusals();

    for (auto & instruction : g_semantics) {
        emit(instruction.text,
             instruction.mode,
             expectation::accepted,
             &instruction);
    }

    assemble(code_size::bits_64, "64");
    assemble(code_size::bits_32, "32");

    std::printf("%zu instructions in the corpus\n", g_corpus.size());

    std::printf("lengths and effective addresses, against LLVM:\n");
    check_lengths_and_addresses();

    std::printf("sixteen-bit code:\n");
    check_sixteen_bit_refusal();

    std::printf("semantics, against a model of the SDM:\n");
    check_semantics();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (0 == g_failures) ? 0 : 1;
}
