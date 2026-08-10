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
 * The addressing bytes were once walked only to find where the
 * instruction ends, on the grounds that the hardware reports the faulting
 * address. It does not always: an EPT violation reports a guest-linear
 * address only when exit qualification bit 7 is set, and the
 * guest-physical address beside it is page granular, so with that bit
 * clear the exit says which page was touched and nothing about where in
 * it. So the addressing is now interpreted as well as walked, and
 * `effective_address` computes what the instruction named.
 *
 * Still deliberately absent: segmentation. A segment override is recorded
 * and then refused, because FS and GS carry bases that are not page
 * aligned and live in an MSR rather than in the instruction.
 */

/**
 * The size of the code the instruction was fetched from, which decides
 * what its bytes mean.
 *
 * Not an optimisation and not diagnostic: the same bytes are different
 * instructions in each of these. In sixteen-bit code the default operand
 * size is two rather than four, so the 0x66 prefix means the *opposite* of
 * what it means here; and `mod=00, rm=110` is a sixteen-bit displacement
 * with no scale-index-base byte, so the instruction ends somewhere else.
 * Measured on `66 89 06 00 03` - `mov [0x0300], ax`, five bytes - which a
 * long-mode decoder reads as three, and a caller that trusted the answer
 * would resume the guest two bytes into the middle of its own instruction.
 *
 * Taken from the guest's own code segment rather than assumed, because a
 * processor coming out of a start-up IPI runs in real mode and this VMM
 * enables unrestricted guest, so sixteen-bit guest code is not a
 * hypothetical here.
 */
enum class code_size : std::uint8_t
{
    /**
     * Real mode, virtual-8086 mode, or a protected-mode code segment with
     * D/B clear. Refused rather than decoded - see `decode`.
     */
    bits_16,
    bits_32,
    bits_64,
};

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
 * Where an instruction's memory operand is, as the encoding names it.
 *
 * The pieces rather than the address, because the address needs the
 * guest's RIP and the instruction's own length, and neither is a property
 * of the ModRM byte. `effective_address` puts them together.
 *
 * This exists because the address the hardware reports is not always
 * usable. An EPT violation reports a guest-linear address only when
 * qualification bit 7 is set, and the guest-physical address it reports
 * alongside is **page granular** - so when that bit is clear there is no
 * offset within the page to be had from the exit at all. Measured: under
 * a hypervisor that reflects EPT violations to this one, twenty-four
 * consecutive accesses to the local APIC page arrived with bit 7 clear,
 * and every one of them looked like a write to offset zero.
 */
struct memory_addressing
{
    /**
     * Whether the fields below were filled in. Clear for an instruction
     * whose operand is a register, and for one this decoder refused.
     */
    bool known{};

    /**
     * Whether a segment override prefix was present.
     *
     * Recorded and never applied. In 64-bit code FS and GS have bases
     * that are not zero and not page aligned, so an override moves the
     * offset within the page as well as the address - and the base lives
     * in an MSR rather than in the instruction. A caller wanting the
     * address must refuse rather than compute one that is quietly wrong.
     */
    bool segment_override{};

    /**
     * Whether the address is 64 bits wide. Clear in 32-bit code, and in
     * 64-bit code carrying the 0x67 prefix, both of which truncate the
     * whole computation rather than any one term of it.
     */
    bool wide{};

    bool has_base{};
    bool has_index{};

    /**
     * Whether the displacement is relative to the end of the instruction
     * rather than absolute. No base and no index in that case.
     */
    bool rip_relative{};

    /**
     * Base and index in *encoding* order, so `register_of` maps them.
     */
    std::uint8_t base{};
    std::uint8_t index{};

    /**
     * One, two, four or eight. Meaningless unless `has_index`.
     */
    std::uint8_t scale{1};

    /**
     * Already sign extended from whatever width the encoding carried.
     */
    std::int64_t displacement{};
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
     * Width of the register a result goes to, which is **not** always the
     * width of the memory access.
     *
     * The widening moves are the whole reason this is separate: `movzx
     * eax, byte [mem]` reads one byte and fills four. Using the memory
     * width for both produced a register holding the old upper bits with a
     * byte pasted in - measured as `ffffffffffffff01` where the
     * architecture gives `0000000000000001`. Zero means "same as the
     * access", which is every form except those.
     */
    std::uint8_t destination_size{};

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

    /**
     * Which kind of examination this is, for the forms that change nothing
     * but the flags. A compare subtracts; a test ands; a bit test reports
     * one bit in the carry flag. They are distinguished here rather than
     * by the caller because only the flags tell them apart.
     * @{
     */
    bool compares{};
    bool tests_bit{};
    /**
     * @}
     */

    /**
     * Where the memory operand is. See `memory_addressing` for why this
     * is answered at all, given that the hardware reports an address.
     */
    memory_addressing where{};
};

namespace instruction_detail
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

    /**
     * Whether 0x67 was present. Recorded rather than ignored, because what
     * it does depends on the code size: in 64-bit code it selects 32-bit
     * addressing, which leaves the ModRM and scale-index-base bytes
     * exactly as they are, and in 32-bit code it selects *16-bit*
     * addressing, which does not. Only the first is ignorable.
     */
    bool address_size{};

    /**
     * Whether any segment override was present. Recorded for the address
     * computation only - see `memory_addressing::segment_override`.
     */
    bool segment_override{};

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

constexpr bool is_segment_override(std::uint8_t byte)
{
    return (0x2e == byte) || (0x36 == byte) || (0x3e == byte) ||
           (0x26 == byte) || (0x64 == byte) || (0x65 == byte);
}

constexpr bool is_ignorable_prefix(std::uint8_t byte)
{
    // The segment overrides, lock and the repeat prefixes. None of them
    // changes the operand this decoder reports or where the instruction
    // ends: an address is never computed here, and lock does not alter the
    // value written.
    //
    // The address-size prefix is deliberately *not* in this list any more.
    // It is recorded instead, because whether it can be ignored depends on
    // the code size - see `prefixes::address_size`.
    return (0x2e == byte) || (0x36 == byte) || (0x3e == byte) ||
           (0x26 == byte) || (0x64 == byte) || (0x65 == byte) ||
           (0xf0 == byte) || (0xf2 == byte) || (0xf3 == byte);
}

constexpr prefixes read_prefixes(cursor & code, code_size size)
{
    prefixes found{};

    for (;;) {
        auto byte = code.peek();

        if (0x66 == byte) {
            found.operand_size = true;
            code.next();
            continue;
        }

        if (0x67 == byte) {
            found.address_size = true;
            code.next();
            continue;
        }

        if (is_ignorable_prefix(byte)) {
            found.segment_override =
                found.segment_override || is_segment_override(byte);
            code.next();
            continue;
        }

        // REX must be the last prefix before the opcode, so recording it
        // also ends the search.
        //
        // Only in 64-bit code. Outside it, 0x40 to 0x4f are the one-byte
        // INC and DEC forms and are opcodes, not prefixes - consuming one
        // as a prefix would read the byte after it as the opcode and
        // decode an unrelated instruction. Unreachable in practice, since
        // a register INC touches no memory and so cannot fault a watched
        // page, but "unreachable" is not a property of this function and
        // this is a compile-time constant test.
        if ((code_size::bits_64 == size) && (0x40 <= byte) &&
            (byte <= 0x4f)) {
            found.rex = byte;
            code.next();
        }

        return found;
    }
}

/**
 * The ModRM byte, and the addressing bytes after it read out.
 *
 * `mod` and `reg` say what the operation is; `rm` and what follows say
 * where its memory operand is. The addressing used to be stepped over
 * rather than interpreted, on the grounds that the hardware reports the
 * address - it does not always, which is what `memory_addressing`
 * records.
 */
struct modrm
{
    std::uint8_t mod{};
    std::uint8_t reg{};
    std::uint8_t rm{};

    memory_addressing where{};

    constexpr bool names_register() const
    {
        return 0x3 == mod;
    }
};

constexpr modrm read_modrm(cursor & code,
                           const prefixes & found_prefixes,
                           code_size mode)
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

    // REX.B extends the base and REX.X the index, each by eight. Both are
    // zero outside 64-bit code, where `read_prefixes` never records a REX
    // byte in the first place.
    auto extend_base =
        static_cast<std::uint8_t>((found_prefixes.rex & 0x1) << 3);
    auto extend_index =
        static_cast<std::uint8_t>((found_prefixes.rex & 0x2) << 2);

    found.where.known = true;
    found.where.segment_override = found_prefixes.segment_override;
    found.where.wide =
        (code_size::bits_64 == mode) && !found_prefixes.address_size;

    // How wide the displacement is, decided before it is read, because
    // the scale-index-base byte can add one where the mod field alone
    // would say there is none.
    std::uint8_t displacement_width{};
    if (1 == found.mod) {
        displacement_width = 1;
    } else if (2 == found.mod) {
        displacement_width = 4;
    }

    if (0x4 == found.rm) {
        // A scale-index-base byte follows. Its base of five with mod zero
        // means a 32-bit displacement instead of a base register.
        auto sib = code.next();

        auto index =
            static_cast<std::uint8_t>(((sib >> 3) & 0x7) | extend_index);

        // Encoding four with REX.X clear is the encoding for *no* index,
        // not for RSP - there is no scaled RSP. With REX.X it is R12 and
        // is an ordinary index.
        found.where.has_index = (4 != index);
        found.where.index = index;
        found.where.scale = static_cast<std::uint8_t>(1u << (sib >> 6));

        if ((0 == found.mod) && (0x5 == (sib & 0x7))) {
            displacement_width = 4;
        } else {
            found.where.has_base = true;
            found.where.base =
                static_cast<std::uint8_t>((sib & 0x7) | extend_base);
        }
    } else if ((0 == found.mod) && (0x5 == found.rm)) {
        // A 32-bit displacement with no base. In 64-bit code it is
        // relative to the end of the instruction; in 32-bit code the same
        // encoding is an absolute address.
        displacement_width = 4;
        found.where.rip_relative = (code_size::bits_64 == mode);
    } else {
        found.where.has_base = true;
        found.where.base =
            static_cast<std::uint8_t>(found.rm | extend_base);
    }

    if (0 != displacement_width) {
        found.where.displacement = static_cast<std::int64_t>(sign_extend(
            code.next_immediate(displacement_width), displacement_width));
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

} // namespace instruction_detail

/**
 * The register a decoded instruction writes its result to.
 *
 * Spelled as a function so the encoding order lives in one place, and
 * returning a pointer-to-member so a caller cannot accidentally index the
 * context by the encoding.
 */
constexpr std::uint64_t context::* register_of(std::uint8_t index)
{
    return instruction_detail::encoded_registers[index & 0xf];
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
 * Sixteen-bit code is refused outright rather than decoded. Every operand
 * width and every instruction length below assumes a default operand size
 * of four and long-mode addressing, and neither holds there - so the
 * answer would not merely be incomplete, it would be wrong, and the caller
 * would advance the guest's instruction pointer into the middle of an
 * instruction. See `code_size` for the measurement.
 *
 * @param code The instruction bytes, from the first prefix.
 * @param registers The guest's registers as of the faulting instruction.
 * @param mode The size of the guest's current code segment. Passed in
 *             rather than assumed, because there is nothing in the bytes
 *             that says which it is.
 */
constexpr std::optional<decoded_instruction>
decode(std::span<const std::byte> code,
       const context & registers,
       code_size mode)
{
    if (code_size::bits_16 == mode) {
        return {};
    }

    instruction_detail::cursor at{code};

    auto found = instruction_detail::read_prefixes(at, mode);

    // An address-size prefix on 32-bit code selects 16-bit addressing, and
    // then `mod=00, rm=110` is a 16-bit displacement and there is no
    // scale-index-base byte at all - so every length below is wrong by two
    // or by four. Refused for the same reason 16-bit code is. In 64-bit
    // code the prefix selects 32-bit addressing, which changes nothing
    // this decoder looks at, and is ignored as it always was.
    if (found.address_size && (code_size::bits_32 == mode)) {
        return {};
    }

    auto opcode = at.next();

    // The two-byte escape, which carries the bit operations and the
    // widening moves.
    auto two_byte = (0x0f == opcode);
    if (two_byte) {
        opcode = at.next();
    }

    auto operand_of = [&](std::uint8_t index,
                          std::uint8_t size) -> std::uint64_t {
        return instruction_detail::truncate(registers.*register_of(index),
                                            size);
    };

    decoded_instruction result{};

    // Declared out here rather than in each case, because every form that
    // reaches the end has a memory operand and the address is stamped on
    // the result once, below, instead of nine times.
    instruction_detail::modrm fields{};

    if (!two_byte) {
        switch (opcode) {
        // MOV to memory, from a register or an immediate. The even opcode
        // of each pair is the byte form.
        case 0x88:
        case 0x89:
        case 0xc6:
        case 0xc7: {
            auto byte_form = (0x88 == opcode) || (0xc6 == opcode);
            auto size = instruction_detail::width_of(found, byte_form);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            // C6 and C7 are group 11, which defines only `/0`. With a
            // memory operand every other value of the ModRM `reg` field
            // is an invalid opcode on hardware, so emulating one as a MOV
            // store performs a write the guest never asked for and then
            // advances RIP past an instruction that should have raised
            // #UD. The `reg` field is not part of the encoding for the
            // two MOV opcodes beside them, which is why this is only
            // asked of C6 and C7.
            if (((0xc6 == opcode) || (0xc7 == opcode)) &&
                (0 != fields.reg)) {
                return {};
            }

            result.what = memory_operation::store;
            result.size = size;

            if ((0x88 == opcode) || (0x89 == opcode)) {
                if (instruction_detail::names_high_byte(
                        size, found, fields.reg)) {
                    return {};
                }

                auto index = static_cast<std::uint8_t>(fields.reg |
                                                       found.extend_reg());

                if (instruction_detail::names_host_stack_pointer(index)) {
                    return {};
                }

                result.operand = operand_of(index, size);
                break;
            }

            auto immediate = instruction_detail::immediate_width(size);
            auto value = at.next_immediate(immediate);

            // The 64-bit form carries 32 bits, sign extended, so a
            // negative immediate stores what the instruction means rather
            // than its low half.
            result.operand = instruction_detail::truncate(
                (8 == size) ? instruction_detail::sign_extend(value, 4)
                            : value,
                size);
            break;
        }

        // MOV from memory into a register.
        case 0x8a:
        case 0x8b: {
            auto size =
                instruction_detail::width_of(found, 0x8a == opcode);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            if (instruction_detail::names_high_byte(
                    size, found, fields.reg)) {
                return {};
            }

            result.what = memory_operation::load;
            result.size = size;
            result.destination =
                static_cast<std::uint8_t>(fields.reg | found.extend_reg());

            // The same guard the stores carry, and for a worse reason.
            // Reading that field hands a host address to the guest;
            // *writing* it pivots the host stack, because the exit stub
            // stores the context's address there and restore_context pops
            // it and iretqs onto whatever it holds. So an emulated
            // `mov rsp, [watched]` would resume the host on an address the
            // guest chose.
            if (instruction_detail::names_host_stack_pointer(
                    result.destination)) {
                return {};
            }

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
            auto size = instruction_detail::width_of(found, byte_form);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            if (instruction_detail::names_high_byte(
                    size, found, fields.reg)) {
                return {};
            }

            auto index =
                static_cast<std::uint8_t>(fields.reg | found.extend_reg());
            if (instruction_detail::names_host_stack_pointer(index)) {
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
            auto size =
                instruction_detail::width_of(found, 0x80 == opcode);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            auto immediate =
                (0x83 == opcode)
                    ? std::uint8_t{1}
                    : instruction_detail::immediate_width(size);
            auto value = at.next_immediate(immediate);

            if ((0x83 == opcode) || (8 == size)) {
                value = instruction_detail::sign_extend(
                    value,
                    (0x83 == opcode) ? std::uint8_t{1} : std::uint8_t{4});
            }

            result.size = size;
            result.operand = instruction_detail::truncate(value, size);

            // Seven is a compare, which leaves memory alone.
            if (7 == fields.reg) {
                result.what = memory_operation::examine;
                result.compares = true;
                break;
            }

            auto how = instruction_detail::group_one_operation(fields.reg);
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
            auto size =
                instruction_detail::width_of(found, 0x84 == opcode);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            auto index =
                static_cast<std::uint8_t>(fields.reg | found.extend_reg());

            // Both guards, which this case was missing entirely. The high
            // byte registers alias differently without REX, and encoding
            // four is the *host* stack pointer - reading it computes the
            // flags against a hypervisor address and leaves it in the
            // guest's RFLAGS.
            if (instruction_detail::names_high_byte(
                    size, found, fields.reg) ||
                instruction_detail::names_host_stack_pointer(index)) {
                return {};
            }

            // TEST ands its operands without storing the result.
            result.what = memory_operation::examine;
            result.size = size;
            result.operand = operand_of(index, size);
            break;
        }

        case 0xf6:
        case 0xf7: {
            auto size =
                instruction_detail::width_of(found, 0xf6 == opcode);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register() || (0 != fields.reg)) {
                // Only the test form. The others in this group - not, neg,
                // mul, div - either need flags or are not worth the
                // surface.
                return {};
            }

            auto value = at.next_immediate(
                instruction_detail::immediate_width(size));

            // Sign extended for the 64-bit form, as the group-one path
            // above does and for the same reason: `immediate_width(8)` is
            // 4, so `test qword [m], -1` encodes 0xffffffff and must
            // become 0xffffffffffffffff before the AND. Without this it
            // was compared against 0x00000000ffffffff, which sets ZF for
            // any value whose only bits are in the high half - and the
            // guest branches on that flag immediately.
            if (8 == size) {
                value = instruction_detail::sign_extend(value, 4);
            }

            result.what = memory_operation::examine;
            result.size = size;
            result.operand = instruction_detail::truncate(value, size);
            break;
        }

        // XCHG with memory, which is where a lock-free updater goes.
        case 0x86:
        case 0x87: {
            auto size =
                instruction_detail::width_of(found, 0x86 == opcode);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            if (instruction_detail::names_high_byte(
                    size, found, fields.reg)) {
                return {};
            }

            auto index =
                static_cast<std::uint8_t>(fields.reg | found.extend_reg());
            if (instruction_detail::names_host_stack_pointer(index)) {
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
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            result.what = memory_operation::load;
            result.size = narrow;

            // The destination is as wide as the operand size says, which
            // for these is what REX.W and the 0x66 prefix decide - the
            // opcode only says how much of memory is read.
            result.destination_size =
                instruction_detail::width_of(found, false);
            result.destination =
                static_cast<std::uint8_t>(fields.reg | found.extend_reg());

            // As for the plain loads above: this writes a register, and
            // encoding four is the host stack pointer.
            if (instruction_detail::names_host_stack_pointer(
                    result.destination)) {
                return {};
            }

            result.writes_register = true;
            result.sign_extends = (0xbe == opcode) || (0xbf == opcode);
            break;
        }

        // The bit group with an immediate bit number: test, and the three
        // that change the bit.
        case 0xba: {
            auto size = instruction_detail::width_of(found, false);
            fields = instruction_detail::read_modrm(at, found, mode);

            if (fields.names_register()) {
                return {};
            }

            auto bit = at.next_immediate(1);

            result.size = size;

            // The bit number is taken modulo the operand width for the
            // immediate form - SDM, BT: "the offset is taken modulo the
            // operand size".
            result.operand =
                bit & ((static_cast<std::uint64_t>(size) * 8u) - 1u);

            if (4 == fields.reg) {
                result.what = memory_operation::examine;
                result.tests_bit = true;
                break;
            }

            auto how = instruction_detail::bit_operation(fields.reg);
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
    result.where = fields.where;
    return result;
}

/**
 * The linear address the instruction's memory operand names.
 *
 * Wanted because the hardware does not always report it. An EPT violation
 * carries a guest-linear address only when exit qualification bit 7 is
 * set, and the guest-physical address beside it is page granular - so
 * where that bit is clear, this is the only thing that says *which*
 * register on a watched page was touched. Measured on the local APIC
 * page: with the address taken from the physical address instead, every
 * access read as offset zero and a guest hypervisor's start-up interrupts
 * were never seen.
 *
 * Refused rather than approximated in three cases, each of which would be
 * wrong rather than merely incomplete:
 *
 *   - a segment override, whose base is not in the instruction,
 *   - a base naming encoding four, since the context handed here holds
 *     the *host's* stack pointer in that slot - the same reason
 *     `names_host_stack_pointer` refuses it as a source operand. The
 *     index cannot name it: encoding four in the index field means no
 *     index at all,
 *   - an instruction whose operand is a register, which has no address.
 *
 * @param instruction As returned by `decode`.
 * @param registers The guest's registers as of the faulting instruction.
 * @param rip The guest's RIP, needed only by the RIP-relative form. It is
 *            the address of the instruction, not of its end - the length
 *            is added here.
 */
constexpr std::optional<std::uint64_t>
effective_address(const decoded_instruction & instruction,
                  const context & registers,
                  std::uint64_t rip)
{
    const auto & where = instruction.where;

    if (!where.known || where.segment_override) {
        return {};
    }

    auto address = static_cast<std::uint64_t>(where.displacement);

    if (where.rip_relative) {
        address += rip + instruction.length;
    } else {
        if (where.has_base) {
            if (instruction_detail::names_host_stack_pointer(where.base)) {
                return {};
            }

            address += registers.*register_of(where.base);
        }

        if (where.has_index) {
            address += registers.*register_of(where.index) * where.scale;
        }
    }

    if (!where.wide) {
        address &= 0xffff'ffffull;
    }

    return address;
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
    auto old = instruction_detail::truncate(current, size);
    auto operand = instruction_detail::truncate(instruction.operand, size);

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
        return instruction_detail::truncate(old + operand, size);
    case combine_with::subtract:
        return instruction_detail::truncate(old - operand, size);
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
 * The status flags an instruction leaves behind.
 *
 * **These are not optional, and leaving them out is how the first attempt
 * at using this decoder killed a guest.** Every operation here except the
 * plain moves and the exchange sets flags, and a guest branches on them
 * immediately: `and [mem], eax` followed by `jz`, `cmp [mem], 1` followed
 * by `jne`. Carrying out the arithmetic and leaving RFLAGS as it was makes
 * the guest take the other branch, which is not a subtle corruption - it
 * was measured as a triple fault after 179 emulated instructions.
 *
 * The narrow decoder this replaces never needed them because it only ever
 * answered MOV, and MOV sets none. Adding the arithmetic made them
 * mandatory in the same change, and that is the trap: the new forms look
 * like more of the same and are not.
 *
 * SDM Vol. 1 3.4.3 for the definitions, and the per-instruction "Flags
 * Affected" sections for which of them each operation touches.
 */
namespace status_flag
{
constexpr std::uint64_t carry = 1ull << 0;
constexpr std::uint64_t parity = 1ull << 2;
constexpr std::uint64_t adjust = 1ull << 4;
constexpr std::uint64_t zero = 1ull << 6;
constexpr std::uint64_t sign = 1ull << 7;
constexpr std::uint64_t overflow = 1ull << 11;

constexpr std::uint64_t arithmetic =
    carry | parity | adjust | zero | sign | overflow;
} // namespace status_flag

namespace instruction_detail
{
constexpr bool parity_of(std::uint64_t result)
{
    // Even parity of the low byte only, which is what the architecture
    // defines however wide the operand is.
    auto low = static_cast<std::uint8_t>(result & 0xff);
    auto ones = 0u;
    for (auto i = 0u; i < 8u; ++i) {
        ones += (low >> i) & 1u;
    }
    return 0 == (ones & 1u);
}

constexpr bool sign_of(std::uint64_t result, std::uint8_t size)
{
    auto bits = static_cast<unsigned>(size) * 8u;
    return 0 != ((result >> (bits - 1u)) & 1u);
}

constexpr std::uint64_t common_flags(std::uint64_t result,
                                     std::uint8_t size)
{
    std::uint64_t flags{};

    if (0 == truncate(result, size)) {
        flags |= status_flag::zero;
    }

    if (sign_of(truncate(result, size), size)) {
        flags |= status_flag::sign;
    }

    if (parity_of(result)) {
        flags |= status_flag::parity;
    }

    return flags;
}

} // namespace instruction_detail

/**
 * The flags after the instruction, given the flags before it, what memory
 * held, and what it now holds.
 *
 * Returns the whole RFLAGS value so a caller assigns rather than merges -
 * merging is where a flag gets left stale, which is the failure this
 * exists to prevent.
 */
constexpr std::uint64_t
flags_after(const decoded_instruction & instruction,
            std::uint64_t before,
            std::uint64_t old_memory,
            std::uint64_t new_memory)
{
    using namespace instruction_detail;

    auto size = instruction.size;
    auto old = truncate(old_memory, size);
    auto operand = truncate(instruction.operand, size);

    switch (instruction.what) {
    case memory_operation::store:
    case memory_operation::load:
    case memory_operation::exchange:
        // MOV, MOVZX, MOVSX and XCHG affect no flags at all.
        return before;

    case memory_operation::combine:
    case memory_operation::examine:
        break;
    }

    // The bit operations set the carry flag from the bit as it was, and
    // the SDM leaves the others undefined - so they are left alone rather
    // than invented.
    switch (instruction.how) {
    case combine_with::set_bit:
    case combine_with::clear_bit:
    case combine_with::flip_bit: {
        auto bit = (old >> operand) & 1u;
        return (before & ~status_flag::carry) |
               (bit ? status_flag::carry : 0);
    }
    default:
        break;
    }

    // A plain bit test, which arrives as an examine with no combining
    // operation, does the same thing.
    if ((memory_operation::examine == instruction.what) &&
        (combine_with::none == instruction.how) && instruction.tests_bit) {
        auto bit = (old >> operand) & 1u;
        return (before & ~status_flag::carry) |
               (bit ? status_flag::carry : 0);
    }

    auto cleared = before & ~status_flag::arithmetic;

    // A compare and a subtract set the same flags; a test and an AND
    // likewise. What differs is only whether the result is written back,
    // which is not this function's business.
    auto subtracting =
        (combine_with::subtract == instruction.how) ||
        ((memory_operation::examine == instruction.what) &&
         (combine_with::none == instruction.how) && instruction.compares);

    if (subtracting) {
        auto result = truncate(old - operand, size);
        auto flags = cleared | common_flags(result, size);

        // Borrow, and signed overflow, both from the operands' signs.
        if (old < operand) {
            flags |= status_flag::carry;
        }

        if (sign_of(old, size) != sign_of(operand, size)) {
            if (sign_of(result, size) != sign_of(old, size)) {
                flags |= status_flag::overflow;
            }
        }

        if ((old & 0xf) < (operand & 0xf)) {
            flags |= status_flag::adjust;
        }

        return flags;
    }

    if (combine_with::add == instruction.how) {
        auto result = truncate(old + operand, size);
        auto flags = cleared | common_flags(result, size);

        if (result < old) {
            flags |= status_flag::carry;
        }

        if (sign_of(old, size) == sign_of(operand, size)) {
            if (sign_of(result, size) != sign_of(old, size)) {
                flags |= status_flag::overflow;
            }
        }

        if (((old & 0xf) + (operand & 0xf)) > 0xf) {
            flags |= status_flag::adjust;
        }

        return flags;
    }

    // The logical operations: carry and overflow cleared, the rest from
    // the result. A test computes the AND it does not store, which is why
    // the result comes from the operands here rather than from memory.
    auto result = (memory_operation::examine == instruction.what)
                      ? truncate(old & operand, size)
                      : truncate(new_memory, size);

    return cleared | common_flags(result, size);
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
    auto read = instruction.size;

    // Where the two differ, the *destination* width decides how much of
    // the register the result occupies, and the access width decides only
    // how much was read. Conflating them is the bug this parameter exists
    // to have prevented.
    auto written = (0 != instruction.destination_size)
                       ? instruction.destination_size
                       : read;

    auto value = instruction_detail::truncate(current_memory, read);

    if (instruction.sign_extends) {
        value = instruction_detail::truncate(
            instruction_detail::sign_extend(value, read), written);
    }

    // A 4-byte or 8-byte result clears the rest of the register; a 1-byte
    // or 2-byte one leaves it alone. That asymmetry is the architecture's
    // and is easy to get wrong in the caller, which is why it is here.
    if (written >= 4) {
        return value;
    }

    return (current_register & ~instruction_detail::mask_for(written)) |
           value;
}

} // namespace zpp::arch::x86_64
