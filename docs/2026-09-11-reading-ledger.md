# Documentation reading ledger

This is an incremental handoff for the request to read the repository's
Markdown. The full corpus is **not yet read**. Existing notes include
superseded diagnoses and old implementation plans; reading one is not an
instruction to repeat its experiment. Current operating instructions are
in `CLAUDE.md` and `AGENTS.md`; current boot evidence is in
`WINDOWS-BOOT-STATUS.md`.

Completed during the later September 11 investigation:

- `docs/hyperv-mmio-burst-and-non-cf9-reset.md` (671 lines).
- `docs/hyperv-ap-timeout-and-reset-paths.md` (807 lines).
- `.references/hyperv/posted-interrupt-descriptor-reads.md`.
- `.references/hyperv/hvix64-current-vp-lookup.md`.
- `.references/hyperv/secure-lock-address-and-vtl0-silence.md`.
- `.references/hyperv/vbs-graceful-degrade.md`.
- `.references/hyperv/vbs-phase1-dma-stall.md` (166 lines).
- `.references/hyperv/securekernel-fault-d93a4.md`.
- `.references/hyperv/vp1-pmtimer-convergence.md`.
- `.references/hyperv/nvme-io-queue-submission.md`.
- `.references/hyperv/NOTES.md`.
- `.references/hyperv/c1-msix-routing.md`.
- `.references/hyperv/vp-flags-188-not-run-state.md`.
- `.references/hyperv/zpp-iommu-vtl-switch.md` (199 lines).
- `.references/hyperv/secure-dma-hvcall.md` (all 2,337 lines).
- `REGRESSION-COVERAGE.md` (all 590 lines).
- `.references/hyperv/dispatch-level-pin.md` (237 lines).
- `.references/hyperv/clock-double-injection.md` (303 lines).
- `.references/hyperv/timer-resolution-worker.md` (351 lines).
- `.references/hyperv/phase1-timer-stall.md` (330 lines).
- `.references/hyperv/smss-to-logonui.md` (277 lines).
- `.references/hyperv/phase1-barrier-unmask-point.md` (240 lines).
- `.references/hyperv/timer-config-reentry.md` (341 lines).
- `.references/hyperv/phase1-tail-to-smss.md` (285 lines).
- `.references/hyperv/storage-stall-worker-thread.md` (388 lines).
- `.references/hyperv/clock-preemption-race.md` (195 lines).
- `.references/hyperv/ntdll-to-smss.md` (256 lines).
- `docs/bare-metal-windows.md` (319 lines).
- `.references/hyperv/ntoskrnl.md` (492 lines).
- `.references/hyperv/eoi-delivery-adversarial.md` (165 lines).
- `.references/hyperv/posted-interrupt-fix-design.md` (215 lines).
- `.references/hyperv/ap-post-handshake.md` (87 lines).
- `.references/hyperv/scheduler-runnable-predicate.md` (227 lines).
- `.references/hyperv/scheduler-idle-decision.md` (218 lines).
- `.references/hyperv/idle-halt-wake-path.md` (278 lines).
- `.references/hyperv/vpassist-coherency-contract.md` (176 lines).
- `.references/hyperv/vpassist-coherency-zpp-side.md` (234 lines).
- `.references/hyperv/vtl1-vpassist-perVP.md` (110 lines).
- `.references/hyperv/ium-workitem-lost-wakeup.md` (259 lines).
- `.references/hyperv/lazy-eoi-and-interrupt-window.md` (290 lines).
- `.references/hyperv/vapic-reinjection-loop.md` (229 lines).
- `.references/hyperv/crash-rendezvous-barrier.md` (246 lines).
- `.references/hyperv/ap-lp-apic-id.md` (105 lines).
- `.references/hyperv/device_interrupt_delivery.md` (305 lines).
- `.references/hyperv/vp-run-state.md` (183 lines).
- `.references/hyperv/live-read-unblock.md` (105 lines).
- `.references/hyperv/ept-kvm-faithful.md` (154 lines).
- `.references/hyperv/hvix64-nvme-passthrough.md` (129 lines).
- `.references/hyperv/vtl-block-deadlock.md` (265 lines).
- `.references/hyperv/multi-cpu-vbs-vtl1.md` (115 lines).
- `.references/hyperv/vtl1-first-ap-resource.md` (103 lines).
- All nine Markdown files in
  `.superpowers/sdd/2026-08-03-cmake-modernization/`.
- `docs/superpowers/plans/2026-08-03-cmake-modernization.md`:
  its lines 44–963 are verified exact copies of the four task briefs
  already read; its remaining lines 1–43 and 964–1020 were read separately.

Corrections that matter when continuing:

- The VTL block note retracts its original lock address and causal ranking;
  some embedded assembly comments still contain the old address. An
  unchanged nonzero lock word need not mean a holder (the note itself
  describes marker 1), and a stable unused register is not proof of a
  deliberate protocol field. Validate state and ABI at the actual boundary.
- The multi-CPU VTL1 notes are historical ranked hypotheses. An absent
  VM-entry failure does not prove that a particular VTL1 entry executed;
  a sampled VTL-call return site does not locate the failure by itself.
  Their APIC/SynIC changes are not selected fixes for this two-CPU boot.
  Inner VTL resources are served by Hyper-V; do not infer zpp owns those
  resources merely because their MSR numbers also exist at another layer.


- The old EPT-parity note anchors 2d93273 and correctly asks for the full
  deployed manifest. Two eager switches being off does not exonerate all
  EPT behavior or prove a watchdog must be an interrupt bug. A linear
  data sweep can still revisit code/page-table pages; subtree reuse costs
  and fault populations need measurements, not that ranking alone.
- The NVMe note's absent strings do not prove an absence of emulation,
  and one functioning admin queue does not establish every I/O queue's
  mapping or completion. Its inert-inner-IOMMU premise is superseded by
  current nested VT-d support. Doorbell, DMA and interrupt claims require
  the actual mappings and request/completion sequence; the note explicitly
  had no rig ownership and does not establish this run's failure.


- The VP run-state note derives start/suspend fields from setters but
  explicitly leaves scheduler consumers and other writers unresolved.
  A start mask alone does not prove runnable state; a nonzero suspend
  byte can be a normal in-progress call, not a permanently missed clear.
  Its fixed addresses belong to its historical boot.
- The live-read note requires matching Hyper-V identity and linkage, but
  overstates what follows. Equal assist-page bytes alone do not establish
  equal backing; unequal asynchronous reads may reflect intervening writes.
  A lazy-EOI snapshot can precede normal reconciliation. Its base/alignment
  shortcut must pass actual PE/RSDS and translation checks on each boot.


- The APIC-ID note maps an old Hyper-V initialization check; its expected
  LP ID must be compared in the selected APIC mode at that check. A later
  matching read does not establish that an earlier comparison passed.
  Its historical CPU/LP example is not the current two-CPU topology.
- The device-delivery note explicitly contains no rig measurements. Its
  conditional posted-interrupt paths and EOI bitmap setters do not prove
  which VMCS controls the current guest enabled, and its pending/in-flight
  snapshots do not prove an event was never drained. Its categorical claim
  that no enlightenment can relax the DPC watchdog is superseded by the
  later UseRelaxedTiming correction recorded below. Neither
  that old 0x133 diagnosis nor its proposed posted-interrupt implementation
  is established by the current tablet's independently unwound 0x9F path.


- The VBS Phase1 DMA note infers a repeated failed attach from static
  wrapper code and declares a loop it did not observe. Its own correction
  retracts the advertised-capability gate. Current nested VT-d support and
  later secure-DMA corrections supersede its inert-IOMMU premise; neither
  faking successful hypercalls nor clearing guest policy follows from it.

- The IUM work-item note's zero-list-link test does not establish a lost
  wakeup: an item can be dequeued/executing, no work may be pending, or a
  later producer may queue it. Establish pending work and the producer /
  consumer ordering before claiming that nothing can call service d2.
  Its wait-reason/queue fields likewise need the actual stopped stack;
  the current SMSS capture separates the main subsystem wait from an
  asynchronous PnP worker using full unwinds.
- The lazy-EOI notes' zero TPR threshold does not imply an outstanding
  interrupt-window request: their own clear routine clears both. Read the
  actual primary controls and blocked condition. A nonempty ISR stack can
  describe an interrupt currently in service; a cleared assist bit can be
  a normal transient before reconciliation. Neither alone proves a lost
  EOI, and a clear pending bit does not prove that an interrupt never
  arrived. The old deadlock shapes remain hypotheses requiring a sequence.
- The crash-rendezvous note describes an old Hyper-V reset path, not this
  run's current state or the preceding directly captured Windows 0x9F.
  Its target/arrived sets must be validated at the comparison, including
  all words and headers; a later equality does not alone prove a broken
  comparison, and a missing arrival does not identify a nonexistent CPU.

- The bare-metal guide's old VMX-hiding/Hyper-V-stand-down phase is not
  the current nested-boot objective. Its old TinyCore/QEMU versions and
  password-only/no-SCP account are historical; the current rig uses QEMU
  11.0.3 and working BatchMode SSH. The latest loader archive also found
  legacy `scp -O` failing in Dropbear's multicall dispatch; direct SSH
  `cat` transferred the file successfully and its local MD5 matched.
- The ntoskrnl note retracts both its missing-binary claim and its claim
  that no boot enlightenment can relax the DPC watchdog. Read its later
  UseRelaxedTiming correction before relying on the early caller list.
  This is not authorization to alter the current clock/CPUID manifest.
- A PDB Info-stream age different from the image's CodeView age does not
  by itself establish a mismatch. Microsoft's OpenValidate4 checks the
  GUID, Info age >= image age, and matching nonzero DBI age. The services
  PDB has Info age 3 and DBI age 1 for an image age of 1; the NT and WDF
  PDBs inspected have Info ages 6/4 and DBI age 1. See the explicit
  validation and source in
  [the Winlogon/SCM note](2026-09-11-winlogon-scm-debug.md).

- The GDB/watchdog passages in CLAUDE.md and BACKLOG.md were checked against
  KVM's nested debug-exit handling. Direct Windows hardware breakpoints on
  September 11 supersede the old claim that the stub can never expose VTL0.
  Single-stepping and long stops retain their documented restrictions.

- These older timer/stack notes include unproven inferences even after their
  headline retractions. A plausible word above RSP is not by itself a live
  frame; the current work uses checked PE unwind metadata. Matching clock
  injection and ISR-entry counts alone does not establish the number of
  injections per timer expiry. The old copy-versus-entry inequalities also
  mix entry attempts with delivery and are not used as loss counts here.
- `smss-to-logonui.md` is a historical stage map, not proof that particular
  subsystems are permanently excluded or that a process count guarantees
  display readiness. Its early residency mistake was explicitly withdrawn.

- The older posted-interrupt/EOI reviews are explicitly anchored to
  2d93273 and a one-CPU device-vector stall. Their correct-looking code
  maps and counter comparisons do not prove that every delivery path is
  fault-free. A generic EOI count does not associate each EOI with the
  affected device request; unavailable hardware controls are a separate
  question from software delivery. Their recommendations to toggle old
  defaults do not supersede this investigation's fixed full manifest.
- The idle/scheduler notes revise the meaning of several VP fields, but
  still overstate what a short observation excludes. A breakpoint on
  HvlSwitchToVsmVtl1 not firing establishes no call at that site during
  the observed interval, not that VTL0 cannot be running other code.
  A halted CPU or old event-queue entries do not establish global system
  idleness or healthy delivery of every source. Revalidate these old
  private Hyper-V layouts and mode-specific paths before a new live read.
- The VP-assist coherency notes disagree in their headline confidence:
  one proposes a private-page alias, the later zpp-side note rejects that
  mechanism against its old code/map. Equal bytes alone do not prove
  identical physical backing; different bytes from separate live reads
  can be an intervening update. Validate the complete translations and
  memory types at one stopped state before calling a coherency defect.
  Likewise, per-VP allocation and registration code does not exhaust all
  causes of a later securekernel fault. The notes' rankings and claimed
  exclusions are not current rig evidence.
- The AP post-handshake note is an old eight-CPU investigation map, not
  evidence that current per-CPU VMCS/AP startup is broken. The present
  two-CPU stack reaches both Hyper-V and Windows; no change is selected
  from its ranking alone.

- The old IOMMU transparency note describes commit `2d93273`; it predates
  the resident nested VT-d implementation. Its claim that no resident
  IOMMU code exists is not a statement about the current tree.
- The secure-DMA note repeatedly revises its proposed object layouts,
  call graph and flag-poking recipes. Sections 25–26 retract the core
  claim that DMA-attach errors explain the protection-call plateau:
  device-domain attachment and bounded page-protection work are different
  paths. Its earlier recipes are not current recommendations. Later boot
  comparisons also show fresh VTL work on progressing guests.
- The AP/reset notes distinguish initial AP startup from resume/reset
  paths. The companion MMIO note corrects the CF9 account: the 8042 reset
  write is unconditional; the CF9 write is conditional. The later PCI-
  sweep note supersedes the earlier VT-d interpretation of bulk MMIO.
- The clock double-injection note explicitly retracts its original
  mechanism: clock-ISR entries and injections matched when compared using
  the right population. Its proposed event-bit fix was already implemented.
- The early Phase1 timer note infers a missing per-VTL contract without
  a live measurement. Hyper-V owns the inner VTL timers; identical MSR
  numbers alone do not demonstrate aliasing in zpp. Its later correction
  says EOM is conditional on MessagePending and empty-slot interrupts
  still enter Windows clock accounting.
- The timer-worker note predates the checked live unwinds. A frame at an
  interrupt-unmasking epilogue can be current without an internal loop or
  repeated requests. Its single-CPU clock-log discriminator must not be
  applied as a proof about one thread in a multiple-CPU guest.
- The ntdll-to-smss note gives useful call/phase landmarks, but its blanket
  exclusion of remaining hypervisor dependencies and its secure-call timing
  conclusions are not measurements of this run. Three processes alone do
  not locate execution inside RtlpCreateUserProcess; earlier phases also
  have three. No current thread location is inferred from that table.
- The regression-coverage inventory is historical: its claim of no Python
  tests and its old CI/harness counts no longer describe the tree. The
  latest rebuilt run contains 228 Python tests and 27 CTest entries. Its
  ranked recommendations are leads to verify, not a current work order.
- The first CMake implementation report used C++17 and host SDK headers;
  later implementation and current project instructions use C++26 and
  freestanding configuration. Do not restore the report's old workaround.

Known incomplete large reads include `BACKLOG.md` (over 80,000 lines;
selected relevant ranges and the current tail have been read). Many other
reference notes were read earlier in the session; this incremental list
does not claim to enumerate all of them. Third-party Markdown under the
reference tooling is also not covered by this ledger.
