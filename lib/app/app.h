/* The meter application: function selection, ranging, the reading pipeline
 * (filter -> REL -> overload -> autorange -> math), keys, and the screen.
 *
 * Runs unchanged on the meter and in the host simulator: everything it
 * touches is in platform_t. Call app_init() once and app_step() forever; it
 * never blocks.
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

#include "acquire.h"
#include "datalog.h"
#include "meas.h"
#include "meas_temp.h"
#include "menu.h"
#include "platform.h"
#include "sound.h"
#include "scpi.h"
#include "settings.h"
#include "store.h"
#include "ui.h"

#define APP_DISPLAY_MS   200u    /* display update period */
#define APP_NPEND          4u    /* samples read during one frame (~150 ms; 16/s at SPEED F) */
#define APP_CLICK_MS      30u    /* key click (sound.c): stock beeps on every key press */
#define APP_DIODE_TH_MIN    1     /* diode threshold, 0.1 V steps: 0.1 .. 3.0 V */
#define APP_DIODE_TH_MAX   30
#define APP_DIODE_TH_DEF    7
#define APP_RTD_LEAD_MAX_MOHM 5000   /* PT100 lead correction up to 5 ohm (B27) */
#define APP_RTD_LEAD_STEP     50     /* menu step, milliohms */
#define APP_CJ_OFFSET_MAX     50     /* +-5.0 C, in 0.1 C (B29) */
#define APP_UCAL_N            16     /* readings averaged for a user-calibration step (B31) */
typedef enum { UCAL_JOB_IDLE = 0, UCAL_JOB_BUSY, UCAL_JOB_DONE, UCAL_JOB_ERR } ucal_job_state_t;
/* AC above the usable band (B20): under 1% error to ~4 kHz, -3 dB at
 * 8.4 kHz on stock (#288). Flagged when dual display knows the frequency. */
#define APP_AC_BAND_HZ      4000.0
/* Our 5 V AC range rolls off sooner: -3.4% at 1 kHz on the meter
 * (2026-11-13, docs/afe-table.md). */
#define APP_AC_BAND_5V_HZ    500.0
/* 10 A input duty (B19): the spec allows above 7 A for 30 s, then 30 s off. */
#define APP_HIGH_A           7.0
#define APP_HIGH_A_ON_MS     30000u
#define APP_HIGH_A_REST_MS   30000u
#define APP_ALARM_EVERY_MS   1000u
/* Auto-hold (B15): freeze the display on a stable reading and beep. */
#define APP_AH_STABLE_N     5     /* display updates in tolerance (~1 s at 200 ms) */
#define APP_AH_TOL_FRAC     0.001 /* stable: within 0.1% of the reading ... */
#define APP_AH_TOL_COUNTS   10    /* ... or 10 counts of the range, whichever is more */
#define APP_AH_MIN_FRAC     0.02  /* a measurement, not idle leads: 2% of full scale */
#define APP_AH_NEW_FRAC     0.005 /* a new hold needs a reading 0.5% (or 50 counts) away */

#define APP_NBANDS_MAX    64
#define APP_NTHEMES        5
#define APP_DIM_PCT       10     /* backlight while dimmed */
#define APP_NSPAN          3     /* graph time spans */
#define APP_RATE_REC       3     /* platform_t.afe_rate: the 15 ms rate for fast recording (B24) */
#define APP_NREC_INT       9     /* recording intervals (the first, 15 ms, is B24 fast recording) */

#ifndef APP_VERSION
#define APP_VERSION "OpenOWON-0.1-dev"   /* tools tell us from stock by "OpenOWON" (older builds: "XDM1241-OPEN") */
#endif

typedef enum { SCREEN_MEASURE = 0, SCREEN_GRAPH, SCREEN_RECORD } app_screen_t;
typedef enum { MATH_OFF = 0, MATH_STATS, MATH_DBM } app_math_t;

/* Everything that ends up on screen, as text. Tests assert on this; the
 * renderer draws only from this. */
typedef struct {
    char reading[24];
    char unit[16];
    char status[48];          /* "UCAL REC DUAL RMT HOLD A-HOLD REL STATS" is 39 */
    char range[32];
    char func[16];
    char sub[80];             /* secondary line: stats, duty, cold junction, dBm, dual */
    char sub2[128];           /* a second line (power: energy, charge, time) */
    char key_label[UI_NKEYS][12];
    char key_value[UI_NKEYS][12];
    uint8_t key_off;                  /* bit i: soft key i does nothing now (drawn greyed) */
    bool overload;
    int8_t  battery;          /* B47: 0-100, -1 not measured */
    int16_t cursor;           /* record screen: plot point under the cursor, -1 none (B36) */
    bool  bar;                /* bar graph under the reading (B37) */
    bool  bar_bipolar;
    float bar_frac;           /* reading / full scale */
    char menu_value[MI_COUNT][20];   /* Utility menu, when open */
} app_view_t;

typedef struct app {
    const platform_t *p;
    acq_t          acq;
    /* A sample completed while a frame was drawing (acquisition keeps
     * running between bands); handled at the start of the next step. */
    acq_sample_t   pend[APP_NPEND];   /* read between bands of a frame */
    uint8_t        npend;
    meas_filter_t  filt;

    meas_fn_t fn;
    int       range[MEAS_NFUNC];      /* remembered per function */
    bool      autorange;
    bool      period;                 /* frequency: show period */

    /* latest display update */
    bool      have;
    double    value;                  /* SI, before REL */
    double    aux;
    double    tc_emf_mv;              /* type K: last EMF as measured, mV; NaN if none (B26) */
    double    rtd_ohm;                /* PT100: last resistance as measured; NaN if none (B27) */
    uint16_t  rtd_lead_mohm;          /* preference: PT100 lead resistance, milliohms (B27) */
    int8_t    cj_offset_dc;           /* preference: cold-junction offset, 0.1 C steps (B29) */
    bool      rec_roll;               /* preference: when the log is full, overwrite the oldest (B35) */
    bool      terminal;               /* preference: SCPI terminal mode, echo and editing (B45) */
    ucal_t    ucal;                   /* user calibration (B31) */
    struct {
        ucal_job_state_t st;
        bool      gain;               /* gain step (else zero) */
        meas_fn_t fn;
        int       range;
        double    target, sum;
        int       n, err;
    } cal_job;
    int16_t   rec_cursor;             /* B36: plot point under the cursor, -1 = none */
    uint8_t   rec_step;               /* B36: cursor step, 1 or 16 points */
    bool      high_a;                 /* B19: above APP_HIGH_A on the A jack */
    uint32_t  high_a_since;
    uint32_t  high_a_off_at;          /* when it last dropped below (rest timer) */
    bool      high_a_rest;            /* resting after a long high-current run */
    uint32_t  alarm_at;
    bool      overload;
    bool      sample_over;            /* a conversion flagged over-range since the last update */
    uint8_t   rate_set;               /* SPEED last given to the board, +1 (0: never) */
    bool      hunting;                /* autoranging: decide on every sample until a range holds */
    uint8_t   low_run;                /* consecutive samples clearly below the range */
    double    hunt_prev;              /* ohms hunting: a fast sample >= 40 ms old (it must agree) */
    uint32_t  hunt_prev_at;
    int8_t    hunt_from;              /* hunting: the range the last step came from (-1: none) */
    bool      hunt_prev_over, hunt_have;
    bool      show_now;               /* a range has just held: update the display at once */

    bool      hold;
    bool      autohold;               /* preference: auto-hold on (B15) */
    bool      bargraph;               /* preference: bar graph under the reading (B37) */
    bool      ah_holding;             /* the display shows ah_value */
    double    ah_value;               /* held raw reading (before REL), SI */
    int       ah_range;
    double    ah_prev;                /* last reading, for the stability test */
    uint8_t   ah_stable;              /* consecutive readings in tolerance */
    bool      rel;
    double    rel_value;
    app_math_t math;
    int       rref_idx;               /* dBm reference impedance */
    double    st_min, st_max, st_sum;
    uint32_t  st_n;

    uint16_t  cont_threshold;         /* ohms, default 50 (stock prefs +0x14) */
    uint8_t   diode_th_dv;            /* diode beep threshold, 0.1 V steps (B18), default 7 */
    bool      beeper;
    bool      beeping;                /* continuity wants the tone */
    sound_t   snd;                    /* beeper: pitch, volume, style (B57) */
    bool      junction;               /* diode: a forward drop is across the leads */

    uint8_t   backlight_pct;
    uint8_t   dim_idx;                /* menu_dim_s[] */
    bool      dimmed;
    uint32_t  last_key_at;
    uint8_t   baud_idx;               /* menu_baud[] */
    uint32_t  baud_set;               /* last rate given to the board */
    int       theme_idx;
    uint8_t   rate;                   /* 0 S, 1 M, 2 F (recorded; acquisition rate not yet wired) */
    meas_tunit_t temp_unit;
    bool      db_mode;                /* MATH_DBM shows dB relative to db_ref */
    int32_t   db_ref_centi;           /* dB reference, 0.01 dBm */

    /* remote control */
    scpi_t    scpi;
    bool      remote;                 /* front panel locked except RUN/STOP = local */

    /* persistence */
    store_t   store;
    uint8_t   saved[SETTINGS_MAX_LEN];
    size_t    saved_len;
    bool      save_pending;
    uint32_t  changed_at;
    uint32_t  saves;                  /* statistics */

    /* dual display (stock parity: ACV/ACI + frequency, manual p. 27) */
    bool      dual;
    bool      dual_menu;              /* choosing the secondary on the softkeys */
    bool      dual_swapped;           /* the secondary is shown as the main reading */
    meas_fn_t dual_fn;                /* the secondary: MEAS_FREQ, or the same input's AC/DC (B23) */
    bool      sec_over;               /* the secondary overloaded its range */
    bool      sec_have;
    double    sec_value;              /* secondary reading, SI */

    /* B21: power. DC current is the main reading, DC volts the secondary
     * (alternating one for one, its own autoranging), watts shown big, and
     * energy and charge integrated over time (trapezoid on millis()). */
    bool      power;
    int       pw_vrange;              /* the volts range */
    bool      pw_have;                /* pw_w is valid */
    double    pw_w;                   /* latest V x I, W, signed */
    double    pw_wh, pw_ah;           /* since entering power or the last clear, signed */
    uint32_t  pw_ms;                  /* time integrated, ms */
    bool      pw_prev_ok;
    double    pw_prev_w, pw_prev_a;
    uint32_t  pw_prev_at;

    app_screen_t screen;
    bool      menu_open;
    int       menu_sel;
    bool      menu_armed;             /* LOAD DEFAULTS waits for a second press */
    bool      cal_page;               /* B32: the calibration page, from Utility -> CALIBRATION */
    uint8_t   cal_sel;

    /* graph */
    uint8_t   span_idx;               /* app_span_decim[] */
    bool      graph_fixed;            /* scale: the range's full scale, not the trace */
    bool      graph_hist;             /* the graph screen shows a histogram (B38) */
    uint8_t   mini;                   /* B58, measure screen: 0 none, 1 trend, 2 histogram under the reading */
    bool      graph_paused;
    double    acc_sum;                /* averaging readings into one trace point */
    uint16_t  acc_n;

    /* recording (lib/datalog) */
    dlog_t    log;
    uint8_t   rec_int_idx;            /* app_rec_interval_ms[] */
    bool      rec_manual;             /* a point per F4 press instead of a timer */
    uint32_t  rec_next_at;
    bool      rec_fast;               /* B24: recording every 15 ms conversion */
    uint32_t  last_frame_at;          /* B24: frames are paced while rec_fast */
    app_key_t keyq[4];                /* keys scanned between bands, run next step */
    uint8_t   nkeyq;
    trace_t   rec_trace;              /* the session, resampled for the screen */
    uint32_t  rec_trace_count;        /* log.count when rec_trace was built */
    uint32_t  rec_trace_session;

    const theme_t *theme;
    trace_t   trace;

    uint32_t  last_update;
    bool      dirty;
    uint32_t  band_hash[APP_NBANDS_MAX];
    bool      hash_valid;
    uint32_t  bands_sent;             /* statistics: bands pushed to the panel */
    uint32_t  frame_key;              /* fingerprint of what the last frame drew */
    uint32_t  frames_drawn;           /* statistics: frames composed */
    uint32_t  frames_skipped;         /* statistics: frames skipped, nothing changed */
    uint32_t  samples;                /* statistics: samples taken (B11: SPEED on the meter) */

    app_view_t view;
} app_t;

extern const theme_t *const app_themes[APP_NTHEMES];
extern const uint8_t app_span_decim[APP_NSPAN];     /* readings per trace point */
extern const uint32_t app_rec_interval_ms[APP_NREC_INT];

void app_init(app_t *a, const platform_t *p);
void app_step(app_t *a);

/* Exposed for tests and the simulator. */
void app_key(app_t *a, app_key_t k);
/* Play a one-shot beeper sound, if the beeper is on (B57). */
void app_sound(app_t *a, snd_event_t e);
/* B31: start a user-calibration step on the current function and range
 * (manual range); it averages APP_UCAL_N uncorrected readings, then
 * stores. Returns 0, or -1 (function not calibrated), -7 (autoranging),
 * -8 (a step is already running). Progress in a->cal_job. */
int  app_ucal_start(app_t *a, bool gain, double target);
void app_select(app_t *a, meas_fn_t fn);
/* B32: the calibration page's rows: every factory slot, its gain and zero,
 * the user correction on it, and BAD if implausible. */
int  app_cal_rows(void);
bool app_cal_row(const app_t *a, int i, meas_fn_t *fn, int *range,
                 char *label, size_t lcap, char *val, size_t vcap);
/* The board measures fn (platform_t.fn_avail). */
bool app_fn_avail(const app_t *a, meas_fn_t fn);
void app_build_view(app_t *a);
int  app_battery_pct(int mv);     /* B47: 0-100 from the cell's mV, -1 if unknown */
void app_draw(const app_t *a, fb_t *f);    /* draw the view into any window */
void app_render(app_t *a);                 /* draw + push changed bands to the panel */

/* Remote control (app_scpi.c). */
void app_scpi_init(app_t *a);
/* The value MEAS? reports: SI (or C/F/K, or dB/dBm in that math mode). */
bool app_measurement(const app_t *a, double *v);
/* RANGE? reply text, in stock's exact form ("5 V", "5 K\u03a9", "500 uA", "Hz"). */
size_t app_scpi_range_text(char *out, size_t cap, meas_fn_t fn, int range, bool period);
void app_reset_defaults(app_t *a);       /* measurement state (*RST) */
/* Dual display: is fn able to have a secondary (stock: ACV, ACI), and switch
 * it on with frequency as the secondary, or off. */
bool app_dual_possible(meas_fn_t fn);
/* The other half of the same input (B23): DC for AC volts/current and back;
 * MEAS_NFUNC if none. */
meas_fn_t app_dual_partner(meas_fn_t fn);
/* Dual display with fn2 as the secondary: MEAS_FREQ (AC only, as stock) or
 * app_dual_partner(). Returns false if not possible. */
bool app_dual_on_fn(app_t *a, meas_fn_t fn2);
/* The secondary as displayed: value and unit ("50.000", "Hz"). */
void app_dual_sec_text(const app_t *a, char *val, size_t vcap, char *unit, size_t ucap);
void app_dual_on(app_t *a);
void app_dual_off(app_t *a);
/* B21: power and energy on DC current (DUAL -> F3 POWER, SCPI FUNC2 POW).
 * False if the board can't, or the main function isn't DC current. */
bool app_power_on(app_t *a);
void app_power_clear(app_t *a);   /* energy, charge and time back to 0 */
void app_factory_defaults(app_t *a);     /* that, plus every preference */
void app_apply_prefs(app_t *a);          /* push backlight, theme, baud to the board */
/* Recording. The value logged is app_measurement()'s, or DLOG_OVERLOAD. */
int  app_rec_start(app_t *a);           /* 0, or -1 with no log storage */
void app_rec_stop(app_t *a);
int  app_rec_point(app_t *a);           /* one point now (manual mode) */
uint8_t app_rec_tag(const app_t *a);    /* what else decides the logged unit */
/* Time shown across the graph for span index i, in seconds. */
uint32_t app_span_seconds(int i);

#endif
