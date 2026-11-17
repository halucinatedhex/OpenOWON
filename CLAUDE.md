# OpenOWON — replacement firmware for the OWON XDM1241

Reverse-engineering an OWON XDM1241 bench multimeter the owner physically
possesses, and writing replacement firmware for it.

**Goal: a full firmware replacement with functional equivalence to stock.**
Not a patch on OWON's binary. The plan is `docs/replacement-plan.md`; read it
before starting new work.

Most analysis has been done **offline**. The meter is now on the bench
over serial and SWD, but **it is readout-protected** (2026-11-10): SWD can
identify the chip and nothing more. Firmware goes on **over serial,
through OWON's bootloader** (`docs/readout-protection.md`). Keep pushing
the offline track.

---

## Read these first

| Doc | What it holds |
| --- | --- |
| `README.md` | **the product page**: what the firmware is, advantages over stock, status |
| `docs/features.md` | **user guide**: keys, screens, menus, recording |
| `docs/developer-guide.md` | **developer guide**: setup, test loop, architecture, adding a feature |
| `docs/README.md` | index of every doc: using / developing / reference |
| `docs/test-plan.md` | **bench validation plan**: tests per function, limits, order of play |
| `docs/bench.md` | **the J-Link runbook: day 1 (read-only), first flash, day 2 captures** |
| `docs/replacement-plan.md` | **the plan** — architecture, subsystem status, milestones, risks, acceptance criteria |
| `docs/firmware-analysis.md` | the stock image: load address, vector table, SCPI pipeline, `.data` decompression, fonts |
| `docs/display.md` | panel driver — 8-bit parallel bus, pin map, command table |
| `docs/analog-frontend.md` | HY3131 AFE — registers, bus, pin map, board components |
| `docs/measurement.md` | **every function: registers → displayed value**, filter, REL, dB, temperature |
| `docs/data-tables.md` | SCPI dispatch, range, unit and menu tables recovered from `.data` |
| `docs/settings-storage.md` | the three flash records (log / preferences / factory), and the write path |
| `docs/calibration.md` | **calibration tables, encoding, and the DCV formula** |
| `docs/settings-dump.md` | **pull the factory + calibration record over serial, no probe needed** |
| `docs/feasibility.md` | what is and is not achievable, honestly |
| `docs/resources.md` | Project Phoenix, Digilent's HY3131 driver, other approaches |
| `docs/ui-development.md` | the host UI harness and how to design screens offline |
| `docs/app.md` | **the application, the host simulator, scenarios, memory budget** |
| `docs/scpi.md` | **remote control: commands, stock reply formats, the CONT/DIOD fix** |
| `docs/boot.md` | **flash map, clock, VTOR, pin gating, the two target builds** |
| `docs/modifying.md` | the code-cave/extension path (not the chosen route; tooling in `attic/extension/`) |
| `docs/backlog.md` | **the prioritised backlog**, hardware mods included |
| `docs/community-research.md` | **what other owners found**: versions, bootloader protocol, hidden menus, prior art |
| `docs/wishlist.md` | feasibility reasoning behind the backlog |
| `docs/readout-protection.md` | **our meter is protected**: what that blocks, what is recoverable, why serial |
| `docs/hardware.md` | board-level notes |
| `docs/bootloader.md` | **OWON's bootloader, read off the meter**: panel init, what an update erases |

---

## The one hard rule

**Never write to the chip before a verified backup exists.** The top 10 KB of
flash holds three records: the data log (0x0803D800), user preferences
(0x0803F000) and the **factory record with calibration (0x0803F800)**.
Overwriting calibration is irreversible and costs the meter its accuracy.
**Nothing may erase pages at or above 0x0803D800.**

`scripts/flash.sh` enforces both: it requires a verified backup, and it
refuses any image that would reach 0x0803D800 (even with the override flag).
A `PreToolUse` hook in `.claude/settings.json` blocks raw flashing commands
that try to go around it.

Reading the chip is always safe. Writing is never reversible. Treat that seam
as the boundary for asking permission.

**On our (protected) meter no SWD backup can exist.** The serial path's
equivalent gate, before the first install: the factory record dumped over
serial (done, `backups/serial-20261108-195034/`) **and** OWON's update file
for the stock app in hand (backlog B7a). The serial updater never sends
area 2 (calibration). **Never clear readout protection**: it erases the
whole chip, bootloader included.

---

## Hardware

| Item | Value |
| --- | --- |
| Digital board | `XDM1041_dig_v1.7` — shared with the XDM1041, so its prior art applies |
| MCU | **GD32F303CCT6A** — Cortex-M4, 256 KB flash, 48 KB SRAM, LQFP48 |
| Clock | HSE × 6, flash latency 2, PPRE1 /2 → 72 MHz if HSE is 12 MHz; falls back to HSI if no crystal. **Confirm the crystal is fitted.** |
| Debug | 5-pin header CO5: `RST · 3V · PA14 · PA13 · GND` (wired to GD32 pins 7/–/37/34/35; 3V reads 2.8 V). SWD only. **Stock V4.3.0 switches SWD off when running and is readout-protected**: connect under a manual reset, identify only (`docs/bench.md`) |
| Panel | 3.7" 480×320, **8-bit parallel 8080 bus, not SPI**. D0–D7 = PA0–PA7, RESET = PB1, D/C = PB2, WR = PB10. Standard MIPI DCS controller |
| AFE | **HYCON HY3131** on an isolated 4-wire SPI, bit-banged: CS = PB12, CK = PB13, DO = PB14, DI = PB15 |
| Isolation | NSI8141 (4-ch) + ELD207 dual opto; FN1-05S05 isolated DC-DC |
| Keypad | **mapped on our meter**: groups PB5–PB9 and PA14/PA15/PB3/PB4/PC13 take turns driving; 19 keys (`docs/hardware.md`, `lib/keypad`) |
| U6 | **ATtiny202**, beside the power circuit with power-button routing — almost certainly soft power control via `PWR_EN` |
| Probe | ST-Link V2 clone via OpenOCD: **SWDIO/SWCLK labels swapped, RST pin dead**. A J-Link is available but changes nothing on a protected chip |

The released firmware image is **v2.1.2, built for the v2.x STM32F103 board**
(proved by its 43-entry IRQ table). The owner's board is v3.x with the
GD32F303. Logic, strings and grammar transfer; **addresses will not** —
re-derive them against a v3.x dump.

---

## Insights that are expensive to rediscover

- **`.data` is LZ-compressed.** armcc stores initialised data compressed; the
  scatter table at 0x0801EDEC names ARM's `__decompress1`. Display opcodes,
  font pointers, SCPI tables and the menu tree are **not in the raw image in
  plain form** — several searches failed for this reason alone. Run
  `scripts/unpack-data.py` first, then `scripts/dump-tables.py`.
- **The display is parallel, not SPI.** Every SPI-shaped search failed before
  this was found. There are no SPI peripheral references anywhere.
- **The panel init is in the bootloader** (`docs/bootloader.md`), not the app image, and **PB1 is the panel CS, not RESET.**
- **There is no panel init sequence in this image.** The whole command
  vocabulary is DISPOFF/DISPON + CASET/RASET/RAMWR. The bootloader
  (0x08000000–0x080027FF, absent from the released file) must do the init.
- **The image loads at 0x08002800**, not 0x08000000.
- **Stock uses one's complement** for negative ADC readings, not two's. One
  LSB different from the datasheet — see `docs/measurement.md`.
- **Calibration is at 0x0803F800, not 0x0803D800.** The 6004-byte record at
  0x0803D800 is the 1000-point data log; earlier notes mislabelled it by size.
  Calibration is `int32 coef[11][10]` at +0x1F8 and `zero[11][10]` at +0x3B0,
  both ×10⁶ (`docs/calibration.md`).
- **Literal scans must follow `ldr` instructions.** Scanning the image for
  RAM-looking words matches instruction pairs (`movs r1,#1; movs r0,#0x20` =
  `0x20002101`). Only count words that a PC-relative load actually reads.
- **`stock_ad1_read_scale` is not the main reading path.** It is the
  large-capacitance (5 mF / 50 mF) charge-slope reader; DCV goes through
  `stock_meas_dcv`.
- **HY3131 register indices are not addresses.** Index 3 is RMS (0x09),
  10 is INTF (0x1E). Table in `docs/analog-frontend.md`.
- **Ghidra output is a reading aid, not source.** Recompiling it is not a
  route to a replacement; see `docs/feasibility.md`.

---

## Tools

### Hardware workflow (also exposed as `/probe`, `/backup`, … slash commands)

| Task | Command | Writes to chip? |
| --- | --- | --- |
| Identify probe + target, backup status (one hot attach) | `./scripts/probe-info.sh` | No |
| **Connect under manual reset**: identity, protection; `--backup` if unprotected | `! ./scripts/connect-reset.sh [--backup]` | No |
| Back up stock firmware, two-pass verified | `./scripts/backup.sh` | No |
| Back up the storage region (log, prefs, calibration) | `./scripts/backup-calib.sh` | No |
| **All of day 1** (probe, backups, serial dump, port symbols) | `./scripts/bench-day1.sh [serial]` | No |
| Factory record over serial (stock meter) | `./scripts/serial-dump.py <port>` | No |
| **Install over serial** through OWON's bootloader (area 5 only; `--stock` with OWON's file, `--no-way-back` for a first install without it, `--stock-gone --reset` for updates) | `! ./scripts/serial-update.py <port> <img> --stock-gone` (add `--confirm INSTALL` without a TTY; `--reset` reboots our running firmware into the bootloader, no button) | **YES** |
| Read the bootloader from our `xdm1241-serial` build | `./scripts/read-boot.py <port>` | No |
| Install the one-pack **flash dumper** (no way back to stock: OWON won't supply V4.x) | `! ./scripts/serial-update.py <port> .pio/build/xdm1241-dumper/firmware.bin --dumper` | **YES** |
| Read all 256 KB from the dumper; report survivors + calibration check | `./scripts/read-boot.py <port> --all` | No |
| **Guided validation against reference parts** (`docs/test-plan.md`) | `./scripts/bench-check.py <port> <check>` (`--list`) | No |
| Capture RTT console | `./scripts/rtt-capture.sh [s] [out]` | No |
| Capture HY3131 writes per range | `./scripts/capture-afe.py --dump ... --port ...` | No |
| First-flash smoke test (needs you at a terminal) | `! ./scripts/hil-smoke.sh` | **YES** |
| Are our log/settings pages free on this board? | `./scripts/check-regions.py <dump>` | No |
| Verify chip against a `.bin` | `./scripts/verify.sh [img]` | No |
| GDB server | `./scripts/debug.sh` | No |
| **Program the chip** (app slot 0x08002800) | `./scripts/flash.sh [img]` | **YES** |
| **Put stock back** from a backup | `./scripts/restore.sh` | **YES** |

Env vars: `PROBE`, `SWD_SPEED`, `JLINK_DEVICE`, `GDB_PORT`.

### Offline analysis

| Tool | Purpose |
| --- | --- |
| `./ghidra/run.sh` | headless Ghidra: import at 0x08002800, apply symbols, deepen, decompile |
| `./ghidra/find.sh fn <name>` | print one decompiled function |
| `scripts/fw-triage.py` | vector table, base detection, strings |
| `scripts/fw-annot.py` | disassembly with literals resolved to values and strings |
| `scripts/unpack-data.py` | **decompress `.data`** — do this before looking for any table |
| `scripts/dump-tables.py` | walk `.data` for string-pointer tables |
| `scripts/pinmap.py` | GPIO pin map from every `GPIO_Init` call |
| `scripts/extract-fonts.py` | locate and emit stock fonts |
| `scripts/emu-afe-table.py` | **run v2.1.2 in an emulator**: the HY3131 + relay writes for every function/range (needs `pip install unicorn`) |
| `scripts/port-symbols.py` | map v2.1.2 symbols onto another build (tested on a synthetic v3) |
| `symbols/*.sym` | recovered addresses (`attic/extension/gen-symbols.py` turns them into a linker script) |

Ghidra needs JDK 21+; `run.sh` pins `JAVA_HOME` to Homebrew's `openjdk@25`
because the system JDK 17 makes `analyzeHeadless` prompt and fail with no TTY.

### Build and test

```sh
make            # list targets
make check      # everything without hardware: test + sim + portable + both target builds
make test       # host unit tests, incl. end-to-end, power-cut, SCPI, menu, recording, ranging (122)
make sim        # run the whole meter on the Mac: scenarios/*.sim -> build/sim-*.png
make screens    # refresh the README's pictures (docs/screens/) from the simulator
make portable   # compile on-meter layers for Cortex-M4, -Werror
make ui         # render every UI screen × theme to build/*.png
make triage     # analyse the stock image
```

---

## Layout

```
lib/app/         the application: pipeline, ranging, keys,   [host-tested,
                 view, banded rendering; platform_t is the   end to end]
                 only way out
lib/store/       power-safe settings records, two flash pages  [host-tested,
                 at 0x0803B800                               power-cut fuzzed]
lib/console/     DIAG:BOOT? flash read-out format            [host-tested]
lib/keypad/      scan code -> key, debounce (map from our meter) [host-tested]
lib/datalog/     recording: 10,668 readings at 0x08030000    [host-tested,
                                                             power-cut fuzzed]
lib/scpi/        SCPI parser: short/long forms, queries, errors  [host-tested]
lib/sim/         HOST ONLY: bit-level HY3131, panel decoder, NOR flash, board
lib/ui/          framebuffer, themes, stock fonts, screens   [host-tested]
lib/display/     panel driver, 8-bit parallel                [host-tested]
lib/afe/         HY3131 driver                               [host-tested]
lib/measure/     every function's arithmetic, temperature,   [host-tested]
                 filter, dB, calibration record (meas.h, meas_temp.h)
lib/hal/         TARGET ONLY: GD32 registers, RTT console, platform_t;
                 pins gated by hal_pins.h (docs/boot.md)
src/             entry point: pin-free unless built as xdm1241-bench
linker/app.ld    app at 0x08002800, behind OWON's bootloader
test/            Unity tests, `pio test -e native`
scripts/         hardware workflow + offline analysis
ghidra/          scripted headless analysis (out/ is gitignored, regenerable)
symbols/         recovered addresses
firmware/        stock image
docs/            all findings
docs/photos/     board photographs (metadata stripped)
docs/manual/     user manual: not committed (README says where to get it)
backups/         verified chip dumps, committed in this private repo only (OWON's code; the public repo is a squashed export without them)
tools/           host UI renderer (`make ui`), meter simulator (`make sim`)
scenarios/       simulator scripts; each `expect` is a check
attic/extension/ code-cave patch path — not the chosen route, still builds
```

---

## Conventions

- **Docs move toward presenting the firmware.** At each milestone (serial
  update, J-Link bring-up, hardware testing...), update `README.md`
  (status, features, comparison with stock) and `docs/features.md` in the
  same change. Keep reverse-engineering detail in the reference docs, not
  the README. Every new feature is documented for users in `features.md`.
  Developer process lives in `docs/developer-guide.md`. Say plainly what is
  proven on the meter and what only in the simulator.

- **The app only reaches hardware through `platform_t`** (`lib/app/platform.h`).
  Anything new the app needs from the board is a new field there.
- **Never hold a full frame.** 480×320 is 300 KB; the chip has 48 KB. Draw
  through `fb_t` and let `app_render` band it.
- **Six target builds.** `xdm1241-dumper` (`src/dumper/`) is a standalone
  one-pack flash reader, not the app. `xdm1241` drives no pins (safe first flash);
  `xdm1241-serial` adds only the serial port (log console, `DIAG:BOOT?`:
  the first image for a protected meter); `xdm1241-panel` adds the panel (confirmed on the meter) and a test pattern; `xdm1241-ui` runs the whole app on the real panel, keys and beeper: **DC V, 2-wire ohms, continuity and diode on the real HY3131**, plus AC volts (R26 0x80; the 5 V range rolls off above ~100 Hz), frequency, capacitance on all seven ranges (50 nF-500 µF on the counters: R24 fix and full counter gate; 5 mF and 50 mF by charge slope; factory arithmetic), DC current on both jacks, AC current on both jacks (all six ranges since 2026-11-17; a voltage above ~2.15 V on the V jack adds to current readings: power mode subtracts it on the A jack, `platform_t.vi_leak`, B59), and temperature (type K; PT100/PT1000 on the confirmed ohms rows) (`lib/hal/afe_table.h`, confirmed rows only), every other function on lib/sim's model; `xdm1241-bench` drives the v2 pin map. Confirm a subsystem on the v3 board,
  then set its flag in `lib/hal/hal_pins.h`.
- **Nothing above `lib/hal/` touches a register.** Every layer takes an injected
  io struct so it runs against mocks on the host and hardware unchanged.
  This is what makes the offline track possible — preserve it.
- **Do not emit unverified data.** `extract-fonts.py` deliberately refuses to
  write fonts whose bases have not been confirmed by rendering. Follow that
  pattern: a wrong table is worse than a missing one.
- **Validate recovered data against something independent**, not by
  inspection. The `.data` decompressor was checked against a font base
  established separately by rendering glyphs; that caught a real error.
- Record negative results. Several dead ends are written up precisely so they
  are not retried.
- Do not add pin assignments on guesswork. Confirm, then record.
- `pio run -t upload` bypasses the backup guard. Use `scripts/flash.sh`.

---

## Where things stand

Done offline: display driver, AFE driver, UI harness with stock fonts, the
measurement path implemented and tested in `lib/measure/`, **the application
running end to end in the host simulator** (`make sim`), SCPI
grammar and dispatch table, range/unit/menu tables, clock tree, GPIO pin map,
the three flash records, calibration table layout, and the reading formula
for every function, plus filter, REL, dB/dBm and temperature.

Open, and roughly in priority order:

0. ~~Serial install~~ **works on our meter 2026-11-10** (B7): the
   `xdm1241-serial` build runs on it (`*IDN?`, heartbeat, `DIAG:BOOT?`
   matches the dumper's copy). Stock is gone for good; use
   `--stock-gone`. SWD is a
   dead end on this meter: readout protection ON (`FMC_OBSTAT` 0x03FFFFFE,
   2026-11-10). **Don't hot-attach SWD to a running stock meter in a
   loop**: it made the relays chatter.
1. ~~Real calibration values~~ **done 2026-11-08**: `INT:TXT` on our meter
   (stock **V4.3.0**) returned the factory record; 0 implausible slots, CJ
   gain 1732. Kept in `backups/serial-20261108-195034/` (committed).
2. ~~Panel init sequence~~ **found 2026-11-10** in the bootloader
   (`docs/bootloader.md`): ILI9488-class, picked by factory bytes
   +0x7E..+0x80. **Every area-5 update erases 0x08002800-0x0803CFFF**,
   our log and settings included (backlog B8).
2d. **HY3131 bus confirmed on the meter 2026-11-11** (read-only): pins
   by a passive check, then every register read twice (`DIAG:AFE?`,
   `docs/analog-frontend.md`). The chip sits at power-on defaults. **The
   per-range mode table can no longer be captured from stock** (erased):
   **recovered by emulating v2.1.2** (`scripts/emu-afe-table.py`,
   `docs/afe-table.md`): routing matches a real XDM2041 capture; relays
   are PA8 and PA13. Provisional until checked on the meter.
2e. **In progress 2026-11-11** (`docs/afe-table.md`, panel build
   `DIAG:CAP` / `DIAG:TEMP`): **capacitance** counters work (they need
   stock's restart: R20 0x70, CTA preset, R20 0x72) but the emulated rows
   read 4-5x high, per range differently: parked (B16). **Temperature**:
   cold junction reads 18.7 C, unchecked; thermocouple untested. The meter
   was left running `xdm1241-panel`; reinstall `xdm1241-ui` for the app.
2c. **Beeper confirmed on the meter 2026-11-11**: PB11, ~3 kHz
   (`DIAG:BEEP <ms>` on `xdm1241-panel`).
2b. **Panel confirmed on the meter 2026-11-11** (`xdm1241-panel`): CS = PB1
   low, MADCTL 0x28 for landscape (`docs/display.md`).
3. ~~Keypad bit → key map~~ **done 2026-11-11**: 19 keys on PB5–PB9 ×
   PA14/PA15/PB3/PB4/PC13 (`docs/hardware.md`, `lib/keypad`). Disconnect
   the ST-Link's SWD wires: unpowered, it holds PA14 low.
4. ~~`PWR_EN` handshake~~ **not needed** (2026-11-10): our pin-free build
   stays on indefinitely. The soft-off path (switching the meter off from
   firmware) is still unknown.
5. **Fonts 0 and 3** into the harness; font 4 already renders.
6. **AFE per-range register values** — adapt Digilent's 27-scale table, or
   investigate the undocumented `HYREG` SCPI command.

Undocumented SCPI service commands exist: `DOWNLOAD`, `JPG`, `BMP`, `TXT`,
`WTXT`, `HYREG`, `PROduce`, `KEYRANDOM`, `VERSION`, `MODEL`. **`DOWNLOAD` is
NOT safe**: it writes a marker to the preferences flash page and resets
(stock's firmware-update entry), and `VERSION <arg>` rewrites the factory
(calibration) page. **`INT:START` runs the factory auto-calibration and has
wiped a user's DC calibration; `start` sent at power-on enters the
bootloader, whose area 2 rewrites calibration.** Never enter the hidden
UTILITY-MATH/DUAL-UP-RANGE-DOWN menus (`community-research.md`). The read-only dump is `INT:TXT`. Only ever use
`scripts/serial-dump.py`, which refuses the writing commands
(`docs/settings-dump.md`). Read a handler before firing anything else.
