#!/usr/bin/env python3
"""
Recover the front-end mode table from OWON's v2.1.2 image by running it.

Stock was erased from our meter before its HY3131 writes could be captured
with a logic analyser, so this runs v2.1.2's own code in a Cortex-M
emulator instead (docs/analog-frontend.md):

  1. the reset handler up to stock_boot_init, so .data is unpacked and RAM
     holds stock's real initial state;
  2. stock_meas_set_function(func, range, 0) for every function and range;
  3. a log of what each call does to the outside world: every HY3131
     register write (hooked at stock's access layer, which sees the
     logical index and the bytes) and every GPIO register write (the relays
     are on GPIO, through the ELD207 opto).

    scripts/emu-afe-table.py [--image firmware/...bin] [--json out.json]

Needs unicorn (pip install unicorn). Offline; touches no hardware.

What it is NOT: the v3 board's values. v2.1.2 is built for the older STM32
board. The analog side is believed shared, but every entry needs checking
on the meter before it is trusted (docs/analog-frontend.md).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
                     UC_HOOK_CODE)
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x08002800
FLASH, FLEN = 0x08000000, 0x40000
RAM, RLEN = 0x20000000, 0x10000

# stock v2.1.2 addresses (symbols/xdm1041_v2.1.2.sym)
RESET_HANDLER = 0x08002900
BOOT_INIT = 0x0800465C           # main's first call: stop here
SET_FUNCTION = 0x08008A50        # (func, range, flag)
HY_WRITE_REGS = 0x0800784A       # (idx, buf, n)
HY_WRITE_REG1 = 0x0800787C       # (idx, value)
HY_READ_REGS = 0x08007888        # (idx, buf, n)
DELAY_BUSY = 0x08004A4A
HY_INDEX_TABLE = 0x0801DD5C      # logical index -> register address
RETURN_TRAP = 0x08000100         # unused flash below the app: "returned"

FUNCS = ["DCV", "ACV", "DCI", "ACI", "OHM2", "OHM4", "CONT", "DIODE", "CAP",
         "FREQ", "TEMP"]
GPIO = {0x40010800: "A", 0x40010C00: "B", 0x40011000: "C", 0x40011400: "D",
        0x40011800: "E"}
GPIO_REG = {0x00: "CRL", 0x04: "CRH", 0x0C: "ODR", 0x10: "BSRR", 0x14: "BRR"}


class Emu:
    def __init__(self, image):
        self.img = image
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(FLASH, FLEN)
        flash = bytearray(b"\xff" * FLEN)
        flash[BASE - FLASH:BASE - FLASH + len(image)] = image
        flash[RETURN_TRAP - FLASH:RETURN_TRAP - FLASH + 2] = b"\xfe\xe7"   # b .
        uc.mem_write(FLASH, bytes(flash))
        uc.mem_map(RAM, RLEN)
        self.regs = {}
        self.events = None            # list while recording, else None
        self.hy_regs = {}             # simulated HY3131 register file
        uc.mmio_map(0x40000000, 0x30000, self.p_read, None, self.p_write, None)
        uc.mmio_map(0xE000E000, 0x1000, self.c_read, None, self.c_write, None)
        # Bit-band alias of the peripheral region: one word per bit.
        uc.mmio_map(0x42000000, 0x600000, self.bb_read, None, self.bb_write, None)
        for addr, fn in ((HY_WRITE_REGS, self.h_write_regs),
                         (HY_WRITE_REG1, self.h_write_reg1),
                         (HY_READ_REGS, self.h_read_regs),
                         (DELAY_BUSY, self.h_return0)):
            uc.hook_add(UC_HOOK_CODE, fn, begin=addr, end=addr)
        self.index = [image[HY_INDEX_TABLE - BASE + i] for i in range(36)]

    # -- peripherals: enough to get through SystemInit, and a GPIO log
    def p_read(self, uc, off, size, _):
        a = 0x40000000 + off
        v = self.regs.get(a, 0)
        if a == 0x40021000:                    # RCC_CR: every clock ready
            v |= (1 << 1) | (1 << 17) | (1 << 25)
        elif a == 0x40021004:                  # RCC_CFGR: SWS follows SW
            v = (v & ~0xC) | ((v & 3) << 2)
        elif a in (0x40021020, 0x40021024):    # RCC_BDCR LSERDY, RCC_CSR LSIRDY
            v |= 1 << 1
        elif a == 0x40002804:                  # RTC_CRL: RTOFF, RSF (synchronised)
            v |= (1 << 5) | (1 << 3)
        return v & ((1 << (8 * size)) - 1)

    def p_write(self, uc, off, size, value, _):
        a = 0x40000000 + off
        self.regs[a] = value
        port = a & ~0x3FF
        if port in GPIO and self.events is not None and (a - port) in GPIO_REG:
            self.events.append(("gpio", GPIO[port], GPIO_REG[a - port], value))

    def bb_target(self, off):
        byte, bit = off // 32, (off // 4) % 8
        word = 0x40000000 + (byte & ~3)
        return word, (byte & 3) * 8 + bit

    def bb_read(self, uc, off, size, _):
        word, bit = self.bb_target(off)
        return (self.p_read(uc, word - 0x40000000, 4, None) >> bit) & 1

    def bb_write(self, uc, off, size, value, _):
        word, bit = self.bb_target(off)
        v = self.regs.get(word, 0)
        v = (v | (1 << bit)) if value & 1 else (v & ~(1 << bit))
        self.p_write(uc, word - 0x40000000, 4, v, None)

    def c_read(self, uc, off, size, _):
        a = 0xE000E000 + off
        v = self.regs.get(a, 0)
        if a == 0xE000E010:
            v |= 1 << 16                       # SysTick COUNTFLAG: time flies
        return v

    def c_write(self, uc, off, size, value, _):
        self.regs[0xE000E000 + off] = value

    # -- stock's HY3131 access layer, replaced
    def ret(self, r0=0):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_R0, r0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR) | 1)

    def h_write_regs(self, uc, addr, size, _):
        idx, buf, n = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        data = bytes(uc.mem_read(buf, n))
        reg = self.index[idx] if idx < len(self.index) else None
        for i, b in enumerate(data):
            if reg is not None:
                self.hy_regs[reg + i] = b
        if self.events is not None:
            self.events.append(("hy", reg, data))
        self.ret()

    def h_write_reg1(self, uc, addr, size, _):
        idx, val = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1) & 0xFF
        reg = self.index[idx] if idx < len(self.index) else None
        if reg is not None:
            self.hy_regs[reg] = val
        if self.events is not None:
            self.events.append(("hy", reg, bytes([val])))
        self.ret()

    def h_read_regs(self, uc, addr, size, _):
        idx, buf, n = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        reg = self.index[idx] if idx < len(self.index) else 0
        if reg == 0x1E:                        # INTF: every conversion ready
            data = bytes([0xFF] * n)
        else:
            data = bytes(self.hy_regs.get(reg + i, 0) for i in range(n))
        uc.mem_write(buf, data)
        self.ret()

    def h_return0(self, uc, addr, size, _):
        self.ret()

    # -- running
    def run(self, start, stop, limit=50_000_000):
        uc = self.uc
        try:
            uc.emu_start(start | 1, stop, count=limit)
        except UcError as e:
            raise RuntimeError(f"emulation fault at 0x{uc.reg_read(UC_ARM_REG_PC):08X}: {e}")
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc != stop:
            raise RuntimeError(f"did not reach 0x{stop:08X} (stuck at 0x{pc:08X})")

    def boot(self):
        sp = struct.unpack_from("<I", self.img, 0)[0]
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.run(RESET_HANDLER, BOOT_INIT)
        self.sp = sp

    def init(self):
        """Run stock_boot_init itself, so the HY3131 register copy holds what
        start-up sets once (bias, references, enables). Returns its events."""
        return self.call(BOOT_INIT)

    def call(self, fn, *args):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, self.sp - 0x400)
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        self.events = []
        try:
            self.run(fn, RETURN_TRAP)
        except RuntimeError as e:
            self.partial, self.events = self.events, None
            raise
        ev, self.events = self.events, None
        return ev


def describe(events):
    out = []
    for e in events:
        if e[0] == "hy":
            _, reg, data = e
            out.append(f"HY 0x{reg:02X} <- {data.hex(' ')}" if reg is not None else f"HY ? <- {data.hex(' ')}")
        else:
            _, port, r, v = e
            if r == "BSRR":
                sets = [f"P{port}{i}" for i in range(16) if v & (1 << i)]
                clrs = [f"P{port}{i}" for i in range(16) if v & (1 << (16 + i))]
                out.append("GPIO " + " ".join([f"{p}=1" for p in sets] + [f"{p}=0" for p in clrs]))
            elif r == "BRR":
                out.append("GPIO " + " ".join(f"P{port}{i}=0" for i in range(16) if v & (1 << i)))
            else:
                out.append(f"GPIO{port} {r} = 0x{v:08X}")
    return out


def fresh(image):
    emu = Emu(image)
    emu.boot()
    try:
        emu.init()
    except RuntimeError:
        pass                          # runs on into UI work; the front-end set-up is done
    return emu


def clean_sweep(image, a):
    table = {}
    for f, name in enumerate(FUNCS):
        emu = fresh(image)            # each function from start-up state
        for r in range(a.ranges):     # its ranges in order, as the meter steps them
            try:
                ev = emu.call(SET_FUNCTION, f, r, 0)
            except RuntimeError as e:
                print(f"{name} r{r}: {e}")
                continue
            table[f"{name}/{r}"] = describe(ev)
            print(f"--- {name} range {r}")
            for l in table[f"{name}/{r}"]:
                print("   " + l)
    if a.json:
        Path(a.json).write_text(json.dumps(table, indent=1))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=str(ROOT / "firmware/XDM1041_OS_V2.1.2_20210525.bin"))
    ap.add_argument("--json")
    ap.add_argument("--ranges", type=int, default=7)
    ap.add_argument("--clean", action="store_true",
                    help="boot afresh for every function, so no function inherits another's "
                         "state (the sequential sweep carries e.g. FREQ's counter set-up into TEMP)")
    a = ap.parse_args(argv)
    image = Path(a.image).read_bytes()
    if a.clean:
        return clean_sweep(image, a)

    emu = Emu(image)
    emu.boot()
    print(f"booted to stock_boot_init; HY index table: "
          + " ".join(f"{r:02x}" for r in emu.index[:12]) + " ...")
    try:
        boot_ev = emu.init()
        print(f"--- stock_boot_init: {len(boot_ev)} events")
        for l in describe(boot_ev):
            if l.startswith("HY") or "PA8" in l or "PA13" in l:
                print("   " + l)
    except RuntimeError as e:
        # It runs on into UI work the emulator doesn't model; the front-end
        # set-up comes first and stays in RAM. Show what it did get to.
        print(f"stock_boot_init: {e}; {len(emu.partial)} events before that")
        for l in describe(emu.partial):
            if l.startswith("HY") or "PA8" in l or "PA13" in l:
                print("   " + l)
    table = {}
    for f, name in enumerate(FUNCS):
        for r in range(a.ranges):
            try:
                ev = emu.call(SET_FUNCTION, f, r, 0)
            except RuntimeError as e:
                print(f"{name} r{r}: {e}")
                continue
            lines = describe(ev)
            table[f"{name}/{r}"] = lines
            print(f"--- {name} range {r}")
            for l in lines:
                print("   " + l)
    if a.json:
        Path(a.json).write_text(json.dumps(table, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
