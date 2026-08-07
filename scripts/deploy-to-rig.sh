#!/bin/sh
# Deploy the built loader to the rig's real EFI system partition, and
# refuse unless what is on the disk afterwards is byte for byte what was
# just built.
#
# Why this exists rather than an scp and a glance: a stale binary does not
# look like a stale binary, it looks like a bug. Twice in one session the
# rig booted a loader from hours earlier - once because the target restored
# an old boot script from its RAM-disk backup and started booting a
# different device, once because a copy was verified through the page cache
# it had just populated instead of from the medium. Both times the hang was
# real, the binary was stale, and every conclusion drawn from the run was
# worthless.
#
# So this does three things the manual version kept getting wrong:
#   - reads back from a FRESH mount, never the one that did the writing
#   - records the deployed hash, so a boot can be checked against it
#   - refuses loudly rather than warning
set -e

TARGET=${ZPP_TARGET:-tc@192.168.1.199}
LOADER=${1:-out/debug/x86_64/zpp_loader.efi}
DEST=${ZPP_DEST:-/EFI/zpp/zpp_loader.efi}
PART=${ZPP_PART:-/dev/nvme0n1p2}
MOUNT=/mnt/zppdeploy

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $TARGET"

if [ ! -f "$LOADER" ]; then
    echo "FAIL: no loader at $LOADER"
    exit 1
fi

WANT=$(md5 -q "$LOADER" 2>/dev/null || md5sum "$LOADER" | cut -d' ' -f1)
echo "built:    $WANT  $LOADER"

# The device has to be back on the host driver, which it is not while the
# VM holds it through VFIO. Say so plainly rather than failing on a mount.
if ! $SSH "test -b $PART" 2>/dev/null; then
    echo "FAIL: $PART is not present on the target."
    echo "      The VM probably still holds the NVMe through vfio-pci."
    exit 1
fi

# Use wherever it is already mounted, and never assume a mount worked.
#
# The target mounts this partition at boot, so a second mount of it fails -
# and a failed mount is not the problem. The problem is what happens next:
# writing to the mount point then writes to a directory on the root file
# system, which succeeds, so the copy lands in RAM and the disk is never
# touched. That happened, and the only reason it was noticed is that the
# read-back came up empty rather than wrong.
EXISTING=$($SSH "mount | grep '^$PART ' | head -1 | cut -d' ' -f3" 2>/dev/null | tr -d '\r')

if [ -n "$EXISTING" ]; then
    MOUNT=$EXISTING
    echo "already mounted at $MOUNT"
else
    $SSH "sudo mkdir -p $MOUNT && sudo mount $PART $MOUNT" 2>/dev/null
    if ! $SSH "mount | grep -q ' $MOUNT '" 2>/dev/null; then
        echo "FAIL: $PART is not mounted at $MOUNT and could not be mounted."
        echo "      Refusing to write - the copy would land on the root"
        echo "      file system and look like it had worked."
        exit 1
    fi
fi

# shellcheck disable=SC2086
cat "$LOADER" | $SSH "sudo tee $MOUNT$DEST > /dev/null && sync"

# The read-back, from a mount that did not just write the file.
GOT=$($SSH "sudo umount $MOUNT >/dev/null 2>&1; sudo mount -o ro $PART $MOUNT >/dev/null 2>&1; md5sum $MOUNT$DEST | cut -d' ' -f1; sudo umount $MOUNT >/dev/null 2>&1; sudo mount $PART $MOUNT >/dev/null 2>&1" 2>/dev/null | tr -d '\r')

echo "on disk:  $GOT"
if [ "$WANT" != "$GOT" ]; then
    echo "FAIL: what is on the disk is not what was built."
    exit 1
fi

echo "$WANT" > .rig-deployed-hash
echo "OK: deployed and verified from a fresh mount."
echo "    hash recorded in .rig-deployed-hash - check any confusing boot"
echo "    against it before debugging the failure itself."
