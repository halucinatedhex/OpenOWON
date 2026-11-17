/* The Utility menu: the meter's preferences, edited on the panel.
 *
 * Opened with UTILITY. UP/DOWN (or F1/F2) pick an item, LEFT/RIGHT (or
 * F3/F4) change it, UTILITY/F6/RUN-STOP close it. Any function key closes
 * the menu and does its usual job. Changes apply at once and are persisted
 * like every other setting (debounced, settings.c).
 *
 * The items follow stock's System and Beeper menus (menu tree, RAM
 * 0x20000390: Backlight, Default, Version, Serial Num, Baud, Beeper,
 * Threshold, Speed) plus ours: diode threshold, auto-hold, dim timeout, temperature
 * unit, dBm reference, theme, calibration status and battery.
 */
#ifndef APP_MENU_H
#define APP_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

typedef enum {
    /* display */
    MI_BACKLIGHT = 0,
    MI_DIM,
    MI_THEME,
    MI_BARGRAPH,
    /* measuring */
    MI_SPEED,
    MI_THRESHOLD,
    MI_DIODE_TH,
    MI_TEMP_UNIT,
    MI_CJ_OFFSET,
    MI_RTD_LEADS,
    MI_DBM_REF,
    /* sound */
    MI_BEEPER,
    MI_BEEP_PITCH,
    MI_BEEP_VOL,
    MI_BEEP_STYLE,
    MI_BEEP_ATTACK,
    MI_BEEP_RELEASE,
    MI_BEEP_GLIDE,
    MI_BEEP_PATTERN,
    MI_CLICK_LEN,
    /* system */
    MI_BAUD,
    MI_TERMINAL,
    MI_DEFAULTS,        /* action: press RIGHT twice */
    MI_VERSION,         /* read-only from here on */
    MI_CLOCK,           /* B33: set over SCPI (SYST:DATE, SYST:TIME) */
    MI_SERIAL,
    MI_CAL,
    MI_BATTERY,
    MI_COUNT
} menu_item_t;

#define MENU_NDIM   5
#define MENU_NBAUD  5
extern const uint16_t menu_dim_s[MENU_NDIM];       /* 0 = never */
extern const uint32_t menu_baud[MENU_NBAUD];
#define MENU_BAUD_DEFAULT 4                        /* 115200 */

struct app;

extern const char *const menu_labels[MI_COUNT];
extern const bool menu_divider[MI_COUNT];         /* a group starts here */

/* Text for an item's current value. */
void menu_value(const struct app *a, menu_item_t it, char *out, size_t cap);
/* Step an item's value by dir (+1/-1). Read-only items ignore it. */
void menu_adjust(struct app *a, menu_item_t it, int dir);
/* Key handling while the menu is open. Returns false when the key should be
 * handled by the measurement screen instead (the menu has then closed). */
bool menu_key(struct app *a, app_key_t k);

#endif
