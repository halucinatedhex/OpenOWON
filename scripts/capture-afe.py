#!/usr/bin/env python3
"""
Capture the HY3131 register writes stock makes for every function and range:
the table afe_configure() needs (lib/hal/hal.c), and the last big unknown on
the analog side.

    scripts/debug.sh &                     # JLinkGDBServer, stock firmware running
    scripts/capture-afe.py --dump backups/<stamp>/flash.bin \\
        --sym symbols/xdm1241_v3.sym --port /dev/cu.usbserial-XXXX

How: GDB breaks on stock_hy_write_regs(idx, buf, n) - stock_hy_write_reg1 calls
it, so this sees every write - prints the arguments and continues. Meanwhile
this script steps the meter through each function and range, over SCPI
(default) or by asking you to press the keys (--manual), and attributes the
writes that follow each step to it. Logical indices are translated to HY3131
register addresses with the index table read from the dump.

Safe: GDB only sets one breakpoint and reads memory. Nothing is written to
flash, and SCPI commands are checked against the same blocklist as
serial-dump.py. Each breakpoint hit pauses the core for a few milliseconds.

Output: build/afe-capture-<stamp>.json, plus a printed summary of which
registers change between steps.
"""
import argparse
import importlib.util
import json
import queue
import re
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GDB = Path.home() / ".platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gdb"
NBYTES = 16                       # bytes printed per hit; stock's longest write is shorter

_sd_spec = importlib.util.spec_from_file_location("sd", ROOT / "scripts/serial-dump.py")
_sd = importlib.util.module_from_spec(_sd_spec)
_sd_spec.loader.exec_module(_sd)

# Default plan: (label, SCPI commands). Stock's exact CONF/RANGE syntax is not
# recovered; edit with --plan, or use --manual and press the keys.
RANGES = {
    "DCV": ("CONF:VOLT:DC", ["5E-2", "5E-1", "5", "50", "500", "1000"]),
    "ACV": ("CONF:VOLT:AC", ["5E-1", "5", "50", "500", "750"]),
    "DCI": ("CONF:CURR:DC", ["5E-4", "5E-3", "5E-2", "5E-1", "5", "10"]),
    "ACI": ("CONF:CURR:AC", ["5E-4", "5E-3", "5E-2", "5E-1", "5", "10"]),
    "RES": ("CONF:RES", ["500", "5E3", "5E4", "5E5", "5E6", "5E7"]),
    "FRES": ("CONF:FRES", ["500", "5E3", "5E4", "5E5", "5E6", "5E7"]),
    "CAP": ("CONF:CAP", ["5E-8", "5E-7", "5E-6", "5E-5", "5E-4", "5E-3", "5E-2"]),
}
# Speed (stock menu Low/Mid/High, SCPI RATE S/M/F) is thought to set AD1OSR
# (register 0x22, analog-frontend.md); these steps show it. Stock refuses RATE
# for continuity..temperature, so it is stepped on DCV and ACV.
RATES = [(f"{fn} 5V rate {r}", [f"{conf} 5", f"RATE {r}"])
         for fn, conf in (("DCV", "CONF:VOLT:DC"), ("ACV", "CONF:VOLT:AC")) for r in "SMF"]
SINGLE = [("CONT", ["CONF:CONT"]), ("DIOD", ["CONF:DIOD"]), ("FREQ", ["CONF:FREQ"]),
          ("PER", ["CONF:PER"]), ("TEMP K", ["CONF:TEMP KITS90"]), ("TEMP PT100", ["CONF:TEMP PT100"])]


def default_plan():
    plan = []
    for name, (conf, ranges) in RANGES.items():
        for i, r in enumerate(ranges):
            plan.append((f"{name} range {i} ({r})", [f"{conf} {r}"]))
    plan += RATES + SINGLE
    return plan


def load_plan(path):
    """Plan file: one step per line, `label | cmd ; cmd`."""
    plan = []
    for line in Path(path).read_text().splitlines():
        line = line.split("#")[0].strip()
        if "|" in line:
            label, cmds = line.split("|", 1)
            plan.append((label.strip(), [c.strip() for c in cmds.split(";") if c.strip()]))
    return plan


# ---- symbols, index table ---------------------------------------------------------

def sym_addr(sym_path, name):
    for line in Path(sym_path).read_text().splitlines():
        m = re.match(r"\s*(0x[0-9a-fA-F]+)\s+(\w+)", line)
        if m and m.group(2) == name:
            return int(m.group(1), 16)
    raise SystemExit(f"{name} not in {sym_path} (run scripts/port-symbols.py on the dump first)")


def index_table(dump_path, sym_path, base=None):
    data = Path(dump_path).read_bytes()
    if base is None:
        base = 0x08000000 if len(data) == 0x40000 else 0x08002800
    t = sym_addr(sym_path, "stock_hy_reg_index_table")
    return list(data[t - base:t - base + 40])


def gdb_script(write_regs, port):
    args = ", ".join(f"*(unsigned char *)($r1 + {k})" for k in range(NBYTES))
    fmt = " ".join(["%02x"] * NBYTES)
    return "\n".join([
        "set pagination off",
        "set confirm off",
        f"target extended-remote :{port}",
        f"break *{write_regs:#x}",
        "commands",
        "silent",
        f'printf "HYW %d %d {fmt}\\n", $r0, $r2, {args}',
        "continue",
        "end",
        "continue",
        "",
    ])


# ---- parsing and attribution --------------------------------------------------------

HYW = re.compile(r"HYW (\d+) (\d+)((?: [0-9a-f]{2}){%d})" % NBYTES)


def parse_line(line, table):
    m = HYW.search(line)
    if not m:
        return None
    idx, n = int(m.group(1)), int(m.group(2))
    raw = [int(b, 16) for b in m.group(3).split()]
    reg = table[idx] if 0 <= idx < len(table) else None
    return {"idx": idx, "reg": reg, "bytes": raw[:min(n, NBYTES)]}


def registers_after(writes):
    """Final value of each register after a sequence of burst writes."""
    regs = {}
    for w in writes:
        if w["reg"] is None:
            continue
        for k, b in enumerate(w["bytes"]):
            regs[w["reg"] + k] = b              # bursts auto-increment
    return regs


def summarise(steps):
    finals = [registers_after(s["writes"]) for s in steps]
    regs = sorted(set().union(*finals)) if finals else []
    lines = [f"{len(steps)} steps, {sum(len(s['writes']) for s in steps)} writes, "
             f"{len(regs)} registers touched"]
    changing = [r for r in regs if len({f.get(r) for f in finals}) > 1]
    lines.append("registers that differ between steps: " + " ".join(f"{r:#04x}" for r in changing))
    for s, f in zip(steps, finals):
        lines.append(f"  {s['label']:<24} " + " ".join(f"{r:02x}={f[r]:02x}" for r in changing if r in f))
    return "\n".join(lines)


class GdbLines:
    """Lines from a running GDB, collected on a thread."""
    def __init__(self, proc):
        self.q = queue.Queue()
        threading.Thread(target=self._pump, args=(proc.stdout,), daemon=True).start()

    def _pump(self, f):
        for line in iter(f.readline, ""):
            self.q.put(line)

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out


def run(plan, lines, table, send, settle=2.0, manual=False, ask=input):
    steps = []
    lines.drain()                               # discard anything before the first step
    for label, cmds in plan:
        if manual:
            ask(f"Set the meter to: {label}  - then press Enter ")
        else:
            for c in cmds:
                _sd.check_command(c)            # never a writing service command
                send(c)
        time.sleep(settle)
        writes = [w for w in (parse_line(l, table) for l in lines.drain()) if w]
        steps.append({"label": label, "scpi": cmds, "writes": writes})
        print(f"  {label:<24} {len(writes)} writes")
    return steps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True, help="full flash dump of this meter (for the index table)")
    ap.add_argument("--sym", default=str(ROOT / "symbols/xdm1241_v3.sym"))
    ap.add_argument("--port", help="serial port for SCPI (omit with --manual)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--plan", help="plan file: `label | cmd ; cmd` per line")
    ap.add_argument("--manual", action="store_true", help="prompt for key presses instead of SCPI")
    ap.add_argument("--gdb-port", type=int, default=2331)
    ap.add_argument("--settle", type=float, default=2.0)
    a = ap.parse_args()

    table = index_table(a.dump, a.sym)
    write_regs = sym_addr(a.sym, "stock_hy_write_regs")
    plan = load_plan(a.plan) if a.plan else default_plan()
    for _, cmds in plan:
        for c in cmds:
            _sd.check_command(c)

    script = ROOT / "build/capture-afe.gdb"
    script.parent.mkdir(exist_ok=True)
    script.write_text(gdb_script(write_regs, a.gdb_port))
    proc = subprocess.Popen([str(GDB), "-q", "-batch", "-x", str(script)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = GdbLines(proc)
    time.sleep(2.0)

    port = None
    if not a.manual:
        import serial
        port = serial.Serial(a.port, a.baud, timeout=0.2)
    send = (lambda c: port.write(c.encode() + b"\n")) if port else (lambda c: None)
    try:
        steps = run(plan, lines, table, send, a.settle, a.manual)
    finally:
        proc.terminate()
        if port:
            port.close()

    out = ROOT / "build" / time.strftime("afe-capture-%Y%m%d-%H%M%S.json")
    out.write_text(json.dumps({"write_regs": hex(write_regs), "index_table": table, "steps": steps}, indent=1))
    print(summarise(steps))
    print(f"\nsaved {out}")


if __name__ == "__main__":
    main()
