#!/usr/bin/env python3
"""
Pull the factory record (calibration + identity) from a STOCK meter over its
serial port. No probe, no firmware change.

    scripts/serial-dump.py /dev/cu.usbserial-XXXX [--baud 115200]

What it sends: only `INT:TXT`, falling back to `SYST:TXT`. TXT is the stock
command whose handler only calls stock_dump_settings_uart (dispatcher case 3,
keyword table at RAM 0x200017A8, index 3 = TXT). The service keywords hang
off the INTernal root: Petteri Aimonen's XDM2041 tools read calibration
with `INT:TXT` and skip the same 24-byte header + identity. See
docs/settings-dump.md.

What it will NEVER send, even with --cmd: the service commands that write
flash or reset the meter. DOWNLOAD writes a marker to the preferences page
and resets (stock's firmware-update entry); VERSION <arg> rewrites the
factory page; WTXT, MODEL, RESTart, SOFTWARERESET, PROduce, HYREG and
KEYRANDOM are unknown or writing. See docs/scpi.md.

The stream: 4-byte header 00 08 00 00, 20 identity bytes (factory +0x28),
then 2048 bytes: the 1392-byte factory record and 0xFF padding.

Writes backups/serial-<stamp>/{raw.bin, factory.bin, report.txt}. Read-only.
"""
import argparse
import re
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = b"\x00\x08\x00\x00"
ID_LEN, PAYLOAD = 20, 2048
RECORD = 0x570
TOTAL = len(HEADER) + ID_LEN + PAYLOAD
COEF, ZERO, CJG = 0x1F8, 0x3B0, 0x56C

SAFE_COMMANDS = ("INT:TXT", "SYST:TXT")
_TXT_FORMS = {"INT:TXT", "INTERNAL:TXT", "SYST:TXT", "SYSTEM:TXT"}
# START: `INT:START` runs the factory auto-calibration and has wiped DC
# calibration on a user's meter (EEVblog XDM1041 thread #265, 2026-03).
# Every other INT command is refused: INT:DOWNLOAD enters the bootloader,
# INT:VERSION/MODEL write flash, the rest are unrecovered.
FORBIDDEN = re.compile(r"(DOWNLOAD|WTXT|VERSION|MODEL|RESTART|SOFTWARERESET|PRODUCE|PROD\b|"
                       r"HYREG|KEYRANDOM|JPG|BMP|UARTCONTROL|PWM|CHANNUM|START|\bINT(ERNAL)?:)", re.I)

# Mirror of lib/measure meas_cal_validate(): functions that own coef rows,
# ranges in use, and stock's clamp limits (0x08005700).
OWNERS = {0: 6, 1: 4, 2: 6, 3: 6, 4: 6, 8: 7}       # function: ranges checked


def clamp(fn, rng):
    if fn == 8:
        return (600000, 1400000) if rng >= 4 else None
    return (800000, 1300000 if fn in (2, 3) else 1200000)


def parse_record(rec):
    i32 = lambda o: struct.unpack_from("<i", rec, o)[0]
    coef = [[i32(COEF + (f * 10 + r) * 4) for r in range(10)] for f in range(11)]
    zero = [[i32(ZERO + (f * 10 + r) * 4) for r in range(10)] for f in range(11)]
    return coef, zero, i32(CJG)


def validate(coef):
    bad = []
    for fn, n in OWNERS.items():
        for r in range(n):
            lo, hi = clamp(fn, r) or (600000, 1400000)
            if not lo <= coef[fn][r] <= hi:
                bad.append((fn, r, coef[fn][r]))
    return bad


def split_stream(raw):
    """Find the frame in what was received. Returns (identity, record) or raises."""
    i = raw.find(HEADER)
    if i < 0:
        raise ValueError("no 00 08 00 00 header in the reply")
    body = raw[i + len(HEADER):]
    if len(body) < ID_LEN + RECORD:
        raise ValueError(f"short frame: {len(body)} bytes after the header, need {ID_LEN + RECORD}")
    return body[:ID_LEN], body[ID_LEN:ID_LEN + RECORD]


def report(identity, rec):
    coef, zero, cjg = parse_record(rec)
    bad = validate(coef)
    names = ["DCV", "ACV", "DCI", "ACI", "OHM", "OHM4", "CONT", "DIODE", "CAP", "FREQ", "TEMP"]
    out = [f"identity block: {identity!r}",
           f"cold-junction G raw: {cjg} ({cjg / 1e4:.4f})",
           f"implausible coef slots: {len(bad)}  (0 means the layout in docs/calibration.md holds)"]
    for fn, n in OWNERS.items():
        out.append(f"  {names[fn]:<5} coef " + " ".join(f"{coef[fn][r]:>8}" for r in range(n)))
        out.append(f"  {'':<5} zero " + " ".join(f"{zero[fn][r]:>8}" for r in range(n)))
    for fn, r, v in bad:
        out.append(f"  OUT OF RANGE: {names[fn]} range {r} coef {v}")
    return "\n".join(out), bad


def check_command(cmd):
    if cmd.strip().lstrip(":").upper() in _TXT_FORMS:
        return                                   # the read-only dump, exactly
    if FORBIDDEN.search(cmd):
        raise SystemExit(f"refusing to send {cmd!r}: that service command writes flash or resets the meter")
    if "?" in cmd:
        raise SystemExit("the dump runs only without '?'")


def dump(port, cmds, timeout=3.0):
    """port: object with write(bytes), read(n) -> bytes (pyserial-like)."""
    for cmd in cmds:
        check_command(cmd)
        port.write(cmd.encode() + b"\n")
        raw, t0 = b"", time.monotonic()
        while time.monotonic() - t0 < timeout and len(raw) < TOTAL + 64:
            chunk = port.read(4096)
            if chunk:
                raw += chunk
                t0 = time.monotonic()            # extend while data flows
        if HEADER in raw:
            return cmd, raw
    return None, b""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", action="append", help="override the command (forbidden ones are refused)")
    a = ap.parse_args()
    cmds = a.cmd or list(SAFE_COMMANDS)
    for c in cmds:
        check_command(c)

    import serial                                 # pyserial
    with serial.Serial(a.port, a.baud, timeout=0.2) as port:
        port.reset_input_buffer()
        cmd, raw = dump(port, cmds)
    if not cmd:
        sys.exit("no dump frame received. Check the port and baud rate (stock's serial menu sets it).")

    identity, rec = split_stream(raw)
    out = ROOT / "backups" / time.strftime("serial-%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    (out / "raw.bin").write_bytes(raw)
    (out / "factory.bin").write_bytes(rec)
    text, bad = report(identity, rec)
    (out / "report.txt").write_text(f"command: {cmd}\nbaud: {a.baud}\n{text}\n")
    print(text)
    print(f"\nsaved {out}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
