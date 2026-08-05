#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * The one place every diagnostic channel is configured.
 *
 * Everything about what is recorded, where it goes, and whether any of it
 * exists in this build is decided here. Adding a sink is three edits in
 * this file - an enumerator, a row in policy_of, and an entry in the
 * program's own zpp/diag/sinks.h - and nothing anywhere else. Turning one
 * off is one word.
 *
 * The whole facility hangs off a single macro, ZPP_DIAG, which the build
 * system always defines as 0 or 1 and forces to 0 in release. Per-sink
 * build system switches deliberately do not exist: enabling a sink also
 * means choosing its capacity and which categories it takes, which is an
 * edit here anyway, and the only thing CI ever flips is the whole
 * facility.
 *
 * Why an always-defined 0/1 macro turned into a constexpr bool, rather
 * than #ifdef - this is the pattern zpp/trace.h and zpp/verify.h already
 * use, and the reasons are worth stating because they are the reasons the
 * rest of this design works:
 *
 * - A disabled branch still has to parse and still has to resolve every
 *   name in it. Code behind #ifdef rots silently and fails the day someone
 *   flips the switch; code behind `if constexpr` cannot.
 * - `#ifdef ZPP_DAIG` is a typo that silently disables everything, while
 *   `= ZPP_DAIG` is an undeclared identifier. The preprocessor has no
 *   spell checker and no scope.
 * - A constant composes. The write path's severity floor below is a fold
 *   over every enabled sink's floor; the consistency checks are
 *   static_asserts. Neither is expressible in the preprocessor.
 * - Capacities are typed constants usable as array bounds and template
 *   arguments, which is what keeps the storage out of a disabled build -
 *   see ring.h.
 *
 * One constraint that comes with `if constexpr` and has already caught
 * this tree out: the names in a discarded branch are still looked up, and
 * an uncalled static *free* function draws -Wunused-function, which
 * -Werror turns into a build failure. An uncalled static *member* function
 * does not. That is why every sink here is a struct of static members
 * rather than a namespace of free functions.
 */
namespace zpp::diag
{
/**
 * Whether this build carries any of it. Forced to 0 in release by
 * cmake/hypervisor/CMakeLists.txt rather than merely defaulted there, so a
 * stale -DZPP_DIAG=ON left in a CMake cache cannot put it into a release
 * binary. That trap is real: BACKLOG.md records a whole class of failure
 * caused by ZPP_VERIFY_HYPERVISOR persisting in a cache unnoticed.
 */
inline constexpr bool enabled = ZPP_DIAG;

/**
 * How much a line is worth saying. Ordered, and compared with at_least
 * below rather than with `>=` on the enumerators, so the ordering is
 * stated in one place.
 */
enum class severity : std::uint8_t
{
    trace,
    info,
    warning,
    error,
};

/**
 * What a line is about. A sink takes a mask of these, so a high volume
 * channel can carry the exit stream while a slow one carries only the
 * things a person reads.
 *
 * Deliberately coarse. A category per source file would be a taxonomy to
 * maintain; these are the questions this hypervisor is actually debugged
 * against.
 */
enum class category : std::uint8_t
{
    general,
    boot,
    memory,
    guest,
    exits,
    apic,
    loader,
    count,
};

/**
 * A set of categories. A mask rather than a list, so the union across
 * sinks below is one `or`.
 */
using category_mask = std::uint32_t;

/**
 * The mask holding one category, and the mask holding all of them.
 * @{
 */
constexpr category_mask only(category which)
{
    return category_mask{1} << static_cast<std::uint8_t>(which);
}

constexpr category_mask all_categories =
    (category_mask{1} << static_cast<std::uint8_t>(category::count)) - 1;
/**
 * @}
 */

/**
 * Whether the first severity is at least the second.
 */
constexpr bool at_least(severity value, severity floor)
{
    return static_cast<std::uint8_t>(value) >=
           static_cast<std::uint8_t>(floor);
}

/**
 * Which form a sink is handed a record in.
 *
 * text is the rendered line, as a span of characters - what a screen or a
 * console wants. binary is the packed record - an event id standing in for
 * the format string plus the argument words - which is what a high volume
 * stream or a disk extent wants, and what a host side decoder turns back
 * into text using the event id database emitted at build time.
 *
 * A sink says which one it takes and is handed exactly that. Both forms
 * are produced at most once per line, by the writer, and only if some
 * enabled sink asked for that form - see log.h.
 */
enum class form : std::uint8_t
{
    text,
    binary,
};

/**
 * Every channel this facility knows how to drive.
 *
 * The enumerators exist whether or not the sink is implemented yet, so
 * that the configuration is the full picture of what the design has to
 * accommodate rather than only of what is written. An enumerator with no
 * row in policy_of is a compile error (the switch has no default and the
 * function has to return), and an enabled one with no implementation in
 * the program's zpp/diag/sinks.h is a static_assert in pump.h.
 */
enum class sink : std::uint8_t
{
    /**
     * Counts what it drains and nothing else. Sounds useless and is not:
     * it answers "is the write path being reached at all" from a debugger
     * with no device, no framebuffer and no serial port, which is the
     * state the development target is actually in.
     */
    counter,

    /**
     * The framebuffer, drawn from the halt paths. Already written and
     * verified under emulation on feat/screen-halt-output.
     */
    framebuffer,

    /**
     * A USB debug channel.
     */
    usb_debug,

    /**
     * The EFI system partition, as a file, through boot services. Loader
     * side only - there are no boot services once the guest is running.
     */
    esp_file,

    /**
     * The EFI system partition, as raw blocks, through a driver of our
     * own. The resident side's only route to a disk.
     */
    esp_blocks,

    /**
     * The firmware console. Loader side only, for the same reason as
     * esp_file.
     */
    firmware_console,

    /**
     * A 16550 at 0x3f8. The channel the loader already uses.
     */
    serial,

    count,
};

/**
 * What one sink is configured to do.
 *
 * A flat struct of constants rather than a class, because the whole point
 * is that the table below can be read as a table.
 */
struct policy
{
    /**
     * Whether this sink exists in this build. Every use of it is inside an
     * `if constexpr` on this, so a false here leaves no code, no storage
     * and no strings - see the note on capacity below.
     */
    bool present{};

    /**
     * The least severity this sink carries.
     */
    severity floor{severity::trace};

    /**
     * Which categories it carries.
     */
    category_mask categories{all_categories};

    /**
     * Which form it is handed.
     */
    form shape{form::text};

    /**
     * How much of whatever this sink counts in - lines, bytes, blocks.
     * Meaningful per sink, and passed as a template argument wherever it
     * sizes storage, which is what makes the storage vanish with the sink:
     * a static data member of a class template is only emitted if the
     * template is instantiated, and a disabled sink's is never named
     * outside a discarded branch.
     */
    std::uint32_t capacity{};

    /**
     * How many consecutive failures make this sink dead.
     *
     * A wedged device must not be retried out of every VM exit for the
     * rest of the boot. After this many failures the pump stops offering
     * records to this sink entirely and says so in its counters. Zero
     * means never give up, which is only correct for a sink that cannot
     * fail.
     */
    std::uint32_t failures_tolerated{8};
};

/**
 * The table. This is the file's reason to exist.
 *
 * A switch rather than an array so that adding an enumerator without a
 * policy is a build failure: there is no default label and the function
 * has to return a value, so -Wreturn-type under -Werror rejects it. The
 * same trick already keeps zpp::error's categories honest.
 */
constexpr policy policy_of(sink which)
{
    switch (which) {
    case sink::counter:
        // Cannot fail, so it never dies. Carries everything, because what
        // it is for is proving the write path ran.
        return {.present = enabled,
                .floor = severity::trace,
                .categories = all_categories,
                .shape = form::binary,
                .capacity = 0,
                .failures_tolerated = 0};

    case sink::framebuffer:
        // Text, and only what a person reads off a screen during a failed
        // boot: the exit stream would scroll the interesting part away.
        return {.present = false,
                .floor = severity::info,
                .categories = all_categories & ~only(category::exits),
                .shape = form::text,
                .capacity = 32,
                .failures_tolerated = 0};

    case sink::usb_debug:
        return {.present = false,
                .floor = severity::trace,
                .categories = all_categories,
                .shape = form::binary,
                .capacity = 4096,
                .failures_tolerated = 8};

    case sink::esp_file:
        return {.present = false,
                .floor = severity::trace,
                .categories = all_categories,
                .shape = form::text,
                .capacity = 0x8000,
                .failures_tolerated = 1};

    case sink::esp_blocks:
        return {.present = false,
                .floor = severity::trace,
                .categories = all_categories,
                .shape = form::binary,
                .capacity = 2048,
                .failures_tolerated = 4};

    case sink::firmware_console:
        return {.present = false,
                .floor = severity::info,
                .categories = all_categories,
                .shape = form::text,
                .capacity = 0,
                .failures_tolerated = 1};

    case sink::serial:
        return {.present = false,
                .floor = severity::trace,
                .categories = all_categories,
                .shape = form::text,
                .capacity = 0,
                .failures_tolerated = 16};

    case sink::count:
        return {};
    }

    return {};
}

/**
 * The retention ring's shape.
 *
 * Not a sink. Every sink reads out of it - see the model note in log.h -
 * so this is the one storage decision the whole facility shares, and it is
 * the only diagnostic memory that exists.
 * @{
 */
struct ring_configuration
{
    /**
     * How many processors have a ring of their own. Per processor and not
     * shared, so the write path takes no lock at all: a lock a stopped
     * processor could be holding is the one thing a VM exit handler must
     * never wait on.
     */
    std::size_t processors{};

    /**
     * How many records each processor retains, and how wide one is. Both
     * powers of two, so the index arithmetic is a mask.
     */
    std::size_t records{};
    std::size_t record_size{};
};

inline constexpr ring_configuration ring{
    .processors = 8, .records = 256, .record_size = 128};
/**
 * @}
 */

/**
 * Whether any enabled sink wants each form, and the write path's floor and
 * category set - the union of what the enabled sinks between them ask for.
 *
 * This is what makes per-severity and per-category selection free rather
 * than merely cheap: a call whose severity or category no enabled sink
 * carries is discarded at the call site, arguments and format string
 * included. Derived here rather than stated, so the two cannot disagree.
 * @{
 */
constexpr bool any_sink_of(form shape)
{
    for (std::uint8_t i{}; i < static_cast<std::uint8_t>(sink::count);
         ++i) {
        auto sink_policy = policy_of(static_cast<sink>(i));
        if (sink_policy.present && (shape == sink_policy.shape)) {
            return true;
        }
    }
    return false;
}

constexpr bool any_sink()
{
    return any_sink_of(form::text) || any_sink_of(form::binary);
}

constexpr severity write_floor()
{
    auto floor = severity::error;
    for (std::uint8_t i{}; i < static_cast<std::uint8_t>(sink::count);
         ++i) {
        auto sink_policy = policy_of(static_cast<sink>(i));
        if (sink_policy.present && at_least(floor, sink_policy.floor)) {
            floor = sink_policy.floor;
        }
    }
    return floor;
}

constexpr category_mask write_categories()
{
    category_mask mask{};
    for (std::uint8_t i{}; i < static_cast<std::uint8_t>(sink::count);
         ++i) {
        auto sink_policy = policy_of(static_cast<sink>(i));
        if (sink_policy.present) {
            mask |= sink_policy.categories;
        }
    }
    return mask;
}

/**
 * Whether a line with this severity and category is worth recording at
 * all. The single test the call site makes.
 */
constexpr bool records(severity level, category which)
{
    return enabled && any_sink() && at_least(level, write_floor()) &&
           (0 != (write_categories() & only(which)));
}
/**
 * @}
 */

/**
 * Whether text is produced at all, and therefore whether a format string
 * survives into the binary. Every string in this facility is referenced
 * from exactly one place - the renderer in format.h - so this constant is
 * the whole answer to "is the format string in .rodata".
 */
inline constexpr bool renders_text = enabled && any_sink_of(form::text);

/**
 * Whether records are packed. Owned by the record format, which is where
 * the event id comes from; this only says whether anything asks for one.
 */
inline constexpr bool packs_records = enabled && any_sink_of(form::binary);

/**
 * Consistency checks. These are the reason the table is constants and not
 * preprocessor lines.
 * @{
 */
static_assert(!enabled || (0 != ring.processors),
              "an enabled facility needs somewhere to keep records");
static_assert(0 == (ring.records & (ring.records - 1)),
              "the record count is masked, so it has to be a power of 2");
static_assert(0 == (ring.record_size & (ring.record_size - 1)),
              "the record size is masked, so it has to be a power of 2");
static_assert(enabled || !any_sink(),
              "a sink is present in a build with the facility disabled");
static_assert(static_cast<std::uint8_t>(category::count) <=
                  (8 * sizeof(category_mask)),
              "more categories than the mask can hold");
/**
 * @}
 */

/**
 * The two things the shared code needs from whichever program it is
 * compiled into, and the only two. Declared here, defined once per program
 * - the hypervisor from its exit handler's own knowledge, the loader as
 * constants.
 *
 * Neither is called from a disabled build, so neither has to exist in one;
 * both are called from inside an `if constexpr` on `enabled`.
 * @{
 */
/**
 * Which processor is executing, as an index below ring.processors.
 *
 * The hypervisor should answer this from the per-processor block its host
 * GS base points at, which is one load and is what an operating system
 * does. Reading the x2APIC id with rdmsr is the zero-dependency interim
 * answer and costs about a hundred cycles. What it must not be is anything
 * that takes a lock or a VMCS read - it runs once per recorded line.
 */
std::size_t current_processor();

/**
 * A monotonic tick, for putting the processors' rings back into one order
 * when they are read. The time stamp counter is the intended source: it is
 * invariant across cores on anything this runs on, and no shared counter
 * means the write path has nothing to contend on.
 */
std::uint64_t timestamp();
/**
 * @}
 */

} // namespace zpp::diag
