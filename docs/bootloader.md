# OWON's bootloader (0x08000000-0x080027FF)

Read off our meter by the flash dumper on 2026-11-10 (two full reads,
byte-identical, CRC-checked): `backups/20261110-210942-flash/bootloader.bin`,
sha256 `f3ec217d…e39e`. 9164 of its 10240 bytes are used. Analysed with
`scripts/fw-annot.py <bin> 0x08000000 0x23cc --base 0x08000000`. Addresses
below are in the bootloader.

It answers the two questions the replacement still had: **the panel init
sequence**, and **what an update erases**.

## Start-up, in order (main at 0x08001D68)

1. Clock setup (0x0800089C, PLL multiplier field 0x380000).
2. GPIO (0x080009A0): push-pull outputs on **PA0-PA7** (panel data),
   **PA11**, **PB0, PB1, PB2, PB10, PB11**. Then PA11 low, PB0 high,
   PB11 low, PB1 high.
3. USART0 init (0x0800107C). It sends the three factory bytes at
   **0x0803F87E, 0x0803F87F, 0x0803F880** (factory record +0x7E..+0x80)
   out of the serial port, one at a time: the `00 01 00` seen at every
   power-on (backlog B39), now explained.
4. **Panel init, chosen by those three bytes** (first one equal to 1 wins):

   | Byte | Init routine | Controller (by its commands) |
   | --- | --- | --- |
   | +0x7E = 1 | 0x08000524 | ILI9488-class (`F7 A9 51 2C 82`, `E9`) |
   | **+0x7F = 1** | **0x0800151E** | **ILI9488-class: this is our meter (bytes `00 01 00`)** |
   | +0x80 = 1 | 0x080016A4 | ST7796-class (`F0 C3`, `F0 96`) |
   | none | 0x08001AB4 | ST7796-class, after a 200 ms wait |

5. Clears the whole screen, draws a white box frame, and **sets PA11 high**
   (the backlight: it matches stock's PA11 backlight PWM, `hardware.md`).
6. Reads the 44-byte preferences header at **0x0803F000**. If the word at
   +8 is **0xA5A5** (what stock's `DOWNLOAD` command writes), it stays in
   update mode and waits about 8 M loops for the host.
7. Otherwise it draws an 8-step progress bar (25 ms a step), listening for
   `start` on the serial port, then jumps to **0x08002800** (0x08000918).

## Observed on the meter (2026-11-10, our `xdm1241-serial` build installed)

A power-on, timestamped on the serial port:

| t (s) | Bytes | What |
| --- | --- | --- |
| 0 | `00 01 00` | the bootloader sending factory bytes +0x7E..+0x80 (step 3) |
| | (screen) | click, backlight on, frame, the bar fills across (step 7) |
| +1.22 | `XDM1241 firmware dev` ... | our firmware's start-up log |

No `7` was sent, so it took the normal path (no update flag). Right
after an install, the bootloader starts the app straight from update mode
and the bar stays empty until the next power-on; that is expected, not a
stuck flag.

**The meter stays on with nothing driving any power pin** (uptime counted
past 170 s, then a clean power cycle): the ATtiny202 needs no keep-alive
from the application (`replacement-plan.md` risk, CLAUDE.md priority 4).

## Our meter's panel init (routine 0x0800151E)

`C` = command (D/C low), `D` = data. Delays are in the bootloader's delay
unit (0x080014E8), ≈ 1 ms: the 120-unit wait after sleep-out elsewhere is
the datasheet's 120 ms.

```
select (PB1 low, see below)
C F7  D A9 51 2C 82        adjust control 3
C 36  D 08                 MADCTL: BGR
C 3A  D 55                 16 bpp
C B4  D 02                 inversion control
C B1  D A0 11              frame rate
C C0  D 0F 0F              power control 1
C C1  D 41                 power control 2
C C2  D 22                 power control 3
C B7  D C6                 entry mode
C C5  D 00 53 80           VCOM
C E0  D 00 08 0C 02 0E 04 30 45 47 04 0C 0A 2E 34 0F    positive gamma
C E1  D 00 11 0D 01 0F 05 39 36 51 06 0F 0D 33 37 0F    negative gamma
C 21                       inversion on
C 11                       sleep out
wait 0x150 (≈ 336 ms)
C 29                       display on
```

Extracted by tracking the `movs r0,#imm` before each call to the command
writer 0x08001CE8 and data writer 0x08001D1C. The other three sequences
don't apply to our meter and aren't written out here.

## Panel bus: the same as stock's app, with one correction

| Function | Address | What it does |
| --- | --- | --- |
| write command | 0x08001CE8 | PB2 low, PB10 low, byte to PA0-7, PB10 high |
| write data | 0x08001D1C | PB2 high, then the same |
| byte to bus | 0x08001D08 | `GPIOA->BOP = b; GPIOA->BC = ~b & 0xFF` |
| select/release | 0x0800197C | see below |

**PB1 is the panel's chip select, not its reset.** 0x0800197C(1) sets PB1
high, waits, sets PB0, PB10 and PB2 high, waits, then **drives PB1 low and
leaves it low**; commands follow. 0x0800197C(0) sets PB1 high. The
bootloader calls (1) before each init and before drawing, (0) after. A
reset line can't be held low while the panel takes commands, so this is a
select. Stock v2.1.2's `lcd_reset` (0x08005E5C) has the same shape and was
misnamed. **PB0** is set high at GPIO init and in select; it may be the
real panel reset (held high), but nothing pulses it. Not confirmed.

## What an update erases

The protocol (`community-research.md`) as the code implements it:

| Area | Erases | When |
| --- | --- | --- |
| **5 (application)** | **117 pages of 2 KB from 0x08002800: 0x08002800-0x0803CFFF**, always, whatever the image size | as soon as the area is chosen, before the pack count. That's why the dumper's erase "took 0.05 s" and why stock didn't survive |
| 2 (calibration) | 1 page at **0x0803F800**: the factory record | as soon as the area is chosen. **Never send it** |

Consequences for our firmware:
- **Every serial update wipes our data log (0x08030000) and settings
  (0x0803B800)**: both sit inside 0x08002800-0x0803CFFF. Either accept that
  (and say so in `features.md`), or move them out of the erased range;
  0x0803D000 is not free (below), so moving means sharing stock's pages.
- **0x0803D000 is the bootloader's own state page** (`50505050`, counters,
  a copy of `start`). Our firmware must never write it.
- After an install the bootloader writes the 44-byte preferences header at
  0x0803F000 (0x08002260 → 0x0800227C: erase 1 KB pages from 0x0803F000 up
  to 0x0803F800, then program). It doesn't touch 0x0803F800.
- Pack data is checked with CRC-16/CCITT (poly 0x1021, 0x080004F2). What it
  stores at 0x0803F004 doesn't match the CRC of the dumper pack computed
  that way; unresolved.

## Checked against something independent

- The dumper's pack in the dump is byte-identical to the built image.
- PA11 = backlight and PB11 low (beeper) agree with what stock v2.1.2's
  app was found to do with those pins (`hardware.md`), from a different
  binary.
- The factory record in the dump is identical to the `INT:TXT` serial dump.
