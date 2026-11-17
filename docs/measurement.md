# The measurement path — every function

Recovered offline from the v2.1.2 image, 2026-11-07. Addresses are v2.x.
Calibration storage and encoding are in `calibration.md`; this page is the
arithmetic from HY3131 registers to the displayed number.

**Implemented** in `lib/measure/meas.c` and `meas_temp.c`, tested by
`test/test_meas` and `test/test_temp`. The tests check the range tables
against the manual, the transcribed constants against the stock image's
bytes, and temperature against NIST/IEC reference points and against stock's
own tables (which must differ only at the two errors below). Outputs are SI,
not stock's per-range units; `meas_unit_exp()` converts.

**Confidence key.** Plain statements are read from the code. *Inferred* marks
anything taken from naming, units or context rather than from the code.

## Shape of one update

```
stock_meas_dispatch (0x08009F3E)        switch on state+0x11 (function code)
  └─ per-function handler                registers → value, calibration applied
       └─ FUN_08008B5E  (filter)         rolling buffer, reset on a step
FUN_0800A30A  (finalise)                 average → temperature → REL → dB/dBm
```

| State field (RAM 0x20003768) | Meaning |
| --- | --- |
| +0x11 | function code (table below) |
| +0x2B + func | current range of that function |
| +0x33 | capacitance sub-range |
| +0x35 | temperature sensor: 0 = type K, 1 = PT100 |
| +0x50 / +0x54 | newest / previous filtered sample |
| +0x5C | finalised reading (after averaging, REL) |
| +0x60 | secondary: duty cycle % (frequency) |
| +0x64 | temperature result |
| +0x68 | temperature over-range flag |
| +0x98 … | sample buffer, up to 200 floats; count at +0x3E0 |
| +0x420 / +0x428 | cold-junction ΔV / cold-junction °C |

## Function codes

From the dispatcher, cross-checked against the full-scale table and the manual.

| Code | Function | Handler | Calibration slots used |
| --- | --- | --- | --- |
| 0 | DC V | `stock_meas_dcv` 0x08008CCE | coef/zero f0 |
| 1 | AC V | 0x080093EA | coef f1 (750 V shares 500 V) |
| 2 | DC I | 0x080092AC | coef f2 |
| 3 | AC I | 0x080094A8 | coef f3 |
| 4 | Ω 2-wire | 0x08009566 | coef/zero f4 |
| 5 | Ω 4-wire | 0x08009566 | **coef f4** (shared), no zero |
| 6 | continuity | 0x08009E76 | coef/zero f4 r0 (500 Ω) |
| 7 | diode | 0x08009DA2 | coef/zero **f0 r2** (5 V DCV) |
| 8 | capacitance | 0x0800971E | coef f8, zero f8 (÷10⁴) |
| 9 | frequency / period | 0x08009B4E | none |
| 10 | temperature | 0x08009F22 → DCV or Ω | via the borrowed function |

ACV and ACI also run 0x080091A0 after the main handler, which measures the
signal's frequency for the secondary display.

Below, `FS` is `int16 FS[func][range]` at RAM 0x2000146E (`calibration.md`),
`coef` and `zero` are the factory-record int32 tables, and `FULL = 8388606.0`.

## AD1 sign handling

Two conventions coexist in stock:

| Reader | Used by | Negative magnitude |
| --- | --- | --- |
| `stock_ad1_read_mag` 0x08008C50 | DCV, DCI, Ω, continuity | `(~raw + 1) & 0xFFFFFF` — **two's complement** |
| inline, 0x08009DA2 | diode | `~raw & 0xFFFFFF` — one's complement |
| `stock_ad1_read_scale` 0x0800889C | large capacitance | `~raw & 0xFFFFFF` — one's complement |

The datasheet specifies two's complement, and so does `hy3131_sext24()` in
`lib/afe/`. The one's-complement paths are one LSB off on negative input;
reproduce them only for bit-exact parity.

Readers return the magnitude plus a sign flag. Every handler works on the
magnitude and restores the sign at the end; where an offset is subtracted it
is negated first, so the net effect is `signed_value × gain − offset`.

## DC voltage — func 0

```c
gain = coef / 1e6;  off = (range <= 1) ? zero / 1e6 : 0;
if (range == 2) v = mag / FULL * 7.51667;                 /* 5 V          */
else            v = mag / FULL * 1.36667 * FS[0][range];  /* 50 mV…1000 V */
reading = v * gain - off;
```

Units follow FS: mV on ranges 0–1, V on 2–5. The offset is applied only on the
50 mV and 500 mV ranges.

## DC current — func 2

```c
reading = mag / FULL * 1.36667 * FS[2][range] * coef / 1e6;     /* no offset */
```

## AC voltage and AC current — func 1, 3 (true RMS)

The RMS register is HY3131 0x09–0x0D (logical index 3), read as 5 bytes and
assembled as a 38-bit integer (the top byte is masked to 6 bits):

```c
rms_sq  = b0 + b1·2⁸ + b2·2¹⁶ + b3·2²⁴ + (b4 & 0x3F)·2³²;   /* 0x08009108 */
reading = sqrt((double)rms_sq) / 262143.0 * K[range] * coef / 1e6;
```

| Range | ACV `K` (0x0801DDD0) | ACI `K` (0x0801DDF8) |
| --- | --- | --- |
| 0 | 2460.0 | 2460.0 |
| 1 | 27.06 | 24.6 |
| 2 | 246.0 | 246.0 |
| 3 | 2460.0 | 2460.0 |
| 4 | 2460.0 (gain from range 3) | 24.6 |
| 5 | — | 246.0 |

262143 = 2¹⁸ − 1. **ACI only:** outside calibration mode, readings below 1% of
full scale (`FS × 0.01`) are forced to 0. There is no AC offset term.

The handlers run only when the AD2 ready bit, latched into RAM 0x20001428
bit 1 by `stock_hy_data_ready`, is set.

## Resistance — func 4, 5

```c
gain = coef[f4][range] / 1e6;              /* 2W and 4W share this row     */
off  = (func == 4) ? zero[f4][range] / 1e6 : 0;
v    = mag / FULL * R[range] * gain;
if (range <= 2)  reading = v - off * (1 + Ca[range] * v / FS[4][range]);
/* the divisor is a literal (500, 5, 50) equal to FS[4][range] */
else             reading = v - off;
if (!cal_mode && reading < 0) reading = 0;
```

| Range | `R` (0x0801DE28) | `Ca` | Ca as a fraction |
| --- | --- | --- | --- |
| 0 (500 Ω) | 1111.11 | 0.2340425532 | 11/47 |
| 1 (5 kΩ) | 11.11 | 0.4656488550 | 61/131 |
| 2 (50 kΩ) | 111.11 | 0.5175438596 | 59/114 |
| 3 (500 kΩ) | 1111.11 | — | |
| 4 (5 MΩ) | 6.67 | — | |
| 5 (50 MΩ) | 66.7 | — | |

The `Ca` term scales the stored offset with the reading. *Inferred:* it
corrects a ratiometric offset in the low ranges' reference network. The
unscaled value is also kept at state +0x58.

## Continuity — func 6

```c
v = mag / FULL * 1000.0 / 0.9 * coef[f4][0] / 1e6 - zero[f4][0] / 1e6;
if (negative) v = 0;
beep = prefs.beeper (+0x12) == 1 && v < prefs.threshold (+0x14, default 50 Ω);
/* compare direction inferred: the float-compare flags are lost in the decompile */
```

1000/0.9 = 1111.11, the same full-scale constant as the 500 Ω range.

## Diode — func 7

```c
gain = coef[f0][2] / 1e6;          /* borrowed: 5 V DCV                  */
off  = zero[f0][2] / 1e9;          /* note 1e9, not 1e6                  */
v = mag / FULL * 1.23 * 5.5555558 / 10.0 * 81.2890778 * gain - off;
beep = prefs.beeper == 1 && v <= 0.7;
```

AD1 is read inline here with one's-complement sign handling. The four constants
multiply to 55.5475.

## Capacitance — func 8

Ranges 0–4 (50 nF – 500 µF) use the HY3131 counters; ranges 5–6 (5 mF, 50 mF)
use a charge-slope measurement.

**Ranges 0–4.** Wait for CTF (reg 0x1E bit 0), read CTB (index 8) and CTC
(index 7):

```c
r = CTC / CTB;
C = r / D[range] * coef / 1e6 - zero / 1e4;
```

| Range | Divisor chain | `D` |
| --- | --- | --- |
| 0, 1 | ÷ 300000 × 1000 | 300 |
| 2 | ÷ 80000 ÷ 3 | 240000 |
| 3 | ÷ 8000 ÷ 3 | 24000 |
| 4 | ÷ 7394 | 7394 |

**Ranges 5–6.** `stock_ad1_read_scale` stores the AD1 voltage as
`raw / FULL × 1.2 / 1.8` at state +0x7C, the previous sample is at +0x78, and
+0x80 is a charge-time count (forced to 2000 on range 6):

```c
dV = v[+0x7C] - v[+0x78];
C  = ldexp((double)count, 1) / 1000.0 / dV * 0.00018 * 1000.0 * coef / 1e6 - zero / 1e4;
if (v[+0x7C] >= 0.666666 && !state[0x15]) C = 0;     /* over-range */
```

**How stock runs it (recovered 2026-11-15 from the v2.1.2 disassembly):**
- A periodic tick, the TIMER2 (0x40000400) interrupt at 0x08004786, calls
  0x0800A8F2, which counts ticks in state +0x84. On capacitance ranges 5
  and 6 it alternates a phase flag at state +0x6A (set by the routine at
  0x08008098, which also switches the PA taps for the phase):

  | Range | phase ≠ 0: read after | phase 0: read after | count +0x80 |
  | --- | --- | --- | --- |
  | 5 (5 mF) | 200 ticks | 400 ticks | **277**, set by the tick |
  | 6 (50 mF) | 2000 ticks | 2500 ticks | **2000**, forced in `stock_meas_cap` |

- Each read (0x0800880A) writes HY3131 index 22 and 24 to zero: **R2A and
  R2C**, the PA switch registers, so the current source is disconnected;
  +0x440 is set, and `stock_meas_cap` then takes AD1 (`stock_ad1_read_scale`)
  as the new +0x7C, the previous one becoming +0x78.
- So: charge from the ~0.18 mA source for a fixed time, read the voltage,
  and C = I·t/ΔV from consecutive readings, with the count standing in for
  t (C uses 2 × count / 1000).
- TIMER2 is set up at 0x0800622A with **prescaler 63, reload 999**: a
  1 kHz tick at a 64 MHz timer clock (~1.1 kHz at 72 MHz). The counts are
  not the tick intervals (277 against 200 ticks on range 5, 2000 against
  2000 on range 6), so the factory calibration absorbs the difference: a
  reimplementation should keep stock's intervals and counts as they are.
- **The two phases (0x08008098, read 2026-11-15)** edit the HY3131 shadow
  (indexed by register address) and set R2A |= 0x80, R2C |= 0x08 (source
  back on) in both:
  - phase flag ≠ 0 → **phase A**: R2F low nibble F, R31 low nibble A,
    R20 bits 4–7 = 0x70 class, R21 = 0xDE. This is the emulated CAP/6 row.
  - phase flag 0 → **phase B**: R2F low nibble 6, R31 low nibble 1 (R20,
    R21 as left).
- **On our meter, 18 mF electrolytic, `DIAG:CAPSLOPE` (panel build):**
  - **phase B charges at constant current**: v = raw / 2²³ × 1.2 / 1.8
    rose 0.144 → 0.565 in 22 s, straight (0.01891 /s, every second within
    ±3%). Stock's formula with count = time in ms gives
    **C = 0.36 / (dv/dt) = 19.0 mF** before the factory coefficient;
  - **phase A pulls it back** to a rest level near v = 0.097, like an RC
    (τ ≈ 4–5 s with 18 mF): it resets the capacitor between charges;
  - **source off (R2A = R2C = 0)**: holds, −20 µv/s. Stock reads here.
  - The first AD1 result after configuring is stale (filter fill): discard.
- So stock: A for 2.5 s (rest), read; B for 2 s (charge), read; C from
  the rise across B. Over-range at v ≥ 0.6667 (B's ramp would reach it
  from rest in ~30 s at 18 mF: the 50 mF range's whole span is one
  2 s charge of ≥ 0.0144 v, i.e. C ≤ 50 mF).
- **1000 µF electrolytic, same day:** rest in A at v 0.0978, then B ramps
  0.0798 per 246 ms conversion (0.3223 /s) until AD1 clips at v = 0.6667
  (8388606 counts: stock's over-range is AD1 full scale). Range 5's row
  becomes range 6's after the first phase A (0x08008098 rewrites R20/R21),
  so the two ranges differ only in timing.
- **The factory record agrees with stock's counts and intervals** (our
  meter's CAP slots: range 5 coef 0.710255, zero −40; range 6 coef
  0.987914, zero 281):
  - range 6: count 2000 over a 2000 ms charge; 18 mF → 19.03 × 0.9879 −
    0.028 = **18.77 mF**;
  - range 5: count 277 over a 200 ms charge (1.385× too big) and a coef of
    0.710: 0.710 × 1.385 = 0.984, the same gain as range 6. 1000 µF →
    0.0997 / 0.0645 × 0.7103 + 0.004 = **1.10 mF**.
  So the tick is 1 ms on the V4 meter and the formula stands as written.
- **Still unknown:** how stock treats the first readings after a range
  change and how it switches between ranges 4, 5 and 6 (autorange).

*Inferred:* 0.18 mA charging current, with C = I·t/ΔV. The 1.2/1.8 factor here
is the one that matches Digilent's HY3131 driver (`125e-1 / 1.8 / 8388608`),
which is evidence that this AD1 path's front-end scaling is common to both.

Outside calibration mode, readings below 1% of full scale are shown as 0 (same
compare-and-zero pattern as ACI, which was confirmed in disassembly).

## Frequency and period — func 9

Wait for CTF, read CTA (index 9), CTB (8) and CTC (7):

```c
gate = (CTA + 0x100000) / 4e6;         /* seconds; 4 MHz reference      */
f    = CTB / gate;                     /* Hz                            */
duty = CTC / (CTA + 0x100000) * 100;   /* %, to state+0x60             */
```

REL (if enabled for func 9) is subtracted before unit scaling. Then:

| Mode (prefs +0x9A) | Condition | Shown as | Range code |
| --- | --- | --- | --- |
| frequency | f < 1000 | Hz | 0 |
| | f < 1e6 | kHz | 1 |
| | otherwise | MHz | 2 |
| period, p = 1/f | p ≥ 1 | s | 3 |
| | p ≥ 0.001 | ms | 4 |
| | otherwise | µs | 5 |

*Inferred:* the kHz/MHz divisors (1000, 1e6) are taken from the thresholds; the
register argument is lost in the decompile. The unit-exponent table below
independently gives 0, 3, 6, 0, −3, −6 for these six codes.

The secondary-display frequency for ACV/ACI (0x080091A0) uses the same gate
formula and stores to state +8.

## Temperature — func 10

Range 0 measures the thermocouple on **DCV 500 mV**; otherwise PT100 on
**Ω 500 Ω**. The conversion happens in the finaliser.

### Cold junction — 0x08008DD4

An on-chip ΔVbe sensor: two DCV 500 mV readings under two bias settings
(state +0x448 = 1, then 2), separated by a busy-wait of 1000 units
(`FUN_08004A4A`, 0x6137 loop iterations each; *inferred* ≈ 1 ms at 72 MHz):

```c
dV   = (V2 - V1) / 2;                       /* ldexpf(…, -1); to +0x420  */
T_cj = dV / G - 273.15 - 16.0;              /* °C, to +0x428             */
```

`G` comes from factory record +0x56C / 10⁴. It must be in 0.10–0.18, otherwise
it falls back to 0.16946 (stored 1694); handled in 0x08008EE0. It appears on
the temperature calibration screen as `G`.

### Conversion — 0x0800A1C8

Two tables in flash, pointed to from `.data` at 0x20001438 / 0x2000143C:

| Sensor | Table | Entries | Span | Units |
| --- | --- | --- | --- | --- |
| type K | 0x0801DE58 | 165 | −270 … 1370 °C, 10 °C steps | mV |
| PT100 | 0x0801E0EC | 91 | −50 … 850 °C, 10 °C steps | Ω |

```c
if (K) x += E_K(T_cj);           /* forward-interpolate K table, 0x0800A14E */
i = search(table, x);            /* tbl[i] <= x < tbl[i+1]; 0x0800A048      */
T = start + 10*i + 10*(x - tbl[i]) / (tbl[i+1] - tbl[i]);
if (T > max || T < min) overload = 1;
T = (unit == °F) ? T*1.8 + 32 : (unit == K) ? T + 273.15 : T;  /* prefs +0x99 */
```

### Errors in the stock tables

Checked against the standard equations, not by eye:

- **PT100 at 290 °C reads 204.48 Ω; IEC 60751 gives 208.48 Ω.** The other 90
  entries match Callendar–Van Dusen (A = 3.9083e-3, B = −5.775e-7,
  C = −4.183e-12) within 0.006 Ω. A stock meter should read wrong between
  280 °C and 300 °C on PT100.
  At ~0.385 Ω/°C that is a **≈ 10 °C** error.
- **Type K at 850 °C reads 35.324 mV; expected ≈ 35.313 mV** (+10.9 µV,
  **≈ 0.27 °C**). Two independent checks agree: the NIST ITS-90 polynomial,
  and the largest isolated spike in the residual's second difference.
- Against NIST the rest of the K table is: −1.4 … +0.4 µV up to 130 °C;
  −4.9 … −0.5 µV from 140 to 670 °C, where stock's values are rounded
  coarser (20.640 for 20.644); ±1.6 µV to 900 °C; then a smooth drift to
  −13 µV (≈ 0.35 °C) at 1370 °C. The drift is consistent with an older
  reference table, not a typo.

A replacement firmware should regenerate both tables from the equations.

## Filter — 0x08008B5E

Every handler ends here.

- Keeps up to 200 samples at state +0x98 (count at +0x3E0).
- If `|new − previous| > FS × 0.1` (rate settings 0–2), all 200 slots are
  overwritten with the new value. A step resets the average immediately.
  (*Inferred* direction: the compare result is lost in the decompile, but a
  reset on a large step is the only reading that makes the buffer useful.)
- Every 20 calls it also measures the update rate (samples/s, to RAM
  0x2000142C + 8).

## Finaliser — 0x0800A30A

1. Average the buffered samples, reset the count, store to +0x5C.
2. Temperature: if func 10 and prefs +0x98 ∈ {0, 1}, convert as above.
3. Low-reading zeroing for capacitance / ACV / ACI, as noted per function.
4. **REL**, if prefs +0x48 + func == 1 and not in calibration mode:
   `reading −= rel / 10^exp[func][range]`, where `rel` is the float at
   prefs +0x54 + func·4 in **SI base units**, and `exp` is the display unit's
   decimal exponent (`.data` 0x20001614):

   | func | exp by range |
   | --- | --- |
   | 0 DCV | −3, −3, 0, 0, 0, 0 |
   | 1 ACV | −3, 0, 0, 0, 0 |
   | 2, 3 current | −6, −3, −3, −3, 0, 0 |
   | 4, 5 Ω | 0, 3, 3, 3, 6, 6 |
   | 8 cap | −9, −9, −6, −6, −6, −3, −3 |
   | 9 freq/period | 0, 3, 6, 0, −3, −6 |

   Capacitance applies REL once per reading (state +0x94 latch).
5. Over-range check (`FUN_08009FE8`, to state +0x69).

## dB and dBm

Active when prefs +0x36 == 1, for DCV and ACV only (0x0800E1F2). The mV ranges
(DCV 0–1, ACV 0) are divided by 1000 first, so `v` is always in volts:

```c
p   = v * v / Rref * 1000;  if (p == 0) p = 1e-9;   /* mW            */
dBm = 10 * log10(p);                             /* to prefs +0x44       */
dB  = dBm - prefs.dB_ref (+0x3C) / 100;          /* to prefs +0x40       */
```

prefs +0x37 selects dBm (1) or dB (0). Logged points carry range code 0xEF
for dBm and 0xFF for dB.

`Rref` is `uint16[17]` at 0x0801E298, indexed by prefs +0x38: 50, 75, 93, 110,
124, 125, 135, 150, 250, 300, 500, 600, 800, 900, 1000, 1200, 8000 Ω.

## Math helpers

The soft-float names are in `calibration.md`. Two more:

| Address | Function | Evidence |
| --- | --- | --- |
| 0x080145C4 | `log10` | its literal pool holds log10(e) and log10(2) |
| 0x080146A8 | `pow` | *inferred*: 2.3 KB, called as `f(10.0, exp)` |
| 0x08015080 | `sqrt` | wraps the core at 0x08013238; used on the RMS sum |
| 0x080129AA | `ldexpf` | adds its argument to the exponent field |
| 0x08012CD2 | `ldexp` | same, for doubles |

## Still open

- The over-range rules in `FUN_08009FE8`.
- What sets the capacitance charge count (+0x80) on range 5.
- The −16 °C term in the cold junction. It is plausibly a board-level offset;
  check it against a thermometer on the bench.
- The AFE register writes per function and range, which are not part of this
  arithmetic (`analog-frontend.md`).
