/*
 * test_jce_environment.c
 *
 * The single authoritative environment state.
 *
 * The value of this module is not the struct -- it is the INVARIANT that
 * everything downstream reads the same numbers. So these tests are about the
 * properties a second copy of the weather would break: that advancing is pure,
 * that sanitize actually closes every hole, and that the derived accessors are
 * functions of the state and nothing else.
 */

#include <jce/middleware/world/jce_environment.h>
#include <jce/middleware/world/jce_weather.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Advance a whole number of SECONDS at 60 Hz.
 *
 * Spelled this way because the obvious form -- a raw loop of `60 * 600`
 * iterations -- reads as "600 of something" and is actually 600 seconds, which
 * is how the first version of these tests came to assert drying behaviour over
 * a span ten times shorter than intended. The duration is the thing under
 * test, so the duration is what the call site should say. */
static void advance_seconds(JceEnvironmentState *s, float seconds)
{
    const int steps = (int)(seconds * 60.0f);
    for (int i = 0; i < steps; ++i)
        jce_environment_advance(s, 1.0f / 60.0f);
}

/* ── 1. The default is usable as-is ────────────────────────────────────
 *
 * Callers are expected to take the default and override one or two fields. If
 * the default itself were out of range, every such caller would inherit the
 * fault and it would be attributed to whatever they changed. */

static void test_default_is_valid(void)
{
    const JceEnvironmentState d = jce_environment_default();

    TEST_ASSERT_TRUE(d.seconds_per_day > 0.0f);
    TEST_ASSERT_TRUE(d.day_fraction >= 0.0f && d.day_fraction < 1.0f);
    TEST_ASSERT_TRUE(d.cloud_coverage >= 0.0f && d.cloud_coverage <= 1.0f);
    TEST_ASSERT_TRUE(d.humidity >= 0.0f && d.humidity <= 1.0f);
    TEST_ASSERT_TRUE(d.global_wetness >= 0.0f && d.global_wetness <= 1.0f);
    TEST_ASSERT_TRUE(d.snow_amount >= 0.0f && d.snow_amount <= 1.0f);
    TEST_ASSERT_EQUAL_UINT32(JCE_ENV_WEATHER_CLEAR, d.weather_type);

    /* The wind direction must be a unit vector even at zero speed: readers
     * normalise nothing, and a zero-length direction silently becomes a NaN
     * the moment anyone divides by its length. */
    const float len = sqrtf(d.wind_direction_ws.x * d.wind_direction_ws.x +
                            d.wind_direction_ws.y * d.wind_direction_ws.y +
                            d.wind_direction_ws.z * d.wind_direction_ws.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, len);

    /* Sanitizing the default must change nothing -- otherwise the default is
     * not actually the thing the engine runs with. */
    JceEnvironmentState c = d;
    jce_environment_sanitize(&c);
    TEST_ASSERT_EQUAL_MEMORY(&d, &c, sizeof d);
}

/* ── 2. THE POINT: the enums agree with the weather system ─────────────
 *
 * JceEnvWeatherType is converted to JceWeatherType by assignment. If the two
 * ever diverge, rain silently becomes snow -- a defect with no crash, no
 * warning, and a plausible appearance. */

static void test_weather_enum_matches_weather_system(void)
{
    TEST_ASSERT_EQUAL_INT((int)JCE_WEATHER_CLEAR, (int)JCE_ENV_WEATHER_CLEAR);
    TEST_ASSERT_EQUAL_INT((int)JCE_WEATHER_RAIN,  (int)JCE_ENV_WEATHER_RAIN);
    TEST_ASSERT_EQUAL_INT((int)JCE_WEATHER_SNOW,  (int)JCE_ENV_WEATHER_SNOW);
}

/* ── 3. sanitize closes every hole it claims to ────────────────────────  */

static void test_sanitize_repairs_garbage(void)
{
    JceEnvironmentState s;
    memset(&s, 0, sizeof s);          /* the realistic hostile input */
    jce_environment_sanitize(&s);
    TEST_ASSERT_TRUE(s.seconds_per_day > 0.0f);
    const float wl = sqrtf(s.wind_direction_ws.x * s.wind_direction_ws.x +
                           s.wind_direction_ws.y * s.wind_direction_ws.y +
                           s.wind_direction_ws.z * s.wind_direction_ws.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, wl);

    /* Out-of-range and non-finite, field by field. */
    s = jce_environment_default();
    s.cloud_coverage = 4.0f;
    s.humidity = -2.0f;
    s.global_wetness = nanf("");
    s.snow_amount = HUGE_VALF;
    s.cloud_density = -1.0f;
    s.day_fraction = 3.7f;
    s.wind_speed_mps = -5.0f;
    s.wind_gust = 9.0f;
    s.seconds_per_day = 0.0f;
    s.weather_type = 99u;
    jce_environment_sanitize(&s);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.cloud_coverage);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.humidity);
    TEST_ASSERT_TRUE(s.global_wetness >= 0.0f && s.global_wetness <= 1.0f);
    TEST_ASSERT_TRUE(s.snow_amount >= 0.0f && s.snow_amount <= 1.0f);
    TEST_ASSERT_TRUE(s.cloud_density > 0.0f);
    TEST_ASSERT_TRUE(s.day_fraction >= 0.0f && s.day_fraction < 1.0f);
    TEST_ASSERT_TRUE(s.wind_speed_mps >= 0.0f);
    TEST_ASSERT_TRUE(s.wind_gust >= 0.0f && s.wind_gust <= 1.0f);
    TEST_ASSERT_TRUE(s.seconds_per_day > 0.0f);
    TEST_ASSERT_TRUE(s.weather_type <= (uint32_t)JCE_ENV_WEATHER_SNOW);

    /* A NaN wind direction must not survive: it would reach the ocean
     * spectrum, the vegetation shader and the overlay in the same frame. */
    s = jce_environment_default();
    s.wind_direction_ws = jce_v3(nanf(""), 0.0f, 0.0f);
    jce_environment_sanitize(&s);
    TEST_ASSERT_TRUE(isfinite(s.wind_direction_ws.x));
    TEST_ASSERT_TRUE(isfinite(s.wind_direction_ws.y));
    TEST_ASSERT_TRUE(isfinite(s.wind_direction_ws.z));
}

/* ── 4. Advancing is pure and deterministic ────────────────────────────
 *
 * Replay compares frames. A clock read or an RNG draw inside advance would
 * make the environment diverge between a recording and its playback, and the
 * symptom would be a slowly drifting sky rather than an obvious failure. */

static void test_advance_is_deterministic(void)
{
    JceEnvironmentState a = jce_environment_default();
    a.weather_type = JCE_ENV_WEATHER_RAIN;
    a.precipitation_rate = 0.8f;
    JceEnvironmentState b = a;

    for (int i = 0; i < 500; ++i) {
        jce_environment_advance(&a, 1.0f / 60.0f);
        jce_environment_advance(&b, 1.0f / 60.0f);
    }
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
}

/* ── 5. The clock wraps but never rewinds ──────────────────────────────  */

static void test_clock_advances_and_wraps(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.seconds_per_day = 100.0f;
    s.day_fraction = 0.0f;
    s.world_time_seconds = 0.0;

    jce_environment_advance(&s, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.25f, s.day_fraction);
    TEST_ASSERT_TRUE(s.world_time_seconds > 24.0);

    /* Past a full day it wraps into range rather than growing without bound;
     * world_time_seconds keeps counting so elapsed time is still knowable. */
    jce_environment_advance(&s, 260.0f);
    TEST_ASSERT_TRUE(s.day_fraction >= 0.0f && s.day_fraction < 1.0f);
    TEST_ASSERT_TRUE(s.world_time_seconds > 284.0);

    /* Degenerate dt is a no-op, NOT a rewind: a paused frame, a debugger
     * break, or a NaN from a broken timer must not move the world backwards. */
    const JceEnvironmentState before = s;
    jce_environment_advance(&s, 0.0f);
    jce_environment_advance(&s, -5.0f);
    jce_environment_advance(&s, nanf(""));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof s);

    jce_environment_advance(NULL, 1.0f);   /* must not crash */
}

/* ── 6. Wetness lags the rain, in both directions ──────────────────────
 *
 * If wetness simply tracked precipitation_rate, the ground would go dry the
 * instant the rain stopped -- which is the single most obvious tell that a
 * weather system is a lookup rather than a simulation. */

static void test_wetness_lags_precipitation(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.weather_type = JCE_ENV_WEATHER_RAIN;
    s.precipitation_rate = 1.0f;
    s.global_wetness = 0.0f;

    /* One frame of rain must not soak the world. */
    jce_environment_advance(&s, 1.0f / 60.0f);
    TEST_ASSERT_TRUE_MESSAGE(s.global_wetness < 0.2f,
        "wetness snapped to full in one frame - it is tracking, not integrating");
    TEST_ASSERT_TRUE(s.global_wetness > 0.0f);

    /* Sustained rain approaches saturation. */
    advance_seconds(&s, 300.0f);            /* five minutes of steady rain */
    TEST_ASSERT_TRUE(s.global_wetness > 0.85f);
    TEST_ASSERT_TRUE(s.global_wetness <= 1.0f);

    /* And it dries SLOWLY once the rain stops. */
    s.precipitation_rate = 0.0f;
    s.weather_type = JCE_ENV_WEATHER_CLEAR;
    const float wet_at_stop = s.global_wetness;
    jce_environment_advance(&s, 1.0f / 60.0f);
    TEST_ASSERT_TRUE_MESSAGE(s.global_wetness < wet_at_stop,
        "wetness does not decay");
    TEST_ASSERT_TRUE_MESSAGE(s.global_wetness > wet_at_stop - 0.05f,
        "wetness fell off a cliff the moment the rain stopped");

    advance_seconds(&s, 1800.0f);           /* half an hour to dry out */
    TEST_ASSERT_TRUE(s.global_wetness < 0.05f);
}

/* ── 7. Snow accumulates only when it is cold ──────────────────────────
 *
 * Rain at -5 C and snow at +20 C are both authorable, and a system that
 * accumulated snow purely from precipitation_rate would pile it up in summer. */

static void test_snow_needs_cold(void)
{
    JceEnvironmentState warm = jce_environment_default();
    warm.weather_type = JCE_ENV_WEATHER_SNOW;
    warm.precipitation_rate = 1.0f;
    warm.temperature_c = 20.0f;
    warm.snow_amount = 0.0f;
    advance_seconds(&warm, 600.0f);         /* ten minutes of snowfall */
    TEST_ASSERT_TRUE_MESSAGE(warm.snow_amount < 0.05f,
        "snow accumulated at +20 C");

    JceEnvironmentState cold = jce_environment_default();
    cold.weather_type = JCE_ENV_WEATHER_SNOW;
    cold.precipitation_rate = 1.0f;
    cold.temperature_c = -8.0f;
    cold.snow_amount = 0.0f;
    advance_seconds(&cold, 600.0f);         /* ten minutes of snowfall */
    TEST_ASSERT_TRUE_MESSAGE(cold.snow_amount > 0.3f, "snow never accumulated at -8 C");

    /* Lying snow melts when it warms up, even with no precipitation. */
    cold.precipitation_rate = 0.0f;
    cold.weather_type = JCE_ENV_WEATHER_CLEAR;
    cold.temperature_c = 15.0f;
    const float lying = cold.snow_amount;
    advance_seconds(&cold, 1800.0f);        /* half an hour above freezing */
    TEST_ASSERT_TRUE_MESSAGE(cold.snow_amount < lying * 0.5f, "snow did not melt");
}

/* ── 8. Derived accessors are functions of the state alone ─────────────
 *
 * This is the property that makes one state worth having: two subsystems that
 * read the same state must get the same answer, with no hidden input. */

static void test_derived_are_pure_functions(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.wind_speed_mps = 10.0f;
    s.wind_gust = 0.5f;

    const float w1 = jce_environment_wind_speed_now(&s);
    const float w2 = jce_environment_wind_speed_now(&s);
    TEST_ASSERT_EQUAL_FLOAT(w1, w2);
    TEST_ASSERT_TRUE_MESSAGE(w1 >= 10.0f, "gust reduced the wind speed");
    TEST_ASSERT_TRUE(w1 <= 20.0f);

    /* Zero gust is exactly the base speed -- not approximately, or the ocean
     * spectrum and the grass disagree in the last bits and the water
     * shimmers against still grass. */
    s.wind_gust = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(10.0f, jce_environment_wind_speed_now(&s));

    /* Key light follows the sun above the horizon and the moon below it. */
    s.sun_direction_ws  = jce_v3(0.0f,  0.8f, 0.6f);
    s.moon_direction_ws = jce_v3(0.0f, -0.8f, 0.6f);
    s.sun_illuminance_lux  = 100000.0f;
    s.moon_illuminance_lux = 0.25f;
    TEST_ASSERT_TRUE(jce_environment_is_daytime(&s));
    TEST_ASSERT_EQUAL_FLOAT(100000.0f, jce_environment_key_illuminance(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_environment_key_direction(&s).y);

    s.sun_direction_ws  = jce_v3(0.0f, -0.5f, 0.86f);
    s.moon_direction_ws = jce_v3(0.0f,  0.5f, 0.86f);
    TEST_ASSERT_FALSE(jce_environment_is_daytime(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, jce_environment_key_illuminance(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_environment_key_direction(&s).y);

    /* NULL is answerable, not fatal: a caller polling the environment before
     * the world is created must get a defensible number, not a crash. */
    TEST_ASSERT_TRUE(isfinite(jce_environment_wind_speed_now(NULL)));
    TEST_ASSERT_TRUE(isfinite(jce_environment_key_illuminance(NULL)));
    TEST_ASSERT_TRUE(isfinite(jce_environment_key_direction(NULL).y));
    TEST_ASSERT_FALSE(jce_environment_is_daytime(NULL));
}

/* ── 9. REGRESSION GUARD: rain has wind ────────────────────────────────
 *
 * This test exists because the bug happened. Moving the weather overlay onto
 * the environment replaced a working `wind_strength = 3.0f * intensity` with a
 * read of a field nothing wrote, so it stayed at its default of zero and rain
 * began falling perfectly vertically -- no crash, no warning, no test failure,
 * and a picture that still looked like rain.
 *
 * A carrier landed before its writer. Asserting the wind is NON-ZERO while it
 * is raining is the cheapest statement of what was actually broken. */

static void test_precipitation_implies_wind(void)
{
    JceEnvironmentState s = jce_environment_default();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_environment_wind_speed_now(&s));

    s.weather_type = JCE_ENV_WEATHER_RAIN;
    s.precipitation_rate = 1.0f;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_TRUE_MESSAGE(jce_environment_wind_speed_now(&s) > 0.5f,
        "rain with no wind - precipitation will fall vertically");
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.wind_speed_mps);

    /* Snow drifts less than rain, which is the whole reason the two differ. */
    s.weather_type = JCE_ENV_WEATHER_SNOW;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, s.wind_speed_mps);
    TEST_ASSERT_TRUE(s.wind_speed_mps > 0.0f);

    /* Scales with intensity. */
    s.weather_type = JCE_ENV_WEATHER_RAIN;
    s.precipitation_rate = 0.5f;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, s.wind_speed_mps);

    /* Clear weather is a BREEZE, not a vacuum.
     *
     * This asserted 0.0 and stayed asserting it after U4 (21e38944) put a
     * JCE_ENV_WIND_CALM_MPS floor under the calm case, so the suite has been
     * red ever since -- a contract changed and its test did not, which is the
     * one thing a test exists to prevent.
     *
     * The floor is the point, not an artefact: with U4 the per-field wind a
     * scene already authored becomes a MULTIPLIER of this value, so a base of
     * zero would stop every blade of grass in every scene the day U4 landed. */
    s.weather_type = JCE_ENV_WEATHER_CLEAR;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_EQUAL_FLOAT(JCE_ENV_WIND_CALM_MPS, s.wind_speed_mps);
    TEST_ASSERT_TRUE(s.wind_speed_mps > 0.0f);

    /* The DIRECTION must survive: it is the field a scene or preset is most
     * likely to author, and clobbering it would make authored wind
     * un-authorable the moment it started raining. */
    s = jce_environment_default();
    s.wind_direction_ws = jce_v3(0.0f, 0.0f, 1.0f);
    s.weather_type = JCE_ENV_WEATHER_RAIN;
    s.precipitation_rate = 1.0f;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.wind_direction_ws.x);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.wind_direction_ws.z);

    jce_environment_apply_weather_wind(NULL);   /* must not crash */
}

/* ── Fog extinction: the numbers are visibilities, so test them as such ──
 *
 * The value under test is an extinction coefficient, and Koschmieder's law
 * turns one into a distance a person can judge: V = 3.912 / s.  Asserting on
 * the visibility rather than on the coefficient is what makes these tests
 * readable and, more importantly, arguable -- "clear air must see past 20 km"
 * is a claim someone can dispute, "must be under 2e-4" is not. */

static float visibility_m(float extinction)
{
    return (extinction > 0.0f) ? (3.912f / extinction) : 1e9f;
}

static void test_fog_clear_air_sees_far(void)
{
    JceEnvironmentState s = jce_environment_default();   /* humidity 0.4 */
    const float v = visibility_m(jce_environment_fog_extinction(&s));
    /* The default state is a clear day. If this ever drops into the low
     * kilometres, every scene that authors no weather has quietly acquired
     * haze -- which is exactly how a derived value gets "fixed" by someone
     * switching fog off altogether. */
    TEST_ASSERT_TRUE(v > 20000.0f);
}

static void test_fog_below_onset_contributes_nothing(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.humidity = 0.0f;
    const float dry = jce_environment_fog_extinction(&s);
    s.humidity = 0.59f;                       /* just under the onset */
    const float damp = jce_environment_fog_extinction(&s);
    TEST_ASSERT_EQUAL_FLOAT(dry, damp);
}

static void test_fog_saturated_air_is_half_a_kilometre(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.humidity = 1.0f;
    const float v = visibility_m(jce_environment_fog_extinction(&s));
    TEST_ASSERT_FLOAT_WITHIN(60.0f, 500.0f, v);
}

static void test_fog_precipitation_adds_on_top(void)
{
    JceEnvironmentState dry = jce_environment_default();
    JceEnvironmentState wet = dry;
    wet.precipitation_rate = 1.0f;
    TEST_ASSERT_TRUE(jce_environment_fog_extinction(&wet) >
                     jce_environment_fog_extinction(&dry));

    /* And it adds rather than replaces: rain in saturated air must be foggier
     * than rain in dry air, or the two inputs are not independently
     * authorable. */
    JceEnvironmentState humid_rain = dry;
    humid_rain.humidity = 1.0f;
    humid_rain.precipitation_rate = 1.0f;
    TEST_ASSERT_TRUE(jce_environment_fog_extinction(&humid_rain) >
                     jce_environment_fog_extinction(&wet));
}

static void test_fog_is_monotonic_in_both_inputs(void)
{
    JceEnvironmentState s = jce_environment_default();
    float prev = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        s.humidity = (float)i / 20.0f;
        const float e = jce_environment_fog_extinction(&s);
        TEST_ASSERT_TRUE(e >= prev);
        prev = e;
    }
    s.humidity = 0.5f;
    prev = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        s.precipitation_rate = (float)i / 20.0f;
        const float e = jce_environment_fog_extinction(&s);
        TEST_ASSERT_TRUE(e >= prev);
        prev = e;
    }
}

static void test_fog_rejects_garbage_and_null(void)
{
    /* NULL is ABSENT, not clear: a caller that forgot the state must get no
     * fog rather than a believable amount of it. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_environment_fog_extinction(NULL));

    JceEnvironmentState s = jce_environment_default();
    s.humidity = nanf("");
    s.precipitation_rate = HUGE_VALF;
    const float e = jce_environment_fog_extinction(&s);
    TEST_ASSERT_TRUE(isfinite(e));
    TEST_ASSERT_TRUE(e >= 0.0f);
    /* Even fully saturated and raining, this is fog -- not an opaque wall.
     * 40 m of visibility is already the densest fog met offices record. */
    TEST_ASSERT_TRUE(visibility_m(e) > 40.0f);
}

/* -- 10. REGRESSION GUARD: the gust term had no writer ------------------
 *
 * wind_gust sat at jce_environment_default()'s 0 in every shipping frame,
 * because nothing outside the sanitiser and this file ever assigned it. So
 * wind_speed_now() computed base * (1 + 0) -- a guaranteed multiply by one --
 * and the grass, the cloud drift and the rain overlay all moved at a speed
 * that never varied.
 *
 * Same shape as the wind-direction bug two tests up, and the same cheapest
 * statement of it: assert the field is NON-ZERO after its writer runs. */

static void test_gust_has_a_writer(void)
{
    JceEnvironmentState s = jce_environment_default();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.wind_gust);

    /* Clear air still gusts -- G 1.20 over open ground. */
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_TRUE_MESSAGE(s.wind_gust > 0.0f,
        "no writer for wind_gust - the (1 + gust) term is a multiply by one");
    TEST_ASSERT_EQUAL_FLOAT(0.20f, s.wind_gust);

    /* Showery air is gustier than frontal air at the same intensity. That
     * ordering is the claim worth pinning; the exact numbers are the
     * implementation's, and the test must not become a second definition. */
    s.precipitation_rate = 1.0f;
    s.weather_type = JCE_ENV_WEATHER_SNOW;
    jce_environment_apply_weather_wind(&s);
    const float snow_gust = s.wind_gust;

    s.weather_type = JCE_ENV_WEATHER_RAIN;
    jce_environment_apply_weather_wind(&s);
    TEST_ASSERT_TRUE_MESSAGE(s.wind_gust > snow_gust,
        "rain is convective and should gust harder than snow");

    /* Every weather leaves the state sanitize-stable. */
    const float before = s.wind_gust;
    jce_environment_sanitize(&s);
    TEST_ASSERT_EQUAL_FLOAT(before, s.wind_gust);
}

/* The gust must actually VARY with time, and stay inside its own envelope.
 *
 * A constant multiplier dressed as a gust would pass "non-zero" above while
 * changing nothing anyone can see -- that is exactly how the field looked
 * before it had a writer, one level up. */

static void test_gust_varies_within_its_envelope(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.wind_speed_mps = 10.0f;
    s.wind_gust      = 0.5f;      /* peak 15 m/s */

    float lo = 1.0e9f, hi = -1.0e9f;
    for (int i = 0; i < 600; ++i) {          /* 120 s at 5 Hz */
        s.world_time_seconds = (double)i * 0.2;
        const float w = jce_environment_wind_speed_now(&s);
        TEST_ASSERT_TRUE_MESSAGE(w >= 10.0f, "gust reduced the wind speed");
        TEST_ASSERT_TRUE_MESSAGE(w <= 15.0f, "gust exceeded its amplitude");
        if (w < lo) lo = w;
        if (w > hi) hi = w;
    }
    /* Over two minutes the envelope must sweep a real part of its range. A
     * third is a deliberately loose floor: the assertion is "this moves",
     * not "this moves by exactly the amount today's periods produce". */
    TEST_ASSERT_TRUE_MESSAGE(hi - lo > (15.0f - 10.0f) / 3.0f,
        "wind_speed_now does not vary with time - the gust is a constant");

    /* Still a pure function: the same state twice gives the same answer, or
     * two subsystems reading it in one frame disagree. */
    s.world_time_seconds = 33.0;
    TEST_ASSERT_EQUAL_FLOAT(jce_environment_wind_speed_now(&s),
                            jce_environment_wind_speed_now(&s));

    /* Zero amplitude removes the term entirely -- exactly, not nearly. */
    s.wind_gust = 0.0f;
    for (int i = 0; i < 16; ++i) {
        s.world_time_seconds = (double)i * 3.7;
        TEST_ASSERT_EQUAL_FLOAT(10.0f, jce_environment_wind_speed_now(&s));
    }
}

/* The sustained accessor exists so the ocean spectrum stops rebuilding.
 *
 * Two claims: it excludes the gust, and it does NOT move as the clock does.
 * The second is the one that matters -- spectrum_differs() in
 * jce_water_field.c keys the FFT spectrum cache on this number, so a value
 * that drifted would re-solve the whole spectrum every frame. */

static void test_sustained_excludes_the_gust(void)
{
    JceEnvironmentState s = jce_environment_default();
    s.wind_speed_mps = 7.5f;
    s.wind_gust      = 0.7f;

    for (int i = 0; i < 64; ++i) {
        s.world_time_seconds = (double)i * 1.9;
        TEST_ASSERT_EQUAL_FLOAT(7.5f, jce_environment_wind_speed_sustained(&s));
    }
    TEST_ASSERT_TRUE(isfinite(jce_environment_wind_speed_sustained(NULL)));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_environment_wind_speed_sustained(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fog_clear_air_sees_far);
    RUN_TEST(test_fog_below_onset_contributes_nothing);
    RUN_TEST(test_fog_saturated_air_is_half_a_kilometre);
    RUN_TEST(test_fog_precipitation_adds_on_top);
    RUN_TEST(test_fog_is_monotonic_in_both_inputs);
    RUN_TEST(test_fog_rejects_garbage_and_null);
    RUN_TEST(test_default_is_valid);
    RUN_TEST(test_weather_enum_matches_weather_system);
    RUN_TEST(test_sanitize_repairs_garbage);
    RUN_TEST(test_advance_is_deterministic);
    RUN_TEST(test_clock_advances_and_wraps);
    RUN_TEST(test_wetness_lags_precipitation);
    RUN_TEST(test_snow_needs_cold);
    RUN_TEST(test_derived_are_pure_functions);
    RUN_TEST(test_precipitation_implies_wind);
    RUN_TEST(test_gust_has_a_writer);
    RUN_TEST(test_gust_varies_within_its_envelope);
    RUN_TEST(test_sustained_excludes_the_gust);
    return UNITY_END();
}
