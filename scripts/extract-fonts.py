#!/usr/bin/env python3
"""
Extract the stock bitmap fonts from an XDM firmware image.

Layout, recovered from the renderer at 0x08006ABE:
  - 5 fonts; metrics tables at 0x0801D760 (columns) and 0x0801D76A (rows)
  - column-major, 1 bpp, MSB of each byte = topmost pixel
  - per glyph: cols * ceil(rows/8) bytes, plus 1 trailing byte
  - 23 glyphs, charset "-.0123456789overladOpn "

Bases are located by shape, not by address: glyph 0 is '-', so its ink must be
a single contiguous band of rows near mid-height, and glyph 22 (space) must be
entirely blank. That is specific enough to be unambiguous.
"""
import argparse, struct, sys

CHARSET = "-.0123456789overladOpn "
NG = len(CHARSET)


def load(path):
    return open(path, "rb").read()


def metrics(d, base):
    A = [struct.unpack_from("<H", d, 0x0801D760 - base + 2 * i)[0] for i in range(5)]
    Bt = [struct.unpack_from("<H", d, 0x0801D76A - base + 2 * i)[0] for i in range(5)]
    return [(A[i], Bt[i]) for i in range(5)]      # (cols, rows)


def px(d, gb, cb, x, y):
    """Column-major, MSB of each byte is the topmost pixel of its 8-row band."""
    return (d[gb + x * cb + (y >> 3)] >> (7 - (y & 7))) & 1


def dash_band(d, gb, cols, rows, cb):
    rs = [y for y in range(rows)
          if any(px(d, gb, cb, x, y) for x in range(cols))]
    return rs


def find_base(d, cols, rows, lo, hi):
    cb = (rows + 7) // 8
    stride = cols * cb + 1
    out = []
    for b in range(lo, hi):
        if b + stride * NG > len(d):
            break
        if any(d[b + 22 * stride + k] for k in range(cols * cb)):
            continue                                   # space must be blank
        rs = dash_band(d, b, cols, rows, cb)
        if not rs or len(rs) > max(4, rows // 4):
            continue
        if rs[-1] - rs[0] + 1 != len(rs):
            continue                                   # contiguous
        if not (rows * 0.3 < rs[0] < rows * 0.75):
            continue                                   # mid-height
        out.append((b, stride, cb, rs[0], rs[-1]))
    return out


def ascii_art(d, b, gi, cols, rows, cb, step=1):
    gb = b + gi * (cols * cb + 1)
    return ["".join("#" if px(d, gb, cb, x, y) else "." for x in range(cols))
            for y in range(0, rows, step)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--base", default="0x08002800")
    ap.add_argument("--search-lo", default="0x14000")
    ap.add_argument("--search-hi", default="0x1D000")
    ap.add_argument("--emit", help="write a C header here")
    ap.add_argument("--show", type=int, default=-1, help="ascii-art one font id")
    a = ap.parse_args()

    d = load(a.image)
    base = int(a.base, 16)
    lo, hi = int(a.search_lo, 16), int(a.search_hi, 16)
    mets = metrics(d, base)

    # Bases confirmed by rendering. Font 4 is corroborated independently:
    # 0x0801769B + 673*23 == 0x0801B312, the per-glyph advance table the
    # renderer loads at 0x08006E0A.
    VERIFIED = {4: 0x0801769B}

    found = {}
    for fid, (cols, rows) in enumerate(mets):
        if fid in VERIFIED:
            b = VERIFIED[fid] - base
            cb = (rows + 7) // 8
            print(f"font {fid}: {cols}x{rows} cb={cb} "
                  f"stride={cols*cb+1}  -> VERIFIED 0x{VERIFIED[fid]:08X}")
            found[fid] = (b, cols, rows, cb)
            continue
        hits = find_base(d, cols, rows, lo, hi)
        print(f"font {fid}: {cols}x{rows} cb={(rows+7)//8} "
              f"stride={cols*((rows+7)//8)+1}  -> {len(hits)} candidate(s)")
        for b, stride, cb, r0, r1 in hits[:3]:
            print(f"    VA 0x{base+b:08X}  dash rows {r0}-{r1}")
        # Shape search alone is NOT sufficient for the small fonts: glyphs
        # that size do not constrain it enough, and rendering the top hits
        # produced noise. Report candidates, but never emit unverified data.
        if hits:
            print("      (candidates only - not emitted until rendered "
                  "and confirmed; add to VERIFIED once checked)")

    if a.show >= 0 and a.show in found:
        b, cols, rows, cb = found[a.show]
        for gi in range(NG):
            print(f"\n--- font {a.show} glyph {gi} {CHARSET[gi]!r} ---")
            step = max(1, rows // 32)
            for line in ascii_art(d, b, gi, cols, rows, cb, step):
                print("  " + line)

    if a.emit:
        with open(a.emit, "w") as f:
            f.write("/* Extracted from the stock firmware by "
                    "scripts/extract-fonts.py. Do not edit. */\n")
            f.write("#ifndef STOCK_FONTS_H\n#define STOCK_FONTS_H\n")
            f.write("#include <stdint.h>\n\n")
            f.write(f'#define STOCK_CHARSET "{CHARSET}"\n')
            f.write(f"#define STOCK_NGLYPH {NG}\n\n")
            f.write("typedef struct { uint8_t left, width; } stock_metric_t;\n")
            f.write("typedef struct { uint8_t cols, rows, colbytes;"
                    " const uint8_t *bits; const stock_metric_t *met;"
                    " } stock_font_t;\n\n")
            for fid, (b, cols, rows, cb) in sorted(found.items()):
                stride = cols * cb + 1
                f.write(f"/* font {fid}: {cols}x{rows}, from VA "
                        f"0x{base+b:08X} */\n")
                f.write(f"static const uint8_t stock_font{fid}_bits"
                        f"[{NG}][{cols*cb}] = {{\n")
                for gi in range(NG):
                    gb = b + gi * stride
                    row = ", ".join(f"0x{d[gb+k]:02X}" for k in range(cols * cb))
                    f.write(f"  {{ {row} }},\n")
                f.write("};\n")
                # Proportional metrics measured from the bitmaps: the stock
                # advance table could not be decoded, so ink extents are used.
                f.write(f"static const stock_metric_t stock_font{fid}_met"
                        f"[{NG}] = {{\n")
                for gi in range(NG):
                    gb = b + gi * stride
                    xs = [x for x in range(cols)
                          if any(px(d, gb, cb, x, y) for y in range(rows))]
                    lo, w = (xs[0], xs[-1] - xs[0] + 1) if xs else (0, cols // 3)
                    f.write(f"  {{ {lo}, {w} }},"
                            f"  /* {CHARSET[gi]!r} */\n")
                f.write("};\n\n")
            f.write("static const stock_font_t stock_fonts[] = {\n")
            for fid, (b, cols, rows, cb) in sorted(found.items()):
                f.write(f"  {{ {cols}, {rows}, {cb}, "
                        f"&stock_font{fid}_bits[0][0], "
                        f"stock_font{fid}_met }},\n")
            f.write("};\n#define STOCK_NFONT "
                    f"{len(found)}\n\n#endif\n")
        print(f"\nwrote {a.emit}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
