#!/usr/bin/env python3
"""
Guided bench validation against reference parts (docs/test-plan.md).

    scripts/bench-check.py <port> <check> [--out results.csv] [--readings N]
    scripts/bench-check.py --list

Runs our firmware's application build (xdm1241-ui) over SCPI. For each
step it tells you what to connect, asks for the part's value from your
reference meter or its marking, takes N readings with MEAS?, and records
the mean, spread and error against the reference, with a PASS/CHECK
verdict at the step's provisional tolerance. Results are appended to a
CSV (default results/bench-<date>.csv) so sessions build up a record.

Checks: dcv, ohm, cont, diode, temp, cap, acv, freq, dci, beep, scpi.
Functions the meter still simulates (docs/afe-table.md) are run anyway
and marked SIM in the CSV: their numbers mean nothing yet.

Safety: it only sends commands from a fixed allow-list (configuration,
MEAS?, beeper settings, identity, errors), and refuses to run unless
*IDN? says OpenOWON (or XDM1241-OPEN, older builds). It never sends anything that writes flash.
Read docs/test-plan.md first: some steps need a current limit or a
low-voltage source, and they say so before asking you to connect.
"""
import argparse
import csv
import datetime as dt
import math
import re
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ALLOWED = re.compile(
    r"^(\*IDN\?|SYST:ERR\?|MEAS[12]?\?|FUNC2?\?|RANGE2?\?|AUTO( ON| OFF)?\??|"
    r"CONF:[A-Z:]+( [A-Z0-9.+\-E]+)?|RANGE [0-9]+|RATE [SMF]|"
    r"CONT:THRE [0-9]+|TEMP:RTD:(TYP|UNIT) [A-Z0-9]+|TEMP:TC:(EMF|CJUN)\?|"
    r"SYST:BEEP:(FREQ [0-9]+|VOL [0-9]+|STYL [A-Z]+|PLAY( [A-Z]+)?)|SYST:BEEP( ON| OFF)?)$")

CONSOLE = re.compile(r"^t=\d+s ")

# Functions on the real front end in xdm1241-ui (lib/hal/afe_table.h). Others: SIM.
REAL = {"dcv", "ohm", "cont", "diode", "cap", "acv", "freq", "dci"}

SI = {"p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3, "": 1.0, "k": 1e3, "K": 1e3,
      "M": 1e6, "G": 1e9}


def parse_si(text):
    """'1.497k' -> 1497.0, '10u' -> 1e-5, '4.7 MΩ' -> 4.7e6. None if blank."""
    t = text.strip().replace("Ω", "").replace("ohm", "").replace("F", "").replace("V", "")
    t = t.replace("Hz", "").replace("A", "").replace("C", "").strip()
    if not t:
        return None
    m = re.fullmatch(r"([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\s*([pnuµmkKMG]?)", t)
    if not m:
        raise ValueError(f"can't read {text!r}: give a number, optionally with p n u m k M G")
    return float(m.group(1)) * SI[m.group(2)]


# Each step: (prompt, SCPI set-up, reference hint, tolerance as (fraction, absolute)).
# Tolerances are provisional: generous enough to catch a wrong range or
# table row, not a datasheet acceptance test (that is docs/test-plan.md).
PLANS = {
    "dcv": [
        ("Short the leads (V and COM together)", ["CONF:VOLT:DC", "AUTO ON"], "0", (0, 50e-6)),
        ("A 1.5 V cell (AA/AAA)", ["CONF:VOLT:DC", "AUTO ON"], "your reference meter", (0.001, 200e-6)),
        ("A 9 V battery", ["CONF:VOLT:DC", "AUTO ON"], "your reference meter", (0.001, 2e-3)),
        ("Bench supply at 12.000 V", ["CONF:VOLT:DC", "AUTO ON"], "your reference meter", (0.001, 2e-3)),
        ("Bench supply at 30 V (if available)", ["CONF:VOLT:DC", "AUTO ON"], "your reference meter", (0.001, 20e-3)),
        ("The 1.5 V cell reversed", ["CONF:VOLT:DC", "AUTO ON"], "negative of the reference", (0.001, 200e-6)),
    ],
    "ohm": [
        ("Short the leads", ["CONF:RES", "AUTO ON"], "0 (lead resistance)", (0, 0.5)),
        ("A 10 Ω resistor", ["CONF:RES", "AUTO ON"], "its value", (0.005, 0.05)),
        ("A 100 Ω resistor", ["CONF:RES", "AUTO ON"], "its value", (0.003, 0.1)),
        ("A 1 kΩ resistor", ["CONF:RES", "AUTO ON"], "its value", (0.002, 1)),
        ("A 10 kΩ resistor", ["CONF:RES", "AUTO ON"], "its value", (0.002, 10)),
        ("A 100 kΩ resistor", ["CONF:RES", "AUTO ON"], "its value", (0.002, 100)),
        ("A 1 MΩ resistor", ["CONF:RES", "AUTO ON"], "its value", (0.005, 1e3)),
        ("A 10 MΩ resistor", ["CONF:RES", "AUTO ON"], "its value", (0.02, 50e3)),
    ],
    "cont": [
        ("Short the leads (expect a beep)", ["CONF:CONT"], "0", (0, 1.0)),
        ("A 47 Ω resistor (just under the 50 Ω threshold: beeps)", ["CONF:CONT"], "its value", (0.02, 1.0)),
        ("A 56 Ω resistor (just over: silent)", ["CONF:CONT"], "its value", (0.02, 1.0)),
    ],
    "diode": [
        ("Short the leads", ["CONF:DIOD"], "0", (0, 0.01)),
        ("A silicon diode (1N4148/1N400x), anode on V", ["CONF:DIOD"], "your reference meter", (0.02, 0.01)),
        ("A Schottky diode (1N5817/BAT85), anode on V", ["CONF:DIOD"], "your reference meter", (0.02, 0.01)),
        ("A red LED, anode on V", ["CONF:DIOD"], "your reference meter", (0.02, 0.02)),
        ("A green or blue LED, anode on V (may read open if over ~3 V)", ["CONF:DIOD"], "your reference meter", (0.02, 0.02)),
    ],
    "temp": [
        ("Type-K probe in free air at room temperature", ["CONF:TEMP KITS90", "TEMP:RTD:UNIT C"], "a room thermometer, °C", (0, 1.5)),
        ("Type-K probe in a stirred ice-water bath", ["CONF:TEMP KITS90", "TEMP:RTD:UNIT C"], "0", (0, 1.5)),
        ("Type-K probe in boiling water", ["CONF:TEMP KITS90", "TEMP:RTD:UNIT C"], "boiling point at your altitude (~100)", (0, 2.0)),
        ("A 100.0 Ω resistor as a PT100 at 0 °C", ["CONF:TEMP PT100", "TEMP:RTD:UNIT C"], "0", (0, 1.0)),
        ("A 138.5 Ω (or 138.51) resistor as a PT100 at 100 °C", ["CONF:TEMP PT100", "TEMP:RTD:UNIT C"], "100", (0, 1.0)),
    ],
    "cap": [
        ("Leads open (expect 0 / no reading)", ["CONF:CAP", "AUTO ON"], "0", (0, 50e-12)),
        ("A 1 nF film or C0G capacitor", ["CONF:CAP", "AUTO ON"], "its value from your reference", (0.02, 20e-12)),
        ("A 100 nF film capacitor", ["CONF:CAP", "AUTO ON"], "its value from your reference", (0.02, 1e-9)),
        ("A 1 µF film capacitor", ["CONF:CAP", "AUTO ON"], "its value from your reference", (0.02, 10e-9)),
        ("A 10 µF capacitor (discharged)", ["CONF:CAP", "AUTO ON"], "its value from your reference", (0.03, 100e-9)),
        ("A 100 µF capacitor (discharged)", ["CONF:CAP", "AUTO ON"], "its value from your reference", (0.03, 1e-6)),
    ],
    "acv": [
        ("Function generator: 1.000 Vrms sine, 50 Hz", ["CONF:VOLT:AC", "AUTO ON"], "1 (or your reference)", (0.01, 2e-3)),
        ("Function generator: 1.000 Vrms sine, 1 kHz", ["CONF:VOLT:AC", "AUTO ON"], "1 (or your reference)", (0.01, 2e-3)),
        ("Function generator: 100 mVrms sine, 1 kHz", ["CONF:VOLT:AC", "AUTO ON"], "0.1", (0.01, 0.5e-3)),
        ("Low-voltage transformer (6-12 VAC), never mains directly", ["CONF:VOLT:AC", "AUTO ON"], "your reference meter", (0.01, 20e-3)),
    ],
    "freq": [
        ("Function generator: 1.000 kHz square, 3 Vpp", ["CONF:FREQ"], "1000", (0.0005, 0.1)),
        ("Function generator: 10.000 kHz square", ["CONF:FREQ"], "10000", (0.0005, 1)),
        ("Function generator: 100.00 kHz square", ["CONF:FREQ"], "100000", (0.0005, 10)),
        ("Function generator: 50 Hz sine", ["CONF:FREQ"], "50", (0.001, 0.05)),
    ],
    "dci": [
        ("mA jack: bench supply 5 V through 4.7 kΩ (≈1 mA), current limit 50 mA", ["CONF:CURR:DC", "AUTO ON"], "your reference ammeter", (0.005, 5e-6)),
        ("mA jack: 5 V through 47 Ω (≈100 mA), current limit 200 mA", ["CONF:CURR:DC", "AUTO ON"], "your reference ammeter", (0.005, 0.5e-3)),
    ],
}


def is_ours(idn):
    """Our firmware, by its *IDN? (older builds said XDM1241-OPEN)."""
    return "OpenOWON" in idn or "XDM1241-OPEN" in idn


class Meter:
    def __init__(self, port, timeout=2.0):
        import serial                                         # pyserial
        self.s = serial.Serial(port, 115200, timeout=timeout)
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def send(self, cmd):
        if not ALLOWED.match(cmd):
            raise ValueError(f"refusing {cmd!r}: not on the allow-list")
        self.s.write((cmd + "\n").encode())
        self.s.flush()

    def query(self, cmd):
        self.send(cmd)
        while True:
            line = self.s.readline().decode(errors="replace").strip()
            # The app's console prints a status line a second (t=12s steps=...)
            # on the same port; a reply never looks like that.
            if not CONSOLE.match(line):
                return line


READ_GAP = 0.35          # s between MEAS? queries: about one display update


def readings(meter, n, gap=None):
    gap = READ_GAP if gap is None else gap
    vals = []
    for _ in range(n):
        r = meter.query("MEAS?")
        try:
            v = float(r)
        except ValueError:
            v = math.nan
        vals.append(v)
        time.sleep(gap)
    return vals


def verdict(mean, ref, tol):
    frac, absol = tol
    if ref is None or math.isnan(mean):
        return "NOREF" if ref is None else "NOREAD", None
    if mean > 9e37:
        return "OVERLOAD", None
    err = mean - ref
    allowed = abs(ref) * frac + absol
    return ("PASS" if abs(err) <= allowed else "CHECK"), err


def run(meter, check, n, out, ask=input, say=print, settle=2.0):
    if check == "beep":
        return run_beep(meter, out, ask, say)
    if check == "scpi":
        return run_scpi(meter, out, say)
    plan = PLANS[check]
    real = check in REAL
    rows = []
    for i, (what, setup, hint, tol) in enumerate(plan, 1):
        say(f"\n[{check} {i}/{len(plan)}] {what}")
        if not real:
            say("  (this function is still simulated on the meter: readings are not real yet)")
        a = ask(f"  connect it, then enter the reference value ({hint}), 's' to skip: ").strip()
        if a.lower() == "s":
            continue
        try:
            ref = parse_si(a)
        except ValueError as e:
            say(f"  {e}; skipped")
            continue
        for c in setup:
            meter.send(c)
        time.sleep(settle)
        fn = meter.query("FUNC?")
        vals = readings(meter, n)
        fn_after = meter.query("FUNC?")
        good = [v for v in vals if not math.isnan(v) and v < 9e37]
        mean = statistics.fmean(good) if good else (9.91e37 if vals and all(v > 9e37 for v in vals) else math.nan)
        spread = statistics.pstdev(good) if len(good) > 1 else 0.0
        v, err = verdict(mean, ref, tol)
        if fn_after != fn:      # a key pressed mid-step (V toggles DC/AC)
            v = "FUNC CHANGED"
        rng = meter.query("RANGE?")
        say(f"  {fn}  mean {mean:.6g}  spread {spread:.3g}  range {rng}  ref {ref}  -> {v}"
            + (f" (error {err:+.4g})" if err is not None else ""))
        rows.append([dt.datetime.now().isoformat(timespec="seconds"), check, what, ref, mean, spread,
                     err if err is not None else "", rng, v, "REAL" if real else "SIM", n])
    write(out, rows)
    return rows


def run_beep(meter, out, ask, say):
    """Loudness against pitch and volume (B57): you read a sound-level meter
    (or a phone app) at a fixed distance and enter dB(A), or skip."""
    rows = []
    say("\nBeeper sweep: hold the sound-level meter 30 cm in front of the meter.")
    meter.send("SYST:BEEP ON")
    meter.send("SYST:BEEP:STYL CLASSIC")
    for hz in (1000, 1500, 2000, 2500, 3000, 3500, 4000, 5000):
        for vol in (100, 50, 25):
            meter.send(f"SYST:BEEP:FREQ {hz}")
            meter.send(f"SYST:BEEP:VOL {vol}")
            meter.send("SYST:BEEP:PLAY TEST")
            a = ask(f"  {hz} Hz at {vol}%: dB(A) (Enter to replay, 's' to skip): ").strip()
            while a == "":
                meter.send("SYST:BEEP:PLAY TEST")
                a = ask(f"  {hz} Hz at {vol}%: dB(A) (Enter to replay, 's' to skip): ").strip()
            if a.lower() == "s":
                continue
            rows.append([dt.datetime.now().isoformat(timespec="seconds"), "beep", f"{hz} Hz {vol}%",
                         "", a, "", "", "", "", "REAL", ""])
    meter.send("SYST:BEEP:FREQ 3000")
    meter.send("SYST:BEEP:VOL 100")
    write(out, rows)
    return rows


def run_scpi(meter, out, say):
    """No parts: identity and the error queue answer, readings arrive."""
    rows = []
    checks = [("*IDN?", lambda r: is_ours(r)), ("SYST:ERR?", lambda r: r.startswith("0")),
              ("FUNC?", lambda r: r.startswith('"')), ("MEAS?", lambda r: bool(re.match(r"^[-+]?\d", r)))]
    for cmd, ok in checks:
        r = meter.query(cmd)
        v = "PASS" if ok(r) else "CHECK"
        say(f"  {cmd:10s} -> {r!r}  {v}")
        rows.append([dt.datetime.now().isoformat(timespec="seconds"), "scpi", cmd, "", r, "", "", "", v, "REAL", 1])
    write(out, rows)
    return rows


HEADER = ["time", "check", "step", "reference", "mean", "spread", "error", "range", "verdict", "front_end", "n"]


def write(out, rows):
    if not rows:
        return
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    new = not out.exists()
    with out.open("a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(HEADER)
        w.writerows(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?")
    ap.add_argument("check", nargs="?", choices=sorted(list(PLANS) + ["beep", "scpi"]))
    ap.add_argument("--out", default=str(ROOT / f"results/bench-{dt.date.today():%Y%m%d}.csv"))
    ap.add_argument("--readings", type=int, default=8)
    ap.add_argument("--list", action="store_true", help="print every step and exit")
    a = ap.parse_args(argv)
    if a.list:
        for k, plan in PLANS.items():
            print(f"{k}{' (real front end)' if k in REAL else ' (simulated on the meter for now)'}:")
            for what, setup, hint, tol in plan:
                print(f"   - {what}   [ref: {hint}; tol ±({tol[0]*100:g}% + {tol[1]:g})]")
        print("beep: loudness sweep, 1-5 kHz x 100/50/25%\nscpi: identity, errors, a reading")
        return 0
    if not a.port or not a.check:
        ap.error("give a port and a check, or --list")
    m = Meter(a.port)
    idn = m.query("*IDN?")
    if not is_ours(idn):
        print(f"*IDN? said {idn!r}: not our firmware's app build. Install xdm1241-ui first.")
        return 2
    print(f"{idn}\nresults -> {a.out}")
    run(m, a.check, a.readings, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
