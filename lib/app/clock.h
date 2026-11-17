/* Calendar arithmetic for the real-time clock (B33): Unix seconds <-> UTC
 * date and time. No time zones: the meter keeps whatever time it is set to.
 * Valid from 2000-01-01 to 2105; host-tested (test_app). */
#ifndef APP_CLOCK_H
#define APP_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int year, month, day, hour, min, sec; } clock_civil_t;

/* 0 if any field is out of range. */
uint32_t clock_from_civil(const clock_civil_t *c);
void     clock_to_civil(uint32_t t, clock_civil_t *c);
/* "2026-11-15 03:12:45" (sep ' ') or "2026-11-15T03:12:45" (sep 'T'). */
void     clock_fmt(char *out, size_t cap, uint32_t t, char sep);

#endif
