#include "keypad.h"

#include <stddef.h>

/* Raw code -> key, as pressed on our meter (docs/hardware.md has the table
 * with line names). Screen1-5 are the softkeys under the display: F1-F5.
 * Range is the app's auto/manual range key, KEY_LEFT (lib/app/app.c).
 * Our panel has no F6, Left or Right key. */
static const struct { uint16_t code; app_key_t key; } MAP[] = {
    { 0x1001, KEY_V },       { 0x1002, KEY_I },      { 0x1004, KEY_OHM },
    { 0x1008, KEY_CAP },     { 0x1010, KEY_FREQ },   { 0x0810, KEY_TEMP },
    { 0x0101, KEY_F1 },      { 0x0201, KEY_F2 },     { 0x0401, KEY_F3 },
    { 0x0801, KEY_F4 },      { 0x0802, KEY_F5 },
    { 0x0208, KEY_UP },      { 0x0408, KEY_DOWN },   { 0x0404, KEY_LEFT },
    { 0x0108, KEY_RUNSTOP }, { 0x0202, KEY_MATH },   { 0x0204, KEY_UTILITY },
    { 0x0104, KEY_RECORD },  { 0x0804, KEY_DUAL },
};

app_key_t keypad_decode(uint32_t code)
{
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (MAP[i].code == code) return MAP[i].key;
    return KEY_NONE;
}

void keypad_reset(keypad_t *k)
{
    k->last = 0;
    k->same = 0;
    k->reported = 0;
}

app_key_t keypad_feed(keypad_t *k, uint32_t code)
{
    if (code != k->last) {
        k->last = code;
        k->same = 1;
        if (code == 0) k->reported = 0;
        return KEY_NONE;
    }
    if (k->same < 255u) k->same++;
    if (code == 0) {
        k->reported = 0;
        return KEY_NONE;
    }
    if (k->reported || k->same < KEYPAD_STABLE) return KEY_NONE;
    k->reported = 1;
    return keypad_decode(code);
}
