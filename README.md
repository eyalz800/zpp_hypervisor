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

Project Configuration
---------------------
The project is configured using the `environment.config` file located at the project root.
This file configures important makefile variables for compiling and debugging environment.
Example:
```make
SUPPORTED_ARCHITECTURES := x86_64
SELECTED_ARCHITECTURE := x86_64
SSH_TARGET := user@192.168.171.136
SSH_PORT := 22
SSH_PASSWORD := password1
GDB_SERVER_ADDRESS := :1337
BUILD_DRIVERS := linux windows uefi
HYPERVISOR_WAIT_FOR_DEBUGGER := 0
LINUX_KERNEL := 4.18.0-15-generic
VISUAL_STUDIO_ROOT := /mnt/c/Program\ Files\ \(x86\)/Microsoft\ Visual\ Studio/2017/Community/VC/Tools/MSVC/14.16.27023
WINDOWS_KITS_ROOT := /mnt/c/Program\ Files\ \(x86\)/Windows\ Kits/10
WINDOWS_KITS_VERSION := 10.0.18362.0
EDK2_ROOT := /mnt/c/Temp/edk2-UDK2018
```

Project Dependencies
--------------------
To compile the project, you need:
1. Intel 64 bit Linux environment or Linux subsystem for Windows.
2. Have clang++7 installed at least.
3. The linux headers for the specific `LINUX_KERNEL` variable inside `environment.config`.
4. Visual Studio and SDK/WDK for Windows support.
5. EDK2 for UEFI support.

To debug the project, you need:
1. gdb with python support.

Compiling The Project
---------------------
Make sure the `./environment.config` file contains your correct paths and settings in
your environment:
1. Adjust the `BUILD_DRIVERS` configuration to build Linux/Windows/UEFI drivers or both.
2. Change `HYPERVISOR_WAIT_FOR_DEBUGGER` to whether or not you wish the hypervisor to 
wait for debugging.
3. In case you are building the Linux driver, adjust the `LINUX_KERNEL` variable to control linux headers 
version.
4. In case you are building the Windows driver, adjust the `VISUAL_STUDIO_ROOT`, `WINDOWS_KITS_ROOT`, and `WINDOWS_KITS_VERSION` 
fields to the correct paths accessible from the Linux machine you are using for your build (can be WSL paths).
5. In case you are building the UEFI driver, adjust the `EDK2_ROOT` field to the EDK2 directory.
Also, you need to set up the Windows fields as specified in the previous point.

Compiling the project is done using the `make -j` command at the project root.
An `out` directory will be created at the project root with the built subprojects.

To compile just the hypervisor, you may use `make -j` inside the hypervisor folder.

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
There is currently no script that automatically loads the hypervisor for Windows, thus, we have
to load the driver manually.

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

The hypervisor can be configured using the `environment.config` file to perform a busy loop until
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

