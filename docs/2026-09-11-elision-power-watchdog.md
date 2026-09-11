# Elision boot reached fourteen processes, then an audio power watchdog

The two-CPU write-elision boot ran from about 16:15 to **16:47:24 UTC** on
September 11. It did not reach a verified login or desktop. Its loader was
`9f1caf2b`, MD5 `98542ddf33cdd78402529b4b6b72423c`, verified from a fresh
mount. The cache-slot projection in `48bcfc9c` was not in this boot.
Module/singleton bases were `0x66e08000`/`0x682f3000`; Windows was at
`0xfffff80477e00000`, CR3 `0x1ae000`. The kernel PE timestamp `0x51a135d9`
and image size `0x1450000` were checked against live memory.

## Progress before the failure

At about fifteen minutes the complete process walk contained smss and
autochk, and requested/pseudo timer intervals both read 156,250 with kernel
resolution count zero. Around twenty-one minutes autochk had exited and a
second smss appeared. At twenty-five minutes csrss was present. A valid
32.182-second window then had fresh VTL calls +2,957/+1,761 and user-mode
samples +1,178/+2,681 on CPUs 0/1, with no VMREAD/VMWRITE failures.

Later complete walks reached fourteen processes: System, Secure System,
Registry, smss, two csrss, wininit, winlogon, services, WerFault, LsaIso,
lsass, and two fontdrvhost. Neither LogonUI nor dwm appeared. A 32.155-second
window with armed power requests still had 1,153 fresh CPU 0 VTL calls and
9,743 CPU 0 user-mode samples. This was active user work while requests
aged, not a completely frozen guest. These changing boot phases do not
isolate the performance effect of write elision.

## Two armed requests; the audio request became the bugcheck argument

The watcher first reported an armed request at 16:42:16 UTC. Subsequent
complete power-list reads found two armed SET_POWER requests:

| Field | Audio | USB |
|---|---|---|
| IRP | `ffffbf815aa84be0` | `ffffbf8158943010` |
| PDO | `ffffbf8157c538f0` | `ffffbf8157e91d80` |
| PDO driver | IntcAudioBus | USBHUB3 |
| Current device | `ffffbf815866add0` (IntcOED) | `ffffbf8157e91d80` (USBHUB3) |
| Watchdog start, 100-ns units | 15,980,600,118 | 16,216,508,312 |
| Age near 16:46:49 UTC | 278.9 s | 255.3 s |

The audio IRP's current stack-location pointer at IRP+0xb8 resolved to
`ffffbf815aa84cf8`, with major 0x16, minor 2, control 0xe1, and IntcOED's
device object. The same decoder refused the USB IRP because it found no
valid major-0x16 candidate. No conclusion depends on that failed decoding.

At **16:47:24 UTC**, QEMU stopped with shutdown status. Preserved
`KiBugCheckData` contained:

```text
0x9f DRIVER_POWER_STATE_FAILURE
p1 = 3
p2 = ffffbf8157c538f0
p3 = fffff8040a7e49f0
p4 = ffffbf815aa84be0
```

The PDO and IRP match the earlier audio request. IntcAudioBus is its
enumerator, while the current device was IntcOED. This identifies the
timed-out request; it does not establish which component caused the delay.

## Power worker and saved stack

Two complete worker-list reads passed the semaphore and backlink checks.
They showed two workers, one idle and one associated with the USB IRP.
There was one in-flight request, zero pending worker-queue entries and
`PendingSetPower=2`. The worker pool was not saturated in these samples.
The busy worker was thread `ffffbf81554bc040`, with device
`ffffbf815890b060` belonging to HidUsb.

Three raw stack captures between 16:46:48.98 and 16:46:49.30 UTC read that
thread as **Ready** both before and after each capture. They took 29–40 ms
and were not atomic. The first two saved KernelStack pointers were
`fffffd830dc166d0`; the third was 48 bytes lower. Raw buffers differ; their
agreement is in the recovered outer frames, not byte-for-byte identity.

The matched kernel disassembly establishes the saved-context layout:
SwapContext pushes RBP, subtracts 0x30 and saves RSP at KTHREAD+0x58.
Its epilogue adds 0x30, pops RBP and returns. All three captures have
`nt+0x6b3f56` at saved KernelStack+0x38, the return into KiSwapContext.
Only RIP, RSP and RBP are seeded; unavailable registers are left unknown.
PE unwind metadata then recovers this common outer chain:

```text
KiSwapContext+0x76
KiQuantumEnd+0xc92
[interrupt dispatch, with a machine frame]
KiCheckForThreadDispatch+0x7f
KeSetSystemGroupAffinityThread+0x18e
KeGenericProcessorCallback+0x14e
KeFlushQueuedDpcs+0x18f
Wdf01000.sys+0x4341f
```

The final address belongs to Wdf01000 in this boot's complete 172-module
list. The unwind stops there: Wdf01000's bytes were not captured from this
boot, so an earlier boot's image is not silently substituted. The third
capture has an additional interrupt-dispatch wrapper but converges on the
same outer stack addresses from KiCheckForThreadDispatch onward.

This is a saved, switched-out Ready context during interrupt dispatch.
It is stronger than the earlier raw stack scan, but the captures cover
only about 0.3 seconds. They do not prove that the worker was continuously
stuck for the entire watchdog interval, or that its USB request caused the
separate audio timeout. No guest registers or memory were modified.

## Preserved artifacts and next experiment

Artifacts are under `/tmp/zpp-20260911/`, prefixed `cache-write-elision-`:
`bugcheck.txt`, `power-armed-{a,b,c}.txt`, `workers-armed-{a,b}.txt`,
`audio-irp-armed.txt`, `usb-irp-armed.txt`, `delta-armed.out`,
`processes-final.txt`, `worker-stack/`, `worker-unwind.txt`, and the three
`swapcontext-*.txt` disassemblies. The offline helper is
`unwind-ready-worker.py`. HidUsb and IntcOED images were captured after
the stop; their metadata explicitly records missing pages.

The old deployed ELF and freshly read loader are archived as
`cache-write-elision-deployed.elf` and `cache-write-elision-loader.efi`.
The loader hash was checked before replacement. Supported teardown
returned NVMe and 15,488 MiB free host RAM without a host restart.
The next experiment deploys only the already-tested cache-slot projection,
with CPU count, clocks, nesting and the full manifest unchanged.
