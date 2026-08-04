#include "zpp/arch/x86_64/exception_entry.h"

namespace zpp::arch::x86_64
{
/**
 * The common tail of every entry stub. The whole exception frame is
 * already on the stack, so all that is left is to name it and call the
 * handler.
 */
extern "C" void __attribute__((naked)) zpp_x86_64_exception_common()
{
    asm(R"!!(
        .intel_syntax noprefix
        // rsp points at the exception frame: the vector and error code
        // the stub pushed, then the rip, cs, rflags, rsp and ss the CPU
        // pushed. The CPU aligns the stack to sixteen bytes before it
        // pushes, and the frame is always seven qwords because the stub
        // fills in a missing error code, so rsp is 8 modulo 16 here and
        // one more qword makes it what the call expects.
        mov rdi, rsp
        sub rsp, 0x8
        call zpp_x86_64_exception
        // zpp_x86_64_exception never returns.
        ud2
    )!!");
}

/**
 * The exception entry table, one stub per vector.
 *
 * The entry point for a vector is the table base plus the vector times
 * exception_entry_stride, so every stub has to be the same length. Two
 * things make that hold: the widest stub is a two byte push, a five byte
 * push and a five byte jump, which is twelve bytes and so fits within the
 * stride; and the alignment directive rounds each one up to exactly the
 * stride. The alignment attribute on the function is what makes the base
 * itself aligned, which the arithmetic also depends on.
 */
extern "C" void __attribute__((naked)) __attribute__((aligned(16)))
zpp_x86_64_exception_entry_table()
{
    asm(R"!!(
        .intel_syntax noprefix

        // The vectors the CPU delivers with an error code already on the
        // stack: #DF, #TS, #NP, #SS, #GP, #PF, #AC, #CP, #VC and #SX.
        .set zpp_x86_64_error_code_mask, 0
        .irp vector, 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
        .set zpp_x86_64_error_code_mask, zpp_x86_64_error_code_mask | (1 << \vector)
        .endr

        .set zpp_x86_64_vector, 0
        .rept 256
        .balign 16

        // Push a zero in place of the error code the CPU did not push, so
        // the handler sees one frame shape. The bound is tested first
        // because the shift is evaluated either way and no vector above
        // thirty carries an error code.
        .if (zpp_x86_64_vector > 30) || !((zpp_x86_64_error_code_mask >> zpp_x86_64_vector) & 1)
        push 0
        .endif

        // The vector, written as an explicit push imm32 encoding. Left to
        // the assembler it would be a two byte push below 128 and a five
        // byte push above, and a stub whose length depends on its vector
        // cannot be reached by multiplying.
        .byte 0x68
        .long zpp_x86_64_vector

        jmp zpp_x86_64_exception_common

        .set zpp_x86_64_vector, zpp_x86_64_vector + 1
        .endr
    )!!");
}

} // namespace zpp::arch::x86_64
