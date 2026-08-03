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
To load the hypervisor on a remote Linux machine, use the `./environment/linux_load.sh` script that will
push the loader driver which has the hypervisor binary within and run it.

The loader driver will load the hypervisor and exit immediately afterwards, due to an intentional
error code return to the Linux kernel, the error code is EPERM.

Note: The `./environment/linux_load.sh` script requires `sshpass` to avoid having to type the password in SSH,
therefore it needs to be installed.

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
When you build the project, a directory named `environment` will be created according to this configuration with
useful environmental scripts.

### Configuring Qemu-KVM for Debugging
This is fairly simple, just add the following option to the qemu-kvm launch command line:
```sh
-gdb tcp::1337
```

### Configuring regular Linux GDB for Debugging
Once having the debug machine ready and waiting for connection, run the following command from the root
directory of the project:
```sh
gdb --command=./environment/gdbcommand
```

Once inside gdb, once your instruction pointer is within the hypervisor, use the `zstartl` command that was added
to gdb in the command file given to it. This command will look for the ELF header of the hypervisor and load symbols.

### Configuring Windows Visual Studio for Debugging
Once having the debug machine ready and waiting for connection, launch the command window of Visual Studio
using the Ctrl+Alt+A shortcut, and define the following alias:
```
>alias d Debug.MIDebugLaunch /Executable:C:/ /OptionsFile:C:/Projects/git/zpp_hypervisor/environment/options.xml
```
This alias will be used to attach to the target machine `gdb` interface.
Notice that the executable switch to the command is not used so it is safe to leave it as `C:/`, as for the `OptionsFile`, remember
to provide a full path to the `options.xml` file inside the `environment/options.xml` located at the project root.

Also, I recommend defining the following alias to easily execue `gdb` commands within `Visual Studio`:
```
>alias e Debug.MIDebugExec
```

Now, to attach to the target machine, execute the following command within the command window:
```
>d
```

To load symbols once the instruction pointer is within the hypervisor, execute the following command
in the command window:
```
>e zstartw
```

Final Words
-----------
I hope that you enjoy using this project and feel free to report any issues.
