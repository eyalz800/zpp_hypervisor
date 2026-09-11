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

All 26 rebuilt host tests passed after these code changes. All 219 Python
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

The tested loader was then deployed and verified from a fresh mount:
MD5 `12ad606548c29d04763ec5f07c53863b`. A new two-vCPU boot started around
13:35 UTC on September 11. Channels passed their startup checks, and the
resident log showed Hyper-V entering its nested guest. The boot is still
under observation; recheck live state rather than assuming it is running.

Session artifacts are under `/tmp/zpp-20260911/`, including the baseline
loader, baseline ELF, serial/log captures, counter samples and test output.
The tmux session is `zpp-rig-20260911`; the current observer is `fixed-logon`.
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
