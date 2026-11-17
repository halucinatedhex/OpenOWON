/* HOST ONLY. Panel model. See sim_lcd.h. */
#include "sim_lcd.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rgb565_t panel[SCR_W * SCR_H];
static bool     dc_data;
static uint8_t  cmd;
static uint8_t  arg[4];
static int      nargs;
static int      cx0, cx1, ry0, ry1, x, y;
static bool     have_hi;
static uint8_t  hi;
static uint32_t nbytes, npix;

static const uint8_t dcs_map[LCD_DCS_MAP_LEN] = LCD_DCS_MAP;

static void bus_dc(bool data) { dc_data = data; }

static void bus_wr(uint8_t b)
{
    nbytes++;
    if (!dc_data) {                       /* command */
        cmd = b; nargs = 0; have_hi = false;
        if (cmd == 0x2C) { x = cx0; y = ry0; }
        return;
    }
    switch (cmd) {
    case 0x2A: case 0x2B:                 /* CASET / RASET: 4 bytes, big-endian */
        if (nargs < 4) arg[nargs++] = b;
        if (nargs == 4) {
            int a0 = arg[0] << 8 | arg[1], a1 = arg[2] << 8 | arg[3];
            if (cmd == 0x2A) { cx0 = a0; cx1 = a1; } else { ry0 = a0; ry1 = a1; }
        }
        break;
    case 0x2C:                            /* RAMWR: RGB565, high byte first */
        if (!have_hi) { hi = b; have_hi = true; break; }
        have_hi = false;
        if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
            panel[y * SCR_W + x] = (rgb565_t)(hi << 8 | b);
        npix++;
        if (++x > cx1) { x = cx0; if (++y > ry1) y = ry0; }
        break;
    default:
        break;
    }
}

const lcd_io_t sim_lcd_io = {
    .wr_byte = bus_wr, .dc = bus_dc, .cmd_map = dcs_map, .cmd_map_len = LCD_DCS_MAP_LEN,
};

void sim_lcd_reset(void)
{
    memset(panel, 0, sizeof panel);
    dc_data = false; cmd = 0; nargs = 0; have_hi = false;
    cx0 = 0; cx1 = SCR_W - 1; ry0 = 0; ry1 = SCR_H - 1; x = y = 0;
    nbytes = npix = 0;
}

const rgb565_t *sim_lcd_pixels(void) { return panel; }
uint32_t sim_lcd_bytes(void) { return nbytes; }
uint32_t sim_lcd_pixels_written(void) { return npix; }

int sim_lcd_save_bmp(const char *path)
{
    int row = (SCR_W * 3 + 3) & ~3, img = row * SCR_H, size = 54 + img;
    unsigned char h[54] = { 'B', 'M' };
    h[2] = (unsigned char)size; h[3] = (unsigned char)(size >> 8); h[4] = (unsigned char)(size >> 16);
    h[10] = 54; h[14] = 40;
    h[18] = SCR_W & 0xFF; h[19] = SCR_W >> 8; h[22] = SCR_H & 0xFF; h[23] = SCR_H >> 8;
    h[26] = 1; h[28] = 24;
    h[34] = (unsigned char)img; h[35] = (unsigned char)(img >> 8); h[36] = (unsigned char)(img >> 16);
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(h, 1, 54, fp);
    unsigned char *line = calloc(1, (size_t)row);
    for (int yy = SCR_H - 1; yy >= 0; yy--) {
        for (int xx = 0; xx < SCR_W; xx++) {
            rgb565_t c = panel[yy * SCR_W + xx];
            line[xx * 3 + 0] = (unsigned char)((c & 0x1F) << 3);
            line[xx * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) << 2);
            line[xx * 3 + 2] = (unsigned char)(((c >> 11) & 0x1F) << 3);
        }
        fwrite(line, 1, (size_t)row, fp);
    }
    free(line);
    fclose(fp);
    return 0;
}
