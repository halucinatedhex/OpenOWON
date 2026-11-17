#!/usr/bin/env python3
"""Run the REAL dumper image (.pio/build/xdm1241-dumper) in a Cortex-M
emulator with a USART model, end to end: startup, clock, VTOR, banner,
*IDN?, an unknown command, DIAG:BOOT? and the full DIAG:FLASH?, parsed and
analysed by read-boot.py. The dumper runs on the meter once, with no
debugger, so this is its rehearsal.

Needs the `unicorn` package (pip install unicorn); skips without it or
without a dumper build.

Flash: bootloader area = pattern A, dumper at 0x08002800 (padded 0xFF to the
pack end, as the installer sends it), stock remains = pattern B, factory
page = pattern C. Feed commands, parse replies with read-boot.py's parser.
"""
import importlib.util
import sys
from pathlib import Path

try:
    from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UcError,
                     UC_HOOK_CODE)
    from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP
except ImportError:
    print("test_dumper_emu: skipped (pip install unicorn to run it)")
    sys.exit(0)

REPO = Path(__file__).resolve().parent.parent
_img = REPO / ".pio/build/xdm1241-dumper/firmware.bin"
if not _img.exists():
    print("test_dumper_emu: skipped (no dumper build: pio run -e xdm1241-dumper)")
    sys.exit(0)
IMG = _img.read_bytes()
spec = importlib.util.spec_from_file_location("rb", REPO / "scripts/read-boot.py")
rb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rb)

FLASH, FLEN = 0x08000000, 0x40000
flash = bytearray(b"\xff" * FLEN)
for i in range(0x2800):
    flash[i] = (i * 13 + 7) & 0xFF                        # "bootloader"
pack = IMG + b"\xff" * (10240 - len(IMG))
flash[0x2800:0x2800 + 10240] = pack                     # the dumper's pack
for i in range(0x5000, 0x1D000):
    flash[i] = (i * 31 + 3) & 0xFF                        # "stock remains"
for i in range(0x3F800, 0x3F800 + 1392):
    flash[i] = (i * 5) & 0xFF                             # "factory record"

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(FLASH, FLEN)
uc.mem_write(FLASH, bytes(flash))
uc.mem_map(0x00000000, FLEN)                             # boot alias
uc.mem_write(0, bytes(flash))
uc.mem_map(0x20000000, 0xC000)

regs = {}
tx = bytearray()
rxq = bytearray()
systick_reads = [0]
USART = 0x40013800


def periph_read(uc, off, size, _):
    addr = 0x40000000 + off
    v = regs.get(addr, 0)
    if addr == 0x40021000:                     # RCU_CTL: everything stable
        v |= (1 << 1) | (1 << 17) | (1 << 25)
    elif addr == 0x40021004:                   # RCU_CFG0: SCSS follows SCS
        v = (v & ~0xC) | ((v & 3) << 2)
    elif addr == USART:                        # STAT0
        v = (1 << 7) | (1 << 6) | ((1 << 5) if rxq else 0)
    elif addr == USART + 4:                    # DATA
        v = rxq.pop(0) if rxq else 0
    return v


def periph_write(uc, off, size, value, _):
    addr = 0x40000000 + off
    regs[addr] = value
    if addr == USART + 4:
        tx.append(value & 0xFF)


def core_read(uc, off, size, _):
    addr = 0xE000E000 + off
    v = regs.get(addr, 0)
    if addr == 0xE000E010:                     # SysTick CTRL: COUNTFLAG sometimes
        systick_reads[0] += 1
        if systick_reads[0] % 50 == 0:
            v |= 1 << 16
    return v


def core_write(uc, off, size, value, _):
    regs[0xE000E000 + off] = value


uc.mmio_map(0x40000000, 0x30000, periph_read, None, periph_write, None)
uc.mmio_map(0xE000E000, 0x1000, core_read, None, core_write, None)

sp = int.from_bytes(IMG[0:4], "little")
reset = int.from_bytes(IMG[4:8], "little")
uc.reg_write(UC_ARM_REG_SP, sp)


def run_until(pred, limit=200_000_000):
    """Run until pred(tx) holds, in slices."""
    pc = uc.reg_read(UC_ARM_REG_PC) | 1 if run_until.started else reset
    run_until.started = True
    done = 0
    while not pred(bytes(tx)) and done < limit:
        try:
            uc.emu_start(pc, 0xFFFFFFFF, count=200_000)
        except UcError as e:
            print("EMULATION FAULT at", hex(uc.reg_read(UC_ARM_REG_PC)), e)
            sys.exit(1)
        pc = uc.reg_read(UC_ARM_REG_PC) | 1
        done += 200_000
    if not pred(bytes(tx)):
        print("TIMEOUT; output so far:", bytes(tx[-300:]))
        sys.exit(1)


run_until.started = False

def check(what, ok):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        sys.exit(1)


print("test_dumper_emu: the real dumper image, emulated")
run_until(lambda t: b"ready\r\n" in t)
check("boots and prints its banner", b"XDM1241-OPEN dumper" in bytes(tx))
check("points VTOR at its own table", regs.get(0xE000ED08) == 0x08002800)

tx.clear(); rxq.extend(b"*IDN?\r")
run_until(lambda t: b"\r\n" in t)
check("answers *IDN?", bytes(tx).startswith(b"XDM1241-OPEN,dumper,"))

tx.clear(); rxq.extend(b"bogus\n")
run_until(lambda t: b"\r\n" in t)
check("rejects an unknown command", bytes(tx).strip() == b"ERR unknown command")

tx.clear(); rxq.extend(b"diag:boot?\r")
run_until(lambda t: b"END " in t and t.endswith(b"\r\n"))
check("DIAG:BOOT? is the bootloader, byte-exact",
      rb.parse_dump(bytes(tx).decode()) == bytes(flash[:0x2800]))

tx.clear(); rxq.extend(b"DIAG:FLASH?\r\n")
run_until(lambda t: b"END " in t and t.endswith(b"\r\n"), limit=3_000_000_000)
full = rb.parse_dump(bytes(tx).decode(), length=0x40000)
check("DIAG:FLASH? is all 256 KB, byte-exact", full == bytes(flash))
report = "\n".join(rb.analyse(full, bytes(flash[0x3F800:0x3F800 + 1392])))
check("analysis finds the surviving stock pages", "data from 0x08005000 to 0x0801D000" in report)
check("analysis matches the factory page", "IDENTICAL" in report)
check("writes nothing to flash", bytes(uc.mem_read(FLASH, FLEN)) == bytes(flash))
