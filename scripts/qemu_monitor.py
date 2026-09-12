"""Prompt-framed, read-only QEMU monitor access for live guest readers.

Consume the greeting before sending a command, then wait for its complete
reply. Fixed sleeps followed by a nonblocking recv can return a partial
answer, and reconnecting for every word makes a process walk take minutes.
One reader owns this connection until it exits; do not run readers together.
"""
import atexit
import re
import socket
import time


class MonitorError(RuntimeError):
    pass


def _plain(data):
    # QEMU's telnet negotiation and readline redraw sequences.
    data = re.sub(rb"\xff[\xfb-\xfe].", b"", data)
    data = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", data)
    return data.replace(b"\r", b"").replace(b"\x08", b"")


class QemuMonitor:
    def __init__(self, host="192.168.1.199", port=4446, timeout=12):
        self.timeout = timeout
        self.sock = socket.create_connection((host, port), timeout=timeout)
        try:
            self._reply()  # the greeting's prompt is not a command reply
        except Exception:
            self.close()
            raise

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _reply(self):
        deadline = time.monotonic() + self.timeout
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise MonitorError("monitor reply timed out before its prompt")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout as exc:
                raise MonitorError("monitor reply timed out before its prompt") from exc
            if not chunk:
                raise MonitorError("monitor closed before its reply was complete")
            data.extend(chunk)
            if len(data) > 4 * 1024 * 1024:
                raise MonitorError("monitor reply exceeded 4 MiB without a prompt")
            plain = _plain(bytes(data))
            if plain.rstrip().endswith(b"(qemu)"):
                return plain.decode("utf-8", "replace")

    def command(self, command):
        if "\n" in command or "\r" in command:
            raise ValueError("send one monitor command at a time")
        if self.sock is None:
            raise MonitorError("monitor connection is closed")
        try:
            self.sock.sendall((command + "\n").encode("ascii"))
            return self._reply()
        except Exception:
            self.close()
            raise

    def read_physical(self, address, count=1, width=8):
        if width not in (1, 2, 4, 8) or count < 1:
            raise ValueError("invalid physical read size")
        unit = {1: "b", 2: "h", 4: "w", 8: "g"}[width]
        reply = self.command(f"xp /{count}x{unit} 0x{address:x}")
        values = {}
        # Read only addressed data rows, never the echoed command's address.
        row_pattern = rf"^\s*([0-9a-fA-F]+):\s*((?:0x[0-9a-fA-F]{{{2 * width}}}\s*)+)$"
        for row in reply.splitlines():
            match = re.fullmatch(row_pattern, row)
            if match:
                start = int(match[1], 16)
                for index, value in enumerate(match[2].split()):
                    location = start + index * width
                    if location in values:
                        raise MonitorError(f"duplicate memory row at {location:#x}")
                    values[location] = int(value, 16)
        wanted = [address + index * width for index in range(count)]
        if len(values) != count or any(location not in values for location in wanted):
            raise MonitorError(f"incomplete physical read: {count} x {width} bytes at {address:#x}")
        return [values[location] for location in wanted]


_connection = None


def read_physical(address, count=1, width=8):
    global _connection
    if _connection is None:
        _connection = QemuMonitor()
        atexit.register(_connection.close)
    return _connection.read_physical(address, count, width)
