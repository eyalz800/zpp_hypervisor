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

# Firmware. Bochs loads exactly one ROM image, and bochsrc.txt maps it at
# 0xffc00000, so a single 4 MB image is required. Distributions disagree on both
# the directory and the naming, and some ship it split in two, so search rather
# than assume:
#   OVMF.fd                                    combined (Debian /usr/share/ovmf)
#   OVMF_VARS_4M.fd + OVMF_CODE_4M.fd          split    (Debian /usr/share/OVMF)
#   edk2-i386-vars.fd + edk2-x86_64-code.fd    split    (QEMU, incl. Homebrew)
# The vars half always comes first; vars plus code is exactly 0x400000.
firmware_dirs="${QEMU_SHARE:-} /usr/share/ovmf /usr/share/OVMF /usr/share/qemu /opt/homebrew/share/qemu /usr/local/share/qemu"

found=""
for dir in $firmware_dirs; do
    [ -d "$dir" ] || continue
    if [ -f "$dir/OVMF.fd" ]; then
        cp "$dir/OVMF.fd" "$work/OVMF.fd"
        found="$dir/OVMF.fd"
    elif [ -f "$dir/OVMF_VARS_4M.fd" ] && [ -f "$dir/OVMF_CODE_4M.fd" ]; then
        cat "$dir/OVMF_VARS_4M.fd" "$dir/OVMF_CODE_4M.fd" > "$work/OVMF.fd"
        found="$dir/OVMF_{VARS,CODE}_4M.fd"
    elif [ -f "$dir/edk2-i386-vars.fd" ] && [ -f "$dir/edk2-x86_64-code.fd" ]; then
        cat "$dir/edk2-i386-vars.fd" "$dir/edk2-x86_64-code.fd" > "$work/OVMF.fd"
        found="$dir/edk2-{i386-vars,x86_64-code}.fd"
    else
        continue
    fi

    size=$(wc -c < "$work/OVMF.fd" | tr -d ' ')
    if [ "$size" = "4194304" ]; then
        break
    fi

    # Wrong size, most likely the 2 MB variant. Keep looking.
    echo "note: $found is $size bytes, not the 4 MB bochsrc expects - skipping" >&2
    rm -f "$work/OVMF.fd"
    found=""
done

if [ -z "$found" ]; then
    echo "no usable 4 MB OVMF firmware found. Looked in:$firmware_dirs" >&2
    echo "Install one (Debian: ovmf, macOS: qemu) or set QEMU_SHARE." >&2
    exit 1
fi

echo "firmware: $found"

# ESP holding the loader at the removable-media default path.
rm -f "$work/esp.img"
# 1M rather than 1m: GNU dd rejects the lowercase suffix that BSD dd accepts.
# stderr is deliberately not suppressed - hiding it here cost a CI cycle.
dd if=/dev/zero of="$work/esp.img" bs=1M count=64 status=none
export MTOOLS_SKIP_CHECK=1
mformat -i "$work/esp.img" -F -v ZPPESP ::
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$out/zpp_loader.efi" ::/EFI/BOOT/BOOTX64.EFI

echo "boot medium ready in $work (config: $config)"
