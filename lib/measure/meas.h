/* Measurement arithmetic: HY3131 register values -> calibrated readings.
 *
 * A reimplementation of the stock v2.1.2 measurement path, recovered offline.
 * The derivation, addresses and confidence of every constant is in
 * docs/measurement.md and docs/calibration.md; comments here cite the stock
 * address each piece came from.
 *
 * Pure functions: no registers, no hardware, no globals. Inputs are the raw
 * values a HAL reads from the HY3131; outputs are SI units (V, A, ohm, F, Hz).
 *
 * Deliberate differences from stock, all documented in docs/measurement.md:
 *   - two's-complement AD1 everywhere (stock's diode and large-capacitance
 *     paths use one's complement: one LSB off on negative input)
 *   - double precision throughout (stock mixes float and double)
 *   - temperature from the IEC 60751 / NIST ITS-90 equations, not stock's
 *     tables, which contain typos (see meas_temp.h)
 */
#ifndef MEAS_H
#define MEAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Function codes, as stored at stock measurement-state +0x11. */
typedef enum {
    MEAS_DCV = 0,
    MEAS_ACV,
    MEAS_DCI,
    MEAS_ACI,
    MEAS_OHM2,
    MEAS_OHM4,
    MEAS_CONT,
    MEAS_DIODE,
    MEAS_CAP,
    MEAS_FREQ,
    MEAS_TEMP,
    MEAS_NFUNC
} meas_fn_t;

#define MEAS_NSLOT      10          /* calibration slots per function */
#define MEAS_MAXRANGE    7

/* ---- calibration ---------------------------------------------------------
 * Parsed from the 1392-byte factory record (flash 0x0803F800). Values are
 * the raw stored integers: coef = gain x 1e6, zero = offset x 1e6 (x 1e4 for
 * capacitance, x 1e9 for diode), cj_g = cold-junction G x 1e4.
 */
typedef struct {
    int32_t coef[MEAS_NFUNC][MEAS_NSLOT];   /* record +0x1F8 */
    int32_t zero[MEAS_NFUNC][MEAS_NSLOT];   /* record +0x3B0 */
    int32_t cj_g;                           /* record +0x56C */
} meas_cal_t;

#define MEAS_CAL_RECORD_LEN   0x570u
#define MEAS_CAL_COEF_OFF     0x1F8u
#define MEAS_CAL_ZERO_OFF     0x3B0u
#define MEAS_CAL_CJG_OFF      0x56Cu

/* Unity gain, zero offset, default G. For running without a real record. */
void meas_cal_defaults(meas_cal_t *cal);

/* Parse a factory record (little-endian). Returns 0, or -1 if len is short. */
int  meas_cal_parse(meas_cal_t *cal, const uint8_t *rec, size_t len);

/* The clamp stock's calibration routine enforces when it writes a coef
 * (0x08005700). Returns false where stock applies no clamp. */
bool meas_cal_clamp(meas_fn_t fn, int range, int32_t *lo, int32_t *hi);

/* Count coef slots that are actually used and fall outside their clamp
 * (or outside 600000..1400000 where stock has none). 0 means plausible.
 * A blank record (all 0xFF) fails every slot. */
int  meas_cal_validate(const meas_cal_t *cal);
/* B32: the functions that own calibration slots, how many each has, and
 * whether one slot's gain is plausible (the test meas_cal_validate counts). */
#define MEAS_CAL_NOWNERS 6
extern const meas_fn_t meas_cal_owners[MEAS_CAL_NOWNERS];
int  meas_cal_nslots(meas_fn_t fn);
bool meas_cal_slot_ok(const meas_cal_t *cal, meas_fn_t fn, int r);

/* ---- ranges ---------------------------------------------------------------- */

/* Number of ranges stock offers for a function (frequency: 6 unit codes;
 * temperature: 2 sensors). */
int     meas_nranges(meas_fn_t fn);

/* Full scale in the range's own unit (stock .data 0x2000146E): DCV range 0
 * is 50 (mV), range 2 is 5 (V). */
int     meas_fullscale(meas_fn_t fn, int range);

/* Decimal exponent of the range's unit (stock .data 0x20001614). */
int     meas_unit_exp(meas_fn_t fn, int range);

/* Convert a value in the range's own unit to SI. */
double  meas_to_si(meas_fn_t fn, int range, double v);

/* ---- per-function conversions -------------------------------------------
 * ad1:    AD1 as a signed 24-bit count (hy3131_sext24)
 * rms_sq: RMS_DATA raw 40-bit value (hy3131 0x09..0x0D, low byte first)
 * cal_mode: true while calibrating; disables stock's low-reading clamps.
 */
double meas_dcv(const meas_cal_t *cal, int range, int32_t ad1);
double meas_dci(const meas_cal_t *cal, int range, int32_t ad1);
double meas_ac(const meas_cal_t *cal, meas_fn_t fn, int range,
               uint64_t rms_sq, bool cal_mode);
double meas_ohm(const meas_cal_t *cal, meas_fn_t fn, int range,
                int32_t ad1, bool cal_mode);
double meas_continuity(const meas_cal_t *cal, int32_t ad1);
double meas_diode(const meas_cal_t *cal, int32_t ad1);

/* Capacitance ranges 0-4: HY3131 counters CTB and CTC. */
double meas_cap_counter(const meas_cal_t *cal, int range,
                        uint32_t ctb, uint32_t ctc);

/* Capacitance ranges 5-6: charge slope. v_prev/v_now from meas_cap_volts(),
 * count is the charge-time count (stock state +0x80; 2000 on range 6).
 * Sets *overrange when v_now reaches stock's 0.666666 limit. */
double meas_cap_volts(int32_t ad1);
double meas_cap_slope(const meas_cal_t *cal, int range, double v_prev,
                      double v_now, uint32_t count, bool *overrange);

/* Capacitance ranges 5-6 from the fitted charge rate (volts as
 * meas_cap_volts, per second): what the app uses. NAN unless dvdt > 0. */
double meas_cap_rate(const meas_cal_t *cal, int range, double dvdt);
#define MEAS_CAP_V_FULL 0.6666          /* AD1 clips here: stock's over-range */
#define MEAS_CAP_V_REST 0.097           /* phase A's rest level, on our meter */

/* Frequency from counters CTA/CTB/CTC. Writes duty cycle in % if non-NULL. */
double meas_freq(uint32_t cta, uint32_t ctb, uint32_t ctc, double *duty_pct);

/* Stock's auto unit code for frequency/period (meas_unit_exp(MEAS_FREQ, c)):
 * 0 Hz, 1 kHz, 2 MHz; period mode 3 s, 4 ms, 5 us. */
int    meas_freq_code(double hz, bool period);

/* ---- post-processing ------------------------------------------------------ */

/* Stock zeroes ACV, ACI and capacitance readings below 1% of full scale,
 * except while calibrating or with REL on (0x0800A30A). value is SI. */
double meas_suppress_small(meas_fn_t fn, int range, double value,
                           bool cal_mode, bool rel_on);

/* Sample filter (0x08008B5E + 0x0800A30A): accumulate samples between display
 * updates; a step larger than 10% of full scale restarts the average at the
 * new sample and holds it until the next update, as stock does. */
#define MEAS_FILTER_LEN 200
typedef struct {
    double buf[MEAS_FILTER_LEN];
    int    n;
    bool   pinned;
    bool   have_prev;
    double prev;
} meas_filter_t;

void   meas_filter_reset(meas_filter_t *f);
void   meas_filter_push(meas_filter_t *f, double v, double step);
/* Mean of what was pushed since the last call; resets. Returns false if empty. */
bool   meas_filter_take(meas_filter_t *f, double *mean);
/* Step threshold for a range, SI: 10% of full scale. */
double meas_step_threshold(meas_fn_t fn, int range);

/* dB / dBm (0x0800E1F2). v in volts RMS. */
#define MEAS_NRREF 17
extern const uint16_t meas_dbm_rref[MEAS_NRREF];    /* ohms, 0x0801E298 */
double meas_dbm(double v, double rref);
/* dB relative to a reference held, as stock does, in 0.01 dBm units. */
double meas_db(double v, double rref, int32_t ref_centi_dbm);

#endif
