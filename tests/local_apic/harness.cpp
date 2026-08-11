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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace zpp::hypervisor
{
namespace
{
std::vector<const char *> g_log;
} // namespace

void log_line(const char * format)
{
    g_log.push_back(format);
}

std::size_t log_count()
{
    return g_log.size();
}

const char * log_at(std::size_t index)
{
    return g_log[index];
}

void log_reset()
{
    g_log.clear();
}

/*
 * The three functions the decode calls that reach hardware in the real
 * VMM. Each records what it was asked for, which is what makes the
 * decode's decisions observable at all: "adopted" and "passed through"
 * differ only in whether a start-up IPI reached a processor.
 */
std::uint64_t hypervisor::local_apic_id()
{
    return this->self_apic_id;
}

hypervisor::start_up_result hypervisor::start_up_processor(
    std::uint64_t destination, std::uint64_t vector)
{
    if (this->start_up_attempt_count <
        (sizeof(this->start_up_attempts) /
         sizeof(this->start_up_attempts[0]))) {
        this->start_up_attempts[this->start_up_attempt_count++] =
            attempt{destination, vector};
    }

    for (std::size_t i{}; i < this->needs_hardware_count; ++i) {
        if (this->needs_hardware_for[i] == destination) {
            return start_up_result::needs_hardware;
        }
    }

    return start_up_result::adopted;
}

void hypervisor::send_start_up_ipi(std::uint64_t apic,
                                   std::uint64_t vector)
{
    if (this->hardware_ipi_count <
        (sizeof(this->hardware_ipis) / sizeof(this->hardware_ipis[0]))) {
        this->hardware_ipis[this->hardware_ipi_count++] =
            attempt{apic, vector};
    }
}

} // namespace zpp::hypervisor

namespace
{
using zpp::hypervisor::hypervisor;

std::size_t g_checks{};
std::size_t g_failures{};

void check(bool condition, const std::string & what)
{
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
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
    std::printf("FAIL: %s\n  expected 0x%llx\n  actual   0x%llx\n",
                what.c_str(),
                static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(actual));
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
void configure(hypervisor & state,
               std::uint64_t self,
               std::initializer_list<std::uint32_t> roster)
{
    state.self_apic_id = self;
    state.apic_id[0] = self;
    state.number_of_known_processors = 1;

    std::size_t i{};
    for (auto id : roster) {
        state.platform_apic_id[i++] = id;
    }
    state.number_of_platform_processors = roster.size();

    zpp::hypervisor::log_reset();
}

/**
 * A configured hypervisor, held by the caller.
 *
 * Returned through a unique_ptr rather than by value because the real
 * class holds a `zpp::spin_lock`, which is deliberately not copyable -
 * the shim keeps the real member rather than a stand-in, so that
 * `processor_slot`'s lock discipline is the real one.
 */
std::unique_ptr<hypervisor>
make(std::uint64_t self = 0,
     std::initializer_list<std::uint32_t> roster = {})
{
    auto state = std::make_unique<hypervisor>();
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
                    state->start_up_attempt_count,
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
    check_equal(0, state->start_up_attempt_count, "init starts nothing");
    check_equal(
        1, state->number_of_known_processors, "init allocates no slot");
    check_equal(
        0, state->hardware_ipi_count, "init sends no hardware ipi");
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
                state->start_up_attempt_count,
                "init level de-assert starts nothing");
    check_equal(0,
                state->hardware_ipi_count,
                "init level de-assert sends no hardware ipi");
    for (std::size_t i{}; i < hypervisor::max_cpus; ++i) {
        check(!state->started_by_guest_start_up_ipi[i],
              "init level de-assert flags no processor as started");
    }
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
                state->start_up_attempt_count,
                "exactly one processor was started");
    check_equal(2,
                state->start_up_attempts[0].destination,
                "started the processor the destination field names");
    check_equal(0x8,
                state->start_up_attempts[0].vector,
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
    state->needs_hardware_for[0] = 2;
    state->needs_hardware_count = 1;

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
                    state->start_up_attempts[0].vector,
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
                    state->start_up_attempts[0].destination,
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
    check_equal(0, state->start_up_attempt_count, "nothing is started");
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
                state->start_up_attempt_count,
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
                state->start_up_attempt_count,
                "three of the four roster entries were started");
    check_equal(0,
                state->ipi_refused_shorthand,
                "and it is not counted as refused");

    for (std::size_t i{}; i < state->start_up_attempt_count; ++i) {
        check(0 != state->start_up_attempts[i].destination,
              "the sender is excluded from the broadcast - SDM Figure "
              "13-12's 'all excluding self'");
        check_equal(0x8,
                    state->start_up_attempts[i].vector,
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

    check_equal(3, state->start_up_attempt_count, "three targets started");
    for (std::size_t i{}; i < state->start_up_attempt_count; ++i) {
        check(4 != state->start_up_attempts[i].destination,
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
    state->needs_hardware_for[0] = 2;
    state->needs_hardware_count = 1;

    auto command = icr{.vector = 0x8,
                       .delivery_mode = delivery_start_up,
                       .shorthand = shorthand_all_excluding_self}
                       .value();

    auto answer = state->on_interrupt_command(command);

    check(!answer.has_value(),
          "the broadcast is still swallowed - the targeted command below "
          "replaces it for the one processor that needed it");
    check_equal(1,
                state->hardware_ipi_count,
                "one targeted start-up ipi went to hardware");
    check_equal(2,
                state->hardware_ipis[0].destination,
                "naming the processor that needed it");
    check_equal(0x8,
                state->hardware_ipis[0].vector,
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
    check_equal(0, state->start_up_attempt_count, "nothing was started");
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
        for (std::size_t i{}; i < state->start_up_attempt_count; ++i) {
            check(0xdeadbeef != state->start_up_attempts[i].destination,
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

    for (std::uint64_t id = 1; id < hypervisor::max_cpus; ++id) {
        auto slot = state->processor_slot(id);
        check(slot.has_value(),
              "id " + std::to_string(id) + " fits in the table");
    }

    check_equal(hypervisor::max_cpus,
                state->number_of_known_processors,
                "the table is exactly full");

    auto overflow = state->processor_slot(0x1000);
    check(!overflow.has_value(),
          "the identifier past the last slot is refused rather than "
          "folded onto an existing one");
    check_equal(hypervisor::max_cpus,
                state->number_of_known_processors,
                "and the count does not grow past the table");
}

} // namespace

int main()
{
    passthrough_delivery_modes();
    init_is_forwarded_and_counted();
    init_level_de_assert_starts_nothing();
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

    std::printf(
        "local_apic: %zu checks, %zu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
