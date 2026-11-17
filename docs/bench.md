# Bench runbook

What to do when a debug probe arrives, in order. Each step says whether it
writes the chip. Everything up to "First flash" is read-only.

> **Result on our meter (stock V4.3.0, 2026-11-10): readout protection is
> on.** The probe connects (under reset) and identifies the chip, but every
> flash read is refused, so **no SWD backup is possible and the SWD-based
> steps below (backup, port-symbols, first flash, day 2) do not apply to
> it.** The project now installs over serial, through OWON's bootloader
> ([`readout-protection.md`](readout-protection.md)). This runbook still
> applies to an unprotected meter; check yours with step 5 below.

## Setup

```sh
brew install --cask segger-jlink     # JLinkExe, JLinkGDBServer, JLinkRTTLogger
make check                           # everything still green before touching hardware
```

## Connecting a probe

**Where:** header **CO5**, on the bottom edge of the digital board
(`XDM1041_dig_v1.7`), right beside the input jacks. It is five plated
through-holes at 2.54 mm pitch, **with no pins fitted**, under the clear
insulating film (`photos/digital_front_1.jpg`). Left to right:
`RST · 3V · PA14 · PA13 · GND`.

**Attaching to it, either way:**
- **Solder in a 1×5 2.54 mm pin header (recommended).** It's reusable for
  every session after this one. Lift the film first. If there isn't room to
  close the case over straight pins, use a right-angle header, or leave the
  case open while developing.
- **No soldering, for a first session:** push a header into the holes at a
  slight angle so the pins bear on the plating, and tape it. Do the reads
  with the two-pass verify, which catches a bad contact.

**Wiring:**

| CO5 | Signal | J-Link 20-pin (BASE, PLUS, EDU) | J-Link EDU Mini (10-pin, 1.27 mm) | ST-Link V2 clone (USB stick, 10-pin) | ST-Link/V2 genuine (20-pin) |
| --- | --- | --- | --- | --- | --- |
| `3V` | VTref, sense only | 1 | 1 | **do not connect**: the clone's 3.3 V pin is an *output* | 1 (VAPP, sense) |
| `PA13` | SWDIO | 7 | 2 | SWDIO **by the label; on ours the labels were swapped** | 7 |
| `PA14` | SWCLK | 9 | 4 | SWCLK (ditto) | 9 |
| `RST` | nRESET | 15 | 10 | **useless on ours**: its RST pin never drives NRST. Ground CO5's RST by hand instead | 15 |
| `GND` | GND | 4 (or any even pin 4–20) | 3 or 5 | GND | 4 |

**ST-Link V2 clone: proven on our meter (2026-11-09/10).** It goes through
OpenOCD (`brew install open-ocd`); the scripts detect it, or set
`PROBE=stlink`. Ours enumerates as "STM32 STLink", firmware V2J37S7.
- **Its labels can lie.** Ours connected only with SWDIO and SWCLK the
  opposite way round from the printed label. Continuity checks prove your
  wires match the labels, not that the labels match the stick, so if
  everything beeps and nothing answers, swap those two.
- **Its RST pin does nothing.** OpenOCD's `connect_assert_srst` had no
  effect, and the meter booted normally with it wired to NRST. Use the
  manual reset below.
- **Its "target voltage" is its own.** It reads about 3.27 V whether or not
  the meter is on, so it tells you nothing about the target.
- **Identifies as:** IDCODE `0x21040414` (device 0x414, the STM32F10x
  high-density class the GD32F303 imitates), `Cortex-M4 r0p1`.
- **Firmware updates:** don't update a clone with ST's own tool; it can
  reject or brick counterfeits.

**Would a J-Link do better?** Easier, not different. It drives a real reset
line (no jumper, no timing) and has native GD32F303 support. Readout
protection is enforced by the chip, so a J-Link is refused the same flash
reads, and its GD32 "unlock" is the same full-chip erase.

Female-to-female jumper wires fit both CO5's header and the 20-pin
connector; keep them under about 15 cm. The EDU Mini's 1.27 mm connector
needs a breakout board.

**Power and order:**
1. Unplug every test lead from the jacks. The header sits beside them, and
   the J-Link ties the meter's digital ground to your computer.
2. Connect the J-Link with the meter **off**.
3. Power the meter as usual: battery, or USB-C for long sessions. **Never**
   let the J-Link supply power. Its 5 V output (pin 19) is off by default;
   leave it off, and never send `power on`.
4. Switch the meter on. With it off, the GD32 has no power and the probe
   reports the target voltage as too low.
5. Run `./scripts/probe-info.sh` **once**. It should show a GD32F303CC
   and its protection status. **On stock V4.3.0 it gets no answer**: the
   running firmware switches SWD off. Use `./scripts/connect-reset.sh`
   (next section) instead. Never retry in a loop (see below).

**If things go wrong:**
- **The scripts default to SWD at 1 MHz.** If reads disagree, try
  `SWD_SPEED=500`, or shorten the wires.
- **If the meter switches itself off while the probe holds the CPU halted,**
  that is the ATtiny202 power controller's handshake (`PWR_EN`), and it's
  worth noting exactly when it happens. Backups halt only for seconds, and
  the two-pass verify catches an interrupted read.

## Connecting under reset (stock V4.3.0)

Stock V4.3.0 takes PA13/PA14 over once it runs, so SWD only answers while
the chip is held in reset. The debug port keeps working in reset; the
script arms a halt on the first instruction, you release reset, and the
core stops before the firmware runs.

```sh
! ./scripts/connect-reset.sh            # identity + readout protection
! ./scripts/connect-reset.sh --backup   # ...plus a two-pass backup, if unprotected
```

It prompts you through it:
1. Meter off. SWDIO, SWCLK, GND connected.
2. Connect CO5's **RST** hole to GND. With a clone, plug the RST lead onto
   the stick's spare GND pin (the 10-pin header has two).
3. Switch on and **keep holding the power button**. The screen stays blank
   (the chip is in reset; the relays still click as power comes up).
4. Press Enter. When it prints **RELEASE RESET NOW**, take the RST lead off
   ground. It waits up to 3 minutes.
5. The core is left halted; switch the meter off and on to run stock again.

Writes only the core's debug registers (DEMCR, DHCSR), never flash.

**Do not hot-attach to a running stock meter repeatedly.** A loop of about
15 connection attempts a second made the relays click wildly: SWD traffic
on PA13/PA14 looked like inputs to the firmware. Key-like inputs are how the
calibration-wiping hidden menus are reached. Calibration was checked
afterwards (`INT:TXT`, byte-identical to the day before), but don't repeat
it. `probe-info.sh` makes exactly one attempt.

### What our meter reported

```
IDCODE      e0042000: 21040414
FMC_OBSTAT  4002201c: 03fffffe     bit 1 (SPC) = 1: protection ON
OB_SPC      1ffff800: ffffff00     SPC byte 0x00 (0xA5 = unprotected)
FlashSize, UID, all flash:  READ FAILED
reset vector loaded as SP 0xFFFFFFFC, PC 0xFFFFFFFE
```

The last line matters: with a debugger attached, even the **core's own**
fetch of the reset vector read back as all ones. So on a protected chip a
debugger can't just watch stock run; there is nothing to step through.

### The path that got here, so it isn't retried

| Attempt | Result | Why |
| --- | --- | --- |
| Hot attach, 1 MHz and 100 kHz, `hla` and `stlink-dap` | no answer | firmware has SWD off, and labels swapped |
| Swap SWDIO/SWCLK while hot | no answer | firmware has SWD off |
| `connect_assert_srst` through the clone's RST pin | no answer | the pin does nothing |
| Power-on race (connect loop while switching on) | relays chattered; probe dropped off USB | **don't**: see above |
| Manual reset, labels as printed | no answer | labels swapped |
| Manual reset, SWDIO/SWCLK swapped | **connected, halted, protection ON** | |

Checks that ruled out wiring on the way: CO5's 3V hole reads **2.8 V** with
the meter on (it decays slowly when off: it is the real rail); continuity
CO5 PA13 → GD32 pin 34, PA14 → pin 37, RST → pin 7; GND → pins 35 and 47.

## Day 1: read everything (read-only)

```sh
./scripts/bench-day1.sh /dev/cu.usbserial-XXXX     # serial port optional
```

| Step | Produces | If it fails |
| --- | --- | --- |
| probe-info | chip ID, flash size | check wiring and that the meter is on; `SWD_SPEED=500` |
| backup | `backups/<stamp>/flash.bin` + `VERIFIED` | all-0xFF: readout protection, see `readout-protection.md` |
| backup-calib | storage region on its own | |
| serial-dump | `backups/serial-<stamp>/` factory record, layout check | wrong baud (stock's serial menu sets it) |
| port-symbols | `symbols/xdm1241_v3.sym`, each line saying how it was found | check `UNMAPPED` lines; most are unused helpers |
| bootloader | `bootloader.bin`, `app.bin` beside the dump | |
| regions | whether the data-log and settings pages are erased | a region in use: leave its flag off |

Then **commit `backups/` and push it** before anything else, so the dumps
exist off this machine. The repo is private; the dumps hold OWON's firmware,
so they must come out of history before it ever goes public.

Things to look at straight away, all offline:
- `report.txt` from the serial dump: 0 implausible slots confirms the
  calibration layout (`calibration.md`) on your board.
- The regions step says whether 0x08030000-0x0803A7FF (data log) and
  0x0803B800-0x0803D7FF (settings) are erased. Set `HAL_LOG_ENABLED` and
  `HAL_STORE_ENABLED` (`lib/hal/hal_pins.h`) only for the ones that are.
- The bootloader: its panel init sequence, and what it does with the
  `0xA5A5` marker `DOWNLOAD` writes (the serial update path, `settings-dump.md`).
- Rerun the analysis on `app.bin`: `make triage`, `ghidra/run.sh` with the v3 symbols.

## First flash (writes the app slot)

```sh
! ./scripts/hil-smoke.sh       # you type FLASH; it refuses without a terminal
```

Flashes the **pin-free** build (drives no pins), captures RTT and checks:
banner, 72 MHz clock, VTOR = 0x08002800 (the bootloader handed over),
calibration found, 1 Hz heartbeat. `./scripts/restore.sh` puts stock back.

## Day 2: let stock generate the data (restore stock first)

| Capture | Command | Becomes |
| --- | --- | --- |
| HY3131 writes per function/range | `scripts/debug.sh &` then `scripts/capture-afe.py --dump <flash.bin> --port <serial>` (or `--manual`) | the `afe_configure()` table |
| Pin confirmation | GDB `watch` on GPIO ODR registers, or a logic analyser | set `HAL_V3_CONFIRMED_*` in `hal_pins.h` |
| Keypad map | breakpoint the key scan, press each key | `key_poll()` in the HAL |
| `PWR_EN` | poll GPIO from power-on to power-off | power hold in the HAL |
| ATtiny202 (U6) pins, CO2 traffic, off-state current | logic analyser on U6 and CO2 through power on, off and charging; current at the battery | the ATtiny firmware plan (`power-controller.md`) |

`capture-afe.py` steps through every function and range over SCPI. Stock's
exact `CONF`/`RANGE` syntax is not recovered, so if the meter ignores the
default plan, edit one (`--plan`, `label | cmd ; cmd` per line) or use
`--manual` and press the keys when asked.

## Then: the bench build

Once pins are confirmed, `pio run -e xdm1241-bench` and flash it with
`./scripts/flash.sh .pio/build/xdm1241-bench/firmware.bin`. That is the
full app, the same code `make sim` runs.

## Safety, in one place

- Nothing in this project erases at or above 0x0803D800 (`flash.sh` refuses).
- `flash.sh` writes only the app slot, only images linked for it, only with a
  verified backup.
- `hil-smoke.sh` needs you at a terminal.
- Never send `DOWNLOAD`, `VERSION <arg>`, `WTXT`, `MODEL`, `PROduce`,
  `HYREG`, `KEYRANDOM`, `RESTart` or `SOFTWARERESET` to a stock meter by hand.
  The scripts refuse them.
