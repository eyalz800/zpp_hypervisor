#pragma once
#include "zpp/containers.h"
#include "zpp/crash_log.h"
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

        // Through to memory that outlives this boot, before the line is
        // moved into the list and its bytes stop being ours to read.
        //
        // Write-through rather than flushed on demand, and that is the
        // only strategy that survives the failure this exists for. A flush
        // needs a moment to happen at, and the failures with no other
        // diagnosis channel are precisely the ones that provide no such
        // moment: a guest that hangs runs no more of our code, a processor
        // stopped in on_unhandled_exit's halt loop has already stopped,
        // and a triple fault resets the machine without asking. Anything
        // held back for a flush is lost in exactly the cases it was
        // collected for.
        //
        // Affordable because of where it sits. This is inside the lock the
        // list already needs, so it introduces no second lock and no way
        // to deadlock - which matters, because zpp::spin_lock is not
        // recursive and this path is reachable from every VM exit handler.
        // The work itself is two memcpy calls and a header update,
        // allocates nothing, and cannot fail.
        m_crash_log.append_line(
            std::span<const char>{text.data(), text.size()});

        lines.push_back(std::move(text));
        if (lines.size() > max_lines) {
            lines.pop_front();
        }
        m_lock.unlock();
    }

    /**
     * Points the log at memory that outlives the boot, or at nothing.
     *
     * Here rather than anywhere else for the reason the aliases above
     * give: where this log's memory comes from is settled in this class
     * alone. An unattached log is the normal state on every platform whose
     * loader does not reserve a region, and costs one comparison per line.
     *
     * Under the lock, because a line may be being written on another
     * processor. In practice the boot processor calls this before any
     * other processor exists, but the cost of being right about it is nil.
     */
    static void attach(zpp::crash_log log)
    {
        m_lock.lock();
        m_crash_log = log;
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
    static line_list & storage()
    {
        return m_lines;
    }

    /**
     * The lines.
     *
     * zpp::allocator's default constructor calls crt::heap(), so this
     * cannot be constant initialized and needs an entry in .init_array.
     * That is deliberate rather than merely tolerated: it is the entry
     * crt::init::main() runs on the boot CPU before anything else, so by
     * the time a second CPU exists the list is already built. A function
     * local static would instead be initialized by whichever CPU logged
     * first, and with -fno-threadsafe-statics there is no guard to make
     * that safe if two ever race - and log() is reachable from every VM
     * exit handler.
     *
     * This was also the tree's first .init_array entry, and it is what
     * exposed the relocation bug in elf_file::relocate that made dynamic
     * initialization look unusable. See the notes there.
     */
    static inline line_list m_lines{};

    /**
     * Memory that outlives the boot, or nothing.
     *
     * Constant initialized, so this costs no .init_array entry of its own
     * - an unattached log is a pair of null span members and needs no
     * constructor to run. Whether it is attached is decided once, by the
     * boot processor, out of what the loader reserved.
     */
    static constinit inline zpp::crash_log m_crash_log{};

    /**
     * Guards the list, and the region above with it. Not recursive, and
     * never held across anything that can fault, so a stopped CPU cannot
     * leave it held.
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
