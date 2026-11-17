/* Beeper sounds (backlog B57): pitch, volume and envelope, per event.
 *
 * The board's beeper is a PWM output (PB11, TIMER1 CH3, docs/hardware.md):
 * the period sets the pitch, the duty sets the drive, so loudness. The app
 * asks for sounds by event; this module turns the owner's settings (pitch,
 * volume, style) into a level over time and drives platform_t.tone (or
 * plain beep on/off when the platform has no tone).
 *
 * One-shot sounds (key click, diode, auto-hold, test) override the gated
 * one (continuity), which resumes when they end. Pure logic: the caller
 * passes the time; nothing here touches hardware.
 */
#ifndef APP_SOUND_H
#define APP_SOUND_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SND_CLICK = 0,      /* key press */
    SND_CONT,           /* continuity: sounds while gated on */
    SND_DIODE,          /* a good junction */
    SND_HOLD,           /* auto-hold took a reading */
    SND_TEST,           /* SYST:BEEP:PLAY / the menu preview */
    SND_ALARM,          /* a warning: the 10 A duty limit (B19) */
    SND_COUNT
} snd_event_t;

/* Styles are presets of the envelope below; editing any envelope setting
 * makes the style CUSTOM. */
typedef enum { SND_CLASSIC = 0, SND_SOFT, SND_CHIRP, SND_CUSTOM,      /* numbers are stored: append */
               SND_DROP, SND_DOUBLE, SND_TWOTONE, SND_WARBLE, SND_NSTYLE } snd_style_t;
/* The menu's order (CUSTOM last). */
extern const snd_style_t snd_style_order[SND_NSTYLE];
/* Patterns: how a beep is shaped in time and pitch, beyond the envelope.
 * They change pitch or rhythm, which this buzzer shows plainly (its
 * loudness barely follows the drive, seen on the meter 2026-11-12). */
typedef enum { SND_PAT_SINGLE = 0, SND_PAT_DOUBLE, SND_PAT_TWOTONE, SND_PAT_WARBLE, SND_NPAT } snd_pat_t;
extern const char *const snd_pat_name[SND_NPAT];

#define SND_NPITCH 7
extern const uint16_t snd_pitch_hz[SND_NPITCH];   /* the menu's steps, 1000 .. 4000 Hz */
#define SND_PITCH_DEFAULT 3000                    /* Hz, near stock's 3.04 kHz */
#define SND_PITCH_MIN      200                    /* SCPI may set any pitch in range */
#define SND_PITCH_MAX     8000
#define SND_VOL_MIN  10
#define SND_VOL_STEP 10
/* Envelope settings and their menu steps. */
#define SND_NATTACK  7
#define SND_NRELEASE 8
#define SND_NGLIDE   11
#define SND_NCLICK   6
extern const uint8_t  snd_attack_ms[SND_NATTACK];    /* 0 .. 50 */
extern const uint16_t snd_release_ms[SND_NRELEASE];  /* 0 .. 300 */
extern const int8_t   snd_glide_pct[SND_NGLIDE];     /* -50 .. +50: pitch rise (or fall) over a beep */
extern const uint8_t  snd_click_ms[SND_NCLICK];      /* 10 .. 80 */
extern const char *const snd_style_name[SND_NSTYLE];

typedef struct {
    /* settings */
    uint16_t    pitch_hz;
    uint8_t     volume;         /* 10-100 % */
    snd_style_t style;
    uint8_t     attack_ms;      /* rise to full level */
    uint16_t    release_ms;     /* fall after the held part (a one-shot: at most its length) */
    int8_t      glide_pct;      /* one-shots: pitch from -glide/2 to +glide/2 % (negative: falls) */
    snd_pat_t   pattern;
    uint8_t     click_ms;       /* key click length */
    /* state */
    bool        gate;           /* SND_CONT wanted */
    uint32_t    gate_at;        /* when the gate last opened */
    bool        gate_was;
    uint32_t    gate_off_at;
    int         shot;           /* one-shot playing, or -1 */
    uint32_t    shot_at;
    uint16_t    out_hz;         /* last output, to send only changes */
    uint8_t     out_level;
} sound_t;

void     sound_init(sound_t *s);
uint16_t sound_pitch(const sound_t *s);
/* Start a one-shot event (not SND_CONT). */
void     sound_play(sound_t *s, snd_event_t e, uint32_t now);
/* Continuity: sound while on. */
void     sound_gate(sound_t *s, bool on, uint32_t now);
/* What should sound now: frequency and level (0 = silent). */
void     sound_level(const sound_t *s, uint32_t now, uint16_t *hz, uint8_t *level);
/* Call often. True when the output must change: drive tone(*hz, *level)
 * (level 0-100, 0 = silent). */
bool     sound_tick(sound_t *s, uint32_t now, uint16_t *hz, uint8_t *level);
/* Next/previous menu pitch step from the current pitch. */
uint16_t sound_pitch_step(uint16_t hz, int dir);
/* Load a style's envelope (not CUSTOM, which keeps the current one). */
void     sound_style(sound_t *s, snd_style_t st);
/* Step an envelope setting through its menu steps; the style becomes CUSTOM.
 * which: 0 attack, 1 release, 2 glide, 3 click length, 4 pattern. */
void     sound_step(sound_t *s, int which, int dir);
/* Clamp every setting to its range (after loading a record or SCPI). */
void     sound_sanitise(sound_t *s);
/* True while a one-shot is playing. */
bool     sound_busy(const sound_t *s, uint32_t now);

#endif
