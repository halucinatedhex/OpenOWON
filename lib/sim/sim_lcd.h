/* HOST ONLY. A 480x320 MIPI-DCS panel on the 8080 bus, for the simulator.
 *
 * Decodes what lib/display actually sends - CASET / RASET / RAMWR with
 * big-endian coordinates and RGB565 high byte first - into a panel image.
 * Anything the app shows in the simulator has gone through the real driver.
 */
#ifndef SIM_LCD_H
#define SIM_LCD_H

#include <stdint.h>

#include "lcd.h"
#include "ui.h"

void sim_lcd_reset(void);
extern const lcd_io_t sim_lcd_io;          /* cmd_map = the stock DCS table */

const rgb565_t *sim_lcd_pixels(void);      /* SCR_W x SCR_H */
uint32_t sim_lcd_bytes(void);              /* bus bytes since reset */
uint32_t sim_lcd_pixels_written(void);

/* Write the panel image as a 24-bit BMP. Returns 0 on success. */
int sim_lcd_save_bmp(const char *path);

#endif
