/* lib/app/sound.c: pitch, volume and envelope per event (B57). */
#include <unity.h>

#include "sound.h"

static sound_t s;

void setUp(void)    { sound_init(&s); }
void tearDown(void) {}

static uint8_t level_at(uint32_t t)
{
    uint16_t hz;
    uint8_t l;
    sound_level(&s, t, &hz, &l);
    return l;
}

static uint16_t hz_at(uint32_t t)
{
    uint16_t hz;
    uint8_t l;
    sound_level(&s, t, &hz, &l);
    return hz;
}

static void test_defaults_are_stock_like(void)
{
    TEST_ASSERT_EQUAL_UINT16(3000, sound_pitch(&s));
    TEST_ASSERT_EQUAL_UINT8(100, s.volume);
    TEST_ASSERT_EQUAL_INT(SND_CLASSIC, s.style);
    TEST_ASSERT_EQUAL_UINT8(0, level_at(0));                 /* silent until asked */
}

static void test_classic_click_is_a_square_30_ms(void)
{
    sound_play(&s, SND_CLICK, 1000);
    TEST_ASSERT_EQUAL_UINT8(100, level_at(1000));
    TEST_ASSERT_EQUAL_UINT8(100, level_at(1029));
    TEST_ASSERT_EQUAL_UINT8(0, level_at(1030));
    TEST_ASSERT_EQUAL_UINT16(3750, hz_at(1010));             /* a key: 125% of the pitch */
}

static void test_volume_scales_the_level(void)
{
    s.volume = 40;
    sound_play(&s, SND_DIODE, 0);
    TEST_ASSERT_EQUAL_UINT8(40, level_at(50));
}

static void test_soft_ramps_up_and_down(void)
{
    sound_style(&s, SND_SOFT);
    sound_play(&s, SND_DIODE, 0);                            /* 150 ms held */
    TEST_ASSERT_TRUE(level_at(1) < 50);                      /* attack */
    TEST_ASSERT_EQUAL_UINT8(100, level_at(100));
    uint8_t mid = level_at(170);                             /* release, 40 ms */
    TEST_ASSERT_TRUE(mid > 0 && mid < 100);
    TEST_ASSERT_EQUAL_UINT8(0, level_at(195));
    TEST_ASSERT_TRUE(sound_busy(&s, 180));
    TEST_ASSERT_FALSE(sound_busy(&s, 195));
}

static void test_chirp_glides_up(void)
{
    sound_style(&s, SND_CHIRP);
    sound_play(&s, SND_TEST, 0);
    uint16_t a = hz_at(10), b = hz_at(200), c = hz_at(400);
    TEST_ASSERT_TRUE(a < b && b < c);
    TEST_ASSERT_TRUE(a < 3000 && c > 3000);
}

/* Continuity sounds while gated; a click interrupts it and it comes back. */
static void test_gate_and_click_override(void)
{
    sound_gate(&s, true, 0);
    TEST_ASSERT_EQUAL_UINT8(100, level_at(5));
    TEST_ASSERT_EQUAL_UINT16(3000, hz_at(5));
    sound_play(&s, SND_CLICK, 100);
    TEST_ASSERT_EQUAL_UINT16(3750, hz_at(110));              /* the click's pitch */
    TEST_ASSERT_EQUAL_UINT16(3000, hz_at(140));              /* back to the tone */
    TEST_ASSERT_EQUAL_UINT8(100, level_at(140));
    sound_gate(&s, false, 200);
    TEST_ASSERT_EQUAL_UINT8(0, level_at(200));               /* classic: stops at once */
}

/* tick reports only changes, so the hardware is touched only when needed. */
static void test_tick_reports_changes_only(void)
{
    uint16_t hz;
    uint8_t l;
    TEST_ASSERT_FALSE(sound_tick(&s, 0, &hz, &l));           /* silent, nothing to do */
    sound_play(&s, SND_DIODE, 10);
    TEST_ASSERT_TRUE(sound_tick(&s, 10, &hz, &l));
    TEST_ASSERT_EQUAL_UINT8(100, l);
    TEST_ASSERT_FALSE(sound_tick(&s, 50, &hz, &l));          /* unchanged */
    TEST_ASSERT_TRUE(sound_tick(&s, 160, &hz, &l));          /* ended */
    TEST_ASSERT_EQUAL_UINT8(0, l);
    TEST_ASSERT_FALSE(sound_tick(&s, 170, &hz, &l));
}

/* The envelope is the owner's: attack, release, glide, click length. */
static void test_custom_envelope(void)
{
    sound_step(&s, 0, +1);                                   /* attack 0 -> 2 ms */
    TEST_ASSERT_EQUAL_INT(SND_CUSTOM, s.style);
    while (s.attack_ms < 30) sound_step(&s, 0, +1);
    for (int i = 0; i < 5; i++) sound_step(&s, 1, +1);       /* release -> 150 ms */
    TEST_ASSERT_EQUAL_UINT16(150, s.release_ms);
    sound_play(&s, SND_TEST, 0);                             /* 400 ms held */
    TEST_ASSERT_TRUE(level_at(15) > 40 && level_at(15) < 60);   /* half-way up at 15 of 30 ms */
    TEST_ASSERT_EQUAL_UINT8(100, level_at(200));
    TEST_ASSERT_TRUE(level_at(475) > 40 && level_at(475) < 60); /* half-way down */
    TEST_ASSERT_TRUE(sound_busy(&s, 549));
    TEST_ASSERT_FALSE(sound_busy(&s, 550));

    sound_style(&s, SND_CLASSIC);                            /* a preset resets it */
    TEST_ASSERT_EQUAL_UINT8(0, s.attack_ms);
    TEST_ASSERT_EQUAL_UINT16(0, s.release_ms);
}

static void test_click_length_and_short_tails(void)
{
    sound_step(&s, 3, +1);                                   /* 30 -> 40 ms */
    TEST_ASSERT_EQUAL_UINT8(40, s.click_ms);
    TEST_ASSERT_EQUAL_INT(SND_CLASSIC, s.style);             /* click length isn't the style */
    sound_play(&s, SND_CLICK, 0);
    TEST_ASSERT_EQUAL_UINT8(100, level_at(39));
    TEST_ASSERT_EQUAL_UINT8(0, level_at(40));
    sound_style(&s, SND_SOFT);
    for (int i = 0; i < 8; i++) sound_step(&s, 1, +1);       /* release 300 ms */
    sound_play(&s, SND_CLICK, 1000);
    TEST_ASSERT_FALSE(sound_busy(&s, 1000 + 80));            /* a click's tail <= its length */
}

static void test_glide_amount(void)
{
    sound_style(&s, SND_CUSTOM);
    s.glide_pct = 20;
    sound_play(&s, SND_TEST, 0);
    TEST_ASSERT_UINT16_WITHIN(15, 2700, hz_at(0));           /* 3000 - 10% */
    TEST_ASSERT_UINT16_WITHIN(15, 3300, hz_at(399));         /* 3000 + 10% */
}

/* The pitch and rhythm styles (2026-11-12): audible on a buzzer whose
 * loudness barely follows the drive. */
static void test_pattern_styles(void)
{
    sound_style(&s, SND_DROP);
    sound_play(&s, SND_TEST, 0);
    TEST_ASSERT_TRUE(hz_at(10) > 3000 && hz_at(390) < 3000);       /* falls */
    sound_style(&s, SND_DOUBLE);
    sound_play(&s, SND_DIODE, 1000);                                /* 150 ms */
    TEST_ASSERT_EQUAL_UINT8(100, level_at(1010));
    TEST_ASSERT_EQUAL_UINT8(0, level_at(1075));                     /* the gap */
    TEST_ASSERT_EQUAL_UINT8(100, level_at(1120));
    sound_style(&s, SND_TWOTONE);
    sound_play(&s, SND_TEST, 2000);
    TEST_ASSERT_EQUAL_UINT16(3780, hz_at(2050));                    /* high first */
    TEST_ASSERT_EQUAL_UINT16(3000, hz_at(2300));
    sound_style(&s, SND_WARBLE);
    sound_play(&s, SND_TEST, 3000);
    TEST_ASSERT_TRUE(hz_at(3010) != hz_at(3035));                   /* alternates */
    sound_play(&s, SND_CLICK, 4000);                                /* the click stays plain */
    TEST_ASSERT_EQUAL_UINT16(3750, hz_at(4010));
    TEST_ASSERT_EQUAL_UINT16(3750, hz_at(4028));
    sound_gate(&s, true, 5000);                                     /* continuity, DOUBLE: pulses */
    sound_style(&s, SND_DOUBLE);
    TEST_ASSERT_TRUE(level_at(5050) > 0);
    TEST_ASSERT_EQUAL_UINT8(0, level_at(5150));
}

static void test_sanitise(void)
{
    s.volume = 0; s.attack_ms = 200; s.release_ms = 5000; s.click_ms = 3; s.style = (snd_style_t)9;
    sound_sanitise(&s);
    TEST_ASSERT_EQUAL_UINT8(100, s.volume);
    TEST_ASSERT_EQUAL_UINT8(50, s.attack_ms);
    TEST_ASSERT_EQUAL_UINT16(300, s.release_ms);
    TEST_ASSERT_EQUAL_UINT8(30, s.click_ms);
    TEST_ASSERT_EQUAL_INT(SND_CLASSIC, s.style);
}

static void test_pitch_steps_and_limits(void)
{
    TEST_ASSERT_EQUAL_UINT16(3500, sound_pitch_step(3000, +1));
    TEST_ASSERT_EQUAL_UINT16(2500, sound_pitch_step(3000, -1));
    TEST_ASSERT_EQUAL_UINT16(4000, sound_pitch_step(4000, +1));
    TEST_ASSERT_EQUAL_UINT16(1000, sound_pitch_step(1000, -1));
    TEST_ASSERT_EQUAL_UINT16(3000, sound_pitch_step(2750, +1));   /* SCPI value -> next step */
    s.pitch_hz = 50;                                          /* garbage -> default */
    TEST_ASSERT_EQUAL_UINT16(SND_PITCH_DEFAULT, sound_pitch(&s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_stock_like);
    RUN_TEST(test_classic_click_is_a_square_30_ms);
    RUN_TEST(test_volume_scales_the_level);
    RUN_TEST(test_soft_ramps_up_and_down);
    RUN_TEST(test_chirp_glides_up);
    RUN_TEST(test_gate_and_click_override);
    RUN_TEST(test_tick_reports_changes_only);
    RUN_TEST(test_custom_envelope);
    RUN_TEST(test_click_length_and_short_tails);
    RUN_TEST(test_glide_amount);
    RUN_TEST(test_pattern_styles);
    RUN_TEST(test_sanitise);
    RUN_TEST(test_pitch_steps_and_limits);
    return UNITY_END();
}
