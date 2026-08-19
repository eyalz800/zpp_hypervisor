#pragma once
#include "zpp/arch/x86_64/ap_start_up.h"
#include "zpp/hypervisor/enlightened_vmcs.h"
#include "zpp/arch/x86_64/context.h"
#include "zpp/arch/x86_64/decoder.h"
#include "zpp/arch/x86_64/exception_entry.h"
#include "zpp/arch/x86_64/generic.h"
#include "zpp/arch/x86_64/instruction.h"
#include "zpp/arch/x86_64/msr.h"
#include "zpp/arch/x86_64/mtrr.h"
#include "zpp/arch/x86_64/os_page_table.h"
#include "zpp/arch/x86_64/page_table.h"
#include "zpp/arch/x86_64/vmx/ept.h"
#include "zpp/arch/x86_64/vmx/msr.h"
#include "zpp/arch/x86_64/vmx/nested_ept.h"
#include "zpp/arch/x86_64/vmx/vmcs.h"
#include "zpp/arch/x86_64/vmx/vmcs12.h"
#include "zpp/arch/x86_64/vmx/vmx.h"
#include "zpp/arch/x86_64/vmx/vmx_exit_reason.h"
#include "zpp/error.h"
#include "zpp/hypervisor/guest_windows.h"
#include "zpp/hypervisor/log.h"
#include "zpp/hypervisor/nested_vmx.h"
#include "zpp/hypervisor/start_up_handoff.h"
#include "zpp/nvme/admin_borrow.h"
#include "zpp/small_map.h"
#include "zpp/spin_lock.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace zpp::hypervisor
{
class hypervisor
{
public:
    /**
     * Records a VM entry that failed without producing a VM exit.
     *
     * Public because the failure path reaches it from a naked assembly
     * wrapper through an extern "C" shim, which is not a member.
     */
    void record_entry_failure(std::uint64_t flags);

    /**
     * Hypervisor errors.
     */
    enum class error
    {
        success = 0,
        vmxon_failed = 1,
        vmclear_failed = 2,
        vmptrld_failed = 3,
        physical_to_virtual_capacity_error = 4,
        out_of_ept_entries = 5,
        host_exception = 6,
        vmx_disabled_by_firmware = 7,
        too_many_processors = 8,

        /**
         * An extended page table entry was asked for before the tables
         * that hold it were built. A programming error rather than a
         * machine one - see the flag it comes from.
         */
        ept_not_initialized = 9,

        /**
         * The channel's controller is not known, so its register page
         * cannot be redirected.
         */
        controller_not_available = 10,

        /**
         * A processor did not acknowledge an extended page table change
         * in time. The change is made either way - the wait is what
         * establishes that nobody is still using the old translation, so
         * failing it means the caller must not proceed as though they
         * were not.
         */
        acknowledgement_timed_out = 11,

        /**
         * The controller did not reach the requested CSTS.RDY within the
         * spin budget the excursion allows it.
         */
        controller_never_settled = 12,

        /**
         * The controller refused one of the excursion's admin commands.
         */
        excursion_refused = 13,

        /**
         * VMXOFF failed with the carry flag, so this processor is still
         * in VMX operation when the code after it assumed otherwise.
         *
         * Distinct from vmxon_failed because the two mean opposite things
         * about where the processor now is, and the sleep quiesce has to
         * be able to tell them apart from a single error code printed by
         * a loader.
         */
        vmxoff_failed = 14,

        /**
         * The sleep quiesce could not work out which VMXON or VMCS region
         * belongs to this processor, because the VPID was outside the
         * range that names one.
         */
        no_region_for_processor = 15,

        /**
         * A guest linear address did not translate through the guest's own
         * page tables: some level was not present, or the paging mode is
         * not one this walker implements.
         *
         * A guest error rather than a machine one in the not-present case,
         * and the caller's job to turn into the fault the guest would have
         * taken.
         */
        guest_address_not_mapped = 16,

        /**
         * A guest physical access ran past what the mapping window can
         * reach in one go, or named a page the extended page tables do not
         * describe.
         */
        guest_memory_unreachable = 17,

        /**
         * The firmware ACPI control structure the loader handed over is
         * not one, or is too short to hold the fields a resume has to read
         * and write.
         *
         * Not a machine failure: the loader read that table before the
         * guest ever ran, and the guest has owned the memory since. A
         * suspend still happens, it just happens without this VMM
         * inserting itself into the resume.
         */
        no_usable_facs = 18,

        /**
         * There is nothing to resume into: the guest left no real mode
         * waking vector, or this VMM has no trampoline page to point the
         * platform at.
         */
        no_waking_vector = 19,

        /**
         * A shadow extended page table could not be finished within the
         * per-processor pool.
         *
         * A refused VM entry rather than a partial table, which is the one
         * place eager construction is stricter than filling on demand: a
         * lazily filled shadow can be discarded and rebuilt at will, an
         * eagerly built one that ran out has no partial state worth
         * entering with. BACKLOG.md records the pool size as a tuning
         * parameter and this as its defined consequence.
         */
        out_of_shadow_ept_tables = 20,

        /**
         * A guest hypervisor asked for a VM entry this VMM will not
         * make, and which of these it was decides the VM-instruction
         * error number the guest hypervisor is given.
         *
         * `nested_controls_unsupported` is a control setting outside what
         * the capability MSRs told it it could have, or an extended
         * page-table pointer the same MSRs refuse - both of which SDM
         * 29.2.1 makes checks on the VM-execution controls, so both
         * answer with error 7. `nested_host_state_unsupported` is SDM
         * 29.2.2's half of the same, and answers with error 8.
         *
         * `nested_msr_area_unsupported` is the one that is a limit here
         * rather than in the architecture: an MSR area may only name
         * indices this VMM will read and write, because it has no WRMSR
         * that can fault and recover - see `msr_area_index_handled`. An
         * index outside that list refuses the entry, which is the same
         * shape of answer a processor gives for an MSR it will not load,
         * rather than the silent skip that would leave a second-level
         * guest running without what its hypervisor asked for.
         * @{
         */
        nested_controls_unsupported = 21,
        nested_host_state_unsupported = 22,
        nested_msr_area_unsupported = 23,
        /**
         * @}
         */

        /**
         * IA32_EFER.NXE is clear on a processor that is about to run on
         * the host page table, which sets the execute disable bit.
         *
         * Refused rather than risked. SDM 5.5.4
         * (.references/sdm.txt:157079): "If IA32_EFER.NXE = 0, the XD
         * flag (bit 63) is reserved in every paging-structure entry", and
         * an entry with a reserved bit set faults on every access through
         * it rather than only on an instruction fetch. So the failure is
         * not "no-execute stops working", it is every mapping that
         * carries the bit becoming a page fault at once, in a place with
         * no recovery point - which is a dead machine with nothing
         * written down. One error code from the loader is worth more.
         */
        execute_disable_not_enabled = 24,

        /**
         * The module protection probe stored a byte into this module's
         * own text and the store was allowed. Compiled only under
         * -DZPP_TEST_MODULE_PROTECTION, which exists to ask exactly this.
         *
         * The passing result is not this code, it is a page fault: 0x60e03
         * from the host exception path - vector 14, error code 3, a
         * present page written by a supervisor. This code is what a
         * *failed* probe returns, and it refuses the launch rather than
         * continuing, because a build that boots is how a protection that
         * was never applied gets mistaken for one that was.
         */
        module_protection_missing = 25,
    };

    /**
     * Maximum number of CPUs supported.
     *
     * Thirty two rather than sixteen. Sixteen was already below what
     * current laptops ship - a mobile part with performance and
     * efficiency cores passes it easily - and the cost of being wrong
     * was not a refusal but a 512 KB stack written past the end of an
     * array, since launch_on_cpu did not bound its index. That is fixed
     * separately; this raises the ceiling to somewhere the fix is
     * unlikely to be reached.
     *
     * It is not free. Every per-processor array scales with it and the
     * stacks dominate: 512 KB each, so this member alone goes from 8 MB
     * to 16 MB. They are zero initialized and therefore live in .bss, so
     * the binary on disk barely moves and the loaded image does.
     */
    static constexpr std::size_t max_cpus = 32;

    /**
     * Page size.
     */
    static constexpr std::size_t page_size = 0x1000;

    /**
     * Large page size, which is the granularity every EPT page directory
     * entry maps at unless it has been split.
     */
    static constexpr std::size_t large_page_size = 0x200000;

    /**
     * Maximum module size in bytes.
     */
    static constexpr std::size_t max_module_size =
        100 * 1024 * 1024; // 100 MB.

    /**
     * Returns the single hypervisor instance, constructing it on the first
     * call.
     *
     * It cannot be constant initialized - constant evaluating the EPT
     * tables alone exceeds the compiler's constexpr step budget - so it is
     * built lazily instead. The heap is already up by the time anything
     * can reach this, so construction may allocate.
     *
     * The build uses -fno-threadsafe-statics, so the compiler emits a
     * plain guard byte test with no __cxa_guard_acquire, and the first
     * call must therefore not race.
     *
     * It does not, but **not** for the reason this comment used to give.
     * The old one said the loader launches processors strictly one at a
     * time. That is true of the Windows and Linux loaders, which loop
     * `call_on_cpu(i, ...)` blocking on each, and it is not the mechanism
     * under UEFI: `number_of_cpus()` returns 1 there, so the boot
     * processor is launched alone and the others are adopted much later
     * from the guest's own start-up IPIs. The serialisation being
     * appealed to is absent on the platform that matters most.
     *
     * The real reason is structural and holds on every platform. This has
     * exactly three callers - `zpp_hypervisor_main`,
     * `zpp_x86_64_exception` and `zpp_ap_start_up_main` - and the latter
     * two are only *reachable* once the boot processor has armed them
     * from inside `main`. The host IDT is built by `initialize_host_idt`
     * and loaded by `main`, both boot processor only. The trampoline page
     * is written by `initialize_start_up_memory`, also boot processor
     * only, and that is what puts `zpp_ap_start_up_main`'s address where
     * a starting processor will find it. Both therefore come alive
     * strictly after construction returned.
     *
     * What would break it is a fourth caller reachable before `main` has
     * armed those two. That is the thing to check when adding one, and
     * unlike the old justification it is checkable by grep.
     */
    static hypervisor & instance();

    /**
     * Launch the hypervisor on a the current CPU, caller must make
     * sure the context switch to another CPU cannot occur.
     * The caller context parameter must contain a context where the OS can
     * continue execution. The caller context must contain a zero based CPU
     * identifier in the range [0, num_cpus) in caller_context.rdi. The
     * first CPU identifier must be zero and each following CPU identifier
     * is incremented by one. The caller context must contain a function to
     * convert physical address within OS page tables to virtual address,
     * this function will be called only during the initialization phase of
     * the hypervisor, after this function returns there shall be no more
     * calls to the function.
     * On failure this function will restore the context with an error code
     * at caller_context.rax.
     */
    void launch_on_cpu(arch::x86_64::context & caller_context);

    /**
     * Records an exception the host IDT caught and unwinds to the recovery
     * point main established, so the launch fails with an error instead of
     * escalating to a triple fault. Public only because the entry stubs in
     * the exception entry table reach it through zpp_x86_64_exception - it
     * is not part of the interface a caller of this class should use.
     *
     * Halts if there is no recovery point, which is the case once the
     * guest is running: the VMCS points the host IDTR here, but main's
     * frame is gone by then and there is nowhere to unwind to.
     *
     * **Returns for a non-maskable interrupt**, which is the one vector
     * that is not caused by the instruction it interrupts and so can
     * simply be resumed from. That is what makes the wake probe in
     * wait_for_ept_acknowledgement usable: such a probe reaches a
     * processor that is inside its own VM exit, where NMI exiting does
     * not apply and the interrupt arrives here instead. Every other
     * vector still unwinds or stops.
     */
    void on_host_exception(const arch::x86_64::exception_frame & frame);

    /**
     * What the start-up trampoline jumps to, once the processor it is
     * running on has reached long mode on the host page table. Does not
     * return: this processor either ends up running the guest or stops.
     *
     * Public only because zpp_ap_start_up_main reaches it from outside the
     * class, in the same way the exception entry stubs reach
     * on_host_exception above. It is not part of the interface a caller of
     * this class should use.
     */
    void start_up_on_this_processor(std::uint64_t slot);

    /**
     * The same, for a processor arriving on an S3 waking vector rather
     * than a start-up IPI: it puts the FACS and the trampoline back the
     * way the guest needs them, rewinds the per-processor bookkeeping, and
     * launches.
     *
     * Public for the same narrow reason as the function above -
     * zpp_resume_from_sleep_main reaches it from outside the class - and
     * no more part of the interface than that one is.
     */
    void resume_from_sleep_on_this_processor(std::uint64_t slot);

    /**
     * Unwinds to the point a VM entry into a second-level guest was
     * decided from, after the processor refused that entry.
     *
     * Does not return. Public for the same narrow reason as the two above:
     * `zpp_vmx_nested_entry_failure` reaches it from the failure stub,
     * which is the only code that runs between the refusal and here.
     */
    [[noreturn]] void
    on_nested_entry_failure(arch::x86_64::context * recovery);

    // Everything below is private, except to the hosted test suite.
    //
    // `ZPP_HOSTED_TESTS` is defined by tests/CMakeLists.txt and by
    // nothing else - no target this tree ships defines it, and the four
    // cross builds do not pass it. What it buys is that a harness under
    // tests/ compiles *this* class rather than a hand-written copy of
    // it. There were six such copies, 1463 lines of them, and they
    // drifted: a member added here had to be added to up to six other
    // files or the suite went red.
    //
    // Two alternatives, both rejected with a reason:
    //
    //   - `friend`. Friendship does not reach the free functions a
    //     harness is written out of, so granting it would mean
    //     restructuring five thousand lines of test into members of one
    //     struct - a much larger change than this, for the same access.
    //   - making the members public outright. The shipped class keeps
    //     its encapsulation this way, and a reader sees one line saying
    //     who else may look and under what switch.
    //
    // Layout is unaffected, and that is checked rather than asserted:
    // [class.mem] fixes the order of members within one access class and
    // leaves it unspecified only *between* access classes, so removing
    // an access specifier can only make the order more determined, never
    // less. Measured both ways with clang - sizeof and every member
    // offset are identical, 0x2c3c000 either way, which is also what
    // llvm-dwarfdump reports for the built x86_64 hypervisor.
#ifdef ZPP_HOSTED_TESTS
public:
#else
private:
#endif
    /**
     * Capture important registers for later use of the hypervisor.
     */
    void initialize_registers();

    /**
     * Find the module base and size.
     */
    void initialize_module_region();

    /**
     * Initialize the OS page table object, allowing
     * to translate virtual addresses to physical addresses.
     */
    void initialize_os_page_table();

    /**
     * Initialize the host page table object, which is the page
     * table that is used once all module is mapped to it.
     */
    void initialize_host_page_table();

    /**
     * Create physical to virtual mapping for our module.
     */
    std::expected<void, zpp::error>
    initialize_module_physical_to_virtual();

    /**
     * Create the host GDT structures that will be used in the hypervisor.
     */
    void initialize_host_gdt();

    /**
     * Create the IDT entries for our hypervisor. Depends on the host GDT
     * having been built already, because the gates need the host code
     * segment selector.
     */
    void initialize_host_idt();

    /**
     * Load the host IDT.
     */
    void load_host_idt();

    /**
     * Load the OS IDT.
     */
    void load_os_idt();

    /**
     * Initialize an intermediate copy of the OS GDT to be used
     * when switching to host page table, but before we launch
     * our VMM and guest.
     */
    void initialize_intermediate_gdt(std::size_t cpu);

    /**
     * Load the intermediate GDT.
     */
    void load_intermediate_gdt(std::size_t cpu);

    /**
     * Load the OS GDT.
     */
    void load_os_gdt();

    /**
     * Initialize important VMX MSR registers and structures.
     */
    void initialize_vmx_msrs();

    /**
     * Returns the cached VMX MSRs register values.
     */
    std::uint64_t & cached_vmx_msr(std::size_t msr);

    /**
     * Initialize MTRR structures to be used when initializing
     * the hardware page tables.
     */
    void initialize_mtrrs();

    /**
     * Initialize hardware page table structures.
     *
     * Fallible because a 2 MB region whose MTRR coverage is not of one
     * type is split into 4 KB entries, and the pool those come from is
     * finite.
     */
    std::expected<void, zpp::error> initialize_ept();

    /**
     * The 4 KB EPT entry covering a host physical address, splitting the
     * 2 MB entry that covers it if that is what it takes.
     *
     * This is the one primitive everything that changes guest access
     * rights is built from. It used to be welded inside protect_module,
     * which meant the second caller had to either copy it or grow that
     * function a parameter - so it is a function now, and protect_module
     * is a loop over it.
     *
     * Fallible for one reason: splitting consumes a table from a finite
     * pool shared with initialize_ept.
     */
    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    epte_for(std::uint64_t physical_address);

    /**
     * Points the guest's view of the controller's register page at
     * register_shadow, or back at the real registers.
     *
     * Armed while this VMM drives the controller itself, so that a guest
     * driver polling CSTS on another processor keeps seeing the values it
     * expects rather than a controller that appears to have come back on
     * its own. That is the race that makes owning the controller for a
     * few milliseconds unsafe otherwise, and it is not a small window:
     * the driver's whole job at that moment is to poll that register.
     *
     * Writers are held for the duration by the page watch, so no
     * processor can program the controller while it is ours, and every
     * processor is made to acknowledge the change before it takes effect
     * - a stale translation would read the real register.
     */
    std::expected<void, zpp::error>
    shadow_controller_registers(bool armed);

    /**
     * Takes the controller for the length of one VM exit, while the guest
     * has itself disabled it, and gives it back exactly as it was.
     *
     * The guest's admin queue is never touched. Its AQA, ASQ and ACQ are
     * saved, ours are programmed in their place - legal only while CC.EN
     * is clear, which is precisely the window the guest has just opened -
     * a private I/O queue is created, whatever is staged is written, and
     * then the controller is disabled again and the guest's registers
     * restored. The guest re-initialises from scratch afterwards, which
     * is what it asked for when it cleared CC.EN.
     *
     * Bracketed by two controller resets, so the ordering hazard that
     * makes a borrow dangerous cannot arise: the guest's own Set Features
     * is still the first admin command after the last reset, and our
     * queue is destroyed before it creates any of its own, so no
     * identifier can collide.
     */
    std::expected<void, zpp::error> run_reset_excursion();

    /**
     * Flushes cached translations after an EPT entry has been changed.
     * Required by every modification made after launch - see the
     * definition for why this had no callers until page watches existed.
     */
    void invalidate_ept();

    /**
     * The invalidation without the announcement, for a processor that is
     * catching up with somebody else's change rather than making one.
     */
    void invalidate_ept_locally();

    /**
     * Prepare module protection from guest access.
     */
    std::expected<void, zpp::error> protect_module();

    /**
     * Hides a physical range from the guest, the way protect_module
     * hides the module.
     *
     * For memory that belongs to this VMM but does not live inside its
     * image - the storage the log queues are created in, which the
     * loader allocates. Anything the guest must not be able to write,
     * and in that case must not be able to write because the controller
     * executes what it finds there.
     */
    std::expected<void, zpp::error>
    protect_region(std::uint64_t physical_address, std::uint64_t size);

    /**
     * Points the window at a physical page and returns the address it can
     * be read and written through.
     *
     * For memory this VMM has to touch and does not own - the guest's
     * admin queue, whose address comes out of the controller's ASQ and
     * ACQ registers and is therefore not ours to choose. Mapping such a
     * page permanently is not an option: the host page table aliases, so
     * an address we did not pick could silently replace the module's own
     * mapping and unmap the VMM from under itself.
     *
     * The caller must hold mapping_window_lock for as long as it uses the
     * returned pointer, and the page must be one the guest is not
     * concurrently changing underneath - which for the admin queue is
     * what the borrow's exclusion is for.
     */
    void * map_window(std::uint64_t physical_address,
                      std::size_t pages = 1);

    /**
     * Which window page a run starts at, so two runs can be mapped at
     * once without one overwriting the other.
     */
    void * map_window_at(std::size_t first_page,
                         std::uint64_t physical_address,
                         std::size_t pages);

    /**
     * Copies out of, and into, guest physical memory.
     *
     * Guest physical rather than host physical because that is what a
     * guest hands over - a VMCS pointer, a VMXON pointer, a page table
     * root. The extended page tables this VMM builds are an identity map
     * of the first 512 GB (see initialize_ept), so the translation is the
     * identity and this does not walk them; what it does do is refuse an
     * address above that, since beyond the map there is no entry and a
     * host access to it would fault in root mode with no recovery point.
     *
     * Takes the mapping window lock for the duration, one page at a time,
     * so a copy that straddles a page boundary is two mappings rather
     * than a requirement on the window's size. The lock is not recursive,
     * so nothing called from inside these may take it again.
     * @{
     */
    std::expected<void, zpp::error> read_guest_physical(
        std::uint64_t guest_physical, std::span<std::byte> into);

    std::expected<void, zpp::error> write_guest_physical(
        std::uint64_t guest_physical, std::span<const std::byte> from);

    /** Records a caller of the two above. See its definition. */
    void note_guest_memory_caller(std::uint64_t caller);
    /**
     * @}
     */

    /**
     * Translates a guest linear address through the guest's own page
     * tables, as they are at this exit.
     *
     * The guest's CR3 comes from the VMCS rather than from the
     * os_page_table built at launch time, and that distinction is the
     * whole point of this existing: the launch-time table is a snapshot of
     * the firmware's identity map, and BACKLOG.md records what using it
     * later costs - a kernel linear address is used as a physical one, and
     * either the window maps a page above MAXPHYADDR and the copy faults
     * in root mode, or unrelated bytes are read and acted on.
     *
     * Four-level paging only, which is what the guest is in whenever this
     * is reachable: everything that calls it is a VMX instruction, and
     * SDM 33.3 makes every one of them raise #UD unless CR0.PE is set,
     * with IA32_EFER.LMA and CS.L agreeing. Five-level paging (CR4.LA57)
     * is refused rather than guessed at.
     */
    std::expected<std::uint64_t, zpp::error>
    guest_linear_to_physical(std::uint64_t linear);

    /**
     * Reserves the disk channel's I/O queue allocation, by borrowing the
     * guest's admin queue for one Set Features (Number of Queues).
     *
     * Called from the exit that saw the guest enable the controller, and
     * only from there. That is the one moment when the borrow is free:
     * the driver has just written CC.EN and must now poll CSTS.RDY, which
     * the architecture allows the controller CAP.TO x 500 ms to answer,
     * so a borrow that takes milliseconds is indistinguishable from a
     * slightly slow controller. The admin queue has also just been reset,
     * so nothing of the guest's is outstanding in it, and the driver's
     * initialisation path is single threaded - no other processor is
     * touching it.
     *
     * It is also the one moment when the *reservation* is legal, and that
     * is the stronger reason. NVMe Base 5.2.30.1.5 - quoted verbatim in
     * NVME-LOG.md - says the allocation is cleared by a controller level
     * reset and established by the first Set Features completed after
     * one, and that a Set Features issued after any I/O queue has been
     * created is aborted with Command Sequence Error. So this has to run
     * after the reset and before anything is created, which is exactly
     * here.
     *
     * **Creates nothing.** Creating is create_channel_queue, and it
     * happens later for the same reason: our Create must not precede the
     * guest's own Set Features.
     *
     * Costs this processor the length of one lap, measured at 16.6 us per
     * admin command against this controller. Costs every other processor
     * nothing.
     */
    void reserve_channel_queue_allocation();

    /**
     * Creates the channel's own queue pair, once the guest has created
     * its own.
     *
     * Called from the exit that saw the guest ring its admin doorbell for
     * the Create that completed its set, and only when the identifiers
     * chosen from what it created are inside the reserved allocation.
     * Unlike the reservation this runs while the guest's driver is live,
     * which is the whole of what makes it risky - see
     * diag::create_channel_queue_after_guest for what stands in for
     * quiescence and what does not.
     */
    void create_channel_queue();

    /**
     * Rewrites the Number of Queues answer the guest is about to read, so
     * that one submission queue identifier is left over for the channel.
     *
     * Called from the exit that saw the guest ring its admin doorbell for
     * its own Set Features (Number of Queues), after the submission has
     * been parsed and the command identifier taken off it. The controller
     * has been told to fetch that command, so this waits for the
     * completion the controller posts for it and edits DW0 in place
     * before the guest - which is this processor, held inside this exit -
     * can look at it.
     *
     * Nothing is submitted, nothing is injected and no doorbell is rung.
     * The completion is the guest's own, at the guest's own slot with the
     * guest's own phase and command identifier, and four bytes of it are
     * changed. See diag::reduce_guest_queue_grant for why the answer has
     * to be edited at all, and for what losing the race against the
     * guest's own interrupt service routine would cost.
     */
    void patch_guest_queue_grant();

    /**
     * The most queues the guest can create in one space: what it asked
     * for, clamped by what it was told it was granted.
     *
     * Both drivers clamp their creates this way - Linux takes
     * `min(*count, nr_io_queues)` in `nvme_set_queue_count`, Windows'
     * stornvme clamps to NSQA+1 and NCQA+1 - so this is the number that
     * decides both when the guest has finished creating and which
     * identifier is free above it. A granted count of zero means nothing
     * was ever reported, in which case the guest's request is all this
     * knows and the answer is the request.
     */
    static constexpr std::uint64_t queue_limit(std::uint64_t requested,
                                               std::uint64_t granted)
    {
        return ((0 != granted) && (granted < requested)) ? granted
                                                         : requested;
    }

    /**
     * Runs one borrow of the guest's admin queue with the given payload,
     * excluding every other processor for its duration.
     *
     * Everything both callers above share: find the guest's queues from
     * the controller's own registers, reach them through the mapping
     * window, wait for the queue to fall quiescent, get every processor
     * to acknowledge the doorbell protection, hold it, borrow, and let
     * go. What differs between them is only the payload and when they
     * are called.
     *
     * Returns zero on success. Any other value is a refusal code, kept
     * distinct from nvme::borrow_result's small numbers by starting at
     * 0xf0, and recorded so that a channel which stays down says which
     * step it stopped at.
     *
     * The caller must **not** hold mapping_window_lock: this takes it,
     * and it is not recursive.
     */
    std::uint64_t
    borrow_guest_admin_queue(const nvme::submission_entry * payload,
                             std::uint32_t payload_count,
                             std::uint16_t * payload_status,
                             std::uint32_t * payload_result);

    /**
     * Records what the guest just submitted on its admin queue, from the
     * entries between where this last looked and the tail it has just
     * published.
     *
     * The caller holds mapping_window_lock, which is why this is a
     * function of its own: the borrow that may follow takes that lock
     * too, so the observation has to finish and let go before it starts.
     */
    void observe_guest_admin_submissions(std::uint32_t tail);

    /**
     * Removes the doorbell page watch, once there is nothing left to see
     * on it.
     *
     * With a doorbell stride of zero - which is what real hardware
     * reports - every I/O queue's doorbell shares the page with the admin
     * one, so a watch left armed is an exit per disk command for the rest
     * of the boot. NVME-LOG.md says the trap is armed only between the
     * enable and our queues existing, and this is the second half of
     * that.
     *
     * Does nothing under observe_controller_admin, whose whole purpose is
     * to price a permanently trapped doorbell page.
     */
    void stop_watching_channel_doorbells();

    /**
     * Forgets everything observed about the guest's queue configuration.
     *
     * Called when the guest disables the controller, because a controller
     * level reset clears the allocation and deletes every queue - so the
     * identifiers, the counts and the reservation all describe a
     * controller that no longer exists.
     */
    void forget_channel_queue_observations();

    /**
     * Turns the preemption timer on or off.
     *
     * On, this VMM gets an exit every controller_poll_microseconds
     * whatever the guest is doing - which is the point, because a guest
     * spinning on a memory mapped read of a passed through device takes
     * no exits of its own and that is precisely when the controller's
     * state has to be seen.
     */
    void arm_controller_poll(bool armed);

    /**
     * Arms or releases the fallback clock: the guest's own timer.
     *
     * Used where the processor refuses the preemption timer, which is not
     * hypothetical - the rig runs under an outer hypervisor that filters
     * the capability MSRs and does not offer it. Without a clock the
     * channel is driven by guest exits alone and stops the moment the
     * guest settles, which is measured in arm_controller_poll.
     */
    void arm_guest_timer_poll(bool armed);

    /**
     * Translates a guest linear address through the guest's own page
     * table, as of this exit.
     *
     * The caller must hold mapping_window_lock: the walk reads each level
     * through the window, and the lock is not recursive so this cannot
     * take it. It also shares a window page with the instruction fetch,
     * so a caller doing both must finish every walk before mapping
     * anything for the fetch.
     *
     * Long mode only. A guest under 32-bit paging has a different table
     * shape, and answering for it with a four level walk would be worse
     * than refusing.
     */
    std::optional<std::uint64_t>
    translate_guest_linear(std::uint64_t linear);

    /**
     * Where the decoder's instruction length disagreed with the
     * processor's, and what the two said.
     *
     * Read from a debugger. A disagreement means the decoder misread an
     * instruction whose store has already been applied to a device
     * register, so it is not resumed from - but it has to be visible,
     * because a processor that stops with no record of why is the worst
     * outcome of the three.
     * @{
     */
    volatile std::uint64_t emulated_length_disagreement{};
    volatile std::uint64_t emulated_length_reported{};
    volatile std::uint64_t emulated_length_decoded{};
    /**
     * @}
     */

    /**
     * Waits until every running processor has picked up the extended
     * page table change just made, or gives up.
     *
     * This is what turns the generation counter from a best effort into
     * a guarantee, and it is the piece a borrow needs: protecting the
     * guest's doorbell is worth nothing if another processor is still
     * writing through a translation cached before the protection.
     *
     * No interprocessor interrupt is involved, and none is available -
     * this VMM leaves "external-interrupt exiting" clear, so an interrupt
     * it sent would be delivered into the guest's own handler rather than
     * causing a VM exit (SDM 28-7). Turning that control on means
     * intercepting and re-injecting every interrupt the guest receives,
     * which is a great deal of machinery to buy a shorter wait.
     *
     * It is not needed, because the acknowledgement is observable
     * directly: each processor stamps ept_generation_seen on its own next
     * exit, and a guest that is running takes exits constantly. So this
     * waits for what it can see rather than demanding what it cannot
     * send.
     *
     * Returns false if some processor did not answer inside the budget,
     * and a caller that needs the guarantee must then do nothing. Not
     * borrowing is always safe; borrowing without exclusion is not.
     */
    /**
     * Waits until every processor has picked up the current extended page
     * table generation.
     *
     * `probe` decides whether a processor that has not answered is sent a
     * wake NMI. It must be false for any caller that a processor might be
     * spinning inside, because such a processor is in **root mode**, NMI
     * exiting governs non-root operation only, and the NMI therefore
     * arrives at the host IDT - where on_host_exception finds no recovery
     * point and halts it for ever. That took the development machine off
     * the network once; a passive wait that gives up is always safe, and a
     * caller that cannot proceed without the acknowledgement should treat
     * giving up as a refusal rather than force the issue.
     */
    bool wait_for_ept_acknowledgement(std::uint64_t budget,
                                      bool probe = true);

    /**
     * Sends a non-maskable interrupt to one processor, to take it out of
     * whatever it is doing.
     *
     * The only tool available for this. An ordinary interrupt is
     * delivered into the guest's own handler while "external-interrupt
     * exiting" is clear, so it causes no exit at all; and a processor
     * the guest has halted executes nothing, so it reaches no exit path
     * by itself. Measured: an acknowledgement wait that never completed
     * because processor 1 was halted and three generations behind.
     *
     * Written straight to the local APIC's command register, which the
     * host page table already maps for the interrupt command watch.
     * Delivery mode 100b is NMI, and the destination is a physical APIC
     * id.
     */
    void send_wake_nmi(std::uint64_t apic);

    /**
     * Sends a start-up IPI to one processor, in whichever local APIC mode
     * this one is actually in.
     *
     * The mode is not a detail here, it is the whole function. The two
     * places that started a processor both wrote the **x2APIC** interrupt
     * command MSR unconditionally, and that MSR does not exist while the
     * APIC is in xAPIC mode - the write takes a general protection fault.
     * Measured exactly that way: vector 13, error code 0, faulting RIP
     * inside `wrmsr`, with the boot processor left halted in
     * `on_host_exception` and no VM exit record to explain it, because
     * the fault was in this VMM rather than in the guest.
     *
     * It was reached the moment a broadcast start-up IPI was resolved
     * into per-processor ones, but it was never specific to that: the
     * targeted path had the same write and the same fault waiting in it.
     * A guest that writes its command to the APIC *page* - which is what
     * "xAPIC" means, and what this machine's guest does - is by
     * definition on a processor where that MSR is unavailable.
     */
    void send_start_up_ipi(std::uint64_t apic, std::uint64_t vector);

    /**
     * Whether the NMI a processor is taking is one this VMM sent.
     *
     * Set before the interrupt is sent and cleared by the processor that
     * takes it. A guest's own NMI finds this clear and is handed back
     * rather than swallowed - swallowing one would lose a watchdog or a
     * machine check the guest was relying on.
     */
    std::atomic<bool> wake_requested[max_cpus]{};

    /**
     * How many NMIs were sent to wake a processor, and how many arrived
     * that were the guest's own and had to be given back.
     * @{
     */
    std::uint64_t wake_nmis_sent{};
    std::uint64_t guest_nmis_reinjected{};

    /**
     * How many processors were read as not executing because they did not
     * answer a non-maskable interrupt. Worth counting: it is the one
     * inference in the exclusion rather than an observation.
     */
    std::uint64_t unresponsive_processors{};

    /**
     * How many exits each processor has taken, for the heartbeat.
     *
     * Per processor and not shared, so the counter costs an increment
     * with no contention on a path every exit runs through.
     */
    std::uint64_t heartbeat_exits_seen[max_cpus]{};

    /**
     * How many writes to a watched page were emulated from a decoded
     * instruction, and how many had to be stepped over instead.
     *
     * The ratio is the decoder's coverage of the traffic that actually
     * occurs, which is the only measure of it that matters - a decoder
     * that handles every form nobody uses is worth nothing. A stepped
     * write is also the one that can still lose a transition, so a
     * non-zero second number is a live hole rather than an inefficiency.
     *
     * volatile because nothing in this program reads them. Without it
     * the stores are dead and the optimizer removes them, leaving the
     * symbols reading their zero initializers for ever.
     */
    /**
     * How many times each processor has reached the resume at the end of
     * the exit handler.
     *
     * Exists to tell "stuck before the resume" from "resumed into a guest
     * that does not execute", which no reading available from outside can
     * distinguish: an application processor stuck in root mode and one
     * whose guest silently fails to advance both show a frozen exit count
     * and an unchanging guest state. If this equals the exit count then
     * the resume was reached every time and the guest is the problem; if
     * it lags by one then the processor never got there.
     *
     * volatile because only a debugger reads it.
     */
    /**
     * Whether this processor has already said the timer is unavailable.
     */
    bool timer_refusal_reported[max_cpus]{};

    /**
     * Why a VM entry failed, per processor, captured by
     * zpp_vmx_entry_failed while the VMCS is still current.
     *
     * `flags` distinguishes the two failure kinds: carry set means there
     * was no current VMCS at all, zero set means the instruction error
     * field below says which check failed (SDM 31.4).
     */
    volatile std::uint64_t entry_failure_flags[max_cpus]{};
    volatile std::uint64_t entry_failure_error[max_cpus]{};
    volatile std::uint64_t entry_failures_seen{};

    /**
     * How the reset excursions went, for a reader with no other channel.
     */
    volatile std::uint64_t excursions_completed{};
    volatile std::uint64_t excursions_refused{};
    volatile std::uint64_t excursion_error{};

    /**
     * Times the return to the real registers could not be confirmed on
     * every processor. The change was made; the acknowledgement was not.
     */
    volatile std::uint64_t unshadow_unconfirmed{};

    volatile std::uint64_t resumes_reached[max_cpus]{};

    /**
     * The activity state, guest RIP and CS each processor was last
     * resumed with.
     *
     * A guest that enters successfully and then executes nothing is a
     * guest that is not runnable, and the only architectural state that
     * does that is the activity state - 2 for shutdown, 3 for
     * wait-for-SIPI. Nothing outside can read the field, so it is
     * recorded here at the moment it matters.
     *
     * The activity state is **consumed** as well as read, which is what
     * makes its freshness load bearing rather than cosmetic:
     * `start_up_processor` gates a guest's start-up IPI on it. The exit
     * path samples it on the way out, so it describes the *previous* exit
     * for the whole of any wait a handler takes - and
     * `emulate_init_signal` takes one that is up to two million iterations
     * long. It therefore writes this field itself before it waits, exactly
     * as `enter_or_park_l2` writes `l2_activity_state` before parking. Any
     * future handler that waits in root mode owes the same.
     */
    volatile std::uint64_t resume_activity_state[max_cpus]{};
    volatile std::uint64_t resume_guest_rip[max_cpus]{};
    volatile std::uint64_t resume_guest_cs[max_cpus]{};

    volatile std::uint64_t emulated_writes{};

    /**
     * Writes a watch refused, counted separately from the ones applied.
     *
     * A refused write is the interesting one: it is a command this VMM
     * decided to answer itself rather than let reach the device. Zero
     * here while a guest is starting processors means the interception
     * is not happening, which is indistinguishable from every other
     * reason a processor fails to start unless it is counted.
     */
    volatile std::uint64_t filtered_writes{};

    /**
     * Interrupt commands the local APIC page filter saw before they were
     * sent. Distinct from filtered_writes, which counts the ones refused.
     */
    volatile std::uint64_t apic_page_commands_filtered{};
    volatile std::uint64_t stepped_writes{};

    /**
     * Where a watched access's offset within its page came from.
     *
     * `decoded` counts the ones the exit could not answer and the
     * instruction could; `unknown` counts the ones neither could, which
     * fall back to the page-granular physical address and so report
     * offset zero. A non-zero `unknown` means a watch is being told about
     * a register that was not the one touched, which is exactly the
     * failure that hid a guest hypervisor's start-up interrupts - so it
     * is counted rather than left to be inferred. It also decides whether
     * the access can be *emulated*, since an address that is not known
     * cannot be written to.
     * @{
     */
    volatile std::uint64_t access_offset_decoded{};
    volatile std::uint64_t access_offset_unknown{};
    /**
     * @}
     */

    /**
     * Whether the guest-physical address's low twelve bits are the
     * faulting offset, which decides whether any of the above is needed.
     *
     * SDM 30.2.1 says the field "receives the guest-physical address that
     * caused the EPT violation", and gives exactly one case where bits
     * 11:0 are cleared - an instruction executing in enclave mode. By
     * that text the offset is always available and the decoder is never
     * needed for it. Against that stands a measurement recorded in
     * `on_ept_violation`: twenty-four consecutive accesses to the local
     * APIC page all reporting offset zero.
     *
     * Both cannot be right, and the disagreement matters because the
     * decoder's guess is what picks *which* device register a write
     * lands on. It is also not a pure hardware question here: the rig
     * runs this VMM as KVM's own guest, so these fields are KVM's
     * construction rather than the processor's - `nested.c:460` assigns
     * `vmcs12->guest_physical_address = fault->address` - which is a
     * reason the answer could differ from bare metal and from the SDM
     * both.
     *
     * So it is counted rather than argued about:
     *
     * - `physical_offset_present` counts violations whose guest-physical
     *   address has any of bits 11:0 set. Zero over a whole boot means
     *   this environment page-aligns the field and the decoder is load
     *   bearing.
     * - the `agreed`/`disagreed` pair is the direct test, taken only on
     *   the exits that also report a valid guest-linear address, where
     *   the two low halves must be equal. Any disagreement means one of
     *   the two sources is wrong about the register being touched.
     * @{
     */
    volatile std::uint64_t physical_offset_present{};
    volatile std::uint64_t physical_offset_agreed{};
    volatile std::uint64_t physical_offset_disagreed{};
    /**
     * @}
     */

    /**
     * What a guest hypervisor was last told when a VMX instruction of
     * its own failed, and how many times.
     *
     * These exist because their absence was mistaken for evidence. Only
     * one of the refusal paths logs - the one where `build_vmcs02`
     * rejects the controls - and the three early ones in
     * `on_guest_vmlaunch` return a plain VMfail with nothing recorded
     * anywhere. So "no VM entry is being refused" was read off a missing
     * log line, and a guest hypervisor executing VMRESUME thousands of
     * times a second and being told error 5 each time looked exactly
     * like one that had stopped asking. Those have entirely different
     * causes and the counters separate them.
     *
     * `nested_last_vmfail` holds the VM-instruction error number, or
     * zero for a VMfailInvalid, which carries none.
     * @{
     */
    volatile std::uint64_t nested_vmfail_count[max_cpus]{};
    volatile std::uint64_t nested_last_vmfail[max_cpus]{};
    /**
     * @}
     */

    /**
     * What the rebuild read out of the controller before borrowing, and
     * how far the borrow got. Enough to tell "the queue was described
     * wrongly" from "the queue was described correctly and the controller
     * ignored it".
     * @{
     */
    std::uint64_t rebuild_submission_depth{};
    std::uint64_t rebuild_completion_depth{};
    std::uint64_t rebuild_submission_base{};
    std::uint64_t rebuild_completion_base{};
    std::uint64_t rebuild_stride{};
    std::uint64_t rebuild_issued{};
    std::uint64_t rebuild_reaped{};
    std::uint64_t rebuild_total{};
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * Watches one page of guest physical memory for writes.
     *
     * Deliberately a general facility rather than a hook for whatever
     * needed it first. A caller names a page and a function; when the
     * guest writes anywhere in that page, the write is allowed to happen
     * and then the function is told it did. Nothing here knows what a
     * page contains, and the handler learns what changed by reading the
     * device or the memory itself.
     *
     * That last point is what keeps this small: an EPT violation reports
     * the address and whether the access was a read, a write or a fetch,
     * and **it does not report the data** - SDM Table 30-7, and the
     * instruction information field of SDM 30.2.4 is not populated for
     * this exit. Emulating the access would therefore need an x86
     * instruction decoder. Letting the guest's own instruction run and
     * then looking at the result needs none, and cannot disagree with
     * hardware about what the instruction meant.
     *
     * The step is done with the monitor trap flag: on the violation the
     * page is opened and MTF armed, and on the MTF exit - one retired
     * instruction later - the page is closed again and the handler runs.
     *
     * **The open window is visible to every other processor.** EPT is
     * shared by all of them through one EPTP, so while one CPU is
     * stepping, another writing the same page is not seen. That is
     * acceptable for a watch on something only one processor touches at
     * a time, and it is not a general guarantee. A watch that must miss
     * nothing needs the guest quiesced or an EPT hierarchy per CPU;
     * neither is built, and this comment is the warning to whoever needs
     * one.
     */
    /**
     * A write a guest made to a watched page, as carried out by this VMM
     * on its behalf.
     *
     * The address comes from the VMCS and the value and width from
     * decoding the instruction, which is the division the hardware
     * imposes: an EPT violation reports where a write went and never
     * what it wrote - SDM Table 30-7.
     */
    struct guest_write
    {
        /**
         * Guest physical address of the write.
         */
        std::uint64_t address{};

        /**
         * The value written, zero extended, of which `size` bytes count.
         */
        std::uint64_t value{};

        /**
         * Width in bytes: 1, 2, 4 or 8.
         */
        std::uint8_t size{};
    };

    struct page_watch
    {
        /**
         * Called after the guest's write has retired. Given the page
         * that was written, not the address, because the step tells us
         * which page was opened and not which byte the instruction
         * touched.
         */
        /**
         * Called after the write has taken effect.
         *
         * `store` is what was written, when the write was emulated from
         * a decoded instruction, and null when it was stepped over
         * instead because the instruction was not one the decoder
         * handles. A handler that needs the value must cope with both:
         * re-reading the register is the fallback, and it is the racy
         * one, which is why the decoded value is passed at all.
         */
        using handler = void (*)(void * context,
                                 std::uint64_t page,
                                 const guest_write * write);

        /**
         * What a violation on this page does.
         *
         * `notify` is the observing form: allow the write, step over it,
         * report it afterwards.
         *
         * `hold` is the excluding form, and it is the more useful of the
         * two. A processor that faults on a held page is **not resumed
         * until the holder releases it** - it spins inside the exit. That
         * is mutual exclusion over a structure the guest owns, obtained
         * without the guest's cooperation and without stopping any
         * processor that is not touching it.
         *
         * It exists for borrowing the controller's admin queue while the
         * guest is live. Detecting a concurrent submission is not enough
         * there, because by the time it is detected the damage is done;
         * what is needed is that it cannot happen, and holding the writer
         * at the faulting instruction is exactly that. The write has not
         * taken effect when the fault is delivered, so the queue is
         * unchanged for as long as the hold lasts.
         */
        enum class mode
        {
            notify,
            hold,
        };

        std::uint64_t page{};
        handler on_write{};

        /**
         * Called *before* the guest's write is allowed to take effect,
         * with the page it is about to land on.
         *
         * The difference matters for the controller's register page. A
         * write there can be the one that clears CC.EN, and once that has
         * landed the queues are gone - so anything still staged is
         * unwritable and gets counted into lost_to_reset instead. This
         * hook is the last moment at which the queue is still the
         * guest's own working controller, so it is where a flush has to
         * happen.
         *
         * It cannot know *which* register is being written without
         * decoding the instruction, and it does not need to: the pages
         * this is armed on are touched while a driver sets itself up and
         * almost never afterwards, so flushing on any write to one is
         * cheap and always safe.
         */
        void (*before_write)(void * context, std::uint64_t page){};

        /**
         * Called *instead of* applying the guest's write, with the value
         * it was about to write, and able to refuse it.
         *
         * Returns the value to write, or nothing to suppress the write
         * entirely. A handler that only wants to observe returns what it
         * was given.
         *
         * `before_write` above cannot do this: it returns void, so by the
         * time anything has been decided the write is going to happen
         * anyway. That is fine for flushing a queue and useless for a
         * register whose *side effect* is the thing being intercepted.
         *
         * The local APIC's interrupt command register is exactly that
         * register, and this is the hook it always needed. Writing its
         * low half sends the interrupt, so a VMM that wants to redirect
         * a start-up IPI into its own trampoline has to decide before the
         * write lands, not after. Measured before this existed: the
         * guest's broadcast start-up IPI reached the hardware first, took
         * all seven application processors out of the wait-for-SIPI state
         * in the same instant, and every trampoline IPI this VMM then
         * sent them was ignored - trampoline stage 0, seven times, while
         * the one processor that did come up had simply won the race.
         *
         * Only reachable when the write is emulated. A stepped write is
         * performed by the guest's own instruction, so there is no moment
         * between deciding and applying for anything to happen in - which
         * is one more reason emulating is the path that matters.
         */
        using filter = std::optional<std::uint64_t> (*)(
            void * context, std::uint64_t page, const guest_write * write);

        filter filter_write{};

        void * context{};
        mode behaviour{mode::notify};
        bool armed{};

        /**
         * Whether writers are currently being held. Atomic because it is
         * set by whichever processor is borrowing and read by any other
         * that faults, with no lock between them.
         */
        std::atomic<bool> held{};
    };

    /**
     * Arms a write watch on the page holding a guest physical address.
     *
     * Guest physical rather than host physical because that is what a
     * caller has: it is what an EPT violation reports and what a device
     * BAR is programmed with. The two are the same in this VMM, which
     * builds an identity EPT - stated here rather than assumed, because
     * it stops being true the moment anything remaps a guest page.
     */
    std::expected<void, zpp::error> watch_guest_page_writes(
        std::uint64_t guest_physical,
        page_watch::handler on_write,
        void * context,
        page_watch::mode behaviour = page_watch::mode::notify,
        void (*before_write)(void * context, std::uint64_t page) = nullptr,
        page_watch::filter filter_write = nullptr);

    /**
     * Starts and stops holding writers to a watched page.
     *
     * Between these two calls any processor that writes the page stops
     * at the faulting instruction and does not proceed. The caller must
     * therefore finish quickly and must not itself take anything a held
     * processor could already own.
     *
     * Returns false if the page is not watched in holding mode, so a
     * caller cannot believe it has exclusion it does not have.
     */
    bool hold_guest_page(std::uint64_t guest_physical);
    void release_guest_page(std::uint64_t guest_physical);

    /**
     * Removes a write watch and gives the page back to the guest.
     */
    void unwatch_guest_page(std::uint64_t guest_physical);

    /**
     * Handles an EPT violation. Returns whether it was ours - a false
     * means nothing had that page watched, which is a bug rather than a
     * guest error, and the caller stops the CPU.
     *
     * The address is a parameter rather than read from the VMCS because a
     * second-level guest's violation names a *second-level* address, and
     * no watch is keyed on one. The nested path composes the two levels
     * first and passes what this VMM's own tables refused.
     */
    bool on_ept_violation(std::size_t cpu,
                          arch::x86_64::context & context,
                          std::uint64_t guest_physical);

    /**
     * The size of the guest's current code segment, which decides what the
     * bytes at its instruction pointer mean.
     *
     * From the CS access rights in the VMCS, whose format SDM Table 27-2
     * gives: bit 13 is L, "64-bit mode active (for CS only)", and bit 14
     * is D/B, "Default operation size (0 = 16-bit segment; 1 = 32-bit
     * segment)". L wins, since a 64-bit code segment has D/B clear.
     *
     * Sixteen bits is a real answer here rather than a corner case. This
     * VMM enables unrestricted guest and starts application processors in
     * real mode, and `apply_start_up` writes those very access rights with
     * `code_64_bit(false)` and `default_operation_size(false)`.
     */
    arch::x86_64::code_size guest_code_size();

    /**
     * Why an entry is in the ring, so the two kinds cannot be mistaken for
     * each other while reading it.
     */
    enum class refusal : std::uint8_t
    {
        /**
         * The decoder does not answer this form. Expected, and the number
         * of them is a coverage measurement rather than a fault.
         */
        not_decoded,

        /**
         * The decoder answered, and the answer cannot be true of the
         * instruction that faulted. See impossible_decodes.
         */
        impossible_operation,
    };

    /**
     * Puts the opening bytes of an instruction into the ring, with the
     * reason it is there and the page it faulted on.
     *
     * One place rather than two, because the two reasons are recorded from
     * the same function and a ring whose entries were written by two
     * different pieces of code would be the easiest thing here to get
     * subtly out of step.
     */
    void record_refused_instruction(const std::uint8_t * code,
                                    refusal why);

    /**
     * Decodes the store that caused the current EPT violation.
     *
     * Nothing if the instruction is not one the decoder handles, which
     * is the caller's signal to fall back to stepping over it.
     */
    std::optional<arch::x86_64::decoded_instruction>
    decode_guest_instruction(std::size_t cpu,
                             arch::x86_64::context & context);

    /**
     * Performs a decoded store against guest physical memory.
     */
    bool apply_guest_store(std::uint64_t guest_physical,
                           const arch::x86_64::memory_store & store);

    /**
     * Handles the monitor trap flag exit that a watched write is stepped
     * with. Returns whether a step was in progress on this CPU.
     */
    bool on_monitor_trap_flag(std::size_t cpu);

    /**
     * Sets or clears the monitor trap flag in the primary processor
     * based controls of the current VMCS.
     */
    void monitor_trap_flag(bool value);

    /**
     * Watches the memory mapped local APIC for interrupt command writes,
     * which is the only way to see an IPI sent by a guest that is not in
     * x2APIC mode.
     *
     * The MSR bitmap catches the x2APIC form and cannot catch this one:
     * in xAPIC mode the interrupt command register is two dwords in a
     * page of ordinary device memory, and writing them is a store, not
     * an instruction this VMM is told about.
     *
     * Armed only while the guest is actually in xAPIC mode. The page is
     * hot - the end of interrupt register lives in it and is written on
     * every interrupt - so trapping it permanently would be a real cost
     * for a mode a modern guest leaves within milliseconds of booting.
     */
    void watch_local_apic(bool watch);

    /**
     * The state of a processor's local APIC, as this VMM last saw it.
     *
     * SDM 13.12.5.1 lists exactly these: "APIC disabled:
     * IA32_APIC_BASE[EN]=0 and IA32_APIC_BASE[EXTD]=0", "xAPIC mode:
     * ...EN=1 and ...EXTD=0", "x2APIC mode: ...EN=1 and ...EXTD=1", and
     * an invalid fourth that a WRMSR cannot reach - "An execution of
     * WRMSR to the IA32_APIC_BASE_MSR that attempts a transition from a
     * valid state to this invalid state causes a general-protection
     * exception."
     */
    enum class apic_mode : std::uint8_t
    {
        /**
         * Not observed yet. A processor that has never launched, which is
         * most entries for most of a boot.
         */
        unknown,
        disabled,
        xapic,
        x2apic,
    };

    /**
     * Reads this processor's IA32_APIC_BASE, records the mode it is in,
     * and re-plumbs both interceptions to match what the machine as a
     * whole now needs.
     *
     * Both are re-derived from *every* processor's recorded mode rather
     * than from the caller's, and that is the point of keeping the array
     * rather than a single value. The interceptions are global - one MSR
     * bitmap shared by every VMCS, one set of extended page tables - and
     * a guest switches its processors to x2APIC one at a time. Deriving
     * from the caller alone would disarm the xAPIC page watch the instant
     * the *first* processor switched, and the remaining ones would still
     * be sending their interrupt commands through that page, unseen. That
     * is the mechanism this VMM adopts application processors by.
     */
    void note_apic_mode(std::size_t cpu);

    /**
     * Sets or clears one port's bit in the I/O permission bitmaps, so
     * that accesses to it exit. A port is watched only when something
     * asks for it; everything else stays with the guest.
     */
    void intercept_io_port(std::uint16_t port, bool intercept);

    /**
     * Handles an I/O instruction that exited. Returns whether it was one
     * this VMM asked to see.
     *
     * Takes the guest context because the value an OUT carries is in
     * RAX - see the note in the definition about why that needs no
     * instruction decoder.
     *
     * Clears re_execute when this VMM performed the access itself, so the
     * guest is resumed past its own instruction rather than at it. Left
     * set on every path that hands the instruction back to the guest.
     */
    bool on_io_instruction(arch::x86_64::context & context,
                           bool & re_execute);

    /**
     * Everything this VMM does about a guest asking for a sleep state.
     *
     * Separate from on_io_instruction because that function's job is to
     * recognise the port; this one's is to decide what a sleep means, and
     * the two answer to different references. Returns whether the guest
     * still has to execute its own OUT - true on the pass-through path,
     * false when this VMM performed the write itself.
     *
     * See zpp/hypervisor/power.h for what the three transitions require.
     *
     * `bytes` is how wide the guest's own access was, from the exit
     * qualification, or zero where that field held a value SDM Table 30-5
     * does not define.
     */
    bool on_sleep_request(std::uint16_t port,
                          std::uint32_t value,
                          std::uint8_t bytes);

    /**
     * Takes this processor out of VMX operation cleanly and then performs
     * the guest's sleep write itself, so that the platform removes power
     * from a processor holding no active VMCS.
     *
     * Called only from on_sleep_request and only with
     * power::quiesce_on_sleep on. Returns normally if the write did not
     * sleep the machine, having put this processor back into VMX operation
     * with its VMCS current and its launch state clear - so the caller has
     * to VMLAUNCH rather than VMRESUME. Returns an error if it could not
     * get back, in which case there is nothing left to resume into and the
     * caller stops the processor.
     */
    std::expected<void, zpp::error> quiesce_and_sleep(std::uint16_t port,
                                                      std::uint32_t value,
                                                      std::uint8_t bytes);

    /**
     * The physical address of this processor's own VMXON and VMCS regions,
     * derived from the VPID.
     *
     * There used to be two shared scalars naming "the current" regions,
     * and these existed because those named whichever processor was
     * virtualized last. They are gone - enter_root_mode derives its own
     * from its slot - so these are now the only way to ask the question,
     * which is what they should always have been.
     * @{
     */
    std::uint64_t own_vmxon_region_physical();
    std::uint64_t own_vmcs_region_physical();
    /**
     * @}
     */

    /**
     * Reads the firmware waking vector the guest left in the FACS, into
     * guest_waking_vector, and says whether it found one.
     *
     * Read only. Nothing is written to the table, and nothing about the
     * resume changes - the point is to establish that the table was found
     * and that the guest did leave a vector there, which is the fact a
     * resume path rests on and the one thing that can be checked without
     * risking a machine that does not come back.
     *
     * Reached through the mapping window, because the FACS is ordinary
     * firmware memory and the host page table maps the module, itself and
     * the local APIC page and nothing else.
     */
    bool observe_guest_waking_vector();

    /**
     * What the FACS says about where a resume goes, read out of guest
     * memory and checked before any of it is believed.
     *
     * A structure rather than four out-parameters because the four are
     * only meaningful together: a vector is worth nothing without the
     * length that says the field was inside the table.
     */
    struct waking_vector_record
    {
        /**
         * The table's own declared length, and whether it was long enough
         * to contain both vector fields. Everything else below is
         * meaningless unless this is set - see power::facs_minimum_length
         * for why the bound is the end of the extended field and not the
         * end of the thirty two bit one.
         */
        std::uint32_t length{};
        bool usable{};

        /**
         * The thirty two bit real mode vector, and the sixty four bit one.
         *
         * Both, because which one the guest used decides whether this
         * approach works at all: the real mode field is the state
         * apply_start_up already builds, and a guest that set only the
         * extended one is entered through a different protocol and cannot
         * be resumed into by anything here.
         * @{
         */
        std::uint32_t vector{};
        std::uint64_t extended{};
        /**
         * @}
         */
    };

    /**
     * Reads and checks that structure out of the handed-over FACS address.
     *
     * Through read_guest_physical rather than the mapping window directly,
     * which is what gets the bound on how far a guest physical address may
     * reach and keeps this off the window page the diagnostic channel's
     * queues use.
     */
    std::expected<waking_vector_record, zpp::error> read_facs();

    /**
     * Points the platform's resume at this VMM's own trampoline, so that
     * an S3 comes back through it rather than straight into the guest.
     *
     * Called on the way down, after the channel has been flushed and
     * before the write that removes power, and only with
     * power::resume_from_waking_vector on. Saves the guest's own vector in
     * guest_waking_vector, rewrites the trampoline's entry to
     * zpp_resume_from_sleep_main, and writes the trampoline page's address
     * into the FACS with the extended field zeroed.
     *
     * Returns an error and changes nothing when the table or the guest's
     * own vector is not something that can be resumed into. That is a
     * normal outcome and not a failure of the suspend: the machine still
     * sleeps, and still comes back unvirtualized, which is what it did
     * before this existed.
     */
    std::expected<void, zpp::error> arm_resume_from_sleep();

    /**
     * Puts the FACS back the way the guest left it, and the trampoline
     * back the way a start-up IPI needs it.
     *
     * Both halves of what arm_resume_from_sleep did, undone as the first
     * thing the resume does rather than the last. That order is
     * deliberate: if anything after it fails, the guest's own table is
     * already truthful and the next suspend goes down the unmodified path
     * instead of a second time into a trampoline whose hypervisor never
     * finished coming back.
     */
    void disarm_resume_from_sleep();

    /**
     * Puts the per-processor bookkeeping back to what it was before any
     * processor had launched, so that a resume can go through the ordinary
     * launch path rather than a second copy of it.
     *
     * Every array here is indexed by a slot handed out at launch, and
     * after an S3 none of those launches has happened: the platform has
     * reset the processors, the guest will send INIT-SIPI-SIPI again, and
     * the existing adoption path handles that. What it cannot handle is
     * slots that are still marked taken - the counters would keep climbing
     * and max_cpus would be reached after a few suspends.
     */
    void rewind_for_resume();

    /**
     * Moves the guest's entry point from the page apply_start_up put it on
     * to the exact address the firmware waking vector names.
     *
     * apply_start_up takes a start-up IPI vector, which is a page number,
     * so it can only begin a guest at a page boundary. The ACPI real mode
     * waking protocol does not require the vector to be aligned: EDK2's
     * AsmTransferControl far-jumps to (vector >> 4):(vector & 0xf),
     * putting the low four bits in IP. For an aligned vector this writes
     * exactly what apply_start_up already wrote.
     */
    void apply_waking_vector();

    /**
     * Whether this processor's launch is an S3 resume rather than a first
     * boot, which decides whether main re-runs the once-per-boot setup.
     *
     * It must not. Everything that setup builds describes physical memory
     * that S3 preserves and has not moved - the host page table, the host
     * GDT and IDT, the extended page tables, the module's own protection -
     * so building it again would at best repeat work and at worst read an
     * operating system page table that is no longer the guest's.
     *
     * Not per processor, because only the boot processor comes back this
     * way: the application processors are started from the guest's own
     * start-up IPIs afterwards, through the path that already exists.
     */
    bool resuming_from_sleep{};

    /**
     * Whether the exit handler has to leave this processor's guest with
     * VMLAUNCH rather than VMRESUME.
     *
     * Set only by the sleep quiesce, which leaves VMX operation and comes
     * back with a VMCLEAR - and VMCLEAR is the only thing that sets the
     * launch state to clear, which VMLAUNCH requires and VMRESUME refuses
     * (SDM 27.1). Per processor because the quiesce runs on whichever one
     * saw the guest's write, and cleared by the handler that acts on it so
     * the next ordinary exit resumes normally.
     */
    bool relaunch_after_sleep[max_cpus]{};

    /**
     * Handles a write the local APIC page watch saw. Reads the interrupt
     * command out of the page and, if one was issued, puts it through
     * the same decision the x2APIC path uses.
     */
    static void on_local_apic_write(void * context,
                                    std::uint64_t page,
                                    const guest_write * write);

    /**
     * The same decision, made *before* the write reaches the register.
     *
     * Which is the only place it can usefully be made. Writing the low
     * half of the interrupt command register is what sends the
     * interrupt, so a start-up IPI this VMM means to redirect has to be
     * refused here - afterwards there is nothing left to redirect, and
     * the processors it named have already left the wait-for-SIPI state
     * that made them startable.
     *
     * Returns the value to write, or nothing to suppress the write.
     * `on_local_apic_write` above remains for the stepped path, where
     * the guest's own instruction performs the write and no such moment
     * exists.
     */
    static std::optional<std::uint64_t> filter_local_apic_write(
        void * context, std::uint64_t page, const guest_write * write);

    /**
     * The guest has written the storage controller's register page.
     *
     * Armed only while the disk channel is live, and only on the page
     * holding the configuration register - not the doorbell page, which
     * is written constantly. The one thing it looks for is CC.EN going
     * clear, because that is a controller reset and a reset destroys the
     * queue pair the channel writes through.
     */
    /**
     * Flushes everything staged while the controller is still the
     * guest's own working one, before a write to its register page is
     * allowed to land. See page_watch::before_write.
     */
    static void on_controller_register_before_write(void * context,
                                                    std::uint64_t page);

    static void on_controller_register_write(void * context,
                                             std::uint64_t page,
                                             const guest_write * write);

    /**
     * The guest has rung a doorbell while the channel was borrowing its
     * admin queue.
     *
     * Nothing to do: by the time this is reached the writer has already
     * been held at the faulting instruction for as long as the borrow
     * lasted, which is the entire purpose. The write has not taken
     * effect, so whatever it was going to say to the controller it says
     * afterwards instead.
     */
    static void on_doorbell_write(void * context,
                                  std::uint64_t page,
                                  const guest_write * write);

    /**
     * The guest physical page of the memory mapped local APIC, or zero
     * while it is not being watched.
     */
    std::uint64_t watched_apic_page{};

    /**
     * The local APIC page this VMM's own page table maps, read from
     * IA32_APIC_BASE once before any guest ran.
     *
     * `filter_local_apic_write` and `on_local_apic_write` dereference the
     * watched page as a **host virtual address**, so the only page they
     * may be pointed at is one the host page table maps - and it maps
     * exactly one, established at initialization. A guest may relocate
     * its local APIC by writing IA32_APIC_BASE, and this VMM follows the
     * move; without this record it would follow it into a page it cannot
     * address, and the first intercepted write would take a #PF in the
     * exit handler, where there is no recovery point.
     *
     * Recorded rather than re-derived, because the mapping cannot be
     * repeated later: `map_from` walks the loader's OS page table through
     * a callback that stops resolving once a processor switches to this
     * table, which is why every other runtime mapping is established
     * before the switch as well.
     */
    std::uint64_t mapped_apic_page{};

    /**
     * What mode each processor's local APIC was in the last time this VMM
     * looked, which is at its launch and on every write it makes to
     * IA32_APIC_BASE.
     *
     * Kept because the decision it feeds is global and the state is not.
     * See note_apic_mode.
     */
    apic_mode observed_apic_mode[max_cpus]{};

    /**
     * Serialises `note_apic_mode`, which is one processor's decision
     * about state that belongs to all of them.
     *
     * Two pieces of state, and the second is the one that makes a lock
     * necessary rather than tidy: the survey reads `observed_apic_mode`
     * across every processor, and the arming it leads to is a
     * read-modify-write of `msr_bitmap`, which is a **single page** every
     * processor's VMCS points at. Without this, two processors switching
     * to x2APIC at once can leave the interception disarmed while one of
     * them is in x2APIC mode - see note_apic_mode for the interleaving.
     *
     * Not `start_up_lock`: that one is held across a start-up hand-over
     * and this is taken from a WRMSR exit, so sharing them would make an
     * unrelated MSR write wait on a processor being started.
     */
    zpp::spin_lock apic_mode_lock{};

    /**
     * Remove protection for unprotected guest memory.
     * We mainly need to use this memory from guest on UEFI boot.
     */
    void unprotect_guest_memory();

    /**
     * Initialize needed vmx structures.
     */
    void initialize_vmx(std::size_t cpu);

    /**
     * Permit VMXON in IA32_FEATURE_CONTROL, which vmxon requires before it
     * will run at all. Per logical processor.
     */
    std::expected<void, zpp::error> enable_vmx_in_feature_control();

    /**
     * Enter root mode on the current CPU.
     */
    std::expected<void, zpp::error> enter_root_mode(std::size_t cpu);

    /**
     * Emulate an INIT signal, which in VMX non-root operation causes a VM
     * exit instead of resetting the processor.
     *
     * Nothing else can do it: the hardware hands us the signal and takes
     * no further action, so unless the guest state is reset here and the
     * activity state left waiting for a start-up IPI, the INIT is simply
     * lost. That is what used to happen, and it is why an application
     * processor could be started exactly once - the firmware parks its
     * APs in a hlt loop and wakes them with INIT-SIPI-SIPI, so every
     * wake after the first went nowhere and the caller spun forever.
     *
     * Also where this processor chooses which of the two start-up
     * hand-offs its next start-up IPI is to arrive through, and publishes
     * the choice in start_up_handoff for the sender to obey. See
     * start_up_handoff_state.
     */
    void emulate_init_signal(arch::x86_64::context & context);

    /**
     * Emulate a start-up IPI: leave the wait-for-SIPI state and begin
     * execution in real mode at the vector the IPI carries, which is the
     * page number of the entry point.
     */
    void emulate_start_up_ipi(arch::x86_64::context & context,
                              std::uint64_t vector);

    /**
     * Applies the state a processor holds after an INIT followed by a
     * start-up IPI, and leaves it runnable at the vector.
     */
    void apply_start_up(arch::x86_64::context & context,
                        std::uint64_t vector,
                        const char * from = "?",
                        bool first_launch = false);

    /**
     * Handles an intercepted write to the x2APIC interrupt command
     * register, and returns the command to actually issue - or nothing,
     * when the write must be swallowed instead of passed on.
     *
     * This is where a guest starting a processor is caught. An INIT is
     * passed straight through, because it is what leaves the target
     * waiting for a start-up IPI and nothing here improves on it. A
     * start-up IPI is not: its vector is replaced with the hypervisor's
     * own, so the processor begins in code that virtualizes it before
     * running a single instruction the guest wrote.
     *
     * For a processor already under the hypervisor the write may be
     * swallowed instead and the vector handed over directly, but only
     * while the target says it is waiting for one that way - see
     * start_up_handoff_state. Swallowing it on any weaker test loses the
     * IPI outright, because the target may be waiting on hardware and
     * only hardware can wake it there.
     */
    std::optional<std::uint64_t>
    on_interrupt_command(std::uint64_t command);

    /**
     * What starting one processor needed.
     */
    enum class start_up_result
    {
        /**
         * It was started, or is already running, and the guest's own
         * write must not go out for it.
         */
        adopted,

        /**
         * Only a real start-up IPI can move it from where it is, so one
         * has to reach the hardware naming this target.
         */
        needs_hardware,
    };

    /**
     * Starts, or hands a vector to, the one processor with the given local
     * APIC id.
     *
     * Split out of on_interrupt_command so that a broadcast can run every
     * processor through exactly the same path a targeted command does.
     * There is no separate broadcast policy, and that is deliberate: the
     * two used to differ, and the difference was that a broadcast did
     * nothing at all.
     */
    start_up_result start_up_processor(std::uint64_t destination,
                                       std::uint64_t vector);

    /**
     * Resolves a broadcast start-up IPI against the platform's roster and
     * starts each target. Returns whether the guest's own write may be
     * swallowed.
     */
    bool start_up_broadcast(std::uint64_t vector);

    /**
     * Returns the index this VMM tracks the processor with the given local
     * APIC id under, allocating one if this is the first time it has been
     * named. Returns nothing when there is no room left.
     */
    std::optional<std::size_t> processor_slot(std::uint64_t apic_id);

    /**
     * Lays out the memory the loader reserved below one megabyte: the
     * start-up trampoline, and the temporary page table it needs to reach
     * long mode. Done once, on the boot processor.
     */
    void initialize_start_up_memory(std::uint64_t memory);

    /**
     * How far the start-up trampoline got on the processor that used it
     * last, as an ap_start_up_stage. Zero when none was ever prepared.
     */
    std::uint32_t start_up_trampoline_stage() const;

    /**
     * Starts the processor tracked under the given index, which is
     * expected to be waiting for a start-up IPI, and waits for it to come
     * up under the hypervisor. Returns false if it did not.
     */
    bool start_application_processor(std::size_t slot,
                                     std::uint64_t guest_vector);

    /**
     * This processor's local APIC id, read out of CPUID so that it is
     * answerable whatever mode the local APIC is in.
     */
    static std::uint64_t local_apic_id();

    /**
     * Whether the local APIC is in x2APIC mode, and therefore whether its
     * registers are MSRs at all. Reading one when it is not raises #GP.
     */
    static bool x2apic_enabled();

    /**
     * Intercept writes to the x2APIC interrupt command register.
     */
    void intercept_interrupt_command(bool intercept);

    /**
     * Append the exit that is about to be resumed from to this CPU's ring
     * in exit_trace, and count it in exit_reason_counts.
     *
     * The context is taken because the exit reason alone does not say what
     * the guest asked for: an RDMSR exit names no MSR and a VMCALL exit no
     * hypercall, and both are in the guest's registers rather than in the
     * VMCS. Passed by reference from the exit handler, which already holds
     * it, so nothing is read out of the VMCS for this.
     */
    void record_exit(arch::x86_64::vmx::exit_reason reason,
                     const arch::x86_64::context & context);

    /**
     * Ask the processor to deliver a general protection fault to the guest
     * on the next VM entry.
     *
     * This is how the VMM says "that instruction would have faulted on
     * real hardware" - the alternative, resuming as though it had
     * succeeded, hands the guest a result it never computed.
     */
    void inject_general_protection_fault(std::uint64_t error_code = 0);

    /**
     * Handles the exit a VMX instruction the guest executed produced.
     *
     * Returns whether it was handled. False means the caller must deliver
     * an invalid opcode exception and leave RIP where it is, which is
     * both what a processor without VMX does and what happens whenever
     * nested_vmx::enabled is false. True means this set the guest's
     * RFLAGS to VMsucceed, VMfailInvalid or VMfailValid and the caller
     * must advance RIP past the instruction, exactly as it would for any
     * instruction that completed - a VMX instruction that *fails* still
     * retires, and its failure is in the flags.
     *
     * It may also have injected a fault of its own, in which case it
     * returns false as well, since the caller's behaviour is the same: do
     * not advance RIP. Whether the exception was #UD or #GP is decided
     * here.
     */
    bool on_vmx_instruction(arch::x86_64::vmx::exit_reason reason,
                            arch::x86_64::context & context);

    /**
     * Answers a read of one of the MSRs nested VMX owns, or says it does
     * not own this one.
     *
     * The two blocks are IA32_FEATURE_CONTROL and the VMX capability
     * range 480H-491H. Both are inside the range the MSR bitmap covers,
     * so they only exit because the bitmap is told to make them - see
     * intercept_msr.
     * @{
     */
    bool on_nested_vmx_msr_read(std::uint32_t index,
                                arch::x86_64::context & context);

    bool on_nested_vmx_msr_write(std::uint32_t index,
                                 arch::x86_64::context & context);
    /**
     * @}
     */

    /**
     * What this VMM reports for one of the VMX capability MSRs.
     *
     * Every value is derived from the hardware's, narrowed rather than
     * invented, and that is deliberate on a machine whose own capability
     * MSRs are already a filtered subset: the test rig runs QEMU with
     * hv-passthrough, which filters them through enlightened VMCS
     * version 1. A value written from a table of constants would offer a
     * guest hypervisor controls the processor underneath does not permit,
     * and the first VM entry using one would fail.
     */
    std::uint64_t nested_vmx_capability_msr(std::size_t msr);

    /**
     * Arms or releases an MSR in the bitmap, for reads, writes or both.
     *
     * The generalisation of intercept_interrupt_command, which arms one
     * bit of one of the four bitmaps and was the only caller until the
     * capability MSRs needed the read halves too.
     */
    void intercept_msr(std::uint32_t index, bool read, bool write);

    /**
     * The shadow extended page table for a guest hypervisor's EPT pointer,
     * built if it is not already current.
     *
     * Eager: the whole table is constructed by descending the guest
     * hypervisor's own tables and composing every mapping it finds with
     * ours. BACKLOG.md records why that was chosen over filling entries as
     * faults arrive - it removes fault-time composition and the
     * widen-without-invalidate case of SDM 31.4.3.4 entirely, both of
     * which fail as a hang rather than as a bug.
     *
     * Rebuilt when the guest hypervisor points elsewhere, and when this
     * VMM's own tables have changed under it.
     */
    std::expected<std::uint64_t, zpp::error>
    shadow_ept_pointer_for(std::size_t cpu, std::uint64_t eptp12);

    /**
     * Builds the shadow from scratch. Fails only where the pool runs out
     * or the guest hypervisor's root cannot be read.
     */

    /**
     * Forces the next entry to rebuild. Called where something has changed
     * that the shadow was composed from - a guest hypervisor's INVEPT, or
     * a change to this VMM's own tables.
     */
    void discard_shadow_ept(std::size_t cpu);

    /**
     * Discards only the shadow built from the given guest EPT pointer.
     *
     * What the single-context INVEPT type actually asks for. Discarding
     * every shadow instead is permitted - SDM 31.4.3.2 lets a processor
     * invalidate any cached mapping at any time - but it throws away the
     * shadows of the guest hypervisor's *other* guests, which it then has
     * to pay to rebuild the next time it switches to one.
     */
    void discard_shadow_ept_for(std::size_t cpu, std::uint64_t root);

    /**
     * Marks one slot unused and hands its tables back to the shared pool.
     *
     * Freeing the tables is not optional. A slot that keeps them while
     * saying it holds nothing is a slot whose pages no build can reach
     * and no build will reclaim, and four of those exhaust a
     * ninety-six-table pool without a single shadow being live.
     */
    void release_shadow_slot(std::size_t cpu, std::size_t slot);

    /**
     * Installs one mapping into the current shadow, reclaiming pool if it
     * has to.
     *
     * The fault path's counterpart to install_shadow_leaf, and the reason
     * it exists is that running out of tables means something different
     * here. An eager build that ran out could fail cleanly: it had no
     * partial shadow worth entering with, so refusing was the answer. A
     * fault has no such option - the guest cannot be resumed until this
     * mapping is there, and refusing stops the processor.
     *
     * So it reclaims instead, in the order that costs least: the other
     * slots first, since losing a shadow costs the faults to refill it,
     * and this slot last, since resetting it loses everything already
     * filled. Either way the pool is non-empty afterwards and the retry
     * succeeds, which is what makes this terminate.
     */
    std::expected<void, zpp::error>
    fill_shadow_leaf(std::size_t cpu,
                     std::uint64_t guest_physical,
                     const arch::x86_64::vmx::ept_walk_result & guest,
                     std::uint64_t shift);

    /**
     * This VMM's own translation for a host physical address, in the shape
     * `compose_ept` takes.
     *
     * Indexed rather than walked, which initialize_ept's complete identity
     * map is what makes possible - the same reasoning epte_for gives for
     * doing it that way.
     */
    arch::x86_64::vmx::ept_walk_result
    host_ept_lookup(std::uint64_t physical_address);

    /**
     * What a processor walking this processor's *shadow* would find for a
     * second-level guest-physical address.
     *
     * Read-only, and it changes nothing: no table is created, no slot is
     * taken, and a walk that ends nowhere is reported rather than filled
     * in. That is what separates it from `shadow_ept_entry`, which is the
     * write path and makes tables as it descends.
     *
     * **It exists because "the fault was handled" and "the access will now
     * succeed" are different claims, and only the second one matters.**
     * Every path in `on_l2_ept_fault` that installs a leaf returns
     * `handled` and resumes the guest; if the leaf it installed does not
     * permit the access that faulted, the guest faults again at the same
     * RIP on the same address, for ever. That is not a hypothetical - it
     * is the livelock this VMM has been chased by twice, once as
     * qualification 0x1aa on a watched page and once as 0x184 on an
     * instruction fetch - and in both cases the handler believed it had
     * succeeded.
     *
     * So this is the oracle for that claim, and it is deliberately the
     * *generic* walker rather than a second implementation: what it
     * reports is what hardware would report, by construction, because
     * `walk_ept` is what composes the entries in the first place.
     *
     * Also the answer to "what does this processor's shadow actually hold"
     * from a debugger, which nothing else gives - the tables are reached
     * by physical address through `module_physical_to_virtual` and cannot
     * be followed by hand.
     */
    arch::x86_64::vmx::ept_walk_result
    shadow_ept_lookup(std::size_t cpu, std::uint64_t guest_physical);

    /**
     * A second-level guest's physical address, translated through the
     * guest hypervisor's extended page tables into one this VMM can
     * reach.
     *
     * **Everything that reads guest memory is wrong without this while a
     * second-level guest runs, and wrong silently.** The addresses in
     * that guest's page tables, and the addresses its instructions name,
     * are physical *in its own guest hypervisor's address space* - one
     * translation short of anything this VMM can map. Reading them
     * directly lands on whatever host memory happens to share the number:
     * either a page above the physical address width, which faults in
     * root operation where there is no recovery point, or unrelated bytes
     * that decode into a plausible instruction and produce a fabricated
     * answer. The second is the dangerous one and it was reachable.
     *
     * KVM cannot make the mistake because the two are separate objects:
     * `nested_ept_init_mmu_context` splits `arch.mmu` from
     * `arch.walk_mmu`, and every table read on the nested path goes
     * through `kvm_translate_gpa`. This is the same split, expressed as a
     * function rather than a type, because there is one caller shape here
     * and not a class of them.
     *
     * Answers the address unchanged when the processor is not running a
     * second-level guest, or when that guest's hypervisor gave it no
     * extended page tables of its own - in both cases the guest-physical
     * address already is one of this VMM's.
     */
    std::expected<std::uint64_t, zpp::error>
    l2_physical_to_l1(std::size_t cpu, std::uint64_t guest_physical);

    /**
     * Reads guest memory named by a guest-physical address, at whichever
     * level is running.
     *
     * The counterpart of `read_guest_physical` for anything reachable
     * from a second-level exit, and the one every such path should use:
     * it is `read_guest_physical` exactly when that is the right answer,
     * and a translation followed by it otherwise.
     */
    std::expected<void, zpp::error>
    read_guest_memory(std::size_t cpu,
                      std::uint64_t guest_physical,
                      std::span<std::byte> into);

    /**
     * The processor's physical-address width, cached.
     *
     * SDM 31.3.3.1 makes it the boundary for an entry's reserved address
     * bits, so the walker needs it per entry - hence the cache rather than
     * a CPUID each time.
     */
    std::uint64_t physical_address_bits();

    /**
     * Whether execute-only translations are offered to a guest hypervisor,
     * which is IA32_VMX_EPT_VPID_CAP bit 0 (SDM A.10).
     *
     * Not offered. It is the one capability that changes what
     * `ept_permissions::normalised` may leave in an entry, so reporting it
     * without honouring it - or honouring it without reporting it - would
     * put the composition and the capability MSR at odds. Clear on both
     * sides is the pairing that cannot drift.
     */
    static constexpr bool execute_only_translations_offered = false;

    /**
     * One paging-structure page out of this processor's shadow pool,
     * zeroed.
     */
    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    shadow_ept_table(std::size_t cpu);

    /**
     * The shadow entry that maps a second-level guest-physical address at
     * a given page size, creating the tables above it as needed.
     */
    std::expected<arch::x86_64::vmx::epte *, zpp::error>
    shadow_ept_entry(std::size_t cpu,
                     std::uint64_t guest_physical,
                     std::uint64_t shift);

    /**
     * Composes one mapping and writes it into the shadow, splitting to 4
     * KB where the composition does not hold across the whole region.
     * @{
     */
    std::expected<void, zpp::error>
    install_shadow_leaf(std::size_t cpu,
                        std::uint64_t guest_physical,
                        const arch::x86_64::vmx::ept_walk_result & guest,
                        std::uint64_t shift);

    std::expected<void, zpp::error>
    install_shadow_split(std::size_t cpu,
                         std::uint64_t guest_physical,
                         const arch::x86_64::vmx::ept_walk_result & guest);
    /**
     * @}
     */

    /**
     * The three ways a VMX instruction reports its outcome in RFLAGS, from
     * SDM 33.2, "Conventions".
     *
     * vmx_fail is the one the operation sections name most often, and it
     * is not a fourth outcome: it is VMfailValid where there is a current
     * VMCS to record the error number in and VMfailInvalid where there is
     * not, because the error field lives in that VMCS.
     * @{
     */
    void vmx_succeed();
    void vmx_fail_invalid();
    void vmx_fail_valid(std::size_t cpu,
                        nested_vmx::instruction_error error);
    void vmx_fail(std::size_t cpu, nested_vmx::instruction_error error);
    /**
     * @}
     */

    /**
     * The linear address of the memory operand of the VMX instruction that
     * caused this exit.
     *
     * Computed rather than read, because nothing reports it: SDM 30.2.1
     * puts only "the value of the instruction's displacement field" in the
     * exit qualification for these instructions, and SDM 27.9.1's list of
     * the exits that use the guest-linear address field does not include
     * any of them. So base, index, scale and segment come out of the
     * instruction-information field and are put back together here.
     *
     * Fails where the operand is a register, which for an instruction
     * whose only operand is m64 is the #UD SDM 33.3 gives for "(register
     * operand)".
     */
    std::expected<std::uint64_t, zpp::error>
    vmx_operand_linear_address(const arch::x86_64::context & context);

    /**
     * The base of one of the segments the instruction-information field's
     * segment-register encoding can name.
     */
    std::uint64_t guest_segment_base(std::uint64_t segment);

    /**
     * Reads and writes a guest general purpose register by its
     * architectural encoding.
     *
     * RSP is special and has to be: the captured context's rsp holds the
     * address of the context structure, because the exit stub puts it
     * there for restore_context to iretq onto. So the guest's own RSP
     * lives in the VMCS and nowhere else, and reading the context for it
     * would hand the guest a hypervisor stack address - which BACKLOG.md
     * records as one of the three defects keeping the write emulator off.
     * @{
     */
    std::uint64_t guest_register(const arch::x86_64::context & context,
                                 std::uint64_t encoding);

    void set_guest_register(arch::x86_64::context & context,
                            std::uint64_t encoding,
                            std::uint64_t value);
    /**
     * @}
     */

    /**
     * Copies out of, and into, guest memory named by a linear address.
     *
     * A page at a time, because a translation is only good for the page it
     * resolved and an operand may straddle two.
     * @{
     */
    std::expected<void, zpp::error>
    read_guest_linear(std::uint64_t linear, std::span<std::byte> into);

    std::expected<void, zpp::error> write_guest_linear(
        std::uint64_t linear, std::span<const std::byte> from);
    /**
     * @}
     */

    /**
     * The 64-bit value the m64 operand of VMXON, VMPTRLD or VMCLEAR points
     * at, which is the physical address of a region.
     */
    std::expected<std::uint64_t, zpp::error>
    read_guest_vmcs_pointer(const arch::x86_64::context & context);

    /**
     * Whether a region pointer passes the checks every one of those three
     * instructions applies to it.
     */
    bool vmcs_pointer_valid(std::uint64_t pointer);

    /**
     * Writes the cached shadow VMCS back to the guest's own region.
     *
     * Called by anything that stops a VMCS being current, which is what
     * makes a VMCS moved between processors keep its contents.
     */
    void flush_guest_vmcs12(std::size_t cpu);

    /**
     * The VMX instructions a guest hypervisor executes, one each.
     *
     * Each returns whether the instruction was completed - true meaning
     * RFLAGS now says VMsucceed, VMfailInvalid or VMfailValid and the
     * caller must advance RIP, false meaning a fault was delivered or is
     * to be delivered by the caller and RIP stays put.
     * @{
     */
    bool on_guest_vmxon(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_vmxoff(std::size_t cpu);
    bool on_guest_vmclear(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmptrld(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmptrst(std::size_t cpu,
                          arch::x86_64::context & context);
    bool on_guest_vmread(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_vmwrite(std::size_t cpu,
                          arch::x86_64::context & context);
    bool
    on_guest_vmlaunch(std::size_t cpu,
                      arch::x86_64::vmx::exit_reason::basic_reason reason,
                      arch::x86_64::context & context);
    bool on_guest_invept(std::size_t cpu, arch::x86_64::context & context);
    bool on_guest_invvpid(std::size_t cpu,
                          arch::x86_64::context & context);
    /**
     * @}
     */

    /**
     * Builds the VMCS a second-level guest runs under and makes it
     * current.
     *
     * Everything the architecture lets the guest hypervisor choose comes
     * from its own VMCS; everything this VMM cannot give up is unioned on
     * top; the host-state area is this VMM's, copied field for field out
     * of the VMCS that was current, because the VM exit comes here and not
     * to the guest hypervisor.
     *
     * On failure nothing has been switched and the caller may still answer
     * the guest hypervisor with VMfail.
     */
    std::expected<void, zpp::error> build_vmcs02(std::size_t cpu);

    /**
     * Refreshes this processor's merged MSR and I/O bitmaps from the guest
     * hypervisor's, when it has named ones this VMM has not read yet.
     */
    std::expected<void, zpp::error> merge_nested_bitmaps(std::size_t cpu);

    /**
     * The VM-entry and VM-exit MSR areas a guest hypervisor named:
     * checked, loaded and stored in software.
     *
     * All three are checked at VM entry rather than each where a processor
     * would look at it, because a failure in either exit area is a VMX
     * abort and there is no shutdown to perform on one guest's behalf.
     * @{
     */
    std::expected<void, zpp::error> check_nested_msr_area(
        std::uint64_t address, std::uint64_t count, bool loading);

    std::expected<void, zpp::error> load_nested_msrs(std::size_t cpu,
                                                     std::uint64_t address,
                                                     std::uint64_t count);

    std::expected<void, zpp::error>
    store_nested_msrs(std::uint64_t address, std::uint64_t count);
    /**
     * @}
     */

    /**
     * What the exit handler does next with an exit the second-level guest
     * took.
     */
    enum class l2_exit_outcome
    {
        /**
         * Given to the guest hypervisor. Its own VMCS is current again,
         * its guest state is where its VM exit would have left it, and
         * nothing else in the handler applies.
         */
        reflected,

        /**
         * Answered here, completely. The second-level guest is resumed
         * without the handler's own cases running - either because the
         * answer is one only this path knows, as it is for an extended
         * page-table fault composed across two levels, or because there
         * was nothing to do.
         */
        handled,

        /**
         * Answered here, by the handler's ordinary cases, with the
         * second-level VMCS current. The exits this VMM intercepts for
         * its own reasons take this path so that one piece of code
         * answers them whichever guest asked.
         */
        deferred,
    };

    /**
     * What becomes of a VM entry into a second-level guest once vmcs02 is
     * built, which is not always "it happens".
     */
    enum class l2_entry_outcome
    {
        /**
         * vmcs02 is current and describes a runnable guest. The caller
         * enters it.
         */
        entered,

        /**
         * The entry did not happen and the guest hypervisor has already
         * been given the VM exit that says so - either a VM-entry failure
         * for a guest-state area this VMM cannot honour, or the start-up
         * IPI its parked virtual processor was waiting for. Its own VMCS
         * is current again.
         */
        reflected,

        /**
         * The entry did not happen and the guest hypervisor has not been
         * told anything. Its own VMCS is current with RIP still on the
         * VMLAUNCH or VMRESUME, so it executes the instruction again and
         * the decision is taken afresh. This is how a second-level guest
         * that is waiting for a start-up IPI is held without the physical
         * processor sitting in an activity state nothing can end.
         */
        retry,
    };

    /**
     * Decides whether the second-level guest vmcs12 describes may be
     * entered at all, and what to do instead when it may not.
     *
     * Called with vmcs02 current and fully built, which is where SDM 29.3
     * puts the checks on the guest-state area: after the VM-execution
     * controls and the host-state area have passed.
     */
    l2_entry_outcome enter_or_park_l2(std::size_t cpu);

    /**
     * Wait, in VMX root operation, for a start-up IPI aimed at a
     * second-level guest that vmcs12 says is in the wait-for-SIPI activity
     * state. Returns the vector once one is handed over, and nothing if
     * the wait gave up.
     */
    std::optional<std::uint64_t> wait_for_l2_start_up_ipi(std::size_t cpu);

    /**
     * Decides what becomes of an exit the second-level guest took, and
     * either gives it to the guest hypervisor, answers it, or leaves it to
     * the exit handler's own cases.
     */
    l2_exit_outcome on_l2_exit(std::size_t cpu,
                               arch::x86_64::vmx::exit_reason reason,
                               arch::x86_64::context & context,
                               bool & advance_rip);

    /**
     * The extended page-table fault half of that decision: which of the
     * two levels of tables refused the access, and therefore whose fault
     * it is.
     */
    l2_exit_outcome on_l2_ept_fault(std::size_t cpu,
                                    arch::x86_64::vmx::exit_reason reason,
                                    arch::x86_64::context & context,
                                    bool & advance_rip);

    /**
     * Whether this VMM's own MSR bitmap or I/O bitmap asked for the
     * access, which is what makes an exit this VMM's rather than the guest
     * hypervisor's.
     * @{
     */
    bool own_msr_intercepted(std::uint32_t index, bool write) const;
    bool own_io_port_intercepted(std::uint16_t port) const;
    /**
     * @}
     */

    /**
     * Whether this VMM must take an exit for itself whatever the guest
     * hypervisor asked for.
     */
    bool l0_wants_l2_exit(std::size_t cpu,
                          arch::x86_64::vmx::exit_reason reason,
                          const arch::x86_64::context & context);

    /**
     * Whether the guest hypervisor's own controls say it wants the exit.
     */
    bool l1_wants_l2_exit(std::size_t cpu,
                          arch::x86_64::vmx::exit_reason reason,
                          const arch::x86_64::context & context);

    /**
     * Hands one exit to the guest hypervisor: its guest state saved back
     * into its VMCS, the exit-information fields written, its own host
     * state loaded, and its VMCS made current again.
     */
    void reflect_l2_exit(std::size_t cpu,
                         arch::x86_64::vmx::exit_reason reason,
                         std::uint64_t qualification);

    /**
     * The guest-state half of that: what the second-level guest changed
     * while it ran, copied back into the guest hypervisor's VMCS.
     */
    void save_l2_state(std::size_t cpu);

    /**
     * The other half: the guest hypervisor's own host state, loaded into
     * the guest-state area of the VMCS that runs it.
     */
    void load_l1_host_state(std::size_t cpu);

    /** One VMWRITE of vmcs01's guest-state area, recorded in call order
     * so `l1_host_changed` can be read without a field lookup. */
    void host_write(std::size_t cpu,
                    arch::x86_64::vmx::vmcs::field which,
                    std::uint64_t value);

    /**
     * Whatever a transition between the two levels has to invalidate.
     */
    void nested_transition_flush();

    /**
     * Ask the processor to deliver an invalid opcode exception to the
     * guest on the next VM entry.
     *
     * The answer for an instruction this VMM intercepts but does not
     * implement, where the guest has already been told the feature is
     * absent. SDM 28.1.1 puts invalid-opcode exceptions *above* VM exits
     * in priority, so a guest whose own view of the machine says the
     * instruction is unrecognised would have taken a #UD on real
     * hardware and never reached a hypervisor at all. Delivering one is
     * therefore not an approximation, it is the same answer bare metal
     * gives.
     *
     * No error code, since #UD pushes none.
     */
    void inject_invalid_opcode_exception();

    /**
     * Record an exit nothing here knows how to handle, and stop this CPU.
     *
     * Does not return. Resuming from an unhandled exit is not a neutral
     * act: the resume path advances RIP by the length of the instruction
     * that caused the exit, so the guest silently skips it and carries on
     * with whatever the instruction was supposed to have produced left
     * undone. For an EPT violation there is no instruction to skip at all.
     * Either way the guest is quietly corrupted, and the eventual failure
     * shows up somewhere unrelated - so this stops instead, where the
     * cause is still visible.
     */
    [[noreturn]] void
    on_unhandled_exit(arch::x86_64::vmx::exit_reason reason);

    /**
     * Record a VM entry failure into vm_entry_failure and stop this CPU.
     *
     * Does not return, and deliberately so. The guest never ran, so there
     * is nothing to resume and no RIP to advance; resuming would re-run
     * the identical entry and fail identically, which is the silent
     * infinite loop this replaces.
     */
    [[noreturn]] void
    on_vm_entry_failure(arch::x86_64::vmx::exit_reason reason);

    /**
     * Everything the VM exit handler does on the way back to whichever
     * guest it came from, and the resume itself.
     *
     * Does not return. Split out of the handler because there are two ways
     * in now: the ordinary end of the exit-reason switch, and a
     * second-level exit reflected into the guest hypervisor, which has
     * nothing left to do below and must not fall through the switch.
     */
    /**
     * Whether an event may be delivered by the next VM entry, given the
     * guest activity state and interruptibility state that entry will
     * carry. SDM 29.3.1.5.
     *
     * Answering false is not an error. It means the guest is in a state
     * the event may not be delivered into *yet* - a processor waiting
     * for its start-up IPI, or one inside an STI shadow - and the caller
     * holds the event for a later entry rather than dropping it.
     */
    bool event_allowed_on_entry(std::uint64_t event) const;

    /**
     * Record an external interrupt this VMM has taken out of the
     * interrupt controller, so that a later entry can put it into the
     * guest.
     *
     * Only ever called with `ZPP_VIRTUALIZE_APIC` on. Compiled
     * unconditionally so that `tests/resume_guest` can drive it - the
     * call site is what the switch removes, and with the switch off the
     * linker drops the body with `--gc-sections`.
     *
     * A vector reaching here has already been consumed at the
     * controller: "acknowledge interrupt on exit" is what put it in the
     * exit-interruption field, and SDM 30.2 says the interrupt is no
     * longer pending once that happens. So there is nowhere to give it
     * back to and losing it here loses it for ever - which is why the
     * queue is a whole 256-bit bitmap rather than the single slot this
     * used to be.
     */
    void queue_external_interrupt(std::size_t cpu, std::uint64_t vector);

    /**
     * Put the highest-priority queued external interrupt into the guest
     * through the entry-interruption field, or arm interrupt-window
     * exiting if the guest cannot take one yet. Returns whether one was
     * injected.
     *
     * Highest vector first, because that is the order the local APIC
     * itself would have delivered them in: the interrupt priority of a
     * vector is `vector / 16` and, within a class, the higher vector
     * wins (SDM 12.8.4).
     *
     * Same caveat as `queue_external_interrupt` about being compiled
     * with the switch off.
     */
    bool deliver_pending_external_interrupt(std::size_t cpu);

    /**
     * `cpuid` is this processor's index, passed rather than re-derived.
     * Both callers are inside `on_vm_exit`, which has it as a parameter,
     * and the seven places in here that used to say `vmcs.vpid()` were
     * seven exits to the layer below apiece - see the comment at the top
     * of `on_vm_exit`.
     */
    [[noreturn]] void resume_guest(std::uint64_t cpuid,
                                   arch::x86_64::context & context,
                                   arch::x86_64::vmx::exit_reason reason,
                                   bool advance_rip);

    /**
     * Setup the VM control structure according to the given guest context,
     * and configured host fields.
     */
    void setup_vmcs(std::size_t cpu,
                    arch::x86_64::context & guest_context);

    /**
     * Configure the RIP and RSP fields of the VM control structure and
     * launch the VM.
     */
    template <typename VmmCode>
    void vm_launch(arch::x86_64::context & guest_context,
                   VmmCode && vmm_code);

    /**
     * The main function of the hypervisor that will launch it
     * on the current CPU. This function is already called with
     * the hypervisor reserved stack.
     */
    std::expected<void, zpp::error>
    main(arch::x86_64::context & caller_context);

    /**
     * The VM exit dispatch: the switch on the basic exit reason, and the
     * whole of what this VMM tells its guest. Defined in
     * hypervisor/src/hypervisor/exit_dispatch.cpp.
     *
     * `cpuid` is the processor index `main` was launched with. It keeps
     * that name because it was a lambda inside `main` until the move and
     * this was the one thing it captured - see that file for why the name
     * was not improved along the way.
     *
     * Called once per exit from the lambda `main` hands to `vm_launch`,
     * and it always ends by resuming the guest.
     */
    void on_vm_exit(std::uint64_t cpuid, arch::x86_64::context & context);

    /**
     * Launches the main function of the hypervisor, updates the rax
     * context value to the returned value from the main function, and
     * restores the context to the caller context..
     */
    static void launch_on_cpu_private_stack(
        hypervisor & hypervisor, arch::x86_64::context & caller_context);

    /**
     * The current index of an available stack.
     */
    std::size_t available_stack_index{};

    /**
     * Stack storage for the hypervisor, each CPU has its own stack.
     */
    alignas(page_size) std::uint8_t stack[max_cpus][512 * 1024]{};

    /**
     * Convert physical address to virtual address for OS page tables,
     * must be used only during initialization phase.
     */
    std::uint64_t (*physical_to_virtual)(std::uint64_t){};

    /**
     * The OS page tables object, translating virtual addresses to
     * physical addresses of the OS.
     * Must be used only during initialization phase.
     */
    arch::x86_64::os_page_table os_page_table{};

    /**
     * The host page tables object, which is assigned to the CPU in
     * hypervisor mode. Also allows translating virtual addresses
     * to physical addresses of the hypervisor module.
     */
    arch::x86_64::page_table host_page_table{};

    /**
     * The next virtual processor id that will be assigned
     * to the VM control structures, starts at 1, and incremented
     * on every launch of a VM on a specific processor.
     */
    std::size_t next_virtual_processor = 1;

    /**
     * The base of the current module.
     */
    const unsigned char * module_base{};

    /**
     * The size of the current module in bytes.
     */
    std::size_t module_size{};

    /**
     * The base the loader handed over, or null where it did not.
     *
     * Preferred over searching for it - see initialize_module_region.
     */
    const void * handed_over_module_base{};

    /**
     * The guest CR0 register.
     */
    std::uint64_t guest_cr0{};

    /**
     * The guest CR3 register.
     */
    std::uint64_t guest_cr3{};

    /**
     * The guest CR4 register.
     */
    std::uint64_t guest_cr4{};

    /**
     * The guest DR7 register.
     */
    std::uint64_t guest_dr7{};

    /**
     * The host CR0 register.
     *
     * What this processor was found running with, adjusted to satisfy the
     * VMX fixed bits. It is also what the guest's own CR0 field is loaded
     * from, which is why write protection is not in here - see
     * host_control_register_0.
     */
    std::uint64_t host_cr0{};

    /**
     * The CR0 this VMM's own code runs with: host_cr0 and write
     * protection.
     *
     * Separate from host_cr0, and the separation is the point. The host
     * page table maps this module's text and read-only data without the
     * write flag, and SDM 5.6.1 (.references/sdm.txt:157661) only
     * consults that flag for a supervisor write when CR0.WP is set -
     * everything here runs at privilege zero, so with WP clear the
     * read-only mappings would deny nothing at all.
     *
     * It is not simply added to host_cr0 because host_cr0 is what the
     * *guest* CR0 field is loaded from, and forcing write protection on a
     * guest whose operating system had it clear changes what that guest's
     * own supervisor writes may do. This VMM owns its own paging and has
     * no business owning the guest's.
     *
     * Loaded in three places, which are the three ways this VMM's code
     * comes to be running: entering root mode before the first launch,
     * the host CR0 field a VM exit loads, and the trampoline a processor
     * this VMM started climbs through.
     */
    constexpr std::uint64_t host_control_register_0() const
    {
        return this->host_cr0 | arch::x86_64::cr0_bits::write_protect;
    }

    /**
     * The host CR3 register.
     */
    std::uint64_t host_cr3{};

    /**
     * The host CR4 register.
     */
    std::uint64_t host_cr4{};

    /**
     * The IDTR register.
     */
    arch::x86_64::idtr idtr{};

    /**
     * The IDTR value describing the host IDT.
     */
    arch::x86_64::idtr host_idtr{};

    /**
     * The GDTR register.
     */
    arch::x86_64::gdtr gdtr{};

    /**
     * The LDTR register of the guest.
     */
    std::uint16_t guest_ldtr{};

    /**
     * The TR register of the guest.
     */
    std::uint16_t guest_tr{};

    /**
     * The TR register of the OS.
     */
    std::uint16_t os_tr{};

    /**
     * The host code segment selector.
     */
    std::uint16_t host_cs{};

    /**
     * The host task segment selector.
     */
    std::uint16_t host_tr{};

    /**
     * Mapping of physical address to virtual address of the module.
     */
    small_map<std::uint64_t, std::uint64_t, max_module_size / page_size>
        module_physical_to_virtual{};

    /**
     * The GDT that will be used by the host VMM.
     */
    alignas(page_size) std::uint64_t host_gdt[0x2000]{};

    /**
     * The IDT that will be used by the host VMM. Two quadwords per gate.
     */
    alignas(page_size) std::uint64_t host_idt[0x2000]{};

    /**
     * Assert the host IDT can hold a gate for every vector.
     */
    static_assert(sizeof(host_idt) >=
                  arch::x86_64::number_of_exception_vectors * 2 *
                      sizeof(std::uint64_t));

    /**
     * The exception the host IDT caught last, as the entry stub found it.
     * Kept for a debugger to read: the launch fails with a
     * host_exception error, which says what happened but not where.
     */
    arch::x86_64::exception_frame host_exception{};

    /**
     * CR2 as of that exception, which is the address that faulted when the
     * vector is a page fault.
     */
    std::uint64_t host_exception_cr2{};

    /**
     * How many non-maskable interrupts this VMM has taken in root mode,
     * and where the last one interrupted.
     *
     * Separate from host_exception, and deliberately: an NMI is not a
     * fault, and letting one overwrite the record of a real one would
     * destroy the evidence for the sake of an event that is usually
     * routine. It also has to be visible, because a processor that is
     * taking NMIs and resuming leaves no other trace - the halt that used
     * to happen was at least loud.
     *
     * Non-zero on a machine nobody is probing means the hardware sends
     * them: thermal, watchdog or performance monitoring. Every one of
     * those used to halt a processor for ever.
     * @{
     */
    volatile std::uint64_t host_nmi_count{};
    volatile std::uint64_t host_nmi_rip{};

    /**
     * Writes to the local APIC page this VMM could not decode.
     *
     * The interrupt command register's handler needs to know *which*
     * register was written, which it takes from the decoded store. A
     * write that was stepped instead leaves that unknown, and guessing
     * would mean sending an interrupt nobody asked for - so it is counted
     * and ignored. Non-zero means the decoder has a gap on a page that
     * matters, which is a bug of its own rather than a tolerable loss.
     */
    volatile std::uint64_t apic_writes_undecoded{};

    /**
     * Which branch of the interrupt-command handler a start-up took.
     *
     * The handler logs every one of these, but the log is a list of heap
     * strings and needs a debugger that can walk it; these are one
     * monitor read each, which is what is available while a guest is
     * running. They exist because the state after the fact could not
     * distinguish them: with `number_of_known_processors` still 1 and no
     * processor virtualized, every early refusal looks identical from
     * outside.
     */
    volatile std::uint64_t ipi_init_seen{};

    volatile std::uint64_t ipi_start_up_seen{};

    volatile std::uint64_t ipi_refused_shorthand{};

    volatile std::uint64_t ipi_refused_logical{};

    volatile std::uint64_t ipi_last_command{};

    /**
     * Which register each watched-page access actually named.
     *
     * The exit ring shows the write happening and being stepped, and the
     * interrupt-command handler shows it never arriving; between those
     * two facts is the question of *which* register was written, and
     * nothing recorded it. Inferring it from the APIC mode was wrong
     * once already, so it is measured here instead.
     *
     * Frozen when full, because the first accesses are the ones that
     * decide whether a processor starts.
     */
    struct watched_access
    {
        std::uint64_t page{};
        std::uint64_t offset{};
    };

    static constexpr std::size_t watched_access_capacity = 24;

    watched_access watched_accesses[watched_access_capacity]{};

    volatile std::uint64_t watched_access_count{};

    /**
     * Each instruction this VMM carried out on the guest's behalf, newest
     * last, wrapping.
     *
     * Wrapping rather than freezing, which is the opposite of the refused
     * ring beside it, and deliberately: a refusal is interesting the first
     * time it happens, whereas an emulation is interesting when it is the
     * *last* thing before the guest died. The failure this exists for is a
     * triple fault after a fixed number of emulations, and what is wanted
     * is the few immediately before it.
     * @{
     */
    static constexpr std::size_t emulated_trace_capacity = 32;

    struct emulated_trace_entry
    {
        std::uint8_t code[8]{};
        std::uint64_t page{};
        std::uint64_t old_value{};
        std::uint64_t new_value{};
        std::uint32_t what{};
        std::uint32_t how{};
        std::uint32_t size{};
        std::uint32_t wrote{};
    };

    emulated_trace_entry emulated_trace[emulated_trace_capacity]{};
    volatile std::uint64_t emulated_trace_count{};

    /**
     * The bytes of the instruction most recently fetched for decoding, so
     * the trace can name it. Written by the fetch and read by the record.
     */
    std::uint8_t last_fetched_code[8]{};
    /**
     * @}
     */

    /**
     * The opening bytes of instructions the store decoder refused, and how
     * many it has refused.
     *
     * The decoder covers the MOV forms that store a register or an
     * immediate and refuses everything else, which is safe - the caller
     * falls back to letting the guest's own instruction run - but it is
     * not free: the interrupt command handler cannot act on a write it
     * could not decode, so a start-up IPI in a refused form is a processor
     * that never joins. Forty-five per cent of one guest's writes to the
     * local APIC page were refused.
     *
     * Naming the forms is the whole point, and a count cannot do it. This
     * keeps the first bytes so the opcode and its prefixes can be read
     * off, and freezes when full because the interesting ones arrive
     * during start-up.
     *
     * Two kinds of entry share the ring, told apart by `why`. A
     * `not_decoded` entry is a coverage gap and is expected; an
     * `impossible_operation` entry is a fault in something else entirely -
     * see impossible_decodes below. They share the ring rather than having
     * one each because the second must read zero, and a ring that is empty
     * is a worse place to keep a thing that must be empty than one that
     * has traffic in it and can be seen to be working.
     * @{
     */
    static constexpr std::size_t refused_instruction_capacity = 64;
    static constexpr std::size_t refused_instruction_bytes = 8;

    struct refused_instruction
    {
        std::uint8_t code[refused_instruction_bytes]{};
        std::uint64_t page{};
        refusal why{};
    };

    refused_instruction
        refused_instructions[refused_instruction_capacity]{};
    volatile std::uint64_t refused_instruction_count{};
    /**
     * @}
     */

    /**
     * Decodes that cannot describe the instruction that faulted.
     *
     * `watch_guest_page_writes` clears the write permission and nothing
     * else, so reads of a watched page are still permitted and a pure read
     * of one cannot fault. Every violation that reaches the emulation path
     * is therefore a write - the caller has already excluded a
     * paging-structure access with bit 8 of the exit qualification - and
     * an instruction that does not write memory cannot have caused one.
     *
     * So a `load` or an `examine` decoded at the instruction pointer of a
     * write-caused violation means the decoder was handed bytes that are
     * **not** the faulting instruction. Two things could do that: a wrong
     * answer from `translate_guest_linear`, or a mapping window pointed at
     * the wrong page. Both would be silent on the write paths and both
     * would produce the wrong value at the wrong address, and advance the
     * guest's instruction pointer by a length measured from unrelated
     * bytes.
     *
     * This must read zero. It is not a coverage measurement like the count
     * above it - a non-zero value is a much larger bug than a decoder gap,
     * and it says so where nothing else would: a fabricated value written
     * to a device register looks exactly like a guest that wrote it.
     */
    volatile std::uint64_t impossible_decodes{};

    /**
     * Synthetic hypervisor MSR accesses forwarded to whatever this VMM
     * runs under, rather than faulted.
     *
     * Non-zero says the guest took up the interface it was offered, which
     * is the point of offering it. Zero with the interface presented means
     * the guest looked and did not use it, and the two are worth telling
     * apart.
     */
    volatile std::uint64_t synthetic_msr_accesses{};

    /**
     * The guest operating system identity, as the guest declared it.
     *
     * It exists to gate the hypercall page: a guest that has not
     * identified itself has no business enabling one, and the reference
     * implementation refuses the enable in that order. Zero means it has
     * not, and writing zero back retires the hypercall page with it.
     */
    volatile std::uint64_t hyperv_guest_os_id{};

    /**
     * The hypercall page's location and enable, in the layout the guest
     * writes: bit 0 enables it, and bits 63:12 are the guest page frame
     * the instructions are to be written into.
     */
    volatile std::uint64_t hyperv_hypercall{};

    /**
     * Hypercall page enables that could not be carried out, split by why.
     *
     * Neither is answered with a fault. **A general protection fault on
     * this MSR is what killed the guest**, so refusing the write the way
     * an absent MSR is refused reintroduces exactly the failure the MSR
     * was implemented to remove - measured as a second wedge, with the
     * index faulted once and three other accesses answered. The write is
     * accepted and the page is simply not installed, which is what the
     * reference does for the first of these.
     *
     * `early` counts an enable that arrived before the guest identified
     * itself; `unwritable` counts a page this VMM could not store into.
     * They are separate because they mean different things: the first is
     * the guest's ordering and is legitimate, the second is a failure
     * here.
     */
    volatile std::uint64_t hypercall_page_early{};

    volatile std::uint64_t hypercall_page_unwritable{};

    /**
     * How often the guest read the VMX capability MSRs, and the feature
     * control MSR, whoever answered them.
     *
     * These separate two states that look identical from
     * `guest_vmxon_count` being zero: a guest that never considered
     * virtualization at all, and one that read what this VMM offers and
     * decided against it. The second is a question about which capability
     * bit is missing; the first is a question about the guest's own
     * policy, and no amount of work on the capability set would move it.
     *
     * Counted at the exit rather than inside a handler, so that an MSR
     * answered by the nested path, by the pass-through path, or by a
     * fault is counted the same way.
     */
    volatile std::uint64_t vmx_capability_reads{};

    volatile std::uint64_t feature_control_reads{};

    /**
     * The first MSR indices this VMM answered with a general protection
     * fault, oldest first, frozen once full.
     *
     * The log already records every one of them, but the log is a list of
     * heap strings and reading it needs a debugger that can walk it. This
     * needs two reads through the emulator's monitor, which is what is
     * available while a guest is wedged.
     *
     * The first is what matters rather than the last. A guest that faults
     * on an MSR usually dies on that one, and everything after it is the
     * wreckage - so a ring that kept the newest would record the
     * consequences and drop the cause.
     */
    static constexpr std::size_t faulted_msr_capacity = 16;

    volatile std::uint64_t faulted_msrs[faulted_msr_capacity]{};

    volatile std::uint64_t faulted_msr_count{};

    /**
     * Which CPUID leaves the guest asked for, in order, and what it was
     * answered in ECX.
     *
     * The instrument for "what does the guest check before it decides".
     * Every other measurement here says what this VMM did; this says what
     * the guest wanted, which is the only thing that can name a missing
     * answer rather than guess at one.
     *
     * Frozen when full rather than wrapping, for the same reason the admin
     * observation ring is: the leaves that decide anything are asked once
     * during start-up, and steady state re-asks a handful for ever.
     * @{
     */
    static constexpr std::size_t cpuid_trace_capacity = 512;

    struct cpuid_trace_entry
    {
        std::uint32_t leaf{};
        std::uint32_t subleaf{};
        std::uint32_t ecx_answered{};
        std::uint32_t eax_answered{};
    };

    cpuid_trace_entry cpuid_trace[cpuid_trace_capacity]{};
    volatile std::uint64_t cpuid_trace_count{};

    /**
     * How many of those were in the range reserved for a hypervisor, which
     * is the subset that says whether the guest went looking for one at
     * all.
     */
    volatile std::uint64_t cpuid_hypervisor_leaves_asked{};
    /**
     * @}
     */

    /**
     * Whether the guest ever looked at VMX, and what it was told.
     *
     * These exist to separate two outcomes that are identical from
     * outside and have opposite causes. A guest that never executes
     * VMXON has either not looked - virtualization based security off,
     * or the VMX bit not reported - or looked and declined, which means
     * a capability it requires is missing from what this VMM advertises.
     *
     * `cpuid_leaf_1_ecx_reported` is what leaf 1 actually answered, so
     * bit 5 says whether the guest was told VMX exists at all.
     * @{
     */
    volatile std::uint64_t nested_capability_reads{};
    volatile std::uint64_t nested_capability_last_msr{};
    volatile std::uint64_t cpuid_leaf_1_ecx_reported{};

    /**
     * The control fields a guest hypervisor actually wrote into its own
     * VMCS, captured at the first entry that uses them.
     *
     * This is the measurement that names what a guest hypervisor
     * *requires*, rather than inferring it from which withholding makes
     * it decline. Bisecting the advertised set costs a reboot per
     * variable and cannot separate controls that the architecture
     * couples; reading what it set answers directly.
     * @{
     */
    volatile std::uint64_t vmcs12_pin_controls{};
    volatile std::uint64_t vmcs12_primary_controls{};
    volatile std::uint64_t vmcs12_secondary_controls{};

    /**
     * Every control either level ever asked for, and every secondary
     * control this VMM ever wrote, OR-accumulated across every entry.
     *
     * The sampled records above take the *first* entry, which is too
     * early: `vmcs12_secondary_controls` reads zero while the primary
     * controls activate the secondary ones and the shadow extended page
     * tables are in use, so the guest hypervisor had simply not written
     * the field yet. An empty record whose emptiness has nothing to do
     * with the question is worse than none.
     *
     * `asked` against `written` is the comparison worth having: a bit set
     * in the first and clear in the second is a capability the guest
     * hypervisor requested and did not get, silently. Virtual-interrupt
     * delivery, APIC-register virtualization and virtualize-APIC-accesses
     * are the three being looked for, since a guest hypervisor delivering
     * interrupts through a virtual APIC page this VMM does not maintain
     * would stall precisely the way the rig stalls.
     * @{
     */
    volatile std::uint64_t vmcs12_secondary_asked{};
    volatile std::uint64_t vmcs02_secondary_written{};
    volatile std::uint64_t vmcs12_primary_asked{};
    volatile std::uint64_t vmcs12_pin_asked{};
    volatile std::uint64_t vmcs12_exit_asked{};
    volatile std::uint64_t vmcs02_exit_written{};
    /**
     * @}
     */

    /**
     * Every external interrupt reflected to a guest hypervisor, counted
     * by the vector the processor reported.
     *
     * Empty until "acknowledge interrupt on exit" reaches vmcs02, which
     * is why the two arrived together: SDM 27.9.2 provides the vector
     * only while that control is 1, so before it there was nothing to
     * count and the field read as invalid on every one of them.
     *
     * What it discriminates is which interrupts a guest hypervisor is
     * being handed. A boot in which every vector is a timer or an
     * inter-processor interrupt and none belongs to a device says the
     * device interrupts are not arriving at all, which is a different
     * fault from their arriving and being mis-dispatched, and the two
     * were indistinguishable while the vector was unavailable.
     */
    volatile std::uint32_t l2_external_vector[max_cpus][256]{};

    /**
     * Every event a guest hypervisor asked VM entry to inject into its
     * guest, counted by vector.
     *
     * The counterpart of `l2_external_vector`, at the other end of the
     * same question. That one says what arrives at the guest hypervisor;
     * this says what it hands on. Together they separate two failures
     * that look identical from outside: an interrupt the guest
     * hypervisor never delivers, and one it delivers that never lands.
     *
     * The vector to look for is `0xd1`. The root partition configures
     * `SINT3` on it and arms a periodic synthetic timer against it,
     * Hyper-V writes `HvMessageTimerExpired` into the message page, and
     * the end-of-message register is never written on any processor -
     * so the message is never read, and the whole machine stops behind
     * it. Whether `0xd1` appears here at all decides which side of the
     * hand-over is at fault, and nothing else measured so far can.
     */
    volatile std::uint32_t l2_injected_vector[max_cpus][256]{};

    /**
     * The same census, taken at the **actual VM entry** rather than
     * where the event was copied out of vmcs12.
     *
     * `l2_injected_vector` counts what `build_vmcs02` wrote into
     * vmcs02 when the guest hypervisor asked. This counts what the
     * field still held at the instant the processor was entered - the
     * last point before `restore_context`, with vmcs02 current.
     *
     * The two exist because they disagree in exactly one way and it is
     * the way that matters. Injection through the VM-entry
     * interruption-information field is unconditional: SDM 27.6
     * delivers the event regardless of RFLAGS.IF, the task priority
     * register or interrupt shadowing. So a successful entry carrying a
     * valid injection *must* vector. The rig shows vector `0xd1`
     * injected 52,799 times, no entry failure, and a second-level guest
     * that never vectored once - which leaves only one possibility
     * worth measuring: the field does not still hold it when the entry
     * happens.
     *
     * If this trails `l2_injected_vector`, the event is being lost
     * between the copy and the entry and the difference says how often.
     * If the two agree, the loss is not here and the landing records
     * are being misread.
     * @{
     */
    /**
     * What the second-level guest is handed back as it resumes, and
     * where it resumes to.
     *
     * The guest is stuck inside its clock interrupt handler, in
     * `KeQueryPerformanceCounter` reaching `HvlpGetRegister64` - the
     * MSR form of the reference counter, so every query is an exit
     * reflected to the guest hypervisor and answered by it. What has
     * never been observed is the **value** that comes back.
     *
     * Recorded here because here is where it becomes visible: the
     * guest hypervisor has already answered, the instruction pointer
     * has been advanced past the RDMSR, and the pair the guest will
     * read is sitting in RAX and RDX waiting to be restored.
     *
     * `reference_count_backwards` is the reason for all of it. A
     * routine that waits for a counter to reach a deadline never
     * finishes if the counter steps backwards, and a counter composed
     * across a reflect-and-resume path is exactly where that can
     * happen. One step back is a bug; none says look elsewhere.
     * @{
     */
    static constexpr std::size_t l2_resume_sample_capacity = 64;
    std::uint64_t l2_resume_rip[max_cpus][l2_resume_sample_capacity]{};
    std::uint64_t l2_resume_value[max_cpus][l2_resume_sample_capacity]{};
    volatile std::uint64_t l2_resume_count[max_cpus]{};
    /**
     * How much of the machine's time is spent inside this VMM.
     *
     * The measurement that decides what to fix. A second-level guest's
     * `rdmsr` of the reference counter was measured at one to four
     * milliseconds end to end, which is what makes the guest unable to
     * finish a clock tick before the next arrives. That cost is either
     * ours - a hundred VMCS accesses per entry, every one of them a VMX
     * instruction trapping to the layer below - or it is the layer
     * below's, and the two need opposite fixes: fewer instructions per
     * exit, or fewer exits.
     *
     * `handler_cycles` accumulates the time stamp counter delta from
     * the top of `on_vm_exit` to the last instant of `resume_guest`.
     * Against wall clock taken from the same counter, it says what
     * fraction of the machine is this VMM's own code. Nothing here has
     * ever measured that.
     * @{
     */
    volatile std::uint64_t handler_cycles[max_cpus]{};
    volatile std::uint64_t handler_exits[max_cpus]{};
    volatile std::uint64_t handler_entry_tsc[max_cpus]{};
    volatile std::uint64_t handler_first_tsc[max_cpus]{};
    volatile std::uint64_t handler_last_tsc[max_cpus]{};

    /**
     * The same span as `handler_cycles`, split by the exit reason that
     * caused it.
     *
     * **This exists because the phase table accounts for about forty per
     * cent of the handler and four optimisations in a row have come out
     * of the other sixty.** Measured on a settled boot: 381,680 cycles
     * an exit inside this VMM, of which `save_l2_state`, the whole of
     * `reflect_l2_exit` and the whole of `build_vmcs02` are 144,652.
     * The phases are timed where somebody once suspected a cost, so they
     * cover the reflection path and nothing else - and only 35% of exits
     * reflect. Nothing has ever measured the rest.
     *
     * Split by reason rather than by call site because the exit-reason
     * histogram is already the one distribution this VMM knows exactly,
     * so the split lands directly on a denominator that is already
     * trusted: `vmresume` at 38% of exits and `vmptrld` at 8% are the
     * guest hypervisor's own instructions, handled entirely outside
     * every phase, and if the cycles are there this says so in one line.
     *
     * Taken in `resume_guest`, from the same pair of reads
     * `handler_cycles` uses, so the two must sum to each other - which
     * is the check that says the split is complete rather than merely
     * plausible.
     * @{
     */
    static constexpr std::size_t handler_reason_slots = 64;
    volatile std::uint64_t handler_reason_cycles[handler_reason_slots]{};
    volatile std::uint64_t handler_reason_exits[handler_reason_slots]{};

    /**
     * How many of those exits came from the *second* level.
     *
     * The question it settles: whether `wrmsr` at 2.4 an exit and
     * `vmresume` at 4.5 are two ends of one round trip or two
     * independent costs. Every exit taken from a second-level guest is
     * reflected, and the guest hypervisor answers it with a VMRESUME
     * that comes straight back here - so if the wrmsr exits are second
     * level, the tick has fewer independent costs than the table implies
     * and halving either one halves both.
     *
     * Sampled at the top of `on_vm_exit`, because `load_l1_host_state`
     * clears `running_l2` during the reflection and by `resume_guest` it
     * always reads false.
     */
    volatile std::uint64_t handler_reason_from_l2[handler_reason_slots]{};
    bool handler_was_l2[max_cpus]{};

    /**
     * The VMCS accesses each reason takes, on the same span as its
     * cycles.
     *
     * The question: a `vmcall` costs 2,407,393 cycles against a
     * `wrmsr`'s 278,066 for what is the same reflection by the same
     * path, and four separate explanations for that have now been
     * named and refuted - the capture is sampled, `arm_vtl_step` is not
     * compiled in, `mark_vtl_half` is an `rdtsc`, and the
     * protection-mask decode runs 0.00 a second. **So the excess is
     * either hardware or software, and this is one number that says
     * which**: if a vmcall takes about eight times the accesses of a
     * wrmsr the cost is VMCS traffic and there is a place to look; if
     * it takes about the same, the vmcall path is doing something
     * expensive that touches nothing and the search is a different one.
     *
     * Sampled where `handler_entry_tsc` is and closed where
     * `handler_cycles` is, so the accesses and the cycles describe the
     * same span by construction rather than by agreement. Costs two
     * loads at each end of a span already bracketed, and the by-reason
     * table itself takes no VMCS access - the split shows its slots at
     * 0.00 reads and 0.00 writes a call.
     * @{
     */
    volatile std::uint64_t handler_reason_reads[handler_reason_slots]{};
    volatile std::uint64_t handler_reason_writes[handler_reason_slots]{};
    std::uint64_t handler_entry_reads[max_cpus]{};
    std::uint64_t handler_entry_writes[max_cpus]{};

    /**
     * The reflection's three phases, for a `vmcall` and for a `wrmsr`
     * side by side.
     *
     * **Every reflection on this machine costs about 51 VMCS accesses
     * except `vmcall`, which costs 230.4** - and the two go through the
     * same path, so the extra 179 are in a phase they share rather than
     * in the hypercall block. Reading that block settled that it cannot
     * be the source: the VTL capture is gated to one switch in
     * sixty-four, `arm_vtl_step` is not compiled in, `mark_vtl_half` is
     * an `rdtsc`, and the prologue that does run is four reads.
     *
     * So the question is which *shared* phase is bigger when the exit is
     * a hypercall, and the comparator is another second-level exit
     * reflected the same way. `wrmsr` is the right one: 2.31 a tick
     * against `vmcall`'s 0.46, same reflection, 51.0 accesses.
     *
     * Bucket 0 is `vmcall` and bucket 1 is `wrmsr`; anything else is not
     * recorded. Slots are `save_l2_state`, `reflect_l2_exit` and the
     * exit-information block - the three phases an L2 exit takes - and
     * the residue against `handler_reason_*` for the same reason is what
     * the reader prints as coverage, since these three do not bracket
     * the whole handler and a split that claims to is lying.
     *
     * **The hypothesis this is aimed at, named so it can be refuted
     * rather than confirmed:** a trust-level switch changes the current
     * vmcs12, and the *read* deferral in `save_l2_state` is keyed the
     * same way the write elision is - so a vmcall may be forced to read
     * guest state that a wrmsr defers. That predicts the excess lands in
     * slot 0. If it lands in slot 1 or in the residue, the hypothesis is
     * dead and the number says where to look instead.
     * @{
     */
    static constexpr std::size_t reflect_buckets = 2;
    static constexpr std::size_t reflect_slots = 3;
    std::uint8_t reason_bucket[max_cpus]{};
    volatile std::uint64_t
        bucket_phase_cycles[reflect_buckets][reflect_slots]{};
    volatile std::uint64_t
        bucket_phase_reads[reflect_buckets][reflect_slots]{};
    volatile std::uint64_t
        bucket_phase_writes[reflect_buckets][reflect_slots]{};
    volatile std::uint64_t bucket_calls[reflect_buckets]{};
    /**
     * @}
     */

    /** Adds one phase's cycles and accesses to whichever bucket this
     * exit belongs to. No-op for an exit that is neither. */
    void note_reflect_phase(std::size_t cpu,
                            std::size_t slot,
                            std::uint64_t start_tsc,
                            std::uint64_t start_reads,
                            std::uint64_t start_writes);
    /**
     * @}
     */

    /**
     * `build_vmcs02` split the way the handler was, and for the same
     * reason.
     *
     * It is 144,136 cycles of the 331,584 a `vmresume` exit costs, and
     * `vmresume` is 58% of a clock tick - so this is 43% of the largest
     * item in the only budget that decides the boot. What it is made of
     * has never been measured beyond three coarse buckets, and the
     * largest of those, "after vmptrld", carries a comment predicting it
     * is "expected to be small ... because both are elided against a
     * cache". That prediction has never been checked, and it is exactly
     * the kind of thing the coverage rule exists to catch.
     *
     * **Adjacent intervals, not nested brackets.** Each slot is the time
     * from the previous mark to this one, so the slots sum to the span
     * between the first mark and the last *by construction* - there is
     * no way for a cost to fall between two of them. What they can miss
     * is a path that returns early, and `vmcs02_split_calls` against
     * `phase_calls[2]` says how often that happened; the reader prints
     * the sum against `phase_cycles[2]` as its coverage figure, because
     * a split that does not add up is not a result.
     * @{
     */
    static constexpr std::size_t vmcs02_split_slots = 8;
    volatile std::uint64_t vmcs02_split_cycles[vmcs02_split_slots]{};

    /**
     * The VMCS accesses each slot takes, which is what says whether its
     * cycles are hardware or software.
     *
     * The question they settle: 49,269 cycles over forty guest-state
     * fields is 1,230 a field, and this VMM's launch-time price for one
     * trapping VMCS access is about 3,100 cycles. 1,230 / 3,100 is 0.4,
     * which is the shape of an elision that still reaches the hardware
     * on a fraction of fields - so either that is what it is doing, and
     * the fix is fewer accesses rather than a cheaper comparison, or the
     * count is zero and 1,230 cycles of pure software a field is a
     * different bug entirely.
     *
     * They also close the contradiction this file has carried since a
     * controlled removal of 5.4 reads an exit moved nothing: cycles
     * divided by accesses, per slot, in the *settled* state, is the
     * marginal price the launch-time benchmark was only ever a proxy
     * for. A benchmark of a thousand back-to-back VMREADs is not the
     * access pattern of a real exit, and nothing here has ever measured
     * the difference.
     * @{
     */
    volatile std::uint64_t vmcs02_split_reads[vmcs02_split_slots]{};
    volatile std::uint64_t vmcs02_split_writes[vmcs02_split_slots]{};
    /**
     * @}
     */
    volatile std::uint64_t vmcs02_split_calls{};
    /**
     * @}
     */
    /**
     * @}
     */
    /** @} */

    /**
     * The time-stamp counter offset this VMM applies to everything
     * inside the virtual machine, and the accounting that says it is
     * doing what it claims. See `ZPP_TIME_DILATION` in `nested_vmx.h`
     * for why it exists at all.
     *
     * `dilation_offset` is what goes into vmcs01's TSC offset field,
     * and - through `build_vmcs02`'s composition, which was already
     * written as a sum for exactly this - into vmcs02's alongside the
     * guest hypervisor's own. It only ever *decreases*, by `(1 - 1/n)`
     * of each interval between an exit and the entry after it, so the
     * counter every level reads stays monotonic without a check.
     *
     * `dilation_mark` is when root operation was entered, taken at the
     * top of `on_vm_exit` and consumed in `resume_guest`. Consuming it
     * sets it to the instant it was consumed, so a second call within
     * one exit - the nested path calls `resume_guest` itself - measures
     * an interval of nothing rather than charging one span twice.
     *
     * `dilation_hidden` and `dilation_charged` are the two halves of the
     * wall clock as the guest sees them, kept apart on purpose: one
     * figure could not say whether the switch is doing anything, and
     * their ratio is the dilation actually achieved, which is not the
     * one asked for - the guest's own execution is never scaled, so what
     * is achieved depends on how much of the machine the guest was
     * getting.
     * @{
     */
    std::uint64_t dilation_offset[max_cpus]{};
    std::uint64_t dilation_mark[max_cpus]{};
    volatile std::uint64_t dilation_hidden[max_cpus]{};
    volatile std::uint64_t dilation_charged[max_cpus]{};
    /** @} */

    /**
     * What the *guest hypervisor* contributes to vmcs02's TSC offset,
     * cached by `build_vmcs02` so that `apply_time_dilation` can add
     * this VMM's own to it without reading vmcs12 again.
     *
     * Needed because the offset has to be refreshed on every entry,
     * including the entries that resume a second-level guest without
     * rebuilding vmcs02 at all.
     */
    std::uint64_t tsc_offset_from_guest[max_cpus]{};

    /**
     * Moves `dilation_offset` on by whatever this VMM has just spent in
     * root operation, and writes the result into whichever VMCS is about
     * to be entered. Called from `resume_guest`, at the point where the
     * next instruction is the entry.
     */
    void apply_time_dilation(std::size_t cpu, std::uint64_t now);

    /**
     * How many second-level physical addresses were actually *walked*
     * into first-level ones, and how many extended-page-table entries
     * that walking read.
     *
     * Both, because one without the other cannot say where the mapping
     * window's cost comes from. `map_window_at` was measured at 126.6
     * calls per exit and 1,088 cycles each, and an attempt to account
     * for them by batching the MSR areas removed five - so the rest are
     * somewhere else, and every level of every walk is one of these.
     *
     * The counter sits after the two early returns that answer without
     * walking, since those cost nothing and counting them would hide the
     * ratio this exists to show: entries read divided by walks is the
     * depth, and walks per exit is what a caller could avoid.
     */
    volatile std::uint64_t l2_translate_walks[max_cpus]{};
    volatile std::uint64_t l2_translate_entries[max_cpus]{};

    /**
     * Which callers reach guest memory, by return address. See
     * `note_guest_memory_caller`, which explains why this exists.
     */
    static constexpr std::size_t guest_memory_callers = 64;

    std::uint64_t guest_memory_caller[guest_memory_callers]{};
    std::uint64_t guest_memory_caller_hits[guest_memory_callers]{};
    volatile std::uint64_t guest_memory_caller_overflow{};

    volatile std::uint64_t reference_count_backwards[max_cpus]{};
    std::uint64_t reference_count_previous[max_cpus]{};
    /** @} */

    volatile std::uint32_t l2_entry_vector[max_cpus][256]{};
    volatile std::uint64_t l2_entries_carrying_nothing[max_cpus]{};
    /** @} */

    /**
     * How many landings the ring below holds.
     */
    static constexpr std::size_t injection_landing_capacity = 16;

    /**
     * Where the second-level guest was when vector `0xd1` was injected
     * into it, and where it was at the very next exit.
     *
     * Every other counter in this investigation observes the hand-over
     * between the two hypervisors. This observes the **guest**, and it
     * is the only thing that separates the two explanations left for a
     * synthetic timer message that is delivered and never acknowledged:
     *
     * - `to_rip` inside an interrupt handler, far from `from_rip`, means
     *   the vector was taken and the handler failed somewhere after it.
     *   The defect is then inside the guest's own path and what this VMM
     *   did wrong is upstream of the handler's inputs.
     * - `to_rip` still at or beside `from_rip` means the vector was
     *   injected and **not taken**, which would be a defect here -
     *   VM-entry injection is not gated on `RFLAGS.IF` or on the
     *   interruptibility state, so nothing about the guest may refuse it.
     *
     * `to_reason` is kept with them because the two readings above are
     * only distinguishable if the exit that produced `to_rip` is known:
     * an exit taken *during* delivery lands at the handler's first
     * instruction and looks like neither.
     * @{
     */
    volatile std::uint64_t
        injection_from_rip[max_cpus][injection_landing_capacity]{};
    volatile std::uint64_t injection_to_rip[max_cpus]
                                           [injection_landing_capacity]{};
    volatile std::uint64_t
        injection_to_reason[max_cpus][injection_landing_capacity]{};
    volatile std::uint64_t injection_landing_count[max_cpus]{};
    volatile std::uint64_t injection_landing_armed[max_cpus]{};
    /**
     * @}
     */

    /**
     * The second-level guest's instruction pointer after **one**
     * instruction following an injected `0xd1`, and the exit that
     * reported it.
     *
     * `injection_to_rip` could not answer the question it was added for,
     * and the reason is worth keeping: both explanations predicted the
     * same observation. A handler that ran read the message page - an
     * ordinary memory read, causing no exit - and returned by `IRET` to
     * the resume point, so the next exit is the next iteration of the
     * guest's loop. A handler that never ran leaves the guest at the
     * resume point, so the next exit is also the next iteration of its
     * loop. Nothing the handler does before the end-of-message write
     * exits, and that write is already known never to happen.
     *
     * So this forces an exit instead of waiting for one. The monitor
     * trap flag exits after a single retired instruction, which makes
     * the answer unambiguous:
     *
     * - a RIP at the interrupt descriptor table's handler for `0xd1`
     *   means the vector was taken, and the defect is inside the
     *   handler's path rather than in the hand-over,
     * - a RIP at or beside the resume point means the vector was
     *   injected and **not taken**, which would be a defect here, since
     *   VM-entry injection is not gated on `RFLAGS.IF` or on the
     *   interruptibility state.
     *
     * `injection_step_reason` is kept beside it because a delivery that
     * faults exits with the fault rather than with the trap flag, and
     * that is a third answer rather than a failure to measure.
     * @{
     */
    volatile std::uint64_t
        injection_step_rip[max_cpus][injection_landing_capacity]{};
    volatile std::uint64_t
        injection_step_reason[max_cpus][injection_landing_capacity]{};
    volatile std::uint64_t injection_step_count[max_cpus]{};
    volatile std::uint64_t injection_step_armed[max_cpus]{};
    /**
     * @}
     */
    volatile std::uint64_t vmcs12_exit_controls{};
    volatile std::uint64_t vmcs12_entry_controls{};
    volatile std::uint64_t vmcs12_controls_captured{};
    /**
     * @}
     */

    /**
     * Every capability MSR the guest read and what it was answered with.
     *
     * A guest hypervisor that reads these and then declines has been told
     * something it will not accept, and the only way to find out which is
     * to see the answers rather than re-derive them. Freezes when full:
     * the reads that matter are the probe, and a guest that goes on to
     * run would otherwise flood it.
     * @{
     */
    static constexpr std::size_t capability_answer_capacity = 48;

    struct capability_answer
    {
        std::uint64_t msr{};
        std::uint64_t value{};
    };

    capability_answer capability_answers[capability_answer_capacity]{};
    /**
     * @}
     */
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * What the guest's driver submitted on its admin queue, and how much
     * the watching cost.
     *
     * A ring, because the interesting commands arrive in one burst during
     * the driver's initialisation and there is no reader until long
     * afterwards. Newest entry is at `(count - 1) % capacity`, the same
     * convention the exit trace uses.
     *
     * Each entry is the first command dword - opcode in its low byte and
     * command identifier in its high half - followed by the two command
     * dwords that carry the operands. That is enough to read every
     * command this question turns on: Set Features carries the feature
     * identifier in the first and the requested queue counts in the
     * second, and both Create I/O Queue commands carry the queue
     * identifier in the low half of the first.
     * @{
     */
    static constexpr std::size_t admin_observation_capacity = 256;

    struct admin_observation
    {
        std::uint32_t command{};
        std::uint32_t dword_10{};
        std::uint32_t dword_11{};
        std::uint32_t namespace_id{};
    };

    admin_observation admin_observations[admin_observation_capacity]{};
    volatile std::uint64_t admin_observation_count{};

    /**
     * How far round the guest's admin submission queue this has already
     * looked, so a doorbell ring reports only what is new.
     */
    std::uint32_t admin_observed_head{};

    /**
     * Every write to the doorbell page, counted. This is the cost of
     * watching it, and the number that decides whether a permanent trap
     * there is affordable.
     */
    volatile std::uint64_t channel_doorbell_writes{};

    /**
     * Every value the guest has written to the configuration register, in
     * order. Set on its own says the guest is shutting the controller
     * down without clearing the enable bit, which is the transition this
     * VMM does not currently notice.
     */
    static constexpr std::size_t configuration_trace_capacity = 64;
    std::uint32_t configuration_trace[configuration_trace_capacity]{};
    volatile std::uint64_t configuration_trace_count{};
    /**
     * @}
     */

    /**
     * One recorded VM exit. Sampled after the exit was handled, so the
     * fields show the state the guest is about to be resumed with rather
     * than the state it exited in.
     */
    /**
     * Whose instruction pointer an `exit_trace_entry` holds. See the
     * `rip_owner` member for why three answers are needed.
     */
    enum class rip_owner : std::uint64_t
    {
        guest,
        second_level,
        first_level,
    };

    struct exit_trace_entry
    {
        std::uint64_t reason{};
        std::uint64_t qualification{};
        std::uint64_t activity_state{};
        std::uint64_t cs_selector{};
        std::uint64_t rip{};

        /**
         * The guest-physical address the exit reported, for the two
         * reasons that report one - EPT violation and EPT
         * misconfiguration. Zero for every other reason, and not read for
         * them either: it is one more VMREAD on a path that runs on every
         * exit.
         *
         * Worth its slot because the qualification says what kind of page
         * refused the access and never which page. "A read-modify-write
         * to something readable and executable but not writable" is every
         * watched page at once, and which one it is decides who is at
         * fault.
         */
        std::uint64_t guest_physical{};

        /**
         * How many times in a row this exact exit repeated, counting the
         * first. One for an ordinary entry.
         *
         * A ring without this is destroyed by any guest that spins. A
         * processor waiting on a lock takes the same VMX-preemption timer
         * exit at the same RIP about a thousand times a second, so all
         * thirty-two slots hold one line and everything that led up to the
         * spin is gone - which is exactly the history worth having, and it
         * was lost that way on every run of this until the counter was
         * added. The log ring carries [times=N] for the same reason.
         */
        std::uint64_t repeated{};

        /**
         * The *value* behind `detail`, for the two MSR reasons - what was
         * written, or what was answered. Zero for every other reason.
         *
         * `detail` names the register and this says what went through it,
         * and the difference is the difference between "the guest is
         * asking about the clock" and "the clock is wrong". A guest
         * parked in a loop that reads the reference count and re-arms a
         * one-shot timer looks identical either way from the reason
         * alone: a deadline that advances is a clock being waited on, and
         * a deadline that does not is a clock that stopped. Four hundred
         * thousand exits were read as the first when only this can tell
         * them apart.
         *
         * Sampled from the context after handling, like `detail`, so on
         * RDMSR it is the answer the guest is about to be resumed with -
         * EDX:EAX recombined, since the halves are separate registers -
         * and on WRMSR the value the guest supplied, which nothing on
         * that path rewrites.
         */
        std::uint64_t detail_value{};

        /**
         * What the guest asked for, where the exit reason alone does not
         * say and the guest's own registers do. Zero for every other
         * reason.
         *
         * Read out of the context the handler already holds, so this costs
         * no VMREAD - which is why it can sit on a path taken by every
         * exit at all.
         *
         * Three reasons fill it in, and each means something different:
         *
         * - RDMSR and WRMSR: the MSR index, ECX. The one thing that tells
         *   an absent architectural MSR from a synthetic one the guest was
         *   invited to ask for, and the difference CLAUDE.md records as
         *   having cost an afternoon - a skipped `rdmsr` of 0x40000022
         *   that Windows reported as 0xc000000d, blaming its own boot
         *   configuration.
         *
         * - VMCALL: EAX in the high half, ECX in the low half. Two
         *   registers rather than one because the exit does not say which
         *   calling convention the caller used, and the two interfaces a
         *   guest here might be speaking disagree: KVM's own takes the
         *   call number in RAX (`kvm_emulate_hypercall` in
         *   .references/kvm/x86.c reads `nr = kvm_rax_read(vcpu)`), while
         *   the interface this VMM announces through the hypercall page
         *   MSR takes its input value in RCX. Both call codes fit in 32
         *   bits, so packing them keeps this one word rather than two and
         *   loses nothing that identifies a leaf.
         *
         * Deliberately not filled in for CPUID, which has a ring of its
         * own in cpuid_trace with the answers as well as the leaves.
         *
         * Sampled after the exit was handled, like every other field
         * here, so it is the register the guest is about to be resumed
         * with rather than the one it exited with. Those are the same
         * value today and it is worth knowing why, because the day they
         * stop being is the day this reads as the answer instead of the
         * question: nothing on the MSR paths writes RCX - the read half
         * answers into RAX and RDX - and VMCALL is refused with an
         * invalid-opcode exception rather than answered, so nothing
         * writes RAX either. A hypercall implementation that returned a
         * status in RAX would make the high half its own.
         */
        std::uint64_t detail{};

        /**
         * Whose instruction pointer `rip` is.
         *
         * `record_exit` runs from `resume_guest`, after the handlers have
         * had their say - deliberately, so the record shows what the
         * processor was about to be resumed with. Under nesting that
         * makes the address belong to one of *three* different guests
         * depending on what the handler did, and nothing said so.
         *
         * - `rip_owner::guest` - the guest that faulted, which for a
         *   second-level exit this VMM answered itself is that
         *   second-level guest. Advanced past the instruction already.
         * - `rip_owner::second_level` - a VMLAUNCH or VMRESUME the guest
         *   hypervisor executed, which the handler answered by *entering*
         *   its guest. vmcs02 is current by then, so the address is that
         *   guest's entry point and not the instruction that caused the
         *   exit.
         * - `rip_owner::first_level` - an exit reflected to the guest
         *   hypervisor. `reflect_l2_exit` has made vmcs01 current and
         *   loaded its host state, so the address is its resume site.
         *
         * Each of the last two cost real time. One address stood against
         * `rdmsr`, `vmcall` and `ext-int` alike - which no instruction can
         * be - and was read as the guest hypervisor executing synthetic
         * model-specific register reads two and a half million times.
         * Another made a first-level VMRESUME look as though the guest
         * hypervisor lived at an address in its guest's kernel, which
         * sent a symbolization at the wrong image. `cs_selector` cannot
         * separate any of them; all three run at `0x10`.
         *
         * The second-level ring is unambiguous by construction:
         * `reflect_l2_exit` writes it while vmcs02 is still current.
         */
        std::uint64_t rip_owner{};
    };

    /**
     * How many exits are kept per CPU. A ring, so this is a window on the
     * most recent exits rather than a limit on how many may happen.
     */
    static constexpr std::size_t exit_trace_capacity = 32;

    /**
     * A ring of the most recent VM exits on each CPU, for a debugger to
     * read.
     *
     * Once the guest is running there is no other way to see what the VMM
     * did: there is no console, the serial port belongs to the guest, and
     * a debugger attached from outside sees guest state only - the VMCS
     * fields that decide whether a CPU runs at all cannot be read without
     * being on that CPU with that VMCS current. So they are recorded here
     * as each exit is handled.
     */
    exit_trace_entry exit_trace[max_cpus][exit_trace_capacity]{};

    /**
     * How many ring slots have been written per CPU. Not reduced modulo
     * the capacity, so it says where the ring wraps - the newest entry is
     * at (count - 1) % capacity.
     *
     * Slots written, not exits taken: a repeat of the exit already in the
     * newest slot grows that entry's `repeated` instead of consuming a
     * slot. exit_total below is the count of exits.
     */
    std::uint64_t exit_trace_count[max_cpus]{};

    /**
     * How many of a processor's earliest timer armings to keep.
     *
     * The earliest, not the latest, and that is the whole design: the
     * guest works out the local APIC timer's rate once, early, and every
     * period it programs afterwards follows from that one measurement.
     * A ring would hold the millionth arming and evict the calibration.
     */
    static constexpr std::size_t timer_arm_capacity = 32;

    /**
     * Every value the guest writes to the timer's initial count
     * (`0x380`), with the processor's time stamp counter as it was
     * written, for the first `timer_arm_capacity` writes per processor.
     *
     * This exists because `filter_local_apic_write` deliberately does not
     * log that register - it is the hottest write on the page and the log
     * ring cannot absorb it - and that exclusion is exactly what hides
     * the calibration. Measured on the rig: this VMM's guest arms its
     * timer to about 2.38e9 where the same guest with nothing underneath
     * arms it to 1,961,755, a ratio near 1213 that is stable across
     * builds, and nothing recorded so far says how it arrives at either.
     *
     * The time stamp is what makes the values interpretable rather than
     * merely suggestive. A calibration is a pair - arm at a known count,
     * wait, read back - so the real time between two armings is what says
     * whether the guest measured a true interval and scaled it wrongly,
     * or measured an interval this VMM had already stretched.
     * @{
     */
    std::uint64_t timer_arm_value[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_tsc[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_count[max_cpus]{};
    /**
     * @}
     */

    /**
     * The *newest* timer armings, in a ring, with what they meant.
     *
     * The array above answers "how did the guest calibrate" and keeps the
     * earliest for that reason. This answers "what deadline was it
     * waiting on when it stopped", and the two cannot share storage: by
     * the time a boot freezes, the earliest thirty-two armings are
     * minutes old and the ones being asked about have long since been
     * refused a slot.
     *
     * `lvt` and `divide` are the local APIC's `0x320` and `0x3e0` as they
     * stood at each arming, because a count means nothing without them.
     * `0x320` carries the mask bit, the mode - one-shot, periodic or TSC
     * deadline - and the vector; `0x3e0` carries the divisor. Measured on
     * the rig, the guest hypervisor changes all three mid-boot: it starts
     * periodic at vector 5, then switches to **one-shot at vector 0xef
     * with divide-by-one**, arming and disarming once per processor it
     * brings up, and the last thing it ever writes to `0x320` leaves the
     * timer armed rather than masked.
     *
     * What this is for: the root partition arms Hyper-V's synthetic timer
     * 0 to 15.625 ms and halts on it, and Hyper-V then wakes only every
     * two to four seconds. In one-shot mode that decay is a *choice of
     * count*, so the counts say which of two things is happening -
     * Hyper-V deliberately sleeping long because it believes nothing is
     * due, or unable to program the short deadline the synthetic timer
     * needs. Nothing else distinguishes them.
     * @{
     */
    std::uint64_t timer_arm_recent_value[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_recent_tsc[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_recent_lvt[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_recent_divide[max_cpus][timer_arm_capacity]{};
    std::uint64_t timer_arm_recent_count[max_cpus]{};

    /**
     * The last `0x320` and `0x3e0` the guest wrote, per processor, so an
     * arming can be recorded with the mode and divisor in force.
     */
    std::uint64_t timer_lvt[max_cpus]{};
    std::uint64_t timer_divide[max_cpus]{};
    /**
     * @}
     */

    /**
     * How many of the newest synthetic-timer samples to keep.
     *
     * A ring keeping the *newest*, which is the opposite of
     * `timer_arm_capacity` above and for the opposite reason. That one
     * holds a calibration, which happens once at the beginning. This one
     * holds the last thing that happened before the machine stopped, and
     * the stop is at the end.
     */
    static constexpr std::size_t reference_sample_capacity = 32;

    /**
     * What Hyper-V's synthetic timer is being compared against, both
     * halves of it, for the newest `reference_sample_capacity` events.
     *
     * Measured 2026-08-11 on the rig: every root-partition virtual
     * processor ends on `wrmsr 0x400000b0`, `wrmsr 0x400000b1`, six
     * `rdmsr 0x40000020`, `hlt`, and is never entered again -
     * `l2_entries` frozen on all eight processors across fifty minutes.
     * So the machine stops on a synthetic timer deadline that never
     * arrives, and the question is which half of the comparison is wrong.
     *
     * Neither half was visible before this. Both `HV_X64_MSR_STIMER0_
     * COUNT` and `HV_X64_MSR_TIME_REF_COUNT` lie outside the MSR bitmap's
     * two ranges, so they exit unconditionally and are reflected to
     * Hyper-V, which is the only layer that implements them (SDM 28.1.3,
     * and `l1_wants_l2_exit`). The write's value is in hand at the exit.
     * **The read's answer is not**: Hyper-V supplies it after the
     * reflection, into the second-level guest's registers.
     *
     * It becomes visible one step later. A VMM loads its guest's general
     * purpose registers into the physical ones before executing VMRESUME
     * - KVM does exactly this in `__vmx_vcpu_run` - so at the VMRESUME
     * exit that follows the reflection, RAX and RDX already hold the
     * value Hyper-V is about to give its guest. That is where this is
     * captured, which is what `reference_read_pending` carries across.
     *
     * With the time stamp beside each sample, two consecutive reads give
     * the rate the guest sees the reference counter advance at - it is a
     * 100 ns counter, so it must be 10 MHz - and a deadline read against
     * that rate says how far away it is. Those two numbers separate "the
     * deadline is enormous because the reference clock is wrong" from
     * "the deadline is right and its expiry is never noticed", and
     * nothing short of both of them does.
     * @{
     */
    std::uint64_t reference_read_value[max_cpus]
                                      [reference_sample_capacity]{};
    std::uint64_t reference_read_tsc[max_cpus]
                                    [reference_sample_capacity]{};
    std::uint64_t reference_read_count[max_cpus]{};
    std::uint64_t stimer_arm_value[max_cpus][reference_sample_capacity]{};
    std::uint64_t stimer_arm_tsc[max_cpus][reference_sample_capacity]{};

    /** 1 for `STIMER0_COUNT`, 2 for `STIMER0_CONFIG`, 0 for an empty
     * slot. Both go in one ring so their order survives, and the order is
     * what says whether a count is a period or an absolute deadline. */
    std::uint64_t stimer_arm_kind[max_cpus][reference_sample_capacity]{};
    std::uint64_t stimer_arm_count[max_cpus]{};
    bool reference_read_pending[max_cpus]{};
    /**
     * @}
     */

    /**
     * How much of the synthetic MSR space is counted, from
     * `0x40000000`. The interface's registers all live in the first
     * couple of hundred, and the range that matters here -
     * `0x40000080`-`0x40000084` for the synthetic interrupt controller,
     * `0x40000090`-`0x4000009f` for the interrupt sources,
     * `0x400000b0`-`0x400000b1` for the timers - is well inside it.
     */
    static constexpr std::size_t synthetic_msr_capacity = 256;

    /**
     * Every second-level access to a synthetic MSR, counted per index and
     * per processor, with the time stamp of the newest write.
     *
     * This exists to settle one question, and the question is worth
     * stating because a count is otherwise a weak thing to add.
     *
     * Measured on the rig: the root partition arms synthetic timer 0
     * periodic at 15.625 ms, and Hyper-V responds by arming its own local
     * APIC timer to 15.167 ms and then 13.125 ms - honouring it exactly
     * twice - and then writes a zero initial count, disarming, and never
     * arms again. Every virtual processor then halts for ever.
     *
     * Two expiries and then silence is the shape of the interface's own
     * message protocol stalling rather than of a timer being
     * mis-programmed. A timer expiry is posted as a message and a
     * synthetic interrupt; the guest must acknowledge it by writing the
     * end-of-message register before the next can be delivered. So a
     * first expiry that is posted, a second that finds the slot still
     * occupied, and no acknowledgement ever, produces exactly this and
     * nothing else does.
     *
     * The acknowledgement is `0x40000084` and the interrupt source the
     * timer posts to is `0x40000093` - both second-level accesses, both
     * outside the MSR bitmap's ranges, so both are seen here. Counting
     * them says which of two things is true: the guest acknowledged for a
     * while and then stopped, meaning delivery decayed, or it never
     * acknowledged at all, meaning the very first synthetic interrupt
     * never arrived. Those want different fixes and nothing recorded so
     * far distinguishes them.
     * @{
     */
    std::uint64_t synthetic_msr_reads[max_cpus][synthetic_msr_capacity]{};
    std::uint64_t synthetic_msr_writes[max_cpus][synthetic_msr_capacity]{};
    std::uint64_t synthetic_msr_last_write_tsc[max_cpus]
                                              [synthetic_msr_capacity]{};

    /**
     * The newest value written to each, which is what turns the counts
     * above into addresses that can be followed.
     *
     * The one that matters is `SIMP`, `0x40000083`: it holds the guest
     * physical address of the synthetic message page, and that page is
     * sixteen 256-byte slots, one per interrupt source. Slot 3 is where a
     * timer configured to post to SINT3 puts its expiry message.
     *
     * Reading it settles what counting cannot. The counts say no message
     * is ever acknowledged; they do not say whether one was ever
     * *written*. If slot 3 holds a message - type `0x80000010`,
     * "timer expired" - then Hyper-V wrote it and only the interrupt that
     * announces it failed to arrive. If the slot is empty, Hyper-V never
     * got as far as writing, and the fault is earlier. Those are
     * different bugs in different layers and nothing recorded so far
     * separates them.
     */
    std::uint64_t synthetic_msr_last_value[max_cpus]
                                          [synthetic_msr_capacity]{};
    /**
     * @}
     */

    /**
     * The state the newest reflected `hlt` handed to the guest
     * hypervisor, per processor.
     *
     * This is the last decision point left. Every virtual processor stops
     * on a `hlt`, and because the guest hypervisor sets HLT exiting that
     * `hlt` is a VM exit rather than a halt - so the processor is parked
     * in *its* software, and whether it is ever woken is decided from the
     * state `save_l2_state` wrote into vmcs12 at that moment and from
     * nothing else.
     *
     * Why it has come down to this: the timer message is written and
     * pending, no interrupt is pending on any local APIC, nothing is in
     * service, TPR and PPR are zero everywhere, and this VMM has never
     * refused an entry - VMLAUNCH plus VMRESUME equals `l2_entries`
     * exactly on every processor. So the guest hypervisor is declining to
     * run a processor it has work for, and what it decides from is here.
     *
     * `rflags` matters for bit 9: a processor whose saved RFLAGS has
     * interrupts disabled cannot be woken by one, so a wrongly cleared IF
     * would produce exactly this and would look like nothing at all.
     * `interruptibility` matters for blocking by STI and by MOV SS.
     * `activity` is what the guest hypervisor is told it was doing.
     * @{
     */
    std::uint64_t hlt_reflect_rflags[max_cpus]{};
    std::uint64_t hlt_reflect_interruptibility[max_cpus]{};
    std::uint64_t hlt_reflect_activity[max_cpus]{};
    std::uint64_t hlt_reflect_rip[max_cpus]{};
    std::uint64_t hlt_reflect_tsc[max_cpus]{};
    std::uint64_t hlt_reflect_count[max_cpus]{};
    /**
     * @}
     */

    /**
     * Which branch of the TPR shadow decision in `build_vmcs02` ran.
     *
     * Three outcomes and, until now, no record of which: honoured, asked
     * for and refused with CR8 exiting forced in its place, or never
     * asked for. The middle one changes how the guest hypervisor learns
     * about interrupt priority, and a guest hypervisor that believes it
     * has a virtualized task priority register when it does not is the
     * shape of partial answer this project's failures are usually made
     * of.
     * @{
     */
    /**
     * What the guest hypervisor asked for in its own VMCS, and what its
     * guest was actually run with.
     *
     * **The one comparison this investigation never made.** Every
     * mechanism here has been measured against what it is supposed to do,
     * and all of them pass - but the machine boots under KVM and not
     * under this VMM, so the difference is something this VMM presents or
     * withholds rather than something it does incorrectly.
     *
     * `build_vmcs02` composes these three fields from vmcs12 and vmcs01
     * and then strips: the preemption timer and posted interrupts always,
     * the TPR shadow conditionally, and whatever `adjust_msr` refuses on
     * top. A control the guest hypervisor set and did not get back is a
     * promise broken silently - it configured itself expecting a
     * behaviour, and its guest runs without it.
     *
     * Recorded as the raw values rather than decoded, because which bit
     * matters is exactly what is not yet known and a decoder here would
     * be a guess about that.
     * @{
     */
    std::uint64_t control_pin_requested[max_cpus]{};
    std::uint64_t control_pin_granted[max_cpus]{};
    std::uint64_t control_primary_requested[max_cpus]{};
    std::uint64_t control_primary_granted[max_cpus]{};
    std::uint64_t control_secondary_requested[max_cpus]{};
    std::uint64_t control_secondary_granted[max_cpus]{};
    /** @} */

    std::uint64_t tpr_shadow_honoured[max_cpus]{};
    std::uint64_t tpr_shadow_refused[max_cpus]{};
    std::uint64_t tpr_shadow_absent[max_cpus]{};
    /**
     * @}
     */

    /**
     * What the second-level guest's task priority actually is when it
     * asks for an interrupt it never receives.
     *
     * Measured on the rig, and this is the whole reason these exist: the
     * guest writes the synthetic interrupt command register 230,933 times
     * with `0x4002f` - a fixed self-directed inter-processor interrupt at
     * vector `0x2f`, which is the vector Windows drains its deferred
     * procedure calls through - and the guest hypervisor injects vector
     * `0x2f` into it **seven** times. Meanwhile it injects `0xd1` 238,237
     * times and `0x40` 3,661 times.
     *
     * Those three numbers are not arbitrary. An interrupt is blocked when
     * its priority class, `vector >> 4`, does not exceed the task priority
     * register: 0xd1 is class 13, 0x40 is class 4, and 0x2f is class
     * **2**. Everything above class 2 is delivered and the one at class 2
     * is not, which is precisely what a task priority stuck at 2 -
     * Windows' DISPATCH_LEVEL - produces. A guest that cannot run its
     * deferred procedure calls does no work, which is what the boot does.
     *
     * What is *not* known is whose fault it is, and one byte settles it.
     * The processor virtualizes the register into the guest hypervisor's
     * own virtual-APIC page, at offset 0x80 - SDM 30.1.1, "VTPR: the
     * value of bits 7:0 of the byte at offset 080H on the virtual-APIC
     * page". If that byte reads 2 while the guest asks for `0x2f`, the
     * guest is genuinely at DISPATCH_LEVEL and the guest hypervisor is
     * right to hold the interrupt; if it reads 0, the guest hypervisor is
     * looking at something else and this VMM has mislaid the page.
     *
     * Sampled only where it is decisive - at a write to the synthetic
     * interrupt command register - rather than on every exit, which would
     * put a guest memory read on the hottest path there is.
     * @{
     */
    static constexpr std::size_t interrupt_request_capacity = 64;

    /**
     * The virtual-APIC page this processor's last entry honoured, so the
     * samples below can be checked against the page they came from.
     */
    std::uint64_t nested_virtual_apic_address[max_cpus]{};

    /**
     * The threshold that went with it, so an emulated CR8 write can raise
     * the exit the processor would have raised.
     */
    std::uint64_t nested_tpr_threshold[max_cpus]{};

    /**
     * What thread the second-level guest was running, sampled while it
     * makes no progress.
     *
     * The exit rings say what *traps*, and a blocked thread traps
     * nothing - which is the wall this investigation reached. Everything
     * measurable about the machine is healthy while a Windows kernel sits
     * in Phase 1 initialisation doing nothing, and the only thing left
     * that could say why is the guest's own idea of what it is waiting
     * for.
     *
     * So the chain in `guest_windows.h` is followed: the GS base out of
     * the VMCS names the processor control region, that names the control
     * block, and that names the running thread. Its start address
     * symbolizes into a function - which is how a thread is identified
     * without a debugger - and its state and wait reason say what it is
     * doing.
     *
     * Sampled, not traced. One sample every `guest_thread_sample_period`
     * second-level entries, because this is four dependent guest memory
     * reads through two levels of translation and the entry path is the
     * hottest one here. A blocked thread stays blocked; it does not need
     * to be watched at a microsecond.
     * @{
     */
    static constexpr std::uint64_t guest_thread_sample_period = 4096;
    static constexpr std::size_t guest_thread_sample_capacity = 32;

    struct guest_thread_sample
    {
        std::uint64_t gs_base{};
        std::uint64_t prcb{};
        std::uint64_t thread{};
        std::uint64_t idle_thread{};
        std::uint64_t start_address{};
        std::uint64_t state{};
        std::uint64_t wait_reason{};
        std::uint64_t wait_irql{};
    };

    guest_thread_sample
        guest_thread_samples[max_cpus][guest_thread_sample_capacity]{};
    std::uint64_t guest_thread_sample_count[max_cpus]{};

    /**
     * How often the second-level guest's task priority sits at each
     * priority class.
     *
     * The question is narrow and it decides whether the deferred-call
     * interrupt can ever be delivered: vector `0x2f` is priority class 2,
     * so it is blocked unless this register falls **below** `0x20`, and a
     * thread at PASSIVE_LEVEL should put it at `0x00`.
     *
     * Two hundred pokes from outside never saw `0x00` - but two hundred
     * arbitrary moments is not a distribution, and reading it from the
     * monitor samples whenever the reader happened to ask rather than
     * whenever the guest is running. This counts every sample the
     * hypervisor itself takes, which is one per four thousand
     * second-level entries and is at least regular.
     *
     * Sixteen buckets because the class is the high nibble, which is what
     * the delivery rule compares.
     */
    std::uint64_t guest_priority_class[max_cpus][16]{};

    /**
     * How often a software interrupt was outstanding when sampled, and
     * how often it was not.
     *
     * Unlike the priority histogram beside it this is not biased by where
     * the samples land: the question is not "what fraction of time" but
     * "is this ever clear", and one clear reading answers it.
     */
    std::uint64_t guest_interrupt_requested[max_cpus]{};
    std::uint64_t guest_interrupt_idle[max_cpus]{};

    /**
     * Reads one, if this entry is a sampling one. Failures are silent and
     * leave the slot zero: every offset it follows is a guess about
     * another operating system's build, and a diagnostic that stopped a
     * processor over one would be worse than the question it answers.
     */
    void sample_guest_thread(std::size_t cpu);

    /** Fills `l2_poll_code` and `l2_poll_stack`, once. */
    void capture_poll_site(std::size_t cpu);

    /**
     * Records one side of a trust-level switch. `kind` is 0 for
     * `HvCallVtlCall` and 1 for `HvCallVtlReturn`. See `vtl_differed`.
     */
    void capture_vtl_switch(std::size_t cpu,
                            std::size_t kind,
                            arch::x86_64::context & context);

    /**
     * Records one monitor-trap-flag step of the trust-level loop and
     * disarms the trace when its ring is full. See `vtl_step_rip`.
     */
    void record_vtl_step(std::size_t cpu);

    /**
     * Arms the instruction trace for whichever trust level the processor
     * comes back in, once per side and only after the loop has settled.
     * See `vtl_step_rip`.
     */
    void arm_vtl_step(std::size_t cpu, std::size_t kind);

    /**
     * Closes the half of the round trip that ends at this switch and
     * opens the next one. See `vtl_half_cycles`.
     */
    void mark_vtl_half(std::size_t cpu, std::size_t kind);

    /**
     * Whether a `guest_state_fields` index may be deferred. False for
     * the two the guest hypervisor reads out of the hardware shadow
     * region without exiting. See `guest_state_deferred`.
     */
    static bool guest_state_deferrable(std::size_t index);

    /** Records one shadow-mode divergence. See
     * `shadow_divergence_by_field`. */
    void record_guest_state_divergence(std::size_t cpu,
                                       std::size_t index,
                                       std::uint64_t in_vmcs02,
                                       std::uint64_t in_vmcs12);

    /**
     * Sets which vmcs12 is current, and invalidates anything that
     * described the old one. **Always use this rather than assigning
     * `guest_current_vmcs`** - see `guest_state_deferred_vmcs`.
     */
    void set_guest_current_vmcs(std::size_t cpu, std::uint64_t address);

    /**
     * Whether the ordering conditions for deferral hold - independent
     * of whether deferral is switched on. Shadow mode asks this; the
     * write path asks `may_defer_guest_state`, which is this **and**
     * the switch.
     */
    bool guest_state_deferral_licensed(std::size_t cpu) const;

    /**
     * Whether the deferred guest-state fields may be left as vmcs02
     * holds them. See `guest_state_deferred_vmcs`.
     */
    bool may_defer_guest_state(std::size_t cpu) const;

    /** Which `guest_state_fields` slot an encoding names, if any. */
    static std::optional<std::size_t>
    guest_state_index_of(std::uint64_t encoding);

    /** Marks a guest-state field the level above has written, so the
     * next entry writes it back and no materialisation overwrites it. */
    void mark_l2_guest_state_dirty(std::size_t cpu,
                                   std::uint64_t encoding);

    /** Materialises the deferred fields if this encoding is one. */
    void materialise_l2_guest_state_for(std::size_t cpu,
                                        std::uint64_t encoding);

    /**
     * Copies the deferred guest-state fields out of vmcs02 into vmcs12,
     * leaving anything the guest hypervisor has written alone.
     *
     * Called from the one interception point that can need them, and
     * **from there as a repair rather than as a precondition** - see
     * `guest_state_deferred`.
     */
    void materialise_l2_guest_state(std::size_t cpu);

    /**
     * Records where the VP assist page really is, by two independent
     * translations, and arms a write-watch on it. See
     * `vp_assist_l2_physical`.
     */
    void settle_vp_assist_page(std::size_t cpu);

    /** The write-watch handler for the VP assist page. See
     * `vp_assist_writes`. */
    static void on_vp_assist_write(void * context,
                                   std::uint64_t page,
                                   const guest_write * write);

    /**
     * Whether a `load_l1_host_state` slot has been audited often enough,
     * and never once found changed, for its write to be skipped. See
     * `l1_host_samples`.
     */
    bool host_field_elidable(std::size_t cpu, std::size_t index) const;

    /**
     * Reads vmcs02's entry-interruption field back at the last
     * instruction before entry, and counts an entry that carries
     * nothing while the guest could have taken a deferred procedure
     * call. See `l2_given_vector`.
     */
    void record_l2_entry_event(std::size_t cpu);

    /**
     * The base of the page-aligned PE image containing an address, found
     * by scanning back for `MZ`, and the name from its export directory.
     * Zero when neither is found within the bound.
     */
    std::uint64_t image_base_of(std::size_t cpu, std::uint64_t address);
    void image_name_of(std::size_t cpu,
                       std::uint64_t base,
                       std::span<char> into);

    /** The name from the image's CodeView record, for the drivers that
     * export nothing and so have no export directory to name them. */
    void image_debug_name_of(std::size_t cpu,
                             std::uint64_t base,
                             std::span<char> into);

    void copy_image_string(std::size_t cpu,
                           std::uint64_t at,
                           std::span<char> into);

    /**
     * The address an image exports a name at, or zero. Needed because
     * the driver holding the boot exports nothing and carries no
     * resident symbol-file record, so the only thing left that names it
     * is the kernel's own loaded-module list - and finding that list
     * means looking `PsLoadedModuleList` up in ntoskrnl's exports.
     */
    std::uint64_t image_export(std::size_t cpu,
                               std::uint64_t base,
                               const char * name);

    /**
     * Walks the kernel's loaded-module list for the image based at
     * `image` and copies its `BaseDllName`. The list is
     * `LDR_DATA_TABLE_ENTRY`s linked through their first member, with
     * `DllBase` at `0x30` and `BaseDllName` at `0x58`.
     */
    void module_name_of(std::size_t cpu,
                        std::uint64_t kernel,
                        std::uint64_t image,
                        std::span<char> into);

    /**
     * Every thread of the running thread's process, with what each is
     * doing.
     *
     * The processor is idle, so its *current* thread is the idle thread
     * and says nothing at all - which is what the samples above showed.
     * The thread worth finding is the blocked one, and during Phase 1 it
     * is in the system process's list along with a handful of others.
     *
     * Walked until it finds a process with more than one thread, and
     * then never again. Two narrower rules were tried first and both
     * failed for reasons worth keeping: walking once on the first
     * plausible thread caught the *idle* process, whose list is one
     * thread long by construction, and walking only from a thread that
     * was not the idle thread never fired at all, because this processor
     * alternates between two virtual trust levels and every sample from
     * the one these offsets describe found it idle. Conditioning on the
     * answer rather than the question is what works.
     *
     * Bounded by `thread_walk_limit` and by the head reappearing, because
     * a list read out of another operating system's memory is not
     * something to trust to terminate - and bounded in time because it
     * stops for good as soon as it succeeds.
     * @{
     */
    struct guest_thread_entry
    {
        std::uint64_t thread{};
        std::uint64_t start_address{};
        std::uint64_t state{};
        std::uint64_t wait_reason{};
        std::uint64_t wait_irql{};
    };

    guest_thread_entry
        guest_thread_list[guest_windows::thread_walk_limit]{};
    std::uint64_t guest_thread_list_count{};
    std::uint64_t guest_thread_list_process{};
    std::uint64_t guest_thread_list_walked{};

    void walk_guest_threads(std::size_t cpu, std::uint64_t thread);

    /**
     * Re-reads what the recorded threads are doing.
     *
     * The walk runs once - it is expensive and the membership does not
     * change while nothing happens - but *what those threads are doing*
     * is the whole question, and the snapshot taken when the list was
     * built is from whatever moment happened to succeed. Here that caught
     * `Phase1Initialization` still running, which says nothing about the
     * stall that follows.
     */
    void refresh_guest_threads(std::size_t cpu);

    std::uint64_t guest_thread_refreshes{};

    /**
     * Return addresses found on the second-level guest's stack, newest
     * first.
     *
     * The instruction pointer says which function is spinning and that
     * has been known for hours - `HvlpGetRegister64`, reading the
     * reference counter. What it cannot say is *why*, because the
     * condition being waited on belongs to whoever called it, and a
     * program counter has no caller in it.
     *
     * Found by scanning rather than unwinding. Unwinding needs the
     * exception directory of an image this VMM does not have and could
     * not trust, whereas a return address is recognisable by where it
     * points: inside the kernel image, which is now known at runtime
     * along with its size. That admits false positives - a stale word
     * from a deeper frame reads exactly like a live one - so what this
     * produces is a set of candidates to symbolize, not a call stack,
     * and it is described that way wherever it is read.
     *
     * It is still decisive here. The functions in this loop are a handful
     * and the callers above them are a handful more; a list of sixteen
     * plausible addresses with names on them separates "waiting for a
     * device" from "waiting for a message" from "waiting for a lock" in
     * one reading.
     * @{
     */
    /**
     * How far up the stack to look, and how many candidates to keep.
     *
     * A kilobyte was the first attempt and it reached only the interrupt
     * frame: every exit this loop takes is a model-specific register
     * access made *inside* the clock interrupt handler, so the newest
     * frames are always `KiInterruptDispatchNoLockNoEtw` and its
     * callees. Four kilobytes was the second and still returned six
     * candidates, all of them that handler's.
     *
     * Sixteen is the whole of a kernel stack, which is what it takes: the
     * interrupted thread's frames are at *higher* addresses than the
     * handler's stack pointer - the stack grows down - and how much
     * higher depends on how deep it was when the tick arrived. Guessing
     * that distance is what the two shorter scans were doing.
     */
    static constexpr std::size_t guest_stack_words = 2048;
    static constexpr std::size_t guest_stack_capacity = 48;

    std::uint64_t guest_kernel_size{};
    std::uint64_t guest_stack_trace[guest_stack_capacity]{};
    std::uint64_t guest_stack_count{};
    std::uint64_t guest_stack_pointer{};

    /**
     * The instruction pointer the stack was sampled at, so a trace can be
     * read against where the guest actually was rather than against an
     * assumption about it.
     */
    std::uint64_t guest_stack_rip{};

    /**
     * Where the second-level guest spends its time, sampled on a clock
     * this VMM owns rather than on the guest's own exits.
     *
     * **Every other instrument here is driven by exits, and the guest
     * being chased takes none.** Between clock ticks it spins on memory:
     * 1,449 second-level entries a second against a hundred ticks is
     * fourteen exits per tick and nothing in between, so every sample
     * from every ring lands in the clock interrupt handler - which is not
     * where the problem is.
     *
     * The VMX-preemption timer fixes that by exiting on a schedule the
     * guest cannot influence. A table rather than a ring, because a spin
     * is a small set of addresses hit enormously often and what is wanted
     * is which they are, not the order.
     * @{
     */
    static constexpr std::size_t profile_capacity = 64;

    std::uint64_t profile_rip[profile_capacity]{};
    std::uint64_t profile_hits[profile_capacity]{};
    std::uint64_t profile_samples{};
    std::uint64_t profile_overflow{};

    /**
     * The registers at a profile sample, for the newest few.
     *
     * The instruction pointer says *which* read repeats and the registers
     * say *what it is reading*. For `HalpPciReadMmConfigUshort` - which
     * is 27.8% of samples at the stall - that is the difference between
     * "enumerating the bus", which is bounded and ordinary, and "polling
     * one device register", which is not.
     *
     * A ring rather than a table, because unlike the addresses these are
     * expected to vary and their *spread* is the answer: registers that
     * never change are a poll, and registers that walk are a scan.
     * @{
     */
    static constexpr std::size_t profile_context_capacity = 32;

    struct profile_context
    {
        std::uint64_t rip{};
        std::uint64_t rax{};
        std::uint64_t rcx{};
        std::uint64_t rdx{};
        std::uint64_t rbx{};
        std::uint64_t rsi{};
        std::uint64_t rdi{};
        std::uint64_t r8{};
    };

    profile_context profile_contexts[profile_context_capacity]{};

    /**
     * The physical address behind the polled configuration pointer.
     *
     * `rcx` at the stalled instruction is a virtual address in a window
     * `HalpPciMapMmConfigPhysicalAddress` made, and it is the same value
     * at every sample. What is wanted is what it *maps to*: subtract the
     * memory-mapped configuration base and the result names the bus,
     * device and function whose vendor identifier reads `0xffff` for
     * ever.
     *
     * Translated on every sample. Rate-limiting it produced a false
     * conclusion: the profiler fires only when the guest runs a long time
     * without exiting, which on a settled machine is about once a minute,
     * so a value refreshed every thirty-two samples is refreshed every
     * half hour - and three readings taken seconds apart returned the
     * same number and were read as three independent observations
     * agreeing. They were one observation read three times.
     *
     * The sample rate bounds the cost, not a counter on top of it.
     */
    std::uint64_t profile_pointer_physical{};
    std::uint64_t profile_pointer_virtual{};

    /**
     * Which instruction the pointer above was taken at.
     *
     * Without it the translation is uninterpretable: it is `rcx` at
     * whatever the profiler happened to catch, and `rcx` means a
     * configuration pointer only inside `HalpPciReadMmConfigUshort`.
     * A reading taken during the page-frame work translated to ordinary
     * memory and said nothing, which is the failure this prevents.
     */
    std::uint64_t profile_pointer_rip{};
    std::uint64_t profile_context_count{};
    /** @} */

    void record_profile_sample(std::uint64_t rip);
    void record_profile_context(std::uint64_t rip,
                                const arch::x86_64::context & context);
    /**
     * @}
     */

    void sample_guest_stack(std::size_t cpu);

    /**
     * The stack pointer of whatever the interrupt interrupted.
     *
     * The clock interrupt runs on its own stack - Windows gives some
     * vectors an interrupt stack table entry - so scanning the handler's
     * stack finds the handler's frames at any depth and never the
     * thread's. Sixteen kilobytes of it returned nothing but
     * `KiInterruptDispatchNoLockNoEtw` and its callees, which is not a
     * scan that was too short but the wrong stack entirely.
     *
     * The interrupted stack pointer is on the handler's stack, though,
     * because hardware put it there. SDM 7.14.2: a 64-bit interrupt
     * pushes SS, RSP, RFLAGS, CS and RIP unconditionally, so the frame is
     * five consecutive quadwords with a recognisable shape - a canonical
     * kernel instruction pointer, then a code selector, then flags with
     * bit 1 set, then a canonical kernel stack pointer, then a stack
     * selector.
     *
     * Found by that shape rather than by an offset, because where the
     * frame sits depends on how much the handler has pushed since.
     * @{
     */
    std::uint64_t guest_interrupted_rsp{};
    std::uint64_t guest_interrupted_rip{};
    std::uint64_t guest_interrupted_trace[guest_stack_capacity]{};
    std::uint64_t guest_interrupted_count{};

    void sample_interrupted_stack(std::size_t cpu,
                                  std::uint64_t stack,
                                  std::uint64_t base,
                                  std::uint64_t size);
    /** @} */
    /**
     * @}
     */

    /**
     * Where the second-level guest's kernel image is loaded, found by
     * stepping down from an address inside it until its header appears.
     *
     * Recorded as well as returned, because it is what makes every
     * instruction pointer in the rings symbolizable offline without
     * deducing the base from an instruction's own bytes - which works,
     * and needs a second anchor whenever the image is not 2 MB aligned.
     */
    std::uint64_t find_guest_kernel_base(std::size_t cpu);

    /**
     * Whether an image at this address is the guest's kernel, asked by
     * making the image name itself rather than by trusting a header
     * match - every image in the address space has the same header.
     */
    bool is_guest_kernel_image(std::size_t cpu,
                               std::uint64_t base,
                               std::uint64_t headers);

    std::uint64_t guest_kernel_base{};
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * Answers a second-level guest's CR8 access against the guest
     * hypervisor's virtual-APIC page.
     *
     * Reached only where the TPR shadow was not handed to the processor -
     * see `nested_vmx::tpr_shadow_offered` - in which case CR8 load and
     * store exiting were forced in its place and these exits are this
     * VMM's to answer.
     *
     * Returns false where the access is not one it can answer, so the
     * caller can fall through to the ordinary refusal rather than resume
     * a guest as though a write had landed.
     */
    bool on_nested_cr8_access(std::size_t cpu,
                              std::uint64_t qualification,
                              arch::x86_64::context & context,
                              bool & advance_rip);

    /**
     * How many CR8 accesses were emulated, and how many of them dropped
     * the priority far enough to owe the guest hypervisor an exit.
     * @{
     */
    std::uint64_t nested_cr8_reads[max_cpus]{};
    std::uint64_t nested_cr8_writes[max_cpus]{};
    std::uint64_t nested_cr8_below_threshold[max_cpus]{};
    /** @} */

    /**
     * VTPR, and the command that was being written, for the newest
     * `interrupt_request_capacity` requests.
     */
    std::uint8_t interrupt_request_vtpr[max_cpus]
                                       [interrupt_request_capacity]{};
    std::uint64_t interrupt_request_command[max_cpus]
                                           [interrupt_request_capacity]{};
    std::uint64_t interrupt_request_count[max_cpus]{};

    /**
     * The same task priority as a histogram, because the ring above
     * holds only the newest `interrupt_request_capacity` requests and
     * the guest makes tens of thousands of them.
     *
     * **The processor priority was sampled here first and it was the
     * dead field again.** SDM 12.8.3.1 makes PPR the value an arriving
     * interrupt's class must exceed, so PPR is the reading this
     * question wants - but SDM 32.1.1 only has the processor maintain
     * VPPR under "virtual-interrupt delivery", which is not offered
     * here and which the layer below does not permit this VMM either.
     * It read `0x00` on 100% of 31,427 requests and would have been
     * reported as "the guest asks while at PASSIVE, so the guest
     * hypervisor is failing to deliver" - the exact opposite of what
     * the maintained field says, which is `0xd0` on 64 of the newest
     * 64.
     *
     * That trap was documented in this header one commit before it was
     * walked into a second time. The lesson stands and is now applied:
     * sample the field the processor maintains, and cross-check any new
     * one against a reading already known good on the same samples.
     */
    std::uint64_t interrupt_request_vtpr_seen[max_cpus][256]{};

    /**
     * Every vector the second-level guest asked for, counted, so the
     * request side can be compared with `l2_injected_vector` on the
     * delivery side without reading a ring.
     */
    std::uint64_t interrupt_request_vector[max_cpus][256]{};

    /**
     * The second-level guest's virtual task priority, sampled on every
     * entry rather than only when it asks for an interrupt.
     *
     * `interrupt_request_vtpr` samples at the synthetic interrupt command
     * write, which happens *inside* the guest's clock handler - so it
     * reads a high priority by construction and cannot answer the
     * question. The question is whether the guest ever comes *down*: it
     * asks for vector `0x2f`, the deferred-procedure-call interrupt,
     * 34,288 times and is given it 12, and a task priority that never
     * falls below `0x20` is the guest masking it itself rather than the
     * level above withholding it.
     *
     * Sampled from the page of the VMCS actually being entered, which
     * the earlier reading did not do: the guest hypervisor keeps a
     * separate VMCS per virtual trust level, and
     * `nested_virtual_apic_address` holds whichever was built last. A
     * histogram of VTPR-over-entries cannot be read off the wrong page.
     */
    std::uint32_t l2_entry_vtpr[max_cpus][256]{};

    /** A self-directed interrupt the second-level guest asked for and the
     * level above has not delivered. See `nested_vmx::deliver_self_ipi`.
     */
    std::uint64_t l2_self_ipi_pending[max_cpus]{};
    volatile std::uint64_t l2_self_ipi_delivered[max_cpus]{};
    volatile std::uint64_t l2_self_ipi_held[max_cpus]{};

    /**
     * Self-directed synthetic interrupt commands withheld from the guest
     * hypervisor because the requesting processor's own task priority
     * refused the vector at the instant it was asked for. See
     * `nested_vmx::intercept_self_ipi`.
     *
     * The pair to read it against is `l2_self_ipi_reflected`, which
     * counts the ones that were **not** withheld because the priority
     * did admit them. A run where the second is zero and the first is
     * large is the guest never being able to take what it asks for,
     * which is what was measured; a run where the first is zero means
     * the swallow rule matched nothing and every number downstream of
     * it describes a configuration nobody built.
     */
    volatile std::uint64_t l2_self_ipi_swallowed[max_cpus]{};
    volatile std::uint64_t l2_self_ipi_reflected[max_cpus]{};

    /**
     * Deferred-call vectors delivered against the architectural masking
     * rule by `nested_vmx::force_dispatch_once`. Bounded at one per
     * processor by construction - it is both the counter and the gate,
     * so a reading above 1 means the gate itself is broken.
     */
    volatile std::uint64_t l2_forced_dispatch[max_cpus]{};

    /**
     * Synthetic interrupt commands that named somewhere other than this
     * processor, so the assumption behind treating a physical
     * destination of zero as "me" is falsifiable rather than implicit.
     * Non-zero means the second-level guest has more than the one
     * virtual processor this rule was written for.
     */
    volatile std::uint64_t l2_ipi_not_self[max_cpus]{};

    /**
     * One capture of the code and stack at the reference-counter poll,
     * taken once per boot.
     *
     * The guest is in a timed retry loop - arm a 2.5 ms timer, poll the
     * counter until it expires, ask for a deferred procedure call,
     * repeat - and what it retries *on* is not in any counter. It is in
     * the instructions around the poll and in the return addresses above
     * it. Both move with address-space layout randomisation every boot,
     * so they have to be fetched from inside rather than computed
     * outside.
     *
     * Read through `translate_guest_linear`, which walks the guest's own
     * page tables and then the guest hypervisor's extended ones - the
     * same two steps `sample_guest_thread` needs, and the reason reading
     * this from a debugger has failed every time: a CR3 sampled from
     * outside is whichever trust level exited last.
     */
    static constexpr std::size_t l2_poll_code_size = 128;
    // Sixty-four, not sixteen. A sixteen-word window caught the driver
    // frames in one capture and nothing but the kernel in the next: the
    // call path varies, and 128 bytes of stack is shallower than the
    // frames that matter. Also visible in that window and worth knowing
    // it is expected - the value `0x61a8`, 25,000, appearing several
    // times over, which is the 2.5 ms delay this loop asks for.
    static constexpr std::size_t l2_poll_stack_words = 64;

    std::uint8_t l2_poll_code[l2_poll_code_size]{};
    std::uint64_t l2_poll_stack[l2_poll_stack_words]{};
    std::uint64_t l2_poll_code_base{};
    std::uint64_t l2_poll_rip{};
    std::uint64_t l2_poll_rsp{};
    volatile std::uint64_t l2_poll_captured{};

    /**
     * The images the retry loop's return addresses belong to, found
     * without symbols.
     *
     * A loaded driver is a page-aligned PE image, so scanning back from
     * any address inside it reaches `MZ`, and the export directory's Name
     * field is the file name it was built as. That is the whole trick,
     * and it needs no `PsLoadedModuleList` and no symbol server - which
     * matters because neither is available from here.
     *
     * `l2_kernel_base` is whatever image the poll itself is in, which is
     * the kernel. `l2_driver_base` is the first return address on the
     * stack that resolves to a *different* image, and that is the one
     * worth naming: the loop's caller is not in the kernel.
     */
    static constexpr std::size_t l2_image_name_size = 96;

    std::uint64_t l2_kernel_base{};
    std::uint64_t l2_driver_base{};
    std::uint64_t l2_driver_address{};
    char l2_kernel_name[l2_image_name_size]{};
    char l2_driver_name[l2_image_name_size]{};

    /**
     * Whether the trust-level switch loop advances, and what it is made
     * of.
     *
     * The second-level guest alternates `HvCallVtlCall` and
     * `HvCallVtlReturn` - hypercall codes 0x11 and 0x12 - at two
     * instruction pointers twenty-five bytes apart in the hypercall
     * page, tens of thousands of times, with nothing else between them.
     * From outside that is indistinguishable from two things that want
     * opposite work: a secure call being *made* repeatedly because it is
     * making progress, and a secure call being *retried* because it is
     * not. Every counter this VMM had said only how many.
     *
     * So the registers are compared against **the previous switch of the
     * same kind** rather than against a fixed first capture. A running
     * count of how often each one changed answers it directly: all zero
     * is a livelock, and whichever entries are non-zero name what the
     * loop carries. Comparing against a fixed first snapshot would not
     * do, because the boot legitimately switches trust levels before the
     * loop starts and there is no way to know from in here which capture
     * is the first one inside it.
     *
     * Slots 0-15 are the general-purpose registers in `context` order,
     * except that slot 4 is the VMCS's guest RSP rather than the
     * context's - the context's is not the second-level guest's. Slot 16
     * is RIP, 17 CR3, 18 RFLAGS, and 19 the extended-page-table pointer
     * out of vmcs12, which is what distinguishes the two trust levels'
     * address spaces from each other.
     */
    static constexpr std::size_t vtl_slot_count = 20;

    /** Two trust-level sides and one synthetic-timer arm; see
     * `timer_arm_kind`, which shares this machinery because what is
     * wanted of it is the same - a stack and an image name for a call
     * site whose instruction pointer says nothing on its own. */
    static constexpr std::size_t vtl_kinds = 3;
    static constexpr std::size_t timer_arm_kind = 2;

    std::uint64_t vtl_switches[max_cpus][vtl_kinds]{};
    std::uint64_t vtl_previous[max_cpus][vtl_kinds][vtl_slot_count]{};
    std::uint64_t vtl_differed[max_cpus][vtl_kinds][vtl_slot_count]{};
    std::uint64_t vtl_first[max_cpus][vtl_kinds][vtl_slot_count]{};
    std::uint64_t vtl_latest[max_cpus][vtl_kinds][vtl_slot_count]{};

    /**
     * One capture of the stack on each side of the switch, taken well
     * inside the loop rather than at its first turn.
     *
     * The instruction pointer says nothing here on purpose: both sides
     * are in the hypercall page, which is a stub the guest hypervisor
     * published and is the same address for every caller. What names the
     * caller is the return addresses above it, exactly as
     * `capture_poll_site` uses them - and unlike that one, the two
     * captures are in *different address spaces*, so each has to be
     * taken while its own side is the one that exited.
     */
    static constexpr std::size_t vtl_stack_words = 64;
    static constexpr std::uint64_t vtl_capture_at = 4096;

    /** How often each trust-level side is re-captured once the loop is
     * running, so two dumps say whether its state advances. */
    static constexpr std::uint64_t vtl_recapture = 64;

    std::uint64_t vtl_stack[vtl_kinds][vtl_stack_words]{};
    std::uint64_t vtl_rip[vtl_kinds]{};
    std::uint64_t vtl_rsp[vtl_kinds]{};
    std::uint64_t vtl_cr3[vtl_kinds]{};
    std::uint64_t vtl_image_base[vtl_kinds]{};
    std::uint64_t vtl_caller_base[vtl_kinds]{};
    std::uint64_t vtl_caller_address[vtl_kinds]{};
    char vtl_image_name[vtl_kinds][l2_image_name_size]{};
    char vtl_caller_name[vtl_kinds][l2_image_name_size]{};
    /** The instructions around each side's caller, so the loop body can
     * be disassembled outside. See `capture_vtl_switch`. */
    // Wide enough to hold the whole loop body, not just the call.
    // Measured: the guest is resumed after a reflected synthetic-MSR
    // write at `ntoskrnl`+0x6a768e and makes the secure call from
    // +0x6a774b - 0xbd bytes apart, so both are in one function, and
    // that function is the loop. A 128 byte window centred on the call
    // could not reach the write.
    // Widened again, backwards. The 384 byte window reached the fast
    // path - the branch that skips the rendezvous - and proved the
    // secure kernel does not stall there, so what decides is earlier
    // still. 1024 bytes starting 0x400 back covers the whole of a
    // plausible dispatch routine either side of the call.
    static constexpr std::size_t vtl_code_size = 1024;
    static constexpr std::uint64_t vtl_code_behind = 0x400;

    /**
     * The page the two trust levels talk through, sampled at each side
     * of the switch and for each level's own copy of it.
     *
     * The loop's registers and stack are byte-identical across thousands
     * of switches, so the retry decision is made from memory, and this
     * is the memory the interface defines for it: the VTL control
     * structure lives at offset 0x100 - entry reason, pending flags,
     * return registers - and the APIC assist at offset 0.
     *
     * Two copies because the register is per trust level and each
     * configures its own, keyed on the extended-page-table pointer in
     * force when it was written. One slot would hold whichever wrote
     * last, with nothing to say which that was.
     */
    static constexpr std::size_t vtl_assist_size = 512;

    /**
     * The distinct instruction pointers the second-level guest is
     * *entered* at, with counts. See the comment at the fill site: this
     * separates "the guest is looping in its own software" from "the
     * guest hypervisor is resuming it at the VMCALL it never advanced
     * past", and nothing else this VMM records does.
     */
    static constexpr std::size_t l2_entry_rip_slots = 8;

    std::uint64_t l2_entry_rip[max_cpus][l2_entry_rip_slots]{};
    volatile std::uint64_t l2_entry_rip_count[max_cpus]
                                             [l2_entry_rip_slots]{};
    volatile std::uint64_t l2_entry_rip_other[max_cpus]{};

    /** What the guest hypervisor arms as its TPR threshold, by value.
     * All zero means it never asks to be told, so the undelivered
     * dispatch vector is its business rather than this VMM's. */
    volatile std::uint64_t l2_tpr_threshold_seen[max_cpus][16]{};

    /** Entries where the threshold was armed and SDM 27.6.7's
     * condition held, against those where it was armed and the guest
     * was already at or above it. See the fill site. */
    /** VMFUNC from the second-level guest: how many, how many were
     * refused with #UD, and how many switched the extended-page-table
     * pointer. Refused rising with switched at zero means the guest
     * hypervisor publishes a list this VMM cannot follow. */
    /** The guest hypervisor's own MSR bitmap address, so what it
     * intercepts can be read from outside and compared with what this
     * VMM merges into it. */
    std::uint64_t nested_guest_msr_bitmap[max_cpus]{};

    volatile std::uint64_t l2_vmfunc_calls[max_cpus]{};
    volatile std::uint64_t l2_vmfunc_refused[max_cpus]{};
    volatile std::uint64_t l2_vmfunc_switched[max_cpus]{};

    volatile std::uint64_t l2_tpr_would_fire[max_cpus]{};
    volatile std::uint64_t l2_tpr_armed_above[max_cpus]{};

    std::uint64_t l2_vp_assist[max_cpus][2]{};
    std::uint64_t l2_vp_assist_eptp[max_cpus][2]{};
    std::uint8_t vtl_assist[vtl_kinds][2][vtl_assist_size]{};

    /** How many bytes of it were read, why the read stopped - the error
     * code with 1 in the high half for the address translation and 2 for
     * the read itself - and what the second level's address translated
     * to. An all-zero buffer means nothing without these. */
    volatile std::uint64_t vtl_assist_read[vtl_kinds][2]{};
    volatile std::uint64_t vtl_assist_error[vtl_kinds][2]{};
    volatile std::uint64_t vtl_assist_first[vtl_kinds][2]{};

    std::uint8_t vtl_code[vtl_kinds][vtl_code_size]{};
    std::uint64_t vtl_code_base[vtl_kinds]{};

    /** Whatever the page-aligned pointer on each side's stack refers
     * to. Registers and stack are identical every iteration, so the
     * secure call's content is in memory, and this is the only
     * candidate the captures have produced. See capture_vtl_switch. */
    static constexpr std::size_t vtl_shared_size = 256;

    std::uint8_t vtl_shared[vtl_kinds][vtl_shared_size]{};
    volatile std::uint64_t vtl_shared_at[vtl_kinds]{};
    volatile std::uint64_t vtl_shared_read[vtl_kinds]{};
    volatile std::uint64_t vtl_shared_error[vtl_kinds]{};

    /** A page-aligned pointer seen on either side's stack, so the side
     * whose mappings cover it can follow it. The second trust level
     * holds one it does not itself map. */
    std::uint64_t vtl_follow_at{};

    /** The globals the secure kernel spins on, at a fixed module
     * offset from its own return address. It takes no exit between the
     * two hypercalls, so this is the only state it can be deciding
     * on. See capture_vtl_switch. */
    static constexpr std::size_t vtl_spin_size = 256;

    std::uint8_t vtl_spin[vtl_kinds][vtl_spin_size]{};
    volatile std::uint64_t vtl_spin_at[vtl_kinds]{};
    volatile std::uint64_t vtl_spin_read[vtl_kinds]{};
    volatile std::uint64_t vtl_spin_error[vtl_kinds]{};

    /** The page the secure kernel's request names, and what both sets
     * of tables say about it. See capture_vtl_switch. */
    volatile std::uint64_t vtl_page[vtl_kinds]{};
    volatile std::uint64_t vtl_page_first[vtl_kinds]{};
    volatile std::uint64_t vtl_page_mapped[vtl_kinds]{};
    volatile std::uint64_t vtl_page_shadow[vtl_kinds]{};
    volatile std::uint64_t vtl_page_rights[vtl_kinds]{};

    /** What the guest hypervisor's own tables grant that page, walked
     * directly - the shadow's permissions are the intersection with
     * ours and so cannot tell a restriction of its making from one of
     * ours. */
    volatile std::uint64_t vtl_page_guest_status[vtl_kinds]{};
    volatile std::uint64_t vtl_page_guest_rights[vtl_kinds]{};
    volatile std::uint64_t vtl_captured[vtl_kinds]{};

    /**
     * Every instruction one side of the trust-level loop executes, taken
     * with the monitor trap flag.
     *
     * This is the one question the whole investigation has left and the
     * only instrument that can ask it. Between `HvCallVtlCall` and
     * `HvCallVtlReturn` the second trust level takes **zero** exits, so
     * nothing this VMM records says what it does - and the first level's
     * registers and stack are byte-identical every iteration, so its
     * decision to call again is invisible too. Both sides are therefore
     * black boxes made of instructions nothing observes, and the monitor
     * trap flag observes exactly that: SDM 26.5.2 makes it an exit after
     * every retired instruction, with no cooperation from the guest and
     * nothing written into its memory.
     *
     * Armed once per side, after the loop has clearly settled, and it
     * disarms itself when the ring is full. Bounded on purpose: it is a
     * VM exit per instruction and a guest stepped for ever would not be
     * the guest being measured. `vtl_step_active` holds the kind plus
     * one while a trace runs, which is also what `build_vmcs02` reads to
     * keep the flag set across the rebuild every entry does.
     *
     * `vtl_step_cr3` is recorded per step rather than once, because the
     * trace is armed from the exit *before* the entry and which trust
     * level the processor comes back in is the thing being established,
     * not something to assume. A trace armed after `HvCallVtlCall` is
     * expected to be the secure kernel and one armed after
     * `HvCallVtlReturn` the ordinary kernel; the recorded control
     * register says which it actually was.
     *
     * Sixteen bytes of code come with **every** step, because a trace of
     * bare addresses cannot be disassembled outside - the tree has no
     * copy of either image, and the 1024 byte window
     * `capture_vtl_switch` takes covers only the call site. Sixteen is
     * the longest instruction x86-64 admits, so it is always enough for
     * the one instruction each address begins.
     *
     * Per step rather than per distinct address, which the first run
     * settled: a 64 slot table of distinct addresses covered 64 of the
     * **971** the secure kernel's side turned out to have, so 94 per
     * cent of the trace came back as addresses with no instruction. The
     * dedicated table was sized for a loop that repeats and the trace is
     * not one.
     * @{
     */
    /**
     * Two sides of the trust-level loop, and one that assumes nothing.
     *
     * Kind 2 is armed on an ordinary second-level entry rather than on a
     * hypercall, every `vtl_step_free_period` entries, and it exists
     * because the other two can only ever see the loop they were built
     * to see. The priority histogram says the guest is at DISPATCH on
     * 30 per cent of its entries and at PASSIVE on 1.3 per cent, which
     * is where its deferred procedure calls would run - and `2,048`
     * instructions of *whatever is executing* is the only way to find
     * out whether they do.
     *
     * Its period is in second-level entries rather than switches, and
     * is long enough that it does not starve the other two: the settled
     * loop takes about 62 entries per trust-level switch, so 2,048
     * switches is roughly 128,000 entries and 60,000 makes the free
     * trace arm about twice as often. A collision costs one period,
     * since all three rings are overwritten anyway.
     */
    static constexpr std::size_t vtl_step_kinds = 3;
    static constexpr std::size_t vtl_step_free_kind = 2;
    static constexpr std::uint64_t vtl_step_free_period = 60000;

    /** Long enough for a whole round trip. Measured on the rig: the
     * secure kernel's side is 447 instructions and the ordinary
     * kernel's is at least 577, so a 1,024 step ring ends in the middle
     * of the second half and never reaches the call that starts the
     * next iteration. */
    static constexpr std::size_t vtl_step_capacity = 2048;
    static constexpr std::size_t vtl_step_code_size = 16;

    /**
     * When to step, and how often to step again.
     *
     * **Re-armed rather than taken once, and that is the whole of what
     * the first run of this taught.** Armed at 4,096 switches the trace
     * came back with 971 distinct addresses out of 1,024 steps on the
     * secure kernel's side and a walk through six `ntoskrnl` functions
     * on the other - a guest doing ordinary work, because at four
     * minutes into a boot that is what it was doing. The same two
     * hypercalls carry the boot and the livelock, and a threshold
     * cannot tell them apart; only a *recent* window can, which is the
     * argument `vtl_recapture` already makes for the register capture
     * beside this one.
     *
     * So the ring is overwritten every `vtl_step_rearm` switches and
     * `vtl_step_at` records which switch each trace was armed on. A
     * dump then always holds the most recent trace, and a trace whose
     * arming count is close to the switch total is one taken now.
     *
     * One constant rather than a threshold and a period: the first
     * arming is simply the first multiple, which is late enough for the
     * same reason every later one is recent enough.
     *
     * **The two sides are staggered by half a period and that is not
     * cosmetic.** Both counts advance together - the loop is one call
     * and one return - so they reach the same multiple within a switch
     * of each other, and with both arming on the same multiple the
     * second always found the first's trace still running and was
     * refused. Measured: `HvCallVtlReturn`'s ring was empty after
     * 23,690 switches while `HvCallVtlCall`'s had been rearmed twice.
     * @{
     */
    static constexpr std::uint64_t vtl_step_rearm = 2048;

    volatile std::uint64_t vtl_step_at[vtl_step_kinds]{};
    /**
     * @}
     */

    std::uint64_t vtl_step_rip[vtl_step_kinds][vtl_step_capacity]{};
    std::uint64_t vtl_step_cr3[vtl_step_kinds][vtl_step_capacity]{};
    volatile std::uint64_t vtl_step_count[vtl_step_kinds]{};

    /** Non-monitor-trap exits taken while a trace was running, which
     * are the trace being interrupted rather than the trace itself. */
    volatile std::uint64_t vtl_step_other[vtl_step_kinds]{};
    volatile std::uint64_t vtl_step_other_reason[vtl_step_kinds]{};

    std::uint8_t vtl_step_code[vtl_step_kinds][vtl_step_capacity]
                              [vtl_step_code_size]{};

    /** The kind being stepped plus one, or zero. Per processor, because
     * the flag lives in that processor's vmcs02. */
    std::uint8_t vtl_step_active[max_cpus]{};

    /**
     * How long the guest gets between one clock interrupt and the next,
     * as a histogram of the base-two logarithm of the time-stamp
     * counter delta.
     *
     * This is the question the instruction trace ends on and cannot
     * answer. In the settled state the instruction immediately after
     * `HvCallVtlReturn` is the interrupt-descriptor-table stub for
     * vector `0xd1`, and in the boot phase the guest manages twenty
     * instructions - the epilogue of one function - before the same
     * stub. Either the timer is genuinely due that often, or
     * expirations are being replayed, and those want opposite fixes:
     *
     * - deltas near the 1.74 millisecond period the guest programmed
     *   mean the timer is right and the guest is simply not being given
     *   the processor, which is a cost;
     * - deltas far below it mean a backlog of expirations is being
     *   drained one interrupt at a time, which is a defect and is
     *   somebody's to fix.
     *
     * The counter rather than the reference page, because the reference
     * page is derived from it and this VMM applies no offset - see the
     * time-stamp composition in `build_vmcs02`. Bucketed by logarithm
     * so the whole range from a microsecond to a second fits in
     * sixty-four counters with no constant to choose.
     * @{
     */
    static constexpr std::uint64_t clock_gap_vector = 0xd1;

    std::uint64_t clock_gap_last[max_cpus]{};
    std::uint64_t clock_gap_buckets[max_cpus][64]{};
    /**
     * @}
     */

    /**
     * What one trust-level round trip costs, split into its two halves.
     *
     * The chain from cost to symptom is measured and closed - a round
     * trip costs more than a guest clock period, so the clock is
     * pending again within a few instructions, so nothing below
     * CLOCK_LEVEL ever runs. What is *not* settled is where that cost
     * is, and the two candidates want opposite work:
     *
     * - if it is the number of second-level exits a round trip takes,
     *   it is this VMM's, and reflecting fewer of them is the fix;
     * - if it is the cycles per exit, it is largely the rig's - a
     *   VMREAD traps to the layer below at about 4,340 cycles here
     *   because this VMM is itself KVM's guest, and that number does
     *   not exist on bare metal.
     *
     * Halves rather than a total, because they are different code and
     * only one of them is the secure kernel's. Half 0 is
     * `HvCallVtlCall` to the matching `HvCallVtlReturn` - the second
     * trust level - and half 1 is that return to the next call, which
     * is the ordinary kernel's clock handler.
     *
     * Accumulated rather than sampled, so a reader divides by the count
     * and gets a mean over the whole boot; and `capture_vtl_switch`
     * already runs on exactly these two exits, so this costs one
     * `rdtsc` per switch and no new decision about when to measure.
     * @{
     */
    static constexpr std::size_t vtl_halves = 2;

    std::uint64_t vtl_half_cycles[max_cpus][vtl_halves]{};
    std::uint64_t vtl_half_exits[max_cpus][vtl_halves]{};
    std::uint64_t vtl_half_count[max_cpus][vtl_halves]{};

    /** Where the last switch left the two counters, and which side it
     * was, plus one. Zero means no switch has been seen yet, so the
     * first half of a boot is dropped rather than measured against an
     * unset mark. */
    std::uint64_t vtl_half_mark_cycles[max_cpus]{};
    std::uint64_t vtl_half_mark_exits[max_cpus]{};
    std::uint8_t vtl_half_mark_kind[max_cpus]{};
    /**
     * @}
     */

    /**
     * Why the DISPATCH_LEVEL software interrupt is never delivered.
     *
     * The guest writes `HV_X64_MSR_ICR` (0x40000071) with `0x4002f` -
     * a self-directed interrupt of vector 0x2f - about 250 times a
     * second, and `l2_injected_vector[0x2f]` is 207 for a whole boot.
     * "Injected zero times" has three distinct causes and they want
     * opposite work, so each gets a counter rather than an argument:
     *
     * - **never requested of us.** `l2_injected_vector` is written from
     *   vmcs12's own entry-interruption field with the self-IPI branch
     *   compiled out (checked in the built binary: `llvm-objdump` finds
     *   no reference to `l2_self_ipi_delivered`), so it already *is*
     *   what the guest hypervisor asked for. `l2_given_vector` is the
     *   same field read back out of vmcs02 at the last instruction
     *   before entry, which closes the asked-versus-given gap the same
     *   way the control sweep did - by reading what the processor will
     *   actually act on rather than what was meant to be written.
     * - **requested and dropped.** That is the two disagreeing.
     * - **queued and never eligible.** `l2_entry_ppr` is the processor
     *   priority register, not the task priority: SDM 12.8.3.1 makes
     *   PPR the value an interrupt's class must exceed, and it is the
     *   maximum of TPR and the highest in-service vector - so a guest
     *   that raised to DISPATCH and never lowered, and a guest with an
     *   unacknowledged in-service interrupt, are different faults and
     *   TPR alone cannot tell them apart.
     *
     * `l2_low_priority_no_event` is the crossing that decides it: an
     * entry made with PPR below the DISPATCH class and no event
     * injected is a moment the guest hypervisor *could* have delivered
     * `0x2f` and did not. Many of those and the fault is above us; none
     * of them and the priority never drops, which is a deadlock with a
     * different fix and not a cost.
     * @{
     */
    std::uint32_t l2_given_vector[max_cpus][256]{};
    std::uint32_t l2_entry_ppr[max_cpus][256]{};
    std::uint64_t l2_low_priority_no_event[max_cpus]{};

    /** The priority this entry was sampled at, carried from where the
     * virtual-APIC page is read to where the event it will carry is
     * known. Both are on the same entry, so this never spans one.
     *
     * It is the **task** priority and not the processor priority,
     * because the processor priority is not maintained in this
     * configuration - see the sample site for the SDM citation. TPR is
     * a lower bound on PPR, so an entry this counts is one where the
     * priority certainly would have admitted the interrupt, which is
     * the direction that makes the count mean something. */
    std::uint8_t l2_entry_priority[max_cpus]{};

    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * Whether the guest hypervisor ever writes the VP assist page.
     *
     * **"512 bytes of zero" is a reading, not an explanation**, and it
     * has been carried as one since the page was first eliminated as
     * "enabled, empty". Two possibilities that could not be more
     * different: the guest hypervisor genuinely does not use
     * `ApicAssist`, which is its business and closes the thread; or it
     * writes the page and those writes do not reach the frame this VMM
     * reads, which is **ours** - and would not be confined to lazy
     * end-of-interrupt, since that page carries the virtual trust level
     * control structure too. A page resolved to the wrong frame is a
     * fault of unknown blast radius that happens to have surfaced here
     * first.
     *
     * The mechanism to suspect is address translation.
     * `HV_X64_MSR_VP_ASSIST_PAGE` is written by *Windows*, so its value
     * is an **L2 guest-physical** address; reading it here needs
     * L2-physical to L1-physical to host-physical, and a level got
     * wrong lands on a different frame that reads as zeros rather than
     * failing.
     *
     * So both translations are recorded and cross-checked - the walk of
     * the guest hypervisor's own extended page tables, and this VMM's
     * identity map - because two paths agreeing is worth more than one
     * path looking sensible. And then the page is *watched*, so that
     * "nothing writes it" is said on the strength of a watch rather
     * than on the strength of it being empty.
     * @{
     */
    std::uint64_t vp_assist_pending[max_cpus]{};

    /** The extended-page-table pointer in force when the register was
     * announced. Each trust level has its own root, and VTL0's
     * addresses do not translate under VTL1's - so a retry taken at an
     * arbitrary exit walks the wrong tables and refuses for ever. */
    std::uint64_t vp_assist_eptp[max_cpus]{};
    volatile std::uint64_t vp_assist_l2_physical{};
    volatile std::uint64_t vp_assist_via_ept12{};
    volatile std::uint64_t vp_assist_via_identity{};
    volatile std::uint64_t vp_assist_paths_agree{};
    volatile std::uint64_t vp_assist_watch_armed{};
    volatile std::uint64_t vp_assist_writes{};
    volatile std::uint64_t vp_assist_write_page{};
    /**
     * @}
     */

    /**
     * How often the guest hypervisor asked for interrupt-window exiting
     * at the moment its guest was entered.
     *
     * It arms that control when it holds an interrupt it cannot yet
     * deliver, and the window opening means "your guest could take one
     * now". In the settled state its guest sits at CLOCK_LEVEL and
     * **never** drops below DISPATCH, so the interrupt it is waiting to
     * deliver - `0x2f` - can never be admitted: the window opens, the
     * priority still refuses, and it arms again. That is a full
     * reflection each time, 1.06 per clock tick, achieving nothing.
     *
     * Counted as a share of entries rather than as arm/disarm edges,
     * because the control is recomputed from vmcs12 on every entry and
     * an edge count would measure this VMM's rebuild rather than the
     * guest hypervisor's intent.
     * @{
     */
    volatile std::uint64_t l2_int_window_armed[max_cpus]{};
    volatile std::uint64_t l2_int_window_clear[max_cpus]{};
    /**
     * @}
     */

    /** The last value written to `HV_X64_MSR_STIMER0_CONFIG`, so the
     * count that follows can be read as a period or as an absolute
     * deadline - the interface makes it one or the other depending on
     * the periodic bit, and a capture that could not tell them apart
     * would fire on every one-shot re-arm. */
    std::uint64_t l2_stimer_config[max_cpus]{};

    /** How many periodic tick periods `ZPP_TICK_FLOOR` refused. Zero on
     * a run with the floor compiled in means it never fired, which is a
     * different answer from "it fired and changed nothing" - and telling
     * those two apart is exactly what `guest_timer_stretched` was added
     * for after a switch that silently never reached the compiler. */
    volatile std::uint64_t guest_tick_floored[max_cpus]{};

    /**
     * What `load_l1_host_state` wrote last time, and how often the
     * processor had changed it by the next reflection.
     *
     * The function is 14.1% of the wall clock in fifty-two VMWRITEs, and
     * SDM 30.3.2 blocks the obvious elision: a VM exit saves the guest
     * hypervisor's own segment bases, limits and access rights over
     * vmcs01's guest-state area, so a cache of what was last written
     * describes something the processor has since overwritten. Eliding
     * against it would skip a write that is owed, which is the defect
     * that killed `ZPP_LAZY_GUEST_STATE`.
     *
     * A field the processor demonstrably never changes is a different
     * matter, and which fields those are is a measurement rather than an
     * assumption about Hyper-V's exit path. `l1_host_changed` counts, per
     * slot, how often vmcs01 held something other than what this VMM
     * wrote there - so a slot that stays zero across a whole boot is one
     * whose write can be dropped, and a slot that does not is one that
     * never could have been.
     *
     * Indexed by *call order* rather than by field encoding, because the
     * order of the writes is fixed by the code: slot N is the same field
     * on every call, so no lookup is needed on the hot path.
     * `l1_host_field` records which encoding that was, so a reader
     * outside needs no copy of the list.
     * @{
     */
    static constexpr std::size_t l1_host_field_count = 64;

    std::uint64_t l1_host_field[max_cpus][l1_host_field_count]{};
    std::uint64_t l1_host_value[max_cpus][l1_host_field_count]{};
    volatile std::uint64_t l1_host_changed[max_cpus]
                                          [l1_host_field_count]{};
    std::uint64_t l1_host_written[max_cpus]{};
    std::uint64_t l1_host_count[max_cpus]{};
    std::uint64_t l1_host_audits[max_cpus]{};

    /**
     * How many times each slot has been audited, and the elision the
     * audit licenses.
     *
     * A slot is elided only once it has been *checked* `l1_host_stable_
     * after` times with `l1_host_changed` still zero - so the elision is
     * never an assumption, it is a claim the audit has already tested at
     * that slot and can go on testing. Measured on the rig before this
     * existed: 48 of 52 slots never diverged across 84,846 samples, and
     * the four that do are `guest_rip` and `guest_rflags` every time
     * plus two segment limits at about 2%.
     *
     * **The audit is the whole safety argument and is never compiled
     * out.** A silently wrong elision here resumes the guest hypervisor
     * with a stale control register or segment, which is the worst
     * failure this path has available; the counter that would catch it
     * must not be the thing that gets switched off.
     *
     * Audited in batches rather than one slot a reflection, and that is
     * the risk window rather than a refinement: sampling one of 52 slots
     * per reflection leaves a slot unchecked for 52 reflections, and an
     * elision is live throughout it. Four a reflection costs four
     * VMREADs against forty-eight VMWRITEs saved and shortens the window
     * to thirteen.
     *
     * A slot that ever diverges has `l1_host_changed` set for ever, so
     * it stops being elidable permanently rather than recovering - the
     * safe direction, since one divergence means the reasoning was
     * wrong and not that the field is merely busy.
     * @{
     */
    /**
     * How many clean checks license an elision.
     *
     * **64 was measured to be far too weak and the rig proved it on the
     * first boot.** Two of the 52 slots move about 2% of the time, and
     * a slot that moves 2% of the time has a **27% chance** of showing
     * 64 clean samples in a row - so it was elided, and then diverged:
     * `host field 0x4804 diverged after being elided: wrote 0xffffffff,
     * found 0xfffffff`. The audit caught it, disabled that slot for
     * ever and logged, which is the design working - but a write was
     * skipped that was owed, and that is the failure this path must not
     * have.
     *
     * At 4,096 the same slot's chance of a clean run is 1.15e-36. The
     * cost is a longer warm-up - 4,096 checks at four slots a
     * reflection over 52 slots is about 53,000 reflections - which a
     * boot passes through in its first seconds against the half-million
     * it makes.
     *
     * **A threshold cannot make this safe on its own**, and that is why
     * the repair below exists: any statistical bound is beaten
     * eventually by a rare enough event, so the audit also puts the
     * field back the moment it finds one.
     */
    /**
     * **Measured consequence: the elision warms up exactly as the guest
     * stalls, so it is off throughout the phase that matters.**
     *
     * A slot is sampled `l1_host_audit_batch` at a time out of 52, so
     * once every 13 calls, and needs 4096 samples - **53,248 calls**. At
     * the clean phase's ~1,024 reflections a second that is 52 seconds,
     * and the guest enters the stall at t=53.
     *
     * Either side of that boundary, measured: **1.9% of writes elided in
     * the clean phase, 80.7% and 90.6% in the settled loop.** This
     * mechanism was designed and validated against the stall, where it
     * works, and contributes nothing to the phase whose cost decides
     * whether the guest survives its own re-arm - `load_l1_host_state`
     * runs 55 VMCS accesses a call there instead of 8, which is 73
     * microseconds a reflection and 266 of the 850 the prevention target
     * needs.
     *
     * **Not simply lowerable.** It was 64 and was raised here after a
     * real divergence on the first boot. What makes elision safe is not
     * this number but the check that notices and repairs before the guest
     * hypervisor resumes - `l1_host_diverged`, and `DIVERGED AFTER
     * ELISION: 0` in the dump. Lowering it trades a larger repair rate
     * for a warm-up that finishes inside the clean phase; raising
     * `l1_host_audit_batch` buys the same warm-up with more VMREADs,
     * which are the thing being removed. `BACKLOG.md` has both numbers.
     */
    static constexpr std::uint64_t l1_host_stable_after = 4096;
    static constexpr std::size_t l1_host_audit_batch = 4;

    std::uint64_t l1_host_samples[max_cpus][l1_host_field_count]{};

    /**
     * How many slots have gathered `l1_host_stable_after` samples.
     *
     * Only so the sweep can tell "still gathering evidence" from "warm"
     * without rescanning the table. It counts slots that are *measured*,
     * not slots that are elidable - a slot the processor changes is fully
     * measured and never elidable, and it must not hold the sweep at its
     * warm-up rate for ever.
     */
    std::uint64_t l1_host_stable_count[max_cpus]{};
    volatile std::uint64_t l1_host_elided[max_cpus]{};
    volatile std::uint64_t l1_host_diverged[max_cpus]{};
    /** @} */

    /**
     * What the loaded-module walk got as far as, so a run that produces
     * no name says *which* step failed rather than only that one did.
     *
     * Three ways it can fail and nothing distinguished them: the export
     * lookup not finding `PsLoadedModuleList` leaves `l2_module_list`
     * zero; wrong `LDR_DATA_TABLE_ENTRY` offsets or a broken link show up
     * as a tiny `l2_modules_walked` and a nonsense
     * `l2_first_module_base`; and a complete walk that matched nothing
     * shows a plausible first base and a walk length in the hundreds,
     * which would mean the base found by scanning for `MZ` is not the one
     * Windows recorded.
     */
    std::uint64_t l2_module_list{};
    std::uint64_t l2_modules_walked{};
    std::uint64_t l2_first_module_base{};

    /**
     * Records one synthetic interrupt command the second-level guest
     * issued, with the task priority in force as it did.
     */
    /**
     * Records the request and returns the virtual task priority it
     * sampled, so a caller deciding whether the vector can be delivered
     * uses **the same byte from the same page** the diagnostic recorded.
     * Two reads could disagree, and then the histogram would describe a
     * decision that was not taken.
     */
    std::uint8_t record_interrupt_request(std::size_t cpu,
                                          std::uint64_t command);
    /**
     * @}
     */

    /**
     * Exits taken per CPU, counting repeats. Together with the ring's
     * `repeated` counts this says how much of the history the window
     * covers.
     */
    std::uint64_t exit_total[max_cpus]{};

    /**
     * How many basic exit reasons are counted below.
     *
     * Sized past the architecture rather than to it: SDM Table C-1 ends at
     * 85, WRMSRNS in its immediate form, so 96 covers every reason the
     * manual defines today and leaves room before the bound below starts
     * discarding. The reason field is sixteen bits, so a processor could
     * in principle report more; anything at or above this is counted
     * nowhere rather than over something else, and the ring beside this
     * still records it in full.
     */
    static constexpr std::size_t exit_reason_capacity = 96;

    /**
     * Every exit this processor has taken, counted by basic exit reason.
     *
     * This answers "what was this processor doing" in one read, which the
     * ring beside it cannot: thirty-two slots is a window on the last
     * fraction of a second, and the question a frozen guest raises is
     * about the minutes before that. A processor that took eighty
     * thousand EPT violations and thirty external interrupts is described
     * completely by two numbers here, and not at all by its ring.
     *
     * Counted in record_exit, which runs exactly once per exit - the
     * paths that record before stopping do so instead of reaching the
     * resume, since on_unhandled_exit does not return. So the row sums to
     * exit_total for the same processor, and a disagreement between them
     * means an exit reason at or above the bound above.
     *
     * Indexed by the basic reason, so the index is the number SDM Table
     * C-1 gives: 0 exception or NMI, 1 external interrupt, 10 CPUID, 12
     * HLT, 28 control register access, 30 I/O, 31 RDMSR, 32 WRMSR, 48 EPT
     * violation, 52 VMX-preemption timer.
     */
    volatile std::uint64_t exit_reason_counts[max_cpus]
                                             [exit_reason_capacity]{};

    /**
     * Whether a start-up IPI has already started each processor, cleared
     * again by an INIT.
     *
     * INIT-SIPI-SIPI sends two start-up IPIs, so a processor that started
     * on the first would be sent back to its entry point by the second -
     * and re-applying RIP zero to a processor already executing wedges it
     * in a way indistinguishable from never having started. The guard is
     * a flag rather than a count because the architectural sequence is
     * fixed at two: the question is only whether this processor has
     * already been started, not how many times it was asked.
     */
    bool started_by_start_up_ipi[max_cpus]{};

    /**
     * Whether the *guest* has ever started this processor, as opposed to
     * the firmware having done so before the guest existed.
     *
     * The two are not the same event and conflating them is what stalled
     * a Windows boot under Hyper-V. `started_by_start_up_ipi` is set by
     * apply_start_up for every vector it applies, including the seven
     * processors the firmware brings up with its broadcast start-up IPI -
     * so an operating system starting its own processors later found them
     * all already flagged, and each of its start-up IPIs was swallowed as
     * a duplicate.
     *
     * Set only where a start-up IPI arrived from the guest, and never
     * cleared: a processor the guest has started once is one whose
     * duplicate start-up IPIs are worth ignoring for the rest of the
     * boot, which is precisely what the guard wants and what the firmware
     * flag cannot say.
     *
     * **Nothing reads it. It is a record, not a guard, and the paragraph
     * above describes a guard that no longer exists.** Kept because the
     * fact is real and is recorded nowhere else - a debugger can still
     * ask which processors the guest itself started - but read on the
     * assumption that it decides something and the conclusion will be
     * wrong.
     *
     * What replaced it is the activity-state test in
     * `start_up_processor`, and the comment there is the one to read:
     * it records the measurement that retired both flags, which
     * is that they are a *proxy* for a fact the processor already keeps.
     * On the rig with Hyper-V running, slots 2-7 had both flags set while
     * their activity state was wait-for-SIPI, so every start-up IPI
     * Hyper-V sent them was swallowed as a duplicate. Two flags, one of
     * them cleared by INIT, could not describe that state; the activity
     * state describes it exactly, and KVM makes the same single test in
     * `kvm_apic_accept_events`.
     *
     * So restoring a guard here would be re-proposing what that
     * measurement already rejected. Anything wanting this fact should
     * take it as evidence about the past, never as permission.
     */
    bool started_by_guest_start_up_ipi[max_cpus]{};

    /**
     * Each processor's x2APIC id, recorded by that processor as it comes
     * up, so an intercepted interrupt command register write naming a
     * destination can be turned back into an index here.
     */
    std::uint64_t apic_id[max_cpus]{};

    /**
     * The states a processor's start-up hand-off takes, held in
     * start_up_handoff below.
     *
     * There are two ways a start-up IPI can reach a processor coming out
     * of an INIT, and they are mutually exclusive: the architectural one,
     * where the processor parks in the wait-for-SIPI activity state and
     * the hardware delivers the IPI as a VM exit, and this VMM's own,
     * where the sending processor hands the vector over through memory
     * because a layer below would discard the hardware one.
     *
     * Which one is in use has to be one fact rather than two opinions.
     * The target chooses, publishes the choice here, and the sender obeys
     * it - and because the target can stop waiting at any moment, the
     * sender's hand-over is a compare-exchange out of software_wait
     * rather than a store. That is what makes it impossible for the
     * sender to consume an IPI the target is expecting from hardware,
     * which is precisely how one used to be lost.
     */
    /**
     * The states a processor's start-up hand-off takes, held in
     * start_up_handoff below.
     *
     * Defined once in zpp/hypervisor/start_up_handoff.h rather than here,
     * and aliased in. tests/ap_start_up asserts claims about these exact
     * numbers and replaces this header with a shim, so a nested copy is
     * somewhere a silent disagreement can live - see that header for what
     * the copy used to cost and for the invariants it now static_asserts.
     */
    using start_up_handoff_state = zpp::hypervisor::start_up_handoff_state;

    /**
     * Which hand-off each processor's next start-up IPI is to arrive
     * through, and the vector once one has been handed over.
     *
     * Atomic because the target writes it and the sender reads and
     * modifies it, on different processors, with no lock between them -
     * and because the transition out of the software wait has to be
     * indivisible from observing a vector delivered into it.
     */
    std::atomic<std::uint64_t> start_up_handoff[max_cpus]{};

    /**
     * Set once every processor has been started, after which the
     * interception is switched off - inter-processor interrupts are hot on
     * a running system and there is no reason to keep paying for them once
     * no more processors are going to start.
     */
    std::atomic<bool> all_processors_started{};

    /**
     * When the last start-up or INIT inter-processor interrupt was seen,
     * as a time-stamp counter reading, or zero if none has been.
     *
     * The only evidence available that bring-up is over. Nothing tells a
     * hypervisor how many processors its guest intends to start, so
     * `all_processors_started` cannot be derived - it can only be guessed
     * at from a long enough silence. See `nested_vmx::disarm_apic_watch`
     * for why that guess is off by default.
     */
    volatile std::uint64_t last_start_up_ipi_tsc{};

    /**
     * The memory the loader reserved below one megabyte, or zero when it
     * supplied none. Zero means this VMM cannot start a processor itself,
     * which is the normal state on the platforms where the loader launches
     * it on all of them.
     */
    std::uint64_t start_up_memory{};

    /**
     * How many entries of the per processor arrays above are in use. Index
     * zero is the boot processor, which exists before anything else does,
     * so this counts from one.
     */
    std::size_t number_of_known_processors = 1;

    /**
     * The local APIC id of every processor the platform reports, copied
     * from the launch block.
     *
     * Distinct from `apic_id` above, which holds only the processors this
     * VMM has met - one it launched on, or one a guest named in a
     * targeted start-up IPI. This is the whole machine, and it exists for
     * the one command that names no destination: a **broadcast** start-up
     * IPI, "all excluding self", which is how Windows starts its
     * processors.
     *
     * Without it a broadcast can only be passed through, and a processor
     * that starts that way is running outside this VMM. With a guest
     * hypervisor above, that is not merely a lost observation - it
     * produces processors that neither layer owns, its rendezvous never
     * completes, and it resets the machine. Measured, on the rig, as a
     * boot that reached the second level and then reset the box.
     *
     * Copied rather than pointed at, like everything else handed over:
     * the launch block belongs to the loader and does not outlive it.
     * @{
     */
    std::uint32_t platform_apic_id[max_cpus]{};
    std::size_t number_of_platform_processors{};
    /**
     * @}
     */

    /**
     * Whether the processor at each index is running under this
     * hypervisor.
     *
     * This is what decides how a start-up IPI for it is handled. One that
     * is already virtualized is sitting in the INIT handler waiting for a
     * vector to be handed to it through memory; one that is not has to be
     * started from scratch, in the trampoline, and virtualized on the way.
     */
    bool processor_virtualized[max_cpus]{};

    /**
     * The vector the guest asked each processor to begin at, kept for the
     * moment its own launch is ready to apply it. Not the vector actually
     * sent, which is the trampoline's.
     */
    std::uint64_t guest_start_up_vector[max_cpus]{};

    /**
     * Whether the processor at each index was started by this VMM's own
     * trampoline rather than launched by the loader.
     *
     * It changes what that processor's launch has to do. There is no
     * state of its own worth capturing - it came out of an INIT, so what
     * it holds is the architectural reset value and not an operating
     * system's - and its guest has to begin where the start-up IPI said
     * rather than where a caller was.
     */
    bool started_by_trampoline[max_cpus]{};

    /**
     * Whether this processor has already had a MONITOR or MWAIT exit
     * recorded in the log. An idle loop executes those continuously, so
     * they are reported once each and then never again - enough to say
     * that the guest reached them, from where, and whether its monitor
     * armed, without burying every other line.
     */
    bool monitor_logged[max_cpus]{};

    /**
     * The nested VMX state, one set per processor.
     *
     * All of it is per processor because all of it describes a *logical
     * processor's* position in VMX operation, which is what the
     * architecture makes it: VMXON puts "the logical processor in VMX
     * operation" and leaves it "with no current VMCS" (SDM 33.3, VMXON),
     * and the current-VMCS pointer is likewise per processor.
     *
     * The shadow VMCS is the exception in kind but not in placement. Its
     * authoritative copy lives in the guest's own VMCS region, so that a
     * VMCS taken from one processor and loaded on another keeps its
     * contents; this is a cache of the current one, loaded by VMPTRLD and
     * written back by anything that stops it being current. That is the
     * arrangement KVM uses, and the reason is the same: VMREAD and
     * VMWRITE are frequent and a guest memory access each would be paid
     * on every one of them.
     * @{
     */
    bool guest_in_vmx_operation[max_cpus]{};

    /**
     * How many times each processor's guest has entered and left VMX
     * operation.
     *
     * Counted rather than inferred from `guest_in_vmx_operation`, because
     * that flag is a *state* and the question is usually about *history*.
     * A guest hypervisor that executes VMXON and then VMXOFF leaves the
     * flag reading false, exactly as one that never tried does - and those
     * two are opposite findings. "Tried and gave up" says the capability
     * set was good enough to start and something later refused it;
     * "never tried" says it was not.
     *
     * So the useful reading is the pair: entries above exits means VMX
     * operation is live now, equal and non-zero means it came and went,
     * and both zero means it was never attempted. This was got wrong once
     * from the flag alone.
     * @{
     */
    volatile std::uint64_t guest_vmxon_count[max_cpus]{};
    volatile std::uint64_t guest_vmxoff_count[max_cpus]{};
    /**
     * @}
     */
    std::uint64_t guest_vmxon_pointer[max_cpus]{};
    std::uint64_t guest_current_vmcs[max_cpus]{};
    arch::x86_64::vmx::vmcs12 guest_vmcs12[max_cpus]{};

    /**
     * Whether this processor is running the second-level guest rather than
     * the guest hypervisor, and whether the VMCS it runs it with has been
     * launched.
     *
     * The second is not the same as vmcs12's launch state and must not be
     * confused with it. vmcs12's belongs to the guest hypervisor and is
     * what VMLAUNCH and VMRESUME check on its behalf; this one belongs to
     * the real VMCS underneath, which VMCLEAR left clear once and which
     * SDM 29 step 5 makes "launched" only after a VM entry has passed
     * every check - so an entry that fails on guest state leaves it clear
     * and the next attempt must be a VMLAUNCH again.
     * @{
     */
    bool running_l2[max_cpus]{};

    /**
     * How many exits arrived with the interrupted-event field valid,
     * split by which guest was running, and the first few raw values.
     *
     * The processor writes that field when a VM exit interrupts the
     * *delivery* of an event it was in the middle of injecting, and the
     * entry-interruption field it came from has already been cleared by
     * then. So an event that is not re-queued from here is destroyed
     * silently, and from the guest hypervisor's side that is
     * indistinguishable from a delivery that succeeded.
     *
     * KVM re-queues it on every exit - `vmx_complete_interrupts`,
     * `.references/kvm/vmx.c:7488` and `__vmx_complete_interrupts` at
     * `:7105-7157`. This VMM reads the field in exactly two places and
     * re-injects it in none, which is what these counters are here to
     * measure rather than assume.
     * @{
     */
    static constexpr std::size_t idt_vectoring_trace_capacity = 16;
    std::uint64_t idt_vectoring_l1[max_cpus]{};
    std::uint64_t idt_vectoring_l2[max_cpus]{};
    std::uint64_t idt_vectoring_trace[idt_vectoring_trace_capacity]{};
    std::uint64_t idt_vectoring_trace_count{};

    /**
     * The event whose delivery the last VM exit interrupted, held until
     * the next entry to the same guest puts it back.
     *
     * Everything the architecture needs to re-deliver it: the
     * interruption information, the error code when its valid bit says
     * there is one, and the instruction length, which a software
     * interrupt or exception needs and a hardware one ignores.
     * @{
     */
    std::uint64_t pending_event[max_cpus]{};
    std::uint64_t pending_event_error[max_cpus]{};
    std::uint64_t pending_event_length[max_cpus]{};
    std::uint64_t events_requeued[max_cpus]{};

    /**
     * Which guest the held event belongs to, so it is put back into that
     * one and no other.
     *
     * A guest hypervisor's VMLAUNCH is itself an exit, so an exit that
     * interrupted a delivery to *it* can be followed immediately by an
     * entry into its guest. Injecting there would deliver one level's
     * event to the other, which is worse than losing it.
     */
    bool pending_event_l2[max_cpus]{};
    std::uint64_t events_deferred[max_cpus]{};

    /**
     * The two other things that can happen to a held event, counted
     * beside the two that already were.
     *
     * `events_yielded` is an event kept back because the exit's own
     * handler had already staged one - the interrupted event is not
     * lost, it goes in on a later entry. `events_discarded` is one
     * dropped because the second-level guest it was being delivered to
     * is gone, which the guest hypervisor's current VMCS identifies.
     *
     * Counted rather than silent because both are the shapes a leak
     * would take. A `yielded` that climbs without `requeued` following
     * it means events are being held and never delivered; a `discarded`
     * that climbs at all means a guest is being torn down with an event
     * owed to it, which is worth knowing even though dropping it is
     * correct.
     * @{
     */
    std::uint64_t events_yielded[max_cpus]{};
    std::uint64_t events_refused_by_state[max_cpus]{};
    std::uint64_t events_discarded[max_cpus]{};
    /**
     * @}
     */

    /**
     * Which second-level guest a held event was being delivered to,
     * named by the guest hypervisor's current VMCS.
     *
     * A held event is not always re-injected on the next entry - one
     * for a second-level guest waits while its hypervisor runs - so
     * "the same guest" has to mean something across that gap. A VMPTRLD
     * of another region is a different guest, and an event held across
     * that switch is owed to a guest that no longer exists.
     */
    std::uint64_t pending_event_vmcs[max_cpus]{};
    /**
     * @}
     */
    /**
     * @}
     */
    bool vmcs02_launched[max_cpus]{};
    bool l2_entry_logged[max_cpus]{};
    /**
     * @}
     */

    /**
     * The activity state each processor's second-level guest is in, held
     * here rather than read back out of vmcs02.
     *
     * It has to be held somewhere, because vmcs02 is not entered in every
     * state vmcs12 may ask for. The wait-for-SIPI state blocks external
     * interrupts, NMIs, INIT signals and SMIs (SDM 29.7.2,
     * .references/sdm.txt:203152) - only a start-up IPI ends it - so a
     * processor entered in it is one this VMM cannot get back by any means
     * of its own, and its own diagnostic clock is not even in vmcs02 to
     * try with. `enter_or_park_l2` therefore holds that state here and
     * waits for the IPI in root operation instead.
     *
     * KVM keeps the same fact outside the VMCS, in `mp_state`, and
     * reconstructs the field from it on the way out
     * (`sync_vmcs02_to_vmcs12`, .references/kvm/nested.c:4539-4544).
     * `save_l2_state` does the same with this.
     *
     * Read by `start_up_processor`, which is the whole point: it decides
     * whether to hand a guest's start-up IPI over through memory, and a
     * processor parked here is exactly one that is waiting for one.
     */
    volatile std::uint64_t l2_activity_state[max_cpus]{};

    /**
     * How many times this processor has held a second-level guest that
     * was waiting for a start-up IPI rather than entering it.
     *
     * The one number that tells "parked, correctly, and still reachable"
     * apart from "frozen", which is the distinction the bug this exists
     * for could not be read either way round: an application processor
     * with `l2_entries` stuck in the hundreds and no exits looked exactly
     * the same whether it was waiting for something that would come or
     * had been entered in a state nothing could end. A rising count here
     * says the processor is alive and its guest hypervisor's virtual
     * processor is not started yet; a still one beside a still
     * `l2_entries` says something else is wrong.
     */
    volatile std::uint64_t l2_start_up_waits[max_cpus]{};

    /**
     * Where each processor's second-level VMCS is, by physical address,
     * and how many times it has entered and left one.
     *
     * The counters exist for the same reason every other counter in this
     * class does: there is no channel out of a running guest but a
     * debugger, and "did the second level ever run" is the first question
     * anybody asks.
     * @{
     */
    std::uint64_t vmcs02_physical[max_cpus]{};
    std::uint64_t l2_entries[max_cpus]{};

    /**
     * The most recent exits taken *while the second-level guest was
     * running*, per processor, recorded where the reflection happens so
     * the instruction pointer is the second-level guest's own.
     *
     * The ring above cannot answer this. It records every exit at both
     * levels, and the guest hypervisor's own traffic - the VMREADs, the
     * VMWRITEs, the writes to its local APIC page - outnumbers the
     * second-level guest's by enough that by the time a boot has settled
     * the newest thirty-two entries are all the guest hypervisor's.
     * Measured: at the freeze on the rig, every one of the boot
     * processor's newest sixteen entries was a write to the APIC page,
     * and what the *root partition* was doing had long been evicted.
     *
     * Which is the question that matters, because the count of
     * second-level entries stops at about 82,100 in every configuration
     * from two processors to eight and in every build tried, and a stop
     * that reproducible is the second-level guest reaching the same place
     * every time rather than a race.
     * @{
     */
    /**
     * Deeper than the ring above, because the second-level guest's
     * traffic is periodic: measured on the rig, a blocked root partition
     * repeats a seventeen-exit timer cycle - arm, acknowledge, send an
     * interrupt, end the message - and thirty-two slots show one turn of
     * it and nothing else. What is wanted is the part that is *not* the
     * cycle, which only a longer window contains.
     */
    static constexpr std::size_t l2_exit_trace_capacity = 256;

    /**
     * How many *working* second-level exits are kept.
     *
     * A second-level guest that is waiting rather than working spins its
     * idle loop at about a hundred exits a second, so the ring above
     * holds two or three seconds of history and the interesting moment
     * is minutes old. This one drops the loop and keeps everything else.
     *
     * 4096 rather than the 256 beside it, and the arithmetic is the
     * reason: the guest takes about 3,500 device interrupts across the
     * whole of early kernel initialisation before it stops, so a ring
     * that holds fewer records than that cannot contain the phase being
     * investigated. It costs 4096 * 48 bytes per processor - 1.5 MB
     * across eight - against a class this VMM already spends 46 MB on.
     */
    static constexpr std::size_t l2_working_trace_capacity = 4096;
    exit_trace_entry l2_exit_trace[max_cpus][l2_exit_trace_capacity]{};

    /**
     * The same records with the idle loop filtered out, so the last
     * thing a waiting guest actually *did* survives long enough to be
     * read. Filtered on the synthetic MSR indices rather than on
     * instruction pointers, which move with every boot.
     */
    exit_trace_entry l2_working_trace[max_cpus]
                                     [l2_working_trace_capacity]{};
    std::uint64_t l2_working_trace_count[max_cpus]{};
    std::uint64_t l2_exit_trace_count[max_cpus]{};

    /**
     * Which VMCS fields the guest hypervisor reads and writes, and how
     * often.
     *
     * Measured on the rig, VMREAD and VMWRITE together are **71% of every
     * exit this VMM takes** - 35,444 and 12,355 out of 61,490 in the first
     * minute of a Hyper-V boot, against 5,877 exits by the guest it is
     * running. Roughly eight of them per second-level exit, each costing a
     * full round trip through here.
     *
     * VMCS shadowing removes exactly those, but not for free: the fields
     * it shadows have to be copied between this VMM's cached vmcs12 and a
     * real shadow region, and a copy of everything would cost more
     * instructions than it saves. So the set to shadow is the set the
     * guest hypervisor actually touches, and this table is how that set is
     * known rather than guessed - KVM's own list in vmcs_shadow_fields.h
     * is a different hypervisor's measurement of a different guest.
     *
     * A linear table rather than an array indexed by encoding: the
     * encodings are sparse across a 16-bit space, and the count that
     * matters is small. Shared across processors and updated without
     * atomics on purpose - this is a diagnostic, a lost increment costs a
     * count and never correctness, and making it atomic would put a locked
     * operation on the hottest path there is.
     * @{
     */
    static constexpr std::size_t vmcs_field_use_capacity = 128;
    std::uint64_t vmcs_field_read_encoding[vmcs_field_use_capacity]{};
    std::uint64_t vmcs_field_read_count[vmcs_field_use_capacity]{};
    std::uint64_t vmcs_field_write_encoding[vmcs_field_use_capacity]{};
    std::uint64_t vmcs_field_write_count[vmcs_field_use_capacity]{};

    /**
     * Counts encodings that did not fit the table, so a full table reads
     * as a full table rather than as a complete answer.
     */
    std::uint64_t vmcs_field_use_overflow{};

    /**
     * Records one use of a VMCS field by the guest hypervisor.
     */
    void record_vmcs_field_use(bool write, std::uint64_t encoding);
    /** @} */

    /**
     * The second-level guest's RCX at the moment its exit was reflected,
     * kept only long enough for the ring above to record it.
     *
     * For a model-specific register access that is the register number,
     * which is the one thing the exit reason does not carry and the whole
     * question at the point this was added: every root-partition
     * processor halts immediately after a write at the same instruction,
     * and which register that write names decides whether it is arming a
     * deadline it is never given.
     */
    std::uint64_t l2_exit_detail[max_cpus]{};
    std::uint64_t l2_exit_detail_value[max_cpus]{};

    /**
     * Every synthetic MSR the second-level guest touches, counted for
     * the whole run, reads and writes apart.
     *
     * The rings cannot answer this and the difference matters. Both are
     * windows - 256 exits and 4,096 filtered ones - and what is being
     * asked is whether something happened *at all*, once, early, before
     * anything worth keeping had been recorded. Specifically whether the
     * guest ever writes `0x40000021`, the reference TSC page: it reads
     * the reference counter through `0x40000020` about fifteen times per
     * clock tick, and each of those costs a reflection to the guest
     * hypervisor and the resume that follows - about 42% of every exit
     * on this machine, spent reading the clock. A guest with the
     * reference TSC page computes the same value from RDTSC and memory
     * and exits for none of it. Absence from a window proves nothing;
     * absence from a counter that has been up since the module loaded
     * proves it was never offered.
     *
     * Indexed by the low byte, so it covers `0x40000000`-`0x400000ff`,
     * which is every synthetic MSR this guest hypervisor uses.
     */
    /**
     * The last value the second-level guest wrote to `0x40000021`, the
     * reference TSC page, per processor.
     *
     * The census below says the guest writes it - so the page *is*
     * enabled - and then reads the reference counter through
     * `0x40000020` twelve thousand times anyway, which is about 42% of
     * every exit on this machine. The Hyper-V reference TSC page carries
     * a sequence number in its first four bytes, and the protocol is
     * that a sequence of zero means the page is invalid and the guest
     * must fall back to the counter MSR. So the address is kept here to
     * be read from outside: `xp` the low word of this page and a zero
     * says the guest hypervisor withdrew the fast path, which moves the
     * question from "why does the guest not use it" to "why does the
     * level above refuse to offer it".
     *
     * The value is as written, enable bit and all - bit 0 enables and
     * bits 12 and up are the guest-physical page number.
     */
    std::uint64_t l2_reference_tsc_written[max_cpus]{};

    /**
     * The fit of the guest hypervisor's own reference counter against the
     * time stamp counter, and whether the page has been published from
     * it. See `nested_vmx::publish_reference_tsc`.
     */
    std::uint64_t reference_scale[max_cpus]{};
    std::uint64_t reference_offset[max_cpus]{};
    volatile std::uint64_t reference_published[max_cpus]{};
    volatile std::uint64_t reference_fit_error[max_cpus]{};

    /** The first answer seen, kept as a long baseline for the fit. A
     * scale fitted across a 32-entry ring spans milliseconds, and a rate
     * error small enough to pass that check still puts a timer deadline
     * seconds away. See `publish_reference_tsc_page`. */
    std::uint64_t reference_first_tsc[max_cpus]{};
    std::uint64_t reference_first_value[max_cpus]{};

    /**
     * Fits that pair from the answers already recorded in
     * `reference_read_value` and `reference_read_tsc`, checks the fit
     * against a later sample, and only then writes the page and makes its
     * sequence non-zero. Called on entry; does nothing until it can.
     */
    void publish_reference_tsc_page(std::size_t cpu);

    std::uint32_t l2_synthetic_msr_reads[max_cpus][256]{};
    std::uint32_t l2_synthetic_msr_writes[max_cpus][256]{};
    /**
     * @}
     */
    /**
     * Set by `reflect_l2_exit` and consumed by `record_exit`, which is
     * the only pair that needs it: it is how the ring knows whose
     * instruction pointer it just read. See `exit_trace_entry::reflected`.
     */
    std::uint64_t exit_reflected[max_cpus]{};

    std::uint64_t l2_exits_reflected[max_cpus]{};
    std::uint64_t l2_exits_handled[max_cpus]{};
    /**
     * @}
     */

    /**
     * Where a failed VM entry into the second-level guest comes back to,
     * and what it came back with.
     *
     * `nested_entry_recovery` is captured immediately before the entry is
     * decided on and restored by `zpp_vmx_nested_entry_failure`, which is
     * the only way back: a failed VM entry produces no VM exit, so the
     * ordinary exit path never runs and the processor is left in host mode
     * with a guest stack. The address of the context is handed to the
     * failure stub through a VMCS field - see
     * `nested_entry_recovery_field`.
     *
     * `nested_entry_failed` is what tells the two arrivals apart, in
     * memory rather than in a register because the second arrival restores
     * every register to what the first left. Same shape as the flag
     * `vm_launch` uses for the same trick.
     * @{
     */
    arch::x86_64::context
        nested_entry_recovery[nested_vmx::enabled ? max_cpus : 1]{};
    std::atomic<bool> nested_entry_failed[max_cpus]{};
    std::uint64_t nested_entry_error[max_cpus]{};

    /**
     * Whether a VMLAUNCH or VMRESUME has already decided where the guest
     * hypervisor's RIP goes, so that the exit handler does not advance it.
     *
     * Two outcomes set it and neither is a VMfail: the second-level guest
     * is about to run and RIP belongs to it now, or its entry failed after
     * loading guest state and the guest hypervisor has been put back at
     * its own host RIP. Only a VMfail leaves RIP to be advanced past the
     * instruction, which is what the architecture does with it.
     */
    bool nested_rip_settled[max_cpus]{};

    /**
     * The MSR-load area failure that produces an entry-failure exit rather
     * than a VMfail, and the entry number SDM 29.8 puts in its exit
     * qualification - "1 for the first entry, 2 for the second, etc.".
     * @{
     */
    bool nested_msr_load_failed[max_cpus]{};
    std::uint64_t nested_msr_failure_entry[max_cpus]{};
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * IA32_FEATURE_CONTROL as the guest sees it.
     *
     * Answered rather than passed through, because the hardware value is
     * locked by the time any guest runs - this VMM's own
     * enable_vmx_in_feature_control sets the lock bit during launch, and
     * the register is write-once per reset. A guest hypervisor reading
     * the real one would find it locked with settings it did not choose
     * and could not change, and a guest that tries to write it would
     * either fault or silently fail. So the guest gets its own copy, and
     * its own write-once semantics on that copy.
     */
    std::uint64_t guest_feature_control[max_cpus]{};
    /**
     * @}
     */

    /**
     * Whether this processor has already had a VMX instruction exit
     * recorded in the log, and how many it has taken.
     *
     * One line per processor for the same reason as monitor_logged: a
     * guest hypervisor probing for VT-x retries, and a line each would
     * bury everything else. The count keeps the information the
     * suppressed lines carried - whether the guest tried once and stood
     * down or is spinning on a feature it has been told it does not have,
     * which are the two things worth telling apart here.
     * @{
     */
    bool vmx_instruction_logged[max_cpus]{};
    std::uint64_t vmx_instructions_refused[max_cpus]{};
    /**
     * @}
     */

    /**
     * The first stack a processor started by the trampoline has, used only
     * until its launch switches to the one reserved for its index.
     *
     * One is enough, and shared on purpose: only one processor is ever
     * being started at a time, which start_up_lock is what guarantees.
     *
     * Worth being exact about the extent of that guarantee, because the
     * lock is held on the *other* processor. A target stops using this
     * stack inside `launch_on_cpu`, which copies its context onto the
     * per-processor stack and switches - long before `start_up_launched`
     * is set and therefore well inside the window the sender holds the
     * lock for. The success path is covered with room to spare. The two
     * that are not: the sender's bounded wait can expire while a slow
     * target is still on this stack, and a target whose `main` *fails*
     * returns onto this frame to log and halt.
     */
    alignas(page_size) std::uint8_t start_up_stack[0x4000]{};

    /**
     * The error each processor's launch failed with, or zero.
     *
     * A processor this VMM started cannot report its own failure: it was
     * reached by an inter-processor interrupt rather than called, so there
     * is nothing to return an error to and nothing left to do with it but
     * stop. The code is recorded here on the way past instead, and read
     * back through the diagnostic CPUID leaf by a processor that is still
     * running.
     */
    std::uint64_t launch_error[max_cpus]{};

    /**
     * Set by a processor being started once it is running the guest, so
     * that the processor that started it knows it succeeded.
     */
    std::atomic<bool> start_up_launched[max_cpus]{};

    /**
     * Serializes starting a processor, because bringing one up walks
     * through shared state - the stack index, the virtual processor
     * counter, and the VMX region pointers - that is only correct for one
     * processor at a time.
     *
     * It is held by the *sender* across the target's entire launch, which
     * is what makes it cover the target too: `start_application_processor`
     * does not release it until `start_up_launched[slot]` is set, and that
     * is written at the very end of `main`, after the target has left the
     * shared trampoline stack and taken its stack index. The
     * serialisation is therefore real but **indirect**, and it has
     * exactly one hole: the wait is bounded, so a target that is only
     * slow is still using all of that when the lock is released. See the
     * note on the timeout in `start_application_processor`. Nothing new
     * may be handed between processors this way - the VMXON and VMCS
     * region addresses were, and that is the defect this note exists to
     * stop being repeated.
     *
     * It also guards `processor_slot`, which hands out the index every
     * per-processor array is addressed by. That is the same resource, not
     * a second one - a slot must not be handed out while somebody is
     * launching into it.
     *
     * Not recursive. Nothing that holds it may reach `processor_slot` or
     * `start_application_processor`, and today nothing does.
     */
    spin_lock start_up_lock{};

    /**
     * The virtual address the temporary mapping window uses.
     *
     * Chosen rather than allocated, because the host page table's storage
     * is fixed at compile time and aliases: two addresses agreeing in bit
     * 38 and in bits 29:12 share a leaf, and the second mapping silently
     * replaces the first. So a window address has to be checked against
     * everything else this table maps rather than picked freely.
     *
     * This one has bit 38 clear and bits 29:12 of 0x10000, against the
     * module's 0x38ddd, the queue storage's 0x3f72b, and the controller
     * BARs which have bit 38 set. Nothing in the host address space lives
     * here otherwise - the table maps itself, the module, and the two
     * device regions.
     */
    static constexpr std::uint64_t mapping_window = 0x10000000;

    /**
     * How many pages the window spans.
     *
     * A guest's admin submission queue is up to 256 entries of 64 bytes,
     * which is four pages, and its completion queue 256 of 16, which is
     * one. Eight covers both with room, mapped as two separate runs, and
     * the borrow needs live access across the whole of each - it writes
     * commands into successive submission slots and reads completions
     * back, so copying is not an option.
     *
     * Every page of it has to clear the aliasing check the address itself
     * did: bits 29:12 run 0x10000 to 0x10007, none of which collide with
     * the module or the queue storage.
     */
    static constexpr std::size_t queue_window_pages = 8;

    /**
     * Two pages per processor, past the queue's eight, for reading the
     * instruction that caused an EPT violation.
     *
     * Per processor and not shared, so the read needs no lock on a path
     * that runs for every write to a watched page - the local APIC page
     * is watched whenever the guest is not in x2APIC mode, and that page
     * is hot. Two because an instruction may straddle a page boundary;
     * fifteen bytes is the architectural maximum and cannot span more.
     */
    /**
     * Whether a write to a watched page is emulated from the decoded
     * instruction, or allowed to execute by opening the page for one
     * single step.
     *
     * Emulating is the better scheme and the reason the decoder exists:
     * stepping leaves the page writable for every processor for the
     * duration, and a second write slips through that window unobserved,
     * which is how a controller reset went unnoticed.
     *
     * Switched **on**. It was off because the emulation could not fetch
     * the instruction it was emulating, and four defects followed from
     * that; all four are now closed and each has a check behind it.
     *
     * It was *not* off because of the guest hang on the real rig - that
     * hang was measured to happen identically with this false, and with
     * the whole diagnostic channel compiled out, so the two are
     * unrelated. Said plainly because a previous version of this comment
     * blamed the hang and was wrong.
     *
     * What was fixed, so that a regression in any of them is recognisable
     * rather than mysterious:
     *
     * - The instruction is fetched by walking `vmcs.guest_cr3()` at exit
     *   time, in `translate_guest_linear`. It used to go through
     *   `os_page_table`, built once from the launch-time CR3, which is
     *   only right while the guest is on the firmware's identity map -
     *   after that a kernel linear address was used as a physical one.
     * - A store sourced from encoding four without REX.R is refused.
     *   `context::rsp` is the *host* stack pointer, stored there
     *   deliberately by the exit stub, so reading it would write a
     *   hypervisor stack address into a device register and hand a
     *   protected module address to the guest.
     * - RIP advances by the length the decoder measured. The VMCS field
     *   is undefined for this exit (SDM 30.2.5) and KVM never reads it.
     *   Where the processor does report one and the two disagree, the
     *   decoder has misread an instruction whose store is already applied
     *   - that is recorded in `emulated_length_disagreement` and the
     *   processor is stopped rather than resumed at either address.
     * - The exit qualification is consulted: only an access by the
     *   instruction's own operand is emulated, never the processor
     *   walking a paging structure. A store straddling the watched page's
     *   end is refused rather than applied whole at the faulting address.
     *
     * The fallback is unchanged and still correct: anything refused above
     * is stepped over as before, which reopens the window this closes but
     * is never wrong.
     */
    static constexpr bool emulate_watched_page_writes = true;

    /**
     * Whether a watched page's writes are carried out through the full
     * instruction decoder or through the narrow store-only one.
     *
     * **Off, and the reason it is off is unresolved rather than
     * understood.** Switching the exit path to the full decoder makes the
     * guest triple fault - exit reason 2 - deterministically, at the same
     * guest RIP, after the same 179 emulations, in early boot.
     *
     * What has been ruled out by measurement, so that the next person does
     * not repeat it:
     *
     * - It is not the decoder's added coverage. A trace of every emulation
     *   that was *not* a plain store recorded **zero** entries, so on this
     *   workload the full decoder accepted exactly what the narrow one
     *   accepted, and the extra opcodes were never exercised.
     * - It is not the status flags. Computing them fixed nothing, and
     *   skipping the write for stores - which set none - fixed nothing
     *   either.
     * - It is not the interrupt command being issued twice. That is a real
     *   defect on this path, fixed separately, and it did not change this.
     * - It is not the destination-width bug in the widening moves. That
     * was real, is fixed, and did not change this.
     *
     * All 179 emulations are the same instruction: `mov [rbx], r12d`
     * storing zero to the local APIC page, which is an end-of-interrupt
     * write and is ordinary traffic - and which the narrow decoder answers
     * identically on a build that boots. So the difference is in this
     * VMM's handling around the decode rather than in the decode, and
     * finding it needs a comparison of the two paths' effects on the same
     * instruction rather than another hypothesis.
     *
     * The decoder itself is kept, tested, and unused by the exit path. Its
     * value does not depend on this switch: `check-instruction.sh` proves
     * what it answers, and the eventual fix for the APIC page is more
     * likely to be hardware APIC-access virtualization than any decoder,
     * which is what a comparable bare-metal implementation relies on.
     */
    static constexpr bool decode_watched_page_fully = false;

    /**
     * Reads the word a decoded instruction is about to act on.
     *
     * Only the forms that need the old contents ask for it - a plain store
     * must not, because a device register that reads back differently from
     * what was written is precisely the case a watch exists to observe.
     */
    std::optional<std::uint64_t>
    read_guest_word(std::uint64_t guest_physical, std::uint8_t size);

    /**
     * Carries out a decoded instruction against guest memory, and reports
     * what it did.
     *
     * The counterpart to the pure decoder: it supplies the current
     * contents that `apply` needs, writes the result back where the
     * instruction changes memory, puts any register result in place, and
     * fills in what the watch handler is told.
     */
    bool carry_out_guest_instruction(
        std::uint64_t guest_physical,
        const arch::x86_64::decoded_instruction & instruction,
        arch::x86_64::context & context,
        guest_write & performed,
        bool & changed_memory,
        std::optional<std::uint64_t> known_contents = {});

    static constexpr std::size_t instruction_window_pages_per_cpu = 2;

    /**
     * One shared pair rather than a pair per processor.
     *
     * A slot per processor needed the window to span seventy-two pages,
     * and the window's address is not freely chosen: the host page
     * table's storage is fixed and aliases, so every page of it has to
     * be checked against everything else the table maps. Growing it
     * eightfold is exactly the sort of change that quietly breaks that
     * on one machine and not another, and enlarging it coincided with
     * the hypervisor no longer initialising on the real rig. Ten pages
     * keeps the window the size the aliasing argument was made about.
     *
     * The cost is that instruction reads serialise on
     * mapping_window_lock, which is real on the local APIC page because
     * that page is hot. That is the right trade until the window's
     * addressing is understood well enough to grow it deliberately.
     */
    static constexpr std::size_t
    instruction_window_first_page(std::size_t cpu)
    {
        static_cast<void>(cpu);
        return queue_window_pages;
    }

    static constexpr std::size_t mapping_window_pages =
        queue_window_pages + instruction_window_pages_per_cpu;

    /**
     * The leaf page-table entry that translates each window page, found
     * once and then written directly.
     *
     * **This is the whole of what made repointing the window cost about
     * eleven hundred cycles.** `page_table::map_page` forwards to
     * `map_page_from`, which rewrites all four levels and resolves three
     * of them with `virtual_to_physical` - and *that* is itself a
     * software walk, so one repoint of one page was three nested walks
     * of this VMM's own tables plus four read-modify-writes plus the
     * INVLPG. Measured at 14.0% of everything this VMM does, across
     * 145,222,992 calls in one boot.
     *
     * All of it except the leaf write and the INVLPG is recomputing a
     * constant. The window's virtual addresses are fixed at compile
     * time, so the entries above the leaf hold the same values on every
     * call - `map_page_from`'s own comment says it rewrites them
     * "rather than tested, because ... pointing an entry at the table it
     * already holds is idempotent and cheaper than the branch that would
     * skip it", which is true of one call and false of a hundred million.
     *
     * Filled on the first mapping of each page, which still goes through
     * `map_page` and so still establishes every level. A null slot means
     * "not yet mapped", and the two counters below say which path ran,
     * because a cache nobody has watched hit is one that may not be
     * hitting.
     * @{
     */
    arch::x86_64::pte * window_entry[mapping_window_pages]{};
    volatile std::uint64_t window_entry_fast{};
    volatile std::uint64_t window_entry_slow{};
    /**
     * @}
     */

    /**
     * Where read_guest_physical and its siblings point the window.
     *
     * The same page as the instruction window, deliberately, and the
     * window is not grown for it. The header above records why growing it
     * is not a small change - the window's address is not freely chosen,
     * every page has to be checked against the aliasing the host page
     * table's fixed storage produces, and enlarging it once coincided
     * with the hypervisor no longer initialising on the real rig.
     *
     * Sharing is safe because both uses hold mapping_window_lock for the
     * whole of the bytes they read through it, and neither is reachable
     * from inside the other - the lock is not recursive, so that is a
     * requirement rather than an observation. One page rather than two
     * because these copy a page at a time and re-point the window for
     * each, so a copy straddling a boundary needs no second page.
     */
    static constexpr std::size_t transfer_window_first_page =
        queue_window_pages;

    /**
     * Serialises the window, which is one address shared by every
     * processor. Held across the whole use, not just the mapping, because
     * the point of the window is the bytes reached through it.
     */
    spin_lock mapping_window_lock{};


    /**
     * What the window self check found: 0 not run, 1 correct, 2 wrong.
     *
     * A member rather than a log line because the log is not on the wire
     * - it lives in memory for a debugger - and this is a fact worth
     * reading with `xp` from the monitor while the guest runs.
     */
    std::uint64_t mapping_window_verified{};

    /**
     * What the disk channel needs to rebuild its queue pair after the
     * guest has reset the controller.
     *
     * Captured when the channel is first configured, because by the time
     * it is needed the channel has forgotten its binding - that is what
     * forgetting means - and the loader that supplied all of it is long
     * gone.
     * @{
     */
    volatile void * channel_bar{};

    /**
     * Whether the controller was enabled last time its configuration
     * register was written. The rebuild is triggered by the transition
     * back to enabled, not by the register being written, so the previous
     * value has to be remembered.
     */
    bool channel_controller_enabled{};

    /**
     * Whether a rebuild is in progress, and how often one was refused
     * because another was.
     *
     * The enable is observed both from the emulated write to the
     * configuration register and from this VMM's own poll, so two
     * processors can reach the rebuild for the same transition. The
     * second must not run: it borrows the guest's admin queue after the
     * driver is live, which is the case that was measured timing out.
     *
     * The count is instrumentation - a reader that finds it non-zero
     * knows the race is real on this machine rather than theoretical.
     * @{
     */
    std::atomic<bool> channel_rebuild_running{};
    volatile std::uint64_t channel_rebuild_reentered{};
    /**
     * @}
     */

    /**
     * How many times the controller's register page has been written by
     * the guest, and what the configuration register said the last time.
     *
     * Instrumentation, because the evidence available without it was
     * consistent with two incompatible stories: the watch is armed, the
     * entry denies writes, the handler is the right one, and the
     * controller ended up enabled without the transition ever being
     * seen. One of those observations has to be wrong and this says
     * which.
     * @{
     */
    /**
     * How long the preemption timer runs between checks, as a count of
     * whatever unit the processor scales it to.
     *
     * The timer counts at a rate proportional to the time stamp counter,
     * divided by 2^n where n is IA32_VMX_MISC[4:0], so the unit differs
     * between machines and the value is computed from that rather than
     * chosen. Ten microseconds: short enough to land well inside the
     * CSTS.RDY wait a driver must already tolerate, long enough that the
     * exits it costs are not noticeable, and only while the channel is
     * down at all.
     */
    static constexpr std::uint64_t controller_poll_microseconds = 1000;

    /**
     * Whether the preemption timer is currently armed, so it is not
     * re-armed on every exit or left running once its reason is gone.
     */
    /**
     * Whether each processor has the preemption timer armed.
     *
     * One entry per processor, because the control it tracks lives in
     * that processor's VMCS - see arm_controller_poll for what a single
     * shared flag did instead.
     */
    bool controller_poll_armed[max_cpus]{};

    /**
     * Why an acknowledgement wait gave up, for reading with `xp`.
     *
     * Three plausible explanations for a refused acknowledgement were
     * wrong in a row, which is the point at which guessing should stop.
     * @{
     */
    std::uint64_t ack_target{};
    std::uint64_t ack_launched_mask{};
    std::uint64_t ack_outstanding_cpu{};
    std::uint64_t ack_outstanding_seen{};
    /**
     * @}
     */

    std::uint64_t channel_register_writes{};
    std::uint64_t channel_last_configuration{};
    /**
     * @}
     */
    std::uint32_t channel_doorbell_stride{};
    std::uint16_t channel_queue_id{};
    std::uint32_t channel_namespace{};
    std::uint64_t channel_submission_physical{};
    std::uint64_t channel_completion_physical{};
    /**
     * @}
     */

    /**
     * How the rebuild went, for reading with `xp` while the guest runs:
     * how many were attempted, and the borrow_result of the last one.
     * @{
     */
    std::uint64_t channel_rebuilds{};
    std::uint64_t channel_rebuild_result{};
    std::uint64_t channel_rebuild_ticks{};

    /**
     * The reservation and the creation that spends it, as separate
     * records.
     *
     * Separate because they happen milliseconds apart, in different exits,
     * and either can fail while the other worked - a reservation that was
     * refused and a creation that never became eligible look identical
     * from the medium, which is nothing at all. All volatile: nothing in
     * this program reads any of them, so without it the stores are dead
     * and the optimizer is entitled to remove them, which is exactly what
     * happened to configure_reject once.
     * @{
     */
    /**
     * Non-zero once the reservation has been attempted this epoch, and
     * how it went: zero while untried, 1 for a reservation that took,
     * otherwise the refusal code borrow_guest_admin_queue returned or
     * 0xe0 for a Set Features the controller refused.
     */
    volatile std::uint64_t channel_reserve_result{};

    /**
     * The status and DW0 of our own Set Features, exactly as the
     * controller answered. DW0 is the whole answer - NSQA in bits 15:0
     * and NCQA in 31:16, both zero's based - and the status only says
     * whether the feature was accepted at all.
     * @{
     */
    volatile std::uint64_t channel_reserve_status{};
    volatile std::uint64_t channel_reserve_allocation{};
    /**
     * @}
     */

    /**
     * The allocation as real counts, which is what every comparison here
     * is against. Zero means no reservation is in force, and nothing may
     * be created.
     * @{
     */
    std::uint32_t channel_allocated_submission_queues{};
    std::uint32_t channel_allocated_completion_queues{};
    /**
     * @}
     */

    /**
     * What the guest asked the controller for, read off its own Set
     * Features (Number of Queues) rather than out of the completion. Real
     * counts. Zero until it has issued one.
     * @{
     */
    volatile std::uint64_t channel_guest_requested_submission_queues{};
    volatile std::uint64_t channel_guest_requested_completion_queues{};
    /**
     * @}
     */

    /**
     * The highest identifier the guest has actually created in each
     * space, and how many it has created. Two spaces, because they are
     * two spaces: measured on the rig, the guest creates completion
     * queues 1 to 8 and submission queues 1 to 16.
     * @{
     */
    volatile std::uint64_t channel_guest_highest_submission_queue{};
    volatile std::uint64_t channel_guest_highest_completion_queue{};
    volatile std::uint64_t channel_guest_created_submission_queues{};
    volatile std::uint64_t channel_guest_created_completion_queues{};
    /**
     * @}
     */

    /**
     * Whether the guest has deleted an I/O queue this epoch.
     *
     * A driver deleting queues is tearing the controller down, and
     * creating one behind it at that point is the worst possible moment.
     * Once this is set nothing is created until the next enable.
     */
    volatile std::uint64_t channel_guest_deleted_queues{};

    /**
     * How the creation went: zero while untried, 1 for a queue pair that
     * exists, otherwise the refusal code, 0xe1 for a Create the
     * controller refused, or 0xe2 for an allocation with no room above
     * what the guest took.
     * @{
     */
    volatile std::uint64_t channel_create_result{};
    volatile std::uint64_t channel_create_status{};
    volatile std::uint64_t channel_create_ticks{};
    /**
     * @}
     */

    /**
     * The identifiers chosen for our own queue pair, in the two spaces.
     * @{
     */
    std::uint16_t channel_created_submission_id{};
    std::uint16_t channel_created_completion_id{};
    /**
     * @}
     */

    /**
     * Whether the doorbell page is watched right now.
     *
     * Armed at the enable and removed once the queue pair exists or has
     * been given up on, which is what NVME-LOG.md prescribes and what
     * keeps the steady state free: with a doorbell stride of zero every
     * I/O doorbell shares that page, so a permanent watch is an exit per
     * disk command.
     */
    bool channel_doorbell_watched{};

    /**
     * The admin submission queue tail the guest last published, which is
     * the value its own doorbell write carried.
     *
     * Kept because a doorbell cannot be read back - PCIe Transport 1.0c
     * 3.1.2.1, "if a doorbell register is read, the value returned is
     * vendor specific" - and a borrow needs it: SQHD equal to this is
     * what says the controller has fetched everything the guest wrote,
     * and therefore that no unfetched entry is about to be overwritten.
     */
    std::uint32_t channel_expected_admin_tail{};

    /**
     * What the quiescence wait was looking at when it gave up. Only
     * meaningful when a result code says it did.
     * @{
     */
    volatile std::uint64_t channel_quiesce_submission_tail{};
    volatile std::uint64_t channel_quiesce_expected_tail{};
    /**
     * @}
     */

    /**
     * How the guest's Number of Queues answer was edited, and what it
     * said before and after.
     *
     * Zero while untried, 1 for an answer that was reduced, 2 for one
     * that needed no reduction because the guest asked for less than the
     * allocation in both spaces, otherwise a refusal code:
     *
     * - 0xfb, there is no controller;
     * - 0xf9, no reservation is in force, so there is no allocation to
     *   hold anything back from and the controller's answer is the truth;
     * - 0xf2, the admin completion queue's geometry is not one this can
     *   reach;
     * - 0xf3, the mapping window could not reach it;
     * - 0xf4, the completion never arrived inside the budget;
     * - 0xf5, too many other completions arrived before it;
     * - 0xe6, the controller refused the guest's own Set Features, so
     *   there is nothing to edit and the guest has an error to handle;
     * - 0xe7, the grant is one queue, and taking the guest's only queue
     *   is not a trade this makes;
     * - 0xe8, the answer the guest was given is not the allocation the
     *   reservation was granted, which falsifies the ordering the whole
     *   scheme rests on - see NVMe Base 5.2.30.1.5 and the check itself.
     *
     * channel_grant_already_posted says which side of the race this was
     * on: 1 means the controller had answered before the doorbell's exit
     * handler got to look, 0 means it was waited for. Neither is a fault
     * - both are handled - but it is the measurement that says how much
     * of a race it actually is on this controller.
     *
     * All volatile: nothing in this program reads them, so without it the
     * stores are dead and the optimizer may remove them.
     * @{
     */
    volatile std::uint64_t channel_grant_result{};
    volatile std::uint64_t channel_grant_reported{};
    volatile std::uint64_t channel_grant_presented{};
    volatile std::uint64_t channel_grant_scanned{};
    volatile std::uint64_t channel_grant_already_posted{};
    volatile std::uint64_t channel_grant_ticks{};
    /**
     * @}
     */

    /**
     * What the guest was told it was granted, as real counts, in the two
     * spaces. This is what its creates are clamped by, so it is what
     * decides the identifier above them - see queue_limit. Zero until an
     * answer has been seen, which is also what a build with
     * diag::reduce_guest_queue_grant off leaves them at.
     * @{
     */
    std::uint32_t channel_guest_granted_submission_queues{};
    std::uint32_t channel_guest_granted_completion_queues{};
    /**
     * @}
     */

    /**
     * The command identifier the guest put on its own Set Features
     * (Number of Queues), and whether one is waiting to be answered.
     *
     * Taken off the submission entry rather than guessed, because it is
     * the only way to recognise that command's completion among whatever
     * else the controller posts in the same window.
     * @{
     */
    std::uint16_t channel_grant_command_id{};
    bool channel_grant_pending{};
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * Where the guest's admin queue is copied to and compared against
     * across a borrow.
     *
     * Sized for the deepest queue the borrow will accept, because the
     * guest picks the depth and picks it again on every reset. Twenty
     * kilobytes of .bss that a build without the channel does not carry -
     * it is only reachable from the rebuild, which is only reachable from
     * the channel being configured.
     * @{
     */
    nvme::submission_entry
        channel_snapshot_submission[nvme::admin_borrow::max_depth]{};
    nvme::completion_entry
        channel_snapshot_completion[nvme::admin_borrow::max_depth]{};
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * The last sleep state the guest asked for, and what was done about
     * it.
     *
     * A record rather than a log line because of where it has to be
     * readable from: after an S3 the machine has been through a reset, so
     * the channel's last block may be the only thing that got out, and
     * after a *failed* suspend the interesting question is what this VMM
     * had already taken apart by the time the write did not sleep. Both
     * want a single place holding the whole of the last attempt.
     */
    struct
    {
        /**
         * Non-zero once a sleep request has been seen. Checked first:
         * every other field is meaningless until this is set.
         */
        std::uint64_t occurred{};

        /**
         * Which of the two control blocks the guest wrote, and the whole
         * value it wrote there.
         * @{
         */
        std::uint64_t port{};
        std::uint64_t value{};
        /**
         * @}
         */

        /**
         * The SLP_TYPx field out of that value. Platform specific and
         * recorded only - see power.h on why it is not compared against a
         * constant.
         */
        std::uint64_t sleep_type{};

        /**
         * Which processor saw the write, as a VPID.
         */
        std::uint64_t processor{};

        /**
         * How far the quiesce got before the OUT was executed, so a
         * machine that never came back says where it stopped. One of
         * power_stage below.
         */
        std::uint64_t stage{};
    } sleep_request{};

    /**
     * The steps on_sleep_request records in sleep_request.stage, in the
     * order it reaches them. A machine that suspends and never resumes
     * leaves the last one it got past.
     */
    enum class power_stage : std::uint64_t
    {
        seen = 1,
        channel_flushed = 2,
        vmcs_cleared = 3,
        left_vmx_operation = 4,
        write_issued = 5,
        write_returned = 6,
        re_established = 7,
    };

    /**
     * How far a resume got, and what it was resuming into.
     *
     * The counterpart of sleep_request, and it exists for a sharper reason
     * than symmetry: a resume that does not finish leaves a machine with
     * no console, no guest and nothing running, so the only account of it
     * is whatever was written to memory before it stopped - and read
     * either from the channel's last block or by a debugger on the next
     * boot. The trampoline's own account of the climb is separate and
     * coarser; see start_up_trampoline_stage.
     */
    struct
    {
        /**
         * Non-zero once a resume has been attempted. Checked first: every
         * other field is meaningless until this is set.
         */
        std::uint64_t occurred{};

        /**
         * How many times this VMM has come back through the waking vector,
         * which is the number a slot leak would show up in.
         */
        std::uint64_t count{};

        /**
         * One of resume_stage. A machine that never came back leaves the
         * last one it got past.
         */
        std::uint64_t stage{};

        /**
         * Where the guest was entered, which is its own waking vector and
         * not the trampoline's.
         */
        std::uint64_t guest_vector{};
    } resume_request{};

    /**
     * The steps the resume records in resume_request.stage, in the order
     * it reaches them.
     *
     * `entered` is written by the first C++ the resuming processor
     * executes, so anything less than it means the failure was in the
     * trampoline or before it - in the firmware's own resume, or in the
     * vector this VMM wrote into the FACS - and start_up_trampoline_stage
     * says which.
     */
    enum class resume_stage : std::uint64_t
    {
        armed = 1,
        entered = 2,
        disarmed = 3,
        rewound = 4,
        launched = 5,
    };

    /**
     * The exit nothing knew how to handle, filled in by
     * on_unhandled_exit just before it stops the CPU. For a debugger, and
     * for the same reason as vm_entry_failure below: none of it can be
     * recovered afterwards, because reading a VMCS field needs the VMCS
     * to still be current on this CPU.
     */
    struct
    {
        /**
         * Non-zero once an unhandled exit has been recorded. Checked
         * first: every other field is meaningless until this is set.
         */
        std::uint64_t occurred{};

        /**
         * The exit reason. Its low bits name which exit it was.
         */
        std::uint64_t reason{};

        /**
         * The exit qualification, whose meaning depends on the reason -
         * for an EPT violation it says what kind of access faulted.
         */
        std::uint64_t qualification{};

        /**
         * The guest linear address, meaningful for the exits that report
         * one, which is what an EPT violation needs to be understood.
         */
        std::uint64_t guest_linear_address{};

        /**
         * Where the guest was, so the offending instruction can be found.
         */
        std::uint64_t guest_rip{};
        std::uint64_t guest_cs_selector{};
    } unhandled_exit{};

    /**
     * Everything known about a VM entry that failed, filled in by
     * on_vm_entry_failure just before it stops the CPU.
     *
     * This exists to be read by a debugger. A failed entry cannot be
     * reported through a return value - the frame that could have returned
     * one is gone by the time the guest is running - and none of it can be
     * recovered afterwards either, because reading a VMCS field needs the
     * VMCS to still be current on this CPU. So it is captured at the one
     * moment it is all available.
     */
    struct
    {
        /**
         * Non-zero once a failure has been recorded. Checked first: every
         * other field is meaningless until this is set.
         */
        std::uint64_t occurred{};

        /**
         * The full exit reason, bit 31 included, whose low bits say which
         * class of failure it was - invalid guest state, MSR loading, or a
         * machine check during entry.
         */
        std::uint64_t reason{};

        /**
         * The exit qualification, which for an invalid guest state names
         * the specific offender for a few cases the processor can be
         * precise about.
         */
        std::uint64_t qualification{};

        /**
         * The VM instruction error. Set when an entry instruction fails
         * outright rather than exiting, so usually stale here - kept
         * because the two failure paths are easy to confuse and seeing
         * both values distinguishes them.
         */
        std::uint64_t instruction_error{};

        /**
         * The guest state that was rejected. These are the fields a
         * real-mode entry gets wrong, and the only way to see them is to
         * read them out while the VMCS is still current.
         */
        std::uint64_t activity_state{};
        std::uint64_t interruptibility_state{};
        std::uint64_t entry_controls{};
        std::uint64_t guest_cr0{};
        std::uint64_t guest_cr4{};
        std::uint64_t guest_rflags{};
        std::uint64_t guest_rip{};
        std::uint64_t guest_cs_selector{};
        std::uint64_t guest_cs_base{};
        std::uint64_t guest_cs_access_rights{};

        /**
         * Which processor failed, and what it believed about itself.
         *
         * Absent until it cost an hour. The record said the guest CS was
         * unusable and the guest RIP was inside this module, which is the
         * state `setup_vmcs` captures from a starting processor's own C
         * frame before `apply_start_up` overwrites it - so the reading was
         * "the start-up state was never applied". Whose it was, and
         * whether that processor thought it had come from the trampoline
         * at all, could only be inferred from the order of log lines.
         *
         * `virtual_processor` is recorded beside `cpu` because the two are
         * indexed independently - `apply_start_up`'s own guard uses
         * `vpid() - 1` while everything around it uses the slot - and they
         * coincide only while processors are launched in slot order.
         * @{
         */
        std::uint64_t cpu{};
        std::uint64_t virtual_processor{};
        std::uint64_t from_trampoline{};
        std::uint64_t start_up_vector{};
        /**
         * @}
         */
    } vm_entry_failure{};

    /**
     * The context to unwind to when the host IDT catches an exception,
     * captured by main once the host page table is live.
     */
    arch::x86_64::context host_exception_recovery{};

    /**
     * The flag in main's frame that says the recovery context above was
     * used, or null while there is no recovery point to unwind to.
     *
     * Only one processor is inside that window at a time, but for a
     * different reason on each platform, and the single reason this
     * comment used to give was wrong on one of them:
     *
     * - The Windows and Linux loaders loop `call_on_cpu(i, ...)`,
     *   blocking on each, so they genuinely launch one after another.
     * - Under UEFI they do not, because `number_of_cpus()` returns 1 and
     *   only the boot processor is ever launched from the loader at all.
     *   The others are adopted later from the guest's own start-up IPIs,
     *   which enter through `start_up_on_this_processor` rather than
     *   through main's recovery window.
     *
     * So the slot being shared is safe today on both, and it is safe by
     * coincidence of two unrelated facts rather than by design. What
     * would break it is a loader that launches processors concurrently:
     * a second fault would overwrite the first one's record, and the
     * first processor would unwind to a context that is no longer its
     * own. That is BACKLOG item 10.
     */
    std::atomic<bool> * host_exception_recovery_flag{};

    /**
     * The data pointed to by the FS register to be used by
     * the host VMM.
     */
    alignas(page_size) std::uint8_t fs_data[page_size]{};

    /**
     * The data pointed to by the GS register to be used by the host VMM,
     * **one page per processor**, with that processor's index in the
     * first quadword.
     *
     * It was a single shared page and nothing read it: `host_gs_base` has
     * to name a canonical address because the VMCS requires one, and this
     * page existed only to be that address. Nothing in this tree reads
     * through GS - there is no `gs:` access anywhere - so the sharing was
     * safe by never being used rather than by design.
     *
     * Per processor now, because the VMCS's `host_gs_base` is host state
     * and therefore already per processor: point each processor's at its
     * own row and **"which processor am I" becomes one memory access**,
     * `arch::x86_64::gs_qword(host_gs_processor_index)`. The alternative
     * this tree uses everywhere else is `vmcs.vpid()`, which is a VMREAD,
     * and a VMREAD traps to the level above at ~2,760 cycles whenever
     * this VMM is itself a guest - which rules it out of anything called
     * often. `map_window_at` is called 48 million times in a boot.
     *
     * Written in `setup_vmcs`, which runs once per processor and already
     * has the index, and beside the `host_gs_base` write so the two
     * cannot drift. That is also the only place `host_gs_base` is
     * written - application processors reach it through the same
     * function - so there is no second path to keep in step.
     *
     * **It has no caller on a hot path today, and it is kept anyway.**
     * The `map_window_at` elision it was built for was measured and
     * reverted (BACKLOG), but the mechanism itself was proven correct in
     * that boot - per-processor counters indexed by it landed entirely
     * on processor zero, which is where the work was. Removing it would
     * mean re-deriving it the next time something on an exit path needs
     * to name its own processor, and the alternative that path would
     * otherwise reach for - `vmcs.vpid()` - is a VMREAD.
     *
     * Any user must gate on CR4.VMXE first. The GS base is only this
     * VMM's while the processor is in root mode, and code reached from
     * the launch path runs before that, with whatever the loader left.
     */
    static constexpr std::size_t host_gs_processor_index = 0;
    alignas(page_size) std::uint8_t gs_data[max_cpus][page_size]{};

    /**
     * The task segment to be used by the host VMM.
     */
    alignas(0x10) std::uint32_t host_tss[26]{};

    /**
     * Unprotected memory to be used by guest in UEFI boot.
     * The size is a multiple of the alignment so the size is guaranteed to
     * be a multiple of page size.
     */
    struct alignas(page_size) unprotected_memory
    {
        /**
         * What a guest reading this module's memory is shown instead.
         *
         * Not-present would be the honest answer and it is not an
         * available one: an EPT violation reports no data and no operand
         * size, so completing the access means either an instruction
         * decoder or letting the guest's own instruction run against
         * *something*. This is that something - a page of zeroes that
         * absorbs writes and reveals nothing.
         *
         * **It lives here, in the memory the guest is meant to reach,
         * rather than among the protected members.** Pointing an EPT
         * entry at a protected frame and marking it writable would work
         * and would quietly contradict protect_module, which makes every
         * page of this module not-present. A hole in that coverage
         * should not be something one has to notice.
         *
         * First in the structure on purpose. The structure is already
         * page aligned, so at offset zero this page is too, and the
         * member does not have to say so a second time - one alignment
         * rather than two agreeing. It does not make the structure any
         * smaller: the total is rounded up to a page either way, so the
         * padding simply moves to the end.
         *
         * Shared by every processor and every redirected page,
         * deliberately. It is a sink, not storage: nothing in this VMM
         * ever reads it, and a guest that reads back what it wrote into
         * it has learned nothing it did not already know.
         */
        std::uint8_t decoy_page[page_size]{};

        /**
         * What the guest reads from the controller's register page while
         * this VMM is using the controller.
         *
         * The alternative was to trap the guest's reads and answer them,
         * which needs the instruction decoder on the read path - and the
         * decoder is the part of this that is not trustworthy. Pointing
         * the guest's extended page table entry at ordinary memory
         * instead costs nothing per access, needs no decoding, and cannot
         * disagree with itself: reads go here at full speed and see
         * exactly the values put here, while writes still fault because
         * the entry stays unwritable.
         *
         * In unprotected_memory because the guest must be able to read
         * it. It is not storage either - it is refilled from the real
         * registers every time the redirect is armed.
         */
        std::uint8_t register_shadow[page_size]{};

        /**
         * The intermediate GDT to be loaded after page table switch
         * and before VMM and guest are launched.
         * Also to be reused in guest in case a new TSS needs to be
         * allocated in UEFI boot.
         */
        alignas(0x10) std::uint64_t intermediate_gdt[max_cpus][0x2000]{};

        /**
         * The task segment to be used by the guest in case no TSS.
         */
        alignas(0x10) std::uint32_t guest_tss[max_cpus][26]{};

    } unprotected_memory;

    /**
     * Assert that unprotected memory size is multiple of page size.
     */
    static_assert(!(sizeof(unprotected_memory) % page_size));

    /**
     * A pointer to the guest GDT memory.
     */
    std::uint64_t * guest_gdt_pointer{};

    /**
     * The guest GDT limit.
     */
    std::size_t guest_gdt_limit{};

    /**
     * The intermediate GDT limit.
     */
    std::size_t intermediate_gdt_limit{};

    /**
     * Intel specific state.
     * @{
     */

    /**
     * The debug control register.
     */
    std::uint64_t ia32_debug_control{};

    /**
     * The FS data.
     */
    std::uint64_t ia32_fs_base{};

    /**
     * The GS data.
     */
    std::uint64_t ia32_gs_base{};

    /**
     * Cache needed VMX MSRs.
     */
    std::uint64_t vmx_msrs[arch::x86_64::vmx::msr::size]{};

    /**
     * An object managing the currently assigned CPU VMCS.
     */
    arch::x86_64::vmx::vmcs vmcs{};

    /**
     * The processor's whole MTRR state - IA32_MTRRCAP,
     * IA32_MTRR_DEF_TYPE, the eleven fixed-range registers and the
     * variable pairs - from which every EPT entry's memory type is
     * derived. The derivation rules live with it, in
     * zpp/arch/x86_64/mtrr.h, because they are architectural rather than
     * anything this VMM decides.
     *
     * The variable ranges alone used to be here, as mtrrs[] beside a
     * separate mtrr_capabilities. That was the defect: without
     * IA32_MTRR_DEF_TYPE there is no default type to give an uncovered
     * range and no way to see either enable bit, and without the
     * fixed-range registers the first 1 MB - the legacy VGA aperture at
     * 0xa0000 among it - had no type of its own at all.
     */
    arch::x86_64::mtrr_state mtrrs{};

    /**
     * The next unused table in the ept pool below.
     *
     * Shared by initialize_ept, which splits any 2 MB page whose MTRR
     * coverage is not of one type, and protect_module, which splits any
     * 2 MB page holding part of this module. Both draw from the same pool,
     * so the index cannot be local to either.
     */
    std::size_t next_ept_table{};

    /**
     * Whether initialize_ept has finished.
     *
     * Guards epte_for, and exists because getting this wrong is silent
     * in both directions. An entry edited before the tables are built
     * lands in memory nothing will ever consult, and one edited between
     * that and initialize_ept is overwritten by it. Either way the
     * protection or the watch is simply absent, and looks exactly like
     * one that is there.
     *
     * Three separate changes in one evening made that mistake - the
     * local APIC watch twice and the queue storage once - so the rule
     * that an extended page table entry may only be touched after
     * initialize_ept and protect_module have run is enforced here rather
     * than remembered.
     */
    bool ept_initialized{};

    /**
     * How many times an extended page table entry has been changed, and
     * how many of those each processor has caught up with.
     *
     * INVEPT is not a broadcast. It invalidates on the processor that
     * executes it and nowhere else, so a watch armed on one processor is
     * simply not armed on the others until they invalidate too - they go
     * on using a translation cached before the change. That is not
     * theoretical: it is why the controller's re-enable was missed after
     * the reset was seen. The reset arrived on one processor, which
     * re-armed the watch and invalidated itself, and the guest's driver
     * then wrote the enable from a processor still holding the old
     * permissive entry, so nothing trapped.
     *
     * The fix is the one KVM uses rather than a rendezvous: mark, and let
     * every processor catch up on its own next entry.
     * `kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu)` sets a per-processor
     * request and `kvm_check_request` services it in the run loop - no
     * processor is ever held. Here that is a counter and a comparison on
     * the exit path, which costs a load and a branch per exit and needs
     * no interprocessor interrupt at all, because a guest that is running
     * is a guest that is taking exits.
     *
     * What it does not give is an immediate guarantee: a processor that
     * takes no exit keeps its stale entry. That is the same weakness the
     * single-processor invalidation already had, bounded now by exit
     * frequency rather than unbounded, and the direction is the safe one
     * - a stale permissive entry costs an observation, never a wrong
     * one. Somewhere that needs the guarantee would have to send the
     * interrupt, which is the part KVM also has and this does not.
     * @{
     */
    std::atomic<std::uint64_t> ept_generation{};
    std::uint64_t ept_generation_seen[max_cpus]{};
    /**
     * @}
     */

    /**
     * How many pages may be watched at once.
     *
     * Small on purpose. Each armed watch costs a VM exit on every guest
     * write to its page, so a design that wants many of them is a design
     * that should be reading memory directly instead. Arming more than
     * this is a programming error and is refused rather than dropped.
     */
    static constexpr std::size_t watch_capacity = 8;

    /**
     * The armed page watches. A fixed table rather than a container:
     * arming happens outside a VM exit but *matching* happens inside
     * one, and nothing on that path may allocate.
     */
    page_watch watches[watch_capacity]{};

    /**
     * Whether this processor is currently stepping a watched write, and
     * the page it opened to do it.
     *
     * Per processor because the step is: open the page, arm the monitor
     * trap flag, resume, take the MTF exit one instruction later, close
     * the page. Two processors can be inside that sequence at once on
     * different pages, and the MTF exit has to know which page *this*
     * one opened. The page number alone cannot say whether a step is in
     * progress, since zero is a legal page, so the flag is separate.
     */
    bool stepping_watch[max_cpus]{};
    std::uint64_t stepping_page[max_cpus]{};

    /**
     * The offset within the watched page that the stepped access named.
     *
     * The processor reports the faulting guest-physical address on every
     * EPT violation, so *which* register was touched is known even where
     * the instruction could not be emulated - and it was being computed
     * and then dropped on the stepping path, which left the handler with
     * a page and no register. Carried here so that a stepped write
     * reaches a handler in the same shape an emulated one does.
     *
     * This is the split both reference implementations use: the offset
     * comes from the exit and the value comes from the page, and neither
     * needs a decoder. SDM 32.4.3.3 describes the APIC-access exit as
     * reporting the offset for exactly this reason, and KVM's
     * `kvm_apic_write_nodecode` takes the offset from the exit and reads
     * the value back out of the page rather than decoding anything.
     */
    std::uint64_t stepping_offset[max_cpus]{};

    /**
     * Where the guest was when the step was armed, so the trap exit can
     * tell whether the instruction actually retired.
     *
     * A monitor-trap-flag exit does not mean the stepped instruction
     * ran. SDM 26.5.2 (.references/sdm.txt:201495): "If the instruction
     * causes a fault, an MTF VM exit is pending on the instruction
     * boundary following delivery of the fault (or any nested
     * exception)", and the paragraph above it says the same of a pending
     * event delivered before the instruction can execute. In both cases
     * the guest's write has not happened and RIP is inside a handler
     * rather than past the instruction.
     *
     * The step exists to report a write to a device register, so
     * reporting one that did not happen is worse than reporting nothing:
     * on the local APIC page it hands `on_interrupt_command` a stale
     * command register, which can adopt a start-up IPI nobody sent.
     * @{
     */
    std::uint64_t stepping_rip[max_cpus]{};

    /**
     * Steps whose instruction did not retire, so nothing was reported.
     * Counted rather than passed over, because a non-zero value means
     * writes to a watched register are being missed and the reason is
     * not visible any other way.
     */
    volatile std::uint64_t stepped_not_retired{};
    /**
     * @}
     */

    /**
     * How many of this module's pages a guest has touched. One per
     * page rather than one per access, since a page is redirected
     * permanently on the first touch and never faults again.
     */
    std::uint64_t module_access_count{};

    /**
     * The hardware page table structures.
     * @{
     */
    alignas(page_size) arch::x86_64::vmx::epte epml4[512];
    alignas(page_size) arch::x86_64::vmx::epte epdpt[512];
    alignas(page_size) arch::x86_64::vmx::epte epd[512][512];
    alignas(page_size) arch::x86_64::vmx::epte ept[1024][512];
    /**
     * @}
     */

    /**
     * The VMX regions for every CPU.
     */
    alignas(page_size) arch::x86_64::vmx::vmx_vmcs vmx[max_cpus];

    /**
     * The VMCS regions for every CPU.
     */
    alignas(page_size) arch::x86_64::vmx::vmx_vmcs vmx_vmcs[max_cpus];

    /**
     * How many processors get a second-level VMCS and the bitmaps that go
     * with it.
     *
     * One when nested VMX is off, for the reason `shadow_ept_tables`
     * gives at length: the nested sources are compiled in both
     * configurations - which is what type-checks them - so the members
     * have to exist, and a zero-length array is not a thing. Nothing
     * reaches them there.
     */
    static constexpr std::size_t nested_regions_per_cpu =
        nested_vmx::enabled ? max_cpus : 1;

    /**
     * The VMCS each processor runs a second-level guest with - vmcs02.
     *
     * A second *real* VMCS rather than a reuse of the first, because both
     * have to hold state at the same time: the guest hypervisor's own
     * guest state stays in vmcs01 for the whole time its guest runs, and
     * is what the processor is put back to on the way out. KVM keeps the
     * pair for the same reason, in `vmx_switch_vmcs`.
     *
     * Both stay *active* on this processor across the switch, which is
     * what makes the switch cheap: VMPTRLD makes the named VMCS current
     * and leaves the outgoing one active, so nothing is written back and
     * nothing is re-read. VMCLEAR is what would force that, and it is
     * executed exactly once per processor - in `setup_vmcs`, to put the
     * launch state where VMLAUNCH needs to find it.
     */
    alignas(page_size)
        arch::x86_64::vmx::vmx_vmcs vmcs02[nested_regions_per_cpu];

    /**
     * The MSR and I/O bitmaps a second-level guest runs under: the union
     * of what the guest hypervisor asked to intercept and what this VMM
     * intercepts for itself.
     *
     * Merged rather than either side's, and merging rather than forcing
     * everything to exit is the decision worth recording. KVM forces
     * unconditional I/O exiting in `prepare_vmcs02_early` because it
     * emulates I/O anyway; this VMM does not emulate I/O or MSR accesses
     * for anybody, so an exit neither side asked for would arrive with
     * nothing able to answer it. A union has the property that matters
     * instead: every exit belongs to one of the two, and the one it
     * belongs to already knows how to answer it.
     *
     * Per processor because the guest hypervisor's bitmaps are per VMCS
     * and a VMCS is current on one processor at a time.
     * @{
     */
    alignas(page_size) std::uint8_t
        nested_msr_bitmap[nested_regions_per_cpu][page_size]{};
    alignas(page_size) std::uint8_t
        nested_io_bitmap[nested_regions_per_cpu][2 * page_size]{};
    /**
     * @}
     */

    /**
     * Where the merged bitmaps are, by physical address, since that is
     * what the VMCS fields hold.
     *
     * There is deliberately nothing here caching what they were merged
     * from. Caching on the guest hypervisor's bitmap *addresses* was
     * written first and is wrong: the contents live in guest memory it
     * writes directly, with no VMWRITE and no exit, so the address is
     * unchanged exactly when a hypervisor adds or drops an intercept. See
     * `merge_nested_bitmaps`.
     * @{
     */
    std::uint64_t nested_msr_bitmap_physical[max_cpus]{};
    std::uint64_t nested_io_bitmap_physical[max_cpus]{};
    /**
     * @}
     */

    /**
     * VMCS shadowing: the region the guest hypervisor's own VMREADs and
     * VMWRITEs are served from, and the bitmaps that decide which fields
     * it may reach without an exit.
     *
     * This is the largest single cost this VMM imposes on a guest
     * hypervisor. Measured on the rig across a four minute Hyper-V boot,
     * 4,287,565 exits on the boot processor: VMREAD 2,070,621 and VMWRITE
     * 765,747, which is **65.7% of every exit taken**, against 359,923
     * exits by the guest that hypervisor was running. Nine of ours per one
     * of its own, and every one of them a field it could have read out of
     * memory.
     *
     * Shadowing is not free either, and the shape of the cost is what
     * decides the field list. A shadowed field lives in a hardware-format
     * region that only VMREAD and VMWRITE can touch, so keeping it and the
     * cached vmcs12 agreeing costs a VMPTRLD, a copy, a VMCLEAR and a
     * VMPTRLD back at each of the two points they can diverge. That is
     * about six VMX instructions per second-level exit however many fields
     * are copied - so the list wants to be *the fields the guest
     * hypervisor actually uses*, and no more.
     *
     * Which is why record_vmcs_field_use exists rather than this list
     * being KVM's. KVM's vmcs_shadow_fields.h is a real measurement, of a
     * different guest under a different VMM.
     * @{
     */
    alignas(page_size) arch::x86_64::vmx::vmx_vmcs
        shadow_vmcs[nested_regions_per_cpu];
    std::uint64_t shadow_vmcs_physical[nested_regions_per_cpu]{};

    /**
     * A set bit means "exit"; a clear bit means "the guest hypervisor may
     * have this one out of the shadow region". Set to all ones and then
     * punched through for the shadowed fields, so a field nobody thought
     * about exits - which is the direction that stays correct.
     *
     * One pair for the whole VMM rather than one per processor: the
     * contents do not depend on which processor is running, and the
     * architecture indexes them by field encoding alone.
     */
    alignas(page_size) std::uint8_t
        vmcs_shadow_read_bitmap[nested_vmx::enabled ? page_size : 1]{};
    alignas(page_size) std::uint8_t
        vmcs_shadow_write_bitmap[nested_vmx::enabled ? page_size : 1]{};
    std::uint64_t vmcs_shadow_read_bitmap_physical{};
    std::uint64_t vmcs_shadow_write_bitmap_physical{};

    /**
     * Whether the processor granted the control. Requested through
     * adjust_msr like every other secondary control, so a processor
     * without it simply runs the old way rather than failing VM entry -
     * and this records which happened, because "the fix did nothing" and
     * "the fix was not applied" look identical from the exit counts.
     */
    bool vmcs_shadowing_enabled{};

    /**
     * How many times each direction was copied, so the cost of the fix is
     * visible beside the exits it removed.
     * @{
     */
    /**
     * What the shadow VMCS region currently holds, so a copy into it can
     * skip the fields that already match.
     *
     * Measured on the rig: this VMM's own exit handler is 85% of wall
     * clock, at about 160 microseconds and roughly 150 trapped VMX
     * instructions per reflected second-level exit. Thirty-six of those
     * are this copy, and most of the fields it writes are unchanged from
     * the last time it ran. **A comparison against memory is free; a
     * VMWRITE traps to the layer below and is not.**
     *
     * Correct despite the guest hypervisor writing the read-write fields
     * itself without exiting, because the cache records what is *in the
     * region* rather than what was intended: `copy_shadow_to_vmcs12`
     * reads those fields back and updates the cache with what it found,
     * so the next copy out compares against the truth.
     *
     * `shadow_cache_valid` starts false so the first copy writes
     * everything.
     * @{
     */
    static constexpr std::size_t shadow_cache_capacity = 64;
    std::uint64_t shadow_cache[max_cpus][shadow_cache_capacity]{};
    bool shadow_cache_valid[max_cpus]{};
    std::uint64_t shadow_writes_skipped[max_cpus]{};
    std::uint64_t shadow_writes_done[max_cpus]{};
    /** @} */

    /**
     * Where the exit handler's time actually goes, per phase.
     *
     * Measured: this VMM is 85% of wall clock at about 156 microseconds
     * an exit. Guessing at the breakdown failed twice, so it is
     * instrumented - `save_l2_state` is 9%, `build_vmcs02` 24% at 270
     * microseconds a call, and the shadow extended-page-table lookup
     * 0.2%, which retires the page tables as a suspect.
     *
     * Indices: 0 save_l2_state, 1 reflect_l2_exit, 2 build_vmcs02,
     * 3 shadow_ept_pointer_for, 4 copy_vmcs12_to_shadow,
     * 5 copy_shadow_to_vmcs12. Anything left over is the rest.
     *
     * The last two are inside the first two and are broken out because
     * reflect_l2_exit is the largest phase at 409,739 cycles a call, and
     * the copies are the only thing in it that executes instructions
     * which can never be shadowed - vmptrst, vmptrld and vmclear always
     * trap to the layer below.
     * @{
     */
    /**
     * The cost of one VMREAD, measured rather than inferred.
     *
     * Everything about what to optimise turns on this. The figure in use
     * - about a tenth of a microsecond - came from comparing two runs at
     * different phases of a boot, which is not a controlled comparison,
     * and if it is wrong by an order of magnitude the conclusion drawn
     * from it inverts.
     *
     * Timed once, on the first exit, over a thousand reads of a field
     * that is read-only and already being read anyway, so the
     * measurement disturbs nothing.
     */
    /**
     * This VMM's own host state and control words, read out of vmcs01
     * once instead of on every nested entry.
     *
     * `build_vmcs02` copied twenty host-state fields out of vmcs01 and
     * into vmcs02 on **every** call - forty VMCS accesses, about seventy
     * microseconds of its two hundred and seventy at the 1.76
     * microseconds a VMREAD costs here - for state that is written once
     * at launch and never changes afterwards. The host is this VMM; its
     * stack, its page tables, its segment selectors and its entry point
     * are fixed for the life of the processor.
     *
     * The control words beside them are the same story: they describe
     * what this VMM asks for when *it* runs a guest, and nothing rewrites
     * them.
     *
     * Filled on the first nested entry per processor, when vmcs01 is
     * still current, and used from then on.
     * @{
     */
    std::uint64_t host_state_cache[max_cpus][24]{};
    std::uint64_t host_controls_cache[max_cpus][8]{};
    bool host_state_cached[max_cpus]{};

    /**
     * Whether vmcs02 already carries this VMM's host state.
     *
     * The other half of the same saving. The host fields were not only
     * re-read from vmcs01 every entry, they were re-*written* into vmcs02
     * every entry - and vmcs02 is cleared once, where it is created, and
     * never again, so the values written the first time are still there.
     * Twenty more VMCS accesses a call at 1.76 microseconds each.
     */
    bool vmcs02_host_written[max_cpus]{};

    /**
     * What vmcs02's guest-state fields actually hold, so `build_vmcs02`
     * can skip writing back what is already there.
     *
     * Forty VMWRITEs on every nested entry, about 120,000 cycles at the
     * 1.4 to 1.8 microseconds a VMCS access costs here, and almost all
     * of them redundant: the processor saves the guest's state into
     * vmcs02 on every exit (SDM 28.3), `save_l2_state` copies that into
     * vmcs12, and the loop then writes vmcs02 the values the processor
     * just put there.
     *
     * **The obvious version of this is wrong.** A cache of what this VMM
     * last *wrote* would skip a field the processor had since changed,
     * and the guest would resume with stale state - a fault that would
     * surface much later as an impossible guest bug. So the cache is
     * filled from `save_l2_state`'s read on the way out, which is what
     * vmcs02 genuinely contains, exactly as `shadow_cache` is filled
     * from what `copy_shadow_to_vmcs12` finds.
     *
     * `guest_state_fresh` is what makes it safe rather than merely
     * likely. It is set by `save_l2_state` and cleared by
     * `build_vmcs02`, so a write can only be elided when a save has run
     * since the last build - which is the one ordering under which the
     * cache is known to describe vmcs02. Between that save and that
     * build the second-level guest does not execute, so nothing else can
     * change those fields.
     * @{
     */
    std::uint64_t guest_state_cache[max_cpus][48]{};
    bool guest_state_fresh[max_cpus]{};

    /**
     * The bulk guest-state copy, deferred until something asks for it.
     *
     * `save_l2_state` read all 46 of `guest_state_fields` out of vmcs02
     * on every exit and wrote them into vmcs12 - about 121,000 cycles,
     * 17% of an exit - so that the guest hypervisor could read 16
     * distinct fields roughly once every fourteen exits, and **none of
     * the 16 is in this set**. 8.5 million copies to serve 37,389
     * reads.
     *
     * It is safe to stop because the processor has already put the
     * values where they belong. SDM 30.3.1
     * (`.references/sdm.txt:204498`) and the sections beside it save
     * every one of these into the guest-state area on **every** VM
     * exit, so vmcs02 holds the second-level guest's state whether this
     * VMM copies it or not. Partitioned by that citation and not by
     * naming convention - a first pass matched field names by prefix
     * and invented a three-field hazard out of a mismatch.
     *
     * **Two fields are excluded and the exclusion is load bearing.**
     * `guest_cs_access_rights` and `guest_ss_access_rights` are on
     * `shadow_read_write_fields`, so the guest hypervisor reads them
     * out of the hardware shadow region **with no exit at all** - there
     * is no interception point at which a deferred value could be
     * materialised, and a stale one would be handed over invisibly.
     * Anything added to that list must be excluded here too.
     *
     * `guest_state_dirty` is which of them the guest hypervisor has
     * written since the last entry, by index into `guest_state_fields`.
     * Only those are written back into vmcs02; the rest are left
     * exactly as the processor saved them, which is what makes the
     * deferral a no-op rather than a lost write.
     * @{
     */
    bool guest_state_deferred[max_cpus]{};
    std::uint64_t guest_state_dirty[max_cpus]{};

    /**
     * Which vmcs12 the deferred state belongs to.
     *
     * **This is the condition the first attempt was missing, and it
     * reset the guest 218 times.** "The processor saves these on every
     * exit" is true and is not sufficient: what makes skipping the
     * write-back a no-op is that vmcs02 was last written by an exit
     * *from the guest about to be entered*. Two ways that fails -
     * the first entry to a vmcs02, where nothing has been saved and the
     * region holds what it was cleared to; and the level above
     * VMPTRLDing a different vmcs12, where vmcs02 is reused per
     * processor and its contents belong to another second-level guest.
     *
     * So the deferral is licensed only while this matches
     * `guest_current_vmcs` and `vmcs02_launched` is set. When it does
     * not, every deferred field is written unconditionally - not merely
     * un-elided, because `guest_state_cache` is stale for exactly those
     * indices and comparing against it could skip a write that is owed.
     */
    std::uint64_t guest_state_deferred_vmcs[max_cpus]{};

    /**
     * Where the deferral's model of vmcs02 differs from what is
     * actually in it. See `nested_vmx::shadow_guest_state`.
     *
     * A field is recorded when the deferral would have skipped its
     * write-back - so vmcs02 would keep what it holds - and vmcs12 says
     * something else. That is exactly the shape of the failure three
     * boots could only report as a reset.
     *
     * Bounded on purpose: a per-field histogram, which is the summary
     * that names the culprit, plus the first few in full for their
     * context. Nothing allocates, nothing spins, and nothing logs per
     * field per exit - the VP assist watch wedged a guest and this must
     * not repeat it.
     * @{
     */
    static constexpr std::size_t shadow_divergence_slots = 16;

    volatile std::uint64_t shadow_divergences[max_cpus]{};
    volatile std::uint64_t shadow_divergence_by_field[48]{};

    volatile std::uint64_t shadow_divergence_field[shadow_divergence_slots]{};
    volatile std::uint64_t
        shadow_divergence_in_vmcs02[shadow_divergence_slots]{};
    volatile std::uint64_t
        shadow_divergence_in_vmcs12[shadow_divergence_slots]{};
    volatile std::uint64_t shadow_divergence_owner[shadow_divergence_slots]{};
    volatile std::uint64_t shadow_divergence_dirty[shadow_divergence_slots]{};
    volatile std::uint64_t shadow_divergence_entries[shadow_divergence_slots]{};
    /**
     * @}
     */

    /**
     * Whether the deferred copy's skip actually happens, and how often.
     *
     * **The cost model this investigation reasoned from is refuted.** A
     * VMCS read is priced at about 2,984 cycles by the benchmark in
     * `on_vm_exit`, `save_l2_state` performs 60 of them for 198,309
     * cycles, and removing 44 changed that by 2%. Those three facts
     * cannot all be about the same machine, and every estimate in this
     * file - the census's 1.16x, the exits-per-tick reframing, the
     * cycles-per-exit ceiling - rests on the arithmetic the third
     * refutes.
     *
     * These two settle the first branch of it directly: "the skip never
     * happened" and "the skip happened and cost nothing" are otherwise
     * the same reading, and only the second is interesting.
     *
     * The region split the rest of the question needs is **already
     * measured** - `handler_cycles` over `handler_exits` is the time
     * from the first instruction this VMM controls on an exit to the
     * last before it resumes, and `handler_last_tsc` gives the gap to
     * the next one. Nothing has ever printed them, which is the fourth
     * counter in this file found running unread.
     */
    volatile std::uint64_t guest_state_reads_skipped[max_cpus]{};
    volatile std::uint64_t guest_state_reads_done[max_cpus]{};

    volatile std::uint64_t guest_state_defers[max_cpus]{};
    volatile std::uint64_t guest_state_materialises[max_cpus]{};
    volatile std::uint64_t guest_state_dirty_writes[max_cpus]{};
    /**
     * @}
     */
    std::uint64_t guest_state_writes_skipped[max_cpus]{};
    std::uint64_t guest_state_writes_done[max_cpus]{};
    /** @} */

    /**
     * The same elision for vmcs02's *control* fields, which is sound for
     * a reason the guest-state one had to work for.
     *
     * `guest_state_cache` cannot hold what this VMM last wrote - the
     * processor saves the guest's own state over those fields on every
     * exit (SDM 30.3), so a last-written cache would skip a write that
     * is owed. That is what killed `ZPP_LAZY_GUEST_STATE`; BACKLOG.md
     * records it, with SDM 30.3.2 on unusable segments.
     *
     * None of that applies here. The processor never writes a
     * VM-execution, VM-exit or VM-entry *control* field, so what was
     * last written is still what vmcs02 holds, and the cache needs no
     * read-back and no freshness flag. It is the same argument
     * `vmcs02_host_written` already makes for the host-state fields, and
     * it rests on the same fact: vmcs02 is VMCLEARed where it is created
     * and never again.
     *
     * **Deliberately excludes three fields that look like controls and
     * are not.** The VM-entry interruption-information field has its
     * valid bit cleared by the processor on entry (SDM 27.6.1); the
     * VMX-preemption timer value is decremented and saved back when the
     * matching exit control is set; and the entry exception error code
     * and instruction length are only written when injecting, so caching
     * them would trade a rare write for a permanent hazard.
     *
     * Twenty-three fields, worth that many VMWRITEs an entry at the 1.4
     * to 1.8 microseconds a VMCS access costs here - `build_vmcs02`
     * measured at 225,952 cycles a call before it, with its guest-state
     * writes already 99.4% elided, so nearly all of that was this.
     *
     * `control_fields` says which four controls are deliberately left
     * out and why; the short version is that they have writers outside
     * `build_vmcs02`.
     * @{
     */
    static constexpr std::size_t control_cache_capacity = 32;

    std::uint64_t control_cache[max_cpus][control_cache_capacity]{};
    bool control_cache_valid[max_cpus][control_cache_capacity]{};
    std::uint64_t control_writes_skipped[max_cpus]{};
    std::uint64_t control_writes_done[max_cpus]{};

    /**
     * Write one of vmcs02's control fields, skipping the VMWRITE when
     * the field already holds that value. Any field not on
     * `control_fields` is written straight through, so a caller cannot
     * silently gain an elision it has not argued for.
     */
    void write_vmcs02_control(std::size_t cpu,
                              arch::x86_64::vmx::vmcs::field control,
                              std::uint64_t value);

    /**
     * Forget what vmcs02 is believed to hold, for both elisions that
     * believe anything about it.
     *
     * Called where vmcs02's region is created and cleared, which in this
     * VMM happens exactly once per processor. It exists as a named
     * function rather than two assignments because the coupling is
     * otherwise invisible: anything that resets vmcs02's contents owes
     * both caches an invalidation, and `tests/nested_exit` is the thing
     * that found this - its `reset` zeroes the fake VMCS between cases,
     * which is a legitimate model of a fresh region, and the control
     * elision then skipped writes that were owed. `vmcs02_host_written`
     * had the same dependency all along and no check that noticed.
     */
    void forget_vmcs02_contents(std::size_t cpu);

    /**
     * Whether each merged bitmap currently holds this VMM's own page
     * and nothing else: MSR, then I/O A, then I/O B.
     *
     * A guest hypervisor that does not use a bitmap leaves the union
     * equal to this VMM's side of it, and that side does not change
     * between VM entries - so the merge produces the same page every
     * time and need not be produced again. Measured on Hyper-V: two of
     * the three pages take that branch on every entry, and
     * `merge_nested_bitmaps` was rebuilding all three of them, a byte
     * at a time, more than a thousand times a second.
     *
     * The flag is about *this VMM's* bitmap, never the guest
     * hypervisor's. The guest hypervisor writes its own pages with no
     * VMWRITE and no exit, which is why caching on the bitmap address
     * is wrong - argued at length in `merge_nested_bitmaps` - and this
     * caches only the case where no guest page is in the answer at all.
     */
    bool nested_bitmap_is_ours[max_cpus][3]{};

    /**
     * Every second-level exit, by the privilege level it came from and
     * by the class of the virtual task priority in force.
     *
     * Sampled in `save_l2_state`, so once per second-level exit and
     * without choosing the moment - which is the whole point. Every
     * priority reading taken so far was at the instruction that writes
     * the synthetic interrupt command, an instruction only executed
     * *at* DISPATCH_LEVEL, so the samples could not have said anything
     * else and were read as though they could.
     *
     * `l2_cpl_seen[3]` is the one that answers the question this VMM
     * exists to answer. Nothing in the tree has ever counted whether
     * the guest reaches user mode.
     */
    std::uint64_t l2_cpl_seen[max_cpus][4]{};

    /**
     * The same census for the **first** level guest, indexed by
     * privilege level, incremented on every exit this VMM takes.
     *
     * `l2_cpl_seen` only exists when nested VMX is on, and the question
     * it answers - does the guest reach user mode - is asked in both
     * configurations. With nested off the fallback was hundreds of
     * `info registers` samples, all of which read CPL 0, and that was
     * reported as "ring 3 was never reached". It establishes no such
     * thing: a booted, idle Windows is at CPL 0 in its idle loop
     * essentially all of the time, so the reading is equally consistent
     * with a machine at the desktop and one livelocked in kernel code.
     * A census over every exit cannot make that mistake.
     *
     * **A proof of existence, not a distribution.** One exit at ring 3
     * proves user mode was reached. A zero is weaker: the exits this VMM
     * takes are biased towards kernel work, and with nested VMX off the
     * guest takes very few of them. The asymmetry is deliberate, because
     * the question is whether user mode happens at all.
     */
    std::uint64_t cpl_seen[max_cpus][4]{};
    std::uint64_t l2_vtpr_class_seen[max_cpus][16]{};

    /**
     * How many synthetic-timer periods `ZPP_STRETCH_GUEST_TIMER`
     * lengthened, per processor.
     *
     * Zero on a build that did not ask for it, and zero on a build that
     * did but whose guest only ever wrote absolute deadlines - which
     * are deliberately left alone and would otherwise make a stretched
     * run look like an unstretched one.
     */
    std::uint64_t guest_timer_stretched[max_cpus]{};

    /**
     * Drops that cache on every processor.
     *
     * Called wherever this VMM edits one of its own bitmaps after the
     * first merge, which today is `intercept_interrupt_command`. It has
     * to be every processor rather than one: the bitmaps being edited
     * are shared, and each processor holds its own merged copy of them.
     *
     * Defined here rather than beside `merge_nested_bitmaps`, which is
     * where it belongs by subject: `tests/local_apic` links
     * `local_apic.cpp` and not `nested_entry.cpp`, so a definition
     * there is an undefined symbol in the host suite.
     */
    constexpr void forget_nested_bitmaps()
    {
        for (auto & per_cpu : this->nested_bitmap_is_ours) {
            for (auto & flag : per_cpu) {
                flag = false;
            }
        }
    }
    /** @} */
    /** @} */

    /**
     * Whether this VMM takes every external interrupt and injects it,
     * the way KVM does, instead of letting the guest's own interrupt
     * controller deliver them natively.
     *
     * Off by default, which is this VMM's design: the interrupts are the
     * guest's, it owns the controller, and letting them arrive without
     * an exit costs nothing. On, `external_interrupt_exiting` and
     * `acknowledge_interrupt_on_exit` are set in vmcs01, every interrupt
     * exits here with its vector in hand, and it is put back into the
     * guest through the entry-interruption field.
     *
     * It exists because that is the one architectural difference between
     * this VMM and KVM that any measurement has pointed at, and the
     * nested guest stalls here while booting under KVM. Whether the
     * difference matters is worth one boot.
     *
     * **This is not what KVM's `acknowledge_interrupt_on_exit` is for**,
     * and the difference is the whole design. KVM takes the interrupt
     * for its *host*: `handle_external_interrupt_irqoff` calls
     * `vmx_do_interrupt_irqoff(gate_offset(host_idt_base + vector))`,
     * which runs the Linux handler, and the Linux handler writes the
     * end-of-interrupt. What KVM's guests receive is a different
     * interrupt entirely, synthesized by the virtual local APIC in
     * `lapic.c` - `kvm_apic_ack_interrupt` sets vISR, `apic_set_eoi`
     * clears it when the guest writes EOI, and `apic_update_ppr` keeps
     * the priority. None of that machinery exists here.
     *
     * What makes taking-and-injecting nevertheless sound *here* is the
     * design KVM does not have: the guest owns the physical local APIC.
     * The hardware acknowledge sets the real ISR bit and the guest's own
     * handler writes the real EOI that clears it, so in-service state
     * and priority are maintained by the hardware, for free - provided
     * every acknowledged vector reaches the guest. That proviso is why
     * the queue below is a 256-bit bitmap and not the single slot it
     * used to be.
     *
     * Reading it from outside, which is the only way this is observable
     * on a running machine:
     *
     * - `taken` counts vectors the hardware acknowledged into this VMM.
     * - `injected` counts vectors handed to the guest. It should chase
     *   `taken` and settle at most `pending` behind it.
     * - `dropped` is the alarm: a vector acknowledged twice with no
     *   intervening delivery, which the bitmap cannot represent and
     *   which the local APIC should make impossible while the first
     *   occurrence's in-service bit is still set. Non-zero means the
     *   model here is wrong.
     * - `pending` is what is queued now, `pending_high_water` the most
     *   ever queued at once. A high-water mark above 1 means delivery is
     *   not keeping up with arrival.
     * - `deferred` counts entries at which a queued vector could not be
     *   delivered - the guest had interrupts masked, was in an STI
     *   shadow, or already had an event staged - and interrupt-window
     *   exiting was armed instead.
     * - `deferred_in_l2` counts entries at which a queued vector was
     *   held because a *second-level* guest was about to run. The
     *   interrupt was signalled to the physical processor, so it belongs
     *   to the first-level guest's world and must not go into L2's IDT.
     *   This one has no bound: nothing here forces an L2 exit to shorten
     *   the wait, and while it waits the physical APIC's in-service bit
     *   blocks everything at or below that priority. If this climbs, that
     *   is the next thing to fix.
     * - `hlt_cleared` counts injections into a halted processor, where
     *   the activity state had to be put back to active. KVM does the
     *   same in `vmx_clear_hlt`.
     *
     * Interrupt-window exits are not counted separately: they are exit
     * reason 7 in `exit_reason_counts`.
     * @{
     */
    volatile std::uint64_t external_interrupts_taken[max_cpus]{};
    volatile std::uint64_t external_interrupts_injected[max_cpus]{};
    volatile std::uint64_t external_interrupts_dropped[max_cpus]{};
    volatile std::uint64_t external_interrupts_deferred[max_cpus]{};
    volatile std::uint64_t external_interrupts_deferred_in_l2[max_cpus]{};
    volatile std::uint64_t external_interrupts_pending[max_cpus]{};
    volatile std::uint64_t
        external_interrupts_pending_high_water[max_cpus]{};
    volatile std::uint64_t external_interrupts_hlt_cleared[max_cpus]{};

    /**
     * The queue itself: one bit per vector, four words of sixty-four.
     * Not a count and not a single slot, because a vector that reaches
     * it has already been taken out of the interrupt controller and
     * cannot be recovered from anywhere.
     */
    static constexpr std::size_t external_vector_words = 4;
    std::uint64_t pending_external_vectors[max_cpus]
                                          [external_vector_words]{};
    /** @} */

    /**
     * How many times each interrupt vector has been acknowledged into
     * this VMM, per processor.
     *
     * One question, and it forks the diagnosis rather than narrowing it:
     * a guest parked with its clock ticking is either not being given a
     * device's interrupt, or never asked the device for anything. The
     * totals cannot tell those apart - `external_interrupts_taken`
     * counts a timer tick and a completion the same. A histogram can:
     * one or two vectors means only the clock is arriving, and whatever
     * the guest is waiting for was never issued.
     *
     * 32 bits and not `volatile`, unlike the totals beside it. These are
     * read from outside through the monitor as a block of physical
     * memory, so a torn count costs a wrong bar on a histogram and
     * nothing else; the totals are read one at a time and are load
     * bearing for whether an interrupt was lost.
     */
    std::uint32_t external_interrupt_vector_counts[max_cpus][256]{};

    /**
     * What a VMCS access costs here, measured rather than inferred.
     *
     * Cycles for a thousand accesses, taken once on the first exit.
     *
     * The field choice is the whole point, and it is not arbitrary.
     * This VMM is KVM's guest, so every VMX instruction it issues may
     * be emulated - but KVM offers *VMCS shadowing* to its guests, and
     * a shadowed field is answered by hardware out of a shadow VMCS
     * with no exit at all. Which fields those are is a fixed list in
     * KVM: `.references/kvm/vmcs_shadow_fields.h`.
     *
     * So the five numbers price the two halves separately:
     *
     * - `exit_reason` is `SHADOW_FIELD_RO`,
     * - `guest_rip` and `guest_rsp` are `SHADOW_FIELD_RW`,
     * - `guest_gdtr_base` and `guest_gdtr_limit` are on neither list.
     *
     * If the shadowed ones come out at tens of cycles and the others at
     * thousands, then the nested path's cost is not "VMCS traffic" at
     * all - it is *the traffic that leaves the shadow list*, and the
     * fix is to move the hot path onto shadowed fields rather than to
     * cache anything. If all five agree, shadowing is not in play and
     * every access has to be removed rather than redirected.
     *
     * The previous measurement priced only `exit_reason` and concluded
     * 3,433 cycles for every access, which assumes the answer to
     * exactly this question.
     */
    std::uint64_t vmread_benchmark_cycles{};
    std::uint64_t vmread_shadowed_cycles{};
    std::uint64_t vmread_unshadowed_cycles{};
    std::uint64_t vmwrite_shadowed_cycles{};
    std::uint64_t vmwrite_unshadowed_cycles{};
    std::uint64_t vmread_benchmark_sink{};
    bool vmread_benchmark_done{};

    /**
     * Phases 6 and 7 are the two VMPTRLDs of a nested round trip.
     *
     * They are timed separately because the elisions made everything
     * else in `build_vmcs02` nearly free and the phase did not move:
     * measured 178,639 cycles a call while the call performed **0.6**
     * VMCS writes (51,068,656 skipped against 551,900 done). Whatever
     * costs that is not a VMWRITE, and the only other trapping
     * instruction in there is the VMPTRLD that makes vmcs02 current.
     *
     * The suspicion is specific rather than general. KVM emulates a
     * guest's VMPTRLD in `handle_vmptrld`: it maps the guest page,
     * copies the whole 4 KB region, syncs its shadow VMCS both ways -
     * `copy_shadow_to_vmcs12`, which VMPTRLDs the shadow region, reads
     * thirty fields and VMPTRLDs back - and sets `dirty_vmcs12`, which
     * makes the *next* entry take `prepare_vmcs02`'s slow path instead
     * of its fast one. That is a lot of work for one instruction, and
     * this VMM issues two of them on every single second-level exit.
     *
     * If they are what the two phases suggest, the fix is structural
     * and worth its size: one VMCS region rather than two, rewritten in
     * place. If they are cheap, the cost is somewhere nobody has looked
     * and this says so instead.
     */
    /**
     * 14 and 15 split `build_vmcs02` at its VMPTRLD, because adding its
     * named children up leaves about 59,500 cycles an entry unaccounted
     * for - the largest single unexplained term in the handler once the
     * copies were made cheap. A container's total minus its children is
     * where cost hides, which this file learned when three phases fell
     * and the handler did not.
     */
    // 16 through 20 are the VMPTRLD decomposition - three adjacent
    // intervals, the shadow publish, and the whole call, so coverage is
    // computed rather than assumed. See `on_guest_vmptrld`.
    // 21 through 24 bracket `materialise_l2_guest_state`, which is 81%
    // of VMPTRLD and had never been measured.
    static constexpr std::size_t phase_count = 28;
    std::uint64_t phase_cycles[max_cpus][phase_count]{};
    std::uint64_t phase_calls[max_cpus][phase_count]{};
    /** @} */

    std::uint64_t vmcs_shadow_loads[max_cpus]{};
    std::uint64_t vmcs_shadow_stores[max_cpus]{};

    /**
     * Exits taken for a field the shadow bitmaps permit, which is the
     * exit VMCS shadowing exists to prevent - so a non-zero count means
     * the control is offered and not in force. See
     * `note_shadowing_ineffective`, which stands the feature down rather
     * than going on paying twenty VMCS accesses an entry for it.
     */
    std::uint64_t shadowing_ineffective[max_cpus]{};
    /** @} */

    void initialize_vmcs_shadowing();
    void set_vmcs_shadowing(std::size_t cpu, bool enabled);
    void note_shadowing_ineffective(std::size_t cpu,
                                    std::uint64_t encoding,
                                    bool write);
    void copy_vmcs12_to_shadow(std::size_t cpu);
    void copy_shadow_to_vmcs12(std::size_t cpu);
    /**
     * @}
     */

    /**
     * How many paging-structure pages each processor's shadow extended
     * page table may use.
     *
     * Ninety six is one page-directory-pointer table plus ninety five page
     * directories, and at 2 MB leaves a page directory covers a gigabyte -
     * so this is 95 GB of second-level guest-physical space per processor
     * before a single split. Splits to 4 KB spend one more each.
     *
     * The shadow is built eagerly, so this is a hard cap with a defined
     * consequence rather than a hint: a build that cannot finish refuses
     * the guest hypervisor's VM entry instead of entering with a table
     * that is quietly incomplete. BACKLOG.md records why eager
     * construction makes that the right failure - a lazy shadow can
     * discard and rebuild on demand, an eager one that ran out has no
     * partial state worth entering with.
     *
     * Per processor rather than shared, which costs the duplication and
     * buys the absence of any lock on the build path.
     *
     * Sized to one page when the switch is off, so a build that cannot run
     * nested VMX does not reserve twelve megabytes for it. It cannot be
     * removed altogether: the nested code is compiled in both
     * configurations - which is what type-checks it - so the members have
     * to exist for it to name. One page rather than zero because a
     * zero-length array is not a thing, and the cap is checked against
     * this same constant, so the off build simply refuses on its first
     * table. That path is unreachable there, since nothing calls it.
     */
    static constexpr std::size_t shadow_ept_tables_per_cpu =
        nested_vmx::enabled ? 96 : 1;

    /**
     * How many shadows each processor keeps, each keyed by the guest
     * hypervisor's own EPT pointer.
     *
     * One was not enough, and the failure is not subtle. A guest
     * hypervisor switches between the extended page tables of the guests
     * it runs, and with a single shadow every switch discarded eleven to
     * twenty-four thousand composed regions and walked them again.
     * Measured on the rig as the whole log filling with alternating
     * rebuilds between two roots, 0x102184000 and 0x102187000, while the
     * second level advanced by a few hundred entries a minute.
     *
     * Four, which is what KVM keeps: `kvm_mmu` holds the current root
     * plus `prev_roots[KVM_MMU_NUM_PREV_ROOTS]`, matched against an
     * incoming EPT pointer by `nested_ept_root_matches` before anything
     * is rebuilt. ACRN answers the same question with a table keyed by
     * guest EPTP - `vept_desc_bucket[MAX_ACTIVE_VVMCS_NUM *
     * MAX_VCPUS_PER_VM]`, looked up by `find_vept_desc` - which is the
     * same design with the bound derived from its configuration rather
     * than fixed. Neither rebuilds on a switch, and neither keeps one.
     *
     * The pool below is *shared* between them rather than divided, for
     * the reason the division would fail: the two shadows this workload
     * produces need 27 and 57 tables, so equal shares of a 96 table pool
     * would starve the larger one while the smaller left half its share
     * unused.
     */
    static constexpr std::size_t shadow_ept_slots =
        nested_vmx::enabled ? 4 : 1;

    /**
     * The shadow extended page tables, one set per processor.
     *
     * The root is separate from the pool because it is never recycled: a
     * rebuild zeroes it and hands the pool back, and the pointer written
     * into the VMCS stays the same. That keeps the EPT pointer stable
     * across rebuilds, which matters because SDM 31.4.2 associates cached
     * mappings with bits 51:12 of the pointer - so a stable root plus a
     * global invalidation is a complete story, where a moving root would
     * leave the old one's mappings cached against an address that had been
     * reused.
     * @{
     */
    alignas(page_size) arch::x86_64::vmx::epte
        shadow_epml4[max_cpus][shadow_ept_slots][512];
    alignas(page_size) arch::x86_64::vmx::epte
        shadow_ept_tables[max_cpus][shadow_ept_tables_per_cpu][512];

    /**
     * Which slot owns each table of the shared pool: zero for free, and
     * otherwise the slot's index plus one.
     *
     * Plus one so that zero means free, which is what the member's own
     * zero initialization already gives - the alternative needed a pass
     * over the pool before the first build, and a pool that is
     * accidentally "all owned by slot zero" fails by finding no free
     * table rather than by saying so.
     *
     * A bitmap allocator in all but name, and ACRN's `sept_page_pool`
     * with its `sept_page_bitmap` is the same thing: shadows differ in
     * size by more than two to one here, so ownership has to be per table
     * rather than per range.
     */
    static constexpr std::uint8_t shadow_table_free = 0;

    std::uint8_t shadow_ept_table_slot[max_cpus]
                                      [shadow_ept_tables_per_cpu]{};

    /**
     * The slot each processor is building into or resuming with. Held
     * rather than passed, so that the table allocator and the leaf
     * installer do not each need it threaded through them.
     */
    std::size_t shadow_ept_current_slot[max_cpus]{};

    /**
     * The slot to replace when every one is in use, advanced on each
     * replacement.
     *
     * Round robin rather than least recently used. The set is four and
     * the workload alternates between two, so the two differ only when
     * the set is genuinely too small - and at that point the right answer
     * is a bigger set, which the counters below make visible.
     */
    std::size_t shadow_ept_next_victim[max_cpus]{};

    /**
     * Whether a shadow was found for the pointer asked for, or had to be
     * built. A cache that never hits is a cache that is the wrong shape,
     * and these are how that shows rather than being argued about.
     * @{
     */
    std::uint64_t shadow_ept_cache_hits[max_cpus]{};
    std::uint64_t shadow_ept_builds[max_cpus]{};

    /**
     * Why a shadow root had to be rebuilt, split because the two have
     * different fixes and the total cannot tell them apart.
     *
     * `new_root` is the guest hypervisor naming tables this processor has
     * not shadowed, which is what the slot set exists to absorb.
     * `stale` is a root this processor *had* shadowed, discarded because
     * `ept_generation` moved - and that counter is global, so a
     * permission change on a single page invalidates every shadow root on
     * every processor. Each discard is then refilled one extended-page-
     * table fault at a time.
     */
    /**
     * The guest-physical addresses each shadow slot's root actually had
     * mapped, so a rebuild of that same root can install them again
     * instead of taking an extended-page-table exit per page.
     *
     * **Why a remembered set rather than a heuristic.** Installing the
     * eight pages *after* each faulting one was tried and reverted:
     * 1,407,690 leaves prefetched, 3.2 a fault, and the fault count fell
     * 5% - so about 95% was never touched, and the fault got 60% dearer
     * paying for it. The 26.7 refaults a rebuilt root takes are scattered
     * addresses, not a run, and adjacency cannot find them. This does not
     * guess: it replays what the guest itself demonstrated it needs.
     *
     * Sized at 64 against a measured 26.7. A root that maps more than
     * that keeps the first 64 and faults for the rest, which degrades to
     * exactly the behaviour without this.
     *
     * **256 was tried on the rig and is a net loss. Do not raise this
     * without re-measuring the replay's cost, which is far higher than
     * it looks.** The case for raising it was good and the arithmetic
     * behind it was wrong:
     *
     * - the set was *saturated* at 64 - 600,295 replays over 9,531
     *   rebuilds is 63.0 each, one below the cap - with 159,015 faults,
     *   17.9 a rebuild, on top. Extended-page-table violations were
     *   51.1% of every exit on the machine.
     * - at 256 the faults collapsed exactly as predicted: **17.9 a
     *   rebuild became 1.17**, and violations fell from 51.1% of exits
     *   to **7.6%**.
     * - and it was still slower. `shadow_ept_pointer_for` went from
     *   **7,079 to 247,362 cycles a call**, `build_vmcs02` from 101,642
     *   to 603,822, wall clock per exit from 387,072 to 827,989, and
     *   `nested_run/s` roughly halved.
     *
     * **What the estimate got wrong is the price of one replay.** It was
     * taken as "a table walk of a few thousand cycles" against a fault
     * worth ~600,000 - the latter being wall clock per exit averaged
     * over every reason, which is not the marginal cost of an
     * extended-page-table fault. The real numbers: `on_l2_ept_fault` is
     * **41,826 cycles**, and a replay walks four levels through
     * `read_guest_physical`, each level re-pointing the shared mapping
     * window at ~955 cycles - 48,001,634 `map_window` calls and 45.9
     * billion cycles in one run, about **15,000 cycles a replay**.
     *
     * So break-even is around three replays per fault avoided. At 64 the
     * ratio is already 3.5; going to 256 bought 16.7 fewer faults for
     * 167 more replays, or **ten replays a fault**. The mechanism is
     * paying for pages a long-lived root once touched and no longer
     * needs, because the set is only reset when a slot changes root.
     *
     * Two things would change this verdict and both are worth more than
     * a bigger cap:
     *
     * - **make a replay cheap.** Two thirds of it is `map_window_at`
     *   re-pointing the window and issuing `invlpg` for a page it may
     *   already be pointing at. A walk's four levels and consecutive
     *   replays share upper-level tables constantly.
     * - **give the set an eviction policy**, so it holds what the root
     *   currently needs rather than everything it has ever touched.
     */
    static constexpr std::size_t shadow_ept_recall_capacity = 64;
    std::uint64_t shadow_ept_recall[max_cpus][shadow_ept_slots]
                                   [shadow_ept_recall_capacity]{};
    std::size_t shadow_ept_recall_count[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_recall_root[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_replayed[max_cpus]{};

    void remember_shadow_page(std::size_t cpu,
                              std::uint64_t guest_physical);

    /**
     * Reads an enlightened VMCS into the cached vmcs12. Nothing calls it
     * yet: the advertisement that makes the guest hypervisor use the
     * structure is deliberately the last step, because advertising an
     * interface with nothing behind it is the failure
     * `announce_hypervisor` already made.
     */
    void copy_enlightened_to_vmcs12(
        std::size_t cpu, const hyperv::enlightened_vmcs & evmcs);

    /**
     * Writes back what the guest hypervisor reads after an exit, since
     * with the enlightenment armed it cannot VMREAD them.
     */
    void copy_vmcs12_to_enlightened(std::size_t cpu,
                                    hyperv::enlightened_vmcs & evmcs);

    /**
     * Where the *guest hypervisor* put its own assist page, written
     * through HV_X64_MSR_VP_ASSIST_PAGE.
     *
     * Not to be confused with `l2_vp_assist`, which is the second
     * level's page - Windows registering with Hyper-V, observed from
     * outside. This one is Hyper-V registering with *this VMM*, and it
     * only ever arrives because the enlightenment is advertised.
     */
    /**
     * Which hypercalls the guest hypervisor makes before declining the
     * enlightenment, and how often. Recorded rather than guessed from
     * the specification: it offers many and this VMM needs to know the
     * few that are actually asked for.
     */
    static constexpr std::size_t hypercall_code_slots = 16;
    std::uint64_t hypercall_codes[hypercall_code_slots]{};
    std::uint64_t hypercall_code_counts[hypercall_code_slots]{};

    /**
     * The same census for the **second-level** guest's hypercalls, which
     * nothing has ever taken.
     *
     * `hypercall_codes` above is gated on `from_guest_hypervisor`, so it
     * counts the level above's calls only. `on_l2_exit` decodes exactly
     * three L2 codes by name - `0x11`, `0x12` and `0x0c` - and counts
     * none of them as a distribution.
     *
     * Why it matters: `vmcall` is 3.46 per trust-level round trip and two
     * of those are the `HvCallVtlCall`/`HvCallVtlReturn` pair that
     * *defines* the round trip, so about 1.46 a round trip are something
     * nobody has named. If they are TLB-flush calls there is an
     * architecturally sanctioned answer at this level - KVM handles
     * `EXIT_REASON_VMCALL` in L0 under the direct-flush enlightenment,
     * `.references/kvm/nested.c` - and that is a whole interface under a
     * negotiated contract rather than a partial answer. If they are
     * `0x0c` protection-mask calls, that route is closed.
     *
     * A linear table for the same reason as the one above: the codes are
     * sparse and the interesting set is small.
     */
    /**
     * VMREADs issued by `materialise_l2_guest_state`, so its cycles can
     * be split into VMCS traffic and software.
     *
     * The distinction decides whether it is a lever at all: reads scale
     * down on bare metal and are already inside the projection, software
     * survives and is the only part worth attacking.
     */
    volatile std::uint64_t materialise_reads[max_cpus]{};

    /**
     * Leaves installed speculatively beside a faulting one, and the walks
     * that produced nothing.
     *
     * The pair is the point: installs alone cannot say whether the guest
     * ever used them, and `refused` rising far faster than `filled` means
     * the window is mostly holes and every one of them cost four
     * guest-physical reads. See `nested_vmx::eager_ept_neighbours`.
     */
    volatile std::uint64_t shadow_ept_neighbours_filled[max_cpus]{};
    volatile std::uint64_t shadow_ept_neighbours_refused[max_cpus]{};

    void install_shadow_neighbours(std::size_t cpu,
                                   std::uint64_t page,
                                   std::uint64_t shift,
                                   std::uint64_t eptp12);

    /**
     * Periodic telemetry to the disk channel, for a machine with no
     * monitor.
     *
     * **Every instrument this project's investigation used goes through
     * the QEMU monitor, and bare metal has none** - no serial port, no
     * debugger, and the screen belongs to the guest
     * (`uefi_loader/src/main.cpp`). Without this a physical boot reports
     * exactly one bit: does the spinner move.
     *
     * What is emitted is the five counters `scripts/entry_poll.py`
     * samples, because they are what decides the verdict - the deferred
     * call vector still being delivered, entries still occurring below
     * DISPATCH, extended-page-table faults still happening, and the
     * round-trip and tick rates whose ratio is the threshold itself.
     * Nothing more: every record is disk traffic on the machine under
     * test.
     *
     * The sequence number is not decoration. A lost block has to read as
     * a **gap** rather than as a plateau, or a silent instrument looks
     * exactly like a settled guest - which is the failure this whole
     * investigation kept meeting.
     * @{
     */
    std::uint64_t telemetry_last_tsc[max_cpus]{};
    std::uint64_t telemetry_sequence[max_cpus]{};

    /**
     * About two seconds at the rig's measured 1.992 GHz.
     *
     * Two rather than the four `entry_poll.py` polls at, because the
     * transition it has to catch happens at about t=53 and lasts less
     * than one sample - the poller saw it in a single 4.1 second bin.
     * Twice the resolution for one record every two seconds, which at
     * 128 bytes a record fills a 4 KB block about once a minute.
     */
    static constexpr std::uint64_t telemetry_period_cycles = 4000000000ull;

    void emit_disk_telemetry(std::size_t cpu);
    /** @} */

    std::uint64_t l2_hypercall_codes[hypercall_code_slots]{};
    std::uint64_t l2_hypercall_code_counts[hypercall_code_slots]{};
    std::uint64_t hypercalls_seen{};

    std::uint64_t hyperv_vp_assist[max_cpus]{};
    std::uint8_t evmcs_armed[max_cpus]{};

    bool load_enlightened_vmcs(std::size_t cpu);
    void store_enlightened_vmcs(std::size_t cpu);
    std::uint64_t hyperv_vp_assist_writes[max_cpus]{};

    /** How often the enlightenment was recommended - to a guest in VMX
     * operation, which is the only kind it is meant for. */
    std::uint64_t evmcs_recommended[max_cpus]{};

    std::uint64_t evmcs_reads[max_cpus]{};
    std::uint64_t evmcs_writes[max_cpus]{};
    void replay_shadow_recall(std::size_t cpu,
                              std::size_t slot,
                              std::uint64_t root);

    std::uint64_t shadow_ept_rebuild_new_root[max_cpus]{};
    std::uint64_t shadow_ept_rebuild_stale[max_cpus]{};

    /**
     * Which `invept` type the guest hypervisor issues, split because the
     * two reach different code and only one of them is a candidate.
     *
     * `single_context` already releases only the slot naming that root -
     * `discard_shadow_ept_for` - so if it dominates, this VMM is already
     * as targeted as the instruction allows and there is nothing to win
     * by narrowing the discard. `all_context` releases **every** slot,
     * and `discard_shadow_ept`'s own comment says it discards more than
     * was asked for because that is "the safe direction". Whether that
     * is costing anything depends entirely on which type arrives, and
     * nothing in this tree has ever counted it.
     *
     * Measured first, changed second. The premise of the whole
     * "keep the tables the way KVM keeps previous roots" idea is that
     * roots are being thrown away that were not named, and that premise
     * is false if the guest hypervisor only ever issues single-context.
     * @{
     */
    std::uint64_t l2_invept_single_context[max_cpus]{};
    std::uint64_t l2_invept_all_context[max_cpus]{};
    /** @} */

    /**
     * The five guest-state fields the processor saves into vmcs02 on
     * every VM exit, as `save_l2_state` read them out - so that
     * `build_vmcs02` can skip writing back a value vmcs02 demonstrably
     * still holds.
     *
     * **Why this is sound without tracking who wrote what.** The
     * comparison is against the *value*, not against a dirty bit, so it
     * covers every writer of the cached vmcs12 alike: the guest
     * hypervisor's intercepted VMWRITEs, this VMM's own RIP advance, the
     * injection path. If any of them changed the value, it differs from
     * what was saved and the write happens.
     *
     * And vmcs02 cannot change underneath the recording: `save_l2_state`
     * reads these while vmcs02 is current, immediately after the exit;
     * vmcs01 is then made current to run the guest hypervisor; and the
     * only VMPTRLD of vmcs02 in the tree is the one in `build_vmcs02`
     * itself. So between the record and the use, no instruction can
     * write vmcs02's guest state.
     *
     * The two preconditions are the ones the deferred read copy already
     * pays for and `tests/nested_exit` already covers: vmcs02 must have
     * been launched, or nothing was ever saved into it; and the vmcs12
     * being entered must be the one that was saved from, since vmcs02 is
     * reused per processor.
     */
    static constexpr std::size_t hot_state_count = 5;
    std::uint64_t hot_state_saved[max_cpus][hot_state_count]{};
    std::uint64_t hot_state_vmcs[max_cpus]{};
    bool hot_state_valid[max_cpus]{};
    std::uint64_t hot_state_writes_skipped[max_cpus]{};
    std::uint64_t hot_state_writes_done[max_cpus]{};

    /**
     * How much wall clock each level actually executes for, split by
     * which one was resumed into.
     *
     * **Every other figure in this tree is denominated in this VMM's own
     * work** - cycles per exit, per entry, per phase - and four changes
     * worth a reproducible 1.9x of that moved no guest-facing indicator
     * at all. The quantity `ZPP_STRETCH_GUEST_TIMER` moved was the
     * second-level guest's wall-clock budget between ticks, and nothing
     * here has ever measured it.
     *
     * The span runs from the resume that enters a level to the first
     * instruction of this VMM's code after the exit that leaves it, so
     * it carries the VM transition with it. That overstates both levels
     * slightly and by the same amount each way; the ratio is what is
     * being asked for.
     */
    std::uint64_t level_run_tsc[max_cpus]{};
    bool level_run_was_l2[max_cpus]{};
    std::uint64_t l2_run_cycles[max_cpus]{};
    std::uint64_t l1_run_cycles[max_cpus]{};
    std::uint64_t shadow_ept_evictions[max_cpus]{};

    /**
     * Pool pressure on the fault path.
     *
     * `reclaims` is a fill that had to drop this processor's *other*
     * shadows to find a table, which costs those shadows the faults to
     * fill again. `resets` is one that had to drop its own as well, and
     * means a single shadow no longer fits the pool - the one number here
     * that says "make the pool bigger" rather than "this is working".
     * @{
     */
    std::uint64_t shadow_ept_reclaims[max_cpus]{};
    std::uint64_t shadow_ept_resets[max_cpus]{};
    /**
     * @}
     */
    /**
     * @}
     */
    /**
     * @}
     */

    /**
     * How much of each processor's pool is in use, and what its shadow was
     * built from.
     *
     * `shadow_ept_source` holds bits 51:12 of the guest hypervisor's own
     * EPT pointer, or zero for a shadow never built. Compared rather than
     * the whole pointer, because SDM 31.4.2 defines the EPTRTA as bits
     * 51:12 and associates mappings with it - so two pointers differing
     * only in memory type or page-walk length describe the same tables and
     * need no rebuild.
     *
     * `shadow_ept_generation_seen` is this VMM's own ept_generation when
     * the shadow was built. Ours changing - a page watch armed, a region
     * protected - invalidates every shadow composed over it, and comparing
     * generations is how that is noticed without an interprocessor
     * interrupt, exactly as the existing catch-up on the exit path does.
     * @{
     */
    std::size_t shadow_ept_tables_used[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_source[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_generation_seen[max_cpus][shadow_ept_slots]{};
    std::uint64_t shadow_ept_pointer[max_cpus][shadow_ept_slots]{};
    /**
     * @}
     */

    /**
     * What each processor's last shadow build covered and spent.
     *
     * The build cost is what decides whether eager construction stays -
     * BACKLOG.md names it as the measurement that would justify moving to
     * lazy fill - and it cannot be measured on a machine nobody can attach
     * to unless it is recorded here.
     * @{
     */
    std::uint64_t shadow_ept_splits[max_cpus]{};

    /**
     * Mappings installed into an existing shadow by a second-level fault,
     * rather than by building the shadow.
     *
     * These are the pages a guest hypervisor mapped *after* its shadow was
     * built, which it is entitled to do without invalidating anything -
     * SDM 31.4.3.3 requires INVEPT when an entry becomes more restrictive,
     * not when it becomes less. A shadow built from a walk cannot learn
     * about them any other way, and before this each one was an exit that
     * resumed unchanged and faulted again for ever.
     */
    std::uint64_t shadow_ept_leaves_filled[max_cpus]{};

    /** The permission bits every shadow leaf was installed with. All
     * of it in slot 7 means the composition never removes anything,
     * which is what `reflected_permission` being zero would follow
     * from. See install_shadow_leaf. */
    volatile std::uint64_t shadow_leaf_permissions[max_cpus][8]{};

    /**
     * What the guest hypervisor's own tables grant for the same leaf,
     * before this VMM's are composed in - the logical-AND of read, write
     * and execute across every entry `eptp12`'s walk used.
     *
     * The pair is the instrument, not either one alone.
     * `shadow_leaf_permissions` says what this VMM installed; this says
     * what it was given. A composed side stuck at 7 means nothing until
     * it is known whether the guest side was ever anything else.
     *
     * The question it settles has been open in this file for several
     * sessions: `HvCallModifyVtlProtectionMask` is issued for ever and
     * no access has ever been refused by anything this VMM shadows.
     * Either the level above does not express that protection through
     * its extended page tables - and this histogram is all sevens - or
     * it does and the composition is discarding it, which would be this
     * VMM's bug and would explain a protection change that never takes
     * effect and is therefore asked for again.
     */
    volatile std::uint64_t guest_leaf_permissions[max_cpus][8]{};

    /**
     * The last `vtl_protect_capacity` `HvCallModifyVtlProtectionMask`
     * control words, the partition id beside each, and the answer the
     * guest hypervisor gave.
     *
     * **Decoded, never censused raw.** RCX is structured -
     * `.references/xen/xen/arch/x86/include/asm/guest/hyperv-tlfs.h:419-425`
     * - with the call code in bits 15:0, the fast form in bit 16, the
     * rep count in bits 43:32 and the rep start index in bits 59:48. A
     * census over the whole register would report one distinct value
     * whenever the rep start happens to be stable and would hide the
     * field being read, which is the failure this replaces: the previous
     * instrument censused RBP, a frame pointer, and its incrementing
     * values were read as page numbers until a later boot showed the
     * same field at zero.
     *
     * The two fields answer the two open questions with no address to
     * interpret. Rep count turns a call rate into a page rate. Rep start
     * says whether the caller is resuming a call that timed out - a rep
     * hypercall that cannot finish its slice returns
     * `HV_STATUS_TIMEOUT` with reps-completed set, and the caller
     * reissues from where it stopped. Advancing is progress; returning
     * to zero is a new request; **not advancing while the call repeats
     * is a livelock**.
     *
     * `vtl_protect_rdx` is kept as a check rather than as data: the
     * value is `HV_PARTITION_ID_SELF`, the first field of the input
     * header, so it confirms the register order the decode assumes.
     *
     * The answer is collected at the next second-level entry, where the
     * guest hypervisor has loaded its guest's registers - the same point
     * and the same mechanism the reference-counter answer uses. Bits
     * 15:0 of RAX are the status, bits 43:32 the reps completed. **Note
     * which level answers**: this call is the guest hypervisor's guest
     * asking the guest hypervisor, so the status is Hyper-V's and not
     * this VMM's. Reps completed coming back zero would make the
     * question "what is this VMM doing to Hyper-V that stops it
     * finishing a rep", which is where shadow-table retention would
     * reconnect on evidence rather than by analogy.
     * @{
     */
    static constexpr std::size_t vtl_protect_capacity = 32;
    std::uint64_t vtl_protect_rcx[max_cpus][vtl_protect_capacity]{};
    std::uint64_t vtl_protect_rdx[max_cpus][vtl_protect_capacity]{};
    std::uint64_t vtl_protect_rax[max_cpus][vtl_protect_capacity]{};
    std::uint64_t vtl_protect_count[max_cpus]{};
    std::size_t vtl_protect_answer_slot[max_cpus]{};
    bool vtl_protect_answer_pending[max_cpus]{};

    /**
     * The control word of an `HvCallVtlCall`, which is **not** a rep
     * hypercall - so its rep count and rep start must both decode to
     * zero.
     *
     * The decode's own control. If a call known to carry no reps comes
     * back with a nonzero rep count, the shifts are wrong and nothing
     * measured with them counts. This is the "cannot be anything else"
     * check that was missing when an incrementing frame pointer was
     * believed to be a page number because 4.70 GiB is a plausible place
     * for one.
     */
    std::uint64_t vtl_call_rcx[max_cpus]{};
    /** @} */
    /** @} */
    /** @} */

    /**
     * The mappings a shadow held when the guest hypervisor invalidated
     * it, so they can be re-walked instead of re-faulted.
     *
     * INVEPT was 58% of every exit this VMM took, second hand: 464,815
     * EPT violations against 15,896 INVEPTs, one shadow thrown away per
     * INVEPT and twenty-eight faults to fill it back in. The guest
     * hypervisor is applying VTL protections one page at a time and
     * invalidates after each, so it changed *one* mapping and this VMM
     * discarded every mapping it had.
     *
     * Re-walking costs the same twenty-eight walks of the guest
     * hypervisor's tables - but in root operation, where a walk is four
     * memory reads and not a VM exit. That is the whole trade.
     *
     * Bounded, and the bound is the safety property rather than a
     * limitation. Once shadows stop being wiped they grow, and a refresh
     * costs proportional to the shadow rather than to the change; past
     * this many mappings the old behaviour - discard, and let them fault
     * back - is cheaper. Overflow is counted, so a bound set too low
     * shows up as a number rather than as a mystery.
     * @{
     */
    static constexpr std::size_t shadow_ept_refresh_capacity = 2048;

    struct shadow_ept_leaf
    {
        std::uint64_t guest_physical{};
        std::uint64_t shift{};
    };

    shadow_ept_leaf shadow_ept_refresh_list[max_cpus]
                                           [shadow_ept_refresh_capacity]{};
    std::uint64_t shadow_ept_refreshes[max_cpus]{};
    std::uint64_t shadow_ept_refresh_leaves[max_cpus]{};
    std::uint64_t shadow_ept_refresh_overflows[max_cpus]{};

    void refresh_shadow_ept_for(std::size_t cpu, std::uint64_t root);
    std::size_t collect_shadow_leaves(std::size_t cpu, std::size_t slot);
    /** @} */

    /**
     * What was done about a second-level extended-page-table fault.
     *
     * Every one of these is a correct answer to *some* fault and a
     * livelock in answer to the wrong one, which is why the fault path
     * records which it chose rather than only that it chose. An exit that
     * repeats is not evidence about the branch taken - all of them repeat
     * when they are wrong, and they repeat identically.
     */
    enum class l2_ept_disposition : std::uint64_t
    {
        none,

        /** The guest hypervisor runs its guest on this VMM's own tables.
         */
        without_ept,

        /** Its tables do not map the address. */
        reflected_walk,

        /** Its tables hold a value the processor rejects. */
        reflected_misconfiguration,

        /** Its tables map the address and refuse the access. */
        reflected_permission,

        /** This VMM's tables refuse it, and something here watches it. */
        watched,

        /** This VMM's tables refuse it, and nothing here watches it. */
        unwatched,

        /** Both levels permit it, and a mapping was installed. */
        installed,

        /** Both permit it, and the mapping could not be installed. */
        install_failed,

        /** Both permit it, and no shadow could be obtained at all. */
        pointer_failed,
    };

    /**
     * How many identical second-level faults in a row are a livelock
     * rather than a busy page.
     *
     * An EPT violation retires no instruction - SDM 30.2 has the saved
     * instruction pointer address the faulting instruction - so the *same*
     * fault, at the same instruction pointer, for the same guest-physical
     * address, is by definition the same access being re-attempted and not
     * satisfied. There is no legitimate reason for five hundred of them:
     * one fault installs the mapping, and the next access to that page
     * does not fault at all.
     *
     * Five hundred and twelve rather than a handful, because a page whose
     * mapping is genuinely re-taken - a watched one stepped over, a shadow
     * slot evicted under pressure - can repeat a few times honestly, and a
     * detector that cries at three would be turned off.
     */
    static constexpr std::uint64_t l2_ept_stall_threshold = 512;

    /**
     * The fault each processor last took, so a repeat can be recognised.
     *
     * Compared among faults only, ignoring whatever exits happen in
     * between: an external interrupt arriving mid-livelock does not make
     * the livelock stop, and a detector reset by one would never fire.
     * @{
     */
    std::uint64_t l2_ept_fault_rip[max_cpus]{};
    std::uint64_t l2_ept_fault_address[max_cpus]{};
    std::uint64_t l2_ept_fault_qualification[max_cpus]{};
    std::uint64_t l2_ept_fault_repeats[max_cpus]{};
    /** @} */

    /**
     * A second-level guest making no forward progress, and everything
     * needed to say why without another boot.
     *
     * This exists because of what finding the last one cost. The symptom
     * is one line - the same instruction pointer, the same qualification,
     * for ever - and it is the same line whichever of nine branches
     * produced it. Every one of those branches is reached from state that
     * is gone by the time anything reads a counter: the guest
     * hypervisor's own walk, this VMM's, and what the two composed to.
     * So they are captured at the moment the repeat count crosses the
     * threshold, which is the last moment they are all in hand.
     *
     * Written once per processor and then left alone, on purpose. The
     * first stall is the one that explains the boot; the millionth repeat
     * of it explains nothing further, and overwriting would lose the
     * transition that led in.
     */
    struct l2_ept_stall_record
    {
        std::uint64_t occurred{};
        std::uint64_t repeats{};

        std::uint64_t rip{};
        std::uint64_t guest_physical{};
        std::uint64_t qualification{};
        std::uint64_t ept_pointer{};

        /** The walk of the guest hypervisor's own tables. */
        std::uint64_t guest_walk_status{};
        std::uint64_t guest_walk_physical{};
        std::uint64_t guest_walk_shift{};
        std::uint64_t guest_walk_permissions{};

        /** This VMM's own translation of what that walk produced. */
        std::uint64_t host_walk_status{};
        std::uint64_t host_walk_permissions{};

        /** What the two composed to. */
        std::uint64_t composition_outcome{};
        std::uint64_t composition_shift{};
        std::uint64_t composition_permissions{};

        /** What the shadow held for the address at that moment. */
        std::uint64_t shadow_status{};
        std::uint64_t shadow_permissions{};

        l2_ept_disposition disposition{};
    };

    l2_ept_stall_record l2_ept_stall[max_cpus]{};

    /**
     * Mappings installed by a fault that did not permit the access which
     * caused it, counted.
     *
     * A guaranteed livelock, and the only counter here that should be
     * zero on every run: the handler resumed a guest whose next act is to
     * take the identical fault. Non-zero means the composition and the
     * installation disagree about what was granted.
     */
    std::uint64_t shadow_ept_leaves_that_did_not_help[max_cpus]{};

    /**
     * How each fault was answered, so the branch is a number rather than a
     * re-derivation.
     */
    std::uint64_t
        l2_ept_dispositions[max_cpus]
                           [1 + static_cast<std::size_t>(
                                    l2_ept_disposition::pointer_failed)]{};
    /**
     * @}
     */

    /**
     * The processor's physical-address width, or zero before it is read.
     */
    std::uint64_t cached_physical_address_bits{};

    /**
     * The MSR bitmap of the VM control structure.
     */
    alignas(page_size) std::uint8_t msr_bitmap[page_size]{};

    /**
     * The two I/O permission bitmaps, one bit per port: the first covers
     * ports 0x0000 to 0x7fff and the second the rest. All zero, so
     * nothing exits until a port is deliberately armed - the guest owns
     * every device this VMM does not, and trapping its I/O would be both
     * ruinous and pointless.
     * @{
     */
    alignas(page_size) std::uint8_t io_bitmap_a[page_size]{};
    alignas(page_size) std::uint8_t io_bitmap_b[page_size]{};
    /**
     * @}
     */

    // The VMXON and VMCS region addresses used to live here, written by
    // initialize_vmx and read back by enter_root_mode. They are locals of
    // enter_root_mode now, derived from its own slot. Every other member
    // below is one value shared by every processor by design; those two
    // were per-processor state kept in a shared place, and the window
    // between the write and the VMXON belonged to whichever processor
    // wrote last. Do not put them back.

    /**
     * The physical address of the hardware page table level 4.
     */
    std::uint64_t epml4_physical{};

    /**
     * The physical address of the MSR bitmap.
     */
    std::uint64_t msr_bitmap_physical{};
    std::uint64_t io_bitmap_a_physical{};
    std::uint64_t io_bitmap_b_physical{};

    /**
     * Where the guest writes to enter a sleep state, as the loader found
     * it, or zero when it found none.
     * @{
     */
    std::uint16_t sleep_control_port{};
    std::uint16_t sleep_control_port_secondary{};

    /**
     * How wide it is, in bytes. Only used on the path that performs the
     * guest's write itself, which has to perform it at the register's own
     * width rather than at a convenient one.
     */
    std::uint8_t sleep_control_width{};

    /**
     * The physical address of the firmware ACPI control structure, or
     * zero. Holds the firmware waking vector - see
     * observe_guest_waking_vector.
     */
    std::uint64_t sleep_facs_physical{};

    /**
     * The waking vector the guest left in that table, read on the way into
     * a sleep state.
     *
     * The address the platform will jump to in real mode when it resumes,
     * which on an unmodified machine is the guest's own resume trampoline.
     * Recorded because a resume path has to enter the guest *there* - the
     * guest is expecting to continue from its own trampoline, and anywhere
     * else is a guest that has been lied to.
     *
     * Zero means either that nothing has read it yet or that the guest
     * left none, and the two are told apart by sleep_request.occurred.
     */
    std::uint64_t guest_waking_vector{};

    /**
     * The extended waking vector the guest left there, saved because
     * arming a resume zeroes it.
     *
     * Zeroing it is what forces the real mode protocol, which is the only
     * one this VMM can serve: EDK2's S3Resume.c takes the sixteen bit
     * vector when XFirmwareWakingVector is zero and a protected- or long
     * mode path otherwise. Saved rather than discarded so that the guest's
     * own table can be put back exactly as it was - see
     * disarm_resume_from_sleep.
     */
    std::uint64_t guest_extended_waking_vector{};
    /**
     * @}
     */
    /**
     * @}
     */
};

/**
 * The hypervisor error category.
 */
inline const zpp::error_category & category(hypervisor::error)
{
    static constexpr auto error_category = zpp::make_error_category(
        "hypervisor::error",
        hypervisor::error::success,
        [](auto code) -> std::string_view {
            switch (code) {
            case hypervisor::error::success:
                return zpp::error::no_error;
            case hypervisor::error::vmxon_failed:
                return "vmxon failed";
            case hypervisor::error::vmptrld_failed:
                return "vmptrld failed";
            case hypervisor::error::vmclear_failed:
                return "vmclear failed";
            case hypervisor::error::physical_to_virtual_capacity_error:
                return "Physical to virtual capacity error";
            case hypervisor::error::out_of_ept_entries:
                return "Out of EPT entries";
            case hypervisor::error::host_exception:
                return "Host exception caught by the host IDT";
            case hypervisor::error::vmx_disabled_by_firmware:
                return "VMX locked off in IA32_FEATURE_CONTROL";
            case hypervisor::error::too_many_processors:
                return "More processors than max_cpus";
            case hypervisor::error::ept_not_initialized:
                return "An EPT entry was asked for before the tables "
                       "were built";
            case hypervisor::error::controller_not_available:
                return "The channel's controller is not known";
            case hypervisor::error::acknowledgement_timed_out:
                return "A processor did not acknowledge an EPT change";
            case hypervisor::error::controller_never_settled:
                return "The controller did not reach the wanted CSTS.RDY";
            case hypervisor::error::excursion_refused:
                return "The controller refused an excursion command";
            case hypervisor::error::vmxoff_failed:
                return "vmxoff failed, still in VMX operation";
            case hypervisor::error::no_region_for_processor:
                return "No VMXON or VMCS region for this processor";
            case hypervisor::error::guest_address_not_mapped:
                return "A guest linear address does not translate";
            case hypervisor::error::guest_memory_unreachable:
                return "Guest physical memory is out of the window's "
                       "reach";
            case hypervisor::error::no_usable_facs:
                return "No usable firmware ACPI control structure";
            case hypervisor::error::no_waking_vector:
                return "No waking vector to resume through";
            case hypervisor::error::out_of_shadow_ept_tables:
                return "Out of shadow extended page table pages";
            case hypervisor::error::nested_controls_unsupported:
                return "Nested VM-execution controls out of range";
            case hypervisor::error::nested_host_state_unsupported:
                return "Nested host state out of range";
            case hypervisor::error::nested_msr_area_unsupported:
                return "Nested MSR-area names an MSR we will not touch";
            case hypervisor::error::execute_disable_not_enabled:
                return "IA32_EFER.NXE is clear, so the host page table's "
                       "execute disable bit is a reserved bit";
            case hypervisor::error::module_protection_missing:
                return "A store into this module's own text was allowed";
            }
        });
    return error_category;
}

} // namespace zpp::hypervisor
