# The multicore death second is a full PCI enumeration sweep

Measured 2026-09-04, boot 85, 2 vCPUs, zpp resident, from QEMU's side.

## How to reproduce the measurement

`scripts/rig-boot.sh` appends `$ZPP_QEMU_EXTRA` verbatim, so:

```sh
ZPP_CPUS=2 ZPP_QEMU_EXTRA='-trace enable=memory_region_ops_read \
  -trace enable=memory_region_ops_write -D /tmp/qemu-trace.log' \
  ./scripts/rig-boot.sh
```

This is QEMU's own **user-space** tracing. It is unrelated to the
ftrace/`trace_pipe` capture that corrupts kernel memory on this rig, and
it is safe. One boot produced 644,848 events.

There is no `python3` on the rig - copy the log back and analyse here.

## What it says

By region, the whole run against the last 20,000 events (the death
window):

| region | whole run | last 20,000 |
|---|---|---|
| `acpi-tmr` | 556,204 | 3,272 |
| **`pcie-mmcfg-mmio`** | **22,439** | **16,551** |
| `serial` | 37,297 | 0 |
| `ioapic` | - | 131 |
| `hpet` | - | 15 |

74% of every ECAM access in the entire boot happens in the second the
machine dies. Decoding those 16,551 addresses (`bus = (a>>20)&0xff`,
`dev = (a>>15)&0x1f`, `fn = (a>>12)&7`, `off = a&0xfff`):

    distinct BDFs touched              8,206
    accesses to config offset 0x0     16,466  of 16,551
    busiest: 00:1f.0 x30, 00:05.0 x29, 00:01.0 x26, then 12 each

Offset 0 is the vendor/device ID register. 8,206 distinct
bus:device.function addresses at about two reads each is not a driver
touching a device - it is a probe of **every possible BDF to discover
what exists**. The guest performs a complete PCI enumeration sweep in
the second it dies, having issued no MMIO at all for the preceding ~100
seconds.

## Why it matters

This tree already records, as a rule paid for once, that reading the
configuration space of a dead passed-through device hangs this rig. A
sweep that reads offset 0 of every BDF does exactly that, and the
machine stops immediately afterwards with every KVM counter frozen
together and no reset port ever written.

## What this eliminates

- **VT-d.** zpp traps the DMAR register page (`nvtd=1`) and absorbed
  **2 reads** on the whole boot, where the hypothesised GSTS poll would
  have shown thousands. `dmar_reads[]`/`dmar_writes[]` had never been
  printed before this - the fourth member in this tree recorded
  faithfully and never read out.
- **Unassigned or unimplemented MMIO.** A boot with
  `-d guest_errors,unimp` produced an **empty** log, so every access in
  the burst hit a modelled region.
- **Both of hvix64's reset writes.** 0xcf9 and 0x64 are both armed in
  zpp's I/O bitmap and recorded; neither is ever written, so
  `HvpResetSystem` did not run.

## Open

What makes the guest sweep PCI at that instant, and whether the sweep
causes the stop or is the last thing an already-dying guest does. Both
are testable against the same trace, which records the sweep's first
access and everything before it - that region of the log has not been
read yet.
