zpp_hypervisor
==============

[![ci](https://github.com/eyalz800/zpp_hypervisor/actions/workflows/ci.yml/badge.svg)](https://github.com/eyalz800/zpp_hypervisor/actions/workflows/ci.yml)

A very simple hypervisor for learning experience.

Abstract
--------
This project is really about playing with intel hardware virtualization features, which for me helps understanding
how they work.

Motivation
----------
Create a hypervisor project such that:
1. It can be debugged easily in source mode.
2. Really simple and fast build.
3. OS independent except for a few wrapper functions during the load inside a driver code
that has to be written for every OS.
4. Works in UEFI.

Project Overview
----------------
The project consists of several parts:
1. hypervisor.
2. Linux loader driver.
3. Windows loader driver.
4. UEFI loader application.

The hypervisor is a self contained ELF binary that aims to be cross platform.
The Linux / Windows / UEFI loader drivers are there to load the hypervisor
under Linux, Windows, and UEFI respectively.

Requirements
------------
- CMake 3.28+ (C++ module file sets)
- Ninja
- LLVM/Clang 18+ (with libc++ headers)
- Podman (optional, for Linux kernel module build)
- Bochs, mtools, x86_64-elf-gdb (optional, for debugging under emulated VT-x)

LLVM is located automatically: an explicit `LLVM_PREFIX`, then `llvm-config` or
`clang` on `PATH`, then the usual layouts for Homebrew, MacPorts, Debian,
Ubuntu, Fedora and RHEL. Pass `-DLLVM_PREFIX=<prefix>` only to override.

Compiling The Project
---------------------
The project uses CMake with presets. The build cross-compiles from the host to x86_64 targets.

### Debug
```sh
cmake --preset debug
cmake --build --preset debug
```

### Release
```sh
cmake --preset release
cmake --build --preset release
```

### Linux Kernel Module (optional)
Requires Podman. Build the linux loader first, then:
```sh
cmake --build --preset debug --target linux_loader_ko
```

The build output will be located at the `./out` folder.

Loading The Hypervisor
----------------------

### Linux
Copy the loader driver to the target machine and load it:
```sh
scp out/debug/x86_64/zpp_loader.ko user@target:/tmp
ssh user@target 'sudo insmod /tmp/zpp_loader.ko'
```

The loader driver will load the hypervisor and exit immediately afterwards, due to an intentional
error code return to the Linux kernel, the error code is EPERM.

### Windows
To load the hypervisor on a Windows machine, move the driver located at `./out/debug/x86_64/zpp_loader.sys`
to the machine and load the driver using:
```sh
> sc create zpp_loader type=kernel binPath=C:/path/to/zpp_loader.sys
> sc start zpp_loader
```

Remember to turn off integrity checks beforehand.

The windows driver returns an error code of `STATUS_INSUFFICIENT_POWER` when it succeeds, to unload itself.

### UEFI
I am yet to be familiar with UEFI best practices. Until now I have used a rather violant
mount of the EFI partition and replaced the `*.efi` image that I knew was the main boot selection.

Currently the UEFI support is really experimental, there are probably many issues that are invisible to me,
some are multi core issues that I did not handle and yet to even have the knowlege to solve.
I am planning to learn more about those issues and solve them eventually.

Debugging The Project
---------------------
Debugging the project can be done using `gdb` together with VMWare or Qemu-KVM that are configured
to allow nested virtualization and expose a `gdb stub`.

A friendly reminder for `Visual Studio` users in Windows, is that it supports connecting to a `gdb stub` allowing
pretty much the same debugging experience as any other application compiled in `Visual Studio`.

The hypervisor can be configured to perform a busy loop until
a debugger is attached and changes the loop variable `gdb_attached`, make sure to enable this and compile the
`./hypervisor/src/hypervisor/main.cpp` file again to enjoy the refreshed setting.

While the hypervisor is inside the busy loop, we typically have the instruction pointer within our module
which makes it easy for our debugging scripts to find the module base and load symbol information, as well
as getting out of the loop by changing the loop variable.

### Configuring VMWare for Debugging
1. In your VMware processor configuration, enable 'Virtualize Intel VT-x/EPT or AMD-V/RVI'.
2. Add the following lines to the VMWare `.vmx` file:
```py
debugStub.listen.guest64.remote = "TRUE"
debugStub.port.guest64 = "1337"
debugStub.hideBreakpoints = "TRUE"
monitor.debugOnStartGuest64 = "TRUE"
```
I recommend using the `hideBreakpoints` configuration listed above,
which makes gdb uses hardware breakpoints instead of patching the code which has lead me to unpleasant pitfalls.
This configurtion however limits the amount of breakpoints to four, use wisely.
In addition, I recommend using the `debugOnStartGuest64` configuration listed above as well as it waits for you to attach to the VM before starting
to use it.
### Configuring Qemu-KVM for Debugging
This is fairly simple, just add the following option to the qemu-kvm launch command line:
```sh
-gdb tcp::1337
```

### Configuring GDB for Debugging
Once having the debug machine ready and waiting for connection, connect with GDB:
```sh
gdb out/debug/x86_64/zpp_hypervisor
(gdb) target remote :1337
```

Once your instruction pointer is within the hypervisor, load symbols using the
`load-symbols` GDB Python script:
```gdb
source scripts/load-symbols
load-symbols $rip "out/debug/x86_64/zpp_hypervisor"
set *(char *)&gdb_attached = 1
```

The script scans backward from the given address to find the ELF header,
then loads debug symbols at the correct relocated addresses.

Final Words
-----------
I hope that you enjoy using this project and feel free to report any issues.


Debugging Under Emulated VT-x
-----------------------------
The hypervisor can be run and debugged on a machine that has no VT-x of its own
(an Apple Silicon Mac, for instance) because Bochs emulates VMX in software.

QEMU cannot do this. Its x86 TCG interpreter has no VMX implementation at all
and silently drops the feature bit:

    qemu-system-x86_64: warning: TCG doesn't support requested feature:
                        CPUID.01H:ECX.vmx [bit 5]

QEMU only exposes VMX through KVM nested virtualization, which requires the host
CPU to have hardware VT-x. On a Linux x86 host QEMU is the better choice by a
wide margin; Bochs is what works everywhere else, at roughly 10-50 MIPS.

This is also why VT-x cannot be tested in GitHub Actions. The hosted runners
report `svm`, not `vmx` - they are AMD, and are themselves Hyper-V guests - so
there is no VMX to expose to a nested guest no matter how QEMU is configured.
The `probe-virtualization` job in `.github/workflows/ci.yml` measures this and
can be re-run manually if that ever changes.

Testing VMX in CI therefore needs an Intel host, which means either a
self-hosted runner on any x86 machine with VT-x, or a paid provider that lets
you select Intel machine types with nested virtualization enabled.

Bochs needs none of that, since it emulates VMX in software and so runs
anywhere, including on those AMD runners - it is simply slow.

Testing the Windows driver on real hardware needs a persistent machine, since
the unsigned `.sys` requires `bcdedit /set testsigning on` and a reboot. Hyper-V
and VBS must be off as well: with Hyper-V running, Windows is already another
hypervisor's root partition, VMX belongs to Hyper-V, and `vmxon` cannot succeed
because this project does not support being nested. Note Windows 11 enables
VBS, and therefore Hyper-V, by default on many installs.

    bcdedit /set hypervisorlaunchtype off
    Disable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All
    Disable-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform
    bcdedit /set testsigning on
    # then clear Memory Integrity / Credential Guard and reboot
    (Get-CimInstance Win32_Processor).VirtualizationFirmwareEnabled  # must be True

There is no CI job for this - it would need a self-hosted runner on an x86
machine with VT-x.

### Building Bochs with the gdb stub

Bochs' gdb stub and its internal debugger are mutually exclusive at compile
time, and the Homebrew bottle is built with the internal debugger, so it reports
"Bochs is not compiled with gdbstub support". A source build is required:

    curl -LO https://downloads.sourceforge.net/project/bochs/bochs/3.0/bochs-3.0.tar.gz
    tar xzf bochs-3.0.tar.gz && cd bochs-3.0

    # On macOS, gui/keymap.cc uses basename() without including <libgen.h>,
    # which newer SDKs no longer provide transitively.
    sed -i '' 's|#include "param_names.h"|#include <libgen.h>\n#include "param_names.h"|' \
        gui/keymap.cc

    ./configure --prefix="$HOME/.local/bochs-gdb" \
        --enable-gdb-stub --enable-vmx=2 --enable-x86-64 --enable-cpu-level=6 \
        --enable-avx --enable-evex --enable-pci --enable-a20-pin --enable-fpu \
        --enable-long-phy-address --enable-large-ramfile --enable-logging \
        --enable-show-ips --enable-cdrom --enable-clgd54xx --enable-usb \
        --with-nogui --with-sdl2
    make -j"$(sysctl -n hw.ncpu)" && make install

Two flags must be left out. `--enable-all-optimizations` implies
handlers-chaining, which the gdb stub does not support. `--enable-smp` is
rejected outright - `bochs.h` has a hard `#error` reading "GDB stub was
written for single processor support", because the stub has no way to say
which CPU gdb is asking about. So a gdbstub build is **single CPU only**.

That matters for this project: `zpp_load_elf` loops over every CPU, and the
argument that `hypervisor::instance()` is safe without a guard rests on CPUs
being launched one at a time. Neither can be exercised under gdb. To test the
multi-CPU paths, build a second Bochs with `--enable-smp --enable-debugger`
(no gdb stub) and drive it from Bochs' own debugger instead.

Verify the result with:

    grep -E 'BX_GDBSTUB|BX_SUPPORT_VMX|BX_DEBUGGER ' config.h
    # want BX_GDBSTUB 1, BX_SUPPORT_VMX 2, BX_DEBUGGER 0

Keeping the Homebrew build alongside is useful - it has the internal debugger,
whose `vmexitbp` breakpoint halts on VMEXIT.

### Booting Windows under emulated VT-x

`scripts/bochs/bochsrc-windows.txt` follows the Bochs manual's Windows 10 guest
page. Two of its settings are load bearing: `ips` must be at least 325000000 or
Windows bugchecks mid-boot, and the CPU model must be
`corei7_sandy_bridge_2600k`, since newer models have historically crashed
Windows while still providing the VT-x and EPT this project needs.

The manual recommends installing Windows in another emulator and booting the
resulting disk under Bochs, rather than installing under Bochs. So:

1. Install Windows in QEMU on this machine. It is a JIT rather than an
   interpreter, so this takes hours rather than days. Install onto a **raw IDE**
   disk - Bochs has no AHCI, and Windows will not find a boot device it has no
   driver loaded for.
2. Inside the guest, `bcdedit /set testsigning on` and reboot, while it can
   still boot quickly. The `.sys` is unsigned.
3. Windows 10 Enterprise Evaluation is the easier target: it is a free 90 day
   ISO and has no TPM requirement. Windows 11 also works but needs its setup
   checks bypassed via `HKLM\SYSTEM\Setup\LabConfig` (`BypassTPMCheck`,
   `BypassSecureBootCheck`, `BypassRAMCheck`), since Bochs emulates no TPM.
4. Boot `windows.img` under Bochs. This takes hours. Once the desktop is up,
   **save the Bochs state** and restore it for each subsequent test rather than
   booting again - this is what makes iterating feasible at all.
5. Load the driver and attach gdb on port 1337 as usual.

Expect minutes per interaction even at the desktop. It is workable for
`sc start` and a breakpoint, not for using the machine.

### Why the UEFI loader cannot be tested under Bochs

Established by running it locally rather than through CI. OVMF boots fine under
Bochs and does reach the loader:

    BdsDxe: starting Boot0001 "UEFI Generic 1234 BXHD00011 "
            from PciRoot(0x0)/Pci(0x1,0x1)/Ata(Primary,Master,0x0)

It then hangs inside `zpp_load_elf`, and Bochs' log says why - an endless tight
loop of:

    read from port 0x0008 with len 4 returns 0xffffffff

That port is OVMF's ACPI power management timer. Bochs' i440fx does not provide
the PIIX4 power management function, so OVMF's PCI configuration read yields
nothing usable, computes a PM base of zero, and places the timer at base plus
eight. Every `MicroSecondDelay` in the firmware therefore polls a port that
always reads back all ones and never advances.

The consequence is structural rather than a tuning problem: **any UEFI service
that waits on time hangs forever under Bochs**, so the UEFI path cannot be used
for automated testing there no matter how long the timeout is.

The way forward is Bochs' legacy BIOS path with a Linux guest, which needs no
OVMF and no ACPI timer, and is what Bochs is actually good at. A small kernel
plus an initramfs containing `zpp_loader.ko` can check the same CPUID signature
from inside the guest. The `linux .ko` CI job already produces a working module.

### Other dependencies

    brew install mtools x86_64-elf-gdb

`mtools` writes the FAT boot image without mounting it. `x86_64-elf-gdb` is the
cross debugger; the plain `gdb` formula targets the host and is not usable for an
x86-64 guest on an ARM Mac.

### Running

    cmake --build --preset debug
    ./scripts/bochs/setup.sh debug     # builds build/bochs/{OVMF.fd,esp.img}
    ./scripts/bochs/run.sh             # halts, waiting for gdb on :1337
    ./scripts/bochs/debug.sh debug     # in another terminal

`setup.sh` concatenates QEMU's split edk2 halves into one 4 MB flash image,
because Bochs loads exactly one ROM, and writes `zpp_loader.efi` to the ESP as
`EFI/BOOT/BOOTX64.EFI`. On attach the guest is halted at the reset vector
(`CS:IP = F000:FFF0`).

Hypervisor symbols only become meaningful once the loader has mapped the ELF, so
load them at that point with the bundled gdb command:

    load-symbols $rip out/debug/x86_64/zpp_hypervisor

To stop the hypervisor before it runs, build with
`-DZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=1`; it then spins until you release it:

    set var zpp::hypervisor::gdb_attached = 1

OVMF's console goes to `build/bochs/serial.out`, and Bochs' own log to
`build/bochs/bochs.log`.
