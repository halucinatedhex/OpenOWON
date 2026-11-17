/* TARGET ONLY. The GD32F303 hardware layer: the one place that touches
 * registers. It provides a platform_t for lib/app (docs/boot.md).
 *
 * Boot facts this relies on (docs/boot.md):
 *   - the app lives at 0x08002800 behind OWON's bootloader, which is kept
 *   - stock points VTOR at the bootloader's table (0x08000000); we point it at
 *     our own, so our interrupts do not depend on the bootloader forwarding
 *   - the clock comes from the internal 8 MHz oscillator (72 MHz PLL), never
 *     the crystal, which may not be fitted on v3.x boards
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

/* VTOR, SysTick at 1 kHz, RTT console. Call first in main(). */
void hal_early_init(void);

/* Configure the subsystems whose pins are enabled (hal_pins.h). */
void hal_board_init(void);

uint32_t hal_millis(void);
bool     hal_clock_ok(void);       /* false: PLL did not lock, running at 8 MHz */
bool     hal_lcd_enabled(void);
bool     hal_afe_enabled(void);

/* Calibration from the factory record in flash (read-only). Returns the
 * meas_cal_validate() result: 0 means the record looked right and is in use;
 * otherwise unity calibration is in use. */
int      hal_load_calibration(void);

const platform_t *hal_platform(void);

/* Reset the chip: OWON's bootloader runs again, as at power-on, so a host
 * calling it with `start` gets in without anyone touching the meter. */
void hal_reset(void);

/* Keypad (HAL_KEYPAD_ENABLED, docs/hardware.md): init returns the lines
 * found low with nothing driven (0 = safe to scan; -1 = keypad not enabled
 * in this build); scan returns
 * A lines in bits 0-4 and B lines in bits 8-12, 1 = pressed. */
int      hal_keypad_init(void);
uint32_t hal_keypad_scan(void);

/* Beeper (HAL_BEEPER_ENABLED): PB11, ~3 kHz PWM. */
void hal_beeper_init(void);
void hal_beep(bool on);
void hal_tone(uint16_t hz, uint8_t level);   /* B57: pitch, drive 0-100 (0 = off) */
void hal_backlight(uint8_t pct);             /* PA11 PWM duty, 0-100 (HAL_BACKLIGHT_ENABLED) */

/* Simulated front end (HAL_SIM_AFE, the xdm1241-ui build): lib/sim's HY3131
 * model stands in for the chip. init after hal_load_calibration; tick with
 * hal_millis() from the main loop. */
bool hal_afe_simulated(void);
bool hal_afe_dcv_real(void);    /* DC V on the real HY3131 (HAL_AFE_DCV_REAL, pin check passed) */
void hal_sim_init(void);
void hal_sim_tick(uint32_t now_ms);

/* Front-end probe (HAL_AFE_PROBE, xdm1241-panel): read-only bring-up.
 * probe_pins reads CS, CK, MISO, MOSI, PA8 with pull-up then pull-down,
 * nothing driven: 'U' = follows the pulls, 'H'/'L' = held. Returns nonzero
 * if the roles look right (inputs undriven, MISO driven). probe_bus then
 * sets the bus up for hy3131_read8(). */
int  hal_afe_probe_pins(char out[5]);
void hal_afe_probe_bus(void);
void hal_afe_relays(bool pa8, bool pa13);   /* docs/afe-table.md */
void hal_afe_relays_release(void);          /* both off */
/* The HY3131 bus speed: half a bit period, us (HAL_AFE_HALF_BIT_US at boot). */
void     hal_afe_set_half_bit_us(uint32_t us);
uint32_t hal_afe_half_bit_us(void);

/* Panel only (the xdm1241-panel build): select it, switch it on, landscape. */
void hal_panel_init(void);

/* Log console on the serial port (HAL_SERIAL_CONSOLE builds; no-ops
 * otherwise). For readout-protected meters, where RTT can't be used. */
void   hal_console_init(void);
void   hal_console_write(const char *s, size_t n);
size_t hal_console_read(char *buf, size_t max);

#endif
