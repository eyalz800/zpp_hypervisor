#pragma once
#include "zpp/diag/config.h"
#include "zpp/diag/format.h"
#include "zpp/diag/ring.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace zpp::diag
{
/**
 * Packing a line into a binary record.
 *
 * Owned by the record format, like event_id in format.h, and stubbed here
 * for the same reason - so this header compiles on its own. What this
 * architecture needs from the real one is only this: a function that takes
 * the spec and the arguments and fills a byte span, never touching the
 * format string. Everything else about the layout is theirs.
 */
struct packer
{
    static std::size_t pack(const format_spec & format,
                            std::span<const argument> values,
                            std::span<std::uint8_t> out)
    {
        std::size_t used{};

        auto put_word = [&](std::uint64_t word) {
            for (std::size_t byte{}; byte < sizeof(word); ++byte) {
                if (used < out.size()) {
                    out[used++] =
                        static_cast<std::uint8_t>(word >> (byte * 8));
                }
            }
        };

        put_word(format.event);
        for (auto & value : values) {
            // A text argument is copied, not referred to: a retained
            // record must hold no pointers. See the note on record.
            if (argument::kind::text == value.what) {
                for (auto * character = value.text;
                     character && *character;
                     ++character) {
                    if (used < out.size()) {
                        out[used++] =
                            static_cast<std::uint8_t>(*character);
                    }
                }
                if (used < out.size()) {
                    out[used++] = 0;
                }
                continue;
            }
            put_word(value.value);
        }

        return used;
    }
};

/**
 * The write path.
 *
 * Records go into the ring and nowhere else. No sink is called from here,
 * no device is touched from here, nothing here can fail and nothing here
 * can wait. Every sink is a reader with a cursor of its own, drained by
 * the pump - see pump.h for what drives it and why that is the only shape
 * that survives a wedged sink.
 *
 * What this costs at a call site, in a build with it enabled: one argument
 * struct per value built in stack slots, one call. The renderer, the
 * packer and the ring are reached through this one out-of-line function
 * per severity and category pair, not instantiated per argument list, so
 * turning the whole exit handler's tracing on does not double its code.
 */
template <severity Severity, category Category>
void emit(const format_spec & format, std::span<const argument> values)
{
    if constexpr (!records(Severity, Category)) {
        static_cast<void>(format);
        static_cast<void>(values);
        return;
    } else {
        // Never trust the hook: an out of range index would write into
        // another processor's ring or past the end of it, and this runs
        // where there is nothing to catch a fault.
        auto processor = current_processor() % ring.processors;
        auto when = timestamp();

        record built{};
        built.when = when;
        built.event = format.event;
        built.level = Severity;
        built.which = Category;

        // Each form is produced at most once, and only if some enabled
        // sink asked for it. A build whose only sinks are binary never
        // reaches the renderer, which is what keeps the format string out
        // of the binary entirely - format_spec::text is null there.
        if constexpr (renders_text) {
            built.shape = form::text;
            auto text = renderer::line(
                format,
                values,
                std::span<char>(reinterpret_cast<char *>(built.payload),
                                record::payload_capacity));
            built.length = static_cast<std::uint16_t>(text.length);
            built.truncated = text.truncated;
            ring_writer::append(processor, built);
        }

        if constexpr (packs_records) {
            built.shape = form::binary;
            built.truncated = false;
            built.length = static_cast<std::uint16_t>(packer::pack(
                format,
                values,
                std::span<std::uint8_t>(built.payload,
                                        record::payload_capacity)));
            ring_writer::append(processor, built);
        }
    }
}

/**
 * Writes a line.
 *
 *     log("cpu {} exit {}", cpu, reason);
 *     log<severity::error, category::guest>("stopping, reason {}", why);
 *
 * `{}` takes the next value, as in std::format - which is not part of the
 * freestanding subset this builds against. Values are rendered as hex. The
 * placeholders are located while compiling and so is the call site: the
 * location arrives inside the format_spec, which is why this can be an
 * ordinary function template. The class-template-with-a-deduction-guide
 * shape that zpp/hypervisor/log.h needs today, and its most vexing parse
 * caveat, are both gone.
 *
 * Two things a caller has to know:
 *
 * - It cannot fail and it reports nothing. A diagnostic that hands a
 *   failure back to a VM exit handler has created a second failure path in
 *   the one place that has nowhere to put one. Losses are counted where
 *   they happen and read out with the log; see cursor.
 * - Arguments are evaluated even when nothing is enabled. C++ has no way
 *   to elide a function argument. A logged `vmcs.guest_rip()` in a build
 *   with the facility off still performs the vmread: the wrappers in
 *   zpp/arch/x86_64 are asm volatile, so the optimiser may not remove
 *   them. Keep log arguments pure. Where a value is expensive, guard the
 *   call with `if constexpr (diag::records(...))` and pass the value in.
 *   A macro would fix this by construction; it is not worth the macro.
 */
template <severity Severity = severity::info,
          category Category = category::general,
          typename... Types>
void log(const format_spec & format, Types &&... values)
{
    if constexpr (!records(Severity, Category)) {
        // The format string is not referenced here, so it is not emitted
        // anywhere: format_spec's consteval constructor keeps no pointer
        // to it in this configuration.
        static_cast<void>(format);
        (static_cast<void>(values), ...);
        return;
    } else if constexpr (0 == sizeof...(Types)) {
        emit<Severity, Category>(format, {});
    } else {
        static_assert(sizeof...(Types) <= format_spec::max_placeholders,
                      "more values than a line can carry");
        const argument built[]{
            make_argument(std::forward<Types>(values))...};
        emit<Severity, Category>(format, built);
    }
}

} // namespace zpp::diag
