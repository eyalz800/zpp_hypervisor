# Working in this repository

Read [CLAUDE.md](CLAUDE.md) for the project's build, architecture, testing,
rig and contribution instructions. They apply to Codex as well. Read
[WINDOWS-BOOT-STATUS.md](WINDOWS-BOOT-STATUS.md) for the current boot investigation,
then the relevant recent entries in [BACKLOG.md](BACKLOG.md).

The backlog, README and reference notes contain historical conclusions that
were later corrected. Check later corrections, current source and live rig
state before treating a claim as established. Do not replace Hyper-V nesting
with a configuration in which it stands down to obtain a successful boot.

For rig work, use the procedures in `.claude/skills/boot-windows-rig/`,
`.claude/skills/read-resident-state/` and `.claude/skills/dump-log-physical/`.
Use `scripts/rig-boot.sh` and `scripts/rig-kill-qemu.sh`; preserve the Windows
installation and Limine recovery loader. Resolve resident offsets from
`.rig-deployed-hypervisor.elf`, and allow only one monitor reader at a time.
