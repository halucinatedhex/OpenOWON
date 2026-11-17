/* lib/measure/meas_temp.c - PT100, type K, cold junction.
 *
 * Reference points are from the published standards' tables (NIST ITS-90
 * type K, IEC 60751 PT100), not from the implementation. The stock tables
 * are read from the firmware image and must agree everywhere except at the
 * two entries recorded as typos in docs/measurement.md.
 */
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meas_temp.h"

#define IMAGE      "firmware/XDM1041_OS_V2.1.2_20210525.bin"
#define IMAGE_BASE 0x08002800u

static uint8_t *img;
static long     img_len;

void setUp(void) {}
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

static float img_f32(uint32_t va)
{
    float v;
    memcpy(&v, img + (va - IMAGE_BASE), sizeof v);
    return v;
}

#define CLOSE(exp, act, tol) TEST_ASSERT_DOUBLE_WITHIN((tol), (exp), (act))

/* NIST ITS-90 type K table values, mV (tables are rounded to 1 uV). */
static void test_type_k_reference_points(void)
{
    const struct { double t, mv; } p[] = {
        { -200, -5.891 }, { -100, -3.554 }, { 0, 0.000 }, { 100, 4.096 },
        { 200, 8.138 }, { 500, 20.644 }, { 1000, 41.276 }, { 1370, 54.819 },
    };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
        CLOSE(p[i].mv, meas_tc_k_mv(p[i].t), 0.0006);
}

/* IEC 60751 table values, ohms (tables are rounded to 0.01 ohm). */
static void test_pt100_reference_points(void)
{
    const struct { double t, r; } p[] = {
        { -50, 80.31 }, { 0, 100.00 }, { 100, 138.51 }, { 290, 208.48 },
        { 500, 280.98 }, { 850, 390.48 },
    };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
        CLOSE(p[i].r, meas_pt100_ohm(p[i].t), 0.006);
}

static void test_inverses_round_trip(void)
{
    bool over;
    for (double t = -270; t <= 1370; t += 7.3) {
        CLOSE(t, meas_tc_k_temp(meas_tc_k_mv(t), &over), 1e-6);
        TEST_ASSERT_FALSE(over);
    }
    for (double t = -50; t <= 850; t += 3.7) {
        CLOSE(t, meas_pt100_temp(meas_pt100_ohm(t), &over), 1e-6);
        TEST_ASSERT_FALSE(over);
    }
    meas_tc_k_temp(60.0, &over);   TEST_ASSERT_TRUE(over);
    meas_pt100_temp(70.0, &over);  TEST_ASSERT_TRUE(over);
    meas_pt100_temp(400.0, &over); TEST_ASSERT_TRUE(over);
}

/* Stock PT100 table: 91 floats, -50..850 C. Every entry matches the equation
 * except 290 C, which stock stores as 204.48 (should be 208.48). */
static void test_stock_pt100_table(void)
{
    if (!img) TEST_IGNORE_MESSAGE("stock image not found");
    int bad = 0;
    for (int i = 0; i < 91; i++) {
        double t = -50.0 + 10.0 * i;
        double d = img_f32(0x0801E0ECu + 4u * (uint32_t)i) - meas_pt100_ohm(t);
        if (t == 290.0) { CLOSE(-4.0, d, 0.01); continue; }
        if (fabs(d) > 0.006) bad++;
    }
    TEST_ASSERT_EQUAL_INT(0, bad);
}

/* Stock type K table: 165 floats, -270..1370 C. Within 5 uV of NIST up to
 * 900 C, drifting smoothly to ~13 uV low by 1370 C, with one isolated error
 * at 850 C (+10.9 uV: 35.324 for ~35.313 mV). The error is found as the
 * largest second difference of the residual, which a smooth drift cannot
 * produce. */
static void test_stock_type_k_table(void)
{
    if (!img) TEST_IGNORE_MESSAGE("stock image not found");
    double res[165];
    for (int i = 0; i < 165; i++) {
        double t = -270.0 + 10.0 * i;
        res[i] = img_f32(0x0801DE58u + 4u * (uint32_t)i) - meas_tc_k_mv(t);
        if (t == 850.0)      CLOSE(0.0109, res[i], 0.0005);
        else if (t <= 900.0) CLOSE(0.0, res[i], 0.005);
        else                 CLOSE(0.0, res[i], 0.014);
    }
    int worst = 0;
    double wd = 0.0;
    for (int i = 1; i < 164; i++) {
        double d2 = fabs(res[i - 1] - 2.0 * res[i] + res[i + 1]);
        if (d2 > wd) { wd = d2; worst = i; }
    }
    TEST_ASSERT_EQUAL_INT(112, worst);               /* 850 C */
}

static void test_compensation(void)
{
    bool over;
    /* zero EMF with the junction at 25 C means the probe is at 25 C */
    CLOSE(25.0, meas_tc_k_compensated(0.0, 25.0, &over), 1e-6);
    /* 4.096 mV is 100 C above a 0 C junction */
    CLOSE(100.0, meas_tc_k_compensated(4.096, 0.0, &over), 0.02);
    /* with a 25 C junction the same probe reads 4.096 - E(25) */
    CLOSE(100.0, meas_tc_k_compensated(4.096 - meas_tc_k_mv(25.0), 25.0, &over), 0.02);
}

static void test_cold_junction(void)
{
    /* T = (V2 - V1)/2 / G - 273.15 - 16 */
    double g = 0.1700;
    double dv = 2.0 * g * (25.0 + 273.15 + 16.0);
    CLOSE(25.0, meas_cold_junction(100.0, 100.0 + dv, 1700), 1e-9);
    /* G outside 0.10..0.18 falls back to 0.169465, as stock does */
    CLOSE(0.169465, meas_cj_gain(-1), 0.0);
    CLOSE(0.169465, meas_cj_gain(5000), 0.0);
    CLOSE(0.1694, meas_cj_gain(1694), 1e-12);
}

static void test_units(void)
{
    CLOSE(212.0, meas_temp_unit(100.0, MEAS_DEG_F), 1e-9);
    CLOSE(-40.0, meas_temp_unit(-40.0, MEAS_DEG_F), 1e-9);
    CLOSE(273.15, meas_temp_unit(0.0, MEAS_KELVIN), 1e-9);
    CLOSE(21.5, meas_temp_unit(21.5, MEAS_DEG_C), 0.0);
}

int main(void)
{
    load_image();
    UNITY_BEGIN();
    RUN_TEST(test_type_k_reference_points);
    RUN_TEST(test_pt100_reference_points);
    RUN_TEST(test_inverses_round_trip);
    RUN_TEST(test_stock_pt100_table);
    RUN_TEST(test_stock_type_k_table);
    RUN_TEST(test_compensation);
    RUN_TEST(test_cold_junction);
    RUN_TEST(test_units);
    free(img);
    return UNITY_END();
}
