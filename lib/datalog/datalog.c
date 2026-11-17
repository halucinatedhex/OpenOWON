/* See datalog.h. */
#include "datalog.h"

#include <string.h>

#define EMPTY 0xFFFFFFFFu
#define WRAP_BIT 0x80000000u
#define MAX_PAGE 0xFFFFu
#define TIMED_BIT 0x80u         /* in the fn byte: the session number is its start time (B33) */

static uint32_t page_addr(const dlog_t *d, uint32_t p) { return d->base + p * DLOG_PAGE; }

/* Our own address guard, independent of the HAL's. */
static bool in_region(const dlog_t *d, uint32_t addr, uint32_t n)
{
    return addr >= d->base && addr + n <= d->base + d->npages * DLOG_PAGE;
}

static uint32_t rd(const dlog_t *d, uint32_t addr)
{
    uint32_t v;
    d->io->read(addr, &v, 4);
    return v;
}

static int prog(const dlog_t *d, uint32_t addr, uint32_t v)
{
    if (!in_region(d, addr, 4) || (addr & 3u)) return -1;
    return d->io->program_word(addr, v);
}

static int erase(const dlog_t *d, uint32_t p)
{
    if (p >= d->npages) return -1;
    return d->io->erase_page(page_addr(d, p));
}

typedef struct { uint32_t session, idx, fn, tag, interval; bool timed; } hdr_t;

static bool read_hdr(const dlog_t *d, uint32_t p, hdr_t *h)
{
    uint32_t w[4];
    d->io->read(page_addr(d, p), w, sizeof w);
    if (w[3] != DLOG_MAGIC || w[0] == EMPTY) return false;
    h->session = w[0];
    h->idx = w[1] & 0xFFFFu;
    h->fn = (w[1] >> 16) & 0x7Fu;
    h->timed = ((w[1] >> 16) & TIMED_BIT) != 0;
    h->tag = w[1] >> 24;
    h->interval = w[2];
    return true;
}

/* Page n of the session (absolute) is in slot n % npages. */
static uint32_t slot_of(const dlog_t *d, uint32_t n) { return n % d->npages; }

static int write_hdr(dlog_t *d, uint32_t n)
{
    uint32_t s = slot_of(d, n), a = page_addr(d, s);
    if (erase(d, s)) return -1;
    if (prog(d, a + 0, d->session)) return -1;
    uint32_t fb = (uint32_t)(d->fn & 0x7Fu) | (d->start_time ? TIMED_BIT : 0u);
    if (prog(d, a + 4, n | fb << 16 | (uint32_t)d->tag << 24)) return -1;
    if (prog(d, a + 8, d->interval_ms | (d->wrap ? WRAP_BIT : 0u))) return -1;
    return prog(d, a + 12, DLOG_MAGIC);             /* last: the page now counts */
}

bool dlog_open(dlog_t *d, const store_io_t *io, uint32_t base, uint32_t npages)
{
    memset(d, 0, sizeof *d);
    if (base < DLOG_BASE || base % DLOG_PAGE || npages == 0 ||
        base + npages * DLOG_PAGE > DLOG_END)
        return false;
    d->io = io;
    d->base = base;
    d->npages = npages;

    /* Session numbers in use anywhere, so a new one never matches a stale page. */
    uint32_t max = 0;
    hdr_t h;
    for (uint32_t p = 0; p < npages; p++)
        if (read_hdr(d, p, &h) && h.session > max) max = h.session;
    d->next_session = max + 1 == EMPTY ? 1 : max + 1;

    /* The newest session is the one with the highest number among complete
     * headers. Its pages must run consecutively down from its newest page;
     * a plain session must reach page 0, a rolling one may have lost its
     * oldest pages to the wrap. */
    uint32_t sess = 0, last = 0;
    bool any = false;
    for (uint32_t p = 0; p < npages; p++) {
        if (!read_hdr(d, p, &h)) continue;
        if (!any || h.session > sess) { sess = h.session; last = h.idx; any = true; }
        else if (h.session == sess && h.idx > last) last = h.idx;
    }
    if (!any) return true;
    hdr_t hl;
    if (!read_hdr(d, slot_of(d, last), &hl) || hl.session != sess || hl.idx != last) return true;
    bool wrap = (hl.interval & WRAP_BIT) != 0;
    uint32_t first = last;
    while (first > 0 && last - (first - 1) < npages) {
        hdr_t hp;
        if (!read_hdr(d, slot_of(d, first - 1), &hp) || hp.session != sess || hp.idx != first - 1) break;
        first--;
    }
    if (!wrap && first != 0) return true;           /* a plain session lost its start */
    uint32_t a = page_addr(d, slot_of(d, last)) + DLOG_HDR, n = 0;
    while (n < DLOG_PER_PAGE && rd(d, a + n * 4) != EMPTY) n++;
    d->have = true;
    d->wrap = wrap;
    d->session = sess;
    d->fn = (uint8_t)hl.fn;
    d->start_time = hl.timed ? sess : 0;
    d->tag = (uint8_t)hl.tag;
    d->interval_ms = hl.interval & ~WRAP_BIT;
    d->first = first;
    d->head = last * DLOG_PER_PAGE + n;
    d->count = d->head - first * DLOG_PER_PAGE;
    return true;
}

static int start(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms, bool wrap, uint32_t now)
{
    if (!d->io) return -1;
    d->active = false;
    d->have = false;
    d->count = d->head = d->first = 0;
    d->wrap = wrap;
    /* B33: with the clock set, the session number is its start time (Unix
     * seconds), as long as that stays above every number in use; else the
     * next number, and no start time. */
    bool timed = now != 0 && now != EMPTY && now >= d->next_session;
    d->session = timed ? now : d->next_session;
    d->start_time = timed ? now : 0;
    d->next_session = d->session + 1 == EMPTY ? 1 : d->session + 1;
    d->fn = fn;
    d->tag = tag;
    d->interval_ms = interval_ms & ~WRAP_BIT;
    if (write_hdr(d, 0)) return -1;
    d->have = true;
    d->active = true;
    return 0;
}

int dlog_start(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms)
{
    return start(d, fn, tag, interval_ms, false, 0);
}

int dlog_start_rolling(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms)
{
    return start(d, fn, tag, interval_ms, true, 0);
}

int dlog_append(dlog_t *d, float v)
{
    if (!d->active) return -1;
    if (!d->wrap && d->count >= dlog_capacity(d)) { d->active = false; return -1; }
    uint32_t n = d->head / DLOG_PER_PAGE, s = d->head % DLOG_PER_PAGE;
    if (s == 0 && n > 0) {
        if (n > MAX_PAGE || write_hdr(d, n)) { d->active = false; return -1; }
        if (n >= d->first + d->npages) {            /* the oldest page is gone */
            d->first = n - d->npages + 1;
            d->count = d->head - d->first * DLOG_PER_PAGE;
        }
    }
    uint32_t w;
    if (v != v) w = 0x7FC00000u;                    /* any NaN: never looks erased */
    else memcpy(&w, &v, 4);
    if (prog(d, page_addr(d, slot_of(d, n)) + DLOG_HDR + s * 4, w)) { d->active = false; return -1; }
    d->head++;
    d->count++;
    if (!d->wrap && d->count >= dlog_capacity(d)) d->active = false;
    return 0;
}

void dlog_stop(dlog_t *d) { d->active = false; }

bool dlog_get(const dlog_t *d, uint32_t i, float *v)
{
    if (!d->have || i >= d->count) return false;
    uint32_t k = d->first * DLOG_PER_PAGE + i;
    uint32_t w = rd(d, page_addr(d, slot_of(d, k / DLOG_PER_PAGE)) + DLOG_HDR + (k % DLOG_PER_PAGE) * 4);
    memcpy(v, &w, 4);
    return true;
}

uint32_t dlog_capacity(const dlog_t *d) { return d->npages * DLOG_PER_PAGE; }

int dlog_start_at(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms, bool rolling, uint32_t now)
{
    return start(d, fn, tag, interval_ms, rolling, now);
}
