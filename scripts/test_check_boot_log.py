#!/usr/bin/env python3
"""Tests for scripts/check-boot-log.py, against logs shaped like src/main.c's output."""
import importlib.util
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("cb", HERE / "check-boot-log.py")
cb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cb)

GOOD = """
XDM1241 firmware dev
clock 72000000 Hz
vector table 0x08002800
calibration: factory record OK, in use
pins: panel off, HY3131 off
pin-free build: heartbeat only
t=1s heartbeat=1
t=2s heartbeat=2
t=3s heartbeat=3
t=4s heartbeat=4
"""


class TestBootLog(unittest.TestCase):
    def test_log_format_matches_main_c(self):
        """The patterns must match what src/main.c actually prints."""
        src = (HERE.parent / "src/main.c").read_text()
        for fmt in ('"\\nXDM1241 firmware %s\\n"', '"clock %lu Hz%s\\n"', '"vector table 0x%08lx\\n"',
                    '"calibration: factory record OK, in use\\n"', '"pins: panel %s, HY3131 %s\\n"',
                    '"t=%lus heartbeat=%lu\\n"'):
            self.assertIn(fmt, src)

    def test_good_boot(self):
        ok, bad, warn = cb.check(GOOD)
        self.assertEqual([], bad)
        self.assertEqual([], warn)
        self.assertEqual(6, len(ok))

    def test_pll_failure_is_a_warning(self):
        ok, bad, warn = cb.check(GOOD.replace("72000000 Hz", "8000000 Hz  (PLL did not lock: running on IRC8M)"))
        self.assertEqual([], bad)
        self.assertTrue(any("8 MHz" in w for w in warn))

    def test_wrong_vtor_fails(self):
        ok, bad, warn = cb.check(GOOD.replace("0x08002800", "0x08000000"))
        self.assertTrue(any("VTOR" in b for b in bad))

    def test_stalled_or_fast_heartbeat_fails(self):
        _, bad, _ = cb.check(re.sub(r"t=[34]s.*\n", "", GOOD))
        self.assertTrue(any("heartbeat" in b for b in bad))
        _, bad, _ = cb.check(GOOD.replace("t=3s", "t=2s"))     # SysTick 2x fast
        self.assertTrue(any("irregular" in b for b in bad))

    def test_implausible_calibration_is_a_warning(self):
        _, bad, warn = cb.check(GOOD.replace("factory record OK, in use",
                                             "35 slots implausible, using unity gain"))
        self.assertEqual([], bad)
        self.assertTrue(any("35" in w for w in warn))

    def test_empty_log_fails_clearly(self):
        _, bad, _ = cb.check("")
        self.assertTrue(any("banner" in b for b in bad))


if __name__ == "__main__":
    unittest.main(verbosity=2)
