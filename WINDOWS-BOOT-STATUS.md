# Nested Windows boot investigation

Updated 2026-09-11. The goal remains a verified Windows login or desktop
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

The current boot started around **16:15 UTC**, with the archived write-elision
loader from `9f1caf2b`, MD5 **`98542ddf33cdd78402529b4b6b72423c`**, verified
from a fresh mount. Two CPUs and build switches are unchanged. All startup
channels answered. Module base is `0x66e08000`, singleton `0x682f3000`.
Windows base **`0xfffff80477e00000`** was derived from the LSTAR value and
validated against its PE timestamp and image size; CR3 is `0x1ae000`.

Use **`.rig-deployed-hypervisor.elf`**, which differs from the newer local
build. `vmcs_cache_write_hits` is at ELF offset `0x14eaeb0`, physical
`0x682f2eb0`; reserved-bit writes are at offset `0x14eaed0`, physical
`0x682f2ed0`. The `elision-logon` watcher in `zpp-rig-20260911` owns the
monitor. Artifacts use the `cache-write-elision-` prefix under the session
directory. The deployed candidate also exists in `elision-ready/`.

The early cumulative report records 49,300 skipped VMWRITEs and zero
VMREAD/VMWRITE failures. CPU 0 has Hyper-V VMX operation and L2 entries;
CPU 1 is still at its early startup state. The two-minute kernel identity
check passed, but the process walk was incomplete and the timer reader
encountered an absent PRCB pointer. These are failed early reads, not an
empty process list or a completed boot. Let startup proceed, then take a
complete process walk and a delta sample before estimating benefit.

**Next build, not deployed:** `48bcfc9c` gives field width/type separate
cache slots. The old mapping squeezed 156 fields into 26 slots; the new
projection gives all 156 distinct slots. All 191 cache assertions and all
27 rebuilt host tests pass (224 Python tests); debug loaders and ELF/
bootability checks pass. It adds 786,432 bytes of BSS and increases the
number of tags cleared on invalidation. Its live net effect is unknown.
Keep it separate from the elision-only experiment now running.

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

Continue observing the current boot. A short early stall is not a terminal
result: several previous boots changed substantially between ten and eighteen
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
