/* HOST ONLY. See sim_board.h. */
#include "sim_board.h"

#include <stdio.h>
#include <string.h>

#include "meas_temp.h"

#define KEYQ 32

static meas_cal_t cal;
static uint32_t   now_ms;
static app_key_t  keyq[KEYQ];
static int        kq_head, kq_tail;
static bool       beeping;
static unsigned   beep_onsets;
static uint16_t   tone_hz;
static uint8_t    tone_level;
static uint8_t    backlight;
static rgb565_t   band[SCR_W * SIM_BAND_ROWS];

/* serial: rx = host to meter, tx = meter to host */
static char   rx[512], tx[2048];
static size_t rx_len, rx_pos, tx_len;

static size_t b_serial_read(char *buf, size_t max)
{
    size_t n = rx_len - rx_pos;
    if (n > max) n = max;
    memcpy(buf, rx + rx_pos, n);
    rx_pos += n;
    if (rx_pos == rx_len) rx_pos = rx_len = 0;
    return n;
}

static void b_serial_write(const char *s, size_t n)
{
    if (tx_len + n > sizeof tx) n = sizeof tx - tx_len;
    memcpy(tx + tx_len, s, n);
    tx_len += n;
}

void sim_board_serial_send(const char *line)
{
    size_t n = strlen(line);
    if (rx_len + n + 1 > sizeof rx) return;
    memcpy(rx + rx_len, line, n);
    rx[rx_len + n] = '\n';
    rx_len += n + 1;
}

bool sim_board_serial_take(char *line, size_t cap)
{
    char *nl = memchr(tx, '\n', tx_len);
    if (!nl) return false;
    size_t n = (size_t)(nl - tx);
    size_t len = n && tx[n - 1] == '\r' ? n - 1 : n;       /* lines end "\r\n", as stock */
    snprintf(line, cap, "%.*s", (int)len, tx);
    memmove(tx, nl + 1, tx_len - n - 1);
    tx_len -= n + 1;
    return true;
}

static uint32_t  b_millis(void) { return now_ms; }
static void      b_beep(bool on) { if (on && !beeping) beep_onsets++; beeping = on; }
static void      b_tone(uint16_t hz, uint8_t level)
{
    b_beep(level > 0);
    tone_hz = hz;
    tone_level = level;
}
static void      b_backlight(uint8_t pct) { backlight = pct; }
static int       battery = -1;               /* mV; -1 = the stock board, no sense */
static uint32_t  baud;
static int       b_cal_status(void) { return 0; }
static int       b_battery(void) { return battery; }
static void      b_baud(uint32_t b) { baud = b; }

static app_key_t b_key(void)
{
    if (kq_head == kq_tail) return KEY_NONE;
    app_key_t k = keyq[kq_tail];
    kq_tail = (kq_tail + 1) % KEYQ;
    return k;
}

static bool (*avail_hook)(meas_fn_t, int);
static bool b_range_avail(meas_fn_t fn, int range) { return avail_hook ? avail_hook(fn, range) : true; }
void sim_board_set_range_avail(bool (*f)(meas_fn_t, int)) { avail_hook = f; }
/* B33: a settable clock that runs with virtual time (0 = not set). */
static uint32_t clk_base, clk_set_at;
static uint32_t b_clock_now(void) { return clk_base ? clk_base + (b_millis() - clk_set_at) / 1000u : 0; }
static void b_clock_set(uint32_t t) { clk_base = t; clk_set_at = b_millis(); }
void sim_board_clock_reset(void) { clk_base = 0; clk_set_at = 0; }

static bool (*fn_hook)(meas_fn_t);
static bool b_fn_avail(meas_fn_t fn) { return fn_hook ? fn_hook(fn) : true; }
void sim_board_set_fn_avail(bool (*f)(meas_fn_t)) { fn_hook = f; }
static bool (*vi_hook)(int, int);
static bool vi_off;
static bool b_vi_avail(int ir, int vr) { return !vi_off && (vi_hook ? vi_hook(ir, vr) : true); }
void sim_board_set_vi_avail(bool (*f)(int, int), bool off) { vi_hook = f; vi_off = off; }

static const platform_t board = {
    .range_avail = b_range_avail,
    .fn_avail = b_fn_avail,
    .vi_avail = b_vi_avail,
    .afe_bus = &sim_hy3131_io,
    .afe_configure = sim_hy3131_configure,
    .lcd = &sim_lcd_io,
    .millis = b_millis,
    .key_poll = b_key,
    .beep = b_beep,
    .tone = b_tone,
    .backlight = b_backlight,
    .afe_rate = sim_hy3131_set_rate,
    .clock_now = b_clock_now,
    .clock_set = b_clock_set,
    .cal = &cal,
    .cal_status = b_cal_status,
    .battery_mv = b_battery,
    .store = &sim_flash_io,
    .log_flash = &sim_flash_io,
    .ucal_flash = &sim_flash_io,
    .serial_read = b_serial_read,
    .serial_write = b_serial_write,
    .serial_number = "SIM0001",
    .serial_baud = b_baud,
    .band = band,
    .band_rows = SIM_BAND_ROWS,
};

void sim_board_reboot(void)
{
    if (clk_base) { clk_base = b_clock_now(); clk_set_at = 0; }   /* the RTC runs on its cell */
    rx_len = rx_pos = tx_len = 0;
    now_ms = 0;
    kq_head = kq_tail = 0;
    beeping = false;
    beep_onsets = 0;
    avail_hook = 0;
    vi_hook = 0; vi_off = false;
    tone_hz = 0;
    tone_level = 0;
    backlight = 0;
    baud = 0;
    sim_hy3131_reset();
    sim_hy3131_set_cj_gain(meas_cj_gain(cal.cj_g));
    sim_lcd_reset();
}

void sim_board_init(const meas_cal_t *c)
{
    if (c) cal = *c; else meas_cal_defaults(&cal);
    sim_board_clock_reset();
    sim_flash_reset();
    battery = -1;
    rx_len = rx_pos = tx_len = 0;
    now_ms = 0;
    kq_head = kq_tail = 0;
    beeping = false;
    beep_onsets = 0;
    avail_hook = 0;
    vi_hook = 0; vi_off = false;
    tone_hz = 0;
    tone_level = 0;
    backlight = 0;
    baud = 0;
    sim_hy3131_reset();
    sim_hy3131_set_cj_gain(meas_cj_gain(cal.cj_g));
    sim_lcd_reset();
}

const platform_t *sim_board_platform(void) { return &board; }

void sim_board_press(app_key_t k)
{
    int next = (kq_head + 1) % KEYQ;
    if (next == kq_tail) return;
    keyq[kq_head] = k;
    kq_head = next;
}

void sim_board_run(app_t *a, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        now_ms++;
        sim_hy3131_tick_ms();
        app_step(a);
    }
}

uint32_t sim_board_millis(void) { return now_ms; }
bool     sim_board_beeping(void) { return beeping; }
unsigned sim_board_beeps(void) { return beep_onsets; }
uint16_t sim_board_tone_hz(void) { return tone_hz; }
uint8_t  sim_board_tone_level(void) { return tone_level; }
uint8_t  sim_board_backlight(void) { return backlight; }

void     sim_board_set_battery_mv(int mv) { battery = mv; }
uint32_t sim_board_baud(void) { return baud; }
