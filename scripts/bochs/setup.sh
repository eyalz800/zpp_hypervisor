#!/bin/sh
# Builds the boot medium for running zpp_hypervisor under Bochs:
#   - OVMF.fd  : QEMU's split edk2 halves concatenated into one 4 MB flash,
#                because Bochs loads exactly one ROM image.
#   - esp.img  : a FAT image with the UEFI loader as the default boot path.
# Written with mtools so no mounting (and no sudo) is needed.
set -e

config="${1:-debug}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/out/$config/x86_64"
work="$root/build/bochs"
qemu_share="${QEMU_SHARE:-/opt/homebrew/share/qemu}"

[ -f "$out/zpp_loader.efi" ] || {
    echo "missing $out/zpp_loader.efi - build the $config preset first" >&2
    exit 1
}

mkdir -p "$work"

# Combined firmware. The vars half must come first: vars (0x84000) plus
# code (0x37C000) is exactly 0x400000, which is what OVMF expects to be
# mapped as a single flash device.
cat "$qemu_share/edk2-i386-vars.fd" "$qemu_share/edk2-x86_64-code.fd" \
    > "$work/OVMF.fd"

size=$(wc -c < "$work/OVMF.fd" | tr -d ' ')
[ "$size" = "4194304" ] || {
    echo "OVMF.fd is $size bytes, expected 4194304" >&2
    exit 1
}

# ESP holding the loader at the removable-media default path.
rm -f "$work/esp.img"
dd if=/dev/zero of="$work/esp.img" bs=1m count=64 2>/dev/null
export MTOOLS_SKIP_CHECK=1
mformat -i "$work/esp.img" -F -v ZPPESP ::
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$out/zpp_loader.efi" ::/EFI/BOOT/BOOTX64.EFI

echo "boot medium ready in $work (config: $config)"
