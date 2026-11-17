#!/usr/bin/env python3
"""
Read OWON's bootloader (0x08000000-0x080027FF) off the meter over serial,
from OUR firmware (the xdm1241-serial build, or the dumper), which answers
`DIAG:BOOT?`. With --all (the dumper only), all 256 KB via `DIAG:FLASH?`.

    scripts/read-boot.py /dev/cu.usbserial-XXXX
    scripts/read-boot.py /dev/cu.usbserial-XXXX --all

Readout protection blocks debuggers, not code on the chip, so our firmware
can read the bootloader where SWD cannot (docs/readout-protection.md).

Safety: it asks `*IDN?` first and refuses unless our firmware answers, so
nothing but `*IDN?` ever reaches stock. Read-only on both sides.

Checks: every line's address in sequence, the trailer's base, length and
CRC-32, and two complete reads byte-identical. Writes
backups/<stamp>-bootloader/{bootloader.bin, manifest.txt}.
"""
import argparse
import hashlib
import re
import sys
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOT_BASE, BOOT_LEN = 0x08000000, 0x2800
FLASH_LEN = 0x40000
APP_BASE, DUMPER_END = 0x08002800, 0x08005000   # the one pack the dumper took
FACTORY = 0x0803F800
PAGE = 0x800
OURS = (b"OpenOWON", b"XDM1241-OPEN")   # in every *IDN? of ours (older builds: the second); never in stock's
LINE = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]{2,64})$")
END = re.compile(r"^END ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})$")


class DumpError(Exception):
    pass


def parse_dump(text, base=BOOT_BASE, length=BOOT_LEN):
    """Parse flashdump text (lib/console/flashdump.h). Lines that are not part
    of the dump (log output) are ignored. Returns the bytes, or raises."""
    data, expect, trailer = bytearray(), base, None
    for raw in text.splitlines():
        s = raw.strip()
        m = LINE.match(s)
        if m:
            addr = int(m.group(1), 16)
            if addr != expect:
                raise DumpError(f"line for 0x{addr:08X}, expected 0x{expect:08X}")
            chunk = bytes.fromhex(m.group(2))
            data += chunk
            expect += len(chunk)
            continue
        m = END.match(s)
        if m:
            trailer = tuple(int(g, 16) for g in m.groups())
            break
    if trailer is None:
        raise DumpError("no END line: the dump was cut short")
    t_base, t_len, t_crc = trailer
    if (t_base, t_len) != (base, length):
        raise DumpError(f"trailer says 0x{t_base:08X}+0x{t_len:X}, expected "
                        f"0x{base:08X}+0x{length:X}")
    if len(data) != length:
        raise DumpError(f"got {len(data)} bytes, expected {length}")
    if zlib.crc32(bytes(data)) != t_crc:
        raise DumpError(f"CRC mismatch: data 0x{zlib.crc32(bytes(data)):08X}, "
                        f"trailer 0x{t_crc:08X}")
    return bytes(data)


def ask(port, cmd, until, timeout, clock=time.monotonic, sleep=time.sleep):
    """Send a command line; collect replies until `until` appears or timeout."""
    port.read(65536)                      # drop heartbeat lines already queued
    port.write(cmd + b"\r\n")
    got, end = b"", clock() + timeout
    while clock() < end:
        chunk = port.read(4096)
        if chunk:
            got += chunk
            if until in got:
                # the END line (or IDN reply) is complete once its \n arrives
                tail = got[got.rfind(until):]
                if b"\n" in tail:
                    return got
        else:
            sleep(0.02)
    return got


def read_boot(port, clock=time.monotonic, sleep=time.sleep, everything=False):
    idn = ask(port, b"*IDN?", b"\n", 3.0, clock, sleep)
    if not any(n in idn for n in OURS):
        raise DumpError("the meter did not identify as our firmware "
                        f"(*IDN? -> {idn.strip()[:60]!r}). Install the xdm1241-serial "
                        "build first; this never sends DIAG:BOOT? to stock.")
    cmd, length, wait = ((b"DIAG:FLASH?", FLASH_LEN, 180.0) if everything
                         else (b"DIAG:BOOT?", BOOT_LEN, 30.0))
    reads = []
    for n in (1, 2):
        text = ask(port, cmd, b"END ", wait, clock, sleep)
        reads.append(parse_dump(text.decode("ascii", "replace"), length=length))
    if reads[0] != reads[1]:
        raise DumpError("the two reads differ: not trusted")
    return reads[0], idn.strip().decode("ascii", "replace")


def analyse(flash, serial_factory=None):
    """What a full dump says: how much of stock survived, and whether the
    calibration page matches the independent serial dump. Returns lines."""
    def at(addr, n):
        return flash[addr - BOOT_BASE: addr - BOOT_BASE + n]
    lines = []
    sp, reset = int.from_bytes(at(BOOT_BASE, 4), "little"), int.from_bytes(at(BOOT_BASE + 4, 4), "little")
    lines.append(f"bootloader  : SP 0x{sp:08X}, reset 0x{reset:08X}")
    # Stock app remains: 0x08005000 up to the stock records.
    erased = kept = 0
    first = last = None
    for a in range(DUMPER_END, 0x0803D800, PAGE):
        page = at(a, PAGE)
        if page.count(0xFF) == PAGE:
            erased += 1
        else:
            kept += 1
            first = a if first is None else first
            last = a + PAGE
    lines.append(f"0x{DUMPER_END:08X}-0x0803D7FF: {kept} pages hold data, {erased} erased")
    if first is not None:
        lines.append(f"  data from 0x{first:08X} to 0x{last:08X}: "
                     f"the surviving stock application (and anything after it)")
    else:
        lines.append("  all erased: the bootloader cleared the whole app area")
    dumper = at(APP_BASE, DUMPER_END - APP_BASE)
    lines.append(f"0x{APP_BASE:08X}-0x{DUMPER_END - 1:08X}: the dumper"
                 f"{'' if b'XDM1241-OPEN dumper' in dumper else ' (NOT FOUND: unexpected)'}")
    if serial_factory is not None:
        mine = at(FACTORY, len(serial_factory))
        lines.append("factory record vs serial dump: " +
                     ("IDENTICAL (calibration intact, and the dump is trustworthy)"
                      if mine == serial_factory else "DIFFERENT: investigate before anything else"))
    return lines


def latest_serial_factory():
    dumps = sorted((ROOT / "backups").glob("serial-*/factory.bin"))
    return dumps[-1] if dumps else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--all", action="store_true",
                    help="all 256 KB (DIAG:FLASH?, the dumper image); about 2 minutes")
    a = ap.parse_args(argv)

    import serial
    with serial.Serial(a.port, a.baud, timeout=0.05) as port:
        try:
            data, idn = read_boot(port, everything=a.all)
        except DumpError as e:
            print(f"ERR {e}", file=sys.stderr)
            return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = ROOT / "backups" / (stamp + ("-flash" if a.all else "-bootloader"))
    out.mkdir(parents=True)
    if a.all:
        (out / "flash.bin").write_bytes(data)
        sf = latest_serial_factory()
        report = analyse(data, sf.read_bytes() if sf else None)
        if sf:
            report.append(f"  (compared with {sf.relative_to(ROOT)})")
        (out / "report.txt").write_text("\n".join(report) + "\n")
        print("\n".join(report))
        data = data[:BOOT_LEN]
    (out / "bootloader.bin").write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()
    sp, reset = int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:8], "little")
    (out / "manifest.txt").write_text(
        "OWON bootloader, read by our firmware over serial (DIAG:BOOT?)\n"
        f"date        : {datetime.now(timezone.utc).isoformat(timespec='seconds')}\n"
        f"firmware    : {idn}\n"
        f"origin      : 0x{BOOT_BASE:08X}\n"
        f"size        : {len(data)} bytes\n"
        f"sha256      : {sha}\n"
        f"two-pass    : identical, CRC-32 checked\n"
        f"vectors     : SP 0x{sp:08X}, reset 0x{reset:08X}\n")
    print(f"saved {out}/bootloader.bin  sha256 {sha}")
    print(f"vectors: SP 0x{sp:08X}, reset 0x{reset:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
