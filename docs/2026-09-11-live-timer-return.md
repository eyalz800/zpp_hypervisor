# A live unwind through VBoxSup's first timer-resolution request

The two-vCPU boot started around 13:35 UTC on September 11 still had
three processes at 24 minutes. Loader MD5
`12ad606548c29d04763ec5f07c53863b` carries the watched-store length fix.
A 31.915-second window after 18 minutes measured CPU 0 at 8,461.26 exits/s,
CPU 1 at 1,192.49/s, and vmcall at 2.01/s. CPU 0's fresh VTL calls remained
27,667 and user-mode samples remained 177. This resembles the unchanged
loader's stall; the correctness fix has not demonstrated a boot improvement.

## Capture and validation

The watcher was stopped while each other reader owned the monitor, then
restarted. The guest was never paused, single-stepped or attached to gdb.
`info registers -a` supplied actual RIP, RSP and general registers. Samples
inside the Windows kernel were followed by physical reads from live RSP
upward through Windows' page tables. Captures took 5–13 ms and are **not
atomic**. Failed unwind attempts are not evidence about a call chain.

Kernel base `0xfffff8027a600000`, CR3 `0x1ae002`: its live PE header matched
the local image's timestamp `0x51a135d9` and SizeOfImage `0x1450000`.
A complete 105-module walk checked every backlink and the kernel anchor.
It located **VBoxSup 7.0.10.158379** at `0xfffff802103f0000`, size `0x12b000`.
The version comes from the driver's resident version resource.

The driver was captured as memory, so RVA equals file offset. Eighteen
unmapped pages at RVAs `0x115000` through `0x126000` were recorded as
missing and excluded from code/metadata reads. Its exception directory is
RVA `0x109000`, size `0xb034`.

An exploratory offline reader applies the PE exception-directory entries
and unwind codes, including saved nonvolatile registers and interrupt
machine frames. It recognizes simple epilogues and stops on missing data
or unsupported shapes. The format was checked against Microsoft's
[x64 exception-handling specification](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64).
This is not a general Windows unwinder.

## The recovered chain

One initial capture and six repeat captures about five minutes later
recovered the same outer chain. The innermost interrupt routines vary:

```text
KiDpcInterruptBypass / KiInterruptDispatchNoLockNoEtw
ExpUpdateTimerConfigurationWorker+0x1c5  RSP ffff930fbb607220
KeGenericProcessorCallback+0x175
ExpUpdateTimerConfiguration+0xc6
ExpUpdateTimerResolution+0x1cd
ExSetTimerResolution+0xbc
VBoxSup+0x25d66                         RSP ffff930fbb607620
VBoxSup+0xc051                          RSP ffff930fbb607650
VBoxSup+0xedc8
VBoxSup+0x9a63
VBoxSup+0x194c9
PnpCallDriverEntry+0x54
IopLoadDriver+0x6f2
IopInitializeSystemDrivers+0x1a6
IoInitSystem+0x2c
Phase1Initialization+0x3b
PspSystemThreadStartup+0x5a
KxStartSystemThread+0x34
```

This recovers the driver frames that earlier scans did not establish.
`PnpEnableWatchdog` is absent. Its appearance in older scans does not make
it a caller of `ExSetTimerResolution`.

## A second check: the driver's result has not been stored

The captured driver bytes independently explain the return addresses:

- RVA `0xc047` loads ECX with `0xee6b3` (976,563 ns). The call at `0xc04c`
  targets `0x25d30`, returning at `0xc051`.
- The function at `0x25d30` divides the request by 100, sets its Boolean
  argument true, and calls the pointer loaded from RVA `0x108188`.
  That live pointer is `nt+0x416570`, exactly `ExSetTimerResolution`.
  The return address is `0x25d66`.
- After returning, the function stores the granted interval through RBX.
  Its caller stores that result at device-extension offset `0xa8`, at
  instruction RVA `0xc092`.

Oracle's corresponding routines are
[`RTTimerRequestSystemGranularity`](https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Runtime/r0drv/nt/timer-r0drv-nt.cpp)
and
[`supdrvGipRequestHigherTimerFrequencyFromSystem`](https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/HostDrivers/Support/SUPDrvGip.cpp).
That source is newer than the resident driver. It corroborates the names;
the arguments, offsets, targets and stores above come from the driver bytes.

The unwound RBX identifies the extension as `0xffff808622e861a0`.
Three reads about one second apart gave:

| Value | Readings |
|---|---|
| Granularity grant at extension + `0xa8` | 0, 0, 0 |
| `ExpKernelResolutionCount` | 1, 1, 1 |
| `ExpLastRequestedTime` | 9,765 throughout |
| `KePseudoHrTimeIncrement` | 9,765 throughout |
| `KiClockTimerOwner` | 0 throughout |
| `HalpClockTickLogIndex` | 1,933,239 → 1,934,900 → 1,936,543 |
| CPU 0 DPC count | 4,447 → 4,449 → 4,452 |

CPU 0's current thread was `0xffff8086204de080`, state Running. CPU 1's
current thread equalled its idle thread. Both PRCB processor numbers were
validated. The queues were empty at those instants, but DPC counts advanced;
another capture was inside the progress-indicator timer DPC. An empty queue
snapshot therefore does not establish that DPC processing never runs.

## What this changes

For **this boot**, the evidence supports one unreturned timer-resolution
request, with interrupts running over the worker's epilogue. It does not
support a driver loop repeatedly requesting the resolution or polling time
after the request returned. Windows committed the interval; the driver has
not received and stored the result.

Why execution does not return through the worker remains unresolved.
These reads do not distinguish interrupt-service cost from incorrect
virtualization state or event handling. CPU 1's idleness is not a missing
rendezvous: the callback targets the one clock owner. Do not remove the
driver or treat its name as an explanation of the hypervisor's behavior.

Artifacts are under `/tmp/zpp-20260911/`: `fixed-live-stack/`,
`fixed-live-stack-repeat/`, `fixed-driver/`, `fixed-unwind-driver.out`,
`fixed-unwind-repeat.out`, `fixed-driver-return.out`, and
`fixed-delta-18min.out`. `live-unwind-sha256.txt` identifies the captures.
Partial driver SHA-256:
`3dd710c4483e5013c37333cbbf17ecd954d493bed9dd3aa31ddf69b46e6245b3`.

The delta reader rejected its by-level split because one independently read
subset exceeded its superset by one (45,430 versus 45,429). The reads are
not atomic; that alone does not prove a wrong stride or mixed binaries,
despite the diagnostic's wording. That split was not used above. Its
printed manifest is also truncated; compare the complete deploy manifest.

## The cache boot reaches the same unreturned request

The boot started around 15:19 UTC with `c054545` and `5ac7cac`, loader MD5
`2ac8432cd5f20272a6eeef00564a2844`, reaches this same chain. Its kernel base
is `0xfffff8017fc00000`, CR3 `0x1ae000`. The kernel timestamp and image size
were checked again. A complete module walk with all backlinks validated
contains 105 images and locates VBoxSup at `0xfffff801149c0000`, size
`0x12b000`. The same 18 trailing pages are absent and excluded from unwinding.

Of 12 live CPU 0 captures around eleven minutes, nine unwind through
`ExpUpdateTimerConfigurationWorker+0x1c5`, `ExSetTimerResolution+0xbc`,
VBoxSup `+0x25d66` and `+0xc051`, then the driver-load/Phase1 chain above.
Three capture that worker RIP directly. The worker's RSP is
`0xfffff58e70a07220`; the two VBoxSup frames have RSP
`0xfffff58e70a07620` and `0xfffff58e70a07650`. Three inconsistent captures
were discarded. Capture durations were 5–13 ms; they are not atomic.

The reconstructed nonvolatile RBX in the caller names device extension
`0xffffa30f8beaf1a0`. The resident bytes again load 976,563 ns at `+0xc047`,
call the granularity routine, and store the result at extension `+0xa8`
only after return. The live import at VBoxSup `+0x108188` is exactly this
boot's `nt+0x416570`. Three validated reads around thirteen minutes find
the grant still zero, resolution count 1, requested/pseudo interval 9,765,
and clock owner 0. CPU 0 runs Phase1 thread `0xffffa30f894a4040`; CPU 1's
current thread equals its idle thread. DPC counts advance and queues are
empty at those instants.

At six minutes no request was active and fresh VTL calls were advancing.
In the 32.106-second window around ten minutes, CPU 0 has zero fresh VTL
calls and CPU 1 has 32. CPU 0 spends 70.08% in zpp's handler, 22.85% in
Hyper-V and 7.07% in Windows. Global epoch bumps remain zero and private
clears run at 9,669.1/s, so the cache change is exercised. It has not removed
this timer-return stall. This still does not isolate service cost from
incorrect virtualization; the guest remains running for later observation.

New artifacts: `cache-live-stack-11min/`, `cache-vbox/`,
`cache-vbox-unwind-11min.out`, `cache-vbox-return-13min.out`,
`cache-delta-10min.out`, and `cache-timer-10min.out` in the session directory.
The resident reader's module-base overwrite and truncated manifest were
fixed separately in `57fd2ef`; the new delta report reads the full manifest.

## The reserved-access-rights boot reaches the release call

The previous cache boot stopped around 15:53 UTC after about 34 minutes.
Its final complete process walk still had three processes. In 32.148 seconds,
CPU 0 made zero fresh VTL calls and CPU 1 made 33; handler shares were
70.07%/10.75%. Three final validated reads still found grant zero, kernel
resolution count 1 and requested/pseudo interval 9,765. Supported teardown
returned the NVMe and 15,491 MiB free RAM. The archived loader's MD5 matches
`2ac8432cd5f20272a6eeef00564a2844`.

The next boot started around **15:54 UTC**, carrying `fe1955ec`'s cache fix
for processor-dependent reserved access-rights bits. Fresh-mount loader MD5
is **`476ff7711e5232f748513e40cb83c22b`**; build switches and CPU count are
unchanged. Module base is `0x66e08000`, singleton `0x682f3000`, Windows base
**`0xfffff800a5800000`**, CR3 `0x1ae000`. The new counter's ELF offset is
`0x14eaec8`, physical `0x682f2ec8`.

Eleven live CPU 0 captures around eight minutes took 5–15 ms. Eight valid
unwinds agree on this chain; three yielded invalid RIP `0x18` and are
discarded. Four valid samples have the interrupted RIP directly:

```
nt+0x2bb96b KiCheckForThreadDispatch+0x7f
  KeSetSystemGroupAffinityThread+0x18e
  KeGenericProcessorCallback+0x14e
  ExpUpdateTimerConfiguration+0xc6
  ExpUpdateTimerResolution+0x1cd
  ExSetTimerResolution+0xbc
  VBoxSup+0x25d24
  VBoxSup+0xf148
  VBoxSup+0x9a63, +0x194c9
  PnpCallDriverEntry / IopLoadDriver / IopInitializeSystemDrivers
  IoInitSystem / Phase1Initialization / PspSystemThreadStartup
  KxStartSystemThread -> 0
```

The sampled `nt+0x2bb96b` follows `mov cr8, rbp` at `+0x2bb967`; its
remaining instructions restore registers and return. The callback is
inside its call to `KeSetSystemGroupAffinityThread` at `+0x30e239`, before
the timer worker call at `+0x30e260`. This is another interruption immediately
after lowering IRQL, now on the way into the release's worker.

A complete 105-module walk, with every backlink and the kernel anchor
validated, identifies VBoxSup base **`0xfffff8003a600000`**, size `0x12b000`.
Its 18 absent tail pages are excluded from all instruction/unwind reads.
The live import `VBoxSup+0x108188` equals this boot's
`nt+0x416570` (`ExSetTimerResolution`). At `VBoxSup+0x25d1a`/`+0x25d1c`,
the code zeros EDX and ECX before calling this import at `+0x25d1e`.
Thus this is the **release** operation. The caller at `+0xf139` loads
extension `+0xa8`, skips release if zero, calls at `+0xf143`, then clears
the field only at the unreturned `+0xf148`.

Reconstructed nonvolatile RBX identifies extension
**`0xffffb48a4ce861a0`**. Three validated reads at 16:02:43–16:02:46 UTC (about nine minutes) find
**grant 500,000**, kernel resolution count **0**, last requested interval
**156,250**, pseudo interval **9,765**, and clock owner **0**. The nonzero
stored grant and release frame prove the preceding request returned. They
do not prove the release completed. CPU 0 runs Phase1 thread
`0xffffb48a494de080`; CPU 1's current thread is its idle thread
`0xffffb48a494f5040`. DPC counts advance, with empty queues at the samples.

The six-minute 31.990-second window has fresh VTL calls +0/+32, L2 entries
on both CPUs and handler shares 70.08%/10.53%. The reserved-bit counter is
zero in the early cumulative report, has delta zero in this window, and is
still **cumulatively zero at 16:03:27 UTC**. Therefore this boot's further
progress cannot be attributed to that cache path being exercised. VMREAD
and VMWRITE failures remain zero in the measured window. A complete process
walk at the same time still has only System, Secure System and Registry.

Artifacts under `/tmp/zpp-20260911/`: `access-rights-live-stack-8min/`,
`access-rights-vbox/`, `access-rights-vbox-unwind-8min.out`,
`access-rights-vbox-release-disassembly.txt`, `access-rights-affinity-disassembly.txt`,
`access-rights-vbox-release-10min.out`, `access-rights-counter-10min.txt`,
`access-rights-delta-6min.out`, and `access-rights-processes-10min.txt`.
The new VMCS write-elision build is verified locally but is **not running
in this guest**. The `ar-logon` watcher owns the monitor again.
