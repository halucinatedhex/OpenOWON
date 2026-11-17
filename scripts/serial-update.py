#!/usr/bin/env python3
"""
Install firmware into the app slot through OWON's own bootloader, over the
meter's USB serial port. No probe, no opening the case.

    scripts/serial-update.py PORT IMAGE --stock OWON_UPDATE.bin
    scripts/serial-update.py PORT OWON_UPDATE.bin --restore     # back to stock

Run it, then switch the meter on: the bootloader only listens for a moment
after power-on, and only if the host is already sending `start`.

The protocol (OWON's XDM1041 updater, as captured by Petteri Aimonen;
docs/community-research.md), 115200 8N1, every step acknowledged by `7`:
    `start`, repeated during power-on    -> 7
    area byte 0x05 (application)         -> 7
    pack count, one byte                 -> 7
    each 10,240-byte pack                -> 7
    0x14                                 exit, the new app boots

What it will NEVER send: area 0x02. That area rewrites the factory record,
calibration included (one owner's failed update corrupted theirs, #194).
There is no option for it; `send_area` refuses anything but 0x05.

What it checks before touching the meter:
  - the image is linked for 0x08002800: SP in SRAM, reset handler a Thumb
    address inside the image, the other vectors 0 or inside it;
  - its packs (the last one padded with 0xFF, erased flash) end before our
    data log at 0x08030000, or, for a stock image (--restore), before
    stock's own log at 0x0803D800;
  - a stock image for the way back is in hand (--stock), unless the image
    being installed is that stock image (--restore);
  - a person is at a terminal and types INSTALL.

Readout protection does not stop this: the bootloader runs on the chip.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

BAUD = 115200
APP_BASE = 0x08002800
OUR_LIMIT = 0x08030000        # our data log starts here (docs/boot.md)
STOCK_LIMIT = 0x0803D800      # stock log / prefs / factory record: never reached
SRAM_BASE, SRAM_END = 0x20000000, 0x2000C000   # GD32F303CC: 48 KB
PACK = 10240
AREA_APP = 0x05
ACK = b"7"
EXIT = b"\x14"
ENTRY = b"start"
DUMPER_BANNER = b"XDM1241-OPEN dumper"
OURS = (b"OpenOWON", b"XDM1241-OPEN")   # in every *IDN? of ours (older builds: the second); never in stock's


def ours(reply):
    return any(n in reply for n in OURS)


class UpdateError(Exception):
    pass


# Images known to be wrong for this meter (GD32F303 board, v3.x/v4.x firmware).
WRONG_FOR_THIS_METER = {
    "16e2706a0f3160d5efd742969f1defe2131c2886c779f17a996111568da67224":
        "OWON v2.1.2, built for the older STM32F103 board (its 43-entry IRQ "
        "table proves it); OWON says v3.x boards have a different panel",
}


def refuse_known_wrong(img):
    import hashlib
    why = WRONG_FOR_THIS_METER.get(hashlib.sha256(img).hexdigest())
    if why:
        raise UpdateError(f"this image is {why}. Not a way back for this meter: "
                          "ask OWON for the update file matching yours (V4.x)")


# ---- image checks -------------------------------------------------------------

def check_image(img, limit):
    """Return the pack count, or raise UpdateError saying what is wrong."""
    if len(img) < 64:
        raise UpdateError(f"image is {len(img)} bytes: too small to be firmware")
    end = APP_BASE + len(img)
    sp, reset = struct.unpack_from("<II", img, 0)
    if not (SRAM_BASE < sp <= SRAM_END and sp % 4 == 0):
        raise UpdateError(f"initial SP 0x{sp:08X} is not in SRAM: not a firmware image")
    if not (reset & 1 and APP_BASE <= (reset & ~1) < end):
        raise UpdateError(
            f"reset handler 0x{reset:08X} is not a Thumb address inside the image "
            f"(0x{APP_BASE:08X}-0x{end:08X}): not linked for the app slot")
    # Exceptions 2..15 (NMI .. SysTick): 0 (reserved/unused) or inside the image.
    for i in range(2, 16):
        v = struct.unpack_from("<I", img, 4 * i)[0]
        if v and not (v & 1 and APP_BASE <= (v & ~1) < end):
            raise UpdateError(f"vector {i} = 0x{v:08X} points outside the image: "
                              f"not linked at 0x{APP_BASE:08X}")
    packs = -(-len(img) // PACK)
    reach = APP_BASE + packs * PACK
    if reach > limit:
        raise UpdateError(
            f"{packs} packs would write up to 0x{reach:08X}, past 0x{limit:08X}; "
            f"the image is {len(img)} bytes, at most {(limit - APP_BASE) // PACK * PACK}")
    if packs > 127:
        raise UpdateError("more than 127 packs: the count is one signed byte")
    return packs


def packs_of(img):
    """Split into 10,240-byte packs, the last padded with 0xFF (erased flash)."""
    n = -(-len(img) // PACK)
    padded = img + b"\xff" * (n * PACK - len(img))
    return [padded[i * PACK:(i + 1) * PACK] for i in range(n)]


# ---- the protocol -------------------------------------------------------------

class Link:
    """The bootloader conversation over a pyserial-like port (write, read)."""

    def __init__(self, port, clock=time.monotonic, sleep=time.sleep, say=print):
        self.port, self.clock, self.sleep, self.say = port, clock, sleep, say

    def drain(self, settle=0.3):
        """Discard input for `settle` seconds (late acks to repeated `start`)."""
        end = self.clock() + settle
        while self.clock() < end:
            self.port.read(256)
            self.sleep(0.05)

    def wait_ack(self, what, timeout):
        end = self.clock() + timeout
        while self.clock() < end:
            if ACK in self.port.read(16):
                return
            self.sleep(0.05)
        raise UpdateError(f"no acknowledgement to {what} within {timeout:.0f} s")

    def request_reset(self, timeout=3.0):
        """Ask OUR firmware to reset the chip, so the bootloader runs again
        with nobody at the power button. Sends `*IDN?` first, and `DIAG:RESET`
        only if the answer is ours: nothing else ever goes to stock.
        Returns True if the reset was requested."""
        self.port.read(65536)
        self.port.write(b"*IDN?\r\n")
        got, end = b"", self.clock() + timeout
        while self.clock() < end and not ours(got):
            got += self.port.read(4096)
            self.sleep(0.05)
        if not ours(got):
            return False
        self.port.write(b"DIAG:RESET\r\n")
        got, end = b"", self.clock() + timeout
        while self.clock() < end and b"OK\r\n" not in got and b"ERR" not in got:
            got += self.port.read(4096)
            self.sleep(0.05)
        if b"OK\r\n" not in got:
            return False        # an older build of ours, without DIAG:RESET
        self.say("Our firmware answered: resetting it into the bootloader.")
        return True

    def enter(self, timeout=120.0, prompt=True):
        """Send `start` every 100 ms until the bootloader answers."""
        if prompt:
            self.say("Switch the meter ON now (or off and on again).")
        end = self.clock() + timeout
        while self.clock() < end:
            self.port.write(ENTRY)
            self.sleep(0.1)
            if ACK in self.port.read(1024):
                self.say("Bootloader answered.")
                self.drain()
                return
        raise UpdateError(f"the bootloader did not answer within {timeout:.0f} s: "
                          "was the meter switched on while this was waiting?")

    def send_area(self, area):
        if area != AREA_APP:
            # Area 2 is the factory record. Nothing in this project writes it.
            raise UpdateError(f"refusing area 0x{area:02X}: only 0x05 (application)")
        self.port.write(bytes([area]))
        self.wait_ack("the area byte", 30.0)

    def install(self, img, packs):
        self.send_area(AREA_APP)
        self.port.write(bytes([len(packs)]))
        self.say(f"Erasing, then writing {len(packs)} packs...")
        t0 = self.clock()
        self.wait_ack("the pack count", 30.0)
        # The erase happens here. Its duration hints at how much was erased
        # (GD32F303 page erase is tens of ms per 2 KB page): record it.
        self.erase_seconds = self.clock() - t0
        self.say(f"  pack count acknowledged after {self.erase_seconds:.2f} s (the erase)")
        for i, p in enumerate(packs, 1):
            self.port.write(p)
            self.wait_ack(f"pack {i}/{len(packs)}", 10.0)
            self.say(f"  pack {i}/{len(packs)} written")
        self.port.write(EXIT)
        self.say("Done: the bootloader is starting the new firmware.")


def stock_gone_evidence(root=None):
    """The newest dumper read-out whose stock app area is all erased, or None."""
    root = root or ROOT
    for f in sorted((root / "backups").glob("*-flash/flash.bin"), reverse=True):
        d = f.read_bytes()
        if len(d) == 0x40000 and d[0x5000:0x3D000].count(0xFF) == 0x38000:
            return f
    return None


def calibration_saved(root=None):
    """The newest saved factory record (scripts/serial-dump.py), or None."""
    root = root or ROOT
    for f in sorted((root / "backups").glob("serial-*/factory.bin"), reverse=True):
        if f.stat().st_size >= 0x400:
            return f
    return None


# ---- main ---------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="the meter's serial port, e.g. /dev/cu.usbserial-1130")
    ap.add_argument("image", type=Path, help="raw image linked at 0x08002800")
    ap.add_argument("--stock", type=Path,
                    help="OWON's update file for this meter: your way back to stock")
    ap.add_argument("--restore", action="store_true",
                    help="IMAGE is OWON's stock image (allows up to 0x0803D800)")
    ap.add_argument("--dumper", action="store_true",
                    help="IMAGE is our one-pack flash dumper (src/dumper): the only "
                         "image allowed without --stock. Stock is NOT restorable after")
    ap.add_argument("--no-way-back", action="store_true",
                    help="instead of --stock: install without OWON's update file, "
                         "accepting that stock can't be put back (OWON doesn't publish "
                         "it). Needs your calibration saved first (scripts/serial-dump.py) "
                         "and the phrase NO WAY BACK")
    ap.add_argument("--stock-gone", action="store_true",
                    help="instead of --stock, for updates: stock is already gone from "
                         "this meter. Accepted with --reset (the meter must answer "
                         "*IDN? as this firmware), or a dumper read-out in "
                         "backups/*-flash/ showing the stock app area erased")
    ap.add_argument("--confirm", metavar="PHRASE",
                    help="give the confirmation phrase up front instead of typing it, "
                         "for running from a session without a terminal while you are "
                         "at the meter to switch it on")
    ap.add_argument("--reset", action="store_true",
                    help="if our firmware is running, reset it into the bootloader "
                         "over serial (DIAG:RESET) instead of asking for a power cycle")
    ap.add_argument("--check", action="store_true",
                    help="check the image(s) and stop; does not open the port")
    a = ap.parse_args(argv)
    must_be_ours = False

    try:
        img = a.image.read_bytes()
        packs = check_image(img, STOCK_LIMIT if a.restore else OUR_LIMIT)
        if a.restore:
            refuse_known_wrong(img)
        print(f"{a.image}: {len(img)} bytes, {packs} packs, "
              f"0x{APP_BASE:08X}-0x{APP_BASE + packs * PACK:08X}: OK")
        if a.dumper:
            if a.restore:
                raise UpdateError("--dumper and --restore don't mix")
            if packs != 1 or DUMPER_BANNER not in img:
                raise UpdateError("--dumper is only for our one-pack dumper image "
                                  "(.pio/build/xdm1241-dumper/firmware.bin)")
            print("dumper image: one pack, 0x08002800-0x08004FFF only")
        elif not a.restore:
            if a.no_way_back and not a.stock:
                cal = calibration_saved()
                if cal is None:
                    raise UpdateError(
                        "--no-way-back needs your calibration saved first: run "
                        "scripts/serial-dump.py PORT (backups/serial-*/factory.bin)")
                print(f"calibration saved: {cal.relative_to(ROOT)}")
                print("no way back: OWON's update file for this meter is not available")
            elif a.stock_gone and not a.stock:
                dump = stock_gone_evidence()
                if dump is not None:
                    print(f"no way back needed: stock is already gone ({dump.relative_to(ROOT)})")
                elif a.reset:
                    must_be_ours = True
                    print("no way back needed if the meter answers as this firmware "
                          "(checked over serial before anything is written)")
                else:
                    raise UpdateError(
                        "--stock-gone needs --reset (the meter must answer *IDN? as this "
                        "firmware) or a dumper read-out showing stock erased")
            elif not a.stock:
                raise UpdateError(
                    "no way back given: --stock OWON_FILE if you have OWON's update "
                    "file; --no-way-back for a first install without it (stock can't "
                    "be put back: docs/install.md); --stock-gone --reset to update")
            if a.stock:
                stock = a.stock.read_bytes()
                check_image(stock, STOCK_LIMIT)
                refuse_known_wrong(stock)
                print(f"{a.stock}: stock image for the way back: OK")
    except (OSError, UpdateError) as e:
        print(f"ERR {e}", file=sys.stderr)
        return 2
    if a.check:
        return 0

    phrase = "NO WAY BACK" if a.dumper or a.no_way_back else "INSTALL"
    if a.confirm is not None:
        if a.confirm.strip() != phrase:
            print(f"ERR --confirm must be exactly {phrase!r}", file=sys.stderr)
            return 1
    elif not sys.stdin.isatty():
        print("ERR needs you at a terminal: you switch the meter on and confirm "
              f"(run it as: ! {sys.argv[0]} ...)", file=sys.stderr)
        return 2
    print("\nThis rewrites the meter's application. Calibration and the bootloader "
          "are not touched.")
    if a.no_way_back:
        print("WITHOUT OWON's update file, STOCK FIRMWARE CANNOT BE PUT BACK after this:\n"
              "the bootloader erases the whole application before writing.")
    if a.dumper:
        print("WITHOUT OWON's update file, STOCK FIRMWARE CANNOT BE PUT BACK after this.\n"
              "The first 10 KB of the stock application are overwritten for good;\n"
              "the rest survives only if the bootloader erases just that one pack.")
    if a.confirm is None and input(f"Type {phrase} to continue: ").strip() != phrase:
        print("Not installing.")
        return 1

    import serial  # pyserial; only needed past this point
    with serial.Serial(a.port, BAUD, timeout=0.05) as port:
        port.reset_input_buffer()
        link = Link(port)
        try:
            reset = a.reset and link.request_reset()
            if must_be_ours and not reset:
                print("ERR the meter didn't answer as this firmware, so stock may still be "
                      "on it: nothing written. For a first install use --no-way-back.",
                      file=sys.stderr)
                return 2
            if a.reset and not reset:
                print("Our firmware didn't answer *IDN?; falling back to a power cycle.")
            link.enter(prompt=not reset)
            link.install(img, packs_of(img))
            print(f"erase took {link.erase_seconds:.2f} s for {packs} pack(s): "
                  "note it in docs/install.md")
        except UpdateError as e:
            print(f"ERR {e}", file=sys.stderr)
            print("The bootloader is still there: switch the meter off, then rerun "
                  "this and switch it on again.", file=sys.stderr)
            return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
