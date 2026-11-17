/* TARGET ONLY. Every pin the HAL may drive, where the assignment came from,
 * and whether it has been confirmed on a v3.x board.
 *
 * All of these were recovered from the v2.1.2 image (docs/hardware.md,
 * scripts/pinmap.py). The owner's board is v3.x. CLAUDE.md: "Do not add pin
 * assignments on guesswork. Confirm, then record." So a subsystem's pins are
 * only configured when its HAL_V3_CONFIRMED_* flag is 1, or when the build
 * explicitly opts in with HAL_TRUST_V2_PINS (the xdm1241-bench environment).
 *
 * To confirm a subsystem: watch the stock firmware drive the pins (GDB on
 * the GPIO registers, or a logic analyser), record it in docs/hardware.md,
 * then set its flag here.
 */
#ifndef HAL_PINS_H
#define HAL_PINS_H

#define HAL_V3_CONFIRMED_LCD        0
#define HAL_V3_CONFIRMED_AFE        0
#define HAL_V3_CONFIRMED_BACKLIGHT  0
#define HAL_V3_CONFIRMED_BEEPER     0
#define HAL_V3_CONFIRMED_SERIAL     0
#define HAL_V3_CONFIRMED_KEYPAD     0   /* mapped on our meter 2026-11-11 (xdm1241-panel) */
/* HAL_V3_CONFIRMED_BEEPER: confirmed 2026-11-11 too. The CONFIRMED flags
 * stay 0 so the pin-free builds stay pin-free; the panel build carries the
 * confirmed subsystems until the full application goes on. */

#if defined(HAL_TRUST_V2_PINS)
#define HAL_LCD_ENABLED       1
#define HAL_AFE_ENABLED       1
#define HAL_BACKLIGHT_ENABLED 1
#define HAL_BEEPER_ENABLED    1
#define HAL_SERIAL_ENABLED    1
#define HAL_KEYPAD_ENABLED    1
#else
#if defined(HAL_PANEL_TEST)
/* The xdm1241-panel build: serial console + panel, nothing else. The panel
 * pins (PA0-7, PB2 D/C, PB10 WR, PB1 CS) are the ones OWON's bootloader on
 * OUR v3 board drives (docs/bootloader.md), and CONFIRMED ON SCREEN
 * 2026-11-11: test pattern, correct colours, MADCTL 0x28 for landscape.
 * HAL_V3_CONFIRMED_LCD stays 0 only so the pin-free builds stay pin-free;
 * set it when the full application goes on. */
#define HAL_LCD_ENABLED       1
#else
#define HAL_LCD_ENABLED       HAL_V3_CONFIRMED_LCD
#endif
#define HAL_AFE_ENABLED       HAL_V3_CONFIRMED_AFE
/* B33: the GD32's RTC on the 32.768 kHz crystal (LXTAL), kept by the CR1220
 * cell (owners report it keeps time with the mains off, EEVblog #156).
 * Started without blocking: if the crystal never comes up, the clock reads
 * "not set". NOT YET CHECKED on our meter. */
#if defined(HAL_RTC_TEST)
#define HAL_RTC_ENABLED 1
#else
#define HAL_RTC_ENABLED 0
#endif

/* B47: the battery on PB0 (ADC0 IN8) through a 1:2 divider from harness
 * VBAT. CONFIRMED on our meter 2026-11-15: PB0 2.087 V with VBAT 4.21 V on
 * the owner's meter, and it rose with charging (DIAG:ADC?). */
#if defined(HAL_BATTERY_TEST)
#define HAL_BATTERY_ENABLED 1
#else
#define HAL_BATTERY_ENABLED 0
#endif

#if defined(HAL_BACKLIGHT_TEST)
/* PA11: OWON's bootloader on our v3 board drives it low, draws the frame,
 * then sets it high as the backlight comes on (docs/bootloader.md), and
 * stock v2.1.2 drives it as TIM1_CH4 PWM. CONFIRMED on our meter
 * 2026-11-14: the app's BACKLIGHT setting dims it (DIAG:BL on the panel
 * build does the same). */
#define HAL_BACKLIGHT_ENABLED 1
#else
#define HAL_BACKLIGHT_ENABLED HAL_V3_CONFIRMED_BACKLIGHT
#endif
#if defined(HAL_BEEPER_TEST)
#define HAL_BEEPER_ENABLED    1
#else
#define HAL_BEEPER_ENABLED    HAL_V3_CONFIRMED_BEEPER
#endif
#if defined(HAL_KEYPAD_TEST)
#define HAL_KEYPAD_ENABLED    1
#else
#define HAL_KEYPAD_ENABLED    HAL_V3_CONFIRMED_KEYPAD
#endif
#if defined(HAL_SERIAL_CONSOLE)
/* The xdm1241-serial build: the serial port and nothing else, as the log
 * console for a readout-protected meter, where RTT cannot work
 * (docs/readout-protection.md). PA9/PA10 are derived from v2 (USART1 with
 * no remap), not confirmed on v3. Circumstantial support: OWON's bootloader
 * and stock V4.3.0 both talk on this port. Driving PA9 as TX is the one
 * pin risk this build takes. */
#define HAL_SERIAL_ENABLED    1
#else
#define HAL_SERIAL_ENABLED    HAL_V3_CONFIRMED_SERIAL
#endif
#endif

/* Panel, 8080 bus: stock lcd_gpio_init 0x08005D8C, lcd_write_byte 0x08005E9A */
#define PIN_LCD_DATA_PORT  GPIOA        /* PA0-PA7 = D0-D7 (PA7 may be shared with the keypad) */
#define PIN_LCD_CTL_PORT   GPIOB
#define PIN_LCD_DC         GPIO_PIN_2   /* PB2  0 = command */
#define PIN_LCD_WR         GPIO_PIN_10  /* PB10 latches on the rising edge */
#define PIN_LCD_CS         GPIO_PIN_1   /* PB1  chip select, active low (docs/bootloader.md).
                                           Not driven yet: the bootloader initialised the panel */

/* HY3131, bit-banged SPI: stock 0x080077C8 */
#define PIN_AFE_PORT       GPIOB
#define PIN_AFE_CS         GPIO_PIN_12  /* PB12 active low */
#define PIN_AFE_CK         GPIO_PIN_13  /* PB13 */
#define PIN_AFE_MISO       GPIO_PIN_14  /* PB14 chip DO -> MCU */
#define PIN_AFE_MOSI       GPIO_PIN_15  /* PB15 MCU -> chip DI */

/* Backlight: PA11 = TIM1_CH4 (GD32 TIMER0 CH3) PWM. stock_lcd_backlight
 * 0x080060C2 writes TIM1 CCR4; set up in 0x080060EE. Recovered. */
#define PIN_BL_PORT        GPIOA
#define PIN_BL             GPIO_PIN_11

/* Beeper, CONFIRMED on our meter 2026-11-11 (DIAG:BEEP, xdm1241-panel): PB11 = TIM2_CH4 under the full remap (GD32 TIMER1 CH3),
 * set up in 0x0800618A as ~3 kHz PWM - the frequency of a typical magnetic
 * buzzer. That stock drives this PWM is recovered; that it is the buzzer is
 * inference. On v3, OWON's bootloader configures PB11 as an output and holds
 * it low (docs/bootloader.md): consistent with a buzzer driver. */
#define PIN_BEEP_PORT      GPIOB
#define PIN_BEEP           GPIO_PIN_11

/* SCPI serial: stock uses USART1 at 0x40013800 (GD32: USART0), whose pins are
 * PA9 TX / PA10 RX without a remap. The peripheral is recovered; the pins
 * follow from it. 115200 8N1 is our default - stock's serial menu sets the
 * baud rate, and its factory value is still to be read from the dump. */
#define PIN_UART_PORT      GPIOA
#define PIN_UART_TX        GPIO_PIN_9
#define PIN_UART_RX        GPIO_PIN_10
#define HAL_UART_BAUD      115200u

/* Settings flash, 0x0803B800-0x0803C7FF (lib/store). Not a pin, but gated the
 * same way and NOT enabled by HAL_TRUST_V2_PINS: whether these pages are free
 * on a v3.x board is only known from its dump. Check the backup shows them
 * erased (or holding nothing stock uses), then set this to 1. */
#ifndef HAL_STORE_ENABLED
#define HAL_STORE_ENABLED 0
#endif

/* Data-log flash, 0x08030000-0x0803B7FF (lib/datalog), gated the same way:
 * set it once the v3 dump shows those 23 pages erased. Our own image must
 * also end below 0x08030000, which linker/app.ld enforces. */
#ifndef HAL_LOG_ENABLED
#define HAL_LOG_ENABLED 0
#endif

/* Backlight PWM frequency. Stock's 11 kHz squeals (EEVblog XDM1041 thread
 * #290). 25 kHz is above hearing; if the panel's LED driver dims badly or
 * flickers at it, fall back to 11000 and note it in docs/hardware.md. */
#ifndef HAL_BACKLIGHT_PWM_HZ
#define HAL_BACKLIGHT_PWM_HZ 25000u
#endif

/* HY3131 bit-bang speed: half a bit period in microseconds. Stock runs
 * 192 us (2.6 kHz, measured on the XDM2041, same isolated bus): a range
 * change's 25-register write took ~80 ms and each register read ~6 ms.
 * On our meter (2026-11-14, DIAG:BUS: write the DC V row, read R20-R37
 * back) every speed from 192 us down to 1 us gave 0 bad read-backs (3000
 * passes at 1 us); 4 us gave 0 in 10000 passes, 4.35 ms a pass against
 * 161 ms at 192 us. 4 us keeps a 4x margin to the fastest clean setting. */
#ifndef HAL_AFE_HALF_BIT_US
#define HAL_AFE_HALF_BIT_US 4u
#endif

/* Keypad: no row pins. Two groups of five lines take turns (stock v2.1.2
 * keypad_row_select 0x08004C94, keypad_scan 0x08004E36): one group driven
 * low, the other read with pull-ups; each key joins one A line to one B
 * line. v2-derived. Circumstantial support on v3: stock V4.3.0 shuts SWD off
 * once running, and PA14 (SWCLK) is a B line. Driven only by the
 * HAL_KEYPAD_TEST build (xdm1241-panel), after a passive check.
 *   A (scan bits 0-4):  PB5 PB6 PB7 PB8 PB9
 *   B (scan bits 8-12): PA14 PA15 PB3 PB4 PC13
 * PA14/PA15/PB3/PB4 are SWD/JTAG pins: using them needs SWJ disabled. */
#define KEY_A_PORT   GPIOB
#define KEY_A_PINS   (GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9)

/* Not yet known at all: PWR_EN. */

#endif
