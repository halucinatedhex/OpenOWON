/*
 * Drives the real driver against a mock HY3131 written independently from
 * the datasheet's description of the wire format. If the driver and the mock
 * disagree about the address shift, the idle bit, bit order or
 * auto-increment, these tests fail.
 */
#include <unity.h>
#include "hy3131.h"
#include <string.h>

/* ---- mock device ------------------------------------------------------- */

static struct {
    uint8_t regs[128];
    bool    cs, clk, mosi, miso;
    int     bit;            /* bits clocked since CS asserted */
    uint8_t addr;           /* latched register address */
    bool    read;           /* latched direction */
    uint8_t inbyte;         /* bits accumulating from the master */
    int     writes;         /* register writes performed */
    int     rising;         /* total rising edges, for timing checks */
} dev;

static void dev_reset(void)
{
    memset(&dev, 0, sizeof dev);
    dev.cs = false;
}

/* Index of the first data bit after CS: 8 address bits, +1 idle bit on read */
static int data_base(void) { return dev.read ? 9 : 8; }

static void dev_rising_edge(void)
{
    dev.rising++;
    int b = dev.bit;

    if (b < 8) {                                   /* address phase */
        dev.inbyte = (uint8_t)((dev.inbyte << 1) | (dev.mosi ? 1 : 0));
        if (b == 7) {
            dev.read = dev.inbyte & 1u;
            dev.addr = (uint8_t)((dev.inbyte >> 1) & 0x7Fu);
        }
    } else if (!dev.read && b >= 8) {              /* write data phase */
        int k = (b - 8) & 7;
        dev.inbyte = (uint8_t)((dev.inbyte << 1) | (dev.mosi ? 1 : 0));
        if (k == 7) {
            dev.regs[dev.addr & 0x7Fu] = dev.inbyte;
            dev.addr = (uint8_t)((dev.addr + 1) & 0x7Fu);
            dev.writes++;
        }
    }
    dev.bit++;
}

/* The slave presents the next MISO bit while the clock is low. */
static void dev_falling_edge(void)
{
    if (!dev.read) { dev.miso = false; return; }
    int b = dev.bit;
    if (b < data_base()) { dev.miso = false; return; }
    int off = b - data_base();
    uint8_t byte = dev.regs[(dev.addr + (off >> 3)) & 0x7Fu];
    dev.miso = (byte >> (7 - (off & 7))) & 1u;
}

static void io_cs(bool a)
{
    if (a && !dev.cs) { dev.bit = 0; dev.inbyte = 0; dev.read = false; }
    dev.cs = a;
    if (!a) dev.miso = false;
}
static void io_clk(bool level)
{
    if (!dev.cs) { dev.clk = level; return; }
    if (level && !dev.clk) dev_rising_edge();
    else if (!level && dev.clk) dev_falling_edge();
    dev.clk = level;
}
static void io_mosi(bool v) { dev.mosi = v; }
static bool io_miso(void)   { return dev.miso; }

static const hy3131_io_t MOCK = { io_cs, io_clk, io_mosi, io_miso, 0 };

void setUp(void)    { dev_reset(); hy3131_init(&MOCK); }
void tearDown(void) {}

/* ---- tests ------------------------------------------------------------- */

static void test_write_then_read_roundtrips(void)
{
    hy3131_write8(HY_R2F, 0x5A);
    TEST_ASSERT_EQUAL_HEX8(0x5A, dev.regs[HY_R2F]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, hy3131_read8(HY_R2F));
}

static void test_address_is_left_shifted_with_rw(void)
{
    /* A write to 0x2F must put 0x5E on the wire, not 0x2F. */
    hy3131_write8(0x2F, 0x11);
    TEST_ASSERT_EQUAL_HEX8(0x2F, dev.addr - 1);   /* auto-incremented past it */
    /* High register addresses must not collide with the rw bit. */
    hy3131_write8(0x7F, 0x22);
    TEST_ASSERT_EQUAL_HEX8(0x22, dev.regs[0x7F]);
}

static void test_read_consumes_the_idle_ninth_bit(void)
{
    dev.regs[HY_AD1_DATA] = 0xAB;
    TEST_ASSERT_EQUAL_HEX8(0xAB, hy3131_read8(HY_AD1_DATA));
    /* 8 address + 1 idle + 8 data = 17 clocks for a one-byte read. */
    TEST_ASSERT_EQUAL_INT(17, dev.rising);
}

static void test_write_has_no_idle_bit(void)
{
    hy3131_write8(HY_R23, 0x01);
    TEST_ASSERT_EQUAL_INT(16, dev.rising);        /* 8 address + 8 data */
}

static void test_burst_auto_increments(void)
{
    const uint8_t src[4] = { 1, 2, 3, 4 };
    hy3131_write_burst(HY_R2A, src, 4);
    TEST_ASSERT_EQUAL_HEX8(1, dev.regs[HY_R2A]);
    TEST_ASSERT_EQUAL_HEX8(2, dev.regs[HY_R2A + 1]);
    TEST_ASSERT_EQUAL_HEX8(3, dev.regs[HY_R2A + 2]);
    TEST_ASSERT_EQUAL_HEX8(4, dev.regs[HY_R2A + 3]);

    uint8_t back[4] = { 0 };
    hy3131_read_burst(HY_R2A, back, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(src, back, 4);
}

static void test_ad1_is_little_endian_and_signed(void)
{
    /* -1 == 0xFFFFFF, stored low byte first */
    dev.regs[0x00] = 0xFF; dev.regs[0x01] = 0xFF; dev.regs[0x02] = 0xFF;
    TEST_ASSERT_EQUAL_INT32(-1, hy3131_read_ad1());

    /* full positive scale 0x7FFFFF */
    dev.regs[0x00] = 0xFF; dev.regs[0x01] = 0xFF; dev.regs[0x02] = 0x7F;
    TEST_ASSERT_EQUAL_INT32(8388607, hy3131_read_ad1());

    /* full negative scale 0x800000 */
    dev.regs[0x00] = 0x00; dev.regs[0x01] = 0x00; dev.regs[0x02] = 0x80;
    TEST_ASSERT_EQUAL_INT32(-8388608, hy3131_read_ad1());

    /* an ordinary mid-scale value */
    dev.regs[0x00] = 0x34; dev.regs[0x01] = 0x12; dev.regs[0x02] = 0x00;
    TEST_ASSERT_EQUAL_INT32(0x001234, hy3131_read_ad1());
}

static void test_sext24_boundaries(void)
{
    TEST_ASSERT_EQUAL_INT32(0,         hy3131_sext24(0x000000));
    TEST_ASSERT_EQUAL_INT32(8388607,   hy3131_sext24(0x7FFFFF));
    TEST_ASSERT_EQUAL_INT32(-8388608,  hy3131_sext24(0x800000));
    TEST_ASSERT_EQUAL_INT32(-1,        hy3131_sext24(0xFFFFFF));
    /* bits above 24 must be ignored, not leak into the result */
    TEST_ASSERT_EQUAL_INT32(0,         hy3131_sext24(0xFF000000));
}

static void test_rms_is_40_bit_signed(void)
{
    memset(dev.regs + 0x09, 0xFF, 5);
    TEST_ASSERT_EQUAL_INT64(-1, hy3131_read_rms());
    memset(dev.regs + 0x09, 0x00, 5);
    dev.regs[0x0D] = 0x1F;                         /* 0x1FFFFFFFFF max */
    dev.regs[0x09] = dev.regs[0x0A] = dev.regs[0x0B] = dev.regs[0x0C] = 0xFF;
    TEST_ASSERT_EQUAL_INT64(0x1FFFFFFFFFll, hy3131_read_rms());
}

static void test_cs_frames_each_transaction(void)
{
    hy3131_read8(HY_INTF);
    TEST_ASSERT_FALSE_MESSAGE(dev.cs, "CS left asserted after a transaction");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_then_read_roundtrips);
    RUN_TEST(test_address_is_left_shifted_with_rw);
    RUN_TEST(test_read_consumes_the_idle_ninth_bit);
    RUN_TEST(test_write_has_no_idle_bit);
    RUN_TEST(test_burst_auto_increments);
    RUN_TEST(test_ad1_is_little_endian_and_signed);
    RUN_TEST(test_sext24_boundaries);
    RUN_TEST(test_rms_is_40_bit_signed);
    RUN_TEST(test_cs_frames_each_transaction);
    return UNITY_END();
}
