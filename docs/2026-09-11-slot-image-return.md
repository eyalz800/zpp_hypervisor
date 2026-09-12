# Cache-slot boot stopped at an interrupted driver-image return

The slot-projection boot ran from 16:55 to about 17:25 UTC on September 11.
It was stopped for the next experiment after the final checks below. It
never reached smss or a verified login. The following evidence applies to that completed run.

## Build identity and earlier observations

The loader was the cache-slot projection from 48bcfc9c, MD5
5756af63a9cbdfdf5a84ff2b353ee35b, verified from a fresh mount. CPU count
and the full manifest were unchanged. All startup channels answered.
The slot projection gives 156 current fields distinct slots instead of
26, adding 786,432 bytes of cache BSS. Its 191 cache checks and all host,
debug-build, invariant and bootability checks passed before deployment.

Use the archived /tmp/zpp-20260911/cache-slots-deployed.elf for this run.
The module base was 66d48000 and singleton offset 15ab000, placing the
singleton at 682f3000. Write-hit and interrupt-shadow-clear offsets were
15aaeb0 and 51b7020, physical 682f2eb0 and 6beff020. Their agreement with
the preceding boot's physical addresses was a relocation coincidence.

Windows base fffff800a3000000 came from the resident field resolved with
that ELF's DWARF. CR3 1ae000 walked its PE header successfully, matching
timestamp 51a135d9 and image size 1450000. Complete process walks from
three minutes onward had System, Secure System and Registry. Timer reads
had resolution count zero, last request ffffffff, pseudo interval 156250
and no interrupt-shadow clearings. Both PRCB processor numbers validated.
DPC counts advanced; CPU 0's queue briefly read two entries, then zero.

Repeated valid CPU 0 unwinds reached KeSwapProcessOrStack through
KiSwapThread+795, with the successful wait result in RBX. CPU 1 repeatedly
reached MiUnlockPageInline+36 during driver-image validation/loading.
A direct CPU 1 capture at thirteen minutes independently had RIP
nt+296dd6, RBX=0, RSP fffff50644e06ab0. Matched disassembly puts it after
mov cr8,rbx and before add rsp,20; pop rbx; ret. The page has already
been released and IRQL restored to PASSIVE. This is not an internal
page-unlock polling loop. Invalid/torn unwinds were excluded.

## Final observations before teardown

Captures around thirteen, twenty-one and thirty minutes recover the same
MiUnlockPageInline+0x36 return at RSP fffff50644e06ab0. At thirteen and
twenty-one minutes all recovered nonvolatile registers in the outer
MiWalkEntireImage frame also agree. This supports lack of observed progress
through that invocation; it is not continuous tracing of every instruction.

The loader's saved argument identifies **\SystemRoot\System32\Drivers\Npfs.SYS**.
MmLoadSystemImageEx's matched prologue saves RCX at its entry RSP+8 and
sets RBP to entry RSP-0x47. In three thirteen-minute unwinds, RBP+0x4f is
fffff50644e07890 and contains the UNICODE_STRING address
fffff50644e078f8. The structure is also in the captured stack: length 74,
maximum length 76, buffer ffffbb0786c27270. A live read at twenty-one minutes
validates the kernel PE again and confirms the saved pointer and descriptor
before and after reading the UTF-16 string. The image is being validated;
this does not blame Npfs's driver code or establish that its DriverEntry ran.

The final process walk still has System, Secure System and Registry. Both
the twenty-two-minute and final power-list walks are well-formed and empty.
The final guest interrupt time is 1,759.7 seconds since boot.

| Late delta | About 22 min | Final |
|---|---:|---:|
| Measured span | 32.089 s | 32.146 s |
| Fresh VTL calls CPU 0/1 | +0/+0 | +0/+0 |
| CPU 0 user-mode samples | +0 | +0 |
| L2 entries/s CPU 0/1 | 3,374.06 / 3,474.65 | 3,369.96 / 3,472.71 |
| Handler share CPU 0/1 | 69.67% / 73.99% | 69.69% / 73.98% |
| VMREAD/VMWRITE failures | +0/+0 | +0/+0 |

The final cache rates are 64,543.0 hits/s, 202,618.1 misses/s,
145,522.2 executed writes/s and 1,507.9 skipped writes/s. These are interval
measurements, but boot-to-boot phase and scheduling differences still prevent
an isolated causal estimate of the slot projection's performance effect.

Artifacts under /tmp/zpp-20260911/ use cache-slots-:
both-stacks-{13min,21min,final}/ and their unwind files,
driver-path-pointers.json, driver-path-21min.txt,
image-walk-13-vs-21min.txt, load-image-prologue.txt,
unlock-page-disassembly.txt, delta-{22min,final}.txt,
processes-final.txt and power-final.txt. The deployed ELF was archived before
any replacement; the staged next candidate is in local-borrow-ready/.
