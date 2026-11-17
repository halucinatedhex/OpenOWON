/* Acquisition: configure the front end, wait for data, read the raw HY3131
 * registers, and convert them with lib/measure. Non-blocking: call
 * acq_poll() from the main loop and it returns true when a sample is ready.
 *
 * Data-ready flags follow stock (stock_hy_data_ready 0x08008C86): INTF is read
 * as a whole and every flag is latched, because reading INTF is assumed to
 * clear it - stock never writes INTF. AD1-based functions wait for AD1F, AC
 * for AD2F (stock checks the AD2 flag for RMS), counters for CTF.
 */
#ifndef APP_ACQUIRE_H
#define APP_ACQUIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "meas.h"
#include "ucal.h"
#include "platform.h"

/* Our choices, not recovered from stock. */
#define ACQ_SETTLE_MS      20u      /* after any reconfiguration */
#define ACQ_CJ_SETTLE_MS 1000u      /* stock busy-waits 1000 units between CJ reads */
/* Re-measuring the cold junction stops readings for ~2 s (two settles, as
 * stock). Stock does it every 600 s; we adapt (B25): every 10 s while it is
 * moving (warm-up, a hand on the terminals), doubling up to 1 min while it
 * holds within ACQ_CJ_STABLE_C. */
#define ACQ_CJ_PERIOD_MS     10000u
#define ACQ_CJ_PERIOD_MAX_MS 60000u
#define ACQ_CJ_STABLE_C      0.2
#define ACQ_DUAL_PRIMARY 5u         /* dual display: primary samples between secondaries */
/* Capacitance 5 / 50 mF (charge slope): rest in phase A, then charge in
 * phase B reading every AD1 conversion (one each ACQ_CAP_CONV_MS, measured
 * on our meter: R22 0x07), and fit the rise. Ours, on stock's hardware
 * path; stock rests 400 / 2500 ms and charges 200 / 2000 ms between two
 * reads, we charge longer to fit more points. */
#define ACQ_CAP_CONV_MS   246u
/* Capacitance counter ranges: a capacitor far above the range never ends
 * a charge cycle (on our meter, 2026-11-15: 1000 uF on 5 uF gave no reading
 * at all), so after this long the reading is over-range and autoranging
 * steps up. A 260 uF part takes ~3 s on 500 uF (docs/features.md). */
#define ACQ_CT_TIMEOUT_MS     4000u
#define ACQ_CT_TIMEOUT_TOP_MS 9000u   /* range 4, 500 uF */
#define ACQ_CAP_MIN_PTS   3
#define ACQ_CAP_FIT_MAX_V 0.60      /* above, the rise flattens toward AD1's clip (on our meter,
                                       1000 uF: 0.079 a conversion below, 0.072 at 0.64) */
#define ACQ_CAP_START_V   0.35      /* charge only from below this (meas_cap_volts units) */
#define ACQ_CAP_REST_MAX_MS 20000u  /* ... or after resting this long (50 mF: tau ~12 s) */

typedef enum {
    ACQ_IDLE, ACQ_SETTLE, ACQ_WAIT, ACQ_CJ1, ACQ_CJ2, ACQ_SEC_SETTLE, ACQ_SEC_WAIT
} acq_state_t;

/* Conversions to drop after each configuration: the HY3131's digital filter
 * needs two to settle (on our meter, 2026-11-11: an open input reads
 * ~0.23 V for two conversions after a DC V configure, then ~0). */
#define ACQ_DISCARD 2
/* Resistance needs one more: the ohms source settles (on our meter, the
 * third conversion after configuring was still 1% low). */
#define ACQ_DISCARD_OHM 3

typedef struct {
    double value;       /* SI, or degrees C for temperature */
    double aux;         /* duty cycle % (frequency), cold junction C (type K) */
    double emf_mv;      /* type K: the thermocouple EMF as measured, mV (B26) */
    double rtd_ohm;     /* PT100: the resistance as measured, leads included (B27) */
    double raw;         /* value before the user calibration (B31) */
    bool   overrange;   /* the conversion itself flagged it (temperature, cap) */
    bool   secondary;   /* dual display: this is the secondary function's reading */
} acq_sample_t;

typedef struct {
    meas_fn_t   fn;
    int         range;          /* for MEAS_TEMP: sensor, 0 type K, 1 PT100 */
    acq_state_t st;
    uint32_t    t0;
    uint8_t     flags;          /* latched INTF bits */
    uint8_t     discard;        /* samples still to drop after configuring */
    uint32_t    ct_check_at;    /* capacitance: last check that the counters stopped */
    uint32_t    ct_prev;        /* CTA at that check */
    uint32_t    ct_gate_at;     /* capacitance: when this counter reading began */
    bool        cal_mode;
    bool        fast;           /* hunting: convert fast where acq_fast_ok (B10d) */

    double      rtd_lead_ohm;   /* PT100: lead resistance to subtract (B27) */
    double      cj_offset_c;    /* type K: added to the measured cold junction (B29) */
    const ucal_t *ucal;         /* user calibration, applied to every sample (B31); may be NULL */
    double      t_cj;           /* cold junction, C */
    bool        have_cj;
    uint32_t    cj_at;
    uint32_t    cj_period;      /* ms until the next CJ measurement (B25) */
    uint32_t    cj_count;       /* statistics: CJ measurements made */
    double      cj_v1;

    /* Dual display: alternate with a second function (MEAS_NFUNC = none),
     * one secondary sample after every ACQ_DUAL_PRIMARY primary ones, as
     * stock does ("the multimeter makes the primary and secondary
     * measurements alternately", manual p. 27). */
    meas_fn_t   fn2;
    int         range2;
    uint32_t    since2;         /* primary samples since the last secondary */
    uint8_t     sec_every;      /* primaries between secondaries; 0 = ACQ_DUAL_PRIMARY */

    /* Capacitance 5 / 50 mF: the charge being fitted. */
    bool        cap_charging;
    bool        cap_full;       /* AD1 clipped: the rest of this charge is unusable */
    double      cap_rest_v;     /* the last reading while resting (phase A) */
    uint32_t    cap_t0;         /* the charge's first fitted conversion */
    int         cap_n;
    double      cap_sk, cap_sv, cap_skk, cap_skv;   /* sums for the line fit */
} acq_t;

void acq_init(acq_t *a);
/* Select a function and range; reconfigures on the next poll. */
void acq_select(acq_t *a, meas_fn_t fn, int range);
bool acq_poll(acq_t *a, const platform_t *p, acq_sample_t *out);
/* Hunting for a range (backlog B10d): convert at the fast, low-resolution
 * rate (AFE_AUX_FAST) for functions where that is known to be safe, so each
 * range tried costs ~80 ms instead of ~0.7 s. Turning it off reconfigures
 * at full resolution. Survives acq_select. */
void acq_set_fast(acq_t *a, bool on);
bool acq_fast_ok(meas_fn_t fn);
/* Measure fn2 too, alternately (MEAS_NFUNC to stop). Survives acq_select. */
void acq_set_secondary(acq_t *a, meas_fn_t fn2, int range2);
/* True while a reading depends on timing every conversion (the 5 / 50 mF
 * charge): the app keeps polling between bands of a frame. */
bool acq_timing_critical(const acq_t *a);
/* Primary samples between secondaries (0: ACQ_DUAL_PRIMARY). Power (B21)
 * alternates one for one. */
static inline void acq_set_secondary_every(acq_t *a, uint8_t n) { a->sec_every = n; }
static inline bool acq_cap_slope(meas_fn_t fn, int range) { return fn == MEAS_CAP && range >= 5; }

#endif
