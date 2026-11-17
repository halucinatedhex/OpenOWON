/* See sound.h. */
#include "sound.h"

const uint16_t snd_pitch_hz[SND_NPITCH] = { 1000, 1500, 2000, 2500, 3000, 3500, 4000 };
const char *const snd_style_name[SND_NSTYLE] = { "CLASSIC", "SOFT", "CHIRP", "CUSTOM",
                                                 "DROP", "DOUBLE", "TWO-TONE", "WARBLE" };
const snd_style_t snd_style_order[SND_NSTYLE] = { SND_CLASSIC, SND_SOFT, SND_CHIRP, SND_DROP, SND_DOUBLE,
                                                  SND_TWOTONE, SND_WARBLE, SND_CUSTOM };
const char *const snd_pat_name[SND_NPAT] = { "SINGLE", "DOUBLE", "TWO-TONE", "WARBLE" };
const uint8_t  snd_attack_ms[SND_NATTACK]   = { 0, 2, 5, 8, 15, 30, 50 };
const uint16_t snd_release_ms[SND_NRELEASE] = { 0, 10, 20, 40, 80, 150, 200, 300 };
const int8_t   snd_glide_pct[SND_NGLIDE]    = { -50, -40, -30, -20, -10, 0, 10, 20, 30, 40, 50 };
const uint8_t  snd_click_ms[SND_NCLICK]     = { 10, 20, 30, 40, 60, 80 };

/* The presets: attack, release, glide, pattern. */
static const struct { uint8_t a; uint16_t r; int8_t g; snd_pat_t p; } preset[SND_NSTYLE] = {
    [SND_CLASSIC] = { 0, 0, 0, SND_PAT_SINGLE },
    [SND_SOFT]    = { 8, 40, 0, SND_PAT_SINGLE },
    [SND_CHIRP]   = { 8, 40, 50, SND_PAT_SINGLE },
    [SND_DROP]    = { 8, 40, -50, SND_PAT_SINGLE },
    [SND_DOUBLE]  = { 0, 0, 0, SND_PAT_DOUBLE },
    [SND_TWOTONE] = { 0, 0, 0, SND_PAT_TWOTONE },
    [SND_WARBLE]  = { 0, 0, 0, SND_PAT_WARBLE },
};

/* Per event: length of the held part (ms) and pitch relative to the
 * owner's setting (percent). The click is short and a little higher so it
 * reads as a key, not a result; the hold beep is distinct from the diode. */
static const struct { uint16_t ms; uint8_t pitch_pct; } shape[SND_COUNT] = {
    [SND_CLICK] = { 30, 125 },        /* length: the click_ms setting */
    [SND_CONT]  = { 0, 100 },
    [SND_DIODE] = { 150, 100 },
    [SND_HOLD]  = { 120, 112 },
    [SND_TEST]  = { 400, 100 },
    [SND_ALARM] = { 250, 133 },       /* higher than anything else */
};

void sound_init(sound_t *s)
{
    *s = (sound_t){ .pitch_hz = SND_PITCH_DEFAULT, .volume = 100, .style = SND_CLASSIC,
                    .click_ms = 30, .shot = -1 };
}

void sound_style(sound_t *s, snd_style_t st)
{
    if (st >= SND_NSTYLE) return;
    s->style = st;
    if (st == SND_CUSTOM) return;
    s->attack_ms = preset[st].a;
    s->release_ms = preset[st].r;
    s->glide_pct = preset[st].g;
    s->pattern = preset[st].p;
}

static int step_u16(const uint16_t *v, int n, int cur, int dir)
{
    if (dir > 0) { for (int i = 0; i < n; i++) if (v[i] > cur) return v[i]; return v[n - 1]; }
    for (int i = n - 1; i >= 0; i--) if (v[i] < cur) return v[i];
    return v[0];
}

static int step_i8(const int8_t *v, int n, int cur, int dir)
{
    if (dir > 0) { for (int i = 0; i < n; i++) if (v[i] > cur) return v[i]; return v[n - 1]; }
    for (int i = n - 1; i >= 0; i--) if (v[i] < cur) return v[i];
    return v[0];
}

static int step_u8(const uint8_t *v, int n, int cur, int dir)
{
    if (dir > 0) { for (int i = 0; i < n; i++) if (v[i] > cur) return v[i]; return v[n - 1]; }
    for (int i = n - 1; i >= 0; i--) if (v[i] < cur) return v[i];
    return v[0];
}

void sound_step(sound_t *s, int which, int dir)
{
    switch (which) {
    case 0: s->attack_ms = (uint8_t)step_u8(snd_attack_ms, SND_NATTACK, s->attack_ms, dir); break;
    case 1: s->release_ms = (uint16_t)step_u16(snd_release_ms, SND_NRELEASE, s->release_ms, dir); break;
    case 2: s->glide_pct = (int8_t)step_i8(snd_glide_pct, SND_NGLIDE, s->glide_pct, dir); break;
    case 3: s->click_ms = (uint8_t)step_u8(snd_click_ms, SND_NCLICK, s->click_ms, dir); return;
    case 4: s->pattern = (snd_pat_t)((s->pattern + SND_NPAT + (dir > 0 ? 1 : -1)) % SND_NPAT); break;
    default: return;
    }
    s->style = SND_CUSTOM;
}

void sound_sanitise(sound_t *s)
{
    if (s->pitch_hz < SND_PITCH_MIN || s->pitch_hz > SND_PITCH_MAX) s->pitch_hz = SND_PITCH_DEFAULT;
    if (s->volume < SND_VOL_MIN || s->volume > 100) s->volume = 100;
    if (s->style >= SND_NSTYLE) s->style = SND_CLASSIC;
    if (s->attack_ms > 50) s->attack_ms = 50;
    if (s->release_ms > 300) s->release_ms = 300;
    if (s->glide_pct > 50) s->glide_pct = 50;
    if (s->glide_pct < -50) s->glide_pct = -50;
    if (s->pattern >= SND_NPAT) s->pattern = SND_PAT_SINGLE;
    if (s->click_ms < 10 || s->click_ms > 80) s->click_ms = 30;
}

uint16_t sound_pitch(const sound_t *s)
{
    return s->pitch_hz >= SND_PITCH_MIN && s->pitch_hz <= SND_PITCH_MAX ? s->pitch_hz
                                                                        : SND_PITCH_DEFAULT;
}

uint16_t sound_pitch_step(uint16_t hz, int dir)
{
    if (dir > 0) {
        for (int i = 0; i < SND_NPITCH; i++) if (snd_pitch_hz[i] > hz) return snd_pitch_hz[i];
        return snd_pitch_hz[SND_NPITCH - 1];
    }
    for (int i = SND_NPITCH - 1; i >= 0; i--) if (snd_pitch_hz[i] < hz) return snd_pitch_hz[i];
    return snd_pitch_hz[0];
}

void sound_play(sound_t *s, snd_event_t e, uint32_t now)
{
    if (e == SND_CONT || e >= SND_COUNT) return;
    s->shot = (int)e;
    s->shot_at = now;
}

void sound_gate(sound_t *s, bool on, uint32_t now)
{
    if (on && !s->gate) s->gate_at = now;
    if (!on && s->gate) s->gate_off_at = now;
    if (on || s->gate) s->gate_was = true;
    s->gate = on;
}

static uint32_t shot_len(const sound_t *s, int e)
{
    return e == SND_CLICK ? s->click_ms : shape[e].ms;
}

/* A one-shot's rise is at most its held length, and its fall at most that
 * too, so a short click stays short however soft the style. */
static uint32_t shot_attack(const sound_t *s, uint32_t len) { return s->attack_ms < len ? s->attack_ms : len; }
static uint32_t shot_release(const sound_t *s, uint32_t len) { return s->release_ms < len ? s->release_ms : len; }

/* Envelope 0-1000 for a one-shot of held length len, t ms in. */
static uint32_t shot_env(const sound_t *s, uint32_t len, uint32_t t)
{
    uint32_t a = shot_attack(s, len), r = shot_release(s, len);
    if (t < a) return 1000u * t / a;
    if (t < len) return 1000u;
    if (r && t < len + r) return 1000u * (len + r - t) / r;
    return 0u;
}

static uint32_t shot_total(const sound_t *s, uint32_t len) { return len + shot_release(s, len); }

bool sound_busy(const sound_t *s, uint32_t now)
{
    if (s->shot < 0) return false;
    return now - s->shot_at < shot_total(s, shot_len(s, s->shot));
}

/* One-shots: DOUBLE is two beeps in the held time (40% on, 20% off, 40%
 * on); TWO-TONE a major third above for the first half, then the pitch;
 * WARBLE alternates between the pitch and 20% above it every
 * 25 ms (20 Hz). */
#define WARBLE_MS 25u
static void pattern_shot(snd_pat_t pat, uint32_t len, uint32_t t, uint32_t *hz, uint32_t *env)
{
    switch (pat) {
    case SND_PAT_DOUBLE:  if (t >= len * 4u / 10u && t < len * 6u / 10u) *env = 0; break;
    case SND_PAT_TWOTONE: if (t < len / 2u) *hz = *hz * 126u / 100u; break;
    case SND_PAT_WARBLE:  if ((t / WARBLE_MS) & 1u) *hz = *hz * 120u / 100u; break;
    default: break;
    }
}

/* Continuity (sustained): DOUBLE pulses 120 ms on / 80 off, TWO-TONE
 * alternates every 250 ms, WARBLE as for one-shots. */
static void pattern_gate(snd_pat_t pat, uint32_t t, uint32_t *hz, uint32_t *env)
{
    switch (pat) {
    case SND_PAT_DOUBLE:  if (t % 200u >= 120u) *env = 0; break;
    case SND_PAT_TWOTONE: if ((t / 250u) & 1u) *hz = *hz * 126u / 100u; break;
    case SND_PAT_WARBLE:  if ((t / WARBLE_MS) & 1u) *hz = *hz * 120u / 100u; break;
    default: break;
    }
}

void sound_level(const sound_t *s, uint32_t now, uint16_t *hz, uint8_t *level)
{
    uint32_t base = sound_pitch(s);
    *hz = (uint16_t)base;
    *level = 0;
    if (sound_busy(s, now)) {
        uint32_t len = shot_len(s, s->shot), t = now - s->shot_at;
        uint32_t p = base * shape[s->shot].pitch_pct / 100u;
        if (s->glide_pct && s->shot != SND_CLICK) {      /* -glide/2 .. +glide/2 % */
            int32_t tot = (int32_t)shot_total(s, len), g = s->glide_pct * 10;
            int32_t tt = (int32_t)(t < (uint32_t)tot ? t : (uint32_t)tot);
            p = (uint32_t)((int64_t)p * (1000 - g / 2 + g * tt / (tot ? tot : 1)) / 1000);
        }
        uint32_t env = shot_env(s, len, t);
        if (s->shot != SND_CLICK) pattern_shot(s->pattern, len, t, &p, &env);
        *hz = (uint16_t)p;
        *level = (uint8_t)(s->volume * env / 1000u);
        if (!*level && env) *level = 1;
        return;
    }
    if (s->gate) {                                       /* continuity */
        uint32_t t = now - s->gate_at;
        uint32_t env = t >= s->attack_ms ? 1000u : 1000u * t / s->attack_ms;
        uint32_t p = base;
        pattern_gate(s->pattern, t, &p, &env);
        *hz = (uint16_t)p;
        *level = (uint8_t)(s->volume * env / 1000u);
        if (!*level && env) *level = 1;                  /* DOUBLE's gaps stay silent */
        return;
    }
    if (s->gate_was && s->release_ms) {                  /* continuity fades out */
        uint32_t t = now - s->gate_off_at;
        if (t < s->release_ms) *level = (uint8_t)(s->volume * (s->release_ms - t) / s->release_ms);
    }
}

bool sound_tick(sound_t *s, uint32_t now, uint16_t *hz, uint8_t *level)
{
    sound_level(s, now, hz, level);
    if (s->shot >= 0 && !sound_busy(s, now)) s->shot = -1;
    if (!s->gate && s->gate_was && now - s->gate_off_at >= s->release_ms)
        s->gate_was = false;
    if (!*level) *hz = s->out_hz;                        /* silence: don't retune */
    if (*hz == s->out_hz && *level == s->out_level) return false;
    s->out_hz = *hz;
    s->out_level = *level;
    return true;
}
