---
name: dump-log-physical
description: Use when the hypervisor's log ring or any resident member has to be read out of a running guest and gdb cannot see it - a settled guest with every processor in its own idle loop, "Cannot access memory", or a gdb dump that keeps reporting no processor inside the module
---

# Reading resident state through physical memory

`scripts/rig-dump-log-physical.sh` dumps the log ring with **no debugger
and no processor in root operation**. Run it and read `/tmp/zpp.log`:

```sh
./scripts/rig-dump-log-physical.sh
```

## Why the gdb path is not enough

`scripts/rig-dump-log.sh` attaches gdb, and gdb reads through the
**current processor's page tables**. The module hides its own pages from
the guest - every extended-page-table permission cleared - so from guest
context every read answers `Cannot access memory` and the class type will
not resolve either, which looks like the module being gone.

It needs a processor inside the module, and **the interesting failure is
exactly when there is none**. A guest hypervisor that has settled leaves
eight processors halted in their own idle loop; forty attaches in a row
found nobody. That is not a flaky sample, it is the steady state.

The monitor's `xp` reads **physical** memory, bypassing the extended page
tables and guest paging both. The module is loaded at a physical address
it prints on serial and is identity mapped, so every pointer inside it is
usable as a physical address with no translation.

## The two things that make it fast enough

A ring of several hundred lines is a linked list, and one monitor round
trip per node is minutes of ssh latency. Neither of these is optional:

- **Bound the span from the list head, then dump it in a few large
  reads.** The head names the first node and the last, the allocator
  hands nodes out of one arena in order, so the whole ring lies between
  them. `xp/4096gx` per read covers 32 KB.
- **Walk the pointers offline.** Once the span is in a dictionary keyed
  by address, chasing `__next_` costs nothing.

The list is `zpp::list<zpp::string>`: `{__prev_, __next_}` then the
string sixteen bytes in. libc++'s string is the small-string
optimisation - the low bit of the first byte says which form. Long is
`{cap, size, data}`, so the pointer is at node + 32; short keeps its
characters at node + 17 and its length in the top seven bits of that
first byte.

## Traps

| Symptom | Cause |
|---|---|
| Plausible rubbish, or a list that never terminates | The module base is from the **previous** run. It is not stable, and a reset reloads the module elsewhere. Read the last `allocate_rwx done at` from *this* run's serial. |
| `list head unreadable` | Wrong base, or the guest reset and the module is gone. Check `info status` before believing anything else. |
| A line reads `<unreadable>` | Its string data fell outside the dumped span. Widen the padding around the node range. |

## Any other member, not just the log

The same two commands answer for every counter, and this is the fast way
to ask "is it still moving" without perturbing anything:

```sh
# offsets, offline, from the binary that was deployed - the ptype first
# expands the compilation unit, and without it the class name does not
# resolve and every query answers "No type ... in namespace zpp"
x86_64-elf-gdb -q -batch out/debug/x86_64/zpp_hypervisor \
  -ex "ptype 'zpp::hypervisor::hypervisor::record_exit'" \
  -ex "print/x &'zpp::hypervisor::hypervisor::instance()::instance'" \
  -ex "print/x (long)&(('zpp::hypervisor::hypervisor' *)0)->l2_entries"

# live, physical: instance = module base + singleton offset + member
printf 'xp/8gx 0x6a85af20\n' | nc -w 6 <rig> 4446
```

**Sample twice.** A frozen counter and a slow one look identical once.
`l2_entries` unchanged across two samples is what says a guest hypervisor
has stopped entering its guest; `exit_total` unchanged is what says a
processor is taking no exits at all, which is a halted processor rather
than a busy one.

**Recompute the offsets against the binary that is deployed.** Adding one
member moves every later one, and a stale offset reads a neighbouring
counter as a plausible number. `.rig-deployed-hash` says what is actually
on the disk.
