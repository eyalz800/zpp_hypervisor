# hvix64: what issues thousands of MMIO accesses at once, and every way it stops the machine without touching `0xCF9`

Decompiled 2026-09-04 from `.references/hyperv/hvix64.bin`, image base
`0xfffff85675e00000`, **RVA == file offset**. Companion to
`docs/hyperv-ap-timeout-and-reset-paths.md`, which established the reset
family; this file answers the three questions that survived it.

Method as before: `llvm-objcopy -I binary -O elf64-x86-64` to give the blob a
`.data` section at address 0, then `llvm-objdump -D --section=.data`, so the
printed address **is** the RVA. Every RIP-relative target below was recomputed
as `next_insn_rva + disp32` by hand. **Proven** means the bytes are quoted;
*inferred* means it is reasoning.

The measurement this answers, from the rig: MMIO exits are **zero for the
entire run**, then **8,242 in the single second the machine dies**; port I/O
goes from a steady **+10/s** to **+25** in that same second; every other
counter freezes together and the VM ends `paused (shutdown)`. `0xCF9` is armed
in zpp's I/O bitmap and **reads never**.

---

## 0. The answers, in order of confidence

1. **A reset that never writes `0xCF9` is not exotic — it is the normal case.**
   `HvpResetSystem` ends in an **unconditional `out 0x64, 0xFE`** (RVA
   `0x2241a2`), the 8042 pulse-reset, which QEMU honours as a full machine
   reset. Three separate conditions skip the ACPI reset register entirely and
   leave only that write, and a fourth turns it into an **MMIO** write rather
   than a port write. **Port `0x64` is not in zpp's I/O bitmap, so the current
   instrument cannot see the thing most likely to have happened.** (§2)

2. **Nothing in the crash-rendezvous or reset path issues thousands of MMIO
   accesses.** I decoded all of it. `HvpCrashRendezvous` touches only RAM and
   the time source; `HvpResetSystem` performs *at most one* MMIO byte write;
   `HvpQuiesceForReset` performs none. With two LPs the rendezvous completes in
   about as long as the second LP takes to accept one IPI. (§3, §4)

3. **The only shape in hvix64 that produces "silent for the whole boot, then
   thousands in a burst" is a VT-d Global Status poll**, and there are two of
   them. One stalls 10 µs per poll with a 100 s deadline (`0x3571b4`, already in
   the corpus); the other — **new here** — is `0x355878`, a **`pause`-only,
   no-stall** poll whose budget is **700,000,000 spins**, i.e. effectively
   infinite under this nesting. Each iteration is one `mov eax,[regs+0x1C]`.
   The observed 8,242 exits/s implies **121 µs per access**, which is what an
   L2 → L0 → QEMU-userspace MMIO round trip costs here. (§5)

4. **The steady +10/s port I/O is almost certainly hvix64 re-anchoring its
   reference clock**, and that is a free cross-check on everything above. (§6)

---

## 1. What `mmio_exits` counts — this reframes the question

`kvm/x86.c:9237` increments `vcpu->stat.mmio_exits` in exactly one place, in
`x86_emulate_instruction`, on the branch that sets
`vcpu->arch.complete_userspace_io = complete_emulated_mmio`:

```c
	} else if (vcpu->mmio_needed) {
		++vcpu->stat.mmio_exits;
		...
		vcpu->arch.complete_userspace_io = complete_emulated_mmio;
```

So **`mmio_exits` counts only MMIO that leaves the kernel for QEMU.** Anything
`kvm_io_bus_write`/`_read` absorbs in-kernel — the local APIC in xAPIC mode, the
IOAPIC, the PIT — never increments it. In this VM the userspace-MMIO population
is therefore small and nameable:

| region | who emulates it | counted? |
|---|---|---|
| passed-through BARs (NVMe, GPU) | mmap'd by vfio-pci | **no** — direct |
| vfio-pci **MSI-X table pages** | QEMU | yes |
| `intel-iommu` registers at `0xFED90000` | QEMU | yes |
| HPET at `0xFED00000` | QEMU | yes |
| q35 ECAM / MMCFG | QEMU | yes |
| any unassigned GPA | QEMU | yes |
| local APIC `0xFEE00000`, IOAPIC | KVM in-kernel | **no** (see §5.3) |

**Zero MMIO exits for the whole boot is itself a strong reading**: hvix64 and
Windows touched *none* of that population while the guest was running. That
rules out a great deal, and it is what makes the burst diagnostic.

---

## 2. Every way hvix64 stops the machine without writing `0xCF9`

### 2.1 `HvpResetSystem` (`0x2240bc`) — re-read to the byte

The predecessor's decompilation was right; the disassembly adds the exact
branch structure, which is where the answer lives:

```asm
224148  cmpb %dil, [0xa9c18]      ; dil == 0.  g_ResetSuppressed
22414f  jne  224195               ;   != 0  -> SKIP the reset register entirely
224151  cmpb %dil, [0xd42a0]      ; g_ResetRegValid
224158  je   224195               ;   == 0  -> SKIP
22415a  movzbl [0xd42a1], %ecx    ; g_ResetRegSpace  (ACPI GAS AddressSpaceId)
224161  test %ecx, %ecx
224163  je   22417c               ;   == 0  SystemMemory -> MMIO WRITE
224165  cmp  $1, %ecx
224168  jne  224195               ;   not 0 and not 1 -> SKIP
22416a  mov  [0xd42a2], %al       ; value
224170  movzwl [0xd42a8], %edx    ; port
224177  nop
224178  outb %al, %dx             ; <-- the 0xCF9 write, when space == 1
224179  nop
22417a  jmp  22418b
22417c  mov  [0xd42b0], %rcx      ; g_ResetMmioVa
224183  mov  [0xd42a2], %al
224189  movb %al, (%rcx)          ; <-- MMIO reset register write, when space == 0
22418b  mov  $0x1388, %ecx        ; 5000 us
224190  call HvpStallExecutionMicroseconds
224195  nop
224196  mov  $0x64, %edx
22419b  mov  $0xFE, %al
22419d  mov  $0xC350, %ecx        ; 50000 us
2241a2  outb %al, %dx             ; <-- 8042 pulse reset.  UNCONDITIONAL.
2241a3  nop
2241a4  call HvpStallExecutionMicroseconds
2241a9  cli
2241aa  lidtq 0x20(%rsp)          ; a null IDT descriptor, zeroed at 2240c6
2241af  hlt
2241b0  jmp  2241af
```

So there are **four** distinct ways to reach a reset with no `0xCF9` write:

| # | condition | what happens instead |
|---|---|---|
| a | `g_ResetSuppressed (0xa9c18) != 0` | ACPI register skipped; `out 0x64, 0xFE` only |
| b | `g_ResetRegValid (0xd42a0) == 0` | same |
| c | `g_ResetRegSpace (0xd42a1) ∉ {0,1}` | same |
| d | `g_ResetRegSpace == 0` (SystemMemory) | **one MMIO byte write** to `*g_ResetMmioVa`, then `out 0x64, 0xFE` anyway |

and in **all four**, plus the ordinary `0xCF9` case, the 8042 write happens.
**`out 0x64, 0xFE` is on every path through this function.** Under QEMU that
drives the i8042 output-port reset line and requests a machine reset; with
`-no-reboot -no-shutdown` the VM lands in `paused (shutdown)` with memory
intact — which is exactly the observed end state, and it is invisible to an I/O
bitmap that only watches `0xCF9`.

**Snapshot values, read out of the image** (`0xd42a0`, 24 bytes):

```
0xd42a0: 01 01 0f 00 00 00 00 00  f9 0c 00 00 00 00 00 00  00 00 00 00 00 00 00 00
         ^  ^  ^                  ^^^^^                    ^^^^^
         |  |  value 0x0F         port 0x0CF9              g_ResetMmioVa = 0
         |  space 1 = SystemIO
         valid
```

This confirms read #4 of the predecessor's list **for the snapshot image**. If
the rig's guest FADT agrees (QEMU's q35 FADT does specify I/O `0xCF9`/`0x0F`),
then case (d) is out and the live question is (a): **is `g_ResetSuppressed`
set?**

### 2.2 `g_ResetSuppressed` is set from a boot-option query (proven)

The only code that writes `0xa9c18` is inside `HvpPlatformInit` (`0x25802c`,
six callers, all in the init/power family), on its phase-2 branch:

```asm
258160  mov  $0x6, %edx
258166  call 0x247110              ; option query: f(cfg /* rcx, set at 258120 */, 6, &out)
25816b  test %al, %al
25816d  je   258176
25816f  movb $1, [0xa9c18]         ; g_ResetSuppressed = 1
```

`0x247110` is the same helper the reference-clock selector calls with `0x2f`
and `0x2e` (`vp1-hpet-vs-pm.md` §1 calls it a privilege test); it takes the
object in `RCX`, the index in `EDX`, and an out-byte in `R8`. **Which option
index 6 is, I did not identify** — that is a guess I am declining to make. What
is proven is that a single boot-time byte decides whether the ACPI reset
register is written at all.

### 2.3 The tail is a deliberate triple-fault fallback (proven, and it matters)

`cli; lidt(<null>); hlt; jmp $` is the last thing on the path. If the reset does
not take, the LP sits halted with a **null IDT**. Any NMI, MCE or exception
delivered to it then takes a double fault with no handler, then a **triple
fault**. In this stack a triple fault that reaches L0 produces
`KVM_EXIT_SHUTDOWN`, which QEMU turns into a system-reset request — *the same*
`paused (shutdown)` end state, with **no port I/O and no MMIO whatsoever**.

So "every counter froze and the VM is `paused (shutdown)`" is compatible with
at least three mechanisms — the 8042 write, an MMIO reset-register write, and a
triple fault — and only the first two leave a trace an I/O bitmap can catch.

### 2.4 Two other machine-stopping paths worth naming

- `HvpResetSystemDirect` (`0x330990`) is 15 bytes: `call 0x224044; call
  HvpResetSystem; int3`. `0x224044` is **`HvpLeaveVmxOperation`**: it zeroes
  MSR `0x48` (`IA32_SPEC_CTRL`) if enabled, then `vmxoff` if `CR4.VMXE`, then
  clears `CR4.VMXE`. *That `vmxoff` is executed by hvix64 inside zpp's guest* —
  worth knowing which way zpp answers it.
- `HvpResetSystem` also performs `GETSEC[SEXIT]` (leaf 5, with `CR4.SMXE` set
  around it, `0x224127`–`0x224145`) when `g_flags_0xd69e0 & 0x20`, this is the
  BSP and `g_StartedLpCount <= 1`. On a machine with no SMX that faults rather
  than resetting.

---

## 3. `HvpCrashRendezvous` — the fast path, decompiled, and its cost with 2 LPs

`0x21f65c`, 1117 bytes, decoded end to end. The predecessor's C is correct; what
follows is the part that answers "how quickly can it complete, and does it
touch MMIO".

### 3.1 The barrier loop, from the bytes

```asm
; --- top of the wait ---------------------------------------------------
21f999  mov  %r10, [0xa8620]              ; g_CrashedLpBitmap header (count)
21f9a3  mov  %r10, 0x30(%rbp)
21f9b5  mov  %rax, 0xa8628(%rcx,%rdi)     ; copy the crashed-LP bitmap words
21f9bd  mov  0x38(%rbp,%rcx), %rax        ;   to the stack
...
21f9ed  mov  0x148(%rbp,%rax,8), %rcx     ; running-set word
21f9f5  mov  0x38(%rbp,%rax,8), %rax      ; crashed-set word
21f9fa  and  %rcx, %rax
21f9fd  cmp  %rcx, %rax                   ; crashed ⊇ running ?
21fa00  jne  21fa6a                       ;   no -> spin
...
21fab3  call 0x223f90                     ; HvpBugCheckReset -- barrier passed
21fab8  int3
; --- the spin ----------------------------------------------------------
21fa6a  pause
21fa6c  mov  %gs:0x0, %rcx
21fa75  testb $0x8, [0xaf158]             ; g_HvFeatureFlags bit 3
21fa7c  je   21fa99
21fa7e  mov  0x28(%rcx), %r8              ;   set: inline rdtsc * per-LP scale
21fa82  rdtsc
...
21fa99  call 0x2563dc                     ;   clear: HvpGetReferenceTime(per-LP block)
21fa9e  sub  %rbx, %rax
21faa1  cmp  $0xEE6B280, %rax             ; 250,000,000 * 100 ns = 25.0 s
21faa7  jbe  21f999                       ; keep waiting
21faad  call 0x330990                     ; HvpResetSystemDirect
21fab2  int3
```

**`g_HvFeatureFlags` bit 3 is clear** (`0xaf158` reads `0x0040fb2011000002`;
`vp1-hpet-vs-pm.md` §1 additionally proves no code in the image ever sets that
bit). So the spin takes the **`call HvpGetReferenceTime`** arm, once per
iteration, with **no stall between iterations**.

### 3.2 What that call costs, and why it is still not MMIO

`HvpGetReferenceTime` (`0x2563dc`) has two arms, gated on the byte at
`0xd6d10`:

```asm
2563e2  cmpb $0, [0xd6d10]
2563e9  je   25643c                       ; ZERO -> the TSC arm
2563eb  mov  %rbx, [0x232e0]              ; else: the selected clock-source object
2563f5  call 0x21e4e4
2563fd  mov  0x70(%rbx), %rax
256401  call *%rax                        ;   read the platform counter (device access)
256406  cmpq $0x989680, 0xc0(%rbx)        ;   frequency == 10,000,000 ?
...
; --- the TSC arm ---
25643c  mov  0x28(%rcx), %r8              ; per-LP scale
256440  rdtsc
25644c  sub  0x10(%rcx), %rdx             ; tsc - cached anchor
256450  cmp  $0xBEBC200, %rdx             ; 200,000,000 ticks (~100 ms at 2 GHz)
256457  ja   25646c                       ;   stale -> re-anchor
256459  ... mul; add 0x18(%rcx); store 0x20(%rcx)   ; cheap: pure arithmetic
25646c  mov  %rax, %rdx
25646f  call 0x256518                     ; HvpResyncReferenceTimeAnchor
```

`0xd6d10` is **0** in the snapshot, so the TSC arm is taken, and the *fast*
path of it is arithmetic only. The re-anchor `0x256518` is the one that touches
the device:

```asm
256527  mov  %rdi, [0x232e0]              ; the selected clock source
256537  call 0x21e4e4
25653f  mov  0x70(%rdi), %rax
256543  call *%rax                        ; <-- the only device read on this path
256548  cmpq $0x989680, 0xc0(%rdi)
```

and it fires only when the per-LP anchor is more than 200,000,000 TSC ticks old
— **about ten times a second per LP**, however tight the spin above it is.

`vp1-hpet-vs-pm.md` proves the selected source on this rig is the **ACPI PM
timer (type 7), not the HPET**, so that read is an `in` from a port, not MMIO.

**Conclusion: `HvpCrashRendezvous` generates no MMIO at all, and about ten port
reads per second per spinning LP.** (proven for the code; the PM-timer
selection is quoted from the corpus, not re-derived here)

### 3.3 How fast the barrier completes with 2 LPs

```
first crasher                          second LP
-------------                          ---------
cmpxchg g_FirstCrashedLpId <- self      (running guest code, L2)
bts g_CrashedLpBitmap[self]
for each running LP not yet crashed:
    lock or  ctx[lp]+0x198, 0x80        ; "crash requested"
(*g_pfnSendIpiToProcessorSet)(set,0xff) ------> vector 0xff arrives
spin: crashed ⊇ running ?                       host interrupt -> immediate exit
                                                handler -> HvpCrashRendezvous(1)
                                                bts g_CrashedLpBitmap[self]
      <---------------------------------------- both see coverage
call HvpBugCheckReset                   call HvpBugCheckReset
  BSP: spin while g_StartedLpCount > 1    non-BSP: cli; lidt(null);
                                                   lock dec g_StartedLpCount; hlt
  HvpResetSystem()
```

The only unbounded quantity is **how long the second LP takes to accept one
interrupt**. It is running as an L2 guest under hvix64 under zpp; an external
interrupt targeted at the host causes an immediate VM exit, so this is
microseconds on a healthy machine and at worst a scheduling quantum. **The 25 s
deadline is nowhere near reachable in a healthy 2-LP rendezvous, and neither is
a full second** — unless the second LP is not taking interrupts at all, in
which case the answer is 25 s and not 1 s either.

**So the observed one-second window is not the rendezvous barrier.** Whatever
took that second happened before it or instead of it.

One correction worth carrying: the 25 s check is evaluated **before**
`HvpBugCheckReset` is entered, and `HvpBugCheckReset` itself has no timeout —
its BSP spin (`while g_StartedLpCount > 1`) terminates only because each parking
LP does `lock dec` at `0x223fe7`.

---

## 4. What the reset path does *not* do (all proven, all negative)

Stated explicitly because each of these was a live hypothesis:

- **`HvpStallExecutionMicroseconds` (`0x25bb48`) is `rdtsc` + `pause` only.**
  No port I/O, no MMIO, no timer. So the 5 ms and 50 ms stalls inside
  `HvpResetSystem` contribute *nothing* to either counter. A stall-based
  explanation of the burst is dead.
- **`HvpQuiesceForReset` (`0x2241b8`) issues no MMIO.** It calls
  `[0xd6cf0](0xd0)` and `[0xd6cf0](0xf0)` — task-priority raises — and then, if
  `[0xd6988] & 4` and `[0xd6d18] == 0`, does this:

  ```asm
  224212  mov  $0x1b, %ecx
  224217  rdmsr                      ; IA32_APIC_BASE
  224223  bt   $10, %rax             ; EXTD (x2APIC) set?
  224228  jae  22424e                ;   no -> nothing to do
  22422a  and  $~0xC00, %r8          ; clear EXTD(10) and EN(11)
  22423b  wrmsr
  22423d  bts  $11, %r8              ; set EN(11) again
  22424c  wrmsr                      ; => xAPIC mode
  ```

  **This is the only write of `IA32_APIC_BASE` anywhere in hvix64** — verified
  by scanning the image for `mov ecx, 0x1b` followed by a `wrmsr` within 40
  bytes: three hits, `0xb5bd` (data), `0x224212` (this), `0x3a684d` (the AP
  trampoline, `rdmsr` only). So hvix64 takes the LP **out of x2APIC and back
  into xAPIC** exactly once, on the reset path, immediately before the reset
  writes.
- **`HvpPreparePartitionForReboot` (`0x2dec40`) is a 157-byte bit-scan over a
  32-bit mask** at `partition+0x63b8`, calling `0x2deda4(partition, 0xD0007,
  bit, …)` per set bit — at most 32 iterations, and `0x2deda4` is a
  property-code dispatch (`0xD0004`/`0xD0006`/`0xD0007`/`0xD0008`) that reads
  and writes partition fields. No device access.
- **`HvpCrashRendezvous`, `HvpBugCheckReset`, `HvpResetSystemDirect`** touch
  only RAM plus the two writes in §2.

**There is therefore no bulk-MMIO generator anywhere on hvix64's reset path.**

---

## 5. What *can* issue thousands of MMIO accesses in a burst

### 5.1 The one that fits: a VT-d Global Status poll (proven code, *inferred* attribution)

Two poll loops exist over `GSTS_REG` (`unit->regs + 0x1C`), and neither issues
anything until it is entered:

| RVA | shape | budget |
|---|---|---|
| `0x3571b4` `HvpIommuWaitForGlobalStatus` | poll, then `HvpStallExecutionMicroseconds(10)` | 100,000,000 µs = **100 s** → `HvpBugCheck(0x17)` |
| **`0x355878`** (new) | poll, then `pause`. **No stall at all** | **700,000,000 spins** → `HvpBugCheck(0x13)` |

`0x355878` decoded in full:

```c
// hvix64 RVA 0x355878.  rcx = unit->regs (mapped VT-d register base), edx = GCMD shadow
void HvpIommuDisableTranslationAndIr(volatile u32 *regs, u32 gcmd)
{
    u64 spins = 0;
    gcmd &= 0x96800000;
    if ((int)gcmd < 0) {                       // GCMD.TE (bit 31) set
        gcmd &= ~(1u << 31);
        regs[0x18/4] = gcmd;                   // GCMD_REG: clear Translation Enable
        while ((int)regs[0x1C/4] < 0) {        // GSTS_REG.TES still set
            __pause();                         // <<< one MMIO read per pause
            if (++spins > 700000000) { HvpBugCheck(0x13, …); __debugbreak(); }
        }
    }
    if (gcmd & (1u << 25)) {                   // GCMD.IRE
        gcmd &= ~(1u << 25);
        regs[0x18/4] = gcmd;
        while (regs[0x1C/4] & (1u << 25)) {    // GSTS_REG.IRES still set
            __pause();
            if (++spins > 700000000) { HvpBugCheck(0x13, …); __debugbreak(); }
        }
    }
}
```

Byte confirmations: `lea 0x1c(%rcx), %r9` at `0x35587f` (the GSTS address held
for the loop), `mov %edx, 0x18(%rcx)` at `0x355898` (the GCMD write),
`mov (%r9), %eax` at `0x35589b` inside the loop, `mov $0x29b92700, %r11d` at
`0x355883` (700,000,000), `call 0x21f21c` (`HvpBugCheck`) with `lea 0x13(%rdx),
%ecx` at `0x3558c7` (code `0x13`).

**Why this is the candidate.** It is the only construct I found in hvix64 that
(a) issues *no* device access at all until entered, (b) then issues one per
iteration with nothing throttling it, and (c) has a budget that is a **spin
count, not a wall-clock deadline** — 700 million iterations at 121 µs each is
23,000 hours, so it never self-terminates on this rig.

**The arithmetic fits and is worth stating**: 8,242 exits in one second is
**121 µs per access**, which is the right order for an L2 → L0 → QEMU-userspace
round trip in this four-level stack. The 10 µs-stalled loop `0x3571b4` produces
the same rate for the same reason (the exit dominates the stall) — the two are
not distinguishable by rate alone, only by which RIP is spinning.

**Where these are called from** (proven by scanning every `E8`/`E9` `rel32`):

```
0x355878  <- 0x358818 <- 0x30a480  ┐
0x355878  <- 0x3598a8 <- 0x30a4e5  ┴ both inside 0x30a278
0x355714  <- 0x3589d0 <- 0x30a306 (a per-unit list walk inside 0x30a278)
0x30a278  <- 0x248fd4, and 0x248150 <- 0x3a667d
```

`0x3a667d` is inside hvix64's **entry stub** — `0x3a6640` sets
`KERNEL_GS_BASE`, takes `RSP` from the loader block (`[0xa24c0]+0x2390+0x4fe0`),
then `call 0x3a6560; call 0x232260; call 0x248150; int3`. So `0x248150` is the
BSP's `HvpMain`, and **`0x30a278` is IOMMU bring-up, not teardown** — it walks
the unit list at `0xb1e70` disabling translation/IR on each (`0x3589d0` →
`HvpIommuDisableAndMaskUnit` at `0x355714`), then re-programs, then walks a
device list at `0xb1f60` calling `0x357470` per device and a second list at
`0xb1f80` calling `0x314550`.

**That is the tension in this hypothesis and I am not going to paper over it.**
Bring-up runs at BSP init, seconds into the boot — not at 100 s. For the
IOMMU to be the burst, one of these must hold, and each is checkable:

- the burst is *not* bring-up but a later re-program (`HvpIommuSetGlobalCommand`
  `0x356f48` has **nine** call sites; `HvpIommuSubmitGlobalCommand` `0x357244`
  has two), e.g. driven by a guest hypercall or a VTL/secure-DMA transition;
- or the whole boot's IOMMU traffic was invisible to `mmio_exits` because zpp
  absorbed it, and the burst is the first traffic that got past zpp.

That second one has a name in this tree. `hypervisor.h` documents
`dmar_register_page` as **full-trapped in the L1 EPT**, with `dmar_mmio`,
`dmar_register_read/write`, and counters `dmar_reads[]`, `dmar_writes[]`,
`dmar_qi_descriptors[]`, `dmar_qi_waits_completed[]` — all behind
`nested_vmx::nested_vtd`, whose manifest field is **`nvtd=`**.

> **Read `nvtd=` out of `zpp switches:` before doing anything else with this
> section.** With `nvtd=1` zpp emulates the VT-d register page itself and
> hvix64's polls never reach QEMU — which explains zero `mmio_exits` for a boot
> in which hvix64 certainly *did* program the unit — and the burst then means
> something reached QEMU that previously did not. With `nvtd=0` the accesses go
> straight through to QEMU's `intel-iommu` model, and zero `mmio_exits` for the
> whole boot instead means hvix64 found **no DMAR unit at all**, which makes the
> IOMMU a poor candidate for the burst and promotes §5.2.
>
> Either way, `dmar_reads[]`/`dmar_writes[]` are already in the binary and have
> apparently never been printed for this failure. That is the cheapest
> discriminator available and it needs no new code.

### 5.2 If it is not the IOMMU, it is not hvix64's reset path

Having decoded that path exhaustively (§4), the remaining userspace-MMIO
populations from §1 point away from hvix64:

- **vfio-pci MSI-X table pages.** QEMU traps them. Windows re-programming or
  mass-masking MSI-X vectors at bugcheck time lands here. Silent during steady
  state, bursty at teardown — the right *shape*, but the count would have to be
  thousands of vector writes, which is more vectors than this rig plausibly has.
- **q35 ECAM.** A full 4 KB config-space read of 8 functions is 8,192 dword
  accesses, and **8,242 = 8,192 + 50** is close enough to be worth one look —
  a PCI enumeration or config-space save is the classic generator of exactly
  that number. *This is numerology until somebody reads the exit's GPA*, and I
  am flagging it as such, but the GPA is one field in an existing exit record.
- **An unassigned GPA touched in a loop** — reads return all-ones, which is what
  makes a driver decide a device vanished.

None of those three is hvix64 code, which is why I am naming them and stopping
rather than guessing at RVAs for them.

### 5.3 The xAPIC flip: a real mechanism, probably not this one

Because `HvpQuiesceForReset` puts the LP back into xAPIC mode (§4), every
subsequent local-APIC access is MMIO at `0xFEE00000` rather than a WRMSR. KVM
normally absorbs those in-kernel — but `kvm/lapic.c:1709` and `:2427` show the
gate:

```c
	if (!kvm_apic_hw_enabled(apic) || apic_x2apic_mode(apic)) {
		if (!kvm_check_has_quirk(vcpu->kvm, KVM_X86_QUIRK_LAPIC_MMIO_HOLE))
			return -EOPNOTSUPP;
		memset(data, 0xff, len);        /* reads */
		return 0;
	}
```

`-EOPNOTSUPP` from the io_bus is what sends an access to userspace. So if KVM's
view of the vCPU's APIC is x2APIC while the guest issues xAPIC MMIO, and the
`LAPIC_MMIO_HOLE` quirk is disabled, those accesses **do** become `mmio_exits`.
That is a genuine way to manufacture MMIO exits out of nothing — but only a
handful of them, on the reset path, after the machine is already dying. I record
it because it inverts cleanly: **a large late MMIO burst at `0xFEE00000` would
mean the two ends disagree about APIC mode**, which is a zpp-side bug, not an
hvix64 one.

---

## 6. The port-I/O arithmetic, which is a free cross-check

Two numbers in the same measurement, and both have candidate explanations in
the bytes. Neither is proven; both are cheap to test.

**The +10/s baseline.** `HvpGetReferenceTime`'s TSC arm re-anchors through
`HvpResyncReferenceTimeAnchor` (`0x256518`) whenever the per-LP anchor is more
than **200,000,000 TSC ticks** stale (`cmp $0xBEBC200` at `0x256450`) — about
100 ms at 2 GHz, i.e. **~10 device reads per second**, and on this rig that
device is the ACPI PM timer, reached by an `in`. A steady 10 port accesses per
second is exactly that. *If this is right it is a useful fingerprint: the
baseline port I/O of a settled guest is hvix64's clock, and a change in it is a
change in how often hvix64 asks the time.*

**The +15 in the death second.** Two things on the stop path produce port
writes, and they add up:

- `HvpLpPowerEntry` (`0x247b20`) calls `0x258820` at `0x247d93`, which
  re-initialises **both 8259 PICs with exactly ten `outb`s** —
  `0x20 <- 0x11`; `0x21 <- 0xD8, 0x04, 0x01, 0xFF`; `0xA0 <- 0x11`;
  `0xA1 <- 0xD8, 0x02, 0x01, 0xFF` (RVAs `0x2588a3`…`0x2588df`, each bracketed
  by `nop`). The identical sequence appears once more at
  `0x258123`…`0x25815f` inside `HvpPlatformInit`.
- `HvpResetSystem` contributes one or two (`0xCF9` if it fires, and `0x64`).

**10 + 1 = 11, and 10 + 2 = 12, against an observed excess of ~15.** That is a
falsifiable prediction rather than a claim: if the extra port I/O in the death
second is ten writes to `0x20/0x21/0xA0/0xA1` plus one to `0x64`, the machine
went through `HvpLpPowerEntry` and `HvpResetSystem`, and the whole story is a
**power/resume transition**, not a crash.

---

## 7. What to instrument next, cheapest first

| # | do this | it settles |
|---|---|---|
| **1** | **Arm ports `0x64` and `0x60` in zpp's I/O bitmap** (and `0x92`, the fast-A20/reset port, for completeness) | whether `HvpResetSystem` ran at all. `0xCF9` alone cannot see it, and `out 0x64,0xFE` is on **every** path through that function (§2.1) |
| **2** | **Arm `0x20/0x21/0xA0/0xA1`** | ten writes in the death second means `HvpLpPowerEntry` ran — i.e. a *power transition*, which also unlocks `HvpRestartAllLogicalProcessors` and its resetting 4 s wait (predecessor §2.2) |
| **3** | **Read `nvtd=` from `zpp switches:`, and print `dmar_reads[]`/`dmar_writes[]`** | whether hvix64's VT-d traffic is being absorbed by zpp or reaching QEMU — the single fork §5.1 rests on. Both already exist in the binary |
| **4** | **Record the GPA of the MMIO exits** (the burst's addresses, not just the count) | names the region outright: `0xFED90000` = VT-d, `0xFEE00000` = the APIC-mode disagreement of §5.3, an ECAM address = a PCI sweep, a BAR address = an MSI-X table. This is one field and it ends the guessing |
| 5 | `hvix64 + 0x235e8` (`g_FirstCrashedLpId`) and `+0xa8628` | still the decisive pair from the predecessor's list, still never read |
| 6 | `hvix64 + 0xa9c18` (`g_ResetSuppressed`) | if `1`, `0xCF9` was *never going to be written* and reads #1/#4 stop being puzzling (§2.2) |

Read #4 is the one I would do first if only one were possible. Every hypothesis
in §5 is a claim about *which physical address* the burst hit, and the exit
record already carries it.

---

## 8. Names established this round

| RVA | name | evidence |
|---|---|---|
| `0x355878` | **`HvpIommuDisableTranslationAndIr(regs, gcmd)`** | writes `GCMD_REG` clearing TE then IRE, `pause`-only polls `GSTS_REG`, 700,000,000-spin budget → `HvpBugCheck(0x13)` |
| `0x355714` | **`HvpIommuDisableAndMaskUnit(unit)`** | clears TE/IRE/QIE via `HvpIommuSetGlobalCommand`, then masks the fault, invalidation and page-request event controls — `regs+0x38` (FECTL), `+0xa0` (IECTL), `+0x9c` (ICS, write-1-to-clear), `+0xe0` (PECTL), `+0xdc` (PRS) — then invalidates the IRT in memory (`unit+0x98`, `unit+0x90` entries, 16 bytes each), then `regs+0x34 = 0x7d` |
| `0x30a278` | **`HvpIommuConfigureAllUnits`** | walks the unit list at `0xb1e70` (`0x3589d0`, `0x358a8c`), the device list at `0xb1f60` (`0x357470`), a third list at `0xb1f80` (`0x314550`); single caller `0x248fd4` |
| `0x248150` | **`HvpMain`** | the third and last call of the entry stub, followed by `int3`; stack comes from the loader block |
| `0x3a6640` | **`HvpBspEntry`** (tail) | `wrmsr(KERNEL_GS_BASE, base+0x30000)`, `RSP = [0xa24c0]+0x2390+0x4fe0`, then `0x3a6560`, `0x232260`, `HvpMain`, `int3` |
| `0x224044` | **`HvpLeaveVmxOperation`** | `wrmsr(0x48, 0)` if enabled, `vmxoff` if `CR4.VMXE`, clear `CR4.VMXE` |
| `0x2241b8` | `HvpQuiesceForReset` (confirmed) | TPR raises to `0xd0`/`0xf0`; the **only** `IA32_APIC_BASE` write in the image, x2APIC → xAPIC |
| `0x256518` | **`HvpResyncReferenceTimeAnchor(perLp, tsc)`** | reads the selected clock source via `[0x232e0]->+0x70`, recomputes the per-LP TSC scale |
| `0x25802c` | **`HvpPlatformInit(phase)`** | phase dispatch; phase 2 does the 8259 init and sets `g_ResetSuppressed` from option 6 |
| `0x258820` | **`HvpProgramPics`** | the ten-`outb` 8259 sequence, called from `HvpLpPowerEntry+0x273` |
| `0x2deda4` | **`HvpPartitionPropertyDispatch(partition, code, vtl, …)`** | codes `0xD0004`/`0xD0006`/`0xD0007`/`0xD0008` |

### Globals

| RVA | name | snapshot value |
|---|---|---|
| `0xd42a0` | `g_ResetRegValid` | `1` |
| `0xd42a1` | `g_ResetRegSpace` | `1` (SystemIO) |
| `0xd42a2` | `g_ResetValue` | `0x0F` |
| `0xd42a8` | `g_ResetPort` | `0x0CF9` |
| `0xd42b0` | `g_ResetMmioVa` | `0` |
| `0xa9c18` | `g_ResetSuppressed` | `0` on disk; set at runtime from option 6 |
| `0xaf158` | `g_HvFeatureFlags` | `0x0040fb2011000002` — **bit 3 clear**, so the crash-rendezvous spin calls `HvpGetReferenceTime` rather than using inline `rdtsc` |
| `0xd6d10` | reference-time source selector | `0` → the TSC arm of `HvpGetReferenceTime` |
| `0x232e0` | `g_ReferenceClockSource` | a heap pointer; `+0x70` is the read method, `+0xc0` the frequency, `+0x128`/`+0x130` the scale |
| `0xb1e70` | IOMMU unit list head | |
| `0xb1f60`, `0xb1f80` | IOMMU device lists (stride via `+0xe0`, `+0x10`) | |

---

## 9. Corrections and additions to the corpus

To `docs/hyperv-ap-timeout-and-reset-paths.md`:

1. §3's `HvpResetSystem` transcription is right, but the **`out 0x64, 0xFE` at
   `0x2241a2` is unconditional** and that is the load-bearing fact, not the
   `0xCF9` write. The document's rig conclusion — "that `out` is an
   unconditional I/O instruction … so the write becomes a machine reset" —
   pointed at `0x224178`, which is the *conditional* one. Swap them.
2. Add the four skip conditions of §2.1 to read #4 of the post-mortem list:
   `{1,1,0x0F,0x0CF9}` at `0xd42a0` is necessary but **not** sufficient for a
   `0xCF9` write, because `g_ResetSuppressed` is checked first.
3. The 25 s deadline in `HvpCrashRendezvous` is evaluated with
   `HvpGetReferenceTime`, not `rdtsc`, because `g_HvFeatureFlags` bit 3 is clear
   — so the deadline inherits whatever the reference clock does.

To `.references/hyperv/hvix64-iommu-gsts-timeout.md`:

4. There is a **second** GSTS poll, `0x355878`, and it is worse than the one
   that note documents: **no stall**, and its budget is a *spin count*
   (700,000,000) rather than 100 s of wall clock, so on a machine where each
   poll costs ~121 µs it will never time out. If the rig ever spins there,
   nothing in hvix64 will end it.
5. `HvpIommuDisableAndMaskUnit` (`0x355714`) gives the rest of the VT-d
   register map hvix64 uses: `0x18` GCMD, `0x1C` GSTS, `0x34`, `0x38` FECTL,
   `0x9C` ICS, `0xA0` IECTL, `0xDC` PRS, `0xE0` PECTL.

To `.references/hyperv/vp1-hpet-vs-pm.md`:

6. That note's `g_TscFreqMsrAvailable` at `0xd6d10` is better named for what it
   *selects*: **zero takes the `rdtsc`-plus-per-LP-scale arm of
   `HvpGetReferenceTime`, non-zero takes the clock-source-object arm.** The
   name reads as if the polarity were the other way round.

---

## 10. Method notes

- **`llvm-objdump` cannot disassemble a raw blob** on this toolchain (`-b` is
  not accepted). Wrap it once — `llvm-objcopy -I binary -O elf64-x86-64` puts
  the whole file in a `.data` section at address 0, so `--start-address` takes
  the RVA directly and the printed address *is* the RVA. Do not trust the `#`
  comments; recompute `next_insn_rva + disp32`.
- **Scanning for a nop-bracketed opcode finds I/O that no decompilation shows.**
  hvix64 pads every `in`/`out` with `0x90` on at least one side, which makes
  `E4`–`E7`/`EC`–`EF` scannable with few false positives. 167 hits over the
  image; the two ten-`outb` PIC sequences and the two reset `out`s fell straight
  out, and one apparent `inl` at `0x24f7d9` turned out to be the middle of a
  `cmpb …(%rip)` — **check every hit by disassembling around it**, which is how
  that one was caught.
- **Scanning for `mov ecx, <imm32>` followed by `rdmsr`/`wrmsr` within 40 bytes**
  answers "where is this MSR touched" in one command. For `0x1b` it gave three
  hits and proved the x2APIC → xAPIC flip is unique to the reset path — a
  negative that would have taken a long time to establish by reading.
- **A call-site scan settles reachability that reading cannot.** `0x30a278`
  looked like an IOMMU *teardown* from its contents and is reached only from
  `HvpMain`; that inverted the conclusion, and it took one scan rather than an
  argument.
- **`mmio_exits` is not "MMIO".** It is "MMIO that left the kernel". Half the
  candidate list for this burst evaporates once that is read out of `x86.c`
  rather than assumed, and the other half becomes nameable. Read the counter's
  definition before reasoning from its value.
