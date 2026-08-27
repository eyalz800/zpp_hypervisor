#!/usr/bin/env python3
"""Read the guest's screen out of guest physical memory, through the
emulator's monitor.

Why this can work at all.  The rig's display adapter is passed through
with VFIO, so the emulator has no console of its own and answers
`screendump` with "There is no console to take a screendump from".  That
is not the same thing as the pixels being unreachable: the adapter's
framebuffer bar is mapped into the **guest's** physical address space, and
`xp` reads guest physical memory.  The only things missing were the
address and the layout, and those now come across in the launch block -
`zpp_framebuffer_info` in `loader/include/zpp/loader.h`, recorded into the
singleton and printed by `scripts/rig-dump-state.py`.

What this shows, and what it does not.  This is the **firmware's** linear
framebuffer, from EFI_GRAPHICS_OUTPUT_PROTOCOL.  The boot graphics - the
vendor logo, the spinner, and a bugcheck screen raised before the display
driver loads - are drawn into it.  A guest that hangs in
Phase1Initialization never gets past that point, which is precisely the
window this exists for.  Once the operating system's own display driver
takes over it may program the adapter differently and this stops
describing what is on screen.

Two questions it answers:

  (a) is the image **changing** between samples - which is the animation
      test, and the honest way to ask "is the spinner spinning";
  (b) what is on it - as an ASCII preview on stdout, and optionally as a
      PNG or PPM file.

Usage
-----
Take the numbers from the state dump, which prints the exact command:

    scripts/rig-dump-state.py | grep -A6 '^framebuffer'
    scripts/rig-screen.py --base 0x... --width 1920 --height 1080 \
                          --stride 1920 --format 1

Or let it resolve them itself from the running hypervisor, which is the
default when `--base` is omitted - it reads the module base off serial and
the members out of the singleton, exactly as `rig-dump-state.py` does:

    scripts/rig-screen.py --out screen.png

Reading device memory, which is the part that is easy to get wrong
--------------------------------------------------------------------
This tree has a measured rule: `xp` is trustworthy on RAM and **not**
across a passed-through device's bar.  On the rig's NVMe, `xp /16xw` over
the controller registers reported a disabled controller while `xp /2xw` of
the same address, repeated sixty times without one disagreement, reported
it enabled and ready.  See CLAUDE.md, "Read device registers narrow, and
repeat".

A framebuffer bar is prefetchable memory rather than a register file, so
wide reads may well be fine there - but "may well be" is not a
measurement, and reading a whole screen one word at a time is far too slow
to be the default.  So this **measures it, per run, before believing
anything**: at a handful of addresses it reads narrow, wide, narrow, wide
and compares.  Narrow agreeing with itself while wide disagrees is a
broken wide read; all four disagreeing is a screen that is animating under
the probe, which is inconclusive rather than damning and is retried
elsewhere.  Wide reads are used for the image only if that check passes,
and `--narrow` forces the slow path regardless.

The monitor takes **one connection**.  Every read here opens one, uses it,
and closes it; a leaked socket makes every later reader fail with no
diagnosis.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time
import zlib

SSH = ["ssh", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=15"]

# EFI_GRAPHICS_PIXEL_FORMAT.  Format 3 has no linear framebuffer at all -
# the firmware offers only Blt() - so there is nothing here to read.
PIXEL_FORMAT = {0: "RGBX (red first)", 1: "BGRX (blue first)",
                2: "bit mask", 3: "blt only - NO linear framebuffer"}

# Darkest to brightest.  Deliberately short: this is a shape detector, not
# a picture, and a long ramp makes noise look like detail.
RAMP = " .:-=+*#%@"


class Monitor:
    """Batched physical-memory reads over the emulator's monitor.

    Lifted from `rig-dump-state.py` rather than imported, and that is a
    deliberate duplication: this script has to be usable on its own, and
    that file is 3,800 lines whose import would drag in a great deal that
    has nothing to do with pixels.  The batching rule below is the part
    that matters and it is copied verbatim with its reasoning.
    """

    def __init__(self, rig, port):
        self.rig, self.port = rig, port
        self.pending = []
        # Reads that never came back after retries.  Non-empty means some
        # pixel below is a zero that was never read - which on a screen
        # looks exactly like a black pixel, so it is reported.
        self.unanswered = []

    def queue(self, address, words):
        self.pending.append((address, words))

    # Batch size, and it is not a performance knob.
    #
    # The monitor echoes each character of a command back with redraws,
    # and with many commands in flight that echo interleaves with the
    # output *within a line*.  A corrupted line still matches the address
    # pattern, so it parses - into the wrong key.  The reader then returns
    # the right number of words, none of them at an address anyone asked
    # for, and every miss reads back as a plausible zero.
    CHUNK = 6

    def _issue(self, batch):
        script = "".join(f"xp/{n}gx 0x{a:x}\n" for a, n in batch)
        proc = subprocess.run(
            SSH + [self.rig, f"cat | nc -w 30 127.0.0.1 {self.port}"],
            input=script, capture_output=True, text=True, errors="replace")
        words = {}
        for line in proc.stdout.replace("\r", "").split("\n"):
            line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line)
            m = re.match(r"^([0-9a-f]{8,16}):((?:\s+0x[0-9a-f]+)+)\s*$",
                         line)
            if not m:
                continue
            address = int(m.group(1), 16)
            for i, word in enumerate(m.group(2).split()):
                words[address + 8 * i] = int(word, 16)
        return words

    def run(self):
        pending, self.pending = self.pending, []
        words = {}

        # Chunked, and then *checked*: a read whose address did not come
        # back is retried alone rather than left to read as zero.
        for start in range(0, len(pending), self.CHUNK):
            batch = pending[start:start + self.CHUNK]
            words.update(self._issue(batch))

            for address, count in batch:
                wanted = [address + 8 * i for i in range(count)]
                if all(w in words for w in wanted):
                    continue
                for _ in range(2):
                    words.update(self._issue([(address, count)]))
                    if all(w in words for w in wanted):
                        break
                else:
                    self.unanswered.append((address, count))

        return words


def gdb_offsets(elf, members):
    """Ask the ELF where each member lives inside the singleton.

    Never carry a table of offsets.  They move whenever a member is added
    and stale ones do not fail - they return plausible zeroes, which on a
    screen reader means a black screen and a wrong conclusion.
    """
    args = []
    for m in members:
        args += ["-ex",
                 f"print/x (long)&(('zpp::hypervisor::hypervisor' *)0)"
                 f"->{m}"]
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf] + args,
                         capture_output=True, text=True).stdout
    values = re.findall(r"^\$\d+ = (0x[0-9a-f]+)$", out, re.M)
    if len(values) != len(members):
        return {}
    return dict(zip(members, (int(v, 16) for v in values)))


def gdb_symbol(elf, symbol):
    out = subprocess.run(["x86_64-elf-gdb", "-q", "-batch", elf,
                          "-ex", f"print/x &'{symbol}'"],
                         capture_output=True, text=True).stdout
    m = re.search(r"(0x[0-9a-f]+)", out)
    if not m:
        sys.exit(f"could not find {symbol} in {elf}")
    return int(m.group(1), 16)


def module_base(args):
    """The module base, off serial.

    Read per run and never carried between builds: it moves whenever the
    binary's size changes, and a reader pointed at a stale base reports
    every field as a plausible zero.
    """
    out = subprocess.run(
        SSH + [args.rig,
               'grep -ah "allocate_rwx done at" /home/tc/zpp/serial.out '
               '2>/dev/null | tail -1'],
        capture_output=True, text=True).stdout.strip()
    m = re.search(r"done at (0x[0-9a-f]+)", out)
    if not m:
        sys.exit("no module base on serial - did the loader run? "
                 "Pass --base/--width/--height/--stride/--format instead.")
    return int(m.group(1), 16)


def resolve_from_hypervisor(args):
    """Read the framebuffer description out of the running singleton.

    The same members `rig-dump-state.py` prints, read the same way.  Two
    of the six are 32 bits and the monitor reads 8-byte words, so each is
    read at its own **aligned** address and the half picked by
    `offset & 4` - which holds however the compiler packs them, instead of
    assuming a layout.
    """
    members = ["framebuffer_base", "framebuffer_size", "framebuffer_width",
               "framebuffer_height", "framebuffer_stride",
               "framebuffer_format", "framebuffer_red_mask",
               "framebuffer_green_mask", "framebuffer_blue_mask",
               "framebuffer_reserved_mask"]
    off = gdb_offsets(args.elf, members)
    if not off:
        sys.exit(f"{args.elf} has no framebuffer members - it predates "
                 f"them. Point --elf at the deployed binary, or pass "
                 f"--base/--width/--height/--stride/--format.")

    base = module_base(args) if args.module_base is None \
        else int(args.module_base, 16)
    instance = base + gdb_symbol(
        args.elf, "zpp::hypervisor::hypervisor::instance()::instance")
    print(f"module base 0x{base:x}, singleton 0x{instance:x}")

    reader = Monitor(args.rig, args.port)
    for name in members:
        reader.queue(instance + (off[name] & ~7), 1)
    words = reader.run()

    def value(name, bits):
        word = words.get(instance + (off[name] & ~7))
        if word is None:
            sys.exit(f"{name} did not read back - is something else "
                     f"holding the monitor connection?")
        if 64 == bits:
            return word
        return (word >> (32 if (off[name] & 4) else 0)) & 0xffffffff

    args.base = value("framebuffer_base", 64)
    args.width = value("framebuffer_width", 32)
    args.height = value("framebuffer_height", 32)
    args.stride = value("framebuffer_stride", 32)
    args.format = value("framebuffer_format", 32)
    args.masks = (value("framebuffer_red_mask", 32),
                  value("framebuffer_green_mask", 32),
                  value("framebuffer_blue_mask", 32))

    if not args.base:
        sys.exit("the hypervisor recorded no framebuffer - the loader "
                 "found no graphics output protocol on this machine")


def decode(value, fmt, masks):
    """One 32-bit pixel to (r, g, b).

    The byte order is the format's, not a guess: UEFI 2.10 defines
    PixelRedGreenBlueReserved8BitPerColor as red in the lowest byte and
    PixelBlueGreenRedReserved8BitPerColor as blue in the lowest, both
    little endian, both four bytes per pixel.
    """
    if 1 == fmt:
        return (value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff
    if 0 == fmt:
        return value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff

    # Bit mask.  Scaled to eight bits by the mask's own width rather than
    # assumed to be eight - the specification allows any contiguous field.
    out = []
    for mask in masks:
        if not mask:
            out.append(0)
            continue
        shift = (mask & -mask).bit_length() - 1
        field = (value & mask) >> shift
        wide = mask >> shift
        out.append(field * 255 // wide if wide else 0)
    return tuple(out)


def pixel_address(args, x, y):
    """Where pixel (x, y) is, in guest physical memory.

    Rows are walked at the **stride**, not at the width.  Firmware
    routinely pads a scan line out to a convenient alignment, and an image
    walked at the visible width shears diagonally - which reads as a
    corrupt framebuffer rather than as a reader bug.
    """
    return args.base + (y * args.stride + x) * 4


def read_narrow(args, addresses):
    """One or two words at a time, which is the trustworthy shape.

    `xp /2gx` rather than `/1gx`: the measured NVMe comparison used two
    words, and one command per point already dominates the cost.
    """
    reader = Monitor(args.rig, args.port)
    for address in addresses:
        reader.queue(address & ~7, 2)
    return reader.run(), reader.unanswered


def read_wide(args, runs):
    """Whole runs in one command each, for the image once wide is proven.
    """
    reader = Monitor(args.rig, args.port)
    for address, words in runs:
        reader.queue(address, words)
    return reader.run(), reader.unanswered


def read_control(args):
    """Prove the reader before believing an all-one-colour screen.

    Why this exists, and it is the whole point of the function.  The
    calibration below compares reads of the **same** address, so if the
    bar answers zero to every one of them they agree perfectly and the
    screen is reported STATIC.  An all-black frame then has three
    causes and the script could distinguish only two of them:

      - the screen really is black;
      - the base is wrong (named in the note at the end);
      - **the bar is not answering at all**, which was not named.

    The third is not hypothetical here.  This tree measured it on the
    rig's NVMe: `xp` over a passed-through device's bar returned values
    that were confidently wrong, and CLAUDE.md's rule from it is to read
    a *control* in the same batch - two neighbouring bars answered when
    the device under test did not, and that is what proved the reader
    rather than the device.

    So: read something whose value is **known in advance**, in the same
    way, at the same time.  The hypervisor module's first bytes are an
    ELF header, so `\x7fELF` is the expected answer and anything else is
    a reader that cannot be trusted about pixels either.  This is the
    same discipline as `rig-dump-state.py`'s `reader proven:` line.

    Returns (address, value, ok) or None when no control is available.
    """
    address = int(args.control, 16) if args.control else None
    if address is None:
        try:
            address = module_base(args)
        except Exception:
            return None
    if not address:
        return None

    reader = Monitor(args.rig, args.port)
    reader.queue(address, 2)
    words = reader.run()
    value = words.get(address)
    if value is None:
        return (address, None, False)
    # ELF magic is the low four bytes of the first quadword: 7f 45 4c 46
    return (address, value, (value & 0xffffffff) == 0x464c457f)


def calibrate(args):
    """Decide whether wide reads may be believed on this bar.

    narrow, wide, narrow, wide at the same addresses, as four separate
    passes.  Three outcomes per point:

      - all four agree: the point is static and wide is fine there;
      - the two narrow agree and wide does not: **wide is broken**, which
        is the NVMe failure exactly;
      - the two narrow disagree: the screen is changing under the probe,
        so this point says nothing either way.

    The third case is why the check is not simply "wide == narrow": on an
    animating screen that comparison fails for an innocent reason and
    would send a reader down the slow path for ever.
    """
    points = []
    for i in range(args.calibrate):
        # Spread down the screen rather than clustered, so a point is not
        # picked entirely inside whatever happens to be animating.
        y = (i * args.height) // max(args.calibrate, 1)
        points.append(pixel_address(args, args.width // 3, y) & ~7)

    passes = []
    for i in range(2):
        narrow, _ = read_narrow(args, points)
        passes.append(("narrow", narrow))
        # A window around the point, wide enough to be a genuinely wide
        # read - 64 words is 512 bytes, four times what the NVMe check
        # used and comfortably past whatever granularity a bar decodes at.
        wide, _ = read_wide(args, [(p & ~511, 64) for p in points])
        passes.append(("wide", wide))

    agreed = broken = moving = 0
    for point in points:
        n1 = passes[0][1].get(point)
        w1 = passes[1][1].get(point)
        n2 = passes[2][1].get(point)
        w2 = passes[3][1].get(point)
        if None in (n1, w1, n2, w2):
            continue
        if n1 != n2:
            moving += 1
        elif w1 == w2 == n1:
            agreed += 1
        else:
            broken += 1

    print(f"wide-read check: {agreed} agreed, {broken} disagreed, "
          f"{moving} moving (of {len(points)} points)")
    if broken and not agreed:
        print("  WIDE READS ARE WRONG on this bar - narrow only. This is "
              "the NVMe failure again; see CLAUDE.md, 'Read device "
              "registers narrow, and repeat'.")
        return False
    if broken:
        print(f"  {broken} point(s) disagreed while {agreed} agreed - "
              f"treating wide as usable, but a changing screen and a bad "
              f"read look alike here. Re-run with --narrow to settle it.")
    if not agreed and not broken:
        print("  inconclusive: every point was moving, or nothing read "
              "back. Falling back to narrow reads.")
        return False
    return True


def sample(args, wide, cols, rows):
    """One frame, as a `rows` x `cols` grid of (r, g, b).

    Downsampled by picking one pixel per cell rather than averaging: an
    average needs every pixel, which is the whole screen, and the question
    here is "did it change", which a point sample answers.
    """
    xs = [(c * args.width) // cols for c in range(cols)]
    ys = [(r * args.height) // rows for r in range(rows)]

    if wide:
        # One command per sampled row, covering the visible part of the
        # scan line.  A whole row in one read is what makes this fast
        # enough to run twice.
        #
        # Aligned down to eight and extended to match, because the lookup
        # below is by `address & ~7` and an odd stride puts alternate rows
        # on a four-byte boundary - at which point every key would miss
        # and every cell would read back as a hole.  A framebuffer with an
        # odd stride is unusual and not impossible, and the failure would
        # look like a screen nobody could read rather than like a bug.
        runs = []
        for y in ys:
            start = pixel_address(args, 0, y)
            aligned = start & ~7
            runs.append((aligned,
                         ((start - aligned) + args.width * 4 + 7) // 8))
        words, unanswered = read_wide(args, runs)
    else:
        points = [pixel_address(args, x, y) for y in ys for x in xs]
        words, unanswered = read_narrow(args, points)

    grid = []
    for y in ys:
        row = []
        for x in xs:
            address = pixel_address(args, x, y)
            word = words.get(address & ~7)
            if word is None:
                # Distinguished from black, because a failed read looks
                # exactly like a black pixel and this file's whole point
                # is not to confuse the two.
                row.append(None)
                continue
            value = (word >> (32 if (address & 4) else 0)) & 0xffffffff
            row.append(decode(value, args.format, args.masks))
        grid.append(row)
    return grid, unanswered


def preview(grid):
    """The frame as text, so a screen can be looked at with no file."""
    for row in grid:
        line = []
        for cell in row:
            if cell is None:
                line.append("?")
                continue
            r, g, b = cell
            # Rec. 601 luma, integer.  Any monotone weighting would do;
            # this one is the conventional choice and needs no defending.
            luma = (299 * r + 587 * g + 114 * b) // 1000
            line.append(RAMP[min(luma * len(RAMP) // 256, len(RAMP) - 1)])
        print("  " + "".join(line))


def write_image(path, grid):
    """PNG if the name ends in .png, PPM otherwise.

    Both are written by hand.  PPM needs nothing, and a PNG is a zlib
    stream plus four chunks - which is less code than depending on an
    imaging library the rig's operator may not have.
    """
    height = len(grid)
    width = len(grid[0]) if height else 0
    rows = []
    for row in grid:
        out = bytearray()
        for cell in row:
            # A cell that never read back is magenta rather than black, so
            # a hole in the reading is visible as a hole.
            r, g, b = (255, 0, 255) if cell is None else cell
            out += bytes((r, g, b))
        rows.append(bytes(out))

    if path.lower().endswith(".png"):
        raw = b"".join(b"\x00" + row for row in rows)

        def chunk(kind, payload):
            body = kind + payload
            return (struct.pack(">I", len(payload)) + body
                    + struct.pack(">I", zlib.crc32(body) & 0xffffffff))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR",
                       struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0,
                                   0))
               + chunk(b"IDAT", zlib.compress(raw, 9))
               + chunk(b"IEND", b""))
        with open(path, "wb") as handle:
            handle.write(png)
    else:
        with open(path, "wb") as handle:
            handle.write(f"P6\n{width} {height}\n255\n".encode())
            for row in rows:
                handle.write(row)
    print(f"wrote {path} ({width}x{height})")


def compare(first, second):
    """How much of the grid moved between two frames."""
    changed = compared = 0
    for a_row, b_row in zip(first, second):
        for a, b in zip(a_row, b_row):
            if a is None or b is None:
                continue
            compared += 1
            if a != b:
                changed += 1
    return changed, compared


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    ap.add_argument("--rig", default="tc@192.168.1.199")
    ap.add_argument("--port", default="4446")
    # The archived copy first, because it is the binary the guest is
    # running; out/ is whatever was built most recently. See
    # deploy-to-rig.sh.
    ap.add_argument("--elf",
                    default=(".rig-deployed-hypervisor.elf"
                             if os.path.exists(
                                 ".rig-deployed-hypervisor.elf")
                             else "out/debug/x86_64/zpp_hypervisor"))
    ap.add_argument("--module-base", default=None,
                    help="module base in hex; read from serial when "
                         "omitted")
    ap.add_argument("--base", default=None,
                    help="framebuffer physical address in hex; read from "
                         "the running hypervisor when omitted")
    ap.add_argument("--width", type=int, default=0)
    ap.add_argument("--height", type=int, default=0)
    ap.add_argument("--stride", type=int, default=0,
                    help="pixels per scan line; defaults to --width, "
                         "which is not always right")
    ap.add_argument("--format", type=int, default=1,
                    help="EFI_GRAPHICS_PIXEL_FORMAT: 0 RGBX, 1 BGRX, "
                         "2 bit mask")
    ap.add_argument("--cols", type=int, default=0,
                    help="sample columns; defaults by read width")
    ap.add_argument("--rows", type=int, default=0,
                    help="sample rows; defaults by read width")
    ap.add_argument("--passes", type=int, default=2,
                    help="how many frames to sample (>=2 to detect "
                         "change)")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="seconds between frames")
    ap.add_argument("--calibrate", type=int, default=6,
                    help="how many addresses the wide-read check uses; "
                         "0 skips it and forces narrow")
    ap.add_argument("--narrow", action="store_true",
                    help="never use wide reads, whatever the check says")
    ap.add_argument("--out", default=None,
                    help="write the last frame to this .png or .ppm")
    ap.add_argument("--control", default=None,
                    help="hex guest-physical address whose content is\nknown, read in the same way to prove the reader. Defaults to the\nhypervisor module base, whose first bytes are an ELF header.")
    ap.add_argument("--no-preview", action="store_true",
                    help="skip the ASCII preview")
    args = ap.parse_args()

    args.masks = (0x00ff0000, 0x0000ff00, 0x000000ff)
    if args.base is None:
        resolve_from_hypervisor(args)
    else:
        args.base = int(args.base, 16)
        if not args.width or not args.height:
            sys.exit("--width and --height are required with --base")
        if not args.stride:
            args.stride = args.width

    print(f"framebuffer 0x{args.base:x}, {args.width}x{args.height}, "
          f"stride {args.stride}, format {args.format} "
          f"({PIXEL_FORMAT.get(args.format, '?')})")

    if 3 == args.format:
        sys.exit("format 3 is PixelBltOnly: the firmware exposes no "
                 "linear framebuffer, so there is nothing to read. The "
                 "base is not an address.")
    if not args.width or not args.height or not args.stride:
        sys.exit("a zero width, height or stride - nothing to sample")

    wide = False
    if not args.narrow and args.calibrate:
        wide = calibrate(args)
    elif args.narrow:
        print("wide-read check skipped: --narrow")
    else:
        print("wide-read check skipped: --calibrate 0")

    # Narrow reads cost one monitor command per sampled pixel, so the grid
    # has to be far smaller or a frame takes minutes.  Stated rather than
    # silently applied, because a coarser grid is a real loss of detail
    # and the operator should know which one they got.
    cols = args.cols or (80 if wide else 24)
    rows = args.rows or (30 if wide else 14)
    print(f"sampling {cols}x{rows} with "
          f"{'wide row' if wide else 'narrow point'} reads")

    frames = []
    for i in range(max(args.passes, 1)):
        if i:
            time.sleep(args.interval)
        started = time.time()
        grid, unanswered = sample(args, wide, cols, rows)
        frames.append(grid)
        note = ""
        if unanswered:
            note = (f"  <- {len(unanswered)} read(s) never came back; "
                    f"those cells are holes, not black")
        print(f"frame {i} in {time.time() - started:.1f}s{note}")

    if not args.no_preview:
        print()
        preview(frames[-1])

    # (a) is it changing.  Consecutive pairs rather than first-against-
    # last: a spinner that returns to its starting position would show as
    # unchanged over the whole span and as changing between the steps.
    print()
    if len(frames) < 2:
        print("only one frame - nothing to compare, pass --passes 2")
    else:
        moved = False
        for i in range(1, len(frames)):
            changed, compared = compare(frames[i - 1], frames[i])
            if not compared:
                print(f"  frames {i - 1}->{i}: nothing compared, every "
                      f"cell was a hole")
                continue
            share = 100.0 * changed / compared
            print(f"  frames {i - 1}->{i}: {changed} of {compared} "
                  f"cells changed ({share:.1f}%)")
            moved = moved or bool(changed)
        print("  CHANGING - something on the screen is being redrawn"
              if moved else
              "  STATIC - no sampled cell moved. Either the screen is "
              "frozen, or whatever is moving is smaller than the sample "
              "grid: raise --cols/--rows or --interval before concluding.")

    # A screen that is entirely one colour is worth naming, because it is
    # what a reader pointed at the wrong address produces and it is
    # indistinguishable from a blanked display.
    colours = {cell for row in frames[-1] for cell in row
               if cell is not None}
    if len(colours) <= 1:
        print(f"  NOTE the whole frame is one value {colours}.")
        # Three things produce this and they want opposite work, so the
        # reader is proven before any of them is reported. See
        # read_control for why the calibration above cannot do it.
        control = read_control(args)
        if control is None:
            print("     no control available - pass --control ADDR, or "
                  "let the module base resolve, before believing this")
        else:
            address, value, ok = control
            if ok:
                print(f"     reader proven: 0x{address:x} = "
                      f"0x{value:016x}, ELF magic as expected.")
                print("     So the reader reaches guest physical memory "
                      "and this frame is what is there: either the "
                      "screen is blank or the base is wrong. Check the "
                      "base against the state dump.")
            elif value is None:
                print(f"     READER BROKEN: the control at 0x{address:x} "
                      f"never came back. This frame is not evidence "
                      f"about the screen at all.")
            else:
                print(f"     READER BROKEN: the control at 0x{address:x} "
                      f"read 0x{value:016x}, not ELF magic. Every pixel "
                      f"above is a value this reader invented; do not "
                      f"record a blank screen from it.")

    if args.out:
        write_image(args.out, frames[-1])


if __name__ == "__main__":
    main()
