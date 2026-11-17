/* What the application needs from a board. Everything the app touches goes
 * through this struct, so the same app runs on the meter (hal/, not yet
 * written) and on the host against the simulator (lib/sim/).
 *
 * Nothing in lib/app touches a register.
 */
#ifndef APP_PLATFORM_H
#define APP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy3131.h"
#include "lcd.h"
#include "meas.h"
#include "store.h"
#include "ui.h"

/* Logical keys, from the front-panel description in the user manual
 * (section 2, figure 2-1). The physical key matrix bit for each one is a
 * bench item (CLAUDE.md, keypad map); the HAL translates. */
typedef enum {
    KEY_NONE = 0,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,   /* right-hand softkeys */
    KEY_V,          /* DC/AC voltage            */
    KEY_I,          /* DC/AC current            */
    KEY_OHM,        /* resistance / continuity / diode */
    KEY_CAP,
    KEY_FREQ,       /* frequency / period       */
    KEY_TEMP,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,          /* range / direction */
    KEY_RUNSTOP,    /* hold                     */
    KEY_RECORD,
    KEY_UTILITY,
    KEY_MATH,
    KEY_DUAL,
    KEY_COUNT
} app_key_t;

/* Front-end configurations beyond (function, range). The cold-junction
 * sensor is read as DCV 500 mV under two bias settings (stock 0x08008DD4,
 * state +0x448 = 1 and 2). AFE_AUX_FAST is the same function and range
 * converting ~12x faster at lower resolution (R22 0x10 for 0x16), used
 * while autoranging hunts for the range (backlog B10d). */
/* Capacitance ranges 5-6 (charge slope): AFE_AUX_NONE is stock's phase A,
 * which rests the capacitor near a fixed level; AFE_AUX_CAP_CHARGE is its
 * phase B, a constant charging current (docs/measurement.md). */
typedef enum { AFE_AUX_NONE = 0, AFE_AUX_CJ1, AFE_AUX_CJ2, AFE_AUX_FAST, AFE_AUX_CAP_CHARGE } afe_aux_t;

typedef struct {
    /* HY3131 bus, driven by lib/afe. */
    const hy3131_io_t *afe_bus;

    /* Put the analog front end into a function and range: HY3131 mode
     * registers plus the board's relays. On hardware this is the per-range
     * table captured from stock on the bench (not yet known). For temperature,
     * range is the sensor: 0 type K, 1 PT100. */
    void (*afe_configure)(meas_fn_t fn, int range, afe_aux_t aux);

    /* Panel bus, driven by lib/display. */
    const lcd_io_t *lcd;

    uint32_t  (*millis)(void);
    app_key_t (*key_poll)(void);            /* KEY_NONE when idle */
    void      (*beep)(bool on);
    /* Beeper with pitch and drive (B57): level 0-100 (% of the strongest
     * drive), 0 = silent. NULL: the app falls back to beep(level > 0). */
    void      (*tone)(uint16_t hz, uint8_t level);
    void      (*backlight)(uint8_t pct);
    /* AD1 speed, the SPEED setting (B11): 0 LOW (R22 0x16, ~246 ms a
     * conversion), 1 MID (0x15, ~123 ms), 2 HIGH (0x14, ~61 ms). Applies to
     * the AD1 rows at 0x16 (DC V, DC I, ohms, temperature); takes effect at
     * the next configure. 3 (APP_RATE_REC): R22 0x12, ~15 ms, for fast
     * recording (B24). NULL: fixed. */
    void      (*afe_rate)(int rate);
    /* Real-time clock (B33): Unix seconds, 0 while not set or not running;
     * set it to Unix seconds. NULL: no clock. */
    uint32_t  (*clock_now)(void);
    void      (*clock_set)(uint32_t unix_s);

    const meas_cal_t *cal;                  /* parsed factory record */
    /* Implausible slots found when loading it: 0 = factory values in use,
     * >0 = rejected and unity used instead. NULL: unknown. */
    int       (*cal_status)(void);

    /* Battery voltage in mV, or -1 (B47: PB0 through a 1:2 divider on the
     * XDM1241). NULL: not measured. */
    int       (*battery_mv)(void);

    /* SCPI serial port. NULL: no remote control. read returns bytes copied. */
    size_t    (*serial_read)(char *buf, size_t max);
    void      (*serial_write)(const char *s, size_t n);
    const char *serial_number;              /* for *IDN?, may be NULL */
    void      (*serial_baud)(uint32_t baud);  /* may be NULL */

    /* Settings flash (lib/store). NULL: settings are not persisted. */
    const store_io_t *store;
    /* Data-log flash (lib/datalog, 0x08030000-0x0803A7FF). NULL: no recording. */
    const store_io_t *log_flash;
    /* Ranges the board can measure. NULL: every range. The app keeps
     * ranging and the range keys inside them (a real front end with rows
     * for only some ranges of a function). */
    bool      (*range_avail)(meas_fn_t fn, int range);
    /* Functions the board can measure. NULL: all. One it can't (no checked
     * rows yet) is never selected, so it never shows a simulated reading
     * as if it were real: its key does nothing, SCPI answers -221. */
    bool      (*fn_avail)(meas_fn_t fn);
    /* B21: volts and current measured in turn on one connection (DC
     * current range irange with DC volts range vrange, -1 = any), for
     * power and energy. NULL: not offered. The meter returns NULL until the
     * bench shows the V input reads right while current flows and that
     * switching rows leaves the current alone (docs/wishlist.md B21). */
    bool      (*vi_avail)(int irange, int vrange);
    /* B21: what a voltage on the V jack adds to DC current range irange's
     * reading, in amps, given that voltage (power mode subtracts it). On our
     * meter the V input's clamp upsets the current reading above ~2.15 V
     * (test-plan.md, 2026-11-16). NULL: nothing. */
    double    (*vi_leak)(int irange, double volts);
    /* User-calibration flash (lib/app/ucal, 0x0803A800-0x0803B7FF). NULL: none. */
    const store_io_t *ucal_flash;

    /* Render buffer: SCR_W x band_rows pixels. The screen is drawn band by
     * band, so this is the only frame memory the app needs. */
    rgb565_t *band;
    int       band_rows;
} platform_t;

#endif
