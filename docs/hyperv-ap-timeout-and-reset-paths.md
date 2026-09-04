# hvix64: the AP start-up timeouts, and every path that deliberately resets the machine

Decompiled 2026-09-04 from `.references/hyperv/hvix64.bin`, image base
`0xfffff85675e00000`, **RVA == file offset** (re-verified: the strings at
`0xcfe8` / `0xd010` / `0xb423` read back at those file offsets).

This file is tracked on purpose. `.references/` is gitignored, so the
decompilation corpus does not survive a clone; this is the readable digest of
it. The companion notes in `.references/hyperv/` — `hvix64-reset-path-and-
crash-record.md`, `hvix64-add-logical-processor-decompiled.md`,
`hvix64-lp-trampoline-emit-decompiled.md` — are corrected in §7 below.

Everything marked **proven** was disassembled from the bytes with
`llvm-objdump` over a raw slice, and every RIP-relative target was recomputed
by hand (`next_insn_rva + disp32`) rather than read off objdump's `#` comment,
which is meaningless for a raw blob and produced a wrong global address twice
before this rule was adopted. Anything marked *inferred* is reasoning, not
bytes.

---

## 0. The answer in one paragraph

Hyper-V has **two** bounded four-second waits for an application processor,
and they behave completely differently on timeout. The one behind hypercall
`0x76` returns a status to Windows and tears the LP down — it does not reset
anything. The other one, `HvpRestartAllLogicalProcessors`, **resets the
machine**. Separately, and much more likely to be what the rig is showing,
hvix64's VM-exit dispatcher treats three exit reasons as fatal to the whole
system: **INIT (basic reason 3) reboots unconditionally**, triple fault
(reason 2) and any VM-entry failure (bit 31 set) reach `HvpVpFatal` /
`HvpVpShutdown`, and for a **root** virtual processor both of those end in a
reset. All of it funnels through one function, `HvpCrashRendezvous` at
`0x21f65c`, which ends in `out 0xCF9, 0x0F` — the ACPI reset register — which
is exactly what makes a QEMU guest read `paused (shutdown)` with every KVM
counter frozen at the same instant.

---

## 1. Names established, with RVAs

### The reset core

| RVA | name | what it is |
|---|---|---|
| `0x21f65c` | **`HvpCrashRendezvous(bool skip_quiesce)`** | the funnel. Latches the first crasher, IPIs the others with vector `0xff`, waits **25 s** for them, then resets. Never returns |
| `0x223f90` | **`HvpBugCheckReset`** | rendezvous completed: non-BSP LPs park, the BSP waits for them, then `HvpResetSystem` |
| `0x330990` | **`HvpResetSystemDirect`** | rendezvous **timed out**: 15 bytes, straight to `HvpResetSystem` |
| `0x2240bc` | **`HvpResetSystem`** | ACPI reset register → 5 ms → `out 0x64, 0xFE` → 50 ms → `cli; lidt(null); hlt` |
| `0x21f21c` | `HvpBugCheck(code, p1..p5)` | records to `0x112660`… then calls `HvpCrashRendezvous` |

### The fatal-exit family

| RVA | name | trigger |
|---|---|---|
| `0x35e1b0` | `hv_dispatch_vmexit` | already named in the corpus. Handles the hot reasons inline, falls through for the rest |
| `0x35d3e0` | **`HvpHandleRareVmExit(vp, u32 exit_reason)`** | the tail of the dispatch. Contains all three fatal cases |
| `0x2c9524` | **`HvpRebootOnInit`** | exit reason **3 (INIT)**. 32 bytes. Prints and reboots. **Unconditional** |
| `0x2c7da4` | **`HvpHandleTripleFault(vp)`** | exit reason **2**, and exit reason `0x25` |
| `0x2ca574` | **`HvpVpShutdown(vp)`** | deliver `HvMessageTypeUnrecoverableException` upward, or reboot |
| `0x2c53e4` | **`HvpVpFatal(vp)`** | root VP → reboot; otherwise shut that VP down |
| `0x35b9a0` | **`HvpHandleVmEntryFailure(vp)`** | reflect to the level above, or `HvpVpFatal` |
| `0x35c08c` | **`HvpBugCheckVmEntryFailure(u16)`** | `HvpBugCheck(6, 4, …)` |
| `0x232ae0` | **`HvpHandleHostException(code)`** | every host IDT stub at `0x3a8000`+ (0x380 apart) calls it. Code `0x1008` → rendezvous; otherwise `HvpBugCheck(0x11)` |
| `0x23540c` | `HvpHandleMachineCheck` | the two "…rebooting" machine-check strings |
| `0x333280` | `HvpMinimalLoopExit` | already named. Six "Rebooting the system" strings, one of them `VMX_EXIT_REASON_INIT_INTR` |

### The AP-start family

| RVA | name | note |
|---|---|---|
| `0x23e1f0` | **`HvpRestartAllLogicalProcessors(bool do_restart)`** | **new**. The second 4 s wait — the one that reboots |
| `0x247b20` | **`HvpLpPowerEntry(u32 phase)`** | **new**. The AP body on the resume/power path; `HvpCrashRendezvous(1)` on any failure |
| `0x3a6690` | `HvpLpLongModeEntry` | dispatches to `0x247fe0` or `0x247b20` — §5 |
| `0x23a800` | `HvpAllocLpSlot` | the `LP_START_RECORD` field layout falls out of its stores — §4 |

### Globals worth reading post-mortem

| RVA | name | value in the image | why |
|---|---|---|---|
| **`0x235e8`** | **`g_FirstCrashedLpId`** (u32) | **`0xffffffff`** (verified) | `!= -1` ⇒ hvix64 went through `HvpCrashRendezvous`, **and it names which LP got there first** |
| `0xa8628` | `g_CrashedLpBitmap` | 0 | one bit per LP, set on entry to the rendezvous |
| `0xa8500` | `g_RunningLpSet` (HV_CPU_SET header) | `{0x20, 1, 1}` | what the rendezvous waits for |
| `0xd6d48` | `g_pfnSendIpiToProcessorSet` | 0 (bound at boot) | called `(set, 0xff)` |
| `0xa8610` | `HvBugCheckOccurred` | 0 | **`0` does not mean "no reset"** — the INIT and VP-shutdown paths never touch it |
| `0x112660`…`0x112688` | bugcheck code + 5 parameters | 0 | |
| `0xd4280` | `g_StartedLpCount` | | each parking LP decrements it in `HvpBugCheckReset` |
| `0xd6bd4` | `g_BspLpId` | | |
| `0xa3d34` | `g_LpStartMode` | 0 | `== 1` disables **both** 4 s deadlines |
| `0x1126a0` | `g_LpStartRecord[]`, stride `0x20` | `[0] = {State 4, Started 1}` | |
| `0xa3f90` | `g_LpContextTable[]`, 8-byte entries | | indexed by `rec[lp].+0x18` |
| `0xfe049`, `0xfe078` | AP entry-path selectors | `0`, `0` | choose `0x247fe0` vs `0x247b20` |
| `0xd42a0`…`0xd42b0` | reset-register descriptor | | `{valid, space, value, port, mmio_va}` |

---

## 2. The two four-second waits

There are exactly four sites in the image with the immediate `0x3d0900`
(4,000,000), found by scanning the raw bytes rather than grepping a
decompilation:

| site | function | what it is |
|---|---|---|
| `0x2394b0` | `HvpAddLogicalProcessorWorker` | AP start deadline → **status** |
| `0x23e2c0` | `HvpRestartAllLogicalProcessors` | AP restart deadline → **reset** |
| `0x28ccea` | `HvCall_0x06a` | a caller-supplied timeout, clamped to 4 s. Unrelated |
| `0x31d8b7` | `0x31d724` | a structure-field initialiser. Unrelated |

### 2.1 Wait A — hypercall `0x76`. Returns a status. (proven)

```
0x2394a0  mov  rbx, r9                  ; rbx = elapsed microseconds = 0
0x2394a3  lea  r12, [rip-0x2394aa]      ; r12 = image base
0x2394aa  mov  r14d, 0x2710             ; 10,000 us
0x2394b0  cmp  rbx, 0x3d0900            ; 4,000,000 us
0x2394b7  jb   0x2394c2
0x2394b9  cmp  dword [rip-0x19578c], 1  ; g_LpStartMode (RVA 0xa3d34)
0x2394c0  jne  0x2394dc                 ; -> timeout
0x2394c2  mov  eax, [rdi + r12 + 0x1126a0]   ; LpStartRecord[lp].State
0x2394ca  cmp  eax, 1
0x2394cd  jne  0x2394f5                 ; left state 1 -> proceed
0x2394cf  mov  rcx, r14
0x2394d2  call 0x25bb48                 ; HvpStallExecutionMicroseconds(10000)
0x2394d7  add  rbx, r14
0x2394da  jmp  0x2394b0
0x2394dc  mov  ebx, 0x3e                ; status
0x2394e1  mov  r14d, [rbp-0x55]
0x2394e5  mov  r15d, 0x338              ; stage
```

`0x3e` is `HV_STATUS_PROCESSOR_STARTUP_TIMEOUT` in the TLFS status list — *name
recalled, not looked up in a document, but the site is literally an AP start-up
timeout, so context corroborates it.*

**On timeout this path does not reset anything.** It falls into the worker's
common failure tail: when the LP was not being reused it calls `HvpStopLp`,
stalls 10 ms, writes `rec->State = 0` and `rec->Started = 0`, frees the LP
block, and stores the `u16` status into the hypercall's output buffer. Windows
gets `0x3e` back from `HvCallAddLogicalProcessor` and decides what to do.

**Consequence for reading a hung guest**: if `lp_state[1] == 1` post-mortem,
**wait A has not fired**, because its own teardown would have written
`State = 0`. `State == 1` therefore means the worker is still inside the wait,
or the machine stopped before four seconds elapsed. It is *evidence against*
the "Hyper-V gave up on the AP and did something drastic" story, not for it.

### 2.2 Wait B — `HvpRestartAllLogicalProcessors`. Resets the machine. (proven)

`0x23e1f0`, 276 bytes, decoded in full:

```c
// hvix64 RVA 0x23e1f0
void HvpRestartAllLogicalProcessors(bool do_restart)
{
    if (g_StartedLpCount /* 0xa7fa0 */ > 1)
        HvpUpdateCpuSet(&g_CpuSet_0xea200, 0xffffffff);      // 0x25a670
    if (!do_restart) return;

    g_LpRestartCount0 /* 0xbfe68 */ = g_StartedLpCount;
    g_LpRestartCount1 /* 0xbfe6c */ = g_StartedLpCount;
    if (g_LpProgramsPatAtWake /* 0x9a441 */) g_dword_0x9c594 = 0;

    for (u32 lp = 1; lp < 0x800; ++lp) {
        LP_START_RECORD *rec = &g_LpStartRecord[lp];         // 0x1126a0, stride 0x20

        if (g_LpCreatedCount /* 0x235d4 */ == g_StartedLpCount)
            return;                                          // everybody is back

        if (rec->State != 4) continue;                       // only previously-live LPs

        g_LpResumeContext /* 0xfe050 */ =
            (u8 *)g_LpContextTable /* 0xa3f90 */ [rec->CtxIndex] + 0x29f90;
        rec->State = 1;
        HvpWakeLp(rec->ApicId, 0xffffffff, NULL);            // 0x2589c0

        // ---- four-second deadline, and it is FATAL -------------------
        for (u64 us = 0; rec->State == 1; us += 10000) {
            HvpStallExecutionMicroseconds(10000);            // 0x25bb48
            if (us + 10000 >= 4000000) {
                HvpCrashRendezvous(1);                       // 0x21f65c -- RESETS
                __debugbreak();
            }
        }
        while (rec->State != 4) { __pause(); }               // unbounded, no timeout
    }
}
```

Byte-level confirmations, all recomputed by hand:

- `lea rbx, [rip-0x12bb95]` at `0x23e24e` ⇒ `0x23e255 - 0x12bb95 = 0x1126c0` =
  `&g_LpStartRecord[1]`; `add rbx, 0x20` per iteration; `cmp edi, 0x800`.
- `mov ecx, [rbx+0x8]` is `HvpWakeLp`'s first argument ⇒ **`+0x08` is the APIC
  id** (§4).
- `mov eax, [rbx+0x18]` indexes `[r15 + 8*rax + 0xa3f90]` with `r15` = image
  base ⇒ **`+0x18` is the context-table index**.
- `cmp rsi, 0x3d0900` / `jae 0x23e2fd` → `mov cl, 1` → `call 0x21f65c`.

**Who calls it**: exactly one caller, `HvpLpPowerEntry` (`0x247b20`) at
`+0xd4`, with `do_restart = (g_LpStartMode != 1)`, on the BSP only
(`gs:[8] == g_BspLpId` is checked immediately before). `HvpLpPowerEntry` is in
turn reached from `HvpLpLongModeEntry` (`0x3a6690`, called twice — `rcx=0` then
`rcx=1`) and from `0x23ede0` (four call sites). This is the **power-transition
/ resume** re-start of the APs, not the first start. *Inferred from the shape;
no string names it.*

---

## 3. `HvpCrashRendezvous` — the funnel, and its own 25-second timeout

`0x21f65c`, 1117 bytes.

```c
// hvix64 RVA 0x21f65c.  Never returns.
void HvpCrashRendezvous(bool skip_quiesce)
{
    u8 set_a[0x108], set_b[0x108], set_c[0x108];
    memset(set_a, 0, 0x108); memset(set_b, 0, 0x108); memset(set_c, 0, 0x108);

    if (!skip_quiesce) HvpQuiesceLp();                       // 0x24f7a8
    __sti();                                                 // yes, interrupts on

    InterlockedCompareExchange(&g_FirstCrashedLpId /* 0x235e8 */, gs:[8], -1);
    bts(&g_CrashedLpBitmap /* 0xa8628 */, gs:[8]);

    if (g_FirstCrashedLpId == gs:[8]) {
        // I am the first.  Mark every other running LP and kick it.
        for (each lp in g_RunningLpSet /* 0xa8500 */ minus g_CrashedLpBitmap)
            _InterlockedOr8(&g_LpContextTable[lp][0x198], 0x80);   // "crash requested"
        (*g_pfnSendIpiToProcessorSet /* 0xd6d48 */)(&set, 0xff);   // vector 0xff
    }

    u64 t0 = HvpGetReferenceTime();                          // 0x2563dc, 100 ns units
    for (;;) {
        if (g_CrashedLpBitmap covers g_RunningLpSet) {
            HvpBugCheckReset(...);                           // 0x223f90
            __debugbreak();
        }
        if (HvpGetReferenceTime() - t0 > 250000000)          // 25.0 SECONDS
            HvpResetSystemDirect();                          // 0x330990
    }
}
```

Byte-level confirmations:

- `mov rax, [rip-0x148bdd]` at `0x21f91e` ⇒ `0x21f925 - 0x148bdd = 0xd6d48`;
  then `lea rcx,[rsp+0x20]; mov edx, 0xff; call rax`.
- `lock or byte [rcx+0x198], 0x80` at `0x21f90f`, with
  `rcx = [rdi + 8*rdx + 0xa3f90]`.
- The `250000000` compare is against a value from `0x2563dc`, whose body
  compares the platform timer frequency against `0x989680` = 10,000,000 —
  i.e. **100 ns units**, so the deadline is **25.0 s**. (proven)

`HvpBugCheckReset` (`0x223f90`), re-read because the earlier note missed one
instruction:

```
0x223f94  call 0x224084                          ; quiesce helper
0x223f99  if (gs:[8] == g_BspLpId) call 0x237d8c(code, 7, ...)   ; report, BSP only
0x223fb3  call 0x22400c
0x223fb8  call 0x224044
          if (gs:[8] != g_BspLpId) {
0x223fdc      cli
0x223fdd      lidt  [null descriptor]
0x223fe2      call 0x2241b8
0x223fe7      lock dec dword [g_StartedLpCount]  ; <-- the earlier note missed this
0x223fee      hlt ; jmp $-2
          }
0x223ff3  while (g_StartedLpCount > 1) pause;
0x223ffe  HvpResetSystem();
```

The `lock dec` is why the BSP's spin terminates on a multiprocessor guest:
each parking LP takes itself out of the count. **Without it the machine would
hang instead of resetting**, so on a 2-CPU guest the reset only completes if
the other LP actually takes the `0xff` IPI. If it never does, the 25 s deadline
in `HvpCrashRendezvous` covers it — but note that deadline is checked *before*
`HvpBugCheckReset` is entered, so once the barrier passes there is no further
timeout anywhere on the path.

`HvpResetSystem` (`0x2240bc`), re-verified in full:

```
  if (g_RootPartition /* 0xa9ed0 */) {
      if (HvpPreparePartitionForReboot(...) /* 0x2dec40 */ != 0)
          FUN_0x369800(**(void***)(g_RootPartition + 0x4658));
  }
  HvpQuiesceForReset();                                   // 0x2241b8
  if ((g_flags_0xd69e0 & 0x20) && gs:[8] == g_BspLpId && g_StartedLpCount <= 1) {
      cr4 |= (1 << 14);                                   // CR4.SMXE
      FUN_0x3a6c30(5, 0, 0, 0);                           // GETSEC[SEXIT]
      cr4 restored;
  }
  if (!g_ResetSuppressed /* 0xa9c18 */ && g_ResetRegValid /* 0xd42a0 */) {
      if (g_ResetRegSpace /* 0xd42a1 */ == 0)  *g_ResetMmioVa /* 0xd42b0 */ = g_ResetValue;
      else if (g_ResetRegSpace == 1)           out(g_ResetPort /* 0xd42a8 */, g_ResetValue /* 0xd42a2 */);
      HvpStallExecutionMicroseconds(5000);                // 0x1388
  }
  out(0x64, 0xFE);                                        // 8042
  HvpStallExecutionMicroseconds(50000);                   // 0xc350
  cli; lidt(null); hlt;
```

**Correction**: the `out dx, al` is at **`0x224178`**, not `0x224179`. There is
a `nop` at `0x224177` and another at `0x224179`. A debugger or an I/O exit that
reports RIP `0x224179` is reporting the address *after* the `out`, which is
exactly what an I/O VM exit does — so the previous identification is right, and
this makes it tighter rather than weaker.

**Why this matters for the rig**: on this platform the FADT's `RESET_REG` is
I/O port `0xCF9`, value `0x0F`. That `out` is an unconditional I/O instruction
executed by zpp's guest. Under QEMU/KVM port `0xCF9` is the chipset reset
control register, so the write becomes a **machine reset**, which with
`-no-reboot -no-shutdown` leaves the VM in `paused (shutdown)` with memory
intact and every KVM counter frozen at the same instant — `exits`,
`nested_run`, `insn_emulation` and `irq_exits` all together, with `halt_exits`
and `blocking` at zero. *That is a mechanism, not a measurement; the
falsifiable part is read #4 in §8.*

---

## 4. `LP_START_RECORD` — corrected layout (proven from `HvpAllocLpSlot`)

`HvpAllocLpSlot` at `0x23a800`, stores at `0x23a8c9`–`0x23a8e5`, with
`rbx = 0x1126a0 + (lp << 5)`:

```c
// hvix64 RVA 0x1126a0, stride 0x20
typedef struct _LP_START_RECORD {
/* +0x00 */ u32 State;      // 0 free / 1 armed / 2,3 LP progress / 4 live
/* +0x04 */ u32 Started;
/* +0x08 */ u32 ApicId;     // <-- CORRECTION: not a context pointer
/* +0x0c */ u32 Flags;      // the third hypercall input word
/* +0x10 */ u32 BlockIndex; // index into the table at 0xa3d80
/* +0x14 */ u16 Status;     // the HV_STATUS the LP reported
/* +0x18 */ u32 CtxIndex;   // index into g_LpContextTable at 0xa3f90
/* +0x1c */ u32 LpIndex;    // == lp, written for readback
} LP_START_RECORD;
```

`+0x08` is proven twice: `HvpAllocLpSlot`'s duplicate scan at `0x23a876`
compares `[rdx + r9 + 0x8]` against the incoming APIC id, and
`HvpRestartAllLogicalProcessors` passes `[rbx+0x8]` as `HvpWakeLp`'s first
argument. The existing note's "`+0x08` Context" is wrong.

Two early returns of `HvpAllocLpSlot`, both proven:

- `lp >= 0x800` → `0x05` (`HV_STATUS_INVALID_PARAMETER`)
- `rec[lp].State == 4`, **or any other record with `State == 4` already holding
  this APIC id** → `0x19` (`HV_STATUS_OBJECT_IN_USE`)

The second is a real hazard for a retry: once an LP has reached state 4, asking
for it again by index *or* by APIC id fails — it does not restart it.

### 4.1 The handler's guards do return a status (correction, proven)

`HvCallAddLogicalProcessor` at `0x2938a0` is 34 bytes and reads in full:

```asm
2938a0  sub  rsp, 0x28
2938a4  mov  rax, gs:[0x360]                  ; the partition
2938ad  test byte [rax + 0x1a0], 1            ; root-partition privilege
2938b4  jne  2938bd
2938b6  mov  eax, 0x06                        ; HV_STATUS_ACCESS_DENIED
2938bb  jmp  2938de
2938bd  mov  eax, [rcx]                       ; in->LpIndex
2938bf  cmp  eax, 0x800
2938c4  jb   2938cd
2938c6  mov  eax, 0x41                        ; HV_STATUS_INVALID_LP_INDEX
2938cb  jmp  2938de
2938cd  mov  r8d, [rcx + 0x8]                 ; in->ProximityDomainId
2938d1  mov  r9,  rdx                         ; out
2938d4  mov  edx, [rcx + 0x4]                 ; in->ApicId
2938d7  mov  ecx, eax                         ; lp index
2938d9  call 0x23909c                         ; the worker
2938de  add  rsp, 0x28
2938e2  ret
```

The existing note says "both guards return without writing `out`" and infers
that a caller reads a stale status out of the untouched buffer. **That is
wrong**: both guards put a status in `EAX`, which is the hypercall's return
value. A refused call reports `0x06` or `0x41` (`HV_STATUS_INVALID_LP_INDEX`,
*TLFS name recalled*), and Windows sees it.

### 4.2 Windows' side: a failed `0x76` does not bugcheck (proven)

From `ntoskrnl.exe`, with the PDB's names:

```c
// ntoskrnl RVA 0x582898
ULONG HvlpStartLogicalProcessor(u32 lp, u32 apic_id, u64 node, void *out38)
{
    u16 node_id = KeNodeBlock[node & 0xffff]->[2];
    short st;
    do {
        if (HvlpDepositPages(node_id) != 0) return that;         // memory first
        u32 *in  = HvlpAcquireHypercallPage(..., 1, 0, 0x10);    // 0x10 bytes
        u32 *outp= HvlpAcquireHypercallPage(..., 2, 0);
        in[0] = lp;
        in[1] = apic_id;
        in[2] = KeNodeBlock[node_id]->[4];                       // proximity domain id
        in[3] = 0x80000001;                                      // proximity info flags
        st = HvlInvokeHypercall(0x76, ...);
        if (st != 0x0b) copy 0x38 bytes of outp into out38, then out38[0..1] = st;
        release both pages;
    } while (st == 0x0b);                                        // INSUFFICIENT_MEMORY: retry
    return st ? 0xC0000001 /* STATUS_UNSUCCESSFUL */ : 0;
}
```

and its caller:

```c
// ntoskrnl RVA 0x580d28, HvlStartBootLogicalProcessors, inner loop
for (i = 1; i < HalQueryMaximumProcessorCount(); ++i) {
    if (!candidate[i].selected) continue;
    if (HvlpEnableNextLogicalProcessor(apic_id, node) < 0) break;   // <-- just STOPS
    ++HvlpLogicalProcessorCount;
}
... HvlpSelectVpSet(); ... HvlNotifyAllProcessorsStarted(); return 0;
```

**So on a `0x3e` start-up timeout Windows quietly boots with fewer logical
processors.** It does not bugcheck, does not retry, and does not reset —
`HV_STATUS_INSUFFICIENT_MEMORY` (`0x0b`) is the *only* status it retries, which
is the loop `ap-add-deposit-retry-loop.md` already documented.

That closes the question from both sides: **neither hvix64 nor Windows resets
the machine because an AP failed to report in through hypercall `0x76`.** The
reset has to come from §6, or from `HvpRestartAllLogicalProcessors` (§2.2), or
from outside hvix64 entirely.

Two further corrections that fall out of this: the input block is **`0x10`
bytes, four dwords**, not `0x0c`; and the third dword — the one the hvix64 side
stores at `LP_START_RECORD + 0x0c` and which the earlier note calls `Flags` —
is Windows' **proximity domain id**, with the actual flag word (`0x80000001`)
in a fourth dword hvix64's handler never reads.

---

## 5. `HvpLpLongModeEntry` (`0x3a6690`) — what the AP actually executes

Decoded because it decides which of the two AP bodies runs, and because two
instructions in it matter to zpp:

```
3a6690  mov ecx, 0xC0000080 ; mov eax, 0xD00 ; xor edx, edx ; wrmsr   ; EFER = LME|LMA|NXE
3a669e  cmp byte [rdi + 0xa8], 0                    ; trampoline page + 0xa8
        ; --- nonzero: the BSP already programmed PAT for us
3a66a7    mov cr0, 0x80010021                       ; PG|WP|NE|ET|PE
3a66b1    mov cr4, (cr4 & CR4.PGE) | 0x2e0
3a66c7    jmp 3a6723
        ; --- zero (the value HvpWakeLp writes on this rig): the AP does PAT itself
3a66c9    mov cr0, 0xC0010021                       ; + CD | NW   <<<< caches OFF
3a66d3    wbinvd                                     ; <<<<
3a66d5    mov cr4, (cr4 & CR4.PGE) | 0x260
3a66eb    mov rax, cr3 ; mov cr3, rax
3a66f1    wrmsr(0x277 /* IA32_PAT */, g_LpPatValue at RVA 0x84830)
3a6705    wbinvd                                     ; <<<<
3a6707    mov rax, cr3 ; mov cr3, rax
3a670d    mov cr0, cr0 & ~CR0.CD
3a6718    mov cr4, cr4 | CR4.PGE
3a6723  cmp byte [0xfe049], 0 ; jne 3a67a7
3a672c  cmp byte [0xfe078], 0 ; jne 3a67a7
        ; --- fresh-start path
3a6735    rbx = &g_LpRegImage (RVA 0x23b60)          ; what HvpWakeLp memcpy'd into
3a673c    lgdt [rbx+0x06] ; lidt [rbx+0x16]
3a6744    bt qword [rbx+0x30], 0 ; jb 3a684d         ; alternate path, reads MSR 0x1b
3a6750    fs=gs=ds=es=ss=0x20 ; ltr 0x30
3a6780    rsp = [rbx+0x28] ; wrmsr(KERNEL_GS_BASE, [rbx+0x20])
3a6795    mov cr3, gs:[0x458]
3a67a1    call 0x247fe0                              ; HvpLpInitAndReportIn
3a67a6    int3
        ; --- resume path
3a67a7    lgdt/lidt from the resume block ; ... ; mov cr3, gs:[0x458]
3a6801    patch the TSS descriptor type byte to 0x89 ; ltr 0x30
3a683b    call 0x247b20 (rcx=0)                      ; HvpLpPowerEntry(0)
3a6847    call 0x247b20 (rcx=1)                      ; HvpLpPowerEntry(1)
3a684c    int3
```

Two things zpp has to survive on the AP, both **proven**, both happening
*before* the AP executes a single hvix64 C function:

- **`CR0.CD` and `CR0.NW` are set, and `wbinvd` runs twice.** If either exits
  and the exit is mishandled, the AP dies here with `LpStartRecord[lp].State`
  still `1`.
- **`cr3` is loaded from `gs:[0x458]`**, so `GS_BASE` must already point at the
  LP block, which the trampoline set up.

`0xfe049` and `0xfe078` are both zero in the on-disk image, so a first boot
takes the `0x247fe0` path. `HvpLpPowerEntry` (`0x247b20`) is the resume path,
and **every failure inside it calls `HvpCrashRendezvous(1)`** — at `0x247d56`,
reached from three separate checks (`0x258744(0)`, `0x258744(1)`, `0x14000c`).
An AP that fails to come back from a power transition resets the machine
rather than reporting anything.

**The fresh-start body does the opposite, and the asymmetry is the point.**
`HvpLpInitAndReportIn` (`0x247fe0`) is not among the sixteen callers of
`HvpCrashRendezvous` — checked by scanning every `E8`/`E9` in the image for a
`rel32` landing on `0x21f65c`. It reports through the record instead:

```asm
247fec  mov  rbp, gs:[0]                      ; the LP block
247ff5  call 0x3a57c0
247ffa  mov  eax, [rbp + 0xa0]                ; this LP's index
248000  lea  rsi, [rip - 0x135967]            ; -> RVA 0x1126a0, the record array
248007  shl  rax, 5                           ; * 0x20
24800d  cmp  dword [rip - 0x1a42e0], 1        ; g_LpStartMode (0xa3d34)
248014  mov  dword [rax + rsi], 2             ; State = 2
...
248100  mov  dword [rax + rsi], 3             ; State = 3
248107  call 0x260f88 / 0x257f6c / 0x235ee4 / 0x3349c0
24811b  sti
24811c  call 0x249404
248121  mov  ecx, [rbp + 0xa0] ; movzx edx, di ; call HvpLpReportStartResult
```

So the rule is: **a processor being started for the first time reports a
status; a processor being resumed resets the machine.** Both write the same
`State` word, which is why the word alone does not distinguish them — pair it
with `g_FirstCrashedLpId` (§8 read #1).

---

## 6. The fatal VM exits — the most likely mechanism

`hv_dispatch_vmexit` (`0x35e1b0`) takes the exit reason in `EDX`, handles the
hot reasons inline (`0x2c` APIC access, `0x30` EPT violation, `0x31` EPT
misconfiguration, …) and calls `HvpHandleRareVmExit(vp, edx)` at `0x35eb5d`
for everything else. That the comparisons really are raw VMX basic exit reasons
is confirmed three ways: `0x30`/`0x31` match the "MinimalLoop EPT violation"
strings, `0x2c` matches "MinimalLoop APIC access", and case `0x22` does
`vmread(0x200a)` — the **VM-entry MSR-load address** — which is only meaningful
for exit reason 34, "VM-entry failure due to MSR loading".

The dispatch head, disassembled (`esi` = `edx` = the full 32-bit reason):

```
35d409  mov esi, edx
35d42b  mov eax, edx
35d444  test edx, edx      ; je -> reason 0    exception or NMI
35d44c  sub  eax, 2        ; je -> reason 2    TRIPLE FAULT
35d455  sub  eax, 1        ; je -> 0x35e19e    reason 3, INIT
...
35d7a9  test esi, esi      ; jns -> normal;  sign set == VM-ENTRY FAILURE
```

### 6.1 INIT — reason 3 — reboots unconditionally (proven)

`0x35e19e` is a `call 0x2c9524`, and `0x2c9524` is 32 bytes long, in its
entirety:

```asm
2c9524  sub  rsp, 0x28
2c9528  mov  edx, gs:[0xa0]                       ; this LP's index
2c9530  lea  rcx, [rip - 0x2bc54f]                ; -> RVA 0xcfe8
                                                  ;    "[%d]: Received INIT; rebooting.\n"
2c9537  call 0x24bd60                             ; HvpDebugPrint
2c953c  xor  ecx, ecx
2c953e  call 0x21f65c                             ; HvpCrashRendezvous(0)
2c9543  int3
```

**There is no condition.** If hvix64 takes a VM exit with basic reason 3 while
running any guest, the machine reboots. A second site with the same behaviour
lives in the minimal-loop handler (string `0x10690`,
`"MinimalLoop VMX_EXIT_REASON_INIT_INTR. Rebooting the system"`).

This sits directly in zpp's path: zpp emulates INIT/SIPI for the guest's
application processors, and hvix64 issues INIT-SIPI-SIPI through
`HvpApicIcrWrite` (`0x257af0`) as part of `HvpWakeLp`. An INIT that is
*delivered to*, or *reflected into*, a processor currently in VMX non-root
operation under hvix64 produces exactly this exit. *That an INIT is what
happened on the rig is a hypothesis; that hvix64 reboots on one is proven.*

### 6.2 Triple fault — reason 2 (proven)

```c
// hvix64 RVA 0x2c7da4
void HvpHandleTripleFault(HV_VP *vp)
{
    if (vp->IsRootVp /* +0x160 */ == 0)
        return HvpVpShutdown(vp);                        // 0x2ca574

    if (InterlockedExchangeAdd(&g_RootPartition->[0x6680], 1) + 1 != 1)
        return HvpVpShutdown(vp);                        // not the first
    if (g_RootPartition->[0x1d0] == 1)                   // *inferred*: VP count
        return HvpVpShutdown(vp);                        // single-VP partition

    bts(&vp->[0x188], 10);
    HvpBugCheck(0x26, current_vtl, ..., ..., ...);       // 0x21f21c
    __debugbreak();
}
```

A root-VP triple fault on a **multi-VP** partition bugchecks with code `0x26`,
readable at `0x112660`. On a **single-VP** partition it falls into
`HvpVpShutdown` instead — which reboots with no bugcheck at all. That is a
discriminator between the 1-CPU and 2-CPU rigs, *if* `+0x1d0` is the VP count.

`vp + 0x160` is the "is a root-partition VP" flag, proven twice: `HvpVpShutdown`
prints "Root virtual processor shutdown" on the branch where it is non-zero,
and `HvpHandleTripleFault` reads `g_RootPartition` only on that same branch.

### 6.3 `HvpVpShutdown` — the "Root virtual processor shutdown" reboot (proven)

```c
// hvix64 RVA 0x2ca574
void HvpVpShutdown(HV_VP *vp)
{
    u8   vtl   = vp->CurrentVtlBlock->[0x14];
    u32  wants = vp->[0x1b0] & vp->Partition->[0x66a0]
               & ~((1u << vtl) | ((1u << vtl) - 1));       // strictly-higher VTLs only
    u32  target = 0;

    if (wants == 0) {
        if (vp->IsRootVp /* +0x160 */ != 0) {
            HvpDebugPrint("[%d]: Root virtual processor shutdown; rebooting.\n", gs:[0xa0]);
            HvpCrashRendezvous(0);                         // 0x21f65c -- RESETS
            __debugbreak();
        }
        if (g_byte_0xae06c && (g_dword_0xae050 & 0x1000))
            HvpTraceEvent(0x1d2c, ...);
    } else target = bsf(wants);

    HvpBuildUnrecoverablePayload(vp, 2, 0, &payload);       // 0x2ca194
    HvpDeliverIntercept(vp, target, /* HvMessageType */ 0x80000021,
                        /* size */ 0x28, &payload);        // 0x2c5a10
}
```

`0x80000021` here is the **message type** `HvMessageTypeUnrecoverableException`
with a `0x28`-byte payload — not a VMX exit reason. The numeric collision with
VMX `0x80000021` is a coincidence; do not conflate them.

Seven callers: `HvpHandleTripleFault` (`0x2c7e40`), `HvpHandleRareVmExit`'s
**exit reason `0x22`** case (`0x35dd0b`, VM-entry failure due to MSR loading),
`0x2c21c4+0x3b4`, `0x2eb6b8+0x110`, `0x2eba3c+0x20`, `0x35f850+0x55`,
`0x38dc34+0x1da`.

### 6.4 `HvpVpFatal` — the widest reset trigger (proven)

```c
// hvix64 RVA 0x2c53e4
void HvpVpFatal(HV_VP *vp)
{
    if (vp->IsRootVp /* +0x160 */ != 0) {
        HvpCrashRendezvous(0);                             // RESETS, silently
        __debugbreak();
    }
    u32 old = vp->[0x188];  vp->[0x188] = old | 2;
    if ((old & 3) == 0 && vp->[0xc38] != 0) { ... }
    HvpClearVpPending(vp);                                 // 0x223cfc
    vp->State = 0x1f;                                      // shutdown
    HvpVpShutdownIntercept(vp, 0, ..., vp->[0x164], 0);    // 0x2e9ef4
}
```

Eleven callers. The one that matters:

```c
// hvix64 RVA 0x35b9a0 -- HvpHandleVmEntryFailure
void HvpHandleVmEntryFailure(HV_VP *vp)
{
    PARTITION *p = vp->[0x3c0];
    if (p->[0x15e0] == 3 && (p->[0x1608] & 1)) {           // can reflect one level up
        VMCS12 *v = *(void **)(p->[0x15e8] + 8);
        v->[0x2b4] = 0x80000021;      // exit reason: VM-entry failure, invalid guest state
        HvpReadExitQualification(&v->[0x2d0]);             // 0x331094
        HvpReflectExitToParent(vp, p);                     // 0x34e560
        return;
    }
    HvpVpFatal(vp);                                        // -> a root VP resets the machine
}
```

The literal `0x80000021` written at `0x35b9cf` is what identifies this
function: it is the exit reason hvix64 *manufactures* for its own parent when
it decides an entry failed with invalid guest state.

And in `HvpHandleRareVmExit` itself, for **any** exit reason with bit 31 set:

```
35d7a9  test esi, esi
35d7ab  jns  0x35dfa1                       ; not an entry failure
35d7b1  cmp  dword [rbx + 0x15e0], 3
35d7b8  jne  0x35e151                       ; -> HvpBugCheckVmEntryFailure -> HvpBugCheck(6,4,..)
35d7be  mov  rcx, rdi
35d7c1  call 0x2c53e4                       ; HvpVpFatal -> a root VP resets
```

**Summary: hvix64 treats every VM-entry failure as fatal to the virtual
processor, and fatal to a *root* virtual processor means resetting the
machine.** For zpp this is the sharpest of the three, because the tree already
records `vm_entry_failure` with `reason=0x80000021` on its own nested path, and
anything that makes hvix64's `VMLAUNCH`/`VMRESUME` of its L2 fail lands here.

### 6.5 The default: unhandled exit reason → `HvpBugCheck(reason + 0x33)`

```
35e148  lea  ecx, [rdx + 0x33]
35e14b  call 0x21f21c                       ; HvpBugCheck
35e150  int3
```

A decode rule when reading `0x112660`: a bugcheck code of `N + 0x33` for a
plausible VMX exit reason `N` means an exit hvix64 did not expect to receive at
all.

---

## 7. Corrections to the notes in `.references/hyperv/`

To `hvix64-reset-path-and-crash-record.md`:

1. **`HvpBugCheckReset` decrements `g_StartedLpCount`** in each parking LP
   (`lock dec` at `0x223fe7`). The earlier transcription omitted it, which made
   the BSP's `while (StartedLpCount > 1)` look like a deadlock on a
   multiprocessor guest. It is not.
2. **The `out` is at `0x224178`**, with `nop`s either side; `0x224179` is the
   *return* address.
3. **`FUN_0xdec40` is `0x2dec40`** and **`FUN_0x169800` is `0x369800`** — the
   earlier note dropped a digit in the VA-to-RVA conversion.
4. **`HvpResetSystem` ends in `cli; lidt(null); hlt`**, not a bare spin, and it
   performs a `GETSEC[SEXIT]` (leaf 5, with `CR4.SMXE` set around it) first
   when `g_flags_0xd69e0 & 0x20` and this is the last running LP.
5. §2's candidate list is superseded. The "Received INIT" and "Root virtual
   processor shutdown" strings are not competing guesses about a watchdog; they
   are two specific VM-exit cases with named handlers (§6.1, §6.3).

To `hvix64-add-logical-processor-decompiled.md`:

6. `LP_START_RECORD +0x08` is the **APIC id**, not a context pointer (§4).
7. There is a **second** four-second AP wait, in
   `HvpRestartAllLogicalProcessors`, and unlike the worker's it resets the
   machine (§2.2).
8. **The handler's two guards do return a status** — `0x06` and `0x41` in
   `EAX` — so "a caller reads whatever was in the untouched output buffer" is
   wrong (§4.1).
9. The hypercall input is **`0x10` bytes, four dwords**, not `0x0c`, and the
   third dword is Windows' proximity domain id rather than a flags word
   (§4.2).
10. `HvpLpInitAndReportIn` (`0x247fe0`) **never** calls `HvpCrashRendezvous`;
    only the resume body `HvpLpPowerEntry` does (§5).

---

## 8. The post-mortem read list, in priority order

All at `paused (shutdown)`, with hvix64's base derived the usual way and its
own CR3 walked. Five reads; the first two are decisive.

| # | read | what it settles |
|---|---|---|
| **1** | `hvix64 + 0x235e8` (u32) | `g_FirstCrashedLpId`. **`0xffffffff` ⇒ hvix64 never entered `HvpCrashRendezvous`, so none of §6 happened** and the reset came from somewhere else entirely. Any other value names the LP that initiated it |
| **2** | `hvix64 + 0xa8628` (qword) | the crashed-LP bitmap. One bit ⇒ the other LP never took the `0xff` IPI, so the reset went out through the 25 s `HvpResetSystemDirect` path. Both bits ⇒ the rendezvous completed and it was `HvpBugCheckReset` |
| 3 | `hvix64 + 0xa8610` (byte) and `+0x112660` (qword) | `0` and `0` **with #1 set** ⇒ INIT or `HvpVpShutdown` (§6.1/§6.3), because neither touches `HvpBugCheck`. Non-zero ⇒ the code names it: `0x26` root triple fault, `0x06` VM-entry failure, `N + 0x33` unhandled exit reason `N` |
| 4 | `hvix64 + 0xd42a0` / `0xd42a1` / `0xd42a2` / `0xd42a8` | must read `{1, 1, 0x0F, 0x0CF9}`. **Falsifiable**: if it does not, the `out 0xCF9` mechanism of §3 is wrong and everything resting on it should be discarded |
| 5 | `hvix64 + 0x1126a0 + 0x20*lp` | the per-LP record with the corrected field layout (§4). `State == 1` ⇒ **wait A did not time out** (§2.1) |

Reads 1 and 2 are what this session was worth. `g_FirstCrashedLpId` is a single
32-bit word that separates "Hyper-V deliberately reset the machine" from
"something else did", and it has never been read.

---

## 9. What is not settled

- **Which** of §6's three triggers fired, if any. That is read #1 plus #3, not
  more disassembly.
- Whether `partition + 0x1d0` really is the VP count (§6.2). Inferred from its
  use as a `== 1` guard beside a per-partition crash counter.
- `HvpRestartAllLogicalProcessors`' trigger. It is reachable only from the
  power/resume entry and no string names the transition, so "this is the S3/S4
  resume path" is *inferred from shape*.
- The `0xff` vector's delivery mechanism: `g_pfnSendIpiToProcessorSet`
  (`0xd6d48`) is bound at boot and reads `0` in the image, so which APIC write
  path it points at was not chased.
- `0x2c21c4` (1024 bytes), `0x2cc6f0` (5365 bytes), `0x23c7e0` and `0x2c3764`
  all reach `HvpCrashRendezvous` and were not decoded. None carries a string,
  so naming them needs real work rather than a grep.

---

## 10. Method notes worth keeping

- **`llvm-objdump` over a raw slice prints garbage in its `#` comments.** The
  blob has no base, so every RIP-relative annotation is wrong. Recompute
  `next_insn_rva + disp32` by hand. Two globals in the earlier notes are wrong
  precisely because this was not done.
- **Ghidra's `FUN_fffff856760xxxxx` names are full VAs, not RVAs.** Subtract
  `0xfffff85675e00000`; do not strip digits. `FUN_fffff856760563dc` is RVA
  `0x2563dc`, not `0x563dc` — and `0x563dc` in this image is zero-filled, so
  the mistake disassembles to nothing and looks like a dead end rather than an
  error.
- **Find call sites by scanning for `E8`/`E9` with a matching `rel32`**, and
  string references by scanning every 4-byte window for `i + 4 + disp32 ==
  target`. Both are a dozen lines of Python over the raw file and they found
  every caller in this document, including the ones no decompilation covered.
- **Scan for an immediate rather than grepping a decompilation.** The second
  four-second wait was found by searching the raw bytes for `00 09 3d 00`; it
  is in no note in the corpus and no amount of reading `HvCall 0x76` would have
  produced it.
