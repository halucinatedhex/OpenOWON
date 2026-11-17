# External resources and other approaches

Things that short-cut work this project was otherwise going to do the hard way.

## Prior art that matters

### Project Phoenix — alternative firmware for a sibling OWON

[aziemer/Project-Phoenix](https://github.com/aziemer/Project-Phoenix) is
replacement firmware for the **Voltcraft VC-7055BT**, a rebrand of the
**OWON XDM-2041** (also sold as PeakTech P 4094). Same family as ours, and
critically the same MCU class — **STM32F103CBTx**.

It already implements, in readable source:

- **TFT display driver with init routines** — the thing static analysis of
  our image has so far failed to find, and the stated blocker for all three
  planned UI features
- **Keypad scanning**
- Voltage, current, resistance, diode, continuity, frequency
- A USB bootloader

Explicitly *not* implemented there: calibration, menus, SCPI, capacitance,
temperature. Everything it displays is uncalibrated.

**Caveat:** different model, and probably a different panel and pin map. Treat
it as a reference implementation and a source of known-good init sequences,
not as a drop-in.

### Digilent DMM Shield — a complete HY3131 driver, open source

[Digilent/Arduino-DMMShield-Library](https://github.com/Digilent/Arduino-DMMShield-Library)
drives the same HY3131. `dmm.cpp` contains **the per-scale register table for
all 27 scales** — exactly the data this project assumed would need a
logic-analyser capture:

```c
{DmmDCVoltage, 5e0, 2, {0x00,0x60,0x00,0x17,0x8B,0x01,0x11,0x00,0x55,0x31,
                        0x00,0x22,0x00,0x00,0x90,0x28,0xA0,0x80,0xC7,0x33,
                        0x20,0x00,0x00,0x00}, 125e-1/1.8/8388608}, // "5 V DC"
```

24 bytes covering registers 0x20–0x37, plus the raw-to-value scale factor
(8388608 = 2²³, the ADC's full scale). `calib.cpp` (51 KB) implements the
calibration maths, and `spi.cpp` the transport.

**Caveat, and it is a real one:** Digilent's external resistor network and
shunts are not OWON's. The PA-network bits and the scale factors are
board-specific and **will not transfer verbatim**. What does transfer is the
register layout, the ordering, which bits matter per mode, and the shape of
the scaling maths. It turns the logic-analyser session from "derive
everything" into "confirm and adjust", which is a large saving.

### HYCON application notes

`APD-DMM001` (6000-count DMM application manual) and `APN-HY3131001` (SPI
communication notes) exist on hycontek.com. The download timed out here;
worth retrying.

## Button input — how it works

`stock_keypad_scan` at **0x08004E36**. It is a **two-row scanned matrix**, not
one pin per button:

1. `stock_keypad_row_select(1)` at 0x08004C94 drives the first row.
2. Read the column pins and pack them into a bitmask, one bit per column.
3. `stock_keypad_row_select(0)` drives the second row.
4. Read again, packing into the upper bits.
5. Return a single word, roughly 13+ bits, one per key.

Columns are spread across ports — GPIOB 3, 4, 5, 6, 7, GPIOA 15, and GPIOC —
and each is read with `GPIO_ReadInputDataBit` then shifted into place.

So the **mechanism** is understood. The **bit-to-key mapping** is not, and is
not worth deriving statically: with a debugger attached, breakpoint the
function, press each key, and read the return value. Five minutes of work
against hours of static analysis.

## Approaches not yet used

Ranked by expected value for this project.

| Approach | What it would crack | Notes |
| --- | --- | --- |
| **Ghidra decompiler** | the display driver, the AFE config functions | Installed (12.2) but never actually run. Decompiled C beats reading objdump for control-flow-heavy code. The single biggest unused lever here. |
| **Emulation (Unicorn / Renode)** | the AFE mode tables, without hardware | Load the image, call each config function, trap the GPIO writes, and recover the register sequences exactly as the stock firmware issues them. This is a software logic analyser and needs no meter. |
| **Logic analyser** | mode tables, display protocol, the `PWR_EN` handshake | Still the ground truth, and the only way to see the v3.x board. |
| **SWD live debugging** | keypad mapping, settings layout, RAM structures | Breakpoint and inspect. Trivially answers questions that resist static analysis. |
| **Differential dumps** | settings and calibration field layout | Dump, change one menu item, dump again, diff. Precise and fast. |
| **The OWON updater** | the update protocol, checksums, flash layout | The `x00`–`x14` chunks in TheHWcave's repo reassemble into a ZIP holding the Windows loader. Not yet examined. |
| **Other firmware versions** | structure, by diffing | Other XDM models and revisions share code. |

### Why emulation is underrated here

The AFE configuration values are inline immediates spread over ~15 functions
(see `analog-frontend.md`). Reversing them by hand is tedious and error-prone.
Emulating them is mechanical: stub the GPIO primitives, call
`stock_hy_write_regs` through an emulator, and log `(register, value)` pairs.
The firmware itself tells you what it writes, for every mode, with no
hardware and no guessing.
