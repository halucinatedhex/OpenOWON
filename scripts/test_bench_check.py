#!/usr/bin/env python3
"""Tests for scripts/bench-check.py with a fake meter (no serial port)."""
import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("bc", HERE / "bench-check.py")
bc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bc)
bc.READ_GAP = 0


class FakeMeter(bc.Meter):
    """Answers MEAS? from a script; goes through the real allow-list."""
    def __init__(self, values, rng="5 V"):
        self.values, self.rng, self.sent = list(values), rng, []

    def send(self, cmd):
        if not bc.ALLOWED.match(cmd):
            raise ValueError(f"refusing {cmd!r}")
        self.sent.append(cmd)

    def query(self, cmd):
        self.send(cmd)
        if cmd == "MEAS?":
            return self.values.pop(0) if self.values else "9.91E+37"
        if cmd == "RANGE?":
            return self.rng
        if cmd == "FUNC?":
            return '"VOLT"'
        if cmd == "*IDN?":
            return "OWON,XDM1241,SIM0001,OpenOWON-0.1-dev"
        if cmd == "SYST:ERR?":
            return '0,"No error"'
        if cmd == "FUNC?":
            return '"VOLT"'
        return ""


class ParseSI(unittest.TestCase):
    def test_values(self):
        self.assertAlmostEqual(bc.parse_si("1.497k"), 1497.0)
        self.assertAlmostEqual(bc.parse_si("10u"), 1e-5)
        self.assertAlmostEqual(bc.parse_si("4.7 MΩ"), 4.7e6)
        self.assertAlmostEqual(bc.parse_si("-1.5"), -1.5)
        self.assertAlmostEqual(bc.parse_si("101.8nF"), 101.8e-9)
        self.assertIsNone(bc.parse_si("  "))
        with self.assertRaises(ValueError):
            bc.parse_si("abc")


class AllowList(unittest.TestCase):
    def test_refuses_writing_and_service_commands(self):
        for bad in ("DOWNLOAD", "VERSION 1", "WTXT x", "INT:START", "START", "PROduce",
                    "HYREG 1", "KEYRANDOM", "RESTart", "SOFTWARERESET", "INT:TXT", "DIAG:RESET",
                    "MODEL x", "*RST"):
            self.assertFalse(bc.ALLOWED.match(bad), bad)

    def test_allows_what_the_plans_use(self):
        for plan in bc.PLANS.values():
            for _, setup, _, _ in plan:
                for c in setup:
                    self.assertTrue(bc.ALLOWED.match(c), c)
        for c in ("MEAS?", "RANGE?", "*IDN?", "SYST:BEEP:FREQ 3000", "SYST:BEEP:PLAY TEST"):
            self.assertTrue(bc.ALLOWED.match(c), c)


class Verdict(unittest.TestCase):
    def test_pass_check_overload(self):
        self.assertEqual(bc.verdict(1.4970, 1.497, (0.001, 0))[0], "PASS")
        self.assertEqual(bc.verdict(1.52, 1.497, (0.001, 0))[0], "CHECK")
        self.assertEqual(bc.verdict(9.91e37, 1.0, (0.01, 0))[0], "OVERLOAD")
        self.assertEqual(bc.verdict(1.0, None, (0.01, 0))[0], "NOREF")


class Run(unittest.TestCase):
    def test_guided_ohms_run_writes_csv(self):
        answers = iter(["0", "10", "s", "1.497k", "s", "s", "s", "s"])
        meter = FakeMeter(["0.1"] * 4 + ["10.01"] * 4 + ["1495.5"] * 4)
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "r.csv"
            rows = bc.run(meter, "ohm", 4, out, ask=lambda _: next(answers), say=lambda *_: None,
                          settle=0)
            self.assertEqual([r[8] for r in rows], ["PASS", "PASS", "PASS"])
            self.assertEqual(rows[2][9], "REAL")
            with out.open() as f:
                data = list(csv.reader(f))
            self.assertEqual(data[0], bc.HEADER)
            self.assertEqual(len(data), 4)
        self.assertIn("CONF:RES", meter.sent)

    def test_simulated_functions_are_marked(self):
        answers = iter(["0", "s", "s", "s", "s", "s"])
        meter = FakeMeter(["0"] * 4)
        with tempfile.TemporaryDirectory() as d:
            rows = bc.run(meter, "temp", 4, Path(d) / "r.csv", ask=lambda _: next(answers),
                          say=lambda *_: None, settle=0)
        self.assertEqual(rows[0][9], "SIM")

    def test_query_skips_console_lines(self):
        class Port:
            lines = [b"t=8s steps=51 step_max=161ms frames=4 skipped=0\n", b"4.103226E-04\r\n"]
            def write(self, b): pass
            def flush(self): pass
            def readline(self): return self.lines.pop(0)
        m = bc.Meter.__new__(bc.Meter)
        m.s = Port()
        self.assertEqual(m.query("MEAS?"), "4.103226E-04")

    def test_scpi_check(self):
        with tempfile.TemporaryDirectory() as d:
            rows = bc.run(FakeMeter(["1.0E+00"]), "scpi", 1, Path(d) / "r.csv", say=lambda *_: None)
        self.assertEqual([r[8] for r in rows], ["PASS"] * 4)


if __name__ == "__main__":
    unittest.main()
