#!/usr/bin/env python3
"""Names the Windows kernel functions behind guest instruction pointers.

The exit rings record where the second-level guest was, and until now that
was as far as it went: `rip=0xfffff806a13a57e7` is not a fact anybody can
act on.  This turns it into a function, a section and the nearest export,
which is the difference between "somewhere in early kernel initialisation"
and "in the INIT section, so Phase 1 has not finished".

Two problems have to be solved and neither needs a debugger or a symbol
server.

**Where the kernel is loaded.**  Address space layout randomisation moves
it every boot, so a recorded instruction pointer means nothing on its own.
It is always 2 MB aligned, though, so the low twenty-one bits of the
address are the low twenty-one bits of the relative address - and the exit
reason says which *instruction* is there.  So: take an instruction pointer
whose exit was a WRMSR, walk the handful of relative addresses that share
its low bits, and keep the one whose bytes are `0f 30`.  In practice
exactly one is, and the base falls out.  `--wrmsr-rip` does that.

**Which function.**  The export table is far too sparse to answer it - the
nearest export to the stall was `MmFreePagesFromMdl+0x10b0`, which names
the wrong function by a mile.  The exception directory is not sparse: every
function with a stack frame has a `RUNTIME_FUNCTION` giving its exact
start and end, and that is what `fn` below reports.  The export is printed
too, as a hint about the neighbourhood, and should be read as nothing more.

    ./scripts/symbolize-guest.py ntoskrnl.exe --wrmsr-rip 0xfffff806a13a57e7 \\
        0xfffff806a1461b51 0xfffff806a155cd54 ...

Get `ntoskrnl.exe` off the machine without mounting anything - the volume
is dirty after a guest is killed and ntfs-3g refuses it, read-only or not:

    sudo ntfscat /dev/nvme0n1p4 /Windows/System32/ntoskrnl.exe > /tmp/nt.exe
"""
import argparse
import bisect
import struct
import sys


class Image:
    """Just enough PE64 to map a relative address to a name."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            sys.exit(f"{path} is not a PE image")

        count, = struct.unpack_from("<H", self.data, pe + 6)
        optional_size, = struct.unpack_from("<H", self.data, pe + 20)
        self.optional = pe + 24

        magic, = struct.unpack_from("<H", self.data, self.optional)
        if 0x20B != magic:
            sys.exit(f"{path} is not 64-bit; this reads PE32+ only")

        self.size_of_image, = struct.unpack_from(
            "<I", self.data, self.optional + 56)

        self.sections = []
        table = self.optional + optional_size
        for i in range(count):
            entry = table + i * 40
            name = self.data[entry:entry + 8].rstrip(b"\0").decode(
                errors="replace")
            virtual_size, address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", self.data, entry + 8)
            self.sections.append(
                (name, address, virtual_size, raw_offset, raw_size))

        # The data directories, by their architectural index. Getting these
        # wrong does not fail - it reads a neighbouring field as a
        # directory and produces a plausible, empty table, which is how
        # every function came back "no entry" the first time.
        directories = self.optional + 112
        self.export_directory = struct.unpack_from(
            "<II", self.data, directories + 0 * 8)
        self.exception_directory = struct.unpack_from(
            "<II", self.data, directories + 3 * 8)

        self._read_exports()
        self._read_functions()

    def offset_of(self, address):
        for _, start, virtual_size, raw_offset, raw_size in self.sections:
            if start <= address < start + max(virtual_size, raw_size):
                within = address - start
                if within < raw_size:
                    return raw_offset + within
        return None

    def section_of(self, address):
        for name, start, virtual_size, _, raw_size in self.sections:
            if start <= address < start + max(virtual_size, raw_size):
                return name
        return "?"

    def _read_exports(self):
        address, _ = self.export_directory
        self.exports = []
        base = self.offset_of(address)
        if base is None:
            return

        _, name_count = struct.unpack_from("<II", self.data, base + 20)
        functions, names, ordinals = struct.unpack_from(
            "<III", self.data, base + 28)
        functions = self.offset_of(functions)
        names = self.offset_of(names)
        ordinals = self.offset_of(ordinals)

        for i in range(name_count):
            name_address, = struct.unpack_from("<I", self.data, names + 4 * i)
            ordinal, = struct.unpack_from("<H", self.data, ordinals + 2 * i)
            target, = struct.unpack_from(
                "<I", self.data, functions + 4 * ordinal)
            start = self.offset_of(name_address)
            end = self.data.index(b"\0", start)
            self.exports.append(
                (target, self.data[start:end].decode(errors="replace")))

        self.exports.sort()
        self.export_starts = [e[0] for e in self.exports]

    def _read_functions(self):
        address, size = self.exception_directory
        self.functions = []
        base = self.offset_of(address)
        if base is None:
            return

        for i in range(size // 12):
            start, end, _ = struct.unpack_from("<III", self.data,
                                               base + 12 * i)
            if 0 == start:
                break
            self.functions.append((start, end))

        self.functions.sort()
        self.function_starts = [f[0] for f in self.functions]

    def describe(self, address):
        index = bisect.bisect_right(self.function_starts, address) - 1
        found = None
        if index >= 0 and self.functions[index][1] > address:
            found = self.functions[index]

        start = found[0] if found else address

        index = bisect.bisect_right(self.export_starts, start) - 1
        export = self.exports[index] if index >= 0 else None

        near = (f"{export[1]}+0x{start - export[0]:x}"
                if export else "?")

        return {
            "section": self.section_of(address),
            "function": start,
            "offset": address - start,
            "bounded": found is not None,
            "near": near,
        }


def base_from_wrmsr(image, rip):
    """The load address, deduced from one instruction pointer.

    A WRMSR exit saves the address *of* the instruction - the guest has not
    retired it - so the two bytes there are `0f 30`. The kernel is 2 MB
    aligned, so only the relative addresses congruent to the pointer modulo
    2 MB are possible, and there are only a handful within the image.
    """
    wrmsr = b"\x0f\x30"
    low = rip & 0x1FFFFF
    matches = []

    for step in range((image.size_of_image >> 21) + 2):
        address = low + step * 0x200000
        offset = image.offset_of(address)
        if offset is None or offset + 2 > len(image.data):
            continue
        if wrmsr == image.data[offset:offset + 2]:
            matches.append(address)

    if 1 != len(matches):
        sys.exit(f"deduced {len(matches)} candidates from 0x{rip:x} "
                 f"({[hex(m) for m in matches]}); pass --base instead")

    return rip - matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="ntoskrnl.exe from the guest's volume")
    parser.add_argument("rips", nargs="*",
                        help="guest instruction pointers, hex")
    parser.add_argument("--base", help="kernel load address, if known")
    parser.add_argument("--wrmsr-rip",
                        help="an instruction pointer whose exit was a "
                             "WRMSR, from which the base is deduced")
    arguments = parser.parse_args()

    image = Image(arguments.image)

    if arguments.base:
        base = int(arguments.base, 0)
    elif arguments.wrmsr_rip:
        base = base_from_wrmsr(image, int(arguments.wrmsr_rip, 0))
    else:
        sys.exit("need --base or --wrmsr-rip")

    print(f"kernel base 0x{base:x}, image size 0x{image.size_of_image:x}, "
          f"{len(image.functions)} functions, {len(image.exports)} exports")

    rips = arguments.rips or [line.strip() for line in sys.stdin
                              if line.strip()]

    for text in rips:
        rip = int(text, 0)
        address = rip - base
        if address < 0 or address >= image.size_of_image:
            print(f"  0x{rip:x} is outside the image")
            continue

        found = image.describe(address)
        note = "" if found["bounded"] else "  (no exception-table entry)"
        print(f"  0x{rip:x}  rva 0x{address:07x}  [{found['section']:8}]  "
              f"fn 0x{found['function']:07x}+0x{found['offset']:<5x}  "
              f"near {found['near']}{note}")


if __name__ == "__main__":
    main()
