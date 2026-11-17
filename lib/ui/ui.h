/*
 * Portable UI layer for the XDM1241.
 *
 * Deliberately knows nothing about the GD32, SPI, or the panel. It renders
 * into a plain RGB565 framebuffer, so exactly the same code runs:
 *   - on the host, rendered to a BMP for inspection and golden tests
 *   - on the meter, by blitting the framebuffer out over SPI
 *
 * That is what lets themes, the plot screen and the config screen be built
 * and reviewed before any hardware exists.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef uint16_t rgb565_t;

#define RGB(r, g, b) ((rgb565_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* A framebuffer is a window onto the 480x320 screen: `px` holds w x h pixels
 * whose top-left is screen pixel (x0, y0). A full-screen buffer has x0 = y0 = 0.
 * The meter has 48 KB of SRAM and a full screen is 300 KB, so on target the
 * screen is drawn in horizontal bands: the same drawing calls, clipped to
 * each band in turn (see fb_init_band). */
typedef struct {
    rgb565_t *px;
    int w, h;
    int x0, y0;
} fb_t;

/* A theme is just colours. Swapping one re-skins every screen, because no
 * drawing code hard-codes a colour. */
typedef struct {
    const char *name;
    rgb565_t bg;      /* main measurement area */
    rgb565_t fg;      /* primary reading */
    rgb565_t dim;     /* secondary text */
    rgb565_t accent;  /* units, highlighted values */
    rgb565_t warn;
    rgb565_t grid;    /* graticule, separators */
    rgb565_t trace;
    rgb565_t bar;     /* top and bottom chrome */
    rgb565_t panel;   /* softkey value buttons */
    rgb565_t label;   /* softkey labels */
    rgb565_t ok;      /* status dot */
    rgb565_t chrome_fg; /* text on chrome */
    bool     title_case; /* draw all-capital labels in Title Case, as stock */
    rgb565_t horizon; /* a glow rising from the footer behind the reading
                       * area, this colour at the footer; 0: none */
    rgb565_t dusk;    /* the glow passes through this colour on the way */
    uint8_t  glow_h;  /* rows of glow above the footer */
} theme_t;

/* Sampled from the manual's screenshots - matches the shipping UI. */
extern const theme_t theme_stock;
extern const theme_t theme_dark;
extern const theme_t theme_amber;
extern const theme_t theme_paper;     /* high-contrast light */
extern const theme_t theme_synthwave; /* neon pink and cyan on violet */

/* framebuffer */
void fb_init(fb_t *f, rgb565_t *backing, int w, int h);
void fb_init_band(fb_t *f, rgb565_t *backing, int w, int h, int x0, int y0);
void fb_clear(fb_t *f, rgb565_t c);
void fb_px(fb_t *f, int x, int y, rgb565_t c);
void fb_rect(fb_t *f, int x, int y, int w, int h, rgb565_t c);
void fb_frame(fb_t *f, int x, int y, int w, int h, rgb565_t c);
void fb_hline(fb_t *f, int x, int y, int w, int c);
void fb_vline(fb_t *f, int x, int y, int h, int c);

/* Stock bitmap font lifted from the firmware (lib/ui/stock_fonts.h).
 * Proportionally spaced from measured ink extents; `gap` is the inter-glyph
 * space in source pixels. Only the 23 stock glyphs render; anything else is
 * skipped. den>1 downscales by nearest-neighbour (den=2 -> half size). */
int  fb_stock_w(const char *s, int gap, int den);
void fb_stock_text(fb_t *f, int x, int y, const char *s, int gap, int den, rgb565_t c);
int  fb_stock_h(int den);

/* Label text, anti-aliased DejaVu Sans (lib/ui/label_font.h). scale picks
 * the size: 1 small, 2 large, 3 label (bold), 4 the unit under the reading. y is the top of
 * the capitals; fb_text_h is the cap height, for centring. UTF-8. */
void fb_text(fb_t *f, int x, int y, const char *s, int scale, rgb565_t c);
int  fb_text_w(const char *s, int scale);
int  fb_text_h(int scale);
int  fb_text_missing(const char *s, int scale);
/* Draw all-capital words in Title Case (set from theme_t.title_case by every
 * screen; exposed for tests and tools). */
void ui_set_title_case(bool on);
void ui_set_battery(int pct);      /* B47: 0-100 draws a gauge in the top bar, -1 none */   /* characters the size lacks (drawn as spaces) */

/* rolling trace for the plot screen */
#define TRACE_MAX 256
typedef struct {
    float v[TRACE_MAX];
    int   n, head;
    float lo, hi;
} trace_t;

void  trace_init(trace_t *t);
void  trace_push(trace_t *t, float v);
float trace_at(const trace_t *t, int i);   /* 0 = oldest */
void  trace_range(const trace_t *t, float *lo, float *hi);

/* screens */
/* Panel geometry: 3.7" TFT, 480x320 (per the user manual). */
#define SCR_W 480
#define SCR_H 320
#define BAR_H  34
#define RAIL_W 92
#define FOOT_H 64
/* Softkeys beside the screen: our panel has five (Screen1-5, docs/hardware.md). */
#define UI_NKEYS 5

/* off: the key does nothing just now (e.g. settings while recording): drawn
 * greyed, in th->dim. */
typedef struct { const char *label, *value; bool off; } softkey_t;

/* Bar graph under the reading (B37): frac is the reading over the range's
 * full scale (clamped to +-1.1). Bipolar puts zero in the middle. Ticks
 * every 10% (longer at 50%); past full scale the bar turns th->warn. */
void ui_bar(fb_t *f, const theme_t *th, int x, int y, int w, int h, float frac, bool bipolar);

void screen_measure(fb_t *f, const theme_t *th, const char *reading,
                    const char *unit, const char *mode,
                    const softkey_t *keys, int nkeys,
                    const char *range_val, const char *func_val);
/* Trend plot. `title` goes in the top bar (e.g. the live reading); `fixed`
 * is {lo, hi} for a fixed scale, or NULL to fit the trace. */
void screen_plot(fb_t *f, const theme_t *th, const trace_t *t,
                 const char *title, const char *unit,
                 const softkey_t *keys, int nkeys, const float *fixed);
/* As screen_plot, with a vertical cursor at trace point `cursor` (-1: none). */
/* Histogram of a trace, with mean and standard deviation (B38). */
/* B58: the trend, or a histogram of it, small, under the big reading. */
void ui_mini_trend(fb_t *f, const theme_t *th, const trace_t *t, int x, int y, int w, int h);
void ui_mini_hist(fb_t *f, const theme_t *th, const trace_t *t, int x, int y, int w, int h);
void screen_hist(fb_t *f, const theme_t *th, const trace_t *t,
                 const char *title, const char *unit, const softkey_t *keys, int nkeys);
void screen_plot_ex(fb_t *f, const theme_t *th, const trace_t *t,
                    const char *title, const char *unit,
                    const softkey_t *keys, int nkeys, const float *fixed, int cursor);
/* A scrolling list of `label ... value` rows with one selected. */
void screen_menu(fb_t *f, const theme_t *th, const char *title,
                 const char *const *items, const char *const *vals, int n, int sel,
                 const softkey_t *keys, int nkeys);
/* As screen_menu, with a thin rule above each row whose divider_before[]
 * is true (the menu's groups). */
void screen_menu_ex(fb_t *f, const theme_t *th, const char *title,
                    const char *const *items, const char *const *vals, int n, int sel,
                    const softkey_t *keys, int nkeys, const bool *divider_before);
int  screen_menu_rows(void);     /* rows visible at once */

#endif
