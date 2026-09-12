# GDB catches the power failure on QEMU's added USB hub

The September 11 19:21 UTC run used deployed hypervisor **6adda123**,
loader MD5 **d564ca8f8057eabdb36a09db2c1e34d5**, two CPUs and the unchanged
full manifest. GDB caught KeBugCheckEx on CPU 0 at **20:55:05 UTC**:

```
Bugcheck  9f
Arg1      3
PDO       ffffe6044844a870
Triage    fffff8002952c600
IRP       ffffe60448a40560
```

The hardware breakpoint capture took 14.7 ms and preserved registers and
2,048 stack bytes. The validated unwind reaches PopIrpWatchdogBugcheck,
PopIrpWatchdog, timer expiration/DPC retirement and KiIdleLoop. QEMU later
paused (shutdown). The maximum process count was fifteen; no LogonUI/dwm
or login was verified. This was the sole SCM script event. The intended
RpcEptMapper/LSM failure ordering remains unobserved.

Kernel base **fffff80096a00000**, system CR3 **1ae000**, PE timestamp
**51a135d9** and SizeOfImage **1450000** were validated. Resident module
base is **66d47000**, singleton **682f3000**. Use new coordinates after
another boot. The final resident census reports mismatching CPUID totals;
its census counts are excluded from conclusions. Its generic region-path
warning is not a root-cause finding: the displayed failed flush and
post-flush disagreement counts are zero, and initial zero/unflushed loads
alone do not prove incorrect VMCS state.

The PDO/FDO chain, device-object extension backlinks and PDO devnode agree.
Both device objects use `\Driver\USBHUB3`. The devnode is Started, with
PendingIrp zero, and names:

```
USB\VID_0409&PID_55AA\MSFT20314159-0000:00:05.0-4
```

This is QEMU's hub: its descriptor specifies VID 0409, PID 55aa and serial
314159 in [QEMU 11.0.3 dev-hub.c](https://github.com/qemu/qemu/blob/v11.0.3/hw/usb/dev-hub.c#L98).
The real stopped guest's `info usb` confirms Bluetooth at root port 1,
tablet at 2, keyboard at 3, hub at 4, and two keyboards at 4.1/4.2. The
launcher actually contains **three** usb-kbd devices and the corresponding
input objects. Earlier shorthand saying one keyboard was incorrect.

QEMU's [usb_claim_port](https://github.com/qemu/qemu/blob/v11.0.3/hw/usb/bus.c#L364)
automatically creates a hub when one free port remains and an unassigned
non-hub device arrives. The xHCI properties default to four USB 2 and four
USB 3 ports, sharing root connectors, as confirmed by the rig binary's
`-device qemu-xhci,help` and
[hcd-xhci.c](https://github.com/qemu/qemu/blob/v11.0.3/hw/usb/hcd-xhci.c#L3162).

The PDB-derived IRP layout is size d0, CurrentStackLocation at b8,
StackCount at 42 and CurrentLocation at 43; stack-location size is 48.
Type 6, size 1288, stack count 14 and current location 11 validate the
stack pointer algebra. Active stack 11 is major 16/minor 2, control e1,
DeviceObject equal to the PDO, completion **UsbHub3+1c910**, context
**ffffe6044aa480d0**. Header IoStatus, Cancel and PendingReturned are zero.
Power parameters are Type 1, State 3. Stack 13 names the FDO and completion
nt+4127d0, with POP_IRP_DATA context ffffe60447c447d0.

Both final power workers have null IRP/device fields. This captures idle
workers at the stopped state; it does not establish the cause or a
continuous prior state. The full System-thread backlink walk has 173
threads: 170 Waiting, two Ready, one Running. All 172 Ready/Waiting stacks
are preserved; the Running saved stack is excluded. With matched kernel
and current WDF/UsbHub3 exception metadata, 92 reach null returns and 80
explicitly stop at other modules for which metadata was not loaded.

Artifacts under `/tmp/zpp-20260911/` include `rpc-gdb-scm-startup/`,
`rpc-gdb-bugcheck-unwind.txt`, `rpc-gdb-final-usb/`,
`rpc-gdb-final-power-workers.txt`, `rpc-gdb-final-system-stacks/`,
`rpc-gdb-final-system-unwind.txt`, `rpc-gdb-final-usbhub3/`,
`rpc-gdb-final-wdf/`, the actual launcher/USB topology, final serial,
NVRAM, resident report and exact deployed ELF. The evidence manifest
`rpc-gdb-evidence-sha256.json` hashes 800 files. Driver memory images
explicitly list missing pages; holes are not treated as captured bytes.

The next experiment changes only `qemu-xhci,id=xhci` to
`qemu-xhci,id=xhci,p2=8`. All five devices/input objects, two CPUs, binary,
manifest, clocks and nested Hyper-V configuration remain. This changes
the device topology and speeds of the former hub's keyboards and tests
whether avoiding that external hub permits progress. It is not a proven
VMM fix, QEMU fault or successful Windows boot. No Windows device, service,
BCD or registry setting is changed.

An isolated 128-MiB stopped TCG test using the rig's QEMU 11.0.3 and
read-only OVMF showed five devices on root ports 1–5 and no hub. It used
a generic mouse for the Bluetooth position, no physical USB/input, VFIO
or Windows disk, and then quit. An initial attempt lacked the default
BIOS; the successful topology test still emitted a kvmvapic.bin warning.
These are preflight limitations, not Windows results. Preserve the guest
evidence before supported teardown, and do not edit the launcher while
its old shell is still interpreting it.


The supported next boot began **21:12:41 UTC September 11**. The previous
QEMU exited normally, NVMe returned, no QEMU/launcher remained, and over
15 GB RAM was free. The one-line launcher change was syntax-checked,
backed up as `boot-zpp.sh.bak-before-direct-ports-20260912`, and read back
byte-for-byte. GPT/NTFS/ESP signatures match; the ESP has its recorded
33,423,360 sectors, not the unreserved partition size 33,554,432. An
initial check assumed the latter and was corrected against the prior
archive and rig procedure. Nothing changed the disk layout.

The fresh read-only loader matches the preceding archive byte-for-byte;
Limine MD5 is c4357a8d21ddb98046aa2f9fd13280b0. The real new guest's info
usb shows Bluetooth on 1, tablet on 2, keyboards on 3/4/5, no external hub.
Windows base is now fffff801e1c00000, kernel PE/system CR3 1ae000 validated,
with complete three-process walks. No login is verified. GDB's hardware
bugcheck guard and prepared SCM handoff are recorded in current STATUS.

A startup-observer race became visible during GDB attach: bare `paused`
was treated as shutdown. It now retries debugger/manual pauses and exits
on `paused (shutdown)`. The continuing logon watcher has the same fix.
The observer's rejected first attempt is preserved; later complete walks
succeed. Shell syntax checks pass. No guest source/binary change or host
suite rerun was needed for this launcher/observer experiment.


The matching UsbHub3 PDB was fetched from Microsoft after the next boot
started. GUID 22b2bc36-58b4-f6e1-69d3-bfa9bf10447c, image age 1, Info age
4 and DBI age 1 agree. RVA 1c910 is exactly
HUBPDO_WdmPnpPowerIrpCompletionRoutineForAsynchronousCompletion. Its
successful path creates a work item, stores the IRP in its typed context,
enqueues the work item and returns c0000016 (MORE_PROCESSING_REQUIRED).
The callback is HUBPDO_EvtCompleteIrpWorkItem at 15370. The saved current
WDF function table resolves offsets bd8/be0 to WdfWorkItemCreate/Enqueue,
so these calls are identified from current pointers as well as symbols.
This maps a future entry/callback breakpoint pair; it does not establish
that this particular IRP reached either call. The old guest's work-item
object/context was not captured and is no longer available after teardown.

At 21:21:56 a 303-ms GDB capture completed all 115 System threads and
identified Phase1 ffff9784b54de080 by both start-address fields. It was
Running, so its saved stack was excluded. At 21:23:26 a hardware stop at
zpp on_l2_exit matched that current Windows thread and an NT RIP, but
refused its proposed stack: context.rsp is the host context pointer, not
VMCS.GUEST_RSP. This is also explicit in exit_dispatch.cpp. That frame
must not be unwound as a Windows stack. A paired Windows-address probe
at 21:24:25 then rejected the changed Phase1 identity, without capturing
a Windows stack or return. Preserve these debugger checks as failed
probes, not guest failures.

The complete process walk first observed smss PID 544 at 21:24:07, about
11m26s after launch, versus roughly 55 minutes in the preceding run.
This is a progress comparison from one run, not causal proof. All fresh
probe artifacts are hub-direct-phase1*, and the current GDB guard was
restored afterwards. No armed power request was observed through 21:24.


## Direct ports still fail: tablet timer stop and its lock owner

The final stopped state, recovered around **06:57 UTC September 12**, is
0x9F/3 with PDO **ffff9784badac060**, triage **fffff801746b49f0**, and IRP
**ffff9784b5489010**. The user subsequently confirmed the physical display
says **“driver power state failure.”** Neither this nor the complete
34-process list (LogonUI 1440, dwm 1452) verifies a prior sign-in screen.
The startup observer ended at its 360-poll bound at 23:38:35; the GDB
guard later expired at 00:34:34. There is no direct breakpoint capture or
known exact crash time for this run. Future observation must survive long
boots rather than silently ending at two or three hours.

The devnode instance is
`USB\VID_0627&PID_0001\28754-0000:00:05.0-2`, service HidUsb, Started.
The actual topology identifies root port 2 as the QEMU tablet. The PDO
uses USBHUB3 and its attached FDO **ffff9784bae3d0a0** uses HidUsb. IRP
stack count/location are 16/12, current completion UsbHub3+1c910, context
**ffff9784b8479110**. IoStatus is c00000bb, Cancel/PendingReturned zero.
An installed completion pointer does not establish that it ran.

The final power-worker list has one busy worker **ffff9784b54ef040**, with
this exact IRP/FDO, and one idle worker. Of 177 System threads, 169 are
Waiting, seven Ready and one Running. All 177 raw stacks are preserved;
176 saved Ready/Waiting contexts are eligible for the checked unwinder.
With current WDF/UsbHub3/HidUsb/HIDCLASS and this boot's Msfs metadata,
93 reach null and 83 explicitly stop at other missing module metadata.

Thread **ffff9784b55d6040** waits in WDF's
`FxPkgPnp::_PowerProcessEventInner+45`, holding package address
**ffff9784baaa6830** in RDI, lock **ffff9784baaa6a38 = package+208** in RBX,
and the failed PDO in R15. The lock's SignalState is zero and its owner
field at +20 is **ffff9784b54ef040**. The work item at **ffff9784babad640**
also records this busy power worker as WorkOnBehalfThread; that latter
field alone is not ownership evidence. Here the lock itself establishes
ownership. The independent timer object **ffff9784b8796810**, recovered
from the owner unwind, also records that thread in its stop-owner field
at +150, whose store is visible in current WDF code.

The Running thread has no valid saved KSP or KTHREAD TrapFrame pointer,
and both KPRCB ContextFrame records are zero. Its stack was instead
recovered from the software VMCS12 region **114fa0000**. The exact deployed
ELF supplies the VMCS12 layout; this is not a guessed hardware VMCS
layout. The resident region ledger's last successful flush agrees with
its RIP **fffff801e21bf139 = KiCheckStall+79**. RSP is
**ffff858055da8b60**, CR3 **1ae002**, GS **ffff858055d14000** (CPU 1),
CS 10. VMXOFF left current-VMCS pointers invalid; the flushed region and
freeze stack are the preserved context. A first read hit the NMI stack's
next unmapped page; the bounded 1,184-byte tail was then captured.

The unwind reaches KiFreezeTargetExecution, KiCheckForFreezeExecution,
KiProcessNMI, KxNmiInterrupt and KiNmiInterruptStart. The actual machine
frame switches to **ffffeb033681e950** inside the preserved owner's stack,
interrupted at HvlEndSystemInterrupt+1e. It continues through
HalPerformEndOfInterrupt, KiDpcInterrupt, KiCheckForThreadDispatch+7f,
KeSetSystemGroupAffinityThread+18e, KeGenericProcessorCallback+14e,
KeFlushQueuedDpcs+18f, **imp_WdfTimerStop+19f**, and
**HUBPDO_EvtDeviceD0Exit+34b**, then the tablet's WDF power state machine,
HidUsb/HIDCLASS, PopIrpWorker and startup to null. The reconstructed IRP,
package and USB context match the independent captures. No volatile
register is claimed preserved across ordinary calls.

Current WDF and UsbHub3 RSDS GUID/age match the already validated PDBs:
**c7872216-06ea-52af-662a-b77697c12a13/1** and
**22b2bc36-58b4-f6e1-69d3-bfa9bf10447c/1**. Current NMI, DPC and dispatch
code anchors match the reference NT bytes exactly. The larger freeze
code range has runtime patches after KiCheckStall; its own bytes and the
relevant unwind prologues are retained, and this range is not described
as an exact whole-range match.

This is a complete crash-time dependency chain. It does not prove a
300-second uninterrupted stall, a lost USB callback, or a specific zpp
fix. A fresh hardware probe can stop at **UsbHub3+15db6** before the timer
stop, then **Wdf+4341a** before KeFlushQueuedDpcs and **Wdf+4341f** after
it. The final USB callback return is **UsbHub3+15dbb**. Resolve module
bases again after boot; do not reuse absolute addresses.

One earlier SMSS call to KeFlushQueuedDpcs did return: the paired hardware
stops at 21:34:05 captured entry in 38.9 ms and return in 29.8 ms, with
14.9 ms of host time between continue and stop. The caller was
MmPageEntireDriver+4d, then this boot's Msfs+ae14. Unchanged shared
InterruptTime does not establish zero execution or a stopped clock.
That completed call cannot establish that all later flush calls return.

New artifacts under `/tmp/zpp-20260911/`: `hub-direct-final-usb/`,
`hub-direct-final-processes/`, `hub-direct-final-system-stacks/`,
`hub-direct-final-system-unwind.txt`, `hub-direct-final-power-workers.txt`,
`hub-direct-power-evidence/`, `hub-direct-lock-owner/`,
`hub-direct-vmcs12/`, `hub-direct-frozen-cpu-page/`,
`hub-direct-unwind-anchors/`, `hub-direct-flush-dpcs/`, `hub-direct-msfs/`,
final serial/NVRAM and exact deployed ELF. The first resident report used
an invalid --l2 40 CPU selector; its CPU-40 appendix is excluded. The
replacement `hub-direct-final-resident-validated-selector.txt` uses CPU 1.
Generic resident stack scans remain heuristic and are not the checked
unwind above. Its broad region-path warning and compiled-out census
zeros do not establish defects.


The direct-port crash's manifest now hashes **1,774 files** (18,729,063
bytes): `hub-direct-evidence-sha256.json`. Supported teardown returned
NVMe and 15,476 MB free RAM. The first preflight assumed the ESP was
mounted after teardown; it was already unmounted, so the unmount command
refused. After checking actual mounts, the fresh read-only mount succeeded.
The loader/launcher are identical, Limine matches, and GPT/NTFS/ESP anchors
validate. The mount was unmounted before the next boot.

The next same-configuration run began **07:22:39 UTC September 12**, with
hardware timer-stop/flush entry-return probes prepared and observers that
stay alive until a terminal state or explicit handoff. All three startup
channels and actual direct USB topology validate. Kernel base is now
**fffff804c5800000**. Its PE maps via 1ae000; the first process/module list
heads were not initialized and those attempts were rejected. New artifacts
and ownership are in `/tmp/zpp-20260912/timer-probe-*` and current STATUS.
The new temporary probe scripts passed syntax checks; actual breakpoint
hits remain to be demonstrated. A concurrent unrelated return abandons a
pair explicitly. Positive guard timeouts remain supported, but this run
uses timeout zero for an indefinite guard. No source/binary/Windows change.


The new run observed SMSS PID 692 at 07:29:12 and autochk PID 712 at
07:42:11. An early kernel flush/dispatcher probe was tried while USBHUB3
was still unloaded. Its 120-second window (roughly 07:42:17–07:44:17)
recorded no entry breakpoint, detached without a call/return claim, and
the manager restored an indefinite hardware bugcheck guard. The old
manager/GDB pair was explicitly interrupted and verified exited before
replacement; the monitor watcher never changed owners. Active state is
`timer-probe-gdb-early/manager.json` and the corresponding tmux window.
The later USB timer-stop probe now additionally has bounded hardware
stops on the dispatcher epilogue and RET; unrelated dispatch threads
leave the outer flush pairing intact. Probe events are journaled with
bounded per-stop writes, with no lifetime limit on completed pairs.


At 07:46:24 autochk was first absent from the complete process walk
(last present 07:46:03). No exit status or check/repair result was captured.
The module list later contains 127 drivers; USBHUB3 is still absent.

An independent bounded dispatcher return probe at **07:54:15** reached
nt+2bb96b, nt+2bb97f and its actual caller nt+30e82e, all on CPU 0,
System thread **ffff9d8776b87040**. RSP advanced 0x48 then 8 and all four
saved/restored nonvolatiles matched. Capture durations were 29.2, 22.3
and 21.0 ms; the two continue-to-stop host intervals total 18.2 ms.
Each captured stack independently unwinds through KeSetSystemGroupAffinityThread,
**HalpCmcWorkerRoutine**, ExpWorkerThread and system-thread startup to null.
This invocation returns; it is not the USB timer-stop path and does not
exonerate other calls. A breakpoint directly at the epilogue can catch
execution after a call whose entry occurred before debugger attachment.

The prior GDB manager/guard were interrupted and verified exited before
starting `timer-probe-gdb-dispatch` in the same tmux session. Its bounded
probe completed, and the manager restored an indefinite bugcheck guard.
The monitor watcher remains the same sole owner. Current manager state,
three raw stacks/register captures, transcript and checked kernel unwind
are under `/tmp/zpp-20260912/timer-probe-gdb-dispatch/`. The prepared USB
probe still takes over when matching USB/WDF images validate. No guest
binary, configuration, register or code-byte changes were made.


USBHUB3 loaded by the **07:55:54** complete module read (147 entries).
The current USB base is **fffff8045f5e0000**, WDF **fffff80457d80000**;
timestamps, sizes and expected probe bytes match. The discovery watcher
stops module reads after producing the validated config, so this is a
point-in-time module count. Second SMSS PID 816 appeared at 07:54:51.

The initial USB probe ran from 07:55:57 without any hit before a supported
GDB-owner handoff. At 07:57:25 the v2 probe started under manager 58201,
GDB 58202, tmux `timer-probe-v2`. It re-arms the flush-call breakpoint
after a paired flush return, because the captured WDF routine can branch
back from +43461 to +433f6 before returning from WdfTimerStop. It captures
the timer's stop-owner word +150 and state bytes +158/+159 at both flush
boundaries. An unrelated thread's flush call preserves the selected outer
timer return without claiming observation of its inner flush. Per-flush
host running intervals restart at each actual flush call.

Current source snapshot and SHA-256 are in `timer-probe-gdb-v2/`, with
manager state, transcripts and captures. The hash is
`f047090757afc61146a94dcd9215c23311fbb6f14d53cfb348f716a6bde10e89`.
The initial script file was edited after GDB had already loaded it;
those edits did not affect its running code. V2 uses a new filename and
an archived source snapshot, and the old owner/child exited before it
attached. As of 07:58:21 v2 has no timer-stop hit. The sole monitor watcher
and Windows run remain continuous across these debugger handoffs.


## 2026-09-12: live USB flush reaches a different dispatcher caller

At 08:14:22, GDB captured SMSS PID 816's single Waiting thread
ffff9d8776c700c0 in PnpSerializeBoot, waiting on nt+f8c3a0,
PnpSystemDeviceEnumerationComplete. Saved KSP/SwapContext and the kernel
unwind validate; stop through detach was 105.9 ms. At v3 attach 08:18:51,
that event is signaled with an empty wait list and the full PnpSerializeBoot
code range matches the PE. No actual return for the selected wait was
observed. CSRSS PID 288 first appears at 08:19:50; no verified login.

The first real USB sequence at 08:15:33 hit USB+15db6 (Wait=1), WDF+4341a,
nt+2bb96b and nt+2bb97f. All are CPU 0/System thread ffff9d87744bc040.
Timer ffff9d87777589c0 names that stop owner; bytes +158/+159 are zero.
The first three stops took 35.8/27.9/26.7 ms through checkpoint; the final
capture took 22.4 ms before the probe assertion. Dispatcher RSP+48h
matches, but its actual return address is nt+2bbb54,
KiProcessDeferredReadyList+0xb4. Both dispatcher stacks unwind through
KeSetPriorityThread -> KeGenericProcessorCallback -> KeFlushQueuedDpcs ->
WDF+4341f, stopping explicitly at missing current driver metadata.
The fixed-affinity-caller assertion was wrong. The probe detached and its
manager automatically restored an indefinite bugcheck guard. This is no
guest failure, complete timer/flush return, or proof of continuous delay.

The old manager 62898/fallback guard 63247 exited before v3 attached at
08:18:51. An earlier handoff attempt first rejected stale child PID 62913
without mutating anything. Current tmux timer-probe-pnp has manager 64140,
GDB 64141, under /tmp/zpp-20260912/timer-probe-gdb-v3/. The corrected probe
follows the actual dispatcher return address; unpaired inner dispatcher
frames preserve the outer flush probe. It also watches the captured PnP
wait's resume address by exact thread/RSP while no USB cycle is active.
There are at most three hardware breakpoints, no guest byte/register writes,
software breakpoints, stepping or inferior calls. The watcher remains PID
49122 throughout. No pairing is claimed across the debugger handoff gap.

V3 source snapshot SHA-256:
e197275fbb7401caea742c7843fbe98e5fcd80312b9d65a27f0e33cbb8ddd69b.
The earlier SMSS and USB captures, exact prior source and partial kernel
unwind are in timer-probe-gdb-smss/. As of 08:23:12 v3 has no hit; process
count six, no armed power watchdog, no LogonUI/dwm. The unchanged 07:22:39
boot continues with both observers indefinite. No build/deploy or Windows
configuration change was made.
