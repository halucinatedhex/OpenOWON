# The ATtiny202 power controller: replacing its firmware

**Status: plan.** Nothing here has been done to the meter. Backlog B49.

U6 on the digital board (`XDM1041_dig_v1.7`, top edge beside header CO2) is
a Microchip ATtiny202: 8 pins, 2 KB flash, 128 B RAM, a 10-bit ADC. Owners
identify it as the soft-power controller (#186, #221). It is not updatable
over the meter's serial port, only through its own one-wire programming
pin (UPDI).

## Why

| Today (stock) | With our ATtiny firmware | Backlog |
| --- | --- | --- |
| The power button must be held 2–3 s | a short press turns the meter on; holding still turns it off, so a brush of the button can't switch it off mid-measurement | B49 |
| About 0.5–1 mA drawn while "off", which flattens the cells in months | microamps when off, if the drain is the ATtiny (its power-down mode draws under 1 µA) | B46 |
| Nothing measures the battery | the ATtiny's ADC reads VBAT and reports it to the GD32, possibly with no other hardware mod | B47 |
| No auto power-off | the GD32 asks the ATtiny to switch off after an idle time | B48 |

Faster power-on has two halves:
1. **The press-and-hold:** that's the ATtiny, fixed by this work.
2. **Boot time:** stock takes about 5 s. Our firmware starts quickly, but
   OWON's bootloader runs first and probably waits briefly for an update
   handshake. Measure it at M1; the bootloader stays either way.

piit79 found the same split (#221): after rewiring the button, start-up was
"still slow because of a delay elsewhere".

## Two things that shape the approach

**1. OWON's ATtiny firmware probably can't be backed up.**
- Production parts are normally locked.
- UPDI then allows only a chip erase, which wipes the original for good.
- So reprogramming in place is one-way: if our firmware misbehaves, OWON's
  is gone. The meter could always be reprogrammed again, but never back to
  stock.

**The safe route is to swap the chip, not overwrite it:**
1. Buy spare ATtiny202 parts (about $0.60 each, `ATTINY202-SSN`, SOIC-8).
2. Program one on a breakout first.
3. Desolder U6 and fit the new one.
4. Keep OWON's original chip; it goes back in if wanted.

First, *try* reading the fitted chip over UPDI (step 3 below). Reading is
safe. If it turns out unlocked, back it up and reflash in place instead.

**2. We don't know what its pins do yet.** The only evidence is piit79's
mod (#221): they cut the button's trace to the ATtiny and used PA1 (high:
slow start, low: immediate shutdown) and PA2 (the button). Those are probably
the ATtiny's own pin names. Several things are still open:
- **Which pin drives `PWR_EN`.**
- **Whether it senses VBAT.**
- **Whether it talks to the GD32,** and if so how. Header CO2 (`scl sda gnd
  3v`) suggests I2C, but on this chip I2C uses PA1/PA2, the same pins piit79
  describes as power and button. One of these readings is wrong. The bench
  settles it.

ATtiny202 SOIC-8 pins, for reference: 1 VDD · 2 PA6 · 3 PA7 · 4 PA1 · 5 PA2
· 6 PA0/UPDI · 7 PA3 · 8 GND.

## Plan

**1. Watch it, without changing anything** (bench day 2, alongside B46/B48).
- **Equipment:** a logic analyser or a scope on U6's pins, plus CO2.
- **Capture:** cold power-on (button press), a normal power-off (hold),
  and charging.
- **Measure:** the off-state current at the battery, with U6's VDD lifted or
  not.
- **Outcome:** which pin is the button, which is PWR_EN, whether VBAT
  reaches an ADC pin, whether anything talks on CO2, and where the off-drain
  comes from.

**2. Get the UPDI pin.**
- Pin 6. The unlabelled pads J1/J2 beside U6 may be UPDI and VDD/GND test
  points; check with a continuity tester.
- **Programmer:** a USB-serial adapter plus a 4.7 kΩ resistor (the
  "SerialUPDI" arrangement), driven by `pymcuprog` or `avrdude -c serialupdi`.
  A J-Link can't program UPDI.

**3. Read only:** `pymcuprog ping`, then read the fuses, the lock byte and,
if unlocked, the flash. This step decides between flashing in place and
swapping the chip.

**4. Write our firmware.** It's small:

| State | Behaviour |
| --- | --- |
| Off | power-down sleep, woken by a pin change on the button |
| Press | latch PWR_EN at once |
| On | watch for a long press (e.g. 1.5 s), then release PWR_EN; serve the GD32: battery voltage, "power off now", perhaps a heartbeat |

- **Same pattern as the rest of the project:** a pure state machine, tested
  on the host, behind a thin hardware layer.
- **Build:** PlatformIO's `atmelmegaavr` platform.
- **What it reproduces:** whatever stock's handshake with the GD32 turns out
  to be, so stock firmware still works with it.

**5. Prove it on a spare chip** on a breakout, then swap U6 (or flash in
place if step 3 allows), and keep the original.

## Risks

| Risk | Mitigation |
| --- | --- |
| The meter doesn't power on | it's a hardware fault, not a brick: UPDI reprogramming, or the original chip back in |
| Battery protection or charging depends on the ATtiny | step 1 checks this. Don't replace it until the charging path is understood |
| The GD32 firmware expects a handshake our ATtiny lacks | step 1 records stock's traffic; we reproduce it |
| SOIC-8 rework damages pads | ordinary SOIC soldering; practise on a scrap board first |
