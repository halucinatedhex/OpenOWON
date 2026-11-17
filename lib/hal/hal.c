/* TARGET ONLY. GD32F303 hardware layer. See hal.h, hal_pins.h, docs/boot.md. */
#include "hal.h"

#include <stdio.h>
#include <string.h>

#include "gd32f30x.h"
#include "hal_pins.h"
#include "hy3131.h"
#include "lcd.h"
#include "afe_table.h"
#if defined(HAL_SIM_AFE)
#include "sim_hy3131.h"
#include "meas_temp.h"
#endif
#include "keypad.h"
#include "meas.h"
#include "rtt.h"
#include "ui.h"
#include "datalog.h"
#include "ucal.h"

/* From linker/app.ld */
extern uint32_t _app_start;

/* Factory record (docs/settings-storage.md). Read-only, always. */
#define FACTORY_RECORD_ADDR 0x0803F800u

static volatile uint32_t ms;

void SysTick_Handler(void) { ms++; }

uint32_t hal_millis(void) { return ms; }

static bool clock_ok;

/* 72 MHz from the internal 8 MHz oscillator: (IRC8M / 2) x 18.
 *
 * SystemInit (framework) is built with __SYSTEM_CLOCK_IRC8M, so it leaves the
 * core on plain IRC8M at 8 MHz; the PLL is brought up here so the wait states
 * can be set first, in the same order as stock (stock_SetSysClock, latency 2).
 * The external crystal is never used: it may not be fitted on v3.x boards,
 * and the framework's crystal path spins forever if it does not start.
 * Every wait is bounded; on failure the core stays at 8 MHz. */
static bool clock_72m_irc8m(void)
{
    fmc_wscnt_set(WS_WSCNT_2);

    RCU_APB1EN |= RCU_APB1EN_PMUEN;
    PMU_CTL |= PMU_CTL_LDOVS;
    RCU_CFG0 |= RCU_AHB_CKSYS_DIV1 | RCU_APB2_CKAHB_DIV1 | RCU_APB1_CKAHB_DIV2;
    RCU_CFG0 &= ~(RCU_CFG0_PLLSEL | RCU_CFG0_PLLMF | RCU_CFG0_PLLMF_4 | RCU_CFG0_PLLMF_5);
    RCU_CFG0 |= RCU_PLL_MUL18;                 /* PLLSEL = 0: IRC8M / 2 */
    RCU_CTL |= RCU_CTL_PLLEN;

    for (uint32_t t = 0; !(RCU_CTL & RCU_CTL_PLLSTB); t++)
        if (t > 100000u) return false;

    RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_SCS) | RCU_CKSYSSRC_PLL;
    for (uint32_t t = 0; (RCU_CFG0 & RCU_SCSS_PLL) != RCU_SCSS_PLL; t++)
        if (t > 100000u) return false;

    SystemCoreClock = 72000000u;
    return true;
}

void hal_early_init(void)
{
    clock_ok = clock_72m_irc8m();

    /* Our own vector table. Stock leaves VTOR at the bootloader's table and
     * relies on it forwarding; we do not. Must precede any interrupt. */
    __disable_irq();
    SCB->VTOR = (uint32_t)&_app_start;
    __DSB();
    __ISB();
    SysTick_Config(SystemCoreClock / 1000u);
    __enable_irq();
    rtt_init();
}

bool hal_clock_ok(void) { return clock_ok; }
bool hal_lcd_enabled(void) { return HAL_LCD_ENABLED; }
bool hal_afe_enabled(void) { return HAL_AFE_ENABLED; }

/* ---- panel: 8080 bus ----------------------------------------------------- */

#if HAL_LCD_ENABLED
static void lcd_wr(uint8_t b)
{
    /* D0-D7 on PA0-PA7 in one write: set the ones, clear the zeros, leaving
     * PA8-PA15 alone. Then a WR pulse; the panel latches on the rising edge. */
    GPIO_BOP(PIN_LCD_DATA_PORT) = (uint32_t)b | ((uint32_t)(uint8_t)~b << 16);
    GPIO_BC(PIN_LCD_CTL_PORT) = PIN_LCD_WR;
    __NOP(); __NOP();
    GPIO_BOP(PIN_LCD_CTL_PORT) = PIN_LCD_WR;
}

static void lcd_dc(bool data)
{
    if (data) GPIO_BOP(PIN_LCD_CTL_PORT) = PIN_LCD_DC;
    else      GPIO_BC(PIN_LCD_CTL_PORT) = PIN_LCD_DC;
}
#else
static void lcd_wr(uint8_t b) { (void)b; }
static void lcd_dc(bool data) { (void)data; }
#endif

static const uint8_t dcs_map[LCD_DCS_MAP_LEN] = LCD_DCS_MAP;
static const lcd_io_t lcd_io = {
    .wr_byte = lcd_wr, .dc = lcd_dc, .cmd_map = dcs_map, .cmd_map_len = LCD_DCS_MAP_LEN,
};

static void lcd_board_init(void)
{
#if HAL_LCD_ENABLED
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    GPIO_BOP(PIN_LCD_CTL_PORT) = PIN_LCD_WR;                /* WR idles high */
    gpio_init(PIN_LCD_DATA_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, 0x00FFu);
    gpio_init(PIN_LCD_CTL_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ,
              PIN_LCD_DC | PIN_LCD_WR | PIN_LCD_CS);
    /* Select the panel. The bootloader's last act before jumping here is to
     * release it (PB1 high, 0x0800197C(0)), so without this every write is
     * ignored. Stock keeps it selected the same way. */
    GPIO_BC(PIN_LCD_CTL_PORT) = PIN_LCD_CS;
    /* No reset pulse: the bootloader initialised the panel and a reset would
     * undo it (there is no init sequence in the app image; docs/display.md).
     * DISPON is index 0x11 in the stock command table. */
    lcd_init(&lcd_io);
    lcd_cmd(0x11);
    /* Landscape, origin top left, as the meter is held. The bootloader
     * leaves MADCTL = 0x08 (portrait, origin top right); MV | BGR = 0x28
     * confirmed on our meter 2026-11-11 (docs/display.md). DCS 0x36 isn't in
     * the stock index table, so it goes out unchanged. */
    lcd_cmd(0x36);
    lcd_data(0x28);
#endif
}

/* ---- HY3131: bit-banged SPI, mode 0 ----------------------------------------- */

#if HAL_AFE_ENABLED || defined(HAL_AFE_PROBE)
static void afe_cs(bool asserted)
{
    if (asserted) GPIO_BC(PIN_AFE_PORT) = PIN_AFE_CS;      /* active low */
    else          GPIO_BOP(PIN_AFE_PORT) = PIN_AFE_CS;
}
static void afe_clk(bool l)  { if (l) GPIO_BOP(PIN_AFE_PORT) = PIN_AFE_CK;  else GPIO_BC(PIN_AFE_PORT) = PIN_AFE_CK; }
static void afe_mosi(bool l) { if (l) GPIO_BOP(PIN_AFE_PORT) = PIN_AFE_MOSI; else GPIO_BC(PIN_AFE_PORT) = PIN_AFE_MOSI; }
static bool afe_miso(void)   { return (GPIO_ISTAT(PIN_AFE_PORT) & PIN_AFE_MISO) != 0; }
#else
static void afe_cs(bool a)   { (void)a; }
static void afe_clk(bool l)  { (void)l; }
static void afe_mosi(bool l) { (void)l; }
static bool afe_miso(void)   { return false; }
#endif

/* Half a bit period, timed with the DWT cycle counter. The bus crosses an
 * isolation barrier: on the XDM2041 the SCK->MISO path goes through
 * optocouplers twice (~50 us), and stock clocks it at 2.6 kHz
 * (owon-xdm2041-info, spi_captures). Start at stock's rate, which is known to
 * work, and raise it on the bench (HAL_AFE_HALF_BIT_US, hal_pins.h). */
static uint32_t afe_half_us = HAL_AFE_HALF_BIT_US;   /* DIAG:BUS tries others */

void hal_afe_set_half_bit_us(uint32_t us) { afe_half_us = us ? us : 1u; }
uint32_t hal_afe_half_bit_us(void)        { return afe_half_us; }

static void afe_delay(void)
{
    uint32_t t0 = DWT->CYCCNT, n = afe_half_us * (SystemCoreClock / 1000000u);
    while (DWT->CYCCNT - t0 < n) { }
}

static const hy3131_io_t afe_io = {
    .cs = afe_cs, .clk = afe_clk, .mosi = afe_mosi, .miso = afe_miso, .delay = afe_delay,
};

static void afe_board_init(void)
{
#if HAL_AFE_ENABLED || defined(HAL_AFE_PROBE)
    rcu_periph_clock_enable(RCU_GPIOB);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;          /* cycle counter for afe_delay */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    GPIO_BOP(PIN_AFE_PORT) = PIN_AFE_CS;                     /* deselected */
    gpio_init(PIN_AFE_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ,
              PIN_AFE_CS | PIN_AFE_CK | PIN_AFE_MOSI);
    gpio_init(PIN_AFE_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_AFE_MISO);
#endif
}

/* ---- front-end probe (HAL_AFE_PROBE): read-only bring-up ---------------------- */

#if defined(HAL_AFE_PROBE)
/* One pin, nothing driven: read it with a pull-up, then a pull-down.
 * Returns 'U' if it follows the pulls (undriven), 'H'/'L' if something
 * holds it there. */
static char pin_state(uint32_t port, uint32_t pin)
{
    gpio_init(port, GPIO_MODE_IPU, GPIO_OSPEED_2MHZ, pin);
    for (volatile int n = 0; n < 2000; n++) { }
    bool up = (GPIO_ISTAT(port) & pin) != 0;
    gpio_init(port, GPIO_MODE_IPD, GPIO_OSPEED_2MHZ, pin);
    for (volatile int n = 0; n < 2000; n++) { }
    bool down = (GPIO_ISTAT(port) & pin) != 0;
    gpio_init(port, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_2MHZ, pin);
    if (up && !down) return 'U';
    return up ? 'H' : 'L';
}

int hal_afe_probe_pins(char out[5])
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    out[0] = pin_state(GPIOB, PIN_AFE_CS);
    out[1] = pin_state(GPIOB, PIN_AFE_CK);
    out[2] = pin_state(GPIOB, PIN_AFE_MISO);
    out[3] = pin_state(GPIOB, PIN_AFE_MOSI);
    out[4] = pin_state(GPIOA, GPIO_PIN_8);
    /* As expected: CS, CK, MOSI feed isolator inputs (undriven); MISO is an
     * isolator output (driven). Only then is the bus worth driving. */
    return out[0] == 'U' && out[1] == 'U' && out[3] == 'U' && out[2] != 'U';
}

void hal_afe_probe_bus(void)
{
    afe_board_init();
    hy3131_init(&afe_io);
}

/* The two relay lines (docs/afe-table.md: PA8 and PA13, through the ELD207
 * opto). Recovered from v2.1.2 by emulation; 1 = relay energised. PA13 is
 * SWDIO: SWJ is already off (the keypad needs PA14/PA15/PB3/PB4). */
static bool relays_driven;    /* PA8/PA13 configured as outputs */

void hal_afe_relays_release(void)
{
    /* Both off. Until hal_afe_relays has run the pins are still inputs, and
     * the external pull-downs already hold the relays off. */
    if (relays_driven) GPIO_BC(GPIOA) = GPIO_PIN_8 | GPIO_PIN_13;
}

void hal_afe_relays(bool pa8, bool pa13)
{
    relays_driven = true;
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);
    gpio_pin_remap_config(GPIO_SWJ_DISABLE_REMAP, ENABLE);
    if (pa8) GPIO_BOP(GPIOA) = GPIO_PIN_8;  else GPIO_BC(GPIOA) = GPIO_PIN_8;
    if (pa13) GPIO_BOP(GPIOA) = GPIO_PIN_13; else GPIO_BC(GPIOA) = GPIO_PIN_13;
    gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_8 | GPIO_PIN_13);
}
#endif

/* The per-(function, range) HY3131 register and relay settings are captured
 * from stock on the bench (docs/boot.md). Until then this only logs. */
static void afe_configure(meas_fn_t fn, int range, afe_aux_t aux)
{
    static int last = -1;
    int key = (int)fn * 100 + range * 10 + (int)aux;
    if (key != last) {
        rtt_printf("afe_configure fn=%d range=%d aux=%d: no captured table yet\n",
                   (int)fn, range, (int)aux);
        last = key;
    }
}

/* ---- DC V on the real front end, the rest simulated (HAL_AFE_DCV_REAL) -------- */

#if defined(HAL_AFE_DCV_REAL)
/* The xdm1241-ui build with HAL_AFE_DCV_REAL: the functions confirmed on our
 * meter (afe_table.h: DC V, 2-wire ohms, continuity, diode) go to the real HY3131; the rest to lib/sim's
 * model, with both relays released. The driver holds one bus, so this bus
 * forwards each call to whichever side is selected. */
static bool real_ok;          /* passive pin check passed at boot */
static bool use_real;

static void hy_cs(bool a)    { if (use_real) afe_cs(a);    else sim_hy3131_io.cs(a); }
static void hy_clk(bool l)   { if (use_real) afe_clk(l);   else sim_hy3131_io.clk(l); }
static void hy_mosi(bool l)  { if (use_real) afe_mosi(l);  else sim_hy3131_io.mosi(l); }
static bool hy_miso(void)    { return use_real ? afe_miso() : sim_hy3131_io.miso(); }
static void hy_delay(void)   { if (use_real) afe_delay(); }

static const hy3131_io_t hybrid_io = {
    .cs = hy_cs, .clk = hy_clk, .mosi = hy_mosi, .miso = hy_miso, .delay = hy_delay,
};

/* With the real front end: capacitance on every range (0-4 counters, 5-6 charge slope). */
static bool hybrid_range_avail(meas_fn_t fn, int range)
{
    if (real_ok && fn == MEAS_CAP) return range >= AFE_CAP_FIRST && range <= 6;
    /* DC current: only ranges with checked rows (all six since 2026-11-14). */
    if (real_ok && fn == MEAS_DCI) return range <= AFE_DCI_LAST;
    /* AC current: all six ranges since 2026-11-17 (afe_table.h). */
    if (real_ok && fn == MEAS_ACI) return range <= AFE_ACI_LAST;
    return true;
}

/* SPEED (B11): the R22 that replaces 0x16 in the AD1 rows. AD1OSR halves
 * per step, so each step halves the conversion time (datasheet). */
static uint8_t afe_r22_rate = 0x16;
static void hybrid_rate(int rate)
{
    static const uint8_t r22[4] = { 0x16, 0x15, 0x14, 0x12 };   /* 0x12: 15 ms, fast recording (B24) */
    afe_r22_rate = r22[rate < 0 ? 0 : rate > 3 ? 3 : rate];
    sim_hy3131_set_rate(rate);              /* the functions still on the model */
}

/* Every function now has checked rows on the real meter (AC current since
 * 2026-11-16, its lower ranges only). Kept so a function can be withheld
 * again: one that would only show lib/sim's numbers must not be offered. */
/* B21: a positive voltage on the V jack above ~2.15 V adds to the current
 * reading while current is measured: the V input's clamp conducts, and its
 * current upsets the shunt measurement (it does not flow in the circuit: a
 * supply showed 0.00 A while ranges read 8-48 mA). Measured on our meter
 * 2026-11-16 with no load, 3-10 V (test-plan.md): the A jack (5 A and 10 A
 * alike) 1.0928 mA/V above 2.146 V, within 0.04 mA; -5 V adds nothing. The
 * 50 / 500 mA ranges see 5-6 mA/V but pair only with mV (vi_avail); 500 uA
 * and 5 mA see none. */
static double vi_leak(int irange, double volts)
{
    if (irange < 4 || irange > 5 || volts <= 2.146) return 0.0;
    return 1.0928e-3 * (volts - 2.146);
}

static bool hybrid_fn_avail(meas_fn_t fn)
{
    (void)fn;
    return true;
}

static void hybrid_configure(meas_fn_t fn, int range, afe_aux_t aux)
{
    _Static_assert(MEAS_DCV == 0 && MEAS_ACV == 1 && MEAS_DCI == 2 && MEAS_OHM2 == 4 && MEAS_CONT == 6 && MEAS_DIODE == 7 &&
                   MEAS_CAP == 8 && MEAS_FREQ == 9 && MEAS_TEMP == 10,
                   "afe_row() numbering");
    const afe_row_t *row = 0;
    if (real_ok && (aux == AFE_AUX_NONE || aux == AFE_AUX_FAST || aux == AFE_AUX_CAP_CHARGE)) row = afe_row(fn, range);
    if (real_ok && (aux == AFE_AUX_CJ1 || aux == AFE_AUX_CJ2)) row = &AFE_TC;   /* type K's cold junction */
    if (row) {
        use_real = true;
        uint8_t r[sizeof row->r20];
        memcpy(r, row->r20, sizeof r);
        if (aux == AFE_AUX_FAST) r[0x22 - 0x20] = AFE_R22_FAST;   /* hunting: B10d */
        else if (r[0x22 - 0x20] == 0x16 && aux == AFE_AUX_NONE) r[0x22 - 0x20] = afe_r22_rate;   /* SPEED: B11 */
        if (aux == AFE_AUX_CAP_CHARGE) {              /* stock's phase B */
            r[0x2F - 0x20] = AFE_CAP_CHARGE_R2F(r[0x2F - 0x20]);
            r[0x31 - 0x20] = AFE_CAP_CHARGE_R31(r[0x31 - 0x20]);
        }
        if (aux == AFE_AUX_CJ1) r[0x33 - 0x20] = AFE_TC_R33_CJ1;
        if (aux == AFE_AUX_CJ2) r[0x33 - 0x20] = AFE_TC_R33_CJ2;
        hy3131_write8(HY_R20, 0x68);                  /* stock's order */
        hy3131_write_burst(HY_R20, r, sizeof r);
        hal_afe_relays(row->pa8, row->pa13);
    } else {
        if (use_real || !real_ok) hal_afe_relays_release();
        use_real = false;
        sim_hy3131_configure(fn, range, aux);
    }
}

static void hybrid_init(void)
{
    char st[6] = { 0 };
    real_ok = hal_afe_probe_pins(st) != 0;
    if (real_ok) hal_afe_probe_bus();
}
#endif

bool hal_afe_dcv_real(void)
{
#if defined(HAL_AFE_DCV_REAL)
    return real_ok;
#else
    return false;
#endif
}

/* ---- the rest of platform_t --------------------------------------------------- */

/* ---- backlight and beeper: timer PWM ---------------------------------------- */

#if HAL_BACKLIGHT_ENABLED || HAL_BEEPER_ENABLED
static void pwm_init(uint32_t timer, uint16_t prescaler, uint32_t period)
{
    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler = prescaler;
    tp.period = period;
    timer_init(timer, &tp);

    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    oc.outputstate = TIMER_CCX_ENABLE;
    oc.ocpolarity = TIMER_OC_POLARITY_HIGH;
    timer_channel_output_config(timer, TIMER_CH_3, &oc);
    timer_channel_output_mode_config(timer, TIMER_CH_3, TIMER_OC_MODE_PWM0);
    timer_channel_output_pulse_value_config(timer, TIMER_CH_3, 0);
    timer_auto_reload_shadow_enable(timer);
    if (timer == TIMER0) timer_primary_output_config(TIMER0, ENABLE);   /* advanced timer */
    timer_enable(timer);
}
#endif

static void backlight_board_init(void)
{
#if HAL_BACKLIGHT_ENABLED
    /* 100 counts (1% steps) at HAL_BACKLIGHT_PWM_HZ. Stock uses 11 kHz with
     * 64 steps and a table that never goes below 31% (docs/hardware.md);
     * owners report an audible squeal that rises with brightness, so ours
     * defaults above hearing. */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_TIMER0);
    gpio_init(PIN_BL_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, PIN_BL);
    pwm_init(TIMER0, (uint16_t)(SystemCoreClock / (100u * HAL_BACKLIGHT_PWM_HZ) - 1u), 99);
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_3, 100u);   /* full on, as the bootloader left it */
#endif
}

static void backlight(uint8_t pct)
{
#if HAL_BACKLIGHT_ENABLED
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_3, pct > 100 ? 100u : pct);
#else
    (void)pct;
#endif
}

/* ---- real-time clock (B33) -------------------------------------------------- */

#if HAL_RTC_ENABLED
/* The library's waits (rtc_lwoff_wait, rtc_register_sync_wait) spin forever
 * if the RTC clock isn't running; these give up. At 32.768 kHz a flag takes
 * a few RTC cycles, well under a millisecond. */
#define RTC_MARK 0xB33Au                  /* BKP_DATA_0: "the counter holds Unix time" */
static uint8_t rtc_state;                 /* 0 off, 1 LXTAL starting, 2 running */

static bool rtc_flag_wait(uint32_t bit)
{
    for (uint32_t n = 0; n < 200000u; n++) if (RTC_CTL & bit) return true;
    return false;
}

static bool rtc_sync(void)
{
    RTC_CTL &= ~RTC_CTL_RSYNF;
    return rtc_flag_wait(RTC_CTL_RSYNF);
}

static void rtc_board_init(void)
{
    rcu_periph_clock_enable(RCU_PMU);
    rcu_periph_clock_enable(RCU_BKPI);
    pmu_backup_write_enable();
    if ((RCU_BDCTL & RCU_BDCTL_RTCEN) && (RCU_BDCTL & RCU_BDCTL_RTCSRC) == RCU_RTCSRC_LXTAL) {
        rtc_state = rtc_sync() ? 2 : 0;   /* already running on the cell */
        return;
    }
    rcu_osci_on(RCU_LXTAL);               /* finish in rtc_poll once it is stable */
    rtc_state = 1;
}

static void rtc_poll(void)
{
    if (rtc_state != 1 || !rcu_flag_get(RCU_FLAG_LXTALSTB)) return;
    rcu_rtc_clock_config(RCU_RTCSRC_LXTAL);
    rcu_periph_clock_enable(RCU_RTC);
    if (!rtc_sync() || !rtc_flag_wait(RTC_CTL_LWOFF)) { rtc_state = 0; return; }
    rtc_prescaler_set(32767u);            /* 1 Hz */
    rtc_state = rtc_flag_wait(RTC_CTL_LWOFF) ? 2 : 0;
}

static uint32_t clock_now(void)
{
    rtc_poll();
    if (rtc_state != 2 || bkp_read_data(BKP_DATA_0) != RTC_MARK) return 0;
    return rtc_counter_get();
}

static void clock_set(uint32_t t)
{
    rtc_poll();
    if (rtc_state != 2 || !rtc_flag_wait(RTC_CTL_LWOFF)) return;
    rtc_counter_set(t);
    if (rtc_flag_wait(RTC_CTL_LWOFF)) bkp_write_data(BKP_DATA_0, RTC_MARK);
}
#else
static void rtc_board_init(void) {}
#endif

static void beeper_board_init(void)
{
#if HAL_BEEPER_ENABLED
    /* Stock's pitch: 3.04 kHz (stock divides 72 MHz by 370 x 64; we count
     * at 6 MHz, 72 MHz / 12, for finer duty steps). */
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_TIMER1);
    /* Full remap puts TIMER1 CH0-CH3 on PA15/PB3/PB10/PB11, but only pins
     * configured as AF follow it: CH3 (PB11) here. PA15/PB3 (keypad) and
     * PB10 (panel WR) stay GPIO. The SPL writes the SWJ field as all ones
     * on this call, so it doesn't re-enable JTAG on the keypad pins. */
    gpio_pin_remap_config(GPIO_TIMER1_FULL_REMAP, ENABLE);
    gpio_init(PIN_BEEP_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, PIN_BEEP);
    pwm_init(TIMER1, 11, 1973);                 /* 6 MHz tick; 3040 Hz, stock's pitch */
#endif
}

/* Pitch and drive (B57). TIMER1 counts 72 MHz / 12 = 6 MHz (the prescaler
 * beeper_board_init sets): the reload sets the pitch (1973 = 3040 Hz,
 * stock's), the compare the duty, in 1/2000ths of a cycle at 3 kHz. Level
 * 100 is stock's 50%, the strongest square drive. */
#define BEEP_TICK_HZ 6000000u                   /* 72 MHz / 12: fine duty steps (B57) */
#if HAL_BEEPER_ENABLED
static uint32_t beep_reload = 1973;
#endif

static void tone(uint16_t hz, uint8_t level)
{
#if HAL_BEEPER_ENABLED
    if (level == 0 || hz == 0) { timer_channel_output_pulse_value_config(TIMER1, TIMER_CH_3, 0); return; }
    uint32_t reload = (BEEP_TICK_HZ + hz / 2u) / hz;
    if (reload < 750u) reload = 750u;           /* 8 kHz */
    if (reload > 30000u) reload = 30000u;       /* 200 Hz */
    reload -= 1u;
    if (reload != beep_reload) {
        timer_autoreload_value_config(TIMER1, reload);
        beep_reload = reload;
    }
    if (level > 100u) level = 100u;
    /* Loudness. Seen on the meter (2026-11-12): duty between 5% and 50%
     * barely changes this buzzer's loudness; it falls off only at a few
     * percent and below. So level maps on a cube: 100 -> 50% (stock's
     * drive), 50 -> 6%, 25 -> 0.8%, 10 -> 0.05%. */
    uint64_t l3 = (uint64_t)level * level * level;              /* up to 1e6 */
    uint32_t pulse = (uint32_t)((uint64_t)(reload + 1u) * l3 / 2000000u);
    timer_channel_output_pulse_value_config(TIMER1, TIMER_CH_3, pulse ? pulse : 1u);
#else
    (void)hz; (void)level;
#endif
}

static void beep(bool on)
{
#if HAL_BEEPER_ENABLED
    timer_channel_output_pulse_value_config(TIMER1, TIMER_CH_3, on ? (beep_reload + 1u) / 2u : 0u);
#else
    (void)on;
#endif
}

/* ---- SCPI serial: USART0, RX by interrupt into a ring ------------------------- */

static volatile char     uart_rx[256];
static volatile uint16_t uart_head, uart_tail;

#if HAL_SERIAL_ENABLED
void USART0_IRQHandler(void)
{
    while (usart_flag_get(USART0, USART_FLAG_RBNE) == SET) {
        char ch = (char)usart_data_receive(USART0);       /* also clears overrun */
        uint16_t next = (uint16_t)((uart_head + 1u) % sizeof uart_rx);
        if (next != uart_tail) { uart_rx[uart_head] = ch; uart_head = next; }
    }
}
#endif

static void serial_board_init(void)
{
#if HAL_SERIAL_ENABLED
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);
    gpio_init(PIN_UART_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, PIN_UART_TX);
    gpio_init(PIN_UART_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_UART_RX);
    usart_deinit(USART0);
    usart_baudrate_set(USART0, HAL_UART_BAUD);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_interrupt_enable(USART0, USART_INT_RBNE);
    nvic_irq_enable(USART0_IRQn, 1, 0);
    usart_enable(USART0);
#endif
}

/* B47: the battery, PB0 on ADC0 IN8 through a 1:2 divider from VBAT.
 * Read against the internal reference (IN17, ~1.2 V) so the 2.87 V supply's
 * tolerance drops out; BAT_K folds the divider and this chip's reference
 * together, set from one point on our meter (VBAT 4.21 V on a second meter,
 * PB0 2087 mV by the nominal 1.2 V: 4210 / 2087). PB0 stays an analog input
 * once set up: the bootloader leaves it a high output, driving the divider. */
#define BAT_K_X1000 2017u
static bool bat_ok;
static int  bat_mv = -1;
static uint32_t bat_at;

static uint16_t adc_one(uint8_t ch)
{
    adc_regular_channel_config(ADC0, 0, ch, ADC_SAMPLETIME_239POINT5);
    adc_flag_clear(ADC0, ADC_FLAG_EOC);
    adc_software_trigger_enable(ADC0, ADC_REGULAR_CHANNEL);
    uint32_t t0 = hal_millis();
    while (adc_flag_get(ADC0, ADC_FLAG_EOC) == RESET)
        if (hal_millis() - t0 > 5u) return 0u;
    return (uint16_t)adc_regular_data_read(ADC0);
}

static void battery_board_init(void)
{
#if HAL_BATTERY_ENABLED
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_init(GPIOB, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, GPIO_PIN_0);
    rcu_periph_clock_enable(RCU_ADC0);
    rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6);    /* 12 MHz */
    adc_deinit(ADC0);
    adc_mode_config(ADC_MODE_FREE);
    adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(ADC0, ADC_REGULAR_CHANNEL, 1);
    adc_external_trigger_source_config(ADC0, ADC_REGULAR_CHANNEL, ADC0_1_2_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC0, ADC_REGULAR_CHANNEL, ENABLE);
    adc_tempsensor_vrefint_enable();
    adc_enable(ADC0);
    uint32_t t0 = hal_millis();
    while (hal_millis() - t0 < 2u) { }
    adc_calibration_enable(ADC0);
    bat_ok = true;
#endif
}

/* 16 pairs, ~1 ms. raw_pin and raw_ref are the sums. */
static int battery_sample(uint32_t *raw_pin, uint32_t *raw_ref)
{
    uint32_t pin = 0, ref = 0;
    for (int i = 0; i < 16; i++) { pin += adc_one(ADC_CHANNEL_8); ref += adc_one(ADC_CHANNEL_17); }
    if (raw_pin) *raw_pin = pin;
    if (raw_ref) *raw_ref = ref;
    if (ref == 0u) return -1;
    return (int)((uint64_t)pin * 1200u * BAT_K_X1000 / ((uint64_t)ref * 1000u));
}

/* Refreshed every 2 s, smoothed (a frame's backlight load moves it a little). */
static int battery_mv(void)
{
    if (!bat_ok) return -1;
    uint32_t now = hal_millis();
    if (bat_mv < 0 || now - bat_at >= 2000u) {
        bat_at = now;
        int v = battery_sample(NULL, NULL);
        if (v > 0) bat_mv = bat_mv < 0 ? v : (bat_mv * 3 + v) / 4;
    }
    return bat_mv;
}

#if defined(HAL_SERIAL_CONSOLE)
static void serial_write(const char *s, size_t n);

/* DIAG:ADC? (B47, read-only): the raw battery reading behind battery_mv. */
static void diag_adc(void)
{
    char out[120];
    int n;
    if (!bat_ok) n = snprintf(out, sizeof out, "ADC battery not enabled in this build\r\n");
    else {
        uint32_t pin, ref;
        int mv = battery_sample(&pin, &ref);
        n = snprintf(out, sizeof out, "ADC PB0 raw=%lu vref=%lu pin=%lumV battery=%dmV\r\n",
                     (unsigned long)(pin / 16u), (unsigned long)(ref / 16u),
                     ref ? (unsigned long)((uint64_t)pin * 1200u / ref) : 0ul, mv);
    }
    if (n > 0) serial_write(out, (size_t)n);
}

/* DIAG:RESET is caught here, under whoever reads the port (the console or
 * the app's SCPI), so serial-update.py --reset works on every console
 * build. It answers OK and resets the chip into OWON's bootloader. */
static void watch_for_reset(char ch)
{
    static char line[12];
    static uint8_t len;
    static bool too_long;
    if (ch == '\r' || ch == '\n') {
        bool match = !too_long && len == 10 && memcmp(line, "DIAG:RESET", 10) == 0;
        bool adc = !too_long && len == 9 && memcmp(line, "DIAG:ADC?", 9) == 0;
        len = 0;
        too_long = false;
        if (adc) diag_adc();
        if (match) {
            static const char ok[] = "OK\r\n";
            for (size_t i = 0; i < sizeof ok - 1; i++) {
                while (usart_flag_get(USART0, USART_FLAG_TBE) == RESET) { }
                usart_data_transmit(USART0, (uint8_t)ok[i]);
            }
            hal_reset();
        }
    } else if (len < sizeof line) {
        line[len++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    } else {
        too_long = true;
    }
}
#endif

static size_t serial_read(char *buf, size_t max)
{
    size_t n = 0;
    while (n < max && uart_tail != uart_head) {
        buf[n] = uart_rx[uart_tail];
        uart_tail = (uint16_t)((uart_tail + 1u) % sizeof uart_rx);
#if defined(HAL_SERIAL_CONSOLE)
        watch_for_reset(buf[n]);
#endif
        n++;
    }
    return n;
}

static void serial_write(const char *s, size_t n)
{
#if HAL_SERIAL_ENABLED
    for (size_t i = 0; i < n; i++) {
        while (usart_flag_get(USART0, USART_FLAG_TBE) == RESET) { }
        usart_data_transmit(USART0, (uint8_t)s[i]);
    }
#else
    (void)s; (void)n;
#endif
}

/* The Utility menu's baud setting. Waits for the last byte to leave, so a
 * reply in flight is not garbled by the change. */
static void serial_baud(uint32_t baud)
{
#if HAL_SERIAL_ENABLED
    while (usart_flag_get(USART0, USART_FLAG_TC) == RESET) { }
    usart_disable(USART0);
    usart_baudrate_set(USART0, baud);
    usart_enable(USART0);
#else
    (void)baud;
#endif
}

/* Keys for the app: scanned every 20 ms, debounced and decoded by
 * lib/keypad. Nothing is driven unless the passive check passed. */
static int keys_ok;
static keypad_t keys;

static void keypad_board_init(void)
{
    keypad_reset(&keys);
    keys_ok = (hal_keypad_init() == 0);
}

static app_key_t key_poll(void)
{
    static uint32_t last_ms;
    if (!keys_ok) return KEY_NONE;
    uint32_t now = hal_millis();
    if (now - last_ms < 20u) return KEY_NONE;
    last_ms = now;
    return keypad_feed(&keys, hal_keypad_scan());
}

/* ---- settings and data-log flash -------------------------------------------- */

/* Second, independent guards (lib/store and lib/datalog have their own): each
 * io reaches only its own pages, and nothing reaches the stock storage at
 * 0x0803D800. Each region is enabled separately in hal_pins.h. */
#define STORE_LO STORE_PAGE0
#define STORE_HI (STORE_PAGE1 + STORE_PAGE_SIZE)
#define LOG_LO   DLOG_BASE
#define LOG_HI   DLOG_END
#define UCAL_LO  UCAL_PAGE0
#define UCAL_HI  (UCAL_PAGE1 + STORE_PAGE_SIZE)
#define STOCK_STORAGE 0x0803D800u
_Static_assert(LOG_HI <= UCAL_LO && UCAL_HI <= STORE_LO, "log, user cal and settings must not overlap");

#if HAL_STORE_ENABLED || HAL_LOG_ENABLED
static bool region_ok(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi)
{
    return a >= lo && a + n <= hi && hi <= STOCK_STORAGE;
}

static int fmc_erase(uint32_t addr, uint32_t lo, uint32_t hi)
{
    if (!region_ok(addr, STORE_PAGE_SIZE, lo, hi) || (addr - lo) % STORE_PAGE_SIZE) return -1;
    fmc_unlock();
    fmc_flag_clear(FMC_FLAG_BANK0_END | FMC_FLAG_BANK0_WPERR | FMC_FLAG_BANK0_PGERR);
    fmc_state_enum r = fmc_page_erase(addr);
    fmc_lock();
    return r == FMC_READY ? 0 : -1;
}

static int fmc_program(uint32_t addr, uint32_t v, uint32_t lo, uint32_t hi)
{
    if (!region_ok(addr, 4, lo, hi) || (addr & 3u)) return -1;
    fmc_unlock();
    fmc_flag_clear(FMC_FLAG_BANK0_END | FMC_FLAG_BANK0_WPERR | FMC_FLAG_BANK0_PGERR);
    fmc_state_enum r = fmc_word_program(addr, v);
    fmc_lock();
    return r == FMC_READY ? 0 : -1;
}
#endif

#if HAL_STORE_ENABLED || HAL_LOG_ENABLED
static void flash_read(uint32_t addr, void *dst, size_t n)
{
    const uint8_t *src = (const uint8_t *)addr;
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = src[i];
}
#endif

#if HAL_STORE_ENABLED
static int store_erase(uint32_t a)               { return fmc_erase(a, STORE_LO, STORE_HI); }
static int store_program(uint32_t a, uint32_t v) { return fmc_program(a, v, STORE_LO, STORE_HI); }
static const store_io_t store_io = {
    .read = flash_read, .erase_page = store_erase, .program_word = store_program,
};
#endif

#if HAL_STORE_ENABLED
static int ucal_erase(uint32_t a)                { return fmc_erase(a, UCAL_LO, UCAL_HI); }
static int ucal_program(uint32_t a, uint32_t v)  { return fmc_program(a, v, UCAL_LO, UCAL_HI); }
static const store_io_t ucal_io = {
    .read = flash_read, .erase_page = ucal_erase, .program_word = ucal_program,
};
#endif

#if HAL_LOG_ENABLED
static int log_erase(uint32_t a)                 { return fmc_erase(a, LOG_LO, LOG_HI); }
static int log_program(uint32_t a, uint32_t v)   { return fmc_program(a, v, LOG_LO, LOG_HI); }
static const store_io_t log_io = {
    .read = flash_read, .erase_page = log_erase, .program_word = log_program,
};
#endif

static meas_cal_t cal;
static int        cal_bad = -1;                /* -1 until hal_load_calibration */
static int        cal_status(void) { return cal_bad; }
static rgb565_t   band[SCR_W * 20];

int hal_load_calibration(void)
{
    meas_cal_t c;
    meas_cal_parse(&c, (const uint8_t *)FACTORY_RECORD_ADDR, MEAS_CAL_RECORD_LEN);
    int bad = meas_cal_validate(&c);
    if (bad == 0) cal = c;
    else meas_cal_defaults(&cal);
    cal_bad = bad;
    return bad;
}

/* B21 power: a current range alternates only with a volts range that leaves
 * PA8 where it is. On our meter, 2026-11-16 (test-plan.md, Power card):
 * 5 mA with 5 V (PA8 still) held the current within 0.002%; 500 mA with
 * 5 V (PA8 moving) **opened the current path for every volts read**: a scope
 * across the load showed it drop to zero at each click. The meter's own
 * current readings and the supply's display were too slow to show it.
 * So: 50 / 500 mA (PA8 on) pair with 50 / 500 mV only; the rest with
 * 5 V and up. vrange -1 asks whether the current range has any partner. */
static bool vi_avail(int irange, int vrange)
{
    if (irange < 0 || irange > AFE_DCI_LAST || vrange > 5) return false;
    return vrange < 0 || AFE_DCI[irange].pa8 == AFE_DCV[vrange].pa8;
}

static const platform_t platform = {
#if defined(HAL_AFE_DCV_REAL)
    .afe_bus = &hybrid_io,
    .afe_configure = hybrid_configure,
    .range_avail = hybrid_range_avail,
    .fn_avail = hybrid_fn_avail,
    .vi_avail = vi_avail,
    .vi_leak = vi_leak,
    .afe_rate = hybrid_rate,
#elif defined(HAL_SIM_AFE)
    /* The xdm1241-ui build: the HY3131 is lib/sim's model, running on the
     * chip. Nothing drives the real front end; readings are simulated. */
    .afe_bus = &sim_hy3131_io,
    .afe_configure = sim_hy3131_configure,
#else
    .afe_bus = &afe_io,
    .afe_configure = afe_configure,
#endif
    .lcd = &lcd_io,
    .millis = hal_millis,
    .key_poll = key_poll,
    .beep = beep,
    .tone = tone,
    .backlight = backlight,
#if HAL_RTC_ENABLED
    .clock_now = clock_now,
    .clock_set = clock_set,
#endif
    .cal = &cal,
    .cal_status = cal_status,
    .battery_mv = battery_mv,                  /* B47: PB0; -1 unless HAL_BATTERY_ENABLED */
#if HAL_STORE_ENABLED
    .store = &store_io,
    .ucal_flash = &ucal_io,
#endif
#if HAL_LOG_ENABLED
    .log_flash = &log_io,
#endif
    .serial_read = HAL_SERIAL_ENABLED ? serial_read : NULL,
    .serial_write = HAL_SERIAL_ENABLED ? serial_write : NULL,
    .serial_baud = HAL_SERIAL_ENABLED ? serial_baud : NULL,
    .band = band,
    .band_rows = 20,
};

void hal_board_init(void)
{
    lcd_board_init();
    afe_board_init();
    backlight_board_init();
    rtc_board_init();
    battery_board_init();
    beeper_board_init();
    serial_board_init();
    keypad_board_init();
}

const platform_t *hal_platform(void) { return &platform; }

bool hal_afe_simulated(void)
{
#if defined(HAL_SIM_AFE)
    return true;
#else
    return false;
#endif
}

void hal_sim_init(void)
{
#if defined(HAL_SIM_AFE)
    (void)afe_configure;                         /* the real ones, unused here */
    (void)afe_io;
    sim_hy3131_reset();
    sim_hy3131_set_cj_gain(meas_cj_gain(cal.cj_g));
    sim_hy3131_world()->noise_counts = 2.0;      /* the last digit moves: alive */
#endif
#if defined(HAL_AFE_DCV_REAL)
    hybrid_init();
#endif
}

void hal_sim_tick(uint32_t now_ms)
{
#if defined(HAL_SIM_AFE)
    static uint32_t done;
    while (done != now_ms) { sim_hy3131_tick_ms(); done++; }
#else
    (void)now_ms;
#endif
}

/* ---- keypad scan (HAL_KEYPAD_ENABLED) -------------------------------------------- */

#if HAL_KEYPAD_ENABLED
static void key_b_mode(uint32_t mode)
{
    gpio_init(GPIOA, mode, GPIO_OSPEED_2MHZ, GPIO_PIN_14 | GPIO_PIN_15);
    gpio_init(GPIOB, mode, GPIO_OSPEED_2MHZ, GPIO_PIN_3 | GPIO_PIN_4);
    gpio_init(GPIOC, mode, GPIO_OSPEED_2MHZ, GPIO_PIN_13);
}

static void key_b_low(void)
{
    GPIO_BC(GPIOA) = GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_BC(GPIOB) = GPIO_PIN_3 | GPIO_PIN_4;
    GPIO_BC(GPIOC) = GPIO_PIN_13;
}

/* Group B as 5 bits, 1 = line high. */
static uint32_t key_b_read(void)
{
    uint32_t a = GPIO_ISTAT(GPIOA), b = GPIO_ISTAT(GPIOB), c = GPIO_ISTAT(GPIOC);
    return ((a >> 14) & 1u) | (((a >> 15) & 1u) << 1) | (((b >> 3) & 1u) << 2) |
           (((b >> 4) & 1u) << 3) | (((c >> 13) & 1u) << 4);
}

static uint32_t key_a_read(void) { return (GPIO_ISTAT(KEY_A_PORT) >> 5) & 0x1Fu; }

static void settle(void) { for (volatile int n = 0; n < 200; n++) { } }   /* ~10 us */

static void all_inputs(void)
{
    gpio_init(KEY_A_PORT, GPIO_MODE_IPU, GPIO_OSPEED_2MHZ, KEY_A_PINS);
    key_b_mode(GPIO_MODE_IPU);
}

int hal_keypad_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_AF);
    /* Free PA14/PA15/PB3/PB4 from SWD/JTAG, as stock does. */
    gpio_pin_remap_config(GPIO_SWJ_DISABLE_REMAP, ENABLE);
    all_inputs();
    settle();
    /* Passive check: nothing driven, every line pulled up. A line reading
     * low now is driven by something else: then we must never drive it. */
    uint32_t stuck = (~key_a_read() & 0x1Fu) | ((~key_b_read() & 0x1Fu) << 8);
    return (int)stuck;
}

uint32_t hal_keypad_scan(void)
{
    /* Phase 1: B driven low, read A. */
    key_b_low();                       /* output latch low before switching */
    key_b_mode(GPIO_MODE_OUT_PP);
    settle();
    uint32_t a = ~key_a_read() & 0x1Fu;
    key_b_mode(GPIO_MODE_IPU);
    /* Phase 2: A driven low, read B. */
    GPIO_BC(KEY_A_PORT) = KEY_A_PINS;
    gpio_init(KEY_A_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, KEY_A_PINS);
    settle();
    uint32_t b = ~key_b_read() & 0x1Fu;
    all_inputs();                      /* idle: nothing driven */
    return a | (b << 8);               /* 1 = pressed */
}
#else
int      hal_keypad_init(void) { return -1; }
uint32_t hal_keypad_scan(void) { return 0; }
#endif

void hal_beeper_init(void) { beeper_board_init(); }
void hal_beep(bool on) { beep(on); }
void hal_tone(uint16_t hz, uint8_t level) { tone(hz, level); }
void hal_backlight(uint8_t pct) { backlight(pct); }

/* ---- reset ---------------------------------------------------------------------- */

void hal_reset(void)
{
#if defined(HAL_SERIAL_CONSOLE)
    /* Let the last byte of the reply leave the shift register first. */
    for (uint32_t n = 0; n < 1000000u && usart_flag_get(USART0, USART_FLAG_TC) == RESET; n++) { }
#endif
    NVIC_SystemReset();
}

void hal_panel_init(void) { lcd_board_init(); }

/* ---- log console on the serial port (HAL_SERIAL_CONSOLE) ---------------------- */

void hal_console_init(void)
{
#if defined(HAL_SERIAL_CONSOLE)
    serial_board_init();
#endif
}

void hal_console_write(const char *s, size_t n)
{
#if defined(HAL_SERIAL_CONSOLE)
    serial_write(s, n);
#else
    (void)s; (void)n;
#endif
}

size_t hal_console_read(char *buf, size_t max)
{
#if defined(HAL_SERIAL_CONSOLE)
    return serial_read(buf, max);
#else
    (void)buf; (void)max;
    return 0;
#endif
}
