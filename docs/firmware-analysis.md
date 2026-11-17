# Stock firmware analysis — XDM1041_OS_V2.1.2 (2021-05-25)

Analysed 2026-11-06 from `firmware/XDM1041_OS_V2.1.2_20210525.bin`
(119,092 bytes, sha256 `16e2706a…67224`), released by OWON with permission to
share via [TheHWcave](https://github.com/TheHWcave/OWON-XDM1041).

**Caveat on applicability.** This image targets the **STM32F103** (v2.x
hardware), not the GD32F303 on our v3.x board — proven below by the 43-entry
IRQ table. The application logic, SCPI grammar, UI strings and range tables
carry over directly. Register-level peripheral setup does not, though the two
parts are largely register-compatible anyway.

## Load address — 0x08002800, and why that matters

The image is **not** loaded at 0x08000000. It is the application only; a
**10 KB bootloader occupies 0x08000000–0x080027FF and is absent from the file.**

Determined two independent ways, which agree:

1. **Pointer resolution.** Scoring every 2 KB-aligned candidate base by how many
   flash-pointer literals resolve to real NUL-terminated string starts:
   0x08002800 scores 41 hits against 11 for the runner-up.
2. **Structural.** With that base, the nine system-exception vectors resolve to
   a canonical stub block — faults hang, unused handlers return:

   ```
   70 47 | fe e7 fe e7 fe e7 fe e7 | 70 47 70 47 70 47 70 47
   NMI   | Hard  Mem   Bus   Usage | SVC   DbgM  PendSV SysTick
   bx lr |  b .   b .   b .   b .  | bx lr bx lr bx lr  bx lr
   ```

   And the reset handler lands at file offset 0x100, immediately after the
   vector table. Both are impossible by coincidence.

### Memory map

| Region | Size | Contents |
| --- | --- | --- |
| 0x08000000–0x08002800 | 10 KB | Bootloader — **not in this file** |
| 0x08002800–0x0801F934 | 116.3 KB | Application (this image) |
| 0x0801F934–0x08040000 | 129.7 KB | Free (50.7% of flash unused) |

**Consequence for our firmware: build to link at 0x08002800 and leave the
bootloader intact.** OWON's USB updater then still works, which is a recovery
path that does not need SWD at all. Taking over 0x08000000 would throw that
away for no benefit.

## Vector table

59 entries, 0xEC bytes → **43 external IRQs, exactly STM32F103 medium-density.**
That is the proof this build targets the STM32F103, not the GD32F303.

Only six vectors point at real code; every other entry is a `b .` or `bx lr` stub:

| Vector | Handler | Note |
| --- | --- | --- |
| Reset | 0x08002900 | calls SystemInit at 0x080029C5, then entry at 0x080028EC |
| RTC | 0x080049AC | matches the CR1220 + 32.768 kHz crystal |
| TIM1_UP | 0x080047CE | |
| TIM3 | 0x08004786 | |
| TIM4 | 0x0800483A | |
| USART1 | 0x080047D6 | the SCPI serial path |

**Absent: ADC, SPI, I2C, DMA, USB.** No ADC interrupt corroborates that
measurement data arrives from the analog board rather than the MCU's own ADC.
No SPI or I2C interrupt means the display and EEPROM are driven by polling,
which is simpler to reimplement than an interrupt-driven design.

## SCPI receive protocol — fully recovered

From the USART1 ISR at 0x080047D6:

- Bytes are appended to a buffer at RAM offset `+0x4CE`, index at `+0x4CD`.
- **Buffer is 50 bytes (0x32); the index wraps to 0 on overflow.**
- **Terminators: `\n` (10), `\r` (13), `;` (59), or any byte < 32.**
- On a terminator the buffer is NUL-terminated, the index reset, and execution
  tail-calls the command dispatcher at **0x08010E1A**.

That is the complete wire-level contract for the serial interface, and our
implementation should match it so existing XDM1041 tooling keeps working.

## CORRECTION — string-table addresses

An earlier pass of this analysis listed the string tables at 0x0801B000–0x0801C5E0.
**Those were file offsets printed with the pre-correction base of 0x08000000, and
are wrong.** Every one is 0x2800 too low. The true addresses are below.

## Recovered tables (corrected)

| Region | Contents |
| --- | --- |
| 0x0801D794 | digit/display charset: `-.0123456789overladOpn ` (packs "overload"/"Open") |
| 0x0801E4D0–0x0801E734 | UI / menu strings: Range, Rate, Trigger, Beeper, Filter, Backlight, Baud, Data Bits, Stop Bits, Parity, Language, Lcd Test, Key Test, Serial Num, CheckSum, Version … |
| 0x0801E9EC–0x0801EAFC | units and ranges: 50 mV–1000 V, 500 uA–10 A, 50 nF–50 mF, Hz/kHz/MHz, ms/us |
| 0x0801EB54–0x0801EBB4 | quoted `FUNC?` responses |
| 0x0801EC04–0x0801EDE0 | SCPI keyword table, 56 entries, short/long form |

## Command pipeline

Three distinct stages, all identified.

### 1. Receive — USART1 ISR at 0x080047D6

Covered above: 50-byte buffer, terminators `\n` `\r` `;` or <32, then tail-calls
the enqueue function.

### 2. Enqueue at 0x08010E1A

Not a dispatcher — it validates and queues. Signature is
`enqueue(char *cmd, uint8_t source)`.

- `strlen` via 0x080127C4, then `(len - 3) >= 48` unsigned → **a command is
  rejected unless its length is 3..50**.
- First byte must be >= 0x20, else rejected.
- Writes into a **ring of 10 slots × 53 bytes at 0x20003C4C**, write index at
  `+0x4CC`, wrapping at 10.
- **If the target slot's pending flag is already set, the command is silently
  dropped.** This is a plausible cause of the "responds OK/nOK to everything"
  fault in OWON's release notes.
- Slot layout: `[0..49]` NUL-terminated text, `[50]` length, `[51]` source,
  `[52]` pending flag. Copy is a byte-wise strcpy at 0x080127EE.

### 3. Service loop at 0x080125F4

Polls all 10 slots each pass; for any slot with the pending flag set it runs a
fixed four-call pipeline, then clears the flag to free the slot:

```
0x080110C0  ->  0x080110EA  ->  0x080124A8  ->  0x08012564
```

Then decrements a countdown byte at `+0x60D` and tail-calls 0x0801230C.

### RAM map of the SCPI state block (0x20003C4C)

| Offset | Meaning |
| --- | --- |
| +0x000 | slot 0; slots are 53 bytes, 10 of them |
| +0x3B4 | cleared per command |
| +0x4B0 | working length |
| +0x4B4 | cleared per command (16-bit) |
| +0x4CC | queue write index |
| +0x4CD | slot index being serviced / byte counter |
| +0x4CE | 50-byte UART receive buffer |
| +0x60D | countdown byte, decremented each service pass |

RX write index is a separate word at **0x20000028**.

## SCPI grammar — recovered

The parser uses a flat array of keyword pointers at **0x0801E2BC–0x0801E37C**,
divided into groups by an **`"end"` sentinel** (the string at 0x0801E4D8).

| # | Address | Keywords |
| --- | --- | --- |
| 1 | 0x0801E2BC | `SHOW` |
| 2 | 0x0801E2C4 | `THREshold` |
| 3 | 0x0801E2CC | `MINimum`, `MAXimum`, `AVERage`, `ALL`, `STATe` |
| 4 | 0x0801E2E4 | `REFerence`, `OFFSet` |
| 5 | 0x0801E2F0 | `RTD`, `TYPe`, `UNIT`, `SHOW` |
| 6 | 0x0801E304 | `KITS90`, `PT100` |
| 7 | 0x0801E310 | `"VOLT"`, `"VOLT AC"`, `"CURR"`, `"CURR AC"`, `"RES"`, `"FRES"`, `"DIOD"`, `"CONT"`, `"CAP"`, `"FREQ"`, `"TEMP"`, `"PER"` |
| 8 | 0x0801E344 | `VOLTage`, `CURRent`, `RESistance`, `FRESistance`, `CONTinuity`, `DIODe`, `CAPacitance`, `FREQuency`, `PERiod`, `TEMPerature`, `SHOW` |
| 9 | 0x0801E374 | `AC`, `DC` |

Group 7 is the `FUNC?` response set (values the meter *returns*, quoted in the
image). Group 8 is the matching *input* keyword set. Groups 5 and 6 are the
temperature subtree. Known consumers: group 1 from 0x08011654 and 0x08011F80,
group 3 from 0x08011AF8, group 7 from 0x080123C0.

## Ghidra

Installed via `brew install ghidra` (formula, **not** a cask — the cask was
removed). Version 12.2, launcher `ghidraRun`.

Import settings that matter:

| Field | Value |
| --- | --- |
| Format | Raw Binary |
| Language | `ARM:LE:32:Cortex` |
| Base address | **0x08002800** |

Then: *Memory Map* → add RAM at 0x20000000 length 0x10000 (RW), and run
Auto Analyze. An STM32F103 SVD through SVD-Loader will name peripheral
registers. Seed the analysis with the known entry points above —
0x08002900 (reset), 0x080047D6 (USART1), 0x08010E1A (enqueue),
0x080125F4 (service loop).

## Reproducing this

```sh
python3 scripts/fw-triage.py firmware/XDM1041_OS_V2.1.2_20210525.bin
./scripts/fw-disasm.sh firmware/XDM1041_OS_V2.1.2_20210525.bin 0x080047D6 0x64
```

Both default to base 0x08002800.

## Next steps

1. **Ghidra** for real structural work — `brew install --cask ghidra`, import as
   raw ARM Cortex little-endian at 0x08002800, then run an SVD loader with an
   STM32F103 SVD to name peripheral registers automatically.
2. Follow the dispatcher at 0x08010E1A to recover the command→handler mapping.
3. Once the J-Link arrives, dump our own v3.x GD32 firmware and diff the string
   tables against this one to see what changed between hardware revisions.

## Font system (partially recovered)

### Confirmed

The glyph renderer is at **0x08006ABE**. Its prologue establishes the whole
font model:

```
r9  = font id (id 1 is remapped to 2)
r0  = 0x0801D794 - 42 = 0x0801D76A   ; metrics table B
r6  = tableB[r9]                      ; u16
r0  = 0x0801D76A - 10 = 0x0801D760   ; metrics table A
r0  = tableA[r9]                      ; u16
r1  = ((r6 + 7) >> 3) * r0 + 1        ; stride per glyph
r0  = *(u32*)(0x20000654 - 20 + r9*4) ; glyph base, from a RAM pointer table
```

- **Five fonts.** tableA = {10, 20, 24, 32, 56}, tableB = {10, 24, 24, 40, 96}.
- **1 bit per pixel**, so colour comes from the caller. This is why theming is
  cheap.
- **Stride is `bytes_per_line * lines + 1`** — one extra byte per glyph,
  most likely the advance width.
- **Charset is 23 glyphs**: `-.0123456789overladOpn ` (the letters spell
  "overload" and "Open", the two text states the main display shows).

### Orientation: column-major

The manual gives the panel as **3.7" TFT, 480x320**, and the meter as 5½
digits. Row-major would make font 4 ninety-six pixels *wide*, so six digits
would need 576 px and could not fit. Column-major makes it **56 wide x 96
tall**: 6 x 56 = 336 px, which fits with room for sign and units. Digits
taller than wide is also the norm. So tableB is the bit-packed axis (height)
and tableA is the line count (width).

| id | w x h | bytes/glyph | use |
| --- | --- | --- | --- |
| 0 | 10 x 10 | 21 | annunciators |
| 1 | 20 x 24 | 61 | small labels |
| 2 | 24 x 24 | 73 | labels |
| 3 | 32 x 40 | 161 | secondary reading |
| 4 | **56 x 96** | 673 | main reading |

### Not yet found: the glyph bitmaps

The base pointers live in **RAM at 0x20000640** (five u32 entries), so they are
set at startup rather than stored as a flash table — a scan for five
consecutive flash pointers finds only the vector table and the SCPI keyword
groups.

Two attempts failed and are recorded so they are not repeated:

1. **Ink-density + spread scan.** Too weak; random code passes it. Rendering
   the best candidate produced visual noise.
2. **Row-correlation scan** (consecutive rows of a glyph are similar, random
   data is not) — correct in principle but O(n) per offset over the whole
   image, and it did not finish in reasonable time at a 1-byte step.

**Next approach, and the one most likely to work:** find the code that writes
0x20000640..0x20000654. That yields the flash addresses directly, with no
statistics. Search for stores through a literal near 0x20000640, or let Ghidra
auto-analysis find the writers now that it is installed.

## Font extraction — font 4 recovered

Superseding the "not yet found" note above: the glyph bitmaps **are** in this
image. The RAM pointer table at 0x20000640 was a red herring — chasing it
statically was the wrong approach, because the data can be located directly.

### What found it

A per-4KB-page scan for *bitmap-like* structure: real glyph data has low ink
(10–23% vs 40%+ for code) and, crucially, near-zero difference between
consecutive rows at the font's own line width. The signal is unambiguous:

| Page | rowdiff @12 | meaning |
| --- | --- | --- |
| 0x08017800–0x0801A000 | 0.027–0.037 | 12 bytes = 96 bits = font 4 |
| 0x08016800 | 0.094 @5 | 40 bits = font 3 |
| 0x08015800 | 0.080 @3 | 24 bits = fonts 1/2 |

That also settles the orientation question from the data itself: the packed
axis is 96 bits, so the fonts are **column-major**, 56 columns of 96 pixels.

### Font 4 — confirmed

| Property | Value |
| --- | --- |
| Base | **0x0801769B** |
| Geometry | 56 wide x 96 tall, column-major |
| Packing | 1 bpp, **MSB of each byte = topmost pixel** |
| Stride | 12 bytes/column x 56 + 1 = **673** |
| Glyphs | 23, charset `-.0123456789overladOpn ` |
| Extent | 0x0801769B – 0x0801B312 |

Corroborated independently: `0x0801769B + 673*23 == 0x0801B312`, exactly the
signed-byte table the renderer loads at 0x08006E0A. The bitmaps end precisely
where that table begins.

**Correction:** that table was initially described here as the per-glyph
advance table. It is not — its values (0, 16, 21, 31, 44, 64, 80, 85, 95) do
not track the glyphs' measured widths, so it is indexed by something else and
remains undecoded. Proportional spacing is instead derived from each glyph's
measured ink extents, which are clean and self-consistent: `.` is 13 px wide,
`l` is 8, digits run 35–46. `scripts/extract-fonts.py` emits those as
`stock_metric_t { left, width }` alongside the bitmaps.

Bit order was settled by rendering both ways; LSB-first produced regular
horizontal striping, MSB-first produced a clean proportional face matching the
manual screenshots. Proof sheet: `docs/fonts/font4-56x96.png`.

Extracted to `lib/ui/stock_fonts.h`, which round-trips byte-identically
against the firmware image.

### Fonts 0–3 — bases since resolved (see `.data` decompression below)

*Historical note, kept as a negative result:* shape-based search returned
multiple candidates for each, and rendering the top hits produced noise, so
**nothing unverified was emitted**. The bases were later read directly from
the decompressed `.data` font table; extraction of fonts 0 and 3 remains. They are packed in
0x08014800–0x0801769B (fonts 0–3 total 7,268 bytes, the region is ~11.4 KB),
so the remaining work is finding the packing order rather than the region.

Reproduce with:

```sh
python3 scripts/extract-fonts.py firmware/XDM1041_OS_V2.1.2_20210525.bin \
        --emit lib/ui/stock_fonts.h
python3 scripts/extract-fonts.py <image> --show 4   # ascii-art a font
```

## `.data` is LZ-compressed — and now readable offline

A significant blocker, resolved. armcc stores initialised data compressed in
flash and expands it at boot, which is why searches for display command
immediates, font base pointers and other initialised values all came back
empty: **those bytes do not exist in the image in plain form.**

The scatter table sits at **0x0801EDEC**, a single entry:

| field | value |
| --- | --- |
| src | 0x0801EE0C |
| dst | 0x20000000 |
| len | 6256 |
| handler | 0x08013776 — ARM `__decompress1` |

`scripts/unpack-data.py` reimplements that handler from its disassembly and
writes the expanded image, so **every initialised variable's boot value is
readable with no hardware**.

Stream format, per block:

```
ctl = next byte
lit = ctl & 7   ; if 0, lit = next byte   -> emit (lit - 1) literals
cnt = ctl >> 4  ; if 0, cnt = next byte
ctl & 8 ?  off = next byte -> copy (cnt + 2) bytes from dst - off
        :  emit cnt zero bytes
```

Validated against a fact established independently: the font base table at
RAM 0x20000640 must contain flash pointers, and font 4's must match the
0x0801769B found by rendering its glyphs. It does, modulo the 1-byte record
prefix.

### What it unlocked immediately

- **The display command table** — standard MIPI DCS (see `display.md`).
- **All five font bases**, resolving the fonts 0–3 gap:

| Font | Geometry | Record base | Notes |
| --- | --- | --- | --- |
| 0 | 10x10 | **RAM 0x20000A34** | lives in `.data`, not flash |
| 1 | 20x24 | 0x0801510C | unused — the renderer remaps id 1 to 2 |
| 2 | 24x24 | 0x0801510C | ASCII face; glyphs verified as `!"#$` |
| 3 | 32x40 | 0x08016B48 | uses the second charset at `0x0801D794 + 24` |
| 4 | 56x96 | 0x0801769A | main reading, bitmap base 0x0801769B |

Bitmaps start at record base + 1.

This technique generalises: any `.data` variable can now be read at its boot
value, which is the cheapest route to several remaining unknowns.
