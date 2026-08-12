#!/bin/sh
# Fetches the public debug symbols for a Windows binary taken off the
# guest's volume, and prints the URL it used.
#
# Why this is worth having rather than reasoning from an export table:
# `ntoskrnl.exe` exports 3,353 names across a 21 MB image, so the nearest
# export to an address is routinely the wrong function by a mile - the
# stalled instruction in this investigation resolved to
# `MmFreePagesFromMdl+0x10b0` and is actually in `HalpHvTimerArm`. With
# the symbols, `llvm-symbolizer` names it exactly, and `llvm-pdbutil`
# gives the structure offsets that let a probe follow the guest's own
# pointers.
#
# Getting the file off the machine needs the guest stopped, because the
# disk is passed through and the host cannot read it while a guest holds
# it - `ntfscat` returns zero bytes rather than failing:
#
#   ./scripts/rig-kill-qemu.sh
#   ssh $RIG 'sudo ntfscat /dev/nvme0n1p4 /Windows/System32/ntoskrnl.exe' \
#       > ntoskrnl.exe
#
# The volume is also dirty after a guest is killed, so ntfs-3g refuses to
# mount it read-only or otherwise. `ntfscat` reads without mounting.
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <pe-file> [output-directory]" >&2
    exit 2
fi

image=$1
into=${2:-$(dirname "$image")}

# The GUID and age are in the image's own CodeView debug record, which is
# what the symbol server indexes by - so the symbols fetched are for
# exactly this build and cannot be silently the wrong ones.
read -r pdb guid age <<EOF
$(python3 - "$image" <<'PY'
import struct, sys

data = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
optional = pe + 24
count, = struct.unpack_from("<H", data, pe + 6)
optional_size, = struct.unpack_from("<H", data, pe + 20)

sections = []
table = optional + optional_size
for i in range(count):
    entry = table + i * 40
    virtual_size, address, raw_size, raw_offset = struct.unpack_from(
        "<IIII", data, entry + 8)
    sections.append((address, virtual_size, raw_offset, raw_size))

def offset_of(rva):
    for address, virtual_size, raw_offset, raw_size in sections:
        if address <= rva < address + max(virtual_size, raw_size):
            within = rva - address
            if within < raw_size:
                return raw_offset + within
    return None

rva, size = struct.unpack_from("<II", data, optional + 112 + 6 * 8)
base = offset_of(rva)

for i in range(size // 28):
    entry = base + i * 28
    kind, = struct.unpack_from("<I", data, entry + 12)
    length, = struct.unpack_from("<I", data, entry + 16)
    raw, = struct.unpack_from("<I", data, entry + 24)
    if 2 != kind:                       # IMAGE_DEBUG_TYPE_CODEVIEW
        continue
    d1, d2, d3 = struct.unpack_from("<IHH", data, raw + 4)
    tail = data[raw + 12:raw + 20]
    age, = struct.unpack_from("<I", data, raw + 20)
    name = data[raw + 24:raw + length].split(b"\0")[0].decode()
    print(f"{name} {d1:08X}{d2:04X}{d3:04X}{tail.hex().upper()} {age:X}")
    break
PY
)
EOF

if [ -z "${pdb:-}" ]; then
    echo "no CodeView debug record in $image" >&2
    exit 1
fi

url="https://msdl.microsoft.com/download/symbols/$pdb/$guid$age/$pdb"
echo "$image -> $url" >&2

curl -sSL --fail --max-time 600 -A "Microsoft-Symbol-Server/10.0.0.0" \
    -o "$into/$pdb" "$url" || {
    echo "could not fetch $pdb" >&2
    exit 1
}

echo "$into/$pdb"
