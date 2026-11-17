/* Overload limits, autoranging, and turning a reading into display text.
 *
 * Overload limits are the user manual's (section 3 and specification note
 * [2]): 10% over range everywhere except the top ranges, which stop at
 * 1050 V, 787.5 V, 10.5 A, 55 Mohm and 50500 uF. Stock's own over-range
 * code (0x08009FE8) is not yet recovered.
 *
 * Autoranging is ours, not stock's (stock's is slow, docs/backlog.md B10).
 * It moves in at most two hops, not one range at a time:
 *   - overload: straight to the top range. An overloaded reading says only
 *     "too big", and the top range measures the real value.
 *   - below 90% of the next range down: straight to the smallest range that
 *     holds the reading, with a margin for the current range's resolution.
 *   - above 105% of full scale but not overloaded: one range up.
 * Capacitance keeps one step at a time: its high ranges are slow to measure.
 *
 * Current is two inputs, never one ladder: the mA jack (ranges 0-3, 500 uA to
 * 500 mA) and the A jack (4-5, 5 A and 10 A). Current flows only through the
 * jack the lead is in, so ranging stays on the selected input (stock V4.3.0
 * does the same: from 5 A, range DOWN stays at 5 A). Overload goes to the
 * top of that input, not of the ladder.
 */
#ifndef APP_READOUT_H
#define APP_READOUT_H

#include <stdbool.h>
#include <stddef.h>

#include "meas.h"

/* Overload threshold for a range, SI. */
double readout_limit(meas_fn_t fn, int range);
bool   readout_overload(meas_fn_t fn, int range, double si);

/* Functions with a range ladder the app can step through. */
bool   readout_autoranges(meas_fn_t fn);

/* Next range for an autoranging function given a reading (SI); returns the
 * current range when no change is needed. */
int    readout_autorange(meas_fn_t fn, int range, double si, bool overload);

#define READOUT_CURRENT_A_FIRST 4      /* first A-jack current range */

/* The ranges autoranging may move between from `range`: the whole ladder,
 * or for current the ranges of the selected jack. */
void   readout_range_span(meas_fn_t fn, int range, int *lo, int *hi);

/* Nominal full scale of a range, SI (5 V on the 5 V range). */
double readout_fullscale(meas_fn_t fn, int range);

/* Display text for the main reading, in the range's own unit, at stock's
 * 5.5-digit resolution (50.000 mV, 500.00 mV, 5.0000 V, ... 1000.0 V).
 * For MEAS_FREQ pass the unit code (meas_freq_code) as range, and si in Hz,
 * or in seconds for period mode.
 * Writes "overload" when over range, "Open" for an open continuity/diode.
 * Only characters the stock big font has are emitted. */
size_t readout_value(char *out, size_t cap, meas_fn_t fn, int range, double si,
                     bool overload);

/* Unit label for the range, e.g. "mV DC", "kOhm", "uF", "Hz", "C". */
const char *readout_unit(meas_fn_t fn, int range, bool period);

/* Short range label for the footer, e.g. "5 V", "500 kOhm". */
size_t readout_range_label(char *out, size_t cap, meas_fn_t fn, int range);

/* Function name for the footer and status bar. */
const char *readout_fn_name(meas_fn_t fn);

#endif
