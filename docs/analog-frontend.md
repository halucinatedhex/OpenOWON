# The analog front end

## What the chip is

**HYCON HY3131** — a 50,000-count DMM analog front end. Datasheet saved at
`docs/datasheets/DS-HY3131_EN.pdf`. This is the whole measurement system on a
chip, which is good news: you are not reimplementing a discrete front end, you
are driving a documented part.

| Block | Registers | Notes |
| --- | --- | --- |
| AD1 | 0x00–0x02 | 24-bit sigma-delta, **the main reading**, two's complement |
| AD2 / AD3 | 0x03–0x05 | 19-bit high speed |
| LPF | 0x06–0x08 | low-pass filtered AD2 |
| RMS | 0x09–0x0D | 40-bit true-RMS accumulator, computed in hardware |
| Peak hold | 0x0E–0x13 | min and max |
| Counters | 0x15–0x1D | frequency and period |
| Config | 0x20–0x36 | mode, ranges, gains, references |

**True RMS, peak hold and frequency counting are done in the chip**, not in
firmware. That removes most of the hard DSP from a replacement firmware.

## How the meter's controls map to registers

This is the part that matters for reimplementation — the UI maps almost
one-to-one onto a handful of registers.

| UI control | Register | Field |
| --- | --- | --- |
| **Function** (V/A/Ω/C/diode/cont) | 0x2F | `SMODE<6:0>` |
| **Range** | 0x2A–0x2E | PA switch network, 10 taps |
| Range scaling | 0x28 | `SAD1RH<2:0>` / `SAD1RL<2:0>` reference select |
| Input gain | 0x25 | `AD1IG<1:0>` |
| **Speed** (Low/Med/High) | 0x22 | `AD1OSR<2:0>` oversampling |
| ADC on | 0x23 | `ENAD1` |
| True RMS / filter | 0x29 | `ENRMS`, `ENLPF`, `LPFBW<2:0>` |

The **PA network** is a resistor ladder — PA<9>=10M, <8>=1M, <7>=100k,
<6>=10k, <5>=1k — with four switches per tap (PS/DS/FS/SS), packed two taps
per register from 0x2A to 0x2E. That ladder *is* the range switch.

## The bus

4-wire SPI (CS, CK, DI, DO) plus IRQ. Protocol, from §7 of the datasheet:

- First byte is `(address << 1) | rw`, **MSB first**, `rw = 1` for read.
- On a **read**, the ninth bit must be idle before data appears.
- On a **write**, data follows immediately.
- **The address auto-increments while CS stays asserted**, so a 24-bit ADC
  read is one burst, not three transactions.

Two board-level facts change how you drive it:

1. **It crosses an isolation barrier.** U5 on the analog board is an 8-pin
   isolator with a milled slot in the PCB running underneath it — a creepage
   barrier. The analog side floats relative to the digital side.
2. **The stock firmware bit-bangs it**, on GPIOB. There are no SPI peripheral
   references (0x40013000 / 0x40003800) anywhere in the image.

### Confirmed on our v3 meter (2026-11-11), read-only

The `xdm1241-panel` build's probe (`HAL_AFE_PROBE`):

- **Pins, passively** (pull-up then pull-down, nothing driven): CS, CK
  and MOSI follow the pulls (isolator inputs); **MISO is held high**
  (isolator output); **PA8 is held low**, so it's driven too, consistent
  with an IRQ line.
- **The chip answers** (`DIAG:AFE?`, reads only), twice, identically, at
  stock's ~2.6 kHz (since 2026-11-14 the bus runs at a 4 µs half-bit:
  `DIAG:BUS` read-backs are clean down to 1 µs, backlog B5):

  ```
  00: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff
  10: 03 00 00 fc 00 00 00 00 00 00 00 00 01 00 00 00
  20: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  30: 00 00 00 20 00 00 00
  ```

  All configuration registers are at power-on reset (nothing has set them
  up since stock was erased), except 0x33 = 0x20. **PKHMIN = 0x03FFFF and
  PKHMAX = 0xFC0000** are a peak detector's reset values (+/- full scale of
  a 19-bit signed value): a floating bus gives 00 or FF, and a wrong bit
  order or a missing idle bit would scramble them. So the protocol in
  `lib/afe` (address byte, idle ninth bit, MSB first) is confirmed too.

### Pin mapping — confirmed

Read directly out of the bit-bang routine at **0x080077C8**, and consistent
with LQFP48 pins 25–29 going to the analog-board connector and with the
`NSS / CLK / SDO / MOSI` silkscreen beside it:

| Pin | Port | Signal | How it was confirmed |
| --- | --- | --- | --- |
| 25 | **PB12** | CS (NSS) | written as a level, 0x1000 |
| 26 | **PB13** | CK | pulsed low at the top of each bit, 0x2000 |
| 27 | **PB14** | DO (SDO) | read via `GPIO_ReadInputDataBit`, 0x4000 |
| 28 | **PB15** | DI (MOSI) | set/cleared from the byte's MSB, 0x8000 |
| 29 | PA8 | IRQ? | the fifth connector pin; **unconfirmed** |

The shift loop is plain MSB-first:

```
ResetBits(GPIOB, PB13)          ; clock low
lsls r0, byte, #24              ; test bit 7
MSB ? SetBits(PB15) : ResetBits(PB15)
SetBits(GPIOB, PB13)            ; clock high, data latched
```

PB12–PB15 are also the SPI2 block, so a replacement firmware may use the
hardware peripheral rather than bit-banging — subject to what the isolator
passes.

Relevant stock routines, now in `symbols/`:

| Address | Role |
| --- | --- |
| 0x080077C8 | write a byte, MSB first |
| 0x0800780A | **read** a byte: clock PB13, sample PB14, MSB first |
| 0x080077B2 | set MOSI (PB15) |
| 0x080077BE | read MISO (PB14) |
| 0x0800778C | CS control (PB12) |
| 0x0800777E | clock low (PB13) |

Isolators limit clock rate. Measure the stock firmware's actual clock before
assuming a hardware SPI peripheral can run faster.

## Implementing it in replacement firmware

`lib/afe/hy3131.h` has the register map and a transport interface. The work
splits cleanly:

1. **Transport** — fill in `hy3131_io_t` (cs/clk/mosi/miso/delay). Bit-bang
   first for correctness; move to hardware SPI later if the isolator allows.
2. **Bring-up** — enable bias and references (`ENBIAS`, `ENVS`, `ENREFO` in
   0x31), enable AD1 (0x23), then poll `AD1F` in `INTF` (0x1E) and read
   0x00–0x02. A plausible DC reading proves the whole chain.
3. **Modes** — one table mapping function + range to the register writes:
   `SMODE`, the PA switches, `SAD1RH/RL`, `AD1IG`. **Do not invent this
   table.** Capture the stock firmware's writes with a logic analyser while
   turning the range knob, and transcribe what it actually does.
4. **Scaling** — raw counts to volts needs the calibration constants from
   flash: the `coef` / `zero` tables in the factory record at 0x0803F800.
   Layout and the DCV formula are in `calibration.md`.

### Why capture rather than derive

The register map tells you *what* each bit does, but not which combination
OWON chose for "5 V DC" on this particular board — that depends on the
external resistor network, the shunts, and their calibration. Capturing the
real sequences is both faster and more accurate than deriving them.

Concretely: put a logic analyser on the isolator's digital side, cycle every
function and range, and decode. That one capture session yields the whole
mode table, and it is the single highest-value thing to do once the case is
open again.

## Board components — identified

Cross-referenced with [pallavaggarwal.in's XDM1041 teardown](https://pallavaggarwal.in/2023/12/30/owon-xdm1041-multimeter-teardown/),
which covers a v3.x (GD32) unit like ours.

| Marking seen | Part | Role |
| --- | --- | --- |
| `P204 HY3131 RMTSM.A` | HYCON HY3131 | the AFE (teardown says HY3132 — see below) |
| `ET D207 333Y` (U5) | **Everlight ELD207** | dual-channel optocoupler |
| `NS182 41WOD X250A7` | **NOVOSENSE NSI8141** | 4-channel digital isolator |
| `B3GA4.5Z 2411B8 2 C` ×2 | **Fujitsu FTR-B3GA4.5Z** | 4.5 V DPDT relays, input/range switching |
| `T2020-N` / `T202-N` (U6) | **Microchip ATtiny202** | second MCU — *not* an EEPROM |
| — | AIPULNION FN1-05S05 | isolated DC-DC, powers the analog side |
| — | YYS37HV-133-A1 | TFT module over SPI |
| — | SIPEX SP202EEN | RS232 level shifter, on the separate interface board |

The isolation budget now makes sense: **NSI8141 gives 4 channels and ELD207
gives 2**, for six isolated signals. Four of them carry the SPI bus.

### The 8-pin part is an ATtiny202, not an EEPROM

This supersedes the EEPROM discussion in `settings-storage.md` and the earlier
note in this file. `T202-N` is an **ATtiny202** — an 8-pin tinyAVR with 2 KB
flash. There is no serial EEPROM on the board, which is why no I2C EEPROM
driver could be found: there is nothing to drive.

What it does is **not yet established**. Two plausible roles, both consistent
with what is on the board:

1. **Soft power control — the strong candidate.** The owner reports U6 sits
   **next to the power circuitry with power-button routing running to it**.
   That fits the harness signals `VBAT`, `VDC-5V` and **`PWR_EN`**, the
   battery option, and the textbook use of a microamp-class MCU: stay alive
   while the main rail is down, latch power on a button press, drop it on a
   long press, and watch the battery.
2. **Button / encoder handling — weaker.** The v2.x firmware already scans
   the keypad itself, reading GPIOB bits 5–7 at 0x08004E36. Possible that
   v3.x moved it, but there is no evidence for it.

**Implication for replacement firmware, and it is a real trap.** If the
ATtiny202 holds `PWR_EN` only while the main MCU keeps up some handshake —
a heartbeat on the I2C link, or a GPIO kept asserted — then firmware that
does not reproduce it will appear to boot and then switch itself off after a
timeout. Before writing custom firmware, watch `PWR_EN` and the `scl`/`sda`
header with a logic analyser from cold boot through a normal power-down, and
reproduce whatever keeps the meter alive.

The teardown speculates RS232 protocol handling, but RS232 level conversion is
on a separate board with its own SP202EEN, so that seems unlikely.

The `scl / sda / gnd / 3v` header fits an I2C link between the GD32 and the
ATtiny202, with the header as a debug tap. Note the ATtiny202 is programmed
over **UPDI**, a single wire, so that header is probably for the I2C link
rather than for programming it.

**None of this can be resolved from the v2.1.2 image**, which targets the v2.x
STM32F103 board and contains no code for any of it. It needs a v3.x dump.

### Two discrepancies with the teardown

1. **HY3131 vs HY3132.** You read `HY3131` off your own board; the teardown
   says HY3132. Our datasheet (`docs/datasheets/`) covers HY3130/HY3131.
   Trust your board's marking, but re-check the register map against the chip
   before relying on it.
2. **"Isolated UART" vs SPI — resolved as SPI.** The teardown describes the
   AFE link as an isolated UART. It is not, at least not in this firmware.
   Three independent findings settle it:

   - **The bus is bidirectional.** A byte-*read* loop exists at 0x0800780A:
     eight iterations clocking PB13 and sampling PB14, MSB first. A display
     is write-only, so a bus with a MISO read path is talking to something
     that returns data — the ADC.
   - **Only USART1 is used.** The five consecutive USART base addresses at
     0x08004568 are an SPL lookup table, not evidence of use; no code loads
     USART2, USART3 or UART4. USART1 is the host SCPI port, reachable at the
     `TX△`/`RX△` test points.
   - The HY3131 datasheet specifies SPI and no UART, and the NSI8141 supplies
     exactly the four isolated channels a 4-wire bus needs.

   Still worth a logic-analyser check before trusting it on hardware, but the
   SPI driver is written against solid evidence rather than an assumption.

### Display

Also worth noting: **no SPI peripheral is used anywhere in the firmware**
(0x40013000, 0x40003800, 0x40003C00 all absent). The TFT is bit-banged too,
which for 480×320×16bpp means the stock firmware must be doing partial
updates rather than full-frame redraws. Relevant to the graph screen — a
replacement using hardware SPI could be much faster.

## The stock access layer

Mapped in full. Registers are addressed by a **logical index** that goes
through a table at **0x0801DD5C**. It is *not* an identity map; the first
twelve entries skip to the start of each multi-byte data register:

| Index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12–35 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Reg | 0x00 | 0x03 | 0x06 | 0x09 | 0x0E | 0x11 | 0x14 | 0x15 | 0x18 | 0x1B | 0x1E | 0x1F | 0x20–0x37 |
| Name | AD1 | AD2 | LPF | RMS | PKHMIN | PKHMAX | CTSTA | CTC | CTB | CTA | INTF | INTE | R20–R37 |

So `stock_hy_read_regs(3, buf, 5)` reads RMS_DATA, and `stock_hy_data_ready(n)`
returns INTF bit AD1F (n = 0), AD2F (1) or CTF (2). See `measurement.md`.

| Address | Signature | Role |
| --- | --- | --- |
| 0x0800784A | `(idx, buf, n)` | write n bytes from idx, one CS frame |
| 0x0800787C | `(idx, value)` | write a single register |
| 0x08007888 | `(idx, buf, n)` | read n bytes from idx |
| 0x080077C8 | `(byte)` | shift a byte out, MSB first |
| 0x0800780A | `-> byte` | shift a byte in, MSB first |

The address byte is formed as `(reg << 25) >> 24`, i.e. `(reg & 0x7F) << 1`
with the rw bit left clear for writes. That independently confirms the
encoding in `lib/afe/hy3131.c`.

### The mode and range values are inline, not a table

Worth knowing before going looking for one. The configuration routines build
their register writes **on the stack from immediates**, for example at
0x08007B3A:

```
sp[0] = 0x00                     write_regs(12, sp, 1)   ; R20 = 0x00
sp[] = { 0x00, 0x00, 0xF0 }      write_regs(9,  sp, 3)   ; CTA = 00 00 F0
sp[0] = 0xF2                     write_regs(12, sp, 1)   ; R20 = 0xF2
```

So there is no single mode/range table to dump. The sequences are spread over
roughly fifteen small functions with hardcoded values. Recovering them all
statically is possible but is real work, and it would still only give the
**v2.x** board's values.

**This does not change the advice:** capture the stock firmware's writes with
a logic analyser while cycling function and range. One session yields the
whole table, for the right hardware revision, in less time than reversing
fifteen functions for the wrong one.
