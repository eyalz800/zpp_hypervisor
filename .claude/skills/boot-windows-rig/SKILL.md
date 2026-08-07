---
name: boot-windows-rig
description: Use when booting Windows as a guest behind zpp_hypervisor on the TinyCore target - the VFIO passthrough rig, the build flags it needs, and the traps that cost a session to rediscover
---

# Booting Windows behind the hypervisor

The rig already exists on the target. **Do not build a QEMU command line by
hand** - an emulated NVMe with a qcow2 overlay is not equivalent to it and
will not boot Windows, which wastes a session proving nothing.

Target: `tc@192.168.1.199`. RAM-based, so it regenerates its SSH host key on
every boot - clear the stale key with `ssh-keygen -R` rather than treating the
warning as an attack, and pass `-o StrictHostKeyChecking=no
-o UserKnownHostsFile=/dev/null`. If the key is gone, the password is `1`.

## The procedure

1. **Build with the rig's flag.** In `uefi_loader/src/main.cpp`:

   ```cpp
   static constexpr bool chain_to_our_own_device_only = false;
   ```

   The committed value is `true`, which is correct for a bare-metal boot where
   the loader and Windows share one ESP. In the rig they do not: the loader
   boots from a virtio ESP and Windows is on the passed-through NVMe, so `true`
   makes the loader search only its own device and report `no boot manager
   found`. **This is a rig-only change - revert it before committing.**

2. **Deploy** the built `out/debug/x86_64/zpp_loader.efi` to
   `~/zpp/esp/EFI/BOOT/BOOTX64.EFI` on the target. Back up whatever is there
   first, and verify the md5 matches after copying.

3. **Run** `cd ~/vm && sudo ./boot-zpp.sh`.

   If it dies with `sudo: dmidecode: command not found` and QEMU then rejects
   `-smbios type=4 ... max-speed=` as "expects a number", **do not reinstall
   dmidecode**. It is almost certainly already there. The package installs to
   `/usr/local/sbin/dmidecode`, and `sudo` resets `PATH` to its `secure_path`,
   which does not include `/usr/local/sbin` - so the binary exists, is on the
   user's `PATH`, is listed in `onboot.lst`, and is still invisible to the
   `sudo dmidecode` calls inside the script. `tce-load` will just answer
   "dmidecode is already installed!" and nothing improves.

   The fix is a symlink into a directory `secure_path` does cover:

   ```sh
   sudo ln -sf /usr/local/sbin/dmidecode /usr/bin/dmidecode
   ```

   TinyCore runs from RAM, so make it persist: append that line to
   `/opt/bootlocal.sh` and run `~/vm/backup.sh` (`filetool.sh -bv`). `/opt`
   and `/home` are already in `/opt/.filetool.lst`, so both are captured.
   Backing up needs the ESP mounted, so it cannot be done while the VM holds
   the NVMe.

4. **Read** `~/zpp/serial.out` for the loader's trace. Windows itself prints
   nothing there.

## What the rig actually does

- VFIO-passes the **real** hardware: GPU `00:02.0` (with `i915ovmf.rom`), GPU
  audio `00:1f.3`, the **real NVMe `02:00.0`**, and WiFi `00:14.3`.
- So **Windows writes to the actual disk.** There is no overlay. Treat every
  run as a real boot of the real installation.
- Copies the host's SMBIOS strings so Windows sees the same machine.
- `intel-iommu` with `caching-mode=on`, which device assignment requires.
- Unmounts the NVMe partitions and rebinds the device to `vfio-pci`, so
  nothing on the host can reach the disk until the VM exits.

## Traps that have already cost time

- **SSH survives.** The route is `eth0`, the USB Ethernet dongle behind the
  xHCI at `00:14.0`, which the script does not pass through. Only WiFi goes to
  the guest. Do not assume the run will disconnect you.
- **One variable per boot.** There are two independent switches, deliberately
  split so a boot can exercise exactly one:
  - `diag::sink::esp_blocks` `present` - the disk channel, which drags in the
    self-test's **admin-queue borrow against the real disk**.
  - `diag::reserve_controller_window` - only the loader's edit of the DMAR
    table to declare a reserved window.

  Turn on at most one of these per boot, on top of a build already known to
  reach the desktop.

- **Verify the build locally before deploying it.** Boot it under plain QEMU
  first and count the trace lines, so what is being shipped is known rather
  than assumed:

  ```sh
  grep -ac 'selftest:' serial   # 0 when the channel is compiled out
  grep -ac 'rmrr:'     serial   # >0 when the reserved window is on
  ```

  Local QEMU is TCG on an ARM Mac, so the hypervisor cannot launch there - the
  trace stops after `start up memory`. That is expected and still proves which
  loader paths run.
- **Never reuse a qcow2 overlay across attempts** if one is in play at all -
  state from a failed boot silently contaminates the next run.
- A control that fails proves nothing. If Windows will not boot, check that it
  boots *without* the hypervisor in the same configuration before blaming the
  hypervisor.

## Debugging a failure live

The script appends `$ZPP_QEMU_EXTRA` verbatim, which is the supported way in -
**do not write a variant script for this**:

```sh
ZPP_QEMU_EXTRA='-gdb tcp:0.0.0.0:1234 -S' sudo -E ./boot-zpp.sh
```

and from the Mac, which has `x86_64-elf-gdb`:

```sh
x86_64-elf-gdb -ex 'target remote 192.168.1.199:1234'
```

Add `-monitor telnet:0.0.0.0:4444,server,nowait` the same way for a channel
that inspects state **without perturbing it** - `info registers -a`, `xp`,
`screendump`. Reach it with `nc 192.168.1.199 4444`.

### Reading the disk channel's state while the guest runs

The channel keeps counters that say exactly where a write went wrong, and
they can be read through the monitor without pausing anything. Their
addresses are `module base + symbol`, and the symbols are template statics
so `llvm-nm` finds them under the mangled `queue_pair<64>` name:

```sh
llvm-nm out/debug/x86_64/zpp_hypervisor | grep -E 'queue_pairILj64EE'
```

What each one means when it is the odd one out:

| counter | reading |
|---------|---------|
| `submitted` > `completed` | commands went out and nothing came back - the controller is not fetching from where we write, or the queue is gone |
| `refused_guard` rising | the epoch check is rejecting writes; the controller was reset |
| `refused_signature` rising | the destination did not carry our signature - the extent table is wrong, or something else took the blocks |
| `failed` rising | the controller completed the command and reported an error |
| `lost_to_reset` > 0 | a reset was noticed and the queue forgotten, with that many writes discarded |
| all zero, nothing on disk | no write was ever attempted - look at the sink, not the queue |

`submitted = 1, completed = 0` with everything else zero is the signature
of a queue the controller is not reading: it has been caused by the two
sides using different queue memory, and by the two sides disagreeing about
the queue position. Both are fixed, and both looked identical from here.

Remember the guest keeps its own copy of nothing - these are the resident
module's statics, and the loader's identically named ones are a different
object in a different binary. Reading the loader's by mistake shows a
healthy queue that has nothing to do with the failure.

### Reading the recorded state out of a stopped guest

This works without symbols, without breakpoints, and without catching the
failure as it happens - which matters, because the interesting failures are
over before a debugger could attach. The VMM records what happened into
members and then stops, and the module is `allocate_rwx`'d and never freed,
so **the evidence is still in memory at the UEFI shell**. Let it fail, then
read it.

The address of any member is
`module_base + <singleton address> + <member offset>`:

```sh
grep 'allocate_rwx done' serial.out              # module base, per run
llvm-nm out/debug/x86_64/zpp_hypervisor | grep 'instanceEvE8instance'
llvm-dwarfdump --name=<member> out/debug/x86_64/zpp_hypervisor \
  | grep -E 'DW_AT_name|data_member_location|DW_AT_type'
```

Then `x/Ngx <address>` in gdb. **Sanity check the arithmetic before
believing a result**: read `host_page_table` (its first quadword is a
present PML4 entry, `...023`) and confirm it looks like a page table. An
all-zero record is a real answer only once the math is known good -
otherwise it just means the address is wrong.

Worth reading, in this order:

- `host_exception` - `vector`, `error_code`, `rip`, `cs`, `rflags`, `rsp`,
  `ss` at offsets 0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, then
  `host_exception_cr2` immediately after. `cs` distinguishes *when*: the
  loader's `0x38` means before the guest ran, the VMCS host `0x08` means
  inside a VM exit.
- `unhandled_exit`, `vm_entry_failure` - check `occurred` first.
- `exit_trace` / `exit_trace_count` - newest at `(count - 1) % capacity`.

`halt()` is naked, so on a stopped processor `x/gx $rsp` gives the return
address and names *which* halt loop it is - there are four, and they mean
different things. Symbolize any address with

```sh
llvm-symbolizer --obj=out/debug/x86_64/zpp_hypervisor \
  --functions=linkage --demangle <rip - module_base>
```

which resolves to function *and source line* even though the shipped binary
is stripped. This is usually faster and more certain than a breakpoint.

To decide whether an address was mapped, walk the host page table by hand
rather than assuming: PML4 index is `addr >> 39 & 0x1ff`, PDPT `addr >> 30
& 0x1ff`. A zero entry with `#PF` `error_code = 0` is a not-present fault
and the two agree.

**A `#UD` at a small offset from the module base is not always executed
data.** The notes name that signature for a bad relocation, and it is also
what a VMX instruction issued outside VMX operation looks like -
`invept`, `invvpid`, `vmread` - since those are plain `#UD` outside root
mode. Symbolize the offset before concluding anything.

### Breaking inside the hypervisor itself

To stop in the VMM's own code - the exit handler, the write path - the
usual rule **inverts**.

- **`hbreak` cannot work there.** SDM, VM-exit state loading: "DR7 is set
  to 400H". Every VM exit disables all four debug registers before the
  handler's first instruction, so a hardware breakpoint in host code can
  never fire. This is architectural; there is no way to ask for it back.
- **`break` (software) does work.** The hypervisor is itself KVM's guest,
  and KVM adds `1u << BP_VECTOR` to the exception bitmap exactly when the
  debugger has a software breakpoint armed - `vmx.c`, `update_exception_
  bitmap`. So an `int3` in host code exits to L0 and reaches gdb.

The "always hbreak, never break" rule elsewhere in these notes is about
**Bochs at reset**, where the byte cached before the module is loaded is
pre-load garbage that gets written back over real instructions. Once the
module is mapped, a software breakpoint is correct - and in the VMM it is
the only thing that can work.

**You do not need to force a VM exit.** They arrive constantly: CPUID, MSR
accesses, EPT violations, and this VMM's own interceptions. Break on the
exit handler and the next one lands in it. If you want a *specific*
instruction to cause one, arm the Monitor Trap Flag from inside the VMM
rather than trying to drive it from the debugger.

### A halted processor's RIP points past the `hlt`

`halt()` is `hlt; ret`, so a processor stopped in a halt loop shows RIP on
the **`ret`**, not on the `hlt`. It reads like a CPU stuck on a return
instruction, which is nonsense and sends you looking for a corrupted
stack. It means the processor is halted with interrupts disabled and will
never resume. Sample RIP twice: frozen plus a `ret` at that address plus
`cli`/`hlt` just before it is the signature.

### Reading state while the guest is running

gdb reads through the **current** CPU's page tables. Once Windows is
running, our module is not mapped in the guest's CR3 and every read of it
answers `Cannot access memory`. That is not a missing symbol or a wrong
address - it is the wrong address space.

Use the monitor's `xp` instead, which reads **physical** memory and
ignores paging entirely:

```
xp /3gx 0x7a20f0b8      # note the space after xp
```

Our module is loaded at a physical address printed on serial and identity
mapped by UEFI, so the addresses computed from `llvm-nm` plus the module
base work directly as physical addresses. This is also the only way to
read the VMM's counters without stopping the guest - and attaching gdb at
all pauses the VM, which silently freezes whatever you were measuring.

Rules that are correctness requirements, not preferences:

- **Never `stepi` the guest through QEMU's gdbstub.** KVM implements
  single-step by ORing `TF` into guest RFLAGS; with pending-debug-exceptions
  `BS` clear that combination fails the nested guest-state check and the next
  `vmresume` takes an entry-failure exit. It destroys the thing being measured
  and the stub never returns a stop reply. Use the **monitor** instead, or arm
  the Monitor Trap Flag from inside the VMM.
- **`hbreak`, never `break`.** A software breakpoint caches the byte it
  replaces; set before the module is in memory, it writes pre-load garbage back
  over real instructions on removal. Hardware breakpoints touch no memory.
- **No gdb inferior calls.** Everything in `zpp/arch/x86_64/asm.h` is
  `__attribute__((naked))`, so calling one from gdb faults and leaves the
  session in a broken called-frame state.
- Symbols need the module's load address, printed on serial as
  `allocate_rwx done at ...`, and it is **not** stable across configurations -
  read it per run. `scripts/load-symbols <addr> out/debug/x86_64/zpp_hypervisor`
  only works once the loader has mapped the module, so break first and load
  symbols after.

For a failure that happens before a debugger can realistically attach, build
with `-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=ON`: the hypervisor spins at its entry
point until released with `set var gdb_attached = 1`. Racing a gdb attach
against a guest that dies is not a strategy.

### Run the tmux session locally, on the Mac

**The target has no `tmux` and no `screen`, and it is not supposed to.** The
session belongs on the **Mac**, holding SSH connections *into* the target -
not on the target itself. Looking for `tmux` there wastes a round trip and
then invites a detached `nohup` that nobody can watch, which is the thing the
rule exists to prevent.

```sh
tmux new-session -d -s zpp-rig -n serial \
  "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
   tc@192.168.1.199 'tail -f /home/tc/zpp/serial.out'"
tmux new-window -t zpp-rig -n target \
  "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null tc@192.168.1.199"
tmux attach -t zpp-rig
```

One window follows the serial trace live, one is a shell on the target for
starting the rig and reading state. QEMU itself is started from the target
window with `setsid nohup`, because it must outlive the SSH connection - the
watchability requirement is satisfied by the tmux session on this side, which
is what can actually be reattached across turns.

The same rule for gdb: run `x86_64-elf-gdb` in a window of that session on the
Mac, pointed at the target's gdbstub.

### `scp` does not work to the target

TinyCore ships no `/usr/libexec/sftp-server`, so plain `scp` dies with
`Connection closed`. Pipe through SSH instead, and verify the hash in the same
command so a truncated copy cannot be mistaken for a deployed one:

```sh
cat out/debug/x86_64/zpp_loader.efi | ssh tc@192.168.1.199 \
  'cat > /home/tc/zpp/esp/EFI/BOOT/BOOTX64.EFI && md5sum /home/tc/zpp/esp/EFI/BOOT/BOOTX64.EFI'
```

`scp -O` would also work, but the pipe is one command and checks itself.

## Reading the result

Windows reaching the desktop is observed on the laptop screen, since the GPU
is passed through. From the host, the only signals are that
`qemu-system-x86_64` is still alive and that the loader's trace in
`serial.out` reached `chainloading \EFI\Microsoft\Boot\bootmgfw.efi`.

To read anything **out of** the booted Windows, shut the VM down first, then
mount the NTFS volume read-only and read the registry - for example
`Enum\PCI\<instance>\Device Parameters\DMA Management` to find out what
Windows decided about DMA remapping for the controller.

## Every change to the target dies on reboot unless it is backed up

TinyCore runs from RAM and restores `/home` and `/opt` from
`mydata.tgz` at boot. **An edit you made and did not back up is gone, and it
comes back as the *old* version rather than as a missing file** - which is
far worse, because nothing looks broken.

This has already cost a full debugging detour. After a reboot:
`boot-zpp.sh` reverted to the version with the synthetic FAT disk, the
`.bak` copies made minutes earlier were gone, and `check-boot-options.sh`
had never existed. The rig then booted the *stale* loader sitting on the
synthetic ESP - a binary from hours earlier with an already-fixed `invept`
bug - and hung. Every conclusion drawn from that run was about a binary
nobody had built that day.

So: **after editing anything under `/home/tc` or `/opt`, back up
immediately**, and verify the backup rather than trusting it.

```sh
sudo mount /dev/nvme0n1p2 /mnt/nvme0n1p2      # backup target lives on the ESP
sudo filetool.sh -b
tar tzf /mnt/nvme0n1p2/EFI/tc/tce/mydata.tgz | grep <the file you changed>
tar xzf /mnt/nvme0n1p2/EFI/tc/tce/mydata.tgz -O home/tc/vm/boot-zpp.sh | grep -c zppesp
```

The backup writes to the real ESP, so it cannot run while the VM holds the
NVMe - kill QEMU and let the device rebind first.

**The tell that this has happened**: trace line numbers that do not match
the source you just built (`ZPP_TRACE loading` at `main.cpp:1399` when your
build puts it at 1430), or expected trace lines missing entirely. Both mean
a different binary ran. Check the md5 of what is actually deployed before
debugging anything.

## Validate the NVRAM boot options *before* every boot

**A boot that lands in the UEFI shell looks exactly like a loader that ran
and failed.** Telling them apart costs a reboot, and the cause is usually
that there was nothing to boot rather than that something broke. Check the
variable store first - it answers this without starting the machine:

```sh
/home/tc/vm/check-boot-options.sh          # lists entries, fails if ours is absent
```

Two distinct failures it catches, both of which have already happened here:

- **No entry at all.** BdsDxe goes straight to the shell and logs *nothing* -
  there is no "failed to load" line, because nothing was attempted. An empty
  boot attempt list next to a shell prompt means a missing option, not a
  broken loader.
- **The entry exists and is still never tried.** `BootOrder` is not a
  complete list of the options that exist. On this machine the real NVMe sits
  in NVRAM as `Boot0001` ("UEFI WDC PC SN520 ... 1834A4806408") and the
  firmware went to the shell anyway, because `BootOrder` did not name it.
  **Presence is not the same as will-be-tried**, so never conclude from
  "the entry is there" that it will boot.

Add the entry from the shell, with the position argument, so it goes to the
front of `BootOrder` as well as being created:

```
bcfg boot add 0 FS0:\EFI\zpp\zpp_loader.efi "zpp loader"
bcfg boot dump -v
```

This persists in `RELEASEX64_OVMF_VARS.fd`, so it survives reboots - and is
therefore also something to re-check after restoring that file from `.orig`,
which throws the entry away.

Related trap already in CLAUDE.md, for the *host's* firmware rather than the
guest's: auto-generated options carry no optional data, and firmware
regenerates and reorders `BootOrder` as devices come and go.

## Booting the loader from the real ESP, not the synthetic disk

The rig normally serves the loader from a synthetic FAT disk
(`file=fat:rw:$ZPP_ESP`, `bootindex=0`). That is fine for ordinary runs and
**wrong for anything involving the ESP reservation**: the reservation
identifies its volume from the loaded image's own device path, so booting
from the synthetic disk makes it resolve to a volume with no NVMe node, and
it refuses.

Use `ZPP_BOOT_FROM_NVME=1`, which drops both synthetic-disk lines so the
firmware boots the passed-through NVMe - the same path bare metal takes.
The original script is backed up as `boot-zpp.sh.bak-prenvme`.

Note the removable-media path on that disk is **Limine**, not us, and
Limine's first menu entry is TinyCore. So booting the NVMe reaches Limine,
not the loader, unless a boot option names the loader directly - which is
what the `bcfg` entry above is for. Never overwrite
`/EFI/Boot/bootx64.efi`; it is the recovery path.

## Always check the machine state before saying what it is

`qemu-system-x86_64` being alive says the process exists, nothing more. It is
alive at the UEFI shell, alive with a halted boot processor, and alive with
Windows on the desktop. **Ask the monitor** rather than asserting from the
process list or from what the last run did:

```sh
{ printf 'info status\n'; sleep 2; printf 'info registers\n'; sleep 2; } \
  | nc -w 6 192.168.1.199 4444
```

`info registers` settles it in one look, without perturbing anything.
Windows is unmistakable: `RIP` and `GS` base in `fffff8xxxxxxxxxx`, `CS=0010`,
`TR` a busy 64 bit TSS. Our own code is a module-base-relative address -
subtract the base from the serial trace and symbolize it. The UEFI shell sits
around `0x7exxxxxx`.

This matters because the wrong assumption is expensive in both directions:
killing a live Windows risks the installation, and treating a parked UEFI
shell as a running guest wastes the session waiting for nothing.

## Afterwards

Shut the guest down, do not just kill it while Windows is live. From the
monitor:

```sh
printf 'system_powerdown\n' | nc -w 5 192.168.1.199 4444
```

That is the ACPI power button and Windows takes it as a clean shutdown.
**Then watch the screen, not the process.** On this rig the shutdown
regularly does not finish - the display goes black and QEMU keeps running
indefinitely. Once the screen is off, Windows is down and

```sh
sudo pkill -x qemu-system-x86_64
```

is the right move. `pkill -x`, never `pkill -f`. Waiting longer does not help
and only holds the NVMe hostage.

Letting the process exit lets the script's restore path rebind the NVMe to the
host: `/dev/nvme0n1*` reappear and
`readlink /sys/bus/pci/devices/0000:02:00.0/driver` goes back to `nvme` from
`vfio-pci`. Nothing on the host can read the disk until that has happened.

## Backing up the disk before anything writes to it

Do this before any change that edits the disk in place - the ESP reservation
in particular, which rewrites the FAT32 total sector count, the FSInfo block
and the backup boot sector at sector 6. It needs the NVMe rebound to the host,
so it cannot be done while the VM holds it.

The layout on this machine, established by reading the boot sectors rather
than by guessing at partition numbers:

| part | size | contents |
|------|------|----------|
| p1 | 16 MB | zeros, reserved |
| **p2** | **16 GiB** | **FAT - the ESP**, and what `HD(2,GPT,...)` refers to |
| p3 | 16 MB | zeros, reserved |
| p4 | ~460 GB | NTFS, Windows |
| p5 | ~1.2 GB | NTFS, recovery |

The ESP is **partition 2, not partition 1**, and it is 16 GiB rather than the
usual few hundred megabytes. Identify it by its boot sector (`MSDOS5.0`),
never by position.

TinyCore's rootfs is in RAM, so a backup written there costs memory and does
not survive a reboot. **Pull it to the Mac**, compressed - the volume is
mostly empty so it shrinks enormously:

```sh
ssh tc@192.168.1.199 'sudo dd if=/dev/nvme0n1p2 bs=4M 2>/dev/null | gzip -1' \
  > esp-p2.img.gz
```

Take both GPT copies too, since a partition level restore needs them: the
primary is the first 34 sectors, the backup is the last 33
(`cat /sys/block/nvme0n1/size` gives the total). Verify by comparing an
`md5sum` taken on the target against one taken from the decompressed image
here - a truncated stream otherwise looks exactly like a short partition.

Reading total-sectors-32 at offset `0x20` of the ESP boot sector says whether
the reservation has already run: equal to the partition size means untouched.
