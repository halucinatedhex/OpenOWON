#!/usr/bin/env python3
"""
Build a modified firmware image: take a stock dump, splice in an extension
binary placed in free flash, and apply hooks that divert stock code to it.

    attic/extension/mkpatch.py --stock fw.bin --base 0x08002800 \
        --place 0x08020000=ext.bin \
        --patch attic/extension/patches.txt -o out.bin

patches.txt directives (one per line, '#' comments):
    vector <idx> <addr>      redirect vector-table entry idx (0=SP,1=Reset,...)
    bl     <at> <target>     write a Thumb-2 BL at <at>
    b      <at> <target>     write a Thumb-2 B.W at <at>
    bytes  <at> <hexbytes>   raw byte patch
"""
import argparse, struct, sys


def enc_bl(at, target, link=True):
    """Thumb-2 BL/B.W (T1/T4). at and target are absolute VAs; target may carry
    the Thumb bit, which is not encoded."""
    off = (target & ~1) - (at + 4)
    if off % 2:
        raise ValueError("odd branch offset")
    if not (-(1 << 24) <= off < (1 << 24)):
        raise ValueError(f"branch out of +/-16MB range: 0x{at:08X} -> 0x{target:08X}")
    s = (off >> 24) & 1
    i1 = (off >> 23) & 1
    i2 = (off >> 22) & 1
    imm10 = (off >> 12) & 0x3FF
    imm11 = (off >> 1) & 0x7FF
    j1 = (~i1 & 1) ^ s
    j2 = (~i2 & 1) ^ s
    hi = 0xF000 | (s << 10) | imm10
    lo = (0xD000 if link else 0x9000) | (j1 << 13) | (j2 << 11) | imm11
    return struct.pack("<HH", hi, lo)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--base", default="0x08002800")
    ap.add_argument("--place", action="append", default=[], metavar="ADDR=FILE")
    ap.add_argument("--patch")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    base = int(a.base, 16)
    img = bytearray(open(a.stock, "rb").read())
    print(f"stock: {len(img)} bytes @ 0x{base:08X}-0x{base+len(img):08X}")

    def w(addr, data, what):
        off = addr - base
        if off < 0:
            sys.exit(f"ERROR: 0x{addr:08X} is below the image base")
        if off + len(data) > len(img):
            img.extend(b"\xff" * (off + len(data) - len(img)))
        img[off:off + len(data)] = data
        print(f"  {what:<22} 0x{addr:08X}  {len(data)} bytes")

    for spec in a.place:
        addr, _, path = spec.partition("=")
        blob = open(path, "rb").read()
        w(int(addr, 16), blob, f"place {path}")

    if a.patch:
        for ln, raw in enumerate(open(a.patch), 1):
            line = raw.split("#")[0].strip()
            if not line:
                continue
            op, *args = line.split()
            try:
                if op == "vector":
                    idx, addr = int(args[0], 0), int(args[1], 16)
                    w(base + idx * 4, struct.pack("<I", addr | 1), f"vector[{idx}]")
                elif op in ("bl", "b"):
                    at, tgt = int(args[0], 16), int(args[1], 16)
                    w(at, enc_bl(at, tgt, link=(op == "bl")), f"{op} -> 0x{tgt:08X}")
                elif op == "bytes":
                    w(int(args[0], 16), bytes.fromhex(args[1]), "bytes")
                else:
                    sys.exit(f"{a.patch}:{ln}: unknown directive {op!r}")
            except Exception as e:
                sys.exit(f"{a.patch}:{ln}: {e}")

    open(a.out, "wb").write(img)
    print(f"wrote {a.out}  ({len(img)} bytes, ends 0x{base+len(img):08X})")


if __name__ == "__main__":
    main()
