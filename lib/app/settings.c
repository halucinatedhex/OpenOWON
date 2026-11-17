/* See settings.h. Layout, version 1 (all bytes unless noted):
 *   0      function
 *   1..11  range per function
 *   12     autorange
 *   13     frequency shows period
 *   14     dBm reference index
 *   15..16 continuity threshold, ohms (u16 LE)
 *   17     beeper
 *   18     backlight %
 *   19     theme index
 *   20     rate (0 S, 1 M, 2 F)
 *   21     temperature unit (0 C, 1 F, 2 K)
 *   22     backlight dim timeout index (menu_dim_s)
 *   23     baud index (menu_baud)
 *   24     graph span index (app_span_decim)
 *   25     recording interval index (app_rec_interval_ms)
 *   26     recording mode (0 timed, 1 manual)
 *   27     diode beep threshold, 0.1 V steps (1..30)
 *   28     auto-hold (0 off, 1 on)
 *   29     bar graph (0 off, 1 on)
 *   30..31 beep pitch, Hz (u16 LE)
 *   32     beep volume, % (10..100)
 *   33     beep style (0 classic, 1 soft, 2 chirp, 3 custom)
 *   34     beep attack, ms (0..50)
 *   35..36 beep release, ms (u16 LE, 0..300)
 *   37     beep glide, % (0..50)
 *   38     key click length, ms (10..80)
 *   39..40 PT100 lead resistance, milliohms (u16 LE, 0..5000)
 *   41     cold-junction offset, 0.1 C (int8, -50..50)
 *   42     recording when full (0 stop, 1 roll over the oldest)
 *   43     SCPI terminal mode (0 off, 1 echo + CR + editing)
 *   44     beep pattern (0 single, 1 double, 2 two-tone, 3 warble)
 * Byte 37 (glide) is signed since 2026-11-12 (-50..50); older records held
 * 0..50, which read the same. Byte 28 (auto-hold) is written 0 and ignored:
 * auto-hold is a RUN/STOP state now, not a preference.
 * Bytes are only ever appended; a shorter record leaves the rest at defaults.
 */
#include "settings.h"

#include "app.h"
#include "store.h"

_Static_assert(SETTINGS_MAX_LEN <= STORE_MAX_PAYLOAD, "a settings record must fit one store record");

size_t settings_pack(const app_t *a, uint8_t *b, size_t cap)
{
    if (cap < 45) return 0;
    b[0] = (uint8_t)a->fn;
    for (int f = 0; f < MEAS_NFUNC; f++) b[1 + f] = (uint8_t)a->range[f];
    b[12] = a->autorange;
    b[13] = a->period;
    b[14] = (uint8_t)a->rref_idx;
    b[15] = (uint8_t)a->cont_threshold;
    b[16] = (uint8_t)(a->cont_threshold >> 8);
    b[17] = a->beeper;
    b[18] = a->backlight_pct;
    b[19] = (uint8_t)a->theme_idx;
    b[20] = a->rate;
    b[21] = (uint8_t)a->temp_unit;
    b[22] = a->dim_idx;
    b[23] = a->baud_idx;
    b[24] = a->span_idx;
    b[25] = a->rec_int_idx;
    b[26] = a->rec_manual;
    b[27] = a->diode_th_dv;
    b[28] = 0;                             /* was auto-hold: a RUN/STOP state now */
    b[29] = a->bargraph;
    b[30] = (uint8_t)a->snd.pitch_hz;
    b[31] = (uint8_t)(a->snd.pitch_hz >> 8);
    b[32] = a->snd.volume;
    b[33] = (uint8_t)a->snd.style;
    b[34] = a->snd.attack_ms;
    b[35] = (uint8_t)a->snd.release_ms;
    b[36] = (uint8_t)(a->snd.release_ms >> 8);
    b[37] = a->snd.glide_pct;
    b[38] = a->snd.click_ms;
    b[39] = (uint8_t)a->rtd_lead_mohm;
    b[40] = (uint8_t)(a->rtd_lead_mohm >> 8);
    b[41] = (uint8_t)a->cj_offset_dc;
    b[42] = a->rec_roll;
    b[43] = a->terminal;
    b[44] = (uint8_t)a->snd.pattern;
    return 45;
}

void settings_apply(app_t *a, const uint8_t *b, size_t len, uint16_t version)
{
    if (version != SETTINGS_VERSION || len < 20) return;
    for (int f = 0; f < MEAS_NFUNC; f++)
        if (b[1 + f] < meas_nranges((meas_fn_t)f)) a->range[f] = b[1 + f];
    if (b[0] < MEAS_NFUNC) a->fn = b[0] == MEAS_OHM4 ? MEAS_OHM2 : (meas_fn_t)b[0];   /* no 4-wire */
    if (b[12] <= 1) a->autorange = b[12];
    if (b[13] <= 1) a->period = b[13];
    if (b[14] < MEAS_NRREF) a->rref_idx = b[14];
    uint16_t th = (uint16_t)(b[15] | b[16] << 8);
    if (th >= 1 && th <= 1000) a->cont_threshold = th;
    if (b[17] <= 1) a->beeper = b[17];
    if (b[18] <= 100) a->backlight_pct = b[18];
    if (b[19] < APP_NTHEMES) a->theme_idx = b[19];
    if (len >= 22) {                       /* added after the first layout */
        if (b[20] <= 2) a->rate = b[20];
        if (b[21] <= 2) a->temp_unit = (meas_tunit_t)b[21];
    }
    if (len >= 25) {
        if (b[22] < MENU_NDIM) a->dim_idx = b[22];
        if (b[23] < MENU_NBAUD) a->baud_idx = b[23];
        if (b[24] < APP_NSPAN) a->span_idx = b[24];
    }
    if (len >= 27) {
        if (b[25] < APP_NREC_INT) a->rec_int_idx = b[25];
        if (b[26] <= 1) a->rec_manual = b[26];
    }
    if (len >= 28 && b[27] >= APP_DIODE_TH_MIN && b[27] <= APP_DIODE_TH_MAX) a->diode_th_dv = b[27];
    /* byte 28 (auto-hold) ignored: not restored across power cycles */
    if (len >= 30 && b[29] <= 1) a->bargraph = b[29];
    if (len >= 34) {
        uint16_t hz = (uint16_t)(b[30] | b[31] << 8);
        if (hz >= SND_PITCH_MIN && hz <= SND_PITCH_MAX) a->snd.pitch_hz = hz;
        if (b[32] >= SND_VOL_MIN && b[32] <= 100) a->snd.volume = b[32];
        if (b[33] < SND_NSTYLE) sound_style(&a->snd, (snd_style_t)b[33]);
    }
    if (len >= 39) {
        a->snd.attack_ms = b[34];
        a->snd.release_ms = (uint16_t)(b[35] | b[36] << 8);
        a->snd.glide_pct = (int8_t)b[37];
        a->snd.click_ms = b[38];
    }
    sound_sanitise(&a->snd);
    if (len >= 41) {
        uint16_t lead = (uint16_t)(b[39] | b[40] << 8);
        if (lead <= APP_RTD_LEAD_MAX_MOHM) a->rtd_lead_mohm = lead;
    }
    if (len >= 42) {
        int8_t off = (int8_t)b[41];
        if (off >= -APP_CJ_OFFSET_MAX && off <= APP_CJ_OFFSET_MAX) a->cj_offset_dc = off;
    }
    if (len >= 43 && b[42] <= 1) a->rec_roll = b[42];
    if (len >= 44 && b[43] <= 1) a->terminal = b[43];
    if (len >= 45 && b[44] < SND_NPAT) a->snd.pattern = (snd_pat_t)b[44];
}
