/* Executes the actual private light-response owner without a GPU or Scene.
 * This proves bounded energy/color behavior, not native foliage pixels. */
#include "middleware/scene/jce_sr_foliage_lighting.h"
#include <float.h>
#include <stdio.h>

static unsigned checks, failures;
static void near_value(float got, float expected, const char *name)
{
    ++checks;
    const float delta = got - expected;
    if (!isfinite(got) || delta < -0.000001f || delta > 0.000001f) {
        ++failures;
        fprintf(stderr, "%s: got %.9g expected %.9g\n", name,
            (double)got, (double)expected);
    }
}
static void check_rgb(const float got[3], const float expected[3], const char *name)
{
    for (unsigned i = 0; i < 3u; ++i) near_value(got[i], expected[i], name);
}

int main(void)
{
    const float authored[3] = {0.5f, 0.75f, 1.0f};
    const float zero[3] = {0, 0, 0}, white[3] = {1, 1, 1};
    const float night_sun[3] = {0.01f, 0.02f, 0.04f};
    const float night_ambient[3] = {0.1f, 0.2f, 0.3f};
    float result[3];

    sr_foliage_lighting_multiplier(authored, white, 1, zero, 0, result);
    check_rgb(result, authored, "unit daylight preserves authored palette");
    sr_foliage_lighting_multiplier(authored, white, 4, white, 2, result);
    check_rgb(result, authored, "bright light cannot wash out the ramp");
    sr_foliage_lighting_multiplier(authored, white, 0, white, 0, result);
    check_rgb(result, zero, "zero intensities remove all surface emission");
    sr_foliage_lighting_multiplier(authored, zero, 1, zero, 1, result);
    check_rgb(result, zero, "zero RGB removes ToD energy even at scalar one");

    const float night_expected[3] = {0.015f, 0.045f, 0.1f};
    sr_foliage_lighting_multiplier(authored, night_sun, 1,
        night_ambient, 0.2f, result);
    check_rgb(result, night_expected, "night keeps actual RGB times intensity");
    const float ambient_expected[3] = {0.01f, 0.03f, 0.06f};
    sr_foliage_lighting_multiplier(authored, zero, 0,
        night_ambient, 0.2f, result);
    check_rgb(result, ambient_expected, "ambient-only scene retains its hue");

    near_value(sr_foliage_incident_channel(-1, 1), 0, "negative color");
    near_value(sr_foliage_incident_channel(1, -1), 0, "negative intensity");
    near_value(sr_foliage_incident_channel(NAN, 1), 0, "NaN color");
    near_value(sr_foliage_incident_channel(1, NAN), 0, "NaN intensity");
    near_value(sr_foliage_incident_channel(INFINITY, 1), 0, "infinite color");
    near_value(sr_foliage_incident_channel(1, INFINITY), 0, "infinite intensity");
    near_value(sr_foliage_incident_channel(FLT_MAX, FLT_MAX), 1,
        "finite overflow is bounded without NaN");

    /* A gradual dusk cannot brighten any component as energy decreases. */
    float previous[3] = {1, 1, 1};
    for (unsigned step = 0; step <= 100u; ++step) {
        const float phase = (100.0f - (float)step) * 0.01f;
        sr_foliage_lighting_multiplier(authored, white, phase,
            night_ambient, 0.2f * phase, result);
        for (unsigned i = 0; i < 3u; ++i) {
            ++checks;
            if (!isfinite(result[i]) || result[i] < 0 || result[i] > previous[i]) {
                ++failures;
                fprintf(stderr, "dusk component %u step %u is not bounded/monotone\n", i, step);
            }
            previous[i] = result[i];
        }
    }
    check_rgb(result, zero, "dusk ends at zero energy");
    printf("foliage lighting: %u checks, %u failures (CPU only)\n", checks, failures);
    return failures ? 1 : 0;
}
