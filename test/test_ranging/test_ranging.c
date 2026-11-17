/* Autoranging latency, measured end to end in the simulator (backlog B10f).
 *
 * Each case settles the meter on one input, steps the input, and counts
 * virtual milliseconds until the display shows the new value correctly: not
 * overload (unless the input really is beyond the top range), within
 * tolerance, and on a range autoranging would stay on. The table is printed
 * so changes can be compared; the assertions bound the worst cases.
 *
 * What the numbers include: the chip's AD1 period (246 ms at full
 * resolution, 20 ms while hunting: B10d), AC 100 ms, the settle time after
 * a reconfiguration and the discarded samples. The conversion times match
 * the meter (DC 0 -> 25 V took 1.5 s there before the fast hunt; this
 * model gave 1.69 s); settle times are still a guess (B10e). The front end saturates at 1.38 x full scale, so an
 * overload says only "too big".
 */
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "readout.h"
#include "sim_board.h"

static app_t app;
static sim_world_t *w;

void setUp(void) {}
void tearDown(void) {}

#define OPEN 1e12           /* open leads, ohms */
#define LIMIT_MS 10000u

static double *input(meas_fn_t fn)
{
    switch (fn) {
    case MEAS_DCV: return &w->dcv;
    case MEAS_ACV: return &w->acv;
    case MEAS_DCI: return &w->dci;
    case MEAS_ACI: return &w->aci;
    default:       return &w->ohm;
    }
}

/* Is the display right for input v? */
static bool correct(meas_fn_t fn, double v)
{
    if (!app.have) return false;
    int r = app.range[fn], lo, top;
    readout_range_span(fn, r, &lo, &top);           /* current: the selected jack's ranges */
    if (readout_overload(fn, top, v))               /* beyond the input: overload on its top */
        return app.overload && r == top;
    if (app.overload) return false;
    double lsb = meas_to_si(fn, r, meas_fullscale(fn, r)) / 50000.0;   /* one count */
    if (fabs(app.value - v) > fmax(2e-3 * fabs(v), 3.0 * lsb)) return false;
    return readout_autorange(fn, r, app.value, false) == r;   /* a resting range */
}

/* Settle on `from`, step to `to` `phase` ms later, return ms until correct
 * (LIMIT_MS if never). The phase matters: a step that lands just before a
 * sample and a display tick is caught at once, one just after waits. */
static uint32_t latency(meas_fn_t fn, double from, double to, uint32_t phase)
{
    sim_board_init(NULL);
    w = sim_hy3131_world();
    app_init(&app, sim_board_platform());
    app_select(&app, fn);
    *input(fn) = from;
    sim_board_run(&app, 6000 + phase);
    TEST_ASSERT_TRUE_MESSAGE(correct(fn, from), "did not settle on the starting value");
    *input(fn) = to;
    for (uint32_t t = 1; t <= LIMIT_MS; t++) {
        sim_board_run(&app, 1);
        if (correct(fn, to)) return t;
    }
    return LIMIT_MS;
}

typedef struct { const char *name; meas_fn_t fn; double from, to; } step_t;

static const step_t steps[] = {
    { "DCV same range   1 V -> 2 V",       MEAS_DCV, 1.0, 2.0 },
    { "DCV up 1         300 mV -> 3 V",    MEAS_DCV, 0.3, 3.0 },
    { "DCV down 1       3 V -> 300 mV",    MEAS_DCV, 3.0, 0.3 },
    { "DCV up 4         10 mV -> 400 V",   MEAS_DCV, 0.01, 400.0 },
    { "DCV down 4       400 V -> 10 mV",   MEAS_DCV, 400.0, 0.01 },
    { "DCV up all       10 mV -> 900 V",   MEAS_DCV, 0.01, 900.0 },
    { "DCV to zero      30 V -> 0 V",      MEAS_DCV, 30.0, 0.0 },
    { "DCI up 3 (mA)    100 uA -> 300 mA", MEAS_DCI, 1e-4, 0.3 },
    { "DCI down 3 (mA)  300 mA -> 100 uA", MEAS_DCI, 0.3, 1e-4 },
    { "DCI over (mA)    100 mA -> 3 A",    MEAS_DCI, 0.1, 3.0 },
    { "OHM up 4         100 R -> 4 M",     MEAS_OHM2, 100.0, 4e6 },
    { "OHM down 4       4 M -> 100 R",     MEAS_OHM2, 4e6, 100.0 },
    { "OHM open -> 1 k  (touch leads)",    MEAS_OHM2, OPEN, 1000.0 },
    { "OHM 1 k -> open  (lift leads)",     MEAS_OHM2, 1000.0, OPEN },
    { "ACV up 3         100 mV -> 300 V",  MEAS_ACV, 0.1, 300.0 },
    { "ACV down 3       300 V -> 100 mV",  MEAS_ACV, 300.0, 0.1 },
};
#define NSTEPS (int)(sizeof steps / sizeof steps[0])

#define NPHASE 12            /* step at 0, 17, 34 ... 187 ms into the cycle */

static uint32_t worst[NSTEPS];

static void test_measure_all(void)
{
    printf("\n  %-36s %8s %8s\n", "step", "mean ms", "max ms");
    uint32_t all = 0;
    for (int i = 0; i < NSTEPS; i++) {
        uint32_t sum = 0, mx = 0;
        for (uint32_t k = 0; k < NPHASE; k++) {
            uint32_t t = latency(steps[i].fn, steps[i].from, steps[i].to, k * 17u);
            sum += t;
            if (t > mx) mx = t;
        }
        worst[i] = mx;
        if (mx > all) all = mx;
        printf("  %-36s %8lu %8lu\n", steps[i].name, (unsigned long)(sum / NPHASE), (unsigned long)mx);
    }
    printf("  worst case overall: %lu ms\n", (unsigned long)all);
    for (int i = 0; i < NSTEPS; i++)
        TEST_ASSERT_TRUE_MESSAGE(worst[i] < LIMIT_MS, steps[i].name);
}

/* Faster must not mean jumpy: hold noisy inputs at each threshold for 30 s
 * and count range changes after the first settle. */
static int changes_near(meas_fn_t fn, double v, double noise_counts)
{
    sim_board_init(NULL);
    w = sim_hy3131_world();
    app_init(&app, sim_board_platform());
    app_select(&app, fn);
    *input(fn) = v;
    w->noise_counts = noise_counts;
    sim_board_run(&app, 3000);
    int r = app.range[fn], n = 0;
    for (int t = 0; t < 30000; t++) {
        sim_board_run(&app, 1);
        if (app.range[fn] != r) { n++; r = app.range[fn]; }
    }
    return n;
}

static void test_no_flapping_at_boundaries(void)
{
    static const struct { meas_fn_t fn; double v; const char *what; } c[] = {
        { MEAS_DCV, 0.450, "DCV at the down threshold (90% of 500 mV)" },
        { MEAS_DCV, 0.400, "DCV at the immediate threshold (80% of 500 mV)" },
        { MEAS_DCV, 0.500, "DCV at full scale of 500 mV" },
        { MEAS_DCV, 5.000, "DCV at full scale of 5 V" },
        { MEAS_OHM2, 450.0, "ohms at the down threshold" },
        { MEAS_DCI, 0.045, "DCI at the down threshold" },
    };
    /* Simulated noise is in ADC counts, so it is the same fraction of full
     * scale on every range: 2000 counts is about 0.035% (some 18 display
     * counts rms, a noisy but plausible meter), 60000 about 1% (500 display
     * counts, far worse than any working meter). */
    static const double noise[] = { 2000.0, 60000.0 };
    for (unsigned k = 0; k < 2; k++)
        for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
            int n = changes_near(c[i].fn, c[i].v, noise[k]);
            printf("  noise %5.0f  %-48s %d range changes in 30 s\n", noise[k], c[i].what, n);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(1, n, c[i].what);
        }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_measure_all);
    RUN_TEST(test_no_flapping_at_boundaries);
    return UNITY_END();
}
