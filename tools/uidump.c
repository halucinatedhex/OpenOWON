/* Host-only: render every screen in every theme to BMP so you can look at
 * design changes without a meter. Build with tools/Makefile. */
#include "../lib/ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W SCR_W
#define H SCR_H

static void write_bmp(const char *path, const fb_t *f)
{
    int row = (f->w * 3 + 3) & ~3, img = row * f->h, size = 54 + img;
    unsigned char hdr[54] = {0};
    memcpy(hdr, "BM", 2);
    hdr[2] = size; hdr[3] = size >> 8; hdr[4] = size >> 16; hdr[5] = size >> 24;
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = f->w; hdr[19] = f->w >> 8;
    hdr[22] = f->h; hdr[23] = f->h >> 8;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = img; hdr[35] = img >> 8; hdr[36] = img >> 16;
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); exit(1); }
    fwrite(hdr, 1, 54, fp);
    unsigned char *line = calloc(1, row);
    for (int y = f->h - 1; y >= 0; y--) {            /* BMP is bottom-up */
        for (int x = 0; x < f->w; x++) {
            rgb565_t c = f->px[y * f->w + x];
            line[x * 3 + 0] = (unsigned char)((c & 0x1F) << 3);
            line[x * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) << 2);
            line[x * 3 + 2] = (unsigned char)(((c >> 11) & 0x1F) << 3);
        }
        fwrite(line, 1, row, fp);
    }
    free(line); fclose(fp);
    printf("  %s\n", path);
}

int main(void)
{
    static rgb565_t backing[W * H];
    fb_t f; fb_init(&f, backing, W, H);

    const theme_t *themes[] = { &theme_stock, &theme_dark, &theme_amber, &theme_paper, &theme_synthwave };

    trace_t t; trace_init(&t);
    for (int i = 0; i < 180; i++)                     /* a plausible signal */
        trace_push(&t, 4.98f + 0.04f * sinf(i / 9.0f) + 0.012f * sinf(i / 2.3f));

    const char *items[] = { "BACKLIGHT", "DIM AFTER", "BEEPER", "CONT THRESHOLD", "SPEED",
                            "TEMP UNIT", "DBM REF", "THEME", "BAUD", "LOAD DEFAULTS" };
    const char *vals[]  = { "80%", "5 MIN", "ON", "50 OHM", "MID", "C", "600 OHM", "DARK",
                            "115200", "" };
    static const softkey_t mk[] = {
        { "ITEM", "UP" }, { "ITEM", "DOWN" }, { "VALUE", "-" }, { "VALUE", "+" }, { 0, 0 }, { "EXIT", 0 },
    };

    for (unsigned i = 0; i < sizeof themes / sizeof themes[0]; i++) {
        const theme_t *th = themes[i];
        char p[128];
        static const softkey_t keys[] = {
            { 0, "ACV" }, { "RANGE", "AUTO" }, { "FUNCTION", "V" },
            { "SPEED", "LOW" }, { 0, 0 }, { "REL", "OFF" },
        };
        screen_measure(&f, th, "0.1556", "V AC", "TRIGGER", keys, 6, "AUTO 5 V", "ACV");
        snprintf(p, sizeof p, "build/ui-%s-measure.bmp", th->name); write_bmp(p, &f);
        {
            static const softkey_t pk[] = {
                { "SPAN", "51 S" }, { "SCALE", "AUTO" }, { "VIEW", "GRAPH" },
                { "CLEAR", 0 }, { "PAUSE", "OFF" }, { "THEME", 0 },
            };
            screen_plot(&f, th, &t, "4.9987", "V DC", pk, 6, NULL);
        }
        snprintf(p, sizeof p, "build/ui-%s-plot.bmp", th->name);    write_bmp(p, &f);
        screen_menu(&f, th, "UTILITY", items, vals, 10, 1, mk, 6);
        snprintf(p, sizeof p, "build/ui-%s-menu.bmp", th->name);  write_bmp(p, &f);
    }
    return 0;
}
