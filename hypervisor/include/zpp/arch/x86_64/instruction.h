#pragma once
#include "zpp/arch/x86_64/context.h"
#include <cstdint>
#include <optional>
#include <span>

namespace zpp::arch::x86_64
{
/**
 * Decoding the instructions that touch memory, far enough to carry one out
 * on the guest's behalf.
 *
 * The narrow decoder beside this one answers a single question - "what
 * value is this MOV storing" - and refuses everything else. That was
 * enough while the only watched page was a device's configuration
 * register, written by a driver that compiles to a plain MOV. It is not
 * enough for a page an operating system touches through its own
 * abstractions: measured on the local APIC page, a large fraction of a
 * guest's writes were forms it could not answer, and a write this VMM
 * cannot decode is one it cannot report to whoever is watching the page.
 *
 * The shape here is deliberately two steps, and the split is the design:
 *
 *   1. `decode` reads the bytes and says *what the instruction would do* -
 *      which operation, at what width, against which operand. It never
 *      touches memory and never needs to.
 *   2. `apply` takes that description together with the value currently in
 *      memory and returns the value that should replace it, plus whatever
 *      the instruction leaves in a register.
 *
 * A read-modify-write instruction is the reason. `and [mem], eax` needs
 * the old contents to compute the new ones, and a decoder that went and
 * fetched them would need a mapping, a lock and a fault path - none of
 * which belongs in something that should be a pure function over bytes.
 * Keeping it pure is also what makes it testable at compile time, which is
 * how every operation below is checked.
 *
 * What is deliberately absent: anything that computes an address. The
 * faulting address is reported by the hardware in the VMCS, so the
 * addressing bytes are walked only to find where the instruction ends and
 * never interpreted. That is the one piece of a general emulator this does
 * not need, and leaving it out removes segmentation, the address-size
 * prefix and the whole of RIP-relative arithmetic from the surface.
 */

/**
 * What an instruction does to the memory it touches.
 */
enum class memory_operation : std::uint8_t
{
    /**
     * The value is replaced outright. The old contents are not read, so
     * `apply` ignores what was there.
     */
    store,

    /**
     * The old contents are needed, combined with the operand, and written
     * back. `apply`'s result is the combination.
     */
    combine,

    /**
     * The old contents are read into a register and memory is unchanged.
     */
    load,

    /**
     * The old contents and the operand are swapped.
     */
    exchange,

    /**
     * The old contents are examined and memory is unchanged - a compare or
     * a test. Present so that a watch sees the access without the value
     * being disturbed.
     */
    examine,
};

/**
 * How `combine` combines. Named after what the instruction means rather
 * than after its opcode, so a reader does not have to hold an encoding
 * table in their head.
 */
enum class combine_with : std::uint8_t
{
    none,
    bitwise_and,
    bitwise_or,
    bitwise_xor,
    add,
    subtract,
    set_bit,
    clear_bit,
    flip_bit,
};

/**
 * One decoded instruction, as much of it as carrying it out requires.
 */
struct decoded_instruction
{
    memory_operation what{};
    combine_with how{combine_with::none};

    /**
     * Width of the memory access in bytes: 1, 2, 4 or 8.
     */
    std::uint8_t size{};

    /**
     * How long the instruction is, in bytes.
     *
     * Reported because the VMCS field that would otherwise answer it is
     * undefined for the exit that needs it - SDM 30.2.5 leaves the VM-exit
     * instruction length unset for an EPT violation outside event
     * delivery, and KVM's handle_ept_violation accordingly never reads it.
     */
    std::uint8_t length{};

    /**
     * The operand the instruction brings to the memory it touches: the
     * value stored, the value combined in, or the value exchanged.
     * Zero-extended to 64 bits and already truncated to `size`.
     */
    std::uint64_t operand{};

    /**
     * Where a result goes, for the forms that leave one in a register.
     * Meaningful when `writes_register` is set.
     *
     * An index into the encoding order, not into the context's layout -
     * `register_of` is the only thing that knows the difference.
     */
    std::uint8_t destination{};
    bool writes_register{};

    /**
     * Whether the result written to that register is sign-extended from
     * `size` rather than zero-extended, which is what separates MOVSX from
     * MOVZX and is the sort of difference that survives testing if it is
     * not stated.
     */
    bool sign_extends{};
};

namespace detail
{
/**
 * The general purpose registers in *encoding* order.
 *
 * Not declaration order, which is alphabetical-ish and puts rbx where rcx
 * belongs. Getting this wrong reads a plausible value out of the wrong
 * register, so the mapping is written once and used everywhere.
 */
constexpr std::uint64_t context::* const encoded_registers[16] = {
    &context::rax,
    &context::rcx,
    &context::rdx,
    &context::rbx,
    &context::rsp,
    &context::rbp,
    &context::rsi,
    &context::rdi,
    &context::r8,
    &context::r9,
    &context::r10,
    &context::r11,
    &context::r12,
    &context::r13,
    &context::r14,
    &context::r15,
};

constexpr std::uint64_t mask_for(std::uint8_t size)
{
    switch (size) {
    case 1:
        return 0xffull;
    case 2:
        return 0xffffull;
    case 4:
        return 0xffffffffull;
    default:
        return ~std::uint64_t{};
    }
}

constexpr std::uint64_t truncate(std::uint64_t value, std::uint8_t size)
{
    return value & mask_for(size);
}

constexpr std::uint64_t sign_extend(std::uint64_t value, std::uint8_t size)
{
    auto bits = static_cast<unsigned>(size) * 8u;
    if (64u <= bits) {
        return value;
    }

    auto sign = std::uint64_t{1} << (bits - 1u);
    auto truncated = truncate(value, size);
    return (0 != (truncated & sign)) ? (truncated | ~mask_for(size))
                                     : truncated;
}

/**
 * A cursor over the instruction's bytes.
 *
 * Every read is bounds checked and a read past the end poisons the cursor
 * rather than returning something arbitrary, so a truncated instruction
 * fails once at the end instead of at each field.
 */
class cursor
{
public:
    constexpr explicit cursor(std::span<const std::byte> code) :
        m_code(code)
    {
    }

    constexpr std::uint8_t next()
    {
        if (m_at >= m_code.size()) {
            m_overran = true;
            return 0;
        }

        return static_cast<std::uint8_t>(m_code[m_at++]);
    }

    constexpr std::uint8_t peek() const
    {
        if (m_at >= m_code.size()) {
            return 0;
        }

        return static_cast<std::uint8_t>(m_code[m_at]);
    }

    constexpr std::uint64_t next_immediate(std::uint8_t bytes)
    {
        std::uint64_t value{};
        for (std::uint8_t i{}; i < bytes; ++i) {
            value |= std::uint64_t{next()} << (i * 8u);
        }
        return value;
    }

    constexpr void skip(std::size_t bytes)
    {
        m_at += bytes;
        if (m_at > m_code.size()) {
            m_overran = true;
        }
    }

    constexpr std::size_t taken() const
    {
        return m_at;
    }

    constexpr bool overran() const
    {
        return m_overran;
    }

private:
    std::span<const std::byte> m_code;
    std::size_t m_at{};
    bool m_overran{};
};

/**
 * The prefixes that change what is being decoded, as opposed to the ones
 * that only change how it executes.
 */
struct prefixes
{
    bool operand_size{};
    std::uint8_t rex{};

    constexpr bool wide() const
    {
        return 0 != (rex & 0x8);
    }

    constexpr std::uint8_t extend_reg() const
    {
        return static_cast<std::uint8_t>((rex & 0x4) << 1);
    }
};

constexpr bool is_ignorable_prefix(std::uint8_t byte)
{
    // Address size, the segment overrides, lock and the repeat prefixes.
    // None of them changes the operand this decoder reports: an address
    // is never computed here, and lock does not alter the value written.
    return (0x67 == byte) || (0x2e == byte) || (0x36 == byte) ||
           (0x3e == byte) || (0x26 == byte) || (0x64 == byte) ||
           (0x65 == byte) || (0xf0 == byte) || (0xf2 == byte) ||
           (0xf3 == byte);
}

constexpr prefixes read_prefixes(cursor & code)
{
    prefixes found{};

    for (;;) {
        auto byte = code.peek();

        if (0x66 == byte) {
            found.operand_size = true;
            code.next();
            continue;
        }

        if (is_ignorable_prefix(byte)) {
            code.next();
            continue;
        }

        // REX must be the last prefix before the opcode, so recording it
        // also ends the search.
        if ((0x40 <= byte) && (byte <= 0x4f)) {
            found.rex = byte;
            code.next();
        }

        return found;
    }
}

/**
 * The ModRM byte, and the addressing bytes after it stepped over.
 *
 * Only `mod` and `reg` are interpreted. `rm` decides how many bytes of
 * addressing follow, which is all this needs it for - the address itself
 * comes from the VMCS.
 */
struct modrm
{
    std::uint8_t mod{};
    std::uint8_t reg{};
    std::uint8_t rm{};

    constexpr bool names_register() const
    {
        return 0x3 == mod;
    }
};

constexpr modrm read_modrm(cursor & code)
{
    auto byte = code.next();

    modrm found{
        .mod = static_cast<std::uint8_t>(byte >> 6),
        .reg = static_cast<std::uint8_t>((byte >> 3) & 0x7),
        .rm = static_cast<std::uint8_t>(byte & 0x7),
    };

    if (found.names_register()) {
        return found;
    }

    if (0x4 == found.rm) {
        // A scale-index-base byte follows. Its base of five with mod zero
        // means a 32-bit displacement instead of a base register.
        auto sib = code.next();
        if ((0 == found.mod) && (0x5 == (sib & 0x7))) {
            code.skip(4);
        }
    } else if ((0 == found.mod) && (0x5 == found.rm)) {
        // RIP relative, which carries a 32-bit displacement.
        code.skip(4);
    }

    if (1 == found.mod) {
        code.skip(1);
    } else if (2 == found.mod) {
        code.skip(4);
    }

    return found;
}

constexpr std::uint8_t width_of(const prefixes & found, bool byte_form)
{
    if (byte_form) {
        return 1;
    }

    // REX.W wins over the operand-size prefix, which is what makes a
    // 64-bit access 64-bit even with 0x66 present.
    if (found.wide()) {
        return 8;
    }

    return found.operand_size ? std::uint8_t{2} : std::uint8_t{4};
}

/**
 * How many bytes of immediate an instruction of this width carries.
 *
 * Never eight: there is no 64-bit immediate form of any instruction that
 * takes a memory operand. The 64-bit forms carry 32 bits, sign extended.
 */
constexpr std::uint8_t immediate_width(std::uint8_t size)
{
    if (1 == size) {
        return 1;
    }

    return (2 == size) ? std::uint8_t{2} : std::uint8_t{4};
}

/**
 * Whether an 8-bit register encoding without REX names a high byte.
 *
 * Encodings four to seven mean AH, CH, DH and BH there, which are halves
 * of registers rather than registers - indexing the table would silently
 * read RSP, RBP, RSI or RDI instead. Refused rather than mistaken.
 */
constexpr bool names_high_byte(std::uint8_t size,
                               const prefixes & found,
                               std::uint8_t reg)
{
    return (1 == size) && (0 == found.rex) && (reg >= 4);
}

/**
 * The stack pointer, which must never be read out of the context handed
 * to this decoder.
 *
 * That field holds the *host* stack pointer: the exit stub stores the
 * address of the context structure there, because restoring it iretqs onto
 * that stack. Reading it would put a hypervisor address into whatever the
 * guest was writing, which both corrupts the write and hands a protected
 * address to the guest. Encoding four with REX.B is R12 and is unaffected.
 */
constexpr bool names_host_stack_pointer(std::uint8_t index)
{
    return 4 == index;
}

/**
 * The group-one operations, indexed by the ModRM `reg` field, for the
 * opcodes that carry their operation there rather than in the opcode.
 */
constexpr combine_with group_one_operation(std::uint8_t reg)
{
    switch (reg) {
    case 0:
        return combine_with::add;
    case 1:
        return combine_with::bitwise_or;
    case 4:
        return combine_with::bitwise_and;
    case 5:
        return combine_with::subtract;
    case 6:
        return combine_with::bitwise_xor;
    default:
        // Two and three are add-with-carry and subtract-with-borrow, which
        // need a flag this decoder does not carry, and seven is a compare
        // handled as an examine by its own opcode. Refused rather than
        // approximated.
        return combine_with::none;
    }
}

/**
 * The bit operations of the two-byte group, likewise indexed by `reg`.
 */
constexpr combine_with bit_operation(std::uint8_t reg)
{
    switch (reg) {
    case 5:
        return combine_with::set_bit;
    case 6:
        return combine_with::clear_bit;
    case 7:
        return combine_with::flip_bit;
    default:
        // Four is a plain bit test, which changes nothing and is reported
        // as an examine by the caller below.
        return combine_with::none;
    }
}

} // namespace detail

/**
 * The register a decoded instruction writes its result to.
 *
 * Spelled as a function so the encoding order lives in one place, and
 * returning a pointer-to-member so a caller cannot accidentally index the
 * context by the encoding.
 */
constexpr std::uint64_t context::* register_of(std::uint8_t index)
{
    return detail::encoded_registers[index & 0xf];
}

/**
 * Decodes the instruction at `code`, if it is one that touches memory in a
 * way this can carry out.
 *
 * Refusing is always safe: the caller's fallback is to let the guest's own
 * instruction run against a briefly writable page. Refusing costs the
 * caller the ability to *observe* the access, which is why the coverage
 * matters even though a refusal is never wrong.
 *
 * @param code The instruction bytes, from the first prefix.
 * @param registers The guest's registers as of the faulting instruction.
 */
constexpr std::optional<decoded_instruction>
decode(std::span<const std::byte> code, const context & registers)
{
    detail::cursor at{code};

    auto found = detail::read_prefixes(at);
    auto opcode = at.next();

    // The two-byte escape, which carries the bit operations and the
    // widening moves.
    auto two_byte = (0x0f == opcode);
    if (two_byte) {
        opcode = at.next();
    }

    auto operand_of = [&](std::uint8_t index,
                          std::uint8_t size) -> std::uint64_t {
        return detail::truncate(registers.*register_of(index), size);
    };

    decoded_instruction result{};

    if (!two_byte) {
        switch (opcode) {
        // MOV to memory, from a register or an immediate. The even opcode
        // of each pair is the byte form.
        case 0x88:
        case 0x89:
        case 0xc6:
        case 0xc7: {
            auto byte_form = (0x88 == opcode) || (0xc6 == opcode);
            auto size = detail::width_of(found, byte_form);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            result.what = memory_operation::store;
            result.size = size;

            if ((0x88 == opcode) || (0x89 == opcode)) {
                if (detail::names_high_byte(size, found, fields.reg)) {
                    return {};
                }

                auto index = static_cast<std::uint8_t>(
                    fields.reg | found.extend_reg());

                if (detail::names_host_stack_pointer(index)) {
                    return {};
                }

                result.operand = operand_of(index, size);
                break;
            }

            auto immediate = detail::immediate_width(size);
            auto value = at.next_immediate(immediate);

            // The 64-bit form carries 32 bits, sign extended, so a
            // negative immediate stores what the instruction means rather
            // than its low half.
            result.operand = detail::truncate(
                (8 == size) ? detail::sign_extend(value, 4) : value, size);
            break;
        }

        // MOV from memory into a register.
        case 0x8a:
        case 0x8b: {
            auto size = detail::width_of(found, 0x8a == opcode);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            if (detail::names_high_byte(size, found, fields.reg)) {
                return {};
            }

            result.what = memory_operation::load;
            result.size = size;
            result.destination = static_cast<std::uint8_t>(
                fields.reg | found.extend_reg());
            result.writes_register = true;
            break;
        }

        // The read-modify-write group with a register operand. Each pair
        // is byte form then wide form, and the memory operand is the
        // destination only for the even-numbered opcode of each group -
        // the odd ones with direction set read memory instead, which is
        // the 0x02-style encoding and is not offered here.
        case 0x00:
        case 0x01:
        case 0x08:
        case 0x09:
        case 0x20:
        case 0x21:
        case 0x28:
        case 0x29:
        case 0x30:
        case 0x31: {
            auto byte_form = (0 == (opcode & 1));
            auto size = detail::width_of(found, byte_form);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            if (detail::names_high_byte(size, found, fields.reg)) {
                return {};
            }

            auto index = static_cast<std::uint8_t>(fields.reg |
                                                   found.extend_reg());
            if (detail::names_host_stack_pointer(index)) {
                return {};
            }

            result.what = memory_operation::combine;
            result.size = size;
            result.operand = operand_of(index, size);

            switch (opcode & 0xf8) {
            case 0x00:
                result.how = combine_with::add;
                break;
            case 0x08:
                result.how = combine_with::bitwise_or;
                break;
            case 0x20:
                result.how = combine_with::bitwise_and;
                break;
            case 0x28:
                result.how = combine_with::subtract;
                break;
            default:
                result.how = combine_with::bitwise_xor;
                break;
            }
            break;
        }

        // The same group with an immediate, where the operation is in the
        // ModRM register field. 0x83 carries one sign-extended byte
        // whatever the operand width.
        case 0x80:
        case 0x81:
        case 0x83: {
            auto size = detail::width_of(found, 0x80 == opcode);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            auto immediate = (0x83 == opcode)
                                 ? std::uint8_t{1}
                                 : detail::immediate_width(size);
            auto value = at.next_immediate(immediate);

            if ((0x83 == opcode) || (8 == size)) {
                value = detail::sign_extend(
                    value, (0x83 == opcode) ? std::uint8_t{1}
                                            : std::uint8_t{4});
            }

            result.size = size;
            result.operand = detail::truncate(value, size);

            // Seven is a compare, which leaves memory alone.
            if (7 == fields.reg) {
                result.what = memory_operation::examine;
                break;
            }

            auto how = detail::group_one_operation(fields.reg);
            if (combine_with::none == how) {
                return {};
            }

            result.what = memory_operation::combine;
            result.how = how;
            break;
        }

        // TEST against a register, and against an immediate. Neither
        // changes memory; both are worth decoding so that a watch sees the
        // access rather than falling back to stepping.
        case 0x84:
        case 0x85: {
            auto size = detail::width_of(found, 0x84 == opcode);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            result.what = memory_operation::examine;
            result.size = size;
            break;
        }

        case 0xf6:
        case 0xf7: {
            auto size = detail::width_of(found, 0xf6 == opcode);
            auto fields = detail::read_modrm(at);

            if (fields.names_register() || (0 != fields.reg)) {
                // Only the test form. The others in this group - not, neg,
                // mul, div - either need flags or are not worth the
                // surface.
                return {};
            }

            at.skip(detail::immediate_width(size));

            result.what = memory_operation::examine;
            result.size = size;
            break;
        }

        // XCHG with memory, which is where a lock-free updater goes.
        case 0x86:
        case 0x87: {
            auto size = detail::width_of(found, 0x86 == opcode);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            if (detail::names_high_byte(size, found, fields.reg)) {
                return {};
            }

            auto index = static_cast<std::uint8_t>(fields.reg |
                                                   found.extend_reg());
            if (detail::names_host_stack_pointer(index)) {
                return {};
            }

            result.what = memory_operation::exchange;
            result.size = size;
            result.operand = operand_of(index, size);
            result.destination = index;
            result.writes_register = true;
            break;
        }

        default:
            return {};
        }
    } else {
        switch (opcode) {
        // The widening moves, which read memory narrower than the register
        // they fill. The only difference between them is the extension,
        // and stating it in the result is what keeps that difference from
        // living in two places.
        case 0xb6:
        case 0xb7:
        case 0xbe:
        case 0xbf: {
            auto narrow = ((0xb6 == opcode) || (0xbe == opcode))
                              ? std::uint8_t{1}
                              : std::uint8_t{2};
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            result.what = memory_operation::load;
            result.size = narrow;
            result.destination = static_cast<std::uint8_t>(
                fields.reg | found.extend_reg());
            result.writes_register = true;
            result.sign_extends = (0xbe == opcode) || (0xbf == opcode);
            break;
        }

        // The bit group with an immediate bit number: test, and the three
        // that change the bit.
        case 0xba: {
            auto size = detail::width_of(found, false);
            auto fields = detail::read_modrm(at);

            if (fields.names_register()) {
                return {};
            }

            auto bit = at.next_immediate(1);

            result.size = size;

            // The bit number is taken modulo the operand width for the
            // immediate form - SDM, BT: "the offset is taken modulo the
            // operand size".
            result.operand = bit & ((static_cast<std::uint64_t>(size) * 8u) -
                                    1u);

            if (4 == fields.reg) {
                result.what = memory_operation::examine;
                break;
            }

            auto how = detail::bit_operation(fields.reg);
            if (combine_with::none == how) {
                return {};
            }

            result.what = memory_operation::combine;
            result.how = how;
            break;
        }

        default:
            return {};
        }
    }

    if (at.overran()) {
        return {};
    }

    result.length = static_cast<std::uint8_t>(at.taken());
    return result;
}

/**
 * What the memory should hold after the instruction runs.
 *
 * `current` is what it holds now, which only the combining and exchanging
 * forms look at. A form that leaves memory alone returns `current`
 * unchanged, so a caller may write the result back unconditionally.
 */
constexpr std::uint64_t apply(const decoded_instruction & instruction,
                              std::uint64_t current)
{
    auto size = instruction.size;
    auto old = detail::truncate(current, size);
    auto operand = detail::truncate(instruction.operand, size);

    switch (instruction.what) {
    case memory_operation::store:
    case memory_operation::exchange:
        return operand;

    case memory_operation::load:
    case memory_operation::examine:
        return old;

    case memory_operation::combine:
        break;
    }

    switch (instruction.how) {
    case combine_with::bitwise_and:
        return old & operand;
    case combine_with::bitwise_or:
        return old | operand;
    case combine_with::bitwise_xor:
        return old ^ operand;
    case combine_with::add:
        return detail::truncate(old + operand, size);
    case combine_with::subtract:
        return detail::truncate(old - operand, size);
    case combine_with::set_bit:
        return old | (std::uint64_t{1} << operand);
    case combine_with::clear_bit:
        return old & ~(std::uint64_t{1} << operand);
    case combine_with::flip_bit:
        return old ^ (std::uint64_t{1} << operand);
    case combine_with::none:
        break;
    }

    return old;
}

/**
 * What the instruction leaves in the register it writes, given what memory
 * held. Meaningful only where `writes_register` is set.
 *
 * The result is a whole 64-bit register value, so a caller assigns rather
 * than merges - except for the narrow forms, where the architecture
 * preserves the upper bits and this preserves them too, which is why the
 * previous value is a parameter.
 */
constexpr std::uint64_t
result_for_register(const decoded_instruction & instruction,
                    std::uint64_t current_memory,
                    std::uint64_t current_register)
{
    auto size = instruction.size;
    auto value = detail::truncate(current_memory, size);

    if (instruction.sign_extends) {
        // The destination width, not the source width, is what the
        // extension fills - and it is the whole register here, since the
        // widening forms with a 16-bit destination are refused above.
        return detail::sign_extend(value, size);
    }

    // A 4-byte or 8-byte result clears the rest of the register; a 1-byte
    // or 2-byte one leaves it alone. That asymmetry is the architecture's
    // and is easy to get wrong in the caller, which is why it is here.
    if (size >= 4) {
        return value;
    }

    return (current_register & ~detail::mask_for(size)) | value;
}

} // namespace zpp::arch::x86_64
