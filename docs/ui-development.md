# Building UI features

Covers the three goals: **theming**, a **graphing mode**, and a **power-on
defaults screen**. All three are the same problem underneath, and most of the
work does not need the meter.

## The architectural decision that saves the most effort

There are two ways to add a screen:

1. **Integrate with the stock UI** — find its menu state machine, its widget
   calls, every colour constant, and splice yours in.
2. **Take over the panel** — while your mode is active, your code owns the
   whole framebuffer; when it is not, stock draws exactly as before.

**Take over.** Option 1 means reverse-engineering the entire UI layer. Option 2
needs **one** thing: the function that pushes pixels to the panel. Everything
else you write yourself, in portable C, testable on a laptop.

That reduces the critical path from "understand the whole UI" to "find the blit
function" — a single identifiable routine that configures an SPI window and
streams bytes.

Consequence per feature:

| Feature | Under takeover |
| --- | --- |
| Graph mode | Fully yours. No stock UI knowledge needed. |
| Config screen | Fully yours, plus a settings store. |
| Themes | Trivial for *your* screens. Re-skinning *stock* screens is the one case that still needs option 1, because you must find each colour constant. |

So: ship themes for your own screens first; re-theming stock screens is a
separate, larger job worth deciding on later.

## The host harness (built, working)

`lib/ui/` renders into a plain RGB565 framebuffer and knows nothing about the
GD32, SPI or the panel. The identical code runs on the host and on the meter.

```sh
make ui                # builds and renders every screen in every theme
make test              # 12 tests, ~1.3 s, no hardware
```

Output lands in `build/ui-<theme>-<screen>.bmp`. Iterate on a design in
seconds, look at the result, commit the ones you like.

A theme is only colours, so nothing hard-codes one:

```c
typedef struct {
    const char *name;
    rgb565_t bg, fg, dim, accent, warn, grid, trace;
} theme_t;
```

Three ship today: `classic`, `dark`, `amber`. Adding a fourth is a struct.

### What is deliberately placeholder

### Fonts

Two kinds:

- **The main reading** uses stock's 56x96 font (font 4), lifted from the
  image by `make setup` (`lib/ui/stock_fonts.h`, not committed). It holds 23
  glyphs only (`-.0123456789overladOpn `), so the stock fonts cannot do labels.
- **Everything else** is Liberation Sans (Arial's metrics, so it matches the
  manual's screens), anti-aliased, 4 bits per pixel (`lib/ui/label_font.h`,
  committed with its licence `lib/ui/LICENSE-Liberation`, SIL OFL 1.1).
  `fb_text(f, x, y, s, scale, colour)`: scale 1 is 14 px (cap height 10),
  2 is 19 px regular (cap 13: values, menus), 3 is 19 px **bold** (soft-key
  labels and footer captions), 4 is 41 px (cap 28) for the unit under the
  reading. These are stock's sizes, measured on the manual's screens scaled
  to 480x320. y is the top of the capitals; centre with `fb_text_h(scale)`.
  Coverage is blended into the band buffer, so text takes any colour on any
  background.

Strings are UTF-8 and ° µ Ω ± render. Units stay ASCII below the UI ("kOhm",
"uF": SCPI uses the same strings) and `fb_text` shows them as kΩ and µF.
The 41 px size holds only the characters units use; `test_app` checks every
unit the app can produce against it (`fb_text_missing`).

**Title Case.** The app writes labels in capitals ("RANGE", "AUTO"); a theme
with `title_case` (stock's) draws them as stock does ("Range", "Auto").
`fb_text` turns each all-capital word of two letters or more into Title
Case, except acronyms and function names in `keep_caps` (DC, AC, DCV, ACV,
PT100, NUM, ...); mixed-case words are untouched. Write full sentences in
sentence case at source ("Over 7 A for 31 s: remove the load"). Width and
drawing go through the same transform (`test_title_case_labels`).

To change sizes or characters, edit `SIZES` in `scripts/make-label-font.py`
and run it on the Liberation 2.1.5 release
(github.com/liberationfonts/liberation-fonts):
`scripts/make-label-font.py LiberationSans-Regular.ttf --bold LiberationSans-Bold.ttf`.
Flash: about 35 KB for the four sizes. The look was chosen from rendered
variants against the manual (2026-11-12; variants A-E2).

## What is still missing, in priority order

### 1. The blit function — the one real blocker

Find the routine that writes a rectangle of pixels to the panel. Then
`fb_blit()` is a few lines and every screen above runs on hardware.

Look for: a function configuring column/page address windows then streaming
halfwords to an SPI data register. The panel resolution is not yet established
(constant-frequency analysis was inconclusive: 128 appears 235 times, 240 94
times, 320 only 8), so **measure it** — count bytes in one full-screen update
with a logic analyser, or read it out of the controller init sequence.

### 2. Settings persistence

Power-on defaults need somewhere to live. The board has a 4-pin I2C header,
very likely an EEPROM holding calibration — the obvious home for settings too.
Dump it early (see `readout-protection.md`; it matters for the backup decision
as well). Fallback: a dedicated 2 KB flash page in the free region above
0x08020000, which costs nothing since half the flash is unused.

### 3. Signature-based symbol porting

Today `symbols/*.sym` is flat addresses tied to one image, so moving to a v3.x
dump means redoing them by hand. Better: store a short byte signature per
function and locate it by pattern in any image. Then the analysis survives
every firmware revision instead of being thrown away.

### 4. Input

Graph and config screens need button and rotary events. The 8-way ribbon goes
to the keypad; the handler is reachable from the TIM3/TIM4 interrupts already
identified.

## Suggested order

1. Keep designing screens on the host — no hardware needed, start now.
2. Extract the five stock fonts from the binary so screens look native.
3. When the J-Link arrives: back up, re-derive addresses, find the blit function.
4. Ship graph mode first — it is self-contained and proves the takeover hook.
5. Then the config screen, once settings storage is decided.
6. Decide separately whether re-theming stock screens is worth option 1.
