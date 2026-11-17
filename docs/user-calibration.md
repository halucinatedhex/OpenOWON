# User calibration (B31)

**Status (2026-11-12): built over SCPI, simulator-tested** (`lib/app/ucal.c`,
`test_user_calibration`). The owner chose storage A. Still open: a panel
flow, and a calibrate mode in `bench-check.py` (decisions 2 and 3 below).

The meter's factory calibration (`docs/calibration.md`) is read and never
written. This adds an owner's correction **on top of it**, stored
in our own flash, never in the factory record, and removable at any time.
It is for an owner with a better reference than the factory used, or a
meter that has drifted, and for the functions we may end up calibrating
ourselves (capacitance, if B16's rows need a new scale).

## What is corrected

Per function and range, a gain and an offset applied after the factory
arithmetic:

    shown = factory_value * (1 + g) + o

`g` in parts per million (int32), `o` in the range's counts (int32), both
zero by default. Applied in `lib/measure` where the factory gain is, so
everything downstream (ranging, REL, statistics, recording, SCPI) sees the
corrected value. Temperature is not calibrated this way (its inputs are
DC volts and ohms, which are).

## Where it is stored

The settings record (112 bytes) cannot hold ~60 slots × 8 bytes. Options:

| Option | Cost | Notes |
| --- | --- | --- |
| **A. Sparse entries in a second store**: two pages taken from the top of the data log (23 → 21 pages, 11,684 → 10,668 readings), 512-byte records of `{fn, range, g, o}`, up to 49 slots | 8.7% of the log | Same power-cut-safe `lib/store` code (now with a per-store record size). **Chosen** (0x0803A800-0x0803B7FF) |
| B. Sparse entries in the settings record | none | At most ~8 slots; enough for "my DC 5 V and 50 V ranges", not more |
| C. A separate page outside the app slot | – | No such page: everything above 0x0803D800 is OWON's and never touched |

## How it is done

Over SCPI first (precise, scriptable, works with `bench-check.py`), the
panel later:

| Step | Command (ours) | Does |
| --- | --- | --- |
| choose | `CONF:VOLT:DC 5` (any function/range, manual) | the slot is the current function and range |
| zero | `CAL:USER:ZERO` | with the input shorted: the offset that makes it read 0 (averaged over 16 readings; refused if the reading is above 1% of the range) |
| gain | `CAL:USER:GAIN <value>` | with a reference near full scale applied: the gain that makes it read `<value>` (refused if the correction is more than ±2%, which would mean a wrong range or a wiring error) |
| wait | `CAL:USER:STAT?` | `BUSY` while averaging, then `DONE`, or `ERR,<n>`: -2 too large for a zero, -3 reference too small, -4 out of bounds, -5 the range moved, -6 storage failed |
| check | `CAL:USER?` | `g,o` (ppm) for the slot; `CAL:USER:LIST?` every non-zero slot as `VOLT/3:g:o;...` |
| undo | `CAL:USER:CLEAR` / `CAL:USER:CLEAR ALL` | back to factory |

The status bar shows **UCAL** whenever the current range has a user
correction, and the Utility menu's CALIBRATION row says FACTORY or
FACTORY + USER n.

## Safety

- The factory record is never written, so clearing the user correction
  always returns the meter to its factory accuracy.
- Corrections are bounded (±2% gain, ±1% of range offset): a bad reference
  or a wrong range is refused, not stored.
- A torn write leaves the previous correction set (`lib/store`).

## Decisions

1. Storage: **A, chosen 2026-11-12**.
2. Is the panel flow wanted, or SCPI only?
3. Should `bench-check.py` gain a `--calibrate` mode that runs zero and
   gain for a list of ranges against a reference?
