#!/usr/bin/env python3
"""
Check an RTT boot log from our firmware (src/main.c) and say plainly what
worked. Used by scripts/hil-smoke.sh; also handy by hand:

    scripts/check-boot-log.py build/rtt.log

Exit 0 when every required check passes. Warnings (e.g. the PLL did not lock
and the core runs at 8 MHz) do not fail the run but are printed.
"""
import re
import sys
from pathlib import Path


def check(text):
    """Returns (passes, failures, warnings), each a list of strings."""
    ok, bad, warn = [], [], []

    m = re.search(r"XDM1241 firmware (\S+)", text)
    (ok if m else bad).append(f"banner{': ' + m.group(1) if m else ' missing - RTT not found, or the app never ran'}")

    m = re.search(r"clock (\d+) Hz", text)
    if not m:
        bad.append("no clock line")
    elif int(m.group(1)) == 72000000:
        ok.append("clock 72 MHz (PLL from IRC8M locked)")
    elif int(m.group(1)) == 8000000:
        warn.append("clock 8 MHz: the PLL did not lock; running on IRC8M")
    else:
        bad.append(f"unexpected clock {m.group(1)} Hz")

    m = re.search(r"vector table 0x([0-9a-fA-F]+)", text)
    if not m:
        bad.append("no vector table line")
    elif int(m.group(1), 16) == 0x08002800:
        ok.append("VTOR = 0x08002800: the bootloader handed over and our table is live")
    else:
        bad.append(f"VTOR = 0x{m.group(1)}, expected 0x08002800")

    if re.search(r"calibration: factory record OK", text):
        ok.append("factory calibration record found and plausible")
    elif m := re.search(r"calibration: (\d+) slots implausible", text):
        warn.append(f"calibration: {m.group(1)} implausible slots - layout differs on v3? check the dump")
    else:
        bad.append("no calibration line")

    m = re.search(r"pins: panel (\w+), HY3131 (\w+)", text)
    if m:
        ok.append(f"pins: panel {m.group(1)}, HY3131 {m.group(2)}")

    beats = [(int(t), int(h)) for t, h in re.findall(r"t=(\d+)s heartbeat=(\d+)", text)]
    if len(beats) < 3:
        bad.append(f"only {len(beats)} heartbeat line(s): the main loop or SysTick is not running")
    else:
        dt = [b[0] - a[0] for a, b in zip(beats, beats[1:])]
        dh = [b[1] - a[1] for a, b in zip(beats, beats[1:])]
        if all(d == 1 for d in dt) and all(d == 1 for d in dh):
            ok.append(f"heartbeat steady at 1 Hz ({len(beats)} beats): SysTick timing correct")
        else:
            bad.append(f"heartbeat irregular: dt={dt} dh={dh} (SysTick misconfigured?)")
    return ok, bad, warn


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ok, bad, warn = check(Path(sys.argv[1]).read_text(errors="replace"))
    for s in ok:
        print(f"  ok    {s}")
    for s in warn:
        print(f"  WARN  {s}")
    for s in bad:
        print(f"  FAIL  {s}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
