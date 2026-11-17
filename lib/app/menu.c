/* See menu.h. */
#include "menu.h"
#include "clock.h"

#include <stdio.h>

#include "app.h"

const char *const menu_labels[MI_COUNT] = {
    [MI_BACKLIGHT] = "BACKLIGHT",
    [MI_DIM]       = "DIM AFTER",
    [MI_BEEPER]    = "BEEPER",
    [MI_BEEP_PITCH] = "BEEP PITCH",
    [MI_BEEP_VOL]  = "BEEP VOLUME",
    [MI_BEEP_STYLE] = "BEEP STYLE",
    [MI_BEEP_ATTACK] = "  ATTACK",
    [MI_BEEP_RELEASE] = "  RELEASE",
    [MI_BEEP_GLIDE] = "  GLIDE",
    [MI_BEEP_PATTERN] = "  PATTERN",
    [MI_CLICK_LEN] = "CLICK LENGTH",
    [MI_THRESHOLD] = "CONT THRESHOLD",
    [MI_DIODE_TH]  = "DIODE THRESHOLD",
    [MI_BARGRAPH]  = "BAR GRAPH",
    [MI_SPEED]     = "SPEED",
    [MI_TEMP_UNIT] = "TEMP UNIT",
    [MI_RTD_LEADS] = "PT100 LEADS",
    [MI_CJ_OFFSET] = "CJ OFFSET",
    [MI_DBM_REF]   = "dBm REFERENCE",
    [MI_THEME]     = "THEME",
    [MI_BAUD]      = "BAUD",
    [MI_TERMINAL]  = "TERMINAL MODE",
    [MI_DEFAULTS]  = "LOAD DEFAULTS",
    [MI_VERSION]   = "VERSION",
    [MI_CLOCK]     = "CLOCK",
    [MI_SERIAL]    = "SERIAL NUM",
    [MI_CAL]       = "CALIBRATION",
    [MI_BATTERY]   = "BATTERY",
};

const bool menu_divider[MI_COUNT] = {
    [MI_SPEED] = true, [MI_BEEPER] = true, [MI_BAUD] = true,
};

const uint16_t menu_dim_s[MENU_NDIM] = { 0, 60, 300, 900, 1800 };
const uint32_t menu_baud[MENU_NBAUD] = { 9600, 19200, 38400, 57600, 115200 };

/* Continuity threshold steps, ohms. Stock's default is 50 (prefs +0x14). */
static const uint16_t thresholds[] = { 5, 10, 20, 30, 50, 100, 200, 500, 1000 };
#define NTHRESH (int)(sizeof thresholds / sizeof thresholds[0])

#define BACKLIGHT_MIN  10      /* the menu never turns the panel fully dark */
#define BACKLIGHT_STEP 10

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void menu_value(const app_t *a, menu_item_t it, char *out, size_t cap)
{
    static const char *const speeds[] = { "LOW", "MID", "HIGH" };
    static const char *const tunits[] = { "\xC2\xB0" "C", "\xC2\xB0" "F", "K" };
    const platform_t *p = a->p;
    out[0] = 0;
    switch (it) {
    case MI_BACKLIGHT: snprintf(out, cap, "%u%%", a->backlight_pct); break;
    case MI_DIM:
        if (!menu_dim_s[a->dim_idx]) snprintf(out, cap, "NEVER");
        else snprintf(out, cap, "%u min", menu_dim_s[a->dim_idx] / 60u);
        break;
    case MI_BEEPER:    snprintf(out, cap, "%s", a->beeper ? "ON" : "OFF"); break;
    case MI_THRESHOLD: snprintf(out, cap, "%u " "\xCE\xA9", a->cont_threshold); break;
    case MI_DIODE_TH:  snprintf(out, cap, "%u.%u V", a->diode_th_dv / 10u, a->diode_th_dv % 10u); break;
    case MI_CJ_OFFSET: snprintf(out, cap, "%+.1f \xC2\xB0" "C", a->cj_offset_dc / 10.0); break;
    case MI_RTD_LEADS: snprintf(out, cap, "%u.%02u \xCE\xA9", a->rtd_lead_mohm / 1000u,
                                (a->rtd_lead_mohm % 1000u) / 10u); break;
    case MI_BEEP_PITCH: snprintf(out, cap, "%u Hz", sound_pitch(&a->snd)); break;
    case MI_BEEP_VOL:  snprintf(out, cap, "%u%%", a->snd.volume); break;
    case MI_BEEP_STYLE: snprintf(out, cap, "%s", snd_style_name[a->snd.style]); break;
    case MI_BEEP_ATTACK: snprintf(out, cap, "%u ms", a->snd.attack_ms); break;
    case MI_BEEP_RELEASE: snprintf(out, cap, "%u ms", a->snd.release_ms); break;
    case MI_BEEP_GLIDE:
        if (a->snd.glide_pct) snprintf(out, cap, "%+d%%", a->snd.glide_pct);
        else snprintf(out, cap, "OFF");
        break;
    case MI_BEEP_PATTERN: snprintf(out, cap, "%s", snd_pat_name[a->snd.pattern]); break;
    case MI_CLICK_LEN: snprintf(out, cap, "%u ms", a->snd.click_ms); break;
    case MI_BARGRAPH:  snprintf(out, cap, "%s", a->bargraph ? "ON" : "OFF"); break;
    case MI_TERMINAL:  snprintf(out, cap, "%s", a->terminal ? "ON" : "OFF"); break;
    case MI_SPEED:     snprintf(out, cap, "%s", speeds[a->rate % 3]); break;
    case MI_TEMP_UNIT: snprintf(out, cap, "%s", tunits[a->temp_unit % 3]); break;
    case MI_DBM_REF:   snprintf(out, cap, "%u " "\xCE\xA9", meas_dbm_rref[a->rref_idx]); break;
    case MI_THEME:     snprintf(out, cap, "%s", a->theme->name); break;
    case MI_BAUD:      snprintf(out, cap, "%lu", (unsigned long)menu_baud[a->baud_idx]); break;
    case MI_DEFAULTS:  snprintf(out, cap, "%s", a->menu_armed ? "Press > again" : ">"); break;
    case MI_VERSION:   snprintf(out, cap, "%s", APP_VERSION); break;
    case MI_CLOCK: {                                 /* B33 */
        uint32_t t = p->clock_now ? p->clock_now() : 0;
        if (!p->clock_now) snprintf(out, cap, "NONE");
        else if (!t) snprintf(out, cap, "NOT SET");
        else { char b[24]; clock_fmt(b, sizeof b, t, ' '); snprintf(out, cap, "%.16s", b); }
        break;
    }
    case MI_SERIAL:    snprintf(out, cap, "%s", p->serial_number ? p->serial_number : "-"); break;
    case MI_CAL: {
        int bad = p->cal_status ? p->cal_status() : -1;
        int user = ucal_count(&a->ucal);           /* B31 */
        if (bad == 0 && user) snprintf(out, cap, "FACTORY + USER %d >", user);
        else if (bad == 0) snprintf(out, cap, "FACTORY >");
        else if (bad > 0) snprintf(out, cap, "UNITY %d BAD >", bad);
        else snprintf(out, cap, "UNKNOWN >");
        break;
    }
    case MI_BATTERY: {
        int mv = p->battery_mv ? p->battery_mv() : -1;
        if (mv < 0) snprintf(out, cap, "NOT MEASURED");
        else snprintf(out, cap, "%d.%02d V  %d%%", mv / 1000, (mv % 1000) / 10, app_battery_pct(mv));
        break;
    }
    default: break;
    }
}

static uint16_t step_threshold(uint16_t cur, int dir)
{
    if (dir > 0) {
        for (int i = 0; i < NTHRESH; i++) if (thresholds[i] > cur) return thresholds[i];
        return thresholds[NTHRESH - 1];
    }
    for (int i = NTHRESH - 1; i >= 0; i--) if (thresholds[i] < cur) return thresholds[i];
    return thresholds[0];
}

void menu_adjust(app_t *a, menu_item_t it, int dir)
{
    dir = dir > 0 ? 1 : -1;
    switch (it) {
    case MI_CAL:                                         /* B32: open the page */
        if (dir > 0) { a->cal_page = true; a->cal_sel = 0; a->hash_valid = false; }
        break;
    case MI_BACKLIGHT: {
        int v = clampi(a->backlight_pct + dir * BACKLIGHT_STEP, BACKLIGHT_MIN, 100);
        if (dir > 0 && a->backlight_pct < BACKLIGHT_MIN) v = BACKLIGHT_MIN;
        a->backlight_pct = (uint8_t)(v / BACKLIGHT_STEP * BACKLIGHT_STEP);
        break;
    }
    case MI_DIM:       a->dim_idx = (uint8_t)clampi(a->dim_idx + dir, 0, MENU_NDIM - 1); break;
    case MI_BEEPER:    a->beeper = !a->beeper; break;
    case MI_THRESHOLD: a->cont_threshold = step_threshold(a->cont_threshold, dir); break;
    /* Beep settings preview themselves (SND_TEST) as they change. */
    case MI_BEEP_PITCH:
        a->snd.pitch_hz = sound_pitch_step(sound_pitch(&a->snd), dir);
        app_sound(a, SND_TEST);
        break;
    case MI_BEEP_VOL:
        a->snd.volume = (uint8_t)clampi(a->snd.volume + dir * SND_VOL_STEP, SND_VOL_MIN, 100);
        app_sound(a, SND_TEST);
        break;
    case MI_BEEP_STYLE:
    {   /* through the menu's order, CUSTOM last */
        int i = 0;
        while (i < SND_NSTYLE && snd_style_order[i] != a->snd.style) i++;
        sound_style(&a->snd, snd_style_order[(i + SND_NSTYLE + dir) % SND_NSTYLE]);
    }
        app_sound(a, SND_TEST);
        break;
    case MI_BEEP_ATTACK:  sound_step(&a->snd, 0, dir); app_sound(a, SND_TEST); break;
    case MI_BEEP_RELEASE: sound_step(&a->snd, 1, dir); app_sound(a, SND_TEST); break;
    case MI_BEEP_GLIDE:   sound_step(&a->snd, 2, dir); app_sound(a, SND_TEST); break;
    case MI_BEEP_PATTERN: sound_step(&a->snd, 4, dir); app_sound(a, SND_TEST); break;
    case MI_CLICK_LEN:    sound_step(&a->snd, 3, dir); app_sound(a, SND_CLICK); break;
    case MI_BARGRAPH:  a->bargraph = !a->bargraph; break;
    case MI_TERMINAL:  a->terminal = !a->terminal; break;
    case MI_CJ_OFFSET:
        a->cj_offset_dc = (int8_t)clampi(a->cj_offset_dc + dir, -APP_CJ_OFFSET_MAX, APP_CJ_OFFSET_MAX);
        break;
    case MI_RTD_LEADS:
        a->rtd_lead_mohm = (uint16_t)clampi(a->rtd_lead_mohm + dir * APP_RTD_LEAD_STEP, 0,
                                            APP_RTD_LEAD_MAX_MOHM);
        break;
    case MI_DIODE_TH:
        a->diode_th_dv = (uint8_t)clampi(a->diode_th_dv + dir, APP_DIODE_TH_MIN, APP_DIODE_TH_MAX);
        break;
    case MI_SPEED:     a->rate = (uint8_t)clampi(a->rate + dir, 0, 2); break;
    case MI_TEMP_UNIT: a->temp_unit = (meas_tunit_t)((a->temp_unit + 3 + dir) % 3); break;
    case MI_DBM_REF:   a->rref_idx = clampi(a->rref_idx + dir, 0, MEAS_NRREF - 1); break;
    case MI_THEME:     a->theme_idx = (a->theme_idx + APP_NTHEMES + dir) % APP_NTHEMES; break;
    case MI_BAUD:      a->baud_idx = (uint8_t)clampi(a->baud_idx + dir, 0, MENU_NBAUD - 1); break;
    case MI_DEFAULTS:
        if (dir < 0) { a->menu_armed = false; break; }
        if (!a->menu_armed) { a->menu_armed = true; return; }
        a->menu_armed = false;
        app_factory_defaults(a);
        break;
    default:
        return;                          /* read-only */
    }
    app_apply_prefs(a);
}

static void close_menu(app_t *a)
{
    a->menu_open = false;
    a->menu_armed = false;
    a->hash_valid = false;
}

bool menu_key(app_t *a, app_key_t k)
{
    a->dirty = true;
    switch (k) {
    case KEY_UP: case KEY_F1:
        a->menu_sel = (a->menu_sel + MI_COUNT - 1) % MI_COUNT;
        a->menu_armed = false;
        return true;
    case KEY_DOWN: case KEY_F2:
        a->menu_sel = (a->menu_sel + 1) % MI_COUNT;
        a->menu_armed = false;
        return true;
    /* + above -, as Item Up above Item Down (seen on the meter 2026-11-12) */
    case KEY_RIGHT: case KEY_F3: menu_adjust(a, (menu_item_t)a->menu_sel, +1); return true;
    case KEY_LEFT: case KEY_F4:  menu_adjust(a, (menu_item_t)a->menu_sel, -1); return true;
    case KEY_UTILITY: case KEY_F5: case KEY_RUNSTOP:     /* F5: EXIT */
        close_menu(a);
        return true;
    default:
        close_menu(a);                   /* a function key: leave and do its job */
        return false;
    }
}
