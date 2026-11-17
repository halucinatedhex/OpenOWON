/* HOST ONLY. A behavioural HY3131 for the simulator.
 *
 * The bus side is bit-level: it implements the hy3131_io_t callbacks as an
 * SPI slave speaking the protocol in lib/afe/hy3131.c (address byte with rw,
 * an idle ninth bit on reads, auto-increment), so the real driver runs
 * against it unchanged.
 *
 * The analog side is a model: you set the physical input (sim_world_t) and the
 * chip produces the register values the app expects for its current
 * configuration. Counts are found by inverting lib/measure at unity
 * calibration, so this tests plumbing (protocol, acquisition, ranging,
 * display), not the formulas - those have their own tests against
 * independent data.
 *
 * Assumptions, until the bench says otherwise:
 *   - reading INTF clears it (stock never writes INTF)
 *   - sample periods: AD1 50 ms, AD2/RMS 100 ms, counters 500 ms
 */
#ifndef SIM_HY3131_H
#define SIM_HY3131_H

#include <stdbool.h>
#include <stdint.h>

#include "hy3131.h"
#include "meas.h"
#include "platform.h"

typedef struct {
    double dcv, acv;        /* V */
    double dci, aci;        /* A */
    double ohm;             /* ohm (also continuity) */
    double diode;           /* V forward drop */
    double cap;             /* F */
    double freq, duty;      /* Hz, % */
    double temp_c;          /* probe temperature */
    double cj_c;            /* cold-junction temperature */
    double noise_counts;    /* RMS noise added to AD1, in counts */
    double jack_a;          /* current lead in the A jack (1) or the mA jack (0) */
    double lead_ohm;        /* test-lead resistance, in series with PT100 (2-wire) */
    double rtd_shorted;     /* 1: the PT100's leads shorted at the sensor (only lead_ohm seen) */
} sim_world_t;

void sim_hy3131_reset(void);
sim_world_t *sim_hy3131_world(void);

/* The board's afe_configure() for the simulator. */
void sim_hy3131_configure(meas_fn_t fn, int range, afe_aux_t aux);
/* SPEED (platform_t.afe_rate): 0 LOW 246 ms, 1 MID 123 ms, 2 HIGH 61 ms a conversion. */
void sim_hy3131_set_rate(int rate);

/* Advance the model by one millisecond. */
void sim_hy3131_tick_ms(void);

/* Cold-junction G the model should use (must match the calibration the app
 * is given; meas_cj_gain(cal.cj_g)). */
void sim_hy3131_set_cj_gain(double g);

/* The bus. */
extern const hy3131_io_t sim_hy3131_io;

/* Inspection for tests. */
uint8_t sim_hy3131_reg(uint8_t reg);
uint32_t sim_hy3131_transactions(void);

#endif
