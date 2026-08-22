#pragma once
#include "zpp/arch/x86_64/asm.h"
#include "zpp/arch/x86_64/vmx/asm.h"
#include "zpp/arch/x86_64/vmx/evmcs.h"
#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
#include "zpp/error.h"
#include <cstdint>
#include <expected>

namespace zpp::arch::x86_64::vmx
{
/**
 * How many VMCS field accesses this processor has executed.
 *
 * A diagnostic, and the only way to stop guessing at the number that
 * decides everything about this VMM's cost. Nested under a hypervisor
 * that does not offer VMCS shadowing, every one of these is an exit to
 * the layer below - measured at about 3,735 cycles for a read and 2,542
 * for a write, which this VMM prints at launch - so the access *count*
 * per exit is the cost, and it had been estimated twice from cycles
 * divided by those prices and both estimates informed a wrong decision.
 *
 * Deliberately not per-processor and not atomic. One counter that is
 * occasionally short by a racing increment answers "about how many per
 * exit" exactly as well as an exact one, and a `lock` prefix here would
 * be a real cost added to the hot path to measure the hot path.
 *
 * `constinit` and never read by anything that decides: it is compiled
 * in unconditionally because a switch would leave the number available
 * only in a build nobody runs, which is how `ZPP_PUBLISH_REFERENCE_TSC`
 * came to be measured against a stale object file.
 */
inline constinit std::uint64_t vmcs_reads_taken{};
inline constinit std::uint64_t vmcs_writes_taken{};

/**
 * Which fields those accesses name, as a table rather than a ring.
 *
 * The count above says 54 reads an exit and the guest-state deferral
 * already reports 0.9 of them, so 53 are something else and no
 * instrument here could say what. KVM's hot exit path reads about eight
 * fields - `sync_vmcs02_to_vmcs12` reads RFLAGS, two AR bytes and the
 * interruptibility state, takes RIP and RSP from its own register cache
 * and computes the activity state - and defers the rest behind
 * `need_sync_vmcs02_to_vmcs12_rare` until L1 actually reads one. Which
 * of our fifty-three are that same rare set is the whole question, and
 * it is answerable only by field.
 *
 * **Both directions, because a read table alone cannot be checked.** The
 * write path used to bump `vmcs_writes_taken` and nothing else, so the
 * 21% of accesses that are writes had no breakdown at all and there was
 * no second quantity for the read table to disagree with. Same shape,
 * same slot function, so a field's two rows sit at the same index.
 *
 * **Direct-mapped and injective, which the first version was not.** It
 * hashed `(encoding >> 1) ^ (encoding >> 9)` into 64 slots and reported
 * `vmcs_read_overflow` of 532,254,425 against 218,870 reads a second
 * tracked - more accesses lost than kept, so a field's absence from the
 * table proved nothing at all and the reading had to be published with
 * that caveat attached.
 *
 * Widening it would not have helped: measured over the 156 encodings in
 * `vmcs_fields.h`, that hash occupies only 60 slots at 64, at 128, at
 * 256 and at 512, because the encodings differ mostly in bits the two
 * shifts fold on top of each other. The slot is taken from the
 * encoding's own structure instead - SDM 25.11.2, "Field Encoding in
 * VMCS": bits 9:1 the index, bits 11:10 the type, bits 14:13 the width.
 * Every index in this tree is 25 or less, so five bits of index, two of
 * type and two of width is a nine-bit key, and over those 156 encodings
 * it collides **zero** times.
 *
 * The identity check and `overflow` are kept anyway, for the case the
 * five-bit index assumption stops holding - an encoding with an index
 * above 31 aliases one below it, and this reports that rather than
 * silently adding the two together.
 */
inline constexpr std::size_t vmcs_use_slots = 512;

/**
 * The slot an encoding occupies in the tables above. See them for why
 * it is a projection of the encoding's fields rather than a hash.
 */
inline constexpr std::size_t vmcs_use_slot(std::uint64_t encoding)
{
    return static_cast<std::size_t>(((encoding >> 1) & 0x1f) |
                                    (((encoding >> 10) & 3) << 5) |
                                    (((encoding >> 13) & 3) << 7));
}

inline constinit std::uint64_t vmcs_read_field[vmcs_use_slots]{};
inline constinit std::uint64_t vmcs_read_hits[vmcs_use_slots]{};
inline constinit std::uint64_t vmcs_read_overflow{};

inline constinit std::uint64_t vmcs_write_field[vmcs_use_slots]{};
inline constinit std::uint64_t vmcs_write_hits[vmcs_use_slots]{};
inline constinit std::uint64_t vmcs_write_overflow{};

/**
 * Records one access against its field, for either table.
 *
 * A slot is claimed by the first encoding to reach it and never evicted,
 * so a count in the table is always a count of the field named beside
 * it. Anything that lands on a claimed slot with a different encoding is
 * counted as overflow rather than added to the sitting tenant.
 */
inline void vmcs_record_use(std::uint64_t encoding,
                            std::uint64_t (&fields)[vmcs_use_slots],
                            std::uint64_t (&hits)[vmcs_use_slots],
                            std::uint64_t & overflow)
{
    auto slot = vmcs_use_slot(encoding);

    if (0 == hits[slot]) {
        fields[slot] = encoding;
    }

    if (fields[slot] == encoding) {
        hits[slot] = hits[slot] + 1;
    } else {
        overflow = overflow + 1;
    }
}

/**
 * Which *code* makes the reads, as opposed to which fields they name.
 *
 * The field table above says `guest_rip` is read 4.6 times an exit and
 * `vm_entry_controls` 2.6, and cannot say by whom - so it cannot say
 * whether those are one caller in a loop or six callers each asking
 * once, and those want opposite fixes. The phase tree has the same blind
 * spot from the other side: on a vmcall it attributes 62 of the 112
 * accesses to named phases and leaves **50.5 reads in "residue"**, which
 * is the largest single item in this VMM's cost and is currently
 * anonymous.
 *
 * This is the instrument that closed exactly this question once already.
 * `note_guest_memory_caller` named `l2_physical_to_l1` as 83.8% of every
 * guest-memory read in one reading, after two sessions of guessing - so
 * the same shape is used here rather than a third round of reasoning
 * about which phase "ought" to be expensive.
 *
 * Return addresses rather than field encodings, resolved offline against
 * the ELF with `info symbol`; the module base moves per run, so what is
 * stored is the raw address and the reader subtracts.
 *
 * Deliberately shared and non-atomic, for the reason `vmcs_reads_taken`
 * gives directly above: a table occasionally short by a racing increment
 * answers "who reads the VMCS" exactly as well as an exact one, and a
 * `lock` prefix here would be a real cost added to the hot path in order
 * to measure the hot path. Linear probe over a small table, and a miss
 * is counted rather than folded into a sitting tenant - an entry that
 * silently absorbed another caller's hits would be worse than an absent
 * one, since it would read as a confident wrong answer.
 */
inline constexpr std::size_t vmcs_caller_slots = 48;

inline constinit std::uint64_t vmcs_read_caller[vmcs_caller_slots]{};
inline constinit std::uint64_t vmcs_read_caller_hits[vmcs_caller_slots]{};
inline constinit std::uint64_t vmcs_read_caller_overflow{};

inline void vmcs_note_read_caller(std::uint64_t caller)
{
    for (std::size_t slot{}; slot < vmcs_caller_slots; ++slot) {
        if (0 == vmcs_read_caller[slot]) {
            vmcs_read_caller[slot] = caller;
            vmcs_read_caller_hits[slot] = 1;
            return;
        }

        if (vmcs_read_caller[slot] == caller) {
            vmcs_read_caller_hits[slot] =
                vmcs_read_caller_hits[slot] + 1;
            return;
        }
    }

    vmcs_read_caller_overflow = vmcs_read_caller_overflow + 1;
}

/**
 * A per-processor cache of VMCS fields, valid from one VM exit to the
 * next VM entry. **Off unless `ZPP_VMCS_CACHE` is set.**
 *
 * **Why it is sound.** The processor writes VMCS fields only on VM entry
 * and VM exit. Between our handler starting and the entry that ends it,
 * no field changes unless *we* write it, so a value read once is good for
 * the rest of that window. Three things end the window - a VM exit, a
 * `vmptrld` and a `vmclear` - and all three bump the epoch below.
 *
 * **Why it is worth having.** Measured: 60,562,642 reads over 1,975,342
 * exits, 30.7 a call, at ~3,529 cycles each, because nested under KVM
 * every access to a field it does not shadow is a VM exit into KVM - 58
 * KVM exits for every exit this VMM takes. The caller census says 37.8%
 * of them are repeats of four fields, `guest_rip` alone 4.6 times an exit.
 *
 * **The epoch is global and that is deliberate.** Invalidating every
 * processor's row when any processor loads a VMCS is *over*-invalidation,
 * and over-invalidation is always safe - it costs a miss, never a wrong
 * answer. It buys something worth more than the precision: the wrappers
 * need no idea which processor they are on, so **nothing on the launch
 * path has to touch GS**.
 *
 * That distinction is the whole design, and it was learned the expensive
 * way. A first version read the processor index through GS on every
 * access, including from the `vmptrld` inside `enter_root_mode`. GS is
 * only this VMM's *in root mode with our page tables loaded* - the
 * comment on `hypervisor::host_gs_processor_index` says so in terms - and
 * on the launch path it still holds whatever the loader left, so the read
 * faulted and the launch died with loader code **0x60e00**, zero exits on
 * every processor. Nothing on that path may read GS.
 *
 * So GS is read only from `read`/`write`, and `setup_vmcs` points the
 * real GS base at this processor's row before it issues its first VMCS
 * access. Ordering there is load bearing.
 * @{
 */
#ifndef ZPP_VMCS_CACHE
#define ZPP_VMCS_CACHE 0
#endif

inline constexpr bool vmcs_cache_enabled = (0 != ZPP_VMCS_CACHE);

inline constexpr std::size_t vmcs_cache_processors = 32;
inline constexpr std::size_t vmcs_cache_entries = 128;

/** Byte offset in the per-processor GS row holding the arming token. */
inline constexpr std::uint64_t vmcs_cache_token_offset = 8;

/** High 56 bits of the token; the low 8 are the processor index. */
inline constexpr std::uint64_t vmcs_cache_token_magic = 0x5a70705643414300;
inline constexpr std::uint64_t vmcs_cache_token_index_mask = 0xff;

/**
 * How many VMCSs a processor can cache at once.
 *
 * **Four because three are live.** A nested round trip moves between
 * vmcs01, vmcs02 and the shadow VMCS, and with a single row per processor
 * every one of those moves threw the row away - measured at 69,419,564
 * epoch bumps over 13,305,860 exits, 5.3 per exit, for a 26.1% hit rate
 * over 429,893,345 reads. A row per VMCS makes the move itself free: what
 * vmcs01 held is still true while vmcs02 is current, because the
 * processor only writes the VMCS that is current.
 *
 * The fourth is headroom, so that adding one more VMCS to the round trip
 * does not silently restore the thrashing this exists to remove.
 */
inline constexpr std::size_t vmcs_cache_sets = 4;

struct vmcs_cache_row
{
    /** The field encoding plus one, so that zero means empty. */
    std::uint64_t tag[vmcs_cache_entries];
    std::uint64_t value[vmcs_cache_entries];
    std::uint64_t epoch;

    /**
     * When non-zero, this row does not describe a VMCS at all: it is the
     * address of an *enlightened* VMCS, a page shared with the layer
     * below, and every access to it is a load or a store rather than a
     * VMREAD or a VMWRITE. See `nested_vmx::evmcs_to_kvm`.
     */
    std::uint64_t evmcs;

    /**
     * The VMCS this row describes, as the physical address `vmptrld` was
     * given, or zero for a row that describes nothing.
     *
     * This is what makes a row survive a pointer change. It is only ever
     * set by the processor-aware `vmptrld`, and dropped whenever the
     * global epoch moves - so a row can never be matched against a VMCS
     * loaded by a path that did not go through it.
     */
    std::uint64_t vmcs;
};

inline constinit vmcs_cache_row
    vmcs_cache[vmcs_cache_processors][vmcs_cache_sets]{};

/** Which of this processor's rows the current VMCS is. */
inline constinit std::uint64_t
    vmcs_cache_active[vmcs_cache_processors]{};

inline constinit std::uint64_t vmcs_cache_epoch{1};

/**
 * Non-zero while a caller is borrowing the current VMCS pointer and will
 * hand it back - the shadow-VMCS copies, which `vmptrld` away, touch only
 * the shadow, and `vmptrld` back.
 *
 * **This is what the hit rate turns on.** Measured: the epoch was bumped
 * 9,584,003 times over 2,076,513 exits, 4.6 window-ends an exit, and only
 * one of those is the exit itself. The rest are the two shadow copies,
 * each of which loads a pointer, clears it and loads the old one back -
 * three bumps apiece, twice a round trip, every one of them wiping fields
 * belonging to a VMCS that never stopped being the one we care about.
 *
 * While it is set, `read` and `write` neither consult nor fill the cache,
 * so nothing belonging to the borrowed VMCS can enter a row. The borrower
 * then calls `vmcs_cache_revalidate` once its own `vmptrld` back has
 * happened, which marks the row current again **without clearing it** -
 * sound precisely because the row still describes the VMCS that is
 * current again, and nothing in between could have changed it.
 *
 * Global rather than per-processor, and safe that way round: another
 * processor seeing it set merely stops caching for a moment. The one
 * thing that would *not* be safe is restoring the epoch to a saved value,
 * because a second processor may have bumped it meanwhile and lowering it
 * would revive that processor's stale rows. Hence re-validating a row
 * forward to the current epoch rather than winding the epoch back.
 */
inline constinit std::uint64_t vmcs_cache_suspended{};
inline constinit std::uint64_t vmcs_cache_revalidations{};
inline constinit std::uint64_t vmcs_cache_hits{};
inline constinit std::uint64_t vmcs_cache_misses{};
inline constinit std::uint64_t vmcs_cache_unarmed{};

/**
 * Ends the window every cached value describes. No GS, no processor
 * index, no memory beyond one counter - which is what makes it safe to
 * call from the launch path and from inside the instruction wrappers.
 */
inline void vmcs_cache_forget()
{
    if constexpr (vmcs_cache_enabled) {
        vmcs_cache_epoch = vmcs_cache_epoch + 1;
    }
}

/**
 * Which row this processor owns, or `vmcs_cache_processors` when the
 * cache is not armed on it.
 *
 * A token rather than a bare index because a bare index taken from a GS
 * base that is not ours would be garbage that is *in range* often enough
 * to matter, and would serve one processor's fields from another's row:
 * no fault, just a wrong answer later.
 */
inline std::size_t vmcs_cache_row_index()
{
    auto token = gs_qword(vmcs_cache_token_offset);

    if (vmcs_cache_token_magic != (token & ~vmcs_cache_token_index_mask)) {
        vmcs_cache_unarmed = vmcs_cache_unarmed + 1;
        return vmcs_cache_processors;
    }

    auto index = static_cast<std::size_t>(token &
                                          vmcs_cache_token_index_mask);
    return (index < vmcs_cache_processors) ? index
                                           : vmcs_cache_processors;
}

/**
 * Marks this processor's row current again without discarding it.
 *
 * Only correct when the VMCS current now is the one the row already
 * describes, and nothing has changed a field of it in between - which is
 * exactly the shadow-copy case `vmcs_cache_suspended` exists for.
 */
inline void vmcs_cache_revalidate()
{
    if constexpr (vmcs_cache_enabled) {
        auto row = vmcs_cache_row_index();

        if (row < vmcs_cache_processors) {
            vmcs_cache[row][vmcs_cache_active[row]].epoch =
                vmcs_cache_epoch;
            vmcs_cache_revalidations = vmcs_cache_revalidations + 1;
        }
    }
}

/**
 * Holds the cache still across a borrow of the VMCS pointer, and hands
 * the row back on the way out. See `vmcs_cache_suspended`.
 */
class vmcs_cache_borrow
{
public:
    vmcs_cache_borrow()
    {
        if constexpr (vmcs_cache_enabled) {
            vmcs_cache_suspended = vmcs_cache_suspended + 1;
        }
    }

    ~vmcs_cache_borrow()
    {
        if constexpr (vmcs_cache_enabled) {
            vmcs_cache_suspended = vmcs_cache_suspended - 1;

            // After the borrower's own `vmptrld` back, so the pointer is
            // the one the row describes again.
            if (0 == vmcs_cache_suspended) {
                vmcs_cache_revalidate();
            }
        }
    }

    vmcs_cache_borrow(const vmcs_cache_borrow &) = delete;
    vmcs_cache_borrow & operator=(const vmcs_cache_borrow &) = delete;
};

/**
 * `vmptrld` and `vmclear`, wrapped so the cache cannot be left describing
 * a VMCS that is no longer current.
 *
 * Here rather than at the call sites deliberately: there are twelve
 * `vmptrld` sites and seven `vmclear` sites, and a design where one can
 * be missed is one that will miss one. The bare instructions are named
 * `_raw`, so a translation unit that reaches for them by the old name
 * fails to compile instead of silently skipping this. `vmptrst` needs no
 * wrapper - it reports the current VMCS without changing it.
 * @{
 */
inline int vmptrld(void * region)
{
    vmcs_cache_forget();
    return vmptrld_raw(region);
}

/**
 * Points this processor's cache at the row describing `region`, without
 * discarding anything.
 *
 * **Takes the processor index rather than reading it from GS**, because
 * `vmptrld` runs on the launch path before `setup_vmcs` has armed the GS
 * base, and reading GS there is how the field cache killed a boot with
 * loader code `0x60e00` the first time it was added. Every caller on the
 * nested path already holds `cpu`; the ones that do not use the plain
 * wrapper above and pay a full flush, which is correct and rare.
 */
inline void vmcs_cache_select(std::uint64_t region, std::size_t cpu)
{
    if constexpr (vmcs_cache_enabled) {
        if (cpu >= vmcs_cache_processors) {
            vmcs_cache_forget();
            return;
        }

        auto * rows = vmcs_cache[cpu];

        for (std::size_t i{}; i < vmcs_cache_sets; ++i) {
            if ((rows[i].vmcs == region) &&
                (rows[i].epoch == vmcs_cache_epoch)) {
                vmcs_cache_active[cpu] = i;
                return;
            }
        }

        // Round robin rather than least-recently-used: with one row per
        // VMCS in the round trip there is nothing to choose between, and
        // a policy that cannot thrash is worth more than one that picks
        // well.
        auto victim = static_cast<std::size_t>(
            (vmcs_cache_active[cpu] + 1) % vmcs_cache_sets);

        for (auto & each : rows[victim].tag) {
            each = 0;
        }

        rows[victim].vmcs = region;
        rows[victim].epoch = vmcs_cache_epoch;
        vmcs_cache_active[cpu] = victim;
    }
}

/**
 * Makes an *enlightened* VMCS current for this processor.
 *
 * The counterpart of `vmcs_cache_select` for a second-level VMCS that
 * lives in a page shared with the layer below. No `vmptrld` follows,
 * because there is nothing to load: the layer below is told which page to
 * use through the assist page, and every access here becomes a load or a
 * store. See `nested_vmx::evmcs_to_kvm`.
 *
 * Takes `cpu` for the same reason `vmptrld` does - GS is not armed
 * everywhere this can be reached from.
 */
inline void vmcs_cache_select_enlightened(std::uint64_t page,
                                          std::size_t cpu)
{
    if constexpr (vmcs_cache_enabled) {
        if (cpu >= vmcs_cache_processors) {
            return;
        }

        auto * rows = vmcs_cache[cpu];

        for (std::size_t i{}; i < vmcs_cache_sets; ++i) {
            if ((rows[i].evmcs == page) &&
                (rows[i].epoch == vmcs_cache_epoch)) {
                vmcs_cache_active[cpu] = i;
                return;
            }
        }

        auto victim = static_cast<std::size_t>(
            (vmcs_cache_active[cpu] + 1) % vmcs_cache_sets);

        for (auto & each : rows[victim].tag) {
            each = 0;
        }

        rows[victim].vmcs = 0;
        rows[victim].evmcs = page;
        rows[victim].epoch = vmcs_cache_epoch;
        vmcs_cache_active[cpu] = victim;
    }
}

/**
 * The enlightened VMCS this processor is currently pointed at, or zero.
 */
inline std::uint64_t vmcs_cache_current_enlightened()
{
    if constexpr (vmcs_cache_enabled) {
        auto row = vmcs_cache_row_index();

        if (row < vmcs_cache_processors) {
            auto & current = vmcs_cache[row][vmcs_cache_active[row]];

            if (current.epoch == vmcs_cache_epoch) {
                return current.evmcs;
            }
        }
    }

    return 0;
}

/**
 * Reads a field out of an enlightened VMCS.
 *
 * Traps on a field the enlightened layout has no home for, rather than
 * falling back to a VMREAD. The fallback would read whatever VMCS happens
 * to be current - this VMM's own - and answer with a plausible value from
 * the wrong one, which is the failure this whole tree keeps having to
 * unpick. There is no correct answer, so there is no answer.
 */
inline std::uint64_t evmcs_load(std::uint64_t page, std::uint64_t encoding)
{
    auto slot = evmcs_offset_of(encoding);

    if (0 == slot.size) {
        __builtin_trap();
    }

    auto * at = reinterpret_cast<const volatile std::uint8_t *>(
        page + slot.offset);

    std::uint64_t value{};

    for (std::uint16_t i{}; i < slot.size; ++i) {
        value |= static_cast<std::uint64_t>(at[i]) << (8 * i);
    }

    return value;
}

/**
 * Writes a field into an enlightened VMCS, and marks the page dirty.
 */
inline void evmcs_store(std::uint64_t page,
                        std::uint64_t encoding,
                        std::uint64_t value)
{
    auto slot = evmcs_offset_of(encoding);

    if (0 == slot.size) {
        // **Writing zero to a field this format does not have is
        // disabling a feature it never had, so it is dropped.** The
        // fields with no enlightened home are all optional capabilities -
        // VMFUNC and its EPTP list, posted interrupts, the
        // page-modification log, sub-page permissions - and this VMM
        // writes zero to them on the second-level path to turn them off.
        // There is nothing to turn off here.
        //
        // Measured, and it is why the first attempt at this never reached
        // a single second-level entry: `write_vmcs02_control(cpu,
        // field::vm_function_controls, 0)` is unconditional, so the very
        // first entry trapped, and `l2-entries` read zero - which looks
        // exactly like a guest that never started.
        //
        // A *non-zero* write still traps, and must: that is a request to
        // switch on something this format cannot express, and carrying on
        // would run the guest with the feature silently absent.
        if (0 != value) {
            __builtin_trap();
        }

        return;
    }

    auto * at =
        reinterpret_cast<volatile std::uint8_t *>(page + slot.offset);

    for (std::uint16_t i{}; i < slot.size; ++i) {
        at[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // Every field dirty, every time. See `evmcs_all_dirty` for why the
    // clean-fields bitmap is not used yet: a wrong bit there means the
    // layer below keeps a stale field and runs the guest with it, which
    // is silent, and this is the correctness-first version.
    *reinterpret_cast<volatile std::uint32_t *>(
        page + evmcs_clean_fields_offset) = evmcs_all_dirty;
}

/**
 * `vmptrld` that keeps the rows for the VMCSs it is not loading.
 */
inline int vmptrld(void * region, std::size_t cpu)
{
    vmcs_cache_select(*static_cast<const std::uint64_t *>(region), cpu);
    return vmptrld_raw(region);
}

/**
 * Discards only the VMCS that just ran, rather than every row.
 *
 * A VM exit updates the guest-state and read-only fields of the VMCS that
 * was current and of no other, so the rows describing the VMCSs that were
 * merely resident stay true. That distinction is the whole reason the
 * per-exit flush is no longer global.
 */
inline void vmcs_cache_forget_current(std::size_t cpu)
{
    if constexpr (vmcs_cache_enabled) {
        if (cpu >= vmcs_cache_processors) {
            vmcs_cache_forget();
            return;
        }

        auto & row = vmcs_cache[cpu][vmcs_cache_active[cpu]];

        if (row.epoch == vmcs_cache_epoch) {
            for (auto & each : row.tag) {
                each = 0;
            }
        }
    }
}

inline int vmclear(void * region)
{
    vmcs_cache_forget();
    return vmclear_raw(region);
}
/**
 * @}
 * @}
 */

/**
 * The VMCS error type.
 */
enum class vmcs_error : int
{
    success,
    fail,
};

/**
 * Returns the VMCS error category.
 */
inline const zpp::error_category & category(vmcs_error)
{
    constexpr static auto error_category = zpp::make_error_category(
        "vmcs", vmcs_error::success, [](auto code) -> std::string_view {
            switch (code) {
            case vmcs_error::success:
                return zpp::error::no_error;
            case vmcs_error::fail:
                return "Fail.";
            default:
                return "Unknown error.";
            }
        });
    return error_category;
}

/**
 * Utility to access currently assigned CPU VMCS.
 */
class vmcs
{
public:
    /**
     * The VMCS error type.
     */
    using error = vmcs_error;

    /**
     * The VMCS field type.
     */
    using field = vmcs_fields::vmcs_field;

    /**
     * Construct the VMCS access utility.
     */
    vmcs() = default;

    /**
     * Write a value to a VMCS field, reporting failure.
     *
     * Only worth calling for a field whose existence depends on a CPU
     * capability, which is the one case where failure is information
     * rather than a bug. Everything else should use write.
     */
    std::expected<void, zpp::error> try_write(field field,
                                              std::uint64_t value) const
    {
        if (0 != vmwrite(field, value)) {
            return std::unexpected(zpp::error{error::fail});
        }

        return {};
    }

    /**
     * Read a specific VMCS field, reporting failure. See try_write.
     */
    std::expected<std::uint64_t, zpp::error> try_read(field field) const
    {
        std::uint64_t value{};
        if (0 != vmread(field, &value)) {
            return std::unexpected(zpp::error{error::fail});
        }
        return value;
    }

    /**
     * Write a value to a VMCS field. Traps if the write fails.
     *
     * There is no such thing as a recoverable vmwrite failure. The
     * instruction fails in exactly three ways - there is no current VMCS,
     * the field encoding does not exist on this processor, or the field is
     * read only - and every one of them is a bug in this code rather than
     * a condition a caller could do anything about. A *wrong value* does
     * not fail here at all; it surfaces later as a VM entry failure.
     *
     * So failure traps, in the same spirit as operator new in the CRT:
     * with -fno-exceptions there is nothing to throw, and a returned error
     * that nobody looks at is worse than a stop. That also keeps the
     * hundred-odd accessors below free of an error type that callers would
     * have had to either check or deliberately discard, which is how these
     * failures came to be ignored in the first place.
     */
    void write(field field, std::uint64_t value) const
    {
        vmcs_writes_taken = vmcs_writes_taken + 1;
        vmcs_record_use(static_cast<std::uint64_t>(field),
                        vmcs_write_field,
                        vmcs_write_hits,
                        vmcs_write_overflow);

        // **An enlightened VMCS is memory, so this is a store**, and it
        // must happen instead of the `vmwrite` rather than beside it: the
        // instruction would write whichever VMCS is current, which is not
        // this one. See `nested_vmx::evmcs_to_kvm`.
        if constexpr (vmcs_cache_enabled) {
            if (auto page = vmcs_cache_current_enlightened(); 0 != page) {
                evmcs_store(page, static_cast<std::uint64_t>(field), value);
                return;
            }
        }

        if (0 != vmwrite(field, value)) {
            __builtin_trap();
        }

        // **Written through, honouring the field's width.** This used
        // to drop the entry instead, on the grounds that a 32-bit field
        // stores only the low half of what is handed to `vmwrite`, so
        // caching the value as written would answer a later read with
        // bits the processor discarded - true, and the conclusion drawn
        // from it was wrong. Re-deriving the width is two shifts, not a
        // lookup: SDM 27.11.2 and Table 27-22
        // (`.references/sdm.txt:200509`) put the width in bits 14:13 of
        // the encoding and the access type in bit 0.
        //
        // Dropping cost far more than the miss it was described as. A
        // write is the *most* likely thing to be followed by a read of
        // the same field, and there are 17 writes to 26 reads on an exit,
        // so every write was arming a guaranteed miss on a path where a
        // miss is an exit to the layer below at about 4,900 cycles.
        if constexpr (vmcs_cache_enabled) {
            auto row = (0 == vmcs_cache_suspended)
                           ? vmcs_cache_row_index()
                           : vmcs_cache_processors;

            if (row < vmcs_cache_processors) {
                auto & current =
                    vmcs_cache[row][vmcs_cache_active[row]];

                if (current.epoch == vmcs_cache_epoch) {
                    auto encoding = static_cast<std::uint64_t>(field);
                    auto slot = static_cast<std::size_t>(
                        (encoding >> 1) % vmcs_cache_entries);

                    // Width 0 is 16-bit and width 2 is 32-bit; widths 1
                    // and 3 are 64-bit and natural, which this
                    // processor stores whole. A "high" access - bit 0 -
                    // names the upper half of a 64-bit field and is
                    // itself 32 bits wide.
                    constexpr std::uint64_t access_type_high = 1;
                    auto width = (encoding >> 13) & 3;

                    auto stored =
                        (0 != (encoding & access_type_high))
                            ? (value & 0xffffffffull)
                            : ((0 == width)   ? (value & 0xffffull)
                               : (2 == width) ? (value & 0xffffffffull)
                                              : value);

                    current.tag[slot] =
                        static_cast<std::uint64_t>(field) + 1;
                    current.value[slot] = stored;
                }
            }
        }
    }

    /**
     * Read a specific VMCS field. Traps if the read fails, for the same
     * reasons as write.
     */
    std::uint64_t read(field field) const
    {
        vmcs_reads_taken = vmcs_reads_taken + 1;
        vmcs_record_use(static_cast<std::uint64_t>(field),
                        vmcs_read_field,
                        vmcs_read_hits,
                        vmcs_read_overflow);
        vmcs_note_read_caller(
            reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));

        // **An enlightened VMCS is memory, so this is a load.** Ahead of
        // the cache, which exists to avoid an access that is not being
        // made here. See `nested_vmx::evmcs_to_kvm`.
        if constexpr (vmcs_cache_enabled) {
            if (auto page = vmcs_cache_current_enlightened(); 0 != page) {
                return evmcs_load(page,
                                  static_cast<std::uint64_t>(field));
            }
        }

        if constexpr (vmcs_cache_enabled) {
            auto row = (0 == vmcs_cache_suspended)
                           ? vmcs_cache_row_index()
                           : vmcs_cache_processors;

            if (row < vmcs_cache_processors) {
                auto slot = static_cast<std::size_t>(
                    (static_cast<std::uint64_t>(field) >> 1) %
                    vmcs_cache_entries);
                auto tag = static_cast<std::uint64_t>(field) + 1;

                auto & current =
                    vmcs_cache[row][vmcs_cache_active[row]];

                // The epoch is checked before the tag, because a row left
                // over from an earlier window may hold a matching tag.
                //
                // **Every** row of this processor is reset, not just the
                // current one, and that is required rather than tidy: a
                // global epoch move means some path loaded a VMCS without
                // going through `vmcs_cache_select`, so an identity left
                // in another row would match a later `vmptrld` of the
                // same address and answer with a different VMCS's values.
                // Dropping the identities is what stops that.
                if (current.epoch != vmcs_cache_epoch) {
                    for (auto & each : vmcs_cache[row]) {
                        for (auto & one : each.tag) {
                            one = 0;
                        }
                        each.vmcs = 0;
                        each.epoch = vmcs_cache_epoch;
                    }
                } else if (current.tag[slot] == tag) {
                    vmcs_cache_hits = vmcs_cache_hits + 1;
                    return current.value[slot];
                }

                vmcs_cache_misses = vmcs_cache_misses + 1;

                std::uint64_t fresh{};
                if (0 != vmread(field, &fresh)) {
                    __builtin_trap();
                }

                current.tag[slot] = tag;
                current.value[slot] = fresh;
                return fresh;
            }
        }

        std::uint64_t value{};
        if (0 != vmread(field, &value)) {
            __builtin_trap();
        }
        return value;
    }

    /**
     * Functions to read and write specific VMCS fields.
     * @{
     */

    std::uint64_t vpid() const
    {
        return read(field::vpid);
    }

    void vpid(std::uint64_t value) const
    {
        return write(field::vpid, value);
    }

    std::uint64_t posted_interrupt_notification_vector() const
    {
        return read(field::posted_interrupt_notification_vector);
    }

    void posted_interrupt_notification_vector(std::uint64_t value) const
    {
        return write(field::posted_interrupt_notification_vector, value);
    }

    std::uint64_t eptp_index() const
    {
        return read(field::eptp_index);
    }

    void eptp_index(std::uint64_t value) const
    {
        return write(field::eptp_index, value);
    }

    std::uint64_t guest_es_selector() const
    {
        return read(field::guest_es_selector);
    }

    void guest_es_selector(std::uint64_t value) const
    {
        return write(field::guest_es_selector, value);
    }

    std::uint64_t guest_cs_selector() const
    {
        return read(field::guest_cs_selector);
    }

    void guest_cs_selector(std::uint64_t value) const
    {
        return write(field::guest_cs_selector, value);
    }

    std::uint64_t guest_ss_selector() const
    {
        return read(field::guest_ss_selector);
    }

    void guest_ss_selector(std::uint64_t value) const
    {
        return write(field::guest_ss_selector, value);
    }

    std::uint64_t guest_ds_selector() const
    {
        return read(field::guest_ds_selector);
    }

    void guest_ds_selector(std::uint64_t value) const
    {
        return write(field::guest_ds_selector, value);
    }

    std::uint64_t guest_fs_selector() const
    {
        return read(field::guest_fs_selector);
    }

    void guest_fs_selector(std::uint64_t value) const
    {
        return write(field::guest_fs_selector, value);
    }

    std::uint64_t guest_gs_selector() const
    {
        return read(field::guest_gs_selector);
    }

    void guest_gs_selector(std::uint64_t value) const
    {
        return write(field::guest_gs_selector, value);
    }

    std::uint64_t guest_ldtr_selector() const
    {
        return read(field::guest_ldtr_selector);
    }

    void guest_ldtr_selector(std::uint64_t value) const
    {
        return write(field::guest_ldtr_selector, value);
    }

    std::uint64_t guest_tr_selector() const
    {
        return read(field::guest_tr_selector);
    }

    void guest_tr_selector(std::uint64_t value) const
    {
        return write(field::guest_tr_selector, value);
    }

    std::uint64_t guest_interrupt_status() const
    {
        return read(field::guest_interrupt_status);
    }

    void guest_interrupt_status(std::uint64_t value) const
    {
        return write(field::guest_interrupt_status, value);
    }

    std::uint64_t pml_index() const
    {
        return read(field::pml_index);
    }

    void pml_index(std::uint64_t value) const
    {
        return write(field::pml_index, value);
    }

    std::uint64_t host_es_selector() const
    {
        return read(field::host_es_selector);
    }

    void host_es_selector(std::uint64_t value) const
    {
        return write(field::host_es_selector, value);
    }

    std::uint64_t host_cs_selector() const
    {
        return read(field::host_cs_selector);
    }

    void host_cs_selector(std::uint64_t value) const
    {
        return write(field::host_cs_selector, value);
    }

    std::uint64_t host_ss_selector() const
    {
        return read(field::host_ss_selector);
    }

    void host_ss_selector(std::uint64_t value) const
    {
        return write(field::host_ss_selector, value);
    }

    std::uint64_t host_ds_selector() const
    {
        return read(field::host_ds_selector);
    }

    void host_ds_selector(std::uint64_t value) const
    {
        return write(field::host_ds_selector, value);
    }

    std::uint64_t host_fs_selector() const
    {
        return read(field::host_fs_selector);
    }

    void host_fs_selector(std::uint64_t value) const
    {
        return write(field::host_fs_selector, value);
    }

    std::uint64_t host_gs_selector() const
    {
        return read(field::host_gs_selector);
    }

    void host_gs_selector(std::uint64_t value) const
    {
        return write(field::host_gs_selector, value);
    }

    std::uint64_t host_tr_selector() const
    {
        return read(field::host_tr_selector);
    }

    void host_tr_selector(std::uint64_t value) const
    {
        return write(field::host_tr_selector, value);
    }

    std::uint64_t io_bitmap_a() const
    {
        return read(field::io_bitmap_a);
    }

    void io_bitmap_a(std::uint64_t value) const
    {
        return write(field::io_bitmap_a, value);
    }

    std::uint64_t io_bitmap_b() const
    {
        return read(field::io_bitmap_b);
    }

    void io_bitmap_b(std::uint64_t value) const
    {
        return write(field::io_bitmap_b, value);
    }

    std::uint64_t msr_bitmap() const
    {
        return read(field::msr_bitmap);
    }

    void msr_bitmap(std::uint64_t value) const
    {
        return write(field::msr_bitmap, value);
    }

    std::uint64_t vm_exit_msr_store_address() const
    {
        return read(field::vm_exit_msr_store_address);
    }

    void vm_exit_msr_store_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_address, value);
    }

    std::uint64_t vm_exit_msr_load_address() const
    {
        return read(field::vm_exit_msr_load_address);
    }

    void vm_exit_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_address, value);
    }

    std::uint64_t vm_entry_msr_load_address() const
    {
        return read(field::vm_entry_msr_load_address);
    }

    void vm_entry_msr_load_address(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_address, value);
    }

    std::uint64_t excecutive_vmcs_pointer() const
    {
        return read(field::excecutive_vmcs_pointer);
    }

    void excecutive_vmcs_pointer(std::uint64_t value) const
    {
        return write(field::excecutive_vmcs_pointer, value);
    }

    std::uint64_t pml_address() const
    {
        return read(field::pml_address);
    }

    void pml_address(std::uint64_t value) const
    {
        return write(field::pml_address, value);
    }

    std::uint64_t tsc_offset() const
    {
        return read(field::tsc_offset);
    }

    void tsc_offset(std::uint64_t value) const
    {
        return write(field::tsc_offset, value);
    }

    std::uint64_t virtual_apic_address() const
    {
        return read(field::virtual_apic_address);
    }

    void virtual_apic_address(std::uint64_t value) const
    {
        return write(field::virtual_apic_address, value);
    }

    std::uint64_t apic_access_address() const
    {
        return read(field::apic_access_address);
    }

    void apic_access_address(std::uint64_t value) const
    {
        return write(field::apic_access_address, value);
    }

    std::uint64_t posted_interrupt_descriptor_address() const
    {
        return read(field::posted_interrupt_descriptor_address);
    }

    void posted_interrupt_descriptor_address(std::uint64_t value) const
    {
        return write(field::posted_interrupt_descriptor_address, value);
    }

    std::uint64_t vm_function_controls() const
    {
        return read(field::vm_function_controls);
    }

    void vm_function_controls(std::uint64_t value) const
    {
        return write(field::vm_function_controls, value);
    }

    std::uint64_t ept_pointer() const
    {
        return read(field::ept_pointer);
    }

    void ept_pointer(std::uint64_t value) const
    {
        return write(field::ept_pointer, value);
    }

    std::uint64_t eio_exit_bitmap_0() const
    {
        return read(field::eio_exit_bitmap_0);
    }

    void eio_exit_bitmap_0(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_0, value);
    }

    std::uint64_t eio_exit_bitmap_1() const
    {
        return read(field::eio_exit_bitmap_1);
    }

    void eio_exit_bitmap_1(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_1, value);
    }

    std::uint64_t eio_exit_bitmap_2() const
    {
        return read(field::eio_exit_bitmap_2);
    }

    void eio_exit_bitmap_2(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_2, value);
    }

    std::uint64_t eio_exit_bitmap_3() const
    {
        return read(field::eio_exit_bitmap_3);
    }

    void eio_exit_bitmap_3(std::uint64_t value) const
    {
        return write(field::eio_exit_bitmap_3, value);
    }

    std::uint64_t eptp_list_address() const
    {
        return read(field::eptp_list_address);
    }

    void eptp_list_address(std::uint64_t value) const
    {
        return write(field::eptp_list_address, value);
    }

    std::uint64_t vmread_bitmap_address() const
    {
        return read(field::vmread_bitmap_address);
    }

    void vmread_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmread_bitmap_address, value);
    }

    std::uint64_t vmwrite_bitmap_address() const
    {
        return read(field::vmwrite_bitmap_address);
    }

    void vmwrite_bitmap_address(std::uint64_t value) const
    {
        return write(field::vmwrite_bitmap_address, value);
    }

    std::uint64_t virtualization_exception_information_address() const
    {
        return read(field::virtualization_exception_information_address);
    }

    void
    virtualization_exception_information_address(std::uint64_t value) const
    {
        return write(field::virtualization_exception_information_address,
                     value);
    }

    std::uint64_t xss_exiting_bitmap() const
    {
        return read(field::xss_exiting_bitmap);
    }

    void xss_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::xss_exiting_bitmap, value);
    }

    std::uint64_t encls_exiting_bitmap() const
    {
        return read(field::encls_exiting_bitmap);
    }

    void encls_exiting_bitmap(std::uint64_t value) const
    {
        return write(field::encls_exiting_bitmap, value);
    }

    std::uint64_t sub_page_permission_table_pointer() const
    {
        return read(field::sub_page_permission_table_pointer);
    }

    void sub_page_permission_table_pointer(std::uint64_t value) const
    {
        return write(field::sub_page_permission_table_pointer, value);
    }

    std::uint64_t tsc_multiplier() const
    {
        return read(field::tsc_multiplier);
    }

    void tsc_multiplier(std::uint64_t value) const
    {
        return write(field::tsc_multiplier, value);
    }

    std::uint64_t guest_physical_address() const
    {
        return read(field::guest_physical_address);
    }

    void guest_physical_address(std::uint64_t value) const
    {
        return write(field::guest_physical_address, value);
    }

    std::uint64_t vmcs_link_pointer() const
    {
        return read(field::vmcs_link_pointer);
    }

    void vmcs_link_pointer(std::uint64_t value) const
    {
        return write(field::vmcs_link_pointer, value);
    }

    std::uint64_t guest_ia32_debugctl() const
    {
        return read(field::guest_ia32_debugctl);
    }

    void guest_ia32_debugctl(std::uint64_t value) const
    {
        return write(field::guest_ia32_debugctl, value);
    }

    std::uint64_t guest_ia32_pat() const
    {
        return read(field::guest_ia32_pat);
    }

    void guest_ia32_pat(std::uint64_t value) const
    {
        return write(field::guest_ia32_pat, value);
    }

    std::uint64_t guest_ia32_efer() const
    {
        return read(field::guest_ia32_efer);
    }

    void guest_ia32_efer(std::uint64_t value) const
    {
        return write(field::guest_ia32_efer, value);
    }

    std::uint64_t guest_ia32_perf_global_ctrl() const
    {
        return read(field::guest_ia32_perf_global_ctrl);
    }

    void guest_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::guest_ia32_perf_global_ctrl, value);
    }

    std::uint64_t guest_pdpte_0() const
    {
        return read(field::guest_pdpte_0);
    }

    void guest_pdpte_0(std::uint64_t value) const
    {
        return write(field::guest_pdpte_0, value);
    }

    std::uint64_t guest_pdpte_1() const
    {
        return read(field::guest_pdpte_1);
    }

    void guest_pdpte_1(std::uint64_t value) const
    {
        return write(field::guest_pdpte_1, value);
    }

    std::uint64_t guest_pdpte_2() const
    {
        return read(field::guest_pdpte_2);
    }

    void guest_pdpte_2(std::uint64_t value) const
    {
        return write(field::guest_pdpte_2, value);
    }

    std::uint64_t guest_pdpte_3() const
    {
        return read(field::guest_pdpte_3);
    }

    void guest_pdpte_3(std::uint64_t value) const
    {
        return write(field::guest_pdpte_3, value);
    }

    std::uint64_t guest_ia32_bndcfgs() const
    {
        return read(field::guest_ia32_bndcfgs);
    }

    void guest_ia32_bndcfgs(std::uint64_t value) const
    {
        return write(field::guest_ia32_bndcfgs, value);
    }

    std::uint64_t host_ia32_pat() const
    {
        return read(field::host_ia32_pat);
    }

    void host_ia32_pat(std::uint64_t value) const
    {
        return write(field::host_ia32_pat, value);
    }

    std::uint64_t host_ia32_efer() const
    {
        return read(field::host_ia32_efer);
    }

    void host_ia32_efer(std::uint64_t value) const
    {
        return write(field::host_ia32_efer, value);
    }

    std::uint64_t host_ia32_perf_global_ctrl() const
    {
        return read(field::host_ia32_perf_global_ctrl);
    }

    void host_ia32_perf_global_ctrl(std::uint64_t value) const
    {
        return write(field::host_ia32_perf_global_ctrl, value);
    }

    std::uint64_t pin_based_vm_execution_controls() const
    {
        return read(field::pin_based_vm_execution_controls);
    }

    void pin_based_vm_execution_controls(std::uint64_t value) const
    {
        return write(field::pin_based_vm_execution_controls, value);
    }

    std::uint64_t primary_processor_based_vm_execution_controls() const
    {
        return read(field::primary_processor_based_vm_execution_controls);
    }

    void primary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(field::primary_processor_based_vm_execution_controls,
                     value);
    }

    std::uint64_t exception_bitmap() const
    {
        return read(field::exception_bitmap);
    }

    void exception_bitmap(std::uint64_t value) const
    {
        return write(field::exception_bitmap, value);
    }

    std::uint64_t page_fault_error_code_mask() const
    {
        return read(field::page_fault_error_code_mask);
    }

    void page_fault_error_code_mask(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_mask, value);
    }

    std::uint64_t page_fault_error_code_match() const
    {
        return read(field::page_fault_error_code_match);
    }

    void page_fault_error_code_match(std::uint64_t value) const
    {
        return write(field::page_fault_error_code_match, value);
    }

    std::uint64_t cr3_target_count() const
    {
        return read(field::cr3_target_count);
    }

    void cr3_target_count(std::uint64_t value) const
    {
        return write(field::cr3_target_count, value);
    }

    std::uint64_t vm_exit_controls() const
    {
        return read(field::vm_exit_controls);
    }

    void vm_exit_controls(std::uint64_t value) const
    {
        return write(field::vm_exit_controls, value);
    }

    std::uint64_t vm_exit_msr_store_count() const
    {
        return read(field::vm_exit_msr_store_count);
    }

    void vm_exit_msr_store_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_store_count, value);
    }

    std::uint64_t vm_exit_msr_load_count() const
    {
        return read(field::vm_exit_msr_load_count);
    }

    void vm_exit_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_exit_msr_load_count, value);
    }

    std::uint64_t vm_entry_controls() const
    {
        return read(field::vm_entry_controls);
    }

    void vm_entry_controls(std::uint64_t value) const
    {
        return write(field::vm_entry_controls, value);
    }

    std::uint64_t vm_entry_msr_load_count() const
    {
        return read(field::vm_entry_msr_load_count);
    }

    void vm_entry_msr_load_count(std::uint64_t value) const
    {
        return write(field::vm_entry_msr_load_count, value);
    }

    std::uint64_t vm_entry_interruption_information_field() const
    {
        return read(field::vm_entry_interruption_information_field);
    }

    void vm_entry_interruption_information_field(std::uint64_t value) const
    {
        return write(field::vm_entry_interruption_information_field,
                     value);
    }

    std::uint64_t vm_entry_exception_error_code() const
    {
        return read(field::vm_entry_exception_error_code);
    }

    void vm_entry_exception_error_code(std::uint64_t value) const
    {
        return write(field::vm_entry_exception_error_code, value);
    }

    std::uint64_t vm_entry_instruction_length() const
    {
        return read(field::vm_entry_instruction_length);
    }

    void vm_entry_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_entry_instruction_length, value);
    }

    std::uint64_t tpr_threshold() const
    {
        return read(field::tpr_threshold);
    }

    void tpr_threshold(std::uint64_t value) const
    {
        return write(field::tpr_threshold, value);
    }

    std::uint64_t secondary_processor_based_vm_execution_controls() const
    {
        return read(
            field::secondary_processor_based_vm_execution_controls);
    }

    void secondary_processor_based_vm_execution_controls(
        std::uint64_t value) const
    {
        return write(
            field::secondary_processor_based_vm_execution_controls, value);
    }

    std::uint64_t ple_gap() const
    {
        return read(field::ple_gap);
    }

    void ple_gap(std::uint64_t value) const
    {
        return write(field::ple_gap, value);
    }

    std::uint64_t ple_window() const
    {
        return read(field::ple_window);
    }

    void ple_window(std::uint64_t value) const
    {
        return write(field::ple_window, value);
    }

    std::uint64_t vm_instruction_error() const
    {
        return read(field::vm_instruction_error);
    }

    void vm_instruction_error(std::uint64_t value) const
    {
        return write(field::vm_instruction_error, value);
    }

    std::uint64_t exit_reason() const
    {
        return read(field::exit_reason);
    }

    void exit_reason(std::uint64_t value) const
    {
        return write(field::exit_reason, value);
    }

    std::uint64_t vm_exit_interruption_information() const
    {
        return read(field::vm_exit_interruption_information);
    }

    void vm_exit_interruption_information(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_information, value);
    }

    std::uint64_t vm_exit_interruption_error_code() const
    {
        return read(field::vm_exit_interruption_error_code);
    }

    void vm_exit_interruption_error_code(std::uint64_t value) const
    {
        return write(field::vm_exit_interruption_error_code, value);
    }

    std::uint64_t idt_vectoring_information_field() const
    {
        return read(field::idt_vectoring_information_field);
    }

    void idt_vectoring_information_field(std::uint64_t value) const
    {
        return write(field::idt_vectoring_information_field, value);
    }

    std::uint64_t idt_vectoring_error_code() const
    {
        return read(field::idt_vectoring_error_code);
    }

    void idt_vectoring_error_code(std::uint64_t value) const
    {
        return write(field::idt_vectoring_error_code, value);
    }

    std::uint64_t vm_exit_instruction_length() const
    {
        return read(field::vm_exit_instruction_length);
    }

    void vm_exit_instruction_length(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_length, value);
    }

    std::uint64_t vm_exit_instruction_information() const
    {
        return read(field::vm_exit_instruction_information);
    }

    void vm_exit_instruction_information(std::uint64_t value) const
    {
        return write(field::vm_exit_instruction_information, value);
    }

    std::uint64_t guest_es_limit() const
    {
        return read(field::guest_es_limit);
    }

    void guest_es_limit(std::uint64_t value) const
    {
        return write(field::guest_es_limit, value);
    }

    std::uint64_t guest_cs_limit() const
    {
        return read(field::guest_cs_limit);
    }

    void guest_cs_limit(std::uint64_t value) const
    {
        return write(field::guest_cs_limit, value);
    }

    std::uint64_t guest_ss_limit() const
    {
        return read(field::guest_ss_limit);
    }

    void guest_ss_limit(std::uint64_t value) const
    {
        return write(field::guest_ss_limit, value);
    }

    std::uint64_t guest_ds_limit() const
    {
        return read(field::guest_ds_limit);
    }

    void guest_ds_limit(std::uint64_t value) const
    {
        return write(field::guest_ds_limit, value);
    }

    std::uint64_t guest_fs_limit() const
    {
        return read(field::guest_fs_limit);
    }

    void guest_fs_limit(std::uint64_t value) const
    {
        return write(field::guest_fs_limit, value);
    }

    std::uint64_t guest_gs_limit() const
    {
        return read(field::guest_gs_limit);
    }

    void guest_gs_limit(std::uint64_t value) const
    {
        return write(field::guest_gs_limit, value);
    }

    std::uint64_t guest_ldtr_limit() const
    {
        return read(field::guest_ldtr_limit);
    }

    void guest_ldtr_limit(std::uint64_t value) const
    {
        return write(field::guest_ldtr_limit, value);
    }

    std::uint64_t guest_tr_limit() const
    {
        return read(field::guest_tr_limit);
    }

    void guest_tr_limit(std::uint64_t value) const
    {
        return write(field::guest_tr_limit, value);
    }

    std::uint64_t guest_gdtr_limit() const
    {
        return read(field::guest_gdtr_limit);
    }

    void guest_gdtr_limit(std::uint64_t value) const
    {
        return write(field::guest_gdtr_limit, value);
    }

    std::uint64_t guest_idtr_limit() const
    {
        return read(field::guest_idtr_limit);
    }

    void guest_idtr_limit(std::uint64_t value) const
    {
        return write(field::guest_idtr_limit, value);
    }

    std::uint64_t guest_es_access_rights() const
    {
        return read(field::guest_es_access_rights);
    }

    void guest_es_access_rights(std::uint64_t value) const
    {
        return write(field::guest_es_access_rights, value);
    }

    std::uint64_t guest_cs_access_rights() const
    {
        return read(field::guest_cs_access_rights);
    }

    void guest_cs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_cs_access_rights, value);
    }

    std::uint64_t guest_ss_access_rights() const
    {
        return read(field::guest_ss_access_rights);
    }

    void guest_ss_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ss_access_rights, value);
    }

    std::uint64_t guest_ds_access_rights() const
    {
        return read(field::guest_ds_access_rights);
    }

    void guest_ds_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ds_access_rights, value);
    }

    std::uint64_t guest_fs_access_rights() const
    {
        return read(field::guest_fs_access_rights);
    }

    void guest_fs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_fs_access_rights, value);
    }

    std::uint64_t guest_gs_access_rights() const
    {
        return read(field::guest_gs_access_rights);
    }

    void guest_gs_access_rights(std::uint64_t value) const
    {
        return write(field::guest_gs_access_rights, value);
    }

    std::uint64_t guest_ldtr_access_rights() const
    {
        return read(field::guest_ldtr_access_rights);
    }

    void guest_ldtr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_ldtr_access_rights, value);
    }

    std::uint64_t guest_tr_access_rights() const
    {
        return read(field::guest_tr_access_rights);
    }

    void guest_tr_access_rights(std::uint64_t value) const
    {
        return write(field::guest_tr_access_rights, value);
    }

    std::uint64_t guest_interruptibility_state() const
    {
        return read(field::guest_interruptibility_state);
    }

    void guest_interruptibility_state(std::uint64_t value) const
    {
        return write(field::guest_interruptibility_state, value);
    }

    std::uint64_t guest_activity_state() const
    {
        return read(field::guest_activity_state);
    }

    void guest_activity_state(std::uint64_t value) const
    {
        return write(field::guest_activity_state, value);
    }

    std::uint64_t guest_smbase() const
    {
        return read(field::guest_smbase);
    }

    void guest_smbase(std::uint64_t value) const
    {
        return write(field::guest_smbase, value);
    }

    std::uint64_t guest_ia32_sysenter_cs() const
    {
        return read(field::guest_ia32_sysenter_cs);
    }

    void guest_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_cs, value);
    }

    std::uint64_t vmx_preemption_timer_value() const
    {
        return read(field::vmx_preemption_timer_value);
    }

    void vmx_preemption_timer_value(std::uint64_t value) const
    {
        return write(field::vmx_preemption_timer_value, value);
    }

    std::uint64_t host_ia32_sysenter_cs() const
    {
        return read(field::host_ia32_sysenter_cs);
    }

    void host_ia32_sysenter_cs(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_cs, value);
    }

    std::uint64_t cr0_guest_host_mask() const
    {
        return read(field::cr0_guest_host_mask);
    }

    void cr0_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr0_guest_host_mask, value);
    }

    std::uint64_t cr4_guest_host_mask() const
    {
        return read(field::cr4_guest_host_mask);
    }

    void cr4_guest_host_mask(std::uint64_t value) const
    {
        return write(field::cr4_guest_host_mask, value);
    }

    std::uint64_t cr0_read_shadow() const
    {
        return read(field::cr0_read_shadow);
    }

    void cr0_read_shadow(std::uint64_t value) const
    {
        return write(field::cr0_read_shadow, value);
    }

    std::uint64_t cr4_read_shadow() const
    {
        return read(field::cr4_read_shadow);
    }

    void cr4_read_shadow(std::uint64_t value) const
    {
        return write(field::cr4_read_shadow, value);
    }

    std::uint64_t cr3_target_value_0() const
    {
        return read(field::cr3_target_value_0);
    }

    void cr3_target_value_0(std::uint64_t value) const
    {
        return write(field::cr3_target_value_0, value);
    }

    std::uint64_t cr3_target_value_1() const
    {
        return read(field::cr3_target_value_1);
    }

    void cr3_target_value_1(std::uint64_t value) const
    {
        return write(field::cr3_target_value_1, value);
    }

    std::uint64_t cr3_target_value_2() const
    {
        return read(field::cr3_target_value_2);
    }

    void cr3_target_value_2(std::uint64_t value) const
    {
        return write(field::cr3_target_value_2, value);
    }

    std::uint64_t cr3_target_value_3() const
    {
        return read(field::cr3_target_value_3);
    }

    void cr3_target_value_3(std::uint64_t value) const
    {
        return write(field::cr3_target_value_3, value);
    }

    /**
     * The exit qualification. Read only, and its meaning depends
     * entirely on the exit reason - for a start-up IPI it carries the
     * vector in its low eight bits.
     */
    std::uint64_t exit_qualification() const
    {
        return read(field::exit_qualification);
    }

    std::uint64_t guest_linear_address() const
    {
        return read(field::guest_linear_address);
    }

    std::uint64_t guest_cr0() const
    {
        return read(field::guest_cr0);
    }

    void guest_cr0(std::uint64_t value) const
    {
        return write(field::guest_cr0, value);
    }

    std::uint64_t guest_cr3() const
    {
        return read(field::guest_cr3);
    }

    void guest_cr3(std::uint64_t value) const
    {
        return write(field::guest_cr3, value);
    }

    std::uint64_t guest_cr4() const
    {
        return read(field::guest_cr4);
    }

    void guest_cr4(std::uint64_t value) const
    {
        return write(field::guest_cr4, value);
    }

    std::uint64_t guest_es_base() const
    {
        return read(field::guest_es_base);
    }

    void guest_es_base(std::uint64_t value) const
    {
        return write(field::guest_es_base, value);
    }

    std::uint64_t guest_cs_base() const
    {
        return read(field::guest_cs_base);
    }

    void guest_cs_base(std::uint64_t value) const
    {
        return write(field::guest_cs_base, value);
    }

    std::uint64_t guest_ss_base() const
    {
        return read(field::guest_ss_base);
    }

    void guest_ss_base(std::uint64_t value) const
    {
        return write(field::guest_ss_base, value);
    }

    std::uint64_t guest_ds_base() const
    {
        return read(field::guest_ds_base);
    }

    void guest_ds_base(std::uint64_t value) const
    {
        return write(field::guest_ds_base, value);
    }

    std::uint64_t guest_fs_base() const
    {
        return read(field::guest_fs_base);
    }

    void guest_fs_base(std::uint64_t value) const
    {
        return write(field::guest_fs_base, value);
    }

    std::uint64_t guest_gs_base() const
    {
        return read(field::guest_gs_base);
    }

    void guest_gs_base(std::uint64_t value) const
    {
        return write(field::guest_gs_base, value);
    }

    std::uint64_t guest_ldtr_base() const
    {
        return read(field::guest_ldtr_base);
    }

    void guest_ldtr_base(std::uint64_t value) const
    {
        return write(field::guest_ldtr_base, value);
    }

    std::uint64_t guest_tr_base() const
    {
        return read(field::guest_tr_base);
    }

    void guest_tr_base(std::uint64_t value) const
    {
        return write(field::guest_tr_base, value);
    }

    std::uint64_t guest_gdtr_base() const
    {
        return read(field::guest_gdtr_base);
    }

    void guest_gdtr_base(std::uint64_t value) const
    {
        return write(field::guest_gdtr_base, value);
    }

    std::uint64_t guest_idtr_base() const
    {
        return read(field::guest_idtr_base);
    }

    void guest_idtr_base(std::uint64_t value) const
    {
        return write(field::guest_idtr_base, value);
    }

    std::uint64_t guest_dr7() const
    {
        return read(field::guest_dr7);
    }

    void guest_dr7(std::uint64_t value) const
    {
        return write(field::guest_dr7, value);
    }

    std::uint64_t guest_rsp() const
    {
        return read(field::guest_rsp);
    }

    void guest_rsp(std::uint64_t value) const
    {
        return write(field::guest_rsp, value);
    }

    std::uint64_t guest_rip() const
    {
        return read(field::guest_rip);
    }

    void guest_rip(std::uint64_t value) const
    {
        return write(field::guest_rip, value);
    }

    std::uint64_t guest_rflags() const
    {
        return read(field::guest_rflags);
    }

    void guest_rflags(std::uint64_t value) const
    {
        return write(field::guest_rflags, value);
    }

    std::uint64_t guest_pending_debug_exceptions() const
    {
        return read(field::guest_pending_debug_exceptions);
    }

    void guest_pending_debug_exceptions(std::uint64_t value) const
    {
        return write(field::guest_pending_debug_exceptions, value);
    }

    std::uint64_t guest_ia32_sysenter_esp() const
    {
        return read(field::guest_ia32_sysenter_esp);
    }

    void guest_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_esp, value);
    }

    std::uint64_t guest_ia32_sysenter_eip() const
    {
        return read(field::guest_ia32_sysenter_eip);
    }

    void guest_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::guest_ia32_sysenter_eip, value);
    }

    std::uint64_t host_cr0() const
    {
        return read(field::host_cr0);
    }

    void host_cr0(std::uint64_t value) const
    {
        return write(field::host_cr0, value);
    }

    std::uint64_t host_cr3() const
    {
        return read(field::host_cr3);
    }

    void host_cr3(std::uint64_t value) const
    {
        return write(field::host_cr3, value);
    }

    std::uint64_t host_cr4() const
    {
        return read(field::host_cr4);
    }

    void host_cr4(std::uint64_t value) const
    {
        return write(field::host_cr4, value);
    }

    std::uint64_t host_fs_base() const
    {
        return read(field::host_fs_base);
    }

    void host_fs_base(std::uint64_t value) const
    {
        return write(field::host_fs_base, value);
    }

    std::uint64_t host_gs_base() const
    {
        return read(field::host_gs_base);
    }

    void host_gs_base(std::uint64_t value) const
    {
        return write(field::host_gs_base, value);
    }

    std::uint64_t host_tr_base() const
    {
        return read(field::host_tr_base);
    }

    void host_tr_base(std::uint64_t value) const
    {
        return write(field::host_tr_base, value);
    }

    std::uint64_t host_gdtr_base() const
    {
        return read(field::host_gdtr_base);
    }

    void host_gdtr_base(std::uint64_t value) const
    {
        return write(field::host_gdtr_base, value);
    }

    std::uint64_t host_idtr_base() const
    {
        return read(field::host_idtr_base);
    }

    void host_idtr_base(std::uint64_t value) const
    {
        return write(field::host_idtr_base, value);
    }

    std::uint64_t host_ia32_sysenter_esp() const
    {
        return read(field::host_ia32_sysenter_esp);
    }

    void host_ia32_sysenter_esp(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_esp, value);
    }

    std::uint64_t host_ia32_sysenter_eip() const
    {
        return read(field::host_ia32_sysenter_eip);
    }

    void host_ia32_sysenter_eip(std::uint64_t value) const
    {
        return write(field::host_ia32_sysenter_eip, value);
    }

    std::uint64_t host_rsp() const
    {
        return read(field::host_rsp);
    }

    void host_rsp(std::uint64_t value) const
    {
        return write(field::host_rsp, value);
    }

    std::uint64_t host_rip() const
    {
        return read(field::host_rip);
    }

    void host_rip(std::uint64_t value) const
    {
        return write(field::host_rip, value);
    }

    /**
     * @}
     */
};

} // namespace zpp::arch::x86_64::vmx
