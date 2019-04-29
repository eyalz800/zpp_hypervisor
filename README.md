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
4. Can later be ported without much efforts to UEFI.

Project Configuration
---------------------
The project is configured using the `environment.config` file located at the project root.
This file configures important makefile variables for compiling and debugging environment.
Example:
```make
SSH_TARGET := user@192.168.171.136
SSH_PORT := 22
SSH_PASSWORD := password1
GDB_SERVER_ADDRESS := :1337
LINUX_KERNEL := 4.18.0-15-generic
HYPERVISOR_WAIT_FOR_DEBUGGER := 0
```

Project Dependencies
--------------------
To compile the project, you need:
1. Intel 64 bit Linux environment or Linux subsystem for Windows.
2. Have clang++7 installed at least.
3. The linux headers for the specific `LINUX_KERNEL` variable inside `environment.config`.

To debug the project, you need:
1. gdb with python support.

Compiling The Project
---------------------
Compiling the project is done using the `make -j` command at the project root.
An `out` directory will be created at the project root with the built subprojects.

To compile just the hypervisor, you may use `make -j` inside the hypervisor folder.

Loading The Hypervisor
----------------------
To load the hypervisor on a remote Linux machine, use the `./environment/load.sh` script that will
push the loader driver which has the hypervisor binary within and run it.

The loader driver will load the hypervisor and exit immediately afterwards, due to an intentional
error code return to the Linux kernel, the error code is EPERM.

Note: The `./environment/load.sh` script requires `sshpass` to avoid having to type the password in SSH,
therefore it needs to be installed.

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

