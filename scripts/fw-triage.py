#!/usr/bin/env python3
"""
Triage a raw Cortex-M firmware image: confirm the load address from the vector
table, map the interrupt handlers, and pull out the strings that reveal what
the firmware does.

Usage: scripts/fw-triage.py firmware/XDM1041_OS_V2.1.2_20210525.bin [--base 0x08000000]
"""
import argparse, re, struct, sys
from collections import Counter

# Cortex-M system exceptions, in vector-table order after SP and Reset.
SYS_VECTORS = [
    "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault",
    "Reserved", "Reserved", "Reserved", "Reserved", "SVCall", "DebugMon",
    "Reserved", "PendSV", "SysTick",
]
# STM32F10x / GD32F30x external interrupts (position 0 onwards).
IRQ_NAMES = [
    "WWDG", "PVD", "TAMPER", "RTC", "FLASH", "RCC", "EXTI0", "EXTI1",
    "EXTI2", "EXTI3", "EXTI4", "DMA1_Ch1", "DMA1_Ch2", "DMA1_Ch3", "DMA1_Ch4",
    "DMA1_Ch5", "DMA1_Ch6", "DMA1_Ch7", "ADC1_2", "USB_HP_CAN_TX",
    "USB_LP_CAN_RX0", "CAN_RX1", "CAN_SCE", "EXTI9_5", "TIM1_BRK",
    "TIM1_UP", "TIM1_TRG_COM", "TIM1_CC", "TIM2", "TIM3", "TIM4",
    "I2C1_EV", "I2C1_ER", "I2C2_EV", "I2C2_ER", "SPI1", "SPI2",
    "USART1", "USART2", "USART3", "EXTI15_10", "RTCAlarm", "USBWakeup",
    "TIM8_BRK", "TIM8_UP", "TIM8_TRG_COM", "TIM8_CC", "ADC3", "FSMC",
    "SDIO", "TIM5", "SPI3", "UART4", "UART5", "TIM6", "TIM7",
    "DMA2_Ch1", "DMA2_Ch2", "DMA2_Ch3", "DMA2_Ch4_5",
]

SRAM_LO, SRAM_HI = 0x20000000, 0x20030000


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def analyse_vectors(data, base):
    size = len(data)
    sp = u32(data, 0)
    ok_sp = SRAM_LO < sp <= SRAM_HI
    print(f"  initial SP     : 0x{sp:08X}   {'plausible SRAM' if ok_sp else 'NOT SRAM - wrong base?'}")

    # The table ends where the first handler body starts: walking past that
    # point reads code as vectors and produces bogus "suspect" entries.
    cands = []
    for i in range(1, 80):
        if i * 4 + 4 > size:
            break
        v = u32(data, i * 4)
        if v and (v & 1) and 0 <= (v & ~1) - base < size:
            cands.append((v & ~1) - base)
    tbl_end = min(cands) if cands else 4
    n = tbl_end // 4
    print(f"  table length   : 0x{tbl_end:X} bytes = {n} entries ({max(n-16,0)} external IRQs)")

    entries, good, bad = [], 0, 0
    for i in range(1, n):
        v = u32(data, i * 4)
        name = SYS_VECTORS[i - 1] if i - 1 < len(SYS_VECTORS) else \
            IRQ_NAMES[i - 16] if 0 <= i - 16 < len(IRQ_NAMES) else f"IRQ{i-16}"
        if v == 0:
            entries.append((i, name, v, "unused"))
            continue
        off = (v & ~1) - base
        if (v & 1) and 0 <= off < size:
            good += 1
            body = data[off:off + 2]
            kind = "stub-hang" if body == b"\xfe\xe7" else \
                   "stub-return" if body == b"\x70\x47" else "REAL"
            entries.append((i, name, v, kind))
        else:
            bad += 1
            entries.append((i, name, v, "SUSPECT"))
    print(f"  handlers       : {good} valid, {bad} suspect")
    return sp, entries, good, bad


def find_strings(data, base, minlen=4):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, data):
        out.append((base + m.start(), m.group().decode("ascii")))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--base", default="0x08002800")
    ap.add_argument("--strings", type=int, default=40, help="how many SCPI-ish strings to print")
    a = ap.parse_args()
    base = int(a.base, 16)
    data = open(a.image, "rb").read()

    print(f"\n=== image ===")
    print(f"  file           : {a.image}")
    print(f"  size           : {len(data)} bytes ({len(data)/1024:.1f} KB)")
    print(f"  assumed base   : 0x{base:08X}")
    print(f"  occupies       : 0x{base:08X}-0x{base+len(data):08X}")
    print(f"  of 256 KB flash: {100*len(data)/0x40000:.1f}% used")

    print(f"\n=== vector table ===")
    sp, entries, good, bad = analyse_vectors(data, base)
    verdict = "CONFIRMED" if good > 10 and bad == 0 else "QUESTIONABLE"
    print(f"  load address   : {verdict}")

    print(f"\n=== peripherals with a REAL handler (not a stub) ===")
    for i, name, v, st in entries:
        if st == "REAL" and name != "Reserved":
            print(f"  {name:<16} -> 0x{v & ~1:08X}")

    strs = find_strings(data, base)
    print(f"\n=== strings: {len(strs)} found ===")

    # SCPI commands: contain ':' or start with '*', mostly uppercase.
    scpi = [(ad, s) for ad, s in strs
            if (s.startswith("*") or ":" in s)
            and re.fullmatch(r"[A-Za-z:*?\[\]|,0-9 _\-]+", s)
            and any(c.isupper() for c in s)]
    print(f"\n--- SCPI-like ({len(scpi)}) ---")
    for ad, s in scpi[: a.strings]:
        print(f"  0x{ad:08X}  {s}")

    # Version / build identifiers.
    ver = [(ad, s) for ad, s in strs
           if re.search(r"(V\d+\.\d+|\d{4}-\d\d-\d\d|\d{8}|OWON|XDM)", s, re.I)]
    print(f"\n--- version / identity ({len(ver)}) ---")
    for ad, s in ver[:20]:
        print(f"  0x{ad:08X}  {s}")

    # Measurement vocabulary.
    units = [(ad, s) for ad, s in strs
             if re.fullmatch(r"[A-Za-zΩµ°%/]{1,12}", s)
             and re.search(r"(V|A|Ohm|Hz|F|DC|AC|RTD|TEMP|CAP|DIOD|CONT|FREQ)", s, re.I)]
    print(f"\n--- units / modes ({len(units)}) ---")
    for ad, s in units[:30]:
        print(f"  0x{ad:08X}  {s}")

    print()


if __name__ == "__main__":
    sys.exit(main())
