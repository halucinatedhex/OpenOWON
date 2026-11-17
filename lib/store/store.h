/* Power-safe record storage in two flash pages.
 *
 * Stock writes its settings by erasing a page and reprogramming it in place
 * (stock_store_write 0x08006564), so a power cut mid-write loses them. This
 * appends fixed-size records instead:
 *
 *   - each record carries a sequence number and a CRC32, so a torn write
 *     never validates and the previous record stays current (the power-cut
 *     test cuts every operation of a save and checks this);
 *   - the magic word is programmed last as well, so a torn record is not
 *     even a candidate - defence in depth against a CRC collision, not the
 *     primary protection (the tests pass without it);
 *   - records fill one page, then move to the other, which is erased first;
 *     a page is only erased when the other holds a newer valid record.
 *
 * Every flash access goes through store_io_t, so the same code runs against
 * a NOR-flash model on the host (lib/sim/sim_flash.c) and the GD32 FMC on the
 * meter. Any address outside the two configured pages is refused here,
 * independently of the HAL.
 */
#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORE_PAGE_SIZE   2048u
#define STORE_RECORD_SIZE  128u
#define STORE_HEADER_SIZE   16u
#define STORE_MAX_PAYLOAD  (STORE_RECORD_SIZE - STORE_HEADER_SIZE)
#define STORE_RECORD_MAX   512u            /* store_open_sized: 128, 256 or 512 */
#define STORE_MAGIC        0x534D4458u     /* "XDMS" */

typedef struct {
    /* Read n bytes at addr. */
    void (*read)(uint32_t addr, void *dst, size_t n);
    /* Erase the page starting at addr. Returns 0 on success. */
    int  (*erase_page)(uint32_t addr);
    /* Program one 32-bit word at a 4-byte-aligned addr. Returns 0 on success. */
    int  (*program_word)(uint32_t addr, uint32_t value);
} store_io_t;

typedef struct {
    const store_io_t *io;
    uint32_t page[2];          /* page base addresses */
    uint32_t rec_size;         /* bytes per record (STORE_RECORD_SIZE by default) */
    /* newest valid record, found by store_open */
    bool     have;
    uint32_t seq;
    int      cur_page;
    int      cur_slot;
} store_t;

/* Our pages: the first two of 0x0803B800-0x0803D7FF (linker/app.ld). */
#define STORE_PAGE0 0x0803B800u
#define STORE_PAGE1 0x0803C000u

/* Scan both pages. Returns true if a valid record exists. */
bool store_open(store_t *s, const store_io_t *io, uint32_t page0, uint32_t page1);
/* The same with another record size (a power of two, 128..STORE_RECORD_MAX),
 * for payloads larger than STORE_MAX_PAYLOAD; the user calibration uses 512. */
bool store_open_sized(store_t *s, const store_io_t *io, uint32_t page0, uint32_t page1,
                      uint32_t rec_size);
/* Largest payload this store takes. */
size_t store_max_payload(const store_t *s);

/* Copy the newest record's payload. Returns its length, or -1 if none. */
int  store_load(const store_t *s, void *dst, size_t cap, uint16_t *version);

/* Append a new record. Returns 0, or -1 on failure (the previous record is
 * still intact and current). */
int  store_save(store_t *s, const void *payload, size_t len, uint16_t version);

uint32_t store_crc32(const void *data, size_t n, uint32_t crc);

#endif
