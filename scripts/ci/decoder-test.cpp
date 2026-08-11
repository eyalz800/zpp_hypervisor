#include "zpp/arch/x86_64/decoder.h"
#include <array>
#include <cstdio>

using namespace zpp::arch::x86_64;

constexpr context make_registers()
{
    context r{};
    r.rax = 0x1111111122222222ull;
    r.rcx = 0x3333333344444444ull;
    r.rdx = 0xaaaaaaaabbbbbbbbull;
    r.rbx = 0x5555555566666666ull;
    r.r8 = 0x99999999eeeeeeeeull;
    return r;
}

template <std::size_t N>
constexpr std::optional<memory_store> run(const std::uint8_t (&bytes)[N])
{
    std::array<std::byte, N> code{};
    for (std::size_t i{}; i < N; ++i) {
        code[i] = static_cast<std::byte>(bytes[i]);
    }
    return decode_memory_store(code, make_registers());
}

// mov [rcx], edx
constexpr std::uint8_t mov_m32_edx[] = {0x89, 0x11};
static_assert(run(mov_m32_edx)->size == 4);
static_assert(run(mov_m32_edx)->value == 0xbbbbbbbbull);

// mov [rcx], rdx  (REX.W)
constexpr std::uint8_t mov_m64_rdx[] = {0x48, 0x89, 0x11};
static_assert(run(mov_m64_rdx)->size == 8);
static_assert(run(mov_m64_rdx)->value == 0xaaaaaaaabbbbbbbbull);

// mov word [rcx], dx
constexpr std::uint8_t mov_m16_dx[] = {0x66, 0x89, 0x11};
static_assert(run(mov_m16_dx)->size == 2);
static_assert(run(mov_m16_dx)->value == 0xbbbbull);

// mov byte [rcx], dl
constexpr std::uint8_t mov_m8_dl[] = {0x88, 0x11};
static_assert(run(mov_m8_dl)->size == 1);
static_assert(run(mov_m8_dl)->value == 0xbbull);

// mov dword [rcx], 0x12345678
constexpr std::uint8_t mov_m32_imm[] = {
    0xc7, 0x01, 0x78, 0x56, 0x34, 0x12};
static_assert(run(mov_m32_imm)->size == 4);
static_assert(run(mov_m32_imm)->value == 0x12345678ull);

// mov dword [rcx+0x10], eax   (mod=1, disp8)
constexpr std::uint8_t mov_disp8_eax[] = {0x89, 0x41, 0x10};
static_assert(run(mov_disp8_eax)->size == 4);
static_assert(run(mov_disp8_eax)->value == 0x22222222ull);

// mov [rax], r8d  (REX.R selects r8)
constexpr std::uint8_t mov_r8d[] = {0x44, 0x89, 0x00};
static_assert(run(mov_r8d)->size == 4);
static_assert(run(mov_r8d)->value == 0xeeeeeeeeull);

// mov dword [rcx+0x10], 0x1  (imm after disp8)
constexpr std::uint8_t mov_disp8_imm[] = {
    0xc7, 0x41, 0x10, 0x01, 0x00, 0x00, 0x00};
static_assert(run(mov_disp8_imm)->size == 4);
static_assert(run(mov_disp8_imm)->value == 1);

// mov qword [rcx], -1  (imm32 sign extended)
constexpr std::uint8_t mov_m64_imm[] = {
    0x48, 0xc7, 0x01, 0xff, 0xff, 0xff, 0xff};
static_assert(run(mov_m64_imm)->size == 8);
static_assert(run(mov_m64_imm)->value == 0xffffffffffffffffull);

// mov [rsp+disp32], eax  -> SIB present
constexpr std::uint8_t mov_sib[] = {
    0x89, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00};
static_assert(run(mov_sib)->size == 4);
static_assert(run(mov_sib)->value == 0x22222222ull);

// mov [rip+disp32], eax
constexpr std::uint8_t mov_riprel[] = {0x89, 0x05, 0x00, 0x10, 0x00, 0x00};
static_assert(run(mov_riprel)->size == 4);

// --- must be refused ---
// mov edx, ecx  (mod=3, register destination)
constexpr std::uint8_t mov_reg_reg[] = {0x89, 0xca};
static_assert(!run(mov_reg_reg).has_value());

// add [rcx], eax  (read-modify-write)
constexpr std::uint8_t add_mem[] = {0x01, 0x01};
static_assert(!run(add_mem).has_value());

// or dword [rcx], 1
constexpr std::uint8_t or_mem[] = {0x83, 0x09, 0x01};
static_assert(!run(or_mem).has_value());

// mov byte [rcx], ah  (no REX, reg>=4 is a high byte) -> refused
constexpr std::uint8_t mov_ah[] = {0x88, 0x21};
static_assert(!run(mov_ah).has_value());

// truncated immediate
constexpr std::uint8_t truncated[] = {0xc7, 0x01, 0x78};
static_assert(!run(truncated).has_value());

// mov [rcx], rsp  (encoding four without REX.R names RSP, whose field in
// the context is the *host* stack pointer, not the guest's) -> refused
constexpr std::uint8_t mov_from_rsp[] = {0x48, 0x89, 0x21};
static_assert(!run(mov_from_rsp).has_value());

// mov [rcx], r12  (encoding four *with* REX.R is r12, and is fine)
constexpr std::uint8_t mov_from_r12[] = {0x4c, 0x89, 0x21};
static_assert(run(mov_from_r12)->size == 8);
static_assert(run(mov_from_r12)->length == 3);

// --- the instruction length, which the VMCS does not supply for an EPT
// --- violation, so a wrong one here is a guest resumed mid-instruction
static_assert(run(mov_m32_edx)->length == 2);   // opcode, modrm
static_assert(run(mov_m64_rdx)->length == 3);   // rex, opcode, modrm
static_assert(run(mov_m16_dx)->length == 3);    // 0x66, opcode, modrm
static_assert(run(mov_m32_imm)->length == 6);   // + imm32
static_assert(run(mov_disp8_eax)->length == 3); // + disp8
static_assert(run(mov_r8d)->length == 3);       // rex, opcode, modrm
static_assert(run(mov_disp8_imm)->length == 7); // + disp8 + imm32
static_assert(run(mov_m64_imm)->length == 7);   // rex + imm32
static_assert(run(mov_sib)->length == 7);       // sib + disp32
static_assert(run(mov_riprel)->length == 6);    // + disp32

// mov word [rcx], 0x1234 -> prefix, opcode, modrm, imm16
constexpr std::uint8_t mov_imm16[] = {0x66, 0xc7, 0x01, 0x34, 0x12};
static_assert(run(mov_imm16)->size == 2);
static_assert(run(mov_imm16)->length == 5);

// Every length has to be the whole instruction and no more, so the one
// property that must hold for all of them is stated once: a decoded
// length never exceeds the bytes it was given.
static_assert(run(mov_m32_edx)->length <= sizeof(mov_m32_edx));
static_assert(run(mov_sib)->length <= sizeof(mov_sib));
static_assert(run(mov_m64_imm)->length <= sizeof(mov_m64_imm));

int main()
{
    std::printf("all decoder static_asserts passed\n");
    return 0;
}
