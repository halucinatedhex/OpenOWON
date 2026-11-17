/* The meter application. See app.h and docs/app.md. */
#include "app.h"
#include "clock.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "readout.h"

const theme_t *const app_themes[APP_NTHEMES] = { &theme_stock, &theme_dark, &theme_amber, &theme_paper,
                                                   &theme_synthwave };   /* append only: stored by index */
const uint8_t app_span_decim[APP_NSPAN] = { 1, 5, 30 };
const uint32_t app_rec_interval_ms[APP_NREC_INT] = { 15, 200, 1000, 2000, 5000, 10000, 30000, 60000, 300000 };
#define REC_FAST_FRAME_MS 500u          /* B24: a frame every 0.5 s, not per 15 ms sample */
#define REC_FAST_MS 15u                  /* B24: every conversion, the AD1 at R22 0x12 (15.4 ms) */

uint32_t app_span_seconds(int i)
{
    return (uint32_t)TRACE_MAX * APP_DISPLAY_MS * app_span_decim[i] / 1000u;
}

/* Power-on ranges: auto, starting mid-ladder. Our choice. */
static const int8_t start_range[MEAS_NFUNC] = {
    [MEAS_DCV] = 2, [MEAS_ACV] = 1, [MEAS_DCI] = 2, [MEAS_ACI] = 2,
    [MEAS_OHM2] = 1, [MEAS_OHM4] = 1, [MEAS_CAP] = 2, [MEAS_FREQ] = 0,
    [MEAS_TEMP] = 0,
};

static void stats_reset(app_t *a)
{
    a->st_min = INFINITY;
    a->st_max = -INFINITY;
    a->st_sum = 0.0;
    a->st_n = 0;
}

static void graph_reset(app_t *a)
{
    trace_init(&a->trace);
    a->acc_sum = 0.0;
    a->acc_n = 0;
}

/* The nearest range the board offers (platform_t.range_avail), r itself if
 * it is available or the board says nothing. */
static int avail(const app_t *a, meas_fn_t fn, int r)
{
    const platform_t *p = a->p;
    if (!p->range_avail || p->range_avail(fn, r)) return r;
    int n = meas_nranges(fn);
    for (int d = 1; d < n; d++) {
        if (r + d < n && p->range_avail(fn, r + d)) return r + d;
        if (r - d >= 0 && p->range_avail(fn, r - d)) return r - d;
    }
    return r;
}

static int dual_range(const app_t *a);
/* B24: fast recording is for the functions on the high-resolution AD1. */
static bool rec_fast_ok(meas_fn_t fn) { return fn == MEAS_DCV || fn == MEAS_DCI || fn == MEAS_OHM2; }
static bool cal_page_key(app_t *a, app_key_t k);

/* Autoranging is looking for a range: convert fast meanwhile (B10d). */
static void set_hunting(app_t *a, bool on)
{
    a->hunting = on;
    a->hunt_have = false;
    if (!on) a->hunt_from = -1;
    acq_set_fast(&a->acq, on);
}

static void restart(app_t *a)
{
    a->range[a->fn] = avail(a, a->fn, a->range[a->fn]);
    acq_select(&a->acq, a->fn, a->range[a->fn]);
    if (a->dual && a->power) acq_set_secondary(&a->acq, MEAS_DCV, a->pw_vrange);           /* B21 */
    else if (a->dual && a->dual_fn != MEAS_FREQ) acq_set_secondary(&a->acq, a->dual_fn, dual_range(a));   /* B23 */
    meas_filter_reset(&a->filt);
    a->have = false;
    a->tc_emf_mv = NAN;                  /* B26: none until a type-K sample */
    a->rtd_ohm = NAN;
    a->acq.rtd_lead_ohm = a->rtd_lead_mohm / 1000.0;
    set_hunting(a, false);               /* a ranging restart sets it again */
    a->low_run = 0;
    a->dirty = true;
}

bool app_fn_avail(const app_t *a, meas_fn_t fn)
{
    return !a->p->fn_avail || a->p->fn_avail(fn);
}

void app_select(app_t *a, meas_fn_t fn)
{
    if (!app_fn_avail(a, fn)) return;           /* not on this board: stay put */
    if (fn != a->fn) {
        if (a->log.active) app_rec_stop(a);     /* a session holds one function */
        app_dual_off(a);                        /* a pair belongs to one function */
        a->fn = fn;
        a->rel = false;
        a->ah_holding = false;                  /* a held reading belongs to one function */
        if (a->math == MATH_DBM && fn != MEAS_DCV && fn != MEAS_ACV) a->math = MATH_OFF;
        stats_reset(a);
        graph_reset(a);
    }
    restart(a);
}

/* Preferences: what the Utility menu edits. */
static void prefs_defaults(app_t *a)
{
    a->cont_threshold = 50;
    a->diode_th_dv = APP_DIODE_TH_DEF;
    a->beeper = true;
    sound_init(&a->snd);                /* pitch 3 kHz, full volume, classic */
    a->rref_idx = 11;                   /* 600 ohm */
    a->backlight_pct = 100;
    a->dim_idx = 0;
    a->theme_idx = 0;
    a->rate = 0;
    a->temp_unit = MEAS_DEG_C;
    a->baud_idx = MENU_BAUD_DEFAULT;
    a->span_idx = 0;
    a->rec_int_idx = 2;                 /* 1 s */
    a->rec_manual = false;
    a->rec_cursor = -1;
    a->rec_step = 1;
}

void app_apply_prefs(app_t *a)
{
    const platform_t *p = a->p;
    a->acq.rtd_lead_ohm = a->rtd_lead_mohm / 1000.0;   /* B27 */
    a->scpi.terminal = a->terminal;                     /* B45 */
    if (a->acq.cj_offset_c != a->cj_offset_dc / 10.0) {
        a->acq.cj_offset_c = a->cj_offset_dc / 10.0;    /* B29 */
        a->acq.have_cj = false;                          /* re-measure with it */
    }
    if (a->theme != app_themes[a->theme_idx]) {
        a->theme = app_themes[a->theme_idx];
        a->hash_valid = false;
    }
    if (p->backlight) p->backlight(a->dimmed && a->backlight_pct > APP_DIM_PCT ? APP_DIM_PCT
                                                                             : a->backlight_pct);
    if (p->afe_rate && a->rate_set != a->rate + 1) {   /* B11: only on change */
        a->rate_set = (uint8_t)(a->rate + 1);
        p->afe_rate(a->rate);
        restart(a);                                       /* reconfigure at the new speed */
    }
    if (p->serial_baud && a->baud_set != menu_baud[a->baud_idx]) {   /* only on change */
        a->baud_set = menu_baud[a->baud_idx];
        p->serial_baud(a->baud_set);
    }
    a->dirty = true;
}

void app_init(app_t *a, const platform_t *p)
{
    memset(a, 0, sizeof *a);
    a->p = p;
    for (int f = 0; f < MEAS_NFUNC; f++) a->range[f] = start_range[f];
    a->autorange = true;
    prefs_defaults(a);
    a->fn = MEAS_DCV;

    /* Power-on settings, if this board persists them. */
    if (p->store) {
        uint8_t buf[SETTINGS_MAX_LEN];
        uint16_t ver = 0;
        store_open(&a->store, p->store, STORE_PAGE0, STORE_PAGE1);
        int n = store_load(&a->store, buf, sizeof buf, &ver);
        if (n > 0) settings_apply(a, buf, (size_t)n, ver);
    }
    if (!app_fn_avail(a, a->fn)) a->fn = MEAS_DCV;   /* saved on a board that had it */
    a->saved_len = settings_pack(a, a->saved, sizeof a->saved);
    stats_reset(a);
    graph_reset(a);
    if (p->log_flash) dlog_open(&a->log, p->log_flash, DLOG_BASE, DLOG_NPAGES);
    app_scpi_init(a);
    acq_init(&a->acq);
    ucal_open(&a->ucal, p->ucal_flash);          /* B31 */
    a->acq.ucal = &a->ucal;
    meas_filter_reset(&a->filt);
    hy3131_init(p->afe_bus);
    lcd_init(p->lcd);
    a->last_key_at = p->millis();
    app_apply_prefs(a);
    restart(a);
}

/* ---- keys ------------------------------------------------------------------ */

static void set_range(app_t *a, int r)
{
    int lo, hi;                                  /* current: stay on the selected jack */
    readout_range_span(a->fn, a->range[a->fn], &lo, &hi);
    if (r < lo || r > hi) return;
    r = avail(a, a->fn, r);
    if (r == a->range[a->fn]) return;
    a->range[a->fn] = r;
    restart(a);
}

/* Does the board offer the A jack for this current function? (On our
 * meter AC current has only the mA jack's lower ranges, 2026-11-16.) */
static bool a_jack_avail(const app_t *a)
{
    return !a->p->range_avail || a->p->range_avail(a->fn, READOUT_CURRENT_A_FIRST);
}

/* Softkeys on the graph screen: F1 span, F2 scale, F5 pause. F3 (view),
 * F4 (clear) and F6 (theme) mean the same on both screens. */
static bool graph_key(app_t *a, app_key_t k)
{
    switch (k) {
    case KEY_F1: a->span_idx = (uint8_t)((a->span_idx + 1) % APP_NSPAN); graph_reset(a); return true;
    case KEY_F2: a->graph_fixed = !a->graph_fixed; return true;
    case KEY_F5: a->graph_paused = !a->graph_paused; return true;
    default:     return false;
    }
}

/* Softkeys on the record screen: F1 start/stop, F2 mode, F3 interval,
 * F4 take a point (manual), F6 back. Settings are fixed while recording. */
static bool record_key(app_t *a, app_key_t k)
{
    switch (k) {
    case KEY_F1:
        if (a->log.active) app_rec_stop(a);
        else app_rec_start(a);
        a->rec_cursor = -1;
        return true;
    case KEY_F2: if (!a->log.active) a->rec_manual = !a->rec_manual; return true;
    case KEY_F3:
        if (!a->log.active) {
            a->rec_int_idx = (uint8_t)((a->rec_int_idx + 1) % APP_NREC_INT);
            if (app_rec_interval_ms[a->rec_int_idx] == REC_FAST_MS && (!rec_fast_ok(a->fn) || a->power))
                a->rec_int_idx = (uint8_t)((a->rec_int_idx + 1) % APP_NREC_INT);   /* not for this function */
        }
        return true;
    case KEY_F4:
        if (a->rec_manual) app_rec_point(a);
        else if (!a->log.active) a->rec_roll = !a->rec_roll;   /* WHEN FULL: STOP / ROLL */
        return true;
    case KEY_F5: case KEY_RECORD: a->screen = SCREEN_MEASURE; a->rec_cursor = -1; return true;   /* EXIT */
    /* B36: browse the recording. UP/DOWN move a cursor along the plot (they
     * would otherwise change range and end the recording); RANGE switches
     * between fine and coarse steps. */
    case KEY_UP: case KEY_DOWN: {
        int n = a->rec_trace.n;
        if (n < 1) return true;
        int step = a->rec_step ? a->rec_step : 1;
        int c = a->rec_cursor < 0 ? n - 1 : a->rec_cursor + (k == KEY_UP ? step : -step);
        a->rec_cursor = (int16_t)(c < 0 ? 0 : c > n - 1 ? n - 1 : c);
        return true;
    }
    case KEY_LEFT: a->rec_step = a->rec_step == 16 ? 1 : 16; return true;
    default:     return false;
    }
}

/* ---- dual display --------------------------------------------------------------- */

meas_fn_t app_dual_partner(meas_fn_t fn)
{
    switch (fn) {
    case MEAS_ACV: return MEAS_DCV;
    case MEAS_DCV: return MEAS_ACV;
    case MEAS_ACI: return MEAS_DCI;
    case MEAS_DCI: return MEAS_ACI;
    default:       return MEAS_NFUNC;
    }
}

bool app_dual_possible(meas_fn_t fn)
{
    return app_dual_partner(fn) < MEAS_NFUNC;
}

/* The secondary's range: the same full scale on the other side. AC volts'
 * ranges (500 mV .. 750 V) sit one index above DC volts' (50 mV .. 1000 V);
 * current's indices match. */
static int dual_range(const app_t *a)
{
    int r = a->range[a->fn], n2 = meas_nranges(a->dual_fn);
    switch (a->dual_fn) {
    case MEAS_DCV: r = r + 1; break;
    case MEAS_ACV: r = r - 1; break;
    case MEAS_FREQ: return 0;
    default: break;
    }
    return r < 0 ? 0 : r >= n2 ? n2 - 1 : r;
}

bool app_dual_on_fn(app_t *a, meas_fn_t fn2)
{
    bool ac = a->fn == MEAS_ACV || a->fn == MEAS_ACI;
    if (!(fn2 == MEAS_FREQ ? ac : fn2 == app_dual_partner(a->fn) && fn2 < MEAS_NFUNC)) return false;
    if (a->p->fn_avail && !a->p->fn_avail(fn2)) return false;   /* e.g. AC current, not offered */
    a->rel = false;                       /* manual: dual display turns math off */
    a->math = MATH_OFF;
    stats_reset(a);
    a->dual = true;
    a->dual_fn = fn2;
    a->dual_menu = false;
    a->dual_swapped = false;
    a->sec_have = a->sec_over = false;
    acq_set_secondary(&a->acq, fn2, dual_range(a));
    a->dirty = true;
    return true;
}

void app_dual_on(app_t *a) { (void)app_dual_on_fn(a, MEAS_FREQ); }

void app_dual_sec_text(const app_t *a, char *val, size_t vcap, char *unit, size_t ucap)
{
    if (a->dual_fn == MEAS_FREQ) {
        int fc = meas_freq_code(a->sec_value, false);
        if (a->sec_have) readout_value(val, vcap, MEAS_FREQ, fc, a->sec_value, false);
        else snprintf(val, vcap, "-----");
        snprintf(unit, ucap, "%s", readout_unit(MEAS_FREQ, fc, false));
        return;
    }
    int r2 = a->acq.range2;
    if (!a->sec_have) snprintf(val, vcap, "-----");
    else readout_value(val, vcap, a->dual_fn, r2, a->sec_value, a->sec_over);
    snprintf(unit, ucap, "%s", readout_unit(a->dual_fn, r2, false));
}

void app_dual_off(app_t *a)
{
    if (a->dual) acq_set_secondary(&a->acq, MEAS_NFUNC, 0);
    acq_set_secondary_every(&a->acq, 0);
    a->dual = a->dual_menu = a->dual_swapped = a->sec_have = false;
    a->power = false;
    a->dirty = true;
}

/* ---- power and energy (B21) ------------------------------------------------- */

#define PW_V_START 3                      /* 50 V: autoranging comes down from here */

static void power_fit_vrange(app_t *a);

static bool power_possible(const app_t *a)
{
    return a->fn == MEAS_DCI && a->p->vi_avail && a->p->vi_avail(a->range[MEAS_DCI], -1);
}

void app_power_clear(app_t *a)
{
    a->pw_wh = a->pw_ah = 0.0;
    a->pw_ms = 0;
    a->pw_prev_ok = false;
    a->dirty = true;
}

bool app_power_on(app_t *a)
{
    if (!power_possible(a)) return false;
    if (!app_dual_on_fn(a, MEAS_DCV)) {   /* the plumbing of DC/AC dual, with volts */
        a->rel = false;
        a->math = MATH_OFF;
        stats_reset(a);
        a->dual = true;
        a->dual_fn = MEAS_DCV;
        a->dual_menu = a->dual_swapped = false;
        a->sec_have = a->sec_over = false;
    }
    a->power = true;
    a->pw_have = false;
    a->pw_vrange = PW_V_START;
    acq_set_secondary_every(&a->acq, 1);
    acq_set_secondary(&a->acq, MEAS_DCV, a->pw_vrange);
    power_fit_vrange(a);
    app_power_clear(a);
    return true;
}

/* Keep the volts on a range the board can alternate with the current range
 * (on our meter the 50 / 500 mA ranges pair with 50 / 500 mV only: any
 * other volts read moves PA8, which opens the current path). Nearest first. */
static void power_fit_vrange(app_t *a)
{
    const platform_t *p = a->p;
    int ir = a->range[MEAS_DCI], vr = a->pw_vrange;
    if (!p->vi_avail || p->vi_avail(ir, vr)) return;
    for (int d = 1; d < 6; d++) {
        int c[2] = { vr + d, vr - d };
        for (int k = 0; k < 2; k++) {
            if (c[k] < 0 || c[k] > 5 || !p->vi_avail(ir, c[k])) continue;
            a->pw_vrange = c[k];
            a->sec_have = false;
            a->pw_have = false;
            acq_set_secondary(&a->acq, MEAS_DCV, c[k]);
            return;
        }
    }
}

/* A volts sample: range it on its own (a range change discards it), and
 * keep it for the next current sample. */
static void power_volts(app_t *a, const acq_sample_t *s)
{
    int r = a->pw_vrange;
    bool over = s->overrange || readout_overload(MEAS_DCV, r, s->value);
    int nr = readout_autorange(MEAS_DCV, r, s->value, over);
    if (a->p->vi_avail && !a->p->vi_avail(a->range[MEAS_DCI], nr)) nr = r;   /* a pair the board can't alternate */
    if (nr != r) {
        a->pw_vrange = nr;
        acq_set_secondary(&a->acq, MEAS_DCV, nr);
        return;
    }
    a->sec_value = s->value;
    a->sec_over = over || (a->p->vi_avail && !a->p->vi_avail(a->range[MEAS_DCI], r));
    a->sec_have = true;
    a->dirty = true;
}

/* A current sample with a volts reading in hand: power, and the integrals.
 * The trapezoid runs from the previous pair; a gap (ranging) is bridged. */
static void power_current(app_t *a, double amps, bool over)
{
    if (over || !a->sec_have || a->sec_over) { a->pw_have = false; return; }
    uint32_t now = a->p->millis();
    double w = a->sec_value * amps;
    a->pw_w = w;
    a->pw_have = true;
    if (a->pw_prev_ok) {
        uint32_t dt = now - a->pw_prev_at;
        double h = (double)dt / 3600000.0;
        a->pw_wh += 0.5 * (w + a->pw_prev_w) * h;
        a->pw_ah += 0.5 * (amps + a->pw_prev_a) * h;
        a->pw_ms += dt;
    }
    a->pw_prev_ok = true;
    a->pw_prev_w = w;
    a->pw_prev_a = amps;
    a->pw_prev_at = now;
    a->dirty = true;
}

/* DUAL opens the choice of secondary (frequency is the only valid one, as
 * stock); once on, DUAL swaps main and secondary and EXIT (F5, the last of
 * our panel's five softkeys) ends it. */
static bool dual_key(app_t *a, app_key_t k)
{
    if (a->dual_menu) {
        a->dual_menu = false;
        bool ac = a->fn == MEAS_ACV || a->fn == MEAS_ACI;
        if (k == KEY_F1 && ac) { app_dual_on_fn(a, MEAS_FREQ); return true; }
        if (k == KEY_F2) { app_dual_on_fn(a, app_dual_partner(a->fn)); return true; }   /* B23 */
        if (k == KEY_F3 && power_possible(a)) { app_power_on(a); return true; }        /* B21 */
        if (k == KEY_F5 || k == KEY_DUAL) return true;   /* CANCEL */
        return false;                    /* any other key: close the list and act */
    }
    if (k == KEY_DUAL) {
        if (a->dual && !a->power) a->dual_swapped = !a->dual_swapped;
        else if (!a->dual && app_dual_possible(a->fn)) a->dual_menu = true;
        return true;                     /* elsewhere DUAL does nothing, as stock */
    }
    if (!a->dual) return false;
    switch (k) {
    case KEY_F5:   app_dual_off(a); return true;
    case KEY_F1:   case KEY_MATH:        /* REL or math on: dual off, then act */
        app_dual_off(a);
        return false;
    default:       return false;
    }
}

void app_key(app_t *a, app_key_t k)
{
    a->dirty = true;
    if (a->menu_open && a->cal_page && cal_page_key(a, k)) return;   /* B32 */
    if (a->menu_open && menu_key(a, k)) return;
    if (a->screen == SCREEN_MEASURE && dual_key(a, k)) return;
    if (a->screen == SCREEN_GRAPH && graph_key(a, k)) return;
    if (a->screen == SCREEN_RECORD && record_key(a, k)) return;
    switch (k) {
    case KEY_V:    app_select(a, a->fn == MEAS_DCV ? MEAS_ACV : MEAS_DCV); break;
    case KEY_I:    app_select(a, a->fn == MEAS_DCI ? MEAS_ACI : MEAS_DCI); break;
    case KEY_OHM:  /* manual: resistance, continuity, diode share one key */
        app_select(a, (a->fn == MEAS_OHM2 || a->fn == MEAS_OHM4) ? MEAS_CONT
                    : a->fn == MEAS_CONT ? MEAS_DIODE : MEAS_OHM2);
        break;
    case KEY_CAP:  app_select(a, MEAS_CAP); break;
    case KEY_FREQ:
        if (a->fn == MEAS_FREQ) a->period = !a->period;
        app_select(a, MEAS_FREQ);
        break;
    case KEY_TEMP:
        if (a->fn == MEAS_TEMP) a->range[MEAS_TEMP] = (a->range[MEAS_TEMP] + 1) % 3;   /* K, PT100, PT1000 */
        app_select(a, MEAS_TEMP);
        break;

    case KEY_UP:   a->autorange = false; set_range(a, a->range[a->fn] + 1); break;
    case KEY_DOWN: a->autorange = false; set_range(a, a->range[a->fn] - 1); break;
    case KEY_LEFT: /* provisional: the manual's auto/manual key */
    case KEY_F2:   a->autorange = !a->autorange; break;

    case KEY_RUNSTOP:                    /* Run -> Hold -> Auto-hold (B15) -> Run */
        if (!a->hold && !a->autohold) a->hold = true;
        else if (a->hold) { a->hold = false; a->autohold = true; a->ah_holding = false; }
        else { a->autohold = false; a->ah_holding = false; }
        break;
    case KEY_F1:
        if (a->rel) a->rel = false;
        else if (a->have && !a->overload) { a->rel = true; a->rel_value = a->value; }
        stats_reset(a);
        break;
    case KEY_F3:     /* VIEW: NUM -> NUM+G -> NUM+H (B58) -> GRAPH -> HISTO (B38) -> NUM */
        if (a->screen == SCREEN_MEASURE && a->mini < 2) a->mini++;
        else if (a->screen == SCREEN_MEASURE) { a->mini = 0; a->screen = SCREEN_GRAPH; a->graph_hist = false; }
        else if (!a->graph_hist) a->graph_hist = true;
        else { a->screen = SCREEN_MEASURE; a->graph_hist = false; a->mini = 0; }
        break;
    case KEY_F4: stats_reset(a); graph_reset(a); if (a->power) app_power_clear(a); break;
    case KEY_F5:
        if (a->fn == MEAS_FREQ) a->period = !a->period;   /* no 4-wire on this meter */
        else if (a->fn == MEAS_TEMP) { a->range[MEAS_TEMP] = (a->range[MEAS_TEMP] + 1) % 3; restart(a); }
        else if ((a->fn == MEAS_DCI || a->fn == MEAS_ACI) && a_jack_avail(a)) {   /* mA jack <-> A jack */
            a->range[a->fn] = a->range[a->fn] >= READOUT_CURRENT_A_FIRST ? READOUT_CURRENT_A_FIRST - 1
                                                                       : READOUT_CURRENT_A_FIRST;
            restart(a);
        }
        else if (a->math == MATH_DBM) a->rref_idx = (a->rref_idx + 1) % MEAS_NRREF;
        break;
    case KEY_MATH:
        a->math = (app_math_t)((a->math + 1) % 3);
        if (a->math == MATH_DBM && a->fn != MEAS_DCV && a->fn != MEAS_ACV) a->math = MATH_OFF;
        stats_reset(a);
        break;
    case KEY_RECORD: a->screen = SCREEN_RECORD; break;
    case KEY_UTILITY:
        a->menu_open = true;
        a->menu_armed = false;
        a->hash_valid = false;
        break;
    default:
        break;
    }
}

/* ---- the reading pipeline ---------------------------------------------- */

static double displayed(const app_t *a)
{
    return a->rel ? a->value - a->rel_value : a->value;
}

bool app_measurement(const app_t *a, double *out)
{
    if (!a->have || a->overload) return false;
    int r = a->range[a->fn];
    double v = displayed(a);
    if (a->fn == MEAS_ACV || a->fn == MEAS_ACI || a->fn == MEAS_CAP)
        v = meas_suppress_small(a->fn, r, v, false, a->rel);
    else if (a->fn == MEAS_TEMP)
        v = meas_temp_unit(v, a->temp_unit);
    else if (a->fn == MEAS_FREQ && a->period)
        v = v != 0.0 ? 1.0 / v : 0.0;
    if (a->math == MATH_DBM && (a->fn == MEAS_DCV || a->fn == MEAS_ACV)) {
        double rr = meas_dbm_rref[a->rref_idx];
        v = a->db_mode ? meas_db(v, rr, a->db_ref_centi) : meas_dbm(v, rr);
    }
    *out = v;
    return true;
}

void app_reset_defaults(app_t *a)
{
    for (int f = 0; f < MEAS_NFUNC; f++) a->range[f] = start_range[f];
    a->autorange = true;
    a->period = false;
    a->rel = false;
    a->hold = false;
    a->math = MATH_OFF;
    a->db_mode = false;
    app_dual_off(a);
    a->screen = SCREEN_MEASURE;
    a->graph_fixed = false;
    a->graph_paused = false;
    a->fn = MEAS_DCV;
    stats_reset(a);
    graph_reset(a);
    restart(a);
}

void app_factory_defaults(app_t *a)
{
    prefs_defaults(a);
    app_reset_defaults(a);
    app_apply_prefs(a);
}

/* ---- recording ---------------------------------------------------------------- */

uint8_t app_rec_tag(const app_t *a)
{
    bool dbm = a->math == MATH_DBM && (a->fn == MEAS_DCV || a->fn == MEAS_ACV);
    return (uint8_t)((a->fn == MEAS_TEMP ? a->temp_unit : 0) |
                     (a->fn == MEAS_FREQ && a->period) << 2 |
                     dbm << 3 | (dbm && a->db_mode) << 4 |
                     (a->fn == MEAS_DCI && a->power) << 5);     /* B21: watts */
}

int app_rec_start(app_t *a)
{
    if (!a->log.io) return -1;
    uint32_t iv = a->rec_manual ? 0 : app_rec_interval_ms[a->rec_int_idx];
    if (iv == REC_FAST_MS && (!rec_fast_ok(a->fn) || a->power))   /* power alternates: not every conversion */
        iv = app_rec_interval_ms[1];                                  /* 200 ms: every reading */
    uint32_t now = a->p->clock_now ? a->p->clock_now() : 0;      /* B33: stamp the session */
    int r = dlog_start_at(&a->log, (uint8_t)a->fn, app_rec_tag(a), iv, a->rec_roll && !a->rec_manual, now);
    if (r) return -1;
    a->rec_next_at = a->p->millis();
    if (iv == REC_FAST_MS && a->p->afe_rate) {   /* B24: the AD1 at 15 ms, every conversion logged */
        a->rec_fast = true;
        a->p->afe_rate(APP_RATE_REC);
        a->rate_set = 0;                         /* SPEED is put back when it stops */
        restart(a);
    }
    a->dirty = true;
    return 0;
}

void app_rec_stop(app_t *a)
{
    dlog_stop(&a->log);
    if (a->rec_fast) {
        a->rec_fast = false;
        app_apply_prefs(a);                      /* the SPEED setting's rate again */
    }
    a->dirty = true;
}

int app_rec_point(app_t *a)
{
    if (!a->log.active) return -1;
    /* What the session holds must stay one quantity in one unit. */
    if (a->log.fn != (uint8_t)a->fn || a->log.tag != app_rec_tag(a)) { app_rec_stop(a); return -1; }
    double v;
    float f = app_measurement(a, &v) ? (float)v : DLOG_OVERLOAD;
    if (a->power) f = a->pw_have ? (float)a->pw_w : DLOG_OVERLOAD;   /* B21: the watts (a range change: a gap) */
    a->dirty = true;
    return dlog_append(&a->log, f);
}

/* Autoranging on single samples (backlog B10). In steady state only a clear
 * case acts at once - an overload, or two readings in a row below 80% of the
 * next range down - and everything marginal waits for the averaged display update, so
 * noise near a boundary cannot make the range flap (down needs < 90% of the
 * lower range, up needs > 100% of it). After a range change,
 * every sample decides ("hunting") until one says stay, and that reading is
 * shown immediately instead of at the next 200 ms tick. Returns true if the
 * range changed (the sample belongs to the old range: drop it). */
/* Autoranging moves to range nr and hunts from there, remembering where it
 * came from (an overload after a step down goes back up one). */
static void range_step(app_t *a, int nr)
{
    int from = a->range[a->fn];
    a->range[a->fn] = nr;
    restart(a);
    set_hunting(a, true);
    a->hunt_from = (int8_t)from;
}

/* ---- B32: calibration page ------------------------------------------------ */

int app_cal_rows(void)
{
    int n = 0;
    for (int i = 0; i < MEAS_CAL_NOWNERS; i++) n += meas_cal_nslots(meas_cal_owners[i]);
    return n;
}

bool app_cal_row(const app_t *a, int i, meas_fn_t *fn, int *range,
                 char *label, size_t lcap, char *val, size_t vcap)
{
    for (int k = 0; k < MEAS_CAL_NOWNERS; k++) {
        meas_fn_t f = meas_cal_owners[k];
        int n = meas_cal_nslots(f);
        if (i >= n) { i -= n; continue; }
        const meas_cal_t *c = a->p->cal;
        char rl[16];
        readout_range_label(rl, sizeof rl, f, i);
        snprintf(label, lcap, "%s %s", readout_fn_name(f), rl);
        double g = c ? (c->coef[f][i] / 1e6 - 1.0) * 100.0 : 0.0;
        int n0 = snprintf(val, vcap, "G %+.2f%%  Z %ld", g, c ? (long)c->zero[f][i] : 0L);
        int32_t ug = i < UCAL_NRANGE ? a->ucal.slot[f][i].g_ppm : 0;
        int32_t uo = i < UCAL_NRANGE ? a->ucal.slot[f][i].o_ppm : 0;
        if ((ug || uo) && n0 > 0 && (size_t)n0 < vcap)
            n0 += snprintf(val + n0, vcap - (size_t)n0, "  U %+.3f%%", ug / 1e4);
        if (c && !meas_cal_slot_ok(c, f, i) && n0 > 0 && (size_t)n0 < vcap)
            snprintf(val + n0, vcap - (size_t)n0, "  BAD");
        if (fn) *fn = f;
        if (range) *range = i;
        return true;
    }
    return false;
}

static bool cal_page_key(app_t *a, app_key_t k)
{
    int n = app_cal_rows();
    a->dirty = true;
    switch (k) {
    case KEY_UP: case KEY_F1:   a->cal_sel = (uint8_t)((a->cal_sel + n - 1) % n); return true;
    case KEY_DOWN: case KEY_F2: a->cal_sel = (uint8_t)((a->cal_sel + 1) % n); return true;
    case KEY_LEFT: case KEY_F5: case KEY_UTILITY:                 /* back to the menu */
        a->cal_page = false; a->hash_valid = false; return true;
    default: return true;                                          /* nothing else here */
    }
}

static bool range_on_sample(app_t *a, const acq_sample_t *s)
{
    if (!a->autorange || !readout_autoranges(a->fn)) { if (a->hunting) set_hunting(a, false); return false; }
    int r = a->range[a->fn];
    bool over = s->overrange || readout_overload(a->fn, r, s->value);
    /* Ohms settles over a time on its high ranges (on the meter, 2026-11-14:
     * the reading decays from above, tau ~20 ms on 500 kohm). While
     * hunting, act only on a fast sample that agrees with one taken at
     * least 40 ms earlier, within 0.5% plus 0.2% of full scale. A time,
     * not a count of conversions: consecutive 4 ms samples agree while a
     * slow decay is still under way. */
    if (a->hunting && a->fn == MEAS_OHM2) {
        uint32_t now = a->p->millis();
        double fs = readout_fullscale(a->fn, r);
        if (!a->hunt_have) {
            a->hunt_prev = s->value; a->hunt_prev_over = over; a->hunt_prev_at = now; a->hunt_have = true;
            return true;                  /* the first: wait */
        }
        if (now - a->hunt_prev_at < 40u) return true;
        bool agree = over ? a->hunt_prev_over
                   : !a->hunt_prev_over && fabs(s->value - a->hunt_prev) <= 5e-3 * fabs(s->value) + 2e-3 * fs;
        a->hunt_prev = s->value; a->hunt_prev_over = over; a->hunt_prev_at = now;
        if (!agree) return true;          /* still settling: drop it, wait for the next */
    }
    int nr = avail(a, a->fn, readout_autorange(a->fn, r, s->value, over));
    /* An overload just after a step down means the step overshot (on the
     * meter, touching the probes to 10 kohm: a brief low reading on 50 Mohm
     * sent the hunt to 5 kohm). Go back up one range, not to the top: the
     * top range is the slowest to settle. A real overload still climbs. */
    if (over && a->hunting && a->hunt_from > r) nr = avail(a, a->fn, r + 1);
    bool low = nr < r && fabs(s->value) < 0.8 * readout_fullscale(a->fn, r - 1);
    a->low_run = low ? (uint8_t)(a->low_run < 255 ? a->low_run + 1 : 255) : 0;
    /* 5 / 50 mF: a reading takes 3-5.5 s, and 0 means the capacitor clipped
     * AD1 before the fit had its points: too small for this range, now. */
    bool clear = over || a->low_run >= 2 || (acq_cap_slope(a->fn, r) && s->value == 0.0);
    if (nr != r && (a->hunting || clear)) {
        range_step(a, nr);
        return true;
    }
    if (a->hunting) {                    /* this range holds: measure at full resolution */
        set_hunting(a, false);
        a->show_now = true;
    }
    return false;
}


int app_ucal_start(app_t *a, bool gain, double target)
{
    if (a->cal_job.st == UCAL_JOB_BUSY) return -8;
    if (!ucal_applies(a->fn)) return -1;
    if (a->autorange && readout_autoranges(a->fn)) return -7;
    a->cal_job.st = UCAL_JOB_BUSY;
    a->cal_job.gain = gain;
    a->cal_job.fn = a->fn;
    a->cal_job.range = a->range[a->fn];
    a->cal_job.target = target;
    a->cal_job.sum = 0;
    a->cal_job.n = 0;
    a->cal_job.err = 0;
    meas_filter_reset(&a->filt);
    return 0;
}

/* B31: one user-calibration step, fed uncorrected readings. Moving off the
 * function or range abandons it (-5); a store failure is -6. */
static void cal_job_sample(app_t *a, const acq_sample_t *s)
{
    if (a->cal_job.st != UCAL_JOB_BUSY || s->secondary) return;
    if (a->fn != a->cal_job.fn || a->range[a->fn] != a->cal_job.range) {
        a->cal_job.st = UCAL_JOB_ERR;
        a->cal_job.err = -5;
        return;
    }
    a->cal_job.sum += s->raw;
    if (++a->cal_job.n < APP_UCAL_N) return;
    double raw = a->cal_job.sum / a->cal_job.n;
    int r = a->cal_job.gain ? ucal_gain(&a->ucal, a->cal_job.fn, a->cal_job.range, raw, a->cal_job.target)
                            : ucal_zero(&a->ucal, a->cal_job.fn, a->cal_job.range, raw);
    if (r == 0 && ucal_save(&a->ucal)) r = -6;
    a->cal_job.st = r ? UCAL_JOB_ERR : UCAL_JOB_DONE;
    a->cal_job.err = r;
    a->dirty = true;
    app_sound(a, r ? SND_ALARM : SND_HOLD);
}

static void sample(app_t *a, const acq_sample_t *s)
{
    a->samples++;
    cal_job_sample(a, s);
    if (s->secondary && a->power) { power_volts(a, s); return; }   /* B21 */
    if (s->secondary) {                   /* dual display: the second reading */
        a->sec_value = s->value;
        a->sec_over = a->dual_fn != MEAS_FREQ &&
                      (s->overrange || readout_overload(a->dual_fn, a->acq.range2, s->value));
        a->sec_have = true;
        a->dirty = true;
        return;
    }
    acq_sample_t fixed;
    if (a->power && a->p->vi_leak && a->sec_have && !a->sec_over) {   /* B21: the V jack's share */
        fixed = *s;
        fixed.value -= a->p->vi_leak(a->range[MEAS_DCI], a->sec_value);
        s = &fixed;
    }
    if (range_on_sample(a, s)) {
        if (a->power) { a->pw_have = false; power_fit_vrange(a); }
        return;
    }
    if (a->power) power_fit_vrange(a);        /* a manual range change */
    if (a->power) power_current(a, s->value, s->overrange || readout_overload(MEAS_DCI, a->range[MEAS_DCI], s->value));
    if (a->rec_fast && a->log.active && !a->hunting) {   /* B24: every conversion */
        if (a->log.fn != (uint8_t)a->fn || a->log.tag != app_rec_tag(a)) app_rec_stop(a);
        else {
            bool over = s->overrange || readout_overload(a->fn, a->range[a->fn], s->value);
            if (dlog_append(&a->log, over ? DLOG_OVERLOAD : (float)s->value)) app_rec_stop(a);
            a->dirty = true;
        }
    }
    double step = meas_step_threshold(a->fn, a->range[a->fn]);
    if (a->fn == MEAS_TEMP || a->fn == MEAS_FREQ) step = INFINITY;
    bool was_pinned = a->filt.pinned;
    meas_filter_push(&a->filt, s->value, step);
    /* A step bigger than 10% of full scale restarts the average (stock); show
     * it now rather than at the next tick, so big changes appear within one
     * conversion. */
    if (a->filt.pinned && !was_pinned) a->show_now = true;
    a->aux = s->aux;
    a->tc_emf_mv = s->emf_mv;
    a->rtd_ohm = s->rtd_ohm;

    /* Beeper follows each sample, not the display rate. */
    bool want = false;
    if (a->beeper && a->fn == MEAS_CONT) want = s->value < a->cont_threshold;
    if (want != a->beeping) sound_gate(&a->snd, want, a->p->millis());
    a->beeping = want;
    /* Diode: one short beep when a good junction appears, not a tone for as
     * long as it stays connected. It re-arms once the leads open again. */
    bool junction = a->fn == MEAS_DIODE && !s->overrange &&
                    s->value <= a->diode_th_dv / 10.0 && s->value >= 0.0;
    if (junction && !a->junction) app_sound(a, SND_DIODE);
    a->junction = junction;
    if (s->overrange) a->sample_over = true;
}

void app_sound(app_t *a, snd_event_t e)
{
    if (a->beeper) sound_play(&a->snd, e, a->p->millis());
}

/* Drive the beeper from the sound module: tone when the board has it,
 * plain on/off otherwise. */
static void sound_out(app_t *a, uint32_t now)
{
    uint16_t hz;
    uint8_t level;
    if (!sound_tick(&a->snd, now, &hz, &level)) return;
    if (a->p->tone) a->p->tone(hz, level);
    else if (a->p->beep) a->p->beep(level > 0);
}

/* Auto-hold (B15), on each fresh reading: once it has held within tolerance
 * for APP_AH_STABLE_N readings and is a real measurement (not open, not
 * idle leads), freeze the display on it and beep. The hold stays when the
 * probes come off; a new stable reading that differs replaces it. Only the
 * display holds: graph, statistics, recording and SCPI stay live. */
static void autohold(app_t *a, double mean, bool over, int r)
{
    if (!a->autohold || a->fn == MEAS_CONT) { a->ah_holding = false; return; }
    double fs = readout_fullscale(a->fn, r);
    double counts = fs > 0 ? fs / 50000.0 : 0.0;
    double tol = fmax(fabs(mean) * APP_AH_TOL_FRAC, APP_AH_TOL_COUNTS * counts);
    bool steady = !over && fabs(mean - a->ah_prev) <= tol;
    a->ah_prev = mean;
    a->ah_stable = steady ? (uint8_t)(a->ah_stable < 255 ? a->ah_stable + 1 : 255) : 0;
    bool real = !over && (fs <= 0 || fabs(mean) >= APP_AH_MIN_FRAC * fs);
    if (a->ah_stable < APP_AH_STABLE_N || !real) return;
    if (a->ah_holding) {
        double held = a->ah_value;
        if (a->ah_range == r && fabs(mean - held) <= fmax(fabs(held) * APP_AH_NEW_FRAC, 50 * counts)) return;
    }
    a->ah_holding = true;
    a->ah_value = mean;
    a->ah_range = r;
    app_sound(a, SND_HOLD);
}

/* B19: the 10 A input's duty. Above APP_HIGH_A on the A jack for
 * APP_HIGH_A_ON_MS: warn under the reading and sound the alarm every second
 * until the current drops; then ask for APP_HIGH_A_REST_MS of rest. */
static void high_current(app_t *a, double amps, bool over, uint32_t now)
{
    bool a_jack = (a->fn == MEAS_DCI || a->fn == MEAS_ACI) &&
                  a->range[a->fn] >= READOUT_CURRENT_A_FIRST;
    bool high = a_jack && (over || fabs(amps) >= APP_HIGH_A);
    if (high && !a->high_a) a->high_a_since = now;
    if (!high && a->high_a) {
        a->high_a_off_at = now;
        if (now - a->high_a_since >= APP_HIGH_A_ON_MS) a->high_a_rest = true;
    }
    a->high_a = high;
    if (a->high_a_rest && !high && now - a->high_a_off_at >= APP_HIGH_A_REST_MS) a->high_a_rest = false;
    if (high && now - a->high_a_since >= APP_HIGH_A_ON_MS && now - a->alarm_at >= APP_ALARM_EVERY_MS) {
        a->alarm_at = now;
        app_sound(a, SND_ALARM);
    }
}

static void update(app_t *a)
{
    double mean;
    if (!meas_filter_take(&a->filt, &mean)) return;
    int r = a->range[a->fn];
    bool over = a->sample_over || readout_overload(a->fn, r, mean);
    a->sample_over = false;

    if (a->hold) return;                 /* HOLD freezes ranging too: nothing restarts */
    if (a->autorange && readout_autoranges(a->fn)) {
        int nr = avail(a, a->fn, readout_autorange(a->fn, r, mean, over));
        if (nr != r) { range_step(a, nr); return; }
    }

    a->value = mean;
    a->overload = over;
    a->have = true;
    a->dirty = true;
    autohold(a, mean, over, r);
    high_current(a, mean, over, a->p->millis());

    if (!over) {
        double v = displayed(a);
        if (a->fn == MEAS_ACV || a->fn == MEAS_ACI || a->fn == MEAS_CAP)
            v = meas_suppress_small(a->fn, r, v, false, a->rel);
        if (v < a->st_min) a->st_min = v;
        if (v > a->st_max) a->st_max = v;
        a->st_sum += v;
        a->st_n++;
        if (a->power) {                  /* B21: the graph follows the power */
            if (!a->pw_have) goto traced;
            v = a->pw_w;
        }
        if (!a->graph_paused) {
            a->acc_sum += v;
            if (++a->acc_n >= app_span_decim[a->span_idx]) {
                trace_push(&a->trace, (float)(a->acc_sum / a->acc_n));
                a->acc_sum = 0.0;
                a->acc_n = 0;
            }
        }
    }
traced:;

    /* Timed recording: one point per interval, from fresh readings only
     * (none while HOLD is on). Overloads are logged as such. */
    uint32_t now = a->p->millis();
    if (a->log.active && !a->rec_manual && !a->rec_fast && (int32_t)(now - a->rec_next_at) >= 0) {
        uint32_t iv = app_rec_interval_ms[a->rec_int_idx];
        a->rec_next_at += iv;
        if ((int32_t)(now - a->rec_next_at) >= 0) a->rec_next_at = now + iv;   /* fell behind */
        app_rec_point(a);
    }
}

/* Save power-on settings once they have been stable for a while. */
static void persist(app_t *a, uint32_t now)
{
    if (!a->p->store) return;
    uint8_t cur[SETTINGS_MAX_LEN];
    size_t n = settings_pack(a, cur, sizeof cur);
    bool differs = n != a->saved_len || memcmp(cur, a->saved, n) != 0;
    if (!differs) { a->save_pending = false; return; }
    if (!a->save_pending) { a->save_pending = true; a->changed_at = now; return; }
    if (now - a->changed_at < SETTINGS_SAVE_DELAY_MS) return;
    if (store_save(&a->store, cur, n, SETTINGS_VERSION) == 0) {
        memcpy(a->saved, cur, n);
        a->saved_len = n;
        a->saves++;
    }
    a->save_pending = false;
}

void app_step(app_t *a)
{
    const platform_t *p = a->p;
    app_key_t k;
    uint32_t now = p->millis();
    bool keyed = a->nkeyq > 0;
    for (;;) {
        if (a->nkeyq) {                   /* scanned while a frame was drawn */
            k = a->keyq[0];
            for (uint8_t i = 1; i < a->nkeyq; i++) a->keyq[i - 1] = a->keyq[i];
            a->nkeyq--;
        } else if (!p->key_poll || (k = p->key_poll()) == KEY_NONE) break;
        keyed = true;
        a->last_key_at = now;
        app_sound(a, SND_CLICK);          /* the click, on every press, as stock */
        if (a->dimmed) {                  /* the first key only wakes the panel */
            a->dimmed = false;
            app_apply_prefs(a);
            continue;
        }
        if (a->remote) {                  /* panel locked; RUN/STOP returns to local */
            if (k == KEY_RUNSTOP) { a->remote = false; a->dirty = true; }
            continue;
        }
        app_key(a, k);
    }
    sound_out(a, p->millis());
    if (p->serial_read) {
        char rx[64];
        size_t n;
        while ((n = p->serial_read(rx, sizeof rx)) > 0) scpi_input(&a->scpi, rx, n);
    }

    acq_sample_t s;
    if (a->npend) {                       /* read while the last frame was drawn */
        uint8_t n = a->npend;
        a->npend = 0;
        for (uint8_t i = 0; i < n; i++) {
            sample(a, &a->pend[i]);
            if (a->acq.st == ACQ_IDLE) break;   /* reconfigured: the rest are stale */
        }
    } else if (acq_poll(&a->acq, p, &s)) sample(a, &s);

    now = p->millis();
    uint32_t dim_ms = menu_dim_s[a->dim_idx] * 1000u;
    if (dim_ms && !a->dimmed && now - a->last_key_at >= dim_ms) {
        a->dimmed = true;
        app_apply_prefs(a);
    }
    if (a->show_now || now - a->last_update >= APP_DISPLAY_MS) {
        a->show_now = false;
        a->last_update = now;
        update(a);
    }
    sound_out(a, p->millis());
    persist(a, now);
    /* Recording every 15 ms makes every sample a change; a frame costs
     * ~150 ms on the meter, so pace them, except straight after a key. */
    if (a->dirty && a->rec_fast && !keyed && now - a->last_frame_at < REC_FAST_FRAME_MS) return;
    if (a->dirty) {
        app_build_view(a);
        app_render(a);
        a->dirty = false;
        a->last_frame_at = p->millis();
    }
}

/* ---- view -------------------------------------------------------------------- */

static void fmt_si(char *out, size_t cap, double v)
{
    static const char *pfx[] = { "n", "u", "m", "", "k", "M" };
    double av = fabs(v);
    int i = av == 0 ? 3 : av < 1e-6 ? 0 : av < 1e-3 ? 1 : av < 1 ? 2 : av < 1e3 ? 3 : av < 1e6 ? 4 : 5;
    static const double mul[] = { 1e9, 1e6, 1e3, 1, 1e-3, 1e-6 };
    snprintf(out, cap, "%.6g%s", v * mul[i], pfx[i]);   /* 5.5 digits */
}

/* B21: five significant digits with an m / k prefix on `base`: "451.20" "mW". */
static void si5(double x, const char *base, char *val, size_t vc, char *unit, size_t uc)
{
    double ax = fabs(x);
    const char *p = "";
    if (ax >= 1000.0) { x /= 1000.0; p = "k"; }
    else if (ax < 1.0 && ax > 0.0) { x *= 1000.0; p = "m"; }
    double m = fabs(x);
    int dec = m >= 1000.0 ? 1 : m >= 100.0 ? 2 : m >= 10.0 ? 3 : 4;
    snprintf(val, vc, "%.*f", dec, x);
    snprintf(unit, uc, "%s%s", p, base);
}

static void key(app_view_t *v, int i, const char *label, const char *value)
{
    snprintf(v->key_label[i], sizeof v->key_label[i], "%s", label ? label : "");
    snprintf(v->key_value[i], sizeof v->key_value[i], "%s", value ? value : "");
}

static void fmt_interval(char *out, size_t cap, uint32_t ms)
{
    if (ms < 1000) snprintf(out, cap, "%lu ms", (unsigned long)ms);
    else if (ms < 60000) snprintf(out, cap, "%lu s", (unsigned long)(ms / 1000));
    else snprintf(out, cap, "%lu min", (unsigned long)(ms / 60000));
}

/* The recorded session, resampled to the trace: each point is the mean of
 * its share of the log, skipping overloads. Rebuilt only when it grows. */
static void rec_trace_build(app_t *a)
{
    const dlog_t *d = &a->log;
    if (a->rec_trace_count == d->count && a->rec_trace_session == d->session) return;
    a->rec_trace_count = d->count;
    a->rec_trace_session = d->session;
    trace_init(&a->rec_trace);
    uint32_t n = d->have ? d->count : 0, buckets = n < TRACE_MAX ? n : TRACE_MAX;
    for (uint32_t b = 0; b < buckets; b++) {
        uint32_t i0 = (uint32_t)((uint64_t)b * n / buckets), i1 = (uint32_t)((uint64_t)(b + 1) * n / buckets);
        double sum = 0;
        uint32_t k = 0;
        for (uint32_t i = i0; i < i1; i++) {
            float f;
            if (dlog_get(d, i, &f) && isfinite(f)) { sum += f; k++; }
        }
        if (k) trace_push(&a->rec_trace, (float)(sum / k));
    }
}

/* Base unit of what a session logged (values are SI). */
static const char *rec_unit(uint8_t fn, uint8_t tag)
{
    static const char *const base[MEAS_NFUNC] = {
        "V DC", "V AC", "A DC", "A AC", "\xCE\xA9", "\xCE\xA9", "\xCE\xA9", "V", "F", "Hz", "C",
    };
    static const char *const tunits[] = { "\xC2\xB0" "C", "\xC2\xB0" "F", "K" };   /* shown, not SCPI */
    if (fn >= MEAS_NFUNC) return "";
    if (tag & 0x20) return "W";
    if (tag & 0x10) return "dB";
    if (tag & 0x08) return "dBm";
    if (fn == MEAS_FREQ && (tag & 0x04)) return "s";
    if (fn == MEAS_TEMP) return tunits[(tag & 3) % 3];
    return base[fn];
}

static void record_view(app_t *a, app_view_t *v)
{
    const dlog_t *d = &a->log;
    char b[16];
    rec_trace_build(a);
    if (!d->io)
        snprintf(v->sub, sizeof v->sub, "No log storage");
    else
        snprintf(v->sub, sizeof v->sub, "%s %lu/%lu",
                 d->active ? (d->wrap ? "ROLL" : "REC") : d->have && d->count >= dlog_capacity(d) ? "FULL" : "STOP",
                 (unsigned long)(d->have ? d->count : 0), (unsigned long)dlog_capacity(d));
    snprintf(v->unit, sizeof v->unit, "%s", d->have ? rec_unit(d->fn, d->tag) : "");
    v->cursor = -1;
    int tn = a->rec_trace.n;
    if (a->rec_cursor >= 0 && tn > 0 && d->have && d->count) {
        /* B36: the reading under the cursor, exactly (the plot shows
         * averages), its number and when it was taken. */
        int c = a->rec_cursor < tn ? a->rec_cursor : tn - 1;
        uint32_t i = tn > 1 ? (uint32_t)((uint64_t)c * (d->count - 1) / (uint32_t)(tn - 1)) : d->count - 1;
        float val = 0;
        dlog_get(d, i, &val);
        char vs[16], ts[32] = "";              /* hours can run to 7 digits */
        if (isinf(val)) snprintf(vs, sizeof vs, "OVERLOAD");
        else if (isnan(val)) snprintf(vs, sizeof vs, "-");
        else fmt_si(vs, sizeof vs, val);
        if (d->interval_ms) {
            uint32_t s = (uint32_t)((uint64_t)i * d->interval_ms / 1000u);
            if (d->start_time) {                  /* B33: the clock time it was taken */
                char ct[24];
                clock_fmt(ct, sizeof ct, d->start_time + s, ' ');
                snprintf(ts, sizeof ts, "  %s", ct + 11);           /* hh:mm:ss */
            } else
                snprintf(ts, sizeof ts, "  +%lu:%02lu:%02lu", (unsigned long)(s / 3600u),
                         (unsigned long)(s / 60u % 60u), (unsigned long)(s % 60u));
        }
        snprintf(v->sub, sizeof v->sub, "#%lu: %s%s", (unsigned long)(i + 1), vs, ts);
        v->cursor = (int16_t)c;
    }
    key(v, 0, "RECORD", d->active ? "ON" : "OFF");
    key(v, 1, "MODE", a->rec_manual ? "MANUAL" : "AUTO");
    if (a->rec_manual) key(v, 2, "INTERVAL", "-");
    else { fmt_interval(b, sizeof b, app_rec_interval_ms[a->rec_int_idx]); key(v, 2, "INTERVAL", b); }
    if (a->rec_manual && d->active) key(v, 3, "TAKE", "POINT");
    else if (!a->rec_manual) key(v, 3, "WHEN FULL", a->rec_roll ? "ROLL" : "STOP");
    key(v, 4, "EXIT", 0);
    /* The settings are fixed while a recording runs (record_key ignores
     * them): grey them, as the owner asked 2026-11-16. TAKE POINT works. */
    if (d->active) v->key_off = (uint8_t)(1u << 1 | 1u << 2 | (a->rec_manual ? 0u : 1u << 3));
}

/* B47: state of charge from the cell voltage: two 18650s in parallel, a
 * typical Li-ion curve under a light load (~0.1-0.2 A here), so a guide, not
 * a fuel gauge. Read while charging it shows high (the charger's 4.2 V). */
int app_battery_pct(int mv)
{
    static const int16_t pts[][2] = {
        { 3300, 0 }, { 3500, 5 }, { 3600, 10 }, { 3680, 20 }, { 3740, 30 }, { 3780, 40 },
        { 3820, 50 }, { 3870, 60 }, { 3930, 70 }, { 4000, 80 }, { 4080, 90 }, { 4150, 100 },
    };
    const int n = (int)(sizeof pts / sizeof pts[0]);
    if (mv < 0) return -1;
    if (mv <= pts[0][0]) return 0;
    if (mv >= pts[n - 1][0]) return 100;
    for (int i = 1; i < n; i++)
        if (mv < pts[i][0])
            return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (mv - pts[i - 1][0]) /
                                   (pts[i][0] - pts[i - 1][0]);
    return 100;
}

void app_build_view(app_t *a)
{
    app_view_t *v = &a->view;
    memset(v, 0, sizeof *v);
    v->battery = (int8_t)app_battery_pct(a->p->battery_mv ? a->p->battery_mv() : -1);
    ui_set_battery(v->battery);           /* in the view, so a change redraws */
    int r = a->range[a->fn];
    double d = displayed(a);
    bool over = a->overload;
    if (a->autohold && a->ah_holding) {          /* B15: the held reading */
        r = a->ah_range;
        d = a->rel ? a->ah_value - a->rel_value : a->ah_value;
        over = false;
    }
    int code = r;
    double shown = d;

    if (a->fn == MEAS_FREQ) {
        code = meas_freq_code(d, a->period);
        shown = a->period ? (d != 0.0 ? 1.0 / d : 0.0) : d;
    } else if (a->fn == MEAS_ACV || a->fn == MEAS_ACI || a->fn == MEAS_CAP) {
        shown = meas_suppress_small(a->fn, r, d, false, a->rel);
    } else if (a->fn == MEAS_TEMP) {
        shown = meas_temp_unit(d, a->temp_unit);
    }

    if (!a->have && !(a->autohold && a->ah_holding))   /* a held reading outlives a ranging restart */
        snprintf(v->reading, sizeof v->reading, "-----");
    else
        readout_value(v->reading, sizeof v->reading, a->fn, code, shown, over);
    v->overload = over;

    /* B37: the bar, for functions with a full scale (not frequency or
     * temperature), from the value shown, on the range shown. */
    double fs = readout_fullscale(a->fn, r);
    if (a->bargraph && a->have && fs > 0 && a->fn != MEAS_FREQ && a->fn != MEAS_TEMP) {
        v->bar = true;
        v->bar_bipolar = a->fn == MEAS_DCV || a->fn == MEAS_DCI || a->rel;
        v->bar_frac = over ? 1.1f : (float)(shown / fs);
    }

    static const char *const tunits[] = { "\xC2\xB0" "C", "\xC2\xB0" "F", "K" };   /* shown, not SCPI */
    snprintf(v->unit, sizeof v->unit, "%s", a->fn == MEAS_TEMP ? tunits[a->temp_unit]
                                              : readout_unit(a->fn, code, a->period));
    snprintf(v->func, sizeof v->func, "%s", readout_fn_name(a->fn));
    char rl[20];
    readout_range_label(rl, sizeof rl, a->fn, r);
    bool ranged = readout_autoranges(a->fn);
    snprintf(v->range, sizeof v->range, "%s%s", ranged && a->autorange ? "AUTO " : "", rl);
    snprintf(v->status, sizeof v->status, "%s%s%s%s%s%s%s%s",
             ucal_active(&a->ucal, a->fn, r) ? "UCAL " : "",
             a->log.active ? "REC " : "", a->power ? "POWER " : a->dual ? "DUAL " : "",
             a->remote ? "RMT " : "", a->hold ? "HOLD " : "", a->autohold ? "A-HOLD " : "",
             a->rel ? "REL " : "",
             a->math == MATH_STATS ? "STATS" : a->math == MATH_DBM ? (a->db_mode ? "dB" : "dBm") : "");
    for (size_t n = strlen(v->status); n && v->status[n - 1] == ' '; n--) v->status[n - 1] = 0;

    char b1[16], b2[16], b3[16];
    uint32_t now = a->p->millis();
    if (a->high_a && now - a->high_a_since >= APP_HIGH_A_ON_MS) {      /* B19: before anything */
        snprintf(v->sub, sizeof v->sub, "Over 7 A for %lu s: remove the load",
                 (unsigned long)((now - a->high_a_since) / 1000u));
    } else if (a->high_a_rest) {
        uint32_t left = APP_HIGH_A_REST_MS - (now - a->high_a_off_at);
        snprintf(v->sub, sizeof v->sub, "10 A input: rest %lu s", (unsigned long)(left / 1000u + 1u));
    } else if (a->power) {
        /* B21: watts big; volts and current below, then energy, charge and
         * the time they cover. */
        char iv[48], vv[48], u[16];
        snprintf(u, sizeof u, "%s", v->unit);
        char *dc = strstr(u, " DC");
        if (dc) *dc = 0;                  /* "mA", not "mA DC": both are DC here */
        snprintf(iv, sizeof iv, "%s %s", v->reading, u);
        if (!a->sec_have) snprintf(vv, sizeof vv, "----- V");
        else {
            char t[24];
            readout_value(t, sizeof t, MEAS_DCV, a->pw_vrange, a->sec_value, a->sec_over);
            snprintf(u, sizeof u, "%s", readout_unit(MEAS_DCV, a->pw_vrange, false));
            if ((dc = strstr(u, " DC"))) *dc = 0;
            snprintf(vv, sizeof vv, "%s %s", t, u);
        }
        snprintf(v->sub, sizeof v->sub, "%.36s   %.36s", vv, iv);
        if (!a->pw_have) {
            snprintf(v->reading, sizeof v->reading, a->overload ? "overload" : "-----");
            snprintf(v->unit, sizeof v->unit, "W");
        } else si5(a->pw_w, "W", v->reading, sizeof v->reading, v->unit, sizeof v->unit);
        v->overload = !a->pw_have && a->overload;
        v->bar = false;
        char e[16], q[16];
        si5(a->pw_wh, "Wh", e, sizeof e, b1, sizeof b1);
        si5(a->pw_ah, "Ah", q, sizeof q, b2, sizeof b2);
        uint32_t s = a->pw_ms / 1000u;
        snprintf(v->sub2, sizeof v->sub2, "%s %s   %s %s   %02lu:%02lu:%02lu", e, b1, q, b2,
                 (unsigned long)(s / 3600u), (unsigned long)(s / 60u % 60u), (unsigned long)(s % 60u));
    } else if (a->dual) {
        /* Stock's bar under the reading: "50.032 Hz | 5 V | Freq". Swapped,
         * the frequency is the main reading and the AC value goes below. */
        char fv[24], fu[16];
        app_dual_sec_text(a, fv, sizeof fv, fu, sizeof fu);
        if (a->dual_swapped) {
            char main_val[24], main_unit[16];
            snprintf(main_val, sizeof main_val, "%s", v->reading);
            snprintf(main_unit, sizeof main_unit, "%s", v->unit);
            snprintf(v->reading, sizeof v->reading, "%s", fv);
            snprintf(v->unit, sizeof v->unit, "%s", fu);
            v->overload = false;
            snprintf(v->sub, sizeof v->sub, "%s %s   %s   %s", main_val, main_unit, rl, readout_fn_name(a->fn));
        } else {
            snprintf(v->sub, sizeof v->sub, "%s %s   %s   %s", fv, fu, rl,
                     a->dual_fn == MEAS_FREQ ? "FREQ" : readout_fn_name(a->dual_fn));
        }
        bool v5 = a->fn == MEAS_ACV && a->range[MEAS_ACV] == 1;
        if (a->dual_fn == MEAS_FREQ && a->sec_have &&
            a->sec_value > (v5 ? APP_AC_BAND_5V_HZ : APP_AC_BAND_HZ)) {   /* B20 */
            size_t n = strlen(v->sub);
            snprintf(v->sub + n, sizeof v->sub - n, v5 ? "   > 500 Hz: AC reads low" : "   > 4 kHz: AC reads low");
        }
    } else if (a->math == MATH_STATS && a->st_n) {
        fmt_si(b1, sizeof b1, a->st_min);
        fmt_si(b2, sizeof b2, a->st_max);
        fmt_si(b3, sizeof b3, a->st_sum / a->st_n);
        snprintf(v->sub, sizeof v->sub, "MIN %s  MAX %s  AVG %s", b1, b2, b3);
    } else if (a->math == MATH_DBM && a->have && !a->overload) {
        double rr = meas_dbm_rref[a->rref_idx];
        if (a->db_mode)
            snprintf(v->sub, sizeof v->sub, "%.2f dB  REF %.2f dBm", meas_db(d, rr, a->db_ref_centi),
                     a->db_ref_centi / 100.0);
        else
            snprintf(v->sub, sizeof v->sub, "%.2f dBm  REF %u " "\xCE\xA9", meas_dbm(d, rr), (unsigned)rr);
    } else if (a->fn == MEAS_FREQ && a->have) {
        snprintf(v->sub, sizeof v->sub, "DUTY %.1f%%", a->aux);
    } else if (a->fn == MEAS_TEMP && r == 0 && a->acq.have_cj) {
        /* B26: the raw EMF beside the cold junction, to check a probe
         * against a reference without the meter's conversion. */
        int n = snprintf(v->sub, sizeof v->sub, "CJ %.1f %s", meas_temp_unit(a->acq.t_cj, a->temp_unit),
                         tunits[a->temp_unit]);
        if (isfinite(a->tc_emf_mv) && n > 0 && (size_t)n < sizeof v->sub)
            snprintf(v->sub + n, sizeof v->sub - (size_t)n, "   EMF %.3f mV", a->tc_emf_mv);
    } else if (a->fn == MEAS_TEMP && r >= 1 && isfinite(a->rtd_ohm)) {
        /* B27: the sensor's resistance and the lead correction in use */
        snprintf(v->sub, sizeof v->sub, "R %.2f \xCE\xA9   LEADS %u.%02u \xCE\xA9", a->rtd_ohm,
                 a->rtd_lead_mohm / 1000u, (a->rtd_lead_mohm % 1000u) / 10u);
    } else if (a->rel) {
        fmt_si(b1, sizeof b1, a->rel_value);
        snprintf(v->sub, sizeof v->sub, "REL %s", b1);
    }

    if (a->dual_menu) {                  /* the choice of secondary, as stock */
        snprintf(v->sub, sizeof v->sub, "Second reading: pick one");
        if (a->fn == MEAS_ACV || a->fn == MEAS_ACI) key(v, 0, "DUAL", "FREQ");
        meas_fn_t pf = app_dual_partner(a->fn);
        if (pf < MEAS_NFUNC && (!a->p->fn_avail || a->p->fn_avail(pf)))
            key(v, 1, "DUAL", pf == MEAS_DCV || pf == MEAS_DCI ? "DC" : "AC");   /* B23 */
        if (power_possible(a)) key(v, 2, "DUAL", "POWER");                          /* B21 */
        key(v, 4, "CANCEL", 0);
        return;
    }
    if (a->menu_open) {
        for (int i = 0; i < MI_COUNT; i++)
            menu_value(a, (menu_item_t)i, v->menu_value[i], sizeof v->menu_value[i]);
        key(v, 0, "ITEM", "UP");
        key(v, 1, "ITEM", "DOWN");
        key(v, 2, "VALUE", "+");
        key(v, 3, "VALUE", "-");
        key(v, 4, "EXIT", 0);
        return;
    }
    if (a->screen == SCREEN_RECORD) {
        record_view(a, v);
        return;
    }
    if (a->screen == SCREEN_GRAPH) {
        uint32_t sec = app_span_seconds(a->span_idx);
        if (sec < 120) snprintf(b1, sizeof b1, "%lu s", (unsigned long)sec);
        else snprintf(b1, sizeof b1, "%lu min", (unsigned long)((sec + 30) / 60));
        key(v, 0, "SPAN", b1);
        key(v, 1, "SCALE", a->graph_fixed ? "RANGE" : "AUTO");
        key(v, 4, "PAUSE", a->graph_paused ? "ON" : "OFF");
    } else {
        key(v, 0, "REL", a->rel ? "ON" : "OFF");
        key(v, 1, "RANGE", ranged ? (a->autorange ? "AUTO" : "MAN") : "-");
    }
    static const char *const mini_name[3] = { "NUM", "NUM+G", "NUM+H" };
    key(v, 2, "VIEW", a->screen != SCREEN_GRAPH ? mini_name[a->mini % 3] : a->graph_hist ? "HISTO" : "GRAPH");
    key(v, 3, "CLEAR", "");
    if (a->screen == SCREEN_GRAPH) { }
    else if (a->fn == MEAS_FREQ) key(v, 4, "SHOW", a->period ? "PERIOD" : "FREQ");
    else if (a->fn == MEAS_TEMP) key(v, 4, "SENSOR", r == 0 ? "K" : r == 1 ? "PT100" : "PT1000");
    else if (a->fn == MEAS_DCI || a->fn == MEAS_ACI) {
        key(v, 4, "INPUT", r >= READOUT_CURRENT_A_FIRST ? "A" : "mA");
        if (!a_jack_avail(a)) v->key_off |= 1u << 4;          /* the board has no A-jack rows */
    }
    else if (a->math == MATH_DBM) { snprintf(b1, sizeof b1, "%u", meas_dbm_rref[a->rref_idx]); key(v, 4, "REF", b1); }
    else key(v, 4, 0, 0);
    /* Our panel has five softkeys (docs/hardware.md): the theme lives in the
     * UTILITY menu, and in dual mode the last key is EXIT. */
    if (a->dual && a->screen == SCREEN_MEASURE) key(v, 4, "EXIT", a->power ? "POWER" : "DUAL");
    if (a->power && a->screen == SCREEN_MEASURE) key(v, 3, "CLEAR", "Wh");
}

/* ---- rendering --------------------------------------------------------------- */

/* Fixed graph scale: the current range's full scale, in the units the trace
 * holds (SI). Functions without a meaningful full scale stay on auto. */
static bool graph_scale(const app_t *a, float out[2])
{
    if (!readout_autoranges(a->fn) || a->fn == MEAS_FREQ || a->fn == MEAS_TEMP) return false;
    int r = a->range[a->fn];
    double fs = meas_to_si(a->fn, r, meas_fullscale(a->fn, r));    /* nominal: 5 V, not 5.5 */
    bool bipolar = a->fn == MEAS_DCV || a->fn == MEAS_DCI || a->rel;
    out[0] = bipolar ? (float)-fs : 0.0f;
    out[1] = (float)fs;
    return true;
}

void app_draw(const app_t *a, fb_t *f)
{
    const app_view_t *v = &a->view;
    const theme_t *th = a->theme;
    softkey_t keys[UI_NKEYS];
    for (int i = 0; i < UI_NKEYS; i++) {
        keys[i].label = v->key_label[i][0] ? v->key_label[i] : 0;
        keys[i].value = v->key_value[i][0] ? v->key_value[i] : 0;
        keys[i].off = (v->key_off >> i) & 1u;
    }
    if (a->menu_open && a->cal_page) {   /* B32: only the visible rows are formatted */
        int n = app_cal_rows(), rows = screen_menu_rows(), sel = a->cal_sel;
        int first = sel - rows / 2;
        if (first > n - rows) first = n - rows;
        if (first < 0) first = 0;
        int m = n - first < rows ? n - first : rows;
        char lab[12][24], val[12][40];
        const char *lp[12], *vp[12];
        if (m > 12) m = 12;
        for (int i = 0; i < m; i++) {
            app_cal_row(a, first + i, NULL, NULL, lab[i], sizeof lab[i], val[i], sizeof val[i]);
            lp[i] = lab[i]; vp[i] = val[i];
        }
        screen_menu_ex(f, th, "CALIBRATION", lp, vp, m, sel - first, keys, UI_NKEYS, NULL);
        return;
    }
    if (a->menu_open) {
        const char *vals[MI_COUNT];
        for (int i = 0; i < MI_COUNT; i++) vals[i] = v->menu_value[i];
        screen_menu_ex(f, th, "UTILITY", menu_labels, vals, MI_COUNT, a->menu_sel, keys, UI_NKEYS,
                       menu_divider);
        return;
    }
    if (a->screen == SCREEN_RECORD) {
        screen_plot_ex(f, th, &a->rec_trace, v->sub, v->unit, keys, UI_NKEYS, NULL, v->cursor);
        return;
    }
    if (a->screen == SCREEN_GRAPH) {
        char title[80];                   /* reading (24) + two spaces + status (48) */
        snprintf(title, sizeof title, "%s%s%s", v->reading, v->status[0] ? "  " : "", v->status);
        if (a->graph_hist) {
            screen_hist(f, th, &a->trace, title, v->unit, keys, UI_NKEYS);
            return;
        }
        float fixed[2];
        bool fx = a->graph_fixed && graph_scale(a, fixed);
        screen_plot(f, th, &a->trace, title, v->unit, keys, UI_NKEYS, fx ? fixed : NULL);
        return;
    }
    theme_t low;                          /* the energy line sits low: only a thin horizon under it */
    if (v->sub2[0] && th->glow_h > 8) { low = *th; low.glow_h = 8; th = &low; }
    if (a->mini) {                        /* B58: the unit goes small, the history under the digits */
        const int w = SCR_W - RAIL_W, uw = fb_text_w(v->unit, 3);
        screen_measure(f, th, v->reading, "", v->status, keys, UI_NKEYS, v->range, v->func);
        fb_text(f, w - 12 - uw, BAR_H + 128, v->unit, 3, th->accent);
        int mx = 12, my = BAR_H + 118, mw = w - 24 - uw - 16, mh = 46;
        if (a->mini == 1) ui_mini_trend(f, th, &a->trace, mx, my, mw, mh);
        else ui_mini_hist(f, th, &a->trace, mx, my, mw, mh);
    } else
        screen_measure(f, th, v->reading, v->unit, v->status, keys, UI_NKEYS, v->range, v->func);
    if (v->bar) ui_bar(f, th, 12, SCR_H - FOOT_H - 24, SCR_W - RAIL_W - 24, 10, v->bar_frac, v->bar_bipolar);
    if (v->sub[0]) {                      /* small when it would run under the rail */
        int ss = fb_text_w(v->sub, 2) <= SCR_W - RAIL_W - 24 ? 2 : 1;
        fb_text(f, 12, BAR_H + 172 + (fb_text_h(2) - fb_text_h(ss)) / 2, v->sub, ss, th->dim);
    }
    if (v->sub2[0]) {                     /* B21: energy, charge, time */
        int ss = fb_text_w(v->sub2, 2) <= SCR_W - RAIL_W - 24 ? 2 : 1;
        fb_text(f, 12, BAR_H + 198 + (fb_text_h(2) - fb_text_h(ss)) / 2, v->sub2, ss, th->fg);
    }
}

static uint32_t fnv1a_more(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static uint32_t fnv1a(const rgb565_t *px, size_t n)
{
    return fnv1a_more(2166136261u, px, n * sizeof *px);
}

/* A fingerprint of everything app_draw reads: the view (cleared before each
 * build, so its bytes are stable), the theme, which screen and menu row,
 * and for the graph screens the trace and its scale. About 1 KB to hash,
 * against 300 KB of pixels for a full compose. */
static uint32_t frame_key(const app_t *a)
{
    uint32_t h = fnv1a_more(2166136261u, &a->view, sizeof a->view);
    struct {
        const theme_t *theme;
        int screen, menu_sel, fn, range;
        bool menu_open, graph_fixed, graph_hist, rel;
        uint8_t mini, cal_sel;
        bool cal_page;
    } s;
    memset(&s, 0, sizeof s);              /* padding too: hashed as bytes */
    s.theme = a->theme;
    s.screen = (int)a->screen;
    s.menu_sel = a->menu_sel;
    s.fn = (int)a->fn;
    s.range = a->range[a->fn];
    s.menu_open = a->menu_open;
    s.graph_fixed = a->graph_fixed;
    s.graph_hist = a->graph_hist;
    s.rel = a->rel;
    s.mini = a->mini;
    s.cal_page = a->cal_page;
    s.cal_sel = a->cal_sel;
    h = fnv1a_more(h, &s, sizeof s);
    if (!a->menu_open && (a->screen == SCREEN_GRAPH || (a->screen == SCREEN_MEASURE && a->mini)))
        h = fnv1a_more(h, &a->trace, sizeof a->trace);
    if (!a->menu_open && a->screen == SCREEN_RECORD) h = fnv1a_more(h, &a->rec_trace, sizeof a->rec_trace);
    return h;
}

/* Draw the screen one band at a time and push only the bands that changed.
 * The same drawing code runs once per band, clipped to that band. Skipped
 * entirely when nothing app_draw reads has changed since the last frame;
 * clearing hash_valid forces a full redraw. */
void app_render(app_t *a)
{
    const platform_t *p = a->p;
    int rows = p->band_rows;
    if (!p->band || rows <= 0) return;
    int nb = (SCR_H + rows - 1) / rows;
    if (nb > APP_NBANDS_MAX) return;
    /* Nothing on screen changed: skip the compose (B56; 84 ms on the meter). */
    uint32_t key = frame_key(a);
    if (a->hash_valid && key == a->frame_key) { a->frames_skipped++; return; }
    a->frame_key = key;
    a->frames_drawn++;
    for (int b = 0; b < nb; b++) {
        /* While ranging, a frame (~150 ms on the meter) would hold up the
         * hunt: keep the front end's settle, discards and fast conversions
         * moving between bands (B10d). Not in steady state: each poll reads
         * INTF over the slow isolated bus (~6 ms at 192 us a half bit). The
         * sample itself waits for the next step, so the frame stays whole. */
        /* At SPEED M and F (B11) too: a reading every 62-125 ms, and a frame
         * takes ~150 ms, so drawing would cap them at ~5 a second (on our
         * meter, 2026-11-16: 5.0/s at both). Queue them; while ranging, one. */
        bool fast = a->hunting || acq_timing_critical(&a->acq);
        uint8_t room = fast ? 1 : APP_NPEND;
        if (b && (fast || a->rec_fast || a->rate) && a->npend < room
            && acq_poll(&a->acq, p, &a->pend[a->npend]))
            a->npend++;
        /* Keep scanning the keypad mid-frame: it needs two scans 20 ms apart,
         * and a tap is shorter than a frame. */
        if (b && p->key_poll && a->nkeyq < sizeof a->keyq / sizeof a->keyq[0]) {
            app_key_t kk = p->key_poll();
            if (kk != KEY_NONE) a->keyq[a->nkeyq++] = kk;
        }
        int y = b * rows, h = SCR_H - y < rows ? SCR_H - y : rows;
        fb_t f;
        fb_init_band(&f, p->band, SCR_W, h, 0, y);
        app_draw(a, &f);
        uint32_t hash = fnv1a(p->band, (size_t)SCR_W * h);
        if (a->hash_valid && a->band_hash[b] == hash) continue;
        a->band_hash[b] = hash;
        lcd_blit(0, (uint16_t)y, SCR_W, (uint16_t)h, p->band, SCR_W);
        a->bands_sent++;
    }
    a->hash_valid = true;
}
