#pragma once
#include "zpp/containers.h"
#include "zpp/spin_lock.h"
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <type_traits>
#include <utility>

namespace zpp::hypervisor
{
/**
 * Storage for the hypervisor's own log.
 *
 * Separate from `log` below because that is a class template, and every
 * instantiation of it would otherwise get a ring of its own.
 *
 * The list is bounded and drops its oldest line rather than growing
 * without limit. The heap is a fixed arena and `operator new` traps on
 * exhaustion, since -fno-exceptions leaves nothing to throw - so an
 * unbounded log would eventually stop the machine, which is a poor way for
 * a diagnostic to behave.
 *
 * The lock is the list's, not the heap's. The heap has its own and is safe
 * to allocate from on several CPUs at once; a std::list is not safe to
 * push to from several CPUs at once whatever the allocator does, and any
 * CPU may log from its own VM exit handler.
 */
class log_storage
{
public:
    /**
     * The types the log is built out of.
     *
     * Named here, and used everywhere below, so that changing where the
     * log's memory comes from is a change to these two aliases and to this
     * class - and to nothing else. Giving each CPU its own heap, for
     * instance, means an allocator that selects per CPU and no change at
     * all to any call site, to `log`, or to the formatting.
     */
    using line = zpp::string;
    using line_list = zpp::list<line>;

    /**
     * How many lines are kept.
     */
    static constexpr std::size_t max_lines = 512;

    /**
     * Adds a line, dropping the oldest if the log is full.
     */
    static void append(line && text)
    {
        auto & lines = storage();

        m_lock.lock();
        lines.push_back(std::move(text));
        if (lines.size() > max_lines) {
            lines.pop_front();
        }
        m_lock.unlock();
    }

    /**
     * The lines, oldest first. For a debugger, and for anything that wants
     * to write the log somewhere durable.
     */
    static const line_list & lines()
    {
        return storage();
    }

    /**
     * Appends a null terminated string.
     */
    static void append_value(line & out, const char * text)
    {
        out.append(text);
    }

    /**
     * Appends a run of characters that is not null terminated, which is
     * what a slice of a format string between two placeholders is.
     */
    static void append_value(line & out, std::span<const char> text)
    {
        out.append(text.data(), text.size());
    }

    /**
     * Appends an unsigned value as 0x-prefixed hex, shortest form.
     */
    static void append_value(line & out, std::uint64_t value)
    {
        out.append("0x");

        std::size_t digits = 1;
        for (auto shifted = value >> 4; shifted; shifted >>= 4) {
            ++digits;
        }

        for (auto i = digits; i-- > 0;) {
            auto nibble =
                static_cast<std::uint8_t>((value >> (i * 4)) & 0xf);
            out.push_back(static_cast<char>(
                nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10)));
        }
    }

    /**
     * Appends whatever a caller passed as a value. Everything numeric
     * becomes hex, which is what register and MSR contents want, and
     * anything enumerated is logged as its underlying value, since a
     * freestanding build has no names to print.
     */
    template <typename Type>
    static void append_any(line & out, Type && value)
    {
        using bare = std::remove_cvref_t<Type>;

        if constexpr (std::is_array_v<bare>) {
            // A string literal is const char[N], not const char *, so it
            // has to decay before anything else can recognise it.
            static_assert(std::is_same_v<
                              std::remove_cv_t<std::remove_extent_t<bare>>,
                              char>,
                          "only character arrays can be logged");
            append_value(out, static_cast<const char *>(value));
        } else if constexpr (std::is_same_v<bare, bool>) {
            append_value(out, value ? "true" : "false");
        } else if constexpr (std::is_enum_v<bare>) {
            append_value(
                out,
                static_cast<std::uint64_t>(
                    static_cast<std::underlying_type_t<bare>>(value)));
        } else if constexpr (std::is_integral_v<bare>) {
            append_value(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<bare, const char *> ||
                             std::is_same_v<bare, char *>) {
            append_value(out, value);
        } else if constexpr (std::is_pointer_v<bare>) {
            append_value(out, reinterpret_cast<std::uint64_t>(value));
        } else {
            static_assert(false, "no way to log a value of this type");
        }
    }

    /**
     * Appends `file(line): ` for the call site. The file name is trimmed
     * to its last component, since source_location reports the path as the
     * compiler was given it, which in this build is absolute and long
     * enough to bury the message.
     */
    static void append_location(line & out, std::source_location location)
    {
        auto path = location.file_name();
        auto name = path;
        for (auto character = path; *character; ++character) {
            if (('/' == *character) || ('\\' == *character)) {
                name = character + 1;
            }
        }

        out.append(name);
        out.push_back('(');

        char digits[20]{};
        std::size_t written{};
        auto value = location.line();
        do {
            digits[written++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value && (written < sizeof(digits)));
        while (written--) {
            out.push_back(digits[written]);
        }

        out.append("): ");
    }

private:
    /**
     * The lines, constructed on first use.
     *
     * Deliberately not a namespace scope container. zpp::allocator's
     * default constructor calls crt::heap(), so such a container cannot be
     * constant initialized and would need an entry in .init_array - and
     * this was the first thing in the tree to ask for one, which is
     * exactly how the boot broke: the array walking in crt.cpp had only
     * ever run on an empty array.
     *
     * A function local static instead, for the same reason and with the
     * same safety argument as hypervisor::instance(): the build uses
     * -fno-threadsafe-statics, so there is no guard and the first call
     * must not race - and it does not, because the loader launches CPUs
     * strictly one at a time, and the boot CPU logs before any other CPU
     * exists.
     */
    static line_list & storage()
    {
        static line_list lines;
        return lines;
    }

    /**
     * Guards the list. Not recursive, and never held across anything that
     * can fault, so a stopped CPU cannot leave it held.
     */
    static inline zpp::spin_lock m_lock{};
};

/**
 * Reports a bad log format at compile time.
 *
 * Deliberately not constexpr: calling it from a constant expression makes
 * the evaluation fail, which is how a consteval function rejects its
 * argument in a build with -fno-exceptions, where `throw` is unavailable.
 * Defined rather than merely declared so no undefined symbol can appear if
 * anything ever reaches it outside constant evaluation.
 */
inline void log_format_error()
{
    __builtin_trap();
}

/**
 * A format string with its placeholders already located.
 *
 * The scan happens in a `consteval` constructor, so it happens while
 * compiling: a format string is always a literal at the call site, and
 * walking it again on every VM exit to find the same `{}` in the same
 * places would be work with a known answer. What reaches the constructor
 * below is the string plus the byte offsets of its placeholders, as
 * constant data.
 *
 * Implicitly constructed, so call sites still just pass a literal.
 */
struct format_spec
{
    /**
     * Placeholders allowed in one line. A line with more than this is a
     * compile error rather than a truncation, since it can only be a
     * mistake at this scale.
     */
    static constexpr std::size_t max_placeholders = 32;

    /**
     * Locates the placeholders. consteval rather than constexpr, so this
     * can never be reached at runtime by accident.
     */
    template <std::size_t Size>
    consteval format_spec(const char (&literal)[Size]) : text(literal)
    {
        for (std::size_t i{}; i < (Size - 1); ++i) {
            if (('{' == literal[i]) && ((i + 1) < (Size - 1)) &&
                ('}' == literal[i + 1])) {
                if (count == max_placeholders) {
                    // Not representable. This is a constant expression, so
                    // calling a function that is not constexpr makes it a
                    // compile error rather than a runtime surprise.
                    log_format_error();
                }
                offsets[count++] = i;
                ++i;
            }
        }

        length = Size - 1;
    }

    /**
     * The format string itself.
     */
    const char * text{};

    /**
     * Its length, excluding the terminator.
     */
    std::size_t length{};

    /**
     * How many placeholders it has.
     */
    std::size_t count{};

    /**
     * The byte offset of each placeholder's opening brace.
     */
    std::size_t offsets[max_placeholders]{};
};

/**
 * Writes a line to the hypervisor's log.
 *
 * The structured records in the hypervisor say what the state was; this
 * says what happened, in order, across CPUs. Both are needed: a record is
 * precise but only describes the last of its kind, and a boot failure is
 * usually a sequence.
 *
 *     log("start-up ipi, vector {}", vector);
 *
 * `{}` takes the next value, as in std::format - which is not available
 * here, since it is not part of the freestanding subset this builds
 * against. Values are logged as hex. The placeholders are located while
 * compiling, by format_spec above, so nothing scans the format string at
 * runtime.
 *
 * A class template with a deduction guide rather than a function, because
 * the source location has to be defaulted and therefore last, and a
 * function cannot put a parameter pack before it. Constructing one is what
 * makes the location work: a default argument is evaluated at the call
 * site, so a line records where it was written without being told.
 *
 * One caveat that comes with the shape: always pass a literal format
 * string first. `log(something);` with a single named argument is a
 * variable declaration, not a call - the most vexing parse - and it would
 * silently log nothing.
 */
template <typename... Types>
struct log
{
    log(format_spec format,
        Types &&... values,
        std::source_location location = std::source_location::current())
    {
        log_storage::line line;
        log_storage::append_location(line, location);

        // Walk the placeholders that were located at compile time rather
        // than looking for them again. The fold visits the values in
        // order, which is what keeps them matched to their placeholders.
        std::size_t position{};
        std::size_t placeholder{};
        auto emit = [&](auto && value) {
            if (placeholder >= format.count) {
                // More values than placeholders. Kept rather than
                // dropped - a value nobody asked to print is still
                // evidence.
                log_storage::append_any(
                    line, std::forward<decltype(value)>(value));
                return;
            }

            auto next = format.offsets[placeholder++];
            log_storage::append_value(
                line,
                std::span<const char>(format.text + position,
                                      next - position));
            position = next + 2;
            log_storage::append_any(line,
                                    std::forward<decltype(value)>(value));
        };
        (emit(std::forward<Types>(values)), ...);

        // Whatever follows the last placeholder consumed.
        log_storage::append_value(
            line,
            std::span<const char>(format.text + position,
                                  format.length - position));

        log_storage::append(std::move(line));
    }
};

/**
 * Deduces the pack from the values, so the source location can keep its
 * place at the end.
 */
template <typename Type, typename... Types>
log(Type, Types &&...) -> log<Types &&...>;

} // namespace zpp::hypervisor
