# Two storage returns interrupted after lowering IRQL

The two-vCPU boot started around 14:22 UTC on September 11 still has
three processes at 21 minutes. Loader MD5
`afecb8bfd26426c201d6d4dd7fa857cf` carries the instruction-shadow fix.
Its clearing counter remains **zero** through 19 minutes. The different
stall location below therefore does not demonstrate an effect of that fix.

## Evidence from this boot

The kernel's live PE header matches timestamp `0x51a135d9` and image size
`0x1450000`, at base `0xfffff801aa000000`, CR3 `0x1ae002`. Repeated complete
module-list walks, checking every backlink and the kernel anchor, contain
76 modules. VBoxSup is absent. `ExpKernelResolutionCount` is zero,
`ExpLastRequestedTime` is `0xffffffff`, and `KePseudoHrTimeIncrement` is
156,250. This boot has not reached the
[earlier VBoxSup request](2026-09-11-live-timer-return.md).

Live-register stack captures at about 11 and 15 minutes use the same
exploratory PE unwinder as that earlier investigation, extended to accept
multiple captured modules. Reads take 4–24 ms and are not atomic. Torn
captures stop with invalid addresses and are excluded from the conclusions.
The guest runs continuously; no debugger, pause or single-step is used.
The watcher yields sole ownership of the monitor to each reader.

CPU 0 repeatedly recovers this chain, with the same outer stack addresses
in both sets of captures:

```text
[interrupt / DPC paths]
KzLowerIrql+0x22                         RSP ffffbc06a12dfcf0
storport!RaidStartIoPacket+0xf27          RSP ffffbc06a12dfd20
storport!RaidUnitSubmitRequest+0x10b
storport!RaDriverScsiIrp+0x39f
IopfCallDriver / IofCallDriver
EhStorClass!FilterDeviceEvtWdmIoctlIrpPreprocess+0x12f
Wdf01000!FxDevice::DispatchWithLock+0x1da
IopfCallDriver / IofCallDriver
disk!DiskPerformSmartCommand+0x1f6
disk!DiskGetIdentifyInfo+0x75
disk!DiskDetectFailurePrediction+0x6c
disk!DiskInitializeFailurePrediction+0x5e
IopProcessWorkItem+0x30e
ExpWorkerThread+0x1b2
PspSystemThreadStartup+0x5a
KxStartSystemThread+0x34
```

The precise interrupted instruction is the epilogue immediately after
`mov cr8, rbx` at `nt+0x3f1f0e`. Restored RBX is zero: IRQL has already
been lowered to PASSIVE. Only stack restoration and RET remain in this
function. The storport caller also has only its return sequence after
the call to `KzLowerIrql`. This is not a storage polling loop.

CPU 1 repeatedly recovers a separate storage path:

```text
[interrupt / DPC paths]
KiSwapThread+0x795                       RSP ffffbc06a1006960
KiCommitThreadWait+0x39d
KeWaitForSingleObject+0x859
CLASSPNP!ClasspModeSense+0x117
CLASSPNP!ClassModeSense+0x13
CLASSPNP!ClasspWriteCacheProperty+0x310
CLASSPNP!ClassDeviceControl+0xaf1
disk!DiskDeviceControl+0xf4
```

Here `nt+0x2cc391` has executed `mov cr8, r14`; R14 is zero. The next
instruction, at the sampled `nt+0x2cc395`, moves RBX to RAX before the
epilogue. RBX is zero, the successful wait result. One capture samples
that exact RIP directly, independently of unwinding an interrupt frame.
The thread has resumed from its wait and is interrupted before returning
the result. Do not interpret the `KeWaitForSingleObject` frame as proof
that this processor is still waiting for the device.

The current threads are `0xffffd003b2dea040` and `0xffffd003b24de080`,
both state Running, with both PRCB processor numbers validated. Between
the reads around 14 and 19 minutes, DPC counts advance from 1,300 to 1,774
and 639 to 796. Empty queue snapshots do not mean DPCs never run.

## Activity window and limits

A 32.197-second window around 14 minutes gives:

| Measurement | CPU 0 | CPU 1 |
|---|---:|---:|
| Exits/s | 6,555.61 | 6,742.81 |
| New VTL calls | 0 | 0 |
| Time in zpp handler | 69.88% | 74.18% |
| Time in Hyper-V | 23.40% | 19.06% |
| Time in Windows | 6.72% | 6.76% |

Both this boot and the earlier timer-request boot repeatedly interrupt
thread code at the first instruction after lowering IRQL. Their outer
operations differ. A VBoxSup-specific wait is therefore insufficient to
explain the stalls. The shared return boundary supports investigating
interrupt service cost and virtualization state; it does not distinguish
those causes or prove every interrupt is correctly delivered.

A separate 30.03-second sample around 21 minutes finds the VINA suppression
counts unchanged, with `vtl_half_mark_kind` equal to 2 on both CPUs. Its
VTL1 suppression gate is therefore inactive during this window. The earlier
CPU 0 address-space refusals total 10,087; that historical total alone
does not establish a wrongly dropped vector in this stall. The staging
counts grow by 1/1 for vector `0x50` on CPUs 0/1 and by 2 for CPU 0's
`0x51`. Device interrupt staging has not stopped entirely.
`l2_entry_vector` is compiled out with `census=0`; its zero rows are not
evidence of missing delivery. This sample is in `shadow-vector-samples.json`.

Driver names come from Microsoft's symbol server, using the RSDS GUID
and age in each resident image. Captured images have no missing pages.
Their identities, bases and SHA-256 hashes are in
`/tmp/zpp-20260911/shadow-module-identities.json`. Other evidence there:
`shadow-live-stack/`, `shadow-both-stacks/`, `shadow-both-unwind-complete.out`,
`shadow-return-registers.out`, `shadow-delta-14min.out`, and
`shadow-timer-14min.out` / `shadow-timer-19min.out`.

The word `complete` in the unwind artifact's filename is not a claim
that every capture unwound completely: CPU 0 reaches its thread start;
CPU 1 stops below the listed chain at a module not loaded into the reader.
No conclusion here uses that missing tail. The delta reader's arm-to-fire
comparison also mixes timer modes and is not used above.
