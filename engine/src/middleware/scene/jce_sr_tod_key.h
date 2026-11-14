/* Private day/night key selection. The sky retains the actual sun direction;
 * color, foliage and CSM use the same environment-owned sun/moon predicate. */
#ifndef JCE_SR_TOD_KEY_H
#define JCE_SR_TOD_KEY_H

#include <jce/middleware/world/jce_environment.h>
#include <jce/middleware/world/jce_time_of_day.h>

static inline jce_vec3 sr_tod_key_direction(const JceTimeOfDayState *tod)
{
    const jce_vec3 sun = tod->sun_direction;
    /* Only the two body directions are consumed by key_direction. Do not
     * mutate the live environment or rename the moon as the sun at night. */
    JceEnvironmentState bodies = {0};
    bodies.sun_direction_ws = sun;
    bodies.moon_direction_ws = jce_v3(-sun.x, -sun.y, -sun.z);
    return jce_environment_key_direction(&bodies);
}

#endif
