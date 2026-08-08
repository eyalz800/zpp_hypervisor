#pragma once
#include "zpp/arch/x86_64/ap_start_up.h"
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
#include "zpp/hypervisor/log.h"
#include "zpp/hypervisor/nested_vmx.h"
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

private:
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
    void initialize_intermediate_gdt();

    /**
     * Load the intermediate GDT.
     */
    void load_intermediate_gdt();

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
     */
    volatile std::uint64_t resume_activity_state[max_cpus]{};
    volatile std::uint64_t resume_guest_rip[max_cpus]{};
    volatile std::uint64_t resume_guest_cs[max_cpus]{};

    volatile std::uint64_t emulated_writes{};
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
        void (*before_write)(void * context,
                             std::uint64_t page) = nullptr);

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
     * derived from the VPID rather than read out of vmx_physical and
     * vmcs_physical.
     *
     * Those two are single scalars that initialize_vmx overwrites on every
     * processor it runs on, so by the time any guest is running they name
     * whichever processor was virtualized last. That is correct where they
     * are used - between initialize_vmx and the launch on one processor,
     * under start_up_lock - and wrong anywhere reached from a VM exit.
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
     * What mode each processor's local APIC was in the last time this VMM
     * looked, which is at its launch and on every write it makes to
     * IA32_APIC_BASE.
     *
     * Kept because the decision it feeds is global and the state is not.
     * See note_apic_mode.
     */
    apic_mode observed_apic_mode[max_cpus]{};

    /**
     * Remove protection for unprotected guest memory.
     * We mainly need to use this memory from guest on UEFI boot.
     */
    void unprotect_guest_memory();

    /**
     * Initialize needed vmx structures.
     */
    void initialize_vmx();

    /**
     * Permit VMXON in IA32_FEATURE_CONTROL, which vmxon requires before it
     * will run at all. Per logical processor.
     */
    std::expected<void, zpp::error> enable_vmx_in_feature_control();

    /**
     * Enter root mode on the current CPU.
     */
    std::expected<void, zpp::error> enter_root_mode();

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
                        std::uint64_t vector);

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
     * in exit_trace.
     */
    void record_exit(arch::x86_64::vmx::exit_reason reason);

    /**
     * Ask the processor to deliver a general protection fault to the guest
     * on the next VM entry.
     *
     * This is how the VMM says "that instruction would have faulted on
     * real hardware" - the alternative, resuming as though it had
     * succeeded, hands the guest a result it never computed.
     */
    void inject_general_protection_fault();

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
    std::expected<void, zpp::error> build_shadow_ept(std::size_t cpu,
                                                     std::uint64_t eptp12);

    /**
     * Forces the next entry to rebuild. Called where something has changed
     * that the shadow was composed from - a guest hypervisor's INVEPT, or
     * a change to this VMM's own tables.
     */
    void discard_shadow_ept(std::size_t cpu);

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
                      arch::x86_64::vmx::exit_reason::basic_reason reason);
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
    [[noreturn]] void resume_guest(arch::x86_64::context & context,
                                   arch::x86_64::vmx::exit_reason reason,
                                   bool advance_rip);

    /**
     * Setup the VM control structure according to the given guest context,
     * and configured host fields.
     */
    void setup_vmcs(arch::x86_64::context & guest_context);

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
     */
    std::uint64_t host_cr0{};

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
    struct exit_trace_entry
    {
        std::uint64_t reason{};
        std::uint64_t qualification{};
        std::uint64_t activity_state{};
        std::uint64_t cs_selector{};
        std::uint64_t rip{};
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
     * Total exits recorded per CPU. Not reduced modulo the capacity, so it
     * also says how many exits happened in total and where the ring wraps
     * - the newest entry is at (count - 1) % capacity.
     */
    std::uint64_t exit_trace_count[max_cpus]{};

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
    struct start_up_handoff_state
    {
        /**
         * No hand-off in progress. Either this processor has never taken
         * an INIT exit, or its last start-up has already been applied. A
         * sender must let the hardware deliver.
         */
        static constexpr std::uint64_t none = 0;

        /**
         * The target is spinning in its INIT handler, in VMX root mode,
         * waiting for a vector to be handed to it. Only in this state may
         * a sender swallow the guest's write to the interrupt command
         * register.
         */
        static constexpr std::uint64_t software_wait = 1;

        /**
         * The target is parked in the wait-for-SIPI activity state and is
         * waiting on hardware. A sender must issue the guest's start-up
         * IPI, because nothing this VMM does will wake it.
         */
        static constexpr std::uint64_t hardware_wait = 2;

        /**
         * A vector has been handed over. The state is this plus the
         * vector, so that vector zero is still distinguishable from no
         * hand-off at all.
         */
        static constexpr std::uint64_t delivered = 3;

        /**
         * The state that carries the given start-up vector.
         */
        static constexpr std::uint64_t deliver(std::uint64_t vector)
        {
            return delivered + vector;
        }

        /**
         * Whether the given state carries a vector.
         */
        static constexpr bool is_delivered(std::uint64_t state)
        {
            return state >= delivered;
        }

        /**
         * The vector such a state carries.
         */
        static constexpr std::uint64_t vector(std::uint64_t state)
        {
            return state - delivered;
        }
    };

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
    bool vmcs02_launched[max_cpus]{};
    bool l2_entry_logged[max_cpus]{};
    /**
     * @}
     */

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
        bool & changed_memory);

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
     * The data pointed to by the GS register to be used by
     * the host VMM.
     */
    alignas(page_size) std::uint8_t gs_data[page_size]{};

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
    alignas(page_size) arch::x86_64::vmx::epte shadow_epml4[max_cpus][512];
    alignas(page_size) arch::x86_64::vmx::epte
        shadow_ept_tables[max_cpus][shadow_ept_tables_per_cpu][512];
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
    std::size_t shadow_ept_next_table[max_cpus]{};
    std::uint64_t shadow_ept_source[max_cpus]{};
    std::uint64_t shadow_ept_generation_seen[max_cpus]{};
    std::uint64_t shadow_ept_pointer[max_cpus]{};
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
    std::uint64_t shadow_ept_regions_built[max_cpus]{};
    std::uint64_t shadow_ept_splits[max_cpus]{};
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

    /**
     * The physical address of the current VMX region to be assigned.
     */
    std::uint64_t vmx_physical{};

    /**
     * The physical address of the current VMCS region to be assigned.
     */
    std::uint64_t vmcs_physical{};

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
            }
        });
    return error_category;
}

} // namespace zpp::hypervisor
