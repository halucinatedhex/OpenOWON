/*
 * Extension firmware for the XDM1241 - example hook.
 *
 * This is the Ship-of-Theseus entry point: it lives in free flash, is reached
 * by patching a stock call site, and may call back into stock firmware by the
 * names declared in the .sym files under symbols/.
 *
 * The example below intercepts SCPI command enqueue so you can add your own
 * commands while leaving every stock command working.
 */
#include <stdint.h>
#include <stddef.h>

/* ---- stock firmware, resolved by the linker from stock_symbols.ld ---- */
extern void     stock_scpi_enqueue(const char *cmd, uint8_t source);
extern uint32_t stock_strlen(const char *s);

/* Our own state lives in EXT_RAM, clear of anything stock touches. */
static uint32_t ext_custom_hits;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/*
 * Replacement for stock_scpi_enqueue. Patch the stock call site to land here:
 *     bl <addr-of-caller> ext_scpi_enqueue_hook
 */
void ext_scpi_enqueue_hook(const char *cmd, uint8_t source)
{
    if (streq(cmd, "EXT:PING")) {
        ext_custom_hits++;
        return;                     /* handled here; stock never sees it */
    }
    stock_scpi_enqueue(cmd, source);  /* everything else behaves as before */
}

/* Called once if you hook the reset path; zeroes our .bss and copies .data. */
extern uint32_t __ext_data_start, __ext_data_end, __ext_data_load;
extern uint32_t __ext_bss_start, __ext_bss_end;

__attribute__((section(".ext_entry"))) void ext_init(void)
{
    uint32_t *s = &__ext_data_load, *d = &__ext_data_start;
    while (d < &__ext_data_end) *d++ = *s++;
    for (d = &__ext_bss_start; d < &__ext_bss_end; ) *d++ = 0;
}
