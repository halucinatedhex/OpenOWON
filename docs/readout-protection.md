# Readout protection

**Our meter has it.** Stock V4.3.0 on a `XDM1041_dig_v1.7` board, checked
2026-11-10 by connecting under reset (`bench.md`): `FMC_OBSTAT.SPC = 1`,
option byte SPC = `0x00`. Expect the same on any recent meter. Check yours
with `./scripts/connect-reset.sh`; it reports `PROTECTION ON` or `off`.

## What it blocks, and what it doesn't

| | Blocked? |
| --- | --- |
| Reading flash over SWD, with **any** probe (ST-Link, J-Link, CMSIS-DAP) | **yes**: every read fails |
| The core fetching code while a debugger is attached | **yes**: after reset it loaded SP/PC as all ones. You can't single-step stock |
| Reading peripherals and SRAM over SWD (`FMC_OBSTAT`, `IDCODE`) | no |
| Code running on the chip reading its own flash | no: that is how stock serves `INT:TXT` |
| OWON's bootloader writing a new app (area 5) | no: that is the serial update path |
| Removing it | only by erasing the **whole chip**, bootloader included |

## What is recoverable without removing it

| Region | Recoverable? | How |
| --- | --- | --- |
| Factory record with **calibration** (0x0803F800) | **yes, done** | `INT:TXT` over serial (`settings-dump.md`). Ours is in `backups/serial-20261108-195034/`, read twice, byte-identical |
| Stock application (0x08002800) | **yes, from OWON** | the update file OWON sends owners is a plain image of this slot (`community-research.md`). Ask OWON support for the current version for your meter |
| Bootloader (0x08000000–0x080027FF) | **not directly** | no serial command reads memory (`settings-dump.md`). Once our app runs, it can read the bootloader out over serial: protection does not stop code on the chip |
| Preferences and stock data log | no | nothing reads them out; minor |

## The decision: install over serial, keep protection

We keep OWON's bootloader and the protection, and install our firmware into
the app slot through the bootloader's serial protocol
(`community-research.md`, backlog B7). This needs no probe and no opening of
the case, so it suits most owners too. Going back to stock means sending
OWON's update file the same way.

What this costs:
- **No SWD debugging on a protected meter.** The debug console has to go
  over the serial port instead of RTT (backlog).
- **The bootloader is a black box until our app can read it out.** That
  includes the panel init sequence and what area 5 erases (backlog B8).

## The alternative, recorded but not chosen: mass erase

Clearing protection erases all 256 KB. You get full SWD, RTT and
breakpoints, but:
- OWON's bootloader is gone for good, and with it the serial update path
  and the only known panel init sequence;
- the stock app is gone (recoverable only from OWON's update file, and it
  can't boot without the bootloader);
- calibration is erased too: it would have to be written back from the
  serial dump;
- the meter stays dark until our own startup code and panel init work.

It also breaks the project's hard rule: no write without a verified backup.
**Nothing in this repository does it.** If you ever choose it, do it
deliberately, by hand, with the factory record and OWON's update file in
hand.

## Earlier notes, corrected

- This page used to say calibration would be "genuinely unrecoverable" if
  SWD was locked. It isn't: `INT:TXT` returns the whole factory record.
- There is no EEPROM: the 4-pin `scl · sda · gnd · 3v` header is unused by
  stock firmware, and calibration lives in internal flash.
