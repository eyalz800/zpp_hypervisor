#!/bin/sh
# Dump the resident hypervisor's log ring through the emulator's monitor,
# reading **physical** memory, with no debugger and no processor inside
# the module.
#
# This exists because scripts/rig-dump-log.sh cannot always work. That one
# attaches gdb, which reads through the *current* processor's page tables -
# and the module hides its own pages from the guest by clearing every
# extended-page-table permission on them. So it needs a processor in root
# operation, and a guest that has settled has none: eight processors
# halted in their own idle loop is the normal state of the failure worth
# debugging, and forty attaches in a row found nobody.
#
# The monitor's `xp` reads physical memory, which bypasses both the
# extended page tables and guest paging. The module is loaded at a
# physical address it prints on serial and is identity mapped, so every
# pointer inside it is usable as a physical address directly.
#
# Output goes to /tmp/zpp.log, the same place as the gdb path, so there is
# one place to look.
set -e

RIG=${ZPP_TARGET:-tc@192.168.1.199}
MONITOR=${ZPP_MONITOR_PORT:-4446}
OUT=${ZPP_LOG_OUT:-/tmp/zpp.log}
ELF=${1:-out/debug/x86_64/zpp_hypervisor}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 $RIG"

[ -f "$ELF" ] || { echo "no such hypervisor ELF: $ELF" >&2; exit 1; }

# The module base, from this run's serial log. Not stable across runs, and
# reading the previous run's would walk reallocated memory that answers
# with plausible rubbish rather than an error.
BASE=$($SSH 'grep -ah "allocate_rwx done at" /home/tc/zpp/serial.out 2>/dev/null | tail -1' \
       | tr -d '\r' | sed 's/.*done at //')
case "$BASE" in
    0x*) ;;
    *)   echo "no module base on serial - did the loader run?" >&2; exit 1 ;;
esac

# Where the list head is, link time, from the binary that was deployed.
HEAD_OFFSET=$(x86_64-elf-gdb -q -batch "$ELF" \
    -ex "print/x &'zpp::hypervisor::log_storage::m_lines'" 2>/dev/null \
    | sed -n 's/^\$1 = //p')
case "$HEAD_OFFSET" in
    0x*) ;;
    *)   echo "could not find log_storage::m_lines in $ELF" >&2; exit 1 ;;
esac

echo "module base $BASE, list head offset $HEAD_OFFSET"

# One monitor round trip per read is far too slow for a ring of hundreds
# of lines, so the nodes are not walked over the wire. The list head names
# the first and last node, the allocator hands them out of one arena in
# order, so the whole ring lies inside that span - dumped once, in a few
# large reads, and walked offline where a pointer chase costs nothing.
python3 - "$RIG" "$MONITOR" "$BASE" "$HEAD_OFFSET" "$OUT" <<'PY'
import subprocess, sys, re

rig, monitor, base, head_offset, out = sys.argv[1:6]
base = int(base, 16)
head = base + int(head_offset, 16)

def monitor_read(commands):
    """Run monitor commands and return {address: 8-byte little-endian int}."""
    script = "".join(c + "\n" for c in commands)
    proc = subprocess.run(
        ["ssh", "-o", "StrictHostKeyChecking=no",
         "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=15",
         rig, f"printf %s {monitor!r} >/dev/null; cat | nc -w 25 127.0.0.1 {monitor}"],
        # errors="replace": the monitor is a telnet socket and answers
        # with IAC (0xff) negotiation bytes, which are not valid UTF-8.
        # With the default strict decoding the whole dump dies on
        # "can't decode byte 0xff in position 0" before a single line is
        # read - and the failure looks like the module being gone.
        input=script, capture_output=True, text=True,
        errors="replace")
    words = {}
    for line in proc.stdout.replace("\r", "").split("\n"):
        line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line)
        m = re.match(r"^([0-9a-f]+):((?:\s+0x[0-9a-f]+)+)\s*$", line)
        if not m:
            continue
        at = int(m.group(1), 16)
        for i, value in enumerate(m.group(2).split()):
            words[at + i * 8] = int(value, 16)
    return words

# The head: prev is the last node, next the first, and the third quadword
# is the list's size.
h = monitor_read([f"xp/4gx 0x{head:x}"])
first, last, count = h.get(head + 8), h.get(head), h.get(head + 16)
if not first or not last:
    print("list head unreadable - wrong base, or the module is gone",
          file=sys.stderr)
    sys.exit(1)
print(f"{count} lines, nodes from 0x{min(first,last):x} to 0x{max(first,last):x}")

# The span, padded, in chunks the monitor will answer in one go.
low = (min(first, last) - 0x2000) & ~0xfff
high = (max(first, last) + 0x4000 + 0xfff) & ~0xfff
memory = {}
chunk = 4096 * 8
address = low
commands = []
while address < high:
    commands.append(f"xp/{chunk // 8}gx 0x{address:x}")
    address += chunk
for i in range(0, len(commands), 4):
    memory.update(monitor_read(commands[i:i + 4]))
print(f"read {len(memory) * 8} bytes of physical memory")

def byte(at):
    word = memory.get(at & ~7)
    return None if word is None else (word >> ((at & 7) * 8)) & 0xff

def text_at(node):
    """The std::string sixteen bytes into the node, libc++ layout."""
    control = byte(node + 16)
    if control is None:
        return None
    if control & 1:                       # long: cap, size, data
        size = memory.get(node + 24)
        data = memory.get(node + 32)
        if size is None or data is None:
            return None
        start = data
    else:                                  # short: size in the byte, chars after
        size = control >> 1
        start = node + 17
    chars = []
    for i in range(min(size or 0, 4096)):
        c = byte(start + i)
        if c is None:
            return None
        chars.append(c)
    return bytes(chars).decode("utf-8", "replace")

lines, node, seen = [], first, set()
while node and node != head and node not in seen:
    seen.add(node)
    line = text_at(node)
    lines.append("<unreadable>" if line is None else line)
    node = memory.get(node + 8)

with open(out, "w") as f:
    for i, line in enumerate(lines):
        f.write(f"{i:4d} {line}\n")
print(f"wrote {len(lines)} lines to {out}")
PY
