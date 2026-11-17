/* See readout.h. */
#include "readout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static double fs_si(meas_fn_t fn, int range)
{
    return meas_to_si(fn, range, meas_fullscale(fn, range));
}

static int int_digits(int fs)
{
    int n = 1;
    while (fs >= 10) { fs /= 10; n++; }
    return n;
}

double readout_limit(meas_fn_t fn, int range)
{
    double fs = fs_si(fn, range);
    int top = meas_nranges(fn) - 1;
    switch (fn) {
    case MEAS_DCV: if (range == top) return 1050.0;   break;
    case MEAS_ACV: if (range == top) return 787.5;    break;
    case MEAS_DCI:
    case MEAS_ACI: if (range == top) return 10.5;     break;
    case MEAS_OHM2:
    case MEAS_OHM4: if (range == top) return 55e6;    break;
    case MEAS_CAP: if (range == top) return 50500e-6; break;
    case MEAS_CONT:  return 1000.0;     /* manual: 1000 ohm range */
    case MEAS_DIODE: return 3.0;        /* manual: 3.0000 V */
    default: return INFINITY;
    }
    return fs * 1.1;
}

bool readout_overload(meas_fn_t fn, int range, double si)
{
    return isnan(si) || fabs(si) > readout_limit(fn, range);
}

bool readout_autoranges(meas_fn_t fn)
{
    switch (fn) {
    case MEAS_DCV: case MEAS_ACV: case MEAS_DCI: case MEAS_ACI:
    case MEAS_OHM2: case MEAS_OHM4: case MEAS_CAP:
        return true;
    default:
        return false;
    }
}

double readout_fullscale(meas_fn_t fn, int range) { return fs_si(fn, range); }

void readout_range_span(meas_fn_t fn, int range, int *lo, int *hi)
{
    *lo = 0;
    *hi = meas_nranges(fn) - 1;
    if (fn == MEAS_DCI || fn == MEAS_ACI) {
        if (range >= READOUT_CURRENT_A_FIRST) *lo = READOUT_CURRENT_A_FIRST;
        else *hi = READOUT_CURRENT_A_FIRST - 1;
    }
}

int readout_autorange(meas_fn_t fn, int range, double si, bool overload)
{
    if (!readout_autoranges(fn)) return range;
    int bottom, top;
    readout_range_span(fn, range, &bottom, &top);
    double v = fabs(si);
    if (fn == MEAS_CAP) {                        /* one step at a time */
        if (overload || v > fs_si(fn, range)) return range < top ? range + 1 : range;
        if (range > bottom && v < 0.9 * fs_si(fn, range - 1)) return range - 1;
        return range;
    }
    if (overload) return top;
    /* Up one past 105%: readings show to 110% (the manual's over-range), and
     * the 5% above full scale keeps noise at full scale from flapping. */
    if (v > 1.05 * fs_si(fn, range)) return range < top ? range + 1 : range;
    if (range == bottom || v >= 0.9 * fs_si(fn, range - 1)) return range;
    /* Down: the smallest range whose full scale holds the reading plus its
     * uncertainty here (2% and three counts of this range). */
    double need = v * 1.02 + 3.0 * fs_si(fn, range) / 50000.0;
    int r = bottom;
    while (r < range && fs_si(fn, r) < need) r++;
    return r;
}


size_t readout_value(char *out, size_t cap, meas_fn_t fn, int range, double si,
                     bool overload)
{
    int n;
    if ((fn == MEAS_CONT || fn == MEAS_DIODE) && overload)
        n = snprintf(out, cap, "Open");
    else if (overload)
        n = snprintf(out, cap, "overload");
    else if (fn == MEAS_TEMP)
        n = snprintf(out, cap, "%.1f", si);
    else if (fn == MEAS_FREQ) {
        /* range is the unit code (meas_freq_code); si is Hz or s */
        double v = si / pow(10.0, meas_unit_exp(fn, range));
        int dec = 5 - int_digits((int)fabs(v));
        n = snprintf(out, cap, "%.*f", dec < 0 ? 0 : dec, v);
    }
    else if (fn == MEAS_CONT)
        n = snprintf(out, cap, "%.1f", si);
    else if (fn == MEAS_DIODE)
        n = snprintf(out, cap, "%.4f", si);
    else {
        int fs = meas_fullscale(fn, range);
        int dec = 5 - int_digits(fs);
        if (dec < 0) dec = 0;
        double v = si / pow(10.0, meas_unit_exp(fn, range));
        if (fabs(v) < 0.5 * pow(10.0, -dec)) v = 0.0;   /* no "-0.000" */
        n = snprintf(out, cap, "%.*f", dec, v);
    }
    return n < 0 ? 0u : (size_t)n;
}

static const char *prefix(int e)
{
    switch (e) {
    case -9: return "n";
    case -6: return "u";
    case -3: return "m";
    case 3:  return "k";
    case 6:  return "M";
    default: return "";
    }
}

const char *readout_unit(meas_fn_t fn, int range, bool period)
{
    static char buf[16];
    int e = meas_unit_exp(fn, range);
    switch (fn) {
    case MEAS_DCV:   snprintf(buf, sizeof buf, "%sV DC", prefix(e)); break;
    case MEAS_ACV:   snprintf(buf, sizeof buf, "%sV AC", prefix(e)); break;
    case MEAS_DCI:   snprintf(buf, sizeof buf, "%sA DC", prefix(e)); break;
    case MEAS_ACI:   snprintf(buf, sizeof buf, "%sA AC", prefix(e)); break;
    case MEAS_OHM2:
    case MEAS_OHM4:  snprintf(buf, sizeof buf, "%sOhm", prefix(e)); break;
    case MEAS_CONT:  return "Ohm";
    case MEAS_DIODE: return "V";
    case MEAS_CAP:   snprintf(buf, sizeof buf, "%sF", prefix(e)); break;
    case MEAS_FREQ:  snprintf(buf, sizeof buf, "%s%s", prefix(e), period ? "s" : "Hz"); break;
    case MEAS_TEMP:  return "C";
    default:         return "";
    }
    return buf;
}

size_t readout_range_label(char *out, size_t cap, meas_fn_t fn, int range)
{
    int n;
    if (fn == MEAS_TEMP)
        n = snprintf(out, cap, "%s", range == 0 ? "TYPE K" : range == 1 ? "PT100" : "PT1000");
    else if (fn == MEAS_FREQ || fn == MEAS_CONT || fn == MEAS_DIODE)
        n = snprintf(out, cap, "-");
    else {
        const char *u = readout_unit(fn, range, false);
        char unit[8];
        size_t i = 0;
        while (u[i] && u[i] != ' ' && i < sizeof unit - 1) { unit[i] = u[i]; i++; }
        unit[i] = 0;
        n = snprintf(out, cap, "%d %s", meas_fullscale(fn, range), unit);
    }
    return n < 0 ? 0u : (size_t)n;
}

const char *readout_fn_name(meas_fn_t fn)
{
    static const char *names[MEAS_NFUNC] = {
        "DCV", "ACV", "DCI", "ACI", "OHM", "4W OHM", "CONT", "DIODE", "CAP", "FREQ", "TEMP",
    };
    return (unsigned)fn < MEAS_NFUNC ? names[fn] : "?";
}
