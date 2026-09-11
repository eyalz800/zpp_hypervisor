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
- All nine Markdown files in
  `.superpowers/sdd/2026-08-03-cmake-modernization/`.
- `docs/superpowers/plans/2026-08-03-cmake-modernization.md`:
  its lines 44–963 are verified exact copies of the four task briefs
  already read; its remaining lines 1–43 and 964–1020 were read separately.

Corrections that matter when continuing:

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
