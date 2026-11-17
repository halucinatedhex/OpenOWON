#!/usr/bin/env python3
"""
DC current row variants against a V/R reference (docs/test-plan.md T-DCI).

    python3 results/dci-sweep.py <port> <amps>

Set-up: supply (20 V, limit 50 mA) -> 4.7 kOhm -> our mA jack, COM back;
the third meter on DC volts across the resistor. <amps> is its voltage
divided by the resistor as measured on our ohms range (2026-11-14: 19.81 V
/ 4623.0 ohm = 4.285e-3). Panel build (`xdm1241-panel`), which applies each
variant with DIAG:ROW and prints readings through the factory calibration.

The 2026-11-14 finding: with the recovered v2.1.2 rows, ranges 0-1 read
-0.9% and ranges 2-3 +1.3% against V/R. Ranges 1 and 2 have the same
registers but for R24 and R32, and 0/2 and 1/3 differ only in PA8, so the
suspects are the PA13 relay and R24's negative input (SAD1FN<2:0>, bits
2:0: 000 SENSE, 001 RLU, 010 VSSA, 011 AGND, 100 PB2, 101 PB3 as
recovered, 110 PB4, 111 PB5). A variant that reads garbage just says no.

Only DIAG:ROW goes out: the HY3131's working registers and the two relays,
at ~4 mA through 4.7 kOhm. Nothing writes flash.
"""
import re
import statistics
import sys
import time

import serial

VARIANTS = [
    ("DCI 1", ""), ("DCI 2", ""),                                  # baseline
    ("DCI 1", "PA13=1"), ("DCI 2", "PA13=1"),
] + [("DCI 1", f"24=B{fn:X}") for fn in (0, 1, 2, 3, 4, 6)] \
  + [("DCI 2", f"24=3{fn:X}") for fn in (0, 1, 2, 3, 4, 6)]

VALUE = re.compile(r"^row DCI/\d+: .* = ([-+0-9.eE]+) \(factory cal\)")


def run(port, cmd, secs=8.0):
    q = time.time()
    while time.time() - q < 1.0:                    # wait until no readings arrive
        if "row" in port.read(4096).decode(errors="replace"):   # (the heartbeat never stops)
            q = time.time()
    port.write((cmd + "\n").encode())
    t, buf = time.time(), ""
    while time.time() - t < secs:
        buf += port.read(4096).decode(errors="replace")
    return buf.splitlines()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port = serial.Serial(sys.argv[1], 115200, timeout=0.2)
    ref = float(sys.argv[2])
    print(f"reference {ref * 1e3:.4f} mA (V/R)\n")
    print(f"{'variant':24s} {'mean mA':>10s} {'error':>8s}  notes")
    for row, extra in VARIANTS:
        cmd = f"DIAG:ROW {row} 6 {extra}".strip()
        lines = run(port, cmd)
        vals = [float(m.group(1)) for l in lines if (m := VALUE.match(l))]
        note = "; ".join(l for l in lines if "DIFFERS" in l or "wrote" in l)
        if len(vals) < 3:
            print(f"{row + ' ' + extra:24s} {'-':>10s} {'-':>8s}  no readings {note}")
            continue
        m = statistics.fmean(vals[2:])
        print(f"{row + ' ' + extra:24s} {m * 1e3:10.4f} {(m / ref - 1) * 100:+7.2f}%  {note}")


if __name__ == "__main__":
    main()
