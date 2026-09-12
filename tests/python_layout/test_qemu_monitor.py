"""A fragmented monitor stream must never become a successful partial read."""
import importlib.util
from pathlib import Path
import socket
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "qemu_monitor", Path(__file__).resolve().parents[2] / "scripts/qemu_monitor.py")
monitor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(monitor)


class Stream:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.sent = []
        self.closed = False

    def settimeout(self, timeout):
        pass

    def recv(self, size):
        value = next(self.chunks, b"")
        if isinstance(value, Exception):
            raise value
        return value

    def sendall(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True


class FramedMonitor(unittest.TestCase):
    def open(self, chunks):
        stream = Stream(chunks)
        with patch.object(monitor.socket, "create_connection", return_value=stream):
            client = monitor.QemuMonitor()
        self.addCleanup(client.close)
        return client, stream

    def test_fragmented_greeting_prompt_and_memory_rows(self):
        client, stream = self.open([
            b"\xff\xfb", b"\x01QEMU monitor\r\n(qe", b"mu) ",
            b"xp /2xg 0x123400\x1b[K\r\n123400: 0x0000000000",
            b"000001 0x0000000000000002\r\n(q", b"emu) ",
            b"xp /1xb 0x123410\r\n123410: 0xab\r\n(qemu) "])
        self.assertEqual(client.read_physical(0x123400, 2), [1, 2])
        self.assertEqual(client.read_physical(0x123410, 1, 1), [0xab])
        self.assertEqual(stream.sent, [b"xp /2xg 0x123400\n", b"xp /1xb 0x123410\n"])
        self.assertFalse(stream.closed)

    def test_eof_and_timeout_discard_partial_data_and_close(self):
        for ending in (b"", socket.timeout("late")):
            with self.subTest(ending=ending):
                client, stream = self.open([
                    b"(qemu) ", b"123400: 0x0000000000000001\r\n", ending])
                with self.assertRaises(monitor.MonitorError):
                    client.read_physical(0x123400)
                self.assertTrue(stream.closed)

    def test_completed_but_missing_wrong_or_duplicate_rows_are_errors(self):
        for data in (
            b"Cannot access memory\r\n",
            b"123400: 0x01\r\n",  # one of two bytes missing
            b"123401: 0x01 0x02\r\n",  # right count, wrong address
            b"123400: 0x01\r\n123400: 0x02\r\n",
        ):
            with self.subTest(data=data):
                client, _ = self.open([
                    b"(qemu) ", b"xp /2xb 0x123400\r\n" + data + b"(qemu) "])
                with self.assertRaises(monitor.MonitorError):
                    client.read_physical(0x123400, 2, 1)

    def test_incomplete_greeting_closes_connection(self):
        stream = Stream([b"QEMU monitor\r\n", b""])
        with patch.object(monitor.socket, "create_connection", return_value=stream):
            with self.assertRaises(monitor.MonitorError):
                monitor.QemuMonitor()
        self.assertTrue(stream.closed)


if __name__ == "__main__":
    unittest.main()
