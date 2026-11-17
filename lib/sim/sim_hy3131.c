/* HOST ONLY. Behavioural HY3131. See sim_hy3131.h. */
#include "sim_hy3131.h"

#include <math.h>
#include <string.h>

#include "meas_temp.h"

#define NREG 0x38
#define FULL 8388606

/* AD1 output period, as the chip: AD1CLK = SYSCLK/30 = 133.3 kHz, divided
 * by the OSR in R22 and doubled by chopping. R22 0x16 (DC V, ohms: OSR
 * 16384, chop) is 246 ms - measured ~242 ms on our meter (2026-11-11/14);
 * R22 0x10 (continuity, diode, and the fast hunting set-up: OSR 256, chop)
 * measured ~20 ms on the meter, polled from the main loop. */
#define AD1_PERIOD_MS      246u
#define AD1_FAST_PERIOD_MS   4u     /* datasheet: 133.3 kHz / 256, chopped = 3.84 ms; the
                                     * meter's DIAG:ROW at R22 0x10 sees one every <5 ms */

/* Ohms: on the high ranges the source settles after a reconfiguration, the
 * reading decaying from above. Measured on our meter 2026-11-14 (47 kohm
 * on the 500 kohm range at the fast rate): 222k at first, ~18% of the
 * remaining error gone per ~4 ms conversion, so tau ~20 ms. 5 Mohm
 * behaves alike (1 Mohm touched/lifted in ~0.3-0.5 s). 50 Mohm is much
 * slower: 9.2 Mohm took ~1.5 s to settle from ~48 Mohm, so tau ~400 ms
 * (results/ohm-step-20261114.md). */
static double ohm_settle_tau_ms(int range)
{
    return range < 3 ? 0.0 : range < 5 ? 20.0 : 400.0;
}
#define AD2_PERIOD_MS 100u
#define CT_PERIOD_MS  500u
#define CT_REF       2000000.0      /* CTA + 2^20: a 0.5 s gate at 4 MHz */

/* ---- state ------------------------------------------------------------- */

static uint8_t     regs[NREG];
static sim_world_t world;
static meas_cal_t  unity;
static meas_fn_t   cfg_fn = MEAS_DCV;
static int         cfg_range = 2;
static afe_aux_t   cfg_aux = AFE_AUX_NONE;
static uint32_t    t_ms;
static double      cj_g = 0.169465;
static uint32_t    rng = 12345u;
static uint32_t    ntrans;
static bool        ct_running;      /* counters counting (ENCTR on, gate not over) */
static double      cap_v = 0.097;   /* charge-slope ranges: the capacitor, in meas_cap_volts() units */
static unsigned    rate_shift;      /* SPEED: 0 LOW, 1 MID, 2 HIGH: the period halves each step */

/* bus */
enum { P_OFF, P_ADDR, P_IDLE, P_READ, P_WRITE };
static int     phase = P_OFF;
static bool    mosi_lvl, clk_lvl;
static uint8_t shift, nbits, addr, out_byte;

/* ---- registers ------------------------------------------------------------ */

static uint8_t reg_read(uint8_t r)
{
    if (r >= NREG) return 0;
    uint8_t v = regs[r];
    if (r == HY_INTF) regs[r] = 0;          /* assumed read-to-clear */
    return v;
}

static void reg_write(uint8_t r, uint8_t v)
{
    if (r < NREG) regs[r] = v;
    if (r == HY_R20) {
        /* Datasheet section 14: ENCTR = 0 clears CTA<7:0>, CTB, CTC; ENCTR = 1
         * starts counting from the CTA preset. */
        if (!(v & 0x02u)) {
            regs[HY_CTA] = 0;
            for (int i = 0; i < 3; i++) regs[HY_CTB + i] = regs[HY_CTC + i] = 0;
            ct_running = false;
        } else {
            ct_running = true;
        }
    }
}

static void put24(uint8_t r, uint32_t v)
{
    regs[r] = (uint8_t)v; regs[r + 1] = (uint8_t)(v >> 8); regs[r + 2] = (uint8_t)(v >> 16);
}

/* ---- bus callbacks (SPI mode 0, MSB first) -------------------------------- */

static void bus_cs(bool asserted)
{
    if (asserted) { phase = P_ADDR; shift = 0; nbits = 0; ntrans++; }
    else phase = P_OFF;
}

static void bus_mosi(bool level) { mosi_lvl = level; }

static void bus_clk(bool level)
{
    bool rising = level && !clk_lvl, falling = !level && clk_lvl;
    clk_lvl = level;
    if (phase == P_OFF) return;

    if (rising) {
        if (phase == P_ADDR || phase == P_WRITE) {
            shift = (uint8_t)(shift << 1 | (mosi_lvl ? 1 : 0));
            nbits++;
        }
        return;
    }
    if (!falling) return;

    switch (phase) {
    case P_ADDR:
        if (nbits == 8) {
            addr = shift >> 1;
            phase = (shift & 1) ? P_IDLE : P_WRITE;
            shift = 0; nbits = 0;
        }
        break;
    case P_IDLE:                            /* the ninth bit before read data */
        phase = P_READ; nbits = 0;
        out_byte = reg_read(addr);
        break;
    case P_READ:
        if (++nbits == 8) { addr++; nbits = 0; out_byte = reg_read(addr); }
        break;
    case P_WRITE:
        if (nbits == 8) { reg_write(addr, shift); addr++; shift = 0; nbits = 0; }
        break;
    }
}

static bool bus_miso(void)
{
    return phase == P_READ ? (out_byte >> (7 - nbits)) & 1u : false;
}

const hy3131_io_t sim_hy3131_io = {
    .cs = bus_cs, .clk = bus_clk, .mosi = bus_mosi, .miso = bus_miso, .delay = 0,
};

/* ---- analog model ------------------------------------------------------------ */

static double gauss(void)
{
    double u1, u2;
    rng = rng * 1664525u + 1013904223u; u1 = ((rng >> 8) + 1.0) / 16777217.0;
    rng = rng * 1664525u + 1013904223u; u2 = ((rng >> 8) + 1.0) / 16777217.0;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static int32_t clamp24(double c)
{
    if (c > 0x7FFFFF) return 0x7FFFFF;
    if (c < -0x800000) return -0x800000;
    return (int32_t)lround(c);
}

/* The front end saturates a little past each range's full scale, so an
 * overloaded reading says only "too big", not by how much. 1.38 x FS is the
 * XDM2041's 5 V range (ADC limit +-0.69 V after the 1:10 divider; Petteri
 * Aimonen's measurement_modes.md). Without this the simulator would report a
 * 1000 V input exactly on the 50 mV range, and ranging could cheat. */
#define SIM_SATURATION 1.38

static double saturate(meas_fn_t fn, int range, double v)
{
    double lim = SIM_SATURATION * meas_to_si(fn, range, meas_fullscale(fn, range));
    return v > lim ? lim : v < -lim ? -lim : v;
}

/* Current flows through one jack only: the mA ranges (0-3) see it when the
 * lead is in the mA jack, the A ranges (4-5) when it is in the A jack. */
static double through_jack(int range, double amps)
{
    bool a_range = range >= 4;
    return a_range == (world.jack_a != 0.0) ? amps : 0.0;
}

/* AD1 counts that make lib/measure (unity calibration) return `value`. */
static int32_t ad1_for(double value, double full_scale_value)
{
    double c = value / full_scale_value * FULL;
    if (world.noise_counts > 0) c += gauss() * world.noise_counts;
    return clamp24(c);
}

static void produce_ad1(void)
{
    int32_t c;
    int r = cfg_range;
    if (cfg_aux == AFE_AUX_CJ1 || cfg_aux == AFE_AUX_CJ2) {
        double v_mv = 100.0;
        if (cfg_aux == AFE_AUX_CJ2) v_mv += 2.0 * cj_g * (world.cj_c + 273.15 + 16.0);
        c = ad1_for(v_mv * 1e-3, meas_dcv(&unity, 1, FULL));
    } else switch (cfg_fn) {
    case MEAS_DCV:  c = ad1_for(saturate(cfg_fn, r, world.dcv), meas_dcv(&unity, r, FULL)); break;
    case MEAS_DCI:  c = ad1_for(saturate(cfg_fn, r, through_jack(r, world.dci)), meas_dci(&unity, r, FULL)); break;
    case MEAS_OHM2:
    case MEAS_OHM4: {
        double ohm = world.ohm, tau = ohm_settle_tau_ms(r);
        if (tau > 0.0 && ohm < 1e11)                     /* open leads read open anyway */
            ohm += 0.4 * meas_to_si(cfg_fn, r, meas_fullscale(cfg_fn, r)) * exp(-(double)t_ms / tau);
        c = ad1_for(saturate(cfg_fn, r, ohm), meas_ohm(&unity, cfg_fn, r, FULL, true));
        break;
    }
    case MEAS_CONT: c = ad1_for(world.ohm, meas_continuity(&unity, FULL)); break;
    case MEAS_DIODE: c = ad1_for(world.diode, meas_diode(&unity, FULL)); break;
    case MEAS_CAP: {                                     /* 5 / 50 mF: the voltage, clipped at full scale */
        double v = cap_v > MEAS_CAP_V_FULL ? 0.6667 : cap_v;
        c = clamp24(v / meas_cap_volts(1) + (world.noise_counts > 0 ? gauss() * world.noise_counts : 0.0));
        if (c > FULL) c = FULL;
        break;
    }
    case MEAS_TEMP:
        if (r == 0) {
            double emf_v = (meas_tc_k_mv(world.temp_c) - meas_tc_k_mv(world.cj_c)) * 1e-3;
            c = ad1_for(emf_v, meas_dcv(&unity, 1, FULL));
        } else {
            double k = r == 2 ? 10.0 : 1.0;                  /* PT1000: 5 kohm range */
            c = ad1_for((world.rtd_shorted ? 0.0 : k * meas_pt100_ohm(world.temp_c)) + world.lead_ohm,
                        meas_ohm(&unity, MEAS_OHM2, r == 2 ? 1 : 0, FULL, true));
        }
        break;
    default:
        return;
    }
    put24(HY_AD1_DATA, (uint32_t)c & 0xFFFFFFu);
    regs[HY_INTF] |= HY_F_AD1;
}

static void produce_rms(void)
{
    double v = saturate(cfg_fn, cfg_range, cfg_fn == MEAS_ACV ? world.acv : through_jack(cfg_range, world.aci));
    double full = 262143.0 * 262143.0;
    double s = meas_ac(&unity, cfg_fn, cfg_range, (uint64_t)full, true);
    double root = fabs(v) / s * 262143.0;
    double sq = root * root;
    if (sq > 0x3FFFFFFFFFull) sq = (double)0x3FFFFFFFFFull;
    uint64_t q = (uint64_t)llround(sq);
    for (int i = 0; i < 5; i++) regs[HY_RMS_DATA + i] = (uint8_t)(q >> (8 * i));
    regs[HY_INTF] |= HY_F_RMS;
}

static void produce_counters(void)
{
    uint32_t cta = (uint32_t)(CT_REF - 1048576.0);
    uint32_t ctb = 0, ctc = 0;
    if (cfg_fn == MEAS_FREQ) {
        /* Reciprocal counting: CTB is whole input cycles, and CTA + 2^20 the
         * 4 MHz reference counted over exactly those cycles. Inferred from
         * stock's f = CTB / gate together with its 50.032 Hz display, which
         * a fixed gate could not resolve (docs/measurement.md). */
        if (world.freq > 0.0) {
            double cycles = floor(world.freq * (CT_REF / 4e6));
            if (cycles < 1.0) cycles = 1.0;
            double ref = cycles / world.freq * 4e6;
            cta = (uint32_t)lround(ref - 1048576.0);
            ctb = (uint32_t)cycles;
            ctc = (uint32_t)lround(world.duty / 100.0 * ref);
        } else {
            ctc = (uint32_t)lround(world.duty / 100.0 * CT_REF);
        }
    } else {                                     /* capacitance: CTC / CTB ratio */
        double per_ratio = meas_cap_counter(&unity, cfg_range, 1, 1);
        double ratio = world.cap / per_ratio;
        double b = ratio > 1.0 ? floor(16000000.0 / ratio) : 100000.0;
        if (b < 1) b = 1;
        if (b > 100000) b = 100000;
        ctb = (uint32_t)b;
        double cc = ratio * b;
        ctc = cc > 0xFFFFFF ? 0xFFFFFFu : (uint32_t)lround(cc);
    }
    put24(HY_CTA, cta & 0xFFFFFFu);
    put24(HY_CTB, ctb & 0xFFFFFFu);
    put24(HY_CTC, ctc & 0xFFFFFFu);
    regs[HY_INTF] |= HY_F_CT;
    ct_running = false;                          /* the gate is over: CTA stops */
}

/* The 5 / 50 mF path as on our meter (docs/measurement.md): phase B
 * charges at a constant current, which meas_cap_rate() at unity gain reads
 * as world.cap; phase A relaxes to the rest level through ~250 ohm. */
static void cap_step_ms(void)
{
    double c = world.cap > 1e-9 ? world.cap : 1e-9;
    if (cfg_aux == AFE_AUX_CAP_CHARGE) {
        double k = cfg_range == 5 ? 277.0 / 200.0 : 1.0;
        cap_v += 0.36e-3 * k / c / 1000.0;
        if (cap_v > 3.0) cap_v = 3.0;                    /* the source's compliance */
    } else {
        double tau_ms = 250.0 * c * 1000.0;
        cap_v += (MEAS_CAP_V_REST - cap_v) * (tau_ms < 1.0 ? 1.0 : 1.0 / tau_ms);
    }
}

void sim_hy3131_tick_ms(void)
{
    t_ms++;
    bool slope = cfg_fn == MEAS_CAP && cfg_range >= 5;
    bool ac = (cfg_fn == MEAS_ACV || cfg_fn == MEAS_ACI) && cfg_aux == AFE_AUX_NONE;
    bool ct = ((cfg_fn == MEAS_CAP && !slope) || cfg_fn == MEAS_FREQ) && cfg_aux == AFE_AUX_NONE;
    if (slope) {
        cap_step_ms();
        if (t_ms % AD1_PERIOD_MS == 0) produce_ad1();
        return;
    }
    /* As the chip: AD2F at the AD2 rate (tens of kHz; every ms here), RMSF
     * only when a new RMS sum is ready (docs/afe-table.md, 2026-11-13). */
    if (ac)      { regs[HY_INTF] |= HY_F_AD2; if (t_ms % AD2_PERIOD_MS == 0) produce_rms(); }
    else if (ct) {
        if (ct_running) {                        /* CTA counts SYSCLK, 4 MHz */
            uint32_t a = ((uint32_t)regs[HY_CTA] | (uint32_t)regs[HY_CTA + 1] << 8 |
                          (uint32_t)regs[HY_CTA + 2] << 16) + 4000u;
            put24(HY_CTA, a & 0xFFFFFFu);
        }
        /* As on the meter (2026-11-15): a capacitor far above the range
         * never ends a charge cycle, so no result at all. */
        bool stuck = cfg_fn == MEAS_CAP &&
                     world.cap > 10.0 * meas_to_si(MEAS_CAP, cfg_range, meas_fullscale(MEAS_CAP, cfg_range));
        if (t_ms % CT_PERIOD_MS == 0 && !stuck) produce_counters();
    }
    else {
        bool fast = cfg_aux == AFE_AUX_FAST || cfg_fn == MEAS_CONT || cfg_fn == MEAS_DIODE;
        bool cj = cfg_aux == AFE_AUX_CJ1 || cfg_aux == AFE_AUX_CJ2;   /* the CJ row keeps 0x16 */
        uint32_t period = fast ? AD1_FAST_PERIOD_MS : cj ? AD1_PERIOD_MS : AD1_PERIOD_MS >> rate_shift;
        if (t_ms % period == 0) produce_ad1();
    }
}

void sim_hy3131_set_rate(int rate)     /* 3: fast recording, 246 >> 4 = 15 ms (B24) */
{
    rate_shift = rate < 0 ? 0u : rate > 2 ? (rate == 3 ? 4u : 2u) : (unsigned)rate;
}

void sim_hy3131_configure(meas_fn_t fn, int range, afe_aux_t aux)
{
    cfg_fn = fn; cfg_range = range; cfg_aux = aux;
    t_ms = 0;
    regs[HY_INTF] = 0;
}

void sim_hy3131_reset(void)
{
    memset(regs, 0, sizeof regs);
    ct_running = false;
    rate_shift = 0;
    cap_v = MEAS_CAP_V_REST;
    memset(&world, 0, sizeof world);
    world.ohm = 1e12;                 /* open leads */
    world.diode = 10.0;
    world.cj_c = 25.0;
    world.temp_c = 25.0;
    meas_cal_defaults(&unity);
    cfg_fn = MEAS_DCV; cfg_range = 2; cfg_aux = AFE_AUX_NONE;
    t_ms = 0; phase = P_OFF; clk_lvl = false; ntrans = 0; rng = 12345u;
    cj_g = 0.169465;
}

sim_world_t *sim_hy3131_world(void) { return &world; }
void sim_hy3131_set_cj_gain(double g) { cj_g = g; }
uint8_t sim_hy3131_reg(uint8_t r) { return r < NREG ? regs[r] : 0; }
uint32_t sim_hy3131_transactions(void) { return ntrans; }
