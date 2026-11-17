#!/usr/bin/env python3
"""
Recover the GPIO pin map from every SPL GPIO_Init call in the decompilation.

SPL's GPIO_InitTypeDef is {uint16 pin; uint8 speed; uint8 mode}, so a 32-bit
initialiser 0x10030002 decodes as pin 0x0002, speed 3 (50 MHz), mode 0x10
(Out_PP). Ghidra renders these as an assignment to a local immediately before
the call, so the two are matched by scanning each function in order.
"""
import re, sys, collections

MODE = {0x00: "AIN", 0x04: "IN_FLOAT", 0x28: "IPD", 0x48: "IPU",
        0x10: "OUT_PP", 0x14: "OUT_OD", 0x18: "AF_PP", 0x1C: "AF_OD"}
SPEED = {1: "10MHz", 2: "2MHz", 3: "50MHz"}
PORT = {0x40010800: "A", 0x40010C00: "B", 0x40011000: "C",
        0x40011400: "D", 0x40011800: "E"}

def main(path, decomp):
    # Resolve the DAT_ constants Ghidra uses for port bases.
    img = open(path, "rb").read()
    import struct
    BASE = 0x08002800
    dat = {}
    for m in re.finditer(r"DAT_(0[0-9a-f]{7})", open(decomp).read()):
        a = int(m.group(1), 16)
        o = a - BASE
        if 0 <= o + 4 <= len(img):
            dat[m.group(0)] = struct.unpack_from("<I", img, o)[0]

    txt = open(decomp).read()
    parts = re.split(r"/\* ===== (\S+) @ (\S+) ===== \*/", txt)
    found = collections.defaultdict(list)

    for i in range(1, len(parts), 3):
        fname, faddr, body = parts[i], parts[i + 1], parts[i + 2]
        pending = {}          # local name -> last assigned 32-bit value
        alias = {}            # temp var -> port base, e.g. uVar1 = DAT_08006070
        for line in body.splitlines():
            al = re.match(r"\s*(\w+) = (DAT_0[0-9a-f]{7});", line)
            if al and al.group(2) in dat:
                alias[al.group(1)] = dat[al.group(2)]
                continue
            cc = re.match(r"\s*(local_[0-9a-f]+) = CONCAT22\([^,]+,(0x[0-9a-f]+|\d+)\);", line)
            if cc:
                prev = pending.get(cc.group(1), 0)
                pending[cc.group(1)] = (prev & 0xFFFF0000) | (int(cc.group(2), 0) & 0xFFFF)
                continue
            a = re.match(r"\s*(local_[0-9a-f]+)(?:\._0_2_)? = (0x[0-9a-f]+|\d+);", line)
            if a:
                v = int(a.group(2), 0)
                if "._0_2_" in line:                 # only the pin field changed
                    prev = pending.get(a.group(1), 0)
                    v = (prev & 0xFFFF0000) | (v & 0xFFFF)
                pending[a.group(1)] = v
                continue
            c = re.search(r"stock_GPIO_Init\(([^,]+),&(local_[0-9a-f]+)\)", line)
            if c:
                port_tok, loc = c.group(1).strip(), c.group(2)
                base = dat.get(port_tok, alias.get(port_tok))
                v = pending.get(loc)
                if base is None or v is None:
                    continue
                pin, speed, mode = v & 0xFFFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF
                p = PORT.get(base)
                if not p or not pin:
                    continue
                for b in range(16):
                    if pin & (1 << b):
                        found[f"P{p}{b}"].append((MODE.get(mode, f"0x{mode:02X}"),
                                                  SPEED.get(speed, str(speed)),
                                                  fname))
    print(f"{len(found)} pins configured\n")
    def key(k):
        return (k[1], int(k[2:]))
    for pin in sorted(found, key=key):
        for mode, speed, fn in found[pin]:
            print(f"  {pin:<5} {mode:<9} {speed:<6} {fn}")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "firmware/XDM1041_OS_V2.1.2_20210525.bin",
         sys.argv[2] if len(sys.argv) > 2 else "ghidra/out/xdm_decomp.c")
