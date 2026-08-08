---
name: read-resident-state
description: Read the running hypervisor's own counters out of guest physical memory with the QEMU monitor, to tell "not produced" from "produced and not written". Use when the disk channel is silent, a guest looks stuck, or a counter on the medium disagrees with what the code should be doing.
---

# Reading resident state while the guest runs

The channel on the disk only tells you what reached the disk. When it goes
quiet, the question is always which of these it is:

- the guest stopped exiting,
- records are produced and not drained,
- records are drained and the device write fails.

All three look identical from the medium. They are trivially distinguished
by reading the resident side's own memory, and that needs no debugger.

## Procedure

1. **Get the module base for *this* boot.**

   ```sh
   ssh tc@RIG 'grep -aoE "allocate_rwx done at 0x[0-9a-fA-F]+" ~/zpp/serial.out | tail -1'
   ```

   Not a base from an earlier run. It moves when the module's size changes.

2. **Resolve symbols from the binary that is actually deployed.**

   ```sh
   NM=/opt/homebrew/opt/llvm/bin/llvm-nm
   $NM out/debug/x86_64/zpp_hypervisor | grep '^[0-9a-f]* b _ZZN3zpp10hypervisor10hypervisor8instanceEvE8instance$'
   $NM out/debug/x86_64/zpp_hypervisor | grep 'esp_blocks_forILNS0_4sinkE4EE'
   $NM out/debug/x86_64/zpp_hypervisor | grep 'queue_pairILj64EE5boundE'
   ```

   Member offsets inside the singleton:

   ```sh
   llvm-dwarfdump --name=hypervisor --show-children out/debug/x86_64/zpp_hypervisor \
     | grep -A4 'DW_AT_name.*heartbeat_exits_seen' | grep -E 'name|data_member_location'
   ```

3. **Read it, twice, with a gap.** A single sample of a counter says
   nothing; the question is nearly always whether it is advancing.

   ```sh
   { printf 'xp/8gx 0xADDR\n'; sleep 2; } | nc -w 12 RIG 4446
   ```

## What each reading means

| Read | Meaning |
|---|---|
| `heartbeat_exits_seen[cpu]` advancing | the guest is exiting; a silent channel is not the guest's fault |
| `ring_storage::written[cpu]` advancing | records are being produced |
| sink `sequence` frozen while `written` climbs | produced, not written - look at the device |
| `queue_pair<64>::bound` all zero | `forget()` ran; the guest reset the controller |
| block `epoch` = 2 on the medium | the queue pair was rebuilt after a reset |
| `configure_reject` = 1 | that is `reject::none`, i.e. success, not a rejection |
| gate `held` non-zero with nobody inside | a gate left held silences the channel permanently |

## Traps, each of which cost time

- **Symbol addresses move between builds.** Two builds one commit apart had
  the sink statics 0x1000 apart. Re-resolve every time. Reading a stale
  address returns plausible numbers, not obvious garbage.
- **The module base moves too**, and by a different amount. Base+offset can
  come out identical for two different (base, offset) pairs, which makes a
  wrong reading look self-consistent. Take both from the same run.
- **A zero counter in a block header is not evidence.** Every counter in the
  header - `records_lost`, `blocks_dropped`, `blocks_lost_to_reset` - only
  reaches the medium inside the *next* block. If the channel stopped, there
  is no next block, so they read as of the last success. Read them from
  memory when the question is what happened afterwards.
- **Never `stepi` the guest through the gdbstub.** KVM ORs `TF` into guest
  RFLAGS and the next `vmresume` takes an entry failure. Use the monitor.
- **A rotating low RIP is not a hang.** One CPU at a low address that is a
  *different* CPU each sample, with the rest parked at one kernel address,
  is an idle guest with a CPU inside the VMM. Check the address against the
  module base before concluding anything.
