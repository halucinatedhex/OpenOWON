# Extracting the settings record over serial

**This needs no probe and no firmware changes** — a USB-serial cable and a
stock, working meter. It is the cheapest route to the **calibration tables**
and can be done today.

## The routine

`stock_dump_settings_uart` at **0x08006606**, reached from the SCPI dispatcher
at 0x080112B4 when the query flag at `+0x3B2` is **clear** — i.e. the command
is issued *without* a trailing `?`.

It writes, byte by byte, to USART1:

| Bytes | Contents |
| --- | --- |
| 4 | header `00 08 00 00` |
| — | ~100 ms delay |
| 20 | from `settings_ram + 0x28` — a tag or identifier block |
| — | ~100 ms delay |
| 1392 | the settings record from `0x20001A48` |
| 656 | `0xFF` padding, to a total payload of 2048 |

Total on the wire: **4 + 20 + 2048 bytes**.

## What it is and is not

> **Corrected 2026-11-07.** This section originally said the dump excluded
> calibration. The record it sends, 1392 bytes from 0x20001A48 (flash
> 0x0803F800), turned out to be the **factory record, which holds the
> calibration tables** (`calibration.md`). The 6004-byte record it does not
> send is the data log.

So this one command, on a stock meter with only a serial cable, gives:

- the factory `coef` / `zero` calibration tables
- the identity strings
- a ground-truth check of the layout in `calibration.md`: every `coef` slot
  should sit near 1,000,000 and inside the clamp limits

It does **not** include the user preferences (0x0803F000), so diffing it
across a menu change will mostly show nothing. Use SWD for that.

## Which command triggers it — `INT:TXT`, and NOT `DOWNLOAD`

> **Corrected 2026-11-07.** This said `SYST:TXT`. The service keywords hang
> off the `INTernal` root: Petteri Aimonen's XDM2041 tools read calibration
> with `INT:TXT` and skip the same 24 bytes we derived
> (`community-research.md`). `serial-dump.py` tries `INT:TXT` first and
> falls back to `SYST:TXT`.
>
> **Confirmed 2026-11-08** on our XDM1241, stock **V4.3.0**: `INT:TXT`
> answered with exactly 4 + 20 + 2048 bytes, and the record has 0
> implausible slots.

> **Corrected 2026-11-07.** This section used to call `DOWNLOAD` "the obvious
> candidate" and "safe to probe". **It is neither.**

Pinned statically. The service dispatcher looks the keyword up in the list at
RAM 0x200017A8 (`FUN_08010FFE`) and switches on the index:

| Index | Keyword | Handler (no `?`) | Safe? |
| --- | --- | --- | --- |
| 3 | `TXT` | `stock_dump_settings_uart()`, nothing else | **yes - this is the dump** |
| 4 | `WTXT` | not read; "W" plausibly write | no |
| 11 | `DOWNLOAD` | writes `0xA5A5` to prefs +8, `stock_store_write_prefs()`, then the next case (`SOFTWARERESET`) | **no: writes flash and resets** |
| 13 | `VERSION` | copies its argument into the factory identity, `stock_store_write_factory()` | **no: rewrites the calibration page** |
| 14 | `MODEL` | copies its argument into factory RAM +0x28 | no |

**Every keyword, from the v2.1.2 jump table** (`tbb` at 0x08011274;
2026-11-10). None reads memory out, so **the firmware can't be dumped over
serial**; the bootloader protocol only writes (`community-research.md`).

| Index | Keyword | Handler |
| --- | --- | --- |
| 0 | `UARTcontrol` | get/set a UART flag |
| 1, 2 | `JPG`, `BMP` | not handled here: error path |
| 3 | `TXT` | the settings dump, above |
| 4–10 | `WTXT`, `RESTart`, `SHOW`, `PWM`, `PROduce`, `CHANnum`, `UARTTEXT` | not handled here: error path |
| 11 | `DOWNLOAD` | update marker, then falls into 12 |
| 12 | `SOFTWARERESET` | reset |
| 13 | `VERSION` | rewrites the factory identity |
| 14 | `MODEL` | copies 20 bytes into the identity in RAM |
| 15 | `CHECKSUM` | prints one stored word, preferences +4. Not computed |
| 16, 17 | `ANSWER`, `NOANSWER` | reply on/off |
| 18 | `START` | factory auto-calibration (wipes DC cal if aborted) |
| 19 | `HYREG` | reads 56 bytes of HY3131 registers (0x08007908) |
| 20, 21, 22 | `MEAS`, `AD`, `KEYRANDOM` | get/set state flags |

V4.3.0 may differ, but nothing suggests a memory reader was added.

`DOWNLOAD` setting a flash marker and resetting is almost certainly stock's
**firmware-update entry**: the bootloader would see the marker and wait for a
new image. That is the serial update path on the wishlist, to confirm from
the bootloader dump.

Run the dump only through the script, which sends `INT:TXT` and refuses the
writing commands even if asked:

```sh
./scripts/serial-dump.py /dev/cu.usbserial-XXXX     # --baud if not 115200
```
