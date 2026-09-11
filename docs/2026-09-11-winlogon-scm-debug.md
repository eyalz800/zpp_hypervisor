# Winlogon event wait and service-start timeout caught with GDB

The unchanged 6adda123 guest ran from 17:27 until a GDB-captured 0x9F
at 19:03:32 UTC, September 11. It reached 73 processes, but no LogonUI,
dwm or verified login. This note continues the earlier local-borrow GDB
progress report. No new hypervisor binary, manifest, Windows configuration
or rig configuration was deployed.

## Winlogon's actual wait

At 18:22:39 UTC a scripted GDB stop captured all three Winlogon threads,
their kernel stacks, active wait blocks, trap frames and user stacks.
Reads took approximately 92 ms; this does not measure the entire attach
and detach latency. All three were Waiting. Current KTHREAD.Process
pointers matched Winlogon's EPROCESS; kernel PE identity and the user
TEB self-pointer/stack bounds were checked.

Winlogon PID 756, main KTHREAD ffff920945b0d080, has one wait block:

- Object ffff920942f108e0, notification-event type 0, SignalState 0.
- WaitBlock.Thread matches the KTHREAD; FirstArgument is handle 214.
- The object's name-info header resolves to TermSrvReadyEvent using
  its InfoMask and the kernel's ObpInfoMaskToOffset table.
- The captured winsta.dll code independently opens the UTF-16 name
  `Global\TermSrvReadyEvent`; its cached handle at RVA 5a8c8 is 214.

The stopped main-thread unwind is:

    SwapContext saved context
    KiSwapContext -> KiSwapThread -> KiCommitThreadWait
    KeWaitForSingleObject -> ObWaitForSingleObject -> NtWaitForSingleObject
    KiSystemCall64 -> ntdll+160407 -> KERNELBASE+1c11f
    winsta+ce31 -> _WinStationWaitForConnectEx+14
    winlogon+5b79e -> winlogon+656b5
    KERNEL32+2ccb7 -> ntdll+aad6c -> null return

The kernel-to-user machine-frame RIP/RSP agree with the independently
captured trap frame. User unwinding starts with only those two known
registers; other registers become known only when unwind metadata
restores them. `_WinStationWaitForConnectEx` is an exact export at
winsta RVA 2aa50. Its helper at ce10 contains the event open and wait.

The other two threads unwind through NtWaitForWorkViaWorkerFactory and
IoRemoveIoCompletion, then their ntdll worker and thread-start frames.
Their queue object is ffff9209439a2580, type 4, SignalState 0. Neither
stack places them inside an active service-start operation.

## Executable identity and paged-out metadata

Winlogon's complete PEB module walk had 29 entries. Several modules have
paged-out headers or exception-directory pages, so the first strict
all-headers-present walk failed. The replacement reports missing headers
per module while still requiring complete list/backlink checks. Every
selected image requires a valid PE header and matching loader size.

Captured images and identity:

| Image | Base | Size | PE timestamp |
|---|---|---|---|
| winlogon.exe | 7ff715bf0000 | f1000 | f578e7a3 |
| ntdll.dll | 7ffe9eae0000 | 266000 | 59a29eb0 |
| KERNEL32.DLL | 7ffe9e360000 | c9000 | e1d1980a |
| KERNELBASE.dll | 7ffe9bdb0000 | 3ff000 | dedf122e |
| winsta.dll | 7ffe9b480000 | 62000 | 338dcfd5 |
| services.exe | 7ff709420000 | dd000 | a4571758 |
| sechost.dll | 7ffe9ddb0000 | a9000 | 9e4e27ca |

Matching executables were fetched from Microsoft's symbol server using
timestamp and image size. The downloaded PE headers, checksum and section
table were compared to the captured image. After applying DIR64 ASLR
relocations, every resident executable byte and exception-directory byte
was compared. Differences were confined to the fothk sections; those
bytes are preserved from the live capture. Missing fothk pages remain
marked missing. They are not silently replaced with unverified runtime
thunk contents.

The services.exe comparison includes 602,112 resident .text bytes;
sechost includes 81,676. Their exception directories were entirely paged
out, so those unwind bytes come from the identified server executable.
The other selected images have nonzero resident exception-directory
coverage. This distinction is retained in each validation record.

## PDB age validation correction

services.exe names services.pdb GUID 07F96885-E22B-AD8B-E182-93AFC8AD8A94,
CodeView age 1. The downloaded PDB has the same GUID, Info-stream age 3,
and DBI-stream age 1. It passes Microsoft's actual matching rule:
GUID equality, Info age at least the requested age, and equality of a
nonzero DBI age with the image age. This is implemented and explained in
[Microsoft PDB1::OpenValidate4](https://github.com/microsoft/microsoft-pdb/blob/master/PDB/dbi/pdb.cpp#L808).
The two ages were read from their separate MSF streams, rather than
treating llvm-pdbutil's summary Age as the only age in the file.

The existing NT PDB has GUID C8A7F11B-37FE-2822-7B6B-11412E3A0519,
Info age 6 and DBI age 1. The WDF PDB previously set aside has GUID
C7872216-06EA-52AF-662A-B77697C12A13, Info age 4 and DBI age 1.
The earlier decision not to use WDF names from an apparent age mismatch
was conservative; Info-age inequality alone was not a valid rejection.
This does not authorize substituting a driver from a different build.

## The service manager is terminating service hosts

At 18:39:18, a hardware breakpoint at NtTerminateProcess (nt+ace740)
caught services.exe PID 956 requesting termination through handle 808,
with status zero. GS.CurrentThread, KTHREAD.Process, process name/PID,
and the caller's CR3 were captured and cross-checked. The target of that
particular handle was not captured; do not retroactively assign it a PID.

At 18:41:59, a separate breakpoint at PspTerminateProcess (nt+8f5c30)
caught the resolved target: svchost.exe PID 2388, EPROCESS
ffff920946115080, CR3 183f78002. Caller services.exe PID 956 had CR3
1acdeb002. The disassembled NtTerminateProcess call passes the referenced
EPROCESS in RCX and its saved exit status in R8D; here R8D was zero.
The stopped capture took 37 ms and includes both kernel and caller user
stacks. It does not yet identify which service PID 2388 hosted.

The checked user unwind reaches:

    ntdll+160907 -> KERNELBASE+ff6c9
    CWin32ServiceRecord::LogonAndStartImage+162e
    CWin32ServiceRecord::StartInternal+843
    CServiceRecord::Start+a6
    ScStartMarkedServicesInServiceSet+1e9
    ScStartServicesInStartList+222
    ScStartServiceAndDependencies+31f
    CDelayStartContext::Perform+201
    ntdll thread-pool and thread-start frames

At LogonAndStartImage+162e, the recovered RBX is 41d (1053),
[ERROR_SERVICE_REQUEST_TIMEOUT](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-).
The matched cleanup block tests EBX, obtains the process handle, clears
EDX, and calls TerminateProcess. A zero termination status therefore
does not imply successful service startup. This is a delayed-start
service's timeout cleanup. It does not establish that this particular
service is the missing signaler of TermSrvReadyEvent.

The next unfiltered PspTerminateProcess stop, at 18:53:48, caught
wermgr.exe PID 2840 terminating itself with status zero. Its command line
contains `-outproc` and PID 956; this is not another captured service-host
timeout. Its only thread was Running, so its saved kernel stack was
correctly excluded. The capture took about 103 ms. A subsequent paired
SCM-cleanup/child-termination probe caught the bugcheck instead; it never
obtained the intended paired service record and child stacks.

## LSM dependency failure and retained endpoint-mapper startup error

A complete, validated 748-entry SCM database walk completed shortly
before the crash and was repeated afterward. Matched services.exe code
establishes the record layout: FindServiceByName walks the pointer at
RVA ca4d0, follows record+18 and reads the name pointer at +38.
ScQueryServiceStatus copies the seven Win32 SERVICE_STATUS DWORDs at +50.
Driver types use a different virtual path, so their cached fields are not
treated as equivalent API status. Each record's vtable and first function
were checked against the matched image, with stable identity fields and
a complete, unique, null-terminated walk.

| Service | API state | API Win32 error | Internal start error +ac | Internal state +b0 |
|---|---|---|---|---|
| LSM | Stopped | 1068 | 1068 | 4, failure |
| RpcEptMapper | Running | 0 | 1070 | 3, completed |
| RpcSs | Running | 0 | 0 | 3, completed |
| DcomLaunch | Running | 0 | 0 | 3, completed |

1068 is ERROR_SERVICE_DEPENDENCY_FAIL; 1070 is ERROR_SERVICE_START_HANG.
TermService, SessionEnv and UmRdpService retained 1077, never started.
The definitions are in [Microsoft's system error codes](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-).

AreDependenciesStarted (services RVA 18d98) establishes the forward
dependency layout at record+70, DEPEND_RECORD next+0, kind+8 and target+10.
Kind 80 refers to a service record. LSM's dependencies are RpcSs,
DcomLaunch and RpcEptMapper; RpcSs depends on the latter two.

Correction: RpcEptMapper's retained error 1070 does **not** make its
current state a failed dependency. The matched routine skips internal
state 3 as completed; failure processing uses state 4. LSM still retains
its own dependency failure. Late completion of the endpoint mapper after
LSM failed is a hypothesis, not a captured transition. Catch the earlier
startup-state/error changes on the next boot before assigning causality.

The RPCSS-group svchost PID 1224, CR3 1a4a63000, has rpcepmap.dll at
7ffe985e0000 and rpcss.dll at 7ffe98610000. Its complete 24-entry PEB
module list and both images, with explicit missing pages, are preserved
for preparing startup breakpoints. Addresses must be rediscovered after
reboot. No service, registry or boot configuration was changed.

## Actual power-watchdog crash and final stopped workers

At 19:03:32, GDB stopped at KeBugCheckEx (nt+4f90b0), with code 9f,
parameter 1 = 3, PDO ffff920946256060, triage pointer fffff801674ec600,
and IRP ffff920946165010 recovered from [RSP+28]. The capture took 35 ms
and detached. The stopped stack unwinds through PopIrpWatchdogBugcheck,
PopIrpWatchdog, timer expiration, DPC retirement and KiIdleLoop to a null
return. A later attempt to read beyond the captured stack hit an unmapped
page; the saved 2,648 bytes contain the complete unwind and bugcheck args.
The event's SignalState was still zero at this stop.

The last complete power reply began at 19:03:27. The two USB requests,
IRPs ffff920946165010 and ffff9209462b78b0, were already about 301 seconds
old before the GDB stop. This is a different late batch from the earlier
audio and USB requests that retired. At 19:03:55 QEMU reported paused
(shutdown), and the watcher exited.

The paused-state worker walk has three workers: two with those USB IRPs,
one idle. Both busy workers are Ready in the final saved state, not
Waiting on a current dispatcher object. Their complete PE unwinds reach:

    SwapContext -> KiSwapContext -> KiQuantumEnd -> interrupt dispatch
    KiCheckForThreadDispatch -> KeSetSystemGroupAffinityThread
    KeGenericProcessorCallback -> KeFlushQueuedDpcs
    Wdf01000 -> UsbHub3 -> WDF power-state-machine frames
    IopPoHandleIrp -> IofCallDriver -> PoCallDriver
    HidUsb -> HIDCLASS -> PopIrpWorker -> system-thread startup -> null

The idle worker reaches PopIrpWorker+112 waiting for new work. These are
post-crash saved contexts; they do not prove uninterrupted residence at
one instruction for 300 seconds or complete worker-pool saturation.

The final complete 199-entry module list reveals that hidusb.sys and
IntcOED.sys unloaded/reloaded at new bases within this boot. Final HidUsb
is 70fc0000, HIDCLASS 70800000, UsbHub3 6a120000 and Wdf01000 67030000
(all prefixed fffff801). The complete final unwind uses these current
images and readable exception directories. Early driver bases must not
be substituted merely because the kernel base and boot are unchanged.

## Artifact locations and next experiment

All artifacts are under /tmp/zpp-20260911/. Key groups:

- cache-local-borrow-gdb-winlogon and its kernel/user unwind text files.
- cache-local-borrow-winlogon-{modules,winsta}, services-images, and their
  verified-server subdirectories, original captures and hole lists.
- cache-local-borrow-gdb-process-exit and -process-exit-target, including
  the latter's user-stack.bin and checked unwind.
- cache-local-borrow-service-modules/process-modules.json: complete
  module walks for 30 selected processes; one svchost PEB was unreadable.
  No readable list contained lsm.dll or termsrv.dll at that observation.
  This neither locates those components elsewhere nor proves that they
  cannot load later.
- services-public-symbols.json, services-pdb-publics.txt, the preserved
  Microsoft PDB source files and services.exe/codeview.json.
- cache-local-borrow-service-{status,dependencies}, including the full
  service database and dependency graph with API/internal status separated.
- cache-local-borrow-final-rpc-images, including the RPCSS module list.
- cache-local-borrow-gdb-scm-child/index.json and stack.bin: the actual
  bugcheck capture, with cache-local-borrow-gdb-final-bugcheck-unwind.txt.
- cache-local-borrow-final-power-{workers.txt,stacks,unwind-complete.txt},
  final-{hidusb,hidclass,usbhub3}, and watch-captures/power-20260911T190327Z.txt.

No watcher or GDB session remains active. The redundant generic bugcheck
guard started after the true capture and was terminated after QEMU's
shutdown; it did not produce gdb-bugcheck/index.json. Do not substitute
that nonexistent artifact for the actual gdb-scm-child capture.

The event's SignalState address is ffff920942f108e4. The initial
three-minute watch expired without a hit; its attempted timeout stack
read used a zpp CR3 and failed, which is not a Windows failure. Later
manual transitions read SignalState zero through the system CR3 before
detaching. All manual transitions are archived with before-* names and
must not be treated as guest bugchecks. The corrected event watcher
uses the system CR3 for the event and the stopped caller's CR3 for its
stack. The process/power watcher retained each complete reply before it
exited. The deployed hypervisor is unchanged.

The next experiment should catch RpcEptMapper's startup and SCM's
1070/1068 transitions early with targeted hardware breakpoints, retaining
short scripted captures, physical CR3 validation and automatic detach.
Also retain the concrete KeFlushQueuedDpcs/affinity-return evidence when
investigating the underlying nested scheduling problem. No new C++ fix
is established by these observations. The Windows boot goal remains open.

Supported teardown returned NVMe and 15,445 MB free without a host reboot.
Final resident report, deployed ELF and fresh-mount loader are archived as
cache-local-borrow-final-resident.txt, cache-local-borrow-deployed.elf and
cache-local-borrow-loader.efi. Loader MD5 remains
d564ca8f8057eabdb36a09db2c1e34d5; disk signatures and ESP size matched.
The resident dump's CPUID census checks failed and must not be used as
valid census data; the raw report retains those failures explicitly.

Matched server PEs for the RPC images subsequently passed the same
resident-byte checks described above. rpcepmap.dll has 46,556 resident
.text bytes compared and zero resident .pdata bytes; its unwind metadata
comes from the validated server PE. rpcss.dll has 929,792 .text and 34,452
.pdata bytes compared. Runtime fothk differences are preserved from RAM.
