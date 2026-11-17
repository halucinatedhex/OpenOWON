#include "ui.h"
#if !__has_include("stock_fonts.h")
#error "lib/ui/stock_fonts.h is generated from the stock firmware image: run make setup (README.md)"
#endif
#include "stock_fonts.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "label_font.h"

/* Sampled from the manual screenshots (docs/manual/screens/). */
/* Sampled from the manual's screens (docs/ui-development.md): yellow
 * (248,189,0) readings and values, cyan labels, slate bars and boxes,
 * green status, Title Case labels. */
const theme_t theme_stock = { "stock",
    RGB(0,0,0), RGB(248,189,0), RGB(170,180,190), RGB(248,189,0),
    RGB(250,80,40), RGB(255,255,255), RGB(248,189,0),
    RGB(61,82,101), RGB(119,127,150), RGB(20,250,248), RGB(0,255,0),
    RGB(255,255,255), true, 0, 0, 0 };

const theme_t theme_dark = { "dark",
    RGB(12,14,18), RGB(232,238,245), RGB(130,140,152), RGB(80,190,255),
    RGB(250,110,90), RGB(52,60,72), RGB(80,225,180),
    RGB(28,33,42), RGB(60,70,84), RGB(90,200,255), RGB(60,220,120),
    RGB(226,232,240), false, 0, 0, 0 };

const theme_t theme_amber = { "amber",
    RGB(10,7,2), RGB(255,176,32), RGB(150,104,20), RGB(255,214,110),
    RGB(255,90,40), RGB(78,54,10), RGB(255,176,32),
    RGB(40,27,6), RGB(92,64,14), RGB(255,214,110), RGB(210,170,40),
    RGB(255,206,96), false, 0, 0, 0 };

const theme_t theme_paper = { "paper",
    RGB(248,248,245), RGB(18,20,24), RGB(110,116,124), RGB(190,60,20),
    RGB(200,40,30), RGB(205,208,205), RGB(20,90,170),
    RGB(222,226,230), RGB(238,240,242), RGB(20,80,150), RGB(30,150,70),
    RGB(24,28,34), false, 0, 0, 0 };

/* Neon on a night sky (the owner's request, 2026-11-16): hot-pink
 * readings, cyan units and traces, a violet grid, sunset orange for
 * warnings so they stay distinct from the pink, and a sunset-yellow glow
 * on the horizon above the footer. */
const theme_t theme_synthwave = { "synthwave",
    RGB(18,6,36), RGB(255,80,200), RGB(160,128,210), RGB(40,230,255),
    RGB(255,150,40), RGB(96,36,140), RGB(40,230,255),
    RGB(44,14,80), RGB(84,30,128), RGB(40,230,255), RGB(70,255,170),
    RGB(250,226,255), false, RGB(255,206,64), RGB(190,30,120), 72 };

void fb_init(fb_t *f, rgb565_t *backing, int w, int h)
{ fb_init_band(f, backing, w, h, 0, 0); }

void fb_init_band(fb_t *f, rgb565_t *backing, int w, int h, int x0, int y0)
{ f->px = backing; f->w = w; f->h = h; f->x0 = x0; f->y0 = y0; }

void fb_clear(fb_t *f, rgb565_t c)
{ for (int i = 0; i < f->w * f->h; i++) f->px[i] = c; }

/* Coordinates are screen coordinates; anything outside this window is dropped. */
void fb_px(fb_t *f, int x, int y, rgb565_t c)
{
    x -= f->x0; y -= f->y0;
    if (x >= 0 && y >= 0 && x < f->w && y < f->h) f->px[y * f->w + x] = c;
}

/* Clipped up front: a band rejects most of a full-screen rect without
 * touching it pixel by pixel. */
void fb_rect(fb_t *f, int x, int y, int w, int h, rgb565_t c)
{
    int xa = x - f->x0, ya = y - f->y0, xb = xa + w, yb = ya + h;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > f->w) xb = f->w;
    if (yb > f->h) yb = f->h;
    for (int j = ya; j < yb; j++)
        for (int i = xa; i < xb; i++) f->px[j * f->w + i] = c;
}

void fb_frame(fb_t *f, int x, int y, int w, int h, rgb565_t c)
{
    fb_hline(f, x, y, w, c); fb_hline(f, x, y + h - 1, w, c);
    fb_vline(f, x, y, h, c); fb_vline(f, x + w - 1, y, h, c);
}

void fb_hline(fb_t *f, int x, int y, int w, int c)
{ if (w > 0) fb_rect(f, x, y, w, 1, (rgb565_t)c); }

void fb_vline(fb_t *f, int x, int y, int h, int c)
{ if (h > 0) fb_rect(f, x, y, 1, h, (rgb565_t)c); }

/* True if rows [y, y + h) miss this band entirely. The screen is drawn in
 * 16 bands, so text outside the band is skipped before any glyph work. */
static int off_band(const fb_t *f, int y, int h)
{ return y + h <= f->y0 || y >= f->y0 + f->h; }

/* Label text, anti-aliased (lib/ui/label_font.h, made by
 * scripts/make-label-font.py). `scale` picks the size: 1 small, 2 large,
 * 3 the label size (the bold cut, for soft-key labels and captions), 4 and
 * up the unit size. y is the top of the capitals; descenders hang below
 * y + fb_text_h(scale). Strings are UTF-8; ° µ Ω ± render. */
static const lf_font_t *lf_for(int scale)
{
    return scale <= 1 ? &lf_small : scale == 2 ? &lf_large : scale == 3 ? &lf_label : &lf_unit;
}

/* Next code point from UTF-8 (two-byte forms only: all our symbols). */
static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if ((c & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_alpha(char c) { return is_lower(c) || (c >= 'A' && c <= 'Z'); }

/* The next code point as it should look: the measurement layers spell
 * units in ASCII ("kOhm", "uF"; SCPI shares them), the screen shows the
 * symbols. "Ohm" after a prefix or a non-letter -> Ω; "u" as a prefix
 * before A, V, F, s or H -> µ. */
static unsigned disp_cp(const char **s, const char *start)
{
    const char *p = *s;
    char prev = p > start ? p[-1] : ' ';
    int unit_pos = !is_alpha(prev) || prev == 'k' || prev == 'M' || prev == 'G' || prev == 'm';
    if (p[0] == 'O' && p[1] == 'h' && p[2] == 'm' && !is_lower(p[3]) && unit_pos) {
        *s = p + 3;
        return 0x3A9;
    }
    if (p[0] == 'u' && !is_alpha(prev) && p[1] && strchr("AVFsH", p[1]) && !is_lower(p[2]) &&
        !(p[1] == 'H' && p[2] != 'z')) {
        *s = p + 1;
        return 0xB5;
    }
    return next_cp(s);
}

/* Glyphs are sorted by code point. NULL if the font lacks it. */
static const lf_glyph_t *lf_find(const lf_font_t *F, unsigned cp)
{
    int lo = 0, hi = F->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (F->g[mid].cp == cp) return &F->g[mid];
        if (F->g[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static const lf_glyph_t *lf_glyph(const lf_font_t *F, unsigned cp)
{
    const lf_glyph_t *g = lf_find(F, cp);
    return g ? g : lf_find(F, ' ');
}

int fb_text_missing(const char *s, int scale)
{
    const lf_font_t *F = lf_for(scale);
    const char *start = s;
    int n = 0;
    while (*s) n += !lf_find(F, disp_cp(&s, start));
    return n;
}

/* Title Case for themes that want it (stock's labels: "Range", "Auto",
 * "Off"). The app keeps its labels in capitals (tests and SCPI see those);
 * this is how they are drawn. Each run of letters and digits that is all
 * capitals and two letters or more becomes Title Case, unless it is an
 * acronym or a function name stock shows in capitals. Mixed-case words
 * ("dBm", "mV", sentences written in sentence case) are left alone. */
static bool title_case_on;
void ui_set_title_case(bool on) { title_case_on = on; }

static const char *const keep_caps[] = {
    "DC", "AC", "DCV", "ACV", "DCI", "ACI", "OHM", "CJ", "PT100", "PT1000", "RMT", "NUM", "REC",
    "MAN", "4W", "SD", "EMF", "SCPI", "USB", "OK", "LED", "RPM", "UCAL", 0 };

static const char *title_case(const char *s, char *buf, size_t cap)
{
    if (!title_case_on) return s;
    size_t n = 0;
    while (*s && n + 1 < cap) {
        const char *e = s;
        int letters = 0, upper = 1;
        while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9')) {
            if (*e >= 'a' && *e <= 'z') upper = 0;
            if ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z')) letters++;
            e++;
        }
        if (e == s) { buf[n++] = *s++; continue; }
        bool keep = !upper || letters < 2;
        for (int i = 0; !keep && keep_caps[i]; i++)
            keep = strlen(keep_caps[i]) == (size_t)(e - s) && !strncmp(s, keep_caps[i], (size_t)(e - s));
        bool first = true;
        for (const char *p = s; p < e && n + 1 < cap; p++) {
            char ch = *p;
            if (!keep && !first && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) first = false;
            buf[n++] = ch;
        }
        s = e;
    }
    buf[n] = 0;
    return buf;
}

int fb_text_h(int scale) { return lf_for(scale)->cap; }

int fb_text_w(const char *s, int scale)
{
    char tc[96];
    s = title_case(s, tc, sizeof tc);
    const lf_font_t *F = lf_for(scale);
    int w = 0;
    const char *start = s;
    while (*s) w += lf_glyph(F, disp_cp(&s, start))->adv;
    return w;
}

/* Mix c over the band's pixel by coverage a (0-15). */
static void blend(fb_t *f, int x, int y, rgb565_t c, unsigned a)
{
    x -= f->x0; y -= f->y0;
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return;
    rgb565_t *d = &f->px[y * f->w + x];
    if (a >= 15) { *d = c; return; }
    unsigned b = *d;
    unsigned r = ((b >> 11) * (15 - a) + (unsigned)(c >> 11) * a) / 15;
    unsigned g = (((b >> 5) & 63) * (15 - a) + ((c >> 5) & 63u) * a) / 15;
    unsigned bl = ((b & 31) * (15 - a) + (c & 31u) * a) / 15;
    *d = (rgb565_t)(r << 11 | g << 5 | bl);
}

void fb_text(fb_t *f, int x, int y, const char *s, int scale, rgb565_t c)
{
    char tc[96];
    s = title_case(s, tc, sizeof tc);
    const lf_font_t *F = lf_for(scale);
    int base = y + F->cap;
    if (off_band(f, base - F->asc, F->asc + F->desc)) return;
    const char *start = s;
    while (*s) {
        const lf_glyph_t *g = lf_glyph(F, disp_cp(&s, start));
        const uint8_t *bits = F->bits + g->off;
        int gx = x + g->xo, gy = base + g->yo;
        int r0 = f->y0 - gy, r1 = f->y0 + f->h - gy;  /* only the rows in this band */
        if (r0 < 0) r0 = 0;
        if (r1 > g->h) r1 = g->h;
        for (int r = r0; r < r1; r++)
            for (int col = 0, i = r * g->w; col < g->w; col++, i++) {
                unsigned a = (bits[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                if (a) blend(f, gx + col, gy + r, c, a);
            }
        x += g->adv;
    }
}

void trace_init(trace_t *t) { t->n = 0; t->head = 0; t->lo = 0; t->hi = 0; }

void trace_push(trace_t *t, float v)
{
    t->v[t->head] = v;
    t->head = (t->head + 1) % TRACE_MAX;
    if (t->n < TRACE_MAX) t->n++;
}

float trace_at(const trace_t *t, int i)
{
    int start = (t->head - t->n + TRACE_MAX) % TRACE_MAX;
    return t->v[(start + i) % TRACE_MAX];
}

void trace_range(const trace_t *t, float *lo, float *hi)
{
    if (t->n == 0) { *lo = 0; *hi = 1; return; }
    float a = trace_at(t, 0), b = a;
    for (int i = 1; i < t->n; i++) {
        float v = trace_at(t, i);
        if (v < a) a = v;
        if (v > b) b = v;
    }
    if (b - a < 1e-9f) { a -= 0.5f; b += 0.5f; }
    float pad = (b - a) * 0.1f;
    *lo = a - pad; *hi = b + pad;
}


/* Minimal float -> text; avoids pulling printf into firmware builds. */
/* Axis label with enough decimals to tell the two ends of the trace apart:
 * one more than the span needs, 0..6. */
static void snprint_f(char *out, size_t cap, float v, float span)
{
    int dec = 3;
    if (span > 0.0f) {
        dec = (int)ceilf(-log10f(span)) + 1;
        if (dec < 0) dec = 0;
        if (dec > 6) dec = 6;
    }
    snprintf(out, cap, "%.*f", dec, (double)v);
}

/* B47: the battery gauge at the right of the top bar, -1 = none. */
static int bat_pct = -1;
void ui_set_battery(int pct) { bat_pct = pct > 100 ? 100 : pct; }

#define BAT_BODY_W 28
#define BAT_SPACE  (BAT_BODY_W + 3 + 14)        /* body, nub, gap before the unit */

/* Where text at the right of the bar must end. */
static int bar_right(void) { return SCR_W - RAIL_W - 10 - (bat_pct >= 0 ? BAT_SPACE : 0); }

static void battery_icon(fb_t *f, const theme_t *th)
{
    const int bw = BAT_BODY_W, bh = 14;
    const int x = SCR_W - RAIL_W - 10 - 3 - bw, y = BAR_H / 2 - bh / 2;
    rgb565_t c = bat_pct <= 10 ? th->warn : th->chrome_fg;
    fb_frame(f, x, y, bw, bh, c);
    fb_rect(f, x + bw, y + 4, 3, bh - 8, c);                  /* the nub */
    int fill = (bw - 4) * bat_pct / 100;
    if (bat_pct > 0 && fill < 2) fill = 2;                    /* never looks dead while it isn't */
    if (fill > 0) fb_rect(f, x + 2, y + 2, fill, bh - 4, bat_pct <= 10 ? th->warn : th->ok);
}

static void chrome(fb_t *f, const theme_t *th, const char *status)
{
    title_case_on = th->title_case;               /* every screen starts here */
    fb_rect(f, 0, 0, SCR_W, BAR_H, th->bar);
    if (status) {
        fb_rect(f, 10, BAR_H / 2 - 4, 8, 8, th->ok);          /* status dot */
        fb_text(f, 24, BAR_H / 2 - 6, status, 2, th->ok);
    }
    if (bat_pct >= 0) battery_icon(f, th);
}

/* The soft-key column: one framed panel per physical key (F1-F5, top to
 * bottom), as stock draws it, so each label and value plainly belongs to
 * the button beside it. Empty keys keep their frame. */
static void rail(fb_t *f, const theme_t *th, const softkey_t *k, int n)
{
    const int x = SCR_W - RAIL_W, slot = SCR_H / UI_NKEYS;
    fb_rect(f, x, 0, RAIL_W, SCR_H, th->bg);                  /* the gaps between panels */
    for (int i = 0; i < UI_NKEYS; i++) {
        const int bx = x + 2, by = i * slot + 2, bw = RAIL_W - 4, bh = slot - 4;
        fb_rect(f, bx, by, bw, bh, th->bar);
        fb_frame(f, bx, by, bw, bh, th->panel);
        if (i >= n || (!k[i].label && !k[i].value)) continue;
        if (!k[i].label) {                                     /* mode chip */
            fb_rect(f, bx + 2, by + 2, bw - 4, bh - 4, th->accent);
            fb_text(f, x + (RAIL_W - fb_text_w(k[i].value, 2)) / 2,
                    by + bh / 2 - fb_text_h(2) / 2, k[i].value, 2, th->bg);
            continue;
        }
        /* The label size (bold where the font has a bold cut), small only
         * when it would not fit across the panel. A key with no value has
         * its label centred. */
        int ls = fb_text_w(k[i].label, 3) <= bw - 6 ? 3 : 1;
        int ly = k[i].value ? by + 5 : by + bh / 2 - fb_text_h(ls) / 2;
        fb_text(f, x + (RAIL_W - fb_text_w(k[i].label, ls)) / 2, ly, k[i].label, ls,
                k[i].off ? th->dim : th->label);
        if (k[i].value) {
            const int vx = bx + 4, vy = by + 23, vw = bw - 8, vh = bh - 27;
            int vs = fb_text_w(k[i].value, 2) <= vw - 4 ? 2 : 1;
            if (!k[i].off) fb_rect(f, vx, vy, vw, vh, th->panel);   /* greyed: no value panel */
            fb_text(f, x + (RAIL_W - fb_text_w(k[i].value, vs)) / 2,
                    vy + vh / 2 - fb_text_h(vs) / 2, k[i].value, vs,
                    k[i].off ? th->dim : th->chrome_fg);
        }
    }
}

static void footer(fb_t *f, const theme_t *th, const char *rv, const char *fv)
{
    const int y = SCR_H - FOOT_H, w = SCR_W - RAIL_W, bw = w / 2 - 12;
    /* Values large where they fit the box; captions in the label size, as
     * stock (seen too small on the real panel). */
    int rs = fb_text_w(rv, 2) <= bw - 8 ? 2 : 1, fs = fb_text_w(fv, 2) <= bw - 8 ? 2 : 1;
    fb_rect(f, 0, y, w, FOOT_H, th->bar);
    int cy = y + 10 - fb_text_h(3) / 2;
    fb_text(f, (w / 2 - fb_text_w("RANGE", 3)) / 2, cy, "RANGE", 3, th->chrome_fg);
    fb_text(f, w / 2 + (w / 2 - fb_text_w("FUNCTION", 3)) / 2, cy, "FUNCTION", 3, th->chrome_fg);
    fb_rect(f, 6, y + 20, bw, FOOT_H - 26, th->panel);
    fb_rect(f, w / 2 + 6, y + 20, bw, FOOT_H - 26, th->panel);
    fb_text(f, 6 + (bw - fb_text_w(rv, rs)) / 2, y + 20 + (FOOT_H - 26) / 2 - fb_text_h(rs) / 2, rv, rs, th->accent);
    fb_text(f, w / 2 + 6 + (bw - fb_text_w(fv, fs)) / 2, y + 20 + (FOOT_H - 26) / 2 - fb_text_h(fs) / 2, fv, fs, th->accent);
}


/* ---- stock firmware font ------------------------------------------------ */

/* The stock font's 'O' is wider than its 56-column cell, so the stored
 * bitmap loses its right-hand curve (scripts/extract-fonts.py --show 4).
 * The letter is symmetric about column 33.5 (the top arc spans 28..39):
 * draw its left half, then the same half mirrored. */
#define STOCK_O_MIRROR   67      /* column c pairs with 67 - c */
#define STOCK_O_WIDTH    60      /* ink 4..63 */

static int stock_glyph_w(const stock_font_t *F, char ch, int gi)
{
    return ch == 'O' ? STOCK_O_WIDTH : F->met[gi].width;
}

static int stock_index(char ch)
{
    for (int i = 0; i < STOCK_NGLYPH; i++)
        if (STOCK_CHARSET[i] == ch) return i;
    return -1;
}

int fb_stock_h(int den)
{
    if (den < 1) den = 1;
    return stock_fonts[0].rows / den;
}

int fb_stock_w(const char *s, int gap, int den)
{
    if (den < 1) den = 1;
    const stock_font_t *F = &stock_fonts[0];
    int w = 0, n = 0;
    for (; *s; s++) {
        int gi = stock_index(*s);
        if (gi < 0) continue;
        w += stock_glyph_w(F, *s, gi) + gap;
        n++;
    }
    if (n) w -= gap;                      /* no trailing gap */
    return w / den;
}

void fb_stock_text(fb_t *f, int x, int y, const char *s, int gap, int den, rgb565_t c)
{
    if (den < 1) den = 1;
    const stock_font_t *F = &stock_fonts[0];
    if (off_band(f, y, (F->rows + den - 1) / den)) return;
    for (; *s; s++) {
        int gi = stock_index(*s);
        if (gi < 0) continue;
        const uint8_t *bits = F->bits + (size_t)gi * F->cols * F->colbytes;
        int left = F->met[gi].left, wide = stock_glyph_w(F, *s, gi);
        for (int col = 0; col < wide; col++) {
            int sx = left + col;
            if (*s == 'O' && sx > STOCK_O_MIRROR / 2) sx = STOCK_O_MIRROR - sx;
            for (int row = 0; row < F->rows; row++) {
                /* column-major, MSB of each byte is the topmost pixel */
                if (bits[sx * F->colbytes + (row >> 3)] & (0x80u >> (row & 7)))
                    fb_px(f, x + col / den, y + row / den, c);
            }
        }
        x += (wide + gap) / den;
    }
}

void ui_bar(fb_t *f, const theme_t *th, int x, int y, int w, int h, float frac, bool bipolar)
{
    if (off_band(f, y, h + 6)) return;
    if (frac > 1.1f) frac = 1.1f;
    if (frac < -1.1f) frac = -1.1f;
    if (!bipolar && frac < 0) frac = -frac;
    rgb565_t c = (frac > 1.0f || frac < -1.0f) ? th->warn : th->accent;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < -1.0f) frac = -1.0f;
    int zero = bipolar ? x + w / 2 : x, span = bipolar ? w / 2 : w;
    int len = (int)(frac * (float)span + (frac >= 0 ? 0.5f : -0.5f));
    if (len > 0) fb_rect(f, zero, y, len, h, c);
    else if (len < 0) fb_rect(f, zero + len, y, -len, h, c);
    for (int i = 0; i <= 10; i++) {               /* ticks below the bar */
        int tx = bipolar ? x + w / 2 + (i - 5) * (w / 10) : x + i * (w / 10);
        if (i == 10) tx = x + w - 1;
        int th_ = (i % 5 == 0) ? 6 : 3;
        fb_rect(f, tx, y + h + 1, 1, th_, th->grid);
    }
    fb_rect(f, x, y + h, w, 1, th->grid);         /* baseline */
}

/* The theme's horizon glow: over the glow_h rows above the footer the
 * background warms through th->dusk to th->horizon, a sunset. It eases in
 * (a square), so the readings above keep the plain background, and is
 * drawn row by row, so it bands like anything else. */
static int mix8(int a, int b, int t256) { return a + (b - a) * t256 / 256; }
static rgb565_t mix565(rgb565_t a, rgb565_t b, int t256)
{
    return RGB(mix8((a >> 8) & 0xF8, (b >> 8) & 0xF8, t256),
               mix8((a >> 3) & 0xFC, (b >> 3) & 0xFC, t256),
               mix8((a << 3) & 0xF8, (b << 3) & 0xF8, t256));
}

static void horizon(fb_t *f, const theme_t *th, int w)
{
    if (!th->horizon || !th->glow_h) return;
    const int gh = th->glow_h, y1 = SCR_H - FOOT_H, y0 = y1 - gh;
    for (int y = y0; y < y1; y++) {
        int t = (y - y0 + 1) * 256 / gh;                       /* 1/gh .. 1 */
        t = t * t / 256;
        rgb565_t c = t < 128 ? mix565(th->bg, th->dusk, t * 2)
                             : mix565(th->dusk, th->horizon, (t - 128) * 2);
        fb_rect(f, 0, y, w, 1, c);
    }
}

void screen_measure(fb_t *f, const theme_t *th, const char *reading,
                    const char *unit, const char *mode,
                    const softkey_t *keys, int nkeys,
                    const char *range_val, const char *func_val)
{
    const int w = SCR_W - RAIL_W;
    fb_clear(f, th->bg);
    fb_rect(f, 0, BAR_H, w, SCR_H - BAR_H - FOOT_H, th->bg);
    horizon(f, th, w);
    chrome(f, th, mode);
    fb_stock_text(f, w - 12 - fb_stock_w(reading, 6, 1), BAR_H + 14,
                  reading, 6, 1, th->fg);
    fb_text(f, w - 12 - fb_text_w(unit, 4), BAR_H + 120, unit, 4, th->accent);
    footer(f, th, range_val, func_val);
    rail(f, th, keys, nkeys);
}

void screen_plot(fb_t *f, const theme_t *th, const trace_t *t,
                 const char *title, const char *unit,
                 const softkey_t *keys, int nkeys, const float *fixed)
{
    screen_plot_ex(f, th, t, title, unit, keys, nkeys, fixed, -1);
}

/* Histogram of the trace (B38): HIST_BINS bins across the trace's range,
 * bar heights scaled to the fullest bin, the mean marked, and the mean and
 * standard deviation printed. */
#define HIST_BINS 32

/* Bin the trace into nb bins across its range. Returns the fullest bin's
 * count (0 if fewer than two points). */
static int hist_bins(const trace_t *t, int *bin, int nb, float *lo, float *hi,
                     double *mean, double *sd)
{
    for (int b = 0; b < nb; b++) bin[b] = 0;
    if (t->n < 2) return 0;
    trace_range(t, lo, hi);
    int most = 0;
    double sum = 0, sq = 0;
    for (int i = 0; i < t->n; i++) {
        float v = trace_at(t, i);
        int b = (int)((v - *lo) / (*hi - *lo) * nb);
        if (b < 0) b = 0;
        if (b >= nb) b = nb - 1;
        if (++bin[b] > most) most = bin[b];
        sum += v;
        sq += (double)v * v;
    }
    *mean = sum / t->n;
    double var = sq / t->n - *mean * *mean;
    *sd = var > 0 ? sqrt(var) : 0;
    return most;
}

void ui_mini_trend(fb_t *f, const theme_t *th, const trace_t *t, int x, int y, int w, int h)
{
    fb_hline(f, x, y + h, w, th->grid);                       /* a baseline, no box */
    if (t->n < 2) return;
    float lo, hi;
    trace_range(t, &lo, &hi);
    int px0 = 0, py0 = 0;
    for (int i = 0; i < t->n; i++) {
        float v = trace_at(t, i);
        int px = x + (w - 1) * i / (t->n - 1);
        int py = y + h - 1 - (int)((v - lo) / (hi - lo) * (float)(h - 2));
        if (py < y) py = y;
        if (py > y + h - 1) py = y + h - 1;
        if (i) {
            int dx = px - px0, dy = py - py0;
            int steps = (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy) ? (dx > 0 ? dx : -dx) : (dy > 0 ? dy : -dy);
            if (!steps) steps = 1;
            for (int k = 0; k <= steps; k++) fb_px(f, px0 + dx * k / steps, py0 + dy * k / steps, th->trace);
        }
        px0 = px; py0 = py;
    }
}

void ui_mini_hist(fb_t *f, const theme_t *th, const trace_t *t, int x, int y, int w, int h)
{
    fb_hline(f, x, y + h, w, th->grid);
    int bin[HIST_BINS];
    float lo = 0, hi = 1;
    double mean = 0, sd = 0;
    int most = hist_bins(t, bin, HIST_BINS, &lo, &hi, &mean, &sd);
    if (!most) return;
    int bw = w / HIST_BINS;
    for (int b = 0; b < HIST_BINS; b++) {
        int bh = (h - 1) * bin[b] / most;
        if (bh) fb_rect(f, x + b * bw, y + h - bh, bw - 1, bh, th->trace);
    }
    int mx = x + (int)((mean - lo) / (hi - lo) * (double)(bw * HIST_BINS));
    fb_vline(f, mx, y, h, th->accent);                        /* the mean */
}

void screen_hist(fb_t *f, const theme_t *th, const trace_t *t,
                 const char *title, const char *unit, const softkey_t *keys, int nkeys)
{
    const int w = SCR_W - RAIL_W;
    fb_clear(f, th->bg);
    chrome(f, th, title);
    fb_text(f, bar_right() - fb_text_w(unit, 2), BAR_H / 2 - 6, unit, 2, th->accent);
    const int gx = 8, gy = BAR_H + 8, gw = w - 16, gh = SCR_H - BAR_H - 16;
    fb_frame(f, gx, gy, gw, gh, th->grid);
    int bin[HIST_BINS];
    float lo = 0, hi = 1;
    double mean = 0, sd = 0;
    int most = hist_bins(t, bin, HIST_BINS, &lo, &hi, &mean, &sd);
    if (most) {
        const int top = gy + 24, bot = gy + gh - 16, bw = (gw - 4) / HIST_BINS;
        for (int b = 0; b < HIST_BINS; b++) {
            int h = most ? (bot - top) * bin[b] / most : 0;
            if (h) fb_rect(f, gx + 2 + b * bw, bot - h, bw - 1, h, th->trace);
        }
        int mx = gx + 2 + (int)((mean - lo) / (hi - lo) * (double)(bw * HIST_BINS));
        fb_vline(f, mx, top - 4, bot - top + 4, th->accent);   /* the mean */
        char buf[16], line[48];
        snprint_f(buf, sizeof buf, lo, hi - lo);
        fb_text(f, gx + 4, gy + gh - 12, buf, 1, th->dim);
        snprint_f(buf, sizeof buf, hi, hi - lo);
        fb_text(f, gx + gw - 4 - fb_text_w(buf, 1), gy + gh - 12, buf, 1, th->dim);
        char m[16], sdv[16];
        snprint_f(m, sizeof m, (float)mean, hi - lo);
        snprint_f(sdv, sizeof sdv, (float)sd, hi - lo);
        snprintf(line, sizeof line, "MEAN %s   SD %s   N %d", m, sdv, t->n);
        fb_text(f, gx + 6, gy + 6, line, 1, th->fg);
    }
    rail(f, th, keys, nkeys);
}

void screen_plot_ex(fb_t *f, const theme_t *th, const trace_t *t,
                    const char *title, const char *unit,
                    const softkey_t *keys, int nkeys, const float *fixed, int cursor)
{
    const int w = SCR_W - RAIL_W;
    fb_clear(f, th->bg);
    chrome(f, th, title);
    fb_text(f, bar_right() - fb_text_w(unit, 2), BAR_H / 2 - 6, unit, 2, th->accent);

    const int gx = 8, gy = BAR_H + 8, gw = w - 16, gh = SCR_H - BAR_H - 16;
    fb_frame(f, gx, gy, gw, gh, th->grid);
    for (int i = 1; i < 4; i++) fb_hline(f, gx + 1, gy + gh * i / 4, gw - 2, th->grid);
    for (int i = 1; i < 6; i++) fb_vline(f, gx + gw * i / 6, gy + 1, gh - 2, th->grid);

    float lo, hi;
    if (fixed && fixed[1] > fixed[0]) { lo = fixed[0]; hi = fixed[1]; }
    else trace_range(t, &lo, &hi);
    if (t->n >= 2) {
        const int top = gy + 1, bot = gy + gh - 2;
        int px0 = 0, py0 = 0;
        for (int i = 0; i < t->n; i++) {
            float v = trace_at(t, i);
            int px = gx + 1 + (gw - 3) * i / (t->n - 1);
            float fy = (float)(gy + gh - 2) - (v - lo) / (hi - lo) * (float)(gh - 4);
            int py = fy < top ? top : fy > bot ? bot : (int)fy;      /* clip to the box */
            if (i) {
                int dx = px - px0, dy = py - py0;
                int ax = dx > 0 ? dx : -dx, ay = dy > 0 ? dy : -dy;
                int steps = ax > ay ? ax : ay;
                if (!steps) steps = 1;
                for (int k = 0; k <= steps; k++)
                    fb_px(f, px0 + dx * k / steps, py0 + dy * k / steps, th->trace);
            }
            px0 = px; py0 = py;
        }
    }
    if (cursor >= 0 && t->n >= 2) {                           /* B36: browse cursor */
        int c = cursor < t->n ? cursor : t->n - 1;
        fb_vline(f, gx + 1 + (gw - 3) * c / (t->n - 1), gy + 1, gh - 2, th->accent);
    }
    if (t->n >= 2 || fixed) {
        char buf[16];
        snprint_f(buf, sizeof buf, hi, hi - lo);
        fb_text(f, gx + 4, gy + 3, buf, 1, th->dim);
        snprint_f(buf, sizeof buf, lo, hi - lo);
        fb_text(f, gx + 4, gy + gh - 11, buf, 1, th->dim);
    }
    rail(f, th, keys, nkeys);
}

int screen_menu_rows(void) { return (SCR_H - BAR_H - 12) / 30; }

void screen_menu(fb_t *f, const theme_t *th, const char *title,
                 const char *const *items, const char *const *vals, int n, int sel,
                 const softkey_t *keys, int nkeys)
{
    screen_menu_ex(f, th, title, items, vals, n, sel, keys, nkeys, NULL);
}

void screen_menu_ex(fb_t *f, const theme_t *th, const char *title,
                    const char *const *items, const char *const *vals, int n, int sel,
                    const softkey_t *keys, int nkeys, const bool *divider_before)
{
    const int w = SCR_W - RAIL_W, rows = screen_menu_rows();
    fb_clear(f, th->bg);
    chrome(f, th, 0);
    fb_text(f, 12, BAR_H / 2 - 6, title, 2, th->chrome_fg);

    /* Keep the selection on screen, centred where possible. */
    int first = sel - rows / 2;
    if (first > n - rows) first = n - rows;
    if (first < 0) first = 0;
    for (int i = first; i < n && i < first + rows; i++) {
        int y = BAR_H + 12 + (i - first) * 30;
        const char *v = vals[i] ? vals[i] : "";
        if (divider_before && divider_before[i] && i > first)      /* between groups */
            fb_rect(f, 12, y - 8, w - 30, 1, th->grid);
        if (i == sel) {
            fb_rect(f, 6, y - 6, w - 18, 26, th->accent);
            fb_text(f, 16, y, items[i], 2, th->bg);
            fb_text(f, w - 22 - fb_text_w(v, 2), y, v, 2, th->bg);
        } else {
            fb_text(f, 16, y, items[i], 2, th->fg);
            fb_text(f, w - 22 - fb_text_w(v, 2), y, v, 2, th->dim);
        }
    }
    if (n > rows) {                                           /* scroll bar */
        int track = rows * 30, y0 = BAR_H + 6;
        fb_rect(f, w - 8, y0, 3, track, th->grid);
        fb_rect(f, w - 8, y0 + track * first / n, 3, track * rows / n, th->dim);
    }
    rail(f, th, keys, nkeys);
}
