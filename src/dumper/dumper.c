/*
 * XDM1241 flash dumper: the smallest image OWON's bootloader can install
 * (one 10 KB pack), so that it overwrites as little of the stock firmware as
 * possible, and then reads the whole flash out over the serial port.
 *
 * Why: the meter is readout-protected, so a debugger can't read flash, and
 * OWON won't provide the V4.x update file. Code on the chip can read its own
 * flash. If the bootloader erases only the pack it writes, everything from
 * 0x08005000 up survives: about 110 KB of the V4.3.0 application, plus the
 * bootloader and the calibration/settings pages (docs/install.md).
 *
 * Read-only. It writes no flash and drives one pin: PA9, serial TX (USART0,
 * the meter's SCPI port; derived from v2, see lib/hal/hal_pins.h).
 * No interrupts. Registers are touched here directly, through the SPL: this
 * is a standalone target tool, not part of the layered application.
 *
 * Commands (115200 8N1, line ending CR or LF):
 *   *IDN?         XDM1241-OPEN,dumper,<version>
 *   DIAG:BOOT?    0x08000000 + 0x2800   (the bootloader)
 *   DIAG:FLASH?   0x08000000 + 0x40000  (everything)
 * Output is lib/console/flashdump.h format; scripts/read-boot.py reads it.
 * While idle it prints a "ready" line every 2 s, so you can see it is alive.
 */
#include <stdint.h>
#include <string.h>

#include "gd32f30x.h"
#include "flashdump.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

#define FLASH_BASE_ADDR 0x08000000u
#define FLASH_LEN       0x40000u      /* GD32F303CC: 256 KB */
#define BOOT_LEN        0x2800u

extern uint32_t _app_start;

/* The bootloader may have started the independent watchdog; it can't be
 * stopped, only fed. Feeding one that isn't running does nothing. */
static void feed(void) { fwdgt_counter_reload(); }

static void tx(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        while (usart_flag_get(USART0, USART_FLAG_TBE) == RESET) { }
        usart_data_transmit(USART0, (uint8_t)s[i]);
    }
}

static void txs(const char *s) { tx(s, strlen(s)); }

static void out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    feed();
    tx(s, n);
}

static int rx(char *ch)
{
    if (usart_flag_get(USART0, USART_FLAG_RBNE) == RESET) {
        /* An overrun leaves RBNE clear but blocks reception; clear it. */
        if (usart_flag_get(USART0, USART_FLAG_ORERR) == SET) (void)usart_data_receive(USART0);
        return 0;
    }
    *ch = (char)usart_data_receive(USART0);
    return 1;
}

static void serial_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);
    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_9);
    gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_10);
    usart_deinit(USART0);
    usart_baudrate_set(USART0, 115200u);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);
}

static void command(char *line)
{
    for (char *p = line; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    if (strcmp(line, "*IDN?") == 0)
        txs("XDM1241-OPEN,dumper," FW_VERSION "\r\n");
    else if (strcmp(line, "DIAG:BOOT?") == 0)
        flashdump_emit((const uint8_t *)FLASH_BASE_ADDR, FLASH_BASE_ADDR, BOOT_LEN, out, NULL);
    else if (strcmp(line, "DIAG:FLASH?") == 0)
        flashdump_emit((const uint8_t *)FLASH_BASE_ADDR, FLASH_BASE_ADDR, FLASH_LEN, out, NULL);
    else if (line[0])
        txs("ERR unknown command\r\n");
}

int main(void)
{
    /* Our own vector table (the bootloader leaves VTOR on its own). No
     * interrupts are enabled, but a fault should land in our handlers. */
    SCB->VTOR = (uint32_t)&_app_start;
    __DSB();
    __ISB();

    serial_init();       /* the core runs on IRC8M (8 MHz): plenty for 115200 */

    /* SysTick as a polled 1 ms tick, no interrupt. */
    SysTick->LOAD = SystemCoreClock / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    txs("\r\nXDM1241-OPEN dumper " FW_VERSION " ready\r\n");

    char line[32];
    size_t len = 0;
    uint32_t idle_ms = 0;
    for (;;) {
        feed();
        char ch;
        if (rx(&ch)) {
            idle_ms = 0;
            if (ch == '\r' || ch == '\n') {
                line[len] = '\0';
                command(line);
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = ch;
            }
        } else if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
            if (++idle_ms >= 2000u && len == 0) {
                idle_ms = 0;
                txs("XDM1241-OPEN dumper ready\r\n");
            }
        }
    }
}
