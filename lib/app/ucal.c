/* See ucal.h. */
#include "ucal.h"

#include <math.h>
#include <string.h>

#include "readout.h"

bool ucal_applies(meas_fn_t fn)
{
    return fn != MEAS_FREQ && fn != MEAS_TEMP && (unsigned)fn < MEAS_NFUNC;
}

static bool valid(meas_fn_t fn, int range)
{
    return ucal_applies(fn) && range >= 0 && range < UCAL_NRANGE && range < meas_nranges(fn);
}

/* Payload: count (u8), then per non-zero slot: fn (u8), range (u8),
 * g_ppm (i32 LE), o_ppm (i32 LE). 10 bytes each: 49 fit in 496. */
void ucal_open(ucal_t *u, const store_io_t *io)
{
    memset(u, 0, sizeof *u);
    if (!io) return;
    u->open = true;
    if (!store_open_sized(&u->st, io, UCAL_PAGE0, UCAL_PAGE1, UCAL_REC_SIZE)) return;
    uint8_t b[UCAL_REC_SIZE];
    uint16_t ver;
    int n = store_load(&u->st, b, sizeof b, &ver);
    if (n < 1 || ver != UCAL_VERSION) return;
    for (int i = 0, k = b[0]; i < k && 1 + (i + 1) * 10 <= n; i++) {
        const uint8_t *e = b + 1 + i * 10;
        int32_t g, o;
        memcpy(&g, e + 2, 4);
        memcpy(&o, e + 6, 4);
        if (!valid((meas_fn_t)e[0], e[1])) continue;
        if (g < -UCAL_MAX_G_PPM || g > UCAL_MAX_G_PPM || o < -UCAL_MAX_O_PPM || o > UCAL_MAX_O_PPM) continue;
        u->slot[e[0]][e[1]] = (ucal_slot_t){ g, o };
    }
}

int ucal_save(ucal_t *u)
{
    if (!u->open || !u->st.rec_size) return -1;      /* opened by ucal_open, record or not */
    uint8_t b[UCAL_REC_SIZE];
    int k = 0;
    for (int f = 0; f < MEAS_NFUNC; f++)
        for (int r = 0; r < UCAL_NRANGE; r++) {
            const ucal_slot_t *s = &u->slot[f][r];
            if (!s->g_ppm && !s->o_ppm) continue;
            if (1 + (k + 1) * 10 > (int)store_max_payload(&u->st)) return -1;
            uint8_t *e = b + 1 + k * 10;
            e[0] = (uint8_t)f;
            e[1] = (uint8_t)r;
            memcpy(e + 2, &s->g_ppm, 4);
            memcpy(e + 6, &s->o_ppm, 4);
            k++;
        }
    b[0] = (uint8_t)k;
    return store_save(&u->st, b, (size_t)(1 + k * 10), UCAL_VERSION);
}

double ucal_apply(const ucal_t *u, meas_fn_t fn, int range, double v)
{
    if (!valid(fn, range) || !isfinite(v)) return v;
    const ucal_slot_t *s = &u->slot[fn][range];
    if (!s->g_ppm && !s->o_ppm) return v;
    return v * (1.0 + s->g_ppm * 1e-6) + s->o_ppm * 1e-6 * readout_fullscale(fn, range);
}

bool ucal_active(const ucal_t *u, meas_fn_t fn, int range)
{
    return valid(fn, range) && (u->slot[fn][range].g_ppm || u->slot[fn][range].o_ppm);
}

int ucal_count(const ucal_t *u)
{
    int n = 0;
    for (int f = 0; f < MEAS_NFUNC; f++)
        for (int r = 0; r < UCAL_NRANGE; r++) n += u->slot[f][r].g_ppm || u->slot[f][r].o_ppm;
    return n;
}

int ucal_zero(ucal_t *u, meas_fn_t fn, int range, double raw)
{
    if (!valid(fn, range)) return -1;
    double fs = readout_fullscale(fn, range);
    if (!(fs > 0) || !isfinite(raw) || fabs(raw) > 0.01 * fs) return -2;
    ucal_slot_t *s = &u->slot[fn][range];
    /* raw * (1 + g) + o * fs = 0 */
    double o = -raw * (1.0 + s->g_ppm * 1e-6) / fs * 1e6;
    if (fabs(o) > UCAL_MAX_O_PPM) return -4;
    s->o_ppm = (int32_t)lround(o);
    return 0;
}

int ucal_gain(ucal_t *u, meas_fn_t fn, int range, double raw, double target)
{
    if (!valid(fn, range)) return -1;
    double fs = readout_fullscale(fn, range);
    if (!(fs > 0) || !isfinite(raw) || !isfinite(target) || fabs(target) < 0.1 * fs || fabs(raw) < 0.05 * fs)
        return -3;
    ucal_slot_t *s = &u->slot[fn][range];
    /* raw * (1 + g) + o * fs = target */
    double g = ((target - s->o_ppm * 1e-6 * fs) / raw - 1.0) * 1e6;
    if (fabs(g) > UCAL_MAX_G_PPM) return -4;
    s->g_ppm = (int32_t)lround(g);
    return 0;
}

void ucal_clear(ucal_t *u, meas_fn_t fn, int range)
{
    if (valid(fn, range)) u->slot[fn][range] = (ucal_slot_t){ 0, 0 };
}

void ucal_clear_all(ucal_t *u)
{
    memset(u->slot, 0, sizeof u->slot);
}
