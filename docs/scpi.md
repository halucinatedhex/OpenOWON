# SCPI remote control

`lib/scpi` (parser) + `lib/app/app_scpi.c` (commands). Host-tested
(`test_scpi`, `test_app`), and scriptable in the simulator (`scenarios/scpi.sim`).
On the meter: USART0 (PA9/PA10), 115200 8N1, enabled in the bench build
(`HAL_SERIAL_ENABLED`). Stock answers at 115200 too (observed). Replies end
in `\r\n`, as stock's do.

## What comes from stock

| Behaviour | Source in the v2.1.2 image |
| --- | --- |
| Keyword set | dispatch table, `data-tables.md` |
| `MEAS?` → `%E` of the value in SI base units | format `%E` at 0x08011660; value × 10^unit-exponent |
| Overload → `1E+9` | 0x08011AE0 |
| Secondary reading joined as `%s,%s`; none → `NONe` | 0x08011AE8, 0x08011AF0 |
| `FUNC?` → quoted `"VOLT"`, `"VOLT AC"`, `"CURR"`, `"CURR AC"`, `"RES"`, `"FRES"`, `"CONT"`, `"DIOD"`, `"CAP"`, `"FREQ"`, `"TEMP"`, `"PER"` | table at 0x0801E310 |
| `RATE` S/M/F, either case; refused for continuity..temperature | 0x08011D00 region |
| Temperature sensors `KITS90`, `PT100`; `TEMP:RTD:TYPe/UNIT` | 0x0801E2F0 |

## Stock V4.3.0, observed on our meter (2026-11-08)

Queries only, over the meter's own USB serial (CH340, 115200 8N1):

| Sent | Stock replied | Ours |
| --- | --- | --- |
| `*IDN?` | `OWON,XDM1241,12345678,V4.3.0,3` (the last field is probably the hardware revision) | `OWON,XDM1241,<serial>,<version>` |
| `FUNC?` | `"VOLT"` | same |
| `MEAS?`, `MEAS1?` | `-6.511538E-05` | same format |
| `MEAS2?` | `NONe` | same |
| `RANGE?` | text, in each function's own form (next table) | **now** the same (was a number) |
| `AUTO?` | `1` | same |
| `RATE?` | `S` | same |
| `SYST:ERR?`, unknown queries | no reply at all | standard error queue: a superset, harmless |
| every reply | ends `\r\n` | **now** the same (was `\n`) |

### `RANGE?` forms (stock V4.3.0, two sessions stepping the front panel)

| Function | Seen | Notes |
| --- | --- | --- |
| DC / AC volts | `500 mV`, `5 V` | |
| Resistance | `500 Ω`, `5 KΩ` … `500 KΩ`, `5 MΩ`, `50 MΩ` | Ω in UTF-8 (`CE A9`), **capital K** |
| DC / AC current | `500 uA`; `5 A`, `10 A` | ASCII `u`. Two separate ladders, see below |
| Capacitance | `50 nF`, `500 nF`, **`5uF`, `50uF`, `500uF`**, `5 mF`, `50 mF` | **no space** before `uF` only: a stock quirk, copied |
| Frequency / period | `Hz` / `s` | `FUNC?` gives `"FREQ"` / `"PER"` |
| Temperature | `KITS90`, **`Pt100`** | |
| Continuity, diode | no reply | `FUNC?` gives `"CONT"` / `"DIOD"`: **correct on V4.3.0**; the swap was reported on 3.3.1. Ours sends no reply line either |

**Not seen yet,** so ours follows the pattern and is unverified: `5 mA`,
`50 mA`, `500 mA`.

**Current is two inputs.** In DC current on the A ranges, range DOWN stayed
at `5 A` and UP went only to `10 A`; the µA/mA ranges appeared separately.
Current flows through whichever jack the lead is in, so stock never ranges
between them. Ours now does the same (`readout_range_span`, F5 switches
input).

**Everything else seen (tests 4 and 5, 2026-11-08):**
- Every setting command is silent.
- `;` separates commands only without a following space (`FUNC?;RANGE?`).
- A leading colon (`:MEAS?`) gets no reply.
- No reply to `CONF?`, `CALC:STAT?`, `CALC:AVER:COUN?`, `CALC:DBM:REF?`,
  `SYST:VERS?`, `*OPC?`, `*ESR?`.

Ours accepts the space and the colon, and answers `COUN?` and `DBM:REF?`.
No tool can depend on stock's silence there.

**Dual display (test 6):** DUAL did nothing visible over serial in DC volts,
frequency, resistance or capacitance. No softkey appeared, and `MEAS2?`,
`FUNC2?` and `MEAS2:SHOW?` stayed `NONe`. That is as the manual says: dual
display supports only **AC volts + frequency** and **AC current + frequency**
(`features.md`, Dual display).

**With dual display on** (captured, AC volts and AC current + Freq):

| Query | Stock replies |
| --- | --- |
| `MEAS?` | **both**, comma-separated: `0.000000E+00,1.460261E+01` |
| `MEAS2?` | the secondary: `1.460261E+01` (`NONe` when dual is off, in frequency mode too) |
| `MEAS:SHOW?` | both as displayed: `0.0000AAC,14.603HZ`. Frequency here is **`HZ`**, 3 decimals; the frequency function itself says `Hz` |
| `MEAS2:SHOW?` | `14.603HZ` |
| `FUNC2?` | `FREQ`, **unquoted** (`FUNC?` quotes); `NONe` when off |
| `RANGE2?` | the AC range in use (`5 V`, `500 mV`, `5 A`); no reply when off |
| `FUNC?`, `RANGE?` | the primary, unchanged |

Stock also returned a stale reading for a moment after leaving dual mode
(`0.9496` in DC current, left over from AC volts). Ours answers "no reading
yet" after a function change instead.

**Calibration dump repeatability:** two `INT:TXT` dumps 30 minutes apart,
with setting commands in between, were byte-identical (SHA-256 `759927…`).

**Speed and timing** (DCV, open input; `MEAS?` polled as fast as stock
answers):

| `RATE` | a new `MEAS?` value every | per second |
| --- | --- | --- |
| `F` | ~300 ms (10th–90th percentile 292–460) | ~3.3 |
| `M` | ~620 ms (587–757) | ~1.6 |
| `S` | ~1050 ms (889–1064) | ~1.0 |

- Stock answers a query in about 23 ms, so about 43 queries/s at most.
- `RATE F/M/S` gets **no reply**: the `OK OK OK` of v2.1.2 is gone on 4.3.0.
- These are stock's averaged readings, not raw conversions: the "65
  readings/s" figure owners quote is the HY3131 datasheet's.

**Other observations:**
- Overload reads `1E+9`, as ours.
- On entering temperature, replies stalled for about 2 s and then arrived
  together: stock's cold-junction refresh (#76 on the forum).

`app_scpi_range_text()` builds these, and `test_app` holds every observed
form.

Two other things seen in the same session:
- `MEAS2?` stayed `NONe` in every mode, including AC and frequency.
- Changing function is not atomic: one poll returned `FUNC? "CURR AC"`
  with `RANGE? 5 V` mid-change. Tools should not rely on two queries being
  consistent across a key press.

**Fixed, not copied:** stock's `FUNC?` table lists DIOD before CONT while its
function codes run continuity (6), diode (7) - consistent with the reported
continuity/diode swap. Ours answers `"CONT"` in continuity and `"DIOD"` in diode.

## Commands

| Command | Notes |
| --- | --- |
| `*IDN?` | `OWON,XDM1241,<serial>,<firmware>` |
| `*RST`, `*CLS`, `SYST:ERR?` | standard; error queue of 8 |
| `MEAS?` / `MEAS1?` / `MEAS2?` | primary / secondary; no reading yet → `9.91E+37` |
| `FUNC2 POWer`, `MEAS:POWer?`, `MEAS:ENERgy?`, `MEAS:CHARge?`, `MEAS:ENERgy:TIME?`, `CALC:ENERgy:CLEar` | ours (B21): power on DC current. `FUNC2?` answers `POW`; `MEAS?` gives `amps,volts`; the queries answer W, Wh, Ah and seconds as `%E` (`9.91E+37` until there's a pair), signed. `-221` outside power mode, and for `FUNC2 POW` where the board can't alternate volts and current or the function isn't DC current |
| `FUNC2 FREQ\|DC\|AC\|NONe`, `FUNC2?`, `RANGE2?`, `MEAS2:SHOW?` | dual display, in stock's format (table below): `FUNC2?` `FREQ` unquoted or `NONe`. Setting `FUNC2` is ours; stock's way of turning it on over serial is unknown. `DC`/`AC` (ours, B23): the other half of the same input; `RANGE2?` then gives the second reading's range, `MEAS:SHOW?` `1.2345VDC,0.1200VAC` |
| `MEAS:SHOW?` | the display as text with its unit, as stock: `-0.0001VDC`, `000.00mVAC` (integer part padded to the range), `0.000 uF`, `0.0000Hz`, `-66.43dBm`, `overload`, `184.58` + GB2312 ℃, `-0.0001MΩ` with **Ω in GB2312** (`A6 B8`, unlike `RANGE?`'s UTF-8). `KΩ` is assumed |
| `CONF:VOLT[:DC]`, `:VOLT:AC`, `:CURR[:DC]`, `:CURR:AC`, `:RES`, `:FRES`, `:CONT`, `:DIOD`, `:CAP`, `:FREQ`, `:PER`, `:TEMP [KITS90\|PT100]` | optional range: SI value (`CONF:VOLT:DC 50` → 50 V, manual), `MIN`, `MAX`, `AUTO`/`DEF`. `FRES` is plain resistance, as stock: the meter has no 4-wire mode |
| `FUNC?`, `FUNC "<name>"` | names as above |
| `RANGE <n>`, `RANGE?` | set: a **1-based range number**, as stock (DCV: `1` = 50 mV … `6` = 1000 V; off the ladder is refused), or `MIN`, `MAX`, `AUTO`. Query: the range as text (table below) |
| `AUTO [ON\|OFF]`, `AUTO?` | stock ignores `AUTO 0`/`OFF`; ours obeys. `AUTO?` is `0` in frequency, period and temperature, as stock |
| `RATE S\|M\|F`, `RATE?` | the AD1 conversion rate (B11): S ~246 ms, M ~123 ms, F ~61 ms a conversion (R22 0x16 / 0x15 / 0x14) for DC V, DC I, ohms and temperature |
| `CALC:FUNC NULL\|AVER\|DB\|DBM`, `CALC:FUNC?`, `CALC:STAT OFF` | `MEAS?` returns dB/dBm in those modes, as stock. `CALC:FUNC?` answers `NULL`, `AVERage`, `DBM`, `DB`, and nothing when math is off, as stock |
| `CALC:NULL:OFFS <v>`, `?` | REL value, SI; the query is `%7.2f` (`   0.00`) as stock |
| `CALC:DB:REF <dBm>`, `CALC:DBM:REF <ohms>` | ohms from the 17 stock impedances. Both queries answer the **impedance** as `%05d` (`00600`): stock answers `DB:REF?` that way and never answers `DBM:REF?` |
| `CALC:AVER:MIN? / MAX? / AVER? / COUN?`, `CALC:CLE` | `%f` (`-0.000109`), `nan` unless statistics are on, as stock. `COUN?` is ours (stock never answers) |
| `SYST:BEEP[:STAT]`, `CONT:THRE <ohms>` | `SYST:BEEP?` answers `NO` for on and `OFF` for off: stock's typo, kept for compatibility |
| `TEMP:RTD:TYP KITS90\|PT100\|PT1000`, `TEMP:RTD:UNIT C\|F\|K` | `PT1000` is ours (also `CONF:TEMP PT1000`; `RANGE?` says `Pt1000`) |
| `SYST:BEEP:FREQ <Hz>`, `SYST:BEEP:VOL <%>`, `SYST:BEEP:STYL CLASSIC\|SOFT\|CHIRP`, `SYST:BEEP:PLAY [CLICK\|DIODE\|HOLD\|TEST\|ALARM]` | ours: beeper pitch (200–8000 Hz, any value), volume (10–100%), style (`CLASSIC`, `SOFT`, `CHIRP`, `DROP`, `DOUBLE`, `TWO-TONE`, `WARBLE`, `CUSTOM`), and play a sound (`-221` with the beeper off). Queries return the setting |
| `SYST:BEEP:ATT <ms>`, `:REL <ms>`, `:GLID <%>`, `:CLIC <ms>`, `:PATT SINGLE\|DOUBLE\|TWO-TONE\|WARBLE` | ours: the envelope and pattern, any value in range (0–50, 0–300, −50–50, 10–80); setting attack, release or glide makes the style `CUSTOM` |
| `TEMP:RTD:LEAD <ohms>\|NULL`, `TEMP:RTD:LEAD?` | ours: PT100 lead resistance (0–5 Ω). `NULL` takes it from the reading now, PT100 selected and the probe's leads shorted at the sensor (under 10 Ω, else `-221`) |
| `TEMP:TC:EMF?`, `TEMP:TC:CJUN?` | ours: the type-K EMF as measured (mV) and the cold junction (°C), `%E`; `9.91E+37` when there is none |
| `CAL:FACT:LIST?` | ours (B32): every factory slot, `DCV 50 mV,1.021573,-14841,OK;…` (gain, zero as stored, OK or BAD) |
| `CAL:USER:ZERO`, `CAL:USER:GAIN <value>`, `CAL:USER:STAT?`, `CAL:USER?`, `CAL:USER:LIST?`, `CAL:USER:CLE [ALL]` | ours: the owner's calibration on the current function and range (manual range), on top of the factory one ([`user-calibration.md`](user-calibration.md)) |
| `SYST:TERM ON\|OFF`, `SYST:TERM?` | ours: terminal mode for typing by hand (also Utility → TERMINAL MODE): Enter (CR) ends a line, typing is echoed, backspace edits, Ctrl-C/U clears. Off by default (stock behaviour: LF ends a line) |
| `SYST:REM`, `SYST:LOC` | remote locks the panel; RUN/STOP returns to local |
| `LOG:FULL STOP\|ROLL`, `LOG:FULL?` | ours: what a recording does when the log is full; ROLL overwrites the oldest readings (set while not recording, else `-221`) |
| `LOG:STAR`, `LOG:STOP`, `LOG:STAT?` | recording (ours: stock's record commands are not recovered). No storage → `-221` |
| `LOG:INT <s>\|MAN`, `LOG:INT?` | nearest offered interval (`0.015` is fast recording, B24); `0` = manual; refused while recording |
| `LOG:TRIG` | one point, manual mode only |
| `LOG:POIN?`, `LOG:CAP?`, `LOG:FUNC?` | points held, capacity (10668), the session's function (`"NONE"`) |
| `LOG:DATA? [start[,count]]` | one line of `%E` values; overload `1E+9` |
| `LOG:TIME?` | ours (B33): when the recording started, `2026-11-15T14:05:20`, or `NONE` without the clock |
| `SYST:DATE y,m,d`, `SYST:TIME h,m,s`, and their queries | ours (B33, SCPI 1999 forms): set or read the real-time clock; each keeps the other half; queries answer `0,0,0` while it isn't set |
| `INT:RECord?` | the log in **stock's download format**, so OWON's DMMEasyControl and owners' scripts read ours: `start:<n>,` then per point `<function>,<value as displayed>,<unit>,` then `end`. Stock (V4.3.0, observed): `start:11,DCV,0.0039,VDC,DCV,1.7859,VDC,…,end`. Ours formats each point on the smallest range that holds it (`-5.100,mVDC` where stock, sitting on 5 V, wrote `-0.0051,VDC`). Only `DCV`/`VDC` is confirmed; other names follow the pattern |

Not implemented: the undocumented service commands. Several write flash on
stock (`DOWNLOAD` sets an update marker and resets; `VERSION <arg>` rewrites
the factory page; `docs/settings-dump.md`). Ours: (`DOWNLOAD`, `HYREG`,
`PROduce`, `KEYRANDOM`, `JPG`, `BMP`, `TXT`, `WTXT`); they return -113.
`PROduce` is plausibly OWON's calibration-rig mode: the image contains the
commands a meter would send to a source (`FUNC DC;:VOLT %f`, `OUTP ON`).
