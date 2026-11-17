#include "measure.h"
#include <stdio.h>
#include <math.h>

static const char *unit_suffix(unit_t u)
{
    switch (u) {
    case UNIT_VOLT:  return "V";
    case UNIT_AMP:   return "A";
    case UNIT_OHM:   return "Ohm";
    case UNIT_FARAD: return "F";
    case UNIT_HERTZ: return "Hz";
    }
    return "?";
}

size_t fmt_reading(char *out, size_t cap, double value, unit_t unit, int digits)
{
    static const char *pfx[] = { "n", "u", "m", "", "k", "M" };
    static const double mul[] = { 1e9, 1e6, 1e3, 1.0, 1e-3, 1e-6 };

    double a = fabs(value);
    int i = 3; /* default: no prefix */

    if      (a == 0.0)  i = 3;
    else if (a < 1e-6)  i = 0;
    else if (a < 1e-3)  i = 1;
    else if (a < 1.0)   i = 2;
    else if (a < 1e3)   i = 3;
    else if (a < 1e6)   i = 4;
    else                i = 5;

    int n = snprintf(out, cap, "%.*f %s%s", digits, value * mul[i], pfx[i], unit_suffix(unit));
    return (n < 0) ? 0u : (size_t)n;
}

int pick_range(const double *ranges, int n, double value)
{
    double a = fabs(value);
    for (int i = 0; i < n; i++) {
        if (a <= ranges[i]) return i;
    }
    return n - 1;
}
