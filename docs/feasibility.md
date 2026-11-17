# Can the firmware be fully recreated?

Honest assessment as of 2026-11-06, after the Ghidra work.

## Three different questions

They get different answers, and conflating them causes wasted effort.

| Question | Answer |
| --- | --- |
| Recompile the decompilation into the same binary? | **No.** Not worth attempting. |
| Modify behaviour while keeping everything else working? | **Yes, today.** Ship-of-Theseus. |
| Write a wholly new firmware with full functionality? | **Yes, realistically** — roughly 70% of the knowledge is in hand. |

## Why recompiling the decompilation does not work

Ghidra's output is a reading aid, not source. It is full of `undefined4`,
`extraout_r3`, `in_r3`, unrecovered structs and stack artefacts — none of
which compiles. Turning 452 functions into building code is a per-function
manual rewrite, and **48% of the image is not decompiled at all** (some of
that is font and string data, but not all of it).

Separately, the original was built with **Keil armcc**, so even a perfect
rewrite would not produce matching output without that exact proprietary
compiler and its flags.

Recompilable decompilation is a research problem. For 116 KB of optimised
Thumb-2 it is not a hobby-scale task, and it buys provenance rather than
capability.

## What actually preserves existing functionality

**Ship-of-Theseus**, already scaffolded (`docs/modifying.md`). The stock
binary keeps running; your code lives in the free flash at 0x08020000 and
takes over only the functions you choose, calling back into stock code by
name through `symbols/*.sym`.

This is the only approach that preserves existing behaviour **by
construction** rather than by effort. Everything you have not replaced is
still OWON's working code.

## Subsystem readiness for a clean reimplementation

| Subsystem | State | What is missing | How to close it |
| --- | --- | --- | --- |
| **Display transport** | driver written and tested (`lib/display/`); pins and DCS command table recovered | panel init sequence | bootloader dump, Project Phoenix, or a stock ILI9486/ST7796 init |
| **Pixel format** | done — RGB565 high byte first | — | — |
| **UI layer** | done — screens, themes, trace (`lib/ui/`) | — | — |
| **Fonts** | font 4 (56×96) extracted and rendering; all five bases known | fonts 0 and 3 | offline; extract and verify by rendering |
| **AFE transport** | driver written, mock- and mutation-tested (`lib/afe/`) | — | — |
| **AFE mode/range values** | Digilent's 27-scale table as reference | this board's values | logic analyser, or adapt Digilent's |
| **SCPI wire protocol** | fully recovered | — | — |
| **SCPI grammar** | 64-entry keyword dispatch table recovered | index→handler routing | Ghidra, offline |
| **Settings + calibration** | three records mapped; calibration tables and DCV formula recovered | other functions' formulas; preferences layout | offline Ghidra; serial dump for real values |
| **Keypad** | mechanism known (2-row matrix) | bit→key mapping | breakpoint and press keys; minutes |
| **Power control** | ATtiny202 identified, role inferred | the `PWR_EN` handshake, if any | logic analyser from cold boot |
| **Clock tree** | recovered: HSE × 6, latency 2, PPRE1 /2 | confirm HSE fitted and its frequency | inspect board, or read RCC at runtime |

Nothing in the "missing" column is research. It is a day or two of bench work
plus some offline Ghidra reading.

## The honest bottom line

**The hard, uncertain parts are done.** The display driver was the one that
could have stayed blocked indefinitely, and it is recovered and implemented.
The AFE is a documented part with an open-source reference driver. The SCPI
grammar, fonts, storage map and keypad mechanism are all in hand.

What remains is mostly **confirmation against hardware**, not discovery. The
single highest-value session is now: attach the J-Link, back up, then in one
sitting read the command table, map the keypad, capture the AFE range writes
and watch `PWR_EN`. That closes most of the table above.

## Recommended order

1. **Ship-of-Theseus first.** Prove you can hook one stock function and have
   the meter still work. Low risk, and it validates the whole toolchain.
2. **Add screens on top of stock**, using `lib/ui/` and `lib/display/` while
   stock still owns measurement. This gets you themes, the graph screen and
   the config screen without reimplementing the hard parts.
3. **Replace subsystems only when there is a reason to.** The measurement
   loop and calibration are the least rewarding to rewrite and the easiest
   to get subtly wrong.

Full replacement is achievable, but "keep the stock firmware and grow into
it" gets you your three features far sooner and with far less risk.
