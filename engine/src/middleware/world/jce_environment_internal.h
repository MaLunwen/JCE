#ifndef JCE_ENVIRONMENT_INTERNAL_H
#define JCE_ENVIRONMENT_INTERNAL_H

#include <jce/middleware/world/jce_environment.h>

/* Authored positive time constants override the default precipitation response. */
void jce_environment_advance_with_surface_times(JceEnvironmentState *state, float dt,
                                                float wetting_seconds, float snowfall_seconds);

#endif
