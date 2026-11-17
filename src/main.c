/*
 * XDM1241 replacement firmware - entry point.
 *
 * Runs at 0x08002800, behind OWON's bootloader (docs/boot.md).
 *
 * Three builds:
 *   xdm1241        drives NO pins. Clock, vector table, SysTick, RTT log,
 *                  heartbeat. The first thing to flash: proves the toolchain,
 *                  the link address, the bootloader handover and the clock,
 *                  and cannot damage anything.
 *   xdm1241-serial the pin-free build plus the serial port, for a
 *                  readout-protected meter (ours): installed with
 *                  scripts/serial-update.py, logs on the serial port, and
 *                  answers `*IDN?` and `DIAG:BOOT?` (the bootloader, as hex:
 *                  scripts/read-boot.py). Nothing else.
 *   xdm1241-bench  HAL_TRUST_V2_PINS: drives the v2-derived pin map and runs
 *                  the full application. Only after the pins are confirmed on
 *                  this board (lib/hal/hal_pins.h).
 *
 * Watch it with:  JLinkRTTClient (unprotected meter), or a terminal on the
 *                 serial port at 115200 (xdm1241-serial).
 */

/* Size, not speed, for this file: it is the console, bench diagnostics and a
 * loop that calls app_step (the hot code lives in lib/). At -O2 the ui image
 * outgrew the app slot below the recording area (2026-11-16, 668 bytes). */
#pragma GCC optimize("Os")

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gd32f30x.h"

#include "app.h"
#include "hal.h"
#include "rtt.h"
#include "flashdump.h"
#include "lcd.h"
#include "keypad.h"
#include "hy3131.h"
#include "afe_table.h"
/* The front-end diagnostics (DIAG:AFE?, DIAG:ROW, DIAG:VI, ...) run from the
 * console, which only the panel build reaches: in the app build the serial
 * port is SCPI's. Left out there to fit the app slot (2026-11-16). */
#if defined(HAL_AFE_PROBE) && !defined(HAL_AFE_DCV_REAL)
#define MAIN_AFE_DIAG 1
#include "afe_provisional.h"
#endif
#include "meas.h"
#include "meas_temp.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

/* volatile + non-static: visible to GDB, survives optimisation */
volatile uint32_t heartbeat = 0u;
volatile uint32_t boot_marker = 0xA11C0DEu;

static app_t app;

/* ---- log: RTT always, the serial port too in xdm1241-serial ---------------- */

static void log_write(const char *s, size_t n)
{
    rtt_write(s, n);
    hal_console_write(s, n);
}

static void log_puts(const char *s) { log_write(s, strlen(s)); }

__attribute__((format(printf, 1, 2)))
static void log_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) log_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* ---- console commands (xdm1241-serial) ---------------------------------------
 * Deliberately tiny. Nothing here writes flash.
 *   *IDN?        identity, so tools can tell our firmware from stock
 *   DIAG:RESET   reply OK, then reset the chip: caught in the HAL's serial
 *                layer (so it also works under the app's SCPI); the
 *                bootloader runs again, so serial-update.py --reset needs
 *                no button press
 *   DIAG:FILL <hex>    panel build only: fill the screen with an RGB565 colour
 *   DIAG:PATTERN       panel build only: redraw the test pattern
 *   DIAG:DCV [r]       panel build only: configure DC V range r (0-5, default
 *                      2 = 5 V) from the recovered table (docs/afe-table.md),
 *                      relays as it says, then read AD1: counts and calibrated
 *                      volts. 0-1 only if the input reads under 0.4 V first
 *   DIAG:OHM [r]       panel build only: 2-wire resistance range r (0-5, default
 *                      1 = 5 kohm) from afe_table.h, both relays on, then AD1
 *                      through the factory calibration
 *   DIAG:CONT, DIAG:DIODE  panel build only: the continuity / diode setting
 *                      from afe_table.h, then AD1
 *   DIAG:TEMP          panel build only: type-K thermocouple, the PROVISIONAL
 *                      emulated rows: the cold junction (on-chip sensor, two
 *                      AD1 readings at SAD1I = 10 / 11), then the
 *                      thermocouple EMF on DC 500 mV, compensated
 *   DIAG:ROW f r [n] [RR=HH ...] [PA8=x] [PA13=x]
 *                      panel build only: any PROVISIONAL emulated row
 *                      (src/afe_provisional.h, e.g. ACV 2, FREQ 0, DCI 1),
 *                      then n readings converted through the factory cal
 *   DIAG:CAPX r RR=HH ...  panel build only: capacitance range r with
 *                      HY3131 registers RR (20-36 hex) overridden, for the
 *                      B16 characterisation (docs/test-plan.md T-CAP)
 *   DIAG:CAP [r]       panel build only: capacitance range r (0-4, default 3 =
 *                      50 uF), the PROVISIONAL emulated row (not yet in
 *                      afe_table.h), then the counters CTB/CTC through the
 *                      factory calibration
 *   DIAG:BUS us n      panel build only: the HY3131 bus at a half-bit time of
 *                      `us` microseconds (stock: 192): write the DC V 5 V row
 *                      n times, read R20-R37 back after each, count
 *                      mismatches; then the speed goes back to the default.
 *                      Writes the chip's working registers only, never the
 *                      relays or flash. Leads disconnected.
 *   DIAG:AFE?          panel build only: read every HY3131 register (0x00-0x36),
 *                      twice; reads only, never writes. Needs the pin check
 *                      at boot to have passed
 *   DIAG:VI i v [n]    panel build only: DC current row i and DC volts row v
 *                      in turn, n cycles (B21, power): both readings and how
 *                      far the current moves across a volts read
 *   DIAG:BL <pct>      panel build only: backlight duty on PA11 (0-100)
 *   DIAG:BEEP <ms>     panel build only: sound the beeper (PB11, ~3 kHz)
 *   DIAG:TONE <hz> <level> <ms>  panel build only: the beeper at a pitch
 *                      (200-8000 Hz) and drive (1-100, 100 = 50% duty, as
 *                      stock) for up to 3 s: for measuring loudness (B57)
 *   DIAG:MADCTL <hex>  panel build only: set the controller's orientation
 *                      register (DCS 0x36), then redraw the pattern
 *   DIAG:BOOT?   OWON's bootloader, 0x08000000-0x080027FF, in flashdump
 *                format. Code on the chip may read its own flash even
 *                under readout protection; only this range is offered. */
#define BOOT_BASE 0x08000000u
#define BOOT_LEN  0x2800u

static void console_out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    hal_console_write(s, n);
}

/* The panel test pattern: eight vertical bars across the long side, and a
 * white square at the origin, so orientation and colour order both show.
 * Bars, left to right if the origin is top-left: red, green, blue, white,
 * black, yellow, cyan, magenta. */
#define PANEL_W 480u
#define PANEL_H 320u

/* Orientation is set in the HAL (MADCTL 0x28); DIAG:MADCTL is for trying
 * others live. DCS 0x36 isn't in the stock index table, so lcd_cmd() sends
 * it unchanged. */
static void panel_madctl(uint8_t v)
{
    lcd_cmd(0x36);
    lcd_data(v);
    log_printf("panel: MADCTL 0x%02x\n", v);
}

static void panel_pattern(void)
{
    static const uint16_t bars[8] = { 0xF800, 0x07E0, 0x001F, 0xFFFF,
                                      0x0000, 0xFFE0, 0x07FF, 0xF81F };
    for (unsigned i = 0; i < 8; i++)
        lcd_fill_rect((uint16_t)(i * PANEL_W / 8), 0, PANEL_W / 8, PANEL_H, bars[i]);
    lcd_fill_rect(0, 0, 40, 40, 0xFFFF);
    lcd_fill_rect(8, 8, 24, 24, 0x0000);
    log_puts("panel: pattern drawn (bars R G B W K Y C M, white square at the origin)\n");
}

#if defined(HAL_KEYPAD_TEST)
/* Log every change of the raw scan: which A and B lines are pulled low. */
static int keys_stuck;

static void keys_poll(void)
{
    static uint32_t last_ms, last_code = 0xFFFFFFFFu;
    uint32_t now = hal_millis();
    if (now - last_ms < 20u) return;
    last_ms = now;
    uint32_t code = hal_keypad_scan();
    if (code == last_code) return;
    last_code = code;
    static const char *const a_names[5] = { "PB5", "PB6", "PB7", "PB8", "PB9" };
    static const char *const b_names[5] = { "PA14", "PA15", "PB3", "PB4", "PC13" };
    char buf[96];
    int n = snprintf(buf, sizeof buf, "keys 0x%04lx", (unsigned long)code);
    for (int i = 0; i < 5; i++)
        if (code & (1u << i)) n += snprintf(buf + n, sizeof buf - (size_t)n, " A:%s", a_names[i]);
    for (int i = 0; i < 5; i++)
        if (code & (1u << (8 + i))) n += snprintf(buf + n, sizeof buf - (size_t)n, " B:%s", b_names[i]);
    static const char *const names[KEY_COUNT] = {
        "none", "F1", "F2", "F3", "F4", "F5", "F6", "V", "A", "OHM", "CAP", "FREQ",
        "TEMP", "UP", "DOWN", "RANGE", "RIGHT", "RUN/STOP", "RECORD", "UTILITY",
        "MATH", "DUAL" };
    app_key_t k = keypad_decode(code);
    log_printf("%s%s%s%s\n", buf, code ? "" : " (released)",
               k != KEY_NONE ? " = " : "", k != KEY_NONE ? names[k] : "");
}
#endif

#if defined(MAIN_AFE_DIAG)
static bool afe_bus_ok;

/* Two full reads of 0x00-0x36: whether the chip answers, and whether the
 * bus is stable (the data registers may move between reads; the
 * configuration registers must not). */
static void afe_dump(void)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not reading\n"); return; }
    uint8_t r[2][0x37];
    for (int pass = 0; pass < 2; pass++)
        for (uint8_t i = 0; i < 0x37; i++) r[pass][i] = hy3131_read8(i);
    for (int pass = 0; pass < 2; pass++)
        for (int row = 0; row < 0x37; row += 16) {          /* 16 a line: the log buffer is short */
            char buf[16 * 3 + 24];
            int n = snprintf(buf, sizeof buf, "afe read %d %02x:", pass + 1, row);
            for (int i = row; i < row + 16 && i < 0x37; i++)
                n += snprintf(buf + n, sizeof buf - (size_t)n, " %02x", r[pass][i]);
            log_printf("%s\n", buf);
        }
    int diff = 0;
    for (int i = 0x20; i < 0x37; i++) diff += r[0][i] != r[1][i];
    log_printf("afe: config registers 0x20-0x36 %s between reads\n", diff ? "DIFFER" : "identical");
}
#endif

#if defined(MAIN_AFE_DIAG)
/* DC V rows: lib/hal/afe_table.h (recovered by emulation, confirmed here). */
static const char *const DCV_NAME[6] = { "50 mV", "500 mV", "5 V", "50 V", "500 V", "1000 V" };

static bool dcv_configure(int r)
{
    const afe_row_t *row = &AFE_DCV[r];
    hy3131_write8(HY_R20, 0x68);                  /* stock's order: R20, burst, relays */
    hy3131_write_burst(HY_R20, row->r20, sizeof row->r20);
    hal_afe_relays(row->pa8, row->pa13);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    for (unsigned i = 0; i < sizeof back; i++) if (back[i] != row->r20[i]) return false;
    return true;
}

/* One settled reading: skip the first two after configuring (the filter
 * fills), then n more. Returns false if AD1 stops converting. */
static bool dcv_read(int r, int n, bool print, double *last)
{
    const meas_cal_t *cal = hal_platform()->cal;
    for (int k = 0; k < n + 2; k++) {
        uint32_t t0 = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - t0 < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("dcv: no AD1 conversion in 1 s (INTF %02x)\n", intf); return false; }
        int32_t c = hy3131_read_ad1();
        *last = meas_dcv(cal, r, c);
        if (print && k >= 2)
            log_printf("dcv %s: AD1 %ld counts = %.6g V (factory cal)%s\n", DCV_NAME[r], (long)c, *last,
                       c >= 8388606 || c <= -8388607 ? "  OVER-RANGE" : "");
    }
    return true;
}

/* 2-wire resistance rows: lib/hal/afe_table.h (confirmed on the meter). */
static const char *const OHM_NAME[6] = { "500 ohm", "5 kohm", "50 kohm", "500 kohm", "5 Mohm", "50 Mohm" };

static void afe_ohm_test(int r)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    if (r < 0 || r > 5) { log_puts("ohm: range 0-5 (500 ohm .. 50 Mohm)\n"); return; }
    const afe_row_t *row = &AFE_OHM2[r];
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row->r20, sizeof row->r20);
    hal_afe_relays(row->pa8, row->pa13);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    int bad = 0;
    for (unsigned i = 0; i < sizeof back; i++) bad += back[i] != row->r20[i];
    log_printf("ohm: configured %s; read back %s\n", OHM_NAME[r], bad ? "DIFFERS" : "identical");
    if (bad) return;
    const meas_cal_t *cal = hal_platform()->cal;
    for (int k = 0; k < 8; k++) {
        uint32_t t0 = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - t0 < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("ohm: no AD1 conversion in 1 s (INTF %02x)\n", intf); return; }
        int32_t c = hy3131_read_ad1();
        if (k < 2) continue;                        /* the filter settles */
        log_printf("ohm %s: AD1 %ld counts = %.6g ohm (factory cal)%s\n", OHM_NAME[r], (long)c,
                   meas_ohm(cal, MEAS_OHM2, r, c, false),
                   c >= 8388606 || c <= -8388607 ? "  OVER-RANGE" : "");
    }
}

/* Continuity and diode rows: lib/hal/afe_table.h (confirmed on the meter). */
static double conv_cont(const meas_cal_t *c, int32_t ad1)  { return meas_continuity(c, ad1); }
static double conv_diode(const meas_cal_t *c, int32_t ad1) { return meas_diode(c, ad1); }

static void afe_row_test(const char *name, const afe_row_t *row, const char *unit,
                         double (*conv)(const meas_cal_t *, int32_t))
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row->r20, sizeof row->r20);
    hal_afe_relays(row->pa8, row->pa13);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    int bad = 0;
    for (unsigned i = 0; i < sizeof back; i++) bad += back[i] != row->r20[i];
    log_printf("%s: configured; read back %s\n", name, bad ? "DIFFERS" : "identical");
    if (bad) return;
    const meas_cal_t *cal = hal_platform()->cal;
    for (int k = 0; k < 9; k++) {
        uint32_t t0 = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - t0 < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("%s: no AD1 conversion in 1 s (INTF %02x)\n", name, intf); return; }
        int32_t c = hy3131_read_ad1();
        if (k < 3) continue;                        /* settle, as ohms */
        log_printf("%s: %lums AD1 %ld counts = %.6g %s (factory cal)%s\n", name,
                   (unsigned long)(hal_millis() - t0), (long)c, conv(cal, c), unit,
                   c >= 8388606 || c <= -8388607 ? "  OVER-RANGE" : "");
    }
}

/* Capacitance, ranges 0-4 (50 nF .. 500 uF): PROVISIONAL, the emulated
 * CAP rows (docs/afe-table.md), on the meter only through DIAG:CAP until
 * confirmed. R20-R37 differ between ranges only in R2C-R2D (the PA taps),
 * R2F and R31. Both relays on, as stock. */
static const uint8_t CAP_R20[5][24] = {
    { 0x72, 0x21, 0x10, 0x8b, 0x85, 0x11, 0x08, 0x15, 0x11, 0xf8, 0x00, 0x00,
      0x00, 0x08, 0x00, 0x9a, 0x80, 0xd7, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 },
    { 0x72, 0x21, 0x10, 0x8b, 0x85, 0x11, 0x08, 0x15, 0x11, 0xf8, 0x00, 0x00,
      0x00, 0x08, 0x00, 0x9a, 0x80, 0xd7, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 },
    { 0x72, 0x21, 0x10, 0x8b, 0x85, 0x11, 0x08, 0x15, 0x11, 0xf8, 0x00, 0x00,
      0x80, 0x00, 0x00, 0x8e, 0x80, 0xd1, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 },
    { 0x72, 0x21, 0x10, 0x8b, 0x85, 0x11, 0x08, 0x15, 0x11, 0xf8, 0x00, 0x00,
      0x08, 0x00, 0x00, 0x8e, 0x80, 0xd1, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 },
    { 0x72, 0x21, 0x10, 0x8b, 0x85, 0x11, 0x08, 0x15, 0x11, 0xf8, 0x00, 0x00,
      0x08, 0x00, 0x00, 0x8e, 0x80, 0xd2, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 },
};
static const char *const CAP_NAME[5] = { "50 nF", "500 nF", "5 uF", "50 uF", "500 uF" };

static uint32_t le24_(const uint8_t b[3]) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16; }

/* Stock restarts the counters after every capacitance reading
 * (0x08007CBC): R20 <- 0x70 stops them, CTA takes a preset (the gate),
 * R20 <- 0x72 runs them again. The preset is 0xBF0000 for ranges 0-4
 * (0x08007CA8). Without it CTA starts from 0 and a gate takes ~4.6 s. */
static uint32_t cap_cta0;                 /* CTA read back just after the restart */
static unsigned cap_rb = 3;               /* DIAG:CAPRB: bit 0 read CTA after start, bit 1 at the end */
static void cap_restart(void)
{
    static const uint8_t cta_preset[3] = { 0x00, 0x00, 0xbf };
    uint8_t a[3];
    hy3131_write8(HY_R20, 0x70);
    hy3131_write_burst(HY_CTA, cta_preset, 3);
    hy3131_write8(HY_R20, 0x72);
    if (!(cap_rb & 1u)) { cap_cta0 = 0xBF0000u; return; }
    hy3131_read_burst(HY_CTA, a, 3);
    cap_cta0 = (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16;
}

/* Configure capacitance from a row and take n counter readings. The
 * counter's done flag (INTF CTF) is cleared by any INTF read and has been
 * seen missed under fast polling (docs/afe-table.md), so a reading also
 * counts as done when the counters have stopped (CTA unchanged over
 * 100 ms) with at least one whole cycle (CTB > 0). */
static void afe_cap_run(int r, const uint8_t row[24], int n)
{
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row, 24);
    hal_afe_relays(true, true);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    int bad = 0;
    for (unsigned i = 0; i < sizeof back; i++) bad += back[i] != row[i];
    log_printf("cap: configured %s; read back %s\n", CAP_NAME[r], bad ? "DIFFERS" : "identical");
    if (bad) return;
    cap_restart();
    const meas_cal_t *cal = hal_platform()->cal;
    double sum = 0, lo = 1e30, hi = -1e30;
    int got = 0;
    for (int k = 0; k < n; k++) {
        uint32_t t0 = hal_millis(), tick = t0, cta_prev = 0xFFFFFFFFu;
        uint8_t intf = 0;
        bool done = false, stopped = false;
        while (!done && hal_millis() - t0 < 12000u) {
            intf = hy3131_read8(HY_INTF);
            done = intf & HY_F_CT;
            if (!done && hal_millis() - tick >= 100u) {
                tick = hal_millis();
                uint8_t c[9];
                hy3131_read_burst(HY_CTC, c, 9);
                uint32_t cta = le24_(c + 6);
                if (cta == cta_prev && le24_(c + 3) > 0) { done = stopped = true; }
                cta_prev = cta;
            }
        }
        if (!done) { log_printf("cap: no counter result in 12 s (INTF %02x)\n", intf); return; }
        uint8_t cb[3], cc[3], ca[3];
        hy3131_read_burst(HY_CTB, cb, 3);
        hy3131_read_burst(HY_CTC, cc, 3);
        if (cap_rb & 2u) hy3131_read_burst(HY_CTA, ca, 3); else memset(ca, 0, sizeof ca);
        uint8_t sta = hy3131_read8(HY_CTSTA);          /* stock checks bit 0 (CTBOV) */
        uint32_t cta0 = cap_cta0, cta1 = le24_(ca);
        /* Datasheet: counting time = (1000000h - CTA initial + CTA final) / SYSCLK (4.9152 MHz). */
        log_printf("cap gate: INTF %02x CTA start %06lX end %06lX = %lu ticks\n", intf, (unsigned long)cta0,
                   (unsigned long)cta1, (unsigned long)(0x1000000u - cta0 + cta1));
        cap_restart();
        uint32_t ctb = le24_(cb), ctc = le24_(cc);
        double c_f = meas_cap_counter(cal, r, ctb, ctc);
        log_printf("cap %s: %lums CTB %lu CTC %lu STA %02x%s = %.6g F (factory cal)\n", CAP_NAME[r],
                   (unsigned long)(hal_millis() - t0), (unsigned long)ctb, (unsigned long)ctc,
                   sta, stopped ? " (flag missed: counters stopped)" : "", c_f);
        if (ctb && !isnan(c_f)) { sum += c_f; got++; if (c_f < lo) lo = c_f; if (c_f > hi) hi = c_f; }
    }
    if (got) log_printf("cap %s: mean %.6g F over %d, spread %.3g%%\n", CAP_NAME[r], sum / got, got,
                        got > 1 ? 100.0 * (hi - lo) / (sum / got) : 0.0);
}

static void afe_cap_test(int r)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    if (r < 0 || r > 4) { log_puts("cap: range 0-4 (50 nF .. 500 uF)\n"); return; }
    afe_cap_run(r, CAP_R20[r], 8);
}

/* DIAG:CAPX <r> <reg>=<hex> ...: range r's row with registers overridden,
 * to characterise capacitance on our board (B16, docs/test-plan.md T-CAP).
 * R20-R36 only: R37 is the chip's test mode ("write 0 only", datasheet). */
static void afe_capx(char *args)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    char *e;
    long r = strtol(args, &e, 10);
    if (e == args || r < 0 || r > 4) { log_puts("capx: DIAG:CAPX <0-4> [RR=HH ...] (RR 20-36 hex)\n"); return; }
    uint8_t row[24];
    memcpy(row, CAP_R20[r], sizeof row);
    char *p = e;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned long reg = strtoul(p, &e, 16);
        if (e == p || *e != '=') { log_puts("capx: expected RR=HH\n"); return; }
        p = e + 1;
        unsigned long val = strtoul(p, &e, 16);
        if (e == p || val > 0xFF || reg < 0x20 || reg > 0x36) {
            log_puts("capx: registers 20-36 hex only (37 is test mode), values 00-FF\n");
            return;
        }
        row[reg - 0x20] = (uint8_t)val;
        log_printf("capx: R%02lX = %02lX\n", reg, val);
        p = e;
    }
    afe_cap_run((int)r, row, 6);
}

/* Temperature, type K: PROVISIONAL, emulated (docs/afe-table.md). The
 * thermocouple row is DC V 500 mV with R31 = 0xD7 (SAGND = 01); the cold
 * junction is the same row with R33 SAD1I = 10, then 11 (the datasheet's
 * on-chip temperature sensor, and stock's 0x08008DD4). */
static const uint8_t TC_R20[24] = {
    0xc0, 0x00, 0x16, 0x8b, 0x85, 0x11, 0x00, 0x15, 0x31, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x81, 0x80, 0xd7, 0x33, 0xa4, 0x00, 0x00, 0x00, 0x00 };

static bool tc_configure(uint8_t r33)
{
    uint8_t row[24];
    memcpy(row, TC_R20, sizeof row);
    row[0x33 - 0x20] = r33;
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row, sizeof row);
    hal_afe_relays(true, false);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    return memcmp(back, row, sizeof row) == 0;
}

/* The mean of n AD1 readings after skipping three, through the 500 mV
 * calibration, in mV. */
static bool tc_read_mv(int n, double *mv, int32_t *counts)
{
    const meas_cal_t *cal = hal_platform()->cal;
    double sum = 0;
    for (int k = 0; k < n + 3; k++) {
        uint32_t t0 = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - t0 < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("temp: no AD1 conversion in 1 s (INTF %02x)\n", intf); return false; }
        int32_t c = hy3131_read_ad1();
        if (k < 3) continue;
        sum += meas_dcv(cal, 1, c) * 1e3;
        *counts = c;
    }
    *mv = sum / n;
    return true;
}

static void afe_temp_test(void)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    const meas_cal_t *cal = hal_platform()->cal;
    double v1, v2, emf;
    int32_t c1, c2, ce;
    if (!tc_configure(0xa6)) { log_puts("temp: CJ1 read back DIFFERS\n"); return; }
    if (!tc_read_mv(2, &v1, &c1)) return;
    if (!tc_configure(0xa7)) { log_puts("temp: CJ2 read back DIFFERS\n"); return; }
    if (!tc_read_mv(2, &v2, &c2)) return;
    double tcj = meas_cold_junction(v1, v2, cal->cj_g);
    log_printf("temp: CJ1 %ld counts %.4f mV, CJ2 %ld counts %.4f mV, G %ld -> cold junction %.2f C\n",
               (long)c1, v1, (long)c2, v2, (long)cal->cj_g, tcj);
    if (!tc_configure(0xa4)) { log_puts("temp: TC row read back DIFFERS\n"); return; }
    for (int i = 0; i < 4; i++) {
        if (!tc_read_mv(1, &emf, &ce)) return;
        bool over = false;
        double t = meas_tc_k_compensated(emf, tcj, &over);
        log_printf("temp: TC %ld counts %.4f mV -> %.2f C%s\n", (long)ce, emf, t, over ? "  OVER-RANGE (open?)" : "");
    }
}

/* Counter restart for DIAG:ROW: stop, gate preset, run, then read CTA back
 * (without the read-back the gate ends after ~70 ms: docs/afe-table.md,
 * 2026-11-13). Returns CTA as read, the start of the counting time. */
static uint32_t row_ct_restart(uint8_t r20, const uint8_t preset[3])
{
    uint8_t a[3];
    hy3131_write8(HY_R20, (uint8_t)(r20 & ~0x02u));
    hy3131_write_burst(HY_CTA, preset, 3);
    hy3131_write8(HY_R20, (uint8_t)(r20 | 0x02u));
    hy3131_read_burst(HY_CTA, a, 3);
    return le24_(a);
}

/* DIAG:ROW <fn> <range> [n]: any PROVISIONAL emulated row (src/
 * afe_provisional.h), then n readings of what that function reads, through
 * the factory calibration: AD1 (DC, ohms, diode, temperature as EMF), RMS
 * when RMSF is set (AC), or the counters
 * (frequency, with stock's restart and a 0xF00000 gate preset). For the
 * bench session (docs/test-plan.md) before a row earns afe_table.h.
 * Optional RR=HH register overrides follow n. */
static const char *const ROW_FN[MEAS_NFUNC] = { "DCV", "ACV", "DCI", "ACI", "OHM", "OHM4", "CONT",
                                                "DIODE", "CAP", "FREQ", "TEMP" };

/* DIAG:BUS: how fast the isolated HY3131 bus can run (B10d). Each pass
 * writes the DC V 5 V row as stock does (R20 = 0x68, then R20-R37) and
 * reads R20-R37 back. R34 bit 0 is masked, as in DIAG:ROW. */
static void afe_bus_test(const char *args)
{
    char *e;
    unsigned long us = strtoul(args, &e, 10), n = strtoul(e, NULL, 10);
    if (us < 1u || us > 1000u || n < 1u || n > 10000u) {
        log_puts("bus: DIAG:BUS <half-bit us 1-1000> <passes 1-10000>\n");
        return;
    }
    const afe_row_t *row = &AFE_DCV[2];
    uint32_t keep = hal_afe_half_bit_us(), bad = 0, badreg = 0;
    hal_afe_set_half_bit_us(us);
    uint32_t t0 = hal_millis();
    for (unsigned long k = 0; k < n; k++) {
        uint8_t back[24];
        hy3131_write8(HY_R20, 0x68);
        hy3131_write_burst(HY_R20, row->r20, sizeof row->r20);
        hy3131_read_burst(HY_R20, back, sizeof back);
        back[0x14] &= 0xFEu;
        bool same = true;
        for (unsigned i = 0; i < sizeof back; i++)
            if (back[i] != (row->r20[i] & (i == 0x14 ? 0xFEu : 0xFFu))) { same = false; badreg |= 1u << i; }
        if (!same) bad++;
    }
    uint32_t ms = hal_millis() - t0;
    hal_afe_set_half_bit_us(keep);
    log_printf("bus: %lu us half-bit, %lu passes, %lu bad (registers 0x%06lx), %lu.%02lu ms a pass\n",
               us, n, (unsigned long)bad, (unsigned long)badreg,
               (unsigned long)(ms / n), (unsigned long)((ms * 100u / n) % 100u));
}

/* DIAG:VI i v [cycles] (B21, power): can volts be read while current flows,
 * by switching rows in turn? DC current row i (0-5) and DC volts row v
 * (0-5), each with its own relays, as the app would alternate them: per
 * cycle, current (2 discarded, 3 averaged), volts, current again. Reports
 * every average, whether PA8 moves between the two rows (a click, and on
 * the 50 / 500 mA ranges a shunt tap changing under the load), and how far
 * the current moved across the volts read. Leaves the chip on DC volts 5 V
 * (safe with the VΩ lead on up to 30 V). Writes no flash. */
static bool vi_row_avg(const afe_row_t *row, bool dci, int r, double *avg)
{
    const meas_cal_t *cal = hal_platform()->cal;
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row->r20, sizeof row->r20);
    hal_afe_relays(row->pa8, row->pa13);
    double sum = 0;
    for (int k = 0; k < 5; k++) {
        uint32_t t0 = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - t0 < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("vi: no AD1 conversion in 1 s (INTF %02x)\n", intf); return false; }
        int32_t c = hy3131_read_ad1();
        if (k < 2) continue;
        sum += dci ? meas_dci(cal, r, c) : meas_dcv(cal, r, c);
    }
    *avg = sum / 3.0;
    return true;
}

static void afe_vi(const char *args)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    int ir = -1, vr = -1, cycles = 3;
    if (sscanf(args, "%d %d %d", &ir, &vr, &cycles) < 2 || ir < 0 || ir > AFE_DCI_LAST || vr < 0 || vr > 5) {
        log_puts("vi: DIAG:VI <current range 0-5> <volts range 0-5> [cycles 1-10]\n");
        return;
    }
    if (cycles < 1 || cycles > 10) cycles = 3;
    const afe_row_t *ri = &AFE_DCI[ir], *rv = &AFE_DCV[vr];
    log_printf("vi: DCI/%d (PA8=%d) with DCV/%d (PA8=%d)%s\n", ir, ri->pa8, vr, rv->pa8,
               ri->pa8 != rv->pa8 ? ": PA8 MOVES each switch (listen; watch the supply's current)" : ": no relay moves");
    double i0 = 0, v = 0, i1 = 0, worst = 0;
    for (int c = 0; c < cycles; c++) {
        uint32_t t0 = hal_millis();
        if (!vi_row_avg(ri, true, ir, &i0) || !vi_row_avg(rv, false, vr, &v) || !vi_row_avg(ri, true, ir, &i1)) break;
        double d = i0 != 0.0 ? (i1 - i0) / i0 * 100.0 : 0.0;
        if (fabs(d) > fabs(worst)) worst = d;
        log_printf("vi: cycle %d: I %.6g A, V %.6g V, I again %.6g A (%+.3f%%), P %.6g W, %lu ms\n",
                   c + 1, i0, v, i1, d, v * 0.5 * (i0 + i1), (unsigned long)(hal_millis() - t0));
    }
    log_printf("vi: worst current change across a volts read %+.3f%%\n", worst);
    dcv_configure(2);
    log_puts("vi: done, chip back on DC volts 5 V\n");
}

/* DIAG:CAPSLOPE r s (B16, 5/50 mF): the provisional CAP/r row (5 or 6), which
 * connects the ~0.18 mA source, then AD1 read continuously for s seconds
 * (max 120) and one line a second: time, counts, stock's v = raw / 2^23 x
 * 1.2 / 1.8. A discharged capacitor should ramp in a straight line; its
 * slope gives C = I / (dv/dt). Ends early at stock's over-range, v >= 0.6667,
 * and leaves the chip on DC volts (source off). No flash writes. */
static void afe_capslope(const char *args)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    int r = 6, secs = 30;
    char mode[8] = "";
    if (sscanf(args, "%d %d %7s", &r, &secs, mode) < 1 || (r != 5 && r != 6)) {
        log_puts("capslope: DIAG:CAPSLOPE <5|6> [seconds] [OFF|B]\n");
        return;
    }
    if (secs < 1 || secs > 120) secs = 30;
    const afe_prov_t *row = 0;
    for (int i = 0; i < AFE_NPROV; i++)
        if (AFE_PROV[i].fn == MEAS_CAP && AFE_PROV[i].range == r) row = &AFE_PROV[i];
    if (!row) { log_puts("capslope: no row\n"); return; }
    afe_prov_t rc = *row;
    bool off = strcmp(mode, "OFF") == 0;  /* stock's read state: R2A = R2C = 0 */
    if (off) { rc.r20[0x0A] = 0; rc.r20[0x0C] = 0; }
    /* Stock's other phase (0x08008098): R2F low nibble 6, R31 low nibble 1. */
    bool pb = strcmp(mode, "B") == 0;
    if (pb) { rc.r20[0x0F] = (uint8_t)((rc.r20[0x0F] & 0xF0u) | 6u); rc.r20[0x11] = (uint8_t)((rc.r20[0x11] & 0xF0u) | 1u); }
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, rc.r20, 24);
    hal_afe_relays(rc.pa8, rc.pa13);
    log_printf("capslope: CAP/%d (PROVISIONAL)%s, %d s\n", r, off ? ", source OFF (R2A=R2C=0)" : pb ? ", phase B (R2F/R31 low 6/1)" : "", secs);
    uint32_t t0 = hal_millis(), next = t0;
    double sum = 0;
    int n = 0;
    for (;;) {
        uint32_t tw = hal_millis();
        uint8_t intf = 0;
        while (!((intf = hy3131_read8(HY_INTF)) & HY_F_AD1) && hal_millis() - tw < 1000u) { }
        if (!(intf & HY_F_AD1)) { log_printf("capslope: no AD1 conversion in 1 s (INTF %02x)\n", intf); break; }
        int32_t c = hy3131_read_ad1();
        double v = (double)c / 8388608.0 * 1.2 / 1.8;
        sum += v; n++;
        uint32_t now = hal_millis();
        if (secs <= 5 || (int32_t)(now - next) >= 0) {   /* short runs: every conversion */
            log_printf("capslope: t=%lu ms counts=%ld v=%.6f (avg of %d)\n", (unsigned long)(now - t0), (long)c, sum / n, n);
            sum = 0; n = 0;
            next += 1000u;
            if (v >= 0.666666) { log_puts("capslope: over-range, stopping\n"); break; }
        }
        if (now - t0 >= (uint32_t)secs * 1000u) break;
    }
    dcv_configure(2);                     /* source off: the chip back on DC volts */
    log_puts("capslope: done, chip back on DC volts\n");
}

static void afe_row_diag(char *args)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    char name[8] = "";
    int r = 0, n = 6;
    if (sscanf(args, "%7s %d %d", name, &r, &n) < 2) {
        log_puts("row: DIAG:ROW <DCV|ACV|DCI|ACI|OHM|CONT|DIODE|CAP|FREQ|TEMP> <range> [n]\n");
        return;
    }
    int fn = -1;
    for (int i = 0; i < MEAS_NFUNC; i++) if (strcmp(name, ROW_FN[i]) == 0) fn = i;
    const afe_prov_t *row = 0;
    for (int i = 0; i < AFE_NPROV; i++)
        if (AFE_PROV[i].fn == fn && AFE_PROV[i].range == r) row = &AFE_PROV[i];
    if (fn < 0 || !row || r >= meas_nranges((meas_fn_t)fn)) { log_puts("row: no such function/range\n"); return; }
    if (n < 1 || n > 20) n = 6;
    /* Optional RR=HH overrides after n (registers 20-36 hex; 37 is test
     * mode and refused), as DIAG:CAPX: DIAG:ROW ACV 2 10 26=80. Relays too:
     * PA8=0|1, PA13=0|1 (current's shunt taps, 2026-11-14). */
    afe_prov_t rowc = *row;
    for (char *t = strchr(args, '='); t; t = strchr(t + 1, '=')) {
        char *st = t;
        while (st > args && st[-1] != ' ') st--;
        if (strncmp(st, "PA8=", 4) == 0 || strncmp(st, "PA13=", 5) == 0) {
            bool on = t[1] == '1';
            if (st[3] == '=') rowc.pa8 = on; else rowc.pa13 = on;
            log_printf("row: %.*s = %d\n", (int)(t - st), st, on);
            continue;
        }
        unsigned long reg = strtoul(st, NULL, 16), val = strtoul(t + 1, NULL, 16);
        if (reg < 0x20 || reg > 0x36 || val > 0xFF) { log_puts("row: overrides RR=HH, RR 20-36 hex\n"); return; }
        rowc.r20[reg - 0x20] = (uint8_t)val;
        log_printf("row: R%02lX = %02lX\n", reg, val);
    }
    row = &rowc;
    hy3131_write8(HY_R20, 0x68);
    hy3131_write_burst(HY_R20, row->r20, 24);
    hal_afe_relays(row->pa8, row->pa13);
    uint8_t back[24];
    hy3131_read_burst(HY_R20, back, sizeof back);
    int bad = 0;
    back[0x14] &= 0xFEu;                   /* R34 bit 0 reads 1 at times: not ours (datasheet "O") */
    for (unsigned i = 0; i < sizeof back; i++) bad += back[i] != row->r20[i];
    log_printf("row: %s/%d (PROVISIONAL) PA8=%d PA13=%d; read back %s\n", ROW_FN[fn], r, row->pa8, row->pa13,
               bad ? "DIFFERS" : "identical");
    for (unsigned i = 0; i < sizeof back; i++)
        if (back[i] != row->r20[i]) log_printf("row: R%02X wrote %02X read %02X\n", 0x20 + i, row->r20[i], back[i]);
    if (bad) return;
    const meas_cal_t *cal = hal_platform()->cal;
    bool ac = fn == MEAS_ACV || fn == MEAS_ACI, ct = fn == MEAS_FREQ || fn == MEAS_CAP;
    uint8_t flag = ac ? HY_F_RMS : ct ? HY_F_CT : HY_F_AD1;
    static const uint8_t gate[3] = { 0x00, 0x00, 0xf0 };
    const uint8_t *preset = fn == MEAS_CAP ? (const uint8_t[]){ 0x00, 0x00, 0xbf } : gate;
    uint32_t cta0 = 0;
    if (ct) cta0 = row_ct_restart(row->r20[0], preset);
    for (int k = 0; k < n + 3; k++) {
        uint32_t t0 = hal_millis(), tick = t0, cta_prev = 0xFFFFFFFFu;
        uint8_t intf = 0;
        bool stopped = false;
        /* Counters: the done flag is sometimes never seen (capacitance,
         * 2026-11-13), so also stop when CTA has not moved for 100 ms. */
        while (!((intf = hy3131_read8(HY_INTF)) & flag) && !stopped && hal_millis() - t0 < 6000u) {
            if (ct && hal_millis() - tick >= 100u) {
                uint8_t c[6];
                tick = hal_millis();
                hy3131_read_burst(HY_CTB, c, 6);
                stopped = le24_(c + 3) == cta_prev && le24_(c) > 0;
                cta_prev = le24_(c + 3);
            }
        }
        if (!(intf & flag) && !stopped) { log_printf("row: no result in 6 s (INTF %02x)\n", intf); return; }
        uint32_t dt_ms = hal_millis() - t0;
        if (ac) {
            uint8_t b[5];
            hy3131_read_burst(HY_RMS_DATA, b, 5);
            uint64_t rms = 0;
            for (int i = 4; i >= 0; i--) rms = rms << 8 | b[i];
            if (k >= 3) log_printf("row %s/%d: %lums RMS %.0f = %.6g (factory cal)\n", ROW_FN[fn], r,
                                   (unsigned long)dt_ms, (double)rms,
                                   meas_ac(cal, (meas_fn_t)fn, r, rms, false));
        } else if (ct) {
            uint8_t c[9];
            hy3131_read_burst(HY_CTC, c, 9);
            uint32_t ctc = le24_(c), ctb = le24_(c + 3), cta = le24_(c + 6);
            uint32_t ticks = 0x1000000u - cta0 + cta;   /* datasheet: the whole counting time */
            (void)hy3131_read8(HY_INTF);                 /* drop a late flag */
            cta0 = row_ct_restart(row->r20[0], preset);
            double duty = 0, v = fn == MEAS_FREQ ? meas_freq(cta, ctb, ctc, &duty)
                                                 : meas_cap_counter(cal, r, ctb, ctc);
            log_printf("row %s/%d: %lums%s CTA %06lx CTB %lu CTC %lu ticks %lu = %.6g%s duty %.1f%%\n", ROW_FN[fn], r,
                       (unsigned long)dt_ms, stopped ? " (flag missed)" : "", (unsigned long)cta,
                       (unsigned long)ctb, (unsigned long)ctc, (unsigned long)ticks,
                       v, fn == MEAS_FREQ ? " Hz" : " F", duty);
        } else {
            int32_t c = hy3131_read_ad1();
            double v;
            switch (fn) {
            case MEAS_DCV:   v = meas_dcv(cal, r, c); break;
            case MEAS_DCI:   v = meas_dci(cal, r, c); break;
            case MEAS_OHM2: case MEAS_OHM4: v = meas_ohm(cal, (meas_fn_t)fn, r, c, false); break;
            case MEAS_CONT:  v = meas_continuity(cal, c); break;
            case MEAS_DIODE: v = meas_diode(cal, c); break;
            default:         v = meas_dcv(cal, 1, c) * 1e3; break;   /* TEMP: EMF, mV */
            }
            if (k >= 3) log_printf("row %s/%d: %lums AD1 %ld = %.6g (factory cal)%s\n", ROW_FN[fn], r,
                                   (unsigned long)dt_ms, (long)c, v,
                                   c >= 8388606 || c <= -8388607 ? "  OVER-RANGE" : "");
        }
    }
}

static void afe_dcv_test(int r)
{
    if (!afe_bus_ok) { log_puts("afe: pin check failed at boot: not configuring\n"); return; }
    if (r < 0 || r > 5) { log_puts("dcv: range 0-5 (50 mV .. 1000 V)\n"); return; }
    double v;
    if (r <= 1) {                                  /* direct input: check first */
        if (!dcv_configure(2) || !dcv_read(2, 1, false, &v)) return;
        if (v > 0.4 || v < -0.4) {
            log_printf("dcv: input reads %.4g V on 5 V: too high for %s, not switching\n", v, DCV_NAME[r]);
            return;
        }
    }
    bool ok = dcv_configure(r);
    log_printf("dcv: configured DCV %s; read back %s\n", DCV_NAME[r], ok ? "identical" : "DIFFERS");
    if (ok) dcv_read(r, 6, true, &v);
}
#endif

static void console_command(char *line)
{
    for (char *p = line; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    if (strcmp(line, "*IDN?") == 0) {
        static const char idn[] = "OpenOWON,serial-console," FW_VERSION "\r\n";
        hal_console_write(idn, sizeof idn - 1);
    } else if (strcmp(line, "DIAG:BOOT?") == 0) {
        flashdump_emit((const uint8_t *)BOOT_BASE, BOOT_BASE, BOOT_LEN, console_out, NULL);
    } else if (hal_lcd_enabled() && strncmp(line, "DIAG:FILL ", 10) == 0) {
        unsigned long c = strtoul(line + 10, NULL, 16);
        lcd_fill_rect(0, 0, PANEL_W, PANEL_H, (uint16_t)c);
        log_printf("filled 0x%04lx\n", c & 0xFFFFu);
#if defined(MAIN_AFE_DIAG)
    } else if (strcmp(line, "DIAG:AFE?") == 0) {
        afe_dump();
    } else if (strcmp(line, "DIAG:CONT") == 0) {
        afe_row_test("cont", &AFE_CONT, "ohm", conv_cont);
    } else if (strcmp(line, "DIAG:DIODE") == 0) {
        afe_row_test("diode", &AFE_DIODE, "V", conv_diode);
    } else if (strcmp(line, "DIAG:TEMP") == 0) {
        afe_temp_test();
    } else if (strncmp(line, "DIAG:ROW ", 9) == 0) {
        afe_row_diag(line + 9);
    } else if (strncmp(line, "DIAG:BUS ", 9) == 0) {
        afe_bus_test(line + 9);
    } else if (strncmp(line, "DIAG:CAPRB ", 11) == 0) {
        cap_rb = (unsigned)strtoul(line + 11, NULL, 0) & 3u;
        log_printf("caprb: %u (bit 0 CTA read after start, bit 1 at the end)\n", cap_rb);
    } else if (strncmp(line, "DIAG:VI ", 8) == 0) {
        afe_vi(line + 8);
    } else if (strncmp(line, "DIAG:CAPSLOPE ", 14) == 0) {
        afe_capslope(line + 14);
    } else if (strncmp(line, "DIAG:CAPX ", 10) == 0) {
        afe_capx(line + 10);
    } else if (strncmp(line, "DIAG:CAP", 8) == 0 && (line[8] == '\0' || line[8] == ' ')) {
        afe_cap_test(line[8] ? atoi(line + 9) : 3);
    } else if (strncmp(line, "DIAG:OHM", 8) == 0 && (line[8] == '\0' || line[8] == ' ')) {
        afe_ohm_test(line[8] ? atoi(line + 9) : 1);
    } else if (strncmp(line, "DIAG:DCV", 8) == 0 && (line[8] == '\0' || line[8] == ' ')) {
        afe_dcv_test(line[8] ? atoi(line + 9) : 2);
#endif
#if defined(HAL_BACKLIGHT_TEST)
    } else if (strncmp(line, "DIAG:BL ", 8) == 0) {
        unsigned long pct = strtoul(line + 8, NULL, 10);
        if (pct > 100u) pct = 100u;
        hal_backlight((uint8_t)pct);
        log_printf("backlight: %lu%% (PA11 PWM)\n", pct);
#endif
#if defined(HAL_BEEPER_TEST)
    } else if (strncmp(line, "DIAG:TONE ", 10) == 0) {
        char *e;
        unsigned long hz = strtoul(line + 10, &e, 10);
        unsigned long lv = strtoul(e, &e, 10);
        unsigned long ms = strtoul(e, NULL, 10);
        if (hz < 200u || hz > 8000u || lv < 1u || lv > 100u || ms > 3000u) {
            log_puts("tone: DIAG:TONE <200-8000 Hz> <1-100> <0-3000 ms>\n");
        } else {
            hal_tone((uint16_t)hz, (uint8_t)lv);
            uint32_t t0 = hal_millis();
            while (hal_millis() - t0 < ms) { }
            hal_tone((uint16_t)hz, 0);
            log_printf("tone: %lu Hz, level %lu, %lu ms\n", hz, lv, ms);
        }
    } else if (strncmp(line, "DIAG:BEEP ", 10) == 0) {
        unsigned long ms = strtoul(line + 10, NULL, 10);
        if (ms > 2000u) ms = 2000u;
        hal_beep(true);
        uint32_t t0 = hal_millis();
        while (hal_millis() - t0 < ms) { }
        hal_beep(false);
        log_printf("beep %lu ms\n", ms);
#endif
    } else if (hal_lcd_enabled() && strncmp(line, "DIAG:MADCTL ", 12) == 0) {
        unsigned long v = strtoul(line + 12, NULL, 16) & 0xFFu;
        panel_madctl((uint8_t)v);
        panel_pattern();
    } else if (hal_lcd_enabled() && strcmp(line, "DIAG:PATTERN") == 0) {
        panel_pattern();
    } else if (line[0]) {
        static const char err[] = "ERR unknown command\r\n";
        hal_console_write(err, sizeof err - 1);
    }
}

static void console_poll(void)
{
    static char line[48];
    static size_t len;
    char ch;
    while (hal_console_read(&ch, 1) == 1) {
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            console_command(line);
            len = 0;
        } else if (len < sizeof line - 1) {
            line[len++] = ch;
        }
    }
}

int main(void)
{
    hal_early_init();
    hal_console_init();

    log_printf("\nXDM1241 firmware %s\n", FW_VERSION);
    log_printf("clock %lu Hz%s\n", (unsigned long)SystemCoreClock,
               hal_clock_ok() ? "" : "  (PLL did not lock: running on IRC8M)");
    log_printf("vector table 0x%08lx\n", (unsigned long)SCB->VTOR);

    int bad = hal_load_calibration();
    if (bad == 0) log_puts("calibration: factory record OK, in use\n");
    else log_printf("calibration: %d slots implausible, using unity gain\n", bad);

    log_printf("pins: panel %s, HY3131 %s\n",
               hal_lcd_enabled() ? "ENABLED" : "off",
               hal_afe_enabled() ? "ENABLED" : hal_afe_simulated() ? "SIMULATED" : "off");

    bool run_app = hal_lcd_enabled() && (hal_afe_enabled() || hal_afe_simulated());
    if (run_app) {
        hal_sim_init();
        hal_board_init();
        app_init(&app, hal_platform());
        if (hal_afe_simulated())
            log_printf("afe: DC V, ohms, continuity, diode %s; other functions simulated\n",
                       hal_afe_dcv_real() ? "on the REAL HY3131" : "simulated (pin check failed or not built in)");
        log_puts("app running\n");
    } else if (hal_lcd_enabled()) {
        hal_panel_init();
        log_puts("panel test build: CS low, DISPON sent\n");
        panel_pattern();
#if defined(HAL_KEYPAD_TEST)
        keys_stuck = hal_keypad_init();
        if (keys_stuck)
            log_printf("keypad: lines 0x%03x low with nothing driven: NOT scanning\n", keys_stuck);
        else
            log_puts("keypad: all 10 lines idle high; scanning every 20 ms\n");
#endif
#if defined(MAIN_AFE_DIAG)
        {
            char st[6] = { 0 };
            afe_bus_ok = hal_afe_probe_pins(st) != 0;
            log_printf("afe pins (U undriven, H/L held): CS=%c CK=%c MISO=%c MOSI=%c PA8=%c -> %s\n",
                       st[0], st[1], st[2], st[3], st[4],
                       afe_bus_ok ? "as expected, bus ready (DIAG:AFE?)" : "NOT as expected: bus left alone");
            if (afe_bus_ok) hal_afe_probe_bus();
        }
#endif
#if defined(HAL_BEEPER_TEST)
        hal_beeper_init();
        log_puts("beeper: PB11 PWM ready (DIAG:BEEP <ms>), silent\n");
#endif
    } else {
        log_puts("pin-free build: heartbeat only\n");
    }

    uint32_t last = hal_millis();
    uint32_t steps = 0, step_max = 0, sim_max = 0;     /* app timing, per second */
    for (;;) {
        if (run_app) {
            uint32_t t0 = hal_millis();
            hal_sim_tick(t0);
            uint32_t t1 = hal_millis();
            app_step(&app);
            uint32_t t2 = hal_millis();
            if (t1 - t0 > sim_max) sim_max = t1 - t0;
            if (t2 - t1 > step_max) step_max = t2 - t1;
            steps++;
        }
        else console_poll();
#if defined(HAL_KEYPAD_TEST)
        if (!run_app && hal_lcd_enabled() && !keys_stuck) keys_poll();
#endif
        uint32_t now = hal_millis();
        if (now - last >= 1000u) {
            last = now;
            heartbeat++;
            if (run_app) {
                static uint32_t sent_before, drawn_before, skipped_before, samples_before;
                log_printf("t=%lus steps=%lu step_max=%lums sim_max=%lums bands=%lu frames=%lu skipped=%lu samples=%lu\n",
                           (unsigned long)(now / 1000u), (unsigned long)steps,
                           (unsigned long)step_max, (unsigned long)sim_max,
                           (unsigned long)(app.bands_sent - sent_before),
                           (unsigned long)(app.frames_drawn - drawn_before),
                           (unsigned long)(app.frames_skipped - skipped_before),
                           (unsigned long)(app.samples - samples_before));
                samples_before = app.samples;
                sent_before = app.bands_sent;
                drawn_before = app.frames_drawn;
                skipped_before = app.frames_skipped;
                steps = step_max = sim_max = 0;
            } else {
                log_printf("t=%lus heartbeat=%lu\n", (unsigned long)(now / 1000u),
                           (unsigned long)heartbeat);
            }
        }
    }
}
