/* Acquisition state machine. See acquire.h. */
#include "acquire.h"

#include <math.h>

#include "hy3131.h"
#include "meas_temp.h"

static uint32_t le24(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
}

static uint8_t need_flag(meas_fn_t fn)
{
    switch (fn) {
    /* RMSF, not AD2F: AD2F comes at the AD2 rate and the RMS sum is then
     * stale (on the meter, 2026-11-13: the same sum every 7 ms). */
    case MEAS_ACV: case MEAS_ACI:  return HY_F_RMS;
    case MEAS_CAP: case MEAS_FREQ: return HY_F_CT;
    default:                       return HY_F_AD1;
    }
}

void acq_init(acq_t *a)
{
    *a = (acq_t){ .fn = MEAS_DCV, .range = 2, .st = ACQ_IDLE, .fn2 = MEAS_NFUNC,
                  .cj_period = ACQ_CJ_PERIOD_MS };
}

void acq_select(acq_t *a, meas_fn_t fn, int range)
{
    a->fn = fn;
    a->range = range;
    a->st = ACQ_IDLE;
    a->since2 = 0;
    a->cj_period = ACQ_CJ_PERIOD_MS;     /* track closely again after a change */
}

/* DC volts and current: their discards are the filter's, which scale with
 * the rate. Ohms too since 2026-11-14: its source settles over a time on
 * the high ranges, which the app's hunt waits out by requiring two fast
 * readings that agree (app.c, range_on_sample). */
bool acq_fast_ok(meas_fn_t fn)
{
    return fn == MEAS_DCV || fn == MEAS_DCI || fn == MEAS_OHM2;
}

void acq_set_fast(acq_t *a, bool on)
{
    if (on == a->fast) return;
    a->fast = on;
    if (acq_fast_ok(a->fn)) a->st = ACQ_IDLE;       /* reconfigure at the new rate */
}

void acq_set_secondary(acq_t *a, meas_fn_t fn2, int range2)
{
    a->fn2 = fn2;
    a->range2 = range2;
    a->since2 = 0;
    a->st = ACQ_IDLE;
}

static void ct_restart(meas_fn_t fn);

static void configure(acq_t *a, const platform_t *p, meas_fn_t fn, int range,
                      afe_aux_t aux, acq_state_t next)
{
    if (aux == AFE_AUX_NONE && a->fast && fn == a->fn && next == ACQ_SETTLE && acq_fast_ok(fn))
        aux = AFE_AUX_FAST;
    if (p->afe_configure) p->afe_configure(fn, range, aux);
    if ((fn == MEAS_CAP && !acq_cap_slope(fn, range)) || fn == MEAS_FREQ) ct_restart(fn);   /* the first gate gets the preset too */
    a->t0 = p->millis();
    a->ct_gate_at = a->t0;
    a->flags = 0;
    a->discard = (fn == MEAS_OHM2 || fn == MEAS_OHM4 || fn == MEAS_CONT || fn == MEAS_DIODE ||
                  (fn == MEAS_TEMP && range > 0))     /* PT100/PT1000: an ohms reading */
                 ? ACQ_DISCARD_OHM : ACQ_DISCARD;
    a->st = next;
}

/* Capacitance: the counter's done flag (INTF CTF) is sometimes never seen
 * on our meter (2026-11-13; the panel build logs "flag missed"), and the
 * counters then sit stopped for good. So a reading is also done when CTA
 * has not moved for 100 ms and at least one whole cycle was counted. */
static bool ct_stopped(acq_t *a, uint32_t now)
{
    if (now - a->ct_check_at < 100u) return false;
    a->ct_check_at = now;
    uint8_t c[6];
    hy3131_read_burst(HY_CTB, c, 6);                 /* CTB, then CTA */
    uint32_t ctb = le24(c), cta = le24(c + 3);
    bool stopped = cta == a->ct_prev && ctb > 0;
    a->ct_prev = cta;
    return stopped;
}

static bool ready(acq_t *a, uint8_t flag)
{
    a->flags |= hy3131_read8(HY_INTF);
    if (!(a->flags & flag)) return false;
    a->flags &= (uint8_t)~flag;
    return true;
}

/* Restart the counters, as stock does after every capacitance reading
 * (0x08007CBC): ENCTR off, the gate preset into CTA, ENCTR on. Without it
 * the counter gives one result and stops.
 *
 * Then read CTA back. On our meter (2026-11-13, docs/afe-table.md) the
 * gate otherwise ends after ~70 ms instead of the preset's ~1.2 s, every
 * time, and the readings are wrong (2-20x high, not linear in C); with the
 * read-back every gate is full length. Why is not known (the read's delay
 * before the first INTF poll is the likeliest); the read is what was
 * tested, so keep it. */
static void ct_restart(meas_fn_t fn)
{
    uint8_t r20 = hy3131_read8(HY_R20), ca[3];
    /* Gate presets: stock's 0xBF0000 for capacitance (0x08007CA8), and
     * 0xF00000 for frequency, which meas_freq() assumes (1048576 ticks). */
    static const uint8_t gate_cap[3] = { 0x00, 0x00, 0xbf }, gate_freq[3] = { 0x00, 0x00, 0xf0 };
    hy3131_write8(HY_R20, (uint8_t)(r20 & ~0x02u));
    hy3131_write_burst(HY_CTA, fn == MEAS_CAP ? gate_cap : gate_freq, 3);
    hy3131_write8(HY_R20, (uint8_t)(r20 | 0x02u));
    hy3131_read_burst(HY_CTA, ca, 3);
}

/* Read the registers for function fn and convert. */
static bool convert(acq_t *a, const platform_t *p, meas_fn_t fn, int range, acq_sample_t *out)
{
    const meas_cal_t *cal = p->cal;
    uint8_t b[5];
    out->aux = 0.0;
    out->emf_mv = NAN;
    out->rtd_ohm = NAN;
    out->overrange = false;
    out->secondary = false;

    switch (fn) {
    case MEAS_ACV: case MEAS_ACI: {
        hy3131_read_burst(HY_RMS_DATA, b, 5);
        uint64_t rms = 0;
        for (int i = 4; i >= 0; i--) rms = rms << 8 | b[i];
        out->value = meas_ac(cal, fn, range, rms, a->cal_mode);
        return true;
    }
    case MEAS_CAP: {
        if (range > 4) return false;     /* charge-slope ranges: docs/app.md */
        uint8_t cb[3], cc[3];
        hy3131_read_burst(HY_CTB, cb, 3);
        hy3131_read_burst(HY_CTC, cc, 3);
        out->value = meas_cap_counter(cal, range, le24(cb), le24(cc));
        (void)hy3131_read8(HY_INTF);           /* drop a late done flag: it would end the next gate */
        a->flags &= (uint8_t)~HY_F_CT;
        ct_restart(MEAS_CAP);
        return !isnan(out->value);
    }
    case MEAS_FREQ: {
        uint8_t ca[3], cb[3], cc[3];
        hy3131_read_burst(HY_CTA, ca, 3);
        hy3131_read_burst(HY_CTB, cb, 3);
        hy3131_read_burst(HY_CTC, cc, 3);
        out->value = meas_freq(le24(ca), le24(cb), le24(cc), &out->aux);
        (void)hy3131_read8(HY_INTF);           /* as capacitance: drop a late flag, restart */
        a->flags &= (uint8_t)~HY_F_CT;
        ct_restart(MEAS_FREQ);
        return true;
    }
    default:
        break;
    }

    hy3131_read_burst(HY_AD1_DATA, b, 3);
    int32_t ad1 = hy3131_sext24(le24(b));
    switch (fn) {
    case MEAS_DCV:   out->value = meas_dcv(cal, range, ad1); break;
    case MEAS_DCI:   out->value = meas_dci(cal, range, ad1); break;
    case MEAS_OHM2:
    case MEAS_OHM4:  out->value = meas_ohm(cal, fn, range, ad1, a->cal_mode); break;
    case MEAS_CONT:  out->value = meas_continuity(cal, ad1); break;
    case MEAS_DIODE: out->value = meas_diode(cal, ad1); break;
    case MEAS_TEMP:
        if (range == 0) {
            double emf_mv = meas_dcv(cal, 1, ad1) * 1e3;
            out->value = meas_tc_k_compensated(emf_mv, a->t_cj, &out->overrange);
            out->aux = a->t_cj;
            out->emf_mv = emf_mv;
        } else {
            /* PT100 on the 500 ohm range; PT1000 (B28) on 5 kohm, as a
             * PT100 at a tenth of the resistance. */
            bool k1 = range == 2;
            double r = meas_ohm(cal, MEAS_OHM2, k1 ? 1 : 0, ad1, a->cal_mode);
            out->rtd_ohm = r;
            out->value = meas_pt100_temp((r - a->rtd_lead_ohm) / (k1 ? 10.0 : 1.0), &out->overrange);
        }
        break;
    default:
        return false;
    }
    return true;
}

static double read_dcv_mv(const platform_t *p)
{
    uint8_t b[3];
    hy3131_read_burst(HY_AD1_DATA, b, 3);
    return meas_dcv(p->cal, 1, hy3131_sext24(le24(b))) * 1e3;
}

/* B31: the owner's correction on top of the factory one; raw keeps the
 * uncorrected value, which the calibration steps average. */
static void user_cal(const acq_t *a, meas_fn_t fn, int range, acq_sample_t *out)
{
    out->raw = out->value;
    if (a->ucal) out->value = ucal_apply(a->ucal, fn, range, out->value);
}

static const uint16_t acq_cap_rest_ms[2]   = { 1000u, 2500u };   /* range 5, 6 */
static const uint16_t acq_cap_charge_ms[2] = { 2000u, 3000u };

bool acq_timing_critical(const acq_t *a) { return acq_cap_slope(a->fn, a->range) && a->cap_charging; }

static void cap_start_charge(acq_t *a, const platform_t *p)
{
    configure(a, p, a->fn, a->range, AFE_AUX_CAP_CHARGE, ACQ_WAIT);
    a->discard = 1;                         /* the conversion straddling the switch */
    a->cap_charging = true;
    a->cap_full = false;
    a->cap_n = 0;
    a->cap_sk = a->cap_sv = a->cap_skk = a->cap_skv = 0.0;
}

/* The charge's rise per second by least squares over (conversion index,
 * volts). The index comes from the time since the first point, rounded to
 * whole conversions, so a late poll or a missed conversion doesn't bend it. */
static double cap_fit(const acq_t *a)
{
    double n = a->cap_n, den = n * a->cap_skk - a->cap_sk * a->cap_sk;
    if (a->cap_n < 2 || den <= 0.0) return NAN;
    double per_conv = (n * a->cap_skv - a->cap_sk * a->cap_sv) / den;
    return per_conv * 1000.0 / ACQ_CAP_CONV_MS;
}

/* Capacitance 5 / 50 mF: rest (phase A), charge (phase B), fit. */
static bool cap_slope_poll(acq_t *a, const platform_t *p, acq_sample_t *out, uint32_t now)
{
    int ri = a->range - 5;
    switch (a->st) {
    case ACQ_IDLE:                          /* phase A: rest */
        configure(a, p, a->fn, a->range, AFE_AUX_NONE, ACQ_SETTLE);
        a->cap_charging = false;
        a->cap_rest_v = INFINITY;           /* not seen yet */
        return false;
    case ACQ_SETTLE:
        /* Rest at least the phase's time, and until the capacitor is low
         * enough for a charge to give points (one left high from a charge
         * or from the circuit would clip at once). */
        if (ready(a, HY_F_AD1)) {
            uint8_t b[3];
            hy3131_read_burst(HY_AD1_DATA, b, 3);
            a->cap_rest_v = meas_cap_volts(hy3131_sext24(le24(b)));
        }
        if (now - a->t0 >= acq_cap_rest_ms[ri] &&
            (a->cap_rest_v < ACQ_CAP_START_V || now - a->t0 >= ACQ_CAP_REST_MAX_MS))
            cap_start_charge(a, p);
        return false;
    case ACQ_WAIT:
        break;
    default:
        a->st = ACQ_IDLE;
        return false;
    }
    if (ready(a, HY_F_AD1)) {
        uint8_t b[3];
        hy3131_read_burst(HY_AD1_DATA, b, 3);
        double v = meas_cap_volts(hy3131_sext24(le24(b)));
        if (a->discard) a->discard--;
        else if (v >= ACQ_CAP_FIT_MAX_V) a->cap_full = true;
        else if (!a->cap_full) {
            if (a->cap_n == 0) a->cap_t0 = now;
            double k = (double)((now - a->cap_t0 + ACQ_CAP_CONV_MS / 2u) / ACQ_CAP_CONV_MS);
            a->cap_n++;
            a->cap_sk += k; a->cap_sv += v; a->cap_skk += k * k; a->cap_skv += k * v;
        }
    }
    if (!a->cap_full && now - a->t0 < acq_cap_charge_ms[ri]) return false;

    /* The charge is over: back to rest, and report. Too few points before
     * AD1 clipped means the capacitor is small for this range: report 0, so
     * autoranging steps down. Clipped with none at all after a rest that
     * never got low (the 20 s limit): too high to say, rest and try again. */
    a->st = ACQ_IDLE;
    a->cap_charging = false;
    if (a->cap_full && a->cap_n == 0 && !(a->cap_rest_v < ACQ_CAP_START_V)) return false;
    out->aux = 0.0; out->emf_mv = NAN; out->rtd_ohm = NAN;
    out->overrange = false; out->secondary = false;
    double rate = cap_fit(a);
    if (a->cap_n < ACQ_CAP_MIN_PTS) out->value = a->cap_full ? 0.0 : NAN;
    else out->value = meas_cap_rate(p->cal, a->range, rate);
    user_cal(a, a->fn, a->range, out);
    return true;
}

/* B21: current with volts only where the board says the pair can alternate
 * (on our meter, where PA8 doesn't move: moving it opens the current path).
 * Checked here, under the app, so ranging can never slip a forbidden volts
 * read between current readings. */
static bool pair_ok(const acq_t *a, const platform_t *p)
{
    if (a->fn != MEAS_DCI || a->fn2 != MEAS_DCV || !p->vi_avail) return true;
    return p->vi_avail(a->range, a->range2);
}

bool acq_poll(acq_t *a, const platform_t *p, acq_sample_t *out)
{
    uint32_t now = p->millis();
    bool tc = (a->fn == MEAS_TEMP && a->range == 0);
    if (acq_cap_slope(a->fn, a->range)) return cap_slope_poll(a, p, out, now);

    switch (a->st) {
    case ACQ_IDLE:
        if (tc && (!a->have_cj || now - a->cj_at >= a->cj_period))
            configure(a, p, MEAS_DCV, 1, AFE_AUX_CJ1, ACQ_CJ1);
        else
            configure(a, p, a->fn, a->range, AFE_AUX_NONE, ACQ_SETTLE);
        return false;

    case ACQ_CJ1:
    case ACQ_CJ2:
        if (now - a->t0 < ACQ_CJ_SETTLE_MS || !ready(a, HY_F_AD1)) return false;
        if (a->st == ACQ_CJ1) {
            a->cj_v1 = read_dcv_mv(p);
            configure(a, p, MEAS_DCV, 1, AFE_AUX_CJ2, ACQ_CJ2);
        } else {
            double t = meas_cold_junction(a->cj_v1, read_dcv_mv(p), p->cal->cj_g) + a->cj_offset_c;
            bool steady = a->have_cj && fabs(t - a->t_cj) < ACQ_CJ_STABLE_C;
            if (a->cj_period < ACQ_CJ_PERIOD_MS) a->cj_period = ACQ_CJ_PERIOD_MS;
            a->cj_period = steady ? (a->cj_period * 2 > ACQ_CJ_PERIOD_MAX_MS ? ACQ_CJ_PERIOD_MAX_MS
                                                                               : a->cj_period * 2)
                                  : ACQ_CJ_PERIOD_MS;
            a->t_cj = t;
            a->have_cj = true;
            a->cj_count++;
            a->cj_at = now;
            configure(a, p, a->fn, a->range, AFE_AUX_NONE, ACQ_SETTLE);
        }
        return false;

    case ACQ_SETTLE:
        if (now - a->t0 < ACQ_SETTLE_MS) return false;
        a->st = ACQ_WAIT;
        /* fall through */
    case ACQ_WAIT:
        if (tc && (!a->have_cj || now - a->cj_at >= a->cj_period)) {
            a->st = ACQ_IDLE;
            return false;
        }
        if (!ready(a, need_flag(a->fn)) && !((a->fn == MEAS_CAP || a->fn == MEAS_FREQ) && ct_stopped(a, now))) {
            uint32_t lim = a->range >= 4 ? ACQ_CT_TIMEOUT_TOP_MS : ACQ_CT_TIMEOUT_MS;
            if (a->fn != MEAS_CAP || now - a->ct_gate_at < lim) return false;
            /* No cycle finished: far too big for this range. */
            *out = (acq_sample_t){ .value = NAN, .emf_mv = NAN, .rtd_ohm = NAN, .raw = NAN, .overrange = true };
            ct_restart(MEAS_CAP);
            a->ct_gate_at = now;
            a->discard = 0;
            return true;
        }
        if (!convert(a, p, a->fn, a->range, out)) return false;
        if (a->fn == MEAS_CAP) a->ct_gate_at = now;
        user_cal(a, a->fn, a->range, out);
        if (a->discard) { a->discard--; return false; }
        if (a->fn2 < MEAS_NFUNC && pair_ok(a, p)
            && ++a->since2 >= (a->sec_every ? a->sec_every : ACQ_DUAL_PRIMARY)) {
            a->since2 = 0;                          /* the secondary's turn next */
            configure(a, p, a->fn2, a->range2, AFE_AUX_NONE, ACQ_SEC_SETTLE);
        }
        return true;

    case ACQ_SEC_SETTLE:
        if (now - a->t0 < ACQ_SETTLE_MS) return false;
        a->st = ACQ_SEC_WAIT;
        /* fall through */
    case ACQ_SEC_WAIT:
        if (!ready(a, need_flag(a->fn2)) && !(a->fn2 == MEAS_FREQ && ct_stopped(a, now))) return false;
        if (!convert(a, p, a->fn2, a->range2, out)) return false;
        user_cal(a, a->fn2, a->range2, out);
        if (a->discard) { a->discard--; return false; }
        out->secondary = true;
        configure(a, p, a->fn, a->range, AFE_AUX_NONE, ACQ_SETTLE);   /* back to the primary */
        return true;
    }
    return false;
}
