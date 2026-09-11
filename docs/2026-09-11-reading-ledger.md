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
- All nine Markdown files in
  `.superpowers/sdd/2026-08-03-cmake-modernization/`.
- `docs/superpowers/plans/2026-08-03-cmake-modernization.md`:
  its lines 44–963 are verified exact copies of the four task briefs
  already read; its remaining lines 1–43 and 964–1020 were read separately.

Corrections that matter when continuing:

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
