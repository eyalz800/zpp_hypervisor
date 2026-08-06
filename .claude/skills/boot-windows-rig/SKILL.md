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

3. **Run** `cd ~/vm && sudo ./boot-zpp.sh`. It needs `dmidecode`; if that is
   missing QEMU refuses to start on the `-smbios type=4` line. Install it
   persistently with `tce-load -wi dmidecode` - which only works while the
   NVMe is still bound to the host, i.e. **not** while the VM is running.

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

Run gdb and the emulator in **named tmux sessions**, never as detached one-shot
commands, so a session can be reattached across turns instead of restarting
everything to look at one value. Note the target has no `tmux` - run it on the
Mac side, or use `nohup` there and keep the observation on the Mac.

## Reading the result

Windows reaching the desktop is observed on the laptop screen, since the GPU
is passed through. From the host, the only signals are that
`qemu-system-x86_64` is still alive and that the loader's trace in
`serial.out` reached `chainloading \EFI\Microsoft\Boot\bootmgfw.efi`.

To read anything **out of** the booted Windows, shut the VM down first, then
mount the NTFS volume read-only and read the registry - for example
`Enum\PCI\<instance>\Device Parameters\DMA Management` to find out what
Windows decided about DMA remapping for the controller.

## Afterwards

Kill QEMU rather than leaving a guest occupying the machine's screen, and let
the script's own restore path rebind the NVMe to the host.
