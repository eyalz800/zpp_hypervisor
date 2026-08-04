#pragma once
extern "C" {
#include <Uefi.h>
}
#include "zpp/loader.h"
#include "zpp/trace.h"

#include <cstddef>
#include <cstdint>

namespace zpp
{
/**
 * The hypervisor self check. Same struct-instead-of-free-functions reason
 * as trace above.
 */
struct verify
{
    /**
     * Whether this build carries the self check. Off by default: it exists
     * a test facility, whether run locally or in CI, and has no place in a
     * loader that is actually being deployed.
     */
    static constexpr bool enabled = ZPP_VERIFY_HYPERVISOR;

    /**
     * cpuid and port output written with register constraints rather than
     * reused from zpp/arch/x86_64/asm.h. Those are naked functions that
     * read their arguments from the System V registers, while this loader
     * is built for the Microsoft ABI, where the third argument arrives in
     * r8 rather than rdx - reusing them here would store the results
     * through the second argument's value instead of a pointer. Letting
     * the compiler allocate registers avoids the question.
     * @{
     */
    static void query_cpuid(std::uint32_t leaf, std::uint32_t (&out)[4])
    {
        std::uint32_t a{};
        std::uint32_t b{};
        std::uint32_t c{};
        std::uint32_t d{};

        asm volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(0u));

        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }

    /**
     * The same, for a leaf that takes a subleaf in ecx. The diagnostic
     * leaf uses it to select which processor is being asked about, so this
     * cannot go through the version above, which forces ecx to zero.
     */
    static void query_cpuid_ecx(std::uint32_t leaf,
                                std::uint32_t subleaf,
                                std::uint32_t (&out)[4])
    {
        std::uint32_t a{};
        std::uint32_t b{};
        std::uint32_t c{};
        std::uint32_t d{};

        asm volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));

        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }

    /**
     * @}
     */

    /**
     * Asks the hypervisor to identify itself, from whichever CPU this runs
     * on.
     *
     * The vmexit handler answers CPUID leaf 0x40000000 with the signature
     * ZppZppZppZpp and sets the hypervisor present bit in leaf 1. Neither
     * can happen unless vmxon, the VMCS setup, vmlaunch, the exit handler
     * and vmresume all worked on this CPU, so one check covers the whole
     * path end to end.
     */
    /**
     * What the CPUID checks saw, so the caller can print the answer
     * instead of only whether it matched. Filled on the CPU under test and
     * read back by the boot CPU afterwards, which is why the reporting is
     * not done here: this runs on an application processor, where touching
     * the UEFI console is not allowed.
     */
    struct hypervisor_signature
    {
        std::uint32_t leaf_1_ecx;
        std::uint32_t signature[4];
    };

    static int on_cpu(void * context)
    {
        auto & result = *static_cast<hypervisor_signature *>(context);
        std::uint32_t registers[4]{};

        // Leaf 1, bit 31 of ecx: a hypervisor is present.
        query_cpuid(1, registers);
        result.leaf_1_ecx = registers[2];

        // Leaf 0x40000000: the vendor signature, in ebx, ecx then edx.
        // Read before the present bit is judged, so a failure still has
        // something to show.
        query_cpuid(1u << 30, registers);
        result.signature[0] = registers[0];
        result.signature[1] = registers[1];
        result.signature[2] = registers[2];
        result.signature[3] = registers[3];

        if (!(result.leaf_1_ecx & (1u << 31))) {
            return -1;
        }

        if ((registers[1] != 0x5a70705a) || (registers[2] != 0x705a7070) ||
            (registers[3] != 0x70705a70)) {
            return -2;
        }

        return 0;
    }

    /**
     * Appends the twelve signature bytes as the text they are meant to
     * spell, which for this hypervisor is ZppZppZppZpp.
     *
     * Non printable bytes are replaced. The bytes come from whatever is
     * answering CPUID, which on a machine running somebody else's
     * hypervisor is arbitrary data, and it must not be able to put control
     * characters into the log.
     */
    static constexpr char *
    append_signature_text(char * out, const std::uint32_t * signature)
    {
        // ebx, then ecx, then edx - the order a CPUID vendor string is
        // assembled in - each little endian.
        for (std::size_t word = 1; word <= 3; ++word) {
            for (std::size_t byte{}; byte < 4; ++byte) {
                auto character = static_cast<char>(
                    (signature[word] >> (byte * 8)) & 0xff);
                *out++ = (character >= 0x20 && character < 0x7f)
                             ? character
                             : '.';
            }
        }
        return out;
    }

    /**
     * Runs the check on every CPU, since the hypervisor is launched per
     * CPU and a failure on one is just as bad as a failure on all. Reports
     * over serial, and also through the UEFI console when one is present.
     */
    static bool present(EFI_SYSTEM_TABLE * system_table,
                        const zpp_loader_parameters & platform)
    {
        // Buffer size for one reported line. The longest this function
        // builds is the leaf 0x40000000 line, at 94 characters including
        // the terminator, so this has room to spare and the append helpers
        // need no bounds checking.
        constexpr std::size_t line_capacity = 128;

        // char16_t and u"" literals rather than EDK2's CHAR16 and L"":
        // char16_t is the standard type that actually means what this
        // needs, and it is what the string literals below are. CHAR16 is a
        // typedef for unsigned short, a distinct type of the same size and
        // alignment, so the reinterpret_cast is confined to the one call
        // that crosses into the firmware.
        auto report = [&](const char * ascii, const char16_t * wide) {
            trace::raw(ascii);
            if (system_table->ConOut) {
                system_table->ConOut->OutputString(
                    system_table->ConOut,
                    reinterpret_cast<CHAR16 *>(
                        const_cast<char16_t *>(wide)));
            }
        };

        // Same two channels, for a line built at runtime. Widening byte by
        // byte is enough because everything printed here is ASCII by
        // construction.
        auto report_line = [&](const char * ascii) {
            trace::raw(ascii);
            if (!system_table->ConOut) {
                return;
            }
            char16_t wide[trace::line_capacity]{};
            std::size_t i{};
            for (; ascii[i] && (i < (line_capacity - 1)); ++i) {
                wide[i] = static_cast<char16_t>(
                    static_cast<unsigned char>(ascii[i]));
            }
            system_table->ConOut->OutputString(
                system_table->ConOut, reinterpret_cast<CHAR16 *>(wide));
        };

        auto cpus = platform.number_of_cpus();
        if (!cpus) {
            report("zpp: ZPP_HYPERVISOR_FAILED no cpus\r\n",
                   u"zpp: ZPP_HYPERVISOR_FAILED no cpus\r\n");
            return false;
        }

        for (std::size_t i{}; i < cpus; ++i) {
            hypervisor_signature found{};
            auto result = platform.call_on_cpu(i, on_cpu, &found);

            // A processor that never ran the function cannot report
            // anything itself, so ask the hypervisor about it from here.
            // This is the only channel: it is not executing, and a
            // debugger cannot be used because QEMU's KVM_GET_MP_STATE
            // discards a pending start-up IPI and so can cause the very
            // failure being diagnosed.
            if (0 != result) {
                std::uint32_t diagnostic[4]{};
                query_cpuid_ecx(0x40000001, static_cast<std::uint32_t>(i),
                                diagnostic);

                char report_buffer[line_capacity]{};
                auto tail =
                    trace::append_text(report_buffer, "zpp: cpu ");
                tail = trace::append_decimal(tail, i);
                tail = trace::append_text(tail, " did not run: exits=");
                tail = trace::append_hex(tail, diagnostic[0], 8);
                tail = trace::append_text(tail, " last_reason=");
                tail = trace::append_hex(tail, diagnostic[1], 8);
                tail = trace::append_text(tail, " qual=");
                tail = trace::append_hex(tail, diagnostic[2], 8);
                tail = trace::append_text(tail, " flags=");
                tail = trace::append_hex(tail, diagnostic[3], 8);
                tail = trace::append_text(tail, "\r\n");
                *tail = 0;
                report_line(report_buffer);
            }

            // Print what CPUID actually answered, pass or fail. The point
            // is to be able to read the signature rather than trust a
            // return value, and a mismatch is exactly the case where the
            // bytes matter most.
            char line[line_capacity]{};
            auto end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, i);
            end = trace::append_text(end, " leaf 1 ecx=");
            end = trace::append_hex(end, found.leaf_1_ecx, 8);
            end = trace::append_text(end, " hypervisor_bit=");
            end = trace::append_text(
                end, (found.leaf_1_ecx & (1u << 31)) ? "1" : "0");
            end = trace::append_text(end, "\r\n");
            *end = 0;
            report_line(line);

            end = trace::append_text(line, "zpp: cpu ");
            end = trace::append_decimal(end, i);
            end = trace::append_text(end, " leaf 0x40000000 ebx=");
            end = trace::append_hex(end, found.signature[1], 8);
            end = trace::append_text(end, " ecx=");
            end = trace::append_hex(end, found.signature[2], 8);
            end = trace::append_text(end, " edx=");
            end = trace::append_hex(end, found.signature[3], 8);
            end = trace::append_text(end, " text=\"");
            end = append_signature_text(end, found.signature);
            end = trace::append_text(end, "\"\r\n");
            *end = 0;
            report_line(line);

            if (result) {
                report(
                    "zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n",
                    u"zpp: ZPP_HYPERVISOR_FAILED on at least one cpu\r\n");
                return false;
            }
        }

        report("zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n",
               u"zpp: ZPP_HYPERVISOR_ACTIVE on every cpu\r\n");
        return true;
    }
};

} // namespace zpp
