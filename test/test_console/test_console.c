/* lib/console: the DIAG:BOOT? flash read-out format.
 * Golden values from Python's zlib (independent CRC-32), and the same text
 * scripts/test_read_boot.py feeds the host parser. */
#include <unity.h>
#include <string.h>

#include "flashdump.h"

static char out[4096];
static size_t used;

static void collect(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    TEST_ASSERT_TRUE(used + n < sizeof out);
    memcpy(out + used, s, n);
    used += n;
    out[used] = '\0';
}

void setUp(void) { used = 0; out[0] = '\0'; }
void tearDown(void) {}

static void test_crc32_check_value(void)
{
    /* The standard CRC-32/IEEE check value for "123456789". */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
        flashdump_crc32(0, (const uint8_t *)"123456789", 9));
}

static void test_crc32_continues_across_chunks(void)
{
    const uint8_t *s = (const uint8_t *)"123456789";
    uint32_t c = flashdump_crc32(0, s, 4);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, flashdump_crc32(c, s + 4, 5));
}

static void test_emit_matches_golden_text(void)
{
    uint8_t mem[40];
    for (int i = 0; i < 40; i++) mem[i] = (uint8_t)i;
    flashdump_emit(mem, 0x08000000u, sizeof mem, collect, NULL);
    TEST_ASSERT_EQUAL_STRING(
        "08000000 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\r\n"
        "08000020 2021222324252627\r\n"
        "END 08000000 00000028 0da62e3c\r\n", out);
}

/* The bootloader is 0x2800 bytes: 320 full lines, then the trailer. */
static size_t lines_seen, bytes_seen;
static void count(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    bytes_seen += n;
    if (n >= 2 && s[n - 1] == '\n') lines_seen++;
    if (n > 4 && memcmp(s, "END ", 4) == 0) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
}

static void test_bootloader_sized_dump(void)
{
    static uint8_t mem[0x2800];
    memset(mem, 0xFF, sizeof mem);
    lines_seen = bytes_seen = 0;
    flashdump_emit(mem, 0x08000000u, sizeof mem, count, NULL);
    TEST_ASSERT_EQUAL(321, lines_seen);
    TEST_ASSERT_EQUAL(320 * (8 + 1 + 64 + 2) + 32, bytes_seen);   /* END line: 32 */
    TEST_ASSERT_EQUAL(0, strncmp(out, "END 08000000 00002800 ", 22));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_crc32_continues_across_chunks);
    RUN_TEST(test_emit_matches_golden_text);
    RUN_TEST(test_bootloader_sized_dump);
    return UNITY_END();
}
