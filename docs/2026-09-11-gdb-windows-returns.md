# Direct GDB debugging of nested Windows returns

At the user's request, the September 11 investigation switched from live
sampling to short scripted hardware breakpoints. They work inside Windows
on the current rig. The old BACKLOG claim that QEMU's stub can never expose
VTL0 is superseded by the breakpoint hits below.

## Identity and method

This is the 17:27 UTC boot of 6adda123, loader MD5
d564ca8f8057eabdb36a09db2c1e34d5, with the unchanged two-CPU manifest.
Module base 66d47000 was read from this serial log. Symbols come from
.rig-deployed-hypervisor.elf; singleton offset 15ac000 resolves to 682f3000.
Windows base fffff801d4a00000 and CR3 1ae000 were validated against its
PE timestamp 51a135d9 and image size 1450000. Live bytes at nt+296dd6
match `48 83 c4 20 5b c3`: add rsp,20; pop rbx; ret.

GDB 17.1 (`x86_64-elf-gdb`) connects to QEMU's port 1234. Scripts are
prepared before attaching and detach in finally blocks. Each breakpoint
is deleted before continuing to another address, avoiding GDB's implicit
single-step over an enabled breakpoint. There are no inferior calls,
software breakpoints, `stepi`, register edits, or guest code patches.
Bounded continues use a ten-second interrupt timeout and detach on a
miss; the ten seconds are spent running, not intentionally stopped.

The relevant source check is .references/kvm/nested.c,
nested_vmx_l0_wants_exit: a debug exception with host hardware debugging
active belongs to L0 even when it arose from L2. The measured result is
stronger than relying on a historical claim about which layer is visible.
The separately documented TF/pending-debug single-step failure is not
retested or declared fixed.

## Breakpoint results

At 17:33:08, zpp's on_l2_exit entry at 66db7030 fired on CPU 0 with raw
ABI arguments this=682f3000, cpu=0 and reason=1 (external interrupt).
The saved context supplies nested RIP and GPRs. Its rsp member is the
VMM context address at this point, not the Windows stack pointer; guest
RSP lives in the VMCS. GDB's pre-prologue parameter display is misleading
here, so the actual ABI registers were recorded. The caller backtrace
reaches on_vm_exit and the VM-launch loop. State capture took 3.96 ms.

At 17:35:12, three direct Windows hardware breakpoints fired on CPU 0:

| Stop | Address relative to nt | Evidence |
|---|---:|---|
| MiUnlockPageInline after mov cr8 | 296dd6 | RBX=0; RSP=ffff9086a7206ec0 |
| After add rsp,20 | 296dda | same CPU, RSP increased by exactly 32 |
| Caller, MiGetSystemPage+0xb2 | 3f644e | saved return target, RSP increased by exactly 48 |

The caller comes from the stopped stack at original RSP+28. Its hit and
matching RSP show this invocation executed the return sequence. This is
stronger than a breakpoint before one instruction. It does not prove the
previous slot boot's different invocation returned, nor rule out starvation
elsewhere. The first Windows capture took 1.66 ms; the subsequent reads
were 0.16 and 0.14 ms. These are capture durations, not complete VM stop
latencies. Continue-to-stop round trips were about 8 and 7 ms.

At 17:36:31, nt+21cee5 (MiGetPageFromSlabAllocator+129) fired on CPU 0
in 31 ms. GDB captured 6,816 stack bytes before the unmapped stack boundary,
then detached. The stopped capture took 3.39 ms. Matched PE unwind metadata
recovers MiGetSlabPage -> MiWalkEntireImage -> section/signing validation
-> MmLoadSystemImageEx -> IopLoadDriver -> PnP device-start worker ->
ExpWorkerThread. This is a PnP worker, not the previous boot's Phase1 stack.
The exploratory unwinder initially printed a hardcoded "non-atomic" label;
this capture's index correctly records that the vCPUs were stopped.

A bounded follow-up examined twelve unlock calls, advancing past each
CPU 0 breakpoint before rearming it. All twelve were CPU 0, with matching
stack movement at the following instruction. No CPU 1 hit was obtained.
That result does not establish CPU 1's location or a failure to execute
this function; the experiment simply did not observe that CPU there.

## Artifacts and remaining question

Artifacts are under /tmp/zpp-20260911/, prefixed cache-local-borrow-gdb-:
l2-entry.{json,txt}, kernel.json, windows-return.{json,txt},
image-return/{index.json,stack.bin}, image-return-unwind.txt, and
cpu1-return.{json,txt}. The corresponding breakpoint scripts are
break-l2-exit.gdb, break-windows-return.py, break-capture.py and
break-cpu1-return.py. The three-process list was complete before the
Windows breakpoints; no login was verified.

Use targeted GDB breakpoints to follow the actual blocked path. Short
hardware breakpoint sequences are a viable tool here. Long pauses and
QEMU/KVM single-stepping retain their documented failure modes.

At 17:40 the complete process walk has five entries, including two smss.exe
instances. At 17:41 a separate GDB script armed nt+4f90b0, KeBugCheckEx,
for up to forty minutes of running time. It captures registers and stack
and detaches automatically on hit or timeout. Its directory is
cache-local-borrow-gdb-bugcheck/ and transcript has the same stem with .txt.
The gdb-bugcheck tmux window owns port 1234; local-borrow-logon is the sole
monitor reader and records process/power progress through port 4446. A
breakpoint merely being armed is not evidence of a bugcheck.

The first bugcheck watch was manually interrupted at 17:42:40 to verify
its target against the kernel export table and prologue. The export agrees:
KeBugCheckEx is nt+4f90b0, beginning with saves of RCX/RDX/R8/R9 into the
home slots. That interruption did not hit KeBugCheckEx; its artifacts carry
-bugcheck-interrupted and must not be treated as a guest failure. The watch
was rearmed immediately afterward at the same validated address. The
latest complete watcher read still has five processes and a running guest.
