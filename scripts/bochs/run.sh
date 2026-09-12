#!/bin/sh
# Starts Bochs with the gdb stub. Bochs halts at reset until gdb connects.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/bochs"
bochs="${BOCHS:-$HOME/.local/bochs-gdb/bin/bochs}"

[ -x "$bochs" ] || {
    echo "no gdbstub-enabled Bochs at $bochs - see README" >&2
    exit 1
}
[ -f "$work/esp.img" ] || {
    echo "run scripts/bochs/setup.sh first" >&2
    exit 1
}

cp "$(dirname "$0")/bochsrc.txt" "$work/bochsrc.txt"
echo "Bochs waiting for gdb on :1337 (attach with scripts/bochs/debug.sh)"
cd "$work" && exec "$bochs" -q -f bochsrc.txt
