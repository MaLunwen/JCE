/* Actual day/night key helper and existing environment predicate.
 * CPU direction/identity checks only; native scene pixels remain separate. */
#include "middleware/scene/jce_sr_tod_key.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned checks = 0, failures = 0;
    for (int step = 0; step < 1440; ++step) {
        const float angle = (float)step * 6.28318530718f / 1440.0f;
        JceTimeOfDayState tod = {0};
        tod.sun_direction = jce_v3(cosf(angle), sinf(angle), 0.0f);
        const jce_vec3 original = tod.sun_direction;
        JceEnvironmentState env = {0};
        env.sun_direction_ws = original;
        env.moon_direction_ws = jce_v3(-original.x, -original.y, -original.z);
        const jce_vec3 key = sr_tod_key_direction(&tod);
        const jce_vec3 expected = jce_environment_key_direction(&env);
        ++checks;
        if (memcmp(&key, &expected, sizeof(key))) ++failures;
        ++checks;
        if (key.y < 0.0f) ++failures;
        ++checks;
        if (memcmp(&original, &tod.sun_direction, sizeof(original))) ++failures;
        ++checks;
        if (fabsf(jce_v3_dot(key, key) - 1.0f) > 1e-5f) ++failures;
    }
    printf("day/night key: %u checks, %u failures (CPU only)\n", checks, failures);
    return failures ? 1 : 0;
}
