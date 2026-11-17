# The display driver

Recovered with Ghidra, 2026-11-06. This was the stated blocker for theming,
the graph screen and the config screen; it is no longer.

## Confirmed on our v3 meter (2026-11-11)

The `xdm1241-panel` build drew a test pattern on the real screen: bars in
the right colours (RGB565, high byte first), so the bus, D/C, WR and CS pins
are right. Two things the app needed that the stock app image never said:

- **Select the panel: PB1 low.** The bootloader releases it (PB1 high) just
  before jumping to the app.
- **Orientation: MADCTL (DCS 0x36) = 0x28** (MV | BGR). The bootloader
  leaves 0x08: portrait addressing, origin at the top right as the meter is
  held, so a 480-wide window clipped at 320. 0xA8 mirrors left-right. With
  0x28, x = 0..479 runs left to right and y = 0..319 top to bottom, origin
  top left. Set in `lcd_board_init` (`lib/hal/hal.c`); `DIAG:MADCTL <hex>`
  on the panel build tries others live.

## It is an 8-bit parallel bus, not SPI

Every earlier search failed because it assumed SPI. The panel is driven over
an **8080-style parallel interface** with direct register writes — which is
also why no SPI peripheral appears anywhere in the image, and why the
RA1–RA4 resistor arrays sit beside the FFC: they are series terminators on
the data lines.

```c
void lcd_write_byte(uint8_t b)          /* 0x08005E9A */
{
    GPIOB->BRR  = 0x400;                /* PB10 = WR low                  */
    GPIOA->BSRR = b;                    /* PA0..PA7 = D0..D7, set 1 bits  */
    GPIOA->BRR  = ~b & 0xFF;            /*                   clear 0 bits */
    GPIOB->BSRR = 0x400;                /* WR rising edge latches the byte */
}
```

Four register writes per byte, no loop — fast enough for a 480×320 panel,
which a bit-banged SPI would not have been.

### Pin map

| Signal | Pin | Evidence |
| --- | --- | --- |
| D0–D7 | **PA0–PA7** | `GPIOA->BSRR = b` / `BRR = ~b & 0xFF` |
| WR | **PB10** | mask 0x400 pulsed around every byte |
| D/C (RS) | **PB2** | mask 0x4, set per command vs data |
| **CS** | **PB1** | active low. The routine at 0x08005E5C (named `lcd_reset` before) leaves PB1 **low** while commands go out; the bootloader's copy is used as select/release (`bootloader.md`) |
| RESET? | PB0 | set high by the bootloader, never pulsed; unconfirmed |

## The call stack

| Address | Name | Role |
| --- | --- | --- |
| 0x08005E9A | `lcd_write_byte` | one byte onto the parallel bus |
| 0x08005E52 | `lcd_dc` | drive PB2: 0 = command, 1 = data |
| 0x08005EC2 | `lcd_cmd` | D/C=0 then a byte **from a lookup table** |
| 0x08005ED6 | `lcd_data` | D/C=1 then a byte |
| 0x08005EE8 | `lcd_set_window` | `(x0, x1, y0, y1)` |
| 0x08005F46 | `lcd_fill_rect` | `(x, y, w, h, rgb565)` — **the blit primitive** |

### Commands go through a translation table — now resolved

`lcd_cmd` does **not** send its argument directly. It indexes a table in RAM
at **0x2000003A**, so the `0x12 / 0x13 / 0x14` seen in `lcd_set_window` are
*indices*, not controller opcodes:

```c
lcd_cmd(0x12);  data(x0>>8); data(x0); data(x1>>8); data(x1);   /* column window */
lcd_cmd(0x13);  data(y0>>8); data(y0); data(y1>>8); data(y1);   /* row window    */
lcd_cmd(0x14);                                                   /* begin write   */
```

The table is populated at startup from `.data`, which armcc stores
**LZ-compressed** in flash — which is why searching the image for `0x2A`
immediates found nothing. `scripts/unpack-data.py` reimplements the
decompressor and recovers it:

```
01 04 05 09 0a 0b 0c 0d 0e 0f 10 11 12 13 20 21 2a 2b 2c 2e 30 34
                                           ^^^^^^^^^^^^
  idx 0x12 -> 0x2A CASET      idx 0x13 -> 0x2B RASET      idx 0x14 -> 0x2C RAMWR
```

That is the **standard MIPI DCS command set**, enumerated in order:
SWRESET, read-status, SLPIN/SLPOUT, PTLON/NORON, INVOFF/INVON, DISPOFF/DISPON,
CASET/RASET/RAMWR, RAMRD, PTLAR, TEOFF. So the panel is an ordinary
DCS-compatible controller — ILI9486 or ST7796 class for 480x320 — and the
indirection was only ever an abstraction layer.

`LCD_DCS_MAP` in `lib/display/lcd.h` carries the table verbatim.

### Pixel format

`lcd_fill_rect` writes `color >> 8` then `color & 0xFF` — **RGB565, high byte
first**. That is already the format `lib/ui/` renders into, so the host
harness and the hardware agree with no conversion.

## What this unblocks

`fb_blit()` becomes a direct translation of `lcd_fill_rect`, and the UI
harness screens can run on real hardware:

```c
static void blit(const fb_t *f, int x, int y, int w, int h)
{
    lcd_set_window(x, x + w - 1, y, y + h - 1);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            rgb565_t c = f->px[(y + j) * f->w + (x + i)];
            lcd_write_byte(c >> 8);
            lcd_write_byte(c & 0xFF);
        }
}
```

All pins are configured `GPIO_Mode_Out_PP` at 50 MHz by `lcd_gpio_init`
(0x08005D8C), after enabling the GPIOA and GPIOB clocks.

## There is no panel init sequence in this firmware

Every call site was enumerated. The firmware's **entire** display command
vocabulary is:

| Command | Opcode | Where |
| --- | --- | --- |
| DISPOFF / DISPON | 0x28 / 0x29 | `lcd_display_on` 0x08006064 |
| CASET / RASET / RAMWR | 0x2A / 0x2B / 0x2C | `lcd_set_window` 0x08005EE8 |

`lcd_cmd` has exactly four callers and `lcd_data` eight — the coordinate
bytes. **No SLPOUT, no pixel-format set, no gamma, no power control.**

> **Found (2026-11-10): the bootloader does it**, with one of four
> sequences picked by factory record bytes +0x7E..+0x80. Ours is an
> ILI9488-class sequence, written out in [`bootloader.md`](bootloader.md).

So the panel is brought up by something else. Either the **bootloader**
(0x08000000–0x080027FF, not present in this image) does it, or the module
self-initialises from an onboard ROM.

**This matters for full replacement.** A replacement that keeps the stock
bootloader inherits whatever it does. One that replaces everything must
supply its own init. Three places to get it:

1. **Dump the bootloader** once hardware is attached — the most authoritative.
2. **Project Phoenix** has working TFT init routines for a sibling OWON.
3. A **stock ILI9486 / ST7796 init** for 480x320, since the controller is
   now known to be DCS-standard.

### Boot order

From `stock_boot_init` at 0x0800465C:

```
...              peripheral init
lcd_gpio_init()                 0x08005D8C
...
lcd_reset(1)                    0x08005E5C
...
lcd_backlight(level)            0x080060C2
lcd_display_on(1)               0x08006064
```

## Reproducing the analysis

```sh
./ghidra/run.sh                    # headless import, analyse, decompile
./ghidra/find.sh fn FUN_08005f46   # print one decompiled function
./ghidra/find.sh display           # candidate display routines
```

Needs JDK 21+; `run.sh` pins `JAVA_HOME` to Homebrew's `openjdk@25` because
the system JDK 17 makes `analyzeHeadless` try to prompt, which fails with no
TTY.
