/* Temperature conversions. See meas_temp.h. */
#include "meas_temp.h"

#include <math.h>

/* ---- PT100, IEC 60751 ---------------------------------------------------- */

#define CVD_A  3.9083e-3
#define CVD_B -5.775e-7
#define CVD_C -4.183e-12

double meas_pt100_ohm(double t)
{
    double r = 1.0 + CVD_A * t + CVD_B * t * t;
    if (t < 0.0) r += CVD_C * (t - 100.0) * t * t * t;
    return 100.0 * r;
}

double meas_pt100_temp(double r, bool *overrange)
{
    /* Closed form for t >= 0 (the C term vanishes), then Newton below 0 C. */
    double q = 1.0 - r / 100.0;
    double t = (-CVD_A + sqrt(CVD_A * CVD_A - 4.0 * CVD_B * q)) / (2.0 * CVD_B);
    if (r < 100.0) {
        for (int i = 0; i < 20; i++) {
            double f  = meas_pt100_ohm(t) - r;
            double df = 100.0 * (CVD_A + 2.0 * CVD_B * t
                      + CVD_C * (4.0 * t * t * t - 300.0 * t * t));
            double dt = f / df;
            t -= dt;
            if (fabs(dt) < 1e-9) break;
        }
    }
    /* 1 micro-degree of slack so a boundary value does not flag on rounding */
    if (overrange) *overrange = (t < MEAS_PT100_MIN - 1e-6 || t > MEAS_PT100_MAX + 1e-6);
    return t;
}

/* ---- type K, NIST ITS-90 ------------------------------------------------- */

static const double k_neg[11] = {     /* -270 .. 0 C */
    0.0,
    0.394501280250e-01,  0.236223735980e-04, -0.328589067840e-06,
   -0.499048287770e-08, -0.675090591730e-10, -0.574103274280e-12,
   -0.310888728940e-14, -0.104516093650e-16, -0.198892668780e-19,
   -0.163226974860e-22,
};
static const double k_pos[10] = {     /* 0 .. 1372 C */
   -0.176004136860e-01,  0.389212049750e-01,  0.185587700320e-04,
   -0.994575928740e-07,  0.318409457190e-09, -0.560728448890e-12,
    0.560750590590e-15, -0.320207200030e-18,  0.971511471520e-22,
   -0.121047212750e-25,
};
#define K_A0  0.118597600000e+00
#define K_A1 -0.118343200000e-03
#define K_A2  0.126968600000e+03

static double poly(const double *c, int n, double t)
{
    double s = 0.0;
    for (int i = n - 1; i >= 0; i--) s = s * t + c[i];
    return s;
}

double meas_tc_k_mv(double t)
{
    if (t < 0.0) return poly(k_neg, 11, t);
    double u = t - K_A2;
    return poly(k_pos, 10, t) + K_A0 * exp(K_A1 * u * u);
}

/* The EMF is monotonic over the whole domain, so bisection always converges;
 * it is cheap enough at display rates and has no failure mode. */
double meas_tc_k_temp(double e, bool *overrange)
{
    double lo = -270.0, hi = 1372.0;
    bool out = e < meas_tc_k_mv(MEAS_TC_K_MIN) || e > meas_tc_k_mv(MEAS_TC_K_MAX);
    if (overrange) *overrange = out;
    if (e <= meas_tc_k_mv(lo)) return lo;           /* polynomial domain ends */
    if (e >= meas_tc_k_mv(hi)) return hi;
    for (int i = 0; i < 60; i++) {
        double mid = 0.5 * (lo + hi);
        if (meas_tc_k_mv(mid) < e) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double meas_tc_k_compensated(double emf_mv, double t_cj, bool *overrange)
{
    return meas_tc_k_temp(emf_mv + meas_tc_k_mv(t_cj), overrange);
}

/* ---- cold junction ------------------------------------------------------- */

double meas_cj_gain(int32_t g_raw)
{
    double g = (double)g_raw / 10000.0;
    if (!(g >= 0.1 && g <= 0.18)) g = 0.169465;     /* 0x08008EE0 fallback */
    return g;
}

double meas_cold_junction(double v1_mv, double v2_mv, int32_t g_raw)
{
    double dv = ldexp(v2_mv - v1_mv, -1);
    return dv / meas_cj_gain(g_raw) - 273.15 - 16.0;
}

double meas_temp_unit(double t_c, meas_tunit_t unit)
{
    switch (unit) {
    /* Stock uses the float32 values of 1.8 and 273.15; exact is correct and
     * differs by under 1e-5 degrees across the range. */
    case MEAS_DEG_F:  return t_c * 1.8 + 32.0;
    case MEAS_KELVIN: return t_c + 273.15;
    default:          return t_c;
    }
}
