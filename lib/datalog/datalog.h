/* Data recording into flash: one session of float readings, written as they
 * arrive, that survives power loss.
 *
 * Stock keeps 1,000 points at 0x0803D800 (the region we never touch). This
 * uses free application flash instead, 0x08030000-0x0803A7FF: 21 pages,
 * 10,668 points (two pages above it hold the user calibration, B31).
 *
 * Layout: every 2 KB page starts with a 16-byte header, then 508 readings as
 * IEEE float32. An erased word (0xFFFFFFFF) ends the data, so no count has to
 * be rewritten; NaN is stored as 0x7FC00000 so a reading can never look
 * erased. Overload is +Inf.
 *
 *   header word 0  session number; with bit 23 set, also its start time
 *                  (Unix seconds, B33)
 *          word 1  page index in the session (bits 0-15), function (16-22),
 *                  bit 23 "word 0 is the start time", tag (24-31, the
 *                  app's unit variant)
 *          word 2  interval, ms (0 = manual)
 *          word 3  magic "XDML", programmed last
 *
 * A session always starts at the first page; later pages are erased as they
 * are reached. A rolling session (B35; bit 31 of header word 2) keeps going
 * when the region is full: page n of the session lives in slot n % npages,
 * so writing page n erases page n - npages, and the log always holds the
 * newest readings. Page numbers are absolute (bits 0-15), so the pages of a
 * rolling session run consecutively up to the newest; the oldest left is
 * where the run ends. A rolling session stops at page 0xFFFF (~33 million
 * readings). A page belongs to the session only if its header is complete
 * and carries the session number and the next page index, so stale pages
 * from an older session, or a header cut by a power loss, end the data.
 * After a power cut the session reopens stopped, holding every reading that
 * was completely written.
 *
 * Every flash access goes through store_io_t, so the host tests run against
 * the NOR-flash model. Addresses outside the configured region are refused
 * here, and the region itself must end at or below DLOG_END.
 */
#ifndef DATALOG_H
#define DATALOG_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

#define DLOG_BASE      0x08030000u
#define DLOG_END       0x0803A800u          /* the user calibration's pages start here
                                               (B31, 2026-11-12; was 0x0803B800, 23 pages) */
#define DLOG_PAGE      2048u
#define DLOG_HDR       16u
#define DLOG_PER_PAGE  ((DLOG_PAGE - DLOG_HDR) / 4u)     /* 508 */
#define DLOG_NPAGES    ((DLOG_END - DLOG_BASE) / DLOG_PAGE)   /* 23 */
#define DLOG_MAGIC     0x4C4D4458u          /* "XDML" */
#define DLOG_OVERLOAD  INFINITY

typedef struct {
    const store_io_t *io;
    uint32_t base;
    uint32_t npages;

    bool     have;          /* a session exists (running or finished) */
    bool     active;        /* appending */
    bool     wrap;          /* rolling: overwrite the oldest when full (B35) */
    uint32_t first;         /* oldest page still held (absolute page number) */
    uint32_t head;          /* absolute index of the next reading */
    uint32_t session;
    uint32_t next_session;
    uint32_t start_time;    /* B33: Unix seconds the session started, 0 if the clock wasn't set */
    uint8_t  fn, tag;
    uint32_t interval_ms;
    uint32_t count;         /* readings held, oldest first */
} dlog_t;

/* Scan the region and find the session in it. Never writes. Returns false if
 * the region is not acceptable (outside DLOG_BASE..DLOG_END). */
bool     dlog_open(dlog_t *d, const store_io_t *io, uint32_t base, uint32_t npages);
/* Start a new session, discarding the old one. Returns 0, or -1. */
int      dlog_start(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms);
/* The same, rolling: when full, the oldest page is overwritten (B35). */
int      dlog_start_rolling(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms);
/* Either kind, stamped with the clock (Unix seconds; 0 = no clock): the
 * session number becomes the start time when it can (B33). */
int      dlog_start_at(dlog_t *d, uint8_t fn, uint8_t tag, uint32_t interval_ms, bool rolling,
                       uint32_t now);
/* Append one reading. Returns 0; -1 when full (the session stops; a rolling
 * one never fills) or the flash refused (the session stops, what is written
 * stays readable). */
int      dlog_append(dlog_t *d, float v);
void     dlog_stop(dlog_t *d);
bool     dlog_get(const dlog_t *d, uint32_t i, float *v);
uint32_t dlog_capacity(const dlog_t *d);

#endif
