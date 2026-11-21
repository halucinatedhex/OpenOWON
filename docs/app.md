# The application and the host simulator

The meter's application runs **today on the Mac**, against a simulated
HY3131 and a simulated panel. The same code will run on the meter once
`hal/` provides a `platform_t`.

```sh
make sim        # run scenarios/*.sim, screenshots to build/sim-*.png
make test       # host tests, including end-to-end app tests
make portable   # compile the on-meter layers for Cortex-M4, -Werror
make check      # all three
```

## Layers

```
lib/app/        the application                          portable
  platform.h      everything the app needs from a board (platform_t)
  app.c           functions, ranging, pipeline, keys, view, rendering
  acquire.c       front-end config -> wait for data -> read -> convert
  readout.c       overload limits, autorange, display text
lib/sim/        HOST ONLY
  sim_hy3131.c    bit-level SPI slave + analog model
  sim_lcd.c       8080/DCS panel decoder -> image
  sim_board.c     platform_t: virtual clock, key queue, beeper
tools/meter-sim.c  scripted simulator (make sim)
scenarios/*.sim    scripts; each `expect` line is a check
```

In the simulator, a physical input goes through **the same code that will
run on the meter**:

```
sim_world_t.dcv = 1.2345
  -> sim_hy3131 (model)  -> SPI bits -> lib/afe driver
  -> acquire.c  -> lib/measure  -> app pipeline  -> view
  -> ui (band by band) -> lib/display driver -> 8080 bytes
  -> sim_lcd decoder -> build/sim-*.png
```

The simulator models are the only code that won't ship. The tests check that
the panel image equals a direct full-screen draw of the same view. That
proves the band renderer, the LCD driver and the decoder agree.

## Writing a scenario

```
set dcv 0.0123          # quantities: dcv acv dci aci ohm diode cap freq duty temp cj noise
key V                   # keys: V I OHM CAP FREQ TEMP UP DOWN LEFT RIGHT RUNSTOP MATH F1..F6 ...
run 2000                # virtual milliseconds
expect reading 12.300   # fields: reading unit range func status sub key1..key6 menu reply
expect replynear 47000 1e-5   # next serial reply as a number, within a relative tolerance
shot build/sim-x.bmp
print
```

`meter-sim` exits non-zero if any `expect` fails, so a scenario is a
regression test. `make sim` runs every file in `scenarios/`.

## Pipeline

Per sample (as fast as the HY3131 delivers):
1. `acq_poll` returns a converted SI value (lib/measure).
2. It goes into the filter: a 200-sample average that resets on a step
   larger than 10% of full scale, as stock does.
3. The continuity and diode beeper is decided per sample, not per display
   update.

Every 200 ms (`APP_DISPLAY_MS`):
1. Take the average.
2. Overload check, using the manual's limits.
3. Autorange: up on overload, down below 90% of the next range down.
4. HOLD freezes the view here.
5. REL, then low-reading suppression (ACV, ACI, capacitance), then
   min/max/avg and the trend trace.

Rendering only happens when something changed, at two levels:

- **Frame:** before composing, `app_render` fingerprints everything
  `app_draw` reads (the view text, theme, screen, menu row, and on the graph
  screens the trace and its scale; about 1 KB). If it matches the last
  frame, nothing is composed (B56: a full compose took 84 ms on the meter).
  Clearing `hash_valid` forces a full redraw. `test_app` walks every screen
  and checks the panel against a fresh full draw after each step, so a field
  missing from the fingerprint fails a test (checked by removing fields).
- **Band:** each 480×20 band is hashed, and only bands that differ from
  last time go out over the bus. A changed reading typically resends 5 of
  16 bands.

The `xdm1241-ui` console heartbeat reports `frames=` composed and
`skipped=` per second.

## Memory on the meter

From `make portable` and a size probe, Cortex-M4 `-Os`:

| Item | Size |
| --- | --- |
| `app_t` (all app state, filter, trace) | 3.4 KB |
| render band, 480 × 20 RGB565 | 19.2 KB |
| code: app, measure, ui, drivers | ≈ 31 KB, before libc / libm |

That leaves about 25 KB of the 48 KB SRAM for the stack, the HAL, SCPI and
storage. A full 480×320 frame would be 300 KB, which is why the UI renders
in bands (`fb_init_band`).

## Keys

Logical keys follow the manual's front panel (`platform.h`). The physical bit
for each key is a bench item.

| Key | Action |
| --- | --- |
| V / I | DC ↔ AC |
| OHM | resistance → continuity → diode |
| CAP, FREQ, TEMP | select; pressed again: FREQ toggles period, TEMP toggles K/PT100 |
| UP / DOWN | manual range up / down |
| LEFT, F2 | auto ↔ manual range (*provisional*: the manual's auto/manual key is unnamed) |
| RUNSTOP | hold |
| MATH | off → min/max/avg → dBm (DCV/ACV) |
| F1 | REL (graph screen: span) |
| F2 | (graph screen: scale, auto ↔ the range's full scale) |
| F3 | numeric ↔ graph |
| F4 | clear stats and trace |
| F5 | mA/A input, frequency/period, sensor, or dBm reference, by context (graph screen: pause). No 2W/4W: the XDM1241 has no 4-wire input, and stock maps `CONF:FRES` to resistance |
| F6 | next theme |
| UTILITY | the Utility menu (below) |
| RECORD | the record screen (below) |
| DUAL | dual display: in ACV/ACI, choose FREQ (F1); then DUAL swaps, F6 exits. See `features.md` |

When the backlight has dimmed, the first key press only wakes it.

## Utility menu

`lib/app/menu.c`. UP/DOWN or F1/F2 pick a row, LEFT/RIGHT or F3/F4 change it,
UTILITY, F6 or RUN/STOP close it. A function key closes it and does its job.
Every change applies at once and is saved like any other setting.

| Row | Values | Source |
| --- | --- | --- |
| BACKLIGHT | 10-100% in 10s | stock System menu (stock's own table never goes below 31%) |
| DIM AFTER | never, 1, 5, 15, 30 min; dims to 10% | ours (wishlist: power) |
| BEEPER | on / off | stock Beeper menu |
| CONT THRESHOLD | 5 … 1000 Ω, 9 steps; an odd value set over SCPI snaps to the next step | stock Threshold, default 50 |
| SPEED | LOW / MID / HIGH | stock Speed; same as SCPI `RATE S/M/F` |
| TEMP UNIT | C / F / K | |
| DBM REFERENCE | the 17 stock references | |
| THEME | the 4 themes | ours |
| BAUD | 9600 … 115200 | stock Serial menu |
| LOAD DEFAULTS | press RIGHT twice; moving away cancels | stock Default |
| VERSION, SERIAL NUM | read-only | stock |
| CALIBRATION | FACTORY, or UNITY n BAD when the record was rejected | ours |
| BATTERY | NOT FITTED on a stock board (`platform_t.battery_mv`) | ours |

Not carried over from stock: Language (English only), Clock (no RTC use
found yet), Model, CheckSum, Lcd Test, Key Test, Data Bits / Parity / Stop
Bits (8N1 only).

## Recording

`lib/datalog` keeps one session of readings in free application flash,
0x08030000-0x0803A7FF: 21 pages of 508 float32 readings, 10,668 in all (the two pages above hold the user calibration, B31)
(stock: 1,000, in its own page we never touch). On the record screen:

| Key | Action |
| --- | --- |
| F1 | start / stop. Starting discards the previous session |
| F2 | mode: AUTO (one reading per interval) or MANUAL (one per F4 press) |
| F3 | interval: 200 ms, 1, 2, 5, 10, 30 s, 1, 5 min |
| F4 | take a point (manual) |
| F6, RECORD | back to the measurement; recording carries on (REC in the status bar) |

- **What is logged:** the value `MEAS?` would report (SI, after REL; °C/°F/K,
  period, dBm or dB as shown). Overload is +Inf, sent as `1E+9` over SCPI.
  No points are taken while HOLD is on.
- **One quantity per session.** Changing function, or anything that changes
  the unit (period, temperature unit, dBm), stops the session.
- **Power loss** keeps every reading that was completely written; the session
  reopens stopped. `test_datalog` cuts power at every flash operation across
  a page boundary to check this.
- **Timing:** points are taken at display updates, so an interval is honoured
  to within 200 ms. There is no clock, so readings carry no timestamps; the
  interval is stored with the session.
- **Cost:** one word programmed per point, plus a page erase every 508 points
  (a few tens of ms on the GD32, the only stall).
- **The screen** plots the whole session, each of 256 points the mean of its
  share of the log, overloads skipped.

Over SCPI (our own `LOG` subsystem; stock's record commands are not
recovered): `docs/scpi.md`.

## Graph screen

A trend of displayed readings. The trace holds 256 points; SPAN sets how many
readings are averaged into each, so it covers 51 s, 4 min or 26 min. SCALE
RANGE fixes the axis at ± the range's full scale (0 to full scale for
unipolar functions), and the trace is clipped to the box. PAUSE freezes the
trace while the reading in the top bar keeps updating. Changing span or
function clears the trace.

## Autoranging

Stock is reported slow everywhere (`community-research.md`). Ours
(`readout_autorange`, `range_on_sample` in `app.c`):

| Situation | What happens |
| --- | --- |
| Overload (the front end saturates at about 1.38× full scale, so the size is unknown) | straight to the top range, which measures it |
| Reading below 90% of the next range down | straight to the smallest range that holds it, with a margin of 2% plus 3 counts of the current range's resolution. From a coarse range, a tiny reading may take one more hop rather than risk overloading and bouncing back |
| Above 105% of full scale, not overloaded | one range up. Readings show to 110%, and the 5% keeps noise at full scale from flapping |
| While ranging | every sample decides; the first sample that holds is shown at once, not at the next 200 ms tick |
| Steady state | only clear cases act at once: an overload, or **two** samples in a row below 80% of the next range down. Everything else waits for the averaged update |
| A step bigger than 10% of full scale within a range | shown at once (the filter restarts on it, as stock's does) |
| Capacitance (B60) | a reading goes straight to the range that holds it (down onto 5 µF or 50 µF only below 30% of them: their cycles are seconds long); an overload goes up one. The first gate after a change is ranged on, not shown; while climbing, a range calls over range after 2-6 s instead of 4-9 s. A reading is the 1.07 s gate plus up to one charge cycle, ~21 ms per µF on 500 µF: that, not the firmware, sets how fast big parts arrive |

**Measured in the simulator** (`test/test_ranging`). Each step is repeated at
12 phases of the sample and display cycles. Since 2026-11-14 the simulator
converts at the chip's own rates: AD1 every 246 ms at R22 0x16 (DC volts,
current, ohms), 20 ms at R22 0x10 (continuity, diode, hunting), AC every
100 ms; 20 ms settle plus the discarded conversions (two, three for ohms)
per reconfiguration. The old model's flat 50 ms predicted 252 ms for DC
0 → 400 V; the meter took 1.5 s (`results/dcv-step-20261114.md`), and the
new model gives 1.69 s, so it now tracks the meter.

**Hunting with fast conversions (B10d).** While autoranging looks for a
range, DC volts, DC current and ohms convert at R22 0x10 (OSR 256, ~4 ms)
instead of 0x16 (OSR 16384, ~246 ms). The sample that settles the range
is shown at once; the meter then reconfigures at full resolution, and the
display keeps that first value until the full-resolution reading
replaces it ~0.7 s later. **Ohms** settles over a time on its high ranges
(on the meter: the reading decays from above, tau ~20 ms on 500 kΩ), so
while hunting it acts only on a fast sample that agrees with one taken at
least 40 ms earlier (0.5% + 0.2% of full scale).

| Step (worst of 12 phases) | 50 ms model | Chip rates | **Chip rates + fast hunt** |
| --- | --- | --- | --- |
| DCV 1 V → 2 V (same range) | 50 ms | 244 ms | 244 ms |
| DCV 300 mV → 3 V (up 1) | 252 ms | 1691 ms | **297 ms** |
| DCV 3 V → 300 mV (down 1) | 151 ms | 983 ms | **272 ms** |
| DCV 10 mV → 400 V (up 4) | 252 ms | 1691 ms | **297 ms** |
| DCV 400 V → 10 mV (down 4) | 151 ms | 985 ms | **236 ms** |
| DCI 100 µA ↔ 300 mA | 252 / 151 ms | 952 / 984 ms | **269 / 267 ms** |
| Ω 100 Ω → 4 MΩ | 252 ms | 2213 ms | **543 ms** |
| Ω 4 MΩ → 100 Ω | 151 ms | 1231 ms | **282 ms** |
| Ω open → 1 kΩ (touch the leads) | 151 ms | 1230 ms | **296 ms** |
| Ω 1 kΩ → open (lift the leads) | 151 ms | 1229 ms | **316 ms** |
| ACV 100 mV ↔ 300 V | 502 / 287 ms | 701 / 387 ms | 700 / 399 ms |

On the meter: DC 0 → 25 V took 1.5 s before, 0.4–0.6 s after (with the
4 µs bus, B5). Ohms is the simulator's model: the 500 kΩ settling is
measured, the 5 and 50 MΩ ranges are modelled the same until measured.

The fast setting is provisional on the DC V rows until checked on the meter
(`lib/hal/afe_table.h`, `AFE_R22_FAST`). In the simulator a fast conversion
is as quiet as a slow one; on the chip it is noisier, so the value shown
while ranging may wander in its last digits until the full-resolution
reading arrives.

The same test holds noisy inputs at every threshold for 30 s:
- At 18 display counts rms, no range changes.
- At 500 counts rms, far worse than any working meter, at most one.

**Stock, for comparison**, measured on the owner's meter (V4.3.0, Slow,
`RANGE?`/`MEAS?` logged at 35 polls/s, 2026-11-08):

| Change | Stock | Ours (simulator, chip rates) |
| --- | --- | --- |
| Ω leads lifted, 500 Ω → 50 MΩ (5 ranges) | 7–9 s: one range at a time, ~1.75 s each | 0.32 s |
| Ω probes touched, 50 MΩ → 500 Ω | ~3.5 s, in two jumps | 0.30 s |
| Ω probes touched while on 5 MΩ | ~11 s before dropping to 500 Ω | 0.3 s |
| DCV 0 → 1.9 V, same range | ~4 s, through in-between values | 0.25 s |
| DCV 1.9 → 0 V | ~1 s | 0.25 s |

Stock's times run from the first visible change, so they slightly
understate it. Its new-reading period is ~1.05 s at Slow, ~620 ms at Mid and
~300 ms at Fast (`scpi.md`).

**Still to do on the meter:** ohms lifts and touches (T-RANGE); the 5 and
50 MΩ ranges' settling with a 1 MΩ and a 10 MΩ resistor; the noise of the
fast reading.

## Decisions that are ours, not stock's

| What | Choice | Why |
| --- | --- | --- |
| Autoranging | at most two hops: overload → top range; down straight to the right range; decided per sample while ranging (see "Autoranging" below) | stock is slow (#36: "slow autoranging on all functions") |
| Settle after reconfigure | 20 ms, plus the first sample dropped | to be tuned on the bench |
| Cold-junction refresh | every 10 s | stock's cadence unknown |
| Display rate | 5 Hz | |
| Power-on | DCV auto, starting on 5 V | |

## Assumptions the bench must confirm

The simulator embodies these. Each is wrong in a way the tests would expose
once real captures exist.

- **Reading INTF clears it.** Stock never writes INTF, which suggests this.
- **Which ready flag per function.** AD1F for DC/Ω/diode/temperature, AD2F
  for AC (stock checks the AD2 flag for RMS), CTF for counters.
- **Counter semantics.** CTA + 2²⁰ is the gate at 4 MHz. CTC/CTB is the
  capacitance ratio. The simulator's choice of CTB magnitude is arbitrary.
- **`afe_configure()`**, the HY3131 register and relay settings per (function,
  range), is the big unknown. The simulator does not need it; the meter does.
  It is captured from stock by driving SCPI while logging register writes
  (`CLAUDE.md`, bench plan).

## Settings persistence

`lib/store` keeps power-on settings (function, ranges per function,
autorange, period mode, dBm reference, continuity threshold, beeper,
backlight, theme, every Utility menu row, and the recording mode and interval) in two 2 KB pages at 0x0803B800, with append-only 128-byte
records. Each record has a sequence number and CRC32, and the magic word is
written last.

- **Power cuts.** `test_store` cuts power at every operation of a save, at
  every fill level including the page-erase case: 1,600 cases. After each
  reboot the meter holds exactly the old or the new settings, and the next
  save works.
- **Wear.** Saves wait until settings have been stable for 3 s
  (`SETTINGS_SAVE_DELAY_MS`), and one page erase serves 16 saves. A failed
  save is retried 3 s later.
- **Two address guards.** `lib/store` touches only its own two pages; the HAL
  rechecks independently and never goes near 0x0803D800.
- **On the meter it is off** until `HAL_STORE_ENABLED` is set
  (`lib/hal/hal_pins.h`), after the v3 dump shows those pages are unused.

The record only ever grows (`settings.c` lists the layout). An older, shorter
record loads, and fields it lacks keep their defaults.

## Not yet built

- Capacitance ranges 5–6 (5 mF, 50 mF). The charge-slope arithmetic exists
  (`meas_cap_slope`), but where stock gets the charge count on range 5 is not
  recovered.
- Dual display.
- More than one recording session, and wrap-around (oldest page reused).
- Wiring SPEED to the AFE. It is recorded and saved; which HY3131 register it
  changes (AD1OSR, 0x22, is the guess) is shown by `capture-afe.py`, whose
  default plan now steps RATE S/M/F on DCV and ACV.
