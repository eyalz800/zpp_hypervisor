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

`.cirrus.yml` covers the gap. Cirrus CI can run on GCE with
`nested_virtualization: true`, which injects Intel VT-x into the VM itself.
That is all this project needs, and it is worth being clear why: the hypervisor
virtualizes the OS it is loaded into, so there is no nested guest involved and
only one level of nesting is required - the ordinary supported case, not the
two levels that a VM-inside-the-runner approach would need. The task builds the
module against the running kernel, insmods it, and checks the machine is still
healthy under load with the hypervisor resident.

Requires a linked GCP project. Nested virtualization is Intel only and needs a
minimum CPU platform of Intel Haswell.

For the Windows driver the same trick applies, but the machine has to persist:
the unsigned `.sys` needs `bcdedit /set testsigning on` and a reboot, which a
throwaway CI VM cannot survive. Create a GCE Windows instance with
`--enable-nested-virtualization`, turn off Hyper-V and VBS, enable test
signing, reboot, and register it as a self-hosted runner labelled `vtx`. The
`windows-root-mode` job then runs there and refuses to proceed unless Hyper-V
and VBS are actually off.

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
