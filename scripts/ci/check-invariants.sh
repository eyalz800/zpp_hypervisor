#!/bin/sh
# Asserts the ELF level invariants this codebase relies on. Nothing in the
# compiler or linker enforces these, and each one has already caught a real
# regression during development.
set -e

config="${1:-debug}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
elf="$root/out/$config/x86_64/zpp_hypervisor"
nm="${NM:-llvm-nm}"
readelf="${READELF:-llvm-readelf}"

[ -f "$elf" ] || { echo "missing $elf" >&2; exit 1; }

status=0

# 1. The hypervisor links with --no-undefined, but check anyway: an undefined
#    symbol here means a CRT or ABI function is missing at runtime.
undefined=$("$nm" -u "$elf" 2>/dev/null | grep -v 'no symbols' || true)
if [ -n "$undefined" ]; then
    echo "FAIL: undefined symbols in $config hypervisor:" >&2
    echo "$undefined" >&2
    status=1
else
    echo "ok: no undefined symbols"
fi

# 2. Dynamic initialization. Every global is currently constant initialized,
#    so there should be no init array at all. This is the check that catches
#    someone accidentally introducing a global needing a runtime constructor.
init_array=$("$readelf" -S "$elf" | grep -c 'INIT_ARRAY' || true)
if [ "$init_array" != "0" ]; then
    echo "WARN: .init_array present - dynamic initialization is in use." >&2
    echo "      That is supported (zpp::crt::init::main walks it), but it" >&2
    echo "      should be a deliberate choice. Sections:" >&2
    "$readelf" -S "$elf" | grep -E 'INIT_ARRAY|FINI_ARRAY' >&2
else
    echo "ok: no .init_array, every global is constant initialized"
fi

# 3. No C runtime startup exists, so these must never appear.
for symbol in _GLOBAL__sub_I __libc_start_main; do
    if "$nm" "$elf" 2>/dev/null | grep -q "$symbol"; then
        echo "FAIL: $symbol present - expects a C runtime that does not exist" >&2
        status=1
    fi
done
echo "ok: no hosted C runtime entry points"

exit "$status"
