# Optimized debug candidate for a controlled boot comparison

Prepared September12; **deployed for the10:50:25 comparison boot**.
The preceding unchanged6adda123 baseline was deliberately ended10:49:07
after verifying the repeated dispatcher probe; it had not crashed again.
No successful sign-in or measured performance improvement is claimed.

The observed USB flush stayed outstanding until a power watchdog, and SCM
startup also timed out. Execution cost is a hypothesis to test, not an
established cause. Earlier release trials in BACKLOG changed CPU count,
eager EPT and VINA switches together and lacked DWARF; their conclusions
were later revised. This candidate preserves the complete current manifest
and debug instrumentation while varying compiler optimization alone.

## Build and artifact validation

Source HEAD8c38d09f has no changes versus6adda123 in hypervisor, shared
loader, UEFI loader, CMake or toolchain sources. The baseline deployed ELF
equaled the normal debug output byte-for-byte. Its29 C++ translation units
have no optimization option (Clang default-O0). Candidate commands retain
all original defines and compile these same29 units with **-g -O2**.
The UEFI loader keeps its normal debug flags and embeds the new ELF.

Exact configuration argv and logs are under `/tmp/zpp-20260912-optimized/`:
`configure-commands.json`, `build-result.json`, `baseline-compile-commands.json`,
`baseline-manifest.txt`, and per-target configure/build logs. The standalone
sub-builds use the existing child configuration arguments, separate build
and output directories, and an additional CMAKE_CXX_FLAGS_DEBUG=-g -O2
for the hypervisor only. No tracked build option was changed.

| Artifact | Size | MD5 | SHA-256 |
| --- | ---: | --- | --- |
| zpp_hypervisor | 4,851,792 | 477ed5ec7fd5742f45d57bf7b3efc87d | eef4556b02933c62f45146d408a2c7c9c6eece48456f1ec9b6e8082b141d5672 |
| zpp_loader.efi | 4,887,040 | 53fc99e459abfc6885b2f9c3aa448ae8 | be47db6f6d57524dc2bd81240ff44c58b894bb76fc0ef6f1360acf2d1c3f281c |

The exact ELF occurs once in the loader, at file offset7000. Both artifacts
contain the same complete manifest as the deployed ELF. ELF checks pass:
no undefined or hosted runtime entry symbols, expected supported init_array,
manifest present, VMX flag checks valid. Bootability checks pass. DWARF is
present; offline GDB resolves the singleton offset**1592000** and the
VMCS12/current-region/running-L2 member offsets185bbe8/185bae8/187bbe8.
Read the module base from the candidate's own serial if it is deployed.

Candidate copies are at `out/optimized-20260912/x86_64/` and
`/tmp/zpp-20260912-optimized/out/`. Deployment now uses these artifacts,
including the matching `.rig-deployed-hypervisor.elf`. The candidate's larger on-disk size includes debug information;
llvm-size text totals are530,819 bytes versus638,082 for the baseline.
Those sizes do not measure execution speed.

## Tests and next comparison

`scripts/ci/run-host-tests.sh zpp-optimized-20260912` rebuilt the native
harnesses with-O2 and retained debug assertions. **All27 CTest entries passed**,
including228 Python tests. The temporary CMakeUserPresets.json was created
only after checking that none existed and was removed after the run. Its
exact contents remain in `host-test-presets.json`; result and full logs are
`host-tests-result.json`, `host-tests.txt` and `host-tests/Testing/Temporary/`.
The suite's own ELF entry checks the normal debug output; the candidate ELF
was independently graded using `check-invariants.sh optimized-20260912`.

The baseline observation ended after repeated dispatcher coverage was
verified on hardware. The new comparison uses this candidate with the same
two-CPU launcher, direct USB topology, full nesting and Windows installation.
Use the supported teardown/deploy/boot procedures, verify disk/loader anchors
and rediscover every guest address. Compare measured progress and actual
entry/return intervals; a different outcome alone does not prove a fix or
exclude instrumentation/timing effects. No Windows power-policy workaround
is part of this candidate.

The unchanged baseline now has a short, defined comparison window:
10:31:23.839–10:32:06.041 UTC,42.2025 host seconds. Two prompt-framed,
non-atomic reads take13.16/13.95ms; first-TSC fingerprints and deployed-ELF
layouts agree. CPU1 closes336,985 handler spans and makes168,493 L2 entries.
RDTSC deltas attributed to handler/L1/L2 are74.740%/18.514%/6.747%; average
handler span186,452 cycles. These are instrumented spans, including time
descheduled underneath the rig, not exclusive instruction execution. CPU0
has only309 closed spans and99.913% in its L1-attributed interval, which can
include halt time. The observed TSC rate is about1.992GHz on both CPUs.
This is a PnP-stage baseline, not a measurement of the earlier USB stall or
an optimized-build result. Raw replies, serial, offline GDB layout and
calculation are in `/tmp/zpp-20260912-repeat-dispatch/execution-cost/`.

## Deployment and first comparison milestones

The baseline ended deliberately10:49:07 with six processes and no active
paired USB call. It had verified the repeated dispatcher instrument through
complete calls and supplied a defined cost window; the later crash was not
reproduced before ending it. Supported teardown removed QEMU20932, returned
NVMe and left15,481 MB RAM free. Fresh pre/post-deployment mounts preserve
Limine, launcher, GPT, ESP BPB and NTFS anchors byte-for-byte. The supported
deployment script verifies the candidate's exact hash and full manifest,
then archives its ELF for all subsequent offsets.

The new two-CPU boot starts10:50:25, all channels answering. Serial gives
module66d61000; candidate singleton offset1592000 gives682f3000. The module
base member independently agrees. NTfffff801dee00000 validates its PE
size1450000/timestamp51a135d9 through System CR31ae000; complete walks agree
with native DTB1ae002. The same five USB devices occupy direct ports1–5.
SMSS640 appears10:53:25 (three minutes); the preceding baseline's first SMSS
appeared21m07s after launch, but one milestone does not control all timing
variation or establish successful boot.

Sole watcher12724, manager12722, initial GDB guard12725. Artifacts and exact
preflight/launch scripts are under `/tmp/zpp-20260912-optimized-run/`.
USB/WDF addresses will be discovered afresh before arming the now-verified
repeated dispatcher probe, and SCM tracing follows services discovery.
No Windows driver, service, registry, BCD or power-policy change.

USB/WDF discovery completes10:56:56 (6m31s), with USBHUB3fffff80177c00000
and WDFfffff80171430000 validated before GDB starts10:56:58. Autochk668 is
present10:55:10–52 and absent10:56:13 (exit status uncaptured); SMSS752
appears10:56:35 and CSRSS932 at10:59:19. Six processes through11:02.
The67.594-second counter window10:55:53.906–10:57:01.500 attributes handler/
L1/L2 spans56.665%/26.455%/16.882% on CPU0 and57.790%/26.294%/15.918% on CPU1.
Mean closed handler spans141,115/129,799 cycles. This window includes Autochk
exit and second-SMSS startup, unlike the baseline's later PnP window, so it
is not a controlled per-operation speedup measurement. Raw reads take
12.58/17.94ms; first-TSC fingerprints/layouts match.

V5 completes two USB timer-stop calls, then10:58:21 its combined RSP/thread-
stack-bounds assertion rejects a CPU1 dispatcher entry atnt+2bb96b. The failed
registers/bounds were not journaled. The third call is unpaired across the
detach, not proven stuck. The fallback crash guard attaches afterward.
At11:01:23, after old manager12722/guard14514 exit, manager15442/GDB15444
start v8 in gdb-v8/. It records dispatchers outside their thread-stack bounds,
retains the outer flush return and declines inner pairing for that case.
PRCB.Number is now checked against the debugger CPU; failed-stop registers
are retained. These changes are only in the host GDB script, and await a
hardware hit of the new case. V8 SHA256
f1d803899ba869bcfb5a0738238937d197f461155961afb01d686ef2a8fcb7f1.
At initialization the PnP completion event is signaled with an empty list.
