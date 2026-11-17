/* Measurement arithmetic. See meas.h and docs/measurement.md.
 *
 * Constants that stock holds as float32 are written as float literals cast to
 * double, so they carry stock's exact value (1.36667f is 1.36667001247...).
 */
#include "meas.h"

#include <math.h>
#include <string.h>

#define FULL        8388606.0                  /* 0x4AFFFFFE, 2^23 - 2 */
#define RMS_FULL    262143.0                   /* 0x410FFFF8 (double), 2^18 - 1 */
#define K_DC        ((double)1.36667f)         /* 0x3FAEEF0B */
#define K_DCV_5V    ((double)7.51667f)         /* 0x40F08890, DCV range 2 */
#define CJ_G_DEFAULT 0.169465                  /* 0x3FC5B107746887A9 */
#define CJ_G_RAW_DEFAULT 1694

/* ---- range tables ------------------------------------------------------- */

static const int8_t nranges[MEAS_NFUNC] = {
    [MEAS_DCV] = 6, [MEAS_ACV] = 5, [MEAS_DCI] = 6, [MEAS_ACI] = 6,
    [MEAS_OHM2] = 6, [MEAS_OHM4] = 6, [MEAS_CONT] = 1, [MEAS_DIODE] = 1,
    [MEAS_CAP] = 7, [MEAS_FREQ] = 6, [MEAS_TEMP] = 3,   /* type K, PT100, PT1000 (B28) */
};

/* int16 FS[func][7], .data 0x2000146E. Verified against the manual's
 * specification table (docs/calibration.md). */
static const int16_t fullscale[MEAS_NFUNC][MEAS_MAXRANGE] = {
    [MEAS_DCV]   = { 50, 500, 5, 50, 500, 1000, 0 },
    [MEAS_ACV]   = { 500, 5, 50, 500, 750, 0, 0 },
    [MEAS_DCI]   = { 500, 5, 50, 500, 5, 10, 0 },
    [MEAS_ACI]   = { 500, 5, 50, 500, 5, 10, 0 },
    [MEAS_OHM2]  = { 500, 5, 50, 500, 5, 50, 100 },
    [MEAS_OHM4]  = { 500, 5, 50, 500, 5, 50, 100 },
    [MEAS_CONT]  = { 1100, 1100, 1100, 1100, 1100, 1100, 1100 },
    [MEAS_DIODE] = { 3, 3, 3, 3, 3, 3, 3 },
    [MEAS_CAP]   = { 50, 500, 5, 50, 500, 5, 50 },
    [MEAS_FREQ]  = { 50, 500, 5, 50, 500, 5, 50 },
    [MEAS_TEMP]  = { 0, 0, 0, 0, 0, 0, 0 },
};

/* Decimal exponent of each range's unit, .data 0x20001614. */
static const int8_t unit_exp[MEAS_NFUNC][MEAS_MAXRANGE] = {
    [MEAS_DCV]  = { -3, -3, 0, 0, 0, 0, 0 },
    [MEAS_ACV]  = { -3, 0, 0, 0, 0, 0, 0 },
    [MEAS_DCI]  = { -6, -3, -3, -3, 0, 0, 0 },
    [MEAS_ACI]  = { -6, -3, -3, -3, 0, 0, 0 },
    [MEAS_OHM2] = { 0, 3, 3, 3, 6, 6, 0 },
    [MEAS_OHM4] = { 0, 3, 3, 3, 6, 6, 0 },
    [MEAS_CAP]  = { -9, -9, -6, -6, -6, -3, -3 },
    [MEAS_FREQ] = { 0, 3, 6, 0, -3, -6, 0 },
};

static bool valid(meas_fn_t fn, int range)
{
    return (unsigned)fn < MEAS_NFUNC && range >= 0 && range < nranges[fn];
}

int meas_nranges(meas_fn_t fn)
{
    return (unsigned)fn < MEAS_NFUNC ? nranges[fn] : 0;
}

int meas_fullscale(meas_fn_t fn, int range)
{
    return valid(fn, range) ? fullscale[fn][range] : 0;
}

int meas_unit_exp(meas_fn_t fn, int range)
{
    return valid(fn, range) ? unit_exp[fn][range] : 0;
}

double meas_to_si(meas_fn_t fn, int range, double v)
{
    return v * pow(10.0, meas_unit_exp(fn, range));
}

/* ---- calibration ---------------------------------------------------------- */

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

void meas_cal_defaults(meas_cal_t *cal)
{
    for (int f = 0; f < MEAS_NFUNC; f++)
        for (int s = 0; s < MEAS_NSLOT; s++) {
            cal->coef[f][s] = 1000000;
            cal->zero[f][s] = 0;
        }
    cal->cj_g = CJ_G_RAW_DEFAULT;
}

int meas_cal_parse(meas_cal_t *cal, const uint8_t *rec, size_t len)
{
    if (len < MEAS_CAL_RECORD_LEN) return -1;
    for (int f = 0; f < MEAS_NFUNC; f++)
        for (int s = 0; s < MEAS_NSLOT; s++) {
            size_t o = (size_t)(f * MEAS_NSLOT + s) * 4u;
            cal->coef[f][s] = le32(rec + MEAS_CAL_COEF_OFF + o);
            cal->zero[f][s] = le32(rec + MEAS_CAL_ZERO_OFF + o);
        }
    cal->cj_g = le32(rec + MEAS_CAL_CJG_OFF);
    return 0;
}

/* 0x08005700 tail: cap ranges 4-6 clamp to 0.6-1.4; cap 0-3 are not clamped;
 * current functions allow up to 1.3; everything else 0.8-1.2. */
bool meas_cal_clamp(meas_fn_t fn, int range, int32_t *lo, int32_t *hi)
{
    if (fn == MEAS_CAP) {
        if (range < 4) return false;
        *lo = 600000; *hi = 1400000;
        return true;
    }
    *lo = 800000;
    *hi = (fn == MEAS_DCI || fn == MEAS_ACI) ? 1300000 : 1200000;
    return true;
}

/* Functions that own coef rows; the others borrow (see measurement.md). */
const meas_fn_t meas_cal_owners[MEAS_CAL_NOWNERS] = {
    MEAS_DCV, MEAS_ACV, MEAS_DCI, MEAS_ACI, MEAS_OHM2, MEAS_CAP };

int meas_cal_nslots(meas_fn_t fn)
{
    if (fn >= MEAS_NFUNC) return 0;
    return fn == MEAS_ACV ? 4 : nranges[fn];     /* 750 V reuses 500 V's slot */
}

bool meas_cal_slot_ok(const meas_cal_t *cal, meas_fn_t fn, int r)
{
    int32_t lo = 600000, hi = 1400000;
    (void)meas_cal_clamp(fn, r, &lo, &hi);
    int32_t c = cal->coef[fn][r];
    return c >= lo && c <= hi;
}

int meas_cal_validate(const meas_cal_t *cal)
{
    int bad = 0;
    for (int i = 0; i < MEAS_CAL_NOWNERS; i++)
        for (int r = 0; r < meas_cal_nslots(meas_cal_owners[i]); r++)
            if (!meas_cal_slot_ok(cal, meas_cal_owners[i], r)) bad++;
    return bad;
}

static double gain(const meas_cal_t *cal, meas_fn_t fn, int slot)
{
    return (double)cal->coef[fn][slot] / 1e6;
}

/* ---- DC ------------------------------------------------------------------- */

/* stock_meas_dcv 0x08008CCE. Offset only on 50 mV and 500 mV. */
double meas_dcv(const meas_cal_t *cal, int range, int32_t ad1)
{
    if (!valid(MEAS_DCV, range)) return NAN;
    double k = (range == 2) ? K_DCV_5V : K_DC * fullscale[MEAS_DCV][range];
    double v = (double)ad1 / FULL * k * gain(cal, MEAS_DCV, range);
    if (range <= 1) v -= (double)cal->zero[MEAS_DCV][range] / 1e6;
    return meas_to_si(MEAS_DCV, range, v);
}

/* 0x080092AC. No offset term. */
double meas_dci(const meas_cal_t *cal, int range, int32_t ad1)
{
    if (!valid(MEAS_DCI, range)) return NAN;
    double v = (double)ad1 / FULL * K_DC * fullscale[MEAS_DCI][range]
             * gain(cal, MEAS_DCI, range);
    return meas_to_si(MEAS_DCI, range, v);
}

/* ---- AC (true RMS) -------------------------------------------------------- */

static const double k_acv[5] = { 2460.0, 27.06, 246.0, 2460.0, 2460.0 }; /* 0x0801DDD0 */
static const double k_aci[6] = { 2460.0, 24.6, 246.0, 2460.0, 24.6, 246.0 }; /* 0x0801DDF8 */

/* 0x080093EA / 0x080094A8, RMS sum from 0x08009108 (top byte masked to 6 bits). */
double meas_ac(const meas_cal_t *cal, meas_fn_t fn, int range,
               uint64_t rms_sq, bool cal_mode)
{
    if ((fn != MEAS_ACV && fn != MEAS_ACI) || !valid(fn, range)) return NAN;
    rms_sq &= 0x3FFFFFFFFFull;
    double k = (fn == MEAS_ACV) ? k_acv[range] : k_aci[range];
    int slot = (fn == MEAS_ACV && range == 4) ? 3 : range;
    double v = sqrt((double)rms_sq) / RMS_FULL * k * gain(cal, fn, slot);
    if (fn == MEAS_ACI && !cal_mode && v < fullscale[fn][range] * 0.01)
        v = 0.0;
    return meas_to_si(fn, range, v);
}

/* ---- resistance ------------------------------------------------------------ */

static const double k_ohm[6] = { 1111.11, 11.11, 111.11, 1111.11, 6.67, 66.7 }; /* 0x0801DE28 */
/* Low-range offset scaling, 0x080097CC..0x080097EC: Ca / FS. */
static const double ohm_ca[3] = { 0.23404255319148937, 0.46564885496183206,
                                  0.5175438596491229 };
static const double ohm_cb[3] = { 500.0, 5.0, 50.0 };

/* 0x08009566. 2W and 4W share the f4 gain row; only 2W has an offset. The
 * low-range term scales with |v|, because stock works on the magnitude. */
double meas_ohm(const meas_cal_t *cal, meas_fn_t fn, int range,
                int32_t ad1, bool cal_mode)
{
    if ((fn != MEAS_OHM2 && fn != MEAS_OHM4) || !valid(fn, range)) return NAN;
    double v = (double)ad1 / FULL * k_ohm[range] * gain(cal, MEAS_OHM2, range);
    double off = (fn == MEAS_OHM2) ? (double)cal->zero[MEAS_OHM2][range] / 1e6 : 0.0;
    double r = (range <= 2) ? v - off * (1.0 + ohm_ca[range] * fabs(v) / ohm_cb[range])
                            : v - off;
    if (!cal_mode && r < 0.0) r = 0.0;
    return meas_to_si(fn, range, r);
}

/* 0x08009E76. Borrows the 500 ohm calibration; a negative ADC reading shows 0. */
double meas_continuity(const meas_cal_t *cal, int32_t ad1)
{
    if (ad1 < 0) return 0.0;
    double v = (double)ad1 / FULL * 1000.0 / (double)0.9f
             * gain(cal, MEAS_OHM2, 0)
             - (double)cal->zero[MEAS_OHM2][0] / 1e6;
    return v;
}

/* 0x08009DA2. Borrows the 5 V DCV calibration; offset is x 1e9. */
double meas_diode(const meas_cal_t *cal, int32_t ad1)
{
    double v = (double)ad1 / FULL * 1.23 * 5.555555820465088 / 10.0
             * 81.28907775878906 * gain(cal, MEAS_DCV, 2)
             - (double)cal->zero[MEAS_DCV][2] / 1e9;
    return v;
}

/* ---- capacitance ----------------------------------------------------------- */

/* 0x0800971E ranges 0-4: r = CTC/CTB, then a per-range divisor chain. */
double meas_cap_counter(const meas_cal_t *cal, int range, uint32_t ctb, uint32_t ctc)
{
    if (range < 0 || range > 4 || ctb == 0) return NAN;
    double r = (double)ctc / (double)ctb;
    switch (range) {
    case 0: case 1: r = r / 300000.0 * 1000.0; break;
    case 2:         r = r / 80000.0 / 3.0;     break;
    case 3:         r = r / 8000.0 / 3.0;      break;
    default:        r = r / 7394.0;            break;
    }
    double c = r * gain(cal, MEAS_CAP, range) - (double)cal->zero[MEAS_CAP][range] / 1e4;
    return meas_to_si(MEAS_CAP, range, c);
}

/* stock_ad1_read_scale 0x0800889C. */
double meas_cap_volts(int32_t ad1)
{
    return (double)ad1 / FULL * (double)1.2f / (double)1.8f;
}

/* 0x0800971E ranges 5-6: C = 2 * count * 0.18e-3 / dV, in the range's unit. */
double meas_cap_slope(const meas_cal_t *cal, int range, double v_prev,
                      double v_now, uint32_t count, bool *overrange)
{
    if (overrange) *overrange = false;
    if (range < 5 || range > 6) return NAN;
    double dv = v_now - v_prev;
    if (dv == 0.0) return NAN;
    double c = ldexp((double)count, 1) / 1000.0 / dv * 0.00018 * 1000.0
             * gain(cal, MEAS_CAP, range) - (double)cal->zero[MEAS_CAP][range] / 1e4;
    if (overrange && v_now >= (double)0.666666f) *overrange = true;
    return meas_to_si(MEAS_CAP, range, c);
}

/* Ranges 5-6 from the charge rate, dv/dt in meas_cap_volts() units per
 * second, fitted over the charge (acquire.c). Stock's formula with the
 * count as time: C = 2 x count / 1000 / dV x 0.18 in mF, dV across `ticks`
 * ms, so C = 0.36 x (count / ticks) / (dv/dt). Range 6: 2000 / 2000; range
 * 5: 277 / 200, which the factory coefficient undoes (ours 0.710: 0.710 x
 * 1.385 = 0.98, range 6's 0.988). On our meter 2026-11-15: 18 mF -> 18.77,
 * 1000 uF -> 1.10 mF (docs/measurement.md). */
double meas_cap_rate(const meas_cal_t *cal, int range, double dvdt)
{
    if (range < 5 || range > 6 || !(dvdt > 0.0)) return NAN;
    double k = range == 5 ? 277.0 / 200.0 : 1.0;
    double c = 0.36 * k / dvdt * gain(cal, MEAS_CAP, range) - (double)cal->zero[MEAS_CAP][range] / 1e4;
    return meas_to_si(MEAS_CAP, range, c);
}

/* ---- frequency ------------------------------------------------------------- */

/* 0x08009B4E: gate = (CTA + 2^20) / 4 MHz. */
double meas_freq(uint32_t cta, uint32_t ctb, uint32_t ctc, double *duty_pct)
{
    double ref = (double)(cta & 0xFFFFFFu) + 1048576.0;
    if (duty_pct) *duty_pct = (double)(ctc & 0xFFFFFFu) / ref * 100.0;
    return (double)(ctb & 0xFFFFFFu) / (ref / 4e6);
}

int meas_freq_code(double hz, bool period)
{
    if (period) {
        if (hz == 0.0) return 3;
        double p = fabs(1.0 / hz);
        if (p >= 1.0)   return 3;
        if (p >= 0.001) return 4;
        return 5;
    }
    double a = fabs(hz);
    if (a < 1000.0) return 0;
    if (a < 1e6)    return 1;
    return 2;
}

/* ---- post-processing ------------------------------------------------------- */

double meas_suppress_small(meas_fn_t fn, int range, double value,
                           bool cal_mode, bool rel_on)
{
    if (cal_mode || rel_on) return value;
    if (fn != MEAS_ACV && fn != MEAS_ACI && fn != MEAS_CAP) return value;
    if (!valid(fn, range)) return value;
    double lim = meas_to_si(fn, range, fullscale[fn][range] * 0.01);
    return fabs(value) < lim ? 0.0 : value;
}

void meas_filter_reset(meas_filter_t *f)
{
    memset(f, 0, sizeof *f);
}

void meas_filter_push(meas_filter_t *f, double v, double step)
{
    bool jump = f->have_prev && fabs(v - f->prev) > step;
    f->prev = v;
    f->have_prev = true;
    if (jump) {
        /* Stock fills all 200 slots with v and stops accepting more until
         * the next display update: the shown value is the step sample. */
        f->buf[0] = v;
        f->n = 1;
        f->pinned = true;
        return;
    }
    if (f->pinned || f->n >= MEAS_FILTER_LEN) return;
    f->buf[f->n++] = v;
}

bool meas_filter_take(meas_filter_t *f, double *mean)
{
    if (f->n == 0) return false;
    double s = 0.0;
    for (int i = 0; i < f->n; i++) s += f->buf[i];
    *mean = s / f->n;
    f->n = 0;
    f->pinned = false;
    return true;
}

double meas_step_threshold(meas_fn_t fn, int range)
{
    return meas_to_si(fn, range, meas_fullscale(fn, range) * (double)0.1f);
}

const uint16_t meas_dbm_rref[MEAS_NRREF] = {
    50, 75, 93, 110, 124, 125, 135, 150, 250, 300, 500, 600, 800, 900, 1000, 1200, 8000
};

double meas_dbm(double v, double rref)
{
    double p = v * v / rref * 1000.0;
    if (p == 0.0 || !isnormal(p)) p = 1e-9;
    return 10.0 * log10(p);
}

double meas_db(double v, double rref, int32_t ref_centi_dbm)
{
    return meas_dbm(v, rref) - (double)ref_centi_dbm / 100.0;
}
