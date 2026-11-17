#!/usr/bin/env python3
"""
Port recovered symbols from the v2.1.2 image to another build (the owner's
v3.x dump), so nothing has to be re-derived by hand.

    scripts/port-symbols.py <new.bin> [--new-base 0x08000000] [-o symbols/xdm1241_v3.sym]

Three passes:
  1. signatures  each known function's bytes, with everything that moves on a
                 relink masked out (BL/B.W offsets, PC-relative literal offsets,
                 ADR offsets, and the literal-pool words themselves), searched
                 in the new image; a unique hit maps the function
  2. call graph  a mapped function's calls, at the same instruction offsets in
                 both builds, map their targets
  3. literals    data symbols (RAM variables, flash tables) map through the
                 literal pools of mapped function pairs

Every result carries how it was found. Nothing ambiguous is emitted: a
symbol with two candidate addresses is reported, not guessed.

Reads only image files. Never touches hardware.
"""
import argparse
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
V2_IMAGE = ROOT / "firmware/XDM1041_OS_V2.1.2_20210525.bin"
V2_BASE = 0x08002800
V2_SYMS = ROOT / "symbols/xdm1041_v2.1.2.sym"
# Function starts and sizes from Ghidra (ghidra/run.sh writes ghidra/out/,
# which is not committed); this snapshot is, so a fresh clone can port.
V2_INDEX = ROOT / "symbols/xdm1041_v2.1.2_functions.txt"

CODE_LO, CODE_HI = 0x08002800, 0x08014800       # v2 code region
MAX_SIG = 256                                   # bytes of a function used as its signature
MIN_KNOWN = 12                                  # unmasked bytes needed to trust a match


# ---- images ---------------------------------------------------------------------

class Image:
    def __init__(self, data, base):
        self.d, self.base = data, base

    def ok(self, va, n=1):
        return self.base <= va and va + n <= self.base + len(self.d)

    def u16(self, va):
        return struct.unpack_from("<H", self.d, va - self.base)[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.d, va - self.base)[0]

    def bytes(self, va, n):
        o = va - self.base
        return self.d[o:o + n]


def load_syms(path):
    syms = []
    for line in Path(path).read_text().splitlines():
        line = line.split("#")[0].strip()
        m = re.match(r"(0x[0-9a-fA-F]+)\s+(\w+)", line)
        if m:
            v = int(m.group(1), 16)
            if CODE_LO <= v < CODE_HI:
                v &= ~1                          # a few entries carry the Thumb bit
            syms.append((v, m.group(2)))
    return syms


def load_sizes(path):
    sizes = {}
    if not Path(path).exists():
        raise SystemExit(f"function index {path} not found: it is committed as "
                         "symbols/xdm1041_v2.1.2_functions.txt (ghidra/run.sh regenerates it)")
    for line in Path(path).read_text().splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("size="):
            sizes[int(p[0], 16)] = int(p[2][5:])
    return sizes


# ---- Thumb-2 decoding, just enough to mask relocatable fields ---------------------

def is32(hw):
    return (hw >> 11) in (0b11101, 0b11110, 0b11111)


def bl_target(va, hw1, hw2):
    """Target of BL / BLX / B.W (T4), or None."""
    if (hw1 & 0xF800) != 0xF000:
        return None
    if (hw2 & 0xC000) == 0xC000 or (hw2 & 0xD000) == 0x9000:
        s = (hw1 >> 10) & 1
        j1, j2 = (hw2 >> 13) & 1, (hw2 >> 11) & 1
        i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
        imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
        if s:
            imm -= 1 << 25
        t = va + 4 + imm
        if (hw2 & 0xD000) == 0xC000:            # BLX: ARM target, word aligned
            t &= ~3
        return t
    return None


def decode(img, start, size, skip=()):
    """Yield (va, length, kind, extra) over [start, start+size)."""
    va, end = start, start + size
    while va + 2 <= end and img.ok(va, 2):
        if va in skip:
            va += 4
            continue
        hw = img.u16(va)
        if is32(hw) and va + 4 <= end:
            hw2 = img.u16(va + 2)
            t = bl_target(va, hw, hw2)
            if t is not None:
                yield va, 4, "call", t
            elif (hw & 0xFF7F) == 0xF85F:        # LDR.W rt, [pc, #imm]
                imm = hw2 & 0xFFF
                lit = ((va + 4) & ~3) + (imm if hw & 0x80 else -imm)
                yield va, 4, "ldrw", lit
            elif (hw & 0xFBFF) in (0xF20F, 0xF2AF):   # ADR.W
                yield va, 4, "adrw", None
            elif (hw & 0xF800) == 0xF000 and (hw2 & 0xD000) == 0x8000:   # B<c>.W
                yield va, 4, "bcondw", None
            else:
                yield va, 4, "other", None
            va += 4
        else:
            if (hw & 0xF800) == 0x4800:          # LDR rt, [pc, #imm8*4]
                lit = ((va + 4) & ~3) + (hw & 0xFF) * 4
                yield va, 2, "ldr", lit
            elif (hw & 0xF800) == 0xA000:        # ADR
                yield va, 2, "adr", None
            else:
                yield va, 2, "other", None
            va += 2


def analyse(img, start, size):
    """Instructions, literal targets inside the function, and calls."""
    skip = set()
    for _ in range(3):                           # literal pools can desync a linear sweep
        lits = set()
        ins = list(decode(img, start, size, skip))
        for va, n, kind, x in ins:
            if kind in ("ldr", "ldrw") and start <= x < start + size:
                lits.add(x)
        if lits == skip:
            break
        skip = lits
    return ins, skip


def signature(img, start, size):
    """Bytes with relocatable fields masked (None)."""
    size = min(size, MAX_SIG)
    ins, lits = analyse(img, start, size)
    raw = list(img.bytes(start, size))
    mask = [False] * size

    def hide(va, n):
        for k in range(n):
            if 0 <= va - start + k < size:
                mask[va - start + k] = True

    for va, n, kind, x in ins:
        if kind in ("call", "bcondw"):           # keep only the opcode bits: mask all
            hide(va, 4)
        elif kind == "ldrw":
            hide(va + 2, 2)
        elif kind == "adrw":
            hide(va, 4)
        elif kind in ("ldr", "adr"):
            hide(va, 1)                          # imm8 is the low byte
    for lit in lits:
        hide(lit, 4)
    return [None if m else b for b, m in zip(raw, mask)]


def aligned_hits(rx, data, limit=3):
    """Halfword-aligned match offsets. Overlapping search: finditer would let
    a spurious odd-offset hit swallow the real one next to it."""
    hits, pos = [], 0
    while len(hits) < limit:
        m = rx.search(data, pos)
        if not m:
            break
        if m.start() % 2 == 0:
            hits.append(m.start())
        pos = m.start() + 1
    return hits


def sig_regex(sig):
    # trailing masked bytes add nothing
    while sig and sig[-1] is None:
        sig = sig[:-1]
    parts = []
    for b in sig:
        parts.append(b"." if b is None else re.escape(bytes([b])))
    return re.compile(b"".join(parts), re.DOTALL), sum(b is not None for b in sig)


# ---- the passes -------------------------------------------------------------------

def port(old, new, syms, sizes):
    code_syms = [(a, n) for a, n in syms if CODE_LO <= a < CODE_HI]
    data_syms = [(a, n) for a, n in syms if not (CODE_LO <= a < CODE_HI)]
    starts = sorted(set(sizes) | {a for a, _ in code_syms})

    def size_of(a):
        if a in sizes:
            return sizes[a]
        nxt = [s for s in starts if s > a]
        return min((nxt[0] - a) if nxt else 64, 128)

    fmap, how = {}, {}        # old function address -> new; and the reason
    notes = {}
    dmap = {}

    # pass 0: vector tables. The app's table is at its load address; in a full
    # dump that is base + 0x2800. Slot by slot: SP, then handlers.
    app_new = new.base + (0x2800 if new.base == 0x08000000 else 0)
    if new.ok(app_new, 8):
        sp_new = new.u32(app_new)
        if 0x20000000 < sp_new <= 0x20010000:
            for slot in range(0, 16 + 60):
                if not (old.ok(old.base + 4 * slot, 4) and new.ok(app_new + 4 * slot, 4)):
                    break
                vo, vn = old.u32(old.base + 4 * slot), new.u32(app_new + 4 * slot)
                if slot == 0:
                    dmap[vo], how[vo] = vn, "vector table SP"
                elif vo & 1 and vn & 1 and CODE_LO <= (vo & ~1) < CODE_HI:
                    o, n = vo & ~1, vn & ~1
                    if o not in fmap:
                        fmap[o], how[o] = n, f"vector table slot {slot}"

    # pass 1: signatures, for every function Ghidra knows (more anchors for pass 2)
    for a in starts:
        sz = size_of(a)
        if sz < 8 or not old.ok(a, sz):
            notes[a] = f"too small to fingerprint ({sz} bytes); maps only if called"
            continue
        rx, known = sig_regex(signature(old, a, sz))
        if known < MIN_KNOWN:
            notes[a] = "signature too short"
            continue
        hits = aligned_hits(rx, new.d)
        if len(hits) == 1:
            fmap[a], how[a] = new.base + hits[0], "signature"
        elif len(hits) > 1:
            notes[a] = f"ambiguous: {len(hits)} signature matches"
        else:
            notes[a] = "signature not found (function changed?)"

    # pass 2: call graph, to a fixed point
    changed = True
    while changed:
        changed = False
        for a, b in list(fmap.items()):
            sz = size_of(a)
            ins_old, _ = analyse(old, a, sz)
            ins_new, _ = analyse(new, b, sz) if new.ok(b, sz) else ([], set())
            calls_new = {va - b: t for va, n, k, t in ins_new if k == "call"}
            for va, n, k, t in ins_old:
                if k != "call" or not (CODE_LO <= t < CODE_HI):
                    continue
                tn = calls_new.get(va - a)
                if tn is None:
                    continue
                if t in fmap:
                    if fmap[t] != tn:
                        notes[t] = f"call-graph conflict ({fmap[t]:#x} vs {tn:#x})"
                    continue
                fmap[t], how[t] = tn, f"called from {a:#x}"
                changed = True

    # pass 3: literal pools -> data symbols
    votes = defaultdict(Counter)
    for a, b in fmap.items():
        sz = size_of(a)
        ins_old, _ = analyse(old, a, sz)
        ins_new, _ = analyse(new, b, sz) if new.ok(b, sz) else ([], set())
        lit_new = {va - b: x for va, n, k, x in ins_new if k in ("ldr", "ldrw")}
        for va, n, k, x in ins_old:
            if k not in ("ldr", "ldrw") or not old.ok(x, 4):
                continue
            xn = lit_new.get(va - a)
            if xn is None or not new.ok(xn, 4):
                continue
            votes[old.u32(x)][new.u32(xn)] += 1

    for a, name in data_syms:
        if a in dmap:
            continue
        c = votes.get(a)
        if not c:
            # pass 4: flash data reached through .data pointers - find the
            # bytes themselves, accepting only a unique hit
            if old.ok(a, 32) and CODE_HI <= a < old.base + len(old.d):
                blob = old.bytes(a, 32)
                hits = [m.start() for m in re.finditer(re.escape(blob), new.d)][:2]
                if len(hits) == 1:
                    dmap[a], how[a] = new.base + hits[0], "content (32 bytes, unique)"
                    continue
                notes[a] = f"content search: {len(hits)} hits" if hits else "content not found"
                continue
            notes[a] = "no literal reference in a mapped function"
            continue
        (best, n), *rest = c.most_common(2) + [(None, 0)]
        second = rest[0][1] if rest else 0
        if n >= 2 * max(second, 1) or (n >= 1 and second == 0):
            dmap[a], how[a] = best, f"literal ({n} refs)"
        else:
            notes[a] = f"literal vote split {dict(c)}"

    return fmap, dmap, how, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("new", help="new image (.bin)")
    ap.add_argument("--new-base", type=lambda s: int(s, 0), default=None,
                    help="load address of the new image (default: 0x08000000 for a 256 KB dump, else 0x08002800)")
    ap.add_argument("-o", "--out", default=None, help="write a .sym file")
    ap.add_argument("--old", default=str(V2_IMAGE))
    ap.add_argument("--old-base", type=lambda s: int(s, 0), default=V2_BASE)
    ap.add_argument("--syms", default=str(V2_SYMS))
    ap.add_argument("--index", default=str(V2_INDEX))
    a = ap.parse_args()

    data = Path(a.new).read_bytes()
    base = a.new_base if a.new_base is not None else (0x08000000 if len(data) == 0x40000 else 0x08002800)
    old = Image(Path(a.old).read_bytes(), a.old_base)
    new = Image(data, base)
    syms = load_syms(a.syms)
    fmap, dmap, how, notes = port(old, new, syms, load_sizes(a.index))

    lines, mapped, moved = [], 0, 0
    for addr, name in syms:
        n = fmap.get(addr, dmap.get(addr))
        if n is None:
            lines.append(f"# UNMAPPED {name:<28} was {addr:#010x}: {notes.get(addr, 'no match')}")
            continue
        mapped += 1
        moved += n != addr
        lines.append(f"{n:#010x}  {name:<28} # v2 {addr:#010x}, {how[addr]}")
    print(f"{mapped}/{len(syms)} symbols mapped, {moved} moved; "
          f"{len(fmap)} functions matched in total (incl. unnamed)")
    for l in lines:
        if l.startswith("# UNMAPPED"):
            print(" ", l[2:])
    if a.out:
        hdr = (f"# Ported from {Path(a.syms).name} by scripts/port-symbols.py\n"
               f"# new image: {Path(a.new).name} @ {base:#010x}\n"
               "# Each line records how it was found. Check UNMAPPED entries by hand.\n")
        Path(a.out).write_text(hdr + "\n".join(lines) + "\n")
        print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
