# The front-end mode table, recovered by emulation

Stock was erased from our meter before its HY3131 traffic could be captured
with a logic analyser (`analog-frontend.md`). This table comes from running
OWON's **v2.1.2** image instead: `scripts/emu-afe-table.py` boots it in a
Cortex-M emulator (Unicorn), runs `stock_boot_init` (which sets bias,
references and enables once), then calls
`stock_meas_set_function(func, range, 0)` for every function and range. It
logs every HY3131 write (hooked at stock's access layer) and every GPIO
write. Raw output: `test/data/afe_v2.1.2_emulated.json`.

**Provisional: v2.1.2 is built for the older STM32 board.** Nothing here
goes to the meter until each entry is checked there.

## What each call does

1. `R20 <- 0x68`, then
2. the whole register file in one burst from 0x00 (data registers are
   read-only, so only 0x20-0x37 take effect), then
3. **two GPIO lines: PA8 and PA13, the relays** (through the ELD207 dual
   opto). On our meter PA8 reads "held low" with nothing driving it
   (`hardware.md`), as an opto LED driver with a pull-down would.

## Checked against a real capture

Petteri Aimonen's logic capture of an XDM2041 (same HY3131) in 5 V DC
(`owon-xdm2041-info/spi_captures/measurement_modes.md`), against our DCV
range 2:

```
reg:      20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33
XDM2041:  c2 21 14 8b 01 11 08 15 31 f8 20 00 00 90 28 a0 80 c7 33 a8
emulated: c0 00 16 8b 01 11 00 15 31 00 20 00 00 90 28 a0 80 c7 33 a4
          ^^ ^^ ^^ == == == ^^ == == ^^ == == == == == == == == == ^^
```

**The routing matches exactly**: AD1 input mux and gain (R23-R25), AD2 mux
(R27), reference (R28), the PA divider taps (R2A-R2E), function select
(R2F), reference and bias enables (R30, R31), op-amps (R32). The
differences are in what the two models run besides AD1: conversion rate
R22 (a user setting, B11), AD2 and the RMS/peak blocks (R26, R29), the
counter (R20, R21) and one bit of R33. Before `stock_boot_init` was run
first, R2F/R30/R31 lacked their enable bits: that's set once at start-up
and kept in stock's register copy.

## On the meter (2026-11-11): DC V

`DIAG:DCV <range>` on the `xdm1241-panel` build writes a DCV row from this
table, drives PA8/PA13 as it says, reads the registers back (identical every
time) and converts AD1 through the factory calibration (`meas_dcv`):

| Input | Range | Counts | Volts |
| --- | --- | --- | --- |
| open | 5 V | -44 .. -108 (after 2 settling readings) | -0.07 mV |
| a source a good meter read as 8.2 V | 5 V | 8,388,606 | over-range (correct) |
| same | 50 V | 981,345 | **8.2253 V** |
| same | 500 V | 98,667 | **8.2266 V** |

Two ranges with different divider taps and different calibration slots
agree to 0.02%: the routing and per-range calibration work. Conversions
every ~242 ms. A cheap handheld read the source as 8.12 V (1.3% low); a
better meter read **8.2 V**, agreeing with ours to its last digit. So the
gap was the cheap meter. R22 (rate) and R33 still differ from the XDM2041
capture; no effect on scale is visible at this level.

## On the meter (2026-11-11): resistance (2-wire)

`DIAG:OHM <range>` (panel build), the emulated OHM2 rows, both relays on.
A 1.5 kohm resistor:

| Range | Counts | Ohms |
| --- | --- | --- |
| 5 kohm | 1,128,485 | **1495.3** |
| 50 kohm | 113,602 | **1497.5** |
| 500 ohm | 8,388,606 | over-range (correct) |

Two ranges, different reference currents and calibration slots, agree to
0.15%; 0.3% from nominal. Registers read back identical. **Ohms needs three
conversions to settle**, not two: the first reading after the two discarded
ones was still 1481 ohm (`ACQ_DISCARD_OHM` = 3). A good meter reads the
resistor as **1.497 kohm**. In the app (`xdm1241-ui`), `CONF:RES` then
`MEAS?`: **1495.5 ohm**. Confirmed; in `afe_table.h`.

## On the meter (2026-11-11): continuity and diode

`DIAG:CONT` / `DIAG:DIODE` (panel build), the emulated CONT/0 and DIODE/0
rows, both relays on. Both convert every **20 ms** (R22 = 0x10, against
0x16 and ~242 ms for DC V and ohms): fast enough for a continuity beeper.

| | Leads shorted | Open | 1.5 kohm |
| --- | --- | --- | --- |
| continuity | **0 .. 0.16 ohm** | over-range | over-range (span ~1 kohm) |
| diode | **-5 .. +2 mV** | 3.262 V | 3.258 V |
| ohms, 500 ohm range (for comparison) | 0.073 ohm | | |

**Continuity: confirmed.** **Diode: reads across the leads (0 V shorted),
open-circuit 3.26 V.** 1.5 kohm leaves it at that open-circuit limit, so
the test current is above ~2.2 mA. **A silicon diode: 0.5616 V. Confirmed.**
Both in `afe_table.h`; in the app, `CONF:DIOD` / `MEAS?` gives 0.569 V on
the same diode.

## On the meter (2026-11-11): capacitance, in progress

`DIAG:CAP <range>` (panel build), the emulated CAP rows 0-4, both relays on.
**Not in `afe_table.h`: the scale does not agree between ranges yet.**

**The counters need a restart after every reading**, as stock does it
(`0x08007CBC`): R20 <- 0x70, then a 3-byte preset into **CTA (0x1B-0x1D),
which is writable**, then R20 <- 0x72. The preset is `00 00 bf` (0xBF0000)
for ranges 0-4 (`0x08007CA8`); it is the `0xbf` at 0x1D in the emulated
burst. Without the restart the counter gives one result and stops; with no
preset the first gate took 4.6 s. With it: one result every ~1.3 s.
CTSTA reads 0x70 while counting and 0x50 when done.

A capacitor sold as ~10 uF (electrolytic):

| Range | CTB | CTC | CTC/CTB | Reads | Spread |
| --- | --- | --- | --- | --- | --- |
| 5 uF (2) | 1 | 2,717,300 | 2,717,300 | 11.47 uF (over range) | 0.01% |
| 50 uF (3) | 5 | 2,135,000 | 427,000 | 17.62 uF | 0.05% |
| 500 uF (4) | 8 | 2,305,300 | 288,200 | 25.26 uF | 0.03% |

**Open leads**, every range 0-4: CTB = 0 (no complete charge cycle), a
result every ~63 ms, CTC 0 or a few thousand counts. So there is no
open-lead offset to subtract; CTB = 0 means "no capacitor" (our
`meas_cap_counter` returns NaN for it; it should display 0 or Open).

**A film/ceramic capacitor a good meter reads as 101.8 nF:**

| Range | CTB | CTC/CTB | Reads | Factor high |
| --- | --- | --- | --- | --- |
| 50 nF (0) | 17-18 | 137,000-140,800 | 428.8-441.0 nF | 4.2-4.3 |
| 500 nF (1) | 17 | 140,800 | 441.0 nF (same row as range 0; same answer) | 4.3 |
| 5 uF (2) | 19 | 125,200 | 526 nF | 5.2 |
| 50 uF, 500 uF | 0 | | no complete cycle | |

So the scale is wrong by a different factor per range, by 4-5x, not by a
constant. **Not the clock:** R33 = 0xA4 selects the crystal (ENXI = 0, as
the XDM2041 capture), and the CTA trace gives SYSCLK ~3.5 MHz, at most a
1.15x effect. The likely cause is the analog set-up of these rows (PA taps
R2C/R2D, comparator thresholds R20 SCMPI and R21 SCMPRH/L) differing from
what this board's stock firmware used. v2.1.2's emulated rows cannot settle
it; a capture from a working XDM1241 in capacitance mode would.

**From the HY3131 datasheet (§14, Frequency Counter)**, which explains the
restart:
- R20 bit 1 is ENCTR. 0 stops the counter and clears CTA<7:0>, CTB, CTC and
  CTBOV; CTA<23:8> takes a preset over SPI only while ENCTR = 0.
- Gate = (0x1000000 - CTA preset) / SYSCLK. After CTA overflows the counter
  runs on to the end of the current input cycle, then sets CTF and stops.
- CTB is whole input cycles; **CTC is SYSCLK ticks while the input is
  high**, so CTC/CTB is the high time per cycle.
- **Reading INTF clears every flag in it.** Polling INTF in a tight loop
  occasionally loses CTF (seen: the counter stops, CTF never seen). Stock
  polls once per main-loop pass. A robust reader should also treat "CTA
  stopped" as done.

**Clean emulation (2026-11-11, `emu-afe-table.py --clean`,
`test/data/afe_v2.1.2_emulated_clean.json`):** each function from a fresh
boot instead of after the previous ones. Two findings:

- R26 = 0x08 and R29 = 0xF8 (AD2, RMS/filter on) are missing from every
  clean row; the real XDM2041 capture has them. Stock sets them once (in
  the part of boot that faults in the emulator, or the first AC visit) and
  keeps them, so for those two registers **the sequential sweep is the one
  that matches hardware**, and the confirmed DCV/ohms rows stand.
- **Stock's capacitance set-up never writes R22, R24 or R28**: their values
  are whatever the previous function left. The sequential rows we tried
  carry R22 = 10, R24 = 85, R28 = 11 (from diode); a clean start has
  R22 = 16, R24 = 01, R28 = 31 (the DCV 5 V values). These set the AD1
  rate, AD1 input mux and reference, which the counter path should not
  need, but they are the first variant to try: `DIAG:CAPX 3 22=16 24=01
  28=31` (docs/test-plan.md T-CAP).

Each range is steady; they disagree.

**Session 2026-11-12 (DIAG:CAPX):**

- **R24 is the main scale error.** Stock's capacitance set-up never writes
  it; after diode it holds 0x85 (AD1 pre-filter on PB0/PB3, the pins the
  capacitance circuit uses), which loads the capacitor. 0x01 (the boot
  value) fixes it: a 10 uF electrolytic reads 10.68 / 10.53 / 10.35 uF on
  the 5 / 50 / 500 uF ranges (was 11.50 / 17.61 / 25.27). R22 and R28 make
  no difference. (A cheap meter reads it 11.1 uF.)
- **R21 (comparator thresholds) sets the scale**: a 10 nF ceramic on the
  50 nF range reads 80 / 22 / 19 nF at R21 = 0x10 / 0x21 / 0x31.
- **Each cycle carries a fixed overhead.** On the 50 uF range, 92 nF gives
  10,800 counts per cycle and 10.5 uF gives 252,000: a line through them is
  ~8,700 counts + ~23,200 counts/uF. So small values on a range read high
  (92 nF reads 450 nF there) unless an offset is subtracted, which stock's
  cal "zero" term does for stock's own set-up.
- **Low ranges often complete no cycle**: 92 nF on the 50 nF-5 uF ranges
  mostly gives CTB = 0 within the ~63 ms gate (the gate is ~63 ms whatever
  the CTA preset suggests; with a slow input the counter waits for a whole
  cycle, as the 10 uF readings of 1.2 s show).
- **To finish:** per range, a two-point fit (scale and offset) from parts
  known to ~1% (film/C0G, measured on an LCR meter), after sorting out the
  CTB = 0 cases (gate or charge current). The cheap meter is not a good
  enough reference to fit to. Stock's divisors (checked again in
the disassembly) imply cycle-time ratios of 10 (range 2 to 3) and 3.25
(3 to 4); measured 6.4 and 1.5. Either some rows' current taps (R2C/R2D,
R31) are not what this board's stock firmware used, or the cycle has a
fixed overhead. Next: a reference value from another meter, and a second,
smaller capacitor (a film cap) to separate scale from offset.

**Session 2026-11-13: eleven parts on an LCR meter** (7.55 nF-263.8 uF,
`results/cap-20261113/`; parts and LCR not precision, electrolytics good
to several percent at DC). Ranges 2-4 with R24 = 01.

- **The counters run in two modes, and only one is right.** After a
  range is configured and restarted, every gate is either **~63 ms** or
  **~1.2 s** (plus up to one input cycle), and stays so for the run. With
  the long gate, stock's factory arithmetic is close and linear; with the
  short one it reads 2-20x high and is not linear in C. The long gate is
  the one the CTA preset predicts (0x410000 counts; ~4.6 s from 0 means
  ~1.17 s from 0xBF0000), so **the short gate is the fault**. Every long
  run began with an empty first reading (CTB 0, CTC 0); every short one
  with counts in it. Slow inputs (big C for the range) ended up long,
  fast ones short. Why is not known yet.
- **The "per-cycle overhead" and "low ranges complete no cycle" above are
  both this.** The 92 nF / 10.5 uF line mixed a short-gate and a
  long-gate reading; and 63 ms is too short for nF on the low ranges.
- **Long gate, factory arithmetic, no fit:**

  | Part (LCR) | 5 uF range | 50 uF | 500 uF |
  | --- | --- | --- | --- |
  | 431.4 nF | 420.9 nF (-2.4%) | | |
  | 1.036 uF | 1.0117 (-2.3%) | | |
  | 3.68 uF | 3.585 (-2.6%) | 3.543 (-3.7%) | |
  | 10.4 uF | 10.07 (-3.2%) | 9.884 (-5.0%) | 9.735 (-6.4%) |
  | 22.7 uF | | 21.51 (-5.3%) | 21.30 (-6.2%) |
  | 45.9 uF | | 47.49 (+3.5%) | 46.64 (+1.6%) |
  | 96.68 uF | | | 98.32 (+1.7%) |
  | 263.8 uF | | | 294.1 (+11.5%) |

  Repeat readings agree to ~0.1-0.3%. The ranges agree with each other
  (same part) within 1-3%; the scatter against the LCR swings both ways
  from part to part (-6% then +3.5%), so it is the parts' DC-vs-LCR value,
  not the meter. The two film-like parts on the 5 uF range agree on
  -2.4%. **No fit is justified by these parts beyond that.**
- **Next:** find what selects the short gate (read CTA during a gate;
  compare stock's sequence around 0x08007CBC: does it discard the first
  reading, or wait between R20 0x70 and 0x72?), make every gate long,
  then re-run this sweep including ranges 0-1.

**Same day: the short gate found and fixed.** Stock's restart (0x08007CBC)
is exactly ours: R20 0x70, CTA = 00 00 BF, R20 0x72, nothing else. The
datasheet (section 14): ENCTR = 0 clears CTA<7:0>, CTB, CTC; CTA<23:8> is
written over SPI only while ENCTR = 0; the gate is (1000000h - CTA)
SYSCLK ticks, then the counter runs on to the end of the input cycle.
Counting time = (1000000h - CTA start + CTA end) ticks; CTC = SYSCLK ticks
while the input is high. A panel-build A/B (`DIAG:CAPRB`, 114 nF on
ranges 2 and 4, run 0-1-2-3-0):

| After R20 = 0x72 | Gate | CTB (range 2 / 4) |
| --- | --- | --- |
| nothing (as before) | ~70 ms, every time | 0-1 / 20 |
| read CTA back | ~1.25 s, every time | 79 / 1380 |
| read CTA only at the end | ~80 ms | 0-1 / 20 |

So **reading CTA right after starting the counter gives the full gate**.
Why is not proven: most likely polling INTF immediately after ENCTR = 1
ends the gate early, and the read is enough delay. CTA reads back as the
preset (BF0000) whenever it was taken. The full gate is 4.34 M ticks in
~1.19 s: SYSCLK ~3.65 MHz on this board. `lib/app/acquire.c`
`cap_restart()` now does the read, also after configuring.

Ranges 0-1 then work: same row, ~1.2 s gates, many cycles:

| Part (LCR) | 50 nF range | 500 nF | 5 uF |
| --- | --- | --- | --- |
| open | -0.04 nF | -0.04 nF | -2.1 nF |
| 7.55 nF | 8.17 (+8%) | 8.14 (+8%) | 6.1 |
| 48.36 nF | 47.36 (-2.1%) | 47.39 (-2.1%) | 45.7 |
| 114.4 nF | 112.4 (-1.7%) | 112.4 (-1.7%) | 110.5 (-3.4%) |

Open leads read ~0 (the factory zero holds), so the 7.55 nF part's +8%
is the part, not a lead offset. **Ranges 0-4 are in the app**, factory
arithmetic, no fit; user calibration for anyone with better parts.

## On the meter (2026-11-13): AC volts and frequency (panel build, `DIAG:ROW`)

Source: the owner's DSO-TC3 generator (sine and square, 3.3 Vpp fixed),
its own scope as the reference (3.24 Vpp at 10 kHz; ~1.02 kHz at the
"1 kHz" setting). Raw lines in `results/acv-freq-20261113.md`.

- **SYSCLK is 4.0 MHz** (1.0668 M ticks in 267 ms): stock's `meas_freq`
  constant holds. (The 3.65 MHz estimate from the capacitance session came
  from the polling loop's timing.)
- **Frequency (FREQ/0 row as emulated, with the CTA read-back restart):**
  99.834 Hz, 1023.03 Hz, 10000.1 Hz, each steady to ~0.01%; the
  generator's "1 kHz" really is ~1.02 kHz on its own scope. Square at
  1 kHz: duty 61.5% (sine 59.5%, 58.8% at 100 Hz/10 kHz). With the ground
  lead off, a steady 60.0 Hz at 33% (mains pickup).
- **AC volts (ACV/2, the 50 V range; first written up as 5 V by mistake):** waiting on AD2F re-reads a stale RMS sum
  every 7 ms; **RMSF** gives a fresh one every ~240 ms (`DIAG:ROW` fixed;
  `acquire.c` still waits on AD2F for AC: fix when AC goes real). Sine
  3.3 Vpp (1.1667 V nominal): 1.1567 V at 100 Hz, 1.1550 V at 1 kHz.
  Square 0-3.3 V at 61.5% duty: 1.590 V (1.606 V expected).
- **Bandwidth is set by R26 (AD2 clock).** The emulated row has R26 = 0x88:
  SAD2CLK = 1, AD2CLK = SYSCLK/4, OSR 32, 31.25 kHz AD2 rate; the comb
  filter then rolls off hard. R26 = 0x80 (SYSCLK/2, 62.5 kHz):

  | Sine, 3.3 Vpp | R26 0x88 | R26 0x80 |
  | --- | --- | --- |
  | 1 kHz | 1.1550 V | 1.1550 V |
  | 3 kHz | 1.1412 V | – |
  | 5 kHz | 1.0862 V (-6.1%) | 1.1227 V (-2.9%) |
  | 10 kHz | 0.518 V (-55%) | 1.009 V (-12%; -13% vs 100 Hz) |

  Same scale at 1 kHz (0.02%), twice the reading rate (~110 ms). Stock is
  reported -3 dB at 8.4 kHz (#288); 0x80 does better. **Use R26 = 0x80 for
  AC** when the rows go into `afe_table.h` (a deliberate departure from the
  emulated table). On ACV/3 (50 V) the same signal reads ~1.24 V with
  either setting (2% of range).
- **All five ranges at 60 Hz** (transformer, 32.4 Vpp on its scope =
  11.455 V; then resistor dividers from it), R26 = 0x80, factory
  arithmetic: 50 V 11.451, 500 V 11.453, 750 V 11.455 V (0x88: within
  0.15%); divider 1: 5 V 2.0704, 50 V 2.0755, 500 V 2.105 V; divider 2:
  500 mV 0.1907, 5 V 0.1910 V. **Every range agrees with its neighbour
  within 0.25% and the top three with the transformer within 0.2%.**
- **The 5 V range (ACV/1) rolls off; the others do not.** Generator sine,
  5 V vs 50 V range on the same signal: 100 Hz 1.1556 / 1.1570 V; 1 kHz
  1.1166 / 1.1550 V (-3.4%); 3 kHz 1.0127 / 1.1440 V (-11.5%). R26 makes
  no difference to this. The two rows differ only in the PA taps (ACV/1:
  R2D D0, R2E 88, i.e. 100k and 1M legs against 10M, ~1:111, which is
  stock's K 27.06 = 24.6 x 1.1; ACV/2: R2D 09, R2E 28, 10k against 10M).
  An uncompensated ~430 pF at the 100k node would do it; stock probably
  compensates on the board or with a tap choice this emulated v2.1.2 row
  does not have. Tap variants at 3 kHz alone (2E=08: 0.735, 2E=80: 0.925,
  2D=50: 0.022) cannot tell flatness from scale: a variant has to be read
  at two frequencies. Open.
- `R34` bit 0 sometimes reads 1 after writing 0 (datasheet: not a
  settable bit); read-back checks mask it.
- **Not yet checked:** frequency's other ranges, the 5 V range's
  roll-off. AC current: the emulated rows with R26 0x80 hold on all six
  ranges (`test-plan.md`, 2026-11-16 and 2026-11-17; B59).

## On the meter (2026-11-11): temperature (type K), in progress

`DIAG:TEMP` (panel build). Rows from a clean emulation (boot_init, then
set_function(TEMP, 0), then stock's cold-junction routine 0x08008DD4; the
table run's TEMP rows carry counter/AD2 state left over from the FREQ
sweep before them):

- **Thermocouple**: the DC 500 mV row with **R31 = 0xD7** (SAGND = 01;
  DCV uses 0xC7). PA8 on, PA13 off.
- **Cold junction**: the same row with **R33 = 0xA6, then 0xA7** (SAD1I =
  10, 11), then back to 0xA4. This is the datasheet's on-chip temperature
  sensor procedure (§12.4): two AD1 readings, half the difference, slope G.

Cold junction, first run: -53.96 mV and +52.67 mV (symmetric, as the
datasheet's two sensor outputs should be), factory G = 0.1732 ->
**18.7 C**. Not yet checked against a room thermometer. The thermocouple
path is untested: a charged capacitor was on the leads.

Next: room temperature to check the cold junction, then a type-K probe at
room temperature, then something warm. Only then into `afe_table.h`.

## In the app (2026-11-11)

`lib/hal/afe_table.h` holds the rows confirmed on the meter (DC V, ranges
0-5). The `xdm1241-ui` build (`HAL_AFE_DCV_REAL`) routes DC V to the real
HY3131 and every other function to the simulator, with the relays released;
the real path is only used if the passive pin check passes at boot. The
app drops two conversions after each configuration (`ACQ_DISCARD`): the
chip's filter needs both. `MEAS?` with the 8.2 V source: 8.2216 V.

On the panel: **8.22 V, autoranged to 50 V**. Leads off: it ranges down
and relay PA8 clicks on (the direct 500 mV / 50 mV ranges, as stock).
Leads back: up to 50 V, PA8 off. Switching function from there gives no
click (PA8 already released), as the table predicts.

## The table

Range numbers are stock's (`measurement.md`); entries past a function's
last range are whatever stock does with an out-of-range index.

| Function/range | R20 … R33 | PA8 | PA13 | Also |
| --- | --- | --- | --- | --- |
| DCV/0 | `c0 00 16 8b 35 11 00 15 31 00 00 00 00 00 08 81 80 c7 3c a4` | 1 | 0 |  |
| DCV/1 | `c0 00 16 8b 85 11 00 15 31 00 00 00 00 00 08 81 80 c7 33 a4` | 1 | 0 |  |
| DCV/2 | `c0 00 16 8b 01 11 00 15 31 00 20 00 00 90 28 a0 80 c7 33 a4` | 0 | 0 |  |
| DCV/3 | `c0 00 16 8b 01 11 00 15 31 00 20 00 00 09 28 a0 80 c7 33 a4` | 0 | 0 |  |
| DCV/4 | `c0 00 16 8b 01 11 00 15 31 00 20 00 90 00 28 a0 80 c7 33 a4` | 0 | 0 |  |
| DCV/5 | `c0 00 16 8b 09 01 00 15 31 00 20 00 90 00 28 a0 80 c7 33 a4` | 0 | 0 |  |
| DCV/6 | `c0 00 16 8b 87 11 00 15 31 00 00 00 00 00 00 a0 80 c7 33 a4` | 0 | 0 |  |
| ACV/0 | `c0 00 1b 16 45 00 88 15 31 f8 00 00 00 00 08 91 80 c7 3c a0` | 1 | 0 |  |
| ACV/1 | `c0 00 1b 16 4d 00 88 15 31 f8 22 00 00 d0 88 a0 ff c7 38 20` | 0 | 0 |  |
| ACV/2 | `c0 00 1b 16 4d 00 88 15 31 f8 22 00 00 09 28 a0 ff c7 38 20` | 0 | 0 |  |
| ACV/3 | `c0 00 1b 16 4d 00 88 15 31 f8 22 00 90 00 28 a0 80 c7 38 20` | 0 | 0 |  |
| ACV/4 | `c0 00 1b 16 4d 00 88 15 31 f8 22 00 90 00 28 a0 80 c7 38 20` | 0 | 0 |  |
| ACV/5 | `c0 00 1b 16 4d 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 33 a0` | 0 | 0 |  |
| ACV/6 | `c0 00 1b 16 4d 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 33 a0` | 0 | 0 |  |
| DCI/0 | `c0 00 16 8b 35 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 3f a8` | 0 | 0 |  |
| DCI/1 | `c0 00 16 8b b5 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 33 a8` | 0 | 0 |  |
| DCI/2 | `c0 00 16 8b 35 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 3f a8` | 1 | 0 |  |
| DCI/3 | `c0 00 16 8b b5 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 33 a8` | 1 | 0 |  |
| DCI/4 | `c0 00 16 8b 35 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 3d a8` | 0 | 0 |  |
| DCI/5 | `c0 00 16 8b 35 01 08 15 11 f8 00 00 00 00 00 a0 80 c7 3d a8` | 0 | 0 |  |
| DCI/6 | `c0 00 16 8b 87 11 08 15 11 f8 00 00 00 00 00 a0 80 c7 33 a0` | 0 | 0 |  |
| ACI/0 | `c0 00 00 00 45 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 3f 20` | 0 | 0 |  |
| ACI/1 | `c0 00 00 00 45 00 88 15 31 f8 00 00 00 00 00 a0 80 c7 3f a0` | 0 | 0 |  |
| ACI/2 | `c0 00 00 00 45 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 3f 20` | 1 | 0 |  |
| ACI/3 | `c0 00 00 00 45 00 88 15 31 f8 00 00 00 00 00 a0 80 c7 3f a0` | 1 | 0 |  |
| ACI/4 | `c0 00 00 00 45 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 3d 20` | 0 | 0 |  |
| ACI/5 | `c0 00 00 00 45 00 88 15 31 f8 00 00 00 00 00 a0 80 c7 3d a0` | 0 | 0 |  |
| ACI/6 | `c0 00 00 00 45 01 88 15 31 f8 00 00 00 00 00 a0 80 c7 33 a0` | 0 | 0 |  |
| OHM2/0 | `c0 00 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| OHM2/1 | `c0 00 16 83 85 01 08 15 00 f8 00 40 60 00 00 94 80 d3 3f a4` | 1 | 1 |  |
| OHM2/2 | `c0 00 16 83 85 01 08 15 00 f8 00 40 00 06 00 94 80 d3 3f a4` | 1 | 1 |  |
| OHM2/3 | `c0 00 16 83 85 01 08 15 00 f8 00 40 00 60 00 94 80 d3 3f a4` | 1 | 1 |  |
| OHM2/4 | `c0 00 16 93 85 01 08 15 55 f8 00 00 00 80 00 86 80 d1 3f a4` | 1 | 1 |  |
| OHM2/5 | `c0 00 16 93 85 01 08 15 55 f8 00 08 00 00 00 86 80 d1 3f a4` | 1 | 1 |  |
| OHM2/6 | `c0 00 16 83 85 01 08 15 00 f8 00 00 00 00 00 86 80 d1 3f a0` | 1 | 1 |  |
| OHM4/0 | `c0 00 16 83 a7 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| OHM4/1 | `c0 00 16 83 a7 01 08 15 00 f8 00 40 60 00 00 94 80 d3 3f ac` | 1 | 1 |  |
| OHM4/2 | `c0 00 16 83 a7 01 08 15 00 f8 00 40 00 06 00 94 80 d3 3f ac` | 1 | 1 |  |
| OHM4/3 | `c0 00 16 93 a7 01 08 15 55 f8 00 40 00 60 00 94 80 d3 3f ac` | 1 | 1 |  |
| OHM4/4 | `c0 00 16 93 a7 01 08 15 55 f8 00 00 00 80 00 86 80 d1 3f ac` | 1 | 1 |  |
| OHM4/5 | `c0 00 16 93 a7 01 08 15 55 f8 00 08 00 00 00 86 80 d1 3f ac` | 1 | 1 |  |
| OHM4/6 | `c0 00 16 93 a7 01 08 15 00 f8 00 00 00 00 00 86 80 d1 3f a0` | 1 | 1 |  |
| CONT/0 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/1 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/2 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/3 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/4 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/5 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| CONT/6 | `74 62 10 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f ac` | 1 | 1 |  |
| DIODE/0 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/1 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/2 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/3 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/4 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/5 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| DIODE/6 | `c0 62 10 8b 8d 11 08 15 11 f8 00 00 08 00 00 86 80 e2 33 a4` | 1 | 1 |  |
| CAP/0 | `72 21 10 8b 85 11 08 15 11 f8 00 00 00 08 00 9a 80 d7 33 a4` | 1 | 1 |  |
| CAP/1 | `72 21 10 8b 85 11 08 15 11 f8 00 00 00 08 00 9a 80 d7 33 a4` | 1 | 1 |  |
| CAP/2 | `72 21 10 8b 85 11 08 15 11 f8 00 00 80 00 00 8e 80 d1 33 a4` | 1 | 1 |  |
| CAP/3 | `72 21 10 8b 85 11 08 15 11 f8 00 00 08 00 00 8e 80 d1 33 a4` | 1 | 1 |  |
| CAP/4 | `72 21 10 8b 85 11 08 15 11 f8 00 00 08 00 00 8e 80 d2 33 a4` | 1 | 1 |  |
| CAP/5 | `c0 21 07 93 85 11 08 15 55 f8 80 00 08 00 00 86 80 d1 33 ac` | 1 | 1 | HY 0x2A <- 00; HY 0x2C <- 00 |
| CAP/6 | `70 de 07 93 85 11 08 15 55 f8 80 00 08 00 00 8f 80 da 33 ac` | 1 | 1 | HY 0x2A <- 00; HY 0x2C <- 00 |
| FREQ/0 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/1 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/2 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/3 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/4 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/5 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| FREQ/6 | `ca de 07 93 85 11 08 15 55 f8 00 00 00 00 00 80 80 d7 33 ac` | 0 | 1 |  |
| TEMP/0 | `c2 de 16 8b 85 11 08 15 31 f8 00 00 00 00 08 81 80 d7 33 a4` | 1 | 0 |  |
| TEMP/1 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| TEMP/2 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| TEMP/3 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| TEMP/4 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| TEMP/5 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
| TEMP/6 | `c2 de 16 83 85 01 08 15 00 f8 00 40 06 00 00 94 80 d2 3f a4` | 1 | 1 |  |
