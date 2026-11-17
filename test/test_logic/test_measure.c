#include <unity.h>
#include "measure.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_millivolts(void)
{
    char buf[32];
    fmt_reading(buf, sizeof buf, 0.0472, UNIT_VOLT, 3);
    TEST_ASSERT_EQUAL_STRING("47.200 mV", buf);
}

static void test_plain_volts(void)
{
    char buf[32];
    fmt_reading(buf, sizeof buf, 12.3456, UNIT_VOLT, 4);
    TEST_ASSERT_EQUAL_STRING("12.3456 V", buf);
}

static void test_megohms(void)
{
    char buf[32];
    fmt_reading(buf, sizeof buf, 2.2e6, UNIT_OHM, 3);
    TEST_ASSERT_EQUAL_STRING("2.200 MOhm", buf);
}

static void test_negative_keeps_sign(void)
{
    char buf[32];
    fmt_reading(buf, sizeof buf, -0.005, UNIT_VOLT, 2);
    TEST_ASSERT_EQUAL_STRING("-5.00 mV", buf);
}

static void test_range_selection(void)
{
    const double r[] = { 0.05, 0.5, 5.0, 50.0, 500.0 };
    TEST_ASSERT_EQUAL_INT(0, pick_range(r, 5, 0.01));
    TEST_ASSERT_EQUAL_INT(2, pick_range(r, 5, 3.3));
    TEST_ASSERT_EQUAL_INT(4, pick_range(r, 5, 9999.0)); /* overrange clamps */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_millivolts);
    RUN_TEST(test_plain_volts);
    RUN_TEST(test_megohms);
    RUN_TEST(test_negative_keeps_sign);
    RUN_TEST(test_range_selection);
    return UNITY_END();
}
