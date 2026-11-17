#!/usr/bin/env python3
"""Tests for scripts/check-regions.py, on synthetic dumps and the stock image."""
import importlib.util
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("cr", HERE / "check-regions.py")
cr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cr)

STOCK = HERE.parent / "firmware/XDM1041_OS_V2.1.2_20210525.bin"


def dump_with_stock_app():
    d = bytearray(b"\xff" * 0x40000)
    app = STOCK.read_bytes()
    d[0x2800:0x2800 + len(app)] = app
    d[0x3F800:0x3F810] = b"\x01" * 16          # calibration, outside our regions
    return d


@unittest.skipUnless(STOCK.exists(), "stock image not present (firmware/README.md)")
class TestRegions(unittest.TestCase):
    def test_stock_layout_is_free(self):
        lines, ok = cr.report(bytes(dump_with_stock_app()))
        self.assertTrue(ok, lines)
        self.assertTrue(any("safe to set HAL_LOG_ENABLED" in l for l in lines))
        self.assertTrue(any("safe to set HAL_STORE_ENABLED" in l for l in lines))
        end = int(lines[0].split("0x")[1][:8], 16)
        self.assertEqual(0x08002800 + len(STOCK.read_bytes().rstrip(b"\xff")), end)
        self.assertLess(end, 0x08030000)

    def test_used_log_region_is_flagged(self):
        d = dump_with_stock_app()
        d[0x31234] = 0x00
        lines, ok = cr.report(bytes(d))
        self.assertFalse(ok)
        self.assertTrue(any("NOT erased" in l and "0x08031234" in l and "HAL_LOG_ENABLED=0" in l
                            for l in lines))

    def test_used_settings_region_is_flagged(self):
        d = dump_with_stock_app()
        d[0x3C7FF] = 0x12                          # last byte of the settings pages
        lines, ok = cr.report(bytes(d))
        self.assertFalse(ok)
        self.assertTrue(any("HAL_STORE_ENABLED=0" in l for l in lines))

    def test_bootloader_state_page_is_not_ours(self):
        d = dump_with_stock_app()
        d[0x3D000:0x3D004] = b"PPPP"               # OWON's bootloader state (0x0803D000)
        lines, ok = cr.report(bytes(d))
        self.assertTrue(any("safe to set HAL_STORE_ENABLED" in l for l in lines), lines)

    def test_wrong_size(self):
        _, ok = cr.report(b"\xff" * 1000)
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main(verbosity=2)
