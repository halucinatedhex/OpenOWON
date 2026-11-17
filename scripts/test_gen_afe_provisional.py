#!/usr/bin/env python3
"""src/afe_provisional.h is current, and agrees with every confirmed row."""
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location("gp", HERE / "gen-afe-provisional.py")
gp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gp)


def rows(text):
    """{(fn, range): (bytes...)} from a C table of { fn, r, { 24 bytes }, ... }."""
    out = {}
    for m in re.finditer(r"\{ (\d+), (\d+), \{ ([0-9a-fx, ]+) \}", text):
        out[(int(m.group(1)), int(m.group(2)))] = tuple(int(b, 16) for b in m.group(3).split(", "))
    return out


class Provisional(unittest.TestCase):
    def test_header_is_current(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "p.h"
            gp.main(["-o", str(out)])
            self.assertEqual(out.read_text(), (ROOT / "src/afe_provisional.h").read_text(),
                             "run scripts/gen-afe-provisional.py")

    def test_confirmed_rows_agree(self):
        prov = rows((ROOT / "src/afe_provisional.h").read_text())
        table = (ROOT / "lib/hal/afe_table.h").read_text()
        blocks = {"AFE_DCV": 0, "AFE_OHM2": 4}
        checked = 0
        for name, fn in blocks.items():
            body = table[table.index(name + "["):]
            body = body[:body.index("};")]
            for r, m in enumerate(re.finditer(r"\{ \{ ([0-9a-fx,\s]+)\}", body)):
                b = tuple(int(x, 16) for x in m.group(1).replace("\n", " ").split(",") if x.strip())
                self.assertEqual(prov[(fn, r)], b, f"{name}[{r}]")
                checked += 1
        self.assertEqual(checked, 12)


if __name__ == "__main__":
    unittest.main()
