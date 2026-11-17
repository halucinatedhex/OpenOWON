# Wishlist: beyond stock parity, scoped

> **The prioritised list is now `backlog.md`.** This page keeps the
> feasibility reasoning, and the summary below says where each wish ended
> up. Most of it is done and running on our meter.

The replacement plan (`replacement-plan.md`) targets functional equivalence
first. This page lists what we want beyond that, grouped, with an honest
feasibility rating for each. Complaints about stock come from user reports
and reviews and are unverified until checked on the meter.

**Feasibility:**

| Rating | Meaning |
| --- | --- |
| **High** | firmware only, and we know how |
| **Medium** | needs bench data or a known hardware mod, with no open technical doubt |
| **Low** | limited by the analog design, or depends on unknowns that could rule it out |

**Needs:**

| Code | Meaning |
| --- | --- |
| FW | firmware only |
| Bench | data from the meter (J-Link day 1/2) |
| Mod | hardware modification |
| Analog | set by the analog circuit, not the code |

## Summary

| # | Wish | Group | Needs | Feasibility | Status (2026-11-17) |
| --- | --- | --- | --- | --- | --- |
| 1 | Trend graph | Display | FW | High | **done** |
| 2 | Histogram, bar graph, big-digit view | Display | FW | High | **done** (big digits moot: the reading already uses stock's 96 px digits); mini graph under the reading too |
| 3 | Backlight levels and dim timeout | Display / Power | FW | High | **done, on the meter** |
| 4 | Faster display and key response | Display | FW (+ Bench to measure) | High | **done**: a change shows within one conversion |
| 5 | Clean fonts, no truncated text | Display | FW | High | **done, on the meter** |
| 6 | Direct keys and soft-key profiles | Display | FW | High | partly: direct keys done; profiles open (B40) |
| 7 | Faster autoranging | Measurement | FW + Bench | High | **done, on the meter**: ohms ~0.3 s lifted (stock 7–9 s); per-range settle times open (B10e) |
| 8 | Dual display, same input (e.g. ACV + DCV) | Measurement | FW + Bench | Medium-High | **done, on the meter** |
| 9 | Dual display, independent (e.g. DCV + DCI) | Measurement | FW + Bench | Medium | **done, on the meter**: power mode, no mod |
| 10 | Better low-end capacitance | Measurement | FW + Bench, partly Analog | Medium | open (B17): below 500 pF; all seven ranges, to 50 mF, on the meter |
| 11 | Wider AC bandwidth | Measurement | Analog | Low | won't fix; **characterised and flagged** (B20) |
| 12 | Speed setting really changes the ADC rate | Measurement | Bench | High | **done, on the meter**: 4 / 8 / 16 readings a second |
| 13 | Bigger internal log | Data | FW | High | **done** (10,668) |
| 14 | Timestamped logs (real-time clock) | Data | FW + Bench | High | **done, on the meter** |
| 15 | Several log sessions, wrap-around | Data | FW | High | wrap-around **done**; several sessions open (B35) |
| 16 | Standard, correct SCPI | Connectivity | FW | High | **done** |
| 17 | sigrok support | Connectivity | FW done; upstream patch | High | open (B44) |
| 18 | Survives host link drops | Connectivity | FW | High | **done** on our side |
| 19 | Firmware update over serial, no opening | Connectivity | Bench | Medium | **done**: how every install is made |
| 20 | Internal ESP32, Wi-Fi web UI | Connectivity | Mod + FW | Medium | open (B42) |
| 21 | Battery monitoring | Power | FW | High | **done, on the meter**: no mod needed (PB0) |
| 22 | Auto power-off | Power | Bench | Medium | open (B48): needs the `PWR_EN` handshake |
| 23 | Beeper frequency, volume and envelope | Display | FW + Bench | High | **done, on the meter**: seven styles, pitch, volume, envelope |

What's left is mostly hardware-gated: auto power-off and the off-state
drain (the ATtiny202, B46-B49), the ESP32 board (20), and two firmware
items, several log sessions (15) and soft-key profiles (6). The sections
below keep the original reasoning; each opens with where it ended up.

---

## Display and UI

**1. Trend graph** — *done.* Spans of 51 s, 4 min and 26 min, auto or
full-scale axis, and pause (`app.md`, Graph screen).

**2. Histogram, bar graph, big-digit view** — *done.* The bar graph, a
histogram on the graph screen, and a mini trend or histogram under the
big reading (`features.md`). The original reasoning:
- The bar graph is cheap: an analog-style bar under the reading, scaled to
  the range.
- A histogram of the current statistics run needs about 1 KB of bins and is
  drawn like the plot.
- Big digits reuse the stock font at a larger scale.
- None needs anything we don't have.

**3. Backlight levels and dim timeout** — *done.* 10-100% and a dim after
1-30 min. Stock never goes below 31%.

**4. Faster display and key response** — *done.* A big change shows
within one conversion. The original reasoning:
- Banded rendering sends only changed rows at 5 Hz. Keys are polled every
  main-loop pass, not tied to the display rate.
- What's left is measuring the real panel write time and loop latency on the
  bench.
- The parallel bus and the ADC rate set the ceiling, not the firmware.

**5. Clean fonts, no truncated text** — *done, on the meter.*
- Our own screens use the stock reading font and a 5×7 label font. Nothing is
  truncated, because every label is ours.
- Labels now use Liberation Sans (Arial metrics, as stock), anti-aliased,
  with bold labels and Title Case as stock, at stock's sizes, with real
  lower case and unit symbols (mA, kΩ, µF, °C). Stock fonts 0-3 turned out
  to hold only the reading's 23 glyphs, so they could never do labels.

**23. Beeper frequency, volume and envelope** — *done, on the meter*
(B57): pitch, volume, seven styles, attack, release and glide. The
original reasoning:
- The beeper is PB11 driven by TIMER1 CH3 PWM at ~3 kHz, stock's setting
  (`hardware.md`). Frequency is the timer period: firmware only.
- Volume is the PWM duty cycle. A buzzer driven off its resonance is quiet,
  and a duty change gives a limited range, so how much control there is
  depends on what U4 is. Measure its loudness against frequency on the bench
  first.
- Envelope: ramp the duty from the timer's update interrupt for an attack
  and decay (a softer key click, a rising continuity tone). Cheap in CPU.
- Each event (key click, continuity, diode, alarms) could get its own
  sound, chosen in the Utility menu and persisted.

**6. Direct keys and soft-key profiles** — *partly.* The keypad is
mapped and every key works; soft-key profiles are open (B40).
- REL, HOLD, manual range and min/max are already one press away.
- User-assignable soft-key profiles are firmware only.
- The physical key map comes from the bench.

## Measurement

**7. Faster autoranging** — *done, on the meter* (B10): ohms lift ~0.3 s
and touch 0.3–0.8 s against stock's 7–9 s; DC 0 → 25 V in 0.4–0.6 s.
Per-range settle times are still open (B10e). The original reasoning:
- Ours ranges up at once on overload, and down below 90% of the lower range.
- The real speed is set by settle times after each range switch, which are
  still our 20 ms guess.
- The day-2 capture shows what stock waits for. Tuning against the real
  front end should get well under stock's reported ~5 s.

**8. Dual display, same input** — *done, on the meter* (B23, 2026-11-16).
The original reasoning:
- The HY3131 runs its DC path (AD1) and its true-RMS path (AD2/RMS) at the
  same time. ACV + DCV, or AC+DC total, from one connection should need no
  input switching.
- Stock already pairs ACV with frequency this way.
- Confirm in the capture that one configuration enables both paths.

**9. Dual display, independent functions** — *done for DCV + DCI, on the
meter, with no mod* (B21, 2026-11-16): power mode alternates the two rows.
The reasoning that got there:

DCV + DCI specifically:
- **The chip is built for it.** The HY3131 has three ADCs with separate input
  multiplexers (datasheet §12.4):
  - AD1 can take FB, SENSE or any of PB<0>–PB<7>.
  - AD2 and AD3 take OP1O, OP2O, PB<4> or PB<7>, against RLU, AGND, PB<3>
    or PB<5>.
  - The datasheet calls AD3 the "Power ΣΔ ADC". The register `SPHACAL`
    phase-aligns AD2 and AD3 "to measure Power", i.e. voltage and current at
    the same instant.
- So voltage on one ADC and the shunt on another can run at the same time,
  with no switching between readings, if the board's routing allows it.
- Volts × amps also gives a power (W) reading for free.

Whether the routing allows it is a bench question with three outcomes:
1. **No mod.** The DCV and DCI configurations use different HY3131 pins (one
   reachable by AD1, the other by AD2/AD3), and no relay changes state between
   them. The day-2 capture answers this directly: compare `SAD1FP`/`SAD1FN`,
   the PA/PB switches and any relay drive between DCV and DCI.
2. **A small mod.** The shunt's sense node only reaches the chip through a
   relay or switch that DCV mode opens. Then add a series resistor from the
   sense node to a free PB<4> or PB<7>. It's one resistor and a wire on the
   analog side, and needs a gain calibrated in firmware against the normal DCI
   reading.
3. **Not worth it.** A relay must change state between the two. Swapping it
   at display rate would wear it out in days (about 100,000 operations in
   14 hours at 2 Hz).
   - A second isolated ADC would avoid that, but every isolation channel is
     already used. It would also need its own isolated supply.
   - That is a redesign, not a mod.

**B21, what the recovered table says (2026-11-15, offline).** Comparing
stock's DC volts and DC current rows (`afe-table.md`, emulated from v2.1.2;
the DCV and DCI rows are confirmed on our meter):

| | DCV 50 / 500 mV | DCV 5 V-1000 V | DCI 500 µA, 5 mA, 5 A, 10 A | DCI 50, 500 mA |
| --- | --- | --- | --- | --- |
| PA8 | **on** | off | off | **on** |
| PA13 | off | off | off | off |
| R24 (AD1 input) | 0x35 / 0x85 | 0x01 / 0x09 | 0x35 / 0xB5 | 0x35 / 0xB5 |
| R2A-R2E (divider taps) | 0 (R2E 08) | set | 0 | 0 |
| R33 | 0xA4 | 0xA4 | 0xA8 | 0xA8 |

- **Both functions read on AD1.** The DCI rows put the shunt on AD1 (R24),
  as the DCV rows put the divider there. No recovered row puts the shunt on
  AD2/AD3, so true simultaneous V + I (outcome 1 with AD3) would need
  register settings nobody has run: not attempted.
- **No relay disconnects the VΩ jack.** PA13 (the ohms source) is off in
  both. The divider is the chip's own PA taps, set by register. So
  **alternating AD1 between the two rows needs no mod**, the same way the
  AC+DC dual display alternates (B23).
- **PA8 is the catch.** It is on for DC volts' direct 50/500 mV ranges and
  for DC current's 50 and 500 mA ranges, off otherwise. Pairs where both
  rows agree switch with no relay movement: **current on 500 µA, 5 mA, 5 A
  or 10 A with volts on 5-1000 V**, or current on 50/500 mA with volts on
  50/500 mV. Every other pair moves PA8 at each switch: a click, wear
  (outcome 3), and on 50/500 mA possibly the shunt tap changing under the
  load (on our meter PA8 shifts those ranges' readings by ~2%,
  `results/dci-20261114.md`).
- **What the bench must show** (card in `test-plan.md`): (1) the volts
  row reads the VΩ input right while current flows through the mA jack;
  (2) the current reads the same before and after a volts read on a
  relay-free pair; (3) on 50/500 mA with volts above 500 mV, whether moving
  PA8 disturbs the load current. If (3) is bad, those pairs can still work
  by reading volts rarely (every ~10 s: a cell's voltage moves slowly),
  which keeps the relay to ~1 cycle in 10 s.
- **Rate:** each switch drops two conversions, so at SPEED low volts and
  current each refresh about every 1.5 s; at high, about every 0.4 s.

The app side is built and tested in the simulator (`features.md`, Power
and energy); on the meter `platform_t.vi_avail` stays NULL until the card
passes, then returns which pairs may alternate.

**Safety for any mod:** the analog board sits at input potential (up to
1000 V). Work with the leads out and the meter unplugged, and keep the
creepage slot clear. A new pin must be protected like the existing inputs.

**Usage limit, no mod can remove:** both readings share COM. The voltage
reading is referenced to a node in the current path, so it either includes
the shunt's burden voltage or reads the load from the low side, depending on
wiring.

Other pairs:
- Pairs needing a different input jack or a current source (resistance,
  continuity, diode, capacitance with anything) stay impossible.
- ACV + DCV on one input is item 8.

**10. Better low-end capacitance** — *open* (B17) below 500 pF. All seven
ranges, 50 nF to 50 mF, read on the meter.
- REL nulling of test-lead capacitance works now.
- Below ~100 pF the limit is the HY3131's charge-ratio measurement and
  board leakage.
- Firmware can still help:
  - averaging longer on the lowest range;
  - an automatic open-lead null.
- It can't add resolution the counter doesn't have.
- Large electrolytics (the 5 mF and 50 mF ranges) need the charge-slope path,
  whose charge-count source isn't recovered yet.

**11. Wider AC bandwidth** — *won't fix; characterised and flagged* (B20).
- The true-RMS converter and input attenuators set the roll-off. Firmware
  can't move it.
- What we can do: measure the response on the bench and state the usable
  bandwidth honestly, or flag readings above it when frequency is also known.
- A frequency-dependent correction would amplify noise and is not
  recommended.

**12. Speed setting changes the ADC rate** — *done, on the meter* (B11):
R22 0x16/0x15/0x14, 4.0 / 8.0 / 16.2 readings a second. The original note:
- Recorded and saved already.
- `capture-afe.py` now steps RATE S/M/F to show which register it changes
  (AD1OSR, 0x22, is the guess).

## Data and logging

**13. Bigger internal log** — *done.* 10,668 readings, power-safe, readable
over SCPI (`app.md`, Recording).

**14. Timestamped logs** — *done, on the meter* (B33): the crystal
oscillates and the RTC keeps time on its cell. The original reasoning:
- The board photos show X1, a 32.768 kHz crystal, and a CR1220 cell on VBAT
  (`hardware.md`). That is the GD32's battery-backed RTC, and stock's menu
  has a Clock entry.
- With it, sessions get a start time, and the Utility menu gets a clock.
- Confirm on the bench that the crystal oscillates.

**15. Several sessions, wrap-around** — *wrap-around done* (B35);
several sessions open.
- The page format already carries a session number per page.
- Several sessions need a small index.
- Wrap-around (a rolling log of the last N points) needs the reader to start
  at the oldest page.
- Both are host-testable like the current log.

## Connectivity

**16. Standard, correct SCPI** — *done.* Conventional SCPI with stock's
reply formats kept, and the CONT/DIOD swap fixed (`scpi.md`).

**17. sigrok support** — *High; firmware side done.*
- libsigrok's `scpi-dmm` driver handles SCPI meters through per-model
  profiles. With our command set, adding one is a small upstream patch to
  libsigrok, not firmware work.
- Whether any OWON XDM model is already listed is unverified; check the
  driver source.
- One source gives the XDM1041's default serial setting as 115200 8N1, which
  matches our default.

**18. Survives host link drops** — *done on our side.*
- Our serial handling is stateless per line: a dropped connection loses at
  most one command.
- Host software freezing is the host's problem. A sigrok or ESP32 front end
  avoids OWON's PC application altogether.

**19. Firmware update over serial** — *done* (B7, 2026-11-10): OWON's
bootloader accepts our image, with no signature; every install since
has gone this way (`install.md`). The original reasoning:
- OWON's bootloader evidently updates the app slot in the field: v2.1.2 ships
  as a bare image linked at 0x08002800, the slot ours is linked for.
- `DOWNLOAD` writes an update marker (0xA5A5 at prefs +8) and resets; that is
  almost certainly the entry point (`settings-dump.md`).
- The protocol and any checksum are only learnable from the bootloader dump
  (day 1).
  - **If it accepts our image:** users update with no tools and no opening,
    and stock stays restorable the same way.
  - **If it checks a signature we can't produce:** the fallback is our own
    loader in the app slot. That is riskier, and still needs one SWD flash
    per meter.
- Keep OWON's bootloader in every case. It is what makes a bad update
  recoverable.

**20. Internal ESP32 with a Wi-Fi web UI** — *Medium, Mod + FW.*
- The plan is to tap the red harness for 5 V and the UART, and serve
  readings, settings, defaults, logs and firmware updates over Wi-Fi.
- The firmware side is small:
  - SCPI already covers settings and logs;
  - item 19 covers updates.
- Open questions that decide feasibility:
  - **Does the harness carry TTL UART, or RS232?** The SP202 level shifter
    sits on the separate interface board. If the harness is after it, the
    ESP32 needs a level shifter, or a tap before the SP202.
  - **Power:** an ESP32 draws peaks of a few hundred mA while transmitting. Check the 5 V rail's headroom and
    the battery's capacity (`photos/battery_1.jpg`); a sleep mode or
    Wi-Fi-off setting may be needed on battery.
  - **Noise:** compare the measurement noise floor with Wi-Fi on and off
    before trusting readings.
  - **Isolation:** the ESP32 must sit on the digital side, never bridging
    the isolation slot.
- Optional: wiring it to the SWD header as well makes it a remote recovery
  probe, which also removes most of the risk in item 19.

## Power

**21. Battery monitoring** — *done, on the meter, no mod* (B47): VBAT
reaches PB0 through a 1:2 divider, as stock V4.x read it. The original
reasoning, before that was found:
- Stock never reads an ADC and has no battery strings.
- VBAT reaches the digital board on the harness, but nothing measures it.
- The firmware hook (`platform_t.battery_mv`) and Utility menu row exist;
  only the source is missing. Two options:
  - **Divider to a free ADC pin:** a two-resistor mod. Which pins are free on
    the v3 board is a bench item.
  - **Query the ATtiny202** over the unused CO2 I2C header, if it already
    senses the battery. This is no mod at all, but needs its firmware or
    protocol, which we don't have.

**22. Auto power-off** — *open* (B48, B49).
- The ATtiny202 almost certainly owns soft power via `PWR_EN`.
- Once its handshake is known from the bench, power-off after an idle time
  is a few lines next to the existing dim timer.
- Until then it isn't attempted. A wrong handshake could stop the meter
  staying on at all.
