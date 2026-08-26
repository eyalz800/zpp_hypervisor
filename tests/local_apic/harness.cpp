/*
 * The interrupt command register, decoded by the real code.
 *
 * `on_interrupt_command` is the one function that decides what a guest's
 * IPI does: whether it goes out as written, whether this VMM starts the
 * target in its own trampoline, or whether it is refused and the
 * processor handed to the guest unvirtualized. Nothing tested it, and
 * three separate defects have been fixed inside it - each one a misread
 * of a field the SDM describes in a single table.
 *
 * The three, and each is a case below:
 *
 * - `c6349a4`: the destination was taken as `command >> 32` with no test
 *   of bit 11. In logical destination mode that field is an eight-bit
 *   message destination address, not an APIC id, and matching it against
 *   the identifier table then made `processor_slot` *allocate a slot* for
 *   a processor that does not exist - handing the rest of the function a
 *   wrong index.
 * - `987441f`: a broadcast start-up IPI was passed through because this
 *   VMM "could not enumerate what it would be broadcasting to". True, and
 *   the conclusion was still wrong: a broadcast is how Windows starts its
 *   processors, so the case being punted on was the only one that ever
 *   happens.
 * - `439abb5`: flagging INIT targets, reverted twice over - a flag
 *   carries no ordering against the start-up IPI that follows, and it was
 *   being set for INIT level de-assert, which starts nothing at all.
 *
 * The register's layout is SDM Vol. 3A Figure 13-12, "Interrupt Command
 * Register (ICR)", .references/sdm.txt:171196, with the field meanings in
 * the table beginning at :171246. Every constant below is spelled out
 * from that figure rather than taken from hypervisor.cpp, for the reason
 * verify_nested.h gives for doing the same: a probe that reads the
 * expected answer out of the source the answer was written from tests
 * nothing.
 */
#include "zpp/hypervisor/hypervisor.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

/**
 * What this harness records about the calls the decode makes into the
 * machine, and the two answers it makes those calls give back.
 *
 * Namespace scope, because they are the harness's and not the
 * hypervisor's. They were members of a stand-in class that is gone.
 *
 * The stand-in also carried a `log` that recorded format strings, with
 * `log_count` and `log_at` to read them back. Neither was ever called -
 * only `log_reset` was - so the real `zpp::hypervisor::log` replaces it
 * and nothing is asserted that was not asserted before. What the decode
 * *says* is still readable, through `log_storage::lines()`, and by the
 * real formatter rather than by a recorder that kept the format string
 * and dropped the values.
 */
struct observations
{
    /**
     * One start-up attempt: which processor, and at what vector.
     */
    struct attempt
    {
        std::uint64_t destination{};
        std::uint64_t vector{};
    };

    static constexpr std::size_t attempt_capacity = 64;

    attempt start_up_attempts[attempt_capacity]{};
    std::size_t start_up_attempt_count{};

    attempt hardware_ipis[attempt_capacity]{};
    std::size_t hardware_ipi_count{};

    /**
     * What `note_apic_mode` asked of the page watch, which is half of
     * what it decides - the other half is the bit in the MSR bitmap.
     */
    std::uint64_t watch_calls{};
    bool watch_last{};

    /**
     * What `local_apic_id` answers, and which destinations
     * `start_up_processor` refuses to adopt. Anything not named answers
     * `adopted`.
     */
    std::uint64_t self_apic_id{};
    std::uint64_t needs_hardware_for[attempt_capacity]{};
    std::size_t needs_hardware_count{};
};

static observations g_observed;

namespace zpp::hypervisor
{
/*
 * The three functions the decode calls that reach hardware in the real
 * VMM. Each records what it was asked for, which is what makes the
 * decode's decisions observable at all: "adopted" and "passed through"
 * differ only in whether a start-up IPI reached a processor.
 */
std::uint64_t hypervisor::local_apic_id()
{
    return g_observed.self_apic_id;
}

hypervisor::start_up_result hypervisor::start_up_processor(
    std::uint64_t destination, std::uint64_t vector)
{
    if (g_observed.start_up_attempt_count <
        (sizeof(g_observed.start_up_attempts) /
         sizeof(g_observed.start_up_attempts[0]))) {
        g_observed.start_up_attempts[g_observed.start_up_attempt_count++] =
            observations::attempt{destination, vector};
    }

    for (std::size_t i{}; i < g_observed.needs_hardware_count; ++i) {
        if (g_observed.needs_hardware_for[i] == destination) {
            return start_up_result::needs_hardware;
        }
    }

    return start_up_result::adopted;
}

void hypervisor::send_start_up_ipi(std::uint64_t apic,
                                   std::uint64_t vector)
{
    if (g_observed.hardware_ipi_count <
        (sizeof(g_observed.hardware_ipis) /
         sizeof(g_observed.hardware_ipis[0]))) {
        g_observed.hardware_ipis[g_observed.hardware_ipi_count++] =
            observations::attempt{apic, vector};
    }
}

/*
 * The page watch, reduced to a recorder.
 *
 * Not cut out of local_apic.cpp with the two beside it: the real one
 * lives in local_apic_write.cpp, which this harness does not build - it
 * reaches the extended page tables through `watch_guest_page_writes`,
 * and none of that is what `note_apic_mode` is being asked about here.
 * What `watch_local_apic` decides on its own - refusing a relocated
 * page - is checked by scripts/ci/check-exit-handler.sh against the
 * source.
 */
void hypervisor::watch_local_apic(bool watch)
{
    g_observed.watch_calls += 1;
    g_observed.watch_last = watch;
}

/**
 * The cached VMX capability MSRs, reached only by `monitor_trap_flag`,
 * which this harness compiles out of local_apic.cpp and asks nothing of.
 * One slot, shared by every index: nothing here reads it back.
 */
std::uint64_t & hypervisor::cached_vmx_msr(std::size_t)
{
    static std::uint64_t unused{};
    return unused;
}

} // namespace zpp::hypervisor

namespace zpp::arch::x86_64
{
namespace
{
/**
 * IA32_APIC_BASE as the processor running `note_apic_mode` reads it.
 *
 * Thread-local, because a processor reads *its own*. The concurrency
 * case below has half its processors in one mode and half in another at
 * the same moment, and a single global here would make the harness the
 * thing that races rather than the code under test.
 */
thread_local std::uint64_t g_apic_base{};
} // namespace

std::uint64_t rdmsr(std::uint32_t index)
{
    // The only model-specific register this path reads. Trapping rather
    // than answering anything else keeps a future caller from being
    // silently handed an APIC base for something else entirely.
    if (msr::ia32_apic_base != index) {
        __builtin_trap();
    }
    return g_apic_base;
}

void wrmsr(std::uint32_t, std::uint64_t)
{
    __builtin_trap();
}

} // namespace zpp::arch::x86_64

namespace
{
std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}", what);
}

void check_equal(std::uint64_t expected,
                 std::uint64_t actual,
                 const std::string & what)
{
    ++g_checks;
    if (expected == actual) {
        return;
    }
    ++g_failures;
    std::println("FAIL: {}\n  expected 0x{:x}\n  actual   0x{:x}",
                 what,
                 expected,
                 actual);
}

/*
 * The ICR, built field by field from SDM Figure 13-12
 * (.references/sdm.txt:171196). Written as a builder rather than as hex
 * literals so a case says which field it is about.
 */
struct icr
{
    std::uint64_t vector{};
    std::uint64_t delivery_mode{};
    bool logical_destination{};
    bool level_assert{true};
    bool trigger_level{};
    std::uint64_t shorthand{};
    std::uint64_t destination{};

    std::uint64_t value() const
    {
        return (vector & 0xff) | ((delivery_mode & 0x7) << 8) |
               (logical_destination ? (1ull << 11) : 0) |
               (level_assert ? (1ull << 14) : 0) |
               (trigger_level ? (1ull << 15) : 0) |
               ((shorthand & 0x3) << 18) | (destination << 32);
    }
};

/*
 * The delivery modes, SDM Figure 13-12's own table
 * (.references/sdm.txt:171200-171211).
 */
constexpr std::uint64_t delivery_fixed = 0;
constexpr std::uint64_t delivery_lowest_priority = 1;
constexpr std::uint64_t delivery_smi = 2;
constexpr std::uint64_t delivery_reserved_3 = 3;
constexpr std::uint64_t delivery_nmi = 4;
constexpr std::uint64_t delivery_init = 5;
constexpr std::uint64_t delivery_start_up = 6;
constexpr std::uint64_t delivery_reserved_7 = 7;

/*
 * The destination shorthands, same figure
 * (.references/sdm.txt:171280-171296).
 */
constexpr std::uint64_t shorthand_none = 0;
constexpr std::uint64_t shorthand_self = 1;
constexpr std::uint64_t shorthand_all_including_self = 2;
constexpr std::uint64_t shorthand_all_excluding_self = 3;

/**
 * A hypervisor with a roster and a known boot processor.
 *
 * `number_of_known_processors` starts at 1 because the real one does:
 * slot 0 is the boot processor, which is running this code.
 */
void configure(zpp::hypervisor::hypervisor & state,
               std::uint64_t self,
               std::initializer_list<std::uint32_t> roster)
{
    // The observations are namespace scope rather than members of a
    // stand-in class, so a fresh machine has to clear them explicitly -
    // a new hypervisor object no longer does it by construction.
    g_observed = observations{};

    g_observed.self_apic_id = self;
    state.apic_id[0] = self;
    state.number_of_known_processors = 1;

    std::size_t i{};
    for (auto id : roster) {
        state.platform_apic_id[i++] = id;
    }
    state.number_of_platform_processors = roster.size();
}

/**
 * A configured hypervisor, held by the caller.
 *
 * Returned through a unique_ptr rather than by value for two reasons,
 * and the second arrived with the real class: it holds a
 * `zpp::spin_lock`, which is deliberately not copyable, and it is
 * 46,383,104 bytes, which is not a thing to put on a stack.
 */
std::unique_ptr<zpp::hypervisor::hypervisor>
make(std::uint64_t self = 0,
     std::initializer_list<std::uint32_t> roster = {})
{
    auto state = std::make_unique<zpp::hypervisor::hypervisor>();
    configure(*state, self, roster);
    return state;
}

// === Delivery modes that are none of this VMM's business ==============
//
// SDM Figure 13-12's delivery mode table: 000 fixed, 001 lowest
// priority, 010 SMI, 100 NMI. None of them starts a processor, so all of
// them must come back exactly as the guest wrote them, with no slot
// allocated and nothing started.
//
// The two reserved encodings, 011 and 111, are here too. They are not
// something a guest should send, and precisely because of that they are
// the ones a decode is most likely to fall through into a case it did
// not mean.
void passthrough_delivery_modes()
{
    struct
    {
        std::uint64_t mode;
        const char * name;
    } modes[]{
        {delivery_fixed, "fixed"},
        {delivery_lowest_priority, "lowest priority"},
        {delivery_smi, "smi"},
        {delivery_reserved_3, "reserved 011"},
        {delivery_nmi, "nmi"},
        {delivery_reserved_7, "reserved 111"},
    };

    for (auto & entry : modes) {
        auto state = make(0, {0, 1, 2, 3});
        auto command = icr{.vector = 0x40,
                           .delivery_mode = entry.mode,
                           .destination = 1}
                           .value();

        auto answer = state->on_interrupt_command(command);

        check(answer.has_value(),
              std::string("delivery mode ") + entry.name +
                  " is passed through");
        if (answer) {
            check_equal(command,
                        *answer,
                        std::string("delivery mode ") + entry.name +
                            " is passed through unmodified");
        }
        check_equal(0,
                    g_observed.start_up_attempt_count,
                    std::string("delivery mode ") + entry.name +
                        " starts nothing");
        check_equal(1,
                    state->number_of_known_processors,
                    std::string("delivery mode ") + entry.name +
                        " allocates no slot");
        check_equal(0,
                    state->ipi_start_up_seen,
                    std::string("delivery mode ") + entry.name +
                        " is not counted as a start-up ipi");
        check_equal(command,
                    state->ipi_last_command,
                    std::string("delivery mode ") + entry.name +
                        " is recorded as the last command");
    }
}

// === INIT ==============================================================
//
// Counted, logged, and passed through with nothing else done. SDM Figure
// 13-12: "101 (INIT) Delivers an INIT request to the target processor or
// processors" (.references/sdm.txt:171246). Leaving the target in
// wait-for-SIPI is the state the start-up IPI path needs it in, so
// forwarding it is not a fallback - it is the whole of what this VMM
// wants to happen.
void init_is_forwarded_and_counted()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command =
        icr{.delivery_mode = delivery_init, .destination = 1}.value();

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(), "init is passed through");
    if (answer) {
        check_equal(command, *answer, "init is passed through unmodified");
    }
    check_equal(1, state->ipi_init_seen, "init is counted");
    check_equal(
        0, g_observed.start_up_attempt_count, "init starts nothing");
    check_equal(
        1, state->number_of_known_processors, "init allocates no slot");
    check_equal(
        0, g_observed.hardware_ipi_count, "init sends no hardware ipi");
}

/**
 * INIT level de-assert, which starts nothing at all.
 *
 * SDM Figure 13-12, "101 (INIT Level De-assert)": "for this delivery mode
 * the level flag must be set to 0 and trigger mode flag to 1"
 * (.references/sdm.txt:171249). The command the rig recorded is
 * 0x100008500 - delivery mode 101, level clear, trigger mode level, and
 * destination shorthand 01.
 *
 * `439abb5` reverted a change that flagged its targets for an INIT they
 * had not been sent. This asserts the surviving behaviour: it is INIT as
 * far as the counter is concerned, and it does not start, adopt or flag
 * anything.
 */
void init_level_de_assert_starts_nothing()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.delivery_mode = delivery_init,
                       .level_assert = false,
                       .trigger_level = true,
                       .shorthand = shorthand_none,
                       .destination = 1}
                       .value();

    // Reconstructed field by field and then checked against the value the
    // rig actually recorded, so the builder above is validated by a real
    // command rather than only by itself. Delivery mode 101, level clear,
    // trigger mode level, no shorthand, destination 1.
    check_equal(0x100008500ull,
                command,
                "the level de-assert command is the one the rig recorded");

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(), "init level de-assert is passed through");
    check_equal(0,
                g_observed.start_up_attempt_count,
                "init level de-assert starts nothing");
    check_equal(0,
                g_observed.hardware_ipi_count,
                "init level de-assert sends no hardware ipi");
    for (std::size_t i{}; i < zpp::hypervisor::hypervisor::max_cpus; ++i) {
        check(!state->started_by_guest_start_up_ipi[i],
              "init level de-assert flags no processor as started");
    }
}

// === INIT supersedes a start-up IPI queued before it ===================
//
// `queued_start_up` holds one vector per processor for the window between
// a target's INIT and its INIT exit, and nothing in the word says which
// INIT-SIPI-SIPI sequence the vector belongs to. `emulate_init_signal`
// cannot supply that: it records, from a measurement, that clearing the
// slot at the top of an INIT threw away the *live* vector, because this
// VMM sees the start-up IPI before the target reaches its INIT exit.
//
// So the sender clears it, which is the ordering KVM gets from
// `apic->pending_events` - `kvm_apic_accept_events` drops a pending
// start-up IPI when it takes an INIT. The measured failure it closes is
// the second start-up IPI of a satisfied sequence being queued against a
// running processor and then applied to the *next* INIT: vector 0x2 held
// over from one sequence started the processor at 0x2000, where the guest
// hypervisor has nothing.

/**
 * The vector a slot is holding, or nothing.
 */
std::optional<std::uint64_t>
queued_vector(zpp::hypervisor::hypervisor & state, std::size_t slot)
{
    auto held = state.queued_start_up[slot].load();
    if (0 == (held & zpp::hypervisor::hypervisor::queued_start_up_valid)) {
        return {};
    }
    return held & 0xff;
}

/**
 * Puts a vector in a slot's mailbox the way `start_up_processor` does.
 */
void queue_vector(zpp::hypervisor::hypervisor & state,
                  std::size_t slot,
                  std::uint64_t vector)
{
    state.queued_start_up[slot].store(
        zpp::hypervisor::hypervisor::queued_start_up_valid | vector);
}

void init_discards_a_superseded_start_up_vector()
{
    auto state = make(0, {0, 1, 2, 3});

    // A processor this VMM already tracks, holding the vector left over
    // from the sequence before - which is exactly the state the rig was
    // measured in.
    state->apic_id[1] = 1;
    state->number_of_known_processors = 2;
    queue_vector(*state, 1, 0x2);

    auto command =
        icr{.delivery_mode = delivery_init, .destination = 1}.value();

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(),
          "an init that discards a stale vector is still passed through");
    if (answer) {
        check_equal(command, *answer, "and passed through unmodified");
    }
    check(!queued_vector(*state, 1).has_value(),
          "the start-up vector queued before the init is discarded - a "
          "vector sent for the previous life must not start the next one");
    check_equal(2,
                state->number_of_known_processors,
                "and no slot is allocated to discover it");
}

/**
 * The boot processor's own mailbox is not touched by an INIT naming
 * somebody else. A clear that reached every slot would look like it
 * worked in every case above and would drop a live vector for a
 * processor the guest was starting at the same moment.
 */
void init_discards_only_for_its_own_target()
{
    auto state = make(0, {0, 1, 2, 3});
    state->apic_id[1] = 1;
    state->apic_id[2] = 2;
    state->number_of_known_processors = 3;
    queue_vector(*state, 1, 0x2);
    queue_vector(*state, 2, 0x87);

    state->on_interrupt_command(
        icr{.delivery_mode = delivery_init, .destination = 1}.value());

    check(!queued_vector(*state, 1).has_value(),
          "the named target's vector is discarded");
    check_equal(0x87,
                queued_vector(*state, 2).value_or(0),
                "and a processor the init did not name keeps its own");
}

/**
 * An INIT for an identifier this VMM has never seen clears nothing and,
 * critically, allocates nothing. `c6349a4` is the defect that made
 * `processor_slot` spend an entry on a destination that was not an
 * identifier at all, and this path must not reopen it.
 */
void init_for_an_unknown_processor_allocates_nothing()
{
    auto state = make(0, {0, 1, 2, 3});

    state->on_interrupt_command(
        icr{.delivery_mode = delivery_init, .destination = 0x33}.value());

    check_equal(1,
                state->number_of_known_processors,
                "an init for an unknown identifier allocates no slot");
}

/**
 * INIT level de-assert supersedes nothing, because it starts nothing.
 * `439abb5` was reverted in part for acting on exactly this command, and
 * a clear that fired here would drop a vector the guest had just sent.
 */
void init_level_de_assert_discards_nothing()
{
    auto state = make(0, {0, 1, 2, 3});
    state->apic_id[1] = 1;
    state->number_of_known_processors = 2;
    queue_vector(*state, 1, 0x87);

    auto command = icr{.delivery_mode = delivery_init,
                       .level_assert = false,
                       .trigger_level = true,
                       .destination = 1}
                       .value();

    state->on_interrupt_command(command);

    check_equal(0x87,
                queued_vector(*state, 1).value_or(0),
                "init level de-assert leaves a queued start-up vector "
                "alone - it starts nothing, so it supersedes nothing");

    // And the de-assert is the *only* form excluded, which is the wider
    // half of the same condition. KVM's `APIC_DM_INIT` case reads
    // `if (!trig_mode || level)` (.references/kvm/lapic.c:1365), so an
    // edge-triggered command with the level bit clear is still an INIT.
    // A test on the level bit alone would ignore it, and this pins the
    // difference between the two conditions rather than leaving it to be
    // rediscovered.
    auto edge = make(0, {0, 1, 2, 3});
    edge->apic_id[1] = 1;
    edge->number_of_known_processors = 2;
    queue_vector(*edge, 1, 0x87);

    edge->on_interrupt_command(icr{.delivery_mode = delivery_init,
                                   .level_assert = false,
                                   .trigger_level = false,
                                   .destination = 1}
                                   .value());

    check(!queued_vector(*edge, 1).has_value(),
          "an edge-triggered init with the level bit clear is still an "
          "init, and still supersedes a queued start-up vector");
}

/**
 * A broadcast INIT is how Windows starts its processors, so the clear has
 * to resolve one - and it has to exclude the sender, which is not a
 * processor anybody is waiting to start.
 */
void broadcast_init_discards_every_target_but_the_sender()
{
    auto state = make(5, {0, 1, 2, 3});
    state->apic_id[0] = 5;
    state->apic_id[1] = 1;
    state->apic_id[2] = 2;
    state->number_of_known_processors = 3;
    queue_vector(*state, 0, 0x11);
    queue_vector(*state, 1, 0x22);
    queue_vector(*state, 2, 0x33);

    state->on_interrupt_command(
        icr{.delivery_mode = delivery_init,
            .shorthand = shorthand_all_excluding_self}
            .value());

    check_equal(0x11,
                queued_vector(*state, 0).value_or(0),
                "a broadcast init excludes the sender's own slot");
    check(!queued_vector(*state, 1).has_value(),
          "and discards the vector held for every other known processor");
    check(!queued_vector(*state, 2).has_value(),
          "including the last of them");
}

/**
 * A logical destination is a bitmask matched against each APIC's LDR and
 * DFR, not an identifier. The start-up path refuses one rather than
 * guessing, and this must refuse it the same way: clearing the slot that
 * a bitmask happens to coincide with would drop a live vector for a
 * processor the guest never named.
 */
void logical_init_discards_nothing()
{
    auto state = make(0, {0, 1, 2, 3});
    state->apic_id[1] = 1;
    state->number_of_known_processors = 2;
    queue_vector(*state, 1, 0x87);

    state->on_interrupt_command(icr{.delivery_mode = delivery_init,
                                    .logical_destination = true,
                                    .destination = 1}
                                    .value());

    check_equal(0x87,
                queued_vector(*state, 1).value_or(0),
                "an init in logical destination mode discards nothing - "
                "the destination field is not an identifier");
}

// === Start-up, physical destination ====================================

void start_up_physical_adopted()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .destination = 2}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(!answer.has_value(),
          "an adopted start-up ipi is swallowed - the guest's own write "
          "must not also go out, or the processor starts twice");
    check_equal(
        1, state->ipi_start_up_seen, "the start-up ipi is counted");
    check_equal(1,
                g_observed.start_up_attempt_count,
                "exactly one processor was started");
    check_equal(2,
                g_observed.start_up_attempts[0].destination,
                "started the processor the destination field names");
    check_equal(0x8,
                g_observed.start_up_attempts[0].vector,
                "with the vector from bits 7:0");

    // The slot is allocated because the identifier was not known, and the
    // flag lands on that slot rather than on slot 0.
    check_equal(2,
                state->number_of_known_processors,
                "a start-up ipi to an unknown identifier allocates one "
                "slot");
    check(state->started_by_guest_start_up_ipi[1],
          "the new slot is flagged as started by the guest");
    check(!state->started_by_guest_start_up_ipi[0],
          "the boot processor's slot is not");
}

void start_up_physical_needs_hardware()
{
    auto state = make(0, {0, 1, 2, 3});
    g_observed.needs_hardware_for[0] = 2;
    g_observed.needs_hardware_count = 1;

    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .destination = 2}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(),
          "a start-up ipi that needs hardware is passed through, because "
          "only a real one can move the target from where it is");
    if (answer) {
        check_equal(command, *answer, "and passed through unmodified");
    }

    // The flag still lands. It is a fact about the guest having asked,
    // not about the attempt succeeding - a start-up IPI this VMM passes
    // to hardware still means the guest started that processor, and its
    // duplicate must still be ignored.
    check(state->started_by_guest_start_up_ipi[1],
          "the target is flagged as started by the guest even when the "
          "command is forwarded");
}

/**
 * The vector is eight bits and the destination is the upper half.
 *
 * A start-up IPI's vector is a page number rather than an interrupt
 * vector - SDM Figure 13-12, "110 (Start-Up) ... The vector typically
 * points to a start-up routine" (.references/sdm.txt:171252) - so the
 * whole byte matters and a decode that masked it narrower would place the
 * trampoline somewhere else entirely.
 */
void start_up_field_extraction()
{
    for (std::uint64_t vector : {std::uint64_t{0},
                                 std::uint64_t{1},
                                 std::uint64_t{0x7f},
                                 std::uint64_t{0x80},
                                 std::uint64_t{0xff}}) {
        auto state = make(0, {0, 1});
        auto command = icr{.vector = vector,
                           .delivery_mode = delivery_start_up,
                           .destination = 1}
                           .value();
        state->on_interrupt_command(command);
        check_equal(vector,
                    g_observed.start_up_attempts[0].vector,
                    "vector " + std::to_string(vector) +
                        " survives the decode");
    }

    for (std::uint64_t destination : {std::uint64_t{1},
                                      std::uint64_t{0xff},
                                      std::uint64_t{0x100},
                                      std::uint64_t{0xffffffff}}) {
        auto state = make(0, {0, 1});
        auto command = icr{.vector = 0x8,
                           .delivery_mode = delivery_start_up,
                           .destination = destination}
                           .value();
        state->on_interrupt_command(command);
        check_equal(destination,
                    g_observed.start_up_attempts[0].destination,
                    "destination " + std::to_string(destination) +
                        " survives the decode");
    }
}

// === Start-up, logical destination =====================================

/**
 * The `c6349a4` defect, stated as a test.
 *
 * SDM Figure 13-12: "Destination Mode - Selects either physical (0) or
 * logical (1) destination mode" (.references/sdm.txt:171259), and 13.6.2
 * makes a logical destination an eight-bit message destination address
 * that a receiving APIC compares against its own LDR under whichever of
 * flat or cluster its DFR selects. It is a bitmask, not an identifier.
 *
 * Two things have to hold, and the second is the one that was wrong:
 * the command is passed through, **and no slot is allocated**. Asking
 * `processor_slot` about a bitmask is worse than not answering - it fails
 * to match, appends, and spends an entry on a processor that does not
 * exist, handing the rest of the function an index that names the wrong
 * one.
 *
 * The flat model makes this hard to see by accident: logical id 0x01 is
 * also physical APIC id 1, so a single-target flat command looks as
 * though it works.
 */
void start_up_logical_is_refused_without_allocating()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .logical_destination = true,
                       .destination = 0x02}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(),
          "a logical-destination start-up ipi is passed through");
    if (answer) {
        check_equal(command, *answer, "and passed through unmodified");
    }
    check_equal(1,
                state->ipi_refused_logical,
                "the refusal is counted, so processors handed to the "
                "guest unvirtualized are accounted for");
    check_equal(
        0, g_observed.start_up_attempt_count, "nothing is started");
    check_equal(
        1,
        state->number_of_known_processors,
        "**no slot is allocated** - asking processor_slot about a "
        "destination bitmask spends an entry on a processor that does "
        "not exist and returns an index naming the wrong one");
}

/**
 * And the case that hides it: the flat model where the bitmask and the
 * identifier coincide.
 *
 * Logical destination 0x01 under a flat model selects the processor whose
 * LDR bit 0 is set, which on a conventionally programmed machine is APIC
 * id 0. A decode that ignored bit 11 would read 1, match APIC id 1, and
 * start the wrong processor - which is exactly the shape that made this
 * look correct in testing.
 */
void start_up_logical_flat_does_not_coincide()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .logical_destination = true,
                       .destination = 0x01}
                       .value();

    state->on_interrupt_command(command);

    check_equal(0,
                g_observed.start_up_attempt_count,
                "a flat logical destination of 0x01 starts nothing, even "
                "though it reads as physical id 1");
    check_equal(
        1, state->ipi_refused_logical, "and is counted as refused");
}

/**
 * A logical destination in any *other* delivery mode is not this
 * function's business at all, and must not be counted as a refusal - the
 * counter says how many processors were handed over unvirtualized, and
 * inflating it with fixed interrupts makes it useless.
 */
void logical_fixed_is_not_counted_as_refused()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.vector = 0x40,
                       .delivery_mode = delivery_fixed,
                       .logical_destination = true,
                       .destination = 0x0f}
                       .value();

    state->on_interrupt_command(command);

    check_equal(0,
                state->ipi_refused_logical,
                "a logical fixed interrupt is not counted as a refused "
                "start-up ipi");
}

// === Start-up, broadcast ===============================================

/**
 * `987441f`: a broadcast start-up IPI is resolved against the roster
 * rather than passed through, because a broadcast is how Windows starts
 * its processors.
 *
 * Passing it through is worse than refusing when a guest hypervisor is
 * above: the processors do start, outside this VMM, so they belong to
 * neither layer, and the guest hypervisor's rendezvous never completes.
 */
void broadcast_resolves_the_roster()
{
    auto state = make(0, {0, 1, 2, 3});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .shorthand = shorthand_all_excluding_self}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(!answer.has_value(), "a resolved broadcast is swallowed");
    check_equal(3,
                g_observed.start_up_attempt_count,
                "three of the four roster entries were started");
    check_equal(0,
                state->ipi_refused_shorthand,
                "and it is not counted as refused");

    for (std::size_t i{}; i < g_observed.start_up_attempt_count; ++i) {
        check(0 != g_observed.start_up_attempts[i].destination,
              "the sender is excluded from the broadcast - SDM Figure "
              "13-12's 'all excluding self'");
        check_equal(0x8,
                    g_observed.start_up_attempts[i].vector,
                    "every target gets the broadcast's own vector");
    }
}

/**
 * The sender is excluded whatever its identifier is, not merely when it
 * is zero. A roster whose first entry is not the boot processor is the
 * ordinary case on a machine whose firmware enumerates by ACPI order.
 */
void broadcast_excludes_the_sender_by_identity()
{
    auto state = make(4, {0, 2, 4, 6});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .shorthand = shorthand_all_excluding_self}
                       .value();

    state->on_interrupt_command(command);

    check_equal(
        3, g_observed.start_up_attempt_count, "three targets started");
    for (std::size_t i{}; i < g_observed.start_up_attempt_count; ++i) {
        check(4 != g_observed.start_up_attempts[i].destination,
              "apic id 4 is the sender and is excluded");
    }
}

/**
 * A broadcast target that needs hardware gets its own targeted command.
 *
 * The guest's broadcast cannot be forwarded for one target and swallowed
 * for another, so the one that still needs a real start-up IPI is sent
 * one naming it - identical in effect to what the guest wrote, restricted
 * to the processor that needs it.
 */
void broadcast_sends_hardware_for_the_targets_that_need_it()
{
    auto state = make(0, {0, 1, 2, 3});
    g_observed.needs_hardware_for[0] = 2;
    g_observed.needs_hardware_count = 1;

    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .shorthand = shorthand_all_excluding_self}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(!answer.has_value(),
          "the broadcast is still swallowed - the targeted command below "
          "replaces it for the one processor that needed it");
    check_equal(1,
                g_observed.hardware_ipi_count,
                "one targeted start-up ipi went to hardware");
    check_equal(2,
                g_observed.hardware_ipis[0].destination,
                "naming the processor that needed it");
    check_equal(0x8,
                g_observed.hardware_ipis[0].vector,
                "with the vector the guest broadcast");
}

/**
 * With no roster there is nothing to resolve against, so the guest's own
 * write goes out and the refusal is counted.
 */
void broadcast_without_a_roster_is_refused()
{
    auto state = make(0, {});
    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .shorthand = shorthand_all_excluding_self}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(answer.has_value(),
          "an unresolvable broadcast is passed through");
    check_equal(1,
                state->ipi_refused_shorthand,
                "and counted, so a boot with no roster is distinguishable "
                "from one with nothing to start");
    check_equal(
        0, g_observed.start_up_attempt_count, "nothing was started");
}

/**
 * All three shorthands take the broadcast path, not only the legal one.
 *
 * SDM Figure 13-12 gives 01 self, 10 all including self and 11 all
 * excluding self. Only 11 is architecturally sound for a start-up IPI -
 * the other two include the sender, which cannot start itself - but a
 * decode that only recognised 11 would read bits 63:32 of a 01 or 10
 * command as a destination, and those bits are whatever the guest left
 * there. That is the mistake the comment on this branch records: it
 * "credited the boot processor with a start-up IPI nobody had sent it".
 */
void every_shorthand_takes_the_broadcast_path()
{
    for (auto shorthand : {shorthand_self,
                           shorthand_all_including_self,
                           shorthand_all_excluding_self}) {
        auto state = make(0, {0, 1});
        auto command = icr{.vector = 0x8,
                           .delivery_mode = delivery_start_up,
                           .shorthand = shorthand,
                           .destination = 0xdeadbeef}
                           .value();

        state->on_interrupt_command(command);

        auto name = std::to_string(shorthand);
        check_equal(1,
                    state->ipi_start_up_seen,
                    "shorthand " + name + " is counted as a start-up ipi");
        for (std::size_t i{}; i < g_observed.start_up_attempt_count; ++i) {
            check(0xdeadbeef !=
                      g_observed.start_up_attempts[i].destination,
                  "shorthand " + name +
                      " does not read bits 63:32 as a destination");
        }
    }
}

// === processor_slot ====================================================

void slot_allocation()
{
    auto state = make(0, {});

    auto first = state->processor_slot(0);
    check(first.has_value(), "the boot processor's own id resolves");
    check_equal(0, *first, "to slot 0");
    check_equal(1,
                state->number_of_known_processors,
                "and allocates nothing, because it is already known");

    auto second = state->processor_slot(7);
    check(second.has_value(), "an unknown id is allocated a slot");
    check_equal(1, *second, "the next one");
    check_equal(2, state->number_of_known_processors, "and is now known");

    auto again = state->processor_slot(7);
    check(again.has_value(), "the same id resolves again");
    check_equal(1, *again, "to the same slot");
    check_equal(2,
                state->number_of_known_processors,
                "without allocating a second");
}

/**
 * The table is bounded, and a slot is not a label.
 *
 * `setup_vmcs` writes `vpid(cpu + 1)` and a VMCS may not be active on
 * more than one logical processor, so an out-of-range slot is refused
 * rather than folded back onto an existing one.
 */
void slot_allocation_is_bounded()
{
    auto state = make(0, {});

    for (std::uint64_t id = 1; id < zpp::hypervisor::hypervisor::max_cpus;
         ++id) {
        auto slot = state->processor_slot(id);
        check(slot.has_value(),
              "id " + std::to_string(id) + " fits in the table");
    }

    check_equal(zpp::hypervisor::hypervisor::max_cpus,
                state->number_of_known_processors,
                "the table is exactly full");

    auto overflow = state->processor_slot(0x1000);
    check(!overflow.has_value(),
          "the identifier past the last slot is refused rather than "
          "folded onto an existing one");
    check_equal(zpp::hypervisor::hypervisor::max_cpus,
                state->number_of_known_processors,
                "and the count does not grow past the table");
}

// === The two mechanisms, and which one is armed ========================
//
// A local APIC answers at a page in memory or through model-specific
// registers, never both, and which one it uses is IA32_APIC_BASE's two
// top bits. SDM 13.12.5.1 reads the pair as four states
// (.references/sdm.txt), and the interception this VMM needs is
// different for each of the two live ones: the page is watched through
// the extended page tables, the register through the MSR bitmap.
//
// Nothing tested any of this. Two fixes landed on it pinned only by
// source rules in check-exit-handler.sh, which cannot see a wrong bit
// index or a wrong survey.

/**
 * IA32_APIC_BASE, SDM Figure 13-5. Only the two mode bits and the base
 * matter here.
 */
constexpr std::uint64_t apic_base_extended = 1ull << 10;
constexpr std::uint64_t apic_base_enabled = 1ull << 11;

/**
 * Where the x2APIC interrupt command register's bit lives in the MSR
 * bitmap.
 *
 * The bitmap is four 1024-byte bitmaps in one page: reads of
 * 0x00000000-0x00001fff, reads of 0xc0000000-0xc0001fff, then writes of
 * each. SDM 25.6.9. The interrupt command register is MSR 0x830, which
 * is in the low range, and it is *writes* that send an interrupt - so
 * the byte is 0x800 + 0x830/8 and the bit is 0x830 % 8.
 *
 * Computed here from the register number rather than taken from the
 * source, because a wrong byte index is exactly the defect this can
 * catch and reading it back from `intercept_interrupt_command` would
 * catch nothing.
 */
constexpr std::uint64_t x2apic_icr_msr = 0x830;
constexpr std::size_t write_low_bitmap = 0x800;
constexpr std::size_t icr_byte = write_low_bitmap + (x2apic_icr_msr / 8);
constexpr std::uint8_t icr_bit =
    static_cast<std::uint8_t>(1u << (x2apic_icr_msr % 8));

bool interception_armed(const zpp::hypervisor::hypervisor & state)
{
    return 0 != (state.msr_bitmap[icr_byte] & icr_bit);
}

/**
 * Put a processor's local APIC into a mode and let the real code notice.
 */
void observe(zpp::hypervisor::hypervisor & state,
             std::size_t cpu,
             std::uint64_t base)
{
    zpp::arch::x86_64::g_apic_base = base;
    state.note_apic_mode(cpu);
}

constexpr std::uint64_t apic_page = 0xfee0'0000;
constexpr std::uint64_t xapic_base = apic_page | apic_base_enabled;
constexpr std::uint64_t x2apic_base =
    apic_page | apic_base_enabled | apic_base_extended;
constexpr std::uint64_t disabled_base = apic_page;

/**
 * The proof that replaces the guess.
 *
 * `all_processors_started` had two ways to become true and both were
 * heuristics - a two-minute silence on the interrupt command register,
 * and "somebody somewhere was handed a start-up vector". The member's
 * own comment said the fact "cannot be derived", and that was wrong: the
 * loader hands over the firmware's roster, each processor records its
 * own identifier in `main` and marks itself virtualized immediately
 * before its launch, so "every processor on this machine is running
 * under this hypervisor" is a join of two tables this VMM already keeps.
 *
 * What each case below pins is a way of getting that join wrong, and the
 * second is the one that would be silent: `apic_id` zero-initializes and
 * zero is a real identifier, so a roster entry for processor 0 would be
 * satisfied by any empty slot if the scan did not require the slot to be
 * virtualized first.
 */
void the_roster_is_the_proof()
{
    // Nothing adopted at all.
    {
        auto state = make(0, {0, 1});
        check(!state->every_platform_processor_adopted(),
              "a roster with nothing virtualized is not fully adopted");
    }

    // The boot processor only, which is the shape of the measured
    // two-processor boot for as long as its application processor is
    // still being started.
    {
        auto state = make(0, {0, 1});
        state->processor_virtualized[0] = true;
        check(!state->every_platform_processor_adopted(),
              "one processor of two is not every processor");
    }

    // The trap. Slot 1 has never been used, so `apic_id[1]` is zero -
    // and roster entry 0 is also zero. A scan that matched identifiers
    // before asking whether the slot is virtualized would call this
    // machine fully adopted on the strength of an empty slot.
    {
        auto state = make(0, {0, 1});
        state->processor_virtualized[0] = true;
        state->apic_id[0] = 0;
        check(!state->every_platform_processor_adopted(),
              "an empty slot whose default identifier happens to equal a "
              "roster entry does not satisfy it");
    }

    // Both, by way of a slot the guest's own start-up IPI allocated
    // rather than one `main` wrote - the two are the same table and this
    // says so.
    {
        auto state = make(0, {0, 1});
        state->processor_virtualized[0] = true;
        auto slot = state->processor_slot(1);
        check(slot.has_value(), "the second identifier takes a slot");
        state->processor_virtualized[*slot] = true;
        check(state->every_platform_processor_adopted(),
              "every roster identifier with a virtualized slot is every "
              "processor adopted");
    }

    // A slot beyond `number_of_known_processors`, which is what `main`
    // produces: it writes `apic_id[cpuid]` for the processor it is
    // running on and never touches the count. A scan bounded by the
    // count would miss it and hold the watch armed for ever.
    {
        auto state = make(0, {0, 7});
        state->processor_virtualized[0] = true;
        state->apic_id[3] = 7;
        state->processor_virtualized[3] = true;
        check_equal(1,
                    state->number_of_known_processors,
                    "the slot was written past the known-processor count");
        check(state->every_platform_processor_adopted(),
              "and is still found, because the two tables have different "
              "writers");
    }

    // No roster is not a proof of anything, and answering true would
    // drop the watch on every loader that hands one over empty.
    {
        auto state = make(0, {});
        state->processor_virtualized[0] = true;
        check(!state->every_platform_processor_adopted(),
              "an empty roster proves nothing and answers no");
    }
}

/**
 * Whether anything is positioned to see a write to the interrupt command
 * register, which is what `emulate_init_signal` now requires before it
 * will wait for a software hand-off.
 */
void the_interception_knows_whether_it_is_armed()
{
    auto state = make(0, {0, 1});

    check(!state->interrupt_command_intercepted(),
          "nothing is armed on a fresh machine");

    state->intercept_interrupt_command(true);
    check(state->interrupt_command_intercepted(),
          "the x2APIC mechanism alone is enough");

    state->intercept_interrupt_command(false);
    check(!state->interrupt_command_intercepted(),
          "and disarming it is noticed");

    state->watched_apic_page = 0xfee00;
    check(state->interrupt_command_intercepted(),
          "the xAPIC mechanism alone is enough");

    state->watched_apic_page = 0;
    check(!state->interrupt_command_intercepted(),
          "with neither armed, no sender can hand a vector over");
}

void the_bitmap_bit_is_the_one_the_architecture_names()
{
    auto state = make();

    state->intercept_interrupt_command(true);
    check(interception_armed(*state),
          "arming sets the write bit for MSR 0x830 in the low-range "
          "write bitmap");

    std::size_t others{};
    for (std::size_t at{}; at < zpp::hypervisor::hypervisor::page_size;
         ++at) {
        if (at == icr_byte) {
            continue;
        }
        others += (0 != state->msr_bitmap[at]) ? 1 : 0;
    }
    check_equal(0,
                others,
                "and sets nothing anywhere else in the page, which is "
                "shared with every other intercept in the machine");

    state->intercept_interrupt_command(false);
    check(!interception_armed(*state), "and disarming clears it again");
}

/**
 * The three states IA32_APIC_BASE can actually be in, and what each
 * arms.
 *
 * The register is an MSR only in x2APIC mode - SDM 13.12.1 - so a guest
 * in xAPIC mode writing it should take #GP, and an armed bit would
 * instead exit and have this VMM perform the write in the host, where
 * the #GP has no recovery point and stops the processor.
 */
void each_mode_arms_its_own_mechanism()
{
    auto disabled = make();
    observe(*disabled, 0, disabled_base);
    check(!interception_armed(*disabled),
          "a disabled local APIC arms no MSR interception");
    check(!g_observed.watch_last,
          "and no page watch, because there is nothing to watch");

    auto xapic = make();
    observe(*xapic, 0, xapic_base);
    check(!interception_armed(*xapic),
          "xAPIC mode arms no MSR interception - the register is not an "
          "MSR in that mode and intercepting it would exit into a host "
          "WRMSR that faults");
    check(g_observed.watch_last, "and watches the page instead");

    auto x2apic = make();
    observe(*x2apic, 0, x2apic_base);
    check(interception_armed(*x2apic),
          "x2APIC mode arms the MSR interception");
    check(!g_observed.watch_last,
          "and unwatches the page, which that mode does not use");
}

/**
 * The case the survey exists for, and the reason it is a survey rather
 * than a look at the caller.
 *
 * A guest switches its processors to x2APIC one at a time, so during the
 * switch both mechanisms are genuinely in use at once. Disarming either
 * loses interrupt commands from the processors that have not moved.
 */
void a_half_switched_machine_arms_both()
{
    auto state = make();

    observe(*state, 0, xapic_base);
    observe(*state, 1, xapic_base);

    // The first processor moves.
    observe(*state, 0, x2apic_base);

    check(interception_armed(*state),
          "the processor that moved has its interrupt command register "
          "intercepted");
    check(g_observed.watch_last,
          "and the page stays watched for the one that has not");

    // And the second.
    observe(*state, 1, x2apic_base);

    check(interception_armed(*state),
          "with both moved the interception stays armed");
    check(!g_observed.watch_last, "and the page is finally given back");
}

/**
 * And the way back, which is the same survey read the other way.
 */
void the_last_processor_to_leave_disarms()
{
    auto state = make();

    observe(*state, 0, x2apic_base);
    observe(*state, 1, x2apic_base);
    check(interception_armed(*state), "both processors in x2APIC mode");

    observe(*state, 0, disabled_base);
    check(interception_armed(*state),
          "one leaving does not disarm the other's interception");

    observe(*state, 1, disabled_base);
    check(!interception_armed(*state), "the last one leaving does");
}

/**
 * A processor index outside the table writes nothing.
 *
 * `note_apic_mode` is called with a slot derived from the VPID, and the
 * arrays it indexes are `max_cpus` long.
 */
void a_processor_outside_the_table_is_not_recorded()
{
    auto state = make();

    observe(*state, zpp::hypervisor::hypervisor::max_cpus, x2apic_base);

    check(!interception_armed(*state),
          "an index past the table records no mode, so the survey finds "
          "nothing and arms nothing");
}

/**
 * The survey and the arming are one decision about machine-wide state.
 *
 * `msr_bitmap` is a single page that every processor's VMCS points at,
 * and `observed_apic_mode` is read across every processor - so
 * `note_apic_mode` is a read-modify-write of shared state performed in
 * root operation. Two processors switching to x2APIC at once each write
 * their own slot and then survey, and a survey that ran before the other
 * store became visible computes "no processor is in x2APIC mode" and
 * disarms what the other has just armed.
 *
 * The assertion is the invariant rather than a schedule: half the
 * processors end in x2APIC mode, so the interception must be armed.
 *
 * **A witness, not a proof, and it is worth being exact about which.**
 * Run against a copy of local_apic.cpp with `apic_mode_lock` removed,
 * this case did not reproduce the window in 2000 attempts on this host -
 * `note_apic_mode` is short enough that eight threads released together
 * mostly do not overlap inside it. What settles the defect is the code
 * rather than the schedule: `msr_bitmap` is a plain `std::uint8_t` array
 * read and written from several processors with no synchronisation,
 * which is a data race by the language's own definition and a byte the
 * processor itself consults on every guest MSR access. The case is kept
 * because it costs milliseconds, because a pass is never wrong, and
 * because the next person to remove the lock may be luckier with the
 * timing than this run was.
 *
 * The threads are not a stand-in for a processor in every respect - the
 * same caveat tests/ap_start_up records - but they share the one thing
 * that matters here: unsynchronised access to the same bytes.
 */
void the_survey_and_the_arming_are_one_decision()
{
    // Enough attempts that an unguarded window is met rather than
    // stepped over, and few enough that the harness stays fast.
    constexpr std::size_t attempts = 2000;
    constexpr std::size_t processors = 8;
    constexpr std::size_t entering = processors / 2;

    std::size_t left_disarmed{};

    for (std::size_t attempt{}; attempt < attempts; ++attempt) {
        auto state = make();

        // Everything starts in xAPIC mode, which is where a machine is
        // before its guest moves any of it.
        for (std::size_t cpu{}; cpu < processors; ++cpu) {
            state->observed_apic_mode[cpu] =
                zpp::hypervisor::hypervisor::apic_mode::xapic;
        }

        std::atomic<bool> go{false};
        std::vector<std::thread> threads;

        for (std::size_t cpu{}; cpu < processors; ++cpu) {
            threads.emplace_back([&, cpu] {
                // Half move into x2APIC and half switch their local APIC
                // off. Both halves at once is what makes this a race
                // rather than a repetition: a survey run by a leaving
                // processor before an entering one's store is visible
                // computes "nobody is in x2APIC mode" and disarms what
                // the entering processor has just armed. With every
                // thread doing the same thing there is nothing to lose,
                // which is why the first version of this case found
                // nothing.
                auto base = (cpu < entering) ? x2apic_base : disabled_base;

                while (!go.load(std::memory_order_acquire)) {
                }

                // Its own register, not a shared one - see the note on
                // g_apic_base.
                zpp::arch::x86_64::g_apic_base = base;
                state->note_apic_mode(cpu);
            });
        }

        go.store(true, std::memory_order_release);
        for (auto & thread : threads) {
            thread.join();
        }

        if (!interception_armed(*state)) {
            ++left_disarmed;
        }
    }

    check_equal(0,
                left_disarmed,
                "half the processors ended in x2APIC mode, so the "
                "interrupt command register must be intercepted - " +
                    std::to_string(left_disarmed) + " of " +
                    std::to_string(attempts) +
                    " concurrent switches left it disarmed");
}

} // namespace

int main()
{
    passthrough_delivery_modes();
    init_is_forwarded_and_counted();
    init_level_de_assert_starts_nothing();
    init_discards_a_superseded_start_up_vector();
    init_discards_only_for_its_own_target();
    init_for_an_unknown_processor_allocates_nothing();
    init_level_de_assert_discards_nothing();
    broadcast_init_discards_every_target_but_the_sender();
    logical_init_discards_nothing();
    start_up_physical_adopted();
    start_up_physical_needs_hardware();
    start_up_field_extraction();
    start_up_logical_is_refused_without_allocating();
    start_up_logical_flat_does_not_coincide();
    logical_fixed_is_not_counted_as_refused();
    broadcast_resolves_the_roster();
    broadcast_excludes_the_sender_by_identity();
    broadcast_sends_hardware_for_the_targets_that_need_it();
    broadcast_without_a_roster_is_refused();
    every_shorthand_takes_the_broadcast_path();
    slot_allocation();
    slot_allocation_is_bounded();
    the_roster_is_the_proof();
    the_interception_knows_whether_it_is_armed();
    the_bitmap_bit_is_the_one_the_architecture_names();
    each_mode_arms_its_own_mechanism();
    a_half_switched_machine_arms_both();
    the_last_processor_to_leave_disarms();
    a_processor_outside_the_table_is_not_recorded();
    the_survey_and_the_arming_are_one_decision();

    std::println(
        "local_apic: {} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
