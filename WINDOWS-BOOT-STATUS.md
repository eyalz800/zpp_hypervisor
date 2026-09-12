# Nested Windows boot investigation

Updated 2026-09-12. The goal remains a verified Windows login or desktop
with Hyper-V running above zpp. This has **not** been achieved by this session.
Earlier appearances of `LogonUI.exe` and `dwm.exe` did not prove a login screen:
the user observed "Please wait" on one long-lived run. Keep that distinction.

## Current setup

The TinyCore rig is `tc@192.168.1.199`. Its RAM filesystem restores files from
a backup on reboot, so inspect its actual launcher after every host restart.
The current launch uses two vCPUs, 11,830 MiB, QEMU 11.0.3 with KVM, a split
interrupt controller and `intremap=off`. The physical NVMe and GPU are passed
through. The stack is KVM, zpp, Hyper-V, then Windows with VBS.

The debug manifest has `nested=1 shadowvmcs=1 reftsc=1 vcache=1 hand=1`,
with `diag=0 blocks=0 win=0`. Read the complete manifest from the deployed
artifact; these few fields are not a replacement for it. Do not enable the
KVM FIFO trace capture: prior runs corrupted the host kernel and pinned RAM.

## Changes verified in this session

- `8d0d3a9`: watched EPT stores retire by the decoded instruction length.
  Intel SDM 30.2.5 leaves the VM-exit length undefined for ordinary EPT
  operand faults, even when nonzero. Both allowed and filtered stores now
  advance context and VMCS RIP consistently. Regression cases failed 12
  assertions before the fix and pass after it. An unrelated nested-exit
  test fixture now initializes EFER to long mode before testing an SCE change.
- `9cbebd2`: process and power-IRP readers use complete, prompt-framed monitor
  replies. They reject incomplete lists, and the watcher reports failed power
  reads as `UNREADABLE`. A three-process read on the rig took 0.047 seconds;
  watcher polls with a 20-second delay fell from about 57 to 24 seconds.
  Power requests are now sampled every poll once the process count reaches six.
- `755d2b7`: instruction emulation clears STI/MOV-SS blocking when the
  instruction retires, preserving NMI/SMI blocking and leaving retries alone.
  Fourteen assertions failed before this fix across three harnesses. The new
  `vmcs_interrupt_shadows_cleared` counter records actual clearings.
- `c054545`: the VMCS field cache invalidates a cached field when a write bypasses
  the cache during a borrow. A new harness reproduces stale RIP/RSP/CR3
  reads after another CPU's borrow ends without an epoch change. Three
  assertions fail before the fix and pass after it. Its counter is
  `vmcs_cache_bypass_invalidations`.
- `5ac7cac`: private shadow clears invalidate only their owning CPU's rows for
  that region, preserving unrelated cached fields. Hardware VMCLEAR and
  pointer restoration remain. The helper is limited to the two shadow-copy
  sites; generic clears still invalidate globally. This depends on the
  preceding borrow-write fix. Both are deployed. Their original 63 cache
  assertions pass; `vmcs_cache_owned_clears` measures use on the rig.
- The resident reader preserves the module base across its VMCALL and
  VTL1 resume-ring reports. They previously replaced that variable with a
  row address, causing later globals and the log to be read elsewhere.
  The manifest reader now requires the full terminated string; its old
  256-byte prefix omitted `vcache` and other later switches. Six regression
  assertions failed before these reader fixes and pass after them.
- The VMCS cache now withholds fills for access-rights writes whose reserved
  bits the processor may discard (SDM 27.4.1; KVM `handle_vmwrite`). The
  next read gets the processor's actual value, preserving both permitted
  processor behaviors. Three regression comparisons failed before the fix;
  all 107 cache checks now pass. The new counter is
  `vmcs_cache_reserved_bit_writes`, also available in delta reports.
  This change is deployed; its counter remains zero through the latest
  nine-minute read, so its cache path does not explain this boot's progress.

All 27 rebuilt host tests passed after these code changes. All 224 Python
tests, including fragmented replies and incomplete list cases, passed.
The debug hypervisor and loaders build, and the ELF and bootability checks pass.
These checks do not prove that Windows boots.

## Current boot and next step

The **direct-port USB experiment crashed with 0x9F/3**. It began at
21:12:41 UTC September 11 using unchanged deployed **6adda123**, loader
MD5 **d564ca8f8057eabdb36a09db2c1e34d5**, two CPUs and the full manifest.
Only xHCI `p2=8` changed: all five USB devices occupy direct root ports.
Avoiding the external hub did not eliminate the power failure. The user
now confirms **“driver power state failure”** on the physical screen.
No sign-in screen or desktop has been verified.

QEMU remains **paused (shutdown)**; preserve this guest until its remaining
evidence is archived. Kernel **fffff801e1c00000**, system CR3 **1ae000**,
module **66d47000**, singleton **682f3000** are validated for this run.
Final complete walks contain 34 processes, including LogonUI PID 1440 and
dwm PID 1452, and 177 System threads. All raw System stacks are saved.
The crash time was not captured: the startup watcher exhausted 360 polls
at 23:38:35, and the later GDB guard expired at 00:34:34 September 12.
All those clients exited. The final state was read around 06:57 UTC.
This crash is a postmortem finding, not a direct KeBugCheckEx breakpoint hit.

The failed IRP **ffff9784b5489010** names PDO **ffff9784badac060**, service
HidUsb, instance `USB\VID_0627&PID_0001\28754-0000:00:05.0-2`: the tablet
at root port 2. One power worker, **ffff9784b54ef040**, owns this IRP;
the other is idle. A WDF power worker waits on the tablet's FxPkgPnp lock
**ffff9784baaa6a38**. Its recorded owner is that same busy power worker.
The WDF timer object's stop owner agrees too.

The owner's Running saved KSP was excluded. Instead, the flushed VTL0
software VMCS12 region **114fa0000** gives RIP **nt+5bf139** and RSP
**ffff858055da8b60**, with CR3 1ae002 and CPU 1's Windows GS. The checked
unwind follows the NMI freeze path onto the busy thread's actual stack:
HvlEndSystemInterrupt -> KiDpcInterrupt -> KiCheckForThreadDispatch ->
KeSetSystemGroupAffinityThread -> KeGenericProcessorCallback ->
KeFlushQueuedDpcs -> **Wdf!imp_WdfTimerStop+19f** ->
**UsbHub3!HUBPDO_EvtDeviceD0Exit+34b** -> WDF power dispatch ->
HidUsb/HIDCLASS -> PopIrpWorker -> system-thread startup -> null.
Recovered package/context/IRP pointers match the independently saved
objects. This proves the crash-time dependency, not continuous residence
in that call for the timeout duration or a specific VMM defect.

Next: preserve/archive this evidence and probe the USB timer-stop call
and its DPC-flush return with hardware breakpoints on a fresh boot. Keep
observation alive until a real terminal state or an explicit handoff;
individual debugger stops and monitor reads remain bounded. Exact files,
matching symbol identities and limitations are in
[the USB power note](docs/2026-09-12-gdb-usb-hub-power.md).

Earlier in this boot, an actual paired GDB stop captured one SMSS
KeFlushQueuedDpcs call returning to MmPageEntireDriver in 14.9 ms of
host time between continues/stops. That successful call does not exonerate
later timer-stop calls. Its unchanged InterruptTime is not zero execution.

The preceding same-binary crash is recorded below.

The unchanged **6adda123** startup run, begun around **19:21 UTC** on
September 11, crashed at **20:55:05 UTC**. Direct GDB caught KeBugCheckEx
on CPU 0: **0x9F, parameter 1 = 3**, PDO `ffffe6044844a870`, triage
`fffff8002952c600`, IRP `ffffe60448a40560`. The capture took 14.7 ms.
The bugcheck stack reaches PopIrpWatchdog and the idle timer/DPC path.
The run reached fifteen processes, without LogonUI/dwm or a verified login.
No SCM failure breakpoint fired before the bugcheck; no RpcEptMapper/LSM
ordering was established. Both the GDB coordinator and monitor watcher
have exited. That stopped guest has now been archived and torn down.

The PDO's validated device node names
`USB\VID_0409&PID_55AA\MSFT20314159-0000:00:05.0-4`, service USBHUB3.
QEMU's actual `info usb` places its automatically added USB hub at port 4,
with two keyboards behind it. The launcher contains Bluetooth passthrough,
a tablet and **three** keyboards. The earlier shorthand of one keyboard
was incorrect. QEMU inserts a hub as its default four root ports fill.

The IRP is at stack location 11 of 14, IRP_MJ_POWER/IRP_MN_SET_POWER,
with completion UsbHub3+1c910 and context `ffffe6044aa480d0`. Both power
workers are idle in the final stopped capture. The full System walk has
173 threads: 172 Ready/Waiting stacks saved, one Running stack excluded.
With current WDF/UsbHub3 images, 92 unwind to null and 80 stop at missing
module metadata; no saved stack was accepted for the Running thread.
The complete final resident report, serial, NVRAM, objects, driver images,
exact deployed ELF and an 800-file SHA-256 manifest are preserved under
`/tmp/zpp-20260911/rpc-gdb*`. CPUID census cross-check failures remain
explicit and those census counts are not used.

The now-completed single-variable experiment changed the launcher's controller from
`qemu-xhci,id=xhci` to `qemu-xhci,id=xhci,p2=8`, retaining all five devices
and input objects, two CPUs, full manifest and the same loader. An isolated
128-MiB stopped TCG preflight with the rig's QEMU 11.0.3 confirmed five
direct ports and no hub. It used generic devices and no Windows disk,
VFIO or host input; it proves the configuration, not Windows success.
Evidence preservation, supported teardown, launcher backup/readback and
fresh-mount loader verification completed before this next boot. See
[the hub crash evidence](docs/2026-09-12-gdb-usb-hub-power.md).

The prior startup probes remain documented in
[the SMSS note](docs/2026-09-11-smss-startup-gdb.md): actual autochk creation,
SMSS main subsystem wait, separate asynchronous PnP wait, and rejected
Phase1/CSRSS identity checks. No live CSRSS frame or matched return was
obtained. New boots require fresh kernel/process/module coordinates.

The completed preceding run is recorded below.

The per-CPU borrow build **`6adda123` crashed with 0x9F** at
**19:03:32 UTC**, September 11, after starting at 17:27 UTC. Loader MD5
**`d564ca8f8057eabdb36a09db2c1e34d5`** and the full unchanged manifest
matched the fresh mount. The two-CPU guest reached 73 processes but no
LogonUI/dwm or verified login. QEMU subsequently paused (shutdown).

The failed boot's module base was `0x66d47000`, singleton offset
`0x15ac000`, physical `0x682f3000`. Windows base was
`0xfffff801d4a00000`, system CR3 `0x1ae000`, timestamp `0x51a135d9`,
size `0x1450000`. These are evidence coordinates, not reusable next-boot
addresses. Resolve every resident field from the matching deployed ELF.

The new gate keeps private shadow-copy suspension on its owning processor;
an unidentifiable owner retains a global fallback. Generic clears/migration
retain global epoch invalidation. The aggregate gauge remains diagnostic.
All 222 cache assertions, 27 rebuilt host tests and 228 Python tests pass;
debug loaders, invariants and bootability checks pass. No isolated live
performance benefit or successful boot is established.

**Direct Windows hardware breakpoints work through QEMU's GDB stub.**
They proved an interrupted MiUnlockPageInline call returned and captured
real Windows kernel/user stacks through a physical-memory CR3 walker.
Use targeted scripted stops, remove a breakpoint before continuing, and
detach automatically. Do not use single-stepping or inferior calls.
See [the initial GDB evidence](docs/2026-09-11-gdb-windows-returns.md).

The final [Winlogon/SCM and crash evidence](docs/2026-09-11-winlogon-scm-debug.md)
establishes:

- Winlogon's main thread waits on the named Global\TermSrvReadyEvent.
  Matched kernel/user PE unwinds reach _WinStationWaitForConnectEx;
  the event remained unsignaled at the bugcheck.
- GDB caught services.exe terminating svchost PID 2388 through startup
  cleanup with recovered error 1053. The specific service is not identified.
- A complete 748-entry SCM database/dependency walk finds LSM Stopped
  with error 1068. RpcEptMapper is Running but retains internal startup
  error 1070. Its internal state is already completed (3), so the earlier
  failure/late-completion sequence is a hypothesis requiring a new capture.
- The actual bugcheck is 0x9F, parameter 1 = 3, USB PDO
  ffff920946256060, IRP ffff920946165010. Its watchdog age already exceeded
  300 seconds in a complete monitor reply before the 35-ms GDB capture.
- Two final power workers are Ready inside affinity changes reached from
  KeFlushQueuedDpcs through WDF/UsbHub3. Both complete saved-stack unwinds
  reach HidUsb/HIDCLASS and PopIrpWorker. A third worker is idle; complete
  worker-pool saturation and continuous 300-second residence are not proved.

Some drivers unloaded/reloaded within this boot. The final 199-entry module
list places HidUsb at fffff80170fc0000, UsbHub3 at fffff8016a120000,
HIDCLASS at fffff80170800000 and WDF at fffff80167030000. Do not reuse the
early HidUsb/IntcOED bases for the final stacks. Matched PDB validation
requires GUID equality, Info age >= image age, and nonzero DBI age equal
to image age; Info-age inequality alone incorrectly rejected usable WDF
symbols earlier. The detailed note cites Microsoft's implementation.

The preceding boot's watcher and GDB sessions exited. Its actual bugcheck artifacts
are `cache-local-borrow-gdb-scm-child/index.json` and `stack.bin`, under
`/tmp/zpp-20260911/`; the later redundant generic guard produced no index.
Final driver images, power stacks, SCM records and RPC component images
are preserved. Supported teardown completed: NVMe returned and 15,445 MB was free,
without a host reboot. The final resident report, ELF and freshly mounted
loader are archived; loader MD5 still matches. The new same-binary GDB run described above targets RpcEptMapper startup
and SCM's 1070/1068 state transitions. No new hypervisor fix is yet
justified, and no Windows service/registry/boot configuration was changed.

The preceding cache-slot boot ran from 16:55 to 17:25 UTC, stayed at three
processes, and was stopped using the supported teardown. NVMe returned and
15,489 MB was free without a host restart. Repeated validated unwinds reached
an interrupted unlock return while validating **Npfs.SYS**; final fresh VTL
calls and user-mode samples did not advance. No reset, bugcheck or armed
power IRP was observed. Its archived ELF and evidence are described in
[the completed slot-boot note](docs/2026-09-11-slot-image-return.md).

## Previous elision boot: progress followed by 0x9F

The elision-only boot (`9f1caf2b`, loader MD5
`98542ddf33cdd78402529b4b6b72423c`) ran from about 16:15 until
**16:47:24 UTC**. It reached smss/autochk around fifteen minutes, csrss
around twenty-five minutes and eventually fourteen processes, including
services, winlogon and lsass. It did not produce LogonUI/dwm or verified
login evidence. Fresh VTL calls and user-mode samples continued while two
power requests aged.

The preserved bugcheck is **0x9F, parameter 1 = 3**. Its PDO
`ffffbf8157c538f0` and IRP `ffffbf815aa84be0` match the previously observed
audio request: IntcAudioBus's PDO, with IntcOED as the current device.
A separate USB request was also armed. Two worker-list reads showed one
idle worker and one associated with the USB request; the pool was not
saturated. Three saved Ready-thread stacks consistently reach
KeFlushQueuedDpcs through affinity setup and interrupt dispatch, called
from Wdf01000. They cover only 0.3 seconds and do not establish that this
USB worker caused the separate audio timeout.

See [the detailed evidence](docs/2026-09-11-elision-power-watchdog.md) for
request identities, the checked saved-context reconstruction and its limits.
The old ELF/loader and post-failure evidence were archived before supported
teardown. NVMe and 15,488 MiB free RAM returned without a host reboot.

## Earlier boots

The reserved-access-rights boot (`fe1955ec`, loader MD5
`476ff7711e5232f748513e40cb83c22b`) ran from about 15:54 to 16:13 UTC.
It returned from VBoxSup's first timer request but remained in its release
call, interrupted immediately after restoring CR8 during thread-affinity
setup. Its stored grant stayed 500,000; the reserved-bit counter stayed
zero. The final complete process walk still had three entries. Its late
32.121-second sample had fresh VTL calls +0/+32, handler shares
70.15%/10.59%, and no VMREAD/VMWRITE failures. Supported teardown returned
NVMe and 15,479 MiB free RAM. The ELF and freshly read loader are archived
as `access-rights-deployed.elf` and `access-rights-loader.efi`.
See [the timer-return evidence](docs/2026-09-11-live-timer-return.md).

The prior cache boot started around **15:19 UTC** with both cache changes:
loader MD5 `2ac8432cd5f20272a6eeef00564a2844`, verified from a fresh mount.
It has two CPUs, unchanged build switches and all startup channels answering.
The corrected five-minute report proves the module base from its complete
manifest and shows Hyper-V in VMX operation with L2 entries on both CPUs.

At about six minutes there are still three processes, but this boot is making
fresh VTL calls: CPU 0 +5 and CPU 1 +1,360 in 32.071 seconds. CPU 0 changed
current threads during a separate validated timer sample, and DPC counts
advance. No kernel timer-resolution request is active. Do not cycle a
progressing guest just because the process count has not increased yet.

The same window has 11,076 private clears/s and zero global epoch bumps;
965 cached fields/s were invalidated by writes during a borrow. VMREAD and
VMWRITE failure counters did not move. Handler shares are 68.78%/43.94%,
L1 shares 23.66%/52.18%, L2 shares 7.56%/3.89%. The CPUs are doing different
work from the previous stalled boot, so these are not an isolated speedup
measurement or proof of a successful Windows boot.

That prior boot's addresses: module `0x66e08000`, singleton `0x682f3000`, Windows
base `0xfffff8017fc00000`, CR3 `0x1ae000`. The private-clear counter is at
ELF offset `0x50f7028`, physical `0x6beff028`; borrow-write invalidations
are at offset `0x14eaec0`, physical `0x682f2ec0`. Use
`.rig-deployed-hypervisor.elf`. The observer is the `cache-logon` tmux window.
Artifacts include `cache-state-5min.out`, `cache-timer-6min.out`,
`cache-delta-6min.out` and `cache-watcher-after-6min.txt`.

By thirteen minutes this boot has reached VBoxSup's first timer-resolution
request. Nine valid live unwinds recover the same unreturned call documented
in [the timer-return evidence](docs/2026-09-11-live-timer-return.md), now with
device extension `0xffffa30f8beaf1a0` and grant still zero. The ten-minute
32.106-second window has no fresh CPU 0 VTL calls, 32 on CPU 1, and a 70.08%
CPU 0 handler share. Global epoch bumps remain zero. The cache changes were
exercised but did not remove this stall. The boot stopped around 15:53 UTC
after about 34 minutes, following a final complete three-process walk and
32.148-second counter window: fresh VTL calls +0/+33, handler shares
70.07%/10.75%, and grant still zero. Supported teardown returned NVMe and
15,491 MiB free RAM. The ELF and freshly read loader are archived as
`cache-deployed.elf` and `cache-loader.efi` in the session directory.

The unchanged baseline loader had MD5 `cb729d76cb6adb055ccbe4776cea0a38`.
It still had only System, Secure System and Registry at 43 minutes.
Counter samples showed roughly two hypercalls per second and no new user-mode
samples. Its power-IRP list was empty. Its 156 emulated watched stores had
zero instruction-length disagreements, so the fix above is **not a demonstrated
cause of this baseline stall**. The guest was stopped with the supported script;
NVMe returned and the host recovered its memory.

The watched-store fix was then deployed and verified from a fresh mount:
MD5 `12ad606548c29d04763ec5f07c53863b`. A new two-vCPU boot started around
13:35 UTC on September 11. Channels passed their startup checks, and the
resident log showed Hyper-V entering its nested guest. This boot was stopped
with the supported script around 14:21 UTC after a final complete three-process
walk and a late activity sample. NVMe returned and the host recovered its RAM.

At 24 minutes it still had three processes. A late window measured 2.01
hypercalls/s with no new CPU 0 VTL calls or user-mode samples. Repeated
live-register captures and PE unwinds recovered VBoxSup's first call to
`ExSetTimerResolution`; the driver has not stored the result, while Windows
has committed the requested interval. See
[the live timer-return evidence](docs/2026-09-11-live-timer-return.md).
This narrows the earlier driver-loop account; the reason the timer worker
does not return remains unresolved.

At about 45 minutes the request's grant field was still zero, and CPU 0 had
no new VTL calls. The last 31.910-second window measured 70.51% of CPU 0 time
inside zpp's handler. The old cumulative 41% figure is not a current estimate.

The prior interrupt-shadow fix was deployed with loader MD5
`afecb8bfd26426c201d6d4dd7fa857cf`, verified from a fresh mount. It started
around **14:22 UTC**, with two CPUs and unchanged build switches. All 26
rebuilt host checks and the debug build passed before deployment; the EFI and
ELF checks also pass. Startup channels answered and Hyper-V entered L2.
Recheck live state before drawing conclusions: early process-list reads were
incomplete, and the shadow-clear counter was zero at about one minute.

At 21 minutes this boot still has three processes and 76 loaded modules.
The shadow-clear counter remains zero through 19 minutes. Live unwinds
show two storage paths interrupted immediately after lowering IRQL:
CPU 0 at `KzLowerIrql+0x22`, CPU 1 at `KiSwapThread+0x795` with a successful
wait result ready to return. VBoxSup has not loaded and no kernel timer-
resolution request is active. See
[the storage-return evidence](docs/2026-09-11-live-storage-returns.md).
Both CPUs made zero fresh VTL calls in a 32.197-second window around
14 minutes. DPC counts continue to advance. This is a distinct stall
location, without demonstrated benefit from the shadow fix.

That prior boot's addresses: module `0x66e08000`, singleton `0x682f3000`, Windows
base `0xfffff801aa000000`, CR3 `0x1ae002`. The new counter's ELF offset is
`0x50f7020` (physical `0x6beff020`). Its ELF is archived as
`/tmp/zpp-20260911/interrupt-shadow-deployed.elf`.

Session artifacts are under `/tmp/zpp-20260911/`, including the baseline
loader, baseline ELF, serial/log captures, counter samples and test output.
The tmux session is `zpp-rig-20260911`; the earlier `shadow-logon` observer is stopped.
`/tmp/logon-watch.txt` holds the latest observation. Temporary files and tmux
sessions are evidence locations, not durable completion claims.

For the next boot, a short early stall is not a terminal result: several previous boots changed substantially between ten and eighteen
minutes. Before cycling a guest, inspect both its current process list and a
late counter window. Preserve a progressing guest and measure outstanding
power requests. Prior runs have died to a power watchdog about 300 seconds
after arming; a zero count predicts neither a visible login nor survival.

Update before the cache deployment: the interrupt-shadow boot still has
three processes after about 54 minutes. Its final 32.118-second sample has
zero fresh VTL calls, handler shares 69.81%/74.18%, and 13,239.7 global
epoch bumps/s. The process watcher is stopped and the final process walk
completed. It was stopped with the supported script; NVMe returned and
the host had 15,476 MiB free. Final evidence is `shadow-processes-final.txt`, `shadow-delta-final.out`
and `interrupt-shadow-final-watch.txt` under the session artifact directory.

The screen belongs to the passed-through GPU, so QEMU cannot capture it.
If LogonUI and dwm appear, obtain a contemporaneous screen observation and
continue checking that the guest survives. Never mark the goal complete from
those process names alone.
