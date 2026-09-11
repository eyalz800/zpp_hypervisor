# Session Manager startup through direct GDB

The unchanged 6adda123 two-CPU boot started around 19:21 UTC on September
11. Its Windows base is `fffff80096a00000`, system CR3 `1ae000`. At
20:38:59 the complete observer has six processes, without services.exe,
LogonUI/dwm or an armed power watchdog. This is progress, not a verified
Windows login. Preserve the running guest and the pending SCM capture.

At 20:32:50, a physical-memory GDB capture validated smss.exe PID 568,
EPROCESS `ffffe60447e2c040`, its CR3, kernel PE and complete four-thread
list. All four threads were Waiting. Kernel/user stacks and the complete
two-image PEB module list were captured. Reading through detach took
209 ms; the guest was not left stopped for offline analysis.

Matched unwind metadata gives these paths, each reaching RtlUserThreadStart
and a null return after crossing the captured trap frame and TEB bounds:

- Main thread `ffffe60447e37040`: NtWaitForAlertByThreadId,
  RtlSleepConditionVariableSRW+1de, SmpWaitForSubSysStartup+ce,
  wmain+92b and the native process entry routines.
- Thread `ffffe60447e35080`: KeWaitForSingleObject, PnpSerializeBoot+4a,
  NtSerializeBoot+3a, SmpNtSerializeBoot+19,
  SmpAsyncMemoryConfiguration+1f and thread-pool callback dispatch.
- The other two threads wait in NtWaitForWorkViaWorkerFactory and
  TppWorkerThread; their stacks do not identify active boot work.

The matched PnpSerializeBoot code waits indefinitely on
PnpSystemDeviceEnumerationComplete (`nt+f8c3a0`) in this path. This is an
asynchronous memory-configuration worker, **not the main thread's wait**.
No event state was separately captured, no continuous wait duration is
proved, and the stack alone does not identify the unfinished device.
SmpWaitForSubSysStartup's captured branch sleeps on a condition variable
while its subsystem list is empty. A second smss.exe, PID 724, appeared by
20:33:16; the process list reached six by 20:38:59. Do not call the older
wait a permanent stall or reuse a retired thread without validation.

A second GDB capture at 20:36:09 validated unchanged live PE headers and
read the executable/metadata pages needed for offline user unwinds. It
took 132 ms through detach. Microsoft symbol-server executables matched
timestamp, SizeOfImage, checksum, section table and captured code after
DIR64 relocation: smss 103,820 code bytes; ntdll 8,192 .text bytes and
47,668 exception-directory bytes. Paged-out metadata came from the matched
server images. Current ntdll fothk bytes were preserved; uncaptured smss
fothk remains an explicit hole and was not used by these unwinds.

PDB GUID/Info-age/DBI-age validation passed: smss
EF4956CD-1D38-BAB2-4007-DF0D073B0974, ages 3/1; ntdll
1DF9DB46-D55D-6B86-9568-C9F6E9287DE4, ages 4/1. Both images specify age 1.
Public symbols were mapped through the exact image section tables.

Artifacts are under `/tmp/zpp-20260911/`: `rpc-gdb-smss/`,
`rpc-gdb-smss-images/`, `rpc-gdb-smss-validated/`,
`rpc-gdb-smss-user-unwind.txt`, `rpc-gdb-smss-kernel-unwind.txt` and
`rpc-gdb-smss-wait-code.txt`. The capture scripts and PDB identities are
preserved beside them. Hardware breakpoints, physical-memory reads and
automatic detach were used; no single-step, inferior call or guest-memory
patch was used.

At 20:47:27, a 192-ms capture of csrss PID 916 completed its three-thread
and fourteen-module walks. The thread starting at csrss+1010 was Running;
its saved stack was excluded. One other thread unwinds through a scheduler
APC at PspUserThreadStartup (without a captured user stack); the last waits
in NtWaitForAlertByThreadId. A 20:48:38 on_l2_exit probe failed its combined
ETHREAD start-address/process identity check and detached. No live CSRSS
frame or matched return was obtained, and neither pointer reuse nor a
guest failure is proved by that assertion. Its artifacts are rpc-gdb-csrss
and rpc-gdb-csrss-live. Wininit/winlogon appeared in that interval.

The automatic SCM handoff completed at 20:49:27, superseding the earlier
guard ownership. services.exe is PID 716, image 7ff681350000, CR3
1b01b3002, EPROCESS ffffe6044ab610c0. GDB PID 69804 now runs the prepared
SCM failure capture; its coordinator is rpc-scm-coordinator-csrss.py,
PID 69732 when started. rpc-logon alone owns the QEMU monitor. The live
status is rpc-scm-coordinator.json, with SCM output rpc-gdb-scm-startup.
At 20:52:39, fourteen processes are present, no LogonUI/dwm and one armed
USB power request. No SCM breakpoint event has yet been captured. There
is no new VMM fix or Windows configuration change from this evidence.
