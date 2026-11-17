#!/usr/bin/env python3
"""Tests for scripts/serial-dump.py with a fake stock meter on the wire."""
import importlib.util
import struct
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("sd", HERE / "serial-dump.py")
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)


def record(coef=1000000, cjg=1694):
    rec = bytearray(b"\xff" * sd.RECORD)
    for f in range(11):
        for r in range(10):
            struct.pack_into("<i", rec, sd.COEF + (f * 10 + r) * 4, coef)
            struct.pack_into("<i", rec, sd.ZERO + (f * 10 + r) * 4, 0)
    struct.pack_into("<i", rec, sd.CJG, cjg)
    return bytes(rec)


class FakeMeter:
    """Answers TXT the way stock_dump_settings_uart does; records writes."""
    def __init__(self, rec, noise=b"", answers=b"INT:TXT"):
        self.rec, self.noise, self.sent, self.out = rec, noise, [], b""
        self.answers = answers

    def write(self, b):
        self.sent.append(b)
        if b.strip().upper() == self.answers:
            ident = b"XDM1241 SN0123456789"[:20].ljust(20, b"\0")
            payload = self.rec + b"\xff" * (sd.PAYLOAD - len(self.rec))
            self.out += self.noise + sd.HEADER + ident + payload

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk


class TestSerialDump(unittest.TestCase):
    def test_round_trip_and_valid_layout(self):
        m = FakeMeter(record(), noise=b"junk\r\n")
        cmd, raw = sd.dump(m, sd.SAFE_COMMANDS, timeout=0.05)
        self.assertEqual("INT:TXT", cmd)
        ident, rec = sd.split_stream(raw)
        self.assertEqual(record(), rec)
        text, bad = sd.report(ident, rec)
        self.assertEqual([], bad)
        self.assertIn("1694", text)

    def test_blank_record_fails_every_owned_slot(self):
        bad = sd.validate(sd.parse_record(b"\xff" * sd.RECORD)[0])
        self.assertEqual(35, len(bad))               # same count as meas_cal_validate()

    def test_clamps_match_the_c_implementation(self):
        coef, _, _ = sd.parse_record(record())
        coef[2][0] = 1250000                          # allowed for current
        coef[0][0] = 1250000                          # not for DCV
        coef[8][2] = 1500000                          # cap 0-3: no stock clamp, generic bound
        bad = sd.validate(coef)
        self.assertEqual([(0, 0, 1250000), (8, 2, 1500000)], bad)

    def test_falls_back_to_syst_txt(self):
        m = FakeMeter(record(), answers=b"SYST:TXT")
        cmd, raw = sd.dump(m, sd.SAFE_COMMANDS, timeout=0.05)
        self.assertEqual("SYST:TXT", cmd)
        self.assertEqual([b"INT:TXT\n", b"SYST:TXT\n"], m.sent)

    def test_real_xdm2041_record(self):
        rec = (HERE.parent / "test/data/xdm2041_calibration.bin").read_bytes()
        text, bad = sd.report(rec[:20], rec[:sd.RECORD])
        self.assertEqual([], bad)
        self.assertIn("1701", text)

    def test_dangerous_commands_are_refused(self):
        for c in ("SYST:DOWNLOAD", "DOWNLOAD", "syst:version 1.0", "SYST:WTXT",
                  "SYST:PROD", "SYST:HYREG 1,2", "SYST:KEYRANDOM", "SYST:RESTART",
                  "SYST:SOFTWARERESET", "SYST:TXT?", "int:start", "INT:START", "SYST:START",
                  "int:record", "INT:DOWNLOAD", "INT:TXT 1", "INT:WTXT", "INT:CHECKSUM"):
            with self.assertRaises(SystemExit, msg=c):
                sd.check_command(c)
        for c in ("INT:TXT", ":INTernal:TXT", "SYST:TXT", "CONF:VOLT:DC 5", "RATE S", "CONF:TEMP KITS90"):
            sd.check_command(c)                       # the safe ones still pass
        m = FakeMeter(record())
        with self.assertRaises(SystemExit):
            sd.dump(m, ["SYST:DOWNLOAD"], timeout=0.01)
        self.assertEqual([], m.sent)                  # nothing reached the wire

    def test_short_or_missing_frame(self):
        with self.assertRaises(ValueError):
            sd.split_stream(b"no header here")
        with self.assertRaises(ValueError):
            sd.split_stream(sd.HEADER + b"\x00" * 100)


if __name__ == "__main__":
    unittest.main(verbosity=2)
