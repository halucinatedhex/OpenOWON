#!/usr/bin/env python3
"""
Say which of our flash regions are free on this meter, from a full dump.

    scripts/check-regions.py backups/<stamp>/flash.bin

Our firmware writes two regions that stock v2.1.2 leaves erased:
  data log  0x08030000-0x0803A7FF  (lib/datalog, HAL_LOG_ENABLED)
  settings  0x0803A800-0x0803C7FF  (lib/store + user cal, HAL_STORE_ENABLED)
Each flag in lib/hal/hal_pins.h should only be set once the v3 dump shows its
region erased. Also reports where stock's own image ends, to compare with the
0x08030000 limit our image is linked to. Read-only.
"""
import sys
from pathlib import Path

BASE = 0x08000000
REGIONS = [
    ("data log", 0x08030000, 0x0803A800, "HAL_LOG_ENABLED"),
    ("settings", 0x0803A800, 0x0803C800, "HAL_STORE_ENABLED"),
]
APP = (0x08002800, 0x0803D800)


def last_used(data, lo, hi):
    """Address just past the last non-0xFF byte in [lo, hi), or lo if all erased."""
    chunk = data[lo - BASE:hi - BASE]
    stripped = chunk.rstrip(b"\xff")
    return lo + len(stripped)


def report(data):
    """Returns (lines, ok): ok is False if a region we write is not erased."""
    if len(data) != 0x40000:
        return [f"dump is {len(data)} bytes, expected 262144 (a full 256 KB backup)"], False
    lines, ok = [], True
    end = last_used(data, *APP)
    lines.append(f"stock app data ends at 0x{end:08X}"
                 + ("" if end <= 0x08030000 else "  - reaches into the data-log region"))
    for name, lo, hi, flag in REGIONS:
        chunk = data[lo - BASE:hi - BASE]
        used = len(chunk) - chunk.count(0xFF)
        if used == 0:
            lines.append(f"{name:<9} 0x{lo:08X}-0x{hi - 1:08X} erased: safe to set {flag}=1")
        else:
            ok = False
            first = lo + next(i for i, b in enumerate(chunk) if b != 0xFF)
            lines.append(f"{name:<9} 0x{lo:08X}-0x{hi - 1:08X} NOT erased ({used} bytes, first at "
                         f"0x{first:08X}): leave {flag}=0 and find out what stock keeps there")
    return lines, ok


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lines, ok = report(Path(sys.argv[1]).read_bytes())
    print("\n".join(lines))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
