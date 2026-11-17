/* Pure measurement/display logic - no registers, no hardware.
 * Everything here is unit-tested on the host (pio test -e native). */
#ifndef MEASURE_H
#define MEASURE_H

#include <stddef.h>

typedef enum { UNIT_VOLT, UNIT_AMP, UNIT_OHM, UNIT_FARAD, UNIT_HERTZ } unit_t;

/* Format a reading into an engineering-notation display string, e.g.
 * 0.0472 V -> "47.200 mV". Returns the number of characters written. */
size_t fmt_reading(char *out, size_t cap, double value, unit_t unit, int digits);

/* Pick the smallest range index whose full-scale contains |value|.
 * ranges must be ascending; returns the last index if nothing fits. */
int pick_range(const double *ranges, int n, double value);

#endif
