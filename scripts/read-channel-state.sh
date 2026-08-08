#!/bin/bash
# Reads the resident side's own channel counters out of the running guest,
# through the emulator's monitor.
#
# The medium only says what reached the medium. When the channel goes
# quiet the question is which of three things happened - the guest stopped
# exiting, records are produced and not drained, or they are drained and
# the device write fails - and all three look identical from the disk.
# They are trivially distinguished from the resident side's memory.
#
# **This exists because doing it by hand is wrong more often than right.**
# Both halves of the address move: the symbol offsets shift whenever the
# module's layout changes, and the base shifts when its size does. Two
# builds one commit apart had the sink's statics 0x1000 apart, and a base
# that had moved by the same amount in the other direction - so a reading
# taken with one build's offsets and another's base came out
# self-consistent and completely wrong. Both are taken from the same run
# here, every time.
set -u

RIG_HOST=${RIG_HOST:-192.168.1.199}
RIG=${RIG:-tc@$RIG_HOST}
MONITOR_PORT=${MONITOR_PORT:-4446}
BINARY=${1:-out/debug/x86_64/zpp_hypervisor}
NM=${NM:-/opt/homebrew/opt/llvm/bin/llvm-nm}
DWARF=${DWARF:-/opt/homebrew/opt/llvm/bin/llvm-dwarfdump}

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o BatchMode=yes"

say() { printf '%s\n' "$*" >&2; }

[ -f "$BINARY" ] || { say "no such binary: $BINARY"; exit 2; }

# shellcheck disable=SC2086
base=$(timeout 30 ssh $SSH_OPTS "$RIG" \
    'grep -aoE "allocate_rwx done at 0x[0-9a-fA-F]+" ~/zpp/serial.out | tail -1' \
    2>/dev/null | grep -oE '0x[0-9a-fA-F]+')

if [ -z "${base:-}" ]; then
    say "could not read the module base from the rig's serial log."
    say ""
    say "The usual cause is not the guest. That line is printed by the"
    say "loader's serial trace, which is off by default:"
    say ""
    say "    grep ZPP_TRACE build/debug/CMakeCache.txt"
    say ""
    say "If it says OFF, rebuild with -DZPP_TRACE=ON and boot again. The"
    say "channel does not depend on it - ZPP_DIAG is a separate switch and"
    say "the log still reaches the disk - so a run with the trace off looks"
    say "exactly like a healthy boot until you try to read memory and have"
    say "no base to read it at. It has already cost one session's worth of"
    say "readings taken at an address inherited from an earlier build."
    say ""
    say "Otherwise: is the guest running, and has the loader got that far?"
    exit 1
fi

# The serial log outlives the guest that wrote it, so a base read out of
# it proves nothing about anything currently running. Without this the
# script happily reports a whole screen of zeroes taken from a stale base
# against a dead machine, which reads exactly like a channel that never
# came up - the one conclusion it exists to let you avoid drawing by
# accident.
if ! { printf 'info status\n'; sleep 1; } \
    | timeout 12 nc -w 8 "$RIG_HOST" "$MONITOR_PORT" 2>/dev/null \
    | grep -qa 'VM status'; then
    say "the monitor on $RIG_HOST:$MONITOR_PORT is not answering."
    say "nothing is running, so every reading below would be stale."
    say "boot the guest with:"
    say "  ZPP_QEMU_EXTRA=\"-monitor telnet:0.0.0.0:$MONITOR_PORT,server,nowait\""
    exit 1
fi

say "module base $base   binary $BINARY"

mon() {
    { printf '%s\n' "$1"; sleep 2; } \
        | timeout 20 nc -w 12 "$RIG_HOST" "$MONITOR_PORT" 2>/dev/null \
        | LC_ALL=C tr -d '\r' | grep -aE '^0000'
}

# A static, by mangled-name fragment.
static() { $NM "$BINARY" | grep -E "$1" | head -1 | awk '{print $1}'; }

# A member of the singleton, by name, as base + instance + offset.
# Prefixed, because llvm-nm prints a bare hex address with leading zeros
# and the arithmetic below is done in python, where a leading zero makes it
# an invalid decimal literal rather than the octal it used to be. The
# static path already prefixed its own; this one did not, so every member
# read failed while every static read worked - which looks like the
# members being absent rather than the script being wrong.
instance=0x$($NM "$BINARY" \
    | grep -E '^[0-9a-f]+ b _ZZN3zpp10hypervisor10hypervisor8instanceEvE8instance$' \
    | awk '{print $1}')

member() {
    $DWARF --name=hypervisor --show-children "$BINARY" 2>/dev/null \
        | grep -A4 "DW_AT_name.*\"$1\"" \
        | grep -m1 'data_member_location' \
        | grep -oE '\(0x[0-9a-f]+\)' | tr -d '()'
}

at() { python3 -c "print(hex($base + $1))"; }

show_static() {
    local addr; addr=$(static "$2")
    [ -n "$addr" ] || { printf '  %-26s <no symbol>\n' "$1"; return; }
    printf '  %-26s ' "$1"
    mon "xp/${3:-1}gx $(at "0x$addr")" | sed 's/^[^:]*: //' | tr '\n' ' '
    printf '\n'
}

show_member() {
    local off; off=$(member "$2")
    [ -n "$off" ] || { printf '  %-26s <no member>\n' "$1"; return; }
    printf '  %-26s ' "$1"
    mon "xp/${3:-1}gx $(at "$instance + $off")" | sed 's/^[^:]*: //' | tr '\n' ' '
    printf '\n'
}

echo "--- is the guest exiting at all ---"
show_member "heartbeat_exits_seen[8]" heartbeat_exits_seen 8

echo "--- are records being produced ---"
show_static  "ring written[8]" 'ring_storageIX.*E7writtenE$' 8
show_static  "gate held" 'gateILNS0_4sinkE0EE4heldE'

echo "--- is the sink draining ---"
show_static  "configure_reject|staged" 'esp_blocks_forILNS0_4sinkE4EE16configure_rejectE'
show_static  "epoch" 'esp_blocks_forILNS0_4sinkE4EE5epochE'
show_static  "sequence" 'esp_blocks_forILNS0_4sinkE4EE8sequenceE'
show_static  "next_block_index" 'esp_blocks_forILNS0_4sinkE4EE16next_block_indexE'
show_static  "dropped" 'esp_blocks_forILNS0_4sinkE4EE7droppedE'

echo "--- is the device still ours ---"
show_static  "queue bound (0 = forgotten)" 'queue_pairILj64EE5boundE' 4
show_static  "lost_to_reset" 'queue_pairILj64EE13lost_to_resetE'

echo "--- emulation and rebuild ---"
show_member  "emulated_writes" emulated_writes
show_member  "length disagreement" emulated_length_disagreement
show_member  "rebuild re-entered" channel_rebuild_reentered
show_member  "excursions completed" excursions_completed
show_member  "borrow result" channel_rebuild_result
show_member  "borrow ticks" channel_rebuild_ticks

echo "--- the reservation, at the enable ---"
show_member  "reserve result" channel_reserve_result
show_member  "reserve status" channel_reserve_status
show_member  "reserve allocation (DW0)" channel_reserve_allocation
show_member  "quiesce sqhd|expected" channel_quiesce_submission_tail 2

echo "--- what the guest was told it was granted ---"
show_member  "grant result" channel_grant_result
show_member  "grant as the controller wrote" channel_grant_reported
show_member  "grant as the guest reads it" channel_grant_presented
show_member  "grant already posted" channel_grant_already_posted
show_member  "grant entries stepped past" channel_grant_scanned
show_member  "grant ticks" channel_grant_ticks
show_member  "granted sq (told)" channel_guest_granted_submission_queues
show_member  "granted cq (told)" channel_guest_granted_completion_queues

echo "--- what the guest's driver did ---"
show_member  "guest requested sq" channel_guest_requested_submission_queues
show_member  "guest requested cq" channel_guest_requested_completion_queues
show_member  "guest highest sq" channel_guest_highest_submission_queue
show_member  "guest highest cq" channel_guest_highest_completion_queue
show_member  "guest created sq" channel_guest_created_submission_queues
show_member  "guest created cq" channel_guest_created_completion_queues
show_member  "guest deleted queues" channel_guest_deleted_queues

echo "--- creating ours above them ---"
show_member  "create result" channel_create_result
show_member  "create status (cq|sq)" channel_create_status
show_member  "create ticks" channel_create_ticks
show_member  "doorbell writes" channel_doorbell_writes

cat >&2 <<'NOTES'

Reading it:
  heartbeat_exits_seen climbing        the guest is exiting; silence is not its fault
  ring written climbing, sequence flat produced but not written - look at the device
  queue bound all zero                 forget() ran; the guest reset the controller
  configure_reject = 1                 that is reject::none, success, not a rejection
  epoch = 2                            the queue pair was rebuilt after a reset

The reservation and the creation, which are two different runs:
  reserve result 0                     never attempted - the enable was not seen
  reserve result 1                     the allocation is ours; read it in DW0,
                                       NSQA in bits 15:0 and NCQA in 31:16, both
                                       zero's based, so add one to each
  reserve result 0xe0                  the controller refused the Set Features;
                                       the status beside it says why, and 0x0c
                                       generic is Command Sequence Error, meaning
                                       an I/O queue already existed
  reserve result 0xf8                  the admin queue never fell quiet; the two
                                       quiesce words are the SQHD seen and the
                                       tail expected, and they should be equal
  reserve result 0xf7                  a processor never acknowledged the doorbell
                                       protection, so nothing was borrowed
The grant, which is what leaves the channel an identifier at all:
  grant result 0                       never attempted - the guest's own Set
                                       Features was not seen on the doorbell
  grant result 1                       the answer was reduced; the two words
                                       beside it are DW0 as the controller
                                       wrote it and DW0 as the guest reads it,
                                       and 0x000f000f becoming 0x000f000e is
                                       sixteen submission queues shown as
                                       fifteen
  grant result 2                       no reduction was needed - the guest
                                       asked for less than the allocation in
                                       both spaces, so an identifier was
                                       already free
  grant result 0xe8                    the guest was answered something other
                                       than what the reservation was granted;
                                       the ordering argument is wrong and
                                       nothing was edited
  grant result 0xf4                    the completion never arrived
  grant already posted 1               the controller answered before this got
                                       to look, which is expected some of the
                                       time and is handled
  granted sq (told) 15                 what the guest's creates are clamped by,
                                       and therefore what decides the identifier
                                       chosen above them
  create result 0xe2                   no room above what the guest took - the
                                       four guest numbers say whether that is the
                                       guest being greedy or the allocation being
                                       small
  create result 0xe1                   the controller refused a Create; 0x4101 in
                                       either half is Invalid Queue Identifier
  create result 1 and epoch 2          the channel is back on its own queue pair
Sample twice with a gap. A single reading of a counter says almost nothing.
NOTES
