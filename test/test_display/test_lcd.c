/* Checks the driver against the byte stream the stock firmware emits. */
#include <unity.h>
#include "lcd.h"
#include <string.h>

#define CAP 4096
static struct { uint8_t b[CAP]; bool dc[CAP]; int n; } bus;

static void m_wr(uint8_t b) { if (bus.n < CAP) { bus.b[bus.n] = b; bus.dc[bus.n] = bus.n ? bus.dc[bus.n-1] : false; bus.n++; } }
static bool cur_dc;
static void m_wr2(uint8_t b) { if (bus.n < CAP) { bus.b[bus.n] = b; bus.dc[bus.n] = cur_dc; bus.n++; } }
static void m_dc(bool d) { cur_dc = d; }

static const lcd_io_t IO = { m_wr2, m_dc, 0, 0 };
void setUp(void) { memset(&bus, 0, sizeof bus); cur_dc = false; lcd_init(&IO); }
void tearDown(void) {}

static void test_set_window_matches_stock_sequence(void)
{
    lcd_set_window(0x0010, 0x01EF, 0x0020, 0x013F);
    /* cmd 0x12, 4 data; cmd 0x13, 4 data; cmd 0x14 = 11 bytes */
    TEST_ASSERT_EQUAL_INT(11, bus.n);
    const uint8_t want[] = {0x12, 0x00,0x10, 0x01,0xEF,
                            0x13, 0x00,0x20, 0x01,0x3F,
                            0x14};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, bus.b, 11);
    /* commands must go out with D/C low, payload with D/C high */
    TEST_ASSERT_FALSE(bus.dc[0]);
    TEST_ASSERT_TRUE(bus.dc[1]);
    TEST_ASSERT_FALSE(bus.dc[5]);
    TEST_ASSERT_FALSE(bus.dc[10]);
}

static void test_fill_is_rgb565_high_byte_first(void)
{
    lcd_fill_rect(0, 0, 2, 1, 0xABCD);
    TEST_ASSERT_EQUAL_INT(11 + 4, bus.n);
    TEST_ASSERT_EQUAL_HEX8(0xAB, bus.b[11]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, bus.b[12]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, bus.b[13]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, bus.b[14]);
}

static void test_window_is_inclusive(void)
{
    lcd_fill_rect(10, 20, 4, 3, 0);
    /* x1 = 10+4-1 = 13, y1 = 20+3-1 = 22 */
    TEST_ASSERT_EQUAL_HEX8(13, bus.b[4]);
    TEST_ASSERT_EQUAL_HEX8(22, bus.b[9]);
    TEST_ASSERT_EQUAL_INT(11 + 4*3*2, bus.n);
}

static void test_blit_respects_stride(void)
{
    static uint16_t fb[4*4];
    for (int i = 0; i < 16; i++) fb[i] = (uint16_t)(0x1000 + i);
    lcd_blit(0, 0, 2, 2, fb, 4);              /* top-left 2x2 of a 4-wide fb */
    TEST_ASSERT_EQUAL_INT(11 + 8, bus.n);
    TEST_ASSERT_EQUAL_HEX8(0x10, bus.b[11]); TEST_ASSERT_EQUAL_HEX8(0x00, bus.b[12]);
    TEST_ASSERT_EQUAL_HEX8(0x10, bus.b[13]); TEST_ASSERT_EQUAL_HEX8(0x01, bus.b[14]);
    /* second row must come from index 4, not index 2 */
    TEST_ASSERT_EQUAL_HEX8(0x04, bus.b[16]);
    TEST_ASSERT_EQUAL_HEX8(0x05, bus.b[18]);
}

static void test_command_map_translates_indices(void)
{
    static const uint8_t map[0x15] = { [0x12] = 0x2A, [0x13] = 0x2B, [0x14] = 0x2C };
    static const lcd_io_t mapped = { m_wr2, m_dc, map, sizeof map };
    lcd_init(&mapped);
    lcd_set_window(0, 1, 0, 1);
    TEST_ASSERT_EQUAL_HEX8(0x2A, bus.b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x2B, bus.b[5]);
    TEST_ASSERT_EQUAL_HEX8(0x2C, bus.b[10]);
}

static void test_zero_size_draws_nothing(void)
{
    lcd_fill_rect(0, 0, 0, 10, 0xFFFF);
    TEST_ASSERT_EQUAL_INT(0, bus.n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_window_matches_stock_sequence);
    RUN_TEST(test_fill_is_rgb565_high_byte_first);
    RUN_TEST(test_window_is_inclusive);
    RUN_TEST(test_blit_respects_stride);
    RUN_TEST(test_command_map_translates_indices);
    RUN_TEST(test_zero_size_draws_nothing);
    return UNITY_END();
}
