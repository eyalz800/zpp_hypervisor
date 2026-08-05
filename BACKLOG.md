# Known defects

Everything here was found by reading the tree or by measuring a running
system, and each entry says which. That distinction is the point of the
document: a defect that has been *seen* justifies a fix on its own, and a
defect that has only been *read* needs its trigger established before the
fix can be trusted.

Recorded at `7174405`, the build deployed for the bare-metal Windows test.
The four fixes already in that build — the hypervisor-present bit cleared,
the MTRR array bounded, `INVD` no longer executed, the first MWAIT logged —
are not repeated here.

The convention for closing an entry: state what was observed afterwards, not
that the code changed. "Guest now reads `cr4=0x0668`" closes one of these;
"masked CR4" does not.

## A hang after the chainload line: what it is, and what it is not

Read this before investigating one, because it cost a day.

**The variable is whether Windows enters its recovery flow.** After a clean
Windows shutdown the loader chainloads and Windows boots - from the boot
order and from the setup menu's boot override alike. Once Windows has
recorded a failed boot, the next attempt takes the recovery path and stops
after the `chainloading` trace line with nothing further drawn: no Windows
output, no bugcheck, no crash dump. Enter sometimes gets through it,
because the `0xc0000001` recovery prompt is *there and waiting for a
keypress*; whether it is drawn is a separate matter from whether it is
running.

Two wrong conclusions were reached and abandoned along the way, both of
them from too few samples of a state that alternates on Windows' own
boot-failure record:

- That it was intermittent. It is not; it is state-dependent.
- That it was deterministic in the launch method - boot override broken,
  boot order fine. It is not. Both work from a clean shutdown and both fail
  from a recovery state. The launch method is now recorded in the trace
  (`BootCurrent`, and its absence) so no future log can be read without
  knowing it, which is the mistake that made this take so long.

Because Windows records a failed boot when it bugchecks, a live bugcheck
keeps re-arming the recovery flow, and the recovery flow then looks like
the defect. Check for a fresh crash dump before believing anything else.

Three things that produce this exact symptom, all worth eliminating before
reaching for a new theory:

- **A `ZPP_VERIFY_HYPERVISOR` build.** It halts every application processor
  in real mode and leaves the boot processor's APIC in x2APIC mode where the
  firmware's xAPIC accesses cannot see it. Silent stop after `chainloading`,
  every time. It must never be deployed to a machine meant to keep booting,
  and note the trap: `-DZPP_VERIFY_HYPERVISOR=ON` persists in the CMake
  cache, so a later plain `cmake --build` inherits it silently. Check the
  built binary for the self-check's strings rather than trusting the build
  command.
- **The recovery flow above.**
- **Item 13 below**, which is on the one path the recovery flow takes and
  the fast path does not.

**Hibernation is disabled on the development target**, measured from the
volume itself: no `hiberfil.sys` at the root, while `pagefile.sys` and
`swapfile.sys` are both present. So fast startup never writes an image
there, a clean shutdown is a real shutdown rather than a resume, and no
boot of that machine has ever been a hibernation resume.

That retires a theory rather than supporting one. The S4 mechanism - a
resume restoring a memory image over frames this VMM had taken and hidden
in EPT - cannot have been happening on this machine, so it explains none of
what was observed here. Item 14 remains a real defect against the
requirement it cites, and it is worth keeping for machines where fast
startup is on, but the improvement seen after fixing it was most likely the
recovery state being cleared by the clean shutdown that preceded it, not
the fix. Two variables, one boot, again.

**The bugcheck has not recurred.** `\WINDOWS\Minidump` holds three dumps
from 20:41, 20:49 and 20:51, and `MEMORY.DMP` from 20:51 - all of them
before the build that clears the hypervisor-present bit was deployed at
22:51. Several Windows boots between then and 00:25 produced no new dump.
Tentative rather than settled: how long Windows was left idle on those
boots is not recorded, and the idle path is what provoked it.

With bugchecks gone and hibernation absent, the loop sustains itself
without either: a boot that hangs is a failed boot, a failed boot arms the
recovery flow, and the recovery flow hangs. A clean shutdown is what breaks
it.

Ruled out by measurement, so that nobody spends a boot on them again: the
module's load address (`0x8916x000` in six logs across four builds, moving
only by the page the loader itself grew, and `start up memory` at
`0x9b000` without exception); the firmware's own view of the processors
(all eight `enabled healthy`, identical before this loader runs and
immediately before the hand-over); and the zpp boot option itself
(`LOAD_OPTION_ACTIVE`, same attribute word as Windows Boot Manager's, same
GPT partition signature, no `LOAD_OPTION_CATEGORY_APP`).

## Measured

These were observed in real state. They are not inferences.

### 1. CR4.VMXE is visible to the guest

The guest reads `cr4 = 0x2668`, VMXE set, while CPUID leaf 1 ECX[5] reports
no VMX. That combination exists nowhere in hardware, and it is exactly the
kind of half-answered interface described under *What the guest is told* in
`CLAUDE.md`: a guest that trusts CR4 over CPUID concludes VMX is available
and takes a `#GP` on its own `vmxon`.

The fix needs CR4's guest/host mask and read shadow, which means a
control-register-access exit (reason 28) has to be handled. A validated
implementation exists on `worktree-agent-a1728a3b15b05a6db` and was not
merged, because it conflicts with `uefi_loader/include/zpp/verify.h` in
three places and its version of that file still *requires* the
hypervisor-present bit that `1d391a3` deliberately clears.

Closing this means the guest reading `0x0668`, and the self-check still
passing with the present bit clear.

### 2. A start-up IPI is swallowed, bare metal only

The sending and receiving sides disagree about which hand-off mechanism is
in use, so the sender consumes a SIPI that the receiver is waiting to
receive from hardware. Reproducing it needs the guest to re-start a
processor this VMM has already adopted, which is why it does not appear
under emulation.

## Structural

Read in the code. Each is a real divergence from what the architecture
requires, but none has been provoked yet — so the first job on each is to
establish the trigger, and only then to fix it.

### 3. `guest_fs_base` is taken from the GDT

`hypervisor/src/hypervisor/hypervisor.cpp:1768` writes
`descriptor.context_dependent_base()`. In long mode the FS base does not
live in the descriptor; it lives in `IA32_FS_BASE`, which is already read
into a member at `:46` and then never used for this. A guest whose FS base
does not fit in a descriptor's 32 bits gets the truncated value.

### 4. `invept` is never called

Declared at `hypervisor/include/zpp/arch/x86_64/vmx/asm.h:122`, zero call
sites. Any change to an EPT entry after launch leaves the old translation
cached in the combined mappings. Today nothing modifies EPT after launch,
which is why this has not bitten — so the fix is as much about making the
invalidation exist for the first modification as about the present state.

### 5. `ia_32e_mode_guest` is set once and never re-derived

Written at `:1731` from the state at launch, cleared at `:1381` for the
real-mode start-up path, and never revisited. A guest that changes
`EFER.LMA` afterwards leaves the VM-entry control stale, and entry
consistency checks tie the two together.

### 6. xAPIC MMIO ICR accesses are not intercepted

Only the x2APIC MSR path reaches the exit handler. A guest in xAPIC mode
writes the ICR through the APIC page and sends IPIs this VMM never sees.

### 7. S3/S4 leaves the machine unvirtualized

There is no resume path, so a suspend takes the hypervisor away and the
guest keeps running on bare hardware without being told.

### 8. Debug and microcode state is unmanaged

No DR7 or `IA32_DEBUGCTL` save/load controls, and MSR `0x79`
(microcode update) is unhandled — it falls into whichever of the two MSR
default paths covers its range rather than being answered deliberately.

### 9. `vmxoff` without `vmclear` on one failure path

`hypervisor.cpp:2139` arms a `scope_exit` that runs `vmxoff` alone. The
path at `:1638` does the same thing correctly, clearing the VMCS first.
Leaving a VMCS current across `vmxoff` leaves it in an implementation-
specific state.

## Concurrency

Found by audit, not by a crash. The comment at
`hypervisor/include/zpp/hypervisor/hypervisor.h:71` is what makes these
worth taking seriously — see item 12.

### 10. Five unsynchronised or unbounded paths

- `start_up_lock` does not cover `next_virtual_processor`.
- The trampoline timeout races the processor it is timing out.
- The slot index and the VPID can diverge, so per-CPU state can be read
  through the wrong index.
- The host-exception recovery slot is shared between processors, so a
  second fault overwrites the first one's record.
- `available_stack_index++` is unbounded; nothing stops it walking past
  the end of the stack array.

## Dead and misleading

These cost nothing at runtime and cost time during every future
investigation, which is the argument for fixing them first rather than
last.

### 11. `launch_error[]` is never written

Declared at `hypervisor/include/zpp/hypervisor/hypervisor.h:709`, read at
`hypervisor.cpp:2404` where its low nibble is folded into the error code
the loader prints. There is no assignment anywhere in the tree, so that
nibble is always zero. A diagnostic that reports a constant is worse than
one that does not exist, because it is trusted.

### 12. Two comments assert a serialisation that no longer holds

`hypervisor.h:71` and `:832` both say the loader launches CPUs strictly one
at a time. That stopped being true, and it is not merely stale prose: the
claim at `:71` is the stated justification for `-fno-threadsafe-statics`
being safe on the function-local static in `hypervisor::instance()`. Either
the serialisation has to be restored or the justification has to be
replaced with a real one.

## Not a defect, but unlanded

The hypervisor's own log does not survive a reboot, so a bare-metal failure
that kills the machine takes its log with it. Work exists on
`worktree-agent-a6c772e3a57c8bba2` (`hypervisor/include/zpp/crash_log.h`).
