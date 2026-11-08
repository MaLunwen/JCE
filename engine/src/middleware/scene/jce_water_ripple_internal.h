#ifndef JCE_WATER_RIPPLE_INTERNAL_H
#define JCE_WATER_RIPPLE_INTERNAL_H

#include <jce/middleware/scene/jce_water_ripple.h>

/* Continuous body/water velocity coupling. target_velocity_y is upward m/s;
 * dt is the executed physics step in seconds. Radius is the authored collider
 * footprint in metres. Wet cells relax toward the body velocity on their local
 * wave-crossing time max(radius, cell spacing) / sqrt(g * depth), without
 * overshooting or accumulating a complete impulse every tick. */
void jce_water_ripple_drive_velocity(JceWaterRipple *r, float x, float z,
    float radius_m, float target_velocity_y, float dt);

#endif
