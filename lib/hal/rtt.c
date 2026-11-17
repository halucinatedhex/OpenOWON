/* TARGET ONLY. Minimal RTT. See rtt.h. */
#include "rtt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RTT_UP_SIZE   1024u
#define RTT_DOWN_SIZE   16u

typedef struct {
    const char       *name;
    char             *buf;
    unsigned          size;
    volatile unsigned wr;
    volatile unsigned rd;
    unsigned          flags;     /* 0: skip when full (non-blocking) */
} rtt_ring_t;

typedef struct {
    char       id[16];
    int        max_up;
    int        max_down;
    rtt_ring_t up[1];
    rtt_ring_t down[1];
} rtt_cb_t;

static char up_buf[RTT_UP_SIZE];
static char down_buf[RTT_DOWN_SIZE];

/* Kept in RAM and found by the probe's scan, so it must not be optimised
 * away and must not be in a section the startup code skips. */
__attribute__((used)) rtt_cb_t _SEGGER_RTT;

void rtt_init(void)
{
    rtt_cb_t *cb = &_SEGGER_RTT;
    memset(cb, 0, sizeof *cb);
    cb->max_up = 1;
    cb->max_down = 1;
    cb->up[0] = (rtt_ring_t){ "Terminal", up_buf, sizeof up_buf, 0, 0, 0 };
    cb->down[0] = (rtt_ring_t){ "Terminal", down_buf, sizeof down_buf, 0, 0, 0 };
    /* Write the ID last, back to front, so a scan never matches a half-built
     * block (SEGGER does the same). */
    static const char id[] = "SEGGER RTT";
    for (int i = (int)sizeof id - 1; i >= 0; i--) cb->id[i] = id[i];
}

size_t rtt_write(const char *s, size_t n)
{
    rtt_ring_t *r = &_SEGGER_RTT.up[0];
    size_t done = 0;
    unsigned wr = r->wr;
    while (done < n) {
        unsigned next = (wr + 1u) % r->size;
        if (next == r->rd) break;               /* full: drop the rest */
        r->buf[wr] = s[done++];
        wr = next;
    }
    __asm volatile ("" ::: "memory");
    r->wr = wr;
    return done;
}

void rtt_puts(const char *s)
{
    rtt_write(s, strlen(s));
}

void rtt_printf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) rtt_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}
