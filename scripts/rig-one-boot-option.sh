#!/bin/sh
# Erase every boot option in the guest's NVRAM and install exactly one:
# ours.
#
# This is not tidiness. Windows writes its own boot option and puts it at
# the front of BootOrder every time it completes a boot, so the firmware
# stops reaching our loader and the next run is Windows on the metal -
# which looks like a successful boot and is a control run. That has
# already invalidated a measurement and the retraction of it, both taken
# from boots with no hypervisor in them and both read as evidence.
#
# So it is done before every boot rather than when someone remembers.
# `scripts/rig-trace.sh` refuses a capture whose serial carries no
# ZPP_TRACE, which catches the same failure after the fact; this prevents
# it.
#
# The template is .pristine, never .orig - no saved copy but .pristine has
# a boot option for our loader, and resetting from .orig silently boots
# Windows.
set -e

RIG=${ZPP_TARGET:-tc@192.168.1.199}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $RIG"
here=$(dirname "$0")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

$SSH 'cat /home/tc/vm/RELEASEX64_OVMF_VARS.fd.pristine' > "$work/pristine.fd"

if [ ! -s "$work/pristine.fd" ]; then
    echo "could not read the pristine varstore from the rig" >&2
    exit 1
fi

python3 "$here/uefi-varstore.py" \
    --only-boot-option '\EFI\zpp\zpp_loader.efi' \
    --description 'zpp hypervisor' \
    --partition 2 \
    --partition-guid 2CC5F47B-1672-4BEB-9C70-FF8869A75262 \
    --first-lba 32768 \
    --sectors 33554432 \
    --timeout 1 \
    --output "$work/one.fd" "$work/pristine.fd" > /dev/null

local_sum=$(md5 -q "$work/one.fd" 2>/dev/null || md5sum "$work/one.fd" | cut -d' ' -f1)
remote_sum=$(cat "$work/one.fd" | $SSH \
    'cat > /home/tc/vm/RELEASEX64_OVMF_VARS.fd && md5sum /home/tc/vm/RELEASEX64_OVMF_VARS.fd' \
    | cut -d' ' -f1)

if [ "$local_sum" != "$remote_sum" ]; then
    echo "varstore did not land intact: $local_sum vs $remote_sum" >&2
    exit 1
fi

echo "one boot option installed: zpp hypervisor (verified $local_sum)"
