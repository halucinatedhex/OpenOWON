/* lib/datalog against the NOR-flash model: layout, capacity, reopening,
 * stale sessions, and a power cut at every flash operation. */
#include <unity.h>
#include <math.h>
#include <string.h>

#include "datalog.h"
#include "sim_flash.h"

static dlog_t d;

void setUp(void)
{
    sim_flash_reset();
    TEST_ASSERT_TRUE(dlog_open(&d, &sim_flash_io, DLOG_BASE, DLOG_NPAGES));
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT32(0, sim_flash_violations()); }

static float val(uint32_t i) { return 1.0f + (float)i * 0.001f; }

static void fill(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) TEST_ASSERT_EQUAL_INT(0, dlog_append(&d, val(i)));
}

static void reopen(void) { TEST_ASSERT_TRUE(dlog_open(&d, &sim_flash_io, DLOG_BASE, DLOG_NPAGES)); }

static void test_geometry(void)
{
    TEST_ASSERT_EQUAL_UINT32(508, DLOG_PER_PAGE);
    TEST_ASSERT_EQUAL_UINT32(21, DLOG_NPAGES);
    TEST_ASSERT_EQUAL_UINT32(10668, dlog_capacity(&d));
    TEST_ASSERT_TRUE(DLOG_END <= 0x0803A800u);               /* user calibration above */
}

static void test_refuses_regions_outside_its_own(void)
{
    dlog_t x;
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, 0x0803B800u, 1));         /* settings */
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, 0x0803A800u, 1));         /* user calibration */
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, 0x0803D800u, 1));         /* stock log */
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, DLOG_BASE, DLOG_NPAGES + 1));
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, DLOG_BASE - DLOG_PAGE, 2));
    TEST_ASSERT_FALSE(dlog_open(&x, &sim_flash_io, DLOG_BASE + 4, 1));
}

static void test_empty_flash_has_no_session(void)
{
    TEST_ASSERT_FALSE(d.have);
    TEST_ASSERT_EQUAL_INT(-1, dlog_append(&d, 1.0f));
    float v;
    TEST_ASSERT_FALSE(dlog_get(&d, 0, &v));
}

static void test_records_and_reopens_across_pages(void)
{
    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 3, 7, 1000));
    fill(1200);                                  /* three pages */
    reopen();
    TEST_ASSERT_TRUE(d.have);
    TEST_ASSERT_FALSE(d.active);                 /* a reopened session is stopped */
    TEST_ASSERT_EQUAL_UINT32(1200, d.count);
    TEST_ASSERT_EQUAL_UINT8(3, d.fn);
    TEST_ASSERT_EQUAL_UINT8(7, d.tag);
    TEST_ASSERT_EQUAL_UINT32(1000, d.interval_ms);
    for (uint32_t i = 0; i < 1200; i += 37) {
        float v;
        TEST_ASSERT_TRUE(dlog_get(&d, i, &v));
        TEST_ASSERT_EQUAL_FLOAT(val(i), v);
    }
    TEST_ASSERT_EQUAL_UINT32(1, sim_flash_erases(DLOG_BASE + 2 * DLOG_PAGE));
    TEST_ASSERT_EQUAL_UINT32(0, sim_flash_erases(DLOG_BASE + 3 * DLOG_PAGE));   /* not reached */
}

static void test_exact_page_boundary(void)
{
    dlog_start(&d, 0, 0, 0);
    fill(DLOG_PER_PAGE);
    reopen();
    TEST_ASSERT_EQUAL_UINT32(DLOG_PER_PAGE, d.count);
}

static void test_fills_and_stops(void)
{
    dlog_start(&d, 0, 0, 200);
    fill(dlog_capacity(&d));
    TEST_ASSERT_FALSE(d.active);
    TEST_ASSERT_EQUAL_INT(-1, dlog_append(&d, 9.0f));
    reopen();
    TEST_ASSERT_EQUAL_UINT32(10668, d.count);
    float v;
    TEST_ASSERT_TRUE(dlog_get(&d, 10667, &v));
    TEST_ASSERT_EQUAL_FLOAT(val(10667), v);
}

static void test_special_values(void)
{
    dlog_start(&d, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, dlog_append(&d, DLOG_OVERLOAD));
    uint32_t allones = 0xFFFFFFFFu;               /* a NaN that would look erased */
    float nan1;
    memcpy(&nan1, &allones, 4);
    TEST_ASSERT_EQUAL_INT(0, dlog_append(&d, nan1));
    TEST_ASSERT_EQUAL_INT(0, dlog_append(&d, -0.0f));
    TEST_ASSERT_EQUAL_INT(0, dlog_append(&d, 2.5f));
    reopen();
    TEST_ASSERT_EQUAL_UINT32(4, d.count);         /* the NaN did not end the data */
    float v;
    dlog_get(&d, 0, &v); TEST_ASSERT_TRUE(isinf(v) && v > 0);
    dlog_get(&d, 1, &v); TEST_ASSERT_TRUE(isnan(v));
    dlog_get(&d, 3, &v); TEST_ASSERT_EQUAL_FLOAT(2.5f, v);
}

static void test_new_session_ignores_stale_pages(void)
{
    dlog_start(&d, 1, 0, 0);
    fill(1500);                                   /* pages 0-2 */
    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 2, 0, 0));
    fill(DLOG_PER_PAGE);                          /* page 0 full, page 1 still old */
    reopen();
    TEST_ASSERT_EQUAL_UINT8(2, d.fn);
    TEST_ASSERT_EQUAL_UINT32(DLOG_PER_PAGE, d.count);
    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 4, 0, 0));    /* reopened: still a fresh number */
    reopen();
    TEST_ASSERT_EQUAL_UINT8(4, d.fn);
    TEST_ASSERT_EQUAL_UINT32(0, d.count);
}

static void test_session_number_is_new_even_if_page0_was_torn(void)
{
    dlog_start(&d, 1, 0, 0);
    fill(1000);                                   /* session 1 on pages 0-1 */
    sim_flash_cut_after(2);                       /* erase page 0, one header word */
    TEST_ASSERT_EQUAL_INT(-1, dlog_start(&d, 2, 0, 0));
    sim_flash_cut_after(-1);
    reopen();
    TEST_ASSERT_FALSE(d.have);                    /* page 0 has no header */
    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 3, 0, 0));
    fill(DLOG_PER_PAGE);                          /* ends exactly where session 1's page 1 */
    reopen();                                     /* still sits: it must not be counted */
    TEST_ASSERT_EQUAL_UINT32(DLOG_PER_PAGE, d.count);
}

/* Cut power after every possible number of flash operations while recording
 * across a page boundary. After each, the log must reopen with exactly the
 * readings whose program completed, all correct. */
static void test_power_cut_at_every_operation(void)
{
    const uint32_t N = DLOG_PER_PAGE + 20;
    for (int cut = 0; cut < (int)N + 12; cut++) {
        sim_flash_reset();
        reopen();
        sim_flash_cut_after(cut);
        uint32_t ok = 0;
        if (dlog_start(&d, 5, 0, 1000) == 0)
            for (uint32_t i = 0; i < N && dlog_append(&d, val(i)) == 0; i++) ok++;
        sim_flash_cut_after(-1);
        reopen();
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(ok, d.count, "count after cut");
        for (uint32_t i = 0; i < d.count; i++) {
            float v;
            dlog_get(&d, i, &v);
            TEST_ASSERT_EQUAL_FLOAT(val(i), v);
        }
    }
}

/* B35: a rolling session keeps the newest readings, oldest first. A
 * two-page region wraps quickly. */
static void open2(void) { TEST_ASSERT_TRUE(dlog_open(&d, &sim_flash_io, DLOG_BASE, 2)); }

static void check_tail(uint32_t written)
{
    TEST_ASSERT_TRUE(d.count <= dlog_capacity(&d));
    TEST_ASSERT_TRUE(d.count >= dlog_capacity(&d) - DLOG_PER_PAGE || d.count == written);
    for (uint32_t i = 0; i < d.count; i++) {
        float v;
        TEST_ASSERT_TRUE(dlog_get(&d, i, &v));
        TEST_ASSERT_EQUAL_FLOAT(val(written - d.count + i), v);
    }
}

/* B33: with the clock set, the session number is the start time, kept over
 * a reopen and across pages; a clock behind the numbers in use gives a
 * plain number and no start time; no clock, as before. */
static void test_sessions_carry_their_start_time(void)
{
    const uint32_t t0 = 1794000000u;                  /* 2026-11 */
    TEST_ASSERT_EQUAL_INT(0, dlog_start_at(&d, 0, 0, 1000, false, t0));
    fill(DLOG_PER_PAGE + 10);                         /* into the second page */
    reopen();
    TEST_ASSERT_TRUE(d.have);
    TEST_ASSERT_EQUAL_UINT32(t0, d.start_time);
    TEST_ASSERT_EQUAL_UINT32(DLOG_PER_PAGE + 10, d.count);
    TEST_ASSERT_EQUAL_UINT8(0, d.fn);

    TEST_ASSERT_EQUAL_INT(0, dlog_start_at(&d, 4, 7, 200, false, t0 - 3600));   /* clock set back */
    fill(5);
    reopen();
    TEST_ASSERT_EQUAL_UINT32(0, d.start_time);
    TEST_ASSERT_TRUE(d.session > t0);                 /* still the newest session */
    TEST_ASSERT_EQUAL_UINT8(4, d.fn);
    TEST_ASSERT_EQUAL_UINT8(7, d.tag);

    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 1, 0, 1000));                       /* no clock */
    reopen();
    TEST_ASSERT_EQUAL_UINT32(0, d.start_time);
}

static void test_rolling_keeps_the_newest(void)
{
    open2();
    TEST_ASSERT_EQUAL_INT(0, dlog_start_rolling(&d, 3, 1, 200));
    uint32_t n = 5 * dlog_capacity(&d) + 123;
    fill(n);                                      /* never fills: wraps five times */
    TEST_ASSERT_TRUE(d.active);
    check_tail(n);
    open2();                                      /* reopens rolling, with the same tail */
    TEST_ASSERT_TRUE(d.have && d.wrap);
    TEST_ASSERT_EQUAL_UINT8(3, d.fn);
    TEST_ASSERT_EQUAL_UINT32(200, d.interval_ms);
    check_tail(n);
}

static void test_plain_session_after_rolling_one(void)
{
    open2();
    dlog_start_rolling(&d, 1, 0, 0);
    fill(3 * dlog_capacity(&d));
    TEST_ASSERT_EQUAL_INT(0, dlog_start(&d, 2, 0, 0));   /* plain, over the rolling pages */
    fill(10);
    open2();
    TEST_ASSERT_FALSE(d.wrap);
    TEST_ASSERT_EQUAL_UINT8(2, d.fn);
    TEST_ASSERT_EQUAL_UINT32(10, d.count);
}

/* A power cut after every flash operation across several wraps: the log
 * reopens holding a consecutive run of the newest completed readings. */
static void test_rolling_power_cut_at_every_operation(void)
{
    const uint32_t N = 2 * 2 * DLOG_PER_PAGE + 40;  /* two full wraps of two pages */
    for (int cut = 0; cut < (int)(N + 3 * 4 + 8); cut++) {
        sim_flash_reset();
        open2();
        sim_flash_cut_after(cut);
        uint32_t ok = 0;
        if (dlog_start_rolling(&d, 5, 0, 1000) == 0)
            for (uint32_t i = 0; i < N && dlog_append(&d, val(i)) == 0; i++) ok++;
        sim_flash_cut_after(-1);
        open2();
        if (!ok) continue;
        TEST_ASSERT_TRUE_MESSAGE(d.have, "a session after a cut");
        TEST_ASSERT_TRUE(d.count <= ok);
        TEST_ASSERT_TRUE_MESSAGE(d.count >= DLOG_PER_PAGE - 1 || d.count == ok || ok < DLOG_PER_PAGE,
                                 "at most the page being replaced is lost");
        for (uint32_t i = 0; i < d.count; i++) {
            float v;
            dlog_get(&d, i, &v);
            TEST_ASSERT_EQUAL_FLOAT(val(ok - d.count + i), v);   /* the newest, in order */
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_geometry);
    RUN_TEST(test_sessions_carry_their_start_time);
    RUN_TEST(test_refuses_regions_outside_its_own);
    RUN_TEST(test_empty_flash_has_no_session);
    RUN_TEST(test_records_and_reopens_across_pages);
    RUN_TEST(test_exact_page_boundary);
    RUN_TEST(test_fills_and_stops);
    RUN_TEST(test_special_values);
    RUN_TEST(test_new_session_ignores_stale_pages);
    RUN_TEST(test_session_number_is_new_even_if_page0_was_torn);
    RUN_TEST(test_power_cut_at_every_operation);
    RUN_TEST(test_rolling_keeps_the_newest);
    RUN_TEST(test_plain_session_after_rolling_one);
    RUN_TEST(test_rolling_power_cut_at_every_operation);
    return UNITY_END();
}
