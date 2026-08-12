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

constexpr std::uint64_t kpcr_current_prcb = ZPP_WINDOWS_KPCR_CURRENT_PRCB;
constexpr std::uint64_t kprcb_current_thread =
    ZPP_WINDOWS_KPRCB_CURRENT_THREAD;
constexpr std::uint64_t kprcb_idle_thread = ZPP_WINDOWS_KPRCB_IDLE_THREAD;
constexpr std::uint64_t kthread_state = ZPP_WINDOWS_KTHREAD_STATE;
constexpr std::uint64_t kthread_wait_irql = ZPP_WINDOWS_KTHREAD_WAIT_IRQL;
constexpr std::uint64_t kthread_wait_reason =
    ZPP_WINDOWS_KTHREAD_WAIT_REASON;
constexpr std::uint64_t ethread_start_address =
    ZPP_WINDOWS_ETHREAD_START_ADDRESS;
/**
 * @}
 */

} // namespace zpp::hypervisor::guest_windows
