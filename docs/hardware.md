# XDM1241 hardware notes

Status key: **confirmed** = read directly off the board photos or measured.
**unverified** = inferred, must be checked before any code depends on it.

## Digital board — `XDM1041_dig_v1.7` (2023.09.18)

The XDM1241 and XDM1041 share this board. Treat XDM1041 community findings as
applicable, but re-confirm anything that touches the analog side.

### MCU — U2, confirmed

`GD32F303CCT6A` / GigaDevice / ARM. Decoded:

- `303` — GD32F303 series, Cortex-M4
- `C` (first) — 48-pin LQFP
- `C` (second) — 256 KB flash
- `T` — LQFP, `6` — −40 to +85 °C
- 48 KB SRAM, up to 120 MHz

### Debug header — confirmed

Silkscreened bottom edge, 5 pins, 0.1" pitch:

| Silk | Signal | Note |
| --- | --- | --- |
| `RST` | NRST | continuity to GD32 pin 7 (measured). Grounding it holds the chip in reset |
| `3V` | MCU rail | **reads 2.8 V** with the meter on (measured), not 3.3. Sense only — power the meter normally |
| `PA14` | SWCLK | continuity to pin 37 (measured) |
| `PA13` | SWDIO | continuity to pin 34 (measured) |
| `GND` | GND | continuity to pins 35 and 47 (measured) |

Stock V4.3.0 turns SWD off once it runs, and the chip is readout-protected:
connect under reset to identify it, and expect no flash reads
(`bench.md`, `readout-protection.md`).

JTAG is impossible here: PA15 (JTDI), PB3 (JTDO) and PB4 (NJTRST) are not
brought out. SWD only.

### Other connectors — confirmed present, function unverified

| Ref | Pins | Silk | Likely purpose |
| --- | --- | --- | --- |
| CO2 | 4 | `scl · sda · gnd · 3v` | I2C, most likely GD32 ↔ ATtiny202 (U6). **Not an EEPROM** |
| — | FFC | — | colour TFT, **8-bit parallel** (the `MOSI/CLK/NSS` points belong to the AFE bus) |
| — | 8-way ribbon | `60V 105C VW-1` | front-panel keypad / rotary |
| — | harness | `GND · VDC-5V · VBAT · PWR_EN` | power + enable from analog board |
| U4 | — | — | buzzer |
| X1 | — | — | 32.768 kHz crystal, with CR1220 on VBAT → RTC |

## Pin map — recovered

Most of this is now established from the firmware rather than guessed. See
`display.md`, `analog-frontend.md` and `scripts/pinmap.py`.

| Function | Pins | Confidence |
| --- | --- | --- |
| Panel data D0–D7 | PA0–PA7 | **confirmed on the v3 screen** 2026-11-11 (test pattern); `lcd_write_byte` 0x08005E9A |
| Panel CS (active low) | PB1 | **confirmed on the v3 screen**: nothing draws until it's low; the bootloader leaves it high. Was misread as RESET (`bootloader.md`) |
| Panel RESET? | PB0 | the bootloader sets it high; unconfirmed |
| Panel D/C | PB2 | **confirmed on the v3 screen** |
| Panel WR | PB10 | **confirmed on the v3 screen** |
| HY3131 CS / CK / DO / DI | PB12 / PB13 / PB14 / PB15 | **confirmed on our meter** 2026-11-11: the chip answers register reads (`analog-frontend.md`) |
| HY3131 IRQ | PA8? | the fifth connector pin; **driven (held low) on our meter**, so likely the IRQ output |
| Keypad group A | PB5, PB6, PB7, PB8, PB9 | **confirmed on our meter** 2026-11-11 (every key pressed and logged) |
| Keypad group B | PA14, PA15, PB3, PB4, PC13 | **confirmed on our meter**; PA14 is SWCLK, hence stock switching SWD off |
| SCPI serial | USART1 | confirmed; `TX△`/`RX△` test points |
| SWD | PA13 / PA14 | confirmed |
| Backlight | PA11, TIM1_CH4 PWM | **confirmed on our meter 2026-11-14**: our 25 kHz PWM dims it from the Utility menu, checked by the owner. Recovered: `stock_lcd_backlight` 0x080060C2 writes TIM1 CCR4; levels table [20,40,60,80,100] of 64, so stock's minimum is 31% and 75/100% both saturate |
| Beeper | PB11, TIM2_CH4 (GD32 TIMER1 CH3, full remap) ~3 kHz PWM | **confirmed on our meter** 2026-11-11: `DIAG:BEEP` sounds it at stock's setting (72 MHz / 370 / 64). Pitch and drive are variable since B57 (`hal.c` `tone`: reload for pitch, compare for duty); loudness against both not yet measured |
| ADC | PB0 (ADC IN8)? | **v2.1.2 (the mains XDM1041 build) never touches the ADC, but stock V4.x on the XDM1241 shows a battery gauge** (owner, 2026-11-15), so V4.x reads VBAT somehow. On LQFP48 the ADC pins are PA0–PA7 (panel data), PB0 and PB1 (panel CS): **PB0 is the only free one**, unless the ATtiny202 reports it over CO2's I²C. Harness VBAT measured ~3.6 V by the owner. **`DIAG:ADC?` 2026-11-15** (ui build, USB cable attached): the bootloader leaves PB0 a push-pull output, high; as analog input it reads a steady **1.99 V** (VDD 2.87 V by VREFINT), so something external drives it, and not a pull-up to 3V. A 1:2 divider would put VBAT at ~3.98 V. Not yet proved: needs VBAT measured at the same time |

`scripts/pinmap.py` misses pins whose init struct is built byte by byte on the
stack (PA11 and PB11 appear as "AIN"); read those functions directly.

### Keypad (mapped on our meter, 2026-11-11)

No row pins: two groups of five lines take turns (stock v2.1.2
`keypad_row_select` 0x08004C94, `keypad_scan` 0x08004E36, read correctly
this time). Group B is driven low and group A read with pull-ups, then the
other way round. Each key joins one A line and one B line. The earlier
notes (columns "PB3–PB7, PA15, PC4", PA7 shared with the panel) were
misreadings: PA7 isn't involved, and the C-port line is **PC13**.

The scan code has the A lines pulled low in bits 0–4 and the B lines in
bits 8–12. Logged on the `xdm1241-panel` build by pressing every key:

| | PA14 (B0) | PA15 (B1) | PB3 (B2) | PB4 (B3) | PC13 (B4) |
| --- | --- | --- | --- | --- | --- |
| **PB5 (A0)** | Screen1 | Screen2 | Screen3 | Screen4 | V |
| **PB6 (A1)** | – | Math | – | Screen5 | A |
| **PB7 (A2)** | Record | Utility | Range | Dual | Ω/Cont/Diode |
| **PB8 (A3)** | Run/Stop | Up | Down | – | Cap |
| **PB9 (A4)** | – | – | – | Temp | Freq |

19 keys; no Left/Right or sixth softkey on this panel. The softkeys
Screen1–4 share PB5 and step through B0–B3 in order, which checks the
pairing independently of the press order. Record and Dual, first paired by
order alone, were pressed again on their own and confirmed. In code: `lib/keypad` (host-tested), scanned
by `lib/hal`. The scan's first step is passive (all ten lines as pulled-up
inputs): a line reading low there is driven by something else, and nothing
is driven. It caught the unpowered ST-Link holding PA14 (SWCLK) low.

## Open questions

Several of the original questions here are now answered; these remain:

1. **Is the HSE crystal fitted, and at what frequency?** The clock code does
   HSE x 6 with flash latency 2, implying 12 MHz for 72 MHz, but it falls
   back to HSI if the crystal is absent — and a teardown of a v3.x unit
   reports the high-frequency crystal unpopulated. Affects all timing.
2. ~~Is readout protection enabled?~~ **Yes** on our V4.3.0 meter
   (`readout-protection.md`).
3. **Does the ATtiny202 require a `PWR_EN` handshake?** If so, firmware that
   does not provide it will boot and then power off.
4. ~~Is PA7 really shared between the panel and the keypad?~~ **No**: the keypad is PB5–PB9, PA14, PA15, PB3, PB4, PC13.

Answered since this file was written: calibration is in MCU flash, not an
EEPROM (there is no EEPROM — U6 is an ATtiny202); the AFE is commanded over a
bit-banged SPI to a HY3131; the panel is an 8-bit parallel bus.
