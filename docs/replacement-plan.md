# Plan: full firmware replacement

**Goal.** A wholly new firmware for the XDM1241 with functional equivalence to
stock, built in PlatformIO, developed and tested offline, **installed over serial
through OWON's bootloader** (our meter is readout-protected: SWD can't back
it up or debug it, 2026-11-10, `readout-protection.md`).
Not a patch on OWON's binary — a replacement.

"Equivalent functionality" is defined concretely in the acceptance list at the
end. Everything in this plan serves that goal.

## Architecture

```
  lib/app/      screens, measurement loop, keys        [host-tested end to end, docs/app.md]
  lib/sim/      host-only board: HY3131 + panel models
  lib/ui/       framebuffer, themes, fonts, widgets      [host-testable]
  lib/measure/  scaling, ranging, formatting             [host-testable]
  lib/afe/      HY3131 driver                            [host-testable via mock]
  lib/display/  panel driver                             [host-testable via mock]
  lib/store/    settings + calibration persistence       [host-testable]
  hal/          GD32 registers: GPIO, timers, USART, flash, clock
```

The rule that makes offline development work: **nothing above `hal/` touches a
register.** Every layer is driven through an injected io struct, so the same
code runs against mocks on the laptop and against hardware unchanged. This is
already true of `lib/afe/` and `lib/display/`.

## Status by subsystem (2026-11-17)

**The replacement runs on our meter** (`xdm1241-ui`), installed over
serial. What's proven where is in `test-plan.md`; what's next in
`backlog.md`.

| Subsystem | State |
| --- | --- |
| Display | **on the meter**: panel init by the bootloader, our driver, banded rendering, five themes |
| Fonts | stock's reading digits; labels in Liberation Sans (stock's metrics) |
| Keypad | **on the meter**: 19 keys mapped |
| Beeper, backlight | **on the meter** |
| HY3131 bus and rows | **on the meter**: rows recovered by emulating v2.1.2, each checked against a reference before use (`afe-table.md`) |
| Measurement | **on the meter**: DC V, AC V, ohms, continuity, diode, frequency, capacitance (all seven ranges, to 50 mF), DC and AC current on both jacks, temperature (K; PT100/PT1000) |
| Calibration | **on the meter**: stock's factory record, read-only; user calibration on top (simulator) |
| Application | ranging, REL/HOLD/stats/dB, dual display, power and energy, graph, histogram, recording, Utility menu, SCPI: **on the meter** |
| Settings and log | **on the meter**: power-safe records, 10,668-point log |
| RTC, battery | **on the meter**: clock on its cell; battery on PB0 |
| Power control | open: soft power-off and the off-state drain (`PWR_EN`, the ATtiny202: B46-B49) |

> The offline and hardware tracks below are the plan as written before
> the meter arrived, kept for the record. Most of it was done another way
> (emulation instead of a logic analyser; serial instead of SWD).

## Offline track — everything achievable without the meter

Ordered so each step unblocks the next. All of this can be done now.

### O1. Finish the Ghidra sweep
Function coverage of the code region is ~82% (468 functions). The remaining
gaps are mostly literal pools and switch tables; mine systematically.

Already closed: the display command table (via `.data` decompression), the
GPIO pin map, the SCPI keyword table, range/unit/menu tables, and the common
measurement scaling path. Still open:

- **Over-range rules** (`FUN_08009FE8`) and min/max/average, the last pieces
  of the measurement path (`measurement.md`).
- **SCPI handler routing.** Follow stages 3/4 of the pipeline from the
  keyword index to the per-command handlers.
- **Measurement extras.** RMS, dB/dBm, rel, min/max/average paths.
- **Menu tree structure.** The strings and tables are recovered; the code
  that walks them is not.
- **Panel init.** ~~Absent from the app image~~ found in the bootloader
  (`bootloader.md`, 2026-11-10).

### O2. Extract fonts 0 and 3
Font 4 is done. All five bases are now known from `.data` (see
`firmware-analysis.md`): font 0 lives in RAM at 0x20000A34, font 3 at
0x08016B48, and font 1 is an alias for font 2. What remains is extraction plus
verification by rendering.

### O3. Build the Renode machine — the big offline lever
Run the **stock firmware** in emulation and watch it work. Renode models
Cortex-M and STM32F103 peripherals; a `.repl` with GPIO and a stub panel lets
you observe, with no hardware:

- the exact display init byte stream
- AFE register writes per mode and range
- keypad scanning behaviour
- the boot sequence and timing

This is a logic analyser that needs no meter, and it answers several
hardware-gated questions early. Worth building before the J-Link arrives.

### O4. Write and test the portable layers
Against mocks, with the harness already in place: menu tree, measurement
math, scaling, formatting, SCPI parser and dispatcher, settings
serialisation. All host-testable.

### O5. Stand up the GD32 HAL
Clock (HSE × 6, latency 2, PPRE1 /2 — recovered), GPIO, timers, USART, flash.
Compiles and is reviewable offline even though it cannot be run.

## Hardware track — one focused bench session

> **Changed 2026-11-10.** Our meter is readout-protected, so steps 1, 2, 4,
> 5 and 7 below (all SWD reads or breakpoints on stock) are impossible on
> it. The serial route replaces them: dump the factory record over serial
> (done), get OWON's update file (backlog B7a), install over the
> bootloader (B7), log over the USART (B7c), and read the bootloader out
> from our own app (B7d). Captures of stock's behaviour (AFE writes,
> keypad, `PWR_EN`) move to a logic analyser. The list is kept for an
> unprotected meter.

When the J-Link arrives, in roughly this order. Most of it is confirmation,
not discovery.

1. `./scripts/backup.sh` and `./scripts/backup-calib.sh`. Nothing else until
   these pass.
2. Dump the bootloader (0x08000000–0x080027FF) — it holds the panel init.
3. Confirm HSE is fitted and its frequency (read RCC, or inspect the board).
4. Map the keypad: breakpoint `stock_keypad_scan`, press every key.
5. Capture AFE register writes while cycling function and range. This is the
   mode table for *this* board.
6. Watch `PWR_EN` and the ATtiny202 link from cold boot through power-down.
7. Read 0x0803F000 across one menu change to get the preferences layout.

## Milestones

| # | Milestone | Gate |
| --- | --- | --- |
| M0 | Backups verified: factory record over serial (**done**); OWON's update file (not available: stock is gone for good) | before any write |
| M1 | Heartbeat from own firmware, installed over serial, logging on the USART | **done 2026-11-10** |
| M2 | Panel initialised, a filled rectangle on screen | **done 2026-11-11** |
| M3 | Stock fonts rendering a static reading | **done 2026-11-11** |
| M4 | Keypad events driving screen changes | **done 2026-11-11** |
| M5 | AFE returns a plausible DC voltage | **done 2026-11-11** |
| M6 | Calibration applied, reading matches a reference | **done 2026-11-14** (DC V within 0.03%) |
| M7 | All functions and ranges working | **done 2026-11-17** (AC current last) |
| M8 | SCPI interface answering existing host tooling | **done** (stock's reply formats) |
| M9 | Settings persist across power cycles | **done 2026-11-14** |

M1–M4 need no AFE knowledge at all. M2 and M3 are reachable almost
immediately once hardware is attached, because the display and font work is
already done.

## Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Readout protection blocks backup | **happened** (our meter, 2026-11-10) | calibration recovered over serial anyway; install over serial; stock app from OWON's update file |
| Our app bricks the serial path (e.g. `PWR_EN`) | meter won't stay on | the bootloader is entered at power-on before our app runs (`start`), so a bad app can always be replaced |
| `PWR_EN` handshake not reproduced | meter powers off mid-boot | capture it before writing firmware; symptom is a clean boot then death after a timeout |
| v3.x addresses differ from v2.x | all recovered addresses wrong | re-derive against own dump; signature-based symbol porting would make this mechanical |
| Calibration layout misread | plausible but wrong readings | validate against references at M6, not by eye |
| Panel is a different controller on v3.x | display work partly wasted | the transport is generic; only the init sequence would change |

## Acceptance — "equivalent functionality"

- DCV, ACV, DCI, ACI, resistance (2-wire; no 4-wire on this meter), continuity, diode, capacitance,
  frequency, period, temperature (PT100 / ITS-90)
- Auto and manual ranging across the stock range set
- Rate Low/Med/High, true RMS, relative, dB/dBm, min/max/average, hold
- Dual display
- The full SCPI command set, compatible with existing host tooling
- Menu system with power-on defaults, persisted
- Backlight, beeper, RTC, battery operation
