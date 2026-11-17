/* User calibration (backlog B31, docs/user-calibration.md): the owner's gain
 * and offset per function and range, on top of the factory calibration,
 * which is never written.
 *
 *   shown = value * (1 + g_ppm / 1e6) + o_ppm / 1e6 * full_scale
 *
 * Bounded: |g| <= 2%, |o| <= 1% of full scale; anything larger means a wrong
 * range or reference and is refused, not stored. Kept in its own
 * power-cut-safe store (two pages below the settings, taken from the top of
 * the data log), one 512-byte record holding only the non-zero slots.
 * Frequency and temperature are not corrected: their inputs are.
 */
#ifndef APP_UCAL_H
#define APP_UCAL_H

#include <stdbool.h>
#include <stdint.h>

#include "meas.h"
#include "store.h"

#define UCAL_PAGE0      0x0803A800u
#define UCAL_PAGE1      0x0803B000u
#define UCAL_REC_SIZE   512u
#define UCAL_VERSION    1
#define UCAL_MAX_G_PPM  20000      /* 2% */
#define UCAL_MAX_O_PPM  10000      /* 1% of full scale */
#define UCAL_NRANGE     8          /* ranges per function kept (all the meter has) */

typedef struct { int32_t g_ppm, o_ppm; } ucal_slot_t;

typedef struct {
    ucal_slot_t slot[MEAS_NFUNC][UCAL_NRANGE];
    store_t     st;
    bool        open;              /* storage available */
} ucal_t;

/* Whether a function takes a user correction at all. */
bool   ucal_applies(meas_fn_t fn);
/* Load from flash (io may be NULL: no storage, all zero). */
void   ucal_open(ucal_t *u, const store_io_t *io);
/* Save every non-zero slot. Returns 0, or -1 (storage refused or absent). */
int    ucal_save(ucal_t *u);
/* Apply the correction for fn/range to a value in SI. */
double ucal_apply(const ucal_t *u, meas_fn_t fn, int range, double v);
bool   ucal_active(const ucal_t *u, meas_fn_t fn, int range);
int    ucal_count(const ucal_t *u);

/* The two calibration steps, from the mean of uncorrected readings (raw)
 * on fn/range. They keep the slot's other term. Return 0, or a negative
 * reason: -1 not this function, -2 reading too large for a zero, -3
 * reference too small for a gain, -4 correction out of bounds. */
int    ucal_zero(ucal_t *u, meas_fn_t fn, int range, double raw);
int    ucal_gain(ucal_t *u, meas_fn_t fn, int range, double raw, double target);
void   ucal_clear(ucal_t *u, meas_fn_t fn, int range);
void   ucal_clear_all(ucal_t *u);

#endif
