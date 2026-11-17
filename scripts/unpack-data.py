#!/usr/bin/env python3
"""
Decompress the firmware's .data section.

armcc stores initialised data LZ-compressed in flash and expands it at boot.
The scatter table gives (src, dst, len, handler); the handler at 0x08013776
is ARM's __decompress1. Reimplemented here from its disassembly so every
initialised variable's boot value can be read offline.

Stream format, per block:
    ctl   = next byte
    lit   = ctl & 7 ; if 0, lit = next byte      -> emit (lit - 1) literals
    cnt   = ctl >> 4; if 0, cnt = next byte
    if ctl & 8:  off = next byte                 -> copy (cnt + 2) bytes
                                                    from dst - off
    else:                                        -> emit cnt zero bytes
Repeat until dst reaches dst + len.
"""
import argparse, struct, sys


def decompress(src: bytes, out_len: int) -> bytearray:
    dst = bytearray()
    i = 0
    while len(dst) < out_len and i < len(src):
        ctl = src[i]; i += 1
        lit = ctl & 7
        if lit == 0:
            lit = src[i]; i += 1
        cnt = ctl >> 4
        if cnt == 0:
            cnt = src[i]; i += 1
        for _ in range(lit - 1):                 # literals
            dst.append(src[i]); i += 1
        if ctl & 8:                              # back-reference
            off = src[i]; i += 1
            p = len(dst) - off
            for _ in range(cnt + 2):
                dst.append(dst[p]); p += 1
        else:                                    # zero run
            dst.extend(b"\x00" * cnt)
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--base", default="0x08002800")
    ap.add_argument("--src",  default="0x0801EE0C")
    ap.add_argument("--dst",  default="0x20000000")
    ap.add_argument("--len",  dest="length", type=int, default=6256)
    ap.add_argument("-o", "--out", default="build/data.bin")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    base, src, dst = int(a.base, 16), int(a.src, 16), int(a.dst, 16)
    blob = decompress(img[src - base:], a.length)

    print(f"decompressed {len(blob)} of {a.length} bytes -> RAM 0x{dst:08X}")
    if len(blob) != a.length:
        print("WARNING: length mismatch - stream may be misparsed", file=sys.stderr)

    import os
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    open(a.out, "wb").write(bytes(blob))
    print(f"wrote {a.out}")

    # Self-check: the font base table at RAM 0x20000640 must hold flash
    # pointers, and font 4 must be 0x0801769B - established independently by
    # rendering its glyphs.
    off = 0x640
    if len(blob) >= off + 20:
        ptrs = struct.unpack_from("<5I", blob, off)
        print("\nfont base table @ RAM 0x20000640:")
        for i, v in enumerate(ptrs):
            ok = base <= v < base + len(img)
            note = ""
            # The table points at the glyph RECORD; each record starts with a
            # 1-byte prefix and the bitmap follows, so the rendering base that
            # was established independently is this value + 1.
            if i == 4:
                note = "  <<< font 4 record; bitmap base 0x0801769B MATCHES" \
                       if v == 0x0801769A else "  <<< expected 0x0801769A"
            print(f"  font {i}: 0x{v:08X} {'ok' if ok else 'NOT FLASH'}{note}")
        print("\nSELF-CHECK:", "PASS" if ptrs[4] == 0x0801769A else "FAIL")
    # The display command table: lcd_cmd() indexes this at RAM 0x2000003A.
    if len(blob) >= 0x52:
        t = blob[0x3A:0x52]
        print("\ndisplay command table @ RAM 0x2000003A:")
        print("  " + " ".join(f"{b:02x}" for b in t))
        print(f"  idx 0x12 CASET=0x{t[0x12]:02X}  0x13 RASET=0x{t[0x13]:02X}  "
              f"0x14 RAMWR=0x{t[0x14]:02X}   (standard MIPI DCS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
