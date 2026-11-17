/*
 * HY3131 driver.
 *
 * Protocol, from DS-HY3131-V10_EN section 7:
 *
 *   - The first byte on the wire is (address << 1) | rw, MSB first, with
 *     rw = 1 for read. The datasheet states this as "register address must
 *     left shift 1 bit and output".
 *   - READ: the ninth bit must be idle before data appears, so one extra
 *     clock is issued after the address byte and its value discarded.
 *   - WRITE: data follows the address byte immediately, with no idle bit.
 *   - The address auto-increments for as long as CS stays asserted, so a
 *     multi-byte register is one burst rather than several transactions.
 *
 * Clocking is SPI mode 0, matching the stock firmware's bit-bang at
 * 0x080077C8: MOSI is set while the clock is low, the device latches it on
 * the rising edge, and MISO is sampled on that same rising edge.
 */

#include "hy3131.h"

static const hy3131_io_t *IO;

void hy3131_init(const hy3131_io_t *io)
{
    IO = io;
    if (!IO) return;
    IO->cs(false);
    IO->clk(false);
    IO->mosi(false);
}

/* One clock period. Drives `out` and returns the bit seen on MISO. */
static bool clock_bit(bool out)
{
    IO->mosi(out);
    if (IO->delay) IO->delay();
    IO->clk(true);                      /* rising edge latches both ways */
    bool in = IO->miso ? IO->miso() : false;
    if (IO->delay) IO->delay();
    IO->clk(false);
    return in;
}

static void put_byte(uint8_t v)
{
    for (int i = 7; i >= 0; i--) clock_bit((v >> i) & 1u);
}

static uint8_t get_byte(void)
{
    uint8_t v = 0;
    for (int i = 7; i >= 0; i--) v = (uint8_t)((v << 1) | clock_bit(false));
    return v;
}

/* Assert CS and send the address byte. For reads, also burn the idle bit. */
static void begin(uint8_t reg, bool read)
{
    IO->cs(true);
    put_byte((uint8_t)((reg << 1) | (read ? 1u : 0u)));
    if (read) (void)clock_bit(false);   /* the mandatory ninth bit */
}

static void end(void)
{
    IO->cs(false);
}

void hy3131_read_burst(uint8_t reg, uint8_t *dst, unsigned n)
{
    if (!IO || !dst || !n) return;
    begin(reg, true);
    for (unsigned i = 0; i < n; i++) dst[i] = get_byte();
    end();
}

void hy3131_write_burst(uint8_t reg, const uint8_t *src, unsigned n)
{
    if (!IO || !src || !n) return;
    begin(reg, false);
    for (unsigned i = 0; i < n; i++) put_byte(src[i]);
    end();
}

uint8_t hy3131_read8(uint8_t reg)
{
    uint8_t v = 0;
    hy3131_read_burst(reg, &v, 1);
    return v;
}

void hy3131_write8(uint8_t reg, uint8_t val)
{
    hy3131_write_burst(reg, &val, 1);
}

/* ---- data conversion ---------------------------------------------------
 * AD1 is 24-bit two's complement: 0x7FFFFF max, 0x800000 min.
 * AD2 / LPF / PKH are 19-bit, already sign-extended by the chip into
 * bits 23:19, so the same 24-bit sign extension is correct for them too.
 */
int32_t hy3131_sext24(uint32_t raw)
{
    raw &= 0xFFFFFFu;
    return (int32_t)(raw & 0x800000u ? raw | 0xFF000000u : raw);
}

static uint32_t le24(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
}

int32_t hy3131_read_ad1(void)
{
    uint8_t b[3];
    hy3131_read_burst(HY_AD1_DATA, b, 3);
    return hy3131_sext24(le24(b));
}

int32_t hy3131_read_ad2(void)
{
    uint8_t b[3];
    hy3131_read_burst(HY_AD2_DATA, b, 3);
    return hy3131_sext24(le24(b));
}

/* RMS is 40 bits: 0x1FFFFFFFFF max, 0xE000000000 min. */
int64_t hy3131_read_rms(void)
{
    uint8_t b[5];
    hy3131_read_burst(HY_RMS_DATA, b, 5);
    uint64_t raw = 0;
    for (int i = 4; i >= 0; i--) raw = (raw << 8) | b[i];
    raw &= 0xFFFFFFFFFFull;
    if (raw & 0x8000000000ull) raw |= 0xFFFFFF0000000000ull;
    return (int64_t)raw;
}
