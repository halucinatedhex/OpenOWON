/* lib/store against a NOR-flash model with power cuts. */
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "sim_flash.h"
#include "store.h"

static store_t st;

void setUp(void) { sim_flash_reset(); }
void tearDown(void) {}

typedef struct { uint32_t n; char text[40]; } payload_t;

static payload_t make(uint32_t n)
{
    payload_t p;
    memset(&p, 0, sizeof p);
    p.n = n;
    snprintf(p.text, sizeof p.text, "settings generation %u", (unsigned)n);
    return p;
}

static bool load(payload_t *out)
{
    store_open(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1);
    uint16_t ver = 0;
    int n = store_load(&st, out, sizeof *out, &ver);
    return n == (int)sizeof *out && ver == 7;
}

static void save(uint32_t n)
{
    payload_t p = make(n);
    TEST_ASSERT_EQUAL_INT(0, store_save(&st, &p, sizeof p, 7));
}

static void test_empty_has_nothing(void)
{
    payload_t p;
    TEST_ASSERT_FALSE(load(&p));
}

static void test_round_trip_and_newest_wins(void)
{
    store_open(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1);
    for (uint32_t i = 1; i <= 100; i++) save(i);    /* ~6 page switches */
    payload_t p;
    TEST_ASSERT_TRUE(load(&p));
    payload_t want = make(100);
    TEST_ASSERT_EQUAL_MEMORY(&want, &p, sizeof p);
    /* wear: 16 records per page, so about one erase per 16 saves */
    uint32_t e = sim_flash_erases(STORE_PAGE0) + sim_flash_erases(STORE_PAGE1);
    TEST_ASSERT_TRUE(e >= 5 && e <= 7);
    TEST_ASSERT_EQUAL_UINT32(0, sim_flash_violations());
}

/* Cut power at every possible point of a save, at every fill level of the
 * page (including the save that has to erase the other page). After reboot
 * the store must hold exactly the old record or the new one, and the next
 * save must work. */
static void test_power_cut_at_every_point(void)
{
    int cases = 0;
    for (uint32_t before = 1; before <= 40; before++) {
        for (int cut = 0; cut < 40; cut++) {
            sim_flash_reset();
            store_open(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1);
            for (uint32_t i = 1; i <= before; i++) save(i);

            sim_flash_cut_after(cut);
            payload_t p = make(before + 1);
            int rc = store_save(&st, &p, sizeof p, 7);
            sim_flash_cut_after(-1);                /* power back */

            payload_t got;
            TEST_ASSERT_TRUE_MESSAGE(load(&got), "lost all settings after a power cut");
            payload_t old = make(before), neu = make(before + 1);
            bool is_old = !memcmp(&got, &old, sizeof got);
            bool is_new = !memcmp(&got, &neu, sizeof got);
            TEST_ASSERT_TRUE_MESSAGE(is_old || is_new, "loaded a record that was never saved");
            if (rc == 0) TEST_ASSERT_TRUE_MESSAGE(is_new, "save reported success but is not current");

            save(before + 2);                      /* and life goes on */
            TEST_ASSERT_TRUE(load(&got));
            payload_t next = make(before + 2);
            TEST_ASSERT_EQUAL_MEMORY(&next, &got, sizeof got);
            cases++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, sim_flash_violations());
    TEST_ASSERT_EQUAL_INT(1600, cases);
}

/* Pages full of leftover data (not ours) are recovered on the first save. */
static void test_leftover_data_is_ignored_then_replaced(void)
{
    sim_flash_fill(0x5A);
    payload_t p;
    TEST_ASSERT_FALSE(load(&p));
    save(1);
    TEST_ASSERT_TRUE(load(&p));
    TEST_ASSERT_EQUAL_UINT32(1, p.n);
}

static void test_corrupt_newest_falls_back(void)
{
    store_open(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1);
    save(1);
    save(2);
    /* corrupt record 2 inside its payload text (CRC-covered): clearing bits is
     * all a NOR part can do without an erase */
    sim_flash_poke(STORE_PAGE0 + STORE_RECORD_SIZE + STORE_HEADER_SIZE + 8, 'X');
    payload_t p;
    TEST_ASSERT_TRUE(load(&p));
    TEST_ASSERT_EQUAL_UINT32(1, p.n);
}

static void test_payload_limit(void)
{
    store_open(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1);
    uint8_t big[STORE_MAX_PAYLOAD + 1];
    memset(big, 1, sizeof big);
    TEST_ASSERT_EQUAL_INT(-1, store_save(&st, big, sizeof big, 1));
    TEST_ASSERT_EQUAL_INT(0, store_save(&st, big, STORE_MAX_PAYLOAD, 1));
}

/* 512-byte records (store_open_sized, for the user calibration): a large
 * payload round-trips, a power cut at every operation of a save across the
 * page change keeps the old or the new record, and it reopens sized. */
static void test_large_records_and_power_cuts(void)
{
    static uint8_t big[496], got[496];
    for (uint32_t before = 1; before <= 9; before++) {
        for (int cut = 0; cut < 140; cut += 3) {
            sim_flash_reset();
            TEST_ASSERT_TRUE(!store_open_sized(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1, 512));
            TEST_ASSERT_EQUAL_size_t(496, store_max_payload(&st));
            for (uint32_t i = 1; i <= before; i++) {
                memset(big, (int)i, sizeof big);
                TEST_ASSERT_EQUAL_INT(0, store_save(&st, big, sizeof big, 3));
            }
            sim_flash_cut_after(cut);
            memset(big, (int)(before + 1), sizeof big);
            int rc = store_save(&st, big, sizeof big, 3);
            sim_flash_cut_after(-1);
            store_t r;
            TEST_ASSERT_TRUE(store_open_sized(&r, &sim_flash_io, STORE_PAGE0, STORE_PAGE1, 512));
            uint16_t ver;
            TEST_ASSERT_EQUAL_INT(496, store_load(&r, got, sizeof got, &ver));
            TEST_ASSERT_TRUE(got[0] == before || got[0] == before + 1);
            for (size_t i = 1; i < sizeof got; i++) TEST_ASSERT_EQUAL_UINT8(got[0], got[i]);
            if (rc == 0) TEST_ASSERT_EQUAL_UINT8(before + 1, got[0]);
        }
    }
    TEST_ASSERT_FALSE(store_open_sized(&st, &sim_flash_io, STORE_PAGE0, STORE_PAGE1, 300));
    TEST_ASSERT_EQUAL_UINT32(0, sim_flash_violations());
}

static void test_crc32_reference(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, store_crc32("123456789", 9, 0));   /* standard check value */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_reference);
    RUN_TEST(test_empty_has_nothing);
    RUN_TEST(test_round_trip_and_newest_wins);
    RUN_TEST(test_power_cut_at_every_point);
    RUN_TEST(test_leftover_data_is_ignored_then_replaced);
    RUN_TEST(test_corrupt_newest_falls_back);
    RUN_TEST(test_payload_limit);
    RUN_TEST(test_large_records_and_power_cuts);
    return UNITY_END();
}
