# Community research: what others found

This aggregates two sources, read 2026-11-07:
- the whole EEVblog thread [OWON XDM1041 the unknown multimeter][thread]
  (12 pages, posts #0–#293, 2021–2026);
- Petteri Aimonen's [owon-xdm2041-info][petteri] (CC0), which documents the
  sibling XDM2041.

Post numbers are given as `#n`. Claims are other people's unless marked
**confirmed here**; treat the rest as leads.

[thread]: https://www.eevblog.com/forum/testgear/owon-xdm1041-the-unknown-multimeter/
[petteri]: https://github.com/PetteriAimonen/owon-xdm2041-info

## What this confirms of our own analysis

Each of these was derived from the v2.1.2 image alone. Someone else's
independent work now agrees.

| Our finding | Independent evidence |
| --- | --- |
| Calibration is `coef[11][10]` ×10⁶ at factory record +0x1F8, in the last flash page | Petteri's backup script checks +0x1F8, +0x22C, +0x350 for ≈1,000,000 in the 2 KB last page. **Confirmed here:** our parser reads his real XDM2041 record with 0 implausible slots, and its cold-junction gain is 1701 (ours: 1694). See `test/data/` |
| The TXT dump is a 4-byte header + 20 identity bytes + 2048 bytes | His script drops exactly 0x18 bytes before the 2048 |
| `DOWNLOAD` writes a marker and resets: the update entry | `INT:DOWNLOAD` sets a "bootloader-enable flag in flash"; the meter then stays at the progress bar until it receives `\r\n\r\n` at 115200 |
| The app is a raw image at 0x08002800 behind OWON's bootloader | The update stream starts `08 4B 00 20 01 29 00 08`: SP 0x20004B08 and reset 0x08002901, an image linked at 0x08002800 |
| The panel is an 8-bit parallel bus: PB10 WR, PB2 D/C | UniSoft (#203, #229): PB10 "CLK", PB2 DC, PB1 "CS" (we have PB1 = RESET; he later said "probably RESET", #161) |
| 2.x firmware targets STM32F103; 3.x the GD32F303 | #222/#223, plus OWON support (#204/#206) |
| The ATtiny202 owns soft power | UniSoft (#186, #221): "standalone ATTINY202-SSF", not updatable over USB/COM |
| CONT/DIOD are swapped in `FUNC?` | fmolinelli (#209), on an XDM1241 |
| 115200 baud | Petteri's tools; XDM1041 defaults are 115200 8N1 |

## Corrections this forces

- **The dump command is `INT:TXT`, not `SYST:TXT`.** The service keywords
  (TXT, DOWNLOAD, VERSION, MODEL, HYREG, CHECKSUM) hang off the `INTernal`
  root. `serial-dump.py` now tries `INT:TXT` first and keeps `SYST:TXT` as a
  fallback.
- **`START` must never be sent.** Two separate dangers:
  - **`INT:START`** runs the factory auto-calibration. A user aborted it and
    was left with wrong DC calibration (#265).
  - **`start`** sent during power-on enters the bootloader (Petteri).

  The blocklist now refuses `START` and every `INT:` command except `INT:TXT`.
- **The HY3131 bus is slow.** On the XDM2041 the SCK→MISO path crosses
  optocouplers twice, about 50 µs, and stock clocks it at **2.6 kHz**. Ours
  was bit-banging at about 500 kHz. `afe_delay` now uses the cycle counter,
  defaulting to stock's rate (`HAL_AFE_HALF_BIT_US`, tune on the bench).
  **On our v3 meter (2026-11-14)** the bus reads back cleanly down to a
  1 µs half-bit (`DIAG:BUS`); the default is now 4 µs (backlog B5).
- **Backlight PWM is audible.** Stock's 11 kHz squeals, rising with
  brightness (#290); our 10 kHz would too. It now defaults to about 25 kHz
  (`HAL_BACKLIGHT_PWM_HZ`). Verify the LED driver dims cleanly at that rate.

## The bootloader protocol (from OWON's XDM1041 updater, via Petteri)

All at 115200 8N1:

1. **Enter:** either send `INT:DOWNLOAD` (sets the flash flag), or send
   `start` repeatedly while the meter powers up. The bootloader answers `7`.
2. **Area byte:** `0x05` is the application, `0x02` is the calibration page.
   The meter answers `7`.
3. **Pack count:** one byte, the number of 10,240-byte packs. Answered `7`.
4. **Data:** each pack is sent raw and answered `7`. There is no framing or
   checksum visible at this level.
5. **Exit:** send `0x14`, or `\r\n\r\n` if the meter is stuck in the
   bootloader.

**Observed on our meter (stock V4.3.0, 2026-11-08)**, listening only while
it was switched off and on twice:
- **No announcement.** Nothing is sent at boot except `00 01 00` at each
  power-on and a single `FC` at one power-off. These are probably the TX
  line twitching as the GD32 powers up and down, possibly a fixed marker.
  Host tools should ignore them.
- **The bootloader is entered by the host, not the meter:** keep sending
  `start` while the meter powers on. An updater's instructions therefore
  read "run it, then switch the meter on".
- **The USB-serial chip stays powered while the meter is off.** It runs from
  the computer's USB, so the port never drops.
- **Start-up is about 5 s,** button to first reading, timed by eye.

Petteri's `xdm2041_bootloader.py` implements this. theHWcave (#201) reports
his tools work on the XDM1041. **Area 2 rewrites calibration:** any updater
we ship must make that impossible to send. One user's update failed at
"checking new version" and left calibration corrupted (#194), restored from
the XDM2041 record.

## Hardware versions

| Firmware | MCU / board | Notes | Source |
| --- | --- | --- | --- |
| 1.2.0, 1.3.0 | first boards (2021) | can't take newer firmware: "the screen changed" | #112, #250 |
| 2.x (2.1.2 = "20210525", our image) | STM32F103 | 2.1.2 appends `OK\nOK\nOK` to every non-query command, which breaks DMMEasyControl | #118, #120, #151 |
| 3.0.1, 3.1.0, 3.3.1 (XDM1241), 3.6.0 | GD32F303 | OWON: "different LCD panel", same functions | #150, #170, #204, #206, #209, #215 |
| 4.0.0 (2023) | GD32 | ceramic 1000 V fuses; MCU-board LDO replaced by a switcher | #231 |
| 4.1, 4.2.0, 4.3.0, 4.4.0, 4.5.0 | GD32 | 4.x exports the internal record (`int:record?`) | #233, #236, #241, #255, #262, #282 |
| 4.7.0 | ? | posted as a .7z by a user who says it "doesn't work well". **Do not trust** | #283 |

Our board, `XDM1041_dig_v1.7 (2023.09.18)`, runs stock **V4.3.0**
(`*IDN?`, 2026-11-08), far newer than the v2.1.2 image we analysed. The
calibration record layout is unchanged.

Other board facts:

| Fact | Source |
| --- | --- |
| A 2025 XDM1241 has a shielded motherboard; analog board silkscreen `XDM1041_Analog_V1.5.1 2025.03.18`; USB board dated 2025.12.19 | #262 |
| Interface board: CH340 USB (most) or SP202/MAX232-class RS232 (early). Either way **the harness to it carries logic-level UART** | #51, #64, #134 |
| Voltage reference: U17 (SOT23-6) usually fitted; optional U6 (SOIC-8) footprint, possibly 1.2 V LM385-class or MAX6190 | #281 |
| HY3131 clock looks like 4 MHz, as on the XDM2041, not the datasheet's 4.9152 MHz. Kleinstein blames this for poor 50 Hz hum rejection | #177, #180 |
| XDM2041 only: relays are driven by a 74HC595 on the same SPI bus. On ours, theHWcave suspects about two discrete relay signals | Petteri; #147 |
| PB6 = external 1.25 V reference (XDM2041 decode) | Petteri |

**Shunts** (#136, kwass, measured):

| Ranges | Shunt | Burden |
| --- | --- | --- |
| 500 µA / 5 mA | 101 Ω | |
| 50 / 500 mA | 1.2 Ω | about 120 mV at 100 mA |
| 5 / 10 A | 0.02 Ω | above 7 A: 30 s on, 30 s off |

- A and mA/µA have separate jacks.
- Some units read up to 4% low at 10 A; trimming the solder on the shunt
  fixed one (#121, #125).

**Power, XDM1041:**
- 5 V 2 A internal switching supply; about 1–1.5 W in use.
- Standby 6 mA at 5 V; the supply is always live when plugged in.
- The hottest part is a 1 Ω resistor by the LCD connector (backlight).
- #57, #93, #167, #178, #226.

**Power, XDM1241:**
- Two 18650 cells in parallel (2200 mAh each, taped or glued; one unit had a
  bad cell contact). USB-C, charges at about 1.6 A.
- Supply current by brightness: 78 mA at 0%, 112 mA at 25%, 144 mA at 50%,
  175 mA at 75%, 202 mA at 100%.
- **About 0.5–1.07 mA drawn while "off"**, which flattens the cells in a few
  months (#241, #274, #291).
- #235, #241, #266, #274.

**Connector "CO2":**
- On a 2025 XDM1241, pin 5 carries the battery voltage at all times, and
  pin 1 is +5 V (#262). An ESP32 was powered from it.
- Our photos show a 4-pin `scl·sda·gnd·3v` CO2. The name may refer to a
  different connector on that revision. **Check on ours.**

**Siblings and rebrands:**
- **XDM1241:** the battery XDM1041.
- **XDM1051/1251:** marketed as "120,000 counts", which is really ±60,000;
  same HY3131 pushed further; no 50 mV AC range (#270, #279).
- **XDM2041:** PeakTech 4095, VoltCraft VC-7055.
- **Victor 8045M:** a clone (#250).
- **Multicomp Pro:** sold by Farnell.

## How stock behaves, as users measured it

**Speed and ranging**
- Up to about 65 readings/s; internal recording works down to 15 ms per
  point (#29, #162).
- Speed is always Slow after power-on (#41, #144, #231, #252).
- Autoranging is slow everywhere (#36).
- Voltage ranges up but never back down; mV and V are separate positions
  (#57, #146).

**Capacitance**
- "Glacial" (#168).
- Autorange is unreliable above 500 µF (#57, #100, #118).
- Values of 500 pF and below read zero, because the lowest range is 50 nF
  (#57).

**AC**
- -3 dB at 8.4 kHz; under 1% error up to 4 kHz; specified 1 kHz (#36, #288).

**Temperature**
- The cold junction is the HY3131's own sensor, far from the jacks
  (#65, #85).
- It is refreshed every 600 s, with a 3 s SCPI stall each time (#76).
- The meter heats itself, so thermocouple readings drift 4–9 °C over the
  first hour (#64–#99, #244). The HI socket runs 0.5 K cooler than LO, and LO
  1.5 K above ambient (#182).
- Turning the backlight down removes about 2 °C of the error (#93).

**PT100**
- REL nulls degrees, not ohms, so lead resistance can't be nulled.
- No PT1000 support.
- The error is about 2.6 °C per ohm of lead resistance (#99, #105, #150).

**Other measurement notes**
- 10 MΩ readings move when a hand comes near: EMI or hum (#96).
- Ohms test voltage: 2.17 V open on the 500 Ω range, 1 V on 5 kΩ–500 kΩ,
  0.225 V on 5–50 MΩ (#293).
- Frequency counter works to 60 MHz, needing 1.5 Vpp at 55 kHz and 3.6 Vpp
  at 60 MHz (#261).
- Continuity beep latency depends on the threshold (#152).
- REL is a math mode, so it can't be combined with averaging or dB (#57).
- There is no auto-hold, only RUN/STOP (#57).

**Recording and UI**
- 1,000 points; viewing the list during recording isn't possible; a new
  recording erases the old (#232).
- Downloaded with `int:record?`, which returns comma-separated values
  (#236). There are Python readers for it (#289) and `rusty_meter` (#251).
- Boot takes about 5 s; the power button needs a 2–3 s hold (#36, #184).

## Hidden menus: do not enter

**UniSoft (#196):**
- UTILITY → MATH → UP → RANGE → DOWN opens manual calibration: the DEBUG,
  KEY and LCD submenus, with editable ZERO/COEF.
- UTILITY → DUAL → UP → RANGE → DOWN starts **automatic calibration, which
  wipes calibration** (#199).
- #202 documents recalibrating by hand against a reference.

Never use either on this meter until the factory record is backed up, by
serial dump and SWD.

## Prior art to reuse

| Project | What it is |
| --- | --- |
| [owon-xdm2041-info][petteri] | bootloader tools, calibration dump/restore, HY3131 SPI captures per mode with decoded registers (`spi_captures/measurement_modes.md`) |
| UniSoft's control-board schematic (#158, `Schematic_XDM1041_2022-01-09.pdf`) | pin map for the GD32 board. **Worth getting:** it would settle keypad, PWR_EN, relays |
| [TheHWcave/OWON-XDM1041](https://github.com/TheHWcave/OWON-XDM1041) | SCPI command list, Python logger, OWON's updater and firmware 2.1.2 |
| `owon-xdm-remote` (Elektroarzt, #256) | ESP32 Wi-Fi/MQTT board that **replaces the USB board**, powered from the meter's 5 V; MicroPython |
| `rusty_meter` (#251), TestControllerUA config (#209), `readrecord.py` (#289) | host software |
| Project Phoenix, essentialscrap.com/owon_fix (#171) | other OWON firmware work |

## Can it measure voltage and current at the same time?

**Physically, at the jacks and the chip, yes:**
- **Jacks:** current flows A (or mA) → shunt → COM. Voltage is VΩ → COM.
  Both share COM, which is how you'd wire a load to read its voltage and
  current together.
- **Chip:** the HY3131 has three ADCs with independent input selectors, and
  calls AD3 its "Power" ADC, phase-aligned with AD2 (`SPHACAL`) for watts.
- **Routing, from the XDM2041's per-mode register captures:**
  - Voltage reaches the HY3131 through the PA divider onto SENSE/RLU
    (AD1 in DCV).
  - Current reaches it through op-amp output OP1O, or pin PB1 on some
    ranges, against PB3.
  - AD3 can select OP1O, so AD1 on volts and AD3 on amps is routable inside
    the chip.

**What's open is one relay:**
- In the XDM2041 captures, relay bit 0 is set in every voltage, resistance
  and capacitance mode, and clear in every current mode.
- Whether that relay disconnects the voltage input or reroutes the current
  path decides between:
  - **no mod** (keep the current-mode state and read SENSE too);
  - **a small mod**;
  - **impractical** (it must toggle per reading and would wear out).
- Ours is a different board, with relays driven differently.

**Settling it on the bench:**
- Run our firmware in the current-mode relay state, with a voltage on VΩ,
  and read SENSE.
- The test only reads; it can't harm anything. Backlog item B21.
