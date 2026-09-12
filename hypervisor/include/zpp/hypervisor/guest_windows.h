#pragma once
#include <cstdint>

namespace zpp::hypervisor::guest_windows
{
/**
 * Field offsets in the guest operating system's own structures.
 *
 * **These are build-specific and this VMM cannot verify them.** They come
 * from the public debug symbols for the exact `ntoskrnl.exe` on the
 * machine under test, read with
 *
 *     llvm-pdbutil dump --types --type-index=<field list> ntkrnlmp.pdb
 *
 * after fetching the file from Microsoft's symbol server using the GUID
 * in the image's own CodeView debug directory - `scripts/guest-symbols.sh`
 * does both. A different Windows build moves them, and nothing here will
 * notice: the probe would follow a pointer read from the wrong place and
 * record a plausible number.
 *
 * That is why every use of them is a *diagnostic* and none of them is on
 * a path that decides anything. The rule this project applies to the SDM
 * applies here too - what is not verified is not acted on - and the guest
 * kernel's internals are exactly the kind of thing that cannot be
 * verified from inside a hypervisor.
 *
 * The values below are for the build these were read from:
 * `ntkrnlmp.pdb` GUID `C8A7F11B37FE28227B6B11412E3A0519` age 1. Override
 * them from CMake to follow a different one.
 *
 * The chain they describe, which is the whole reason they are here: the
 * VMCS holds the guest's GS base, which in kernel mode addresses the
 * processor control region; that names the processor control block; and
 * that names the thread the processor is running. From the thread comes
 * its start address, which symbolizes into a function name and so says
 * *which* thread it is - and its state and wait reason, which say what it
 * is doing.
 * @{
 */
#ifndef ZPP_WINDOWS_KPCR_CURRENT_PRCB
#define ZPP_WINDOWS_KPCR_CURRENT_PRCB 32
#endif

#ifndef ZPP_WINDOWS_KPRCB_CURRENT_THREAD
#define ZPP_WINDOWS_KPRCB_CURRENT_THREAD 8
#endif

/**
 * `KPRCB.InterruptRequest`, the byte that says a software interrupt is
 * outstanding on this processor.
 *
 * It is what `HalRequestSoftwareInterrupt` is asking for, and it is the
 * one reading that separates the two remaining stories. If it is set
 * whenever this is sampled, the guest has asked for a deferred call and
 * has not had it - which is the fault. If it is clear, the requests are
 * being serviced and the low delivery count of vector `0x2f` means only
 * that Windows drains the queue inline when it lowers priority, which it
 * does on real hardware too.
 */
#ifndef ZPP_WINDOWS_KPRCB_INTERRUPT_REQUEST
#define ZPP_WINDOWS_KPRCB_INTERRUPT_REQUEST 6
#endif

#ifndef ZPP_WINDOWS_KPRCB_IDLE_THREAD
#define ZPP_WINDOWS_KPRCB_IDLE_THREAD 24
#endif

#ifndef ZPP_WINDOWS_KTHREAD_STATE
#define ZPP_WINDOWS_KTHREAD_STATE 388
#endif

#ifndef ZPP_WINDOWS_KTHREAD_WAIT_IRQL
#define ZPP_WINDOWS_KTHREAD_WAIT_IRQL 390
#endif

#ifndef ZPP_WINDOWS_KTHREAD_WAIT_REASON
#define ZPP_WINDOWS_KTHREAD_WAIT_REASON 643
#endif

#ifndef ZPP_WINDOWS_ETHREAD_START_ADDRESS
#define ZPP_WINDOWS_ETHREAD_START_ADDRESS 1248
#endif

#ifndef ZPP_WINDOWS_KTHREAD_PROCESS
#define ZPP_WINDOWS_KTHREAD_PROCESS 544
#endif

/**
 * `_KTRAP_FRAME`, whose last five fields are the frame hardware pushes.
 *
 * These are the offsets that let a trap frame be located from the
 * hardware frame rather than the other way round.
 * `sample_interrupted_stack` already finds the five quadwords SDM 7.14.2
 * describes - RIP, CS, RFLAGS, RSP, SS - by their shape; subtracting
 * `ktrap_frame_rip` from the address of the first of them gives the base
 * of the enclosing `_KTRAP_FRAME`, and everything below is then a fixed
 * offset from it.
 *
 * Two things are wanted from there and neither is reachable any other
 * way:
 *
 * - **`PreviousIrql` is the interrupted thread's real interrupt request
 *   level.** `kthread_wait_irql` is not: `_KTHREAD.WaitIrql` records the
 *   level at which a thread called `KeWaitForSingleObject` and is stale
 *   for a thread that is not waiting, which is exactly the thread being
 *   sampled here. And the virtual task priority read from the
 *   virtual-APIC page is the *processor's* current priority, which
 *   inside an interrupt handler is the handler's and not the thread's.
 *   This byte is the only one that answers "what level was the
 *   interrupted code running at".
 * - **The interrupted general purpose registers.** An instruction
 *   pointer alone cannot tell a loop that is retrying the same work from
 *   one that is walking through it - the addresses being read and
 *   written are what separate those, and they are in this structure.
 *   See `interrupted_context` for what that settles.
 *
 * **Verified against `ntkrnlmp.pdb` rather than recalled**, by the type
 * stream: `_KTRAP_FRAME` is 400 bytes with `Rip` at 360, `SegCs` 368,
 * `EFlags` 376, `Rsp` 384 and `SegSs` 392 - the five consecutive
 * quadwords the shape search matches, and the last five fields of the
 * structure. Every other offset here comes from the same dump. As with
 * every offset above, these belong to one build of one operating system
 * and are overridable for that reason.
 * @{
 */
#ifndef ZPP_WINDOWS_KTRAP_FRAME_RIP
#define ZPP_WINDOWS_KTRAP_FRAME_RIP 360
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_PREVIOUS_IRQL
#define ZPP_WINDOWS_KTRAP_FRAME_PREVIOUS_IRQL 41
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_RCX
#define ZPP_WINDOWS_KTRAP_FRAME_RCX 56
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_RDX
#define ZPP_WINDOWS_KTRAP_FRAME_RDX 64
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_R8
#define ZPP_WINDOWS_KTRAP_FRAME_R8 72
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_RDI
#define ZPP_WINDOWS_KTRAP_FRAME_RDI 328
#endif

#ifndef ZPP_WINDOWS_KTRAP_FRAME_RSI
#define ZPP_WINDOWS_KTRAP_FRAME_RSI 336
#endif
/** @} */

#ifndef ZPP_WINDOWS_ETHREAD_THREAD_LIST_ENTRY
#define ZPP_WINDOWS_ETHREAD_THREAD_LIST_ENTRY 1400
#endif

#ifndef ZPP_WINDOWS_EPROCESS_THREAD_LIST_HEAD
#define ZPP_WINDOWS_EPROCESS_THREAD_LIST_HEAD 880
#endif

constexpr std::uint64_t kpcr_current_prcb = ZPP_WINDOWS_KPCR_CURRENT_PRCB;
constexpr std::uint64_t kprcb_current_thread =
    ZPP_WINDOWS_KPRCB_CURRENT_THREAD;
constexpr std::uint64_t kprcb_idle_thread = ZPP_WINDOWS_KPRCB_IDLE_THREAD;
constexpr std::uint64_t kprcb_interrupt_request =
    ZPP_WINDOWS_KPRCB_INTERRUPT_REQUEST;
constexpr std::uint64_t kthread_state = ZPP_WINDOWS_KTHREAD_STATE;
constexpr std::uint64_t kthread_wait_irql = ZPP_WINDOWS_KTHREAD_WAIT_IRQL;
constexpr std::uint64_t kthread_wait_reason =
    ZPP_WINDOWS_KTHREAD_WAIT_REASON;
constexpr std::uint64_t ethread_start_address =
    ZPP_WINDOWS_ETHREAD_START_ADDRESS;
constexpr std::uint64_t kthread_process = ZPP_WINDOWS_KTHREAD_PROCESS;
constexpr std::uint64_t ethread_thread_list_entry =
    ZPP_WINDOWS_ETHREAD_THREAD_LIST_ENTRY;
constexpr std::uint64_t eprocess_thread_list_head =
    ZPP_WINDOWS_EPROCESS_THREAD_LIST_HEAD;
constexpr std::uint64_t ktrap_frame_rip = ZPP_WINDOWS_KTRAP_FRAME_RIP;
constexpr std::uint64_t ktrap_frame_previous_irql =
    ZPP_WINDOWS_KTRAP_FRAME_PREVIOUS_IRQL;
constexpr std::uint64_t ktrap_frame_rcx = ZPP_WINDOWS_KTRAP_FRAME_RCX;
constexpr std::uint64_t ktrap_frame_rdx = ZPP_WINDOWS_KTRAP_FRAME_RDX;
constexpr std::uint64_t ktrap_frame_r8 = ZPP_WINDOWS_KTRAP_FRAME_R8;
constexpr std::uint64_t ktrap_frame_rdi = ZPP_WINDOWS_KTRAP_FRAME_RDI;
constexpr std::uint64_t ktrap_frame_rsi = ZPP_WINDOWS_KTRAP_FRAME_RSI;

/**
 * How many threads of the running one's process to record.
 *
 * The point is not a census. The processor is idle, so its *current*
 * thread is the idle thread and says nothing; what is wanted is the
 * thread that is blocked, and it is one of the system process's. Sixteen
 * is enough to reach it during Phase 1, when there are few, and bounds a
 * walk of a list this VMM cannot trust to be well formed.
 */
constexpr std::size_t thread_walk_limit = 16;

/**
 * Where `PsInitialSystemProcess` sits in the image.
 *
 * The system process is what the interesting threads belong to, and
 * reaching it from a running thread does not work: the processor is idle
 * whenever these offsets apply to it, so the thread found is the idle
 * thread and its process is the idle process, which has one thread per
 * processor and never any others. Both narrower rules were tried and
 * both failed that way.
 *
 * This is the global that names it directly. Segment 27 - `ALMOSTRO`, at
 * relative address 0xfc5000 - plus 0x1af0, read from the public symbols
 * with `llvm-pdbutil dump --publics`.
 */
#ifndef ZPP_WINDOWS_PS_INITIAL_SYSTEM_PROCESS
#define ZPP_WINDOWS_PS_INITIAL_SYSTEM_PROCESS 0xfc6af0
#endif

constexpr std::uint64_t ps_initial_system_process =
    ZPP_WINDOWS_PS_INITIAL_SYSTEM_PROCESS;

/**
 * How far to look for the kernel's image header, in 2 MB steps.
 *
 * The relative address above is useless without the address the image was
 * loaded at, which moves every boot and which nothing tells this VMM. It
 * is findable, though: any instruction pointer the second-level guest
 * exits with is inside the image, the image is 2 MB aligned - checked
 * against three boots, which put it at 0xfffff8047bc00000,
 * 0xfffff806a1000000 and 0xfffff802c7200000 - and its first two bytes are
 * `MZ`. So step down from the faulting address until the header appears.
 *
 * Sixty-four steps is 128 MB, comfortably more than the 21 MB image and
 * far short of wandering into unmapped memory for long: every read that
 * misses simply fails and costs a translation.
 */
constexpr std::size_t kernel_base_scan_limit = 64;
/**
 * @}
 */

} // namespace zpp::hypervisor::guest_windows
