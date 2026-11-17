/* See store.h.
 *
 * Record layout, little-endian, rec_size bytes (STORE_RECORD_SIZE unless
 * opened with store_open_sized):
 *   +0   magic    "XDMS", programmed last
 *   +4   seq      monotonically increasing
 *   +8   len      payload bytes (u16), version (u16)
 *   +12  crc32    over bytes +4..+11 and the payload
 *   +16  payload, padded with 0xFF
 */
#include "store.h"

#include <string.h>

#define SLOTS(s) ((int)(STORE_PAGE_SIZE / (s)->rec_size))
#define REC(s)   ((s)->rec_size)

size_t store_max_payload(const store_t *s) { return REC(s) - STORE_HEADER_SIZE; }

uint32_t store_crc32(const void *data, size_t n, uint32_t crc)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool ours(const store_t *s, uint32_t addr, size_t n)
{
    for (int p = 0; p < 2; p++)
        if (addr >= s->page[p] && addr + n <= s->page[p] + STORE_PAGE_SIZE) return true;
    return false;
}

static uint32_t slot_addr(const store_t *s, int page, int slot)
{
    return s->page[page] + (uint32_t)slot * REC(s);
}

static bool slot_erased(const store_t *s, int page, int slot)
{
    uint8_t b[STORE_RECORD_MAX];
    s->io->read(slot_addr(s, page, slot), b, REC(s));
    for (size_t i = 0; i < REC(s); i++) if (b[i] != 0xFF) return false;
    return true;
}

/* Valid record? Fills seq/len/version. */
static bool slot_valid(const store_t *s, int page, int slot, uint32_t *seq,
                       uint16_t *len, uint16_t *ver, uint8_t *rec)
{
    uint8_t b[STORE_RECORD_MAX];
    if (!rec) rec = b;
    s->io->read(slot_addr(s, page, slot), rec, REC(s));
    uint32_t magic, crc;
    memcpy(&magic, rec, 4);
    memcpy(seq, rec + 4, 4);
    memcpy(len, rec + 8, 2);
    memcpy(ver, rec + 10, 2);
    memcpy(&crc, rec + 12, 4);
    if (magic != STORE_MAGIC || *len > store_max_payload(s)) return false;
    uint32_t c = store_crc32(rec + 4, 8, 0);
    c = store_crc32(rec + STORE_HEADER_SIZE, *len, c);
    return c == crc;
}

bool store_open(store_t *s, const store_io_t *io, uint32_t page0, uint32_t page1)
{
    return store_open_sized(s, io, page0, page1, STORE_RECORD_SIZE);
}

bool store_open_sized(store_t *s, const store_io_t *io, uint32_t page0, uint32_t page1,
                      uint32_t rec_size)
{
    memset(s, 0, sizeof *s);
    if (rec_size < STORE_RECORD_SIZE || rec_size > STORE_RECORD_MAX || (rec_size & (rec_size - 1)))
        return false;
    s->rec_size = rec_size;
    s->io = io;
    s->page[0] = page0;
    s->page[1] = page1;
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < SLOTS(s); k++) {
            uint32_t seq; uint16_t len, ver;
            if (!slot_valid(s, p, k, &seq, &len, &ver, NULL)) continue;
            if (!s->have || (int32_t)(seq - s->seq) > 0) {
                s->have = true; s->seq = seq; s->cur_page = p; s->cur_slot = k;
            }
        }
    return s->have;
}

int store_load(const store_t *s, void *dst, size_t cap, uint16_t *version)
{
    if (!s->have) return -1;
    uint8_t rec[STORE_RECORD_MAX];
    uint32_t seq; uint16_t len, ver;
    if (!slot_valid(s, s->cur_page, s->cur_slot, &seq, &len, &ver, rec)) return -1;
    if (len > cap) return -1;
    memcpy(dst, rec + STORE_HEADER_SIZE, len);
    if (version) *version = ver;
    return len;
}

static int write_record(store_t *s, int page, int slot, const uint8_t *rec)
{
    uint32_t a = slot_addr(s, page, slot);
    if (!ours(s, a, REC(s))) return -1;
    /* Everything except the magic first; the magic commits the record. */
    for (uint32_t o = 4; o < REC(s); o += 4) {
        uint32_t w;
        memcpy(&w, rec + o, 4);
        if (w != 0xFFFFFFFFu && s->io->program_word(a + o, w)) return -1;
    }
    uint32_t magic;
    memcpy(&magic, rec, 4);
    if (s->io->program_word(a, magic)) return -1;
    uint8_t back[STORE_RECORD_MAX];
    s->io->read(a, back, REC(s));
    return memcmp(back, rec, REC(s)) ? -1 : 0;
}

int store_save(store_t *s, const void *payload, size_t len, uint16_t version)
{
    if (!s->rec_size || len > store_max_payload(s)) return -1;
    uint8_t rec[STORE_RECORD_MAX];
    memset(rec, 0xFF, REC(s));
    uint32_t magic = STORE_MAGIC, seq = s->have ? s->seq + 1u : 1u;
    uint16_t l = (uint16_t)len;
    memcpy(rec, &magic, 4);
    memcpy(rec + 4, &seq, 4);
    memcpy(rec + 8, &l, 2);
    memcpy(rec + 10, &version, 2);
    memcpy(rec + STORE_HEADER_SIZE, payload, len);
    uint32_t crc = store_crc32(rec + 4, 8, 0);
    crc = store_crc32(rec + STORE_HEADER_SIZE, len, crc);
    memcpy(rec + 12, &crc, 4);

    /* Next erased slot after the current record, in the current page. Slots
     * holding a torn write are skipped, not reused. */
    int page = s->have ? s->cur_page : 0;
    int start = s->have ? s->cur_slot + 1 : 0;
    for (int k = start; k < SLOTS(s); k++) {
        if (!slot_erased(s, page, k)) continue;
        if (write_record(s, page, k, rec) == 0) {
            s->have = true; s->seq = seq; s->cur_page = page; s->cur_slot = k;
            return 0;
        }
    }
    /* Page full, or no record yet and page 0 has no usable slot (e.g. leftover
     * data): erase the other page. It holds only older records - the newest
     * is in `page` - so a power cut during this erase loses nothing. */
    int other = 1 - page;
    if (!ours(s, s->page[other], STORE_PAGE_SIZE)) return -1;
    if (s->io->erase_page(s->page[other])) return -1;
    for (int k = 0; k < SLOTS(s); k++) {
        if (!slot_erased(s, other, k)) continue;
        if (write_record(s, other, k, rec) == 0) {
            s->have = true; s->seq = seq; s->cur_page = other; s->cur_slot = k;
            return 0;
        }
    }
    return -1;
}
