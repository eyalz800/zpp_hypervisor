# Nested EPT and exit reflection, read against KVM v6.12

A static review of `hypervisor/src/hypervisor/nested_*.cpp` and
`zpp/arch/x86_64/vmx/nested_ept.h` against KVM's `arch/x86/kvm/vmx/nested.c`,
`vmx.c` and `arch/x86/kvm/mmu/{mmu.c,paging_tmpl.h,spte.c}` at tag v6.12
(`.references/kvm/VERSION`), and against the SDM in `.references/sdm.txt`.

No production code was changed. Nothing was booted.

## The premise this review was given is already withdrawn

The review was commissioned to explain a second-level guest "livelocked on a
single EPT violation - the same guest RIP, qualification `0x181`, repeating
until the guest gives up".

`ae6e873` ("docs: withdraw the EPT livelock claim; the guest is waiting, not
looping"), on `develop`, retracts that reading of the ring: the nine
violations at `rip=0xfffff802c78a72f3` carry consecutive second-level entry
numbers `0x1b4ef`..`0x1b4fb`, and the records *after* them are three MSR
reads, two x2APIC LVT writes, CPUID, and the synthetic interrupt controller
being set up. Nine faults for one instruction is a page being faulted in.

So **there is no EPT livelock to explain**, and findings are ranked below
against the symptom that actually survives: the root partition sets up the
synthetic interrupt controller, arms synthetic timer 0, and then does no work
at all for hours - `l2_working_trace_count` frozen at 89,094 while
`l2_exit_trace_count` passes four million, MSI-X vectors never programmed, the
storage stack never reached.

That reframing matters for the ranking, and it promotes a finding that has
nothing to do with reflection: **the watched-page emulator translates a
second-level guest's linear addresses as though it were the first-level
guest**, and the pages it watches are the local APIC and the NVMe controller
registers - which is exactly the class of thing a guest waits on for ever.

---

## Ranked findings

### 1. `translate_guest_linear` walks a second-level guest's page tables as if its CR3 held host-physical addresses

**Severity: high. Fits the surviving symptom. Not a reflection bug at all.**

**What we do.** `hypervisor::translate_guest_linear`
(`hypervisor/src/hypervisor/hypervisor.cpp:2807`) walks the guest's four-level
page table starting at `this->vmcs.guest_cr3()` (`:2840`) and reaches each
level with

```cpp
auto * entries = static_cast<const std::uint64_t *>(
    map_window_at(transfer_window_first_page, table, 1));   // :2857
```

`map_window_at` (`hypervisor.cpp:1366`) maps a **host-physical** page into the
window. That is correct while `vmcs01` is current: the first-level guest's
physical addresses are host-physical, because `initialize_ept` builds an
identity map.

It is wrong while **`vmcs02`** is current. There, `guest_cr3()` is the
*second-level* guest's CR3, and every table it names is a **second-level
guest-physical** address, which has to be run through the guest hypervisor's
own EPT before it means anything. Nothing on this path does that, and nothing
on it checks `running_l2`.

The path is reachable, and by the hot route. `on_l2_ept_fault`
(`nested_entry.cpp:3135` and `:3289`) calls
`on_ept_violation(cpu, context, guest_walk.physical_address)` for exactly the
case where our own tables refused the access - a watched page.
`on_ept_violation` (`watched_page.cpp:225`) matches the watch and then calls
`decode_guest_instruction(cpu, context)` (`watched_page.cpp:295`), which is
`hypervisor.cpp:2911` and whose first act is
`translate_guest_linear(vmcs.guest_rip())` (`:2924`).

The watched pages are the NVMe doorbell and controller registers
(`hypervisor.cpp:1732`, `:6316`, `:6351`) and the local APIC page
(`local_apic_write.cpp:414`).

The failure is silent in both directions. `map_window_at` performs no range
check of its own, so a second-level guest-physical address is simply mapped as
host-physical and read; the `present` bit test at `:2865` is applied to
whatever bytes are there. Either it fails and the caller falls back to
*stepping* the instruction, or it succeeds and the decoder is handed unrelated
bytes. `translate_guest_linear`'s own comment at `:2816-2820` names both
outcomes - "the copy faults in root mode with no recovery point armed, or
unrelated bytes decode into a plausible instruction and a fabricated value is
written to a device register" - it just attributes them to `os_page_table`
rather than to nesting.

The stepping fallback is the outcome with the larger blast radius, and
`build_vmcs02` already documents it (`nested_entry.cpp:1157-1181`): stepping
opens the page **for every processor** for one instruction, and if anything is
reflected to the guest hypervisor before the monitor-trap exit arrives, the
control is recomputed away, `stepping_watch[cpu]` stays set for ever and the
page is left writable with nothing re-closing it - "this VMM stops seeing
local APIC writes entirely".

**What KVM does.** KVM never confuses the two levels because the translation
function is selected per level. `nested_ept_init_mmu_context`
(`.references/kvm/nested.c:474-485`) sets `vcpu->arch.mmu = &vcpu->arch.guest_mmu`
with `get_guest_pgd = nested_ept_get_eptp`, and separately sets
`vcpu->arch.walk_mmu = &vcpu->arch.nested_mmu`. Every read of a guest page
table during a walk then goes through
`kvm_translate_gpa(vcpu, mmu, gfn_to_gpa(table_gfn), nested_access, &walker->fault)`
(`.references/kvm/paging_tmpl.h:379`), which is a no-op for the direct MMU and
a full L1-EPT translation for `nested_mmu`. An L2 GPA is never dereferenced as
a host address.

**SDM.** Not needed - this is a level-confusion bug, not an architectural
disagreement. The relevant architectural fact, that a second-level
guest-physical address must be translated by the guest hypervisor's EPT before
it names memory, is SDM 31.3.2 (`.references/sdm.txt:205446`), which the
project already relies on in `walk_ept`.

**Could it produce the stall?** Yes, and it is the best fit of anything in
this review. The root partition owns the NVMe controller; bringing up the
storage stack means writing `CC`, the admin queue base registers and the
doorbells - every one of them on a watched page. Under `vmcs02` those writes
are either emulated from garbage or stepped, and "MSI-X vectors are never
programmed" is what a storage stack that never completes initialisation looks
like from outside.

**One measurement that settles it.** Count it, do not reason about it. Add a
per-CPU counter incremented in `decode_guest_instruction` when
`running_l2[cpu]` is set, and a second when `translate_guest_linear` returns
empty on that path. A boot in which the first is non-zero says the path is
live under `vmcs02`; the ratio of the second to the first says whether it is
failing loudly (stepping) or silently (garbage decode). It costs no behaviour
change. If the first counter is zero, this finding is inert on the rig and
drops to a latent bug.

**Same defect, second copy.** `hypervisor::guest_linear_to_physical`
(`guest_memory.cpp:114`) has the identical shape: it walks from
`vmcs.guest_cr3()` and reads each level with `read_guest_physical`, which is an
identity-map read (`guest_memory.cpp:48`). Its two callers are
`read_guest_linear` / `write_guest_linear` (`nested_vmx.cpp:851`, `:880`),
which serve the VMX-instruction operands - executed by the *first-level*
guest with `vmcs01` current - so it is correct today. It is one caller away
from not being.

---

### 2. Host-state checks on vmcs12 are ours to make, and we make one of about twenty

**Severity: high. A guest instruction can stop a processor. Cannot explain the stall.**

**What we do.** `build_vmcs02` checks exactly one host-state field:

```cpp
if (0 == (exit12 & exit_host_address_space_size)) {          // nested_entry.cpp:879
    return std::unexpected(zpp::error{error::nested_host_state_unsupported});
}
```

Everything else in vmcs12's host-state area is copied straight into
**vmcs01's guest-state area** by `load_l1_host_state`
(`nested_entry.cpp:2239-2436`) at reflection time: `host_rip` (`:2260`),
`host_rsp`, `host_cr0/3/4`, the six selectors, `host_fs_base`, `host_gs_base`,
`host_gdtr_base`, `host_idtr_base`, `host_tr_base`, the three SYSENTER fields.

Hardware cannot check these on our behalf. vmcs02's host state is *ours*
(`host_state_fields`, `nested_entry.cpp:347`), so the processor validates our
host state, not the guest hypervisor's. The guest hypervisor's is validated
only when it is loaded as vmcs01's **guest** state - and a failure there is a
VM-entry failure on `vmcs01`, which `on_vm_entry_failure` halts the processor
on. That is precisely the outcome CLAUDE.md forbids: a guest instruction
(`VMLAUNCH` with a malformed host-state area) stopping a CPU.

**What KVM does.** `nested_vmx_check_host_state`
(`.references/kvm/nested.c:3013-3080`) makes all of them before entry, and
returns `VMXERR_ENTRY_INVALID_HOST_STATE_FIELD`:

- `nested_host_cr0_valid` / `nested_host_cr4_valid` against the FIXED0/FIXED1 MSRs, `kvm_vcpu_is_legal_cr3`;
- `is_noncanonical_address` on `host_ia32_sysenter_esp`, `host_ia32_sysenter_eip`, `host_fs_base`, `host_gs_base`, `host_gdtr_base`, `host_idtr_base`, `host_tr_base`, `host_rip`;
- `kvm_pat_valid(host_ia32_pat)` when the load control is set;
- `host_cr4 & X86_CR4_PAE` required when `ia32e`; and when not `ia32e`, `VM_ENTRY_IA32E_MODE` clear, `host_cr4 & PCIDE` clear, `host_rip >> 32` zero;
- every host selector with RPL and TI bits clear, `host_cs_selector != 0`, `host_tr_selector != 0`, `host_ss_selector != 0` unless `ia32e`;
- `kvm_valid_efer(host_ia32_efer)` with LMA and LME each equal to the host address-space size control.

**SDM.** These are SDM 29.2.2, and the project's own `build_vmcs02` comment at
`nested_entry.cpp:876-882` already names it as "an error 8 condition on its own
terms - the host-state checks are where the address-space size and the CS
selector are validated together". The CS selector check is the one that
comment cites and the code does not make.

**Could it produce the stall?** No. Hyper-V's host-state area is well formed;
if it were not, we would see a halted processor, not a guest idling. Listed
second because of severity, not fit.

**Confirmed or refuted in one measurement.** It cannot be, from a boot - the
gap is only reachable from a malformed vmcs12. It is a code change, not a
measurement: transcribe the list above into `build_vmcs02` and refuse with
`nested_host_state_unsupported`.

---

### 3. No shadow-EPT invalidation when an EPT violation is reflected

**Severity: medium. Real divergence. Structurally cannot explain a `reflected_walk` stall.**

**What we do.** `on_l2_ept_fault`'s three reflecting branches
(`nested_entry.cpp:3048`, `:3060`, `:3123`) call `reflect_l2_exit` and return.
Nothing drops the shadow leaf for the faulting address and nothing executes
`INVEPT`. `invalidate_ept_locally` is called only after a leaf is *installed*
(`nested_entry.cpp:3217`) and on a shadow rebuild (`nested_ept.cpp:498`).

**What KVM does.** Two invalidations, on every reflected nested EPT violation:

- `kvm_inject_emulated_page_fault` (`.references/kvm/x86.c:986-1005`), with the
  comment *"Invalidate the TLB entry for the faulting address, if it exists,
  else the access will fault indefinitely (and to emulate hardware)"*, calls
  `kvm_mmu_invalidate_addr(vcpu, fault_mmu, fault->address, KVM_MMU_ROOT_CURRENT)`
  whenever the fault reported `PFERR_PRESENT_MASK` and not `PFERR_RSVD_MASK`;
- `nested_ept_inject_page_fault` (`.references/kvm/nested.c:455`) then calls
  `nested_ept_invalidate_addr(vcpu, vmcs12->ept_pointer, fault->address)` to
  reach *other* cached EPTP02 roots derived from the same EP4TA, because "the
  TLB associates mappings to the EP4TA rather than the full EPTP".

**SDM.** This is the architectural contract a guest hypervisor is entitled to
rely on, SDM 31.4.3.4, "Guidelines for Use of the INVEPT Instruction"
(`.references/sdm.txt:206431`):

> Software may use the INVEPT instruction after modifying a present EPT
> paging-structure entry [...] to change any of the privilege bits 2:0 from 0
> to 1. Failure to do so may cause an EPT violation that would not otherwise
> occur. Because an EPT violation invalidates any mappings that would be used
> by the access that caused the EPT violation (see Section 31.4.3.1), an EPT
> violation will not recur if the original access is performed again, even if
> the INVEPT instruction is not executed.

A guest hypervisor that relaxes a permission and skips `INVEPT` is correct on
hardware and is not correct against our shadow, because our shadow keeps the
old leaf.

**Could it produce the observed record?** Only for `reflected_permission`
(`nested_entry.cpp:3122-3129`), never for `reflected_walk`. The withdrawn
record's qualification `0x181` has bits 5:3 clear, and
`reflected_ept_violation_qualification` derives those from
`guest_walk.permissions` - which is empty only when the guest hypervisor's own
walk did not reach a leaf, and in that case `install_shadow_leaf` deliberately
left the shadow entry absent (`nested_ept.cpp:268-270`), so there is nothing
cached to invalidate. **This is a real gap that cannot explain that symptom**,
and saying so is the point of the finding.

**One measurement.** Count `reflected_permission` dispositions per boot -
`l2_ept_dispositions[cpu][reflected_permission]` already exists
(`nested_entry.cpp:2929`). If it is zero on a real boot, the gap is unreachable
in practice and the fix is a two-line hygiene change rather than a priority.

**The citation beside it is off by one subsection.** `nested_entry.cpp:3161`
cites "SDM 31.4.3.3, Guidelines for Use of the INVEPT Instruction"; in this
revision that section is **31.4.3.4** (`.references/sdm.txt:206431`), and
31.4.3.3 is a different one. CLAUDE.md's rule about unverified section numbers
applies to drifted ones too.

---

### 4. `walk_ept` cannot tell "the entry could not be read" from "the entry is not present"

**Severity: medium. Narrow trigger. Would produce exactly the symptom this review was commissioned for, if it fired.**

**What we do.** `walk_ept` (`nested_ept.h:414`) reports an unreadable entry as
absent:

```cpp
auto entry = read(table + (index * sizeof(std::uint64_t)));
if (!entry) {
    result.status = ept_walk_status::not_present;   // nested_ept.h:440
    result.permissions = ept_permissions();
    return result;
}
```

and the doc comment says so outright (`nested_ept.h:407-412`). The reader is
the lambda at `nested_entry.cpp:2991-3001`, which fails when
`read_guest_physical` fails, which is `guest_memory.cpp:54-57`: refused for any
address at or above `guest_physical_limit = 512 GB` (`guest_memory.cpp:23`).

`compose_ept` then reports `reflect_violation` (`nested_ept.h:596-598`) and
the guest hypervisor is told its own tables do not map a page they do map.
Its tables say otherwise, so it has nothing to do, and it resumes into the
identical fault. Permanent.

**What KVM does.** The same, and deliberately: `FNAME(walk_addr_generic)`
(`.references/kvm/paging_tmpl.h:395-406`) does `goto error` when the table page
has no visible memslot or the host address is bad, and `error` builds a
reflected EPT violation. So the *policy* matches. Two differences matter:

- KVM's L0 has a flat memslot map of the whole of L1's memory, so the case
  arises only for genuine MMIO, whereas ours has a hard 512 GB ceiling;
- KVM's reflected `exit_qualification` carries the permissions accumulated
  from the levels **above** the failure - `pte_access` is only updated after a
  successful read (`paging_tmpl.h:415`) - so an unreadable-table reflection has
  bits 5:3 non-zero, while a genuinely-not-present one has them zero. Ours
  reports zero for both.

**SDM.** SDM Table 30-7 Note 2 (`.references/sdm.txt:203939`): "Bits 5:3 are
cleared to 0 if either (1) any of EPT paging-structure entries used to
translate the guest-physical address of the access causing the EPT violation
is not present; or (2) 4-level EPT is in use and the guest-physical address
sets any bits in the range 51:48". Our behaviour is the *architectural* answer
for a not-present entry; the objection is that we give it for a case that is
not that.

**Could it produce the observed record?** It is the only mechanism in the
review that produces a permanently reflected violation with bits 5:3 clear at a
fixed RIP. But its trigger is narrow: `map_window_at` cannot fail for this
caller (`transfer_window_first_page` is 8, `mapping_window_pages` is 10, and a
straddling entry read needs exactly two pages - `hypervisor.h:4605`, `:4734`,
`:4760`, `:4780`), so the *only* way to reach it is a guest-physical address at
or above 512 GB.

**One measurement, and it needs no boot.** Read the rig launcher's `-m`. If the
guest has less than 512 GB, this cannot fire and the finding is latent. If it
can fire, the code change that makes it diagnosable is one enum value:
`ept_walk_status::unreadable`, recorded into
`l2_ept_stall_record::guest_walk_status`, which the stall log already prints
(`nested_entry.cpp:2940-2955`).

---

### 5. `ept_permissions::present()` counts bit 10 with mode-based execute control off

**Severity: medium. Wrong exit reason handed to the guest hypervisor. Cannot explain the stall.**

**What we do.** `present()` (`nested_ept.h:88-91`) returns
`m_read || m_write || m_execute || m_execute_user`, unconditionally, and
`ept_permissions::of` (`:47`) takes `m_execute_user` from `epte::execute_user()`,
which is bit 10 (`ept.h:129`). `build_vmcs02` clears
`secondary_mode_based_execute` from vmcs02 unconditionally
(`nested_entry.cpp:1317-1319`), with a good reason given there.

So for an entry in the guest hypervisor's tables with bits 2:0 clear and bit 10
set, `walk_ept` decides the entry is present (`nested_ept.h:447`), falls
through to `ept_walk::misconfigured` (`:465`), and that returns true - because
`normalised(false)` of `(0,0,0,1)` is `(0,0,0,0)` (`nested_ept.h:136-150`). The
result is `reflect_misconfiguration` and the guest hypervisor is handed **exit
reason 49** for an entry hardware treats as simply not present.

**What KVM does.** `FNAME(is_present_gpte)` for `PTTYPE_EPT` tests the RWX bits
the MMU was configured with; `nested_ept_new_eptp`
(`.references/kvm/nested.c:463-472`) passes `execonly` and the A/D setting into
`kvm_init_shadow_ept_mmu`, so presence and reserved-bit rules follow the
controls actually in force rather than a fixed mask.

**SDM.** SDM 31.3.2 (`.references/sdm.txt:205446`): "An EPT paging-structure
entry is present if any of bits 2:0 is 1; otherwise, the entry is not present.
The processor ignores bits 62:3 and uses the entry neither to reference another
EPT paging-structure entry nor to produce a physical address." And the note at
`.references/sdm.txt:205481`: "If the 'mode-based execute control for EPT'
VM-execution control is 1, an EPT paging-structure entry is present if any of
bits 2:0 or bit 10 is 1." Bit 10 counts **only** when that control is 1, and it
never is here.

**Why it matters in practice.** Bits 62:3 of a not-present EPT entry are
ignored by hardware, so a hypervisor is free to store software metadata in
them, and bit 10 is inside that range. A guest hypervisor doing so is handed a
misconfiguration exit for its own bookkeeping.

**Could it produce the stall?** No - it produces exit reason 49, and the record
under review is reason 48. It also cannot produce the *current* symptom, which
carries no reflected misconfiguration.

**One measurement.** `l2_ept_dispositions[cpu][reflected_misconfiguration]`
already counts it (`nested_entry.cpp:2929`). Non-zero on a boot where Hyper-V
is running means this is live.

**Fix shape.** Thread the mode-based-execute setting into `present()` the way
`normalised()` already threads `execute_only_supported`, or - simpler, and
truer to what vmcs02 is programmed with - stop reading bit 10 into
`ept_permissions` at all while the control is unconditionally cleared.

---

### 6. vmcs12's VMCS-link pointer is never checked

**Severity: low. Cannot explain the stall.**

**What we do.** `build_vmcs02` writes vmcs02's link pointer to all-ones
(`nested_entry.cpp:1433`) and never reads vmcs12's.

**What KVM does.** `nested_vmx_check_vmcs_link_ptr`
(`.references/kvm/nested.c:3082-3110`): if the pointer is not `INVALID_GPA`, it
must be a valid page address, the region's revision id must be
`VMCS12_REVISION`, and its `shadow_vmcs` flag must agree with
`nested_cpu_has_shadow_vmcs(vmcs12)`. Failure sets
`*entry_failure_code = ENTRY_FAIL_VMCS_LINK_PTR` - a VM-entry failure with exit
qualification 3, not a VMfail.

**Consequence.** A guest hypervisor that sets a link pointer expecting the
entry to fail gets a successful entry instead. Since we do not offer VMCS
shadowing to the guest hypervisor, nothing else acts on it, so the cost is a
wrong answer rather than a wrong execution.

---

### 7. Smaller divergences in `l1_wants_l2_exit`, listed once

None of these can produce the stall; all are cases where we reflect an exit
KVM keeps.

| exit reason | KVM | us |
|---|---|---|
| `MCE_DURING_VMENTRY` | L0 takes it (`nested.c:6358`) | not named in `l0_wants_l2_exit`; `default:` reflects (`nested_entry.cpp:2086`) |
| `NOTIFY` | never exposed to L1 (`nested.c:6525-6527`) | `default:` reflects |
| `UMWAIT` / `TPAUSE` | gated on `SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE` (`nested.c:6519`) | `default:` reflects |
| `BUS_LOCK`, `PML_FULL`, `VMFUNC` | L0 takes them (`nested.c:6378-6392`) | `default:` reflects |

Every one of them is unreachable today because the corresponding control is
withheld by `nested_vmx::supported_secondary_controls`, or because we never set
it in `vmcs01`. Machine-check-during-VM-entry is the exception: it needs no
control, and reflecting it hands a guest hypervisor a machine check it cannot
act on. Worth naming in the `l0_wants_l2_exit` switch, if only so the choice is
visible.

Also worth recording as **agreement**, since they were checked and are fine:

- the `nested_vmx_check_eptp` list, which `build_vmcs02:906-978` implements
  against the *reported* capability bits rather than constants, matching
  `.references/kvm/nested.c:2790`;
- `nested_vmx_check_nmi_controls` (`nested.c:2777`), implemented at
  `nested_entry.cpp:832-841`;
- `nested_check_guest_non_reg_state` (`nested.c:3115-3123`), implemented at
  `nested_entry.cpp:2479-2483` with the identical three-state set;
- `nested_vmx_exit_handled_cr`'s four cases, `_io`'s per-byte walk and wrap
  rule, and `_msr`'s bitmap indexing, all implemented at
  `nested_entry.cpp:1849-2084` and in several places *more* precisely than KVM
  (we apply vmcs12's page-fault error-code mask and match to vector 14, where
  `nested_vmx_l1_wants_exit` short-circuits `is_page_fault` to `true`,
  `nested.c:6421-6422`).

One check KVM makes that is worth adding and costs nothing:
`CC(nested_cpu_has_vpid(vmcs12) && !vmcs12->virtual_processor_id)`
(`nested.c:2869`). We ignore vmcs12's VPID entirely and run the second-level
guest on `vpid01` (`nested_entry.cpp:1429`), compensating with
`nested_transition_flush`, so a vmcs12 asking for VPID 0 is accepted where a
processor would refuse.

---

## The six questions, answered directly

### 1. Which vmcs12 fields does KVM write on a reflected exit, and what do we miss?

KVM's write set is `sync_vmcs02_to_vmcs12` (`.references/kvm/nested.c:4516-4584`)
plus `prepare_vmcs12` (`:4597-4642`), driven from `nested_vmx_vmexit`
(`:4911`), plus - for a nested EPT violation specifically -
`vmcs12->guest_physical_address = fault->address` written by
`nested_ept_inject_page_fault` **after** the vmexit (`:460`).

Field by field against `save_l2_state` (`nested_entry.cpp:2103`) and
`reflect_l2_exit` (`:2607`):

| KVM writes | we write | verdict |
|---|---|---|
| `guest_cr0`/`guest_cr4` through the masks (`:4526`) | `:2190`, `:2197` | same, and we additionally strip CR4.VMXE |
| `guest_rsp`/`rip`/`rflags` (`:4529`) | `:2114-2116` | same |
| `guest_cs_ar_bytes`, `guest_ss_ar_bytes` (`:4533`) | whole `guest_state_fields` list (`:390`) | we write strictly more |
| `guest_interruptibility_info` (`:4536`) | `:2178` | same, plus we clear the STI/MOV-SS bits when the activity state is not active (`:2174`) - KVM has no equivalent |
| `guest_activity_state` from `mp_state` (`:4539-4544`) | from `l2_activity_state` when not `running_l2` (`:2137`) | same shape, cited in our comment |
| `guest_cr3` + PDPTEs (`:4560-4567`) | `guest_state_fields` | same |
| `guest_linear_address` (`:4570`) | `:2757` | same |
| `vm_entry_controls` IA32E bit (`:4575`) | `:2204` | same |
| `guest_dr7`, `guest_ia32_efer` on the save controls (`:4579-4583`) | `:2213-2227`, plus IA32_PAT | we write more |
| `vm_exit_reason`, `exit_qualification` (`:4602-4605`) | `:2742-2743` | same |
| `launch_state = 1` (`:4613`) | `:2750` | same |
| `vm_entry_intr_info_field` valid bit cleared (`:4617`) | `:2787` | same |
| `idt_vectoring_*` via `vmcs12_save_pending_event` (`:4623`) | copied from hardware (`:2778-2781`) | different mechanism, same result; ours is the processor's own account |
| `vm_exit_intr_info`, `vm_exit_instruction_len`, `vmx_instruction_info` (`:4626-4628`) | `:2761-2768` | same |
| `vm_exit_intr_error_code`, only for an exception with an error code (`nested.c:6576-6581`) | unconditionally (`:2763`) | ours writes a field the SDM leaves undefined; harmless, but it is a value a guest could come to depend on |
| MSR-store area (`:4636`) | `:2804` | same, including the abort-as-triple-fault answer |
| `guest_physical_address` (`nested.c:460`) | `:2759` | same |
| **`kvm_mmu_invalidate_addr` + `nested_ept_invalidate_addr`** (`x86.c:1001`, `nested.c:455`) | **nothing** | **Finding 3** |
| `mp_state = KVM_MP_STATE_RUNNABLE` (`:5051`) | `l2_activity_state = active` (`:2465`, `:2577`) | same |

Interruptibility and activity state, which the brief called out specifically,
are both written and are in fact handled more carefully here than in KVM, because
`save_l2_state:2144-2176` enforces the SDM 29.3.1.5 pairing that KVM never has
to (it never puts hardware into those states). The exit qualification's
construction is Finding 4's subject and is otherwise correct: SDM Table 30-7
bits 5:3 from the guest hypervisor's own walk, bits 2:0 and 7:8 from hardware,
which is exactly KVM's split (`paging_tmpl.h:491-514` and `nested.c:441-444`).

**Nothing KVM writes into vmcs12 is missing from ours.** The only thing on the
reflect path KVM does and we do not is the invalidation, and Finding 3 explains
why that cannot be the withdrawn symptom.

### 2. How does KVM decide an L2 EPT violation belongs to L1?

In two stages, and the first is unconditional.
`nested_vmx_l0_wants_exit` returns `true` for `EXIT_REASON_EPT_VIOLATION` and
`EXIT_REASON_EPT_MISCONFIG` always (`.references/kvm/nested.c:6360-6375`), with
the comment "L0 always deals with the EPT violation. If nested EPT is used, and
the nested mmu code discovers that the address is missing in the guest EPT table
(EPT12), the EPT violation will be injected with nested_ept_inject_page_fault()".
`l0_wants_l2_exit` (`nested_entry.cpp:1685-1692`) reaches the identical
conclusion with the identical reasoning, and `on_l2_exit` routes both reasons to
`on_l2_ept_fault` before consulting either predicate (`:3342-3349`).

The second stage is the L1-EPT walk. KVM reflects **only** when
`FNAME(walk_addr_generic)` fails - a not-present entry (`paging_tmpl.h:417`), a
reserved-bit entry (`:420`, which becomes `EXIT_REASON_EPT_MISCONFIG` at
`nested.c:437-439`), a `permission_fault` on the accumulated `pte_access`
(`:436`), or a table page it cannot read (`:396-406`). That is the same four-way
split as `compose_ept` (`nested_ept.h:582-637`) plus the `permits()` test
(`nested_entry.cpp:3094-3130`), with one deliberate and correct addition on our
side: the "guest hypervisor maps it but our own tables refuse it" case, which
has no analogue in KVM because KVM's L0 has no watched pages.

**Where the two disagree:**

- *KVM handles at L0 where we reflect*: nowhere, except Finding 4's unreadable
  table page - and even there KVM reflects too, just with different
  qualification bits.
- *We handle at L0 where KVM reflects*: nowhere. `host_denied` and the
  `composed`-but-we-refuse case are both ours by construction.
- *KVM emulates where we stop the processor*: Question 3.

The ordering of the two permission tests at `nested_entry.cpp:3122` and `:3134` -
the guest hypervisor's own permissions first, ours second - is right and the
comment above it explains why better than KVM's code does.

### 3. An address L1 maps but L0 cannot translate

**KVM emulates the instruction. It never reflects.**

`FNAME(page_fault)` (`paging_tmpl.h:816`) calls `kvm_faultin_pfn` after a
successful L1-EPT walk. With no memslot for the resulting L1 GPA,
`kvm_faultin_pfn` (`mmu.c:4463-4464`) calls `kvm_handle_noslot_fault`
(`mmu.c:3308-3345`), which installs an MMIO SPTE and returns
`RET_PF_CONTINUE`, or returns `RET_PF_EMULATE` outright when MMIO caching is
off or the gfn exceeds `kvm_mmu_max_gfn()`. `kvm_mmu_page_fault` then reaches
`x86_emulate_instruction` (`mmu.c:6162-6164`). Forward progress is guaranteed by
the emulator, not by the tables.

**We do not have that answer.** `host_ept_lookup` (`nested_ept.cpp:112-128`)
reports `not_present` for anything at or above 512 GB, `compose_ept` turns that
into `host_denied` (`nested_ept.h:608-610`), and `on_l2_ept_fault:3289` hands it
to `on_ept_violation`; if nothing watches the page, `on_unhandled_exit` **stops
the processor** (`nested_entry.cpp:3290-3299`). BACKLOG.md already records this
as a limit rather than a design, and the code says so at `:3286-3288`.

**A failed read of L1's own EPT tables** is the same in both: KVM `goto error`
(`paging_tmpl.h:397`, `:402`, `:406`) and reflects; we report `not_present` and
reflect. Finding 4 is about the *indistinguishability*, not the policy.

### 4. Can the same L2 access fault for ever in KVM, and what stops it?

It can, and KVM has four separate mechanisms against it. We have none of them -
we have a *detector*, which is not the same thing.

1. **The instruction emulator.** `RET_PF_EMULATE` (`mmu.c:6159-6164`) is the
   universal escape: any fault the tables cannot resolve is retired by executing
   the instruction in software. Every other mechanism is an optimisation on top
   of it.
2. **The explicit RIP+address guard.** `kvm_mmu_write_protect_fault`
   (`mmu.c:6023-6025`) returns `RET_PF_EMULATE` when
   `last_retry_eip == kvm_rip_read(vcpu) && last_retry_addr == cr2_or_gpa`,
   under the comment "doing so will likely put the vCPU into an infinite [loop]".
   This is the closest analogue to `l2_ept_fault_repeats` - and where ours only
   logs at the threshold (`nested_entry.cpp:2936-2956`), KVM's *changes what
   happens*.
3. **Staleness detection.** `is_page_fault_stale` (`mmu.c:4552-4579`) plus the
   `mmu_invalidate_seq` snapshot taken in `kvm_faultin_pfn` (`mmu.c:4450`) make a
   fault raced by a memslot change retry rather than install a wrong mapping.
4. **Write-flooding.** `detect_write_flooding` (`mmu.c:5880-5890`) zaps a shadow
   page after three emulated writes to it, so a guest writing its own page
   tables stops paying for shadowing rather than looping.
   `mmu_sync_children` carries an explicit forward-progress guarantee
   (`paging_tmpl.h:688-691`).

What we lack, named: **any action at all on repetition**. `l2_ept_stall_threshold`
is 512 (`hypervisor.h:6213`) and reaching it writes one log line and one record.
`shadow_ept_leaves_that_did_not_help` (`nested_entry.cpp:3247`) detects the
specific "installed a leaf that still refuses the access" case and also only
logs. That is the right first move - the project's own note says the last two
livelocks were found by hand days later - but the second move, a behaviour that
guarantees progress, does not exist, and cannot exist without either an
instruction emulator on this path or a policy of reflecting a distinguishable
failure to the guest hypervisor.

### 5. APIC-access page and virtual APIC under nested EPT

**KVM refuses to map L1's APIC-access page into L2 at all.** `kvm_faultin_pfn`
(`mmu.c:4474-4488`):

```c
if (slot->id == APIC_ACCESS_PAGE_PRIVATE_MEMSLOT) {
        /*
         * Don't map L1's APIC access page into L2, KVM doesn't support
         * using APICv/AVIC to accelerate L2 accesses to L1's APIC,
         * i.e. the access needs to be emulated. [...]
         */
        if (is_guest_mode(vcpu))
                return kvm_handle_noslot_fault(vcpu, fault, access);
```

so an L2 access to that GPA becomes an MMIO SPTE and is emulated by L0. For
the controls themselves, `nested_vmx_check_apic_access_controls`
(`nested.c:772-780`) validates `vmcs12->apic_access_addr` when
`SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES` is set, and
`nested_vmx_check_apicv_controls` (`nested.c:782-825`) enforces the four
cross-control rules, including "if virtualize x2apic mode is enabled,
virtualize apic access must be disabled" and "tpr shadow is needed by all apicv
features". `nested_vmx_l1_wants_exit` returns `true` unconditionally for
`APIC_ACCESS`, `APIC_WRITE` and `EOI_INDUCED` because "the controls [...] only
come from vmcs12" (`nested.c:6494-6502`).

**What we do.** None of virtualize-APIC-accesses, APIC-register
virtualization, virtual-interrupt delivery or virtualize-x2APIC-mode is offered
in `nested_vmx::supported_secondary_controls`, and `build_vmcs02`'s capability
check (`nested_entry.cpp:811-826`) refuses a vmcs12 that sets one. So none of
KVM's checks above have anything to guard, and the three APIC exit reasons
cannot occur. That is a coherent position, and `vmcs12_secondary_asked` versus
`vmcs02_secondary_written` (`nested_entry.cpp:1353-1357`) exists precisely to
catch a guest hypervisor asking for one anyway - read those two words on the
next boot before concluding anything about the APIC.

The TPR shadow **is** offered, and its three outcomes at
`nested_entry.cpp:998-1102` and `:1204-1255` match
`nested_get_vmcs12_pages`'s fallback and `prepare_vmcs02_early`'s forcing of
`CR8_LOAD_EXITING | CR8_STORE_EXITING`, which the comments cite correctly.

The relevant fact for the surviving symptom is the opposite of KVM's concern:
the guest is in **x2APIC** mode (the record shows writes to MSRs `0x834` and
`0x836`), so there is no APIC-access page in play and the LVT writes are MSR
accesses routed through the merged bitmap. What that means is that
`l0_wants_l2_exit`'s deliberate omission of MSR exits
(`nested_entry.cpp:1705-1732`) sends the second-level guest's x2APIC traffic to
the guest hypervisor without this VMM seeing it - which is the documented and
correct choice, but it does mean **the local-APIC bookkeeping in
`local_apic.cpp` observes nothing a second-level guest does**. If the stall
turns out to involve an inter-processor interrupt the root partition sends and
never sees delivered, that is where to look, and it is a different review.

### 6. `nested_vmx_check_*` we do not make

Ordered by what a guest could do with the gap.

| KVM check | where | do we? | what a guest gets |
|---|---|---|---|
| `nested_vmx_check_host_state`, all of it | `nested.c:3013` | only the address-space-size bit (`nested_entry.cpp:879`) | **halts the processor** - Finding 2 |
| `nested_vmx_check_vmcs_link_ptr` | `nested.c:3082` | no | entry succeeds where a processor fails it - Finding 6 |
| `nested_cpu_has_vpid(vmcs12) && !virtual_processor_id` | `nested.c:2869` | no | VPID 0 accepted |
| `cr3_target_count > nested_cpu_vmx_misc_cr3_count` | `nested.c:2858` | no (we report 0 targets and write 0) | entry succeeds where a processor fails it |
| `page_address_valid` on the I/O and MSR bitmap addresses | `nested.c:523`, `:536` | only implicitly, via `read_guest_physical` range-checking in `merge_nested_bitmaps` (`nested_entry.cpp:727-749`) | a **misaligned** bitmap address is accepted and read across a page boundary |
| `nested_check_vm_entry_controls` event-injection consistency | `nested.c:2914-2977` | no - the injection is passed to vmcs02 (`nested_entry.cpp:1548-1556`) and hardware checks it | correctly reflected as an entry failure; arguably better than KVM |
| `nested_vmx_check_guest_state`'s CR0/CR4 fixed bits, DR7, PAT, BNDCFGS, EFER | `nested.c:3125-3186` | no - hardware checks them on the vmcs02 entry | correctly reflected; but note we *modify* the values first (`guest_cr4((cr4_12 \| cr4_vmxe) & ~cr4_smxe)`, `nested_entry.cpp:1487`), so a vmcs12 that hardware would refuse for VMXE or SMXE is silently repaired |
| `nested_vmx_check_apic_access_controls`, `_apicv_controls`, `_pml_controls`, `_shadow_vmcs_controls`, `_mode_based_ept_exec_controls`, `_unrestricted_guest_controls` | `nested.c:772`, `:782`, `:865`, `:896`, `:887`, `:878` | n/a | the controls are withheld by the capability MSRs and refused by `within_capability` (`nested_entry.cpp:811`) |

The general pattern is sound and worth stating: **we delegate guest-state checks
to hardware and reflect the entry failure, which is both correct and cheaper
than transcribing them.** That delegation works for everything the processor
loads from vmcs02. It does not work for the host-state area, because vmcs02's
host state is ours - which is exactly why Finding 2 is the one gap in this
family that matters.

---

## Summary

1. **`translate_guest_linear` is wrong for a second-level guest** - it reads L2
   guest-physical addresses as host-physical while emulating a watched-page
   write. The watched pages are the local APIC and the NVMe registers, and the
   guest stalls just before the storage stack. `hypervisor.cpp:2807`,
   `:2857`; reached from `nested_entry.cpp:3135`, `:3289` via
   `watched_page.cpp:295`.
2. **vmcs12's host state is never validated**, and it is the one area hardware
   cannot validate for us, so a malformed one halts a processor.
   `nested_entry.cpp:879` versus `.references/kvm/nested.c:3013`.
3. **No shadow invalidation on a reflected EPT violation**, where KVM does two -
   with a comment naming the exact failure. `.references/kvm/x86.c:996-1002`,
   `nested.c:455`. A real gap that provably cannot explain the record it was
   commissioned for.

And, above all of them: the symptom this was commissioned to explain does not
exist. `ae6e873` withdrew it before this review was written, and the surviving
question - what the root partition is waiting for after arming synthetic timer
0 - is not answered by anything in the nested EPT path.
