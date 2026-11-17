# Installing OpenOWON

OpenOWON installs over the meter's USB serial port, through OWON's own
bootloader: the same path OWON uses for its updates. No opening the case,
no debug probe.

> **There is very likely no going back to OWON's firmware.**
>
> OWON's firmware for current XDM1241 hardware (the v3.x digital board,
> firmware V4.x) is **not publicly available**, and OWON support declined
> to send it when asked (2026-11). The bootloader erases the whole
> application before writing a new one, so once OpenOWON is installed, the
> stock firmware is gone, and you can only put it back if you have OWON's
> update file for your exact hardware version.
>
> You also can't back the stock firmware up first: the chip is
> **readout-protected** (below).
>
> **Your calibration is kept.** It lives in its own flash page, which the
> installer never writes, and OpenOWON reads it, never changes it.

## Why there is no backup: readout protection

The meter's GD32F303 has readout protection switched on from the factory.
A debug probe can identify the chip, but it can't read the flash, so the
stock firmware can't be copied off the meter. The protection can be
cleared, but **clearing it erases the whole chip, bootloader included**,
and the bootloader is what makes serial updates (and recovery) possible.
Never clear it. Details: [`readout-protection.md`](readout-protection.md).

What *can* be read safely, over serial, is the **factory calibration
record**, and the installer insists you save it first.

## What you need

1. **The meter's USB cable and a computer with Python 3 and pyserial**
   (`python3 -m pip install pyserial`).
2. **The firmware image**: a release file, or build it:
   `pio run -e xdm1241-ui` → `.pio/build/xdm1241-ui/firmware.bin`.

## First install

1. **Save your calibration record** (read-only; writes
   `backups/serial-<date>/`):
   ```sh
   scripts/serial-dump.py /dev/cu.usbserial-XXXX
   ```
   Keep that folder somewhere safe.
2. **Install:**
   ```sh
   scripts/serial-update.py /dev/cu.usbserial-XXXX firmware.bin --no-way-back
   ```
   It checks the image, finds your saved calibration, and asks you to type
   `NO WAY BACK`.
3. When it says **"Switch the meter ON now"**, switch the meter off, then on.
   The bootloader only listens for a moment after power-on.
4. It writes the image in 10 KB packs, each acknowledged by the meter (about
   20 s in all), then starts OpenOWON.

If you do have OWON's update file for your meter, pass it instead of
`--no-way-back` (`--stock OWON_FILE.bin`), and keep it: it is your way back
(`--restore`).

## Updating

With OpenOWON running, no power cycle is needed:

```sh
scripts/serial-update.py /dev/cu.usbserial-XXXX firmware.bin --stock-gone --reset
```

The installer asks the meter `*IDN?`; only if OpenOWON answers does it
reset the meter into the bootloader and install. If anything else answers
(or nothing), it stops without writing. Every update rewrites the whole
application area, so **settings and recordings are cleared** by an update.

## If something goes wrong

The bootloader is never touched, and it runs first at every power-on. So
whatever state the application is in (half written, crashing), switching
the meter on while the installer is calling gets you back into the
bootloader. Run the installer again.

## What the installer checks, and what it never does

| | |
| --- | --- |
| Writes calibration (bootloader area 2) | **never**: there is no option for it, and the code refuses any area but 5 |
| Image linked for the app slot | stack pointer in RAM; reset and exception vectors inside the image at 0x08002800 |
| Size | OpenOWON must end before 0x08030000, where its own data log starts |
| First install without OWON's file | `--no-way-back`, a saved calibration record, and the phrase typed |
| Updates | the meter must answer as OpenOWON before anything is written |
| You are there | needs a terminal and the phrase typed (or `--confirm` with it) |
| Every step | acknowledged by the meter within 10–30 s, or it stops |

Check what it would do without touching the meter: add `--check`.

The bootloader protocol, from OWON's updater as captured by Petteri
Aimonen, is in [`community-research.md`](community-research.md); what the
bootloader erases is in [`bootloader.md`](bootloader.md).
