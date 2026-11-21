# Backlog

Everything we might build beyond stock parity, in one list to prioritise.
Feasibility reasoning for older items is in `wishlist.md`; the sources for
new ones are in `community-research.md` (EEVblog post numbers as `#n`).

**Columns:**

| Column | Values |
| --- | --- |
| Type | **FW** firmware only · **FW+B** firmware, needing bench data · **B** bench work · **HW** a hardware mod · **Host** PC-side tool |
| Value | what an owner gains: H / M / L |
| Effort | S (hours) · M (days) · L (a week or more) |
| Priority | a proposal: P0 now/safety · P1 next · P2 soon · P3 later · — parked |

Hardware mods are first-class items, marked **HW**. Each says what it
changes on the board.

## Done (for reference)

- Utility menu, graph softkeys, backlight 10–100% and dimming.
- 10,668-point power-safe log with SCPI download.
- Standard SCPI with CONT/DIOD fixed.
- Settings persist (speed included, a top complaint: #144, #231, #252).
- mV↔V autoranging in both directions (stock: #57, #146).
- REL alongside stats/dB (stock can't: #57).
- Added with this research:
  - SCPI blocklist covers `START` and `INT:` (#265);
  - the serial dump uses `INT:TXT`;
  - HY3131 bus at stock's 2.6 kHz;
  - backlight PWM moved above hearing (#290);
  - our calibration parser checked against a real XDM2041 record.

## The list

### Bring-up and safety

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B1 | Read the stock firmware version on our meter | B | H | S | **done** | **V4.3.0** (`*IDN?`, 2026-11-08). port-symbols maps from v2.1.2, so expect more unmapped symbols than on the synthetic test |
| B2 | Get UniSoft's control-board schematic (#158) | B | M | S | P2 | EEVblog attachment, needs a forum login. Keypad, relays and the panel pins are settled on the meter; it would still help with `PWR_EN` and the off-state drain (B46, B48) |
| B3 | Capture relay drive alongside the HY3131 writes on day 2 | B | H | S | **superseded 2026-11-11**: stock is gone; relays recovered by emulation (PA8, PA13, `afe-table.md`) | `capture-afe.py` sees only HY3131 traffic. Relays are GPIO or 74HC595 (#147, XDM2041). Needed for `afe_configure` and B21 |
| B4 | Provisional `afe_configure` table from the XDM2041 captures | FW | M | M | P2 | Same HY3131, different board: a starting point to verify, never to trust. Lets the simulator use real register images |
| B5 | Tune HY3131 bus speed up from 2.6 kHz | FW+B | M | S | **done 2026-11-14** | `DIAG:BUS` on our meter: 0 bad read-backs from 192 µs down to 1 µs a half-bit; default now 4 µs (10,000 clean passes; a row write + read-back 161 → 4.4 ms). On this v3 board the bus is not limited by the XDM2041's ~50 µs optocoupler path |
| B6 | Confirm the 25 kHz backlight PWM dims cleanly and silently | B | M | S | **done 2026-11-14** | Dims from the Utility menu, checked by the owner (PA11 confirmed) |

### Firmware updates

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B7 | `scripts/serial-update.py`: install our firmware over OWON's bootloader (area 5) | Host | **H** | M | **works on the meter 2026-11-10** (dumper, then `xdm1241-serial`, 8 packs) | Tested against a simulated bootloader (19 tests); image checks pass OWON's real v2.1.2 file at 12 packs, the count in the captured stream. Not yet run on a meter: waits for B7a. User guide: `install.md`. **Now the only install route for our meter**: it is readout-protected, so SWD can neither back up nor debug it (`readout-protection.md`, 2026-11-10). Protocol known (`community-research.md`). The bootloader is silent at power-on (observed), so the tool sends `start` until it sees `7` while the user switches the meter on. Must make area 2 (calibration) impossible to send, refuse images not linked at 0x08002800, verify `7` acks. Testable now against a simulated bootloader; confirmed on the bench with a stock round-trip first |
| B7a | Get OWON's current update file for this meter (V4.x) | B | M | S | — | **Moot:** OWON won't supply V4.x, and stock is gone from our meter (B7e saved what survived). Still worth having for other owners who want a way back (B9) |
| B7b | Simulated bootloader for `serial-update.py` tests | Host | H | S | **done** | `scripts/test_serial_update.py`. Model the `start`/`7`/area/packs/`0x14` exchange, including refusing area 2 |
| B7c | Debug console on the USART for the serial-installed build | FW | **H** | S | **done** | The `xdm1241-serial` build: logs and a heartbeat on the serial port, `*IDN?`. Only in that build, so SCPI clients never see log lines. RTT needs SWD, which a protected chip refuses. Log lines must not upset SCPI clients: a mode, or a prefix |
| B7d | `INT:FLASH?`-style read-out of the bootloader from our app | FW | H | S | **done** | `DIAG:BOOT?` in `xdm1241-serial` (`lib/console`, host-tested), read by `scripts/read-boot.py` (two passes, CRC). Protection doesn't stop code on the chip reading its own flash. Gives us the bootloader: panel init, and B8. Read-only, bootloader range only |
| B7e | One-pack flash dumper: save what survives of V4.3.0, since OWON won't supply it | FW+Host | **H** | S | **run 2026-11-10** | Installed and read out (`backups/20261110-210942-flash/`). **Stock V4.3.0 did not survive**: the bootloader erases the whole app area for area 5, not just the packs it writes. Kept: the bootloader (10 KB, two reads + CRC), the calibration page (identical to the serial dump) and the stock records. No way back to stock (`install.md`) |
| B8 | Check what the bootloader erases for area 5 | B | H | S | **answered 2026-11-10** | **All of 0x08002800-0x0803CFFF**, 117 pages, before the pack count (`bootloader.md`). Our log (0x08030000) and settings (0x0803B800) are inside it: every update wipes them. Open: accept and document in `features.md`, or relocate (0x0803D000 is the bootloader's state page, not free) |
| B9 | One-step "back to stock" over serial | Host | M | S | P2 | Same path, with the image from your backup. Keeps the no-tools promise both ways |

### Measurement

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B10 | **Faster autoranging** (owner's priority). Stock is slow everywhere (#36, #41, #52, #57); measured on ours: ~1.75 s per range, 7–9 s across the ohms ranges (`app.md`). Today ours decides only at the 200 ms display tick and moves one range per decision: 50 mV → 1000 V takes five decisions, over a second. Split: | | | | **done on the meter 2026-11-14** | |
| B10a | Decide on the first good sample after a range change, not at the display tick | FW | H | S | **done** | Plus: steps shown at once (same range: 247 → 50 ms) |
| B10b | Jump straight to the right range when ranging down, as far as the current reading's resolution justifies | FW | H | S | **done** | |
| B10c | On overload, jump to the top (or +2) instead of +1, then come straight down: at most two hops for any step | FW | H | S | **done** | To the top. Revisit for ohms if the high ranges settle slowly on the bench |
| B10d | Hunt with a fast conversion (low AD1OSR, or AD2/LPF), then measure at the chosen speed | FW+B | H | M | **done 2026-11-14; DC on the meter, ohms to check** | DC V, DC I and ohms hunt at R22 0x10 (~4 ms) instead of 0x16 (~246 ms). DC 0 → 25 V on the meter 1.5 → 0.4–0.6 s. Ohms waits for two fast samples ≥40 ms apart to agree (its source settles over time): simulator 2.2 → 0.54 s worst |
| B10e | Per-range settle times instead of our flat 20 ms | B | H | S | P1 | From the day-2 capture: relays, the divider RC, the ohms source charging the lead capacitance |
| B10f | A ranging-latency test: virtual time from an input step to a correct display, for every function and step size | FW | M | S | **done** | `test/test_ranging`: worst case 1498 → 502 ms (DC 252 ms); no flapping under noise. Simulator now saturates at 1.38× FS |
| B11 | Speed (S/M/F) actually changes AD1OSR | FW+B | M | S | **done on the meter 2026-11-16** | XDM2041 DCV 5 V uses AD1OSR = 4 (4096×); the day-2 RATE steps give the rest. Stock's output rates, measured: S ~1/s, M ~1.6/s, F ~3.3/s. **Done in firmware 2026-11-14:** S/M/F = R22 0x16/0x15/0x14, ~246/123/61 ms a conversion (LOW kept at the old fixed rate so nothing gets slower); `platform_t.afe_rate`. **On the meter 2026-11-16:** 4.0 / 8.0 / 16.2 samples a second (console `samples=`) |
| B12 | Mains-hum rejection: rates chosen for the real HY3131 clock (4 MHz?) so notches land on 50/60 Hz | FW+B | M | M | P2 | Hum rejection is poor at 50 Hz (#177, #180). A 50/60 Hz setting |
| B13 | **HW:** fit a 4.9152 MHz HY3131 crystal (the datasheet value) | HW | M | M | P3 | Kleinstein (#177): about 50 cents, better hum rejection, slightly more AC bandwidth. **Invalidates calibration** (needs B30) and changes counter maths |
| B14 | Fast continuity via the HY3131 comparator | FW+B | M | M | P2 | The chip has a short-test comparator. Beep latency is a common complaint (#152) |
| B15 | Auto-hold (freeze on a stable reading, beep) | FW | M | S | **done 2026-11-11** | RUN/STOP cycles Run → Hold → Auto-hold (owner, 2026-11-12; was a menu item). Fixed on the meter: '-----' flashed when the probes lifted (a ranging restart cleared the held reading). Holds after ~1 s within 0.1% (or 10 counts) on a real measurement; keeps it when the probes come off; a new stable reading replaces it. Display only: graph, stats, recording and SCPI stay live. Stock has only RUN/STOP (#57) |
| B16 | Capacitance: fix autorange above 500 µF; speed it up | FW+B | M | M | **done 2026-11-15** | #57, #100, #118, #168. Ranges 5–6 need the charge-slope source. **On our meter (2026-11-11) the emulated v2.1.2 rows read 4–5x high, by a different factor per range** (`afe-table.md`): characterise our own rows against reference capacitors, or capture a working XDM1241. Capacitance stays simulated in the app until then **2026-11-15:** stock's 5/50 mF procedure recovered from the disassembly (`measurement.md`, ranges 5–6): a timed charge from the 0.18 mA source, reads every 200 / 2000 ticks with the source switched off (R2A, R2C = 0), counts 277 / 2000. To finish: TIMER2's tick period, the phase PA settings, then a bench session with a 1–20 mF capacitor | **2026-11-12:** R24 = 0x01 fixes the main error (ranges 2-4 agree within 3% on 10 uF); R21 sets the scale; a per-cycle offset remains; low ranges often see no cycle. Needs reference parts to ~1% for a two-point fit per range (`afe-table.md`). **2026-11-13:** the "offset" and "no cycle" were a counter gate cut to ~70 ms; with a CTA read-back after each restart every gate is ~1.2 s and ranges 0-4 read within the LCR parts' uncertainty on factory arithmetic. Ranges 0-4 in the app. Left: 5-6 (charge-slope). **2026-11-15: 5 mF and 50 mF on the meter** (1000 µF reads 1.085 mF, 18 mF 17.64 mF): all seven ranges in the app.
| B17 | Capacitance below 500 pF (stock reads 0) | FW+B | M | M | P3 | Longer averaging on the 50 nF range plus an open-lead null. Counter resolution sets the floor |
| B18 | Diode beep threshold setting (stock has one; was fixed at 0.7 V) | FW | L | S | **done 2026-11-11** | Utility menu "DIODE THRESHOLD", 0.1–3.0 V, persisted (settings byte 27). #255 asks for exactly this |
| B19 | 10 A duty timer: warn after 30 s above 7 A | FW | M | S | **done 2026-11-11** (simulator) | Warning line and an alarm every second after 30 s above 7 A on the A jack, then a 30 s rest countdown (`test_ten_amp_duty_warning`). New sound event SND_ALARM (`SYST:BEEP:PLAY ALARM`). Real current path still simulated |
| B20 | AC above the usable band: flag readings above about 4 kHz when frequency is known | FW | L | S | **done 2026-11-11** (simulator) | In dual display (AC + frequency) the line under the reading adds `> 4 kHz: AC READS LOW`. -3 dB at 8.4 kHz, under 1% to 4 kHz (#288); the bench (T-ACV) will measure ours |
| B21 | **V + I at once (and watts)**: AD1 on SENSE, AD3 on OP1O | FW+B | H | M | **done 2026-11-16 on the meter** | No mod by alternating AD1 between the DCV and DCI rows; relay-free when PA8 agrees (`wishlist.md` item 9). DUAL → POWER on DC current: W big, V and I below, Wh / Ah / time integrated (trapezoid), F4 clears; `FUNC2 POW`, `MEAS:POW? / ENER? / CHAR?`, `CALC:ENER:CLE`. On the meter 2026-11-16: 5 mA / 5 V relay-free passed (within 0.002%); 0.4761 W with Wh and Ah = V × I × t in the app. **PA8 moving opens the current path** (scope across the load, 500 mA with 5 V): 50 / 500 mA pair with 50 / 500 mV only (`vi_avail`; acquisition skips any other volts read, `pair_ok`); above 5 mA use the A jack. A jack checked with the scope (no break). Recording logs watts (tag bit 5, `LOG:FUNC?` "POW") |
| B22 | **HW:** route the shunt sense to a free HY3131 PB pin through a resistor | HW | H | S | P3 | Only if B21 shows a relay in the way. Analog side: high-voltage care |
| B22a | **Stock-parity dual display:** ACV + Hz and ACI + Hz, DUAL to swap, Exit to close (manual p. 27) | FW+B | M | M | **done (sim)** | The only pairings stock supports. Alternate RMS and counter, or run them together if the counter sees the AC path. Stock's SCPI in dual mode is captured (`scpi.md`): `MEAS?` gives both, `FUNC2?` `FREQ`, `HZ` in `MEAS:SHOW?` |
| B23 | Dual display, same input (ACV + DCV, AC+DC) | FW+B | M | M | **done 2026-11-16 on the meter** (100 Hz, 311 mV RMS on 0.44 V DC: 310 mV / 438.9 mV) | DUAL → F2 DC/AC: the secondary on the matching range of the other side, following the primary's range; FUNC2 DC/AC over SCPI. Alternates (5:1) rather than running AD1 and RMS at once; relays may click |
| B24 | Fast sampling: record at 15–50 ms like stock's internal recorder | FW+B | M | M | **done 2026-11-16 on the meter** (574 readings in ~10 s; keys responsive) | A 15 ms interval for DC V, DC I and ohms: the AD1 at R22 0x12 (15.4 ms) while recording, every conversion logged from the acquisition path (not the 200 ms tick), acquisition polled between frame bands so redraws don't drop readings; SPEED's rate back on stop. To check on the meter: readings lost at each 2 KB page erase |

### Temperature

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B25 | Cold-junction refresh without the 2 s gap every 10 s | FW | M | S | **done 2026-11-11** (adaptive) | Every 10 s while the CJ moves by 0.2 °C or more, doubling to once a minute while steady: ~12 refreshes in 10 min instead of 60 (`test_cold_junction_refresh_adapts`). A change is caught within a minute. Still open, needs the meter: how short the two 1 s settles can be (stock: 3 s every 600 s, #76) |
| B26 | Show the raw thermocouple mV (and CJ) on screen and over SCPI | FW | L | S | **done 2026-11-11** | Under the reading: `CJ 24.0 °C   EMF 7.799 mV`. SCPI (ours): `TEMP:TC:EMF?`, `TEMP:TC:CJUN?` |
| B27 | PT100 lead-resistance null in ohms, and manual lead-R entry | FW | M | S | **done 2026-11-11** | Utility menu PT100 LEADS (0–5 Ω, 0.05 Ω steps, persisted bytes 39–40); SCPI `TEMP:RTD:LEAD <ohms>\|NULL`. Under the reading: `R 139.31 Ω   LEADS 0.80 Ω`. A panel null key could follow if it proves useful |
| B28 | PT1000 support | FW | M | S | **done 2026-11-11** (simulator) | Third sensor: TEMP/F5 cycle K → PT100 → PT1000; read on the 5 kΩ range (its row is confirmed on the meter), converted as a PT100 at a tenth; lead correction applies. SCPI `TEMP:RTD:TYP PT1000`. Bench: a 1 kΩ / 1.385 kΩ resistor pair (T-TEMP-PT100 style) |
| B29 | CJ correction: user offset, and/or a self-heating model (time since power-on, backlight level) | FW | M | M | **offset done 2026-11-11** | Utility menu CJ OFFSET (±5.0 °C, persisted byte 41), applied at once (the CJ is re-measured). Its value comes from T-TEMP-CJ. The self-heating model waits for bench data |
| B30 | **HW:** external cold-junction sensor at the input jacks | HW | M | M | P3 | The real fix (#65, #73). Needs a free HY3131 input (PB pin) or the I2C header. Analog side |

### Calibration

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B31 | User calibration: guided, against a reference, stored in **our** pages | FW | H | M | **built over SCPI 2026-11-12** (simulator) | `lib/app/ucal.c`: gain/offset per range on top of the factory record (never written), bounded ±2% / ±1% FS, in its own store at 0x0803A800 (two pages from the log, owner's choice; log now 10,668). `CAL:USER:ZERO/GAIN/STAT?/LIST?/CLEAR`; UCAL in the status bar. Open: a panel flow, a `bench-check.py` calibrate mode |
| B32 | Calibration health screen: factory vs user, implausible slots | FW | L | S | **done 2026-11-15** (simulator) | Utility → CALIBRATION, RIGHT: every factory slot (gain %, zero, user correction, BAD); `CAL:FACT:LIST?`; `meas_cal_slot_ok` |

### Recording

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B33 | Timestamps from the RTC (32.768 kHz crystal + CR1220 cell on the board; the RTC kept time with mains off, #156) | FW+B | H | M | **done 2026-11-16 on the meter** (3 s fast after 22.7 h, off overnight: ~35 ppm) | GD32 RTC on LXTAL, started without blocking (`hal.c`); `SYST:DATE`/`SYST:TIME`, Utility → CLOCK; a recording's session number is its start time when the clock is set (`datalog`, header bit 23), `LOG:TIME?`, clock times on the browse cursor. **Bench:** set it, power off overnight, read it back |
| B34 | `INT:RECord?` alias in stock's format | FW | M | S | **done** | Format captured from stock V4.3.0 (`start:n,DCV,1.7859,VDC,…,end`). Other functions' names unconfirmed |
| B35 | Several sessions; rolling (wrap-around) log | FW | M | M | **rolling done 2026-11-11**; several sessions open | Record screen F4 WHEN FULL: STOP/ROLL (persisted byte 42), SCPI `LOG:FULL`. `lib/datalog`: rolling sessions in the same format (wrap flag in the header, absolute page numbers, slot = page mod 23); power cut at every flash operation across wraps tested. Several sessions still to do |
| B36 | Browse a recording on the meter | FW | L | M | **done 2026-11-11** (simulator) | Record screen: ▲/▼ move a cursor along the plot, RANGE switches 1/16-point steps; the top line shows the exact reading, its number and time. ▲/▼ no longer change range there (that would have ended the recording) |

### Display and UI

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B37 | Bar graph under the reading | FW | M | S | **done 2026-11-11** | Utility menu BAR GRAPH (persisted, byte 29; off by default). Zero-centred for DC V/I and REL; warn colour past full scale; not for frequency/temperature. `scenarios/features.sim` shows it. Asked for (#135, #136) |
| B38 | Histogram and big-digit views | FW | M | M | **done 2026-11-12** (big digits moot) | VIEW cycles NUM → GRAPH → HISTO: 32 bins over the graph's trace, mean marked, MEAN/SD/N. A big-digit view is moot: the reading already uses stock's 96 px digits |
| B58 | **Mini graph or histogram under the big reading**, in the space the unit line uses now (owner's wish, 2026-11-15) | FW | M | S | **done 2026-11-15** (simulator; on the meter with the next install) | A trend sparkline or a small histogram drawn between the digits and the soft-key row, so the number stays big and the history is visible at the same time; the unit moves beside the digits (or shrinks). A VIEW option (NUM → NUM+GRAPH → NUM+HISTO → …) or a Utility setting. Reuses the trend buffer (`app.trace`) and B38's bins; needs a layout check at 480×320 with every unit (mV AC, kΩ, µF, °C) and the bar graph (B37) on, which uses the same strip |
| B59 | ~~**AC current: 500 mA and the A jack**~~; the V-lead effect on current | FW+B | H | M | **done** | **Done 2026-11-17:** all six AC current ranges in the app on the emulated rows (R26 0x80) and stock's factory calibration: 500 µA / 5 mA / 50 mA within 0.5%, 500 mA −0.2% against 50 mA at 28 mA, 5 A +0.25% and 10 A +0.5% at 1.77 A (`test-plan.md`). The earlier −2.4% / +5.0% were bad references (volts read with the 50/500 mA path open; V lead on). Open: 500 mA above 6% of scale. **A voltage on the V jack adds to the current reading** (+19% at 9.5 V, AC 500 mA): the V input's clamp above 2.15 V; A jack 1.09 mA/V, 50/500 mA 5–6 mA/V, 500 µA/5 mA none; not a real current. Power mode corrects the A jack on DC (`vi_leak`), checked to 0.01 mA at 5 V |
| B60 | **Fast capacitance autoranging** (owner, 2026-11-19: "critical to usability"; a 1000 µF part took 22 s) | FW+B | **H** | S | **done 2026-11-19, on the meter** (the fast probe still to do) | **The hardware sets the floor:** a counter reading is the 1.07 s gate run on to the end of a charge cycle, and a cycle is ~21 ms per µF on 500 µF (100 µF: a reading every 2.14 s on our meter), ~3.25x that on 50 µF and ~10x more on 5 µF; a part over range gives nothing until a cycle ends. So: a reading goes straight to its range; the first gate after a change is ranged on (not shown); one discard, not two; while climbing, short over-range timeouts (2 / 2 / 3 / 3 / 6 s on ranges 0-4, then the full 4 / 9 s); stepping down onto 5 µF or 50 µF only below 30% of them. On our meter, fresh start from 5 µF: **100 µF 14.5 → ~12 s, 1000 µF 22 → 15.0 s** (5 µF gives up at 3.3 s, 50 µF at 3.0 s, 500 µF at 6 s, 5 mF reads 1.081 mF). The first gate after a change can end on a leftover partial cycle: a reading from it is ranged on only with ≥3 whole cycles, and does not end the short timeout. Tried and dropped: calling over range from CTC before a cycle ends (it does not count that way on the meter). Simulator with the meter's cycle times, old → new, from CAP selected: 30 nF 9.9 → 3.3 s, 3 µF 6.4 → 4.4 s, 20 µF 8.3 → 5.9 s, 100 µF 14.5 → 10.4 s, 1000 µF 19.3 → 14.3 s (meter, old: 22 s), 18 mF 25.6 → 20.6 s. **Next, the real fix:** the 5 mF charge-slope method at fast AD1 conversions could read anything over ~10 µF in 1-2 s whatever the cycle time; needs its front-end setting proven on the bench |
| B39 | Faster boot: measure ours, then trim init | FW+B | M | S | P2 | Stock about 5 s (#36; about 5 s on ours, by eye). Our HAL should drive USART TX high early: stock emits `00 01 00` at power-on: not a glitch, the bootloader sending the factory panel-select bytes +0x7E..+0x80 (`bootloader.md`). The bootloader's own delay stays unless we replace it (we won't) |
| B40 | Soft-key profiles | FW | L | M | P3 | |
| B41 | Better label font (stock fonts 0/3, or a proportional one) | FW | **M** | M | **done in the simulator 2026-11-12** | Owner chose variant E2 from rendered comparisons with the manual: Liberation Sans (Arial metrics), bold labels, stock's sizes (19/19-bold/41 px) and colours, Title Case for the stock theme (draw-time, acronyms kept). ~35 KB flash. **Still to check on the real panel**, and its effect on the redraw time (B56) |
| B57 | Beeper sound: frequency, volume and envelope, per event (click, continuity, diode, alarms) | FW+B | M | S | **done in firmware 2026-11-11; to measure on the bench** | `lib/app/sound.c`: per-event shapes (click 30 ms at 125% pitch, diode 150 ms, hold 120 ms at 112%), styles CLASSIC/SOFT/CHIRP as presets of an owner-set envelope (attack 0–50 ms, release 0–300 ms, glide 0–50%, click length 10–80 ms; CUSTOM when edited), volume as PWM duty (100% = stock's 50%). Utility menu BEEP PITCH/VOLUME/STYLE/ATTACK/RELEASE/GLIDE and CLICK LENGTH, each previewing, persisted (bytes 30–38); SCPI `SYST:BEEP:FREQ/VOL/STYL/ATT/REL/GLID/CLIC/PLAY`; `platform_t.tone` in the HAL (TIMER1 reload and compare); `DIAG:TONE <Hz> <level> <ms>` on the panel build. **Bench** (`docs/test-plan.md` T-BEEP): loudness vs pitch and duty to set the menu's steps and defaults; the envelope runs at main-loop rate, so check SOFT is smooth while the screen redraws | **On the meter 2026-11-12:** volume and SOFT were inaudible: this buzzer's loudness barely changes between 5% and 50% duty. Fixed with a 6 MHz timer tick and a cubic level curve (down to 0.05%); added DROP/DOUBLE/TWO-TONE/WARBLE, which change pitch or rhythm. **Volume confirmed on the meter 2026-11-12: steps feel even.**

### Connectivity

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B42 | **HW:** ESP32 interface board replacing the USB board: Wi-Fi web UI, settings, logs, updates through B7 | HW | H | L | P2 | Prior art: `owon-xdm-remote` (#252, #256). The harness is logic-level UART plus 5 V, and the board swaps back. Ours adds the web UI and updates |
| B43 | **HW:** SWD from the ESP32 too, as a remote recovery probe | HW | M | M | P3 | Makes even a failed update recoverable without opening the case |
| B44 | sigrok `scpi-dmm` profile, upstream | Host | M | S | P3 | |
| B45 | Echo / line-editing option for terminal users | FW | L | S | **done 2026-11-12** | `SYST:TERM ON` / Utility TERMINAL MODE (byte 43): CR ends a line (screen sends CR), echo, backspace, Ctrl-C/U |

### Power (XDM1241)

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B46 | Find the 0.5–1 mA "off" drain | B | **H** | S | **P1** | It flattens the cells in months (#274, #291). If the GD32 or panel stays powered when "off", our firmware can deep-sleep it; if it's the ATtiny or a regulator, it needs B49 |
| B47 | ~~Battery voltage and a gauge~~ | FW | M | S | **done** | 2026-11-15: no mod needed. VBAT reaches **PB0** (ADC0 IN8) through a 1:2 divider, as stock V4.x read it: 2.087 V at the pin with 4.21 V on the cell (owner's meter). Gauge in the top bar (red at 10%), volts and % in Utility, `DIAG:ADC?` for the raw counts. % is from a typical 18650 curve: a guide |
| B48 | Auto power-off | FW+B | M | S | P2 | Needs the PWR_EN handshake (B2 helps) |
| B49 | **HW:** our own ATtiny202 firmware: a short press powers on, hold to switch off | HW+FW | **H** | M | **P2** | Owner wants it. Also the likely route to B46 (off-drain), B47 (battery voltage via its ADC) and B48 (auto power-off). Swap the chip rather than overwrite it, since stock's is probably locked. Plan: `power-controller.md` |
| B50 | **HW:** cell maintenance: matched cells, contacts, no glue | HW | L | S | — | Reported poor contact and mismatched cells (#235, #266). A note, not work |

### Analog quality (all hardware)

| ID | Item | Type | Value | Effort | Pri | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| B51 | **HW:** better voltage reference in the optional U6 footprint (#281) | HW | M | M | — | Needs recalibration (B31). Only worth it with a reference to calibrate against |
| B52 | **HW:** vents or thermal changes to cut self-heating | HW | L | S | — | Helps thermocouple accuracy. B29 does most of this in firmware |
| B54 | Key click: beep on every key press, as stock does | FW | H | S | **done 2026-11-11** | Seen missing on the meter 2026-11-11 (`xdm1241-ui`): the app beeps only for continuity and diode. Parity item |
| B55 | Five softkeys, not six: the app still assumes an F6 | FW | H | S | **done 2026-11-11** | Our panel has Screen1-5 only (`hardware.md`). F6 cycles the theme on the measure screen; everything else has another key. Move the theme, then drop F6 from the softkey rail |
| B56 | Render speed: skip the redraw when nothing on screen changed | FW | M | M | **done in the simulator 2026-11-11** | Was 84 ms per full compose of 16 bands every 200 ms on the meter, even with zero bands sent. Now a ~1 KB fingerprint of what `app_draw` reads skips the compose when unchanged (`docs/app.md`). **On the meter:** check the heartbeat (`frames=`, `skipped=`) with a steady input. A changing reading still composes every frame: next would be composing only the bands the changed fields touch |
| B53 | Low-voltage ohms for in-circuit work | — | L | — | — | The test voltage (up to 2.17 V, #293) is set by the HY3131 network; not a firmware option |

## Proposed order

1. **P0, no hardware needed:** B1, B2.
2. **Offline now:** B7 (serial
   updater, tested against a fake bootloader), B18, B25, B26, B27.
3. **P1, bench:** B3 and B10e (one capture session), B6, B8, B46, then
   B10d, B11, B33.
4. **P2:** in value order, B21 → B42 → B31 → B24 → B14 → the rest.
5. **HW mods:**
   - **B42 (ESP32 board)** is the one with the most payoff and no risk to
     the measurement side.
   - **B22, B30 and B13** touch the analog board: do them only with a reason
     from the bench.
