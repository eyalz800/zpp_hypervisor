// Compile-time tests for the instruction decoder.
//
// Every case is a static_assert, so the check is the build: a decoder that
// stops answering an encoding cannot be committed, and the encodings are
// written out as bytes rather than assembled, so the test does not depend
// on an assembler agreeing with the decoder about what it meant.
#include "zpp/arch/x86_64/instruction.h"

#include <print>

using namespace zpp::arch::x86_64;

constexpr context registers_for_test()
{
    context registers{};
    registers.rax = 0x1111'1111'1111'1111;
    registers.rcx = 0x2222'2222'2222'2222;
    registers.rdx = 0x0000'0000'dead'beef;
    registers.rbx = 0x4444'4444'4444'4444;
    registers.rsp =
        0xffff'8000'0000'0000; // the host stack, never readable
    registers.rbp = 0x6666'6666'6666'6666;
    registers.rsi = 0x7777'7777'7777'7777;
    registers.rdi = 0x8888'8888'8888'8888;
    registers.r12 = 0xcccc'cccc'cccc'cccc;
    return registers;
}

template <std::size_t Size>
constexpr auto run_in(const std::uint8_t (&bytes)[Size], code_size mode)
{
    std::byte code[Size]{};
    for (std::size_t i{}; i < Size; ++i) {
        code[i] = static_cast<std::byte>(bytes[i]);
    }

    return decode(std::span<const std::byte>{code, Size},
                  registers_for_test(),
                  mode);
}

// The default for every case below, so the existing bodies read unchanged.
// The code size cases state theirs explicitly.
template <std::size_t Size>
constexpr auto run(const std::uint8_t (&bytes)[Size])
{
    return run_in(bytes, code_size::bits_64);
}

// --- plain stores, which the narrow decoder also handled ---------------

// mov [rcx], edx
constexpr std::uint8_t store_dword[] = {0x89, 0x11};
static_assert(run(store_dword)->what == memory_operation::store);
static_assert(run(store_dword)->size == 4);
static_assert(run(store_dword)->operand == 0xdeadbeef);
static_assert(run(store_dword)->length == 2);

// mov [rcx], rdx
constexpr std::uint8_t store_qword[] = {0x48, 0x89, 0x11};
static_assert(run(store_qword)->size == 8);
static_assert(run(store_qword)->operand == 0xdeadbeef);

// mov word [rcx], dx
constexpr std::uint8_t store_word[] = {0x66, 0x89, 0x11};
static_assert(run(store_word)->size == 2);
static_assert(run(store_word)->operand == 0xbeef);

// mov dword [rcx], 0x12345678
constexpr std::uint8_t store_immediate[] = {
    0xc7, 0x01, 0x78, 0x56, 0x34, 0x12};
static_assert(run(store_immediate)->operand == 0x12345678);
static_assert(run(store_immediate)->length == 6);

// mov qword [rcx], -1 -- the immediate is 32 bits, sign extended
constexpr std::uint8_t store_negative[] = {
    0x48, 0xc7, 0x01, 0xff, 0xff, 0xff, 0xff};
static_assert(run(store_negative)->operand == 0xffffffffffffffffull);

// --- loads, which it did not -------------------------------------------

// mov edx, [rcx]
constexpr std::uint8_t load_dword[] = {0x8b, 0x11};
static_assert(run(load_dword)->what == memory_operation::load);
static_assert(run(load_dword)->size == 4);
static_assert(run(load_dword)->writes_register);
static_assert(run(load_dword)->destination == 2); // rdx

// A 4-byte load clears the upper half of the register; a 2-byte one does
// not. That asymmetry is the architecture's and is the easiest thing here
// to get wrong in a caller.
static_assert(result_for_register(*run(load_dword),
                                  0xaabbccdd,
                                  0xffff'ffff'ffff'ffff) == 0xaabbccdd);

// mov dx, [rcx]
constexpr std::uint8_t load_word[] = {0x66, 0x8b, 0x11};
static_assert(run(load_word)->size == 2);
static_assert(result_for_register(*run(load_word),
                                  0xaabb,
                                  0xffff'ffff'ffff'ffff) ==
              0xffff'ffff'ffff'aabb);

// --- the widening moves ------------------------------------------------

// movzx edx, byte [rcx]
constexpr std::uint8_t widen_zero[] = {0x0f, 0xb6, 0x11};
static_assert(run(widen_zero)->what == memory_operation::load);
static_assert(run(widen_zero)->size == 1);
static_assert(!run(widen_zero)->sign_extends);
static_assert(result_for_register(*run(widen_zero),
                                  0xff,
                                  0xffff'ffff'ffff'ffff) == 0xff);

// movsx edx, byte [rcx]
constexpr std::uint8_t widen_sign[] = {0x0f, 0xbe, 0x11};
static_assert(run(widen_sign)->sign_extends);
// A 32-bit destination is filled and the rest of the register cleared, so
// sign extending a byte of 0xff gives 0x00000000ffffffff - not all ones.
static_assert(result_for_register(*run(widen_sign),
                                  0xff,
                                  0xffff'ffff'ffff'ffff) ==
              0x0000'0000'ffff'ffff);

// movsx edx, word [rcx]
constexpr std::uint8_t widen_sign_word[] = {0x0f, 0xbf, 0x11};
static_assert(run(widen_sign_word)->size == 2);
static_assert(result_for_register(*run(widen_sign_word),
                                  0x8000,
                                  0xffff'ffff'ffff'ffff) ==
              0x0000'0000'ffff'8000);

// movzx ax, byte [rcx] -- a 16-bit destination preserves the upper bits
constexpr std::uint8_t widen_zero_word_dest[] = {0x66, 0x0f, 0xb6, 0x11};
static_assert(result_for_register(*run(widen_zero_word_dest),
                                  0x01,
                                  0xffff'ffff'ffff'ffff) ==
              0xffff'ffff'ffff'0001);

// --- read-modify-write, which is what a driver does to a register ------

// or [rcx], edx
constexpr std::uint8_t combine_or[] = {0x09, 0x11};
static_assert(run(combine_or)->what == memory_operation::combine);
static_assert(run(combine_or)->how == combine_with::bitwise_or);
static_assert(apply(*run(combine_or), 0x0000'0001) == 0xdead'beef);

// and [rcx], edx
constexpr std::uint8_t combine_and[] = {0x21, 0x11};
static_assert(run(combine_and)->how == combine_with::bitwise_and);
static_assert(apply(*run(combine_and), 0xffff'0000) == 0xdead'0000);

// xor [rcx], edx
constexpr std::uint8_t combine_xor[] = {0x31, 0x11};
static_assert(run(combine_xor)->how == combine_with::bitwise_xor);
static_assert(apply(*run(combine_xor), 0xffff'ffff) == 0x2152'4110);

// add [rcx], edx -- and the width has to wrap, not carry out of it
constexpr std::uint8_t combine_add[] = {0x01, 0x11};
static_assert(run(combine_add)->how == combine_with::add);
static_assert(apply(*run(combine_add), 0xffff'ffff) == 0xdead'beee);

// sub [rcx], edx
constexpr std::uint8_t combine_sub[] = {0x29, 0x11};
static_assert(apply(*run(combine_sub), 0xdead'beef) == 0);

// --- the immediate group, where the operation is in the ModRM ----------

// or dword [rcx], 0x40
constexpr std::uint8_t group_or[] = {0x83, 0x09, 0x40};
static_assert(run(group_or)->what == memory_operation::combine);
static_assert(run(group_or)->how == combine_with::bitwise_or);
static_assert(apply(*run(group_or), 1) == 0x41);
static_assert(run(group_or)->length == 3);

// and dword [rcx], -16 -- 0x83 sign extends its single byte
constexpr std::uint8_t group_and[] = {0x83, 0x21, 0xf0};
static_assert(run(group_and)->how == combine_with::bitwise_and);
static_assert(apply(*run(group_and), 0xff) == 0xf0);

// cmp dword [rcx], 1 -- examines, and leaves memory alone
constexpr std::uint8_t group_compare[] = {0x83, 0x39, 0x01};
static_assert(run(group_compare)->what == memory_operation::examine);
static_assert(apply(*run(group_compare), 0x1234) == 0x1234);

// --- the bit operations, which is how a flag gets set in a register ----

// bts dword [rcx], 12
constexpr std::uint8_t bit_set[] = {0x0f, 0xba, 0x29, 0x0c};
static_assert(run(bit_set)->what == memory_operation::combine);
static_assert(run(bit_set)->how == combine_with::set_bit);
static_assert(apply(*run(bit_set), 0) == 0x1000);

// btr dword [rcx], 12
constexpr std::uint8_t bit_clear[] = {0x0f, 0xba, 0x31, 0x0c};
static_assert(run(bit_clear)->how == combine_with::clear_bit);
static_assert(apply(*run(bit_clear), 0xffff'ffff) == 0xffff'efff);

// btc dword [rcx], 0
constexpr std::uint8_t bit_flip[] = {0x0f, 0xba, 0x39, 0x00};
static_assert(run(bit_flip)->how == combine_with::flip_bit);
static_assert(apply(*run(bit_flip), 1) == 0);

// bt dword [rcx], 3 -- examines only
constexpr std::uint8_t bit_test[] = {0x0f, 0xba, 0x21, 0x03};
static_assert(run(bit_test)->what == memory_operation::examine);

// A bit number outside the operand is refused, not reduced.
//
// This case asserted the opposite - `->operand == 1`, the modulo the
// decoder used to take - and went on asserting it after the decoder
// stopped, because nothing ran this file. The modulo applies only where
// the bit base is a *register*: `.references/sdm.txt:38974`, "if the bit
// base operand specifies a register, the instruction takes the modulo
// 16, 32, or 64 of the bit offset operand". With a memory bit base the
// processor moves the access instead, to `Effective Address + (4 *
// (BitOffset DIV 32))` for a 32-bit operand (`:38988`).
//
// So `btsl $33, (%rcx)` sets bit 1 of the dword at `[rcx+4]`, and
// reducing 33 to 1 set the right bit of the *wrong* dword - on a watched
// page, a write to a device register four bytes from the one the guest
// named, with the named one left alone. Refused rather than followed,
// because on_ept_violation pastes the low twelve bits of the effective
// address onto the page the violation reported and an adjusted address
// would land back inside it.
constexpr std::uint8_t bit_set_wrapping[] = {0x0f, 0xba, 0x29, 0x21};
static_assert(!run(bit_set_wrapping).has_value());

// And the last offset that is still inside a dword operand is accepted,
// so the refusal above is a boundary rather than a blanket.
constexpr std::uint8_t bit_set_last[] = {0x0f, 0xba, 0x29, 0x1f};
static_assert(run(bit_set_last)->operand == 31);

// --- exchange ----------------------------------------------------------

// xchg [rcx], edx
constexpr std::uint8_t exchange[] = {0x87, 0x11};
static_assert(run(exchange)->what == memory_operation::exchange);
static_assert(apply(*run(exchange), 0x1234) == 0xdead'beef);
static_assert(run(exchange)->writes_register);
static_assert(result_for_register(*run(exchange), 0x1234, 0) == 0x1234);

// --- test --------------------------------------------------------------

// test [rcx], edx
constexpr std::uint8_t test_register[] = {0x85, 0x11};
static_assert(run(test_register)->what == memory_operation::examine);

// test dword [rcx], 0x10
constexpr std::uint8_t test_immediate[] = {
    0xf7, 0x01, 0x10, 0x00, 0x00, 0x00};
static_assert(run(test_immediate)->what == memory_operation::examine);
static_assert(run(test_immediate)->length == 6);

// --- addressing forms, which only affect where the instruction ends ----

// mov [rcx+0x10], edx
constexpr std::uint8_t displacement_byte[] = {0x89, 0x51, 0x10};
static_assert(run(displacement_byte)->length == 3);

// mov [rcx+0x100], edx
constexpr std::uint8_t displacement_dword[] = {
    0x89, 0x91, 0x00, 0x01, 0x00, 0x00};
static_assert(run(displacement_dword)->length == 6);

// mov [rax+rbx*4], edx
constexpr std::uint8_t scaled_index[] = {0x89, 0x14, 0x98};
static_assert(run(scaled_index)->length == 3);

// mov [rip+disp32], edx
constexpr std::uint8_t rip_relative[] = {
    0x89, 0x15, 0x00, 0x10, 0x00, 0x00};
static_assert(run(rip_relative)->length == 6);

// mov [rsp+0x8], edx -- a stack base is fine; only reading rsp is not
constexpr std::uint8_t stack_base[] = {0x89, 0x54, 0x24, 0x08};
static_assert(run(stack_base)->length == 4);
static_assert(run(stack_base)->operand == 0xdeadbeef);

// --- what must be refused ----------------------------------------------

// mov edx, ecx -- a register destination is not a memory access
constexpr std::uint8_t register_destination[] = {0x89, 0xca};
static_assert(!run(register_destination).has_value());

// mov [rcx], rsp -- that field holds the *host* stack pointer
constexpr std::uint8_t from_stack_pointer[] = {0x48, 0x89, 0x21};
static_assert(!run(from_stack_pointer).has_value());

// mov [rcx], r12 -- encoding four with REX.B is r12 and is fine
constexpr std::uint8_t from_r12[] = {0x4c, 0x89, 0x21};
static_assert(run(from_r12)->operand == 0xcccc'cccc'cccc'cccc);

// mov byte [rcx], ah -- a high byte, not a register
constexpr std::uint8_t from_high_byte[] = {0x88, 0x21};
static_assert(!run(from_high_byte).has_value());

// adc dword [rcx], 1 -- needs a carry flag this does not carry
constexpr std::uint8_t needs_carry[] = {0x83, 0x11, 0x01};
static_assert(!run(needs_carry).has_value());

// truncated immediate
constexpr std::uint8_t truncated[] = {0xc7, 0x01, 0x78};
static_assert(!run(truncated).has_value());

// truncated modrm
constexpr std::uint8_t truncated_modrm[] = {0x89};
static_assert(!run(truncated_modrm).has_value());

// an opcode outside the set
constexpr std::uint8_t unknown[] = {0x0f, 0x05};
static_assert(!run(unknown).has_value());

// --- the code size, which decides what the same bytes mean --------------

// mov [0x0300], ax -- 66 89 06 00 03, five bytes in 16-bit code. There the
// 0x66 prefix selects a 32-bit operand rather than a 16-bit one, and
// mod=00, rm=110 is a 16-bit displacement with no scale-index-base byte.
// A long-mode reading calls it three bytes and a 32-bit store, so a caller
// that trusted it would resume two bytes inside the instruction. Refused.
constexpr std::uint8_t sixteen_bit_store[] = {
    0x66, 0x89, 0x06, 0x00, 0x03};
static_assert(!run_in(sixteen_bit_store, code_size::bits_16).has_value());

// The same bytes are decodable in the two sizes that are answered, and the
// wrong length is exactly the one recorded above - which is what makes
// refusing 16-bit code the fix rather than a caution.
static_assert(run_in(sixteen_bit_store, code_size::bits_64)->length == 3);
static_assert(run_in(sixteen_bit_store, code_size::bits_64)->size == 2);

// Nothing at all is decoded in 16-bit code, not merely the ambiguous
// forms.
static_assert(!run_in(store_dword, code_size::bits_16).has_value());
static_assert(!run_in(load_dword, code_size::bits_16).has_value());
static_assert(!run_in(bit_set, code_size::bits_16).has_value());

// --- 32-bit code, which is answered ------------------------------------

// mov [ecx], edx. The default operand size is four in a D/B code segment,
// same as long mode without REX.W, and the addressing bytes have the same
// shape - so the answer is identical.
static_assert(run_in(store_dword, code_size::bits_32)->size == 4);
static_assert(run_in(store_dword, code_size::bits_32)->length == 2);

// mov word [ecx], dx -- 0x66 narrows in 32-bit code just as it does here.
static_assert(run_in(store_word, code_size::bits_32)->size == 2);
static_assert(run_in(store_word, code_size::bits_32)->operand == 0xbeef);

// 0x40 to 0x4f are INC and DEC opcodes outside 64-bit code, not REX. Read
// as a prefix, the byte after one becomes the opcode and an unrelated
// instruction comes out; `48 89 11` is `mov [rcx], rdx` in long mode and
// `dec eax` in 32-bit code, which touches no memory.
static_assert(run_in(store_qword, code_size::bits_64)->size == 8);
static_assert(!run_in(store_qword, code_size::bits_32).has_value());

// An address-size prefix selects 16-bit addressing in 32-bit code, which
// changes the addressing bytes and therefore every length. Refused there,
// and ignored in 64-bit code where it selects 32-bit addressing and
// changes nothing this decoder reads.
constexpr std::uint8_t address_size_store[] = {0x67, 0x89, 0x11};
static_assert(run_in(address_size_store, code_size::bits_64)->length == 3);
static_assert(!run_in(address_size_store, code_size::bits_32).has_value());

// A decoded length never exceeds the bytes it was given, for every form.
static_assert(run(store_dword)->length <= sizeof(store_dword));
static_assert(run(bit_set)->length <= sizeof(bit_set));
static_assert(run(scaled_index)->length <= sizeof(scaled_index));
static_assert(run(test_immediate)->length <= sizeof(test_immediate));

// --- flags, which are not optional -------------------------------------
//
// Leaving these out killed a guest: it triple faulted after 179 emulated
// instructions, because `and [mem], eax; jz` took the wrong branch.

constexpr std::uint64_t no_flags = 0;
constexpr std::uint64_t all_arithmetic = status_flag::arithmetic;

// and [rcx], edx where the result is zero -> ZF set, CF and OF cleared
static_assert((flags_after(*run(combine_and),
                           all_arithmetic,
                           0x0000'0000,
                           0x0000'0000) &
               status_flag::zero) != 0);
static_assert((flags_after(*run(combine_and), all_arithmetic, 0, 0) &
               status_flag::carry) == 0);
static_assert((flags_after(*run(combine_and), all_arithmetic, 0, 0) &
               status_flag::overflow) == 0);

// a non-zero logical result clears ZF
static_assert((flags_after(*run(combine_or), no_flags, 1, 0xdead'beef) &
               status_flag::zero) == 0);

// and a negative one sets SF
static_assert((flags_after(*run(combine_or), no_flags, 0, 0x8000'0000) &
               status_flag::sign) != 0);

// cmp dword [rcx], 1 with memory holding 1 -> equal, so ZF and no CF
static_assert((flags_after(*run(group_compare), no_flags, 1, 1) &
               status_flag::zero) != 0);
static_assert((flags_after(*run(group_compare), no_flags, 1, 1) &
               status_flag::carry) == 0);

// with memory holding 0, 0 - 1 borrows -> CF set, ZF clear
static_assert((flags_after(*run(group_compare), no_flags, 0, 0) &
               status_flag::carry) != 0);
static_assert((flags_after(*run(group_compare), no_flags, 0, 0) &
               status_flag::zero) == 0);

// sub [rcx], edx to zero sets ZF
static_assert((flags_after(*run(combine_sub), no_flags, 0xdead'beef, 0) &
               status_flag::zero) != 0);

// add [rcx], edx that wraps sets CF
static_assert(
    (flags_after(*run(combine_add), no_flags, 0xffff'ffff, 0xdead'beee) &
     status_flag::carry) != 0);

// bts reports the bit as it was in CF, and leaves the rest alone
static_assert((flags_after(*run(bit_set), no_flags, 0x1000, 0x1000) &
               status_flag::carry) != 0);
static_assert((flags_after(*run(bit_set), no_flags, 0, 0x1000) &
               status_flag::carry) == 0);
static_assert(flags_after(*run(bit_set), status_flag::zero, 0, 0x1000) ==
              (status_flag::zero));

// bt likewise
static_assert((flags_after(*run(bit_test), no_flags, 0x8, 0x8) &
               status_flag::carry) != 0);

// test dword [rcx], 0x10 against memory without that bit -> ZF
static_assert((flags_after(*run(test_immediate), no_flags, 0x01, 0x01) &
               status_flag::zero) != 0);
static_assert((flags_after(*run(test_immediate), no_flags, 0x10, 0x10) &
               status_flag::zero) == 0);

// the moves and the exchange affect nothing at all
static_assert(flags_after(*run(store_dword), all_arithmetic, 0, 0) ==
              all_arithmetic);
static_assert(flags_after(*run(load_dword), all_arithmetic, 0, 0) ==
              all_arithmetic);
static_assert(flags_after(*run(widen_sign), all_arithmetic, 0, 0) ==
              all_arithmetic);
static_assert(flags_after(*run(exchange), all_arithmetic, 0, 0) ==
              all_arithmetic);

// --- the effective address ---------------------------------------------
//
// Wanted because an EPT violation does not always report one: it carries a
// guest-linear address only with exit qualification bit 7 set, and the
// guest-physical address beside it is page granular. Every case below is
// therefore an offset within a page as much as an address.

template <std::size_t Size>
constexpr auto address_of(const std::uint8_t (&bytes)[Size],
                          std::uint64_t rip = 0)
{
    return effective_address(*run(bytes), registers_for_test(), rip);
}

// mov [rcx], edx -- base only. rcx is 0x2222'2222'2222'2222.
static_assert(*address_of(store_dword) == 0x2222222222222222ull);

// mov [rcx+0x300], edx -- the encoding a local APIC write compiles to,
// and the one this whole thing exists for.
constexpr std::uint8_t store_disp32[] = {
    0x89, 0x91, 0x00, 0x03, 0x00, 0x00};
static_assert(*address_of(store_disp32) ==
              (0x2222222222222222ull + 0x300));

// mov [rcx+8], edx -- a one-byte displacement.
constexpr std::uint8_t store_disp8[] = {0x89, 0x51, 0x08};
static_assert(*address_of(store_disp8) == 0x222222222222222aull);

// mov [rcx-8], edx -- and a negative one, which is sign extended rather
// than added as 0xf8.
constexpr std::uint8_t store_negative_disp8[] = {0x89, 0x51, 0xf8};
static_assert(*address_of(store_negative_disp8) == 0x222222222222221aull);

// mov [rcx+rax*4], edx -- a scale-index-base byte with both registers.
constexpr std::uint8_t store_indexed[] = {0x89, 0x14, 0x81};
static_assert(*address_of(store_indexed) ==
              (0x2222222222222222ull + (0x1111111111111111ull * 4)));

// mov [rcx+rax*4+0x300], edx -- the same with a displacement, which is
// how a compiler indexes a register file.
constexpr std::uint8_t store_indexed_disp[] = {
    0x89, 0x94, 0x81, 0x00, 0x03, 0x00, 0x00};
static_assert(*address_of(store_indexed_disp) ==
              (0x2222222222222222ull + (0x1111111111111111ull * 4) +
               0x300));

// mov [rcx*1], edx with no base -- mod zero, base five in the
// scale-index-base byte, which means a 32-bit displacement instead.
constexpr std::uint8_t store_index_only[] = {
    0x89, 0x14, 0x0d, 0x00, 0x03, 0x00, 0x00};
static_assert(*address_of(store_index_only) ==
              (0x2222222222222222ull + 0x300));

// mov [rip+0x300], edx -- relative to the *end* of the instruction, so
// the length is part of the answer.
constexpr std::uint8_t store_rip_relative[] = {
    0x89, 0x15, 0x00, 0x03, 0x00, 0x00};
static_assert(*address_of(store_rip_relative, 0xfee00000) ==
              (0xfee00000ull + 6 + 0x300));

// The same bytes in 32-bit code are an absolute address, not a relative
// one - mod zero with rm five means something else there.
static_assert(*effective_address(*run_in(store_rip_relative,
                                         code_size::bits_32),
                                 registers_for_test(),
                                 0xfee00000) == 0x300);

// A segment override is refused rather than approximated: FS and GS carry
// bases that are not page aligned and are not in the instruction.
constexpr std::uint8_t store_through_gs[] = {
    0x65, 0x89, 0x91, 0x00, 0x03, 0x00, 0x00};
static_assert(!address_of(store_through_gs).has_value());

// So is a base of RSP, because the context this is handed holds the
// *host's* stack pointer in that slot.
constexpr std::uint8_t store_through_rsp[] = {0x89, 0x14, 0x24};
static_assert(!address_of(store_through_rsp).has_value());

// R12 uses the same encoding with REX.B and is unaffected.
constexpr std::uint8_t store_through_r12[] = {0x41, 0x89, 0x14, 0x24};
static_assert(*address_of(store_through_r12) == 0xccccccccccccccccull);

// The address-size prefix truncates the whole computation rather than any
// one term of it.
constexpr std::uint8_t store_address_size[] = {0x67, 0x89, 0x11};
static_assert(*address_of(store_address_size) == 0x22222222ull);

// A read-modify-write form addresses memory the same way. The APIC page
// sees these too, which is why the decoder answers them at all.
constexpr std::uint8_t combine_disp32[] = {
    0x09, 0x91, 0x00, 0x03, 0x00, 0x00};
static_assert(*address_of(combine_disp32) ==
              (0x2222222222222222ull + 0x300));

int main()
{
    std::println(
        "all instruction decoder static_asserts passed");
    return 0;
}
