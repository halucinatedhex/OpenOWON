# Bench test plan

How to validate the firmware on the meter, function by function, with
reference parts and instruments. Each test has an ID (`T-…`), what it needs,
the steps, and what counts as a pass. Results go in two places:

- **the numbers**, in a CSV that `scripts/bench-check.py` appends to
  (`results/bench-<date>.csv`, committed with the session): reference,
  mean, spread, error, verdict;
- **the conclusions**, in the reference doc for that subsystem
  (`docs/afe-table.md` for the front end, `docs/hardware.md` for the board),
  in the same change that moves a row into `lib/hal/afe_table.h` or ticks a
  backlog item.

Tolerances below are **provisional acceptance limits** for this project:
tight enough to catch a wrong range, row or calibration slot; not the
manufacturer's accuracy specification. Where your reference instrument is
worse than the limit, the test can only say "agrees with the reference".

## Bench session 2026-11-17: AC current, 500 mA and the A jack, results

Panel build, `DIAG:ROW ACI <r>` with `26=80`, the rows now in
`afe_table.h`. **AC current is complete: all six ranges in the app.**
High-power runs (36 VAC into a ~9.3 Ω load, about 30 W) were short
bursts: a script waited for current, read each row four times and
beeped, and the supply went off after 5-10 s.

**The reference, A jack.** The load's resistance on DC first, with the
V lead on the far side of the load from COM, so V–COM reads
**negative** and the V-input leak adds nothing: −9.934 V at 1.0635 A,
**9.341 Ω**. Then on AC, the load's AC volts on the 50 V range (PA8 off,
as the A jack's ranges): 16.56 V, so **1.773 A**. The current readings
were taken with the V lead **off**.

**The reference, 500 mA.** The 50 mA range (passed 2026-11-16) on the
same current: both ranges run with PA8 on, the same path, so they see
the same current. 9 VAC transformer through 389.4 Ω.

| Range | Current | Read | Result |
| --- | --- | --- | --- |
| 500 mA | 28.35 mA (50 mA range) | 28.29 mA | **−0.2%, pass** (6% of the range) |
| 5 A | 1.773 A | 1.7773 A (7 readings, 0.4% spread) | **+0.25%, pass** |
| 10 A | same | 1.783 A settled (1.806 → 1.782 in 4 s) | **+0.5%, pass** |

The factory gain on the A jack (×1.04711, as stock applies it) is right:
without it the 5 A range would read −4.3%. The "+5.0%" of 2026-11-16 was
against a reference read with the V lead on the transformer end.

**Why 500 mA looked −2.4%.** At ~160 mA (60.5 Ω, transformer) the
reference volts were read on ACV 50 V, **PA8 off, which opens the
50/500 mA current path** (the DIAG:VI scope trace, below): the meter
read the transformer's **unloaded** 9.952 V. V–COM through the mA jack
measures 61.38 Ω on ohms, so 9.952 V would mean 162.1 mA against
159.2 mA read; but 159.2 mA × 61.38 Ω = 9.77 V loaded, a 1.1 Ω
transformer, matching the sag the scope showed (9.76 V open, 9.50-9.67 V
loaded). A loaded 500 mA reference needs volts read with PA8 on, which
no volts range above 500 mV does; the cross-range check stands instead.
**Not checked: 500 mA above 6% of its scale.**

**Findings:**
- **The V-input leak in AC:** with the V lead across the 60.5 Ω load
  (~9.9 V AC), 500 mA read 189.2 mA against 159.2 mA off: +19%, as
  2026-11-16. On the A jack at 16.6 V AC it was within the supply's
  burst-to-burst change (1.766 vs 1.777 A). Power mode is DC only.
- **The first reading after a range change** is a settling artefact
  (57 V, 2.79 A, 36 mA ...): `DIAG:ROW` doesn't discard it; the app does.
- The 10 A range drifts down for a few seconds after selection, as does
  500 mA a little (28.37 → 28.26 mA).

## AC current card (~25 min, panel build): run 2026-11-16, results below

AC current is withheld on the meter (`hybrid_fn_avail`): its rows are
emulated from v2.1.2 (`src/afe_provisional.h`) and unchecked. DC current's
emulated rows worked unchanged on our board; AC volts' needed one change,
R26 0x88 → 0x80 (bandwidth). So each range is read twice, as emulated and
with `26=80`. Reads only: `DIAG:ROW` writes the HY3131 and relays, never
flash.

**The reference is the scope, across the load resistor.** The generator
is old: 100 Hz at the lowest, and last time it gave 880 mVpp (0.31 V RMS)
riding on ~0.44 V of DC. So no second meter is needed: the current is the
resistor's AC RMS voltage on the scope divided by its resistance, measured
first on this meter's ohms (confirmed). The scope's own error is taken out
in step 0 against this meter's AC volts (confirmed: 310 mV against the
scope's 311 on 2026-11-16). Expect agreement to ~2%, the scope's limit;
the tighter test is that adjacent ranges agree on the same current
(steps 1b, 2b, 3b), which needs no reference at all.

**Setup.** Install `xdm1241-panel`. Generator output → this meter's
**mA** jack; **COM** → the load resistor → generator ground. The
resistor is on the ground side so the scope's ground clip can sit on
generator ground (both are earthed): probe on the COM/resistor node, clip
on generator ground. Scope channel **AC-coupled** (the generator's DC
offset passes as DC current; this meter's AC reading ignores it, as on AC
volts), **Measure → Vrms**, acquire **Average 16**, the trace filling at
least half the screen height. Generator: sine, **100 Hz**, as much
amplitude as it gives. Readings below 1% of full scale show 0 (stock's
suppression).

**Sizing.** Current ≈ V<sub>gen,RMS</sub> / (R + 50 Ω generator + the
meter's shunt), so a weak generator limits the top ranges. At 0.31 V RMS:

| # | Load (measure it first) | Expect | Range (row) | Commands |
| --- | --- | --- | --- | --- |
| 0 | none: generator straight to **V** / COM | 0.31 V | AC volts | the app's AC volts (or `DIAG:ROW ACV 0 6 26=80`) against the scope's Vrms on the same node: the scope's factor *k* = meter / scope |
| 1 | 1 kΩ | ~0.29 mA | 500 µA (0) | `DIAG:ROW ACI 0 6`, then `DIAG:ROW ACI 0 6 26=80` |
| 1b | same | | 5 mA (1) | `DIAG:ROW ACI 1 6`, `... 26=80`: agrees with 1 (6% of the range) |
| 2 | 47 Ω | ~3 mA | 5 mA (1) | `DIAG:ROW ACI 1 6`, `... 26=80` |
| 2b | same | | 50 mA (2) | `DIAG:ROW ACI 2 6`, `... 26=80`: agrees with 2 |
| 3 | 1 kΩ, generator at **1 kHz** | ~0.29 mA | 500 µA (0) | both variants: 26=80 should hold; how far 88 falls |
| 4 | **transformer** (the one from AC volts), secondary through ~100 Ω ≥2 W, all floating | ~0.1 A at 9–12 VAC | 500 mA (3) | `DIAG:ROW ACI 3 6`, `... 26=80` |
| 4b | same | | 50 mA (2) | only if step 4 is under 50 mA |
| 5 | *optional*: transformer through ~22 Ω ≥5 W, lead in the **A** jack | ~0.4 A | 5 A (4), 10 A (5) | `DIAG:ROW ACI 4 6`, `ACI 5 6`, `26=80` |

On the transformer steps nothing is earthed, so the scope clip can go on
either end of the resistor. If its amplitude allows, the 47 Ω step can
also run on the 50 mA range at ~20 mA (more of the range).

**Write down, per step:** R (meter ohms), the scope's Vrms, and the six
`row ACI/r: ... = <amps> (factory cal)` lines. The reference is
I = *k* · Vrms / R. **Pass:** each range within 2% of the reference, and
the b-steps within 0.5% of their partner.

**If a range reads ~0, or a fixed multiple off:** the emulated table has
seven ACI rows for six ranges, so try the neighbouring row index on the
same current (e.g. `DIAG:ROW ACI 2 6` on the 500 mA step), and the other
PA8 state (`PA8=0` / `PA8=1`): DC current's shunt taps were found that way
(2026-11-14).

**Same session, 5 min more:** step 8 below (DC current, mA jack against
the A jack) can use the same reference: bench supply at 5 V through one
measured resistor on the ground side, the scope (DC-coupled, Vavg)
across it for I = V / R while the meter reads it on the mA jack, then on
the A jack.

**Then:** the passing rows go into `afe_table.h` as `AFE_ACI`, `afe_row()`
returns them, and `hybrid_fn_avail` offers AC current, on the mA jack only
unless step 5 passed (`hybrid_range_avail`).

## Bench session 2026-11-16 (evening): the V lead and DC current, results

App build, DC current over SCPI (`CONF:CURR:DC`, `RANGE n`, `MEAS?`), 10
readings each. Supply → 60 Ω (59.87 Ω measured) → current jack; COM →
supply −. The scope across the resistor showed the current unchanged
between steps.

| Jack, range | V lead loose | V lead on supply + (5 V) | Added |
| --- | --- | --- | --- |
| mA, 500 mA | 81.359 mA | 95.155 mA | **+13.80 mA (+17.0%)** |
| mA, 500 mA | | on supply − (= COM): 81.356 mA | none |
| A, 5 A | 83.666 mA | 86.784 mA | **+3.12 mA (+3.7%)** |

The jacks agree with the lead loose (within ~0.4%, the scope's
resolution); **step 8's 95.11 / 86.65 mA were this effect exactly.**

**No load, the V lead alone** (supply + → V, − → COM; volts read by this
meter), readings in mA, the 5 A range less its 0.17 mA zero:

| V | 500 µA | 5 mA | 50 mA | 500 mA | 5 A | 10 A |
| --- | --- | --- | --- | --- | --- | --- |
| 1.0 | | | 0.00 | 0.00 | 0.00 | |
| 3.0 | | | 5.30 | 4.13 | 0.93 | |
| 4.001 | | | 11.67 | 9.10 | 2.00 | |
| 5.0 | | | 17.98 | 14.02 | 3.13 | |
| 7.000 | | | 30.36 | 23.72 | 5.34 | |
| 9.007 | | | 42.46 | 33.22 | 7.51 | |
| 10.001 | 0.00 | 0.00 | 48.40 | 37.89 | 8.55 | 8.52 |
| −5 (reversed) | | 0.00 | 0.07 | 0.05 | 0.12 | 0.12 |

- **Not a real current:** the supply read 0.00 A (10 mA steps) while the
  ranges read 8-48 mA, and the ranges disagree with each other, which
  real current never does. The V input's clamp conducts above ~2.15 V
  and upsets the measurement inside the meter.
- **Ranges:** 500 µA and 5 mA none (PA8 off); 50 / 500 mA 5-6 mA/V (PA8
  on: the relay that puts the V jack straight on the converter for the mV
  ranges); the A jack 1.09 mA/V (5 A and 10 A alike).
- **Fit, the A jack:** 1.0928 mA/V × (V − 2.146 V), residuals ≤ 0.04 mA
  over 3-10 V. Only positive voltages. Not measured above 10 V (the
  supply's limit).
- With the load on and off the added current agreed within 0.2 mA: it adds
  to the real current, independent of it.

**Fix (installed 2026-11-16):** `platform_t.vi_leak`; power mode subtracts
it, using the volts it reads, from the current, the watts and the
totals. **Checked:** 5 V, 60 Ω, A jack: DC current with the lead loose
83.585 mA; power mode with the lead on supply + 83.59 mA (six readings
83.565-83.622), 5.0076 V, 0.4186 W. Uncorrected it would read 86.7 mA.
The 50 / 500 mA ranges need no correction in power mode: they pair only
with the mV ranges, which show over range long before 2.15 V.

**Plain DC (and AC) current with a voltage on the V jack is still read
as is**: there's no voltage reading to correct with. The user guide says
to leave the V jack empty in current modes.

## Bench session 2026-11-16 (afternoon): AC current, results

Panel build, `DIAG:ROW ACI <r> 6` and `... 26=80`. **The reference: this
meter's own AC volts across a measured resistor in the loop**, read in the
volts range that sets the relays as the current range does (500 µA / 5 mA
↔ 5 V, PA8 off; 50 / 500 mA ↔ 500 mV, PA8 on; A jack ↔ 50 V), so the
current path is the same during both reads. The scope (Rigol DS1052E)
proved too coarse as a reference (three digits, ±3%: it put a good range
2% off) and served only to confirm the current hadn't changed between
reads. Generator: 100 Hz, a 0-3.3 V sine (1.154 V RMS AC on 1.63 V DC),
DC blocked by 1000 µF in series. Transformer: 9 VAC, 250 mA.

| Range | Load | Reference | Read (emulated / R26 0x80) | Result |
| --- | --- | --- | --- | --- |
| 500 µA | 3615.0 Ω | 0.28721 mA | +0.39% / +0.41% | **pass** |
| 5 mA | same | same | +0.44% / +0.50% (6% of range) | **pass**; agrees with 500 µA to 0.1% |
| 5 mA | 389.4 Ω | 1.4569 mA | −0.11% / −0.05% | **pass** |
| 50 mA | same | 1.6638 mA | −0.21% / −0.32% | **pass** (the 500 mV reference read 0.648 V, 30% past its scale) |
| 50 mA, 1 kHz | same | 1.6589 mA | +0.21% / +0.29% | **pass**; R26 makes no difference at 1 kHz |
| 500 mA | 60 Ω, transformer | ~0.160 A (scope, ±1%) | −2.4% | **not offered**; reference wrong, passed 2026-11-17 |
| 5 A | same, A jack | 0.1597 A | +5.0% | **not offered**: +0.3% without the factory gain (×1.04711); reference wrong (V lead on), passed 2026-11-17 |
| 10 A | same | same | +6.9% (1.6% of range: noise) | **not offered**; passed 2026-11-17 |

**In the app (installed 2026-11-16):** AC current on the mA jack, 500 µA,
5 mA and 50 mA, with R26 0x80. Autoranging stops at 50 mA with an
overload; the A jack (INPUT) is greyed. Stock's arithmetic applies the
factory gain the same way (0x080094A8), so the 500 mA and A-jack errors
are in the emulated v2.1.2 rows: the factory calibrated V4.3's rows.
Finishing them needs a reference at 10-50% of each range (a second meter
in series, or V/R with a 0.1-1 Ω shunt read on the 500 mV range) and
then either the right rows or our own calibration (B59).

**Findings:**
- **A voltage on the V jack adds to the AC current reading.** With the V
  lead on the transformer end of the load (9.5 V across V–COM), the 500 mA
  range read 0.1854 A; with the lead off, 0.1558 A, at the same current.
  At ~1 V (step 1) it made no difference (0.28825 → 0.28833 mA). In normal
  use only one input carries a lead. Power mode: the card above.
- **Relay trials with current flowing:** `PA13=1` on the 5 A row with
  160 mA through the mA jack read 6.5 A. Nothing was damaged (every mA
  range re-read as before, the 500 mA raw ratio 1/107 → 1/108), but
  change relays only with the circuit unpowered.
- Three "330 Ω" parts read 360.2 Ω, 389.4 Ω (a 390, confirmed on two
  ohms ranges), and a "220 Ω" was the 60 Ω: measure every part first.

## Bench session 2026-11-16: results

| # | Step | Result |
| --- | --- | --- |
| 1 | Clock (B33) | **pass**: 3 s fast after 22.7 h, the meter off overnight (~35 ppm) |
| 2 | Speed (B11) | **pass**: 4.0 / 8.0 / 16.2 samples a second at S / M / F (console `samples=`; statistics count display updates, every 200 ms, so they can't show it) |
| 3 | Fast recording (B24) | **pass**: 574 readings in ~10 s at 15 ms; keys responsive throughout. Owner: grey the settings keys while recording (done) |
| 4 | Battery (B47) | 4.20 V on the owner's meter, 4.20 V 100% on the gauge: same charge as the scale point, so the % curve is still to check part way down a discharge |
| 5 | AC + DC dual (B23) | **pass**: generator 100 Hz, 880 mVpp (311 mV RMS) on ~0.44 V DC (scope: 538 mV RMS total): 310 mV AC, 438.89 mV DC |
| 6 | Power card (B21), panel build | step 1 **pass**; step 2 **fail** (found in step 7), below |
| 7 | Power in the app | arithmetic right (52.5 Ω on 5 V, 500 mA range: 0.4761 W; 1.4044 mWh and 0.2805 mAh after 10.62 s = W × t, I × t), **but a scope across the load showed the current dropping to zero at every relay click**. The meter's current readings (1,000 polls) never saw it: it isn't measuring current while it reads volts; the supply's display was too slow. Fixed: 50 / 500 mA pair with 50 / 500 mV only, enforced in acquisition (`vi_avail`, `pair_ok`); above 5 mA use the A jack. Rechecked on the meter: 500 mA range, no clicks, scope flat, volts over range; **A jack (5 A range): no clicks, 0.4329 W = 4.996 V × 86.65 mA, Wh and Ah = W × t** |
| 8 | Current, the two jacks disagree? | the same "58 Ω" load read 95.11 mA on the 500 mA range (step 6) and 86.65 mA on the 5 A range; the resistor measures 60 Ω (≤ 83 mA at 5 V); the supply shows 0.08 A. Both reads were with the V lead live on supply +: **the V-lead effect** (evening session, above), exactly; with the lead loose the jacks agree |

**Power card** (wiring low side: supply − → COM; + → load → mA; VΩ → +; 5.0 V):

| Load | Command | Result |
| --- | --- | --- |
| 4.7 kΩ | `DIAG:VI 1 2 5` | no relay; I 1.04641 mA, every "I again" within 0.002%; V 4.99991–4.99998 V |
| ~52.5 Ω | `DIAG:VI 3 2 5` | PA8 clicks each switch; I 95.11 mA within 0.005% before and after each volts read, V 4.998 V. **Not a pass:** "I again" is read after PA8 is back, so it can't see the break, and the supply's display was too slow. The scope (step 7) showed the current path open during every volts read. **Watch the load with a scope** for any test of switching under current |

A first run with the VΩ lead unconnected read ~0 V: the volts are only
the supply's when that lead is on supply +.

Open, for later: the leak above 10 V (extrapolated); battery % part way down; AC current's 500 mA and A-jack ranges; the 1–4 mA gap between the current pairs; the 5 V AC range's
roll-off; the 50 MΩ range's ~1.5 s settling.

Capacitance 5/50 mF **done 2026-11-15** in the app: 1000 µF → 1.085 mF
(±0.3%), 18 mF → 17.64 mF (±0.45%); autoranging from 500 µF reaches 50 mF
in ~10 s. Both parts are unmeasured electrolytics: accuracy is still open.

## Where things stand (2026-11-17)

| Test | State |
| --- | --- |
| Session 0 | used every session; boot is "fast" (B39: not yet timed) |
| T-UI-FONT, T-UI-SPEED, T-UI-FEATURES | not run |
| T-BEEP-1/2/3 | T-BEEP-3 (continuity latency): owner's check by hand, "working great"; no timed measurement. T-BEEP-1/2 not run |
| T-DCV | **`bench-check.py dcv` run 2026-11-14** (`results/dcv-20261114.md`): short passes; 1.5–30 V agree with a third meter to its resolution (the handheld first used reads ~1% low), within 0.03% of the supply's set points; ranges agree to 0.04%; 500 mV range agrees to the third meter's 1 mV. 50 mV range, 10 MΩ input, step time to do |
| T-OHM | spot check only: 1.5 kΩ reads 1495.3 / 1497.5 Ω on two ranges |
| T-CONT, T-DIODE | **done 2026-11-14.** Diodes (`results/diode-ohm-20261114.md`): 1N4148 0.567 V, Schottky 0.15 V, red LED 1.49 V, reversed open. Continuity checked by the owner by hand: threshold and beep working |
| Backlight | **confirmed 2026-11-14** by the owner: dims from the Utility menu (PA11 PWM) |
| T-TEMP | **type K complete 2026-11-14** (owner's call, no ice): cold junction 20.05 °C and probe in air 19.35 °C vs a 20 °C thermometer; fingers → past 30 °C; in the app 19.9–20.1 °C. PT100/PT1000 enabled on the confirmed ohms rows (500 Ω / 5 kΩ, as stock's PT100 row); live (thermocouple wire reads over range); not yet checked with a resistor |
| T-CAP | **ranges 0–4 (50 nF–500 µF) done** with 11 LCR-measured parts; **5 mF and 50 mF done 2026-11-15**: 1000 µF reads 1.085 mF ±0.3%, 18 mF reads 17.64 mF ±0.45% (`features.md`) |
| T-ACV | **60 Hz on all five ranges done** (agree within 0.25%; transformer within 0.2%); band characterised on 5 V and 50 V. **Open: the 5 V range rolls off above ~100 Hz** |
| T-FREQ | **60 Hz–10 kHz done** (~0.01%); 10 Hz, 100 kHz, 1 MHz and duty not run |
| T-DUAL | **AC + DC done 2026-11-16** in the app; DC V + DC I (power) done |
| T-DCI | **2026-11-14, panel build** (`results/dci-20261114.md`): rows configure and read on ranges 0–3, but against V/R (third meter's DC volts across a measured 4.7 kΩ) ranges 0–1 read **−0.9%** and 2–3 **+1.3%**: a 2.2% split between the two shunt taps, probably a v2/v3 routing difference. With the factory mapping the low pair is right at 82 µA (−0.03%) and the high pair at 41 mA (+0.1%); an unexplained 2% gap between the pairs at 1–4 mA. **A jack** ~1 A: 5 A 1.0084, 10 A 1.0089 (supply 1.001). **All six ranges in the app** |
| T-ACI | **done 2026-11-17, all six ranges in the app**: 500 µA–50 mA within 0.4% (2026-11-16), 500 mA −0.2% against 50 mA at 28 mA, 5 A +0.25% and 10 A +0.5% at 1.77 A. Open: 500 mA above 6% of its scale |
| T-SPEED, T-RANGE | DC 0 → 25 V, 2026-11-14 (`results/dcv-step-20261114.md`): 1.5 s at first; **0.4–0.6 s** after the fast hunt (B10d) and the 4 µs bus (B5): passes, just. **Ohms 2026-11-14** (`results/ohm-step-20261114.md`): lift ~0.3 s, touch 0.3–0.8 s (stock 7–9 s / ~3.5 s); 1 MΩ alike; 9.2 MΩ on 50 MΩ ~1.5 s (source settling, no ranging involved) |
| T-STORE | **part 1 passed 2026-11-14** (first build with flash storage on); power cuts skipped: the XDM1241 runs on its battery |
| T-REC | **passed 2026-11-14**: the session survived the power key; whether that key cuts power at once is not yet known |

Physical testing is paused; resume at [Order of play](#order-of-play).

## Ground rules

- **No flash writes outside our app slot.** Installs go through
  `scripts/serial-update.py` (area 5 only, refuses anything that would
  reach 0x0803D800). Never send stock's service commands (`CLAUDE.md`).
- **Low voltage first.** Every voltage test starts at ≤ 30 V DC / ≤ 12 V AC
  from a current-limited source. Mains is out of scope for this plan.
- **Current tests use a current-limited supply and a series resistor**, the
  limit set below the range's fuse. The 10 A jack is last, and only after
  the mA path passes.
- **Discharge capacitors** (short them through a resistor) before connecting
  them in capacitance mode.
- **One change at a time.** A test that fails is repeated once before
  anything is changed; record both runs.
- Functions still simulated on the meter (`docs/afe-table.md`) are tested
  through the panel build's `DIAG:` commands first. Only when a row passes
  there does it go into `afe_table.h`, and then the app is tested with
  `bench-check.py`.

## Equipment

| Item | Used by | Minimum | Better |
| --- | --- | --- | --- |
| Reference DMM | everything | 4½-digit handheld | 6½-digit bench meter |
| Resistors | T-OHM, T-CONT, T-TEMP | 1% metal film: 10, 47, 56, 100 Ω; 1, 10, 100 kΩ; 1, 10 MΩ | 0.1% set, measured on the reference meter |
| Capacitors | T-CAP | film/C0G 1 nF, 100 nF, 1 µF; electrolytic 10 µF, 100 µF | an LCR meter to give each its value |
| Diodes | T-DIODE | 1N4148, 1N400x, a Schottky, red/green/blue LEDs | a zener (for the threshold edge) |
| DC source | T-DCV, T-DCI | AA and 9 V cells | current-limited bench supply, 0–30 V |
| AC source | T-ACV, T-FREQ | function generator (sine/square, 10 Hz–100 kHz) | low-voltage transformer (6–12 VAC) |
| Thermocouple | T-TEMP | type-K bead probe, ice water, a kettle | a thermometer you trust to 0.2 °C |
| Sound level | T-BEEP | a phone SPL app (relative only) | a sound-level meter, A-weighted |
| Timing | T-SPEED, T-CONT | the console's timestamps | a scope on the beeper or a logic analyser |
| USB serial adaptor | all | the one already wired to the meter | |

## Session 0: install and smoke (every session)

**T-SETUP**
1. `make check` on the laptop: everything must pass.
2. Install the build under test (`docs/install.md`), e.g.
   `./scripts/serial-update.py <port> .pio/build/xdm1241-ui/firmware.bin --stock-gone --reset`.
3. `./scripts/bench-check.py <port> scpi`: identity, error queue and a
   reading answer. **Pass:** four PASS lines.
4. Power-cycle; the meter boots to the measure screen within ~6 s and
   remembers the last function. Note the boot time (B39).

## The panel and keys

**T-UI-FONT (B41).** Install each font variant in turn
(`build/review/` has them rendered). For each, at arm's length and at 30°
off-axis, in a lit room: can you read the soft-key labels, the footer and
the menu without effort? Rank them. **Outcome:** the chosen variant becomes
the default.

**T-UI-SPEED (B56).** On `xdm1241-ui` the console prints one line a second:
`steps= step_max= ... frames= skipped=`.
1. Steady DC input, no keys for 10 s. **Pass:** `frames=` 0 or 1 a second,
   `skipped=` about 5; `step_max` under 20 ms.
2. A changing input (a slowly turned pot). **Pass:** frames ≈ 5 a second,
   step_max within 10% of the old 84 ms or better.
3. Press keys rapidly: every press clicks and acts (no lost keys).

**T-UI-FEATURES.** With a DC source:
- Bar graph (B37): menu BAR GRAPH on; 0 V, +half scale, −half scale, over
  range. **Pass:** bar at centre, half right, half left, full and in the
  warning colour.
- Auto-hold (B15): menu AUTO HOLD on; touch 3.3 V, hold 2 s, lift.
  **Pass:** one beep, AHOLD shown, reading stays 3.3 V after lifting;
  a second point (1.5 V) replaces it with a second beep; `MEAS?` follows
  the live input throughout.
- Diode threshold (B18): see T-DIODE.

## The beeper (B57)

**T-BEEP-1: loudness against pitch and volume.** Meter on the bench, SPL
meter 30 cm in front, room quiet.
`./scripts/bench-check.py <port> beep` plays 1–5 kHz at 100/50/25% and
asks for each dB(A). **Outcome:** a curve. Expected: a peak near the
transducer's resonance (stock chose 3.04 kHz), and volume steps that are
audible. If 25% is no quieter than 100%, the duty range is too narrow and
the volume scale needs rework (e.g. a lower top duty or fewer steps).
The menu's pitch steps and default come from this curve.

**T-BEEP-2: envelopes.** For each style (CLASSIC, SOFT, CHIRP) and a CUSTOM
one (attack 30 ms, release 150 ms, glide 30%): `SYST:BEEP:PLAY TEST`, then
`PLAY CLICK`, then continuity with the leads shorted and released.
**Pass:** each is audibly what it claims (no clicks or pops at the start of
SOFT; CHIRP rises; continuity fades out over the release). With a scope on
the beeper drive (PB11 side of the transistor) the envelope is visible as
a changing duty. Repeat SOFT while the screen redraws continuously (a
changing reading): the envelope runs at main-loop rate, so check it is
not stepped audibly (if it is: move the envelope into the 1 ms tick).

**T-BEEP-3: latency.** Continuity with a short: the time from touching the
probes to the tone. Scope on the beeper and on the probe tips (or record
both with a phone at 240 fps). **Pass:** under 100 ms (stock is a known
complaint, #152).

## DC voltage (real front end)

**T-DCV.** `./scripts/bench-check.py <port> dcv`: short, 1.5 V cell, 9 V,
12 V and 30 V from the supply, the cell reversed. Read each reference on
your reference meter at the same moment.
**Pass:** within ±(0.1% + 2 counts of the range) of the reference.
Then by hand:
- each range manually (`RANGE 1`…`RANGE 6` over SCPI, or DOWN/UP):
  50 mV and 500 mV with a divider from the cell (e.g. 1 MΩ + 10 kΩ),
  the rest with the supply. **Pass:** the same limit on every range.
- input resistance: 10 V through a 10 MΩ resistor. **Pass:** reads about
  5 V (10 MΩ input), confirming the divider.
- autorange step: switch 0.2 V → 25 V; note the time to a settled reading
  (B10e).

## Resistance, continuity, diode (real front end)

**T-OHM.** `bench-check.py <port> ohm`: short, then 10 Ω to 10 MΩ.
**Pass:** within ±(0.2% + lead resistance) to 100 kΩ, ±0.5% at 1 MΩ, ±2% at
10 MΩ. Note the time to a settled reading on each (the ohms source charging
the cable; ACQ_DISCARD_OHM and B10e).

**T-CONT.** `bench-check.py <port> cont`, then by ear: 47 Ω beeps, 56 Ω
silent at the 50 Ω threshold; set the threshold to 10 Ω and check 8.2/12 Ω.
Latency: T-BEEP-3.

**T-DIODE.** `bench-check.py <port> diode`: short, 1N4148, Schottky, LEDs.
**Pass:** within 2% + 10 mV of the reference meter's diode reading (both
meters' test currents differ: note them; expect a few tens of mV for LEDs).
Threshold (B18): at 0.7 V the 1N4148 beeps once, the red LED does not; at
2.0 V the red LED beeps once. Reverse a diode: no beep, reads open.

## Temperature (first real use)

**T-TEMP-CJ.** Panel build, `DIAG:TEMP` with the leads open, meter at room
temperature for 30 min. Compare the cold junction with a thermometer at the
input terminals. **Pass:** within 1 °C. Then hold a finger on the terminals
for a minute and watch it rise. Record the offset; if there is one, B29 (CJ
offset) gets its value from here.

**T-TEMP-K.** A type-K probe in the V/COM jacks: room air, stirred ice
water, boiling water (check your altitude's boiling point). Compare with
your thermometer. **Pass:** within 1.5 °C (probe tolerance dominates: a
class 2 probe is ±2.2 °C). `TEMP:TC:EMF?` gives the raw EMF to compare
against the type-K table.

**T-TEMP-PT100.** Resistors standing in for a PT100: 100.0 Ω (0 °C),
138.5 Ω (100 °C), 175.9 Ω (200 °C). **Pass:** within 1 °C plus the lead
resistance error (2.6 °C per ohm: measure your leads on T-OHM and
subtract, or use B27's null when it lands).

## Capacitance

**Done for ranges 0–4 (50 nF–500 µF), 2026-11-13.** The 4–5× error was
the counter gate ending after ~70 ms; reading CTA back after starting the
counter gives the full gate, and the factory arithmetic then holds
(`docs/afe-table.md`, raw logs in `results/cap-20261113/`). Against 11
parts measured on a hobby LCR meter (neither is a precision reference):
open leads ~0; film parts −1.7 to −3.5%; electrolytics within the parts'
uncertainty; 7.55 nF +8%, which looks like the part.

**T-CAP-1 (the register matrix)** is retired: the variants it compared were
all measuring the short gate.

**Readings creep up after connecting (2026-11-14).** A 100 nF part on
the app (4 µs bus): 88.7 nF first, 95.4 after ~20 s, 100.7 after ~60 s,
levelling at ~102.2 nF after ~2 min. Yesterday's raw panel-build readings
of the 114.4 nF film part did the same (93.4 → 97.0 nF over six readings,
CTB falling 80 → 77), so it is neither the display filter nor the bus
speed. Yesterday's numbers were taken in the first ~10 s, so the film
parts' −1.7 to −3.5% may be partly this. Cause open (dielectric
absorption under the measuring bias, or drift in the meter's charging
circuit): compare a settled reading against the LCR meter. A 10 µF
electrolytic (yesterday's 10.4 µF part, LCR) does **not** creep: 9.875–9.94
µF flat for 3 min on the 50 µF range, the same as yesterday's 9.884 µF raw
at the old bus speed. So the 4 µs bus leaves capacitance unchanged, and
the creep looks like the small part's dielectric, not the meter. The first
reading took 17 s (one range at a time from 500 nF).

**T-CAP-2, still open.**
- Repeat the film parts against a better reference (a 0.1% or 1% part,
  or a calibrated LCR meter) to tell a real −2% from the reference's error.
  **Pass:** within ±2% + 5 counts.
- Ranges 5 and 6 (5 mF, 50 mF): stock reads these with the charge-slope
  path (`stock_ad1_read_scale`), not the counters, so they need their own
  rows. A 4.7 mF and a 22 mF electrolytic.
- `bench-check.py <port> cap` on the app for the record.

## AC voltage and frequency

**In the app since 2026-11-13** (`results/acv-freq-20261113.md`,
`docs/afe-table.md`). AC waits for RMSF; R26 is 0x80 (AD2 at 62.5 kHz) on
every range; frequency counts SYSCLK at 4.0 MHz with a 0xF00000 gate preset.
Ranges are 0–4 = 500 mV, 5 V, 50 V, 500 V, 750 V (`DIAG:ROW ACV 1` is 5 V,
`ACV 2` is 50 V).

**T-ACV.** Done: all five ranges agree at 60 Hz within 0.25%, a 32.4 Vpp
transformer within 0.2% of the scope. Still to run:
- 10 kHz and 20 kHz on the 500 mV and 50 V ranges, to put a number on the
  band (#288: −3 dB at 8.4 kHz on stock).
- 1.000 Vrms checked on a reference meter at 50 Hz and 1 kHz.
  **Pass:** within ±(1% + 5 counts).
- **The 5 V range (open):** −3.4% at 1 kHz, −11.5% at 3 kHz against the
  50 V range. The app warns above 500 Hz on that range. Each candidate tap
  setting (R2D, R2E) needs readings at two frequencies with `DIAG:ROW ACV 1
  n RR=HH`.

**T-FREQ.** Done: 60 Hz–10 kHz to ~0.01%. Still to run: 10 Hz, 100 kHz,
1 MHz (square, 3 Vpp), and duty 25/50/75% at 1 kHz (computed from CTC,
unchecked). **Pass:** frequency within the generator's accuracy +
1 count; duty within ±1%.

**T-DUAL.** AC volts with frequency (DUAL) in the app: both readings as in
T-ACV and T-FREQ at once; note the relay clicks as it alternates.

## Current (last)

**T-DCI.** Current is still simulated in the app (no rows in
`lib/hal/afe_table.h`): first the recovered rows on the panel build, then
the app. Ready to run (2026-11-14): `DIAG:ROW DCI <r> [n]` applies the
emulated row and its relays (`src/afe_provisional.h`) and converts AD1 with
`meas_dci` through the factory calibration.

| Range | Full scale | Jack | PA8 (recovered) |
| --- | --- | --- | --- |
| 0 | 500 µA | mA | off |
| 1 | 5 mA | mA | off |
| 2 | 50 mA | mA | on |
| 3 | 500 mA | mA | on |
| 4 | 5 A | 10 A | off |
| 5 | 10 A | 10 A | off |

Set-up: supply in series with a resistor, the meter's mA (or 10 A) jack and
COM, and the reference meter on its current range in series too. Set the
supply's **current limit before connecting**. Install `xdm1241-panel`.

1. **≈100 µA** (5 V through 47 kΩ, limit 50 mA): `DIAG:ROW DCI 0 8`, then
   `DCI 1 8` (the same current on the next range: the two should agree).
2. **≈1 mA** (5 V through 4.7 kΩ, limit 50 mA): `DCI 1 8`, `DCI 2 8`.
3. **≈100 mA** (5 V through 47 Ω, ≥1 W; limit 200 mA): `DCI 3 8`.
   Range 2 would overload at 100 mA: skip it here.
4. **Open circuit**, leads out: `DCI 0 8` reads ~0 (offset).
5. **10 A jack, ≈1 A** (≈1.2 V across a 1 Ω, ≥5 W resistor; limit
   1.5 A), 10 s at a time: `DCI 4 8`, `DCI 5 8`.

**Pass:** within ±(0.5% + 5 counts) of the reference; neighbouring ranges
agree on the same current. **Reference:** the third meter's mA ranges read ~1% low;
use V/R instead (its DC volts across a resistor measured on our ohms
range), which is good to ~0.2%. Switching ranges does not change the
current (the shunts stay in series; checked 2026-11-14). A row that reads wrong by a fixed factor, or not
at all, is a routing difference on the v3 board (as capacitance's R24
was): try `RR=HH` overrides against the XDM2041 capture
(`docs/afe-table.md`) before giving up on it. Rows that pass move into
`afe_table.h`; then `bench-check.py <port> dci` on the app. The 10 A duty
warning (B19) is checked only in the simulator.

**T-ACI.** As T-DCI with a transformer and a resistor; skip if no isolated
low-voltage AC source.

## Speed and ranging (B10d, B11, B12)

**T-SPEED.** DCV 5 V, steady input. For RATE S, M, F: timestamps of 50
`MEAS?` readings (the console logs AD1 events), giving conversions per
second. **Outcome:** the per-OSR timing B11 needs; compare with stock's S
~1/s, M ~1.6/s, F ~3.3/s.
**T-RANGE.** Step inputs (0.1 → 25 V, 25 → 0.1 V, open → 1 kΩ → 10 MΩ):
time from the step to a settled, correctly ranged reading. **Pass:** DC
under 600 ms, ohms under 1.5 s (`test_ranging`, at the chip's conversion rates with the fast hunt, predicts 362 ms for DC and 2.2 s for ohms).

## Recording and settings

**T-REC.** Record 2 minutes at 200 ms, switch off with the power key
mid-recording, power up. **Passed 2026-11-14** (1 minute at 200 ms,
temperature): the session was there after power-on. Still to check: the
last point against the switch-off time, which says whether the power key
cuts power at once (a real power-cut test) or lets the firmware finish. **Pass:** the session is there up to the cut, LOG:DATA? reads it, a new
session starts cleanly. Repeat with RECORD manual.
**T-STORE.** Change five settings (theme, beep pitch, speed, bar
graph, diode threshold) and the function, wait 3 s, power-cycle. **Pass:**
all kept. **Passed 2026-11-14** (theme amber, pitch 3500 Hz, bar graph on,
diode 2.0 V, speed fast, ohms), on the first build that stores to flash:
until then `xdm1241-ui` had `HAL_STORE_ENABLED`/`HAL_LOG_ENABLED` off and
kept everything in RAM.

Power cuts (pull the power within 0.5 s of a change, ten times; the meter
always boots with the old or the new value): **skipped**. The XDM1241 runs
on its battery, so the supply can't simply be pulled; it would mean opening
the battery compartment each time. The host power-cut fuzz in `lib/store`
and `lib/datalog` covers the logic; an XDM1041 (mains only) could run it.

## Order of play

1. T-DCV, T-OHM, T-CONT, T-DIODE with `bench-check.py` against a reference
   meter (the first full record of the confirmed functions).
2. T-UI-FONT, T-UI-SPEED, T-UI-FEATURES, T-BEEP-1/2/3 (settles the font and
   the beeper defaults; T-BEEP-3 needs a scope or a 240 fps phone).
3. T-TEMP (panel build `DIAG:TEMP`, then rows into the app).
4. The rest of T-ACV / T-FREQ / T-DUAL, the 5 V range, T-CAP-2.
5. T-SPEED, T-RANGE.
6. T-DCI, then T-ACI (current last).
