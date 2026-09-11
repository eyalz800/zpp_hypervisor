# Per-CPU borrow boot: debugger captures and continuing startup

This is the 17:27 UTC September 11 boot of 6adda123, loader MD5
d564ca8f8057eabdb36a09db2c1e34d5, on the unchanged two-CPU manifest.
The module is at 66d47000 and the singleton at 682f3000. Windows is at
fffff801d4a00000; system CR3 1ae000, PE timestamp 51a135d9 and image size
1450000 are validated. All addresses below belong to this run.

## GDB memory access and a shadow-copy hypothesis

A copy_vmcs12_to_shadow breakpoint at module+84ca0 fired on CPU 1, but
GDB's virtual read of the VMCS12 failed. This was an instrumentation
failure, not a guest failure. Switching the memory channel to physical
reads worked. QEMU documents the Qqemu.PhyMemMode:1/0 packets in its
[GDB memory documentation](https://www.qemu.org/docs/master/system/gdb.html#examining-physical-memory).

At 17:52:00, the stopped copy had a valid shadow cache and six differences
among its fifteen fields: exit reason, RIP, RFLAGS, entry interruption
information, CS access rights and SS access rights. A second hardware
breakpoint caught the same call's return: CPU and RSP agreed; writes
advanced by six, skipped writes by nine, and completed stores by one.
The caller was reflect_l2_exit at module+6756f. Captures took 12.3 and
4.1 ms, excluding complete stop/resume latency. This call needs its update;
no shortcut was implemented based on the earlier empty-copy hypothesis.
Current source and disassembly have two direct callers, reflect_l2_exit
and set_vmcs_shadowing; the function's old four-caller comment is stale.

PhysicalGuestMemory in /tmp/zpp-20260911/gdb-guest-memory.py now walks a
captured CR3 while GDB has the vCPUs stopped. It checks present entries,
supports 4 KiB/2 MiB/1 GiB mappings, limits transfers at page boundaries,
and restores normal GDB memory mode on exit. Windows stack captures first
validate the kernel PE through that address space. This also fixes the
bugcheck capture's dependence on GDB's unreliable nested virtual view.

## csrss scheduling

A complete process/thread walk at 17:58:22 found csrss PID 944,
EPROCESS ffff9209433d6140, CR3 1a230e002 and main thread
ffff92094339c140. The main thread was Running, with 404 context switches;
its WaitReason field was not interpreted as a current wait. The other
three threads were Waiting. Every process/thread backlink and termination
at its list head was checked.

At 18:00:24 a hardware write watchpoint on main-thread+154 fired at
nt+6b4581, immediately after inc dword [rsi+154], with RSI naming the
selected thread and CR3=1a230e002. It proves that thread was scheduled on
CPU 1. The following virtual-memory read failed, so no stack was recovered
from that stop. A later thirty-second physical-reader probe did not obtain
another watchpoint hit; it does not prove starvation or a permanent stall.
The boot subsequently advanced to other user processes.

## Power requests and worker stacks

The watcher now retains process and power replies in a selected capture
directory, with a UTC poll-start timestamp. It reads power state every poll,
including below six processes, and keeps failed reads separate. The power
reader no longer labels several aging requests as an upstream cause, or
one aging request as proof against a driver. Those causal claims did not
follow from the list. The old footer also used 600 seconds beside the
reader's measured 300-second reference; it is removed.

The audio request ffff920943d35d80, PDO ffff920942dd4d50 (IntcAudioBus),
current device ffff920942dd5d50 (IntcOED), was armed at the 18:01:01 and
18:01:27 polls, ages 6.7 and 33.0 seconds. It was absent from the next
complete list. This does not reveal its completion status. Subsequent USB
requests appeared, left the list, and reused IRP addresses. Track the data
entry, PDO and watchdog start as well as the IRP address.

At 18:04:17, the checked worker walk had three workers, two inside HidUsb
and one idle, with no pending queue count. The active pairs were:

| Thread | IRP | Device |
|---|---|---|
| ffff9209404ef040 | ffff9209432d6480 | ffff9209432f1060 |
| ffff920945ba6040 | ffff920945be9730 | ffff9209432d7060 |

At 18:05:02, GDB stopped the vCPUs and captured both threads in about
34 ms. Both were Waiting (State 5, Executive), and matched PE unwind
metadata reconstructs:

SwapContext -> KiSwapContext -> KiSwapThread -> KiCommitThreadWait ->
KeWaitForSingleObject -> PopIrpWorker+112 -> PspSystemThreadStartup ->
KxStartSystemThread.

They had returned to the worker's wait for new work by this later capture.
The earlier HidUsb call must not be reported as continuously active.
The compatibility record's before/after fields describe this one stopped
capture; they are not two independent live samples.

A later batch of four USB requests remained armed from about 18:05 until
18:09. The complete 18:09:39 power list had none armed, before the first
request reached the reader's 300-second reference. No successful device
completion status is inferred merely from leaving the list.

## Current driver images and wininit

The current Wdf01000, IntcOED and HidUsb images were captured after a
complete 169-module walk, using this boot's mappings. Their PE sizes,
timestamps and CodeView identities are recorded. Missing pages are explicit:
11 for WDF, 74 for IntcOED and 4 for HidUsb. All three exception directories
are readable. The older WDF PDB has the same GUID but a different reported
age, so its names are not silently substituted; the current PE unwind
metadata suffices for the captures here.

At 18:08:46, WerFault PID 732 had command line
C:\\WINDOWS\\system32\\WerFault.exe -k -c.
Its own PEB, process CR3, executable name and image-path suffix were checked.
This is not evidence, by itself, that its parent wininit crashed.
Wininit PID 632 had seven Waiting threads. A 20-ms GDB capture at
18:09:44 of its main thread reconstructs NtWaitForSingleObject through
ObWaitForSingleObject and KeWaitForSingleObject. The final machine frame
returns to user RIP 7ffe9ec40407 and RSP 23db94f468; user stack memory was
not captured, so the waited-on user object/caller is not identified.

## State at handoff and artifacts

At 18:11:31 the watcher had nineteen processes, a running guest, no armed
power watchdogs and no LogonUI/dwm. Services, svchost, WUDFHost and wermgr
have appeared. The Windows login/desktop goal is still unverified.

GDB's KeBugCheckEx breakpoint remains armed in tmux window gdb-wininit,
using port 1234. Its transcript is cache-local-borrow-gdb-wininit-main.txt;
a hit/timeout writes cache-local-borrow-gdb-bugcheck/index.json. The
physical-memory stack reader is active in that script. The forty-minute
running bound restarted after the wininit capture. Manual interruptions
from earlier probe transitions are archived with -before-* suffixes and
must not be treated as bugchecks. local-borrow-logon alone owns port 4446.

All artifacts are under /tmp/zpp-20260911/. The main groups are
cache-local-borrow-gdb-{shadow-copy-physical,csrss-switch*,power-workers,
wininit-main} and their JSON/stack/unwind files; the -wdf, -intcoed and
-hidusb directories; -werfault-parameters and -wininit-threads; and
-watch-captures with every complete/failed reply. The append-only watcher
transcript is cache-local-borrow-watcher.txt.

Shell syntax and live capture files were checked. All 27 rebuilt host tests
passed in 30.68 seconds, including 228 Python tests. These reader changes
and debugger observations do not change the deployed hypervisor binary.
