#pragma once
#include "zpp/arch/x86_64/context.h"
#include <cstdint>
#include <optional>
#include <span>

namespace zpp::arch::x86_64
{
/**
 * A store to memory, as decoded from the instruction making it.
 *
 * Only what an EPT violation does not already report. The faulting
 * address comes from the VMCS guest linear and guest physical address
 * fields, and the instruction's length from the VM-exit instruction
 * length field, so neither is decoded here - SDM Table 30-7 lists what a
 * violation supplies, and the data and the operand size are the two
 * things missing from it.
 */
struct memory_store
{
    /**
     * The value written, zero extended to 64 bits. A caller storing it
     * must use exactly `size` bytes of it.
     */
    std::uint64_t value{};

    /**
     * Width of the store in bytes: 1, 2, 4 or 8.
     */
    std::uint8_t size{};

    /**
     * How long the instruction is, in bytes.
     *
     * Reported because the VMCS field that would otherwise answer this is
     * not available for the exit that needs it. SDM 30.2.5 leaves the
     * VM-exit instruction length *undefined* for an EPT violation not
     * encountered during event delivery, and KVM's handle_ept_violation
     * accordingly never reads it - its skip_emulated_instruction carries
     * an explicit warning that the field is not always set.
     *
     * The decoder already knows where the instruction ends, so it says
     * so, and a caller that has a length from anywhere else should refuse
     * on disagreement rather than pick one.
     */
    std::uint8_t length{};
};

namespace detail
{
/**
 * The general purpose registers in *encoding* order.
 *
 * Not the order they are declared in, which is alphabetical-ish and puts
 * rbx where rcx belongs. Getting this wrong reads a plausible value out
 * of the wrong register, which is the sort of mistake that survives
 * testing, so the mapping is written out once and used everywhere.
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

/**
 * Truncates a register's value to an operand size.
 */
constexpr std::uint64_t truncate(std::uint64_t value, std::uint8_t size)
{
    switch (size) {
    case 1:
        return value & 0xffull;
    case 2:
        return value & 0xffffull;
    case 4:
        return value & 0xffffffffull;
    default:
        return value;
    }
}

} // namespace detail

/**
 * Decodes a store to memory, if the instruction at `code` is one this
 * decoder understands.
 *
 * Deliberately small. It handles the MOV forms that store a register or
 * an immediate to memory and refuses everything else, because refusing
 * is safe: the caller keeps its existing path of letting the guest's own
 * instruction perform the write. The point of the decoder is to remove
 * the *window* that path opens, and the driver traffic that window loses
 * is plain MOV - a memory mapped register write compiles to one.
 *
 * Read-modify-write forms (AND, OR, BTS on memory) are refused rather
 * than half handled. They would need the original contents combined with
 * the operand, and getting that subtly wrong writes a wrong value to a
 * device register, which is worse than falling back.
 *
 * @param code The instruction bytes, starting at the first prefix.
 * @param registers The guest's registers as of the faulting instruction.
 *
 * @return The store, or nothing if this is not a MOV to memory.
 */
constexpr std::optional<memory_store> decode_memory_store(
    std::span<const std::byte> code, const context & registers)
{
    std::size_t at{};

    auto byte_at = [&](std::size_t index) -> std::uint8_t {
        return static_cast<std::uint8_t>(code[index]);
    };

    // Prefixes. Only the ones that change what is being decoded are
    // acted on; the rest are stepped over, since a lock or a segment
    // override does not alter the value or the width of the store.
    bool operand_size_override{};
    std::uint8_t rex{};

    for (; at < code.size(); ++at) {
        auto byte = byte_at(at);

        if (0x66 == byte) {
            operand_size_override = true;
            continue;
        }

        // Address size, segment overrides, lock and the repeat
        // prefixes. None of them affect the operand this decoder
        // returns.
        if ((0x67 == byte) || (0x2e == byte) || (0x36 == byte) ||
            (0x3e == byte) || (0x26 == byte) || (0x64 == byte) ||
            (0x65 == byte) || (0xf0 == byte) || (0xf2 == byte) ||
            (0xf3 == byte)) {
            continue;
        }

        // REX must be the last prefix before the opcode, so this both
        // records it and stops looking.
        if ((0x40 <= byte) && (byte <= 0x4f)) {
            rex = byte;
            ++at;
        }

        break;
    }

    if (at >= code.size()) {
        return {};
    }

    auto opcode = byte_at(at++);

    // MOV r/m, r and MOV r/m, imm. The even opcode of each pair is the
    // 8-bit form.
    const auto byte_operand = (0x88 == opcode) || (0xc6 == opcode);
    const auto from_register = (0x88 == opcode) || (0x89 == opcode);
    const auto from_immediate = (0xc6 == opcode) || (0xc7 == opcode);

    if (!from_register && !from_immediate) {
        return {};
    }

    if (at >= code.size()) {
        return {};
    }

    auto modrm = byte_at(at++);
    auto mod = static_cast<std::uint8_t>(modrm >> 6);
    auto reg = static_cast<std::uint8_t>((modrm >> 3) & 0x7);
    auto rm = static_cast<std::uint8_t>(modrm & 0x7);

    // A register destination is not a store, so it cannot be the write
    // that faulted.
    if (0x3 == mod) {
        return {};
    }

    // The operand width. REX.W wins over the 0x66 prefix, which is what
    // makes a 64-bit store 64-bit even with one present.
    std::uint8_t size = 4;
    if (byte_operand) {
        size = 1;
    } else if (rex & 0x8) {
        size = 8;
    } else if (operand_size_override) {
        size = 2;
    }

    // Everything between the ModRM byte and the end of the instruction
    // has to be stepped over, which is the one place this decoder has to
    // understand addressing at all. It never computes the address - the
    // VMCS already reported it - only how many bytes it occupies.
    //
    // Walked for every form, not only the immediate one, because the
    // instruction's length is part of the answer and there is no length
    // without reaching its end.
    if (0x4 == rm) {
        // A SIB byte is present. Its base of five with mod zero means a
        // 32-bit displacement rather than a base register.
        if (at >= code.size()) {
            return {};
        }

        auto sib = byte_at(at++);
        if ((0 == mod) && (0x5 == (sib & 0x7))) {
            at += 4;
        }
    } else if ((0 == mod) && (0x5 == rm)) {
        // RIP relative, which carries a 32-bit displacement.
        at += 4;
    }

    if (1 == mod) {
        at += 1;
    } else if (2 == mod) {
        at += 4;
    }

    if (from_register) {
        // An 8-bit store without REX names AH, CH, DH or BH for
        // encodings four to seven, which are high halves rather than
        // whole registers. Refused rather than mistaken for RSP, RBP,
        // RSI and RDI, which is what indexing the table would do.
        if ((1 == size) && (0 == rex) && (reg >= 4)) {
            return {};
        }

        auto index = static_cast<std::uint8_t>(reg | ((rex & 0x4) << 1));

        // Encoding four without REX.R is RSP, and the context this is
        // handed is the *host's*: the exit stub deliberately stores the
        // address of the context structure in its rsp field, because
        // restoring it iretqs onto that stack. So reading it here would
        // write a hypervisor stack address into a device register and
        // hand a protected module address to the guest at the same time.
        //
        // Refused rather than filled from the VMCS guest RSP, because
        // storing RSP to a device register is not a thing a driver does,
        // and a refusal costs nothing here - the caller falls back to
        // letting the guest's own instruction perform the write.
        //
        // Encoding twelve is R12 and is unaffected: REX.B moves the
        // index past the point where it would collide.
        if (4 == index) {
            return {};
        }

        if (at > code.size()) {
            return {};
        }

        return memory_store{
            .value = detail::truncate(
                registers.*detail::encoded_registers[index], size),
            .size = size,
            .length = static_cast<std::uint8_t>(at),
        };
    }

    // The immediate itself. The 64-bit form still carries only 32 bits,
    // sign extended, and there is no 64-bit immediate MOV to memory.
    auto immediate_size = static_cast<std::size_t>((1 == size) ? 1 : 2);
    if (size >= 4) {
        immediate_size = 4;
    }

    if ((at + immediate_size) > code.size()) {
        return {};
    }

    std::uint64_t value{};
    for (std::size_t i{}; i < immediate_size; ++i) {
        value |= static_cast<std::uint64_t>(byte_at(at + i)) << (i * 8);
    }

    // Sign extension, so a 64-bit store of a negative immediate writes
    // the value the instruction means rather than its low half.
    if ((8 == size) && (value & 0x80000000ull)) {
        value |= 0xffffffff00000000ull;
    }

    return memory_store{
        .value = detail::truncate(value, size),
        .size = size,
        .length = static_cast<std::uint8_t>(at + immediate_size),
    };
}

} // namespace zpp::arch::x86_64
