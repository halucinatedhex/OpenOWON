#!/usr/bin/env python3
"""
Disassemble a region and annotate it: resolve every PC-relative literal load to
its value, and if that value points at a string in the image, show the string.

Usage: scripts/fw-annot.py <image> <start_hex> <len_hex> [--base 0x08002800]
"""
import argparse, os, re, struct, subprocess, sys, glob

OBJDUMP = (glob.glob(os.path.expanduser(
    "~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objdump")) or [None])[0]


def load(img):
    return open(img, "rb").read()


def string_at(d, off, maxlen=60):
    if not (0 <= off < len(d)):
        return None
    end = d.find(b"\x00", off)
    if end < 0 or end - off < 2 or end - off > maxlen:
        return None
    s = d[off:end]
    try:
        t = s.decode("ascii")
    except UnicodeDecodeError:
        return None
    return t if all(32 <= c < 127 for c in s) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image"); ap.add_argument("start"); ap.add_argument("length")
    ap.add_argument("--base", default="0x08002800")
    a = ap.parse_args()
    base = int(a.base, 16); start = int(a.start, 16); ln = int(a.length, 16)
    d = load(a.image)

    out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "arm_any",
                          "-M", "force-thumb", f"--adjust-vma={base}",
                          f"--start-address={start}", f"--stop-address={start+ln}",
                          a.image], capture_output=True, text=True).stdout

    lit = re.compile(r";\s*\(?(0x[0-9a-f]+)\)?$")
    for line in out.splitlines():
        if not re.match(r"\s*[0-9a-f]+:", line):
            continue
        note = ""
        m = lit.search(line.strip())
        if m and ("ldr" in line or "adr" in line):
            la = int(m.group(1), 16)
            off = la - base
            if 0 <= off + 4 <= len(d):
                val = struct.unpack_from("<I", d, off)[0]
                note = f"   => 0x{val:08X}"
                s = string_at(d, val - base)
                if s is not None:
                    note += f'  "{s}"'
                elif 0x20000000 <= val < 0x20030000:
                    note += "  (RAM)"
                elif 0x40000000 <= val < 0x50000000:
                    note += "  (PERIPHERAL)"
                elif base <= val < base + len(d):
                    note += "  (code/data)"
        print(line + note)


if __name__ == "__main__":
    sys.exit(main())
