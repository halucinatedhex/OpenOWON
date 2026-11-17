/* Power-on settings: what the meter remembers across power cycles.
 *
 * Packed into a small versioned record and kept by lib/store. Unpacking
 * validates every field, so a record from an older layout or a corrupt one
 * degrades to defaults field by field rather than failing as a whole.
 * Saving is debounced (SETTINGS_SAVE_DELAY_MS after the last change) so that
 * stepping through ranges does not spend flash erase cycles.
 */
#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#define SETTINGS_VERSION        1
#define SETTINGS_MAX_LEN      112    /* = STORE_MAX_PAYLOAD; 42 used (2026-11-11) */
#define SETTINGS_SAVE_DELAY_MS 3000u

struct app;   /* app_t */

/* Returns the packed length. */
size_t settings_pack(const struct app *a, uint8_t *buf, size_t cap);
/* Apply a packed record; fields that fail validation are left as they are. */
void   settings_apply(struct app *a, const uint8_t *buf, size_t len, uint16_t version);

#endif
