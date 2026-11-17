# Tables recovered from `.data`

All of these were invisible in the raw image because armcc stores `.data`
LZ-compressed. `scripts/unpack-data.py` expands it; `scripts/dump-tables.py`
walks the result. Everything here was obtained **offline, with no hardware**.

```sh
python3 scripts/unpack-data.py firmware/XDM1041_OS_V2.1.2_20210525.bin
python3 scripts/dump-tables.py firmware/XDM1041_OS_V2.1.2_20210525.bin
python3 scripts/dump-tables.py <image> --at 0x20000390      # one table
```

The encoding is consistent throughout: arrays of pointers into the flash
string region, split into groups by an `"end"` sentinel.

## SCPI dispatch — RAM 0x20001768, 64 entries

Stage 2 of the pipeline (`stock_scpi_stage2`, 0x080110EA) matches the command
against this table and stores the resulting index at `+0x3AC`, with a query
flag at `+0x3B2` when the token ends in `?`.

| Group | Keywords |
| --- | --- |
| 0 — IEEE-488 common (`*`) | `IDN`, `RST` |
| 1 — root | `INTernal`, `SYSTem`, `RANGE`, `AUTO`, `SHOW`, `RATE`, `MEASure`, `CALCulate`, `CONFigure`, `FUNCtion`, `TEMPerature`, `CONTinuity` |
| 2 — system / service | `UARTcontrol`, `JPG`, `BMP`, `TXT`, `WTXT`, `RESTart`, `SHOW`, `PWM`, `PROduce`, `CHANnum`, `UARTTEXT`, `DOWNLOAD`, `SOFTWARERESET`, `VERSION`, `MODEL`, `CHECKSUM`, `ANSWER`, `NOANSWER`, `START`, `HYREG`, `MEAS`, `AD`, `KEYRANDOM` |
| 3 — mode | `REMote`, `LOCal`, `BEEPer`, `DEF`, `MAXimum`, `MINimum`, `AUTO` |
| 4 — boolean | `OFF`, `ON` |
| 5 — CALC | `AVERage`, `DB`, `DBM`, `FUNCtion`, `NULL`, `STATe`, `CLEAR` |
| 6 — CALC function | `NULL`, `DB`, `DBM`, `AVERage` |

Group 2 is notable: `JPG`, `BMP`, `DOWNLOAD`, `HYREG`, `KEYRANDOM` and
`PROduce` are **undocumented service commands** absent from the user manual.
`HYREG` very likely gives direct HY3131 register access, which would be a
fast route to the mode tables - but read its handler first: several of these
commands write flash (`DOWNLOAD`, `VERSION`; see `settings-dump.md`).

## Range tables

These match the manual exactly, which is a good cross-check on the
decompression.

| Function | RAM | Ranges |
| --- | --- | --- |
| DC voltage | 0x200012F4 | 50 mV, 500 mV, 5 V, 50 V, 500 V, 1000 V |
| AC voltage | 0x20001310 | 500 mV, 5 V, 50 V, 500 V, 750 V |
| DC current | 0x2000132C | 500 µA, 5 mA, 50 mA, 500 mA, 5 A, 10 A |
| AC current | 0x20001348 | 500 µA, 5 mA, 50 mA, 500 mA, 5 A, 10 A |
| Resistance | 0x20001364 | 500 Ω, 5 kΩ, 50 kΩ, 500 kΩ, 5 MΩ, 50 MΩ |
| Resistance (ext) | 0x2000139C | 27 entries, including 100 MΩ |
| Temperature | 0x2000140C | `KITS90`, `Pt100` |

## Unit strings, indexed by range

The displayed unit changes with range, so each function has a parallel array:

| Function | RAM | Units |
| --- | --- | --- |
| DCV | 0x200011C0 | mVDC, mVDC, VDC, VDC, VDC, VDC |
| ACV | 0x200011DC | mVAC, VAC, VAC, VAC, VAC |
| DCI | 0x200011F8 | uADC, mADC, mADC, mADC, ADC, ADC |
| ACI | 0x20001214 | uAAC, mAAC, mAAC, mAAC, AAC, AAC |
| Resistance | 0x20001230 / 0x2000124C / 0x20001268 | Ω, kΩ, kΩ, kΩ, MΩ, MΩ |

A replacement firmware can lift these directly — they are part of matching
stock behaviour, not something to redesign.

## Menu tree — RAM 0x20000390, 172 entries

**86 menu items × 2 languages**, interleaved Chinese (GB2312) then English:

```
[0] <zh>   [1] Range      [2] <zh>   [3] Speed     [4] <zh>  [5] Filter
[6] <zh>   [7] input Z    [8] <zh>   [9] Rel      [10] <zh> [11] Function
[12] <zh> [13] Next      [14] <zh>  [15] Back    [16] <zh> [17] Low
[18] <zh> [19] Mid       [20] <zh>  [21] High    ...
```

That is the complete menu structure, including the System submenu
(Language, Backlight, Clock, Default, Model, Version, Serial Num, CheckSum).
It also explains the Language setting: the index selects which of the pair is
drawn.

## Other tables

| RAM | Contents |
| --- | --- |
| 0x20000080 | serial parity: `None`, `Odd`, `Even` |
| 0x20001284 | 20 entries mixing VDC and nF — probably the dual-display unit set |
| 0x2000003A | display command table (see `display.md`) |
| 0x20000640 | font base pointers (see `firmware-analysis.md`) |
