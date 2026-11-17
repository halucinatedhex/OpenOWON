#include <unity.h>
#include "ui.h"
#include <string.h>

static rgb565_t buf[SCR_W * SCR_H];
static fb_t f;

void setUp(void)    { fb_init(&f, buf, SCR_W, SCR_H); }
void tearDown(void) {}

static void test_trace_is_fifo(void)
{
    trace_t t; trace_init(&t);
    for (int i = 0; i < 10; i++) trace_push(&t, (float)i);
    TEST_ASSERT_EQUAL_INT(10, t.n);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, trace_at(&t, 0));
    TEST_ASSERT_EQUAL_FLOAT(9.0f, trace_at(&t, 9));
}

static void test_trace_wraps_and_keeps_newest(void)
{
    trace_t t; trace_init(&t);
    for (int i = 0; i < TRACE_MAX + 50; i++) trace_push(&t, (float)i);
    TEST_ASSERT_EQUAL_INT(TRACE_MAX, t.n);
    TEST_ASSERT_EQUAL_FLOAT((float)(TRACE_MAX + 49), trace_at(&t, TRACE_MAX - 1));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, trace_at(&t, 0));
}

static void test_trace_range_pads_and_handles_flat(void)
{
    trace_t t; trace_init(&t);
    for (int i = 0; i < 5; i++) trace_push(&t, 3.0f);   /* flat input */
    float lo, hi; trace_range(&t, &lo, &hi);
    TEST_ASSERT_TRUE(hi > lo);                           /* must not divide by zero */
    TEST_ASSERT_TRUE(lo < 3.0f && hi > 3.0f);
}

static void test_text_widths_add_up_and_grow_with_scale(void)
{
    TEST_ASSERT_EQUAL_INT(0, fb_text_w("", 1));
    TEST_ASSERT_EQUAL_INT(fb_text_w("A", 2) + fb_text_w("B", 2), fb_text_w("AB", 2));
    TEST_ASSERT_TRUE(fb_text_w("ABC", 1) < fb_text_w("ABC", 2));
    TEST_ASSERT_TRUE(fb_text_w("ABC", 2) < fb_text_w("ABC", 4));
    TEST_ASSERT_TRUE(fb_text_w("i", 2) < fb_text_w("W", 2));      /* proportional */
    TEST_ASSERT_TRUE(fb_text_h(1) < fb_text_h(2) && fb_text_h(2) < fb_text_h(4));
}

/* Units are spelled in ASCII below the UI (SCPI shares them); the screen
 * shows the symbols. A "u" inside a word stays a "u". */
static void test_unit_symbols_on_screen(void)
{
    TEST_ASSERT_EQUAL_INT(fb_text_w("k\xCE\xA9", 2), fb_text_w("kOhm", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("\xC2\xB5" "F", 2), fb_text_w("uF", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("5 \xC2\xB5" "A", 2), fb_text_w("5 uA", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("A", 2) + fb_text_w("u", 2) + fb_text_w("to", 2),
                          fb_text_w("Auto", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("O", 2) + fb_text_w("hms", 2), fb_text_w("Ohms", 2));
    TEST_ASSERT_EQUAL_INT(0, fb_text_missing("k\xCE\xA9 x", 2));     /* full ASCII at scale 2 */
    TEST_ASSERT_EQUAL_INT(1, fb_text_missing("x", 4));                /* the unit size is a subset */
}

/* Ink stays in the box fb_text_w and the font metrics claim, and edges are
 * blended (anti-aliased), not just on/off. */
/* Stock-style themes draw capital labels in Title Case; acronyms, units,
 * mixed-case words and single letters are left as they are. Width and
 * drawing agree because both go through the same transform. */
static void test_title_case_labels(void)
{
    ui_set_title_case(true);
    TEST_ASSERT_EQUAL_INT(fb_text_w("Auto 5 V", 2), fb_text_w("AUTO 5 V", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("Dim After", 2), fb_text_w("DIM AFTER", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("ACV", 2) + fb_text_w(" ", 2) + fb_text_w("DC", 2),
                          fb_text_w("ACV DC", 2));                 /* kept in capitals */
    TEST_ASSERT_EQUAL_INT(fb_text_w("dBm Reference", 2), fb_text_w("dBm REFERENCE", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("PT100 Leads", 2), fb_text_w("PT100 LEADS", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("A-Hold", 2), fb_text_w("A-HOLD", 2));
    TEST_ASSERT_EQUAL_INT(fb_text_w("k\xCE\xA9", 2), fb_text_w("kOhm", 2));   /* units still map */
    ui_set_title_case(false);
    TEST_ASSERT_TRUE(fb_text_w("AUTO", 2) > fb_text_w("Auto", 2));
}

static void test_label_text_is_bounded_and_antialiased(void)
{
    fb_clear(&f, 0);
    const int x = 100, y = 100, s = 2;
    const char *txt = "Hg\xCE\xA9";
    fb_text(&f, x, y, txt, s, 0xFFFF);
    int partial = 0, full = 0;
    for (int j = 0; j < SCR_H; j++)
        for (int i = 0; i < SCR_W; i++) {
            rgb565_t p = buf[j * SCR_W + i];
            if (!p) continue;
            TEST_ASSERT_TRUE(i >= x - 2 && i < x + fb_text_w(txt, s) + 2);
            TEST_ASSERT_TRUE(j >= y - fb_text_h(s) / 2 && j < y + 2 * fb_text_h(s));
            if (p == 0xFFFF) full++; else partial++;
        }
    TEST_ASSERT_TRUE(full > 20);
    TEST_ASSERT_TRUE(partial > 10);
}

static int count_colour(rgb565_t c, int y)
{
    int n = 0;
    for (int i = 0; i < SCR_W; i++) n += buf[y * SCR_W + i] == c;
    return n;
}

/* B37: bar length tracks the fraction; past full scale it turns warn. */
static void test_bar_graph_draws_to_scale(void)
{
    fb_clear(&f, 0);
    ui_bar(&f, &theme_dark, 10, 100, 300, 10, 0.5f, false);
    TEST_ASSERT_INT_WITHIN(1, 150, count_colour(theme_dark.accent, 105));
    fb_clear(&f, 0);
    ui_bar(&f, &theme_dark, 10, 100, 300, 10, -0.5f, true);  /* left of centre */
    TEST_ASSERT_INT_WITHIN(1, 75, count_colour(theme_dark.accent, 105));
    TEST_ASSERT_EQUAL_HEX16(theme_dark.accent, buf[105 * SCR_W + 10 + 150 - 10]);
    TEST_ASSERT_EQUAL_HEX16(0, buf[105 * SCR_W + 10 + 150 + 10]);
    fb_clear(&f, 0);
    ui_bar(&f, &theme_dark, 10, 100, 300, 10, 1.05f, false); /* over range */
    TEST_ASSERT_INT_WITHIN(1, 300, count_colour(theme_dark.warn, 105));
    TEST_ASSERT_EQUAL_INT(11, count_colour(theme_dark.grid, 112));   /* 11 ticks */
}

static void test_clipping_does_not_corrupt_memory(void)
{
    fb_clear(&f, 0);
    fb_px(&f, -5, -5, 0xFFFF);
    fb_px(&f, 10000, 10000, 0xFFFF);
    fb_rect(&f, -20, -20, 10, 10, 0xFFFF);
    for (int i = 0; i < SCR_W * SCR_H; i++) TEST_ASSERT_EQUAL_HEX16(0, buf[i]);
}

static void test_screens_actually_paint(void)
{
    /* A screen that leaves the buffer uniform has drawn nothing. */
    trace_t t; trace_init(&t);
    for (int i = 0; i < 40; i++) trace_push(&t, (float)(i % 7));
    screen_plot(&f, &theme_dark, &t, "PLOT", "VDC", NULL, 0, NULL);
    int distinct = 0;
    rgb565_t first = buf[0];
    for (int i = 0; i < SCR_W * SCR_H; i++) if (buf[i] != first) { distinct = 1; break; }
    TEST_ASSERT_TRUE_MESSAGE(distinct, "plot screen painted nothing");
}

static void test_plot_fixed_scale_clips_to_the_box(void)
{
    /* A trace far outside a fixed scale must stay inside the graph box. */
    trace_t t; trace_init(&t);
    for (int i = 0; i < 40; i++) trace_push(&t, i % 2 ? 100.0f : -100.0f);
    const float fixed[2] = { -1.0f, 1.0f };
    screen_plot(&f, &theme_dark, &t, "PLOT", "VDC", NULL, 0, fixed);
    const int top = BAR_H + 8, bot = SCR_H - 8;      /* graph box, see screen_plot */
    for (int y = BAR_H; y < SCR_H; y++)
        for (int x = 0; x < SCR_W - RAIL_W; x++)
            if (buf[y * SCR_W + x] == theme_dark.trace)
                TEST_ASSERT_TRUE(y > top && y < bot);
}

static int rows_with(rgb565_t c, int x)
{
    int n = 0;
    for (int y = 0; y < SCR_H; y++) n += buf[y * SCR_W + x] == c;
    return n;
}

static void test_menu_keeps_selection_visible(void)
{
    const char *items[20], *vals[20];
    for (int i = 0; i < 20; i++) { items[i] = "ITEM"; vals[i] = "VAL"; }
    for (int sel = 0; sel < 20; sel += 19) {
        screen_menu(&f, &theme_dark, "MENU", items, vals, 20, sel, NULL, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(26, rows_with(theme_dark.accent, 10), "selected row drawn once");
    }
    TEST_ASSERT_TRUE(screen_menu_rows() >= 8);
}

static void test_stock_font_metrics(void)
{
    /* '.' must be much narrower than '0' - proves proportional spacing works */
    TEST_ASSERT_TRUE(fb_stock_w(".", 0, 1) < fb_stock_w("0", 0, 1) / 2);
    /* advance accumulates with the gap, minus no trailing gap */
    int one = fb_stock_w("0", 4, 1), two = fb_stock_w("00", 4, 1);
    TEST_ASSERT_EQUAL_INT(one * 2 + 4, two);
    /* unknown characters are skipped, not drawn as garbage */
    TEST_ASSERT_EQUAL_INT(fb_stock_w("0", 0, 1), fb_stock_w("0@#$", 0, 1));
    /* den halves the size */
    TEST_ASSERT_EQUAL_INT(96, fb_stock_h(1));
    TEST_ASSERT_EQUAL_INT(48, fb_stock_h(2));
}

static void test_stock_font_paints_within_bounds(void)
{
    fb_clear(&f, 0);
    fb_stock_text(&f, 10, 10, "0.1556", 6, 1, 0xFFFF);
    int lit = 0;
    for (int i = 0; i < SCR_W * SCR_H; i++) if (buf[i]) lit++;
    TEST_ASSERT_TRUE_MESSAGE(lit > 500, "stock font painted nothing");
    /* nothing outside the drawn box */
    for (int x = 0; x < 10; x++)
        for (int y = 0; y < SCR_H; y++)
            TEST_ASSERT_EQUAL_HEX16(0, buf[y * SCR_W + x]);
}

static void test_themes_are_distinct(void)
{
    TEST_ASSERT_NOT_EQUAL(theme_stock.bg, theme_dark.bg);
    TEST_ASSERT_NOT_EQUAL(theme_dark.bg, theme_amber.bg);
    /* readability: foreground must differ from background in every theme */
    TEST_ASSERT_NOT_EQUAL(theme_stock.fg, theme_stock.bg);
    TEST_ASSERT_NOT_EQUAL(theme_dark.fg, theme_dark.bg);
    TEST_ASSERT_NOT_EQUAL(theme_amber.fg, theme_amber.bg);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trace_is_fifo);
    RUN_TEST(test_trace_wraps_and_keeps_newest);
    RUN_TEST(test_trace_range_pads_and_handles_flat);
    RUN_TEST(test_text_widths_add_up_and_grow_with_scale);
    RUN_TEST(test_unit_symbols_on_screen);
    RUN_TEST(test_title_case_labels);
    RUN_TEST(test_label_text_is_bounded_and_antialiased);
    RUN_TEST(test_bar_graph_draws_to_scale);
    RUN_TEST(test_clipping_does_not_corrupt_memory);
    RUN_TEST(test_screens_actually_paint);
    RUN_TEST(test_plot_fixed_scale_clips_to_the_box);
    RUN_TEST(test_menu_keeps_selection_visible);
    RUN_TEST(test_stock_font_metrics);
    RUN_TEST(test_stock_font_paints_within_bounds);
    RUN_TEST(test_themes_are_distinct);
    return UNITY_END();
}
