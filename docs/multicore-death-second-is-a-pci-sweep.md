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

## What immediately precedes the sweep

Located by walking back from the end of the log to the burst's first
line (627,100 of 644,848). The 25 events before it are all this, on
cpu 0:

    memory_region_ops_read cpu 0 addr 0x608 value 0x217973 name 'acpi-tmr'
    memory_region_ops_read cpu 0 addr 0x608 value 0x217981 name 'acpi-tmr'
    ... 23 more, monotonically increasing

That is the ACPI PM timer being polled in a tight loop - 0x217973 to
0x217ad0 is 349 ticks of a 3.579545 MHz counter over 25 reads, about
4 us per read and ~97 us total. **cpu 0 is in a timed stall**, which
agrees with its post-mortem RIP sitting in hvix64's
`HvpStallExecutionMicroseconds` on every boot.

The sweep then begins cleanly at the bottom of the bus:

    addr 0xe0000000 value 0x8086     size 2   (vendor id, 00:00.0)
    addr 0xe0000000 value 0x8086     size 2
    addr 0xe0000000 value 0x8086     size 2
    addr 0xe0000000 value 0x29c08086 size 4   (vendor+device)
    addr 0xe0000004 value 0x7        size 4   (command/status)
    addr 0xe0000008 value 0x6000000  size 4   (class code)

ECAM base here is 0xe0000000, so `bus = (a>>20)&0xff` etc. decode
directly off the traced address.

So the order is: cpu 0 stalls on the PM timer, then a full enumeration
starts from 00:00.0, then everything stops.

## Still open

Whether the sweep is Windows/PnP re-enumerating, or firmware.

**The "two boot cycles" worry is REFUTED - measured, do not re-raise.**
An earlier revision of this file reasoned that four `allocate_rwx`
markers meant two boot cycles, since one boot gives two. Sampling the
marker count through a single run settles it:

    t=45s  markers 4  VM status: running
    t=75s  markers 4  running
    t=105s markers 4  running
    t=135s markers 4  paused (shutdown)
    t=165s markers 4  paused (shutdown)

Four from the first sample and never changing, so the loader simply
emits four in this configuration and the machine boots **once**. The
sweep belongs to that single boot. The run also brackets the death
between 105 s and 135 s.

## Where the guest actually is at the wall (named, 2026-09-04)

The wall is a fixed point at ~21,25x `HvCallVtlCall` (21,251 / 21,248 /
21,255 over three boots) while cpu 0's exits vary 14% and cpu 1's
second-level entries vary sevenfold. So it is a guest-side milestone.

The dump already walks the second-level call stack and had never been
read at THIS wall - only at the single-processor one. Symbolised through
`scripts/guest-securekernel-syms.py` against `ntkrnlmp.pdb` with the
kernel base out of zpp's own log:

    KiSystemStartup+0x283
      KiInitializeKernel+0x805
        InitBootProcessor+0x127
          ExpRevokeBootLoaderPagePrivileges+0x50
            KeSetPagePrivilege+0x2c
              VslRemoveProtectedPage+0x5c
                VslpEnterIumSecureMode+0x3a8
                  HvlSwitchToVsmVtl1+0xab        <- current

`KiSelectIdealProcessorSetForGroup+0x112` also appears in the walk,
which is processor-topology work that only exists above one CPU.

**The guest is revoking the boot loader's page privileges**, one trust
level switch per page, inside `InitBootProcessor` - very early kernel
initialisation, long before anything a login screen needs. A loop over a
fixed number of pages is exactly why the VTL count is reproducible to
0.03%: it is counting pages, not time.

Note the shape rivals the single-processor wall recorded in memory
("~37,15x VTL calls and exactly 12,387 copies"), which is also a
page-count loop. This one is EARLIER and different.

Frame-walk caveat: the walk is a raw stack scan, so it carries stale
values too - `KiIdleLoop`, `wil_details_FeatureReporting_*` and repeated
`+0xfd25c0` entries are noise. The chain quoted above is the part that
is self-consistent, and it is consistent across the run.
