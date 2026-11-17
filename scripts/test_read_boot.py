#!/usr/bin/env python3
"""Tests for scripts/read-boot.py: the parser against the golden text that
test/test_console checks the C emitter against, and a fake meter."""
import importlib.util
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("rb", HERE / "read-boot.py")
rb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rb)

# Byte-for-byte what flashdump_emit() produces for bytes 0..39 at 0x08000000
# (asserted in test/test_console/test_console.c).
GOLDEN = ("08000000 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\r\n"
          "08000020 2021222324252627\r\n"
          "END 08000000 00000028 0da62e3c\r\n")


def emit(data, base=rb.BOOT_BASE):
    """Python mirror of flashdump_emit, for building fake meter replies."""
    out = []
    for off in range(0, len(data), 32):
        out.append("%08x %s\r\n" % (base + off, data[off:off + 32].hex()))
    out.append("END %08x %08x %08x\r\n" % (base, len(data), zlib.crc32(data)))
    return "".join(out)


class Parser(unittest.TestCase):
    def test_golden_text(self):
        self.assertEqual(rb.parse_dump(GOLDEN, length=40), bytes(range(40)))

    def test_mirror_matches_golden(self):
        self.assertEqual(emit(bytes(range(40))), GOLDEN)

    def test_log_lines_around_the_dump_are_ignored(self):
        text = "t=12s heartbeat=12\n" + GOLDEN + "t=13s heartbeat=13\n"
        self.assertEqual(rb.parse_dump(text, length=40), bytes(range(40)))

    def test_cut_short(self):
        with self.assertRaisesRegex(rb.DumpError, "no END"):
            rb.parse_dump(GOLDEN.split("END")[0], length=40)

    def test_corrupted_byte_fails_crc(self):
        bad = GOLDEN.replace("0a0b", "0a0c")
        with self.assertRaisesRegex(rb.DumpError, "CRC"):
            rb.parse_dump(bad, length=40)

    def test_missing_line_fails_sequence(self):
        lines = emit(bytes(96)).splitlines(keepends=True)
        del lines[1]
        with self.assertRaisesRegex(rb.DumpError, "expected 0x08000020"):
            rb.parse_dump("".join(lines), length=96)

    def test_wrong_region_is_refused(self):
        with self.assertRaisesRegex(rb.DumpError, "trailer"):
            rb.parse_dump(emit(bytes(40), base=0x08002800), base=0x08002800, length=0x2800)


class Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeMeter:
    def __init__(self, idn=b"OpenOWON,serial-console,dev\r\n", boot=None, flaky=False):
        self.idn, self.flaky, self.sent, self.out = idn, flaky, [], b""
        self.boot = boot if boot is not None else bytes((i * 13) & 0xFF for i in range(0x2800))
        self.reads = 0
        self.flash = self.boot + bytes(0x40000 - 0x2800)

    def write(self, b):
        self.sent.append(bytes(b))
        cmd = bytes(b).strip()
        if cmd == b"*IDN?":
            self.out += self.idn
        elif cmd in (b"DIAG:BOOT?", b"DIAG:FLASH?"):
            self.reads += 1
            data = self.boot if cmd == b"DIAG:BOOT?" else self.flash
            if self.flaky and self.reads == 2:
                data = data[:100] + b"\x00" + data[101:]
            self.out += b"t=5s heartbeat=5\n" + emit(data).encode()

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk


class Reading(unittest.TestCase):
    def run_on(self, meter):
        c = Clock()
        return rb.read_boot(meter, clock=c.now, sleep=c.sleep)

    def test_reads_twice_and_returns_the_bootloader(self):
        m = FakeMeter()
        data, idn = self.run_on(m)
        self.assertEqual(data, m.boot)
        self.assertEqual(m.reads, 2)
        self.assertIn("OpenOWON", idn)

    def test_refuses_stock_and_sends_it_nothing_else(self):
        m = FakeMeter(idn=b"OWON,XDM1241,12345678,V4.3.0,3\r\n")
        with self.assertRaisesRegex(rb.DumpError, "our firmware"):
            self.run_on(m)
        self.assertEqual(m.sent, [b"*IDN?\r\n"])

    def test_reads_that_differ_are_not_trusted(self):
        # Each read is internally consistent (its CRC matches) but they differ.
        with self.assertRaisesRegex(rb.DumpError, "differ"):
            self.run_on(FakeMeter(flaky=True))


class FullDump(unittest.TestCase):
    def flash(self, stock_pages_erased=False):
        f = bytearray(b"\xff" * 0x40000)
        f[0:8] = (0x20001000).to_bytes(4, "little") + (0x08000101).to_bytes(4, "little")
        f[0x2800:0x2800 + 24] = b"XDM1241-OPEN dumper ver."
        if not stock_pages_erased:
            f[0x5000:0x1D000] = bytes((i * 3) & 0x7F for i in range(0x18000))
        f[0x3F800:0x3F800 + 1392] = bytes((i * 5) & 0xFF for i in range(1392))
        return bytes(f)

    def test_reads_all_256k_twice(self):
        m = FakeMeter()
        m.flash = self.flash()
        c = Clock()
        data, _ = rb.read_boot(m, clock=c.now, sleep=c.sleep, everything=True)
        self.assertEqual(data, m.flash)
        self.assertIn(b"DIAG:FLASH?\r\n", m.sent)

    def test_analysis_reports_survivors_and_calibration(self):
        f = self.flash()
        report = "\n".join(rb.analyse(f, f[0x3F800:0x3F800 + 1392]))
        self.assertIn("48 pages hold data", report)
        self.assertIn("data from 0x08005000 to 0x0801D000", report)
        self.assertIn("IDENTICAL", report)

    def test_analysis_flags_a_full_erase_and_a_calibration_mismatch(self):
        f = self.flash(stock_pages_erased=True)
        report = "\n".join(rb.analyse(f, b"\x00" * 1392))
        self.assertIn("all erased", report)
        self.assertIn("DIFFERENT", report)


if __name__ == "__main__":
    unittest.main(verbosity=1)
