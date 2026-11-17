# Boot, flash map and the HAL

| Range | Contents |
| --- | --- |
| 0x08000000-0x080027FF | OWON bootloader - **kept**; it initialises the panel |
| 0x08002800-0x0802FFFF | our application (`linker/app.ld`, 182 KB; about 75 KB used) |
| 0x08030000-0x0803A7FF | our data log (`lib/datalog`, 21 pages, 10,668 readings) |
| 0x0803A800-0x0803B7FF | user calibration (`lib/app/ucal`, two store pages, B31) |
| 0x0803B800-0x0803D7FF | our settings (`lib/store`, first two pages) |
| 0x0803D800-0x0803FFFF | stock log / prefs / factory calibration - never erased |

- **Clock:** `SystemInit` is built with `__SYSTEM_CLOCK_IRC8M` (8 MHz internal);
  `hal_early_init()` sets flash wait states to 2 (as stock does) and then
  brings up a 72 MHz PLL from IRC8M, with bounded waits. The framework
  default (120 MHz from the crystal) spins forever if the crystal is absent,
  which a v3.x teardown reports. The crystal is never used.
- **VTOR:** stock sets VTOR = 0x08000000, relying on the bootloader to forward
  interrupts (`stock_SystemInit` 0x080029FA). We point VTOR at our own table.
- **Pins:** the `xdm1241` build drives none (RTT log + heartbeat only).
  `xdm1241-serial` (`HAL_SERIAL_CONSOLE`) adds the serial port only: log,
  heartbeat, `*IDN?`, `DIAG:BOOT?` (`install.md`). It is the first image for
  a readout-protected meter.
  `xdm1241-bench` (`HAL_TRUST_V2_PINS`) drives the v2-derived map and runs the
  app. Confirm each subsystem on the v3 board, then set its flag in
  `lib/hal/hal_pins.h`.
- **Console:** SEGGER RTT (`lib/hal/rtt.c`), no pins: `JLinkRTTClient`.
  **Not available on a readout-protected meter** (ours): a debugger there
  can't even let the core fetch code. The serial-installed build needs its
  console on the USART instead (backlog B7c).
- **Calibration:** read (never written) from 0x0803F800 at boot; used only if
  `meas_cal_validate()` returns 0.
- **Flashing:** on a protected meter (ours), only over serial through OWON's
  bootloader (backlog B7). On an unprotected one, `scripts/flash.sh` writes at 0x08002800, checks the image's
  vector table was linked there, and refuses to reach 0x0803D800.
  `scripts/restore.sh` writes stock bootloader+app back from a backup.
- **Settings flash:** 0x0803B800-0x0803C7FF via `lib/store`, written only when
  built with `HAL_STORE_ENABLED=1` - set it after the v3 dump shows those
  pages unused. Not implied by `HAL_TRUST_V2_PINS`.
- **Data-log flash:** 0x08030000-0x0803A7FF via `lib/datalog`, written only
  with `HAL_LOG_ENABLED=1`, on the same condition. `scripts/check-regions.py`
  (bench day 1, step 7) checks both regions in the dump. The linker stops our
  image at 0x08030000, so flashing an update never erases a recording.
- **Stubbed until the bench:** `afe_configure` (per-range table), key matrix,
  PWR_EN.
