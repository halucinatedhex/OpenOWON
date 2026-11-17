#!/usr/bin/env python3
"""Tests for scripts/capture-afe.py: GDB script, parsing, attribution, safety."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ca", HERE / "capture-afe.py")
ca = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ca)

V2 = HERE.parent / "firmware/XDM1041_OS_V2.1.2_20210525.bin"
SYM = HERE.parent / "symbols/xdm1041_v2.1.2.sym"


def hyw(idx, data):
    pad = data + [0xEE] * (ca.NBYTES - len(data))
    return f"HYW {idx} {len(data)} " + " ".join(f"{b:02x}" for b in pad) + "\n"


class FakeLines:
    """Stands in for GDB: emits writes when the fake meter receives a command."""
    def __init__(self, responses):
        self.pending, self.responses = ["noise before\n"], responses

    def on_send(self, cmd):
        self.pending += self.responses.get(cmd, [])

    def drain(self):
        out, self.pending = self.pending, []
        return out


NEED_IMAGE = unittest.skipUnless(V2.exists(), "stock image not present (firmware/README.md)")


class TestCaptureAfe(unittest.TestCase):
    @NEED_IMAGE
    def test_index_table_from_image_matches_docs(self):
        t = ca.index_table(V2, SYM, base=0x08002800)
        self.assertEqual([0x00, 0x03, 0x06, 0x09, 0x0E, 0x11, 0x14, 0x15, 0x18, 0x1B, 0x1E, 0x1F, 0x20], t[:13])

    def test_gdb_script_breaks_on_write_regs(self):
        s = ca.gdb_script(0x0800784A, 2331)
        self.assertIn("break *0x800784a", s)
        self.assertIn("target extended-remote :2331", s)
        self.assertNotIn("load", s.lower().replace("payload", ""))   # never programs the chip

    @NEED_IMAGE
    def test_parse_translates_index_and_trims_to_n(self):
        table = ca.index_table(V2, SYM, base=0x08002800)
        w = ca.parse_line(hyw(12 + 0x0F, [0x2A, 0x00, 0x11]), table)   # idx 27 -> 0x2F
        self.assertEqual(0x2F, w["reg"])
        self.assertEqual([0x2A, 0x00, 0x11], w["bytes"])
        self.assertIsNone(ca.parse_line("Breakpoint 1 at 0x800784a", table))

    @NEED_IMAGE
    def test_attribution_and_summary(self):
        table = ca.index_table(V2, SYM, base=0x08002800)
        fake = FakeLines({
            "CONF:VOLT:DC 5E-2": [hyw(27, [0x31]), hyw(28, [0x05, 0x06])],   # SMODE, R30, R31
            "CONF:VOLT:DC 5E-1": [hyw(27, [0x31]), hyw(28, [0x07, 0x06])],
        })
        plan = [("DCV 50mV", ["CONF:VOLT:DC 5E-2"]), ("DCV 500mV", ["CONF:VOLT:DC 5E-1"])]
        steps = ca.run(plan, fake, table, fake.on_send, settle=0)
        self.assertEqual(2, len(steps[0]["writes"]))
        self.assertEqual({0x2F: 0x31, 0x30: 0x05, 0x31: 0x06}, ca.registers_after(steps[0]["writes"]))
        summary = ca.summarise(steps)
        self.assertIn("differ between steps: 0x30", summary)        # only R30 changes

    def test_writing_commands_never_sent(self):
        fake = FakeLines({})
        sent = []
        with self.assertRaises(SystemExit):
            ca.run([("bad", ["SYST:DOWNLOAD"])], fake, [0] * 40, sent.append, settle=0)
        self.assertEqual([], sent)

    def test_plan_file_and_default_plan(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("# comment\nDCV 5V | CONF:VOLT:DC ; RANGE 5\n")
        self.assertEqual([("DCV 5V", ["CONF:VOLT:DC", "RANGE 5"])], ca.load_plan(f.name))
        plan = ca.default_plan()
        self.assertEqual(6 + 5 + 6 + 6 + 6 + 6 + 7 + 6 + 6, len(plan))   # ranges + rates + singles
        self.assertIn(("DCV 5V rate M", ["CONF:VOLT:DC 5", "RATE M"]), plan)


if __name__ == "__main__":
    unittest.main(verbosity=2)
