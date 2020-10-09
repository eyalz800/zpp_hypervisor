zpp_hypervisor
==============
[![Build Status](https://dev.azure.com/eyalz800/zpp_hypervisor/_apis/build/status/eyalz800.zpp_hypervisor?branchName=master)](https://dev.azure.com/eyalz800/zpp_hypervisor/_build/latest?definitionId=1&branchName=master)
[![Build Status](https://travis-ci.org/eyalz800/zpp_hypervisor.svg?branch=master)](https://travis-ci.org/eyalz800/zpp_hypervisor)
[![Build status](https://ci.appveyor.com/api/projects/status/wi0kttdsy3v7jd3d/branch/master?svg=true)](https://ci.appveyor.com/project/eyalz800/zpp-hypervisor/branch/master)

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

Project Configuration
---------------------
The project is configured using the `environment.config` file located at the project root.
This file configures important makefile variables for compiling and debugging environment.
Example:
```make
# The supported architectures.
SUPPORTED_ARCHITECTURES := x86_64

# Selected architecture to build.
SELECTED_ARCHITECTURE := x86_64

# For SSH deployment, the SSH target, port, and password.
SSH_TARGET := user@192.168.171.136
SSH_PORT := 22
SSH_PASSWORD := password1

# For GDB debugging, the GDB server address.
GDB_SERVER_ADDRESS := :1337

# The drivers to build, linux, windows, uefi and both.
BUILD_DRIVERS := linux windows uefi

# Whether the hypervisor is configured to wait for debugger.
HYPERVISOR_WAIT_FOR_DEBUGGER := 0

# The linux kernel version for the linux driver.
LINUX_KERNEL := 4.18.0-15-generic

# Windows build dependencies can be referenced from either
# the windows side, WSL, or an arbitrary linux machine with
# access to the needed resources below.
ROOT :=
ifeq ($(OS), Windows_NT)
ROOT := C:
else
ROOT := /mnt/c
endif

# Windows build dependencies.
VISUAL_STUDIO_ROOT := $(ROOT)/Program\ Files\ \(x86\)/Microsoft\ Visual\ Studio/2017/Community/VC/Tools/MSVC/14.16.27023
WINDOWS_KITS_ROOT := $(ROOT)/Program\ Files\ \(x86\)/Windows\ Kits/10
WINDOWS_KITS_VERSION := 10.0.18362.0
EDK2_ROOT := $(ROOT)/Temp/edk2-UDK2018
ANDROID_NDK_ROOT := $(ROOT)/CustomPrograms/android-ndk-r19b
LLVM_ROOT := $(ROOT)/Program\ Files/LLVM
```

Compiling The Project
---------------------
This section describes what is needed to compile the project
and its subprojects.

The list of requirements varies between the host system that is used for
the build and the loader drivers that are participating in the build.

### Windows non-WSL:
Download / Install:
1. [Android NDK](https://developer.android.com/ndk/downloads) to build the Hypervisor.
2. [LLVM Releases](http://releases.llvm.org/download.html) to build the Windows and UEFI drivers.
3. [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk) to build the Windows and UEFI drivers.
4. [Windows WDK](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) to build the Windows and UEFI drivers.
5. [Tianocore EDK2](https://github.com/tianocore/edk2) - to build UEFI driver.
6. [Visual Studio Community](https://visualstudio.microsoft.com/vs/community) - for C/C++ headers use in Windows and UEFI drivers.
7. [Git](https://git-scm.com/downloads) - for use of Linux commands located in the `/usr/bin` subfolder.

- Make sure the WDK and SDK versions are the same.
- Use `$(ANDROID_NDK_ROOT)/prebuilt/windows-x86_64/bin/make.exe` for compilation.
- Prior to the build, set the PATH environment variable to have `C:/Program Files/Git/usr/bin` as first directory.
- After finishing, proceed to the environment settings part below.

### Linux / Windows WSL
Download / Install:
1. Following packages:
    * git
    * make
    * clang-7 or higher
    * clang++-7 or higher
    * lld-7 or higher
    * libc++-7-dev or higher
    * build-essential
    * libelf-dev
    * linux-headers-$(LINUX_KERNEL) where $(LINUX_KERNEL) is whatever version we want to build the Linux driver for,
      typically $(uname -r) for non Windows machines.
    * python, for generation of compile commands
2. [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk) to build the Windows and UEFI drivers.
3. [Windows WDK](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) to build the Windows and UEFI drivers.
5. [Tianocore EDK2](https://github.com/tianocore/edk2) - to build UEFI driver.
5. [Visual Studio Community](https://visualstudio.microsoft.com/vs/community) - for C/C++ headers use in Windows and UEFI drivers.

Notes:
- Make sure clang and clang++ point to the correct clang and clang++ versions.
- Installing the Windows SDK, WDK as well as Visual Studio is meant to be done
in a windows machine. The intention is to either copy the Visual Studio headers and the SDK,
WDK to the Linux machine or use a shared folder.
- After finishing, proceed to the environment settings part below.

### Environment Settings (Shared)
Make sure the `./environment.config` file contains your correct paths and settings in
your environment:
1. Adjust the `BUILD_DRIVERS` configuration to build Linux/Windows/UEFI drivers or both.
To compile just the hypervisor, leave `BUILD_DRIVERS` empty.
2. Change `HYPERVISOR_WAIT_FOR_DEBUGGER` to whether or not you wish the hypervisor to
wait for debugging.
3. For Linux driver build:
    * Adjust the `LINUX_KERNEL` variable to control linux headers version.
    * Note: under Windows this must use WSL.
4. For Windows driver build:
    * Adjust the `VISUAL_STUDIO_ROOT`, `WINDOWS_KITS_ROOT`, and `WINDOWS_KITS_VERSION` accordingly.
    * Note: Supports Linux machines as well as WSL, as long as those paths are accessible.
5. For UEFI driver build:
    * In case you are building the UEFI driver, adjust the `EDK2_ROOT` field to the EDK2 directory.
    * Make sure all requirements for Windows driver build are met.
6. For Windows non-WSL build, adjust the `LLVM_ROOT` and `ANDROID_NDK_ROOT` to the LLVM installation
folder and `ANDROID_NDK_ROOT` folder respectively.

### Compilation Command
After completing the above steps, just run `make -j` or `make -j mode=release` for to build the
project for debug or release configuration respectively.

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

