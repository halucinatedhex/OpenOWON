#!/usr/bin/env python3
"""
Dump the string pointer tables held in the firmware's .data section.

These are invisible in the raw image because .data is LZ-compressed; run
scripts/unpack-data.py first. Tables are 'end'-terminated groups of pointers
into the flash string region, which is how the SCPI grammar, the range lists
and the menu tree are all encoded.
"""
import argparse, struct, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--data", default="build/data.bin")
    ap.add_argument("--base", default="0x08002800")
    ap.add_argument("--ram",  default="0x20000000")
    ap.add_argument("--min-run", type=int, default=4)
    ap.add_argument("--at", help="dump one table at this RAM address")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    blob = open(a.data, "rb").read()
    base, ram = int(a.base, 16), int(a.ram, 16)

    def s(va, maxn=32):
        o = va - base
        if not (0 <= o < len(img)):
            return None
        e = img.find(b"\x00", o)
        if not (0 < e - o < maxn):
            return None
        try:
            return img[o:e].decode("ascii")
        except UnicodeDecodeError:
            return img[o:e].decode("latin-1")

    def dump(off, limit=None):
        i, grp = 0, 0
        while off + i * 4 + 4 <= len(blob) and (limit is None or i < limit):
            v = struct.unpack_from("<I", blob, off + i * 4)[0]
            t = s(v)
            if t is None:
                break
            if t == "end":
                print(f"       -- group {grp} ends --")
                grp += 1
            else:
                print(f"  [{i:3d}] {t}")
            i += 1
        return i

    if a.at:
        off = int(a.at, 16) - ram
        print(f"=== table at RAM {a.at} (.data +0x{off:04X}) ===")
        dump(off)
        return 0

    # Find every run of >= min-run consecutive pointers that resolve to strings.
    runs, cur = [], []
    for o in range(0, len(blob) - 3, 4):
        v = struct.unpack_from("<I", blob, o)[0]
        if base <= v < base + len(img) and s(v):
            cur.append(o)
        else:
            if len(cur) >= a.min_run:
                runs.append(cur)
            cur = []
    if len(cur) >= a.min_run:
        runs.append(cur)

    print(f"{len(runs)} string-pointer tables in .data\n")
    for r in runs:
        addr = ram + r[0]
        vals = [s(struct.unpack_from("<I", blob, o)[0]) for o in r]
        preview = ", ".join(repr(v) for v in vals[:6])
        print(f"  RAM 0x{addr:08X}  {len(r):4d} entries  {preview}"
              + (" ..." if len(vals) > 6 else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main())
