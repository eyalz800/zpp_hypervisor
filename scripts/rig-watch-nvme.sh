#!/bin/sh
# Poll the passed-through NVMe's controller registers from the start of a
# run, so "when did it stop answering" is a measurement rather than a
# recollection.
#
# Why this exists. A guest was found stalled with the whole storage stack
# and `Ntfs.sys` resident and nothing read from the volume, and the
# controller's BAR read all-ones. Two neighbouring passed-through BARs
# read correctly in the same batch, so the read mechanism was working -
# but minutes later the host itself became unreachable, and a control
# taken *before* a failure cannot rule out that the failure and the host
# loss were one event. The reading was recorded as an observation and
# withdrawn as a diagnosis for exactly that reason.
#
# What settles it is the *first* all-ones, timestamped, against a host
# that is still up. Hence polling from the start rather than reading once
# at the end.
#
# Three rules from CLAUDE.md are baked in, each of which has already
# produced a wrong answer here:
#
#   - Read device registers NARROW and repeat. A wide `xp` over this same
#     BAR once reported `CC.EN = 0` on a controller that was enabled.
#   - Read a CONTROL in the same batch. Two neighbouring BARs answer if
#     the mechanism works, so all-ones from one device is the device and
#     not the reader.
#   - The monitor takes exactly ONE connection. Each sample opens and
#     closes; nothing is held between samples, or every later reader in
#     the session breaks.
#
# Doorbells are deliberately not read: they are write-only, so a zero
# says nothing about what was submitted, and that has been mistaken for
# "no I/O was issued" before.
# DO NOT diagnose an all-ones device by reading its HOST config space.
# `sudo lspci -vv -s <dev>` or a hexdump of
# /sys/bus/pci/devices/*/config on a vfio-bound device that has stopped
# answering **hangs the whole machine** - measured 2026-08-27, the host
# was unreachable within seconds and needed a physical power cycle. It
# is also the most likely explanation for an earlier unexplained host
# loss that forced an NVMe observation to be withdrawn. Reading the
# guest-physical BAR with `xp`, as below, is safe; host config space is
# not.
set -u

RIG=${ZPP_RIG:-192.168.1.199}
PORT=${ZPP_MONITOR_PORT:-4446}
INTERVAL=${1:-15}
NVME=${ZPP_NVME_BAR:-0x7011108000}
# The controls. Any two BARs that are not the device under test; these are
# the xhci and the neighbour that answered when the NVMe did not.
CTRL_A=${ZPP_CTRL_A:-0x7011100000}
CTRL_B=${ZPP_CTRL_B:-0x701110c000}

# CC is at +0x14 and CSTS at +0x1c. **+0x18 is RESERVED** - reading a
# two-word pair at +0x14 gets CC and the reserved dword, which is
# legitimately zero, and a zero there reads exactly like CSTS.RDY=0 on a
# controller that never came ready. That misreading was made here and
# reported as "the controller is enabled but not ready" before the
# register map was checked. Two separate narrow reads, at the two real
# offsets.
CC=$(printf '0x%x' $(( NVME + 0x14 )))
CSTS=$(printf '0x%x' $(( NVME + 0x1c )))

# `$CC_CSTS` was named here and assigned nowhere - the pair was split
# into `$CC` and `$CSTS` above and this line was not updated. Under
# `set -u` that is an unbound-variable error on the line BEFORE the
# polling loop, so this script has exited 127 without ever taking a
# sample, and every "first all-ones at HH:MM:SS" reading it exists to
# produce has never been produced.
echo "polling $NVME (CC at $CC, CSTS at $CSTS) every ${INTERVAL}s"
echo "controls: $CTRL_A $CTRL_B - if these answer and the NVMe does not,"
echo "          the device is the problem and not the reader"
echo

first_bad=""
while :; do
    now=$(date +%H:%M:%S)

    if ! ping -c 1 -W 2 "$RIG" >/dev/null 2>&1; then
        echo "$now  HOST UNREACHABLE - stop here; anything after this is"
        echo "         a reading of a machine that is going away, and the"
        echo "         last device reading before it is not attributable"
        exit 2
    fi

    out=$(printf 'xp /2xw %s\nxp /2xw %s\nxp /2xw %s\nxp /2xw %s\n' \
              "$CC" "$CSTS" "$CTRL_A" "$CTRL_B" \
          | nc -w 8 "$RIG" "$PORT" 2>/dev/null \
          | tr -d '\r' | grep -oE '0x[0-9a-f]{8} 0x[0-9a-f]{8}')

    nvme=$(echo "$out" | sed -n 1p)
    csts=$(echo "$out" | sed -n 2p)
    a=$(echo "$out" | sed -n 3p)
    b=$(echo "$out" | sed -n 4p)

    if [ -z "$nvme" ]; then
        echo "$now  no answer from the monitor (is another reader holding it?)"
    elif [ "$nvme" = "0xffffffff 0xffffffff" ]; then
        if [ "$a" = "0xffffffff 0xffffffff" ] &&
           [ "$b" = "0xffffffff 0xffffffff" ]; then
            echo "$now  ALL THREE all-ones - the reader or the machine, NOT"
            echo "         the device. Do not record this as a device fault."
        else
            if [ -z "$first_bad" ]; then
                first_bad=$now
                echo "$now  *** FIRST ALL-ONES on the NVMe while the controls"
                echo "         still answer: $a / $b"
                echo "         The device stopped, the host is up, and this"
                echo "         timestamp is the measurement."
            else
                echo "$now  NVMe all-ones (first seen $first_bad)"
            fi
        fi
    else
        cc=$(echo "$nvme" | cut -d' ' -f1)
        st=$(echo "$csts" | cut -d' ' -f1)
        en=$(( $(printf '%d' "$cc") & 1 ))
        rdy=$(( $(printf '%d' "$st") & 1 ))
        echo "$now  CC $cc CSTS $st  (EN=$en RDY=$rdy)"
    fi

    sleep "$INTERVAL"
done
