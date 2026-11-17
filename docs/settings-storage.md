# Settings and calibration storage

Recovered from the v2.1.2 image, 2026-11-06.

## It is internal flash, not an EEPROM

The firmware contains the STM32/GD32 flash unlock keys (`0x45670123`,
`0xCDEF89AB` at 0x08002FD4) and **no I2C peripheral references whatsoever** —
neither 0x40005400 (I2C1) nor 0x40005800 (I2C2) appears anywhere in the image.

**Resolved later:** the 8-pin part next to that header is an **ATtiny202**
(marking `T202-N`), a second microcontroller — not a serial EEPROM. There is
no EEPROM on the board, so no I2C EEPROM driver exists to find. The header is
most likely an I2C link between the GD32 and the ATtiny202. See
`analog-frontend.md`.

The conclusion that calibration lives in internal flash stands, and now has no
competing candidate. Earlier notes in `readout-protection.md` suggested dumping
it to recover calibration; that will not help. Persisted data lives in MCU
flash, which is exactly the case where erasing the chip destroys it.

## Map

> **Corrected 2026-11-07.** This map originally called the 6004-byte record
> "calibration" and missed the 168-byte record entirely. Both were wrong; see
> `calibration.md`. The table below is from the store wrappers at
> 0x08006540–0x08006600 and the code that uses each RAM copy.

Three records at the top of the 256 KB flash:

| Flash | Size | RAM copy | Holds | Read / write |
| --- | --- | --- | --- | --- |
| 0x0803D800–0x0803F000 (3 pages) | **6004 B** | 0x20001FB8 | **data log**, 1000 points | `FUN_08006558` / `0x080065F4` |
| 0x0803F000–0x0803F800 (1 page) | **168 B** | 0x200019A0 | **user preferences** | `FUN_08006540` / `FUN_08006600` |
| 0x0803F800–0x08040000 (1 page) | **1392 B** | 0x20001A48 | **factory record: calibration + identity** | `FUN_0800654C` / `0x080065E8` |

All three are loaded at boot by `FUN_0800457C` (word copy, `stock_store_read`
0x08006666) and written back whole.

### Data log — 0x0803D800

| Offset | Type | Contents |
| --- | --- | --- |
| +0x0000 | int16 (+2 pad) | point count, 0–1000 (clamped to 0 at boot if > 1000) |
| +0x0004 | float[1000] | value |
| +0x0FA4 | u8[1000] | function code |
| +0x138C | u8[1000] | range index (0xFF / 0xEF mark REL variants) |

4 + 4000 + 1000 + 1000 = 6004 = 0x1774. Append is `FUN_0800D83C`
(`if (count > 999) return`); `FUN_0800D2FC` draws the log viewer, 9 rows per
page. The manual's "Record Length 1,000 points" is this array.

### User preferences — 0x0803F000

Validated at boot by a magic word. On mismatch `FUN_08010A02` writes defaults
and saves:

| Offset | Type | Default | Notes |
| --- | --- | --- | --- |
| +0x10 | u16 | 100 | backlight (passed to `stock_lcd_backlight`) |
| +0x14 | u32 | 50 | |
| +0x18 | u32 | **0x55AA1357** | magic |
| +0x36, +0x37, +0x40, +0x44 | | 0 | REL state and values (used by log append) |
| +0x9C | i16 | 100 | record length, valid 1–1000 |
| +0xA0 | i32 | 1000 | record interval in ms, valid 15–9,999,999 (manual: 15 ms – 9999.999 s) |

### Factory record — 0x0803F800

Calibration `coef` / `zero` tables at +0x1F8 / +0x3B0, identity strings at
+0x14 / +0x133. **Never reset by firmware.** Layout in `calibration.md`.

## The write path

`stock_store_write` at 0x08006564, called as `(start, end, ram, len)`:

```
cpsid i                             ; interrupts off for the whole operation
FLASH_Unlock()                      ; 0x08002C06
pages = (end - start) >> 11         ; 2 KB page granularity
for each page: FLASH_ErasePage()    ; 0x08002C7C
for each word: FLASH_ProgramWord()  ; 0x08002DBE
FLASH_Lock()                        ; 0x08002C1E
cpsie i
```

Erase-then-program the whole region, no wear levelling and no A/B ping-pong,
so a power cut mid-write loses the record. Worth improving in our own
firmware; the spare page at 0x0803F000 is sitting right there for it.

## Why this matters more than it looks

**Calibration can be preserved across a full custom-firmware flash.** It is
one 2 KB page at a fixed address (0x0803F800), independent of the
application. Save 0x0803D800–0x08040000 before erasing and write it back
afterwards, and the meter keeps its factory accuracy even with entirely new
firmware.

That de-risks the whole project, and it means the backup step should capture
this region separately as well as the full image.

## Correction to the free-flash figure

Earlier notes said 129.7 KB is free above the application. That is wrong at
the top end: the last 10 KB is this storage. Usable free flash is

```
0x0801F934 (app end) .. 0x0803D800 (storage start)  =  ~120 KB
```

Extension code at 0x08020000 is safely inside that, but **nothing may erase
pages at or above 0x0803D800**.

## Still unknown

- Most of the preferences record beyond the fields above, and the first
  0x1F8 bytes of the factory record. A serial dump (`settings-dump.md`) plus a
  diff across one menu change gives the preferences side; note that dump sends
  the *factory* record, so preference diffs need SWD or another command.
- **No checksum guards any record at load** (`calibration.md`). The `CHECKSUM`
  SCPI keyword and menu entry exist, so a checksum may still be computed
  somewhere, but not at boot.
