/* lib/app on the simulated board.
 *
 * Unit tests for readout (limits from the manual, formatting, autorange),
 * then end to end: a physical input on the simulated HY3131 goes through the
 * real driver, the bit-level bus model, acquisition, lib/measure, the app,
 * band rendering, the real LCD driver and the panel decoder.
 */
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "clock.h"
#include "hy3131.h"
#include "readout.h"
#include "meas_temp.h"
#include "sim_board.h"
#include "sim_flash.h"

static app_t app;
static sim_world_t *w;

void setUp(void)
{
    sim_board_init(NULL);
    w = sim_hy3131_world();
    app_init(&app, sim_board_platform());
}
void tearDown(void) {}

#define READS(s)  TEST_ASSERT_EQUAL_STRING((s), app.view.reading)

/* ---- readout -------------------------------------------------------------- */

static void test_overload_limits_from_manual(void)
{
    TEST_ASSERT_EQUAL_DOUBLE(1050.0, readout_limit(MEAS_DCV, 5));
    TEST_ASSERT_EQUAL_DOUBLE(787.5, readout_limit(MEAS_ACV, 4));
    TEST_ASSERT_EQUAL_DOUBLE(10.5, readout_limit(MEAS_DCI, 5));
    TEST_ASSERT_EQUAL_DOUBLE(55e6, readout_limit(MEAS_OHM2, 5));
    TEST_ASSERT_EQUAL_DOUBLE(50500e-6, readout_limit(MEAS_CAP, 6));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 5.5, readout_limit(MEAS_DCV, 2));      /* 10% over */
    TEST_ASSERT_DOUBLE_WITHIN(1e-15, 55e-3, readout_limit(MEAS_DCV, 0));
    TEST_ASSERT_TRUE(readout_overload(MEAS_DCV, 2, 5.51));
    TEST_ASSERT_FALSE(readout_overload(MEAS_DCV, 2, -5.49));
}

static void test_value_resolution(void)
{
    char b[24];
    readout_value(b, sizeof b, MEAS_DCV, 0, 12.3456e-3, false); TEST_ASSERT_EQUAL_STRING("12.346", b);
    readout_value(b, sizeof b, MEAS_DCV, 1, 0.12345, false);    TEST_ASSERT_EQUAL_STRING("123.45", b);
    readout_value(b, sizeof b, MEAS_DCV, 2, 1.23456, false);    TEST_ASSERT_EQUAL_STRING("1.2346", b);
    readout_value(b, sizeof b, MEAS_DCV, 5, 999.94, false);     TEST_ASSERT_EQUAL_STRING("999.9", b);
    readout_value(b, sizeof b, MEAS_DCV, 2, -0.00001, false);   TEST_ASSERT_EQUAL_STRING("0.0000", b);
    readout_value(b, sizeof b, MEAS_OHM2, 4, 1.5e6, false);     TEST_ASSERT_EQUAL_STRING("1.5000", b);
    readout_value(b, sizeof b, MEAS_DCV, 2, 9.0, true);         TEST_ASSERT_EQUAL_STRING("overload", b);
    readout_value(b, sizeof b, MEAS_CONT, 0, 2000.0, true);     TEST_ASSERT_EQUAL_STRING("Open", b);
    readout_value(b, sizeof b, MEAS_FREQ, 1, 1234.5, false);    TEST_ASSERT_EQUAL_STRING("1.2345", b);
    /* only glyphs the stock big font has */
    const char *ok = "-.0123456789overladOpn ";
    readout_value(b, sizeof b, MEAS_DCV, 3, -12.3, false);
    for (const char *c = b; *c; c++) TEST_ASSERT_NOT_NULL(strchr(ok, *c));
}

static void test_units_and_labels(void)
{
    char b[24];
    TEST_ASSERT_EQUAL_STRING("mV DC", readout_unit(MEAS_DCV, 0, false));
    TEST_ASSERT_EQUAL_STRING("uA AC", readout_unit(MEAS_ACI, 0, false));
    TEST_ASSERT_EQUAL_STRING("MOhm", readout_unit(MEAS_OHM2, 5, false));
    TEST_ASSERT_EQUAL_STRING("nF", readout_unit(MEAS_CAP, 0, false));
    TEST_ASSERT_EQUAL_STRING("ms", readout_unit(MEAS_FREQ, 4, true));
    readout_range_label(b, sizeof b, MEAS_DCV, 2);  TEST_ASSERT_EQUAL_STRING("5 V", b);
    readout_range_label(b, sizeof b, MEAS_OHM2, 3); TEST_ASSERT_EQUAL_STRING("500 kOhm", b);
}

static void test_autorange_rules(void)
{
    TEST_ASSERT_EQUAL_INT(5, readout_autorange(MEAS_DCV, 2, 6.0, true));      /* overload: to the top */
    TEST_ASSERT_EQUAL_INT(2, readout_autorange(MEAS_DCV, 2, 5.2, false));     /* 104%: stays, shows 5.2000 */
    TEST_ASSERT_EQUAL_INT(3, readout_autorange(MEAS_DCV, 2, 5.3, false));     /* 106%, not overload: +1 */
    TEST_ASSERT_EQUAL_INT(4, readout_autorange(MEAS_DCV, 5, 400.0, false));   /* down: straight to 500 V */
    TEST_ASSERT_EQUAL_INT(0, readout_autorange(MEAS_DCV, 3, 0.01, false));    /* 50 V -> 50 mV in one jump */
    /* 10 mV read on the 1000 V range is +-60 mV (3 counts): 50 mV could
     * overload and bounce back to the top, so stop at 500 mV first. */
    TEST_ASSERT_EQUAL_INT(1, readout_autorange(MEAS_DCV, 5, 0.01, false));
    TEST_ASSERT_EQUAL_INT(0, readout_autorange(MEAS_DCV, 1, 0.01, false));
    TEST_ASSERT_EQUAL_INT(5, readout_autorange(MEAS_OHM2, 0, 1e12, true));    /* open: top */
    TEST_ASSERT_EQUAL_INT(3, readout_autorange(MEAS_CAP, 2, 1.0, true));      /* cap: one step */
    TEST_ASSERT_EQUAL_INT(2, readout_autorange(MEAS_DCV, 2, 4.9, false));     /* stays */
    TEST_ASSERT_EQUAL_INT(2, readout_autorange(MEAS_DCV, 2, 0.46, false));    /* above 90% of 500 mV */
    TEST_ASSERT_EQUAL_INT(1, readout_autorange(MEAS_DCV, 2, 0.44, false));    /* below: down */
    TEST_ASSERT_EQUAL_INT(5, readout_autorange(MEAS_DCV, 5, 2000.0, true));   /* top stays */
    TEST_ASSERT_EQUAL_INT(0, readout_autorange(MEAS_DCV, 0, 0.0, false));
    TEST_ASSERT_EQUAL_INT(5, readout_autorange(MEAS_CAP, 4, 1.0, true));      /* on to the slope ranges */
    TEST_ASSERT_EQUAL_INT(6, readout_autorange(MEAS_CAP, 6, 1.0, true));      /* top stays */
}

/* ---- bus and display plumbing ---------------------------------------------- */

static void test_bus_model_round_trips_through_driver(void)
{
    hy3131_init(&sim_hy3131_io);
    hy3131_write8(HY_R22, 0xA5);
    TEST_ASSERT_EQUAL_HEX8(0xA5, sim_hy3131_reg(HY_R22));
    TEST_ASSERT_EQUAL_HEX8(0xA5, hy3131_read8(HY_R22));
    uint8_t in[3] = { 1, 2, 3 }, out[3];
    hy3131_write_burst(HY_R2A, in, 3);
    hy3131_read_burst(HY_R2A, out, 3);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in, out, 3);
}

static rgb565_t full[SCR_W * SCR_H];

/* The panel, assembled band by band through the LCD driver, must equal one
 * full-screen draw of the same view. */
static void test_band_render_matches_full_render(void)
{
    w->dcv = 3.3;
    sim_board_run(&app, 1000);
    fb_t f;
    fb_init(&f, full, SCR_W, SCR_H);
    app_draw(&app, &f);
    TEST_ASSERT_EQUAL_MEMORY(full, sim_lcd_pixels(), sizeof full);
}

static void test_only_changed_bands_are_sent(void)
{
    w->dcv = 1.0;
    sim_board_run(&app, 1000);
    uint32_t sent = app.bands_sent;
    app.dirty = true;
    sim_board_run(&app, 1);                  /* same view: nothing to send */
    TEST_ASSERT_EQUAL_UINT32(sent, app.bands_sent);
    w->dcv = 2.0;
    sim_board_run(&app, 500);
    uint32_t delta = app.bands_sent - sent;
    TEST_ASSERT_TRUE(delta > 0 && delta < (SCR_H / SIM_BAND_ROWS));
}

/* ---- end to end ---------------------------------------------------------------- */

static void test_dcv_reading(void)
{
    w->dcv = 1.2345;
    sim_board_run(&app, 1000);
    READS("1.2345");
    TEST_ASSERT_EQUAL_STRING("V DC", app.view.unit);
    TEST_ASSERT_EQUAL_STRING("AUTO 5 V", app.view.range);
    TEST_ASSERT_EQUAL_UINT8(100, sim_board_backlight());
}

static void test_dcv_autoranges_down_and_up(void)
{
    w->dcv = 0.012;
    sim_board_run(&app, 2000);
    READS("12.000");
    TEST_ASSERT_EQUAL_STRING("mV DC", app.view.unit);
    w->dcv = 42.0;
    sim_board_run(&app, 3000);
    READS("42.000");
    TEST_ASSERT_EQUAL_STRING("AUTO 50 V", app.view.range);
}

static void test_dcv_manual_range_overload(void)
{
    app_key(&app, KEY_DOWN);                    /* manual, 500 mV */
    w->dcv = 0.7;
    sim_board_run(&app, 1000);
    READS("overload");
    TEST_ASSERT_EQUAL_STRING("500 mV", app.view.range);
    w->dcv = -0.25;
    sim_board_run(&app, 1000);
    READS("-250.00");
}

static void test_acv_and_dbm(void)
{
    app_key(&app, KEY_V);
    w->acv = 0.25;
    sim_board_run(&app, 2000);
    READS("250.00");
    TEST_ASSERT_EQUAL_STRING("mV AC", app.view.unit);
    w->acv = sqrt(0.6);                          /* 0 dBm into 600 ohm */
    app_key(&app, KEY_MATH);
    app_key(&app, KEY_MATH);
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_STRING("0.00 dBm  REF 600 \xCE\xA9", app.view.sub);
}

static void test_current(void)
{
    app_key(&app, KEY_I);
    w->dci = 0.0123;
    sim_board_run(&app, 1000);
    READS("12.300");
    TEST_ASSERT_EQUAL_STRING("mA DC", app.view.unit);
}

static void test_resistance_continuity_diode(void)
{
    app_key(&app, KEY_OHM);
    w->ohm = 4700.0;
    sim_board_run(&app, 1000);
    READS("4.7000");
    TEST_ASSERT_EQUAL_STRING("kOhm", app.view.unit);

    app_key(&app, KEY_OHM);                      /* continuity */
    w->ohm = 10.0;
    sim_board_run(&app, 500);
    READS("10.0");
    TEST_ASSERT_TRUE(sim_board_beeping());
    w->ohm = 1e9;
    sim_board_run(&app, 500);
    READS("Open");
    TEST_ASSERT_FALSE(sim_board_beeping());

    app_key(&app, KEY_OHM);                      /* diode */
    unsigned before = sim_board_beeps();
    w->diode = 0.65;
    sim_board_run(&app, 500);
    READS("0.6500");
    /* one short beep for a good junction, then quiet while it stays */
    TEST_ASSERT_EQUAL_UINT(before + 1, sim_board_beeps());
    TEST_ASSERT_FALSE(sim_board_beeping());
    sim_board_run(&app, 1000);
    TEST_ASSERT_EQUAL_UINT(before + 1, sim_board_beeps());
    w->diode = 1e9;                              /* leads open: re-arms */
    sim_board_run(&app, 500);
    READS("Open");
    w->diode = 0.65;
    sim_board_run(&app, 500);
    TEST_ASSERT_EQUAL_UINT(before + 2, sim_board_beeps());
}

/* The unit size of the label font is a subset (scripts/make-label-font.py):
 * every unit the app can show must be in it, or it draws a gap. */
static void test_every_unit_renders_in_the_unit_font(void)
{
    for (int fn = 0; fn < MEAS_NFUNC; fn++)
        for (int r = 0; r < 16; r++)
            for (int per = 0; per < 2; per++) {
                const char *u = readout_unit((meas_fn_t)fn, r, per);
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, fb_text_missing(u, 4), u);
            }
    static const char *const more[] = { "\xC2\xB0" "C", "\xC2\xB0" "F", "K", "dB", "dBm",
                                        "\xCE\xA9", "V DC", "A AC" };
    for (size_t i = 0; i < sizeof more / sizeof more[0]; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, fb_text_missing(more[i], 4), more[i]);
}

static void test_capacitance_and_frequency(void)
{
    app_key(&app, KEY_CAP);
    w->cap = 1e-6;
    sim_board_run(&app, 2000);
    READS("1.0000");
    TEST_ASSERT_EQUAL_STRING("uF", app.view.unit);

    app_key(&app, KEY_FREQ);
    w->freq = 1000.0;
    w->duty = 25.0;
    sim_board_run(&app, 2000);
    READS("1.0000");
    TEST_ASSERT_EQUAL_STRING("kHz", app.view.unit);
    TEST_ASSERT_EQUAL_STRING("DUTY 25.0%", app.view.sub);
    app_key(&app, KEY_FREQ);                     /* again: period */
    sim_board_run(&app, 2000);
    READS("1.0000");
    TEST_ASSERT_EQUAL_STRING("ms", app.view.unit);
}

/* Capacitance 5 / 50 mF (charge slope): autoranging climbs from the
 * counter ranges, a reading fits the charge, and a small capacitor goes
 * back down. The sim charges like the meter (sim_hy3131.c cap_step_ms). */
static void test_capacitance_charge_slope(void)
{
    app_key(&app, KEY_CAP);
    w->cap = 18e-3;
    sim_board_run(&app, 45000);
    TEST_ASSERT_EQUAL_INT(6, app.range[MEAS_CAP]);
    TEST_ASSERT_FALSE(app.overload);
    double v = 0;
    TEST_ASSERT_TRUE(app_measurement(&app, &v));
    TEST_ASSERT_FLOAT_WITHIN(0.18e-3, 18e-3, v);                  /* 1% */
    TEST_ASSERT_EQUAL_STRING("mF", app.view.unit);

    w->cap = 1e-3;                                                /* down to 5 mF */
    sim_board_run(&app, 30000);
    TEST_ASSERT_EQUAL_INT(5, app.range[MEAS_CAP]);
    TEST_ASSERT_TRUE(app_measurement(&app, &v));
    TEST_ASSERT_FLOAT_WITHIN(0.01e-3, 1e-3, v);

    w->cap = 200e-6;                                              /* back to the counters */
    sim_board_run(&app, 20000);
    TEST_ASSERT_EQUAL_INT(4, app.range[MEAS_CAP]);
    TEST_ASSERT_TRUE(app_measurement(&app, &v));
    TEST_ASSERT_FLOAT_WITHIN(2e-6, 200e-6, v);

    w->cap = 80e-3;                                               /* past 50 mF: overload */
    sim_board_run(&app, 60000);                                   /* from a capacitor left high */
    TEST_ASSERT_EQUAL_INT(6, app.range[MEAS_CAP]);
    TEST_ASSERT_TRUE(app.overload);
}

static const char *ask(const char *cmd, uint32_t ms);

/* B25: the cold junction is re-measured every 10 s while it moves and
 * backs off to once a minute while steady, so readings stop far less
 * often than the old fixed 10 s (each refresh costs ~2 s of readings). */
static void test_cold_junction_refresh_adapts(void)
{
    app_key(&app, KEY_TEMP);
    w->temp_c = 100.0;
    w->cj_c = 25.0;
    sim_board_run(&app, 600000);                       /* 10 min, steady */
    uint32_t steady = app.acq.cj_count;
    TEST_ASSERT_TRUE_MESSAGE(steady <= 14, "fixed 10 s would be 60");
    TEST_ASSERT_EQUAL_UINT32(ACQ_CJ_PERIOD_MAX_MS, app.acq.cj_period);
    READS("100.0");

    w->cj_c = 28.0;                                    /* a hand on the terminals */
    sim_board_run(&app, ACQ_CJ_PERIOD_MAX_MS + 5000);
    TEST_ASSERT_DOUBLE_WITHIN(0.2, 28.0, app.acq.t_cj);
    READS("100.0");                                    /* compensated */
    TEST_ASSERT_TRUE(app.acq.cj_period < ACQ_CJ_PERIOD_MAX_MS);   /* tracking closely again */
}

static void test_temperature(void)
{
    app_key(&app, KEY_TEMP);                     /* type K, needs the cold junction */
    w->temp_c = 500.0;
    w->cj_c = 30.0;
    sim_board_run(&app, 4000);
    READS("500.0");
    /* B26: the EMF the probe produces with its cold end at 30 C */
    double emf = meas_tc_k_mv(500.0) - meas_tc_k_mv(30.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, emf, app.tc_emf_mv);
    char want[64];
    snprintf(want, sizeof want, "CJ 30.0 \xC2\xB0" "C   EMF %.3f mV", app.tc_emf_mv);
    TEST_ASSERT_EQUAL_STRING(want, app.view.sub);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, emf, atof(ask("TEMP:TC:EMF?", 5)));
    TEST_ASSERT_DOUBLE_WITHIN(0.2, 30.0, atof(ask("TEMPERATURE:TCOUPLE:CJUNCTION?", 5)));

    app_key(&app, KEY_TEMP);                     /* PT100 */
    w->temp_c = 100.0;
    sim_board_run(&app, 1000);
    READS("100.0");
    TEST_ASSERT_EQUAL_STRING("PT100", app.view.range);
}

static void test_rel_hold_stats(void)
{
    w->dcv = 1.0;
    sim_board_run(&app, 1000);
    app_key(&app, KEY_F1);                       /* REL */
    sim_board_run(&app, 500);
    READS("0.0000");
    w->dcv = 1.5;
    sim_board_run(&app, 1000);
    READS("0.5000");

    app_key(&app, KEY_RUNSTOP);                  /* HOLD */
    w->dcv = 3.0;
    sim_board_run(&app, 1000);
    READS("0.5000");
    TEST_ASSERT_NOT_NULL(strstr(app.view.status, "HOLD"));
    app_key(&app, KEY_RUNSTOP);                  /* auto-hold */
    app_key(&app, KEY_RUNSTOP);                  /* run */

    app_key(&app, KEY_F1);                       /* REL off */
    app_key(&app, KEY_MATH);                     /* stats */
    sim_board_run(&app, 1000);
    w->dcv = 2.0;
    sim_board_run(&app, 1000);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "MIN 2"));
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "MAX 3"));
}

static void test_noise_is_averaged(void)
{
    w->dcv = 2.5;
    w->noise_counts = 200.0;                     /* ~0.18 mV per sample on 5 V */
    sim_board_run(&app, 8000);                   /* ~32 conversions at 246 ms */
    TEST_ASSERT_DOUBLE_WITHIN(2e-4, 2.5, app.value);
}

/* ---- Utility menu, dimming, graph ------------------------------------------ */

static void reboot(void);

static void menu_goto(menu_item_t it)
{
    if (!app.menu_open) app_key(&app, KEY_UTILITY);
    while (app.menu_sel != (int)it) app_key(&app, KEY_DOWN);
}

/* The theme is a menu item: there is no sixth softkey on our panel. */
static void set_theme_dark(void)
{
    menu_goto(MI_THEME);
    app_key(&app, KEY_RIGHT);
    app_key(&app, KEY_UTILITY);
}

/* B56: a frame whose content did not change is not composed at all. */
static void test_unchanged_frame_is_not_composed(void)
{
    w->dcv = 1.0;
    sim_board_run(&app, 1000);
    uint32_t drawn = app.frames_drawn, skipped = app.frames_skipped;
    for (int i = 0; i < 5; i++) { app.dirty = true; sim_board_run(&app, 1); }
    TEST_ASSERT_EQUAL_UINT32(drawn, app.frames_drawn);
    TEST_ASSERT_TRUE(app.frames_skipped >= skipped + 5);
    w->dcv = 2.0;                                    /* a change is drawn */
    sim_board_run(&app, 500);
    TEST_ASSERT_TRUE(app.frames_drawn > drawn);
}

/* The panel must equal a fresh full draw after every kind of change: a
 * field app_draw reads that the frame fingerprint misses would leave stale
 * pixels here. */
static void panel_is_current(const char *what)
{
    sim_board_run(&app, 600);
    fb_t f;
    fb_init(&f, full, SCR_W, SCR_H);
    app_draw(&app, &f);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(full, sim_lcd_pixels(), sizeof full, what);
}

static void test_panel_stays_current_through_every_screen(void)
{
    w->dcv = 1.234;
    panel_is_current("measure");
    app_key(&app, KEY_F1);           panel_is_current("REL on");
    app_key(&app, KEY_F1);           panel_is_current("REL off");
    app_key(&app, KEY_MATH);         panel_is_current("stats");
    app_key(&app, KEY_MATH);         panel_is_current("dBm");
    app_key(&app, KEY_MATH);         panel_is_current("math off");
    app_key(&app, KEY_DOWN);         panel_is_current("manual range");
    app_key(&app, KEY_F2);           panel_is_current("auto again");
    app_key(&app, KEY_F3);           panel_is_current("number + mini trend (B58)");
    app_key(&app, KEY_F3);           panel_is_current("number + mini histogram (B58)");
    app_key(&app, KEY_F3);           panel_is_current("graph");
    app_key(&app, KEY_F1);           panel_is_current("graph span");
    app_key(&app, KEY_F2);           panel_is_current("graph scale");
    w->dcv = 4.0;                    panel_is_current("graph, new reading");
    app_key(&app, KEY_F3);           panel_is_current("histogram");
    app_key(&app, KEY_F3);           panel_is_current("back to numbers");
    app_key(&app, KEY_RECORD);       panel_is_current("record screen");
    app_key(&app, KEY_F5);           panel_is_current("record exit");
    menu_goto(MI_BACKLIGHT);         panel_is_current("menu open");
    menu_goto(MI_BEEPER);            panel_is_current("menu, row moved on a drawn menu");
    app_key(&app, KEY_RIGHT);        panel_is_current("menu, value changed");
    app_key(&app, KEY_RIGHT);        panel_is_current("menu, value back");
    app_key(&app, KEY_UTILITY);      panel_is_current("menu closed");
    set_theme_dark();                panel_is_current("theme");
    app_key(&app, KEY_V);            panel_is_current("AC V");
    app_key(&app, KEY_OHM);          panel_is_current("ohms");
    app_key(&app, KEY_TEMP);         panel_is_current("temperature");
    app_key(&app, KEY_DUAL);         panel_is_current("dual");
}

/* Five softkeys (docs/hardware.md): nothing is ever put in a sixth. */
static void test_no_screen_uses_a_sixth_softkey(void)
{
    TEST_ASSERT_EQUAL_INT(5, UI_NKEYS);
    app_key(&app, KEY_F5);                                        /* measure: nothing to exit */
    TEST_ASSERT_EQUAL_INT(SCREEN_MEASURE, app.screen);
    TEST_ASSERT_FALSE(app.menu_open);
}

/* Stock beeps on every key press. A physical press (through key_poll)
 * clicks for APP_CLICK_MS, unless the beeper is off in the menu. */
static void test_key_click(void)
{
    sim_board_press(KEY_V);
    sim_board_run(&app, 5);
    TEST_ASSERT_TRUE(sim_board_beeping());
    sim_board_run(&app, APP_CLICK_MS + 5);
    TEST_ASSERT_FALSE(sim_board_beeping());

    app.beeper = false;
    sim_board_press(KEY_V);
    sim_board_run(&app, 5);
    TEST_ASSERT_FALSE(sim_board_beeping());
}

/* A click during continuity doesn't silence it: afterwards the beeper goes
 * back to what the reading wants. */
static void test_click_returns_to_the_continuity_tone(void)
{
    app_key(&app, KEY_OHM);
    app_key(&app, KEY_OHM);                                       /* continuity */
    sim_hy3131_world()->ohm = 10.0;
    sim_board_run(&app, 500);
    TEST_ASSERT_TRUE(sim_board_beeping());
    sim_board_press(KEY_RUNSTOP);
    sim_board_run(&app, APP_CLICK_MS + 200);
    TEST_ASSERT_TRUE(sim_board_beeping());
}

static void test_menu_edits_preferences(void)
{
    app_key(&app, KEY_UTILITY);
    TEST_ASSERT_TRUE(app.menu_open);
    TEST_ASSERT_EQUAL_INT(MI_BACKLIGHT, app.menu_sel);
    for (int i = 0; i < 4; i++) app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_UINT8(60, sim_board_backlight());           /* applied at once */
    for (int i = 0; i < 20; i++) app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_UINT8(10, app.backlight_pct);               /* never fully dark */

    menu_goto(MI_BEEPER);
    app_key(&app, KEY_RIGHT);
    TEST_ASSERT_FALSE(app.beeper);
    menu_goto(MI_THEME);
    app_key(&app, KEY_F3);                                        /* softkeys work too: F3 is + */
    TEST_ASSERT_EQUAL_STRING("dark", app.theme->name);
    menu_goto(MI_TEMP_UNIT);
    app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_INT(MEAS_KELVIN, app.temp_unit);            /* wraps */
    menu_goto(MI_SPEED);
    app_key(&app, KEY_RIGHT); app_key(&app, KEY_RIGHT); app_key(&app, KEY_RIGHT);
    TEST_ASSERT_EQUAL_UINT8(2, app.rate);                         /* clamps at HIGH */

    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("HIGH", app.view.menu_value[MI_SPEED]);
    TEST_ASSERT_EQUAL_STRING("ITEM", app.view.key_label[0]);
    TEST_ASSERT_EQUAL_STRING("EXIT", app.view.key_label[4]);
    app_key(&app, KEY_F5);                                        /* EXIT: the fifth softkey */
    TEST_ASSERT_FALSE(app.menu_open);
    TEST_ASSERT_EQUAL_STRING("dark", app.theme->name);
}

static void test_menu_function_key_closes_and_acts(void)
{
    app_key(&app, KEY_UTILITY);
    app_key(&app, KEY_V);
    TEST_ASSERT_FALSE(app.menu_open);
    TEST_ASSERT_EQUAL_INT(MEAS_ACV, app.fn);
}

static void test_menu_threshold_steps_and_snaps(void)
{
    app.cont_threshold = 37;                                      /* e.g. set remotely */
    menu_goto(MI_THRESHOLD);
    app_key(&app, KEY_RIGHT);
    TEST_ASSERT_EQUAL_UINT16(50, app.cont_threshold);
    app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_UINT16(30, app.cont_threshold);
    for (int i = 0; i < 12; i++) app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_UINT16(5, app.cont_threshold);
}

static void test_menu_defaults_need_two_presses(void)
{
    set_theme_dark();
    app_key(&app, KEY_V);                                         /* ACV */
    app.cont_threshold = 200;
    menu_goto(MI_DEFAULTS);
    app_key(&app, KEY_RIGHT);
    TEST_ASSERT_TRUE(app.menu_armed);
    TEST_ASSERT_EQUAL_STRING("dark", app.theme->name);            /* nothing yet */
    app_key(&app, KEY_DOWN); app_key(&app, KEY_UP);               /* moving away disarms */
    TEST_ASSERT_FALSE(app.menu_armed);
    app_key(&app, KEY_RIGHT);
    app_key(&app, KEY_RIGHT);
    TEST_ASSERT_EQUAL_STRING("stock", app.theme->name);
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.fn);
    TEST_ASSERT_EQUAL_UINT16(50, app.cont_threshold);
}

static void test_menu_information_rows(void)
{
    char b[24];
    menu_value(&app, MI_VERSION, b, sizeof b);  TEST_ASSERT_EQUAL_STRING(APP_VERSION, b);
    menu_value(&app, MI_SERIAL, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("SIM0001", b);
    menu_value(&app, MI_CAL, b, sizeof b);      TEST_ASSERT_EQUAL_STRING("FACTORY >", b);
    menu_value(&app, MI_BATTERY, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("NOT MEASURED", b);
    sim_board_set_battery_mv(3820);
    menu_value(&app, MI_BATTERY, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("3.82 V  50%", b);
    /* B47: the gauge follows the cell; the view carries it, so it redraws. */
    TEST_ASSERT_EQUAL_INT(-1, app_battery_pct(-1));
    TEST_ASSERT_EQUAL_INT(0, app_battery_pct(3100));
    TEST_ASSERT_EQUAL_INT(100, app_battery_pct(4210));
    TEST_ASSERT_EQUAL_INT(10, app_battery_pct(3600));
    TEST_ASSERT_EQUAL_INT(85, app_battery_pct(4040));
    app_build_view(&app);
    TEST_ASSERT_EQUAL_INT(50, app.view.battery);
    sim_board_set_battery_mv(3550);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_INT(7, app.view.battery);
    sim_board_set_battery_mv(-1);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_INT(-1, app.view.battery);
    menu_goto(MI_BATTERY);
    app_key(&app, KEY_RIGHT);                                     /* read-only: harmless */
    TEST_ASSERT_TRUE(app.menu_open);
    app_key(&app, KEY_DOWN);
    TEST_ASSERT_EQUAL_INT(0, app.menu_sel);                       /* wraps to the top */
}

static void test_baud_from_menu(void)
{
    TEST_ASSERT_EQUAL_UINT32(115200, sim_board_baud());
    menu_goto(MI_BAUD);
    app_key(&app, KEY_LEFT);
    TEST_ASSERT_EQUAL_UINT32(57600, sim_board_baud());
}

static void test_backlight_dims_and_first_key_only_wakes(void)
{
    menu_goto(MI_DIM);
    app_key(&app, KEY_RIGHT);                                     /* 1 minute */
    app_key(&app, KEY_UTILITY);
    sim_board_press(KEY_RUNSTOP);                                 /* activity via the panel: */
    sim_board_press(KEY_RUNSTOP);                                 /* hold, then auto-hold */
    sim_board_run(&app, 59000);
    TEST_ASSERT_EQUAL_UINT8(100, sim_board_backlight());
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_UINT8(APP_DIM_PCT, sim_board_backlight());
    sim_board_press(KEY_V);
    sim_board_run(&app, 10);
    TEST_ASSERT_EQUAL_UINT8(100, sim_board_backlight());
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.fn);                      /* swallowed */
    sim_board_press(KEY_V);
    sim_board_run(&app, 10);
    TEST_ASSERT_EQUAL_INT(MEAS_ACV, app.fn);
}

static void test_graph_softkeys(void)
{
    w->dcv = 1.0;
    for (int i = 0; i < 3; i++) app_key(&app, KEY_F3);           /* NUM+G, NUM+H, GRAPH */
    TEST_ASSERT_EQUAL_INT(SCREEN_GRAPH, app.screen);
    app_key(&app, KEY_F1);                                        /* span, not REL */
    TEST_ASSERT_FALSE(app.rel);
    TEST_ASSERT_EQUAL_UINT8(1, app.span_idx);
    sim_board_run(&app, 10000);                                   /* ~40 readings at 246 ms */
    TEST_ASSERT_EQUAL_STRING("4 min", app.view.key_value[0]);
    TEST_ASSERT_INT_WITHIN(2, 40 / 5, app.trace.n);               /* 5 readings per point */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, trace_at(&app.trace, 0));

    app_key(&app, KEY_F5);                                        /* pause */
    int n = app.trace.n;
    sim_board_run(&app, 5000);
    TEST_ASSERT_EQUAL_INT(n, app.trace.n);
    app_key(&app, KEY_F2);
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("RANGE", app.view.key_value[1]);
    TEST_ASSERT_EQUAL_STRING("ON", app.view.key_value[4]);
    TEST_ASSERT_EQUAL_STRING("1.0000", app.view.reading);         /* live reading still updates */

    app_key(&app, KEY_F3);                                        /* histogram (B38) */
    app_key(&app, KEY_F3);                                        /* back: F1 is REL again */
    app_key(&app, KEY_F1);
    TEST_ASSERT_TRUE(app.rel);
}

static void test_new_preferences_persist_and_old_records_load(void)
{
    menu_goto(MI_DIM);   app_key(&app, KEY_RIGHT); app_key(&app, KEY_RIGHT);
    menu_goto(MI_BAUD);  app_key(&app, KEY_LEFT);
    app_key(&app, KEY_UTILITY);
    for (int i = 0; i < 3; i++) app_key(&app, KEY_F3);
    app_key(&app, KEY_F1); app_key(&app, KEY_F1);
    sim_board_run(&app, 4000);
    reboot();
    TEST_ASSERT_EQUAL_UINT8(2, app.dim_idx);
    TEST_ASSERT_EQUAL_UINT32(57600, sim_board_baud());
    TEST_ASSERT_EQUAL_UINT8(2, app.span_idx);

    uint8_t rec[SETTINGS_MAX_LEN];
    size_t n = settings_pack(&app, rec, sizeof rec);
    TEST_ASSERT_EQUAL_size_t(45, n);
    app_t fresh;
    app_init(&fresh, sim_board_platform());
    fresh.dim_idx = 0;
    settings_apply(&fresh, rec, 22, SETTINGS_VERSION);            /* a pre-menu record */
    TEST_ASSERT_EQUAL_UINT8(0, fresh.dim_idx);                    /* new fields untouched */
    settings_apply(&fresh, rec, n, SETTINGS_VERSION);
    TEST_ASSERT_EQUAL_UINT8(2, fresh.dim_idx);
}

/* B18: the diode beep threshold is a setting (stock has one; was fixed at
 * 0.7 V). An LED's 1.8 V drop beeps only once the threshold is above it. */
static void test_diode_threshold_setting(void)
{
    char v[20];
    menu_value(&app, MI_DIODE_TH, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("0.7 V", v);
    app_key(&app, KEY_OHM); app_key(&app, KEY_OHM); app_key(&app, KEY_OHM);   /* diode */
    unsigned before = sim_board_beeps();
    w->diode = 1.8;                                       /* a red LED */
    sim_board_run(&app, 600);
    TEST_ASSERT_EQUAL_UINT(before, sim_board_beeps());    /* above 0.7 V: silent */

    menu_goto(MI_DIODE_TH);
    for (int i = 0; i < 12; i++) app_key(&app, KEY_RIGHT); /* 0.7 -> 1.9 V */
    menu_value(&app, MI_DIODE_TH, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("1.9 V", v);
    app_key(&app, KEY_UTILITY);
    w->diode = 1e9;                                       /* leads off, then the LED again */
    sim_board_run(&app, 600);
    w->diode = 1.8;
    sim_board_run(&app, 600);
    TEST_ASSERT_EQUAL_UINT(before + 1, sim_board_beeps());

    for (int i = 0; i < 40; i++) menu_adjust(&app, MI_DIODE_TH, +1);
    TEST_ASSERT_EQUAL_UINT8(APP_DIODE_TH_MAX, app.diode_th_dv);   /* clamped at 3.0 V */
    for (int i = 0; i < 40; i++) menu_adjust(&app, MI_DIODE_TH, -1);
    TEST_ASSERT_EQUAL_UINT8(APP_DIODE_TH_MIN, app.diode_th_dv);
    menu_adjust(&app, MI_DIODE_TH, +1);
    app.dirty = true;
    sim_board_run(&app, 4000);
    reboot();
    TEST_ASSERT_EQUAL_UINT8(2, app.diode_th_dv);                  /* persisted */
}

/* B15: auto-hold freezes the display on a stable reading and beeps once;
 * it keeps the reading when the probes come off and takes a new stable
 * one. Only the display holds: SCPI stays live. */
static void test_auto_hold(void)
{
    app_key(&app, KEY_RUNSTOP);                              /* Run -> Hold */
    TEST_ASSERT_TRUE(app.hold);
    app_key(&app, KEY_RUNSTOP);                              /* Hold -> Auto-hold */
    TEST_ASSERT_FALSE(app.hold);
    TEST_ASSERT_TRUE(app.autohold);
    sim_board_run(&app, 50);
    TEST_ASSERT_NOT_NULL(strstr(app.view.status, "A-HOLD"));

    unsigned b0 = sim_board_beeps();
    w->dcv = 3.3;
    sim_board_run(&app, 2500);
    READS("3.3000");
    TEST_ASSERT_TRUE(app.ah_holding);
    TEST_ASSERT_EQUAL_UINT(b0 + 1, sim_board_beeps());       /* one beep for the hold */
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_UINT(b0 + 1, sim_board_beeps());       /* steady: no more */

    w->dcv = 0.0;                                            /* probes off */
    sim_board_run(&app, 3000);
    READS("3.3000");                                         /* still held */
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, atof(ask("MEAS?", 5)));   /* SCPI live */
    TEST_ASSERT_EQUAL_UINT(b0 + 1, sim_board_beeps());       /* idle leads don't hold */

    w->dcv = 1.5;                                            /* a new point */
    sim_board_run(&app, 3000);
    READS("1.5000");
    TEST_ASSERT_EQUAL_UINT(b0 + 2, sim_board_beeps());

    app_key(&app, KEY_RUNSTOP);                              /* Auto-hold -> Run: live again */
    w->dcv = 0.7;
    sim_board_run(&app, 1500);
    READS("0.7000");
    TEST_ASSERT_NULL(strstr(app.view.status, "A-HOLD"));
}

/* B37: the bar graph under the reading, off by default. */
static void test_bar_graph(void)
{
    w->dcv = 2.5;
    sim_board_run(&app, 1500);
    TEST_ASSERT_FALSE(app.view.bar);                         /* off by default */
    menu_goto(MI_BARGRAPH);
    app_key(&app, KEY_RIGHT);
    app_key(&app, KEY_UTILITY);
    sim_board_run(&app, 600);
    TEST_ASSERT_TRUE(app.view.bar);
    TEST_ASSERT_TRUE(app.view.bar_bipolar);                  /* DC: zero in the middle */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, app.view.bar_frac);      /* 2.5 V of 5 V */
    w->dcv = -1.0;
    sim_board_run(&app, 1500);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.2f, app.view.bar_frac);
    app_key(&app, KEY_OHM);
    w->ohm = 4000.0;
    sim_board_run(&app, 3000);
    TEST_ASSERT_FALSE(app.view.bar_bipolar);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.8f, app.view.bar_frac);      /* 4 k of 5 k */
    app_key(&app, KEY_FREQ);
    sim_board_run(&app, 2000);
    TEST_ASSERT_FALSE(app.view.bar);                         /* no full scale */
}

/* B57: beeper pitch, volume and style from the menu and SCPI; continuity
 * sounds at the chosen pitch; settings persist. */
static void test_beep_tone_control(void)
{
    menu_goto(MI_BEEP_PITCH);
    app_key(&app, KEY_LEFT);                                 /* 3000 -> 2500 Hz */
    sim_board_run(&app, 20);
    TEST_ASSERT_EQUAL_UINT16(2500, sim_board_tone_hz());     /* the preview plays */
    TEST_ASSERT_TRUE(sim_board_beeping());
    menu_goto(MI_BEEP_VOL);
    for (int i = 0; i < 5; i++) app_key(&app, KEY_LEFT);     /* 100 -> 50% */
    sim_board_run(&app, 20);
    TEST_ASSERT_EQUAL_UINT8(50, sim_board_tone_level());
    menu_goto(MI_BEEP_STYLE);
    app_key(&app, KEY_RIGHT);                                /* CLASSIC -> SOFT */
    char v[20];
    menu_value(&app, MI_BEEP_STYLE, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("SOFT", v);
    app_key(&app, KEY_UTILITY);
    sim_board_run(&app, 1000);
    TEST_ASSERT_FALSE(sim_board_beeping());                  /* previews end */

    app_key(&app, KEY_OHM); app_key(&app, KEY_OHM);          /* continuity */
    w->ohm = 5.0;
    sim_board_run(&app, 500);
    TEST_ASSERT_TRUE(sim_board_beeping());
    TEST_ASSERT_EQUAL_UINT16(2500, sim_board_tone_hz());
    TEST_ASSERT_EQUAL_UINT8(50, sim_board_tone_level());

    ask("SYST:BEEP:FREQ 1234", 5);                           /* SCPI: any pitch */
    TEST_ASSERT_EQUAL_STRING("1234", ask("SYST:BEEP:FREQ?", 5));
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_UINT16(1234, sim_board_tone_hz());
    ask("SYST:BEEP:VOL 80", 5);
    TEST_ASSERT_EQUAL_STRING("80", ask("SYST:BEEP:VOL?", 5));
    ask("SYST:BEEP:STYL CHIRP", 5);
    TEST_ASSERT_EQUAL_STRING("CHIRP", ask("SYST:BEEP:STYL?", 5));
    TEST_ASSERT_EQUAL_STRING("50", ask("SYST:BEEP:GLID?", 5));      /* the preset's glide */
    ask("SYST:BEEP:REL 120", 5);                              /* any value: CUSTOM */
    TEST_ASSERT_EQUAL_STRING("CUSTOM", ask("SYST:BEEP:STYL?", 5));
    TEST_ASSERT_EQUAL_STRING("120", ask("SYST:BEEP:REL?", 5));
    ask("SYST:BEEP:CLIC 50", 5);
    menu_goto(MI_BEEP_ATTACK);
    app_key(&app, KEY_RIGHT);                                 /* menu: 8 -> 15 ms */
    app_key(&app, KEY_UTILITY);
    char ev[20];
    menu_value(&app, MI_BEEP_ATTACK, ev, sizeof ev);
    TEST_ASSERT_EQUAL_STRING("15 ms", ev);
    ask("SYST:BEEP:FREQ 20", 5);                             /* out of range: refused */
    TEST_ASSERT_EQUAL_STRING("1234", ask("SYST:BEEP:FREQ?", 5));
    TEST_ASSERT_EQUAL_STRING("-224", strtok((char *)ask("SYST:ERR?", 5), ","));

    w->ohm = 1e9;                                            /* quiet, then play one */
    sim_board_run(&app, 500);
    unsigned b = sim_board_beeps();
    ask("SYST:BEEP:PLAY HOLD", 5);
    sim_board_run(&app, 20);
    TEST_ASSERT_EQUAL_UINT(b + 1, sim_board_beeps());

    app.dirty = true;
    sim_board_run(&app, 4000);
    reboot();
    TEST_ASSERT_EQUAL_UINT16(1234, app.snd.pitch_hz);
    TEST_ASSERT_EQUAL_UINT8(80, app.snd.volume);
    TEST_ASSERT_EQUAL_INT(SND_CUSTOM, app.snd.style);
    TEST_ASSERT_EQUAL_UINT8(15, app.snd.attack_ms);
    TEST_ASSERT_EQUAL_UINT16(120, app.snd.release_ms);
    TEST_ASSERT_EQUAL_UINT8(50, app.snd.glide_pct);
    TEST_ASSERT_EQUAL_UINT8(50, app.snd.click_ms);
}

/* B27: two-wire PT100 reads the leads too (about 2.6 C per ohm). The lead
 * resistance is subtracted: set in the menu, or nulled over SCPI with the
 * probe's leads shorted at the sensor. */
static void test_pt100_lead_correction(void)
{
    app_key(&app, KEY_TEMP);
    app_key(&app, KEY_F5);                                    /* SENSOR: PT100 */
    w->temp_c = 100.0;
    w->lead_ohm = 0.8;
    sim_board_run(&app, 3000);
    double off = atof(app.view.reading);
    TEST_ASSERT_DOUBLE_WITHIN(0.3, 102.1, off);              /* 0.8 ohm reads ~2 C hot */
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "LEADS 0.00"));

    ask("TEMP:RTD:LEAD NULL", 5);                             /* not shorted: refused */
    TEST_ASSERT_EQUAL_STRING("-221", strtok((char *)ask("SYST:ERR?", 5), ","));
    w->rtd_shorted = 1;                                       /* short the probe's leads */
    sim_board_run(&app, 3000);
    ask("TEMP:RTD:LEAD NULL", 5);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.8, atof(ask("TEMP:RTD:LEAD?", 5)));
    w->rtd_shorted = 0;
    sim_board_run(&app, 3000);
    READS("100.0");                                           /* corrected */

    menu_goto(MI_RTD_LEADS);                                  /* the menu steps 0.05 ohm */
    app_key(&app, KEY_LEFT);
    app_key(&app, KEY_UTILITY);
    char v[20];
    menu_value(&app, MI_RTD_LEADS, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("0.75 \xCE\xA9", v);
    app.dirty = true;
    sim_board_run(&app, 4000);
    reboot();
    TEST_ASSERT_EQUAL_UINT16(750, app.rtd_lead_mohm);         /* persisted */
    w->lead_ohm = 0.0;
}

/* B19: the 10 A input may carry over 7 A for 30 s, then needs 30 s off.
 * After 30 s the meter warns and alarms every second; once the load is off
 * it counts down the rest. Below 7 A, or on the mA jack, nothing. */
static void test_ten_amp_duty_warning(void)
{
    app_key(&app, KEY_I);
    app_key(&app, KEY_F5);                                    /* INPUT: the A jack */
    w->jack_a = 1;
    w->dci = 5.0;
    sim_board_run(&app, 40000);
    TEST_ASSERT_NULL(strstr(app.view.sub, "7 A"));            /* 5 A: no limit */
    unsigned b = sim_board_beeps();
    w->dci = 8.0;
    sim_board_run(&app, 25000);
    TEST_ASSERT_EQUAL_UINT(b, sim_board_beeps());             /* under 30 s: quiet */
    sim_board_run(&app, 10000);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "remove the load"));
    TEST_ASSERT_TRUE(sim_board_beeps() >= b + 4);             /* alarm each second */
    w->dci = 0.0;
    sim_board_run(&app, 2000);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "rest"));
    unsigned quiet = sim_board_beeps();
    sim_board_run(&app, 30000);
    TEST_ASSERT_EQUAL_STRING("", app.view.sub);               /* rested */
    TEST_ASSERT_EQUAL_UINT(quiet, sim_board_beeps());
    w->jack_a = 0;
}

/* B29: a cold-junction offset for a meter whose on-chip sensor reads off
 * from the terminals (the bench measures it: docs/test-plan.md T-TEMP-CJ). */
static void test_cold_junction_offset(void)
{
    app_key(&app, KEY_TEMP);
    w->temp_c = 100.0;
    w->cj_c = 25.0;
    sim_board_run(&app, 4000);
    READS("100.0");
    menu_goto(MI_CJ_OFFSET);
    for (int i = 0; i < 15; i++) app_key(&app, KEY_RIGHT);   /* +1.5 C */
    app_key(&app, KEY_UTILITY);
    char v[20];
    menu_value(&app, MI_CJ_OFFSET, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("+1.5 \xC2\xB0" "C", v);
    sim_board_run(&app, 4000);
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 26.5, app.acq.t_cj);       /* re-measured with it */
    TEST_ASSERT_DOUBLE_WITHIN(0.15, 101.5, atof(app.view.reading));
    app.dirty = true;
    sim_board_run(&app, 4000);
    reboot();
    TEST_ASSERT_EQUAL_INT8(15, app.cj_offset_dc);
}

/* B20: with the frequency known (dual display), AC above ~4 kHz is flagged:
 * the analog front end reads low there; on our 5 V range above 500 Hz. */
static void test_ac_band_flag(void)
{
    app_key(&app, KEY_V);                                     /* AC volts */
    w->acv = 10.0;                                            /* the 50 V range */
    w->freq = 1000.0;
    app_key(&app, KEY_DUAL);
    app_key(&app, KEY_F1);                                    /* the frequency */
    sim_board_run(&app, 4000);
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_ACV]);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "FREQ"));
    TEST_ASSERT_NULL(strstr(app.view.sub, "reads low"));
    w->freq = 6000.0;
    sim_board_run(&app, 4000);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(app.view.sub, "> 4 kHz: AC reads low"), app.view.sub);
    w->acv = 1.0;                                             /* the 5 V range */
    w->freq = 300.0;
    sim_board_run(&app, 6000);
    TEST_ASSERT_EQUAL_INT(1, app.range[MEAS_ACV]);
    TEST_ASSERT_NULL_MESSAGE(strstr(app.view.sub, "reads low"), app.view.sub);
    w->freq = 1000.0;
    sim_board_run(&app, 4000);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(app.view.sub, "> 500 Hz: AC reads low"), app.view.sub);
}

/* B28: PT1000 is the third sensor (TEMP and F5 cycle K -> PT100 -> PT1000),
 * read on the 5 kohm range. */
static void test_pt1000(void)
{
    app_key(&app, KEY_TEMP);
    app_key(&app, KEY_F5);
    app_key(&app, KEY_F5);
    w->temp_c = 100.0;
    sim_board_run(&app, 3000);
    TEST_ASSERT_EQUAL_STRING("PT1000", app.view.key_value[4]);
    READS("100.0");
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 1385.1, app.rtd_ohm);
    TEST_ASSERT_EQUAL_STRING("PT1000", ask("TEMP:RTD:TYP?", 5));
    app_key(&app, KEY_F5);                                    /* back to K */
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("K", app.view.key_value[4]);
    ask("CONF:TEMP PT1000", 5);
    sim_board_run(&app, 3000);
    READS("100.0");
}

/* B35: a recording can roll over its oldest readings instead of stopping
 * when the log is full. */
static void test_rolling_recording(void)
{
    app_key(&app, KEY_RECORD);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("STOP", app.view.key_value[3]);   /* WHEN FULL */
    app_key(&app, KEY_F4);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("ROLL", app.view.key_value[3]);
    TEST_ASSERT_EQUAL_STRING("ROLL", ask("LOG:FULL?", 5));
    app_key(&app, KEY_F1);                                     /* start */
    TEST_ASSERT_TRUE(app.log.active && app.log.wrap);
    for (uint32_t i = 0; i < dlog_capacity(&app.log) + 2000; i++) dlog_append(&app.log, (float)i);
    float v;
    dlog_get(&app.log, app.log.count - 1, &v);
    TEST_ASSERT_EQUAL_FLOAT((float)(dlog_capacity(&app.log) + 1999), v);   /* the newest is kept */
    sim_board_run(&app, 300);
    TEST_ASSERT_TRUE(app.log.active);                          /* never full */
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "ROLL"));
    ask("LOG:FULL STOP", 5);                                   /* not while recording */
    TEST_ASSERT_EQUAL_STRING("-221", strtok((char *)ask("SYST:ERR?", 5), ","));
    app_key(&app, KEY_F1);
}

/* B36: browse a recording on the meter: UP/DOWN move a cursor along the
 * plot, RANGE switches fine/coarse steps; the title shows the exact reading
 * under it, its number and its time. */
static void test_browse_recording(void)
{
    app_key(&app, KEY_RECORD);
    app_key(&app, KEY_F1);                                     /* start, 1 s interval */
    dlog_t *d = &app.log;
    while (d->count < 1000) dlog_append(d, (float)d->count * 0.01f);   /* 0.00 .. 9.99 */
    app_key(&app, KEY_F1);                                     /* stop */
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_INT(-1, app.view.cursor);
    app_key(&app, KEY_DOWN);                                   /* cursor on: the newest */
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_INT(app.rec_trace.n - 1, app.view.cursor);
    TEST_ASSERT_EQUAL_STRING_LEN("#1000: 9.99", app.view.sub, 11);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "+0:16:39"));    /* reading 1000 at 1 s */
    app_key(&app, KEY_LEFT);                                   /* RANGE: coarse */
    for (int i = 0; i < 40; i++) app_key(&app, KEY_DOWN);      /* to the start */
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_INT(0, app.view.cursor);
    TEST_ASSERT_EQUAL_STRING_LEN("#1: 0 ", app.view.sub, 6);
    app_key(&app, KEY_LEFT);                                   /* fine again */
    app_key(&app, KEY_UP);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_INT(1, app.view.cursor);
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.fn);                   /* UP/DOWN didn't change range */
    app_key(&app, KEY_F5);                                     /* EXIT clears it */
    TEST_ASSERT_EQUAL_INT(-1, app.rec_cursor);
}

/* B38/B58: VIEW cycles NUM -> NUM+G -> NUM+H -> GRAPH -> HISTO -> NUM; the
 * histogram is drawn from the graph's trace (and the panel stays current
 * through the cycle). */
static void test_histogram_view(void)
{
    w->dcv = 2.5;
    w->noise_counts = 600;
    for (int i = 0; i < 4; i++) app_key(&app, KEY_F3);
    sim_board_run(&app, 20000);
    TEST_ASSERT_EQUAL_STRING("HISTO", app.view.key_value[2]);
    panel_is_current("histogram");
    app_key(&app, KEY_F3);
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("NUM", app.view.key_value[2]);
    TEST_ASSERT_EQUAL_INT(SCREEN_MEASURE, app.screen);
    w->noise_counts = 0;
}

/* B31: user calibration over SCPI. Zero with the leads shorted, gain against
 * a reference; stored, survives a reboot, cleared back to factory; bounded. */
static void wait_cal(void)
{
    for (int i = 0; i < 40 && !strcmp(ask("CAL:USER:STAT?", 5), "BUSY"); i++) sim_board_run(&app, 500);
}

static void test_user_calibration(void)
{
    ask("CAL:USER:ZERO", 5);                                  /* autoranging: refused */
    TEST_ASSERT_EQUAL_STRING("-221", strtok((char *)ask("SYST:ERR?", 5), ","));
    ask("CONF:VOLT:DC 5", 5);                                  /* 5 V range, manual */
    w->dcv = 0.0012;                                           /* "shorted" leads read 1.2 mV */
    sim_board_run(&app, 2000);
    ask("CAL:USER:ZERO", 5);
    wait_cal();
    TEST_ASSERT_EQUAL_STRING("DONE", ask("CAL:USER:STAT?", 5));
    sim_board_run(&app, 2000);
    TEST_ASSERT_DOUBLE_WITHIN(20e-6, 0.0, atof(ask("MEAS?", 5)));
    TEST_ASSERT_NOT_NULL(strstr(app.view.status, "UCAL"));

    w->dcv = 4.0012;                                           /* the reference says 4.0400 V */
    sim_board_run(&app, 2000);
    ask("CAL:USER:GAIN 4.04", 5);
    wait_cal();
    TEST_ASSERT_EQUAL_STRING("DONE", ask("CAL:USER:STAT?", 5));
    sim_board_run(&app, 2000);
    TEST_ASSERT_DOUBLE_WITHIN(100e-6, 4.04, atof(ask("MEAS?", 5)));
    w->dcv = 0.0012;                                           /* zero still holds with the gain */
    sim_board_run(&app, 2000);
    TEST_ASSERT_DOUBLE_WITHIN(20e-6, 0.0, atof(ask("MEAS?", 5)));
    TEST_ASSERT_NOT_NULL(strstr(ask("CAL:USER:LIST?", 5), "VOLT/3:"));

    reboot();                                                  /* stored */
    TEST_ASSERT_TRUE(ucal_active(&app.ucal, MEAS_DCV, 2));
    char v[24];
    menu_value(&app, MI_CAL, v, sizeof v);
    TEST_ASSERT_NOT_NULL(strstr(v, "USER 1") ? v : NULL);

    ask("CONF:VOLT:DC 5", 5);
    w->dcv = 4.0012;
    sim_board_run(&app, 2000);
    ask("CAL:USER:GAIN 4.5", 5);                               /* 12%: refused, nothing stored */
    wait_cal();
    TEST_ASSERT_EQUAL_STRING("ERR,-4", ask("CAL:USER:STAT?", 5));
    ask("CAL:USER:CLEAR", 5);                                  /* back to factory */
    sim_board_run(&app, 2000);
    TEST_ASSERT_DOUBLE_WITHIN(100e-6, 4.0012, atof(ask("MEAS?", 5)));
    TEST_ASSERT_EQUAL_STRING("NONE", ask("CAL:USER:LIST?", 5));
    w->dcv = 0.0;
}

/* B31 on a meter that recorded before: the user-calibration pages were the
 * top of the old 23-page log and hold its data. They read as no
 * calibration, and the first save takes them over. */
static void test_user_calibration_over_old_log_data(void)
{
    for (uint32_t a = UCAL_PAGE0; a < UCAL_PAGE1 + STORE_PAGE_SIZE; a += 4)
        sim_flash_io.program_word(a, 0x3F800000u + (a & 0xFFu));   /* floats, as the log wrote */
    reboot();
    TEST_ASSERT_EQUAL_INT(0, ucal_count(&app.ucal));
    app.ucal.slot[MEAS_DCV][2].g_ppm = 1234;
    TEST_ASSERT_EQUAL_INT(0, ucal_save(&app.ucal));
    reboot();
    TEST_ASSERT_EQUAL_INT32(1234, app.ucal.slot[MEAS_DCV][2].g_ppm);
}

/* A board with rows for only some ranges (the meter's capacitance: 2-4):
 * selecting, autoranging and the range keys stay inside them. */
static bool cap_2_to_4(meas_fn_t fn, int r) { return fn != MEAS_CAP || (r >= 2 && r <= 4); }

static void test_board_range_limits(void)
{
    sim_board_set_range_avail(cap_2_to_4);
    app_key(&app, KEY_CAP);
    w->cap = 10e-9;                                            /* belongs on range 0 */
    sim_board_run(&app, 4000);
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_CAP]);             /* lowest offered */
    w->cap = 2e-3;                                             /* belongs on range 5 */
    sim_board_run(&app, 14000);                                /* 2 and 3 time out (4 s each) */
    TEST_ASSERT_EQUAL_INT(4, app.range[MEAS_CAP]);
    TEST_ASSERT_TRUE(app.overload);                            /* says so, stays on 4 */
    app_key(&app, KEY_F2);                                     /* manual */
    app_key(&app, KEY_UP);
    TEST_ASSERT_EQUAL_INT(4, app.range[MEAS_CAP]);
    for (int i = 0; i < 4; i++) app_key(&app, KEY_DOWN);
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_CAP]);
    sim_board_set_range_avail(NULL);
}

/* A board offering AC current on the mA jack's 500 uA-50 mA only (our
 * meter until 2026-11-17): autoranging stops at 50 mA with an overload,
 * and the INPUT key (A jack) is greyed and does nothing. DC current keeps
 * both jacks. */
static bool aci_low(meas_fn_t fn, int r) { return fn != MEAS_ACI || r <= 2; }

static void test_board_aci_low_ranges(void)
{
    sim_board_set_range_avail(aci_low);
    app_key(&app, KEY_I);
    app_key(&app, KEY_I);
    TEST_ASSERT_EQUAL_INT(MEAS_ACI, app.fn);
    w->aci = 0.2;                                              /* belongs on 500 mA */
    sim_board_run(&app, 6000);
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_ACI]);
    TEST_ASSERT_TRUE(app.overload);
    app_key(&app, KEY_F5);                                     /* A jack: not offered */
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_ACI]);
    app_build_view(&app);
    TEST_ASSERT_TRUE(app.view.key_off & (1u << 4));
    app_key(&app, KEY_I);                                      /* DC current: both jacks */
    TEST_ASSERT_EQUAL_INT(MEAS_DCI, app.fn);
    app_build_view(&app);
    TEST_ASSERT_FALSE(app.view.key_off & (1u << 4));
    w->aci = 0;
    sim_board_set_range_avail(NULL);
}

/* A function the board can't measure yet (the meter's AC current) is never
 * selected: its key does nothing, SCPI refuses it, and a saved selection
 * falls back to DC volts. A simulated reading would look real. */
static bool no_aci(meas_fn_t fn) { return fn != MEAS_ACI; }

static void test_board_function_limits(void)
{
    sim_board_set_fn_avail(no_aci);
    app_key(&app, KEY_I);
    TEST_ASSERT_EQUAL_INT(MEAS_DCI, app.fn);
    app_key(&app, KEY_I);                                      /* would be AC current */
    TEST_ASSERT_EQUAL_INT(MEAS_DCI, app.fn);
    ask("CONF:CURR:AC", 50);
    TEST_ASSERT_EQUAL_INT(MEAS_DCI, app.fn);
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 50));
    sim_board_set_fn_avail(NULL);
}

/* AC waits for a fresh RMS sum (RMSF), not for AD2F, which the chip raises
 * at the AD2 rate: waiting on AD2F re-reads a stale sum (on the meter,
 * 2026-11-13). The model raises AD2F every ms and RMSF every 100 ms. */
static uint32_t fake_ms;
static uint32_t fake_millis(void) { return fake_ms; }

static void test_ac_waits_for_rms(void)
{
    platform_t p = *sim_board_platform();
    p.millis = fake_millis;
    acq_t a;
    acq_init(&a);
    acq_select(&a, MEAS_ACV, 2);
    w->acv = 1.0;
    int n = 0;
    acq_sample_t s;
    for (fake_ms = 0; fake_ms < 2000; fake_ms++) {
        sim_hy3131_tick_ms();
        if (acq_poll(&a, &p, &s)) {
            n++;
            TEST_ASSERT_DOUBLE_WITHIN(0.01, 1.0, s.value);
        }
    }
    TEST_ASSERT_INT_WITHIN(3, 18, n);          /* 20 sums in 2 s, 2 discarded */
}

/* Touching the probes: a brief low reading can send the hunt one range too
 * far down (on the meter, 50 Mohm -> 5 kohm for a 10 kohm part). The
 * overload that follows goes back up one range, not to the top. */
static void test_hunt_overshoot_goes_back_up_one(void)
{
    app_key(&app, KEY_OHM);
    w->ohm = 1e12;                                   /* open: the top range */
    sim_board_run(&app, 3000);
    int top = app.range[MEAS_OHM2];
    w->ohm = 1000.0;                                 /* the touch, briefly low */
    int went_below = 0, back_to_top = 0;
    for (int t = 0; t < 3000; t++) {
        sim_board_run(&app, 1);
        int r = app.range[MEAS_OHM2];
        if (r < 2 && !went_below) w->ohm = 10000.0;  /* gone by the time the hunt lands */
        if (r < 2) went_below = 1;
        if (went_below && r == top) back_to_top = 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(went_below, "the brief low reading should take it below 50 kohm");
    TEST_ASSERT_FALSE_MESSAGE(back_to_top, "the overshoot should not climb back to the top range");
    TEST_ASSERT_EQUAL_INT(2, app.range[MEAS_OHM2]);  /* 50 kohm */
    TEST_ASSERT_DOUBLE_WITHIN(20.0, 10000.0, app.value);
}

/* B58: the mini views keep the big reading and draw the trend or its
 * histogram under it; the panel redraws as the trace grows. */
static void test_mini_views_under_the_reading(void)
{
    w->dcv = 1.0;
    app_key(&app, KEY_F3);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("NUM+G", app.view.key_value[2]);
    TEST_ASSERT_EQUAL_INT(SCREEN_MEASURE, app.screen);
    sim_board_run(&app, 3000);
    w->dcv = 2.0;
    sim_board_run(&app, 3000);
    TEST_ASSERT_TRUE(app.trace.n >= 2);
    TEST_ASSERT_EQUAL_STRING("2.0000", app.view.reading);       /* the big reading is still there */
    panel_is_current("mini trend");
    app_key(&app, KEY_F3);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("NUM+H", app.view.key_value[2]);
    panel_is_current("mini histogram");
    app_key(&app, KEY_F3);
    TEST_ASSERT_EQUAL_INT(SCREEN_GRAPH, app.screen);
    TEST_ASSERT_EQUAL_UINT8(0, app.mini);
}

/* B33: calendar arithmetic against known Unix times. */
static void test_clock_calendar(void)
{
    static const struct { clock_civil_t c; uint32_t t; } k[] = {
        { { 2000, 1, 1, 0, 0, 0 }, 946684800u },
        { { 2024, 2, 29, 23, 59, 59 }, 1709251199u },
        { { 2026, 11, 15, 3, 12, 45 }, 1794712365u },
        { { 2105, 12, 31, 23, 59, 59 }, 4291747199u },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(k[i].t, clock_from_civil(&k[i].c));
        clock_civil_t c;
        clock_to_civil(k[i].t, &c);
        TEST_ASSERT_EQUAL_INT(k[i].c.year, c.year);
        TEST_ASSERT_EQUAL_INT(k[i].c.month, c.month);
        TEST_ASSERT_EQUAL_INT(k[i].c.day, c.day);
        TEST_ASSERT_EQUAL_INT(k[i].c.sec, c.sec);
    }
    clock_civil_t bad = { 2025, 2, 29, 0, 0, 0 };               /* not a leap year */
    TEST_ASSERT_EQUAL_UINT32(0, clock_from_civil(&bad));
    char b[24];
    clock_fmt(b, sizeof b, 1794712365u, 'T');
    TEST_ASSERT_EQUAL_STRING("2026-11-15T03:12:45", b);
}

/* B33: the clock is set over SCPI, survives a reboot (the RTC's cell), and
 * stamps recordings: LOG:TIME? and the browse cursor show clock time. */
static void test_clock_and_recording_timestamps(void)
{
    TEST_ASSERT_EQUAL_STRING("0,0,0", ask("SYST:DATE?", 50));
    TEST_ASSERT_EQUAL_STRING("NONE", ask("LOG:TIME?", 50));
    ask("SYST:DATE 2026,11,15", 50);
    ask("SYST:TIME 3,12,45", 50);
    TEST_ASSERT_EQUAL_STRING("0,\"No error\"", ask("SYST:ERR?", 50));
    TEST_ASSERT_EQUAL_STRING("2026,11,15", ask("SYST:DATE?", 50));
    ask("SYST:DATE 2025,2,29", 50);                             /* refused */
    TEST_ASSERT_EQUAL_STRING("-224,\"Illegal parameter value\"", ask("SYST:ERR?", 50));

    w->dcv = 1.0;
    ask("LOG:INT 1", 50);
    ask("LOG:STAR", 50);
    sim_board_run(&app, 5500);
    ask("LOG:STOP", 50);
    const char *t = ask("LOG:TIME?", 50);
    TEST_ASSERT_EQUAL_INT(0, strncmp(t, "2026-11-15T03:12:", 17));

    reboot();                                                   /* the clock runs on */
    TEST_ASSERT_EQUAL_STRING("2026,11,15", ask("SYST:DATE?", 50));
    TEST_ASSERT_EQUAL_INT(0, strncmp(ask("LOG:TIME?", 50), "2026-11-15T03:12:", 17));

    menu_goto(MI_CLOCK);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_INT(0, strncmp(app.view.menu_value[MI_CLOCK], "2026-11-15 03:1", 15));
    app_key(&app, KEY_UTILITY);
}

/* B32: Utility -> CALIBRATION, RIGHT: a page of every factory slot, with
 * the user correction on it and BAD for an implausible gain. */
static void test_calibration_page(void)
{
    TEST_ASSERT_EQUAL_INT(6 + 4 + 6 + 6 + 6 + 7, app_cal_rows());
    char l[24], v[40];
    meas_fn_t fn; int r;
    TEST_ASSERT_TRUE(app_cal_row(&app, 0, &fn, &r, l, sizeof l, v, sizeof v));
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, fn);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_STRING("G +0.00%  Z 0", v);               /* the simulator: unity */
    TEST_ASSERT_FALSE(app_cal_row(&app, app_cal_rows(), NULL, NULL, l, sizeof l, v, sizeof v));
    const char *fl = ask("CAL:FACT:LIST?", 50);
    TEST_ASSERT_EQUAL_INT(0, strncmp(fl, "DCV 50 mV,1.000000,0,OK;", 24));

    app.ucal.slot[MEAS_DCV][2].g_ppm = 120;                      /* a user correction: +0.012% */
    app_cal_row(&app, 2, NULL, NULL, l, sizeof l, v, sizeof v);
    TEST_ASSERT_NOT_NULL(strstr(v, "U +0.012%"));
    app.ucal.slot[MEAS_DCV][2].g_ppm = 0;

    menu_goto(MI_CAL);
    app_key(&app, KEY_RIGHT);
    TEST_ASSERT_TRUE(app.cal_page);
    app_key(&app, KEY_DOWN); app_key(&app, KEY_DOWN);
    TEST_ASSERT_EQUAL_UINT8(2, app.cal_sel);
    panel_is_current("calibration page");
    app_key(&app, KEY_F5);                                       /* back to the menu */
    TEST_ASSERT_FALSE(app.cal_page);
    TEST_ASSERT_TRUE(app.menu_open);
    app_key(&app, KEY_UTILITY);
}

/* B11: SPEED sets the AD1 rate. Over SCPI (RATE S/M/F) the board hook is
 * called, and samples arrive every ~246 / 123 / 61 ms. */
static int dcv_samples_in(uint32_t ms)
{
    platform_t p = *sim_board_platform();
    p.millis = fake_millis;
    acq_t a;
    acq_init(&a);
    acq_select(&a, MEAS_DCV, 2);
    w->dcv = 1.0;
    int n = 0;
    acq_sample_t s;
    for (fake_ms = 0; fake_ms < ms; fake_ms++) {
        sim_hy3131_tick_ms();
        if (acq_poll(&a, &p, &s)) n++;
    }
    return n;
}

static void test_speed_sets_the_conversion_rate(void)
{
    ask("RATE S", 50);
    int slow = dcv_samples_in(5000);
    ask("RATE F", 50);
    TEST_ASSERT_EQUAL_STRING("F", ask("RATE?", 50));
    int fast = dcv_samples_in(5000);
    ask("RATE M", 50);
    int mid = dcv_samples_in(5000);
    ask("RATE S", 50);
    TEST_ASSERT_INT_WITHIN(2, 18, slow);        /* 20 conversions in 5 s, 2 discarded */
    TEST_ASSERT_INT_WITHIN(2, 38, mid);
    TEST_ASSERT_INT_WITHIN(3, 79, fast);
}

/* While recording, the settings keys do nothing: they are drawn greyed
 * (owner, 2026-11-16). RECORD and EXIT stay live; TAKE POINT in manual. */
static void test_record_settings_greyed_while_recording(void)
{
    app_key(&app, KEY_RECORD);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_UINT8(0, app.view.key_off);
    app_key(&app, KEY_F1);                                    /* start */
    TEST_ASSERT_TRUE(app.log.active);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_UINT8(0x0E, app.view.key_off);          /* MODE, INTERVAL, WHEN FULL */
    app_key(&app, KEY_F1);                                    /* stop */
    app_key(&app, KEY_F2);                                    /* manual */
    app_key(&app, KEY_F1);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_UINT8(0x06, app.view.key_off);          /* TAKE POINT stays live */
    app_key(&app, KEY_F1);
    app_key(&app, KEY_F2);                                    /* back to auto */
    app_key(&app, KEY_F5);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_UINT8(0, app.view.key_off);
}

/* B24: the 15 ms interval records every conversion with the AD1 at
 * R22 0x12 (~65 a second); stopping puts SPEED's rate back. Functions off
 * the AD1 (AC volts) skip it. */
static void test_fast_recording(void)
{
    w->dcv = 1.25;
    sim_board_run(&app, 2000);
    ask("LOG:INT 0.015", 50);
    TEST_ASSERT_EQUAL_STRING("0.015", ask("LOG:INT?", 50));
    ask("LOG:STAR", 50);
    TEST_ASSERT_TRUE(app.rec_fast);
    uint32_t f0 = app.frames_drawn;
    sim_board_run(&app, 2000);
    /* Frames are paced (~150 ms each on the meter, which starved the keys). */
    TEST_ASSERT_TRUE_MESSAGE(app.frames_drawn - f0 <= 6, "a frame every 0.5 s while recording fast");
    uint32_t n = app.log.count;
    TEST_ASSERT_TRUE_MESSAGE(n > 100 && n < 140, "about 2 s / 15 ms readings");
    float f = 0;
    TEST_ASSERT_TRUE(dlog_get(&app.log, n - 1, &f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.25f, f);
    ask("LOG:STOP", 50);
    TEST_ASSERT_FALSE(app.rec_fast);
    TEST_ASSERT_EQUAL_INT(18, dcv_samples_in(5000) - 0);       /* back to ~246 ms (SPEED low) */

    app_key(&app, KEY_V);                                      /* AC volts: 15 ms is skipped */
    app_key(&app, KEY_RECORD);
    for (int i = 0; i < APP_NREC_INT; i++) {
        app_key(&app, KEY_F3);
        TEST_ASSERT_TRUE(app_rec_interval_ms[app.rec_int_idx] != 15u);
    }
    app_key(&app, KEY_F5);
}


/* ---- persistence ---------------------------------------------------------- */

static void reboot(void)
{
    sim_board_reboot();
    app_init(&app, sim_board_platform());
}

static void test_settings_survive_a_reboot(void)
{
    app_key(&app, KEY_OHM);            /* resistance */
    app_key(&app, KEY_DOWN);           /* manual, 500 ohm */
    set_theme_dark();
    menu_goto(MI_BACKLIGHT);           /* menu: backlight 100 -> 60 */
    for (int i = 0; i < 4; i++) app_key(&app, KEY_LEFT);
    app_key(&app, KEY_UTILITY);
    sim_board_run(&app, 4000);
    TEST_ASSERT_EQUAL_UINT32(1, app.saves);

    reboot();
    TEST_ASSERT_EQUAL_INT(MEAS_OHM2, app.fn);
    TEST_ASSERT_EQUAL_INT(0, app.range[MEAS_OHM2]);
    TEST_ASSERT_FALSE(app.autorange);
    TEST_ASSERT_EQUAL_STRING("dark", app.theme->name);
    TEST_ASSERT_EQUAL_UINT8(60, sim_board_backlight());
    w->ohm = 220.0;
    sim_board_run(&app, 1000);
    READS("220.00");
}

static void test_saves_are_debounced(void)
{
    for (int i = 0; i < 4; i++) { app_key(&app, KEY_UP); sim_board_run(&app, 500); }
    TEST_ASSERT_EQUAL_UINT32(0, app.saves);            /* still changing */
    sim_board_run(&app, 3500);
    TEST_ASSERT_EQUAL_UINT32(1, app.saves);            /* one save for the burst */
    sim_board_run(&app, 5000);
    TEST_ASSERT_EQUAL_UINT32(1, app.saves);            /* nothing new to save */
}

static void test_power_cut_during_save_keeps_old_settings(void)
{
    app_key(&app, KEY_V);                              /* ACV */
    sim_board_run(&app, 4000);
    app_key(&app, KEY_I);                              /* DCI, then cut power mid-save */
    TEST_ASSERT_EQUAL_UINT32(1, app.saves);
    static uint8_t before[SIM_FLASH_END - SIM_FLASH_BASE], after[sizeof before];
    sim_flash_io.read(SIM_FLASH_BASE, before, sizeof before);
    sim_flash_cut_after(3);
    sim_board_run(&app, 4000);
    sim_flash_cut_after(-1);
    sim_flash_io.read(SIM_FLASH_BASE, after, sizeof after);
    TEST_ASSERT_EQUAL_UINT32(1, app.saves);            /* the save did not complete... */
    TEST_ASSERT_TRUE(memcmp(before, after, sizeof before) != 0);   /* ...but was under way */
    reboot();
    TEST_ASSERT_EQUAL_INT(MEAS_ACV, app.fn);
}

static void test_fresh_board_boots_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.fn);
    TEST_ASSERT_TRUE(app.autorange);
    TEST_ASSERT_EQUAL_STRING("stock", app.theme->name);
    TEST_ASSERT_EQUAL_UINT32(0, sim_flash_violations());
}

/* ---- SCPI over the simulated serial port ------------------------------------ */

static char reply[128];

static const char *ask(const char *cmd, uint32_t ms)
{
    sim_board_serial_send(cmd);
    sim_board_run(&app, ms);
    reply[0] = 0;
    if (!sim_board_serial_take(reply, sizeof reply)) snprintf(reply, sizeof reply, "<none>");
    return reply;
}

static void test_scpi_identity_and_errors(void)
{
    TEST_ASSERT_EQUAL_STRING("OWON,XDM1241,SIM0001," APP_VERSION, ask("*IDN?", 5));
    TEST_ASSERT_EQUAL_STRING("0,\"No error\"", ask("SYST:ERR?", 5));
    sim_board_serial_send("BOGUS:CMD");
    TEST_ASSERT_EQUAL_STRING("-113,\"Undefined header\"", ask("SYST:ERR?", 5));
}

static void test_scpi_measure_formats(void)
{
    w->dcv = 1.2345;
    sim_board_run(&app, 1000);
    TEST_ASSERT_EQUAL_STRING("1.234500E+00", ask("MEAS?", 5));     /* stock's %E, SI */
    TEST_ASSERT_EQUAL_STRING("NONe", ask("MEAS2?", 5));           /* stock's string */
    ask("CONF:VOLT 0.5", 1500);                                   /* manual 500 mV */
    w->dcv = 0.7;
    sim_board_run(&app, 1000);
    TEST_ASSERT_EQUAL_STRING("1E+9", ask("MEAS?", 5));            /* stock's overload */
    TEST_ASSERT_EQUAL_STRING("500 mV", ask("RANGE?", 5));       /* stock's form, "5 V" */
    /* Every form stock V4.3.0 was seen to send (2026-11-08) */
    char t[24];
    static const struct { meas_fn_t fn; int r; bool per; const char *want; } st[] = {
        { MEAS_DCV, 1, false, "500 mV" }, { MEAS_DCV, 2, false, "5 V" },
        { MEAS_ACV, 1, false, "5 V" },    { MEAS_OHM2, 0, false, "500 \xce\xa9" },
        { MEAS_OHM2, 1, false, "5 K\xce\xa9" }, { MEAS_OHM2, 2, false, "50 K\xce\xa9" },
        { MEAS_DCI, 0, false, "500 uA" }, { MEAS_DCI, 4, false, "5 A" },
        { MEAS_ACI, 0, false, "500 uA" }, { MEAS_CAP, 0, false, "50 nF" },
        { MEAS_FREQ, 0, false, "Hz" },    { MEAS_TEMP, 0, false, "KITS90" },
        /* second session */
        { MEAS_OHM2, 3, false, "500 K\xce\xa9" }, { MEAS_OHM2, 4, false, "5 M\xce\xa9" },
        { MEAS_OHM2, 5, false, "50 M\xce\xa9" },  { MEAS_DCI, 5, false, "10 A" },
        { MEAS_ACI, 5, false, "10 A" },     { MEAS_CAP, 1, false, "500 nF" },
        { MEAS_CAP, 2, false, "5uF" },      { MEAS_CAP, 3, false, "50uF" },
        { MEAS_CAP, 4, false, "500uF" },    { MEAS_CAP, 5, false, "5 mF" },
        { MEAS_CAP, 6, false, "50 mF" },    { MEAS_FREQ, 0, true, "s" },
        { MEAS_TEMP, 1, false, "Pt100" },
    };
    for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
        app_scpi_range_text(t, sizeof t, st[i].fn, st[i].r, st[i].per);
        TEST_ASSERT_EQUAL_STRING(st[i].want, t);
    }
    TEST_ASSERT_EQUAL_STRING("0", ask("AUTO?", 5));
}

static void test_scpi_functions_and_func_query(void)
{
    static const struct { const char *conf, *func; } t[] = {
        { "CONF:VOLT:AC", "\"VOLT AC\"" }, { "CONF:CURR", "\"CURR\"" },
        { "CONF:RES", "\"RES\"" }, { "CONF:FRES", "\"RES\"" },    /* no 4-wire, as stock */
        { "CONF:CONT", "\"CONT\"" }, { "CONF:DIOD", "\"DIOD\"" },  /* not swapped */
        { "CONF:CAP", "\"CAP\"" }, { "CONF:FREQ", "\"FREQ\"" },
        { "CONF:PER", "\"PER\"" }, { "CONF:TEMP PT100", "\"TEMP\"" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        sim_board_serial_send(t[i].conf);
        TEST_ASSERT_EQUAL_STRING(t[i].func, ask("FUNC?", 5));
    }
    w->temp_c = 100.0;
    ask("TEMP:RTD:UNIT F", 1500);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 212.0, atof(ask("MEAS?", 5)));   /* ADC quantisation */
    TEST_ASSERT_EQUAL_STRING("\xC2\xB0" "F", app.view.unit);
}

static void test_scpi_rate_rel_stats_remote(void)
{
    TEST_ASSERT_EQUAL_STRING("S", ask("RATE?", 5));
    ask("RATE f", 5);                                             /* either case, as stock */
    TEST_ASSERT_EQUAL_STRING("F", ask("RATE?", 5));

    w->dcv = 2.0;
    sim_board_run(&app, 1000);
    ask("CALC:FUNC NULL", 5);
    w->dcv = 2.5;
    sim_board_run(&app, 1000);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.5, atof(ask("MEAS?", 5)));
    TEST_ASSERT_EQUAL_STRING("NULL", ask("CALC:FUNC?", 5));
    ask("CALC:STAT OFF", 5);
    ask("CALC:FUNC AVER", 2000);
    TEST_ASSERT_EQUAL_STRING("2.500000", ask("CALC:AVER:AVER?", 5));      /* stock's %f */

    ask("SYST:REM", 5);
    TEST_ASSERT_NOT_NULL(strstr(app.view.status, "RMT"));
    sim_board_press(KEY_V);                                       /* locked */
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.fn);
    sim_board_press(KEY_RUNSTOP);                                 /* back to local */
    sim_board_run(&app, 50);
    TEST_ASSERT_FALSE(app.remote);
}

/* Replies observed on stock V4.3.0 over serial (docs/scpi.md, 2026-11-08). */
static void test_scpi_matches_stock_v430(void)
{
    /* RANGE takes a 1-based range number; numbers off the ladder are refused */
    ask("RANGE 1", 1000);
    TEST_ASSERT_EQUAL_STRING("50 mV", ask("RANGE?", 5));
    TEST_ASSERT_EQUAL_STRING("0", ask("AUTO?", 5));
    ask("RANGE 6", 1000);
    TEST_ASSERT_EQUAL_STRING("1000 V", ask("RANGE?", 5));
    ask("RANGE 7", 1000);
    TEST_ASSERT_EQUAL_STRING("1000 V", ask("RANGE?", 5));
    ask("SYST:ERR?", 5);                                          /* drop the -224 */
    ask("AUTO 1", 1000);
    ask("CONF:VOLT:DC 50", 1000);                                 /* CONF still takes a value */
    TEST_ASSERT_EQUAL_STRING("50 V", ask("RANGE?", 5));
    ask("AUTO 1", 5);

    /* math */
    TEST_ASSERT_EQUAL_STRING("<none>", ask("CALC:FUNC?", 5));    /* silent when off */
    TEST_ASSERT_EQUAL_STRING("nan", ask("CALC:AVER:MIN?", 5));
    TEST_ASSERT_EQUAL_STRING("   0.00", ask("CALC:NULL:OFFS?", 5));
    TEST_ASSERT_EQUAL_STRING("00600", ask("CALC:DB:REF?", 5));   /* the dBm reference ohms */
    w->dcv = -0.0001;
    sim_board_run(&app, 1500);
    ask("CALC:FUNC AVER", 1500);
    TEST_ASSERT_EQUAL_STRING("AVERage", ask("CALC:FUNC?", 5));
    TEST_ASSERT_EQUAL_STRING("-0.000100", ask("CALC:AVER:MIN?", 5));
    ask("CALC:STAT OFF", 5);

    /* beeper: stock says NO for on */
    TEST_ASSERT_EQUAL_STRING("NO", ask("SYST:BEEP?", 5));
    ask("SYST:BEEP OFF", 5);
    TEST_ASSERT_EQUAL_STRING("OFF", ask("SYST:BEEP?", 5));
    ask("SYST:BEEP ON", 5);

    /* MEAS:SHOW?: the display, with its unit. Stock sat on 5 V for this
     * reading (ours autoranges to 50 mV), so compare on that range. */
    ask("RANGE 3", 1500);
    TEST_ASSERT_EQUAL_STRING("-0.0001VDC", ask("MEAS:SHOW?", 5));
    ask("RANGE 1", 1500);
    TEST_ASSERT_EQUAL_STRING("-00.100mVDC", ask("MEAS:SHOW?", 5));    /* padded to the range */
    ask("AUTO 1", 5);
    ask("CONF:VOLT:AC", 5);
    w->acv = 0.0;
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_STRING("000.00mVAC", ask("MEAS:SHOW?", 5));
    ask("CONF:RES", 5);
    w->ohm = 1e12;                                                /* open */
    sim_board_run(&app, 3000);
    TEST_ASSERT_EQUAL_STRING("overload", ask("MEAS:SHOW?", 5));
    TEST_ASSERT_EQUAL_STRING("1E+9", ask("MEAS?", 5));
    w->ohm = 2.2e6;                                               /* MOhm, GB2312 omega */
    sim_board_run(&app, 3000);
    TEST_ASSERT_EQUAL_STRING("2.2000M\xa6\xb8", ask("MEAS:SHOW?", 5));
    ask("CONF:FREQ", 5);
    w->freq = 50; w->duty = 50;
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_STRING("NONe", ask("MEAS2?", 5));          /* stock: NONe unless dual is on */
    ask("CONF:TEMP KITS90", 5);
    w->temp_c = 184.58; w->cj_c = 25.0;
    sim_board_run(&app, 4000);
    TEST_ASSERT_EQUAL_STRING("0", ask("AUTO?", 5));               /* stock: 0 where nothing ranges */
    const char *t = ask("MEAS:SHOW?", 5);
    TEST_ASSERT_EQUAL_STRING("\xa1\xe6", t + strlen(t) - 2);     /* GB2312 degree-C */
}

static void test_scpi_dbm(void)
{
    ask("CONF:VOLT:AC", 5);
    w->acv = sqrt(0.6);
    sim_board_run(&app, 2000);
    ask("CALC:FUNC DBM", 5);
    ask("CALC:DBM:REF 600", 1000);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, atof(ask("MEAS?", 5)));
    ask("CALC:DB:REF -3", 5);
    ask("CALC:FUNC DB", 1000);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 3.0, atof(ask("MEAS?", 5)));
    sim_board_serial_send("CONF:CURR");
    ask("CALC:FUNC DBM", 5);                                      /* not for current */
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
}

/* ---- dual display (stock parity: ACV/ACI + frequency, manual p. 27) --------- */

static void test_dual_display(void)
{
    app_key(&app, KEY_V);                                     /* AC volts */
    w->acv = 0.23; w->freq = 50.0; w->duty = 50.0;
    sim_board_run(&app, 2000);

    app_key(&app, KEY_DUAL);                                  /* the choice of secondary */
    TEST_ASSERT_TRUE(app.dual_menu);
    sim_board_run(&app, 10);
    TEST_ASSERT_EQUAL_STRING("FREQ", app.view.key_value[0]);
    app_key(&app, KEY_F1);                                    /* pick frequency */
    TEST_ASSERT_TRUE(app.dual);
    sim_board_run(&app, 4000);
    READS("230.00");
    TEST_ASSERT_EQUAL_STRING("DUAL", app.view.status);
    TEST_ASSERT_EQUAL_STRING("50.000 Hz   500 mV   FREQ", app.view.sub);
    TEST_ASSERT_EQUAL_STRING("EXIT", app.view.key_label[4]);

    /* serial, in stock's dual-display format */
    double v1, v2;
    TEST_ASSERT_EQUAL_INT(2, sscanf(ask("MEAS?", 5), "%lE,%lE", &v1, &v2));
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.23, v1);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 50.0, v2);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 50.0, atof(ask("MEAS2?", 5)));
    TEST_ASSERT_EQUAL_STRING("FREQ", ask("FUNC2?", 5));        /* unquoted */
    TEST_ASSERT_EQUAL_STRING("500 mV", ask("RANGE2?", 5));
    TEST_ASSERT_EQUAL_STRING("230.00mVAC,50.000HZ", ask("MEAS:SHOW?", 5));
    TEST_ASSERT_EQUAL_STRING("50.000HZ", ask("MEAS2:SHOW?", 5));

    /* both keep updating while the front end alternates */
    w->acv = 0.31; w->freq = 60.0;
    sim_board_run(&app, 4000);
    READS("310.00");
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 60.0, app.sec_value);

    app_key(&app, KEY_DUAL);                                  /* swap */
    sim_board_run(&app, 300);
    READS("60.000");
    TEST_ASSERT_EQUAL_STRING("Hz", app.view.unit);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "310.00 mV AC"));
    app_key(&app, KEY_DUAL);                                  /* and back */
    sim_board_run(&app, 300);
    READS("310.00");

    app_key(&app, KEY_F5);                                    /* EXIT */
    TEST_ASSERT_FALSE(app.dual);
    TEST_ASSERT_EQUAL_STRING("NONe", ask("FUNC2?", 5));
    TEST_ASSERT_EQUAL_STRING("NONe", ask("MEAS2?", 5));
    TEST_ASSERT_EQUAL_STRING("3.100000E-01", ask("MEAS?", 5));
}

static void test_dual_display_rules(void)
{
    app_key(&app, KEY_DUAL);                                  /* DC volts: AC only (B23), no FREQ */
    TEST_ASSERT_TRUE(app.dual_menu);
    sim_board_run(&app, 10);
    TEST_ASSERT_EQUAL_STRING("", app.view.key_value[0]);
    TEST_ASSERT_EQUAL_STRING("AC", app.view.key_value[1]);
    app_key(&app, KEY_F5);                                    /* cancel */
    TEST_ASSERT_FALSE(app.dual);
    ask("FUNC2 FREQ", 5);
    TEST_ASSERT_FALSE(app.dual);
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));

    app_key(&app, KEY_V);                                     /* AC volts, dual on over SCPI */
    w->acv = 0.23; w->freq = 50.0;
    sim_board_run(&app, 1500);
    app_key(&app, KEY_MATH);                                  /* statistics on */
    ask("FUNC2 FREQ", 5);
    TEST_ASSERT_TRUE(app.dual);
    TEST_ASSERT_EQUAL_INT(MATH_OFF, app.math);                /* dual turned math off */

    app_key(&app, KEY_MATH);                                  /* and math turns dual off */
    TEST_ASSERT_FALSE(app.dual);
    TEST_ASSERT_EQUAL_INT(MATH_STATS, app.math);

    app_key(&app, KEY_MATH); app_key(&app, KEY_MATH);         /* math off again */
    app_dual_on(&app);
    sim_board_run(&app, 1500);
    app_key(&app, KEY_F1);                                    /* REL: dual off, REL on */
    TEST_ASSERT_FALSE(app.dual);
    TEST_ASSERT_TRUE(app.rel);

    app_key(&app, KEY_F1);
    app_dual_on(&app);
    app_key(&app, KEY_I);                                     /* another function ends it */
    TEST_ASSERT_FALSE(app.dual);
    TEST_ASSERT_EQUAL_INT(MEAS_NFUNC, app.acq.fn2);           /* and the front end stops alternating */

    app_key(&app, KEY_V); app_key(&app, KEY_V);               /* back to AC volts */
    app_key(&app, KEY_DUAL);
    app_key(&app, KEY_F5);                                    /* cancel the choice */
    TEST_ASSERT_FALSE(app.dual_menu);
    TEST_ASSERT_FALSE(app.dual);
}

/* B23: the same input two ways: DC volts with the AC volts on a matching
 * range, and back; both readings keep updating; it follows the primary's
 * range; over SCPI too. */
static void test_dual_dc_and_ac_of_one_input(void)
{
    w->dcv = 3.3; w->acv = 0.12;
    sim_board_run(&app, 2000);
    app_key(&app, KEY_DUAL);
    app_key(&app, KEY_F2);                                    /* AC */
    TEST_ASSERT_TRUE(app.dual);
    TEST_ASSERT_EQUAL_INT(MEAS_ACV, app.dual_fn);
    sim_board_run(&app, 6000);
    READS("3.3000");
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.12, app.sec_value);
    TEST_ASSERT_EQUAL_INT(1, app.acq.range2);                 /* 5 V DC -> 5 V AC */
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "ACV"));
    TEST_ASSERT_EQUAL_STRING("AC", ask("FUNC2?", 5));
    double v1, v2;
    TEST_ASSERT_EQUAL_INT(2, sscanf(ask("MEAS?", 5), "%lE,%lE", &v1, &v2));
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 3.3, v1);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.12, v2);

    w->dcv = 30.0;                                            /* the primary ranges up ... */
    sim_board_run(&app, 6000);
    READS("30.000");
    TEST_ASSERT_EQUAL_INT(2, app.acq.range2);                 /* ... and the AC follows: 50 V */

    app_key(&app, KEY_DUAL);                                  /* swap: AC as the main reading */
    sim_board_run(&app, 400);
    TEST_ASSERT_NOT_NULL(strstr(app.view.unit, "AC"));
    app_key(&app, KEY_F5);
    TEST_ASSERT_FALSE(app.dual);

    app_key(&app, KEY_V);                                     /* AC volts: FREQ or DC */
    ask("FUNC2 DC", 5);
    TEST_ASSERT_TRUE(app.dual);
    TEST_ASSERT_EQUAL_INT(MEAS_DCV, app.dual_fn);
    ask("FUNC2 AC", 5);                                       /* AC of AC volts: refused */
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
}

/* B21: power on DC current. Volts alternate with the current one for
 * one, on their own range; watts big; energy and charge integrated. */
static void test_power_and_energy(void)
{
    app_key(&app, KEY_I);
    w->dci = 0.5; w->dcv = 12.0;
    sim_board_run(&app, 3000);
    app_key(&app, KEY_DUAL);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("POWER", app.view.key_value[2]);
    app_key(&app, KEY_F3);
    TEST_ASSERT_TRUE(app.power);
    sim_board_run(&app, 15000);                               /* volts range down to 50 V ... */
    TEST_ASSERT_EQUAL_INT(3, app.pw_vrange);                  /* ... 12 V stays on 50 V */
    TEST_ASSERT_TRUE(app.pw_have);
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 6.0, app.pw_w);           /* 0.5% */
    TEST_ASSERT_EQUAL_STRING("W", app.view.unit);
    TEST_ASSERT_EQUAL_STRING("12.000 V   500.00 mA", app.view.sub);
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub, "mA"));
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub2, "Wh"));
    TEST_ASSERT_NOT_NULL(strstr(app.view.status, "POWER"));
    TEST_ASSERT_EQUAL_STRING("POW", ask("FUNC2?", 5));
    double p = 0;
    TEST_ASSERT_EQUAL_INT(1, sscanf(ask("MEAS:POW?", 5), "%lE", &p));
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 6.0, p);

    /* An hour at 6 W: 6 Wh and 0.5 Ah, to 0.5%. */
    app_key(&app, KEY_F4);                                    /* CLEAR Wh */
    TEST_ASSERT_EQUAL_DOUBLE(0.0, app.pw_wh);
    sim_board_run(&app, 3600000);
    double t = app.pw_ms / 3600000.0;                         /* the time the integral covers */
    TEST_ASSERT_DOUBLE_WITHIN(0.002, 1.0, t);
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 6.0, app.pw_wh);
    TEST_ASSERT_DOUBLE_WITHIN(0.0025, 0.5, app.pw_ah);
    double wh = 0;
    TEST_ASSERT_EQUAL_INT(1, sscanf(ask("MEAS:ENER?", 5), "%lE", &wh));
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 6.0, wh);
    TEST_ASSERT_TRUE(strstr(app.view.sub2, "00:59:5") || strstr(app.view.sub2, "01:00:0"));   /* an hour */
    TEST_ASSERT_NOT_NULL(strstr(app.view.sub2, "Wh"));        /* 6.0000 Wh, 500.00 mAh */

    /* Current the other way (discharging, or the leads swapped): negative
     * watts, and the energy runs back down. */
    double before = app.pw_wh;
    w->dci = -0.5;
    sim_board_run(&app, 60000);
    TEST_ASSERT_TRUE(app.pw_w < -5.9);
    TEST_ASSERT_TRUE(app.pw_wh < before - 0.09);

    ask("CALC:ENER:CLE", 5);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, app.pw_wh);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, app.pw_ah);

    w->dcv = 3.7;                                             /* a cell: volts range down to 5 V */
    sim_board_run(&app, 15000);
    TEST_ASSERT_EQUAL_INT(2, app.pw_vrange);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, -1.85, app.pw_w);

    app_key(&app, KEY_F5);                                    /* EXIT */
    TEST_ASSERT_FALSE(app.power);
    TEST_ASSERT_FALSE(app.dual);
    ask("MEAS:POW?", 5);
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
}

/* B21 on our meter (2026-11-16): PA8 is on for DC current 50 / 500 mA and
 * DC volts 50 / 500 mV; moving it opens the current path. The board pairs
 * only ranges where it stays, and acquisition must never configure a volts
 * read that moves it while current is the function, through ranging too. */
static const bool dci_pa8[6] = { 0, 0, 1, 1, 0, 0 }, dcv_pa8[6] = { 1, 1, 0, 0, 0, 0 };
static bool relay_rule(int ir, int vr)
{
    if (ir < 0 || ir > 5 || vr > 5) return false;
    return vr < 0 || dci_pa8[ir] == dcv_pa8[vr];
}
static int cur_pa8 = -1, relay_breaks;
static void (*real_configure)(meas_fn_t, int, afe_aux_t);
static void watch_configure(meas_fn_t fn, int range, afe_aux_t aux)
{
    if (fn == MEAS_DCI) cur_pa8 = dci_pa8[range];
    else if (fn == MEAS_DCV && cur_pa8 >= 0 && dcv_pa8[range] != cur_pa8) relay_breaks++;
    else if (fn != MEAS_DCV) cur_pa8 = -1;
    real_configure(fn, range, aux);
}

static void test_power_never_moves_the_current_relay(void)
{
    static app_t pa;
    static platform_t p;
    p = *sim_board_platform();
    real_configure = p.afe_configure;
    p.afe_configure = watch_configure;
    sim_board_set_vi_avail(relay_rule, false);
    app_init(&pa, &p);
    w->dci = 0.095; w->dcv = 5.0;                             /* the bench: 5 V into 52 ohms */
    app_key(&pa, KEY_I);
    sim_board_run(&pa, 3000);
    TEST_ASSERT_EQUAL_INT(3, pa.range[MEAS_DCI]);             /* 500 mA */
    TEST_ASSERT_TRUE(app_power_on(&pa));
    TEST_ASSERT_TRUE(pa.pw_vrange <= 1);                      /* 50 / 500 mV only */
    relay_breaks = 0;
    sim_board_run(&pa, 8000);
    TEST_ASSERT_EQUAL_INT(0, relay_breaks);
    TEST_ASSERT_TRUE(pa.sec_over);                            /* 5 V on 500 mV: over range, honestly */

    w->dci = 0.001;                                           /* down to 5 mA: 5 V pairs again */
    sim_board_run(&pa, 10000);
    TEST_ASSERT_EQUAL_INT(1, pa.range[MEAS_DCI]);
    TEST_ASSERT_TRUE(pa.pw_vrange >= 2);
    TEST_ASSERT_TRUE(pa.pw_have);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.005, pa.pw_w);        /* 5 V x 1 mA */
    w->dci = 0.095;                                           /* and back up, through the hunt */
    sim_board_run(&pa, 10000);
    TEST_ASSERT_EQUAL_INT(3, pa.range[MEAS_DCI]);
    TEST_ASSERT_EQUAL_INT(0, relay_breaks);
    sim_board_set_vi_avail(NULL, false);
}

/* B21: a recording made in power mode holds the watts, and says so. */
static void test_power_is_recorded(void)
{
    app_key(&app, KEY_I);
    w->dci = 0.5; w->dcv = 12.0;
    sim_board_run(&app, 3000);
    TEST_ASSERT_TRUE(app_power_on(&app));
    sim_board_run(&app, 15000);
    TEST_ASSERT_TRUE(app.pw_have);
    ask("LOG:INT 1", 5);
    ask("LOG:STAR", 5);
    TEST_ASSERT_TRUE(app.log.active);
    TEST_ASSERT_FALSE(app.rec_fast);
    sim_board_run(&app, 5500);
    ask("LOG:STOP", 5);
    TEST_ASSERT_EQUAL_STRING("\"POW\"", ask("LOG:FUNC?", 5));
    TEST_ASSERT_TRUE(app.log.count >= 4);                     /* 1 s points; readings every ~0.5 s */
    float f = 0;
    TEST_ASSERT_TRUE(dlog_get(&app.log, app.log.count - 1, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 6.0f, f);                 /* 12 V x 0.5 A */
    app_key(&app, KEY_RECORD);
    app_build_view(&app);
    TEST_ASSERT_EQUAL_STRING("W", app.view.unit);
    app_key(&app, KEY_RECORD);

}

/* The meter's V input upsets the current reading above ~2.15 V (on the A
 * jack ~1.09 mA/V): power mode subtracts the board's figure for the volts
 * it read, from the current shown and from the watts and totals. */
static double leak_10ma_at_12v(int irange, double volts)
{
    return irange >= 4 && volts > 11.0 ? 0.010 : 0.0;
}

static void test_power_subtracts_the_v_input_leak(void)
{
    static app_t pa;
    static platform_t p;
    p = *sim_board_platform();
    p.vi_leak = leak_10ma_at_12v;
    app_init(&pa, &p);
    w->dci = 0.510; w->dcv = 12.0; w->jack_a = 1;             /* 0.5 A true, 10 mA from the V input */
    app_key(&pa, KEY_I);
    app_key(&pa, KEY_F5);                                     /* the A jack */
    sim_board_run(&pa, 3000);
    TEST_ASSERT_TRUE(app_power_on(&pa));
    sim_board_run(&pa, 15000);
    TEST_ASSERT_TRUE(pa.pw_have);
    TEST_ASSERT_DOUBLE_WITHIN(0.003, 6.0, pa.pw_w);           /* 12 V x 0.5 A, not 0.51 A */
    app_build_view(&pa);
    TEST_ASSERT_NOT_NULL(strstr(pa.view.sub, "0.500"));       /* the current shown: corrected too */
    w->jack_a = 0;
}

/* A board that can't alternate volts and current (the meter, until the
 * bench check): POWER isn't offered, and FUNC2 POW is refused. */
static void test_power_not_offered(void)
{
    sim_board_set_vi_avail(NULL, true);
    app_key(&app, KEY_I);
    sim_board_run(&app, 1000);
    app_key(&app, KEY_DUAL);
    sim_board_run(&app, 50);
    TEST_ASSERT_EQUAL_STRING("", app.view.key_value[2]);
    app_key(&app, KEY_F3);
    TEST_ASSERT_FALSE(app.power);
    ask("FUNC2 POW", 5);
    TEST_ASSERT_FALSE(app.power);
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
    app_key(&app, KEY_V);                                     /* and not on DC volts */
    sim_board_set_vi_avail(NULL, false);
    ask("FUNC2 POW", 5);
    TEST_ASSERT_FALSE(app.power);
}

/* ---- current: two inputs ------------------------------------------------------ */

static void test_current_ranges_stay_on_one_jack(void)
{
    app_key(&app, KEY_I);                                     /* DC current, mA jack */
    TEST_ASSERT_TRUE(app.range[MEAS_DCI] < READOUT_CURRENT_A_FIRST);
    w->dci = 0.3;
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_INT(3, app.range[MEAS_DCI]);            /* 500 mA */
    READS("300.00");

    w->dci = 3.0;                                             /* too much for the mA input */
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_INT(3, app.range[MEAS_DCI]);            /* never jumps to the A jack */
    TEST_ASSERT_TRUE(app.overload);
    app_key(&app, KEY_UP);
    TEST_ASSERT_EQUAL_INT(3, app.range[MEAS_DCI]);            /* nor by hand */

    app_key(&app, KEY_F5);                                    /* switch to the A jack */
    TEST_ASSERT_EQUAL_INT(READOUT_CURRENT_A_FIRST, app.range[MEAS_DCI]);
    w->jack_a = 1;                                            /* and move the lead */
    sim_board_run(&app, 2000);
    READS("3.0000");
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("A", app.view.key_value[4]);
    w->dci = 0.2;                                             /* small current, A jack: */
    sim_board_run(&app, 2000);
    TEST_ASSERT_EQUAL_INT(READOUT_CURRENT_A_FIRST, app.range[MEAS_DCI]);   /* stays on 5 A */
    app_key(&app, KEY_DOWN);
    TEST_ASSERT_EQUAL_INT(READOUT_CURRENT_A_FIRST, app.range[MEAS_DCI]);   /* as stock: DOWN stays */
    app_key(&app, KEY_F5);
    TEST_ASSERT_EQUAL_INT(READOUT_CURRENT_A_FIRST - 1, app.range[MEAS_DCI]);
}

/* ---- recording -------------------------------------------------------------- */

static void test_record_timed_session_survives_reboot(void)
{
    w->dcv = 1.5;
    sim_board_run(&app, 1000);
    app_key(&app, KEY_RECORD);
    TEST_ASSERT_EQUAL_INT(SCREEN_RECORD, app.screen);
    app_key(&app, KEY_F3);                                     /* interval 1 s -> 2 s */
    app_key(&app, KEY_F1);                                     /* start */
    TEST_ASSERT_TRUE(app.log.active);
    sim_board_run(&app, 10100);
    TEST_ASSERT_UINT32_WITHIN(1, 6, app.log.count);            /* t = 0, 2, 4, 6, 8, 10 s */
    TEST_ASSERT_EQUAL_STRING("REC 6/10668", app.view.sub);
    TEST_ASSERT_EQUAL_STRING("2 s", app.view.key_value[2]);
    TEST_ASSERT_EQUAL_STRING("V DC", app.view.unit);
    TEST_ASSERT_TRUE(app.rec_trace.n > 0);
    float f;
    TEST_ASSERT_TRUE(dlog_get(&app.log, 3, &f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.5f, f);

    app_key(&app, KEY_F5);                                     /* leave: still recording */
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("REC", app.view.status);
    uint32_t n = app.log.count;
    reboot();                                                  /* power off mid-session */
    TEST_ASSERT_FALSE(app.log.active);
    TEST_ASSERT_EQUAL_UINT32(n, app.log.count);
    TEST_ASSERT_EQUAL_UINT8(MEAS_DCV, app.log.fn);
    TEST_ASSERT_EQUAL_UINT32(2000, app_rec_interval_ms[app.rec_int_idx]);   /* setting persisted too */
}

static void test_record_manual_points(void)
{
    w->dcv = 0.25;
    sim_board_run(&app, 1000);
    app_key(&app, KEY_RECORD);
    app_key(&app, KEY_F2);                                     /* manual */
    app_key(&app, KEY_F1);
    sim_board_run(&app, 5000);
    TEST_ASSERT_EQUAL_UINT32(0, app.log.count);                /* no timer */
    TEST_ASSERT_EQUAL_STRING("POINT", app.view.key_value[3]);
    for (int i = 0; i < 3; i++) app_key(&app, KEY_F4);
    TEST_ASSERT_EQUAL_UINT32(3, app.log.count);
    app_key(&app, KEY_F3);                                     /* interval fixed while recording */
    TEST_ASSERT_EQUAL_UINT32(1000, app_rec_interval_ms[app.rec_int_idx]);
}

static void test_record_stops_when_the_quantity_changes(void)
{
    sim_board_run(&app, 1000);
    TEST_ASSERT_EQUAL_INT(0, app_rec_start(&app));
    sim_board_run(&app, 2100);
    app_key(&app, KEY_I);                                      /* another function */
    TEST_ASSERT_FALSE(app.log.active);
    TEST_ASSERT_EQUAL_UINT8(MEAS_DCV, app.log.fn);             /* the data stays */

    app_key(&app, KEY_FREQ);
    w->freq = 50;
    sim_board_run(&app, 2000);
    app_rec_start(&app);
    sim_board_run(&app, 1100);
    TEST_ASSERT_TRUE(app.log.active);
    app_key(&app, KEY_F5);                                     /* now period: another unit */
    sim_board_run(&app, 1100);
    TEST_ASSERT_FALSE(app.log.active);
}

static void test_record_logs_overload_and_scpi_reads_it(void)
{
    ask("CONF:VOLT 0.5", 1500);                                /* manual 500 mV */
    ask("LOG:INT MAN", 5);
    TEST_ASSERT_EQUAL_STRING("0", ask("LOG:INT?", 5));
    w->dcv = 0.3;
    sim_board_run(&app, 1000);
    ask("LOG:STAR", 5);
    ask("LOG:TRIG", 5);
    w->dcv = 2.0;                                              /* over range */
    sim_board_run(&app, 1000);
    ask("LOG:TRIG", 5);
    ask("LOG:STOP", 5);
    TEST_ASSERT_EQUAL_STRING("2", ask("LOG:POIN?", 5));
    TEST_ASSERT_EQUAL_STRING("\"VOLT\"", ask("LOG:FUNC?", 5));
    const char *d = ask("LOG:DATA?", 5);
    double v0, v1;
    TEST_ASSERT_EQUAL_INT(2, sscanf(d, "%lE,%lE", &v0, &v1));
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.3, v0);
    TEST_ASSERT_EQUAL_DOUBLE(1E+9, v1);                        /* overload, as MEAS? */
    TEST_ASSERT_EQUAL_STRING("1.000000E+09", ask("LOG:DATA? 1,5", 5));
    TEST_ASSERT_EQUAL_STRING("", ask("LOG:DATA? 7", 5));
    TEST_ASSERT_EQUAL_STRING("0,\"No error\"", ask("SYST:ERR?", 5));
}

/* INT:RECord? in the shape stock V4.3.0 sends (docs/scpi.md). */
static void test_int_record_matches_stock_format(void)
{
    TEST_ASSERT_EQUAL_STRING("start:0,end", ask("int:record?", 5));
    ask("LOG:INT MAN", 5);
    w->dcv = 1.7859;
    sim_board_run(&app, 1500);
    ask("LOG:STAR", 5);
    ask("LOG:TRIG", 5);
    w->dcv = -0.0051;
    sim_board_run(&app, 1500);
    ask("LOG:TRIG", 5);
    w->dcv = 20.0;
    sim_board_run(&app, 1500);
    ask("LOG:TRIG", 5);
    TEST_ASSERT_EQUAL_STRING("start:3,DCV,1.7859,VDC,DCV,-5.100,mVDC,DCV,20.000,VDC,end",
                             ask("INT:RECord?", 5));
}

static void test_scpi_log_settings_and_errors(void)
{
    ask("LOG:INT 4", 5);                                       /* nearest offered: 5 s */
    TEST_ASSERT_EQUAL_STRING("5", ask("LOG:INT?", 5));
    TEST_ASSERT_EQUAL_STRING("10668", ask("LOG:CAP?", 5));
    TEST_ASSERT_EQUAL_STRING("\"NONE\"", ask("LOG:FUNC?", 5));
    ask("LOG:STAR", 5);
    TEST_ASSERT_EQUAL_STRING("1", ask("LOG:STAT?", 5));
    ask("LOG:INT 1", 5);                                       /* refused while recording */
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
    ask("LOG:TRIG", 5);                                        /* timed mode: no manual points */
    TEST_ASSERT_EQUAL_STRING("-221,\"Settings conflict\"", ask("SYST:ERR?", 5));
    TEST_ASSERT_EQUAL_STRING("5", ask("LOG:INT?", 5));
}

static void test_record_without_storage(void)
{
    static platform_t bare;
    bare = *sim_board_platform();
    bare.log_flash = NULL;
    app_init(&app, &bare);
    TEST_ASSERT_EQUAL_INT(-1, app_rec_start(&app));
    app_key(&app, KEY_RECORD);
    sim_board_run(&app, 300);
    TEST_ASSERT_EQUAL_STRING("No log storage", app.view.sub);
    TEST_ASSERT_EQUAL_STRING("0", ask("LOG:CAP?", 5));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_overload_limits_from_manual);
    RUN_TEST(test_value_resolution);
    RUN_TEST(test_units_and_labels);
    RUN_TEST(test_autorange_rules);
    RUN_TEST(test_bus_model_round_trips_through_driver);
    RUN_TEST(test_band_render_matches_full_render);
    RUN_TEST(test_only_changed_bands_are_sent);
    RUN_TEST(test_dcv_reading);
    RUN_TEST(test_dcv_autoranges_down_and_up);
    RUN_TEST(test_dcv_manual_range_overload);
    RUN_TEST(test_acv_and_dbm);
    RUN_TEST(test_current);
    RUN_TEST(test_resistance_continuity_diode);
    RUN_TEST(test_cold_junction_refresh_adapts);
    RUN_TEST(test_diode_threshold_setting);
    RUN_TEST(test_auto_hold);
    RUN_TEST(test_bar_graph);
    RUN_TEST(test_beep_tone_control);
    RUN_TEST(test_pt100_lead_correction);
    RUN_TEST(test_ten_amp_duty_warning);
    RUN_TEST(test_cold_junction_offset);
    RUN_TEST(test_ac_band_flag);
    RUN_TEST(test_pt1000);
    RUN_TEST(test_rolling_recording);
    RUN_TEST(test_browse_recording);
    RUN_TEST(test_histogram_view);
    RUN_TEST(test_user_calibration);
    RUN_TEST(test_user_calibration_over_old_log_data);
    RUN_TEST(test_board_range_limits);
    RUN_TEST(test_board_aci_low_ranges);
    RUN_TEST(test_board_function_limits);
    RUN_TEST(test_speed_sets_the_conversion_rate);
    RUN_TEST(test_mini_views_under_the_reading);
    RUN_TEST(test_clock_calendar);
    RUN_TEST(test_calibration_page);
    RUN_TEST(test_fast_recording);
    RUN_TEST(test_record_settings_greyed_while_recording);
    RUN_TEST(test_capacitance_charge_slope);
    RUN_TEST(test_clock_and_recording_timestamps);
    RUN_TEST(test_hunt_overshoot_goes_back_up_one);
    RUN_TEST(test_ac_waits_for_rms);
    RUN_TEST(test_every_unit_renders_in_the_unit_font);
    RUN_TEST(test_capacitance_and_frequency);
    RUN_TEST(test_temperature);
    RUN_TEST(test_rel_hold_stats);
    RUN_TEST(test_noise_is_averaged);
    RUN_TEST(test_settings_survive_a_reboot);
    RUN_TEST(test_saves_are_debounced);
    RUN_TEST(test_power_cut_during_save_keeps_old_settings);
    RUN_TEST(test_fresh_board_boots_defaults);
    RUN_TEST(test_scpi_identity_and_errors);
    RUN_TEST(test_scpi_measure_formats);
    RUN_TEST(test_scpi_functions_and_func_query);
    RUN_TEST(test_scpi_rate_rel_stats_remote);
    RUN_TEST(test_scpi_dbm);
    RUN_TEST(test_scpi_matches_stock_v430);
    RUN_TEST(test_unchanged_frame_is_not_composed);
    RUN_TEST(test_panel_stays_current_through_every_screen);
    RUN_TEST(test_no_screen_uses_a_sixth_softkey);
    RUN_TEST(test_key_click);
    RUN_TEST(test_click_returns_to_the_continuity_tone);
    RUN_TEST(test_menu_edits_preferences);
    RUN_TEST(test_menu_function_key_closes_and_acts);
    RUN_TEST(test_menu_threshold_steps_and_snaps);
    RUN_TEST(test_menu_defaults_need_two_presses);
    RUN_TEST(test_menu_information_rows);
    RUN_TEST(test_baud_from_menu);
    RUN_TEST(test_backlight_dims_and_first_key_only_wakes);
    RUN_TEST(test_graph_softkeys);
    RUN_TEST(test_new_preferences_persist_and_old_records_load);
    RUN_TEST(test_current_ranges_stay_on_one_jack);
    RUN_TEST(test_dual_display);
    RUN_TEST(test_dual_display_rules);
    RUN_TEST(test_dual_dc_and_ac_of_one_input);
    RUN_TEST(test_power_and_energy);
    RUN_TEST(test_power_not_offered);
    RUN_TEST(test_power_never_moves_the_current_relay);
    RUN_TEST(test_power_is_recorded);
    RUN_TEST(test_power_subtracts_the_v_input_leak);
    RUN_TEST(test_record_timed_session_survives_reboot);
    RUN_TEST(test_record_manual_points);
    RUN_TEST(test_record_stops_when_the_quantity_changes);
    RUN_TEST(test_record_logs_overload_and_scpi_reads_it);
    RUN_TEST(test_scpi_log_settings_and_errors);
    RUN_TEST(test_record_without_storage);
    RUN_TEST(test_int_record_matches_stock_format);
    return UNITY_END();
}
