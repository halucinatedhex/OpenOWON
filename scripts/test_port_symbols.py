#!/usr/bin/env python3
"""
Tests for scripts/port-symbols.py against a synthetic "v3" whose correct
answer is known exactly:

  - a full 256 KB dump: a fake bootloader, the app at 0x08002800, blank above
  - 0x40 bytes inserted at a function boundary mid-code, with every BL/B.W,
    code/flash pointer in literal pools, and vector across it relocated
  - every RAM address in a literal pool moved up by 0x100 (and the SP)

So every code symbol after the insertion must move by 0x40, every RAM symbol
by 0x100, and nothing may map anywhere else. Run: python3 scripts/test_port_symbols.py
"""
import importlib.util
import random
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("port", HERE / "port-symbols.py")
port = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port)

SHIFT, RAM_SHIFT = 0x40, 0x100


def enc_bl(src, dst, hw2_kind):
    """Re-encode a BL/BLX/B.W at src to reach dst, keeping its kind bits."""
    off = dst - (src + 4)
    if hw2_kind == "blx":
        off = (dst & ~3) - ((src + 4) & ~3)
    s = 1 if off < 0 else 0
    imm = off & ((1 << 25) - 1)
    i1, i2 = (imm >> 23) & 1, (imm >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    hw1 = 0xF000 | (s << 10) | ((imm >> 12) & 0x3FF)
    base = {"bl": 0xD000, "blx": 0xC000, "bw": 0x9000}[hw2_kind]
    hw2 = base | (j1 << 13) | (j2 << 11) | ((imm >> 1) & 0x7FF)
    if hw2_kind == "blx":
        hw2 &= ~1
    return hw1, hw2


class Synthetic:
    def __init__(self):
        old = port.Image(port.V2_IMAGE.read_bytes(), port.V2_BASE)
        sizes = port.load_sizes(port.V2_INDEX)
        starts = sorted(a for a in sizes if port.CODE_LO <= a < port.CODE_HI)
        ins = min(starts, key=lambda a: abs(a - 0x0800A000))      # a function boundary
        self.ins = ins
        o = ins - old.base

        def moved(va):
            return va + SHIFT if va >= ins else va

        app = bytearray(old.d[:o] + b"\x00\xbf" * (SHIFT // 2) + old.d[o:])
        new = port.Image(bytes(app), old.base)

        def put16(va_new, v):
            struct.pack_into("<H", app, va_new - old.base, v)

        def put32(va_new, v):
            struct.pack_into("<I", app, va_new - old.base, v)

        lit_done = set()
        for a in starts:
            ins_list, _ = port.analyse(old, a, sizes[a])
            for va, n, kind, x in ins_list:
                if kind == "call":
                    hw2 = old.u16(va + 2)
                    k = "bl" if (hw2 & 0xD000) == 0xD000 else "blx" if (hw2 & 0xD000) == 0xC000 else "bw"
                    if moved(va) - va != moved(x) - x:
                        h1, h2 = enc_bl(moved(va), moved(x), k)
                        put16(moved(va), h1)
                        put16(moved(va) + 2, h2)
                elif kind in ("ldr", "ldrw") and old.ok(x, 4) and x not in lit_done:
                    lit_done.add(x)
                    v = old.u32(x)
                    if 0x20000000 <= v < 0x2000C000:
                        put32(moved(x), v + RAM_SHIFT)
                    elif old.base <= (v & ~1) < old.base + len(old.d):
                        put32(moved(x), moved(v & ~1) | (v & 1))
        # vector table
        put32(old.base, old.u32(old.base) + RAM_SHIFT)
        for slot in range(1, 76):
            v = old.u32(old.base + 4 * slot)
            if v & 1 and port.CODE_LO <= v < port.CODE_HI:
                put32(old.base + 4 * slot, moved(v & ~1) | 1)

        rnd = random.Random(1)
        boot = bytes(rnd.randrange(256) for _ in range(0x2800))
        full = boot + bytes(app)
        full += b"\xff" * (0x40000 - len(full))
        self.image = port.Image(full, 0x08000000)
        self.old, self.sizes, self.moved = old, sizes, moved

    def expected(self, a):
        if 0x20000000 <= a < 0x2000C000:
            return a + RAM_SHIFT
        return self.moved(a)


@unittest.skipUnless(port.V2_IMAGE.exists(), "stock image not present (firmware/README.md)")
class TestPortSymbols(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Synthetic()
        cls.syms = port.load_syms(port.V2_SYMS)
        cls.fmap, cls.dmap, cls.how, cls.notes = port.port(cls.s.old, cls.s.image, cls.syms, cls.s.sizes)

    def test_no_wrong_mappings(self):
        wrong = []
        for a, name in self.syms:
            got = self.fmap.get(a, self.dmap.get(a))
            if got is not None and got != self.s.expected(a):
                wrong.append(f"{name}: {got:#x}, expected {self.s.expected(a):#x} ({self.how[a]})")
        self.assertEqual([], wrong)

    def test_most_symbols_mapped(self):
        mapped = sum(1 for a, _ in self.syms if a in self.fmap or a in self.dmap)
        self.assertGreaterEqual(mapped, 130, f"only {mapped}/{len(self.syms)}")

    def test_moves_are_real(self):
        moved_code = [a for a, _ in self.syms if a in self.fmap and a >= self.s.ins]
        moved_ram = [a for a, _ in self.syms if a in self.dmap and 0x20000000 <= a < 0x2000C000]
        self.assertGreater(len(moved_code), 30)
        self.assertGreater(len(moved_ram), 5)

    def test_identity_never_moves_anything(self):
        fmap, dmap, how, notes = port.port(self.s.old, self.s.old, self.syms, self.s.sizes)
        bad = [hex(a) for a, b in {**fmap, **dmap}.items() if a != b]
        self.assertEqual([], bad)

    def test_every_unmapped_symbol_has_a_reason(self):
        for a, name in self.syms:
            if a not in self.fmap and a not in self.dmap:
                self.assertIn(a, self.notes, name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
