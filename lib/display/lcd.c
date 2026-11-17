#include "lcd.h"

static const lcd_io_t *IO;

void lcd_init(const lcd_io_t *io) { IO = io; }

void lcd_cmd(uint8_t index)
{
    if (!IO) return;
    uint8_t op = index;
    if (IO->cmd_map && index < IO->cmd_map_len) op = IO->cmd_map[index];
    IO->dc(false);
    IO->wr_byte(op);
}

void lcd_data(uint8_t b)
{
    if (!IO) return;
    IO->dc(true);
    IO->wr_byte(b);
}

/* Stock order: column window, row window, then begin-write. The coordinate
 * pairs go out as big-endian 16-bit values. */
void lcd_set_window(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1)
{
    lcd_cmd(LCD_CMD_COLUMN);
    lcd_data((uint8_t)(x0 >> 8)); lcd_data((uint8_t)x0);
    lcd_data((uint8_t)(x1 >> 8)); lcd_data((uint8_t)x1);
    lcd_cmd(LCD_CMD_ROW);
    lcd_data((uint8_t)(y0 >> 8)); lcd_data((uint8_t)y0);
    lcd_data((uint8_t)(y1 >> 8)); lcd_data((uint8_t)y1);
    lcd_cmd(LCD_CMD_WRITE);
}

/* After the write command the bus stays in data mode, so the stock firmware
 * streams pixels with bare byte writes rather than re-asserting D/C. */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    if (!IO || !w || !h) return;
    lcd_set_window(x, (uint16_t)(x + w - 1), y, (uint16_t)(y + h - 1));
    IO->dc(true);
    uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)c;
    for (uint32_t n = (uint32_t)w * h; n; n--) {
        IO->wr_byte(hi);
        IO->wr_byte(lo);
    }
}

void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
              const uint16_t *src, uint16_t stride)
{
    if (!IO || !src || !w || !h) return;
    lcd_set_window(x, (uint16_t)(x + w - 1), y, (uint16_t)(y + h - 1));
    IO->dc(true);
    for (uint16_t j = 0; j < h; j++) {
        const uint16_t *row = src + (uint32_t)j * stride;
        for (uint16_t i = 0; i < w; i++) {
            IO->wr_byte((uint8_t)(row[i] >> 8));
            IO->wr_byte((uint8_t)row[i]);
        }
    }
}
