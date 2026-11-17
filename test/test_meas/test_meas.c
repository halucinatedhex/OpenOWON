/* lib/measure/meas.c - the recovered measurement arithmetic.
 *
 * Checked three ways, never by re-deriving from the implementation:
 *   1. range tables against the user manual's specification table
 *   2. transcribed constants against the bytes in the stock image itself
 *   3. formulas against values worked by hand from docs/measurement.md
 */
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meas.h"

#define IMAGE      "firmware/XDM1041_OS_V2.1.2_20210525.bin"
#define IMAGE_BASE 0x08002800u
#define FULL       8388606

static uint8_t *img;
static long     img_len;
static meas_cal_t cal;

void setUp(void) { meas_cal_defaults(&cal); }
void tearDown(void) {}

static void load_image(void)
{
    FILE *f = fopen(IMAGE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    img_len = ftell(f);
    rewind(f);
    img = malloc((size_t)img_len);
    if (fread(img, 1, (size_t)img_len, f) != (size_t)img_len) { free(img); img = NULL; }
    fclose(f);
}

static double img_f64(uint32_t va)
{
    double d;
    memcpy(&d, img + (va - IMAGE_BASE), sizeof d);
    return d;
}

#define CLOSE(exp, act, tol) TEST_ASSERT_DOUBLE_WITHIN((tol), (exp), (act))

/* ---- 1. ranges against the manual (p. 37) -------------------------------- */

static void check_ranges(meas_fn_t fn, const double *si, int n)
{
    TEST_ASSERT_EQUAL_INT(n, meas_nranges(fn));
    for (int r = 0; r < n; r++)
        CLOSE(si[r], meas_to_si(fn, r, meas_fullscale(fn, r)), si[r] * 1e-12);
}

static void test_ranges_match_manual(void)
{
    const double dcv[] = { 50e-3, 500e-3, 5, 50, 500, 1000 };
    const double acv[] = { 500e-3, 5, 50, 500, 750 };
    const double dci[] = { 500e-6, 5000e-6, 50e-3, 500e-3, 5, 10 };
    const double ohm[] = { 500, 5e3, 50e3, 500e3, 5e6, 50e6 };
    const double cap[] = { 50e-9, 500e-9, 5e-6, 50e-6, 500e-6, 5e-3, 50e-3 };
    check_ranges(MEAS_DCV, dcv, 6);
    check_ranges(MEAS_ACV, acv, 5);
    check_ranges(MEAS_DCI, dci, 6);
    check_ranges(MEAS_ACI, dci, 6);
    check_ranges(MEAS_OHM2, ohm, 6);
    check_ranges(MEAS_OHM4, ohm, 6);
    check_ranges(MEAS_CAP, cap, 7);
}

/* The manual allows 10% over range on DC ranges below the top one, so the
 * full ADC count must reach at least 1.1 x full scale. */
static void test_dc_full_count_covers_overrange(void)
{
    for (int r = 0; r < 5; r++) {
        double fs = meas_to_si(MEAS_DCV, r, meas_fullscale(MEAS_DCV, r));
        TEST_ASSERT_TRUE(meas_dcv(&cal, r, FULL) >= 1.1 * fs);
        double fi = meas_to_si(MEAS_DCI, r, meas_fullscale(MEAS_DCI, r));
        TEST_ASSERT_TRUE(meas_dci(&cal, r, FULL) >= 1.1 * fi);
        double fo = meas_to_si(MEAS_OHM2, r, meas_fullscale(MEAS_OHM2, r));
        TEST_ASSERT_TRUE(meas_ohm(&cal, MEAS_OHM2, r, FULL, false) >= 1.1 * fo);
    }
}

/* ---- 2. constants against the image -------------------------------------- */

static void test_ac_and_ohm_tables_match_image(void)
{
    if (!img) TEST_IGNORE_MESSAGE("stock image not found");
    const double acv[5] = { 2460.0, 27.06, 246.0, 2460.0, 2460.0 };
    const double aci[6] = { 2460.0, 24.6, 246.0, 2460.0, 24.6, 246.0 };
    const double ohm[6] = { 1111.11, 11.11, 111.11, 1111.11, 6.67, 66.7 };
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_DOUBLE(acv[i], img_f64(0x0801DDD0u + 8u * i));
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL_DOUBLE(aci[i], img_f64(0x0801DDF8u + 8u * i));
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL_DOUBLE(ohm[i], img_f64(0x0801DE28u + 8u * i));
    /* AC: unity-gain full RMS gives exactly the table value */
    uint64_t full = 262143ull * 262143ull;
    CLOSE(2.460, meas_ac(&cal, MEAS_ACV, 0, full, false), 1e-12);
    CLOSE(27.06, meas_ac(&cal, MEAS_ACV, 1, full, false), 1e-12);
}

static void test_dbm_rref_matches_image(void)
{
    if (!img) TEST_IGNORE_MESSAGE("stock image not found");
    for (int i = 0; i < MEAS_NRREF; i++) {
        uint32_t o = 0x0801E298u - IMAGE_BASE + 2u * (uint32_t)i;
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(img[o] | img[o + 1] << 8), meas_dbm_rref[i]);
    }
}

/* ---- calibration record ---------------------------------------------------- */

static void put32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}

static void test_cal_parse_offsets(void)
{
    uint8_t rec[MEAS_CAL_RECORD_LEN];
    memset(rec, 0, sizeof rec);
    put32(rec + 0x1F8, 1000123);                     /* coef f0 r0 */
    put32(rec + 0x298 + 8, 999000);                  /* coef f4 r2 */
    put32(rec + 0x3B0 + 8 * 0x28 + 12, -4242);       /* zero f8 r3 */
    put32(rec + 0x56C, 1694);
    meas_cal_t c;
    TEST_ASSERT_EQUAL_INT(0, meas_cal_parse(&c, rec, sizeof rec));
    TEST_ASSERT_EQUAL_INT32(1000123, c.coef[MEAS_DCV][0]);
    TEST_ASSERT_EQUAL_INT32(999000, c.coef[MEAS_OHM2][2]);
    TEST_ASSERT_EQUAL_INT32(-4242, c.zero[MEAS_CAP][3]);
    TEST_ASSERT_EQUAL_INT32(1694, c.cj_g);
    TEST_ASSERT_EQUAL_INT(-1, meas_cal_parse(&c, rec, 100));
}

/* A real factory record (test/data/README.md): an XDM2041's, same platform.
 * Independent of the v2.1.2 image our layout was derived from. */
static void test_cal_real_xdm2041_record(void)
{
    FILE *f = fopen("test/data/xdm2041_calibration.bin", "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "run from the project root");
    uint8_t rec[2048];
    TEST_ASSERT_EQUAL_size_t(sizeof rec, fread(rec, 1, sizeof rec, f));
    fclose(f);
    meas_cal_t c;
    TEST_ASSERT_EQUAL_INT(0, meas_cal_parse(&c, rec, MEAS_CAL_RECORD_LEN));
    TEST_ASSERT_EQUAL_INT(0, meas_cal_validate(&c));
    TEST_ASSERT_EQUAL_INT32(1025169, c.coef[MEAS_DCV][0]);
    TEST_ASSERT_EQUAL_INT32(-200598, c.zero[MEAS_DCV][0]);
    TEST_ASSERT_EQUAL_INT32(1701, c.cj_g);
    TEST_ASSERT_EQUAL_MEMORY("XDM2041", rec, 7);
}

static void test_cal_validate(void)
{
    TEST_ASSERT_EQUAL_INT(0, meas_cal_validate(&cal));
    uint8_t blank[MEAS_CAL_RECORD_LEN];
    memset(blank, 0xFF, sizeof blank);
    meas_cal_t c;
    meas_cal_parse(&c, blank, sizeof blank);
    /* owned slots: DCV 6 + ACV 4 + DCI 6 + ACI 6 + ohm 6 + cap 7 */
    TEST_ASSERT_EQUAL_INT(35, meas_cal_validate(&c));
    cal.coef[MEAS_DCI][0] = 1250000;                 /* allowed for current */
    cal.coef[MEAS_DCV][0] = 1250000;                 /* not allowed for DCV */
    TEST_ASSERT_EQUAL_INT(1, meas_cal_validate(&cal));
}

static void test_cal_clamps(void)
{
    int32_t lo, hi;
    TEST_ASSERT_TRUE(meas_cal_clamp(MEAS_DCV, 0, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(800000, lo);  TEST_ASSERT_EQUAL_INT32(1200000, hi);
    TEST_ASSERT_TRUE(meas_cal_clamp(MEAS_ACI, 2, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(1300000, hi);
    TEST_ASSERT_TRUE(meas_cal_clamp(MEAS_CAP, 5, &lo, &hi));
    TEST_ASSERT_EQUAL_INT32(600000, lo);  TEST_ASSERT_EQUAL_INT32(1400000, hi);
    TEST_ASSERT_FALSE(meas_cal_clamp(MEAS_CAP, 2, &lo, &hi));
}

/* ---- 3. formulas ----------------------------------------------------------- */

static void test_dcv(void)
{
    /* full count, 50 mV range: 1.36667f x 50 mV */
    CLOSE(1.36667001247406 * 50e-3, meas_dcv(&cal, 0, FULL), 1e-15);
    /* 5 V range uses its own constant, not FS */
    CLOSE(7.516670227050781, meas_dcv(&cal, 2, FULL), 1e-12);
    /* gain scales, offset subtracts in mV on ranges 0-1 only */
    cal.coef[MEAS_DCV][0] = 1100000;
    cal.zero[MEAS_DCV][0] = 2000000;                 /* 2 mV */
    CLOSE(-2e-3, meas_dcv(&cal, 0, 0), 1e-15);
    CLOSE(1.36667001247406 * 50e-3 * 1.1 - 2e-3, meas_dcv(&cal, 0, FULL), 1e-12);
    cal.zero[MEAS_DCV][3] = 2000000;                 /* ignored on 50 V */
    CLOSE(0.0, meas_dcv(&cal, 3, 0), 1e-15);
    /* antisymmetric without offset */
    CLOSE(-meas_dcv(&cal, 4, 12345), meas_dcv(&cal, 4, -12345), 1e-12);
}

static void test_dci(void)
{
    CLOSE(1.36667001247406 * 500e-6, meas_dci(&cal, 0, FULL), 1e-15);
    cal.zero[MEAS_DCI][0] = 5000000;                 /* DCI has no offset */
    CLOSE(0.0, meas_dci(&cal, 0, 0), 0.0);
}

static void test_ac(void)
{
    uint64_t full = 262143ull * 262143ull;
    /* 750 V range takes its gain from the 500 V slot */
    cal.coef[MEAS_ACV][3] = 1100000;
    cal.coef[MEAS_ACV][4] = 500000;
    CLOSE(2460.0 * 1.1, meas_ac(&cal, MEAS_ACV, 4, full, false), 1e-9);
    /* bits above 38 are ignored */
    CLOSE(meas_ac(&cal, MEAS_ACV, 2, full, false),
          meas_ac(&cal, MEAS_ACV, 2, full | (3ull << 38), false), 0.0);
    /* ACI below 1% of full scale reads 0, except while calibrating */
    uint64_t tiny = 100ull * 100ull;                 /* 100/262143*2460 uA = 0.94 uA < 5 uA */
    CLOSE(0.0, meas_ac(&cal, MEAS_ACI, 0, tiny, false), 0.0);
    CLOSE(100.0 / 262143.0 * 2460.0 * 1e-6, meas_ac(&cal, MEAS_ACI, 0, tiny, true), 1e-15);
    TEST_ASSERT_TRUE(isnan(meas_ac(&cal, MEAS_DCV, 0, full, false)));
}

static void test_ohm(void)
{
    int32_t half = FULL / 2;
    double v0 = (double)half / FULL * 1111.11;       /* range 0, ohms */
    cal.zero[MEAS_OHM2][0] = 300000;                 /* 0.3 ohm */
    double ca = 0.23404255319148937;
    CLOSE(v0 - 0.3 * (1.0 + ca * v0 / 500.0), meas_ohm(&cal, MEAS_OHM2, 0, half, false), 1e-9);
    /* 4-wire: same gain row, no offset */
    CLOSE(v0, meas_ohm(&cal, MEAS_OHM4, 0, half, false), 1e-9);
    cal.coef[MEAS_OHM2][0] = 1050000;
    CLOSE(v0 * 1.05, meas_ohm(&cal, MEAS_OHM4, 0, half, false), 1e-9);
    /* upper ranges: plain subtraction, in the range unit */
    cal.zero[MEAS_OHM2][4] = 10000;                  /* 0.01 Mohm */
    double v4 = (double)half / FULL * 6.67;
    CLOSE((v4 - 0.01) * 1e6, meas_ohm(&cal, MEAS_OHM2, 4, half, false), 1e-6);
    /* negative clamps to 0 outside calibration; the low-range term uses |v| */
    CLOSE(0.0, meas_ohm(&cal, MEAS_OHM2, 0, -half, false), 0.0);
    double vn = -v0 * 1.05;
    CLOSE(vn - 0.3 * (1.0 + ca * fabs(vn) / 500.0), meas_ohm(&cal, MEAS_OHM2, 0, -half, true), 1e-9);
}

static void test_continuity_and_diode(void)
{
    CLOSE(1000.0 / (double)0.9f, meas_continuity(&cal, FULL), 1e-9);
    CLOSE(0.0, meas_continuity(&cal, -5), 0.0);
    cal.coef[MEAS_OHM2][0] = 900000;                 /* borrowed from 500 ohm */
    CLOSE(1000.0 / (double)0.9f * 0.9, meas_continuity(&cal, FULL), 1e-9);

    double k = 1.23 * 5.555555820465088 / 10.0 * 81.28907775878906;
    CLOSE(55.5475, k, 0.0001);                       /* as recorded in the doc */
    CLOSE(k, meas_diode(&cal, FULL), 1e-12);
    cal.coef[MEAS_DCV][2] = 1010000;                 /* borrowed from 5 V DCV */
    cal.zero[MEAS_DCV][2] = 1000000;                 /* 1 mV: diode offset is x 1e9 */
    CLOSE(0.0, meas_diode(&cal, 0) + 1e-3, 1e-15);
    CLOSE(k * 1.01 - 1e-3, meas_diode(&cal, FULL), 1e-12);
}

static void test_capacitance(void)
{
    /* range 0: r / 300000 * 1000 nF */
    CLOSE(2.0 / 300.0 * 1e-9, meas_cap_counter(&cal, 0, 1000, 2000), 1e-24);
    CLOSE(2.0 / 240000.0 * 1e-6, meas_cap_counter(&cal, 2, 1000, 2000), 1e-21);
    CLOSE(2.0 / 24000.0 * 1e-6, meas_cap_counter(&cal, 3, 1000, 2000), 1e-21);
    CLOSE(2.0 / 7394.0 * 1e-6, meas_cap_counter(&cal, 4, 1000, 2000), 1e-21);
    cal.zero[MEAS_CAP][0] = 10000;                   /* 1 nF: cap offset is x 1e4 */
    CLOSE(-1e-9, meas_cap_counter(&cal, 0, 1000, 0), 1e-24);
    TEST_ASSERT_TRUE(isnan(meas_cap_counter(&cal, 0, 0, 1)));
    TEST_ASSERT_TRUE(isnan(meas_cap_counter(&cal, 5, 1, 1)));

    /* slope: 2 * 2000 * 0.18e-3 / 0.1 V = 7.2 (mF on range 6) */
    bool over = true;
    CLOSE(7.2e-3, meas_cap_slope(&cal, 6, 0.2, 0.3, 2000, &over), 1e-15);
    TEST_ASSERT_FALSE(over);
    meas_cap_slope(&cal, 6, 0.2, 0.7, 2000, &over);
    TEST_ASSERT_TRUE(over);
    CLOSE(1.2 / 1.8, meas_cap_volts(FULL), 1e-7);
}

static void test_frequency(void)
{
    /* CTA + 2^20 = 4e6 gives a 1 s gate, so f = CTB */
    double duty = 0;
    uint32_t cta = 4000000u - 1048576u;
    CLOSE(50.0, meas_freq(cta, 50, 2000000u, &duty), 1e-9);
    CLOSE(50.0, duty, 1e-9);
    TEST_ASSERT_EQUAL_INT(0, meas_freq_code(999.0, false));
    TEST_ASSERT_EQUAL_INT(1, meas_freq_code(1000.0, false));
    TEST_ASSERT_EQUAL_INT(2, meas_freq_code(2e6, false));
    TEST_ASSERT_EQUAL_INT(3, meas_freq_code(0.5, true));    /* 2 s */
    TEST_ASSERT_EQUAL_INT(4, meas_freq_code(50.0, true));   /* 20 ms */
    TEST_ASSERT_EQUAL_INT(5, meas_freq_code(1e5, true));    /* 10 us */
    TEST_ASSERT_EQUAL_INT(-6, meas_unit_exp(MEAS_FREQ, 5));
}

/* ---- post-processing ------------------------------------------------------- */

static void test_suppress_small(void)
{
    /* 1% of 500 mV = 5 mV */
    CLOSE(0.0, meas_suppress_small(MEAS_ACV, 0, 4e-3, false, false), 0.0);
    CLOSE(6e-3, meas_suppress_small(MEAS_ACV, 0, 6e-3, false, false), 0.0);
    CLOSE(4e-3, meas_suppress_small(MEAS_ACV, 0, 4e-3, true, false), 0.0);
    CLOSE(4e-3, meas_suppress_small(MEAS_ACV, 0, 4e-3, false, true), 0.0);
    CLOSE(4e-3, meas_suppress_small(MEAS_DCV, 0, 4e-3, false, false), 0.0);
}

static void test_filter(void)
{
    static meas_filter_t f;
    double m;
    meas_filter_reset(&f);
    TEST_ASSERT_FALSE(meas_filter_take(&f, &m));
    meas_filter_push(&f, 1.0, 0.5);
    meas_filter_push(&f, 1.2, 0.5);
    meas_filter_push(&f, 1.4, 0.5);
    TEST_ASSERT_TRUE(meas_filter_take(&f, &m));
    CLOSE(1.2, m, 1e-12);

    /* a step pins the step sample until the next take */
    meas_filter_push(&f, 1.0, 0.5);
    meas_filter_push(&f, 3.0, 0.5);
    meas_filter_push(&f, 3.1, 0.5);
    TEST_ASSERT_TRUE(meas_filter_take(&f, &m));
    CLOSE(3.0, m, 0.0);
    meas_filter_push(&f, 3.1, 0.5);
    meas_filter_push(&f, 3.3, 0.5);
    meas_filter_take(&f, &m);
    CLOSE(3.2, m, 1e-12);

    /* capped at 200 (fresh filter, so the first push is not a step) */
    meas_filter_reset(&f);
    for (int i = 0; i < 300; i++) meas_filter_push(&f, i < 200 ? 1.0 : 1.4, 1.0);
    meas_filter_take(&f, &m);
    CLOSE(1.0, m, 0.0);

    CLOSE(5e-3, meas_step_threshold(MEAS_DCV, 0), 1e-9);
}

static void test_dbm(void)
{
    /* 0 dBm is 1 mW: 0.7746 V into 600 ohm, 0.2236 V into 50 ohm */
    CLOSE(0.0, meas_dbm(sqrt(0.6), 600.0), 1e-9);
    CLOSE(0.0, meas_dbm(sqrt(0.05), 50.0), 1e-9);
    CLOSE(10.0 * log10(20.0), meas_dbm(1.0, 50.0), 1e-9);   /* 13.01 dBm */
    CLOSE(-90.0, meas_dbm(0.0, 600.0), 1e-9);               /* stock floor 1e-9 mW */
    CLOSE(-3.0, meas_db(sqrt(0.6), 600.0, 300), 1e-9);
    TEST_ASSERT_EQUAL_UINT16(600, meas_dbm_rref[11]);
}

/* Ranges 5-6 from the charge rate, against our meter (2026-11-15, factory
 * CAP slots 5: 710255 / -40, 6: 987914 / 281; docs/measurement.md). */
static void test_capacitance_charge_rate(void)
{
    cal.coef[MEAS_CAP][5] = 710255;  cal.zero[MEAS_CAP][5] = -40;
    cal.coef[MEAS_CAP][6] = 987914;  cal.zero[MEAS_CAP][6] = 281;
    TEST_ASSERT_DOUBLE_WITHIN(0.005e-3, 18.776e-3, meas_cap_rate(&cal, 6, 0.018914));   /* 18 mF part */
    TEST_ASSERT_DOUBLE_WITHIN(0.005e-3, 1.1028e-3, meas_cap_rate(&cal, 5, 0.3223));     /* 1000 uF part */
    TEST_ASSERT_TRUE(isnan(meas_cap_rate(&cal, 6, 0.0)));
    TEST_ASSERT_TRUE(isnan(meas_cap_rate(&cal, 6, -0.1)));
    TEST_ASSERT_TRUE(isnan(meas_cap_rate(&cal, 4, 0.1)));
}

int main(void)
{
    load_image();
    UNITY_BEGIN();
    RUN_TEST(test_ranges_match_manual);
    RUN_TEST(test_cal_real_xdm2041_record);
    RUN_TEST(test_dc_full_count_covers_overrange);
    RUN_TEST(test_ac_and_ohm_tables_match_image);
    RUN_TEST(test_dbm_rref_matches_image);
    RUN_TEST(test_cal_parse_offsets);
    RUN_TEST(test_cal_validate);
    RUN_TEST(test_cal_clamps);
    RUN_TEST(test_dcv);
    RUN_TEST(test_dci);
    RUN_TEST(test_ac);
    RUN_TEST(test_ohm);
    RUN_TEST(test_continuity_and_diode);
    RUN_TEST(test_capacitance);
    RUN_TEST(test_capacitance_charge_rate);
    RUN_TEST(test_frequency);
    RUN_TEST(test_suppress_small);
    RUN_TEST(test_filter);
    RUN_TEST(test_dbm);
    free(img);
    return UNITY_END();
}
