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
 * Whether the loader declares a reserved memory window for the storage
 * controller to the guest.
 *
 * Separate from any sink's presence on purpose. Declaring the window and
 * borrowing the controller's admin queue are independent mechanisms with
 * independent ways of going wrong, and a boot that changes both at once
 * cannot say which one did anything. They shared one switch until a boot
 * was needed that exercised exactly one of them.
 *
 * **Off by default**, and no longer merely following `enabled`. Tying it
 * to the facility meant it was on for every build that carried any
 * diagnostics at all - which is every rig build - so a boot that wanted
 * only a log ring also rewrote the guest's DMAR table, reserved a tail
 * of the EFI system partition, and restarted the machine to pick the
 * reservation up. None of that is free and none of it was asked for.
 *
 * `-DZPP_DIAG_RESERVE_WINDOW=ON` asks for it. That it needs asking is
 * the point: it edits firmware tables the guest reads and takes space
 * from a real partition on a real disk.
 */
inline constexpr bool reserve_controller_window =
#if defined(ZPP_DIAG_RESERVE_WINDOW) && ZPP_DIAG_RESERVE_WINDOW
    enabled;
#else
    false;
#endif

/**
 * How much of the EFI system partition to take for the log, in
 * megabytes.
 *
 * Taken by shrinking the file system out of a tail of the partition, so
 * this is space no file can be allocated in and nothing can delete.
 *
 * It is a request rather than a demand. A partition that cannot give
 * this much gives what it can - see `esp_reservation_minimum_megabytes`
 * - because a smaller log is worth having and a machine refusing to log
 * at all because it could not spare sixty four megabytes is worth
 * nothing.
 */
inline constexpr std::uint32_t esp_reservation_megabytes = 64;

/**
 * The least worth taking.
 *
 * Below this the log holds so little that the first interesting thing
 * would already have scrolled out of it, and the file system has been
 * modified for no benefit. Refuse instead, and say so.
 */
inline constexpr std::uint32_t esp_reservation_minimum_megabytes = 1;

/**
 * Whether to restart immediately after establishing the reservation.
 *
 * The reservation cannot be used on the boot that creates it. The
 * firmware's file system driver mounted the volume before the loader ran
 * and still believes it owns the range, so the loader would be reserving
 * blocks the driver may hand out for the rest of that boot - and the
 * loader itself writes a trace file to that very partition later on. The
 * channel therefore stays dark until the next boot, when the driver
 * mounts a file system that ends where we left it.
 *
 * "The next boot" can be now. The reservation is established exactly
 * once in the life of a machine, so this costs one extra restart ever,
 * and it buys a channel that is live the first time the feature is
 * switched on rather than the second - which matters most on a machine
 * somebody has to walk over to.
 *
 * The loop this could become is worth naming. A restart that finds the
 * reservation missing would establish it and restart again, forever. Two
 * things stop it: the new total is computed from the partition's size
 * rather than the current one, so a second pass over an already reserved
 * volume changes nothing and reports "already reserved"; and the shrink
 * is read back and refused if it did not take, so a restart is only
 * reached after the medium has confirmed the write. If a machine ever
 * does loop, this is the switch to turn off.
 */
inline constexpr bool restart_after_reservation = enabled;

/**
 * Whether to rebuild the log channel's queue pair after the guest's
 * driver has reset the controller.
 *
 * **Off, and it has been tried.** The design is sound and the pieces all
 * work: the reset is detected, the mapping window reaches the guest's
 * admin queue at an address we do not choose, and the borrow itself is
 * proven byte-exact against this controller. What does not work yet is
 * the timing.
 *
 * The borrow needs the admin queue to itself, and the one moment that is
 * free is the CSTS.RDY wait the driver enters straight after writing
 * CC.EN - a wait it must allow the controller CAP.TO half-seconds to
 * finish. Being inside that window means noticing the enable
 * immediately, and neither way of noticing is immediate enough:
 *
 * - Catching the write does not work, because a watch lets the guest's
 *   write land by making the page writable and stepping one instruction,
 *   and for that window the page is writable for every processor. A
 *   driver writes CC twice in succession, disable then enable, and the
 *   second write goes through the window the first opened. Measured: one
 *   trapped write of 0x00460000, a controller afterwards reading
 *   0x00460001, and no transition seen.
 * - Polling the register on our own exits does notice it, but late. The
 *   driver's own CSTS.RDY polling is memory mapped reads to a passed
 *   through device, which cause no exits at all, so nothing forces us to
 *   look during precisely the window that matters. By the time we do,
 *   the driver has begun submitting its own admin commands and the
 *   borrow is no longer alone in the queue. Measured: a borrow that
 *   spent its whole 285 ms budget and returned timed_out.
 *
 * A timed out borrow leaves the guest's admin queue desynchronised,
 * which its driver survives only by resetting the controller. That is
 * not a thing to leave switched on.
 *
 * What it needed was emulating the guest's write instead of stepping
 * over it, so the page is never writable and no second write can slip
 * past. That is what a VMM with an instruction decoder does, and this one
 * now has one: `emulate_watched_page_writes` is on, and the four defects
 * that kept it off are closed, each with a check behind it.
 *
 * Measured on the rig with emulation on and this still off: 197 writes to
 * watched pages emulated rather than stepped, and zero disagreements
 * between the decoder's instruction length and the processor's. So the
 * window the second write used to slip through is closed, which is the
 * one thing this was waiting for.
 *
 * **Turned on, run, and turned back off again.** The emulation was
 * necessary and is not sufficient.
 *
 * What worked: the enable transition is seen, the borrow completes, and
 * the channel comes back on a new queue pair. The proof is on the medium
 * - the newest boot's blocks carry epoch 2, and the epoch only advances
 * when the queue is rebuilt, so every earlier boot in the region reads
 * epoch 1. It wrote 28 blocks where the same build with this off stopped
 * at 18, which is the channel continuing past the reset that used to end
 * it.
 *
 * What did not: the guest boot loops. Measured as twelve runs of the
 * loader in four and a half minutes, and independently as a different
 * kernel base in every sample, which is a fresh boot each time rather
 * than one guest moving around. The same build with only the emulation
 * on boots and settles, so this switch is the difference.
 *
 * **Why it boot loops was then diagnosed, and it was not the timing.**
 * Two causes, both measured, both recorded at the end of NVME-LOG.md:
 *
 * - **The identifier collided.** The loader hardcodes I/O queue 4 and
 *   the resident side inherited it. With `observe_controller_admin` on,
 *   the guest's own admin queue says what it does: it asks for sixteen
 *   submission queues and eight completion queues, then creates
 *   completion queues 1 to 8 and submission queues 1 to 16. Four is
 *   inside both ranges, so the guest's own Create for it is refused, its
 *   storage initialisation fails, and it bugchecks and resets - which
 *   re-enters the rebuild, which recreates the queue, which is why it
 *   never converged. Raising the identifier to 32 was refused with
 *   status 0x4101, Invalid Queue Identifier, because it exceeded the
 *   allocation - and the guest then booted, which is the control that
 *   isolates queue creation as the thing that breaks it.
 * - **The ordering was wrong.** NVMe Base 5.2.30.1.5 says a Set Features
 *   (Number of Queues) submitted after any I/O queue has been created is
 *   aborted with Command Sequence Error, that the allocation is cleared
 *   by a controller level reset, and that it is fixed by the first Set
 *   Features completed after one. Creating our queues at the CC.EN edge
 *   put them before the guest's own Set Features.
 *
 * So this switch now runs the sequence NVME-LOG.md's "The order to use"
 * prescribes, and it is split across two switches because the two halves
 * carry very different risk:
 *
 * **This one is the reservation, steps 1 to 3.** At the CC.EN 0 to 1
 * edge, borrow the admin queue once and issue Set Features (Number of
 * Queues) asking for the maximum, **creating nothing**. The guest's own
 * Set Features arrives later with still no I/O queue created, so it
 * completes, and the controller reports the allocation reserved here -
 * which is at least what the guest asked for. No completion patch is
 * needed and none is built: an earlier section of NVME-LOG.md describes
 * one, and this ordering makes it obsolete.
 *
 * On its own this creates no queue and therefore restores no channel. It
 * is the control run: everything the borrow does happens, at the same
 * moment it did when the guest boot looped, and the only difference is
 * that nothing is created. A guest that boots normally with this on has
 * cleared the borrow itself of the boot loop.
 *
 * The doorbell page is watched while this is on, from the enable until
 * the queue is created or given up on, which is what records the guest's
 * Create commands. That costs an exit per admin command and, with a
 * doorbell stride of zero, per I/O command too - see
 * observe_controller_admin, which exists to price exactly that.
 */
inline constexpr bool rebuild_channel_after_reset = false;

/**
 * Whether to create the channel's own queue pair once the guest has
 * created its own - step 4, and the unproven half.
 *
 * Needs rebuild_channel_after_reset, which reserves the allocation this
 * spends. Off independently of it so that the reservation can be run on
 * its own first: a boot loop with only that on is a fault in the borrow,
 * and a boot loop with this on as well is a fault in creating a queue
 * behind a live driver. Those are different problems and one boot each
 * separates them.
 *
 * **The identifier is chosen from what the guest is observed to create,
 * never from what it was told.** The guest is told the whole allocation,
 * which after asking for the maximum is far more than it wants, and
 * NVME-LOG.md's observation notes that no run has yet shown what this
 * driver does with an allocation larger than its request. What is
 * observed is its admin submission queue: the Number of Queues it asks
 * for, and every Create I/O SQ and Create I/O CQ identifier it actually
 * issues, tracked in the two separate spaces. Ours is one above the
 * higher of what it created and what it asked for, in each space, and
 * only if that is still inside the allocation - otherwise nothing is
 * created and the channel stays down, which is the outcome that lets the
 * guest boot.
 *
 * **The honest part: this borrows the admin queue while the guest's
 * driver is live**, which is the quiescence problem that has blocked
 * this all along. What makes it defensible rather than merely narrower
 * than before:
 *
 * - It runs from inside the VM exit of the guest's own doorbell write
 *   for its last Create, so the processor that drives storage
 *   initialisation is held in our handler and cannot submit anything.
 *   Windows' storage stack is single threaded at that point.
 * - The doorbell page is held for the borrow, so any other processor
 *   that rings any doorbell stops at the faulting instruction with its
 *   write not yet applied. The hold is preceded by an acknowledgement
 *   wait, so no processor can still be running on a stale translation.
 * - The wait probes with a wake NMI, which is usable again: a processor
 *   that takes one in root mode now returns from it instead of halting
 *   for ever. That is the whole reason a passive wait was needed before,
 *   and passive waits are what timed out rather than succeeding.
 * - The borrow starts only once the guest's admin queue is quiescent,
 *   measured rather than assumed: the located submission tail and
 *   completion tail have to stop moving before a command of ours is
 *   submitted. The guest's last Create is in flight when the doorbell
 *   write is seen, and borrowing over it would overwrite entries the
 *   controller has not fetched.
 *
 * **What is not solved, stated rather than hidden:**
 *
 * - The admin queue's interrupt is not masked. NVME-LOG.md says vector 0
 *   must be masked in the controller's MSI-X table for the duration, and
 *   no code here can do that - there is no MSI-X table access on the
 *   resident side. The argument standing in for it is that the only
 *   processor which could take that interrupt is the one held inside
 *   this exit with interrupts disabled, so the interrupt stays pending
 *   in its local APIC and is delivered after everything is restored,
 *   where it finds the guest's own completions and nothing else. That is
 *   an argument about when this runs, not a property of what it does.
 * - The borrow rings the guest's completion queue doorbell as it laps,
 *   so at the end the controller believes the guest has consumed
 *   entries it may not have read yet. That is pre-existing in
 *   admin_borrow and is why the quiescence wait matters.
 * - Neither emulator can exercise any of this: QEMU models NVMe but has
 *   no VT-x, Bochs has VT-x and no NVMe at all. It is first run on
 *   hardware.
 */
inline constexpr bool create_channel_queue_after_guest = false;

/**
 * Whether to reduce what the guest is told it was granted, so that one
 * submission queue identifier is left over for the channel.
 *
 * **This is the piece that makes create_channel_queue_after_guest able to
 * succeed at all on the development machine, and it is needed because of
 * a measurement rather than because of a design.** Measured on the rig
 * with the reservation on: asking for the maximum is granted DW0 =
 * 0x000f000f - sixteen submission and sixteen completion queues, both
 * halves zero's based - and that is everything the controller has. The
 * guest, read off its own admin submission queue, asks for sixteen
 * submission and eight completion queues and then creates submission
 * queues 1 to 16 and completion queues 1 to 8. So there are eight spare
 * completion identifiers and **no spare submission identifier at all**,
 * and reserving a larger allocation cannot manufacture one, because the
 * guest wants the whole of it.
 *
 * The one remaining lever is what the guest *believes*. NVMe Base
 * 5.2.30.1.5 freezes the allocation at the first Set Features (Number of
 * Queues) completed after a controller level reset; ours is that one, so
 * the controller's allocation is sixteen and cannot change. The guest's
 * own Set Features arrives second and is answered with the same
 * allocation - and both drivers clamp their creates to
 * `min(their request, what they were told)`: Linux in
 * `nvme_set_queue_count`, `nr_io_queues = min(result & 0xffff, result >>
 * 16) + 1` followed by `*count = min(*count, nr_io_queues)`, and Windows'
 * stornvme to NSQA+1 and NCQA+1. Telling the guest one less than the
 * controller granted therefore leaves exactly one identifier that is
 * valid for us and unreachable for it.
 *
 * **How the answer is edited, and why it needs no lap.** The completion
 * is a DMA write by the controller, so there is no VM exit for it. What
 * there *is* an exit for is the guest's admin submission doorbell, which
 * is already watched. The processor that rang it is held inside that
 * exit, so it has not yet looked at its completion queue - and the
 * completion for its own command lands in its own queue at its own slot
 * with its own phase, posted by the controller for a command the guest
 * itself submitted. Nothing has to be injected, no doorbell is rung, and
 * the controller's completion queue tail is not disturbed: four bytes of
 * DW0 are rewritten in place, in an entry the controller has finished
 * writing and will never read back (Base 2.0 3.3.1.2, the tail "is only
 * used internally by the controller and is not visible to the host").
 *
 * **Substitution was considered and rejected**, and it was the shape
 * NVME-LOG.md favoured, so the reason matters. It would put a command of
 * ours into the guest's submission slot before the doorbell landed and
 * then rewrite both the command identifier and DW0 of the resulting
 * completion. Two things kill it. The watch calls its handler *after*
 * the store has been applied - see page_watch::handler, "called after the
 * write has taken effect" - so the doorbell has already rung by the time
 * anything of ours runs, and deferring it is new machinery on the one
 * path that must not be wrong. And a substituted command puts a
 * completion carrying *our* identifier into the guest's queue for a
 * window, which is strictly worse than the guest's own identifier under
 * the unmasked interrupt below: an unrecognised command identifier handed
 * to a closed source boot disk miniport is the risk this whole design
 * avoids everywhere else.
 *
 * **What the unmasked admin interrupt can do here, stated exactly,
 * because it is different from what it can do to a lap.** Vector 0 is
 * still not masked - there is no MSI-X table access on the resident side
 * - so a guest interrupt service routine on another processor could read
 * that completion between the controller posting it and this rewriting
 * it. The entry it would find is the guest's own, with the guest's own
 * command identifier and the controller's own status; the only thing
 * wrong with it is that DW0 says sixteen rather than fifteen. So the
 * failure mode of losing that race is **no channel** - the guest creates
 * sixteen submission queues, create_channel_queue finds no room and
 * refuses, and the guest boots. That is the same outcome as the switch
 * being off. The lap in create_channel_queue_after_guest has the real
 * exposure and still carries the argument written there.
 *
 * Only the half that has no spare is reduced. On this machine that is the
 * submission half alone: the guest asks for eight completion queues
 * against an allocation of sixteen, so NCQA is left exactly as the
 * controller wrote it and the guest's completion queue arrangement is
 * untouched.
 *
 * Off by default. Turning it on changes what a live driver is told about
 * its own controller, which is the most invasive thing in this file.
 */
inline constexpr bool reduce_guest_queue_grant = false;

/**
 * Whether to watch the controller's doorbell page and record what the
 * guest's driver submits on its admin queue.
 *
 * Observation only. Nothing is emulated, nothing is created, and no
 * command of ours is submitted - the handler reads the guest's own
 * submission queue entries and records their opcode, identifier and the
 * two command dwords, which is enough to read a Set Features (Number of
 * Queues) request and every Create I/O Queue.
 *
 * It exists because every remaining decision about surviving the guest's
 * controller reset currently rests on a guess about the guest: what its
 * driver asks for, what it does with an allocation larger than it asked
 * for, the highest queue identifier it actually creates in each space,
 * how many times it disables the controller in a boot and in an idle
 * hour, and whether its low power entry clears the enable bit at all or
 * only sets the shutdown notification - which this VMM does not currently
 * look at. One boot with this on settles all of them.
 *
 * The cost is the point as much as the facts are. With a doorbell stride
 * of zero every I/O doorbell shares this page with the admin ones, so
 * this traps the guest's entire disk traffic, and how much that actually
 * costs has been asserted to be disqualifying without ever being
 * measured. `channel_doorbell_writes` counts them, so the boot that
 * gathers the facts also prices the mechanism.
 *
 * Off by default and never appropriate to leave on: it is an exit per
 * command submitted.
 */
inline constexpr bool observe_controller_admin = false;

/**
 * Whether to take the controller for the length of one VM exit when the
 * guest disables it, to write out whatever is staged.
 *
 * This is the alternative to the rebuild above, and it is safe in the two
 * ways that one is not. The guest's admin queue is never touched - ours is
 * programmed into ASQ and ACQ instead, which is legal only while CC.EN is
 * clear and is therefore exactly what the guest has just made true. And
 * the excursion is bracketed by two controller resets, so the guest's own
 * Set Features is still the first admin command after the last one, and
 * our queue is destroyed before it creates any of its own - so neither the
 * ordering hazard nor an identifier collision can arise.
 *
 * The guest's view of the register page is redirected to ordinary memory
 * for the duration, so a driver polling CSTS on another processor keeps
 * seeing the controller it believes it disabled rather than one that
 * appears to have come back by itself. Writers are held. That is what
 * makes it race free rather than merely narrow, and it needs no
 * instruction decoding: reads go to the shadow at full speed and writes
 * still fault.
 *
 * What it does not give is continuity between resets. The controller is
 * handed back disabled, as the guest asked, so the private queue is gone
 * again and the next window is the next reset.
 *
 * **Switched off, and it took the development machine off the network the
 * first time it ran.** Not the guest - the host, which had to be power
 * cycled. What is almost certainly wrong is the shadow's use of
 * wait_for_ept_acknowledgement while the register page is held: a
 * processor that faults on the held page spins inside its own VM exit, in
 * root mode, and the acknowledgement wait sends it a wake NMI. NMI
 * exiting governs non-root operation only, so that NMI goes to the host
 * IDT, reaches on_host_exception with no recovery point armed, and halts
 * that processor for ever. Do that to several and every vCPU thread spins
 * at once.
 *
 * So the ordering has to change before this is tried again: acknowledge
 * the extended page table change *before* taking the hold, never while
 * holding it, and never send a wake NMI to a processor that may be in
 * root mode. Until then this stays false.
 */
inline constexpr bool excursion_at_controller_reset = false;

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
        // Off, and deliberately so rather than by omission.
        //
        // Turning it on does two things at once: it makes the resident
        // side write log blocks, and it makes the loader run the self
        // test - which borrows the controller's admin queue and *writes
        // to the disk* on whatever machine it boots.
        //
        // The destination is no longer the missing piece: the ESP
        // reservation resolves one, and fills in a log_target with the
        // namespace, the block size, both GUIDs and the absolute first
        // LBA of the reserved tail. What is missing is the join. The
        // hand-over the loader passes across carries a target field that
        // nothing ever fills, and the self test writes its proof block
        // to a hard coded LBA instead - which is fine against an
        // emulated disk and is not fine here, where that LBA lands
        // inside the EFI system partition among real boot files rather
        // than in the reserved tail.
        //
        // So this stays off until the loader runs the reservation
        // *before* the self test, hands the resolved target to it, and
        // the proof write goes to the reserved region like every write
        // after it. Enabling it before that would corrupt the volume it
        // is supposed to be logging to.
        //
        // The code behind it does not rot while it is off.
        // hypervisor/src/diag/instantiate.cpp explicitly instantiates
        // every sink so the freestanding toolchain compiles all of it
        // regardless, which is the trap this file warns about elsewhere.
        //
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
static_assert(!reduce_guest_queue_grant || rebuild_channel_after_reset,
              "reducing the grant needs the reservation that establishes "
              "the allocation, and the doorbell watch it arms");
static_assert(!create_channel_queue_after_guest ||
                  rebuild_channel_after_reset,
              "creating a queue needs the allocation the reservation "
              "spends");
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
