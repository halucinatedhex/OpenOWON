/* SCPI remote control, bound to the app.
 *
 * Grounded in the stock image where it could be (docs/scpi.md):
 *   - keywords from the recovered dispatch table (data-tables.md)
 *   - MEAS? replies "%E" in SI base units; overload replies "1E+9";
 *     with a secondary reading, "%s,%s" (stock 0x080115C8 region)
 *   - FUNC? replies quoted names from stock's table at 0x0801E310
 *   - RATE replies S / M / F and accepts either case
 * Where stock's syntax is not recovered, conventional SCPI is used.
 *
 * One deliberate difference: FUNC? reports "CONT" in continuity and "DIOD" in
 * diode. Stock's reply table lists DIOD before CONT while its function codes
 * run continuity (6) then diode (7), consistent with the swap users report.
 */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "readout.h"

#define APP(s) ((app_t *)(s)->ctx)

static const char *const fn_name[MEAS_NFUNC] = {
    "VOLT", "VOLT AC", "CURR", "CURR AC", "RES", "FRES", "CONT", "DIOD", "CAP", "FREQ", "TEMP",
};

/* ---- helpers ---------------------------------------------------------------- */

static void need_query(scpi_t *s, const scpi_call_t *c, bool want)
{
    if (c->query != want) scpi_error(s, c->query ? SCPI_E_QUERY : SCPI_E_COMMAND);
}

/* Range argument: a value in SI (pick the smallest range that holds it),
 * MIN, MAX, DEF or AUTO. Returns false on a bad argument. */
static bool set_manual_range(app_t *a, meas_fn_t fn, int r);

static bool apply_range_arg(app_t *a, meas_fn_t fn, const char *arg)
{
    if (!arg) return true;
    int n = meas_nranges(fn);
    if (scpi_match_word(arg, "AUTO") || scpi_match_word(arg, "DEFault")) {
        a->autorange = true;
        return true;
    }
    int r;
    if (scpi_match_word(arg, "MINimum")) r = 0;
    else if (scpi_match_word(arg, "MAXimum")) r = n - 1;
    else {
        double v;
        if (!scpi_parse_number(arg, &v) || v < 0) return false;
        for (r = 0; r < n - 1; r++)
            if (meas_to_si(fn, r, meas_fullscale(fn, r)) >= v * (1.0 - 1e-9)) break;
    }
    return set_manual_range(a, fn, r);
}

/* RANGE <n>: stock V4.3.0 takes a 1-based range number (DCV: 1 = 50 mV ...
 * 6 = 1000 V) and ignores numbers outside the ladder (observed). CONF takes
 * an SI value instead. */
static bool apply_range_number(app_t *a, meas_fn_t fn, const char *arg)
{
    int n = meas_nranges(fn);
    if (scpi_match_word(arg, "AUTO") || scpi_match_word(arg, "DEFault")) { a->autorange = true; return true; }
    if (scpi_match_word(arg, "MINimum")) return set_manual_range(a, fn, 0);
    if (scpi_match_word(arg, "MAXimum")) return set_manual_range(a, fn, n - 1);
    double v;
    if (!scpi_parse_number(arg, &v) || v != floor(v) || v < 1 || v > n) return false;
    return set_manual_range(a, fn, (int)v - 1);
}

static bool set_manual_range(app_t *a, meas_fn_t fn, int r)
{
    a->autorange = false;
    a->range[fn] = r;
    return true;
}

static void configure(scpi_t *s, const scpi_call_t *c, meas_fn_t fn)
{
    app_t *a = APP(s);
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    if (!app_fn_avail(a, fn)) { scpi_error(s, SCPI_E_SETTINGS); return; }   /* not on this board */
    if (!apply_range_arg(a, fn, c->argc ? c->argv[0] : NULL)) { scpi_error(s, SCPI_E_PARAM); return; }
    app_select(a, fn);
    a->dirty = true;
}

/* ---- handlers ----------------------------------------------------------------- */

static void h_idn(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, true);
    if (!c->query) return;
    app_t *a = APP(s);
    scpi_replyf(s, "OWON,XDM1241,%s,%s", a->p->serial_number ? a->p->serial_number : "0", APP_VERSION);
}

static void h_rst(scpi_t *s, const scpi_call_t *c) { (void)c; app_reset_defaults(APP(s)); }

static void h_cls(scpi_t *s, const scpi_call_t *c) { (void)c; while (scpi_error_pop(s)) { } }

static void h_err(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, true);
    if (!c->query) return;
    int e = scpi_error_pop(s);
    scpi_replyf(s, "%d,\"%s\"", e, scpi_error_text(e));
}

static void h_meas(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, true);
    if (!c->query) return;
    app_t *a = APP(s);
    char sec[24] = "";
    if (a->dual) {
        if (!a->sec_have) snprintf(sec, sizeof sec, "9.91E+37");
        else if (a->sec_over) snprintf(sec, sizeof sec, "1E+9");
        else snprintf(sec, sizeof sec, "%E", a->sec_value);
    }
    if (c->suffix == 2) {                     /* secondary display */
        /* Stock: "%E" of the frequency with dual display on, NONe otherwise
         * (in frequency mode too, though it shows duty on screen). */
        scpi_reply(s, a->dual ? sec : "NONe");
        return;
    }
    char prim[24];
    double v;
    if (!a->have) snprintf(prim, sizeof prim, "9.91E+37");    /* SCPI not-a-number */
    else if (!app_measurement(a, &v)) snprintf(prim, sizeof prim, "1E+9");
    else snprintf(prim, sizeof prim, "%E", v);
    /* Stock, dual on: MEAS? gives both, "0.000000E+00,1.460261E+01" */
    if (a->dual && c->suffix != 1) scpi_replyf(s, "%s,%s", prim, sec);
    else scpi_reply(s, prim);
}

#define CONF(name, fn) static void name(scpi_t *s, const scpi_call_t *c) { configure(s, c, fn); }
CONF(h_conf_vdc, MEAS_DCV)
CONF(h_conf_vac, MEAS_ACV)
CONF(h_conf_idc, MEAS_DCI)
CONF(h_conf_iac, MEAS_ACI)
CONF(h_conf_res, MEAS_OHM2)
CONF(h_conf_fres, MEAS_OHM2)    /* no 4-wire on the XDM1241: stock maps FRES to RES */
CONF(h_conf_cont, MEAS_CONT)
CONF(h_conf_diod, MEAS_DIODE)
CONF(h_conf_cap, MEAS_CAP)

static void h_conf_freq(scpi_t *s, const scpi_call_t *c) { APP(s)->period = false; configure(s, c, MEAS_FREQ); }
static void h_conf_per(scpi_t *s, const scpi_call_t *c)  { APP(s)->period = true;  configure(s, c, MEAS_FREQ); }

static void h_conf_temp(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    if (c->argc) {
        if (scpi_match_word(c->argv[0], "KITS90")) a->range[MEAS_TEMP] = 0;
        else if (scpi_match_word(c->argv[0], "PT100")) a->range[MEAS_TEMP] = 1;
    else if (scpi_match_word(c->argv[0], "PT1000")) a->range[MEAS_TEMP] = 2;
        else if (scpi_match_word(c->argv[0], "PT1000")) a->range[MEAS_TEMP] = 2;   /* ours (B28) */
        else { scpi_error(s, SCPI_E_PARAM); return; }
    }
    app_select(a, MEAS_TEMP);
}

static void h_func(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->suffix == 2) {                     /* FUNC2: the secondary */
        if (c->query) {                       /* stock: unquoted FREQ; ours too: DC, AC (B23) */
            const char *n = !a->dual ? "NONe" : a->power ? "POW" : a->dual_fn == MEAS_FREQ ? "FREQ"
                          : a->dual_fn == MEAS_DCV || a->dual_fn == MEAS_DCI ? "DC" : "AC";
            scpi_reply(s, n);
            return;
        }
        if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
        if (scpi_match_word(c->argv[0], "NONe")) { app_dual_off(a); return; }
        if (scpi_match_word(c->argv[0], "POWer")) {                 /* B21 */
            if (!app_power_on(a)) scpi_error(s, SCPI_E_SETTINGS);
            return;
        }
        meas_fn_t f2;
        if (scpi_match_word(c->argv[0], "FREQuency")) f2 = MEAS_FREQ;
        else if (scpi_match_word(c->argv[0], "DC") || scpi_match_word(c->argv[0], "AC")) {
            f2 = app_dual_partner(a->fn);
            bool want_dc = scpi_match_word(c->argv[0], "DC");
            if (f2 >= MEAS_NFUNC || want_dc != (f2 == MEAS_DCV || f2 == MEAS_DCI)) {
                scpi_error(s, SCPI_E_SETTINGS); return;
            }
        } else { scpi_error(s, SCPI_E_PARAM); return; }
        if (!app_dual_on_fn(a, f2)) scpi_error(s, SCPI_E_SETTINGS);
        return;
    }
    if (c->query) {
        const char *n = (a->fn == MEAS_FREQ && a->period) ? "PER" : fn_name[a->fn];
        scpi_replyf(s, "\"%s\"", n);
        return;
    }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    for (int f = 0; f < MEAS_NFUNC; f++) {
        if (!strcmp(c->argv[0], fn_name[f])) { a->period = false; app_select(a, (meas_fn_t)f); return; }
    }
    if (!strcmp(c->argv[0], "PER")) { a->period = true; app_select(a, MEAS_FREQ); return; }
    scpi_error(s, SCPI_E_PARAM);
}

/* RANGE? text exactly as stock V4.3.0 sends it (docs/scpi.md, observed
 * 2026-11-08): "500 mV", "5 V", "500 Ω" (UTF-8), "5 KΩ" (capital K),
 * "5 MΩ", "500 uA" (ASCII u), "5 A", "10 A", "50 nF", "5uF" (no space),
 * "5 mF", "Hz", "s" (period), "KITS90", "Pt100". Only the mA current ranges
 * other than 500 uA are not yet seen; they follow the pattern. */
size_t app_scpi_range_text(char *out, size_t cap, meas_fn_t fn, int range, bool period)
{
    int n;
    if (fn == MEAS_TEMP)  return (size_t)snprintf(out, cap, "%s", range == 0 ? "KITS90" : range == 1 ? "Pt100" : "Pt1000");
    if (fn == MEAS_FREQ)  return (size_t)snprintf(out, cap, "%s", period ? "s" : "Hz");
    char label[24];
    readout_range_label(label, sizeof label, fn, range);          /* "5 kOhm", "500 uA" */
    char *o = strstr(label, "Ohm");
    char *uf = strstr(label, " uF");
    if (uf) memmove(uf, uf + 1, strlen(uf));        /* stock: "5uF", but "50 nF", "5 mF" */
    if (o) {
        if (o > label && o[-1] == 'k') o[-1] = 'K';
        n = snprintf(out, cap, "%.*s\xce\xa9", (int)(o - label), label);
    } else {
        n = snprintf(out, cap, "%s", label);
    }
    return n < 0 ? 0u : (size_t)n;
}

static void h_range(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->suffix == 2) {                     /* RANGE2?: the AC range in use, dual only */
        if (!c->query || !a->dual) { scpi_error(s, SCPI_E_SETTINGS); return; }
        char text[24];
        if (a->dual_fn == MEAS_FREQ) app_scpi_range_text(text, sizeof text, a->fn, a->range[a->fn], false);
        else app_scpi_range_text(text, sizeof text, a->dual_fn, a->acq.range2, false);   /* B23 */
        scpi_reply(s, text);
        return;
    }
    if (c->query) {
        if (a->fn == MEAS_CONT || a->fn == MEAS_DIODE) { scpi_error(s, SCPI_E_SETTINGS); return; }
        char text[24];
        app_scpi_range_text(text, sizeof text, a->fn, a->range[a->fn], a->period);
        scpi_reply(s, text);
        return;
    }
    if (!readout_autoranges(a->fn)) { scpi_error(s, SCPI_E_SETTINGS); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    if (!apply_range_number(a, a->fn, c->argv[0])) { scpi_error(s, SCPI_E_PARAM); return; }
    app_select(a, a->fn);
}

/* B21: power mode's numbers, SI, "%E"; 9.91E+37 until there is a value.
 * Settings conflict (-221) outside power mode. */
static void power_reply(scpi_t *s, const scpi_call_t *c, int what)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    if (!c->query) return;
    if (!a->power) { scpi_error(s, SCPI_E_SETTINGS); return; }
    double v = what == 0 ? a->pw_w : what == 1 ? a->pw_wh : what == 2 ? a->pw_ah : a->pw_ms / 1000.0;
    if (what == 0 && !a->pw_have) scpi_reply(s, "9.91E+37");
    else scpi_replyf(s, "%E", v);
}
static void h_meas_pow(scpi_t *s, const scpi_call_t *c)  { power_reply(s, c, 0); }
static void h_meas_ener(scpi_t *s, const scpi_call_t *c) { power_reply(s, c, 1); }
static void h_meas_char(scpi_t *s, const scpi_call_t *c) { power_reply(s, c, 2); }
static void h_meas_etime(scpi_t *s, const scpi_call_t *c) { power_reply(s, c, 3); }
static void h_ener_clear(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    (void)c;
    if (!a->power) { scpi_error(s, SCPI_E_SETTINGS); return; }
    app_power_clear(a);
}

/* MEAS2:SHOW?: the secondary as displayed, "14.603HZ" (stock, dual on). */
static void h_meas2_show(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    if (!c->query) return;
    if (!a->dual) { scpi_reply(s, "NONe"); return; }
    if (a->dual_fn == MEAS_FREQ) { scpi_replyf(s, "%.3fHZ", a->sec_have ? a->sec_value : 0.0); return; }
    char v[24], u[16], uu[16];
    app_dual_sec_text(a, v, sizeof v, u, sizeof u);
    size_t j = 0;
    for (size_t i = 0; u[i] && j < sizeof uu - 1; i++) if (u[i] != ' ') uu[j++] = u[i];
    uu[j] = 0;
    scpi_replyf(s, "%s%s", a->sec_over ? "overload" : v, a->sec_over ? "" : uu);
}

/* MEAS:SHOW?: the reading as the display shows it, with its unit, as stock
 * V4.3.0 answers: "-0.0001VDC", "000.00mVAC", "-0.0000ADC", "0.000 uF",
 * "0.0000Hz", "-66.43dBm", "184.58" + GB2312 degree-C, "overload".
 * Observed forms are tested; others follow them (docs/scpi.md). */
static void h_meas_show(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, true);
    if (!c->query) return;
    app_t *a = APP(s);
    if (!a->have) { scpi_reply(s, "-----"); return; }
    if (a->overload) { scpi_reply(s, "overload"); return; }
    double v;
    app_measurement(a, &v);
    if (a->math == MATH_DBM && (a->fn == MEAS_DCV || a->fn == MEAS_ACV)) {
        scpi_replyf(s, "%.2f%s", v, a->db_mode ? "dB" : "dBm");
        return;
    }
    const app_view_t *vw = &a->view;
    const char *num = vw->reading;
    char out[64];
    /* Stock pads the integer part to the range's width: "000.00" on 500 mV. */
    int r = a->range[a->fn], pad = 0;
    int neg = num[0] == '-';
    if (readout_autoranges(a->fn)) {
        int fs = meas_fullscale(a->fn, r), want = 1;
        while (fs >= 10) { fs /= 10; want++; }
        const char *dot = strchr(num, '.');
        int have = dot ? (int)(dot - num) - neg : 0;
        if (dot && have < want) pad = want - have;
    }
    const char *u = vw->unit;            /* "mV DC", "kOhm", "uF", "Hz", "C" */
    char unit[24];
    if (a->fn == MEAS_TEMP) {
        snprintf(unit, sizeof unit, "%s", a->temp_unit == MEAS_DEG_C ? "\xa1\xe6" : u);
    } else if (strstr(u, "Ohm")) {
        /* Stock writes this Omega in GB2312 (A6 B8), unlike RANGE?'s UTF-8:
         * "-0.0001M\xa6\xb8" observed. The K prefix is assumed capital, as
         * RANGE? has it. */
        const char *pfx = u[0] == 'k' ? "K" : u[0] == 'M' ? "M" : "";
        snprintf(unit, sizeof unit, "%s\xa6\xb8", pfx);
    } else if (a->fn == MEAS_CAP) {
        snprintf(unit, sizeof unit, " %s", u);                           /* stock: "0.000 uF" */
    } else {
        size_t j = 0;
        for (size_t i = 0; u[i] && j < sizeof unit - 1; i++) if (u[i] != ' ') unit[j++] = u[i];
        unit[j] = 0;                                                     /* "mVDC", "Hz" */
    }
    snprintf(out, sizeof out, "%s%.*s%s%s", neg ? "-" : "", pad, "000000", num + neg, unit);
    if (a->dual && a->dual_fn == MEAS_FREQ)
        scpi_replyf(s, "%s,%.3fHZ", out, a->sec_have ? a->sec_value : 0.0);   /* stock: "...,14.603HZ" */
    else if (a->dual) {                                                     /* B23: "1.2345VDC,0.2300VAC" */
        char v[24], u[16], uu[16];
        app_dual_sec_text(a, v, sizeof v, u, sizeof u);
        size_t j = 0;
        for (size_t i = 0; u[i] && j < sizeof uu - 1; i++) if (u[i] != ' ') uu[j++] = u[i];
        uu[j] = 0;
        scpi_replyf(s, "%s,%s%s", out, a->sec_over ? "overload" : v, a->sec_over ? "" : uu);
    } else scpi_reply(s, out);
}

static void h_auto(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) {                     /* stock: 0 in frequency, period, temperature */
        bool none = a->fn == MEAS_FREQ || a->fn == MEAS_TEMP;
        scpi_reply(s, a->autorange && !none ? "1" : "0");
        return;
    }
    bool on = true;
    if (c->argc && !scpi_parse_bool(c->argv[0], &on)) { scpi_error(s, SCPI_E_PARAM); return; }
    a->autorange = on;
    a->dirty = true;
}

static void h_rate(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    static const char rates[] = "SMF";
    if (c->query) { char r[2] = { rates[a->rate], 0 }; scpi_reply(s, r); return; }
    if (!c->argc || strlen(c->argv[0]) != 1) { scpi_error(s, SCPI_E_PARAM); return; }
    if (a->fn >= MEAS_CONT) { scpi_error(s, SCPI_E_SETTINGS); return; }   /* stock: not for 6..10 */
    const char *p = strchr(rates, c->argv[0][0] & ~0x20);
    if (!p) { scpi_error(s, SCPI_E_PARAM); return; }
    a->rate = (uint8_t)(p - rates);
    app_apply_prefs(a);                 /* B11: the board's AD1 speed follows */
}

static void h_calc_func(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) {
        /* stock V4.3.0: NULL, AVERage, DBM, DB; no reply at all when off */
        const char *n = a->rel ? "NULL" : a->math == MATH_STATS ? "AVERage"
                      : a->math == MATH_DBM ? (a->db_mode ? "DB" : "DBM") : NULL;
        if (n) scpi_reply(s, n);
        return;
    }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    const char *f = c->argv[0];
    if (scpi_match_word(f, "NULL")) {
        if (!a->have || a->overload) { scpi_error(s, SCPI_E_SETTINGS); return; }
        a->rel = true; a->rel_value = a->value;
    } else if (scpi_match_word(f, "AVERage")) {
        a->math = MATH_STATS;                 /* fresh statistics, as the MATH key does */
        a->st_min = INFINITY; a->st_max = -INFINITY; a->st_sum = 0; a->st_n = 0;
    } else if (scpi_match_word(f, "DB") || scpi_match_word(f, "DBM")) {
        if (a->fn != MEAS_DCV && a->fn != MEAS_ACV) { scpi_error(s, SCPI_E_SETTINGS); return; }
        a->math = MATH_DBM;
        a->db_mode = scpi_match_word(f, "DB");
    } else { scpi_error(s, SCPI_E_PARAM); return; }
    a->dirty = true;
}

static void h_calc_state(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, (a->rel || a->math != MATH_OFF) ? "1" : "0"); return; }
    bool on;
    if (!c->argc || !scpi_parse_bool(c->argv[0], &on)) { scpi_error(s, SCPI_E_PARAM); return; }
    if (!on) { a->rel = false; a->math = MATH_OFF; a->dirty = true; }
}

static void h_null_offset(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%7.2f", a->rel ? a->rel_value : 0.0); return; }   /* stock: "   0.00" */
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v)) { scpi_error(s, SCPI_E_PARAM); return; }
    a->rel = true; a->rel_value = v; a->dirty = true;
}

static void h_db_ref(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    /* stock V4.3.0 answers CALC:DB:REF? with the dBm reference impedance,
     * "%05d" ("00050"); the dB reference itself has no query there. */
    if (c->query) { scpi_replyf(s, "%05u", meas_dbm_rref[a->rref_idx]); return; }
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v) || fabs(v) > 200) { scpi_error(s, SCPI_E_PARAM); return; }
    a->db_ref_centi = (int32_t)lround(v * 100.0);
    a->dirty = true;
}

static void h_dbm_ref(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%05u", meas_dbm_rref[a->rref_idx]); return; }   /* stock: silent */
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v)) { scpi_error(s, SCPI_E_PARAM); return; }
    for (int i = 0; i < MEAS_NRREF; i++)
        if (fabs(meas_dbm_rref[i] - v) < 0.5) { a->rref_idx = i; a->dirty = true; return; }
    scpi_error(s, SCPI_E_PARAM);
}

static void stat_q(scpi_t *s, const scpi_call_t *c, int which)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    if (!c->query) return;
    /* stock V4.3.0: "%f" ("-0.000109"), "nan" before any reading; COUN? is
     * ours (stock never answers it) */
    bool on = a->math == MATH_STATS;          /* stock: nan unless statistics are on */
    if ((!on || !a->st_n) && which != 3) { scpi_reply(s, "nan"); return; }
    switch (which) {
    case 0: scpi_replyf(s, "%f", a->st_min); break;
    case 1: scpi_replyf(s, "%f", a->st_max); break;
    case 2: scpi_replyf(s, "%f", a->st_sum / a->st_n); break;
    default: scpi_replyf(s, "%lu", (unsigned long)a->st_n); break;
    }
}
static void h_min(scpi_t *s, const scpi_call_t *c)   { stat_q(s, c, 0); }
static void h_max(scpi_t *s, const scpi_call_t *c)   { stat_q(s, c, 1); }
static void h_avg(scpi_t *s, const scpi_call_t *c)   { stat_q(s, c, 2); }
static void h_count(scpi_t *s, const scpi_call_t *c) { stat_q(s, c, 3); }

static void h_clear(scpi_t *s, const scpi_call_t *c)
{
    (void)c;
    app_t *a = APP(s);
    a->st_min = INFINITY; a->st_max = -INFINITY; a->st_sum = 0; a->st_n = 0;
    trace_init(&a->trace);
    a->dirty = true;
}

static void h_beep(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, a->beeper ? "NO" : "OFF"); return; }   /* sic: stock says NO for on */
    bool on;
    if (!c->argc || !scpi_parse_bool(c->argv[0], &on)) { scpi_error(s, SCPI_E_PARAM); return; }
    a->beeper = on;
}

/* Ours (B57): beeper pitch, volume and style, and playing a sound, for
 * setting it up by ear or measuring it on the bench. */
static void h_beep_freq(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%u", sound_pitch(&a->snd)); return; }
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v)) { scpi_error(s, SCPI_E_PARAM); return; }
    if (v < SND_PITCH_MIN || v > SND_PITCH_MAX) { scpi_error(s, SCPI_E_PARAM); return; }
    a->snd.pitch_hz = (uint16_t)lround(v);
    a->dirty = true;
}

static void h_beep_vol(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%u", a->snd.volume); return; }
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v)) { scpi_error(s, SCPI_E_PARAM); return; }
    if (v < SND_VOL_MIN || v > 100) { scpi_error(s, SCPI_E_PARAM); return; }
    a->snd.volume = (uint8_t)lround(v);
    a->dirty = true;
}

static void h_beep_style(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, snd_style_name[a->snd.style]); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    for (int i = 0; i < SND_NSTYLE; i++)
        if (scpi_match_word(c->argv[0], snd_style_name[i])) { sound_style(&a->snd, (snd_style_t)i); a->dirty = true; return; }
    scpi_error(s, SCPI_E_PARAM);
}

/* Envelope settings, any value in range (the menu steps through a list).
 * Setting one makes the style CUSTOM, as the menu does. */
static void beep_env(scpi_t *s, const scpi_call_t *c, int which, int lo, int hi)
{
    app_t *a = APP(s);
    int cur[] = { a->snd.attack_ms, a->snd.release_ms, a->snd.glide_pct, a->snd.click_ms };
    if (c->query) { scpi_replyf(s, "%d", cur[which]); return; }
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v) || v < lo || v > hi) { scpi_error(s, SCPI_E_PARAM); return; }
    int n = (int)lround(v);
    switch (which) {
    case 0: a->snd.attack_ms = (uint8_t)n; break;
    case 1: a->snd.release_ms = (uint16_t)n; break;
    case 2: a->snd.glide_pct = (int8_t)n; break;
    default: a->snd.click_ms = (uint8_t)n; break;
    }
    if (which < 3) a->snd.style = SND_CUSTOM;
    a->dirty = true;
}
static void h_beep_att(scpi_t *s, const scpi_call_t *c)  { beep_env(s, c, 0, 0, 50); }
static void h_beep_rel(scpi_t *s, const scpi_call_t *c)  { beep_env(s, c, 1, 0, 300); }
static void h_beep_glid(scpi_t *s, const scpi_call_t *c) { beep_env(s, c, 2, -50, 50); }

static void h_beep_patt(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, snd_pat_name[a->snd.pattern]); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    for (int i = 0; i < SND_NPAT; i++)
        if (scpi_match_word(c->argv[0], snd_pat_name[i])) {
            a->snd.pattern = (snd_pat_t)i;
            a->snd.style = SND_CUSTOM;
            a->dirty = true;
            return;
        }
    scpi_error(s, SCPI_E_PARAM);
}
static void h_beep_clic(scpi_t *s, const scpi_call_t *c) { beep_env(s, c, 3, 10, 80); }

static void h_beep_play(scpi_t *s, const scpi_call_t *c)
{
    static const char *const ev[] = { "CLICK", "CONT", "DIODE", "HOLD", "TEST", "ALARM" };
    app_t *a = APP(s);
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    snd_event_t e = SND_TEST;
    if (c->argc) {
        int i = 0;
        while (i < SND_COUNT && !scpi_match_word(c->argv[0], ev[i])) i++;
        if (i == SND_COUNT || i == SND_CONT) { scpi_error(s, SCPI_E_PARAM); return; }
        e = (snd_event_t)i;
    }
    if (!a->beeper) { scpi_error(s, SCPI_E_SETTINGS); return; }
    app_sound(a, e);
}

/* Ours (B45): terminal mode for typing by hand (echo, CR, editing). */
static void h_terminal(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, a->terminal ? "1" : "0"); return; }
    bool on;
    if (!c->argc || !scpi_parse_bool(c->argv[0], &on)) { scpi_error(s, SCPI_E_PARAM); return; }
    a->terminal = on;
    s->terminal = on;
    a->dirty = true;
}

/* Ours (B31, docs/user-calibration.md): the owner's calibration on the
 * current function and range (select it with CONF:<fn> <range>, manual).
 * ZERO and GAIN start a step that averages 16 readings; STAT? reports it. */
static void ucal_start(scpi_t *s, bool gain, double target)
{
    int r = app_ucal_start(APP(s), gain, target);
    if (r) scpi_error(s, SCPI_E_SETTINGS);
}

static void h_ucal_zero(scpi_t *s, const scpi_call_t *c)
{
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    ucal_start(s, false, 0);
}

static void h_ucal_gain(scpi_t *s, const scpi_call_t *c)
{
    double v;
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    if (!c->argc || !scpi_parse_number(c->argv[0], &v)) { scpi_error(s, SCPI_E_PARAM); return; }
    ucal_start(s, true, v);
}

static void h_ucal_stat(scpi_t *s, const scpi_call_t *c)
{
    static const char *const st[] = { "IDLE", "BUSY", "DONE", "ERR" };
    app_t *a = APP(s);
    need_query(s, c, true);
    if (a->cal_job.st == UCAL_JOB_ERR) scpi_replyf(s, "ERR,%d", a->cal_job.err);
    else scpi_reply(s, st[a->cal_job.st]);
}

static void h_ucal(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    int r = a->range[a->fn];
    if (!ucal_applies(a->fn) || r >= UCAL_NRANGE) { scpi_reply(s, "0,0"); return; }
    scpi_replyf(s, "%ld,%ld", (long)a->ucal.slot[a->fn][r].g_ppm, (long)a->ucal.slot[a->fn][r].o_ppm);
}

/* CAL:FACT:LIST? (B32): every factory slot, "DCV 50 mV,1.021573,-14841,OK;..."
 * (function and range, gain, zero as stored, OK or BAD). */
static void h_fcal_list(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    if (!c->query) return;
    const meas_cal_t *cal = a->p->cal;
    if (!cal || !s->write) { scpi_reply(s, "NONE"); return; }
    for (int i = 0; i < app_cal_rows(); i++) {            /* streamed: one slot at a time */
        char l[24], v[40], b[64];
        meas_fn_t fn; int r;
        app_cal_row(a, i, &fn, &r, l, sizeof l, v, sizeof v);
        int k = snprintf(b, sizeof b, "%s%s,%.6f,%ld,%s", i ? ";" : "", l,
                         cal->coef[fn][r] / 1e6, (long)cal->zero[fn][r],
                         meas_cal_slot_ok(cal, fn, r) ? "OK" : "BAD");
        if (k > 0) s->write(s->ctx, b, (size_t)(k < (int)sizeof b ? k : (int)sizeof b - 1));
    }
    s->write(s->ctx, "\r\n", 2);
}

static void h_ucal_list(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    need_query(s, c, true);
    char line[512];
    size_t n = 0;
    line[0] = 0;
    for (int f = 0; f < MEAS_NFUNC; f++)
        for (int r = 0; r < UCAL_NRANGE; r++) {
            const ucal_slot_t *u = &a->ucal.slot[f][r];
            if (!u->g_ppm && !u->o_ppm) continue;
            int k = snprintf(line + n, sizeof line - n, "%s%s/%d:%ld:%ld", n ? ";" : "", fn_name[f], r + 1,
                             (long)u->g_ppm, (long)u->o_ppm);
            if (k < 0 || (size_t)k >= sizeof line - n) break;
            n += (size_t)k;
        }
    scpi_reply(s, n ? line : "NONE");
}

static void h_ucal_clear(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_error(s, SCPI_E_QUERY); return; }
    if (c->argc && scpi_match_word(c->argv[0], "ALL")) ucal_clear_all(&a->ucal);
    else if (c->argc) { scpi_error(s, SCPI_E_PARAM); return; }
    else ucal_clear(&a->ucal, a->fn, a->range[a->fn]);
    if (ucal_save(&a->ucal)) scpi_error(s, SCPI_E_SETTINGS);
    a->dirty = true;
}

static void h_remote(scpi_t *s, const scpi_call_t *c) { (void)c; APP(s)->remote = true;  APP(s)->dirty = true; }
static void h_local(scpi_t *s, const scpi_call_t *c)  { (void)c; APP(s)->remote = false; APP(s)->dirty = true; }

static void h_temp_type(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, a->range[MEAS_TEMP] == 0 ? "KITS90" : a->range[MEAS_TEMP] == 1 ? "PT100" : "PT1000"); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    if (scpi_match_word(c->argv[0], "KITS90")) a->range[MEAS_TEMP] = 0;
    else if (scpi_match_word(c->argv[0], "PT100")) a->range[MEAS_TEMP] = 1;
    else if (scpi_match_word(c->argv[0], "PT1000")) a->range[MEAS_TEMP] = 2;
    else { scpi_error(s, SCPI_E_PARAM); return; }
    if (a->fn == MEAS_TEMP) app_select(a, MEAS_TEMP);
}

static void h_temp_unit(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    static const char *u[] = { "C", "F", "K" };
    if (c->query) { scpi_reply(s, u[a->temp_unit]); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    for (int i = 0; i < 3; i++)
        if (scpi_match_word(c->argv[0], u[i])) { a->temp_unit = (meas_tunit_t)i; a->dirty = true; return; }
    scpi_error(s, SCPI_E_PARAM);
}

/* Ours (B26): the type-K EMF as measured, mV, and the cold junction, C.
 * NaN as 9.91E+37 when there is none (not type K, or no reading yet). */
static void h_tc_emf(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (!c->query) { scpi_error(s, SCPI_E_COMMAND); return; }
    bool ok = a->fn == MEAS_TEMP && a->range[MEAS_TEMP] == 0 && isfinite(a->tc_emf_mv);
    scpi_replyf(s, "%E", ok ? a->tc_emf_mv : 9.91E+37);
}

static void h_tc_cj(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (!c->query) { scpi_error(s, SCPI_E_COMMAND); return; }
    scpi_replyf(s, "%E", a->acq.have_cj ? a->acq.t_cj : 9.91E+37);
}

/* Ours (B27): PT100 lead resistance, ohms, or NULL to take it from the
 * reading now (PT100 selected, the probe leads shorted at the sensor end,
 * so the meter sees only the leads: under 10 ohm). */
static void h_rtd_lead(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%.3f", a->rtd_lead_mohm / 1000.0); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    double v;
    if (scpi_match_word(c->argv[0], "NULL")) {
        if (a->fn != MEAS_TEMP || a->range[MEAS_TEMP] < 1 || !isfinite(a->rtd_ohm) || a->rtd_ohm >= 10.0) {
            scpi_error(s, SCPI_E_SETTINGS);
            return;
        }
        v = a->rtd_ohm < 0 ? 0 : a->rtd_ohm;
    } else if (!scpi_parse_number(c->argv[0], &v) || v < 0 || v > APP_RTD_LEAD_MAX_MOHM / 1000.0) {
        scpi_error(s, SCPI_E_PARAM);
        return;
    }
    a->rtd_lead_mohm = (uint16_t)lround(v * 1000.0);
    a->acq.rtd_lead_ohm = a->rtd_lead_mohm / 1000.0;
    a->dirty = true;
}

static void h_cont_thr(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_replyf(s, "%u", a->cont_threshold); return; }
    double v;
    if (!c->argc || !scpi_parse_number(c->argv[0], &v) || v < 1 || v > 1000) { scpi_error(s, SCPI_E_PARAM); return; }
    a->cont_threshold = (uint16_t)lround(v);
}

/* ---- recording: our LOG subsystem (stock's record commands are not recovered) -- */

static void h_log_start(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, false);
    if (app_rec_start(APP(s))) scpi_error(s, SCPI_E_SETTINGS);
}

static void h_log_stop(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, false);
    app_rec_stop(APP(s));
}

static void h_log_state(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, true);
    scpi_reply(s, APP(s)->log.active ? "1" : "0");
}

/* Ours (B35): what a recording does when the log is full. */
static void h_log_full(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) { scpi_reply(s, a->rec_roll ? "ROLL" : "STOP"); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    if (a->log.active) { scpi_error(s, SCPI_E_SETTINGS); return; }
    if (scpi_match_word(c->argv[0], "ROLL")) a->rec_roll = true;
    else if (scpi_match_word(c->argv[0], "STOP")) a->rec_roll = false;
    else { scpi_error(s, SCPI_E_PARAM); return; }
    a->dirty = true;
}

static void h_log_points(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    scpi_replyf(s, "%lu", (unsigned long)(d->have ? d->count : 0));
}

static void h_log_capacity(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    scpi_replyf(s, "%lu", (unsigned long)(d->io ? dlog_capacity(d) : 0));
}

/* LOG:INTerval <seconds | MANual>: the nearest interval the meter offers.
 * The query replies the seconds, or 0 in manual mode. */
static void h_log_interval(scpi_t *s, const scpi_call_t *c)
{
    app_t *a = APP(s);
    if (c->query) {
        scpi_replyf(s, "%g", a->rec_manual ? 0.0 : app_rec_interval_ms[a->rec_int_idx] / 1000.0);
        return;
    }
    if (a->log.active) { scpi_error(s, SCPI_E_SETTINGS); return; }
    if (!c->argc) { scpi_error(s, SCPI_E_MISSING_PARAM); return; }
    if (scpi_match_word(c->argv[0], "MANual")) { a->rec_manual = true; a->dirty = true; return; }
    double v;
    if (!scpi_parse_number(c->argv[0], &v) || v <= 0) { scpi_error(s, SCPI_E_PARAM); return; }
    int best = 0;
    for (int i = 1; i < APP_NREC_INT; i++)
        if (fabs(app_rec_interval_ms[i] / 1000.0 - v) < fabs(app_rec_interval_ms[best] / 1000.0 - v)) best = i;
    a->rec_int_idx = (uint8_t)best;
    a->rec_manual = false;
    a->dirty = true;
}

static void h_log_func(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    if (!d->have || d->fn >= MEAS_NFUNC) { scpi_reply(s, "\"NONE\""); return; }
    if (d->tag & 0x20) { scpi_reply(s, "\"POW\""); return; }   /* B21: a power session */
    scpi_replyf(s, "\"%s\"", fn_name[d->fn]);
}

/* ---- B33: the real-time clock --------------------------------------------- */

static bool three_ints(const scpi_call_t *c, int v[3])
{
    if (c->argc != 3) return false;
    for (int i = 0; i < 3; i++) {
        double d;
        if (!scpi_parse_number(c->argv[i], &d) || d != (int)d) return false;
        v[i] = (int)d;
    }
    return true;
}

/* SYST:DATE y,m,d and SYST:TIME h,m,s (SCPI 1999 forms): each keeps the
 * other half. Queries answer 0,0,0 while the clock is not set. */
static void clock_field(scpi_t *s, const scpi_call_t *c, bool date)
{
    const platform_t *p = APP(s)->p;
    if (!p->clock_now || !p->clock_set) { scpi_error(s, SCPI_E_SETTINGS); return; }
    uint32_t t = p->clock_now();
    clock_civil_t cv = { 2000, 1, 1, 0, 0, 0 };
    if (t) clock_to_civil(t, &cv);
    if (c->query) {
        if (!t) { scpi_reply(s, "0,0,0"); return; }
        if (date) scpi_replyf(s, "%d,%d,%d", cv.year, cv.month, cv.day);
        else scpi_replyf(s, "%d,%d,%d", cv.hour, cv.min, cv.sec);
        return;
    }
    int v[3];
    if (!three_ints(c, v)) { scpi_error(s, SCPI_E_PARAM); return; }
    if (date) { cv.year = v[0]; cv.month = v[1]; cv.day = v[2]; }
    else { cv.hour = v[0]; cv.min = v[1]; cv.sec = v[2]; }
    uint32_t n = clock_from_civil(&cv);
    if (!n) { scpi_error(s, SCPI_E_PARAM); return; }
    p->clock_set(n);
    APP(s)->dirty = true;
}
static void h_sys_date(scpi_t *s, const scpi_call_t *c) { clock_field(s, c, true); }
static void h_sys_time(scpi_t *s, const scpi_call_t *c) { clock_field(s, c, false); }

/* LOG:TIME?: when the session started, ISO 8601, or NONE without a clock. */
static void h_log_time(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    if (!d->have || !d->start_time) { scpi_reply(s, "NONE"); return; }
    char b[24];
    clock_fmt(b, sizeof b, d->start_time, 'T');
    scpi_reply(s, b);
}

static void h_log_point(scpi_t *s, const scpi_call_t *c)
{
    need_query(s, c, false);
    if (!APP(s)->rec_manual || app_rec_point(APP(s))) scpi_error(s, SCPI_E_SETTINGS);
}

/* LOG:DATA? [start[,count]]: one line of comma-separated %E values, streamed
 * so it needs no buffer. Overload reads 1E+9, as in MEAS?. */
static void h_log_data(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    uint32_t n = d->have ? d->count : 0;
    double start = 0, count = n;
    if ((c->argc > 0 && (!scpi_parse_number(c->argv[0], &start) || start < 0)) ||
        (c->argc > 1 && (!scpi_parse_number(c->argv[1], &count) || count < 0))) {
        scpi_error(s, SCPI_E_PARAM);
        return;
    }
    uint32_t i0 = start < n ? (uint32_t)start : n;
    uint32_t i1 = count < n - i0 ? i0 + (uint32_t)count : n;
    char buf[20];
    for (uint32_t i = i0; i < i1; i++) {
        float f = 0;
        dlog_get(d, i, &f);
        int len = snprintf(buf, sizeof buf, "%s%E", i > i0 ? "," : "", isfinite(f) ? (double)f : 1E+9);
        s->write(s->ctx, buf, (size_t)len);
    }
    s->write(s->ctx, "\r\n", 2);
}

/* INT:RECord?: the log in stock's download format, so OWON's DMMEasyControl
 * and owners' scripts read ours unchanged. Observed on V4.3.0 (11 points):
 *   start:11,DCV,0.0039,VDC,DCV,1.7859,VDC,...,end\r\n
 * one line; per point function, value as displayed, unit. Only DCV/VDC is
 * confirmed; the other names follow the pattern (docs/scpi.md). Our log keeps
 * SI values, so each is formatted on the range that would hold it. */
static const char *const rec_fn[MEAS_NFUNC] = {
    "DCV", "ACV", "DCI", "ACI", "RES", "RES", "CONT", "DIOD", "CAP", "FREQ", "TEMP",
};

static void rec_point(char *out, size_t cap, uint8_t fn, uint8_t tag, float f)
{
    if (!isfinite(f)) { snprintf(out, cap, "%s,overload,", (tag & 0x20) ? "POW" : rec_fn[fn]); return; }
    double v = f;
    char num[24], unit[16];
    if (tag & 0x20) { snprintf(out, cap, "POW,%.6g,W,", v); return; }   /* B21: ours; stock had none */
    if (tag & 0x18) {                                   /* dBm / dB sessions */
        snprintf(out, cap, "%s,%.2f,%s,", rec_fn[fn], v, (tag & 0x10) ? "dB" : "dBm");
        return;
    }
    if (fn == MEAS_TEMP) {
        static const char *const tu[] = { "C", "F", "K" };
        snprintf(out, cap, "TEMP,%.1f,%s,", v, tu[(tag & 3) % 3]);
        return;
    }
    int code;
    if (fn == MEAS_FREQ) code = meas_freq_code(v, (tag & 4) != 0);
    else {
        int hi = meas_nranges((meas_fn_t)fn) - 1;      /* the smallest range that holds it */
        for (code = 0; code < hi; code++)
            if (!readout_overload((meas_fn_t)fn, code, v)) break;
    }
    double shown = (fn == MEAS_FREQ && (tag & 4)) ? (v != 0.0 ? 1.0 / v : 0.0) : v;
    readout_value(num, sizeof num, (meas_fn_t)fn, code, shown, false);
    const char *u = readout_unit((meas_fn_t)fn, code, (tag & 4) != 0);
    size_t j = 0;
    for (size_t i = 0; u[i] && j < sizeof unit - 1; i++) if (u[i] != ' ') unit[j++] = u[i];
    unit[j] = 0;
    snprintf(out, cap, "%s,%s,%s,", fn == MEAS_FREQ && (tag & 4) ? "PER" : rec_fn[fn], num, unit);
}

static void h_int_record(scpi_t *s, const scpi_call_t *c)
{
    const dlog_t *d = &APP(s)->log;
    need_query(s, c, true);
    if (!c->query) return;
    uint32_t n = d->have ? d->count : 0;
    char buf[64];
    int len = snprintf(buf, sizeof buf, "start:%lu,", (unsigned long)n);
    s->write(s->ctx, buf, (size_t)len);
    for (uint32_t i = 0; i < n; i++) {
        float f = 0;
        dlog_get(d, i, &f);
        rec_point(buf, sizeof buf, d->fn < MEAS_NFUNC ? d->fn : 0, d->tag, f);
        s->write(s->ctx, buf, strlen(buf));
    }
    s->write(s->ctx, "end\r\n", 5);
}

static const scpi_cmd_t cmds[] = {
    { "*IDN", h_idn }, { "*RST", h_rst }, { "*CLS", h_cls },
    { "SYSTem:ERRor[:NEXT]", h_err },
    { "SYSTem:BEEPer[:STATe]", h_beep }, { "SYSTem:TERMinal", h_terminal },
    { "CALibration:USER:ZERO", h_ucal_zero }, { "CALibration:USER:GAIN", h_ucal_gain },
    { "CALibration:USER:STATe", h_ucal_stat }, { "CALibration:USER", h_ucal },
    { "CALibration:USER:LIST", h_ucal_list }, { "CALibration:USER:CLEar", h_ucal_clear },
    { "CALibration:FACTory:LIST", h_fcal_list },
    { "SYSTem:BEEPer:FREQuency", h_beep_freq }, { "SYSTem:BEEPer:VOLume", h_beep_vol },
    { "SYSTem:BEEPer:STYLe", h_beep_style }, { "SYSTem:BEEPer:PLAY", h_beep_play },
    { "SYSTem:BEEPer:ATTack", h_beep_att }, { "SYSTem:BEEPer:RELease", h_beep_rel },
    { "SYSTem:BEEPer:GLIDe", h_beep_glid }, { "SYSTem:BEEPer:CLICk", h_beep_clic },
    { "SYSTem:BEEPer:PATTern", h_beep_patt },
    { "SYSTem:REMote", h_remote }, { "SYSTem:LOCal", h_local },
    { "SYSTem:DATE", h_sys_date }, { "SYSTem:TIME", h_sys_time },
    { "MEASure", h_meas }, { "MEASure:SHOW", h_meas_show }, { "MEASure2:SHOW", h_meas2_show },
    { "MEASure:POWer", h_meas_pow }, { "MEASure:ENERgy", h_meas_ener },
    { "MEASure:CHARge", h_meas_char }, { "MEASure:ENERgy:TIME", h_meas_etime },
    { "CALCulate:ENERgy:CLEar", h_ener_clear },
    { "CONFigure:VOLTage[:DC]", h_conf_vdc }, { "CONFigure:VOLTage:AC", h_conf_vac },
    { "CONFigure:CURRent[:DC]", h_conf_idc }, { "CONFigure:CURRent:AC", h_conf_iac },
    { "CONFigure:RESistance", h_conf_res }, { "CONFigure:FRESistance", h_conf_fres },
    { "CONFigure:CONTinuity", h_conf_cont }, { "CONFigure:DIODe", h_conf_diod },
    { "CONFigure:CAPacitance", h_conf_cap },
    { "CONFigure:FREQuency", h_conf_freq }, { "CONFigure:PERiod", h_conf_per },
    { "CONFigure:TEMPerature", h_conf_temp },
    { "FUNCtion", h_func },
    { "RANGE", h_range }, { "AUTO", h_auto }, { "RATE", h_rate },
    { "CALCulate:FUNCtion", h_calc_func }, { "CALCulate:STATe", h_calc_state },
    { "CALCulate:NULL:OFFSet", h_null_offset },
    { "CALCulate:DB:REFerence", h_db_ref }, { "CALCulate:DBM:REFerence", h_dbm_ref },
    { "CALCulate:AVERage:MINimum", h_min }, { "CALCulate:AVERage:MAXimum", h_max },
    { "CALCulate:AVERage:AVERage", h_avg }, { "CALCulate:AVERage:COUNt", h_count },
    { "CALCulate:CLEar", h_clear },
    { "TEMPerature:RTD:TYPe", h_temp_type }, { "TEMPerature:RTD:UNIT", h_temp_unit },
    { "TEMPerature:RTD:LEAD", h_rtd_lead },
    { "CONTinuity:THREshold", h_cont_thr },
    { "TEMPerature:TCouple:EMF", h_tc_emf }, { "TEMPerature:TCouple:CJUNction", h_tc_cj },
    { "LOG:STARt", h_log_start }, { "LOG:STOP", h_log_stop }, { "LOG:STATe", h_log_state },
    { "LOG:FULL", h_log_full },
    { "LOG:POINts", h_log_points }, { "LOG:CAPacity", h_log_capacity },
    { "LOG:INTerval", h_log_interval }, { "LOG:FUNCtion", h_log_func },
    { "LOG:TRIGger", h_log_point }, { "LOG:DATA", h_log_data },
    { "LOG:TIME", h_log_time },
    { "INTernal:RECord", h_int_record },
};

static void serial_out(void *ctx, const char *s, size_t n)
{
    app_t *a = ctx;
    if (a->p->serial_write) a->p->serial_write(s, n);
}

void app_scpi_init(app_t *a)
{
    scpi_init(&a->scpi, cmds, (int)(sizeof cmds / sizeof cmds[0]), a, serial_out);
}
