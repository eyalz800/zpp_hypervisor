---
name: boot-windows-rig
description: Use when booting Windows as a guest behind zpp_hypervisor on the TinyCore target - the VFIO passthrough rig, the build flags it needs, and the traps that cost a session to rediscover
---

# Booting Windows behind the hypervisor

The rig already exists on the target and it is **passthrough only**. Do not
build a QEMU command line by hand, and do not introduce an emulated
controller or a copy-on-write image as a "safer" intermediate step. There is
one rig; everything is tested on it.

That is a deliberate decision, not an oversight. An emulated NVMe removes
the thing being tested: the guest and the channel would talk to QEMU's
*model* of a controller, so the admin queue is QEMU's, the doorbell stride
and `MQES` are QEMU's, and no real-device behaviour is exercised at all. The
log blocks would land in a file that disappears, when the whole point is
that an agent elsewhere reads them off the medium. It also cannot be
combined with passthrough - VFIO means the guest drives the hardware
directly, so there is no layer to interpose an image into - so anything
proved that way has to be proved again anyway.

The consequence is that every experiment risks the real machine, so the
discipline below is not optional: a verified backup in hand, one variable
per boot, the known-good loader restored afterwards, and
`scripts/check-bootable.sh` between the build and the disk.

Target: `tc@192.168.1.199`. RAM-based, so it regenerates its SSH host key on
every boot - clear the stale key with `ssh-keygen -R` rather than treating the
warning as an attack, and pass `-o StrictHostKeyChecking=no
-o UserKnownHostsFile=/dev/null`. If the key is gone, the password is `1`.

## The procedure

**The synthetic FAT boot disk is gone.** The rig used to serve the loader from
`file=fat:rw:$ZPP_ESP` at `bootindex=0`, and the loader ran from a volume that
was not the one Windows lives on. That is removed: the firmware now boots the
**passed-through NVMe**, exactly as bare metal does.

It had to go. The ESP reservation identifies its volume from the loaded
image's own device path, so a loader started from the synthetic disk resolved
to a volume with no NVMe node and refused to reserve anything. Anything
touching the disk channel has to boot the real ESP.

1. **Build.** No source edits are needed for the rig any more - what used
   to be an edit to `chain_to_our_own_device_only` is now derived from
   `-DZPP_SEARCH_ALL_DEVICES`.

   ```sh
   cmake --preset debug -DZPP_DIAG=ON -DZPP_SEARCH_ALL_DEVICES=ON
   ```

   `ZPP_SEARCH_ALL_DEVICES=ON` makes the loader look for a boot manager on
   every volume; `OFF` restricts it to the one it booted from. Now that the
   loader boots the real ESP, `OFF` is the bare-metal-correct setting and
   ought to work, since Windows' boot manager is on that same partition -
   but every rig boot so far has used `ON`, so treat switching it as its own
   experiment rather than folding it into another one.

   For the disk channel, `diag::sink::esp_blocks` must additionally be
   `present` in `diag/include/zpp/diag/config.h`.

2. **Deploy** with the script, which refuses unless the disk matches the
   build and reads back from a *fresh* mount rather than the one that wrote
   it:

   ```sh
   ./scripts/deploy-to-rig.sh            # to /EFI/zpp/zpp_loader.efi
   ```

   It records the hash in `.rig-deployed-hash`. Check any confusing boot
   against that before debugging the failure itself.

   **Never write `/EFI/Boot/bootx64.efi`** - that is Limine, the recovery
   path, and the only way back if a boot leaves the machine unbootable.

   **Never leave a build on the ESP that has not completed successfully at
   least once.** Between experiments the ESP should hold the last known
   good loader, not the experiment. A build that hangs Windows is
   recoverable only while there is a channel to replace it, and the
   channel is the machine itself - deploy something risky, lose the
   machine, and the only way back is a power cycle and picking TinyCore
   out of the Limine menu by hand. That has happened.

   If the machine is unreachable and its ESP holds an experiment, the
   recovery is: power cycle, select the **first** Limine entry (`/entry`,
   TinyCore) rather than `/zpp + windows`, then deploy a known good
   loader before doing anything else.

3. **Make sure something will boot it.** The firmware needs a boot option
   naming `\EFI\zpp\zpp_loader.efi`; see the NVRAM section below, and note
   that Windows reasserts itself at the front of `BootOrder` on every boot it
   completes. A `startup.nsh` at the ESP root is the reliable fallback, since
   the shell runs it automatically - but remove it once an NVRAM entry
   exists, or `bcfg boot add` will pile up duplicates on every boot.

4. **Run** `cd ~/vm && sudo ./boot-zpp.sh`. **The `sudo` is not optional
   and leaving it off does not fail loudly.** Without it the script keeps
   going past every device step, printing

   ```
   can't create /sys/bus/pci/devices/0000:02:00.0/driver/unbind: Permission denied
   can't create /sys/bus/pci/drivers/vfio-pci/remove_id: Permission denied
   can't create /sys/bus/pci/rescan: Permission denied
   ```

   and then runs its own teardown, so it exits with no QEMU and no error
   that names the cause. `nohup ... &` from an ssh command hides those
   lines unless the log is read back, and the run then looks exactly like
   a guest that booted and died. Launch it as
   `setsid sh -c "cd /home/tc/vm && nohup sudo ./boot-zpp.sh > /tmp/boot-zpp.log 2>&1 &"`
   and confirm a `qemu` process exists by `/proc/<pid>/comm` before
   believing it started.

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

5. **Read** `~/zpp/serial.out` for the loader's trace. Windows itself prints
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

## Reading VMM memory through the monitor: four ways to get a plausible lie

Every one of these produced a confident, wrong number in one session. All
four are silent - none of them looks like an error.

**A reading of `0x00010102464c457f` is a failed symbol lookup, not data.**
That is `\x7fELF`. A helper like

    A=$(llvm-nm ... | grep "$1" | awk '{print $1}' | head -1)
    xp /1gx $((BASE + 0x$A))

yields an empty `$A` when the name does not exist, so the address becomes
the module base and the read returns the ELF header. `submitted`,
`completed` and `lost_to_reset` are *not* statics of `esp_blocks_for`, and
reading them this way reported queue depths that were never queue depths.
List the symbols once and read them by their listed addresses.

**`_ZGV...` is the guard variable, not the object.** A loose match on
`instance` returns both `_ZZN...E8instance` (the object) and
`_ZGVZN...E8instance` (its `__cxa_guard` byte). They were 0x1ced000 apart,
and the guard came first in the sort. Match the exact mangling.

**`llvm-dwarfdump --name=<member>` returns the first DIE anywhere in the
file with that name**, including members of nested and unrelated types. In
one binary `module_base` resolved to `0x1406008` and
`heartbeat_exits_seen` to `0x38`; only one of those is an offset into that
class. Check the parent DIE.

**Validate the base before trusting any offset.** Two cheap anchors, both
of which must hold: `xp /2gx BASE` shows ELF magic, and a known constant
reads its known value - `staged_deadline_ticks` at `BASE + 0x1048` must be
`0x989680`. A base that passes both can still be wrong for members whose
offsets came from the traps above, so prefer a *self-validating* read:
`module_base` is itself a member, so reading it back proves the address
used for every other member of that object.

## The VFIO rig forgets nothing: reset OVMF NVRAM every run

`boot-zpp.sh` uses `RELEASEX64_OVMF_VARS.fd` as writable pflash, and
Windows writes its own boot entry there the first time it boots. From then
on the firmware boots Windows *directly* and never runs our loader -
`bootindex=0` does not save you, because an explicit NVRAM boot option
outranks it.

The symptom is not an error. `serial.out` fills with 37 KB of firmware
chatter (`i915:`, DP link training) and contains **zero** `zpp:` lines,
while QEMU runs and Windows boots perfectly. It looks like the loader
crashed early.

    cp RELEASEX64_OVMF_VARS.fd.orig RELEASEX64_OVMF_VARS.fd

before every run. There is also a `.beforetest` copy; `.orig` is the
pristine one.

Confirm the loader actually ran before reading anything:

    grep -ac zpp /home/tc/zpp/serial.out          # must be non-zero
    grep -a "ZPP_TRACE loading"  serial.out       # hypervisor launch began
    grep -a "ZPP_TRACE loaded"   serial.out       # and returned
    grep -a ZPP_HYPERVISOR_FAILED serial.out      # absent means success

`loading`/`loaded` with no `ZPP_HYPERVISOR_FAILED` means `zpp_load_elf`
returned zero. Note this build prints no positive "hypervisor is live"
line unless `verify::enabled`, so success here is the absence of a failure
rather than a confirmation.

## The guest barely exits, so nothing runs on its own

Measured with Windows running steadily: about 1500 VM exits on the busiest
processor and 160 on the others - not millions. Anything reached only from
the exit handler effectively never runs once the guest settles. If a
counter is frozen, check the exit count before suspecting the feature.

Driving work from the VMX-preemption timer is the answer, but note that
arming it and handling exit reason 52 are separate; a tree can contain the
first without the second, in which case the first tick halts the processor
in `cli; hlt`. **A frozen RIP inside our own module, rather than in the
guest, is the signature of a deliberate halt** - read `unhandled_exit` and
`vm_entry_failure` before anything else.

## Iterating on the real NVMe, which is the only option

Windows on the passed-through disk has produced one "Inaccessible boot
device" already, and repeated hard kills are how you get there. There is no
safer rig to fall back to, so the cost has to be paid in discipline rather
than in isolation: back up first, change one thing per boot, keep the
switch for anything new defaulted **off** so a plain build cannot run it,
and restore the known-good loader after every experiment. A wedge costs a
power cycle and needs someone at the machine - budget for that before
enabling something untried. After any run, verify:

    sudo dd if=/dev/nvme0n1 bs=512 skip=1 count=1 | od -An -c -N8       # EFI PART
    sudo dd if=/dev/nvme0n1 bs=512 skip=32768 count=1 | od -An -tu4 -j32 -N4
    sudo dd if=/dev/nvme0n1 bs=512 skip=33456128 count=1 | od -An -c -N8

expecting `EFI PART`, `33423360` (the shrunk ESP - it must not shrink
again) and `ZPLOGBLK`.

## Three more silent liars

**A debugger-only field must be `volatile`.** If nothing in the program
reads it, its stores are dead and the optimizer deletes them; the symbol
survives in .bss and reads its zero initializer for ever. This reported
"configure never ran" on a run where three other fields proved it had.

**Read `module_base` back before believing any member.** It is itself a
member of the singleton, so it validates the base *and* the offset
arithmetic in one read. This matters most because a guest reset destroys
the hypervisor while leaving QEMU up: every subsequent read is recycled
RAM, and recycled RAM looks exactly like a feature that never
initialised - zeroed counters plus one word churning thousands of times a
second. If `module_base` does not read back the traced base, stop; you are
not reading our memory.

**Symbol addresses shift between builds.** Switching one `constexpr` flag
off removed code and moved the singleton from 0x1435000 to 0x1434000, and
every static with it. Re-derive addresses from the binary you actually
deployed, every time.

## Do not `pkill -f qemu-system` over ssh

The pattern matches the ssh session's own command line, so the shell kills
itself before it launches anything. The failure is silent and confusing:
no output, and the file you redirected to keeps its old timestamp, so it
looks as though the previous run's log is the current one.

    sudo pkill -9 -x qemu-system-x86_64     # exact name, no -f

For the same reason `ps aux | grep -c "[q]emu-system"` counts your own
command line. Check the actual `ps` lines rather than the count.

## Check the config flags before blaming the code

`rebuild_channel_after_reset` was left `enabled` after being measured,
against its own comment saying it must not be. Every build for hours
carried the one setting known to desynchronise the guest's admin queue and
take Windows down - which under `-no-reboot` exits QEMU, and which
destroys the hypervisor so that all its counters read as garbage
afterwards. Before debugging a new failure, diff the flags in
`diag/include/zpp/diag/config.h` against what the comments say they should
be.

## Where the loader actually goes on the VFIO rig

**`/EFI/zpp/zpp_loader.efi` on the real NVMe's ESP** (`/dev/nvme0n1p2`),
not `~/zpp/esp/...`. That second path is read by nothing:
`boot-zpp.sh` sets `ZPP_ESP` at the top and **never passes it to QEMU** -
the only `-drive` entries are the two pflash ones - so the guest boots
whatever the passed-through NVMe's ESP holds.

That ESP holds Limine, whose `limine.conf` has two entries: TinyCore
itself (`/EFI/tc/vm` - the *host* boots from this disk too, so Limine is
also the recovery path and must stay intact) and `/zpp + windows`
pointing at `boot():/EFI/zpp/zpp_loader.efi`.

Deploying, with the old one kept:

    cat out/debug/x86_64/zpp_loader.efi | ssh tc@rig 'cat > /tmp/new.efi'
    ssh tc@rig 'sudo mount /dev/nvme0n1p2 /tmp/resp &&
      sudo cp -n /tmp/resp/EFI/zpp/zpp_loader.efi /tmp/resp/EFI/zpp/zpp_loader.efi.bak &&
      sudo cp /tmp/new.efi /tmp/resp/EFI/zpp/zpp_loader.efi && sync &&
      md5sum /tmp/resp/EFI/zpp/zpp_loader.efi && sudo umount /tmp/resp'

**This cost hours.** Deploying to the unused path meant a build from 14:31
was under test all afternoon while newer binaries were compiled,
"deployed" and measured. Every anomaly it produced looked like a
hypervisor bug: counters at zero, trace lines that never appeared, a
"misdetected module base", a vanishing CPUID. Two write-ups were made and
withdrawn on the strength of it.

Two symptoms identify it instantly, and both are cheaper than any of the
theories:

  - `wc -c /home/tc/zpp/serial.out` is **byte-identical across builds**.
    Adding a trace line must change that number. If it does not, the
    binary did not change.
  - a trace you just added does not appear while the lines around it do.

Always `md5sum` the file **at the path the firmware reads**, and compare
it to the local build, on every deploy.

## Booting from anywhere but the disk under test changes the experiment

Firmware drives only what it needs to reach its boot option, so a disk it
did not boot from has no driver and an NVMe controller left with `CC.EN
0`. Two things then break, and both look like device bugs:

  - the self test finds no live driver to borrow an admin queue from, and
    logs exactly that;
  - `esp_reservation` targets the device the loader was *loaded* from, so
    booting off a virtual FAT disk points the channel's destination at
    the wrong device and the resident side rejects the hand-over with
    `target_unusable`.

The loader now calls `connect_all_controllers()` before the self test,
which fixes the first. The second is a reason to keep booting the loader
off the disk under test.

## Do not kill a running Windows - ask it to shut down

`pkill -9 qemu-system-x86_64` while Windows is up is an unclean shutdown
every single time, and they accumulate: enough of them and Windows stops
booting normally and comes up in recovery instead, which costs a repair
cycle and someone sitting at the machine. That happened here after a long
run of experiments, and it was entirely self-inflicted.

Ask first, kill only if it does not finish:

    # graceful: an ACPI power button event, which Windows honours
    { printf "system_powerdown\n"; sleep 1; } | nc -w 6 <rig> 4446
    # give it time - a real shutdown takes tens of seconds
    # only then, if it has not exited:
    sudo pkill -9 -x qemu-system-x86_64

Two bonuses for free. A graceful shutdown is the only way to exercise the
`CC.SHN` path, where the guest sets shutdown-notification with `CC.EN`
still set and the queues are still alive - which is the *easy* half of
surviving a controller reset. And a Windows that shuts down cleanly boots
cleanly next time, which is worth more than the seconds saved by killing
it.

If Windows does end up in recovery: let it boot **without** the hypervisor
(`boot.sh` rather than `boot-zpp.sh`), let it finish whatever it wants to
do, and shut it down from inside Windows. Do not try to debug the
hypervisor against a machine that is mid-repair - nothing measured there
means anything.

## Reserve a host core, so a wedge cannot cost you the connection

`boot-zpp.sh` used to compute `cpus=$(grep -c ^processor /proc/cpuinfo)`
and hand the guest **every** logical CPU. A guest processor that spins
rather than halts never yields its host CPU - and a halted one does, which
is why only the spinning case bites. With every CPU given away, a VMM bug
that leaves processors spinning starves the host completely: it stays
alive and simply never gets scheduled, so ssh reports "No route to host"
rather than refusing the connection, and only a power cycle recovers it.

Now one physical core - both hyperthreads - is reserved:

    host_cpus=$(grep -c ^processor /proc/cpuinfo)
    threads=2
    cpus=$((host_cpus - threads))

which gives a valid topology (6 = 3 cores x 2 threads on this 8-CPU box).
Measured after the change: Windows reached the kernel and ssh answered on
every probe throughout the boot.

Keep it while the VMM is being actively broken. It costs one core and it
removes "lost the machine" from the list of things a bug can do, which
means an experiment no longer needs someone standing by. `-cpu host` is
unaffected and stays; the backup of the original is
`boot-zpp.sh.before-cpu-reserve`.
