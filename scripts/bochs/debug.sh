#!/bin/sh
# Attaches the cross gdb to a running Bochs gdb stub.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
config="${1:-debug}"
gdb="${GDB:-/opt/homebrew/bin/x86_64-elf-gdb}"

[ -x "$gdb" ] || {
    echo "no x86_64-elf-gdb - brew install x86_64-elf-gdb" >&2
    exit 1
}

exec "$gdb" \
    -ex "set architecture i386:x86-64" \
    -ex "set confirm off" \
    -ex "source $root/scripts/load-symbols" \
    -ex "target remote :1337" \
    -ex "echo \n=== attached. hypervisor symbols are not loaded yet ===\n" \
    -ex "echo Once the loader has mapped the ELF, run:\n" \
    -ex "echo   load-symbols \$rip $root/out/$config/x86_64/zpp_hypervisor\n" \
    -ex "echo If built with ZPP_HYPERVISOR_WAIT_FOR_DEBUGGER=1, release it with:\n" \
    -ex "echo   set var zpp::hypervisor::gdb_attached = 1\n"
