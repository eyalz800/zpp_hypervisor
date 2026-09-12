#!/bin/sh
# What KVM offers a nested guest, read from the rig's own KVM.
#
# The comparison this investigation needed: the rig boots this Windows
# installation under KVM and not under this VMM, so the difference is
# something presented rather than something done wrong. KVM_GET_MSRS on
# /dev/kvm answers the VMX capability MSRs a guest hypervisor reads,
# which is the menu Hyper-V configures itself from.
#
# The probe is cross-compiled here because the rig runs from RAM and
# carries neither a compiler nor a libc. Freestanding, raw syscalls,
# static - see kvm-nested-msrs.c.
set -eu

rig=${1:-192.168.1.199}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

clang --target=x86_64-unknown-linux-gnu -fuse-ld=lld -nostdlib -static \
    -ffreestanding -fno-stack-protector -O1 \
    -o "$out/probe" "$(dirname "$0")/kvm-nested-msrs.c"

cat "$out/probe" | ssh -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null "tc@$rig" \
    'cat > /tmp/kvm-msrs && chmod +x /tmp/kvm-msrs && /tmp/kvm-msrs && rm -f /tmp/kvm-msrs'
