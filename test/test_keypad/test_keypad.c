/* lib/keypad: the codes logged on our meter, and debounce. */
#include <unity.h>

#include "keypad.h"

static keypad_t kp;

void setUp(void) { keypad_reset(&kp); }
void tearDown(void) {}

/* The 19 presses, in the order they were logged (keys.log, 2026-11-11). */
static void test_every_logged_code_decodes(void)
{
    static const struct { uint32_t code; app_key_t key; } seen[] = {
        { 0x1001, KEY_V }, { 0x1002, KEY_I }, { 0x1004, KEY_OHM }, { 0x0101, KEY_F1 },
        { 0x1008, KEY_CAP }, { 0x1010, KEY_FREQ }, { 0x0810, KEY_TEMP }, { 0x0208, KEY_UP },
        { 0x0201, KEY_F2 }, { 0x0108, KEY_RUNSTOP }, { 0x0202, KEY_MATH }, { 0x0404, KEY_LEFT },
        { 0x0401, KEY_F3 }, { 0x0204, KEY_UTILITY }, { 0x0104, KEY_RECORD }, { 0x0804, KEY_DUAL },
        { 0x0408, KEY_DOWN }, { 0x0801, KEY_F4 }, { 0x0802, KEY_F5 },
    };
    for (unsigned i = 0; i < sizeof seen / sizeof seen[0]; i++)
        TEST_ASSERT_EQUAL_INT(seen[i].key, keypad_decode(seen[i].code));
}

static void test_each_key_is_one_a_line_and_one_b_line(void)
{
    for (uint32_t a = 0; a < 5; a++)
        for (uint32_t b = 0; b < 5; b++) {
            uint32_t code = (1u << a) | (1u << (8 + b));
            app_key_t k = keypad_decode(code);
            if (k == KEY_NONE) continue;
            /* no two codes give the same key */
            for (uint32_t a2 = 0; a2 < 5; a2++)
                for (uint32_t b2 = 0; b2 < 5; b2++) {
                    uint32_t c2 = (1u << a2) | (1u << (8 + b2));
                    if (c2 != code) TEST_ASSERT_NOT_EQUAL(k, keypad_decode(c2));
                }
        }
}

static void test_no_key_two_keys_and_unknown_codes_are_none(void)
{
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_decode(0));
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_decode(0x1001 | 0x0202));
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_decode(0x0102));      /* unused crossing */
}

static void test_a_press_is_reported_once_after_it_settles(void)
{
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x1001));   /* first scan: bounce? */
    TEST_ASSERT_EQUAL_INT(KEY_V, keypad_feed(&kp, 0x1001));      /* stable: report */
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x1001)); /* held: nothing */
    keypad_feed(&kp, 0);
    keypad_feed(&kp, 0);
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x1001));
    TEST_ASSERT_EQUAL_INT(KEY_V, keypad_feed(&kp, 0x1001));      /* pressed again */
}

static void test_a_one_scan_glitch_is_ignored(void)
{
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x0208));
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0));
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0));
}

static void test_rolling_onto_a_second_key_reports_nothing_until_release(void)
{
    keypad_feed(&kp, 0x1001);
    TEST_ASSERT_EQUAL_INT(KEY_V, keypad_feed(&kp, 0x1001));
    keypad_feed(&kp, 0x1001 | 0x0202);                           /* two keys */
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x1001 | 0x0202));
    keypad_feed(&kp, 0x0202);                                    /* V released, Math held */
    TEST_ASSERT_EQUAL_INT(KEY_NONE, keypad_feed(&kp, 0x0202));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_logged_code_decodes);
    RUN_TEST(test_each_key_is_one_a_line_and_one_b_line);
    RUN_TEST(test_no_key_two_keys_and_unknown_codes_are_none);
    RUN_TEST(test_a_press_is_reported_once_after_it_settles);
    RUN_TEST(test_a_one_scan_glitch_is_ignored);
    RUN_TEST(test_rolling_onto_a_second_key_reports_nothing_until_release);
    return UNITY_END();
}
