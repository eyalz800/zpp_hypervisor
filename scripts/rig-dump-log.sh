#!/bin/sh
# Dump the resident hypervisor's log ring off a running guest, without
# restarting it and without perturbing what it is doing.
#
# Every step here exists because doing it by hand lost a run:
#
#   - The module base is printed on serial and is NOT stable across runs,
#     so it is read from the rig every time rather than remembered.
#   - The launcher used to truncate serial.out on start, so a relaunch
#     destroyed the base of the run being debugged. boot-zpp.sh now keeps
#     serial.out.prev; this reads whichever of the two has a base.
#   - QEMU's monitor accepts ONE connection. A probe that opens it and
#     leaves it open makes every later query queue unaccepted, and the
#     monitor looks dead while QEMU is fine. So this uses the gdb stub on
#     its own socket, which the launcher opens with -gdb, and never the
#     monitor.
#
# Output goes to /tmp/zpp.log, always, so there is one place to look.
set -e

RIG=${ZPP_TARGET:-tc@192.168.1.199}
PORT=${ZPP_GDB_PORT:-1234}
OUT=${ZPP_LOG_OUT:-/tmp/zpp.log}
ELF=${1:-out/debug/x86_64/zpp_hypervisor}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 $RIG"

[ -f "$ELF" ] || { echo "no such hypervisor ELF: $ELF" >&2; exit 1; }

# The CURRENT run's last base, and only if this run has one. Reading both
# files at once put serial.out.prev last, so `tail -1` answered with the
# base of the run before this one - and gdb then read memory that had been
# reallocated, reporting "Cannot access memory" as though the module were
# gone. A guest that resets reloads the module at a NEW address, so the
# last line of the live file is the only right answer.
BASE=$($SSH 'grep -ah "allocate_rwx done at" /home/tc/zpp/serial.out 2>/dev/null | tail -1' \
       | tr -d '\r' | sed 's/.*done at //')

case "$BASE" in
    0x*) ;;
    *)   echo "no module base on serial - did the loader run?" >&2; exit 1 ;;
esac
echo "module base: $BASE"

# How far the module reaches, so a processor can be told to be inside it.
# Every read below is of a page the module has hidden from the guest, so it
# only answers on a processor in root operation - see zppcpu.
# awk is BSD awk here and has no strtonum, so the hex arithmetic is the
# shell's.
SPAN=0
for pair in $(llvm-readelf -l "$ELF" 2>/dev/null \
              | awk '/^  LOAD/ {print $3 "+" $6}'); do
    end=$(( ${pair%+*} + ${pair#*+} ))
    [ "$end" -gt "$SPAN" ] && SPAN=$end
done
[ "$SPAN" -gt 0 ] || SPAN=$((0x400000))
CPUS=${ZPP_CPUS:-8}

# Attaching samples one instant, and at that instant every processor may
# be in guest context - which is the normal case for a guest that is
# running well, since that is where a guest spends its time. Nothing can
# be read then, so this samples again rather than reporting a healthy
# guest as unreadable. Attach, look, detach: each attempt lands somewhere
# else, and the guest keeps running between them.
ATTEMPTS=${ZPP_DUMP_ATTEMPTS:-12}

attempt=1
while [ "$attempt" -le "$ATTEMPTS" ]; do
    # batch mode, so a stub that never answers ends the run instead of
    # leaving an interactive gdb nobody is watching.
    OUTPUT=$(x86_64-elf-gdb -q -batch \
        -ex "set confirm off" \
        -ex "set pagination off" \
        -ex "target remote ${RIG#*@}:$PORT" \
        -ex "add-symbol-file $ELF -o $BASE" \
        -ex "source scripts/zpp.gdb" \
        -ex "info threads" \
        -ex "set \$zpp_base = $BASE" \
        -ex "set \$zpp_end = $BASE + $SPAN" \
        -ex "set \$zpp_cpus = $CPUS" \
        -ex "zppcpu" \
        -ex "set logging file $OUT" \
        -ex "set logging redirect on" \
        -ex "set logging overwrite on" \
        -ex "set logging enabled on" \
        -ex "zpplog" \
        -ex "zppwhy" \
        -ex "zppstat" \
        -ex "zppexits 0" \
        -ex "zppexits 1" \
        -ex "zppexits 2" \
        -ex "zppexits 3" \
        -ex "zppexits 4" \
        -ex "zppexits 5" \
        -ex "zppexits 6" \
        -ex "zppexits 7" \
        -ex "set logging enabled off" \
        -ex "detach" 2>&1)

    case "$OUTPUT" in
        *"could not connect"*)
            echo "$OUTPUT" | tail -3
            echo "no gdb stub on port $PORT - launch with" >&2
            echo "  ZPP_QEMU_EXTRA=\"-gdb tcp:0.0.0.0:$PORT\" sudo -E ./boot-zpp.sh" >&2
            exit 1
            ;;
        *"reading from cpu"*)
            echo "$OUTPUT" | grep "reading from cpu"
            break
            ;;
    esac

    echo "attempt $attempt: every processor was in guest context"
    attempt=$((attempt + 1))
done

if [ "$attempt" -gt "$ATTEMPTS" ]; then
    echo "no processor entered the module in $ATTEMPTS attaches." >&2
    echo "the guest may be running entirely in guest context - which is" >&2
    echo "healthy - or stopped. Ask the monitor: info status." >&2
    exit 1
fi

echo "log ring in $OUT: $(wc -l < "$OUT") lines"
