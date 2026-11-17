/* The XDM1241 front panel: raw scan code -> key, with debounce.
 *
 * The scan (lib/hal) returns the A lines pulled low in bits 0-4 and the B
 * lines in bits 8-12 (docs/hardware.md). One key joins one A line and one
 * B line, so a single press sets exactly one bit in each half.
 *
 * Mapped on our meter 2026-11-11 by pressing every key with the
 * xdm1241-panel build logging raw codes. Pure: host-tested. */
#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>

#include "platform.h"

/* One code -> one key. KEY_NONE for no key, two keys at once, or a code no
 * key on our panel produces. */
app_key_t keypad_decode(uint32_t code);

/* Debounce: a key is reported once, when its code has been the same for
 * KEYPAD_STABLE consecutive scans. Nothing more until all keys are up. */
#define KEYPAD_STABLE 2u

typedef struct {
    uint32_t last;       /* last raw code seen */
    uint8_t  same;       /* consecutive scans with that code */
    uint8_t  reported;   /* the current press has been reported */
} keypad_t;

void      keypad_reset(keypad_t *k);
app_key_t keypad_feed(keypad_t *k, uint32_t code);   /* call once per scan */

#endif
