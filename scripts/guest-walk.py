"""Walks the second-level guest's page tables through the QEMU monitor.

Needed because the guest's kernel image is the only copy of its symbols
reachable on this rig: `ntoskrnl.exe` lives on the passed-through NVMe,
so the host cannot read the file while the guest runs.

Take the root from `l2_exit_cr3` in the singleton, NOT from `guest_cr3` -
that one is this VMM's own, and a walk with it reports the guest kernel
unmapped, which reads exactly like a guest that has not loaded one.

    python3 scripts/guest-walk.py <l2_exit_cr3> <guest virtual address>

Traps, both paid for:
  - `xp` prints its address column with NO `0x` prefix.
  - The monitor takes ONE connection. A leaked socket makes every later
    reader report None for ever after.
"""
import socket, sys, time, re

class Mon:
    def __init__(self, host="192.168.1.199", port=4446):
        self.s = socket.create_connection((host, port), 10)
        self.s.settimeout(8); time.sleep(0.4)
        try: self.s.recv(1 << 20)
        except Exception: pass
    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        buf = b""; t = time.time()
        while time.time() - t < 8:
            try: d = self.s.recv(1 << 20)
            except socket.timeout: break
            if not d: break
            buf += d
            if buf.rstrip().endswith(b"(qemu)"): break
        # strip readline echo garbage
        return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]|[\x08\x0d]", b"", buf).decode(errors="replace")
    def close(self): self.s.close()

def qwords(mon, phys, n):
    """xp /Ngx - the address column has NO 0x prefix (CLAUDE.md)."""
    out = mon.cmd(f"xp /{n}gx 0x{phys:x}")
    vals = []
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]{4,16}):\s+(.*)$", line)
        if not m: continue
        for v in re.findall(r"0x([0-9a-f]{16})", m.group(2)):
            vals.append(int(v, 16))
    return vals

def translate(mon, cr3, va):
    """Four-level walk. Returns physical address or None."""
    table = cr3 & 0x000ffffffffff000
    for shift, level in ((39, 4), (30, 3), (21, 2), (12, 1)):
        idx = (va >> shift) & 0x1ff
        e = qwords(mon, table + idx * 8, 1)
        if not e: return None
        e = e[0]
        if not (e & 1): return None
        if (level in (3, 2)) and (e & 0x80):          # large page
            mask = (1 << shift) - 1
            return (e & 0x000fffffffe00000 & ~mask) | (va & mask) if level == 2 \
                   else (e & 0x000fffffc0000000) | (va & ((1 << 30) - 1))
        table = e & 0x000ffffffffff000
    return table | (va & 0xfff)

if __name__ == "__main__":
    cr3 = int(sys.argv[1], 16); va = int(sys.argv[2], 16)
    m = Mon()
    try:
        p = translate(m, cr3, va)
        print(f"va 0x{va:x} -> phys {hex(p) if p else 'UNMAPPED'}")
        if p:
            w = qwords(m, p, 2)
            print("first qwords:", [hex(x) for x in w])
            if w: print("  as bytes:", w[0].to_bytes(8, 'little'))
    finally:
        m.close()
