/*
 * XDM1241 panel driver - 8080-style 8-bit parallel bus.
 *
 * Recovered from the stock firmware (docs/display.md):
 *   D0-D7  PA0-PA7      WR  PB10      D/C  PB2
 *   pixel format RGB565, high byte first
 *   commands are INDICES into a table the stock firmware keeps at 0x2000003A,
 *   recovered from compressed .data: it is the standard MIPI DCS set, so
 *   index 0x12 = CASET 0x2A, 0x13 = RASET 0x2B, 0x14 = RAMWR 0x2C
 *
 * Transport is abstracted so the same drawing code runs on the host against
 * a mock, and on hardware against real registers.
 */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Command indices as the stock firmware uses them. lcd_cmd() maps these
 * through cmd_map; LCD_DCS_MAP below is the stock table, decompressed from
 * .data, and is the standard MIPI DCS command set. */
#define LCD_CMD_COLUMN  0x12      /* -> DCS 0x2A CASET */
#define LCD_CMD_ROW     0x13      /* -> DCS 0x2B RASET */
#define LCD_CMD_WRITE   0x14      /* -> DCS 0x2C RAMWR */

/* The stock index->opcode table, verbatim. */
#define LCD_DCS_MAP { \
    0x01,0x04,0x05,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11, \
    0x12,0x13,0x20,0x21,0x28,0x29,0x2A,0x2B,0x2C,0x2E,0x30,0x34 }
#define LCD_DCS_MAP_LEN 24

typedef struct {
    void (*wr_byte)(uint8_t b);   /* one byte onto D0-D7, pulse WR */
    void (*dc)(bool data);        /* false = command, true = data  */
    /* Index -> controller opcode. NULL sends the index unchanged, which is
     * what you want while the real table is still unknown. */
    const uint8_t *cmd_map;
    uint8_t        cmd_map_len;
} lcd_io_t;

void lcd_init(const lcd_io_t *io);
void lcd_cmd(uint8_t index);
void lcd_data(uint8_t b);
void lcd_set_window(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);
/* Push a framebuffer rectangle. src is row-major, stride in pixels. */
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
              const uint16_t *src, uint16_t stride);

#endif
