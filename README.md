zpp_hypervisor
==============

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
- CMake 3.25+
- Ninja
- LLVM/Clang 18+ (with libc++ headers)
- Podman (optional, for Linux kernel module build)

See [clang.md](clang.md) for detailed toolchain setup.

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

Once your instruction pointer is within the hypervisor, load symbols and resume:
```gdb
set *(char *)&gdb_attached = 1
```

Final Words
-----------
I hope that you enjoy using this project and feel free to report any issues.
