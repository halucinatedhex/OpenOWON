/* HOST ONLY. The simulated meter: a platform_t wired to the HY3131 model,
 * the panel model, a virtual millisecond clock, a key queue and a beeper.
 *
 *   sim_board_init(NULL);              // unity calibration
 *   app_init(&app, sim_board_platform());
 *   sim_hy3131_world()->dcv = 1.2345;
 *   sim_board_run(&app, 1000);         // one virtual second
 *   sim_lcd_save_bmp("build/sim.bmp");
 */
#ifndef SIM_BOARD_H
#define SIM_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "platform.h"
#include "sim_flash.h"
#include "sim_hy3131.h"
#include "sim_lcd.h"

#define SIM_BAND_ROWS 20     /* 480 x 20 x 2 = 19.2 KB, what the target can afford */

/* cal may be NULL for unity calibration. */
void sim_board_init(const meas_cal_t *cal);
const platform_t *sim_board_platform(void);
/* Power cycle: clock, chip and panel reset; the settings flash is kept. */
void sim_board_reboot(void);

void     sim_board_press(app_key_t k);
void     sim_board_run(app_t *a, uint32_t ms);   /* app_step + chip tick per ms */
uint32_t sim_board_millis(void);
bool     sim_board_beeping(void);
unsigned sim_board_beeps(void);     /* times the beeper has switched on */
uint16_t sim_board_tone_hz(void);   /* the beeper's last pitch and level (0-100) */
/* Limit the ranges the board offers, as the meter's HAL does (NULL: all). */
void     sim_board_set_range_avail(bool (*f)(meas_fn_t fn, int range));
void     sim_board_set_fn_avail(bool (*f)(meas_fn_t fn));
/* B21: which V/I range pairs may alternate (NULL: all); off: none (as the meter today). */
void     sim_board_set_vi_avail(bool (*f)(int irange, int vrange), bool off);
void     sim_board_clock_reset(void);          /* B33: the clock back to "not set" */
uint8_t  sim_board_tone_level(void);
uint8_t  sim_board_backlight(void);
uint32_t sim_board_baud(void);              /* last rate the app set, 0 if none */
void     sim_board_set_battery_mv(int mv);  /* -1: no battery sense (stock board) */

/* Serial port: send a line to the meter (a newline is appended), and take the
 * meter's replies one line at a time. take returns false when none is queued. */
void     sim_board_serial_send(const char *line);
bool     sim_board_serial_take(char *line, size_t cap);

#endif
