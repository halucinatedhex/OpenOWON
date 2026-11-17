# Calibration — where it lives and how it is applied

Recovered offline from the v2.1.2 image, 2026-11-07. Addresses are v2.x;
re-derive them against a v3.x dump. The *structure* should carry over.

## Correction: calibration is not at 0x0803D800

Earlier notes put calibration in the 6004-byte record at 0x0803D800. **That
record is the 1000-point data log** (see `settings-storage.md`). Calibration is
in the 1392-byte record at **0x0803F800**, which was previously called
"settings". The functions at 0x0800D090 / 0x0800D31C that were suggested as
starting points are log code.

The safety rule does not change: nothing may erase pages at or above
0x0803D800. All three records are up there.

## The two tables

Inside the factory record (flash 0x0803F800, RAM 0x20001A48):

| Offset | Shape | Encoding | Calibration-screen label |
| --- | --- | --- | --- |
| **+0x1F8** | `int32 coef[11][10]` | gain × 1,000,000 (nominal 1000000) | `coef` |
| **+0x3B0** | `int32 zero[11][10]` | offset × 1,000,000, in display units* | `zero` |

```c
coef = *(int32_t *)(rec + 0x1F8 + func * 0x28 + range * 4);
zero = *(int32_t *)(rec + 0x3B0 + func * 0x28 + range * 4);
```

`func` is the function code at measurement-state `+0x11` (state struct at RAM
0x20003768). `range` is that function's current range at state
`+0x2B + func`. Each function gets 10 slots of 4 bytes, and 11 functions × 0x28
= 0x1B8, so `coef` ends exactly where `zero` begins and `zero` ends at 0x568.

\* Capacitance (`func` 8) scales its `zero` by 10,000, not 1,000,000, and uses
the sub-range at state `+0x33` rather than `+0x2B + func`.

Some slots are read under other names, so not every slot is a plain gain:

| Offset | Slot it aliases | Read by | Note |
| --- | --- | --- | --- |
| +0x22C | coef f1 r3 | cal screen | ACV range 4 special case |
| +0x248–+0x284 | coef f2/f3 | `FUN_08008EE0` | derived values; also written there. **Not understood** |
| +0x298 + n·4 | coef f4 | cal screen | resistance, by sub-index at state `+0x2F` |

### Other calibration fields

| Offset | Type | Use |
| --- | --- | --- |
| +0x566 + range·2 | int16 | `ACC`, shown for ACV ranges 1 and 2 (`%03d`) |
| +0x56C | int32 | sanity-checked and defaulted (0x69E) by `FUN_08008EE0` |
| +0x14, +0x133 | strings | identity/model text; `+0x78` selects which one is shown |
| +0x28 | 20 bytes | sent ahead of the record by the serial dump |

## How it is applied — DC voltage (all functions: `measurement.md`)

`FUN_08009F3E` is the per-function dispatcher. It switches on `func`, and
function 0 (DCV) calls **`FUN_08008CCE(func, range)`**:

```c
if (!ad1_ready()) return;                    /* HY3131 reg 10, bit 2      */
gain   = (double)((float)coef / 1e6f);
offset = (range <= 1) ? (double)((float)zero / 1e6f) : 0;
mag    = ad1_read(&neg);                     /* 24-bit, TWO's complement  */
if (range == 2)  v = (float)mag / 8388606.0f * 7.51667f;
else             v = (float)mag / 8388606.0f * 1.36667f * FS[func][range];
reading = (float)((double)v * gain - (neg ? -offset : offset));
if (neg) reading = -reading;                 /* == signed_v * gain - offset */
state[0x430] = reading;
```

So **reading = v × coef/10⁶ − zero/10⁶**, with the offset applied only on the
50 mV and 500 mV ranges. `FS` is the full-scale table below.

Ranges 0–1 read in mV and ranges 2–5 in V, matching how `FS` is expressed.

## Full-scale table — `.data`, RAM 0x2000146E

`int16 FS[func][7]`, stride 0xE. Read from the decompressed `.data`
(`scripts/unpack-data.py`). **Validated against the manual's specification
table** (user manual p. 37):

| func | Values | Manual |
| --- | --- | --- |
| 0 DCV | 50, 500, 5, 50, 500, 1000 | 50 mV, 500 mV, 5 V, 50 V, 500 V, 1000 V |
| 1 ACV | 500, 5, 50, 500, 750 | 500 mV – 750 V |
| 2 DCI | 500, 5, 50, 500, 5, 10 | 500 µA, 5000 µA, 50 mA, 500 mA, 5 A, 10 A |
| 3 ACI | 500, 5, 50, 500, 5, 10 | 500 µA – 10 A |
| 4 Ω 2W | 500, 5, 50, 500, 5, 50, 100 | 500 Ω – 50 MΩ (7th entry unlisted) |
| 5 Ω 4W | as 4 | |
| 6 | 1100 ×7 | continuity, 1000 Ω (+10%) |
| 7 | 3 ×7 | diode, 3.0000 V |
| 8 Cap | 50, 500, 5, 50, 500, 5, 50 | 50 nF – 50 mF |
| 9 | as 8 | |

## How it is written — the calibration routine

The calibration step lives in `FUN_08005700` (it also handles other UI state;
how it is entered is not yet traced). For the current
(func, range) it computes a new `coef` from the old one, the full-scale value
and the current reading (state `+0x5C`), then multiplies by 10⁶, adds 0.5,
converts to int32 and stores it. It then clamps:

| Case | Allowed `coef` range |
| --- | --- |
| default | 800,000 – 1,200,000 (gain 0.8 – 1.2) |
| DCI / ACI (func 2, 3) | up to 1,300,000 |
| capacitance ranges 4–6 | 600,000 – 1,400,000 |

`FUN_0800AABC` writes the `zero` table, including zeroing a slot.
`FUN_0800BB90` is the calibration screen: it prints `zero` and `coef` as raw
integers (`%07d`, `%08d` when negative) and `temp` / `G` for temperature.

## No checksum

Boot (`FUN_0800457C`) loads all three records and validates **only** the
168-byte preferences record, against a magic word. The factory record is used
as-is, with no checksum, magic or range check at load. Clamping happens only
when the calibration routine writes a value. A replacement firmware should add
its own check rather than trust a blank (0xFFFFFFFF) table.

## Soft-float helpers (armcc fplib)

Identified by their exponent arithmetic; needed to read any of the above.

| Address | Function |
| --- | --- |
| 0x08012D00 | `__aeabi_i2f` |
| 0x08012D12 | `__aeabi_ui2f` (was mislabelled `stock_aeabi_i2f`) |
| 0x08012D38 | `__aeabi_i2d` |
| 0x08012E2C | `__aeabi_f2d` |
| 0x08012EB4 | `__aeabi_d2f` |
| 0x08012DBE | `__aeabi_d2iz` |
| 0x0801292E | `__aeabi_fdiv` (was `stock_float_muldiv_a`) |
| 0x080128CA | `__aeabi_fmul` (was `stock_float_muldiv_b`) |
| 0x080129C2 | `__aeabi_dadd` |
| 0x08012B04 | `__aeabi_dsub` (flips the sign, then dadd) |
| 0x08012B10 | `__aeabi_dmul` |
| 0x08012BF4 | `__aeabi_ddiv` |

## Getting the real values

- **Over serial, no probe.** `stock_dump_settings_uart` sends this record, so
  `settings-dump.md` is now the cheapest way to get the calibration tables.
- **Over SWD.** `scripts/backup-calib.sh` reads 0x0803D800–0x08040000, which
  covers all three records.

## Still open

- ~~The other functions' formulas~~ — done, in `measurement.md`. Which slots
  each function borrows is tabulated there (4-wire Ω, continuity and diode
  all reuse other functions' rows).
- What `FUN_08008EE0` derives in the f2/f3 slots, and what `+0x56C` is.
- Function codes 6, 7, 9 and 10 are now tied to their handlers' behaviour
  (beep thresholds, counters, the temperature split); not yet to menu strings.
