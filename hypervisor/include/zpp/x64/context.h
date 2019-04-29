#pragma once
#include <cstdint>

namespace zpp::x64
{
/**
 * Represents the unprivileged state of the processor execution context.
 */
struct alignas(0x10) context
{
    // register - offset
    std::uint64_t rax{};          // 0x00
    std::uint64_t rbx{};          // 0x08
    std::uint64_t rcx{};          // 0x10
    std::uint64_t rdx{};          // 0x18
    std::uint64_t rsp{};          // 0x20
    std::uint64_t rbp{};          // 0x28
    std::uint64_t rsi{};          // 0x30
    std::uint64_t rdi{};          // 0x38
    std::uint64_t r8{};           // 0x40
    std::uint64_t r9{};           // 0x48
    std::uint64_t r10{};          // 0x50
    std::uint64_t r11{};          // 0x58
    std::uint64_t r12{};          // 0x60
    std::uint64_t r13{};          // 0x68
    std::uint64_t r14{};          // 0x70
    std::uint64_t r15{};          // 0x78
    std::uint64_t rip{};          // 0x80
    std::uint64_t rflags{};       // 0x88
    std::uint64_t xmm0[2]{};      // 0x90
    std::uint64_t xmm1[2]{};      // 0xa0
    std::uint64_t xmm2[2]{};      // 0xb0
    std::uint64_t xmm3[2]{};      // 0xc0
    std::uint64_t xmm4[2]{};      // 0xd0
    std::uint64_t xmm5[2]{};      // 0xe0
    std::uint64_t xmm6[2]{};      // 0xf0
    std::uint64_t xmm7[2]{};      // 0x100
    std::uint64_t xmm8[2]{};      // 0x110
    std::uint64_t xmm9[2]{};      // 0x120
    std::uint64_t xmm10[2]{};     // 0x130
    std::uint64_t xmm11[2]{};     // 0x140
    std::uint64_t xmm12[2]{};     // 0x150
    std::uint64_t xmm13[2]{};     // 0x160
    std::uint64_t xmm14[2]{};     // 0x170
    std::uint64_t xmm15[2]{};     // 0x180
    std::uint8_t fxsave[0x200]{}; // 0x190
    std::uint32_t mxcsr{};        // 0x390
    std::uint16_t cs{};           // 0x394
    std::uint16_t ds{};           // 0x396
    std::uint16_t es{};           // 0x398
    std::uint16_t fs{};           // 0x39a
    std::uint16_t gs{};           // 0x39c
    std::uint16_t ss{};           // 0x39e
    // size: 0x3a0
};

static_assert(sizeof(context) == 0x3a0, "Context size mismatch.");

} // namespace zpp::x64
