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
- The VMCS field cache now invalidates a cached field when a write bypasses
  the cache during a borrow. A new harness reproduces stale RIP/RSP/CR3
  reads after another CPU's borrow ends without an epoch change. Three
  assertions fail before the fix and pass after it. This fix is built and
  **not yet deployed**; its counter is `vmcs_cache_bypass_invalidations`.

All 27 rebuilt host tests passed after these code changes. All 219 Python
tests, including fragmented replies and incomplete list cases, passed.
The debug hypervisor and loaders build, and the ELF and bootability checks pass.
These checks do not prove that Windows boots.

## Rig evidence and next step

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

The interrupt-shadow fix is now deployed: loader MD5
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

Current addresses: module `0x66e08000`, singleton `0x682f3000`, Windows
base `0xfffff801aa000000`, CR3 `0x1ae002`. The new counter's ELF offset is
`0x50f7020` (physical `0x6beff020`). Use `.rig-deployed-hypervisor.elf`.

Session artifacts are under `/tmp/zpp-20260911/`, including the baseline
loader, baseline ELF, serial/log captures, counter samples and test output.
The tmux session is `zpp-rig-20260911`; the current observer is `shadow-logon`.
`/tmp/logon-watch.txt` holds the latest observation. Temporary files and tmux
sessions are evidence locations, not durable completion claims.

Continue observing the current boot. A short early stall is not a terminal
result: several previous boots changed substantially between ten and eighteen
minutes. Before cycling a guest, inspect both its current process list and a
late counter window. Preserve a progressing guest and measure outstanding
power requests. Prior runs have died to a power watchdog about 300 seconds
after arming; a zero count predicts neither a visible login nor survival.

The screen belongs to the passed-through GPU, so QEMU cannot capture it.
If LogonUI and dwm appear, obtain a contemporaneous screen observation and
continue checking that the guest survives. Never mark the goal complete from
those process names alone.
