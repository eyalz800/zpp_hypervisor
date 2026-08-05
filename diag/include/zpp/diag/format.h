#pragma once
#include "zpp/diag/config.h"

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <type_traits>

namespace zpp::diag
{
/**
 * Reports a bad format at compile time.
 *
 * Deliberately not constexpr: calling it from a constant expression makes
 * the evaluation fail, which is how a consteval function rejects its
 * argument in a build with -fno-exceptions, where `throw` is unavailable.
 * Defined rather than declared so nothing can be undefined at link time if
 * it is ever reached outside constant evaluation. This is the shape
 * zpp/hypervisor/log.h already uses.
 */
inline void format_error()
{
    __builtin_trap();
}

/**
 * The event id, standing in for the format string.
 *
 * Owned by the record format rather than by this file: the id has to be
 * stable across builds, has to be emitted into a database the host side
 * decoder reads, and the packing of the argument words around it is the
 * record format's business. What this architecture needs from it is
 * exactly this signature and one guarantee - that it is consteval, so that
 * a binary-only build carries an integer immediate and never the string it
 * came from. That is the whole trick behind tokenized logging, and it is
 * why an event id is not merely a size optimisation: it is what lets a
 * build with no text sink contain no format strings at all.
 *
 * The definition below is a placeholder FNV-1a so this header compiles on
 * its own. Replace it with the record format's, do not add a second one.
 */
consteval std::uint32_t event_id(const char * text, std::size_t length)
{
    std::uint32_t hash = 0x811c9dc5;
    for (std::size_t i{}; i < length; ++i) {
        hash ^= static_cast<std::uint8_t>(text[i]);
        hash *= 0x01000193;
    }
    return hash;
}

/**
 * A format string with its placeholders already located, its event id
 * already computed, and its call site already recorded.
 *
 * Three things happen here, all of them while compiling:
 *
 * - The `{}` positions are found once rather than on every VM exit.
 * - The event id is computed, so a build with no text sink needs no
 *   string.
 * - The call site is captured. source_location::current() as a default
 *   argument is evaluated where the *constructor* is invoked, which is the
 *   log call, so the location travels with the format string. That is what
 *   lets the log entry point be an ordinary function template rather than
 *   the class template with a deduction guide that zpp/hypervisor/log.h
 *   needs today - a function cannot put a defaulted parameter after a
 *   pack, but it does not have to when the location arrives with the
 *   first argument. The most-vexing-parse caveat on `log(something);`
 *   goes away with it.
 *
 * The pointers are null when no text sink is compiled in. That is not
 * tidiness: a pointer to a string literal stored in a constant expression
 * is what forces the literal into .rodata, and not storing it is what
 * keeps every format string in the tree out of a binary-only build. The
 * consteval constructor runs entirely in the compiler, so a literal it
 * looks at but does not keep leaves nothing behind.
 */
struct format_spec
{
    /**
     * Placeholders allowed in one line. More is a compile error rather
     * than a truncation, since at this scale it can only be a mistake.
     */
    static constexpr std::size_t max_placeholders = 12;

    template <std::size_t Size>
    consteval format_spec(
        const char (&literal)[Size],
        std::source_location where = std::source_location::current()) :
        event{event_id(literal, Size - 1)},
        length{static_cast<std::uint16_t>(Size - 1)},
        line{static_cast<std::uint16_t>(where.line())}
    {
        for (std::size_t i{}; i < (Size - 1); ++i) {
            if (('{' == literal[i]) && ((i + 1) < (Size - 1)) &&
                ('}' == literal[i + 1])) {
                if (count == max_placeholders) {
                    // A constant expression calling a function that is not
                    // constexpr is a compile error, which is the only way
                    // to reject an argument here.
                    format_error();
                }
                if constexpr (renders_text) {
                    offsets[count] = static_cast<std::uint8_t>(i);
                }
                ++count;
                ++i;
            }
        }

        if constexpr (renders_text) {
            text = literal;
            file = trailing_component(where.file_name());
        }
    }

    /**
     * The last component of a path. source_location reports the path as
     * the compiler was given it, which in this build is absolute and long
     * enough to bury the message.
     */
    static consteval const char * trailing_component(const char * path)
    {
        auto name = path;
        for (auto character = path; *character; ++character) {
            if (('/' == *character) || ('\\' == *character)) {
                name = character + 1;
            }
        }
        return name;
    }

    /**
     * The format string and its file, or null in a build that renders no
     * text.
     * @{
     */
    const char * text{};
    const char * file{};
    /**
     * @}
     */

    /**
     * What the record format identifies this line by.
     */
    std::uint32_t event{};

    /**
     * The format string's length, excluding the terminator, and the line
     * it was written on.
     * @{
     */
    std::uint16_t length{};
    std::uint16_t line{};
    /**
     * @}
     */

    /**
     * How many placeholders, and where each one starts. The offsets are
     * only filled in when something renders text.
     * @{
     */
    std::uint8_t count{};
    std::uint8_t offsets[max_placeholders]{};
    /**
     * @}
     */
};

/**
 * One logged value, reduced to a word and a tag.
 *
 * This is what lets the call site stay small and the renderer stay out of
 * line: the values are turned into an array of these at the call site -
 * stores of registers into stack slots, nothing more - and everything that
 * knows how to format is reached through one call taking a span of them.
 * Without it, the whole renderer would be instantiated per argument list
 * and inlined into every VM exit handler case.
 *
 * A text argument keeps a pointer, which is safe for as long as the
 * rendering, and only that long. Nothing retained ever holds one - see the
 * note in ring.h - so a logged string cannot dangle into a record read
 * back an hour later by a debugger.
 */
struct argument
{
    enum class kind : std::uint8_t
    {
        unsigned_integer,
        signed_integer,
        boolean,
        text,
        pointer,
    };

    std::uint64_t value{};
    const char * text{};
    kind what{kind::unsigned_integer};
};

/**
 * Turns whatever a caller passed into an argument.
 *
 * Everything numeric becomes hex, which is what register and MSR contents
 * want, and anything enumerated becomes its underlying value: a
 * freestanding build has no names to print. Same type dispatch as
 * log_storage::append_any today, in one place so both forms agree about
 * what a value is.
 */
template <typename Type>
constexpr argument make_argument(Type && value)
{
    using bare = std::remove_cvref_t<Type>;

    if constexpr (std::is_array_v<bare>) {
        // A string literal is const char[N], not const char *, so it has
        // to decay before anything else can recognise it.
        static_assert(
            std::is_same_v<std::remove_cv_t<std::remove_extent_t<bare>>,
                           char>,
            "only character arrays can be logged");
        return {.text = static_cast<const char *>(value),
                .what = argument::kind::text};
    } else if constexpr (std::is_same_v<bare, bool>) {
        return {.value = value ? 1u : 0u, .what = argument::kind::boolean};
    } else if constexpr (std::is_enum_v<bare>) {
        return {.value = static_cast<std::uint64_t>(
                    static_cast<std::underlying_type_t<bare>>(value)),
                .what = argument::kind::unsigned_integer};
    } else if constexpr (std::is_same_v<bare, const char *> ||
                         std::is_same_v<bare, char *>) {
        return {.text = value, .what = argument::kind::text};
    } else if constexpr (std::is_pointer_v<bare>) {
        return {.value = reinterpret_cast<std::uint64_t>(value),
                .what = argument::kind::pointer};
    } else if constexpr (std::is_signed_v<bare>) {
        return {.value = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(value)),
                .what = argument::kind::signed_integer};
    } else if constexpr (std::is_integral_v<bare>) {
        return {.value = static_cast<std::uint64_t>(value),
                .what = argument::kind::unsigned_integer};
    } else {
        static_assert(false, "no way to log a value of this type");
    }
}

/**
 * What rendering produced. The truncation flag is reported rather than
 * hidden, because a line that was cut is a line that may have lost the
 * value it was written for.
 */
struct rendered
{
    std::size_t length{};
    bool truncated{};
};

/**
 * The text renderer.
 *
 * Writes into a caller-supplied buffer and stops at its end - no
 * allocation, no growth, no way to fault past it. A whole line is built
 * once, by the writer, and every text sink is handed the same span.
 *
 * A struct of static members rather than free functions, for the reason
 * zpp/trace.h gives: these are named from inside discarded `if constexpr`
 * branches, and an uncalled static free function is a -Werror failure
 * while an uncalled static member function is not.
 */
struct renderer
{
    static constexpr char * put(char * out, char * end, char character)
    {
        if (out < end) {
            *out++ = character;
        }
        return out;
    }

    static constexpr char * put_text(char * out,
                                     char * end,
                                     const char * text)
    {
        for (; *text && (out < end); ++text) {
            *out++ = *text;
        }
        return out;
    }

    static constexpr char * put_run(char * out,
                                    char * end,
                                    std::span<const char> run)
    {
        for (auto character : run) {
            if (out >= end) {
                break;
            }
            *out++ = character;
        }
        return out;
    }

    static constexpr char * put_decimal(char * out,
                                        char * end,
                                        std::uint64_t value)
    {
        char digits[20]{};
        std::size_t used{};
        do {
            digits[used++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value && (used < sizeof(digits)));
        while (used--) {
            out = put(out, end, digits[used]);
        }
        return out;
    }

    /**
     * Hex, 0x prefixed, shortest form - what a register or an MSR wants.
     */
    static constexpr char * put_hex(char * out,
                                    char * end,
                                    std::uint64_t value)
    {
        out = put_text(out, end, "0x");

        std::size_t digits = 1;
        for (auto shifted = value >> 4; shifted; shifted >>= 4) {
            ++digits;
        }
        for (auto i = digits; i-- > 0;) {
            auto nibble =
                static_cast<std::uint8_t>((value >> (i * 4)) & 0xf);
            out =
                put(out,
                    end,
                    static_cast<char>(nibble < 10 ? ('0' + nibble)
                                                  : ('a' + nibble - 10)));
        }
        return out;
    }

    static constexpr char * put_argument(char * out,
                                         char * end,
                                         const argument & value)
    {
        switch (value.what) {
        case argument::kind::boolean:
            return put_text(out, end, value.value ? "true" : "false");
        case argument::kind::text:
            return put_text(out, end, value.text ? value.text : "(null)");
        case argument::kind::signed_integer: {
            auto signed_value = static_cast<std::int64_t>(value.value);
            if (signed_value < 0) {
                out = put(out, end, '-');
                return put_hex(
                    out, end, static_cast<std::uint64_t>(-signed_value));
            }
            return put_hex(out, end, value.value);
        }
        case argument::kind::pointer:
        case argument::kind::unsigned_integer:
            return put_hex(out, end, value.value);
        }

        return out;
    }

    /**
     * Renders `file(line): text` with each placeholder replaced by the
     * next value.
     *
     * Values past the last placeholder are appended anyway: a value nobody
     * asked to print is still evidence. Placeholders past the last value
     * are left as they are, which is what a mismatch should look like.
     */
    static constexpr rendered line(const format_spec & format,
                                   std::span<const argument> values,
                                   std::span<char> out)
    {
        auto * cursor = out.data();
        auto * end = out.data() + out.size();

        if constexpr (!renders_text) {
            // Nothing in the spec to render from - the pointers are null
            // by construction in this configuration.
            static_cast<void>(format);
            static_cast<void>(values);
            return {};
        } else {
            cursor = put_text(cursor, end, format.file);
            cursor = put(cursor, end, '(');
            cursor = put_decimal(cursor, end, format.line);
            cursor = put_text(cursor, end, "): ");

            std::size_t position{};
            std::size_t placeholder{};
            for (auto & value : values) {
                if (placeholder < format.count) {
                    auto next = format.offsets[placeholder++];
                    cursor = put_run(
                        cursor,
                        end,
                        std::span<const char>(format.text + position,
                                              next - position));
                    position = next + 2;
                }
                cursor = put_argument(cursor, end, value);
            }

            cursor =
                put_run(cursor,
                        end,
                        std::span<const char>(format.text + position,
                                              format.length - position));

            return {.length =
                        static_cast<std::size_t>(cursor - out.data()),
                    .truncated = (cursor == end)};
        }
    }
};

/**
 * The formatting is constant expressions, so it is tested here rather than
 * only in an emulator - the same thing zpp/trace.h does, and the reason
 * that file's helpers were trustworthy enough to build the self check on.
 * @{
 */
static_assert([] {
    char buffer[64]{};
    auto * end =
        renderer::put_hex(buffer, buffer + sizeof(buffer) - 1, 0x5a70705a);
    *end = 0;
    const char expected[] = "0x5a70705a";
    for (std::size_t i{}; i < sizeof(expected); ++i) {
        if (buffer[i] != expected[i]) {
            return false;
        }
    }
    return true;
}());

static_assert([] {
    // Truncation stops at the buffer and reports itself rather than
    // writing past it.
    char buffer[4]{};
    auto * end = renderer::put_text(
        buffer, buffer + sizeof(buffer), "far too long");
    return end == (buffer + sizeof(buffer));
}());

/**
 * The location really is the call site's and not this header's, which is
 * the one property of the format_spec trick that would silently produce
 * useless logs if it were wrong. __LINE__ on the same line as the
 * construction is the only way to state it.
 */
static_assert(format_spec{"x"}.line == __LINE__);
static_assert(format_spec{"a {} b {}"}.count == 2);
static_assert(format_spec{"no placeholders"}.count == 0);
static_assert(!renders_text || (format_spec{"kept"}.text != nullptr));
static_assert(renders_text || (format_spec{"dropped"}.text == nullptr));
/**
 * @}
 */

} // namespace zpp::diag
