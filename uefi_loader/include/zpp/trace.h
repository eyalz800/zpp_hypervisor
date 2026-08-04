#pragma once
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>

namespace zpp
{
/**
 * The serial channel used for tracing and for the self check's verdict.
 *
 * Grouped into a struct rather than left as free functions because of how
 * `if constexpr` discards: the branch is dropped from codegen, but the
 * names in it are still looked up, so the functions have to exist. An
 * uncalled static free function draws -Wunused-function, which -Werror
 * turns into a build failure; an uncalled static member function does not.
 */
struct trace
{
    /**
     * Whether this build carries the serial tracing.
     *
     * Its own switch rather than the self check's, so tracing can be
     * turned on while chasing something without also compiling the check,
     * and off while running the check. The build system defaults it to
     * whatever the check is set to.
     *
     * The check's own verdict deliberately does not go through this gate:
     * a verdict is the result, not a diagnostic, so it is written with raw
     * and survives tracing being switched off.
     */
    static constexpr bool enabled = ZPP_TRACE;

    /**
     * Appends a string, returning the new end. No bounds checking: every
     * caller below writes into a buffer sized for the fixed longest line
     * it can produce.
     */
    static constexpr char * append_text(char * out, const char * text)
    {
        while (*text) {
            *out++ = *text++;
        }
        return out;
    }

    /**
     * Appends a value as 0x-prefixed hex with a fixed number of digits.
     */
    static constexpr char * append_hex(char * out,
                                       std::uint64_t value,
                                       std::size_t digits)
    {
        out = append_text(out, "0x");
        for (auto i = digits; i-- > 0;) {
            auto nibble =
                static_cast<std::uint8_t>((value >> (i * 4)) & 0xf);
            *out++ = static_cast<char>(nibble < 10 ? ('0' + nibble)
                                                   : ('a' + nibble - 10));
        }
        return out;
    }

    /**
     * Appends a small unsigned value in decimal.
     */
    static constexpr char * append_decimal(char * out, std::size_t value)
    {
        char digits[20]{};
        std::size_t count{};
        do {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value);
        while (count--) {
            *out++ = digits[count];
        }
        return out;
    }

    /**
     * Returns just the file name out of a path. source_location reports
     * the path as the compiler was given it, which in this build is
     * absolute and long enough to bury the message.
     */
    static constexpr const char * file_name(const char * path)
    {
        auto name = path;
        for (auto * character = path; *character; ++character) {
            if (('/' == *character) || ('\\' == *character)) {
                name = character + 1;
            }
        }
        return name;
    }

    /**
     * Buffer size for one emitted line. Shared rather than function local
     * because both emitters below and the per-CPU reporting in verify
     * build into one of these, and they have to agree.
     */
    static constexpr std::size_t line_capacity = 192;

    /**
     * Writes a string straight to the first serial port, bypassing UEFI
     * console services. OVMF does not necessarily route ConOut to serial,
     * and with no video device there may be nowhere else for it to go, so
     * this is the channel that can be relied on to reach Bochs' serial
     * capture.
     *
     * Raw: no prefix and no newline, so callers that build a whole line
     * themselves can hand it over in one piece.
     */
    /**
     * Whether a 16550 compatible UART answers at the trace port.
     *
     * Probed rather than assumed, because the development target has no
     * serial port at all - and the drain loop below spends a hundred
     * thousand port reads per character waiting for a transmitter that is
     * never going to be ready. That is bounded, so it cannot wedge, but it
     * is slow enough to matter across a whole boot's worth of tracing.
     *
     * The scratch register is the standard detection: it is the one
     * register with no hardware meaning, so a byte written to it reads
     * back only if something is there to store it.
     */
    static bool serial_present()
    {
        constexpr std::uint16_t scratch = port + 7;
        constexpr std::uint8_t probe_value = 0x5a;

        // Probed once. A serial port does not appear part way through a
        // boot, and repeating this per character would defeat the purpose.
        static bool probed = false;
        static bool present = false;

        if (probed) {
            return present;
        }
        probed = true;

        std::uint8_t read_back{};
        asm volatile("outb %0, %1" : : "a"(probe_value), "Nd"(scratch));
        asm volatile("inb %1, %0" : "=a"(read_back) : "Nd"(scratch));

        present = (probe_value == read_back);
        return present;
    }

    /**
     * Everything traced so far, kept so it can be written somewhere
     * durable before control leaves this loader.
     *
     * A machine with no serial port and no debugger leaves nothing else:
     * the screen belongs to whatever is booted next, and a failure that
     * takes the machine down takes the reason with it. Bare metal
     * bring-up needs the reason to outlive the attempt.
     *
     * Fixed size and silently truncating. Losing the tail of a log is a
     * far better failure than allocating - this runs before, during and
     * after the hypervisor takes over, and must not depend on boot
     * services being in any particular state.
     */
    static constexpr std::size_t log_capacity = 0x8000;
    static inline char log_buffer[log_capacity]{};
    static inline std::size_t log_used{};

    /**
     * The trace accumulated so far. A span rather than a pointer and a
     * length, so the two cannot disagree.
     */
    static std::span<const char> log()
    {
        return {log_buffer, log_used};
    }

    /**
     * The trace port. A 16550 at the usual place.
     */
    static constexpr std::uint16_t port = 0x3f8;

    /**
     * Writes a string straight to the first serial port, bypassing UEFI
     * console services, and keeps a copy in log_buffer.
     *
     * OVMF does not necessarily route ConOut to serial, and with no video
     * device there may be nowhere else for it to go, so this is the
     * channel that can be relied on to reach Bochs' serial capture. Where
     * there is no serial port at all the copy is the only record, and
     * write_trace_log puts it on disk.
     *
     * Raw: no prefix and no newline, so callers that build a whole line
     * themselves can hand it over in one piece.
     */
    static void raw(const char * text)
    {
        constexpr std::uint16_t line_status = port + 5;
        constexpr std::uint8_t transmitter_empty = 1u << 5;

        // Kept regardless of whether anything is listening on the port.
        for (auto * character = text; *character; ++character) {
            if (log_used < (log_capacity - 1)) {
                log_buffer[log_used++] = *character;
            }
        }

        if (!serial_present()) {
            return;
        }

        for (auto * character = text; *character; ++character) {
            // Wait for the transmit holding register to drain. Without
            // this only the first sixteen characters ever appear -
            // that is the 16550's FIFO depth, and anything written
            // past a full FIFO is dropped. Bounded, so a machine whose
            // UART stops answering cannot wedge the loader here.
            for (std::uint32_t attempt{}; attempt < 100000u; ++attempt) {
                std::uint8_t status{};
                asm volatile("inb %1, %0"
                             : "=a"(status)
                             : "Nd"(line_status));
                if (status & transmitter_empty) {
                    break;
                }
            }

            asm volatile("outb %0, %1"
                         :
                         : "a"(static_cast<std::uint8_t>(*character)),
                           "Nd"(port));
        }
    }

    /**
     * Stamps a line with where it was written, using the default argument
     * trick: source_location::current() in a default argument is evaluated
     * at the call site, so this reports the caller rather than itself.
     *
     * Anything that forwards to this - the report helpers in verify -
     * has to take a location of its own and pass it on explicitly, or
     * every line would blame the forwarding function.
     */
    static void
    line(const char * text,
         std::source_location location = std::source_location::current())
    {
        if constexpr (!enabled) {
            static_cast<void>(text);
            static_cast<void>(location);
            return;
        }
        char buffer[line_capacity]{};
        auto end = append_text(buffer, "zpp: [");
        end = append_text(end, file_name(location.file_name()));
        end = append_text(end, ":");
        end = append_decimal(end, location.line());
        end = append_text(end, "] ");
        end = append_text(end, text);
        end = append_text(end, "\r\n");
        *end = 0;
        raw(buffer);
    }

    /**
     * The same, with a value appended as hex. Kept as one call so the
     * value cannot end up on a line of its own with its own location
     * stamp.
     */
    static void hex_line(
        const char * text,
        std::uint64_t value,
        std::source_location location = std::source_location::current())
    {
        if constexpr (!enabled) {
            static_cast<void>(text);
            static_cast<void>(value);
            static_cast<void>(location);
            return;
        }
        char buffer[line_capacity]{};
        auto end = append_text(buffer, "zpp: [");
        end = append_text(end, file_name(location.file_name()));
        end = append_text(end, ":");
        end = append_decimal(end, location.line());
        end = append_text(end, "] ");
        end = append_text(end, text);
        end = append_hex(end, value, 16);
        end = append_text(end, "\r\n");
        *end = 0;
        raw(buffer);
    }
};

// The formatting primitives are constant expressions, so the signature
// decoding and the path trimming are tested here rather than only in an
// emulator.
static_assert(trace::file_name("/a/b/main.cpp")[0] == 'm');
static_assert(trace::file_name("main.cpp")[0] == 'm');
static_assert([] {
    char buffer[64]{};
    auto end = trace::append_text(buffer, "n=");
    end = trace::append_decimal(end, 1234);
    end = trace::append_text(end, " h=");
    end = trace::append_hex(end, 0x5a70705a, 8);
    *end = 0;
    const char expected[] = "n=1234 h=0x5a70705a";
    for (std::size_t i{}; i < sizeof(expected); ++i) {
        if (buffer[i] != expected[i]) {
            return false;
        }
    }
    return true;
}());

} // namespace zpp
